/*
 * checkasm/sao.c - the SAO kernels (class N265_ASM_SAO).
 * See checkasm.h for the harness and the convention.
 *
 * The band kernel takes one of two routes per strip of rows, so the
 * populations are chosen by how many bands a block spans: one (a flat
 * fill), a few (a fill with noise across a band edge) and many (random).
 * The run counts strips of each kind and fails if the narrow kind never
 * occurs. The edge kernels need equal neighbors to reach categories 2 and
 * 3, which random samples almost never give, so their rows are drawn
 * from a few nearby levels.
 *
 * Copyright (c) 2026, the yah265 authors. GPL-2.0-or-later; see LICENSE.
 */

#include "checkasm.h"

#include "encoder/sao.h"

enum { POP_FLAT, POP_NARROW, POP_WIDE, POP_EDGE, NPOP };
static const char *const pop_name[NPOP] = { "flat", "narrow", "wide", "edge" };

static pixel draw(int pop, int lvl)
{
    int v;
    switch (pop) {
    case POP_FLAT:   v = lvl; break;
    case POP_NARROW: v = lvl + (int)(rnd() % (12 << BIT_DEPTH_SHIFT)); break;
    case POP_EDGE:   v = (rnd() & 1) ? PIXEL_MAX : 0; break;
    default:         v = (int)ca_pix(); break;
    }
    return (pixel)(v < 0 ? 0 : v > PIXEL_MAX ? PIXEL_MAX : v);
}

static void fill_pop(pixel *p, int stride, int w, int h, int pop)
{
    int lvl = (int)ca_pix();
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++)
            p[(size_t)y * stride + x] = draw(pop, lvl);
}

/* a few levels close together, so neighbors are often equal */
static void fill_levels(pixel *p, int n)
{
    int lvl = (int)ca_pix(), spread = 1 + (int)(rnd() % 3);
    for (int x = 0; x < n; x++) {
        int v = (rnd() % 5 == 0) ? (int)ca_pix()
                                 : lvl + (int)(rnd() % (2 * spread + 1)) - spread;
        p[x] = (pixel)(v < 0 ? 0 : v > PIXEL_MAX ? PIXEL_MAX : v);
    }
}

static void fill_off(int8_t off[4])
{
    for (int i = 0; i < 4; i++)
        off[i] = (int8_t)((int)(rnd() % 63) - 31);
}

#if CA_SIMD_SAO

static int check_band_stats(void)
{
    int bad = 0, narrow = 0;
    for (int rep = 0; rep < 3000 && !bad; rep++) {
        int w = 1 + (int)(rnd() % 64), h = 1 + (int)(rnd() % 64);
        int pop = (int)(rnd() % 3);
        if (rep % 4 == 0)
            w = 16 << (rnd() % 3);
        int ss = w + (int)(rnd() % 16), rs = w + (int)(rnd() % 16);
        size_t sb = ca_pg_window_pix(ss, w, h), rb = ca_pg_window_pix(rs, w, h);
        pixel *src = ca_guard_alloc(sb), *rec = ca_guard_alloc(rb);
        int64_t rn[32], rd[32], gn[32], gd[32];

        fill_pop(src, ss, w, h, (int)(rnd() % 3));
        fill_pop(rec, rs, w, h, pop);
        narrow += pop != POP_WIDE && w >= 16;
        for (int b = 0; b < 32; b++) {
            rn[b] = gn[b] = (int64_t)(rnd() % 1000);
            rd[b] = gd[b] = (int64_t)(rnd() % 2000) - 1000;
        }
        n265_sao_band_stats_ref(src, ss, rec, rs, w, h, rn, rd);
        ca_guard_scramble(src, sb);
        ca_guard_scramble(rec, rb);
        CA_SIMD(n265_sao_band_stats)(src, ss, rec, rs, w, h, gn, gd);
        for (int b = 0; b < 32 && !bad; b++)
            if (rn[b] != gn[b] || rd[b] != gd[b]) {
                printf("    sao_band_stats MISMATCH %dx%d %s band %d: ref "
                       "%lld/%lld simd %lld/%lld\n", w, h, pop_name[pop], b,
                       (long long)rn[b], (long long)rd[b],
                       (long long)gn[b], (long long)gd[b]);
                bad++;
            }
        ca_guard_free(src);
        ca_guard_free(rec);

        if (rep % 50)
            continue;
        for (int tail = 0; tail < 2 && !bad; tail++) {
            ca_pg g1, g2;
            pixel *s2 = ca_pg_alloc(&g1, sb, tail), *r2 = ca_pg_alloc(&g2, rb, tail);
            char what[80];
            fill_pop(s2, ss, w, h, POP_WIDE);
            fill_pop(r2, rs, w, h, pop);
            snprintf(what, sizeof(what), "sao_band_stats %dx%d, %s edge", w, h,
                     tail ? "far" : "near");
            ca_pg_arm(what);
            CA_SIMD(n265_sao_band_stats)(s2, ss, r2, rs, w, h, gn, gd);
            ca_pg_disarm();
            ca_pg_free(&g1);
            ca_pg_free(&g2);
        }
    }
    if (!bad && !narrow) {
        printf("    sao_band_stats: no narrow block was drawn\n");
        bad++;
    }
    return bad;
}

/* the four rows an edge kernel reads, `n` samples each */
struct eorow { pixel *src, *cur, *a, *b, *dst; };

static int check_eo(void)
{
    int bad = 0, cat_seen[5] = { 0 };
    for (int rep = 0; rep < 4000 && !bad; rep++) {
        int n = 1 + (int)(rnd() % 80);
        size_t bytes = (size_t)n * sizeof(pixel);
        pixel *src = ca_guard_alloc(bytes), *cur = ca_guard_alloc(bytes);
        pixel *a = ca_guard_alloc(bytes), *b = ca_guard_alloc(bytes);
        pixel *dr = ca_guard_alloc(bytes), *dg = ca_guard_alloc(bytes);
        int64_t rn[5], rd[5], gn[5], gd[5];
        int8_t off[4];

        ca_fill_pix(src, n);
        if (rnd() % 4) {
            fill_levels(cur, n);
            for (int x = 0; x < n; x++) {
                a[x] = (rnd() & 1) ? cur[x] : (pixel)(cur[x] ^ (rnd() & 3));
                b[x] = (rnd() & 1) ? cur[x] : (pixel)(cur[x] ^ (rnd() & 3));
            }
        } else {
            ca_fill_pix(cur, n);
            ca_fill_pix(a, n);
            ca_fill_pix(b, n);
        }
        for (int k = 0; k < 5; k++) {
            rn[k] = gn[k] = (int64_t)(rnd() % 100);
            rd[k] = gd[k] = (int64_t)(rnd() % 200) - 100;
        }
        n265_sao_eo_stats_ref(src, cur, a, b, n, rn, rd);
        for (int k = 1; k < 5; k++)
            cat_seen[k] += rn[k] != gn[k];
        CA_SIMD(n265_sao_eo_stats)(src, cur, a, b, n, gn, gd);
        for (int k = 0; k < 5 && !bad; k++)
            if (rn[k] != gn[k] || rd[k] != gd[k]) {
                printf("    sao_eo_stats MISMATCH n %d cat %d: ref %lld/%lld "
                       "simd %lld/%lld\n", n, k, (long long)rn[k],
                       (long long)rd[k], (long long)gn[k], (long long)gd[k]);
                bad++;
            }

        fill_off(off);
        ca_fill_pix(dr, n);
        memcpy(dg, dr, bytes);
        ca_guard_poison(dg, bytes);
        n265_sao_eo_apply_ref(dr, cur, a, b, n, off);
        CA_SIMD(n265_sao_eo_apply)(dg, cur, a, b, n, off);
        bad += ca_guard_check(dg, bytes, "sao_eo_apply");
        for (int x = 0; x < n && !bad; x++)
            if (dr[x] != dg[x]) {
                printf("    sao_eo_apply MISMATCH n %d at %d: ref %d simd %d\n",
                       n, x, dr[x], dg[x]);
                bad++;
            }

        int pos = (int)(rnd() % 32);
        if (rnd() & 1)          /* aim the bands at the row */
            pos = (cur[0] >> (BIT_DEPTH - 5)) - (int)(rnd() % 4);
        pos = pos < 0 ? 0 : pos;
        ca_fill_pix(dr, n);
        memcpy(dg, dr, bytes);
        ca_guard_poison(dg, bytes);
        n265_sao_bo_apply_ref(dr, cur, n, pos, off);
        CA_SIMD(n265_sao_bo_apply)(dg, cur, n, pos, off);
        bad += ca_guard_check(dg, bytes, "sao_bo_apply");
        for (int x = 0; x < n && !bad; x++)
            if (dr[x] != dg[x]) {
                printf("    sao_bo_apply MISMATCH n %d pos %d at %d: ref %d "
                       "simd %d\n", n, pos, x, dr[x], dg[x]);
                bad++;
            }
        ca_guard_free(src);
        ca_guard_free(cur);
        ca_guard_free(a);
        ca_guard_free(b);
        ca_guard_free(dr);
        ca_guard_free(dg);

        if (rep % 100 || bad)
            continue;
        for (int tail = 0; tail < 2; tail++) {
            ca_pg g[5];
            pixel *p[5];
            char what[64];
            for (int i = 0; i < 5; i++) {
                p[i] = ca_pg_alloc(&g[i], bytes, tail);
                ca_fill_pix(p[i], n);
            }
            snprintf(what, sizeof(what), "sao edge/band rows n %d, %s edge", n,
                     tail ? "far" : "near");
            ca_pg_arm(what);
            CA_SIMD(n265_sao_eo_stats)(p[0], p[1], p[2], p[3], n, gn, gd);
            CA_SIMD(n265_sao_eo_apply)(p[4], p[1], p[2], p[3], n, off);
            CA_SIMD(n265_sao_bo_apply)(p[4], p[1], n, pos, off);
            ca_pg_disarm();
            for (int i = 0; i < 5; i++)
                ca_pg_free(&g[i]);
        }
    }
    for (int k = 1; k < 5 && !bad; k++)
        if (!cat_seen[k]) {
            printf("    sao_eo_stats: category %d was never reached\n", k);
            bad++;
        }
    return bad;
}
#endif /* CA_SIMD_SAO */

/* ---- bench ---------------------------------------------------------- */

struct sctx {
    int w, h, pos;
    pixel src[64 * 64], rec[64 * 64], a[64], b[64], dst[64];
    int64_t bn[32], bd[32], en[5], ed[5];
    int8_t off[4];
};

static void band_c(void *v)
{
    struct sctx *c = v;
    n265_sao_band_stats_ref(c->src, c->w, c->rec, c->w, c->w, c->h, c->bn,
                            c->bd);
}

static void eo_stats_c(void *v)
{
    struct sctx *c = v;
    n265_sao_eo_stats_ref(c->src, c->rec, c->a, c->b, c->w, c->en, c->ed);
}

static void eo_apply_c(void *v)
{
    struct sctx *c = v;
    n265_sao_eo_apply_ref(c->dst, c->rec, c->a, c->b, c->w, c->off);
}

static void bo_apply_c(void *v)
{
    struct sctx *c = v;
    n265_sao_bo_apply_ref(c->dst, c->rec, c->w, c->pos, c->off);
}

#if CA_SIMD_SAO
static void band_s(void *v)
{
    struct sctx *c = v;
    CA_SIMD(n265_sao_band_stats)(c->src, c->w, c->rec, c->w, c->w, c->h,
                                 c->bn, c->bd);
}

static void eo_stats_s(void *v)
{
    struct sctx *c = v;
    CA_SIMD(n265_sao_eo_stats)(c->src, c->rec, c->a, c->b, c->w, c->en, c->ed);
}

static void eo_apply_s(void *v)
{
    struct sctx *c = v;
    CA_SIMD(n265_sao_eo_apply)(c->dst, c->rec, c->a, c->b, c->w, c->off);
}

static void bo_apply_s(void *v)
{
    struct sctx *c = v;
    CA_SIMD(n265_sao_bo_apply)(c->dst, c->rec, c->w, c->pos, c->off);
}
#define SIMD_OR_NULL(f) f
#else
#define SIMD_OR_NULL(f) NULL
#endif

int check_sao(void)
{
    int bad = 0;
#if CA_SIMD_SAO
    bad += check_band_stats();
    if (!bad)
        bad += check_eo();
#endif
    if (bad)
        return bad;

    static struct sctx c;
    static const int pops[3] = { POP_FLAT, POP_NARROW, POP_WIDE };
    for (int i = 0; i < 3; i++)
        for (int w = 32; w <= 64; w *= 2) {
            char name[48];
            c.w = c.h = w;
            fill_pop(c.src, w, w, w, POP_WIDE);
            fill_pop(c.rec, w, w, w, pops[i]);
            snprintf(name, sizeof(name), "sao_band_stats %dx%d %s", w, w,
                     pop_name[pops[i]]);
            bench(name, band_c, band_c, SIMD_OR_NULL(band_s), &c, 20000);
        }
    c.w = 64;
    fill_levels(c.rec, 64);
    fill_levels(c.a, 64);
    fill_levels(c.b, 64);
    fill_off(c.off);
    c.pos = c.rec[0] >> (BIT_DEPTH - 5);
    bench("sao_eo_stats row 64", eo_stats_c, eo_stats_c,
          SIMD_OR_NULL(eo_stats_s), &c, 200000);
    bench("sao_eo_apply row 64", eo_apply_c, eo_apply_c,
          SIMD_OR_NULL(eo_apply_s), &c, 200000);
    bench("sao_bo_apply row 64", bo_apply_c, bo_apply_c,
          SIMD_OR_NULL(bo_apply_s), &c, 200000);
    return 0;
}
