/*
 * sao_avx2.c - the SAO kernels (N265_ASM_SAO) on AVX2.
 *
 * The entry points are src/encoder/arm/neon.h's sao block, renamed, and
 * the method is sao_neon.c's: the band histogram summed band by band under
 * a compare mask over a strip spanning few bands, the edge offset by its
 * category sum sign(c - a) + sign(c - b). Both depths run sixteen int16
 * lanes, an 8-bit row widened on load and narrowed on store, so one body
 * serves both.
 *
 * Range: a count lane sees at most SAO_STRIP rows of four vectors (a
 * block is at most 64 wide) or SAO_EO_RUN vectors of a row, and a sum
 * lane adds pairs of samples through madd, at most 2 * 1023 a step, in
 * int32. Columns past the last whole vector go to the C reference.
 *
 * Copyright (c) 2026, the yah265 authors. GPL-2.0-or-later; see LICENSE.
 */

#include <immintrin.h>

#include "encoder/sao.h"
#include "encoder/x86/avx2.h"

#define SAO_STRIP   8
#define SAO_NB_MAX  6
#define SAO_EO_RUN  64
#define SAO_SHIFT   (BIT_DEPTH - 5)

#if BIT_DEPTH == 8
static inline __m256i ld16(const pixel *p)
{
    return _mm256_cvtepu8_epi16(_mm_loadu_si128((const __m128i *)p));
}

static inline void st16(pixel *p, __m256i v)
{
    _mm_storeu_si128((__m128i *)p,
                     _mm_packus_epi16(_mm256_castsi256_si128(v),
                                      _mm256_extracti128_si256(v, 1)));
}
#else
static inline __m256i ld16(const pixel *p)
{
    return _mm256_loadu_si256((const __m256i *)p);
}

static inline void st16(pixel *p, __m256i v)
{
    _mm256_storeu_si256((__m256i *)p, v);
}
#endif

static inline int64_t hsum32(__m256i v)
{
    __m128i s = _mm_add_epi32(_mm256_castsi256_si128(v),
                              _mm256_extracti128_si256(v, 1));
    s = _mm_add_epi32(s, _mm_shuffle_epi32(s, 0x4e));
    s = _mm_add_epi32(s, _mm_shuffle_epi32(s, 0xb1));
    return _mm_cvtsi128_si32(s);
}

/* the int16 lanes summed, as int32 pairs */
static inline __m256i pairs(__m256i v)
{
    return _mm256_madd_epi16(v, _mm256_set1_epi16(1));
}

static inline int hmin16(__m256i v)
{
    __m128i m = _mm_min_epu16(_mm256_castsi256_si128(v),
                              _mm256_extracti128_si256(v, 1));
    return _mm_cvtsi128_si32(_mm_minpos_epu16(m)) & 0xffff;
}

static inline int hmax16(__m256i v)
{
    __m128i m = _mm_max_epu16(_mm256_castsi256_si128(v),
                              _mm256_extracti128_si256(v, 1));
    m = _mm_xor_si128(m, _mm_set1_epi16(-1));
    return 0xffff ^ (_mm_cvtsi128_si32(_mm_minpos_epu16(m)) & 0xffff);
}

void n265_sao_band_stats_avx2(const pixel *src, int src_stride,
                              const pixel *rec, int rec_stride, int w, int h,
                              int64_t bn[32], int64_t bd[32])
{
    int wv = w & ~15;
    if (wv < w)
        n265_sao_band_stats_ref(src + wv, src_stride, rec + wv, rec_stride,
                                w - wv, h, bn, bd);
    for (int y0 = 0; wv && y0 < h; y0 += SAO_STRIP) {
        int sh = h - y0 < SAO_STRIP ? h - y0 : SAO_STRIP;
        const pixel *s = src + (size_t)y0 * src_stride;
        const pixel *r = rec + (size_t)y0 * rec_stride;

        __m256i mn = _mm256_set1_epi16(-1), mx = _mm256_setzero_si256();
        for (int y = 0; y < sh; y++)
            for (int x = 0; x < wv; x += 16) {
                __m256i v = ld16(r + (size_t)y * rec_stride + x);
                mn = _mm256_min_epu16(mn, v);
                mx = _mm256_max_epu16(mx, v);
            }
        int b0 = hmin16(mn) >> SAO_SHIFT, b1 = hmax16(mx) >> SAO_SHIFT;
        if (b1 - b0 >= SAO_NB_MAX) {
            n265_sao_band_stats_ref(s, src_stride, r, rec_stride, wv, sh,
                                    bn, bd);
            continue;
        }
        for (int b = b0; b <= b1; b++) {
            __m256i bv = _mm256_set1_epi16((int16_t)b);
            __m256i cnt = _mm256_setzero_si256();
            __m256i ssum = _mm256_setzero_si256(), rsum = ssum;
            for (int y = 0; y < sh; y++)
                for (int x = 0; x < wv; x += 16) {
                    __m256i rv = ld16(r + (size_t)y * rec_stride + x);
                    __m256i sv = ld16(s + (size_t)y * src_stride + x);
                    __m256i m = _mm256_cmpeq_epi16(
                        _mm256_srli_epi16(rv, SAO_SHIFT), bv);
                    cnt = _mm256_sub_epi16(cnt, m);
                    ssum = _mm256_add_epi32(ssum,
                                            pairs(_mm256_and_si256(sv, m)));
                    rsum = _mm256_add_epi32(rsum,
                                            pairs(_mm256_and_si256(rv, m)));
                }
            bn[b] += hsum32(pairs(cnt));
            bd[b] += hsum32(ssum) - hsum32(rsum);
        }
    }
}

/* sign(c - a) + sign(c - b), -2..2 per int16 lane */
static inline __m256i eo_cat2(__m256i c, __m256i a, __m256i b)
{
    __m256i sa = _mm256_sub_epi16(_mm256_cmpgt_epi16(a, c),
                                  _mm256_cmpgt_epi16(c, a));
    __m256i sb = _mm256_sub_epi16(_mm256_cmpgt_epi16(b, c),
                                  _mm256_cmpgt_epi16(c, b));
    return _mm256_add_epi16(sa, sb);
}

void n265_sao_eo_stats_avx2(const pixel *src, const pixel *cur,
                            const pixel *a, const pixel *b, int n,
                            int64_t en[5], int64_t ed[5])
{
    static const int16_t sgn[4] = { -2, -1, 1, 2 };   /* categories 1..4 */
    int nv = n & ~15;
    for (int x0 = 0; x0 < nv; x0 += 16 * SAO_EO_RUN) {
        int x1 = x0 + 16 * SAO_EO_RUN < nv ? x0 + 16 * SAO_EO_RUN : nv;
        __m256i cnt[4], ssum[4], csum[4];
        for (int k = 0; k < 4; k++)
            cnt[k] = ssum[k] = csum[k] = _mm256_setzero_si256();
        for (int x = x0; x < x1; x += 16) {
            __m256i c = ld16(cur + x), sv = ld16(src + x);
            __m256i s = eo_cat2(c, ld16(a + x), ld16(b + x));
            for (int k = 0; k < 4; k++) {
                __m256i m = _mm256_cmpeq_epi16(s, _mm256_set1_epi16(sgn[k]));
                cnt[k] = _mm256_sub_epi16(cnt[k], m);
                ssum[k] = _mm256_add_epi32(ssum[k],
                                           pairs(_mm256_and_si256(sv, m)));
                csum[k] = _mm256_add_epi32(csum[k],
                                           pairs(_mm256_and_si256(c, m)));
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

static inline __m256i clip1(__m256i v)
{
    return _mm256_max_epi16(_mm256_min_epi16(v, _mm256_set1_epi16(PIXEL_MAX)),
                            _mm256_setzero_si256());
}

/* o[i] where idx == key[i], 0 elsewhere */
static inline __m256i pick4(__m256i idx, const int16_t key[4],
                            const int8_t off[4])
{
    __m256i o = _mm256_setzero_si256();
    for (int i = 0; i < 4; i++)
        o = _mm256_or_si256(o, _mm256_and_si256(
            _mm256_cmpeq_epi16(idx, _mm256_set1_epi16(key[i])),
            _mm256_set1_epi16(off[i])));
    return o;
}

void n265_sao_eo_apply_avx2(pixel *dst, const pixel *cur, const pixel *a,
                            const pixel *b, int n, const int8_t off[4])
{
    static const int16_t sgn[4] = { -2, -1, 1, 2 };
    int nv = n & ~15;
    for (int x = 0; x < nv; x += 16) {
        __m256i c = ld16(cur + x);
        __m256i s = eo_cat2(c, ld16(a + x), ld16(b + x));
        __m256i res = clip1(_mm256_add_epi16(c, pick4(s, sgn, off)));
        __m256i keep = _mm256_cmpeq_epi16(s, _mm256_setzero_si256());
        st16(dst + x, _mm256_blendv_epi8(res, ld16(dst + x), keep));
    }
    if (nv < n)
        n265_sao_eo_apply_ref(dst + nv, cur + nv, a + nv, b + nv, n - nv, off);
}

void n265_sao_bo_apply_avx2(pixel *dst, const pixel *in, int n, int pos,
                            const int8_t off[4])
{
    static const int16_t idx[4] = { 0, 1, 2, 3 };
    __m256i pv = _mm256_set1_epi16((int16_t)pos);
    int nv = n & ~15;
    for (int x = 0; x < nv; x += 16) {
        __m256i v = ld16(in + x);
        __m256i i = _mm256_sub_epi16(_mm256_srli_epi16(v, SAO_SHIFT), pv);
        __m256i in4 = _mm256_and_si256(
            _mm256_cmpgt_epi16(i, _mm256_set1_epi16(-1)),
            _mm256_cmpgt_epi16(_mm256_set1_epi16(4), i));
        __m256i res = clip1(_mm256_add_epi16(v, pick4(i, idx, off)));
        st16(dst + x, _mm256_blendv_epi8(ld16(dst + x), res, in4));
    }
    if (nv < n)
        n265_sao_bo_apply_ref(dst + nv, in + nv, n - nv, pos, off);
}
