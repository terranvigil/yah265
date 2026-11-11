/*
 * mc_avx2.c - the motion-compensation family (N265_ASM_MC) on AVX2.
 *
 * The entry points are src/encoder/arm/neon.h's mc block, renamed, and
 * each is bit-exact with its C reference in src/encoder/mc.c, at both
 * depths.
 *
 * Every row is cut into 16-sample chunks and then one chunk of 8, 4, 2
 * and 1 for what is left, so any w and h works (4:2:0 chroma hands the
 * chroma forms w = 2 and 6, 4:4:4 hands them 64) and every load reads
 * only samples the C reads. checkasm's page guard holds the kernels to
 * that.
 *
 * Filters over u8 (8 bits) run two taps at a time through maddubs on
 * interleaved sources, so each lane gets f[k] * s[k] + f[k+1] * s[k+1] in
 * int16. No pair saturates: the largest |f[k]| + |f[k+1]| in Table
 * 8-11/8-12 is 68, and 68 * 255 is under 32767. The full 8-tap sum lies
 * in [-6120, 22440] (filt[2]: positive taps 88, negative 24), so int16
 * holds it and the modular adds of the partial sums give the exact value.
 *
 * At Main10 that sum lies in [-24552, 90024], which int16 does not hold,
 * so a sample row is filtered as the int16 rows are: a sample is at most
 * 1023, a positive int16, and goes through madd like the cached plane.
 *
 * Filters over int16 (the cached hf plane, the 2-D temp, Main10 samples)
 * run two taps at a time through madd on interleaved rows, which is the
 * C's int multiply and add in int32 lanes. Even adversarial int16 input
 * reaches only 88 * 32767 + 24 * 32768 in magnitude, inside int32.
 *
 * The narrows reproduce the C's casts and clips exactly, with
 * shift3 = 14 - BitDepth (6, or 4 at Main10):
 *   - clip1((v + (1 << (shift3 - 1))) >> shift3) on int16 v is
 *     mulhrs(v, 1 << (15 - shift3)), which rounds in 32 bits, so
 *     v = 32767 does not wrap. A one-stage filter's (s + 32) >> 6 is this
 *     narrow at 8 bits and a 32-bit one at Main10.
 *   - (int16_t)(s >> k) on int32 s keeps the low 16 bits, as the C's
 *     conversion does; (s << (16 - k)) >> 16 sign-extends exactly those
 *     bits, so the saturating pack that follows has nothing to saturate.
 *   - clip1 of an int32 is a saturating pack to int16 and then packus,
 *     or min/max against PIXEL_MAX at Main10: the C's two clips.
 *
 * Copyright (c) 2026, the yah265 authors. GPL-2.0-or-later; see LICENSE.
 */

#include <immintrin.h>
#include <stddef.h>
#include <string.h>

#include "encoder/mc.h"
#include "encoder/x86/avx2.h"

#define AI static inline __attribute__((always_inline))

#define SHIFT1  (BIT_DEPTH - 8)
#define SHIFT3  (14 - BIT_DEPTH)
#define SHIFT23 (6 + SHIFT3)

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
typedef struct { __m256i b[4], w[4]; } taps;

AI void taps_init(taps *t, const int8_t *f, int nt)
{
    for (int j = 0; j < nt / 2; j++) {
        uint16_t bb = (uint16_t)((uint8_t)f[2 * j] |
                                 ((unsigned)(uint8_t)f[2 * j + 1] << 8));
        uint32_t ww = (uint32_t)(uint16_t)f[2 * j] |
                      ((uint32_t)(uint16_t)f[2 * j + 1] << 16);
        t->b[j] = _mm256_set1_epi16((short)bb);
        t->w[j] = _mm256_set1_epi32((int)ww);
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

AI int chunk_n(int left)
{
    return left >= 8 ? 8 : left >= 4 ? 4 : left >= 2 ? 2 : 1;
}

/* ---- samples in and out ---------------------------------------------
 *
 * The only helpers that know a sample's width. In: samples as int16
 * lanes. Out: clip1 of int16 lanes, stored as samples. */

#if BIT_DEPTH == 8
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

AI __m256i ld_pix16(const pixel *p)
{
    return _mm256_cvtepu8_epi16(_mm_loadu_si128((const __m128i *)p));
}

AI __m128i ld_pix_n(const pixel *p, int n)
{
    return _mm_cvtepu8_epi16(ld8_n(p, n));
}

AI void st_pix16(pixel *d, __m256i v)
{
    _mm_storeu_si128((__m128i *)d,
                     _mm_packus_epi16(_mm256_castsi256_si128(v),
                                      _mm256_extracti128_si256(v, 1)));
}

AI void st_pix_n(pixel *d, __m128i v, int n)
{
    st8_n(d, _mm_packus_epi16(v, v), n);
}

AI void cp_pix16(pixel *d, const pixel *s)
{
    _mm_storeu_si128((__m128i *)d, _mm_loadu_si128((const __m128i *)s));
}

AI void cp_pix_n(pixel *d, const pixel *s, int n)
{
    st8_n(d, ld8_n(s, n), n);
}
#else
AI __m256i ld_pix16(const pixel *p)
{
    return _mm256_loadu_si256((const __m256i *)p);
}

AI __m128i ld_pix_n(const pixel *p, int n)
{
    return ld16_n((const int16_t *)p, n);
}

AI void st_pix16(pixel *d, __m256i v)
{
    v = _mm256_min_epi16(_mm256_max_epi16(v, _mm256_setzero_si256()),
                         _mm256_set1_epi16(PIXEL_MAX));
    _mm256_storeu_si256((__m256i *)d, v);
}

AI void st_pix_n(pixel *d, __m128i v, int n)
{
    v = _mm_min_epi16(_mm_max_epi16(v, _mm_setzero_si128()),
                      _mm_set1_epi16(PIXEL_MAX));
    st16_n((int16_t *)d, v, n);
}

AI void cp_pix16(pixel *d, const pixel *s)
{
    _mm256_storeu_si256((__m256i *)d, _mm256_loadu_si256((const __m256i *)s));
}

AI void cp_pix_n(pixel *d, const pixel *s, int n)
{
    st16_n((int16_t *)d, ld16_n((const int16_t *)s, n), n);
}
#endif

/* ---- the filters ---------------------------------------------------- */

#if BIT_DEPTH == 8
/* nt-tap filter over u8 at p, p + st, ..., n <= 8 outputs, int16 */
AI __m128i fu8_8(const uint8_t *p, ptrdiff_t st, int n, const taps *t, int nt)
{
    __m128i acc = _mm_setzero_si128();
    for (int j = 0; j < nt / 2; j++) {
        __m128i a = ld8_n(p + 2 * j * st, n);
        __m128i b = ld8_n(p + (2 * j + 1) * st, n);
        acc = _mm_add_epi16(acc,
                            _mm_maddubs_epi16(_mm_unpacklo_epi8(a, b),
                                              _mm256_castsi256_si128(t->b[j])));
    }
    return acc;
}

/* the same for 16 outputs, in order */
AI __m256i fu8_16(const uint8_t *p, ptrdiff_t st, const taps *t, int nt)
{
    __m256i acc = _mm256_setzero_si256();
    for (int j = 0; j < nt / 2; j++) {
        __m128i a = _mm_loadu_si128((const __m128i *)(p + 2 * j * st));
        __m128i b = _mm_loadu_si128((const __m128i *)(p + (2 * j + 1) * st));
        __m256i v = _mm256_inserti128_si256(
            _mm256_castsi128_si256(_mm_unpacklo_epi8(a, b)),
            _mm_unpackhi_epi8(a, b), 1);
        acc = _mm256_add_epi16(acc, _mm256_maddubs_epi16(v, t->b[j]));
    }
    return acc;
}
#endif

/* nt-tap filter over int16 rows p, p + st, ..., n <= 8 outputs: int32
 * sums of outputs 0-3 in lo and 4-7 in hi */
AI void fs16_8(const int16_t *p, ptrdiff_t st, int n, const taps *t, int nt,
               __m128i *lo, __m128i *hi)
{
    __m128i l = _mm_setzero_si128(), h = _mm_setzero_si128();
    for (int j = 0; j < nt / 2; j++) {
        __m128i a = ld16_n(p + 2 * j * st, n);
        __m128i b = ld16_n(p + (2 * j + 1) * st, n);
        __m128i c = _mm256_castsi256_si128(t->w[j]);
        l = _mm_add_epi32(l, _mm_madd_epi16(_mm_unpacklo_epi16(a, b), c));
        h = _mm_add_epi32(h, _mm_madd_epi16(_mm_unpackhi_epi16(a, b), c));
    }
    *lo = l;
    *hi = h;
}

/* 16 outputs: lo holds outputs 0-3 | 8-11, hi 4-7 | 12-15, so a per-lane
 * pack of (lo, hi) comes out in order */
AI void fs16_16(const int16_t *p, ptrdiff_t st, const taps *t, int nt,
                __m256i *lo, __m256i *hi)
{
    __m256i l = _mm256_setzero_si256(), h = _mm256_setzero_si256();
    for (int j = 0; j < nt / 2; j++) {
        __m256i a = _mm256_loadu_si256((const __m256i *)(p + 2 * j * st));
        __m256i b = _mm256_loadu_si256((const __m256i *)(p + (2 * j + 1) * st));
        l = _mm256_add_epi32(l, _mm256_madd_epi16(_mm256_unpacklo_epi16(a, b),
                                                  t->w[j]));
        h = _mm256_add_epi32(h, _mm256_madd_epi16(_mm256_unpackhi_epi16(a, b),
                                                  t->w[j]));
    }
    *lo = l;
    *hi = h;
}

/* ---- the narrows ---------------------------------------------------- */

/* (v + (1 << (SHIFT3 - 1))) >> SHIFT3 of int16 lanes */
AI __m128i rnd3_8(__m128i v)
{
    return _mm_mulhrs_epi16(v, _mm_set1_epi16(1 << (15 - SHIFT3)));
}

AI __m256i rnd3_16(__m256i v)
{
    return _mm256_mulhrs_epi16(v, _mm256_set1_epi16(1 << (15 - SHIFT3)));
}

/* (int16_t)(s >> k) */
AI __m128i shk_s16_8(__m128i lo, __m128i hi, int k)
{
    lo = _mm_srai_epi32(_mm_slli_epi32(lo, 16 - k), 16);
    hi = _mm_srai_epi32(_mm_slli_epi32(hi, 16 - k), 16);
    return _mm_packs_epi32(lo, hi);
}

AI __m256i shk_s16_16(__m256i lo, __m256i hi, int k)
{
    lo = _mm256_srai_epi32(_mm256_slli_epi32(lo, 16 - k), 16);
    hi = _mm256_srai_epi32(_mm256_slli_epi32(hi, 16 - k), 16);
    return _mm256_packs_epi32(lo, hi);
}

/* (s + (1 << (k - 1))) >> k, saturated to int16 */
AI __m128i rndk_8(__m128i lo, __m128i hi, int k)
{
    const __m128i r = _mm_set1_epi32(1 << (k - 1));
    lo = _mm_srai_epi32(_mm_add_epi32(lo, r), k);
    hi = _mm_srai_epi32(_mm_add_epi32(hi, r), k);
    return _mm_packs_epi32(lo, hi);
}

AI __m256i rndk_16(__m256i lo, __m256i hi, int k)
{
    const __m256i r = _mm256_set1_epi32(1 << (k - 1));
    lo = _mm256_srai_epi32(_mm256_add_epi32(lo, r), k);
    hi = _mm256_srai_epi32(_mm256_add_epi32(hi, r), k);
    return _mm256_packs_epi32(lo, hi);
}

/* ---- the block drivers ---------------------------------------------- */

/* A one-stage filter over samples: taps along `st` (1 horizontal, rs
 * vertical) from p, which already points at tap 0 of output (0, 0).
 * pix: clip1((s + 32) >> 6) into samples; otherwise (int16_t)(s >> shift1). */
#if BIT_DEPTH == 8
AI void blk_px(void *dst, int ds, const pixel *p, int rs, ptrdiff_t st,
               int w, int h, const taps *t, int nt, int pix)
{
    for (int y = 0; y < h; y++, p += rs) {
        uint8_t *d8 = (uint8_t *)dst + (ptrdiff_t)y * ds;
        int16_t *d16 = (int16_t *)dst + (ptrdiff_t)y * ds;
        int x = 0;
        for (; x + 16 <= w; x += 16) {
            __m256i v = fu8_16(p + x, st, t, nt);
            if (pix)
                st_pix16(d8 + x, rnd3_16(v));
            else
                _mm256_storeu_si256((__m256i *)(d16 + x), v);
        }
        while (x < w) {
            int n = chunk_n(w - x);
            __m128i v = fu8_8(p + x, st, n, t, nt);
            if (pix)
                st_pix_n(d8 + x, rnd3_8(v), n);
            else
                st16_n(d16 + x, v, n);
            x += n;
        }
    }
}
#else
AI void blk_px(void *dst, int ds, const pixel *p, int rs, ptrdiff_t st,
               int w, int h, const taps *t, int nt, int pix)
{
    for (int y = 0; y < h; y++, p += rs) {
        const int16_t *q = (const int16_t *)p;
        pixel *dp = (pixel *)dst + (ptrdiff_t)y * ds;
        int16_t *d16 = (int16_t *)dst + (ptrdiff_t)y * ds;
        int x = 0;
        for (; x + 16 <= w; x += 16) {
            __m256i lo, hi;
            fs16_16(q + x, st, t, nt, &lo, &hi);
            if (pix)
                st_pix16(dp + x, rndk_16(lo, hi, 6));
            else
                _mm256_storeu_si256((__m256i *)(d16 + x),
                                    shk_s16_16(lo, hi, SHIFT1));
        }
        while (x < w) {
            int n = chunk_n(w - x);
            __m128i lo, hi;
            fs16_8(q + x, st, n, t, nt, &lo, &hi);
            if (pix)
                st_pix_n(dp + x, rndk_8(lo, hi, 6), n);
            else
                st16_n(d16 + x, shk_s16_8(lo, hi, SHIFT1), n);
            x += n;
        }
    }
}
#endif

/* the vertical filter over int16 rows; p points at tap 0 of output
 * (0, 0). pix: clip1((s + (1 << (shift23 - 1))) >> shift23); otherwise
 * (int16_t)(s >> 6). */
AI void blk_s16(void *dst, int ds, const int16_t *p, int hs, int w, int h,
                const taps *t, int nt, int pix)
{
    for (int y = 0; y < h; y++, p += hs) {
        pixel *dp = (pixel *)dst + (ptrdiff_t)y * ds;
        int16_t *d16 = (int16_t *)dst + (ptrdiff_t)y * ds;
        int x = 0;
        for (; x + 16 <= w; x += 16) {
            __m256i lo, hi;
            fs16_16(p + x, hs, t, nt, &lo, &hi);
            if (pix)
                st_pix16(dp + x, rndk_16(lo, hi, SHIFT23));
            else
                _mm256_storeu_si256((__m256i *)(d16 + x),
                                    shk_s16_16(lo, hi, 6));
        }
        while (x < w) {
            int n = chunk_n(w - x);
            __m128i lo, hi;
            fs16_8(p + x, hs, n, t, nt, &lo, &hi);
            if (pix)
                st_pix_n(dp + x, rndk_8(lo, hi, SHIFT23), n);
            else
                st16_n(d16 + x, shk_s16_8(lo, hi, 6), n);
            x += n;
        }
    }
}

/* full pel: a copy, or (int16_t)(sample << shift3) */
AI void blk_copy(void *dst, int ds, const pixel *p, int rs, int w, int h,
                 int pix)
{
    for (int y = 0; y < h; y++, p += rs) {
        pixel *dp = (pixel *)dst + (ptrdiff_t)y * ds;
        int16_t *d16 = (int16_t *)dst + (ptrdiff_t)y * ds;
        int x = 0;
        for (; x + 16 <= w; x += 16) {
            if (pix)
                cp_pix16(dp + x, p + x);
            else
                _mm256_storeu_si256((__m256i *)(d16 + x),
                                    _mm256_slli_epi16(ld_pix16(p + x), SHIFT3));
        }
        while (x < w) {
            int n = chunk_n(w - x);
            if (pix)
                cp_pix_n(dp + x, p + x, n);
            else
                st16_n(d16 + x, _mm_slli_epi16(ld_pix_n(p + x, n), SHIFT3),
                       n);
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
        blk_px(dst, ds, ref - off, rs, 1, w, h, &t, nt, pix);
        return;
    }
    if (!fh) {
        taps_init(&t, fv, nt);
        blk_px(dst, ds, ref - (ptrdiff_t)off * rs, rs, rs, w, h, &t, nt,
               pix);
        return;
    }
    taps_init(&t, fh, nt);
    blk_px(tmp, w, ref - (ptrdiff_t)off * rs - off, rs, 1, w, h + nt - 1,
           &t, nt, 0);
    taps_init(&t, fv, nt);
    blk_s16(dst, ds, tmp, w, w, h, &t, nt, pix);
}

/* ---- the entry points ----------------------------------------------- */

void n265_mc_luma_wh_avx2(pixel *dst, int ds, int w, int h, const pixel *ref,
                          int ref_stride, int fx, int fy)
{
    int16_t tmp[(64 + 7) * 64];
    interp(dst, ds, w, h, ref, ref_stride, fx ? luma_f[fx] : NULL,
           fy ? luma_f[fy] : NULL, 8, 1, tmp);
}

void n265_mc_luma_14_wh_avx2(int16_t *dst, int ds, int w, int h,
                             const pixel *ref, int ref_stride, int fx, int fy)
{
    int16_t tmp[(64 + 7) * 64];
    interp(dst, ds, w, h, ref, ref_stride, fx ? luma_f[fx] : NULL,
           fy ? luma_f[fy] : NULL, 8, 0, tmp);
}

void n265_mc_chroma_wh_avx2(pixel *dst, int ds, int w, int h,
                            const pixel *ref, int ref_stride, int fx, int fy)
{
    int16_t tmp[(64 + 3) * 64];
    interp(dst, ds, w, h, ref, ref_stride, fx ? chroma_f[fx] : NULL,
           fy ? chroma_f[fy] : NULL, 4, 1, tmp);
}

void n265_mc_chroma_14_wh_avx2(int16_t *dst, int ds, int w, int h,
                               const pixel *ref, int ref_stride, int fx,
                               int fy)
{
    int16_t tmp[(64 + 3) * 64];
    interp(dst, ds, w, h, ref, ref_stride, fx ? chroma_f[fx] : NULL,
           fy ? chroma_f[fy] : NULL, 4, 0, tmp);
}

/* fx = 0 included: the C runs the identity row of the table */
void n265_mc_luma_hplane_avx2(int16_t *dst, int ds, int w, int h,
                              const pixel *ref, int ref_stride, int fx)
{
    taps t;
    taps_init(&t, luma_f[fx], 8);
    blk_px(dst, ds, ref - 3, ref_stride, 1, w, h, &t, 8, 0);
}

void n265_mc_luma_hf_avx2(pixel *dst, int ds, int w, int h,
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
            st_pix16(dst + x, rnd3_16(_mm256_loadu_si256(
                                  (const __m256i *)(hf + x))));
        while (x < w) {
            int n = chunk_n(w - x);
            st_pix_n(dst + x, rnd3_8(ld16_n(hf + x, n)), n);
            x += n;
        }
    }
}

void n265_mc_luma_14_hf_avx2(int16_t *dst, int ds, int w, int h,
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
        for (; x + 16 <= w; x += 16)
            _mm256_storeu_si256((__m256i *)(dst + x),
                                _mm256_loadu_si256((const __m256i *)(hf + x)));
        while (x < w) {
            int n = chunk_n(w - x);
            st16_n(dst + x, ld16_n(hf + x, n), n);
            x += n;
        }
    }
}

/* ---- bi average and weighted prediction ----------------------------- */

/* clip1((a + b + (1 << shift3)) >> (shift3 + 1)). h = floor((a + b) / 2)
 * fits int16, and floor((s + 2^k) / 2^(k+1)) ==
 * floor((floor(s / 2) + 2^(k-1)) / 2^k), the dropped half never carrying
 * across a multiple of 2^k; then rnd3 as above. */
AI __m256i half_sum16(__m256i a, __m256i b)
{
    __m256i odd = _mm256_and_si256(_mm256_and_si256(a, b),
                                   _mm256_set1_epi16(1));
    return _mm256_add_epi16(_mm256_add_epi16(_mm256_srai_epi16(a, 1),
                                             _mm256_srai_epi16(b, 1)),
                            odd);
}

AI __m128i half_sum8(__m128i a, __m128i b)
{
    __m128i odd = _mm_and_si128(_mm_and_si128(a, b), _mm_set1_epi16(1));
    return _mm_add_epi16(_mm_add_epi16(_mm_srai_epi16(a, 1),
                                       _mm_srai_epi16(b, 1)),
                         odd);
}

void n265_bi_avg_wh_avx2(pixel *dst, int ds, const int16_t *a,
                         const int16_t *b, int w, int h)
{
    for (int y = 0; y < h; y++, dst += ds, a += w, b += w) {
        int x = 0;
        for (; x + 16 <= w; x += 16) {
            __m256i va = _mm256_loadu_si256((const __m256i *)(a + x));
            __m256i vb = _mm256_loadu_si256((const __m256i *)(b + x));
            st_pix16(dst + x, rnd3_16(half_sum16(va, vb)));
        }
        while (x < w) {
            int n = chunk_n(w - x);
            st_pix_n(dst + x,
                     rnd3_8(half_sum8(ld16_n(a + x, n), ld16_n(b + x, n))), n);
            x += n;
        }
    }
}

/* The weighted forms are madd on (sample, second) pairs against
 * (weight, second weight), the C's int expression term for term in int32
 * lanes. Weights and rounding terms fit int16 (|wt| <= 255, the rounding
 * term at most 1 << 12), and no pair is two -32768 products, so madd
 * cannot wrap. The shift is a runtime count. The saturating pack and the
 * sample store are clip1's two clips. */
AI __m128i wp_out8(__m128i lo, __m128i hi, __m128i sh, __m128i o)
{
    lo = _mm_add_epi32(_mm_sra_epi32(lo, sh), o);
    hi = _mm_add_epi32(_mm_sra_epi32(hi, sh), o);
    return _mm_packs_epi32(lo, hi);
}

AI __m256i wp_out16(__m256i lo, __m256i hi, __m128i sh, __m256i o)
{
    lo = _mm256_add_epi32(_mm256_sra_epi32(lo, sh), o);
    hi = _mm256_add_epi32(_mm256_sra_epi32(hi, sh), o);
    return _mm256_packs_epi32(lo, hi);
}

AI __m256i pair32(int lo16, int hi16)
{
    return _mm256_set1_epi32((int)((uint32_t)(uint16_t)lo16 |
                                   ((uint32_t)(uint16_t)hi16 << 16)));
}

/* clip1(((src * wt + rnd) >> shift) + o) over int16 or sample sources */
AI void wp_one(pixel *dst, int ds, const void *src, int ss, int pxsrc,
               int w, int h, int wt, int shift, int o)
{
    const __m256i c = pair32(wt, shift >= 1 ? 1 << (shift - 1) : 0);
    const __m256i one = _mm256_set1_epi16(1);
    const __m256i o32 = _mm256_set1_epi32(o);
    const __m128i sh = _mm_cvtsi32_si128(shift);
    for (int y = 0; y < h; y++, dst += ds) {
        const int16_t *s16 = (const int16_t *)src + (ptrdiff_t)y * ss;
        const pixel *sp = (const pixel *)src + (ptrdiff_t)y * ss;
        int x = 0;
        for (; x + 16 <= w; x += 16) {
            __m256i v = pxsrc
                ? ld_pix16(sp + x)
                : _mm256_loadu_si256((const __m256i *)(s16 + x));
            __m256i lo = _mm256_madd_epi16(_mm256_unpacklo_epi16(v, one), c);
            __m256i hi = _mm256_madd_epi16(_mm256_unpackhi_epi16(v, one), c);
            st_pix16(dst + x, wp_out16(lo, hi, sh, o32));
        }
        while (x < w) {
            int n = chunk_n(w - x);
            __m128i v = pxsrc ? ld_pix_n(sp + x, n) : ld16_n(s16 + x, n);
            __m128i c1 = _mm256_castsi256_si128(c);
            __m128i one1 = _mm256_castsi256_si128(one);
            __m128i lo = _mm_madd_epi16(_mm_unpacklo_epi16(v, one1), c1);
            __m128i hi = _mm_madd_epi16(_mm_unpackhi_epi16(v, one1), c1);
            st_pix_n(dst + x,
                     wp_out8(lo, hi, sh, _mm256_castsi256_si128(o32)), n);
            x += n;
        }
    }
}

/* log2Wd = denom + shift3, and the offset is in 8-bit levels; with
 * shift 0 the rounding term is 0, which is the C's log2Wd < 1 arm */
void n265_wp_uni_wh_avx2(pixel *dst, int ds, const int16_t *src, int w,
                         int h, int wt, int off, int denom)
{
    wp_one(dst, ds, src, w, 0, w, h, wt, denom + SHIFT3,
           off * (1 << BIT_DEPTH_SHIFT));
}

void n265_wp_pix_wh_avx2(pixel *dst, int ds, const pixel *src, int ss,
                         int w, int h, int wt, int off, int denom)
{
    wp_one(dst, ds, src, ss, 1, w, h, wt, denom,
           off * (1 << BIT_DEPTH_SHIFT));
}

void n265_wp_bi_wh_avx2(pixel *dst, int ds, const int16_t *a,
                        const int16_t *b, int w, int h, int w0, int o0,
                        int w1, int o1, int denom)
{
    int log2wd = denom + SHIFT3;
    const __m256i c = pair32(w0, w1);
    const __m256i add = _mm256_set1_epi32(
        ((o0 + o1) * (1 << BIT_DEPTH_SHIFT) + 1) * (1 << log2wd));
    const __m256i zero = _mm256_setzero_si256();
    const __m128i sh = _mm_cvtsi32_si128(log2wd + 1);
    for (int y = 0; y < h; y++, dst += ds, a += w, b += w) {
        int x = 0;
        for (; x + 16 <= w; x += 16) {
            __m256i va = _mm256_loadu_si256((const __m256i *)(a + x));
            __m256i vb = _mm256_loadu_si256((const __m256i *)(b + x));
            __m256i lo = _mm256_add_epi32(
                _mm256_madd_epi16(_mm256_unpacklo_epi16(va, vb), c), add);
            __m256i hi = _mm256_add_epi32(
                _mm256_madd_epi16(_mm256_unpackhi_epi16(va, vb), c), add);
            st_pix16(dst + x, wp_out16(lo, hi, sh, zero));
        }
        while (x < w) {
            int n = chunk_n(w - x);
            __m128i va = ld16_n(a + x, n), vb = ld16_n(b + x, n);
            __m128i c1 = _mm256_castsi256_si128(c);
            __m128i a1 = _mm256_castsi256_si128(add);
            __m128i lo = _mm_add_epi32(
                _mm_madd_epi16(_mm_unpacklo_epi16(va, vb), c1), a1);
            __m128i hi = _mm_add_epi32(
                _mm_madd_epi16(_mm_unpackhi_epi16(va, vb), c1), a1);
            st_pix_n(dst + x,
                     wp_out8(lo, hi, sh, _mm256_castsi256_si128(zero)), n);
            x += n;
        }
    }
}
