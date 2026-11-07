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
 * prediction-block availability (6.4.2) turns on whether a neighbor
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

/* Duplicate pairs within the merge list (phase 14 item 10). Returns
 * whether any pair of candidates is the same motion, and sets *dup34
 * for the MAX_MERGE-2 / MAX_MERGE-1 pair, the only pair whose two
 * merge_idx codings cost the same number of bits. */
int n265_merge_dups(struct n265_frame_work *e, const pu_geo *g, int *dup34);

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

/* N265_SUBPEL_GATE (phase 10 item 1b): percent. Where arm 1's re-rank
 * would have to BUY its SATDs, spend them only where the ladder's own
 * last square came out FLAT - its best two positions within X% of each
 * other. 0 is ungated. Read by ctu.c. */
int n265_subpel_gate(void);

/* N265_SUBME_RUNG (phase 11 item 2): the subpel ladder's own quarter-pel
 * rung - what the N265_SUBPEL_RATE=1 re-rank does, done inside
 * `n265_me_search` on the SATDs the ladder already holds. 0 off, 1/2/3
 * the three fire predicates, -1 the output-neutral SHADOW the census is
 * read in. Defined in mv.c; ctu.c reads it to decide whether the ladder
 * gets a rate model and whether its own re-rank still has work to do. */
int n265_subme_rung(void);

/* N265_RUNG_GATE (phase 11 item 2): signed percent, the same predicate
 * on the same square `N265_SUBPEL_GATE` reads. */
int n265_rung_gate(void);

/* N265_RUNG_RERANK (phase 11 item 2): what the post-search re-rank does
 * on a list pass the rung has already refined. 0 skips it (the speed
 * form), 1 keeps it (the quality ceiling). */
int n265_rung_rerank(void);

/* What the ladder's last square already knows, handed back so the
 * N265_SUBPEL_RATE=1 re-rank does not buy it a second time (phase 10
 * item 1b). The re-rank ranks the settled vector against its four
 * quarter-pel neighbors on luma SATD, and where that vector is still
 * the uni winner this ladder returned they are the same Hadamards of the
 * same block against the same reference: `mot_satd_wh` of a uni `mot_t`
 * is `mc_satd` of its list's vector, line for line.
 *
 * `satd` is the SATD at the RETURNED vector (UINT32_MAX under
 * N265_FULLPEL, where the ladder never takes one). `nb` is indexed in
 * the re-rank's own offset order - (1,0), (-1,0), (0,1), (0,-1) - and
 * holds what the ladder's final QUARTER-pel square costed there,
 * UINT32_MAX where the clamp dropped a position or the square never ran.
 * It may be read only when `nb_valid`, which says the square was centered
 * on the vector that came back - i.e. the square that did not move.
 *
 * `sq_best` and `sq_second` are the two smallest PURE Hadamards of that
 * last square whatever its step, center included - the flatness the gate
 * reads - and `sq_valid` says the square ranked in SATD and had a
 * runner-up to report. *
 * `rung_fired` says `N265_SUBME_RUNG` took its step on this pass, so on
 * a UNI pass in the same units the re-rank above would re-run the same
 * five positions to the same answer; `rung_mv` is the vector it chose,
 * carried separately because in SHADOW mode the ladder does not take
 * it. Both are zero when the rung is off. */
struct n265_me_keep {
    uint32_t satd;
    uint32_t nb[4];
    int nb_valid;
    uint32_t sq_best, sq_second;
    int sq_valid;
    int rung_fired;
    mv_t rung_mv;
};

/* Motion search on one reference list around the given start
 * (quarter-pel in/out). Returns the best cost. `keep` and `rc` may both
 * be NULL; passing `keep` costs four stores in the last square and
 * changes nothing the search decides. */
uint32_t n265_me_search(struct n265_frame_work *e, int list, int ridx, int x0, int y0,
                        int w, int h, mv_t start, mv_t *best,
                        struct n265_me_keep *keep,
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

/* POC-distance MV scaling (8.5.3.2.8), exposed for the MV-reuse census:
 * scale a vector found against a ref at temporal distance `td` to the ref
 * at distance `tb`. */
mv_t n265_mv_poc_scale(mv_t mv, int tb, int td);

#endif
