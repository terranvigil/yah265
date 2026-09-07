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

#endif
