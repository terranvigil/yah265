/*
 * Transforms and quantization. The dequant + inverse transform pair is
 * normative (8.6.3/8.6.4) and must match a conformant decoder bit-exactly;
 * the forward pair only shapes what levels the encoder picks.
 * Copyright (c) 2026, the yah265 authors. GPL-2.0-or-later; see LICENSE.
 */

#include <string.h>

#include "encoder/tables.h"
#include "encoder/transform.h"

#define BIT_DEPTH 8

/* DST-VII 4x4 for intra luma 4x4 TBs (8.6.4.2) */
static const int8_t dst4[4][4] = {
    { 29, 55, 74, 84 },
    { 74, 74,  0,-74 },
    { 84,-29,-74, 55 },
    { 55,-84, 74,-29 },
};

static inline int mat(int i, int j, int stride, int use_dst)
{
    extern int8_t n265_tmatrix[32][32];
    return use_dst ? dst4[i][j] : n265_tmatrix[i * stride][j];
}

static inline int clip3(int lo, int hi, int v)
{
    return v < lo ? lo : v > hi ? hi : v;
}

/* one forward stage: out[i][j] = sum_k T[i][k] * in[k][j], scaled */
static void fwd_stage(int32_t *out, const int32_t *in, int log2n, int shift,
                      int use_dst)
{
    int n = 1 << log2n;
    int stride = 32 >> log2n;   /* matrix row stride for this size */
    int add = 1 << (shift - 1);
    for (int i = 0; i < n; i++)
        for (int j = 0; j < n; j++) {
            int64_t s = 0;
            for (int k = 0; k < n; k++)
                s += (int64_t)mat(i, k, stride, use_dst) * in[k * n + j];
            out[i * n + j] = (int32_t)((s + add) >> shift);
        }
}

void n265_fdct(int16_t *coef, const int16_t *resid, int log2n, int use_dst)
{
    int n = 1 << log2n;
    int32_t a[32 * 32], b[32 * 32];
    int shift1 = log2n + BIT_DEPTH - 9;
    int shift2 = log2n + 6;

    for (int i = 0; i < n * n; i++)
        a[i] = resid[i];
    /* columns then rows: stage on the transpose twice */
    fwd_stage(b, a, log2n, shift1, use_dst);
    /* transpose b */
    for (int i = 0; i < n; i++)
        for (int j = 0; j < n; j++)
            a[j * n + i] = b[i * n + j];
    fwd_stage(b, a, log2n, shift2, use_dst);
    for (int i = 0; i < n; i++)
        for (int j = 0; j < n; j++)
            coef[i * n + j] = (int16_t)clip3(-32768, 32767, b[j * n + i]);
}

void n265_dequant(int16_t *dq, const int16_t *level, int log2n, int qp)
{
    /* 8.6.3 with flat scaling (m = 16) */
    static const int level_scale[6] = { 40, 45, 51, 57, 64, 72 };
    int n = 1 << log2n;
    int bd_shift = BIT_DEPTH + log2n - 5;
    int64_t add = 1 << (bd_shift - 1);
    int64_t scale = (int64_t)level_scale[qp % 6] << (qp / 6);
    for (int i = 0; i < n * n; i++) {
        int64_t v = ((level[i] * scale * 16) + add) >> bd_shift;
        dq[i] = (int16_t)(v < -32768 ? -32768 : v > 32767 ? 32767 : v);
    }
}

/* one inverse stage: out[j][i] = sum_k T[k][j] * in[k][i] (transposed
 * matrix), scaled and clipped to 16 bits after stage 1 */
static void inv_stage(int32_t *out, const int32_t *in, int log2n, int shift,
                      int clip, int use_dst)
{
    int n = 1 << log2n;
    int stride = 32 >> log2n;
    int add = 1 << (shift - 1);
    for (int j = 0; j < n; j++)
        for (int i = 0; i < n; i++) {
            int64_t s = 0;
            for (int k = 0; k < n; k++)
                s += (int64_t)mat(k, j, stride, use_dst) * in[k * n + i];
            int32_t v = (int32_t)((s + add) >> shift);
            out[j * n + i] = clip ? clip3(-32768, 32767, v) : v;
        }
}

void n265_idct_add(uint8_t *dst, int dst_stride, const int16_t *dq,
                   int log2n, const uint8_t *pred, int pred_stride,
                   int use_dst)
{
    int n = 1 << log2n;
    int32_t a[32 * 32], b[32 * 32];
    /* 8.6.4: stage 1 shift 7 (clip to 16 bits), stage 2 shift 20-BitDepth */
    for (int y = 0; y < n; y++)
        for (int x = 0; x < n; x++)
            a[y * n + x] = dq[y * n + x];
    /* stage 1 transforms columns: treat a as [row][col], sum over rows */
    inv_stage(b, a, log2n, 7, 1, use_dst);
    /* b is [col-transformed], second stage over the other dimension: we
     * transpose in and out so both stages use the same kernel */
    for (int i = 0; i < n; i++)
        for (int j = 0; j < n; j++)
            a[j * n + i] = b[i * n + j];
    inv_stage(b, a, log2n, 20 - BIT_DEPTH, 0, use_dst);
    for (int y = 0; y < n; y++)
        for (int x = 0; x < n; x++) {
            int r = b[x * n + y];
            dst[y * dst_stride + x] =
                (uint8_t)clip3(0, 255, pred[y * pred_stride + x] + r);
        }
}

int n265_quant(int16_t *level, const int16_t *coef, int log2n, int qp,
               int intra)
{
    static const int quant_scale[6] = {
        26214, 23302, 20560, 18396, 16384, 14564
    };
    int n = 1 << log2n;
    int qbits = 21 + qp / 6 - log2n;    /* 14 + qp/6 + (15 - bd - log2n) */
    int64_t add = ((int64_t)1 << qbits) / (intra ? 3 : 6);
    int q = quant_scale[qp % 6];
    int nz = 0;
    for (int i = 0; i < n * n; i++) {
        int c = coef[i];
        int sign = c < 0 ? -1 : 1;
        int l = (int)(((int64_t)(c < 0 ? -c : c) * q + add) >> qbits);
        if (l > 32767)
            l = 32767;
        level[i] = (int16_t)(sign * l);
        nz += l != 0;
    }
    return nz;
}

int n265_chroma_qp(int qp_y)
{
    /* Table 8-10 for 4:2:0, no offsets */
    static const int map[14] = {
        29, 30, 31, 32, 33, 33, 34, 34, 35, 35, 36, 36, 37, 37
    };
    if (qp_y < 30)
        return qp_y;
    if (qp_y <= 43)
        return map[qp_y - 30];
    return qp_y - 6;
}
