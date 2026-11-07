/*
 * intra_avx2.c - intra prediction and reference smoothing (N265_ASM_INTRA)
 * on AVX2. The entry points are src/encoder/arm/neon.h's intra block,
 * renamed, and every one is bit-exact with src/encoder/intra.c.
 *
 * Angular rows interleave r[x + idx] with r[x + idx + 1] and take
 * _mm_maddubs_epi16 against the byte pair (32 - fact, fact). The weights
 * sum to 32, so a lane is at most 32 * 255 = 8160 and never saturates.
 * Modes under 18 predict into a local block along the left reference and
 * transpose it out, as the NEON twin does.
 *
 * Planar's four weights sum to 2n, so a lane is at most
 * n * (2 * 255 + 1) = 16352 at n = 32, inside int16.
 *
 * Reads of `refs` and `in` stay inside their 4n + 1 samples: checkasm's
 * page guard holds every kernel here to that.
 *
 * Copyright (c) 2026, the yah265 authors. GPL-2.0-or-later; see LICENSE.
 */

#include <immintrin.h>
#include <string.h>

#include "encoder/intra.h"
#include "encoder/x86/avx2.h"

static const int8_t ia_angle[35] = {
    0, 0, 32, 26, 21, 17, 13, 9, 5, 2, 0, -2, -5, -9, -13, -17, -21, -26,
    -32, -26, -21, -17, -13, -9, -5, -2, 0, 2, 5, 9, 13, 17, 21, 26, 32
};
static const int16_t ia_inv_angle[35] = {
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, -4096, -1638, -910, -630, -482, -390,
    -315, -256, -315, -390, -482, -630, -910, -1638, -4096, 0, 0, 0, 0, 0,
    0, 0, 0, 0
};

static inline uint32_t ld32u(const uint8_t *p)
{
    uint32_t v;
    memcpy(&v, p, 4);
    return v;
}

static inline void st32u(uint8_t *p, uint32_t v)
{
    memcpy(p, &v, 4);
}

static inline void st4x4(uint8_t *d, int ds, __m128i v)
{
    st32u(d, (uint32_t)_mm_cvtsi128_si32(v));
    st32u(d + ds, (uint32_t)_mm_extract_epi32(v, 1));
    st32u(d + 2 * ds, (uint32_t)_mm_extract_epi32(v, 2));
    st32u(d + 3 * ds, (uint32_t)_mm_extract_epi32(v, 3));
}

/* (x + 16) >> 5 per int16 lane */
static inline __m128i rnd5(__m128i x)
{
    return _mm_srli_epi16(_mm_add_epi16(x, _mm_set1_epi16(16)), 5);
}

static inline __m256i rnd5_256(__m256i x)
{
    return _mm256_srli_epi16(_mm256_add_epi16(x, _mm256_set1_epi16(16)), 5);
}

/* ---- angular (8.4.4.2.6) ------------------------------------------- */

/* the 4x4 block as 16 bytes, row-major */
static inline __m128i ang4(const uint8_t *r, int angle)
{
    uint32_t a4[4], b4[4];
    int8_t w[32];
    __m128i a, b, lo, hi;

    for (int y = 0; y < 4; y++) {
        int idx = ((y + 1) * angle) >> 5;
        int f = ((y + 1) * angle) & 31;
        a4[y] = ld32u(r + idx);
        b4[y] = ld32u(r + idx + 1);
        for (int k = 0; k < 4; k++) {
            w[8 * y + 2 * k] = (int8_t)(32 - f);
            w[8 * y + 2 * k + 1] = (int8_t)f;
        }
    }
    a = _mm_loadu_si128((const __m128i *)a4);
    b = _mm_loadu_si128((const __m128i *)b4);
    lo = _mm_maddubs_epi16(_mm_unpacklo_epi8(a, b),
                           _mm_loadu_si128((const __m128i *)w));
    hi = _mm_maddubs_epi16(_mm_unpackhi_epi8(a, b),
                           _mm_loadu_si128((const __m128i *)(w + 16)));
    return _mm_packus_epi16(rnd5(lo), rnd5(hi));
}

static void ang_rows(uint8_t *out, int os, const uint8_t *r, int n,
                     int angle)
{
    for (int y = 0; y < n; y++) {
        int idx = ((y + 1) * angle) >> 5;
        int f = ((y + 1) * angle) & 31;
        const uint8_t *p = r + idx;
        uint8_t *o = out + (size_t)y * os;

        if (f == 0) {
            if (n == 8)
                _mm_storel_epi64((__m128i *)o,
                                 _mm_loadl_epi64((const __m128i *)p));
            else if (n == 16)
                _mm_storeu_si128((__m128i *)o,
                                 _mm_loadu_si128((const __m128i *)p));
            else
                _mm256_storeu_si256((__m256i *)o,
                                    _mm256_loadu_si256((const __m256i *)p));
            continue;
        }
        if (n == 32) {
            __m256i w = _mm256_set1_epi16((int16_t)((f << 8) | (32 - f)));
            __m256i a = _mm256_loadu_si256((const __m256i *)p);
            __m256i b = _mm256_loadu_si256((const __m256i *)(p + 1));
            __m256i lo = _mm256_maddubs_epi16(_mm256_unpacklo_epi8(a, b), w);
            __m256i hi = _mm256_maddubs_epi16(_mm256_unpackhi_epi8(a, b), w);
            _mm256_storeu_si256((__m256i *)o,
                                _mm256_packus_epi16(rnd5_256(lo),
                                                    rnd5_256(hi)));
        } else {
            __m128i w = _mm_set1_epi16((int16_t)((f << 8) | (32 - f)));
            if (n == 16) {
                __m128i a = _mm_loadu_si128((const __m128i *)p);
                __m128i b = _mm_loadu_si128((const __m128i *)(p + 1));
                __m128i lo = _mm_maddubs_epi16(_mm_unpacklo_epi8(a, b), w);
                __m128i hi = _mm_maddubs_epi16(_mm_unpackhi_epi8(a, b), w);
                _mm_storeu_si128((__m128i *)o,
                                 _mm_packus_epi16(rnd5(lo), rnd5(hi)));
            } else {
                __m128i a = _mm_loadl_epi64((const __m128i *)p);
                __m128i b = _mm_loadl_epi64((const __m128i *)(p + 1));
                __m128i lo = _mm_maddubs_epi16(_mm_unpacklo_epi8(a, b), w);
                _mm_storel_epi64((__m128i *)o,
                                 _mm_packus_epi16(rnd5(lo), lo));
            }
        }
    }
}

static inline void trans8x8(uint8_t *d, int ds, const uint8_t *s, int ss)
{
    __m128i r[8], a[4], b[4], c[4];

    for (int i = 0; i < 8; i++)
        r[i] = _mm_loadl_epi64((const __m128i *)(s + (size_t)i * ss));
    for (int i = 0; i < 4; i++)
        a[i] = _mm_unpacklo_epi8(r[2 * i], r[2 * i + 1]);
    b[0] = _mm_unpacklo_epi16(a[0], a[1]);
    b[1] = _mm_unpackhi_epi16(a[0], a[1]);
    b[2] = _mm_unpacklo_epi16(a[2], a[3]);
    b[3] = _mm_unpackhi_epi16(a[2], a[3]);
    c[0] = _mm_unpacklo_epi32(b[0], b[2]);
    c[1] = _mm_unpackhi_epi32(b[0], b[2]);
    c[2] = _mm_unpacklo_epi32(b[1], b[3]);
    c[3] = _mm_unpackhi_epi32(b[1], b[3]);
    for (int i = 0; i < 4; i++) {
        _mm_storel_epi64((__m128i *)(d + (size_t)(2 * i) * ds), c[i]);
        _mm_storel_epi64((__m128i *)(d + (size_t)(2 * i + 1) * ds),
                         _mm_unpackhi_epi64(c[i], c[i]));
    }
}

/* r[-n-1 .. 2n-1] plus 32 zeroed samples above, which the row loads
 * reach and weight by zero */
static void build_ref(uint8_t *r, const uint8_t *main, const uint8_t *side,
                      int corner, int n, int angle, int inv)
{
    r[-1] = (uint8_t)corner;
    switch (n) {
    case 4:
        _mm_storel_epi64((__m128i *)r, _mm_loadl_epi64((const __m128i *)main));
        break;
    case 8:
        _mm_storeu_si128((__m128i *)r, _mm_loadu_si128((const __m128i *)main));
        break;
    case 16:
        _mm256_storeu_si256((__m256i *)r,
                            _mm256_loadu_si256((const __m256i *)main));
        break;
    default:
        _mm256_storeu_si256((__m256i *)r,
                            _mm256_loadu_si256((const __m256i *)main));
        _mm256_storeu_si256((__m256i *)(r + 32),
                            _mm256_loadu_si256((const __m256i *)(main + 32)));
        break;
    }
    _mm256_storeu_si256((__m256i *)(r + 2 * n), _mm256_setzero_si256());
    if (angle < 0) {
        int last = (n * angle) >> 5;
        for (int i = -2; i >= last; i--)
            r[i] = side[(((i + 1) * inv + 128) >> 8) - 1];
    }
}

static void pred_angular(uint8_t *dst, int stride, const uint8_t *refs,
                         int log2n, int mode, int c_idx)
{
    int n = 1 << log2n;
    const uint8_t *top = refs + 1;
    const uint8_t *left = refs + 2 * n + 1;
    int corner = refs[0];
    int angle = ia_angle[mode];
    int inv = ia_inv_angle[mode];
    uint8_t ref[4 * 32 + 2 + 32];
    uint8_t *r = ref + n + 1;

    if (mode >= 18) {
        build_ref(r, top, left, corner, n, angle, inv);
        if (n == 4)
            st4x4(dst, stride, ang4(r, angle));
        else
            ang_rows(dst, stride, r, n, angle);
        if (mode == 26 && c_idx == 0 && n < 32)
            for (int y = 0; y < n; y++) {
                int v = top[0] + ((left[y] - corner) >> 1);
                dst[(size_t)y * stride] =
                    (uint8_t)(v < 0 ? 0 : v > PIXEL_MAX ? PIXEL_MAX : v);
            }
    } else {
        build_ref(r, left, top, corner, n, angle, inv);
        if (n == 4) {
            const __m128i tr = _mm_setr_epi8(0, 4, 8, 12, 1, 5, 9, 13, 2, 6,
                                             10, 14, 3, 7, 11, 15);
            st4x4(dst, stride, _mm_shuffle_epi8(ang4(r, angle), tr));
        } else {
            uint8_t tmp[32 * 32];
            ang_rows(tmp, n, r, n, angle);
            for (int by = 0; by < n; by += 8)
                for (int bx = 0; bx < n; bx += 8)
                    trans8x8(dst + (size_t)bx * stride + by, stride,
                             tmp + (size_t)by * n + bx, n);
        }
        if (mode == 10 && c_idx == 0 && n < 32)
            for (int x = 0; x < n; x++) {
                int v = left[0] + ((top[x] - corner) >> 1);
                dst[x] = (uint8_t)(v < 0 ? 0 : v > PIXEL_MAX ? PIXEL_MAX : v);
            }
    }
}

/* ---- planar (8.4.4.2.4) -------------------------------------------- */

static void pred_planar(uint8_t *dst, int stride, const uint8_t *refs,
                        int log2n)
{
    int n = 1 << log2n;
    const uint8_t *top = refs + 1;
    const uint8_t *left = refs + 2 * n + 1;
    int tr = top[n];
    int bl = left[n];
    __m128i sh = _mm_cvtsi32_si128(log2n + 1);
    const __m256i ramp = _mm256_setr_epi16(0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10,
                                           11, 12, 13, 14, 15);
    /* columns 0-15 and 16-31: n-1-x, (x+1)*tr + n, top[x] */
    __m256i xw[2], base[2], t[2];
    int halves = n == 32 ? 2 : 1;

    for (int h = 0; h < halves; h++) {
        __m256i xi = _mm256_add_epi16(ramp, _mm256_set1_epi16((int16_t)(16 * h)));
        xw[h] = _mm256_sub_epi16(_mm256_set1_epi16((int16_t)(n - 1)), xi);
        base[h] = _mm256_add_epi16(
            _mm256_mullo_epi16(_mm256_add_epi16(xi, _mm256_set1_epi16(1)),
                               _mm256_set1_epi16((int16_t)tr)),
            _mm256_set1_epi16((int16_t)n));
        /* at n = 4 the eight bytes are top's 2n samples */
        __m128i tb = n >= 16
            ? _mm_loadu_si128((const __m128i *)(top + 16 * h))
            : _mm_loadl_epi64((const __m128i *)top);
        t[h] = _mm256_cvtepu8_epi16(tb);
    }

    for (int y = 0; y < n; y++) {
        __m256i ly = _mm256_set1_epi16(left[y]);
        __m256i ny = _mm256_set1_epi16((int16_t)(n - 1 - y));
        __m256i dy = _mm256_set1_epi16((int16_t)((y + 1) * bl));
        uint8_t *o = dst + (size_t)y * stride;

        for (int h = 0; h < halves; h++) {
            __m256i acc = _mm256_add_epi16(base[h],
                                           _mm256_mullo_epi16(xw[h], ly));
            acc = _mm256_add_epi16(acc, _mm256_mullo_epi16(t[h], ny));
            acc = _mm256_srl_epi16(_mm256_add_epi16(acc, dy), sh);
            __m128i p = _mm_packus_epi16(_mm256_castsi256_si128(acc),
                                         _mm256_extracti128_si256(acc, 1));
            if (n >= 16)
                _mm_storeu_si128((__m128i *)(o + 16 * h), p);
            else if (n == 8)
                _mm_storel_epi64((__m128i *)o, p);
            else
                st32u(o, (uint32_t)_mm_cvtsi128_si32(p));
        }
    }
}

/* ---- DC (8.4.4.2.5) ------------------------------------------------ */

static void pred_dc(uint8_t *dst, int stride, const uint8_t *refs,
                    int log2n, int filter_edges)
{
    int n = 1 << log2n;
    const uint8_t *top = refs + 1;
    const uint8_t *left = refs + 2 * n + 1;
    uint32_t sum;
    int dc;

    if (n == 32) {
        __m256i s = _mm256_add_epi64(
            _mm256_sad_epu8(_mm256_loadu_si256((const __m256i *)top),
                            _mm256_setzero_si256()),
            _mm256_sad_epu8(_mm256_loadu_si256((const __m256i *)left),
                            _mm256_setzero_si256()));
        __m128i q = _mm_add_epi64(_mm256_castsi256_si128(s),
                                  _mm256_extracti128_si256(s, 1));
        sum = (uint32_t)_mm_cvtsi128_si32(_mm_add_epi64(q,
                                          _mm_unpackhi_epi64(q, q)));
    } else {
        __m128i a, b;
        if (n == 16) {
            a = _mm_loadu_si128((const __m128i *)top);
            b = _mm_loadu_si128((const __m128i *)left);
        } else if (n == 8) {
            a = _mm_loadl_epi64((const __m128i *)top);
            b = _mm_loadl_epi64((const __m128i *)left);
        } else {
            a = _mm_cvtsi32_si128((int)ld32u(top));
            b = _mm_cvtsi32_si128((int)ld32u(left));
        }
        __m128i q = _mm_add_epi64(_mm_sad_epu8(a, _mm_setzero_si128()),
                                  _mm_sad_epu8(b, _mm_setzero_si128()));
        sum = (uint32_t)_mm_cvtsi128_si32(_mm_add_epi64(q,
                                          _mm_unpackhi_epi64(q, q)));
    }
    dc = (int)((sum + (uint32_t)n) >> (log2n + 1));

    {
        __m256i v = _mm256_set1_epi8((char)dc);
        for (int y = 0; y < n; y++) {
            uint8_t *d = dst + (size_t)y * stride;
            if (n == 32)
                _mm256_storeu_si256((__m256i *)d, v);
            else if (n == 16)
                _mm_storeu_si128((__m128i *)d, _mm256_castsi256_si128(v));
            else if (n == 8)
                _mm_storel_epi64((__m128i *)d, _mm256_castsi256_si128(v));
            else
                st32u(d, (uint32_t)_mm_cvtsi128_si32(_mm256_castsi256_si128(v)));
        }
    }

    if (filter_edges && n < 32) {
        dst[0] = (uint8_t)((left[0] + 2 * dc + top[0] + 2) >> 2);
        for (int x = 1; x < n; x++)
            dst[x] = (uint8_t)((top[x] + 3 * dc + 2) >> 2);
        for (int y = 1; y < n; y++)
            dst[(size_t)y * stride] = (uint8_t)((left[y] + 3 * dc + 2) >> 2);
    }
}

void n265_intra_pred_avx2(pixel *dst, int dst_stride, const pixel *refs,
                          int log2n, int mode, int c_idx)
{
    if (mode == INTRA_PLANAR)
        pred_planar(dst, dst_stride, refs, log2n);
    else if (mode == INTRA_DC)
        pred_dc(dst, dst_stride, refs, log2n, c_idx == 0);
    else
        pred_angular(dst, dst_stride, refs, log2n, mode, c_idx);
}

/* ---- [1 2 1] reference smoothing (8.4.4.2.3) ----------------------- */

static inline void filt16(uint8_t *out, const uint8_t *in, int i)
{
    __m256i a = _mm256_cvtepu8_epi16(_mm_loadu_si128((const __m128i *)(in + i - 1)));
    __m256i b = _mm256_cvtepu8_epi16(_mm_loadu_si128((const __m128i *)(in + i)));
    __m256i c = _mm256_cvtepu8_epi16(_mm_loadu_si128((const __m128i *)(in + i + 1)));
    __m256i t = _mm256_add_epi16(_mm256_add_epi16(a, c),
                                 _mm256_add_epi16(b, b));
    t = _mm256_srli_epi16(_mm256_add_epi16(t, _mm256_set1_epi16(2)), 2);
    _mm_storeu_si128((__m128i *)(out + i),
                     _mm_packus_epi16(_mm256_castsi256_si128(t),
                                      _mm256_extracti128_si256(t, 1)));
}

static inline void filt8(uint8_t *out, const uint8_t *in, int i)
{
    __m128i a = _mm_cvtepu8_epi16(_mm_loadl_epi64((const __m128i *)(in + i - 1)));
    __m128i b = _mm_cvtepu8_epi16(_mm_loadl_epi64((const __m128i *)(in + i)));
    __m128i c = _mm_cvtepu8_epi16(_mm_loadl_epi64((const __m128i *)(in + i + 1)));
    __m128i t = _mm_add_epi16(_mm_add_epi16(a, c), _mm_add_epi16(b, b));
    t = _mm_srli_epi16(_mm_add_epi16(t, _mm_set1_epi16(2)), 2);
    _mm_storel_epi64((__m128i *)(out + i), _mm_packus_epi16(t, t));
}

/* a run of `count` outputs from `first`; the last chunk is placed to end
 * on the run, so no load passes in[first + count] */
static void filt_run(uint8_t *out, const uint8_t *in, int first, int count)
{
    int end = first + count;
    int i = first;

    if (count >= 16) {
        for (; i + 16 <= end; i += 16)
            filt16(out, in, i);
        if (i < end)
            filt16(out, in, end - 16);
    } else if (count >= 8) {
        for (; i + 8 <= end; i += 8)
            filt8(out, in, i);
        if (i < end)
            filt8(out, in, end - 8);
    } else {
        for (; i < end; i++)
            out[i] = (uint8_t)((in[i - 1] + 2 * in[i] + in[i + 1] + 2) >> 2);
    }
}

void n265_filter_refs_avx2(pixel *out, const pixel *in, int log2n, int strong)
{
    int n = 1 << log2n;

    if (strong && n == 32) {
        int tl = in[0], tr = in[2 * n], bl = in[4 * n];
        int thr = 1 << (BIT_DEPTH - 5);
        int dt = tl + tr - 2 * in[n];
        int dl = tl + bl - 2 * in[2 * n + n];

        if ((dt < 0 ? -dt : dt) < thr && (dl < 0 ? -dl : dl) < thr) {
            out[0] = (pixel)tl;
            for (int i = 1; i <= 2 * n; i++)
                out[i] = (pixel)(((2 * n - i) * tl + i * tr + n) / (2 * n));
            for (int i = 1; i <= 2 * n; i++)
                out[2 * n + i] =
                    (pixel)(((2 * n - i) * tl + i * bl + n) / (2 * n));
            return;
        }
    }
    out[0] = (pixel)((in[2 * n + 1] + 2 * in[0] + in[1] + 2) >> 2);
    filt_run(out, in, 1, 2 * n - 1);
    out[2 * n] = in[2 * n];
    out[2 * n + 1] = (pixel)((in[0] + 2 * in[2 * n + 1] +
                              in[2 * n + 2] + 2) >> 2);
    filt_run(out, in, 2 * n + 2, 2 * n - 2);
    out[4 * n] = in[4 * n];
}
