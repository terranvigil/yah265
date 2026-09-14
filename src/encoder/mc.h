/*
 * Motion-compensated interpolation (8.4.4.3.2/8.4.4.3.3).
 * Copyright (c) 2026, the yah265 authors. GPL-2.0-or-later; see LICENSE.
 */

#ifndef N265_MC_H
#define N265_MC_H

#include <stdint.h>

#include "common/pixel.h"

/* dst (w x h at dst_stride) from the integer-position ref pointer plus a
 * fractional phase: luma fx/fy in quarter-pel (0..3), chroma in
 * eighth-pel (0..7). The ref must be readable 3 above/left and 4
 * below/right of the block (padded planes guarantee it). Rectangular
 * blocks are the inter PU shapes (2NxN/Nx2N and the AMP quarters). */
void n265_mc_luma_wh(pixel *dst, int dst_stride, int w, int h,
                     const pixel *ref, int ref_stride, int fx, int fy);
void n265_mc_luma_wh_ref(pixel *dst, int dst_stride, int w, int h,
                         const pixel *ref, int ref_stride, int fx, int fy);
void n265_mc_chroma_wh(pixel *dst, int dst_stride, int w, int h,
                       const pixel *ref, int ref_stride, int fx, int fy);
void n265_mc_chroma_wh_ref(pixel *dst, int dst_stride, int w, int h,
                           const pixel *ref, int ref_stride, int fx,
                           int fy);

/* 14-bit intermediate variants for bi-prediction (no final rounding) */
void n265_mc_luma_14_wh(int16_t *dst, int dst_stride, int w, int h,
                        const pixel *ref, int ref_stride, int fx, int fy);
void n265_mc_luma_14_wh_ref(int16_t *dst, int dst_stride, int w, int h,
                            const pixel *ref, int ref_stride, int fx,
                            int fy);
void n265_mc_chroma_14_wh(int16_t *dst, int dst_stride, int w, int h,
                          const pixel *ref, int ref_stride, int fx, int fy);
void n265_mc_chroma_14_wh_ref(int16_t *dst, int dst_stride, int w, int h,
                              const pixel *ref, int ref_stride, int fx,
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
void n265_mc_luma_2d(pixel *dst, int dst_stride, int w, int h,
                     const pixel *p2, int p2_stride);
/* Round 4: the bi path's copy, out of the int16 plane holding the
 * UNROUNDED result n265_mc_luma_14_hf would have written. */
void n265_mc_luma_2d_14(int16_t *dst, int dst_stride, int w, int h,
                        const int16_t *p2, int p2_stride);

void n265_mc_luma_hf(pixel *dst, int dst_stride, int w, int h,
                     const pixel *ref, int ref_stride,
                     const int16_t *hf, int hf_stride, int fx, int fy);
void n265_mc_luma_14_hf(int16_t *dst, int dst_stride, int w, int h,
                        const pixel *ref, int ref_stride,
                        const int16_t *hf, int hf_stride, int fx, int fy);
/* the C references of the cached-first-stage forms, for the fx != 0 and
 * hf != NULL case only - the other case is the entry point's own fall
 * through to the plain form and never reaches a kernel. checkasm compares
 * these with the `_neon` twins directly rather than through the
 * dispatching entry point. */
void n265_mc_luma_hf_ref(pixel *dst, int dst_stride, int w, int h,
                         const int16_t *hf, int hf_stride, int fy);
void n265_mc_luma_14_hf_ref(int16_t *dst, int dst_stride, int w, int h,
                            const int16_t *hf, int hf_stride, int fy);

/* Build one phase-fx horizontal intermediate plane (w x h at dst_stride)
 * from the integer plane, which must be readable 3 left and 4 right of
 * every column written. */
void n265_mc_luma_hplane(int16_t *dst, int dst_stride, int w, int h,
                         const pixel *ref, int ref_stride, int fx);
void n265_mc_luma_hplane_ref(int16_t *dst, int dst_stride, int w, int h,
                             const pixel *ref, int ref_stride, int fx);

/* square forms (dst stride == n) */
void n265_mc_luma(pixel *dst, int n, const pixel *ref, int ref_stride,
                  int fx, int fy);
void n265_mc_chroma(pixel *dst, int n, const pixel *ref, int ref_stride,
                    int fx, int fy);
void n265_mc_luma_14(int16_t *dst, int n, const pixel *ref, int ref_stride,
                     int fx, int fy);
void n265_mc_chroma_14(int16_t *dst, int n, const pixel *ref,
                       int ref_stride, int fx, int fy);

/* 8.5.3.3.2's reference fetch, which clips every coordinate into the
 * picture. Our pictures carry a finite N265_REF_PAD border instead,
 * which is only the same thing while the read stays inside it: merge and
 * AMVP motion vectors are not clamped (only the search is), so a
 * candidate inherited from a neighbour can reach past it. Returns the
 * block origin the filter may read `hl` samples left/above and `hr`
 * right/below of - the plane itself when the window is inside the
 * border, else `tmp`, gathered with the spec's clipping. `tmp` holds
 * (w + hl + hr) * (h + hl + hr) SAMPLES; luma wants hl 3 / hr 4, chroma
 * hl 1 / hr 2. The border is given per AXIS: at 4:2:2 a chroma plane is
 * half as wide as the luma one and exactly as tall, so its two borders
 * differ and one number would have to be the smaller of them. */
const pixel *n265_ref_win(const pixel *plane, int stride, int pic_w,
                            int pic_h, int pad_x, int pad_y, int ix, int iy,
                            int w, int h, int hl, int hr, pixel *tmp,
                            int *out_stride, int *clipped);

/* the largest window n265_ref_win can gather: a 64x64 luma PU with the
 * 8-tap's 3-left/4-right halo. In SAMPLES since phase 8 item 1 - a
 * caller declares `pixel win[N265_REF_WIN_SAMPLES]` and the width comes
 * from the type. */
#define N265_REF_WIN_SAMPLES ((64 + 7) * (64 + 7))

/* default bi combine: (a + b + offset) >> (15 - BitDepth), clipped -
 * 64 and 7 at 8 bits. a/b are packed w x h. */
void n265_bi_avg_wh(pixel *dst, int dst_stride, const int16_t *a,
                    const int16_t *b, int w, int h);
/* the C reference of the above; checkasm compares `_ref` with `_neon`
 * directly rather than through the dispatching entry point */
void n265_bi_avg_wh_ref(pixel *dst, int dst_stride, const int16_t *a,
                        const int16_t *b, int w, int h);
void n265_bi_avg(pixel *dst, const int16_t *a, const int16_t *b, int n);

/* ---- 8.5.3.3.4.3, explicit weighted sample prediction --------------
 *
 * Both forms take the weight at the table's denominator (LumaWeightLX /
 * ChromaWeightLX, 7.4.7.3) and the offset in the syntax's own 8-bit
 * levels; the depth shifts - log2Wd = denom + (14 - BitDepth) and
 * WpOffsetBdShiftY - are applied here, so a caller passes what the
 * pred_weight_table carries and nothing else.
 *
 * The inputs are the 14-bit intermediates, NOT rounded samples: the spec
 * weights before the final rounding, and weighting a rounded sample is a
 * different number at every fractional position. `a` and `b` are packed
 * w x h, the shape the bi path already builds.
 *
 * At the identity (wt = 1 << denom, off = 0) both reduce EXACTLY to the
 * default forms - the uni rounding, and n265_bi_avg_wh - which is what
 * makes turning weighted_pred_flag on with an all-off table a no-op on
 * the samples rather than a rounding change. */
void n265_wp_uni_wh(pixel *dst, int ds, const int16_t *src, int w, int h,
                    int wt, int off, int denom);
void n265_wp_uni_wh_ref(pixel *dst, int ds, const int16_t *src, int w, int h,
                        int wt, int off, int denom);
void n265_wp_bi_wh(pixel *dst, int ds, const int16_t *a, const int16_t *b,
                   int w, int h, int w0, int o0, int w1, int o1, int denom);
void n265_wp_bi_wh_ref(pixel *dst, int ds, const int16_t *a, const int16_t *b,
                       int w, int h, int w0, int o0, int w1, int o1,
                       int denom);

/* The SEARCH's form: the same map applied to an already-rounded sample
 * block. It is EXACT at a full-pel position - shift1 cancels out of
 * ((s << shift1) * wt + (1 << (log2Wd - 1))) >> log2Wd - and differs
 * from the prediction only in the subpel rounding, which is the right
 * trade for a cost function: a motion search is allowed to be
 * approximate and a reconstruction is not. Using it keeps the search off
 * the 14-bit path, where every candidate would pay a second buffer and a
 * second pass. */
void n265_wp_pix_wh(pixel *dst, int ds, const pixel *src, int ss, int w,
                    int h, int wt, int off, int denom);
void n265_wp_pix_wh_ref(pixel *dst, int ds, const pixel *src, int ss, int w,
                        int h, int wt, int off, int denom);

#endif
