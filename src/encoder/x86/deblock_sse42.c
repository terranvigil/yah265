/*
 * deblock_sse42.c - the deblocking filter kernels (N265_ASM_DBK) on
 * SSE4.2.
 *
 * deblock_avx2.c's arithmetic in 128-bit registers, at both depths: one
 * int16 lane per sample, every filter output computed for every lane, the
 * decisions picking between them. An xmm holds two four-line segments,
 * so a segment's line-0 and line-3 decisions reach its lanes with one
 * pshufb. A vertical edge's eight lines are transposed into lanes and
 * back and stored whole, so p3 and q3 are rewritten with the values they
 * held. A tail under two segments, or under eight chroma lines, goes to
 * the C reference.
 *
 * Copyright (c) 2026, the yah265 authors. GPL-2.0-or-later; see LICENSE.
 */

#include <smmintrin.h>
#include <string.h>

#include "encoder/deblock.h"
#include "encoder/x86/sse42.h"

#if BIT_DEPTH == 8
static inline __m128i ld8(const pixel *p)
{
    return _mm_cvtepu8_epi16(_mm_loadl_epi64((const __m128i *)p));
}

static inline void st8(pixel *p, __m128i v)
{
    _mm_storel_epi64((__m128i *)p, _mm_packus_epi16(v, v));
}
#else
static inline __m128i ld8(const pixel *p)
{
    return _mm_loadu_si128((const __m128i *)p);
}

static inline void st8(pixel *p, __m128i v)
{
    _mm_storeu_si128((__m128i *)p, v);
}
#endif

static inline __m128i clamp3(__m128i v, __m128i lo, __m128i hi)
{
    return _mm_max_epi16(_mm_min_epi16(v, hi), lo);
}

static inline __m128i clip1(__m128i v)
{
    return clamp3(v, _mm_setzero_si128(), _mm_set1_epi16(PIXEL_MAX));
}

static inline __m128i lt(__m128i a, __m128i b)
{
    return _mm_cmpgt_epi16(b, a);
}

static inline __m128i rshr3(__m128i v)
{
    return _mm_srai_epi16(_mm_add_epi16(v, _mm_set1_epi16(4)), 3);
}

static inline __m128i rshr2(__m128i v)
{
    return _mm_srai_epi16(_mm_add_epi16(v, _mm_set1_epi16(2)), 2);
}

/* {v0 x4, v1 x4} from two int16 */
static inline __m128i seg_pair(const int16_t *v)
{
    int32_t w;
    memcpy(&w, v, sizeof(w));
    __m128i b = _mm_cvtsi32_si128(w);
    b = _mm_unpacklo_epi16(b, b);
    return _mm_unpacklo_epi32(b, b);
}

static inline int pair_zero(const int16_t *v)
{
    uint32_t w;
    memcpy(&w, v, sizeof(w));
    return !w;
}

/* p + d and its clamp to [p - t, p + t], then clip1 */
static inline __m128i near(__m128i v, __m128i p, __m128i t)
{
    return clip1(clamp3(v, _mm_sub_epi16(p, t), _mm_add_epi16(p, t)));
}

/* v[0..7] = p3..q3 across the edge, one line per lane; rewrites p2..q2.
 * Returns 0, touching nothing, when neither segment is filtered. */
static inline int luma_pair(__m128i v[8], __m128i beta, __m128i tc)
{
    __m128i p3 = v[0], p2 = v[1], p1 = v[2], p0 = v[3];
    __m128i q0 = v[4], q1 = v[5], q2 = v[6], q3 = v[7];
    const __m128i i0 = _mm_setr_epi8(0, 1, 0, 1, 0, 1, 0, 1,
                                     8, 9, 8, 9, 8, 9, 8, 9);
    const __m128i i3 = _mm_setr_epi8(6, 7, 6, 7, 6, 7, 6, 7,
                                     14, 15, 14, 15, 14, 15, 14, 15);

    __m128i dp = _mm_abs_epi16(_mm_sub_epi16(_mm_add_epi16(p2, p0),
                                             _mm_slli_epi16(p1, 1)));
    __m128i dq = _mm_abs_epi16(_mm_sub_epi16(_mm_add_epi16(q2, q0),
                                             _mm_slli_epi16(q1, 1)));
    __m128i dp03 = _mm_add_epi16(_mm_shuffle_epi8(dp, i0),
                                 _mm_shuffle_epi8(dp, i3));
    __m128i dq03 = _mm_add_epi16(_mm_shuffle_epi8(dq, i0),
                                 _mm_shuffle_epi8(dq, i3));
    __m128i on = lt(_mm_add_epi16(dp03, dq03), beta);
    if (!_mm_movemask_epi8(on))
        return 0;

    /* 8.7.2.5.6 per line, then both of a segment's lines must agree */
    __m128i tc5 = _mm_srai_epi16(
        _mm_add_epi16(_mm_mullo_epi16(tc, _mm_set1_epi16(5)),
                      _mm_set1_epi16(1)), 1);
    __m128i sl = _mm_and_si128(
        _mm_and_si128(
            lt(_mm_slli_epi16(_mm_add_epi16(dp, dq), 1),
               _mm_srai_epi16(beta, 2)),
            lt(_mm_add_epi16(_mm_abs_epi16(_mm_sub_epi16(p3, p0)),
                             _mm_abs_epi16(_mm_sub_epi16(q0, q3))),
               _mm_srai_epi16(beta, 3))),
        lt(_mm_abs_epi16(_mm_sub_epi16(p0, q0)), tc5));
    __m128i strong = _mm_and_si128(
        _mm_and_si128(_mm_shuffle_epi8(sl, i0), _mm_shuffle_epi8(sl, i3)),
        on);
    __m128i side = _mm_srai_epi16(
        _mm_add_epi16(beta, _mm_srai_epi16(beta, 1)), 3);
    __m128i dep = lt(dp03, side), deq = lt(dq03, side);

    /* strong filter */
    __m128i tc2 = _mm_slli_epi16(tc, 1);
    __m128i pq0 = _mm_add_epi16(p0, q0);
    __m128i three = _mm_set1_epi16(3);
    __m128i sp0 = rshr3(_mm_add_epi16(_mm_add_epi16(p2, q1),
        _mm_slli_epi16(_mm_add_epi16(p1, pq0), 1)));
    __m128i sp1 = rshr2(_mm_add_epi16(_mm_add_epi16(p2, p1), pq0));
    __m128i sp2 = rshr3(_mm_add_epi16(
        _mm_add_epi16(_mm_slli_epi16(p3, 1), _mm_mullo_epi16(p2, three)),
        _mm_add_epi16(p1, pq0)));
    __m128i sq0 = rshr3(_mm_add_epi16(_mm_add_epi16(q2, p1),
        _mm_slli_epi16(_mm_add_epi16(q1, pq0), 1)));
    __m128i sq1 = rshr2(_mm_add_epi16(_mm_add_epi16(q2, q1), pq0));
    __m128i sq2 = rshr3(_mm_add_epi16(
        _mm_add_epi16(_mm_slli_epi16(q3, 1), _mm_mullo_epi16(q2, three)),
        _mm_add_epi16(q1, pq0)));
    sp0 = near(sp0, p0, tc2);
    sp1 = near(sp1, p1, tc2);
    sp2 = near(sp2, p2, tc2);
    sq0 = near(sq0, q0, tc2);
    sq1 = near(sq1, q1, tc2);
    sq2 = near(sq2, q2, tc2);

    /* normal filter */
    __m128i ntc = _mm_sub_epi16(_mm_setzero_si128(), tc);
    __m128i delta = _mm_srai_epi16(_mm_add_epi16(_mm_sub_epi16(
        _mm_mullo_epi16(_mm_sub_epi16(q0, p0), _mm_set1_epi16(9)),
        _mm_mullo_epi16(_mm_sub_epi16(q1, p1), three)),
        _mm_set1_epi16(8)), 4);
    __m128i nok = lt(_mm_abs_epi16(delta),
                     _mm_mullo_epi16(tc, _mm_set1_epi16(10)));
    delta = clamp3(delta, ntc, tc);
    __m128i np0 = clip1(_mm_add_epi16(p0, delta));
    __m128i nq0 = clip1(_mm_sub_epi16(q0, delta));
    __m128i tch = _mm_srai_epi16(tc, 1);
    __m128i ntch = _mm_sub_epi16(_mm_setzero_si128(), tch);
    __m128i np1 = clip1(_mm_add_epi16(p1, clamp3(_mm_srai_epi16(
        _mm_add_epi16(_mm_sub_epi16(_mm_avg_epu16(p2, p0), p1), delta), 1),
        ntch, tch)));
    __m128i nq1 = clip1(_mm_add_epi16(q1, clamp3(_mm_srai_epi16(
        _mm_sub_epi16(_mm_sub_epi16(_mm_avg_epu16(q2, q0), q1), delta), 1),
        ntch, tch)));

    __m128i nm = _mm_andnot_si128(strong, _mm_and_si128(on, nok));
    v[1] = _mm_blendv_epi8(p2, sp2, strong);
    v[2] = _mm_blendv_epi8(
        _mm_blendv_epi8(p1, np1, _mm_and_si128(nm, dep)), sp1, strong);
    v[3] = _mm_blendv_epi8(_mm_blendv_epi8(p0, np0, nm), sp0, strong);
    v[4] = _mm_blendv_epi8(_mm_blendv_epi8(q0, nq0, nm), sq0, strong);
    v[5] = _mm_blendv_epi8(
        _mm_blendv_epi8(q1, nq1, _mm_and_si128(nm, deq)), sq1, strong);
    v[6] = _mm_blendv_epi8(q2, sq2, strong);
    return 1;
}

void n265_dbk_luma_h_sse42(pixel *pix, ptrdiff_t stride, const int16_t *beta,
                           const int16_t *tc, int nseg)
{
    int k = 0;
    for (; k + 2 <= nseg; k += 2) {
        if (pair_zero(tc + k))
            continue;
        pixel *b = pix + 4 * k;
        __m128i r[8];
        for (int i = 0; i < 8; i++)
            r[i] = ld8(b + (i - 4) * stride);
        if (!luma_pair(r, seg_pair(beta + k), seg_pair(tc + k)))
            continue;
        for (int i = 1; i < 7; i++)
            st8(b + (i - 4) * stride, r[i]);
    }
    if (k < nseg)
        n265_dbk_luma_h_ref(pix + 4 * k, stride, beta + k, tc + k, nseg - k);
}

/* 8x8 transpose of int16, its own inverse */
static inline void transpose8(__m128i r[8])
{
    __m128i a[8], t[8];
    for (int i = 0; i < 4; i++) {
        a[2 * i] = _mm_unpacklo_epi16(r[2 * i], r[2 * i + 1]);
        a[2 * i + 1] = _mm_unpackhi_epi16(r[2 * i], r[2 * i + 1]);
    }
    for (int h = 0; h < 2; h++)
        for (int j = 0; j < 2; j++) {
            t[4 * h + 2 * j] =
                _mm_unpacklo_epi32(a[4 * h + j], a[4 * h + j + 2]);
            t[4 * h + 2 * j + 1] =
                _mm_unpackhi_epi32(a[4 * h + j], a[4 * h + j + 2]);
        }
    for (int j = 0; j < 4; j++) {
        r[2 * j] = _mm_unpacklo_epi64(t[j], t[j + 4]);
        r[2 * j + 1] = _mm_unpackhi_epi64(t[j], t[j + 4]);
    }
}

void n265_dbk_luma_v_sse42(pixel *pix, ptrdiff_t stride, const int16_t *beta,
                           const int16_t *tc, int nseg)
{
    int k = 0;
    for (; k + 2 <= nseg; k += 2) {
        if (pair_zero(tc + k))
            continue;
        pixel *b = pix + 4 * k * stride - 4;
        __m128i r[8];
        for (int i = 0; i < 8; i++)
            r[i] = ld8(b + i * stride);
        transpose8(r);
        if (!luma_pair(r, seg_pair(beta + k), seg_pair(tc + k)))
            continue;
        transpose8(r);
        for (int i = 0; i < 8; i++)
            st8(b + i * stride, r[i]);
    }
    if (k < nseg)
        n265_dbk_luma_v_ref(pix + 4 * k * stride, stride, beta + k, tc + k,
                            nseg - k);
}

/* 8.7.2.5.5 on eight lines: returns p0' and q0' */
static inline void chroma8(__m128i p1, __m128i *p0, __m128i *q0, __m128i q1,
                           __m128i tc)
{
    __m128i d = rshr3(_mm_sub_epi16(_mm_add_epi16(
        _mm_slli_epi16(_mm_sub_epi16(*q0, *p0), 2), p1), q1));
    d = clamp3(d, _mm_sub_epi16(_mm_setzero_si128(), tc), tc);
    *p0 = clip1(_mm_add_epi16(*p0, d));
    *q0 = clip1(_mm_sub_epi16(*q0, d));
}

void n265_dbk_chroma_h_sse42(pixel *pix, ptrdiff_t stride, const int16_t *tc,
                             int n)
{
    int i = 0;
    for (; i + 8 <= n; i += 8) {
        __m128i t = _mm_loadu_si128((const __m128i *)(tc + i));
        if (_mm_testz_si128(t, t))
            continue;
        pixel *b = pix + i;
        __m128i p1 = ld8(b - 2 * stride), p0 = ld8(b - stride);
        __m128i q0 = ld8(b), q1 = ld8(b + stride);
        chroma8(p1, &p0, &q0, q1, t);
        st8(b - stride, p0);
        st8(b, q0);
    }
    if (i < n)
        n265_dbk_chroma_h_ref(pix + i, stride, tc + i, n - i);
}

#if BIT_DEPTH == 8
static inline int ld4(const pixel *p)
{
    int v;
    memcpy(&v, p, sizeof(v));
    return v;
}

#define CV_ST(v, j) do { \
    uint16_t w_ = (uint16_t)_mm_extract_epi16(v, j); \
    memcpy(b + (j) * stride - 1, &w_, sizeof(w_)); \
} while (0)

void n265_dbk_chroma_v_sse42(pixel *pix, ptrdiff_t stride, const int16_t *tc,
                             int n)
{
    /* a 4x4 byte transpose inside each 32-bit group of four rows */
    const __m128i tr = _mm_setr_epi8(0, 4, 8, 12, 1, 5, 9, 13,
                                     2, 6, 10, 14, 3, 7, 11, 15);
    int i = 0;
    for (; i + 8 <= n; i += 8) {
        __m128i t = _mm_loadu_si128((const __m128i *)(tc + i));
        if (_mm_testz_si128(t, t))
            continue;
        pixel *b = pix + i * stride;
        __m128i x[2];
        for (int g = 0; g < 2; g++) {
            const pixel *r = b + 4 * g * stride - 2;
            x[g] = _mm_shuffle_epi8(_mm_setr_epi32(ld4(r), ld4(r + stride),
                                                   ld4(r + 2 * stride),
                                                   ld4(r + 3 * stride)), tr);
        }
        /* lo: p1 of lines 0-7, then p0; hi: q0, then q1 */
        __m128i lo = _mm_unpacklo_epi32(x[0], x[1]);
        __m128i hi = _mm_unpackhi_epi32(x[0], x[1]);
        __m128i p1 = _mm_cvtepu8_epi16(lo);
        __m128i p0 = _mm_cvtepu8_epi16(_mm_srli_si128(lo, 8));
        __m128i q0 = _mm_cvtepu8_epi16(hi);
        __m128i q1 = _mm_cvtepu8_epi16(_mm_srli_si128(hi, 8));
        chroma8(p1, &p0, &q0, q1, t);
        __m128i v = _mm_unpacklo_epi8(_mm_packus_epi16(p0, p0),
                                      _mm_packus_epi16(q0, q0));
        CV_ST(v, 0); CV_ST(v, 1); CV_ST(v, 2); CV_ST(v, 3);
        CV_ST(v, 4); CV_ST(v, 5); CV_ST(v, 6); CV_ST(v, 7);
    }
    if (i < n)
        n265_dbk_chroma_v_ref(pix + i * stride, stride, tc + i, n - i);
}
#else
/* p1 p0 q0 q1 of eight lines, as four columns of eight */
static inline void cv_cols8(const pixel *b, ptrdiff_t stride, __m128i c[4])
{
    __m128i lo[2], hi[2];
    for (int g = 0; g < 2; g++) {
        const pixel *r = b + 4 * g * stride - 2;
        __m128i a = _mm_unpacklo_epi16(
            _mm_loadl_epi64((const __m128i *)r),
            _mm_loadl_epi64((const __m128i *)(r + stride)));
        __m128i d = _mm_unpacklo_epi16(
            _mm_loadl_epi64((const __m128i *)(r + 2 * stride)),
            _mm_loadl_epi64((const __m128i *)(r + 3 * stride)));
        lo[g] = _mm_unpacklo_epi32(a, d);
        hi[g] = _mm_unpackhi_epi32(a, d);
    }
    c[0] = _mm_unpacklo_epi64(lo[0], lo[1]);
    c[1] = _mm_unpackhi_epi64(lo[0], lo[1]);
    c[2] = _mm_unpacklo_epi64(hi[0], hi[1]);
    c[3] = _mm_unpackhi_epi64(hi[0], hi[1]);
}

#define CV_ST(v, j, row) do { \
    uint32_t w_ = (uint32_t)_mm_extract_epi32(v, j); \
    memcpy(b + (row) * stride - 1, &w_, sizeof(w_)); \
} while (0)

void n265_dbk_chroma_v_sse42(pixel *pix, ptrdiff_t stride, const int16_t *tc,
                             int n)
{
    int i = 0;
    for (; i + 8 <= n; i += 8) {
        __m128i t = _mm_loadu_si128((const __m128i *)(tc + i));
        if (_mm_testz_si128(t, t))
            continue;
        pixel *b = pix + i * stride;
        __m128i c[4];
        cv_cols8(b, stride, c);
        chroma8(c[0], &c[1], &c[2], c[3], t);
        __m128i lo = _mm_unpacklo_epi16(c[1], c[2]);
        __m128i hi = _mm_unpackhi_epi16(c[1], c[2]);
        CV_ST(lo, 0, 0); CV_ST(lo, 1, 1); CV_ST(lo, 2, 2); CV_ST(lo, 3, 3);
        CV_ST(hi, 0, 4); CV_ST(hi, 1, 5); CV_ST(hi, 2, 6); CV_ST(hi, 3, 7);
    }
    if (i < n)
        n265_dbk_chroma_v_ref(pix + i * stride, stride, tc + i, n - i);
}
#endif
