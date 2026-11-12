/*
 * avx512.h - the x86-64 AVX-512 entry points (F, BW and VL).
 *
 * A kernel is `n265_<kernel>_avx512(...)` with its AVX2 twin's signature
 * and contract, defined in src/encoder/x86/<family>_avx512.c, the only
 * files built with the AVX-512 flags; every one is BIT-EXACT with its C
 * reference, at both depths. Unlike the other tiers a family is NOT
 * ported whole: an entry point whose AVX-512 kernel measured no faster
 * than AVX2 on real hardware is aliased to the AVX2 kernel here, so
 * checkasm's tier still names every kernel and dispatch keeps AVX2 for it.
 * An alias undefines the name first, since symbols10.h may rename it.
 *
 * Copyright (c) 2026, the yah265 authors. GPL-2.0-or-later; see LICENSE.
 */

#ifndef N265_X86_AVX512_H
#define N265_X86_AVX512_H

#include <stddef.h>
#include <stdint.h>

#include "common/cpu.h"     /* N265_AVX512_<FAMILY>, normalized to 0/1 */
#include "common/pixel.h"

/* ---- satd (N265_ASM_SATD) ------------------------------------------ */
#if N265_AVX512_SATD
uint32_t n265_satd_wh_avx512(const pixel *a, int as, const pixel *b,
                             int bs, int w, int h);
uint32_t n265_sad_wh_avx512(const pixel *a, int as, const pixel *b,
                            int bs, int w, int h);
uint64_t n265_ssd_wh_avx512(const pixel *a, int as, const pixel *b,
                            int bs, int w, int h);
uint32_t n265_psy_energy_avx512(const pixel *p, int stride, int n);

/* The shapes the kernels above widen. Every other shape they hand to
 * the AVX2 kernel, so a dispatch site calls that one directly. */
static inline int n265_satd_wh_avx512_wide(int w, int h)
{
    return !((w | h) & 7);
}

static inline int n265_sad_wh_avx512_wide(int w, int h)
{
#if BIT_DEPTH == 8
    return w > 16 || (h >= 4 && (w >= 9 || w == 8 || w == 4));
#else
    return w > 8 || (h >= 4 && (w == 8 || w == 4));
#endif
}

static inline int n265_ssd_wh_avx512_wide(int w, int h)
{
    return w > 8 || (h >= 4 && (w == 8 || w == 4));
}

static inline int n265_psy_energy_avx512_wide(int n)
{
    return n >= 16 && !(n & 15);
}
#else
#undef n265_satd_wh_avx512
#define n265_satd_wh_avx512 n265_satd_wh_avx2
#undef n265_sad_wh_avx512
#define n265_sad_wh_avx512 n265_sad_wh_avx2
#undef n265_ssd_wh_avx512
#define n265_ssd_wh_avx512 n265_ssd_wh_avx2
#undef n265_psy_energy_avx512
#define n265_psy_energy_avx512 n265_psy_energy_avx2
#endif
#undef n265_satd4_avx512
#define n265_satd4_avx512 n265_satd4_avx2
#undef n265_satd8_avx512
#define n265_satd8_avx512 n265_satd8_avx2
#undef n265_sad_rowcol_avx512
#define n265_sad_rowcol_avx512 n265_sad_rowcol_avx2
#undef n265_sum_sq16_avx512
#define n265_sum_sq16_avx512 n265_sum_sq16_avx2

/* ---- mc (N265_ASM_MC) ---------------------------------------------- */
#undef n265_bi_avg_wh_avx512
#define n265_bi_avg_wh_avx512 n265_bi_avg_wh_avx2
#undef n265_wp_uni_wh_avx512
#define n265_wp_uni_wh_avx512 n265_wp_uni_wh_avx2
#undef n265_wp_bi_wh_avx512
#define n265_wp_bi_wh_avx512 n265_wp_bi_wh_avx2
#undef n265_wp_pix_wh_avx512
#define n265_wp_pix_wh_avx512 n265_wp_pix_wh_avx2
#undef n265_mc_luma_hf_avx512
#define n265_mc_luma_hf_avx512 n265_mc_luma_hf_avx2
#undef n265_mc_luma_14_hf_avx512
#define n265_mc_luma_14_hf_avx512 n265_mc_luma_14_hf_avx2
#undef n265_mc_luma_wh_avx512
#define n265_mc_luma_wh_avx512 n265_mc_luma_wh_avx2
#undef n265_mc_luma_14_wh_avx512
#define n265_mc_luma_14_wh_avx512 n265_mc_luma_14_wh_avx2
#undef n265_mc_luma_hplane_avx512
#define n265_mc_luma_hplane_avx512 n265_mc_luma_hplane_avx2
#undef n265_mc_chroma_wh_avx512
#define n265_mc_chroma_wh_avx512 n265_mc_chroma_wh_avx2
#undef n265_mc_chroma_14_wh_avx512
#define n265_mc_chroma_14_wh_avx512 n265_mc_chroma_14_wh_avx2

/* ---- transform (N265_ASM_DCT, N265_ASM_QUANT) ---------------------- */
#undef n265_fdct_avx512
#define n265_fdct_avx512 n265_fdct_avx2
#undef n265_idct_add_avx512
#define n265_idct_add_avx512 n265_idct_add_avx2
#undef n265_dequant_avx512
#define n265_dequant_avx512 n265_dequant_avx2
#undef n265_quant_avx512
#define n265_quant_avx512 n265_quant_avx2

/* ---- families with no AVX-512 kernel ------------------------------- */
#undef n265_rdoq_rtn_avx512
#define n265_rdoq_rtn_avx512 n265_rdoq_rtn_avx2
#undef n265_rdoq_sb_nz_avx512
#define n265_rdoq_sb_nz_avx512 n265_rdoq_sb_nz_avx2
#undef n265_csbf_map_avx512
#define n265_csbf_map_avx512 n265_csbf_map_avx2
#undef n265_coeff_group_avx512
#define n265_coeff_group_avx512 n265_coeff_group_avx2
#undef n265_sbh_adjust_avx512
#define n265_sbh_adjust_avx512 n265_sbh_adjust_avx2
#undef n265_intra_pred_avx512
#define n265_intra_pred_avx512 n265_intra_pred_avx2
#undef n265_filter_refs_avx512
#define n265_filter_refs_avx512 n265_filter_refs_avx2
#undef n265_dbk_luma_v_avx512
#define n265_dbk_luma_v_avx512 n265_dbk_luma_v_avx2
#undef n265_dbk_luma_h_avx512
#define n265_dbk_luma_h_avx512 n265_dbk_luma_h_avx2
#undef n265_dbk_chroma_v_avx512
#define n265_dbk_chroma_v_avx512 n265_dbk_chroma_v_avx2
#undef n265_dbk_chroma_h_avx512
#define n265_dbk_chroma_h_avx512 n265_dbk_chroma_h_avx2
#undef n265_sao_band_stats_avx512
#define n265_sao_band_stats_avx512 n265_sao_band_stats_avx2
#undef n265_sao_eo_stats_avx512
#define n265_sao_eo_stats_avx512 n265_sao_eo_stats_avx2
#undef n265_sao_eo_apply_avx512
#define n265_sao_eo_apply_avx512 n265_sao_eo_apply_avx2
#undef n265_sao_bo_apply_avx512
#define n265_sao_bo_apply_avx512 n265_sao_bo_apply_avx2

#endif /* N265_X86_AVX512_H */
