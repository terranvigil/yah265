/*
 * Intra prediction.
 * Copyright (c) 2026, the yah265 authors. GPL-2.0-or-later; see LICENSE.
 */

#ifndef N265_INTRA_H
#define N265_INTRA_H

#include <stdint.h>

#include "common/pixel.h"

/* HEVC intra mode numbers */
enum {
    INTRA_PLANAR = 0,
    INTRA_DC = 1,
    /* angular 2..34 */
};

/* z-scan order key at 4x4 granularity: CTB raster index in the high bits,
 * interleaved in-CTB coordinates below. pic_w is the padded width the CTB
 * grid spans. Decode order is increasing in this key, which is how
 * availability is decided. */
uint64_t n265_zorder(int x, int y, int pic_w, int ctb_log2);

/* Gather the 4n+1 reference samples for an NxN TB at plane position
 * (x0,y0), applying the unavailable-sample substitution (8.4.4.2.2).
 * Availability = inside the (padded) picture and earlier in z-scan
 * decode order than (x0,y0). refs layout: refs[0] = corner p[-1][-1],
 * refs[1..2n] = top row p[0..2n-1][-1], refs[2n+1..4n] = left column
 * p[-1][0..2n-1]. ctb_log2 defines the z-scan geometry. */
void n265_intra_refs(pixel *refs, const pixel *recon, int stride,
                     int x0, int y0, int log2n,
                     int pic_w, int pic_h, int ctb_log2);

/* DC prediction (8.4.4.2.5); filter_edges enables the luma <32x32 DC
 * boundary smoothing. */
void n265_pred_dc(pixel *dst, int dst_stride, const pixel *refs,
                  int log2n, int filter_edges);

/* [1 2 1] reference smoothing (8.4.4.2.3), strong smoothing excluded
 * (disabled in the SPS). in and out may not alias. */
void n265_filter_refs(pixel *out, const pixel *in, int log2n);

/* Whether mode/size/component uses the filtered references. */
int n265_use_filtered_refs(int mode, int log2n, int c_idx);

/* The C references the entry points above dispatch away from, and what
 * checkasm holds the NEON to. The entry points are a branch and a tail
 * call; these carry the spec text, so a reader after the algorithm reads
 * them and not the dispatcher. Keeping them exported is also what stops
 * the harness from becoming a tautology: with N265_ASM_OFF=intra the
 * entry point IS the reference, and a check written against the entry
 * point would then compare C against itself.
 *
 * n265_intra_refs is the exception - it has no NEON arm, because the win
 * over the spec's per-candidate availability test is a prefix search and
 * not a vector one. Its oracle is n265_intra_refs_spec below, which is
 * the spec walked literally and is called by nothing but checkasm. */
void n265_intra_refs_ref(pixel *refs, const pixel *recon, int stride,
                         int x0, int y0, int log2n,
                         int pic_w, int pic_h, int ctb_log2);
void n265_intra_refs_spec(pixel *refs, const pixel *recon, int stride,
                          int x0, int y0, int log2n,
                          int pic_w, int pic_h, int ctb_log2);
void n265_filter_refs_ref(pixel *out, const pixel *in, int log2n);
void n265_intra_pred_ref(pixel *dst, int dst_stride, const pixel *refs,
                         int log2n, int mode, int c_idx);

/* Any of the 35 modes; refs must already be the correct (un)filtered set.
 * c_idx gates the luma-only boundary filters. */
void n265_intra_pred(pixel *dst, int dst_stride, const pixel *refs,
                     int log2n, int mode, int c_idx);

#endif
