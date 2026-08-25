/*
 * Coding-tree walk and intra CU encoding (DC mode, 2Nx2N, TU == CU).
 * Copyright (c) 2026, the next265 authors. GPL-2.0-or-later; see LICENSE.
 */

#include <string.h>

#include "encoder/encoder.h"
#include "encoder/intra.h"
#include "encoder/residual.h"
#include "encoder/transform.h"

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
                          int x0, int y0, int log2n, int qp, int coded)
{
    int n = 1 << log2n;
    uint8_t *recon = e->recon[c_idx];
    int stride = e->recon_stride[c_idx];
    const uint8_t *src = e->pad_plane[c_idx] + y0 * e->pad_stride[c_idx] + x0;
    int pic_w = c_idx ? e->pad_w / 2 : e->pad_w;
    int pic_h = c_idx ? e->pad_h / 2 : e->pad_h;
    int ctb_log2 = c_idx ? e->ctb_log2 - 1 : e->ctb_log2;

    uint8_t refs[4 * 32 + 1];
    uint8_t pred[32 * 32];
    n265_intra_refs(refs, recon, stride, x0, y0, log2n, pic_w, pic_h,
                    ctb_log2);
    n265_pred_dc(pred, n, refs, log2n, c_idx == 0);

    int16_t resid[32 * 32], coef[32 * 32], level[32 * 32], dq[32 * 32];
    for (int y = 0; y < n; y++)
        for (int x = 0; x < n; x++)
            resid[y * n + x] =
                (int16_t)(src[y * e->pad_stride[c_idx] + x] - pred[y * n + x]);
    n265_fdct(coef, resid, log2n);
    int nz = n265_quant(level, coef, log2n, qp);

    if (!coded)
        return nz != 0;     /* first pass: just report cbf */

    if (nz) {
        n265_residual_coding(cb, e->ctx, level, log2n, c_idx, 0);
        n265_dequant(dq, level, log2n, qp);
        n265_idct_add(recon + y0 * stride + x0, stride, dq, log2n, pred, n);
    } else {
        for (int y = 0; y < n; y++)
            memcpy(recon + (y0 + y) * stride + x0, pred + y * n, (size_t)n);
    }
    return nz != 0;
}

static void encode_cu(struct n265_encoder *e, cabac_t *cb, int x0, int y0,
                      int log2n)
{
    int n = 1 << log2n;
    int mode = INTRA_DC;

    if (log2n == MIN_CB_LOG2)
        n265_cabac_bin(cb, &e->ctx[CTX_PART_MODE], 1);  /* PART_2Nx2N */

    write_luma_mode(e, cb, x0, y0, mode);
    /* intra_chroma_pred_mode: derived-from-luma is bin 0 */
    n265_cabac_bin(cb, &e->ctx[CTX_CHROMA_MODE], 0);

    /* dry-run the transforms to learn the cbfs (syntax order puts chroma
     * cbfs before any residual), then code for real */
    int qp_c = n265_chroma_qp(e->qp);
    int cbf_cb = code_component(e, cb, 1, x0 / 2, y0 / 2, log2n - 1, qp_c, 0);
    int cbf_cr = code_component(e, cb, 2, x0 / 2, y0 / 2, log2n - 1, qp_c, 0);
    int cbf_l = code_component(e, cb, 0, x0, y0, log2n, e->qp, 0);

    /* transform_tree, no split: cbf_cb, cbf_cr, then transform_unit */
    n265_cabac_bin(cb, &e->ctx[CTX_CBF_CHROMA + 0], cbf_cb);
    n265_cabac_bin(cb, &e->ctx[CTX_CBF_CHROMA + 0], cbf_cr);
    n265_cabac_bin(cb, &e->ctx[CTX_CBF_LUMA + 1], cbf_l);

    code_component(e, cb, 0, x0, y0, log2n, e->qp, 1);
    code_component(e, cb, 1, x0 / 2, y0 / 2, log2n - 1, qp_c, 1);
    code_component(e, cb, 2, x0 / 2, y0 / 2, log2n - 1, qp_c, 1);

    /* bookkeeping maps at 4x4 granularity */
    for (int y = 0; y < n; y += 4)
        for (int x = 0; x < n; x += 4)
            e->intra_mode[((y0 + y) >> 2) * e->map_stride + ((x0 + x) >> 2)] =
                (uint8_t)mode;
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
