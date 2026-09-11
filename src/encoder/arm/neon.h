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

/* The transform's other half. Same file and same family, but its own
 * class bit (N265_ASM_QUANT), because the campaign prices dequant
 * separately from the butterflies. Same five square shapes, packed. */

void n265_dequant_neon(int16_t *dq, const int16_t *level, int log2n, int qp,
                       int *nzw, int *nzh);
int n265_quant_neon(int16_t *level, const int16_t *coef, int log2n, int qp,
                    int intra);

/* ---- satd (N265_ASM_SATD) ------------------------------------------ */

/* The two Hadamard tiles, exported so checkasm can compare each against
 * its own loop-nest reference (n265_satd4_ref / n265_satd8_ref) and not
 * only in aggregate, plus n265_satd_wh's whole w x h loop so that a call
 * dispatches once instead of once per tile. The loop form reproduces the
 * N265_SATD8 mixed tiling as well as the default one. */
uint32_t n265_satd4_neon(const uint8_t *a, int as, const uint8_t *b, int bs);
uint32_t n265_satd8_neon(const uint8_t *a, int as, const uint8_t *b, int bs);
uint32_t n265_satd_wh_neon(const uint8_t *a, int as, const uint8_t *b,
                           int bs, int w, int h);

/* The two plain block reductions the encoder's call sites were factored
 * onto (src/encoder/satd.c): |a - b| summed, and (a - b)^2 summed. */
uint32_t n265_sad_wh_neon(const uint8_t *a, int as, const uint8_t *b,
                          int bs, int w, int h);
uint64_t n265_ssd_wh_neon(const uint8_t *a, int as, const uint8_t *b,
                          int bs, int w, int h);

#if defined(__ARM_FEATURE_DOTPROD)
/* The FEAT_DotProd arm of n265_ssd_wh_neon, which selects it at runtime
 * on N265_CPU_DOTPROD (udot of a difference with itself is the sum of
 * four squares). Exported so checkasm can check and time BOTH arms on a
 * host that has the extension; not a dispatch site's entry point. */
uint64_t n265_ssd_wh_dot_neon(const uint8_t *a, int as, const uint8_t *b,
                              int bs, int w, int h);
#endif

/* ---- intra (N265_ASM_INTRA) ---------------------------------------- */

/* Shapes taken from the encoder (2026-09-10 survey, both entry points
 * instrumented on foreman_cif 30 frames and park_joy_720p 10 frames at
 * both presets): log2n 2..5 and nothing else - the encoder never predicts
 * a 64x64 intra TB, because a TB is capped at 32 - and chroma reaches
 * only log2n 2..4. What the survey settles is the MODE mix: the 35-mode
 * ranking loop in ctu.c runs every mode on every candidate TB, so the
 * modes come out very nearly uniform (planar 3.5-4.1%, DC 3.1-4.0%, the
 * 33 angular modes the rest, and 39% of all calls are a NEGATIVE angle
 * and so pay for the projected reference). The angular kernel is
 * therefore the family, and the planar/DC arms are worth having only
 * because they share the entry point.
 *
 * n265_intra_refs_neon has no vector width to speak of; see the head of
 * intra_neon.c for what it does instead and why it is here. */

void n265_intra_pred_neon(uint8_t *dst, int dst_stride, const uint8_t *refs,
                          int log2n, int mode, int c_idx);
void n265_intra_refs_neon(uint8_t *refs, const uint8_t *recon, int stride,
                          int x0, int y0, int log2n,
                          int pic_w, int pic_h, int ctb_log2);
void n265_filter_refs_neon(uint8_t *out, const uint8_t *in, int log2n);

#endif /* N265_NEON */

#endif /* N265_ARM_NEON_H */
