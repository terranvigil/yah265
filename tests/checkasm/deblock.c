/*
 * checkasm/deblock.c - the deblocking filters (class N265_ASM_DBK).
 * See checkasm.h for the harness and the convention.
 *
 * Random samples almost never pass the luma filter's d < beta test, so
 * each segment is drawn as a level and a step across the edge with a
 * little noise on top, at a beta and tc from the spec's tables. That
 * reaches the strong filter, the normal filter with and without the p1
 * and q1 legs, a step too tall for it, and the clip ends. The run counts which paths the C took
 * and fails if one was never reached.
 *
 * Copyright (c) 2026, the yah265 authors. GPL-2.0-or-later; see LICENSE.
 */

#include "checkasm.h"

#include "encoder/deblock.h"

static const uint8_t ca_beta_tab[52] = {
     0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
     6,  7,  8,  9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 20, 22, 24,
    26, 28, 30, 32, 34, 36, 38, 40, 42, 44, 46, 48, 50, 52, 54, 56,
    58, 60, 62, 64
};
static const uint8_t ca_tc_tab[54] = {
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    1, 1, 1, 1, 1, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4,
    4, 4, 5, 5, 6, 6, 7, 8, 9, 10, 11, 13, 14, 16, 18, 20, 22, 24
};

static int clipp(int v)
{
    return v < 0 ? 0 : v > PIXEL_MAX ? PIXEL_MAX : v;
}

/* one line across the edge: `len` samples each side of it */
static void fill_line(pixel *p, ptrdiff_t step, int len, int kind, int lvl,
                      int stp, int amp)
{
    for (int i = -len; i < len; i++) {
        int v;
        switch (kind) {
        case 0:     /* random */
            v = (int)ca_pix();
            break;
        case 1:     /* a clip end */
            v = (rnd() & 1) ? PIXEL_MAX : 0;
            break;
        default:    /* level, step and noise */
            v = lvl + (i >= 0 ? stp : 0) +
                (amp ? (int)(rnd() % (2 * amp + 1)) - amp : 0);
            break;
        }
        p[i * step] = (pixel)clipp(v);
    }
}

static int pick_level(void)
{
    switch (rnd() % 4) {
    case 0:  return (int)(rnd() % (8 << BIT_DEPTH_SHIFT));
    case 1:  return PIXEL_MAX - (int)(rnd() % (8 << BIT_DEPTH_SHIFT));
    default: return (int)ca_pix();
    }
}

static void pick_bt(int16_t *beta, int16_t *tc)
{
    int q = (int)(rnd() % 64) - 6;
    int bs = 1 + (int)(rnd() & 1);
    int b = q < 0 ? 0 : q > 51 ? 51 : q;
    int t = q + 2 * (bs - 1);
    t = t < 0 ? 0 : t > 53 ? 53 : t;
    *beta = (int16_t)(ca_beta_tab[b] << BIT_DEPTH_SHIFT);
    *tc = (rnd() % 8) ? (int16_t)(ca_tc_tab[t] << BIT_DEPTH_SHIFT) : 0;
}

/* a segment's lines: kind, level, step and noise held for its four */
static void fill_segment(pixel *q0, ptrdiff_t across, ptrdiff_t along,
                         int nlines, int len)
{
    int kind = (rnd() % 8) == 0 ? 0 : (rnd() % 8) == 0 ? 1 : 2;
    int lvl = pick_level();
    int stp = (int)(rnd() % (24 << BIT_DEPTH_SHIFT)) -
              (12 << BIT_DEPTH_SHIFT);
    if (rnd() % 3 == 0)         /* past 10 * tc: the normal filter's skip */
        stp = (int)(rnd() % (PIXEL_MAX + 1)) - PIXEL_MAX / 2;
    int amp = (int)(rnd() % 4) << BIT_DEPTH_SHIFT;
    if (rnd() & 1)
        amp = (int)(rnd() % 2);
    for (int l = 0; l < nlines; l++)
        fill_line(q0 + l * along, across, len, kind, lvl, stp, amp);
}

enum { LUMA_V, LUMA_H, CHROMA_V, CHROMA_H, NKERN };
static const char *const kname[NKERN] = {
    "dbk_luma_v", "dbk_luma_h", "dbk_chroma_v", "dbk_chroma_h"
};

typedef void (*luma_fn)(pixel *, ptrdiff_t, const int16_t *,
                        const int16_t *, int);
typedef void (*chroma_fn)(pixel *, ptrdiff_t, const int16_t *, int);

static const luma_fn luma_ref[2] = { n265_dbk_luma_v_ref,
                                     n265_dbk_luma_h_ref };
static const chroma_fn chroma_ref[2] = { n265_dbk_chroma_v_ref,
                                         n265_dbk_chroma_h_ref };

/* the window a call declares: its first sample, width and height, and
 * where q0 of line 0 sits in it */
struct win { int w, h; ptrdiff_t q0; };

static struct win window(int kern, int n, int stride)
{
    struct win wv;
    switch (kern) {
    case LUMA_V:   wv.w = 8; wv.h = 4 * n; wv.q0 = 4; break;
    case LUMA_H:   wv.w = 4 * n; wv.h = 8; wv.q0 = 4 * (ptrdiff_t)stride; break;
    case CHROMA_V: wv.w = 4; wv.h = n; wv.q0 = 2; break;
    default:       wv.w = n; wv.h = 4; wv.q0 = 2 * (ptrdiff_t)stride; break;
    }
    return wv;
}

/* fill a window and its params; `n` is segments (luma) or lines (chroma) */
static void fill_case(int kern, pixel *base, int stride, int n,
                      int16_t *beta, int16_t *tc)
{
    struct win wv = window(kern, n, stride);
    pixel *q0 = base + wv.q0;
    ca_fill_pix(base, (size_t)(wv.h - 1) * stride + wv.w);
    if (kern == LUMA_V || kern == LUMA_H) {
        for (int k = 0; k < n; k++) {
            pick_bt(&beta[k], &tc[k]);
            if (kern == LUMA_V)
                fill_segment(q0 + 4 * k * (ptrdiff_t)stride, 1, stride, 4, 4);
            else
                fill_segment(q0 + 4 * k, stride, 1, 4, 4);
        }
    } else {
        for (int i = 0; i < n; i += 2) {
            int16_t b, t;
            int m = n - i < 2 ? n - i : 2;
            pick_bt(&b, &t);
            for (int j = 0; j < m; j++)
                tc[i + j] = t;
            if (kern == CHROMA_V)
                fill_segment(q0 + i * (ptrdiff_t)stride, 1, stride, m, 2);
            else
                fill_segment(q0 + i, stride, 1, m, 2);
        }
    }
}

static void run_ref(int kern, pixel *q0, int stride, const int16_t *beta,
                    const int16_t *tc, int n)
{
    if (kern < CHROMA_V)
        luma_ref[kern](q0, stride, beta, tc, n);
    else
        chroma_ref[kern - CHROMA_V](q0, stride, tc, n);
}

#if CA_SIMD_DEBLOCK
static void run_simd(int kern, pixel *q0, int stride, const int16_t *beta,
                     const int16_t *tc, int n)
{
    /* CA_SIMD picks the tier at run time on x86-64 */
    const luma_fn luma_simd[2] = { CA_SIMD(n265_dbk_luma_v),
                                   CA_SIMD(n265_dbk_luma_h) };
    const chroma_fn chroma_simd[2] = { CA_SIMD(n265_dbk_chroma_v),
                                       CA_SIMD(n265_dbk_chroma_h) };

    if (kern < CHROMA_V)
        luma_simd[kern](q0, stride, beta, tc, n);
    else
        chroma_simd[kern - CHROMA_V](q0, stride, tc, n);
}

/* which path the C took on each luma segment, from what it changed */
static void census(int kern, const pixel *before, const pixel *after,
                   int stride, int n, int cnt[3])
{
    struct win wv = window(kern, n, stride);
    for (int k = 0; k < n; k++) {
        int p2 = 0, p0 = 0;
        for (int l = 0; l < 4; l++) {
            ptrdiff_t o = wv.q0 + (kern == LUMA_V ? (4 * k + l) * (ptrdiff_t)stride
                                                  : 4 * k + l);
            ptrdiff_t s = kern == LUMA_V ? 1 : stride;
            p2 |= before[o - 3 * s] != after[o - 3 * s];
            p0 |= before[o - s] != after[o - s];
        }
        cnt[p2 ? 2 : p0 ? 1 : 0]++;
    }
}

static int check_one(int kern, int n, int stride, int cnt[3])
{
    struct win wv = window(kern, n, stride);
    size_t npix = (size_t)(wv.h - 1) * stride + wv.w;
    size_t bytes = npix * sizeof(pixel);
    pixel *src = ca_guard_alloc(bytes), *ref = ca_guard_alloc(bytes);
    pixel *got = ca_guard_alloc(bytes);
    int16_t beta[64], tc[64];
    int bad = 0;

    fill_case(kern, src, stride, n, beta, tc);
    memcpy(ref, src, bytes);
    run_ref(kern, ref + wv.q0, stride, beta, tc, n);
    if (kern < CHROMA_V)
        census(kern, src, ref, stride, n, cnt);

    for (int pass = 0; pass < 2 && !bad; pass++) {
        memcpy(got, src, bytes);
        ca_guard_poison(got, bytes);
        if (pass)
            ca_guard_scramble(got, bytes);
        run_simd(kern, got + wv.q0, stride, beta, tc, n);
        if (pass == 0)
            bad += ca_guard_check(got, bytes, kname[kern]);
        for (size_t i = 0; i < npix && !bad; i++)
            if (ref[i] != got[i]) {
                printf("    %s MISMATCH n %d stride %d at (%d,%d): "
                       "in %d ref %d simd %d\n", kname[kern], n, stride,
                       (int)(i % stride), (int)(i / stride), src[i],
                       ref[i], got[i]);
                bad++;
            }
    }

    /* the window and not a sample more */
    for (int tail = 0; tail < 2 && !bad; tail++) {
        ca_pg g;
        pixel *pg = ca_pg_alloc(&g, bytes, tail);
        char what[96];
        memcpy(pg, src, bytes);
        snprintf(what, sizeof(what), "%s n %d stride %d, %s edge",
                 kname[kern], n, stride, tail ? "far" : "near");
        ca_pg_arm(what);
        run_simd(kern, pg + wv.q0, stride, beta, tc, n);
        ca_pg_disarm();
        ca_pg_free(&g);
    }
    ca_guard_free(src);
    ca_guard_free(ref);
    ca_guard_free(got);
    return bad;
}

static int check_dbk_simd(void)
{
    int bad = 0;
    for (int kern = 0; kern < NKERN && !bad; kern++) {
        int cnt[3] = { 0, 0, 0 };
        int nmax = kern < CHROMA_V ? 16 : 64;
        for (int n = 1; n <= nmax && !bad; n++)
            for (int rep = 0; rep < 40 && !bad; rep++) {
                struct win wv = window(kern, n, 0);
                int stride = wv.w + (int)(rnd() % 24);
                bad += check_one(kern, n, stride, cnt);
            }
        if (!bad && kern < CHROMA_V && (!cnt[0] || !cnt[1] || !cnt[2])) {
            printf("    %s: a path was never reached (off %d normal %d "
                   "strong %d)\n", kname[kern], cnt[0], cnt[1], cnt[2]);
            bad++;
        }
    }
    return bad;
}
#endif /* CA_SIMD_DEBLOCK */

/* ---- bench ---------------------------------------------------------- */

struct dctx {
    int kern, n, stride;
    size_t npix;
    pixel buf[64 * 64], work[64 * 64];
    int16_t beta[64], tc[64];
};

static void prep(struct dctx *c)
{
    memcpy(c->work, c->buf, c->npix * sizeof(pixel));
}

static void dbk_c_call(void *v)
{
    struct dctx *c = v;
    struct win wv = window(c->kern, c->n, c->stride);
    prep(c);
    run_ref(c->kern, c->work + wv.q0, c->stride, c->beta, c->tc, c->n);
}

#if CA_SIMD_DEBLOCK
static void dbk_simd_call(void *v)
{
    struct dctx *c = v;
    struct win wv = window(c->kern, c->n, c->stride);
    prep(c);
    run_simd(c->kern, c->work + wv.q0, c->stride, c->beta, c->tc, c->n);
}
#define DBK_SIMD dbk_simd_call
#else
#define DBK_SIMD NULL
#endif

int check_deblock(void)
{
    int bad = 0;
#if CA_SIMD_DEBLOCK
    bad += check_dbk_simd();
#endif
    if (bad)
        return bad;

    /* a CTB's worth of segments with the encoder's mix of filtered and
     * unfiltered, at a tight stride; the copy-in is in both columns */
    static struct dctx c;
    static const int bn[NKERN] = { 16, 16, 32, 32 };
    for (int kern = 0; kern < NKERN; kern++) {
        char name[48];
        c.kern = kern;
        c.n = bn[kern];
        c.stride = window(kern, c.n, 0).w;
        c.npix = (size_t)window(kern, c.n, 0).h * c.stride;
        fill_case(kern, c.buf, c.stride, c.n, c.beta, c.tc);
        snprintf(name, sizeof(name), "%s n %d", kname[kern], c.n);
        bench(name, dbk_c_call, dbk_c_call, DBK_SIMD, &c, 20000);
    }
    return 0;
}
