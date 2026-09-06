/*
 * Psychovisual distortion term for mode decision.
 * Copyright (c) 2026, the yah265 authors. GPL-2.0-or-later; see LICENSE.
 */
#ifndef N265_PSY_H
#define N265_PSY_H
#include <stdint.h>
/* sum over 4x4 blocks of |Hadamard AC coefficients| of an n x n block */
uint32_t n265_psy_energy(const uint8_t *p, int stride, int n);
/* the cost to add to a candidate's distortion: |E_src - E_rec| scaled by
 * lambda and the strength; 0 when the strength is 0 */
uint64_t n265_psy_cost(const uint8_t *src, int src_stride, const uint8_t *rec,
                       int rec_stride, int n, uint32_t lam_q8, double strength);
#endif
