/*
 * intra_neon.c - NEON intra prediction and its reference gathers
 * (class N265_ASM_INTRA). See arm/neon.h for the dispatch convention and
 * src/encoder/intra.c for the C references these are held to.
 *
 * Two kernels, both of them per-row arithmetic:
 *
 *   n265_intra_pred_neon    the 35 modes. Genuinely vector work: every
 *                           mode is a per-row weighted pair of adjacent
 *                           reference samples, which is a load, a
 *                           multiply-accumulate and a rounding narrow.
 *   n265_filter_refs_neon   [1 2 1] along the boundary. Same shape, one
 *                           dimension.
 *
 * A third once lived here, n265_intra_refs_neon, which was not a vector
 * kernel at all: its win was that the availability of the reference
 * samples is a prefix, so a binary search replaces 4n+1 n265_zorder
 * calls. Being under a NEON class bit charged that to NEON and kept it
 * off every other target, so it moved into src/encoder/intra.c as the C
 * reference itself.
 *
 * PHASE 8 ITEM 3. Both kernels are per-row weighted sums of reference
 * SAMPLES, so both are written twice, once per sample width, and the
 * 16-bit forms are not a retype of the 8-bit ones: what a widening
 * multiply into u16 lanes does at 8 bits, a plain u16 multiply does at
 * Main10, because every accumulator here is bounded by a weight sum
 * times PIXEL_MAX and each of those bounds still fits u16 - by a lot in
 * the angular and filter cases and by 32 counts in planar's. The narrow
 * differs too: at 8 bits vrshrn/vshrn ARE the store, and at Main10 the
 * shift stays in u16 lanes and the store is a plain vst1q_u16. Each
 * bound is written out where it is used.
 *
 * Copyright (c) 2026, the yah265 authors. GPL-2.0-or-later; see LICENSE.
 */

#if N265_NEON_INTRA

#include <arm_neon.h>
#include <string.h>

#include "encoder/intra.h"
#include "encoder/arm/neon.h"

/* Table 8-4/8-5, the same two tables the C reference reads. */
static const int8_t ia_angle[35] = {
    0, 0, 32, 26, 21, 17, 13, 9, 5, 2, 0, -2, -5, -9, -13, -17, -21, -26,
    -32, -26, -21, -17, -13, -9, -5, -2, 0, 2, 5, 9, 13, 17, 21, 26, 32
};
static const int16_t ia_inv_angle[35] = {
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, -4096, -1638, -910, -630, -482, -390,
    -315, -256, -315, -390, -482, -630, -910, -1638, -4096, 0, 0, 0, 0, 0,
    0, 0, 0, 0
};

#if BIT_DEPTH == 8
/* Unaligned 4-byte load/store. memcpy is the portable spelling and clang
 * folds it to a single ldr/str; the destination of a 4x4 TB is a plain
 * uint8_t row at stride 4, so nothing here may assume alignment. */
static inline uint32_t ld32u(const uint8_t *p)
{
    uint32_t v;
    memcpy(&v, p, 4);
    return v;
}

static inline void st32u(uint8_t *p, uint32_t v)
{
    memcpy(p, &v, 4);
}

/* store the low `cnt` bytes of a d-register; cnt is a compile-time
 * constant at every call site, so the branch folds away */
static inline void st_n(uint8_t *d, uint8x8_t v, int cnt)
{
    if (cnt >= 8) {
        vst1_u8(d, v);
    } else {
        uint8_t t[8];
        vst1_u8(t, v);
        memcpy(d, t, (size_t)cnt);
    }
}
#else
/* store the low `cnt` lanes of a q-register of samples; cnt is a
 * compile-time constant at every call site, so the branch folds away */
static inline void st_n16(pixel *d, uint16x8_t v, int cnt)
{
    if (cnt >= 8) {
        vst1q_u16(d, v);
    } else {
        uint16_t t[8];
        vst1q_u16(t, v);
        memcpy(d, t, (size_t)cnt * sizeof(pixel));
    }
}
#endif

/* ---- angular (8.4.4.2.6) --------------------------------------------
 *
 * Per row y of the main direction the spec fixes an integer offset
 * idx = ((y+1)*angle) >> 5 and a weight fact = ((y+1)*angle) & 31, and
 * emits ((32-fact)*r[x+idx] + fact*r[x+idx+1] + 16) >> 5 across the row.
 * That is one unaligned pair of adjacent loads from the projected
 * reference and a widening multiply-accumulate.
 *
 * RANGE. The claim that matters is the SUM: the two weights are
 * (32-fact) and fact and they add to 32 exactly, so the accumulator is
 * bounded by 32 * PIXEL_MAX + 16 - 8176 at 8 bits, 32752 at Main10 - and
 * BOTH are under 2^16. So u16 lanes are exact at either depth, and only
 * the instructions differ: at 8 bits the multiply widens out of u8
 * (vmull_u8/vmlal_u8) and the narrow vrshrn_n_u16(acc, 5) is both the
 * rounding shift and the store; at Main10 the operands are already u16,
 * so the multiply is the plain vmulq_u16/vmlaq_u16 - exact because the
 * SUM fits, not merely each product - and vrshrq_n_u16(acc, 5) leaves
 * the result in the lane, at most PIXEL_MAX, to be stored as it is.
 *
 * fact == 0 is a straight copy of the reference run - (32*a + 16) >> 5 is
 * a for every a in 0..PIXEL_MAX - which is the whole of modes 2, 10, 18,
 * 26 and 34 and one row in several others.
 */

/* out[y][x] = interp(r[x + idx(y)], r[x + idx(y) + 1], fact(y)) for a
 * block of n rows by n columns, written at out_stride. `r` must be
 * readable from r[idx_min] through r[2n + 16]; see build_ref(). */
static void ang_rows(pixel *out, int out_stride, const pixel *r, int n,
                     int angle)
{
#if BIT_DEPTH == 8
    if (n == 4) {
        /* the whole 4x4 in one q-register: four 4-byte runs side by side,
         * four weights duplicated four ways. 15 instructions for the
         * block, against four passes of the general form. */
        uint32_t a4[4], b4[4];
        uint8_t wa[16], wb[16];
        uint8x16_t a8, b8;
        uint16x8_t lo, hi;
        uint8x16_t res;

        for (int y = 0; y < 4; y++) {
            int idx = ((y + 1) * angle) >> 5;
            int f = ((y + 1) * angle) & 31;
            a4[y] = ld32u(r + idx);
            b4[y] = ld32u(r + idx + 1);
            memset(wa + y * 4, 32 - f, 4);
            memset(wb + y * 4, f, 4);
        }
        a8 = vreinterpretq_u8_u32(vld1q_u32(a4));
        b8 = vreinterpretq_u8_u32(vld1q_u32(b4));
        lo = vmull_u8(vget_low_u8(a8), vld1_u8(wa));
        lo = vmlal_u8(lo, vget_low_u8(b8), vld1_u8(wb));
        hi = vmull_u8(vget_high_u8(a8), vld1_u8(wa + 8));
        hi = vmlal_u8(hi, vget_high_u8(b8), vld1_u8(wb + 8));
        res = vcombine_u8(vrshrn_n_u16(lo, 5), vrshrn_n_u16(hi, 5));
        {
            uint32x4_t r32 = vreinterpretq_u32_u8(res);
            st32u(out + 0 * out_stride, vgetq_lane_u32(r32, 0));
            st32u(out + 1 * out_stride, vgetq_lane_u32(r32, 1));
            st32u(out + 2 * out_stride, vgetq_lane_u32(r32, 2));
            st32u(out + 3 * out_stride, vgetq_lane_u32(r32, 3));
        }
        return;
    }

    for (int y = 0; y < n; y++) {
        int idx = ((y + 1) * angle) >> 5;
        int f = ((y + 1) * angle) & 31;
        const uint8_t *p = r + idx;
        uint8_t *o = out + (size_t)y * out_stride;

        if (f == 0) {
            /* n is 8, 16 or 32 here: vector copies, where a memcpy of a
             * run-time length was a libc call per row (item 39) */
            if (n == 8) {
                vst1_u8(o, vld1_u8(p));
            } else {
                for (int x = 0; x < n; x += 16)
                    vst1q_u8(o + x, vld1q_u8(p + x));
            }
            continue;
        }
        {
            uint8x8_t wa = vdup_n_u8((uint8_t)(32 - f));
            uint8x8_t wb = vdup_n_u8((uint8_t)f);
            if (n >= 16) {
                for (int x = 0; x < n; x += 16) {
                    uint8x16_t a = vld1q_u8(p + x);
                    uint8x16_t b = vld1q_u8(p + x + 1);
                    uint16x8_t lo = vmull_u8(vget_low_u8(a), wa);
                    uint16x8_t hi = vmull_u8(vget_high_u8(a), wa);
                    lo = vmlal_u8(lo, vget_low_u8(b), wb);
                    hi = vmlal_u8(hi, vget_high_u8(b), wb);
                    vst1q_u8(o + x,
                             vrshrn_high_n_u16(vrshrn_n_u16(lo, 5), hi, 5));
                }
                continue;
            }
            for (int x = 0; x < n; x += 8) {
                uint16x8_t acc = vmull_u8(vld1_u8(p + x), wa);
                acc = vmlal_u8(acc, vld1_u8(p + x + 1), wb);
                vst1_u8(o + x, vrshrn_n_u16(acc, 5));
            }
        }
    }
#else   /* Main10: the same rows, eight samples to a q-register */
    for (int y = 0; y < n; y++) {
        int idx = ((y + 1) * angle) >> 5;
        int f = ((y + 1) * angle) & 31;
        const pixel *p = r + idx;
        pixel *o = out + (size_t)y * out_stride;

        if (f == 0) {
            memcpy(o, p, (size_t)n * sizeof(pixel));
            continue;
        }
        {
            uint16x8_t wa = vdupq_n_u16((uint16_t)(32 - f));
            uint16x8_t wb = vdupq_n_u16((uint16_t)f);

            if (n == 4) {
                /* two rows would need two different weights, so the 4x4
                 * runs one row at a time and stores four of the eight
                 * lanes; the upper four read r[idx+4 .. idx+8], which
                 * build_ref defines (2n + 17 samples of slack). */
                uint16x8_t acc = vmulq_u16(vld1q_u16(p), wa);

                acc = vmlaq_u16(acc, vld1q_u16(p + 1), wb);
                st_n16(o, vrshrq_n_u16(acc, 5), 4);
                continue;
            }
            for (int x = 0; x < n; x += 8) {
                uint16x8_t acc = vmulq_u16(vld1q_u16(p + x), wa);

                acc = vmlaq_u16(acc, vld1q_u16(p + x + 1), wb);
                vst1q_u16(o + x, vrshrq_n_u16(acc, 5));
            }
        }
    }
#endif
}

#if BIT_DEPTH == 8
/* d[i][j] = s[j][i] for an 8x8 byte tile: three transpose stages at 8,
 * 16 and 32 bit granularity. */
static inline void trans8x8(uint8_t *d, int ds, const uint8_t *s, int ss)
{
    uint8x8x2_t a01 = vtrn_u8(vld1_u8(s + 0 * ss), vld1_u8(s + 1 * ss));
    uint8x8x2_t a23 = vtrn_u8(vld1_u8(s + 2 * ss), vld1_u8(s + 3 * ss));
    uint8x8x2_t a45 = vtrn_u8(vld1_u8(s + 4 * ss), vld1_u8(s + 5 * ss));
    uint8x8x2_t a67 = vtrn_u8(vld1_u8(s + 6 * ss), vld1_u8(s + 7 * ss));
    uint16x4x2_t b02 = vtrn_u16(vreinterpret_u16_u8(a01.val[0]),
                                vreinterpret_u16_u8(a23.val[0]));
    uint16x4x2_t b13 = vtrn_u16(vreinterpret_u16_u8(a01.val[1]),
                                vreinterpret_u16_u8(a23.val[1]));
    uint16x4x2_t b46 = vtrn_u16(vreinterpret_u16_u8(a45.val[0]),
                                vreinterpret_u16_u8(a67.val[0]));
    uint16x4x2_t b57 = vtrn_u16(vreinterpret_u16_u8(a45.val[1]),
                                vreinterpret_u16_u8(a67.val[1]));
    uint32x2x2_t c0 = vtrn_u32(vreinterpret_u32_u16(b02.val[0]),
                               vreinterpret_u32_u16(b46.val[0]));
    uint32x2x2_t c1 = vtrn_u32(vreinterpret_u32_u16(b13.val[0]),
                               vreinterpret_u32_u16(b57.val[0]));
    uint32x2x2_t c2 = vtrn_u32(vreinterpret_u32_u16(b02.val[1]),
                               vreinterpret_u32_u16(b46.val[1]));
    uint32x2x2_t c3 = vtrn_u32(vreinterpret_u32_u16(b13.val[1]),
                               vreinterpret_u32_u16(b57.val[1]));

    vst1_u8(d + 0 * ds, vreinterpret_u8_u32(c0.val[0]));
    vst1_u8(d + 1 * ds, vreinterpret_u8_u32(c1.val[0]));
    vst1_u8(d + 2 * ds, vreinterpret_u8_u32(c2.val[0]));
    vst1_u8(d + 3 * ds, vreinterpret_u8_u32(c3.val[0]));
    vst1_u8(d + 4 * ds, vreinterpret_u8_u32(c0.val[1]));
    vst1_u8(d + 5 * ds, vreinterpret_u8_u32(c1.val[1]));
    vst1_u8(d + 6 * ds, vreinterpret_u8_u32(c2.val[1]));
    vst1_u8(d + 7 * ds, vreinterpret_u8_u32(c3.val[1]));
}
#endif

#if BIT_DEPTH != 8
/* d[i][j] = s[j][i] for an 8x8 block of samples: the same three stages
 * one width up - pairs at 16, 32 and 64 bits. */
static inline void trans8x8_16(pixel *d, int ds, const pixel *s, int ss)
{
    uint16x8_t r[8], t[8];

    for (int i = 0; i < 8; i++)
        r[i] = vld1q_u16(s + (size_t)i * ss);
    for (int i = 0; i < 8; i += 2) {
        t[i] = vtrn1q_u16(r[i], r[i + 1]);
        t[i + 1] = vtrn2q_u16(r[i], r[i + 1]);
    }
    for (int i = 0; i < 8; i += 4) {
        r[i] = vreinterpretq_u16_u32(
            vtrn1q_u32(vreinterpretq_u32_u16(t[i]),
                       vreinterpretq_u32_u16(t[i + 2])));
        r[i + 2] = vreinterpretq_u16_u32(
            vtrn2q_u32(vreinterpretq_u32_u16(t[i]),
                       vreinterpretq_u32_u16(t[i + 2])));
        r[i + 1] = vreinterpretq_u16_u32(
            vtrn1q_u32(vreinterpretq_u32_u16(t[i + 1]),
                       vreinterpretq_u32_u16(t[i + 3])));
        r[i + 3] = vreinterpretq_u16_u32(
            vtrn2q_u32(vreinterpretq_u32_u16(t[i + 1]),
                       vreinterpretq_u32_u16(t[i + 3])));
    }
    for (int i = 0; i < 4; i++) {
        vst1q_u16(d + (size_t)i * ds, vreinterpretq_u16_u64(
            vtrn1q_u64(vreinterpretq_u64_u16(r[i]),
                       vreinterpretq_u64_u16(r[i + 4]))));
        vst1q_u16(d + (size_t)(i + 4) * ds, vreinterpretq_u16_u64(
            vtrn2q_u64(vreinterpretq_u64_u16(r[i]),
                       vreinterpretq_u64_u16(r[i + 4]))));
    }
}
#endif

/* transpose an n x n block of samples held at stride n into d at stride ds */
static void trans_n(pixel *d, int ds, const pixel *s, int n)
{
#if BIT_DEPTH == 8
    if (n == 4) {
        static const uint8_t idx[16] = {
            0, 4, 8, 12, 1, 5, 9, 13, 2, 6, 10, 14, 3, 7, 11, 15
        };
        uint8x16_t v = vqtbl1q_u8(vld1q_u8(s), vld1q_u8(idx));
        uint32x4_t r32 = vreinterpretq_u32_u8(v);
        st32u(d + 0 * ds, vgetq_lane_u32(r32, 0));
        st32u(d + 1 * ds, vgetq_lane_u32(r32, 1));
        st32u(d + 2 * ds, vgetq_lane_u32(r32, 2));
        st32u(d + 3 * ds, vgetq_lane_u32(r32, 3));
        return;
    }
    for (int by = 0; by < n; by += 8)
        for (int bx = 0; bx < n; bx += 8)
            trans8x8(d + (size_t)bx * ds + by, ds,
                     s + (size_t)by * n + bx, n);
#else
    if (n == 4) {
        uint16x4_t r0 = vld1_u16(s), r1 = vld1_u16(s + 4);
        uint16x4_t r2 = vld1_u16(s + 8), r3 = vld1_u16(s + 12);
        uint16x4_t a0 = vtrn1_u16(r0, r1), a1 = vtrn2_u16(r0, r1);
        uint16x4_t a2 = vtrn1_u16(r2, r3), a3 = vtrn2_u16(r2, r3);

        vst1_u16(d, vreinterpret_u16_u32(
            vtrn1_u32(vreinterpret_u32_u16(a0), vreinterpret_u32_u16(a2))));
        vst1_u16(d + ds, vreinterpret_u16_u32(
            vtrn1_u32(vreinterpret_u32_u16(a1), vreinterpret_u32_u16(a3))));
        vst1_u16(d + 2 * ds, vreinterpret_u16_u32(
            vtrn2_u32(vreinterpret_u32_u16(a0), vreinterpret_u32_u16(a2))));
        vst1_u16(d + 3 * ds, vreinterpret_u16_u32(
            vtrn2_u32(vreinterpret_u32_u16(a1), vreinterpret_u32_u16(a3))));
        return;
    }
    for (int by = 0; by < n; by += 8)
        for (int bx = 0; bx < n; bx += 8)
            trans8x8_16(d + (size_t)bx * ds + by, ds,
                        s + (size_t)by * n + bx, n);
#endif
}

/* Build the projected main reference r[-n-1 .. 2n-1] the row loop reads,
 * plus 17 bytes of defined slack at the top end. The slack exists because
 * the row loop loads eight (or four) samples at x = n - 8 and again at
 * x + 1; the only index it can reach past the reference proper is r[2n],
 * and 8.4.4.2.6 gives that sample weight zero at every row that reaches
 * it (idx == n needs (y+1)*angle >= 32n with y+1 <= n and angle <= 32, so
 * angle is exactly 32 and fact is 0), which is also why the C reference
 * can leave it at the zero its memset put there. Everything above 2n is
 * loaded and discarded. `ref` is a local array, so none of this is a read
 * outside a caller's buffer and the page guard has nothing to see.
 */
static void build_ref(pixel *r, const pixel *main, const pixel *side,
                      int corner, int n, int angle, int inv)
{
    r[-1] = (pixel)corner;
#if BIT_DEPTH == 8
    /* 2n is 8, 16, 32 or 64: vector copies rather than a libc call of a
     * run-time length on every angular prediction (item 39). Spelled out
     * per size: a loop of them is a copy idiom clang turns straight back
     * into the memcpy call. */
    switch (n) {
    case 4:
        vst1_u8(r, vld1_u8(main));
        break;
    case 8:
        vst1q_u8(r, vld1q_u8(main));
        break;
    case 16:
        vst1q_u8_x2(r, vld1q_u8_x2(main));
        break;
    default:
        vst1q_u8_x4(r, vld1q_u8_x4(main));
        break;
    }
#else
    memcpy(r, main, (size_t)(2 * n) * sizeof(pixel));
#endif
    memset(r + 2 * n, 0, 17 * sizeof(pixel));
    if (angle < 0) {
        /* spec ref[x] = p[-1][-1 + ((x*invAngle + 128) >> 8)]; our r[i] is
         * the spec's ref[i+1] */
        int last = (n * angle) >> 5;
        for (int i = -2; i >= last; i--)
            r[i] = side[(((i + 1) * inv + 128) >> 8) - 1];
    }
}

static void pred_angular_neon(pixel *dst, int stride, const pixel *refs,
                              int log2n, int mode, int c_idx)
{
    int n = 1 << log2n;
    const pixel *top = refs + 1;
    const pixel *left = refs + 2 * n + 1;
    int corner = refs[0];
    int angle = ia_angle[mode];
    int inv = ia_inv_angle[mode];
    /* r[-n-1 .. 2n+16]; ref[0] is r[-n-1] */
    pixel ref[4 * 32 + 2 + 32];
    pixel *r = ref + n + 1;

    if (mode >= 18) {                   /* main = top, rows written direct */
        build_ref(r, top, left, corner, n, angle, inv);
        ang_rows(dst, stride, r, n, angle);
        if (mode == 26 && c_idx == 0 && n < 32)
            for (int y = 0; y < n; y++) {
                int v = top[0] + ((left[y] - corner) >> 1);
                dst[(size_t)y * stride] =
                    (pixel)(v < 0 ? 0 : v > PIXEL_MAX ? PIXEL_MAX : v);
            }
    } else {                            /* main = left, the walk transposes */
        pixel tmp[32 * 32];

        build_ref(r, left, top, corner, n, angle, inv);
        ang_rows(tmp, n, r, n, angle);
        trans_n(dst, stride, tmp, n);
        if (mode == 10 && c_idx == 0 && n < 32)
            for (int x = 0; x < n; x++) {
                int v = left[0] + ((top[x] - corner) >> 1);
                dst[x] = (pixel)(v < 0 ? 0 : v > PIXEL_MAX ? PIXEL_MAX : v);
            }
    }
}

/* ---- planar (8.4.4.2.4) ---------------------------------------------
 *
 * dst[y][x] = ((n-1-x)*left[y] + (x+1)*tr + (n-1-y)*top[x] + (y+1)*bl + n)
 *             >> (log2n + 1)
 *
 * RANGE, and this is the TIGHTEST bound in the file. The four weights
 * sum to 2n, so the four products together are at most 2n * PIXEL_MAX;
 * with the +n rounding term the accumulator is bounded by
 * n * (2 * PIXEL_MAX + 1), which is 16352 at 8 bits and, at the largest
 * size at Main10, 32 * 2047 = 65504 - inside u16 by 32 counts. It holds
 * because n is at most 32 (a TB is capped there and the encoder never
 * predicts a 64x64 intra block), and it would not hold at 12 bits or at
 * n = 64, so the assert is written into the comment rather than left to
 * be rediscovered. Every term is non-negative, so u16 throughout is
 * exact, and the shift by log2n+1 lands at PIXEL_MAX exactly
 * (65504 >> 6 = 1023).
 *
 * The shift is an immediate, so the body is instantiated once per size;
 * that also constant-folds n and lets clang unroll the row.
 */
#if BIT_DEPTH == 8
#define PLANAR_LOAD_TOP(x)  vmovl_u8(vld1_u8(top + (x)))
#define PLANAR_STORE(d, acc, SH, CNT) \
    st_n((d), vshrn_n_u16((acc), SH), (CNT))
#else
#define PLANAR_LOAD_TOP(x)  vld1q_u16(top + (x))
#define PLANAR_STORE(d, acc, SH, CNT) \
    st_n16((d), vshrq_n_u16((acc), SH), (CNT))
#endif

#define PLANAR_BODY(NN, SH)                                                \
    do {                                                                   \
        for (int y = 0; y < (NN); y++) {                                   \
            uint16x8_t ly = vdupq_n_u16((uint16_t)left[y]);                \
            uint16x8_t dy = vdupq_n_u16((uint16_t)((y + 1) * bl));         \
            uint16_t ny = (uint16_t)((NN) - 1 - y);                        \
            for (int x = 0; x < (NN); x += 8) {                            \
                uint16x8_t t = PLANAR_LOAD_TOP(x);                         \
                uint16x8_t acc = vmlaq_u16(vld1q_u16(base + x),            \
                                           vld1q_u16(xw + x), ly);         \
                acc = vmlaq_n_u16(acc, t, ny);                             \
                acc = vaddq_u16(acc, dy);                                  \
                PLANAR_STORE(dst + (size_t)y * stride + x, acc, SH,        \
                             (NN) < 8 ? (NN) : 8);                         \
            }                                                              \
        }                                                                  \
    } while (0)

static void pred_planar_neon(pixel *dst, int stride, const pixel *refs,
                             int log2n)
{
    int n = 1 << log2n;
    const pixel *top = refs + 1;
    const pixel *left = refs + 2 * n + 1;
    int tr = top[n];            /* p[nTbS][-1] */
    int bl = left[n];           /* p[-1][nTbS] */
    uint16_t xw[32], base[32];

    /* (x+1)*tr + n, at most 32 * PIXEL_MAX + 32, and (n-1-x). The tail up
     * to eight is zeroed rather than left alone: at n = 4 the row is one
     * eight-lane pass and four of its lanes are computed and thrown away,
     * and a lane built out of an uninitialized table is a sanitizer
     * report waiting to happen even where the value cannot escape. */
    /* built eight lanes at a time (item 39): the scalar fill and the zero
     * tail compiled to a loop and a bzero call per prediction. At n = 4
     * lanes 4..7 hold n - 1 - x and (x + 1) * tr + n continued (wrapped
     * modulo 2^16 for xw), defined values in lanes no store keeps. */
    {
        static const uint16_t ramp[8] = { 0, 1, 2, 3, 4, 5, 6, 7 };
        uint16x8_t id = vld1q_u16(ramp);

        for (int x = 0; x < (n < 8 ? 8 : n); x += 8) {
            uint16x8_t xi = vaddq_u16(id, vdupq_n_u16((uint16_t)x));

            vst1q_u16(xw + x, vsubq_u16(vdupq_n_u16((uint16_t)(n - 1)), xi));
            vst1q_u16(base + x,
                      vmlaq_n_u16(vdupq_n_u16((uint16_t)n),
                                  vaddq_u16(xi, vdupq_n_u16(1)),
                                  (uint16_t)tr));
        }
    }
    /* At n = 4 the row is eight lanes wide and only four are stored. The
     * extra lanes read top[4..7] and xw/base[4..7], all of which exist:
     * top has 2n = 8 samples and the two tables are 32 long. */
    switch (log2n) {
    case 2: PLANAR_BODY(4, 3); break;
    case 3: PLANAR_BODY(8, 4); break;
    case 4: PLANAR_BODY(16, 5); break;
    default: PLANAR_BODY(32, 6); break;
    }
}

/* ---- DC (8.4.4.2.5) ------------------------------------------------- */

static void pred_dc_neon(pixel *dst, int stride, const pixel *refs,
                         int log2n, int filter_edges)
{
    int n = 1 << log2n;
    const pixel *top = refs + 1;
    const pixel *left = refs + 2 * n + 1;
    uint32_t sum = (uint32_t)n;
    int dc;

    if (n >= 8) {
        /* A lane takes one top and one left sample per pass, so it holds
         * at most (n / 8) * 2 * PIXEL_MAX: 2040 at 8 bits and 8184 at
         * Main10, both far inside u16, and the horizontal vaddlvq widens
         * before it sums. */
        uint16x8_t acc = vdupq_n_u16(0);
        for (int i = 0; i < n; i += 8)
#if BIT_DEPTH == 8
            acc = vaddq_u16(acc, vaddl_u8(vld1_u8(top + i),
                                          vld1_u8(left + i)));
#else
            acc = vaddq_u16(acc, vaddq_u16(vld1q_u16(top + i),
                                           vld1q_u16(left + i)));
#endif
        sum += vaddlvq_u16(acc);
    } else {
        for (int i = 0; i < n; i++)
            sum += (uint32_t)top[i] + left[i];
    }
    dc = (int)(sum >> (log2n + 1));

#if BIT_DEPTH == 8
    /* a splat stored per row: memset of a run-time length was a libc
     * call per row (item 39) */
    {
        uint8x16_t v = vdupq_n_u8((uint8_t)dc);

        for (int y = 0; y < n; y++) {
            uint8_t *d = dst + (size_t)y * stride;

            if (n == 4)
                st32u(d, vgetq_lane_u32(vreinterpretq_u32_u8(v), 0));
            else if (n == 8)
                vst1_u8(d, vget_low_u8(v));
            else
                for (int x = 0; x < n; x += 16)
                    vst1q_u8(d + x, v);
        }
    }
#else
    /* memset fills BYTES; a 10-bit DC is two of them and not necessarily
     * equal, so the fill is a store of a splat - and it is written as one
     * store per row wherever the width allows, because the C this is held
     * against is a row loop clang turns into exactly that. The first
     * draft stored eight lanes at a time and read 0.70x at 32x32 against
     * the C; st1 of four registers reads 1.6x. */
    {
        uint16x8_t v = vdupq_n_u16((uint16_t)dc);
        uint16x8x2_t v2 = { { v, v } };
        uint16x8x4_t v4 = { { v, v, v, v } };

        for (int y = 0; y < n; y++) {
            pixel *o = dst + (size_t)y * stride;

            switch (n) {
            case 4:  vst1_u16(o, vget_low_u16(v)); break;
            case 8:  vst1q_u16(o, v); break;
            case 16: vst1q_u16_x2(o, v2); break;
            default: vst1q_u16_x4(o, v4); break;
            }
        }
    }
#endif

    if (filter_edges && n < 32) {
        dst[0] = (pixel)((left[0] + 2 * dc + top[0] + 2) >> 2);
        for (int x = 1; x < n; x++)
            dst[x] = (pixel)((top[x] + 3 * dc + 2) >> 2);
        for (int y = 1; y < n; y++)
            dst[(size_t)y * stride] = (pixel)((left[y] + 3 * dc + 2) >> 2);
    }
}

void n265_intra_pred_neon(pixel *dst, int dst_stride, const pixel *refs,
                          int log2n, int mode, int c_idx)
{
    if (mode == INTRA_PLANAR)
        pred_planar_neon(dst, dst_stride, refs, log2n);
    else if (mode == INTRA_DC)
        pred_dc_neon(dst, dst_stride, refs, log2n, c_idx == 0);
    else
        pred_angular_neon(dst, dst_stride, refs, log2n, mode, c_idx);
}

/* ---- [1 2 1] reference smoothing (8.4.4.2.3) -------------------------
 *
 * RANGE: 4 * PIXEL_MAX + 2 - 1022 at 8 bits, 4094 at Main10 - so u16
 * lanes are exact at both and the rounding shift by 2 lands back inside
 * the sample. At 8 bits vrshrn_n_u16(t, 2) is that shift and the narrow
 * in one instruction; at Main10 it is vrshrq_n_u16 and the lane is
 * stored as it stands.
 *
 * The filter runs along the boundary, which our layout stores as two
 * runs: the top row at 1..2n-1 and the left column at 2n+2..4n-1, with
 * four samples at the seams (0, 2n, 2n+1, 4n) that the spec treats
 * specially. A chunk starting at i reads in[i-1 .. i+8], so the last
 * chunk of a run is placed to END on the run rather than to start eight
 * before its end: the overlap recomputes a few outputs to the same
 * values (in and out do not alias) and never reads past in[4n].
 */
#if BIT_DEPTH == 8
#define FILT_CHUNK(out, in, i) do {                                       \
        uint16x8_t t_ = vaddl_u8(vld1_u8((in) + (i) - 1),                 \
                                 vld1_u8((in) + (i) + 1));                \
        t_ = vmlal_u8(t_, vld1_u8((in) + (i)), vdup_n_u8(2));             \
        vst1_u8((out) + (i), vrshrn_n_u16(t_, 2));                        \
    } while (0)
#else
#define FILT_CHUNK(out, in, i) do {                                       \
        uint16x8_t t_ = vaddq_u16(vld1q_u16((in) + (i) - 1),              \
                                  vld1q_u16((in) + (i) + 1));             \
        t_ = vmlaq_n_u16(t_, vld1q_u16((in) + (i)), 2);                   \
        vst1q_u16((out) + (i), vrshrq_n_u16(t_, 2));                      \
    } while (0)
#endif

static void filt_run(pixel *out, const pixel *in, int first, int count)
{
    int i = first;

    if (count >= 8) {
        int end = first + count;

        for (; i + 8 <= end; i += 8)
            FILT_CHUNK(out, in, i);
        if (i < end) {
            i = end - 8;
            FILT_CHUNK(out, in, i);
            i = end;
        }
        return;
    }
    for (; i < first + count; i++)
        out[i] = (pixel)((in[i - 1] + 2 * in[i] + in[i + 1] + 2) >> 2);
}

void n265_filter_refs_neon(pixel *out, const pixel *in, int log2n, int strong)
{
    int n = 1 << log2n;

    if (strong && n == 32) {
        /* strong smoothing: three corners, a divide by 2n and a linear
         * ramp. Rare enough (it needs both boundaries near-linear) and
         * short enough that the C form is what runs. */
        int tl = in[0], tr = in[2 * n], bl = in[4 * n];
        int thr = 1 << (BIT_DEPTH - 5);     /* 8.4.4.2.3, and intra.c's */
        int dt = tl + tr - 2 * in[n];
        int dl = tl + bl - 2 * in[2 * n + n];

        if ((dt < 0 ? -dt : dt) < thr && (dl < 0 ? -dl : dl) < thr) {
            out[0] = (pixel)tl;
            for (int i = 1; i <= 2 * n; i++)
                out[i] = (pixel)(((2 * n - i) * tl + i * tr + n) / (2 * n));
            for (int i = 1; i <= 2 * n; i++)
                out[2 * n + i] =
                    (pixel)(((2 * n - i) * tl + i * bl + n) / (2 * n));
            return;
        }
    }
    out[0] = (pixel)((in[2 * n + 1] + 2 * in[0] + in[1] + 2) >> 2);
    filt_run(out, in, 1, 2 * n - 1);
    out[2 * n] = in[2 * n];
    out[2 * n + 1] = (pixel)((in[0] + 2 * in[2 * n + 1] +
                              in[2 * n + 2] + 2) >> 2);
    filt_run(out, in, 2 * n + 2, 2 * n - 2);
    out[4 * n] = in[4 * n];
}

#endif /* N265_NEON_INTRA */
