/*
 * The --target-vmaf CRF search (cli/tvmaf.c) against synthetic CRF-to-VMAF
 * curves: it must land inside the window, never score a CRF twice, stay
 * inside the bounds and say how it ended.
 * Copyright (c) 2026, the yah265 authors. GPL-2.0-or-later; see LICENSE.
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "tvmaf.h"

static int fails;

#define CHECK(c) do { if (!(c)) { \
    fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #c); fails++; } \
} while (0)

typedef struct {
    double mid, k;              /* logistic: 100 / (1 + exp(k (crf - mid))) */
    double cap_crf;             /* below this, a VBV cap freezes the curve */
    double seen[256];
    int n, repeats, outside;
    const tv_params *p;
} curve;

static double curve_at(const curve *c, double crf)
{
    if (crf < c->cap_crf)
        crf = c->cap_crf;
    return 100.0 / (1.0 + exp(c->k * (crf - c->mid)));
}

static int fake(void *ctx, double crf, tv_score *out)
{
    curve *c = ctx;
    for (int i = 0; i < c->n; i++)
        if (fabs(c->seen[i] - crf) < 1e-9)
            c->repeats++;
    if (crf < c->p->crf_lo - 1e-9 || crf > c->p->crf_hi + 1e-9)
        c->outside++;
    c->seen[c->n++] = crf;
    out->vmaf = curve_at(c, crf);
    out->kbps = 8000.0 * exp(-0.115 * (crf < c->cap_crf ? c->cap_crf : crf));
    out->bytes = (long)out->kbps;
    return 0;
}

static void run(double mid, double k, double target, int floor_pol)
{
    tv_params p;
    tv_params_default(&p);
    p.target = target;
    p.floor = floor_pol;
    curve c = { .mid = mid, .k = k, .cap_crf = -1e9, .p = &p };
    tv_result r;
    CHECK(tv_search(&p, fake, &c, &r) == 0);
    CHECK(c.repeats == 0);
    CHECK(c.outside == 0);
    CHECK(r.trials == c.n);
    CHECK(r.trials <= 8);
    CHECK(fabs(r.s.vmaf - curve_at(&c, r.crf)) < 1e-9);
    if (floor_pol) {
        CHECK(r.s.vmaf >= target);
        /* the next grid point toward size misses */
        CHECK(r.status == TV_CONVERGED ||
              curve_at(&c, r.crf + p.step) < target);
    } else {
        CHECK(r.status == TV_CONVERGED || r.status == TV_BRACKETED);
        CHECK(fabs(r.s.vmaf - target) < p.tol ||
              r.status == TV_BRACKETED);
    }
    if (fails)
        fprintf(stderr, "  curve mid %.1f k %.2f target %.1f floor %d: crf "
                "%.2f vmaf %.3f in %d\n", mid, k, target, floor_pol, r.crf,
                r.s.vmaf, r.trials);
}

int main(void)
{
    /* steep and shallow curves, targets across the useful range */
    static const double mids[] = { 18.0, 26.0, 34.0, 40.0 };
    static const double ks[] = { 0.12, 0.2, 0.35, 0.6 };
    static const double targets[] = { 75.0, 85.0, 90.0, 93.0, 95.0 };
    for (int a = 0; a < 4; a++)
        for (int b = 0; b < 4; b++)
            for (int t = 0; t < 5; t++) {
                double crf = mids[a] + log(100.0 / targets[t] - 1.0) / ks[b];
                if (crf < 9.0 || crf > 44.0)
                    continue;   /* outside the default bounds */
                run(mids[a], ks[b], targets[t], 0);
                run(mids[a], ks[b], targets[t], 1);
            }

    tv_params p;
    tv_result r;
    tv_params_default(&p);

    /* even the best quality misses */
    p.target = 99.5;
    curve hi = { .mid = 20.0, .k = 0.2, .cap_crf = -1e9, .p = &p };
    CHECK(tv_search(&p, fake, &hi, &r) == 0);
    CHECK(r.status == TV_UNREACHABLE && r.crf == p.crf_lo);
    CHECK(hi.repeats == 0);

    /* even the worst quality beats it */
    p.target = 20.0;
    curve lo = { .mid = 60.0, .k = 0.2, .cap_crf = -1e9, .p = &p };
    CHECK(tv_search(&p, fake, &lo, &r) == 0);
    CHECK(r.status == TV_EXCEEDED && r.crf == p.crf_hi);

    /* a cap freezes quality below crf 30, short of the target */
    p.target = 95.0;
    curve cap = { .mid = 34.0, .k = 0.2, .cap_crf = 30.0, .p = &p };
    CHECK(tv_search(&p, fake, &cap, &r) == 0);
    CHECK(r.status == TV_PLATEAU);
    CHECK(r.trials < 12);

    /* nonsense parameters are refused */
    p.target = 100.0;
    CHECK(tv_search(&p, fake, &cap, &r) != 0);
    p.target = 90.0;
    p.step = 0.0;
    CHECK(tv_search(&p, fake, &cap, &r) != 0);

    /* the floor's whole-file correction: meridian 1080p at 85, where the
     * windows read 85.07 and the whole film 84.47 */
    tv_params_default(&p);
    p.target = 85.0;
    tv_point mw[] = { { 25.25, 84.472 } };
    tv_point mt[] = { { 26.0, 83.9 }, { 25.5, 84.758 }, { 25.25, 85.069 } };
    CHECK(fabs(tv_fix_next(&p, mw, 1, mt, 3) - 24.5) < 1e-9);
    /* no samples: the prior slope, still at least one grid step */
    double nx = tv_fix_next(&p, mw, 1, NULL, 0);
    CHECK(nx > 0 && nx <= 25.0 && nx >= 25.25 - 4.0);
    /* nothing toward quality is left at the bound */
    tv_point lo_w[] = { { 8.0, 80.0 } };
    CHECK(tv_fix_next(&p, lo_w, 1, mt, 3) < 0);

    /* against curves where the whole file reads under its samples: it
     * meets the floor inside the bound, never repeats a CRF and never
     * lands a whole CRF below the cheapest grid point that meets it */
    for (int a = 0; a < 4; a++)
        for (int b = 0; b < 4; b++)
            for (int t = 0; t < 5; t++)
                for (int o = 1; o <= 4; o++) {
                    double gap = 0.5 * o;
                    tv_params q;
                    tv_params_default(&q);
                    q.target = targets[t];
                    q.floor = 1;
                    curve c = { .mid = mids[a], .k = ks[b], .cap_crf = -1e9,
                                .p = &q };
                    if (tv_search(&q, fake, &c, &r) != 0 ||
                        r.status != TV_CONVERGED && r.status != TV_BRACKETED)
                        continue;
                    tv_point tr[64], w[TV_FIX_STEPS + 1];
                    int ntr = c.n < 64 ? c.n : 64;
                    for (int i = 0; i < ntr; i++) {
                        tr[i].crf = c.seen[i];
                        tr[i].vmaf = curve_at(&c, c.seen[i]);
                    }
                    w[0].crf = r.crf;
                    w[0].vmaf = curve_at(&c, r.crf) - gap;
                    int nw = 1;
                    while (w[nw - 1].vmaf < q.target && nw <= TV_FIX_STEPS) {
                        double x = tv_fix_next(&q, w, nw, tr, ntr);
                        if (x < 0)
                            break;
                        CHECK(x < w[nw - 1].crf - 1e-9);
                        CHECK(fabs(remainder(x - q.crf_lo, q.step)) < 1e-6);
                        w[nw].crf = x;
                        w[nw].vmaf = curve_at(&c, x) - gap;
                        nw++;
                    }
                    double cheapest = q.crf_lo;
                    for (double x = q.crf_hi; x >= q.crf_lo; x -= q.step)
                        if (curve_at(&c, x) - gap >= q.target) {
                            cheapest = x;
                            break;
                        }
                    if (cheapest <= q.crf_lo + 1e-9)
                        continue;   /* out of reach at the bound */
                    CHECK(w[nw - 1].vmaf >= q.target);
                    CHECK(w[nw - 1].crf > cheapest - 1.0);
                    if (fails) {
                        fprintf(stderr, "  fix: mid %.0f k %.2f target %.0f "
                                "gap %.1f: crf %.2f -> %.2f in %d, cheapest "
                                "%.2f\n", mids[a], ks[b], targets[t], gap,
                                r.crf, w[nw - 1].crf, nw - 1, cheapest);
                        return 1;
                    }
                }

    /* the model follows the class and the frame rate */
    tv_scorer s = { .w = 352, .h = 288, .fps = 30.0 };
    CHECK(!strcmp(tv_model_name(&s), "vmaf_v1.0.16_3d0h"));
    CHECK(tv_upscaled(352, 288) && !tv_upscaled(1920, 800));
    s.w = 3840; s.h = 1714; s.fps = 59.94;
    CHECK(!strcmp(tv_model_name(&s), "vmaf_v1.0.16_hfr_1d5h_2160"));
    s.w = 2048; s.h = 858; s.fps = 24.0; s.hdr = 1;
    CHECK(!strcmp(tv_model_name(&s), "hdr_vmaf_v1.1.8_3d0h"));
    s.model_dir = "/m";
    char path[256];
    CHECK(tv_model_path(&s, path, sizeof path) == 0);
    CHECK(!strcmp(path, "/m/hdr_vmaf_v1.1.8/hdr_vmaf_v1.1.8_3d0h.json"));

    if (fails)
        fprintf(stderr, "tvmaf: %d failures\n", fails);
    return fails != 0;
}
