/*
 * neon.h - every NEON kernel entry point in one place.
 *
 * The dispatch convention (docs/instruments.md, "NEON dispatch (phase 6)"):
 *
 *   - A NEON kernel is an exported function `n265_<kernel>_neon(...)` with
 *     the SAME signature as its C entry point `n265_<kernel>(...)`.
 *   - It is declared here, under `#if N265_NEON_<FAMILY>`, and defined in
 *     exactly one of the src/encoder/arm family files. One file per family so the
 *     family worktrees never touch the same translation unit, and
 *     meson.build never conflicts at merge.
 *   - A FAMILY IS PER DEPTH (phase 8 item 3). meson compiles a family's
 *     file, and defines its N265_NEON_<FAMILY>, only for the depths whose
 *     lanes are proved: `pixel` is uint8_t at 8 bits and uint16_t at
 *     Main10, so a kernel is written for BOTH widths or the family is
 *     absent from the build that has no kernel, and its dispatch sites
 *     compile out with it. That is why every signature below is typed on
 *     `pixel` and never on uint8_t.
 *   - The C entry point keeps its body and dispatches at the TOP, after any
 *     N265_RC_TOUCH lines (those stay in the C entry point: the
 *     N265_REF_CHECK proof of docs/threading-design.md G2 reads them):
 *
 *         #if N265_NEON_MC
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

#include <stddef.h>
#include <stdint.h>

#include "common/cpu.h"     /* N265_NEON_<FAMILY>, normalised to 0/1 */
#include "common/pixel.h"

#if N265_NEON_MC
/* ---- mc (N265_ASM_MC) ---------------------------------------------- */

void n265_bi_avg_wh_neon(pixel *dst, int dst_stride, const int16_t *a,
                         const int16_t *b, int w, int h);

/* 8.5.3.3.4.3's three forms. The shift is a RUNTIME value (the slice's
 * weight denominator plus shift1), which is why none of these can use a
 * narrowing intrinsic with an immediate shift. Same signatures as their
 * entry points. */
void n265_wp_uni_wh_neon(pixel *dst, int ds, const int16_t *src, int w,
                         int h, int wt, int off, int denom);
void n265_wp_bi_wh_neon(pixel *dst, int ds, const int16_t *a,
                        const int16_t *b, int w, int h, int w0, int o0,
                        int w1, int o1, int denom);
void n265_wp_pix_wh_neon(pixel *dst, int ds, const pixel *src, int ss,
                         int w, int h, int wt, int off, int denom);

/* The E1 cached-first-stage luma forms. These two take `hf` alone rather
 * than the entry point's full argument list: the reference plane, its
 * stride and fx are consumed by the fall-through ABOVE the dispatch (a
 * NULL cache or fx == 0 is the plain form's business), so below it the
 * kernel is a function of the cached int16 rows and fy. Precondition:
 * hf != NULL, and the block is covered from 3 rows above to 4 below. */
void n265_mc_luma_hf_neon(pixel *dst, int dst_stride, int w, int h,
                          const int16_t *hf, int hf_stride, int fy);
void n265_mc_luma_14_hf_neon(int16_t *dst, int dst_stride, int w, int h,
                             const int16_t *hf, int hf_stride, int fy);

/* the plain forms, and the whole-plane first stage they share their
 * horizontal pass with */
void n265_mc_luma_wh_neon(pixel *dst, int dst_stride, int w, int h,
                          const pixel *ref, int ref_stride, int fx, int fy);
void n265_mc_luma_14_wh_neon(int16_t *dst, int dst_stride, int w, int h,
                             const pixel *ref, int ref_stride, int fx,
                             int fy);
void n265_mc_luma_hplane_neon(int16_t *dst, int dst_stride, int w, int h,
                              const pixel *ref, int ref_stride, int fx);

/* The chroma 4-tap eighth-pel forms. 4:2:0 halves every luma width, so
 * these see w = 2 and w = 6 and h = 2, which no luma shape presents;
 * 4:4:4 hands them the luma table itself, to 64x64, and 4:2:2 the luma
 * widths halved against the luma heights, to 32x64 (phase 11 item 3 -
 * before it, n265_encoder_open stood the whole class down at both
 * formats). Both ends of that range run the same row drivers: the
 * 16/8/4/2-lane steps and the scalar remainder walk any w and h. What IS
 * sized by the block is the 2-D temp, and it has two sizes rather than
 * one so that a 4:2:0 call does not reserve 4:4:4's frame. */
void n265_mc_chroma_wh_neon(pixel *dst, int dst_stride, int w, int h,
                            const pixel *ref, int ref_stride, int fx,
                            int fy);
void n265_mc_chroma_14_wh_neon(int16_t *dst, int dst_stride, int w, int h,
                               const pixel *ref, int ref_stride, int fx,
                               int fy);

#endif /* N265_NEON_MC */

#if N265_NEON_TRANSFORM
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
void n265_idct_add_neon(pixel *dst, int dst_stride, const int16_t *dq,
                        int log2n, const pixel *pred, int pred_stride,
                        int use_dst, int nzw, int nzh);

/* The transform's other half. Same file and same family, but its own
 * class bit (N265_ASM_QUANT), because the campaign prices dequant
 * separately from the butterflies. Same five square shapes, packed. */

void n265_dequant_neon(int16_t *dq, const int16_t *level, int log2n, int qp,
                       int *nzw, int *nzh);
int n265_quant_neon(int16_t *level, const int16_t *coef, int log2n, int qp,
                    int intra);

#endif /* N265_NEON_TRANSFORM */

#if N265_NEON_SATD
/* ---- satd (N265_ASM_SATD) ------------------------------------------ */

/* The two Hadamard tiles, exported so checkasm can compare each against
 * its own loop-nest reference (n265_satd4_ref / n265_satd8_ref) and not
 * only in aggregate, plus n265_satd_wh's whole w x h loop so that a call
 * dispatches once instead of once per tile. The loop form reproduces the
 * N265_SATD8 mixed tiling as well as the default one. */
uint32_t n265_satd4_neon(const pixel *a, int as, const pixel *b, int bs);
uint32_t n265_satd8_neon(const pixel *a, int as, const pixel *b, int bs);
uint32_t n265_satd_wh_neon(const pixel *a, int as, const pixel *b,
                           int bs, int w, int h);

/* The two plain block reductions the encoder's call sites were factored
 * onto (src/encoder/satd.c): |a - b| summed, and (a - b)^2 summed. */
uint32_t n265_sad_wh_neon(const pixel *a, int as, const pixel *b,
                          int bs, int w, int h);
uint64_t n265_ssd_wh_neon(const pixel *a, int as, const pixel *b,
                          int bs, int w, int h);

/* The same absolute difference kept as two marginal sums instead of one
 * total: row[y] and col[x] over an n x n block, for ctu.c's rect_proxy. */
void n265_sad_rowcol_neon(const pixel *a, int as, const pixel *b,
                          int bs, int n, uint32_t *row, uint32_t *col);

/* n265_psy_energy: the 4x4 Hadamard AC energy of an n x n block */
uint32_t n265_psy_energy_neon(const pixel *p, int stride, int n);

/* n265_sum_sq_wh at w = 16, h <= 16 */
uint32_t n265_sum_sq16_neon(const pixel *p, int stride, int h, uint64_t *sq);

#if BIT_DEPTH == 8 && defined(__ARM_FEATURE_DOTPROD)
/* The FEAT_DotProd arm of n265_ssd_wh_neon, which selects it at runtime
 * on N265_CPU_DOTPROD (udot of a difference with itself is the sum of
 * four squares). Exported so checkasm can check and time BOTH arms on a
 * host that has the extension; not a dispatch site's entry point.
 * 8-BIT ONLY: udot is defined on byte lanes and has no 16-bit form
 * outside SVE, so the Main10 SSD's widening multiply-accumulate is the
 * fastest shape available to it. */
uint64_t n265_ssd_wh_dot_neon(const pixel *a, int as, const pixel *b,
                              int bs, int w, int h);
#endif

#endif /* N265_NEON_SATD */

#if N265_NEON_RDOQ
/* ---- rdoq (N265_ASM_QUANT) ----------------------------------------- */

/* The two elementwise pieces around the RDOQ lattice; the lattice itself
 * is sequential in the CABAC state and stays on C. Shapes are the four
 * square transform blocks, packed (stride n), same as the transform
 * family: 4x4, 8x8, 16x16, 32x32 and nothing else. See rdoq_neon.c for
 * the range proof that the C's int64 product fits int32 lanes and that
 * the pairwise energy reduction gives the C's double bit for bit. */

int n265_rdoq_rtn_neon(int16_t *rtn, double *all_zero, uint64_t *sb_nz,
                       const int16_t *coef, int log2n, int q, int64_t add,
                       int qbits, int czero);
uint64_t n265_rdoq_sb_nz_neon(const int16_t *blk, int log2n);
#endif /* N265_NEON_RDOQ */

#if N265_NEON_COST
/* ---- coefficient costing (N265_ASM_QUANT) --------------------------
 *
 * residual_coding()'s two elementwise passes over a transform block's
 * levels; src/encoder/residual.h has the contracts. Both are gathers over
 * a 4x4 group, and the bin-by-bin context walk they feed is serial by
 * construction and stays in C.
 *
 * `stride` is in int16 elements. The group kernel reads exactly four rows
 * of four and nothing wider, so a group at the far edge of a block stays
 * inside its own declared window. */
void n265_csbf_map_neon(const int16_t *level, int log2n, uint8_t *out);
uint32_t n265_coeff_group_neon(const int16_t *grp, ptrdiff_t stride,
                               int scan_idx, uint16_t *abs_out,
                               uint32_t *neg_mask);

/* The sign-hiding parity fix over every 4x4 group of a block, in place.
 * Same per-group read window as the gather above. */
void n265_sbh_adjust_neon(int16_t *level, int log2n, int scan_idx);
#endif /* N265_NEON_COST */

#if N265_NEON_INTRA
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
 */

void n265_intra_pred_neon(pixel *dst, int dst_stride, const pixel *refs,
                          int log2n, int mode, int c_idx);
void n265_filter_refs_neon(pixel *out, const pixel *in, int log2n,
                           int strong);

#endif /* N265_NEON_INTRA */

#endif /* N265_ARM_NEON_H */
