/*
 * Motion-compensated interpolation (8.4.4.3.2/8.4.4.3.3).
 * Copyright (c) 2026, the yah265 authors. GPL-2.0-or-later; see LICENSE.
 */

#ifndef N265_MC_H
#define N265_MC_H

#include <stdint.h>

/* dst (n x n, stride n) from the integer-position ref pointer plus a
 * fractional phase: luma fx/fy in quarter-pel (0..3), chroma in
 * eighth-pel (0..7). The ref must be readable 3 above/left and 4
 * below/right of the block (padded planes guarantee it). */
void n265_mc_luma(uint8_t *dst, int n, const uint8_t *ref, int ref_stride,
                  int fx, int fy);
void n265_mc_chroma(uint8_t *dst, int n, const uint8_t *ref, int ref_stride,
                    int fx, int fy);

/* 14-bit intermediate variants for bi-prediction (no final rounding) */
void n265_mc_luma_14(int16_t *dst, int n, const uint8_t *ref, int ref_stride,
                     int fx, int fy);
void n265_mc_chroma_14(int16_t *dst, int n, const uint8_t *ref,
                       int ref_stride, int fx, int fy);

/* default bi combine: (a + b + 64) >> 7, clipped */
void n265_bi_avg(uint8_t *dst, const int16_t *a, const int16_t *b, int n);

#endif
