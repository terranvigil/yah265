/*
 * Coding-tree analysis: decisions, reconstruction, and record commit.
 * No entropy coding happens here; the trailing pass in entropy.c replays
 * the record stream (docs/threading-design.md).
 * Copyright (c) 2026, the next265 authors. GPL-2.0-or-later; see LICENSE.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "encoder/encoder.h"
#include "encoder/intra.h"
#include "encoder/records.h"
#include "encoder/residual.h"
#include "encoder/transform.h"

static int *g_hist;
static int g_nxn, g_2nx2n_min;
static void print_hist(void)
{
    fprintf(stderr, "mode_hist:");
    for (int i = 0; i < 35; i++)
        fprintf(stderr, " %d:%d", i, g_hist[i]);
    fprintf(stderr, "\nmin_cb: nxn %d, 2nx2n %d\n", g_nxn, g_2nx2n_min);
}
static void atexit_hist(int *h)
{
    g_hist = h;
    atexit(print_hist);
}

static void set_blk(struct n265_encoder *e, uint8_t *map, int x0, int y0,
                    int n, int v)
{
    for (int y = 0; y < n; y += 4)
        for (int x = 0; x < n; x += 4)
            map[((y0 + y) >> 2) * e->map_stride + ((x0 + x) >> 2)] =
                (uint8_t)v;
}

static void set_mode_map(struct n265_encoder *e, int x0, int y0, int n,
                         int mode)
{
    set_blk(e, e->intra_mode, x0, y0, n, mode);
}

static void mark_cu_edges(struct n265_encoder *e, int x0, int y0, int n)
{
    for (int y = 0; y < n; y += 4)
        e->edge_map[((y0 + y) >> 2) * e->map_stride + (x0 >> 2)] |= 1;
    for (int x = 0; x < n; x += 4)
        e->edge_map[(y0 >> 2) * e->map_stride + ((x0 + x) >> 2)] |= 2;
}

/* one component TB: predict + transform + quant (+SBH). recon and record
 * are separate so the inter skip/merge decision can look at nz first. */
typedef struct tb_ctx {
    uint8_t pred[32 * 32];
    int16_t level[32 * 32];
    int nz;
} tb_ctx;

static void tb_analyze(struct n265_encoder *e, int c_idx, int x0, int y0,
                       int log2n, int qp, int mode, tb_ctx *tb)
{
    int inter = mode == MODE_INTER;
    int use_dst = !inter && c_idx == 0 && log2n == 2;
    int n = 1 << log2n;
    uint8_t *recon = e->recon[c_idx];
    int stride = e->recon_stride[c_idx];
    const uint8_t *src = e->pad_plane[c_idx] + y0 * e->pad_stride[c_idx] + x0;
    int pic_w = c_idx ? e->pad_w / 2 : e->pad_w;
    int pic_h = c_idx ? e->pad_h / 2 : e->pad_h;
    int ctb_log2 = c_idx ? e->ctb_log2 - 1 : e->ctb_log2;

    if (inter) {
        const uint8_t *rp = e->ref_plane[c_idx] + y0 * stride + x0;
        for (int y = 0; y < n; y++)
            memcpy(tb->pred + y * n, rp + y * stride, (size_t)n);
    } else {
        uint8_t refs[4 * 32 + 1], refs_f[4 * 32 + 1];
        n265_intra_refs(refs, recon, stride, x0, y0, log2n, pic_w, pic_h,
                        ctb_log2);
        if (n265_use_filtered_refs(mode, log2n, c_idx)) {
            n265_filter_refs(refs_f, refs, log2n);
            n265_intra_pred(tb->pred, n, refs_f, log2n, mode, c_idx);
        } else {
            n265_intra_pred(tb->pred, n, refs, log2n, mode, c_idx);
        }
    }

    int16_t resid[32 * 32], coef[32 * 32];
    for (int y = 0; y < n; y++)
        for (int x = 0; x < n; x++)
            resid[y * n + x] = (int16_t)(src[y * e->pad_stride[c_idx] + x] -
                                         tb->pred[y * n + x]);
    n265_fdct(coef, resid, log2n, use_dst);
    tb->nz = n265_quant(tb->level, coef, log2n, qp);
    if (tb->nz)
        n265_sbh_adjust(tb->level, log2n,
                        inter ? 0 : n265_scan_idx(mode, log2n, c_idx));
}

static void tb_recon(struct n265_encoder *e, int c_idx, int x0, int y0,
                     int log2n, int qp, int mode, tb_ctx *tb)
{
    int inter = mode == MODE_INTER;
    int use_dst = !inter && c_idx == 0 && log2n == 2;
    int n = 1 << log2n;
    uint8_t *recon = e->recon[c_idx];
    int stride = e->recon_stride[c_idx];
    if (tb->nz) {
        int16_t dq[32 * 32];
        n265_dequant(dq, tb->level, log2n, qp);
        n265_idct_add(recon + y0 * stride + x0, stride, dq, log2n, tb->pred,
                      n, use_dst);
    } else {
        for (int y = 0; y < n; y++)
            memcpy(recon + (y0 + y) * stride + x0, tb->pred + y * n,
                   (size_t)n);
    }
    if (c_idx == 0)
        set_blk(e, e->blk_cbf_y, x0, y0, n < 4 ? 4 : n, tb->nz != 0);
}

static int tb_full(struct n265_encoder *e, int c_idx, int x0, int y0,
                   int log2n, int qp, int mode, tb_ctx *tb)
{
    tb_analyze(e, c_idx, x0, y0, log2n, qp, mode, tb);
    tb_recon(e, c_idx, x0, y0, log2n, qp, mode, tb);
    return tb->nz != 0;
}

/* pick the luma mode by SAD over prediction from the given ref plane */
static int search_luma_mode(struct n265_encoder *e, int x0, int y0, int log2n,
                            const uint8_t *ref_plane, uint32_t *cost_out)
{
    int n = 1 << log2n;
    const uint8_t *src = e->pad_plane[0] + y0 * e->pad_stride[0] + x0;
    uint8_t refs[4 * 32 + 1], refs_f[4 * 32 + 1];
    uint8_t pred[32 * 32];

    n265_intra_refs(refs, ref_plane, e->recon_stride[0], x0, y0, log2n,
                    e->pad_w, e->pad_h, e->ctb_log2);
    n265_filter_refs(refs_f, refs, log2n);

    int best_mode = 0;
    uint32_t best_cost = UINT32_MAX;
    for (int mode = 0; mode < 35; mode++) {
        const uint8_t *r = n265_use_filtered_refs(mode, log2n, 0) ? refs_f
                                                                  : refs;
        n265_intra_pred(pred, n, r, log2n, mode, 0);
        uint32_t cost = 0;
        for (int y = 0; y < n; y++)
            for (int x = 0; x < n; x++) {
                int d = src[y * e->pad_stride[0] + x] - pred[y * n + x];
                cost += (uint32_t)(d < 0 ? -d : d);
            }
        if (cost < best_cost) {
            best_cost = cost;
            best_mode = mode;
        }
    }
    if (cost_out)
        *cost_out = best_cost;
    static int hist[35], armed = -1;
    if (armed < 0)
        armed = getenv("N265_MODE_HIST") != NULL;
    if (armed) {
        hist[best_mode]++;
        static int registered;
        if (!registered) {
            registered = 1;
            atexit_hist(hist);
        }
    }
    return best_mode;
}

static uint32_t skip_sad(struct n265_encoder *e, int x0, int y0, int n)
{
    const uint8_t *src = e->pad_plane[0] + y0 * e->pad_stride[0] + x0;
    const uint8_t *ref = e->ref_plane[0] + y0 * e->recon_stride[0] + x0;
    uint32_t sad = 0;
    for (int y = 0; y < n; y++)
        for (int x = 0; x < n; x++) {
            int d = src[y * e->pad_stride[0] + x] -
                    ref[y * e->recon_stride[0] + x];
            sad += (uint32_t)(d < 0 ? -d : d);
        }
    return sad;
}

static int want_skip(struct n265_encoder *e, int x0, int y0, int log2n)
{
    if (e->slice_type != 1)
        return 0;
    int n = 1 << log2n;
    static int thr8 = -1;
    if (thr8 < 0) {
        const char *v = getenv("N265_SKIP_THR");
        thr8 = v ? atoi(v) : 8 + e->qp / 4;
    }
    return skip_sad(e, x0, y0, n) * 8u <= (uint32_t)(n * n * thr8);
}

/* transform tree for a 2Nx2N CU (intra or inter): records, per node,
 * [cbf_cb][cbf_cr] then per leaf [cbf_l][levels l][levels cb][levels cr].
 * A 64x64 CB splits into four 32x32 TUs (maxTB). */
static void tt_analyze(struct n265_encoder *e, int mode, int x0, int y0,
                       int log2ts)
{
    int qp_c = n265_chroma_qp(e->qp);
    int split = log2ts > 5;
    tb_ctx tb;

    if (split) {
        /* children reconstruct sequentially (a child's chroma prediction
         * reads the previous child's chroma recon), so the root cbfs -
         * which precede the children in the record stream - are patched
         * in after the loop. */
        int h = 1 << (log2ts - 1);
        size_t root_off = e->rec_len;
        rec_u8(e, 0);
        rec_u8(e, 0);
        int root_cb = 0, root_cr = 0;
        tb_ctx tcb, tcr;
        for (int i = 0; i < 4; i++) {
            int x1 = x0 + (i & 1) * h, y1 = y0 + (i >> 1) * h;
            mark_cu_edges(e, x1, y1, h);
            tb_analyze(e, 1, x1 / 2, y1 / 2, log2ts - 2, qp_c, mode, &tcb);
            tb_analyze(e, 2, x1 / 2, y1 / 2, log2ts - 2, qp_c, mode, &tcr);
            root_cb |= tcb.nz != 0;
            root_cr |= tcr.nz != 0;
            rec_u8(e, tcb.nz != 0);
            rec_u8(e, tcr.nz != 0);
            int cbf_l = tb_full(e, 0, x1, y1, log2ts - 1, e->qp, mode, &tb);
            rec_u8(e, (unsigned)cbf_l);
            if (cbf_l)
                rec_levels(e, tb.level, h * h);
            tb_recon(e, 1, x1 / 2, y1 / 2, log2ts - 2, qp_c, mode, &tcb);
            if (tcb.nz)
                rec_levels(e, tcb.level, (h / 2) * (h / 2));
            tb_recon(e, 2, x1 / 2, y1 / 2, log2ts - 2, qp_c, mode, &tcr);
            if (tcr.nz)
                rec_levels(e, tcr.level, (h / 2) * (h / 2));
        }
        e->rec_buf[root_off] = (uint8_t)root_cb;
        e->rec_buf[root_off + 1] = (uint8_t)root_cr;
        return;
    }

    int n = 1 << log2ts;
    mark_cu_edges(e, x0, y0, n);
    tb_ctx tcb, tcr;
    tb_analyze(e, 1, x0 / 2, y0 / 2, log2ts - 1, qp_c, mode, &tcb);
    tb_analyze(e, 2, x0 / 2, y0 / 2, log2ts - 1, qp_c, mode, &tcr);
    tb_analyze(e, 0, x0, y0, log2ts, e->qp, mode, &tb);
    rec_u8(e, tcb.nz != 0);
    rec_u8(e, tcr.nz != 0);
    rec_u8(e, tb.nz != 0);
    tb_recon(e, 0, x0, y0, log2ts, e->qp, mode, &tb);
    if (tb.nz)
        rec_levels(e, tb.level, n * n);
    tb_recon(e, 1, x0 / 2, y0 / 2, log2ts - 1, qp_c, mode, &tcb);
    if (tcb.nz)
        rec_levels(e, tcb.level, (n / 2) * (n / 2));
    tb_recon(e, 2, x0 / 2, y0 / 2, log2ts - 1, qp_c, mode, &tcr);
    if (tcr.nz)
        rec_levels(e, tcr.level, (n / 2) * (n / 2));
}

static void skip_cu(struct n265_encoder *e, int x0, int y0, int log2n)
{
    int n = 1 << log2n;
    for (int y = 0; y < n; y++)
        memcpy(e->recon[0] + (y0 + y) * e->recon_stride[0] + x0,
               e->ref_plane[0] + (y0 + y) * e->recon_stride[0] + x0,
               (size_t)n);
    for (int p = 1; p <= 2; p++)
        for (int y = 0; y < n / 2; y++)
            memcpy(e->recon[p] + (y0 / 2 + y) * e->recon_stride[p] + x0 / 2,
                   e->ref_plane[p] + (y0 / 2 + y) * e->recon_stride[p] +
                       x0 / 2,
                   (size_t)(n / 2));
    set_blk(e, e->blk_intra, x0, y0, n, 0);
    set_blk(e, e->blk_skip, x0, y0, n, 1);
    set_blk(e, e->blk_cbf_y, x0, y0, n, 0);
    set_mode_map(e, x0, y0, n, INTRA_DC);
}

static void analyze_cu(struct n265_encoder *e, int x0, int y0, int log2n)
{
    int n = 1 << log2n;
    int qp_c = n265_chroma_qp(e->qp);
    mark_cu_edges(e, x0, y0, n);

    if (e->slice_type == 1) {
        int skip = 0, inter = 0;
        static tb_ctx tl, tcb, tcr;
        if (want_skip(e, x0, y0, log2n)) {
            skip = 1;
        } else if (log2n <= 5) {
            uint32_t intra_cost;
            search_luma_mode(e, x0, y0, log2n, e->pad_plane[0],
                             &intra_cost);
            if (skip_sad(e, x0, y0, n) <= intra_cost) {
                tb_analyze(e, 0, x0, y0, log2n, e->qp, MODE_INTER, &tl);
                tb_analyze(e, 1, x0 / 2, y0 / 2, log2n - 1, qp_c,
                           MODE_INTER, &tcb);
                tb_analyze(e, 2, x0 / 2, y0 / 2, log2n - 1, qp_c,
                           MODE_INTER, &tcr);
                if (!tl.nz && !tcb.nz && !tcr.nz)
                    skip = 1;
                else
                    inter = 1;
            }
        }
        if (skip) {
            skip_cu(e, x0, y0, log2n);
            return;
        }
        if (inter) {
            set_blk(e, e->blk_intra, x0, y0, n, 0);
            set_blk(e, e->blk_skip, x0, y0, n, 0);
            set_mode_map(e, x0, y0, n, INTRA_DC);
            rec_u8(e, tcb.nz != 0);
            rec_u8(e, tcr.nz != 0);
            rec_u8(e, tl.nz != 0);
            tb_recon(e, 0, x0, y0, log2n, e->qp, MODE_INTER, &tl);
            if (tl.nz)
                rec_levels(e, tl.level, n * n);
            tb_recon(e, 1, x0 / 2, y0 / 2, log2n - 1, qp_c, MODE_INTER,
                     &tcb);
            if (tcb.nz)
                rec_levels(e, tcb.level, (n / 2) * (n / 2));
            tb_recon(e, 2, x0 / 2, y0 / 2, log2n - 1, qp_c, MODE_INTER,
                     &tcr);
            if (tcr.nz)
                rec_levels(e, tcr.level, (n / 2) * (n / 2));
            return;
        }
    }

    /* intra */
    set_blk(e, e->blk_intra, x0, y0, n, 1);
    set_blk(e, e->blk_skip, x0, y0, n, 0);

    int nxn = 0;
    int modes[4] = { 0, 0, 0, 0 };
    if (log2n == MIN_CB_LOG2) {
        uint32_t cost2n;
        modes[0] = search_luma_mode(e, x0, y0, log2n, e->recon[0], &cost2n);
        uint32_t costn = 0;
        int nmodes[4];
        for (int i = 0; i < 4; i++) {
            uint32_t c;
            nmodes[i] = search_luma_mode(e, x0 + (i & 1) * 4,
                                         y0 + (i >> 1) * 4, log2n - 1,
                                         e->pad_plane[0], &c);
            costn += c;
        }
        if (costn < cost2n) {
            nxn = 1;
            for (int i = 0; i < 4; i++)
                modes[i] = nmodes[i];
        }
        rec_u8(e, (unsigned)nxn);
        if (getenv("N265_MODE_HIST")) {
            g_nxn += nxn;
            g_2nx2n_min += !nxn;
        }
    } else {
        modes[0] = search_luma_mode(e, x0, y0, log2n > 5 ? 5 : log2n,
                                    e->recon[0], NULL);
    }

    if (!nxn) {
        set_mode_map(e, x0, y0, n, modes[0]);
        tt_analyze(e, modes[0], x0, y0, log2n);
        return;
    }

    /* NxN: chroma at the parent, four 4x4 luma TUs with DST */
    tb_ctx tcb, tcr, tl;
    tb_analyze(e, 1, x0 / 2, y0 / 2, 2, qp_c, modes[0], &tcb);
    tb_analyze(e, 2, x0 / 2, y0 / 2, 2, qp_c, modes[0], &tcr);
    rec_u8(e, tcb.nz != 0);
    rec_u8(e, tcr.nz != 0);
    for (int i = 0; i < 4; i++) {
        int px = x0 + (i & 1) * 4, py = y0 + (i >> 1) * 4;
        set_mode_map(e, px, py, 4, modes[i]);
        int cbf_l = tb_full(e, 0, px, py, 2, e->qp, modes[i], &tl);
        rec_u8(e, (unsigned)cbf_l);
        if (cbf_l)
            rec_levels(e, tl.level, 16);
    }
    tb_recon(e, 1, x0 / 2, y0 / 2, 2, qp_c, modes[0], &tcb);
    if (tcb.nz)
        rec_levels(e, tcb.level, 16);
    tb_recon(e, 2, x0 / 2, y0 / 2, 2, qp_c, modes[0], &tcr);
    if (tcr.nz)
        rec_levels(e, tcr.level, 16);
}

static int decide_split(struct n265_encoder *e, int x0, int y0, int log2n)
{
    uint32_t parent;
    search_luma_mode(e, x0, y0, log2n > 5 ? 5 : log2n, e->pad_plane[0],
                     &parent);
    if (log2n > 5)
        parent *= 4;
    int h = 1 << (log2n - 1);
    uint32_t kids = 0;
    for (int i = 0; i < 4; i++) {
        uint32_t c;
        search_luma_mode(e, x0 + (i & 1) * h, y0 + (i >> 1) * h,
                         log2n - 1 > 5 ? 5 : log2n - 1, e->pad_plane[0], &c);
        kids += c;
    }
    return kids + (kids >> 4) < parent;
}

static void quadtree(struct n265_encoder *e, int x0, int y0, int log2n,
                     int depth)
{
    int n = 1 << log2n;
    int inside = x0 + n <= e->pad_w && y0 + n <= e->pad_h;

    if (inside && log2n > MIN_CB_LOG2) {
        int split = want_skip(e, x0, y0, log2n) ? 0
                    : decide_split(e, x0, y0, log2n);
        if (split) {
            /* no record: the split flag replays from ct_depth */
            int h = 1 << (log2n - 1);
            for (int i = 0; i < 4; i++)
                quadtree(e, x0 + (i & 1) * h, y0 + (i >> 1) * h,
                         log2n - 1, depth + 1);
            return;
        }
    } else if (!inside && log2n > MIN_CB_LOG2) {
        int h = n >> 1;
        for (int i = 0; i < 4; i++) {
            int x1 = x0 + (i & 1) * h, y1 = y0 + (i >> 1) * h;
            if (x1 < e->pad_w && y1 < e->pad_h)
                quadtree(e, x1, y1, log2n - 1, depth + 1);
        }
        return;
    } else if (!inside) {
        return;
    }

    analyze_cu(e, x0, y0, log2n);
    set_blk(e, e->ct_depth, x0, y0, n, depth);
}

void n265_analyze_ctu(struct n265_encoder *e, int ctb_x, int ctb_y)
{
    quadtree(e, ctb_x << e->ctb_log2, ctb_y << e->ctb_log2, e->ctb_log2, 0);
}
