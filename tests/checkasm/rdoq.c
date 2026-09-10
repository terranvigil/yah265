/*
 * checkasm/rdoq.c - the elementwise half of the RDOQ lattice
 * (class N265_ASM_QUANT). See checkasm.h for the harness and the
 * convention; src/encoder/arm/rdoq_neon.c for what the kernels compute
 * and why the vector arithmetic is the C's.
 *
 * Two kernels, and between them three outputs that all have to match bit
 * for bit: the int16 level block, the sub-block occupancy word, and the
 * all-zero energy, which is a DOUBLE. The energy is compared with memcmp
 * of the bit pattern rather than with ==, because "within rounding" is not
 * the claim being made here - the vector form reduces pairwise where the C
 * sums in raster order, and the test is that the two produce the same 64
 * bits, which they do because every partial sum is an exact integer below
 * 2^53.
 *
 * Copyright (c) 2026, the yah265 authors. GPL-2.0-or-later; see LICENSE.
 */

#include "checkasm.h"

#include "encoder/rdoq.h"
#include "encoder/arm/neon.h"   /* self-guarded; the family kernels land here */

/* rdoq.c's own derivation of the quantiser parameters, which is what the
 * kernel's range proof is stated against. Kept here rather than exported
 * because it is the CALLER's arithmetic: the kernel takes the results. */
static const int ca_quant_scale[6] = {
    26214, 23302, 20560, 18396, 16384, 14564
};

struct qparm { int q, qbits, czero; int64_t add; };

static struct qparm qparm(int log2n, int qp, int intra)
{
    struct qparm p;
    p.qbits = 21 + qp / 6 - log2n;
    p.add = ((int64_t)1 << p.qbits) / (intra ? 3 : 6);
    p.q = ca_quant_scale[qp % 6];
    p.czero = (int)((((int64_t)1 << p.qbits) - p.add + p.q - 1) / p.q);
    return p;
}

/* Coefficient populations. The kernel's paths do not branch on the value,
 * but the three OUTPUTS do: `sparse` is what decides whether a sub-block
 * bit is set, and `extreme` is where the int32 range proof is tightest
 * (INT16_MIN has no positive int16, and the C's `-coef[i]` promotes to
 * int, so +32768 is a value the abs must produce). */
enum { CK_DENSE, CK_SPARSE, CK_DC, CK_EXTREME, CK_ZERO, N_CKIND };
static const char *const ckind_name[N_CKIND] = {
    "dense", "sparse", "dc", "extreme", "zero"
};

static void fill_coef(int16_t *coef, int nn, int kind)
{
    for (int i = 0; i < nn; i++) {
        switch (kind) {
        case CK_DENSE:
            coef[i] = (int16_t)((int)(rnd() >> 16) - 32768);
            break;
        case CK_SPARSE:
            coef[i] = (rnd() & 15) ? 0
                                   : (int16_t)((int)(rnd() >> 20) - 2048);
            break;
        case CK_DC:
            coef[i] = i ? 0 : (int16_t)((int)(rnd() >> 17) - 16384);
            break;
        case CK_EXTREME: {
            /* the ends of int16 and the values either side of the
             * deadzone and of the 32767 clip */
            static const int16_t v[] = { -32768, 32767, -32767, 0, 1, -1,
                                         32766, -32766 };
            coef[i] = v[rnd() % (sizeof(v) / sizeof(v[0]))];
            break;
        }
        default:
            coef[i] = 0;
            break;
        }
    }
}

static void fill_level(int16_t *lv, int nn, int kind)
{
    fill_coef(lv, nn, kind);
}

#if N265_NEON

/* ---- correctness ---------------------------------------------------- */

static int cmp_rtn(const int16_t *ref, const int16_t *got, int nn,
                   const char *shape, const char *kind, int qp, int intra)
{
    for (int i = 0; i < nn; i++)
        if (ref[i] != got[i]) {
            printf("    rdoq_rtn MISMATCH %s / %s qp %d intra %d at %d: "
                   "ref %d neon %d\n", shape, kind, qp, intra, i,
                   ref[i], got[i]);
            return 1;
        }
    return 0;
}

/* The declared windows are exactly n*n int16 on both buffers, with no
 * stride and no spare row, so an eight-lane load run once too far faults
 * whichever end it runs off. Four qps rather than all 52: qbits steps with
 * qp / 6 and the scale with qp % 6, and 51 is where the shift is widest -
 * the GEOMETRY the mapping checks does not move with the value. */
static void check_rtn_window(int log2n, int kind, int qp)
{
    int n = 1 << log2n, nn = n * n;
    size_t bytes = (size_t)nn * sizeof(int16_t);

    for (int tail = 0; tail < 2; tail++) {
        for (int intra = 0; intra < 2; intra++) {
            struct qparm p = qparm(log2n, qp, intra);
            ca_pg gc, gr;
            int16_t *coef = ca_pg_alloc(&gc, bytes, tail);
            int16_t *rtn = ca_pg_alloc(&gr, bytes, tail);
            double az;
            uint64_t m;
            char what[96];

            fill_coef(coef, nn, kind);
            snprintf(what, sizeof(what),
                     "rdoq_rtn %dx%d / %s qp %d intra %d, %s edge", n, n,
                     ckind_name[kind], qp, intra, tail ? "far" : "near");
            ca_pg_arm(what);
            n265_rdoq_rtn_neon(rtn, &az, &m, coef, log2n, p.q, p.add,
                               p.qbits, p.czero);
            ca_pg_disarm();

            snprintf(what, sizeof(what), "rdoq_sb_nz %dx%d / %s, %s edge",
                     n, n, ckind_name[kind], tail ? "far" : "near");
            ca_pg_arm(what);
            (void)n265_rdoq_sb_nz_neon(rtn, log2n);
            ca_pg_disarm();
            ca_pg_free(&gc);
            ca_pg_free(&gr);
        }
    }
}

static int check_rdoq_neon(void)
{
    int bad = 0;
    static const int pg_qp[] = { 0, 4, 30, 51 };

    for (int log2n = 2; log2n <= 5; log2n++)
        for (int kind = 0; kind < N_CKIND; kind++)
            for (unsigned k = 0; k < sizeof(pg_qp) / sizeof(pg_qp[0]); k++)
                check_rtn_window(log2n, kind, pg_qp[k]);

    for (int log2n = 2; log2n <= 5 && !bad; log2n++) {
        int n = 1 << log2n, nn = n * n;
        size_t bytes = (size_t)nn * sizeof(int16_t);
        int16_t *coef = ca_guard_alloc(bytes);
        int16_t *rtn = ca_guard_alloc(bytes);
        int16_t ref[32 * 32];
        char shape[16];

        snprintf(shape, sizeof(shape), "%dx%d", n, n);
        for (int kind = 0; kind < N_CKIND && !bad; kind++) {
            /* ---- rdoq_rtn: every qp the encoder can hand it ---- */
            for (int qp = 0; qp <= 51 && !bad; qp++) {
                for (int intra = 0; intra < 2 && !bad; intra++) {
                    struct qparm p = qparm(log2n, qp, intra);
                    double raz = 0, gaz = 0;
                    uint64_t rm = 0, gm = 0;
                    int rany, gany;

                    fill_coef(coef, nn, kind);
                    rany = n265_rdoq_rtn_ref(ref, &raz, &rm, coef, log2n,
                                             p.q, p.add, p.qbits, p.czero);
                    ca_guard_poison(rtn, bytes);
                    ca_guard_scramble(coef, bytes);
                    gany = n265_rdoq_rtn_neon(rtn, &gaz, &gm, coef, log2n,
                                              p.q, p.add, p.qbits,
                                              p.czero);
                    bad += ca_guard_check(rtn, bytes, "rdoq_rtn_neon rtn");
                    bad += cmp_rtn(ref, rtn, nn, shape, ckind_name[kind],
                                   qp, intra);
                    if (memcmp(&raz, &gaz, sizeof(raz))) {
                        printf("    rdoq_rtn ENERGY %s / %s qp %d intra "
                               "%d: ref %.17g neon %.17g\n", shape,
                               ckind_name[kind], qp, intra, raz, gaz);
                        bad++;
                    }
                    if (rm != gm || rany != gany) {
                        printf("    rdoq_rtn SBNZ %s / %s qp %d intra %d: "
                               "ref %llx/%d neon %llx/%d\n", shape,
                               ckind_name[kind], qp, intra,
                               (unsigned long long)rm, rany,
                               (unsigned long long)gm, gany);
                        bad++;
                    }
                }
            }
            /* ---- rdoq_sb_nz, on level blocks of the same populations,
             * repeated so a sparse draw covers many occupancy words ---- */
            for (int rep = 0; rep < 64 && !bad; rep++) {
                uint64_t rm, gm;

                fill_level(coef, nn, kind);
                rm = n265_rdoq_sb_nz_ref(coef, log2n);
                ca_guard_scramble(coef, bytes);
                gm = n265_rdoq_sb_nz_neon(coef, log2n);
                if (rm != gm) {
                    printf("    rdoq_sb_nz MISMATCH %s / %s: ref %llx "
                           "neon %llx\n", shape, ckind_name[kind],
                           (unsigned long long)rm,
                           (unsigned long long)gm);
                    bad++;
                }
            }
        }
        ca_guard_free(coef);
        ca_guard_free(rtn);
    }
    return bad;
}
#endif /* N265_NEON */

/* ---- bench ---------------------------------------------------------- */

struct rctx {
    int log2n;
    struct qparm p;
    int16_t coef[32 * 32], rtn[32 * 32];
    double az;
    uint64_t m;
};

static void rtn_c_call(void *v)
{
    struct rctx *c = v;
    n265_rdoq_rtn_ref(c->rtn, &c->az, &c->m, c->coef, c->log2n, c->p.q,
                      c->p.add, c->p.qbits, c->p.czero);
}

static void sbnz_c_call(void *v)
{
    struct rctx *c = v;
    c->m = n265_rdoq_sb_nz_ref(c->coef, c->log2n);
}

#if N265_NEON
static void rtn_neon_call(void *v)
{
    struct rctx *c = v;
    n265_rdoq_rtn_neon(c->rtn, &c->az, &c->m, c->coef, c->log2n, c->p.q,
                       c->p.add, c->p.qbits, c->p.czero);
}

static void sbnz_neon_call(void *v)
{
    struct rctx *c = v;
    c->m = n265_rdoq_sb_nz_neon(c->coef, c->log2n);
}
#define RTN_NEON  rtn_neon_call
#define SBNZ_NEON sbnz_neon_call
#else
#define RTN_NEON  NULL
#define SBNZ_NEON NULL
#endif

int check_rdoq(void)
{
    int bad = 0;
    static struct rctx c;

    /* the C form against itself first: rdoq.c's pass 1 is the entry point
     * for both, and a reference that disagreed with the shape the encoder
     * hands it would be caught here rather than in the NEON column */
    for (int log2n = 2; log2n <= 5 && !bad; log2n++) {
        int n = 1 << log2n, nn = n * n;
        int16_t rtn[32 * 32];
        double az;
        uint64_t m;

        for (int kind = 0; kind < N_CKIND && !bad; kind++) {
            struct qparm p = qparm(log2n, 30, 1);

            fill_coef(c.coef, nn, kind);
            if (n265_rdoq_rtn_ref(rtn, &az, &m, c.coef, log2n, p.q, p.add,
                                  p.qbits, p.czero)
                != (m != 0)) {
                printf("    rdoq_rtn_ref ANY disagrees with the mask "
                       "(%dx%d / %s)\n", n, n, ckind_name[kind]);
                bad++;
            }
            if (m != n265_rdoq_sb_nz_ref(rtn, log2n)) {
                printf("    rdoq_rtn_ref mask != sb_nz_ref(rtn) "
                       "(%dx%d / %s)\n", n, n, ckind_name[kind]);
                bad++;
            }
        }
    }
#if N265_NEON
    bad += check_rdoq_neon();
#endif
    if (bad)
        return bad;

    /* Both kernels have no C fast path distinct from their reference, so
     * the first two bench columns are the same function - the C the
     * dispatch falls back to - and the multiple is the number that
     * matters. Two populations: `dense` is the worst case for the
     * occupancy word (every bit set), `sparse` is what a real block at a
     * usable QP looks like, and neither changes the kernel's path. */
    for (int log2n = 2; log2n <= 5; log2n++) {
        int n = 1 << log2n, nn = n * n;
        char name[40];

        c.log2n = log2n;
        c.p = qparm(log2n, 30, 1);
        fill_coef(c.coef, nn, CK_DENSE);
        snprintf(name, sizeof(name), "rdoq_rtn %dx%d dense", n, n);
        bench(name, rtn_c_call, rtn_c_call, RTN_NEON, &c, 20000);
        snprintf(name, sizeof(name), "rdoq_sb_nz %dx%d dense", n, n);
        bench(name, sbnz_c_call, sbnz_c_call, SBNZ_NEON, &c, 20000);
        fill_coef(c.coef, nn, CK_SPARSE);
        snprintf(name, sizeof(name), "rdoq_rtn %dx%d sparse", n, n);
        bench(name, rtn_c_call, rtn_c_call, RTN_NEON, &c, 20000);
        snprintf(name, sizeof(name), "rdoq_sb_nz %dx%d sparse", n, n);
        bench(name, sbnz_c_call, sbnz_c_call, SBNZ_NEON, &c, 20000);
    }
    return 0;
}
