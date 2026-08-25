/*
 * Coding-tree analysis: decisions, reconstruction, and record commit.
 * No entropy coding happens here; the trailing pass in entropy.c replays
 * the record stream (docs/threading-design.md).
 * Copyright (c) 2026, the next265 authors. GPL-2.0-or-later; see LICENSE.
 */

#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "encoder/encoder.h"
#include "encoder/est.h"
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
    uint8_t recon[32 * 32];
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

/* reconstruct into the TB's scratch buffer */
static void tb_make_recon(int c_idx, int log2n, int qp, int mode, tb_ctx *tb)
{
    int inter = mode == MODE_INTER;
    int use_dst = !inter && c_idx == 0 && log2n == 2;
    int n = 1 << log2n;
    if (tb->nz) {
        int16_t dq[32 * 32];
        n265_dequant(dq, tb->level, log2n, qp);
        n265_idct_add(tb->recon, n, dq, log2n, tb->pred, n, use_dst);
    } else {
        memcpy(tb->recon, tb->pred, (size_t)n * n);
    }
}

/* blit the scratch recon into the frame and mark the cbf map */
static void tb_commit(struct n265_encoder *e, int c_idx, int x0, int y0,
                      int log2n, tb_ctx *tb)
{
    int n = 1 << log2n;
    uint8_t *recon = e->recon[c_idx];
    int stride = e->recon_stride[c_idx];
    for (int y = 0; y < n; y++)
        memcpy(recon + (y0 + y) * stride + x0, tb->recon + y * n, (size_t)n);
    if (c_idx == 0)
        set_blk(e, e->blk_cbf_y, x0, y0, n < 4 ? 4 : n, tb->nz != 0);
}

static void tb_recon(struct n265_encoder *e, int c_idx, int x0, int y0,
                     int log2n, int qp, int mode, tb_ctx *tb)
{
    tb_make_recon(c_idx, log2n, qp, mode, tb);
    tb_commit(e, c_idx, x0, y0, log2n, tb);
}

/* SSD of a TB scratch recon against the source */
static uint64_t tb_ssd(struct n265_encoder *e, int c_idx, int x0, int y0,
                       int log2n, const tb_ctx *tb)
{
    int n = 1 << log2n;
    const uint8_t *src = e->pad_plane[c_idx] + y0 * e->pad_stride[c_idx] + x0;
    uint64_t ssd = 0;
    for (int y = 0; y < n; y++)
        for (int x = 0; x < n; x++) {
            int d = src[y * e->pad_stride[c_idx] + x] - tb->recon[y * n + x];
            ssd += (uint64_t)(d * d);
        }
    return ssd;
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

/* mode-decision lambda 0.85 * 2^((qp-12)/3), Q8 */
static uint32_t lambda_mode_q8(int qp)
{
    static uint32_t tab[52];
    if (!tab[51]) {
        for (int q = 0; q < 52; q++) {
            double l = 0.85 * pow(2.0, (q - 12) / 3.0);
            tab[q] = (uint32_t)(l * 256.0 + 0.5);
        }
    }
    return tab[qp < 0 ? 0 : qp > 51 ? 51 : qp];
}

/* J = D + lambda * bits, with bits in Q8 and lambda in Q8 */
static uint64_t rd_j(uint64_t ssd, uint32_t bits_q8, uint32_t lam_q8)
{
    return ssd + (((uint64_t)bits_q8 * lam_q8) >> 16);
}

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
    rec_u8(e, 0);   /* split_transform_flag 0 (flat TU) */
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

/* --- estimator-side syntax costing (mirrors entropy.c) --- */

static void est_mpm_bits(struct n265_encoder *e, est_state *st, int x0,
                         int y0, int mode)
{
    int a = INTRA_DC, b = INTRA_DC;
    if (x0 > 0 && e->blk_intra[(y0 >> 2) * e->map_stride + ((x0 - 1) >> 2)])
        a = e->intra_mode[(y0 >> 2) * e->map_stride + ((x0 - 1) >> 2)];
    if (y0 > 0 && (y0 & ((1 << e->ctb_log2) - 1)) != 0 &&
        e->blk_intra[((y0 - 1) >> 2) * e->map_stride + (x0 >> 2)])
        b = e->intra_mode[((y0 - 1) >> 2) * e->map_stride + (x0 >> 2)];
    int cand[3];
    if (a == b) {
        if (a < 2) { cand[0] = 0; cand[1] = 1; cand[2] = 26; }
        else { cand[0] = a; cand[1] = 2 + ((a + 29) % 32);
               cand[2] = 2 + ((a - 2 + 1) % 32); }
    } else {
        cand[0] = a; cand[1] = b;
        cand[2] = (a != 0 && b != 0) ? 0 : (a != 1 && b != 1) ? 1 : 26;
    }
    int idx = -1;
    for (int i = 0; i < 3; i++)
        if (cand[i] == mode)
            idx = i;
    st->bits += n265_est_bin(&st->ctx[CTX_PREV_INTRA], idx >= 0);
    st->bits += n265_est_bypass_bits(idx >= 0 ? (idx == 0 ? 1 : 2) : 5);
}

static int est_skip_inc(struct n265_encoder *e, int x0, int y0)
{
    int inc = 0;
    if (x0 > 0)
        inc += e->blk_skip[(y0 >> 2) * e->map_stride + ((x0 - 1) >> 2)];
    if (y0 > 0)
        inc += e->blk_skip[((y0 - 1) >> 2) * e->map_stride + (x0 >> 2)];
    return inc;
}

static void est_mvd(est_state *st, int mx, int my)
{
    int ax = mx < 0 ? -mx : mx, ay = my < 0 ? -my : my;
    st->bits += n265_est_bin(&st->ctx[CTX_MVD_GR0], ax > 0);
    st->bits += n265_est_bin(&st->ctx[CTX_MVD_GR0], ay > 0);
    if (ax > 0)
        st->bits += n265_est_bin(&st->ctx[CTX_MVD_GR1], ax > 1);
    if (ay > 0)
        st->bits += n265_est_bin(&st->ctx[CTX_MVD_GR1], ay > 1);
    for (int c = 0; c < 2; c++) {
        int a = c ? ay : ax;
        if (a > 0) {
            if (a > 1) {
                int v = a - 2, bl = 1, bits = 1;
                while (v >= (1 << bl)) { v -= 1 << bl; bl++; bits += 2; }
                st->bits += n265_est_bypass_bits(bits + bl - 1);
            }
            st->bits += n265_est_bypass_bits(1);
        }
    }
}

/* residual + cbf bits of a flat (TU == CU) transform set */
static void est_tt_bits(struct n265_encoder *e, est_state *st, int log2n,
                        int mode, const tb_ctx *tl, const tb_ctx *tcb,
                        const tb_ctx *tcr)
{
    res_ops ops;
    n265_est_ops(&ops, st);
    int inter = mode == MODE_INTER;
    st->bits += n265_est_bin(&st->ctx[CTX_TSPLIT + 5 - log2n], 0);
    st->bits += n265_est_bin(&st->ctx[CTX_CBF_CHROMA + 0], tcb->nz != 0);
    st->bits += n265_est_bin(&st->ctx[CTX_CBF_CHROMA + 0], tcr->nz != 0);
    if (!(inter && !tcb->nz && !tcr->nz))
        st->bits += n265_est_bin(&st->ctx[CTX_CBF_LUMA + 1], tl->nz != 0);
    int scan_l = inter ? 0 : n265_scan_idx(mode, log2n, 0);
    int scan_c = inter ? 0 : n265_scan_idx(mode, log2n - 1, 1);
    if (tl->nz)
        n265_residual_coding(&ops, tl->level, log2n, 0, scan_l);
    if (tcb->nz)
        n265_residual_coding(&ops, tcb->level, log2n - 1, 1, scan_c);
    if (tcr->nz)
        n265_residual_coding(&ops, tcr->level, log2n - 1, 2, scan_c);
}

/* choose flat vs 4-TU split for an inter winner and commit records +
 * recon accordingly. Inter prediction is neighbor-free, so both
 * arrangements evaluate order-free. CB8 splits keep chroma at the
 * parent. */
static void commit_inter_tt(struct n265_encoder *e, mot_t mot, int x0,
                            int y0, int log2n, tb_ctx *tl, tb_ctx *tcb,
                            tb_ctx *tcr)
{
    int n = 1 << log2n;
    int qp_c = n265_chroma_qp(e->qp);
    uint32_t lam = lambda_mode_q8(e->qp);
    int h = n >> 1;
    int child_chroma = log2n > 3;
    static tb_ctx s_l[4], s_c[4][2];

    /* split arrangement */
    uint64_t d_split = 0;
    int root_cb = 0, root_cr = 0;
    for (int i = 0; i < 4; i++) {
        int x1 = x0 + (i & 1) * h, y1 = y0 + (i >> 1) * h;
        tb_analyze(e, 0, x1, y1, log2n - 1, e->qp, MODE_INTER, mot,
                   &s_l[i]);
        tb_make_recon(0, log2n - 1, e->qp, MODE_INTER, &s_l[i]);
        d_split += tb_ssd(e, 0, x1, y1, log2n - 1, &s_l[i]);
        if (child_chroma) {
            tb_analyze(e, 1, x1 / 2, y1 / 2, log2n - 2, qp_c, MODE_INTER,
                       mot, &s_c[i][0]);
            tb_analyze(e, 2, x1 / 2, y1 / 2, log2n - 2, qp_c, MODE_INTER,
                       mot, &s_c[i][1]);
            tb_make_recon(1, log2n - 2, qp_c, MODE_INTER, &s_c[i][0]);
            tb_make_recon(2, log2n - 2, qp_c, MODE_INTER, &s_c[i][1]);
            d_split += tb_ssd(e, 1, x1 / 2, y1 / 2, log2n - 2, &s_c[i][0]);
            d_split += tb_ssd(e, 2, x1 / 2, y1 / 2, log2n - 2, &s_c[i][1]);
            root_cb |= s_c[i][0].nz != 0;
            root_cr |= s_c[i][1].nz != 0;
        }
    }
    if (!child_chroma) {
        root_cb = tcb->nz != 0;
        root_cr = tcr->nz != 0;
        d_split += tb_ssd(e, 1, x0 / 2, y0 / 2, log2n - 1, tcb);
        d_split += tb_ssd(e, 2, x0 / 2, y0 / 2, log2n - 1, tcr);
    }

    uint64_t d_flat = tb_ssd(e, 0, x0, y0, log2n, tl) +
        tb_ssd(e, 1, x0 / 2, y0 / 2, log2n - 1, tcb) +
        tb_ssd(e, 2, x0 / 2, y0 / 2, log2n - 1, tcr);

    /* bits, both arrangements, on estimator copies */
    cabac_ctx_t local[CTX_COUNT];
    est_state st;
    res_ops ops;

    memcpy(local, e->est_ctx, sizeof(local));
    st.ctx = local; st.bits = 0;
    n265_est_ops(&ops, &st);
    est_tt_bits(e, &st, log2n, MODE_INTER, tl, tcb, tcr);
    uint64_t j_flat = rd_j(d_flat, st.bits, lam);

    memcpy(local, e->est_ctx, sizeof(local));
    st.ctx = local; st.bits = 0;
    st.bits += n265_est_bin(&local[CTX_TSPLIT + 5 - log2n], 1);
    st.bits += n265_est_bin(&local[CTX_CBF_CHROMA + 0], root_cb);
    st.bits += n265_est_bin(&local[CTX_CBF_CHROMA + 0], root_cr);
    for (int i = 0; i < 4; i++) {
        if (child_chroma) {
            if (root_cb)
                st.bits += n265_est_bin(&local[CTX_CBF_CHROMA + 1],
                                        s_c[i][0].nz != 0);
            if (root_cr)
                st.bits += n265_est_bin(&local[CTX_CBF_CHROMA + 1],
                                        s_c[i][1].nz != 0);
        }
        st.bits += n265_est_bin(&local[CTX_CBF_LUMA + 0], s_l[i].nz != 0);
        if (s_l[i].nz)
            n265_residual_coding(&ops, s_l[i].level, log2n - 1, 0, 0);
        if (child_chroma) {
            if (s_c[i][0].nz)
                n265_residual_coding(&ops, s_c[i][0].level, log2n - 2, 1, 0);
            if (s_c[i][1].nz)
                n265_residual_coding(&ops, s_c[i][1].level, log2n - 2, 2, 0);
        } else if (i == 3) {
            if (tcb->nz)
                n265_residual_coding(&ops, tcb->level, log2n - 1, 1, 0);
            if (tcr->nz)
                n265_residual_coding(&ops, tcr->level, log2n - 1, 2, 0);
        }
    }
    uint64_t j_split = rd_j(d_split, st.bits, lam);

    if (j_flat <= j_split) {
        rec_u8(e, 0);
        rec_u8(e, tcb->nz != 0);
        rec_u8(e, tcr->nz != 0);
        rec_u8(e, tl->nz != 0);
        tb_commit(e, 0, x0, y0, log2n, tl);
        if (tl->nz)
            rec_levels(e, tl->level, n * n);
        tb_commit(e, 1, x0 / 2, y0 / 2, log2n - 1, tcb);
        if (tcb->nz)
            rec_levels(e, tcb->level, (n / 2) * (n / 2));
        tb_commit(e, 2, x0 / 2, y0 / 2, log2n - 1, tcr);
        if (tcr->nz)
            rec_levels(e, tcr->level, (n / 2) * (n / 2));
        return;
    }

    /* commit split: record layout mirrors replay_tt */
    rec_u8(e, 1);
    rec_u8(e, (unsigned)root_cb);
    rec_u8(e, (unsigned)root_cr);
    for (int i = 0; i < 4; i++) {
        int x1 = x0 + (i & 1) * h, y1 = y0 + (i >> 1) * h;
        mark_cu_edges(e, x1, y1, h);
        if (child_chroma) {
            rec_u8(e, s_c[i][0].nz != 0);
            rec_u8(e, s_c[i][1].nz != 0);
        }
        rec_u8(e, s_l[i].nz != 0);
        tb_commit(e, 0, x1, y1, log2n - 1, &s_l[i]);
        if (s_l[i].nz)
            rec_levels(e, s_l[i].level, h * h);
        if (child_chroma) {
            tb_commit(e, 1, x1 / 2, y1 / 2, log2n - 2, &s_c[i][0]);
            if (s_c[i][0].nz)
                rec_levels(e, s_c[i][0].level, (h / 2) * (h / 2));
            tb_commit(e, 2, x1 / 2, y1 / 2, log2n - 2, &s_c[i][1]);
            if (s_c[i][1].nz)
                rec_levels(e, s_c[i][1].level, (h / 2) * (h / 2));
        } else if (i == 3) {
            tb_commit(e, 1, x0 / 2, y0 / 2, log2n - 1, tcb);
            if (tcb->nz)
                rec_levels(e, tcb->level, (n / 2) * (n / 2));
            tb_commit(e, 2, x0 / 2, y0 / 2, log2n - 1, tcr);
            if (tcr->nz)
                rec_levels(e, tcr->level, (n / 2) * (n / 2));
        }
    }
}

static uint64_t cu_area_ssd(struct n265_encoder *e, int x0, int y0, int n)
{
    uint64_t ssd = 0;
    for (int c = 0; c < 3; c++) {
        int sh = c ? 1 : 0;
        const uint8_t *src = e->pad_plane[c];
        const uint8_t *rec = e->recon[c];
        for (int y = y0 >> sh; y < (y0 + n) >> sh; y++)
            for (int x = x0 >> sh; x < (x0 + n) >> sh; x++) {
                int d = src[y * e->pad_stride[c] + x] -
                        rec[y * e->recon_stride[c] + x];
                ssd += (uint64_t)(d * d);
            }
    }
    return ssd;
}

/* qstep^2 / 8: the per-pixel MSE below which a skip is "clean" and the
 * split trial cannot plausibly pay for its syntax */
static uint64_t skip_clean_thr(int qp, int n)
{
    double qstep = pow(2.0, (qp - 4) / 6.0);
    return (uint64_t)((double)n * n * qstep * qstep / 4.0);
}

static uint64_t analyze_cu(struct n265_encoder *e, int x0, int y0, int log2n)
{
    int n = 1 << log2n;
    int qp_c = n265_chroma_qp(e->qp);
    e->last_skip_clean = 0;
    mark_cu_edges(e, x0, y0, n);

    size_t pos0 = e->rec_len;

    if (e->slice_type != 2) {
        static tb_ctx m_tl, m_tcb, m_tcr;   /* merge candidate TBs */
        static tb_ctx a_tl, a_tcb, a_tcr;   /* amvp candidate TBs */
        static tb_ctx i_tl, i_tcb, i_tcr;   /* intra candidate TBs */
        mot_t cand = n265_merge_cand0(e, x0, y0, n, n);
        uint32_t cand_plain_sad = mot_sad(e, x0, y0, log2n, cand);

        /* strict static skip at the merge candidate */
        if (cand_plain_sad * 8u <= (uint32_t)(n * n * skip_thr8())) {
            skip_cu(e, x0, y0, log2n, cand);
            e->rec_pos = pos0;
            est_state adv = { e->est_ctx, 0 };
            n265_est_advance_cu(e, &adv, x0, y0, log2n);
            uint64_t ssd = cu_area_ssd(e, x0, y0, n);
            e->last_skip_clean = ssd <= skip_clean_thr(e->qp, n);
            return rd_j(ssd, adv.bits, lambda_mode_q8(e->qp));
        }

        if (log2n <= 5) {
            uint32_t lam = lambda_mode_q8(e->qp);
            int inc = est_skip_inc(e, x0, y0);
            /* economy gates: a very good merge prediction prunes the
             * expensive candidates (classic early-skip shape) */
            int gate_amvp = cand_plain_sad < (uint32_t)(n * n) / 2;
            int gate_intra = cand_plain_sad < (uint32_t)(n * n);

            /* merge candidate (and the forced-skip variant) */
            tb_analyze(e, 0, x0, y0, log2n, e->qp, MODE_INTER, cand, &m_tl);
            tb_analyze(e, 1, x0 / 2, y0 / 2, log2n - 1, qp_c, MODE_INTER,
                       cand, &m_tcb);
            tb_analyze(e, 2, x0 / 2, y0 / 2, log2n - 1, qp_c, MODE_INTER,
                       cand, &m_tcr);
            tb_make_recon(0, log2n, e->qp, MODE_INTER, &m_tl);
            tb_make_recon(1, log2n - 1, qp_c, MODE_INTER, &m_tcb);
            tb_make_recon(2, log2n - 1, qp_c, MODE_INTER, &m_tcr);
            int m_any = m_tl.nz || m_tcb.nz || m_tcr.nz;
            uint64_t d_merge = tb_ssd(e, 0, x0, y0, log2n, &m_tl) +
                tb_ssd(e, 1, x0 / 2, y0 / 2, log2n - 1, &m_tcb) +
                tb_ssd(e, 2, x0 / 2, y0 / 2, log2n - 1, &m_tcr);
            /* forced skip: prediction only */
            uint64_t d_skip;
            {
                tb_ctx t;
                memcpy(t.recon, m_tl.pred, (size_t)n * n);
                d_skip = tb_ssd(e, 0, x0, y0, log2n, &t);
                memcpy(t.recon, m_tcb.pred, (size_t)(n / 2) * (n / 2));
                d_skip += tb_ssd(e, 1, x0 / 2, y0 / 2, log2n - 1, &t);
                memcpy(t.recon, m_tcr.pred, (size_t)(n / 2) * (n / 2));
                d_skip += tb_ssd(e, 2, x0 / 2, y0 / 2, log2n - 1, &t);
            }
            est_state st;
            cabac_ctx_t local[CTX_COUNT];

            /* skip bits */
            memcpy(local, e->est_ctx, sizeof(local));
            st.ctx = local; st.bits = 0;
            st.bits += n265_est_bin(&local[CTX_CU_SKIP + inc], 1);
            uint64_t j_skip = rd_j(d_skip, st.bits, lam);

            /* merge bits */
            uint64_t j_merge = UINT64_MAX;
            if (m_any) {
                memcpy(local, e->est_ctx, sizeof(local));
                st.ctx = local; st.bits = 0;
                st.bits += n265_est_bin(&local[CTX_CU_SKIP + inc], 0);
                st.bits += n265_est_bin(&local[CTX_PRED_MODE], 0);
                st.bits += n265_est_bin(&local[CTX_PART_MODE], 1);
                st.bits += n265_est_bin(&local[CTX_MERGE_FLAG], 1);
                est_tt_bits(e, &st, log2n, MODE_INTER, &m_tl, &m_tcb,
                            &m_tcr);
                j_merge = rd_j(d_merge, st.bits, lam);
            }

            /* amvp candidate */
            mv_t mvp[2] = { { 0, 0 }, { 0, 0 } }, me_mv = { 0, 0 };
            uint64_t j_amvp = UINT64_MAX;
            int a_any = 0, mvp_idx = 0;
            mot_t amot = { { { 0, 0 }, { 0, 0 } }, 1 };
            int same_as_cand = 1;
            if (!gate_amvp) {
                n265_amvp(e, x0, y0, n, n, mvp);
                n265_me_search(e, x0, y0, log2n, mvp[0], &me_mv);
                amot.mv[0] = me_mv;
                same_as_cand = cand.pred == 1 && me_mv.x == cand.mv[0].x &&
                               me_mv.y == cand.mv[0].y;
            }
            if (!gate_amvp && !same_as_cand && !getenv("N265_NO_AMVP")) {
                tb_analyze(e, 0, x0, y0, log2n, e->qp, MODE_INTER, amot,
                           &a_tl);
                tb_analyze(e, 1, x0 / 2, y0 / 2, log2n - 1, qp_c,
                           MODE_INTER, amot, &a_tcb);
                tb_analyze(e, 2, x0 / 2, y0 / 2, log2n - 1, qp_c,
                           MODE_INTER, amot, &a_tcr);
                tb_make_recon(0, log2n, e->qp, MODE_INTER, &a_tl);
                tb_make_recon(1, log2n - 1, qp_c, MODE_INTER, &a_tcb);
                tb_make_recon(2, log2n - 1, qp_c, MODE_INTER, &a_tcr);
                a_any = a_tl.nz || a_tcb.nz || a_tcr.nz;
                uint64_t d = tb_ssd(e, 0, x0, y0, log2n, &a_tl) +
                    tb_ssd(e, 1, x0 / 2, y0 / 2, log2n - 1, &a_tcb) +
                    tb_ssd(e, 2, x0 / 2, y0 / 2, log2n - 1, &a_tcr);
                int c0 = (me_mv.x - mvp[0].x < 0 ? mvp[0].x - me_mv.x
                                                 : me_mv.x - mvp[0].x) +
                         (me_mv.y - mvp[0].y < 0 ? mvp[0].y - me_mv.y
                                                 : me_mv.y - mvp[0].y);
                int c1 = (me_mv.x - mvp[1].x < 0 ? mvp[1].x - me_mv.x
                                                 : me_mv.x - mvp[1].x) +
                         (me_mv.y - mvp[1].y < 0 ? mvp[1].y - me_mv.y
                                                 : me_mv.y - mvp[1].y);
                mvp_idx = c1 < c0;
                memcpy(local, e->est_ctx, sizeof(local));
                st.ctx = local; st.bits = 0;
                st.bits += n265_est_bin(&local[CTX_CU_SKIP + inc], 0);
                st.bits += n265_est_bin(&local[CTX_PRED_MODE], 0);
                st.bits += n265_est_bin(&local[CTX_PART_MODE], 1);
                st.bits += n265_est_bin(&local[CTX_MERGE_FLAG], 0);
                if (e->slice_type == 0) {
                    int depth = e->ctb_log2 - log2n;
                    st.bits += n265_est_bin(&local[CTX_INTER_IDC + depth], 0);
                    st.bits += n265_est_bin(&local[CTX_INTER_IDC + 4], 0);
                }
                est_mvd(&st, me_mv.x - mvp[mvp_idx].x,
                        me_mv.y - mvp[mvp_idx].y);
                st.bits += n265_est_bin(&local[CTX_MVP_FLAG], mvp_idx);
                st.bits += n265_est_bin(&local[CTX_RQT_ROOT_CBF], a_any);
                if (a_any)
                    est_tt_bits(e, &st, log2n, MODE_INTER, &a_tl, &a_tcb,
                                &a_tcr);
                j_amvp = rd_j(d, st.bits, lam);
            }

            /* intra candidate (2Nx2N, SAD-ranked best mode) */
            uint64_t j_intra = UINT64_MAX;
            int imode = 1;
            if (!gate_intra) {
            imode = search_luma_mode(e, x0, y0, log2n, e->recon[0],
                                     NULL);
            tb_analyze(e, 1, x0 / 2, y0 / 2, log2n - 1, qp_c, imode,
                       mot_zero, &i_tcb);
            tb_analyze(e, 2, x0 / 2, y0 / 2, log2n - 1, qp_c, imode,
                       mot_zero, &i_tcr);
            tb_analyze(e, 0, x0, y0, log2n, e->qp, imode, mot_zero, &i_tl);
            tb_make_recon(0, log2n, e->qp, imode, &i_tl);
            tb_make_recon(1, log2n - 1, qp_c, imode, &i_tcb);
            tb_make_recon(2, log2n - 1, qp_c, imode, &i_tcr);
            uint64_t d_intra = tb_ssd(e, 0, x0, y0, log2n, &i_tl) +
                tb_ssd(e, 1, x0 / 2, y0 / 2, log2n - 1, &i_tcb) +
                tb_ssd(e, 2, x0 / 2, y0 / 2, log2n - 1, &i_tcr);
            memcpy(local, e->est_ctx, sizeof(local));
            st.ctx = local; st.bits = 0;
            st.bits += n265_est_bin(&local[CTX_CU_SKIP + inc], 0);
            st.bits += n265_est_bin(&local[CTX_PRED_MODE], 1);
            if (log2n == MIN_CB_LOG2)
                st.bits += n265_est_bin(&local[CTX_PART_MODE], 1);
            est_mpm_bits(e, &st, x0, y0, imode);
            st.bits += n265_est_bin(&local[CTX_CHROMA_MODE], 0);
            {
                res_ops ops;
                n265_est_ops(&ops, &st);
                st.bits += n265_est_bin(&local[CTX_CBF_CHROMA + 0],
                                        i_tcb.nz != 0);
                st.bits += n265_est_bin(&local[CTX_CBF_CHROMA + 0],
                                        i_tcr.nz != 0);
                st.bits += n265_est_bin(&local[CTX_CBF_LUMA + 1],
                                        i_tl.nz != 0);
                if (i_tl.nz)
                    n265_residual_coding(&ops, i_tl.level, log2n, 0,
                                         n265_scan_idx(imode, log2n, 0));
                int sc = n265_scan_idx(imode, log2n - 1, 1);
                if (i_tcb.nz)
                    n265_residual_coding(&ops, i_tcb.level, log2n - 1, 1, sc);
                if (i_tcr.nz)
                    n265_residual_coding(&ops, i_tcr.level, log2n - 1, 2, sc);
            }
            j_intra = rd_j(d_intra, st.bits, lam);
            }

            /* pick the winner */
            uint64_t best = j_skip;
            int kind = 0;   /* 0 skip, 1 merge, 2 amvp, 3 intra */
            if (j_merge < best) { best = j_merge; kind = 1; }
            if (j_amvp < best) { best = j_amvp; kind = 2; }
            if (j_intra < best) { best = j_intra; kind = 3; }

            if (kind == 0) {
                skip_cu(e, x0, y0, log2n, cand);
                e->last_skip_clean = d_skip <= skip_clean_thr(e->qp, n);
            } else if (kind == 1 || kind == 2) {
                mot_t mot = kind == 1 ? cand : amot;
                tb_ctx *tl = kind == 1 ? &m_tl : &a_tl;
                tb_ctx *tcb = kind == 1 ? &m_tcb : &a_tcb;
                tb_ctx *tcr = kind == 1 ? &m_tcr : &a_tcr;
                int any = kind == 1 ? m_any : a_any;
                set_blk(e, e->blk_intra, x0, y0, n, 0);
                set_blk(e, e->blk_skip, x0, y0, n, 0);
                set_mot_map(e, x0, y0, n, mot);
                set_mode_map(e, x0, y0, n, INTRA_DC);
                mark_cu_edges(e, x0, y0, n);
                rec_u8(e, (unsigned)((kind == 1) | (mvp_idx << 1) |
                                     ((any != 0) << 2)));
                if (kind == 2) {
                    int16_t mvd[2] = {
                        (int16_t)(mot.mv[0].x - mvp[mvp_idx].x),
                        (int16_t)(mot.mv[0].y - mvp[mvp_idx].y)
                    };
                    rec_levels(e, mvd, 2);
                }
                if (any) {
                    commit_inter_tt(e, mot, x0, y0, log2n, tl, tcb, tcr);
                } else {
                    tb_commit(e, 0, x0, y0, log2n, tl);
                    tb_commit(e, 1, x0 / 2, y0 / 2, log2n - 1, tcb);
                    tb_commit(e, 2, x0 / 2, y0 / 2, log2n - 1, tcr);
                }
            }
            if (kind != 3) {
                e->rec_pos = pos0;
                est_state adv = { e->est_ctx, 0 };
                n265_est_advance_cu(e, &adv, x0, y0, log2n);
                return rd_j(cu_area_ssd(e, x0, y0, n), adv.bits,
                            lambda_mode_q8(e->qp));
            }
            /* intra wins: fall through to the intra section, which also
             * arbitrates NxN at min CB */
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
        e->rec_pos = pos0;
        est_state adv = { e->est_ctx, 0 };
        n265_est_advance_cu(e, &adv, x0, y0, log2n);
        return rd_j(cu_area_ssd(e, x0, y0, n), adv.bits,
                    lambda_mode_q8(e->qp));
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
    e->rec_pos = pos0;
    est_state adv = { e->est_ctx, 0 };
    n265_est_advance_cu(e, &adv, x0, y0, log2n);
    return rd_j(cu_area_ssd(e, x0, y0, n), adv.bits, lambda_mode_q8(e->qp));
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

/* full encoder-state snapshot of a CU area, for split-vs-nosplit trials */
typedef struct qt_snap {
    uint8_t recon[64 * 64 * 3 / 2];
    uint8_t maps[7][16 * 16];
    int16_t mv[16 * 16 * 2];
    cabac_ctx_t est[CTX_COUNT];
    size_t rec_len;
    /* the records this arm appended beyond rec_base (the other arm
     * overwrites the shared tail, so content must be kept too) */
    size_t rec_base;
    size_t rec_copy_len;
    uint8_t rec_copy[16384];
} qt_snap;

static uint8_t *snap_maps(struct n265_encoder *e, int i)
{
    uint8_t *m[7];
    m[0] = e->ct_depth; m[1] = e->intra_mode; m[2] = e->edge_map;
    m[3] = e->blk_intra; m[4] = e->blk_skip; m[5] = e->blk_cbf_y;
    m[6] = e->blk_pred;
    return m[i];
}

static void qt_save(struct n265_encoder *e, qt_snap *sn, int x0, int y0,
                    int n)
{
    uint8_t *p = sn->recon;
    for (int c = 0; c < 3; c++) {
        int sh = c ? 1 : 0;
        for (int y = y0 >> sh; y < (y0 + n) >> sh; y++) {
            memcpy(p, e->recon[c] + y * e->recon_stride[c] + (x0 >> sh),
                   (size_t)(n >> sh));
            p += n >> sh;
        }
    }
    int cells = n >> 2;
    for (int i = 0; i < 7; i++)
        for (int y = 0; y < cells; y++)
            memcpy(sn->maps[i] + y * cells,
                   snap_maps(e, i) + ((y0 >> 2) + y) * e->map_stride +
                       (x0 >> 2),
                   (size_t)cells);
    for (int y = 0; y < cells; y++)
        memcpy(sn->mv + y * cells * 2 * 2,
               e->blk_mv + (((y0 >> 2) + y) * e->map_stride + (x0 >> 2)) * 4,
               (size_t)cells * 4 * sizeof(int16_t));
    memcpy(sn->est, e->est_ctx, sizeof(sn->est));
    sn->rec_len = e->rec_len;
    sn->rec_copy_len = 0;
}

static void qt_save_records(struct n265_encoder *e, qt_snap *sn,
                            size_t base)
{
    sn->rec_base = base;
    sn->rec_copy_len = e->rec_len - base;
    /* worst case is an intra CB64's full record (~12.5 KB) */
    assert(sn->rec_copy_len <= sizeof(sn->rec_copy));
    memcpy(sn->rec_copy, e->rec_buf + base, sn->rec_copy_len);
}

static void qt_restore(struct n265_encoder *e, const qt_snap *sn, int x0,
                       int y0, int n)
{
    const uint8_t *p = sn->recon;
    for (int c = 0; c < 3; c++) {
        int sh = c ? 1 : 0;
        for (int y = y0 >> sh; y < (y0 + n) >> sh; y++) {
            memcpy(e->recon[c] + y * e->recon_stride[c] + (x0 >> sh), p,
                   (size_t)(n >> sh));
            p += n >> sh;
        }
    }
    int cells = n >> 2;
    for (int i = 0; i < 7; i++)
        for (int y = 0; y < cells; y++)
            memcpy(snap_maps(e, i) + ((y0 >> 2) + y) * e->map_stride +
                       (x0 >> 2),
                   sn->maps[i] + y * cells, (size_t)cells);
    for (int y = 0; y < cells; y++)
        memcpy(e->blk_mv + (((y0 >> 2) + y) * e->map_stride + (x0 >> 2)) * 4,
               sn->mv + y * cells * 2 * 2,
               (size_t)cells * 4 * sizeof(int16_t));
    memcpy(e->est_ctx, sn->est, sizeof(sn->est));
    e->rec_len = sn->rec_len;
    if (sn->rec_copy_len)
        memcpy(e->rec_buf + sn->rec_base, sn->rec_copy, sn->rec_copy_len);
}

/* RD quadtree: at each inside node try no-split (one CU) and split (four
 * children, recursively), keep the cheaper tree. Returns the committed
 * subtree's J. Boundary nodes recurse without a flag. */
static uint64_t quadtree(struct n265_encoder *e, int x0, int y0, int log2n,
                         int depth)
{
    int n = 1 << log2n;
    int inside = x0 + n <= e->pad_w && y0 + n <= e->pad_h;
    uint32_t lam = lambda_mode_q8(e->qp);

    if (!inside && log2n > MIN_CB_LOG2) {
        uint64_t j = 0;
        int h = n >> 1;
        for (int i = 0; i < 4; i++) {
            int x1 = x0 + (i & 1) * h, y1 = y0 + (i >> 1) * h;
            if (x1 < e->pad_w && y1 < e->pad_h)
                j += quadtree(e, x1, y1, log2n - 1, depth + 1);
        }
        return j;
    }
    if (!inside)
        return 0;

    int split_inc = 0;
    if (x0 > 0)
        split_inc += e->ct_depth[(y0 >> 2) * e->map_stride +
                                 ((x0 - 1) >> 2)] > depth;
    if (y0 > 0)
        split_inc += e->ct_depth[((y0 - 1) >> 2) * e->map_stride +
                                 (x0 >> 2)] > depth;

    if (log2n == MIN_CB_LOG2) {
        uint64_t j = analyze_cu(e, x0, y0, log2n);
        set_blk(e, e->ct_depth, x0, y0, n, depth);
        return j;
    }

    /* strict-skip regions never split: try only the CU */
    if (want_skip(e, x0, y0, log2n)) {
        uint32_t b = n265_est_bin(&e->est_ctx[CTX_SPLIT_CU + split_inc], 0);
        uint64_t j = analyze_cu(e, x0, y0, log2n) + ((uint64_t)b * lam >> 16);
        set_blk(e, e->ct_depth, x0, y0, n, depth);
        return j;
    }

    static qt_snap pre[4], won[4];      /* per recursion depth (0..3) */
    qt_snap *sp = &pre[depth], *sw = &won[depth];

    qt_save(e, sp, x0, y0, n);

    /* arm 1: no split */
    size_t rec_base = e->rec_len;
    uint32_t b0 = n265_est_bin(&e->est_ctx[CTX_SPLIT_CU + split_inc], 0);
    uint64_t j_cu = analyze_cu(e, x0, y0, log2n) + ((uint64_t)b0 * lam >> 16);
    set_blk(e, e->ct_depth, x0, y0, n, depth);
    /* early skip: a CLEAN whole-CU skip ends the trial (children could
     * only re-buy the same prediction with more syntax) */
    if (e->last_skip_clean)
        return j_cu;
    qt_save(e, sw, x0, y0, n);
    qt_save_records(e, sw, rec_base);

    /* arm 2: split */
    qt_restore(e, sp, x0, y0, n);
    uint32_t b1 = n265_est_bin(&e->est_ctx[CTX_SPLIT_CU + split_inc], 1);
    uint64_t j_split = (uint64_t)b1 * lam >> 16;
    int h = n >> 1;
    for (int i = 0; i < 4; i++)
        j_split += quadtree(e, x0 + (i & 1) * h, y0 + (i >> 1) * h,
                            log2n - 1, depth + 1);

    if (j_cu <= j_split) {
        qt_restore(e, sw, x0, y0, n);
        return j_cu;
    }
    return j_split;
}

void n265_analyze_ctu(struct n265_encoder *e, int ctb_x, int ctb_y)
{
    (void)quadtree(e, ctb_x << e->ctb_log2, ctb_y << e->ctb_log2,
                   e->ctb_log2, 0);
}
