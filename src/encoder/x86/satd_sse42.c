/*
 * satd_sse42.c - the SATD / SAD / SSD family (N265_ASM_SATD) on SSE4.2.
 *
 * satd_avx2.c's structure in 128-bit registers, at both depths: a row of
 * 8 samples in 8 int16 lanes is one 8x8 tile row or two 4x4 tile rows, the
 * horizontal transform runs inside each 4- or 8-lane group with in-lane
 * shuffles and the vertical one across the row registers. The 2-D
 * Walsh-Hadamard gives the same set of values in any butterfly order, so
 * the absolute sums match the C. The AVX2 file has the range arguments,
 * Main10's included (the 8x8 tile's last stage as 2 * max(|a|, |b|), the
 * 4x4 sums added in pairs); they hold lane for lane.
 *
 * Every load reads exactly the bytes of the block: 16, 8 or 4 per row and
 * a scalar tail below 4.
 *
 * Copyright (c) 2026, the yah265 authors. GPL-2.0-or-later; see LICENSE.
 */

#include <smmintrin.h>
#include <string.h>

#include "encoder/satd.h"
#include "encoder/x86/sse42.h"

/* n = 8 or 4 samples, zero-extended to 8 int16 lanes */
static inline __m128i ld_w8(const pixel *p, int n)
{
#if BIT_DEPTH > 8
    return n == 8 ? _mm_loadu_si128((const __m128i *)p)
                  : _mm_loadl_epi64((const __m128i *)p);
#else
    __m128i x;
    if (n == 8) {
        x = _mm_loadl_epi64((const __m128i *)p);
    } else {
        int32_t t;
        memcpy(&t, p, 4);
        x = _mm_cvtsi32_si128(t);
    }
    return _mm_cvtepu8_epi16(x);
#endif
}

static inline __m128i ld_diff(const pixel *a, const pixel *b, int n)
{
    return _mm_sub_epi16(ld_w8(a, n), ld_w8(b, n));
}

/* radix-2 stage over lane pairs at distance 1, 2 or 4 inside each group */
static inline __m128i hstage1(__m128i v)
{
    const __m128i swp = _mm_setr_epi8(2, 3, 0, 1, 6, 7, 4, 5, 10, 11, 8, 9,
                                      14, 15, 12, 13);
    const __m128i sg = _mm_setr_epi16(1, -1, 1, -1, 1, -1, 1, -1);
    return _mm_add_epi16(_mm_sign_epi16(v, sg), _mm_shuffle_epi8(v, swp));
}

static inline __m128i hstage2(__m128i v)
{
    const __m128i sg = _mm_setr_epi16(1, 1, -1, -1, 1, 1, -1, -1);
    return _mm_add_epi16(_mm_sign_epi16(v, sg), _mm_shuffle_epi32(v, 0xb1));
}

static inline __m128i hstage4(__m128i v)
{
    const __m128i sg = _mm_setr_epi16(1, 1, 1, 1, -1, -1, -1, -1);
    return _mm_add_epi16(_mm_sign_epi16(v, sg), _mm_shuffle_epi32(v, 0x4e));
}

static inline uint32_t hsum_epi32(__m128i s)
{
    s = _mm_add_epi32(s, _mm_shuffle_epi32(s, 0x4e));
    s = _mm_add_epi32(s, _mm_shuffle_epi32(s, 0xb1));
    return (uint32_t)_mm_cvtsi128_si32(s);
}

static inline uint64_t hsum_epi64(__m128i s)
{
    s = _mm_add_epi64(s, _mm_unpackhi_epi64(s, s));
    return (uint64_t)_mm_cvtsi128_si64(s);
}

/* the vertical 4-point transform of four horizontally transformed rows,
 * absolute values summed into epi32 lane pairs */
static inline __m128i vabs4(__m128i r0, __m128i r1, __m128i r2, __m128i r3)
{
    const __m128i one = _mm_set1_epi16(1);
    __m128i s0 = _mm_add_epi16(r0, r1), d0 = _mm_sub_epi16(r0, r1);
    __m128i s1 = _mm_add_epi16(r2, r3), d1 = _mm_sub_epi16(r2, r3);
    __m128i o0 = _mm_abs_epi16(_mm_add_epi16(s0, s1));
    __m128i o1 = _mm_abs_epi16(_mm_sub_epi16(s0, s1));
    __m128i o2 = _mm_abs_epi16(_mm_add_epi16(d0, d1));
    __m128i o3 = _mm_abs_epi16(_mm_sub_epi16(d0, d1));
#if BIT_DEPTH == 8
    return _mm_madd_epi16(_mm_add_epi16(_mm_add_epi16(o0, o1),
                                        _mm_add_epi16(o2, o3)), one);
#else
    return _mm_add_epi32(_mm_madd_epi16(_mm_add_epi16(o0, o1), one),
                         _mm_madd_epi16(_mm_add_epi16(o2, o3), one));
#endif
}

/* one 4-row band of 4x4 tiles across w (a multiple of 4), each rounded */
static inline uint32_t band4(const pixel *a, int as, const pixel *b, int bs,
                             int w)
{
    __m128i acc = _mm_setzero_si128();
    for (int x = 0; x < w;) {
        int n = w - x >= 8 ? 8 : 4;
        __m128i r0 = hstage2(hstage1(ld_diff(a + x, b + x, n)));
        __m128i r1 = hstage2(hstage1(ld_diff(a + as + x, b + bs + x, n)));
        __m128i r2 = hstage2(hstage1(ld_diff(a + 2 * as + x,
                                             b + 2 * bs + x, n)));
        __m128i r3 = hstage2(hstage1(ld_diff(a + 3 * as + x,
                                             b + 3 * bs + x, n)));
        __m128i m = vabs4(r0, r1, r2, r3);
        /* a tile is two adjacent epi32 lanes; after this both hold its sum */
        m = _mm_add_epi32(m, _mm_shuffle_epi32(m, 0xb1));
        m = _mm_srli_epi32(_mm_add_epi32(m, _mm_set1_epi32(1)), 1);
        acc = _mm_add_epi32(acc, m);
        x += n;
    }
    /* every tile was counted twice */
    return hsum_epi32(acc) >> 1;
}

/* one 8-row band of 8x8 tiles across w (a multiple of 8) */
static inline uint32_t band8(const pixel *a, int as, const pixel *b, int bs,
                             int w)
{
    const __m128i one = _mm_set1_epi16(1);
    uint32_t sum = 0;
    for (int x = 0; x < w; x += 8) {
        __m128i r[8];
        for (int y = 0; y < 8; y++)
            r[y] = hstage4(hstage2(hstage1(
                ld_diff(a + y * as + x, b + y * bs + x, 8))));
#if BIT_DEPTH == 8
        const int last = 8;
#else
        const int last = 4;
#endif
        for (int s = 1; s < last; s <<= 1)
            for (int i = 0; i < 8; i += s << 1)
                for (int j = i; j < i + s; j++) {
                    __m128i t = r[j];
                    r[j] = _mm_add_epi16(t, r[j + s]);
                    r[j + s] = _mm_sub_epi16(t, r[j + s]);
                }
        __m128i m = _mm_setzero_si128();
#if BIT_DEPTH == 8
        for (int y = 0; y < 8; y += 2) {
            __m128i p = _mm_add_epi16(_mm_abs_epi16(r[y]),
                                      _mm_abs_epi16(r[y + 1]));
            m = _mm_add_epi32(m, _mm_madd_epi16(p, one));
        }
#else
        /* the last vertical stage, as 2 * max(|a|, |b|) */
        for (int y = 0; y < 4; y++) {
            __m128i p = _mm_max_epi16(_mm_abs_epi16(r[y]),
                                      _mm_abs_epi16(r[y + 4]));
            m = _mm_add_epi32(m, _mm_madd_epi16(p, one));
        }
        m = _mm_slli_epi32(m, 1);
#endif
        sum += (hsum_epi32(m) + 2) >> 2;
    }
    return sum;
}

uint32_t n265_satd4_sse42(const pixel *a, int as, const pixel *b, int bs)
{
    return band4(a, as, b, bs, 4);
}

uint32_t n265_satd8_sse42(const pixel *a, int as, const pixel *b, int bs)
{
    return band8(a, as, b, bs, 8);
}

/* n265_satd_wh_c's tiling, both arms of N265_SATD8 */
uint32_t n265_satd_wh_sse42(const pixel *a, int as, const pixel *b, int bs,
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

#if BIT_DEPTH == 8
uint32_t n265_sad_wh_sse42(const pixel *a, int as, const pixel *b, int bs,
                           int w, int h)
{
    __m128i acc = _mm_setzero_si128();
    uint32_t tail = 0;
    for (int y = 0; y < h; y++) {
        const pixel *p = a + (ptrdiff_t)y * as, *q = b + (ptrdiff_t)y * bs;
        int x = 0;
        for (; x + 16 <= w; x += 16)
            acc = _mm_add_epi64(acc, _mm_sad_epu8(
                _mm_loadu_si128((const __m128i *)(p + x)),
                _mm_loadu_si128((const __m128i *)(q + x))));
        if (x + 8 <= w) {
            acc = _mm_add_epi64(acc, _mm_sad_epu8(
                _mm_loadl_epi64((const __m128i *)(p + x)),
                _mm_loadl_epi64((const __m128i *)(q + x))));
            x += 8;
        }
        if (x + 4 <= w) {
            int32_t s, t;
            memcpy(&s, p + x, 4);
            memcpy(&t, q + x, 4);
            acc = _mm_add_epi64(acc, _mm_sad_epu8(_mm_cvtsi32_si128(s),
                                                  _mm_cvtsi32_si128(t)));
            x += 4;
        }
        for (; x < w; x++) {
            int d = p[x] - q[x];
            tail += (uint32_t)(d < 0 ? -d : d);
        }
    }
    return (uint32_t)hsum_epi64(acc) + tail;
}
#else
static inline __m128i absdiff16(__m128i x, __m128i y)
{
    return _mm_sub_epi16(_mm_max_epu16(x, y), _mm_min_epu16(x, y));
}

/* a 64x64 block sums to at most 4096 * 1023 in epi32 lanes */
uint32_t n265_sad_wh_sse42(const pixel *a, int as, const pixel *b, int bs,
                           int w, int h)
{
    const __m128i one = _mm_set1_epi16(1);
    __m128i acc = _mm_setzero_si128();
    uint32_t tail = 0;
    for (int y = 0; y < h; y++) {
        const pixel *p = a + (ptrdiff_t)y * as, *q = b + (ptrdiff_t)y * bs;
        int x = 0;
        while (w - x >= 4) {
            int n = w - x >= 8 ? 8 : 4;
            acc = _mm_add_epi32(acc, _mm_madd_epi16(
                absdiff16(ld_w8(p + x, n), ld_w8(q + x, n)), one));
            x += n;
        }
        for (; x < w; x++) {
            int d = p[x] - q[x];
            tail += (uint32_t)(d < 0 ? -d : d);
        }
    }
    return hsum_epi32(acc) + tail;
}
#endif

uint64_t n265_ssd_wh_sse42(const pixel *a, int as, const pixel *b, int bs,
                           int w, int h)
{
    __m128i acc = _mm_setzero_si128();   /* epi64 */
    uint64_t tail = 0;
    for (int y = 0; y < h; y++) {
        const pixel *p = a + (ptrdiff_t)y * as, *q = b + (ptrdiff_t)y * bs;
        __m128i row = _mm_setzero_si128();   /* epi32, one row only */
        int x = 0;
        while (w - x >= 4) {
            int n = w - x >= 8 ? 8 : 4;
            __m128i d = ld_diff(p + x, q + x, n);
            row = _mm_add_epi32(row, _mm_madd_epi16(d, d));
            x += n;
        }
        for (; x < w; x++) {
            int d = p[x] - q[x];
            tail += (uint64_t)(d * d);
        }
        acc = _mm_add_epi64(acc, _mm_cvtepu32_epi64(row));
        acc = _mm_add_epi64(acc, _mm_cvtepu32_epi64(
                                     _mm_unpackhi_epi64(row, row)));
    }
    return hsum_epi64(acc) + tail;
}

#if BIT_DEPTH == 8
uint32_t n265_sum_sq16_sse42(const pixel *p, int stride, int h, uint64_t *sq)
{
    const __m128i z = _mm_setzero_si128();
    __m128i s1 = z, s2 = z;
    for (int y = 0; y < h; y++) {
        __m128i v =
            _mm_loadu_si128((const __m128i *)(p + (ptrdiff_t)y * stride));
        __m128i lo = _mm_unpacklo_epi8(v, z), hi = _mm_unpackhi_epi8(v, z);
        s1 = _mm_add_epi64(s1, _mm_sad_epu8(v, z));
        s2 = _mm_add_epi32(s2, _mm_add_epi32(_mm_madd_epi16(lo, lo),
                                             _mm_madd_epi16(hi, hi)));
    }
    *sq = hsum_epi32(s2);
    return (uint32_t)hsum_epi64(s1);
}
#else
uint32_t n265_sum_sq16_sse42(const pixel *p, int stride, int h, uint64_t *sq)
{
    const __m128i one = _mm_set1_epi16(1);
    __m128i s1 = _mm_setzero_si128(), s2 = s1;
    for (int y = 0; y < h; y++) {
        const pixel *r = p + (ptrdiff_t)y * stride;
        __m128i lo = _mm_loadu_si128((const __m128i *)r);
        __m128i hi = _mm_loadu_si128((const __m128i *)(r + 8));
        s1 = _mm_add_epi32(s1, _mm_add_epi32(_mm_madd_epi16(lo, one),
                                             _mm_madd_epi16(hi, one)));
        /* 16 rows of four squares per lane: 64 * 1046529 fits */
        s2 = _mm_add_epi32(s2, _mm_add_epi32(_mm_madd_epi16(lo, lo),
                                             _mm_madd_epi16(hi, hi)));
    }
    *sq = hsum_epi32(s2);
    return hsum_epi32(s1);
}
#endif

/* ---- per-row and per-column SAD -------------------------------------
 *
 * Column sums stay in u16 lanes: n * PIXEL_MAX is at most 65472 at
 * n = 64. */
#if BIT_DEPTH == 8
static inline __m128i absdiff8_16(const pixel *p, const pixel *q, int n)
{
    __m128i x = n == 16 ? _mm_loadu_si128((const __m128i *)p)
                        : _mm_loadl_epi64((const __m128i *)p);
    __m128i y = n == 16 ? _mm_loadu_si128((const __m128i *)q)
                        : _mm_loadl_epi64((const __m128i *)q);
    return _mm_sub_epi8(_mm_max_epu8(x, y), _mm_min_epu8(x, y));
}

void n265_sad_rowcol_sse42(const pixel *a, int as, const pixel *b, int bs,
                           int n, uint32_t *row, uint32_t *col)
{
    if ((n & 7) || n > 64 || n <= 0) {
        n265_sad_rowcol_ref(a, as, b, bs, n, row, col);
        return;
    }
    const __m128i z = _mm_setzero_si128();
    __m128i cacc[8];
    int nc = (n + 15) >> 4;
    for (int k = 0; k < 2 * nc; k++)
        cacc[k] = z;
    for (int y = 0; y < n; y++) {
        const pixel *p = a + (ptrdiff_t)y * as, *q = b + (ptrdiff_t)y * bs;
        __m128i rs = z;
        for (int k = 0; k < nc; k++) {
            int m = n - 16 * k >= 16 ? 16 : 8;
            __m128i d = absdiff8_16(p + 16 * k, q + 16 * k, m);
            rs = _mm_add_epi64(rs, _mm_sad_epu8(d, z));
            cacc[2 * k] = _mm_add_epi16(cacc[2 * k], _mm_unpacklo_epi8(d, z));
            cacc[2 * k + 1] = _mm_add_epi16(cacc[2 * k + 1],
                                            _mm_unpackhi_epi8(d, z));
        }
        row[y] = (uint32_t)hsum_epi64(rs);
    }
    for (int k = 0; k < n / 8; k++) {
        _mm_storeu_si128((__m128i *)(col + 8 * k),
                         _mm_unpacklo_epi16(cacc[k], z));
        _mm_storeu_si128((__m128i *)(col + 8 * k + 4),
                         _mm_unpackhi_epi16(cacc[k], z));
    }
}
#else
void n265_sad_rowcol_sse42(const pixel *a, int as, const pixel *b, int bs,
                           int n, uint32_t *row, uint32_t *col)
{
    if ((n & 7) || n > 64 || n <= 0) {
        n265_sad_rowcol_ref(a, as, b, bs, n, row, col);
        return;
    }
    const __m128i one = _mm_set1_epi16(1), z = _mm_setzero_si128();
    __m128i cacc[8];
    int nc = n >> 3;
    for (int k = 0; k < nc; k++)
        cacc[k] = z;
    for (int y = 0; y < n; y++) {
        const pixel *p = a + (ptrdiff_t)y * as, *q = b + (ptrdiff_t)y * bs;
        __m128i rs = z;
        for (int k = 0; k < nc; k++) {
            __m128i d = absdiff16(ld_w8(p + 8 * k, 8), ld_w8(q + 8 * k, 8));
            rs = _mm_add_epi32(rs, _mm_madd_epi16(d, one));
            cacc[k] = _mm_add_epi16(cacc[k], d);
        }
        row[y] = hsum_epi32(rs);
    }
    for (int k = 0; k < nc; k++) {
        _mm_storeu_si128((__m128i *)(col + 8 * k),
                         _mm_unpacklo_epi16(cacc[k], z));
        _mm_storeu_si128((__m128i *)(col + 8 * k + 4),
                         _mm_unpackhi_epi16(cacc[k], z));
    }
}
#endif

/* ---- n265_psy_energy ------------------------------------------------
 *
 * The 4x4 tiles' absolute Hadamard coefficients without their DC; lane 0
 * of each 4-lane group of the first coefficient row is that DC. */
uint32_t n265_psy_energy_sse42(const pixel *p, int stride, int n)
{
    const __m128i one = _mm_set1_epi16(1);
    __m128i acc = _mm_setzero_si128();
    for (int y = 0; y < n; y += 4) {
        const pixel *r = p + (ptrdiff_t)y * stride;
        for (int x = 0; x < n;) {
            int m = n - x >= 8 ? 8 : 4;
            __m128i r0 = hstage2(hstage1(ld_w8(r + x, m)));
            __m128i r1 = hstage2(hstage1(ld_w8(r + stride + x, m)));
            __m128i r2 = hstage2(hstage1(ld_w8(r + 2 * stride + x, m)));
            __m128i r3 = hstage2(hstage1(ld_w8(r + 3 * stride + x, m)));
            __m128i s0 = _mm_add_epi16(r0, r1);
            __m128i d0 = _mm_sub_epi16(r0, r1);
            __m128i s1 = _mm_add_epi16(r2, r3);
            __m128i d1 = _mm_sub_epi16(r2, r3);
            __m128i o0 = _mm_blend_epi16(_mm_add_epi16(s0, s1),
                                         _mm_setzero_si128(), 0x11);
            __m128i t0 = _mm_add_epi16(_mm_abs_epi16(o0),
                                       _mm_abs_epi16(_mm_sub_epi16(s0, s1)));
            __m128i t1 = _mm_add_epi16(_mm_abs_epi16(_mm_add_epi16(d0, d1)),
                                       _mm_abs_epi16(_mm_sub_epi16(d0, d1)));
#if BIT_DEPTH == 8
            acc = _mm_add_epi32(acc,
                                _mm_madd_epi16(_mm_add_epi16(t0, t1), one));
#else
            acc = _mm_add_epi32(acc, _mm_add_epi32(_mm_madd_epi16(t0, one),
                                                   _mm_madd_epi16(t1, one)));
#endif
            x += m;
        }
    }
    return hsum_epi32(acc);
}
