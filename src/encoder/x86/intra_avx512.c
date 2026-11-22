/*
 * intra_avx512.c - intra_pred (N265_ASM_INTRA) on AVX-512 BW at 16x16 and
 * 32x32, both depths: angular and planar. DC, the smaller sizes and
 * filter_refs go to the AVX2 kernel.
 *
 * One vector holds two 32-sample rows or four 16-sample rows; each row's
 * samples are loaded into its own lane, so a row keeps its own index and
 * fraction. At 8 bits the angular rows take no shuffle: a word of r + idx
 * is the byte pair (r[2i], r[2i + 1]) and a word of r + idx + 1 is
 * (r[2i + 1], r[2i + 2]), so pmaddubsw against (32 - f, f) gives the even
 * and the odd outputs in place, and the odd ones are moved to the high
 * byte. A lane is at most 32 * 255 + 16 = 8176, so (x << 3) & 0xff00 is
 * (x >> 5) << 8. At Main10 a lane is a * 32 + (b - a) * f, which is
 * a * (32 - f) + b * f, at most 32736.
 *
 * Planar carries (n - 1 - y) * top[x] + (y + 1) * bl from row to row,
 * which is at most 32 * 1023. The lane bounds are intra_avx2.c's.
 *
 * Modes under 18 predict along the left reference into a local block and
 * transpose it out by 8x8 tiles, as intra_avx2.c does.
 *
 * Copyright (c) 2026, the yah265 authors. GPL-2.0-or-later; see LICENSE.
 */

#include <immintrin.h>
#include <string.h>

#include "encoder/intra.h"
#include "encoder/x86/avx2.h"
#include "encoder/x86/avx512.h"

#define AI static inline __attribute__((always_inline))

static const int8_t ia_angle[35] = {
    0, 0, 32, 26, 21, 17, 13, 9, 5, 2, 0, -2, -5, -9, -13, -17, -21, -26,
    -32, -26, -21, -17, -13, -9, -5, -2, 0, 2, 5, 9, 13, 17, 21, 26, 32
};
static const int16_t ia_inv_angle[35] = {
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, -4096, -1638, -910, -630, -482, -390,
    -315, -256, -315, -390, -482, -630, -910, -1638, -4096, 0, 0, 0, 0, 0,
    0, 0, 0, 0
};

/* r[-n-1 .. 2n-1], then 32 zeroed samples the row loads reach with f = 0 */
static void build_ref(pixel *r, const pixel *main, const pixel *side,
                      int corner, int n, int angle, int inv)
{
    r[-1] = (pixel)corner;
    memcpy(r, main, (size_t)(2 * n) * sizeof(pixel));
    memset(r + 2 * n, 0, 32 * sizeof(pixel));
    if (angle < 0) {
        int last = (n * angle) >> 5;
        for (int i = -2; i >= last; i--)
            r[i] = side[(((i + 1) * inv + 128) >> 8) - 1];
    }
}

#if BIT_DEPTH == 8

/* rows y .. y + 32/n... of the block: two rows at n = 32, four at 16 */
static void ang_rows(uint8_t *out, int os, const uint8_t *r, int n,
                     int angle)
{
    const __m512i r16 = _mm512_set1_epi16(16);
    const __m512i hi8 = _mm512_set1_epi16((int16_t)0xff00);
    const int per = 64 / n;

    for (int y = 0; y < n; y += per) {
        __m512i a, b, w;
        if (n == 32) {
            int i0 = ((y + 1) * angle) >> 5, f0 = ((y + 1) * angle) & 31;
            int i1 = ((y + 2) * angle) >> 5, f1 = ((y + 2) * angle) & 31;
            a = _mm512_inserti64x4(_mm512_castsi256_si512(
                    _mm256_loadu_si256((const __m256i *)(r + i0))),
                _mm256_loadu_si256((const __m256i *)(r + i1)), 1);
            b = _mm512_inserti64x4(_mm512_castsi256_si512(
                    _mm256_loadu_si256((const __m256i *)(r + i0 + 1))),
                _mm256_loadu_si256((const __m256i *)(r + i1 + 1)), 1);
            w = _mm512_mask_blend_epi32(
                0xff00, _mm512_set1_epi32(((f0 << 8) | (32 - f0)) * 0x10001),
                _mm512_set1_epi32(((f1 << 8) | (32 - f1)) * 0x10001));
        } else {
            __m128i ra[4], rb[4];
            w = _mm512_setzero_si512();
            for (int k = 0; k < 4; k++) {
                int i = ((y + k + 1) * angle) >> 5;
                int f = ((y + k + 1) * angle) & 31;
                ra[k] = _mm_loadu_si128((const __m128i *)(r + i));
                rb[k] = _mm_loadu_si128((const __m128i *)(r + i + 1));
                w = _mm512_mask_mov_epi32(
                    w, (__mmask16)(0xf << (4 * k)),
                    _mm512_set1_epi32(((f << 8) | (32 - f)) * 0x10001));
            }
            a = _mm512_inserti32x4(_mm512_inserti32x4(_mm512_inserti32x4(
                    _mm512_castsi128_si512(ra[0]), ra[1], 1), ra[2], 2),
                ra[3], 3);
            b = _mm512_inserti32x4(_mm512_inserti32x4(_mm512_inserti32x4(
                    _mm512_castsi128_si512(rb[0]), rb[1], 1), rb[2], 2),
                rb[3], 3);
        }
        __m512i ev = _mm512_srli_epi16(
            _mm512_add_epi16(_mm512_maddubs_epi16(a, w), r16), 5);
        __m512i od = _mm512_slli_epi16(
            _mm512_add_epi16(_mm512_maddubs_epi16(b, w), r16), 3);
        /* ev | (od & 0xff00) */
        __m512i v = _mm512_ternarylogic_epi32(ev, od, hi8, 0xf8);
        uint8_t *o = out + (size_t)y * os;
        if (n == 32) {
            _mm256_storeu_si256((__m256i *)o, _mm512_castsi512_si256(v));
            _mm256_storeu_si256((__m256i *)(o + os),
                                _mm512_extracti64x4_epi64(v, 1));
        } else {
            _mm_storeu_si128((__m128i *)o, _mm512_castsi512_si128(v));
            _mm_storeu_si128((__m128i *)(o + os),
                             _mm512_extracti32x4_epi32(v, 1));
            _mm_storeu_si128((__m128i *)(o + 2 * os),
                             _mm512_extracti32x4_epi32(v, 2));
            _mm_storeu_si128((__m128i *)(o + 3 * os),
                             _mm512_extracti32x4_epi32(v, 3));
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

#else   /* Main10 */

static void ang_rows(uint16_t *out, int os, const uint16_t *r, int n,
                     int angle)
{
    const __m512i r16 = _mm512_set1_epi16(16);
    const int per = 32 / n;

    for (int y = 0; y < n; y += per) {
        __m512i a, b, w;
        if (n == 32) {
            int i = ((y + 1) * angle) >> 5, f = ((y + 1) * angle) & 31;
            a = _mm512_loadu_si512(r + i);
            b = _mm512_loadu_si512(r + i + 1);
            w = _mm512_set1_epi16((int16_t)f);
        } else {
            int i0 = ((y + 1) * angle) >> 5, f0 = ((y + 1) * angle) & 31;
            int i1 = ((y + 2) * angle) >> 5, f1 = ((y + 2) * angle) & 31;
            a = _mm512_inserti64x4(_mm512_castsi256_si512(
                    _mm256_loadu_si256((const __m256i *)(r + i0))),
                _mm256_loadu_si256((const __m256i *)(r + i1)), 1);
            b = _mm512_inserti64x4(_mm512_castsi256_si512(
                    _mm256_loadu_si256((const __m256i *)(r + i0 + 1))),
                _mm256_loadu_si256((const __m256i *)(r + i1 + 1)), 1);
            w = _mm512_mask_blend_epi32(0xff00, _mm512_set1_epi16((int16_t)f0),
                                        _mm512_set1_epi16((int16_t)f1));
        }
        __m512i v = _mm512_add_epi16(
            _mm512_slli_epi16(a, 5),
            _mm512_mullo_epi16(_mm512_sub_epi16(b, a), w));
        v = _mm512_srli_epi16(_mm512_add_epi16(v, r16), 5);
        uint16_t *o = out + (size_t)y * os;
        if (n == 32) {
            _mm512_storeu_si512(o, v);
        } else {
            _mm256_storeu_si256((__m256i *)o, _mm512_castsi512_si256(v));
            _mm256_storeu_si256((__m256i *)(o + os),
                                _mm512_extracti64x4_epi64(v, 1));
        }
    }
}

static inline void trans8x8(uint16_t *d, int ds, const uint16_t *s, int ss)
{
    __m128i r[8], a[8], b[8];

    for (int i = 0; i < 8; i++)
        r[i] = _mm_loadu_si128((const __m128i *)(s + (size_t)i * ss));
    for (int i = 0; i < 4; i++) {
        a[2 * i] = _mm_unpacklo_epi16(r[2 * i], r[2 * i + 1]);
        a[2 * i + 1] = _mm_unpackhi_epi16(r[2 * i], r[2 * i + 1]);
    }
    for (int i = 0; i < 2; i++) {
        b[4 * i] = _mm_unpacklo_epi32(a[4 * i], a[4 * i + 2]);
        b[4 * i + 1] = _mm_unpackhi_epi32(a[4 * i], a[4 * i + 2]);
        b[4 * i + 2] = _mm_unpacklo_epi32(a[4 * i + 1], a[4 * i + 3]);
        b[4 * i + 3] = _mm_unpackhi_epi32(a[4 * i + 1], a[4 * i + 3]);
    }
    for (int i = 0; i < 4; i++) {
        _mm_storeu_si128((__m128i *)(d + (size_t)(2 * i) * ds),
                         _mm_unpacklo_epi64(b[i], b[4 + i]));
        _mm_storeu_si128((__m128i *)(d + (size_t)(2 * i + 1) * ds),
                         _mm_unpackhi_epi64(b[i], b[4 + i]));
    }
}
#endif

static void pred_angular(pixel *dst, int stride, const pixel *refs,
                         int log2n, int mode, int c_idx)
{
    int n = 1 << log2n;
    const pixel *top = refs + 1;
    const pixel *left = refs + 2 * n + 1;
    int corner = refs[0];
    int angle = ia_angle[mode];
    int inv = ia_inv_angle[mode];
    pixel ref[4 * 32 + 2 + 32];
    pixel *r = ref + n + 1;

    if (mode >= 18) {
        build_ref(r, top, left, corner, n, angle, inv);
        ang_rows(dst, stride, r, n, angle);
        if (mode == 26 && c_idx == 0 && n < 32)
            for (int y = 0; y < n; y++) {
                int v = top[0] + ((left[y] - corner) >> 1);
                dst[(size_t)y * stride] =
                    (pixel)(v < 0 ? 0 : v > PIXEL_MAX ? PIXEL_MAX : v);
            }
    } else {
        pixel tmp[32 * 32];
        build_ref(r, left, top, corner, n, angle, inv);
        ang_rows(tmp, n, r, n, angle);
        for (int by = 0; by < n; by += 8)
            for (int bx = 0; bx < n; bx += 8)
                trans8x8(dst + (size_t)bx * stride + by, stride,
                         tmp + (size_t)by * n + bx, n);
        if (mode == 10 && c_idx == 0 && n < 32)
            for (int x = 0; x < n; x++) {
                int v = left[0] + ((top[x] - corner) >> 1);
                dst[x] = (pixel)(v < 0 ? 0 : v > PIXEL_MAX ? PIXEL_MAX : v);
            }
    }
}

/* ---- planar (8.4.4.2.4) -------------------------------------------- */

#if BIT_DEPTH == 8
AI __m512i ld_top(const uint8_t *p, int n)
{
    if (n == 32)
        return _mm512_cvtepu8_epi16(_mm256_loadu_si256((const __m256i *)p));
    __m256i t = _mm256_cvtepu8_epi16(_mm_loadu_si128((const __m128i *)p));
    return _mm512_inserti64x4(_mm512_castsi256_si512(t), t, 1);
}
#else
AI __m512i ld_top(const uint16_t *p, int n)
{
    if (n == 32)
        return _mm512_loadu_si512(p);
    __m256i t = _mm256_loadu_si256((const __m256i *)p);
    return _mm512_inserti64x4(_mm512_castsi256_si512(t), t, 1);
}
#endif

static void pred_planar(pixel *dst, int stride, const pixel *refs, int log2n)
{
    const int n = 1 << log2n;
    const pixel *top = refs + 1;
    const pixel *left = refs + 2 * n + 1;
    const int tr = top[n], bl = left[n];
    const int per = n == 32 ? 1 : 2;
    /* lane i is column i % n of row i / n */
    const __m512i lane = _mm512_setr_epi32(
        0x10000, 0x30002, 0x50004, 0x70006, 0x90008, 0xb000a, 0xd000c,
        0xf000e, 0x110010, 0x130012, 0x150014, 0x170016, 0x190018,
        0x1b001a, 0x1d001c, 0x1f001e);
    const __m512i xi = _mm512_and_si512(lane, _mm512_set1_epi16((int16_t)(n - 1)));
    const __m512i row = _mm512_srli_epi16(lane, (unsigned)log2n);
    const __m512i sh = _mm512_set1_epi16((int16_t)(log2n + 1));
    const __m512i xw = _mm512_sub_epi16(_mm512_set1_epi16((int16_t)(n - 1)), xi);
    const __m512i base = _mm512_add_epi16(
        _mm512_mullo_epi16(_mm512_add_epi16(xi, _mm512_set1_epi16(1)),
                           _mm512_set1_epi16((int16_t)tr)),
        _mm512_set1_epi16((int16_t)n));
    const __m512i t = ld_top(top, n);
    const __m512i blv = _mm512_set1_epi16((int16_t)bl);
    const __m512i step = _mm512_mullo_epi16(_mm512_sub_epi16(blv, t),
                                            _mm512_set1_epi16((int16_t)per));
    /* (n - 1 - y) * top[x] + (y + 1) * bl at y = row */
    __m512i p = _mm512_add_epi16(
        _mm512_mullo_epi16(_mm512_sub_epi16(_mm512_set1_epi16((int16_t)(n - 1)),
                                            row), t),
        _mm512_mullo_epi16(_mm512_add_epi16(row, _mm512_set1_epi16(1)), blv));

    for (int y = 0; y < n; y += per) {
        __m512i ly = n == 32
            ? _mm512_set1_epi16((int16_t)left[y])
            : _mm512_mask_blend_epi32(0xff00,
                                      _mm512_set1_epi16((int16_t)left[y]),
                                      _mm512_set1_epi16((int16_t)left[y + 1]));
        __m512i v = _mm512_add_epi16(_mm512_add_epi16(base, p),
                                     _mm512_mullo_epi16(xw, ly));
        v = _mm512_srlv_epi16(v, sh);
        p = _mm512_add_epi16(p, step);
        pixel *o = dst + (size_t)y * stride;
#if BIT_DEPTH == 8
        __m256i b = _mm512_cvtepi16_epi8(v);
        if (n == 32) {
            _mm256_storeu_si256((__m256i *)o, b);
        } else {
            _mm_storeu_si128((__m128i *)o, _mm256_castsi256_si128(b));
            _mm_storeu_si128((__m128i *)(o + stride),
                             _mm256_extracti128_si256(b, 1));
        }
#else
        if (n == 32) {
            _mm512_storeu_si512(o, v);
        } else {
            _mm256_storeu_si256((__m256i *)o, _mm512_castsi512_si256(v));
            _mm256_storeu_si256((__m256i *)(o + stride),
                                _mm512_extracti64x4_epi64(v, 1));
        }
#endif
    }
}

void n265_intra_pred_avx512(pixel *dst, int dst_stride, const pixel *refs,
                            int log2n, int mode, int c_idx)
{
    if (!n265_intra_pred_avx512_wide(log2n, mode))
        n265_intra_pred_avx2(dst, dst_stride, refs, log2n, mode, c_idx);
    else if (mode == INTRA_PLANAR)
        pred_planar(dst, dst_stride, refs, log2n);
    else
        pred_angular(dst, dst_stride, refs, log2n, mode, c_idx);
}
