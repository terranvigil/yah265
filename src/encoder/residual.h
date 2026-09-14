/*
 * residual_coding() syntax writer.
 * Copyright (c) 2026, the yah265 authors. GPL-2.0-or-later; see LICENSE.
 */

#ifndef N265_RESIDUAL_H
#define N265_RESIDUAL_H

#include <stddef.h>
#include <stdint.h>

#include "encoder/cabac.h"
#include "encoder/est.h"

/* Encode one transform block's levels (raster order, n*n int16) per
 * 7.3.8.11. Caller guarantees at least one nonzero level (cbf gates the
 * call). scan_idx: 0 diag, 1 horizontal, 2 vertical. ctx points at the
 * slice context array (tables.h layout).
 *
 * `sbh` is sign_data_hiding_enabled_flag && !cu_transquant_bypass_flag
 * (7.4.9.11): the writer's half of the condition is the per-sub-block
 * scan-distance test below, and the per-stream and per-CU halves are the
 * caller's. Under --lossless it is 0 everywhere. */
void n265_residual_coding(const res_ops *ops, const int16_t *level,
                          int log2n, int c_idx, int scan_idx, int sbh);

/* ---- the coefficient-costing kernels (class N265_ASM_QUANT) --------
 *
 * residual_coding()'s two elementwise passes over the levels, split out
 * so the NEON family has an entry point and checkasm has a reference.
 * Neither touches the arithmetic coder: the bin-by-bin walk between them
 * is serial by construction and stays where it is. */

/* Per-4x4-group significance of an n x n block, in RASTER sub-block
 * order: out[sby * (n >> 2) + sbx] = 1 iff that group holds a nonzero.
 * Scan-independent - all three scans permute the same sixteen positions -
 * so it needs no scan table. `out` is (n >> 2)^2 bytes. */
void n265_csbf_map(const int16_t *level, int log2n, uint8_t *out);
void n265_csbf_map_ref(const int16_t *level, int log2n, uint8_t *out);

/* One 4x4 group at `grp` (its top-left level, rows `stride` apart)
 * gathered into `scan_idx` scan order. abs_out[i] is |level| at scan
 * position i, UNSIGNED so that the -32768 end reads back as 32768 the way
 * the scalar form's `int` arithmetic did. Returns the significance mask
 * (bit i) and writes the sign mask (bit i set = negative). */
uint32_t n265_coeff_group(const int16_t *grp, ptrdiff_t stride, int scan_idx,
                          uint16_t *abs_out, uint32_t *neg_mask);
uint32_t n265_coeff_group_ref(const int16_t *grp, ptrdiff_t stride,
                              int scan_idx, uint16_t *abs_out,
                              uint32_t *neg_mask);

/* The sig_coeff_flag ctxInc PATTERN over a 4x4 group's sixteen scan
 * positions (9.3.4.2.5's position term alone; the group's own offset is
 * added once per group). [scan_idx][prev_csbf], plus a fifth row for the
 * 4x4 TB's own ctxIdxMap. Built by n265_residual_tables_init(); exported
 * so checkasm can prove it against the literal spec derivation. */
extern uint8_t n265_sig_pat[3][5][16];
void n265_residual_tables_init(void);

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
void n265_sbh_adjust_ref(int16_t *level, int log2n, int scan_idx);

#endif
