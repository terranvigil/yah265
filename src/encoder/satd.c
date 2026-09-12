/*
 * Hadamard SATD.
 * Copyright (c) 2026, the yah265 authors. GPL-2.0-or-later; see LICENSE.
 */

#include <stdlib.h>

#include "common/cpu.h"
#include "encoder/satd.h"
#include "encoder/preset.h"
#include "encoder/arm/neon.h"   /* self-guarded */

/* The 4x4 tile is unchanged. It was already the 8-add butterfly with the
 * abs-sum folded, and writing it out any other way measured 0.85x on the
 * checkasm bench, so the only thing replaced in this file is the 8x8
 * tile. n265_satd4_ref below is this same function under the name the
 * checkasm reference dispatcher uses. */
static uint32_t satd4(const pixel *a, int as, const pixel *b, int bs)
{
    int d[16], m[16];
    for (int y = 0; y < 4; y++)
        for (int x = 0; x < 4; x++)
            d[y * 4 + x] = a[y * as + x] - b[y * bs + x];
    for (int y = 0; y < 4; y++) {
        int s0 = d[y * 4] + d[y * 4 + 2], s1 = d[y * 4 + 1] + d[y * 4 + 3];
        int t0 = d[y * 4] - d[y * 4 + 2], t1 = d[y * 4 + 1] - d[y * 4 + 3];
        m[y * 4] = s0 + s1; m[y * 4 + 1] = s0 - s1;
        m[y * 4 + 2] = t0 + t1; m[y * 4 + 3] = t0 - t1;
    }
    uint32_t sum = 0;
    for (int x = 0; x < 4; x++) {
        int s0 = m[x] + m[8 + x], s1 = m[4 + x] + m[12 + x];
        int t0 = m[x] - m[8 + x], t1 = m[4 + x] - m[12 + x];
        int e0 = s0 + s1, e1 = s0 - s1, e2 = t0 + t1, e3 = t0 - t1;
        sum += (uint32_t)((e0 < 0 ? -e0 : e0) + (e1 < 0 ? -e1 : e1) +
                          (e2 < 0 ? -e2 : e2) + (e3 < 0 ? -e3 : e3));
    }
    return (sum + 1) >> 1;
}

uint32_t n265_satd4_ref(const pixel *a, int as, const pixel *b, int bs)
{
    return satd4(a, as, b, bs);
}

/* The loop-nest 8x8 Hadamard this file shipped before the butterfly
 * below replaced it: the reference checkasm checks against. */
uint32_t n265_satd8_ref(const pixel *a, int as, const pixel *b, int bs)
{
    int d[64], t;
    for (int y = 0; y < 8; y++)
        for (int x = 0; x < 8; x++)
            d[y * 8 + x] = a[y * as + x] - b[y * bs + x];
    for (int y = 0; y < 8; y++) {
        int *r = d + y * 8;
        for (int step = 1; step < 8; step <<= 1)
            for (int i = 0; i < 8; i += step << 1)
                for (int j = i; j < i + step; j++) {
                    t = r[j];
                    r[j] = t + r[j + step];
                    r[j + step] = t - r[j + step];
                }
    }
    uint32_t sum = 0;
    for (int x = 0; x < 8; x++) {
        int c[8];
        for (int y = 0; y < 8; y++)
            c[y] = d[y * 8 + x];
        for (int step = 1; step < 8; step <<= 1)
            for (int i = 0; i < 8; i += step << 1)
                for (int j = i; j < i + step; j++) {
                    t = c[j];
                    c[j] = t + c[j + step];
                    c[j + step] = t - c[j + step];
                }
        for (int y = 0; y < 8; y++)
            sum += (uint32_t)(c[y] < 0 ? -c[y] : c[y]);
    }
    return (sum + 2) >> 2;
}

#define ABS_(v) ((uint32_t)((v) < 0 ? -(v) : (v)))

/* The 8-point Hadamard as its 24 adds. The shipped form walked the three
 * radix-2 stages with a triple loop nest whose bounds the compiler cannot
 * see through; the butterfly below is the same permutation of the same
 * sums, written out. */
#define HAD8(o, i0, i1, i2, i3, i4, i5, i6, i7) do {                    \
        int b0_ = (i0) + (i1), b1_ = (i0) - (i1);                       \
        int b2_ = (i2) + (i3), b3_ = (i2) - (i3);                       \
        int b4_ = (i4) + (i5), b5_ = (i4) - (i5);                       \
        int b6_ = (i6) + (i7), b7_ = (i6) - (i7);                       \
        int c0_ = b0_ + b2_, c2_ = b0_ - b2_;                           \
        int c1_ = b1_ + b3_, c3_ = b1_ - b3_;                           \
        int c4_ = b4_ + b6_, c6_ = b4_ - b6_;                           \
        int c5_ = b5_ + b7_, c7_ = b5_ - b7_;                           \
        (o)[0] = c0_ + c4_; (o)[4] = c0_ - c4_;                         \
        (o)[1] = c1_ + c5_; (o)[5] = c1_ - c5_;                         \
        (o)[2] = c2_ + c6_; (o)[6] = c2_ - c6_;                         \
        (o)[3] = c3_ + c7_; (o)[7] = c3_ - c7_;                         \
    } while (0)

static uint32_t satd8(const pixel *a, int as, const pixel *b, int bs)
{
    int d[64];
    for (int y = 0; y < 8; y++) {
        const pixel *p = a + y * as, *q = b + y * bs;
        int r[8];
        HAD8(r, p[0] - q[0], p[1] - q[1], p[2] - q[2], p[3] - q[3],
             p[4] - q[4], p[5] - q[5], p[6] - q[6], p[7] - q[7]);
        for (int x = 0; x < 8; x++)
            d[x * 8 + y] = r[x];        /* transposed: columns go contiguous */
    }
    uint32_t sum = 0;
    for (int x = 0; x < 8; x++) {
        const int *c = d + x * 8;
        int r[8];
        HAD8(r, c[0], c[1], c[2], c[3], c[4], c[5], c[6], c[7]);
        for (int y = 0; y < 8; y++)
            sum += ABS_(r[y]);
    }
    return (sum + 2) >> 2;
}

/* N265_SATD8: on a shape whose sides are not both multiples of 8 - the
 * CB8 rectangles and the CB16 asymmetric partitions, the only two the
 * tiling misses - cover the 8-aligned interior with 8x8 tiles anyway and
 * leave 4x4 tiles for the strips. Everything else already ranks on the
 * 8x8 (SA8D-class) tile. This CHANGES the ranking metric, so it is an
 * arm and not an economy: 0 is the shipped tiling. */
int n265_satd8_mixed(void)
{
    static int v = -1;
    if (v < 0) { const char *s = n265_env("N265_SATD8"); v = s ? atoi(s) : 0; }
    return v;
}

void n265_satd_warm(void)
{
    (void)n265_satd8_mixed();
}

/* The C path, exported so checkasm can time it against the NEON without
 * going through the dispatch below - otherwise the bench's middle column
 * would be the NEON a second time and the multiple it prints would be 1.
 * Not a path any encoder call site takes directly. */
uint32_t n265_satd_wh_c(const pixel *a, int a_stride, const pixel *b,
                        int b_stride, int w, int h)
{
    uint32_t sum = 0;
    if ((w & 7) || (h & 7)) {
        int w8 = n265_satd8_mixed() ? (w & ~7) : 0;
        int h8 = n265_satd8_mixed() ? (h & ~7) : 0;
        for (int y = 0; y < h8; y += 8)
            for (int x = 0; x < w8; x += 8)
                sum += satd8(a + y * a_stride + x, a_stride,
                             b + y * b_stride + x, b_stride);
        for (int y = 0; y < h8; y += 4)
            for (int x = w8; x < w; x += 4)
                sum += satd4(a + y * a_stride + x, a_stride,
                             b + y * b_stride + x, b_stride);
        for (int y = h8; y < h; y += 4)
            for (int x = 0; x < w; x += 4)
                sum += satd4(a + y * a_stride + x, a_stride,
                             b + y * b_stride + x, b_stride);
        return sum;
    }
    for (int y = 0; y < h; y += 8)
        for (int x = 0; x < w; x += 8)
            sum += satd8(a + y * a_stride + x, a_stride,
                         b + y * b_stride + x, b_stride);
    return sum;
}

uint32_t n265_satd_wh(const pixel *a, int a_stride, const pixel *b,
                      int b_stride, int w, int h)
{
#if N265_NEON
    /* 4x4 is the one shape that is a single 4x4 tile, and a single tile
     * is the one case where the vector form loses: its dependency chain
     * (load, three butterfly stages, transpose, three more, abs, the
     * pairwise reduction, the move back to a GPR) is longer than the
     * whole scalar butterfly, and it measured 0.68x on checkasm --bench
     * where every other shape measured 1.03x to 3.1x. Both paths are
     * bit-exact, so this predicate moves time and nothing else. */
    if (!(w == 4 && h == 4) && n265_asm_on(N265_ASM_SATD))
        return n265_satd_wh_neon(a, a_stride, b, b_stride, w, h);
#endif
    return n265_satd_wh_c(a, a_stride, b, b_stride, w, h);
}

uint32_t n265_satd(const pixel *a, int a_stride, const pixel *b,
                   int b_stride, int n)
{
    return n265_satd_wh(a, a_stride, b, b_stride, n, n);
}

/* ---- SAD and SSD -----------------------------------------------------
 *
 * Both were open-coded double loops at half a dozen call sites. Nothing
 * about them was per-site: the same |a - b| and the same (a - b)^2 over a
 * w x h block, at whatever pair of strides the caller had. Factoring them
 * here is what lets one NEON kernel cover motion search, the intra
 * ranking, the lookahead and the TB and CU distortion at once, and it is
 * also what gives them a reference checkasm can hold the NEON to.
 *
 * The SSD accumulator is uint64_t at the boundary because the CALLERS sum
 * many blocks into one, but the per-block sum itself is exact in 32 bits:
 * (a - b)^2 <= 65025 and the largest block any caller passes is 64x64, so
 * 4096 * 65025 = 266342400 < 2^32. The C keeps the u64 add per pixel
 * anyway - it is the reference, and it is not on any hot path once the
 * NEON is dispatched.
 */

uint32_t n265_sad_wh_c(const pixel *a, int as, const pixel *b, int bs,
                       int w, int h)
{
    uint32_t sad = 0;
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            int d = a[(size_t)y * as + x] - b[(size_t)y * bs + x];
            sad += (uint32_t)(d < 0 ? -d : d);
        }
    return sad;
}

uint32_t n265_sad_wh(const pixel *a, int as, const pixel *b, int bs,
                     int w, int h)
{
#if N265_NEON
    if (n265_asm_on(N265_ASM_SATD))
        return n265_sad_wh_neon(a, as, b, bs, w, h);
#endif
    return n265_sad_wh_c(a, as, b, bs, w, h);
}

uint64_t n265_ssd_wh_c(const pixel *a, int as, const pixel *b, int bs,
                       int w, int h)
{
    uint64_t ssd = 0;
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            int d = a[(size_t)y * as + x] - b[(size_t)y * bs + x];
            ssd += (uint64_t)(d * d);
        }
    return ssd;
}

uint64_t n265_ssd_wh(const pixel *a, int as, const pixel *b, int bs,
                     int w, int h)
{
#if N265_NEON
    if (n265_asm_on(N265_ASM_SATD))
        return n265_ssd_wh_neon(a, as, b, bs, w, h);
#endif
    return n265_ssd_wh_c(a, as, b, bs, w, h);
}

/* ---- per-row and per-column SAD --------------------------------------
 *
 * ctu.c's rect_proxy ranks the two rectangular splits off the 2Nx2N
 * winner's residual: it wants the n row sums and the n column sums of
 * |src - pred| over the CU, and then the mean absolute deviation of each
 * against their common mean. The reduction is the whole of that pass -
 * the two deviation sums are 2n terms and cost nothing - so this is what
 * the kernel covers, and the caller keeps the deviations.
 *
 * Both outputs are exact in u32 by a wide margin: a column sum is at most
 * n * 255, 16320 at the largest CU the caller has, which is why the NEON
 * can keep its column accumulators in u16 lanes for every n up to 257.
 * The u32 array is the caller's, not the accumulator's.
 *
 * Square rather than w x h: rect_proxy is the only caller and a CU is
 * square. Its shape set is 8, 16 and 32 and cannot be anything else -
 * the rectangular-partition block lives under analyze_cu's log2n <= 5
 * arm, and MIN_CB_LOG2 is 3 - which is what the row[32]/col[32] on the
 * caller's stack has always rested on. An instrumented run over 300
 * frames of foreman and 120 of park_joy at the shipping default agrees:
 * 661019 calls, 68% of them n = 8, and not one n = 4 or n = 64.
 */

void n265_sad_rowcol_ref(const pixel *a, int as, const pixel *b, int bs,
                         int n, uint32_t *row, uint32_t *col)
{
    for (int x = 0; x < n; x++)
        col[x] = 0;
    for (int y = 0; y < n; y++) {
        uint32_t r = 0;
        for (int x = 0; x < n; x++) {
            int d = a[(size_t)y * as + x] - b[(size_t)y * bs + x];
            uint32_t v = (uint32_t)(d < 0 ? -d : d);
            r += v;
            col[x] += v;
        }
        row[y] = r;
    }
}

void n265_sad_rowcol(const pixel *a, int as, const pixel *b, int bs,
                     int n, uint32_t *row, uint32_t *col)
{
#if N265_NEON
    if (n265_asm_on(N265_ASM_SATD)) {
        n265_sad_rowcol_neon(a, as, b, bs, n, row, col);
        return;
    }
#endif
    n265_sad_rowcol_ref(a, as, b, bs, n, row, col);
}
