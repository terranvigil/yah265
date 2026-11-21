/*
 * satd_avx512.c - the SATD / SAD / SSD family (N265_ASM_SATD) on
 * AVX-512 BW and VL, at both depths.
 *
 * The method is satd_avx2.c's in 32 int16 lanes. Every 128-bit lane is
 * one tile row, so a zmm row vector holds four 8x8 tiles side by side
 * (32 columns), or two 16-column halves of rows 8 apart, or four
 * 8-column quarters of rows 8 apart; the vertical transform then runs
 * across the row vectors as before and each 128-bit lane ends as one
 * tile. The horizontal radix-2 stages take their partner with a rotate
 * (pairs and quads) or an in-lane pshufd (octets), and a masked subtract
 * in place of AVX2's psignw, which AVX-512 lacks.
 *
 * The ranges are satd_avx2.c's. A shape this file doesn't widen goes to
 * the AVX2 kernel, which is exact on every shape, so a sum split between
 * the two is still the C's.
 *
 * Masked loads read only the samples of the block, so a row tail costs
 * no scalar loop and the page guard holds.
 *
 * Copyright (c) 2026, the yah265 authors. GPL-2.0-or-later; see LICENSE.
 */

#include <immintrin.h>
#include <string.h>

#include "encoder/satd.h"
#include "encoder/x86/avx2.h"
#include "encoder/x86/avx512.h"

static inline __mmask32 lanes32(int n)
{
    return n >= 32 ? 0xffffffffu : (__mmask32)((1u << n) - 1);
}

/* n <= 32 samples of one row, zero-extended to 32 int16 lanes */
static inline __m512i ldm32(const pixel *p, int n)
{
#if BIT_DEPTH == 8
    return _mm512_cvtepu8_epi16(_mm256_maskz_loadu_epi8(lanes32(n), p));
#else
    return _mm512_maskz_loadu_epi16(lanes32(n), p);
#endif
}

/* n <= 16 samples of rows p and q, in the low and high halves */
static inline __m512i ldm2x16(const pixel *p, const pixel *q, int n)
{
    __mmask16 m = (__mmask16)lanes32(n);
#if BIT_DEPTH == 8
    return _mm512_cvtepu8_epi16(_mm256_inserti128_si256(
        _mm256_castsi128_si256(_mm_maskz_loadu_epi8(m, p)),
        _mm_maskz_loadu_epi8(m, q), 1));
#else
    return _mm512_inserti64x4(
        _mm512_castsi256_si512(_mm256_maskz_loadu_epi16(m, p)),
        _mm256_maskz_loadu_epi16(m, q), 1);
#endif
}

static inline __m512i ld32(const pixel *p)
{
#if BIT_DEPTH == 8
    return _mm512_cvtepu8_epi16(_mm256_loadu_si256((const __m256i *)p));
#else
    return _mm512_loadu_si512(p);
#endif
}

static inline __m512i ld2x16(const pixel *p, const pixel *q)
{
#if BIT_DEPTH == 8
    return _mm512_cvtepu8_epi16(_mm256_inserti128_si256(
        _mm256_castsi128_si256(_mm_loadu_si128((const __m128i *)p)),
        _mm_loadu_si128((const __m128i *)q), 1));
#else
    return _mm512_inserti64x4(
        _mm512_castsi256_si512(_mm256_loadu_si256((const __m256i *)p)),
        _mm256_loadu_si256((const __m256i *)q), 1);
#endif
}

/* 8 samples of each of four rows `st` apart */
static inline __m512i ld4x8(const pixel *p, ptrdiff_t st)
{
#if BIT_DEPTH == 8
    __m128i lo = _mm_unpacklo_epi64(
        _mm_loadl_epi64((const __m128i *)p),
        _mm_loadl_epi64((const __m128i *)(p + st)));
    __m128i hi = _mm_unpacklo_epi64(
        _mm_loadl_epi64((const __m128i *)(p + 2 * st)),
        _mm_loadl_epi64((const __m128i *)(p + 3 * st)));
    return _mm512_cvtepu8_epi16(
        _mm256_inserti128_si256(_mm256_castsi128_si256(lo), hi, 1));
#else
    __m256i lo = _mm256_inserti128_si256(
        _mm256_castsi128_si256(_mm_loadu_si128((const __m128i *)p)),
        _mm_loadu_si128((const __m128i *)(p + st)), 1);
    __m256i hi = _mm256_inserti128_si256(
        _mm256_castsi128_si256(_mm_loadu_si128((const __m128i *)(p + 2 * st))),
        _mm_loadu_si128((const __m128i *)(p + 3 * st)), 1);
    return _mm512_inserti64x4(_mm512_castsi256_si512(lo), hi, 1);
#endif
}

#if BIT_DEPTH == 8
/* 4 bytes of each of four rows */
static inline __m128i ld4x4b(const pixel *p, ptrdiff_t st)
{
    int32_t r0, r1, r2, r3;
    memcpy(&r0, p, 4);
    memcpy(&r1, p + st, 4);
    memcpy(&r2, p + 2 * st, 4);
    memcpy(&r3, p + 3 * st, 4);
    return _mm_setr_epi32(r0, r1, r2, r3);
}
#endif

/* 4 samples of each of four rows, as 16 int16 lanes */
static inline __m256i ld4x4(const pixel *p, ptrdiff_t st)
{
#if BIT_DEPTH == 8
    return _mm256_cvtepu8_epi16(ld4x4b(p, st));
#else
    __m128i lo = _mm_unpacklo_epi64(
        _mm_loadl_epi64((const __m128i *)p),
        _mm_loadl_epi64((const __m128i *)(p + st)));
    __m128i hi = _mm_unpacklo_epi64(
        _mm_loadl_epi64((const __m128i *)(p + 2 * st)),
        _mm_loadl_epi64((const __m128i *)(p + 3 * st)));
    return _mm256_inserti128_si256(_mm256_castsi128_si256(lo), hi, 1);
#endif
}

/* radix-2 stages inside each 2-, 4- and 8-lane group: sum low, difference high */
static inline __m512i hstage1(__m512i v)
{
    __m512i s = _mm512_rol_epi32(v, 16);
    return _mm512_mask_sub_epi16(_mm512_add_epi16(v, s), 0xaaaaaaaau, s, v);
}

static inline __m512i hstage2(__m512i v)
{
    __m512i s = _mm512_rol_epi64(v, 32);
    return _mm512_mask_sub_epi16(_mm512_add_epi16(v, s), 0xccccccccu, s, v);
}

static inline __m512i hstage4(__m512i v)
{
    __m512i s = _mm512_shuffle_epi32(v, (_MM_PERM_ENUM)0x4e);
    return _mm512_mask_sub_epi16(_mm512_add_epi16(v, s), 0xf0f0f0f0u, s, v);
}

/* Eight row vectors of differences, four 8x8 tiles, one per 128-bit
 * lane: every dword of a lane ends as that tile's rounded SATD. */
static inline __m512i tiles8(__m512i r[8])
{
    const __m512i one = _mm512_set1_epi16(1);
    for (int y = 0; y < 8; y++)
        r[y] = hstage4(hstage2(hstage1(r[y])));
    for (int s = 1; s < 4; s <<= 1)
        for (int i = 0; i < 8; i += s << 1)
            for (int j = i; j < i + s; j++) {
                __m512i t = r[j];
                r[j] = _mm512_add_epi16(t, r[j + s]);
                r[j + s] = _mm512_sub_epi16(t, r[j + s]);
            }
    /* the last vertical stage, as |a + b| + |a - b| = 2 * max(|a|, |b|) */
    __m512i m = _mm512_setzero_si512();
    for (int y = 0; y < 4; y++)
        m = _mm512_add_epi32(m, _mm512_madd_epi16(
            _mm512_max_epi16(_mm512_abs_epi16(r[y]),
                             _mm512_abs_epi16(r[y + 4])), one));
    m = _mm512_slli_epi32(m, 1);
    m = _mm512_add_epi32(m, _mm512_shuffle_epi32(m, (_MM_PERM_ENUM)0x4e));
    m = _mm512_add_epi32(m, _mm512_shuffle_epi32(m, (_MM_PERM_ENUM)0xb1));
    return _mm512_srli_epi32(_mm512_add_epi32(m, _mm512_set1_epi32(2)), 2);
}

/* 8 rows x 32 columns */
static inline __m512i band32(const pixel *a, int as, const pixel *b, int bs)
{
    __m512i r[8];
    for (int y = 0; y < 8; y++)
        r[y] = _mm512_sub_epi16(ld32(a + (ptrdiff_t)y * as),
                                ld32(b + (ptrdiff_t)y * bs));
    return tiles8(r);
}

/* 16 rows x 16 columns */
static inline __m512i block16(const pixel *a, int as, const pixel *b, int bs)
{
    __m512i r[8];
    for (int y = 0; y < 8; y++) {
        const pixel *p = a + (ptrdiff_t)y * as, *q = b + (ptrdiff_t)y * bs;
        r[y] = _mm512_sub_epi16(ld2x16(p, p + 8 * (ptrdiff_t)as),
                                ld2x16(q, q + 8 * (ptrdiff_t)bs));
    }
    return tiles8(r);
}

/* 32 rows x 8 columns */
static inline __m512i block8x32(const pixel *a, int as, const pixel *b,
                                int bs)
{
    __m512i r[8];
    for (int y = 0; y < 8; y++)
        r[y] = _mm512_sub_epi16(ld4x8(a + (ptrdiff_t)y * as, 8 * (ptrdiff_t)as),
                                ld4x8(b + (ptrdiff_t)y * bs, 8 * (ptrdiff_t)bs));
    return tiles8(r);
}

/* 8 rows x 16 columns, rows y and y + 4 in one vector, so the last
 * vertical stage pairs the two 256-bit halves */
static inline __m512i block16x8(const pixel *a, int as, const pixel *b,
                                int bs)
{
    const __m512i one = _mm512_set1_epi16(1);
    __m512i r[4];
    for (int y = 0; y < 4; y++) {
        const pixel *p = a + (ptrdiff_t)y * as, *q = b + (ptrdiff_t)y * bs;
        r[y] = hstage4(hstage2(hstage1(_mm512_sub_epi16(
            ld2x16(p, p + 4 * (ptrdiff_t)as),
            ld2x16(q, q + 4 * (ptrdiff_t)bs)))));
    }
    for (int s = 1; s < 4; s <<= 1)
        for (int i = 0; i < 4; i += s << 1)
            for (int j = i; j < i + s; j++) {
                __m512i t = r[j];
                r[j] = _mm512_add_epi16(t, r[j + s]);
                r[j + s] = _mm512_sub_epi16(t, r[j + s]);
            }
    /* both halves hold max(|a|, |b|) of a pair, so their sum is the
     * pair's |a + b| + |a - b| */
    __m512i m = _mm512_setzero_si512();
    for (int y = 0; y < 4; y++) {
        __m512i v = _mm512_abs_epi16(r[y]);
        m = _mm512_add_epi32(m, _mm512_madd_epi16(
            _mm512_max_epi16(v, _mm512_shuffle_i64x2(v, v, 0x4e)), one));
    }
    m = _mm512_add_epi32(m, _mm512_shuffle_i64x2(m, m, 0x4e));
    m = _mm512_add_epi32(m, _mm512_shuffle_epi32(m, (_MM_PERM_ENUM)0x4e));
    m = _mm512_add_epi32(m, _mm512_shuffle_epi32(m, (_MM_PERM_ENUM)0xb1));
    m = _mm512_srli_epi32(_mm512_add_epi32(m, _mm512_set1_epi32(2)), 2);
    /* one tile per 128-bit lane of the low half */
    return _mm512_maskz_mov_epi32(0x00ff, m);
}

static inline __m256i hstage1_y(__m256i v)
{
    __m256i s = _mm256_rol_epi32(v, 16);
    return _mm256_mask_sub_epi16(_mm256_add_epi16(v, s), 0xaaaau, s, v);
}

static inline __m256i hstage2_y(__m256i v)
{
    __m256i s = _mm256_rol_epi64(v, 32);
    return _mm256_mask_sub_epi16(_mm256_add_epi16(v, s), 0xccccu, s, v);
}

static inline __m256i hstage4_y(__m256i v)
{
    __m256i s = _mm256_shuffle_epi32(v, 0x4e);
    return _mm256_mask_sub_epi16(_mm256_add_epi16(v, s), 0xf0f0u, s, v);
}

/* 8 samples of rows p and q, in the low and high 128-bit halves */
static inline __m256i ld2x8(const pixel *p, const pixel *q)
{
#if BIT_DEPTH == 8
    return _mm256_cvtepu8_epi16(_mm_unpacklo_epi64(
        _mm_loadl_epi64((const __m128i *)p), _mm_loadl_epi64((const __m128i *)q)));
#else
    return _mm256_inserti128_si256(
        _mm256_castsi128_si256(_mm_loadu_si128((const __m128i *)p)),
        _mm_loadu_si128((const __m128i *)q), 1);
#endif
}

/* one 8x8 tile in 256 bits: block16x8's layout at half the width */
static inline uint32_t tile8x8(const pixel *a, int as, const pixel *b, int bs)
{
    const __m256i one = _mm256_set1_epi16(1);
    __m256i r[4];
    for (int y = 0; y < 4; y++) {
        const pixel *p = a + (ptrdiff_t)y * as, *q = b + (ptrdiff_t)y * bs;
        r[y] = hstage4_y(hstage2_y(hstage1_y(_mm256_sub_epi16(
            ld2x8(p, p + 4 * (ptrdiff_t)as), ld2x8(q, q + 4 * (ptrdiff_t)bs)))));
    }
    for (int s = 1; s < 4; s <<= 1)
        for (int i = 0; i < 4; i += s << 1)
            for (int j = i; j < i + s; j++) {
                __m256i t = r[j];
                r[j] = _mm256_add_epi16(t, r[j + s]);
                r[j + s] = _mm256_sub_epi16(t, r[j + s]);
            }
    __m256i m = _mm256_setzero_si256();
    for (int y = 0; y < 4; y++) {
        __m256i v = _mm256_abs_epi16(r[y]);
        m = _mm256_add_epi32(m, _mm256_madd_epi16(
            _mm256_max_epi16(v, _mm256_permute4x64_epi64(v, 0x4e)), one));
    }
    __m128i s = _mm_add_epi32(_mm256_castsi256_si128(m),
                              _mm256_extracti128_si256(m, 1));
    s = _mm_add_epi32(s, _mm_shuffle_epi32(s, 0x4e));
    s = _mm_add_epi32(s, _mm_shuffle_epi32(s, 0xb1));
    return ((uint32_t)_mm_cvtsi128_si32(s) + 2) >> 2;
}

/* The 8x8 tiling only; a shape with a 4-sample side is the AVX2 kernel's.
 * Every tile is summed once, so any split of the block is exact. */
uint32_t n265_satd_wh_avx512(const pixel *a, int as, const pixel *b, int bs,
                             int w, int h)
{
    if ((w & 7) || (h & 7))
        return n265_satd_wh_avx2(a, as, b, bs, w, h);
    if (w == 8 && h == 8)
        return tile8x8(a, as, b, bs);
    __m512i acc = _mm512_setzero_si512();
    uint32_t rest = 0;
    int x = 0;
    for (; x + 32 <= w; x += 32)
        for (int y = 0; y < h; y += 8)
            acc = _mm512_add_epi32(acc, band32(a + (ptrdiff_t)y * as + x, as,
                                               b + (ptrdiff_t)y * bs + x, bs));
    if (w - x >= 16) {
        int y = 0;
        for (; y + 16 <= h; y += 16)
            acc = _mm512_add_epi32(acc, block16(a + (ptrdiff_t)y * as + x, as,
                                                b + (ptrdiff_t)y * bs + x, bs));
        if (y < h)
            acc = _mm512_add_epi32(acc, block16x8(a + (ptrdiff_t)y * as + x,
                                                  as, b + (ptrdiff_t)y * bs + x,
                                                  bs));
        x += 16;
    }
    if (x < w) {
        int y = 0;
        for (; y + 32 <= h; y += 32)
            acc = _mm512_add_epi32(acc, block8x32(a + (ptrdiff_t)y * as + x,
                                                  as, b + (ptrdiff_t)y * bs + x,
                                                  bs));
        for (; y < h; y += 8)
            rest += tile8x8(a + (ptrdiff_t)y * as + x, as,
                            b + (ptrdiff_t)y * bs + x, bs);
    }
    /* each tile sits in four dwords */
    return ((uint32_t)_mm512_reduce_add_epi32(acc) >> 2) + rest;
}

/* ---- SAD / SSD ------------------------------------------------------ */

#if BIT_DEPTH == 8
static inline __mmask64 lanes64(int n)
{
    return n >= 64 ? ~0ull : (1ull << n) - 1;
}

/* four rows of n <= 16 bytes, one per 128-bit lane */
static inline __m512i ld4x16m(const pixel *p, ptrdiff_t st, __mmask16 m)
{
    __m512i v = _mm512_castsi128_si512(_mm_maskz_loadu_epi8(m, p));
    v = _mm512_inserti32x4(v, _mm_maskz_loadu_epi8(m, p + st), 1);
    v = _mm512_inserti32x4(v, _mm_maskz_loadu_epi8(m, p + 2 * st), 2);
    return _mm512_inserti32x4(v, _mm_maskz_loadu_epi8(m, p + 3 * st), 3);
}

/* four rows of 8 bytes */
static inline __m256i ld4x8b(const pixel *p, ptrdiff_t st)
{
    __m128i lo = _mm_unpacklo_epi64(_mm_loadl_epi64((const __m128i *)p),
                                    _mm_loadl_epi64((const __m128i *)(p + st)));
    __m128i hi = _mm_unpacklo_epi64(_mm_loadl_epi64((const __m128i *)(p + 2 * st)),
                                    _mm_loadl_epi64((const __m128i *)(p + 3 * st)));
    return _mm256_inserti128_si256(_mm256_castsi128_si256(lo), hi, 1);
}

/* the rows below a multiple of 4 go to the AVX2 kernel */
static uint32_t sad_narrow(const pixel *a, int as, const pixel *b, int bs,
                           int w, int h)
{
    int h4 = h & ~3;
    uint32_t rest = h4 < h ? n265_sad_wh_avx2(a + (ptrdiff_t)h4 * as, as,
                                              b + (ptrdiff_t)h4 * bs, bs,
                                              w, h - h4)
                           : 0;
    if (w == 8) {
        __m256i acc = _mm256_setzero_si256();
        for (int y = 0; y < h4; y += 4)
            acc = _mm256_add_epi64(acc, _mm256_sad_epu8(
                ld4x8b(a + (ptrdiff_t)y * as, as),
                ld4x8b(b + (ptrdiff_t)y * bs, bs)));
        __m128i s = _mm_add_epi64(_mm256_castsi256_si128(acc),
                                  _mm256_extracti128_si256(acc, 1));
        s = _mm_add_epi64(s, _mm_unpackhi_epi64(s, s));
        return (uint32_t)_mm_cvtsi128_si32(s) + rest;
    }
    if (w == 4) {
        __m128i acc = _mm_setzero_si128();
        for (int y = 0; y < h4; y += 4)
            acc = _mm_add_epi64(acc, _mm_sad_epu8(
                ld4x4b(a + (ptrdiff_t)y * as, as),
                ld4x4b(b + (ptrdiff_t)y * bs, bs)));
        acc = _mm_add_epi64(acc, _mm_unpackhi_epi64(acc, acc));
        return (uint32_t)_mm_cvtsi128_si32(acc) + rest;
    }
    __mmask16 m = (__mmask16)lanes32(w);
    __m512i acc = _mm512_setzero_si512();
    for (int y = 0; y < h4; y += 4)
        acc = _mm512_add_epi64(acc, _mm512_sad_epu8(
            ld4x16m(a + (ptrdiff_t)y * as, as, m),
            ld4x16m(b + (ptrdiff_t)y * bs, bs, m)));
    return (uint32_t)_mm512_reduce_add_epi64(acc) + rest;
}

uint32_t n265_sad_wh_avx512(const pixel *a, int as, const pixel *b, int bs,
                            int w, int h)
{
    if (w <= 16) {
        if (h < 4 || (w < 9 && w != 8 && w != 4))
            return n265_sad_wh_avx2(a, as, b, bs, w, h);
        return sad_narrow(a, as, b, bs, w, h);
    }
    __m512i acc = _mm512_setzero_si512();
    if (w > 32) {
        for (int y = 0; y < h; y++) {
            const pixel *p = a + (ptrdiff_t)y * as, *q = b + (ptrdiff_t)y * bs;
            for (int x = 0; x < w; x += 64) {
                __mmask64 m = lanes64(w - x);
                acc = _mm512_add_epi64(acc, _mm512_sad_epu8(
                    _mm512_maskz_loadu_epi8(m, p + x),
                    _mm512_maskz_loadu_epi8(m, q + x)));
            }
        }
    } else {
        __mmask32 m = lanes32(w);
        int y = 0;
        for (; y + 2 <= h; y += 2) {
            const pixel *p = a + (ptrdiff_t)y * as, *q = b + (ptrdiff_t)y * bs;
            __m512i va = _mm512_inserti64x4(
                _mm512_castsi256_si512(_mm256_maskz_loadu_epi8(m, p)),
                _mm256_maskz_loadu_epi8(m, p + as), 1);
            __m512i vb = _mm512_inserti64x4(
                _mm512_castsi256_si512(_mm256_maskz_loadu_epi8(m, q)),
                _mm256_maskz_loadu_epi8(m, q + bs), 1);
            acc = _mm512_add_epi64(acc, _mm512_sad_epu8(va, vb));
        }
        if (y < h) {
            __m256i s = _mm256_sad_epu8(
                _mm256_maskz_loadu_epi8(m, a + (ptrdiff_t)y * as),
                _mm256_maskz_loadu_epi8(m, b + (ptrdiff_t)y * bs));
            acc = _mm512_add_epi64(acc, _mm512_zextsi256_si512(s));
        }
    }
    return (uint32_t)_mm512_reduce_add_epi64(acc);
}
#else
/* |a - b| fits int16 at 10 bits, and 32 of them fit one int16 lane */
uint32_t n265_sad_wh_avx512(const pixel *a, int as, const pixel *b, int bs,
                            int w, int h)
{
    const __m512i one = _mm512_set1_epi16(1);
    if (w == 8 && h >= 4) {
        /* 4 rows a vector, at most 16 vectors to a lane */
        int h4 = h & ~3;
        uint32_t rest = h4 < h ? n265_sad_wh_avx2(a + (ptrdiff_t)h4 * as, as,
                                                  b + (ptrdiff_t)h4 * bs, bs,
                                                  w, h - h4)
                               : 0;
        __m512i acc = _mm512_setzero_si512();
        for (int y0 = 0; y0 < h4; y0 += 64) {
            __m512i a16 = _mm512_setzero_si512();
            for (int y = y0; y < h4 && y < y0 + 64; y += 4)
                a16 = _mm512_add_epi16(a16, _mm512_abs_epi16(_mm512_sub_epi16(
                    ld4x8(a + (ptrdiff_t)y * as, as),
                    ld4x8(b + (ptrdiff_t)y * bs, bs))));
            acc = _mm512_add_epi32(acc, _mm512_madd_epi16(a16, one));
        }
        return (uint32_t)_mm512_reduce_add_epi32(acc) + rest;
    }
    if (w == 4 && h >= 4) {
        int h4 = h & ~3;
        uint32_t rest = h4 < h ? n265_sad_wh_avx2(a + (ptrdiff_t)h4 * as, as,
                                                  b + (ptrdiff_t)h4 * bs, bs,
                                                  w, h - h4)
                               : 0;
        __m256i acc = _mm256_setzero_si256();
        for (int y0 = 0; y0 < h4; y0 += 64) {
            __m256i a16 = _mm256_setzero_si256();
            for (int y = y0; y < h4 && y < y0 + 64; y += 4)
                a16 = _mm256_add_epi16(a16, _mm256_abs_epi16(_mm256_sub_epi16(
                    ld4x4(a + (ptrdiff_t)y * as, as),
                    ld4x4(b + (ptrdiff_t)y * bs, bs))));
            acc = _mm256_add_epi32(acc, _mm256_madd_epi16(
                a16, _mm512_castsi512_si256(one)));
        }
        return (uint32_t)_mm512_reduce_add_epi32(_mm512_zextsi256_si512(acc)) +
               rest;
    }
    if (w <= 8)
        return n265_sad_wh_avx2(a, as, b, bs, w, h);
    __m512i acc = _mm512_setzero_si512(), a16 = _mm512_setzero_si512();
    int k = 0;
    if (w > 16) {
        for (int y = 0; y < h; y++) {
            const pixel *p = a + (ptrdiff_t)y * as, *q = b + (ptrdiff_t)y * bs;
            for (int x = 0; x < w; x += 32) {
                a16 = _mm512_add_epi16(a16, _mm512_abs_epi16(_mm512_sub_epi16(
                    ldm32(p + x, w - x), ldm32(q + x, w - x))));
                if (++k == 32) {
                    acc = _mm512_add_epi32(acc, _mm512_madd_epi16(a16, one));
                    a16 = _mm512_setzero_si512();
                    k = 0;
                }
            }
        }
    } else {
        for (int y = 0; y < h; y += 2) {
            const pixel *p = a + (ptrdiff_t)y * as, *q = b + (ptrdiff_t)y * bs;
            /* a missing second row reads row y again under a zero mask */
            int n2 = y + 1 < h ? w : 0;
            ptrdiff_t da = y + 1 < h ? as : 0, db = y + 1 < h ? bs : 0;
            __m512i va = _mm512_inserti64x4(
                _mm512_castsi256_si512(_mm256_maskz_loadu_epi16(
                    (__mmask16)lanes32(w), p)),
                _mm256_maskz_loadu_epi16((__mmask16)lanes32(n2), p + da), 1);
            __m512i vb = _mm512_inserti64x4(
                _mm512_castsi256_si512(_mm256_maskz_loadu_epi16(
                    (__mmask16)lanes32(w), q)),
                _mm256_maskz_loadu_epi16((__mmask16)lanes32(n2), q + db), 1);
            a16 = _mm512_add_epi16(a16,
                                   _mm512_abs_epi16(_mm512_sub_epi16(va, vb)));
            if (++k == 32) {
                acc = _mm512_add_epi32(acc, _mm512_madd_epi16(a16, one));
                a16 = _mm512_setzero_si512();
                k = 0;
            }
        }
    }
    acc = _mm512_add_epi32(acc, _mm512_madd_epi16(a16, one));
    return (uint32_t)_mm512_reduce_add_epi32(acc);
}
#endif

/* epi32 lanes, folded into epi64 every 32 vectors: one lane takes two
 * squares per vector, 2 * 1023^2 at most */
uint64_t n265_ssd_wh_avx512(const pixel *a, int as, const pixel *b, int bs,
                            int w, int h)
{
    if ((w == 8 || w == 4) && h >= 4) {
        /* four rows a vector; at most 16 vectors over a 64-row block */
        int h4 = h & ~3;
        uint64_t rest = h4 < h ? n265_ssd_wh_avx2(a + (ptrdiff_t)h4 * as, as,
                                                  b + (ptrdiff_t)h4 * bs, bs,
                                                  w, h - h4)
                               : 0;
        __m512i acc = _mm512_setzero_si512();
        if (w == 8) {
            for (int y = 0; y < h4; y += 4) {
                __m512i d = _mm512_sub_epi16(ld4x8(a + (ptrdiff_t)y * as, as),
                                             ld4x8(b + (ptrdiff_t)y * bs, bs));
                acc = _mm512_add_epi32(acc, _mm512_madd_epi16(d, d));
            }
        } else {
            for (int y = 0; y < h4; y += 4) {
                __m256i d = _mm256_sub_epi16(ld4x4(a + (ptrdiff_t)y * as, as),
                                             ld4x4(b + (ptrdiff_t)y * bs, bs));
                acc = _mm512_add_epi32(acc, _mm512_zextsi256_si512(
                    _mm256_madd_epi16(d, d)));
            }
        }
        return (uint64_t)_mm512_reduce_add_epi64(_mm512_add_epi64(
            _mm512_cvtepu32_epi64(_mm512_castsi512_si256(acc)),
            _mm512_cvtepu32_epi64(_mm512_extracti64x4_epi64(acc, 1)))) + rest;
    }
    if (w <= 8)
        return n265_ssd_wh_avx2(a, as, b, bs, w, h);
    __m512i acc = _mm512_setzero_si512(), a32 = _mm512_setzero_si512();
    int k = 0;
    for (int y = 0; y < h;) {
        const pixel *p = a + (ptrdiff_t)y * as, *q = b + (ptrdiff_t)y * bs;
        if (w > 16) {
            for (int x = 0; x < w; x += 32) {
                __m512i d = _mm512_sub_epi16(ldm32(p + x, w - x),
                                             ldm32(q + x, w - x));
                a32 = _mm512_add_epi32(a32, _mm512_madd_epi16(d, d));
                k++;
            }
            y++;
        } else {
            int two = y + 1 < h;
            __m512i d = _mm512_sub_epi16(
                ldm2x16(p, p + (two ? as : 0), w),
                ldm2x16(q, q + (two ? bs : 0), w));
            /* a missing second row reads row y twice; drop its half */
            if (!two)
                d = _mm512_maskz_mov_epi16(0xffffu, d);
            a32 = _mm512_add_epi32(a32, _mm512_madd_epi16(d, d));
            k++;
            y += 2;
        }
        if (k >= 32) {
            acc = _mm512_add_epi64(acc, _mm512_add_epi64(
                _mm512_cvtepu32_epi64(_mm512_castsi512_si256(a32)),
                _mm512_cvtepu32_epi64(_mm512_extracti64x4_epi64(a32, 1))));
            a32 = _mm512_setzero_si512();
            k = 0;
        }
    }
    acc = _mm512_add_epi64(acc, _mm512_add_epi64(
        _mm512_cvtepu32_epi64(_mm512_castsi512_si256(a32)),
        _mm512_cvtepu32_epi64(_mm512_extracti64x4_epi64(a32, 1))));
    return (uint64_t)_mm512_reduce_add_epi64(acc);
}

/* ---- n265_psy_energy ------------------------------------------------
 *
 * satd_avx2.c's 4x4 tiles without their DC, eight tiles to a vector: 32
 * columns of one 4-row band, or 16 columns of two bands. */
static inline __m512i psy_tiles(__m512i r0, __m512i r1, __m512i r2,
                                __m512i r3)
{
    const __m512i one = _mm512_set1_epi16(1);
    r0 = hstage2(hstage1(r0));
    r1 = hstage2(hstage1(r1));
    r2 = hstage2(hstage1(r2));
    r3 = hstage2(hstage1(r3));
    __m512i s0 = _mm512_add_epi16(r0, r1), d0 = _mm512_sub_epi16(r0, r1);
    __m512i s1 = _mm512_add_epi16(r2, r3), d1 = _mm512_sub_epi16(r2, r3);
    __m512i o0 = _mm512_maskz_mov_epi16(0xeeeeeeeeu, _mm512_add_epi16(s0, s1));
    __m512i t0 = _mm512_add_epi16(_mm512_abs_epi16(o0),
                                  _mm512_abs_epi16(_mm512_sub_epi16(s0, s1)));
    __m512i t1 = _mm512_add_epi16(_mm512_abs_epi16(_mm512_add_epi16(d0, d1)),
                                  _mm512_abs_epi16(_mm512_sub_epi16(d0, d1)));
#if BIT_DEPTH == 8
    return _mm512_madd_epi16(_mm512_add_epi16(t0, t1), one);
#else
    return _mm512_add_epi32(_mm512_madd_epi16(t0, one),
                            _mm512_madd_epi16(t1, one));
#endif
}

uint32_t n265_psy_energy_avx512(const pixel *p, int stride, int n)
{
    if (n < 16 || (n & 15))
        return n265_psy_energy_avx2(p, stride, n);
    __m512i acc = _mm512_setzero_si512();
    if (n >= 32 && !(n & 31)) {
        for (int y = 0; y < n; y += 4) {
            const pixel *r = p + (ptrdiff_t)y * stride;
            for (int x = 0; x < n; x += 32)
                acc = _mm512_add_epi32(acc, psy_tiles(
                    ld32(r + x), ld32(r + stride + x),
                    ld32(r + 2 * stride + x), ld32(r + 3 * stride + x)));
        }
    } else {
        const ptrdiff_t s4 = 4 * (ptrdiff_t)stride;
        for (int y = 0; y < n; y += 8) {
            const pixel *r = p + (ptrdiff_t)y * stride;
            for (int x = 0; x < n; x += 16)
                acc = _mm512_add_epi32(acc, psy_tiles(
                    ld2x16(r + x, r + s4 + x),
                    ld2x16(r + stride + x, r + s4 + stride + x),
                    ld2x16(r + 2 * stride + x, r + s4 + 2 * stride + x),
                    ld2x16(r + 3 * stride + x, r + s4 + 3 * stride + x)));
        }
    }
    return (uint32_t)_mm512_reduce_add_epi32(acc);
}
