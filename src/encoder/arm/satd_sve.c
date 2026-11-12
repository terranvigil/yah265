/*
 * satd_sve.c - SVE pixel metrics (class N265_ASM_SATD), vector-length
 * agnostic. See src/encoder/arm/sve.h for the convention.
 *
 * Ranges: an 8-bit |a - b| is at most 255 and a Main10 one 1023. udot on
 * bytes adds four products to a u32 lane, and on halfwords four to a u64
 * lane, so no lane below can overflow for any block up to 64x64.
 *
 * Copyright (c) 2026, the yah265 authors. GPL-2.0-or-later; see LICENSE.
 */

#if N265_SVE_SATD

#include <arm_sve.h>

#include "encoder/arm/sve.h"

/* ---- 8x8 SATD, one tile per 128-bit segment ---------------------------
 *
 * Every shuffle below (trn1/trn2 at 16, 32 and 64 bits) stays inside a
 * 128-bit segment, so a vector of N segments carries N horizontally
 * adjacent 8x8 tiles through the NEON kernel's own transform: pass 1 down
 * the columns, transpose, pass 2. Pass 2 drops its last stage because
 * |x + y| + |x - y| = 2 max(|x|, |y|), so a tile's abs-sum is 2M and the
 * C's (2M + 2) >> 2 is (M + 1) >> 1 at both depths.
 *
 * At Main10 pass 1 reaches 8 * 1023 = 8184 and pass 2's two stages
 * 4 * 8184 = 32736, so everything stays int16. A max is at most 32736,
 * so two of them sum in u16 before the widening udot; at 8 bits each is
 * at most 8160 and four sum in u16.
 */
#define BFLY(x, y) do {                                                 \
        svint16_t s_ = svadd_x(pt, x, y);                               \
        y = svsub_x(pt, x, y);                                          \
        x = s_;                                                         \
    } while (0)

static inline svint16_t row_diff(svbool_t pg, const pixel *a,
                                 const pixel *b)
{
#if BIT_DEPTH == 8
    return svsub_s16_x(svptrue_b16(), svld1ub_s16(pg, a), svld1ub_s16(pg, b));
#else
    return svreinterpret_s16(svsub_u16_x(svptrue_b16(), svld1_u16(pg, a),
                                         svld1_u16(pg, b)));
#endif
}

#define T16(x, y) do {                                                  \
        svint16_t u_ = svtrn1(x, y);                                    \
        y = svtrn2(x, y);                                               \
        x = u_;                                                         \
    } while (0)
#define T32(x, y) do {                                                  \
        svint32_t a_ = svreinterpret_s32(x), b_ = svreinterpret_s32(y); \
        x = svreinterpret_s16(svtrn1(a_, b_));                          \
        y = svreinterpret_s16(svtrn2(a_, b_));                          \
    } while (0)
#define T64(x, y) do {                                                  \
        svint64_t a_ = svreinterpret_s64(x), b_ = svreinterpret_s64(y); \
        x = svreinterpret_s16(svtrn1(a_, b_));                          \
        y = svreinterpret_s16(svtrn2(a_, b_));                          \
    } while (0)

/* the per-tile rounded values of one strip, one u64 per tile segment
 * (both lanes of a segment hold it) */
static inline svuint64_t satd8_strip(svbool_t pg, const pixel *a, int as,
                                     const pixel *b, int bs)
{
    const svbool_t pt = svptrue_b16();
    svint16_t r0 = row_diff(pg, a, b);
    svint16_t r1 = row_diff(pg, a + as, b + bs);
    svint16_t r2 = row_diff(pg, a + 2 * as, b + 2 * bs);
    svint16_t r3 = row_diff(pg, a + 3 * as, b + 3 * bs);
    svint16_t r4 = row_diff(pg, a + 4 * as, b + 4 * bs);
    svint16_t r5 = row_diff(pg, a + 5 * as, b + 5 * bs);
    svint16_t r6 = row_diff(pg, a + 6 * as, b + 6 * bs);
    svint16_t r7 = row_diff(pg, a + 7 * as, b + 7 * bs);

    /* pass 1, down the columns */
    BFLY(r0, r1); BFLY(r2, r3); BFLY(r4, r5); BFLY(r6, r7);
    BFLY(r0, r2); BFLY(r1, r3); BFLY(r4, r6); BFLY(r5, r7);
    BFLY(r0, r4); BFLY(r1, r5); BFLY(r2, r6); BFLY(r3, r7);

    /* transpose each segment's 8x8 */
    T16(r0, r1); T16(r2, r3); T16(r4, r5); T16(r6, r7);
    T32(r0, r2); T32(r1, r3); T32(r4, r6); T32(r5, r7);
    T64(r0, r4); T64(r1, r5); T64(r2, r6); T64(r3, r7);

    /* pass 2 without its last stage */
    BFLY(r0, r1); BFLY(r2, r3); BFLY(r4, r5); BFLY(r6, r7);
    BFLY(r0, r2); BFLY(r1, r3); BFLY(r4, r6); BFLY(r5, r7);
#define MAXABS(x, y) svreinterpret_u16(svmax_x(pt, svabs_x(pt, x), svabs_x(pt, y)))
    svuint16_t m0 = MAXABS(r0, r4), m1 = MAXABS(r1, r5);
    svuint16_t m2 = MAXABS(r2, r6), m3 = MAXABS(r3, r7);
#undef MAXABS
    const svuint16_t one = svdup_u16(1);
    svuint64_t acc;
#if BIT_DEPTH == 8
    svuint16_t m = svadd_x(pt, svadd_x(pt, m0, m1), svadd_x(pt, m2, m3));
    acc = svdot(svdup_u64(0), m, one);
#else
    acc = svdot(svdup_u64(0), svadd_x(pt, m0, m1), one);
    acc = svdot(acc, svadd_x(pt, m2, m3), one);
#endif
    /* M per segment, then (M + 1) >> 1 */
    const svbool_t pd = svptrue_b64();
    svuint64_t t = svadd_x(pd, svtrn1(acc, acc), svtrn2(acc, acc));
    return svlsr_x(pd, svadd_x(pd, t, 1), 1);
}

uint32_t n265_satd_wh_sve(const pixel *a, int as, const pixel *b, int bs,
                          int w, int h)
{
    const int step = (int)svcnth();
    svuint64_t tot = svdup_u64(0);

    for (int y = 0; y < h; y += 8) {
        const pixel *pa = a + (size_t)y * as, *pb = b + (size_t)y * bs;
        for (int x = 0; x < w; x += step)
            tot = svadd_x(svptrue_b64(), tot,
                          satd8_strip(svwhilelt_b16(x, w), pa + x, as,
                                      pb + x, bs));
    }
    /* each tile was counted in both lanes of its segment */
    return (uint32_t)(svaddv(svptrue_b64(), tot) >> 1);
}

/* ---- SAD, SSD and the sample sums --------------------------------- */

#if BIT_DEPTH == 8

uint32_t n265_sad_wh_sve(const pixel *a, int as, const pixel *b, int bs,
                         int w, int h)
{
    const int step = (int)svcntb();
    const svuint8_t one = svdup_u8(1);
    svuint32_t acc = svdup_u32(0);

    for (int y = 0; y < h; y++) {
        const pixel *pa = a + (size_t)y * as, *pb = b + (size_t)y * bs;
        for (int x = 0; x < w; x += step) {
            svbool_t pg = svwhilelt_b8(x, w);
            svuint8_t d = svabd_x(svptrue_b8(), svld1(pg, pa + x),
                                  svld1(pg, pb + x));
            acc = svdot(acc, d, one);
        }
    }
    return (uint32_t)svaddv(svptrue_b32(), acc);
}

uint64_t n265_ssd_wh_sve(const pixel *a, int as, const pixel *b, int bs,
                         int w, int h)
{
    const int step = (int)svcntb();
    svuint32_t acc = svdup_u32(0);

    for (int y = 0; y < h; y++) {
        const pixel *pa = a + (size_t)y * as, *pb = b + (size_t)y * bs;
        for (int x = 0; x < w; x += step) {
            svbool_t pg = svwhilelt_b8(x, w);
            svuint8_t d = svabd_x(svptrue_b8(), svld1(pg, pa + x),
                                  svld1(pg, pb + x));
            acc = svdot(acc, d, d);
        }
    }
    return svaddv(svptrue_b32(), acc);
}

uint32_t n265_sum_sq_wh_sve(const pixel *p, int stride, int w, int h,
                            uint64_t *sq)
{
    const int step = (int)svcntb();
    const svuint8_t one = svdup_u8(1);
    svuint32_t s = svdup_u32(0), q = svdup_u32(0);

    for (int y = 0; y < h; y++, p += stride)
        for (int x = 0; x < w; x += step) {
            svuint8_t v = svld1(svwhilelt_b8(x, w), p + x);
            s = svdot(s, v, one);
            q = svdot(q, v, v);
        }
    *sq = svaddv(svptrue_b32(), q);
    return (uint32_t)svaddv(svptrue_b32(), s);
}

#else /* Main10: the 16-bit udot accumulates straight into u64 lanes */

uint32_t n265_sad_wh_sve(const pixel *a, int as, const pixel *b, int bs,
                         int w, int h)
{
    const int step = (int)svcnth();
    const svuint16_t one = svdup_u16(1);
    svuint64_t acc = svdup_u64(0);

    for (int y = 0; y < h; y++) {
        const pixel *pa = a + (size_t)y * as, *pb = b + (size_t)y * bs;
        for (int x = 0; x < w; x += step) {
            svbool_t pg = svwhilelt_b16(x, w);
            svuint16_t d = svabd_x(svptrue_b16(), svld1(pg, pa + x),
                                   svld1(pg, pb + x));
            acc = svdot(acc, d, one);
        }
    }
    return (uint32_t)svaddv(svptrue_b64(), acc);
}

uint64_t n265_ssd_wh_sve(const pixel *a, int as, const pixel *b, int bs,
                         int w, int h)
{
    const int step = (int)svcnth();
    svuint64_t acc = svdup_u64(0);

    for (int y = 0; y < h; y++) {
        const pixel *pa = a + (size_t)y * as, *pb = b + (size_t)y * bs;
        for (int x = 0; x < w; x += step) {
            svbool_t pg = svwhilelt_b16(x, w);
            svuint16_t d = svabd_x(svptrue_b16(), svld1(pg, pa + x),
                                   svld1(pg, pb + x));
            acc = svdot(acc, d, d);
        }
    }
    return svaddv(svptrue_b64(), acc);
}

uint32_t n265_sum_sq_wh_sve(const pixel *p, int stride, int w, int h,
                            uint64_t *sq)
{
    const int step = (int)svcnth();
    const svuint16_t one = svdup_u16(1);
    svuint64_t s = svdup_u64(0), q = svdup_u64(0);

    for (int y = 0; y < h; y++, p += stride)
        for (int x = 0; x < w; x += step) {
            svuint16_t v = svld1(svwhilelt_b16(x, w), p + x);
            s = svdot(s, v, one);
            q = svdot(q, v, v);
        }
    *sq = svaddv(svptrue_b64(), q);
    return (uint32_t)svaddv(svptrue_b64(), s);
}

#endif

#else
typedef int n265_satd_sve_unused_;
#endif /* N265_SVE_SATD */
