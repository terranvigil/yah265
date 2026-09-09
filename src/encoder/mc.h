/*
 * Motion-compensated interpolation (8.4.4.3.2/8.4.4.3.3).
 * Copyright (c) 2026, the yah265 authors. GPL-2.0-or-later; see LICENSE.
 */

#ifndef N265_MC_H
#define N265_MC_H

#include <stdint.h>

/* dst (w x h at dst_stride) from the integer-position ref pointer plus a
 * fractional phase: luma fx/fy in quarter-pel (0..3), chroma in
 * eighth-pel (0..7). The ref must be readable 3 above/left and 4
 * below/right of the block (padded planes guarantee it). Rectangular
 * blocks are the inter PU shapes (2NxN/Nx2N and the AMP quarters). */
void n265_mc_luma_wh(uint8_t *dst, int dst_stride, int w, int h,
                     const uint8_t *ref, int ref_stride, int fx, int fy);
void n265_mc_chroma_wh(uint8_t *dst, int dst_stride, int w, int h,
                       const uint8_t *ref, int ref_stride, int fx, int fy);

/* 14-bit intermediate variants for bi-prediction (no final rounding) */
void n265_mc_luma_14_wh(int16_t *dst, int dst_stride, int w, int h,
                        const uint8_t *ref, int ref_stride, int fx, int fy);
void n265_mc_chroma_14_wh(int16_t *dst, int dst_stride, int w, int h,
                          const uint8_t *ref, int ref_stride, int fx, int fy);

/* E1: the same two luma predictors served off a cached horizontal first
 * stage. `hf` points at the block origin inside the phase-fx
 * intermediate plane; NULL falls back to the plain form, which is what a
 * caller passes when the cache is off or cannot cover the block. The
 * intermediates are the identical int16 values the plain path builds, so
 * the output is bit-identical either way. `hf` is read from 3 rows above
 * to 4 rows below the block. */
void n265_mc_luma_hf(uint8_t *dst, int dst_stride, int w, int h,
                     const uint8_t *ref, int ref_stride,
                     const int16_t *hf, int hf_stride, int fx, int fy);
void n265_mc_luma_14_hf(int16_t *dst, int dst_stride, int w, int h,
                        const uint8_t *ref, int ref_stride,
                        const int16_t *hf, int hf_stride, int fx, int fy);

/* Build one phase-fx horizontal intermediate plane (w x h at dst_stride)
 * from the integer plane, which must be readable 3 left and 4 right of
 * every column written. */
void n265_mc_luma_hplane(int16_t *dst, int dst_stride, int w, int h,
                         const uint8_t *ref, int ref_stride, int fx);

/* square forms (dst stride == n) */
void n265_mc_luma(uint8_t *dst, int n, const uint8_t *ref, int ref_stride,
                  int fx, int fy);
void n265_mc_chroma(uint8_t *dst, int n, const uint8_t *ref, int ref_stride,
                    int fx, int fy);
void n265_mc_luma_14(int16_t *dst, int n, const uint8_t *ref, int ref_stride,
                     int fx, int fy);
void n265_mc_chroma_14(int16_t *dst, int n, const uint8_t *ref,
                       int ref_stride, int fx, int fy);

/* default bi combine: (a + b + 64) >> 7, clipped. a/b are packed w x h. */
void n265_bi_avg_wh(uint8_t *dst, int dst_stride, const int16_t *a,
                    const int16_t *b, int w, int h);
void n265_bi_avg(uint8_t *dst, const int16_t *a, const int16_t *b, int n);

#endif
