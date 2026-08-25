/*
 * Merge/AMVP derivation and full-pel motion search.
 * Copyright (c) 2026, the next265 authors. GPL-2.0-or-later; see LICENSE.
 */

#include "encoder/mv.h"

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

mv_t n265_merge_cand0(struct n265_encoder *e, int x0, int y0, int nw, int nh)
{
    uint64_t cur = n265_zorder(x0, y0, e->pad_w, e->ctb_log2);
    /* A1, B1, B0, A0, B2 */
    const int pos[5][2] = {
        { x0 - 1, y0 + nh - 1 },
        { x0 + nw - 1, y0 - 1 },
        { x0 + nw, y0 - 1 },
        { x0 - 1, y0 + nh },
        { x0 - 1, y0 - 1 },
    };
    for (int i = 0; i < 5; i++) {
        int x = pos[i][0], y = pos[i][1];
        if (inter_at(e, x, y, cur)) {
            int m = (y >> 2) * e->map_stride + (x >> 2);
            mv_t r = { e->blk_mv[2 * m], e->blk_mv[2 * m + 1] };
            return r;
        }
    }
    mv_t z = { 0, 0 };
    return z;
}

void n265_amvp(struct n265_encoder *e, int x0, int y0, int nw, int nh,
               mv_t mvp[2])
{
    uint64_t cur = n265_zorder(x0, y0, e->pad_w, e->ctb_log2);
    int n = 0;
    /* candidate A: first available of A0 (below-left), A1 (left).
     * Same ref (only one), so no scaling variants apply. */
    const int a_pos[2][2] = { { x0 - 1, y0 + nh }, { x0 - 1, y0 + nh - 1 } };
    for (int i = 0; i < 2 && n == 0; i++)
        if (inter_at(e, a_pos[i][0], a_pos[i][1], cur)) {
            int m = (a_pos[i][1] >> 2) * e->map_stride + (a_pos[i][0] >> 2);
            mvp[n].x = e->blk_mv[2 * m];
            mvp[n].y = e->blk_mv[2 * m + 1];
            n++;
        }
    /* candidate B: exactly one, the first available of B0, B1, B2; then
     * assemble [A?, B?] with the duplicate pruned and zero-fill */
    const int b_pos[3][2] = {
        { x0 + nw, y0 - 1 }, { x0 + nw - 1, y0 - 1 }, { x0 - 1, y0 - 1 }
    };
    for (int i = 0; i < 3; i++)
        if (inter_at(e, b_pos[i][0], b_pos[i][1], cur)) {
            int m = (b_pos[i][1] >> 2) * e->map_stride + (b_pos[i][0] >> 2);
            mv_t c = { e->blk_mv[2 * m], e->blk_mv[2 * m + 1] };
            if (!(n == 1 && c.x == mvp[0].x && c.y == mvp[0].y))
                mvp[n++] = c;
            break;
        }
    while (n < 2) {
        mvp[n].x = 0;
        mvp[n].y = 0;
        n++;
    }
}

uint32_t n265_mc_sad(struct n265_encoder *e, int x0, int y0, int log2n,
                     mv_t mv)
{
    int n = 1 << log2n;
    const uint8_t *src = e->pad_plane[0] + y0 * e->pad_stride[0] + x0;
    const uint8_t *ref = e->ref_plane[0] +
        (y0 + (mv.y >> 2)) * e->ref_stride[0] + x0 + (mv.x >> 2);
    uint32_t sad = 0;
    for (int y = 0; y < n; y++)
        for (int x = 0; x < n; x++) {
            int d = src[y * e->pad_stride[0] + x] -
                    ref[y * e->ref_stride[0] + x];
            sad += (uint32_t)(d < 0 ? -d : d);
        }
    return sad;
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

uint32_t n265_me_search(struct n265_encoder *e, int x0, int y0, int log2n,
                        mv_t start, mv_t *best)
{
    int n = 1 << log2n;
    /* even-pel grid: chroma (mv/2) stays integer until the interpolation
     * filters land */
    mv_t cur = { (int16_t)((start.x >> 3) * 8), (int16_t)((start.y >> 3) * 8) };
    clamp_mv(e, x0, y0, n, &cur);
    mv_t zero = { 0, 0 };
    uint32_t best_sad = n265_mc_sad(e, x0, y0, log2n, cur);
    uint32_t zs = n265_mc_sad(e, x0, y0, log2n, zero);
    if (zs < best_sad) {
        best_sad = zs;
        cur = zero;
    }
    /* iterative small diamond, range bounded by iteration count */
    for (int iter = 0; iter < 24; iter++) {
        mv_t b = cur;
        uint32_t bs = best_sad;
        static const int d[4][2] = { {8,0}, {-8,0}, {0,8}, {0,-8} };
        for (int i = 0; i < 4; i++) {
            mv_t t = { (int16_t)(cur.x + d[i][0]),
                       (int16_t)(cur.y + d[i][1]) };
            mv_t tc = t;
            clamp_mv(e, x0, y0, n, &tc);
            if (tc.x != t.x || tc.y != t.y)
                continue;
            uint32_t s = n265_mc_sad(e, x0, y0, log2n, t);
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
    *best = cur;
    return best_sad;
}
