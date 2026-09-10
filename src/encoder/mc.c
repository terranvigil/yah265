/*
 * Interpolation filters (ITU-T H.265 Table 8-11/8-12): 8-tap luma at
 * quarter-pel, 4-tap chroma at eighth-pel. Uni-prediction rounding:
 * 14-bit intermediate, (v + 32) >> 6 at the end.
 * Copyright (c) 2026, the yah265 authors. GPL-2.0-or-later; see LICENSE.
 */

#include "encoder/mc.h"
#include "encoder/refchk.h"

static const int8_t luma_filt[4][8] = {
    {  0, 0,   0, 64,  0,   0, 0,  0 },
    { -1, 4, -10, 58, 17,  -5, 1,  0 },
    { -1, 4, -11, 40, 40, -11, 4, -1 },
    {  0, 1,  -5, 17, 58, -10, 4, -1 },
};
static const int8_t chroma_filt[8][4] = {
    {  0, 64,  0,  0 },
    { -2, 58, 10, -2 },
    { -4, 54, 16, -2 },
    { -6, 46, 28, -4 },
    { -4, 36, 36, -4 },
    { -4, 28, 46, -6 },
    { -2, 16, 54, -4 },
    { -2, 10, 58, -2 },
};

static inline uint8_t clip1(int v)
{
    return (uint8_t)(v < 0 ? 0 : v > 255 ? 255 : v);
}

void n265_mc_luma_wh(uint8_t *dst, int ds, int w, int h,
                     const uint8_t *ref, int ref_stride, int fx, int fy)
{
    /* the extreme lines this call reads, reported from inside the filter
     * rather than from the caller's bound (refchk.h) */
    N265_RC_TOUCH(ref + (fy ? -3 : 0) * ref_stride);
    N265_RC_TOUCH(ref + (fy ? h + 3 : h - 1) * ref_stride);
    if (!fx && !fy) {
        for (int y = 0; y < h; y++)
            for (int x = 0; x < w; x++)
                dst[y * ds + x] = ref[y * ref_stride + x];
        return;
    }
    if (!fy) {          /* horizontal only: (sum + 32) >> 6 */
        const int8_t *f = luma_filt[fx];
        for (int y = 0; y < h; y++)
            for (int x = 0; x < w; x++) {
                int s = 0;
                for (int k = 0; k < 8; k++)
                    s += f[k] * ref[y * ref_stride + x + k - 3];
                dst[y * ds + x] = clip1((s + 32) >> 6);
            }
        return;
    }
    if (!fx) {          /* vertical only */
        const int8_t *f = luma_filt[fy];
        for (int y = 0; y < h; y++)
            for (int x = 0; x < w; x++) {
                int s = 0;
                for (int k = 0; k < 8; k++)
                    s += f[k] * ref[(y + k - 3) * ref_stride + x];
                dst[y * ds + x] = clip1((s + 32) >> 6);
            }
        return;
    }
    /* 2-D: horizontal to 16-bit temp rows (3 above, 4 below), then
     * vertical with >> 12 */
    int16_t tmp[(64 + 7) * 64];
    const int8_t *fh = luma_filt[fx], *fv = luma_filt[fy];
    for (int y = -3; y < h + 4; y++)
        for (int x = 0; x < w; x++) {
            int s = 0;
            for (int k = 0; k < 8; k++)
                s += fh[k] * ref[y * ref_stride + x + k - 3];
            tmp[(y + 3) * w + x] = (int16_t)s;
        }
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            int s = 0;
            for (int k = 0; k < 8; k++)
                s += fv[k] * tmp[(y + k) * w + x];
            dst[y * ds + x] = clip1((s + 2048) >> 12);
        }
}

void n265_mc_luma_hplane(int16_t *dst, int ds, int w, int h,
                         const uint8_t *ref, int ref_stride, int fx)
{
    N265_RC_TOUCH(ref);
    N265_RC_TOUCH(ref + (h - 1) * ref_stride);
    const int8_t *f = luma_filt[fx];
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            int s = 0;
            for (int k = 0; k < 8; k++)
                s += f[k] * ref[y * ref_stride + x + k - 3];
            dst[y * ds + x] = (int16_t)s;
        }
}

/* The cached-first-stage forms. The plain path's horizontal-only branch
 * emits clip1((s + 32) >> 6) of exactly the value cached here, and its
 * 2-D branch runs the vertical filter over exactly these int16 rows, so
 * both arms below reproduce it bit for bit. */
void n265_mc_luma_hf(uint8_t *dst, int ds, int w, int h,
                     const uint8_t *ref, int ref_stride,
                     const int16_t *hf, int hs, int fx, int fy)
{
    if (!fx || !hf) {
        n265_mc_luma_wh(dst, ds, w, h, ref, ref_stride, fx, fy);
        return;
    }
    N265_RC_TOUCH16(hf + (fy ? -3 : 0) * hs);
    N265_RC_TOUCH16(hf + (fy ? h + 3 : h - 1) * hs);
    if (!fy) {
        for (int y = 0; y < h; y++)
            for (int x = 0; x < w; x++)
                dst[y * ds + x] = clip1((hf[y * hs + x] + 32) >> 6);
        return;
    }
    const int8_t *fv = luma_filt[fy];
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            int s = 0;
            for (int k = 0; k < 8; k++)
                s += fv[k] * hf[(y + k - 3) * hs + x];
            dst[y * ds + x] = clip1((s + 2048) >> 12);
        }
}

void n265_mc_luma_14_hf(int16_t *dst, int ds, int w, int h,
                        const uint8_t *ref, int ref_stride,
                        const int16_t *hf, int hs, int fx, int fy)
{
    if (!fx || !hf) {
        n265_mc_luma_14_wh(dst, ds, w, h, ref, ref_stride, fx, fy);
        return;
    }
    N265_RC_TOUCH16(hf + (fy ? -3 : 0) * hs);
    N265_RC_TOUCH16(hf + (fy ? h + 3 : h - 1) * hs);
    if (!fy) {
        for (int y = 0; y < h; y++)
            for (int x = 0; x < w; x++)
                dst[y * ds + x] = hf[y * hs + x];
        return;
    }
    const int8_t *fv = luma_filt[fy];
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            int s = 0;
            for (int k = 0; k < 8; k++)
                s += fv[k] * hf[(y + k - 3) * hs + x];
            dst[y * ds + x] = (int16_t)(s >> 6);
        }
}

void n265_mc_chroma_wh(uint8_t *dst, int ds, int w, int h,
                       const uint8_t *ref, int ref_stride, int fx, int fy)
{
    N265_RC_TOUCH(ref + (fy ? -1 : 0) * ref_stride);
    N265_RC_TOUCH(ref + (fy ? h + 1 : h - 1) * ref_stride);
    if (!fx && !fy) {
        for (int y = 0; y < h; y++)
            for (int x = 0; x < w; x++)
                dst[y * ds + x] = ref[y * ref_stride + x];
        return;
    }
    if (!fy) {
        const int8_t *f = chroma_filt[fx];
        for (int y = 0; y < h; y++)
            for (int x = 0; x < w; x++) {
                int s = 0;
                for (int k = 0; k < 4; k++)
                    s += f[k] * ref[y * ref_stride + x + k - 1];
                dst[y * ds + x] = clip1((s + 32) >> 6);
            }
        return;
    }
    if (!fx) {
        const int8_t *f = chroma_filt[fy];
        for (int y = 0; y < h; y++)
            for (int x = 0; x < w; x++) {
                int s = 0;
                for (int k = 0; k < 4; k++)
                    s += f[k] * ref[(y + k - 1) * ref_stride + x];
                dst[y * ds + x] = clip1((s + 32) >> 6);
            }
        return;
    }
    int16_t tmp[(32 + 3) * 32];
    const int8_t *fh = chroma_filt[fx], *fv = chroma_filt[fy];
    for (int y = -1; y < h + 2; y++)
        for (int x = 0; x < w; x++) {
            int s = 0;
            for (int k = 0; k < 4; k++)
                s += fh[k] * ref[y * ref_stride + x + k - 1];
            tmp[(y + 1) * w + x] = (int16_t)s;
        }
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            int s = 0;
            for (int k = 0; k < 4; k++)
                s += fv[k] * tmp[(y + k) * w + x];
            dst[y * ds + x] = clip1((s + 2048) >> 12);
        }
}

/* 14-bit outputs: 1-D keeps sum<<0 (values are sum of coef*sample, i.e.
 * sample<<6 scale); full-pel is sample<<6; 2-D second stage >>6. */
void n265_mc_luma_14_wh(int16_t *dst, int ds, int w, int h,
                        const uint8_t *ref, int ref_stride, int fx, int fy)
{
    N265_RC_TOUCH(ref + (fy ? -3 : 0) * ref_stride);
    N265_RC_TOUCH(ref + (fy ? h + 3 : h - 1) * ref_stride);
    if (!fx && !fy) {
        for (int y = 0; y < h; y++)
            for (int x = 0; x < w; x++)
                dst[y * ds + x] = (int16_t)(ref[y * ref_stride + x] << 6);
        return;
    }
    if (!fy) {
        const int8_t *f = luma_filt[fx];
        for (int y = 0; y < h; y++)
            for (int x = 0; x < w; x++) {
                int s = 0;
                for (int k = 0; k < 8; k++)
                    s += f[k] * ref[y * ref_stride + x + k - 3];
                dst[y * ds + x] = (int16_t)s;
            }
        return;
    }
    if (!fx) {
        const int8_t *f = luma_filt[fy];
        for (int y = 0; y < h; y++)
            for (int x = 0; x < w; x++) {
                int s = 0;
                for (int k = 0; k < 8; k++)
                    s += f[k] * ref[(y + k - 3) * ref_stride + x];
                dst[y * ds + x] = (int16_t)s;
            }
        return;
    }
    int16_t tmp[(64 + 7) * 64];
    const int8_t *fh = luma_filt[fx], *fv = luma_filt[fy];
    for (int y = -3; y < h + 4; y++)
        for (int x = 0; x < w; x++) {
            int s = 0;
            for (int k = 0; k < 8; k++)
                s += fh[k] * ref[y * ref_stride + x + k - 3];
            tmp[(y + 3) * w + x] = (int16_t)s;
        }
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            int s = 0;
            for (int k = 0; k < 8; k++)
                s += fv[k] * tmp[(y + k) * w + x];
            dst[y * ds + x] = (int16_t)(s >> 6);
        }
}

void n265_mc_chroma_14_wh(int16_t *dst, int ds, int w, int h,
                          const uint8_t *ref, int ref_stride, int fx, int fy)
{
    N265_RC_TOUCH(ref + (fy ? -1 : 0) * ref_stride);
    N265_RC_TOUCH(ref + (fy ? h + 1 : h - 1) * ref_stride);
    if (!fx && !fy) {
        for (int y = 0; y < h; y++)
            for (int x = 0; x < w; x++)
                dst[y * ds + x] = (int16_t)(ref[y * ref_stride + x] << 6);
        return;
    }
    if (!fy) {
        const int8_t *f = chroma_filt[fx];
        for (int y = 0; y < h; y++)
            for (int x = 0; x < w; x++) {
                int s = 0;
                for (int k = 0; k < 4; k++)
                    s += f[k] * ref[y * ref_stride + x + k - 1];
                dst[y * ds + x] = (int16_t)s;
            }
        return;
    }
    if (!fx) {
        const int8_t *f = chroma_filt[fy];
        for (int y = 0; y < h; y++)
            for (int x = 0; x < w; x++) {
                int s = 0;
                for (int k = 0; k < 4; k++)
                    s += f[k] * ref[(y + k - 1) * ref_stride + x];
                dst[y * ds + x] = (int16_t)s;
            }
        return;
    }
    int16_t tmp[(32 + 3) * 32];
    const int8_t *fh = chroma_filt[fx], *fv = chroma_filt[fy];
    for (int y = -1; y < h + 2; y++)
        for (int x = 0; x < w; x++) {
            int s = 0;
            for (int k = 0; k < 4; k++)
                s += fh[k] * ref[y * ref_stride + x + k - 1];
            tmp[(y + 1) * w + x] = (int16_t)s;
        }
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            int s = 0;
            for (int k = 0; k < 4; k++)
                s += fv[k] * tmp[(y + k) * w + x];
            dst[y * ds + x] = (int16_t)(s >> 6);
        }
}

void n265_mc_luma(uint8_t *dst, int n, const uint8_t *ref, int ref_stride,
                  int fx, int fy)
{
    n265_mc_luma_wh(dst, n, n, n, ref, ref_stride, fx, fy);
}

void n265_mc_chroma(uint8_t *dst, int n, const uint8_t *ref, int ref_stride,
                    int fx, int fy)
{
    n265_mc_chroma_wh(dst, n, n, n, ref, ref_stride, fx, fy);
}

void n265_mc_luma_14(int16_t *dst, int n, const uint8_t *ref, int ref_stride,
                     int fx, int fy)
{
    n265_mc_luma_14_wh(dst, n, n, n, ref, ref_stride, fx, fy);
}

void n265_mc_chroma_14(int16_t *dst, int n, const uint8_t *ref,
                       int ref_stride, int fx, int fy)
{
    n265_mc_chroma_14_wh(dst, n, n, n, ref, ref_stride, fx, fy);
}

void n265_bi_avg_wh(uint8_t *dst, int ds, const int16_t *a, const int16_t *b,
                    int w, int h)
{
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            int v = (a[y * w + x] + b[y * w + x] + 64) >> 7;
            dst[y * ds + x] = (uint8_t)(v < 0 ? 0 : v > 255 ? 255 : v);
        }
}

void n265_bi_avg(uint8_t *dst, const int16_t *a, const int16_t *b, int n)
{
    n265_bi_avg_wh(dst, n, a, b, n, n);
}
