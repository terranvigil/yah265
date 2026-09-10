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

/* the C path with the dispatch stripped off, so checkasm's bench can time
 * C against NEON in one process; not a call site's entry point */
uint32_t n265_satd_wh_c(const uint8_t *a, int a_stride, const uint8_t *b,
                        int b_stride, int w, int h);

/* square form: n a power of two >= 4 */
uint32_t n265_satd(const uint8_t *a, int a_stride, const uint8_t *b,
                   int b_stride, int n);

/* cache the N265_SATD8 arm on the driver before any worker exists */
void n265_satd_warm(void);

/* The N265_SATD8 arm itself. Not a knob a caller sets: it is the tiling
 * n265_satd_wh chose, and the NEON form of that loop has to make the same
 * choice, because the arm changes the ranking metric and a NEON path that
 * silently took the default tiling would be a different encoder under
 * N265_SATD8=1. Cached by n265_satd_warm on the driver thread. */
int n265_satd8_mixed(void);

/* The loop-nest forms of the two tiles, kept as the C reference the
 * unrolled butterflies are checked against (tests/checkasm). Not a path
 * the encoder takes. */
uint32_t n265_satd4_ref(const uint8_t *a, int as, const uint8_t *b, int bs);
uint32_t n265_satd8_ref(const uint8_t *a, int as, const uint8_t *b, int bs);

#endif
