/*
 * Hadamard SATD costs.
 * Copyright (c) 2026, the yah265 authors. GPL-2.0-or-later; see LICENSE.
 */

#ifndef N265_SATD_H
#define N265_SATD_H

#include <stdint.h>

/* SATD of (a - b) over a w x h block (both multiples of 4), computed as
 * 8x8 Hadamard tiles, or 4x4 tiles when either side is not a multiple of
 * 8 (rectangular PUs: 8x4, 4x16, 12x16, ...). */
uint32_t n265_satd_wh(const uint8_t *a, int a_stride, const uint8_t *b,
                      int b_stride, int w, int h);

/* square form: n a power of two >= 4 */
uint32_t n265_satd(const uint8_t *a, int a_stride, const uint8_t *b,
                   int b_stride, int n);

/* cache the N265_SATD8 arm on the driver before any worker exists */
void n265_satd_warm(void);

/* The loop-nest forms of the two tiles, kept as the C reference the
 * unrolled butterflies are checked against (tests/checkasm). Not a path
 * the encoder takes. */
uint32_t n265_satd4_ref(const uint8_t *a, int as, const uint8_t *b, int bs);
uint32_t n265_satd8_ref(const uint8_t *a, int as, const uint8_t *b, int bs);

#endif
