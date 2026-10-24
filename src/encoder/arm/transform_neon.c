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

/* ---- the DCT at 4x4 and 8x8 in int16 lanes (phase 14 item 39) ------
 *
 * The int32 form above runs four transforms per vector, and at these two
 * sizes that left both on C. These run eight (8x8) or four (4x4) per
 * vector in int16 and widen only where a value can leave int16: every
 * stage-1 butterfly sum and difference is an int16 add, every multiply
 * is a widening int16 x constant into int32, and the rounding shift
 * narrows straight back.
 *
 * RANGE. The residual is src - pred, |r| <= PIXEL_MAX (the checkasm
 * domain, and every caller's). At 8 bits stage 1's sums are |e| <= 510,
 * |EE| <= 1020 and |EE0 + EE1| <= 2040; at Main10 four times those, 8184
 * at most, so all of them are int16. The pre-shift stage-1 outputs are
 * bounded by 64 * 2040 = 130560 (the 64 rows), 119 * 1020 = 121380 (the
 * 83/36 rows) and 232 * 510 = 118320 (the odd rows) at 8x8 and 8 bits,
 * and the shift of 2 brings the largest to 32640. At 4x4 the same rows
 * give 65280 and 60690 against a shift of 1: 32640 again. Main10
 * multiplies each input by four and adds two to the shift, so the
 * post-shift bound is unmoved. The stage-1 output is therefore exact in
 * int16, which is the value the C's int32 `b` holds. Stage 2's butterfly
 * sums of two such values are NOT int16, so stage 2 widens first
 * (vaddl/vsubl) and runs in int32.
 *
 * Every sum below is the same integer the C's fwd_dct_1d forms. The
 * grouping differs (the 4-point even part is written out instead of
 * walked as levels), but nothing overflows, so integer addition is exact
 * in any order. The rounding narrow is the C's (s + half) >> shift, and
 * its saturating form is clip3(-32768, 32767, .): the instruction's own
 * saturation, at both stages. */

/* the 4x4 int16 transpose, in d registers */
static inline void tr4_s16(int16x4_t v[4])
{
    int16x4_t a0 = vtrn1_s16(v[0], v[1]), a1 = vtrn2_s16(v[0], v[1]);
    int16x4_t a2 = vtrn1_s16(v[2], v[3]), a3 = vtrn2_s16(v[2], v[3]);
    int32x2_t b0 = vreinterpret_s32_s16(a0), b1 = vreinterpret_s32_s16(a1);
    int32x2_t b2 = vreinterpret_s32_s16(a2), b3 = vreinterpret_s32_s16(a3);

    v[0] = vreinterpret_s16_s32(vtrn1_s32(b0, b2));
    v[1] = vreinterpret_s16_s32(vtrn1_s32(b1, b3));
    v[2] = vreinterpret_s16_s32(vtrn2_s32(b0, b2));
    v[3] = vreinterpret_s16_s32(vtrn2_s32(b1, b3));
}

/* the 8x8 int16 transpose: pair on 16, then 32, then 64 */
static inline void tr8_s16(int16x8_t v[8])
{
    int16x8_t t[8];
    int32x4_t u[8];

    for (int i = 0; i < 8; i += 2) {
        t[i] = vtrn1q_s16(v[i], v[i + 1]);
        t[i + 1] = vtrn2q_s16(v[i], v[i + 1]);
    }
    for (int i = 0; i < 8; i += 4) {
        u[i] = vtrn1q_s32(vreinterpretq_s32_s16(t[i]),
                          vreinterpretq_s32_s16(t[i + 2]));
        u[i + 2] = vtrn2q_s32(vreinterpretq_s32_s16(t[i]),
                              vreinterpretq_s32_s16(t[i + 2]));
        u[i + 1] = vtrn1q_s32(vreinterpretq_s32_s16(t[i + 1]),
                              vreinterpretq_s32_s16(t[i + 3]));
        u[i + 3] = vtrn2q_s32(vreinterpretq_s32_s16(t[i + 1]),
                              vreinterpretq_s32_s16(t[i + 3]));
    }
    for (int i = 0; i < 4; i++) {
        int64x2_t a = vreinterpretq_s64_s32(u[i]);
        int64x2_t b = vreinterpretq_s64_s32(u[i + 4]);

        v[i] = vreinterpretq_s16_s64(vtrn1q_s64(a, b));
        v[i + 4] = vreinterpretq_s16_s64(vtrn2q_s64(a, b));
    }
}

#define FSH1_4 (BIT_DEPTH - 7)          /* log2n + BitDepth - 9 */
#define FSH1_8 (BIT_DEPTH - 6)

static __attribute__((noinline))
void fdct4_s16(int16_t *coef, const int16_t *resid)
{
    int16x4_t r0 = vld1_s16(resid), r1 = vld1_s16(resid + 4);
    int16x4_t r2 = vld1_s16(resid + 8), r3 = vld1_s16(resid + 12);
    int16x4_t e0 = vadd_s16(r0, r3), e1 = vadd_s16(r1, r2);
    int16x4_t o0 = vsub_s16(r0, r3), o1 = vsub_s16(r1, r2);
    int16x4_t b[4], c[4];

    /* stage 1, lanes = columns: b[k] lane x is the C's b[k][x] */
    b[0] = vqrshrn_n_s32(vshll_n_s16(vadd_s16(e0, e1), 6), FSH1_4);
    b[2] = vqrshrn_n_s32(vshll_n_s16(vsub_s16(e0, e1), 6), FSH1_4);
    b[1] = vqrshrn_n_s32(vmlal_n_s16(vmull_n_s16(o0, 83), o1, 36), FSH1_4);
    b[3] = vqrshrn_n_s32(vmlsl_n_s16(vmull_n_s16(o0, 36), o1, 83), FSH1_4);
    tr4_s16(b);                         /* b[x], lanes = rows */
    /* stage 2, lanes = rows: c[j] is column j of the coefficients */
    {
        int32x4_t e0w = vaddl_s16(b[0], b[3]), e1w = vaddl_s16(b[1], b[2]);
        int32x4_t o0w = vsubl_s16(b[0], b[3]), o1w = vsubl_s16(b[1], b[2]);

        c[0] = vqrshrn_n_s32(vshlq_n_s32(vaddq_s32(e0w, e1w), 6), 8);
        c[2] = vqrshrn_n_s32(vshlq_n_s32(vsubq_s32(e0w, e1w), 6), 8);
        c[1] = vqrshrn_n_s32(vmlaq_n_s32(vmulq_n_s32(o0w, 83), o1w, 36), 8);
        c[3] = vqrshrn_n_s32(vmlsq_n_s32(vmulq_n_s32(o0w, 36), o1w, 83), 8);
    }
    tr4_s16(c);
    vst1q_s16(coef, vcombine_s16(c[0], c[1]));
    vst1q_s16(coef + 8, vcombine_s16(c[2], c[3]));
}

/* stage 1 of the 8-point DCT on one half (lo or hi) of eight int16
 * vectors: the 4-point even rows on EE/EO, the odd rows on o */
#define FDCT8_S1_HALF(b, e, o, MULL, MLAL, MLSL, SHLL) do {                \
        int16x8_t ee0_ = vaddq_s16(e[0], e[3]);                            \
        int16x8_t ee1_ = vaddq_s16(e[1], e[2]);                            \
        int16x8_t eo0_ = vsubq_s16(e[0], e[3]);                            \
        int16x8_t eo1_ = vsubq_s16(e[1], e[2]);                            \
        (b)[0] = SHLL(vaddq_s16(ee0_, ee1_));                              \
        (b)[4] = SHLL(vsubq_s16(ee0_, ee1_));                              \
        (b)[2] = MLAL(MULL(eo0_, 83), eo1_, 36);                           \
        (b)[6] = MLSL(MULL(eo0_, 36), eo1_, 83);                           \
        (b)[1] = MLAL(MLAL(MLAL(MULL(o[0], 89), o[1], 75), o[2], 50),      \
                      o[3], 18);                                           \
        (b)[3] = MLSL(MLSL(MLSL(MULL(o[0], 75), o[1], 18), o[2], 89),      \
                      o[3], 50);                                           \
        (b)[5] = MLAL(MLAL(MLSL(MULL(o[0], 50), o[1], 89), o[2], 18),      \
                      o[3], 75);                                           \
        (b)[7] = MLSL(MLAL(MLSL(MULL(o[0], 18), o[1], 50), o[2], 75),      \
                      o[3], 89);                                           \
    } while (0)

#define MULL_LO(v, c) vmull_n_s16(vget_low_s16(v), c)
#define MLAL_LO(a, v, c) vmlal_n_s16(a, vget_low_s16(v), c)
#define MLSL_LO(a, v, c) vmlsl_n_s16(a, vget_low_s16(v), c)
#define SHLL_LO(v) vshll_n_s16(vget_low_s16(v), 6)
#define MULL_HI(v, c) vmull_high_n_s16(v, c)
#define MLAL_HI(a, v, c) vmlal_high_n_s16(a, v, c)
#define MLSL_HI(a, v, c) vmlsl_high_n_s16(a, v, c)
#define SHLL_HI(v) vshll_high_n_s16(v, 6)

/* the 8-point DCT on int32 vectors (stage 2), one output per matrix row */
static inline void fdct8_s32(int32x4_t out[8], const int32x4_t e[4],
                             const int32x4_t o[4])
{
    int32x4_t ee0 = vaddq_s32(e[0], e[3]), ee1 = vaddq_s32(e[1], e[2]);
    int32x4_t eo0 = vsubq_s32(e[0], e[3]), eo1 = vsubq_s32(e[1], e[2]);

    out[0] = vshlq_n_s32(vaddq_s32(ee0, ee1), 6);
    out[4] = vshlq_n_s32(vsubq_s32(ee0, ee1), 6);
    out[2] = vmlaq_n_s32(vmulq_n_s32(eo0, 83), eo1, 36);
    out[6] = vmlsq_n_s32(vmulq_n_s32(eo0, 36), eo1, 83);
    out[1] = vmlaq_n_s32(vmlaq_n_s32(vmlaq_n_s32(vmulq_n_s32(o[0], 89),
                                                 o[1], 75), o[2], 50),
                         o[3], 18);
    out[3] = vmlsq_n_s32(vmlsq_n_s32(vmlsq_n_s32(vmulq_n_s32(o[0], 75),
                                                 o[1], 18), o[2], 89),
                         o[3], 50);
    out[5] = vmlaq_n_s32(vmlaq_n_s32(vmlsq_n_s32(vmulq_n_s32(o[0], 50),
                                                 o[1], 89), o[2], 18),
                         o[3], 75);
    out[7] = vmlsq_n_s32(vmlaq_n_s32(vmlsq_n_s32(vmulq_n_s32(o[0], 18),
                                                 o[1], 50), o[2], 75),
                         o[3], 89);
}

static __attribute__((noinline))
void fdct8_s16(int16_t *coef, const int16_t *resid)
{
    int16x8_t r[8], e[4], o[4], b[8];
    int32x4_t lo[8], hi[8];

    for (int y = 0; y < 8; y++)
        r[y] = vld1q_s16(resid + y * 8);
    for (int k = 0; k < 4; k++) {
        e[k] = vaddq_s16(r[k], r[7 - k]);
        o[k] = vsubq_s16(r[k], r[7 - k]);
    }
    /* stage 1, lanes = columns */
    FDCT8_S1_HALF(lo, e, o, MULL_LO, MLAL_LO, MLSL_LO, SHLL_LO);
    FDCT8_S1_HALF(hi, e, o, MULL_HI, MLAL_HI, MLSL_HI, SHLL_HI);
    for (int k = 0; k < 8; k++)
        b[k] = vcombine_s16(vqrshrn_n_s32(lo[k], FSH1_8),
                            vqrshrn_n_s32(hi[k], FSH1_8));
    tr8_s16(b);                         /* b[x], lanes = rows */
    /* stage 2, lanes = rows, int32 from the first butterfly on */
    {
        int32x4_t el[4], ol[4], eh[4], oh[4];

        for (int k = 0; k < 4; k++) {
            el[k] = vaddl_s16(vget_low_s16(b[k]), vget_low_s16(b[7 - k]));
            ol[k] = vsubl_s16(vget_low_s16(b[k]), vget_low_s16(b[7 - k]));
            eh[k] = vaddl_high_s16(b[k], b[7 - k]);
            oh[k] = vsubl_high_s16(b[k], b[7 - k]);
        }
        fdct8_s32(lo, el, ol);
        fdct8_s32(hi, eh, oh);
    }
    for (int j = 0; j < 8; j++)
        b[j] = vcombine_s16(vqrshrn_n_s32(lo[j], 9),
                            vqrshrn_n_s32(hi[j], 9));
    tr8_s16(b);
    for (int y = 0; y < 8; y++)
        vst1q_s16(coef + y * 8, b[y]);
}

/* ---- 16x16 and 32x32 in int16 lanes (phase 14 item 39) ---------------
 *
 * The int32 forms these replaced ran four transforms per vector, and
 * the compiler neither unrolled their level loops nor kept
 * the by-lane multiply: every coefficient became a dup and a widen ahead
 * of its MLA, and a 32x32 forward retired 33K instructions. These run
 * eight transforms per vector with the butterflies in int16 wherever the
 * value is int16, every level written out, and the coefficients read as
 * lanes of a constant table, so a multiply-accumulate is one smlal (or
 * one mla in the forward's int32 second stage).
 *
 * The odd half of an N-point matrix - rows 2i + 1, columns j < N/2 - is
 * symmetric, since its entry is a cosine of (2i + 1)(2j + 1). So one
 * table serves the forward's odd outputs (row i dotted with the
 * differences) and the inverse's odd sums (column j dotted with the odd
 * coefficients). The 8-point rows are padded to eight so a row is one
 * q-register load.
 *
 * Every sum is the same integer the C's partial butterflies form (no
 * level overflows int32, so the grouping is free) and every narrow is
 * the C's rounding shift; the range notes sit with each stage. */

#define DCT_O32_ROWS {                                                     \
    { 90, 90, 88, 85, 82, 78, 73, 67, 61, 54, 46, 38, 31, 22, 13, 4 },    \
    { 90, 82, 67, 46, 22, -4, -31, -54, -73, -85, -90, -88, -78, -61,      \
      -38, -13 },                                                          \
    { 88, 67, 31, -13, -54, -82, -90, -78, -46, -4, 38, 73, 90, 85, 61,    \
      22 },                                                                \
    { 85, 46, -13, -67, -90, -73, -22, 38, 82, 88, 54, -4, -61, -90, -78,  \
      -31 },                                                               \
    { 82, 22, -54, -90, -61, 13, 78, 85, 31, -46, -90, -67, 4, 73, 88,     \
      38 },                                                                \
    { 78, -4, -82, -73, 13, 85, 67, -22, -88, -61, 31, 90, 54, -38, -90,   \
      -46 },                                                               \
    { 73, -31, -90, -22, 78, 67, -38, -90, -13, 82, 61, -46, -88, -4, 85,  \
      54 },                                                                \
    { 67, -54, -78, 38, 85, -22, -90, 4, 90, 13, -88, -31, 82, 46, -73,    \
      -61 },                                                               \
    { 61, -73, -46, 82, 31, -88, -13, 90, -4, -90, 22, 85, -38, -78, 54,   \
      67 },                                                                \
    { 54, -85, -4, 88, -46, -61, 82, 13, -90, 38, 67, -78, -22, 90, -31,   \
      -73 },                                                               \
    { 46, -90, 38, 54, -90, 31, 61, -88, 22, 67, -85, 13, 73, -82, 4,      \
      78 },                                                                \
    { 38, -88, 73, -4, -67, 90, -46, -31, 85, -78, 13, 61, -90, 54, 22,    \
      -82 },                                                               \
    { 31, -78, 90, -61, 4, 54, -88, 82, -38, -22, 73, -90, 67, -13, -46,   \
      85 },                                                                \
    { 22, -61, 85, -90, 73, -38, -4, 46, -78, 90, -82, 54, -13, -31, 67,   \
      -88 },                                                               \
    { 13, -38, 61, -78, 88, -90, 85, -73, 54, -31, 4, 22, -46, 67, -82,    \
      90 },                                                                \
    { 4, -13, 22, -31, 38, -46, 54, -61, 67, -73, 78, -82, 85, -88, 90,    \
      -90 },                                                               \
}

#define DCT_O16_ROWS {                                                     \
    { 90, 87, 80, 70, 57, 43, 25, 9 },                                     \
    { 87, 57, 9, -43, -80, -90, -70, -25 },                                \
    { 80, 9, -70, -87, -25, 57, 90, 43 },                                  \
    { 70, -43, -87, 9, 90, 25, -80, -57 },                                 \
    { 57, -80, -25, 90, -9, -87, 43, 70 },                                 \
    { 43, -90, 57, 25, -87, 70, 9, -80 },                                  \
    { 25, -70, 90, -80, 43, 9, -57, 87 },                                  \
    { 9, -25, 43, -57, 70, -80, 87, -90 },                                 \
}

#define DCT_O8_ROWS {                                                      \
    { 89, 75, 50, 18, 0, 0, 0, 0 },                                        \
    { 75, -18, -89, -50, 0, 0, 0, 0 },                                     \
    { 50, -89, 18, 75, 0, 0, 0, 0 },                                       \
    { 18, -50, 75, -89, 0, 0, 0, 0 },                                      \
}

static const int16_t dct_o32_s16[16][16] = DCT_O32_ROWS;
static const int16_t dct_o16_s16[8][8] = DCT_O16_ROWS;
static const int16_t dct_o8_s16[4][8] = DCT_O8_ROWS;
static const int32_t dct_o32_s32[16][16] = DCT_O32_ROWS;
static const int32_t dct_o16_s32[8][8] = DCT_O16_ROWS;
static const int32_t dct_o8_s32[4][8] = DCT_O8_ROWS;

#define AI_ static inline __attribute__((always_inline))

/* clip3(-32768, 32767, (v + half) >> sh), the shift a constant once the
 * caller is inlined: sqrshrn takes it as an immediate */
#define NRW_C_(s) case s: return vqrshrn_n_s32(v, s);
AI_ int16x4_t nrw4(int32x4_t v, const int sh)
{
    switch (sh) {
    NRW_C_(1) NRW_C_(2) NRW_C_(3) NRW_C_(4) NRW_C_(5) NRW_C_(6)
    NRW_C_(7) NRW_C_(8) NRW_C_(9) NRW_C_(10) NRW_C_(11) NRW_C_(12)
    NRW_C_(13) NRW_C_(14) NRW_C_(15)
    default: return vqrshrn_n_s32(v, 16);
    }
}
#undef NRW_C_

AI_ int16x8_t nrw8(int32x4_t lo, int32x4_t hi, const int sh)
{
    return vcombine_s16(nrw4(lo, sh), nrw4(hi, sh));
}

/* sum_t row[t] * v[t] over t < K, eight lanes as two int32 halves. K is
 * a constant at every call, so the guards fold. */
#define DL_(t, cv, ln) if ((t) < K) {                                      \
        l = vmlal_laneq_s16(l, vget_low_s16(v[t]), cv, ln);                \
        h = vmlal_high_laneq_s16(h, v[t], cv, ln);                         \
    }
AI_ void dot_s16(int32x4_t *lo, int32x4_t *hi, const int16x8_t *v,
                 const int16_t *row, const int K)
{
    int16x8_t c0 = vld1q_s16(row);
    int32x4_t l = vmull_laneq_s16(vget_low_s16(v[0]), c0, 0);
    int32x4_t h = vmull_high_laneq_s16(v[0], c0, 0);

    DL_(1, c0, 1) DL_(2, c0, 2) DL_(3, c0, 3)
    DL_(4, c0, 4) DL_(5, c0, 5) DL_(6, c0, 6) DL_(7, c0, 7)
    if (K > 8) {
        int16x8_t c1 = vld1q_s16(row + 8);

        DL_(8, c1, 0) DL_(9, c1, 1) DL_(10, c1, 2) DL_(11, c1, 3)
        DL_(12, c1, 4) DL_(13, c1, 5) DL_(14, c1, 6) DL_(15, c1, 7)
    }
    *lo = l;
    *hi = h;
}
#undef DL_

/* the same over four int32 lanes and an int32 row */
#define DM_(t, cv, ln) if ((t) < K) a = vmlaq_laneq_s32(a, v[t], cv, ln);
AI_ int32x4_t dot_s32(const int32x4_t *v, const int32_t *row, const int K)
{
    int32x4_t c0 = vld1q_s32(row);
    int32x4_t a = vmulq_laneq_s32(v[0], c0, 0);

    DM_(1, c0, 1) DM_(2, c0, 2) DM_(3, c0, 3)
    if (K > 4) {
        int32x4_t c1 = vld1q_s32(row + 4);

        DM_(4, c1, 0) DM_(5, c1, 1) DM_(6, c1, 2) DM_(7, c1, 3)
    }
    if (K > 8) {
        int32x4_t c2 = vld1q_s32(row + 8), c3 = vld1q_s32(row + 12);

        DM_(8, c2, 0) DM_(9, c2, 1) DM_(10, c2, 2) DM_(11, c2, 3)
        DM_(12, c3, 0) DM_(13, c3, 1) DM_(14, c3, 2) DM_(15, c3, 3)
    }
    return a;
}
#undef DM_

/* the 2-point odd pair (83, 36 / 36, -83) and the 64-row pair, int16 in */
AI_ void pair_s16(int16x8_t *oa, int16x8_t *ob, int16x8_t a, int16x8_t b,
                  const int sh)
{
    *oa = nrw8(vmlal_n_s16(vmull_n_s16(vget_low_s16(a), 83),
                           vget_low_s16(b), 36),
               vmlal_high_n_s16(vmull_high_n_s16(a, 83), b, 36), sh);
    *ob = nrw8(vmlsl_n_s16(vmull_n_s16(vget_low_s16(a), 36),
                           vget_low_s16(b), 83),
               vmlsl_high_n_s16(vmull_high_n_s16(a, 36), b, 83), sh);
}

AI_ int16x8_t dc_s16(int16x8_t s, const int sh)
{
    return nrw8(vshll_n_s16(vget_low_s16(s), 6), vshll_high_n_s16(s, 6),
                sh);
}

/*
 * Forward stage 1: one N-point DCT (N = 16 or 32) on eight columns, int16
 * in and out.
 *
 * RANGE. The input is a residual, |r| <= PIXEL_MAX. Each butterfly level
 * at most doubles it: at 32 points the deepest sum is 32 * PIXEL_MAX =
 * 8160 at 8 bits and 32736 at Main10, int16 at both. A pre-shift output
 * is at most (sum of the row's |coefficients|) times the largest input
 * of its level: 64 * 8160 = 522240 for the 64 rows, 942 * 510 for the
 * 32-point odd rows, 461 * 1020, 232 * 2040 and 119 * 4080 below them,
 * all under 2^19, and shift1 = 4 (8 bits) leaves at most 32640 - int16
 * again, which the C's int32 `b` holds unclipped. 16 points: 64 * 4080,
 * 461 * 510, 232 * 1020, 119 * 2040, under 2^18, against shift1 = 3.
 * Main10 multiplies each input by four and adds two to the shift.
 */
AI_ void fwd1d_s16(int16x8_t *out, const int16x8_t *in, const int N,
                   const int sh)
{
    const int H = N / 2, Q = N / 4;
    int16x8_t e[16], o[16], ee[8], eo[8], e3[4], o3[4];
    int32x4_t l, h;

    for (int k = 0; k < H; k++) {
        e[k] = vaddq_s16(in[k], in[N - 1 - k]);
        o[k] = vsubq_s16(in[k], in[N - 1 - k]);
    }
    for (int i = 0; i < H; i++) {
        dot_s16(&l, &h, o, N == 32 ? dct_o32_s16[i] : dct_o16_s16[i], H);
        out[2 * i + 1] = nrw8(l, h, sh);
    }
    for (int k = 0; k < Q; k++) {
        ee[k] = vaddq_s16(e[k], e[H - 1 - k]);
        eo[k] = vsubq_s16(e[k], e[H - 1 - k]);
    }
    for (int i = 0; i < Q; i++) {
        dot_s16(&l, &h, eo, N == 32 ? dct_o16_s16[i] : dct_o8_s16[i], Q);
        out[4 * i + 2] = nrw8(l, h, sh);
    }
    if (N == 32) {
        int16x8_t a, b;

        for (int k = 0; k < 4; k++) {
            e3[k] = vaddq_s16(ee[k], ee[7 - k]);
            o3[k] = vsubq_s16(ee[k], ee[7 - k]);
        }
        for (int i = 0; i < 4; i++) {
            dot_s16(&l, &h, o3, dct_o8_s16[i], 4);
            out[8 * i + 4] = nrw8(l, h, sh);
        }
        a = vaddq_s16(e3[0], e3[3]);
        b = vaddq_s16(e3[1], e3[2]);
        pair_s16(&out[8], &out[24], vsubq_s16(e3[0], e3[3]),
                 vsubq_s16(e3[1], e3[2]), sh);
        out[0] = dc_s16(vaddq_s16(a, b), sh);
        out[16] = dc_s16(vsubq_s16(a, b), sh);
    } else {
        int16x8_t a = vaddq_s16(ee[0], ee[3]), b = vaddq_s16(ee[1], ee[2]);

        pair_s16(&out[4], &out[12], vsubq_s16(ee[0], ee[3]),
                 vsubq_s16(ee[1], ee[2]), sh);
        out[0] = dc_s16(vaddq_s16(a, b), sh);
        out[8] = dc_s16(vsubq_s16(a, b), sh);
    }
}

/*
 * Forward stage 2: one N-point DCT on four rows (the low or high half of
 * eight int16 lanes), int32 from the first butterfly on.
 *
 * RANGE. The inputs are stage 1's, |b| <= 32640, so the first level's
 * sums reach 65280 and leave int16 - which is why this stage widens -
 * and the largest pre-shift output is 64 * 32 * 32640 = 66.8M at 32
 * points, inside int32. The narrow is the C's clip to int16.
 */
AI_ void fwd1d_s32(int16x4_t *out, const int16x8_t *in, const int N,
                   const int hi, const int sh)
{
    const int H = N / 2, Q = N / 4;
    int32x4_t e[16], o[16], ee[8], eo[8], e3[4], o3[4];

    for (int k = 0; k < H; k++) {
        if (hi) {
            e[k] = vaddl_high_s16(in[k], in[N - 1 - k]);
            o[k] = vsubl_high_s16(in[k], in[N - 1 - k]);
        } else {
            e[k] = vaddl_s16(vget_low_s16(in[k]),
                             vget_low_s16(in[N - 1 - k]));
            o[k] = vsubl_s16(vget_low_s16(in[k]),
                             vget_low_s16(in[N - 1 - k]));
        }
    }
    for (int i = 0; i < H; i++)
        out[2 * i + 1] = nrw4(dot_s32(o, N == 32 ? dct_o32_s32[i]
                                                 : dct_o16_s32[i], H), sh);
    for (int k = 0; k < Q; k++) {
        ee[k] = vaddq_s32(e[k], e[H - 1 - k]);
        eo[k] = vsubq_s32(e[k], e[H - 1 - k]);
    }
    for (int i = 0; i < Q; i++)
        out[4 * i + 2] = nrw4(dot_s32(eo, N == 32 ? dct_o16_s32[i]
                                                  : dct_o8_s32[i], Q), sh);
    {
        const int32x4_t *se = ee;
        int32x4_t a, b, p, q;
        int i8 = N == 32 ? 8 : 4;       /* where the 2-point pair lands */

        if (N == 32) {
            for (int k = 0; k < 4; k++) {
                e3[k] = vaddq_s32(ee[k], ee[7 - k]);
                o3[k] = vsubq_s32(ee[k], ee[7 - k]);
            }
            for (int i = 0; i < 4; i++)
                out[8 * i + 4] = nrw4(dot_s32(o3, dct_o8_s32[i], 4), sh);
            se = e3;
        }
        a = vaddq_s32(se[0], se[3]);
        b = vaddq_s32(se[1], se[2]);
        p = vsubq_s32(se[0], se[3]);
        q = vsubq_s32(se[1], se[2]);
        out[i8] = nrw4(vmlaq_n_s32(vmulq_n_s32(p, 83), q, 36), sh);
        out[3 * i8] = nrw4(vmlsq_n_s32(vmulq_n_s32(p, 36), q, 83), sh);
        out[0] = nrw4(vshlq_n_s32(vaddq_s32(a, b), 6), sh);
        out[2 * i8] = nrw4(vshlq_n_s32(vsubq_s32(a, b), 6), sh);
    }
}

AI_ void fdct_big(int16_t *coef, const int16_t *resid, const int N)
{
    const int lg = N == 32 ? 5 : 4;
    const int sh1 = lg + BIT_DEPTH - 9, sh2 = lg + 6;
    int16_t tmp[32 * 32];       /* tmp[x * N + k] is the C's b[k][x] */

    for (int x0 = 0; x0 < N; x0 += 8) {
        int16x8_t in[32], out[32];

        for (int y = 0; y < N; y++)
            in[y] = vld1q_s16(resid + y * N + x0);
        fwd1d_s16(out, in, N, sh1);
        for (int k0 = 0; k0 < N; k0 += 8) {
            int16x8_t t[8];

            for (int i = 0; i < 8; i++)
                t[i] = out[k0 + i];
            tr8_s16(t);
            for (int j = 0; j < 8; j++)
                vst1q_s16(tmp + (x0 + j) * N + k0, t[j]);
        }
    }
    for (int r0 = 0; r0 < N; r0 += 8) {
        int16x8_t in[32];
        int16x4_t lo[32], hi[32];

        for (int x = 0; x < N; x++)
            in[x] = vld1q_s16(tmp + x * N + r0);    /* lanes = rows */
        fwd1d_s32(lo, in, N, 0, sh2);
        fwd1d_s32(hi, in, N, 1, sh2);
        for (int j0 = 0; j0 < N; j0 += 8) {
            int16x8_t t[8];

            for (int i = 0; i < 8; i++)
                t[i] = vcombine_s16(lo[j0 + i], hi[j0 + i]);
            tr8_s16(t);
            for (int r = 0; r < 8; r++)
                vst1q_s16(coef + (r0 + r) * N + j0, t[r]);
        }
    }
}

static __attribute__((noinline))
void fdct16_s16(int16_t *coef, const int16_t *resid)
{
    fdct_big(coef, resid, 16);
}

static __attribute__((noinline))
void fdct32_s16(int16_t *coef, const int16_t *resid)
{
    fdct_big(coef, resid, 32);
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
    case 2:  fdct4_s16(coef, resid); break;
    case 3:  fdct8_s16(coef, resid); break;
    case 4:  fdct16_s16(coef, resid); break;
    default: fdct32_s16(coef, resid); break;
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
static __attribute__((noinline))
void inv_dct_1d_x4(int32x4_t *dst, const int32x4_t *src, int n,
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

/* ---- the inverse DCT at 4x4 and 8x8 in int16 lanes (item 39) --------
 *
 * The forward's layout run backwards: dq rows in int16 lanes, widening
 * multiplies into int32, a saturating rounding narrow after each stage.
 *
 * RANGE. Stage 1's narrow is the C's clip to int16 after the shift of 7,
 * the instruction's own saturation. Stage 2's inputs are that clipped
 * int16, and an 8-point output is at most 479 (the largest column sum of
 * |T|) times 32768, 15.7M; the shift of 20 - BitDepth leaves 3833 at 8
 * bits and 15330 at Main10. So stage 2's narrow never saturates, and the
 * int16 value plus a sample stays inside int16 for the clip that
 * follows. The 4-point column sum is 247, smaller still. */

#define IDCT8_HALF(y, d, MULL, MLAL, MLSL, SHLL) do {                      \
        int32x4_t d0s_ = SHLL(d[0]);                                       \
        int32x4_t ee0_ = MLAL(d0s_, d[4], 64), ee1_ = MLSL(d0s_, d[4], 64); \
        int32x4_t eo0_ = MLAL(MULL(d[2], 83), d[6], 36);                   \
        int32x4_t eo1_ = MLSL(MULL(d[2], 36), d[6], 83);                   \
        int32x4_t e_[4], o_[4];                                            \
        e_[0] = vaddq_s32(ee0_, eo0_); e_[3] = vsubq_s32(ee0_, eo0_);      \
        e_[1] = vaddq_s32(ee1_, eo1_); e_[2] = vsubq_s32(ee1_, eo1_);      \
        o_[0] = MLAL(MLAL(MLAL(MULL(d[1], 89), d[3], 75), d[5], 50),       \
                     d[7], 18);                                            \
        o_[1] = MLSL(MLSL(MLSL(MULL(d[1], 75), d[3], 18), d[5], 89),       \
                     d[7], 50);                                            \
        o_[2] = MLAL(MLAL(MLSL(MULL(d[1], 50), d[3], 89), d[5], 18),       \
                     d[7], 75);                                            \
        o_[3] = MLSL(MLAL(MLSL(MULL(d[1], 18), d[3], 50), d[5], 75),       \
                     d[7], 89);                                            \
        for (int j_ = 0; j_ < 4; j_++) {                                   \
            (y)[j_] = vaddq_s32(e_[j_], o_[j_]);                           \
            (y)[7 - j_] = vsubq_s32(e_[j_], o_[j_]);                       \
        }                                                                  \
    } while (0)

/* one 8-point inverse on eight int16 vectors, both halves, narrowed by
 * SH with saturation */
#define IDCT8_S16(out, d, SH) do {                                         \
        int32x4_t lo_[8], hi_[8];                                          \
        IDCT8_HALF(lo_, d, MULL_LO, MLAL_LO, MLSL_LO, SHLL_LO);            \
        IDCT8_HALF(hi_, d, MULL_HI, MLAL_HI, MLSL_HI, SHLL_HI);            \
        for (int k_ = 0; k_ < 8; k_++)                                     \
            (out)[k_] = vcombine_s16(vqrshrn_n_s32(lo_[k_], SH),           \
                                     vqrshrn_n_s32(hi_[k_], SH));          \
    } while (0)

/* eight int16 residuals added to eight samples, clipped to a sample. The
 * add saturates, so a residual the narrow before it saturated (the 32-
 * point inverse at Main10 can, on adversarial input) still lands on the
 * clip the C's int32 sum would: past +32767 the sample is PIXEL_MAX, past
 * -32768 it is 0, whatever the prediction. */
static inline void add_row8(pixel *d, const pixel *p, int16x8_t r)
{
#if BIT_DEPTH == 8
    int16x8_t s = vqaddq_s16(vreinterpretq_s16_u16(vmovl_u8(vld1_u8(p))), r);

    vst1_u8(d, vqmovun_s16(s));
#else
    int16x8_t s = vqaddq_s16(vreinterpretq_s16_u16(vld1q_u16(p)), r);

    s = vminq_s16(vmaxq_s16(s, vdupq_n_s16(0)), vdupq_n_s16(PIXEL_MAX));
    vst1q_u16(d, vreinterpretq_u16_s16(s));
#endif
}

/* the same for two 4-sample rows held in one vector */
static inline void add_rows4x2(pixel *d0, pixel *d1, const pixel *p0,
                               const pixel *p1, int16x8_t r)
{
#if BIT_DEPTH == 8
    uint32_t w0, w1;
    uint8x8_t pv;
    uint8x8_t o;

    memcpy(&w0, p0, 4);
    memcpy(&w1, p1, 4);
    pv = vreinterpret_u8_u64(vcreate_u64((uint64_t)w0 | ((uint64_t)w1 << 32)));
    o = vqmovun_s16(vaddq_s16(vreinterpretq_s16_u16(vmovl_u8(pv)), r));
    w0 = vget_lane_u32(vreinterpret_u32_u8(o), 0);
    w1 = vget_lane_u32(vreinterpret_u32_u8(o), 1);
    memcpy(d0, &w0, 4);
    memcpy(d1, &w1, 4);
#else
    int16x8_t s = vaddq_s16(
        vreinterpretq_s16_u16(vcombine_u16(vld1_u16(p0), vld1_u16(p1))), r);

    s = vminq_s16(vmaxq_s16(s, vdupq_n_s16(0)), vdupq_n_s16(PIXEL_MAX));
    vst1_u16(d0, vreinterpret_u16_s16(vget_low_s16(s)));
    vst1_u16(d1, vreinterpret_u16_s16(vget_high_s16(s)));
#endif
}

#define ISH2 (20 - BIT_DEPTH)

/* 4-point inverse on four int16x4 vectors, narrowed by SH */
#define IDCT4_S16(y, d, SH) do {                                           \
        int32x4_t d0s_ = vshll_n_s16(d[0], 6);                             \
        int32x4_t e0_ = vmlal_n_s16(d0s_, d[2], 64);                       \
        int32x4_t e1_ = vmlsl_n_s16(d0s_, d[2], 64);                       \
        int32x4_t o0_ = vmlal_n_s16(vmull_n_s16(d[1], 83), d[3], 36);      \
        int32x4_t o1_ = vmlsl_n_s16(vmull_n_s16(d[1], 36), d[3], 83);      \
        (y)[0] = vqrshrn_n_s32(vaddq_s32(e0_, o0_), SH);                   \
        (y)[1] = vqrshrn_n_s32(vaddq_s32(e1_, o1_), SH);                   \
        (y)[2] = vqrshrn_n_s32(vsubq_s32(e1_, o1_), SH);                   \
        (y)[3] = vqrshrn_n_s32(vsubq_s32(e0_, o0_), SH);                   \
    } while (0)

static __attribute__((noinline))
void idct4_s16(pixel *dst, int ds, const int16_t *dq, const pixel *pred,
               int ps)
{
    int16x4_t d[4], y[4];

    for (int k = 0; k < 4; k++)
        d[k] = vld1_s16(dq + 4 * k);
    IDCT4_S16(y, d, 7);                 /* y[j] lane x: the C's b[x][j] */
    tr4_s16(y);                         /* y[x], lanes = rows */
    IDCT4_S16(d, y, ISH2);              /* d[x'] lane y: residual column */
    tr4_s16(d);                         /* residual rows */
    add_rows4x2(dst, dst + ds, pred, pred + ps, vcombine_s16(d[0], d[1]));
    add_rows4x2(dst + 2 * ds, dst + 3 * ds, pred + 2 * ps, pred + 3 * ps,
                vcombine_s16(d[2], d[3]));
}

static __attribute__((noinline))
void idct8_s16(pixel *dst, int ds, const int16_t *dq, const pixel *pred,
               int ps)
{
    int16x8_t d[8], y[8];

    for (int k = 0; k < 8; k++)
        d[k] = vld1q_s16(dq + 8 * k);
    IDCT8_S16(y, d, 7);
    tr8_s16(y);
    IDCT8_S16(d, y, ISH2);
    tr8_s16(d);
    for (int r = 0; r < 8; r++)
        add_row8(dst + (ptrdiff_t)r * ds, pred + (ptrdiff_t)r * ps, d[r]);
}

/*
 * The inverse at 16 and 32 points, eight transforms per vector: inv_dct_1d
 * from transform.c with its levels written out. The even half is built
 * first (64-row pair, 83/36 pair, then each odd level's dot products on
 * the coefficients that level reads), then each odd sum O_j is formed and
 * spent at once on out[j] = E_j + O_j and out[N-1-j] = E_j - O_j.
 *
 * `kb` bounds the input: in[k] is zero for k >= kb (8, 16 or 32, a
 * constant at each call), so the two big dot products stop at the last
 * odd index under it. Every index at or past kb is still read as the zero
 * it is by the cheap levels.
 *
 * RANGE. The input is int16 (stage 1: dequantised, clipped; stage 2:
 * stage 1's output, clipped). A sum is at most the column's |T| total
 * times 32768, under 2^26 at 32 points, inside int32 at every level. The
 * narrow is the C's rounding shift with clip3(-32768, 32767): stage 1's
 * clip exactly, and at stage 2 a saturation the clip to a sample after
 * it absorbs (add_row8).
 */
AI_ void inv1d_s16(int16x8_t *out, const int16x8_t *in, const int N,
                   const int sh, const int kb)
{
    const int H = N / 2, Q = N / 4;
    int16x8_t od[16], ev[8], e4[4];
    int32x4_t el[16], eh[16], l, h, l2, h2;

    /* the 4-point even core on the inputs at multiples of N/4 */
    {
        const int s = N / 4;
        int16x8_t a0 = in[0], a1 = in[s], a2 = in[2 * s], a3 = in[3 * s];
        int32x4_t pl = vshll_n_s16(vget_low_s16(a0), 6);
        int32x4_t ph = vshll_high_n_s16(a0, 6);
        int32x4_t e0l = vmlal_n_s16(pl, vget_low_s16(a2), 64);
        int32x4_t e0h = vmlal_high_n_s16(ph, a2, 64);
        int32x4_t e1l = vmlsl_n_s16(pl, vget_low_s16(a2), 64);
        int32x4_t e1h = vmlsl_high_n_s16(ph, a2, 64);
        int32x4_t o0l = vmlal_n_s16(vmull_n_s16(vget_low_s16(a1), 83),
                                    vget_low_s16(a3), 36);
        int32x4_t o0h = vmlal_high_n_s16(vmull_high_n_s16(a1, 83), a3, 36);
        int32x4_t o1l = vmlsl_n_s16(vmull_n_s16(vget_low_s16(a1), 36),
                                    vget_low_s16(a3), 83);
        int32x4_t o1h = vmlsl_high_n_s16(vmull_high_n_s16(a1, 36), a3, 83);

        el[0] = vaddq_s32(e0l, o0l); eh[0] = vaddq_s32(e0h, o0h);
        el[3] = vsubq_s32(e0l, o0l); eh[3] = vsubq_s32(e0h, o0h);
        el[1] = vaddq_s32(e1l, o1l); eh[1] = vaddq_s32(e1h, o1h);
        el[2] = vsubq_s32(e1l, o1l); eh[2] = vsubq_s32(e1h, o1h);
    }
    /* the 8-point level: odd inputs at (2i + 1) * N/8 */
    {
        const int s = N / 8;

        for (int i = 0; i < 4; i++)
            e4[i] = in[(2 * i + 1) * s];
        for (int j = 3; j >= 0; j--) {
            dot_s16(&l, &h, e4, dct_o8_s16[j], 4);
            el[7 - j] = vsubq_s32(el[j], l); eh[7 - j] = vsubq_s32(eh[j], h);
            el[j] = vaddq_s32(el[j], l);     eh[j] = vaddq_s32(eh[j], h);
        }
    }
    /* the 16-point level at 32 points: odd inputs at 4i + 2 */
    if (N == 32) {
        const int K = kb / 4;

        for (int i = 0; i < 8; i++)
            ev[i] = in[4 * i + 2];
        for (int j = 7; j >= 0; j--) {
            dot_s16(&l, &h, ev, dct_o16_s16[j], K);
            el[15 - j] = vsubq_s32(el[j], l); eh[15 - j] = vsubq_s32(eh[j], h);
            el[j] = vaddq_s32(el[j], l);      eh[j] = vaddq_s32(eh[j], h);
        }
    }
    /* the top level: odd inputs at 2i + 1, spent as they are formed */
    {
        const int K = kb / 2 < H ? kb / 2 : H;

        for (int i = 0; i < H; i++)
            od[i] = in[2 * i + 1];
        for (int j = 0; j < H; j++) {
            dot_s16(&l, &h, od, N == 32 ? dct_o32_s16[j] : dct_o16_s16[j],
                    K);
            l2 = vsubq_s32(el[j], l);
            h2 = vsubq_s32(eh[j], h);
            out[j] = nrw8(vaddq_s32(el[j], l), vaddq_s32(eh[j], h), sh);
            out[N - 1 - j] = nrw8(l2, h2, sh);
        }
    }
    (void)Q;
}

AI_ void idct_big(pixel *dst, int ds, const int16_t *dq, const pixel *pred,
                  int ps, const int N, int nzw, int nzh)
{
    int16_t tmp[32 * 32];       /* tmp[x * N + j] is the C's b[x][j] */
    int xg = (nzw + 7) & ~7;
    int16x8_t z = vdupq_n_s16(0);

    for (int x0 = 0; x0 < xg; x0 += 8) {
        int16x8_t in[32], out[32];

        for (int k = 0; k < N; k++)
            in[k] = k < nzh ? vld1q_s16(dq + k * N + x0) : z;
        if (nzh <= 8)
            inv1d_s16(out, in, N, 7, 8);
        else if (nzh <= 16)
            inv1d_s16(out, in, N, 7, 16);
        else
            inv1d_s16(out, in, N, 7, 32);
        for (int j0 = 0; j0 < N; j0 += 8) {
            int16x8_t t[8];

            for (int i = 0; i < 8; i++)
                t[i] = out[j0 + i];
            tr8_s16(t);         /* t[x] lane i = b[x0 + x][j0 + i] */
            for (int x = 0; x < 8; x++)
                vst1q_s16(tmp + (x0 + x) * N + j0, t[x]);
        }
    }
    for (int y0 = 0; y0 < N; y0 += 8) {
        int16x8_t in[32], out[32];

        for (int x = 0; x < N; x++)
            in[x] = x < xg ? vld1q_s16(tmp + x * N + y0) : z;
        if (xg <= 8)
            inv1d_s16(out, in, N, 20 - BIT_DEPTH, 8);
        else if (xg <= 16)
            inv1d_s16(out, in, N, 20 - BIT_DEPTH, 16);
        else
            inv1d_s16(out, in, N, 20 - BIT_DEPTH, 32);
        for (int x0 = 0; x0 < N; x0 += 8) {
            int16x8_t t[8];

            for (int i = 0; i < 8; i++)
                t[i] = out[x0 + i];     /* lane r = residual (y0 + r, x0 + i) */
            tr8_s16(t);
            for (int r = 0; r < 8; r++)
                add_row8(dst + (ptrdiff_t)(y0 + r) * ds + x0,
                         pred + (ptrdiff_t)(y0 + r) * ps + x0, t[r]);
        }
    }
}

static __attribute__((noinline))
void idct16_s16(pixel *dst, int ds, const int16_t *dq, const pixel *pred,
                int ps, int nzw, int nzh)
{
    idct_big(dst, ds, dq, pred, ps, 16, nzw, nzh);
}

static __attribute__((noinline))
void idct32_s16(pixel *dst, int ds, const int16_t *dq, const pixel *pred,
                int ps, int nzw, int nzh)
{
    idct_big(dst, ds, dq, pred, ps, 32, nzw, nzh);
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
        idct4_s16(dst, dst_stride, dq, pred, pred_stride);
        break;
    case 3:
        idct8_s16(dst, dst_stride, dq, pred, pred_stride);
        break;
    case 4:
        idct16_s16(dst, dst_stride, dq, pred, pred_stride, nzw, nzh);
        break;
    default:
        idct32_s16(dst, dst_stride, dq, pred, pred_stride, nzw, nzh);
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
/* Phase 14 item 39: the dequant on eight lanes, int16 in, for rows of
 * NV * 8 levels (NV a constant at each call, so every array below is
 * registers). Sh <= 9216 and Sl < 2^bd <= 1024 are both int16, so each
 * product is one widening multiply from the level as loaded; the sum is
 * the split described above, in the same int32 bound. A row whose levels
 * are all zero dequantises to zeros (the C's own skip), and after the
 * lattice that is most rows, so it is stored without the arithmetic.
 * The bounding box comes from an OR of the results: per row for nzh,
 * per column chunk for nzw. */
AI_ void deq_rows(int16_t *dq, const int16_t *level, const int NV,
                  int16_t sh16, int16_t sl16, int32x4_t addv, int32x4_t sh,
                  int *mx, int *my)
{
    static const uint16_t col_id[8] = { 1, 2, 3, 4, 5, 6, 7, 8 };
    const int n = NV * 8;
    int16x8_t z = vdupq_n_s16(0);
    uint16x8_t cacc[4];

    for (int k = 0; k < NV; k++)
        cacc[k] = vdupq_n_u16(0);
    for (int y = 0; y < n; y++) {
        const int16_t *lp = level + (ptrdiff_t)y * n;
        int16_t *dp = dq + (ptrdiff_t)y * n;
        int16x8_t l[4];
        uint16x8_t rowor = vdupq_n_u16(0);

        for (int k = 0; k < NV; k++) {
            l[k] = vld1q_s16(lp + 8 * k);
            rowor = vorrq_u16(rowor, vreinterpretq_u16_s16(l[k]));
        }
        if (!vmaxvq_u16(rowor)) {
            for (int k = 0; k < NV; k++)
                vst1q_s16(dp + 8 * k, z);
            continue;
        }
        rowor = vdupq_n_u16(0);
        for (int k = 0; k < NV; k++) {
            int32x4_t lo = vmlal_n_s16(addv, vget_low_s16(l[k]), sl16);
            int32x4_t hi = vmlal_high_n_s16(addv, l[k], sl16);
            int16x8_t v;

            lo = vmlal_n_s16(vshlq_s32(lo, sh), vget_low_s16(l[k]), sh16);
            hi = vmlal_high_n_s16(vshlq_s32(hi, sh), l[k], sh16);
            /* vqmovn is the C's clip to int16, saturation included */
            v = vqmovn_high_s32(vqmovn_s32(lo), hi);
            vst1q_s16(dp + 8 * k, v);
            cacc[k] = vorrq_u16(cacc[k], vreinterpretq_u16_s16(v));
            rowor = vorrq_u16(rowor, vreinterpretq_u16_s16(v));
        }
        if (vmaxvq_u16(rowor))
            *my = y;
    }
    for (int k = NV - 1; k >= 0; k--) {
        int m = vmaxvq_u16(vandq_u16(vtstq_u16(cacc[k], cacc[k]),
                                     vld1q_u16(col_id)));
        if (m) {
            *mx = 8 * k + m - 1;
            break;
        }
    }
}

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

    if (n >= 8) {
        /* item 39's eight-lane form, one instance per row width */
        int16_t sh16 = (int16_t)s_hi, sl16 = (int16_t)s_lo;

        if (n == 8)
            deq_rows(dq, level, 1, sh16, sl16, addv, sh, &mx, &my);
        else if (n == 16)
            deq_rows(dq, level, 2, sh16, sl16, addv, sh, &mx, &my);
        else
            deq_rows(dq, level, 4, sh16, sl16, addv, sh, &mx, &my);
        if (nzw) *nzw = mx + 1;
        if (nzh) *nzh = my + 1;
        return;
    }

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
    int32_t add = (int32_t)(((int64_t)1 << qbits) / (intra == 2 ? 2 : intra ? 3 : 6));
    int q = quant_scale[qpq % 6];
    uint32x4_t addv = vdupq_n_u32((uint32_t)add);
    int32x4_t sh = vdupq_n_s32(-qbits);
    uint16x8_t cnt = vdupq_n_u16(0);

    /* Phase 14 item 39: eight lanes from the int16 load, no widen of the
     * input. vabsq_s16 wraps -32768 to itself, and READ AS UNSIGNED that
     * is 32768 - the C's |c| exactly - so the magnitude is a u16 for every
     * input. The widening unsigned multiply-accumulate forms |c| * q + add
     * <= 9.04e8, inside u32 and inside int32 too, so after the logical
     * shift the value read as signed is the same number, and vqmovn_s32's
     * saturation at 32767 IS the C's clamp. The sign goes back on as
     * (l ^ s) - s with s = c >> 15. The count is a u16 lane accumulator,
     * at most 128 per lane at 32x32. */
    for (int i = 0; i < nn; i += 8) {
        int16x8_t c = vld1q_s16(coef + i);
        uint16x8_t a = vreinterpretq_u16_s16(vabsq_s16(c));
        uint32x4_t pl = vshlq_u32(vmlal_n_u16(addv, vget_low_u16(a),
                                              (uint16_t)q), sh);
        uint32x4_t ph = vshlq_u32(vmlal_high_n_u16(addv, a, (uint16_t)q),
                                  sh);
        int16x8_t l = vqmovn_high_s32(vqmovn_s32(vreinterpretq_s32_u32(pl)),
                                      vreinterpretq_s32_u32(ph));
        int16x8_t s = vshrq_n_s16(c, 15);

        cnt = vsubq_u16(cnt, vtstq_s16(l, l));
        vst1q_s16(level + i, vsubq_s16(veorq_s16(l, s), s));
    }
    return (int)vaddlvq_u16(cnt);
}

#else
typedef int n265_transform_neon_unused_;  /* C wants a non-empty TU */
#endif /* N265_NEON_TRANSFORM */
