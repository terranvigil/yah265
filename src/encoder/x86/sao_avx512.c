/*
 * sao_avx512.c - the SAO row kernels (N265_ASM_SAO) on AVX-512 BW, at
 * both depths: eo_stats, eo_apply and bo_apply. band_stats stays AVX2.
 *
 * 32 int16 lanes, an 8-bit row widened on load and narrowed on store. The
 * edge category is sao_avx2.c's sign(c - a) + sign(c - b); each category
 * is a k-mask, so a count is a popcount and the sums are masked moves into
 * pmaddwd. The offsets are a vpermw lookup, and the store is masked to the
 * samples that change. A row tail is a masked load and store, so no column
 * goes to the C reference.
 *
 * Range: a sum lane adds pairs of src - cur, at most 2 * 1023 a step, in
 * int32; a row is at most 64 samples.
 *
 * Copyright (c) 2026, the yah265 authors. GPL-2.0-or-later; see LICENSE.
 */

#include <immintrin.h>

#include "encoder/sao.h"
#include "encoder/x86/avx2.h"
#include "encoder/x86/avx512.h"

#define AI static inline __attribute__((always_inline))
#define SAO_SHIFT (BIT_DEPTH - 5)

#if BIT_DEPTH == 8
AI __m512i ld32(const pixel *p, __mmask32 k)
{
    return _mm512_cvtepu8_epi16(_mm256_maskz_loadu_epi8(k, p));
}

AI void st32(pixel *p, __mmask32 k, __m512i v)
{
    _mm256_mask_storeu_epi8(p, k, _mm512_cvtepi16_epi8(v));
}
#else
AI __m512i ld32(const pixel *p, __mmask32 k)
{
    return _mm512_maskz_loadu_epi16(k, p);
}

AI void st32(pixel *p, __mmask32 k, __m512i v)
{
    _mm512_mask_storeu_epi16(p, k, v);
}
#endif

AI __mmask32 tail(int left)
{
    return left >= 32 ? 0xffffffffu : (__mmask32)((1u << left) - 1);
}

/* sign(c - a) + sign(c - b), -2..2 per lane */
AI __m512i eo_cat2(__m512i c, __m512i a, __m512i b)
{
    const __m512i one = _mm512_set1_epi16(1);
    __m512i s = _mm512_setzero_si512();
    s = _mm512_mask_add_epi16(s, _mm512_cmpgt_epi16_mask(c, a), s, one);
    s = _mm512_mask_sub_epi16(s, _mm512_cmpgt_epi16_mask(a, c), s, one);
    s = _mm512_mask_add_epi16(s, _mm512_cmpgt_epi16_mask(c, b), s, one);
    s = _mm512_mask_sub_epi16(s, _mm512_cmpgt_epi16_mask(b, c), s, one);
    return s;
}

AI __m512i clip1(__m512i v)
{
    return _mm512_max_epi16(_mm512_min_epi16(v, _mm512_set1_epi16(PIXEL_MAX)),
                            _mm512_setzero_si512());
}

void n265_sao_eo_stats_avx512(const pixel *src, const pixel *cur,
                              const pixel *a, const pixel *b, int n,
                              int64_t en[5], int64_t ed[5])
{
    static const int16_t sgn[4] = { -2, -1, 1, 2 };   /* categories 1..4 */
    const __m512i one = _mm512_set1_epi16(1);
    __m512i dsum[4];
    int64_t cnt[4] = { 0, 0, 0, 0 };

    for (int k = 0; k < 4; k++)
        dsum[k] = _mm512_setzero_si512();
    for (int x = 0; x < n; x += 32) {
        __mmask32 kt = tail(n - x);
        __m512i c = ld32(cur + x, kt);
        __m512i d = _mm512_sub_epi16(ld32(src + x, kt), c);
        __m512i s = eo_cat2(c, ld32(a + x, kt), ld32(b + x, kt));
        for (int k = 0; k < 4; k++) {
            __mmask32 m = _mm512_mask_cmpeq_epi16_mask(
                kt, s, _mm512_set1_epi16(sgn[k]));
            cnt[k] += _mm_popcnt_u32(m);
            dsum[k] = _mm512_add_epi32(
                dsum[k], _mm512_madd_epi16(_mm512_maskz_mov_epi16(m, d), one));
        }
    }
    for (int k = 0; k < 4; k++) {
        en[k + 1] += cnt[k];
        ed[k + 1] += _mm512_reduce_add_epi32(dsum[k]);
    }
}

void n265_sao_eo_apply_avx512(pixel *dst, const pixel *cur, const pixel *a,
                              const pixel *b, int n, const int8_t off[4])
{
    /* indexed by category sum + 2 */
    const __m512i lut = _mm512_setr_epi32(
        (uint16_t)off[0] | ((uint32_t)(uint16_t)off[1] << 16),
        (uint32_t)(uint16_t)off[2] << 16, (uint16_t)off[3],
        0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0);
    const __m512i two = _mm512_set1_epi16(2);

    for (int x = 0; x < n; x += 32) {
        __mmask32 kt = tail(n - x);
        __m512i c = ld32(cur + x, kt);
        __m512i s = eo_cat2(c, ld32(a + x, kt), ld32(b + x, kt));
        __m512i o = _mm512_permutexvar_epi16(_mm512_add_epi16(s, two), lut);
        __mmask32 m = _mm512_mask_test_epi16_mask(kt, s, s);
        st32(dst + x, m, clip1(_mm512_add_epi16(c, o)));
    }
}

void n265_sao_bo_apply_avx512(pixel *dst, const pixel *in, int n, int pos,
                              const int8_t off[4])
{
    const __m512i lut = _mm512_setr_epi32(
        (uint16_t)off[0] | ((uint32_t)(uint16_t)off[1] << 16),
        (uint16_t)off[2] | ((uint32_t)(uint16_t)off[3] << 16),
        0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0);
    const __m512i pv = _mm512_set1_epi16((int16_t)pos);
    const __m512i four = _mm512_set1_epi16(4);

    for (int x = 0; x < n; x += 32) {
        __mmask32 kt = tail(n - x);
        __m512i v = ld32(in + x, kt);
        __m512i band = _mm512_sub_epi16(_mm512_srli_epi16(v, SAO_SHIFT), pv);
        __mmask32 m = _mm512_mask_cmplt_epu16_mask(kt, band, four);
        __m512i o = _mm512_permutexvar_epi16(band, lut);
        st32(dst + x, m, clip1(_mm512_add_epi16(v, o)));
    }
}
