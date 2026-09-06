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
static int g_ctb_log2 = 6;
static int g_keyint = 60;

static int g_bframes = 1;
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
void n265_set_mode(int mode, int qp, int ctb_log2, int keyint, int bframes,
                   int aq)
{
    g_mode = mode;
    g_qp = qp;
    g_ctb_log2 = ctb_log2;
    g_keyint = keyint < 1 ? 1 : keyint;
    g_bframes = bframes ? 1 : 0;
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
    e->ctb_log2 = e->mode == N265_MODE_PCM ? 4 : g_ctb_log2;
    e->keyint = e->mode == N265_MODE_PCM ? 1 : g_keyint;
    e->bframes = e->mode == N265_MODE_PCM ? 0 : g_bframes;
    e->no_sao = getenv("N265_NO_SAO") != NULL;      /* diagnostic */
    e->aq = g_aq || getenv("N265_AQ") != NULL;      /* variance AQ */
    e->la_depth = e->mode == N265_MODE_PCM || !e->bframes ? g_la_depth
                                                          : g_la_depth;
    e->cutree = e->mode == N265_MODE_PCM ? 0 : g_cutree;
    e->scenecut = e->mode != N265_MODE_PCM;
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
        for (int sl = 0; sl < 2; sl++) {
            e->dpb[sl].stride[i] = w + 2 * pad;
            e->dpb[sl].base[i] =
                malloc((size_t)e->dpb[sl].stride[i] * (h + 2 * pad));
            if (!e->dpb[sl].base[i])
                goto fail;
            e->dpb[sl].plane[i] =
                e->dpb[sl].base[i] + pad * e->dpb[sl].stride[i] + pad;
        }
    }
    for (int sl = 0; sl < 2; sl++) {
        int c16 = ((e->pad_w + 15) >> 4) * ((e->pad_h + 15) >> 4);
        e->dpb[sl].col_pred = calloc((size_t)c16, 1);
        e->dpb[sl].col_mv = calloc((size_t)c16, 8);
        if (!e->dpb[sl].col_pred || !e->dpb[sl].col_mv)
            goto fail;
    }
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
    e->blk_qp = calloc((size_t)e->map_stride * (e->pad_h >> 2), 1);
    e->ctu_qp_off = calloc((size_t)e->ctb_cols * e->ctb_rows, 1);
    if (!e->blk_intra || !e->blk_skip || !e->blk_cbf_y || !e->blk_mv ||
        !e->blk_pred || !e->blk_qp || !e->ctu_qp_off)
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
    free(e->blk_qp);
    free(e->ctu_qp_off);
    free(e->sao);
    for (int i = 0; i < 3; i++) {
        free(e->sao_tmp[i]);
        free(e->dpb[0].base[i]);
        free(e->dpb[1].base[i]);
    }
    for (int sl = 0; sl < 2; sl++) {
        free(e->dpb[sl].col_pred);
        free(e->dpb[sl].col_mv);
    }
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
        /* st_ref_pic_set(idx 0) written inline */
        int npos = e->slice_type == 0 ? 1 : 0;
        bs_put_ue(bs, 1);           /* num_negative_pics */
        bs_put_ue(bs, (uint32_t)npos);
        bs_put_ue(bs, (uint32_t)(e->poc - e->ref_l0->poc - 1));
        bs_put_bits(bs, 1, 1);      /* used_by_curr_pic_s0_flag[0] */
        if (npos) {
            bs_put_ue(bs, (uint32_t)(e->ref_l1->poc - e->poc - 1));
            bs_put_bits(bs, 1, 1);  /* used_by_curr_pic_s1_flag[0] */
        }
        bs_put_bits(bs, 1, 1);      /* slice_temporal_mvp_enabled_flag */
    }
    if (e->mode != N265_MODE_PCM) {
        bs_put_bits(bs, 1, 1);      /* slice_sao_luma_flag */
        bs_put_bits(bs, 1, 1);      /* slice_sao_chroma_flag */
    }
    if (e->slice_type != 2) {
        bs_put_bits(bs, 0, 1);      /* num_ref_idx_active_override_flag */
        if (e->slice_type == 0)
            bs_put_bits(bs, 0, 1);  /* mvd_l1_zero_flag */
        if (e->slice_type == 0)
            bs_put_bits(bs, 1, 1);  /* collocated_from_l0_flag */
        bs_put_ue(bs, 5 - MAX_MERGE);   /* five_minus_max_num_merge_cand */
    }
    bs_put_se(bs, e->qp - 26);      /* slice_qp_delta */
    if (e->mode != N265_MODE_PCM)
        bs_put_bits(bs, 1, 1);      /* slice_loop_filter_across_slices_enabled
                                     * (present because deblocking is on) */
    bs_align_one_then_zero(bs);     /* byte_alignment() */
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
                         int nonref, int display_idx, size_t *out_used,
                         int *count)
{
    e->slice_type = slice_type;
    e->poc = poc;
    e->nonref = nonref;
    e->display_idx = display_idx;
    /* frame-type QP ladder (the matched reference applies its ip/pb
     * ratios in CQP mode too): I anchors lower, non-ref B higher */
    int q = e->qp_base + (slice_type == 2 ? -3 : slice_type == 0 ? 2 : 0);
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
        if (!e->cutree)
            memset(e->ctu_qp_off, 0, (size_t)e->ctb_cols * e->ctb_rows);
        if (e->aq && getenv("N265_AQ_FLAT"))
            memset(e->ctu_qp_off, (uint8_t)atoi(getenv("N265_AQ_FLAT")),
                   (size_t)e->ctb_cols * e->ctb_rows);
        else if (e->aq) {
            int ctb = 1 << e->ctb_log2;
            int nctu = e->ctb_cols * e->ctb_rows;
            double *lv = malloc((size_t)nctu * sizeof(double));
            double avg = 0;
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
                    lv[cy * e->ctb_cols + cx] = log2(var + 1.0);
                    avg += lv[cy * e->ctb_cols + cx];
                }
            avg /= nctu;
            for (int i = 0; i < nctu; i++) {
                double off = 1.0 * (lv[i] - avg);    /* strength 1.0 */
                int o = (int)(off > 0 ? off + 0.5 : off - 0.5) +
                        e->ctu_qp_off[i];
                e->ctu_qp_off[i] = (int8_t)(o < -12 ? -12 : o > 12 ? 12 : o);
            }
            free(lv);
        }

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
        n265_sao_analyze(e);
        if (e->no_sao)
            memset(e->sao, 0,
                   (size_t)e->ctb_cols * e->ctb_rows * sizeof(sao_params));
        n265_sao_apply(e);
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

    if (e->mode != N265_MODE_PCM && !nonref) {
        /* store the filtered picture into the DPB slot not holding the
         * current L0 anchor */
        struct n265_refpic *slot = e->ref_valid && e->ref_l0 == &e->dpb[0]
                                       ? &e->dpb[1] : &e->dpb[0];
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
                }
            slot->col_ref_poc[0] = e->ref_valid ? e->ref_l0->poc : 0;
            slot->col_ref_poc[1] = (e->slice_type == 0 && e->ref_l1)
                                       ? e->ref_l1->poc
                                       : slot->col_ref_poc[0];
        }
        slot->poc = poc;
        slot->valid = 1;
        e->ref_l0 = slot;       /* newest anchor is the next L0 */
        e->ref_valid = 1;
    }
    dump_recon_frame(e);
    e->frames_done++;
}

/* schedule and encode from the front of the lookahead queue: one
 * display step per call (an anchor may bring the B it delayed). Returns
 * the number of source frames consumed. */
static int drive_scheduler(n265_encoder *e, size_t *out_used, int *count)
{
    n265_la *la = &e->la;
    if (la->count == 0)
        return 0;

    la_frame *f0 = &la->f[0];
    int d0 = f0->display_idx;
    const uint8_t *s0[3] = { f0->src[0], f0->src[1], f0->src[2] };
    int st[3] = { e->pad_stride[0], e->pad_stride[1], e->pad_stride[2] };

    int idr0 = e->mode == N265_MODE_PCM || e->keyint == 1 ||
               (d0 % e->keyint) == 0 || !e->ref_valid ||
               (e->scenecut && d0 - e->gop_start_display >= 4 &&
                n265_la_scenecut(e, 0));

    int is_b_slot = e->bframes && !idr0 &&
                    ((d0 - e->gop_start_display) & 1) == 1;

    memset(e->ctu_qp_off, 0, (size_t)e->ctb_cols * e->ctb_rows);

    if (!is_b_slot || la->count < 2) {
        /* anchor (or a tail frame with no future anchor: code as P) */
        if (idr0) {
            e->gop_start_display = d0;
            e->dpb[0].valid = e->dpb[1].valid = 0;
            e->ref_valid = 0;
        }
        if (e->cutree)
            n265_la_cutree(e, 0);
        encode_frame(e, s0, st, idr0 ? 2 : 1,
                     idr0 ? 0 : d0 - e->gop_start_display, 0, d0,
                     out_used, count);
        n265_la_pop(e);
        return 1;
    }

    /* B slot: its future anchor is queue index 1, unless that frame
     * starts a new GOP, in which case this one closes the old GOP as P */
    la_frame *f1 = &la->f[1];
    int d1 = f1->display_idx;
    int idr1 = (d1 % e->keyint) == 0 ||
               (e->scenecut && d1 - e->gop_start_display >= 4 &&
                n265_la_scenecut(e, 1));
    if (idr1) {
        if (e->cutree)
            n265_la_cutree(e, 0);
        encode_frame(e, s0, st, 1, d0 - e->gop_start_display, 0, d0,
                     out_used, count);
        n265_la_pop(e);
        return 1;
    }

    /* encode the anchor first, then the B between the two anchors */
    struct n265_refpic *prev_anchor = e->ref_l0;
    const uint8_t *s1[3] = { f1->src[0], f1->src[1], f1->src[2] };
    if (e->cutree)
        n265_la_cutree(e, 1);
    encode_frame(e, s1, st, 1, d1 - e->gop_start_display, 0, d1, out_used,
                 count);
    struct n265_refpic *next_anchor = e->ref_l0;
    e->ref_l0 = prev_anchor;
    e->ref_l1 = next_anchor;
    memset(e->ctu_qp_off, 0, (size_t)e->ctb_cols * e->ctb_rows);
    if (e->cutree)
        n265_la_cutree(e, 0);
    encode_frame(e, s0, st, 0, d0 - e->gop_start_display, 1, d0, out_used,
                 count);
    e->ref_l0 = next_anchor;
    n265_la_pop(e);
    n265_la_pop(e);
    return 2;
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
