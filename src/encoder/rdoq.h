/*
 * Rate-distortion optimised quantisation of one transform block.
 * Copyright (c) 2026, the yah265 authors. GPL-2.0-or-later; see LICENSE.
 */
#ifndef N265_RDOQ_H
#define N265_RDOQ_H
#include <stdint.h>
#include "encoder/cabac.h"

void n265_rdoq_init(void);

/* N265_SBH_RD: RDOQ decides the sign-bit-hiding parity fix itself (see
 * rdoq.c), so the caller must not run n265_sbh_adjust after it. */
int n265_rdoq_sbh_on(void);

/* Quantise coef (raster, n*n) into level with the RD choice per coefficient
 * (see rdoq.c). ctx is the slice context array the block will be coded
 * against (read, not adapted); lam_q8 the mode-decision lambda (Q8) in
 * pixel-SSD units. Returns the number of nonzero levels. */
int n265_rdoq(int16_t *level, const int16_t *coef, int log2n, int qp,
              int intra, int c_idx, int scan_idx, const cabac_ctx_t *ctx,
              uint32_t lam_q8);
#endif
