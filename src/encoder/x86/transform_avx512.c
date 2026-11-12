/*
 * transform_avx512.c - the transform family (N265_ASM_DCT, N265_ASM_QUANT)
 * on AVX-512 BW, at both depths: the 32-point forward and inverse DCT,
 * quant and dequant.
 *
 * transform_avx2.c's method and range arguments at twice the width. A
 * 32x32 pass takes all 32 columns in one vector per row, so each pass
 * forms half the products' instructions, and the transposes between the
 * passes move four 8x8 tiles per vector: one 64-byte row load spans four
 * tiles, the in-lane unpacks transpose each, and each lane is stored to
 * its own tile. The 4-, 8- and 16-point transforms stay AVX2.
 *
 * Quant and dequant are transform_avx2.c's per-coefficient arithmetic in
 * 32 lanes, with the sign and the zero tests in mask registers.
 *
 * The pair tables are transform_avx2.c's, as transform_sse42.c copies
 * them.
 *
 * Copyright (c) 2026, the yah265 authors. GPL-2.0-or-later; see LICENSE.
 */

#include <immintrin.h>
#include <stddef.h>
#include <string.h>

#include "encoder/transform.h"
#include "encoder/x86/avx2.h"
#include "encoder/x86/avx512.h"

#define AI static inline __attribute__((always_inline))

static const int16_t fwd_pairs[32 * 16][2] = {
    { 64, 64 }, { 64, 64 }, { 64, 64 }, { 64, 64 }, { 64, 64 }, { 64, 64 },
    { 64, 64 }, { 64, 64 }, { 64, 64 }, { 64, 64 }, { 64, 64 }, { 64, 64 },
    { 64, 64 }, { 64, 64 }, { 64, 64 }, { 64, 64 }, { 90, 90 }, { 88, 85 },
    { 82, 78 }, { 73, 67 }, { 61, 54 }, { 46, 38 }, { 31, 22 }, { 13, 4 },
    { -4, -13 }, { -22, -31 }, { -38, -46 }, { -54, -61 }, { -67, -73 },
    { -78, -82 }, { -85, -88 }, { -90, -90 }, { 90, 87 }, { 80, 70 },
    { 57, 43 }, { 25, 9 }, { -9, -25 }, { -43, -57 }, { -70, -80 },
    { -87, -90 }, { -90, -87 }, { -80, -70 }, { -57, -43 }, { -25, -9 },
    { 9, 25 }, { 43, 57 }, { 70, 80 }, { 87, 90 }, { 90, 82 }, { 67, 46 },
    { 22, -4 }, { -31, -54 }, { -73, -85 }, { -90, -88 }, { -78, -61 },
    { -38, -13 }, { 13, 38 }, { 61, 78 }, { 88, 90 }, { 85, 73 }, { 54, 31 },
    { 4, -22 }, { -46, -67 }, { -82, -90 }, { 89, 75 }, { 50, 18 },
    { -18, -50 }, { -75, -89 }, { -89, -75 }, { -50, -18 }, { 18, 50 },
    { 75, 89 }, { 89, 75 }, { 50, 18 }, { -18, -50 }, { -75, -89 },
    { -89, -75 }, { -50, -18 }, { 18, 50 }, { 75, 89 }, { 88, 67 },
    { 31, -13 }, { -54, -82 }, { -90, -78 }, { -46, -4 }, { 38, 73 },
    { 90, 85 }, { 61, 22 }, { -22, -61 }, { -85, -90 }, { -73, -38 },
    { 4, 46 }, { 78, 90 }, { 82, 54 }, { 13, -31 }, { -67, -88 }, { 87, 57 },
    { 9, -43 }, { -80, -90 }, { -70, -25 }, { 25, 70 }, { 90, 80 },
    { 43, -9 }, { -57, -87 }, { -87, -57 }, { -9, 43 }, { 80, 90 },
    { 70, 25 }, { -25, -70 }, { -90, -80 }, { -43, 9 }, { 57, 87 },
    { 85, 46 }, { -13, -67 }, { -90, -73 }, { -22, 38 }, { 82, 88 },
    { 54, -4 }, { -61, -90 }, { -78, -31 }, { 31, 78 }, { 90, 61 },
    { 4, -54 }, { -88, -82 }, { -38, 22 }, { 73, 90 }, { 67, 13 },
    { -46, -85 }, { 83, 36 }, { -36, -83 }, { -83, -36 }, { 36, 83 },
    { 83, 36 }, { -36, -83 }, { -83, -36 }, { 36, 83 }, { 83, 36 },
    { -36, -83 }, { -83, -36 }, { 36, 83 }, { 83, 36 }, { -36, -83 },
    { -83, -36 }, { 36, 83 }, { 82, 22 }, { -54, -90 }, { -61, 13 },
    { 78, 85 }, { 31, -46 }, { -90, -67 }, { 4, 73 }, { 88, 38 },
    { -38, -88 }, { -73, -4 }, { 67, 90 }, { 46, -31 }, { -85, -78 },
    { -13, 61 }, { 90, 54 }, { -22, -82 }, { 80, 9 }, { -70, -87 },
    { -25, 57 }, { 90, 43 }, { -43, -90 }, { -57, 25 }, { 87, 70 },
    { -9, -80 }, { -80, -9 }, { 70, 87 }, { 25, -57 }, { -90, -43 },
    { 43, 90 }, { 57, -25 }, { -87, -70 }, { 9, 80 }, { 78, -4 },
    { -82, -73 }, { 13, 85 }, { 67, -22 }, { -88, -61 }, { 31, 90 },
    { 54, -38 }, { -90, -46 }, { 46, 90 }, { 38, -54 }, { -90, -31 },
    { 61, 88 }, { 22, -67 }, { -85, -13 }, { 73, 82 }, { 4, -78 },
    { 75, -18 }, { -89, -50 }, { 50, 89 }, { 18, -75 }, { -75, 18 },
    { 89, 50 }, { -50, -89 }, { -18, 75 }, { 75, -18 }, { -89, -50 },
    { 50, 89 }, { 18, -75 }, { -75, 18 }, { 89, 50 }, { -50, -89 },
    { -18, 75 }, { 73, -31 }, { -90, -22 }, { 78, 67 }, { -38, -90 },
    { -13, 82 }, { 61, -46 }, { -88, -4 }, { 85, 54 }, { -54, -85 },
    { 4, 88 }, { 46, -61 }, { -82, 13 }, { 90, 38 }, { -67, -78 }, { 22, 90 },
    { 31, -73 }, { 70, -43 }, { -87, 9 }, { 90, 25 }, { -80, -57 },
    { 57, 80 }, { -25, -90 }, { -9, 87 }, { 43, -70 }, { -70, 43 },
    { 87, -9 }, { -90, -25 }, { 80, 57 }, { -57, -80 }, { 25, 90 },
    { 9, -87 }, { -43, 70 }, { 67, -54 }, { -78, 38 }, { 85, -22 },
    { -90, 4 }, { 90, 13 }, { -88, -31 }, { 82, 46 }, { -73, -61 },
    { 61, 73 }, { -46, -82 }, { 31, 88 }, { -13, -90 }, { -4, 90 },
    { 22, -85 }, { -38, 78 }, { 54, -67 }, { 64, -64 }, { -64, 64 },
    { 64, -64 }, { -64, 64 }, { 64, -64 }, { -64, 64 }, { 64, -64 },
    { -64, 64 }, { 64, -64 }, { -64, 64 }, { 64, -64 }, { -64, 64 },
    { 64, -64 }, { -64, 64 }, { 64, -64 }, { -64, 64 }, { 61, -73 },
    { -46, 82 }, { 31, -88 }, { -13, 90 }, { -4, -90 }, { 22, 85 },
    { -38, -78 }, { 54, 67 }, { -67, -54 }, { 78, 38 }, { -85, -22 },
    { 90, 4 }, { -90, 13 }, { 88, -31 }, { -82, 46 }, { 73, -61 },
    { 57, -80 }, { -25, 90 }, { -9, -87 }, { 43, 70 }, { -70, -43 },
    { 87, 9 }, { -90, 25 }, { 80, -57 }, { -57, 80 }, { 25, -90 }, { 9, 87 },
    { -43, -70 }, { 70, 43 }, { -87, -9 }, { 90, -25 }, { -80, 57 },
    { 54, -85 }, { -4, 88 }, { -46, -61 }, { 82, 13 }, { -90, 38 },
    { 67, -78 }, { -22, 90 }, { -31, -73 }, { 73, 31 }, { -90, 22 },
    { 78, -67 }, { -38, 90 }, { -13, -82 }, { 61, 46 }, { -88, 4 },
    { 85, -54 }, { 50, -89 }, { 18, 75 }, { -75, -18 }, { 89, -50 },
    { -50, 89 }, { -18, -75 }, { 75, 18 }, { -89, 50 }, { 50, -89 },
    { 18, 75 }, { -75, -18 }, { 89, -50 }, { -50, 89 }, { -18, -75 },
    { 75, 18 }, { -89, 50 }, { 46, -90 }, { 38, 54 }, { -90, 31 },
    { 61, -88 }, { 22, 67 }, { -85, 13 }, { 73, -82 }, { 4, 78 }, { -78, -4 },
    { 82, -73 }, { -13, 85 }, { -67, -22 }, { 88, -61 }, { -31, 90 },
    { -54, -38 }, { 90, -46 }, { 43, -90 }, { 57, 25 }, { -87, 70 },
    { 9, -80 }, { 80, -9 }, { -70, 87 }, { -25, -57 }, { 90, -43 },
    { -43, 90 }, { -57, -25 }, { 87, -70 }, { -9, 80 }, { -80, 9 },
    { 70, -87 }, { 25, 57 }, { -90, 43 }, { 38, -88 }, { 73, -4 },
    { -67, 90 }, { -46, -31 }, { 85, -78 }, { 13, 61 }, { -90, 54 },
    { 22, -82 }, { 82, -22 }, { -54, 90 }, { -61, -13 }, { 78, -85 },
    { 31, 46 }, { -90, 67 }, { 4, -73 }, { 88, -38 }, { 36, -83 },
    { 83, -36 }, { -36, 83 }, { -83, 36 }, { 36, -83 }, { 83, -36 },
    { -36, 83 }, { -83, 36 }, { 36, -83 }, { 83, -36 }, { -36, 83 },
    { -83, 36 }, { 36, -83 }, { 83, -36 }, { -36, 83 }, { -83, 36 },
    { 31, -78 }, { 90, -61 }, { 4, 54 }, { -88, 82 }, { -38, -22 },
    { 73, -90 }, { 67, -13 }, { -46, 85 }, { -85, 46 }, { 13, -67 },
    { 90, -73 }, { 22, 38 }, { -82, 88 }, { -54, -4 }, { 61, -90 },
    { 78, -31 }, { 25, -70 }, { 90, -80 }, { 43, 9 }, { -57, 87 },
    { -87, 57 }, { -9, -43 }, { 80, -90 }, { 70, -25 }, { -25, 70 },
    { -90, 80 }, { -43, -9 }, { 57, -87 }, { 87, -57 }, { 9, 43 },
    { -80, 90 }, { -70, 25 }, { 22, -61 }, { 85, -90 }, { 73, -38 },
    { -4, 46 }, { -78, 90 }, { -82, 54 }, { -13, -31 }, { 67, -88 },
    { 88, -67 }, { 31, 13 }, { -54, 82 }, { -90, 78 }, { -46, 4 },
    { 38, -73 }, { 90, -85 }, { 61, -22 }, { 18, -50 }, { 75, -89 },
    { 89, -75 }, { 50, -18 }, { -18, 50 }, { -75, 89 }, { -89, 75 },
    { -50, 18 }, { 18, -50 }, { 75, -89 }, { 89, -75 }, { 50, -18 },
    { -18, 50 }, { -75, 89 }, { -89, 75 }, { -50, 18 }, { 13, -38 },
    { 61, -78 }, { 88, -90 }, { 85, -73 }, { 54, -31 }, { 4, 22 },
    { -46, 67 }, { -82, 90 }, { -90, 82 }, { -67, 46 }, { -22, -4 },
    { 31, -54 }, { 73, -85 }, { 90, -88 }, { 78, -61 }, { 38, -13 },
    { 9, -25 }, { 43, -57 }, { 70, -80 }, { 87, -90 }, { 90, -87 },
    { 80, -70 }, { 57, -43 }, { 25, -9 }, { -9, 25 }, { -43, 57 },
    { -70, 80 }, { -87, 90 }, { -90, 87 }, { -80, 70 }, { -57, 43 },
    { -25, 9 }, { 4, -13 }, { 22, -31 }, { 38, -46 }, { 54, -61 },
    { 67, -73 }, { 78, -82 }, { 85, -88 }, { 90, -90 }, { 90, -90 },
    { 88, -85 }, { 82, -78 }, { 73, -67 }, { 61, -54 }, { 46, -38 },
    { 31, -22 }, { 13, -4 },
};

static const int16_t inv32_pairs[16 * 16][2] = {
    { 64, 90 }, { 89, 87 }, { 83, 80 }, { 75, 70 }, { 64, 57 }, { 50, 43 },
    { 36, 25 }, { 18, 9 }, { 90, 90 }, { 88, 85 }, { 82, 78 }, { 73, 67 },
    { 61, 54 }, { 46, 38 }, { 31, 22 }, { 13, 4 }, { 64, 87 }, { 75, 57 },
    { 36, 9 }, { -18, -43 }, { -64, -80 }, { -89, -90 }, { -83, -70 },
    { -50, -25 }, { 90, 82 }, { 67, 46 }, { 22, -4 }, { -31, -54 },
    { -73, -85 }, { -90, -88 }, { -78, -61 }, { -38, -13 }, { 64, 80 },
    { 50, 9 }, { -36, -70 }, { -89, -87 }, { -64, -25 }, { 18, 57 },
    { 83, 90 }, { 75, 43 }, { 88, 67 }, { 31, -13 }, { -54, -82 },
    { -90, -78 }, { -46, -4 }, { 38, 73 }, { 90, 85 }, { 61, 22 }, { 64, 70 },
    { 18, -43 }, { -83, -87 }, { -50, 9 }, { 64, 90 }, { 75, 25 },
    { -36, -80 }, { -89, -57 }, { 85, 46 }, { -13, -67 }, { -90, -73 },
    { -22, 38 }, { 82, 88 }, { 54, -4 }, { -61, -90 }, { -78, -31 },
    { 64, 57 }, { -18, -80 }, { -83, -25 }, { 50, 90 }, { 64, -9 },
    { -75, -87 }, { -36, 43 }, { 89, 70 }, { 82, 22 }, { -54, -90 },
    { -61, 13 }, { 78, 85 }, { 31, -46 }, { -90, -67 }, { 4, 73 }, { 88, 38 },
    { 64, 43 }, { -50, -90 }, { -36, 57 }, { 89, 25 }, { -64, -87 },
    { -18, 70 }, { 83, 9 }, { -75, -80 }, { 78, -4 }, { -82, -73 },
    { 13, 85 }, { 67, -22 }, { -88, -61 }, { 31, 90 }, { 54, -38 },
    { -90, -46 }, { 64, 25 }, { -75, -70 }, { 36, 90 }, { 18, -80 },
    { -64, 43 }, { 89, 9 }, { -83, -57 }, { 50, 87 }, { 73, -31 },
    { -90, -22 }, { 78, 67 }, { -38, -90 }, { -13, 82 }, { 61, -46 },
    { -88, -4 }, { 85, 54 }, { 64, 9 }, { -89, -25 }, { 83, 43 },
    { -75, -57 }, { 64, 70 }, { -50, -80 }, { 36, 87 }, { -18, -90 },
    { 67, -54 }, { -78, 38 }, { 85, -22 }, { -90, 4 }, { 90, 13 },
    { -88, -31 }, { 82, 46 }, { -73, -61 }, { 64, -9 }, { -89, 25 },
    { 83, -43 }, { -75, 57 }, { 64, -70 }, { -50, 80 }, { 36, -87 },
    { -18, 90 }, { 61, -73 }, { -46, 82 }, { 31, -88 }, { -13, 90 },
    { -4, -90 }, { 22, 85 }, { -38, -78 }, { 54, 67 }, { 64, -25 },
    { -75, 70 }, { 36, -90 }, { 18, 80 }, { -64, -43 }, { 89, -9 },
    { -83, 57 }, { 50, -87 }, { 54, -85 }, { -4, 88 }, { -46, -61 },
    { 82, 13 }, { -90, 38 }, { 67, -78 }, { -22, 90 }, { -31, -73 },
    { 64, -43 }, { -50, 90 }, { -36, -57 }, { 89, -25 }, { -64, 87 },
    { -18, -70 }, { 83, -9 }, { -75, 80 }, { 46, -90 }, { 38, 54 },
    { -90, 31 }, { 61, -88 }, { 22, 67 }, { -85, 13 }, { 73, -82 }, { 4, 78 },
    { 64, -57 }, { -18, 80 }, { -83, 25 }, { 50, -90 }, { 64, 9 },
    { -75, 87 }, { -36, -43 }, { 89, -70 }, { 38, -88 }, { 73, -4 },
    { -67, 90 }, { -46, -31 }, { 85, -78 }, { 13, 61 }, { -90, 54 },
    { 22, -82 }, { 64, -70 }, { 18, 43 }, { -83, 87 }, { -50, -9 },
    { 64, -90 }, { 75, -25 }, { -36, 80 }, { -89, 57 }, { 31, -78 },
    { 90, -61 }, { 4, 54 }, { -88, 82 }, { -38, -22 }, { 73, -90 },
    { 67, -13 }, { -46, 85 }, { 64, -80 }, { 50, -9 }, { -36, 70 },
    { -89, 87 }, { -64, 25 }, { 18, -57 }, { 83, -90 }, { 75, -43 },
    { 22, -61 }, { 85, -90 }, { 73, -38 }, { -4, 46 }, { -78, 90 },
    { -82, 54 }, { -13, -31 }, { 67, -88 }, { 64, -87 }, { 75, -57 },
    { 36, -9 }, { -18, 43 }, { -64, 80 }, { -89, 90 }, { -83, 70 },
    { -50, 25 }, { 13, -38 }, { 61, -78 }, { 88, -90 }, { 85, -73 },
    { 54, -31 }, { 4, 22 }, { -46, 67 }, { -82, 90 }, { 64, -90 },
    { 89, -87 }, { 83, -80 }, { 75, -70 }, { 64, -57 }, { 50, -43 },
    { 36, -25 }, { 18, -9 }, { 4, -13 }, { 22, -31 }, { 38, -46 },
    { 54, -61 }, { 67, -73 }, { 78, -82 }, { 85, -88 }, { 90, -90 },
};

/* ---- helpers -------------------------------------------------------- */

AI __m512i pair(const int16_t p[2])
{
    int32_t w;

    memcpy(&w, p, 4);
    return _mm512_set1_epi32(w);
}

AI __m512i ld32(const int16_t *p)
{
    return _mm512_loadu_si512(p);
}

/* (lo + add) >> sh and (hi + add) >> sh, narrowed with saturation */
AI __m512i rnd_pack(__m512i lo, __m512i hi, __m512i add, __m128i sh)
{
    lo = _mm512_sra_epi32(_mm512_add_epi32(lo, add), sh);
    hi = _mm512_sra_epi32(_mm512_add_epi32(hi, add), sh);
    return _mm512_packs_epi32(lo, hi);
}

/* 32 x 32 int16, both packed: per 8-row band, one row load spans four
 * 8x8 tiles and lane j of each output vector is row r of tile j */
static void transpose32(int16_t *dst, const int16_t *src)
{
    for (int ty = 0; ty < 32; ty += 8) {
        __m512i r[8], a[8], b[8];

        for (int i = 0; i < 8; i++)
            r[i] = ld32(src + (ty + i) * 32);
        for (int i = 0; i < 4; i++) {
            a[2 * i] = _mm512_unpacklo_epi16(r[2 * i], r[2 * i + 1]);
            a[2 * i + 1] = _mm512_unpackhi_epi16(r[2 * i], r[2 * i + 1]);
        }
        for (int h = 0; h < 2; h++) {
            b[4 * h + 0] = _mm512_unpacklo_epi32(a[4 * h], a[4 * h + 2]);
            b[4 * h + 1] = _mm512_unpackhi_epi32(a[4 * h], a[4 * h + 2]);
            b[4 * h + 2] = _mm512_unpacklo_epi32(a[4 * h + 1], a[4 * h + 3]);
            b[4 * h + 3] = _mm512_unpackhi_epi32(a[4 * h + 1], a[4 * h + 3]);
        }
        for (int i = 0; i < 4; i++) {
            __m512i o[2] = { _mm512_unpacklo_epi64(b[i], b[4 + i]),
                             _mm512_unpackhi_epi64(b[i], b[4 + i]) };

            for (int k = 0; k < 2; k++) {
                int16_t *d = dst + (2 * i + k) * 32 + ty;

                _mm_storeu_si128((__m128i *)d, _mm512_castsi512_si128(o[k]));
                _mm_storeu_si128((__m128i *)(d + 8 * 32),
                                 _mm512_extracti32x4_epi32(o[k], 1));
                _mm_storeu_si128((__m128i *)(d + 16 * 32),
                                 _mm512_extracti32x4_epi32(o[k], 2));
                _mm_storeu_si128((__m128i *)(d + 24 * 32),
                                 _mm512_extracti32x4_epi32(o[k], 3));
            }
        }
    }
}

AI __m512i rnd_add(int shift)
{
    return _mm512_set1_epi32(1 << (shift - 1));
}

AI __m128i rnd_sh(int shift)
{
    return _mm_cvtsi32_si128(shift);
}

/* ---- forward -------------------------------------------------------- */

/* pass 1 with the first butterfly level: even outputs dot the sums of
 * rows k and 31 - k, odd ones their differences */
static void fwd32_split(int16_t *out, const int16_t *in, __m512i add,
                        __m128i sh)
{
    for (int par = 0; par < 2; par++) {
        __m512i xl[8], xh[8];

        for (int m = 0; m < 8; m++) {
            __m512i a0 = ld32(in + (2 * m) * 32), b0 = ld32(in + (31 - 2 * m) * 32);
            __m512i a1 = ld32(in + (2 * m + 1) * 32);
            __m512i b1 = ld32(in + (30 - 2 * m) * 32);
            __m512i v0 = par ? _mm512_sub_epi16(a0, b0) : _mm512_add_epi16(a0, b0);
            __m512i v1 = par ? _mm512_sub_epi16(a1, b1) : _mm512_add_epi16(a1, b1);

            xl[m] = _mm512_unpacklo_epi16(v0, v1);
            xh[m] = _mm512_unpackhi_epi16(v0, v1);
        }
        for (int i = par; i < 32; i += 2) {
            const int16_t (*row)[2] = fwd_pairs + i * 16;
            __m512i lo = _mm512_setzero_si512(), hi = lo;

            for (int m = 0; m < 8; m++) {
                __m512i c = pair(row[m]);

                lo = _mm512_add_epi32(lo, _mm512_madd_epi16(xl[m], c));
                hi = _mm512_add_epi32(hi, _mm512_madd_epi16(xh[m], c));
            }
            _mm512_storeu_si512(out + i * 32, rnd_pack(lo, hi, add, sh));
        }
    }
}

/* pass 2, the direct product: its butterfly sums would leave int16 */
static void fwd32_direct(int16_t *out, const int16_t *in, __m512i add,
                         __m128i sh)
{
    __m512i il[16], ih[16];

    for (int m = 0; m < 16; m++) {
        __m512i a = ld32(in + 2 * m * 32), b = ld32(in + (2 * m + 1) * 32);

        il[m] = _mm512_unpacklo_epi16(a, b);
        ih[m] = _mm512_unpackhi_epi16(a, b);
    }
    for (int i = 0; i < 32; i++) {
        const int16_t (*row)[2] = fwd_pairs + i * 16;
        __m512i lo = _mm512_setzero_si512(), hi = lo;

        for (int m = 0; m < 16; m++) {
            __m512i c = pair(row[m]);

            lo = _mm512_add_epi32(lo, _mm512_madd_epi16(il[m], c));
            hi = _mm512_add_epi32(hi, _mm512_madd_epi16(ih[m], c));
        }
        _mm512_storeu_si512(out + i * 32, rnd_pack(lo, hi, add, sh));
    }
}

void n265_fdct_avx512(int16_t *coef, const int16_t *resid, int log2n,
                      int use_dst)
{
    if (log2n != 5 || use_dst) {
        n265_fdct_avx2(coef, resid, log2n, use_dst);
        return;
    }
    _Alignas(64) int16_t b[32 * 32], t[32 * 32];
    const int s1 = 5 + BIT_DEPTH - 9, s2 = 5 + 6;

    fwd32_split(b, resid, rnd_add(s1), rnd_sh(s1));
    transpose32(t, b);
    fwd32_direct(b, t, rnd_add(s2), rnd_sh(s2));
    transpose32(coef, b);
}

/* ---- inverse -------------------------------------------------------- */

/* out[j] = E[j] + O[j] and out[31-j] = E[j] - O[j]; input rows at or past
 * kb are zero and are not read */
static void inv32_split(int16_t *out, const int16_t *in, int kb, __m512i add,
                        __m128i sh)
{
    int me = (kb + 3) >> 2, mo = (kb + 2) >> 2;
    __m512i el[8], eh[8], ol[8], oh[8];

    if (me > 8) me = 8;
    if (mo > 8) mo = 8;
    for (int m = 0; m < me; m++) {
        __m512i a = ld32(in + 4 * m * 32), b = ld32(in + (4 * m + 2) * 32);

        el[m] = _mm512_unpacklo_epi16(a, b);
        eh[m] = _mm512_unpackhi_epi16(a, b);
    }
    for (int m = 0; m < mo; m++) {
        __m512i a = ld32(in + (4 * m + 1) * 32), b = ld32(in + (4 * m + 3) * 32);

        ol[m] = _mm512_unpacklo_epi16(a, b);
        oh[m] = _mm512_unpackhi_epi16(a, b);
    }
    for (int j = 0; j < 16; j++) {
        const int16_t (*row)[2] = inv32_pairs + j * 16;
        __m512i elo = _mm512_setzero_si512(), ehi = elo, olo = elo, ohi = elo;

        for (int m = 0; m < me; m++) {
            __m512i c = pair(row[m]);

            elo = _mm512_add_epi32(elo, _mm512_madd_epi16(el[m], c));
            ehi = _mm512_add_epi32(ehi, _mm512_madd_epi16(eh[m], c));
        }
        for (int m = 0; m < mo; m++) {
            __m512i c = pair(row[8 + m]);

            olo = _mm512_add_epi32(olo, _mm512_madd_epi16(ol[m], c));
            ohi = _mm512_add_epi32(ohi, _mm512_madd_epi16(oh[m], c));
        }
        _mm512_storeu_si512(out + j * 32,
            rnd_pack(_mm512_add_epi32(elo, olo), _mm512_add_epi32(ehi, ohi),
                     add, sh));
        _mm512_storeu_si512(out + (31 - j) * 32,
            rnd_pack(_mm512_sub_epi32(elo, olo), _mm512_sub_epi32(ehi, ohi),
                     add, sh));
    }
}

/* dst = clip(pred + r) over 32 x 32; r is one vector per row, or the
 * same vector for every row */
static void add_pred32(pixel *dst, int ds, const pixel *pred, int ps,
                       const int16_t *r, int rstep)
{
    const __m512i z = _mm512_setzero_si512();

    for (int y = 0; y < 32; y++, r += rstep) {
        const pixel *p = pred + (ptrdiff_t)y * ps;
        pixel *d = dst + (ptrdiff_t)y * ds;
#if BIT_DEPTH == 8
        __m512i v = _mm512_adds_epi16(ld32(r), _mm512_cvtepu8_epi16(
            _mm256_loadu_si256((const __m256i *)p)));

        _mm256_storeu_si256((__m256i *)d,
                            _mm512_cvtusepi16_epi8(_mm512_max_epi16(v, z)));
#else
        __m512i v = _mm512_adds_epi16(ld32(r), _mm512_loadu_si512(p));

        _mm512_storeu_si512(d, _mm512_min_epi16(_mm512_max_epi16(v, z),
                                                _mm512_set1_epi16(PIXEL_MAX)));
#endif
    }
}

void n265_idct_add_avx512(pixel *dst, int dst_stride, const int16_t *dq,
                          int log2n, const pixel *pred, int pred_stride,
                          int use_dst, int nzw, int nzh)
{
    if (log2n != 5 || use_dst) {
        n265_idct_add_avx2(dst, dst_stride, dq, log2n, pred, pred_stride,
                           use_dst, nzw, nzh);
        return;
    }
    _Alignas(64) int16_t b[32 * 32], t[32 * 32];
    const int s2 = 20 - BIT_DEPTH;

    nzw = nzw < 1 ? 1 : nzw > 32 ? 32 : nzw;
    nzh = nzh < 1 ? 1 : nzh > 32 ? 32 : nzh;
    if (nzw == 1 && nzh == 1) {
        /* the C's DC-only constant */
        int v = (64 * dq[0] + 64) >> 7;

        v = v < -32768 ? -32768 : v > 32767 ? 32767 : v;
        _mm512_store_si512(t, _mm512_set1_epi16(
            (int16_t)((64 * v + (1 << (s2 - 1))) >> s2)));
        add_pred32(dst, dst_stride, pred, pred_stride, t, 0);
        return;
    }
    inv32_split(b, dq, nzh, rnd_add(7), rnd_sh(7));
    transpose32(t, b);
    inv32_split(b, t, nzw, rnd_add(s2), rnd_sh(s2));
    transpose32(t, b);
    add_pred32(dst, dst_stride, pred, pred_stride, t, 32);
}

/* ---- quant / dequant (N265_ASM_QUANT) --------------------------------
 *
 * transform_avx2.c's arithmetic; a 4x4 block is one vector's low half. */

AI __mmask32 lanes(int nn)
{
    return nn >= 32 ? 0xffffffffu : (__mmask32)((1u << nn) - 1);
}

void n265_dequant_avx512(int16_t *dq, const int16_t *level, int log2n, int qp,
                         int *nzw, int *nzh)
{
    static const int level_scale[6] = { 40, 45, 51, 57, 64, 72 };
    const int n = 1 << log2n, nn = n * n;
    const int qpq = qp + QP_BD_OFFSET;
    const int bd = BIT_DEPTH + log2n - 5;
    int32_t s = (int32_t)(((int64_t)level_scale[qpq % 6] << (qpq / 6)) * 16);
    __m512i shi = _mm512_set1_epi32(s >> bd);
    __m512i slo = _mm512_set1_epi32(s & ((1 << bd) - 1));
    __m512i add = rnd_add(bd), z = _mm512_setzero_si512();
    __m128i sh = rnd_sh(bd);
    const __mmask32 m = lanes(nn);
    uint32_t cols = 0;
    int my = 0;

    for (int i = 0; i < nn; i += 32) {
        __m512i l = _mm512_maskz_loadu_epi16(m, level + i);
        __m512i ll = _mm512_unpacklo_epi16(l, z), lh = _mm512_unpackhi_epi16(l, z);
        __m512i vl = _mm512_add_epi32(_mm512_madd_epi16(ll, shi),
            _mm512_sra_epi32(_mm512_add_epi32(_mm512_madd_epi16(ll, slo), add), sh));
        __m512i vh = _mm512_add_epi32(_mm512_madd_epi16(lh, shi),
            _mm512_sra_epi32(_mm512_add_epi32(_mm512_madd_epi16(lh, slo), add), sh));
        __m512i v = _mm512_packs_epi32(vl, vh);
        uint32_t nz = _mm512_test_epi16_mask(v, v);

        _mm512_mask_storeu_epi16(dq + i, m, v);
        if (!nz)
            continue;
        /* one mask bit per coefficient, 32 / n rows to a vector */
        if (n == 32) {
            cols |= nz;
            my = i / 32;
        } else {
            for (int r = 0; r < 32 / n; r++) {
                uint32_t row = (nz >> (n * r)) & ((1u << n) - 1);

                if (row) {
                    cols |= row;
                    my = i / n + r;
                }
            }
        }
    }
    if (nzw) *nzw = cols ? 32 - __builtin_clz(cols) : 1;
    if (nzh) *nzh = my + 1;
}

int n265_quant_avx512(int16_t *level, const int16_t *coef, int log2n, int qp,
                      int intra)
{
    static const int quant_scale[6] = {
        26214, 23302, 20560, 18396, 16384, 14564
    };
    const int nn = 1 << (2 * log2n);
    const int qpq = qp + QP_BD_OFFSET;
    const int qbits = 29 - BIT_DEPTH + qpq / 6 - log2n;
    int32_t add = (int32_t)(((int64_t)1 << qbits) /
                            (intra == 2 ? 2 : intra ? 3 : 6));
    __m512i q = _mm512_set1_epi16((int16_t)quant_scale[qpq % 6]);
    __m512i addv = _mm512_set1_epi32(add), z = _mm512_setzero_si512();
    __m128i sh = _mm_cvtsi32_si128(qbits);
    const __mmask32 m = lanes(nn);
    int nz = 0;

    for (int i = 0; i < nn; i += 32) {
        __m512i c = _mm512_maskz_loadu_epi16(m, coef + i);
        __m512i a = _mm512_abs_epi16(c);
        __m512i pl = _mm512_mullo_epi16(a, q), ph = _mm512_mulhi_epu16(a, q);
        __m512i lo = _mm512_srl_epi32(
            _mm512_add_epi32(_mm512_unpacklo_epi16(pl, ph), addv), sh);
        __m512i hi = _mm512_srl_epi32(
            _mm512_add_epi32(_mm512_unpackhi_epi16(pl, ph), addv), sh);
        __m512i l = _mm512_packs_epi32(lo, hi);

        nz += __builtin_popcount(_mm512_mask_test_epi16_mask(m, l, l));
        l = _mm512_mask_sub_epi16(l, _mm512_movepi16_mask(c), z, l);
        _mm512_mask_storeu_epi16(level + i, m, l);
    }
    return nz;
}
