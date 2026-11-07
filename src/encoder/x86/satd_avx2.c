/*
 * satd_avx2.c - the SATD / SAD / SSD family (N265_ASM_SATD) on AVX2.
 *
 * The entry points are src/encoder/arm/neon.h's satd block, renamed. All
 * of them are 8-bit: a sample is one byte and widens to an int16 lane.
 *
 * The Hadamard tiles never transpose. A 16-byte row load widened to 16
 * int16 lanes is two 8x8 tiles side by side, or four 4x4 tiles, so the
 * horizontal transform runs inside each 4- or 8-lane group with in-lane
 * shuffles (`x' = sign(x, +-) + swap(x)` per radix-2 stage) and the
 * vertical one runs across the row registers. The order of the outputs is
 * not the C's, but the 2-D Walsh-Hadamard of a block is the same set of
 * values in any butterfly order, and every caller sums their absolute
 * values, so the integer is the same. The per-tile rounding of the C
 * (`(s + 1) >> 1` at 4x4, `(s + 2) >> 2` at 8x8) is applied per tile.
 *
 * Ranges at 8 bits: a difference is within +-255, a 4x4 coefficient
 * within +-4080 and an 8x8 one within +-16320, so int16 holds every stage.
 * Two 8x8 absolute values sum to at most 32640, which is still a positive
 * int16, so pairs are added before the widening madd.
 *
 * Every load reads exactly the bytes of the block: 16, 8 or 4 per row and
 * a scalar tail below 4. checkasm's page guard holds the kernels to that.
 *
 * Copyright (c) 2026, the yah265 authors. GPL-2.0-or-later; see LICENSE.
 */

#include <immintrin.h>
#include <string.h>

#include "encoder/satd.h"
#include "encoder/x86/avx2.h"

/* n = 16, 8 or 4 bytes, zero-extended to 16 int16 lanes */
static inline __m256i ld_w16(const pixel *p, int n)
{
    __m128i x;
    if (n == 16) {
        x = _mm_loadu_si128((const __m128i *)p);
    } else if (n == 8) {
        x = _mm_loadl_epi64((const __m128i *)p);
    } else {
        int32_t t;
        memcpy(&t, p, 4);
        x = _mm_cvtsi32_si128(t);
    }
    return _mm256_cvtepu8_epi16(x);
}

static inline __m256i ld_diff(const pixel *a, const pixel *b, int n)
{
    return _mm256_sub_epi16(ld_w16(a, n), ld_w16(b, n));
}

/* radix-2 stage over lane pairs at distance 1, 2 or 4 inside each group */
static inline __m256i hstage1(__m256i v)
{
    const __m256i swp = _mm256_setr_epi8(2, 3, 0, 1, 6, 7, 4, 5, 10, 11, 8, 9,
                                         14, 15, 12, 13, 2, 3, 0, 1, 6, 7, 4,
                                         5, 10, 11, 8, 9, 14, 15, 12, 13);
    const __m256i sg = _mm256_setr_epi16(1, -1, 1, -1, 1, -1, 1, -1, 1, -1, 1,
                                         -1, 1, -1, 1, -1);
    return _mm256_add_epi16(_mm256_sign_epi16(v, sg),
                            _mm256_shuffle_epi8(v, swp));
}

static inline __m256i hstage2(__m256i v)
{
    const __m256i sg = _mm256_setr_epi16(1, 1, -1, -1, 1, 1, -1, -1, 1, 1, -1,
                                         -1, 1, 1, -1, -1);
    return _mm256_add_epi16(_mm256_sign_epi16(v, sg),
                            _mm256_shuffle_epi32(v, 0xb1));
}

static inline __m256i hstage4(__m256i v)
{
    const __m256i sg = _mm256_setr_epi16(1, 1, 1, 1, -1, -1, -1, -1, 1, 1, 1,
                                         1, -1, -1, -1, -1);
    return _mm256_add_epi16(_mm256_sign_epi16(v, sg),
                            _mm256_shuffle_epi32(v, 0x4e));
}

static inline uint32_t hsum_epi32(__m256i v)
{
    __m128i s = _mm_add_epi32(_mm256_castsi256_si128(v),
                              _mm256_extracti128_si256(v, 1));
    s = _mm_add_epi32(s, _mm_shuffle_epi32(s, 0x4e));
    s = _mm_add_epi32(s, _mm_shuffle_epi32(s, 0xb1));
    return (uint32_t)_mm_cvtsi128_si32(s);
}

static inline uint64_t hsum_epi64(__m256i v)
{
    __m128i s = _mm_add_epi64(_mm256_castsi256_si128(v),
                              _mm256_extracti128_si256(v, 1));
    s = _mm_add_epi64(s, _mm_unpackhi_epi64(s, s));
    return (uint64_t)_mm_cvtsi128_si64(s);
}

/* The 4-point transform of four rows of 4x4 tiles, already transformed
 * horizontally: the four coefficient rows' absolute values, summed. */
static inline __m256i vabs4(__m256i r0, __m256i r1, __m256i r2, __m256i r3)
{
    __m256i s0 = _mm256_add_epi16(r0, r1), d0 = _mm256_sub_epi16(r0, r1);
    __m256i s1 = _mm256_add_epi16(r2, r3), d1 = _mm256_sub_epi16(r2, r3);
    __m256i o0 = _mm256_abs_epi16(_mm256_add_epi16(s0, s1));
    __m256i o1 = _mm256_abs_epi16(_mm256_sub_epi16(s0, s1));
    __m256i o2 = _mm256_abs_epi16(_mm256_add_epi16(d0, d1));
    __m256i o3 = _mm256_abs_epi16(_mm256_sub_epi16(d0, d1));
    /* 4 * 4080 fits int16 */
    return _mm256_add_epi16(_mm256_add_epi16(o0, o1),
                            _mm256_add_epi16(o2, o3));
}

/* One 4-row band of 4x4 SATD tiles across `w` columns (a multiple of 4),
 * each tile rounded as the C rounds it. */
static inline uint32_t band4(const pixel *a, int as, const pixel *b, int bs,
                             int w)
{
    const __m256i one = _mm256_set1_epi16(1);
    __m256i acc = _mm256_setzero_si256();
    for (int x = 0; x < w;) {
        int n = w - x >= 16 ? 16 : w - x >= 8 ? 8 : 4;
        __m256i r0 = hstage2(hstage1(ld_diff(a + x, b + x, n)));
        __m256i r1 = hstage2(hstage1(ld_diff(a + as + x, b + bs + x, n)));
        __m256i r2 = hstage2(hstage1(ld_diff(a + 2 * as + x,
                                             b + 2 * bs + x, n)));
        __m256i r3 = hstage2(hstage1(ld_diff(a + 3 * as + x,
                                             b + 3 * bs + x, n)));
        __m256i m = _mm256_madd_epi16(vabs4(r0, r1, r2, r3), one);
        /* a tile is two adjacent epi32 lanes; after this both hold its sum */
        m = _mm256_add_epi32(m, _mm256_shuffle_epi32(m, 0xb1));
        m = _mm256_srli_epi32(_mm256_add_epi32(m, _mm256_set1_epi32(1)), 1);
        acc = _mm256_add_epi32(acc, m);
        x += n;
    }
    /* every tile was counted twice, and both copies are exact */
    return hsum_epi32(acc) >> 1;
}

/* One 8-row band of 8x8 tiles across `w` columns (a multiple of 8). */
static inline uint32_t band8(const pixel *a, int as, const pixel *b, int bs,
                             int w)
{
    const __m256i one = _mm256_set1_epi16(1);
    uint32_t sum = 0;
    for (int x = 0; x < w;) {
        int n = w - x >= 16 ? 16 : 8;
        __m256i r[8];
        for (int y = 0; y < 8; y++)
            r[y] = hstage4(hstage2(hstage1(
                ld_diff(a + y * as + x, b + y * bs + x, n))));
        for (int s = 1; s < 8; s <<= 1)
            for (int i = 0; i < 8; i += s << 1)
                for (int j = i; j < i + s; j++) {
                    __m256i t = r[j];
                    r[j] = _mm256_add_epi16(t, r[j + s]);
                    r[j + s] = _mm256_sub_epi16(t, r[j + s]);
                }
        __m256i m = _mm256_setzero_si256();
        for (int y = 0; y < 8; y += 2) {
            __m256i p = _mm256_add_epi16(_mm256_abs_epi16(r[y]),
                                         _mm256_abs_epi16(r[y + 1]));
            m = _mm256_add_epi32(m, _mm256_madd_epi16(p, one));
        }
        /* one tile per 128-bit half */
        m = _mm256_add_epi32(m, _mm256_shuffle_epi32(m, 0x4e));
        m = _mm256_add_epi32(m, _mm256_shuffle_epi32(m, 0xb1));
        m = _mm256_srli_epi32(_mm256_add_epi32(m, _mm256_set1_epi32(2)), 2);
        sum += (uint32_t)_mm256_extract_epi32(m, 0) +
               (uint32_t)_mm256_extract_epi32(m, 4);
        x += n;
    }
    return sum;
}

uint32_t n265_satd4_avx2(const pixel *a, int as, const pixel *b, int bs)
{
    return band4(a, as, b, bs, 4);
}

uint32_t n265_satd8_avx2(const pixel *a, int as, const pixel *b, int bs)
{
    return band8(a, as, b, bs, 8);
}

/* n265_satd_wh_c's tiling, both arms of N265_SATD8 */
uint32_t n265_satd_wh_avx2(const pixel *a, int as, const pixel *b, int bs,
                           int w, int h)
{
    uint32_t sum = 0;
    if ((w & 7) || (h & 7)) {
        int mixed = n265_satd8_mixed();
        int w8 = mixed ? (w & ~7) : 0;
        int h8 = mixed ? (h & ~7) : 0;
        for (int y = 0; y < h8; y += 8) {
            const pixel *pa = a + (ptrdiff_t)y * as;
            const pixel *pb = b + (ptrdiff_t)y * bs;
            if (w8)
                sum += band8(pa, as, pb, bs, w8);
            if (w > w8) {
                sum += band4(pa + w8, as, pb + w8, bs, w - w8);
                sum += band4(pa + 4 * as + w8, as, pb + 4 * bs + w8, bs,
                             w - w8);
            }
        }
        for (int y = h8; y < h; y += 4)
            sum += band4(a + (ptrdiff_t)y * as, as, b + (ptrdiff_t)y * bs, bs,
                         w);
        return sum;
    }
    for (int y = 0; y < h; y += 8)
        sum += band8(a + (ptrdiff_t)y * as, as, b + (ptrdiff_t)y * bs, bs, w);
    return sum;
}

/* ---- SAD / SSD ------------------------------------------------------ */

uint32_t n265_sad_wh_avx2(const pixel *a, int as, const pixel *b, int bs,
                          int w, int h)
{
    __m256i acc = _mm256_setzero_si256();
    __m128i acc1 = _mm_setzero_si128();
    uint32_t tail = 0;
    for (int y = 0; y < h; y++) {
        const pixel *p = a + (ptrdiff_t)y * as, *q = b + (ptrdiff_t)y * bs;
        int x = 0;
        for (; x + 32 <= w; x += 32)
            acc = _mm256_add_epi64(acc, _mm256_sad_epu8(
                _mm256_loadu_si256((const __m256i *)(p + x)),
                _mm256_loadu_si256((const __m256i *)(q + x))));
        if (x + 16 <= w) {
            acc1 = _mm_add_epi64(acc1, _mm_sad_epu8(
                _mm_loadu_si128((const __m128i *)(p + x)),
                _mm_loadu_si128((const __m128i *)(q + x))));
            x += 16;
        }
        if (x + 8 <= w) {
            acc1 = _mm_add_epi64(acc1, _mm_sad_epu8(
                _mm_loadl_epi64((const __m128i *)(p + x)),
                _mm_loadl_epi64((const __m128i *)(q + x))));
            x += 8;
        }
        if (x + 4 <= w) {
            int32_t s, t;
            memcpy(&s, p + x, 4);
            memcpy(&t, q + x, 4);
            acc1 = _mm_add_epi64(acc1, _mm_sad_epu8(_mm_cvtsi32_si128(s),
                                                    _mm_cvtsi32_si128(t)));
            x += 4;
        }
        for (; x < w; x++) {
            int d = p[x] - q[x];
            tail += (uint32_t)(d < 0 ? -d : d);
        }
    }
    acc1 = _mm_add_epi64(acc1, _mm_unpackhi_epi64(acc1, acc1));
    return (uint32_t)hsum_epi64(acc) + (uint32_t)_mm_cvtsi128_si32(acc1) +
           tail;
}

uint64_t n265_ssd_wh_avx2(const pixel *a, int as, const pixel *b, int bs,
                          int w, int h)
{
    __m256i acc = _mm256_setzero_si256();   /* epi64 */
    uint64_t tail = 0;
    for (int y = 0; y < h; y++) {
        const pixel *p = a + (ptrdiff_t)y * as, *q = b + (ptrdiff_t)y * bs;
        __m256i row = _mm256_setzero_si256();   /* epi32, one row only */
        int x = 0;
        while (w - x >= 4) {
            int n = w - x >= 16 ? 16 : w - x >= 8 ? 8 : 4;
            __m256i d = ld_diff(p + x, q + x, n);
            row = _mm256_add_epi32(row, _mm256_madd_epi16(d, d));
            x += n;
        }
        for (; x < w; x++) {
            int d = p[x] - q[x];
            tail += (uint64_t)(d * d);
        }
        acc = _mm256_add_epi64(acc, _mm256_cvtepu32_epi64(
                                        _mm256_castsi256_si128(row)));
        acc = _mm256_add_epi64(acc, _mm256_cvtepu32_epi64(
                                        _mm256_extracti128_si256(row, 1)));
    }
    return hsum_epi64(acc) + tail;
}

uint32_t n265_sum_sq16_avx2(const pixel *p, int stride, int h, uint64_t *sq)
{
    __m128i s1 = _mm_setzero_si128();
    __m256i s2 = _mm256_setzero_si256();
    for (int y = 0; y < h; y++) {
        __m128i v =
            _mm_loadu_si128((const __m128i *)(p + (ptrdiff_t)y * stride));
        __m256i w = _mm256_cvtepu8_epi16(v);
        s1 = _mm_add_epi64(s1, _mm_sad_epu8(v, _mm_setzero_si128()));
        /* 16 rows of two squares per lane: 16 * 130050 fits */
        s2 = _mm256_add_epi32(s2, _mm256_madd_epi16(w, w));
    }
    s1 = _mm_add_epi64(s1, _mm_unpackhi_epi64(s1, s1));
    *sq = hsum_epi32(s2);
    return (uint32_t)_mm_cvtsi128_si32(s1);
}

/* ---- per-row and per-column SAD -------------------------------------
 *
 * Column sums stay in u16 lanes: n * 255 is at most 16320 at the n = 64
 * this takes. Any other shape goes to the reference. */
static inline __m128i absdiff8_16(const pixel *p, const pixel *q, int n)
{
    __m128i x = n == 16 ? _mm_loadu_si128((const __m128i *)p)
                        : _mm_loadl_epi64((const __m128i *)p);
    __m128i y = n == 16 ? _mm_loadu_si128((const __m128i *)q)
                        : _mm_loadl_epi64((const __m128i *)q);
    return _mm_sub_epi8(_mm_max_epu8(x, y), _mm_min_epu8(x, y));
}

void n265_sad_rowcol_avx2(const pixel *a, int as, const pixel *b, int bs,
                          int n, uint32_t *row, uint32_t *col)
{
    if ((n & 7) || n > 64 || n <= 0) {
        n265_sad_rowcol_ref(a, as, b, bs, n, row, col);
        return;
    }
    __m256i cacc[4];
    int nc = (n + 15) >> 4;
    for (int k = 0; k < nc; k++)
        cacc[k] = _mm256_setzero_si256();
    for (int y = 0; y < n; y++) {
        const pixel *p = a + (ptrdiff_t)y * as, *q = b + (ptrdiff_t)y * bs;
        __m128i rs = _mm_setzero_si128();
        for (int k = 0; k < nc; k++) {
            int m = n - 16 * k >= 16 ? 16 : 8;
            __m128i d = absdiff8_16(p + 16 * k, q + 16 * k, m);
            rs = _mm_add_epi64(rs, _mm_sad_epu8(d, _mm_setzero_si128()));
            cacc[k] = _mm256_add_epi16(cacc[k], _mm256_cvtepu8_epi16(d));
        }
        rs = _mm_add_epi64(rs, _mm_unpackhi_epi64(rs, rs));
        row[y] = (uint32_t)_mm_cvtsi128_si32(rs);
    }
    for (int k = 0; k < nc; k++) {
        _mm256_storeu_si256((__m256i *)(col + 16 * k),
                            _mm256_cvtepu16_epi32(
                                _mm256_castsi256_si128(cacc[k])));
        if (n - 16 * k >= 16)
            _mm256_storeu_si256((__m256i *)(col + 16 * k + 8),
                                _mm256_cvtepu16_epi32(
                                    _mm256_extracti128_si256(cacc[k], 1)));
    }
}

/* ---- n265_psy_energy ------------------------------------------------
 *
 * The 4x4 tiles' absolute Hadamard coefficients without their DC, over
 * an n x n block of samples (n a multiple of 4). After the horizontal
 * stages lane 0 of each 4-lane group is that row's sum, and after the
 * vertical one the first coefficient row's lane 0 is the tile's DC, so it
 * is blended to zero before the abs. */
uint32_t n265_psy_energy_avx2(const pixel *p, int stride, int n)
{
    const __m256i one = _mm256_set1_epi16(1);
    __m256i acc = _mm256_setzero_si256();
    for (int y = 0; y < n; y += 4) {
        const pixel *r = p + (ptrdiff_t)y * stride;
        for (int x = 0; x < n;) {
            int m = n - x >= 16 ? 16 : n - x >= 8 ? 8 : 4;
            __m256i r0 = hstage2(hstage1(ld_w16(r + x, m)));
            __m256i r1 = hstage2(hstage1(ld_w16(r + stride + x, m)));
            __m256i r2 = hstage2(hstage1(ld_w16(r + 2 * stride + x, m)));
            __m256i r3 = hstage2(hstage1(ld_w16(r + 3 * stride + x, m)));
            __m256i s0 = _mm256_add_epi16(r0, r1);
            __m256i d0 = _mm256_sub_epi16(r0, r1);
            __m256i s1 = _mm256_add_epi16(r2, r3);
            __m256i d1 = _mm256_sub_epi16(r2, r3);
            __m256i o0 = _mm256_blend_epi16(_mm256_add_epi16(s0, s1),
                                            _mm256_setzero_si256(), 0x11);
            __m256i t = _mm256_add_epi16(
                _mm256_add_epi16(_mm256_abs_epi16(o0),
                                 _mm256_abs_epi16(_mm256_sub_epi16(s0, s1))),
                _mm256_add_epi16(_mm256_abs_epi16(_mm256_add_epi16(d0, d1)),
                                 _mm256_abs_epi16(_mm256_sub_epi16(d0, d1))));
            acc = _mm256_add_epi32(acc, _mm256_madd_epi16(t, one));
            x += m;
        }
    }
    return hsum_epi32(acc);
}
