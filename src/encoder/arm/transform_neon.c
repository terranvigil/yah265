/*
 * transform_neon.c - NEON forward/inverse transform kernels (class
 * N265_ASM_DCT). One file per kernel family; see src/encoder/arm/neon.h for
 * the dispatch convention every kernel here follows.
 *
 * THE SHAPE OF THE VECTORISATION. A 2-D separable transform is 2N
 * independent 1-D transforms, and the cheapest way to put NEON on one is
 * to keep the 1-D butterfly EXACTLY as the C writes it and run four of
 * them at once, one per lane. Lanes therefore index the independent
 * transform (a column in stage 1, a row of the intermediate in stage 2)
 * and never the length dimension, so no horizontal add ever appears and
 * every coefficient multiply is a multiply-by-scalar into four lanes.
 *
 * That is also what makes bit-exactness a property rather than an
 * argument: per lane, the sequence of int32 adds, subs, multiply-adds and
 * rounding shifts is the same sequence, in the same order, that
 * src/encoder/transform.c's fwd_dct_1d / inv_dct_1d perform on one
 * scalar. The only reordering is the four-way accumulator split in the
 * forward odd dot product, and two's-complement addition is associative,
 * so the regrouped sum is the same integer (modulo 2^32, and the range
 * notes on each kernel say we are nowhere near that).
 *
 * Between the two stages the data has to change which dimension is
 * contiguous, which is one 4x4 int32 transpose per 4x4 tile. That is the
 * whole cost of the layout, and it is why the intermediate buffer is
 * written transposed rather than the stage-2 loads being strided.
 *
 * Copyright (c) 2026, the yah265 authors. GPL-2.0-or-later; see LICENSE.
 */

#if N265_NEON_TRANSFORM

#include <arm_neon.h>
#include <stddef.h>
#include <string.h>

#include "encoder/arm/neon.h"
#include "encoder/tables.h"

/* BIT_DEPTH, PIXEL_MAX and QP_BD_OFFSET come from common/pixel.h through
 * neon.h: this file is compiled at every depth whose lanes it handles,
 * and the constants below are the same expressions transform.c's C
 * reference reads. Phase 8 item 3 - what the depth actually moves here is
 * three shifts (the forward shift1, the inverse's second stage, the
 * dequant's bdShift), the quantiser's Qp' index, and the width of a
 * sample at the two ends where pred comes in and dst goes out. The
 * butterflies themselves are int32 lanes over int16 coefficients and do
 * not know the depth at all, which is why this family was the first one
 * ported and why it keeps its whole 8-bit multiple at Main10. */

/* ---- helpers -------------------------------------------------------- */

/* 4x4 int32 transpose: out[i] lane l = in[l] lane i. */
static inline void tr4_s32(int32x4_t v[4])
{
    int32x4_t a0 = vtrn1q_s32(v[0], v[1]);
    int32x4_t a1 = vtrn2q_s32(v[0], v[1]);
    int32x4_t a2 = vtrn1q_s32(v[2], v[3]);
    int32x4_t a3 = vtrn2q_s32(v[2], v[3]);
    int64x2_t b0 = vreinterpretq_s64_s32(a0), b1 = vreinterpretq_s64_s32(a1);
    int64x2_t b2 = vreinterpretq_s64_s32(a2), b3 = vreinterpretq_s64_s32(a3);

    v[0] = vreinterpretq_s32_s64(vtrn1q_s64(b0, b2));
    v[1] = vreinterpretq_s32_s64(vtrn1q_s64(b1, b3));
    v[2] = vreinterpretq_s32_s64(vtrn2q_s64(b0, b2));
    v[3] = vreinterpretq_s32_s64(vtrn2q_s64(b1, b3));
}

/* four unaligned samples as four int32 lanes */
static inline int32x4_t load4_pix(const pixel *p)
{
#if BIT_DEPTH == 8
    uint32_t w;

    memcpy(&w, p, 4);                       /* no alignment guarantee */
    return vreinterpretq_s32_u32(
        vmovl_u16(vget_low_u16(vmovl_u8(vreinterpret_u8_u32(vdup_n_u32(w))))));
#else
    return vreinterpretq_s32_u32(vmovl_u16(vld1_u16(p)));
#endif
}

/* four int32 lanes as four samples, clipped.
 *
 * At 8 bits vqmovun_s32 saturates below at 0 and vqmovn_u16 above at 255,
 * which is clip3(0, 255, v) exactly: both clips are the instruction's own
 * and not a separate min/max. At Main10 the upper bound is not a narrow's
 * own, so it is one vmin: vqmovun_s32 gives max(0, min(65535, v)) and the
 * vmin brings that to PIXEL_MAX, which is clip3(0, PIXEL_MAX, v) for
 * every int32 v - the 65535 clamp can only fire above PIXEL_MAX and the
 * vmin swallows it. */
static inline void store4_pix(pixel *p, int32x4_t v)
{
#if BIT_DEPTH == 8
    uint16x4_t h = vqmovun_s32(v);
    uint8x8_t b = vqmovn_u16(vcombine_u16(h, h));
    uint32_t w = vget_lane_u32(vreinterpret_u32_u8(b), 0);

    memcpy(p, &w, 4);
#else
    vst1_u16(p, vmin_u16(vqmovun_s32(v), vdup_n_u16(PIXEL_MAX)));
#endif
}

/* The C's `(s + (1 << (shift - 1))) >> shift` with a run-time shift.
 * SRSHL by a negative amount is defined as exactly that: add the half-ulp,
 * then shift right arithmetically. `sh` carries -shift in every lane, so
 * this is the C expression verbatim and not a rounding of its own. */
#define RSH(v, sh) vrshlq_s32((v), (sh))

/* Matrix coefficients as vector LANES, not as duplicated scalars.
 *
 * This is the whole difference between a kernel that beats the C and one
 * that ties it. n265_tmatrix is int8 in memory, and the obvious
 * `vmlaq_n_s32(acc, o[k], row[k])` compiles to ldrsb + dup + mla - three
 * instructions for four multiply-accumulates, and the dup sits on the
 * critical path. Widening a whole row into int32 lanes once and then
 * addressing it with the by-element form of MLA costs one load and a
 * handful of widens per row, after which every multiply-accumulate is one
 * instruction.
 *
 * Both forms were built side by side and benched alternately (loaded box,
 * so read the pairs and not the absolutes): idct_add 32x32 3334 ns
 * by-scalar against 1752 by-lane, 16x16 683 against 398. The forward
 * gains far less - 1501 against 1446 at 32x32 - because its odd dot
 * product already ran four independent accumulators, which hid the dup
 * latency that the inverse's single-accumulator-per-output shape could
 * not. Same bits out of both.
 *
 * A tmatrix row is 32 bytes, so reading 8 or 16 of them is always inside
 * the table whatever the transform length is. */
static inline void row4_s32(const int8_t *row, int32x4_t c[1])
{
    c[0] = vmovl_s16(vget_low_s16(vmovl_s8(vld1_s8(row))));
}

static inline void row8_s32(const int8_t *row, int32x4_t c[2])
{
    int16x8_t w = vmovl_s8(vld1_s8(row));

    c[0] = vmovl_s16(vget_low_s16(w));
    c[1] = vmovl_high_s16(w);
}

static inline void row16_s32(const int8_t *row, int32x4_t c[4])
{
    int8x16_t r = vld1q_s8(row);
    int16x8_t lo = vmovl_s8(vget_low_s8(r)), hi = vmovl_high_s8(r);

    c[0] = vmovl_s16(vget_low_s16(lo));
    c[1] = vmovl_high_s16(lo);
    c[2] = vmovl_s16(vget_low_s16(hi));
    c[3] = vmovl_high_s16(hi);
}

/* acc = o[b] * c[0] + o[b+1] * c[1] + o[b+2] * c[2] + o[b+3] * c[3] */
#define DOT4(o, b, cv) \
    vmlaq_laneq_s32( \
        vmlaq_laneq_s32( \
            vmlaq_laneq_s32(vmulq_laneq_s32((o)[(b) + 0], (cv), 0), \
                            (o)[(b) + 1], (cv), 1), \
            (o)[(b) + 2], (cv), 2), \
        (o)[(b) + 3], (cv), 3)

/* o[b..b+3] += c * cv[0..3] */
#define ACC4(o, b, c, cv) do { \
    (o)[(b) + 0] = vmlaq_laneq_s32((o)[(b) + 0], (c), (cv), 0); \
    (o)[(b) + 1] = vmlaq_laneq_s32((o)[(b) + 1], (c), (cv), 1); \
    (o)[(b) + 2] = vmlaq_laneq_s32((o)[(b) + 2], (c), (cv), 2); \
    (o)[(b) + 3] = vmlaq_laneq_s32((o)[(b) + 3], (c), (cv), 3); \
} while (0)

/* sum_k row[k] * o[k], k < h. The four partial sums are summed pairwise
 * rather than in a single chain: two's-complement addition is
 * associative, so the regrouped total is the same integer, and the range
 * notes below say no partial sum can overflow on the way there either. */
static inline int32x4_t fwd_odd_dot(const int32x4_t *o, const int8_t *row,
                                    int h)
{
    int32x4_t cv[4], s0, s1;

    switch (h) {
    case 1:
        return vmulq_n_s32(o[0], row[0]);
    case 2:
        return vmlaq_n_s32(vmulq_n_s32(o[0], row[0]), o[1], row[1]);
    case 4:
        row4_s32(row, cv);
        return DOT4(o, 0, cv[0]);
    case 8:
        row8_s32(row, cv);
        return vaddq_s32(DOT4(o, 0, cv[0]), DOT4(o, 4, cv[1]));
    default:
        row16_s32(row, cv);
        s0 = vaddq_s32(DOT4(o, 0, cv[0]), DOT4(o, 4, cv[1]));
        s1 = vaddq_s32(DOT4(o, 8, cv[2]), DOT4(o, 12, cv[3]));
        return vaddq_s32(s0, s1);
    }
}

/* o[j] += row[j] * c, j < len: the inverse's odd contribution, same
 * accumulation and same order as the C's inner loop. */
static inline void inv_odd_acc(int32x4_t *o, int32x4_t c, const int8_t *row,
                               int len)
{
    int32x4_t cv[4];

    switch (len) {
    case 1:
        o[0] = vmlaq_n_s32(o[0], c, row[0]);
        return;
    case 2:
        o[0] = vmlaq_n_s32(o[0], c, row[0]);
        o[1] = vmlaq_n_s32(o[1], c, row[1]);
        return;
    case 4:
        row4_s32(row, cv);
        ACC4(o, 0, c, cv[0]);
        return;
    case 8:
        row8_s32(row, cv);
        ACC4(o, 0, c, cv[0]);
        ACC4(o, 4, c, cv[1]);
        return;
    default:
        row16_s32(row, cv);
        ACC4(o, 0, c, cv[0]);
        ACC4(o, 4, c, cv[1]);
        ACC4(o, 8, c, cv[2]);
        ACC4(o, 12, c, cv[3]);
        return;
    }
}

/* ---- forward ------------------------------------------------------- */

/*
 * One 1-D forward DCT of length n on four columns at once. `e` holds the
 * n inputs (one per lane) and is clobbered; `out` takes the n outputs.
 * This is fwd_dct_1d() from transform.c with int32x4_t in place of
 * int32_t and nothing else changed.
 *
 * RANGE. The domain is a residual, |r| <= PIXEL_MAX, because that is all
 * src - pred can be; it is also the bound the C's own int32 accumulators
 * need, so this kernel is exact wherever the C fast path is. Each level
 * halves the length and doubles the magnitude, so entering the level of
 * length `len` the sums obey |e| <= 255 * n / len and |o| <= 510 * n /
 * len at 8 bits. An odd dot product there is at most (len / 2) * 90 *
 * 510 * n / len = 22950 * n, i.e. 734K at n = 32 - independent of the
 * level, which is the useful part. After `shift1 = log2n - 1` that
 * leaves stage-1 outputs under 45900, and the same argument on stage 2
 * gives 90 * 45900 * n = 132M, with 64 * e[0] at 94M.
 *
 * At Main10 the residual is four times as large and shift1 is two
 * larger (`log2n + BitDepth - 9`), so the pre-shift dot product goes to
 * 2.94M and EVERYTHING AFTER IT IS UNMOVED: stage-1 outputs under 45900,
 * stage 2 under 132M. The two moves cancel by construction - that is the
 * property the whole depth rests on (docs/phase8-plan.md, item 2) - so
 * one range argument covers both builds. Every one of those numbers is
 * inside int32 with four bits to spare, so the lane arithmetic never
 * wraps and is exact rather than merely modular-exact.
 *
 * The four accumulators exist to break the multiply-accumulate dependency
 * chain, which is the whole latency of the inner loop. Reassociating an
 * int32 sum is exact in two's complement, and the bound above says no
 * partial sum overflows on the way either.
 *
 * THE STAGING ARRAY IS GONE, AND IT WAS NOT FREE (phase 13 A2). The C
 * and round 1's kernel both wrote the level's even half to a `t[]` and
 * copied it back into `e[]` after the odd dot products. `h` is a
 * run-time value there, so clang lowers that copy to a CALL TO
 * `_platform_memmove` - which the A2 census caught as the largest single
 * memmove site in the encoder, 22-26% of all memmove samples on the
 * three profile clips. The copy is unnecessary at any `h`: at step k the
 * loop reads `e[k]` and `e[len - 1 - k]`, and `len - 1 - k >= h > k - 1`
 * for every k < h, so no read ever lands on an index the loop has
 * already written. Writing `e[k]` in place is the same integer in the
 * same order with the staging removed.
 */
static inline __attribute__((always_inline))
void fwd_dct_1d_body(int32x4_t *out, int32x4_t *e, int n, int32x4_t sh)
{
    int32x4_t o[16];

    for (int len = n, oscale = 1; len > 1; len >>= 1, oscale <<= 1) {
        int h = len >> 1, rs = 32 / len;

        for (int k = 0; k < h; k++) {
            int32x4_t a = e[k], b = e[len - 1 - k];

            o[k] = vsubq_s32(a, b);
            e[k] = vaddq_s32(a, b);
        }
        for (int i = 0; i < h; i++)
            out[(2 * i + 1) * oscale] =
                RSH(fwd_odd_dot(o, n265_tmatrix[(2 * i + 1) * rs], h), sh);
    }
    out[0] = RSH(vshlq_n_s32(e[0], 6), sh);
}

/*
 * ONE INSTANCE PER LENGTH, and never inlined into the 2-D wrapper.
 *
 * Round 1 wrote a single routine with `n` as an argument, and the A2
 * census says what that costs. With `n` run-time the level loop has a
 * run-time trip count, every index into `e`, `o` and `out` is dynamic,
 * the matrix row address is a multiply, and `fwd_odd_dot` dispatches
 * through a switch on the run-time half-length at every one of its
 * calls. The NEON dispatch only ever reaches this routine at 16 and 32
 * (transform.c gates the class on `log2n >= 4 || use_dst`, and the DST
 * has its own 4-point form), so there are exactly two instances to make
 * and both of them fold: constant trip counts, constant `oscale` and
 * `rs`, constant `n265_tmatrix` row addresses, and the switch resolved
 * at every level.
 *
 * `noinline` for round 6's reason (docs/phase6-neon.md, round 6 part B):
 * an unrolled 32-point butterfly sitting in the same function as the
 * 16-point one makes both pay for the register allocation, and the
 * wrapper's own `e`/`v` arrays are in memory either way.
 */
static __attribute__((noinline))
void fwd_dct_1d_x4_16(int32x4_t *out, int32x4_t *e, int32x4_t sh)
{
    fwd_dct_1d_body(out, e, 16, sh);
}

static __attribute__((noinline))
void fwd_dct_1d_x4_32(int32x4_t *out, int32x4_t *e, int32x4_t sh)
{
    fwd_dct_1d_body(out, e, 32, sh);
}

static inline __attribute__((always_inline))
void fwd_dct_1d_x4(int32x4_t *out, int32x4_t *e, int n, int32x4_t sh)
{
    if (n == 16)
        fwd_dct_1d_x4_16(out, e, sh);
    else if (n == 32)
        fwd_dct_1d_x4_32(out, e, sh);
    else
        fwd_dct_1d_body(out, e, n, sh);
}

/* DST-VII 4-point forward, four columns at once; fwd_dst4_1d() verbatim.
 * Every output is a combination of the four inputs with total weight
 * 29 + 29 + 55 + 55 + 74 = 242, so with |x| <= 255 stage 1 stays under
 * 61710 and, after shift1 = 1, hands stage 2 inputs under 30855 for a
 * stage-2 bound of 7.5M. int32 throughout, and at Main10 the same
 * cancellation as the DCT's: 247K before a shift1 of 3, the same 30855
 * after it. */
static inline void fwd_dst4_1d_x4(int32x4_t *out, const int32x4_t *x,
                                  int32x4_t sh)
{
    int32x4_t c0 = vaddq_s32(x[0], x[3]);
    int32x4_t c1 = vaddq_s32(x[1], x[3]);
    int32x4_t c2 = vsubq_s32(x[0], x[1]);
    int32x4_t c3 = vmulq_n_s32(x[2], 74);
    int32x4_t m = vsubq_s32(vaddq_s32(x[0], x[1]), x[3]);

    out[0] = RSH(vaddq_s32(vmlaq_n_s32(vmulq_n_s32(c0, 29), c1, 55), c3), sh);
    out[1] = RSH(vmulq_n_s32(m, 74), sh);
    out[2] = RSH(vsubq_s32(vmlaq_n_s32(vmulq_n_s32(c2, 29), c0, 55), c3), sh);
    out[3] = RSH(vaddq_s32(vmlsq_n_s32(vmulq_n_s32(c2, 55), c1, 29), c3), sh);
}

/* Stage 1 down the columns, stage 2 across the rows of the result. `bt`
 * holds the stage-1 output TRANSPOSED (bt[x][r] = the C's b[r][x]) so
 * stage 2 loads four independent transforms contiguously, exactly as
 * stage 1 loads four residual columns contiguously. */
static inline void fdct_block(int16_t *coef, const int16_t *resid, int n,
                              int use_dst, int shift1, int shift2)
{
    int32x4_t sh1 = vdupq_n_s32(-shift1), sh2 = vdupq_n_s32(-shift2);
    int32_t bt[32 * 32];

    for (int x0 = 0; x0 < n; x0 += 4) {
        int32x4_t e[32], v[32], u[4];

        for (int y = 0; y < n; y++)
            e[y] = vmovl_s16(vld1_s16(resid + y * n + x0));
        if (use_dst)
            fwd_dst4_1d_x4(v, e, sh1);
        else
            fwd_dct_1d_x4(v, e, n, sh1);
        for (int r0 = 0; r0 < n; r0 += 4) {
            u[0] = v[r0]; u[1] = v[r0 + 1];
            u[2] = v[r0 + 2]; u[3] = v[r0 + 3];
            tr4_s32(u);
            for (int i = 0; i < 4; i++)
                vst1q_s32(bt + (x0 + i) * n + r0, u[i]);
        }
    }
    for (int r0 = 0; r0 < n; r0 += 4) {
        int32x4_t e[32], s[32], b[4];

        for (int k = 0; k < n; k++)
            e[k] = vld1q_s32(bt + k * n + r0);
        if (use_dst)
            fwd_dst4_1d_x4(s, e, sh2);
        else
            fwd_dct_1d_x4(s, e, n, sh2);
        for (int j0 = 0; j0 < n; j0 += 4) {
            b[0] = s[j0]; b[1] = s[j0 + 1];
            b[2] = s[j0 + 2]; b[3] = s[j0 + 3];
            tr4_s32(b);
            /* vqmovn_s32 IS clip3(-32768, 32767, v): the saturation is the
             * instruction's own, so the two paths agree on the values that
             * saturate and not only on the ones that do not. */
            for (int i = 0; i < 4; i++)
                vst1_s16(coef + (r0 + i) * n + j0, vqmovn_s32(b[i]));
        }
    }
}

/*
 * 4x4 and 8x8: the block IS the register file.
 *
 * The generic path above hands stage 1's output to stage 2 through an
 * int32 buffer. That is the right shape at 16 and 32 - 256 or 1024 int32
 * were never going to sit in 32 registers - and the wrong one here, where
 * the whole intermediate is four or sixteen vectors. At these two sizes
 * the round trip is real traffic, which is part of why round 1 measured
 * 0.38-0.65x against a C path clang auto-vectorises and left both on C.
 *
 * This is the same kernel with the buffer deleted: `bt` is an array of
 * vectors, every index into it is a constant once `n` is, and the only
 * memory the transform touches is the residual it loads and the
 * coefficients it stores. The lane assignment, the 1-D routine and the
 * order of every add are the generic path's, so it is bit-exact for the
 * same structural reason - and checkasm compares it against
 * n265_fdct_ref at both sizes, DST included.
 */
static inline __attribute__((always_inline))
void fdct_small(int16_t *coef, const int16_t *resid, int n, int use_dst,
                int shift1, int shift2)
{
    int32x4_t sh1 = vdupq_n_s32(-shift1), sh2 = vdupq_n_s32(-shift2);
    int32x4_t bt[8][2];         /* bt[x][r >> 2] lane r & 3 = the C's b[r][x] */

    for (int x0 = 0; x0 < n; x0 += 4) {
        int32x4_t e[8], v[8], u[4];

        for (int y = 0; y < n; y++)
            e[y] = vmovl_s16(vld1_s16(resid + y * n + x0));
        if (use_dst)
            fwd_dst4_1d_x4(v, e, sh1);
        else
            fwd_dct_1d_x4(v, e, n, sh1);
        for (int r0 = 0; r0 < n; r0 += 4) {
            u[0] = v[r0]; u[1] = v[r0 + 1];
            u[2] = v[r0 + 2]; u[3] = v[r0 + 3];
            tr4_s32(u);
            for (int i = 0; i < 4; i++)
                bt[x0 + i][r0 >> 2] = u[i];
        }
    }
    for (int r0 = 0; r0 < n; r0 += 4) {
        int32x4_t e[8], s[8], b[4];

        for (int k = 0; k < n; k++)
            e[k] = bt[k][r0 >> 2];
        if (use_dst)
            fwd_dst4_1d_x4(s, e, sh2);
        else
            fwd_dct_1d_x4(s, e, n, sh2);
        for (int j0 = 0; j0 < n; j0 += 4) {
            b[0] = s[j0]; b[1] = s[j0 + 1];
            b[2] = s[j0 + 2]; b[3] = s[j0 + 3];
            tr4_s32(b);
            for (int i = 0; i < 4; i++)
                vst1_s16(coef + (r0 + i) * n + j0, vqmovn_s32(b[i]));
        }
    }
}

/* One out-of-line function per size, and never inlined into the dispatch.
 * An unrolled 8x8 with sixteen live vectors sitting in the same function
 * as the generic kernel is a register allocation and an icache footprint
 * the 16x16 and 32x32 paths would then pay for, and they are the whole
 * value of the class. */
static __attribute__((noinline))
void fdct4_neon(int16_t *coef, const int16_t *resid, int use_dst,
                int shift1, int shift2)
{
    fdct_small(coef, resid, 4, use_dst, shift1, shift2);
}

static __attribute__((noinline))
void fdct8_neon(int16_t *coef, const int16_t *resid, int shift1, int shift2)
{
    fdct_small(coef, resid, 8, 0, shift1, shift2);
}

void n265_fdct_neon(int16_t *coef, const int16_t *resid, int log2n,
                    int use_dst)
{
    int shift1 = log2n + BIT_DEPTH - 9, shift2 = log2n + 6;

    if (use_dst) {
        fdct4_neon(coef, resid, 1, shift1, shift2);
        return;
    }
    switch (log2n) {
    case 2:  fdct4_neon(coef, resid, 0, shift1, shift2); break;
    case 3:  fdct8_neon(coef, resid, shift1, shift2); break;
    case 4:  fdct_block(coef, resid, 16, 0, shift1, shift2); break;
    default: fdct_block(coef, resid, 32, 0, shift1, shift2); break;
    }
}

/* ---- inverse ------------------------------------------------------- */

/*
 * One 1-D inverse DCT of length n on four columns at once: inv_dct_1d()
 * from transform.c, lane for lane.
 *
 * RANGE. The input is a dequantised coefficient, already clipped to int16
 * by n265_dequant, so |c| <= 32768. The level of length `len` adds at
 * most len * 90 * 32768 to e, and summing that over the levels 1, 2, ...,
 * n/2 plus the 64 * src[0] seed gives |e| <= 90 * 32768 * (n - 1) + 64 *
 * 32768 = 93.5M at n = 32. Stage 1 shifts by 7 and CLIPS to int16, so
 * stage 2 sees at most 32768 again and carries the same bound. int32
 * lanes hold both with more than four bits to spare, which is the
 * headroom the C's int32 accumulators run on.
 *
 * `clip` selects the stage-1 narrowing: vqmovn_s32 saturates at exactly
 * -32768 / 32767, so vmovl_s16(vqmovn_s32(v)) is clip3(-32768, 32767, v)
 * and the saturating case is the instruction's own rather than an
 * approximation of the C's ternary.
 */
static inline __attribute__((always_inline))
void inv_dct_1d_body(int32x4_t *dst, const int32x4_t *src, int n,
                     int32x4_t sh, int clip)
{
    int32x4_t e[32], o[16];

    e[0] = vshlq_n_s32(src[0], 6);
    for (int len = 1; len < n; len <<= 1) {
        int step = n / (2 * len), rs = 32 / (2 * len);

        for (int j = 0; j < len; j++)
            o[j] = vdupq_n_s32(0);
        for (int i = 0; i < len; i++)
            inv_odd_acc(o, src[(2 * i + 1) * step],
                        n265_tmatrix[(2 * i + 1) * rs], len);
        for (int j = len - 1; j >= 0; j--) {
            int32x4_t ev = e[j];

            e[j] = vaddq_s32(ev, o[j]);
            e[2 * len - 1 - j] = vsubq_s32(ev, o[j]);
        }
    }
    for (int j = 0; j < n; j++) {
        int32x4_t v = RSH(e[j], sh);

        dst[j] = clip ? vmovl_s16(vqmovn_s32(v)) : v;
    }
}

/* Same two instances, same reason, as the forward's (phase 13 A2): `n`
 * and `clip` are both compile-time at every call site once the 2-D
 * wrapper has its size, and with them the level loop unrolls, `step`,
 * `rs` and the matrix row addresses fold, and `inv_odd_acc`'s switch on
 * the run-time level width resolves. `clip` is a parameter of the
 * instance and not of the call because stage 1 always clips and stage 2
 * never does. */
static __attribute__((noinline))
void inv_dct_1d_x4_16(int32x4_t *dst, const int32x4_t *src, int32x4_t sh,
                      int clip)
{
    if (clip)
        inv_dct_1d_body(dst, src, 16, sh, 1);
    else
        inv_dct_1d_body(dst, src, 16, sh, 0);
}

static __attribute__((noinline))
void inv_dct_1d_x4_32(int32x4_t *dst, const int32x4_t *src, int32x4_t sh,
                      int clip)
{
    if (clip)
        inv_dct_1d_body(dst, src, 32, sh, 1);
    else
        inv_dct_1d_body(dst, src, 32, sh, 0);
}

static inline __attribute__((always_inline))
void inv_dct_1d_x4(int32x4_t *dst, const int32x4_t *src, int n,
                   int32x4_t sh, int clip)
{
    if (n == 16)
        inv_dct_1d_x4_16(dst, src, sh, clip);
    else if (n == 32)
        inv_dct_1d_x4_32(dst, src, sh, clip);
    else
        inv_dct_1d_body(dst, src, n, sh, clip);
}

/*
 * The same transform under the C's bound: every src entry at index >= nc
 * is zero. inv_dct_1d_nz() lane for lane, including the index arithmetic
 * that decides how many odd coefficients survive at each level - that is
 * a scalar decision about the LENGTH dimension and the lanes are the
 * orthogonal one, so the bound needs no rounding up to the vector width.
 * A coefficient zero in all four lanes is skipped, which is the C's
 * `if (!c) continue` weakened to a four-lane test; adding zero is exact
 * either way, so the two agree whatever the test decides.
 */
static inline __attribute__((always_inline))
void inv_dct_1d_nz_body(int32x4_t *dst, const int32x4_t *src,
                        int n, int32x4_t sh, int clip, int nc)
{
    int32x4_t e[32], o[16];

    e[0] = vshlq_n_s32(src[0], 6);
    for (int len = 1, lgstep = __builtin_ctz((unsigned)n) - 1; len < n;
         len <<= 1, lgstep--) {
        int stepi = 1 << lgstep;
        int rs = 32 / (2 * len);
        int nodd = ((nc + stepi - 1) >> lgstep) >> 1;

        if (nodd > len)
            nodd = len;
        if (nodd <= 0) {
            for (int j = 0; j < len; j++)
                e[2 * len - 1 - j] = e[j];
            continue;
        }
        for (int j = 0; j < len; j++)
            o[j] = vdupq_n_s32(0);
        for (int i = 0; i < nodd; i++) {
            int32x4_t c = src[(2 * i + 1) * stepi];

            if (len >= 4 && vminvq_u32(vceqzq_s32(c)) == 0xffffffffu)
                continue;               /* zero in all four lanes */
            inv_odd_acc(o, c, n265_tmatrix[(2 * i + 1) * rs], len);
        }
        for (int j = len - 1; j >= 0; j--) {
            int32x4_t ev = e[j];

            e[j] = vaddq_s32(ev, o[j]);
            e[2 * len - 1 - j] = vsubq_s32(ev, o[j]);
        }
    }
    for (int j = 0; j < n; j++) {
        int32x4_t v = RSH(e[j], sh);

        dst[j] = clip ? vmovl_s16(vqmovn_s32(v)) : v;
    }
}

/* The bounded form's two instances. `idct_block` only ever reaches it at
 * 16 and 32 - below that it takes the unbounded form, which costs less
 * than the bound's own index arithmetic - so the same two, and `nc` is
 * the one thing here that stays run-time. */
static __attribute__((noinline))
void inv_dct_1d_x4_nz_16(int32x4_t *dst, const int32x4_t *src, int32x4_t sh,
                         int clip, int nc)
{
    if (clip)
        inv_dct_1d_nz_body(dst, src, 16, sh, 1, nc);
    else
        inv_dct_1d_nz_body(dst, src, 16, sh, 0, nc);
}

static __attribute__((noinline))
void inv_dct_1d_x4_nz_32(int32x4_t *dst, const int32x4_t *src, int32x4_t sh,
                         int clip, int nc)
{
    if (clip)
        inv_dct_1d_nz_body(dst, src, 32, sh, 1, nc);
    else
        inv_dct_1d_nz_body(dst, src, 32, sh, 0, nc);
}

static inline __attribute__((always_inline))
void inv_dct_1d_x4_nz(int32x4_t *dst, const int32x4_t *src, int n,
                      int32x4_t sh, int clip, int nc)
{
    if (n == 16)
        inv_dct_1d_x4_nz_16(dst, src, sh, clip, nc);
    else if (n == 32)
        inv_dct_1d_x4_nz_32(dst, src, sh, clip, nc);
    else
        inv_dct_1d_nz_body(dst, src, n, sh, clip, nc);
}

/* DST-VII 4-point inverse, four columns at once; inv_dst4_1d() verbatim.
 * Same total weight of 242 as the forward, so with |i| <= 32768 no sum
 * exceeds 7.9M; stage 1 clips to int16, so stage 2 carries the same
 * bound. int32 throughout. */
static inline void inv_dst4_1d_x4(int32x4_t *out, const int32x4_t *in,
                                  int32x4_t sh, int clip)
{
    int32x4_t c0 = vaddq_s32(in[0], in[2]);
    int32x4_t c1 = vaddq_s32(in[2], in[3]);
    int32x4_t c2 = vsubq_s32(in[0], in[3]);
    int32x4_t c3 = vmulq_n_s32(in[1], 74);
    int32x4_t m = vaddq_s32(vsubq_s32(in[0], in[2]), in[3]);
    int32x4_t v[4];

    v[0] = RSH(vaddq_s32(vmlaq_n_s32(vmulq_n_s32(c0, 29), c1, 55), c3), sh);
    v[1] = RSH(vaddq_s32(vmlsq_n_s32(vmulq_n_s32(c2, 55), c1, 29), c3), sh);
    v[2] = RSH(vmulq_n_s32(m, 74), sh);
    v[3] = RSH(vsubq_s32(vmlaq_n_s32(vmulq_n_s32(c2, 29), c0, 55), c3), sh);
    for (int j = 0; j < 4; j++)
        out[j] = clip ? vmovl_s16(vqmovn_s32(v[j])) : v[j];
}

/* pred + residual clipped to a sample, four rows by four columns, the
 * residual already transposed so c[i] is row y0 + i. */
static inline void add_clip_4x4(pixel *dst, int ds, const pixel *pred,
                                int ps, int y0, int x0, int32x4_t c[4])
{
    for (int i = 0; i < 4; i++) {
        int32x4_t p = load4_pix(pred + (ptrdiff_t)(y0 + i) * ps + x0);

        store4_pix(dst + (ptrdiff_t)(y0 + i) * ds + x0, vaddq_s32(p, c[i]));
    }
}

/* eight columns at a time where there are eight: one store per row
 * instead of two, which matters because the output stage is a third of
 * this kernel's work at 16x16 and above. */
static inline void add_clip_4x8(pixel *dst, int ds, const pixel *pred,
                                int ps, int y0, int x0, int32x4_t lo[4],
                                int32x4_t hi[4])
{
    for (int i = 0; i < 4; i++) {
#if BIT_DEPTH == 8
        uint16x8_t p = vmovl_u8(vld1_u8(pred + (ptrdiff_t)(y0 + i) * ps + x0));
#else
        uint16x8_t p = vld1q_u16(pred + (ptrdiff_t)(y0 + i) * ps + x0);
#endif
        int32x4_t pl = vreinterpretq_s32_u32(vmovl_u16(vget_low_u16(p)));
        int32x4_t ph = vreinterpretq_s32_u32(vmovl_high_u16(p));
        uint16x4_t a = vqmovun_s32(vaddq_s32(pl, lo[i]));
        uint16x4_t b = vqmovun_s32(vaddq_s32(ph, hi[i]));

#if BIT_DEPTH == 8
        /* clip3(0, 255, v) again: saturate s32 -> u16 at 0, then u16 -> u8
         * at 255. The upper u16 bound cannot fire first - the residual is
         * under 2^24 - so neither clip can mask a value the C would keep. */
        vst1_u8(dst + (ptrdiff_t)(y0 + i) * ds + x0,
                vqmovn_u16(vcombine_u16(a, b)));
#else
        /* and clip3(0, PIXEL_MAX, v) at Main10: the s32 -> u16 saturation
         * is the lower half of it, the vmin the upper, exactly as in
         * store4_pix. */
        vst1q_u16(dst + (ptrdiff_t)(y0 + i) * ds + x0,
                  vminq_u16(vcombine_u16(a, b), vdupq_n_u16(PIXEL_MAX)));
#endif
    }
}

static inline void idct_block(pixel *dst, int ds, const int16_t *dq, int n,
                              const pixel *pred, int ps, int use_dst,
                              int nzw, int nzh)
{
    int32x4_t sh1 = vdupq_n_s32(-7);
    int32x4_t sh2 = vdupq_n_s32(-(20 - BIT_DEPTH));
    int16_t b[32 * 32];
    int xg = (nzw + 3) & ~3;

    /* stage 1: one lane per coefficient column. Columns at or past nzw are
     * zero and transform to zeros, so a partly covered group of four is
     * exact with no tail; only whole groups past xg are left out. */
    for (int x0 = 0; x0 < xg; x0 += 4) {
        int32x4_t e[32], v[32], u[4];

        for (int y = 0; y < n; y++)
            e[y] = vmovl_s16(vld1_s16(dq + y * n + x0));
        if (use_dst)
            inv_dst4_1d_x4(v, e, sh1, 1);
        else if (nzh >= n || n < 16)
            inv_dct_1d_x4(v, e, n, sh1, 1);
        else
            inv_dct_1d_x4_nz(v, e, n, sh1, 1, nzh);
        /* b[x][j], written transposed so stage 2 loads contiguously. It is
         * int16 because stage 1 clipped to int16: half the traffic, and
         * the value is what the C's int32 b holds by definition of that
         * clip. */
        for (int j0 = 0; j0 < n; j0 += 4) {
            u[0] = v[j0]; u[1] = v[j0 + 1];
            u[2] = v[j0 + 2]; u[3] = v[j0 + 3];
            tr4_s32(u);
            for (int i = 0; i < 4; i++)
                vst1_s16(b + (x0 + i) * n + j0, vqmovn_s32(u[i]));
        }
    }
    /* the small-block stage 2 below reads every column, so the ones stage
     * 1 skipped have to be the zeros they stand for. At 16 and 32 the
     * bounded stage 2 reads only indices under nzw <= xg, and the rows
     * past xg are never touched: inv_dct_1d_nz's largest index is
     * (ceil(nc / stepi) - 1) * stepi, which is strictly below nc. */
    if (n < 16 && xg < n)
        memset(b + (size_t)xg * n, 0, (size_t)(n - xg) * n * sizeof(int16_t));

    /* stage 2: one lane per spatial row. */
    for (int y0 = 0; y0 < n; y0 += 4) {
        int32x4_t e[32], r[32], c[4], c2[4];

        for (int k = 0; k < n; k++)
            e[k] = vmovl_s16(vld1_s16(b + k * n + y0));
        if (use_dst)
            inv_dst4_1d_x4(r, e, sh2, 0);
        else if (nzw >= n || n < 16)
            inv_dct_1d_x4(r, e, n, sh2, 0);
        else
            inv_dct_1d_x4_nz(r, e, n, sh2, 0, nzw);
        if (n == 4) {
            c[0] = r[0]; c[1] = r[1]; c[2] = r[2]; c[3] = r[3];
            tr4_s32(c);
            add_clip_4x4(dst, ds, pred, ps, y0, 0, c);
            continue;
        }
        for (int x0 = 0; x0 < n; x0 += 8) {
            c[0] = r[x0];      c[1] = r[x0 + 1];
            c[2] = r[x0 + 2];  c[3] = r[x0 + 3];
            c2[0] = r[x0 + 4]; c2[1] = r[x0 + 5];
            c2[2] = r[x0 + 6]; c2[3] = r[x0 + 7];
            tr4_s32(c);
            tr4_s32(c2);
            add_clip_4x8(dst, ds, pred, ps, y0, x0, c, c2);
        }
    }
}

/* Both stages collapse to a constant when only the DC coefficient can be
 * nonzero, and it is the constant the general path forms: E mirrors at
 * every level, so stage 1 emits clip16((64 * dq0 + 64) >> 7) in every row
 * and stage 2 turns that into (64 * v + (1 << (19 - BitDepth))) >>
 * (20 - BitDepth) in every column. Same reduction the C entry point
 * makes; worth vectorising because at 32x32 it is 1024 clipped sample
 * adds.
 *
 * int16 lanes hold the whole add at both depths: |r| <= 64 * 32768 >>
 * (20 - BitDepth), which is 512 at 8 bits and 2048 at Main10, and the
 * sample it is added to is at most PIXEL_MAX, so |s| <= 3071 either way.
 * The clip differs, and only because the narrow's own bound is the right
 * one at 8 bits (vqmovun_s16 saturates at 0 and 255) and is not at
 * Main10, where it is a max/min pair. */
static inline void idct_dc(pixel *dst, int ds, int32_t r, int n,
                           const pixel *pred, int ps)
{
    int16x8_t rv = vdupq_n_s16((int16_t)r);

    for (int y = 0; y < n; y++) {
        const pixel *p = pred + (ptrdiff_t)y * ps;
        pixel *d = dst + (ptrdiff_t)y * ds;

        for (int x = 0; x < n; x += 8) {
#if BIT_DEPTH == 8
            int16x8_t s = vaddq_s16(
                vreinterpretq_s16_u16(vmovl_u8(vld1_u8(p + x))), rv);

            vst1_u8(d + x, vqmovun_s16(s));
#else
            int16x8_t s = vaddq_s16(
                vreinterpretq_s16_u16(vld1q_u16(p + x)), rv);

            s = vminq_s16(vmaxq_s16(s, vdupq_n_s16(0)),
                          vdupq_n_s16(PIXEL_MAX));
            vst1q_u16(d + x, vreinterpretq_u16_s16(s));
#endif
        }
    }
}

/* The inverse at 4x4 and 8x8, register-resident for the same reason as
 * fdct_small: `b` is the stage-1 output, sixteen vectors at most, and it
 * never leaves the register file. Stage 1 already clipped to int16 (the
 * `clip` arm of inv_dct_1d_x4), so the generic path's narrow-to-int16
 * store and widening reload are the identity on these values and dropping
 * them changes nothing. The column bound is kept: a whole group of four
 * columns at or past nzw transforms to zeros, and stage 2 here reads
 * every column, so those groups are set to zero rather than skipped. */
static inline __attribute__((always_inline))
void idct_small(pixel *dst, int ds, const int16_t *dq, int n,
                const pixel *pred, int ps, int use_dst, int nzw)
{
    int32x4_t sh1 = vdupq_n_s32(-7);
    int32x4_t sh2 = vdupq_n_s32(-(20 - BIT_DEPTH));
    int32x4_t b[8][2];          /* b[x][j >> 2] lane j & 3 = the C's b[x][j] */
    int xg = (nzw + 3) & ~3;

    for (int x0 = 0; x0 < n; x0 += 4) {
        int32x4_t e[8], v[8], u[4];

        if (x0 >= xg) {
            for (int i = 0; i < 4; i++)
                for (int g = 0; g < (n >> 2); g++)
                    b[x0 + i][g] = vdupq_n_s32(0);
            continue;
        }
        for (int y = 0; y < n; y++)
            e[y] = vmovl_s16(vld1_s16(dq + y * n + x0));
        if (use_dst)
            inv_dst4_1d_x4(v, e, sh1, 1);
        else
            inv_dct_1d_x4(v, e, n, sh1, 1);
        for (int j0 = 0; j0 < n; j0 += 4) {
            u[0] = v[j0]; u[1] = v[j0 + 1];
            u[2] = v[j0 + 2]; u[3] = v[j0 + 3];
            tr4_s32(u);
            for (int i = 0; i < 4; i++)
                b[x0 + i][j0 >> 2] = u[i];
        }
    }
    for (int y0 = 0; y0 < n; y0 += 4) {
        int32x4_t e[8], r[8], c[4], c2[4];

        for (int k = 0; k < n; k++)
            e[k] = b[k][y0 >> 2];
        if (use_dst)
            inv_dst4_1d_x4(r, e, sh2, 0);
        else
            inv_dct_1d_x4(r, e, n, sh2, 0);
        if (n == 4) {
            c[0] = r[0]; c[1] = r[1]; c[2] = r[2]; c[3] = r[3];
            tr4_s32(c);
            add_clip_4x4(dst, ds, pred, ps, y0, 0, c);
            continue;
        }
        for (int x0 = 0; x0 < n; x0 += 8) {
            c[0] = r[x0];      c[1] = r[x0 + 1];
            c[2] = r[x0 + 2];  c[3] = r[x0 + 3];
            c2[0] = r[x0 + 4]; c2[1] = r[x0 + 5];
            c2[2] = r[x0 + 6]; c2[3] = r[x0 + 7];
            tr4_s32(c);
            tr4_s32(c2);
            add_clip_4x8(dst, ds, pred, ps, y0, x0, c, c2);
        }
    }
}

/* out of line, for the reason fdct4_neon gives */
static __attribute__((noinline))
void idct4_neon(pixel *dst, int ds, const int16_t *dq, const pixel *pred,
                int ps, int use_dst, int nzw)
{
    idct_small(dst, ds, dq, 4, pred, ps, use_dst, nzw);
}

static __attribute__((noinline))
void idct8_neon(pixel *dst, int ds, const int16_t *dq, const pixel *pred,
                int ps, int nzw)
{
    idct_small(dst, ds, dq, 8, pred, ps, 0, nzw);
}

void n265_idct_add_neon(pixel *dst, int dst_stride, const int16_t *dq,
                        int log2n, const pixel *pred, int pred_stride,
                        int use_dst, int nzw, int nzh)
{
    int n = 1 << log2n;

    if (use_dst) {
        nzw = nzh = n;
    } else {
        if (nzw < 1) nzw = 1;
        if (nzh < 1) nzh = 1;
        if (nzw > n) nzw = n;
        if (nzh > n) nzh = n;
    }

    if (!use_dst && nzw == 1 && nzh == 1) {
        int32_t v = (64 * dq[0] + 64) >> 7;
        int32_t r;

        v = v < -32768 ? -32768 : v > 32767 ? 32767 : v;
        r = (64 * v + (1 << (19 - BIT_DEPTH))) >> (20 - BIT_DEPTH);
        if (n >= 8) {
            idct_dc(dst, dst_stride, r, n, pred, pred_stride);
        } else {
            int32x4_t rv = vdupq_n_s32(r);

            for (int y = 0; y < 4; y++)
                store4_pix(dst + (ptrdiff_t)y * dst_stride,
                          vaddq_s32(load4_pix(pred + (ptrdiff_t)y *
                                             pred_stride), rv));
        }
        return;
    }

    if (use_dst) {
        idct4_neon(dst, dst_stride, dq, pred, pred_stride, 1, 4);
        return;
    }
    switch (log2n) {
    case 2:
        idct4_neon(dst, dst_stride, dq, pred, pred_stride, 0, nzw);
        break;
    case 3:
        idct8_neon(dst, dst_stride, dq, pred, pred_stride, nzw);
        break;
    case 4:
        idct_block(dst, dst_stride, dq, 16, pred, pred_stride, 0, nzw, nzh);
        break;
    default:
        idct_block(dst, dst_stride, dq, 32, pred, pred_stride, 0, nzw, nzh);
        break;
    }
}


/* ---- quant / dequant (class N265_ASM_QUANT) ------------------------
 *
 * These two live here because they are the transform's other half and
 * the family owns the file, but they carry their own class bit: the
 * campaign prices dequant separately from the butterflies, and the
 * shape survey found it at 1.3% of a foreman encode against the pair's
 * 12%.
 *
 * Both C forms reach for int64 and neither needs to, which is the whole
 * trick that makes four lanes possible instead of two.
 */

/* n265_dequant's arithmetic is `(level * S + add) >> bd_shift` with
 * S = level_scale[qP % 6] << (qP / 6) * 16 and |level| <= 32768, so the
 * product reaches 9.7e9 at 8 bits (3.9e10 at Main10) and the C carries
 * it in int64. Split S at the shift instead - S = Sh * 2^bd + Sl - and
 *
 *     (level * S + add) >> bd == level * Sh + ((level * Sl + add) >> bd)
 *
 * exactly, for either sign, because the first term is a whole multiple
 * of 2^bd and >> is floor division by it. Now every part is int32, and
 * AT BOTH DEPTHS: the depth moves S and bd_shift together, by two bits
 * of Qp' and two bits of BitDepth, so Sh does not move at all. 8 bits
 * gives Sh <= 294912 >> 5 = 9216, Main10 gives 1179648 >> 7 = 9216, and
 * either way |level * Sh| <= 302M; Sl < 2^bd <= 1024 gives
 * |level * Sl| <= 33.6M, and the sum is under 336M. Four lanes, not two.
 *
 * `qp` arrives as QpY - the 8-bit-domain index, negative below zero at
 * Main10 - and the spec's qP is Qp' (8.6.3), so the offset is added here
 * exactly as n265_dequant_ref adds it. At 8 bits QP_BD_OFFSET is 0 and
 * every expression below folds to the one that was here.
 *
 * A zero level needs no special case: (0 + 2^(bd-1)) >> bd is 0, which
 * is the C's early-out value, so the branch the C takes to skip the
 * int64 multiply is arithmetic here and costs nothing.
 *
 * The bounding box comes out of the same pass. Per row, each lane
 * contributes its own x where the result is nonzero and -1 where it is
 * not; vmaxvq_s32 across the row gives the row's last nonzero column,
 * and a row that has one is the last nonzero row so far. That is the C's
 * mx / my exactly, including the all-zero block's mx = my = 0. */
void n265_dequant_neon(int16_t *dq, const int16_t *level, int log2n, int qp,
                       int *nzw, int *nzh)
{
    static const int level_scale[6] = { 40, 45, 51, 57, 64, 72 };
    static const int32_t lane_id[4] = { 0, 1, 2, 3 };
    int n = 1 << log2n;
    int qpq = qp + QP_BD_OFFSET;                 /* Qp', 0 .. 63 */
    int bd_shift = BIT_DEPTH + log2n - 5;        /* 5 .. 8, 7 .. 10 at 10 */
    int32_t s = (int32_t)(((int64_t)level_scale[qpq % 6] << (qpq / 6)) * 16);
    int32_t s_hi = s >> bd_shift, s_lo = s & ((1 << bd_shift) - 1);
    int32x4_t addv = vdupq_n_s32(1 << (bd_shift - 1));
    int32x4_t sh = vdupq_n_s32(-bd_shift);
    int32x4_t none = vdupq_n_s32(-1);
    int32x4_t ids = vld1q_s32(lane_id);
    int mx = 0, my = 0;

    for (int y = 0; y < n; y++) {
        const int16_t *lp = level + (ptrdiff_t)y * n;
        int16_t *dp = dq + (ptrdiff_t)y * n;
        int32x4_t rowmax = none;
        int r;

        for (int x = 0; x < n; x += 4) {
            int32x4_t l = vmovl_s16(vld1_s16(lp + x));
            int32x4_t v = vaddq_s32(
                vmulq_n_s32(l, s_hi),
                vshlq_s32(vmlaq_n_s32(addv, l, s_lo), sh));

            /* vqmovn_s32 is the C's clip to int16, saturation included */
            vst1_s16(dp + x, vqmovn_s32(v));
            rowmax = vmaxq_s32(rowmax,
                               vbslq_s32(vceqzq_s32(v), none,
                                         vaddq_s32(ids, vdupq_n_s32(x))));
        }
        r = vmaxvq_s32(rowmax);
        if (r >= 0) {
            my = y;
            if (r > mx)
                mx = r;
        }
    }
    if (nzw) *nzw = mx + 1;
    if (nzh) *nzh = my + 1;
}

/* n265_quant's int64 is unnecessary too. qbits = 29 - BitDepth + qP / 6
 * - log2n is in [16, 27] at 8 bits and [14, 27] at Main10 - the top is
 * the same number, because Qp's twelve extra steps buy exactly the two
 * bits BitDepth costs - so the deadzone add is 2^qbits / 3 <= 44.7M at
 * both, |coef| <= 32768 and quant_scale <= 26214 give
 * |coef| * q + add <= 9.04e8, inside int32 with a bit to spare, and the
 * shift, the 32767 clamp and the sign are all int32 from there.
 * vabsq_s32 after the widen and not before it: the C negates an `int`,
 * and |-32768| does not fit int16.
 *
 * `qp` is QpY here too, and the table index is Qp': same substitution as
 * the dequant above, and the same fold to the old expression at 8 bits.
 *
 * The nonzero count is a lane accumulator (subtract the all-ones compare
 * mask, which adds one) reduced once at the end, so the C's per-element
 * `nz += l != 0` never becomes a branch. */
int n265_quant_neon(int16_t *level, const int16_t *coef, int log2n, int qp,
                    int intra)
{
    static const int quant_scale[6] = {
        26214, 23302, 20560, 18396, 16384, 14564
    };
    int nn = 1 << (2 * log2n);                   /* 16 at least, so 8 fits */
    int qpq = qp + QP_BD_OFFSET;                 /* Qp', 0 .. 63 */
    int qbits = 29 - BIT_DEPTH + qpq / 6 - log2n;
    int32_t add = (int32_t)(((int64_t)1 << qbits) / (intra ? 3 : 6));
    int q = quant_scale[qpq % 6];
    int32x4_t addv = vdupq_n_s32(add), sh = vdupq_n_s32(-qbits);
    int32x4_t maxv = vdupq_n_s32(32767);
    int32x4_t cnt = vdupq_n_s32(0);

    for (int i = 0; i < nn; i += 8) {
        int16x8_t c = vld1q_s16(coef + i);
        int32x4_t cl = vmovl_s16(vget_low_s16(c)), ch = vmovl_high_s16(c);
        int32x4_t ll = vminq_s32(
            vshlq_s32(vmlaq_n_s32(addv, vabsq_s32(cl), q), sh), maxv);
        int32x4_t lh = vminq_s32(
            vshlq_s32(vmlaq_n_s32(addv, vabsq_s32(ch), q), sh), maxv);

        cnt = vsubq_s32(cnt,
                        vreinterpretq_s32_u32(vmvnq_u32(vceqzq_s32(ll))));
        cnt = vsubq_s32(cnt,
                        vreinterpretq_s32_u32(vmvnq_u32(vceqzq_s32(lh))));
        /* |sign * l| <= 32767, so vqmovn_s32 never actually saturates
         * here; it is the narrow, and the clamp above is the C's. */
        vst1q_s16(level + i, vcombine_s16(
            vqmovn_s32(vbslq_s32(vcltzq_s32(cl), vnegq_s32(ll), ll)),
            vqmovn_s32(vbslq_s32(vcltzq_s32(ch), vnegq_s32(lh), lh))));
    }
    return vaddvq_s32(cnt);
}

#else
typedef int n265_transform_neon_unused_;  /* C wants a non-empty TU */
#endif /* N265_NEON_TRANSFORM */
