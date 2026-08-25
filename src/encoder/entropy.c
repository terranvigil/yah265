/*
 * Trailing entropy pass: replays the analysis record stream and the
 * frame maps through the real CABAC engine, one slice per frame
 * (docs/threading-design.md sec 2). Everything read here is final:
 * neighbor-derived contexts see exactly the values they had at analysis
 * time because maps are write-once per position per frame.
 * Copyright (c) 2026, the next265 authors. GPL-2.0-or-later; see LICENSE.
 */

#include "encoder/encoder.h"
#include "encoder/intra.h"
#include "encoder/records.h"
#include "encoder/residual.h"
#include "encoder/sao.h"

static void mpm_list(struct n265_encoder *e, int x0, int y0, int cand[3])
{
    int a = INTRA_DC, b = INTRA_DC;
    if (x0 > 0 && e->blk_intra[(y0 >> 2) * e->map_stride + ((x0 - 1) >> 2)])
        a = e->intra_mode[(y0 >> 2) * e->map_stride + ((x0 - 1) >> 2)];
    if (y0 > 0 && (y0 & ((1 << e->ctb_log2) - 1)) != 0 &&
        e->blk_intra[((y0 - 1) >> 2) * e->map_stride + (x0 >> 2)])
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

static int map_at(struct n265_encoder *e, const uint8_t *map, int x, int y)
{
    return map[(y >> 2) * e->map_stride + (x >> 2)];
}

/* EG1, CABAC ones-first bypass form */
static void bypass_eg1(cabac_t *cb, int v)
{
    int b = 1;
    while (v >= (1 << b)) {
        n265_cabac_bypass(cb, 1);
        v -= 1 << b;
        b++;
    }
    n265_cabac_bypass(cb, 0);
    n265_cabac_bypass_bits(cb, (uint32_t)v, b);
}

/* mvd_coding (7.3.8.9) */
static void write_mvd(struct n265_encoder *e, cabac_t *cb, int mx, int my)
{
    int ax = mx < 0 ? -mx : mx, ay = my < 0 ? -my : my;
    n265_cabac_bin(cb, &e->ctx[CTX_MVD_GR0], ax > 0);
    n265_cabac_bin(cb, &e->ctx[CTX_MVD_GR0], ay > 0);
    if (ax > 0)
        n265_cabac_bin(cb, &e->ctx[CTX_MVD_GR1], ax > 1);
    if (ay > 0)
        n265_cabac_bin(cb, &e->ctx[CTX_MVD_GR1], ay > 1);
    if (ax > 0) {
        if (ax > 1)
            bypass_eg1(cb, ax - 2);
        n265_cabac_bypass(cb, mx < 0);
    }
    if (ay > 0) {
        if (ay > 1)
            bypass_eg1(cb, ay - 2);
        n265_cabac_bypass(cb, my < 0);
    }
}

static void replay_residual(struct n265_encoder *e, cabac_t *cb, int c_idx,
                            int log2n, int mode)
{
    int n = 1 << log2n;
    const int16_t *lv = rd_levels(e, n * n);
    int scan = mode == MODE_INTER ? 0 : n265_scan_idx(mode, log2n, c_idx);
    n265_residual_coding(cb, e->ctx, lv, log2n, c_idx, scan);
}

/* replay one 2Nx2N transform tree (intra or zero-MV inter) */
static void replay_tt(struct n265_encoder *e, cabac_t *cb, int mode, int x0,
                      int y0, int log2ts)
{
    int split = log2ts > 5;

    if (split) {
        int h = 1 << (log2ts - 1);
        int cbf_cb = (int)rd_u8(e), cbf_cr = (int)rd_u8(e);
        n265_cabac_bin(cb, &e->ctx[CTX_CBF_CHROMA + 0], cbf_cb);
        n265_cabac_bin(cb, &e->ctx[CTX_CBF_CHROMA + 0], cbf_cr);
        for (int i = 0; i < 4; i++) {
            int x1 = x0 + (i & 1) * h, y1 = y0 + (i >> 1) * h;
            int ccb = (int)rd_u8(e), ccr = (int)rd_u8(e);
            if (cbf_cb)
                n265_cabac_bin(cb, &e->ctx[CTX_CBF_CHROMA + 1], ccb);
            if (cbf_cr)
                n265_cabac_bin(cb, &e->ctx[CTX_CBF_CHROMA + 1], ccr);
            int cbf_l = (int)rd_u8(e);
            n265_cabac_bin(cb, &e->ctx[CTX_CBF_LUMA + 0], cbf_l);
            int m = mode == MODE_INTER ? MODE_INTER
                                       : map_at(e, e->intra_mode, x1, y1);
            if (cbf_l)
                replay_residual(e, cb, 0, log2ts - 1, m);
            if (ccb)
                replay_residual(e, cb, 1, log2ts - 2, m);
            if (ccr)
                replay_residual(e, cb, 2, log2ts - 2, m);
        }
        return;
    }

    int cbf_cb = (int)rd_u8(e), cbf_cr = (int)rd_u8(e);
    int cbf_l = (int)rd_u8(e);
    n265_cabac_bin(cb, &e->ctx[CTX_CBF_CHROMA + 0], cbf_cb);
    n265_cabac_bin(cb, &e->ctx[CTX_CBF_CHROMA + 0], cbf_cr);
    /* inter, depth 0, no chroma cbf: cbf_luma inferred 1 */
    if (!(mode == MODE_INTER && !cbf_cb && !cbf_cr))
        n265_cabac_bin(cb, &e->ctx[CTX_CBF_LUMA + 1], cbf_l);
    if (cbf_l)
        replay_residual(e, cb, 0, log2ts, mode);
    if (cbf_cb)
        replay_residual(e, cb, 1, log2ts - 1, mode);
    if (cbf_cr)
        replay_residual(e, cb, 2, log2ts - 1, mode);
}

static void replay_cu(struct n265_encoder *e, cabac_t *cb, int x0, int y0,
                      int log2n)
{
    int is_skip = map_at(e, e->blk_skip, x0, y0);
    int is_intra = map_at(e, e->blk_intra, x0, y0);

    if (e->slice_type != 2) {
        int inc = 0;
        if (x0 > 0)
            inc += map_at(e, e->blk_skip, x0 - 1, y0);
        if (y0 > 0)
            inc += map_at(e, e->blk_skip, x0, y0 - 1);
        n265_cabac_bin(cb, &e->ctx[CTX_CU_SKIP + inc], is_skip);
        if (is_skip)
            return;     /* merge_idx absent (MaxNumMergeCand == 1) */
        n265_cabac_bin(cb, &e->ctx[CTX_PRED_MODE], is_intra);
        if (!is_intra) {
            n265_cabac_bin(cb, &e->ctx[CTX_PART_MODE], 1);  /* 2Nx2N */
            unsigned flags = rd_u8(e);
            int merge = flags & 1;
            int mvp_idx = (flags >> 1) & 1;
            int root_cbf = (flags >> 2) & 1;
            n265_cabac_bin(cb, &e->ctx[CTX_MERGE_FLAG], merge);
            if (!merge) {
                if (e->slice_type == 0) {
                    /* inter_pred_idc = PRED_L0: bin0 0 (ctx by CU depth),
                     * bin1 0 (ctx 4) */
                    int depth = e->ctb_log2 - log2n;
                    n265_cabac_bin(cb, &e->ctx[CTX_INTER_IDC + depth], 0);
                    n265_cabac_bin(cb, &e->ctx[CTX_INTER_IDC + 4], 0);
                }
                const int16_t *mvd = rd_levels(e, 2);
                write_mvd(e, cb, mvd[0], mvd[1]);
                n265_cabac_bin(cb, &e->ctx[CTX_MVP_FLAG], mvp_idx);
                n265_cabac_bin(cb, &e->ctx[CTX_RQT_ROOT_CBF], root_cbf);
                if (!root_cbf)
                    return;
            }
            replay_tt(e, cb, MODE_INTER, x0, y0, log2n);
            return;
        }
    }

    /* intra */
    int nxn = 0;
    if (log2n == MIN_CB_LOG2) {
        nxn = (int)rd_u8(e);
        n265_cabac_bin(cb, &e->ctx[CTX_PART_MODE], !nxn);
    }

    if (!nxn) {
        int mode = map_at(e, e->intra_mode, x0, y0);
        write_luma_mode(e, cb, x0, y0, mode);
        n265_cabac_bin(cb, &e->ctx[CTX_CHROMA_MODE], 0);    /* DM */
        replay_tt(e, cb, mode, x0, y0, log2n);
        return;
    }

    /* NxN */
    int modes[4], prev[4], idx_or_rem[4];
    for (int i = 0; i < 4; i++) {
        int px = x0 + (i & 1) * 4, py = y0 + (i >> 1) * 4;
        modes[i] = map_at(e, e->intra_mode, px, py);
        int cand[3];
        mpm_list(e, px, py, cand);
        int idx = -1;
        for (int k = 0; k < 3; k++)
            if (cand[k] == modes[i])
                idx = k;
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
    }
    for (int i = 0; i < 4; i++)
        n265_cabac_bin(cb, &e->ctx[CTX_PREV_INTRA], prev[i]);
    for (int i = 0; i < 4; i++) {
        if (prev[i]) {
            n265_cabac_bypass(cb, idx_or_rem[i] > 0);
            if (idx_or_rem[i] > 0)
                n265_cabac_bypass(cb, idx_or_rem[i] > 1);
        } else {
            n265_cabac_bypass_bits(cb, (uint32_t)idx_or_rem[i], 5);
        }
    }
    n265_cabac_bin(cb, &e->ctx[CTX_CHROMA_MODE], 0);        /* DM */

    int cbf_cb = (int)rd_u8(e), cbf_cr = (int)rd_u8(e);
    n265_cabac_bin(cb, &e->ctx[CTX_CBF_CHROMA + 0], cbf_cb);
    n265_cabac_bin(cb, &e->ctx[CTX_CBF_CHROMA + 0], cbf_cr);
    for (int i = 0; i < 4; i++) {
        int cbf_l = (int)rd_u8(e);
        n265_cabac_bin(cb, &e->ctx[CTX_CBF_LUMA + 0], cbf_l);
        if (cbf_l)
            replay_residual(e, cb, 0, 2, modes[i]);
    }
    if (cbf_cb)
        replay_residual(e, cb, 1, 2, modes[0]);
    if (cbf_cr)
        replay_residual(e, cb, 2, 2, modes[0]);
}

static void replay_quadtree(struct n265_encoder *e, cabac_t *cb, int x0,
                            int y0, int log2n, int depth)
{
    int n = 1 << log2n;
    int inside = x0 + n <= e->pad_w && y0 + n <= e->pad_h;

    if (inside && log2n > MIN_CB_LOG2) {
        int split = map_at(e, e->ct_depth, x0, y0) > depth;
        int inc = 0;
        if (x0 > 0)
            inc += map_at(e, e->ct_depth, x0 - 1, y0) > depth;
        if (y0 > 0)
            inc += map_at(e, e->ct_depth, x0, y0 - 1) > depth;
        n265_cabac_bin(cb, &e->ctx[CTX_SPLIT_CU + inc], split);
        if (split) {
            int h = 1 << (log2n - 1);
            for (int i = 0; i < 4; i++)
                replay_quadtree(e, cb, x0 + (i & 1) * h, y0 + (i >> 1) * h,
                                log2n - 1, depth + 1);
            return;
        }
    } else if (!inside && log2n > MIN_CB_LOG2) {
        int h = n >> 1;
        for (int i = 0; i < 4; i++) {
            int x1 = x0 + (i & 1) * h, y1 = y0 + (i >> 1) * h;
            if (x1 < e->pad_w && y1 < e->pad_h)
                replay_quadtree(e, cb, x1, y1, log2n - 1, depth + 1);
        }
        return;
    } else if (!inside) {
        return;
    }

    replay_cu(e, cb, x0, y0, log2n);
}

void n265_entropy_ctu(struct n265_encoder *e, cabac_t *cb, int ctb_x,
                      int ctb_y)
{
    n265_sao_write(e, cb, ctb_x, ctb_y);
    replay_quadtree(e, cb, ctb_x << e->ctb_log2, ctb_y << e->ctb_log2,
                    e->ctb_log2, 0);
}
