/*
 * Interpolation filters (ITU-T H.265 Table 8-11/8-12): 8-tap luma at
 * quarter-pel, 4-tap chroma at eighth-pel. Uni-prediction rounding:
 * 14-bit intermediate, (v + 32) >> 6 at the end.
 * Copyright (c) 2026, the next265 authors. GPL-2.0-or-later; see LICENSE.
 */

#include "encoder/mc.h"

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

void n265_mc_luma(uint8_t *dst, int n, const uint8_t *ref, int ref_stride,
                  int fx, int fy)
{
    if (!fx && !fy) {
        for (int y = 0; y < n; y++)
            for (int x = 0; x < n; x++)
                dst[y * n + x] = ref[y * ref_stride + x];
        return;
    }
    if (!fy) {          /* horizontal only: (sum + 32) >> 6 */
        const int8_t *f = luma_filt[fx];
        for (int y = 0; y < n; y++)
            for (int x = 0; x < n; x++) {
                int s = 0;
                for (int k = 0; k < 8; k++)
                    s += f[k] * ref[y * ref_stride + x + k - 3];
                dst[y * n + x] = clip1((s + 32) >> 6);
            }
        return;
    }
    if (!fx) {          /* vertical only */
        const int8_t *f = luma_filt[fy];
        for (int y = 0; y < n; y++)
            for (int x = 0; x < n; x++) {
                int s = 0;
                for (int k = 0; k < 8; k++)
                    s += f[k] * ref[(y + k - 3) * ref_stride + x];
                dst[y * n + x] = clip1((s + 32) >> 6);
            }
        return;
    }
    /* 2-D: horizontal to 16-bit temp rows (3 above, 4 below), then
     * vertical with >> 12 */
    int16_t tmp[(64 + 7) * 64];
    const int8_t *fh = luma_filt[fx], *fv = luma_filt[fy];
    for (int y = -3; y < n + 4; y++)
        for (int x = 0; x < n; x++) {
            int s = 0;
            for (int k = 0; k < 8; k++)
                s += fh[k] * ref[y * ref_stride + x + k - 3];
            tmp[(y + 3) * n + x] = (int16_t)s;
        }
    for (int y = 0; y < n; y++)
        for (int x = 0; x < n; x++) {
            int s = 0;
            for (int k = 0; k < 8; k++)
                s += fv[k] * tmp[(y + k) * n + x];
            dst[y * n + x] = clip1((s + 2048) >> 12);
        }
}

void n265_mc_chroma(uint8_t *dst, int n, const uint8_t *ref, int ref_stride,
                    int fx, int fy)
{
    if (!fx && !fy) {
        for (int y = 0; y < n; y++)
            for (int x = 0; x < n; x++)
                dst[y * n + x] = ref[y * ref_stride + x];
        return;
    }
    if (!fy) {
        const int8_t *f = chroma_filt[fx];
        for (int y = 0; y < n; y++)
            for (int x = 0; x < n; x++) {
                int s = 0;
                for (int k = 0; k < 4; k++)
                    s += f[k] * ref[y * ref_stride + x + k - 1];
                dst[y * n + x] = clip1((s + 32) >> 6);
            }
        return;
    }
    if (!fx) {
        const int8_t *f = chroma_filt[fy];
        for (int y = 0; y < n; y++)
            for (int x = 0; x < n; x++) {
                int s = 0;
                for (int k = 0; k < 4; k++)
                    s += f[k] * ref[(y + k - 1) * ref_stride + x];
                dst[y * n + x] = clip1((s + 32) >> 6);
            }
        return;
    }
    int16_t tmp[(32 + 3) * 32];
    const int8_t *fh = chroma_filt[fx], *fv = chroma_filt[fy];
    for (int y = -1; y < n + 2; y++)
        for (int x = 0; x < n; x++) {
            int s = 0;
            for (int k = 0; k < 4; k++)
                s += fh[k] * ref[y * ref_stride + x + k - 1];
            tmp[(y + 1) * n + x] = (int16_t)s;
        }
    for (int y = 0; y < n; y++)
        for (int x = 0; x < n; x++) {
            int s = 0;
            for (int k = 0; k < 4; k++)
                s += fv[k] * tmp[(y + k) * n + x];
            dst[y * n + x] = clip1((s + 2048) >> 12);
        }
}
