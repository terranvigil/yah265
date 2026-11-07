/*
 * Transforms, quantization, reconstruction helpers.
 * Copyright (c) 2026, the yah265 authors. GPL-2.0-or-later; see LICENSE.
 */

#ifndef N265_TRANSFORM_H
#define N265_TRANSFORM_H

#include <stdint.h>

#include "common/pixel.h"

/* Forward DCT of an NxN residual block (encoder-side, standard two-stage
 * shifts for 8-bit; the scale matches what quantization below expects). */
void n265_fdct(int16_t *coef, const int16_t *resid, int log2n, int use_dst);

/* Normative inverse path (8.6.3 dequant, 8.6.4 inverse transform); this is
 * the half that must match the decoder bit-exactly. */
/* nzw/nzh (optional) report the dequantized block's nonzero bounding box:
 * every coefficient at x >= *nzw or y >= *nzh is zero. Feed them straight
 * to n265_idct_add, which then skips the rows and columns that cannot
 * contribute. Passing the full size is always correct, just slower. */
void n265_dequant(int16_t *dq, const int16_t *level, int log2n, int qp,
                  int *nzw, int *nzh);
void n265_idct_add(pixel *dst, int dst_stride, const int16_t *dq,
                   int log2n, const pixel *pred, int pred_stride,
                   int use_dst, int nzw, int nzh);

/* The literal matrix-multiply forms of the pair above. They are the C
 * reference the partial-butterfly kernels are checked against
 * (tests/checkasm), not a path the encoder takes; the butterflies must
 * reproduce them bit for bit. */
/* Same for the quant pair, which has no partial-butterfly fast form: the
 * entry points above are a dispatch and these are the C they fall back
 * to, so checkasm has something to compare a NEON kernel against that
 * the ablation knob cannot quietly turn into the kernel itself. */
void n265_dequant_ref(int16_t *dq, const int16_t *level, int log2n, int qp,
                      int *nzw, int *nzh);
int n265_quant_ref(int16_t *level, const int16_t *coef, int log2n, int qp,
                   int intra);

void n265_fdct_ref(int16_t *coef, const int16_t *resid, int log2n,
                   int use_dst);
void n265_idct_add_ref(pixel *dst, int dst_stride, const int16_t *dq,
                       int log2n, const pixel *pred, int pred_stride,
                       int use_dst);

/* Encoder-side scalar quant; returns the number of nonzero levels.
 * `intra` picks the rounding: 0 the inter deadzone (1/6 of a step), 1 the
 * intra one (1/3), 2 plain rounding (1/2; N265_CDZ=2 on inter chroma). */
int n265_quant(int16_t *level, const int16_t *coef, int log2n, int qp,
               int intra);

/* Chroma QP mapping (8.6.1): Table 8-10 at ChromaArrayType 1 (4:2:0),
 * Min(qPi, 51) at every other one - so the argument is
 * chroma_format_idc itself and the function reads the spec's condition
 * rather than a predicate that happens to agree with it. The return is
 * QpC in the 8-BIT DOMAIN, not Qp'C: the quantizer adds QpBdOffsetC
 * itself and the chroma deblocking filter (8.7.2.5.5) does not, so
 * returning the offset index would be silently right for one caller and
 * silently wrong for the other. */
int n265_chroma_qp(int qp_y, int chroma_idc);

#endif
