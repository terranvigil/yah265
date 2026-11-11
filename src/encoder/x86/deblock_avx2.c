/*
 * deblock_avx2.c - the deblocking filter kernels (N265_ASM_DBK) on AVX2.
 *
 * The entry points are src/encoder/arm/neon.h's deblock block, renamed,
 * and the arithmetic is deblock_neon.c's: one int16 lane per sample, every
 * filter output computed for every lane, the decisions picking between
 * them. A ymm holds four four-line segments, two per 128-bit half, so a
 * segment's line-0 and line-3 decisions reach its lanes with one in-lane
 * pshufb.
 *
 * A vertical edge's sixteen lines are transposed into lanes and back and
 * stored whole, so p3 and q3 are rewritten with the values they held. A
 * tail under four segments, or under sixteen chroma lines, goes to the C
 * reference.
 *
 * Copyright (c) 2026, the yah265 authors. GPL-2.0-or-later; see LICENSE.
 */

#include <immintrin.h>
#include <string.h>

#include "encoder/deblock.h"
#include "encoder/x86/avx2.h"

static inline __m256i ld16(const pixel *p)
{
    return _mm256_cvtepu8_epi16(_mm_loadu_si128((const __m128i *)p));
}

static inline __m128i pack16(__m256i v)
{
    return _mm_packus_epi16(_mm256_castsi256_si128(v),
                            _mm256_extracti128_si256(v, 1));
}

static inline __m256i clamp3(__m256i v, __m256i lo, __m256i hi)
{
    return _mm256_max_epi16(_mm256_min_epi16(v, hi), lo);
}

static inline __m256i clip1(__m256i v)
{
    return clamp3(v, _mm256_setzero_si256(), _mm256_set1_epi16(PIXEL_MAX));
}

static inline __m256i lt(__m256i a, __m256i b)
{
    return _mm256_cmpgt_epi16(b, a);
}

static inline __m256i rshr3(__m256i v)
{
    return _mm256_srai_epi16(_mm256_add_epi16(v, _mm256_set1_epi16(4)), 3);
}

static inline __m256i rshr2(__m256i v)
{
    return _mm256_srai_epi16(_mm256_add_epi16(v, _mm256_set1_epi16(2)), 2);
}

/* {v0 x4, v1 x4, v2 x4, v3 x4} from four int16 */
static inline __m256i seg_quad(const int16_t *v)
{
    __m128i b = _mm_loadl_epi64((const __m128i *)v);
    __m128i bb = _mm_unpacklo_epi16(b, b);
    return _mm256_set_m128i(_mm_unpackhi_epi32(bb, bb),
                            _mm_unpacklo_epi32(bb, bb));
}

static inline int quad_zero(const int16_t *v)
{
    uint64_t w;
    memcpy(&w, v, sizeof(w));
    return !w;
}

/* v[0..7] = p3..q3 across the edge, one line per lane; rewrites p2..q2.
 * Returns 0, touching nothing, when no segment is filtered. */
static inline int luma_quad(__m256i v[8], __m256i beta, __m256i tc)
{
    __m256i p3 = v[0], p2 = v[1], p1 = v[2], p0 = v[3];
    __m256i q0 = v[4], q1 = v[5], q2 = v[6], q3 = v[7];
    const __m256i i0 = _mm256_setr_epi8(0, 1, 0, 1, 0, 1, 0, 1,
                                        8, 9, 8, 9, 8, 9, 8, 9,
                                        0, 1, 0, 1, 0, 1, 0, 1,
                                        8, 9, 8, 9, 8, 9, 8, 9);
    const __m256i i3 = _mm256_setr_epi8(6, 7, 6, 7, 6, 7, 6, 7,
                                        14, 15, 14, 15, 14, 15, 14, 15,
                                        6, 7, 6, 7, 6, 7, 6, 7,
                                        14, 15, 14, 15, 14, 15, 14, 15);

    __m256i dp = _mm256_abs_epi16(_mm256_sub_epi16(_mm256_add_epi16(p2, p0),
                                                   _mm256_slli_epi16(p1, 1)));
    __m256i dq = _mm256_abs_epi16(_mm256_sub_epi16(_mm256_add_epi16(q2, q0),
                                                   _mm256_slli_epi16(q1, 1)));
    __m256i dp03 = _mm256_add_epi16(_mm256_shuffle_epi8(dp, i0),
                                    _mm256_shuffle_epi8(dp, i3));
    __m256i dq03 = _mm256_add_epi16(_mm256_shuffle_epi8(dq, i0),
                                    _mm256_shuffle_epi8(dq, i3));
    __m256i on = lt(_mm256_add_epi16(dp03, dq03), beta);
    if (!_mm256_movemask_epi8(on))
        return 0;

    /* 8.7.2.5.6 per line, then both of a segment's lines must agree */
    __m256i tc5 = _mm256_srai_epi16(
        _mm256_add_epi16(_mm256_mullo_epi16(tc, _mm256_set1_epi16(5)),
                         _mm256_set1_epi16(1)), 1);
    __m256i sl = _mm256_and_si256(
        _mm256_and_si256(
            lt(_mm256_slli_epi16(_mm256_add_epi16(dp, dq), 1),
               _mm256_srai_epi16(beta, 2)),
            lt(_mm256_add_epi16(_mm256_abs_epi16(_mm256_sub_epi16(p3, p0)),
                                _mm256_abs_epi16(_mm256_sub_epi16(q0, q3))),
               _mm256_srai_epi16(beta, 3))),
        lt(_mm256_abs_epi16(_mm256_sub_epi16(p0, q0)), tc5));
    __m256i strong = _mm256_and_si256(
        _mm256_and_si256(_mm256_shuffle_epi8(sl, i0),
                         _mm256_shuffle_epi8(sl, i3)), on);
    __m256i side = _mm256_srai_epi16(
        _mm256_add_epi16(beta, _mm256_srai_epi16(beta, 1)), 3);
    __m256i dep = lt(dp03, side), deq = lt(dq03, side);

    /* strong filter */
    __m256i tc2 = _mm256_slli_epi16(tc, 1);
    __m256i pq0 = _mm256_add_epi16(p0, q0);
    __m256i three = _mm256_set1_epi16(3);
    __m256i sp0 = rshr3(_mm256_add_epi16(_mm256_add_epi16(p2, q1),
        _mm256_slli_epi16(_mm256_add_epi16(p1, pq0), 1)));
    __m256i sp1 = rshr2(_mm256_add_epi16(_mm256_add_epi16(p2, p1), pq0));
    __m256i sp2 = rshr3(_mm256_add_epi16(
        _mm256_add_epi16(_mm256_slli_epi16(p3, 1), _mm256_mullo_epi16(p2, three)),
        _mm256_add_epi16(p1, pq0)));
    __m256i sq0 = rshr3(_mm256_add_epi16(_mm256_add_epi16(q2, p1),
        _mm256_slli_epi16(_mm256_add_epi16(q1, pq0), 1)));
    __m256i sq1 = rshr2(_mm256_add_epi16(_mm256_add_epi16(q2, q1), pq0));
    __m256i sq2 = rshr3(_mm256_add_epi16(
        _mm256_add_epi16(_mm256_slli_epi16(q3, 1), _mm256_mullo_epi16(q2, three)),
        _mm256_add_epi16(q1, pq0)));
    sp0 = clip1(clamp3(sp0, _mm256_sub_epi16(p0, tc2), _mm256_add_epi16(p0, tc2)));
    sp1 = clip1(clamp3(sp1, _mm256_sub_epi16(p1, tc2), _mm256_add_epi16(p1, tc2)));
    sp2 = clip1(clamp3(sp2, _mm256_sub_epi16(p2, tc2), _mm256_add_epi16(p2, tc2)));
    sq0 = clip1(clamp3(sq0, _mm256_sub_epi16(q0, tc2), _mm256_add_epi16(q0, tc2)));
    sq1 = clip1(clamp3(sq1, _mm256_sub_epi16(q1, tc2), _mm256_add_epi16(q1, tc2)));
    sq2 = clip1(clamp3(sq2, _mm256_sub_epi16(q2, tc2), _mm256_add_epi16(q2, tc2)));

    /* normal filter */
    __m256i ntc = _mm256_sub_epi16(_mm256_setzero_si256(), tc);
    __m256i delta = _mm256_srai_epi16(_mm256_add_epi16(_mm256_sub_epi16(
        _mm256_mullo_epi16(_mm256_sub_epi16(q0, p0), _mm256_set1_epi16(9)),
        _mm256_mullo_epi16(_mm256_sub_epi16(q1, p1), three)),
        _mm256_set1_epi16(8)), 4);
    __m256i nok = lt(_mm256_abs_epi16(delta),
                     _mm256_mullo_epi16(tc, _mm256_set1_epi16(10)));
    delta = clamp3(delta, ntc, tc);
    __m256i np0 = clip1(_mm256_add_epi16(p0, delta));
    __m256i nq0 = clip1(_mm256_sub_epi16(q0, delta));
    __m256i tch = _mm256_srai_epi16(tc, 1);
    __m256i ntch = _mm256_sub_epi16(_mm256_setzero_si256(), tch);
    __m256i np1 = clip1(_mm256_add_epi16(p1, clamp3(_mm256_srai_epi16(
        _mm256_add_epi16(_mm256_sub_epi16(_mm256_avg_epu16(p2, p0), p1), delta),
        1), ntch, tch)));
    __m256i nq1 = clip1(_mm256_add_epi16(q1, clamp3(_mm256_srai_epi16(
        _mm256_sub_epi16(_mm256_sub_epi16(_mm256_avg_epu16(q2, q0), q1), delta),
        1), ntch, tch)));

    __m256i nm = _mm256_andnot_si256(strong, _mm256_and_si256(on, nok));
    v[1] = _mm256_blendv_epi8(p2, sp2, strong);
    v[2] = _mm256_blendv_epi8(
        _mm256_blendv_epi8(p1, np1, _mm256_and_si256(nm, dep)), sp1, strong);
    v[3] = _mm256_blendv_epi8(_mm256_blendv_epi8(p0, np0, nm), sp0, strong);
    v[4] = _mm256_blendv_epi8(_mm256_blendv_epi8(q0, nq0, nm), sq0, strong);
    v[5] = _mm256_blendv_epi8(
        _mm256_blendv_epi8(q1, nq1, _mm256_and_si256(nm, deq)), sq1, strong);
    v[6] = _mm256_blendv_epi8(q2, sq2, strong);
    return 1;
}

void n265_dbk_luma_h_avx2(pixel *pix, ptrdiff_t stride, const int16_t *beta,
                          const int16_t *tc, int nseg)
{
    int k = 0;
    for (; k + 4 <= nseg; k += 4) {
        if (quad_zero(tc + k))
            continue;
        pixel *b = pix + 4 * k;
        __m256i r[8];
        for (int i = 0; i < 8; i++)
            r[i] = ld16(b + (i - 4) * stride);
        if (!luma_quad(r, seg_quad(beta + k), seg_quad(tc + k)))
            continue;
        for (int i = 1; i < 7; i++)
            _mm_storeu_si128((__m128i *)(b + (i - 4) * stride), pack16(r[i]));
    }
    if (k < nseg)
        n265_dbk_luma_h_ref(pix + 4 * k, stride, beta + k, tc + k, nseg - k);
}

/* sixteen rows of eight bytes <-> eight columns of sixteen */
static inline void rows_to_cols(const pixel *b, ptrdiff_t stride,
                                __m128i c[8])
{
    __m128i a[8], t[8], u[8];
    for (int i = 0; i < 8; i++)
        a[i] = _mm_unpacklo_epi8(
            _mm_loadl_epi64((const __m128i *)(b + 2 * i * stride)),
            _mm_loadl_epi64((const __m128i *)(b + (2 * i + 1) * stride)));
    for (int i = 0; i < 4; i++) {
        t[2 * i] = _mm_unpacklo_epi16(a[2 * i], a[2 * i + 1]);
        t[2 * i + 1] = _mm_unpackhi_epi16(a[2 * i], a[2 * i + 1]);
    }
    /* t[0], t[1]: rows 0-3, columns 0-3 and 4-7; t[2], t[3]: rows 4-7 ... */
    for (int h = 0; h < 2; h++)
        for (int j = 0; j < 2; j++) {
            u[4 * h + 2 * j] = _mm_unpacklo_epi32(t[4 * h + j], t[4 * h + j + 2]);
            u[4 * h + 2 * j + 1] = _mm_unpackhi_epi32(t[4 * h + j], t[4 * h + j + 2]);
        }
    /* u[0..3]: rows 0-7, column pairs 01 23 45 67; u[4..7]: rows 8-15 */
    for (int j = 0; j < 4; j++) {
        c[2 * j] = _mm_unpacklo_epi64(u[j], u[j + 4]);
        c[2 * j + 1] = _mm_unpackhi_epi64(u[j], u[j + 4]);
    }
}

static inline void cols_to_rows(pixel *b, ptrdiff_t stride, const __m128i c[8])
{
    __m128i d[8], e[8];
    for (int j = 0; j < 4; j++) {
        d[2 * j] = _mm_unpacklo_epi8(c[2 * j], c[2 * j + 1]);
        d[2 * j + 1] = _mm_unpackhi_epi8(c[2 * j], c[2 * j + 1]);
    }
    /* d[2j]: lines 0-7 of columns 2j, 2j+1; d[2j+1]: lines 8-15 */
    for (int h = 0; h < 2; h++) {
        e[4 * h] = _mm_unpacklo_epi16(d[h], d[h + 2]);
        e[4 * h + 1] = _mm_unpackhi_epi16(d[h], d[h + 2]);
        e[4 * h + 2] = _mm_unpacklo_epi16(d[h + 4], d[h + 6]);
        e[4 * h + 3] = _mm_unpackhi_epi16(d[h + 4], d[h + 6]);
    }
    /* e[4h], e[4h+1]: columns 0-3 of lines 8h..8h+3, 8h+4..8h+7;
     * e[4h+2], e[4h+3]: columns 4-7 of the same lines */
    for (int h = 0; h < 2; h++)
        for (int j = 0; j < 2; j++) {
            __m128i lo = _mm_unpacklo_epi32(e[4 * h + j], e[4 * h + j + 2]);
            __m128i hi = _mm_unpackhi_epi32(e[4 * h + j], e[4 * h + j + 2]);
            pixel *r = b + (8 * h + 4 * j) * stride;
            _mm_storel_epi64((__m128i *)r, lo);
            _mm_storeh_pd((double *)(void *)(r + stride), _mm_castsi128_pd(lo));
            _mm_storel_epi64((__m128i *)(r + 2 * stride), hi);
            _mm_storeh_pd((double *)(void *)(r + 3 * stride), _mm_castsi128_pd(hi));
        }
}

void n265_dbk_luma_v_avx2(pixel *pix, ptrdiff_t stride, const int16_t *beta,
                          const int16_t *tc, int nseg)
{
    int k = 0;
    for (; k + 4 <= nseg; k += 4) {
        if (quad_zero(tc + k))
            continue;
        pixel *b = pix + 4 * k * stride - 4;
        __m128i c[8];
        __m256i r[8];
        rows_to_cols(b, stride, c);
        for (int i = 0; i < 8; i++)
            r[i] = _mm256_cvtepu8_epi16(c[i]);
        if (!luma_quad(r, seg_quad(beta + k), seg_quad(tc + k)))
            continue;
        for (int i = 1; i < 7; i++)
            c[i] = pack16(r[i]);
        cols_to_rows(b, stride, c);
    }
    if (k < nseg)
        n265_dbk_luma_v_ref(pix + 4 * k * stride, stride, beta + k, tc + k,
                            nseg - k);
}

/* 8.7.2.5.5 on sixteen lines: returns p0' and q0' */
static inline void chroma16(__m256i p1, __m256i *p0, __m256i *q0, __m256i q1,
                            __m256i tc)
{
    __m256i d = rshr3(_mm256_sub_epi16(_mm256_add_epi16(
        _mm256_slli_epi16(_mm256_sub_epi16(*q0, *p0), 2), p1), q1));
    d = clamp3(d, _mm256_sub_epi16(_mm256_setzero_si256(), tc), tc);
    *p0 = clip1(_mm256_add_epi16(*p0, d));
    *q0 = clip1(_mm256_sub_epi16(*q0, d));
}

void n265_dbk_chroma_h_avx2(pixel *pix, ptrdiff_t stride, const int16_t *tc,
                            int n)
{
    int i = 0;
    for (; i + 16 <= n; i += 16) {
        __m256i t = _mm256_loadu_si256((const __m256i *)(tc + i));
        if (_mm256_testz_si256(t, t))
            continue;
        pixel *b = pix + i;
        __m256i p1 = ld16(b - 2 * stride), p0 = ld16(b - stride);
        __m256i q0 = ld16(b), q1 = ld16(b + stride);
        chroma16(p1, &p0, &q0, q1, t);
        _mm_storeu_si128((__m128i *)(b - stride), pack16(p0));
        _mm_storeu_si128((__m128i *)b, pack16(q0));
    }
    if (i < n)
        n265_dbk_chroma_h_ref(pix + i, stride, tc + i, n - i);
}

static inline int ld4(const pixel *p)
{
    int v;
    memcpy(&v, p, sizeof(v));
    return v;
}

#define CV_ST(v, j, row) do { \
    uint16_t w_ = (uint16_t)_mm_extract_epi16(v, j); \
    memcpy(b + (row) * stride - 1, &w_, sizeof(w_)); \
} while (0)

void n265_dbk_chroma_v_avx2(pixel *pix, ptrdiff_t stride, const int16_t *tc,
                            int n)
{
    /* a 4x4 byte transpose inside each 32-bit group of four rows */
    const __m128i tr = _mm_setr_epi8(0, 4, 8, 12, 1, 5, 9, 13,
                                     2, 6, 10, 14, 3, 7, 11, 15);
    int i = 0;
    for (; i + 16 <= n; i += 16) {
        __m256i t = _mm256_loadu_si256((const __m256i *)(tc + i));
        if (_mm256_testz_si256(t, t))
            continue;
        pixel *b = pix + i * stride;
        __m128i x[4];
        for (int g = 0; g < 4; g++) {
            const pixel *r = b + 4 * g * stride - 2;
            x[g] = _mm_shuffle_epi8(_mm_setr_epi32(ld4(r), ld4(r + stride),
                                                   ld4(r + 2 * stride),
                                                   ld4(r + 3 * stride)), tr);
        }
        __m128i lo01 = _mm_unpacklo_epi32(x[0], x[1]);
        __m128i hi01 = _mm_unpackhi_epi32(x[0], x[1]);
        __m128i lo23 = _mm_unpacklo_epi32(x[2], x[3]);
        __m128i hi23 = _mm_unpackhi_epi32(x[2], x[3]);
        __m256i p1 = _mm256_cvtepu8_epi16(_mm_unpacklo_epi64(lo01, lo23));
        __m256i p0 = _mm256_cvtepu8_epi16(_mm_unpackhi_epi64(lo01, lo23));
        __m256i q0 = _mm256_cvtepu8_epi16(_mm_unpacklo_epi64(hi01, hi23));
        __m256i q1 = _mm256_cvtepu8_epi16(_mm_unpackhi_epi64(hi01, hi23));
        chroma16(p1, &p0, &q0, q1, t);
        __m128i pb = pack16(p0), qb = pack16(q0);
        __m128i lo = _mm_unpacklo_epi8(pb, qb), hi = _mm_unpackhi_epi8(pb, qb);
        CV_ST(lo, 0, 0);  CV_ST(lo, 1, 1);  CV_ST(lo, 2, 2);  CV_ST(lo, 3, 3);
        CV_ST(lo, 4, 4);  CV_ST(lo, 5, 5);  CV_ST(lo, 6, 6);  CV_ST(lo, 7, 7);
        CV_ST(hi, 0, 8);  CV_ST(hi, 1, 9);  CV_ST(hi, 2, 10); CV_ST(hi, 3, 11);
        CV_ST(hi, 4, 12); CV_ST(hi, 5, 13); CV_ST(hi, 6, 14); CV_ST(hi, 7, 15);
    }
    if (i < n)
        n265_dbk_chroma_v_ref(pix + i * stride, stride, tc + i, n - i);
}
