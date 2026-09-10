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
    for (int d = 0; d < 2; d++)
        for (int v = 0; v < n; v++) {
            int g = group_idx(v);
            uint32_t bits = 0;
            for (int i = 0; i < g; i++)
                bits += bin_cost(&ctx[base[d] + ctx_off + (i >> ctx_shift)], 1);
            if (g < cmax)
                bits += bin_cost(&ctx[base[d] + ctx_off + (g >> ctx_shift)], 0);
            if (g > 3)
                bits += (uint32_t)((g >> 1) - 1) << 8;
            out[d][v] = bits;
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
 * codes; 2 also lets it land on a position the block quantised to zero,
 * which is where the cheapest fix usually is - a coefficient sitting just
 * under its own rounding boundary costs almost nothing to promote, where
 * every already-coded level is at least a full quantiser step from its
 * neighbour. Measured on the CIF band 2026-09-08: 1 is -2.8% median
 * BD-VMAF-NEG on the rule form, 2 another -2.3%, both 7 of 7 clips. 2 is
 * the default. */
int n265_rdoq_sbh_on(void)
{
    static int v = -1;
    if (v < 0) { const char *s = getenv("N265_SBH_RD"); v = s ? atoi(s) : 2; }
    return v;
}

/* Cached knobs at file scope so n265_rdoq_warm() can initialize them on
 * the driver before any worker exists (T0 audit item 2). */
static int rdoq_up_probe(void)
{
    static int v = -1;
    if (v < 0) { const char *s = getenv("N265_RDOQ_UP"); v = s ? atoi(s) : 1; }
    return v;
}
static int rdoq_adapt(void)
{
    static int v = -1;
    if (v < 0) { const char *s = getenv("N265_RDOQ_ADAPT"); v = s ? atoi(s) : 0; }
    return v;
}
static int rdoq_lam_pct(void)
{
    static int v = -1;
    if (v < 0) { const char *s = getenv("N265_RDOQ_LAM"); v = s ? atoi(s) : 100; }
    return v;
}
static int rdoq_decimate(void)
{
    static int v = -1;
    if (v < 0) { const char *s = getenv("N265_DECIMATE"); v = s ? atoi(s) : 15; }
    return v;
}

/* level_scale of 8.6.3 */
static const int level_scale[6] = { 40, 45, 51, 57, 64, 72 };
static const int quant_scale[6] = { 26214, 23302, 20560, 18396, 16384, 14564 };

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
    int qbits = 21 + qp / 6 - log2n;
    int64_t add = ((int64_t)1 << qbits) / (intra ? 3 : 6);
    int q = quant_scale[qp % 6];
    /* dequant of level l: (l * scale * 16 + dadd) >> bd_shift */
    int bd_shift = 8 + log2n - 5;
    int64_t dadd = 1 << (bd_shift - 1);
    int64_t dscale = (int64_t)level_scale[qp % 6] << (qp / 6);
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
    int sb_dim = n >> 2, num_sb = sb_dim * sb_dim;
    /* scan position of the sub-block at (x, y): the right and below
     * neighbours' coded flags were found by walking the whole scan table
     * per sub-block, which is num_sb^2 steps for one lookup each */
    uint8_t sb_at[8][8];
    for (int t = 0; t < num_sb; t++)
        sb_at[sb_scan[t] >> 4][sb_scan[t] & 15] = (uint8_t)t;

    /* 1. round-to-nearest levels, the candidates' upper bound; find the
     * block's last significant position. A coefficient below `czero`
     * cannot reach level 1, which is most of them at any usable QP, so
     * the 64-bit multiply only runs where it can produce something. The
     * all-zero energy the block-drop test wants is accumulated here in
     * the same raster order it used to be summed in, so the double adds
     * land in the same order. */
    int16_t rtn[32 * 32];
    int any = 0;
    double all_zero = 0;
    int czero = (int)((((int64_t)1 << qbits) - add + q - 1) / q);
    for (int i = 0; i < nn; i++) {
        int c = coef[i] < 0 ? -coef[i] : coef[i];
        all_zero += (double)coef[i] * coef[i];
        if (c < czero) {
            rtn[i] = 0;
            continue;
        }
        int l = (int)(((int64_t)c * q + add) >> qbits);
        if (l > 32767) l = 32767;
        rtn[i] = (int16_t)l;
        any = 1;
    }
    memset(level, 0, (size_t)nn * sizeof(int16_t));
    if (!any)
        return 0;

    /* per-position bookkeeping in scan order within each sub-block */
    double d0[32 * 32];          /* distortion of coding zero */
    double dsel[32 * 32];        /* distortion of the chosen level */
    double rsel[32 * 32];        /* rate (bits, real) of the chosen level incl. sig flag */
    double rsig0[32 * 32];       /* rate of a sig_coeff_flag = 0 at this position */
    double rsig1[32 * 32];       /* rate of a sig_coeff_flag = 1 (adapt mode) */
    double alt_cost[32 * 32];    /* J penalty of the cheapest opposite-parity level */
    int alt_lvl[32 * 32];        /* that level (SBH-RD mode) */
    int lsel[32 * 32];           /* chosen |level| */
    uint8_t keep[32 * 32];       /* survived the last-position pass */
    int csbf[64] = { 0 };
    if (sbh_rd)
        memset(keep, 0, (size_t)nn);
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
        int csbf_right = sb_x + 1 < sb_dim ? csbf[sb_at[sb_y][sb_x + 1]] : 0;
        int csbf_below = sb_y + 1 < sb_dim ? csbf[sb_at[sb_y + 1][sb_x]] : 0;
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
            int sctx = -1;
            if (sig_coded && sig_ctx_ok) {
                sctx = sig_ctx(x, y, log2n, c_idx, scan_idx, prev_csbf);
                r_sig1 = bin_cost(&ctx[CTX_SIG + sctx], 1) / 256.0;
                r_sig0 = bin_cost(&ctx[CTX_SIG + sctx], 0) / 256.0;
            }
            rsig0[i] = sig_coded ? r_sig0 : 0.0;
            rsig1[i] = sig_coded ? r_sig1 : 0.0;
            double best = dz + lam * rsig0[i];
            int bestl = 0;
            double bestd = dz, bestr = rsig0[i];
            double jcand[4]; int lcand[4], ncand = 0;
            jcand[ncand] = best; lcand[ncand] = 0; ncand++;
            int lhi = lmax, llo = lmax - 1;
            if (up_probe && lmax >= 1) lhi = lmax + 1;   /* away-from-zero probe */
            if (lhi > 32767) lhi = 32767;
            /* N265_SBH_RD=2 also prices making a zero position significant,
             * so a parity fix can be spent on the coefficient nearest its
             * own rounding boundary rather than on one already coded. The
             * level is priced but never selected as the block's own. */
            int probe_only = 0;
            if (sbh_rd >= 2 && lmax == 0) { lhi = 1; llo = 1; probe_only = 1; }
            for (int l = lhi; l >= 1 && l >= llo; l--) {
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
                jcand[ncand] = j; lcand[ncand] = l; ncand++;
                if (!probe_only && j < best) { best = j; bestl = l; bestd = d; bestr = r; }
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
        for (int c = start_c; c >= 0; c--) {
            int x = sx + (co_scan[c] & 15), y = sy + (co_scan[c] >> 4);
            int i = y * n + x;
            if (lsel[i]) {
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
     * two on content the band does not cover. */
    int decimate = rdoq_decimate();
    if (best_s < 0 || all_zero <= best_total * (1.0 + decimate / 100.0))
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
            keep[i] = !past;
            nz += l != 0;
        }
    }

    if (sbh_rd) {
        for (int s = 0; s < num_sb; s++) {
            int sx = (sb_scan[s] & 15) << 2, sy = (sb_scan[s] >> 4) << 2;
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
    n265_rdoq_init();       /* the cost tables and the parseval probe */
}
