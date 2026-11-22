/*
 * cost_avx512.c - csbf_map (N265_ASM_QUANT) on AVX-512 BW, at both depths.
 *
 * A sub-block row of the TB (four rows) is n / 8 vectors of 32 levels
 * at n >= 8. Their nonzero mask is folded to one bit per 4x4 group, and
 * the whole TB's map is written from one mask at the end. coeff_group and
 * sbh_adjust work on a single 4x4 group and stay AVX2.
 *
 * Copyright (c) 2026, the yah265 authors. GPL-2.0-or-later; see LICENSE.
 */

#include <immintrin.h>

#include "encoder/residual.h"
#include "encoder/x86/avx2.h"
#include "encoder/x86/avx512.h"

#define AI static inline __attribute__((always_inline))

/* one bit per 4-lane group of a 32-lane mask whose row is n lanes */
AI uint32_t sb_bits(uint32_t k, int n)
{
    if (n <= 16)
        k |= k >> 16;
    if (n <= 8)
        k |= k >> 8;
    k |= k >> 1;
    k |= k >> 2;
    return _pext_u32(k, n == 32 ? 0x11111111u : n == 16 ? 0x1111u : 0x11u);
}

void n265_csbf_map_avx512(const int16_t *level, int log2n, uint8_t *out)
{
    const int n = 1 << log2n, sb_dim = n >> 2;
    uint64_t m = 0;

    if (log2n == 2) {
        __m256i v = _mm256_loadu_si256((const __m256i *)level);
        out[0] = !_mm256_testz_si256(v, v);
        return;
    }
    const int per = n >> 3;
    for (int sy = 0; sy < sb_dim; sy++) {
        const int16_t *p = level + (size_t)sy * 4 * n;
        __m512i o = _mm512_loadu_si512(p);
        for (int i = 1; i < per; i++)
            o = _mm512_or_si512(o, _mm512_loadu_si512(p + 32 * i));
        m |= (uint64_t)sb_bits((uint32_t)_mm512_test_epi16_mask(o, o), n)
             << (sy * sb_dim);
    }
    __m512i bytes = _mm512_maskz_mov_epi8(m, _mm512_set1_epi8(1));
    int nsb = sb_dim * sb_dim;
    _mm512_mask_storeu_epi8(out, nsb == 64 ? ~0ull : (1ull << nsb) - 1,
                            bytes);
}
