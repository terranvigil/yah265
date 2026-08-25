/*
 * Coding-tree walk and intra CU encoding (DC mode, 2Nx2N, TU == CU).
 * Copyright (c) 2026, the next265 authors. GPL-2.0-or-later; see LICENSE.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "encoder/encoder.h"
#include "encoder/intra.h"
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

/* MPM candidate list (8.4.2). All-intra: neighbors are intra when inside
 * the picture; above outside the current CTB row degrades to DC. */
static void mpm_list(struct n265_encoder *e, int x0, int y0, int cand[3])
{
    int a = INTRA_DC, b = INTRA_DC;
    if (x0 > 0)
        a = e->intra_mode[(y0 >> 2) * e->map_stride + ((x0 - 1) >> 2)];
    if (y0 > 0 && (y0 & ((1 << e->ctb_log2) - 1)) != 0)
        b = e->intra_mode[((y0 - 1) >> 2) * e->map_stride + (x0 >> 2)];

    if (a == b) {
        if (a < 2) {
            cand[0] = INTRA_PLANAR;
            cand[1] = INTRA_DC;
            cand[2] = 26;
        } else {
            cand[0] = a;
            cand[1] = 2 + ((a + 29) % 32);
            cand[2] = 2 + ((a - 2 + 1) % 32);
        }
    } else {
        cand[0] = a;
        cand[1] = b;
        if (a != INTRA_PLANAR && b != INTRA_PLANAR)
            cand[2] = INTRA_PLANAR;
        else if (a != INTRA_DC && b != INTRA_DC)
            cand[2] = INTRA_DC;
        else
            cand[2] = 26;
    }
}

static void write_luma_mode(struct n265_encoder *e, cabac_t *cb, int x0,
                            int y0, int mode)
{
    int cand[3];
    mpm_list(e, x0, y0, cand);
    int idx = -1;
    for (int i = 0; i < 3; i++)
        if (cand[i] == mode)
            idx = i;
    n265_cabac_bin(cb, &e->ctx[CTX_PREV_INTRA], idx >= 0);
    if (idx >= 0) {
        n265_cabac_bypass(cb, idx > 0);
        if (idx > 0)
            n265_cabac_bypass(cb, idx > 1);
    } else {
        /* rem_intra_luma_pred_mode: mode minus the smaller candidates,
         * candidates sorted descending first (spec swap steps) */
        int c[3] = { cand[0], cand[1], cand[2] }, t;
        if (c[0] > c[1]) { t = c[0]; c[0] = c[1]; c[1] = t; }
        if (c[0] > c[2]) { t = c[0]; c[0] = c[2]; c[2] = t; }
        if (c[1] > c[2]) { t = c[1]; c[1] = c[2]; c[2] = t; }
        int rem = mode;
        for (int i = 2; i >= 0; i--)
            if (mode > c[i])
                rem--;
        n265_cabac_bypass_bits(cb, (uint32_t)rem, 5);
    }
}

/* one color component of the TU: predict, transform, quant, code, recon.
 * Returns cbf. */
static int code_component(struct n265_encoder *e, cabac_t *cb, int c_idx,
                          int x0, int y0, int log2n, int qp, int mode,
                          int coded)
{
    int use_dst = c_idx == 0 && log2n == 2;     /* intra luma 4x4: DST-VII */
    int n = 1 << log2n;
    uint8_t *recon = e->recon[c_idx];
    int stride = e->recon_stride[c_idx];
    const uint8_t *src = e->pad_plane[c_idx] + y0 * e->pad_stride[c_idx] + x0;
    int pic_w = c_idx ? e->pad_w / 2 : e->pad_w;
    int pic_h = c_idx ? e->pad_h / 2 : e->pad_h;
    int ctb_log2 = c_idx ? e->ctb_log2 - 1 : e->ctb_log2;

    uint8_t refs[4 * 32 + 1], refs_f[4 * 32 + 1];
    uint8_t pred[32 * 32];
    n265_intra_refs(refs, recon, stride, x0, y0, log2n, pic_w, pic_h,
                    ctb_log2);
    if (n265_use_filtered_refs(mode, log2n, c_idx)) {
        n265_filter_refs(refs_f, refs, log2n);
        n265_intra_pred(pred, n, refs_f, log2n, mode, c_idx);
    } else {
        n265_intra_pred(pred, n, refs, log2n, mode, c_idx);
    }

    int16_t resid[32 * 32], coef[32 * 32], level[32 * 32], dq[32 * 32];
    for (int y = 0; y < n; y++)
        for (int x = 0; x < n; x++)
            resid[y * n + x] =
                (int16_t)(src[y * e->pad_stride[c_idx] + x] - pred[y * n + x]);
    n265_fdct(coef, resid, log2n, use_dst);
    int nz = n265_quant(level, coef, log2n, qp);

    if (!coded)
        return nz != 0;     /* first pass: just report cbf */

    if (nz) {
        n265_residual_coding(cb, e->ctx, level, log2n, c_idx,
                             n265_scan_idx(mode, log2n, c_idx));
        n265_dequant(dq, level, log2n, qp);
        n265_idct_add(recon + y0 * stride + x0, stride, dq, log2n, pred, n,
                      use_dst);
    } else {
        for (int y = 0; y < n; y++)
            memcpy(recon + (y0 + y) * stride + x0, pred + y * n, (size_t)n);
    }
    return nz != 0;
}

/* pick the luma mode by SAD over prediction from decoded refs (decision
 * quality is a later phase's business; the gate only needs consistency) */
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
    /* coverage-honesty tap: N265_MODE_HIST=1 prints the pick histogram at
     * exit so the conformance suite can prove every mode runs */
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

static void set_mode_map(struct n265_encoder *e, int x0, int y0, int n,
                         int mode)
{
    for (int y = 0; y < n; y += 4)
        for (int x = 0; x < n; x += 4)
            e->intra_mode[((y0 + y) >> 2) * e->map_stride + ((x0 + x) >> 2)] =
                (uint8_t)mode;
}

static void mark_cu_edges(struct n265_encoder *e, int x0, int y0, int n)
{
    for (int y = 0; y < n; y += 4)
        e->edge_map[((y0 + y) >> 2) * e->map_stride + (x0 >> 2)] |= 1;
    for (int x = 0; x < n; x += 4)
        e->edge_map[(y0 >> 2) * e->map_stride + ((x0 + x) >> 2)] |= 2;
}

static void encode_cu(struct n265_encoder *e, cabac_t *cb, int x0, int y0,
                      int log2n)
{
    int n = 1 << log2n;
    int qp_c = n265_chroma_qp(e->qp);
    mark_cu_edges(e, x0, y0, n);

    /* at min CB size, decide 2Nx2N vs NxN by summed SAD (NxN searched
     * against source refs since PU recon does not exist yet) */
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
        n265_cabac_bin(cb, &e->ctx[CTX_PART_MODE], !nxn);
        if (getenv("N265_MODE_HIST")) {
            g_nxn += nxn;
            g_2nx2n_min += !nxn;
        }
    } else {
        modes[0] = search_luma_mode(e, x0, y0, log2n, e->recon[0], NULL);
    }

    if (!nxn) {
        int mode = modes[0];
        write_luma_mode(e, cb, x0, y0, mode);
        n265_cabac_bin(cb, &e->ctx[CTX_CHROMA_MODE], 0);    /* DM */

        int cbf_cb = code_component(e, cb, 1, x0 / 2, y0 / 2, log2n - 1,
                                    qp_c, mode, 0);
        int cbf_cr = code_component(e, cb, 2, x0 / 2, y0 / 2, log2n - 1,
                                    qp_c, mode, 0);
        int cbf_l = code_component(e, cb, 0, x0, y0, log2n, e->qp, mode, 0);

        n265_cabac_bin(cb, &e->ctx[CTX_CBF_CHROMA + 0], cbf_cb);
        n265_cabac_bin(cb, &e->ctx[CTX_CBF_CHROMA + 0], cbf_cr);
        n265_cabac_bin(cb, &e->ctx[CTX_CBF_LUMA + 1], cbf_l);

        code_component(e, cb, 0, x0, y0, log2n, e->qp, mode, 1);
        code_component(e, cb, 1, x0 / 2, y0 / 2, log2n - 1, qp_c, mode, 1);
        code_component(e, cb, 2, x0 / 2, y0 / 2, log2n - 1, qp_c, mode, 1);

        set_mode_map(e, x0, y0, n, mode);
        return;
    }

    /* NxN: four PUs, four 4x4 luma TUs (split_transform_flag inferred 1),
     * chroma stays 4x4 at the parent and is coded after the last luma TU.
     * Mode signalling: all four prev_intra flags, then the four
     * mpm_idx/rem values; candidate lists are derived sequentially with
     * earlier PU modes visible (map updated as we go). */
    int prev[4], idx_or_rem[4], is_mpm[4];
    for (int i = 0; i < 4; i++) {
        int px = x0 + (i & 1) * 4, py = y0 + (i >> 1) * 4;
        int cand[3];
        mpm_list(e, px, py, cand);
        int idx = -1;
        for (int k = 0; k < 3; k++)
            if (cand[k] == modes[i])
                idx = k;
        is_mpm[i] = idx >= 0;
        prev[i] = idx >= 0;
        if (idx >= 0) {
            idx_or_rem[i] = idx;
        } else {
            int c[3] = { cand[0], cand[1], cand[2] }, t;
            if (c[0] > c[1]) { t = c[0]; c[0] = c[1]; c[1] = t; }
            if (c[0] > c[2]) { t = c[0]; c[0] = c[2]; c[2] = t; }
            if (c[1] > c[2]) { t = c[1]; c[1] = c[2]; c[2] = t; }
            int rem = modes[i];
            for (int k = 2; k >= 0; k--)
                if (modes[i] > c[k])
                    rem--;
            idx_or_rem[i] = rem;
        }
        set_mode_map(e, px, py, 4, modes[i]);
    }
    for (int i = 0; i < 4; i++)
        n265_cabac_bin(cb, &e->ctx[CTX_PREV_INTRA], prev[i]);
    for (int i = 0; i < 4; i++) {
        if (is_mpm[i]) {
            n265_cabac_bypass(cb, idx_or_rem[i] > 0);
            if (idx_or_rem[i] > 0)
                n265_cabac_bypass(cb, idx_or_rem[i] > 1);
        } else {
            n265_cabac_bypass_bits(cb, (uint32_t)idx_or_rem[i], 5);
        }
    }
    n265_cabac_bin(cb, &e->ctx[CTX_CHROMA_MODE], 0);    /* DM (PU0 mode) */

    /* transform_tree depth 0: chroma cbfs (TBs live here), then the four
     * luma TUs at depth 1 */
    int cbf_cb = code_component(e, cb, 1, x0 / 2, y0 / 2, 2, qp_c,
                                modes[0], 0);
    int cbf_cr = code_component(e, cb, 2, x0 / 2, y0 / 2, 2, qp_c,
                                modes[0], 0);
    n265_cabac_bin(cb, &e->ctx[CTX_CBF_CHROMA + 0], cbf_cb);
    n265_cabac_bin(cb, &e->ctx[CTX_CBF_CHROMA + 0], cbf_cr);

    for (int i = 0; i < 4; i++) {
        int px = x0 + (i & 1) * 4, py = y0 + (i >> 1) * 4;
        int cbf_l = code_component(e, cb, 0, px, py, 2, e->qp, modes[i], 0);
        /* cbf_luma at trafoDepth 1 */
        n265_cabac_bin(cb, &e->ctx[CTX_CBF_LUMA + 0], cbf_l);
        code_component(e, cb, 0, px, py, 2, e->qp, modes[i], 1);
        if (i == 3) {
            code_component(e, cb, 1, x0 / 2, y0 / 2, 2, qp_c, modes[0], 1);
            code_component(e, cb, 2, x0 / 2, y0 / 2, 2, qp_c, modes[0], 1);
        }
    }
}

static void quadtree(struct n265_encoder *e, cabac_t *cb, int x0, int y0,
                     int log2n, int depth)
{
    int n = 1 << log2n;
    int inside = x0 + n <= e->pad_w && y0 + n <= e->pad_h;

    if (inside && log2n > MIN_CB_LOG2) {
        /* we always code split 0 (largest CU); context from neighbors */
        int inc = 0;
        if (x0 > 0)
            inc += e->ct_depth[(y0 >> 2) * e->map_stride + ((x0 - 1) >> 2)] >
                   depth;
        if (y0 > 0)
            inc += e->ct_depth[((y0 - 1) >> 2) * e->map_stride + (x0 >> 2)] >
                   depth;
        n265_cabac_bin(cb, &e->ctx[CTX_SPLIT_CU + inc], 0);
    } else if (!inside && log2n > MIN_CB_LOG2) {
        /* forced split, flag not coded */
        int h = n >> 1;
        for (int i = 0; i < 4; i++) {
            int x1 = x0 + (i & 1) * h, y1 = y0 + (i >> 1) * h;
            if (x1 < e->pad_w && y1 < e->pad_h)
                quadtree(e, cb, x1, y1, log2n - 1, depth + 1);
        }
        return;
    } else if (!inside) {
        return;     /* min CB outside: nothing to code */
    }

    encode_cu(e, cb, x0, y0, log2n);
    for (int y = 0; y < n; y += 4)
        for (int x = 0; x < n; x += 4)
            e->ct_depth[((y0 + y) >> 2) * e->map_stride + ((x0 + x) >> 2)] =
                (uint8_t)depth;
}

void n265_encode_ctu(struct n265_encoder *e, cabac_t *cb, int ctb_x,
                     int ctb_y)
{
    quadtree(e, cb, ctb_x << e->ctb_log2, ctb_y << e->ctb_log2, e->ctb_log2,
             0);
}
