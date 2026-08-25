/*
 * Encoder core. Two modes: N265_MODE_INTRA is the real path (DC intra,
 * CTB 32, TU == CU, fixed QP); N265_MODE_PCM is the phase-0 I_PCM path
 * kept for harness A/B. Every frame is an IDR either way.
 * Copyright (c) 2026, the next265 authors. GPL-2.0-or-later; see LICENSE.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "encoder/deblock.h"
#include "encoder/encoder.h"
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
static int g_ctb_log2 = 5;
static int g_keyint = 60;

void n265_set_mode(int mode, int qp, int ctb_log2, int keyint);
void n265_set_mode(int mode, int qp, int ctb_log2, int keyint)
{
    g_mode = mode;
    g_qp = qp;
    g_ctb_log2 = ctb_log2;
    g_keyint = keyint < 1 ? 1 : keyint;
}

void n265_encoder_set_dump(n265_encoder *e, FILE *f)
{
    e->dump_recon = f;
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
    e->ctb_log2 = e->mode == N265_MODE_PCM ? 4 : g_ctb_log2;
    e->keyint = e->mode == N265_MODE_PCM ? 1 : g_keyint;
    e->no_sao = getenv("N265_NO_SAO") != NULL;      /* diagnostic */
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
        int pad = i ? N265_REF_PAD / 2 : N265_REF_PAD;
        e->ref_stride[i] = w + 2 * pad;
        e->ref_base[i] = malloc((size_t)e->ref_stride[i] * (h + 2 * pad));
        if (!e->pad_plane[i] || !e->recon[i] || !e->ref_base[i])
            goto fail;
        e->ref_plane[i] = e->ref_base[i] + pad * e->ref_stride[i] + pad;
    }
    e->map_stride = e->pad_w >> 2;
    e->ct_depth = calloc((size_t)e->map_stride * (e->pad_h >> 2), 1);
    e->intra_mode = calloc((size_t)e->map_stride * (e->pad_h >> 2), 1);
    e->edge_map = calloc((size_t)e->map_stride * (e->pad_h >> 2), 1);
    e->blk_intra = calloc((size_t)e->map_stride * (e->pad_h >> 2), 1);
    e->blk_skip = calloc((size_t)e->map_stride * (e->pad_h >> 2), 1);
    e->blk_cbf_y = calloc((size_t)e->map_stride * (e->pad_h >> 2), 1);
    e->blk_mv = calloc((size_t)e->map_stride * (e->pad_h >> 2) * 2,
                       sizeof(int16_t));
    if (!e->blk_intra || !e->blk_skip || !e->blk_cbf_y || !e->blk_mv)
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
    e->out_cap = nal_pack_bound(frame_bytes + 4096) + 4096;
    e->out = malloc(e->out_cap);
    if (!e->out)
        goto fail;
    n265_tables_init();
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
    free(e->sao);
    for (int i = 0; i < 3; i++) {
        free(e->sao_tmp[i]);
        free(e->ref_base[i]);
    }
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
    if (e->slice_type == 1) {
        bs_put_bits(bs, (uint32_t)(e->poc & 255), 8); /* pic_order_cnt_lsb */
        bs_put_bits(bs, 0, 1);      /* short_term_ref_pic_set_sps_flag */
        /* st_ref_pic_set(0): one negative ref, the previous picture */
        bs_put_ue(bs, 1);           /* num_negative_pics */
        bs_put_ue(bs, 0);           /* num_positive_pics */
        bs_put_ue(bs, 0);           /* delta_poc_s0_minus1[0] */
        bs_put_bits(bs, 1, 1);      /* used_by_curr_pic_s0_flag[0] */
    }
    if (e->mode != N265_MODE_PCM) {
        bs_put_bits(bs, 1, 1);      /* slice_sao_luma_flag */
        bs_put_bits(bs, 1, 1);      /* slice_sao_chroma_flag */
    }
    if (e->slice_type == 1) {
        bs_put_bits(bs, 0, 1);      /* num_ref_idx_active_override_flag */
        bs_put_ue(bs, 4);           /* five_minus_max_num_merge_cand: 1 cand */
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

static void dump_recon_frame(n265_encoder *e)
{
    if (!e->dump_recon)
        return;
    const uint8_t **planes =
        e->mode == N265_MODE_PCM ? (const uint8_t **)e->pad_plane
                                 : (const uint8_t **)e->recon;
    for (int i = 0; i < 3; i++) {
        int w = i ? e->param.width / 2 : e->param.width;
        int h = i ? e->param.height / 2 : e->param.height;
        int stride = e->recon_stride[i];
        for (int y = 0; y < h; y++)
            fwrite(planes[i] + y * stride, 1, (size_t)w, e->dump_recon);
    }
}

int n265_encoder_encode(n265_encoder *e, n265_nal **nal, int *nnal,
                        const n265_picture *pic)
{
    if (!e)
        return -1;
    *nal = e->nal;
    *nnal = 0;
    if (!pic)
        return 0;

    size_t out_used = 0;
    int count = 0;

    int idr = e->mode == N265_MODE_PCM ||
              (e->frames_done % e->keyint) == 0 || !e->ref_valid;
    e->slice_type = idr ? 2 : 1;
    e->poc = idr ? 0 : e->poc + 1;

    if (e->frames_done == 0) {
        n265_write_vps(e); add_nal(e, N265_NAL_VPS, &out_used, &count);
        n265_write_sps(e); add_nal(e, N265_NAL_SPS, &out_used, &count);
        n265_write_pps(e); add_nal(e, N265_NAL_PPS, &out_used, &count);
    }

    pad_plane(e->pad_plane[0], e->pad_stride[0], e->pad_w, e->pad_h,
              pic->plane[0], pic->stride[0], e->param.width, e->param.height);
    for (int i = 1; i < 3; i++)
        pad_plane(e->pad_plane[i], e->pad_stride[i], e->pad_w / 2, e->pad_h / 2,
                  pic->plane[i], pic->stride[i],
                  e->param.width / 2, e->param.height / 2);

    int last = e->ctb_rows * e->ctb_cols - 1;
    cabac_t cb;

    if (e->mode != N265_MODE_PCM) {
        /* analysis: decisions + recon + records, then in-loop filters and
         * SAO decision; the trailing entropy pass replays the records */
        memset(e->edge_map, 0, (size_t)e->map_stride * (e->pad_h >> 2));
        memset(e->blk_intra, 0, (size_t)e->map_stride * (e->pad_h >> 2));
        memset(e->blk_skip, 0, (size_t)e->map_stride * (e->pad_h >> 2));
        memset(e->blk_cbf_y, 0, (size_t)e->map_stride * (e->pad_h >> 2));
        memset(e->blk_mv, 0,
               (size_t)e->map_stride * (e->pad_h >> 2) * 2 * sizeof(int16_t));
        e->rec_len = 0;
        e->rec_pos = 0;
        for (int i = 0; i <= last; i++)
            n265_analyze_ctu(e, i % e->ctb_cols, i / e->ctb_cols);
        n265_deblock_frame(e);
        n265_sao_analyze(e);
        if (e->no_sao)
            memset(e->sao, 0,
                   (size_t)e->ctb_cols * e->ctb_rows * sizeof(sao_params));
        n265_sao_apply(e);
    }

    bs_reset(&e->rbsp);
    write_slice_header(e);
    n265_cabac_start(&cb, &e->rbsp);
    for (int i = 0; i < CTX_COUNT; i++)
        n265_cabac_ctx_init(&e->ctx[i], e->slice_type == 2
                            ? n265_ctx_init_i[i] : n265_ctx_init_p[i],
                            e->qp);

    for (int i = 0; i <= last; i++) {
        if (e->mode == N265_MODE_PCM)
            write_pcm_ctu(e, &cb, &e->ctx[CTX_PART_MODE],
                          i % e->ctb_cols, i / e->ctb_cols);
        else
            n265_entropy_ctu(e, &cb, i % e->ctb_cols, i / e->ctb_cols);
        n265_cabac_terminate(&cb, i == last);
    }
    bs_align_zero(&e->rbsp);
    add_nal(e, idr ? N265_NAL_IDR_W_RADL : 1 /* TRAIL_R */, &out_used,
            &count);

    if (e->mode != N265_MODE_PCM) {
        /* the filtered picture becomes the reference for the next frame,
         * border-extended so MC may read outside the picture */
        for (int i = 0; i < 3; i++) {
            int w = i ? e->pad_w / 2 : e->pad_w;
            int h = i ? e->pad_h / 2 : e->pad_h;
            int pad = i ? N265_REF_PAD / 2 : N265_REF_PAD;
            int rs = e->ref_stride[i];
            for (int y = 0; y < h; y++) {
                uint8_t *d = e->ref_plane[i] + y * rs;
                memcpy(d, e->recon[i] + y * e->recon_stride[i], (size_t)w);
                memset(d - pad, d[0], (size_t)pad);
                memset(d + w, d[w - 1], (size_t)pad);
            }
            for (int y = 1; y <= pad; y++) {
                memcpy(e->ref_plane[i] - y * rs - pad,
                       e->ref_plane[i] - pad, (size_t)rs);
                memcpy(e->ref_plane[i] + (h - 1 + y) * rs - pad,
                       e->ref_plane[i] + (h - 1) * rs - pad, (size_t)rs);
            }
        }
        e->ref_valid = 1;
    }
    dump_recon_frame(e);
    e->frames_done++;
    *nnal = count;
    return (int)out_used;
}
