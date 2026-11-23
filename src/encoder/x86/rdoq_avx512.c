/*
 * rdoq_avx512.c - the elementwise half of the RDOQ lattice (N265_ASM_QUANT)
 * on AVX-512 BW, at both depths.
 *
 * rdoq_avx2.c's arithmetic and rdoq_neon.c's range proofs, 32 int16
 * coefficients per vector. q <= 26214 fits a u16, so |coef| * q is formed
 * as the mullo/mulhi pair and interleaved to int32; the in-lane packs then
 * undo the interleave, and their signed saturation is the 32767 clamp.
 * pabsw maps -32768 to 0x8000, which the unsigned multiply reads as 32768.
 *
 * A sub-block row (four rows of the block) is n / 8 vectors at n >= 8;
 * their nonzero masks are folded to n lanes, then one bit per 4 lanes.
 * At n = 4 the 16 coefficients go through a masked load and store.
 *
 * Copyright (c) 2026, the yah265 authors. GPL-2.0-or-later; see LICENSE.
 */

#include <immintrin.h>

#include "encoder/rdoq.h"
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

typedef struct {
    __m512i q, add, lo32, acc, sh;
} rtn_ctx;

AI __m512i rtn32(rtn_ctx *k, __m512i c)
{
    __m512i a = _mm512_abs_epi16(c);
    __m512i lo = _mm512_mullo_epi16(a, k->q);
    __m512i hi = _mm512_mulhi_epu16(a, k->q);
    __m512i p0 = _mm512_add_epi32(_mm512_unpacklo_epi16(lo, hi), k->add);
    __m512i p1 = _mm512_add_epi32(_mm512_unpackhi_epi16(lo, hi), k->add);
    /* pair sums of squares, read as uint32 */
    __m512i e = _mm512_madd_epi16(c, c);

    k->acc = _mm512_add_epi64(k->acc, _mm512_and_si512(e, k->lo32));
    k->acc = _mm512_add_epi64(k->acc, _mm512_srli_epi64(e, 32));
    return _mm512_packs_epi32(_mm512_srlv_epi32(p0, k->sh),
                              _mm512_srlv_epi32(p1, k->sh));
}

int n265_rdoq_rtn_avx512(int16_t *rtn, double *all_zero, uint64_t *sb_nz,
                         const int16_t *coef, int log2n, int q, int64_t add,
                         int qbits, int czero)
{
    const int n = 1 << log2n, sb_dim = n >> 2;
    rtn_ctx k = {
        _mm512_set1_epi16((int16_t)q), _mm512_set1_epi32((int32_t)add),
        _mm512_set1_epi64(0xffffffff), _mm512_setzero_si512(),
        _mm512_set1_epi32(qbits),
    };
    uint64_t m = 0;

    (void)czero;
    if (n == 4) {
        __m512i l = rtn32(&k, _mm512_maskz_loadu_epi16(0xffff, coef));
        _mm512_mask_storeu_epi16(rtn, 0xffff, l);
        m = _mm512_test_epi16_mask(l, l) != 0;
    } else {
        const int per = n >> 3;             /* vectors per sub-block row */
        for (int sy = 0; sy < sb_dim; sy++) {
            const int16_t *c = coef + (size_t)sy * 4 * n;
            int16_t *r = rtn + (size_t)sy * 4 * n;
            __m512i o = _mm512_setzero_si512();
            for (int i = 0; i < per; i++) {
                __m512i l = rtn32(&k, _mm512_loadu_si512(c + 32 * i));
                _mm512_storeu_si512(r + 32 * i, l);
                o = _mm512_or_si512(o, l);
            }
            m |= (uint64_t)sb_bits((uint32_t)_mm512_test_epi16_mask(o, o), n)
                 << (sy * sb_dim);
        }
    }

    *all_zero = (double)(uint64_t)_mm512_reduce_add_epi64(k.acc);
    *sb_nz = m;
    return m != 0;
}
