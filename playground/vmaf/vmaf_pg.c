/*
 * VMAF v1 for the playground, scored the way the project's results are:
 * the v1 3d0h model, both sides at 10 bits, CAMBI told the coded size and
 * that the encode is 8-bit, and a picture smaller than 1920x1080 in both
 * dimensions scaled up (bicubic, aspect kept) to fit 1920x1080 first.
 * The page hands in 8-bit I420 pictures at the coded size.
 *
 * Copyright (c) 2026, the yah265 authors. GPL-2.0-or-later; see LICENSE.
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <emscripten/emscripten.h>

#include "libvmaf/libvmaf.h"
#include "libvmaf/model.h"
#include "libvmaf/feature.h"
#include "libvmaf/picture.h"

typedef struct Taps {
    int *idx;      /* 4 source indices per output sample */
    float *wt;     /* 4 weights */
} Taps;

static VmafContext *ctx;
static VmafModel *model;
static unsigned char *in_ref, *in_dis;
static float *tmp;
static int sw, sh, dw, dh, nread;
static Taps hx[2], vy[2];

/* bicubic with B = 0, C = 0.6, ffmpeg's default for flags=bicubic */
static float cubic(float x)
{
    const float c = 0.6f;
    x = fabsf(x);
    if (x < 1.0f)
        return (2.0f - c) * x * x * x + (c - 3.0f) * x * x + 1.0f;
    if (x < 2.0f)
        return -c * x * x * x + 5.0f * c * x * x - 8.0f * c * x + 4.0f * c;
    return 0.0f;
}

static int taps(Taps *t, int n_in, int n_out)
{
    t->idx = malloc(sizeof(int) * 4 * (size_t)n_out);
    t->wt = malloc(sizeof(float) * 4 * (size_t)n_out);
    if (!t->idx || !t->wt)
        return -1;
    double s = (double)n_in / n_out;
    for (int o = 0; o < n_out; o++) {
        double p = (o + 0.5) * s - 0.5;
        int i0 = (int)floor(p) - 1;
        float sum = 0.0f;
        for (int k = 0; k < 4; k++) {
            int i = i0 + k;
            float w = cubic((float)(p - i));
            t->idx[o * 4 + k] = i < 0 ? 0 : i >= n_in ? n_in - 1 : i;
            t->wt[o * 4 + k] = w;
            sum += w;
        }
        for (int k = 0; k < 4; k++)
            t->wt[o * 4 + k] /= sum;
    }
    return 0;
}

static void free_taps(Taps *t)
{
    free(t->idx);
    free(t->wt);
    t->idx = NULL;
    t->wt = NULL;
}

/* one 8-bit plane, w x h, into a 10-bit plane, ow x oh */
static void scale_plane(const unsigned char *src, int w, int h, uint16_t *dst,
                        ptrdiff_t dstride, int ow, int oh, const Taps *tx,
                        const Taps *ty)
{
    if (ow == w && oh == h) {
        for (int y = 0; y < h; y++)
            for (int x = 0; x < w; x++)
                dst[y * dstride + x] = (uint16_t)(src[y * w + x] << 2);
        return;
    }
    for (int y = 0; y < h; y++) {
        const unsigned char *r = src + (size_t)y * w;
        float *t = tmp + (size_t)y * ow;
        for (int x = 0; x < ow; x++) {
            const int *i = tx->idx + x * 4;
            const float *k = tx->wt + x * 4;
            t[x] = r[i[0]] * k[0] + r[i[1]] * k[1] + r[i[2]] * k[2] + r[i[3]] * k[3];
        }
    }
    for (int y = 0; y < oh; y++) {
        const int *i = ty->idx + y * 4;
        const float *k = ty->wt + y * 4;
        const float *a = tmp + (size_t)i[0] * ow, *b = tmp + (size_t)i[1] * ow;
        const float *c = tmp + (size_t)i[2] * ow, *d = tmp + (size_t)i[3] * ow;
        uint16_t *o = dst + y * dstride;
        for (int x = 0; x < ow; x++) {
            float v = (a[x] * k[0] + b[x] * k[1] + c[x] * k[2] + d[x] * k[3]) * 4.0f;
            int q = (int)lrintf(v);
            o[x] = (uint16_t)(q < 0 ? 0 : q > 1023 ? 1023 : q);
        }
    }
}

static int to_pic(const unsigned char *in, VmafPicture *pic)
{
    if (vmaf_picture_alloc(pic, VMAF_PIX_FMT_YUV420P, 10, (unsigned)dw, (unsigned)dh))
        return -1;
    const unsigned char *p[3] = { in, in + sw * sh, in + sw * sh + (sw / 2) * (sh / 2) };
    for (int c = 0; c < 3; c++) {
        int w = c ? sw / 2 : sw, h = c ? sh / 2 : sh;
        int ow = c ? dw / 2 : dw, oh = c ? dh / 2 : dh;
        scale_plane(p[c], w, h, pic->data[c], pic->stride[c] / 2, ow, oh,
                    &hx[c > 0], &vy[c > 0]);
    }
    return 0;
}

static void vw_free(void)
{
    if (ctx)
        vmaf_close(ctx);
    if (model)
        vmaf_model_destroy(model);
    ctx = NULL;
    model = NULL;
    free(in_ref);
    free(in_dis);
    free(tmp);
    in_ref = in_dis = NULL;
    tmp = NULL;
    for (int i = 0; i < 2; i++) {
        free_taps(&hx[i]);
        free_taps(&vy[i]);
    }
}

/* w x h is the coded size; ow x oh the size it is scored at */
EMSCRIPTEN_KEEPALIVE int vw_open(int w, int h, int ow, int oh,
                                 const char *model_path)
{
    vw_free();
    sw = w;
    sh = h;
    dw = ow;
    dh = oh;
    nread = 0;
    VmafConfiguration cfg = { .log_level = VMAF_LOG_LEVEL_NONE, .n_threads = 0,
                              .n_subsample = 1, .cpumask = 0 };
    if (vmaf_init(&ctx, cfg))
        return -1;
    VmafModelConfig mc = { .name = "v1", .flags = VMAF_MODEL_FLAGS_DEFAULT };
    if (vmaf_model_load_from_path(&model, &mc, model_path))
        return -2;
    VmafFeatureDictionary *d = NULL;
    char bw[16], bh[16];
    snprintf(bw, sizeof bw, "%d", w);
    snprintf(bh, sizeof bh, "%d", h);
    if (vmaf_feature_dictionary_set(&d, "enc_bitdepth", "8") ||
        vmaf_feature_dictionary_set(&d, "enc_width", bw) ||
        vmaf_feature_dictionary_set(&d, "enc_height", bh) ||
        vmaf_model_feature_overload(model, "cambi", d))
        return -3;
    if (vmaf_use_features_from_model(ctx, model))
        return -4;
    in_ref = malloc((size_t)w * h * 3 / 2);
    in_dis = malloc((size_t)w * h * 3 / 2);
    tmp = malloc(sizeof(float) * (size_t)h * ow);
    if (!in_ref || !in_dis || !tmp ||
        taps(&hx[0], w, ow) || taps(&vy[0], h, oh) ||
        taps(&hx[1], w / 2, ow / 2) || taps(&vy[1], h / 2, oh / 2))
        return -5;
    return 0;
}

EMSCRIPTEN_KEEPALIVE unsigned char *vw_ref(void) { return in_ref; }
EMSCRIPTEN_KEEPALIVE unsigned char *vw_dis(void) { return in_dis; }

/* scores the pair now in vw_ref / vw_dis */
EMSCRIPTEN_KEEPALIVE int vw_push(void)
{
    VmafPicture r, d;
    if (to_pic(in_ref, &r))
        return -1;
    if (to_pic(in_dis, &d)) {
        vmaf_picture_unref(&r);
        return -1;
    }
    if (vmaf_read_pictures(ctx, &r, &d, (unsigned)nread))
        return -2;
    nread++;
    return 0;
}

/* the mean over every frame read, or a negative number */
EMSCRIPTEN_KEEPALIVE double vw_score(void)
{
    double s = -1.0;
    if (!ctx || !nread)
        return -1.0;
    if (vmaf_read_pictures(ctx, NULL, NULL, 0))
        return -2.0;
    if (vmaf_score_pooled(ctx, model, VMAF_POOL_METHOD_MEAN, &s, 0,
                          (unsigned)nread - 1))
        return -3.0;
    return s;
}

EMSCRIPTEN_KEEPALIVE void vw_close(void)
{
    vw_free();
}
