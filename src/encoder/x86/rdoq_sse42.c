/*
 * rdoq_sse42.c - the elementwise half of the RDOQ lattice (N265_ASM_QUANT)
 * on SSE4.2.
 *
 * rdoq_avx2.c's kernels with eight coefficients as two 4-lane int32
 * halves. rdoq_neon.c's header holds the proofs: the energy is an exact
 * integer below 2^53 in any summation order, and |coef| * q + add fits
 * int32. A pmaddwd pair sum of two -32768 squares is 2^31, right only as
 * uint32, so the pair sums are zero-extended before the int64 adds.
 *
 * Copyright (c) 2026, the yah265 authors. GPL-2.0-or-later; see LICENSE.
 */

#include <smmintrin.h>

#include "encoder/rdoq.h"
#include "encoder/x86/sse42.h"

typedef struct {
    __m128i q, add, max, acc;
    __m128i sh;
} rtn_ctx;

static inline __m128i level4(const rtn_ctx *k, __m128i c16)
{
    __m128i c = _mm_abs_epi32(_mm_cvtepi16_epi32(c16));
    __m128i l = _mm_add_epi32(_mm_mullo_epi32(c, k->q), k->add);
    return _mm_min_epi32(_mm_srl_epi32(l, k->sh), k->max);
}

/* eight coefficients: their levels stored as int16, and returned as int32
 * in lo (the first four) and hi */
static inline void rtn8(rtn_ctx *k, int16_t *rtn, const int16_t *coef,
                        __m128i *lo, __m128i *hi)
{
    __m128i c16 = _mm_loadu_si128((const __m128i *)coef);
    __m128i l = level4(k, c16), h = level4(k, _mm_srli_si128(c16, 8));
    __m128i sq = _mm_madd_epi16(c16, c16);

    k->acc = _mm_add_epi64(k->acc, _mm_cvtepu32_epi64(sq));
    k->acc = _mm_add_epi64(k->acc, _mm_cvtepu32_epi64(_mm_srli_si128(sq, 8)));
    _mm_storeu_si128((__m128i *)rtn, _mm_packs_epi32(l, h));
    *lo = l;
    *hi = h;
}

int n265_rdoq_rtn_sse42(int16_t *rtn, double *all_zero, uint64_t *sb_nz,
                        const int16_t *coef, int log2n, int q, int64_t add,
                        int qbits, int czero)
{
    const int n = 1 << log2n, sb_dim = n >> 2;
    rtn_ctx k = {
        _mm_set1_epi32(q), _mm_set1_epi32((int32_t)add),
        _mm_set1_epi32(32767), _mm_setzero_si128(),
        _mm_cvtsi32_si128(qbits),
    };
    uint64_t m = 0;

    (void)czero;
    if (n == 4) {
        __m128i a, b, c, d;
        rtn8(&k, rtn, coef, &a, &b);
        rtn8(&k, rtn + 8, coef + 8, &c, &d);
        __m128i any = _mm_or_si128(_mm_or_si128(a, b), _mm_or_si128(c, d));
        m = !_mm_testz_si128(any, any);
    } else {
        for (int y = 0; y < n; y += 4) {
            for (int x = 0; x < n; x += 8) {
                __m128i ol = _mm_setzero_si128(), oh = ol;
                int sb = (y >> 2) * sb_dim + (x >> 2);

                for (int r = 0; r < 4; r++) {
                    int idx = (y + r) * n + x;
                    __m128i l, h;
                    rtn8(&k, rtn + idx, coef + idx, &l, &h);
                    ol = _mm_or_si128(ol, l);
                    oh = _mm_or_si128(oh, h);
                }
                if (!_mm_testz_si128(ol, ol))
                    m |= (uint64_t)1 << sb;
                if (!_mm_testz_si128(oh, oh))
                    m |= (uint64_t)1 << (sb + 1);
            }
        }
    }

    *all_zero = (double)(uint64_t)(_mm_cvtsi128_si64(k.acc) +
                                   _mm_extract_epi64(k.acc, 1));
    *sb_nz = m;
    return m != 0;
}

/* the four rows of 8 int16 at p, OR'd: lane 0 is one sub-block, lane 1
 * the next */
static inline __m128i or4(const int16_t *p, int n)
{
    return _mm_or_si128(
        _mm_or_si128(_mm_loadu_si128((const __m128i *)p),
                     _mm_loadu_si128((const __m128i *)(p + n))),
        _mm_or_si128(_mm_loadu_si128((const __m128i *)(p + 2 * n)),
                     _mm_loadu_si128((const __m128i *)(p + 3 * n))));
}

uint64_t n265_rdoq_sb_nz_sse42(const int16_t *blk, int log2n)
{
    const int n = 1 << log2n, sb_dim = n >> 2;
    uint64_t m = 0;

    if (n == 4) {
        __m128i v = _mm_or_si128(_mm_loadu_si128((const __m128i *)blk),
                                 _mm_loadu_si128((const __m128i *)(blk + 8)));
        return !_mm_testz_si128(v, v);
    }
    for (int y = 0; y < n; y += 4) {
        for (int x = 0; x < n; x += 8) {
            __m128i v = or4(blk + y * n + x, n);
            unsigned z = (unsigned)_mm_movemask_pd(_mm_castsi128_pd(
                _mm_cmpeq_epi64(v, _mm_setzero_si128())));
            m |= (uint64_t)(~z & 3) << ((y >> 2) * sb_dim + (x >> 2));
        }
    }
    return m;
}
