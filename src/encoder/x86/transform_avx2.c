/*
 * transform_avx2.c - the transform family (N265_ASM_DCT, N265_ASM_QUANT)
 * on AVX2. The entry points are src/encoder/arm/neon.h's transform block,
 * renamed, at both depths.
 *
 * Each 1-D pass runs one independent transform per int16 lane (a column
 * of the block), so a row of the block is one vector and the length
 * dimension never crosses lanes. Products come from _mm256_madd_epi16 on
 * two interleaved rows against a broadcast pair of matrix entries. Every
 * output is then a sum of the exact products the C's matrix form adds, in
 * another order. Nothing overflows int32 (below), so the sum is the same
 * integer, and the rounding shift and the clips are the C's. Between the
 * passes the block is transposed in int16.
 *
 * RANGE at 8 bits (the row and column |T| sums are 256, 512, 1024, 2048
 * and 247, 479, 940, 1862 at 4..32 points; 242 for the DST):
 *   - forward pass 1: |resid| <= 255, so a butterfly sum is at most
 *     255 * N and the rounded output at most 255 * 2048 >> 4 = 32640.
 *     It is exact in int16, which is the C's int32 `b`.
 *   - forward pass 2: |b| <= 32640, sums under 2^27, packs_epi32's
 *     saturation is the C's clip to int16.
 *   - inverse: |dq| <= 32768 and pass 1's output is clipped to int16 by
 *     packs, so both passes sum under 2^26. Pass 2's packs can saturate
 *     only where the sample clip after it saturates too (adds_epi16, then
 *     packus), which is clip3(0, 255, pred + r) for the C's int32 r.
 *   - madd overflows only on -32768 * -32768 twice, and no matrix entry
 *     is -32768.
 *
 * At Main10 |resid| <= 1023 and the forward shift1 is two larger, so pass
 * 1's output is at most 1023 * 2048 >> 6 = 32736 and every bound above
 * holds. Only add_pred sees a sample: it clips with min/max against
 * PIXEL_MAX where 8 bits uses packus.
 *
 * The matrix tables are the C's n265_tmatrix (tables.c builds it at run
 * time) and the DST-VII, written as the pairs each madd broadcasts.
 *
 * Copyright (c) 2026, the yah265 authors. GPL-2.0-or-later; see LICENSE.
 */

#include <immintrin.h>
#include <stddef.h>
#include <string.h>

#include "encoder/transform.h"
#include "encoder/x86/avx2.h"

#define AI_ static inline __attribute__((always_inline))

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

static const int16_t dst_fwd_pairs[4 * 2][2] = {
    { 29, 55 }, { 74, 84 }, { 74, 74 }, { 0, -74 }, { 84, -29 }, { -74, 55 },
    { 55, -84 }, { 74, -29 },
};

static const int16_t dst_inv_pairs[4 * 2][2] = {
    { 29, 74 }, { 84, 55 }, { 55, 74 }, { -29, -84 }, { 74, 0 }, { -74, 74 },
    { 84, -74 }, { 55, -29 },
};

static const int16_t inv4_pairs[2 * 2][2] = {
    { 64, 64 }, { 83, 36 }, { 64, -64 }, { 36, -83 },
};

static const int16_t inv8_pairs[4 * 4][2] = {
    { 64, 83 }, { 64, 36 }, { 89, 75 }, { 50, 18 }, { 64, 36 }, { -64, -83 },
    { 75, -18 }, { -89, -50 }, { 64, -36 }, { -64, 83 }, { 50, -89 },
    { 18, 75 }, { 64, -83 }, { 64, -36 }, { 18, -50 }, { 75, -89 },
};

static const int16_t inv16_pairs[8 * 8][2] = {
    { 64, 89 }, { 83, 75 }, { 64, 50 }, { 36, 18 }, { 90, 87 }, { 80, 70 },
    { 57, 43 }, { 25, 9 }, { 64, 75 }, { 36, -18 }, { -64, -89 },
    { -83, -50 }, { 87, 57 }, { 9, -43 }, { -80, -90 }, { -70, -25 },
    { 64, 50 }, { -36, -89 }, { -64, 18 }, { 83, 75 }, { 80, 9 },
    { -70, -87 }, { -25, 57 }, { 90, 43 }, { 64, 18 }, { -83, -50 },
    { 64, 75 }, { -36, -89 }, { 70, -43 }, { -87, 9 }, { 90, 25 },
    { -80, -57 }, { 64, -18 }, { -83, 50 }, { 64, -75 }, { -36, 89 },
    { 57, -80 }, { -25, 90 }, { -9, -87 }, { 43, 70 }, { 64, -50 },
    { -36, 89 }, { -64, -18 }, { 83, -75 }, { 43, -90 }, { 57, 25 },
    { -87, 70 }, { 9, -80 }, { 64, -75 }, { 36, 18 }, { -64, 89 },
    { -83, 50 }, { 25, -70 }, { 90, -80 }, { 43, 9 }, { -57, 87 },
    { 64, -89 }, { 83, -75 }, { 64, -50 }, { 36, -18 }, { 9, -25 },
    { 43, -57 }, { 70, -80 }, { 87, -90 },
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

AI_ __m256i pair(const int16_t p[2])
{
    int32_t w;

    memcpy(&w, p, 4);
    return _mm256_set1_epi32(w);
}

/* n int16 (4, 8 or 16) zero-extended to a vector */
AI_ __m256i ld_s16(const int16_t *p, int n)
{
    if (n == 16)
        return _mm256_loadu_si256((const __m256i *)p);
    if (n == 8)
        return _mm256_zextsi128_si256(_mm_loadu_si128((const __m128i *)p));
    return _mm256_zextsi128_si256(_mm_loadl_epi64((const __m128i *)p));
}

AI_ void st_s16(int16_t *p, __m256i v, int n)
{
    if (n == 16)
        _mm256_storeu_si256((__m256i *)p, v);
    else if (n == 8)
        _mm_storeu_si128((__m128i *)p, _mm256_castsi256_si128(v));
    else
        _mm_storel_epi64((__m128i *)p, _mm256_castsi256_si128(v));
}

/* (lo + add) >> sh and (hi + add) >> sh, narrowed with saturation */
AI_ __m256i rnd_pack(__m256i lo, __m256i hi, __m256i add, __m128i sh)
{
    lo = _mm256_sra_epi32(_mm256_add_epi32(lo, add), sh);
    hi = _mm256_sra_epi32(_mm256_add_epi32(hi, add), sh);
    return _mm256_packs_epi32(lo, hi);
}

static void tr4(int16_t *dst, const int16_t *src)
{
    __m128i r0 = _mm_loadl_epi64((const __m128i *)src);
    __m128i r1 = _mm_loadl_epi64((const __m128i *)(src + 4));
    __m128i r2 = _mm_loadl_epi64((const __m128i *)(src + 8));
    __m128i r3 = _mm_loadl_epi64((const __m128i *)(src + 12));
    __m128i a = _mm_unpacklo_epi16(r0, r1), b = _mm_unpacklo_epi16(r2, r3);

    _mm_storeu_si128((__m128i *)dst, _mm_unpacklo_epi32(a, b));
    _mm_storeu_si128((__m128i *)(dst + 8), _mm_unpackhi_epi32(a, b));
}

AI_ void tr8_tile(int16_t *dst, const int16_t *src, int n)
{
    __m128i r[8], a[8], b[8];

    for (int i = 0; i < 8; i++)
        r[i] = _mm_loadu_si128((const __m128i *)(src + i * n));
    for (int i = 0; i < 4; i++) {
        a[2 * i] = _mm_unpacklo_epi16(r[2 * i], r[2 * i + 1]);
        a[2 * i + 1] = _mm_unpackhi_epi16(r[2 * i], r[2 * i + 1]);
    }
    for (int h = 0; h < 2; h++) {
        b[4 * h + 0] = _mm_unpacklo_epi32(a[4 * h], a[4 * h + 2]);
        b[4 * h + 1] = _mm_unpackhi_epi32(a[4 * h], a[4 * h + 2]);
        b[4 * h + 2] = _mm_unpacklo_epi32(a[4 * h + 1], a[4 * h + 3]);
        b[4 * h + 3] = _mm_unpackhi_epi32(a[4 * h + 1], a[4 * h + 3]);
    }
    for (int i = 0; i < 4; i++) {
        _mm_storeu_si128((__m128i *)(dst + (2 * i) * n),
                         _mm_unpacklo_epi64(b[i], b[4 + i]));
        _mm_storeu_si128((__m128i *)(dst + (2 * i + 1) * n),
                         _mm_unpackhi_epi64(b[i], b[4 + i]));
    }
}

/* n x n int16, both packed */
static void transpose(int16_t *dst, const int16_t *src, int n)
{
    if (n == 4) {
        tr4(dst, src);
        return;
    }
    for (int ty = 0; ty < n; ty += 8)
        for (int tx = 0; tx < n; tx += 8)
            tr8_tile(dst + tx * n + ty, src + ty * n + tx, n);
}

/* ---- the 1-D passes, over the rows of an n x n int16 block ----------
 *
 * `in` and `out` are row-major with stride n; a pass handles columns
 * [c0, c0 + w) with w = min(n, 16). Row k of the input is v[k]. */

/* forward DCT pass with the first butterfly level: even outputs dot the
 * sums, odd ones the differences. Only for inputs whose sums fit int16. */
AI_ void fwd_split(int16_t *out, const int16_t *in, const int N, int c0,
                   __m256i add, __m128i sh)
{
    const int w = N < 16 ? N : 16, rs = 32 / N, q = N / 4;
    __m256i t[16], o[16], tl[8], th[8], ol[8], oh[8];

    for (int k = 0; k < N / 2; k++) {
        __m256i a = ld_s16(in + k * N + c0, w);
        __m256i b = ld_s16(in + (N - 1 - k) * N + c0, w);

        t[k] = _mm256_add_epi16(a, b);
        o[k] = _mm256_sub_epi16(a, b);
    }
    for (int m = 0; m < q; m++) {
        tl[m] = _mm256_unpacklo_epi16(t[2 * m], t[2 * m + 1]);
        th[m] = _mm256_unpackhi_epi16(t[2 * m], t[2 * m + 1]);
        ol[m] = _mm256_unpacklo_epi16(o[2 * m], o[2 * m + 1]);
        oh[m] = _mm256_unpackhi_epi16(o[2 * m], o[2 * m + 1]);
    }
    for (int i = 0; i < N; i++) {
        const int16_t (*row)[2] = fwd_pairs + i * rs * 16;
        const __m256i *xl = i & 1 ? ol : tl, *xh = i & 1 ? oh : th;
        __m256i lo = _mm256_setzero_si256(), hi = _mm256_setzero_si256();

        for (int m = 0; m < q; m++) {
            __m256i c = pair(row[m]);

            lo = _mm256_add_epi32(lo, _mm256_madd_epi16(xl[m], c));
            if (N > 4)
                hi = _mm256_add_epi32(hi, _mm256_madd_epi16(xh[m], c));
        }
        st_s16(out + i * N + c0, rnd_pack(lo, hi, add, sh), w);
    }
}

/* out[i] = sum_k tab[i * rstride + k / 2][k & 1] * in[k], k < N */
AI_ void mat_direct(int16_t *out, const int16_t *in, const int N, int c0,
                    const int16_t (*tab)[2], int rstride, __m256i add,
                    __m128i sh)
{
    const int w = N < 16 ? N : 16;
    __m256i il[16], ih[16];

    for (int m = 0; m < N / 2; m++) {
        __m256i a = ld_s16(in + 2 * m * N + c0, w);
        __m256i b = ld_s16(in + (2 * m + 1) * N + c0, w);

        il[m] = _mm256_unpacklo_epi16(a, b);
        ih[m] = _mm256_unpackhi_epi16(a, b);
    }
    for (int i = 0; i < N; i++) {
        const int16_t (*row)[2] = tab + i * rstride;
        __m256i lo = _mm256_setzero_si256(), hi = _mm256_setzero_si256();

        for (int m = 0; m < N / 2; m++) {
            __m256i c = pair(row[m]);

            lo = _mm256_add_epi32(lo, _mm256_madd_epi16(il[m], c));
            if (N > 4)
                hi = _mm256_add_epi32(hi, _mm256_madd_epi16(ih[m], c));
        }
        st_s16(out + i * N + c0, rnd_pack(lo, hi, add, sh), w);
    }
}

/* inverse DCT pass, out[j] = E[j] + O[j] and out[N-1-j] = E[j] - O[j].
 * Input rows at or past kb are zero and are not read. */
AI_ void inv_split(int16_t *out, const int16_t *in, const int N, int c0,
                   const int16_t (*tab)[2], int kb, __m256i add, __m128i sh)
{
    const int w = N < 16 ? N : 16, q = N / 4;
    int me = (kb + 3) >> 2, mo = (kb + 2) >> 2;
    __m256i el[8], eh[8], ol[8], oh[8];

    if (me > q) me = q;
    if (mo > q) mo = q;
    for (int m = 0; m < me; m++) {
        __m256i a = ld_s16(in + 4 * m * N + c0, w);
        __m256i b = ld_s16(in + (4 * m + 2) * N + c0, w);

        el[m] = _mm256_unpacklo_epi16(a, b);
        eh[m] = _mm256_unpackhi_epi16(a, b);
    }
    for (int m = 0; m < mo; m++) {
        __m256i a = ld_s16(in + (4 * m + 1) * N + c0, w);
        __m256i b = ld_s16(in + (4 * m + 3) * N + c0, w);

        ol[m] = _mm256_unpacklo_epi16(a, b);
        oh[m] = _mm256_unpackhi_epi16(a, b);
    }
    for (int j = 0; j < N / 2; j++) {
        const int16_t (*row)[2] = tab + j * (N / 2);
        __m256i elo = _mm256_setzero_si256(), ehi = _mm256_setzero_si256();
        __m256i olo = _mm256_setzero_si256(), ohi = _mm256_setzero_si256();

        for (int m = 0; m < me; m++) {
            __m256i c = pair(row[m]);

            elo = _mm256_add_epi32(elo, _mm256_madd_epi16(el[m], c));
            if (N > 4)
                ehi = _mm256_add_epi32(ehi, _mm256_madd_epi16(eh[m], c));
        }
        for (int m = 0; m < mo; m++) {
            __m256i c = pair(row[q + m]);

            olo = _mm256_add_epi32(olo, _mm256_madd_epi16(ol[m], c));
            if (N > 4)
                ohi = _mm256_add_epi32(ohi, _mm256_madd_epi16(oh[m], c));
        }
        st_s16(out + j * N + c0,
               rnd_pack(_mm256_add_epi32(elo, olo), _mm256_add_epi32(ehi, ohi),
                        add, sh), w);
        st_s16(out + (N - 1 - j) * N + c0,
               rnd_pack(_mm256_sub_epi32(elo, olo), _mm256_sub_epi32(ehi, ohi),
                        add, sh), w);
    }
}

AI_ __m256i rnd_add(int shift)
{
    return _mm256_set1_epi32(1 << (shift - 1));
}

AI_ __m128i rnd_sh(int shift)
{
    return _mm_cvtsi32_si128(shift);
}

/* ---- forward -------------------------------------------------------- */

AI_ void fdct_n(int16_t *coef, const int16_t *resid, const int N)
{
    _Alignas(32) int16_t b[32 * 32], t[32 * 32];
    const int log2n = __builtin_ctz((unsigned)N), rs = 32 / N;
    const int s1 = log2n + BIT_DEPTH - 9, s2 = log2n + 6;
    __m256i a1 = rnd_add(s1), a2 = rnd_add(s2);
    __m128i h1 = rnd_sh(s1), h2 = rnd_sh(s2);

    for (int c0 = 0; c0 < N; c0 += 16)
        fwd_split(b, resid, N, c0, a1, h1);
    transpose(t, b, N);
    for (int c0 = 0; c0 < N; c0 += 16)
        mat_direct(b, t, N, c0, fwd_pairs, rs * 16, a2, h2);
    transpose(coef, b, N);
}

static __attribute__((noinline))
void fdct4_avx2(int16_t *coef, const int16_t *resid) { fdct_n(coef, resid, 4); }
static __attribute__((noinline))
void fdct8_avx2(int16_t *coef, const int16_t *resid) { fdct_n(coef, resid, 8); }
static __attribute__((noinline))
void fdct16_avx2(int16_t *coef, const int16_t *resid) { fdct_n(coef, resid, 16); }
static __attribute__((noinline))
void fdct32_avx2(int16_t *coef, const int16_t *resid) { fdct_n(coef, resid, 32); }

static __attribute__((noinline))
void fdst4_avx2(int16_t *coef, const int16_t *resid)
{
    _Alignas(32) int16_t b[16], t[16];
    const int s1 = 2 + BIT_DEPTH - 9, s2 = 2 + 6;

    mat_direct(b, resid, 4, 0, dst_fwd_pairs, 2, rnd_add(s1), rnd_sh(s1));
    tr4(t, b);
    mat_direct(b, t, 4, 0, dst_fwd_pairs, 2, rnd_add(s2), rnd_sh(s2));
    tr4(coef, b);
}

void n265_fdct_avx2(int16_t *coef, const int16_t *resid, int log2n,
                    int use_dst)
{
    if (use_dst) {
        fdst4_avx2(coef, resid);
        return;
    }
    switch (log2n) {
    case 2: fdct4_avx2(coef, resid); break;
    case 3: fdct8_avx2(coef, resid); break;
    case 4: fdct16_avx2(coef, resid); break;
    default: fdct32_avx2(coef, resid); break;
    }
}

/* ---- inverse -------------------------------------------------------- */

/* dst = clip(pred + r) over n x n, r packed with stride n */
#if BIT_DEPTH == 8
AI_ void add_pred(pixel *dst, int ds, const pixel *pred, int ps,
                  const int16_t *r, const int N)
{
    for (int y = 0; y < N; y++) {
        const pixel *p = pred + (ptrdiff_t)y * ps;
        pixel *d = dst + (ptrdiff_t)y * ds;
        const int16_t *ry = r + y * N;

        if (N == 32) {
            __m256i p0 = _mm256_cvtepu8_epi16(_mm_loadu_si128((const __m128i *)p));
            __m256i p1 = _mm256_cvtepu8_epi16(
                _mm_loadu_si128((const __m128i *)(p + 16)));
            __m256i v0 = _mm256_adds_epi16(ld_s16(ry, 16), p0);
            __m256i v1 = _mm256_adds_epi16(ld_s16(ry + 16, 16), p1);

            _mm256_storeu_si256((__m256i *)d, _mm256_permute4x64_epi64(
                _mm256_packus_epi16(v0, v1), 0xd8));
        } else if (N == 16) {
            __m256i v = _mm256_adds_epi16(ld_s16(ry, 16), _mm256_cvtepu8_epi16(
                _mm_loadu_si128((const __m128i *)p)));

            _mm_storeu_si128((__m128i *)d,
                             _mm_packus_epi16(_mm256_castsi256_si128(v),
                                              _mm256_extracti128_si256(v, 1)));
        } else if (N == 8) {
            __m128i v = _mm_adds_epi16(_mm_loadu_si128((const __m128i *)ry),
                _mm_cvtepu8_epi16(_mm_loadl_epi64((const __m128i *)p)));

            _mm_storel_epi64((__m128i *)d, _mm_packus_epi16(v, v));
        } else {
            int32_t pw;
            __m128i v;

            memcpy(&pw, p, 4);
            v = _mm_adds_epi16(_mm_loadl_epi64((const __m128i *)ry),
                               _mm_cvtepu8_epi16(_mm_cvtsi32_si128(pw)));
            pw = _mm_cvtsi128_si32(_mm_packus_epi16(v, v));
            memcpy(d, &pw, 4);
        }
    }
}
#else
AI_ void add_pred(pixel *dst, int ds, const pixel *pred, int ps,
                  const int16_t *r, const int N)
{
    const __m256i z = _mm256_setzero_si256();
    const __m256i mx = _mm256_set1_epi16(PIXEL_MAX);

    for (int y = 0; y < N; y++) {
        const pixel *p = pred + (ptrdiff_t)y * ps;
        pixel *d = dst + (ptrdiff_t)y * ds;
        const int16_t *ry = r + y * N;

        if (N >= 16) {
            for (int x = 0; x < N; x += 16) {
                __m256i v = _mm256_adds_epi16(ld_s16(ry + x, 16),
                    _mm256_loadu_si256((const __m256i *)(p + x)));

                _mm256_storeu_si256((__m256i *)(d + x),
                    _mm256_min_epi16(_mm256_max_epi16(v, z), mx));
            }
        } else if (N == 8) {
            __m128i v = _mm_adds_epi16(_mm_loadu_si128((const __m128i *)ry),
                                       _mm_loadu_si128((const __m128i *)p));

            _mm_storeu_si128((__m128i *)d,
                _mm_min_epi16(_mm_max_epi16(v, _mm256_castsi256_si128(z)),
                              _mm256_castsi256_si128(mx)));
        } else {
            __m128i v = _mm_adds_epi16(_mm_loadl_epi64((const __m128i *)ry),
                                       _mm_loadl_epi64((const __m128i *)p));

            _mm_storel_epi64((__m128i *)d,
                _mm_min_epi16(_mm_max_epi16(v, _mm256_castsi256_si128(z)),
                              _mm256_castsi256_si128(mx)));
        }
    }
}
#endif

AI_ void idct_n(pixel *dst, int ds, const int16_t *dq, const pixel *pred,
                int ps, const int N, const int16_t (*tab)[2], int nzw,
                int nzh)
{
    _Alignas(32) int16_t b[32 * 32], t[32 * 32];
    const int s2 = 20 - BIT_DEPTH;
    __m256i a1 = rnd_add(7), a2 = rnd_add(s2);
    __m128i h1 = rnd_sh(7), h2 = rnd_sh(s2);

    for (int c0 = 0; c0 < N; c0 += 16) {
        if (c0 < nzw) {
            inv_split(b, dq, N, c0, tab, nzh, a1, h1);
        } else {
            for (int y = 0; y < N; y++)
                st_s16(b + y * N + c0, _mm256_setzero_si256(), 16);
        }
    }
    transpose(t, b, N);
    for (int c0 = 0; c0 < N; c0 += 16)
        inv_split(b, t, N, c0, tab, nzw, a2, h2);
    transpose(t, b, N);
    add_pred(dst, ds, pred, ps, t, N);
}

static __attribute__((noinline))
void idct4_avx2(pixel *dst, int ds, const int16_t *dq, const pixel *pred,
                int ps, int nzw, int nzh)
{
    idct_n(dst, ds, dq, pred, ps, 4, inv4_pairs, nzw, nzh);
}

static __attribute__((noinline))
void idct8_avx2(pixel *dst, int ds, const int16_t *dq, const pixel *pred,
                int ps, int nzw, int nzh)
{
    idct_n(dst, ds, dq, pred, ps, 8, inv8_pairs, nzw, nzh);
}

static __attribute__((noinline))
void idct16_avx2(pixel *dst, int ds, const int16_t *dq, const pixel *pred,
                 int ps, int nzw, int nzh)
{
    idct_n(dst, ds, dq, pred, ps, 16, inv16_pairs, nzw, nzh);
}

static __attribute__((noinline))
void idct32_avx2(pixel *dst, int ds, const int16_t *dq, const pixel *pred,
                 int ps, int nzw, int nzh)
{
    idct_n(dst, ds, dq, pred, ps, 32, inv32_pairs, nzw, nzh);
}

static __attribute__((noinline))
void idst4_avx2(pixel *dst, int ds, const int16_t *dq, const pixel *pred,
                int ps)
{
    _Alignas(32) int16_t b[16], t[16];
    const int s2 = 20 - BIT_DEPTH;

    mat_direct(b, dq, 4, 0, dst_inv_pairs, 2, rnd_add(7), rnd_sh(7));
    tr4(t, b);
    mat_direct(b, t, 4, 0, dst_inv_pairs, 2, rnd_add(s2), rnd_sh(s2));
    tr4(t, b);
    add_pred(dst, ds, pred, ps, t, 4);
}

/* the C's DC-only constant, over the whole block */
static void idct_dc(pixel *dst, int ds, const pixel *pred, int ps, int n,
                    int dc)
{
    _Alignas(32) int16_t r[32 * 32];
    const int sh2 = 20 - BIT_DEPTH;
    int v = (64 * dc + 64) >> 7;
    __m256i rv;

    v = v < -32768 ? -32768 : v > 32767 ? 32767 : v;
    rv = _mm256_set1_epi16((int16_t)((64 * v + (1 << (sh2 - 1))) >> sh2));
    for (int i = 0; i < n * n; i += 16)
        _mm256_store_si256((__m256i *)(r + i), rv);
    switch (n) {
    case 4: add_pred(dst, ds, pred, ps, r, 4); break;
    case 8: add_pred(dst, ds, pred, ps, r, 8); break;
    case 16: add_pred(dst, ds, pred, ps, r, 16); break;
    default: add_pred(dst, ds, pred, ps, r, 32); break;
    }
}

void n265_idct_add_avx2(pixel *dst, int dst_stride, const int16_t *dq,
                        int log2n, const pixel *pred, int pred_stride,
                        int use_dst, int nzw, int nzh)
{
    int n = 1 << log2n;

    if (use_dst) {
        idst4_avx2(dst, dst_stride, dq, pred, pred_stride);
        return;
    }
    nzw = nzw < 1 ? 1 : nzw > n ? n : nzw;
    nzh = nzh < 1 ? 1 : nzh > n ? n : nzh;
    if (nzw == 1 && nzh == 1) {
        idct_dc(dst, dst_stride, pred, pred_stride, n, dq[0]);
        return;
    }
    switch (log2n) {
    case 2: idct4_avx2(dst, dst_stride, dq, pred, pred_stride, nzw, nzh); break;
    case 3: idct8_avx2(dst, dst_stride, dq, pred, pred_stride, nzw, nzh); break;
    case 4: idct16_avx2(dst, dst_stride, dq, pred, pred_stride, nzw, nzh); break;
    default: idct32_avx2(dst, dst_stride, dq, pred, pred_stride, nzw, nzh); break;
    }
}

/* ---- quant / dequant (N265_ASM_QUANT) --------------------------------
 *
 * Neither needs the C's int64 (the ranges are proved in
 * src/encoder/arm/transform_neon.c). The dequant splits its scale at the
 * shift, S = Sh * 2^bd + Sl, and (l * S + add) >> bd is
 * l * Sh + ((l * Sl + add) >> bd) exactly; Sh <= 9216 and Sl < 2^bd, so
 * both products are one madd of (l, 0) against (S*, 0). The quant's
 * |c| * q + add is under 9.04e8; |c| is read as u16 so -32768 gives
 * 32768, and the product is formed from mullo and mulhi_epu16. */

void n265_dequant_avx2(int16_t *dq, const int16_t *level, int log2n, int qp,
                       int *nzw, int *nzh)
{
    static const int level_scale[6] = { 40, 45, 51, 57, 64, 72 };
    const int n = 1 << log2n, nn = n * n;
    const int qpq = qp + QP_BD_OFFSET;
    const int bd = BIT_DEPTH + log2n - 5;
    int32_t s = (int32_t)(((int64_t)level_scale[qpq % 6] << (qpq / 6)) * 16);
    __m256i shi = _mm256_set1_epi32(s >> bd);
    __m256i slo = _mm256_set1_epi32(s & ((1 << bd) - 1));
    __m256i add = rnd_add(bd), z = _mm256_setzero_si256();
    __m128i sh = rnd_sh(bd);
    uint64_t cols = 0;
    int my = 0;

    for (int i = 0; i < nn; i += 16) {
        __m256i l = _mm256_loadu_si256((const __m256i *)(level + i));
        __m256i ll = _mm256_unpacklo_epi16(l, z), lh = _mm256_unpackhi_epi16(l, z);
        __m256i vl = _mm256_add_epi32(_mm256_madd_epi16(ll, shi),
            _mm256_sra_epi32(_mm256_add_epi32(_mm256_madd_epi16(ll, slo), add), sh));
        __m256i vh = _mm256_add_epi32(_mm256_madd_epi16(lh, shi),
            _mm256_sra_epi32(_mm256_add_epi32(_mm256_madd_epi16(lh, slo), add), sh));
        __m256i v = _mm256_packs_epi32(vl, vh);
        uint32_t nz = ~(uint32_t)_mm256_movemask_epi8(_mm256_cmpeq_epi16(v, z));

        _mm256_storeu_si256((__m256i *)(dq + i), v);
        if (!nz)
            continue;
        /* two mask bits per coefficient */
        if (n >= 16) {
            cols |= (uint64_t)nz << (2 * (i % n));
            my = i / n;
        } else {
            for (int r = 0; r < 16 / n; r++) {
                uint32_t m = (nz >> (2 * n * r)) & ((1u << (2 * n)) - 1);

                if (m) {
                    cols |= m;
                    my = i / n + r;
                }
            }
        }
    }
    if (nzw) *nzw = cols ? (63 - __builtin_clzll(cols)) / 2 + 1 : 1;
    if (nzh) *nzh = my + 1;
}

int n265_quant_avx2(int16_t *level, const int16_t *coef, int log2n, int qp,
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
    __m256i q = _mm256_set1_epi16((int16_t)quant_scale[qpq % 6]);
    __m256i addv = _mm256_set1_epi32(add), z = _mm256_setzero_si256();
    __m256i zeros = z;
    __m128i sh = _mm_cvtsi32_si128(qbits);

    for (int i = 0; i < nn; i += 16) {
        __m256i c = _mm256_loadu_si256((const __m256i *)(coef + i));
        __m256i a = _mm256_abs_epi16(c);
        __m256i pl = _mm256_mullo_epi16(a, q), ph = _mm256_mulhi_epu16(a, q);
        __m256i lo = _mm256_srl_epi32(
            _mm256_add_epi32(_mm256_unpacklo_epi16(pl, ph), addv), sh);
        __m256i hi = _mm256_srl_epi32(
            _mm256_add_epi32(_mm256_unpackhi_epi16(pl, ph), addv), sh);
        __m256i l = _mm256_packs_epi32(lo, hi);

        zeros = _mm256_sub_epi16(zeros, _mm256_cmpeq_epi16(l, z));
        _mm256_storeu_si256((__m256i *)(level + i), _mm256_sign_epi16(l, c));
    }
    zeros = _mm256_madd_epi16(zeros, _mm256_set1_epi16(1));
    __m128i s = _mm_add_epi32(_mm256_castsi256_si128(zeros),
                              _mm256_extracti128_si256(zeros, 1));
    s = _mm_add_epi32(s, _mm_shuffle_epi32(s, 0x4e));
    s = _mm_add_epi32(s, _mm_shuffle_epi32(s, 0xb1));
    return nn - _mm_cvtsi128_si32(s);
}
