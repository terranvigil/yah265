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
        bench(name, fdct_ref_call, fdct_opt_call, NULL, &c, 20000);
        snprintf(name, sizeof(name), "idct_add %dx%d", n, n);
        bench(name, idct_ref_call, idct_opt_call, NULL, &c, 20000);
        /* the shape the encoder actually hands it: a small low-frequency
         * corner, and DC alone */
        fill_dq(c.dq, n * n, 4);
        for (int y = 0; y < 4 && y < n; y++)
            for (int x = 0; x < 4 && x < n; x++)
                c.dq[y * n + x] = (int16_t)((int)(rnd() >> 20) - 2048);
        dq_bbox(c.dq, n, &c.nzw, &c.nzh);
        snprintf(name, sizeof(name), "idct_add %dx%d 4x4nz", n, n);
        bench(name, idct_ref_call, idct_opt_call, NULL, &c, 20000);
        fill_dq(c.dq, n * n, 4);
        c.dq[0] = 1234;
        c.nzw = c.nzh = 1;
        snprintf(name, sizeof(name), "idct_add %dx%d dc", n, n);
        bench(name, idct_ref_call, idct_opt_call, NULL, &c, 20000);
    }
    c.log2n = 2; c.use_dst = 1;
    fill_resid(c.resid, 16, 0);
    fill_dq(c.dq, 16, 0);
    c.nzw = c.nzh = 4;
    bench("fdst 4x4", fdct_ref_call, fdct_opt_call, NULL, &c, 20000);
    bench("idst_add 4x4", idct_ref_call, idct_opt_call, NULL, &c, 20000);
    return 0;
}

