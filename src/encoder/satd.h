/*
 * Hadamard SATD costs.
 * Copyright (c) 2026, the yah265 authors. GPL-2.0-or-later; see LICENSE.
 */

#ifndef N265_SATD_H
#define N265_SATD_H

#include <stdint.h>

/* SATD of (a - b) over an n x n block (n a power of two >= 4), computed
 * as 8x8 Hadamard tiles (4x4 tiles for n == 4). */
uint32_t n265_satd(const uint8_t *a, int a_stride, const uint8_t *b,
                   int b_stride, int n);

#endif
