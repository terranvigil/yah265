/*
 * neon.h - every NEON kernel entry point in one place.
 *
 * The dispatch convention (docs/instruments.md, "NEON dispatch (phase 6)"):
 *
 *   - A NEON kernel is an exported function `n265_<kernel>_neon(...)` with
 *     the SAME signature as its C entry point `n265_<kernel>(...)`.
 *   - It is declared here, under `#if N265_NEON`, and defined in exactly one
 *     of src/encoder/arm/{transform,satd,mc}_neon.c. One file per family so
 *     the family worktrees never touch the same translation unit, and
 *     meson.build never conflicts at merge.
 *   - The C entry point keeps its body and dispatches at the TOP, after any
 *     N265_RC_TOUCH lines (those stay in the C entry point: the
 *     N265_REF_CHECK proof of docs/threading-design.md G2 reads them):
 *
 *         #if N265_NEON
 *             if (n265_asm_on(N265_ASM_MC))
 *                 return n265_foo_neon(...);
 *         #endif
 *
 *     One acquire load and a branch per call, which is the cost yah264
 *     measured as acceptable once the pthread_once walk moved off the fast
 *     path.
 *
 *     A shape predicate may join the class test where the kernel has no
 *     vector path for that shape (`w >= 4 && n265_asm_on(...)`): both
 *     paths are bit-exact, so it moves time and nothing else. Take the
 *     shape set from the ENCODER, not from the luma PU table - 4:2:0
 *     chroma halves every luma width, so 2 and 6 are real.
 *   - Every NEON kernel is BIT-EXACT with its C reference. Not "close", not
 *     "within rounding": the determinism gate, the cross-thread identity set
 *     and the N265_ASM_OFF ablation measurement all rest on it, and it is
 *     what lets a kernel item skip the quality band entirely.
 *
 * Copyright (c) 2026, the yah265 authors. GPL-2.0-or-later; see LICENSE.
 */

#ifndef N265_ARM_NEON_H
#define N265_ARM_NEON_H

#if N265_NEON

#include <stdint.h>

/* ---- mc (N265_ASM_MC) ---------------------------------------------- */

void n265_bi_avg_wh_neon(uint8_t *dst, int dst_stride, const int16_t *a,
                         const int16_t *b, int w, int h);

/* ---- transform (N265_ASM_DCT) -------------------------------------- */

/* Shapes taken from the encoder, not from the spec tables (2026-09-10
 * survey: foreman_cif 30 frames and park_joy_720p --preset fast 10 frames,
 * with both entry points instrumented). 4x4, 4x4 DST, 8x8, 16x16 and
 * 32x32, and nothing else - a transform block is square by construction
 * and the DST only ever appears at 4x4 intra luma. Both strides were n in
 * every one of the 2.7M calls surveyed, so the kernels take a general
 * stride but are tuned for the packed block. What does vary is the idct's
 * nzw/nzh bound: on CIF 61% of calls have nzh < n and 20% are DC only, so
 * the bounded form is the common case and not the exception. */

void n265_fdct_neon(int16_t *coef, const int16_t *resid, int log2n,
                    int use_dst);
void n265_idct_add_neon(uint8_t *dst, int dst_stride, const int16_t *dq,
                        int log2n, const uint8_t *pred, int pred_stride,
                        int use_dst, int nzw, int nzh);

/* ---- satd (N265_ASM_SATD) ------------------------------------------ */

#endif /* N265_NEON */

#endif /* N265_ARM_NEON_H */
