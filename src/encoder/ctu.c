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
#include "encoder/mc.h"
#include "encoder/mv.h"
#include "encoder/records.h"
#include "encoder/residual.h"
#include "encoder/satd.h"
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

/* uni MC from one list at any quarter-pel MV into dst (n x n) */
static void mc_pred_uni(struct n265_encoder *e, int list, int c_idx, int x0,
                        int y0, int n, mv_t mv, uint8_t *dst)
{
    struct n265_refpic *r = list ? e->ref_l1 : e->ref_l0;
    int rs = r->stride[c_idx];
    if (c_idx) {
        int dx = mv.x >> 3, dy = mv.y >> 3;
        const uint8_t *rp = r->plane[c_idx] + (y0 + dy) * rs + x0 + dx;
        n265_mc_chroma(dst, n, rp, rs, mv.x & 7, mv.y & 7);
    } else {
        int dx = mv.x >> 2, dy = mv.y >> 2;
        const uint8_t *rp = r->plane[0] + (y0 + dy) * rs + x0 + dx;
        n265_mc_luma(dst, n, rp, rs, mv.x & 3, mv.y & 3);
    }
}

static void mc_pred14(struct n265_encoder *e, int list, int c_idx, int x0,
                      int y0, int n, mv_t mv, int16_t *dst)
{
    struct n265_refpic *r = list ? e->ref_l1 : e->ref_l0;
    int rs = r->stride[c_idx];
    if (c_idx) {
        int dx = mv.x >> 3, dy = mv.y >> 3;
        const uint8_t *rp = r->plane[c_idx] + (y0 + dy) * rs + x0 + dx;
        n265_mc_chroma_14(dst, n, rp, rs, mv.x & 7, mv.y & 7);
    } else {
        int dx = mv.x >> 2, dy = mv.y >> 2;
        const uint8_t *rp = r->plane[0] + (y0 + dy) * rs + x0 + dx;
        n265_mc_luma_14(dst, n, rp, rs, mv.x & 3, mv.y & 3);
    }
}

/* full motion (uni either list, or bi with the default average) */
static void mc_pred(struct n265_encoder *e, int c_idx, int x0, int y0,
                    int n, mot_t mot, uint8_t *dst)
{
    if (mot.pred == 3) {
        int16_t a[64 * 64], b[64 * 64];
        mc_pred14(e, 0, c_idx, x0, y0, n, mot.mv[0], a);
        mc_pred14(e, 1, c_idx, x0, y0, n, mot.mv[1], b);
        n265_bi_avg(dst, a, b, n);
    } else {
        int list = mot.pred == 2;
        mc_pred_uni(e, list, c_idx, x0, y0, n, mot.mv[list], dst);
    }
}

static void tb_analyze(struct n265_encoder *e, int c_idx, int x0, int y0,
                       int log2n, int qp, int mode, mot_t mot, tb_ctx *tb)
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
        mc_pred(e, c_idx, x0, y0, n, mot, tb->pred);
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
    tb->nz = n265_quant(tb->level, coef, log2n, qp, !inter);
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

static const mot_t mot_zero = { { { 0, 0 }, { 0, 0 } }, 1 };

static int tb_full(struct n265_encoder *e, int c_idx, int x0, int y0,
                   int log2n, int qp, int mode, tb_ctx *tb)
{
    tb_analyze(e, c_idx, x0, y0, log2n, qp, mode, mot_zero, tb);
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

/* SAD-domain lambda: sqrt(0.85 * 2^((qp-12)/3)), Q4 fixed point */
static int lambda_sad_q4(int qp)
{
    static const uint16_t tab[52] = {
        4, 5, 5, 6, 7, 7, 8, 9, 10, 12, 13, 15, 16, 18, 21, 23,
        26, 29, 33, 37, 41, 46, 52, 58, 66, 74, 83, 93, 104, 117,
        131, 148, 166, 186, 209, 234, 263, 295, 331, 372, 418, 469,
        526, 590, 663, 744, 835, 937, 1052, 1181, 1325, 1487
    };
    return tab[qp < 0 ? 0 : qp > 51 ? 51 : qp];
}

/* rough MVD rate in bits */
static int mvd_bits(int dx, int dy)
{
    int b = 2;
    for (int c = 0; c < 2; c++) {
        int a = c ? (dy < 0 ? -dy : dy) : (dx < 0 ? -dx : dx);
        while (a) {
            b += 2;
            a >>= 1;
        }
    }
    return b;
}

static uint32_t mot_sad(struct n265_encoder *e, int x0, int y0, int log2n,
                        mot_t mot);

static int skip_thr8(void)
{
    static int thr8 = -1;
    if (thr8 < 0) {
        const char *v = getenv("N265_SKIP_THR");
        thr8 = v ? atoi(v) : 2;     /* strict: ~0.25/pixel; anything
                                     * looser must earn skip through the
                                     * residual-quantizes-to-zero test */
    }
    return thr8;
}

/* strict static-region check at the merge candidate: used by the split
 * gate; the CU decision proper re-derives */
static int want_skip(struct n265_encoder *e, int x0, int y0, int log2n)
{
    if (e->slice_type == 2)
        return 0;
    int n = 1 << log2n;
    mot_t cand = n265_merge_cand0(e, x0, y0, n, n);
    return mot_sad(e, x0, y0, log2n, cand) * 8u <=
           (uint32_t)(n * n * skip_thr8());
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
            tb_analyze(e, 1, x1 / 2, y1 / 2, log2ts - 2, qp_c, mode,
                       mot_zero, &tcb);
            tb_analyze(e, 2, x1 / 2, y1 / 2, log2ts - 2, qp_c, mode,
                       mot_zero, &tcr);
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
    tb_analyze(e, 1, x0 / 2, y0 / 2, log2ts - 1, qp_c, mode, mot_zero, &tcb);
    tb_analyze(e, 2, x0 / 2, y0 / 2, log2ts - 1, qp_c, mode, mot_zero, &tcr);
    tb_analyze(e, 0, x0, y0, log2ts, e->qp, mode, mot_zero, &tb);
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

static void set_mot_map(struct n265_encoder *e, int x0, int y0, int n,
                        mot_t mot)
{
    for (int y = 0; y < n; y += 4)
        for (int x = 0; x < n; x += 4) {
            int m = ((y0 + y) >> 2) * e->map_stride + ((x0 + x) >> 2);
            e->blk_mv[4 * m] = mot.mv[0].x;
            e->blk_mv[4 * m + 1] = mot.mv[0].y;
            e->blk_mv[4 * m + 2] = mot.mv[1].x;
            e->blk_mv[4 * m + 3] = mot.mv[1].y;
            e->blk_pred[m] = mot.pred;
        }
}

static void skip_cu(struct n265_encoder *e, int x0, int y0, int log2n,
                    mot_t mot)
{
    int n = 1 << log2n;
    uint8_t pred[64 * 64];
    for (int c = 0; c < 3; c++) {
        int sh = c ? 1 : 0;
        mc_pred(e, c, x0 >> sh, y0 >> sh, n >> sh, mot, pred);
        for (int y = 0; y < n >> sh; y++)
            memcpy(e->recon[c] + ((y0 >> sh) + y) * e->recon_stride[c] +
                       (x0 >> sh),
                   pred + y * (n >> sh), (size_t)(n >> sh));
    }
    set_blk(e, e->blk_intra, x0, y0, n, 0);
    set_blk(e, e->blk_skip, x0, y0, n, 1);
    set_blk(e, e->blk_cbf_y, x0, y0, n, 0);
    set_mot_map(e, x0, y0, n, mot);
    set_mode_map(e, x0, y0, n, INTRA_DC);
}

/* SAD of the full-motion prediction (luma) */
static uint32_t mot_sad(struct n265_encoder *e, int x0, int y0, int log2n,
                        mot_t mot)
{
    int n = 1 << log2n;
    uint8_t pred[64 * 64];
    mc_pred(e, 0, x0, y0, n, mot, pred);
    const uint8_t *src = e->pad_plane[0] + y0 * e->pad_stride[0] + x0;
    uint32_t sad = 0;
    for (int y = 0; y < n; y++)
        for (int x = 0; x < n; x++) {
            int d = src[y * e->pad_stride[0] + x] - pred[y * n + x];
            sad += (uint32_t)(d < 0 ? -d : d);
        }
    return sad;
}

/* SATD of the full-motion prediction (luma): the decision metric that
 * actually tracks post-transform bits */
static uint32_t mot_satd(struct n265_encoder *e, int x0, int y0, int log2n,
                         mot_t mot)
{
    int n = 1 << log2n;
    uint8_t pred[64 * 64];
    mc_pred(e, 0, x0, y0, n, mot, pred);
    return n265_satd(e->pad_plane[0] + y0 * e->pad_stride[0] + x0,
                     e->pad_stride[0], pred, n, n);
}

static void analyze_cu(struct n265_encoder *e, int x0, int y0, int log2n)
{
    int n = 1 << log2n;
    int qp_c = n265_chroma_qp(e->qp);
    mark_cu_edges(e, x0, y0, n);

    if (e->slice_type != 2) {
        static tb_ctx tl, tcb, tcr;      /* single-thread only; row_ctx at T1 */
        mot_t cand = n265_merge_cand0(e, x0, y0, n, n);
        uint32_t cand_plain_sad = mot_sad(e, x0, y0, log2n, cand);

        /* strict static skip at the merge candidate (plain SAD: this is
         * a near-identity test, not a coding-cost estimate) */
        if (cand_plain_sad * 8u <= (uint32_t)(n * n * skip_thr8())) {
            skip_cu(e, x0, y0, log2n, cand);
            return;
        }
        uint32_t cand_sad = cand_plain_sad;

        if (log2n <= 5) {
            uint32_t intra_sad;
            search_luma_mode(e, x0, y0, log2n, e->pad_plane[0],
                             &intra_sad);
            uint32_t lam = (uint32_t)lambda_sad_q4(e->qp);
            uint32_t cand_cost = cand_sad + (2 * lam >> 4);
            uint32_t intra_cost = intra_sad + (6 * lam >> 4);

            /* uni-L0 AMVP search in both P and B (our streams carry no
             * uni-L1 neighbors, so L0 AMVP never needs cross-list
             * scaling); B bi/L1 explicit ME is future work */
            int merge = 1;
            mv_t mvp[2] = { { 0, 0 }, { 0, 0 } }, me_mv = { 0, 0 };
            uint32_t inter_cost = cand_cost;
            {
                n265_amvp(e, x0, y0, n, n, mvp);
                uint32_t me_sad = n265_me_search(e, x0, y0, log2n, mvp[0],
                                                 &me_mv);
                int mb = mvd_bits(me_mv.x - mvp[0].x, me_mv.y - mvp[0].y);
                uint32_t me_cost = me_sad +
                    ((uint32_t)((e->slice_type == 0 ? 8 : 6) + mb) * lam
                     >> 4);
                merge = (cand.pred == 1 && me_mv.x == cand.mv[0].x &&
                         me_mv.y == cand.mv[0].y) ||
                        cand_cost <= me_cost;
                if (getenv("N265_NO_AMVP"))
                    merge = 1;
                if (!merge)
                    inter_cost = me_cost;
            }
            mot_t mot = cand;
            if (!merge) {
                mot.pred = 1;
                mot.mv[0] = me_mv;
                mot.mv[1].x = mot.mv[1].y = 0;
            }

            if (inter_cost <= intra_cost) {
                tb_analyze(e, 0, x0, y0, log2n, e->qp, MODE_INTER, mot, &tl);
                tb_analyze(e, 1, x0 / 2, y0 / 2, log2n - 1, qp_c,
                           MODE_INTER, mot, &tcb);
                tb_analyze(e, 2, x0 / 2, y0 / 2, log2n - 1, qp_c,
                           MODE_INTER, mot, &tcr);
                int any = tl.nz || tcb.nz || tcr.nz;
                if (merge && !any) {
                    skip_cu(e, x0, y0, log2n, mot);
                    return;
                }
                set_blk(e, e->blk_intra, x0, y0, n, 0);
                set_blk(e, e->blk_skip, x0, y0, n, 0);
                set_mot_map(e, x0, y0, n, mot);
                set_mode_map(e, x0, y0, n, INTRA_DC);
                /* record: flags, then mvd for the AMVP path */
                int mvp_idx = 0;
                mv_t mv = mot.mv[0];
                if (!merge && !getenv("N265_MVP0")) {
                    int c0 = (mv.x - mvp[0].x < 0 ? mvp[0].x - mv.x
                                                  : mv.x - mvp[0].x) +
                             (mv.y - mvp[0].y < 0 ? mvp[0].y - mv.y
                                                  : mv.y - mvp[0].y);
                    int c1 = (mv.x - mvp[1].x < 0 ? mvp[1].x - mv.x
                                                  : mv.x - mvp[1].x) +
                             (mv.y - mvp[1].y < 0 ? mvp[1].y - mv.y
                                                  : mv.y - mvp[1].y);
                    mvp_idx = c1 < c0;
                }
                rec_u8(e, (unsigned)(merge | (mvp_idx << 1) | ((any != 0) << 2)));
                if (!merge) {
                    int16_t mvd[2] = { (int16_t)(mv.x - mvp[mvp_idx].x),
                                       (int16_t)(mv.y - mvp[mvp_idx].y) };
                    rec_levels(e, mvd, 2);
                }
                if (any) {
                    rec_u8(e, tcb.nz != 0);
                    rec_u8(e, tcr.nz != 0);
                    rec_u8(e, tl.nz != 0);
                    tb_recon(e, 0, x0, y0, log2n, e->qp, MODE_INTER, &tl);
                    if (tl.nz)
                        rec_levels(e, tl.level, n * n);
                    tb_recon(e, 1, x0 / 2, y0 / 2, log2n - 1, qp_c,
                             MODE_INTER, &tcb);
                    if (tcb.nz)
                        rec_levels(e, tcb.level, (n / 2) * (n / 2));
                    tb_recon(e, 2, x0 / 2, y0 / 2, log2n - 1, qp_c,
                             MODE_INTER, &tcr);
                    if (tcr.nz)
                        rec_levels(e, tcr.level, (n / 2) * (n / 2));
                } else {
                    /* AMVP, rqt_root_cbf 0: prediction is the recon */
                    tb_recon(e, 0, x0, y0, log2n, e->qp, MODE_INTER, &tl);
                    tb_recon(e, 1, x0 / 2, y0 / 2, log2n - 1, qp_c,
                             MODE_INTER, &tcb);
                    tb_recon(e, 2, x0 / 2, y0 / 2, log2n - 1, qp_c,
                             MODE_INTER, &tcr);
                }
                return;
            }
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
    tb_analyze(e, 1, x0 / 2, y0 / 2, 2, qp_c, modes[0], mot_zero, &tcb);
    tb_analyze(e, 2, x0 / 2, y0 / 2, 2, qp_c, modes[0], mot_zero, &tcr);
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
