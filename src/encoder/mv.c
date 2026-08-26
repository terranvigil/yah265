/*
 * Merge/AMVP derivation and full-pel motion search.
 * Copyright (c) 2026, the next265 authors. GPL-2.0-or-later; see LICENSE.
 */

#include <stdlib.h>

#include "encoder/mc.h"
#include "encoder/mv.h"
#include "encoder/satd.h"

/* is the 4x4 cell holding (x,y) an already-decoded inter block? */
static int inter_at(struct n265_encoder *e, int x, int y, uint64_t cur_z)
{
    if (x < 0 || y < 0 || x >= e->pad_w || y >= e->pad_h)
        return 0;
    if (n265_zorder(x, y, e->pad_w, e->ctb_log2) >= cur_z)
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
    return r;
}

static int same_motion(const mot_t *a, const mot_t *b)
{
    if (a->pred != b->pred)
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
static int tmvp_mv(struct n265_encoder *e, int x0, int y0, int nw, int nh,
                   int list, mv_t *out)
{
    const struct n265_refpic *col = e->ref_l0;
    if (!col || !col->valid)
        return 0;
    int c16w = (e->pad_w + 15) >> 4;
    int i16 = -1;
    int xbr = x0 + nw, ybr = y0 + nh;
    if (xbr < e->pad_w && ybr < e->pad_h &&
        (y0 >> e->ctb_log2) == (ybr >> e->ctb_log2)) {
        int t = (ybr >> 4) * c16w + (xbr >> 4);
        if (col->col_pred[t])
            i16 = t;
    }
    if (i16 < 0) {
        int xc = x0 + nw / 2, yc = y0 + nh / 2;
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
    int td = col->poc - col->col_ref_poc[lc];
    int tb = e->poc - (list ? e->ref_l1->poc : e->ref_l0->poc);
    td = td < -128 ? -128 : td > 127 ? 127 : td;
    tb = tb < -128 ? -128 : tb > 127 ? 127 : tb;
    *out = mv_scale(v, tb, td);
    return 1;
}

int n265_merge_list(struct n265_encoder *e, int x0, int y0, int nw, int nh,
                    mot_t out[], int max)
{
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
        avail[i] = inter_at(e, pos[i][0], pos[i][1], cur);
        if (avail[i])
            m[i] = mot_at(e, pos[i][0], pos[i][1]);
    }
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
        if (tmvp_mv(e, x0, y0, nw, nh, 0, &v)) {
            t.mv[0] = v;
            t.pred |= 1;
        }
        if (e->slice_type == 0 && tmvp_mv(e, x0, y0, nw, nh, 1, &v)) {
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
            /* spec adds the pair when the refs differ OR the mvs differ;
             * our l0/l1 refs are always different pictures, so every
             * flag-eligible pair is added (8.5.3.2.4) */
            mot_t c;
            c.mv[0] = out[i].mv[0];
            c.mv[1] = out[j].mv[1];
            c.pred = 3;
            out[n++] = c;
        }
    }
    /* zero fill: uni in P, bi in B (single ref per list, so the zero
     * candidate repeats when more slots remain) */
    while (n < max) {
        out[n].mv[0].x = out[n].mv[0].y = 0;
        out[n].mv[1].x = out[n].mv[1].y = 0;
        out[n].pred = e->slice_type == 0 ? 3 : 1;
        n++;
    }
    return n;
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
static int amvp_take(struct n265_encoder *e, int m, int list, int scaled,
                     mv_t *out)
{
    int ref_poc[2] = { e->ref_l0 ? e->ref_l0->poc : 0,
                       e->ref_l1 ? e->ref_l1->poc : 0 };
    for (int p = 0; p < 2; p++) {
        int ly = p ? !list : list;
        if (!(e->blk_pred[m] & (1 << ly)))
            continue;
        int same = ref_poc[ly] == ref_poc[list];
        if (!scaled && !same)
            continue;
        mv_t v = { e->blk_mv[4 * m + 2 * ly],
                   e->blk_mv[4 * m + 2 * ly + 1] };
        if (scaled) {
            int td = e->poc - ref_poc[ly];
            int tb = e->poc - ref_poc[list];
            td = td < -128 ? -128 : td > 127 ? 127 : td;
            tb = tb < -128 ? -128 : tb > 127 ? 127 : tb;
            v = mv_scale(v, tb, td);
        }
        *out = v;
        return 1;
    }
    return 0;
}

void n265_amvp(struct n265_encoder *e, int list, int x0, int y0, int nw,
               int nh, mv_t mvp[2])
{
    uint64_t cur = n265_zorder(x0, y0, e->pad_w, e->ctb_log2);
    const int a_pos[2][2] = { { x0 - 1, y0 + nh }, { x0 - 1, y0 + nh - 1 } };
    const int b_pos[3][2] = {
        { x0 + nw, y0 - 1 }, { x0 + nw - 1, y0 - 1 }, { x0 - 1, y0 - 1 }
    };
    int a_avail[2], a_m[2];
    for (int i = 0; i < 2; i++) {
        a_avail[i] = inter_at(e, a_pos[i][0], a_pos[i][1], cur);
        a_m[i] = (a_pos[i][1] >> 2) * e->map_stride + (a_pos[i][0] >> 2);
    }
    int is_scaled = a_avail[0] || a_avail[1];

    /* candidate A: unscaled pass over A0, A1; then the scaled pass */
    mv_t mva = { 0, 0 }, mvb = { 0, 0 };
    int avail_a = 0, avail_b = 0;
    for (int i = 0; i < 2 && !avail_a; i++)
        if (a_avail[i] && amvp_take(e, a_m[i], list, 0, &mva))
            avail_a = 1;
    for (int i = 0; i < 2 && !avail_a; i++)
        if (a_avail[i] && amvp_take(e, a_m[i], list, 1, &mva))
            avail_a = 1;

    /* candidate B: unscaled over B0, B1, B2 */
    for (int i = 0; i < 3 && !avail_b; i++) {
        int bx = b_pos[i][0], by = b_pos[i][1];
        if (inter_at(e, bx, by, cur) &&
            amvp_take(e, (by >> 2) * e->map_stride + (bx >> 2), list, 0,
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
            if (inter_at(e, bx, by, cur) &&
                amvp_take(e, (by >> 2) * e->map_stride + (bx >> 2), list,
                          1, &mvb))
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
        if (tmvp_mv(e, x0, y0, nw, nh, list, &v))
            mvp[n++] = v;
    }
    while (n < 2) {
        mvp[n].x = 0;
        mvp[n].y = 0;
        n++;
    }
}

uint32_t n265_mc_sad(struct n265_encoder *e, int list, int x0, int y0,
                     int log2n, mv_t mv)
{
    const struct n265_refpic *ref = list ? e->ref_l1 : e->ref_l0;
    int n = 1 << log2n;
    const uint8_t *src = e->pad_plane[0] + y0 * e->pad_stride[0] + x0;
    if ((mv.x | mv.y) & 3) {    /* fractional: interpolate then SAD */
        uint8_t pred[64 * 64];
        const uint8_t *rp = ref->plane[0] +
            (y0 + (mv.y >> 2)) * ref->stride[0] + x0 + (mv.x >> 2);
        n265_mc_luma(pred, n, rp, ref->stride[0], mv.x & 3, mv.y & 3);
        uint32_t sad = 0;
        for (int y = 0; y < n; y++)
            for (int x = 0; x < n; x++) {
                int d = src[y * e->pad_stride[0] + x] - pred[y * n + x];
                sad += (uint32_t)(d < 0 ? -d : d);
            }
        return sad;
    }
    const uint8_t *rp = ref->plane[0] +
        (y0 + (mv.y >> 2)) * ref->stride[0] + x0 + (mv.x >> 2);
    uint32_t sad = 0;
    for (int y = 0; y < n; y++)
        for (int x = 0; x < n; x++) {
            int d = src[y * e->pad_stride[0] + x] -
                    rp[y * ref->stride[0] + x];
            sad += (uint32_t)(d < 0 ? -d : d);
        }
    return sad;
}

/* SATD of luma MC at mv: phase-aware cost for subpel refinement, where
 * SAD cannot tell interpolation blur from misalignment */
static uint32_t mc_satd(struct n265_encoder *e, int list, int x0, int y0,
                        int log2n, mv_t mv)
{
    const struct n265_refpic *ref = list ? e->ref_l1 : e->ref_l0;
    int n = 1 << log2n;
    const uint8_t *src = e->pad_plane[0] + y0 * e->pad_stride[0] + x0;
    uint8_t pred[64 * 64];
    const uint8_t *rp = ref->plane[0] +
        (y0 + (mv.y >> 2)) * ref->stride[0] + x0 + (mv.x >> 2);
    n265_mc_luma(pred, n, rp, ref->stride[0], mv.x & 3, mv.y & 3);
    return n265_satd(src, e->pad_stride[0], pred, n, n);
}

/* clamp a full-pel mv so MC stays inside the padded reference */
static void clamp_mv(struct n265_encoder *e, int x0, int y0, int n, mv_t *mv)
{
    int lo_x = -(x0 + N265_REF_PAD - 8) * 4;
    int hi_x = (e->pad_w + N265_REF_PAD - 8 - (x0 + n)) * 4;
    int lo_y = -(y0 + N265_REF_PAD - 8) * 4;
    int hi_y = (e->pad_h + N265_REF_PAD - 8 - (y0 + n)) * 4;
    if (mv->x < lo_x) mv->x = (int16_t)lo_x;
    if (mv->x > hi_x) mv->x = (int16_t)hi_x;
    if (mv->y < lo_y) mv->y = (int16_t)lo_y;
    if (mv->y > hi_y) mv->y = (int16_t)hi_y;
}

uint32_t n265_me_search(struct n265_encoder *e, int list, int x0, int y0,
                        int log2n, mv_t start, mv_t *best)
{
    int n = 1 << log2n;
    mv_t cur = { (int16_t)((start.x >> 2) * 4), (int16_t)((start.y >> 2) * 4) };
    clamp_mv(e, x0, y0, n, &cur);
    mv_t zero = { 0, 0 };
    uint32_t best_sad = n265_mc_sad(e, list, x0, y0, log2n, cur);
    uint32_t zs = n265_mc_sad(e, list, x0, y0, log2n, zero);
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
            clamp_mv(e, x0, y0, n, &tc);
            if (tc.x != t.x || tc.y != t.y)
                continue;
            uint32_t s = n265_mc_sad(e, list, x0, y0, log2n, t);
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
    uint32_t best_satd = fullpel ? 0 : mc_satd(e, list, x0, y0, log2n, cur);
    for (int round = 0; round < (fullpel ? 0 : 2); round++)
    for (int step = 2; step >= 1; step--) {
        mv_t b = cur;
        uint32_t bs = best_satd;
        for (int dy = -step; dy <= step; dy += step)
            for (int dx = -step; dx <= step; dx += step) {
                if (!dx && !dy)
                    continue;
                mv_t t = { (int16_t)(cur.x + dx), (int16_t)(cur.y + dy) };
                mv_t tc = t;
                clamp_mv(e, x0, y0, n, &tc);
                if (tc.x != t.x || tc.y != t.y)
                    continue;
                uint32_t s = mc_satd(e, list, x0, y0, log2n, t);
                if (s < bs) {
                    bs = s;
                    b = t;
                }
            }
        cur = b;
        best_satd = bs;
    }
    *best = cur;
    return best_sad;
}
