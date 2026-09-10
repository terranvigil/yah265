/*
 * checkasm: validates every dispatched kernel against its C reference and
 * benchmarks it. "Dispatched" includes a C kernel that replaced a slower
 * C one: the reference stays in the tree and the fast path is checked
 * against it here, on random and on adversarial input, before it merges.
 * Copyright (c) 2026, the yah265 authors. GPL-2.0-or-later; see LICENSE.
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "encoder/tables.h"
#include "encoder/satd.h"
#include "encoder/transform.h"

static uint32_t rs = 0x2545f491u;

static uint32_t rnd(void)
{
    rs ^= rs << 13;
    rs ^= rs >> 17;
    rs ^= rs << 5;
    return rs;
}

static double now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1e9 + ts.tv_nsec;
}

/* the benchmark line every kernel prints: ns per call, both paths */
static void bench(const char *what, void (*ref)(void *), void (*opt)(void *),
                  void *ctx, int iters)
{
    double t0, t1, t2;
    for (int i = 0; i < iters / 8; i++) { ref(ctx); opt(ctx); }
    t0 = now_ns();
    for (int i = 0; i < iters; i++) ref(ctx);
    t1 = now_ns();
    for (int i = 0; i < iters; i++) opt(ctx);
    t2 = now_ns();
    printf("    %-22s ref %8.1f ns   opt %8.1f ns   %5.2fx\n", what,
           (t1 - t0) / iters, (t2 - t1) / iters, (t1 - t0) / (t2 - t1));
}

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

static int check_transforms(void)
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
        bench(name, fdct_ref_call, fdct_opt_call, &c, 20000);
        snprintf(name, sizeof(name), "idct_add %dx%d", n, n);
        bench(name, idct_ref_call, idct_opt_call, &c, 20000);
        /* the shape the encoder actually hands it: a small low-frequency
         * corner, and DC alone */
        fill_dq(c.dq, n * n, 4);
        for (int y = 0; y < 4 && y < n; y++)
            for (int x = 0; x < 4 && x < n; x++)
                c.dq[y * n + x] = (int16_t)((int)(rnd() >> 20) - 2048);
        dq_bbox(c.dq, n, &c.nzw, &c.nzh);
        snprintf(name, sizeof(name), "idct_add %dx%d 4x4nz", n, n);
        bench(name, idct_ref_call, idct_opt_call, &c, 20000);
        fill_dq(c.dq, n * n, 4);
        c.dq[0] = 1234;
        c.nzw = c.nzh = 1;
        snprintf(name, sizeof(name), "idct_add %dx%d dc", n, n);
        bench(name, idct_ref_call, idct_opt_call, &c, 20000);
    }
    c.log2n = 2; c.use_dst = 1;
    fill_resid(c.resid, 16, 0);
    fill_dq(c.dq, 16, 0);
    c.nzw = c.nzh = 4;
    bench("fdst 4x4", fdct_ref_call, fdct_opt_call, &c, 20000);
    bench("idst_add 4x4", idct_ref_call, idct_opt_call, &c, 20000);
    return 0;
}

/* ---- SATD ---------------------------------------------------------- */

struct sctx {
    int w, h;
    uint8_t a[64 * 64], b[64 * 64];
};

/* the tiles the encoder actually asks for: squares 4..32, the
 * rectangular PUs, and the AMP shapes whose 12 and 4 sides fall to the
 * 4x4 tiling */
static const int satd_shapes[][2] = {
    { 4, 4 }, { 8, 8 }, { 16, 16 }, { 32, 32 }, { 64, 64 },
    { 8, 4 }, { 4, 8 }, { 16, 8 }, { 8, 16 }, { 32, 16 }, { 16, 32 },
    { 16, 4 }, { 4, 16 }, { 12, 16 }, { 16, 12 }, { 24, 32 }, { 32, 24 },
};

static void satd_ref_call(void *p)
{
    struct sctx *c = p;
    uint32_t sum = 0;
    if ((c->w & 7) || (c->h & 7)) {
        for (int y = 0; y < c->h; y += 4)
            for (int x = 0; x < c->w; x += 4)
                sum += n265_satd4_ref(c->a + y * 64 + x, 64,
                                      c->b + y * 64 + x, 64);
    } else {
        for (int y = 0; y < c->h; y += 8)
            for (int x = 0; x < c->w; x += 8)
                sum += n265_satd8_ref(c->a + y * 64 + x, 64,
                                      c->b + y * 64 + x, 64);
    }
    c->a[4095] = (uint8_t)sum;      /* keep the call live */
}

static void satd_opt_call(void *p)
{
    struct sctx *c = p;
    c->a[4095] = (uint8_t)n265_satd_wh(c->a, 64, c->b, 64, c->w, c->h);
}

static uint32_t satd_ref_wh(const uint8_t *a, int as, const uint8_t *b,
                            int bs, int w, int h)
{
    uint32_t sum = 0;
    if ((w & 7) || (h & 7)) {
        for (int y = 0; y < h; y += 4)
            for (int x = 0; x < w; x += 4)
                sum += n265_satd4_ref(a + y * as + x, as, b + y * bs + x, bs);
        return sum;
    }
    for (int y = 0; y < h; y += 8)
        for (int x = 0; x < w; x += 8)
            sum += n265_satd8_ref(a + y * as + x, as, b + y * bs + x, bs);
    return sum;
}

static int check_satd(void)
{
    static struct sctx c;
    int bad = 0;
    int nsh = (int)(sizeof(satd_shapes) / sizeof(satd_shapes[0]));

    for (int s = 0; s < nsh; s++) {
        c.w = satd_shapes[s][0];
        c.h = satd_shapes[s][1];
        for (int kind = 0; kind < 3; kind++) {
            for (int rep = 0; rep < 128; rep++) {
                for (int i = 0; i < 64 * 64; i++) {
                    switch (kind) {
                    case 0:
                        c.a[i] = (uint8_t)(rnd() >> 24);
                        c.b[i] = (uint8_t)(rnd() >> 24);
                        break;
                    case 1:     /* the extremes the abs-sum has to survive */
                        c.a[i] = (uint8_t)((rnd() & 1) ? 255 : 0);
                        c.b[i] = (uint8_t)((rnd() & 1) ? 255 : 0);
                        break;
                    default:    /* near-identical: the common encoder case */
                        c.a[i] = (uint8_t)(rnd() >> 24);
                        c.b[i] = c.a[i];
                        break;
                    }
                }
                uint32_t r = satd_ref_wh(c.a, 64, c.b, 64, c.w, c.h);
                uint32_t o = n265_satd_wh(c.a, 64, c.b, 64, c.w, c.h);
                if (r != o) {
                    printf("    satd MISMATCH %dx%d kind %d: ref %u opt %u\n",
                           c.w, c.h, kind, r, o);
                    bad++;
                    rep = 128;
                }
            }
        }
    }
    if (bad)
        return bad;

    for (int s = 0; s < nsh; s++) {
        char name[32];
        c.w = satd_shapes[s][0];
        c.h = satd_shapes[s][1];
        for (int i = 0; i < 64 * 64; i++) {
            c.a[i] = (uint8_t)(rnd() >> 24);
            c.b[i] = (uint8_t)(rnd() >> 24);
        }
        snprintf(name, sizeof(name), "satd %dx%d", c.w, c.h);
        bench(name, satd_ref_call, satd_opt_call, &c, 40000);
    }
    return 0;
}

typedef struct kernel_test {
    const char *name;
    int (*run)(void);   /* 0 = pass */
} kernel_test;

static const kernel_test tests[] = {
    { "transform", check_transforms },
    { "satd", check_satd },
    { NULL, NULL },
};

int main(void)
{
    int fails = 0, ran = 0;
    n265_tables_init();
    for (const kernel_test *t = tests; t->name; t++) {
        int r = t->run();
        printf("%-32s %s\n", t->name, r ? "FAIL" : "ok");
        fails += !!r;
        ran++;
    }
    printf("checkasm: %d kernel(s), %d failure(s)\n", ran, fails);
    return fails ? 1 : 0;
}
