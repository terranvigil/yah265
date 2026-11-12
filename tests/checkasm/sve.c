/*
 * checkasm/sve.c - the SVE kernels (docs/sve.md), each against its C
 * reference and timed against its NEON twin.
 *
 * Every kernel is called directly and through its dispatched entry point,
 * on random and adversarial input, at tight and padded strides, with the
 * borders and row padding scrambled between two runs, and inside page
 * guards that end exactly at the declared window. The bench line is NEON
 * against SVE on one shape, which is what decides where a kernel ships.
 *
 * Copyright (c) 2026, the yah265 authors. GPL-2.0-or-later; see LICENSE.
 */

#include "checkasm.h"

#include "common/cpu.h"
#include "encoder/arm/neon.h"
#include "encoder/arm/sve.h"
#include "encoder/mc.h"
#include "encoder/satd.h"

static const int sides[] = { 4, 8, 12, 16, 24, 32, 48, 64 };
#define NSIDE ((int)(sizeof(sides) / sizeof(sides[0])))

#if N265_SVE_SATD

typedef uint64_t (*metric_fn)(const pixel *a, int as, const pixel *b, int bs,
                              int w, int h, uint64_t *aux);

/* ---- adapters: one signature for the four metrics ------------------- */

#define PAIR_ADAPT(name, call)                                            \
    static uint64_t name(const pixel *a, int as, const pixel *b, int bs,  \
                         int w, int h, uint64_t *aux)                     \
    {                                                                     \
        *aux = 0;                                                         \
        return call;                                                      \
    }
PAIR_ADAPT(satd_c, n265_satd_wh_c(a, as, b, bs, w, h))
PAIR_ADAPT(satd_n, n265_satd_wh_neon(a, as, b, bs, w, h))
PAIR_ADAPT(satd_s, n265_satd_wh_sve(a, as, b, bs, w, h))
PAIR_ADAPT(satd_d, n265_satd_wh(a, as, b, bs, w, h))
PAIR_ADAPT(sad_c, n265_sad_wh_c(a, as, b, bs, w, h))
PAIR_ADAPT(sad_n, n265_sad_wh_neon(a, as, b, bs, w, h))
PAIR_ADAPT(sad_s, n265_sad_wh_sve(a, as, b, bs, w, h))
PAIR_ADAPT(sad_d, n265_sad_wh(a, as, b, bs, w, h))
PAIR_ADAPT(ssd_c, n265_ssd_wh_c(a, as, b, bs, w, h))
PAIR_ADAPT(ssd_n, n265_ssd_wh_neon(a, as, b, bs, w, h))
PAIR_ADAPT(ssd_s, n265_ssd_wh_sve(a, as, b, bs, w, h))
PAIR_ADAPT(ssd_d, n265_ssd_wh(a, as, b, bs, w, h))
#undef PAIR_ADAPT

/* the sample sums read only `a` */
static uint64_t sumsq_c(const pixel *a, int as, const pixel *b, int bs,
                        int w, int h, uint64_t *aux)
{
    (void)b; (void)bs;
    return n265_sum_sq_wh_c(a, as, w, h, aux);
}

static uint64_t sumsq_n(const pixel *a, int as, const pixel *b, int bs,
                        int w, int h, uint64_t *aux)
{
    (void)b; (void)bs;
    if (w != 16 || h > 16)
        return n265_sum_sq_wh_c(a, as, w, h, aux);
    return n265_sum_sq16_neon(a, as, h, aux);
}

static uint64_t sumsq_s(const pixel *a, int as, const pixel *b, int bs,
                        int w, int h, uint64_t *aux)
{
    (void)b; (void)bs;
    return n265_sum_sq_wh_sve(a, as, w, h, aux);
}

static uint64_t sumsq_d(const pixel *a, int as, const pixel *b, int bs,
                        int w, int h, uint64_t *aux)
{
    (void)b; (void)bs;
    return n265_sum_sq_wh(a, as, w, h, aux);
}

typedef struct metric {
    const char *name;
    metric_fn ref, neon, sve, disp;
    int mult8;          /* shapes must be multiples of 8 */
} metric;

static const metric metrics[] = {
    { "satd", satd_c, satd_n, satd_s, satd_d, 1 },
    { "sad", sad_c, sad_n, sad_s, sad_d, 0 },
    { "ssd", ssd_c, ssd_n, ssd_s, ssd_d, 0 },
    { "sum_sq", sumsq_c, sumsq_n, sumsq_s, sumsq_d, 0 },
};
#define NMETRIC ((int)(sizeof(metrics) / sizeof(metrics[0])))

/* natural-order Hadamard sign */
static int had_sign(int k, int j)
{
    return (__builtin_popcount(k & j) & 1) ? -1 : 1;
}

#define NKIND 5

static void fill_pair(pixel *a, int as, pixel *b, int bs, int w, int h,
                      int kind)
{
    int ky = (int)(rnd() & 7), kx = (int)(rnd() & 7);

    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            pixel va, vb;
            switch (kind) {
            case 0:
                va = ca_pix();
                vb = ca_pix();
                break;
            case 1:
                va = ca_pix_edge();
                vb = ca_pix_edge();
                break;
            case 2:
                va = vb = ca_pix();
                break;
            case 3: {   /* the Hadamard's worst case: one basis function */
                int s = had_sign(ky, y & 7) * had_sign(kx, x & 7);
                va = (pixel)(s > 0 ? PIXEL_MAX : 0);
                vb = (pixel)(PIXEL_MAX - va);
                break;
            }
            default:
                va = (pixel)PIXEL_MAX;
                vb = 0;
                break;
            }
            a[(size_t)y * as + x] = va;
            b[(size_t)y * bs + x] = vb;
        }
}

static void scramble_pad(pixel *p, int stride, int w, int h)
{
    for (int y = 0; y < h; y++)
        for (int x = w; x < stride; x++)
            p[(size_t)y * stride + x] = ca_pix();
}

static int check_one(const metric *m, int w, int h)
{
    int bad = 0;
    char what[64];

    for (int is = 0; is < 2 && !bad; is++) {
        int as = is ? w + 5 : w, bs = is ? w + 13 : w;
        size_t ab = (size_t)as * h * sizeof(pixel);
        size_t bb = (size_t)bs * h * sizeof(pixel);
        pixel *a = ca_guard_alloc(ab), *b = ca_guard_alloc(bb);

        snprintf(what, sizeof(what), "%s_sve %dx%d s%d", m->name, w, h, is);
        for (int kind = 0; kind < NKIND && !bad; kind++)
            for (int rep = 0; rep < 8 && !bad; rep++) {
                uint64_t xr, xs, xd, xs2;
                memset(a, CA_POISON, ab);
                memset(b, CA_POISON, bb);
                fill_pair(a, as, b, bs, w, h, kind);
                ca_guard_poison(a, ab);
                ca_guard_poison(b, bb);
                uint64_t r = m->ref(a, as, b, bs, w, h, &xr);
                uint64_t s = m->sve(a, as, b, bs, w, h, &xs);
                uint64_t d = m->disp(a, as, b, bs, w, h, &xd);
                if (r != s || xr != xs) {
                    printf("    %s MISMATCH kind %d: ref %llu/%llu sve "
                           "%llu/%llu\n", what, kind, (unsigned long long)r,
                           (unsigned long long)xr, (unsigned long long)s,
                           (unsigned long long)xs);
                    bad++;
                    break;
                }
                if (r != d || xr != xd) {
                    printf("    %s DISPATCH MISMATCH kind %d\n", what, kind);
                    bad++;
                    break;
                }
                bad += ca_guard_check(a, ab, what);
                bad += ca_guard_check(b, bb, what);
                ca_guard_scramble(a, ab);
                ca_guard_scramble(b, bb);
                scramble_pad(a, as, w, h);
                scramble_pad(b, bs, w, h);
                uint64_t s2 = m->sve(a, as, b, bs, w, h, &xs2);
                if (s2 != s || xs2 != xs) {
                    printf("    %s: reads outside the block (kind %d)\n",
                           what, kind);
                    bad++;
                }
            }
        ca_guard_free(a);
        ca_guard_free(b);
    }

    /* the page guard, the window flush against each end */
    for (int tail = 0; tail < 2 && !bad; tail++) {
        int as = w + 3, bs = w + 7;
        size_t aw = ca_pg_window_pix(as, w, h), bw = ca_pg_window_pix(bs, w, h);
        ca_pg ga, gb;
        pixel *a = ca_pg_alloc(&ga, aw, tail);
        pixel *b = ca_pg_alloc(&gb, bw, tail);
        uint64_t xr, xs;

        ca_pg_fill_pix(&ga, aw / sizeof(pixel));
        ca_pg_fill_pix(&gb, bw / sizeof(pixel));
        snprintf(what, sizeof(what), "%s_sve %dx%d page tail %d", m->name,
                 w, h, tail);
        ca_pg_arm(what);
        uint64_t s = m->sve(a, as, b, bs, w, h, &xs);
        ca_pg_disarm();
        uint64_t r = m->ref(a, as, b, bs, w, h, &xr);
        if (r != s || xr != xs) {
            printf("    %s MISMATCH\n", what);
            bad++;
        }
        ca_pg_free(&ga);
        ca_pg_free(&gb);
    }
    return bad;
}

/* ---- bench: NEON against SVE ---------------------------------------- */

static double time_fn(metric_fn f, const pixel *a, int as, const pixel *b,
                      int bs, int w, int h, int iters)
{
    volatile uint64_t sink = 0;
    uint64_t aux;
    for (int i = 0; i < iters / 8; i++)
        sink += f(a, as, b, bs, w, h, &aux);
    double t0 = now_ns();
    for (int i = 0; i < iters; i++)
        sink += f(a, as, b, bs, w, h, &aux);
    (void)sink;
    return (now_ns() - t0) / iters;
}

static void bench_one(const metric *m, int w, int h)
{
    int as = 128, bs = 160;
    pixel *a = malloc((size_t)as * h * sizeof(pixel));
    pixel *b = malloc((size_t)bs * h * sizeof(pixel));
    int iters = 2000000 / (w * h) + 2000;

    fill_pair(a, as, b, bs, w, h, 0);
    /* best of three, alternating, so neither side gets a warmer cache */
    double tn = 1e30, ts = 1e30;
    for (int r = 0; r < 3; r++) {
        double n = time_fn(m->neon, a, as, b, bs, w, h, iters);
        double s = time_fn(m->sve, a, as, b, bs, w, h, iters);
        tn = n < tn ? n : tn;
        ts = s < ts ? s : ts;
    }
    printf("    bench %-7s %2dx%-2d  neon %8.2f ns  %s %8.2f ns  %5.2fx\n",
           m->name, w, h, tn, ca_isa, ts, tn / ts);
    free(a);
    free(b);
}

static int check_metrics(void)
{
    int bad = 0;
    for (int k = 0; k < NMETRIC && !bad; k++)
        for (int i = 0; i < NSIDE && !bad; i++)
            for (int j = 0; j < NSIDE && !bad; j++) {
                int w = sides[i], h = sides[j];
                if (metrics[k].mult8 && ((w | h) & 7))
                    continue;
                bad += check_one(&metrics[k], w, h);
            }
    if (ca_bench && !bad)
        for (int k = 0; k < NMETRIC; k++)
            for (int i = 0; i < NSIDE; i++) {
                int w = sides[i];
                if (metrics[k].mult8 && (w & 7))
                    continue;
                bench_one(&metrics[k], w, w);
                if (w >= 8 && !(metrics[k].mult8 && ((w / 2) & 7)))
                    bench_one(&metrics[k], w, w / 2);
            }
    return bad;
}

#else
static int check_metrics(void)
{
    return 0;
}
#endif /* N265_SVE_SATD */

#if N265_SVE_MC

/* ---- the vertical luma pass over cached int16 rows ------------------
 *
 * The cached rows span the first stage's range at either depth; kind 1
 * puts every sample at one end of it, which drives the accumulator to
 * the extremes where the 14-bit store wraps. */
#define HF_LO (-6000)
#define HF_HI 23000

typedef void (*vpix_fn)(pixel *, int, int, int, const int16_t *, int, int);
typedef void (*vs16_fn)(int16_t *, int, int, int, const int16_t *, int, int);

static void fill_hf(int16_t *p, size_t n, int kind)
{
    for (size_t i = 0; i < n; i++)
        p[i] = (int16_t)(kind ? ((rnd() & 1) ? HF_HI : HF_LO)
                              : HF_LO + (int)(rnd() % (HF_HI - HF_LO + 1)));
}

static void vpix_disp(pixel *d, int ds, int w, int h, const int16_t *hf,
                      int hs, int fy)
{
    n265_mc_luma_hf(d, ds, w, h, NULL, 0, hf, hs, 1, fy);
}

static void vs16_disp(int16_t *d, int ds, int w, int h, const int16_t *hf,
                      int hs, int fy)
{
    n265_mc_luma_14_hf(d, ds, w, h, NULL, 0, hf, hs, 1, fy);
}

/* one shape at one fy, both output forms, through the kernel and the
 * dispatched entry point; `hf` is readable from row -3 to row h + 4 */
static int check_v_shape(const int16_t *hf, int hs, int w, int h, int fy,
                         const char *what)
{
    int ds = w + 9, bad = 0;
    size_t n = (size_t)ds * h;
    pixel *r = ca_guard_alloc(n * sizeof(pixel));
    pixel *o = ca_guard_alloc(n * sizeof(pixel));
    int16_t *r16 = ca_guard_alloc(n * 2), *o16 = ca_guard_alloc(n * 2);

    for (int via = 0; via < 2 && !bad; via++) {
        vpix_fn fp = via ? vpix_disp : n265_mc_luma_hf_sve;
        vs16_fn fs = via ? vs16_disp : n265_mc_luma_14_hf_sve;

        memset(r, CA_POISON, n * sizeof(pixel));
        memset(o, CA_POISON, n * sizeof(pixel));
        memset(r16, CA_POISON, n * 2);
        memset(o16, CA_POISON, n * 2);
        ca_guard_poison(o, n * sizeof(pixel));
        ca_guard_poison(o16, n * 2);
        n265_mc_luma_hf_ref(r, ds, w, h, hf, hs, fy);
        n265_mc_luma_14_hf_ref(r16, ds, w, h, hf, hs, fy);
        fp(o, ds, w, h, hf, hs, fy);
        fs(o16, ds, w, h, hf, hs, fy);
        if (memcmp(r, o, n * sizeof(pixel))) {
            printf("    %s pixel MISMATCH%s\n", what, via ? " (dispatched)" : "");
            bad++;
        }
        if (memcmp(r16, o16, n * 2)) {
            printf("    %s 14-bit MISMATCH%s\n", what, via ? " (dispatched)" : "");
            bad++;
        }
        bad += ca_guard_check(o, n * sizeof(pixel), what);
        bad += ca_guard_check(o16, n * 2, what);
        bad += ca_check_pad_pix(o, ds, w, h, what);
        bad += ca_check_pad((const uint8_t *)o16, ds * 2, w * 2, h, what);
    }
    ca_guard_free(r);
    ca_guard_free(o);
    ca_guard_free(r16);
    ca_guard_free(o16);
    return bad;
}

static void bench_mc(void)
{
    for (int i = 0; i < NSIDE; i++)
        for (int fy = 1; fy < 3; fy++) {
            int w = sides[i], h = w, hs = 80, ds = 72;
            int16_t *hf = malloc((size_t)hs * (h + 7) * 2);
            pixel *d = malloc((size_t)ds * h * sizeof(pixel));
            int16_t *d16 = malloc((size_t)ds * h * 2);
            const int16_t *src = hf + 3 * hs;
            int iters = 4000000 / (w * h) + 2000;
            double t[4] = { 1e30, 1e30, 1e30, 1e30 };

            fill_hf(hf, (size_t)hs * (h + 7), 0);
            for (int rep = 0; rep < 3; rep++) {
                double t0 = now_ns();
                for (int k = 0; k < iters; k++)
                    n265_mc_luma_hf_neon(d, ds, w, h, src, hs, fy);
                double t1 = now_ns();
                for (int k = 0; k < iters; k++)
                    n265_mc_luma_hf_sve(d, ds, w, h, src, hs, fy);
                double t2 = now_ns();
                for (int k = 0; k < iters; k++)
                    n265_mc_luma_14_hf_neon(d16, ds, w, h, src, hs, fy);
                double t3 = now_ns();
                for (int k = 0; k < iters; k++)
                    n265_mc_luma_14_hf_sve(d16, ds, w, h, src, hs, fy);
                double t4 = now_ns();
                double v[4] = { t1 - t0, t2 - t1, t3 - t2, t4 - t3 };
                for (int q = 0; q < 4; q++)
                    if (v[q] / iters < t[q])
                        t[q] = v[q] / iters;
            }
            printf("    bench mc_hf   %2dx%-2d fy%d neon %8.2f ns  %s %8.2f ns  "
                   "%5.2fx   14: neon %8.2f  %s %8.2f  %5.2fx\n", w, h, fy,
                   t[0], ca_isa, t[1], t[0] / t[1], t[2], ca_isa, t[3],
                   t[2] / t[3]);
            free(hf);
            free(d);
            free(d16);
        }
}

static int check_mc(void)
{
    int bad = 0;
    char what[64];

    for (int i = 0; i < NSIDE && !bad; i++)
        for (int j = 0; j < NSIDE && !bad; j++)
            for (int fy = 0; fy < 4 && !bad; fy++)
                for (int kind = 0; kind < 2 && !bad; kind++) {
                    int w = sides[i], h = sides[j], hs = w + 11;
                    size_t n = (size_t)hs * (h + 7);
                    int16_t *buf = ca_guard_alloc(n * 2);

                    fill_hf(buf, n, kind);
                    snprintf(what, sizeof(what), "mc_v_sve %dx%d fy%d k%d",
                             w, h, fy, kind);
                    bad += check_v_shape(buf + 3 * hs, hs, w, h, fy, what);
                    ca_guard_free(buf);
                }

    /* page guards hard against the first and the last row read */
    for (int i = 0; i < NSIDE && !bad; i++)
        for (int fy = 0; fy < 4 && !bad; fy++)
            for (int tail = 0; tail < 2 && !bad; tail++) {
                int w = sides[i], h = sides[NSIDE - 1 - i], hs = w + 5;
                int above = fy ? 3 : 0, rows = h + (fy ? 7 : 0);
                size_t win = (size_t)(rows - 1) * hs + w;
                ca_pg g;
                int16_t *p = ca_pg_alloc(&g, win * 2, tail);

                fill_hf(p, win, 0);
                snprintf(what, sizeof(what), "mc_v_sve %dx%d fy%d page %d",
                         w, h, fy, tail);
                ca_pg_arm(what);
                bad += check_v_shape(p + (size_t)above * hs, hs, w, h, fy,
                                     what);
                ca_pg_disarm();
                ca_pg_free(&g);
            }

    if (ca_bench && !bad)
        bench_mc();
    return bad;
}

#else
static int check_mc(void)
{
    return 0;
}
#endif /* N265_SVE_MC */

int check_sve(void)
{
    int bad = check_metrics();
    bad += check_mc();
    return bad;
}

