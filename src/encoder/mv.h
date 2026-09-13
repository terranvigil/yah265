/*
 * Motion vectors: merge candidate and AMVP derivation (8.5.3.2), motion
 * search. Shared between analysis (decisions) and nothing else - the
 * entropy pass replays recorded MVDs and never re-derives.
 * Copyright (c) 2026, the yah265 authors. GPL-2.0-or-later; see LICENSE.
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
    int8_t ref[2];  /* reference index per list (docs/refstruct-design.md) */
} mot_t;

/* One prediction unit in its coding block. The CB is carried because
 * prediction-block availability (6.4.2) turns on whether a neighbour
 * lies inside the current coding block; everything else - the spatial
 * positions, both temporal positions - comes from the PU. */
typedef struct pu_geo {
    int cbx, cby, cbn;      /* coding block origin and size */
    int x, y, w, h;         /* prediction block */
    int excl_a1, excl_b1;   /* second-PU spatial merge exclusions */
} pu_geo;

/* Merge candidate list (8.5.3.2.2/8.5.3.2.3): pruned spatials
 * A1,B1,B0,A0,B2 (with the second-PU exclusions), the temporal
 * candidate, combined bi-pred (B), then zero fill up to max. A PU whose
 * w + h is 12 (8x4 / 4x8) has its bi candidates converted to L0
 * uni-prediction, per the last step of 8.5.3.2.1. Fills out[] and
 * returns the count (== max after zero fill). */
int n265_merge_list_pu(struct n265_frame_work *e, const pu_geo *g, mot_t out[],
                       int max);
int n265_merge_list(struct n265_frame_work *e, int x0, int y0, int nw, int nh,
                    mot_t out[], int max);

/* out[0] of the list above */
mot_t n265_merge_cand0(struct n265_frame_work *e, int x0, int y0, int nw,
                       int nh);

/* AMVP candidate list for the given reference of the given list
 * (8.5.3.2.6). No second-PU exclusions here: A1 of the second PU of a
 * vertical split, and B1 of the second PU of a horizontal split, are in
 * the same CB and available (6.4.2), so the first PU's motion must
 * already be in the maps. */
void n265_amvp_pu(struct n265_frame_work *e, int list, int ref,
                  const pu_geo *g, mv_t mvp[2]);
void n265_amvp(struct n265_frame_work *e, int list, int ref, int x0, int y0, int nw,
               int nh, mv_t mvp[2]);

/* The caller's rate model, handed to the subpel ladder under
 * N265_SUBPEL_RATE 2 and 3 (phase 10 item 1). `bits_q8` returns the
 * syntax cost of coding `mv` on the caller's list in Q8 bits, in the
 * caller's own model, so the ladder ranks in exactly the units the site
 * that owns the vector arbitrates in; `lam4` is that site's lambda_sad
 * at Q4 and the shift is the call sites' own (`bits * lam4 >> 12`).
 * NULL - and at N265_SUBPEL_RATE 0 or 1 - leaves the ladder pure SATD. */
struct n265_me_rate {
    uint32_t (*bits_q8)(void *ctx, mv_t mv);
    void *ctx;
    uint32_t lam4;
};

/* N265_SUBPEL_RATE (phase 10 item 1): where lambda x mvd reaches the
 * subpel vector. 0 off, 1 the post-search quarter-pel re-rank in ctu.c,
 * 2 the rate term inside the ladder's LAST round, 3 inside every round.
 * Defined in mv.c and read by ctu.c, which owns arm 1. */
int n265_subpel_rate(void);

/* Motion search on one reference list around the given start
 * (quarter-pel in/out). Returns the best cost. `rc` may be NULL. */
uint32_t n265_me_search(struct n265_frame_work *e, int list, int ridx, int x0, int y0,
                        int w, int h, mv_t start, mv_t *best,
                        const struct n265_me_rate *rc);

/* SAD of luma MC prediction at mv on one list. */
uint32_t n265_mc_sad(struct n265_frame_work *e, int list, int ridx, int x0, int y0,
                     int w, int h, mv_t mv);

/* Clamp a quarter-pel MV so the interpolation window of a w x h block at
 * (x0,y0) stays inside the padded reference. A caller that wants to
 * REJECT an out-of-range vector rather than move it compares before and
 * after. */
void n265_clamp_mv(struct n265_frame_work *e, int x0, int y0, int w, int h,
                   mv_t *mv);

#endif
