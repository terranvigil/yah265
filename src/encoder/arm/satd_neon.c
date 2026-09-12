/*
 * satd_neon.c - NEON SATD/SAD kernels (class N265_ASM_SATD). One file per
 * kernel family; see src/encoder/arm/neon.h for the dispatch convention
 * every kernel here follows.
 * Copyright (c) 2026, the yah265 authors. GPL-2.0-or-later; see LICENSE.
 */

#if N265_NEON_SATD

#include <arm_neon.h>
#include <stddef.h>
#include <string.h>

#include "common/cpu.h"
#include "encoder/arm/neon.h"
#include "encoder/satd.h"

/* ---- range, and why the lane widths hold it ------------------------
 *
 * SATD works on d = a - b with a, b in [0, PIXEL_MAX], so every input to
 * the transform is in [-PIXEL_MAX, PIXEL_MAX]. An n-point Hadamard is a
 * sum of n terms each plus or minus one input, so it grows the magnitude
 * by at most n per pass. The two depths part company here, and this is
 * the one family where they do (phase 8 item 3):
 *
 *                     8 bits                 Main10
 *   8x8 pass 1   |v| <= 8 * 255 = 2040   8 * 1023 = 8184
 *   8x8 pass 2   |v| <= 16320            |v| <= 65472   -- NOT int16
 *   4x4 pass 1   |v| <= 1020             |v| <= 4092
 *   4x4 pass 2   |v| <= 4080             |v| <= 16368   -- int16, just
 *
 * So at 8 bits both passes of both tiles are exact in int16 and nothing
 * saturates. At Main10 the 4x4 tile is still entirely int16 - 16368 is
 * half of 32767 - and the 8x8 tile's SECOND pass is not: it is run in
 * int32 lanes, two halves of four columns each, after a pass 1 and a
 * transpose that stay int16 because 8184 fits. That costs the 8x8 tile
 * its second pass twice over and is the whole of what the depth takes
 * from this family.
 *
 * vabsq_s16 is exact wherever it is used for the same range reason: the
 * one input whose absolute value it cannot represent is -32768, which is
 * outside every bound above; vabsq_s32 at Main10 is nowhere near its own.
 *
 * The abs-sum widens: vpadalq_u16 folds pairs of u16 into u32 lanes, so
 * one 8-bit 8x8 tile accumulates at most 64 * 16320 = 1044480 across four
 * u32 lanes; at Main10 the u32 lanes take |v| <= 65472 directly and the
 * tile total is at most 64 * 65472 = 4190208. Both are exact in u32 and
 * so is the horizontal vaddvq_u32 of either.
 *
 * The difference itself is one instruction at both depths - vsubl_u8 at
 * 8 bits, vsubq_u16 at Main10 - computed modulo 2^16 and reinterpreted as
 * s16. That is the C's int subtraction exactly: the true value lies in
 * [-PIXEL_MAX, PIXEL_MAX], which s16 represents at both depths, and
 * two's complement is what the lane and the C int both hold.
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

/* the same 8-point butterfly in int32 lanes, for the Main10 8x8 tile's
 * second pass: the operation sequence is the int16 macro's, lane for
 * lane, and only the lane width differs. */
#define HAD8_V32(o0, o1, o2, o3, o4, o5, o6, o7,                        \
                 i0, i1, i2, i3, i4, i5, i6, i7) do {                   \
        int32x4_t b0_ = vaddq_s32(i0, i1), b1_ = vsubq_s32(i0, i1);     \
        int32x4_t b2_ = vaddq_s32(i2, i3), b3_ = vsubq_s32(i2, i3);     \
        int32x4_t b4_ = vaddq_s32(i4, i5), b5_ = vsubq_s32(i4, i5);     \
        int32x4_t b6_ = vaddq_s32(i6, i7), b7_ = vsubq_s32(i6, i7);     \
        int32x4_t c0_ = vaddq_s32(b0_, b2_), c2_ = vsubq_s32(b0_, b2_); \
        int32x4_t c1_ = vaddq_s32(b1_, b3_), c3_ = vsubq_s32(b1_, b3_); \
        int32x4_t c4_ = vaddq_s32(b4_, b6_), c6_ = vsubq_s32(b4_, b6_); \
        int32x4_t c5_ = vaddq_s32(b5_, b7_), c7_ = vsubq_s32(b5_, b7_); \
        (o0) = vaddq_s32(c0_, c4_); (o4) = vsubq_s32(c0_, c4_);         \
        (o1) = vaddq_s32(c1_, c5_); (o5) = vsubq_s32(c1_, c5_);         \
        (o2) = vaddq_s32(c2_, c6_); (o6) = vsubq_s32(c2_, c6_);         \
        (o3) = vaddq_s32(c3_, c7_); (o7) = vsubq_s32(c3_, c7_);         \
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

/* ---- the three difference loaders, one pair of forms per depth ------
 *
 * Every SATD path below starts from one of these, so they are the only
 * place in the family that knows how wide a sample is. Each returns the
 * eight sign-correct differences of one row (or of two half-rows) as
 * int16 lanes, which is the type both tiles' first pass runs in at both
 * depths. */

#if BIT_DEPTH == 8

/* four bytes at p. memcpy rather than a uint32_t * cast: the pointer
 * carries no alignment guarantee and UBSan is part of the item gate. */
static inline uint32_t ld4u(const pixel *p)
{
    uint32_t v;
    memcpy(&v, p, 4);
    return v;
}

/* one row of an 8x8 tile: the eight byte differences, sign-correct */
static inline int16x8_t diff8(const pixel *a, const pixel *b)
{
    return vreinterpretq_s16_u16(vsubl_u8(vld1_u8(a), vld1_u8(b)));
}

/* two 4-byte rows packed into one d register: lanes 0-3 from p0, lanes
 * 4-7 from p1. The two halves are the two 4x4 tiles the kernel below
 * transforms side by side, and no butterfly or shuffle in it crosses the
 * 64-bit boundary, so they stay independent. */
static inline uint8x8_t ld4x2(const pixel *p0, const pixel *p1)
{
    return vreinterpret_u8_u64(
        vcreate_u64((uint64_t)ld4u(p0) | ((uint64_t)ld4u(p1) << 32)));
}

/* four samples at p in the low half, upper half zero */
static inline uint8x8_t ld4z(const pixel *p)
{
    return vreinterpret_u8_u64(vcreate_u64((uint64_t)ld4u(p)));
}

/* the two 4x4 forms: two tiles' rows side by side, and one tile's row
 * with the upper half zeroed */
static inline int16x8_t diff4x2(const pixel *a0, const pixel *a1,
                                const pixel *b0, const pixel *b1)
{
    return vreinterpretq_s16_u16(vsubl_u8(ld4x2(a0, a1), ld4x2(b0, b1)));
}

static inline int16x8_t diff4z(const pixel *a, const pixel *b)
{
    return vreinterpretq_s16_u16(vsubl_u8(ld4z(a), ld4z(b)));
}

#else   /* Main10: a sample is already 16 bits wide, so the subtraction is
         * in place and the "widening" load is a plain one. */

static inline int16x8_t diff8(const pixel *a, const pixel *b)
{
    return vreinterpretq_s16_u16(vsubq_u16(vld1q_u16(a), vld1q_u16(b)));
}

static inline int16x8_t diff4x2(const pixel *a0, const pixel *a1,
                                const pixel *b0, const pixel *b1)
{
    uint16x8_t av = vcombine_u16(vld1_u16(a0), vld1_u16(a1));
    uint16x8_t bv = vcombine_u16(vld1_u16(b0), vld1_u16(b1));

    return vreinterpretq_s16_u16(vsubq_u16(av, bv));
}

static inline int16x8_t diff4z(const pixel *a, const pixel *b)
{
    uint16x4_t z = vdup_n_u16(0);

    return vreinterpretq_s16_u16(
        vsubq_u16(vcombine_u16(vld1_u16(a), z), vcombine_u16(vld1_u16(b), z)));
}

#endif

/* The 8x8 tile, reduced but NOT narrowed: the value stays in a d
 * register so that a caller tiling a 64x64 block can accumulate 64 of
 * these without one vector-to-GPR move per tile. The tile rounding is
 * still per tile - it happens here, on the tile's own sum. */
static inline uint64x1_t satd8_tile(const pixel *a, int as,
                                    const pixel *b, int bs)
{
    int16x8_t r0 = diff8(a + 0 * as, b + 0 * bs);
    int16x8_t r1 = diff8(a + 1 * as, b + 1 * bs);
    int16x8_t r2 = diff8(a + 2 * as, b + 2 * bs);
    int16x8_t r3 = diff8(a + 3 * as, b + 3 * bs);
    int16x8_t r4 = diff8(a + 4 * as, b + 4 * bs);
    int16x8_t r5 = diff8(a + 5 * as, b + 5 * bs);
    int16x8_t r6 = diff8(a + 6 * as, b + 6 * bs);
    int16x8_t r7 = diff8(a + 7 * as, b + 7 * bs);
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

#if BIT_DEPTH == 8
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
#else
    /* Main10 pass 2, in int32. |v| reaches 8 * 8184 = 65472 here, which
     * is outside int16 by a factor of two, so the eight transposed rows
     * are widened and the SAME butterfly is run twice - once on columns
     * 0-3 and once on 4-7. Everything before this point is unchanged:
     * pass 1 tops out at 8184 and the transpose only moves lanes.
     *
     * The abs-sum needs no pairwise fold at this width: |v| <= 65472
     * lands in a u32 lane as it stands and 64 of them total 4190208, so
     * the accumulate is a plain vaddq_u32 and the reduction is exact. */
    {
        int16x8_t rr[8] = { r0, r1, r2, r3, r4, r5, r6, r7 };

        acc = vdupq_n_u32(0);
        for (int half = 0; half < 2; half++) {
            int32x4_t e[8], w[8];

            for (int i = 0; i < 8; i++)
                e[i] = half ? vmovl_high_s16(rr[i])
                            : vmovl_s16(vget_low_s16(rr[i]));
            HAD8_V32(w[0], w[1], w[2], w[3], w[4], w[5], w[6], w[7],
                     e[0], e[1], e[2], e[3], e[4], e[5], e[6], e[7]);
            for (int i = 0; i < 8; i++)
                acc = vaddq_u32(acc,
                                vreinterpretq_u32_s32(vabsq_s32(w[i])));
        }
    }
    p = vpaddlq_u32(acc);
#endif
    s = vadd_u64(vget_low_u64(p), vget_high_u64(p));
    return vshr_n_u64(vadd_u64(s, vdup_n_u64(2)), 2);   /* (sum + 2) >> 2 */
}

uint32_t n265_satd8_neon(const pixel *a, int as, const pixel *b, int bs)
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
static inline uint64x2_t satd4_ctg(const pixel *a, int as,
                                   const pixel *b, int bs)
{
#define ROW_(y) diff8(a + (y) * as, b + (y) * bs)
    return satd4_core(ROW_(0), ROW_(1), ROW_(2), ROW_(3));
#undef ROW_
}

/* two tiles STACKED (a1 = a0 + 4 * as): a row of the pair is two
 * four-byte reads, which is what a one-tile-wide block costs to keep the
 * upper half of the registers doing work */
static inline uint64x2_t satd4_stk(const pixel *a0, const pixel *a1,
                                   int as, const pixel *b0,
                                   const pixel *b1, int bs)
{
#define ROW_(y) diff4x2(a0 + (y) * as, a1 + (y) * as,                     \
                        b0 + (y) * bs, b1 + (y) * bs)
    return satd4_core(ROW_(0), ROW_(1), ROW_(2), ROW_(3));
#undef ROW_
}

/* ONE tile: the upper half of every register is 0 - 0, so its tile sum is
 * 0 and (0 + 1) >> 1 is 0. The odd tile therefore costs nothing to carry
 * through the same code, and the kernel reads exactly the four bytes per
 * row the C reads. */
static inline uint64x2_t satd4_one(const pixel *a, int as,
                                   const pixel *b, int bs)
{
#define ROW_(y) diff4z(a + (y) * as, b + (y) * bs)
    return satd4_core(ROW_(0), ROW_(1), ROW_(2), ROW_(3));
#undef ROW_
}

uint32_t n265_satd4_neon(const pixel *a, int as, const pixel *b, int bs)
{
    return (uint32_t)vgetq_lane_u64(satd4_round(satd4_one(a, as, b, bs)), 0);
}

/* the 4x4 tiling of the sub-rectangle [x0, w) x [y0, h) */
static uint64x2_t satd4_region(const pixel *a, int as, const pixel *b,
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
        const pixel *pa = a + (size_t)y * as;
        const pixel *pb = b + (size_t)y * bs;
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
uint32_t n265_satd_wh_neon(const pixel *a, int as, const pixel *b,
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

/* ---- SAD -------------------------------------------------------------
 *
 * sum |a - b| over a w x h block. The absolute difference is one
 * instruction at both depths and cannot overflow at either - vabdq_u8 on
 * bytes, vabdq_u16 on Main10 samples - and it needs no sign handling at
 * all. The only question left is where the sum is allowed to live, and
 * that answer IS depth-dependent.
 *
 * At 8 bits vpadalq_u8 folds pairs of u8 into u16 lanes at 2 * 255 = 510
 * per lane per 16-byte chunk; a row takes at most four chunks (w <= 64)
 * plus an 8- and a 4-wide tail, peaks below 2600, and is widened into u32
 * once per row. The w = 8 and w = 4 forms skip the per-row widen because
 * their whole block fits one u16 accumulator: at w = 8 a lane takes one
 * sample per row, so it holds any h up to 257 (65535 / 255).
 *
 * AT MAIN10 THAT LAST CLAIM IS GONE, and the C's comment in
 * src/encoder/satd.c says so: one lane taking one sample per row reaches
 * h * 1023, which is 65472 at h = 64 - inside u16 by 63, for the tallest
 * block this encoder has and no taller. A kernel whose correctness rests
 * on a caller never passing h = 65 is not a kernel, so the 16-bit forms
 * accumulate into u32 from the start: vabal_u16 IS the widening
 * accumulate (u32 += |a - b|) for four lanes, and vpadalq_u16 does the
 * pairwise fold for eight. A 64x64 block totals at most 4096 * 1023 =
 * 4190208, exact in u32 at both depths.
 */
uint32_t n265_sad_wh_neon(const pixel *a, int as, const pixel *b,
                          int bs, int w, int h)
{
    uint32x4_t acc = vdupq_n_u32(0);
    uint32_t rest = 0;

#if BIT_DEPTH == 8
    if (w == 8) {
        /* 8x4, 8x8, 8x16, 8x32: 163168 of the 500000-odd n265_mc_sad
         * calls on a CIF encode are 8x8 alone */
        uint16x8_t r = vdupq_n_u16(0);
        for (int y = 0; y < h; y++)
            r = vabal_u8(r, vld1_u8(a + (size_t)y * as),
                         vld1_u8(b + (size_t)y * bs));
        return vaddvq_u32(vpaddlq_u16(r));
    }
    if (w == 4) {
        /* two rows per vector: the 4-wide shapes are the ones where a
         * half-empty register would cost half the throughput */
        uint16x8_t r = vdupq_n_u16(0);
        int y = 0;
        for (; y + 2 <= h; y += 2)
            r = vabal_u8(r,
                         ld4x2(a + (size_t)y * as, a + (size_t)(y + 1) * as),
                         ld4x2(b + (size_t)y * bs, b + (size_t)(y + 1) * bs));
        for (; y < h; y++)
            r = vabal_u8(r, ld4z(a + (size_t)y * as),
                         ld4z(b + (size_t)y * bs));
        return vaddvq_u32(vpaddlq_u16(r));
    }
    for (int y = 0; y < h; y++) {
        const pixel *pa = a + (size_t)y * as;
        const pixel *pb = b + (size_t)y * bs;
        uint16x8_t r = vdupq_n_u16(0);
        int x = 0;
        for (; x + 16 <= w; x += 16)
            r = vpadalq_u8(r, vabdq_u8(vld1q_u8(pa + x), vld1q_u8(pb + x)));
        if (x + 8 <= w) {
            r = vabal_u8(r, vld1_u8(pa + x), vld1_u8(pb + x));
            x += 8;
        }
        if (x + 4 <= w) {
            /* the 12-wide AMP quarter ends here, and ld4z reads exactly
             * the four bytes the C reads: the upper half of the register
             * is |0 - 0| and contributes nothing */
            r = vabal_u8(r, ld4z(pa + x), ld4z(pb + x));
            x += 4;
        }
        acc = vpadalq_u16(acc, r);
#else   /* Main10 */
    /* vabaq_u16 is the whole inner step - one instruction per eight
     * samples, accumulator included - and what makes it usable is that a
     * u16 lane can hold SOME of the block but not all of it. A lane takes
     * one sample per pass, so folding into u32 every 32 passes caps it at
     * 32 * 1023 = 32736, half of u16's range, for any w and any h. That
     * is the shape of every arm below: accumulate narrow, fold on a
     * counter, and never rest on a bound the caller could break. */
    if (w == 8) {
        /* 8x4, 8x8, 8x16, 8x32: 163168 of the 500000-odd n265_mc_sad
         * calls on a CIF encode are 8x8 alone */
        uint16x8_t r = vdupq_n_u16(0);

        for (int y = 0; y < h; y++) {
            r = vabaq_u16(r, vld1q_u16(a + (size_t)y * as),
                          vld1q_u16(b + (size_t)y * bs));
            if ((y & 31) == 31) {
                acc = vpadalq_u16(acc, r);
                r = vdupq_n_u16(0);
            }
        }
        return vaddvq_u32(vpadalq_u16(acc, r));
    }
    if (w == 4) {
        /* two rows per vector: the 4-wide shapes are the ones where a
         * half-empty register would cost half the throughput */
        uint16x8_t r = vdupq_n_u16(0);
        int y = 0;

        for (; y + 2 <= h; y += 2) {
            r = vabaq_u16(r,
                          vcombine_u16(vld1_u16(a + (size_t)y * as),
                                       vld1_u16(a + (size_t)(y + 1) * as)),
                          vcombine_u16(vld1_u16(b + (size_t)y * bs),
                                       vld1_u16(b + (size_t)(y + 1) * bs)));
            if ((y & 63) == 62) {
                acc = vpadalq_u16(acc, r);
                r = vdupq_n_u16(0);
            }
        }
        acc = vpadalq_u16(acc, r);
        for (; y < h; y++)
            acc = vabal_u16(acc, vld1_u16(a + (size_t)y * as),
                            vld1_u16(b + (size_t)y * bs));
        return vaddvq_u32(acc);
    }
    for (int y = 0; y < h; y++) {
        const pixel *pa = a + (size_t)y * as;
        const pixel *pb = b + (size_t)y * bs;
        uint16x8_t r = vdupq_n_u16(0);
        int x = 0;

        /* a row is at most 64 samples, so eight lanes take eight passes
         * and reach 8184: one fold per row is enough here */
        for (; x + 16 <= w; x += 16) {
            r = vabaq_u16(r, vld1q_u16(pa + x), vld1q_u16(pb + x));
            r = vabaq_u16(r, vld1q_u16(pa + x + 8), vld1q_u16(pb + x + 8));
        }
        if (x + 8 <= w) {
            r = vabaq_u16(r, vld1q_u16(pa + x), vld1q_u16(pb + x));
            x += 8;
        }
        acc = vpadalq_u16(acc, r);
        if (x + 4 <= w) {
            /* the 12-wide AMP quarter, and the 4-wide tail of any odd
             * width: vabal_u16 reads exactly the four samples the C reads */
            acc = vabal_u16(acc, vld1_u16(pa + x), vld1_u16(pb + x));
            x += 4;
        }
#endif
        /* Every w the encoder passes is a multiple of four - the callers
         * are all luma, so unlike bi_avg there is no 4:2:0 halving to
         * produce a 2 or a 6 - but the kernel is not allowed to be wrong
         * if one turns up, and a scalar tail costs nothing when it does
         * not run. */
        for (; x < w; x++) {
            int d = pa[x] - pb[x];
            rest += (uint32_t)(d < 0 ? -d : d);
        }
    }
    return vaddvq_u32(acc) + rest;
}

/* ---- per-row and per-column SAD --------------------------------------
 *
 * rect_proxy's one pass over the CU (satd.c). The two marginal sums want
 * opposite reductions of the same difference vector: a column sum
 * accumulates lane by lane down the rows, a row sum folds the lanes of
 * one row together.
 *
 * So the loop is over COLUMN STRIPS rather than rows. A strip holds its
 * column accumulator in registers for the whole block - two u16x8 for a
 * 16-wide strip - and pays for the row sums with one horizontal add per
 * row into the row array, which is at most 64 u32 and sits in L1. The
 * other order would need n/8 column accumulators live at once and would
 * spill every one of them on a 32-wide CU, which is where this kernel's
 * calls carry the most pixels.
 *
 * The u16 column lanes are exact AT BOTH DEPTHS, but for different
 * reasons and with very different margins. A column of n samples sums to
 * at most n * PIXEL_MAX: at 8 bits that is 16320 at the largest CU and
 * the lane would hold any n up to 257; at Main10 it is n * 1023, which
 * is 32736 at the n = 32 this kernel's callers reach and 65472 at n = 64.
 * The caller's shape set is 8, 16 and 32 and cannot be anything else (a
 * CU is square, the rectangular-partition block lives under analyze_cu's
 * log2n <= 5 arm, MIN_CB_LOG2 is 3, and an instrumented 420k-call run
 * found no n = 4 and no n = 64), so u16 columns are exact here - but the
 * Main10 margin is one doubling of n, not five, and the scalar tail
 * below is what a larger n would have to go through.
 */
void n265_sad_rowcol_neon(const pixel *a, int as, const pixel *b,
                          int bs, int n, uint32_t *row, uint32_t *col)
{
    int x = 0;

    for (int y = 0; y < n; y++)
        row[y] = 0;

#if BIT_DEPTH == 8
    for (; x + 16 <= n; x += 16) {
        uint16x8_t c0 = vdupq_n_u16(0), c1 = vdupq_n_u16(0);

        for (int y = 0; y < n; y++) {
            uint8x16_t d = vabdq_u8(vld1q_u8(a + (size_t)y * as + x),
                                    vld1q_u8(b + (size_t)y * bs + x));
            row[y] += vaddlvq_u8(d);
            c0 = vaddw_u8(c0, vget_low_u8(d));
            c1 = vaddw_u8(c1, vget_high_u8(d));
        }
        vst1q_u32(col + x, vmovl_u16(vget_low_u16(c0)));
        vst1q_u32(col + x + 4, vmovl_u16(vget_high_u16(c0)));
        vst1q_u32(col + x + 8, vmovl_u16(vget_low_u16(c1)));
        vst1q_u32(col + x + 12, vmovl_u16(vget_high_u16(c1)));
    }
    if (x + 8 <= n) {
        /* the 8x8 CU, two thirds of the calls on a 720p encode at medium */
        uint16x8_t c0 = vdupq_n_u16(0);

        for (int y = 0; y < n; y++) {
            uint8x8_t d = vabd_u8(vld1_u8(a + (size_t)y * as + x),
                                  vld1_u8(b + (size_t)y * bs + x));
            row[y] += vaddlv_u8(d);
            c0 = vaddw_u8(c0, d);
        }
        vst1q_u32(col + x, vmovl_u16(vget_low_u16(c0)));
        vst1q_u32(col + x + 4, vmovl_u16(vget_high_u16(c0)));
        x += 8;
    }
    if (x + 4 <= n) {
        /* ld4z reads exactly the four bytes the C reads; the upper half
         * of the register is |0 - 0| and adds nothing to either sum */
        uint16x8_t c0 = vdupq_n_u16(0);

        for (int y = 0; y < n; y++) {
            uint8x8_t d = vabd_u8(ld4z(a + (size_t)y * as + x),
                                  ld4z(b + (size_t)y * bs + x));
            row[y] += vaddlv_u8(d);
            c0 = vaddw_u8(c0, d);
        }
        vst1q_u32(col + x, vmovl_u16(vget_low_u16(c0)));
        x += 4;
    }
#else   /* Main10: the same strips, eight samples to a vector instead of
         * sixteen, and the row fold is vaddlvq_u16 (8 * 1023 = 8184, so
         * the u32 it returns is exact). */
    for (; x + 8 <= n; x += 8) {
        uint16x8_t c0 = vdupq_n_u16(0);

        for (int y = 0; y < n; y++) {
            uint16x8_t d = vabdq_u16(vld1q_u16(a + (size_t)y * as + x),
                                     vld1q_u16(b + (size_t)y * bs + x));
            row[y] += vaddlvq_u16(d);
            c0 = vaddq_u16(c0, d);
        }
        vst1q_u32(col + x, vmovl_u16(vget_low_u16(c0)));
        vst1q_u32(col + x + 4, vmovl_u16(vget_high_u16(c0)));
    }
    if (x + 4 <= n) {
        uint16x4_t c0 = vdup_n_u16(0);

        for (int y = 0; y < n; y++) {
            uint16x4_t d = vabd_u16(vld1_u16(a + (size_t)y * as + x),
                                    vld1_u16(b + (size_t)y * bs + x));
            row[y] += vaddlv_u16(d);
            c0 = vadd_u16(c0, d);
        }
        vst1q_u32(col + x, vmovl_u16(c0));
        x += 4;
    }
#endif
    /* A CU is a power of two of at least 8, so no caller reaches this -
     * but the kernel is not allowed to be wrong if one ever does, and a
     * tail costs nothing when it does not run. */
    for (; x < n; x++) {
        uint32_t c = 0;

        for (int y = 0; y < n; y++) {
            int d = a[(size_t)y * as + x] - b[(size_t)y * bs + x];
            uint32_t v = (uint32_t)(d < 0 ? -d : d);

            row[y] += v;
            c += v;
        }
        col[x] = c;
    }
}

/* ---- SSD -------------------------------------------------------------
 *
 * sum of (a - b)^2. The absolute difference is the signed one up to
 * sign, and squaring loses the sign, so both depths go through the
 * unsigned |a - b| and neither loses anything by it.
 *
 * At 8 bits |d| <= 255 and vmull_u8 of it is at most 65025, which fits
 * u16 exactly; vpadalq_u16 folds pairs of those into u32 lanes, and the
 * largest block any caller passes is 64x64 (cu_area_ssd at a CB64,
 * luma), whose total is at most 4096 * 65025 = 266342400 - so neither a
 * lane nor the horizontal sum of all four can overflow, and the widen to
 * u64 happens once, at the return.
 *
 * AT MAIN10 BOTH OF THOSE STAGES MOVE. |d| <= 1023 squares to 1046529,
 * which needs a u32 lane (vmlal_u16 accumulates straight into one), and
 * the whole-block bound is 4096 * 1046529 = 4286222784 against 2^32 =
 * 4294967296 - inside by a fifth of a percent. That is not a margin to
 * ship a kernel on, and it is the warning src/encoder/satd.c left for
 * this item, so the 16-bit form carries a u32 accumulator for ONE ROW at
 * a time (at most 64 * 1046529 = 66977856) and folds it into u64 lanes
 * per row. The u64 in the signature is for the callers either way, which
 * sum many blocks.
 *
 * The FEAT_DotProd arm is 8-bit only and stays that way: udot is defined
 * on byte lanes, there is no 16-bit equivalent outside SVE, and the
 * widening multiply-accumulate is already the one instruction per four
 * samples that this reduction wants.
 */
uint64_t n265_ssd_wh_neon(const pixel *a, int as, const pixel *b,
                          int bs, int w, int h)
{
    uint64_t rest = 0;
#if BIT_DEPTH == 8
    uint32x4_t acc = vdupq_n_u32(0);

#if defined(__ARM_FEATURE_DOTPROD)
    /* The DotProd arm below is faster at every shape this kernel sees and
     * twice as fast at the big ones (64x64: 158 ns against 318), because
     * udot(acc, d, d) replaces two vmull_u8 and two vpadalq_u16 per
     * sixteen bytes with one instruction. Both arms are bit-exact against
     * the same C reference and checkasm holds both to it, so this
     * selection moves time and nothing else. */
    if (n265_cpu_flags() & N265_CPU_DOTPROD)
        return n265_ssd_wh_dot_neon(a, as, b, bs, w, h);
#endif

    for (int y = 0; y < h; y++) {
        const pixel *pa = a + (size_t)y * as;
        const pixel *pb = b + (size_t)y * bs;
        int x = 0;
        for (; x + 16 <= w; x += 16) {
            uint8x16_t d = vabdq_u8(vld1q_u8(pa + x), vld1q_u8(pb + x));
            acc = vpadalq_u16(acc, vmull_u8(vget_low_u8(d), vget_low_u8(d)));
            acc = vpadalq_u16(acc, vmull_high_u8(d, d));
        }
        if (x + 8 <= w) {
            uint8x8_t d = vabd_u8(vld1_u8(pa + x), vld1_u8(pb + x));
            acc = vpadalq_u16(acc, vmull_u8(d, d));
            x += 8;
        }
        if (x + 4 <= w) {
            uint8x8_t d = vabd_u8(ld4z(pa + x), ld4z(pb + x));
            acc = vpadalq_u16(acc, vmull_u8(d, d));
            x += 4;
        }
        for (; x < w; x++) {
            int d = pa[x] - pb[x];
            rest += (uint64_t)(d * d);
        }
    }
    return (uint64_t)vaddvq_u32(acc) + rest;
#else
    uint64x2_t total = vdupq_n_u64(0);

    for (int y = 0; y < h; y++) {
        const pixel *pa = a + (size_t)y * as;
        const pixel *pb = b + (size_t)y * bs;
        uint32x4_t acc = vdupq_n_u32(0);
        int x = 0;

        for (; x + 8 <= w; x += 8) {
            uint16x8_t d = vabdq_u16(vld1q_u16(pa + x), vld1q_u16(pb + x));

            acc = vmlal_u16(acc, vget_low_u16(d), vget_low_u16(d));
            acc = vmlal_high_u16(acc, d, d);
        }
        if (x + 4 <= w) {
            uint16x4_t d = vabd_u16(vld1_u16(pa + x), vld1_u16(pb + x));

            acc = vmlal_u16(acc, d, d);
            x += 4;
        }
        for (; x < w; x++) {
            int d = pa[x] - pb[x];
            rest += (uint64_t)(d * d);
        }
        /* one fold per row: a row is at most 64 * 1046529 in u32 lanes,
         * and the block total is not */
        total = vpadalq_u32(total, acc);
    }
    return vaddvq_u64(total) + rest;
#endif
}

#if BIT_DEPTH == 8 && defined(__ARM_FEATURE_DOTPROD)
/* The FEAT_DotProd arm. udot(acc, d, d) IS the sum of four squares
 * accumulated into a u32 lane, which is the whole of this kernel's inner
 * step: one instruction where the base form needs two vmull_u8 and two
 * vpadalq_u16 per sixteen bytes.
 *
 * The range claim is the same one, one stage shorter: each udot adds at
 * most 4 * 65025 = 260100 to a lane, a 64x64 block has 256 sixteen-byte
 * chunks, and 256 * 260100 = 66585600 per lane - a quarter of the
 * 266342400 whole-block bound, all of it exact in u32.
 *
 * Selected at RUNTIME by n265_ssd_wh_neon on N265_CPU_DOTPROD, never by
 * the compile-time macro alone: the macro says the compiler can emit the
 * instruction, the feature word says this CPU can run it. Exported so
 * checkasm checks and times BOTH arms on a host that has the extension.
 */
uint64_t n265_ssd_wh_dot_neon(const pixel *a, int as, const pixel *b,
                              int bs, int w, int h)
{
    uint32x4_t acc = vdupq_n_u32(0);
    uint32x2_t acc2 = vdup_n_u32(0);
    uint64_t rest = 0;

    for (int y = 0; y < h; y++) {
        const pixel *pa = a + (size_t)y * as;
        const pixel *pb = b + (size_t)y * bs;
        int x = 0;
        for (; x + 16 <= w; x += 16) {
            uint8x16_t d = vabdq_u8(vld1q_u8(pa + x), vld1q_u8(pb + x));
            acc = vdotq_u32(acc, d, d);
        }
        if (x + 8 <= w) {
            uint8x8_t d = vabd_u8(vld1_u8(pa + x), vld1_u8(pb + x));
            acc2 = vdot_u32(acc2, d, d);
            x += 8;
        }
        if (x + 4 <= w) {
            uint8x8_t d = vabd_u8(ld4z(pa + x), ld4z(pb + x));
            acc2 = vdot_u32(acc2, d, d);
            x += 4;
        }
        for (; x < w; x++) {
            int d = pa[x] - pb[x];
            rest += (uint64_t)(d * d);
        }
    }
    return (uint64_t)vaddvq_u32(acc) + vaddv_u32(acc2) + rest;
}
#endif /* BIT_DEPTH == 8 && __ARM_FEATURE_DOTPROD */

#else
typedef int n265_satd_neon_unused_;      /* C wants a non-empty TU */
#endif /* N265_NEON_SATD */
