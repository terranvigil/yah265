/*
 * sao_sse42.c - the SAO kernels (N265_ASM_SAO) on SSE4.2.
 *
 * sao_avx2.c's kernels with eight int16 lanes to a vector, at both
 * depths: the band histogram summed band by band under a compare mask
 * over a strip spanning few bands, the edge offset by its category sum
 * sign(c - a) + sign(c - b). An 8-bit row is widened on load and narrowed
 * on store.
 *
 * Range: a count lane sees at most SAO_STRIP rows of eight vectors (a
 * block is at most 64 wide) or SAO_EO_RUN vectors of a row, and a sum
 * lane adds pairs of samples through madd, at most 2 * 1023 a step, in
 * int32. Columns past the last whole vector go to the C reference.
 *
 * Copyright (c) 2026, the yah265 authors. GPL-2.0-or-later; see LICENSE.
 */

#include <smmintrin.h>

#include "encoder/sao.h"
#include "encoder/x86/sse42.h"

#define SAO_STRIP   8
#define SAO_NB_MAX  6
#define SAO_EO_RUN  128
#define SAO_SHIFT   (BIT_DEPTH - 5)

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

static inline int64_t hsum32(__m128i s)
{
    s = _mm_add_epi32(s, _mm_shuffle_epi32(s, 0x4e));
    s = _mm_add_epi32(s, _mm_shuffle_epi32(s, 0xb1));
    return _mm_cvtsi128_si32(s);
}

/* the int16 lanes summed, as int32 pairs */
static inline __m128i pairs(__m128i v)
{
    return _mm_madd_epi16(v, _mm_set1_epi16(1));
}

static inline int hmin16(__m128i v)
{
    return _mm_cvtsi128_si32(_mm_minpos_epu16(v)) & 0xffff;
}

static inline int hmax16(__m128i v)
{
    v = _mm_xor_si128(v, _mm_set1_epi16(-1));
    return 0xffff ^ (_mm_cvtsi128_si32(_mm_minpos_epu16(v)) & 0xffff);
}

void n265_sao_band_stats_sse42(const pixel *src, int src_stride,
                               const pixel *rec, int rec_stride, int w, int h,
                               int64_t bn[32], int64_t bd[32])
{
    int wv = w & ~7;
    if (wv < w)
        n265_sao_band_stats_ref(src + wv, src_stride, rec + wv, rec_stride,
                                w - wv, h, bn, bd);
    for (int y0 = 0; wv && y0 < h; y0 += SAO_STRIP) {
        int sh = h - y0 < SAO_STRIP ? h - y0 : SAO_STRIP;
        const pixel *s = src + (size_t)y0 * src_stride;
        const pixel *r = rec + (size_t)y0 * rec_stride;

        __m128i mn = _mm_set1_epi16(-1), mx = _mm_setzero_si128();
        for (int y = 0; y < sh; y++)
            for (int x = 0; x < wv; x += 8) {
                __m128i v = ld8(r + (size_t)y * rec_stride + x);
                mn = _mm_min_epu16(mn, v);
                mx = _mm_max_epu16(mx, v);
            }
        int b0 = hmin16(mn) >> SAO_SHIFT, b1 = hmax16(mx) >> SAO_SHIFT;
        if (b1 - b0 >= SAO_NB_MAX) {
            n265_sao_band_stats_ref(s, src_stride, r, rec_stride, wv, sh,
                                    bn, bd);
            continue;
        }
        for (int b = b0; b <= b1; b++) {
            __m128i bv = _mm_set1_epi16((int16_t)b);
            __m128i cnt = _mm_setzero_si128();
            __m128i ssum = _mm_setzero_si128(), rsum = ssum;
            for (int y = 0; y < sh; y++)
                for (int x = 0; x < wv; x += 8) {
                    __m128i rv = ld8(r + (size_t)y * rec_stride + x);
                    __m128i sv = ld8(s + (size_t)y * src_stride + x);
                    __m128i m = _mm_cmpeq_epi16(_mm_srli_epi16(rv, SAO_SHIFT),
                                                bv);
                    cnt = _mm_sub_epi16(cnt, m);
                    ssum = _mm_add_epi32(ssum, pairs(_mm_and_si128(sv, m)));
                    rsum = _mm_add_epi32(rsum, pairs(_mm_and_si128(rv, m)));
                }
            bn[b] += hsum32(pairs(cnt));
            bd[b] += hsum32(ssum) - hsum32(rsum);
        }
    }
}

/* sign(c - a) + sign(c - b), -2..2 per int16 lane */
static inline __m128i eo_cat2(__m128i c, __m128i a, __m128i b)
{
    __m128i sa = _mm_sub_epi16(_mm_cmpgt_epi16(a, c), _mm_cmpgt_epi16(c, a));
    __m128i sb = _mm_sub_epi16(_mm_cmpgt_epi16(b, c), _mm_cmpgt_epi16(c, b));
    return _mm_add_epi16(sa, sb);
}

void n265_sao_eo_stats_sse42(const pixel *src, const pixel *cur,
                             const pixel *a, const pixel *b, int n,
                             int64_t en[5], int64_t ed[5])
{
    static const int16_t sgn[4] = { -2, -1, 1, 2 };   /* categories 1..4 */
    int nv = n & ~7;
    for (int x0 = 0; x0 < nv; x0 += 8 * SAO_EO_RUN) {
        int x1 = x0 + 8 * SAO_EO_RUN < nv ? x0 + 8 * SAO_EO_RUN : nv;
        __m128i cnt[4], ssum[4], csum[4];
        for (int k = 0; k < 4; k++)
            cnt[k] = ssum[k] = csum[k] = _mm_setzero_si128();
        for (int x = x0; x < x1; x += 8) {
            __m128i c = ld8(cur + x), sv = ld8(src + x);
            __m128i s = eo_cat2(c, ld8(a + x), ld8(b + x));
            for (int k = 0; k < 4; k++) {
                __m128i m = _mm_cmpeq_epi16(s, _mm_set1_epi16(sgn[k]));
                cnt[k] = _mm_sub_epi16(cnt[k], m);
                ssum[k] = _mm_add_epi32(ssum[k], pairs(_mm_and_si128(sv, m)));
                csum[k] = _mm_add_epi32(csum[k], pairs(_mm_and_si128(c, m)));
            }
        }
        for (int k = 0; k < 4; k++) {
            en[k + 1] += hsum32(pairs(cnt[k]));
            ed[k + 1] += hsum32(ssum[k]) - hsum32(csum[k]);
        }
    }
    if (nv < n)
        n265_sao_eo_stats_ref(src + nv, cur + nv, a + nv, b + nv, n - nv,
                              en, ed);
}

static inline __m128i clip1(__m128i v)
{
    return _mm_max_epi16(_mm_min_epi16(v, _mm_set1_epi16(PIXEL_MAX)),
                         _mm_setzero_si128());
}

/* o[i] where idx == key[i], 0 elsewhere */
static inline __m128i pick4(__m128i idx, const int16_t key[4],
                            const int8_t off[4])
{
    __m128i o = _mm_setzero_si128();
    for (int i = 0; i < 4; i++)
        o = _mm_or_si128(o, _mm_and_si128(
            _mm_cmpeq_epi16(idx, _mm_set1_epi16(key[i])),
            _mm_set1_epi16(off[i])));
    return o;
}

void n265_sao_eo_apply_sse42(pixel *dst, const pixel *cur, const pixel *a,
                             const pixel *b, int n, const int8_t off[4])
{
    static const int16_t sgn[4] = { -2, -1, 1, 2 };
    int nv = n & ~7;
    for (int x = 0; x < nv; x += 8) {
        __m128i c = ld8(cur + x);
        __m128i s = eo_cat2(c, ld8(a + x), ld8(b + x));
        __m128i res = clip1(_mm_add_epi16(c, pick4(s, sgn, off)));
        __m128i keep = _mm_cmpeq_epi16(s, _mm_setzero_si128());
        st8(dst + x, _mm_blendv_epi8(res, ld8(dst + x), keep));
    }
    if (nv < n)
        n265_sao_eo_apply_ref(dst + nv, cur + nv, a + nv, b + nv, n - nv, off);
}

void n265_sao_bo_apply_sse42(pixel *dst, const pixel *in, int n, int pos,
                             const int8_t off[4])
{
    static const int16_t idx[4] = { 0, 1, 2, 3 };
    __m128i pv = _mm_set1_epi16((int16_t)pos);
    int nv = n & ~7;
    for (int x = 0; x < nv; x += 8) {
        __m128i v = ld8(in + x);
        __m128i i = _mm_sub_epi16(_mm_srli_epi16(v, SAO_SHIFT), pv);
        __m128i in4 = _mm_and_si128(_mm_cmpgt_epi16(i, _mm_set1_epi16(-1)),
                                    _mm_cmpgt_epi16(_mm_set1_epi16(4), i));
        __m128i res = clip1(_mm_add_epi16(v, pick4(i, idx, off)));
        st8(dst + x, _mm_blendv_epi8(ld8(dst + x), res, in4));
    }
    if (nv < n)
        n265_sao_bo_apply_ref(dst + nv, in + nv, n - nv, pos, off);
}
