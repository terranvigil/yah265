/*
 * checkasm/cost.c - the coefficient-costing family (class N265_ASM_QUANT).
 *
 * Two kernels and one table:
 *
 *   n265_csbf_map      per-4x4-group significance of a whole TB
 *   n265_coeff_group   one group gathered into scan order (|level|, sign,
 *                      significance)
 *   n265_sig_pat       the sig_coeff_flag ctxInc pattern residual_coding
 *                      reads instead of re-deriving 9.3.4.2.5 per
 *                      coefficient. It is not a kernel and cannot be
 *                      benched, but it is new derived data on the rate
 *                      path, so it is proved here against a literal
 *                      transcription of the spec clause.
 *
 * The level POPULATION is what varies across cases here rather than the
 * geometry: an all-zero group, a group with one nonzero, a dense one and
 * the two ends of the int16 domain are different paths through both the
 * reference and the vector form, and the -32768 end is the one where
 * "abs" has to be pinned down (ARM ABS wraps, and the reference's
 * unsigned negation is written to agree with it).
 *
 * See checkasm.h for the harness and the convention.
 * Copyright (c) 2026, the yah265 authors. GPL-2.0-or-later; see LICENSE.
 */

#include "checkasm.h"

#include "encoder/residual.h"
#include "encoder/tables.h"

/* ---- the spec table, proved against the literal clause -------------- */

/* 9.3.4.2.5 transcribed straight from the clause, the way residual.c had
 * it before the pattern split: the full ctxInc for one coefficient. */
static int sig_ctx_literal(int xc, int yc, int log2n, int c_idx, int scan_idx,
                           int prev_csbf)
{
    if (log2n == 2) {
        int base = ((yc << 2) | xc) < 15
            ? n265_sig_ctx_map4x4[(yc << 2) | xc] : 0;
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

/* the offset residual.c adds to the pattern, kept in step with it here */
static int group_off(int log2n, int c_idx, int scan_idx, int sb_nonzero)
{
    if (log2n == 2)
        return c_idx ? 27 : 0;
    if (c_idx == 0)
        return (sb_nonzero ? 3 : 0) +
               ((log2n == 3) ? (scan_idx == 0 ? 9 : 15) : 21);
    return 27 + ((log2n == 3) ? 9 : 12);
}

static int check_sig_pat(void)
{
    int bad = 0;

    n265_residual_tables_init();
    for (int log2n = 2; log2n <= 5; log2n++) {
        int sb_dim = (1 << log2n) >> 2;
        for (int c_idx = 0; c_idx < 2; c_idx++)
            for (int scan_idx = 0; scan_idx < 3; scan_idx++)
                for (int sby = 0; sby < sb_dim; sby++)
                    for (int sbx = 0; sbx < sb_dim; sbx++)
                        for (int p = 0; p < 4; p++) {
                            const uint8_t *pat =
                                n265_sig_pat[scan_idx][log2n == 2 ? 4 : p];
                            int off = group_off(log2n, c_idx, scan_idx,
                                                sbx + sby > 0);
                            for (int c = 0; c < 16; c++) {
                                int xc = (sbx << 2) +
                                         (n265_scan_coef[scan_idx][c] & 15);
                                int yc = (sby << 2) +
                                         (n265_scan_coef[scan_idx][c] >> 4);
                                int want = sig_ctx_literal(xc, yc, log2n,
                                                           c_idx, scan_idx, p);
                                int got = off + pat[c];
                                /* the TB's DC is the clause's one exception
                                 * and residual.c overrides it by hand */
                                if (log2n > 2 && xc + yc == 0)
                                    got = c_idx ? 27 : 0;
                                if (want != got) {
                                    printf("    sig_pat MISMATCH log2n %d "
                                           "cidx %d scan %d sb (%d,%d) "
                                           "prev %d c %d: spec %d table %d\n",
                                           log2n, c_idx, scan_idx, sbx, sby,
                                           p, c, want, got);
                                    if (++bad > 8)
                                        return bad;
                                }
                            }
                        }
    }
    return bad;
}

/* ---- level generators ----------------------------------------------
 *
 * Levels reach residual_coding straight out of the quantizer (and out of
 * n265_sbh_adjust, which can grow one magnitude by 1). Sparse blocks are
 * the encoder's common case by a long way - the 2026-09-10 survey found
 * 86% of the sub-blocks scanned by the csbf pass hold nothing at all -
 * so the population cases are weighted that way, with the int16 ends
 * present because the abs and the masks are proved at them.
 */
static const char *lkind_name[] = {
    "empty", "one nonzero", "sparse", "small dense", "full-range dense",
    "all +32767", "all -32768", "int16 ends alternating", "dc only",
    "low-frequency corner",
};
#define N_LKIND ((int)(sizeof(lkind_name) / sizeof(lkind_name[0])))

static void fill_level(int16_t *l, int n, int kind)
{
    int nn = n * n;

    for (int i = 0; i < nn; i++)
        l[i] = 0;
    switch (kind) {
    case 0:
        break;
    case 1:
        l[rnd() % (unsigned)nn] = (int16_t)((int)(rnd() >> 26) - 32);
        break;
    case 2:
        for (int k = 0; k < 1 + nn / 32; k++)
            l[rnd() % (unsigned)nn] = (int16_t)((int)(rnd() >> 26) - 32);
        break;
    case 3:
        for (int i = 0; i < nn; i++)
            l[i] = (int16_t)((int)(rnd() >> 28) - 8);
        break;
    case 4:
        for (int i = 0; i < nn; i++)
            l[i] = (int16_t)(rnd() >> 16);
        break;
    case 5:
    case 6:
        for (int i = 0; i < nn; i++)
            l[i] = kind == 5 ? 32767 : (int16_t)-32768;
        break;
    case 7:
        for (int i = 0; i < nn; i++)
            l[i] = (rnd() & 1) ? 32767 : (int16_t)-32768;
        break;
    case 8:
        l[0] = (int16_t)((int)(rnd() >> 20) - 2048);
        break;
    default: {
        int c = n < 4 ? n : 4;
        for (int y = 0; y < c; y++)
            for (int x = 0; x < c; x++)
                l[y * n + x] = (int16_t)((int)(rnd() >> 24) - 128);
        break;
    }
    }
}

/* ---- the C-fast leg's bench context --------------------------------- */

struct cctx {
    int log2n, scan_idx;
    int16_t level[32 * 32];
    uint8_t map[64];
    uint16_t abs[16];
    int16_t seed[32 * 32];
    uint32_t neg, sig;
};

static void csbf_ref_call(void *p)
{
    struct cctx *c = p;
    n265_csbf_map_ref(c->level, c->log2n, c->map);
}

static void group_ref_call(void *p)
{
    struct cctx *c = p;
    c->sig = n265_coeff_group_ref(c->level, 1 << c->log2n, c->scan_idx,
                                  c->abs, &c->neg);
}

/* sbh_adjust is IN PLACE, so the bench cannot run the same buffer twice
 * and get the same work: each call re-seeds from the untouched copy the
 * context carries, which is one 2 KiB memcpy at 32x32 charged equally to
 * every leg. */
static void sbh_ref_call(void *p)
{
    struct cctx *c = p;
    memcpy(c->level, c->seed, sizeof(c->level));
    n265_sbh_adjust_ref(c->level, c->log2n, c->scan_idx);
}

#if CA_SIMD_COST
static void csbf_neon_call(void *p)
{
    struct cctx *c = p;
    CA_SIMD(n265_csbf_map)(c->level, c->log2n, c->map);
}

static void group_neon_call(void *p)
{
    struct cctx *c = p;
    c->sig = CA_SIMD(n265_coeff_group)(c->level, 1 << c->log2n, c->scan_idx,
                                   c->abs, &c->neg);
}

static void sbh_neon_call(void *p)
{
    struct cctx *c = p;
    memcpy(c->level, c->seed, sizeof(c->level));
    CA_SIMD(n265_sbh_adjust)(c->level, c->log2n, c->scan_idx);
}
#define CSBF_NEON csbf_neon_call
#define GROUP_NEON group_neon_call
#define SBH_NEON sbh_neon_call
#else
#define CSBF_NEON NULL
#define GROUP_NEON NULL
#define SBH_NEON NULL
#endif

#if CA_SIMD_COST

/* The TB shapes residual_coding actually sees. From the same 2026-09-10
 * survey: 4x4 through 32x32 luma, 4x4/8x8/16x16 chroma, and the two
 * non-diagonal scans only at 4x4 and 8x8 luma intra (the mode-dependent
 * scan of 8.4.4.2.7). csbf_map takes no scan at all, so its table is the
 * sizes alone. */
static const int tsizes[] = { 2, 3, 4, 5 };
#define N_TSIZE ((int)(sizeof(tsizes) / sizeof(tsizes[0])))

/* csbf_map's declared window is exactly the n*n int16 of the block in and
 * the (n>>2)^2 bytes of the map out: no stride, no halo, no tail. So the
 * page-guard mapping IS the block on both sides, and a store one vector
 * too far past a 2-byte map is caught as readily as a load past the last
 * row. */
static void check_csbf_window(int log2n, int kind)
{
    int n = 1 << log2n, sb_dim = n >> 2;

    for (int tail = 0; tail < 2; tail++) {
        ca_pg gl, gm;
        int16_t *level = ca_pg_alloc(&gl, (size_t)n * n * sizeof(int16_t),
                                     tail);
        uint8_t *map = ca_pg_alloc(&gm, (size_t)sb_dim * sb_dim, tail);
        char what[96];

        fill_level(level, n, kind);
        snprintf(what, sizeof(what), "csbf_map %dx%d / %s, %s edge", n, n,
                 lkind_name[kind], tail ? "far" : "near");
        ca_pg_arm(what);
        CA_SIMD(n265_csbf_map)(level, log2n, map);
        ca_pg_disarm();
        ca_pg_free(&gl);
        ca_pg_free(&gm);
    }
}

/* coeff_group's window is the four rows of four it declares and NOT the
 * rest of the block: a group at the bottom right of a 32x32 has only
 * 3 * stride + 4 elements ahead of it, so the mapping is sized to that.
 * A kernel that loaded a whole 16-byte row per group (eight coefficients
 * where four were declared) faults here and nowhere else. */
static void check_group_window(int log2n, int scan_idx, int kind)
{
    int n = 1 << log2n;
    size_t win = ca_pg_window(n, 4, 4) * sizeof(int16_t);

    for (int tail = 0; tail < 2; tail++) {
        ca_pg gl, ga;
        int16_t *grp = ca_pg_alloc(&gl, win, tail);
        uint16_t *abs_out = ca_pg_alloc(&ga, 16 * sizeof(uint16_t), tail);
        uint32_t neg;
        char what[96];

        ca_pg_fill(&gl, win);
        if (kind == 0)
            memset(grp, 0, win);
        snprintf(what, sizeof(what), "coeff_group stride %d scan %d / %s, "
                 "%s edge", n, scan_idx, lkind_name[kind],
                 tail ? "far" : "near");
        ca_pg_arm(what);
        CA_SIMD(n265_coeff_group)(grp, n, scan_idx, abs_out, &neg);
        ca_pg_disarm();
        ca_pg_free(&gl);
        ca_pg_free(&ga);
    }
}

static int check_csbf_neon(void)
{
    int bad = 0;

    for (int s = 0; s < N_TSIZE; s++)
        for (int kind = 0; kind < N_LKIND; kind++)
            check_csbf_window(tsizes[s], kind);

    for (int s = 0; s < N_TSIZE && !bad; s++) {
        int log2n = tsizes[s], n = 1 << log2n, sb_dim = n >> 2;
        size_t lb = (size_t)n * n * sizeof(int16_t);
        size_t mb = (size_t)sb_dim * sb_dim;
        int16_t *level = ca_guard_alloc(lb);
        uint8_t *map = ca_guard_alloc(mb);
        uint8_t ref[64], first[64];

        for (int kind = 0; kind < N_LKIND && !bad; kind++) {
            int reps = kind <= 2 ? 32 : 4;

            for (int rep = 0; rep < reps; rep++) {
                fill_level(level, n, kind);
                n265_csbf_map_ref(level, log2n, ref);

                ca_guard_poison(map, mb);
                ca_guard_scramble(level, lb);
                CA_SIMD(n265_csbf_map)(level, log2n, map);
                bad += ca_guard_check(map, mb, "csbf_map_neon map");
                for (size_t i = 0; i < mb; i++)
                    if (ref[i] != map[i]) {
                        printf("    csbf_map MISMATCH %dx%d / %s: [%zu] "
                               "ref %d simd %d\n", n, n, lkind_name[kind], i,
                               ref[i], map[i]);
                        bad++;
                        break;
                    }
                memcpy(first, map, mb);

                /* read window: the same block behind a different border
                 * must give the same map, or the kernel loaded outside it */
                ca_guard_scramble(level, lb);
                memset(map, 0, mb);
                CA_SIMD(n265_csbf_map)(level, log2n, map);
                if (memcmp(first, map, mb)) {
                    printf("    csbf_map_neon %dx%d / %s: READ past the "
                           "block (border changed the result)\n", n, n,
                           lkind_name[kind]);
                    bad++;
                }
                if (bad)
                    break;
            }
        }
        ca_guard_free(level);
        ca_guard_free(map);
    }
    return bad;
}

static int check_group_neon(void)
{
    int bad = 0;

    for (int s = 0; s < N_TSIZE; s++)
        for (int scan_idx = 0; scan_idx < 3; scan_idx++)
            for (int kind = 0; kind < N_LKIND; kind++)
                check_group_window(tsizes[s], scan_idx, kind);

    for (int s = 0; s < N_TSIZE && !bad; s++) {
        int log2n = tsizes[s], n = 1 << log2n, sb_dim = n >> 2;
        size_t lb = (size_t)n * n * sizeof(int16_t);
        size_t ab = 16 * sizeof(uint16_t);
        int16_t *level = ca_guard_alloc(lb);
        uint16_t *abs_out = ca_guard_alloc(ab);
        uint16_t ref_abs[16], first[16];
        uint32_t ref_neg, neg, ref_sig, sig, first_sig, first_neg;

        for (int scan_idx = 0; scan_idx < 3 && !bad; scan_idx++)
            for (int kind = 0; kind < N_LKIND && !bad; kind++) {
                int reps = kind <= 2 ? 8 : 2;

                for (int rep = 0; rep < reps && !bad; rep++) {
                    int16_t save[32 * 32];

                    fill_level(level, n, kind);
                    memcpy(save, level, lb);
                    for (int sby = 0; sby < sb_dim && !bad; sby++)
                        for (int sbx = 0; sbx < sb_dim && !bad; sbx++) {
                            const int16_t *grp = level + (size_t)(sby << 2) * n
                                                 + (sbx << 2);

                            ref_sig = n265_coeff_group_ref(grp, n, scan_idx,
                                                           ref_abs, &ref_neg);
                            ca_guard_poison(abs_out, ab);
                            sig = CA_SIMD(n265_coeff_group)(grp, n, scan_idx,
                                                        abs_out, &neg);
                            bad += ca_guard_check(abs_out, ab,
                                                  "coeff_group_neon abs");
                            if (sig != ref_sig || neg != ref_neg) {
                                printf("    coeff_group MASK MISMATCH %dx%d "
                                       "scan %d / %s sb (%d,%d): sig ref "
                                       "%04x simd %04x, neg ref %04x simd "
                                       "%04x\n", n, n, scan_idx,
                                       lkind_name[kind], sbx, sby, ref_sig,
                                       sig, ref_neg, neg);
                                bad++;
                                break;
                            }
                            for (int i = 0; i < 16; i++)
                                if (ref_abs[i] != abs_out[i]) {
                                    printf("    coeff_group ABS MISMATCH "
                                           "%dx%d scan %d / %s sb (%d,%d): "
                                           "[%d] ref %u simd %u\n", n, n,
                                           scan_idx, lkind_name[kind], sbx,
                                           sby, i, ref_abs[i], abs_out[i]);
                                    bad++;
                                    break;
                                }
                            if (bad)
                                break;
                            memcpy(first, abs_out, ab);
                            first_sig = sig;
                            first_neg = neg;

                            /* read window: the kernel declares four rows of
                             * four, so randomizing the rest of the block -
                             * this group's sixteen levels put back exactly -
                             * must not move the answer */
                            for (int i = 0; i < n * n; i++)
                                level[i] = (int16_t)rnd();
                            for (int y = 0; y < 4; y++)
                                for (int x = 0; x < 4; x++) {
                                    size_t o = (size_t)((sby << 2) + y) * n +
                                               (sbx << 2) + x;
                                    level[o] = save[o];
                                }
                            ca_guard_scramble(level, lb);
                            memset(abs_out, 0, ab);
                            sig = CA_SIMD(n265_coeff_group)(grp, n, scan_idx,
                                                        abs_out, &neg);
                            if (memcmp(first, abs_out, ab) ||
                                sig != first_sig || neg != first_neg) {
                                printf("    coeff_group_neon %dx%d scan %d / "
                                       "%s: READ past the group\n", n, n,
                                       scan_idx, lkind_name[kind]);
                                bad++;
                            }
                            memcpy(level, save, lb);
                        }
                }
            }
        ca_guard_free(level);
        ca_guard_free(abs_out);
    }
    return bad;
}
#endif /* CA_SIMD_COST */

#if CA_SIMD_COST

/* sbh_adjust's window is the whole block and nothing else - it reads four
 * rows of four per group at the block stride and writes at most one
 * element per group, all inside n*n. The mapping is therefore the block
 * itself, and a kernel that loaded eight coefficients where four were
 * declared faults on the last group of the last row. */
static void check_sbh_window(int log2n, int scan_idx, int kind)
{
    int n = 1 << log2n;
    size_t lb = (size_t)n * n * sizeof(int16_t);

    for (int tail = 0; tail < 2; tail++) {
        ca_pg gl;
        int16_t *level = ca_pg_alloc(&gl, lb, tail);
        char what[96];

        fill_level(level, n, kind);
        snprintf(what, sizeof(what), "sbh_adjust %dx%d scan %d / %s, %s edge",
                 n, n, scan_idx, lkind_name[kind], tail ? "far" : "near");
        ca_pg_arm(what);
        CA_SIMD(n265_sbh_adjust)(level, log2n, scan_idx);
        ca_pg_disarm();
        ca_pg_free(&gl);
    }
}

/* A population aimed at the two decisions the kernel makes that the
 * generic level kinds hit only by accident: the `last - first <= 3` gate,
 * where a group whose span is exactly 3 must be left alone and one whose
 * span is exactly 4 must be considered, and the parity test, which has to
 * see both answers at both signs of the first coefficient. Positions are
 * chosen in SCAN order and written through the scan table, so the case is
 * the same case under all three scans. */
static void fill_span(int16_t *l, int n, int log2n, int scan_idx,
                      int span, int par_bump, int neg_first)
{
    const uint8_t *sb_scan = n265_scan_sb[scan_idx][log2n - 2];
    const uint8_t *co_scan = n265_scan_coef[scan_idx];
    int sb_dim = n >> 2;

    memset(l, 0, (size_t)n * n * sizeof(int16_t));
    for (int s = 0; s < sb_dim * sb_dim; s++) {
        int sx = (sb_scan[s] & 15) << 2, sy = (sb_scan[s] >> 4) << 2;
        int first = (int)(rnd() % (unsigned)(16 - span));
        int last = first + span;

        for (int k = first; k <= last; k++) {
            int x = sx + (co_scan[k] & 15), y = sy + (co_scan[k] >> 4);

            if (k == first)
                l[y * n + x] = neg_first ? -1 : 1;
            else if (k == last)
                l[y * n + x] = (int16_t)(1 + par_bump);
            else
                l[y * n + x] = (int16_t)(rnd() & 1 ? 0 : 3);
        }
    }
}

static int check_sbh_neon(void)
{
    int bad = 0;

    for (int s = 0; s < N_TSIZE; s++)
        for (int k = 0; k < 3; k++)
            for (int kind = 0; kind < N_LKIND; kind++)
                check_sbh_window(tsizes[s], k, kind);

    for (int s = 0; s < N_TSIZE && !bad; s++) {
        int log2n = tsizes[s], n = 1 << log2n, nn = n * n;
        size_t lb = (size_t)nn * sizeof(int16_t);
        int16_t *level = ca_guard_alloc(lb);
        int16_t seed[32 * 32], ref[32 * 32];

        for (int k = 0; k < 3 && !bad; k++) {
            /* N_LKIND generic populations, then the span/parity sweep */
            int cases = N_LKIND + 4 * 2 * 2;

            for (int ci = 0; ci < cases && !bad; ci++) {
                int reps = ci < N_LKIND ? 24 : 8;

                for (int rep = 0; rep < reps && !bad; rep++) {
                    if (ci < N_LKIND) {
                        fill_level(seed, n, ci);
                    } else {
                        int t = ci - N_LKIND;

                        fill_span(seed, n, log2n, k, 2 + (t >> 2),
                                  (t >> 1) & 1, t & 1);
                    }
                    memcpy(ref, seed, lb);
                    n265_sbh_adjust_ref(ref, log2n, k);

                    memcpy(level, seed, lb);
                    ca_guard_poison(level, lb);
                    CA_SIMD(n265_sbh_adjust)(level, log2n, k);
                    bad += ca_guard_check(level, lb, "sbh_adjust_neon level");
                    if (memcmp(ref, level, lb)) {
                        int at = -1;

                        for (int i = 0; i < nn; i++)
                            if (ref[i] != level[i]) { at = i; break; }
                        printf("    sbh_adjust MISMATCH %dx%d scan %d case "
                               "%d rep %d: (%d,%d) ref %d simd %d\n", n, n, k,
                               ci, rep, at % n, at / n, ref[at], level[at]);
                        bad++;
                    }
                }
            }
        }
        ca_guard_free(level);
    }
    return bad;
}

#endif /* CA_SIMD_COST */

int check_cost(void)
{
    struct cctx c;
    int bad = 0;

    n265_residual_tables_init();
    bad += check_sig_pat();
#if CA_SIMD_COST
    bad += check_csbf_neon();
    bad += check_group_neon();
    bad += check_sbh_neon();
#endif
    if (bad || !ca_bench)
        return bad;

    for (int s = 0; s < 4; s++) {
        char what[64];
        c.log2n = 2 + s;
        c.scan_idx = 0;
        /* the survey's population: mostly-empty groups with a sparse
         * low-frequency corner, which is what the encoder feeds it */
        fill_level(c.level, 1 << c.log2n, 2);
        snprintf(what, sizeof(what), "csbf_map %dx%d",
                 1 << c.log2n, 1 << c.log2n);
        bench(what, csbf_ref_call, csbf_ref_call, CSBF_NEON, &c, 20000);
    }
    for (int k = 0; k < 3; k++) {
        static const char *sn[] = { "diag", "horiz", "vert" };
        char what[64];
        c.log2n = 4;
        c.scan_idx = k;
        fill_level(c.level, 16, 3);
        snprintf(what, sizeof(what), "coeff_group %s", sn[k]);
        bench(what, group_ref_call, group_ref_call, GROUP_NEON, &c, 40000);
    }
    for (int s = 0; s < 4; s++) {
        char what[64];
        c.log2n = 2 + s;
        c.scan_idx = 0;
        /* the encoder's population: a sparse low-frequency corner, which
         * is what survives RDOQ */
        fill_level(c.seed, 1 << c.log2n, 2);
        snprintf(what, sizeof(what), "sbh_adjust %dx%d",
                 1 << c.log2n, 1 << c.log2n);
        bench(what, sbh_ref_call, sbh_ref_call, SBH_NEON, &c, 20000);
    }
    return bad;
}
