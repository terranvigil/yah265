/*
 * Intra prediction: reference gathering with substitution, DC mode.
 * Copyright (c) 2026, the yah265 authors. GPL-2.0-or-later; see LICENSE.
 */

#include <string.h>

#include "common/cpu.h"
#include "encoder/intra.h"
#include "encoder/arm/neon.h"   /* self-guarded */
#include "encoder/x86/avx2.h"   /* self-guarded */

static inline int abs_int(int v) { return v < 0 ? -v : v; }

/* bits 0..7 of v to the even bit positions 0..14 */
static inline uint32_t spread_bits8(uint32_t v)
{
    v &= 0xffu;
    v = (v | (v << 4)) & 0x0f0fu;
    v = (v | (v << 2)) & 0x3333u;
    v = (v | (v << 1)) & 0x5555u;
    return v;
}

uint64_t n265_zorder(int x, int y, int pic_w, int ctb_log2)
{
    int ctb_cols = (pic_w + (1 << ctb_log2) - 1) >> ctb_log2;
    int cx = x >> ctb_log2, cy = y >> ctb_log2;
    uint32_t rx = (uint32_t)(x & ((1 << ctb_log2) - 1)) >> 2;
    uint32_t ry = (uint32_t)(y & ((1 << ctb_log2) - 1)) >> 2;
    /* rx and ry are under 2^(ctb_log2 - 2), at most 16 (CTB 64), so the
     * interleave is a fixed bit spread rather than a loop over the bits:
     * the same key, without the loop on every availability probe */
    uint32_t z = spread_bits8(rx) | (spread_bits8(ry) << 1);
    return ((uint64_t)(cy * ctb_cols + cx) << (2 * (ctb_log2 - 2))) | z;
}

/* The spec text, literally: every one of the 4n+1 candidates asked about
 * separately, and the substitution done by a scan. n265_intra_refs_ref
 * below computes the same thing far more cheaply, which makes this the
 * oracle and not the implementation - it is what checkasm holds the
 * prefix argument to, and nothing on a hot path calls it. Do not fold the
 * two together: the worth of the second implementation is that it was
 * written from the spec and not from the fast one. */
void n265_intra_refs_spec(pixel *refs, const pixel *recon, int stride,
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
            refs[i] = 1 << (BIT_DEPTH - 1);
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
        refs[i] = (pixel)val[i];
}

/* ---- the reference gather, as the encoder actually runs it ----------
 *
 * The spec form above asks, for each of the 4n+1 candidates, whether it is
 * inside the picture and earlier in decode order, and decode order is
 * n265_zorder(x, y) - a CTB raster index above interleaved in-CTB
 * coordinates. That is up to 129 zorder evaluations per call, each a loop
 * over the CTB's bits, and on the shape survey n265_intra_refs is called
 * 80k times on 30 frames of foreman alone.
 *
 * All but a handful are unnecessary, because zorder is MONOTONE along a
 * scan line. Fix y and increase x: within a 4x4 unit the key does not
 * move; across a unit either cx is unchanged and rx rises by one, which
 * raises the interleaved field (the rx bits sit at the even positions and
 * the ry bits, unchanged, at the odd ones, so comparing two keys with
 * equal ry reduces to comparing rx), or cx rises by one, which raises the
 * raster index above a field that cannot carry. So zorder(x, y) is
 * non-decreasing in x at fixed y, and by the same argument in y at fixed
 * x. Availability is `zorder < cur`, so on the top row (fixed y0-1, x
 * running up from x0) and on the left column (fixed x0-1, y running down
 * from y0) the available samples are a PREFIX, and the length of that
 * prefix is a binary search: six evaluations instead of 2n.
 *
 * The substitution then has a closed form rather than a scan. In the
 * spec's walk order - up the left column from its bottom end, the corner,
 * then along the top row - availability is
 *
 *     [2n - nl unavailable] [left nl-1 .. 0] [corner?] [top 0 .. nt-1]
 *     [2n - nt unavailable]
 *
 * so the leading run takes the first available sample anywhere (the
 * spec's forward search, and the spec form's seeding pass), and the two
 * trailing gaps take the last value written before them. Which is what
 * the code below writes directly.
 *
 * This began life in src/encoder/arm/intra_neon.c under N265_ASM_INTRA,
 * because that is where the convention puts a fast path and where
 * checkasm can hold it to the C. It never was a vector kernel - there is
 * not one intrinsic in it - and leaving it there charged the win to NEON
 * and left every other target paying 129 zorder calls. It is C now, on
 * every build, and not ablatable.
 */
/* the availability key of a PLANE position, mapped into the z-scan's own
 * coordinates first (n265_zgeo). The shifts are 0 at every format but
 * 4:2:2 chroma, where the mapping is the spec's SubWidthC / SubHeightC
 * and the key is a luma one. Monotonicity along a scan line survives the
 * mapping - it is a non-decreasing function of a non-decreasing one - so
 * the prefix searches below are still binary searches. */
static inline uint64_t zkey(int x, int y, const n265_zgeo *z)
{
    return n265_zorder(x << z->shx, y << z->shy, z->pic_w, z->ctb_log2);
}

static int avail_prefix_x(int x0, int y, int lim, uint64_t cur,
                          const n265_zgeo *z)
{
    int lo = 0, hi = lim;

    if (lim <= 0)
        return 0;
    while (lo < hi) {
        int mid = (lo + hi + 1) / 2;
        if (zkey(x0 + mid - 1, y, z) < cur)
            lo = mid;
        else
            hi = mid - 1;
    }
    return lo;
}

static int avail_prefix_y(int x, int y0, int lim, uint64_t cur,
                          const n265_zgeo *z)
{
    int lo = 0, hi = lim;

    if (lim <= 0)
        return 0;
    while (lo < hi) {
        int mid = (lo + hi + 1) / 2;
        if (zkey(x, y0 + mid - 1, z) < cur)
            lo = mid;
        else
            hi = mid - 1;
    }
    return lo;
}

void n265_intra_refs_g(pixel *refs, const pixel *recon, int stride,
                       int x0, int y0, int log2n, int pic_w, int pic_h,
                       const n265_zgeo *z)
{
    int n = 1 << log2n;
    uint64_t cur = zkey(x0, y0, z);
    int nt = 0, nl = 0, cav = 0, corner = 0, seed;

    /* The first n of each run need no search (phase 14 item 39): the TB
     * is an aligned n x n square, and the aligned square directly above
     * it (or to its left) comes earlier in z-scan order - inside one CTB
     * by the quadtree, and across a CTB edge by the raster above it. Only
     * the above-right and below-left halves are searched. 4:2:2's mapped
     * geometry is not a square z-scan, and a block wider than the CTB is
     * not one square of it, so both search the whole run. */
    int known = ((z->shx | z->shy) || log2n > z->ctb_log2) ? 0 : n;

    if (y0 > 0) {
        int lim = pic_w - x0, k;
        if (lim > 2 * n)
            lim = 2 * n;
        k = known < lim ? known : lim;
        nt = k + avail_prefix_x(x0 + k, y0 - 1, lim - k, cur, z);
    }
    if (x0 > 0) {
        int lim = pic_h - y0, k;
        if (lim > 2 * n)
            lim = 2 * n;
        k = known < lim ? known : lim;
        nl = k + avail_prefix_y(x0 - 1, y0 + k, lim - k, cur, z);
        if (y0 > 0 && zkey(x0 - 1, y0 - 1, z) < cur) {
            cav = 1;
            corner = recon[(size_t)(y0 - 1) * stride + x0 - 1];
        }
    }

    if (!nt && !nl && !cav) {           /* nothing available: mid-grey */
        for (int i = 0; i <= 4 * n; i++)
            refs[i] = 1 << (BIT_DEPTH - 1);
        return;
    }

    /* the left column, refs[2n+1 .. 4n], available for the first nl */
    for (int k = 0; k < nl; k++)
        refs[2 * n + 1 + k] = recon[(size_t)(y0 + k) * stride + x0 - 1];
    /* the top row, refs[1 .. 2n], available for the first nt */
    if (nt)
        n265_cpy(refs + 1, recon + (size_t)(y0 - 1) * stride + x0,
                 (size_t)nt * sizeof(pixel));

    /* first available in the spec's walk order */
    seed = nl ? refs[2 * n + nl] : cav ? corner : refs[1];
    /* the walk starts at 4n and runs down, so the unavailable tail of the
     * left column is what the seed fills */
    /* sample fills, not byte fills: memset stopped serving here the
     * moment the pixel could be wider than a byte */
    if (nl < 2 * n)
        for (int k = nl; k < 2 * n; k++)
            refs[2 * n + 1 + k] = (pixel)seed;
    refs[0] = (pixel)(cav ? corner : nl ? refs[2 * n + 1] : seed);
    if (nt < 2 * n) {
        pixel fill = nt ? refs[nt] : refs[0];
        for (int k = nt; k < 2 * n; k++)
            refs[1 + k] = fill;
    }
}

/* the plane's own geometry, which is every caller but 4:2:2 chroma */
void n265_intra_refs_ref(pixel *refs, const pixel *recon, int stride,
                         int x0, int y0, int log2n,
                         int pic_w, int pic_h, int ctb_log2)
{
    n265_zgeo z = { 0, 0, pic_w, ctb_log2 };
    n265_intra_refs_g(refs, recon, stride, x0, y0, log2n, pic_w, pic_h, &z);
}

void n265_intra_refs(pixel *refs, const pixel *recon, int stride,
                     int x0, int y0, int log2n,
                     int pic_w, int pic_h, int ctb_log2)
{
    n265_intra_refs_ref(refs, recon, stride, x0, y0, log2n,
                        pic_w, pic_h, ctb_log2);
}

void n265_pred_dc(pixel *dst, int dst_stride, const pixel *refs,
                  int log2n, int filter_edges)
{
    int n = 1 << log2n;
    const pixel *top = refs + 1;
    const pixel *left = refs + 2 * n + 1;
    int sum = n;
    for (int i = 0; i < n; i++)
        sum += top[i] + left[i];
    int dc = sum >> (log2n + 1);

    for (int y = 0; y < n; y++)
        for (int x = 0; x < n; x++)
            dst[y * dst_stride + x] = (pixel)dc;

    if (filter_edges && n < 32) {
        dst[0] = (pixel)((left[0] + 2 * dc + top[0] + 2) >> 2);
        for (int x = 1; x < n; x++)
            dst[x] = (pixel)((top[x] + 3 * dc + 2) >> 2);
        for (int y = 1; y < n; y++)
            dst[y * dst_stride] = (pixel)((left[y] + 3 * dc + 2) >> 2);
    }
}

void n265_filter_refs_ref(pixel *out, const pixel *in, int log2n, int strong)
{
    int n = 1 << log2n;
    /* strong smoothing (8.4.4.2.3, enabled in the SPS): for 32x32 luma
     * with near-linear boundaries, the references are rebuilt by
     * bilinear interpolation of the three corners. biIntFlag carries
     * `cIdx == 0` at every chroma format, so a 32x32 CHROMA block at
     * 4:4:4 takes the [1 2 1] run below and not this. */
    if (strong && n == 32) {
        int tl = in[0], tr = in[2 * n], bl = in[4 * n];
        int thr = 1 << (BIT_DEPTH - 5);
        int flat_top = abs_int(tl + tr - 2 * in[n]) < thr;
        int flat_left = abs_int(tl + bl - 2 * in[2 * n + n]) < thr;
        if (flat_top && flat_left) {
            out[0] = (pixel)tl;
            for (int i = 1; i <= 2 * n; i++)
                out[i] = (pixel)(((2 * n - i) * tl + i * tr + n) /
                                   (2 * n));
            for (int i = 1; i <= 2 * n; i++)
                out[2 * n + i] = (pixel)(((2 * n - i) * tl + i * bl + n) /
                                           (2 * n));
            return;
        }
    }
    /* spec filters along the contiguous boundary left-bottom -> corner ->
     * top-right; our layout keeps corner at 0, top at 1..2n, left at
     * 2n+1..4n. Neighbors along the boundary of index i in the top row are
     * i-1 and i+1 with the corner before index 1; in the left column,
     * lower index = closer to the corner. */
    out[0] = (pixel)((in[2 * n + 1] + 2 * in[0] + in[1] + 2) >> 2);
    for (int i = 1; i < 2 * n; i++)
        out[i] = (pixel)((in[i - 1] + 2 * in[i] + in[i + 1] + 2) >> 2);
    out[2 * n] = in[2 * n];
    out[2 * n + 1] = (pixel)((in[0] + 2 * in[2 * n + 1] +
                                in[2 * n + 2] + 2) >> 2);
    for (int i = 2 * n + 2; i < 4 * n; i++)
        out[i] = (pixel)((in[i - 1] + 2 * in[i] + in[i + 1] + 2) >> 2);
    out[4 * n] = in[4 * n];
}

void n265_filter_refs(pixel *out, const pixel *in, int log2n, int strong)
{
#if N265_NEON_INTRA
    if (n265_asm_on(N265_ASM_INTRA)) {
        n265_filter_refs_neon(out, in, log2n, strong);
        return;
    }
#endif
#if N265_AVX2_INTRA
    if (n265_avx2_on(N265_ASM_INTRA)) {
        n265_filter_refs_avx2(out, in, log2n, strong);
        return;
    }
#endif
    n265_filter_refs_ref(out, in, log2n, strong);
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

static void pred_planar(pixel *dst, int stride, const pixel *refs,
                        int log2n)
{
    int n = 1 << log2n;
    const pixel *top = refs + 1;
    const pixel *left = refs + 2 * n + 1;
    int tr = top[n];        /* p[nTbS][-1] */
    int bl = left[n];       /* p[-1][nTbS] */
    for (int y = 0; y < n; y++)
        for (int x = 0; x < n; x++)
            dst[y * stride + x] = (pixel)(((n - 1 - x) * left[y] +
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

static void pred_angular(pixel *dst, int stride, const pixel *refs,
                         int log2n, int mode, int c_idx)
{
    int n = 1 << log2n;
    const pixel *top = refs + 1;
    const pixel *left = refs + 2 * n + 1;
    int corner = refs[0];
    int angle = intra_angle[mode];
    pixel ref[4 * 32 + 2];    /* projected main reference */
    pixel *r = ref + n + 1;   /* r[i], i in -n-1..2n */
    memset(ref, 0, sizeof(ref));

    if (mode >= 18) {           /* main = top */
        r[-1] = (pixel)corner;
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
                dst[y * stride + x] = (pixel)(((32 - fact) *
                    r[x + idx] + fact * r[x + idx + 1] + 16) >> 5);
        }
        if (mode == 26 && c_idx == 0 && n < 32)
            for (int y = 0; y < n; y++) {
                int v = top[0] + ((left[y] - corner) >> 1);
                dst[y * stride] = (pixel)(v < 0 ? 0 : v > PIXEL_MAX ? PIXEL_MAX : v);
            }
    } else {                    /* main = left, transposed walk */
        r[-1] = (pixel)corner;
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
                dst[y * stride + x] = (pixel)(((32 - fact) *
                    r[y + idx] + fact * r[y + idx + 1] + 16) >> 5);
        }
        if (mode == 10 && c_idx == 0 && n < 32)
            for (int x = 0; x < n; x++) {
                int v = left[0] + ((top[x] - corner) >> 1);
                dst[x] = (pixel)(v < 0 ? 0 : v > PIXEL_MAX ? PIXEL_MAX : v);
            }
    }
}

void n265_intra_pred_ref(pixel *dst, int dst_stride, const pixel *refs,
                         int log2n, int mode, int c_idx)
{
    if (mode == INTRA_PLANAR)
        pred_planar(dst, dst_stride, refs, log2n);
    else if (mode == INTRA_DC)
        n265_pred_dc(dst, dst_stride, refs, log2n, c_idx == 0);
    else
        pred_angular(dst, dst_stride, refs, log2n, mode, c_idx);
}

void n265_intra_pred(pixel *dst, int dst_stride, const pixel *refs,
                     int log2n, int mode, int c_idx)
{
#if N265_NEON_INTRA
    /* Every shape. 8x8 at an angular mode of 18 or above stayed on C
     * until phase 14 item 39, at 0.92-0.97x of a row walk clang
     * auto-vectorises; the kernel retires 15% fewer instructions there
     * (340 against 400 a call) since its reference copy stopped being a
     * libc call, so it takes the shape too. Both paths are bit-exact, so
     * the predicate moves time and nothing else. */
    if (n265_asm_on(N265_ASM_INTRA)) {
        n265_intra_pred_neon(dst, dst_stride, refs, log2n, mode, c_idx);
        return;
    }
#endif
#if N265_AVX2_INTRA
    if (n265_avx2_on(N265_ASM_INTRA)) {
        n265_intra_pred_avx2(dst, dst_stride, refs, log2n, mode, c_idx);
        return;
    }
#endif
    n265_intra_pred_ref(dst, dst_stride, refs, log2n, mode, c_idx);
}
