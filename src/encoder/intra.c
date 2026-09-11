/*
 * Intra prediction: reference gathering with substitution, DC mode.
 * Copyright (c) 2026, the yah265 authors. GPL-2.0-or-later; see LICENSE.
 */

#include <string.h>

#include "common/cpu.h"
#include "encoder/intra.h"
#include "encoder/arm/neon.h"   /* self-guarded */

static inline int abs_int(int v) { return v < 0 ? -v : v; }

uint64_t n265_zorder(int x, int y, int pic_w, int ctb_log2)
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

void n265_intra_refs_ref(uint8_t *refs, const uint8_t *recon, int stride,
                         int x0, int y0, int log2n,
                         int pic_w, int pic_h, int ctb_log2)
{
    int n = 1 << log2n;
    int total = 4 * n + 1;
    uint64_t cur = n265_zorder(x0, y0, pic_w, ctb_log2);
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
                 n265_zorder(x, y, pic_w, ctb_log2) < cur;
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

void n265_intra_refs(uint8_t *refs, const uint8_t *recon, int stride,
                     int x0, int y0, int log2n,
                     int pic_w, int pic_h, int ctb_log2)
{
#if N265_NEON
    if (n265_asm_on(N265_ASM_INTRA)) {
        n265_intra_refs_neon(refs, recon, stride, x0, y0, log2n,
                             pic_w, pic_h, ctb_log2);
        return;
    }
#endif
    n265_intra_refs_ref(refs, recon, stride, x0, y0, log2n,
                        pic_w, pic_h, ctb_log2);
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

void n265_filter_refs_ref(uint8_t *out, const uint8_t *in, int log2n)
{
    int n = 1 << log2n;
    /* strong smoothing (8.4.4.2.3, enabled in the SPS): for 32x32 luma
     * with near-linear boundaries, the references are rebuilt by
     * bilinear interpolation of the three corners */
    if (n == 32) {
        int tl = in[0], tr = in[2 * n], bl = in[4 * n];
        int thr = 1 << (8 - 5);
        int flat_top = abs_int(tl + tr - 2 * in[n]) < thr;
        int flat_left = abs_int(tl + bl - 2 * in[2 * n + n]) < thr;
        if (flat_top && flat_left) {
            out[0] = (uint8_t)tl;
            for (int i = 1; i <= 2 * n; i++)
                out[i] = (uint8_t)(((2 * n - i) * tl + i * tr + n) /
                                   (2 * n));
            for (int i = 1; i <= 2 * n; i++)
                out[2 * n + i] = (uint8_t)(((2 * n - i) * tl + i * bl + n) /
                                           (2 * n));
            return;
        }
    }
    /* spec filters along the contiguous boundary left-bottom -> corner ->
     * top-right; our layout keeps corner at 0, top at 1..2n, left at
     * 2n+1..4n. Neighbors along the boundary of index i in the top row are
     * i-1 and i+1 with the corner before index 1; in the left column,
     * lower index = closer to the corner. */
    out[0] = (uint8_t)((in[2 * n + 1] + 2 * in[0] + in[1] + 2) >> 2);
    for (int i = 1; i < 2 * n; i++)
        out[i] = (uint8_t)((in[i - 1] + 2 * in[i] + in[i + 1] + 2) >> 2);
    out[2 * n] = in[2 * n];
    out[2 * n + 1] = (uint8_t)((in[0] + 2 * in[2 * n + 1] +
                                in[2 * n + 2] + 2) >> 2);
    for (int i = 2 * n + 2; i < 4 * n; i++)
        out[i] = (uint8_t)((in[i - 1] + 2 * in[i] + in[i + 1] + 2) >> 2);
    out[4 * n] = in[4 * n];
}

void n265_filter_refs(uint8_t *out, const uint8_t *in, int log2n)
{
#if N265_NEON
    if (n265_asm_on(N265_ASM_INTRA)) {
        n265_filter_refs_neon(out, in, log2n);
        return;
    }
#endif
    n265_filter_refs_ref(out, in, log2n);
}

int n265_use_filtered_refs(int mode, int log2n, int c_idx)
{
    if (c_idx || mode == INTRA_DC || log2n == 2)
        return 0;
    int n = 1 << log2n;
    int dist = mode == INTRA_PLANAR
        ? 10    /* planar passes the distance test at every size >= 8 */
        : (abs_int(mode - 26) < abs_int(mode - 10) ? abs_int(mode - 26)
                                                   : abs_int(mode - 10));
    int thres = n == 8 ? 7 : n == 16 ? 1 : 0;
    return dist > thres;
}

static void pred_planar(uint8_t *dst, int stride, const uint8_t *refs,
                        int log2n)
{
    int n = 1 << log2n;
    const uint8_t *top = refs + 1;
    const uint8_t *left = refs + 2 * n + 1;
    int tr = top[n];        /* p[nTbS][-1] */
    int bl = left[n];       /* p[-1][nTbS] */
    for (int y = 0; y < n; y++)
        for (int x = 0; x < n; x++)
            dst[y * stride + x] = (uint8_t)(((n - 1 - x) * left[y] +
                (x + 1) * tr + (n - 1 - y) * top[x] + (y + 1) * bl + n) >>
                (log2n + 1));
}

/* Table 8-4/8-5 */
static const int8_t intra_angle[35] = {
    0, 0, 32, 26, 21, 17, 13, 9, 5, 2, 0, -2, -5, -9, -13, -17, -21, -26,
    -32, -26, -21, -17, -13, -9, -5, -2, 0, 2, 5, 9, 13, 17, 21, 26, 32
};
static const int16_t inv_angle[35] = {
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, -4096, -1638, -910, -630, -482, -390,
    -315, -256, -315, -390, -482, -630, -910, -1638, -4096, 0, 0, 0, 0, 0,
    0, 0, 0, 0
};

static void pred_angular(uint8_t *dst, int stride, const uint8_t *refs,
                         int log2n, int mode, int c_idx)
{
    int n = 1 << log2n;
    const uint8_t *top = refs + 1;
    const uint8_t *left = refs + 2 * n + 1;
    int corner = refs[0];
    int angle = intra_angle[mode];
    uint8_t ref[4 * 32 + 2];    /* projected main reference */
    uint8_t *r = ref + n + 1;   /* r[i], i in -n-1..2n */
    memset(ref, 0, sizeof(ref));

    if (mode >= 18) {           /* main = top */
        r[-1] = (uint8_t)corner;
        for (int i = 0; i < 2 * n; i++)
            r[i] = top[i];
        if (angle < 0) {
            /* spec ref[x] = p[-1][-1+((x*invAngle+128)>>8)]; our r[i] is
             * spec ref[i+1]. The projection is needed down to r[last],
             * which is the smallest index the row loop below reads
             * (idx is smallest at y = n-1); the spec's own range runs one
             * further, and that one entry is dead. Writing it anyway cost
             * a read of left[15] at 4x4 in modes 11 and 25, eight bytes
             * past the 4n+1 references the caller declares - harmless in
             * the encoder, where the buffer is always the full 129 bytes
             * and the value is discarded, but a genuine overread and one
             * the checkasm page guard fires on. Output is unchanged
             * either way: the entry is never read. */
            int last = (n * angle) >> 5;
            for (int i = -2; i >= last; i--)
                r[i] = left[(((i + 1) * inv_angle[mode] + 128) >> 8) - 1];
        }
        for (int y = 0; y < n; y++) {
            int idx = ((y + 1) * angle) >> 5;
            int fact = ((y + 1) * angle) & 31;
            for (int x = 0; x < n; x++)
                dst[y * stride + x] = (uint8_t)(((32 - fact) *
                    r[x + idx] + fact * r[x + idx + 1] + 16) >> 5);
        }
        if (mode == 26 && c_idx == 0 && n < 32)
            for (int y = 0; y < n; y++) {
                int v = top[0] + ((left[y] - corner) >> 1);
                dst[y * stride] = (uint8_t)(v < 0 ? 0 : v > 255 ? 255 : v);
            }
    } else {                    /* main = left, transposed walk */
        r[-1] = (uint8_t)corner;
        for (int i = 0; i < 2 * n; i++)
            r[i] = left[i];
        if (angle < 0) {
            int last = (n * angle) >> 5;
            for (int i = -2; i >= last; i--)
                r[i] = top[(((i + 1) * inv_angle[mode] + 128) >> 8) - 1];
        }
        for (int x = 0; x < n; x++) {
            int idx = ((x + 1) * angle) >> 5;
            int fact = ((x + 1) * angle) & 31;
            for (int y = 0; y < n; y++)
                dst[y * stride + x] = (uint8_t)(((32 - fact) *
                    r[y + idx] + fact * r[y + idx + 1] + 16) >> 5);
        }
        if (mode == 10 && c_idx == 0 && n < 32)
            for (int x = 0; x < n; x++) {
                int v = left[0] + ((top[x] - corner) >> 1);
                dst[x] = (uint8_t)(v < 0 ? 0 : v > 255 ? 255 : v);
            }
    }
}

void n265_intra_pred_ref(uint8_t *dst, int dst_stride, const uint8_t *refs,
                         int log2n, int mode, int c_idx)
{
    if (mode == INTRA_PLANAR)
        pred_planar(dst, dst_stride, refs, log2n);
    else if (mode == INTRA_DC)
        n265_pred_dc(dst, dst_stride, refs, log2n, c_idx == 0);
    else
        pred_angular(dst, dst_stride, refs, log2n, mode, c_idx);
}

void n265_intra_pred(uint8_t *dst, int dst_stride, const uint8_t *refs,
                     int log2n, int mode, int c_idx)
{
#if N265_NEON
    /* Everything except one shape. An angular mode of 18 or above walks
     * the block in rows, which is what clang already auto-vectorises the
     * C reference into, and at 8x8 the kernel's projected-reference setup
     * is not amortised over the 64 samples: 0.92-0.97x, against 1.7-1.9x
     * at 4x4 (the whole block is one q-register there), parity at 16x16
     * and 1.10x at 32x32. Below 18 the C walks COLUMNS - a scattered byte
     * store per sample - and the kernel writes rows into a scratch block
     * and transposes it in 8x8 tiles, which is 2.1x at 8x8 rising to
     * 5.8x at 32x32. Both paths are bit-exact, so the predicate moves
     * time and nothing else. */
    if (!(mode >= 18 && log2n == 3) && n265_asm_on(N265_ASM_INTRA)) {
        n265_intra_pred_neon(dst, dst_stride, refs, log2n, mode, c_idx);
        return;
    }
#endif
    n265_intra_pred_ref(dst, dst_stride, refs, log2n, mode, c_idx);
}
