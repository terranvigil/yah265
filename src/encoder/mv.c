/*
 * Merge/AMVP derivation and full-pel motion search.
 * Copyright (c) 2026, the yah265 authors. GPL-2.0-or-later; see LICENSE.
 */

#include <math.h>
#include <stdio.h>
#include <stdlib.h>

#include "encoder/mc.h"
#include "encoder/mv.h"
#include "encoder/refchk.h"
#include "encoder/satd.h"
#include "encoder/preset.h"

/* is the 4x4 cell holding (x,y) an already-decoded inter block?
 * Prediction-block availability (6.4.2): a neighbour inside the CURRENT
 * coding block skips the z-scan test entirely - it is the first PU of a
 * two-PU split, already derived and stored. Outside the CB the z-scan
 * order decides (6.4.1). */
static int inter_at_cb(struct n265_frame_work *e, int x, int y, uint64_t cur_z,
                       const pu_geo *g)
{
    if (x < 0 || y < 0 || x >= e->pad_w || y >= e->pad_h)
        return 0;
    int same_cb = g && x >= g->cbx && x < g->cbx + g->cbn &&
                  y >= g->cby && y < g->cby + g->cbn;
    if (!same_cb && n265_zorder(x, y, e->pad_w, e->ctb_log2) >= cur_z)
        return 0;
    /* decoded and not intra => inter */
    return !e->blk_intra[(y >> 2) * e->map_stride + (x >> 2)];
}

static mot_t mot_at(struct n265_frame_work *e, int x, int y)
{
    int m = (y >> 2) * e->map_stride + (x >> 2);
    mot_t r;
    r.mv[0].x = e->blk_mv[4 * m];
    r.mv[0].y = e->blk_mv[4 * m + 1];
    r.mv[1].x = e->blk_mv[4 * m + 2];
    r.mv[1].y = e->blk_mv[4 * m + 3];
    r.pred = e->blk_pred[m];
    r.ref[0] = e->blk_ref[2 * m];
    r.ref[1] = e->blk_ref[2 * m + 1];
    return r;
}

static int same_motion(const mot_t *a, const mot_t *b)
{
    if (a->pred != b->pred)
        return 0;
    if ((a->pred & 1) && a->ref[0] != b->ref[0])
        return 0;
    if ((a->pred & 2) && a->ref[1] != b->ref[1])
        return 0;
    if ((a->pred & 1) && (a->mv[0].x != b->mv[0].x ||
                          a->mv[0].y != b->mv[0].y))
        return 0;
    if ((a->pred & 2) && (a->mv[1].x != b->mv[1].x ||
                          a->mv[1].y != b->mv[1].y))
        return 0;
    return 1;
}

/* POC-distance MV scaling (8.5.3.2.8 shape) */
static mv_t mv_scale(mv_t mv, int tb, int td)
{
    if (td == 0 || tb == td)
        return mv;
    int tx = (16384 + (td < 0 ? -td : td) / 2) / td;
    int dsf = (tb * tx + 32) >> 6;
    dsf = dsf < -4096 ? -4096 : dsf > 4095 ? 4095 : dsf;
    mv_t r;
    int v = dsf * mv.x;
    v = (v >= 0 ? (v + 127) >> 8 : -((-v + 127) >> 8));
    r.x = (int16_t)(v < -32768 ? -32768 : v > 32767 ? 32767 : v);
    v = dsf * mv.y;
    v = (v >= 0 ? (v + 127) >> 8 : -((-v + 127) >> 8));
    r.y = (int16_t)(v < -32768 ? -32768 : v > 32767 ? 32767 : v);
    return r;
}

/* temporal MV prediction for target list X ref 0 (8.5.3.2.8): C0
 * (bottom-right, same CTB row) else C1 (center) in the collocated
 * picture (always l0[0], collocated_from_l0 == 1), listCol per
 * NoBackwardPredFlag, POC-distance scaling. */
/* Cached knobs at file scope so n265_mv_warm() can initialize them on
 * the driver before any worker exists (T0 audit item 2). */
static int no_tmvp_on(void)
{
    static int v = -1;
    if (v < 0) v = n265_env("N265_NO_TMVP") != NULL;
    return v;
}
static int merge_trace_disp(void)
{
    static int v = -2;
    if (v == -2) { const char *s = n265_env("N265_MERGE_TRACE"); v = s ? atoi(s) : -1; }
    return v;
}
static int fullpel_on(void)
{
    static int v = -1;
    if (v < 0) v = n265_env("N265_FULLPEL") != NULL;
    return v;
}
static int subpel_dedup(void)
{
    static int v = -1;
    if (v < 0) { const char *s = n265_env("N265_SUBPEL_DEDUP"); v = s ? atoi(s) != 0 : 1; }
    return v;
}

static int tmvp_mv(struct n265_frame_work *e, const pu_geo *g,
                   int list, int ref, mv_t *out)
{
    int no_tmvp = no_tmvp_on();
    /* D2: the collocated picture and the POC it scales against were
     * frozen at dispatch. The coding path must not read a live DPB
     * slot's marking - a later frame's dispatch retires pictures while
     * this one is still analysing. */
    const struct n265_refpic *col = e->col;
    if (!col || no_tmvp)
        return 0;
    int c16w = (e->pad_w + 15) >> 4;
    int i16 = -1;
    /* bottom-right: the PREDICTION block's corner, gated on the
     * prediction block's CTB row (8.5.3.2.8). Verified against ffmpeg
     * and libde265 on rect PUs: taking the CODING block's corner (both
     * PUs of a split sharing one C0) mismatches both decoders. */
    int xbr = g->x + g->w, ybr = g->y + g->h;
    if (xbr < e->pad_w && ybr < e->pad_h &&
        (g->y >> e->ctb_log2) == (ybr >> e->ctb_log2)) {
        /* G2 entry point: the collocated MV field is written by the
         * publish task of the row the cell lies in */
        n265_ref_wait(e, col, ybr);
        int t = (ybr >> 4) * c16w + (xbr >> 4);
        /* the check computes the owning CTB row from the CELL index, not
         * from the line ref_wait was given */
        N265_SIDE_CHECK(e, col, e->col_gen,
                        (ybr >> 4) >> (e->ctb_log2 - 4), "tmvp C0");
        if (col->col_pred[t])
            i16 = t;
    }
    if (i16 < 0) {      /* centre: the prediction block's */
        int xc = g->x + g->w / 2, yc = g->y + g->h / 2;
        n265_ref_wait(e, col, yc);
        int t = (yc >> 4) * c16w + (xc >> 4);
        N265_SIDE_CHECK(e, col, e->col_gen,
                        (yc >> 4) >> (e->ctb_log2 - 4), "tmvp C1");
        if (col->col_pred[t])
            i16 = t;
    }
    if (i16 < 0)
        return 0;
    int pf = col->col_pred[i16];
    int lc;
    if (!(pf & 1))
        lc = 1;
    else if (!(pf & 2))
        lc = 0;
    else if (e->slice_type != 0)
        lc = list;      /* NoBackwardPredFlag: all refs precede us */
    else
        lc = 1;         /* N = collocated_from_l0_flag */
    mv_t v = { col->col_mv[4 * i16 + 2 * lc],
               col->col_mv[4 * i16 + 2 * lc + 1] };
    int td = e->col_poc - col->col_ref[2 * i16 + lc];
    /* a live DPB slot's poc, read from the coding path */
    N265_SIDE_CHECK(e, e->refl[list][ref], e->ref_gen[list][ref], -1,
                    "tmvp target ref poc");
    int tb = e->poc - e->refl[list][ref]->poc;
    td = td < -128 ? -128 : td > 127 ? 127 : td;
    tb = tb < -128 ? -128 : tb > 127 ? 127 : tb;
    *out = mv_scale(v, tb, td);
    return 1;
}

int n265_merge_list_pu(struct n265_frame_work *e, const pu_geo *g, mot_t out[],
                       int max)
{
    int x0 = g->x, y0 = g->y, nw = g->w, nh = g->h;
    uint64_t cur = n265_zorder(x0, y0, e->pad_w, e->ctb_log2);
    /* spatial positions in list order: A1, B1, B0, A0, B2 */
    const int pos[5][2] = {
        { x0 - 1, y0 + nh - 1 },
        { x0 + nw - 1, y0 - 1 },
        { x0 + nw, y0 - 1 },
        { x0 - 1, y0 + nh },
        { x0 - 1, y0 - 1 },
    };
    int avail[5];
    mot_t m[5];
    for (int i = 0; i < 5; i++) {
        avail[i] = inter_at_cb(e, pos[i][0], pos[i][1], cur, g);
        if (avail[i])
            m[i] = mot_at(e, pos[i][0], pos[i][1]);
    }
    /* 8.5.3.2.2: the second PU of a vertical split cannot take A1, the
     * second PU of a horizontal split cannot take B1 - either would make
     * the split degenerate into the single motion it was meant to beat */
    if (g->excl_a1)
        avail[0] = 0;
    if (g->excl_b1)
        avail[1] = 0;
    /* pairwise pruning is between POSITIONS (8.5.3.2.3), not between
     * list entries: B1~A1, B0~B1, A0~A1, B2~A1 and B2~B1 */
    int n = 0;
    if (avail[0])
        out[n++] = m[0];
    if (n < max && avail[1] && !(avail[0] && same_motion(&m[1], &m[0])))
        out[n++] = m[1];
    if (n < max && avail[2] && !(avail[1] && same_motion(&m[2], &m[1])))
        out[n++] = m[2];
    if (n < max && avail[3] && !(avail[0] && same_motion(&m[3], &m[0])))
        out[n++] = m[3];
    if (n < max && n < 4 && avail[4] &&
        !(avail[0] && same_motion(&m[4], &m[0])) &&
        !(avail[1] && same_motion(&m[4], &m[1])))
        out[n++] = m[4];
    /* temporal candidate (8.5.3.2.1 step: after the spatials) */
    if (n < max) {
        mot_t t = { { { 0, 0 }, { 0, 0 } }, 0 };
        mv_t v;
        if (tmvp_mv(e, g, 0, 0, &v)) {
            t.mv[0] = v;
            t.pred |= 1;
        }
        if (e->slice_type == 0 && tmvp_mv(e, g, 1, 0, &v)) {
            t.mv[1] = v;
            t.pred |= 2;
        }
        if (t.pred)
            out[n++] = t;
    }

    /* combined bi-predictive candidates (8.5.3.2.4), B slices, from the
     * first candidate pairs (l0 of i, l1 of j), order (0,1),(1,0),... */
    if (e->slice_type == 0 && n > 1 && n < max) {
        static const int pr[12][2] = {
            {0,1},{1,0},{0,2},{2,0},{1,2},{2,1},
            {0,3},{3,0},{1,3},{3,1},{2,3},{3,2}
        };
        int spat = n;
        for (int k = 0; k < 12 && n < max; k++) {
            int i = pr[k][0], j = pr[k][1];
            if (i >= spat || j >= spat)
                continue;
            if (!(out[i].pred & 1) || !(out[j].pred & 2))
                continue;
            /* 8.5.3.2.4: added when the two reference pictures differ or
             * the two motion vectors differ */
            N265_SIDE_CHECK(e, e->refl[0][out[i].ref[0]],
                            e->ref_gen[0][out[i].ref[0]], -1, "bi-prune poc");
            N265_SIDE_CHECK(e, e->refl[1][out[j].ref[1]],
                            e->ref_gen[1][out[j].ref[1]], -1, "bi-prune poc");
            int p0 = e->refl[0][out[i].ref[0]]->poc, p1 = e->refl[1][out[j].ref[1]]->poc;
            if (p0 == p1 && out[i].mv[0].x == out[j].mv[1].x && out[i].mv[0].y == out[j].mv[1].y)
                continue;
            mot_t c;
            c.mv[0] = out[i].mv[0];
            c.mv[1] = out[j].mv[1];
            c.pred = 3;
            c.ref[0] = out[i].ref[0];
            c.ref[1] = out[j].ref[1];
            out[n++] = c;
        }
    }
    /* zero candidates (8.5.3.2.5): uni in P, bi in B, the reference index
     * walking 0.. numRefIdx-1 then staying at 0 */
    int num_ref = e->slice_type == 0 ? (e->nref[0] < e->nref[1] ? e->nref[0] : e->nref[1]) : e->nref[0];
    for (int zi = 0; n < max; zi++) {
        int r = zi < num_ref ? zi : 0;
        out[n].mv[0].x = out[n].mv[0].y = 0;
        out[n].mv[1].x = out[n].mv[1].y = 0;
        out[n].pred = e->slice_type == 0 ? 3 : 1;
        out[n].ref[0] = (int8_t)r;
        out[n].ref[1] = (int8_t)(e->slice_type == 0 ? r : 0);
        n++;
    }
    /* 8.5.3.2.1, last step: an 8x4 or 4x8 PU may not be bi-predicted, so
     * every bi candidate collapses to its L0 half */
    if (nw + nh == 12)
        for (int i = 0; i < n; i++)
            if (out[i].pred == 3) {
                out[i].pred = 1;
                out[i].mv[1].x = out[i].mv[1].y = 0;
                out[i].ref[1] = 0;
            }
    int trace_disp = merge_trace_disp();
    if (trace_disp >= 0 && e->display_idx == trace_disp) {
        fprintf(stderr, "MRG disp=%d cu=(%d,%d,%dx%d) type=%d nref=%d/%d:", e->display_idx, x0, y0, nw, nh, e->slice_type, e->nref[0], e->nref[1]);
        for (int i = 0; i < n; i++) fprintf(stderr, " %d:p%d r%d/%d (%d,%d|%d,%d)", i, out[i].pred, out[i].ref[0], out[i].ref[1], out[i].mv[0].x, out[i].mv[0].y, out[i].mv[1].x, out[i].mv[1].y);
        fprintf(stderr, "\n");
    }
    return n;
}

int n265_merge_list(struct n265_frame_work *e, int x0, int y0, int nw, int nh,
                    mot_t out[], int max)
{
    pu_geo g = { x0, y0, nw, x0, y0, nw, nh, 0, 0 };
    return n265_merge_list_pu(e, &g, out, max);
}

mot_t n265_merge_cand0(struct n265_frame_work *e, int x0, int y0, int nw, int nh)
{
    mot_t out[1];
    n265_merge_list(e, x0, y0, nw, nh, out, 1);
    return out[0];
}

/* neighbor take for AMVP (8.5.3.2.7): unscaled pass wants a ref with
 * the target's POC (same list first, then the other); the scaled pass
 * takes the first motion present (same list first) and scales it to
 * the target distance. One ref per list, all short-term. */
static int amvp_take(struct n265_frame_work *e, int m, int list, int ref, int scaled,
                     mv_t *out)
{
    N265_SIDE_CHECK(e, e->refl[list][ref], e->ref_gen[list][ref], -1,
                    "amvp target poc");
    int target_poc = e->refl[list][ref]->poc;
    for (int p = 0; p < 2; p++) {
        int ly = p ? !list : list;
        if (!(e->blk_pred[m] & (1 << ly)))
            continue;
        N265_SIDE_CHECK(e, e->refl[ly][e->blk_ref[2 * m + ly]],
                        e->ref_gen[ly][e->blk_ref[2 * m + ly]], -1,
                        "amvp neighbour poc");
        int nb_poc = e->refl[ly][e->blk_ref[2 * m + ly]]->poc;
        int same = nb_poc == target_poc;
        if (!scaled && !same)
            continue;
        mv_t v = { e->blk_mv[4 * m + 2 * ly],
                   e->blk_mv[4 * m + 2 * ly + 1] };
        if (scaled && !same) {
            int td = e->poc - nb_poc;
            int tb = e->poc - target_poc;
            td = td < -128 ? -128 : td > 127 ? 127 : td;
            tb = tb < -128 ? -128 : tb > 127 ? 127 : tb;
            v = mv_scale(v, tb, td);
        }
        *out = v;
        return 1;
    }
    return 0;
}

void n265_amvp_pu(struct n265_frame_work *e, int list, int ref,
                  const pu_geo *g, mv_t mvp[2])
{
    int x0 = g->x, y0 = g->y, nw = g->w, nh = g->h;
    uint64_t cur = n265_zorder(x0, y0, e->pad_w, e->ctb_log2);
    const int a_pos[2][2] = { { x0 - 1, y0 + nh }, { x0 - 1, y0 + nh - 1 } };
    const int b_pos[3][2] = {
        { x0 + nw, y0 - 1 }, { x0 + nw - 1, y0 - 1 }, { x0 - 1, y0 - 1 }
    };
    int a_avail[2], a_m[2];
    for (int i = 0; i < 2; i++) {
        a_avail[i] = inter_at_cb(e, a_pos[i][0], a_pos[i][1], cur, g);
        a_m[i] = (a_pos[i][1] >> 2) * e->map_stride + (a_pos[i][0] >> 2);
    }
    int is_scaled = a_avail[0] || a_avail[1];

    /* candidate A: unscaled pass over A0, A1; then the scaled pass */
    mv_t mva = { 0, 0 }, mvb = { 0, 0 };
    int avail_a = 0, avail_b = 0;
    for (int i = 0; i < 2 && !avail_a; i++)
        if (a_avail[i] && amvp_take(e, a_m[i], list, ref, 0, &mva))
            avail_a = 1;
    for (int i = 0; i < 2 && !avail_a; i++)
        if (a_avail[i] && amvp_take(e, a_m[i], list, ref, 1, &mva))
            avail_a = 1;

    /* candidate B: unscaled over B0, B1, B2 */
    for (int i = 0; i < 3 && !avail_b; i++) {
        int bx = b_pos[i][0], by = b_pos[i][1];
        if (inter_at_cb(e, bx, by, cur, g) &&
            amvp_take(e, (by >> 2) * e->map_stride + (bx >> 2), list, ref, 0,
                      &mvb))
            avail_b = 1;
    }
    /* neither A position inter: unscaled B is promoted to the A slot
     * and B re-derives with scaling allowed */
    if (!is_scaled) {
        avail_a = avail_b;
        mva = mvb;
        avail_b = 0;
        for (int i = 0; i < 3 && !avail_b; i++) {
            int bx = b_pos[i][0], by = b_pos[i][1];
            if (inter_at_cb(e, bx, by, cur, g) &&
                amvp_take(e, (by >> 2) * e->map_stride + (bx >> 2), list,
                          ref, 1, &mvb))
                avail_b = 1;
        }
    }

    int n = 0;
    if (avail_a)
        mvp[n++] = mva;
    if (avail_b && !(avail_a && mvb.x == mva.x && mvb.y == mva.y))
        mvp[n++] = mvb;
    if (n < 2) {
        mv_t v;
        if (tmvp_mv(e, g, list, ref, &v))
            mvp[n++] = v;
    }
    while (n < 2) {
        mvp[n].x = 0;
        mvp[n].y = 0;
        n++;
    }
}

void n265_amvp(struct n265_frame_work *e, int list, int ref, int x0, int y0,
               int nw, int nh, mv_t mvp[2])
{
    pu_geo g = { x0, y0, nw, x0, y0, nw, nh, 0, 0 };
    n265_amvp_pu(e, list, ref, &g, mvp);
}

uint32_t n265_mc_sad(struct n265_frame_work *e, int list, int ridx, int x0, int y0,
                     int w, int h, mv_t mv)
{
    const struct n265_refpic *ref = e->refl[list][ridx];
    const uint8_t *src = e->pad_plane[0] + y0 * e->pad_stride[0] + x0;
    if ((mv.x | mv.y) & 3) {    /* fractional: interpolate then SAD */
        uint8_t pred[64 * 64];
        int ix = x0 + (mv.x >> 2), iy = y0 + (mv.y >> 2);
        /* G2 entry point: the 8-tap reaches 4 lines below the block, and
         * n265_ref_hf's own coverage test wants the same line, so one
         * bound serves both arms. */
        int need = n265_ref_wait(e, ref, iy + h + 4);
        const int16_t *hf = n265_ref_hf(ref, ix, iy, w, h, mv.x & 3);
        const uint8_t *p2 = n265_ref_2d(ref, ix, iy, w, h, mv.x & 3,
                                        mv.y & 3);
        n265_hf2_tally(e, ref, ix, iy, w, h, mv.x & 3, mv.y & 3, 0);
        N265_REF_GUARD2(e, ref, need, 0, n265_ref_hf_base(ref, mv.x & 3),
                        n265_ref_2d_base(ref, mv.x & 3, mv.y & 3));
        if (p2) {
            n265_mc_luma_2d(pred, w, w, h, p2, ref->hf2_stride);
        } else {
            uint8_t win[N265_REF_WIN_BYTES];
            int ws, wclip;
            const uint8_t *rp = n265_ref_win(ref->plane[0], ref->stride[0],
                                             e->pad_w, e->pad_h,
                                             N265_REF_PAD, ix, iy, w, h,
                                             3, 4, win, &ws, &wclip);
            n265_mc_luma_hf(pred, w, w, h, rp, ws, wclip ? NULL : hf,
                            ref->hf_stride, mv.x & 3, mv.y & 3);
        }
        N265_REF_UNGUARD();
        return n265_sad_wh(src, e->pad_stride[0], pred, w, w, h);
    }
    int ix = x0 + (mv.x >> 2), iy = y0 + (mv.y >> 2);
    int need = n265_ref_wait(e, ref, iy + h - 1);
    N265_REF_GUARD(e, ref, need, 0, NULL);
    uint8_t win[N265_REF_WIN_BYTES];
    int ws, wclip;
    const uint8_t *rp = n265_ref_win(ref->plane[0], ref->stride[0],
                                     e->pad_w, e->pad_h, N265_REF_PAD,
                                     ix, iy, w, h, 0, 0, win, &ws, &wclip);
    N265_RC_TOUCH(rp);
    N265_RC_TOUCH(rp + (h - 1) * ws);
    N265_REF_UNGUARD();
    return n265_sad_wh(src, e->pad_stride[0], rp, ws, w, h);
}

/* SATD of luma MC at mv: phase-aware cost for subpel refinement, where
 * SAD cannot tell interpolation blur from misalignment */
static uint32_t mc_satd(struct n265_frame_work *e, int list, int ridx, int x0, int y0,
                        int w, int h, mv_t mv)
{
    const struct n265_refpic *ref = e->refl[list][ridx];
    const uint8_t *src = e->pad_plane[0] + y0 * e->pad_stride[0] + x0;
    uint8_t pred[64 * 64];
    int ix = x0 + (mv.x >> 2), iy = y0 + (mv.y >> 2);
    int need = n265_ref_wait(e, ref, iy + h + 4);   /* G2 entry point */
    const int16_t *hf = n265_ref_hf(ref, ix, iy, w, h, mv.x & 3);
    const uint8_t *p2 = n265_ref_2d(ref, ix, iy, w, h, mv.x & 3, mv.y & 3);
    n265_hf2_tally(e, ref, ix, iy, w, h, mv.x & 3, mv.y & 3, 0);
    N265_REF_GUARD2(e, ref, need, 0, n265_ref_hf_base(ref, mv.x & 3),
                    n265_ref_2d_base(ref, mv.x & 3, mv.y & 3));
    if (p2) {
        n265_mc_luma_2d(pred, w, w, h, p2, ref->hf2_stride);
    } else {
        uint8_t win[N265_REF_WIN_BYTES];
        int ws, wclip;
        const uint8_t *rp = n265_ref_win(ref->plane[0], ref->stride[0],
                                         e->pad_w, e->pad_h, N265_REF_PAD,
                                         ix, iy, w, h, 3, 4, win, &ws,
                                         &wclip);
        n265_mc_luma_hf(pred, w, w, h, rp, ws, wclip ? NULL : hf,
                        ref->hf_stride, mv.x & 3, mv.y & 3);
    }
    N265_REF_UNGUARD();
    return n265_satd_wh(src, e->pad_stride[0], pred, w, w, h);
}

/* clamp a full-pel mv so MC stays inside the padded reference */
void n265_clamp_mv(struct n265_frame_work *e, int x0, int y0, int w, int h,
                   mv_t *mv)
{
    int lo_x = -(x0 + N265_REF_PAD - 8) * 4;
    int hi_x = (e->pad_w + N265_REF_PAD - 8 - (x0 + w)) * 4;
    int lo_y = -(y0 + N265_REF_PAD - 8) * 4;
    int hi_y = (e->pad_h + N265_REF_PAD - 8 - (y0 + h)) * 4;
    if (mv->x < lo_x) mv->x = (int16_t)lo_x;
    if (mv->x > hi_x) mv->x = (int16_t)hi_x;
    if (mv->y < lo_y) mv->y = (int16_t)lo_y;
    if (mv->y > hi_y) mv->y = (int16_t)hi_y;
}

/* N265_SUBME (phase 4): how much of the subpel ladder each block gets.
 * 0 is the two-round half-pel/quarter-pel square this encoder has had
 * since phase 2.
 *
 * Still 0 after phase 6b, and the reason is not the band. Mode 3 is
 * 1.113x inside the placed medium and the CIF-7 median stays well
 * inside the budget with it, but on the fifteen-clip corpus it takes
 * `sintel_720p` from -0.47% to +0.67% BD-VMAF-NEG - a clip that is
 * ahead of x265 today ending up behind it, which the placement's budget
 * forbids outright. Its joint cost also exceeds its solo cost by about
 * two thirds of a point in each mode (docs/phase6b-economy.md,
 * point B). `--preset fast` still takes it. */
static int subme_mode(void)
{
    static int v = -1;
    if (v < 0) {
        const char *s = n265_env("N265_SUBME");
        v = s ? atoi(s) : 0;
    }
    return v;
}

/* N265_ME_PATTERN (phase 4): 0 the unit diamond, 1 the hexagon with a
 * diamond finish, 2 and 3 that plus an early-out on the seed. */
static int me_pattern(void)
{
    static int v = -1;
    if (v < 0) {
        const char *s = n265_env("N265_ME_PATTERN");
        v = s ? atoi(s) : 0;
    }
    return v;
}

/* quantiser step at qp, Q8: the unit the early-out bound is asked in,
 * because a prediction already inside the quantiser floor leaves the
 * pattern nothing to find */
static uint32_t qstep_q8(int qp)
{
    static uint32_t tab[52];
    if (!tab[51])
        for (int q = 0; q < 52; q++)
            tab[q] = (uint32_t)(pow(2.0, (q - 4) / 6.0) * 256.0 + 0.5);
    return tab[qp < 0 ? 0 : qp > 51 ? 51 : qp];
}

uint32_t n265_me_search(struct n265_frame_work *e, int list, int ridx, int x0, int y0,
                        int w, int h, mv_t start, mv_t *best)
{
    mv_t cur = { (int16_t)((start.x >> 2) * 4), (int16_t)((start.y >> 2) * 4) };
    n265_clamp_mv(e, x0, y0, w, h, &cur);
    uint32_t best_sad = n265_mc_sad(e, list, ridx, x0, y0, w, h, cur);
    /* the zero probe, unless the seed already is zero - costing the
     * same block twice cannot change the answer */
    if (cur.x || cur.y) {
        mv_t zero = { 0, 0 };
        uint32_t zs = n265_mc_sad(e, list, ridx, x0, y0, w, h, zero);
        if (zs < best_sad) {
            best_sad = zs;
            cur = zero;
        }
    }
    static const int dia4[4][2] = { {4,0}, {-4,0}, {0,4}, {0,-4} };
    int pattern = me_pattern();
    /* Early-out: a seed whose prediction is already inside the
     * quantiser floor has nothing for the pattern to find - whatever it
     * moves to, the residual quantises to the same nothing - so the
     * walk is bought to lose. Asked per pixel in quantiser steps. */
    int early = 0;
    if (pattern >= 2) {
        unsigned pct = pattern == 2 ? 50u : 100u;
        uint64_t thr = (uint64_t)(w * h) * qstep_q8(e->cur_ctu_qp) * pct / 100u;
        early = (uint64_t)best_sad * 256u <= thr;
    }
    if (early) {
        /* nothing */
    } else if (pattern) {
        /* Hexagon search (HEXBS, Zhu/Lin/Chau 2002): six points on a
         * flattened hexagon so a step covers two pels of ground for the
         * same six evaluations, walked until the centre wins, then a
         * unit-diamond finish for the pel the hexagon steps over. The
         * walk is bounded at the reference's merange class - 57 full
         * pels from the seed - rather than by an iteration count. */
        static const int hex6[6][2] = {
            {8,0}, {4,8}, {-4,8}, {-8,0}, {-4,-8}, {4,-8}
        };
        const int rng = 57 * 4;
        int cx0 = cur.x, cy0 = cur.y;
        for (int iter = 0; iter < 64; iter++) {
            mv_t b = cur;
            uint32_t bs = best_sad;
            for (int i = 0; i < 6; i++) {
                mv_t t = { (int16_t)(cur.x + hex6[i][0]),
                           (int16_t)(cur.y + hex6[i][1]) };
                if (t.x - cx0 > rng || cx0 - t.x > rng ||
                    t.y - cy0 > rng || cy0 - t.y > rng)
                    continue;
                mv_t tc = t;
                n265_clamp_mv(e, x0, y0, w, h, &tc);
                if (tc.x != t.x || tc.y != t.y)
                    continue;
                uint32_t s = n265_mc_sad(e, list, ridx, x0, y0, w, h, t);
                if (s < bs) {
                    bs = s;
                    b = t;
                }
            }
            if (b.x == cur.x && b.y == cur.y)
                break;
            cur = b;
            best_sad = bs;
        }
        for (int iter = 0; iter < 4; iter++) {
            mv_t b = cur;
            uint32_t bs = best_sad;
            for (int i = 0; i < 4; i++) {
                mv_t t = { (int16_t)(cur.x + dia4[i][0]),
                           (int16_t)(cur.y + dia4[i][1]) };
                mv_t tc = t;
                n265_clamp_mv(e, x0, y0, w, h, &tc);
                if (tc.x != t.x || tc.y != t.y)
                    continue;
                uint32_t s = n265_mc_sad(e, list, ridx, x0, y0, w, h, t);
                if (s < bs) {
                    bs = s;
                    b = t;
                }
            }
            if (b.x == cur.x && b.y == cur.y)
                break;
            cur = b;
            best_sad = bs;
        }
    } else {
        /* iterative small diamond, range bounded by iteration count */
        for (int iter = 0; iter < 24; iter++) {
            mv_t b = cur;
            uint32_t bs = best_sad;
            for (int i = 0; i < 4; i++) {
                mv_t t = { (int16_t)(cur.x + dia4[i][0]),
                           (int16_t)(cur.y + dia4[i][1]) };
                mv_t tc = t;
                n265_clamp_mv(e, x0, y0, w, h, &tc);
                if (tc.x != t.x || tc.y != t.y)
                    continue;
                uint32_t s = n265_mc_sad(e, list, ridx, x0, y0, w, h, t);
                if (s < bs) {
                    bs = s;
                    b = t;
                }
            }
            if (b.x == cur.x && b.y == cur.y)
                break;
            cur = b;
            best_sad = bs;
        }
    }
    /* subpel refinement: one half-pel round, one quarter-pel round,
     * SATD-costed (SAD favors the blur of interpolated phases) */
    int fullpel = fullpel_on();
    int dedup = subpel_dedup();
    /* N265_SUBME, the subme-2 ladder. 1 runs the quarter-pel square
     * only where the half-pel one moved: where it did not, the block's
     * best half-pel phase is the centre the round started at and the
     * quarter-pel square is refining a position the search has already
     * decided nothing better is near. 2 also drops it for 8x4 and 4x8
     * PUs, where the sixteen extra interpolations buy a quarter of a
     * pel on a block too small to carry the mvd that pays for it. 3
     * caps the whole thing at one round. 4 costs the half-pel square in
     * SAD and keeps the Hadamard for the quarter-pel one, so a round is
     * eight interpolations plus eight SADs instead of eight SATDs. 5 is
     * 3 and 4 together, 6 the round cap on its own. */
    int sm = subme_mode();
    int hsad = sm == 4 || sm == 5;
    int gate_q_move = sm == 1 || sm == 2 || sm == 3 || sm == 5;
    int gate_q_small = (sm == 2 || sm == 3 || sm == 5) && w + h == 12;
    int rounds = (sm == 3 || sm == 5 || sm == 6) ? 1 : 2;
    uint32_t best_satd = fullpel ? 0 : mc_satd(e, list, ridx, x0, y0, w, h, cur);
    for (int round = 0; round < (fullpel ? 0 : rounds); round++) {
        mv_t round_start = cur;
        for (int step = 2; step >= 1; step--) {
            if (step == 1 && gate_q_small)
                break;
            if (step == 1 && gate_q_move &&
                cur.x == round_start.x && cur.y == round_start.y)
                break;
            int sad_step = hsad && step == 2;
            mv_t b = cur;
            uint32_t bs = sad_step
                ? n265_mc_sad(e, list, ridx, x0, y0, w, h, cur) : best_satd;
            for (int dy = -step; dy <= step; dy += step)
                for (int dx = -step; dx <= step; dx += step) {
                    if (!dx && !dy)
                        continue;
                    mv_t t = { (int16_t)(cur.x + dx), (int16_t)(cur.y + dy) };
                    mv_t tc = t;
                    n265_clamp_mv(e, x0, y0, w, h, &tc);
                    if (tc.x != t.x || tc.y != t.y)
                        continue;
                    uint32_t s = sad_step
                        ? n265_mc_sad(e, list, ridx, x0, y0, w, h, t)
                        : mc_satd(e, list, ridx, x0, y0, w, h, t);
                    if (s < bs) {
                        bs = s;
                        b = t;
                    }
                }
            if (sad_step) {
                /* the two steps rank in different metrics, so the
                 * quarter-pel square needs its centre re-costed */
                if (b.x != cur.x || b.y != cur.y) {
                    cur = b;
                    best_satd = mc_satd(e, list, ridx, x0, y0, w, h, cur);
                }
            } else {
                cur = b;
                best_satd = bs;
            }
        }
        /* A round that did not move the centre leaves the search in the
         * state it started in - same centre, same best cost - so the
         * next one walks the identical candidate set to the identical
         * answer. Not running it is bit-identical, and it is half the
         * subpel budget on every block that had already converged. */
        if (dedup && cur.x == round_start.x && cur.y == round_start.y)
            break;
    }
    *best = cur;
    return best_sad;
}

/* Initialize every cached knob and lazy table on the driver at encoder
 * open; see n265_ctu_warm. */
void n265_mv_warm(void)
{
    (void)subme_mode();
    (void)me_pattern();
    (void)no_tmvp_on();
    (void)merge_trace_disp();
    (void)fullpel_on();
    (void)subpel_dedup();
    (void)qstep_q8(0);      /* fills its lazy table */
}
