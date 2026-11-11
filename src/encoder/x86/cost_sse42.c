/*
 * cost_sse42.c - the coefficient-costing kernels (N265_ASM_QUANT) on
 * SSE4.2.
 *
 * cost_avx2.c's kernels in 128-bit registers. A 4x4 group is four 8-byte
 * rows; rows 0-1 and rows 2-3 make two xmm, and one pshufb per row pair
 * (bit 7 of the index zeroes the byte the other pair supplies) puts scan
 * positions 0-7 in one xmm and 8-15 in another. `pabsw` maps -32768 to
 * 0x8000, as the reference's unsigned negation does.
 *
 * Reads: four rows of four int16 per group at its stride, nothing wider.
 *
 * Copyright (c) 2026, the yah265 authors. GPL-2.0-or-later; see LICENSE.
 */

#include <smmintrin.h>
#include <string.h>

#include "encoder/residual.h"
#include "encoder/tables.h"
#include "encoder/x86/sse42.h"

/* the low k bits of nz, one 0/1 byte each */
static inline void put_bytes(uint8_t *out, unsigned nz, int k)
{
    for (int i = 0; i < k; i++)
        out[i] = (uint8_t)((nz >> i) & 1);
}

/* the four rows of 8 int16 at p, OR'd */
static inline __m128i or4(const int16_t *p, int n)
{
    return _mm_or_si128(
        _mm_or_si128(_mm_loadu_si128((const __m128i *)p),
                     _mm_loadu_si128((const __m128i *)(p + n))),
        _mm_or_si128(_mm_loadu_si128((const __m128i *)(p + 2 * n)),
                     _mm_loadu_si128((const __m128i *)(p + 3 * n))));
}

void n265_csbf_map_sse42(const int16_t *level, int log2n, uint8_t *out)
{
    int n = 1 << log2n, sb_dim = n >> 2;

    if (log2n == 2) {
        __m128i v = _mm_or_si128(_mm_loadu_si128((const __m128i *)level),
                                 _mm_loadu_si128((const __m128i *)(level + 8)));
        out[0] = !_mm_testz_si128(v, v);
        return;
    }
    for (int b = 0; b < sb_dim; b++) {
        const int16_t *r = level + (size_t)(b << 2) * n;
        uint8_t *o = out + b * sb_dim;

        for (int x = 0; x < n; x += 8) {
            __m128i v = or4(r + x, n);
            unsigned z = (unsigned)_mm_movemask_pd(_mm_castsi128_pd(
                _mm_cmpeq_epi64(v, _mm_setzero_si128())));
            put_bytes(o + (x >> 2), ~z & 3, 2);
        }
    }
}

/* byte indices into the 32-byte group (row y, column x at y * 8 + x * 2),
 * scan positions 0-7 first and 8-15 after */
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

/* pshufb indices for one 8-position half: r01 picks bytes 0-15, r23
 * bytes 16-31 */
typedef struct { __m128i lo[2], hi[2]; } scan_idx_t;

static inline void scan_sel(int scan_idx, scan_idx_t *s)
{
    const __m128i b4 = _mm_set1_epi8(0x10);
    for (int h = 0; h < 2; h++) {
        __m128i t = _mm_loadu_si128((const __m128i *)
                                    (scan_gidx[scan_idx] + 16 * h));
        __m128i th = _mm_xor_si128(t, b4);
        s->lo[h] = _mm_or_si128(t, _mm_slli_epi16(_mm_and_si128(t, b4), 3));
        s->hi[h] = _mm_or_si128(th,
                                _mm_slli_epi16(_mm_and_si128(th, b4), 3));
    }
}

static inline __m128i ld_rows2(const int16_t *p, ptrdiff_t stride)
{
    return _mm_unpacklo_epi64(_mm_loadl_epi64((const __m128i *)p),
                              _mm_loadl_epi64((const __m128i *)(p + stride)));
}

static inline __m128i gather_scan(__m128i r01, __m128i r23,
                                  const scan_idx_t *s, int h)
{
    return _mm_or_si128(_mm_shuffle_epi8(r01, s->lo[h]),
                        _mm_shuffle_epi8(r23, s->hi[h]));
}

/* significance and sign masks of the sixteen levels in scan order */
static inline void sig_neg(__m128i v0, __m128i v1, uint32_t *sig,
                           uint32_t *neg)
{
    const __m128i z = _mm_setzero_si128();
    uint32_t eq = (uint32_t)_mm_movemask_epi8(
        _mm_packs_epi16(_mm_cmpeq_epi16(v0, z), _mm_cmpeq_epi16(v1, z)));

    *sig = ~eq & 0xffff;
    *neg = (uint32_t)_mm_movemask_epi8(_mm_packs_epi16(v0, v1));
}

uint32_t n265_coeff_group_sse42(const int16_t *grp, ptrdiff_t stride,
                                int scan_idx, uint16_t *abs_out,
                                uint32_t *neg_mask)
{
    scan_idx_t s;
    uint32_t sig, neg;

    scan_sel(scan_idx, &s);
    __m128i r01 = ld_rows2(grp, stride);
    __m128i r23 = ld_rows2(grp + 2 * stride, stride);
    __m128i v0 = gather_scan(r01, r23, &s, 0);
    __m128i v1 = gather_scan(r01, r23, &s, 1);
    _mm_storeu_si128((__m128i *)abs_out, _mm_abs_epi16(v0));
    _mm_storeu_si128((__m128i *)(abs_out + 8), _mm_abs_epi16(v1));
    sig_neg(v0, v1, &sig, &neg);
    *neg_mask = neg;
    return sig;
}

/* Parity of the sum of magnitudes is the parity of the count of odd
 * levels, read off bit 0 of each lane. */
void n265_sbh_adjust_sse42(int16_t *level, int log2n, int scan_idx)
{
    int n = 1 << log2n;
    int sb_dim = n >> 2;
    int num_sb = sb_dim * sb_dim;
    const uint8_t *sb_scan = n265_scan_sb[scan_idx][log2n - 2];
    const uint8_t *co_scan = n265_scan_coef[scan_idx];
    scan_idx_t s;

    scan_sel(scan_idx, &s);
    for (int b = 0; b < num_sb; b++) {
        int sx = (sb_scan[b] & 15) << 2, sy = (sb_scan[b] >> 4) << 2;
        const int16_t *g = level + (size_t)sy * n + sx;
        __m128i r01 = ld_rows2(g, n), r23 = ld_rows2(g + 2 * n, n);
        __m128i any = _mm_or_si128(r01, r23);
        uint32_t sig, neg, odd;
        int first, last, want, par;

        if (_mm_testz_si128(any, any))
            continue;
        __m128i v0 = gather_scan(r01, r23, &s, 0);
        __m128i v1 = gather_scan(r01, r23, &s, 1);
        sig_neg(v0, v1, &sig, &neg);
        first = __builtin_ctz(sig);
        last = 31 - __builtin_clz(sig);
        if (last - first <= 3)
            continue;

        odd = (uint32_t)_mm_movemask_epi8(_mm_slli_epi16(v0, 15)) |
              ((uint32_t)_mm_movemask_epi8(_mm_slli_epi16(v1, 15)) << 16);
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
