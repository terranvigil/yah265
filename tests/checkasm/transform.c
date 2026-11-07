/*
 * checkasm/transform.c - the fdct/idct family (class N265_ASM_DCT).
 * Split out of checkasm.c unchanged on 2026-09-10; see checkasm.h for the
 * harness and the convention.
 * Copyright (c) 2026, the yah265 authors. GPL-2.0-or-later; see LICENSE.
 */

#include "checkasm.h"

#include "encoder/transform.h"

/* ---- transforms ---------------------------------------------------- */

struct tctx {
    int log2n, use_dst, nzw, nzh;
    int16_t resid[32 * 32], coef[32 * 32], dq[32 * 32];
    pixel pred[32 * 32], dst[32 * 32];
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

#if CA_SIMD_TRANSFORM
static void dequant_neon_call(void *p)
{
    struct qctx *c = p;
    CA_SIMD(n265_dequant)(c->dq, c->level, c->log2n, c->qp, &c->nzw, &c->nzh);
}

static void quant_neon_call(void *p)
{
    struct qctx *c = p;
    c->nz = CA_SIMD(n265_quant)(c->level, c->coef, c->log2n, c->qp, c->intra);
}
#define DEQ_NEON dequant_neon_call
#define QNT_NEON quant_neon_call
#else
#define DEQ_NEON NULL
#define QNT_NEON NULL
#endif

#if CA_SIMD_TRANSFORM
static void fdct_neon_call(void *p)
{
    struct tctx *c = p;
    CA_SIMD(n265_fdct)(c->coef, c->resid, c->log2n, c->use_dst);
}

static void idct_neon_call(void *p)
{
    struct tctx *c = p;
    CA_SIMD(n265_idct_add)(c->dst, 32, c->dq, c->log2n, c->pred, 32,
                           c->use_dst, c->nzw, c->nzh);
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
/* A residual is src - pred on samples of THIS build, so its domain is
 * +-PIXEL_MAX: 255 at 8 bits and 1023 at Main10, which is what the
 * forward transform's lane-headroom argument is written against. Each
 * expression below folds to the 8-bit literal it replaced and draws the
 * same number of words from the stream, so the 8-bit harness tests
 * exactly what it tested before. */
static int16_t ca_resid(void)
{
    return (int16_t)((int)(rnd() >> (31 - BIT_DEPTH)) - PIXEL_MAX);
}

static void fill_resid(int16_t *r, int nn, int kind)
{
    for (int i = 0; i < nn; i++) {
        switch (kind) {
        case 0: r[i] = ca_resid(); break;
        case 1: r[i] = (int16_t)(PIXEL_MAX - 55 + (int)(rnd() & 55)); break;
        case 2: r[i] = (int16_t)((rnd() & 1) ? PIXEL_MAX : -PIXEL_MAX); break;
        default: r[i] = 0; break;
        }
    }
    if (kind == 3)
        r[rnd() % (unsigned)nn] = ca_resid();
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

#if CA_SIMD_TRANSFORM

/* ---- the NEON leg ---------------------------------------------------
 *
 * n265_x_ref against n265_x_neon DIRECTLY: never through the dispatching
 * entry point, which N265_ASM_OFF can turn into the C path underneath the
 * harness. Every optimized call runs in a guard buffer, and every input
 * buffer is guarded too, so a load that grabbed four samples where one
 * was declared is caught at the block edge and not only at the far end of
 * a page. The read whose value is DISCARDED gets past both of those, so
 * every kernel here also gets a page-guard pass (checkasm.h): the same
 * call against a mapping sized to its declared window with an unmapped
 * page hard against it.
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
 * adversarial ends of the domain (the residual is src - pred on this
 * build's samples, so |r| <= PIXEL_MAX and the extremes are exactly
 * +-PIXEL_MAX). */
static const char *rkind_name[] = {
    "random", "flat-bright", "alternating +-max", "sparse",
    "all +max", "all -max", "column ramp",
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
        int16_t v = (int16_t)(kind == 4 ? PIXEL_MAX : -PIXEL_MAX);
        for (int i = 0; i < nn; i++)
            r[i] = v;
        return;
    }
    for (int y = 0; y < n; y++)
        for (int x = 0; x < n; x++) {
            int step = 16 << BIT_DEPTH_SHIFT;
            r[y * n + x] = (int16_t)(((x + y) & 1) ? PIXEL_MAX - step * x
                                                   : -PIXEL_MAX + step * y);
        }
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
            printf("    %s MISMATCH %s / %s: [%d] ref %d simd %d\n",
                   what, shape, kind, i, ref[i], got[i]);
            return 1;
        }
    return 0;
}

static int cmp_pix(const pixel *ref, const pixel *got, int stride, int n,
                   const char *what, const char *shape, const char *kind,
                   int nzw, int nzh)
{
    for (int y = 0; y < n; y++)
        for (int x = 0; x < n; x++)
            if (ref[y * stride + x] != got[y * stride + x]) {
                printf("    %s MISMATCH %s / %s (nzw %d nzh %d): (%d,%d) "
                       "ref %d simd %d\n", what, shape, kind, nzw, nzh, x, y,
                       ref[y * stride + x], got[y * stride + x]);
                return 1;
            }
    return 0;
}

/* fdct's declared windows are exactly n*n int16 each way: it reads the
 * residual block and writes the coefficient block, with no stride and no
 * halo anywhere in the signature. So the mapping IS the block, on the
 * input and on the output both - the output because a store one vector
 * too far past a 4x4 is the same clang widening in the other direction. */
static void check_fdct_window(int log2n, int use_dst, int kind)
{
    int n = 1 << log2n;
    size_t rb = (size_t)n * n * sizeof(int16_t);

    for (int tail = 0; tail < 2; tail++) {
        ca_pg gr, gc;
        int16_t *resid = ca_pg_alloc(&gr, rb, tail);
        int16_t *coef = ca_pg_alloc(&gc, rb, tail);
        char what[96];

        fill_resid_ex(resid, n, kind);
        snprintf(what, sizeof(what), "fdct %dx%d%s / %s, %s edge", n, n,
                 use_dst ? "-dst" : "", rkind_name[kind],
                 tail ? "far" : "near");
        ca_pg_arm(what);
        CA_SIMD(n265_fdct)(coef, resid, log2n, use_dst);
        ca_pg_disarm();
        ca_pg_free(&gr);
        ca_pg_free(&gc);
    }
}

static int check_fdct_neon(void)
{
    int bad = 0;

    for (unsigned s = 0; s < sizeof(tshapes) / sizeof(tshapes[0]); s++)
        for (int kind = 0; kind < N_RKIND; kind++)
            check_fdct_window(tshapes[s].log2n, tshapes[s].use_dst, kind);

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
                CA_SIMD(n265_fdct)(coef, resid, log2n, use_dst);
                bad += ca_guard_check(coef, rb, "fdct_neon coef");
                bad += cmp16(ref, coef, nn, "fdct", tshapes[s].name,
                             rkind_name[kind]);
                memcpy(first, coef, rb);

                /* read window: the same block with a different border must
                 * give the same answer, or the kernel loaded outside it */
                ca_guard_scramble(resid, rb);
                memset(coef, 0, rb);
                CA_SIMD(n265_fdct)(coef, resid, log2n, use_dst);
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

/* dequant and quant are the same shape as fdct - n*n in, n*n out, no
 * stride - so the mapping is the block on all four buffers. The level
 * POPULATION is what varies here rather than the geometry: the dequant's
 * early-out makes a sparse block a different path from a dense one, and a
 * path that is only taken on sparse input is a path whose tail has never
 * been page-guarded. Four qps rather than all 52: the scale steps with
 * qp % 6 and qp / 6, the shift is tightest at 51, and the geometry the
 * mapping is checking does not move with the value. At Main10 QpY starts
 * at -QpBdOffsetY = -12, so N265_QP_MIN is in the list too: it is the
 * value that only exists at this depth and the one a table left on the
 * bare QP indexes out of bounds. */
static void check_quant_window(int log2n, int kind, int qp)
{
    int n = 1 << log2n;
    size_t bytes = (size_t)n * n * sizeof(int16_t);

    for (int tail = 0; tail < 2; tail++) {
        ca_pg gl, gd, gc;
        int16_t *level = ca_pg_alloc(&gl, bytes, tail);
        int16_t *dq = ca_pg_alloc(&gd, bytes, tail);
        int16_t *coef = ca_pg_alloc(&gc, bytes, tail);
        int nzw, nzh;
        char what[96];

        fill_level(level, n, kind);
        memcpy(coef, level, bytes);
        snprintf(what, sizeof(what), "dequant %dx%d / %s qp %d, %s edge",
                 n, n, lkind_name[kind], qp, tail ? "far" : "near");
        ca_pg_arm(what);
        CA_SIMD(n265_dequant)(dq, level, log2n, qp, &nzw, &nzh);
        CA_SIMD(n265_dequant)(dq, level, log2n, qp, NULL, NULL);
        ca_pg_disarm();

        snprintf(what, sizeof(what), "quant %dx%d / %s qp %d, %s edge",
                 n, n, lkind_name[kind], qp, tail ? "far" : "near");
        ca_pg_arm(what);
        for (int intra = 0; intra < 3; intra++)   /* 2: N265_CDZ=2 */
            CA_SIMD(n265_quant)(level, coef, log2n, qp, intra);
        ca_pg_disarm();
        ca_pg_free(&gl);
        ca_pg_free(&gd);
        ca_pg_free(&gc);
    }
}

/* Every qp the encoder can hand these, not a sample: the deadzone add and
 * the dequant scale both step with qp % 6 and qp / 6, and 51 is where the
 * shift and the clip are closest to their bounds. The sweep runs QpY's
 * whole range at the build's depth - -12 .. 51 at Main10 - because the
 * kernels index their tables with Qp' and a sign error there shows at the
 * bottom of the ladder and nowhere else. */
static int check_quant_neon(void)
{
    int bad = 0;
    static const int pg_qp[] = { N265_QP_MIN, 0, 4, 30, 51 };

    for (int log2n = 2; log2n <= 5; log2n++)
        for (int kind = 0; kind < N_LKIND; kind++)
            for (unsigned q = 0; q < sizeof(pg_qp) / sizeof(pg_qp[0]); q++)
                check_quant_window(log2n, kind, pg_qp[q]);

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
            for (int qp = N265_QP_MIN; qp <= N265_QP_MAX && !bad; qp++) {
                int rw = 0, rh = 0, gw = 0, gh = 0, rn, gn;

                /* ---- dequant ---- */
                fill_level(level, n, kind);
                n265_dequant_ref(ref, level, log2n, qp, &rw, &rh);
                ca_guard_poison(dq, bytes);
                ca_guard_scramble(level, bytes);
                CA_SIMD(n265_dequant)(dq, level, log2n, qp, &gw, &gh);
                bad += ca_guard_check(dq, bytes, "dequant_neon dq");
                bad += cmp16(ref, dq, nn, "dequant", shape,
                             lkind_name[kind]);
                if (rw != gw || rh != gh) {
                    printf("    dequant BOX %s / %s qp %d: ref %dx%d "
                           "simd %dx%d\n", shape, lkind_name[kind], qp,
                           rw, rh, gw, gh);
                    bad++;
                }
                /* the NULL-pointer arm the header allows */
                CA_SIMD(n265_dequant)(dq, level, log2n, qp, NULL, NULL);

                /* ---- quant ---- */
                for (int i = 0; i < nn; i++)
                    coef[i] = level[i];
                for (int intra = 0; intra < 3 && !bad; intra++) {
                    rn = n265_quant_ref(ref, coef, log2n, qp, intra);
                    ca_guard_poison(level, bytes);
                    ca_guard_scramble(coef, bytes);
                    gn = CA_SIMD(n265_quant)(level, coef, log2n, qp, intra);
                    bad += ca_guard_check(level, bytes,
                                          "quant_neon level");
                    bad += cmp16(ref, level, nn, "quant", shape,
                                 lkind_name[kind]);
                    if (rn != gn) {
                        printf("    quant NZ %s / %s qp %d intra %d: "
                               "ref %d simd %d\n", shape, lkind_name[kind],
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

/* idct_add is the one in this family with strides, so its two sample
 * windows stop at the last SAMPLE of the last row and not at the end of
 * it: ca_pg_window_pix, which is that window in bytes. A read into an interior row's padding is what the
 * scramble pass below is for; what this catches is the row after the
 * last, and the vector that ran past the end of it. The coefficient block
 * has no stride and is exactly n*n.
 *
 * The nonzero bound is the axis that matters here. The kernel is free to
 * skip work outside nzw x nzh, so the bounded call and the full-block call
 * are different code, and the bounded one is what the encoder presents on
 * the majority of calls (the survey in src/encoder/arm/neon.h). */
static void check_idct_window(int log2n, int use_dst, int kind, int pad)
{
    int n = 1 << log2n;
    int ds = pad ? n + 5 : n, ps = pad ? n + 3 : n;
    size_t qb = (size_t)n * n * sizeof(int16_t);
    size_t dbytes = ca_pg_window_pix(ds, n, n);
    size_t pbytes = ca_pg_window_pix(ps, n, n);

    for (int tail = 0; tail < 2; tail++) {
        ca_pg gq, gd, gp;
        int16_t *dq = ca_pg_alloc(&gq, qb, tail);
        pixel *dst = ca_pg_alloc(&gd, dbytes, tail);
        pixel *pred = ca_pg_alloc(&gp, pbytes, tail);
        int bw, bh;
        char what[96];

        fill_dq_ex(dq, n, kind);
        ca_pg_fill_pix(&gp, pbytes / sizeof(pixel));
        dq_bbox(dq, n, &bw, &bh);
        for (int b = 0; b < 3; b++) {
            int nzw = b == 0 ? bw : b == 1 ? n : (bw + n + 1) / 2;
            int nzh = b == 0 ? bh : b == 1 ? n : (bh + n + 1) / 2;

            snprintf(what, sizeof(what),
                     "idct_add %dx%d%s / %s nz %dx%d s%d, %s edge", n, n,
                     use_dst ? "-dst" : "", ckind_name[kind], nzw, nzh, pad,
                     tail ? "far" : "near");
            ca_pg_arm(what);
            CA_SIMD(n265_idct_add)(dst, ds, dq, log2n, pred, ps, use_dst,
                               nzw, nzh);
            ca_pg_disarm();
        }
        ca_pg_free(&gq);
        ca_pg_free(&gd);
        ca_pg_free(&gp);
    }
}

static int check_idct_neon(void)
{
    int bad = 0;

    for (unsigned s = 0; s < sizeof(tshapes) / sizeof(tshapes[0]); s++)
        for (int kind = 0; kind < N_CKIND; kind++)
            for (int pad = 0; pad < 2; pad++)
                check_idct_window(tshapes[s].log2n, tshapes[s].use_dst,
                                  kind, pad);

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
            size_t dbytes = (size_t)ds * n * sizeof(pixel);
            size_t pbytes = (size_t)ps * n * sizeof(pixel);
            pixel *dst = ca_guard_alloc(dbytes);
            pixel *pred = ca_guard_alloc(pbytes);
            pixel ref[37 * 32], first[37 * 32];

            for (int kind = 0; kind < N_CKIND && !bad; kind++) {
                int reps = kind == 0 || kind == 3 ? 32 : 4;

                for (int rep = 0; rep < reps && !bad; rep++) {
                    int bw, bh;

                    fill_dq_ex(dq, n, kind);
                    ca_fill_pix(pred, pbytes / sizeof(pixel));
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
                        CA_SIMD(n265_idct_add)(dst, ds, dq, log2n, pred, ps,
                                           use_dst, nzw, nzh);
                        bad += ca_guard_check(dst, dbytes, "idct_neon dst");
                        bad += ca_check_pad_pix(dst, ds, n, n,
                                                "idct_neon dst");
                        bad += cmp_pix(ref, dst, ds, n, "idct", tshapes[s].name,
                                    ckind_name[kind], nzw, nzh);
                        if (bad)
                            break;
                        if (b)
                            continue;
                        memcpy(first, dst, dbytes);
                        ca_guard_scramble(pred, pbytes);
                        memset(dst, 0, dbytes);
                        CA_SIMD(n265_idct_add)(dst, ds, dq, log2n, pred, ps,
                                           use_dst, nzw, nzh);
                        for (int y = 0; y < n; y++)
                            if (memcmp(first + y * ds, dst + y * ds,
                                       (size_t)n * sizeof(pixel))) {
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
#endif /* CA_SIMD_TRANSFORM */

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
                    pixel ref[32 * 32];
                    int bw, bh;
                    fill_dq(c.dq, nn, kind);
                    ca_fill_pix(c.pred, (size_t)nn);
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
                                       (size_t)n * sizeof(pixel))) {
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
#if CA_SIMD_TRANSFORM
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
#if CA_SIMD_TRANSFORM
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
#endif
    return 0;
}
