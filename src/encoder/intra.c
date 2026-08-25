/*
 * Intra prediction: reference gathering with substitution, DC mode.
 * Copyright (c) 2026, the next265 authors. GPL-2.0-or-later; see LICENSE.
 */

#include "encoder/intra.h"

/* z-scan order key at 4x4 granularity: CTB raster index in the high bits,
 * interleaved in-CTB coordinates below. pic_w is the padded width the CTB
 * grid spans. */
static uint64_t zorder(int x, int y, int pic_w, int ctb_log2)
{
    int ctb_cols = (pic_w + (1 << ctb_log2) - 1) >> ctb_log2;
    int cx = x >> ctb_log2, cy = y >> ctb_log2;
    uint32_t rx = (uint32_t)(x & ((1 << ctb_log2) - 1)) >> 2;
    uint32_t ry = (uint32_t)(y & ((1 << ctb_log2) - 1)) >> 2;
    uint32_t z = 0;
    for (int b = 0; b < ctb_log2 - 2; b++)
        z |= ((rx >> b) & 1u) << (2 * b) | ((ry >> b) & 1u) << (2 * b + 1);
    return ((uint64_t)(cy * ctb_cols + cx) << (2 * (ctb_log2 - 2))) | z;
}

void n265_intra_refs(uint8_t *refs, const uint8_t *recon, int stride,
                     int x0, int y0, int log2n,
                     int pic_w, int pic_h, int ctb_log2)
{
    int n = 1 << log2n;
    int total = 4 * n + 1;
    uint64_t cur = zorder(x0, y0, pic_w, ctb_log2);
    /* gather with availability; -1 marks unavailable */
    int val[4 * 32 + 1];

    for (int i = 0; i < total; i++) {
        int x, y;
        if (i == 0) {
            x = x0 - 1; y = y0 - 1;
        } else if (i <= 2 * n) {
            x = x0 + (i - 1); y = y0 - 1;
        } else {
            x = x0 - 1; y = y0 + (i - 2 * n - 1);
        }
        int ok = x >= 0 && y >= 0 && x < pic_w && y < pic_h &&
                 zorder(x, y, pic_w, ctb_log2) < cur;
        val[i] = ok ? recon[y * stride + x] : -1;
    }

    /* 8.4.4.2.2 substitution: search from the bottom-left end of the left
     * column (our index 4n) toward the corner and then along the top row;
     * if everything is unavailable, mid-grey. */
    int first = -1;
    for (int i = total - 1; i >= 0 && first < 0; i--)
        if (val[i] >= 0)
            first = i;
    if (first < 0) {
        for (int i = 0; i < total; i++)
            refs[i] = 128;
        return;
    }
    /* spec order runs p[-1][2n-1] up the left col, corner, then top row;
     * in our layout that's index 4n down to 2n+1, 0, then 1..2n. Walk that
     * order propagating the previous value into gaps. */
    int order[4 * 32 + 1];
    int m = 0;
    for (int i = 4 * n; i >= 2 * n + 1; i--)
        order[m++] = i;
    order[m++] = 0;
    for (int i = 1; i <= 2 * n; i++)
        order[m++] = i;

    /* first pass: if the very first in spec order is missing, seed it with
     * the first available sample anywhere (spec: search forward). */
    if (val[order[0]] < 0) {
        for (int k = 1; k < m; k++)
            if (val[order[k]] >= 0) {
                val[order[0]] = val[order[k]];
                break;
            }
    }
    for (int k = 1; k < m; k++)
        if (val[order[k]] < 0)
            val[order[k]] = val[order[k - 1]];

    for (int i = 0; i < total; i++)
        refs[i] = (uint8_t)val[i];
}

void n265_pred_dc(uint8_t *dst, int dst_stride, const uint8_t *refs,
                  int log2n, int filter_edges)
{
    int n = 1 << log2n;
    const uint8_t *top = refs + 1;
    const uint8_t *left = refs + 2 * n + 1;
    int sum = n;
    for (int i = 0; i < n; i++)
        sum += top[i] + left[i];
    int dc = sum >> (log2n + 1);

    for (int y = 0; y < n; y++)
        for (int x = 0; x < n; x++)
            dst[y * dst_stride + x] = (uint8_t)dc;

    if (filter_edges && n < 32) {
        dst[0] = (uint8_t)((left[0] + 2 * dc + top[0] + 2) >> 2);
        for (int x = 1; x < n; x++)
            dst[x] = (uint8_t)((top[x] + 3 * dc + 2) >> 2);
        for (int y = 1; y < n; y++)
            dst[y * dst_stride] = (uint8_t)((left[y] + 3 * dc + 2) >> 2);
    }
}
