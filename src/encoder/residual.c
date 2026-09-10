/*
 * residual_coding() (ITU-T H.265 7.3.8.11) with the 9.3.4.2.4-9.3.4.2.7
 * context derivations and 9.3.3 binarizations.
 * Copyright (c) 2026, the yah265 authors. GPL-2.0-or-later; see LICENSE.
 */

#include <assert.h>

#include "common/cpu.h"
#include "encoder/arm/neon.h"
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

/* sig_coeff_flag ctxInc (9.3.4.2.5), split at the group boundary.
 *
 * The derivation has exactly one term that varies WITHIN a 4x4 group -
 * the position term below, a function of (x, y) inside the group and of
 * the right/below neighbour flags - and three that do not: the group's
 * own +3, the TB size and scan offset, and the component. So the sixteen
 * ctxIncs of a group are one fixed sixteen-byte pattern plus a scalar the
 * caller works out once per group, and the per-coefficient switch the
 * scalar form ran (over a loop that also carries an indirect call per
 * bin) becomes a byte load. The patterns are spec-fixed; checkasm proves
 * them against the literal 9.3.4.2.5 form. */
uint8_t n265_sig_pat[3][5][16];

static int sig_pos_term(int xp, int yp, int prev_csbf)
{
    switch (prev_csbf) {
    case 0: return (xp + yp == 0) ? 2 : (xp + yp < 3) ? 1 : 0;
    case 1: return (yp == 0) ? 2 : (yp == 1) ? 1 : 0;
    case 2: return (xp == 0) ? 2 : (xp == 1) ? 1 : 0;
    default: return 2;
    }
}

void n265_residual_tables_init(void)
{
    static int done;
    if (done)
        return;
    for (int k = 0; k < 3; k++)
        for (int c = 0; c < 16; c++) {
            int x = n265_scan_coef[k][c] & 15, y = n265_scan_coef[k][c] >> 4;
            for (int p = 0; p < 4; p++)
                n265_sig_pat[k][p][c] = (uint8_t)sig_pos_term(x, y, p);
            /* the 4x4 TB's own map; (3,3) is the block's last position and
             * is never coded, which is why the spec table has 15 entries */
            n265_sig_pat[k][4][c] =
                ((y << 2) | x) < 15 ? n265_sig_ctx_map4x4[(y << 2) | x] : 0;
        }
    done = 1;
}

/* the constant added to the pattern for one group (all of the ctxInc that
 * does not vary within it); the DC coefficient of the whole TB is the one
 * exception and the caller overrides it */
static int sig_group_off(int log2n, int c_idx, int scan_idx, int sb_nonzero)
{
    if (log2n == 2)
        return c_idx ? 27 : 0;
    if (c_idx == 0)
        return (sb_nonzero ? 3 : 0) +
               ((log2n == 3) ? (scan_idx == 0 ? 9 : 15) : 21);
    return 27 + ((log2n == 3) ? 9 : 12);
}

/* ---- the two elementwise passes and their dispatch ------------------ */

void n265_csbf_map_ref(const int16_t *level, int log2n, uint8_t *out)
{
    int n = 1 << log2n, sb_dim = n >> 2;

    for (int sby = 0; sby < sb_dim; sby++)
        for (int sbx = 0; sbx < sb_dim; sbx++) {
            unsigned nz = 0;
            for (int y = 0; y < 4; y++)
                for (int x = 0; x < 4; x++)
                    nz |= (uint16_t)level[(sby * 4 + y) * n + sbx * 4 + x];
            out[sby * sb_dim + sbx] = nz != 0;
        }
}

void n265_csbf_map(const int16_t *level, int log2n, uint8_t *out)
{
#if N265_NEON
    if (n265_asm_on(N265_ASM_QUANT)) {
        n265_csbf_map_neon(level, log2n, out);
        return;
    }
#endif
    n265_csbf_map_ref(level, log2n, out);
}

uint32_t n265_coeff_group_ref(const int16_t *grp, ptrdiff_t stride,
                              int scan_idx, uint16_t *abs_out,
                              uint32_t *neg_mask)
{
    const uint8_t *co = n265_scan_coef[scan_idx];
    uint32_t sig = 0, neg = 0;

    for (int c = 0; c < 16; c++) {
        int v = grp[(co[c] >> 4) * stride + (co[c] & 15)];
        /* unsigned negation so that -32768 gives 32768 and not UB; ARM
         * ABS leaves the same 0x8000 in the lane, which is what makes the
         * two paths bit-exact at that end of the domain */
        abs_out[c] = v < 0 ? (uint16_t)(0u - (unsigned)v) : (uint16_t)v;
        sig |= (uint32_t)(v != 0) << c;
        neg |= (uint32_t)(v < 0) << c;
    }
    *neg_mask = neg;
    return sig;
}

uint32_t n265_coeff_group(const int16_t *grp, ptrdiff_t stride, int scan_idx,
                          uint16_t *abs_out, uint32_t *neg_mask)
{
#if N265_NEON
    if (n265_asm_on(N265_ASM_QUANT))
        return n265_coeff_group_neon(grp, stride, scan_idx, abs_out,
                                     neg_mask);
#endif
    return n265_coeff_group_ref(grp, stride, scan_idx, abs_out, neg_mask);
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

    /* Per-sub-block significance on the sub-block GRID: the context
     * derivation below wants the right and below neighbours of each
     * sub-block, and finding them by walking the scan is quadratic in the
     * sub-block count (4096 lookups for a 32x32 TB, on a path the mode
     * decision runs per candidate). The grid is in raster order, so the
     * scan-order flag is one indexed read where it is wanted and no
     * second array is kept. */
    uint8_t csbf_xy[64] = { 0 };
    n265_csbf_map(level, log2n, csbf_xy);

    /* last significant position in scan order */
    int last_s = -1, last_c = -1;
    for (int s = num_sb - 1; s >= 0; s--)
        if (csbf_xy[(sb_scan[s] >> 4) * sb_dim + (sb_scan[s] & 15)]) {
            last_s = s;
            break;
        }
    assert(last_s >= 0);

    int last_sx = (sb_scan[last_s] & 15) << 2;
    int last_sy = (sb_scan[last_s] >> 4) << 2;
    for (int c = 15; c >= 0; c--) {
        int x = last_sx + (co_scan[c] & 15), y = last_sy + (co_scan[c] >> 4);
        if (level[y * n + x]) {
            last_c = c;
            break;
        }
    }
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
        int sb_x = sb_scan[s] & 15, sb_y = sb_scan[s] >> 4;
        int sx = sb_x << 2, sy = sb_y << 2;
        int csbf_s = csbf_xy[sb_y * sb_dim + sb_x];

        /* neighbor csbf for contexts: right and below sub-blocks */
        int csbf_right = sb_x + 1 < sb_dim
            ? csbf_xy[sb_y * sb_dim + sb_x + 1] : 0;
        int csbf_below = sb_y + 1 < sb_dim
            ? csbf_xy[(sb_y + 1) * sb_dim + sb_x] : 0;
        int prev_csbf = csbf_right | (csbf_below << 1);

        int infer_sb_dc = 0;
        if (s < last_s && s > 0) {
            int inc = ((csbf_right || csbf_below) ? 1 : 0) + (c_idx ? 2 : 0);
            ops->bin(ops->h, CTX_SIG_GROUP + inc, csbf_s);
            if (!csbf_s)
                continue;
            infer_sb_dc = 1;
        }
        /* s == 0: csbf is inferred 1 by the decoder, so its significance
         * flags are always coded, even when the whole sub-block is zero. */

        /* The group's sixteen levels in scan order, once: the significance
         * the flag pass codes, and the magnitudes and signs the four level
         * passes below all want. */
        uint16_t abs_sc[16];
        uint32_t neg_mask;
        uint32_t sig_mask = n265_coeff_group(level + sy * n + sx, n, scan_idx,
                                             abs_sc, &neg_mask);
        const uint8_t *sig_pat = n265_sig_pat[scan_idx][log2n == 2 ? 4
                                                                  : prev_csbf];
        int sig_off = sig_group_off(log2n, c_idx, scan_idx, sb_x + sb_y > 0);
        /* the TB's DC is the one coefficient outside the group pattern
         * (9.3.4.2.5 gives it ctxInc 0, or 27 on chroma), and it can only
         * be scan position 0 of sub-block 0 */
        int dc_inc = c_idx ? 27 : 0;

        /* significance flags, reverse scan; last coeff inferred in its SB */
        int start_c = (s == last_s) ? last_c - 1 : 15;
        int sig_pos[16], num_sig = 0;
        if (s == last_s) {
            sig_pos[num_sig++] = last_c;
        }
        for (int c = start_c; c >= 0; c--) {
            int sig = (int)((sig_mask >> c) & 1);
            if (c == 0 && infer_sb_dc && num_sig == 0) {
                /* would-be-empty coded sub-block: DC inferred significant */
                sig_pos[num_sig++] = 0;
                break;
            }
            int inc = (log2n > 2 && s == 0 && c == 0) ? dc_inc
                                                      : sig_off + sig_pat[c];
            ops->bin(ops->h, CTX_SIG + inc, sig);
            if (sig)
                sig_pos[num_sig++] = c;
        }

        /* level coding: greater1 (first 8), greater2 (first g1), signs,
         * remaining. */
        int abs_v[16], neg_v[16];
        for (int i = 0; i < num_sig; i++) {
            int c = sig_pos[i];
            abs_v[i] = abs_sc[c];
            neg_v[i] = (int)((neg_mask >> c) & 1);
        }
        int ctx_set = ((s > 0 && c_idx == 0) ? 2 : 0) + (prev_sb_g1 ? 1 : 0);
        int g1_ctx = 1;
        int first_g1_idx = -1;
        int this_sb_g1 = 0;
        int n_g1 = num_sig < 8 ? num_sig : 8;
        for (int i = 0; i < n_g1; i++) {
            int g1 = abs_v[i] > 1;
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
            g2 = abs_v[first_g1_idx] > 2;
            ops->bin(ops->h, CTX_G2 + ctx_set + (c_idx ? 4 : 0), g2);
        }
        /* sign hiding: when the sub-block qualifies, the sign of the
         * first-in-scan significant coefficient (last entry of sig_pos)
         * is inferred from the abs-sum parity and not coded */
        int hidden = num_sig > 0 &&
            sig_pos[0] - sig_pos[num_sig - 1] > 3;
        for (int i = 0; i < num_sig; i++) {
            if (hidden && i == num_sig - 1)
                continue;
            ops->bypass(ops->h, neg_v[i]);
        }
        int rice = 0;
        for (int i = 0; i < num_sig; i++) {
            int abs_l = abs_v[i];
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
