/*
 * checkasm/transform.c - the fdct/idct family (class N265_ASM_DCT).
 * Split out of checkasm.c unchanged on 2026-09-10; see checkasm.h for the
 * harness and the convention.
 * Copyright (c) 2026, the yah265 authors. GPL-2.0-or-later; see LICENSE.
 */

#include "checkasm.h"

#include "encoder/transform.h"
#include "encoder/arm/neon.h"   /* self-guarded; the family kernels land here */

/* ---- transforms ---------------------------------------------------- */

struct tctx {
    int log2n, use_dst, nzw, nzh;
    int16_t resid[32 * 32], coef[32 * 32], dq[32 * 32];
    uint8_t pred[32 * 32], dst[32 * 32];
};

static void fdct_ref_call(void *p)
{
    struct tctx *c = p;
    n265_fdct_ref(c->coef, c->resid, c->log2n, c->use_dst);
}

static void fdct_opt_call(void *p)
{
    struct tctx *c = p;
    n265_fdct(c->coef, c->resid, c->log2n, c->use_dst);
}

static void idct_ref_call(void *p)
{
    struct tctx *c = p;
    n265_idct_add_ref(c->dst, 32, c->dq, c->log2n, c->pred, 32, c->use_dst);
}

static void idct_opt_call(void *p)
{
    struct tctx *c = p;
    n265_idct_add(c->dst, 32, c->dq, c->log2n, c->pred, 32, c->use_dst,
                  c->nzw, c->nzh);
}

struct qctx {
    int log2n, qp, intra;
    int16_t coef[32 * 32], level[32 * 32], dq[32 * 32];
    int nzw, nzh, nz;
};

static void dequant_c_call(void *p)
{
    struct qctx *c = p;
    n265_dequant_ref(c->dq, c->level, c->log2n, c->qp, &c->nzw, &c->nzh);
}

static void quant_c_call(void *p)
{
    struct qctx *c = p;
    c->nz = n265_quant_ref(c->level, c->coef, c->log2n, c->qp, c->intra);
}

#if N265_NEON
static void dequant_neon_call(void *p)
{
    struct qctx *c = p;
    n265_dequant_neon(c->dq, c->level, c->log2n, c->qp, &c->nzw, &c->nzh);
}

static void quant_neon_call(void *p)
{
    struct qctx *c = p;
    c->nz = n265_quant_neon(c->level, c->coef, c->log2n, c->qp, c->intra);
}
#define DEQ_NEON dequant_neon_call
#define QNT_NEON quant_neon_call
#else
#define DEQ_NEON NULL
#define QNT_NEON NULL
#endif

#if N265_NEON
static void fdct_neon_call(void *p)
{
    struct tctx *c = p;
    n265_fdct_neon(c->coef, c->resid, c->log2n, c->use_dst);
}

static void idct_neon_call(void *p)
{
    struct tctx *c = p;
    n265_idct_add_neon(c->dst, 32, c->dq, c->log2n, c->pred, 32, c->use_dst,
                       c->nzw, c->nzh);
}
#define FDCT_NEON fdct_neon_call
#define IDCT_NEON idct_neon_call
#else
#define FDCT_NEON NULL
#define IDCT_NEON NULL
#endif

/* the bounding box n265_dequant would have reported */
static void dq_bbox(const int16_t *dq, int n, int *nzw, int *nzh)
{
    int mx = 0, my = 0;
    for (int y = 0; y < n; y++)
        for (int x = 0; x < n; x++)
            if (dq[y * n + x]) {
                if (x > mx) mx = x;
                if (y > my) my = y;
            }
    *nzw = mx + 1;
    *nzh = my + 1;
}

/* residual/coefficient generators: uniform noise, flat (DC-heavy), the
 * extremes the shift bounds are proved against, and sparse blocks with a
 * single nonzero, which is the case the zero-aware inverse special-cases */
static void fill_resid(int16_t *r, int nn, int kind)
{
    for (int i = 0; i < nn; i++) {
        switch (kind) {
        case 0: r[i] = (int16_t)((int)(rnd() >> 23) - 255); break;
        case 1: r[i] = (int16_t)(200 + (int)(rnd() & 55)); break;
        case 2: r[i] = (int16_t)((rnd() & 1) ? 255 : -255); break;
        default: r[i] = 0; break;
        }
    }
    if (kind == 3)
        r[rnd() % (unsigned)nn] = (int16_t)((int)(rnd() >> 23) - 255);
}

static void fill_dq(int16_t *d, int nn, int kind)
{
    for (int i = 0; i < nn; i++) {
        switch (kind) {
        case 0: d[i] = (int16_t)((int)(rnd() >> 20) - 2048); break;
        case 1: d[i] = (int16_t)((rnd() & 1) ? 32767 : -32768); break;
        default: d[i] = 0; break;
        }
    }
    if (kind == 2)
        d[0] = (int16_t)((int)(rnd() >> 18) - 8192);
    if (kind == 3) {
        int p = (int)(rnd() % (unsigned)nn);
        d[p] = (int16_t)((int)(rnd() >> 18) - 8192);
    }
}

#if N265_NEON

/* ---- the NEON leg ---------------------------------------------------
 *
 * n265_x_ref against n265_x_neon DIRECTLY: never through the dispatching
 * entry point, which N265_ASM_OFF can turn into the C path underneath the
 * harness. Every optimized call runs in a guard buffer, and every input
 * buffer is guarded too, so a load that grabbed four samples where one
 * was declared is caught at the block edge and not only at the far end of
 * a page.
 *
 * The shape table is the ENCODER's, from the 2026-09-10 survey in
 * src/encoder/arm/neon.h: five shapes, no more, and no rectangles. What
 * varies instead is the idct's nonzero bound, which the survey found in
 * its bounded form on the majority of calls, so every dq case is run
 * three ways: at the true bounding box, at the full block, and at a bound
 * between the two.
 */

static const struct { int log2n, use_dst; const char *name; } tshapes[] = {
    { 2, 0, "4x4" }, { 2, 1, "4x4-dst" }, { 3, 0, "8x8" },
    { 4, 0, "16x16" }, { 5, 0, "32x32" },
};

/* Residual kinds. 0-3 are the ones the C-fast leg above uses; 4-6 are the
 * adversarial ends of the domain (the residual is src - pred on 8-bit
 * samples, so |r| <= 255 and the extremes are exactly +-255). */
static const char *rkind_name[] = {
    "random", "flat-bright", "alternating +-255", "sparse",
    "all +255", "all -255", "column ramp",
};
#define N_RKIND ((int)(sizeof(rkind_name) / sizeof(rkind_name[0])))

static void fill_resid_ex(int16_t *r, int n, int kind)
{
    int nn = n * n;

    if (kind < 4) {
        fill_resid(r, nn, kind);
        return;
    }
    if (kind == 4 || kind == 5) {
        int16_t v = kind == 4 ? 255 : -255;
        for (int i = 0; i < nn; i++)
            r[i] = v;
        return;
    }
    for (int y = 0; y < n; y++)
        for (int x = 0; x < n; x++)
            r[y * n + x] = (int16_t)(((x + y) & 1) ? 255 - 16 * x
                                                  : -255 + 16 * y);
}

/* Coefficient kinds. dq comes out of n265_dequant already clipped to
 * int16, so the extremes are the full int16 ends and the kernel has to
 * hold them: that is what the stage-1 clip and the int32 accumulators are
 * proved against. */
static const char *ckind_name[] = {
    "random", "alternating int16 ends", "dc only", "sparse",
    "all +32767", "all -32768", "low-frequency corner", "empty",
};
#define N_CKIND ((int)(sizeof(ckind_name) / sizeof(ckind_name[0])))

static void fill_dq_ex(int16_t *d, int n, int kind)
{
    int nn = n * n;

    if (kind < 4) {
        fill_dq(d, nn, kind);
        return;
    }
    for (int i = 0; i < nn; i++)
        d[i] = 0;
    if (kind == 4 || kind == 5) {
        int16_t v = kind == 4 ? 32767 : -32768;
        for (int i = 0; i < nn; i++)
            d[i] = v;
    } else if (kind == 6) {
        int c = n < 4 ? n : 4;
        for (int y = 0; y < c; y++)
            for (int x = 0; x < c; x++)
                d[y * n + x] = (int16_t)((int)(rnd() >> 20) - 2048);
    }
}

static int cmp16(const int16_t *ref, const int16_t *got, int nn,
                 const char *what, const char *shape, const char *kind)
{
    for (int i = 0; i < nn; i++)
        if (ref[i] != got[i]) {
            printf("    %s MISMATCH %s / %s: [%d] ref %d neon %d\n",
                   what, shape, kind, i, ref[i], got[i]);
            return 1;
        }
    return 0;
}

static int cmp8(const uint8_t *ref, const uint8_t *got, int stride, int n,
                const char *what, const char *shape, const char *kind,
                int nzw, int nzh)
{
    for (int y = 0; y < n; y++)
        for (int x = 0; x < n; x++)
            if (ref[y * stride + x] != got[y * stride + x]) {
                printf("    %s MISMATCH %s / %s (nzw %d nzh %d): (%d,%d) "
                       "ref %d neon %d\n", what, shape, kind, nzw, nzh, x, y,
                       ref[y * stride + x], got[y * stride + x]);
                return 1;
            }
    return 0;
}

static int check_fdct_neon(void)
{
    int bad = 0;

    for (unsigned s = 0; s < sizeof(tshapes) / sizeof(tshapes[0]); s++) {
        int log2n = tshapes[s].log2n, use_dst = tshapes[s].use_dst;
        int n = 1 << log2n, nn = n * n;
        size_t rb = (size_t)nn * sizeof(int16_t);
        int16_t *resid = ca_guard_alloc(rb);
        int16_t *coef = ca_guard_alloc(rb);
        int16_t ref[32 * 32], first[32 * 32];

        for (int kind = 0; kind < N_RKIND && !bad; kind++) {
            int reps = kind == 0 || kind == 3 ? 48 : 4;

            for (int rep = 0; rep < reps; rep++) {
                fill_resid_ex(resid, n, kind);
                n265_fdct_ref(ref, resid, log2n, use_dst);

                ca_guard_poison(coef, rb);
                ca_guard_scramble(resid, rb);
                n265_fdct_neon(coef, resid, log2n, use_dst);
                bad += ca_guard_check(coef, rb, "fdct_neon coef");
                bad += cmp16(ref, coef, nn, "fdct", tshapes[s].name,
                             rkind_name[kind]);
                memcpy(first, coef, rb);

                /* read window: the same block with a different border must
                 * give the same answer, or the kernel loaded outside it */
                ca_guard_scramble(resid, rb);
                memset(coef, 0, rb);
                n265_fdct_neon(coef, resid, log2n, use_dst);
                if (memcmp(first, coef, rb)) {
                    printf("    fdct_neon %s / %s: READ past the block "
                           "(border changed the result)\n", tshapes[s].name,
                           rkind_name[kind]);
                    bad++;
                }
                if (bad)
                    break;
            }
        }
        ca_guard_free(resid);
        ca_guard_free(coef);
        if (bad)
            return bad;
    }
    return bad;
}

/* Levels as the scan produces them, plus the ends of the int16 domain the
 * dequant's clip is proved against: a level near +-32767 with qp 51 is
 * what drives dq past the clip, and an all-zero block is what the
 * bounding box has to report as 1x1 rather than 0x0. */
static const char *lkind_name[] = {
    "random small", "random full-range", "sparse", "empty",
    "all +32767", "all -32768", "low-frequency corner", "one at the corner",
};
#define N_LKIND ((int)(sizeof(lkind_name) / sizeof(lkind_name[0])))

static void fill_level(int16_t *l, int n, int kind)
{
    int nn = n * n;

    for (int i = 0; i < nn; i++)
        l[i] = 0;
    switch (kind) {
    case 0:
        for (int i = 0; i < nn; i++)
            l[i] = (int16_t)((int)(rnd() >> 26) - 32);
        break;
    case 1:
        for (int i = 0; i < nn; i++)
            l[i] = (int16_t)(rnd() >> 16);
        break;
    case 2:
        for (int k = 0; k < 5; k++)
            l[rnd() % (unsigned)nn] = (int16_t)((int)(rnd() >> 20) - 2048);
        break;
    case 3:
        break;
    case 4:
    case 5:
        for (int i = 0; i < nn; i++)
            l[i] = kind == 4 ? 32767 : (int16_t)-32768;
        break;
    case 6: {
        int c = n < 4 ? n : 4;
        for (int y = 0; y < c; y++)
            for (int x = 0; x < c; x++)
                l[y * n + x] = (int16_t)((int)(rnd() >> 24) - 128);
        break;
    }
    default:
        l[nn - 1] = (int16_t)((int)(rnd() >> 24) - 128);
        break;
    }
}

/* Every qp the encoder can hand these, not a sample: the deadzone add and
 * the dequant scale both step with qp % 6 and qp / 6, and 51 is where the
 * shift and the clip are closest to their bounds. */
static int check_quant_neon(void)
{
    int bad = 0;

    for (int log2n = 2; log2n <= 5 && !bad; log2n++) {
        int n = 1 << log2n, nn = n * n;
        size_t bytes = (size_t)nn * sizeof(int16_t);
        int16_t *level = ca_guard_alloc(bytes);
        int16_t *dq = ca_guard_alloc(bytes);
        int16_t *coef = ca_guard_alloc(bytes);
        int16_t ref[32 * 32];
        char shape[16];

        snprintf(shape, sizeof(shape), "%dx%d", n, n);
        for (int kind = 0; kind < N_LKIND && !bad; kind++) {
            for (int qp = 0; qp <= 51 && !bad; qp++) {
                int rw = 0, rh = 0, gw = 0, gh = 0, rn, gn;

                /* ---- dequant ---- */
                fill_level(level, n, kind);
                n265_dequant_ref(ref, level, log2n, qp, &rw, &rh);
                ca_guard_poison(dq, bytes);
                ca_guard_scramble(level, bytes);
                n265_dequant_neon(dq, level, log2n, qp, &gw, &gh);
                bad += ca_guard_check(dq, bytes, "dequant_neon dq");
                bad += cmp16(ref, dq, nn, "dequant", shape,
                             lkind_name[kind]);
                if (rw != gw || rh != gh) {
                    printf("    dequant BOX %s / %s qp %d: ref %dx%d "
                           "neon %dx%d\n", shape, lkind_name[kind], qp,
                           rw, rh, gw, gh);
                    bad++;
                }
                /* the NULL-pointer arm the header allows */
                n265_dequant_neon(dq, level, log2n, qp, NULL, NULL);

                /* ---- quant ---- */
                for (int i = 0; i < nn; i++)
                    coef[i] = level[i];
                for (int intra = 0; intra < 2 && !bad; intra++) {
                    rn = n265_quant_ref(ref, coef, log2n, qp, intra);
                    ca_guard_poison(level, bytes);
                    ca_guard_scramble(coef, bytes);
                    gn = n265_quant_neon(level, coef, log2n, qp, intra);
                    bad += ca_guard_check(level, bytes,
                                          "quant_neon level");
                    bad += cmp16(ref, level, nn, "quant", shape,
                                 lkind_name[kind]);
                    if (rn != gn) {
                        printf("    quant NZ %s / %s qp %d intra %d: "
                               "ref %d neon %d\n", shape, lkind_name[kind],
                               qp, intra, rn, gn);
                        bad++;
                    }
                }
            }
        }
        ca_guard_free(level);
        ca_guard_free(dq);
        ca_guard_free(coef);
    }
    return bad;
}

static int check_idct_neon(void)
{
    int bad = 0;

    for (unsigned s = 0; s < sizeof(tshapes) / sizeof(tshapes[0]); s++) {
        int log2n = tshapes[s].log2n, use_dst = tshapes[s].use_dst;
        int n = 1 << log2n, nn = n * n;
        size_t db = (size_t)nn * sizeof(int16_t);
        int16_t *dq = ca_guard_alloc(db);

        /* two stride shapes: the packed block the encoder always passes
         * (survey: n in 2.7M calls), and a padded one so ca_check_pad has
         * something to guard */
        for (int sv = 0; sv < 2; sv++) {
            int ds = sv ? n + 5 : n, ps = sv ? n + 3 : n;
            size_t dbytes = (size_t)ds * n, pbytes = (size_t)ps * n;
            uint8_t *dst = ca_guard_alloc(dbytes);
            uint8_t *pred = ca_guard_alloc(pbytes);
            uint8_t ref[37 * 32], first[37 * 32];

            for (int kind = 0; kind < N_CKIND && !bad; kind++) {
                int reps = kind == 0 || kind == 3 ? 32 : 4;

                for (int rep = 0; rep < reps && !bad; rep++) {
                    int bw, bh;

                    fill_dq_ex(dq, n, kind);
                    for (size_t i = 0; i < pbytes; i++)
                        pred[i] = (uint8_t)(rnd() >> 24);
                    dq_bbox(dq, n, &bw, &bh);
                    memset(ref, CA_POISON, sizeof(ref));
                    n265_idct_add_ref(ref, ds, dq, log2n, pred, ps, use_dst);

                    /* the true box, the loose whole-block bound, and one
                     * in between: all three have to reproduce the
                     * reference, because a caller is free to over-report */
                    for (int b = 0; b < 3; b++) {
                        int nzw = b == 0 ? bw : b == 1 ? n
                                              : (bw + n + 1) / 2;
                        int nzh = b == 0 ? bh : b == 1 ? n
                                              : (bh + n + 1) / 2;

                        memset(dst, CA_POISON, dbytes);
                        ca_guard_poison(dst, dbytes);
                        ca_guard_scramble(pred, pbytes);
                        n265_idct_add_neon(dst, ds, dq, log2n, pred, ps,
                                           use_dst, nzw, nzh);
                        bad += ca_guard_check(dst, dbytes, "idct_neon dst");
                        bad += ca_check_pad(dst, ds, n, n, "idct_neon dst");
                        bad += cmp8(ref, dst, ds, n, "idct", tshapes[s].name,
                                    ckind_name[kind], nzw, nzh);
                        if (bad)
                            break;
                        if (b)
                            continue;
                        memcpy(first, dst, dbytes);
                        ca_guard_scramble(pred, pbytes);
                        memset(dst, 0, dbytes);
                        n265_idct_add_neon(dst, ds, dq, log2n, pred, ps,
                                           use_dst, nzw, nzh);
                        for (int y = 0; y < n; y++)
                            if (memcmp(first + y * ds, dst + y * ds,
                                       (size_t)n)) {
                                printf("    idct_neon %s / %s: READ past the "
                                       "block (border changed the result)\n",
                                       tshapes[s].name, ckind_name[kind]);
                                bad++;
                                break;
                            }
                    }
                }
            }
            ca_guard_free(dst);
            ca_guard_free(pred);
            if (bad) {
                ca_guard_free(dq);
                return bad;
            }
        }
        ca_guard_free(dq);
    }
    return bad;
}
#endif /* N265_NEON */

int check_transform(void)
{
    static struct tctx c;
    int bad = 0;

    for (int log2n = 2; log2n <= 5; log2n++) {
        int n = 1 << log2n, nn = n * n;
        for (int use_dst = 0; use_dst <= (log2n == 2); use_dst++) {
            for (int kind = 0; kind < 4; kind++) {
                for (int rep = 0; rep < 64; rep++) {
                    int16_t ref[32 * 32];
                    fill_resid(c.resid, nn, kind);
                    c.log2n = log2n; c.use_dst = use_dst;
                    n265_fdct_ref(ref, c.resid, log2n, use_dst);
                    n265_fdct(c.coef, c.resid, log2n, use_dst);
                    if (memcmp(ref, c.coef, (size_t)nn * sizeof(int16_t))) {
                        printf("    fdct MISMATCH log2n %d dst %d kind %d\n",
                               log2n, use_dst, kind);
                        bad++;
                        rep = 64;
                    }
                }
                for (int rep = 0; rep < 64; rep++) {
                    uint8_t ref[32 * 32];
                    int bw, bh;
                    fill_dq(c.dq, nn, kind);
                    for (int i = 0; i < nn; i++)
                        c.pred[i] = (uint8_t)(rnd() >> 24);
                    c.log2n = log2n; c.use_dst = use_dst;
                    dq_bbox(c.dq, n, &bw, &bh);
                    n265_idct_add_ref(ref, 32, c.dq, log2n, c.pred, 32,
                                      use_dst);
                    /* both the exact bounding box and the loose full-size
                     * bound must reproduce the reference */
                    for (int b = 0; b < 2; b++) {
                        c.nzw = b ? n : bw;
                        c.nzh = b ? n : bh;
                        memset(c.dst, 0, sizeof(c.dst));
                        idct_opt_call(&c);
                        for (int y = 0; y < n; y++)
                            if (memcmp(ref + y * 32, c.dst + y * 32,
                                       (size_t)n)) {
                                printf("    idct MISMATCH log2n %d dst %d "
                                       "kind %d bbox %d\n", log2n, use_dst,
                                       kind, !b);
                                bad++;
                                rep = 64;
                                b = 2;
                                break;
                            }
                    }
                }
            }
        }
    }
#if N265_NEON
    bad += check_fdct_neon();
    bad += check_idct_neon();
    bad += check_quant_neon();
#endif
    if (bad)
        return bad;

    for (int log2n = 2; log2n <= 5; log2n++) {
        char name[32];
        int n = 1 << log2n;
        c.log2n = log2n; c.use_dst = 0;
        fill_resid(c.resid, n * n, 0);
        fill_dq(c.dq, n * n, 0);
        c.nzw = c.nzh = n;
        snprintf(name, sizeof(name), "fdct %dx%d", n, n);
        bench(name, fdct_ref_call, fdct_opt_call, FDCT_NEON, &c, 20000);
        snprintf(name, sizeof(name), "idct_add %dx%d", n, n);
        bench(name, idct_ref_call, idct_opt_call, IDCT_NEON, &c, 20000);
        /* the shape the encoder actually hands it: a small low-frequency
         * corner, and DC alone */
        fill_dq(c.dq, n * n, 4);
        for (int y = 0; y < 4 && y < n; y++)
            for (int x = 0; x < 4 && x < n; x++)
                c.dq[y * n + x] = (int16_t)((int)(rnd() >> 20) - 2048);
        dq_bbox(c.dq, n, &c.nzw, &c.nzh);
        snprintf(name, sizeof(name), "idct_add %dx%d 4x4nz", n, n);
        bench(name, idct_ref_call, idct_opt_call, IDCT_NEON, &c, 20000);
        fill_dq(c.dq, n * n, 4);
        c.dq[0] = 1234;
        c.nzw = c.nzh = 1;
        snprintf(name, sizeof(name), "idct_add %dx%d dc", n, n);
        bench(name, idct_ref_call, idct_opt_call, IDCT_NEON, &c, 20000);
    }
    c.log2n = 2; c.use_dst = 1;
    fill_resid(c.resid, 16, 0);
    fill_dq(c.dq, 16, 0);
    c.nzw = c.nzh = 4;
    bench("fdst 4x4", fdct_ref_call, fdct_opt_call, FDCT_NEON, &c, 20000);
    bench("idst_add 4x4", idct_ref_call, idct_opt_call, IDCT_NEON, &c, 20000);

    /* quant and dequant have no reference distinct from their C form, so
     * the first two columns are the same function - the C the dispatch
     * falls back to, called directly - and the multiple is the number
     * that matters (checkasm.h). Two level populations, because the C
     * dequant's early-out makes a sparse block a different kernel from a
     * dense one. */
    {
        static struct qctx q;
        char name[40];

        for (int log2n = 2; log2n <= 5; log2n++) {
            int n = 1 << log2n;

            q.log2n = log2n; q.qp = 30; q.intra = 1;
            fill_level(q.level, n, 0);
            memcpy(q.coef, q.level, sizeof(q.coef));
            snprintf(name, sizeof(name), "dequant %dx%d dense", n, n);
            bench(name, dequant_c_call, dequant_c_call, DEQ_NEON, &q, 20000);
            snprintf(name, sizeof(name), "quant %dx%d", n, n);
            bench(name, quant_c_call, quant_c_call, QNT_NEON, &q, 20000);
            fill_level(q.level, n, 2);
            memcpy(q.coef, q.level, sizeof(q.coef));
            snprintf(name, sizeof(name), "dequant %dx%d sparse", n, n);
            bench(name, dequant_c_call, dequant_c_call, DEQ_NEON, &q, 20000);
        }
    }
    return 0;
}
