/*
 * Encoder core, phase 0: every frame an IDR whose CTUs are all I_PCM, so
 * reconstruction is the input bit-exact and the whole NAL/slice/CABAC
 * framing gets exercised end to end.
 * Copyright (c) 2026, the next265 authors. GPL-2.0-or-later; see LICENSE.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "encoder/encoder.h"

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

n265_encoder *n265_encoder_open(const n265_param *p)
{
    if (!p || p->width < N265_CTB_SIZE || p->height < N265_CTB_SIZE ||
        (p->width & 1) || (p->height & 1) || p->csp != N265_CSP_I420)
        return NULL;

    n265_encoder *e = calloc(1, sizeof(*e));
    if (!e)
        return NULL;
    e->param = *p;
    e->pad_w = (p->width + N265_CTB_SIZE - 1) & ~(N265_CTB_SIZE - 1);
    e->pad_h = (p->height + N265_CTB_SIZE - 1) & ~(N265_CTB_SIZE - 1);
    e->ctb_cols = e->pad_w / N265_CTB_SIZE;
    e->ctb_rows = e->pad_h / N265_CTB_SIZE;

    for (int i = 0; i < 3; i++) {
        int w = i ? e->pad_w / 2 : e->pad_w;
        int h = i ? e->pad_h / 2 : e->pad_h;
        e->pad_stride[i] = w;
        e->pad_plane[i] = malloc((size_t)w * h);
        if (!e->pad_plane[i])
            goto fail;
    }
    /* PCM payload dominates: luma+chroma bytes plus generous framing room */
    size_t frame_bytes = (size_t)e->pad_w * e->pad_h * 3 / 2;
    if (bs_init(&e->rbsp, frame_bytes + 4096) < 0)
        goto fail;
    e->out_cap = nal_pack_bound(frame_bytes + 4096) + 4096;
    e->out = malloc(e->out_cap);
    if (!e->out)
        goto fail;
    return e;

fail:
    n265_encoder_close(e);
    return NULL;
}

void n265_encoder_close(n265_encoder *e)
{
    if (!e)
        return;
    for (int i = 0; i < 3; i++)
        free(e->pad_plane[i]);
    bs_free(&e->rbsp);
    free(e->out);
    free(e);
}

/* slice_segment_header for an IDR I slice, everything defaulted (7.3.6.1) */
static void write_slice_header(n265_encoder *e)
{
    bitstream_t *bs = &e->rbsp;
    bs_put_bits(bs, 1, 1);          /* first_slice_segment_in_pic_flag */
    bs_put_bits(bs, 0, 1);          /* no_output_of_prior_pics_flag (IRAP) */
    bs_put_ue(bs, 0);               /* slice_pic_parameter_set_id */
    bs_put_ue(bs, 2);               /* slice_type: I */
    /* IDR: no POC, no RPS. SAO off, deblock settled in PPS. */
    bs_put_se(bs, 0);               /* slice_qp_delta -> SliceQpY = 26 */
    bs_align_one_then_zero(bs);     /* byte_alignment() */
}

static void write_pcm_ctu(n265_encoder *e, cabac_t *cb, cabac_ctx_t *part_mode,
                          int ctb_x, int ctb_y)
{
    bitstream_t *bs = &e->rbsp;
    /* CTB == min CB: no split_cu_flag. I slice: pred mode inferred intra.
     * CB at min size: part_mode is coded; bin 1 = PART_2Nx2N. */
    n265_cabac_bin(cb, part_mode, 1);
    /* pcm_flag == 1: terminate bin, engine flush, then alignment zeros */
    n265_cabac_terminate(cb, 1);
    bs_align_zero(bs);              /* pcm_alignment_zero_bit */

    int x0 = ctb_x * N265_CTB_SIZE;
    int y0 = ctb_y * N265_CTB_SIZE;
    const uint8_t *l = e->pad_plane[0] + y0 * e->pad_stride[0] + x0;
    for (int y = 0; y < N265_CTB_SIZE; y++)
        for (int x = 0; x < N265_CTB_SIZE; x++)
            bs_put_bits(bs, l[y * e->pad_stride[0] + x], 8);
    for (int p = 1; p <= 2; p++) {
        const uint8_t *c = e->pad_plane[p] +
            (y0 / 2) * e->pad_stride[p] + x0 / 2;
        for (int y = 0; y < N265_CTB_SIZE / 2; y++)
            for (int x = 0; x < N265_CTB_SIZE / 2; x++)
                bs_put_bits(bs, c[y * e->pad_stride[p] + x], 8);
    }
    /* arithmetic engine restarts after PCM; contexts persist (9.3.1) */
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

int n265_encoder_encode(n265_encoder *e, n265_nal **nal, int *nnal,
                        const n265_picture *pic)
{
    if (!e)
        return -1;
    *nal = e->nal;
    *nnal = 0;
    if (!pic)
        return 0;   /* zero-latency: nothing buffered to flush */

    size_t out_used = 0;
    int count = 0;

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

    bs_reset(&e->rbsp);
    write_slice_header(e);

    cabac_t cb;
    cabac_ctx_t part_mode;
    n265_cabac_start(&cb, &e->rbsp);
    /* part_mode ctxIdx 0, initType 0 (I slice), initValue 184 (Table 9-13),
     * SliceQpY 26 */
    n265_cabac_ctx_init(&part_mode, 184, 26);

    int last = e->ctb_rows * e->ctb_cols - 1;
    for (int i = 0; i <= last; i++) {
        write_pcm_ctu(e, &cb, &part_mode, i % e->ctb_cols, i / e->ctb_cols);
        /* end_of_slice_segment_flag */
        n265_cabac_terminate(&cb, i == last);
    }
    bs_align_zero(&e->rbsp);        /* trailing zeros after the flush's 1 */
    add_nal(e, N265_NAL_IDR_W_RADL, &out_used, &count);

    e->frames_done++;
    *nnal = count;
    return (int)out_used;
}
