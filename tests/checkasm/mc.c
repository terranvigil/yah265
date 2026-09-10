/*
 * checkasm/mc.c - the motion-compensation family (class N265_ASM_MC).
 * See checkasm.h for the harness and the convention.
 * Copyright (c) 2026, the yah265 authors. GPL-2.0-or-later; see LICENSE.
 */

#include "checkasm.h"

#include "common/cpu.h"
#include "encoder/mc.h"
#include "encoder/arm/neon.h"   /* self-guarded; the family kernels land here */

/* ---- bi_avg --------------------------------------------------------- */

/* Every width and height a bi-predicted block can present. The luma PU
 * shapes give the square CB sizes, the rectangular halves and the AMP
 * quarters (4 and 12, the two the vector loop cannot cover without a
 * 4-lane tail) - but 4:2:0 CHROMA halves all of them, so 2 and 6 are here
 * too and they are the shapes that matter: a kernel whose narrowest path
 * is four lanes wide is wrong on both, and wrong in the direction that
 * changes the picture. The list is taken from the shapes the encoder was
 * instrumented to actually emit, not from what luma alone would suggest. */
static const int bi_dims[] = { 2, 4, 6, 8, 12, 16, 24, 32, 48, 64 };
#define NBI_DIM ((int)(sizeof(bi_dims) / sizeof(bi_dims[0])))

/* the block is written at a stride wider than it, and not by a multiple of
 * four, so a store that ran one vector too far lands in padding this
 * checks rather than in the next row where it would hide */
#define BI_PAD 5

/* Compare against the NEON kernel DIRECTLY. Going through n265_bi_avg_wh
 * would let N265_ASM_OFF turn this check into C against C without saying
 * so; on a host with no NEON the entry point is the only thing there is
 * and the check degenerates honestly. */
static void bi_opt(uint8_t *dst, int ds, const int16_t *a, const int16_t *b,
                   int w, int h)
{
#if N265_NEON
    n265_bi_avg_wh_neon(dst, ds, a, b, w, h);
#else
    n265_bi_avg_wh(dst, ds, a, b, w, h);
#endif
}

/* Input generators. The 14-bit MC intermediates the encoder really hands
 * this kernel live near the middle of the int16 range, but the two clips
 * in the C form are only exercised at the ends, so kind 1 is the whole
 * int16 extreme set and kind 2 walks the sum across both clip boundaries
 * (255 * 128 = 32640 is where the upper clip starts to bite). */
static void bi_fill(int16_t *a, int16_t *b, int n, int kind)
{
    for (int i = 0; i < n; i++) {
        switch (kind) {
        case 0:
            a[i] = (int16_t)(rnd() >> 16);
            b[i] = (int16_t)(rnd() >> 16);
            break;
        case 1:
            a[i] = (int16_t)((rnd() & 1) ? 32767 : -32768);
            b[i] = (int16_t)((rnd() & 1) ? 32767 : -32768);
            break;
        case 2: {
            int v = (int)(rnd() % 33200u) - 300;     /* sum across 0..32640 */
            a[i] = (int16_t)(v / 2);
            b[i] = (int16_t)(v - v / 2);
            break;
        }
        default:
            a[i] = 0;
            b[i] = (int16_t)((rnd() & 1) ? 127 : 128);  /* the >> 7 boundary */
            break;
        }
    }
}

struct bctx {
    uint8_t *dst;
    int16_t *a, *b;
    int w, h, ds;
};

static void bi_ref_call(void *p)
{
    struct bctx *c = p;
    n265_bi_avg_wh_ref(c->dst, c->ds, c->a, c->b, c->w, c->h);
}

static void bi_opt_call(void *p)
{
    struct bctx *c = p;
    bi_opt(c->dst, c->ds, c->a, c->b, c->w, c->h);
}

static int check_bi_avg(void)
{
    int bad = 0;

    for (int iw = 0; iw < NBI_DIM && !bad; iw++) {
        for (int ih = 0; ih < NBI_DIM && !bad; ih++) {
            int w = bi_dims[iw], h = bi_dims[ih];
            int ds = w + BI_PAD;
            size_t nn = (size_t)w * h;
            size_t ab_bytes = nn * sizeof(int16_t);
            size_t d_bytes = (size_t)ds * h;
            int16_t *a = ca_guard_alloc(ab_bytes);
            int16_t *b = ca_guard_alloc(ab_bytes);
            uint8_t *dst = ca_guard_alloc(d_bytes);
            uint8_t *ref = ca_guard_alloc(d_bytes);
            uint8_t *alt = ca_guard_alloc(d_bytes);
            char what[48];

            snprintf(what, sizeof(what), "bi_avg %dx%d", w, h);
            for (int kind = 0; kind < 4 && !bad; kind++) {
                for (int rep = 0; rep < 24 && !bad; rep++) {
                    bi_fill(a, b, (int)nn, kind);
                    memset(ref, CA_POISON, d_bytes);
                    memset(dst, CA_POISON, d_bytes);
                    ca_guard_poison(a, ab_bytes);
                    ca_guard_poison(b, ab_bytes);
                    ca_guard_poison(dst, d_bytes);

                    n265_bi_avg_wh_ref(ref, ds, a, b, w, h);
                    bi_opt(dst, ds, a, b, w, h);

                    for (int y = 0; y < h; y++)
                        if (memcmp(ref + (size_t)y * ds, dst + (size_t)y * ds,
                                   (size_t)w)) {
                            printf("    %s MISMATCH kind %d row %d\n", what,
                                   kind, y);
                            bad++;
                            break;
                        }
                    if (bad)
                        break;
                    /* wrote past the block, or into the row padding, or
                     * scribbled on an input */
                    bad += ca_guard_check(dst, d_bytes, what);
                    bad += ca_check_pad(dst, ds, w, h, what);
                    bad += ca_guard_check(a, ab_bytes, what);
                    bad += ca_guard_check(b, ab_bytes, what);
                    /* the reference has to respect the padding too, else
                     * the comparison above proves nothing */
                    bad += ca_check_pad(ref, ds, w, h, "bi_avg ref");
                    if (bad)
                        break;

                    /* Overread: the kernel's declared read window is
                     * exactly w*h samples of a and b. Run it twice with
                     * different garbage outside that window; an output
                     * that moves means it read something it was not
                     * given. */
                    ca_guard_scramble(a, ab_bytes);
                    ca_guard_scramble(b, ab_bytes);
                    memset(dst, CA_POISON, d_bytes);
                    bi_opt(dst, ds, a, b, w, h);
                    ca_guard_scramble(a, ab_bytes);
                    ca_guard_scramble(b, ab_bytes);
                    memset(alt, CA_POISON, d_bytes);
                    bi_opt(alt, ds, a, b, w, h);
                    if (memcmp(dst, alt, d_bytes)) {
                        printf("    %s: output depends on bytes OUTSIDE "
                               "the block (kind %d)\n", what, kind);
                        bad++;
                    }
                }
            }
            ca_guard_free(a);
            ca_guard_free(b);
            ca_guard_free(dst);
            ca_guard_free(ref);
            ca_guard_free(alt);
        }
    }
    return bad;
}

/* the shapes worth a line: every width at its square height, plus the two
 * AMP-ish rectangles whose tails and row counts differ */
static const int bi_bench_shapes[][2] = {
    { 2, 8 }, { 4, 4 }, { 6, 8 }, { 8, 8 }, { 12, 16 }, { 16, 16 },
    { 24, 32 }, { 32, 32 }, { 48, 48 }, { 64, 64 },
};

static void bench_bi_avg(void)
{
    static int16_t a[64 * 64], b[64 * 64];
    static uint8_t dst[64 * 70];
    struct bctx c;
    int n = (int)(sizeof(bi_bench_shapes) / sizeof(bi_bench_shapes[0]));

    bi_fill(a, b, 64 * 64, 0);
    c.a = a;
    c.b = b;
    c.dst = dst;
    for (int i = 0; i < n; i++) {
        char name[32];
        c.w = bi_bench_shapes[i][0];
        c.h = bi_bench_shapes[i][1];
        c.ds = c.w + BI_PAD;
        snprintf(name, sizeof(name), "bi_avg %dx%d", c.w, c.h);
        /* no C fast path distinct from the reference here, so the middle
         * column is a control: it should read the same as the first */
#if N265_NEON
        bench(name, bi_ref_call, bi_ref_call, bi_opt_call, &c, 200000);
#else
        bench(name, bi_ref_call, bi_opt_call, NULL, &c, 200000);
#endif
    }
}

int check_mc(void)
{
    int bad = check_bi_avg();
    if (bad)
        return bad;
    bench_bi_avg();
    return 0;
}
