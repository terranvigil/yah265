/*
 * cost_avx2.c - the coefficient-costing kernels (N265_ASM_QUANT) on AVX2.
 *
 * The entry points are src/encoder/arm/neon.h's cost block, renamed; the
 * shape survey and the range arguments are cost_neon.c's.
 *
 * A 4x4 group is four 8-byte rows. Rows 0-1 and rows 2-3 make two xmm
 * halves, each broadcast to both ymm lanes, so one in-lane pshufb per half
 * (bit 7 of the index zeroes the byte the other half supplies) puts all
 * sixteen levels in scan order in one ymm. `pabsw` maps -32768 to 0x8000,
 * as the reference's unsigned negation does.
 *
 * Reads: four rows of four int16 per group at its stride, nothing wider.
 *
 * Copyright (c) 2026, the yah265 authors. GPL-2.0-or-later; see LICENSE.
 */

#include <immintrin.h>
#include <string.h>

#include "encoder/residual.h"
#include "encoder/tables.h"
#include "encoder/x86/avx2.h"

/* nonzero bits of a k-bit group mask, one 0/1 byte each */
static inline void put_bytes(uint8_t *out, unsigned nz, int k)
{
    uint64_t b = _pdep_u64(nz, 0x0101010101010101ull);
    memcpy(out, &b, (size_t)k);
}

void n265_csbf_map_avx2(const int16_t *level, int log2n, uint8_t *out)
{
    int n = 1 << log2n, sb_dim = n >> 2;

    if (log2n == 2) {
        __m256i v = _mm256_loadu_si256((const __m256i *)level);
        out[0] = !_mm256_testz_si256(v, v);
        return;
    }
    for (int b = 0; b < sb_dim; b++) {
        const int16_t *r = level + (size_t)(b << 2) * n;
        uint8_t *o = out + b * sb_dim;

        if (log2n == 3) {
            __m128i v = _mm_or_si128(
                _mm_or_si128(_mm_loadu_si128((const __m128i *)r),
                             _mm_loadu_si128((const __m128i *)(r + n))),
                _mm_or_si128(_mm_loadu_si128((const __m128i *)(r + 2 * n)),
                             _mm_loadu_si128((const __m128i *)(r + 3 * n))));
            unsigned z = (unsigned)_mm_movemask_pd(_mm_castsi128_pd(
                _mm_cmpeq_epi64(v, _mm_setzero_si128())));
            put_bytes(o, ~z & 3, 2);
            continue;
        }
        for (int x = 0; x < n; x += 16) {
            const int16_t *p = r + x;
            __m256i v = _mm256_or_si256(
                _mm256_or_si256(_mm256_loadu_si256((const __m256i *)p),
                                _mm256_loadu_si256((const __m256i *)(p + n))),
                _mm256_or_si256(
                    _mm256_loadu_si256((const __m256i *)(p + 2 * n)),
                    _mm256_loadu_si256((const __m256i *)(p + 3 * n))));
            unsigned z = (unsigned)_mm256_movemask_pd(_mm256_castsi256_pd(
                _mm256_cmpeq_epi64(v, _mm256_setzero_si256())));
            put_bytes(o + (x >> 2), ~z & 15, 4);
        }
    }
}

/* byte indices into the 32-byte group (row y, column x at y * 8 + x * 2),
 * scan positions 0-7 in the low lane and 8-15 in the high one */
static const uint8_t scan_gidx[3][32] = {
    { /* diag */
         0,  1,  8,  9,  2,  3, 16, 17, 10, 11,  4,  5, 24, 25, 18, 19,
        12, 13,  6,  7, 26, 27, 20, 21, 14, 15, 28, 29, 22, 23, 30, 31,
    },
    { /* horizontal */
         0,  1,  2,  3,  4,  5,  6,  7,  8,  9, 10, 11, 12, 13, 14, 15,
        16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27, 28, 29, 30, 31,
    },
    { /* vertical */
         0,  1,  8,  9, 16, 17, 24, 25,  2,  3, 10, 11, 18, 19, 26, 27,
         4,  5, 12, 13, 20, 21, 28, 29,  6,  7, 14, 15, 22, 23, 30, 31,
    },
};

/* pshufb indices for the two halves: bit 4 of an index picks the half */
static inline void scan_sel(int scan_idx, __m256i *lo, __m256i *hi)
{
    __m256i t = _mm256_loadu_si256((const __m256i *)scan_gidx[scan_idx]);
    __m256i b4 = _mm256_set1_epi8(0x10);
    __m256i th = _mm256_xor_si256(t, b4);

    *lo = _mm256_or_si256(t, _mm256_slli_epi16(_mm256_and_si256(t, b4), 3));
    *hi = _mm256_or_si256(th, _mm256_slli_epi16(_mm256_and_si256(th, b4), 3));
}

static inline __m256i ld_rows2(const int16_t *p, ptrdiff_t stride)
{
    __m128i x = _mm_unpacklo_epi64(_mm_loadl_epi64((const __m128i *)p),
                                   _mm_loadl_epi64((const __m128i *)(p + stride)));
    return _mm256_broadcastsi128_si256(x);
}

static inline __m256i gather_scan(__m256i r01, __m256i r23, __m256i lo,
                                  __m256i hi)
{
    return _mm256_or_si256(_mm256_shuffle_epi8(r01, lo),
                           _mm256_shuffle_epi8(r23, hi));
}

/* significance and sign masks of sixteen int16 lanes in scan order */
static inline void sig_neg(__m256i v, uint32_t *sig, uint32_t *neg)
{
    __m256i eq = _mm256_cmpeq_epi16(v, _mm256_setzero_si256());
    uint32_t m = (uint32_t)_mm256_movemask_epi8(_mm256_packs_epi16(eq, v));

    *sig = ~((m & 0xff) | ((m >> 8) & 0xff00)) & 0xffff;
    *neg = ((m >> 8) & 0xff) | ((m >> 16) & 0xff00);
}

uint32_t n265_coeff_group_avx2(const int16_t *grp, ptrdiff_t stride,
                               int scan_idx, uint16_t *abs_out,
                               uint32_t *neg_mask)
{
    __m256i lo, hi, v;
    uint32_t sig, neg;

    scan_sel(scan_idx, &lo, &hi);
    v = gather_scan(ld_rows2(grp, stride), ld_rows2(grp + 2 * stride, stride),
                    lo, hi);
    _mm256_storeu_si256((__m256i *)abs_out, _mm256_abs_epi16(v));
    sig_neg(v, &sig, &neg);
    *neg_mask = neg;
    return sig;
}

/* Parity of the sum of magnitudes is the parity of the count of odd
 * levels, read off bit 0 of each lane. */
void n265_sbh_adjust_avx2(int16_t *level, int log2n, int scan_idx)
{
    int n = 1 << log2n;
    int sb_dim = n >> 2;
    int num_sb = sb_dim * sb_dim;
    const uint8_t *sb_scan = n265_scan_sb[scan_idx][log2n - 2];
    const uint8_t *co_scan = n265_scan_coef[scan_idx];
    __m256i lo, hi;

    scan_sel(scan_idx, &lo, &hi);
    for (int s = 0; s < num_sb; s++) {
        int sx = (sb_scan[s] & 15) << 2, sy = (sb_scan[s] >> 4) << 2;
        const int16_t *g = level + (size_t)sy * n + sx;
        __m256i r01 = ld_rows2(g, n), r23 = ld_rows2(g + 2 * n, n);
        __m256i any = _mm256_or_si256(r01, r23);
        uint32_t sig, neg, odd;
        int first, last, want, par;

        if (_mm256_testz_si256(any, any))
            continue;
        __m256i v = gather_scan(r01, r23, lo, hi);
        sig_neg(v, &sig, &neg);
        first = __builtin_ctz(sig);
        last = 31 - __builtin_clz(sig);
        if (last - first <= 3)
            continue;

        odd = (uint32_t)_mm256_movemask_epi8(_mm256_slli_epi16(v, 15));
        par = __builtin_popcount(odd & 0xaaaaaaaau) & 1;
        want = (int)((neg >> first) & 1);
        if (par != want) {
            int lx = sx + (co_scan[last] & 15);
            int ly = sy + (co_scan[last] >> 4);
            int16_t *p = &level[(size_t)ly * n + lx];

            *p += *p > 0 ? 1 : -1;
        }
    }
}
