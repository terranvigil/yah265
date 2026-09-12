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

/* The geometry the AVAILABILITY test runs in, which is not always the
 * plane's own. 8.4.4.2.2 maps every chroma position UP by SubWidthC /
 * SubHeightC and invokes 6.4.1 on the luma pair, so the z-scan order is
 * a luma one.
 *
 * At 4:2:0 and 4:4:4 the chroma plane is a UNIFORM scaling of the luma
 * one, and a z-scan key taken in chroma coordinates orders chroma
 * positions exactly as the luma keys order their images (z-order is
 * hierarchical, and a whole 4x4-unit block's keys are contiguous, so
 * comparing the blocks by their first unit is the same comparison). Those
 * two formats therefore pass zero shifts and the plane's own geometry,
 * which is byte for byte the call the encoder made before 4:2:2 existed.
 *
 * 4:2:2 is where that breaks: the chroma CTB is half as wide as it is
 * tall, no chroma-space z-scan describes it, and the second of a TU's two
 * stacked chroma blocks has to see the first as available. So it passes
 * shx 1 / shy 0 with the LUMA pic_w and ctb_log2, and every candidate's
 * key is taken at (x << shx, y << shy). */
typedef struct n265_zgeo {
    int shx, shy;       /* log2 SubWidthC / SubHeightC of the mapping */
    int pic_w;          /* the z-scan's picture width, in ITS units */
    int ctb_log2;       /* ...and its CTB size */
} n265_zgeo;

/* n265_intra_refs with the availability geometry given apart from the
 * plane's. `pic_w` / `pic_h` still bound the PLANE (an out-of-picture
 * sample is unavailable whatever the z-scan says). */
void n265_intra_refs_g(pixel *refs, const pixel *recon, int stride,
                       int x0, int y0, int log2n, int pic_w, int pic_h,
                       const n265_zgeo *z);

/* DC prediction (8.4.4.2.5); filter_edges enables the luma <32x32 DC
 * boundary smoothing. */
void n265_pred_dc(pixel *dst, int dst_stride, const pixel *refs,
                  int log2n, int filter_edges);

/* [1 2 1] reference smoothing (8.4.4.2.3). in and out may not alias.
 *
 * `strong` arms the 32x32 bilinear rebuild (biIntFlag), whose conditions
 * include `cIdx == 0` at EVERY chroma format - so it is 1 for luma and 0
 * for chroma, and the argument exists because at 4:4:4 a chroma TB
 * reaches 32x32 and would otherwise fall into the luma branch. The [1 2 1]
 * run itself DOES apply to chroma at 4:4:4 (8.4.4.2.1 invokes the
 * filtering when `cIdx == 0 || ChromaArrayType == 3`), which is what the
 * caller's c_idx-for-filtering argument decides. */
void n265_filter_refs(pixel *out, const pixel *in, int log2n, int strong);

/* Whether mode/size/component uses the filtered references. `c_idx` here
 * is the FILTERING component index: 0 for luma and, at 4:4:4, 0 for
 * chroma too, since 8.4.4.2.1 gives chroma the luma process there. */
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
void n265_filter_refs_ref(pixel *out, const pixel *in, int log2n,
                          int strong);
void n265_intra_pred_ref(pixel *dst, int dst_stride, const pixel *refs,
                         int log2n, int mode, int c_idx);

/* Any of the 35 modes; refs must already be the correct (un)filtered set.
 * c_idx gates the luma-only boundary filters. */
void n265_intra_pred(pixel *dst, int dst_stride, const pixel *refs,
                     int log2n, int mode, int c_idx);

#endif
