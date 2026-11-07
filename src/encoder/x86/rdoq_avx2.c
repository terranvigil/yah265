/*
 * rdoq_avx2.c - the elementwise half of the RDOQ lattice (N265_ASM_QUANT)
 * on AVX2.
 *
 * The entry points are src/encoder/arm/neon.h's rdoq block, renamed.
 * rdoq_neon.c's header holds the two proofs this relies on: the energy is
 * an exact integer below 2^53 in any summation order, and |coef| * q + add
 * fits int32, so the deadzone branch (`czero`) is not needed.
 *
 * `pmaddwd` of a row with itself sums two squares; (-32768)^2 * 2 is
 * 2^31, which wraps the signed lane but is right read as uint32, so the
 * pair sums are zero-extended before the int64 accumulation.
 *
 * Copyright (c) 2026, the yah265 authors. GPL-2.0-or-later; see LICENSE.
 */

#include <immintrin.h>

#include "encoder/rdoq.h"
#include "encoder/x86/avx2.h"

typedef struct {
    __m256i q, add, max, acc;
    __m128i sh;
} rtn_ctx;

/* eight coefficients: their levels stored as int16, returned as int32 */
static inline __m256i rtn8(rtn_ctx *k, int16_t *rtn, const int16_t *coef)
{
    __m128i c16 = _mm_loadu_si128((const __m128i *)coef);
    __m256i c = _mm256_abs_epi32(_mm256_cvtepi16_epi32(c16));
    __m256i l = _mm256_add_epi32(_mm256_mullo_epi32(c, k->q), k->add);

    l = _mm256_min_epi32(_mm256_srl_epi32(l, k->sh), k->max);
    k->acc = _mm256_add_epi64(
        k->acc, _mm256_cvtepu32_epi64(_mm_madd_epi16(c16, c16)));
    _mm_storeu_si128((__m128i *)rtn,
                     _mm256_castsi256_si128(_mm256_permute4x64_epi64(
                         _mm256_packs_epi32(l, l), 0x08)));
    return l;
}

int n265_rdoq_rtn_avx2(int16_t *rtn, double *all_zero, uint64_t *sb_nz,
                       const int16_t *coef, int log2n, int q, int64_t add,
                       int qbits, int czero)
{
    const int n = 1 << log2n, sb_dim = n >> 2;
    rtn_ctx k = {
        _mm256_set1_epi32(q), _mm256_set1_epi32((int32_t)add),
        _mm256_set1_epi32(32767), _mm256_setzero_si256(),
        _mm_cvtsi32_si128(qbits),
    };
    uint64_t m = 0;

    (void)czero;
    if (n == 4) {
        __m256i any = _mm256_or_si256(rtn8(&k, rtn, coef),
                                      rtn8(&k, rtn + 8, coef + 8));
        m = !_mm256_testz_si256(any, any);
    } else {
        for (int y = 0; y < n; y += 4) {
            for (int x = 0; x < n; x += 8) {
                __m256i o = _mm256_setzero_si256();
                int sb = (y >> 2) * sb_dim + (x >> 2);

                for (int r = 0; r < 4; r++) {
                    int idx = (y + r) * n + x;
                    o = _mm256_or_si256(o, rtn8(&k, rtn + idx, coef + idx));
                }
                /* lane 0 is sub-block sb, lane 1 is sb + 1 */
                unsigned z = (unsigned)_mm256_movemask_ps(_mm256_castsi256_ps(
                    _mm256_cmpeq_epi32(o, _mm256_setzero_si256())));
                if ((z & 0x0f) != 0x0f)
                    m |= (uint64_t)1 << sb;
                if ((z & 0xf0) != 0xf0)
                    m |= (uint64_t)1 << (sb + 1);
            }
        }
    }

    __m128i e = _mm_add_epi64(_mm256_castsi256_si128(k.acc),
                              _mm256_extracti128_si256(k.acc, 1));
    *all_zero = (double)(uint64_t)(_mm_cvtsi128_si64(e) +
                                   _mm_extract_epi64(e, 1));
    *sb_nz = m;
    return m != 0;
}

uint64_t n265_rdoq_sb_nz_avx2(const int16_t *blk, int log2n)
{
    const int n = 1 << log2n, sb_dim = n >> 2;
    uint64_t m = 0;

    if (n == 4) {
        __m256i v = _mm256_loadu_si256((const __m256i *)blk);
        return !_mm256_testz_si256(v, v);
    }
    if (n == 8) {
        for (int y = 0; y < n; y += 4) {
            const int16_t *p = blk + y * n;
            __m128i v = _mm_or_si128(
                _mm_or_si128(_mm_loadu_si128((const __m128i *)p),
                             _mm_loadu_si128((const __m128i *)(p + n))),
                _mm_or_si128(_mm_loadu_si128((const __m128i *)(p + 2 * n)),
                             _mm_loadu_si128((const __m128i *)(p + 3 * n))));
            unsigned z = (unsigned)_mm_movemask_pd(_mm_castsi128_pd(
                _mm_cmpeq_epi64(v, _mm_setzero_si128())));
            m |= (uint64_t)(~z & 3) << ((y >> 2) * sb_dim);
        }
        return m;
    }
    for (int y = 0; y < n; y += 4) {
        for (int x = 0; x < n; x += 16) {
            const int16_t *p = blk + y * n + x;
            __m256i v = _mm256_or_si256(
                _mm256_or_si256(_mm256_loadu_si256((const __m256i *)p),
                                _mm256_loadu_si256((const __m256i *)(p + n))),
                _mm256_or_si256(
                    _mm256_loadu_si256((const __m256i *)(p + 2 * n)),
                    _mm256_loadu_si256((const __m256i *)(p + 3 * n))));
            unsigned z = (unsigned)_mm256_movemask_pd(_mm256_castsi256_pd(
                _mm256_cmpeq_epi64(v, _mm256_setzero_si256())));
            m |= (uint64_t)(~z & 15) << ((y >> 2) * sb_dim + (x >> 2));
        }
    }
    return m;
}
