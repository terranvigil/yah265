/*
 * Deblocking filter (ITU-T H.265 8.7.2). QP is constant per slice for
 * now (the per-4x4 hooks arrive with cu_qp_delta); single reference, so
 * the different-ref bS leg cannot fire yet.
 * Copyright (c) 2026, the yah265 authors. GPL-2.0-or-later; see LICENSE.
 */

#include <stdlib.h>
#include "encoder/deblock.h"
#include "encoder/transform.h"
#include "encoder/preset.h"

/* Table 8-12 */
static const uint8_t beta_tab[52] = {
     0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
     6,  7,  8,  9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 20, 22, 24,
    26, 28, 30, 32, 34, 36, 38, 40, 42, 44, 46, 48, 50, 52, 54, 56,
    58, 60, 62, 64
};
static const uint8_t tc_tab[54] = {
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    1, 1, 1, 1, 1, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4,
    4, 4, 5, 5, 6, 6, 7, 8, 9, 10, 11, 13, 14, 16, 18, 20, 22, 24
};

static inline int clip3(int lo, int hi, int v)
{
    return v < lo ? lo : v > hi ? hi : v;
}
static inline pixel clip1(int v)
{
    return (pixel)(v < 0 ? 0 : v > PIXEL_MAX ? PIXEL_MAX : v);
}

/* 8.7.2.5.3: beta = beta' * (1 << (BitDepth - 8)) and tC likewise, so
 * the two tables above are read through a scale that is 1 at 8 bits -
 * which is why they were read straight. The QP the tables are INDEXED
 * by still has to grow a QpBdOffset term; that is phase 8 item 2's,
 * along with every other per-QP table. */
#define BETA_AT(i)  (beta_tab[i] << BIT_DEPTH_SHIFT)
#define TC_AT(i)    (tc_tab[i] << BIT_DEPTH_SHIFT)
static inline int iabs(int v) { return v < 0 ? -v : v; }

/* filter one 4-line luma segment across an edge; pix points at the first
 * q0 sample of the segment, xstride steps across the edge (p side is
 * -xstride), ystride steps along it. */
static void luma_segment(pixel *pix, int xstride, int ystride, int beta,
                         int tc)
{
    pixel *l0 = pix, *l3 = pix + 3 * ystride;
    int dp0 = iabs(l0[-3 * xstride] - 2 * l0[-2 * xstride] + l0[-xstride]);
    int dq0 = iabs(l0[2 * xstride] - 2 * l0[xstride] + l0[0]);
    int dp3 = iabs(l3[-3 * xstride] - 2 * l3[-2 * xstride] + l3[-xstride]);
    int dq3 = iabs(l3[2 * xstride] - 2 * l3[xstride] + l3[0]);
    int d = dp0 + dq0 + dp3 + dq3;
    if (d >= beta)
        return;

    int strong = 1;
    for (int i = 0; i < 4; i += 3) {    /* lines 0 and 3 */
        pixel *l = pix + i * ystride;
        int dpq = (i == 0) ? dp0 + dq0 : dp3 + dq3;
        if (!(2 * dpq < (beta >> 2) &&
              iabs(l[-4 * xstride] - l[-xstride]) + iabs(l[0] - l[3 * xstride])
                  < (beta >> 3) &&
              iabs(l[-xstride] - l[0]) < ((5 * tc + 1) >> 1)))
            strong = 0;
    }

    int dep = (dp0 + dp3) < ((beta + (beta >> 1)) >> 3);
    int deq = (dq0 + dq3) < ((beta + (beta >> 1)) >> 3);

    for (int i = 0; i < 4; i++) {
        pixel *l = pix + i * ystride;
        int p3 = l[-4 * xstride], p2 = l[-3 * xstride];
        int p1 = l[-2 * xstride], p0 = l[-xstride];
        int q0 = l[0], q1 = l[xstride], q2 = l[2 * xstride];
        int q3 = l[3 * xstride];
        if (strong) {
            l[-xstride] = clip1(clip3(p0 - 2 * tc, p0 + 2 * tc,
                (p2 + 2 * p1 + 2 * p0 + 2 * q0 + q1 + 4) >> 3));
            l[-2 * xstride] = clip1(clip3(p1 - 2 * tc, p1 + 2 * tc,
                (p2 + p1 + p0 + q0 + 2) >> 2));
            l[-3 * xstride] = clip1(clip3(p2 - 2 * tc, p2 + 2 * tc,
                (2 * p3 + 3 * p2 + p1 + p0 + q0 + 4) >> 3));
            l[0] = clip1(clip3(q0 - 2 * tc, q0 + 2 * tc,
                (q2 + 2 * q1 + 2 * q0 + 2 * p0 + p1 + 4) >> 3));
            l[xstride] = clip1(clip3(q1 - 2 * tc, q1 + 2 * tc,
                (q2 + q1 + q0 + p0 + 2) >> 2));
            l[2 * xstride] = clip1(clip3(q2 - 2 * tc, q2 + 2 * tc,
                (2 * q3 + 3 * q2 + q1 + q0 + p0 + 4) >> 3));
        } else {
            int delta = (9 * (q0 - p0) - 3 * (q1 - p1) + 8) >> 4;
            if (iabs(delta) >= 10 * tc)
                continue;
            delta = clip3(-tc, tc, delta);
            l[-xstride] = clip1(p0 + delta);
            l[0] = clip1(q0 - delta);
            if (dep) {
                int dp = clip3(-(tc >> 1), tc >> 1,
                               (((p2 + p0 + 1) >> 1) - p1 + delta) >> 1);
                l[-2 * xstride] = clip1(p1 + dp);
            }
            if (deq) {
                int dq = clip3(-(tc >> 1), tc >> 1,
                               (((q2 + q0 + 1) >> 1) - q1 - delta) >> 1);
                l[xstride] = clip1(q1 + dq);
            }
        }
    }
}

static void chroma_segment(pixel *pix, int xstride, int ystride, int tc,
                           int nlines)
{
    for (int i = 0; i < nlines; i++) {
        pixel *l = pix + i * ystride;
        int p1 = l[-2 * xstride], p0 = l[-xstride];
        int q0 = l[0], q1 = l[xstride];
        int delta = clip3(-tc, tc,
                          (((q0 - p0) * 4 + p1 - q1 + 4) >> 3));
        l[-xstride] = clip1(p0 + delta);
        l[0] = clip1(q0 - delta);
    }
}

/* is there a CU/TU/PU boundary crossing the 8-grid position? edge_map
 * bit 0 = left edge here, bit 1 = top edge; bits 2 and 3 say that edge is
 * also a TRANSFORM edge, which the cbf leg of the bS derivation needs
 * (8.7.2.4) - a PU boundary inside a CU is a prediction edge only. */
static int has_edge(const struct n265_frame_work *e, int x, int y, int vertical)
{
    return e->edge_map[(y >> 2) * e->map_stride + (x >> 2)] &
           (vertical ? 1 : 2);
}

static int is_tu_edge(const struct n265_frame_work *e, int x, int y, int vertical)
{
    return e->edge_map[(y >> 2) * e->map_stride + (x >> 2)] &
           (vertical ? 4 : 8);
}

/* Cached env knob at file scope so n265_deblock_warm() can resolve it on
 * the driver thread. seg_bs() runs per 4-line segment of every edge in
 * the frame, so the uncached read was a getenv per segment: 0.7% of the
 * encode's self time sat in __findenv_locked between this and entropy's
 * own (the 2026-09-10 phase 6 profile). Output-neutral - the diagnostic
 * rule still fires exactly when the variable is set. */
static int old_bs = -1;

static int old_bs_on(void)
{
    if (old_bs < 0)
        old_bs = n265_env("N265_OLD_BS") != NULL;
    return old_bs;
}

void n265_deblock_warm(void)
{
    (void)old_bs_on();
}

/* boundary strength for the 4-line segment whose q-side starts at luma
 * (x,y) (8.7.2.4). The cbf leg applies to transform edges only; PU edges
 * inside a CU reach the motion legs below. */
static int seg_bs(const struct n265_frame_work *e, int x, int y, int vertical)
{
    int qi = (y >> 2) * e->map_stride + (x >> 2);
    int pi = vertical ? (y >> 2) * e->map_stride + ((x - 1) >> 2)
                      : ((y - 1) >> 2) * e->map_stride + (x >> 2);
    if (e->blk_intra[pi] || e->blk_intra[qi])
        return 2;
    if (is_tu_edge(e, x, y, vertical) &&
        (e->blk_cbf_y[pi] || e->blk_cbf_y[qi]))
        return 1;
    /* 8.7.2.4 by reference PICTURE: different pictures or a different
     * number of motion vectors -> 1; same pictures -> compare the vectors
     * that point at the same picture, both pairings when both point at
     * one picture twice */
    if (old_bs_on()) {          /* diagnostic: the list-based rule */
        if (e->blk_pred[pi] != e->blk_pred[qi]) return 1;
        for (int l = 0; l < 2; l++) {
            if (!(e->blk_pred[pi] & (1 << l))) continue;
            int dx = e->blk_mv[4 * pi + 2 * l] - e->blk_mv[4 * qi + 2 * l];
            int dy = e->blk_mv[4 * pi + 2 * l + 1] - e->blk_mv[4 * qi + 2 * l + 1];
            if (dx <= -4 || dx >= 4 || dy <= -4 || dy >= 4) return 1;
        }
        return 0;
    }
    int np = (e->blk_pred[pi] & 1) + ((e->blk_pred[pi] >> 1) & 1);
    int nq = (e->blk_pred[qi] & 1) + ((e->blk_pred[qi] >> 1) & 1);
    if (np != nq)
        return 1;
    int pp[2], pq[2], nmp = 0, nmq = 0;     /* the pictures (POCs) each block uses */
    const int16_t *mp[2], *mq[2];
    for (int l = 0; l < 2; l++) {
        if (e->blk_pred[pi] & (1 << l)) { N265_SIDE_CHECK(e, e->refl[l][e->blk_ref[2 * pi + l]], e->ref_gen[l][e->blk_ref[2 * pi + l]], -1, "bS poc"); pp[nmp] = e->refl[l][e->blk_ref[2 * pi + l]]->poc; mp[nmp++] = e->blk_mv + 4 * pi + 2 * l; }
        if (e->blk_pred[qi] & (1 << l)) { N265_SIDE_CHECK(e, e->refl[l][e->blk_ref[2 * qi + l]], e->ref_gen[l][e->blk_ref[2 * qi + l]], -1, "bS poc"); pq[nmq] = e->refl[l][e->blk_ref[2 * qi + l]]->poc; mq[nmq++] = e->blk_mv + 4 * qi + 2 * l; }
    }
#define MVDIFF(a, b) ((a)[0] - (b)[0] <= -4 || (a)[0] - (b)[0] >= 4 || (a)[1] - (b)[1] <= -4 || (a)[1] - (b)[1] >= 4)
    if (np == 1)
        return pp[0] != pq[0] || MVDIFF(mp[0], mq[0]);
    /* two vectors each: the same two pictures? */
    if (!((pp[0] == pq[0] && pp[1] == pq[1]) || (pp[0] == pq[1] && pp[1] == pq[0])))
        return 1;
    if (pp[0] != pp[1]) {                   /* two different pictures: pair by picture */
        if (pp[0] == pq[0])
            return MVDIFF(mp[0], mq[0]) || MVDIFF(mp[1], mq[1]);
        return MVDIFF(mp[0], mq[1]) || MVDIFF(mp[1], mq[0]);
    }
    /* both vectors of both blocks point at the same picture: bS 1 only if
     * BOTH pairings differ */
    int direct = MVDIFF(mp[0], mq[0]) || MVDIFF(mp[1], mq[1]);
    int cross = MVDIFF(mp[0], mq[1]) || MVDIFF(mp[1], mq[0]);
    return direct && cross;
#undef MVDIFF
}

/* per-segment average QP from the block QP map */
static int seg_qp(const struct n265_frame_work *e, int x, int y, int vertical)
{
    int qi = (y >> 2) * e->map_stride + (x >> 2);
    int pi = vertical ? (y >> 2) * e->map_stride + ((x - 1) >> 2)
                      : ((y - 1) >> 2) * e->map_stride + (x >> 2);
    /* qPL = ((QpQ + QpP + 1) >> 1) (8.7.2.5.3), in the 8-BIT DOMAIN: the
     * spec builds it from QpY and not from Qp'Y, so the map's bias comes
     * off here and the Table 8-12 index below is a plain QpY plus the
     * slice offsets. */
    return N265_QP_LOAD((e->blk_qp[pi] + e->blk_qp[qi] + 1) >> 1);
}

/* One CTB row's share of 8.7.2, in the order the row wavefront runs it
 * (docs/threading-design.md sec 1): every vertical edge inside the band,
 * then the horizontal edge on the band's TOP boundary (between rows
 * r-1 and r) and the band's internal horizontal edges. The boundary at
 * the band's bottom belongs to row r+1.
 *
 * This is the same result as the old whole-picture "all vertical, then
 * all horizontal" pass. A horizontal edge at luma y reads lines y-4 to
 * y+3, which lie in bands r-1 and r, and both have had their vertical
 * edges filtered by the time deblock(r) runs its horizontal half.
 * Vertical filtering reads only along its own line, so nothing a later
 * row does can reach back. */
void n265_deblock_row(struct n265_frame_work *e, int r)
{
    /* 8.7.2.5.3 / 8.7.2.5.7: where cu_transquant_bypass_flag is 1 for the
     * coding unit a filtered sample belongs to, the filtered value is
     * replaced by the unfiltered one. Under --lossless that holds for
     * every sample in the picture, so the whole pass is the identity and
     * the PPS says so (pps_deblocking_filter_disabled_flag). */
    if (e->lossless)
        return;
    pixel *luma = e->recon[0];
    int ls = e->recon_stride[0];
    /* 8.7.2.5.3: Q for beta is qP + 2*slice_beta_offset_div2, and for tC
     * qP + 2*(bS-1) + 2*slice_tc_offset_div2 */
    int bo = 2 * e->dbf_beta, to = 2 * e->dbf_tc;
    int ctb = 1 << e->ctb_log2;
    int y0 = r * ctb, y1 = y0 + ctb;
    if (y1 > e->pad_h)
        y1 = e->pad_h;

    for (int x = 8; x < e->pad_w; x += 8)
        for (int y = y0; y < y1; y += 4)
            if (has_edge(e, x, y, 1)) {
                int bs = seg_bs(e, x, y, 1);
                if (bs) {
                    int q = seg_qp(e, x, y, 1);
                    luma_segment(luma + y * ls + x, 1, ls,
                                 BETA_AT(clip3(0, 51, q + bo)),
                                 TC_AT(clip3(0, 53, q + 2 * (bs - 1) + to)));
                }
            }
    for (int y = y0 < 8 ? 8 : y0; y < y1; y += 8)
        for (int x = 0; x < e->pad_w; x += 4)
            if (has_edge(e, x, y, 0)) {
                int bs = seg_bs(e, x, y, 0);
                if (bs) {
                    int q = seg_qp(e, x, y, 0);
                    luma_segment(luma + y * ls + x, ls, 1,
                                 BETA_AT(clip3(0, 51, q + bo)),
                                 TC_AT(clip3(0, 53, q + 2 * (bs - 1) + to)));
                }
            }

    for (int p = 1; p <= 2; p++) {
        pixel *c = e->recon[p];
        int cs = e->recon_stride[p];
        int cw = e->pad_w / 2, ch = e->pad_h / 2;
        int cy0 = y0 / 2, cy1 = y1 / 2;
        if (cy1 > ch)
            cy1 = ch;
        /* chroma edges on the 16-luma grid, bS 2 only. Two chroma lines
         * share one luma bS segment, so check per 2-line half. */
        for (int x = 8; x < cw; x += 8)
            for (int y = cy0; y < cy1; y += 2)
                if (has_edge(e, 2 * x, 2 * y, 1) &&
                    seg_bs(e, 2 * x, 2 * y, 1) == 2) {
                    int q = n265_chroma_qp(seg_qp(e, 2 * x, 2 * y, 1));
                    int t = TC_AT(clip3(0, 53, q + 2 + to));
                    if (t)
                        chroma_segment(c + y * cs + x, 1, cs, t, 2);
                }
        for (int y = cy0 < 8 ? 8 : cy0; y < cy1; y += 8)
            for (int x = 0; x < cw; x += 2)
                if (has_edge(e, 2 * x, 2 * y, 0) &&
                    seg_bs(e, 2 * x, 2 * y, 0) == 2) {
                    int q = n265_chroma_qp(seg_qp(e, 2 * x, 2 * y, 0));
                    int t = TC_AT(clip3(0, 53, q + 2 + to));
                    if (t)
                        chroma_segment(c + y * cs + x, cs, 1, t, 2);
                }
    }
}

/* qPY_PREV (8.6.1) with entropy_coding_sync off is a frame-serial chain,
 * so the QP map the filter reads cannot be finished by the analysis of
 * one row on its own. Analysis writes N265_QP_INHERIT where the CU coded
 * no delta and its own QP where it did; this pass resolves the markers
 * in raster CTU order, carrying `prev` across rows. It is a pure
 * function of the QP offset map and the markers, it is O(4x4 blocks),
 * and it runs one row ahead of the filter. */
void n265_qpmap_row(struct n265_frame_work *e, int r, int *prev)
{
    int ctb = 1 << e->ctb_log2;
    int by0 = (r * ctb) >> 2;
    int by1 = ((r + 1) * ctb) >> 2;
    int bh = e->pad_h >> 2;
    if (by1 > bh)
        by1 = bh;
    for (int cx = 0; cx < e->ctb_cols; cx++) {
        int q = e->qp + e->ctu_qp_off[r * e->ctb_cols + cx];
        q = n265_qp_clip(q);
        int bx0 = (cx * ctb) >> 2;
        int bx1 = ((cx + 1) * ctb) >> 2;
        int bw = e->pad_w >> 2;
        if (bx1 > bw)
            bx1 = bw;
        int coded = 0;
        for (int by = by0; by < by1; by++)
            for (int bx = bx0; bx < bx1; bx++) {
                uint8_t *v = &e->blk_qp[by * e->map_stride + bx];
                if (*v == N265_QP_INHERIT)
                    *v = (uint8_t)N265_QP_STORE(*prev);   /* prev is QpY */
                else
                    coded = 1;
            }
        if (coded)
            *prev = q;
    }
}

void n265_deblock_frame(struct n265_frame_work *e)
{
    pixel *luma = e->recon[0];
    int ls = e->recon_stride[0];
    /* 8.7.2.5.3: Q for beta is qP + 2*slice_beta_offset_div2, and for tC
     * qP + 2*(bS-1) + 2*slice_tc_offset_div2 */
    int bo = 2 * e->dbf_beta, to = 2 * e->dbf_tc;

    /* vertical edges, whole picture */
    for (int x = 8; x < e->pad_w; x += 8)
        for (int y = 0; y < e->pad_h; y += 4)
            if (has_edge(e, x, y, 1)) {
                int bs = seg_bs(e, x, y, 1);
                if (bs) {
                    int q = seg_qp(e, x, y, 1);
                    luma_segment(luma + y * ls + x, 1, ls,
                                 BETA_AT(clip3(0, 51, q + bo)),
                                 TC_AT(clip3(0, 53, q + 2 * (bs - 1) + to)));
                }
            }
    /* horizontal edges */
    for (int y = 8; y < e->pad_h; y += 8)
        for (int x = 0; x < e->pad_w; x += 4)
            if (has_edge(e, x, y, 0)) {
                int bs = seg_bs(e, x, y, 0);
                if (bs) {
                    int q = seg_qp(e, x, y, 0);
                    luma_segment(luma + y * ls + x, ls, 1,
                                 BETA_AT(clip3(0, 51, q + bo)),
                                 TC_AT(clip3(0, 53, q + 2 * (bs - 1) + to)));
                }
            }

    for (int p = 1; p <= 2; p++) {
        pixel *c = e->recon[p];
        int cs = e->recon_stride[p];
        int cw = e->pad_w / 2, ch = e->pad_h / 2;
        /* chroma edges on the 16-luma grid, bS 2 only. Two chroma lines
         * share one luma bS segment, so check per 2-line half. */
        for (int x = 8; x < cw; x += 8)
            for (int y = 0; y < ch; y += 2)
                if (has_edge(e, 2 * x, 2 * y, 1) &&
                    seg_bs(e, 2 * x, 2 * y, 1) == 2) {
                    int q = n265_chroma_qp(seg_qp(e, 2 * x, 2 * y, 1));
                    int t = TC_AT(clip3(0, 53, q + 2 + to));
                    if (t)
                        chroma_segment(c + y * cs + x, 1, cs, t, 2);
                }
        for (int y = 8; y < ch; y += 8)
            for (int x = 0; x < cw; x += 2)
                if (has_edge(e, 2 * x, 2 * y, 0) &&
                    seg_bs(e, 2 * x, 2 * y, 0) == 2) {
                    int q = n265_chroma_qp(seg_qp(e, 2 * x, 2 * y, 0));
                    int t = TC_AT(clip3(0, 53, q + 2 + to));
                    if (t)
                        chroma_segment(c + y * cs + x, cs, 1, t, 2);
                }
    }
}
