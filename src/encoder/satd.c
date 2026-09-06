/*
 * Hadamard SATD.
 * Copyright (c) 2026, the yah265 authors. GPL-2.0-or-later; see LICENSE.
 */

#include "encoder/satd.h"

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

static uint32_t satd8(const uint8_t *a, int as, const uint8_t *b, int bs)
{
    int d[64], t;
    for (int y = 0; y < 8; y++)
        for (int x = 0; x < 8; x++)
            d[y * 8 + x] = a[y * as + x] - b[y * bs + x];
    /* rows */
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
    /* columns */
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

uint32_t n265_satd(const uint8_t *a, int a_stride, const uint8_t *b,
                   int b_stride, int n)
{
    if (n == 4)
        return satd4(a, a_stride, b, b_stride);
    uint32_t sum = 0;
    for (int y = 0; y < n; y += 8)
        for (int x = 0; x < n; x += 8)
            sum += satd8(a + y * a_stride + x, a_stride,
                         b + y * b_stride + x, b_stride);
    return sum;
}
