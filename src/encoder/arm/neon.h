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

/* The E1 cached-first-stage luma forms. These two take `hf` alone rather
 * than the entry point's full argument list: the reference plane, its
 * stride and fx are consumed by the fall-through ABOVE the dispatch (a
 * NULL cache or fx == 0 is the plain form's business), so below it the
 * kernel is a function of the cached int16 rows and fy. Precondition:
 * hf != NULL, and the block is covered from 3 rows above to 4 below. */
void n265_mc_luma_hf_neon(uint8_t *dst, int dst_stride, int w, int h,
                          const int16_t *hf, int hf_stride, int fy);
void n265_mc_luma_14_hf_neon(int16_t *dst, int dst_stride, int w, int h,
                             const int16_t *hf, int hf_stride, int fy);

/* the plain forms, and the whole-plane first stage they share their
 * horizontal pass with */
void n265_mc_luma_wh_neon(uint8_t *dst, int dst_stride, int w, int h,
                          const uint8_t *ref, int ref_stride, int fx, int fy);
void n265_mc_luma_14_wh_neon(int16_t *dst, int dst_stride, int w, int h,
                             const uint8_t *ref, int ref_stride, int fx,
                             int fy);
void n265_mc_luma_hplane_neon(int16_t *dst, int dst_stride, int w, int h,
                              const uint8_t *ref, int ref_stride, int fx);

/* the chroma 4-tap eighth-pel forms. 4:2:0 halves every luma width, so
 * these see w = 2 and w = 6 and h = 2, which no luma shape presents. */
void n265_mc_chroma_wh_neon(uint8_t *dst, int dst_stride, int w, int h,
                            const uint8_t *ref, int ref_stride, int fx,
                            int fy);
void n265_mc_chroma_14_wh_neon(int16_t *dst, int dst_stride, int w, int h,
                               const uint8_t *ref, int ref_stride, int fx,
                               int fy);

/* ---- transform (N265_ASM_DCT) -------------------------------------- */

/* ---- satd (N265_ASM_SATD) ------------------------------------------ */

#endif /* N265_NEON */

#endif /* N265_ARM_NEON_H */
