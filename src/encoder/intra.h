/*
 * Intra prediction.
 * Copyright (c) 2026, the next265 authors. GPL-2.0-or-later; see LICENSE.
 */

#ifndef N265_INTRA_H
#define N265_INTRA_H

#include <stdint.h>

/* HEVC intra mode numbers */
enum {
    INTRA_PLANAR = 0,
    INTRA_DC = 1,
    /* angular 2..34 */
};

/* Gather the 4n+1 reference samples for an NxN TB at plane position
 * (x0,y0), applying the unavailable-sample substitution (8.4.4.2.2).
 * Availability = inside the (padded) picture and earlier in z-scan
 * decode order than (x0,y0). refs layout: refs[0] = corner p[-1][-1],
 * refs[1..2n] = top row p[0..2n-1][-1], refs[2n+1..4n] = left column
 * p[-1][0..2n-1]. ctb_log2 defines the z-scan geometry. */
void n265_intra_refs(uint8_t *refs, const uint8_t *recon, int stride,
                     int x0, int y0, int log2n,
                     int pic_w, int pic_h, int ctb_log2);

/* DC prediction (8.4.4.2.5); filter_edges enables the luma <32x32 DC
 * boundary smoothing. */
void n265_pred_dc(uint8_t *dst, int dst_stride, const uint8_t *refs,
                  int log2n, int filter_edges);

#endif
