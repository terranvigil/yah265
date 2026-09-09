/*
 * Encoder core. Two modes: N265_MODE_INTRA is the real path (DC intra,
 * CTB 32, TU == CU, fixed QP); N265_MODE_PCM is the phase-0 I_PCM path
 * kept for harness A/B. Every frame is an IDR either way.
 * Copyright (c) 2026, the yah265 authors. GPL-2.0-or-later; see LICENSE.
 */

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "encoder/deblock.h"
#include "encoder/encoder.h"
#include "encoder/est.h"
#include "encoder/la.h"
#include "encoder/mc.h"
#include "encoder/rc.h"
#include "encoder/sao.h"

const char *n265_version_str(void)
{
    return "0.0.1";
}

void n265_param_default(n265_param *p)
{
    memset(p, 0, sizeof(*p));
    p->csp = N265_CSP_I420;
    p->fps_num = 25;
    p->fps_den = 1;
}

static void pad_plane(uint8_t *dst, int dst_stride, int dst_w, int dst_h,
                      const uint8_t *src, int src_stride, int src_w, int src_h)
{
    for (int y = 0; y < dst_h; y++) {
        const uint8_t *s = src + (y < src_h ? y : src_h - 1) * src_stride;
        uint8_t *d = dst + y * dst_stride;
        memcpy(d, s, (size_t)src_w);
        memset(d + src_w, s[src_w - 1], (size_t)(dst_w - src_w));
    }
}

/* internal knobs the public API doesn't expose yet */
static int g_mode = N265_MODE_INTRA;
static int g_qp = 26;
static double g_crf = -1.0;      /* n265_set_crf; < 0 = CQP */
static double g_bitrate = 0.0;   /* n265_set_bitrate, kbit/s; 0 = off */
static int g_ctb_log2 = 6;
static int g_keyint = 250;      /* x265 4.3's own default (2026-09-08) */

static int g_bframes = 3;         /* hierarchical B default (2026-09-07) */
static int g_ref = 1;            /* n265_set_ref */
static int g_badapt = 1;         /* adaptive B run length, default on */
static int g_aq = 0;
static int g_la_depth = 12;
static int g_cutree = 0;

void n265_set_la(int depth, int cutree);
void n265_set_la(int depth, int cutree)
{
    g_la_depth = depth < 1 ? 1 : depth > LA_MAX - 4 ? LA_MAX - 4 : depth;
    g_cutree = cutree ? 1 : 0;
}

void n265_set_mode(int mode, int qp, int ctb_log2, int keyint, int bframes,
                   int aq);
void n265_set_crf(double crf) { g_crf = crf; }
void n265_set_bitrate(double kbps) { g_bitrate = kbps > 0.0 ? kbps : 0.0; }
void n265_set_ref(int n) { g_ref = n < 1 ? 1 : n > N265_MAX_REF ? N265_MAX_REF : n; }
/* 0 off (fixed cadence), 1 the motion rule, 2 the priced mini-GOP */
void n265_set_badapt(int on) { g_badapt = on < 0 ? 0 : on > 3 ? 3 : on; }

void n265_set_mode(int mode, int qp, int ctb_log2, int keyint, int bframes,
                   int aq)
{
    g_mode = mode;
    g_qp = qp;
    g_ctb_log2 = ctb_log2;
    g_keyint = keyint < 1 ? 1 : keyint;
    g_bframes = bframes < 0 ? 0 : bframes > 7 ? 7 : bframes;
    g_aq = aq ? 1 : 0;
}

void n265_encoder_set_dump(n265_encoder *e, FILE *f)
{
    e->dump_recon = f;
}

void n265_encoder_set_dump_decode(n265_encoder *e, FILE *f);
void n265_encoder_set_dump_decode(n265_encoder *e, FILE *f)
{
    e->dump_decode = f;
}

n265_encoder *n265_encoder_open(const n265_param *p)
{
    if (!p || (p->width & 1) || (p->height & 1) || p->csp != N265_CSP_I420)
        return NULL;

    n265_encoder *e = calloc(1, sizeof(*e));
    if (!e)
        return NULL;
    e->param = *p;
    e->mode = g_mode;
    e->qp = g_qp;
    e->qp_base = g_qp;
    e->crf_on = g_crf >= 0.0;
    e->crf = g_crf;
    /* ABR overrides CRF: the rate factor is solved, not given. The
     * fractional base QP rides with it - see encode_frame. */
    e->abr_on = g_bitrate > 0.0 && e->mode != N265_MODE_PCM;
    e->abr_bitrate = g_bitrate * 1000.0;
    { const char *v = getenv("N265_QP_FRAC");
      e->qp_frac = v ? atoi(v) != 0 : e->abr_on; }
    if (e->abr_on)
        e->crf_on = 0;
    e->ref_max = e->mode == N265_MODE_PCM ? 1 : g_ref;
    e->badapt = g_badapt;
    { const char *v = getenv("N265_CUTREE_STR"); e->cutree_str = v ? (float)atof(v) : 2.0f; }
    /* AQ 0.7 with the joint field rounded once (2026-09-07 sweep vs cu-tree
     * strength: 6/7 clips better, -0.9% mean BD-VMAF-NEG at matched rate;
     * AQ 0.4 and cu-tree 2.5 each bought median by giving bus back 2-3%).
     * N265_AQ_STR=1 N265_QP_ROUND1=0 restores the old field. */
    { const char *v = getenv("N265_AQ_STR"); e->aq_str = v ? (float)atof(v) : 0.7f; }
    { const char *v = getenv("N265_QP_ROUND1"); e->qp_round1 = v ? atoi(v) != 0 : 1; }
    /* rectangular (2NxN/Nx2N) and asymmetric (2NxnU/D, nLx2N, nRx2N)
     * inter partitions. AMP rides on rect: the AMP trial is only entered
     * where a rectangular split already beat 2Nx2N, so turning rect off
     * turns AMP off with it. N265_NO_RECT / N265_NO_AMP are the escapes. */
    e->rect = e->mode != N265_MODE_PCM && !getenv("N265_NO_RECT");
    e->amp = e->rect && !getenv("N265_NO_AMP");
    e->ctb_log2 = e->mode == N265_MODE_PCM ? 4 : g_ctb_log2;
    e->keyint = e->mode == N265_MODE_PCM ? 1 : g_keyint;
    e->bframes = e->mode == N265_MODE_PCM ? 0 : g_bframes;
    e->no_sao = getenv("N265_NO_SAO") != NULL;      /* diagnostic */
    e->cu_stats = getenv("N265_CU_STATS") != NULL;  /* diagnostic */
    /* RDOQ on by default (2026-09-06: -5.9% median BD-VMAF-NEG at matched
     * rate, 7/7 clips, worst -5.5%); N265_RDOQ=0 is the escape. */
    { const char *v = getenv("N265_RDOQ"); e->rdoq = v ? atoi(v) : 1; }
    { const char *v = getenv("N265_PSY"); e->psy = v ? atof(v) : 0.0; }
    e->aq = g_aq || getenv("N265_AQ") != NULL;      /* variance AQ */
    e->la_depth = e->mode == N265_MODE_PCM || !e->bframes ? g_la_depth
                                                          : g_la_depth;
    e->cutree = e->mode == N265_MODE_PCM ? 0 : g_cutree;
    e->qp_delta_on = e->mode != N265_MODE_PCM &&
                     (e->aq || e->cutree || e->qp_frac);
    e->scenecut = e->mode != N265_MODE_PCM;
    /* deblocking offsets: both clamped to the spec's +-6, and both zero by
     * default, which keeps the PPS control block out of the stream */
    { const char *v = getenv("N265_DBF_BETA"); e->dbf_beta = v ? atoi(v) : 0; }
    { const char *v = getenv("N265_DBF_TC"); e->dbf_tc = v ? atoi(v) : 0; }
    if (e->dbf_beta < -6) e->dbf_beta = -6;
    if (e->dbf_beta > 6) e->dbf_beta = 6;
    if (e->dbf_tc < -6) e->dbf_tc = -6;
    if (e->dbf_tc > 6) e->dbf_tc = 6;
    int align = e->mode == N265_MODE_PCM ? 16 : (1 << MIN_CB_LOG2);
    if (p->width < align || p->height < align)
        goto fail;
    e->pad_w = (p->width + align - 1) & ~(align - 1);
    e->pad_h = (p->height + align - 1) & ~(align - 1);
    int ctb = 1 << e->ctb_log2;
    e->ctb_cols = (e->pad_w + ctb - 1) >> e->ctb_log2;
    e->ctb_rows = (e->pad_h + ctb - 1) >> e->ctb_log2;

    for (int i = 0; i < 3; i++) {
        int w = i ? e->pad_w / 2 : e->pad_w;
        int h = i ? e->pad_h / 2 : e->pad_h;
        e->pad_stride[i] = w;
        e->recon_stride[i] = w;
        e->pad_plane[i] = malloc((size_t)w * h);
        e->recon[i] = malloc((size_t)w * h);
        if (!e->pad_plane[i] || !e->recon[i])
            goto fail;
        int pad = i ? N265_REF_PAD / 2 : N265_REF_PAD;
        for (int sl = 0; sl < N265_DPB_SLOTS; sl++) {
            e->dpb[sl].stride[i] = w + 2 * pad;
            e->dpb[sl].base[i] =
                malloc((size_t)e->dpb[sl].stride[i] * (h + 2 * pad));
            if (!e->dpb[sl].base[i])
                goto fail;
            e->dpb[sl].plane[i] =
                e->dpb[sl].base[i] + pad * e->dpb[sl].stride[i] + pad;
        }
    }
    for (int sl = 0; sl < N265_DPB_SLOTS; sl++) {
        int c16 = ((e->pad_w + 15) >> 4) * ((e->pad_h + 15) >> 4);
        e->dpb[sl].col_pred = calloc((size_t)c16, 1);
        e->dpb[sl].col_mv = calloc((size_t)c16, 8);
        e->dpb[sl].col_ref = calloc((size_t)c16, 4);
        if (!e->dpb[sl].col_pred || !e->dpb[sl].col_mv || !e->dpb[sl].col_ref)
            goto fail;
        size_t nctu = (size_t)e->ctb_cols * e->ctb_rows;
        e->dpb[sl].ctu_dmin = malloc(nctu);
        e->dpb[sl].ctu_dmax = malloc(nctu);
        if (!e->dpb[sl].ctu_dmin || !e->dpb[sl].ctu_dmax)
            goto fail;
        memset(e->dpb[sl].ctu_dmin, 0xff, nctu);
        memset(e->dpb[sl].ctu_dmax, 0xff, nctu);
    }
    e->ctu_dmin = malloc((size_t)e->ctb_cols * e->ctb_rows);
    e->ctu_dmax = malloc((size_t)e->ctb_cols * e->ctb_rows);
    if (!e->ctu_dmin || !e->ctu_dmax)
        goto fail;
    e->map_stride = e->pad_w >> 2;
    e->ct_depth = calloc((size_t)e->map_stride * (e->pad_h >> 2), 1);
    e->intra_mode = calloc((size_t)e->map_stride * (e->pad_h >> 2), 1);
    e->edge_map = calloc((size_t)e->map_stride * (e->pad_h >> 2), 1);
    e->blk_intra = calloc((size_t)e->map_stride * (e->pad_h >> 2), 1);
    e->blk_skip = calloc((size_t)e->map_stride * (e->pad_h >> 2), 1);
    e->blk_cbf_y = calloc((size_t)e->map_stride * (e->pad_h >> 2), 1);
    e->blk_mv = calloc((size_t)e->map_stride * (e->pad_h >> 2) * 4,
                       sizeof(int16_t));
    e->blk_pred = calloc((size_t)e->map_stride * (e->pad_h >> 2), 1);
    e->blk_ref = calloc((size_t)e->map_stride * (e->pad_h >> 2) * 2, 1);
    e->blk_qp = calloc((size_t)e->map_stride * (e->pad_h >> 2), 1);
    e->ctu_qp_off = calloc((size_t)e->ctb_cols * e->ctb_rows, 1);
    e->ctu_qp_raw = calloc((size_t)e->ctb_cols * e->ctb_rows, sizeof(float));
    if (!e->blk_intra || !e->blk_skip || !e->blk_cbf_y || !e->blk_mv ||
        !e->blk_pred || !e->blk_qp || !e->ctu_qp_off || !e->ctu_qp_raw)
        goto fail;
    e->sao = calloc((size_t)e->ctb_cols * e->ctb_rows, sizeof(sao_params));
    for (int i = 0; i < 3; i++) {
        int w = i ? e->pad_w / 2 : e->pad_w;
        int h = i ? e->pad_h / 2 : e->pad_h;
        e->sao_tmp[i] = malloc((size_t)w * h);
        if (!e->sao_tmp[i])
            goto fail;
    }
    if (!e->ct_depth || !e->intra_mode || !e->edge_map || !e->sao)
        goto fail;

    size_t frame_bytes = (size_t)e->pad_w * e->pad_h * 3 / 2;
    if (bs_init(&e->rbsp, frame_bytes + 4096) < 0)
        goto fail;
    /* up to two coded frames (anchor + B) leave per call */
    e->out_cap = 3 * nal_pack_bound(frame_bytes + 4096) + 4096;
    e->out = malloc(e->out_cap);
    if (!e->out)
        goto fail;
    if (n265_la_init(e) < 0)
        goto fail;
    n265_rc_open(e);        /* after la_init: it needs the lowres geometry */
    n265_tables_init();
    n265_tables_init_est();
    return e;

fail:
    n265_encoder_close(e);
    return NULL;
}

void n265_encoder_close(n265_encoder *e)
{
    if (!e)
        return;
    for (int i = 0; i < 3; i++) {
        free(e->pad_plane[i]);
        free(e->recon[i]);
    }
    free(e->ct_depth);
    free(e->intra_mode);
    free(e->edge_map);
    free(e->blk_intra);
    free(e->blk_skip);
    free(e->blk_cbf_y);
    free(e->blk_mv);
    free(e->blk_pred);
    free(e->blk_ref);
    free(e->blk_qp);
    free(e->ctu_qp_off);
    free(e->ctu_qp_raw);
    free(e->sao);
    for (int i = 0; i < 3; i++) {
        free(e->sao_tmp[i]);
        for (int sl = 0; sl < N265_DPB_SLOTS; sl++)
            free(e->dpb[sl].base[i]);
    }
    for (int sl = 0; sl < N265_DPB_SLOTS; sl++) {
        free(e->dpb[sl].col_pred);
        free(e->dpb[sl].col_mv);
        free(e->dpb[sl].col_ref);
        free(e->dpb[sl].ctu_dmin);
        free(e->dpb[sl].ctu_dmax);
        for (int i = 0; i < 3; i++)
            free(e->dpb[sl].hf_base[i]);
    }
    free(e->ctu_dmin);
    free(e->ctu_dmax);
    n265_la_free(e);
    bs_free(&e->rbsp);
    free(e->rec_buf);
    free(e->out);
    free(e);
}

static void write_slice_header(n265_encoder *e)
{
    bitstream_t *bs = &e->rbsp;
    bs_put_bits(bs, 1, 1);          /* first_slice_segment_in_pic_flag */
    if (e->slice_type == 2)
        bs_put_bits(bs, 0, 1);      /* no_output_of_prior_pics_flag (IRAP) */
    bs_put_ue(bs, 0);               /* slice_pic_parameter_set_id */
    bs_put_ue(bs, (uint32_t)e->slice_type);
    if (e->slice_type != 2) {
        bs_put_bits(bs, (uint32_t)(e->poc & 255), 8); /* pic_order_cnt_lsb */
        bs_put_bits(bs, 0, 1);      /* short_term_ref_pic_set_sps_flag */
        /* st_ref_pic_set(idx 0) inline: every valid DPB picture, negative
         * POCs nearest first then positive nearest first; used_by_curr
         * marks the ones in the current lists (docs/refstruct-design.md) */
        int negs[N265_DPB_SLOTS], poss[N265_DPB_SLOTS], nn = 0, np = 0;
        for (int sl = 0; sl < N265_DPB_SLOTS; sl++) {
            if (!e->dpb[sl].valid) continue;   /* every kept picture is listed; eviction is at mini-GOP ends */
            if (e->dpb[sl].poc < e->poc) negs[nn++] = sl; else poss[np++] = sl;
        }
        for (int i = 1; i < nn; i++)      /* sort: nearest (largest POC) first */
            for (int k = i; k > 0 && e->dpb[negs[k]].poc > e->dpb[negs[k - 1]].poc; k--) { int t = negs[k]; negs[k] = negs[k - 1]; negs[k - 1] = t; }
        for (int i = 1; i < np; i++)      /* nearest (smallest POC) first */
            for (int k = i; k > 0 && e->dpb[poss[k]].poc < e->dpb[poss[k - 1]].poc; k--) { int t = poss[k]; poss[k] = poss[k - 1]; poss[k - 1] = t; }
        bs_put_ue(bs, (uint32_t)nn);      /* num_negative_pics */
        bs_put_ue(bs, (uint32_t)np);      /* num_positive_pics */
        int prev = e->poc;
        for (int i = 0; i < nn; i++) {
            const struct n265_refpic *r = &e->dpb[negs[i]];
            bs_put_ue(bs, (uint32_t)(prev - r->poc - 1));   /* delta_poc_s0_minus1 */
            int used = 0;
            for (int l = 0; l < 2; l++) for (int k = 0; k < e->nref[l]; k++) if (e->refl[l][k] == r) used = 1;
            bs_put_bits(bs, (uint32_t)used, 1);            /* used_by_curr_pic_s0_flag */
            prev = r->poc;
        }
        prev = e->poc;
        for (int i = 0; i < np; i++) {
            const struct n265_refpic *r = &e->dpb[poss[i]];
            bs_put_ue(bs, (uint32_t)(r->poc - prev - 1));   /* delta_poc_s1_minus1 */
            int used = 0;
            for (int l = 0; l < 2; l++) for (int k = 0; k < e->nref[l]; k++) if (e->refl[l][k] == r) used = 1;
            bs_put_bits(bs, (uint32_t)used, 1);
            prev = r->poc;
        }
        bs_put_bits(bs, getenv("N265_NO_TMVP") ? 0 : 1, 1);      /* slice_temporal_mvp_enabled_flag */
    }
    if (e->mode != N265_MODE_PCM && n265_sao_enabled()) {
        bs_put_bits(bs, 1, 1);      /* slice_sao_luma_flag */
        bs_put_bits(bs, 1, 1);      /* slice_sao_chroma_flag */
    }
    if (e->slice_type != 2) {
        int override = e->nref[0] != 1 || (e->slice_type == 0 && e->nref[1] != 1);
        bs_put_bits(bs, (uint32_t)override, 1);     /* num_ref_idx_active_override_flag */
        if (override) {
            bs_put_ue(bs, (uint32_t)(e->nref[0] - 1));
            if (e->slice_type == 0)
                bs_put_ue(bs, (uint32_t)(e->nref[1] - 1));
        }
        if (e->slice_type == 0)
            bs_put_bits(bs, 0, 1);  /* mvd_l1_zero_flag */
        if (!getenv("N265_NO_TMVP")) {
            if (e->slice_type == 0)
                bs_put_bits(bs, 1, 1);  /* collocated_from_l0_flag */
            if (e->nref[0] > 1)
                bs_put_ue(bs, 0);       /* collocated_ref_idx: L0[0] */
        }
        bs_put_ue(bs, 5 - MAX_MERGE);   /* five_minus_max_num_merge_cand */
    }
    bs_put_se(bs, e->qp - 26);      /* slice_qp_delta */
    if (e->mode != N265_MODE_PCM)
        bs_put_bits(bs, 1, 1);      /* slice_loop_filter_across_slices_enabled
                                     * (present because deblocking is on) */
    bs_align_one_then_zero(bs);     /* byte_alignment() */
}

/* E1: the reference's cached horizontal first stage, three frame-sized
 * 8-tap passes once per stored reference. Every later subpel candidate
 * with a nonzero fx - which is most of them - then skips its own. Off
 * with N265_HFCACHE=0, which is the A/B arm and nothing else: the two
 * paths produce identical bytes. */
static int hf_cache_on(void)
{
    static int on = -1;
    if (on < 0) {
        const char *v = getenv("N265_HFCACHE");
        on = v ? atoi(v) != 0 : 1;
    }
    return on;
}

static void build_hf_cache(n265_encoder *e, struct n265_refpic *r)
{
    for (int i = 0; i < 3; i++)
        r->hf[i] = NULL;
    if (!hf_cache_on())
        return;
    int w = e->pad_w + 2 * N265_HF_PAD, h = e->pad_h + 2 * N265_HF_PAD;
    int rs = r->stride[0];
    const uint8_t *src = r->plane[0] - N265_HF_PAD * rs - N265_HF_PAD;
    r->hf_stride = w;
    for (int i = 0; i < 3; i++) {
        if (!r->hf_base[i]) {
            r->hf_base[i] = malloc((size_t)w * h * sizeof(int16_t));
            if (!r->hf_base[i]) {   /* no cache is a slow path, not a fail */
                for (int k = 0; k < 3; k++)
                    r->hf[k] = NULL;
                return;
            }
        }
        n265_mc_luma_hplane(r->hf_base[i], w, w, h, src, rs, i + 1);
        r->hf[i] = r->hf_base[i] + N265_HF_PAD * w + N265_HF_PAD;
    }
    r->hf_x0 = -N265_HF_PAD;
    r->hf_x1 = e->pad_w + N265_HF_PAD;
    r->hf_y0 = -N265_HF_PAD;
    r->hf_y1 = e->pad_h + N265_HF_PAD;
}

static void write_pcm_ctu(n265_encoder *e, cabac_t *cb, cabac_ctx_t *part_mode,
                          int ctb_x, int ctb_y)
{
    bitstream_t *bs = &e->rbsp;
    int ctb = 1 << e->ctb_log2;
    n265_cabac_bin(cb, part_mode, 1);
    n265_cabac_terminate(cb, 1);
    bs_align_zero(bs);              /* pcm_alignment_zero_bit */

    int x0 = ctb_x * ctb;
    int y0 = ctb_y * ctb;
    const uint8_t *l = e->pad_plane[0] + y0 * e->pad_stride[0] + x0;
    for (int y = 0; y < ctb; y++)
        for (int x = 0; x < ctb; x++)
            bs_put_bits(bs, l[y * e->pad_stride[0] + x], 8);
    for (int p = 1; p <= 2; p++) {
        const uint8_t *c = e->pad_plane[p] +
            (y0 / 2) * e->pad_stride[p] + x0 / 2;
        for (int y = 0; y < ctb / 2; y++)
            for (int x = 0; x < ctb / 2; x++)
                bs_put_bits(bs, c[y * e->pad_stride[p] + x], 8);
    }
    n265_cabac_start(cb, bs);
}

static void add_nal(n265_encoder *e, int type, size_t *out_used, int *nnal)
{
    size_t sz = nal_pack(e->out + *out_used, e->out_cap - *out_used,
                         type, e->rbsp.buf, e->rbsp.bytes);
    e->nal[*nnal].type = type;
    e->nal[*nnal].payload = e->out + *out_used;
    e->nal[*nnal].size = (int)sz;
    (*nnal)++;
    *out_used += sz;
}

static void dump_planes(n265_encoder *e, FILE *f)
{
    const uint8_t **planes =
        e->mode == N265_MODE_PCM ? (const uint8_t **)e->pad_plane
                                 : (const uint8_t **)e->recon;
    for (int i = 0; i < 3; i++) {
        int w = i ? e->param.width / 2 : e->param.width;
        int h = i ? e->param.height / 2 : e->param.height;
        int stride = e->recon_stride[i];
        for (int y = 0; y < h; y++)
            fwrite(planes[i] + y * stride, 1, (size_t)w, f);
    }
}

static void dump_recon_frame(n265_encoder *e)
{
    if (e->dump_recon) {
        /* display order: frames land at their display index (B frames
         * are encoded out of order, so seek) */
        long frame_bytes =
            (long)e->param.width * e->param.height * 3 / 2;
        fseek(e->dump_recon, (long)e->display_idx * frame_bytes, SEEK_SET);
        dump_planes(e, e->dump_recon);
    }
    if (e->dump_decode)
        dump_planes(e, e->dump_decode);
}

/* encode one frame from the given source planes; appends NALs */
static void encode_frame(n265_encoder *e, const uint8_t *const src[3],
                         const int src_stride[3], int slice_type, int poc,
                         int nonref, int display_idx, int b_depth,
                         size_t *out_used, int *count)
{
    size_t frame_out0 = *out_used;
    e->slice_type = slice_type;
    e->b_depth = b_depth;
    e->poc = poc;
    e->nonref = nonref;
    e->display_idx = display_idx;
    /* P2: no CTU of this picture has a decided depth range yet */
    memset(e->ctu_dmin, 0xff, (size_t)e->ctb_cols * e->ctb_rows);
    memset(e->ctu_dmax, 0xff, (size_t)e->ctb_cols * e->ctb_rows);
    /* frame-type QP ladder (the matched reference applies its ip/pb
     * ratios in CQP mode too): I anchors lower, non-ref B higher */
    /* CRF (yah264's final form, written for this encoder): a constant base per
     * frame, crf + the cu-tree operating-point shift. The cu-tree offsets are
     * left un-centred under CRF (a referenced region nets a lower QP, a leaf
     * region does not), so their mean-negative field is netted out by a
     * fixed shift and only the DIFFERENTIAL survives; centring them would
     * throw the cross-frame allocation away. N265_CRF_SHIFT (default 5.4 =
     * (1 - qcomp) * 13.5 at qcomp 0.6, 0 without cu-tree). The AQ frame-mean
     * term lands on the base below, once the variances are known. */
    double base = e->qp_base;
    if (e->abr_on) {
        /* ABR: the base is the rate factor rc.c solved for this frame from
         * the bits spent against the bits planned (docs/instruments.md). */
        base = e->rc_rf;
    } else if (e->crf_on) {
        double shift = 0.0;
        if (e->cutree) { const char *v = getenv("N265_CRF_SHIFT"); shift = v ? atof(v) : 5.4; }
        base = e->crf + shift;
    }
    /* the pyramid QP cascade: I anchors -3, P anchors +0, and each B layer
     * one step deeper than its parent (the reference B nearest the anchors
     * codes best, the leaves worst). N265_PB_STEP scales the per-layer
     * increment. */
    double pb = 1.0; { const char *v = getenv("N265_PB_STEP"); if (v) pb = atof(v); }   /* live in CQP and CRF alike */
    int cascade = slice_type == 2 ? -3 : slice_type == 0 ? (int)lround(1 + b_depth * pb) : 0;
    /* The signalled base QP is an integer, so a control variable that only
     * moves it is quantised to 5-15% of the rate per step and cannot land on
     * a size. Carry the base's fraction instead and spend it over the CTUs
     * below; qfrac is exactly 0 when the fractional path is off, which is
     * what keeps CQP and CRF byte-identical to the pre-ABR encoder. */
    int qi = (int)lround(base);
    double qfrac = e->qp_frac ? base - (double)qi : 0.0;
    int q = qi + cascade;
    e->qp = q < 0 ? 0 : q > 51 ? 51 : q;

    if (e->frames_done == 0) {
        n265_write_vps(e); add_nal(e, N265_NAL_VPS, out_used, count);
        n265_write_sps(e); add_nal(e, N265_NAL_SPS, out_used, count);
        n265_write_pps(e); add_nal(e, N265_NAL_PPS, out_used, count);
    }

    pad_plane(e->pad_plane[0], e->pad_stride[0], e->pad_w, e->pad_h,
              src[0], src_stride[0], e->param.width, e->param.height);
    for (int i = 1; i < 3; i++)
        pad_plane(e->pad_plane[i], e->pad_stride[i], e->pad_w / 2,
                  e->pad_h / 2, src[i], src_stride[i],
                  e->param.width / 2, e->param.height / 2);

    int last = e->ctb_rows * e->ctb_cols - 1;
    cabac_t cb;

    if (e->mode != N265_MODE_PCM) {
        memset(e->edge_map, 0, (size_t)e->map_stride * (e->pad_h >> 2));
        memset(e->blk_intra, 0, (size_t)e->map_stride * (e->pad_h >> 2));
        memset(e->blk_skip, 0, (size_t)e->map_stride * (e->pad_h >> 2));
        memset(e->blk_cbf_y, 0, (size_t)e->map_stride * (e->pad_h >> 2));
        memset(e->blk_pred, 0, (size_t)e->map_stride * (e->pad_h >> 2));
        memset(e->blk_mv, 0,
               (size_t)e->map_stride * (e->pad_h >> 2) * 4 * sizeof(int16_t));
        /* per-CTU QP offsets: cu-tree (filled by the scheduler) plus
         * variance AQ on top */
        if (!e->cutree) {
            memset(e->ctu_qp_off, 0, (size_t)e->ctb_cols * e->ctb_rows);
            memset(e->ctu_qp_raw, 0, (size_t)e->ctb_cols * e->ctb_rows * sizeof(float));
        }
        if (e->aq && getenv("N265_AQ_FLAT"))
            memset(e->ctu_qp_off, (uint8_t)atoi(getenv("N265_AQ_FLAT")),
                   (size_t)e->ctb_cols * e->ctb_rows);
        else if (e->aq) {
            int ctb = 1 << e->ctb_log2;
            int nctu = e->ctb_cols * e->ctb_rows;
            double *lv = malloc((size_t)nctu * sizeof(double));
            double avg = 0;
            /* N265_AQ_FLOOR: a floor on the CTU variance the AQ field is
             * built from. The term models masking, and masking is a
             * property of content the eye can see; below the coding noise
             * floor there is nothing to mask and nothing to hide, so a
             * CTU flatter than the floor is not flatter for this purpose.
             * Natural video has no such CTUs (film grain and sensor noise
             * put a variance floor in every block); synthetic animation is
             * full of them, and without a floor the field reads a
             * perfectly flat fill as maximally masking-free and drives it
             * many QP steps DOWN, buying bits for a region that codes to
             * nothing while the outlines that carry the picture pay. 4 is
             * a standard deviation of two luma levels: below it an 8-bit
             * picture is flat to within the smallest step the quantiser
             * reproduces. 0 is the pre-2026-09-09 behaviour and is
             * exactly inert.
             *
             * The floor reaches the per-CTU term ONLY. The frame mean below
             * it feeds a different term with a different job - the AQ_DC
             * across-shot allocation - and there the unfloored mean is the
             * right quantity: a black frame really is cheaper to code, and
             * flooring it makes the complexity proxy worse. Measured: with
             * the floor on both, the across-shot gate on ms_720p_50 went
             * -16.92% to -16.08%; with it on the per-CTU field alone the
             * gate holds. */
            double vfloor = 4.0;
            { const char *v = getenv("N265_AQ_FLOOR"); if (v) vfloor = atof(v); }
            double avg_dc = 0;      /* the same field WITHOUT the floor */
            for (int cy = 0; cy < e->ctb_rows; cy++)
                for (int cx = 0; cx < e->ctb_cols; cx++) {
                    int x0 = cx * ctb, y0 = cy * ctb;
                    int w = x0 + ctb > e->pad_w ? e->pad_w - x0 : ctb;
                    int h = y0 + ctb > e->pad_h ? e->pad_h - y0 : ctb;
                    int64_t s1 = 0, s2 = 0;
                    for (int y = y0; y < y0 + h; y++)
                        for (int x = x0; x < x0 + w; x++) {
                            int v = e->pad_plane[0][y * e->pad_stride[0] + x];
                            s1 += v;
                            s2 += v * v;
                        }
                    int np = w * h;
                    double var = (double)s2 / np -
                        ((double)s1 / np) * ((double)s1 / np);
                    avg_dc += log2((var < 0.0 ? 0.0 : var) + 1.0);
                    if (var < vfloor) var = vfloor;
                    lv[cy * e->ctb_cols + cx] = log2(var + 1.0);
                    avg += lv[cy * e->ctb_cols + cx];
                }
            avg /= nctu;
            avg_dc /= nctu;
            /* Under CRF the frame mean of the AQ field is the across-frame /
             * across-shot allocation term (yah264 row 10, 2026-09-05): a busy
             * frame codes a little cheaper, a quiet one a little better. It
             * goes on the BASE QP at its own strength (N265_AQ_DC, default
             * 1.0) against an absolute anchor in log2(variance) units
             * (N265_AQ_ANCHOR, default 6.4), so every base-keyed decision sees
             * it; the per-CTU part below stays centred on the frame. */
            if (e->crf_on || e->abr_on) {
                const char *v = getenv("N265_AQ_DC"), *a = getenv("N265_AQ_ANCHOR");
                double dc = (v ? atof(v) : 1.0) * (avg_dc - (a ? atof(a) : 6.4));
                int dci = (int)lround(dc);
                int nq = e->qp + dci;
                e->qp = nq < 0 ? 0 : nq > 51 ? 51 : nq;
                if (e->qp_frac)
                    qfrac += dc - (double)dci;
            }
            /* per-CTU term at N265_AQ_STR. Default: round it on its own and
             * add it to the already-rounded cu-tree offset (two roundings);
             * N265_QP_ROUND1 sums the two float fields and rounds once, so
             * fractional strengths on either side survive. */
            /* The per-CTU term is a perceptual correction, not a rate
             * lever, and it is proportional to the frame's variance SPREAD,
             * which is unbounded: flat animation puts black CTUs and drawn
             * detail in the same frame and the term reaches +-4 QP, taxing
             * exactly the CTUs the metric reads. N265_AQ_CLAMP bounds it
             * before the cu-tree field is added. 12 is the old effective
             * bound (the offset byte's own clamp); 3 ships (2026-09-08) and
             * is inert wherever the spread is small - four of the seven CIF
             * clips are byte-identical under it and the worst moves 0.43. */
            double aqc = 3.0;
            { const char *v = getenv("N265_AQ_CLAMP"); if (v) aqc = atof(v); }
            if (getenv("N265_AQ_TRACE")) {
                /* the field the per-CTU term is built from: how far the
                 * frame's log2-variance spread reaches, and how much of
                 * it the clamp is already eating */
                double mn = 1e9, mx = -1e9, sd = 0, lo = 0, hi = 0;
                for (int i = 0; i < nctu; i++) {
                    if (lv[i] < mn) mn = lv[i];
                    if (lv[i] > mx) mx = lv[i];
                    sd += (lv[i] - avg) * (lv[i] - avg);
                    double o = e->aq_str * (lv[i] - avg);
                    lo += o < -aqc;
                    hi += o > aqc;
                }
                fprintf(stderr, "AQT,%d,%d,%d,%.3f,%.3f,%.3f,%.3f,%.4f,%.4f\n",
                        display_idx, slice_type, nctu, avg, mn, mx,
                        sqrt(sd / nctu), lo / nctu, hi / nctu);
            }
            for (int i = 0; i < nctu; i++) {
                double off = e->aq_str * (lv[i] - avg);
                if (off > aqc) off = aqc;
                else if (off < -aqc) off = -aqc;
                int o;
                if (e->qp_round1) {
                    off += e->ctu_qp_raw[i];
                    o = (int)(off > 0 ? off + 0.5 : off - 0.5);
                } else {
                    o = (int)(off > 0 ? off + 0.5 : off - 0.5) +
                        e->ctu_qp_off[i];
                }
                e->ctu_qp_off[i] = (int8_t)(o < -12 ? -12 : o > 12 ? 12 : o);
            }
            free(lv);
        }

        /* Spend the base QP's fractional part over the CTUs: each one is
         * rounded against its own threshold from a low-discrepancy
         * sequence, so the frame's MEAN QP is the float base to within one
         * CTU and the achieved rate is continuous in the control variable.
         * The threshold is a pure function of the CTU raster index and the
         * frame counter - not of a running error - so it stays
         * position-independent and survives any future thread schedule. */
        if (e->qp_frac && qfrac != 0.0) {
            int nctu = e->ctb_cols * e->ctb_rows;
            for (int i = 0; i < nctu; i++) {
                double d = (double)(i + 1) * 0.7548776662466927 +
                           (double)(e->frames_done + 1) * 0.5698402909980532;
                d -= floor(d);
                int o = e->ctu_qp_off[i] + (int)floor(qfrac + d);
                e->ctu_qp_off[i] = (int8_t)(o < -12 ? -12 : o > 12 ? 12 : o);
            }
        }

        if (getenv("N265_QP_TRACE"))
            fprintf(stderr, "QPT disp=%d type=%d qp=%d qf=%+.3f\n",
                    display_idx, slice_type, e->qp, qfrac);
        e->ent_qp_prev = e->qp;
        e->ent_parent_chroma = 0;
        e->rec_len = 0;
        e->rec_pos = 0;
        for (int i = 0; i < CTX_COUNT; i++)
            n265_cabac_ctx_init(&e->est_ctx[i],
                                e->slice_type == 2 ? n265_ctx_init_i[i]
                                : e->slice_type == 1 ? n265_ctx_init_p[i]
                                                     : n265_ctx_init_b[i],
                                e->qp);
        for (int i = 0; i <= last; i++)
            n265_analyze_ctu(e, i % e->ctb_cols, i / e->ctb_cols);
        e->rec_pos = 0;     /* rewind for the entropy pass */
        n265_deblock_frame(e);
        if (n265_sao_enabled()) {
            n265_sao_analyze(e);
            if (e->no_sao)
                memset(e->sao, 0,
                       (size_t)e->ctb_cols * e->ctb_rows * sizeof(sao_params));
            n265_sao_apply(e);
        }
    }

    e->ent_qp_prev = e->qp;
    e->ent_parent_chroma = 0;
    e->in_entropy = 1;
    bs_reset(&e->rbsp);
    write_slice_header(e);
    n265_cabac_start(&cb, &e->rbsp);
    for (int i = 0; i < CTX_COUNT; i++)
        n265_cabac_ctx_init(&e->ctx[i],
                            e->slice_type == 2 ? n265_ctx_init_i[i]
                            : e->slice_type == 1 ? n265_ctx_init_p[i]
                                                 : n265_ctx_init_b[i],
                            e->qp);

    for (int i = 0; i <= last; i++) {
        if (e->mode == N265_MODE_PCM)
            write_pcm_ctu(e, &cb, &e->ctx[CTX_PART_MODE],
                          i % e->ctb_cols, i / e->ctb_cols);
        else
            n265_entropy_ctu(e, &cb, i % e->ctb_cols, i / e->ctb_cols);
        n265_cabac_terminate(&cb, i == last);
    }
    e->in_entropy = 0;
    bs_align_zero(&e->rbsp);
    add_nal(e, e->slice_type == 2 ? N265_NAL_IDR_W_RADL
                                  : (nonref ? 0 /* TRAIL_N */
                                            : 1 /* TRAIL_R */),
            out_used, count);

    if (e->cu_stats) {
        /* one CSV row per frame: what the entropy pass actually wrote.
         * Shares are of the coded luma area, so they sum to 1. */
        double a = 0;
        for (int i = 0; i < N265_CS_ZEROCBF; i++) a += (double)e->cs_area[i];
        if (a != (double)e->pad_w * e->pad_h)
            fprintf(stderr, "CUS-BUG frame %d: area %.0f != %d\n",
                    display_idx, a, e->pad_w * e->pad_h);
        if (a <= 0) a = 1;
        double tu = 0;
        for (int i = 0; i < 4; i++) tu += (double)e->cs_tu[i];
        if (tu <= 0) tu = 1;
        fprintf(stderr,
                "CUS,%d,%d,%d,%d,%zu,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,"
                "%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,%llu\n",
                display_idx, slice_type, b_depth, e->qp,
                *out_used - frame_out0,
                e->cs_area[N265_CS_SKIP] / a, e->cs_area[N265_CS_MERGE] / a,
                e->cs_area[N265_CS_AMVP] / a, e->cs_area[N265_CS_RECT] / a,
                e->cs_area[N265_CS_AMP] / a, e->cs_area[N265_CS_INTRA] / a,
                e->cs_area[N265_CS_INTRA_NXN] / a,
                e->cs_area[N265_CS_ZEROCBF] / a,
                e->cs_tu[0] / tu, e->cs_tu[1] / tu, e->cs_tu[2] / tu,
                e->cs_tu[3] / tu,
                (double)(e->cs_tu_cbf[0] + e->cs_tu_cbf[1] +
                         e->cs_tu_cbf[2] + e->cs_tu_cbf[3]) / tu,
                (unsigned long long)e->cs_coef);
        memset(e->cs_area, 0, sizeof e->cs_area);
        memset(e->cs_tu, 0, sizeof e->cs_tu);
        memset(e->cs_tu_cbf, 0, sizeof e->cs_tu_cbf);
        e->cs_coef = 0;
    }

    if (e->mode != N265_MODE_PCM && !nonref) {
        /* store the filtered picture into the DPB slot not holding the
         * current L0 anchor */
        /* the slot to overwrite: an invalid one, else the oldest picture
         * that is not in the current lists */
        struct n265_refpic *slot = NULL;
        for (int sl = 0; sl < N265_DPB_SLOTS && !slot; sl++)
            if (!e->dpb[sl].valid) slot = &e->dpb[sl];
        for (int sl = 0; sl < N265_DPB_SLOTS && !slot; sl++) {
            int in_list = 0;
            for (int l = 0; l < 2; l++)
                for (int k = 0; k < e->nref[l]; k++)
                    if (e->refl[l][k] == &e->dpb[sl]) in_list = 1;
            if (!in_list && (!slot || e->dpb[sl].poc < slot->poc)) slot = &e->dpb[sl];
        }
        if (!slot) slot = &e->dpb[0];
        for (int i = 0; i < 3; i++) {
            int w = i ? e->pad_w / 2 : e->pad_w;
            int h = i ? e->pad_h / 2 : e->pad_h;
            int pad = i ? N265_REF_PAD / 2 : N265_REF_PAD;
            int rs = slot->stride[i];
            for (int y = 0; y < h; y++) {
                uint8_t *d = slot->plane[i] + y * rs;
                memcpy(d, e->recon[i] + y * e->recon_stride[i], (size_t)w);
                memset(d - pad, d[0], (size_t)pad);
                memset(d + w, d[w - 1], (size_t)pad);
            }
            for (int y = 1; y <= pad; y++) {
                memcpy(slot->plane[i] - y * rs - pad,
                       slot->plane[i] - pad, (size_t)rs);
                memcpy(slot->plane[i] + (h - 1 + y) * rs - pad,
                       slot->plane[i] + (h - 1) * rs - pad, (size_t)rs);
            }
        }
        build_hf_cache(e, slot);
        {
            int c16w = (e->pad_w + 15) >> 4;
            int c16h = (e->pad_h + 15) >> 4;
            for (int cy = 0; cy < c16h; cy++)
                for (int cx = 0; cx < c16w; cx++) {
                    int m = (cy * 4) * e->map_stride + cx * 4;
                    int i16 = cy * c16w + cx;
                    if (e->slice_type == 2 || e->blk_intra[m]) {
                        slot->col_pred[i16] = 0;
                        continue;
                    }
                    slot->col_pred[i16] = e->blk_pred[m];
                    memcpy(slot->col_mv + 4 * i16, e->blk_mv + 4 * m, 8);
                    for (int l = 0; l < 2; l++)
                        slot->col_ref[2 * i16 + l] = (e->blk_pred[m] & (1 << l))
                            ? (int16_t)e->refl[l][e->blk_ref[2 * m + l]]->poc : 0;
                }
            slot->col_ref_poc[0] = e->ref_valid ? e->refl[0][0]->poc : 0;
            slot->col_ref_poc[1] = (e->slice_type == 0 && e->refl[1][0])
                                       ? e->refl[1][0]->poc
                                       : slot->col_ref_poc[0];
        }
        /* P2: carry this picture's per-CTU depth range with it, so a
         * later frame can read the collocated range */
        memcpy(slot->ctu_dmin, e->ctu_dmin, (size_t)e->ctb_cols * e->ctb_rows);
        memcpy(slot->ctu_dmax, e->ctu_dmax, (size_t)e->ctb_cols * e->ctb_rows);
        slot->poc = poc;
        slot->valid = 1;
        slot->is_anchor = e->slice_type != 0;   /* I/P; a reference B is not */
        e->ref_valid = 1;
    }
    dump_recon_frame(e);
    if (e->abr_on || e->rc.trace)
        n265_rc_done(e, e->rc_cls, e->rc_cplx, base,
                     (double)(*out_used - frame_out0) * 8.0);
    e->frames_done++;
}

/* Build the reference lists for the picture that follows POC `cur`
 * (or, with `future` set, for a B whose L1 starts at `future`): L0 = valid
 * DPB pictures before cur, nearest first, up to ref_max; L1 = the future
 * picture then the past ones. A caller coding a B passes the past anchor
 * POC as `cur` so the L0 head is that anchor. */
void n265_build_lists(n265_encoder *e, int poc, int is_b, struct n265_refpic *future)
{
    (void)future;
    /* every valid DPB picture, split at `poc`: past = poc < this, nearest
     * (largest POC) first; future = poc > this, nearest (smallest) first.
     * L0 = past then future (for B); L1 = future then past (B only). */
    struct n265_refpic *past[N265_DPB_SLOTS], *fut[N265_DPB_SLOTS];
    int np = 0, nf = 0;
    for (int sl = 0; sl < N265_DPB_SLOTS; sl++) {
        if (!e->dpb[sl].valid) continue;
        if (e->dpb[sl].poc < poc) past[np++] = &e->dpb[sl];
        else if (e->dpb[sl].poc > poc) fut[nf++] = &e->dpb[sl];
    }
    for (int i = 1; i < np; i++)
        for (int k = i; k > 0 && past[k]->poc > past[k - 1]->poc; k--) { struct n265_refpic *t = past[k]; past[k] = past[k - 1]; past[k - 1] = t; }
    for (int i = 1; i < nf; i++)
        for (int k = i; k > 0 && fut[k]->poc < fut[k - 1]->poc; k--) { struct n265_refpic *t = fut[k]; fut[k] = fut[k - 1]; fut[k - 1] = t; }
    e->nref[0] = e->nref[1] = 0;
    for (int i = 0; i < np && e->nref[0] < e->ref_max; i++) e->refl[0][e->nref[0]++] = past[i];
    if (is_b) for (int i = 0; i < nf && e->nref[0] < e->ref_max; i++) e->refl[0][e->nref[0]++] = fut[i];
    if (is_b) {
        for (int i = 0; i < nf && e->nref[1] < e->ref_max; i++) e->refl[1][e->nref[1]++] = fut[i];
        for (int i = 0; i < np && e->nref[1] < e->ref_max; i++) e->refl[1][e->nref[1]++] = past[i];
    }
}

/* The minimum distance between the last key frame and a SCENECUT key
 * frame, in display frames. `--keyint` still forces one on its own
 * period; this only bounds how close together the detector may place
 * them. Ours was 4 - one mini-GOP - which is a lower bound with no
 * derivation, and on cut-out animation the detector reaches it: the
 * lookahead's pcost/icost ratio sits near 1 on every frame that moves,
 * so every anchor becomes an IDR (sita_toon: 30 of 120 frames intra,
 * 65% of the file). x265's own default is auto = min(keyint/10, fps),
 * i.e. 24-25 at keyint 250, and the same argument applies here: an IDR
 * costs the whole prediction chain, so the detector has to be very sure
 * before it may place one close to the last. N265_MIN_KEYINT sets it;
 * 0 means the auto rule; 4 is the pre-2026-09-09 behaviour. */
static int min_keyint(n265_encoder *e)
{
    static int v = -2;
    if (v == -2) { const char *s = getenv("N265_MIN_KEYINT"); v = s ? atoi(s) : 4; }
    int m = v;
    if (m == 0) {
        int fps = e->param.fps_den > 0
                      ? (e->param.fps_num + e->param.fps_den / 2) / e->param.fps_den
                      : 25;
        m = e->keyint / 10;
        if (fps < m) m = fps;
    }
    return m < 1 ? 1 : m;
}

/* Is the lookahead frame at index i a forced key frame (IDR)? keyint,
 * a scene cut, or the encoder's first frame. */
static int la_is_idr(n265_encoder *e, int i)
{
    int d = e->la.f[i].display_idx;
    return e->mode == N265_MODE_PCM || e->keyint == 1 || (d % e->keyint) == 0 ||
           !e->ref_valid ||
           (e->scenecut && d - e->gop_start_display >= min_keyint(e) &&
            n265_la_scenecut(e, i));
}

/* Emit the pyramid coding order for the B lookahead indices [blo, bhi],
 * bracketed by already-coded references: the midpoint first (a reference if
 * it has children on either side), then its two halves. */
static void plan_brange(n265_encoder *e, int blo, int bhi, int depth)
{
    if (blo > bhi)
        return;
    int mid = (blo + bhi) / 2;
    int is_ref = (blo <= mid - 1) || (mid + 1 <= bhi);
    int k = e->plan_len++;
    e->plan_li[k] = mid; e->plan_kind[k] = 0; e->plan_depth[k] = depth;
    e->plan_nonref[k] = !is_ref;
    plan_brange(e, blo, mid - 1, depth + 1);
    plan_brange(e, mid + 1, bhi, depth + 1);
}

/* Build the coding-order plan for the mini-GOP at the front of the queue. */
static void build_plan(n265_encoder *e)
{
    n265_la *la = &e->la;
    e->plan_len = e->plan_pos = 0;
    if (la_is_idr(e, 0)) {
        e->gop_start_display = la->f[0].display_idx;
        for (int sl = 0; sl < N265_DPB_SLOTS; sl++) e->dpb[sl].valid = 0;
        e->ref_valid = 0;
        e->plan_li[0] = 0; e->plan_kind[0] = 2; e->plan_depth[0] = 0; e->plan_nonref[0] = 0;
        e->plan_len = 1; e->plan_pop = 1;
        return;
    }
    /* the anchor offset A: as many B frames as bframes, cut short by the
     * first forced key frame in the run (that frame opens the next GOP). */
    int maxa = e->bframes < la->count ? e->bframes : la->count - 1;
    int A = 0;
    /* adaptive B placement: extend the B run frame by frame, but end it at
     * the first frame whose lookahead motion x inter/intra ratio says a far
     * bidirectional prediction will code badly (high-motion pans hurt as
     * deep B; foliage/texture with low motion do not). N265_BADAPT_THR the
     * cutoff on (mean |lowres MV| per block) x (pcost/icost).
     *
     * The motion term is a count of LOWRES PIXELS, so the same physical pan
     * reads 3-5x larger at 720p/1080p than at the CIF the cutoff was
     * calibrated on, and the run collapses to one B frame on any HD clip
     * that moves. N265_BADAPT_REFW scales it back to a fraction of the
     * frame width: the term is multiplied by refw / lowres width, with
     * refw the width the cutoff is taken to hold at. The scale is CLAMPED
     * AT 1.0, so the normalisation can only ever relax the cutoff, never
     * tighten it: everything at or below refw - the whole CIF half of the
     * corpus, where the 0.35 was read - is byte-identical to the
     * pre-2026-09-08 encoder, and only the clips whose motion the term was
     * over-reading move. 0 turns it off (the pre-2026-09-08 encoder). */
    double bthr = 0.35; { const char *v = getenv("N265_BADAPT_THR"); if (v) bthr = atof(v); }
    double refw = 128.0; { const char *v = getenv("N265_BADAPT_REFW"); if (v) refw = atof(v); }
    double wscale = 1.0;
    if (refw > 0.0 && la->lw > 0 && (double)la->lw > refw)
        wscale = refw / (double)la->lw;
    int nb = la->bw * la->bh;
    int trace = getenv("N265_BADAPT_TRACE") != NULL;
    /* how far the key-frame edge lets the run go at all */
    int keymax = maxa;
    for (int i = 1; i <= maxa; i++) {
        if (la_is_idr(e, i)) { keymax = i - 1; break; }
    }
    /* b-adapt 2: price the mini-GOP instead of ruling on motion. The run
     * length is the one whose amortised lowres cost is lowest - one anchor
     * predicted at distance A+1 plus A bidirectionally-predicted leaves,
     * over A+1 frames. It has no calibrated constant in it. */
    if (e->badapt == 3 && keymax >= 1) {
        double dthr = 1.6;
        { const char *v = getenv("N265_BADAPT_DEG"); if (v) dthr = atof(v); }
        A = n265_la_degrade(e, keymax, dthr);
    } else if (e->badapt == 2 && keymax >= 1) {
        A = n265_la_brun(e, keymax);
    } else
    for (int i = 1; i <= maxa; i++) {
        if (la_is_idr(e, i)) break;
        A = i;
        if (e->badapt) {
            double mv = (double)la->f[i].mvabs_sum / (nb ? nb : 1);
            double m = mv * wscale * (double)la->f[i].sc_ratio;
            if (trace)
                fprintf(stderr, "BADAPT d=%d i=%d mv=%.3f ratio=%.3f "
                        "scale=%.3f m=%.4f thr=%.4f %s\n",
                        la->f[i].display_idx, i, mv,
                        (double)la->f[i].sc_ratio, wscale, m, bthr,
                        m > bthr ? "END" : "extend");
            if (m > bthr) break;    /* this frame ends the run (it is the anchor) */
        }
    }
    if (A == 0) {                       /* next frame is a key frame: front is a plain P */
        e->plan_li[0] = 0; e->plan_kind[0] = 1; e->plan_depth[0] = 0; e->plan_nonref[0] = 0;
        e->plan_len = 1; e->plan_pop = 1;
        return;
    }
    /* the anchor first, then the B pyramid between the anchors */
    e->plan_li[0] = A; e->plan_kind[0] = 1; e->plan_depth[0] = 0; e->plan_nonref[0] = 0;
    e->plan_len = 1;
    plan_brange(e, 0, A - 1, 1);
    e->plan_pop = A + 1;
}

/* Encode one frame from the plan (building it first if none is active).
 * Returns the number of source frames retired from the queue (0 until the
 * plan completes, then the whole mini-GOP). */
static int drive_scheduler(n265_encoder *e, size_t *out_used, int *count)
{
    n265_la *la = &e->la;
    if (la->count == 0)
        return 0;
    if (e->plan_pos >= e->plan_len)
        build_plan(e);

    int li = e->plan_li[e->plan_pos];
    int kind = e->plan_kind[e->plan_pos];
    la_frame *f = &la->f[li];
    const uint8_t *src[3] = { f->src[0], f->src[1], f->src[2] };
    int st[3] = { e->pad_stride[0], e->pad_stride[1], e->pad_stride[2] };
    int poc = f->display_idx - e->gop_start_display;

    memset(e->ctu_qp_off, 0, (size_t)e->ctb_cols * e->ctb_rows);
    if (e->cutree)
        n265_la_cutree(e, li);
    /* reference lists from the DPB around this frame's POC (anchors have
     * only L0; B frames have both) */
    n265_build_lists(e, kind == 2 ? poc : poc, kind == 0, NULL);
    int slice_type = kind == 2 ? 2 : kind == 1 ? 1 : 0;
    e->sc_ratio = f->sc_ratio;      /* lookahead pcost/icost, P3 reads it */
    e->la_icost = f->icost;         /* the same field per CTU, R3 reads it */
    e->la_pcost = f->pcost;
    e->la_bw = la->bw;
    e->la_bh = la->bh;
    e->rc_cls = n265_rc_class(slice_type, e->plan_nonref[e->plan_pos]);
    e->rc_cplx = n265_rc_cplx(e, li, e->rc_cls);
    if (e->abr_on)
        e->rc_rf = n265_rc_next(e, e->rc_cls, e->rc_cplx);
    if (kind != 0)          /* the priced b-adapt prices against this plane */
        n265_la_set_anchor(e, li);
    encode_frame(e, src, st, slice_type, poc, e->plan_nonref[e->plan_pos],
                 f->display_idx, e->plan_depth[e->plan_pos], out_used, count);
    e->plan_pos++;

    if (e->plan_pos >= e->plan_len) {
        int pop = e->plan_pop;
        for (int i = 0; i < pop; i++) n265_la_pop(e);
        /* the mini-GOP is done: reference B frames live only within it, and
         * only ref_max anchors carry into the next one. Retire the rest so
         * the DPB (and the next slice's RPS) hold exactly what is reusable. */
        for (int sl = 0; sl < N265_DPB_SLOTS; sl++)
            if (e->dpb[sl].valid && !e->dpb[sl].is_anchor) e->dpb[sl].valid = 0;
        int nanchor = 0;
        for (int sl = 0; sl < N265_DPB_SLOTS; sl++) if (e->dpb[sl].valid) nanchor++;
        while (nanchor > e->ref_max) {
            int oldest = -1;
            for (int sl = 0; sl < N265_DPB_SLOTS; sl++)
                if (e->dpb[sl].valid && (oldest < 0 || e->dpb[sl].poc < e->dpb[oldest].poc)) oldest = sl;
            e->dpb[oldest].valid = 0; nanchor--;
        }
        return pop;
    }
    return 0;
}

int n265_encoder_encode(n265_encoder *e, n265_nal **nal, int *nnal,
                        const n265_picture *pic)
{
    if (!e)
        return -1;
    *nal = e->nal;
    *nnal = 0;
    size_t out_used = 0;
    int count = 0;

    if (pic) {
        const uint8_t *planes[3] = { pic->plane[0], pic->plane[1],
                                     pic->plane[2] };
        n265_la_push(e, planes, pic->stride);
        e->in_display_idx++;
        while (e->la.count > e->la_depth)
            drive_scheduler(e, &out_used, &count);
    } else {
        /* flush: one scheduler step per call until drained */
        drive_scheduler(e, &out_used, &count);
    }

    *nnal = count;
    return (int)out_used;
}
