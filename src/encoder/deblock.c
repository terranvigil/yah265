/*
 * Deblocking filter (ITU-T H.265 8.7.2). All-intra simplifications: every
 * filtered edge has bS 2, QP is constant per slice for now (the per-4x4
 * hooks arrive with cu_qp_delta).
 * Copyright (c) 2026, the next265 authors. GPL-2.0-or-later; see LICENSE.
 */

#include "encoder/deblock.h"
#include "encoder/transform.h"

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
static inline uint8_t clip1(int v)
{
    return (uint8_t)(v < 0 ? 0 : v > 255 ? 255 : v);
}
static inline int iabs(int v) { return v < 0 ? -v : v; }

/* filter one 4-line luma segment across an edge; pix points at the first
 * q0 sample of the segment, xstride steps across the edge (p side is
 * -xstride), ystride steps along it. */
static void luma_segment(uint8_t *pix, int xstride, int ystride, int beta,
                         int tc)
{
    uint8_t *l0 = pix, *l3 = pix + 3 * ystride;
    int dp0 = iabs(l0[-3 * xstride] - 2 * l0[-2 * xstride] + l0[-xstride]);
    int dq0 = iabs(l0[2 * xstride] - 2 * l0[xstride] + l0[0]);
    int dp3 = iabs(l3[-3 * xstride] - 2 * l3[-2 * xstride] + l3[-xstride]);
    int dq3 = iabs(l3[2 * xstride] - 2 * l3[xstride] + l3[0]);
    int d = dp0 + dq0 + dp3 + dq3;
    if (d >= beta)
        return;

    int strong = 1;
    for (int i = 0; i < 4; i += 3) {    /* lines 0 and 3 */
        uint8_t *l = pix + i * ystride;
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
        uint8_t *l = pix + i * ystride;
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

static void chroma_segment(uint8_t *pix, int xstride, int ystride, int tc,
                           int nlines)
{
    for (int i = 0; i < nlines; i++) {
        uint8_t *l = pix + i * ystride;
        int p1 = l[-2 * xstride], p0 = l[-xstride];
        int q0 = l[0], q1 = l[xstride];
        int delta = clip3(-tc, tc,
                          ((((q0 - p0) << 2) + p1 - q1 + 4) >> 3));
        l[-xstride] = clip1(p0 + delta);
        l[0] = clip1(q0 - delta);
    }
}

/* is there a CU/TU boundary crossing the 8-grid position? edge_map bit 0 =
 * left edge here, bit 1 = top edge. */
static int has_edge(const struct n265_encoder *e, int x, int y, int vertical)
{
    return e->edge_map[(y >> 2) * e->map_stride + (x >> 2)] &
           (vertical ? 1 : 2);
}

/* boundary strength for the 4-line segment whose q-side starts at luma
 * (x,y) (8.7.2.4). All inter blocks are zero-MV ref-0 for now, so the MV
 * legs of the bS-1 rule reduce to the cbf test; extend when ME lands. */
static int seg_bs(const struct n265_encoder *e, int x, int y, int vertical)
{
    int qi = (y >> 2) * e->map_stride + (x >> 2);
    int pi = vertical ? (y >> 2) * e->map_stride + ((x - 1) >> 2)
                      : ((y - 1) >> 2) * e->map_stride + (x >> 2);
    if (e->blk_intra[pi] || e->blk_intra[qi])
        return 2;
    if (e->blk_cbf_y[pi] || e->blk_cbf_y[qi])
        return 1;
    return 0;
}

void n265_deblock_frame(struct n265_encoder *e)
{
    int qp = e->qp;
    int beta = beta_tab[clip3(0, 51, qp)];
    int tc_bs[3] = { 0, tc_tab[clip3(0, 53, qp)],
                     tc_tab[clip3(0, 53, qp + 2)] };
    int qpc = n265_chroma_qp(qp);
    int tc_c = tc_tab[clip3(0, 53, qpc + 2)];
    uint8_t *luma = e->recon[0];
    int ls = e->recon_stride[0];

    /* vertical edges, whole picture */
    for (int x = 8; x < e->pad_w; x += 8)
        for (int y = 0; y < e->pad_h; y += 4)
            if (has_edge(e, x, y, 1)) {
                int bs = seg_bs(e, x, y, 1);
                if (bs)
                    luma_segment(luma + y * ls + x, 1, ls, beta, tc_bs[bs]);
            }
    /* horizontal edges */
    for (int y = 8; y < e->pad_h; y += 8)
        for (int x = 0; x < e->pad_w; x += 4)
            if (has_edge(e, x, y, 0)) {
                int bs = seg_bs(e, x, y, 0);
                if (bs)
                    luma_segment(luma + y * ls + x, ls, 1, beta, tc_bs[bs]);
            }

    if (tc_c == 0)
        return;
    for (int p = 1; p <= 2; p++) {
        uint8_t *c = e->recon[p];
        int cs = e->recon_stride[p];
        int cw = e->pad_w / 2, ch = e->pad_h / 2;
        /* chroma edges on the 16-luma grid, bS 2 only. Two chroma lines
         * share one luma bS segment, so check per 2-line half. */
        for (int x = 8; x < cw; x += 8)
            for (int y = 0; y < ch; y += 2)
                if (has_edge(e, 2 * x, 2 * y, 1) &&
                    seg_bs(e, 2 * x, 2 * y, 1) == 2)
                    chroma_segment(c + y * cs + x, 1, cs, tc_c, 2);
        for (int y = 8; y < ch; y += 8)
            for (int x = 0; x < cw; x += 2)
                if (has_edge(e, 2 * x, 2 * y, 0) &&
                    seg_bs(e, 2 * x, 2 * y, 0) == 2)
                    chroma_segment(c + y * cs + x, cs, 1, tc_c, 2);
    }
}
