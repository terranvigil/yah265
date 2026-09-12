/*
 * Transforms and quantization. The dequant + inverse transform pair is
 * normative (8.6.3/8.6.4) and must match a conformant decoder bit-exactly;
 * the forward pair only shapes what levels the encoder picks.
 * Copyright (c) 2026, the yah265 authors. GPL-2.0-or-later; see LICENSE.
 */

#include <string.h>

#include "common/cpu.h"
#include "encoder/arm/neon.h"   /* self-guarded; nothing off aarch64 */
#include "encoder/tables.h"
#include "encoder/transform.h"

/* DST-VII 4x4 for intra luma 4x4 TBs (8.6.4.2) */
static const int8_t dst4[4][4] = {
    { 29, 55, 74, 84 },
    { 74, 74,  0,-74 },
    { 84,-29,-74, 55 },
    { 55,-84, 74,-29 },
};

static inline int mat(int i, int j, int stride, int use_dst)
{
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

void n265_fdct_ref(int16_t *coef, const int16_t *resid, int log2n,
                   int use_dst)
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

void n265_dequant(int16_t *dq, const int16_t *level, int log2n, int qp,
                  int *nzw, int *nzh)
{
#if N265_NEON
    if (n265_asm_on(N265_ASM_QUANT)) {
        n265_dequant_neon(dq, level, log2n, qp, nzw, nzh);
        return;
    }
#endif
    n265_dequant_ref(dq, level, log2n, qp, nzw, nzh);
}

void n265_dequant_ref(int16_t *dq, const int16_t *level, int log2n, int qp,
                      int *nzw, int *nzh)
{
    /* 8.6.3 with flat scaling (m = 16). `qp` is QpY/QpC, the 8-bit-domain
     * index; the scale table is indexed by qP = Qp' = qp + QpBdOffset and
     * bdShift is BitDepth + Log2(nTbS) - 5. QP_BD_OFFSET is 0 at 8 bits,
     * so this is the arithmetic the 8-bit encoder has always run. */
    static const int level_scale[6] = { 40, 45, 51, 57, 64, 72 };
    int qpq = qp + QP_BD_OFFSET;
    int n = 1 << log2n;
    int bd_shift = BIT_DEPTH + log2n - 5;
    int64_t add = 1 << (bd_shift - 1);
    int64_t scale = (int64_t)level_scale[qpq % 6] << (qpq / 6);
    int mx = 0, my = 0;
    /* a zero level dequantises to add >> bd_shift, which is zero for every
     * size (bd_shift >= 5), so the skip below is the same arithmetic. The
     * bounding box it collects is what lets the inverse transform leave
     * the empty rows and columns alone. */
    for (int y = 0; y < n; y++)
        for (int x = 0; x < n; x++) {
            int i = y * n + x;
            if (!level[i]) {
                dq[i] = 0;
                continue;
            }
            int64_t v = ((level[i] * scale * 16) + add) >> bd_shift;
            dq[i] = (int16_t)(v < -32768 ? -32768 : v > 32767 ? 32767 : v);
            if (dq[i]) {
                if (x > mx) mx = x;
                if (y > my) my = y;
            }
        }
    if (nzw) *nzw = mx + 1;
    if (nzh) *nzh = my + 1;
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

void n265_idct_add_ref(pixel *dst, int dst_stride, const int16_t *dq,
                       int log2n, const pixel *pred, int pred_stride,
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
                (pixel)clip3(0, PIXEL_MAX, pred[y * pred_stride + x] + r);
        }
}

/* ---------------------------------------------------------------------
 * Partial-butterfly cores.
 *
 * The core matrices are built so that T_2N[2k][j] = T_N[k][j] and
 * T_2N[i][2N-1-j] = (-1)^i T_2N[i][j] (tables.c). Both identities are
 * exact on the integers, so an N-point transform splits into an
 * (N/2)-point transform of the sums and one dot product per odd output
 * against the differences, with no rounding anywhere in the split: the
 * regrouped sums are the same integers the full matrix multiply adds,
 * and the single rounding shift still lands on the finished sum. That
 * makes the forward identical to n265_fdct_ref and the inverse identical
 * to 8.6.4.2, which is what the checkasm test and the corpus assert.
 *
 * Multiplies per 1-D transform fall from N^2 to about N^2/3 (32-point:
 * 1024 -> 344). Accumulators stay int32: the widest sum any stage forms
 * is bounded by max|input| * sum_k|T[i][k]| <= 45 * N * max|input|, which
 * for the forward's 45900 and the inverse's 32768 second-stage clip is
 * under 2^27. UBSan in `make asan` is the standing check on that.
 * ------------------------------------------------------------------ */

/* One 1-D forward DCT of length n, src read with stride sstep, dst
 * written with stride dstep, rounded by `shift`. n is a compile-time
 * constant at every call site, so the level loop unrolls. */
static inline void fwd_dct_1d(int32_t *dst, int dstep, const int32_t *src,
                              int sstep, int n, int shift)
{
    int32_t e[32], t[16], o[16];
    int32_t add = (int32_t)1 << (shift - 1);

    for (int i = 0; i < n; i++)
        e[i] = src[i * sstep];
    for (int len = n, oscale = 1; len > 1; len >>= 1, oscale <<= 1) {
        int h = len >> 1, rs = 32 / len;
        for (int k = 0; k < h; k++) {
            o[k] = e[k] - e[len - 1 - k];
            t[k] = e[k] + e[len - 1 - k];
        }
        for (int i = 0; i < h; i++) {
            const int8_t *row = n265_tmatrix[(2 * i + 1) * rs];
            int32_t s = 0;
            for (int k = 0; k < h; k++)
                s += row[k] * o[k];
            dst[(2 * i + 1) * oscale * dstep] = (s + add) >> shift;
        }
        for (int k = 0; k < h; k++)
            e[k] = t[k];
    }
    dst[0] = (64 * e[0] + add) >> shift;
}

/* One 1-D inverse DCT of length n: out[j] = sum_k T[k][j] * in[k].
 * Built up from the 1-point transform: at each length the odd-indexed
 * coefficients contribute a vector O, and out = (E + O, reverse(E - O)).
 * The coefficient loop is the outer one so the matrix row is walked
 * contiguously. */
static inline void inv_dct_1d(int32_t *dst, int dstep, const int32_t *src,
                              int sstep, int n, int shift, int clip)
{
    int32_t e[32], o[16];
    int32_t add = (int32_t)1 << (shift - 1);

    e[0] = 64 * src[0];
    for (int len = 1; len < n; len <<= 1) {
        int step = (n / (2 * len)) * sstep, rs = 32 / (2 * len);
        for (int j = 0; j < len; j++)
            o[j] = 0;
        for (int i = 0; i < len; i++) {
            int32_t c = src[(2 * i + 1) * step];
            const int8_t *row = n265_tmatrix[(2 * i + 1) * rs];
            for (int j = 0; j < len; j++)
                o[j] += row[j] * c;
        }
        for (int j = len - 1; j >= 0; j--) {
            int32_t ev = e[j];
            e[j] = ev + o[j];
            e[2 * len - 1 - j] = ev - o[j];
        }
    }
    for (int j = 0; j < n; j++) {
        int32_t v = (e[j] + add) >> shift;
        dst[j * dstep] = clip ? clip3(-32768, 32767, v) : v;
    }
}

/* The same transform with a bound on the input: every src entry at index
 * >= nc is zero (nc < n). Two things follow. A level whose odd
 * coefficients all sit at or past nc contributes nothing and only mirrors
 * E; below that, the odd loop stops at the last index under nc, and a
 * zero coefficient inside the surviving range skips its row.
 *
 * This is a second function rather than a branch inside the one above
 * because the bound is a run-time value: the loops here do not unroll,
 * and folding them into the dense path cost that path 3x on 8x8 in the
 * checkasm bench. Both forms compute the same sums, so both reproduce
 * n265_idct_add_ref. */
static inline void inv_dct_1d_nz(int32_t *dst, int dstep, const int32_t *src,
                                 int sstep, int n, int shift, int clip,
                                 int nc)
{
    int32_t e[32], o[16];
    int32_t add = (int32_t)1 << (shift - 1);

    e[0] = 64 * src[0];
    for (int len = 1, lgstep = __builtin_ctz((unsigned)n) - 1; len < n;
         len <<= 1, lgstep--) {
        int stepi = 1 << lgstep;
        int step = stepi * sstep, rs = 32 / (2 * len);
        /* odd coefficients live at (2i+1)*stepi */
        int nodd = ((nc + stepi - 1) >> lgstep) >> 1;
        if (nodd > len)
            nodd = len;
        if (nodd <= 0) {
            for (int j = 0; j < len; j++)
                e[2 * len - 1 - j] = e[j];
            continue;
        }
        for (int j = 0; j < len; j++)
            o[j] = 0;
        for (int i = 0; i < nodd; i++) {
            int32_t c = src[(2 * i + 1) * step];
            if (!c)
                continue;
            const int8_t *row = n265_tmatrix[(2 * i + 1) * rs];
            for (int j = 0; j < len; j++)
                o[j] += row[j] * c;
        }
        for (int j = len - 1; j >= 0; j--) {
            int32_t ev = e[j];
            e[j] = ev + o[j];
            e[2 * len - 1 - j] = ev - o[j];
        }
    }
    for (int j = 0; j < n; j++) {
        int32_t v = (e[j] + add) >> shift;
        dst[j * dstep] = clip ? clip3(-32768, 32767, v) : v;
    }
}

/* DST-VII 4-point, forward (rows of dst4) and inverse (its columns),
 * factored to 8 multiplies instead of 16. */
static inline void fwd_dst4_1d(int32_t *dst, int dstep, const int32_t *src,
                               int sstep, int shift)
{
    int32_t add = (int32_t)1 << (shift - 1);
    int32_t x0 = src[0], x1 = src[sstep], x2 = src[2 * sstep],
            x3 = src[3 * sstep];
    int32_t c0 = x0 + x3, c1 = x1 + x3, c2 = x0 - x1, c3 = 74 * x2;

    dst[0]         = (29 * c0 + 55 * c1 + c3 + add) >> shift;
    dst[dstep]     = (74 * (x0 + x1 - x3) + add) >> shift;
    dst[2 * dstep] = (29 * c2 + 55 * c0 - c3 + add) >> shift;
    dst[3 * dstep] = (55 * c2 - 29 * c1 + c3 + add) >> shift;
}

static inline void inv_dst4_1d(int32_t *dst, int dstep, const int32_t *src,
                               int sstep, int shift, int clip)
{
    int32_t add = (int32_t)1 << (shift - 1);
    int32_t i0 = src[0], i1 = src[sstep], i2 = src[2 * sstep],
            i3 = src[3 * sstep];
    int32_t c0 = i0 + i2, c1 = i2 + i3, c2 = i0 - i3, c3 = 74 * i1;
    int32_t v[4];

    v[0] = (29 * c0 + 55 * c1 + c3 + add) >> shift;
    v[1] = (55 * c2 - 29 * c1 + c3 + add) >> shift;
    v[2] = (74 * (i0 - i2 + i3) + add) >> shift;
    v[3] = (29 * c2 + 55 * c0 - c3 + add) >> shift;
    for (int j = 0; j < 4; j++)
        dst[j * dstep] = clip ? clip3(-32768, 32767, v[j]) : v[j];
}

/* stage 1 over the columns, stage 2 over the rows of the result; `a`
 * carries the residual transposed so stage 1 reads a contiguous column
 * and stage 2 reads a contiguous stage-1 row. */
#define FDCT_N(N)                                                       \
    do {                                                                \
        for (int x = 0; x < (N); x++)                                   \
            fwd_dct_1d(b + x, N, a + x * (N), 1, N, shift1);            \
        for (int r = 0; r < (N); r++) {                                 \
            int32_t s[N];                                               \
            fwd_dct_1d(s, 1, b + r * (N), 1, N, shift2);                \
            for (int j = 0; j < (N); j++)                               \
                coef[r * (N) + j] = (int16_t)clip3(-32768, 32767, s[j]); \
        }                                                               \
    } while (0)

void n265_fdct(int16_t *coef, const int16_t *resid, int log2n, int use_dst)
{
    int n = 1 << log2n;
    int32_t a[32 * 32], b[32 * 32];
    int shift1 = log2n + BIT_DEPTH - 9;
    int shift2 = log2n + 6;

#if N265_NEON
    /* 16x16, 32x32 and the DST-VII 4x4. The NEON kernel is correct at
     * every size, but at 4x4 and 8x8 the DCT's two-stage layout costs more
     * in transposes than the vector butterfly saves against a C fast path
     * clang has already auto-vectorised. Round 6 rewrote both small sizes
     * register-resident, with no intermediate buffer between the stages at
     * all, and they still read 0.47x and 0.63x, so the size predicate
     * stays: the transposes, and not the buffer, are what the small DCT
     * cannot pay for. The DST-VII 4x4 is the shape that form does suit -
     * no mirror symmetry to exploit, five constants, the whole block in
     * four registers - and it went the other way: 10.3 ns of C against
     * 4.8, where the old NEON form was 14.9. checkasm --bench; see
     * docs/phase6-neon.md, round 6. Every path is bit-exact, so the
     * predicate moves time and nothing else. */
    if ((log2n >= 4 || use_dst) && n265_asm_on(N265_ASM_DCT)) {
        n265_fdct_neon(coef, resid, log2n, use_dst);
        return;
    }
#endif
    for (int y = 0; y < n; y++)
        for (int x = 0; x < n; x++)
            a[x * n + y] = resid[y * n + x];

    if (use_dst) {
        for (int x = 0; x < 4; x++)
            fwd_dst4_1d(b + x, 4, a + x * 4, 1, shift1);
        for (int r = 0; r < 4; r++) {
            int32_t s[4];
            fwd_dst4_1d(s, 1, b + r * 4, 1, shift2);
            for (int j = 0; j < 4; j++)
                coef[r * 4 + j] = (int16_t)clip3(-32768, 32767, s[j]);
        }
        return;
    }
    switch (log2n) {
    case 2: FDCT_N(4); break;
    case 3: FDCT_N(8); break;
    case 4: FDCT_N(16); break;
    default: FDCT_N(32); break;
    }
}

/* stage 1 transforms each coefficient column into b[x][y], stage 2 each
 * spatial row of that. b is written transposed for the same reason.
 *
 * nzw/nzh bound the coefficient block: every dq[y][x] with x >= nzw or
 * y >= nzh is zero, which the scan already knows from the last
 * significant position. A column past nzw transforms to zeros, so stage 1
 * never runs it and stage 2's own bound never reads it. Each stage picks
 * the dense kernel when its bound is the whole block, and at 4x4 and 8x8
 * always: the unrolled dense form is 67 ns at 8x8 against 138 ns for the
 * bounded one on a 4x4 corner, so below 16 the bound costs more than the
 * work it removes. The whole-block DC case is caught before either. */
#define IDCT_N(N)                                                       \
    do {                                                                \
        if (nzh >= (N) || (N) < 16)                                     \
            for (int x = 0; x < nzw; x++)                               \
                inv_dct_1d(b + x * (N), 1, a + x * (N), 1, N, 7, 1);     \
        else                                                            \
            for (int x = 0; x < nzw; x++)                               \
                inv_dct_1d_nz(b + x * (N), 1, a + x * (N), 1, N, 7, 1,  \
                              nzh);                                     \
        /* the small-block stage 2 below reads the whole column, so the \
         * rows stage 1 skipped have to be the zeros they stand for */  \
        if ((N) < 16 && nzw < (N))                                      \
            memset(b + nzw * (N), 0,                                    \
                   (size_t)((N) - nzw) * (N) * sizeof(int32_t));        \
        for (int y = 0; y < (N); y++) {                                 \
            int32_t r[N];                                               \
            if (nzw >= (N) || (N) < 16)                                 \
                inv_dct_1d(r, 1, b + y, N, N, 20 - BIT_DEPTH, 0);       \
            else                                                        \
                inv_dct_1d_nz(r, 1, b + y, N, N, 20 - BIT_DEPTH, 0,     \
                              nzw);                                     \
            for (int x = 0; x < (N); x++)                                \
                dst[y * dst_stride + x] = (pixel)                        \
                    clip3(0, PIXEL_MAX, pred[y * pred_stride + x] + r[x]); \
        }                                                               \
    } while (0)

void n265_idct_add(pixel *dst, int dst_stride, const int16_t *dq,
                   int log2n, const pixel *pred, int pred_stride,
                   int use_dst, int nzw, int nzh)
{
    int n = 1 << log2n;
    int32_t a[32 * 32], b[32 * 32];

#if N265_NEON
    /* Same predicate and the same reasons as n265_fdct above. The DCT at
     * 4x4 and 8x8 reads 0.52x and 0.49x register-resident and stays on C;
     * the DST-VII 4x4, which is the only shape use_dst ever names, is
     * 20.4 ns of C against 5.2 (the old NEON form was 14.3) and does
     * not. */
    if ((log2n >= 4 || use_dst) && n265_asm_on(N265_ASM_DCT)) {
        n265_idct_add_neon(dst, dst_stride, dq, log2n, pred, pred_stride,
                           use_dst, nzw, nzh);
        return;
    }
#endif
    /* the DST-VII core has no mirror symmetry to exploit and is four
     * points wide either way, so it runs the whole block */
    if (use_dst) {
        nzw = nzh = n;
    } else {
        if (nzw < 1) nzw = 1;
        if (nzh < 1) nzh = 1;
        if (nzw > n) nzw = n;
        if (nzh > n) nzh = n;
    }

    /* DC only: both stages reduce to a constant, and the constant is the
     * one the general path forms (E mirrors at every level, so stage 1
     * emits clip16((64*dq0 + 64) >> 7) in every row and stage 2 turns
     * that into (64*v + (1 << (19 - BitDepth))) >> (20 - BitDepth) in
     * every column - 2048 and 12 at 8 bits). */
    if (!use_dst && nzw == 1 && nzh == 1) {
        int sh2 = 20 - BIT_DEPTH;
        int32_t v = clip3(-32768, 32767, (64 * dq[0] + 64) >> 7);
        int32_t r = (64 * v + (1 << (sh2 - 1))) >> sh2;
        for (int y = 0; y < n; y++)
            for (int x = 0; x < n; x++)
                dst[y * dst_stride + x] = (pixel)
                    clip3(0, PIXEL_MAX, pred[y * pred_stride + x] + r);
        return;
    }

    /* a[x][k]: one coefficient column per row, only for the columns that
     * can carry anything */
    for (int x = 0; x < nzw; x++)
        for (int y = 0; y < n; y++)
            a[x * n + y] = dq[y * n + x];

    if (use_dst) {
        for (int x = 0; x < 4; x++)
            inv_dst4_1d(b + x * 4, 1, a + x * 4, 1, 7, 1);
        for (int y = 0; y < 4; y++) {
            int32_t r[4];
            inv_dst4_1d(r, 1, b + y, 4, 20 - BIT_DEPTH, 0);
            for (int x = 0; x < 4; x++)
                dst[y * dst_stride + x] = (pixel)
                    clip3(0, PIXEL_MAX, pred[y * pred_stride + x] + r[x]);
        }
        return;
    }
    switch (log2n) {
    case 2: IDCT_N(4); break;
    case 3: IDCT_N(8); break;
    case 4: IDCT_N(16); break;
    default: IDCT_N(32); break;
    }
}

int n265_quant(int16_t *level, const int16_t *coef, int log2n, int qp,
               int intra)
{
#if N265_NEON
    if (n265_asm_on(N265_ASM_QUANT))
        return n265_quant_neon(level, coef, log2n, qp, intra);
#endif
    return n265_quant_ref(level, coef, log2n, qp, intra);
}

int n265_quant_ref(int16_t *level, const int16_t *coef, int log2n, int qp,
                   int intra)
{
    static const int quant_scale[6] = {
        26214, 23302, 20560, 18396, 16384, 14564
    };
    int n = 1 << log2n;
    /* 14 + qP/6 + (15 - BitDepth - log2n); 21 at 8 bits. qP is Qp', the
     * 8-bit-domain `qp` plus QpBdOffset, so at Main10 the two moves
     * cancel: BIT_DEPTH costs 2 and QP_BD_OFFSET/6 returns 2, and a
     * coefficient of a given magnitude quantises to the same level it
     * would at 8 bits. That is the property the whole depth carries -
     * the forward DCT's shift1 already shed the two extra sample bits. */
    int qpq = qp + QP_BD_OFFSET;
    int qbits = 29 - BIT_DEPTH + qpq / 6 - log2n;
    int64_t add = ((int64_t)1 << qbits) / (intra ? 3 : 6);
    int q = quant_scale[qpq % 6];
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

/* 8.6.1: qPiCb = Clip3(-QpBdOffsetC, 57, QpY + offsets), QpCb = Table
 * 8-10 of that for 4:2:0, and the quantiser's index is Qp'Cb = QpCb +
 * QpBdOffsetC. We return QpC, the 8-BIT-DOMAIN value, and not Qp'Cb,
 * because the two callers want different things and only one of them
 * wants the offset: n265_quant/n265_dequant add QP_BD_OFFSET themselves
 * (as they do for luma), while the chroma deblocking filter indexes
 * Table 8-12 with QpC + 2 + offsets and no QpBdOffset at all
 * (8.7.2.5.5). Returning the offset index would have been silently
 * right for the quantiser and silently wrong for the filter.
 *
 * The mapping's identity leg extends straight down through zero - for
 * qPi < 30 QpC IS qPi - so every negative QpY maps to itself and the
 * clip's only job at 4:2:0 is the -QpBdOffsetC floor. pps/slice/CU
 * chroma offsets are all zero today (headers.c), so `qp_y` arrives as
 * qPi already. */
int n265_chroma_qp(int qp_y)
{
    /* Table 8-10 for 4:2:0, no offsets */
    static const int map[14] = {
        29, 30, 31, 32, 33, 33, 34, 34, 35, 35, 36, 36, 37, 37
    };
    int qpi = qp_y < -QP_BD_OFFSET ? -QP_BD_OFFSET : qp_y > 57 ? 57 : qp_y;
    if (qpi < 30)
        return qpi;
    if (qpi <= 43)
        return map[qpi - 30];
    return qpi - 6;
}
