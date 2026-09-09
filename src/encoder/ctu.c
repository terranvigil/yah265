/*
 * Coding-tree analysis: decisions, reconstruction, and record commit.
 * No entropy coding happens here; the trailing pass in entropy.c replays
 * the record stream (docs/threading-design.md).
 * Copyright (c) 2026, the yah265 authors. GPL-2.0-or-later; see LICENSE.
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
#include "encoder/rdoq.h"
#include "encoder/psy.h"
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

/* CU and TU boundaries: filtered edges that are also TRANSFORM edges,
 * so the cbf leg of the bS derivation applies to them (edge_map bits
 * 2/3, deblock.c) */
static void mark_cu_edges(struct n265_encoder *e, int x0, int y0, int n)
{
    for (int y = 0; y < n; y += 4)
        e->edge_map[((y0 + y) >> 2) * e->map_stride + (x0 >> 2)] |= 1 | 4;
    for (int x = 0; x < n; x += 4)
        e->edge_map[(y0 >> 2) * e->map_stride + ((x0 + x) >> 2)] |= 2 | 8;
}

/* the internal PU boundary of a split CU is a prediction edge and, on
 * its own, NOT a transform edge (8.7.2.4 gates the cbf leg on the latter),
 * so it sets only the filtered-edge bit. The deblock walk visits the 8x8
 * grid, so an AMP or 8x8-CU cut at a 4-offset is marked and correctly
 * never filtered. */
static void mark_pu_edge(struct n265_encoder *e, int part, int x0, int y0,
                         int n)
{
    if (part == PART_2Nx2N)
        return;
    int px, py, pw, ph;
    n265_pu_rect(part, 1, x0, y0, n, &px, &py, &pw, &ph);
    if (PART_IS_HORIZ(part))
        for (int x = 0; x < n; x += 4)
            e->edge_map[(py >> 2) * e->map_stride + ((x0 + x) >> 2)] |= 2;
    else
        for (int y = 0; y < n; y += 4)
            e->edge_map[((y0 + y) >> 2) * e->map_stride + (px >> 2)] |= 1;
}

/* one component TB: predict + transform + quant (+SBH). recon and record
 * are separate so the inter skip/merge decision can look at nz first. */
typedef struct tb_ctx {
    uint8_t pred[32 * 32];
    uint8_t recon[32 * 32];
    int16_t level[32 * 32];
    int nz;
} tb_ctx;

/* uni MC from one list at any quarter-pel MV into dst (w x h at ds) */
static void mc_pred_uni(struct n265_encoder *e, int list, int ref, int c_idx, int x0,
                        int y0, int w, int h, mv_t mv, uint8_t *dst, int ds)
{
    struct n265_refpic *r = e->refl[list][ref];
    int rs = r->stride[c_idx];
    if (c_idx) {
        int dx = mv.x >> 3, dy = mv.y >> 3;
        const uint8_t *rp = r->plane[c_idx] + (y0 + dy) * rs + x0 + dx;
        n265_mc_chroma_wh(dst, ds, w, h, rp, rs, mv.x & 7, mv.y & 7);
    } else {
        int ix = x0 + (mv.x >> 2), iy = y0 + (mv.y >> 2);
        const uint8_t *rp = r->plane[0] + iy * rs + ix;
        n265_mc_luma_hf(dst, ds, w, h, rp, rs,
                        n265_ref_hf(r, ix, iy, w, h, mv.x & 3),
                        r->hf_stride, mv.x & 3, mv.y & 3);
    }
}

static void mc_pred14(struct n265_encoder *e, int list, int ref, int c_idx, int x0,
                      int y0, int w, int h, mv_t mv, int16_t *dst)
{
    struct n265_refpic *r = e->refl[list][ref];
    int rs = r->stride[c_idx];
    if (c_idx) {
        int dx = mv.x >> 3, dy = mv.y >> 3;
        const uint8_t *rp = r->plane[c_idx] + (y0 + dy) * rs + x0 + dx;
        n265_mc_chroma_14_wh(dst, w, w, h, rp, rs, mv.x & 7, mv.y & 7);
    } else {
        int ix = x0 + (mv.x >> 2), iy = y0 + (mv.y >> 2);
        const uint8_t *rp = r->plane[0] + iy * rs + ix;
        n265_mc_luma_14_hf(dst, w, w, h, rp, rs,
                           n265_ref_hf(r, ix, iy, w, h, mv.x & 3),
                           r->hf_stride, mv.x & 3, mv.y & 3);
    }
}

/* full motion (uni either list, or bi with the default average) */
static void mc_pred_wh(struct n265_encoder *e, int c_idx, int x0, int y0,
                       int w, int h, mot_t mot, uint8_t *dst, int ds)
{
    if (mot.pred == 3) {
        int16_t a[64 * 64], b[64 * 64];
        mc_pred14(e, 0, mot.ref[0], c_idx, x0, y0, w, h, mot.mv[0], a);
        mc_pred14(e, 1, mot.ref[1], c_idx, x0, y0, w, h, mot.mv[1], b);
        n265_bi_avg_wh(dst, ds, a, b, w, h);
    } else {
        int list = mot.pred == 2;
        mc_pred_uni(e, list, mot.ref[list], c_idx, x0, y0, w, h,
                    mot.mv[list], dst, ds);
    }
}

static void mc_pred(struct n265_encoder *e, int c_idx, int x0, int y0,
                    int n, mot_t mot, uint8_t *dst)
{
    mc_pred_wh(e, c_idx, x0, y0, n, n, mot, dst, n);
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
        if (e->cu_pred[c_idx]) {
            /* a partitioned CU predicts PU by PU once; every TB inside it
             * crops that instead of running whole-TB MC on one motion */
            int sh = c_idx ? 1 : 0;
            int cs = e->cu_pred_stride[c_idx];
            const uint8_t *p = e->cu_pred[c_idx] +
                (y0 - (e->cu_pred_y0 >> sh)) * cs +
                (x0 - (e->cu_pred_x0 >> sh));
            for (int y = 0; y < n; y++)
                memcpy(tb->pred + y * n, p + y * cs, (size_t)n);
        } else {
            mc_pred(e, c_idx, x0, y0, n, mot, tb->pred);
        }
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
    int scan = inter ? 0 : n265_scan_idx(mode, log2n, c_idx);
    int sbh_done = 0;
    if (e->rdoq) {
        tb->nz = n265_rdoq(tb->level, coef, log2n, qp, !inter, c_idx, scan,
                           e->est_ctx, n265_lambda_mode_q8(qp));
        sbh_done = n265_rdoq_sbh_on();      /* RDOQ hid the signs itself */
    } else {
        tb->nz = n265_quant(tb->level, coef, log2n, qp, !inter);
    }
    if (tb->nz && !sbh_done)
        n265_sbh_adjust(tb->level, log2n, scan);
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
    /* the psychovisual term (psy.c): luma only, the same strength for every
     * candidate so it only ever re-ranks them */
    if (c_idx == 0 && e->psy > 0.0)
        ssd += n265_psy_cost(src, e->pad_stride[0], tb->recon, n, n,
                             n265_lambda_mode_q8(e->cur_ctu_qp), e->psy);
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

#define INTRA_RD_MAX 8

/* pick the luma mode by SAD over prediction from the given ref plane;
 * optionally reports the nk best-ranked modes for RD refinement */
static int search_luma_mode_k(struct n265_encoder *e, int x0, int y0,
                              int log2n, const uint8_t *ref_plane,
                              uint32_t *cost_out, int topk[], int nk)
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
    uint32_t ck[INTRA_RD_MAX];
    int mk[INTRA_RD_MAX];
    int nkc = nk < 1 ? 1 : nk > INTRA_RD_MAX ? INTRA_RD_MAX : nk;
    for (int i = 0; i < nkc; i++) { ck[i] = UINT32_MAX; mk[i] = i < 2 ? i : 26; }
    for (int mode = 0; mode < 35; mode++) {
        const uint8_t *r = n265_use_filtered_refs(mode, log2n, 0) ? refs_f
                                                                  : refs;
        n265_intra_pred(pred, n, r, log2n, mode, 0);
        /* rank by SATD (post-transform bits proxy); the reported cost
         * stays SAD-domain for the callers that compare across metrics */
        uint32_t cost = n265_satd(src, e->pad_stride[0], pred, n, n);
        if (topk && cost < ck[nkc - 1]) {
            int p = nkc - 1;
            while (p > 0 && cost < ck[p - 1]) {
                ck[p] = ck[p - 1]; mk[p] = mk[p - 1];
                p--;
            }
            ck[p] = cost; mk[p] = mode;
        }
        if (cost < best_cost) {
            best_cost = cost;
            best_mode = mode;
        }
    }
    if (topk)
        for (int i = 0; i < nkc; i++)
            topk[i] = mk[i];
    if (cost_out) {
        const uint8_t *r = n265_use_filtered_refs(best_mode, log2n, 0)
                               ? refs_f : refs;
        n265_intra_pred(pred, n, r, log2n, best_mode, 0);
        uint32_t sad = 0;
        for (int y = 0; y < n; y++)
            for (int x = 0; x < n; x++) {
                int d = src[y * e->pad_stride[0] + x] - pred[y * n + x];
                sad += (uint32_t)(d < 0 ? -d : d);
            }
        *cost_out = sad;
    }
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

static int search_luma_mode(struct n265_encoder *e, int x0, int y0,
                            int log2n, const uint8_t *ref_plane,
                            uint32_t *cost_out)
{
    return search_luma_mode_k(e, x0, y0, log2n, ref_plane, cost_out, NULL, 0);
}

/* SAD-domain lambda: sqrt(0.68 * 2^((qp-12)/3)), Q4 fixed point */
static int lambda_sad_q4(int qp)
{
    static const uint16_t tab[52] = {
        3, 4, 4, 5, 5, 6, 7, 7, 8, 9, 10, 12, 13, 15, 17, 19,
        21, 24, 26, 30, 33, 37, 42, 47, 53, 59, 66, 75, 84, 94, 106, 118,
        133, 149, 168, 188, 211, 237, 266, 299, 335, 376, 422, 474, 532, 597, 670, 752,
        844, 948, 1064, 1194
    };
    return tab[qp < 0 ? 0 : qp > 51 ? 51 : qp];
}

static uint32_t mot_sad(struct n265_encoder *e, int x0, int y0, int log2n,
                        mot_t mot);
static void est_tt_bits(struct n265_encoder *e, est_state *st, int log2n,
                        int mode, const tb_ctx *tl, const tb_ctx *tcb,
                        const tb_ctx *tcr);
static void set_mot_map(struct n265_encoder *e, int x0, int y0, int n,
                        mot_t mot);

/* mode-decision lambda 0.68 * 2^((qp-12)/3), Q8 (shared with SAO;
 * N265_LAMBDA overrides the factor) */
uint32_t n265_lambda_mode_q8(int qp)
{
    static uint32_t tab[52];
    if (!tab[51]) {
        for (int q = 0; q < 52; q++) {
            double l = (getenv("N265_LAMBDA") ? atof(getenv("N265_LAMBDA")) : 0.68) * pow(2.0, (q - 12) / 3.0);
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

/* P1, the early-out trio (docs/phase4-ladder.md). All three are off by
 * default; each one reshapes a decision, so each is priced on the band
 * before it becomes a default. */

/* N265_ES_THR: percent scale on the clean-skip distortion bound that
 * ends the quadtree trial. 100 is the shipped rule. */
static int es_thr_pct(void)
{
    static int v = -1;
    if (v < 0) {
        const char *s = getenv("N265_ES_THR");
        v = s ? atoi(s) : 100;
    }
    return v;
}

/* N265_RSKIP: stop the recursion where the no-split winner carried no
 * residual at all. 1 also requires that neither decoded neighbour went
 * deeper than this node; 2 drops that condition. */
static int rskip_mode(void)
{
    static int v = -1;
    if (v < 0) {
        const char *s = getenv("N265_RSKIP");
        v = s ? atoi(s) : 0;
    }
    return v;
}

/* N265_CBF_FAST: once a candidate quantises to nothing, stop trying the
 * expensive ones. Two independent stages: the merge candidate's cbf
 * gating AMVP, intra and the partitions, and the AMVP candidate's cbf
 * gating intra and the partitions. 1 is the first stage, 2 is both,
 * 3 is the second alone, the default: stage 1 costs three times what
 * stage 2 does and buys less per point (docs/phase4-ladder.md). 0 is the
 * pre-P1 encoder. */
static int cbf_fast(void)
{
    static int v = -1;
    if (v < 0) {
        const char *s = getenv("N265_CBF_FAST");
        v = s ? atoi(s) : 3;    /* stage 2, the one that priced (P1) */
    }
    return v;
}

/* N265_DEPTH_BOUND (P2): bound the depth range the quadtree searches to
 * the range the decoded neighbours and the collocated CTU already used.
 * 0 off; 1 widens that range by one each way; 3 takes it as it stands.
 * 2 and 4 are 1 and 3 plus the whole-CTU prediction-only skip trial at
 * depth 0 even where the bound forbids the CTB depth. 5 and 6 keep only
 * the upper half of the bound and read it locally, off the 4x4 cells
 * immediately left of and above the node rather than off whole
 * neighbouring CTUs: 5 stops one level below the deepest of them, 6 at
 * their depth. */
static int depth_bound(void)
{
    static int v = -1;
    if (v < 0) {
        const char *s = getenv("N265_DEPTH_BOUND");
        v = s ? atoi(s) : 0;
    }
    return v;
}

/* how far the bound is widened each way, per mode */
static int depth_bound_widen(void)
{
    return depth_bound() >= 3 ? 0 : 1;
}

/* modes 5 and 6: the CTU-range bound is replaced by a local one */
static int depth_bound_local(void)
{
    return depth_bound() >= 5;
}

/* The deepest the decoded 4x4 cells immediately left of and above this
 * node went. Both edges are available in decode order - the same
 * neighbours split_inc and the AMVP derivation read - and the split
 * trial restores the map, so this reads the committed tree only. With
 * no decoded neighbour at all it does not bound. */
static int nb_depth_max(struct n265_encoder *e, int x0, int y0, int n)
{
    int cells = n >> 2, hi = -1;
    if (x0 > 0) {
        int cx = (x0 - 1) >> 2;
        for (int i = 0; i < cells; i++) {
            int d = e->ct_depth[((y0 >> 2) + i) * e->map_stride + cx];
            if (d > hi) hi = d;
        }
    }
    if (y0 > 0) {
        int cy = (y0 - 1) >> 2;
        for (int i = 0; i < cells; i++) {
            int d = e->ct_depth[cy * e->map_stride + ((x0 >> 2) + i)];
            if (d > hi) hi = d;
        }
    }
    return hi;
}

/* whether the CTB skip trial survives a bound that forbids depth 0 */
static int depth_bound_ctb_skip(void)
{
    return depth_bound() == 2 || depth_bound() == 4;
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
    int qp_c = n265_chroma_qp(e->cur_ctu_qp);
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
            int cbf_l = tb_full(e, 0, x1, y1, log2ts - 1, e->cur_ctu_qp, mode, &tb);
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

    /* <= 32: choose flat TU == CU vs four TUs with sequential intra
     * prediction (each TU predicts from the recon of the previous ones,
     * so the split arm commits as it goes and restores on loss) */
    int n = 1 << log2ts;
    mark_cu_edges(e, x0, y0, n);
    uint32_t lam = n265_lambda_mode_q8(e->cur_ctu_qp);
    int h = n >> 1;
    int child_chroma = log2ts > 3;

    /* flat arrangement (no frame commits yet) */
    tb_ctx tcb, tcr;
    tb_analyze(e, 1, x0 / 2, y0 / 2, log2ts - 1, qp_c, mode, mot_zero, &tcb);
    tb_analyze(e, 2, x0 / 2, y0 / 2, log2ts - 1, qp_c, mode, mot_zero, &tcr);
    tb_analyze(e, 0, x0, y0, log2ts, e->cur_ctu_qp, mode, mot_zero, &tb);
    tb_make_recon(0, log2ts, e->cur_ctu_qp, mode, &tb);
    tb_make_recon(1, log2ts - 1, qp_c, mode, &tcb);
    tb_make_recon(2, log2ts - 1, qp_c, mode, &tcr);
    uint64_t d_flat = tb_ssd(e, 0, x0, y0, log2ts, &tb) +
        tb_ssd(e, 1, x0 / 2, y0 / 2, log2ts - 1, &tcb) +
        tb_ssd(e, 2, x0 / 2, y0 / 2, log2ts - 1, &tcr);
    cabac_ctx_t local[CTX_COUNT];
    est_state st = { local, 0 };
    memcpy(local, e->est_ctx, sizeof(local));
    st.bits = 0;
    est_tt_bits(e, &st, log2ts, mode, &tb, &tcb, &tcr);
    uint64_t j_flat = rd_j(d_flat, st.bits, lam);

    /* split arrangement: sequential, committing recon TU by TU */
    uint8_t save_l[32 * 32], save_c[2][16 * 16];
    uint8_t split_l[32 * 32], split_c[2][16 * 16];
    for (int y = 0; y < n; y++)
        memcpy(save_l + y * n,
               e->recon[0] + (y0 + y) * e->recon_stride[0] + x0, (size_t)n);
    for (int c = 0; c < 2; c++)
        for (int y = 0; y < n / 2; y++)
            memcpy(save_c[c] + y * (n / 2),
                   e->recon[c + 1] + (y0 / 2 + y) * e->recon_stride[c + 1] +
                       x0 / 2,
                   (size_t)(n / 2));

    static tb_ctx s_l[4], s_c[4][2];
    uint64_t d_split = 0;
    int root_cb = 0, root_cr = 0;
    for (int i = 0; i < 4; i++) {
        int x1 = x0 + (i & 1) * h, y1 = y0 + (i >> 1) * h;
        tb_analyze(e, 0, x1, y1, log2ts - 1, e->cur_ctu_qp, mode, mot_zero,
                   &s_l[i]);
        tb_recon(e, 0, x1, y1, log2ts - 1, e->cur_ctu_qp, mode, &s_l[i]);
        d_split += tb_ssd(e, 0, x1, y1, log2ts - 1, &s_l[i]);
        if (child_chroma) {
            tb_analyze(e, 1, x1 / 2, y1 / 2, log2ts - 2, qp_c, mode,
                       mot_zero, &s_c[i][0]);
            tb_recon(e, 1, x1 / 2, y1 / 2, log2ts - 2, qp_c, mode,
                     &s_c[i][0]);
            tb_analyze(e, 2, x1 / 2, y1 / 2, log2ts - 2, qp_c, mode,
                       mot_zero, &s_c[i][1]);
            tb_recon(e, 2, x1 / 2, y1 / 2, log2ts - 2, qp_c, mode,
                     &s_c[i][1]);
            d_split += tb_ssd(e, 1, x1 / 2, y1 / 2, log2ts - 2,
                              &s_c[i][0]);
            d_split += tb_ssd(e, 2, x1 / 2, y1 / 2, log2ts - 2,
                              &s_c[i][1]);
            root_cb |= s_c[i][0].nz != 0;
            root_cr |= s_c[i][1].nz != 0;
        }
    }
    if (!child_chroma) {
        /* CB8: chroma stays a single 4x4 at the parent, predicted after
         * the luma children per decode order */
        tb_analyze(e, 1, x0 / 2, y0 / 2, 2, qp_c, mode, mot_zero,
                   &s_c[0][0]);
        tb_recon(e, 1, x0 / 2, y0 / 2, 2, qp_c, mode, &s_c[0][0]);
        tb_analyze(e, 2, x0 / 2, y0 / 2, 2, qp_c, mode, mot_zero,
                   &s_c[0][1]);
        tb_recon(e, 2, x0 / 2, y0 / 2, 2, qp_c, mode, &s_c[0][1]);
        d_split += tb_ssd(e, 1, x0 / 2, y0 / 2, 2, &s_c[0][0]);
        d_split += tb_ssd(e, 2, x0 / 2, y0 / 2, 2, &s_c[0][1]);
        root_cb = s_c[0][0].nz != 0;
        root_cr = s_c[0][1].nz != 0;
    }
    /* keep the split recon, restore the pre-CU state */
    for (int y = 0; y < n; y++) {
        memcpy(split_l + y * n,
               e->recon[0] + (y0 + y) * e->recon_stride[0] + x0, (size_t)n);
        memcpy(e->recon[0] + (y0 + y) * e->recon_stride[0] + x0,
               save_l + y * n, (size_t)n);
    }
    for (int c = 0; c < 2; c++)
        for (int y = 0; y < n / 2; y++) {
            memcpy(split_c[c] + y * (n / 2),
                   e->recon[c + 1] + (y0 / 2 + y) * e->recon_stride[c + 1] +
                       x0 / 2,
                   (size_t)(n / 2));
            memcpy(e->recon[c + 1] + (y0 / 2 + y) * e->recon_stride[c + 1] +
                       x0 / 2,
                   save_c[c] + y * (n / 2), (size_t)(n / 2));
        }

    memcpy(local, e->est_ctx, sizeof(local));
    st.bits = 0;
    {
        res_ops ops;
        n265_est_ops(&ops, &st);
        st.bits += n265_est_bin(&local[CTX_TSPLIT + 5 - log2ts], 1);
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
            st.bits += n265_est_bin(&local[CTX_CBF_LUMA + 0],
                                    s_l[i].nz != 0);
            if (s_l[i].nz)
                n265_residual_coding(&ops, s_l[i].level, log2ts - 1, 0,
                                     n265_scan_idx(mode, log2ts - 1, 0));
            if (child_chroma) {
                int sc = n265_scan_idx(mode, log2ts - 2, 1);
                if (s_c[i][0].nz)
                    n265_residual_coding(&ops, s_c[i][0].level, log2ts - 2,
                                         1, sc);
                if (s_c[i][1].nz)
                    n265_residual_coding(&ops, s_c[i][1].level, log2ts - 2,
                                         2, sc);
            } else if (i == 3) {
                int sc = n265_scan_idx(mode, 2, 1);
                if (root_cb)
                    n265_residual_coding(&ops, s_c[0][0].level, 2, 1, sc);
                if (root_cr)
                    n265_residual_coding(&ops, s_c[0][1].level, 2, 2, sc);
            }
        }
    }
    uint64_t j_split = rd_j(d_split, st.bits, lam);

    if (j_flat <= j_split) {
        rec_u8(e, 0);
        rec_u8(e, tcb.nz != 0);
        rec_u8(e, tcr.nz != 0);
        rec_u8(e, tb.nz != 0);
        tb_commit(e, 0, x0, y0, log2ts, &tb);
        if (tb.nz)
            rec_levels(e, tb.level, n * n);
        tb_commit(e, 1, x0 / 2, y0 / 2, log2ts - 1, &tcb);
        if (tcb.nz)
            rec_levels(e, tcb.level, (n / 2) * (n / 2));
        tb_commit(e, 2, x0 / 2, y0 / 2, log2ts - 1, &tcr);
        if (tcr.nz)
            rec_levels(e, tcr.level, (n / 2) * (n / 2));
        return;
    }

    /* commit split: blit the saved split recon and write records */
    for (int y = 0; y < n; y++)
        memcpy(e->recon[0] + (y0 + y) * e->recon_stride[0] + x0,
               split_l + y * n, (size_t)n);
    for (int c = 0; c < 2; c++)
        for (int y = 0; y < n / 2; y++)
            memcpy(e->recon[c + 1] + (y0 / 2 + y) * e->recon_stride[c + 1] +
                       x0 / 2,
                   split_c[c] + y * (n / 2), (size_t)(n / 2));
    rec_u8(e, 1);
    rec_u8(e, (unsigned)root_cb);
    rec_u8(e, (unsigned)root_cr);
    for (int i = 0; i < 4; i++) {
        int x1 = x0 + (i & 1) * h, y1 = y0 + (i >> 1) * h;
        mark_cu_edges(e, x1, y1, h);
        set_blk(e, e->blk_cbf_y, x1, y1, h, s_l[i].nz != 0);
        if (child_chroma) {
            rec_u8(e, s_c[i][0].nz != 0);
            rec_u8(e, s_c[i][1].nz != 0);
        }
        rec_u8(e, s_l[i].nz != 0);
        if (s_l[i].nz)
            rec_levels(e, s_l[i].level, h * h);
        if (child_chroma) {
            if (s_c[i][0].nz)
                rec_levels(e, s_c[i][0].level, (h / 2) * (h / 2));
            if (s_c[i][1].nz)
                rec_levels(e, s_c[i][1].level, (h / 2) * (h / 2));
        } else if (i == 3) {
            if (root_cb)
                rec_levels(e, s_c[0][0].level, 16);
            if (root_cr)
                rec_levels(e, s_c[0][1].level, 16);
        }
    }
}

static void set_mot_map_wh(struct n265_encoder *e, int x0, int y0, int w,
                           int h, mot_t mot)
{
    for (int y = 0; y < h; y += 4)
        for (int x = 0; x < w; x += 4) {
            int m = ((y0 + y) >> 2) * e->map_stride + ((x0 + x) >> 2);
            e->blk_mv[4 * m] = mot.mv[0].x;
            e->blk_mv[4 * m + 1] = mot.mv[0].y;
            e->blk_mv[4 * m + 2] = mot.mv[1].x;
            e->blk_mv[4 * m + 3] = mot.mv[1].y;
            e->blk_pred[m] = mot.pred;
            e->blk_ref[2 * m] = mot.ref[0];
            e->blk_ref[2 * m + 1] = mot.ref[1];
        }
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
            e->blk_ref[2 * m] = mot.ref[0];
            e->blk_ref[2 * m + 1] = mot.ref[1];
        }
}

static void skip_cu(struct n265_encoder *e, int x0, int y0, int log2n,
                    mot_t mot, int merge_idx)
{
    int n = 1 << log2n;
    rec_u8(e, (unsigned)merge_idx);
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
static uint32_t mot_satd_wh(struct n265_encoder *e, int x0, int y0, int w,
                            int h, mot_t mot)
{
    uint8_t pred[64 * 64];
    mc_pred_wh(e, 0, x0, y0, w, h, mot, pred, w);
    return n265_satd_wh(e->pad_plane[0] + y0 * e->pad_stride[0] + x0,
                        e->pad_stride[0], pred, w, w, h);
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

/* mvd_coding (7.3.8.9): the two greater-flags on their own contexts, the
 * EG1 remainder and the sign in bypass. The contexts are advanced, so a
 * caller that only wants a PRICE passes copies. Q8 bits. */
static uint32_t mvd_cost_q8(cabac_ctx_t *g0, cabac_ctx_t *g1, int mx, int my)
{
    int ax = mx < 0 ? -mx : mx, ay = my < 0 ? -my : my;
    uint32_t b = 0;
    b += n265_est_bin(g0, ax > 0);
    b += n265_est_bin(g0, ay > 0);
    if (ax > 0)
        b += n265_est_bin(g1, ax > 1);
    if (ay > 0)
        b += n265_est_bin(g1, ay > 1);
    for (int c = 0; c < 2; c++) {
        int a = c ? ay : ax;
        if (a > 0) {
            if (a > 1) {
                int v = a - 2, bl = 1, bits = 1;
                while (v >= (1 << bl)) { v -= 1 << bl; bl++; bits += 2; }
                b += n265_est_bypass_bits(bits + bl - 1);
            }
            b += n265_est_bypass_bits(1);
        }
    }
    return b;
}

static void est_mvd(est_state *st, int mx, int my)
{
    st->bits += mvd_cost_q8(&st->ctx[CTX_MVD_GR0], &st->ctx[CTX_MVD_GR1],
                            mx, my);
}

/* --- the motion-candidate rate model (N265_MVD_BITS, N265_BI_REFINE) --- */

static int mvd_bits_on(void)
{
    static int on = -1;
    if (on < 0) {
        const char *v = getenv("N265_MVD_BITS");
        on = v ? atoi(v) != 0 : 0;
    }
    return on;
}

/* Hadamard instead of SAD in the 2Nx2N reference ranking and the
 * uni/bi arbitration (the rect/AMP PU search already uses it) */
static int me_satd_on(void)
{
    static int on = -1;
    if (on < 0) {
        const char *v = getenv("N265_ME_SATD");
        on = v ? atoi(v) != 0 : 0;
    }
    return on;
}

/* AMVP 2Nx2N at the CB64 node (the quadtree's log2n == 6 arm set). On
 * since 2026-09-07: -0.38 median BD-VMAF-NEG on the CIF band at 1.06-1.09x
 * the encode time; N265_CB64_AMVP=0 is the escape. */
static int cb64_amvp(void)
{
    static int on = -1;
    if (on < 0) {
        const char *v = getenv("N265_CB64_AMVP");
        on = v ? atoi(v) != 0 : 1;
    }
    return on;
}

/* percent by which an intra CU must beat the best inter cost in a B
 * slice to be admitted; N265_B_INTRA_MARGIN=0 is the plain comparison.
 * 30 since 2026-09-07: -0.21 median BD-VMAF-NEG on the CIF band for no
 * measurable time. The optimum is broad - 15, 30 and 60 land within
 * 0.04 of each other - so this is a shape, not a tuned constant. */
static int b_intra_margin(void)
{
    static int mg = -1;
    if (mg < 0) {
        const char *v = getenv("N265_B_INTRA_MARGIN");
        mg = v ? atoi(v) : 30;
        if (mg < 0)
            mg = 0;
    }
    return mg;
}

/* number of L0/L1 refinement sweeps; N265_BI_REFINE=0 is the escape.
 * One sweep since 2026-09-07: -0.29 mean BD-VMAF-NEG on the CIF band
 * (6/7 clips, bus flat) at 1.05x the encode time. A second sweep buys
 * another 0.02 mean for another 4%, so one is where it stops paying. */
static int bi_refine_iters(void)
{
    static int it = -1;
    if (it < 0) {
        const char *v = getenv("N265_BI_REFINE");
        it = v ? atoi(v) : 1;
        if (it > 4)
            it = 4;
    }
    return it;
}

/* L1 distance to the nearer of the two AMVP predictors: the historical
 * proxy's "one bit per quarter-pel" measure, and the mvp_lX_flag choice */
static int mvd_proxy(mv_t mv, const mv_t p[2])
{
    int c0 = abs(mv.x - p[0].x) + abs(mv.y - p[0].y);
    int c1 = abs(mv.x - p[1].x) + abs(mv.y - p[1].y);
    return c1 < c0 ? c1 : c0;
}

static int mvp_pick(mv_t mv, const mv_t p[2])
{
    return abs(mv.x - p[1].x) + abs(mv.y - p[1].y) <
           abs(mv.x - p[0].x) + abs(mv.y - p[0].y);
}

/* ref_idx_lX (truncated unary, two contexts then bypass) in Q8 bits */
static uint32_t ref_bits_q8(struct n265_encoder *e, int list, int r)
{
    int cmax = e->nref[list] - 1;
    if (cmax < 1)
        return 0;
    cabac_ctx_t c0 = e->est_ctx[CTX_REF_IDX + 0];
    cabac_ctx_t c1 = e->est_ctx[CTX_REF_IDX + 1];
    uint32_t b = n265_est_bin(&c0, r > 0);
    if (r > 0 && cmax > 1)
        b += n265_est_bin(&c1, r > 1);
    if (r > 1 && cmax > 2)
        b += 256u * (uint32_t)((r < cmax ? r : cmax - 1) - 1);
    return b;
}

/* inter_pred_idc in Q8 bits (9.3.3.8: bin 0 on the CU depth context, the
 * uni-direction bin at index 4; an 8x4/4x8 PU codes only the latter) */
static uint32_t idc_bits_q8(struct n265_encoder *e, int log2cb, int pw, int ph,
                            int pred)
{
    if (e->slice_type != 0)
        return 0;
    uint32_t b = 0;
    if (pw + ph != 12) {
        cabac_ctx_t c = e->est_ctx[CTX_INTER_IDC + (e->ctb_log2 - log2cb)];
        b += n265_est_bin(&c, pred == 3);
        if (pred == 3)
            return b;
    }
    cabac_ctx_t c4 = e->est_ctx[CTX_INTER_IDC + 4];
    b += n265_est_bin(&c4, pred >> 1);
    return b;
}

/* Motion syntax rate of one AMVP candidate, Q8 bits, for the SATD-domain
 * candidate ranking. With N265_MVD_BITS off this is the historical proxy
 * - one bit per quarter-pel of MVD distance plus two per reference index
 * plus `base` - written in Q8 so the call sites are shared; the two are
 * numerically identical there (a Q8 value scaled by lambda_sad >> 12 is
 * the old expression scaled >> 4). */
static uint32_t mot_rate_q8(struct n265_encoder *e, const pu_geo *g,
                            const mot_t *m, mv_t mvp[2][2], int base)
{
    uint32_t b = 0;
    int real = mvd_bits_on();
    if (real)
        b += idc_bits_q8(e, 31 - __builtin_clz((unsigned)g->cbn), g->w, g->h,
                         m->pred);
    for (int l = 0; l < 2; l++) {
        if (!(m->pred & (1 << l)))
            continue;
        if (!real) {
            b += (uint32_t)(mvd_proxy(m->mv[l], mvp[l]) + 2 * m->ref[l] +
                            base) << 8;
            continue;
        }
        int k = mvp_pick(m->mv[l], mvp[l]);
        cabac_ctx_t g0 = e->est_ctx[CTX_MVD_GR0];
        cabac_ctx_t g1 = e->est_ctx[CTX_MVD_GR1];
        cabac_ctx_t mf = e->est_ctx[CTX_MVP_FLAG];
        b += mvd_cost_q8(&g0, &g1, m->mv[l].x - mvp[l][k].x,
                         m->mv[l].y - mvp[l][k].y);
        b += n265_est_bin(&mf, k);
        b += ref_bits_q8(e, l, m->ref[l]);
    }
    return b;
}

/* SATD of a bi prediction whose fixed list is already interpolated into
 * fix14: only the moving list pays an MC. */
static uint32_t bi_satd_fixed(struct n265_encoder *e, int x0, int y0, int w,
                              int h, const int16_t *fix14, int list, int ref,
                              mv_t mv)
{
    int16_t a[64 * 64];
    uint8_t pred[64 * 64];
    mc_pred14(e, list, ref, 0, x0, y0, w, h, mv, a);
    n265_bi_avg_wh(pred, w, a, fix14, w, h);
    return n265_satd_wh(e->pad_plane[0] + y0 * e->pad_stride[0] + x0,
                        e->pad_stride[0], pred, w, w, h);
}

/* Bi-prediction refinement (N265_BI_REFINE). Pairing the two independent
 * per-list winners optimises each half against the SOURCE; the average
 * they form is never searched. Holding one list fixed makes the bi
 * prediction an affine function of the other, so the pair walks downhill
 * on the combined SATD under a small quarter-pel square pattern, one
 * list at a time. A zero-MV bi probe goes first: it is the one pair the
 * per-list winners cannot produce once either list has drifted, and it
 * costs one SATD.
 *
 * `bi` and `mvp` are updated in place; the caller keeps its own copies
 * until it knows the bi candidate won, because a probe that changes a
 * reference index changes the predictor pair with it and a stale mvp
 * would make the decoder reconstruct a different vector. */
static uint32_t bi_refine(struct n265_encoder *e, const pu_geo *g, mot_t *bi,
                          mv_t mvp[2][2], int lam4, int base)
{
    int w = g->w, h = g->h, x0 = g->x, y0 = g->y;
    int16_t fix[64 * 64];
    uint32_t best = mot_satd_wh(e, x0, y0, w, h, *bi) +
        (uint32_t)(((uint64_t)mot_rate_q8(e, g, bi, mvp, base) * lam4) >> 12);

    if (bi->mv[0].x || bi->mv[0].y || bi->mv[1].x || bi->mv[1].y ||
        bi->ref[0] || bi->ref[1]) {
        mot_t z = { { { 0, 0 }, { 0, 0 } }, 3, { 0, 0 } };
        mv_t zp[2][2];
        n265_amvp_pu(e, 0, 0, g, zp[0]);
        n265_amvp_pu(e, 1, 0, g, zp[1]);
        uint32_t c = mot_satd_wh(e, x0, y0, w, h, z) +
            (uint32_t)(((uint64_t)mot_rate_q8(e, g, &z, zp, base) * lam4) >> 12);
        if (c < best) {
            best = c;
            *bi = z;
            memcpy(mvp, zp, sizeof(zp));
        }
    }

    int iters = bi_refine_iters();
    for (int it = 0; it < iters; it++) {
        int moved = 0;
        for (int l = 0; l < 2; l++) {
            int o = !l;
            mc_pred14(e, o, bi->ref[o], 0, x0, y0, w, h, bi->mv[o], fix);
            mot_t t = *bi;
            mv_t bmv = bi->mv[l];
            for (int dy = -1; dy <= 1; dy++)
                for (int dx = -1; dx <= 1; dx++) {
                    if (!dx && !dy)
                        continue;
                    mv_t v = { (int16_t)(bi->mv[l].x + dx),
                               (int16_t)(bi->mv[l].y + dy) };
                    mv_t vc = v;
                    n265_clamp_mv(e, x0, y0, w, h, &vc);
                    if (vc.x != v.x || vc.y != v.y)
                        continue;
                    t.mv[l] = v;
                    uint32_t c = bi_satd_fixed(e, x0, y0, w, h, fix, l,
                                               bi->ref[l], v) +
                        (uint32_t)(((uint64_t)mot_rate_q8(e, g, &t, mvp, base)
                                    * lam4) >> 12);
                    if (c < best) {
                        best = c;
                        bmv = v;
                    }
                }
            if (bmv.x != bi->mv[l].x || bmv.y != bi->mv[l].y) {
                bi->mv[l] = bmv;
                moved = 1;
            }
        }
        if (!moved)
            break;
    }
    return best;
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
    int qp_c = n265_chroma_qp(e->cur_ctu_qp);
    uint32_t lam = n265_lambda_mode_q8(e->cur_ctu_qp);
    int h = n >> 1;
    int child_chroma = log2n > 3;
    static tb_ctx s_l[4], s_c[4][2];

    /* split arrangement */
    uint64_t d_split = 0;
    int root_cb = 0, root_cr = 0;
    for (int i = 0; i < 4; i++) {
        int x1 = x0 + (i & 1) * h, y1 = y0 + (i >> 1) * h;
        tb_analyze(e, 0, x1, y1, log2n - 1, e->cur_ctu_qp, MODE_INTER, mot,
                   &s_l[i]);
        tb_make_recon(0, log2n - 1, e->cur_ctu_qp, MODE_INTER, &s_l[i]);
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

    /* past maxTB the split is inferred and the flat arrangement does
     * not exist (CB64: forced 4x TU32) */
    int flat_ok = log2n <= 5;
    uint64_t d_flat = !flat_ok ? 0 :
        tb_ssd(e, 0, x0, y0, log2n, tl) +
        tb_ssd(e, 1, x0 / 2, y0 / 2, log2n - 1, tcb) +
        tb_ssd(e, 2, x0 / 2, y0 / 2, log2n - 1, tcr);

    /* bits, both arrangements, on estimator copies */
    cabac_ctx_t local[CTX_COUNT];
    est_state st;
    res_ops ops;

    uint64_t j_flat = UINT64_MAX;
    if (flat_ok) {
        memcpy(local, e->est_ctx, sizeof(local));
        st.ctx = local; st.bits = 0;
        n265_est_ops(&ops, &st);
        est_tt_bits(e, &st, log2n, MODE_INTER, tl, tcb, tcr);
        j_flat = rd_j(d_flat, st.bits, lam);
    }

    memcpy(local, e->est_ctx, sizeof(local));
    st.ctx = local; st.bits = 0;
    n265_est_ops(&ops, &st);
    if (flat_ok)
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

    /* commit split: record layout mirrors replay_tt (which reads no
     * split byte where the split is inferred past maxTB) */
    if (flat_ok)
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

/* the three most probable modes for this position (9.3.3.6), the list the
 * mode's own syntax cost is built on */
static void mpm_cands(struct n265_encoder *e, int x0, int y0, int cand[3])
{
    int a = INTRA_DC, b = INTRA_DC;
    if (x0 > 0 && e->blk_intra[(y0 >> 2) * e->map_stride + ((x0 - 1) >> 2)])
        a = e->intra_mode[(y0 >> 2) * e->map_stride + ((x0 - 1) >> 2)];
    if (y0 > 0 && (y0 & ((1 << e->ctb_log2) - 1)) != 0 &&
        e->blk_intra[((y0 - 1) >> 2) * e->map_stride + (x0 >> 2)])
        b = e->intra_mode[((y0 - 1) >> 2) * e->map_stride + (x0 >> 2)];
    if (a == b) {
        if (a < 2) { cand[0] = 0; cand[1] = 1; cand[2] = 26; }
        else { cand[0] = a; cand[1] = 2 + ((a + 29) % 32);
               cand[2] = 2 + ((a - 2 + 1) % 32); }
    } else {
        cand[0] = a; cand[1] = b;
        cand[2] = (a != 0 && b != 0) ? 0 : (a != 1 && b != 1) ? 1 : 26;
    }
}

/* shortlist width for the intra RD stage (N265_INTRA_RD_N, 3 by default)
 * and whether the MPMs join it (N265_INTRA_MPM) */
static int intra_rd_n(void)
{
    static int v = -1;
    if (v < 0) {
        const char *s = getenv("N265_INTRA_RD_N");
        v = s ? atoi(s) : 3;
        if (v < 1) v = 1;
        if (v > INTRA_RD_MAX) v = INTRA_RD_MAX;
    }
    return v;
}

static int intra_rd_mpm(void)
{
    static int v = -1;
    if (v < 0) { const char *s = getenv("N265_INTRA_MPM"); v = s ? atoi(s) : 0; }
    return v;
}

/* RD-refine the intra luma mode over the SATD-ranked shortlist: only
 * mode-dependent bits (MPM, cbfs, residuals) enter the compare */
static int intra_rd_pick(struct n265_encoder *e, int x0, int y0, int log2n)
{
    int cands[INTRA_RD_MAX + 3];
    int nk = intra_rd_n(), ncand = nk;
    search_luma_mode_k(e, x0, y0, log2n, e->recon[0], NULL, cands, nk);
    if (intra_rd_mpm()) {
        /* an MPM costs two bits where a remainder costs six, so a mode the
         * SATD prefilter ranked just outside can still win on J */
        int mpm[3];
        mpm_cands(e, x0, y0, mpm);
        for (int i = 0; i < 3; i++) {
            int dup = 0;
            for (int q = 0; q < ncand; q++)
                if (cands[q] == mpm[i])
                    dup = 1;
            if (!dup)
                cands[ncand++] = mpm[i];
        }
    }
    int qp_c = n265_chroma_qp(e->cur_ctu_qp);
    uint32_t lam = n265_lambda_mode_q8(e->cur_ctu_qp);
    static tb_ctx tl, tcb, tcr;
    uint64_t best_j = UINT64_MAX;
    int best = cands[0];
    for (int k = 0; k < ncand; k++) {
        int mode = cands[k];
        int dup = 0;
        for (int q = 0; q < k; q++)
            if (cands[q] == mode)
                dup = 1;
        if (dup)
            continue;
        tb_analyze(e, 1, x0 / 2, y0 / 2, log2n - 1, qp_c, mode, mot_zero,
                   &tcb);
        tb_analyze(e, 2, x0 / 2, y0 / 2, log2n - 1, qp_c, mode, mot_zero,
                   &tcr);
        tb_analyze(e, 0, x0, y0, log2n, e->cur_ctu_qp, mode, mot_zero, &tl);
        tb_make_recon(0, log2n, e->cur_ctu_qp, mode, &tl);
        tb_make_recon(1, log2n - 1, qp_c, mode, &tcb);
        tb_make_recon(2, log2n - 1, qp_c, mode, &tcr);
        uint64_t d = tb_ssd(e, 0, x0, y0, log2n, &tl) +
            tb_ssd(e, 1, x0 / 2, y0 / 2, log2n - 1, &tcb) +
            tb_ssd(e, 2, x0 / 2, y0 / 2, log2n - 1, &tcr);
        cabac_ctx_t local[CTX_COUNT];
        memcpy(local, e->est_ctx, sizeof(local));
        est_state st = { local, 0 };
        res_ops ops;
        n265_est_ops(&ops, &st);
        est_mpm_bits(e, &st, x0, y0, mode);
        st.bits += n265_est_bin(&local[CTX_CBF_CHROMA + 0], tcb.nz != 0);
        st.bits += n265_est_bin(&local[CTX_CBF_CHROMA + 0], tcr.nz != 0);
        st.bits += n265_est_bin(&local[CTX_CBF_LUMA + 1], tl.nz != 0);
        if (tl.nz)
            n265_residual_coding(&ops, tl.level, log2n, 0,
                                 n265_scan_idx(mode, log2n, 0));
        int sc = n265_scan_idx(mode, log2n - 1, 1);
        if (tcb.nz)
            n265_residual_coding(&ops, tcb.level, log2n - 1, 1, sc);
        if (tcr.nz)
            n265_residual_coding(&ops, tcr.level, log2n - 1, 2, sc);
        uint64_t j = rd_j(d, st.bits, lam);
        if (j < best_j) {
            best_j = j;
            best = mode;
        }
    }
    return best;
}

static uint64_t cu_area_ssd(struct n265_encoder *e, int x0, int y0, int n)
{
    uint64_t ssd = 0;
    if (e->psy > 0.0)       /* the same psy term the TB candidates carry */
        ssd += n265_psy_cost(e->pad_plane[0] + y0 * e->pad_stride[0] + x0, e->pad_stride[0],
                             e->recon[0] + y0 * e->recon_stride[0] + x0, e->recon_stride[0],
                             n, n265_lambda_mode_q8(e->cur_ctu_qp), e->psy);
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
    return (uint64_t)((double)n * n * qstep * qstep / 4.0 *
                      es_thr_pct() / 100.0);
}

/* --- rectangular and asymmetric inter partitions --- */

/* one PU's motion decision */
typedef struct pu_dec {
    mot_t mot;
    int merge;
    int midx;
    int mvp_idx[2];
    mv_t mvp[2][2];     /* the AMVP predictor pair, per list */
} pu_dec;

/* the CU's motion maps across a partition trial. The first PU's motion
 * has to be visible to the second PU's AMVP derivation - 6.4.2 makes a
 * neighbour inside the same coding block available, and A1 of a vertical
 * split (B1 of a horizontal one) is exactly that - so every trial writes
 * the maps and then puts them back. */
typedef struct mot_snap {
    int16_t mv[16 * 16 * 4];
    uint8_t pred[16 * 16];
    int8_t ref[16 * 16 * 2];
    uint8_t intra[16 * 16];
} mot_snap;

static void mot_save(struct n265_encoder *e, int x0, int y0, int n,
                     mot_snap *sn)
{
    int cells = n >> 2;
    for (int y = 0; y < cells; y++) {
        int m = ((y0 >> 2) + y) * e->map_stride + (x0 >> 2);
        memcpy(sn->mv + y * cells * 4, e->blk_mv + 4 * m,
               (size_t)cells * 4 * sizeof(int16_t));
        memcpy(sn->pred + y * cells, e->blk_pred + m, (size_t)cells);
        memcpy(sn->ref + y * cells * 2, e->blk_ref + 2 * m,
               (size_t)cells * 2);
        memcpy(sn->intra + y * cells, e->blk_intra + m, (size_t)cells);
    }
}

static void mot_load(struct n265_encoder *e, int x0, int y0, int n,
                     const mot_snap *sn)
{
    int cells = n >> 2;
    for (int y = 0; y < cells; y++) {
        int m = ((y0 >> 2) + y) * e->map_stride + (x0 >> 2);
        memcpy(e->blk_mv + 4 * m, sn->mv + y * cells * 4,
               (size_t)cells * 4 * sizeof(int16_t));
        memcpy(e->blk_pred + m, sn->pred + y * cells, (size_t)cells);
        memcpy(e->blk_ref + 2 * m, sn->ref + y * cells * 2,
               (size_t)cells * 2);
        memcpy(e->blk_intra + m, sn->intra + y * cells, (size_t)cells);
    }
}

/* one PU: the merge list against an AMVP search, both ranked by SATD of
 * the prediction plus lambda x a syntax proxy. The 2Nx2N winner's MVs
 * seed the search where they beat the AMVP predictor. */
static uint32_t search_pu(struct n265_encoder *e, const pu_geo *g,
                          const mv_t start[2], pu_dec *d)
{
    int lam4 = lambda_sad_q4(e->cur_ctu_qp);
    int w = g->w, h = g->h;
    int no_bi = w + h == 12;    /* 8x4 and 4x8 PUs stay uni-predicted */

    mot_t mlist[MAX_MERGE];
    n265_merge_list_pu(e, g, mlist, MAX_MERGE);
    int best_m = 0;
    uint32_t best_mc = UINT32_MAX;
    for (int i = 0; i < MAX_MERGE; i++) {
        uint32_t c = mot_satd_wh(e, g->x, g->y, w, h, mlist[i]) +
                     (uint32_t)(((i + 1) * lam4) >> 4);
        if (c < best_mc) {
            best_mc = c;
            best_m = i;
        }
    }

    mv_t me[2] = { { 0, 0 }, { 0, 0 } }, mvp[2][2];
    int8_t bref[2] = { 0, 0 };
    uint32_t side[2] = { UINT32_MAX, UINT32_MAX };
    int nl = e->slice_type == 0 ? 2 : 1;
    for (int l = 0; l < 2; l++) {
        mvp[l][0].x = mvp[l][0].y = mvp[l][1].x = mvp[l][1].y = 0;
        if (l >= nl)
            continue;
        for (int r = 0; r < e->nref[l]; r++) {
            mv_t p[2], mv;
            n265_amvp_pu(e, l, r, g, p);
            mv_t seed = p[0];
            if (start && (start[l].x != p[0].x || start[l].y != p[0].y) &&
                n265_mc_sad(e, l, r, g->x, g->y, w, h, start[l]) <
                    n265_mc_sad(e, l, r, g->x, g->y, w, h, p[0]))
                seed = start[l];
            n265_me_search(e, l, r, g->x, g->y, w, h, seed, &mv);
            mot_t u;
            u.mv[0].x = u.mv[0].y = u.mv[1].x = u.mv[1].y = 0;
            u.ref[0] = u.ref[1] = 0;
            u.pred = (uint8_t)(1 << l);
            u.mv[l] = mv;
            u.ref[l] = (int8_t)r;
            mv_t up[2][2] = { { { 0, 0 }, { 0, 0 } },
                              { { 0, 0 }, { 0, 0 } } };
            up[l][0] = p[0];
            up[l][1] = p[1];
            uint32_t c = mot_satd_wh(e, g->x, g->y, w, h, u) +
                (uint32_t)(((uint64_t)mot_rate_q8(e, g, &u, up, 6) * lam4)
                           >> 12);
            if (c < side[l]) {
                side[l] = c;
                me[l] = mv;
                bref[l] = (int8_t)r;
                mvp[l][0] = p[0];
                mvp[l][1] = p[1];
            }
        }
    }

    mot_t amot;
    amot.mv[0] = me[0];
    amot.mv[1].x = amot.mv[1].y = 0;
    amot.ref[0] = bref[0];
    amot.ref[1] = 0;
    amot.pred = 1;
    uint32_t acost = side[0];
    if (nl == 2) {
        if (side[1] < acost) {
            amot.mv[0].x = amot.mv[0].y = 0;
            amot.ref[0] = 0;
            amot.mv[1] = me[1];
            amot.ref[1] = bref[1];
            amot.pred = 2;
            acost = side[1];
        }
        if (!no_bi && side[0] != UINT32_MAX && side[1] != UINT32_MAX) {
            mot_t bi;
            bi.mv[0] = me[0];
            bi.mv[1] = me[1];
            bi.ref[0] = bref[0];
            bi.ref[1] = bref[1];
            bi.pred = 3;
            mv_t bp[2][2];
            memcpy(bp, mvp, sizeof(bp));
            uint32_t c;
            if (bi_refine_iters()) {
                c = bi_refine(e, g, &bi, bp, lam4, 6);
            } else {
                c = mot_satd_wh(e, g->x, g->y, w, h, bi) +
                    (uint32_t)(((uint64_t)mot_rate_q8(e, g, &bi, bp, 6) *
                                lam4) >> 12);
            }
            if (c < acost) {
                amot = bi;
                acost = c;
                memcpy(mvp, bp, sizeof(bp));
            }
        }
    }

    if (best_mc <= acost) {
        d->merge = 1;
        d->midx = best_m;
        d->mot = mlist[best_m];
        d->mvp_idx[0] = d->mvp_idx[1] = 0;
        return best_mc;
    }
    d->merge = 0;
    d->midx = 0;
    d->mot = amot;
    for (int l = 0; l < 2; l++) {
        d->mvp[l][0] = mvp[l][0];
        d->mvp[l][1] = mvp[l][1];
        int c0 = abs(amot.mv[l].x - mvp[l][0].x) +
                 abs(amot.mv[l].y - mvp[l][0].y);
        int c1 = abs(amot.mv[l].x - mvp[l][1].x) +
                 abs(amot.mv[l].y - mvp[l][1].y);
        d->mvp_idx[l] = c1 < c0;
    }
    return acost;
}

/* rank one candidate partition: search both PUs in decode order, then
 * restore the maps the search wrote */
static uint32_t try_part(struct n265_encoder *e, int x0, int y0, int n,
                         int part, const mv_t start[2], pu_dec d[2])
{
    mot_snap sn;
    mot_save(e, x0, y0, n, &sn);
    set_blk(e, e->blk_intra, x0, y0, n, 0);
    uint32_t tot = 0;
    memset(d, 0, 2 * sizeof(d[0]));
    for (int p = 0; p < 2; p++) {
        pu_geo g;
        g.cbx = x0; g.cby = y0; g.cbn = n;
        n265_pu_rect(part, p, x0, y0, n, &g.x, &g.y, &g.w, &g.h);
        g.excl_a1 = p == 1 && !PART_IS_HORIZ(part);
        g.excl_b1 = p == 1 && PART_IS_HORIZ(part);
        tot += search_pu(e, &g, start, &d[p]);
        set_mot_map_wh(e, g.x, g.y, g.w, g.h, d[p].mot);
    }
    mot_load(e, x0, y0, n, &sn);
    return tot;
}

/* a partition candidate carried from its RD trial to its commit */
typedef struct part_cand {
    int part;
    int any;                    /* rqt_root_cbf */
    pu_dec d[2];
    tb_ctx tl, tcb, tcr;
    uint8_t py[32 * 32];        /* the PU-by-PU prediction of the CU */
    uint8_t pc[2][16 * 16];
} part_cand;

static void set_cu_pred(struct n265_encoder *e, int x0, int y0, int n,
                        part_cand *c)
{
    e->cu_pred[0] = c->py;
    e->cu_pred[1] = c->pc[0];
    e->cu_pred[2] = c->pc[1];
    e->cu_pred_stride[0] = n;
    e->cu_pred_stride[1] = e->cu_pred_stride[2] = n >> 1;
    e->cu_pred_x0 = x0;
    e->cu_pred_y0 = y0;
}

static void clear_cu_pred(struct n265_encoder *e)
{
    e->cu_pred[0] = e->cu_pred[1] = e->cu_pred[2] = NULL;
}

/* CU-level inter syntax on the estimator: everything up to and including
 * rqt_root_cbf. Mirrors replay_cu's inter branch exactly - the 2Nx2N
 * candidates go through here too, so the two can't drift. */
static void est_inter_cu_bits(struct n265_encoder *e, est_state *st, int x0,
                              int y0, int log2n, int part,
                              const pu_dec d[2], int any, int inc)
{
    cabac_ctx_t *local = st->ctx;
    int n = 1 << log2n;
    res_ops ops;
    n265_est_ops(&ops, st);
    st->bits += n265_est_bin(&local[CTX_CU_SKIP + inc], 0);
    st->bits += n265_est_bin(&local[CTX_PRED_MODE], 0);
    n265_write_part_mode(e, &ops, log2n, part);
    for (int p = 0; p < N265_NUM_PU(part); p++) {
        int px, py, pw, ph;
        n265_pu_rect(part, p, x0, y0, n, &px, &py, &pw, &ph);
        st->bits += n265_est_bin(&local[CTX_MERGE_FLAG], d[p].merge);
        if (d[p].merge) {
            st->bits += n265_est_bin(&local[CTX_MERGE_IDX], d[p].midx > 0);
            if (d[p].midx > 0 && MAX_MERGE > 2)
                st->bits += 256u * (uint32_t)(d[p].midx < MAX_MERGE - 1
                                              ? d[p].midx : d[p].midx - 1);
            continue;
        }
        int pred = d[p].mot.pred;
        if (e->slice_type == 0) {
            if (pw + ph != 12) {
                st->bits += n265_est_bin(
                    &local[CTX_INTER_IDC + (e->ctb_log2 - log2n)], pred == 3);
                if (pred != 3)
                    st->bits += n265_est_bin(&local[CTX_INTER_IDC + 4],
                                             pred >> 1);
            } else {
                st->bits += n265_est_bin(&local[CTX_INTER_IDC + 4], pred >> 1);
            }
        }
        for (int l = 0; l < 2; l++) {
            if (!(pred & (1 << l)))
                continue;
            if (e->nref[l] > 1) {      /* ref_idx_lX, truncated unary */
                int r = d[p].mot.ref[l], cmax = e->nref[l] - 1;
                st->bits += n265_est_bin(&local[CTX_REF_IDX + 0], r > 0);
                if (r > 0 && cmax > 1)
                    st->bits += n265_est_bin(&local[CTX_REF_IDX + 1], r > 1);
                if (r > 1 && cmax > 2)
                    st->bits += 256u *
                        (uint32_t)((r < cmax ? r : cmax - 1) - 1);
            }
            int k = d[p].mvp_idx[l];
            est_mvd(st, d[p].mot.mv[l].x - d[p].mvp[l][k].x,
                    d[p].mot.mv[l].y - d[p].mvp[l][k].y);
            st->bits += n265_est_bin(&local[CTX_MVP_FLAG], k);
        }
    }
    /* rqt_root_cbf is absent, and inferred 1, only for a merged 2Nx2N CU */
    if (!(part == PART_2Nx2N && d[0].merge))
        st->bits += n265_est_bin(&local[CTX_RQT_ROOT_CBF], any);
}

/* full RD of one partition: predict PU by PU, then cost the CU with a
 * flat transform set (the commit re-decides flat vs split, exactly as
 * the 2Nx2N candidates do) */
static uint64_t rd_part(struct n265_encoder *e, int x0, int y0, int log2n,
                        int inc, part_cand *c)
{
    int n = 1 << log2n, hn = n >> 1;
    int qp_c = n265_chroma_qp(e->cur_ctu_qp);
    for (int p = 0; p < N265_NUM_PU(c->part); p++) {
        int px, py, pw, ph;
        n265_pu_rect(c->part, p, x0, y0, n, &px, &py, &pw, &ph);
        mc_pred_wh(e, 0, px, py, pw, ph, c->d[p].mot,
                   c->py + (py - y0) * n + (px - x0), n);
        for (int ci = 1; ci <= 2; ci++)
            mc_pred_wh(e, ci, px / 2, py / 2, pw / 2, ph / 2, c->d[p].mot,
                       c->pc[ci - 1] + ((py - y0) / 2) * hn + (px - x0) / 2,
                       hn);
    }
    set_cu_pred(e, x0, y0, n, c);
    tb_analyze(e, 0, x0, y0, log2n, e->cur_ctu_qp, MODE_INTER, mot_zero,
               &c->tl);
    tb_analyze(e, 1, x0 / 2, y0 / 2, log2n - 1, qp_c, MODE_INTER, mot_zero,
               &c->tcb);
    tb_analyze(e, 2, x0 / 2, y0 / 2, log2n - 1, qp_c, MODE_INTER, mot_zero,
               &c->tcr);
    tb_make_recon(0, log2n, e->cur_ctu_qp, MODE_INTER, &c->tl);
    tb_make_recon(1, log2n - 1, qp_c, MODE_INTER, &c->tcb);
    tb_make_recon(2, log2n - 1, qp_c, MODE_INTER, &c->tcr);
    clear_cu_pred(e);
    c->any = c->tl.nz || c->tcb.nz || c->tcr.nz;
    uint64_t d = tb_ssd(e, 0, x0, y0, log2n, &c->tl) +
        tb_ssd(e, 1, x0 / 2, y0 / 2, log2n - 1, &c->tcb) +
        tb_ssd(e, 2, x0 / 2, y0 / 2, log2n - 1, &c->tcr);

    cabac_ctx_t local[CTX_COUNT];
    memcpy(local, e->est_ctx, sizeof(local));
    est_state st = { local, 0 };
    est_inter_cu_bits(e, &st, x0, y0, log2n, c->part, c->d, c->any, inc);
    if (c->any)
        est_tt_bits(e, &st, log2n, MODE_INTER, &c->tl, &c->tcb, &c->tcr);
    return rd_j(d, st.bits, n265_lambda_mode_q8(e->cur_ctu_qp));
}

/* commit an inter CU (any partition): maps, records, recon */
static void commit_inter_cu(struct n265_encoder *e, int x0, int y0,
                            int log2n, int part, const pu_dec d[2], int any,
                            tb_ctx *tl, tb_ctx *tcb, tb_ctx *tcr,
                            part_cand *pred_src)
{
    int n = 1 << log2n;
    set_blk(e, e->blk_intra, x0, y0, n, 0);
    set_blk(e, e->blk_skip, x0, y0, n, 0);
    set_mode_map(e, x0, y0, n, INTRA_DC);
    mark_cu_edges(e, x0, y0, n);
    mark_pu_edge(e, part, x0, y0, n);
    rec_u8(e, (unsigned)part);
    for (int p = 0; p < N265_NUM_PU(part); p++) {
        int px, py, pw, ph;
        n265_pu_rect(part, p, x0, y0, n, &px, &py, &pw, &ph);
        /* merge records carry the index in bits 3-5; the mvp bits belong
         * to AMVP records only (a stale mvp_idx[1] on a merge record
         * reads as index + 4 once five candidates are allowed; found
         * 2026-09-06 while widening MAX_MERGE) */
        unsigned f = d[p].merge
            ? (1u | ((unsigned)d[p].midx << 3))
            : (((unsigned)d[p].mvp_idx[0] << 1) |
               ((unsigned)(d[p].mot.pred - 1) << 3) |
               ((unsigned)d[p].mvp_idx[1] << 5));
        rec_u8(e, f);
        if (!d[p].merge)
            for (int l = 0; l < 2; l++) {
                if (!(d[p].mot.pred & (1 << l)))
                    continue;
                rec_u8(e, (unsigned)d[p].mot.ref[l]);
                int k = d[p].mvp_idx[l];
                int16_t mvd[2] = {
                    (int16_t)(d[p].mot.mv[l].x - d[p].mvp[l][k].x),
                    (int16_t)(d[p].mot.mv[l].y - d[p].mvp[l][k].y)
                };
                rec_levels(e, mvd, 2);
            }
        set_mot_map_wh(e, px, py, pw, ph, d[p].mot);
    }
    rec_u8(e, (unsigned)(any != 0));
    if (pred_src)
        set_cu_pred(e, x0, y0, n, pred_src);
    if (any) {
        commit_inter_tt(e, d[0].mot, x0, y0, log2n, tl, tcb, tcr);
    } else {
        tb_commit(e, 0, x0, y0, log2n, tl);
        tb_commit(e, 1, x0 / 2, y0 / 2, log2n - 1, tcb);
        tb_commit(e, 2, x0 / 2, y0 / 2, log2n - 1, tcr);
    }
    clear_cu_pred(e);
}

static uint64_t analyze_cu(struct n265_encoder *e, int x0, int y0, int log2n)
{
    int n = 1 << log2n;
    int qp_c = n265_chroma_qp(e->cur_ctu_qp);
    e->last_skip_clean = 0;
    e->last_cu_nores = 0;
    mark_cu_edges(e, x0, y0, n);

    size_t pos0 = e->rec_len;

    if (e->slice_type != 2) {
        static tb_ctx m_tl, m_tcb, m_tcr;   /* merge candidate TBs */
        static tb_ctx a_tl, a_tcb, a_tcr;   /* amvp candidate TBs */
        static tb_ctx i_tl, i_tcb, i_tcr;   /* intra candidate TBs */
        mot_t mlist[MAX_MERGE];
        n265_merge_list(e, x0, y0, n, n, mlist, MAX_MERGE);
        int midx = 0;
        uint32_t cand_plain_sad = mot_sad(e, x0, y0, log2n, mlist[0]);
        for (int i = 1; i < MAX_MERGE; i++) {
            uint32_t sd = mot_sad(e, x0, y0, log2n, mlist[i]);
            if (sd < cand_plain_sad) {
                cand_plain_sad = sd;
                midx = i;
            }
        }
        mot_t cand = mlist[midx];

        /* strict static skip at the best merge candidate */
        if (cand_plain_sad * 8u <= (uint32_t)(n * n * skip_thr8())) {
            skip_cu(e, x0, y0, log2n, cand, midx);
            e->rec_pos = pos0;
            est_state adv = { e->est_ctx, 0 };
            n265_est_advance_cu(e, &adv, x0, y0, log2n);
            set_blk(e, e->blk_qp, x0, y0, n,
                    e->ent_qp_coded ? e->cur_ctu_qp : e->ent_qp_prev);
            uint64_t ssd = cu_area_ssd(e, x0, y0, n);
            e->last_skip_clean = ssd <= skip_clean_thr(e->cur_ctu_qp, n);
            e->last_cu_nores = 1;
            return rd_j(ssd, adv.bits, n265_lambda_mode_q8(e->cur_ctu_qp));
        }

        if (log2n <= 5) {
            uint32_t lam = n265_lambda_mode_q8(e->cur_ctu_qp);
            int inc = est_skip_inc(e, x0, y0);
            /* economy gates: a very good merge prediction prunes the
             * expensive candidates (classic early-skip shape) */
            static int no_gates = -1;
            if (no_gates < 0)
                no_gates = getenv("N265_NO_GATES") != NULL;
            int gate_amvp = !no_gates &&
                            cand_plain_sad < (uint32_t)(n * n) / 8;
            int gate_intra = !no_gates &&
                             cand_plain_sad < (uint32_t)(n * n) / 4;
            int gate_part = 0;

            /* merge candidate (and the forced-skip variant) */
            tb_analyze(e, 0, x0, y0, log2n, e->cur_ctu_qp, MODE_INTER, cand, &m_tl);
            tb_analyze(e, 1, x0 / 2, y0 / 2, log2n - 1, qp_c, MODE_INTER,
                       cand, &m_tcb);
            tb_analyze(e, 2, x0 / 2, y0 / 2, log2n - 1, qp_c, MODE_INTER,
                       cand, &m_tcr);
            tb_make_recon(0, log2n, e->cur_ctu_qp, MODE_INTER, &m_tl);
            tb_make_recon(1, log2n - 1, qp_c, MODE_INTER, &m_tcb);
            tb_make_recon(2, log2n - 1, qp_c, MODE_INTER, &m_tcr);
            int m_any = m_tl.nz || m_tcb.nz || m_tcr.nz;
            /* P1 CBF fast: the merge prediction already quantised to
             * nothing, so every remaining candidate can only re-buy the
             * same block with more syntax */
            if ((cbf_fast() == 1 || cbf_fast() == 2) && !m_any)
                gate_amvp = gate_intra = 1;
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
            st.bits += n265_est_bin(&local[CTX_MERGE_IDX], midx > 0);
            if (midx > 0 && MAX_MERGE > 2)
                st.bits += 256u * (uint32_t)(midx < MAX_MERGE - 1 ? midx
                                                                  : midx - 1);
            uint64_t j_skip = rd_j(d_skip, st.bits, lam);

            /* merge bits */
            pu_dec pd[2];
            memset(pd, 0, sizeof(pd));
            pd[0].mot = cand;
            pd[0].merge = 1;
            pd[0].midx = midx;
            uint64_t j_merge = UINT64_MAX;
            if (m_any) {
                memcpy(local, e->est_ctx, sizeof(local));
                st.ctx = local; st.bits = 0;
                est_inter_cu_bits(e, &st, x0, y0, log2n, PART_2Nx2N, pd,
                                  m_any, inc);
                est_tt_bits(e, &st, log2n, MODE_INTER, &m_tl, &m_tcb,
                            &m_tcr);
                j_merge = rd_j(d_merge, st.bits, lam);
            }

            /* amvp candidate: per-list ME, then the best of uni-L0,
             * uni-L1, bi by prediction SAD carries to RD */
            mv_t mvp[2][2] = { { { 0, 0 }, { 0, 0 } },
                               { { 0, 0 }, { 0, 0 } } };
            mv_t me_mv[2] = { { 0, 0 }, { 0, 0 } };
            uint64_t j_amvp = UINT64_MAX;
            int a_any = 0;
            int mvp_idx[2] = { 0, 0 };
            mot_t amot = { { { 0, 0 }, { 0, 0 } }, 1 };
            int same_as_cand = 1;
            pu_geo pg = { x0, y0, n, x0, y0, n, n, 0, 0 };
            int lam4 = lambda_sad_q4(e->cur_ctu_qp);
            if (!gate_amvp) {
                /* per list: search every reference, keep the one whose
                 * SAD + lambda x (mvd + ref_idx) bits is lowest, and the
                 * predictors of that reference */
                for (int l = 0; l < (e->slice_type == 0 ? 2 : 1); l++) {
                    uint32_t best_c = UINT32_MAX;
                    for (int r = 0; r < e->nref[l]; r++) {
                        mv_t p[2], mv;
                        n265_amvp(e, l, r, x0, y0, n, n, p);
                        uint32_t sad = n265_me_search(e, l, r, x0, y0, n, n, p[0], &mv);
                        mot_t u;
                        u.mv[0].x = u.mv[0].y = u.mv[1].x = u.mv[1].y = 0;
                        u.ref[0] = u.ref[1] = 0;
                        u.pred = (uint8_t)(1 << l);
                        u.mv[l] = mv;
                        u.ref[l] = (int8_t)r;
                        mv_t up[2][2] = { { { 0, 0 }, { 0, 0 } },
                                          { { 0, 0 }, { 0, 0 } } };
                        up[l][0] = p[0];
                        up[l][1] = p[1];
                        /* rate proxy (or the real syntax under
                         * N265_MVD_BITS), at lambda_sad. n265_me_search
                         * returns the FULL-PEL SAD, so the reference
                         * ranking never sees what the subpel round did
                         * to the winner; N265_ME_SATD re-costs it. */
                        uint32_t d = me_satd_on()
                            ? mot_satd_wh(e, x0, y0, n, n, u) : sad;
                        uint32_t c = d +
                            (uint32_t)(((uint64_t)mot_rate_q8(e, &pg, &u, up, 4)
                                        * lam4) >> 12);
                        if (c < best_c) { best_c = c; me_mv[l] = mv; mvp[l][0] = p[0]; mvp[l][1] = p[1]; amot.ref[l] = (int8_t)r; }
                    }
                }
                amot.mv[0] = me_mv[0];
                if (e->slice_type == 0) {
                    mot_t u1 = { { { 0, 0 }, me_mv[1] }, 2, { 0, amot.ref[1] } };
                    mot_t bi = { { me_mv[0], me_mv[1] }, 3, { amot.ref[0], amot.ref[1] } };
                    mv_t bp[2][2];
                    memcpy(bp, mvp, sizeof(bp));
                    if (bi_refine_iters())
                        (void)bi_refine(e, &pg, &bi, bp, lam4, 6);
                    /* SAD cannot tell the blur of a bi average from a
                     * misalignment, and this is the comparison that
                     * decides bi against uni; N265_ME_SATD makes it the
                     * Hadamard cost the rect/AMP PU search already uses */
                    int ms = me_satd_on();
                    uint32_t s0 = ms ? mot_satd_wh(e, x0, y0, n, n, amot)
                                     : mot_sad(e, x0, y0, log2n, amot);
                    uint32_t s1 = ms ? mot_satd_wh(e, x0, y0, n, n, u1)
                                     : mot_sad(e, x0, y0, log2n, u1);
                    uint32_t sb = ms ? mot_satd_wh(e, x0, y0, n, n, bi)
                                     : mot_sad(e, x0, y0, log2n, bi);
                    if (mvd_bits_on()) {
                        s0 += (uint32_t)(((uint64_t)mot_rate_q8(e, &pg, &amot, mvp, 6) * lam4) >> 12);
                        s1 += (uint32_t)(((uint64_t)mot_rate_q8(e, &pg, &u1, mvp, 6) * lam4) >> 12);
                        sb += (uint32_t)(((uint64_t)mot_rate_q8(e, &pg, &bi, bp, 6) * lam4) >> 12);
                    }
                    if (s1 < s0 && s1 <= sb)
                        amot = u1;
                    else if (sb < s0) {
                        amot = bi;
                        memcpy(mvp, bp, sizeof(bp));
                    }
                }
                same_as_cand = amot.pred == cand.pred &&
                    (!(amot.pred & 1) ||
                     (amot.mv[0].x == cand.mv[0].x &&
                      amot.mv[0].y == cand.mv[0].y && amot.ref[0] == cand.ref[0])) &&
                    (!(amot.pred & 2) ||
                     (amot.mv[1].x == cand.mv[1].x &&
                      amot.mv[1].y == cand.mv[1].y && amot.ref[1] == cand.ref[1]));
            }
            if (!gate_amvp && !same_as_cand && !getenv("N265_NO_AMVP")) {
                tb_analyze(e, 0, x0, y0, log2n, e->cur_ctu_qp, MODE_INTER, amot,
                           &a_tl);
                tb_analyze(e, 1, x0 / 2, y0 / 2, log2n - 1, qp_c,
                           MODE_INTER, amot, &a_tcb);
                tb_analyze(e, 2, x0 / 2, y0 / 2, log2n - 1, qp_c,
                           MODE_INTER, amot, &a_tcr);
                tb_make_recon(0, log2n, e->cur_ctu_qp, MODE_INTER, &a_tl);
                tb_make_recon(1, log2n - 1, qp_c, MODE_INTER, &a_tcb);
                tb_make_recon(2, log2n - 1, qp_c, MODE_INTER, &a_tcr);
                a_any = a_tl.nz || a_tcb.nz || a_tcr.nz;
                uint64_t d = tb_ssd(e, 0, x0, y0, log2n, &a_tl) +
                    tb_ssd(e, 1, x0 / 2, y0 / 2, log2n - 1, &a_tcb) +
                    tb_ssd(e, 2, x0 / 2, y0 / 2, log2n - 1, &a_tcr);
                /* the vector actually coded, not the per-list ME winner:
                 * the two are the same only until a bi refinement moves
                 * one of them, and a predictor picked for the other
                 * vector would make the decoder rebuild a different MV */
                for (int l = 0; l < 2; l++) {
                    if (!(amot.pred & (1 << l)))
                        continue;
                    mvp_idx[l] = mvp_pick(amot.mv[l], mvp[l]);
                }
                pd[1].mot = amot;
                pd[1].merge = 0;
                pd[1].midx = 0;
                for (int l = 0; l < 2; l++) {
                    pd[1].mvp_idx[l] = mvp_idx[l];
                    pd[1].mvp[l][0] = mvp[l][0];
                    pd[1].mvp[l][1] = mvp[l][1];
                }
                memcpy(local, e->est_ctx, sizeof(local));
                st.ctx = local; st.bits = 0;
                est_inter_cu_bits(e, &st, x0, y0, log2n, PART_2Nx2N, &pd[1],
                                  a_any, inc);
                if (a_any)
                    est_tt_bits(e, &st, log2n, MODE_INTER, &a_tl, &a_tcb,
                                &a_tcr);
                j_amvp = rd_j(d, st.bits, lam);
                /* P1 CBF fast, second stage: the searched vector's
                 * residual quantised away too */
                if (cbf_fast() >= 2 && !a_any)
                    gate_intra = gate_part = 1;
            }

            /* intra candidate (2Nx2N, SAD-ranked best mode) */
            uint64_t j_intra = UINT64_MAX;
            int imode = 1;
            if (!gate_intra) {
            imode = search_luma_mode(e, x0, y0, log2n, e->recon[0], NULL);
            tb_analyze(e, 1, x0 / 2, y0 / 2, log2n - 1, qp_c, imode,
                       mot_zero, &i_tcb);
            tb_analyze(e, 2, x0 / 2, y0 / 2, log2n - 1, qp_c, imode,
                       mot_zero, &i_tcr);
            tb_analyze(e, 0, x0, y0, log2n, e->cur_ctu_qp, imode, mot_zero, &i_tl);
            tb_make_recon(0, log2n, e->cur_ctu_qp, imode, &i_tl);
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

            /* pick the winner among the 2Nx2N candidates */
            uint64_t best = j_skip;
            int kind = 0;   /* 0 skip, 1 merge, 2 amvp, 3 intra, 4 split PU */
            if (j_merge < best) { best = j_merge; kind = 1; }
            if (j_amvp < best) { best = j_amvp; kind = 2; }
            /* Intra admission margin in B (N265_B_INTRA_MARGIN, percent).
             * An intra CU in a referenced B costs more than its own RD
             * says: it cuts the prediction chain the frames above it in
             * the pyramid ride on, and the local J cannot see that. Make
             * it win by a margin instead of by a hair. Zero = the plain
             * comparison. */
            uint64_t j_intra_adm = j_intra;
            {
                int mg = b_intra_margin();
                if (mg && e->slice_type == 0 && j_intra != UINT64_MAX)
                    j_intra_adm = j_intra * (uint64_t)(100 + mg) / 100;
            }
            if (j_intra_adm < best) { best = j_intra; kind = 3; }

            /* rectangular and asymmetric partitions. Gated the way the
             * expensive candidates are: a static region (skip won, or the
             * merge prediction was already good enough to prune AMVP)
             * cannot be paying for two motion fields. Rank the two
             * rectangular splits by SATD, RD only the better; try AMP
             * only in the orientation a rectangular split already won,
             * and only above the min CB where the spec allows it. */
            static part_cand pca, pcb;
            part_cand *cur = &pca, *pwin = NULL;
            if (e->rect && kind != 0 && !gate_amvp && !gate_part) {
                mv_t start[2] = { me_mv[0], me_mv[1] };
                pu_dec dh[2], dv[2];
                uint32_t ch = try_part(e, x0, y0, n, PART_2NxN, start, dh);
                uint32_t cv = try_part(e, x0, y0, n, PART_Nx2N, start, dv);
                int horiz = ch <= cv;
                cur->part = horiz ? PART_2NxN : PART_Nx2N;
                memcpy(cur->d, horiz ? dh : dv, sizeof(cur->d));
                uint64_t jr = rd_part(e, x0, y0, log2n, inc, cur);
                if (jr < best) {
                    best = jr;
                    kind = 4;
                    pwin = cur;
                    cur = cur == &pca ? &pcb : &pca;
                    if (e->amp && log2n > MIN_CB_LOG2) {
                        int a0 = horiz ? PART_2NxnU : PART_nLx2N;
                        int a1 = horiz ? PART_2NxnD : PART_nRx2N;
                        pu_dec da[2], db[2];
                        uint32_t ca = try_part(e, x0, y0, n, a0, start, da);
                        uint32_t cb = try_part(e, x0, y0, n, a1, start, db);
                        cur->part = ca <= cb ? a0 : a1;
                        memcpy(cur->d, ca <= cb ? da : db, sizeof(cur->d));
                        uint64_t ja = rd_part(e, x0, y0, log2n, inc, cur);
                        if (ja < best) {
                            best = ja;
                            pwin = cur;
                        }
                    }
                }
            }

            if (kind == 0) {
                skip_cu(e, x0, y0, log2n, cand, midx);
                e->last_skip_clean = d_skip <= skip_clean_thr(e->cur_ctu_qp, n);
                e->last_cu_nores = 1;
            } else if (kind == 4) {
                commit_inter_cu(e, x0, y0, log2n, pwin->part, pwin->d,
                                pwin->any, &pwin->tl, &pwin->tcb,
                                &pwin->tcr, pwin);
                e->last_cu_nores = !pwin->any;
            } else if (kind == 1 || kind == 2) {
                tb_ctx *tl = kind == 1 ? &m_tl : &a_tl;
                tb_ctx *tcb = kind == 1 ? &m_tcb : &a_tcb;
                tb_ctx *tcr = kind == 1 ? &m_tcr : &a_tcr;
                int any = kind == 1 ? m_any : a_any;
                commit_inter_cu(e, x0, y0, log2n, PART_2Nx2N,
                                kind == 1 ? &pd[0] : &pd[1], any, tl, tcb,
                                tcr, NULL);
                e->last_cu_nores = !any;
            }
            if (kind != 3) {
                e->rec_pos = pos0;
                est_state adv = { e->est_ctx, 0 };
                n265_est_advance_cu(e, &adv, x0, y0, log2n);
                set_blk(e, e->blk_qp, x0, y0, n,
                        e->ent_qp_coded ? e->cur_ctu_qp : e->ent_qp_prev);
                return rd_j(cu_area_ssd(e, x0, y0, n), adv.bits,
                            n265_lambda_mode_q8(e->cur_ctu_qp));
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
    } else if (log2n <= 5) {
        modes[0] = intra_rd_pick(e, x0, y0, log2n);
    } else {
        modes[0] = search_luma_mode(e, x0, y0, 5, e->recon[0], NULL);
    }

    if (!nxn) {
        set_mode_map(e, x0, y0, n, modes[0]);
        tt_analyze(e, modes[0], x0, y0, log2n);
        e->rec_pos = pos0;
        est_state adv = { e->est_ctx, 0 };
        n265_est_advance_cu(e, &adv, x0, y0, log2n);
        set_blk(e, e->blk_qp, x0, y0, n,
                e->ent_qp_coded ? e->cur_ctu_qp : e->ent_qp_prev);
        return rd_j(cu_area_ssd(e, x0, y0, n), adv.bits,
                    n265_lambda_mode_q8(e->cur_ctu_qp));
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
        int cbf_l = tb_full(e, 0, px, py, 2, e->cur_ctu_qp, modes[i], &tl);
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
    set_blk(e, e->blk_qp, x0, y0, n,
            e->ent_qp_coded ? e->cur_ctu_qp : e->ent_qp_prev);
    return rd_j(cu_area_ssd(e, x0, y0, n), adv.bits,
                n265_lambda_mode_q8(e->cur_ctu_qp));
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
    uint8_t maps[8][16 * 16];
    int16_t mv[16 * 16 * 4];
    int8_t ref[16 * 16 * 2];    /* the reference-index map (multi-ref, 2026-09-06) */
    cabac_ctx_t est[CTX_COUNT];
    int ent_qp_coded;
    size_t rec_len;
    /* the records this arm appended beyond rec_base (the other arm
     * overwrites the shared tail, so content must be kept too) */
    size_t rec_base;
    size_t rec_copy_len;
    uint8_t rec_copy[16384];
} qt_snap;

static uint8_t *snap_maps(struct n265_encoder *e, int i)
{
    uint8_t *m[8];
    m[0] = e->ct_depth; m[1] = e->intra_mode; m[2] = e->edge_map;
    m[3] = e->blk_intra; m[4] = e->blk_skip; m[5] = e->blk_cbf_y;
    m[6] = e->blk_pred; m[7] = e->blk_qp;
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
    for (int i = 0; i < 8; i++)
        for (int y = 0; y < cells; y++)
            memcpy(sn->maps[i] + y * cells,
                   snap_maps(e, i) + ((y0 >> 2) + y) * e->map_stride +
                       (x0 >> 2),
                   (size_t)cells);
    for (int y = 0; y < cells; y++)
        memcpy(sn->mv + y * cells * 2 * 2,
               e->blk_mv + (((y0 >> 2) + y) * e->map_stride + (x0 >> 2)) * 4,
               (size_t)cells * 4 * sizeof(int16_t));
    for (int y = 0; y < cells; y++)
        memcpy(sn->ref + y * cells * 2,
               e->blk_ref + (((y0 >> 2) + y) * e->map_stride + (x0 >> 2)) * 2,
               (size_t)cells * 2);
    memcpy(sn->est, e->est_ctx, sizeof(sn->est));
    sn->ent_qp_coded = e->ent_qp_coded;
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
    for (int i = 0; i < 8; i++)
        for (int y = 0; y < cells; y++)
            memcpy(snap_maps(e, i) + ((y0 >> 2) + y) * e->map_stride +
                       (x0 >> 2),
                   sn->maps[i] + y * cells, (size_t)cells);
    for (int y = 0; y < cells; y++)
        memcpy(e->blk_mv + (((y0 >> 2) + y) * e->map_stride + (x0 >> 2)) * 4,
               sn->mv + y * cells * 2 * 2,
               (size_t)cells * 4 * sizeof(int16_t));
    for (int y = 0; y < cells; y++)
        memcpy(e->blk_ref + (((y0 >> 2) + y) * e->map_stride + (x0 >> 2)) * 2,
               sn->ref + y * cells * 2, (size_t)cells * 2);
    memcpy(e->est_ctx, sn->est, sizeof(sn->est));
    e->ent_qp_coded = sn->ent_qp_coded;
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
    uint32_t lam = n265_lambda_mode_q8(e->cur_ctu_qp);

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

    /* P2: this node is above the depth range the neighbourhood used, so
     * the no-split arm (and, at the CTB, its CB64 arms) never wins.
     * Recurse straight away. Modes 2 and 4 keep one arm here: the
     * whole-CTU prediction-only skip, which is cheap and is the one
     * thing a neighbourhood bound gets wrong when a moving picture
     * goes still. */
    if (depth_bound() && depth < e->db_lo) {
        static qt_snap fpre, fwon;      /* only depth 0 uses these */
        uint64_t j_cu2 = UINT64_MAX;
        size_t rec_base2 = e->rec_len;
        if (depth_bound_ctb_skip() && log2n == e->ctb_log2 &&
            e->slice_type != 2 && e->ref_valid) {
            qt_save(e, &fpre, x0, y0, n);
            uint32_t b0 = n265_est_bin(&e->est_ctx[CTX_SPLIT_CU + split_inc],
                                       0);
            mot_t mlist[MAX_MERGE];
            n265_merge_list(e, x0, y0, n, n, mlist, MAX_MERGE);
            int midx = 0;
            uint32_t sd0 = mot_sad(e, x0, y0, log2n, mlist[0]);
            for (int i = 1; i < MAX_MERGE; i++) {
                uint32_t sd = mot_sad(e, x0, y0, log2n, mlist[i]);
                if (sd < sd0) { sd0 = sd; midx = i; }
            }
            size_t pos0 = e->rec_len;
            mark_cu_edges(e, x0, y0, n);
            skip_cu(e, x0, y0, log2n, mlist[midx], midx);
            e->rec_pos = pos0;
            est_state adv = { e->est_ctx, 0 };
            n265_est_advance_cu(e, &adv, x0, y0, log2n);
            set_blk(e, e->blk_qp, x0, y0, n,
                    e->ent_qp_coded ? e->cur_ctu_qp : e->ent_qp_prev);
            set_blk(e, e->ct_depth, x0, y0, n, depth);
            j_cu2 = rd_j(cu_area_ssd(e, x0, y0, n), adv.bits + b0, lam);
            qt_save(e, &fwon, x0, y0, n);
            qt_save_records(e, &fwon, rec_base2);
            qt_restore(e, &fpre, x0, y0, n);
        }
        uint32_t b1 = n265_est_bin(&e->est_ctx[CTX_SPLIT_CU + split_inc], 1);
        uint64_t js = (uint64_t)b1 * lam >> 16;
        int hh = n >> 1;
        for (int i = 0; i < 4; i++)
            js += quadtree(e, x0 + (i & 1) * hh, y0 + (i >> 1) * hh,
                           log2n - 1, depth + 1);
        if (j_cu2 <= js) {
            qt_restore(e, &fwon, x0, y0, n);
            return j_cu2;
        }
        return js;
    }

    static int no_split = -1;
    if (no_split < 0)
        no_split = getenv("N265_NO_SPLIT") != NULL;
    if (no_split) {
        uint32_t b0s = n265_est_bin(&e->est_ctx[CTX_SPLIT_CU + split_inc], 0);
        uint64_t j = analyze_cu(e, x0, y0, log2n) +
                     ((uint64_t)b0s * lam >> 16);
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
    static int no_es = -1;
    if (no_es < 0)
        no_es = getenv("N265_NO_EARLYSKIP") != NULL;
    if (e->last_skip_clean && !no_es)
        return j_cu;
    /* P1 rskip: the no-split arm left no residual at all, so the four
     * children can only re-buy the same prediction with four sets of
     * syntax. Mode 1 also asks that neither decoded neighbour went
     * deeper than this node - where they did, this block is on a real
     * boundary and the split is worth trying anyway. Above 32 the CU
     * arm is intra-or-strict-skip and the CB64 arms below have not run,
     * so the rule stays where the full candidate set decided. */
    if (rskip_mode() && log2n <= 5 && e->last_cu_nores &&
        (rskip_mode() >= 2 || !split_inc))
        return j_cu;

    /* 64x64 whole-CU skip: prediction-only, so it is legal where the
     * flat-TB inter candidates are not (max TB 32). RD it against the
     * intra/static outcome of arm 1 before trying the split. */
    if (log2n == 6 && e->slice_type != 2) {
        qt_snap *ss = &won[depth];      /* reuse after the compare */
        qt_save(e, ss, x0, y0, n);
        qt_save_records(e, ss, rec_base);
        qt_restore(e, sp, x0, y0, n);
        e->rec_len = rec_base;
        /* the restore rewound the split-flag context mutation arm 1
         * made; re-apply it so analysis tracks the entropy pass */
        (void)n265_est_bin(&e->est_ctx[CTX_SPLIT_CU + split_inc], 0);

        mot_t mlist[MAX_MERGE];
        n265_merge_list(e, x0, y0, n, n, mlist, MAX_MERGE);
        int midx = 0;
        uint32_t sd0 = mot_sad(e, x0, y0, log2n, mlist[0]);
        for (int i = 1; i < MAX_MERGE; i++) {
            uint32_t sd = mot_sad(e, x0, y0, log2n, mlist[i]);
            if (sd < sd0) {
                sd0 = sd;
                midx = i;
            }
        }
        size_t pos0 = e->rec_len;
        mark_cu_edges(e, x0, y0, n);
        skip_cu(e, x0, y0, log2n, mlist[midx], midx);
        e->rec_pos = pos0;
        est_state adv = { e->est_ctx, 0 };
        n265_est_advance_cu(e, &adv, x0, y0, log2n);
        set_blk(e, e->blk_qp, x0, y0, n,
                e->ent_qp_coded ? e->cur_ctu_qp : e->ent_qp_prev);
        set_blk(e, e->ct_depth, x0, y0, n, depth);
        uint64_t j_skip = rd_j(cu_area_ssd(e, x0, y0, n),
                               adv.bits + b0, lam);
        if (j_skip < j_cu) {
            if (getenv("N265_CB64_TRACE"))
                fprintf(stderr, "cb64skip poc=%d ctu=(%d,%d) midx=%d pred=%d mv0=(%d,%d) mv1=(%d,%d)\n",
                        e->poc, x0 >> 6, y0 >> 6, midx, mlist[midx].pred,
                        mlist[midx].mv[0].x, mlist[midx].mv[0].y,
                        mlist[midx].mv[1].x, mlist[midx].mv[1].y);
            j_cu = j_skip;      /* current state is the skip */
        } else {
            qt_restore(e, ss, x0, y0, n);   /* back to arm-1 outcome */
        }

        /* merge with residual (forced 4x TU32 tree) */
        qt_save(e, ss, x0, y0, n);
        qt_save_records(e, ss, rec_base);
        qt_restore(e, sp, x0, y0, n);
        (void)n265_est_bin(&e->est_ctx[CTX_SPLIT_CU + split_inc], 0);
        pu_dec pdm;
        memset(&pdm, 0, sizeof(pdm));
        pdm.mot = mlist[midx];
        pdm.merge = 1;
        pdm.midx = midx;
        commit_inter_cu(e, x0, y0, log2n, PART_2Nx2N, &pdm, 1, NULL, NULL,
                        NULL, NULL);
        e->rec_pos = pos0;
        est_state madv = { e->est_ctx, 0 };
        n265_est_advance_cu(e, &madv, x0, y0, log2n);
        set_blk(e, e->blk_qp, x0, y0, n,
                e->ent_qp_coded ? e->cur_ctu_qp : e->ent_qp_prev);
        set_blk(e, e->ct_depth, x0, y0, n, depth);
        uint64_t j_m = rd_j(cu_area_ssd(e, x0, y0, n), madv.bits + b0,
                            lam);
        if (j_m < j_cu)
            j_cu = j_m;
        else
            qt_restore(e, ss, x0, y0, n);

        /* AMVP 2Nx2N at 64x64 (N265_CB64_AMVP). The CB64 node had merge
         * and skip only, so a 64x64 region whose motion is not in the
         * merge list had to be split into four 32x32 CUs and pay four
         * motion fields plus the split flags for one. The transform is
         * the same forced TU32 quad the merge arm uses. Entered only
         * where the PU search prefers AMVP to merge - where merge wins
         * the arm above already has it. */
        if (cb64_amvp()) {
            qt_save(e, ss, x0, y0, n);
            qt_save_records(e, ss, rec_base);
            qt_restore(e, sp, x0, y0, n);
            (void)n265_est_bin(&e->est_ctx[CTX_SPLIT_CU + split_inc], 0);
            pu_geo g = { x0, y0, n, x0, y0, n, n, 0, 0 };
            pu_dec pda;
            memset(&pda, 0, sizeof(pda));
            (void)search_pu(e, &g, NULL, &pda);
            uint64_t j_a = UINT64_MAX;
            if (!pda.merge) {
                commit_inter_cu(e, x0, y0, log2n, PART_2Nx2N, &pda, 1, NULL,
                                NULL, NULL, NULL);
                e->rec_pos = pos0;
                est_state aadv = { e->est_ctx, 0 };
                n265_est_advance_cu(e, &aadv, x0, y0, log2n);
                set_blk(e, e->blk_qp, x0, y0, n,
                        e->ent_qp_coded ? e->cur_ctu_qp : e->ent_qp_prev);
                set_blk(e, e->ct_depth, x0, y0, n, depth);
                j_a = rd_j(cu_area_ssd(e, x0, y0, n), aadv.bits + b0, lam);
            }
            if (j_a < j_cu)
                j_cu = j_a;
            else
                qt_restore(e, ss, x0, y0, n);
        }
    }

    /* P2: the neighbourhood never went below this depth, so the split
     * arm is not worth its four candidate sets. The no-split arm has
     * run and its state stands; its J already carries the split flag. */
    if (depth_bound() && !depth_bound_local() && depth >= e->db_hi)
        return j_cu;
    if (depth_bound_local()) {
        int hi = nb_depth_max(e, x0, y0, n);
        if (hi >= 0 && depth >= hi + (depth_bound() == 5))
            return j_cu;
    }

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

/* P2: the depth range this CTU's quadtree may search, from the decoded
 * spatial neighbours (left, above, above-left, above-right) and, in an
 * inter slice, the collocated CTU of the nearest reference, widened by
 * one each way. With nothing decided yet the range is the whole tree. */
static void depth_bounds(struct n265_encoder *e, int ctb_x, int ctb_y)
{
    int maxd = e->ctb_log2 - MIN_CB_LOG2;
    e->db_lo = 0;
    e->db_hi = maxd;
    if (!depth_bound() || depth_bound_local())
        return;
    static const int dx[4] = { -1, 0, -1, 1 };
    static const int dy[4] = { 0, -1, -1, -1 };
    int lo = maxd, hi = 0, have = 0;
    for (int i = 0; i < 4; i++) {
        int nx = ctb_x + dx[i], ny = ctb_y + dy[i];
        if (nx < 0 || ny < 0 || nx >= e->ctb_cols || ny >= e->ctb_rows)
            continue;
        int k = ny * e->ctb_cols + nx;
        if (e->ctu_dmin[k] == 0xff)
            continue;
        if (e->ctu_dmin[k] < lo) lo = e->ctu_dmin[k];
        if (e->ctu_dmax[k] > hi) hi = e->ctu_dmax[k];
        have = 1;
    }
    if (e->slice_type != 2 && e->ref_valid) {
        const struct n265_refpic *near = NULL;
        for (int l = 0; l < 2; l++)
            for (int r = 0; r < e->nref[l]; r++) {
                const struct n265_refpic *p = e->refl[l][r];
                if (!p || !p->ctu_dmin)
                    continue;
                if (!near || abs(p->poc - e->poc) < abs(near->poc - e->poc))
                    near = p;
            }
        int k = ctb_y * e->ctb_cols + ctb_x;
        if (near && near->ctu_dmin[k] != 0xff) {
            if (near->ctu_dmin[k] < lo) lo = near->ctu_dmin[k];
            if (near->ctu_dmax[k] > hi) hi = near->ctu_dmax[k];
            have = 1;
        }
    }
    if (!have)
        return;
    int w = depth_bound_widen();
    e->db_lo = lo > w ? lo - w : 0;
    e->db_hi = hi + w < maxd ? hi + w : maxd;
}

void n265_analyze_ctu(struct n265_encoder *e, int ctb_x, int ctb_y)
{
    int q = e->qp + e->ctu_qp_off[ctb_y * e->ctb_cols + ctb_x];
    e->cur_ctu_qp = q < 0 ? 0 : q > 51 ? 51 : q;
    e->ent_ctu_qp = e->cur_ctu_qp;
    e->ent_qp_coded = 0;
    clear_cu_pred(e);
    depth_bounds(e, ctb_x, ctb_y);
    (void)quadtree(e, ctb_x << e->ctb_log2, ctb_y << e->ctb_log2,
                   e->ctb_log2, 0);
    if (depth_bound()) {    /* record what this CTU actually used */
        int x0 = ctb_x << e->ctb_log2, y0 = ctb_y << e->ctb_log2;
        int n = 1 << e->ctb_log2;
        int xe = (x0 + n) >> 2, ye = (y0 + n) >> 2;
        if (xe > (e->pad_w >> 2)) xe = e->pad_w >> 2;
        if (ye > (e->pad_h >> 2)) ye = e->pad_h >> 2;
        int lo = 0xff, hi = 0;
        for (int y = y0 >> 2; y < ye; y++)
            for (int x = x0 >> 2; x < xe; x++) {
                int d = e->ct_depth[y * e->map_stride + x];
                if (d < lo) lo = d;
                if (d > hi) hi = d;
            }
        int k = ctb_y * e->ctb_cols + ctb_x;
        e->ctu_dmin[k] = (uint8_t)(lo == 0xff ? 0 : lo);
        e->ctu_dmax[k] = (uint8_t)hi;
    }
    if (e->ent_qp_coded)
        e->ent_qp_prev = e->cur_ctu_qp;
}
