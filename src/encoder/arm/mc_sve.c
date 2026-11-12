/*
 * mc_sve.c - SVE luma interpolation (class N265_ASM_MC), the vertical
 * pass over the cached first-stage int16 rows. See src/encoder/arm/sve.h.
 *
 * Each row is loaded sign-extended straight into int32 lanes, so a tap
 * is one multiply-accumulate per svcntw() outputs with no widening step.
 * The sum is the C's: exact in int32 (the largest is 3669928) and in any
 * order. The pixel store clips to [0, PIXEL_MAX] and narrows with a
 * truncating st1b/st1h; the 14-bit store keeps the low 16 bits of s >> 6,
 * which is the C's int16_t conversion.
 *
 * Copyright (c) 2026, the yah265 authors. GPL-2.0-or-later; see LICENSE.
 */

#if N265_SVE_MC

#include <arm_sve.h>

#include "encoder/arm/sve.h"

#define SHIFT3  (14 - BIT_DEPTH)
#define SHIFT23 (6 + SHIFT3)

static const int16_t filt[4][8] = {
    {  0, 0,   0, 64,  0,   0, 0,  0 },
    { -1, 4, -10, 58, 17,  -5, 1,  0 },
    { -1, 4, -11, 40, 40, -11, 4, -1 },
    {  0, 1,  -5, 17, 58, -10, 4, -1 },
};

#define ATTR static inline __attribute__((always_inline))

/* one output row from eight input rows; with fy a constant the zero taps
 * drop out */
ATTR svint32_t tap8(const int fy, svint32_t r0, svint32_t r1, svint32_t r2,
                    svint32_t r3, svint32_t r4, svint32_t r5, svint32_t r6,
                    svint32_t r7)
{
    const int16_t *f = filt[fy];
    const svbool_t pt = svptrue_b32();
    svint32_t o = svmul_n_s32_x(pt, r3, f[3]);

    if (f[0])
        o = svmla_n_s32_x(pt, o, r0, f[0]);
    o = svmla_n_s32_x(pt, o, r1, f[1]);
    o = svmla_n_s32_x(pt, o, r2, f[2]);
    o = svmla_n_s32_x(pt, o, r4, f[4]);
    o = svmla_n_s32_x(pt, o, r5, f[5]);
    o = svmla_n_s32_x(pt, o, r6, f[6]);
    if (f[7])
        o = svmla_n_s32_x(pt, o, r7, f[7]);
    return o;
}

ATTR svint32_t v1(svbool_t pg, const int16_t *p, int hs, const int fy)
{
    const int16_t *f = filt[fy];
    const svbool_t pt = svptrue_b32();
    svint32_t o = svdup_s32(0);

    for (int k = 0; k < 8; k++)
        if (f[k])
            o = svmla_n_s32_x(pt, o, svld1sh_s32(pg, p + (ptrdiff_t)(k - 3) * hs),
                              f[k]);
    return o;
}

ATTR void st_pix(svbool_t pg, pixel *d, svint32_t s)
{
    const svbool_t pt = svptrue_b32();
    s = svasr_n_s32_x(pt, svadd_n_s32_x(pt, s, 1 << (SHIFT23 - 1)), SHIFT23);
    s = svmin_n_s32_x(pt, svmax_n_s32_x(pt, s, 0), PIXEL_MAX);
#if BIT_DEPTH == 8
    svst1b_s32(pg, (int8_t *)d, s);
#else
    svst1h_s32(pg, (int16_t *)d, s);
#endif
}

ATTR void st_s16(svbool_t pg, int16_t *d, svint32_t s)
{
    svst1h_s32(pg, d, svasr_n_s32_x(svptrue_b32(), s, 6));
}

/* rows -3 .. 7 of a four-row group, sign-extended into int32 lanes */
#define LD_(k) svld1sh_s32(pg, p_ + (ptrdiff_t)((k) - 3) * hs)
#define LOAD11(p) \
    const int16_t *p_ = (p); \
    svint32_t r0 = LD_(0), r1 = LD_(1), r2 = LD_(2), r3 = LD_(3); \
    svint32_t r4 = LD_(4), r5 = LD_(5), r6 = LD_(6), r7 = LD_(7); \
    svint32_t r8 = LD_(8), r9 = LD_(9), r10 = LD_(10)

ATTR void v_pix(pixel *dst, int ds, int w, int h, const int16_t *hf, int hs,
                const int fy)
{
    const int step = (int)svcntw();
    int y = 0;

    for (; y + 4 <= h; y += 4)
        for (int x = 0; x < w; x += step) {
            svbool_t pg = svwhilelt_b32(x, w);
            pixel *d = dst + (ptrdiff_t)y * ds + x;
            LOAD11(hf + (ptrdiff_t)y * hs + x);
            st_pix(pg, d, tap8(fy, r0, r1, r2, r3, r4, r5, r6, r7));
            st_pix(pg, d + ds, tap8(fy, r1, r2, r3, r4, r5, r6, r7, r8));
            st_pix(pg, d + 2 * ds, tap8(fy, r2, r3, r4, r5, r6, r7, r8, r9));
            st_pix(pg, d + 3 * ds, tap8(fy, r3, r4, r5, r6, r7, r8, r9, r10));
        }
    for (; y < h; y++)
        for (int x = 0; x < w; x += step) {
            svbool_t pg = svwhilelt_b32(x, w);
            st_pix(pg, dst + (ptrdiff_t)y * ds + x,
                   v1(pg, hf + (ptrdiff_t)y * hs + x, hs, fy));
        }
}

ATTR void v_s16(int16_t *dst, int ds, int w, int h, const int16_t *hf,
                int hs, const int fy)
{
    const int step = (int)svcntw();
    int y = 0;

    for (; y + 4 <= h; y += 4)
        for (int x = 0; x < w; x += step) {
            svbool_t pg = svwhilelt_b32(x, w);
            int16_t *d = dst + (ptrdiff_t)y * ds + x;
            LOAD11(hf + (ptrdiff_t)y * hs + x);
            st_s16(pg, d, tap8(fy, r0, r1, r2, r3, r4, r5, r6, r7));
            st_s16(pg, d + ds, tap8(fy, r1, r2, r3, r4, r5, r6, r7, r8));
            st_s16(pg, d + 2 * ds, tap8(fy, r2, r3, r4, r5, r6, r7, r8, r9));
            st_s16(pg, d + 3 * ds, tap8(fy, r3, r4, r5, r6, r7, r8, r9, r10));
        }
    for (; y < h; y++)
        for (int x = 0; x < w; x += step) {
            svbool_t pg = svwhilelt_b32(x, w);
            st_s16(pg, dst + (ptrdiff_t)y * ds + x,
                   v1(pg, hf + (ptrdiff_t)y * hs + x, hs, fy));
        }
}

/* fy == 0: clip1((v + round) >> shift3) of the cached row */
static void copy_pix(pixel *dst, int ds, int w, int h, const int16_t *hf,
                     int hs)
{
    const svbool_t pt = svptrue_b32();
    const int step = (int)svcntw();

    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x += step) {
            svbool_t pg = svwhilelt_b32(x, w);
            svint32_t s = svld1sh_s32(pg, hf + (ptrdiff_t)y * hs + x);
            s = svasr_n_s32_x(pt, svadd_n_s32_x(pt, s, 1 << (SHIFT3 - 1)),
                              SHIFT3);
            s = svmin_n_s32_x(pt, svmax_n_s32_x(pt, s, 0), PIXEL_MAX);
#if BIT_DEPTH == 8
            svst1b_s32(pg, (int8_t *)dst + (ptrdiff_t)y * ds + x, s);
#else
            svst1h_s32(pg, (int16_t *)dst + (ptrdiff_t)y * ds + x, s);
#endif
        }
}

void n265_mc_luma_hf_sve(pixel *dst, int ds, int w, int h,
                         const int16_t *hf, int hs, int fy)
{
    switch (fy) {
    case 0:  copy_pix(dst, ds, w, h, hf, hs); return;
    case 1:  v_pix(dst, ds, w, h, hf, hs, 1); return;
    case 2:  v_pix(dst, ds, w, h, hf, hs, 2); return;
    default: v_pix(dst, ds, w, h, hf, hs, 3); return;
    }
}

void n265_mc_luma_14_hf_sve(int16_t *dst, int ds, int w, int h,
                            const int16_t *hf, int hs, int fy)
{
    switch (fy) {
    case 0:
        for (int y = 0; y < h; y++)
            for (int x = 0; x < w; x += (int)svcnth()) {
                svbool_t pg = svwhilelt_b16(x, w);
                svst1(pg, dst + (ptrdiff_t)y * ds + x,
                      svld1(pg, hf + (ptrdiff_t)y * hs + x));
            }
        return;
    case 1:  v_s16(dst, ds, w, h, hf, hs, 1); return;
    case 2:  v_s16(dst, ds, w, h, hf, hs, 2); return;
    default: v_s16(dst, ds, w, h, hf, hs, 3); return;
    }
}

#else
typedef int n265_mc_sve_unused_;
#endif /* N265_SVE_MC */
