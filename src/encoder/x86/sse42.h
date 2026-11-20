/*
 * sse42.h - every x86-64 SSE4.2 kernel entry point in one place.
 *
 * The tier for x86-64 CPUs without AVX2. A kernel is
 * `n265_<kernel>_sse42(...)` with its AVX2 twin's signature and contract,
 * defined in src/encoder/x86/<family>_sse42.c, the only files built with
 * -msse4.2. A family is ported whole, at both depths (symbols10.h gives
 * the Main10 names a _10 suffix), and every kernel is BIT-EXACT with its
 * C reference.
 *
 * Copyright (c) 2026, the yah265 authors. GPL-2.0-or-later; see LICENSE.
 */

#ifndef N265_X86_SSE42_H
#define N265_X86_SSE42_H

#include <stddef.h>
#include <stdint.h>

#include "common/cpu.h"     /* N265_SSE42_<FAMILY>, normalized to 0/1 */
#include "common/pixel.h"

#if N265_SSE42_SATD
/* ---- satd (N265_ASM_SATD) ------------------------------------------ */

uint32_t n265_satd4_sse42(const pixel *a, int as, const pixel *b, int bs);
uint32_t n265_satd8_sse42(const pixel *a, int as, const pixel *b, int bs);
uint32_t n265_satd_wh_sse42(const pixel *a, int as, const pixel *b,
                            int bs, int w, int h);
uint32_t n265_sad_wh_sse42(const pixel *a, int as, const pixel *b,
                           int bs, int w, int h);
uint64_t n265_ssd_wh_sse42(const pixel *a, int as, const pixel *b,
                           int bs, int w, int h);
void n265_sad_rowcol_sse42(const pixel *a, int as, const pixel *b,
                           int bs, int n, uint32_t *row, uint32_t *col);
uint32_t n265_psy_energy_sse42(const pixel *p, int stride, int n);
uint32_t n265_sum_sq16_sse42(const pixel *p, int stride, int h,
                             uint64_t *sq);
#endif /* N265_SSE42_SATD */

#if N265_SSE42_MC
/* ---- mc (N265_ASM_MC) ---------------------------------------------- */

void n265_bi_avg_wh_sse42(pixel *dst, int dst_stride, const int16_t *a,
                          const int16_t *b, int w, int h);
void n265_wp_uni_wh_sse42(pixel *dst, int ds, const int16_t *src, int w,
                          int h, int wt, int off, int denom);
void n265_wp_bi_wh_sse42(pixel *dst, int ds, const int16_t *a,
                         const int16_t *b, int w, int h, int w0, int o0,
                         int w1, int o1, int denom);
void n265_wp_pix_wh_sse42(pixel *dst, int ds, const pixel *src, int ss,
                          int w, int h, int wt, int off, int denom);
void n265_mc_luma_hf_sse42(pixel *dst, int dst_stride, int w, int h,
                           const int16_t *hf, int hf_stride, int fy);
void n265_mc_luma_14_hf_sse42(int16_t *dst, int dst_stride, int w, int h,
                              const int16_t *hf, int hf_stride, int fy);
void n265_mc_luma_wh_sse42(pixel *dst, int dst_stride, int w, int h,
                           const pixel *ref, int ref_stride, int fx, int fy);
void n265_mc_luma_14_wh_sse42(int16_t *dst, int dst_stride, int w, int h,
                              const pixel *ref, int ref_stride, int fx,
                              int fy);
void n265_mc_luma_hplane_sse42(int16_t *dst, int dst_stride, int w, int h,
                               const pixel *ref, int ref_stride, int fx);
void n265_mc_chroma_wh_sse42(pixel *dst, int dst_stride, int w, int h,
                             const pixel *ref, int ref_stride, int fx,
                             int fy);
void n265_mc_chroma_14_wh_sse42(int16_t *dst, int dst_stride, int w, int h,
                                const pixel *ref, int ref_stride, int fx,
                                int fy);
#endif /* N265_SSE42_MC */

#if N265_SSE42_TRANSFORM
/* ---- transform (N265_ASM_DCT, N265_ASM_QUANT) ---------------------- */

void n265_fdct_sse42(int16_t *coef, const int16_t *resid, int log2n,
                     int use_dst);
void n265_idct_add_sse42(pixel *dst, int dst_stride, const int16_t *dq,
                         int log2n, const pixel *pred, int pred_stride,
                         int use_dst, int nzw, int nzh);
void n265_dequant_sse42(int16_t *dq, const int16_t *level, int log2n, int qp,
                        int *nzw, int *nzh);
int n265_quant_sse42(int16_t *level, const int16_t *coef, int log2n, int qp,
                     int intra);
#endif /* N265_SSE42_TRANSFORM */

#if N265_SSE42_RDOQ
/* ---- rdoq (N265_ASM_QUANT) ----------------------------------------- */

int n265_rdoq_rtn_sse42(int16_t *rtn, double *all_zero, uint64_t *sb_nz,
                        const int16_t *coef, int log2n, int q, int64_t add,
                        int qbits, int czero);
uint64_t n265_rdoq_sb_nz_sse42(const int16_t *blk, int log2n);
#endif /* N265_SSE42_RDOQ */

#if N265_SSE42_COST
/* ---- coefficient costing (N265_ASM_QUANT) -------------------------- */

void n265_csbf_map_sse42(const int16_t *level, int log2n, uint8_t *out);
uint32_t n265_coeff_group_sse42(const int16_t *grp, ptrdiff_t stride,
                                int scan_idx, uint16_t *abs_out,
                                uint32_t *neg_mask);
void n265_sbh_adjust_sse42(int16_t *level, int log2n, int scan_idx);
#endif /* N265_SSE42_COST */

#if N265_SSE42_INTRA
/* ---- intra (N265_ASM_INTRA) ---------------------------------------- */

void n265_intra_pred_sse42(pixel *dst, int dst_stride, const pixel *refs,
                           int log2n, int mode, int c_idx);
void n265_filter_refs_sse42(pixel *out, const pixel *in, int log2n,
                            int strong);
#endif /* N265_SSE42_INTRA */

#if N265_SSE42_DEBLOCK
/* ---- deblock (N265_ASM_DBK) ---------------------------------------- */

void n265_dbk_luma_v_sse42(pixel *pix, ptrdiff_t stride, const int16_t *beta,
                           const int16_t *tc, int nseg);
void n265_dbk_luma_h_sse42(pixel *pix, ptrdiff_t stride, const int16_t *beta,
                           const int16_t *tc, int nseg);
void n265_dbk_chroma_v_sse42(pixel *pix, ptrdiff_t stride, const int16_t *tc,
                             int n);
void n265_dbk_chroma_h_sse42(pixel *pix, ptrdiff_t stride, const int16_t *tc,
                             int n);
#endif /* N265_SSE42_DEBLOCK */

#if N265_SSE42_SAO
/* ---- sao (N265_ASM_SAO) -------------------------------------------- */

void n265_sao_band_stats_sse42(const pixel *src, int src_stride,
                               const pixel *rec, int rec_stride, int w, int h,
                               int64_t bn[32], int64_t bd[32]);
void n265_sao_eo_stats_sse42(const pixel *src, const pixel *cur,
                             const pixel *a, const pixel *b, int n,
                             int64_t en[5], int64_t ed[5]);
void n265_sao_eo_apply_sse42(pixel *dst, const pixel *cur, const pixel *a,
                             const pixel *b, int n, const int8_t off[4]);
void n265_sao_bo_apply_sse42(pixel *dst, const pixel *in, int n, int pos,
                             const int8_t off[4]);
#endif /* N265_SSE42_SAO */

#endif /* N265_X86_SSE42_H */
