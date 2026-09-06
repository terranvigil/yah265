/*
 * residual_coding() syntax writer.
 * Copyright (c) 2026, the yah265 authors. GPL-2.0-or-later; see LICENSE.
 */

#ifndef N265_RESIDUAL_H
#define N265_RESIDUAL_H

#include <stdint.h>

#include "encoder/cabac.h"
#include "encoder/est.h"

/* Encode one transform block's levels (raster order, n*n int16) per
 * 7.3.8.11. Caller guarantees at least one nonzero level (cbf gates the
 * call). scan_idx: 0 diag, 1 horizontal, 2 vertical. ctx points at the
 * slice context array (tables.h layout). */
void n265_residual_coding(const res_ops *ops, const int16_t *level,
                          int log2n, int c_idx, int scan_idx);

/* real-CABAC emitter backend */
typedef struct real_emit {
    cabac_t *cb;
    cabac_ctx_t *ctx;
} real_emit;
void n265_real_ops(res_ops *ops, real_emit *re);

/* Sign-bit hiding: enforce, per 4x4 sub-block that qualifies
 * (last - first sig scan pos > 3), that the abs-level sum parity encodes
 * the sign of the first significant coefficient. Adjusts levels in place
 * (grows one magnitude by 1 when parity disagrees). Run after quant and
 * before reconstruction. */
void n265_sbh_adjust(int16_t *level, int log2n, int scan_idx);

#endif
