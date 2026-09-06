/*
 * Rate-distortion optimised quantisation (RDOQ) for one transform block.
 *
 * The quantiser picks, per coefficient, the level that minimises
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
 * quantiser.
 *
 * Copyright (c) 2026, the yah265 authors. GPL-2.0-or-later; see LICENSE.
 */
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include "encoder/rdoq.h"
#include "encoder/transform.h"
#include "encoder/tables.h"
#include "encoder/est.h"

/* -log2(p) tables in Q8 for the two symbols of every state, no adaptation */
static uint16_t cost_mps[64], cost_lps[64];
/* pixel SSD = coefficient SSD / parseval[log2n] (Q8 fixed point of 1/F) */
static double parseval[6];
static int inited;

static void probe_parseval(void)
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
    inited = 1;
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

/* bits (Q8) of coeff_abs_level_remaining = rem with Rice parameter k */
static uint32_t remaining_bits(int rem, int k)
{
    if ((rem >> k) < 4)
        return (uint32_t)((rem >> k) + 1 + k) << 8;
    int v = rem - (4 << k), b = k + 1, bits = 4;
    while (v >= (1 << b)) {
        bits++;
        v -= 1 << b;
        b++;
    }
    return (uint32_t)(bits + 1 + b) << 8;
}

/* bits (Q8) of the last_sig_coeff position (prefix contexts + suffix) */
static uint32_t last_pos_bits(const cabac_ctx_t *ctx, int last_x, int last_y,
                              int log2n, int c_idx)
{
    int cmax = (log2n << 1) - 1;
    int ctx_off, ctx_shift;
    if (c_idx == 0) {
        ctx_off = 3 * (log2n - 2) + ((log2n - 1) >> 2);
        ctx_shift = (log2n + 1) >> 2;
    } else {
        ctx_off = 15;
        ctx_shift = log2n - 2;
    }
    uint32_t bits = 0;
    int g[2] = { group_idx(last_x), group_idx(last_y) };
    int base[2] = { CTX_LAST_X, CTX_LAST_Y };
    for (int d = 0; d < 2; d++) {
        for (int i = 0; i < g[d]; i++)
            bits += bin_cost(&ctx[base[d] + ctx_off + (i >> ctx_shift)], 1);
        if (g[d] < cmax)
            bits += bin_cost(&ctx[base[d] + ctx_off + (g[d] >> ctx_shift)], 0);
        if (g[d] > 3)
            bits += (uint32_t)((g[d] >> 1) - 1) << 8;
    }
    return bits;
}

/* level_scale of 8.6.3 */
static const int level_scale[6] = { 40, 45, 51, 57, 64, 72 };
static const int quant_scale[6] = { 26214, 23302, 20560, 18396, 16384, 14564 };

int n265_rdoq(int16_t *level, const int16_t *coef, int log2n, int qp,
              int intra, int c_idx, int scan_idx, const cabac_ctx_t *ctx,
              uint32_t lam_q8)
{
    n265_rdoq_init();
    int n = 1 << log2n;
    int nn = n * n;
    int qbits = 21 + qp / 6 - log2n;
    int64_t add = ((int64_t)1 << qbits) / (intra ? 3 : 6);
    int q = quant_scale[qp % 6];
    /* dequant of level l: (l * scale * 16 + dadd) >> bd_shift */
    int bd_shift = 8 + log2n - 5;
    int64_t dadd = 1 << (bd_shift - 1);
    int64_t dscale = (int64_t)level_scale[qp % 6] << (qp / 6);
    /* lambda in coefficient-SSD units: J = SSD_coef / F + lam * bits, so
     * scale lambda by F and keep everything in coefficient SSD */
    double lam = (double)lam_q8 / 256.0 * parseval[log2n];

    const uint8_t *sb_scan = n265_scan_sb[scan_idx][log2n - 2];
    const uint8_t *co_scan = n265_scan_coef[scan_idx];
    int sb_dim = n >> 2, num_sb = sb_dim * sb_dim;

    /* 1. round-to-nearest levels, the candidates' upper bound; find the
     * block's last significant position */
    int16_t rtn[32 * 32];
    int any = 0;
    for (int i = 0; i < nn; i++) {
        int c = coef[i] < 0 ? -coef[i] : coef[i];
        int l = (int)(((int64_t)c * q + add) >> qbits);
        if (l > 32767) l = 32767;
        rtn[i] = (int16_t)l;
        any |= l != 0;
    }
    memset(level, 0, (size_t)nn * sizeof(int16_t));
    if (!any)
        return 0;

    /* per-position bookkeeping in scan order within each sub-block */
    double d0[32 * 32];          /* distortion of coding zero */
    double dsel[32 * 32];        /* distortion of the chosen level */
    double rsel[32 * 32];        /* rate (bits, real) of the chosen level incl. sig flag */
    double rsig0[32 * 32];       /* rate of a sig_coeff_flag = 0 at this position */
    int lsel[32 * 32];           /* chosen |level| */
    int csbf[64] = { 0 };
    double sb_cost[64], sb_zero[64];
    int last_s = -1, last_c = -1;
    for (int s = num_sb - 1; s >= 0 && last_s < 0; s--) {
        int sx = (sb_scan[s] & 15) << 2, sy = (sb_scan[s] >> 4) << 2;
        for (int c = 15; c >= 0; c--) {
            int x = sx + (co_scan[c] & 15), y = sy + (co_scan[c] >> 4);
            if (rtn[y * n + x]) { last_s = s; last_c = c; break; }
        }
    }

    int prev_sb_g1 = 0;
    for (int s = last_s; s >= 0; s--) {
        int sx = (sb_scan[s] & 15) << 2, sy = (sb_scan[s] >> 4) << 2;
        int sb_x = sb_scan[s] & 15, sb_y = sb_scan[s] >> 4;
        int csbf_right = 0, csbf_below = 0;
        for (int t = 0; t < num_sb; t++) {
            if ((sb_scan[t] & 15) == sb_x + 1 && (sb_scan[t] >> 4) == sb_y) csbf_right = csbf[t];
            if ((sb_scan[t] & 15) == sb_x && (sb_scan[t] >> 4) == sb_y + 1) csbf_below = csbf[t];
        }
        int prev_csbf = csbf_right | (csbf_below << 1);
        int ctx_set = ((s > 0 && c_idx == 0) ? 2 : 0) + (prev_sb_g1 ? 1 : 0);
        int g1_ctx = 1, n_g1 = 0, g2_done = 0, rice = 0, this_g1 = 0;
        double cost = 0, zero = 0;
        int start_c = (s == last_s) ? last_c : 15;
        for (int c = start_c; c >= 0; c--) {
            int x = sx + (co_scan[c] & 15), y = sy + (co_scan[c] >> 4);
            int i = y * n + x;
            int ac = coef[i] < 0 ? -coef[i] : coef[i];
            int lmax = rtn[i];
            /* distortion of level l in coefficient units */
            double dz = (double)ac * ac;
            d0[i] = dz;
            int sig_coded = !(s == last_s && c == last_c);   /* the last is inferred */
            /* the 4x4 map has no entry for (3,3): that position is only ever
             * the last coefficient, whose flag is inferred */
            int sig_ctx_ok = !(log2n == 2 && (x & 3) == 3 && (y & 3) == 3);
            double r_sig1 = 0.0, r_sig0 = 0.0;
            if (sig_coded && sig_ctx_ok) {
                int sctx = sig_ctx(x, y, log2n, c_idx, scan_idx, prev_csbf);
                r_sig1 = bin_cost(&ctx[CTX_SIG + sctx], 1) / 256.0;
                r_sig0 = bin_cost(&ctx[CTX_SIG + sctx], 0) / 256.0;
            }
            rsig0[i] = sig_coded ? r_sig0 : 0.0;
            double best = dz + lam * rsig0[i];
            int bestl = 0;
            double bestd = dz, bestr = rsig0[i];
            for (int l = lmax; l >= 1 && l >= lmax - 1; l--) {
                int64_t dq = (l * dscale * 16 + dadd) >> bd_shift;
                double e = (double)ac - (double)dq;
                double d = e * e;
                /* rate: sig (unless inferred), greater1 (first 8), greater2
                 * (first g1), sign (1 bypass), remaining */
                double r = (sig_coded ? r_sig1 : 0.0) + 1.0;
                int base = 1;
                if (n_g1 < 8) {
                    int inc = ctx_set * 4 + (g1_ctx < 3 ? g1_ctx : 3) + (c_idx ? 16 : 0);
                    r += bin_cost(&ctx[CTX_G1 + inc], l > 1) / 256.0;
                    base += l > 1;
                    if (l > 1 && !g2_done) {
                        r += bin_cost(&ctx[CTX_G2 + ctx_set + (c_idx ? 4 : 0)], l > 2) / 256.0;
                        base += l > 2;
                    }
                }
                int cap = 1 + (n_g1 < 8 ? 1 : 0) + ((n_g1 < 8 && l > 1 && !g2_done) ? 1 : 0);
                if (n_g1 >= 8 || base == cap)
                    r += remaining_bits(l - base, rice) / 256.0;
                double j = d + lam * r;
                if (j < best) { best = j; bestl = l; bestd = d; bestr = r; }
            }
            lsel[i] = bestl;
            dsel[i] = bestd;
            rsel[i] = bestr;
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
        }
        sb_cost[s] = cost;
        sb_zero[s] = zero;
        prev_sb_g1 = this_g1;
    }

    /* 2. the last position: walk the significant coefficients from the
     * chosen last backwards; making position p the last drops everything
     * after it (their distortion becomes d0, their rate vanishes) and
     * changes the last-position code. Accumulate the cost of the tail. */
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
        for (int c = start_c; c >= 0; c--) {
            int x = sx + (co_scan[c] & 15), y = sy + (co_scan[c] >> 4);
            int i = y * n + x;
            if (lsel[i]) {
                int lx = x, ly = y;
                if (scan_idx == 2) { int t = lx; lx = ly; ly = t; }
                double lastbits = last_pos_bits(ctx, lx, ly, log2n, c_idx) / 256.0;
                /* as the last, this coefficient's own sig flag is inferred */
                /* as the last, this coefficient's own sig flag is no longer
                 * coded: take its sig=1 cost back out of the body */
                double own_sig = 0.0;
                if (!(log2n == 2 && (x & 3) == 3 && (y & 3) == 3) && !(s == last_s && c == last_c))
                    own_sig = bin_cost(&ctx[CTX_SIG + sig_ctx(x, y, log2n, c_idx, scan_idx, 0)], 1) / 256.0;
                double total = total_body - tail_saved + tail_d + lam * lastbits - lam * own_sig;
                if (total < best_total) { best_total = total; best_s = s; best_c = c; }
            }
            /* this position joins the tail for the candidates before it */
            tail_saved += lsel[i] ? dsel[i] + lam * rsel[i] : d0[i] + lam * rsig0[i];
            tail_d += d0[i];
        }
    }
    /* 4. all zero: nothing coded at all */
    double all_zero = 0;
    for (int i = 0; i < nn; i++) all_zero += (double)coef[i] * coef[i];
    if (best_s < 0 || all_zero <= best_total)
        return 0;

    /* write the levels: everything after the chosen last is zero */
    int nz = 0, past = 1;
    for (int s = last_s; s >= 0; s--) {
        int sx = (sb_scan[s] & 15) << 2, sy = (sb_scan[s] >> 4) << 2;
        int start_c = (s == last_s) ? last_c : 15;
        for (int c = start_c; c >= 0; c--) {
            int x = sx + (co_scan[c] & 15), y = sy + (co_scan[c] >> 4);
            int i = y * n + x;
            if (s == best_s && c == best_c) past = 0;
            int l = past ? 0 : lsel[i];
            level[i] = (int16_t)(coef[i] < 0 ? -l : l);
            nz += l != 0;
        }
    }
    return nz;
}
