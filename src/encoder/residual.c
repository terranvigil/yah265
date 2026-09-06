/*
 * residual_coding() (ITU-T H.265 7.3.8.11) with the 9.3.4.2.4-9.3.4.2.7
 * context derivations and 9.3.3 binarizations.
 * Copyright (c) 2026, the yah265 authors. GPL-2.0-or-later; see LICENSE.
 */

#include <assert.h>

#include "encoder/residual.h"
#include "encoder/tables.h"

static void real_bin_cb(void *h, int ctx_idx, int bin)
{
    real_emit *re = h;
    n265_cabac_bin(re->cb, &re->ctx[ctx_idx], bin);
}

static void real_bypass_cb(void *h, int bin)
{
    n265_cabac_bypass(((real_emit *)h)->cb, bin);
}

static void real_bypass_bits_cb(void *h, uint32_t v, int n)
{
    n265_cabac_bypass_bits(((real_emit *)h)->cb, v, n);
}

void n265_real_ops(res_ops *ops, real_emit *re)
{
    ops->h = re;
    ops->bin = real_bin_cb;
    ops->bypass = real_bypass_cb;
    ops->bypass_bits = real_bypass_bits_cb;
}

/* last_sig_coeff group index: 0..3 direct, then log-spaced */
static int group_idx(int v)
{
    if (v < 4)
        return v;
    int lz = 31 - __builtin_clz((unsigned)v);
    return (lz << 1) | ((v >> (lz - 1)) & 1);
}

static const uint8_t min_in_group[10] = { 0, 1, 2, 3, 4, 6, 8, 12, 16, 24 };

static void write_last_prefix(const res_ops *ops, int ctx_base, int g,
                              int cmax, int ctx_off, int ctx_shift)
{
    for (int i = 0; i < g; i++)
        ops->bin(ops->h, ctx_base + ctx_off + (i >> ctx_shift), 1);
    if (g < cmax)
        ops->bin(ops->h, ctx_base + ctx_off + (g >> ctx_shift), 0);
}

/* sig_coeff_flag ctxInc (9.3.4.2.5); returns index into CTX_SIG..+41 */
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

/* coeff_abs_level_remaining: truncated-Rice prefix, EGk escape (ones-first
 * form), all bypass */
static void write_remaining(const res_ops *ops, int rem, int k)
{
    if ((rem >> k) < 4) {
        int q = rem >> k;
        for (int i = 0; i < q; i++)
            ops->bypass(ops->h, 1);
        ops->bypass(ops->h, 0);
        if (k)
            ops->bypass_bits(ops->h, (uint32_t)rem & ((1u << k) - 1), k);
    } else {
        for (int i = 0; i < 4; i++)
            ops->bypass(ops->h, 1);
        int v = rem - (4 << k);
        int b = k + 1;
        while (v >= (1 << b)) {
            ops->bypass(ops->h, 1);
            v -= 1 << b;
            b++;
        }
        ops->bypass(ops->h, 0);
        ops->bypass_bits(ops->h, (uint32_t)v, b);
    }
}

void n265_sbh_adjust(int16_t *level, int log2n, int scan_idx)
{
    int n = 1 << log2n;
    int sb_dim = n >> 2;
    int num_sb = sb_dim * sb_dim;
    const uint8_t *sb_scan = n265_scan_sb[scan_idx][log2n - 2];
    const uint8_t *co_scan = n265_scan_coef[scan_idx];

    for (int s = 0; s < num_sb; s++) {
        int sx = (sb_scan[s] & 15) << 2, sy = (sb_scan[s] >> 4) << 2;
        int first = -1, last = -1;
        int sum = 0;
        for (int c = 0; c < 16; c++) {
            int x = sx + (co_scan[c] & 15), y = sy + (co_scan[c] >> 4);
            int l = level[y * n + x];
            if (l) {
                if (first < 0)
                    first = c;
                last = c;
                sum += l < 0 ? -l : l;
            }
        }
        if (first < 0 || last - first <= 3)
            continue;
        int fx = sx + (co_scan[first] & 15), fy = sy + (co_scan[first] >> 4);
        int want = level[fy * n + fx] < 0;
        if ((sum & 1) != want) {
            /* grow the highest-frequency nonzero away from zero: keeps
             * first/last stable, flips the parity */
            int lx = sx + (co_scan[last] & 15), ly = sy + (co_scan[last] >> 4);
            level[ly * n + lx] += level[ly * n + lx] > 0 ? 1 : -1;
        }
    }
}

void n265_residual_coding(const res_ops *ops, const int16_t *level,
                          int log2n, int c_idx, int scan_idx)
{
    int n = 1 << log2n;
    int sb_dim = n >> 2;                    /* sub-blocks per side */
    int num_sb = sb_dim * sb_dim;
    const uint8_t *sb_scan = n265_scan_sb[scan_idx][log2n - 2];
    const uint8_t *co_scan = n265_scan_coef[scan_idx];

    /* per-sub-block significance map */
    uint8_t csbf[64] = { 0 };
    for (int s = 0; s < num_sb; s++) {
        int sx = (sb_scan[s] & 15) << 2, sy = (sb_scan[s] >> 4) << 2;
        for (int c = 0; c < 16; c++) {
            int x = sx + (co_scan[c] & 15), y = sy + (co_scan[c] >> 4);
            if (level[y * n + x])
                csbf[s] = 1;
        }
    }

    /* last significant position in scan order */
    int last_s = -1, last_c = -1;
    for (int s = num_sb - 1; s >= 0 && last_s < 0; s--) {
        if (!csbf[s])
            continue;
        for (int c = 15; c >= 0; c--) {
            int sx = (sb_scan[s] & 15) << 2, sy = (sb_scan[s] >> 4) << 2;
            int x = sx + (co_scan[c] & 15), y = sy + (co_scan[c] >> 4);
            if (level[y * n + x]) {
                last_s = s;
                last_c = c;
                break;
            }
        }
    }
    assert(last_s >= 0);

    int last_sx = (sb_scan[last_s] & 15) << 2;
    int last_sy = (sb_scan[last_s] >> 4) << 2;
    int last_x = last_sx + (co_scan[last_c] & 15);
    int last_y = last_sy + (co_scan[last_c] >> 4);
    if (scan_idx == 2) {  /* vertical: swap */
        int t = last_x; last_x = last_y; last_y = t;
    }

    /* last_sig_coeff_x/y prefix+suffix */
    int cmax = (log2n << 1) - 1;
    int ctx_off, ctx_shift;
    if (c_idx == 0) {
        ctx_off = 3 * (log2n - 2) + ((log2n - 1) >> 2);
        ctx_shift = (log2n + 1) >> 2;
    } else {
        ctx_off = 15;
        ctx_shift = log2n - 2;
    }
    int gx = group_idx(last_x), gy = group_idx(last_y);
    write_last_prefix(ops, CTX_LAST_X, gx, cmax, ctx_off, ctx_shift);
    write_last_prefix(ops, CTX_LAST_Y, gy, cmax, ctx_off, ctx_shift);
    if (gx > 3)
        ops->bypass_bits(ops->h, (uint32_t)(last_x - min_in_group[gx]),
                         (gx >> 1) - 1);
    if (gy > 3)
        ops->bypass_bits(ops->h, (uint32_t)(last_y - min_in_group[gy]),
                         (gy >> 1) - 1);

    int prev_sb_g1 = 0;     /* any greater1==1 in previously coded CSB */

    for (int s = last_s; s >= 0; s--) {
        int sx = (sb_scan[s] & 15) << 2, sy = (sb_scan[s] >> 4) << 2;
        int sb_x = sb_scan[s] & 15, sb_y = sb_scan[s] >> 4;

        /* neighbor csbf for contexts: right and below sub-blocks */
        int csbf_right = 0, csbf_below = 0;
        if (sb_x + 1 < sb_dim) {
            for (int t = 0; t < num_sb; t++)
                if ((sb_scan[t] & 15) == sb_x + 1 && (sb_scan[t] >> 4) == sb_y)
                    csbf_right = csbf[t];
        }
        if (sb_y + 1 < sb_dim) {
            for (int t = 0; t < num_sb; t++)
                if ((sb_scan[t] & 15) == sb_x && (sb_scan[t] >> 4) == sb_y + 1)
                    csbf_below = csbf[t];
        }
        int prev_csbf = csbf_right | (csbf_below << 1);

        int infer_sb_dc = 0;
        if (s < last_s && s > 0) {
            int inc = ((csbf_right || csbf_below) ? 1 : 0) + (c_idx ? 2 : 0);
            ops->bin(ops->h, CTX_SIG_GROUP + inc, csbf[s]);
            if (!csbf[s])
                continue;
            infer_sb_dc = 1;
        }
        /* s == 0: csbf is inferred 1 by the decoder, so its significance
         * flags are always coded, even when the whole sub-block is zero. */

        /* significance flags, reverse scan; last coeff inferred in its SB */
        int start_c = (s == last_s) ? last_c - 1 : 15;
        int sig_pos[16], num_sig = 0;
        if (s == last_s) {
            sig_pos[num_sig++] = last_c;
        }
        for (int c = start_c; c >= 0; c--) {
            int x = sx + (co_scan[c] & 15), y = sy + (co_scan[c] >> 4);
            int sig = level[y * n + x] != 0;
            if (c == 0 && infer_sb_dc && num_sig == 0) {
                /* would-be-empty coded sub-block: DC inferred significant */
                sig_pos[num_sig++] = 0;
                break;
            }
            int inc = sig_ctx(x, y, log2n, c_idx, scan_idx, prev_csbf);
            ops->bin(ops->h, CTX_SIG + inc, sig);
            if (sig)
                sig_pos[num_sig++] = c;
        }

        /* level coding: greater1 (first 8), greater2 (first g1), signs,
         * remaining */
        int ctx_set = ((s > 0 && c_idx == 0) ? 2 : 0) + (prev_sb_g1 ? 1 : 0);
        int g1_ctx = 1;
        int first_g1_idx = -1;
        int this_sb_g1 = 0;
        int n_g1 = num_sig < 8 ? num_sig : 8;
        for (int i = 0; i < n_g1; i++) {
            int c = sig_pos[i];
            int x = sx + (co_scan[c] & 15), y = sy + (co_scan[c] >> 4);
            int abs_l = level[y * n + x] < 0 ? -level[y * n + x]
                                             : level[y * n + x];
            int g1 = abs_l > 1;
            int inc = ctx_set * 4 + (g1_ctx < 3 ? g1_ctx : 3) +
                      (c_idx ? 16 : 0);
            ops->bin(ops->h, CTX_G1 + inc, g1);
            if (g1) {
                this_sb_g1 = 1;
                if (first_g1_idx < 0)
                    first_g1_idx = i;
                g1_ctx = 0;
            } else if (g1_ctx > 0) {
                g1_ctx++;
            }
        }
        int g2 = 0;
        if (first_g1_idx >= 0) {
            int c = sig_pos[first_g1_idx];
            int x = sx + (co_scan[c] & 15), y = sy + (co_scan[c] >> 4);
            int abs_l = level[y * n + x] < 0 ? -level[y * n + x]
                                             : level[y * n + x];
            g2 = abs_l > 2;
            ops->bin(ops->h, CTX_G2 + ctx_set + (c_idx ? 4 : 0), g2);
        }
        /* sign hiding: when the sub-block qualifies, the sign of the
         * first-in-scan significant coefficient (last entry of sig_pos)
         * is inferred from the abs-sum parity and not coded */
        int hidden = num_sig > 0 &&
            sig_pos[0] - sig_pos[num_sig - 1] > 3;
        for (int i = 0; i < num_sig; i++) {
            int c = sig_pos[i];
            int x = sx + (co_scan[c] & 15), y = sy + (co_scan[c] >> 4);
            if (hidden && i == num_sig - 1)
                continue;
            ops->bypass(ops->h, level[y * n + x] < 0);
        }
        int rice = 0;
        for (int i = 0; i < num_sig; i++) {
            int c = sig_pos[i];
            int x = sx + (co_scan[c] & 15), y = sy + (co_scan[c] >> 4);
            int abs_l = level[y * n + x] < 0 ? -level[y * n + x]
                                             : level[y * n + x];
            int base = 1;
            if (i < 8)
                base += abs_l > 1;      /* the coded g1 value */
            if (i == first_g1_idx)
                base += g2;
            /* remaining present iff every coded flag hit its cap */
            int cap = 1 + (i < 8 ? 1 : 0);
            if (i == first_g1_idx)
                cap += 1;
            int present = (i < 8) ? (base == cap) : 1;
            if (present) {
                write_remaining(ops, abs_l - base, rice);
                if (abs_l > (3 << rice) && rice < 4)
                    rice++;
            }
        }
        prev_sb_g1 = this_sb_g1;
    }
}
