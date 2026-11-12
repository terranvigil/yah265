/*
 * sve.h - the SVE kernel entry points (docs/sve.md).
 *
 * The NEON convention (neon.h) with _sve names: each kernel has its C
 * entry point's signature, lives in src/encoder/arm/<family>_sve.c, and is
 * bit-exact with the C reference at every vector length. A dispatch site
 * tries it before the NEON form, under n265_sve_on(class, the feature
 * bits of the CPU shapes where it measured faster than NEON).
 *
 * Copyright (c) 2026, the yah265 authors. GPL-2.0-or-later; see LICENSE.
 */

#ifndef N265_ARM_SVE_H
#define N265_ARM_SVE_H

#include <stddef.h>
#include <stdint.h>

#include "common/cpu.h"
#include "common/pixel.h"

#define N265_SVE_(f)    ((f) & N265_CPU_SVE)
#define N265_SVE256_(f) ((f) & N265_CPU_SVE256)

#if N265_SVE_SATD
/* Where each kernel ships, from the checkasm --bench tables in
 * docs/sve.md: f is the feature word, and N265_CPU_SVE256 tells the
 * 256-bit Graviton3 shape from the 128-bit Graviton4 one. */
#define N265_SVE_SATD_OK(f, w, h) (!(((w) | (h)) & 7) && (w) >= 16 && \
    (BIT_DEPTH > 8 ? N265_SVE_(f) : N265_SVE256_(f)))
#if BIT_DEPTH == 8
#define N265_SVE_SAD_OK(f, w, h) (N265_SVE_(f) && ((w) & 15) && (w) > 8)
#define N265_SVE_SSD_OK(f, w, h) \
    (N265_SVE_(f) && ((w) >= 12 || ((w) == 4 && (h) == 4)))
#else
#define N265_SVE_SAD_OK(f, w, h) \
    (N265_SVE_(f) && (w) >= 12 && (N265_SVE256_(f) || (w) <= 32))
#define N265_SVE_SSD_OK(f, w, h) (N265_SVE_(f) && (w) >= 8)
#endif
#define N265_SVE_SUMSQ_OK(f, w, h) (N265_SVE_(f) && (w) >= 16 && (h) >= 16)

/* w and h multiples of 8 */
uint32_t n265_satd_wh_sve(const pixel *a, int as, const pixel *b, int bs,
                          int w, int h);
uint32_t n265_sad_wh_sve(const pixel *a, int as, const pixel *b, int bs,
                         int w, int h);
uint64_t n265_ssd_wh_sve(const pixel *a, int as, const pixel *b, int bs,
                         int w, int h);
uint32_t n265_sum_sq_wh_sve(const pixel *p, int stride, int w, int h,
                            uint64_t *sq);
#endif

#if N265_SVE_MC
/* fy != 0 only; the copy forms stay on NEON */
#define N265_SVE_MCV_OK(f, w, h, fy) (N265_SVE_(f) && (fy) && \
    ((w) == 4 || (w) == 12 || (N265_SVE256_(f) && (w) == 8)))
#define N265_SVE_MCV14_OK(f, w, h, fy) (N265_SVE_(f) && (fy) && \
    ((w) == 4 || (w) == 12 || N265_SVE256_(f)))
void n265_mc_luma_hf_sve(pixel *dst, int dst_stride, int w, int h,
                         const int16_t *hf, int hf_stride, int fy);
void n265_mc_luma_14_hf_sve(int16_t *dst, int dst_stride, int w, int h,
                            const int16_t *hf, int hf_stride, int fy);
#endif

#endif /* N265_ARM_SVE_H */
