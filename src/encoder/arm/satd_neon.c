/*
 * satd_neon.c - NEON SATD/SAD kernels (class N265_ASM_SATD). One file per
 * kernel family; see src/encoder/arm/neon.h for the dispatch convention
 * every kernel here follows.
 * Copyright (c) 2026, the yah265 authors. GPL-2.0-or-later; see LICENSE.
 */

#if N265_NEON

#include <arm_neon.h>
#include <stddef.h>
#include <string.h>

#include "encoder/arm/neon.h"
#include "encoder/satd.h"

/* ---- range, and why the lane widths hold it ------------------------
 *
 * SATD works on d = a - b with a, b in [0, 255], so every input to the
 * transform is in [-255, 255]. An n-point Hadamard is a sum of n terms
 * each plus or minus one input, so it grows the magnitude by at most n
 * per pass:
 *
 *   8x8 tile: after the first pass |v| <= 8 * 255 = 2040, after the
 *             second |v| <= 8 * 2040 = 16320.
 *   4x4 tile: after the first pass |v| <= 1020, after the second 4080.
 *
 * 16320 < 32767, so both passes are exact in int16 and no lane ever
 * saturates or wraps. vabsq_s16 is exact for the same reason: the one
 * input whose absolute value it cannot represent is -32768, which is
 * outside the range above.
 *
 * The abs-sum widens: vpadalq_u16 folds pairs of u16 into u32 lanes, so
 * one 8x8 tile accumulates at most 64 * 16320 = 1044480 across four u32
 * lanes and the horizontal vaddvq_u32 of that is exact.
 *
 * vsubl_u8(a, b) computes a - b modulo 65536 and the result is
 * reinterpreted as s16. That is the C's int subtraction exactly: the true
 * value lies in [-255, 255], which s16 represents, and two's complement
 * is what both the lane and the C int hold.
 *
 * ---- why the pass ORDER may differ from the C ------------------------
 *
 * The C does the row (horizontal) pass first and the column pass second;
 * the kernels below do the column pass first, then transpose, then the
 * same butterfly again. A separable transform commutes - H(XH^T) is
 * (HX)H^T - so the 2D coefficient MATRIX is the same either way up to a
 * permutation of its rows and columns, and the C's own butterfly already
 * emits its outputs permuted (HAD8 is radix-2, so output index k carries
 * Hadamard row bit-reverse(k)). What a tile returns is the sum of the
 * absolute values of all n*n coefficients, and that is invariant under
 * any permutation of them. So the tile's value is not merely close to the
 * C's, it is the same integer - which is what checkasm proves on all-max,
 * all-min and alternating input as well as on random.
 */

/* the C's 8-point butterfly (satd.c HAD8), one int16x8_t per input */
#define HAD8_V(o0, o1, o2, o3, o4, o5, o6, o7,                          \
               i0, i1, i2, i3, i4, i5, i6, i7) do {                     \
        int16x8_t b0_ = vaddq_s16(i0, i1), b1_ = vsubq_s16(i0, i1);     \
        int16x8_t b2_ = vaddq_s16(i2, i3), b3_ = vsubq_s16(i2, i3);     \
        int16x8_t b4_ = vaddq_s16(i4, i5), b5_ = vsubq_s16(i4, i5);     \
        int16x8_t b6_ = vaddq_s16(i6, i7), b7_ = vsubq_s16(i6, i7);     \
        int16x8_t c0_ = vaddq_s16(b0_, b2_), c2_ = vsubq_s16(b0_, b2_); \
        int16x8_t c1_ = vaddq_s16(b1_, b3_), c3_ = vsubq_s16(b1_, b3_); \
        int16x8_t c4_ = vaddq_s16(b4_, b6_), c6_ = vsubq_s16(b4_, b6_); \
        int16x8_t c5_ = vaddq_s16(b5_, b7_), c7_ = vsubq_s16(b5_, b7_); \
        (o0) = vaddq_s16(c0_, c4_); (o4) = vsubq_s16(c0_, c4_);         \
        (o1) = vaddq_s16(c1_, c5_); (o5) = vsubq_s16(c1_, c5_);         \
        (o2) = vaddq_s16(c2_, c6_); (o6) = vsubq_s16(c2_, c6_);         \
        (o3) = vaddq_s16(c3_, c7_); (o7) = vsubq_s16(c3_, c7_);         \
    } while (0)

/* the C's 4-point butterfly (satd.c satd4), one int16x8_t per input; the
 * two halves of each vector are two independent 4x4 tiles */
#define HAD4_V(o0, o1, o2, o3, i0, i1, i2, i3) do {                     \
        int16x8_t s0_ = vaddq_s16(i0, i2), s1_ = vaddq_s16(i1, i3);     \
        int16x8_t t0_ = vsubq_s16(i0, i2), t1_ = vsubq_s16(i1, i3);     \
        (o0) = vaddq_s16(s0_, s1_); (o1) = vsubq_s16(s0_, s1_);         \
        (o2) = vaddq_s16(t0_, t1_); (o3) = vsubq_s16(t0_, t1_);         \
    } while (0)

#define S32_(v) vreinterpretq_s32_s16(v)
#define S64_(v) vreinterpretq_s64_s32(v)
#define F64_(v) vreinterpretq_s16_s64(v)
#define F32_(v) vreinterpretq_s16_s32(v)

/* one row of an 8x8 tile: the eight byte differences, sign-correct */
static inline int16x8_t satd_row8(const uint8_t *a, const uint8_t *b)
{
    return vreinterpretq_s16_u16(vsubl_u8(vld1_u8(a), vld1_u8(b)));
}

/* four bytes at p. memcpy rather than a uint32_t * cast: the pointer
 * carries no alignment guarantee and UBSan is part of the item gate. */
static inline uint32_t ld4u(const uint8_t *p)
{
    uint32_t v;
    memcpy(&v, p, 4);
    return v;
}

/* two 4-byte rows packed into one d register: lanes 0-3 from p0, lanes
 * 4-7 from p1. The two halves are the two 4x4 tiles the kernel below
 * transforms side by side, and no butterfly or shuffle in it crosses the
 * 64-bit boundary, so they stay independent. */
static inline uint8x8_t ld4x2(const uint8_t *p0, const uint8_t *p1)
{
    return vreinterpret_u8_u64(
        vcreate_u64((uint64_t)ld4u(p0) | ((uint64_t)ld4u(p1) << 32)));
}

/* four bytes at p in the low half, upper half zero */
static inline uint8x8_t ld4z(const uint8_t *p)
{
    return vreinterpret_u8_u64(vcreate_u64((uint64_t)ld4u(p)));
}

/* The 8x8 tile, reduced but NOT narrowed: the value stays in a d
 * register so that a caller tiling a 64x64 block can accumulate 64 of
 * these without one vector-to-GPR move per tile. The tile rounding is
 * still per tile - it happens here, on the tile's own sum. */
static inline uint64x1_t satd8_tile(const uint8_t *a, int as,
                                    const uint8_t *b, int bs)
{
    int16x8_t r0 = satd_row8(a + 0 * as, b + 0 * bs);
    int16x8_t r1 = satd_row8(a + 1 * as, b + 1 * bs);
    int16x8_t r2 = satd_row8(a + 2 * as, b + 2 * bs);
    int16x8_t r3 = satd_row8(a + 3 * as, b + 3 * bs);
    int16x8_t r4 = satd_row8(a + 4 * as, b + 4 * bs);
    int16x8_t r5 = satd_row8(a + 5 * as, b + 5 * bs);
    int16x8_t r6 = satd_row8(a + 6 * as, b + 6 * bs);
    int16x8_t r7 = satd_row8(a + 7 * as, b + 7 * bs);
    int16x8_t q0, q1, q2, q3, q4, q5, q6, q7;
    uint32x4_t acc;
    uint64x2_t p;
    uint64x1_t s;

    /* pass 1: down the columns, across the eight row vectors */
    HAD8_V(q0, q1, q2, q3, q4, q5, q6, q7, r0, r1, r2, r3, r4, r5, r6, r7);

    /* transpose 8x8 int16: pair on 16, then on 32, then on 64 */
    {
        int16x8_t t0 = vtrn1q_s16(q0, q1), t1 = vtrn2q_s16(q0, q1);
        int16x8_t t2 = vtrn1q_s16(q2, q3), t3 = vtrn2q_s16(q2, q3);
        int16x8_t t4 = vtrn1q_s16(q4, q5), t5 = vtrn2q_s16(q4, q5);
        int16x8_t t6 = vtrn1q_s16(q6, q7), t7 = vtrn2q_s16(q6, q7);
        int32x4_t u0 = vtrn1q_s32(S32_(t0), S32_(t2));
        int32x4_t u2 = vtrn2q_s32(S32_(t0), S32_(t2));
        int32x4_t u1 = vtrn1q_s32(S32_(t1), S32_(t3));
        int32x4_t u3 = vtrn2q_s32(S32_(t1), S32_(t3));
        int32x4_t u4 = vtrn1q_s32(S32_(t4), S32_(t6));
        int32x4_t u6 = vtrn2q_s32(S32_(t4), S32_(t6));
        int32x4_t u5 = vtrn1q_s32(S32_(t5), S32_(t7));
        int32x4_t u7 = vtrn2q_s32(S32_(t5), S32_(t7));
        r0 = F64_(vtrn1q_s64(S64_(u0), S64_(u4)));
        r4 = F64_(vtrn2q_s64(S64_(u0), S64_(u4)));
        r1 = F64_(vtrn1q_s64(S64_(u1), S64_(u5)));
        r5 = F64_(vtrn2q_s64(S64_(u1), S64_(u5)));
        r2 = F64_(vtrn1q_s64(S64_(u2), S64_(u6)));
        r6 = F64_(vtrn2q_s64(S64_(u2), S64_(u6)));
        r3 = F64_(vtrn1q_s64(S64_(u3), S64_(u7)));
        r7 = F64_(vtrn2q_s64(S64_(u3), S64_(u7)));
    }

    /* pass 2: the same butterfly, now down what were the rows */
    HAD8_V(q0, q1, q2, q3, q4, q5, q6, q7, r0, r1, r2, r3, r4, r5, r6, r7);

    /* Two abs-sums folded at a time. vabaq_s16 would be one instruction
     * fewer per pair but its accumulator is s16 and 64 * 16320 does not
     * fit there; the pairwise widen is what keeps the claim exact. */
    acc = vpadalq_u16(vpaddlq_u16(vreinterpretq_u16_s16(vabsq_s16(q0))),
                      vreinterpretq_u16_s16(vabsq_s16(q1)));
    acc = vpadalq_u16(acc, vreinterpretq_u16_s16(vabsq_s16(q2)));
    acc = vpadalq_u16(acc, vreinterpretq_u16_s16(vabsq_s16(q3)));
    acc = vpadalq_u16(acc, vreinterpretq_u16_s16(vabsq_s16(q4)));
    acc = vpadalq_u16(acc, vreinterpretq_u16_s16(vabsq_s16(q5)));
    acc = vpadalq_u16(acc, vreinterpretq_u16_s16(vabsq_s16(q6)));
    acc = vpadalq_u16(acc, vreinterpretq_u16_s16(vabsq_s16(q7)));
    p = vpaddlq_u32(acc);
    s = vadd_u64(vget_low_u64(p), vget_high_u64(p));
    return vshr_n_u64(vadd_u64(s, vdup_n_u64(2)), 2);   /* (sum + 2) >> 2 */
}

uint32_t n265_satd8_neon(const uint8_t *a, int as, const uint8_t *b, int bs)
{
    return (uint32_t)vget_lane_u64(satd8_tile(a, as, b, bs), 0);
}

/* ---- the 4x4 tile, two at a time -----------------------------------
 *
 * Lanes 0-3 of every register are one tile and lanes 4-7 another. No
 * butterfly and no shuffle below crosses the 64-bit boundary, so the two
 * are independent all the way to the reduction, where vpadalq_u16 and
 * vpaddlq_u32 keep them apart for free: u32 lanes 0-1 come from u16 lanes
 * 0-3 and u32 lanes 2-3 from u16 lanes 4-7, so the final u64 pair is
 * exactly the two tile sums. That matters because the C rounds PER TILE -
 * (sum + 1) >> 1 on each 4x4 - and a kernel that summed the pair first
 * and rounded once would be off by one on half the blocks.
 */
static inline uint64x2_t satd4_core(int16x8_t r0, int16x8_t r1,
                                    int16x8_t r2, int16x8_t r3)
{
    int16x8_t q0, q1, q2, q3;
    uint32x4_t acc;

    HAD4_V(q0, q1, q2, q3, r0, r1, r2, r3);
    {
        int16x8_t t0 = vtrn1q_s16(q0, q1), t1 = vtrn2q_s16(q0, q1);
        int16x8_t t2 = vtrn1q_s16(q2, q3), t3 = vtrn2q_s16(q2, q3);
        r0 = F32_(vtrn1q_s32(S32_(t0), S32_(t2)));
        r2 = F32_(vtrn2q_s32(S32_(t0), S32_(t2)));
        r1 = F32_(vtrn1q_s32(S32_(t1), S32_(t3)));
        r3 = F32_(vtrn2q_s32(S32_(t1), S32_(t3)));
    }
    HAD4_V(q0, q1, q2, q3, r0, r1, r2, r3);

    acc = vpadalq_u16(vpaddlq_u16(vreinterpretq_u16_s16(vabsq_s16(q0))),
                      vreinterpretq_u16_s16(vabsq_s16(q1)));
    acc = vpadalq_u16(acc, vreinterpretq_u16_s16(vabsq_s16(q2)));
    acc = vpadalq_u16(acc, vreinterpretq_u16_s16(vabsq_s16(q3)));
    return vpaddlq_u32(acc);            /* [tile in lanes 0-3, tile in 4-7] */
}

/* (sum + 1) >> 1 on each of the two tile sums */
static inline uint64x2_t satd4_round(uint64x2_t p)
{
    return vshrq_n_u64(vaddq_u64(p, vdupq_n_u64(1)), 1);
}

/* two tiles SIDE BY SIDE: the eight samples of a row are contiguous, so
 * this is one 8-byte load per row per plane and nothing else */
static inline uint64x2_t satd4_ctg(const uint8_t *a, int as,
                                   const uint8_t *b, int bs)
{
#define ROW_(y) vreinterpretq_s16_u16(vsubl_u8(vld1_u8(a + (y) * as),      \
                                               vld1_u8(b + (y) * bs)))
    return satd4_core(ROW_(0), ROW_(1), ROW_(2), ROW_(3));
#undef ROW_
}

/* two tiles STACKED (a1 = a0 + 4 * as): a row of the pair is two
 * four-byte reads, which is what a one-tile-wide block costs to keep the
 * upper half of the registers doing work */
static inline uint64x2_t satd4_stk(const uint8_t *a0, const uint8_t *a1,
                                   int as, const uint8_t *b0,
                                   const uint8_t *b1, int bs)
{
#define ROW_(y) vreinterpretq_s16_u16(                                    \
        vsubl_u8(ld4x2(a0 + (y) * as, a1 + (y) * as),                     \
                 ld4x2(b0 + (y) * bs, b1 + (y) * bs)))
    return satd4_core(ROW_(0), ROW_(1), ROW_(2), ROW_(3));
#undef ROW_
}

/* ONE tile: the upper half of every register is 0 - 0, so its tile sum is
 * 0 and (0 + 1) >> 1 is 0. The odd tile therefore costs nothing to carry
 * through the same code, and the kernel reads exactly the four bytes per
 * row the C reads. */
static inline uint64x2_t satd4_one(const uint8_t *a, int as,
                                   const uint8_t *b, int bs)
{
#define ROW_(y) vreinterpretq_s16_u16(vsubl_u8(ld4z(a + (y) * as),         \
                                               ld4z(b + (y) * bs)))
    return satd4_core(ROW_(0), ROW_(1), ROW_(2), ROW_(3));
#undef ROW_
}

uint32_t n265_satd4_neon(const uint8_t *a, int as, const uint8_t *b, int bs)
{
    return (uint32_t)vgetq_lane_u64(satd4_round(satd4_one(a, as, b, bs)), 0);
}

/* the 4x4 tiling of the sub-rectangle [x0, w) x [y0, h) */
static uint64x2_t satd4_region(const uint8_t *a, int as, const uint8_t *b,
                               int bs, int x0, int y0, int w, int h)
{
    uint64x2_t acc = vdupq_n_u64(0);

    if (w - x0 == 4) {
        /* one tile wide (w = 4, and the right-hand strip of w = 12): pair
         * the tiles VERTICALLY, so the upper half of every register still
         * does work */
        int y = y0;
        for (; y + 8 <= h; y += 8)
            acc = vaddq_u64(acc, satd4_round(satd4_stk(
                a + (size_t)y * as + x0, a + (size_t)(y + 4) * as + x0, as,
                b + (size_t)y * bs + x0, b + (size_t)(y + 4) * bs + x0, bs)));
        for (; y < h; y += 4)
            acc = vaddq_u64(acc, satd4_round(satd4_one(
                a + (size_t)y * as + x0, as, b + (size_t)y * bs + x0, bs)));
        return acc;
    }
    for (int y = y0; y < h; y += 4) {
        const uint8_t *pa = a + (size_t)y * as;
        const uint8_t *pb = b + (size_t)y * bs;
        int x = x0;
        for (; x + 8 <= w; x += 8)
            acc = vaddq_u64(acc,
                            satd4_round(satd4_ctg(pa + x, as, pb + x, bs)));
        for (; x < w; x += 4)
            acc = vaddq_u64(acc,
                            satd4_round(satd4_one(pa + x, as, pb + x, bs)));
    }
    return acc;
}

/* The w x h loop and both tilings, dispatched once per call instead of
 * once per tile. It mirrors n265_satd_wh_c in src/encoder/satd.c exactly,
 * the N265_SATD8 mixed arm included: that arm CHANGES the ranking metric,
 * so its NEON output has to equal its own C output and not the default
 * arm's.
 */
uint32_t n265_satd_wh_neon(const uint8_t *a, int as, const uint8_t *b,
                           int bs, int w, int h)
{
    uint64x1_t s8 = vdup_n_u64(0);
    uint64x2_t s4 = vdupq_n_u64(0);

    if ((w & 7) || (h & 7)) {
        int w8 = n265_satd8_mixed() ? (w & ~7) : 0;
        int h8 = n265_satd8_mixed() ? (h & ~7) : 0;
        for (int y = 0; y < h8; y += 8)
            for (int x = 0; x < w8; x += 8)
                s8 = vadd_u64(s8, satd8_tile(a + (size_t)y * as + x, as,
                                             b + (size_t)y * bs + x, bs));
        if (h8 > 0 && w8 < w)
            s4 = vaddq_u64(s4, satd4_region(a, as, b, bs, w8, 0, w, h8));
        if (h8 < h)
            s4 = vaddq_u64(s4, satd4_region(a, as, b, bs, 0, h8, w, h));
        return (uint32_t)(vget_lane_u64(s8, 0) + vaddvq_u64(s4));
    }
    for (int y = 0; y < h; y += 8)
        for (int x = 0; x < w; x += 8)
            s8 = vadd_u64(s8, satd8_tile(a + (size_t)y * as + x, as,
                                         b + (size_t)y * bs + x, bs));
    return (uint32_t)vget_lane_u64(s8, 0);
}

#else
typedef int n265_satd_neon_unused_;      /* C wants a non-empty TU */
#endif /* N265_NEON */
