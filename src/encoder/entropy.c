/*
 * Trailing entropy pass: replays the analysis record stream and the
 * frame maps through the real CABAC engine, one slice per frame
 * (docs/threading-design.md sec 2). Everything read here is final:
 * neighbor-derived contexts see exactly the values they had at analysis
 * time because maps are write-once per position per frame.
 * Copyright (c) 2026, the yah265 authors. GPL-2.0-or-later; see LICENSE.
 */

#include <stdio.h>
#include <stdlib.h>

#include "encoder/encoder.h"
#include "encoder/intra.h"
#include "encoder/records.h"
#include "encoder/residual.h"
#include "encoder/sao.h"
#include "encoder/preset.h"

static void mpm_list(struct n265_frame_work *e, int x0, int y0, int cand[3])
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

static void write_luma_mode(struct n265_frame_work *e, const res_ops *ops,
                            int x0, int y0, int mode)
{
    int cand[3];
    mpm_list(e, x0, y0, cand);
    int idx = -1;
    for (int i = 0; i < 3; i++)
        if (cand[i] == mode)
            idx = i;
    ops->bin(ops->h, CTX_PREV_INTRA, idx >= 0);
    if (idx >= 0) {
        ops->bypass(ops->h, idx > 0);
        if (idx > 0)
            ops->bypass(ops->h, idx > 1);
    } else {
        int c[3] = { cand[0], cand[1], cand[2] }, t;
        if (c[0] > c[1]) { t = c[0]; c[0] = c[1]; c[1] = t; }
        if (c[0] > c[2]) { t = c[0]; c[0] = c[2]; c[2] = t; }
        if (c[1] > c[2]) { t = c[1]; c[1] = c[2]; c[2] = t; }
        int rem = mode;
        for (int i = 2; i >= 0; i--)
            if (mode > c[i])
                rem--;
        ops->bypass_bits(ops->h, (uint32_t)rem, 5);
    }
}

static int map_at(struct n265_frame_work *e, const uint8_t *map, int x, int y)
{
    return map[(y >> 2) * e->map_stride + (x >> 2)];
}

/* EG1, CABAC ones-first bypass form */
static void bypass_eg1(const res_ops *ops, int v)
{
    int b = 1;
    while (v >= (1 << b)) {
        ops->bypass(ops->h, 1);
        v -= 1 << b;
        b++;
    }
    ops->bypass(ops->h, 0);
    ops->bypass_bits(ops->h, (uint32_t)v, b);
}

/* mvd_coding (7.3.8.9) */
static void write_mvd(const res_ops *ops, int mx, int my)
{
    int ax = mx < 0 ? -mx : mx, ay = my < 0 ? -my : my;
    ops->bin(ops->h, CTX_MVD_GR0, ax > 0);
    ops->bin(ops->h, CTX_MVD_GR0, ay > 0);
    if (ax > 0)
        ops->bin(ops->h, CTX_MVD_GR1, ax > 1);
    if (ay > 0)
        ops->bin(ops->h, CTX_MVD_GR1, ay > 1);
    if (ax > 0) {
        if (ax > 1)
            bypass_eg1(ops, ax - 2);
        ops->bypass(ops->h, mx < 0);
    }
    if (ay > 0) {
        if (ay > 1)
            bypass_eg1(ops, ay - 2);
        ops->bypass(ops->h, my < 0);
    }
}

/* cu_qp_delta_abs/sign at the first cbf TU of a quantization group
 * (QG == CTB): TU prefix cMax 5 (bin0 ctx 0, later bins ctx 1), EG0
 * escape, bypass sign */
static void maybe_qp_delta(struct n265_frame_work *e, const res_ops *ops,
                           int any_cbf)
{
    if (!e->qp_delta_on || !any_cbf || e->ent_qp_coded)
        return;
    e->ent_qp_coded = 1;
    int delta = e->ent_ctu_qp - e->ent_qp_prev;
    if (n265_env("N265_QP_TRACE"))
        fprintf(stderr, "[%s %d->%d]", e->in_entropy ? "ENT" : "ana",
                e->ent_qp_prev, e->ent_ctu_qp);
    int a = delta < 0 ? -delta : delta;
    int prefix = a < 5 ? a : 5;
    for (int i = 0; i < prefix; i++)
        ops->bin(ops->h, CTX_CU_QP + (i > 0), 1);
    if (prefix < 5)
        ops->bin(ops->h, CTX_CU_QP + (prefix > 0), 0);
    else {
        int v = a - 5, b = 0;
        while (v >= (1 << b)) {
            ops->bypass(ops->h, 1);
            v -= 1 << b;
            b++;
        }
        ops->bypass(ops->h, 0);
        if (b)
            ops->bypass_bits(ops->h, (uint32_t)v, b);
    }
    if (a)
        ops->bypass(ops->h, delta < 0);
}

static void replay_residual(struct n265_frame_work *e, const res_ops *ops,
                            int c_idx, int log2n, int mode)
{
    int n = 1 << log2n;
    const int16_t *lv = rd_levels(e, n * n);
    int scan = mode == MODE_INTER ? 0 : n265_scan_idx(mode, log2n, c_idx);
    if (e->cu_stats && e->in_entropy && c_idx == 0)
        for (int i = 0; i < n * n; i++)
            e->cs_coef += lv[i] != 0;
    n265_residual_coding(ops, lv, log2n, c_idx, scan);
}

/* replay one 2Nx2N transform tree, general RQT form (maxTB 32, min TB 4,
 * MaxTrafoDepth 1). Record layout per node: [tsplit byte when the flag is
 * coded][cbf_cb][cbf_cr when log2ts > 2], then children or the leaf's
 * [cbf_l][levels l][levels cb][levels cr]. A CB8 split keeps chroma at
 * the parent, coded after the last luma child. */
static void replay_tt(struct n265_frame_work *e, const res_ops *ops, int mode,
                      int x0, int y0, int log2ts, int depth, int pcb,
                      int pcr)
{
    int coded_flag = log2ts <= 5 && log2ts > 2 && depth == 0;
    int split = log2ts > 5 ? 1 : coded_flag ? (int)rd_u8(e) : 0;
    if (coded_flag)
        ops->bin(ops->h, CTX_TSPLIT + 5 - log2ts, split);

    int cbf_cb = pcb, cbf_cr = pcr;
    if (log2ts > 2) {
        cbf_cb = (int)rd_u8(e);
        cbf_cr = (int)rd_u8(e);
        if (depth == 0 || pcb)
            ops->bin(ops->h, CTX_CBF_CHROMA + depth, cbf_cb);
        else
            cbf_cb = 0;
        if (depth == 0 || pcr)
            ops->bin(ops->h, CTX_CBF_CHROMA + depth, cbf_cr);
        else
            cbf_cr = 0;
    }

    int m = mode == MODE_INTER ? MODE_INTER
                               : map_at(e, e->intra_mode, x0, y0);

    if (split) {
        int h = 1 << (log2ts - 1);
        int chroma_at_parent = log2ts == 3;
        for (int i = 0; i < 4; i++) {
            int x1 = x0 + (i & 1) * h, y1 = y0 + (i >> 1) * h;
            /* every 4x4 child TU sees the parent's chroma cbfs in its
             * transform_unit condition (7.3.8.10) */
            if (chroma_at_parent && (cbf_cb || cbf_cr))
                e->ent_parent_chroma = 1;
            replay_tt(e, ops, mode, x1, y1, log2ts - 1, depth + 1, cbf_cb,
                      cbf_cr);
            e->ent_parent_chroma = 0;
            if (chroma_at_parent && i == 3) {
                if (cbf_cb)
                    replay_residual(e, ops, 1, log2ts - 1, m);
                if (cbf_cr)
                    replay_residual(e, ops, 2, log2ts - 1, m);
            }
        }
        return;
    }

    /* leaf */
    int cbf_l = (int)rd_u8(e);
    if (e->cu_stats && e->in_entropy && log2ts >= 2 && log2ts <= 5) {
        e->cs_tu[log2ts - 2]++;
        e->cs_tu_cbf[log2ts - 2] += cbf_l != 0;
    }
    if (mode == MODE_INTER && depth == 0 && !cbf_cb && !cbf_cr)
        ;   /* cbf_luma inferred 1 */
    else
        ops->bin(ops->h, CTX_CBF_LUMA + (depth == 0), cbf_l);
    maybe_qp_delta(e, ops, cbf_l || (log2ts > 2 && (cbf_cb || cbf_cr)) ||
                   e->ent_parent_chroma);
    if (cbf_l)
        replay_residual(e, ops, 0, log2ts, m);
    if (log2ts > 2) {
        if (cbf_cb)
            replay_residual(e, ops, 1, log2ts - 1, m);
        if (cbf_cr)
            replay_residual(e, ops, 2, log2ts - 1, m);
    }
}

/* merge_idx: TU with cMax MAX_MERGE-1; first bin ctx-coded, the rest
 * bypass */
static void write_merge_idx(const res_ops *ops, int idx)
{
    ops->bin(ops->h, CTX_MERGE_IDX, idx > 0);
    for (int i = 1; i < MAX_MERGE - 1 && idx > 0; i++) {
        ops->bypass(ops->h, idx > i);
        if (idx <= i)
            break;
    }
}

/* part_mode for an INTER CU (Table 9-34). ctxInc is binIdx for bins 0
 * and 1; bin 2 takes ctx 3 above the min CB (the min-CB ctx 2 cannot
 * occur here: MinCbLog2SizeY is 3, where the code is two bins and inter
 * NxN does not exist). Bin 3, the AMP up/down choice, is bypass. */
void n265_write_part_mode(struct n265_frame_work *e, const res_ops *ops,
                          int log2n, int part)
{
    ops->bin(ops->h, CTX_PART_MODE + 0, part == PART_2Nx2N);
    if (part == PART_2Nx2N)
        return;
    ops->bin(ops->h, CTX_PART_MODE + 1, PART_IS_HORIZ(part));
    if (log2n == MIN_CB_LOG2 || !e->amp)
        return;
    ops->bin(ops->h, CTX_PART_MODE + 3, !PART_IS_AMP(part));
    if (PART_IS_AMP(part))
        ops->bypass(ops->h, part == PART_2NxnD || part == PART_nRx2N);
}

/* inter_pred_idc (9.3.3.9): bin0 is coded by CU depth and selects bi;
 * an 8x4 or 4x8 PU cannot be bi-predicted, so it codes the list bin
 * alone */
static void write_inter_idc(const res_ops *ops, int depth, int pw, int ph,
                            int pred)
{
    if (pw + ph != 12) {
        ops->bin(ops->h, CTX_INTER_IDC + depth, pred == 3);
        if (pred == 3)
            return;
    }
    ops->bin(ops->h, CTX_INTER_IDC + 4, pred >> 1);
}

/* one PU's motion syntax; returns merge_flag */
static int replay_pu(struct n265_frame_work *e, const res_ops *ops, int log2n,
                     int pw, int ph)
{
    unsigned flags = rd_u8(e);
    int merge = flags & 1;
    ops->bin(ops->h, CTX_MERGE_FLAG, merge);
    if (merge) {
        write_merge_idx(ops, (int)(flags >> 3) & 7);
        return 1;
    }
    int pred = ((int)(flags >> 3) & 3) + 1;
    int mvp_idx[2] = { (int)(flags >> 1) & 1, (int)(flags >> 5) & 1 };
    if (e->slice_type == 0)
        write_inter_idc(ops, e->ctb_log2 - log2n, pw, ph, pred);
    for (int l = 0; l < 2; l++) {
        if (!(pred & (1 << l)))
            continue;
        int ref = (int)rd_u8(e);
        if (e->nref[l] > 1) {
            /* ref_idx_lX: truncated unary, cMax = nref - 1, bins 0
             * and 1 context coded, the rest bypass */
            int cmax = e->nref[l] - 1;
            ops->bin(ops->h, CTX_REF_IDX + 0, ref > 0);
            if (ref > 0 && cmax > 1)
                ops->bin(ops->h, CTX_REF_IDX + 1, ref > 1);
            for (int b = 2; b < cmax && ref > b - 1; b++)
                ops->bypass(ops->h, ref > b);
        }
        const int16_t *mvd = rd_levels(e, 2);
        write_mvd(ops, mvd[0], mvd[1]);
        ops->bin(ops->h, CTX_MVP_FLAG, mvp_idx[l]);
    }
    return 0;
}

static void replay_cu(struct n265_frame_work *e, const res_ops *ops, int x0,
                      int y0, int log2n)
{
    int is_skip = map_at(e, e->blk_skip, x0, y0);
    int is_intra = map_at(e, e->blk_intra, x0, y0);

    if (e->slice_type != 2) {
        int inc = 0;
        if (x0 > 0)
            inc += map_at(e, e->blk_skip, x0 - 1, y0);
        if (y0 > 0)
            inc += map_at(e, e->blk_skip, x0, y0 - 1);
        ops->bin(ops->h, CTX_CU_SKIP + inc, is_skip);
        if (is_skip) {
            write_merge_idx(ops, (int)rd_u8(e));
            if (e->cu_stats && e->in_entropy)
                e->cs_area[N265_CS_SKIP] += (uint64_t)1 << (2 * log2n);
            return;
        }
        ops->bin(ops->h, CTX_PRED_MODE, is_intra);
        if (!is_intra) {
            int part = (int)rd_u8(e);
            n265_write_part_mode(e, ops, log2n, part);
            int npu = N265_NUM_PU(part), merge0 = 0;
            for (int p = 0; p < npu; p++) {
                int px, py, pw, ph;
                n265_pu_rect(part, p, x0, y0, 1 << log2n, &px, &py, &pw, &ph);
                int m = replay_pu(e, ops, log2n, pw, ph);
                if (p == 0)
                    merge0 = m;
            }
            int root_cbf = (int)rd_u8(e);
            /* rqt_root_cbf is absent, and inferred 1, only for a merged
             * 2Nx2N CU (7.3.8.5) */
            if (!(part == PART_2Nx2N && merge0))
                ops->bin(ops->h, CTX_RQT_ROOT_CBF, root_cbf);
            if (e->cu_stats && e->in_entropy) {
                uint64_t a = (uint64_t)1 << (2 * log2n);
                e->cs_area[PART_IS_AMP(part) ? N265_CS_AMP
                           : part != PART_2Nx2N ? N265_CS_RECT
                           : merge0 ? N265_CS_MERGE : N265_CS_AMVP] += a;
                if (!root_cbf)
                    e->cs_area[N265_CS_ZEROCBF] += a;
            }
            if (!root_cbf)
                return;
            replay_tt(e, ops, MODE_INTER, x0, y0, log2n, 0, 1, 1);
            return;
        }
    }

    /* intra */
    int nxn = 0;
    if (log2n == MIN_CB_LOG2) {
        nxn = (int)rd_u8(e);
        ops->bin(ops->h, CTX_PART_MODE, !nxn);
    }
    if (e->cu_stats && e->in_entropy)
        e->cs_area[nxn ? N265_CS_INTRA_NXN : N265_CS_INTRA] +=
            (uint64_t)1 << (2 * log2n);

    if (!nxn) {
        int mode = map_at(e, e->intra_mode, x0, y0);
        write_luma_mode(e, ops, x0, y0, mode);
        ops->bin(ops->h, CTX_CHROMA_MODE, 0);    /* DM */
        replay_tt(e, ops, mode, x0, y0, log2n, 0, 1, 1);
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
        ops->bin(ops->h, CTX_PREV_INTRA, prev[i]);
    for (int i = 0; i < 4; i++) {
        if (prev[i]) {
            ops->bypass(ops->h, idx_or_rem[i] > 0);
            if (idx_or_rem[i] > 0)
                ops->bypass(ops->h, idx_or_rem[i] > 1);
        } else {
            ops->bypass_bits(ops->h, (uint32_t)idx_or_rem[i], 5);
        }
    }
    ops->bin(ops->h, CTX_CHROMA_MODE, 0);        /* DM */

    int cbf_cb = (int)rd_u8(e), cbf_cr = (int)rd_u8(e);
    ops->bin(ops->h, CTX_CBF_CHROMA + 0, cbf_cb);
    ops->bin(ops->h, CTX_CBF_CHROMA + 0, cbf_cr);
    for (int i = 0; i < 4; i++) {
        int cbf_l = (int)rd_u8(e);
        ops->bin(ops->h, CTX_CBF_LUMA + 0, cbf_l);
        /* 4x4 luma TUs read the PARENT's chroma cbfs in their
         * transform_unit condition (7.3.8.10), all four of them */
        maybe_qp_delta(e, ops, cbf_l || cbf_cb || cbf_cr);
        if (cbf_l)
            replay_residual(e, ops, 0, 2, modes[i]);
    }
    if (cbf_cb)
        replay_residual(e, ops, 1, 2, modes[0]);
    if (cbf_cr)
        replay_residual(e, ops, 2, 2, modes[0]);
}

static void replay_quadtree(struct n265_frame_work *e, const res_ops *ops,
                            int x0, int y0, int log2n, int depth)
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
        ops->bin(ops->h, CTX_SPLIT_CU + inc, split);
        if (split) {
            int h = 1 << (log2n - 1);
            for (int i = 0; i < 4; i++)
                replay_quadtree(e, ops, x0 + (i & 1) * h, y0 + (i >> 1) * h,
                                log2n - 1, depth + 1);
            return;
        }
    } else if (!inside && log2n > MIN_CB_LOG2) {
        int h = n >> 1;
        for (int i = 0; i < 4; i++) {
            int x1 = x0 + (i & 1) * h, y1 = y0 + (i >> 1) * h;
            if (x1 < e->pad_w && y1 < e->pad_h)
                replay_quadtree(e, ops, x1, y1, log2n - 1, depth + 1);
        }
        return;
    } else if (!inside) {
        return;
    }

    replay_cu(e, ops, x0, y0, log2n);
}

void n265_entropy_ctu(struct n265_frame_work *e, cabac_t *cb, int ctb_x,
                      int ctb_y)
{
    int q = e->qp + e->ctu_qp_off[ctb_y * e->ctb_cols + ctb_x];
    e->ent_ctu_qp = q < 0 ? 0 : q > 51 ? 51 : q;
    e->ent_qp_coded = 0;
    real_emit re = { cb, e->ctx };
    res_ops ops_s;
    n265_real_ops(&ops_s, &re);
    const res_ops *ops = &ops_s;
    if (n265_sao_enabled())
        n265_sao_write(e, cb, ctb_x, ctb_y);
    replay_quadtree(e, ops, ctb_x << e->ctb_log2, ctb_y << e->ctb_log2,
                    e->ctb_log2, 0);
    if (e->ent_qp_coded)
        e->ent_qp_prev = e->ent_ctu_qp;
}

/* estimator advance: replay one CU's just-committed records through the
 * cost-model contexts (rec_pos must point at the CU's record start) */
void n265_est_advance_cu(struct n265_frame_work *e, est_state *st, int x0,
                         int y0, int log2n)
{
    res_ops ops_s;
    n265_est_ops(&ops_s, st);
    replay_cu(e, &ops_s, x0, y0, log2n);
}
