/*
 * Transforms, quantization, reconstruction helpers.
 * Copyright (c) 2026, the next265 authors. GPL-2.0-or-later; see LICENSE.
 */

#ifndef N265_TRANSFORM_H
#define N265_TRANSFORM_H

#include <stdint.h>

/* Forward DCT of an NxN residual block (encoder-side, standard two-stage
 * shifts for 8-bit; the scale matches what quantization below expects). */
void n265_fdct(int16_t *coef, const int16_t *resid, int log2n, int use_dst);

/* Normative inverse path (8.6.3 dequant, 8.6.4 inverse transform); this is
 * the half that must match the decoder bit-exactly. */
void n265_dequant(int16_t *dq, const int16_t *level, int log2n, int qp);
void n265_idct_add(uint8_t *dst, int dst_stride, const int16_t *dq,
                   int log2n, const uint8_t *pred, int pred_stride,
                   int use_dst);

/* Encoder-side scalar quant; returns number of nonzero levels. */
int n265_quant(int16_t *level, const int16_t *coef, int log2n, int qp);

/* Chroma QP mapping for 4:2:0 (Table 8-10). */
int n265_chroma_qp(int qp_y);

#endif
