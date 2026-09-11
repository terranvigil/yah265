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
void n265_mc_luma_wh_ref(uint8_t *dst, int dst_stride, int w, int h,
                         const uint8_t *ref, int ref_stride, int fx, int fy);
void n265_mc_chroma_wh(uint8_t *dst, int dst_stride, int w, int h,
                       const uint8_t *ref, int ref_stride, int fx, int fy);
void n265_mc_chroma_wh_ref(uint8_t *dst, int dst_stride, int w, int h,
                           const uint8_t *ref, int ref_stride, int fx,
                           int fy);

/* 14-bit intermediate variants for bi-prediction (no final rounding) */
void n265_mc_luma_14_wh(int16_t *dst, int dst_stride, int w, int h,
                        const uint8_t *ref, int ref_stride, int fx, int fy);
void n265_mc_luma_14_wh_ref(int16_t *dst, int dst_stride, int w, int h,
                            const uint8_t *ref, int ref_stride, int fx,
                            int fy);
void n265_mc_chroma_14_wh(int16_t *dst, int dst_stride, int w, int h,
                          const uint8_t *ref, int ref_stride, int fx, int fy);
void n265_mc_chroma_14_wh_ref(int16_t *dst, int dst_stride, int w, int h,
                              const uint8_t *ref, int ref_stride, int fx,
                              int fy);

/* E1: the same two luma predictors served off a cached horizontal first
 * stage. `hf` points at the block origin inside the phase-fx
 * intermediate plane; NULL falls back to the plain form, which is what a
 * caller passes when the cache is off or cannot cover the block. The
 * intermediates are the identical int16 values the plain path builds, so
 * the output is bit-identical either way. `hf` is read from 3 rows above
 * to 4 rows below the block. */
/* Phase 6 round 3: the block is already in the reference's fy = 2 plane,
 * so the "filter" is a copy. Its own entry point rather than an argument
 * to n265_mc_luma_hf: the caller has already decided, and the hf entry
 * point is the hottest call in the encoder. */
void n265_mc_luma_2d(uint8_t *dst, int dst_stride, int w, int h,
                     const uint8_t *p2, int p2_stride);

void n265_mc_luma_hf(uint8_t *dst, int dst_stride, int w, int h,
                     const uint8_t *ref, int ref_stride,
                     const int16_t *hf, int hf_stride, int fx, int fy);
void n265_mc_luma_14_hf(int16_t *dst, int dst_stride, int w, int h,
                        const uint8_t *ref, int ref_stride,
                        const int16_t *hf, int hf_stride, int fx, int fy);
/* the C references of the cached-first-stage forms, for the fx != 0 and
 * hf != NULL case only - the other case is the entry point's own fall
 * through to the plain form and never reaches a kernel. checkasm compares
 * these with the `_neon` twins directly rather than through the
 * dispatching entry point. */
void n265_mc_luma_hf_ref(uint8_t *dst, int dst_stride, int w, int h,
                         const int16_t *hf, int hf_stride, int fy);
void n265_mc_luma_14_hf_ref(int16_t *dst, int dst_stride, int w, int h,
                            const int16_t *hf, int hf_stride, int fy);

/* Build one phase-fx horizontal intermediate plane (w x h at dst_stride)
 * from the integer plane, which must be readable 3 left and 4 right of
 * every column written. */
void n265_mc_luma_hplane(int16_t *dst, int dst_stride, int w, int h,
                         const uint8_t *ref, int ref_stride, int fx);
void n265_mc_luma_hplane_ref(int16_t *dst, int dst_stride, int w, int h,
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

/* 8.5.3.3.2's reference fetch, which clips every coordinate into the
 * picture. Our pictures carry a finite N265_REF_PAD border instead,
 * which is only the same thing while the read stays inside it: merge and
 * AMVP motion vectors are not clamped (only the search is), so a
 * candidate inherited from a neighbour can reach past it. Returns the
 * block origin the filter may read `hl` samples left/above and `hr`
 * right/below of - the plane itself when the window is inside the
 * border, else `tmp`, gathered with the spec's clipping. `tmp` holds
 * (w + hl + hr) * (h + hl + hr) bytes; luma wants hl 3 / hr 4, chroma
 * hl 1 / hr 2. */
const uint8_t *n265_ref_win(const uint8_t *plane, int stride, int pic_w,
                            int pic_h, int pad, int ix, int iy, int w,
                            int h, int hl, int hr, uint8_t *tmp,
                            int *out_stride, int *clipped);

/* the largest window n265_ref_win can gather: a 64x64 luma PU with the
 * 8-tap's 3-left/4-right halo */
#define N265_REF_WIN_BYTES ((64 + 7) * (64 + 7))

/* default bi combine: (a + b + 64) >> 7, clipped. a/b are packed w x h. */
void n265_bi_avg_wh(uint8_t *dst, int dst_stride, const int16_t *a,
                    const int16_t *b, int w, int h);
/* the C reference of the above; checkasm compares `_ref` with `_neon`
 * directly rather than through the dispatching entry point */
void n265_bi_avg_wh_ref(uint8_t *dst, int dst_stride, const int16_t *a,
                        const int16_t *b, int w, int h);
void n265_bi_avg(uint8_t *dst, const int16_t *a, const int16_t *b, int n);

#endif
