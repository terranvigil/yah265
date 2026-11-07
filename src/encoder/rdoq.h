/*
 * Rate-distortion optimised quantisation of one transform block.
 * Copyright (c) 2026, the yah265 authors. GPL-2.0-or-later; see LICENSE.
 */
#ifndef N265_RDOQ_H
#define N265_RDOQ_H
#include <stdint.h>
#include "encoder/cabac.h"

void n265_rdoq_init(void);

/* the forward transform's energy scale at log2n (2..5): pixel SSD is
 * coefficient SSD divided by it. Measured once, by n265_rdoq_init. */
double n265_rdoq_parseval(int log2n);

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

/* The two elementwise pieces of the lattice, exported so checkasm can put
 * the NEON forms against these references directly (class N265_ASM_QUANT).
 *
 * n265_rdoq_rtn is the lattice's first pass over a packed n x n block: the
 * round-to-nearest level of every coefficient, which is the upper bound of
 * that coefficient's candidate set; the block's all-zero coefficient
 * energy, which is what the whole-block drop test is priced against; and
 * the occupancy of the 4x4 sub-blocks of the levels it just wrote. Bit s
 * of *sb_nz is set when the sub-block at raster index
 * s = (y >> 2) * (n >> 2) + (x >> 2) holds a nonzero level. Returns
 * nonzero when any level is nonzero. `czero` is the deadzone threshold the
 * C uses to skip the multiply below it; it is an optimisation over an
 * expression that is already correct there, so a kernel may ignore it
 * (rdoq_neon.c carries the proof).
 *
 * n265_rdoq_sb_nz is that same occupancy over an arbitrary packed block,
 * which is what the sign-bit-hiding pass needs over the FINAL levels.
 *
 * Preconditions for both: log2n in [2, 5], the block packed at stride n,
 * and q / add / qbits as rdoq.c derives them from a qp in [0, 51]. */
int n265_rdoq_rtn(int16_t *rtn, double *all_zero, uint64_t *sb_nz,
                  const int16_t *coef, int log2n, int q, int64_t add,
                  int qbits, int czero);
int n265_rdoq_rtn_ref(int16_t *rtn, double *all_zero, uint64_t *sb_nz,
                      const int16_t *coef, int log2n, int q, int64_t add,
                      int qbits, int czero);
uint64_t n265_rdoq_sb_nz(const int16_t *blk, int log2n);
uint64_t n265_rdoq_sb_nz_ref(const int16_t *blk, int log2n);
#endif
