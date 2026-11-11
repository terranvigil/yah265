/*
 * Rate-distortion optimized quantization (RDOQ) for one transform block.
 *
 * The quantizer picks, per coefficient, the level that minimizes
 * distortion + lambda * rate instead of rounding to nearest. Distortion is
 * taken in the coefficient domain: the core transforms are orthogonal up
 * to a per-size constant, so the pixel-domain SSD of a coefficient error
 * is its square divided by that constant, which is measured once at start
 * by pushing a known block through the forward transform (no hand-derived
 * scaling to get wrong). Rate comes from the residual_coding syntax the
 * real coder emits (residual.c), priced with the entropy of the current
 * CABAC context states, read without adapting them inside the block.
 *
 * Decisions, in the decoder's own order (reverse scan within each 4x4
 * sub-block, sub-blocks in reverse scan):
 *   1. every coefficient: the cheapest of {round-to-nearest level, one
 *      below it, zero}, with the greater1/greater2/rice state the coder
 *      would be in;
 *   2. the last significant position: any earlier significant coefficient
 *      may become the last, dropping everything after it to zero, when the
 *      saved rate (the dropped levels plus the cheaper last-position code)
 *      beats the added distortion;
 *   3. each coded sub-block: all-zero when its coded_sub_block_flag=0 is
 *      cheaper than its coefficients;
 *   4. the whole block: all-zero when even the best arrangement costs more
 *      than the distortion of coding nothing.
 * Sign-bit hiding is applied afterwards by the caller, as after the plain
 * quantizer.
 *
 * The coefficient loop reads its context costs out of small tables built
 * once per sub-block rather than deriving them per coefficient, and
 * collapses the level-1 probe to constants. Every one of those is the same
 * arithmetic on the same values, so the levels do not move; the form each
 * replaced is kept under `-DN265_LAT_AUDIT` as an in-place
 * `__builtin_trap()` compare, so a build with that define aborts on the
 * first bit of disagreement. See docs/phase6-neon.md, round 5.
 *
 * Copyright (c) 2026, the yah265 authors. GPL-2.0-or-later; see LICENSE.
 */
#include <math.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "common/cpu.h"
#include "encoder/arm/neon.h"   /* self-guarded; nothing off aarch64 */
#include "encoder/x86/avx2.h"   /* self-guarded */
#include "encoder/x86/sse42.h"   /* self-guarded */
#include "encoder/rdoq.h"
#include "encoder/transform.h"
#include "encoder/tables.h"
#include "encoder/est.h"
#include "encoder/preset.h"

/* -log2(p) tables in Q8 for the two symbols of every state, no adaptation */
static uint16_t cost_mps[64], cost_lps[64];
/* pixel SSD = coefficient SSD / parseval[log2n] (Q8 fixed point of 1/F).
 *
 * It is MEASURED through n265_fdct rather than derived, which is what
 * makes it right at Main10 for free: the forward transform's shift1 is
 * log2n + BitDepth - 9, so at 10 bits a residual four times as large
 * produces the same coefficients, the measured ratio comes out 16 times
 * smaller, and a coefficient error therefore converts to the 16-times
 * larger pixel SSD that 10-bit samples really carry. The mode lambda
 * handed in is 16 times larger too (n265_lambda_mode_q8 indexes by Qp'),
 * so `lam` below - the product - is unchanged, and the lattice makes the
 * same decisions on an upshifted clip that it made on the original. */
static double parseval[6];
static int inited;
/* the position half of the sig_coeff_flag ctxInc, [scan][prev_csbf][c];
 * see sig_ctx() and the lattice's per-sub-block table */
static uint8_t sig_base_k[3][4][16];
static uint8_t sig_dc_c[3];     /* the scan step that is (0, 0) */
/* a scan step's offset from the corner of its sub-block, [scan][log2n-2][c] */
static uint8_t scan_off[3][4][16];

/* out of line: inlined, its 4 KB of arrays sat in n265_rdoq_init's frame,
 * which every lattice call enters, and the stack probe ran each time
 * (phase 14 item 39) */
static __attribute__((noinline)) void probe_parseval(void)
{
    /* A fixed pseudo-random residual block per size; the ratio of squared
     * coefficient energy to squared residual energy is the transform's
     * scale, and it is the same for every block up to rounding. */
    for (int log2n = 2; log2n <= 5; log2n++) {
        int n = 1 << log2n;
        int16_t resid[32 * 32], coef[32 * 32];
        uint32_t s = 0x9e3779b9u ^ (uint32_t)log2n;
        double er = 0;
        for (int i = 0; i < n * n; i++) {
            s = s * 1664525u + 1013904223u;
            resid[i] = (int16_t)((int)(s >> 24) - 128);
            er += (double)resid[i] * resid[i];
        }
        n265_fdct(coef, resid, log2n, 0);
        double ec = 0;
        for (int i = 0; i < n * n; i++)
            ec += (double)coef[i] * coef[i];
        parseval[log2n] = ec / er;
    }
}

void n265_rdoq_init(void)
{
    if (inited)
        return;
    double alpha = pow(0.01875 / 0.5, 1.0 / 63.0);
    for (int st = 0; st < 64; st++) {
        double plps = 0.5 * pow(alpha, st);
        cost_lps[st] = (uint16_t)lround(-log2(plps) * 256.0);
        cost_mps[st] = (uint16_t)lround(-log2(1.0 - plps) * 256.0);
    }
    probe_parseval();
    /* The switch in sig_ctx() below reads only the position INSIDE the 4x4
     * sub-block and prev_csbf; everything it adds afterwards is constant
     * over a sub-block. Build the position half once - 3 scans x 4
     * prev_csbf x 16 steps, 192 bytes - so the lattice pays one byte load
     * per coefficient instead of the switch. n265_tables_init() owns the
     * scan this reads and is idempotent; the driver has already run it
     * (encoder.c) by the time any worker gets here. */
    n265_tables_init();
    for (int sc = 0; sc < 3; sc++) {
        const uint8_t *cs = n265_scan_coef[sc];
        for (int c = 0; c < 16; c++)
            if (cs[c] == 0)
                sig_dc_c[sc] = (uint8_t)c;
        for (int p = 0; p < 4; p++)
            for (int c = 0; c < 16; c++) {
                int xp = cs[c] & 15, yp = cs[c] >> 4, base;
                switch (p) {
                case 0: base = (xp + yp == 0) ? 2 : (xp + yp < 3) ? 1 : 0; break;
                case 1: base = (yp == 0) ? 2 : (yp == 1) ? 1 : 0; break;
                case 2: base = (xp == 0) ? 2 : (xp == 1) ? 1 : 0; break;
                default: base = 2; break;
                }
                sig_base_k[sc][p][c] = (uint8_t)base;
            }
        /* and the step's own offset inside the block, so the coefficient
         * loop adds a byte to its sub-block's corner instead of running
         * y * n + x per position (n is a runtime value, so that was a real
         * multiply). The largest entry is 3 * 32 + 3. */
        for (int lg = 2; lg <= 5; lg++)
            for (int c = 0; c < 16; c++)
                scan_off[sc][lg - 2][c] =
                    (uint8_t)(((cs[c] >> 4) << lg) + (cs[c] & 15));
    }
    inited = 1;
}

double n265_rdoq_parseval(int log2n)
{
    return parseval[log2n];
}

static inline uint32_t bin_cost(const cabac_ctx_t *ctx, int bin)
{
    return bin == ctx->mps ? cost_mps[ctx->state] : cost_lps[ctx->state];
}

/* the coder's context derivations, mirrored from residual.c */
static int group_idx(int v)
{
    if (v < 4)
        return v;
    int lz = 31 - __builtin_clz((unsigned)v);
    return (lz << 1) | ((v >> (lz - 1)) & 1);
}

static int sig_ctx(int xc, int yc, int log2n, int c_idx, int scan_idx,
                   int prev_csbf)
{
    if (log2n == 2) {
        int base = n265_sig_ctx_map4x4[(yc << 2) | xc];
        return c_idx ? 27 + base : base;
    }
    if (xc + yc == 0)
        return c_idx ? 27 : 0;
    int xp = xc & 3, yp = yc & 3;
    int base;
    switch (prev_csbf) {
    case 0: base = (xp + yp == 0) ? 2 : (xp + yp < 3) ? 1 : 0; break;
    case 1: base = (yp == 0) ? 2 : (yp == 1) ? 1 : 0; break;
    case 2: base = (xp == 0) ? 2 : (xp == 1) ? 1 : 0; break;
    default: base = 2; break;
    }
    if (c_idx == 0) {
        if ((xc >> 2) + (yc >> 2) > 0)
            base += 3;
        base += (log2n == 3) ? (scan_idx == 0 ? 9 : 15) : 21;
        return base;
    }
    base += (log2n == 3) ? 9 : 12;
    return 27 + base;
}

/* bits of coeff_abs_level_remaining = rem with Rice parameter k.
 *
 * This counted whole bits and returned them in Q8, and every caller then
 * divided by 256.0. `(uint32_t)v << 8` for the v this counts (under 64) is
 * exact and `/ 256.0` takes it straight back, so the Q8 round trip was the
 * identity on a small integer: the count is returned as the double it
 * always became. */
static inline double remaining_bits(int rem, int k)
{
    if ((rem >> k) < 4)
        return (double)((rem >> k) + 1 + k);
    int v = rem - (4 << k), b = k + 1, bits = 4;
    while (v >= (1 << b)) {
        bits++;
        v -= 1 << b;
        b++;
    }
    return (double)(bits + 1 + b);
}

/* bits (Q8) of the last_sig_coeff position (prefix contexts + suffix).
 * The x and y halves are independent sums over disjoint context families,
 * so the cost of a position is one table lookup per axis. The last-
 * position pass below asks for it once per significant coefficient, where
 * building the two tables costs 2n evaluations for the whole block. */
static void last_pos_tables(const cabac_ctx_t *ctx, int log2n, int c_idx,
                            uint32_t *bx, uint32_t *by)
{
    int n = 1 << log2n;
    int cmax = (log2n << 1) - 1;
    int ctx_off, ctx_shift;
    if (c_idx == 0) {
        ctx_off = 3 * (log2n - 2) + ((log2n - 1) >> 2);
        ctx_shift = (log2n + 1) >> 2;
    } else {
        ctx_off = 15;
        ctx_shift = log2n - 2;
    }
    int base[2] = { CTX_LAST_X, CTX_LAST_Y };
    uint32_t *out[2] = { bx, by };
    for (int d = 0; d < 2; d++) {
        /* A position's cost depends on v only through group_idx(v), and
         * the prefix part of group g is the same sum group g+1 pays plus
         * one more bin - a running total, not a fresh inner loop. Build
         * the cmax+1 group costs once and read one per position: O(n +
         * cmax) bin_cost calls for what used to be O(n * cmax), same
         * arithmetic in the same order within each sum. cmax is 2*log2n-1,
         * so 9 at 32x32. */
        uint32_t tab[10];
        uint32_t run = 0;       /* sum of the '1' prefix bins below g */

        for (int g = 0; g <= cmax; g++) {
            uint32_t bits = run;
            if (g < cmax) {
                const cabac_ctx_t *c =
                    &ctx[base[d] + ctx_off + (g >> ctx_shift)];
                bits += bin_cost(c, 0);
                run += bin_cost(c, 1);
            }
            if (g > 3)
                bits += (uint32_t)((g >> 1) - 1) << 8;
            tab[g] = bits;
        }
        for (int v = 0; v < n; v++)
            out[d][v] = tab[group_idx(v)];
    }
}

/* Sign-bit hiding decided by RD instead of by rule (N265_SBH_RD).
 *
 * SBH costs nothing when the sub-block's abs-level sum already has the
 * parity that encodes the first significant coefficient's sign, and costs
 * one changed level when it does not. n265_sbh_adjust always grows the
 * highest-frequency nonzero, which is a rule, not a decision: it spends
 * bits and distortion at whichever position happens to sit last in the
 * scan. Here every position in the sub-block offers its cheapest
 * already-priced level of the opposite parity (RDOQ evaluated 0, l-1, l
 * and l+1 at that position anyway, so the J of each is in hand) and the
 * cheapest one wins. Changes that would move the sub-block's first or
 * last significant position are excluded: those change the SBH
 * qualification itself, and the sign being hidden.
 *
 * 0 is the rule form. 1 spends the fix on a coefficient the block already
 * codes; 2 also lets it land on a position the block quantized to zero,
 * which is where the cheapest fix usually is - a coefficient sitting just
 * under its own rounding boundary costs almost nothing to promote, where
 * every already-coded level is at least a full quantizer step from its
 * neighbor. Measured on the CIF band 2026-09-08: 1 is -2.8% median
 * BD-VMAF-NEG on the rule form, 2 another -2.3%, both 7 of 7 clips. 2 is
 * the default. */
int n265_rdoq_sbh_on(void)
{
    static int v = -1;
    if (v < 0) { const char *s = n265_env("N265_SBH_RD"); v = s ? atoi(s) : 2; }
    return v;
}

/* Cached knobs at file scope so n265_rdoq_warm() can initialize them on
 * the driver before any worker exists (T0 audit item 2). */
static int rdoq_up_probe(void)
{
    static int v = -1;
    if (v < 0) { const char *s = n265_env("N265_RDOQ_UP"); v = s ? atoi(s) : 1; }
    return v;
}
static int rdoq_adapt(void)
{
    static int v = -1;
    if (v < 0) { const char *s = n265_env("N265_RDOQ_ADAPT"); v = s ? atoi(s) : 0; }
    return v;
}
static int rdoq_lam_pct(void)
{
    static int v = -1;
    if (v < 0) { const char *s = n265_env("N265_RDOQ_LAM"); v = s ? atoi(s) : 100; }
    return v;
}
static int rdoq_decimate(void)
{
    static int v = -1;
    if (v < 0) { const char *s = n265_env("N265_DECIMATE"); v = s ? atoi(s) : 15; }
    return v;
}

/* N265_DECIMATE_GRAD: the all-zero margin on a gradient-shaped block, in
 * percent, where every other block keeps N265_DECIMATE. Unset is the
 * sentinel below and means "the same margin everywhere", i.e. main byte
 * for byte. See rdoq_grad_shape() for what counts as gradient-shaped and
 * docs/phase7-psy-design.md M2 for why the two want different margins. */
#define DECGRAD_OFF 1000000
static int rdoq_decimate_grad(void)
{
    static int v, done;
    if (!done) {
        const char *s = n265_env("N265_DECIMATE_GRAD");
        v = s ? atoi(s) : DECGRAD_OFF;
        /* at -100 and below the threshold goes to zero or negative and
         * the test inverts instead of relaxing */
        if (v != DECGRAD_OFF && v < -99) v = -99;
        done = 1;
    }
    return v;
}

/* N265_DECGRAD_TRACE (diagnostic, output-neutral): the population census
 * the knob above acts on, printed once at exit. The counters are touched
 * only when it is set. Read it with N265_DECIMATE_GRAD UNSET: the
 * "by-margin" columns count what the margin in force did, so under an arm
 * they describe the arm and not the population it was chosen against. */
static atomic_ullong dg_n[2][6];
enum { DG_TB, DG_ZERO, DG_MARGIN, DG_GRAD, DG_GRAD_MARGIN, DG_GRAD_LAT };
static void dg_report(void)
{
    static const char *nm[6] = { "tb", "zeroed", "by-margin", "grad",
                                 "grad-by-margin", "grad-lattice-zero" };
    for (int p = 0; p < 2; p++) {
        fprintf(stderr, "decgrad %s:", p ? "chroma" : "luma");
        for (int k = 0; k < 6; k++)
            fprintf(stderr, " %s=%llu", nm[k],
                    (unsigned long long)atomic_load(&dg_n[p][k]));
        fprintf(stderr, "\n");
    }
}
static int rdoq_dg_trace(void)
{
    static int v = -1;
    if (v < 0) {
        v = n265_env("N265_DECGRAD_TRACE") != NULL;
        if (v) atexit(dg_report);
    }
    return v;
}
static inline void dg_bump(int p, int k)
{
    atomic_fetch_add_explicit(&dg_n[p][k], 1, memory_order_relaxed);
}

/* Is the arrangement RDOQ settled on gradient-shaped?
 *
 * Read off the levels alone, which is all the all-zero test has in hand:
 * every surviving level sits in the DC sub-block, on its first row or
 * first column, and at least one of them is not the DC. That is what a
 * smooth ramp leaves in the residual - a separable transform sends
 * `a*x + b*y` to (1,0) and (0,1) and nothing else - and it is the block
 * whose last coefficients the decimate margin turns into a band. A
 * DC-only block is excluded on purpose: dropping it shifts a flat patch,
 * which is blocking and not banding, and the margin is right about it. */
static int rdoq_grad_shape(const int *lsel, int n, int best_s, int best_c,
                           const uint8_t *co_scan)
{
    if (best_s != 0)            /* sub-block scan 0 is the DC sub-block */
        return 0;
    int ac = 0;
    for (int c = best_c; c >= 0; c--) {
        int x = co_scan[c] & 15, y = co_scan[c] >> 4;
        if (!lsel[y * n + x])
            continue;
        if (x && y)             /* off the first row and first column */
            return 0;
        ac += (x | y) != 0;     /* the DC alone is not a gradient */
    }
    return ac > 0;
}

/* level_scale of 8.6.3 */
static const int level_scale[6] = { 40, 45, 51, 57, 64, 72 };
static const int quant_scale[6] = { 26214, 23302, 20560, 18396, 16384, 14564 };

/* Pass 1 of the lattice: round-to-nearest levels, the block's coefficient
 * energy, and the 4x4 occupancy of the levels. A coefficient below `czero`
 * cannot reach level 1, which is most of them at any usable QP, so the
 * 64-bit multiply only runs where it can produce something; the vector
 * form drops that branch instead, which rdoq_neon.c proves is the same
 * arithmetic. The energy was accumulated in raster order out of caution
 * about the double adds; every partial sum is an exact integer below 2^53,
 * so the order never mattered and the kernel is free to reduce pairwise. */
int n265_rdoq_rtn_ref(int16_t *rtn, double *all_zero, uint64_t *sb_nz,
                      const int16_t *coef, int log2n, int q, int64_t add,
                      int qbits, int czero)
{
    int n = 1 << log2n, sb_dim = n >> 2;
    double az = 0;
    uint64_t m = 0;

    for (int y = 0; y < n; y++) {
        int sb_row = (y >> 2) * sb_dim;
        for (int x = 0; x < n; x++) {
            int i = y * n + x;
            int c = coef[i] < 0 ? -coef[i] : coef[i];
            az += (double)coef[i] * coef[i];
            if (c < czero) {
                rtn[i] = 0;
                continue;
            }
            int l = (int)(((int64_t)c * q + add) >> qbits);
            if (l > 32767) l = 32767;
            rtn[i] = (int16_t)l;
            m |= (uint64_t)1 << (sb_row + (x >> 2));
        }
    }
    *all_zero = az;
    *sb_nz = m;
    return m != 0;
}

int n265_rdoq_rtn(int16_t *rtn, double *all_zero, uint64_t *sb_nz,
                  const int16_t *coef, int log2n, int q, int64_t add,
                  int qbits, int czero)
{
#if N265_NEON_RDOQ
    if (n265_asm_on(N265_ASM_QUANT))
        return n265_rdoq_rtn_neon(rtn, all_zero, sb_nz, coef, log2n, q,
                                  add, qbits, czero);
#endif
#if N265_AVX2_RDOQ
    if (n265_avx2_on(N265_ASM_QUANT))
        return n265_rdoq_rtn_avx2(rtn, all_zero, sb_nz, coef, log2n, q,
                                  add, qbits, czero);
#endif
#if N265_SSE42_RDOQ
    if (n265_sse42_on(N265_ASM_QUANT))
        return n265_rdoq_rtn_sse42(rtn, all_zero, sb_nz, coef, log2n, q,
                                  add, qbits, czero);
#endif
    return n265_rdoq_rtn_ref(rtn, all_zero, sb_nz, coef, log2n, q, add,
                             qbits, czero);
}

/* Which 4x4 sub-blocks of a packed n x n block hold a nonzero. */
uint64_t n265_rdoq_sb_nz_ref(const int16_t *blk, int log2n)
{
    int n = 1 << log2n, sb_dim = n >> 2;
    uint64_t m = 0;

    for (int y = 0; y < n; y++) {
        int sb_row = (y >> 2) * sb_dim;
        for (int x = 0; x < n; x++)
            if (blk[y * n + x])
                m |= (uint64_t)1 << (sb_row + (x >> 2));
    }
    return m;
}

uint64_t n265_rdoq_sb_nz(const int16_t *blk, int log2n)
{
#if N265_NEON_RDOQ
    if (n265_asm_on(N265_ASM_QUANT))
        return n265_rdoq_sb_nz_neon(blk, log2n);
#endif
#if N265_AVX2_RDOQ
    if (n265_avx2_on(N265_ASM_QUANT))
        return n265_rdoq_sb_nz_avx2(blk, log2n);
#endif
#if N265_SSE42_RDOQ
    if (n265_sse42_on(N265_ASM_QUANT))
        return n265_rdoq_sb_nz_sse42(blk, log2n);
#endif
    return n265_rdoq_sb_nz_ref(blk, log2n);
}

/* N265_RDOQ_TAIL (phase 14 item 34): before the lattice runs, walk the
 * coded sub-blocks from the last one toward the first and drop each whose
 * every coefficient is at most one quantizer step UNROUNDED, stopping at
 * the first that is not. Sub-block 0 is never dropped. A level that is
 * only there because of the rounding offset is the one the last-position
 * pass mostly removes anyway, and the lattice then never visits the
 * stretch. 0 is main. */
static int rdoq_tail(void)
{
    static int v = -1;
    if (v < 0) { const char *s = n265_env("N265_RDOQ_TAIL"); v = s ? atoi(s) : 0; }
    return v;
}

/* N265_RDOQ_TAIL's drop on the rounded levels: returns the occupancy
 * left. Out of line, so the lattice's own frame doesn't carry it. */
static __attribute__((noinline)) uint64_t
tail_drop(int16_t *rtn, const int16_t *coef, uint64_t sb_nz, int log2n,
          const uint8_t *sb_scan, int q, int qbits)
{
    int n = 1 << log2n, sb_dim = n >> 2, num_sb = sb_dim * sb_dim;
    int64_t one = (int64_t)1 << qbits;
    uint64_t m = 0;
    for (int s = num_sb - 1; s >= 1; s--) {
        int sb_x = sb_scan[s] & 15, sb_y = sb_scan[s] >> 4;
        int bit = sb_y * sb_dim + sb_x;
        if (!((sb_nz >> bit) & 1))
            continue;
        const int16_t *p = coef + (sb_y << 2) * n + (sb_x << 2);
        for (int y = 0; y < 4; y++)
            for (int x = 0; x < 4; x++) {
                int c = p[y * n + x];
                if ((int64_t)(c < 0 ? -c : c) * q > one)
                    goto done;
            }
        m |= 1ull << bit;
        int16_t *r = rtn + (sb_y << 2) * n + (sb_x << 2);
        for (int y = 0; y < 4; y++)
            memset(r + y * n, 0, 4 * sizeof(int16_t));
    }
done:
    return sb_nz & ~m;
}

int n265_rdoq(int16_t *level, const int16_t *coef, int log2n, int qp,
              int intra, int c_idx, int scan_idx, const cabac_ctx_t *ctx,
              uint32_t lam_q8)
{
    n265_rdoq_init();
    /* the away-from-zero level probe: -2.7% median BD-VMAF-NEG, 7/7 clips
     * (2026-09-07). On by default; N265_RDOQ_UP=0 restores the 3-candidate set. */
    int up_probe = rdoq_up_probe();
    /* context adaptation along the scan INSIDE the block (N265_RDOQ_ADAPT):
     * the sig/greater1/greater2/coded_sub_block states move as the real
     * coder would move them, on a private copy, so a long run of levels is
     * priced at the probabilities it creates rather than at the block's
     * opening state. The three families are disjoint and each is visited in
     * its own coding order, so within a sub-block the adaptation is exact;
     * what stays approximate is the last-position and sub-block-zero passes,
     * which run after the states have already moved. Nothing leaves the
     * block, so the slice contexts and determinism are untouched. */
    int adapt = rdoq_adapt();
    int sbh_rd = n265_rdoq_sbh_on();
    cabac_ctx_t lctx[CTX_COUNT];
    const cabac_ctx_t *ctx0 = ctx;      /* the block's opening states */
    if (adapt) {
        n265_tables_init_est();
        memcpy(lctx, ctx, sizeof(lctx));
        ctx = lctx;
    }
    int n = 1 << log2n;
    int nn = n * n;
    /* The same pair transform.c runs, and it has to BE the same pair or
     * the lattice prices a quantizer the encoder does not use: qP is
     * Qp' = qp + QpBdOffset, and both shifts read BIT_DEPTH (29 - BD and
     * BD + log2n - 5). Both fold back to 21 and 8 + log2n - 5 at 8 bits. */
    int qpq = qp + QP_BD_OFFSET;
    int qbits = 29 - BIT_DEPTH + qpq / 6 - log2n;
    int64_t add = ((int64_t)1 << qbits) / (intra == 2 ? 2 : intra ? 3 : 6);
    int q = quant_scale[qpq % 6];
    /* dequant of level l: (l * scale * 16 + dadd) >> bd_shift */
    int bd_shift = BIT_DEPTH + log2n - 5;
    int64_t dadd = 1 << (bd_shift - 1);
    int64_t dscale = (int64_t)level_scale[qpq % 6] << (qpq / 6);
    /* (l * dscale) * 16 with the 16 folded in: dscale is at most
     * 72 << (Qp'max / 6) - 72 << 8 at 8 bits, 72 << 10 at Main10 - and l
     * at most 32767, so neither grouping reaches 2^36 and the two are the
     * same int64 */
    int64_t dq_k = dscale * 16;
    int64_t dq1 = (dq_k + dadd) >> bd_shift;    /* the dequant of level 1 */
    /* lambda in coefficient-SSD units: J = SSD_coef / F + lam * bits, so
     * scale lambda by F and keep everything in coefficient SSD */
    /* N265_RDOQ_LAM scales the coefficient-decision lambda away from the
     * mode lambda (percent, 100 = the same). Below 100 the lattice keeps
     * coefficients the mode decision would have priced away, which is the
     * one lever in this file that trades rate for retained texture
     * directly; the rate-matched band then reads whether the bits are
     * better spent there or on a lower QP. */
    int lam_pct = rdoq_lam_pct();
    double lam = (double)lam_q8 / 256.0 * parseval[log2n];
    if (lam_pct != 100)
        lam = lam * lam_pct / 100.0;

    const uint8_t *sb_scan = n265_scan_sb[scan_idx][log2n - 2];
    const uint8_t *co_scan = n265_scan_coef[scan_idx];
    const uint8_t *soff = scan_off[scan_idx][log2n - 2];
    int sb_dim = n >> 2, num_sb = sb_dim * sb_dim;
    /* scan position of the sub-block at (x, y): the right and below
     * neighbors' coded flags were found by walking the whole scan table
     * per sub-block, which is num_sb^2 steps for one lookup each */
    uint8_t sb_at[8][8];
    for (int t = 0; t < num_sb; t++)
        sb_at[sb_scan[t] >> 4][sb_scan[t] & 15] = (uint8_t)t;

    /* 1. round-to-nearest levels, the candidates' upper bound, the
     * all-zero energy the block-drop test is priced against, and the
     * sub-block occupancy the two scans below use to skip empty
     * sub-blocks whole. One pass over the block, and the only elementwise
     * pass in this file (n265_rdoq_rtn_ref, dispatched). */
    /* Every per-position array is sized to the block (phase 14 item 39
     * left them at 32x32): eleven of them made a 60 KB frame that the
     * stack probe walked page by page on every call, and four of five
     * calls are a 4x4 or an 8x8. */
    int16_t rtn[nn];
    double all_zero = 0;
    uint64_t sb_nz = 0;
    int czero = (int)((((int64_t)1 << qbits) - add + q - 1) / q);
    int any = n265_rdoq_rtn(rtn, &all_zero, &sb_nz, coef, log2n, q, add,
                            qbits, czero);
    if (any && rdoq_tail()) {
        sb_nz = tail_drop(rtn, coef, sb_nz, log2n, sb_scan, q, qbits);
        any = sb_nz != 0;
    }
    memset(level, 0, (size_t)nn * sizeof(int16_t));
    if (!any)
        return 0;

    /* per-position bookkeeping in scan order within each sub-block */
    double d0[nn];          /* distortion of coding zero */
    double dsel[nn];        /* distortion of the chosen level */
    double rsel[nn];        /* rate (bits, real) of the chosen level incl. sig flag */
    double rsig0[nn];       /* rate of a sig_coeff_flag = 0 at this position */
    double rsig1[nn];       /* rate of a sig_coeff_flag = 1 (adapt mode) */
    double alt_cost[nn];    /* J penalty of the cheapest opposite-parity level */
    int alt_lvl[nn];        /* that level (SBH-RD mode) */
    int lsel[nn];           /* chosen |level| */
    uint8_t keep[nn];       /* survived the last-position pass */
    int csbf[64] = { 0 };
    if (sbh_rd)
        memset(keep, 0, (size_t)nn);
    double sb_cost[64], sb_zero[64];
    int last_s = -1, last_c = -1;
    /* The occupancy word pass 1 built: an empty sub-block cannot hold the
     * last significant position, and on a 32x32 whose only levels are in
     * the DC corner that is 63 of the 64 sub-blocks, walked 16 positions
     * at a time before this bit test replaced them. */
    for (int s = num_sb - 1; s >= 0 && last_s < 0; s--) {
        int sb_x = sb_scan[s] & 15, sb_y = sb_scan[s] >> 4;
        if (!((sb_nz >> (sb_y * sb_dim + sb_x)) & 1))
            continue;
        int sx = sb_x << 2, sy = sb_y << 2;
        for (int c = 15; c >= 0; c--) {
            int x = sx + (co_scan[c] & 15), y = sy + (co_scan[c] >> 4);
            if (rtn[y * n + x]) { last_s = s; last_c = c; break; }
        }
    }

    int prev_sb_g1 = 0;
    for (int s = last_s; s >= 0; s--) {
        int sx = (sb_scan[s] & 15) << 2, sy = (sb_scan[s] >> 4) << 2;
        int sb_x = sb_scan[s] & 15, sb_y = sb_scan[s] >> 4;
        int csbf_right = sb_x + 1 < sb_dim ? csbf[sb_at[sb_y][sb_x + 1]] : 0;
        int csbf_below = sb_y + 1 < sb_dim ? csbf[sb_at[sb_y + 1][sb_x]] : 0;
        int prev_csbf = csbf_right | (csbf_below << 1);
        int ctx_set = ((s > 0 && c_idx == 0) ? 2 : 0) + (prev_sb_g1 ? 1 : 0);
        int g1_ctx = 1, n_g1 = 0, g2_done = 0, rice = 0, this_g1 = 0;
        double cost = 0, zero = 0;
        int start_c = (s == last_s) ? last_c : 15;
        /* Everything the per-coefficient rate reads out of a context is a
         * per-SUB-BLOCK constant, and the lattice was re-deriving it per
         * coefficient (and, for greater1/greater2, per candidate). Build
         * the three small tables once here.
         *
         *  - sig: for log2n > 2 the ctxInc is sig_base_k[scan][prev_csbf][c]
         *    plus an offset that is fixed over the sub-block, so three
         *    contexts cover every position in it; entry 3 is the block's DC
         *    position, whose ctxInc is a special case of its own. 4x4 keeps
         *    the sig_ctx() call: its map has nine values over sixteen
         *    positions, so a table would cost more lookups than it saves.
         *  - greater1: the context moves with g1_ctx (four values) and
         *    nothing else inside the sub-block.
         *  - greater2: one context for the whole sub-block.
         *
         * The entries are the same bin_cost(...) / 256.0 the coefficient
         * loop computed, on the same context, so the doubles are identical
         * bit for bit - this is a lookup moved, not an arithmetic changed.
         * Under N265_RDOQ_ADAPT the states move along the scan, so the two
         * entries a position actually reads are refreshed below; that costs
         * the same bin_cost calls the old code made. */
        double rs0[4], rs1[4], g1c[4][2], g2c[2];
        int sctxk[4];
        const uint8_t *sigk = NULL;
        int dc_k = -1;
        if (log2n > 2) {
            int sb_add = c_idx == 0
                ? ((sb_x + sb_y > 0) ? 3 : 0) +
                  (log2n == 3 ? (scan_idx == 0 ? 9 : 15) : 21)
                : 27 + (log2n == 3 ? 9 : 12);
            sigk = sig_base_k[scan_idx][prev_csbf];
            if (sb_x == 0 && sb_y == 0)
                dc_k = sig_dc_c[scan_idx];
            sctxk[0] = sb_add;
            sctxk[1] = sb_add + 1;
            sctxk[2] = sb_add + 2;
            /* entry 3, the block's DC position, exists in one sub-block of
             * the block and is not priced in the rest */
            sctxk[3] = c_idx ? 27 : 0;
            for (int k = 0; k < (dc_k >= 0 ? 4 : 3); k++) {
                rs1[k] = bin_cost(&ctx[CTX_SIG + sctxk[k]], 1) / 256.0;
                rs0[k] = bin_cost(&ctx[CTX_SIG + sctxk[k]], 0) / 256.0;
            }
        }
        int g2_inc = CTX_G2 + ctx_set + (c_idx ? 4 : 0);
        for (int k = 0; k < 4; k++) {
            int inc = ctx_set * 4 + k + (c_idx ? 16 : 0);
            g1c[k][0] = bin_cost(&ctx[CTX_G1 + inc], 0) / 256.0;
            g1c[k][1] = bin_cost(&ctx[CTX_G1 + inc], 1) / 256.0;
        }
        g2c[0] = bin_cost(&ctx[g2_inc], 0) / 256.0;
        g2c[1] = bin_cost(&ctx[g2_inc], 1) / 256.0;
        int sb_base = sy * n + sx, is_last_sb = s == last_s;
        /* A sub-block pass 1 left empty (phase 14 item 39). Every
         * position in it has lmax == 0, so the decision is level 0 at
         * every one, and the coder state the walk carries does not move.
         * What survives of the general loop is what pass 2 and the
         * sub-block test read: d0, rsig0, lsel and the two running sums,
         * formed by the same statements in the same order, so the doubles
         * are the same bits. What it drops is the sign-hiding pricing:
         * alt_cost and alt_lvl are read only in a sub-block that ends up
         * holding a level, and this one cannot (pass 2 only zeroes
         * levels, and the parity fix only runs where one is already
         * nonzero). Not under N265_RDOQ_ADAPT, whose context walk is per
         * position, and not at 4x4, whose only sub-block is the last. */
        if (!adapt && sigk && !is_last_sb &&
            !((sb_nz >> (sb_y * sb_dim + sb_x)) & 1)) {
            for (int c = 15; c >= 0; c--) {
                int i = sb_base + soff[c];
                int ac = coef[i] < 0 ? -coef[i] : coef[i];
                double dz = (double)ac * ac;
                double rs = rs0[c == dc_k ? 3 : sigk[c]];
                double best = dz + lam * rs;

                d0[i] = dz;
                rsig0[i] = rs;
                lsel[i] = 0;
                cost += best;
                zero += dz;
            }
            goto sb_whole;
        }
        for (int c = start_c; c >= 0; c--) {
            int i = sb_base + soff[c];
            int ac = coef[i] < 0 ? -coef[i] : coef[i];
            int lmax = rtn[i];
            /* distortion of level l in coefficient units */
            double dz = (double)ac * ac;
            d0[i] = dz;
            int sig_coded = !(is_last_sb && c == last_c);   /* the last is inferred */
            double r_sig1 = 0.0, r_sig0 = 0.0;
            int sctx = -1;
            if (sigk) {
                if (sig_coded) {
                    int k = c == dc_k ? 3 : sigk[c];
                    sctx = sctxk[k];
                    if (adapt) {
                        rs1[k] = bin_cost(&ctx[CTX_SIG + sctx], 1) / 256.0;
                        rs0[k] = bin_cost(&ctx[CTX_SIG + sctx], 0) / 256.0;
                    }
                    r_sig1 = rs1[k];
                    r_sig0 = rs0[k];
                }
            } else {
                /* 4x4: one sub-block at the block's corner, so the scan
                 * step is the position. The map has no entry for (3,3):
                 * that position is only ever the last coefficient, whose
                 * flag is inferred */
                int x = co_scan[c] & 15, y = co_scan[c] >> 4;
                int sig_ctx_ok = !(x == 3 && y == 3);
                if (sig_coded && sig_ctx_ok) {
                    sctx = sig_ctx(x, y, log2n, c_idx, scan_idx, prev_csbf);
                    r_sig1 = bin_cost(&ctx[CTX_SIG + sctx], 1) / 256.0;
                    r_sig0 = bin_cost(&ctx[CTX_SIG + sctx], 0) / 256.0;
                }
            }
            double rs = sig_coded ? r_sig0 : 0.0;
            rsig0[i] = rs;
            /* rsig1 is read by the last-position pass under N265_RDOQ_ADAPT
             * and nowhere else; off it, that pass reprices the flag itself */
            if (adapt)
                rsig1[i] = sig_coded ? r_sig1 : 0.0;
            double best = dz + lam * rs;
            int bestl = 0;
            double bestd = dz, bestr = rs;
            int hasg1 = n_g1 < 8, g1i = g1_ctx < 3 ? g1_ctx : 3;
            if (adapt && hasg1) {
                int inc = ctx_set * 4 + g1i + (c_idx ? 16 : 0);
                g1c[g1i][0] = bin_cost(&ctx[CTX_G1 + inc], 0) / 256.0;
                g1c[g1i][1] = bin_cost(&ctx[CTX_G1 + inc], 1) / 256.0;
                if (!g2_done) {
                    g2c[0] = bin_cost(&ctx[g2_inc], 0) / 256.0;
                    g2c[1] = bin_cost(&ctx[g2_inc], 1) / 256.0;
                }
            }
#ifdef N265_LAT_AUDIT
            /* the pre-restructuring form, recomputed and compared bit for
             * bit; scratch build only (-DN265_LAT_AUDIT) */
            {
                int x = sx + (co_scan[c] & 15), y = sy + (co_scan[c] >> 4);
                if (i != y * n + x)
                    __builtin_trap();
                int a_ok = !(log2n == 2 && (x & 3) == 3 && (y & 3) == 3);
                int a_sctx = -1;
                double a_r1 = 0.0, a_r0 = 0.0;
                if (sig_coded && a_ok) {
                    a_sctx = sig_ctx(x, y, log2n, c_idx, scan_idx, prev_csbf);
                    a_r1 = bin_cost(&ctx[CTX_SIG + a_sctx], 1) / 256.0;
                    a_r0 = bin_cost(&ctx[CTX_SIG + a_sctx], 0) / 256.0;
                }
                if (a_sctx != sctx || memcmp(&a_r1, &r_sig1, sizeof a_r1) ||
                    memcmp(&a_r0, &r_sig0, sizeof a_r0))
                    __builtin_trap();
                for (int k = 0; k < 4; k++) {
                    int inc = ctx_set * 4 + k + (c_idx ? 16 : 0);
                    double v0 = bin_cost(&ctx[CTX_G1 + inc], 0) / 256.0;
                    double v1 = bin_cost(&ctx[CTX_G1 + inc], 1) / 256.0;
                    if ((!adapt || (hasg1 && k == g1i)) &&
                        (memcmp(&v0, &g1c[k][0], sizeof v0) ||
                         memcmp(&v1, &g1c[k][1], sizeof v1)))
                        __builtin_trap();
                }
                double w0 = bin_cost(&ctx[CTX_G2 + ctx_set + (c_idx ? 4 : 0)], 0) / 256.0;
                double w1 = bin_cost(&ctx[CTX_G2 + ctx_set + (c_idx ? 4 : 0)], 1) / 256.0;
                if ((!adapt || (hasg1 && !g2_done)) &&
                    (memcmp(&w0, &g2c[0], sizeof w0) ||
                     memcmp(&w1, &g2c[1], sizeof w1)))
                    __builtin_trap();
            }
#endif
            /* N265_SBH_RD=2 also prices making a zero position significant,
             * so a parity fix can be spent on the coefficient nearest its
             * own rounding boundary rather than on one already coded. The
             * level is priced but never selected as the block's own. */
            if (sbh_rd >= 2 && lmax == 0) {
                /* The l = 1 probe on a position the quantizer rounded to
                 * zero, which is 77-85% of every position the lattice
                 * visits (foreman 84.4%, park_joy 76.6% at qp 30). At l = 1
                 * the whole candidate collapses to constants: the dequant
                 * is the block's dq1; greater1 is charged its bin-0 cost
                 * and greater2 is never reached, so base stays 1 while cap
                 * is 2 and the escape is not coded; past the eighth
                 * greater1 there is no greater1 bin either, base == cap == 1
                 * and the escape is remaining_bits(0, rice), which is
                 * 1 + rice bits. The terms are the same values added in the
                 * same order as the general loop below, so J is the same
                 * double. The candidate set is {0, 1} against bestl = 0, so
                 * the parity scan has one answer and does not need running,
                 * and dsel/rsel are the zero candidate's, which nothing
                 * reads at a position whose lsel is 0. */
                double e = (double)ac - (double)dq1;
                double d = e * e;
                double r = (sig_coded ? r_sig1 : 0.0) + 1.0;
                r += hasg1 ? g1c[g1i][0] : (double)(1 + rice);
                double j = d + lam * r;     /* one statement, as below: a
                                             * fused multiply-add is a
                                             * contraction the compiler is
                                             * allowed inside one and not
                                             * across two */
                lsel[i] = 0;
                alt_cost[i] = j - best;
                alt_lvl[i] = 1;
#ifdef N265_LAT_AUDIT
                {
                    /* the general loop at l = 1, run as it was written */
                    int64_t a_dq = (1 * dscale * 16 + dadd) >> bd_shift;
                    double a_e = (double)ac - (double)a_dq;
                    double a_d = a_e * a_e;
                    double a_r = (sig_coded ? r_sig1 : 0.0) + 1.0;
                    int a_base = 1;
                    if (n_g1 < 8) {
                        int inc = ctx_set * 4 + (g1_ctx < 3 ? g1_ctx : 3) + (c_idx ? 16 : 0);
                        a_r += bin_cost(&ctx[CTX_G1 + inc], 0) / 256.0;
                    }
                    int a_cap = 1 + (n_g1 < 8 ? 1 : 0);
                    if (n_g1 >= 8 || a_base == a_cap)
                        a_r += remaining_bits(0, rice);
                    double a_j = a_d + lam * a_r;
                    double a_alt = a_j - best;
                    if (memcmp(&a_d, &d, sizeof a_d) ||
                        memcmp(&a_r, &r, sizeof a_r) ||
                        memcmp(&a_alt, &alt_cost[i], sizeof a_alt))
                        __builtin_trap();
                }
#endif
            } else {
                int lhi = lmax, llo = lmax - 1;
                if (up_probe && lmax >= 1) lhi = lmax + 1;  /* away-from-zero probe */
                if (lhi > 32767) lhi = 32767;
                double jcand[4]; int lcand[4], ncand = 0;
                jcand[ncand] = best; lcand[ncand] = 0; ncand++;
                double r_s1 = sig_coded ? r_sig1 : 0.0;
                for (int l = lhi; l >= 1 && l >= llo; l--) {
                    int64_t dq = (l * dq_k + dadd) >> bd_shift;
#ifdef N265_LAT_AUDIT
                    if (dq != ((l * dscale * 16 + dadd) >> bd_shift))
                        __builtin_trap();
#endif
                    double e = (double)ac - (double)dq;
                    double d = e * e;
                    /* rate: sig (unless inferred), greater1 (first 8), greater2
                     * (first g1), sign (1 bypass), remaining */
                    double r = r_s1 + 1.0;
                    int base = 1;
                    if (hasg1) {
                        r += g1c[g1i][l > 1];
                        base += l > 1;
                        if (l > 1 && !g2_done) {
                            r += g2c[l > 2];
                            base += l > 2;
                        }
                    }
                    int cap = 1 + (hasg1 ? 1 : 0) + ((hasg1 && l > 1 && !g2_done) ? 1 : 0);
                    if (!hasg1 || base == cap)
                        r += remaining_bits(l - base, rice);
                    double j = d + lam * r;
                    jcand[ncand] = j; lcand[ncand] = l; ncand++;
                    if (j < best) { best = j; bestl = l; bestd = d; bestr = r; }
                }
                lsel[i] = bestl;
                dsel[i] = bestd;
                rsel[i] = bestr;
                if (sbh_rd) {
                    /* the cheapest already-priced level of the opposite parity:
                     * what a sign-bit-hiding parity fix at this position costs */
                    double ac2 = 1e300;
                    int al = -1;
                    for (int t = 0; t < ncand; t++)
                        if (((lcand[t] ^ bestl) & 1) && jcand[t] - best < ac2) {
                            ac2 = jcand[t] - best;
                            al = lcand[t];
                        }
                    alt_cost[i] = ac2;
                    alt_lvl[i] = al;
                }
            }
            if (adapt) {
                /* the bins this decision commits, on the local copy, in the
                 * coder's own order within each context family */
                if (sctx >= 0)
                    n265_est_bin(&lctx[CTX_SIG + sctx], bestl != 0);
                if (bestl && n_g1 < 8) {
                    int inc = ctx_set * 4 + (g1_ctx < 3 ? g1_ctx : 3) + (c_idx ? 16 : 0);
                    n265_est_bin(&lctx[CTX_G1 + inc], bestl > 1);
                    if (bestl > 1 && !g2_done)
                        n265_est_bin(&lctx[CTX_G2 + ctx_set + (c_idx ? 4 : 0)], bestl > 2);
                }
            }
            if (bestl) {
                /* advance the coder state the chosen level implies */
                if (n_g1 < 8) {
                    if (bestl > 1) {
                        this_g1 = 1;
                        if (!g2_done) g2_done = 1;
                        g1_ctx = 0;
                    } else if (g1_ctx > 0) {
                        g1_ctx++;
                    }
                    n_g1++;
                }
                if (bestl > (3 << rice) && rice < 4)
                    rice++;
                csbf[s] = 1;
            }
            cost += best;
            zero += dz;
        }
    sb_whole:
        /* 3. the sub-block as a whole: coded_sub_block_flag = 0 instead
         * (not for the last sub-block, whose flag is implied by the last
         * position, nor the DC sub-block, whose flag is inferred) */
        if (s < last_s && s > 0) {
            int inc = ((csbf_right || csbf_below) ? 1 : 0) + (c_idx ? 2 : 0);
            double r1 = bin_cost(&ctx[CTX_SIG_GROUP + inc], 1) / 256.0;
            double r0 = bin_cost(&ctx[CTX_SIG_GROUP + inc], 0) / 256.0;
            cost += lam * r1;
            if (zero + lam * r0 <= cost || !csbf[s]) {
                csbf[s] = 0;
                for (int c = 15; c >= 0; c--) {
                    int x = sx + (co_scan[c] & 15), y = sy + (co_scan[c] >> 4);
                    lsel[y * n + x] = 0;
                }
                cost = zero + lam * r0;
                this_g1 = 0;
            }
            if (adapt)
                n265_est_bin(&lctx[CTX_SIG_GROUP + inc], csbf[s]);
        }
        sb_cost[s] = cost;
        sb_zero[s] = zero;
        prev_sb_g1 = this_g1;
    }

    /* 2. the last position: walk the significant coefficients from the
     * chosen last backwards; making position p the last drops everything
     * after it (their distortion becomes d0, their rate vanishes) and
     * changes the last-position code. Accumulate the cost of the tail. */
    uint32_t lastb_x[32], lastb_y[32];
    last_pos_tables(ctx0, log2n, c_idx, lastb_x, lastb_y);
    double best_total = 1e300;
    int best_s = -1, best_c = -1;
    double tail_d = 0;          /* distortion of the dropped tail (coded zero) */
    double tail_saved = 0;      /* rate+distortion the tail cost when coded */
    double total_body = 0;
    for (int s = 0; s <= last_s; s++)
        total_body += sb_cost[s];
    for (int s = last_s; s >= 0; s--) {
        int sx = (sb_scan[s] & 15) << 2, sy = (sb_scan[s] >> 4) << 2;
        int start_c = (s == last_s) ? last_c : 15;
        int sb_base = sy * n + sx;
        for (int c = start_c; c >= 0; c--) {
            /* the position through the step-offset table, as the lattice
             * walks it; (x, y) only where a level is (item 39) */
            int i = sb_base + soff[c];
            if (lsel[i]) {
                int x = sx + (co_scan[c] & 15), y = sy + (co_scan[c] >> 4);
                int lx = x, ly = y;
                if (scan_idx == 2) { int t = lx; lx = ly; ly = t; }
                /* last_sig_coeff_x/y is coded before any of this block's
                 * other bins, so it is priced at the opening states */
                double lastbits = (lastb_x[lx] + lastb_y[ly]) / 256.0;
                /* as the last, this coefficient's own sig flag is inferred */
                /* as the last, this coefficient's own sig flag is no longer
                 * coded: take its sig=1 cost back out of the body */
                double own_sig = 0.0;
                if (adapt)
                    own_sig = rsig1[i];     /* the price it was actually charged */
                else if (!(log2n == 2 && (x & 3) == 3 && (y & 3) == 3) && !(s == last_s && c == last_c))
                    own_sig = bin_cost(&ctx[CTX_SIG + sig_ctx(x, y, log2n, c_idx, scan_idx, 0)], 1) / 256.0;
                double total = total_body - tail_saved + tail_d + lam * lastbits - lam * own_sig;
                if (total < best_total) { best_total = total; best_s = s; best_c = c; }
            }
            /* this position joins the tail for the candidates before it */
            tail_saved += lsel[i] ? dsel[i] + lam * rsel[i] : d0[i] + lam * rsig0[i];
            tail_d += d0[i];
        }
    }
    /* 4. all zero: nothing coded at all. N265_DECIMATE widens that test by
     * a percentage of the coded arrangement's cost, so a block the lattice
     * only just prefers to code gets dropped instead - the RD-domain form
     * of dct-decimate, and speed-positive since a zero block skips the
     * inverse transform and every downstream cbf.
     *
     * Swept on the CIF band 2026-09-08: 0 / 8 / 15 / 25 read -8.17 /
     * -7.87 / -8.97 / -8.82 median and -9.39 / -9.43 / -9.77 / -9.84 mean,
     * so the optimum is broad between 15 and 25 and 15 is the default -
     * best median and best worst clip, and the more conservative of the
     * two on content the band does not cover.
     *
     * The margin is right about a fine-scale block and wrong about a
     * gradient one, which is what the broad optimum above is averaging:
     * dropping a block's last low-frequency coefficients flattens a ramp
     * into a band, and a band is coarse structure the metric reads (M2,
     * docs/phase7-psy-design.md). N265_DECIMATE_GRAD gives the
     * gradient-shaped arrangement its own margin; unset, both are
     * N265_DECIMATE and nothing moves. */
    int decimate = rdoq_decimate();
    if (best_s < 0)
        return 0;
    int dgrad = rdoq_decimate_grad(), trace = rdoq_dg_trace();
    int grad = (dgrad != DECGRAD_OFF || trace)
             ? rdoq_grad_shape(lsel, n, best_s, best_c, co_scan) : 0;
    if (grad && dgrad != DECGRAD_OFF)
        decimate = dgrad;
    int zero = all_zero <= best_total * (1.0 + decimate / 100.0);
    if (trace) {
        int p = c_idx != 0;
        dg_bump(p, DG_TB);
        if (zero) dg_bump(p, DG_ZERO);
        if (zero && all_zero > best_total) dg_bump(p, DG_MARGIN);
        if (grad) {
            dg_bump(p, DG_GRAD);
            if (zero && all_zero > best_total) dg_bump(p, DG_GRAD_MARGIN);
            if (all_zero <= best_total) dg_bump(p, DG_GRAD_LAT);
        }
    }
    if (zero)
        return 0;

    /* write the levels. Everything after the chosen last is zero, and
     * `level` has been zero since the memset after pass 1 and `keep` since
     * its own; the walk therefore starts AT (best_s, best_c) instead of at
     * (last_s, last_c), and the stretch between them - which on a block
     * the last-position pass cuts hard is most of it - is skipped rather
     * than rewritten with the values it already holds. */
    int nz = 0;
    for (int s = best_s; s >= 0; s--) {
        int sx = (sb_scan[s] & 15) << 2, sy = (sb_scan[s] >> 4) << 2;
        int start_c = (s == best_s) ? best_c : 15;
        int sb_base = sy * n + sx;
        for (int c = start_c; c >= 0; c--) {
            int i = sb_base + soff[c];
            int l = lsel[i];
            level[i] = (int16_t)(coef[i] < 0 ? -l : l);
            keep[i] = 1;
            nz += l != 0;
        }
    }

    if (sbh_rd) {
        /* SBH only qualifies a sub-block that holds a level, so the
         * occupancy of the FINAL levels answers the whole first pass of
         * the walk below for every empty one - and after the last-position
         * pass most of them are empty. */
        uint64_t lv_nz = n265_rdoq_sb_nz(level, log2n);
        for (int s = 0; s < num_sb; s++) {
            int sb_x = sb_scan[s] & 15, sb_y = sb_scan[s] >> 4;
            if (!((lv_nz >> (sb_y * sb_dim + sb_x)) & 1))
                continue;
            int sx = sb_x << 2, sy = sb_y << 2;
            int first = -1, last = -1, sum = 0;
            for (int c = 0; c < 16; c++) {
                int x = sx + (co_scan[c] & 15), y = sy + (co_scan[c] >> 4);
                int l = level[y * n + x];
                if (l) {
                    if (first < 0) first = c;
                    last = c;
                    sum += l < 0 ? -l : l;
                }
            }
            if (first < 0 || last - first <= 3)
                continue;
            int fi = (sy + (co_scan[first] >> 4)) * n + sx + (co_scan[first] & 15);
            int want = level[fi] < 0;
            if ((sum & 1) == want)
                continue;
            double bestc = 1e300;
            int bi = -1;
            for (int c = 0; c < 16; c++) {
                int x = sx + (co_scan[c] & 15), y = sy + (co_scan[c] >> 4);
                int i = y * n + x;
                /* only positions whose decision survived the last-position
                 * pass carry a valid alternative */
                if (!keep[i] || alt_lvl[i] < 0)
                    continue;
                /* the change must not move first or last */
                if (lsel[i] == 0) {
                    if (sbh_rd < 2 || c <= first || c >= last)
                        continue;
                } else if (alt_lvl[i] == 0 && (c == first || c == last)) {
                    continue;
                }
                if (alt_cost[i] < bestc) { bestc = alt_cost[i]; bi = i; }
            }
            if (bi < 0) {
                /* nothing priced here: the rule form, grow the last */
                int lx = sx + (co_scan[last] & 15), ly = sy + (co_scan[last] >> 4);
                int i = ly * n + lx;
                level[i] += level[i] > 0 ? 1 : -1;
                continue;
            }
            int nl = alt_lvl[bi], had = level[bi] != 0;
            level[bi] = (int16_t)(coef[bi] < 0 ? -nl : nl);
            nz += (nl != 0) - had;
        }
    }
    return nz;
}

/* Initialize every cached knob and the lazy tables on the driver at
 * encoder open; see n265_ctu_warm. */
void n265_rdoq_warm(void)
{
    (void)n265_rdoq_sbh_on();
    (void)rdoq_up_probe();
    (void)rdoq_adapt();
    (void)rdoq_lam_pct();
    (void)rdoq_decimate();
    (void)rdoq_decimate_grad();
    (void)rdoq_dg_trace();
    (void)rdoq_tail();
    n265_rdoq_init();       /* the cost tables and the parseval probe */
}
