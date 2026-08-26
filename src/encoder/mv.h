/*
 * Motion vectors: merge candidate and AMVP derivation (8.5.3.2), motion
 * search. Shared between analysis (decisions) and nothing else - the
 * entropy pass replays recorded MVDs and never re-derives.
 * Copyright (c) 2026, the next265 authors. GPL-2.0-or-later; see LICENSE.
 */

#ifndef N265_MV_H
#define N265_MV_H

#include "encoder/encoder.h"

typedef struct mv_t {
    int16_t x, y;   /* quarter-pel units */
} mv_t;

/* full motion of a PU: per-list MVs and prediction flags */
typedef struct mot_t {
    mv_t mv[2];
    uint8_t pred;   /* bit0 = L0, bit1 = L1 */
} mot_t;

/* Merge candidate list (8.5.3.2.3, TMVP off): pruned spatials
 * A1,B1,B0,A0,B2, then combined bi-pred (B), then zero fill up to max.
 * Fills out[] and returns the count (== max after zero fill). */
int n265_merge_list(struct n265_encoder *e, int x0, int y0, int nw, int nh,
                    mot_t out[], int max);

/* out[0] of the list above */
mot_t n265_merge_cand0(struct n265_encoder *e, int x0, int y0, int nw,
                       int nh);

/* AMVP candidate list for L0 ref 0 (8.5.3.2.6): mvp[0], mvp[1]. */
void n265_amvp(struct n265_encoder *e, int x0, int y0, int nw, int nh,
               mv_t mvp[2]);

/* Full-pel motion search around the given start (quarter-pel in/out;
 * result is full-pel aligned). Returns best SAD. */
uint32_t n265_me_search(struct n265_encoder *e, int x0, int y0, int log2n,
                        mv_t start, mv_t *best);

/* SAD of luma MC prediction at mv (full-pel aligned). */
uint32_t n265_mc_sad(struct n265_encoder *e, int x0, int y0, int log2n,
                     mv_t mv);

#endif
