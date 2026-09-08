/*
 * Merge/AMVP derivation and full-pel motion search.
 * Copyright (c) 2026, the yah265 authors. GPL-2.0-or-later; see LICENSE.
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdlib.h>

#include "encoder/mc.h"
#include "encoder/mv.h"
#include "encoder/satd.h"

/* is the 4x4 cell holding (x,y) an already-decoded inter block?
 * Prediction-block availability (6.4.2): a neighbour inside the CURRENT
 * coding block skips the z-scan test entirely - it is the first PU of a
 * two-PU split, already derived and stored. Outside the CB the z-scan
 * order decides (6.4.1). */
static int inter_at_cb(struct n265_encoder *e, int x, int y, uint64_t cur_z,
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

static mot_t mot_at(struct n265_encoder *e, int x, int y)
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
static int tmvp_mv(struct n265_encoder *e, const pu_geo *g,
                   int list, int ref, mv_t *out)
{
    const struct n265_refpic *col = e->refl[0][0];
    if (!col || !col->valid || getenv("N265_NO_TMVP"))   /* diagnostic toggle */
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
        int t = (ybr >> 4) * c16w + (xbr >> 4);
        if (col->col_pred[t])
            i16 = t;
    }
    if (i16 < 0) {      /* centre: the prediction block's */
        int xc = g->x + g->w / 2, yc = g->y + g->h / 2;
        int t = (yc >> 4) * c16w + (xc >> 4);
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
    int td = col->poc - col->col_ref[2 * i16 + lc];
    int tb = e->poc - e->refl[list][ref]->poc;
    td = td < -128 ? -128 : td > 127 ? 127 : td;
    tb = tb < -128 ? -128 : tb > 127 ? 127 : tb;
    *out = mv_scale(v, tb, td);
    return 1;
}

int n265_merge_list_pu(struct n265_encoder *e, const pu_geo *g, mot_t out[],
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
    if (getenv("N265_MERGE_TRACE") && e->display_idx == atoi(getenv("N265_MERGE_TRACE"))) {
        fprintf(stderr, "MRG disp=%d cu=(%d,%d,%dx%d) type=%d nref=%d/%d:", e->display_idx, x0, y0, nw, nh, e->slice_type, e->nref[0], e->nref[1]);
        for (int i = 0; i < n; i++) fprintf(stderr, " %d:p%d r%d/%d (%d,%d|%d,%d)", i, out[i].pred, out[i].ref[0], out[i].ref[1], out[i].mv[0].x, out[i].mv[0].y, out[i].mv[1].x, out[i].mv[1].y);
        fprintf(stderr, "\n");
    }
    return n;
}

int n265_merge_list(struct n265_encoder *e, int x0, int y0, int nw, int nh,
                    mot_t out[], int max)
{
    pu_geo g = { x0, y0, nw, x0, y0, nw, nh, 0, 0 };
    return n265_merge_list_pu(e, &g, out, max);
}

mot_t n265_merge_cand0(struct n265_encoder *e, int x0, int y0, int nw, int nh)
{
    mot_t out[1];
    n265_merge_list(e, x0, y0, nw, nh, out, 1);
    return out[0];
}

/* neighbor take for AMVP (8.5.3.2.7): unscaled pass wants a ref with
 * the target's POC (same list first, then the other); the scaled pass
 * takes the first motion present (same list first) and scales it to
 * the target distance. One ref per list, all short-term. */
static int amvp_take(struct n265_encoder *e, int m, int list, int ref, int scaled,
                     mv_t *out)
{
    int target_poc = e->refl[list][ref]->poc;
    for (int p = 0; p < 2; p++) {
        int ly = p ? !list : list;
        if (!(e->blk_pred[m] & (1 << ly)))
            continue;
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

void n265_amvp_pu(struct n265_encoder *e, int list, int ref,
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

void n265_amvp(struct n265_encoder *e, int list, int ref, int x0, int y0,
               int nw, int nh, mv_t mvp[2])
{
    pu_geo g = { x0, y0, nw, x0, y0, nw, nh, 0, 0 };
    n265_amvp_pu(e, list, ref, &g, mvp);
}

uint32_t n265_mc_sad(struct n265_encoder *e, int list, int ridx, int x0, int y0,
                     int w, int h, mv_t mv)
{
    const struct n265_refpic *ref = e->refl[list][ridx];
    const uint8_t *src = e->pad_plane[0] + y0 * e->pad_stride[0] + x0;
    if ((mv.x | mv.y) & 3) {    /* fractional: interpolate then SAD */
        uint8_t pred[64 * 64];
        int ix = x0 + (mv.x >> 2), iy = y0 + (mv.y >> 2);
        const uint8_t *rp = ref->plane[0] + iy * ref->stride[0] + ix;
        n265_mc_luma_hf(pred, w, w, h, rp, ref->stride[0],
                        n265_ref_hf(ref, ix, iy, w, h, mv.x & 3),
                        ref->hf_stride, mv.x & 3, mv.y & 3);
        uint32_t sad = 0;
        for (int y = 0; y < h; y++)
            for (int x = 0; x < w; x++) {
                int d = src[y * e->pad_stride[0] + x] - pred[y * w + x];
                sad += (uint32_t)(d < 0 ? -d : d);
            }
        return sad;
    }
    const uint8_t *rp = ref->plane[0] +
        (y0 + (mv.y >> 2)) * ref->stride[0] + x0 + (mv.x >> 2);
    uint32_t sad = 0;
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            int d = src[y * e->pad_stride[0] + x] -
                    rp[y * ref->stride[0] + x];
            sad += (uint32_t)(d < 0 ? -d : d);
        }
    return sad;
}

/* SATD of luma MC at mv: phase-aware cost for subpel refinement, where
 * SAD cannot tell interpolation blur from misalignment */
static uint32_t mc_satd(struct n265_encoder *e, int list, int ridx, int x0, int y0,
                        int w, int h, mv_t mv)
{
    const struct n265_refpic *ref = e->refl[list][ridx];
    const uint8_t *src = e->pad_plane[0] + y0 * e->pad_stride[0] + x0;
    uint8_t pred[64 * 64];
    int ix = x0 + (mv.x >> 2), iy = y0 + (mv.y >> 2);
    const uint8_t *rp = ref->plane[0] + iy * ref->stride[0] + ix;
    n265_mc_luma_hf(pred, w, w, h, rp, ref->stride[0],
                    n265_ref_hf(ref, ix, iy, w, h, mv.x & 3),
                    ref->hf_stride, mv.x & 3, mv.y & 3);
    return n265_satd_wh(src, e->pad_stride[0], pred, w, w, h);
}

/* clamp a full-pel mv so MC stays inside the padded reference */
void n265_clamp_mv(struct n265_encoder *e, int x0, int y0, int w, int h,
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

uint32_t n265_me_search(struct n265_encoder *e, int list, int ridx, int x0, int y0,
                        int w, int h, mv_t start, mv_t *best)
{
    mv_t cur = { (int16_t)((start.x >> 2) * 4), (int16_t)((start.y >> 2) * 4) };
    n265_clamp_mv(e, x0, y0, w, h, &cur);
    mv_t zero = { 0, 0 };
    uint32_t best_sad = n265_mc_sad(e, list, ridx, x0, y0, w, h, cur);
    uint32_t zs = n265_mc_sad(e, list, ridx, x0, y0, w, h, zero);
    if (zs < best_sad) {
        best_sad = zs;
        cur = zero;
    }
    /* iterative small diamond, range bounded by iteration count */
    for (int iter = 0; iter < 24; iter++) {
        mv_t b = cur;
        uint32_t bs = best_sad;
        static const int d[4][2] = { {4,0}, {-4,0}, {0,4}, {0,-4} };
        for (int i = 0; i < 4; i++) {
            mv_t t = { (int16_t)(cur.x + d[i][0]),
                       (int16_t)(cur.y + d[i][1]) };
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
    /* subpel refinement: one half-pel round, one quarter-pel round,
     * SATD-costed (SAD favors the blur of interpolated phases) */
    static int fullpel = -1;
    if (fullpel < 0)
        fullpel = getenv("N265_FULLPEL") != NULL;
    static int dedup = -1;
    if (dedup < 0) {
        const char *v = getenv("N265_SUBPEL_DEDUP");
        dedup = v ? atoi(v) != 0 : 1;
    }
    uint32_t best_satd = fullpel ? 0 : mc_satd(e, list, ridx, x0, y0, w, h, cur);
    for (int round = 0; round < (fullpel ? 0 : 2); round++) {
        mv_t round_start = cur;
        for (int step = 2; step >= 1; step--) {
            mv_t b = cur;
            uint32_t bs = best_satd;
            for (int dy = -step; dy <= step; dy += step)
                for (int dx = -step; dx <= step; dx += step) {
                    if (!dx && !dy)
                        continue;
                    mv_t t = { (int16_t)(cur.x + dx), (int16_t)(cur.y + dy) };
                    mv_t tc = t;
                    n265_clamp_mv(e, x0, y0, w, h, &tc);
                    if (tc.x != t.x || tc.y != t.y)
                        continue;
                    uint32_t s = mc_satd(e, list, ridx, x0, y0, w, h, t);
                    if (s < bs) {
                        bs = s;
                        b = t;
                    }
                }
            cur = b;
            best_satd = bs;
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
