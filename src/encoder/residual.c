/*
 * residual_coding() (ITU-T H.265 7.3.8.11) with the 9.3.4.2.4-9.3.4.2.7
 * context derivations and 9.3.3 binarizations.
 * Copyright (c) 2026, the yah265 authors. GPL-2.0-or-later; see LICENSE.
 */

#include <assert.h>
#include <string.h>

#include "common/cpu.h"
#include "encoder/arm/neon.h"
#include "encoder/x86/avx2.h"   /* self-guarded */
#include "encoder/x86/avx512.h"
#include "encoder/x86/sse42.h"   /* self-guarded */
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
    ops->est = NULL;
    ops->frozen = 0;
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

/* ---- the two instances of the walk --------------------------------
 *
 * The syntax below is written once and compiled twice, on a parameter
 * `est` that is a compile-time constant in each instance. The real
 * emitter's instance is the one that was here before, bin by bin through
 * the res_ops function pointers. The estimator's instance is the point
 * of phase 9 step 2c: the same bins in the same order, but with the
 * per-bin call inlined (n265_est_bin's packed step, est.h) and every
 * BYPASS bin priced as a COUNT instead of a call, because a bypass bin
 * costs exactly 256 in Q8 whatever it is. The estimator was 4-7% of the
 * single-thread wall with the residual walk another 7-12%, and 30% of the bins
 * it charged were bypass ones that carried no information at all.
 *
 * BIT-EXACT, and by construction rather than by measurement: the same
 * context array is stepped by the same function on the same bins in the
 * same order, and the accumulator is a uint32 whose additions are
 * associative, so deferring the bypass count to one shift at the end
 * reaches the same digit. -DN265_EST_AUDIT runs BOTH instances from the
 * same entry contexts on every call and traps on any difference in the
 * total or in the 152 contexts.
 *
 * THE THIRD INSTANCE (phase 13 A1, `est == EST_FROZEN`) is the thing
 * phase 9 step 2c refused to do silently and this item does openly: the
 * context bins are priced from the states the block was ENTERED with and
 * the states are not written at all. Sixteen sig_coeff_flag bins of a
 * 4x4 group land on three contexts, so this is a different estimate and
 * a different bitstream - not a faster way to the same digit - which is
 * why it is a MODE with a band behind it and not an optimization. The
 * reason to want it is that a price
 * with no read-after-write on the context array is a price a vector
 * kernel can compute sixteen at a time (A1b). The walk, the bin count
 * and the bypass handling are IDENTICAL between the two estimator
 * instances; only the lookup moves.
 */
typedef struct { uint32_t bits, byp; } est_acc;

/* the values of the walk's `est` parameter, a compile-time constant in
 * each of the three instances */
#define EST_REAL   0
#define EST_ADAPT  1
#define EST_FROZEN 2

/* the audit's own vacuity check: a scratch build with -DN265_EST_PERT=1
 * moves every context bin's price by one Q8 unit and must trap at once.
 * Zero, and compiled away, in every other build. */
#ifndef N265_EST_PERT
#define N265_EST_PERT 0
#endif

#define N265_AI __attribute__((always_inline)) static inline

/* A context bin. `a` and `ectx` are NULL in the real emitter's instance,
 * and every use of them sits inside the `if (est)` BODY - so they are
 * never dereferenced there even in a build that does not fold the
 * constant. */
#define EMIT_BIN(idx, b)                                                \
    do {                                                                \
        if (est == EST_FROZEN)                                          \
            a->bits += n265_est_cost(&ectx[(idx)], (b)) + N265_EST_PERT;\
        else if (est)                                                   \
            a->bits += n265_est_bin(&ectx[(idx)], (b)) + N265_EST_PERT; \
        else                                                            \
            ops->bin(ops->h, (idx), (b));                               \
    } while (0)
#define EMIT_BYPN(v, n)                                                 \
    do {                                                                \
        if (est)                                                        \
            a->byp += (uint32_t)(n);                                    \
        else                                                            \
            ops->bypass_bits(ops->h, (uint32_t)(v), (n));               \
    } while (0)

N265_AI void write_last_prefix(int est, const res_ops *ops,
                               cabac_ctx_t *ectx, est_acc *a,
                               int ctx_base, int g,
                               int cmax, int ctx_off, int ctx_shift)
{
    int base = ctx_base + ctx_off;
    for (int i = 0; i < g; i++)
        EMIT_BIN(base + (i >> ctx_shift), 1);
    if (g < cmax)
        EMIT_BIN(base + (g >> ctx_shift), 0);
}

/* sig_coeff_flag ctxInc (9.3.4.2.5), split at the group boundary.
 *
 * The derivation has exactly one term that varies WITHIN a 4x4 group -
 * the position term below, a function of (x, y) inside the group and of
 * the right/below neighbor flags - and three that do not: the group's
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
#if N265_NEON_COST
    if (n265_asm_on(N265_ASM_QUANT)) {
        n265_csbf_map_neon(level, log2n, out);
        return;
    }
#endif
#if N265_AVX512_COST
    if (n265_csbf_map_avx512_wide(log2n) && n265_avx512_on(N265_ASM_QUANT)) {
        n265_csbf_map_avx512(level, log2n, out);
        return;
    }
#endif
#if N265_AVX2_COST
    if (n265_avx2_on(N265_ASM_QUANT)) {
        n265_csbf_map_avx2(level, log2n, out);
        return;
    }
#endif
#if N265_SSE42_COST
    if (n265_sse42_on(N265_ASM_QUANT)) {
        n265_csbf_map_sse42(level, log2n, out);
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
#if N265_NEON_COST
    if (n265_asm_on(N265_ASM_QUANT))
        return n265_coeff_group_neon(grp, stride, scan_idx, abs_out,
                                     neg_mask);
#endif
#if N265_AVX2_COST
    if (n265_avx2_on(N265_ASM_QUANT))
        return n265_coeff_group_avx2(grp, stride, scan_idx, abs_out,
                                     neg_mask);
#endif
#if N265_SSE42_COST
    if (n265_sse42_on(N265_ASM_QUANT))
        return n265_coeff_group_sse42(grp, stride, scan_idx, abs_out,
                                     neg_mask);
#endif
    return n265_coeff_group_ref(grp, stride, scan_idx, abs_out, neg_mask);
}

/* coeff_abs_level_remaining is bypass from end to end, so the estimator
 * never needed the bins: its whole cost is the BIN COUNT times 256, and
 * the count is what the writer below emits. Same arithmetic in the same
 * order as write_remaining(), counted instead of pushed. */
static inline int remaining_bin_count(int rem, int k)
{
    if ((rem >> k) < 4)
        return (rem >> k) + 1 + k;
    int v = rem - (4 << k), b = k + 1, bits = 4;
    while (v >= (1 << b)) {
        bits++;
        v -= 1 << b;
        b++;
    }
    return bits + 1 + b;
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
#if N265_NEON_COST
    if (n265_asm_on(N265_ASM_QUANT)) {
        n265_sbh_adjust_neon(level, log2n, scan_idx);
        return;
    }
#endif
#if N265_AVX2_COST
    if (n265_avx2_on(N265_ASM_QUANT)) {
        n265_sbh_adjust_avx2(level, log2n, scan_idx);
        return;
    }
#endif
#if N265_SSE42_COST
    if (n265_sse42_on(N265_ASM_QUANT)) {
        n265_sbh_adjust_sse42(level, log2n, scan_idx);
        return;
    }
#endif
    n265_sbh_adjust_ref(level, log2n, scan_idx);
}

void n265_sbh_adjust_ref(int16_t *level, int log2n, int scan_idx)
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

N265_AI void residual_walk(int est, const res_ops *ops, cabac_ctx_t *ectx,
                           est_acc *a, const int16_t *level,
                           int log2n, int c_idx, int scan_idx, int sbh)
{
    int n = 1 << log2n;
    int sb_dim = n >> 2;                    /* sub-blocks per side */
    int num_sb = sb_dim * sb_dim;
    const uint8_t *sb_scan = n265_scan_sb[scan_idx][log2n - 2];
    const uint8_t *co_scan = n265_scan_coef[scan_idx];

    /* Per-sub-block significance on the sub-block GRID: the context
     * derivation below wants the right and below neighbors of each
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
    write_last_prefix(est, ops, ectx, a, CTX_LAST_X, gx, cmax, ctx_off,
                      ctx_shift);
    write_last_prefix(est, ops, ectx, a, CTX_LAST_Y, gy, cmax, ctx_off,
                      ctx_shift);
    if (gx > 3)
        EMIT_BYPN(last_x - min_in_group[gx], (gx >> 1) - 1);
    if (gy > 3)
        EMIT_BYPN(last_y - min_in_group[gy], (gy >> 1) - 1);

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
            EMIT_BIN(CTX_SIG_GROUP + inc, csbf_s);
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
        /* the whole ctxInc except the pattern byte, added once */
        int sig_base = CTX_SIG + sig_off;
        int dc_sb = log2n > 2 && s == 0;

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
            int idx = (dc_sb && c == 0) ? CTX_SIG + dc_inc
                                        : sig_base + sig_pat[c];
            EMIT_BIN(idx, sig);
            if (sig)
                sig_pos[num_sig++] = c;
        }

        /* level coding: greater1 (first 8), greater2 (first g1), signs,
         * remaining. */
        int abs_v[16], neg_v[16];
        for (int i = 0; i < num_sig; i++) {
            int c = sig_pos[i];
            abs_v[i] = abs_sc[c];
            /* the estimator prices the sign pass by counting it and never
             * reads a sign, so it does not gather one */
            if (!est)
                neg_v[i] = (int)((neg_mask >> c) & 1);
        }
        int ctx_set = ((s > 0 && c_idx == 0) ? 2 : 0) + (prev_sb_g1 ? 1 : 0);
        int g1_ctx = 1;
        int first_g1_idx = -1;
        int this_sb_g1 = 0;
        int n_g1 = num_sig < 8 ? num_sig : 8;
        /* 9.3.4.2.6's ctxSet and the component offset are fixed over the
         * sub-block; only greater1Ctx walks, and it walks 1,2,3 then
         * sticks at 0 forever after the first greater1 */
        int g1_base = CTX_G1 + ctx_set * 4 + (c_idx ? 16 : 0);
        for (int i = 0; i < n_g1; i++) {
            int g1 = abs_v[i] > 1;
            EMIT_BIN(g1_base + (g1_ctx < 3 ? g1_ctx : 3), g1);
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
            EMIT_BIN(CTX_G2 + ctx_set + (c_idx ? 4 : 0), g2);
        }
        /* sign hiding: when the sub-block qualifies, the sign of the
         * first-in-scan significant coefficient (last entry of sig_pos)
         * is inferred from the abs-sum parity and not coded. `sbh` is the
         * rest of 7.4.9.11's condition, which the caller owns:
         * sign_data_hiding_enabled_flag AND !cu_transquant_bypass_flag.
         * A bypass block that dropped a sign here would desynchronize the
         * decoder's parse from that sub-block on, because the decoder
         * does NOT infer one. */
        int hidden = sbh && num_sig > 0 &&
            sig_pos[0] - sig_pos[num_sig - 1] > 3;
        if (est) {
            /* every sign is one bypass bin, so the pass is its count */
            a->byp += (uint32_t)(num_sig - (hidden ? 1 : 0));
        } else {
            for (int i = 0; i < num_sig; i++) {
                if (hidden && i == num_sig - 1)
                    continue;
                ops->bypass(ops->h, neg_v[i]);
            }
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
                if (est)
                    a->byp += (uint32_t)remaining_bin_count(abs_l - base,
                                                            rice);
                else
                    write_remaining(ops, abs_l - base, rice);
                if (abs_l > (3 << rice) && rice < 4)
                    rice++;
            }
        }
        prev_sb_g1 = this_sb_g1;
    }
}

#undef EMIT_BIN
#undef EMIT_BYPN

/* ---- N265_EST_TRACE: the census behind the mode -------------------
 *
 * A scratch build (`-DN265_EST_TRACE`) that runs BOTH estimator
 * instances on every trial's TB from the same entry contexts, keeps the
 * ADAPTIVE answer (so the traced encode is byte for byte the default's,
 * which is what makes the census a census of the shipped encoder's own
 * blocks) and tallies how far the frozen one is from it. Per TB size and
 * component, since the disagreement is a function of how many bins land
 * on one context. Nothing under this is compiled into a shipped binary.
 */
#ifdef N265_EST_TRACE
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>

#define ET_NB 65            /* |rel| in 0.5% steps to 32%, then the tail */
static _Atomic unsigned long long et_n[4][2], et_ad[4][2], et_fr[4][2],
    et_absd[4][2], et_over[4][2], et_hist[4][2][ET_NB];

static void et_report(void)
{
    static const char *sz[4] = { "4x4", "8x8", "16x16", "32x32" };
    fprintf(stderr, "\n--- N265_EST_TRACE: frozen vs adapting residual "
            "estimate, per transform block ---\n");
    fprintf(stderr, "%-7s %-6s %10s %10s %10s %8s %8s %8s %8s %8s %8s\n",
            "size", "comp", "blocks", "adapt_b", "frozen_b", "mean|d|",
            "mean%", "p50%", "p90%", "p99%", "over%");
    unsigned long long tn = 0, tad = 0, tfr = 0, tabs = 0, tov = 0;
    unsigned long long th[ET_NB] = { 0 };
    for (int k = 0; k < 4; k++)
        for (int p = 0; p < 2; p++) {
            unsigned long long n = et_n[k][p];
            if (!n)
                continue;
            unsigned long long ad = et_ad[k][p], fr = et_fr[k][p];
            unsigned long long ab = et_absd[k][p], ov = et_over[k][p];
            unsigned long long h[ET_NB];
            for (int i = 0; i < ET_NB; i++) {
                h[i] = et_hist[k][p][i];
                th[i] += h[i];
            }
            tn += n; tad += ad; tfr += fr; tabs += ab; tov += ov;
            double pc[3];
            const double want[3] = { 0.50, 0.90, 0.99 };
            unsigned long long acc = 0;
            int j = 0;
            for (int i = 0; i < ET_NB && j < 3; i++) {
                acc += h[i];
                while (j < 3 && (double)acc >= want[j] * (double)n)
                    pc[j++] = (i + 1) * 0.5;
            }
            while (j < 3)
                pc[j++] = 99.0;
            fprintf(stderr, "%-7s %-6s %10llu %10.0f %10.0f %8.2f %8.2f "
                    "%8.1f %8.1f %8.1f %8.1f\n",
                    sz[k], p ? "chroma" : "luma", n, ad / 256.0, fr / 256.0,
                    (double)ab / n / 256.0,
                    100.0 * (double)ab / (double)(ad ? ad : 1),
                    pc[0], pc[1], pc[2], 100.0 * (double)ov / (double)n);
        }
    if (tn) {
        double pc[3];
        const double want[3] = { 0.50, 0.90, 0.99 };
        unsigned long long acc = 0;
        int j = 0;
        for (int i = 0; i < ET_NB && j < 3; i++) {
            acc += th[i];
            while (j < 3 && (double)acc >= want[j] * (double)tn)
                pc[j++] = (i + 1) * 0.5;
        }
        while (j < 3)
            pc[j++] = 99.0;
        fprintf(stderr, "%-7s %-6s %10llu %10.0f %10.0f %8.2f %8.2f %8.1f "
                "%8.1f %8.1f %8.1f\n", "ALL", "", tn, tad / 256.0,
                tfr / 256.0, (double)tabs / tn / 256.0,
                100.0 * (double)tabs / (double)(tad ? tad : 1), pc[0], pc[1],
                pc[2], 100.0 * (double)tov / (double)tn);
        fprintf(stderr, "net frozen-adapt: %+.3f%% of all residual bits "
                "(%.0f vs %.0f)\n",
                100.0 * ((double)tfr - (double)tad) / (double)(tad ? tad : 1),
                tfr / 256.0, tad / 256.0);
    }
}

static void et_tally(int log2n, int c_idx, uint32_t ad, uint32_t fr)
{
    static atomic_int armed;
    int k = log2n - 2, p = c_idx != 0;
    if (!atomic_exchange_explicit(&armed, 1, memory_order_relaxed))
        atexit(et_report);
    long long d = (long long)fr - (long long)ad;
    unsigned long long abs_d = d < 0 ? (unsigned long long)-d
                                     : (unsigned long long)d;
    double rel = ad ? 100.0 * (double)abs_d / (double)ad : 0.0;
    int b = (int)(rel / 0.5);
    if (b >= ET_NB)
        b = ET_NB - 1;
    atomic_fetch_add_explicit(&et_n[k][p], 1, memory_order_relaxed);
    atomic_fetch_add_explicit(&et_ad[k][p], ad, memory_order_relaxed);
    atomic_fetch_add_explicit(&et_fr[k][p], fr, memory_order_relaxed);
    atomic_fetch_add_explicit(&et_absd[k][p], abs_d, memory_order_relaxed);
    atomic_fetch_add_explicit(&et_over[k][p], d > 0, memory_order_relaxed);
    atomic_fetch_add_explicit(&et_hist[k][p][b], 1, memory_order_relaxed);
}
#endif

void n265_residual_coding(const res_ops *ops, const int16_t *level,
                          int log2n, int c_idx, int scan_idx, int sbh)
{
    est_state *st = ops->est;

    if (!st) {
        residual_walk(EST_REAL, ops, NULL, NULL, level, log2n, c_idx,
                      scan_idx, sbh);
        return;
    }

    /* phase 13 A1: the frozen-context mode. The contexts are read and
     * not written, so "the state at TB entry" needs no snapshot - the
     * array IS the snapshot for the length of the block. */
    if (ops->frozen) {
        est_acc f = { 0, 0 };
        residual_walk(EST_FROZEN, ops, st->ctx, &f, level, log2n, c_idx,
                      scan_idx, sbh);
        st->bits += f.bits + (f.byp << 8);
        return;
    }

#ifdef N265_EST_TRACE
    {
        cabac_ctx_t entry[CTX_COUNT];
        memcpy(entry, st->ctx, sizeof entry);
        est_acc ad = { 0, 0 };
        residual_walk(EST_ADAPT, ops, st->ctx, &ad, level, log2n, c_idx,
                      scan_idx, sbh);
        uint32_t a_bits = ad.bits + (ad.byp << 8);
        cabac_ctx_t after[CTX_COUNT];
        memcpy(after, st->ctx, sizeof after);
        memcpy(st->ctx, entry, sizeof entry);
        est_acc fz = { 0, 0 };
        residual_walk(EST_FROZEN, ops, st->ctx, &fz, level, log2n, c_idx,
                      scan_idx, sbh);
        et_tally(log2n, c_idx, a_bits, fz.bits + (fz.byp << 8));
        memcpy(st->ctx, after, sizeof after);
        st->bits += a_bits;
        return;
    }
#endif

#ifdef N265_EST_AUDIT
    /* In-place trap compare, the method used for the lattice. The
     * PRE-ITEM form is the est=0 instance driven through this backend's
     * own callbacks, which for the estimator ARE est_bin_cb /
     * est_bypass_cb: it is the code that shipped, bin for bin. Run it from the
     * entry contexts, rewind, run the straight-line instance from the same
     * entry contexts, and trap unless the Q8 total AND all 152 contexts agree.
     * Scratch build only (-DN265_EST_AUDIT); nothing under it is compiled in a
     * shipped binary. */
    {
        cabac_ctx_t save[CTX_COUNT], old_ctx[CTX_COUNT];
        memcpy(save, st->ctx, sizeof save);
        uint32_t b0 = st->bits;
        residual_walk(EST_REAL, ops, NULL, NULL, level, log2n, c_idx,
                      scan_idx, sbh);
        uint32_t old_bits = st->bits - b0;
        memcpy(old_ctx, st->ctx, sizeof old_ctx);
        memcpy(st->ctx, save, sizeof save);
        st->bits = b0;
        est_acc chk = { 0, 0 };
        residual_walk(EST_ADAPT, ops, st->ctx, &chk, level, log2n, c_idx,
                      scan_idx, sbh);
        uint32_t new_bits = chk.bits + (chk.byp << 8);
#if N265_EST_PERT == 2
        st->ctx[CTX_SIG].state ^= 1;    /* vacuity check of the ctx leg */
#endif
        if (new_bits != old_bits ||
            memcmp(old_ctx, st->ctx, sizeof old_ctx) != 0)
            __builtin_trap();
        st->bits += new_bits;
        return;
    }
#else
    est_acc a = { 0, 0 };
    residual_walk(EST_ADAPT, ops, st->ctx, &a, level, log2n, c_idx, scan_idx,
                  sbh);
    /* the bypass count is folded once: 256 per bin, and uint32 addition
     * is associative, so the digit is the one the per-call form reached */
    st->bits += a.bits + (a.byp << 8);
#endif
}
