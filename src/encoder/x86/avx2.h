/*
 * avx2.h - every x86-64 AVX2 kernel entry point in one place.
 *
 * The x86 twin of src/encoder/arm/neon.h, and the same convention:
 *
 *   - A kernel is `n265_<kernel>_avx2(...)` with the SAME signature as
 *     its NEON twin `n265_<kernel>_neon(...)`, declared here under
 *     `#if N265_AVX2_<FAMILY>` and defined in src/encoder/x86/
 *     <family>_avx2.c. A family is ported whole: every entry point in its
 *     neon.h block gets an _avx2 twin, because checkasm checks the family
 *     through one name map (tests/checkasm/checkasm.h, CA_SIMD).
 *   - Those files are the only ones compiled with -mavx2 -mfma -mbmi2.
 *     Everything else stays baseline x86-64 and reaches a kernel through
 *     a dispatch site that tests n265_avx2_on(class), which reads CPUID.
 *   - Both depths. `pixel` is uint8_t in the 8-bit library and uint16_t
 *     in the Main10 one, where symbols10.h gives every name a _10 suffix.
 *   - Every kernel is BIT-EXACT with its C reference.
 *
 * Copyright (c) 2026, the yah265 authors. GPL-2.0-or-later; see LICENSE.
 */

#ifndef N265_X86_AVX2_H
#define N265_X86_AVX2_H

#include <stddef.h>
#include <stdint.h>

#include "common/cpu.h"     /* N265_AVX2_<FAMILY>, normalized to 0/1 */
#include "common/pixel.h"

#if N265_AVX2_SATD
/* ---- satd (N265_ASM_SATD) ------------------------------------------ */

uint32_t n265_satd4_avx2(const pixel *a, int as, const pixel *b, int bs);
uint32_t n265_satd8_avx2(const pixel *a, int as, const pixel *b, int bs);
uint32_t n265_satd_wh_avx2(const pixel *a, int as, const pixel *b,
                           int bs, int w, int h);
uint32_t n265_sad_wh_avx2(const pixel *a, int as, const pixel *b,
                          int bs, int w, int h);
uint64_t n265_ssd_wh_avx2(const pixel *a, int as, const pixel *b,
                          int bs, int w, int h);
void n265_sad_rowcol_avx2(const pixel *a, int as, const pixel *b,
                          int bs, int n, uint32_t *row, uint32_t *col);
uint32_t n265_psy_energy_avx2(const pixel *p, int stride, int n);
uint32_t n265_sum_sq16_avx2(const pixel *p, int stride, int h,
                            uint64_t *sq);
#endif /* N265_AVX2_SATD */

/* The contracts, shapes and range arguments are the NEON twins' in
 * neon.h. */

#if N265_AVX2_MC
/* ---- mc (N265_ASM_MC) ---------------------------------------------- */

void n265_bi_avg_wh_avx2(pixel *dst, int dst_stride, const int16_t *a,
                         const int16_t *b, int w, int h);
void n265_wp_uni_wh_avx2(pixel *dst, int ds, const int16_t *src, int w,
                         int h, int wt, int off, int denom);
void n265_wp_bi_wh_avx2(pixel *dst, int ds, const int16_t *a,
                        const int16_t *b, int w, int h, int w0, int o0,
                        int w1, int o1, int denom);
void n265_wp_pix_wh_avx2(pixel *dst, int ds, const pixel *src, int ss,
                         int w, int h, int wt, int off, int denom);
void n265_mc_luma_hf_avx2(pixel *dst, int dst_stride, int w, int h,
                          const int16_t *hf, int hf_stride, int fy);
void n265_mc_luma_14_hf_avx2(int16_t *dst, int dst_stride, int w, int h,
                             const int16_t *hf, int hf_stride, int fy);
void n265_mc_luma_wh_avx2(pixel *dst, int dst_stride, int w, int h,
                          const pixel *ref, int ref_stride, int fx, int fy);
void n265_mc_luma_14_wh_avx2(int16_t *dst, int dst_stride, int w, int h,
                             const pixel *ref, int ref_stride, int fx,
                             int fy);
void n265_mc_luma_hplane_avx2(int16_t *dst, int dst_stride, int w, int h,
                              const pixel *ref, int ref_stride, int fx);
void n265_mc_chroma_wh_avx2(pixel *dst, int dst_stride, int w, int h,
                            const pixel *ref, int ref_stride, int fx,
                            int fy);
void n265_mc_chroma_14_wh_avx2(int16_t *dst, int dst_stride, int w, int h,
                               const pixel *ref, int ref_stride, int fx,
                               int fy);
#endif /* N265_AVX2_MC */

#if N265_AVX2_TRANSFORM
/* ---- transform (N265_ASM_DCT, N265_ASM_QUANT) ---------------------- */

void n265_fdct_avx2(int16_t *coef, const int16_t *resid, int log2n,
                    int use_dst);
void n265_idct_add_avx2(pixel *dst, int dst_stride, const int16_t *dq,
                        int log2n, const pixel *pred, int pred_stride,
                        int use_dst, int nzw, int nzh);
void n265_dequant_avx2(int16_t *dq, const int16_t *level, int log2n, int qp,
                       int *nzw, int *nzh);
int n265_quant_avx2(int16_t *level, const int16_t *coef, int log2n, int qp,
                    int intra);
#endif /* N265_AVX2_TRANSFORM */

#if N265_AVX2_RDOQ
/* ---- rdoq (N265_ASM_QUANT) ----------------------------------------- */

int n265_rdoq_rtn_avx2(int16_t *rtn, double *all_zero, uint64_t *sb_nz,
                       const int16_t *coef, int log2n, int q, int64_t add,
                       int qbits, int czero);
uint64_t n265_rdoq_sb_nz_avx2(const int16_t *blk, int log2n);
#endif /* N265_AVX2_RDOQ */

#if N265_AVX2_COST
/* ---- coefficient costing (N265_ASM_QUANT) -------------------------- */

void n265_csbf_map_avx2(const int16_t *level, int log2n, uint8_t *out);
uint32_t n265_coeff_group_avx2(const int16_t *grp, ptrdiff_t stride,
                               int scan_idx, uint16_t *abs_out,
                               uint32_t *neg_mask);
void n265_sbh_adjust_avx2(int16_t *level, int log2n, int scan_idx);
#endif /* N265_AVX2_COST */

#if N265_AVX2_INTRA
/* ---- intra (N265_ASM_INTRA) ---------------------------------------- */

void n265_intra_pred_avx2(pixel *dst, int dst_stride, const pixel *refs,
                          int log2n, int mode, int c_idx);
void n265_filter_refs_avx2(pixel *out, const pixel *in, int log2n,
                           int strong);
#endif /* N265_AVX2_INTRA */

#if N265_AVX2_DEBLOCK
/* ---- deblock (N265_ASM_DBK) ---------------------------------------- */

void n265_dbk_luma_v_avx2(pixel *pix, ptrdiff_t stride, const int16_t *beta,
                          const int16_t *tc, int nseg);
void n265_dbk_luma_h_avx2(pixel *pix, ptrdiff_t stride, const int16_t *beta,
                          const int16_t *tc, int nseg);
void n265_dbk_chroma_v_avx2(pixel *pix, ptrdiff_t stride, const int16_t *tc,
                            int n);
void n265_dbk_chroma_h_avx2(pixel *pix, ptrdiff_t stride, const int16_t *tc,
                            int n);
#endif /* N265_AVX2_DEBLOCK */

#if N265_AVX2_SAO
/* ---- sao (N265_ASM_SAO) -------------------------------------------- */

/* src/encoder/sao.h has the contracts. */
void n265_sao_band_stats_avx2(const pixel *src, int src_stride,
                              const pixel *rec, int rec_stride, int w, int h,
                              int64_t bn[32], int64_t bd[32]);
void n265_sao_eo_stats_avx2(const pixel *src, const pixel *cur,
                            const pixel *a, const pixel *b, int n,
                            int64_t en[5], int64_t ed[5]);
void n265_sao_eo_apply_avx2(pixel *dst, const pixel *cur, const pixel *a,
                            const pixel *b, int n, const int8_t off[4]);
void n265_sao_bo_apply_avx2(pixel *dst, const pixel *in, int n, int pos,
                            const int8_t off[4]);
#endif /* N265_AVX2_SAO */

#endif /* N265_X86_AVX2_H */
