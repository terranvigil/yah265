/*
 * mc_sse42.c - the motion-compensation family (N265_ASM_MC) on SSE4.2.
 *
 * mc_avx2.c's kernels in 128-bit registers: a 16-sample chunk is two
 * 8-lane halves, and the 8, 4, 2 and 1 chunks for what is left are the
 * AVX2 file's own 128-bit tails. So any w and h works and every load reads
 * only bytes the C reads. The AVX2 file has the range arguments and the
 * proofs that the narrows are the C's casts and clips; they hold lane for
 * lane.
 *
 * Copyright (c) 2026, the yah265 authors. GPL-2.0-or-later; see LICENSE.
 */

#include <smmintrin.h>
#include <stddef.h>
#include <string.h>

#include "encoder/mc.h"
#include "encoder/x86/sse42.h"

#define AI static inline __attribute__((always_inline))

static const int8_t luma_f[4][8] = {
    {  0, 0,   0, 64,  0,   0, 0,  0 },
    { -1, 4, -10, 58, 17,  -5, 1,  0 },
    { -1, 4, -11, 40, 40, -11, 4, -1 },
    {  0, 1,  -5, 17, 58, -10, 4, -1 },
};
static const int8_t chroma_f[8][4] = {
    {  0, 64,  0,  0 },
    { -2, 58, 10, -2 },
    { -4, 54, 16, -2 },
    { -6, 46, 28, -4 },
    { -4, 36, 36, -4 },
    { -4, 28, 46, -6 },
    { -2, 16, 54, -4 },
    { -2, 10, 58, -2 },
};

/* tap pairs: b[] for maddubs (byte k low, k + 1 high), w[] for madd */
typedef struct { __m128i b[4], w[4]; } taps;

AI void taps_init(taps *t, const int8_t *f, int nt)
{
    for (int j = 0; j < nt / 2; j++) {
        uint16_t bb = (uint16_t)((uint8_t)f[2 * j] |
                                 ((unsigned)(uint8_t)f[2 * j + 1] << 8));
        uint32_t ww = (uint32_t)(uint16_t)f[2 * j] |
                      ((uint32_t)(uint16_t)f[2 * j + 1] << 16);
        t->b[j] = _mm_set1_epi16((short)bb);
        t->w[j] = _mm_set1_epi32((int)ww);
    }
}

/* n = 16, 8, 4, 2 or 1 bytes, exactly */
AI __m128i ld8_n(const uint8_t *p, int n)
{
    int32_t t;
    uint16_t s;
    switch (n) {
    case 16: return _mm_loadu_si128((const __m128i *)p);
    case 8:  return _mm_loadl_epi64((const __m128i *)p);
    case 4:  memcpy(&t, p, 4); return _mm_cvtsi32_si128(t);
    case 2:  memcpy(&s, p, 2); return _mm_cvtsi32_si128(s);
    default: return _mm_cvtsi32_si128(p[0]);
    }
}

AI void st8_n(uint8_t *p, __m128i v, int n)
{
    int32_t t;
    uint16_t s;
    switch (n) {
    case 16: _mm_storeu_si128((__m128i *)p, v); return;
    case 8:  _mm_storel_epi64((__m128i *)p, v); return;
    case 4:  t = _mm_cvtsi128_si32(v); memcpy(p, &t, 4); return;
    case 2:  s = (uint16_t)_mm_cvtsi128_si32(v); memcpy(p, &s, 2); return;
    default: p[0] = (uint8_t)_mm_cvtsi128_si32(v); return;
    }
}

/* n = 8, 4, 2 or 1 int16 samples, exactly */
AI __m128i ld16_n(const int16_t *p, int n)
{
    int32_t t;
    switch (n) {
    case 8:  return _mm_loadu_si128((const __m128i *)p);
    case 4:  return _mm_loadl_epi64((const __m128i *)p);
    case 2:  memcpy(&t, p, 4); return _mm_cvtsi32_si128(t);
    default: return _mm_cvtsi32_si128((uint16_t)p[0]);
    }
}

AI void st16_n(int16_t *p, __m128i v, int n)
{
    int32_t t;
    switch (n) {
    case 8:  _mm_storeu_si128((__m128i *)p, v); return;
    case 4:  _mm_storel_epi64((__m128i *)p, v); return;
    case 2:  t = _mm_cvtsi128_si32(v); memcpy(p, &t, 4); return;
    default: p[0] = (int16_t)_mm_extract_epi16(v, 0); return;
    }
}

AI __m128i ld16(const int16_t *p)
{
    return _mm_loadu_si128((const __m128i *)p);
}

AI void st16(int16_t *p, __m128i v)
{
    _mm_storeu_si128((__m128i *)p, v);
}

AI int chunk_n(int left)
{
    return left >= 8 ? 8 : left >= 4 ? 4 : left >= 2 ? 2 : 1;
}

/* ---- the filters ---------------------------------------------------- */

/* nt-tap filter over u8 at p, p + st, ..., n <= 8 outputs, int16 */
AI __m128i fu8_8(const uint8_t *p, ptrdiff_t st, int n, const taps *t, int nt)
{
    __m128i acc = _mm_setzero_si128();
    for (int j = 0; j < nt / 2; j++) {
        __m128i a = ld8_n(p + 2 * j * st, n);
        __m128i b = ld8_n(p + (2 * j + 1) * st, n);
        acc = _mm_add_epi16(acc, _mm_maddubs_epi16(_mm_unpacklo_epi8(a, b),
                                                   t->b[j]));
    }
    return acc;
}

/* the same for 16 outputs: 0-7 in lo, 8-15 in hi */
AI void fu8_16(const uint8_t *p, ptrdiff_t st, const taps *t, int nt,
               __m128i *lo, __m128i *hi)
{
    __m128i l = _mm_setzero_si128(), h = _mm_setzero_si128();
    for (int j = 0; j < nt / 2; j++) {
        __m128i a = _mm_loadu_si128((const __m128i *)(p + 2 * j * st));
        __m128i b = _mm_loadu_si128((const __m128i *)(p + (2 * j + 1) * st));
        l = _mm_add_epi16(l, _mm_maddubs_epi16(_mm_unpacklo_epi8(a, b),
                                               t->b[j]));
        h = _mm_add_epi16(h, _mm_maddubs_epi16(_mm_unpackhi_epi8(a, b),
                                               t->b[j]));
    }
    *lo = l;
    *hi = h;
}

/* nt-tap filter over int16 rows p, p + st, ..., n <= 8 outputs: int32
 * sums of outputs 0-3 in lo and 4-7 in hi */
AI void fs16_8(const int16_t *p, ptrdiff_t st, int n, const taps *t, int nt,
               __m128i *lo, __m128i *hi)
{
    __m128i l = _mm_setzero_si128(), h = _mm_setzero_si128();
    for (int j = 0; j < nt / 2; j++) {
        __m128i a = ld16_n(p + 2 * j * st, n);
        __m128i b = ld16_n(p + (2 * j + 1) * st, n);
        l = _mm_add_epi32(l, _mm_madd_epi16(_mm_unpacklo_epi16(a, b),
                                            t->w[j]));
        h = _mm_add_epi32(h, _mm_madd_epi16(_mm_unpackhi_epi16(a, b),
                                            t->w[j]));
    }
    *lo = l;
    *hi = h;
}

/* ---- the narrows ---------------------------------------------------- */

/* clip1((v + 32) >> 6) of int16 lanes, to bytes */
AI __m128i rnd6_s16(__m128i v)
{
    return _mm_mulhrs_epi16(v, _mm_set1_epi16(512));
}

AI __m128i rnd6_pix8(__m128i v)
{
    v = rnd6_s16(v);
    return _mm_packus_epi16(v, v);
}

AI __m128i rnd6_pix16(__m128i lo, __m128i hi)
{
    return _mm_packus_epi16(rnd6_s16(lo), rnd6_s16(hi));
}

/* (int16_t)(s >> 6) */
AI __m128i sh6_s16_8(__m128i lo, __m128i hi)
{
    lo = _mm_srai_epi32(_mm_slli_epi32(lo, 10), 16);
    hi = _mm_srai_epi32(_mm_slli_epi32(hi, 10), 16);
    return _mm_packs_epi32(lo, hi);
}

/* clip1((s + 2048) >> 12), as saturated int16 */
AI __m128i sh12_s16(__m128i lo, __m128i hi)
{
    const __m128i r = _mm_set1_epi32(2048);
    lo = _mm_srai_epi32(_mm_add_epi32(lo, r), 12);
    hi = _mm_srai_epi32(_mm_add_epi32(hi, r), 12);
    return _mm_packs_epi32(lo, hi);
}

AI __m128i sh12_pix8(__m128i lo, __m128i hi)
{
    __m128i v = sh12_s16(lo, hi);
    return _mm_packus_epi16(v, v);
}

/* ---- the block drivers ---------------------------------------------- */

/* A one-stage filter over u8: taps along `st` (1 horizontal, rs
 * vertical) from p, which already points at tap 0 of output (0, 0).
 * pix: clip1((s + 32) >> 6) into bytes; otherwise s into int16. */
AI void blk_u8(void *dst, int ds, const uint8_t *p, int rs, ptrdiff_t st,
               int w, int h, const taps *t, int nt, int pix)
{
    for (int y = 0; y < h; y++, p += rs) {
        uint8_t *d8 = (uint8_t *)dst + (ptrdiff_t)y * ds;
        int16_t *d16 = (int16_t *)dst + (ptrdiff_t)y * ds;
        int x = 0;
        for (; x + 16 <= w; x += 16) {
            __m128i lo, hi;
            fu8_16(p + x, st, t, nt, &lo, &hi);
            if (pix) {
                _mm_storeu_si128((__m128i *)(d8 + x), rnd6_pix16(lo, hi));
            } else {
                st16(d16 + x, lo);
                st16(d16 + x + 8, hi);
            }
        }
        while (x < w) {
            int n = chunk_n(w - x);
            __m128i v = fu8_8(p + x, st, n, t, nt);
            if (pix)
                st8_n(d8 + x, rnd6_pix8(v), n);
            else
                st16_n(d16 + x, v, n);
            x += n;
        }
    }
}

/* the vertical filter over int16 rows; p points at tap 0 of output
 * (0, 0). pix: clip1((s + 2048) >> 12); otherwise (int16_t)(s >> 6). */
AI void blk_s16(void *dst, int ds, const int16_t *p, int hs, int w, int h,
                const taps *t, int nt, int pix)
{
    for (int y = 0; y < h; y++, p += hs) {
        uint8_t *d8 = (uint8_t *)dst + (ptrdiff_t)y * ds;
        int16_t *d16 = (int16_t *)dst + (ptrdiff_t)y * ds;
        int x = 0;
        for (; x + 16 <= w; x += 16) {
            __m128i l0, h0, l1, h1;
            fs16_8(p + x, hs, 8, t, nt, &l0, &h0);
            fs16_8(p + x + 8, hs, 8, t, nt, &l1, &h1);
            if (pix) {
                _mm_storeu_si128((__m128i *)(d8 + x),
                                 _mm_packus_epi16(sh12_s16(l0, h0),
                                                  sh12_s16(l1, h1)));
            } else {
                st16(d16 + x, sh6_s16_8(l0, h0));
                st16(d16 + x + 8, sh6_s16_8(l1, h1));
            }
        }
        while (x < w) {
            int n = chunk_n(w - x);
            __m128i lo, hi;
            fs16_8(p + x, hs, n, t, nt, &lo, &hi);
            if (pix)
                st8_n(d8 + x, sh12_pix8(lo, hi), n);
            else
                st16_n(d16 + x, sh6_s16_8(lo, hi), n);
            x += n;
        }
    }
}

/* full pel: a copy, or (int16_t)(sample << 6) */
AI void blk_copy(void *dst, int ds, const uint8_t *p, int rs, int w, int h,
                 int pix)
{
    const __m128i z = _mm_setzero_si128();
    for (int y = 0; y < h; y++, p += rs) {
        uint8_t *d8 = (uint8_t *)dst + (ptrdiff_t)y * ds;
        int16_t *d16 = (int16_t *)dst + (ptrdiff_t)y * ds;
        int x = 0;
        for (; x + 16 <= w; x += 16) {
            __m128i v = _mm_loadu_si128((const __m128i *)(p + x));
            if (pix) {
                _mm_storeu_si128((__m128i *)(d8 + x), v);
            } else {
                st16(d16 + x, _mm_slli_epi16(_mm_unpacklo_epi8(v, z), 6));
                st16(d16 + x + 8, _mm_slli_epi16(_mm_unpackhi_epi8(v, z), 6));
            }
        }
        while (x < w) {
            int n = chunk_n(w - x);
            __m128i v = ld8_n(p + x, n);
            if (pix)
                st8_n(d8 + x, v, n);
            else
                st16_n(d16 + x, _mm_slli_epi16(_mm_cvtepu8_epi16(v), 6), n);
            x += n;
        }
    }
}

/* the whole interpolation for one block: nt taps, off = nt / 2 - 1
 * samples left and above. tmp holds (h + nt - 1) rows of w. */
AI void interp(void *dst, int ds, int w, int h, const pixel *ref, int rs,
               const int8_t *fh, const int8_t *fv, int nt, int pix,
               int16_t *tmp)
{
    const int off = nt / 2 - 1;
    taps t;
    if (!fh && !fv) {
        blk_copy(dst, ds, ref, rs, w, h, pix);
        return;
    }
    if (!fv) {
        taps_init(&t, fh, nt);
        blk_u8(dst, ds, ref - off, rs, 1, w, h, &t, nt, pix);
        return;
    }
    if (!fh) {
        taps_init(&t, fv, nt);
        blk_u8(dst, ds, ref - (ptrdiff_t)off * rs, rs, rs, w, h, &t, nt,
               pix);
        return;
    }
    taps_init(&t, fh, nt);
    blk_u8(tmp, w, ref - (ptrdiff_t)off * rs - off, rs, 1, w, h + nt - 1,
           &t, nt, 0);
    taps_init(&t, fv, nt);
    blk_s16(dst, ds, tmp, w, w, h, &t, nt, pix);
}

/* ---- the entry points ----------------------------------------------- */

void n265_mc_luma_wh_sse42(pixel *dst, int ds, int w, int h, const pixel *ref,
                           int ref_stride, int fx, int fy)
{
    int16_t tmp[(64 + 7) * 64];
    interp(dst, ds, w, h, ref, ref_stride, fx ? luma_f[fx] : NULL,
           fy ? luma_f[fy] : NULL, 8, 1, tmp);
}

void n265_mc_luma_14_wh_sse42(int16_t *dst, int ds, int w, int h,
                              const pixel *ref, int ref_stride, int fx,
                              int fy)
{
    int16_t tmp[(64 + 7) * 64];
    interp(dst, ds, w, h, ref, ref_stride, fx ? luma_f[fx] : NULL,
           fy ? luma_f[fy] : NULL, 8, 0, tmp);
}

void n265_mc_chroma_wh_sse42(pixel *dst, int ds, int w, int h,
                             const pixel *ref, int ref_stride, int fx, int fy)
{
    int16_t tmp[(64 + 3) * 64];
    interp(dst, ds, w, h, ref, ref_stride, fx ? chroma_f[fx] : NULL,
           fy ? chroma_f[fy] : NULL, 4, 1, tmp);
}

void n265_mc_chroma_14_wh_sse42(int16_t *dst, int ds, int w, int h,
                                const pixel *ref, int ref_stride, int fx,
                                int fy)
{
    int16_t tmp[(64 + 3) * 64];
    interp(dst, ds, w, h, ref, ref_stride, fx ? chroma_f[fx] : NULL,
           fy ? chroma_f[fy] : NULL, 4, 0, tmp);
}

/* fx = 0 included: the C runs the identity row of the table */
void n265_mc_luma_hplane_sse42(int16_t *dst, int ds, int w, int h,
                               const pixel *ref, int ref_stride, int fx)
{
    taps t;
    taps_init(&t, luma_f[fx], 8);
    blk_u8(dst, ds, ref - 3, ref_stride, 1, w, h, &t, 8, 0);
}

void n265_mc_luma_hf_sse42(pixel *dst, int ds, int w, int h,
                           const int16_t *hf, int hs, int fy)
{
    if (fy) {
        taps t;
        taps_init(&t, luma_f[fy], 8);
        blk_s16(dst, ds, hf - (ptrdiff_t)3 * hs, hs, w, h, &t, 8, 1);
        return;
    }
    for (int y = 0; y < h; y++, dst += ds, hf += hs) {
        int x = 0;
        for (; x + 16 <= w; x += 16)
            _mm_storeu_si128((__m128i *)(dst + x),
                             rnd6_pix16(ld16(hf + x), ld16(hf + x + 8)));
        while (x < w) {
            int n = chunk_n(w - x);
            st8_n(dst + x, rnd6_pix8(ld16_n(hf + x, n)), n);
            x += n;
        }
    }
}

void n265_mc_luma_14_hf_sse42(int16_t *dst, int ds, int w, int h,
                              const int16_t *hf, int hs, int fy)
{
    if (fy) {
        taps t;
        taps_init(&t, luma_f[fy], 8);
        blk_s16(dst, ds, hf - (ptrdiff_t)3 * hs, hs, w, h, &t, 8, 0);
        return;
    }
    for (int y = 0; y < h; y++, dst += ds, hf += hs) {
        int x = 0;
        for (; x + 8 <= w; x += 8)
            st16(dst + x, ld16(hf + x));
        while (x < w) {
            int n = chunk_n(w - x);
            st16_n(dst + x, ld16_n(hf + x, n), n);
            x += n;
        }
    }
}

/* ---- bi average and weighted prediction ----------------------------- */

/* clip1((a + b + 64) >> 7) as rnd6 of floor((a + b) / 2), which fits
 * int16 (mc_avx2.c) */
AI __m128i half_sum8(__m128i a, __m128i b)
{
    __m128i odd = _mm_and_si128(_mm_and_si128(a, b), _mm_set1_epi16(1));
    return _mm_add_epi16(_mm_add_epi16(_mm_srai_epi16(a, 1),
                                       _mm_srai_epi16(b, 1)),
                         odd);
}

void n265_bi_avg_wh_sse42(pixel *dst, int ds, const int16_t *a,
                          const int16_t *b, int w, int h)
{
    for (int y = 0; y < h; y++, dst += ds, a += w, b += w) {
        int x = 0;
        for (; x + 16 <= w; x += 16)
            _mm_storeu_si128((__m128i *)(dst + x),
                             rnd6_pix16(half_sum8(ld16(a + x), ld16(b + x)),
                                        half_sum8(ld16(a + x + 8),
                                                  ld16(b + x + 8))));
        while (x < w) {
            int n = chunk_n(w - x);
            st8_n(dst + x,
                  rnd6_pix8(half_sum8(ld16_n(a + x, n), ld16_n(b + x, n))), n);
            x += n;
        }
    }
}

/* madd on (sample, second) pairs against (weight, second weight): the
 * C's int expression in int32 lanes (mc_avx2.c has the ranges) */
AI __m128i wp_s16(__m128i lo, __m128i hi, __m128i sh, __m128i o)
{
    lo = _mm_add_epi32(_mm_sra_epi32(lo, sh), o);
    hi = _mm_add_epi32(_mm_sra_epi32(hi, sh), o);
    return _mm_packs_epi32(lo, hi);
}

AI __m128i pair32(int lo16, int hi16)
{
    return _mm_set1_epi32((int)((uint32_t)(uint16_t)lo16 |
                                ((uint32_t)(uint16_t)hi16 << 16)));
}

/* clip1(((src * wt + rnd) >> shift) + o) of 8 int16 samples */
AI __m128i wp_one8(__m128i v, __m128i c, __m128i sh, __m128i o)
{
    const __m128i one = _mm_set1_epi16(1);
    __m128i lo = _mm_madd_epi16(_mm_unpacklo_epi16(v, one), c);
    __m128i hi = _mm_madd_epi16(_mm_unpackhi_epi16(v, one), c);
    return wp_s16(lo, hi, sh, o);
}

/* clip1(((src * wt + rnd) >> shift) + o) over int16 or u8 sources */
AI void wp_one(pixel *dst, int ds, const void *src, int ss, int u8src,
               int w, int h, int wt, int shift, int o)
{
    const __m128i c = pair32(wt, shift >= 1 ? 1 << (shift - 1) : 0);
    const __m128i o32 = _mm_set1_epi32(o);
    const __m128i sh = _mm_cvtsi32_si128(shift);
    const __m128i z = _mm_setzero_si128();
    for (int y = 0; y < h; y++, dst += ds) {
        const int16_t *s16 = (const int16_t *)src + (ptrdiff_t)y * ss;
        const uint8_t *s8 = (const uint8_t *)src + (ptrdiff_t)y * ss;
        int x = 0;
        for (; x + 16 <= w; x += 16) {
            __m128i v0, v1;
            if (u8src) {
                __m128i v = _mm_loadu_si128((const __m128i *)(s8 + x));
                v0 = _mm_unpacklo_epi8(v, z);
                v1 = _mm_unpackhi_epi8(v, z);
            } else {
                v0 = ld16(s16 + x);
                v1 = ld16(s16 + x + 8);
            }
            _mm_storeu_si128((__m128i *)(dst + x),
                             _mm_packus_epi16(wp_one8(v0, c, sh, o32),
                                              wp_one8(v1, c, sh, o32)));
        }
        while (x < w) {
            int n = chunk_n(w - x);
            __m128i v = u8src ? _mm_cvtepu8_epi16(ld8_n(s8 + x, n))
                              : ld16_n(s16 + x, n);
            v = wp_one8(v, c, sh, o32);
            st8_n(dst + x, _mm_packus_epi16(v, v), n);
            x += n;
        }
    }
}

/* log2Wd = denom + 6 at 8 bits, and the offset is in 8-bit levels; with
 * shift 0 the rounding term is 0, which is the C's log2Wd < 1 arm */
void n265_wp_uni_wh_sse42(pixel *dst, int ds, const int16_t *src, int w,
                          int h, int wt, int off, int denom)
{
    wp_one(dst, ds, src, w, 0, w, h, wt, denom + 6, off);
}

void n265_wp_pix_wh_sse42(pixel *dst, int ds, const pixel *src, int ss,
                          int w, int h, int wt, int off, int denom)
{
    wp_one(dst, ds, src, ss, 1, w, h, wt, denom, off);
}

/* clip1((a * w0 + b * w1 + add) >> (log2Wd + 1)) of 8 samples */
AI __m128i wp_bi8(__m128i va, __m128i vb, __m128i c, __m128i add, __m128i sh)
{
    __m128i lo = _mm_add_epi32(
        _mm_madd_epi16(_mm_unpacklo_epi16(va, vb), c), add);
    __m128i hi = _mm_add_epi32(
        _mm_madd_epi16(_mm_unpackhi_epi16(va, vb), c), add);
    return wp_s16(lo, hi, sh, _mm_setzero_si128());
}

void n265_wp_bi_wh_sse42(pixel *dst, int ds, const int16_t *a,
                         const int16_t *b, int w, int h, int w0, int o0,
                         int w1, int o1, int denom)
{
    int log2wd = denom + 6;
    const __m128i c = pair32(w0, w1);
    const __m128i add = _mm_set1_epi32((o0 + o1 + 1) * (1 << log2wd));
    const __m128i sh = _mm_cvtsi32_si128(log2wd + 1);
    for (int y = 0; y < h; y++, dst += ds, a += w, b += w) {
        int x = 0;
        for (; x + 16 <= w; x += 16)
            _mm_storeu_si128((__m128i *)(dst + x), _mm_packus_epi16(
                wp_bi8(ld16(a + x), ld16(b + x), c, add, sh),
                wp_bi8(ld16(a + x + 8), ld16(b + x + 8), c, add, sh)));
        while (x < w) {
            int n = chunk_n(w - x);
            __m128i v = wp_bi8(ld16_n(a + x, n), ld16_n(b + x, n), c, add, sh);
            st8_n(dst + x, _mm_packus_epi16(v, v), n);
            x += n;
        }
    }
}
