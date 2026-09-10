/*
 * Hadamard SATD.
 * Copyright (c) 2026, the yah265 authors. GPL-2.0-or-later; see LICENSE.
 */

#include <stdlib.h>

#include "encoder/satd.h"
#include "encoder/preset.h"

/* The 4x4 tile is unchanged. It was already the 8-add butterfly with the
 * abs-sum folded, and writing it out any other way measured 0.85x on the
 * checkasm bench, so the only thing replaced in this file is the 8x8
 * tile. n265_satd4_ref below is this same function under the name the
 * checkasm reference dispatcher uses. */
static uint32_t satd4(const uint8_t *a, int as, const uint8_t *b, int bs)
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

uint32_t n265_satd4_ref(const uint8_t *a, int as, const uint8_t *b, int bs)
{
    return satd4(a, as, b, bs);
}

/* The loop-nest 8x8 Hadamard this file shipped before the butterfly
 * below replaced it: the reference checkasm checks against. */
uint32_t n265_satd8_ref(const uint8_t *a, int as, const uint8_t *b, int bs)
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

static uint32_t satd8(const uint8_t *a, int as, const uint8_t *b, int bs)
{
    int d[64];
    for (int y = 0; y < 8; y++) {
        const uint8_t *p = a + y * as, *q = b + y * bs;
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
static int satd8_mixed(void)
{
    static int v = -1;
    if (v < 0) { const char *s = n265_env("N265_SATD8"); v = s ? atoi(s) : 0; }
    return v;
}

void n265_satd_warm(void)
{
    (void)satd8_mixed();
}

uint32_t n265_satd_wh(const uint8_t *a, int a_stride, const uint8_t *b,
                      int b_stride, int w, int h)
{
    uint32_t sum = 0;
    if ((w & 7) || (h & 7)) {
        int w8 = satd8_mixed() ? (w & ~7) : 0;
        int h8 = satd8_mixed() ? (h & ~7) : 0;
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

uint32_t n265_satd(const uint8_t *a, int a_stride, const uint8_t *b,
                   int b_stride, int n)
{
    return n265_satd_wh(a, a_stride, b, b_stride, n, n);
}
