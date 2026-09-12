/*
 * checkasm/intra.c - the intra family (class N265_ASM_INTRA).
 * See checkasm.h for the harness and the convention.
 *
 * The family is three entry points with very different failure modes, so
 * it gets three checks:
 *
 *   n265_intra_pred    35 modes x 4 sizes x 2 components, and every one of
 *                      the 280 combinations is checked at every input
 *                      kind - the mode number picks the angle, the main
 *                      direction, whether the projected reference is
 *                      built at all, and which edge filter runs, so there
 *                      is no such thing as a representative mode here.
 *   n265_filter_refs   the [1 2 1] run, plus the four seam samples the
 *                      spec treats specially and the strong-smoothing
 *                      branch, which needs an input flat enough to take
 *                      it (kind 3 builds one).
 *   n265_intra_refs    a whole reconstructed plane and a position in it.
 *                      The interesting inputs are the picture edges and
 *                      the z-scan boundaries, where availability changes,
 *                      so the geometry is enumerated rather than random.
 *
 * Copyright (c) 2026, the yah265 authors. GPL-2.0-or-later; see LICENSE.
 */

#include "checkasm.h"

#include "common/cpu.h"
#include "encoder/intra.h"
#include "encoder/arm/neon.h"   /* self-guarded */

/* ---- input generators -----------------------------------------------
 *
 * Random is the weakest. What the angular kernel can actually get wrong
 * is a lane whose accumulator is one bit too narrow, and the input that
 * says so is kind 1: every reference sample at a clip end, which drives
 * (32-f)*a + f*b to its bound of 32 * PIXEL_MAX - and, at Main10,
 * planar's four weighted terms to 2n * PIXEL_MAX + n = 65504, which is
 * the tightest u16 claim in the family. Kind 3 is the one filter_refs
 * needs, a near-linear boundary, because that is the only input that
 * reaches the strong-smoothing branch at 32x32.
 */
#define NKIND 5

static void fill_refs(pixel *refs, int n, int kind)
{
    int total = 4 * n + 1;
    int a = (int)ca_pix(), b = (int)ca_pix();

    for (int i = 0; i < total; i++) {
        int v;
        switch (kind) {
        case 0: v = (int)ca_pix(); break;
        case 1: v = (rnd() & 1) ? PIXEL_MAX : 0; break;
        case 2: v = a; break;                       /* flat */
        case 3:                                     /* near-linear ramp */
            v = a + ((b - a) * i) / total + (int)(rnd() % 3) - 1;
            v = v < 0 ? 0 : v > PIXEL_MAX ? PIXEL_MAX : v;
            break;
        default: v = (i & 1) ? PIXEL_MAX : 0; break;  /* alternating */
        }
        refs[i] = (pixel)v;
    }
}

/* ---- n265_intra_pred ------------------------------------------------ */

static void pred_opt(pixel *dst, int ds, const pixel *refs, int log2n,
                     int mode, int c_idx)
{
#if N265_NEON_INTRA
    n265_intra_pred_neon(dst, ds, refs, log2n, mode, c_idx);
#else
    n265_intra_pred(dst, ds, refs, log2n, mode, c_idx);
#endif
}

static int check_pred(void)
{
    int bad = 0;

    for (int log2n = 2; log2n <= 5 && !bad; log2n++) {
        int n = 1 << log2n;
        int total = 4 * n + 1;
        /* two destination strides per size: tight, where the guard border
         * sits against the last written sample, and padded by an amount
         * that is not a multiple of the vector width, where a store of
         * eight on a four-wide row lands in padding ca_check_pad sees */
        for (int is = 0; is < 2 && !bad; is++) {
            int ds = is ? n + 13 : n;
            size_t dbytes = (size_t)ds * n * sizeof(pixel);
            pixel *dst = ca_guard_alloc(dbytes);
            pixel *ref = ca_guard_alloc(dbytes);
            size_t rbytes = (size_t)total * sizeof(pixel);
            pixel *refs = ca_guard_alloc(rbytes);

            for (int kind = 0; kind < NKIND && !bad; kind++) {
                for (int mode = 0; mode < 35 && !bad; mode++) {
                    for (int c_idx = 0; c_idx < 2 && !bad; c_idx++) {
                        char what[64];

                        snprintf(what, sizeof(what),
                                 "pred %dx%d mode %d c%d s%d k%d", n, n,
                                 mode, c_idx, is, kind);
                        fill_refs(refs, n, kind);
                        ca_guard_poison(refs, rbytes);
                        memset(dst, CA_POISON, dbytes);
                        memset(ref, CA_POISON, dbytes);
                        ca_guard_poison(dst, dbytes);
                        ca_guard_poison(ref, dbytes);

                        n265_intra_pred_ref(ref, ds, refs, log2n, mode,
                                            c_idx);
                        pred_opt(dst, ds, refs, log2n, mode, c_idx);

                        for (int y = 0; y < n && !bad; y++)
                            for (int x = 0; x < n; x++)
                                if (dst[(size_t)y * ds + x] !=
                                    ref[(size_t)y * ds + x]) {
                                    printf("    %s MISMATCH at (%d,%d): "
                                           "ref %u opt %u\n", what, x, y,
                                           ref[(size_t)y * ds + x],
                                           dst[(size_t)y * ds + x]);
                                    bad++;
                                    break;
                                }
                        if (bad)
                            break;
                        bad += ca_guard_check(dst, dbytes, what);
                        bad += ca_check_pad_pix(dst, ds, n, n, what);
                        /* the kernel must not write its reference array */
                        bad += ca_guard_check(refs, rbytes, what);
                        if (bad)
                            break;

                        /* the answer must not depend on anything outside
                         * the 4n+1 references it was handed */
                        ca_guard_scramble(refs, rbytes);
                        memset(dst, CA_POISON, dbytes);
                        pred_opt(dst, ds, refs, log2n, mode, c_idx);
                        for (int y = 0; y < n && !bad; y++)
                            for (int x = 0; x < n; x++)
                                if (dst[(size_t)y * ds + x] !=
                                    ref[(size_t)y * ds + x]) {
                                    printf("    %s: result depends on "
                                           "bytes OUTSIDE the reference "
                                           "array\n", what);
                                    bad++;
                                    break;
                                }
                    }
                }
            }
            ca_guard_free(dst);
            ca_guard_free(ref);
            ca_guard_free(refs);
        }
    }
    return bad;
}

/* ---- n265_filter_refs ----------------------------------------------- */

static void filt_opt(pixel *out, const pixel *in, int log2n)
{
#if N265_NEON_INTRA
    n265_filter_refs_neon(out, in, log2n);
#else
    n265_filter_refs(out, in, log2n);
#endif
}

static int check_filter(void)
{
    int bad = 0;

    for (int log2n = 2; log2n <= 5 && !bad; log2n++) {
        int n = 1 << log2n;
        size_t total = (size_t)(4 * n + 1) * sizeof(pixel);
        pixel *in = ca_guard_alloc(total);
        pixel *out = ca_guard_alloc(total);
        pixel *ref = ca_guard_alloc(total);

        for (int kind = 0; kind < NKIND && !bad; kind++) {
            for (int rep = 0; rep < 64 && !bad; rep++) {
                char what[48];

                snprintf(what, sizeof(what), "filter_refs %d k%d", n, kind);
                fill_refs(in, n, kind);
                ca_guard_poison(in, total);
                memset(out, CA_POISON, total);
                memset(ref, CA_POISON, total);
                ca_guard_poison(out, total);
                ca_guard_poison(ref, total);

                n265_filter_refs_ref(ref, in, log2n);
                filt_opt(out, in, log2n);
                if (memcmp(out, ref, total)) {
                    for (size_t i = 0; i < total; i++)
                        if (out[i] != ref[i]) {
                            printf("    %s MISMATCH at %zu: ref %u opt %u\n",
                                   what, i, ref[i], out[i]);
                            break;
                        }
                    bad++;
                    break;
                }
                bad += ca_guard_check(out, total, what);
                bad += ca_guard_check(in, total, what);
            }
        }
        ca_guard_free(in);
        ca_guard_free(out);
        ca_guard_free(ref);
    }
    return bad;
}

/* ---- n265_intra_refs ------------------------------------------------
 *
 * The geometry, not the samples, is what this kernel gets wrong, so the
 * positions are enumerated: the picture corners and edges, the first and
 * last TB of a CTB row, and a scatter of interior positions at every
 * alignment a TB of that size can have. Every one of them is run at three
 * CTB sizes, because the z-scan key - and so which neighbours count as
 * already decoded - is a function of ctb_log2.
 */
static void refs_opt(pixel *refs, const pixel *recon, int stride,
                     int x0, int y0, int log2n, int pic_w, int pic_h,
                     int ctb_log2)
{
    n265_intra_refs_ref(refs, recon, stride, x0, y0, log2n, pic_w, pic_h,
                        ctb_log2);
}

static int check_refs(void)
{
    int bad = 0;
    /* a padded CIF-ish plane and a small one, so a TB at the far edge is
     * reachable in a test that runs in milliseconds */
    static const int planes[][2] = { { 352, 288 }, { 64, 64 }, { 80, 48 } };

    for (int p = 0; p < 3 && !bad; p++) {
        int pic_w = planes[p][0], pic_h = planes[p][1];
        int stride = pic_w + 21;
        size_t bytes = (size_t)stride * pic_h * sizeof(pixel);
        pixel *recon = ca_guard_alloc(bytes);

        ca_fill_pix(recon, bytes / sizeof(pixel));
        ca_guard_poison(recon, bytes);

        for (int ctb_log2 = 4; ctb_log2 <= 6 && !bad; ctb_log2++) {
            for (int log2n = 2; log2n <= 5 && !bad; log2n++) {
                int n = 1 << log2n;
                size_t total = (size_t)(4 * n + 1) * sizeof(pixel);
                pixel *got = ca_guard_alloc(total);
                pixel *want = ca_guard_alloc(total);

                for (int y0 = 0; y0 + n <= pic_h && !bad; y0 += n)
                    for (int x0 = 0; x0 + n <= pic_w && !bad; x0 += n) {
                        char what[72];

                        /* the interior is uniform once availability has
                         * settled; sample it rather than walk it */
                        if (x0 > 2 * (1 << ctb_log2) &&
                            y0 > 2 * (1 << ctb_log2) &&
                            x0 + n + 2 * (1 << ctb_log2) < pic_w &&
                            (rnd() & 7))
                            continue;
                        snprintf(what, sizeof(what),
                                 "intra_refs %d at (%d,%d) ctb %d pic %dx%d",
                                 n, x0, y0, 1 << ctb_log2, pic_w, pic_h);
                        memset(got, CA_POISON, total);
                        memset(want, CA_POISON, total);
                        ca_guard_poison(got, total);
                        ca_guard_poison(want, total);

                        n265_intra_refs_spec(want, recon, stride, x0, y0,
                                             log2n, pic_w, pic_h, ctb_log2);
                        refs_opt(got, recon, stride, x0, y0, log2n, pic_w,
                                 pic_h, ctb_log2);
                        if (memcmp(got, want, total)) {
                            for (size_t i = 0; i < total; i++)
                                if (got[i] != want[i]) {
                                    printf("    %s MISMATCH at %zu: "
                                           "ref %u opt %u\n", what, i,
                                           want[i], got[i]);
                                    break;
                                }
                            bad++;
                            break;
                        }
                        bad += ca_guard_check(got, total, what);
                        bad += ca_guard_check(recon, bytes, what);
                    }
                ca_guard_free(got);
                ca_guard_free(want);
            }
        }
        ca_guard_free(recon);
    }
    return bad;
}

/* ---- read windows ----------------------------------------------------
 *
 * The guard borders above catch a read whose value reaches the output.
 * These catch the one whose value is thrown away: a mapping sized to the
 * exact declared window with an unmapped page hard against it, run once
 * at each edge. The prediction and the smoothing filter declare the
 * 4n+1 reference array; the gather declares the picture, through the last
 * sample of the last row and not one byte of that row's padding.
 */
static void window_pred(void)
{
    for (int tail = 0; tail < 2; tail++)
        for (int log2n = 2; log2n <= 5; log2n++) {
            int n = 1 << log2n;
            size_t total = (size_t)(4 * n + 1) * sizeof(pixel);
            pixel dst[32 * 32];

            for (int mode = 0; mode < 35; mode++)
                for (int c_idx = 0; c_idx < 2; c_idx++) {
                    ca_pg g;
                    pixel *refs = ca_pg_alloc(&g, total, tail);
                    char what[80];

                    ca_pg_fill_pix(&g, total / sizeof(pixel));
                    snprintf(what, sizeof(what),
                             "intra_pred %d mode %d c%d, %s edge", n, mode,
                             c_idx, tail ? "far" : "near");
                    ca_pg_arm(what);
                    pred_opt(dst, n, refs, log2n, mode, c_idx);
                    ca_pg_disarm();
                    ca_pg_free(&g);
                }
        }
}

static void window_filter(void)
{
    for (int tail = 0; tail < 2; tail++)
        for (int log2n = 2; log2n <= 5; log2n++) {
            int n = 1 << log2n;
            size_t total = (size_t)(4 * n + 1) * sizeof(pixel);
            pixel out[4 * 32 + 1];
            ca_pg g;
            pixel *in = ca_pg_alloc(&g, total, tail);
            char what[64];

            ca_pg_fill_pix(&g, total / sizeof(pixel));
            snprintf(what, sizeof(what), "filter_refs %d, %s edge", n,
                     tail ? "far" : "near");
            ca_pg_arm(what);
            filt_opt(out, in, log2n);
            ca_pg_disarm();
            ca_pg_free(&g);
        }
}

static void window_refs(void)
{
    int pic_w = 80, pic_h = 48, stride = 96;

    for (int tail = 0; tail < 2; tail++)
        for (int ctb_log2 = 4; ctb_log2 <= 6; ctb_log2++)
            for (int log2n = 2; log2n <= 5; log2n++) {
                int n = 1 << log2n;
                pixel refs[4 * 32 + 1];
                size_t win = ca_pg_window_pix(stride, pic_w, pic_h);
                ca_pg g;
                pixel *recon = ca_pg_alloc(&g, win, tail);

                ca_pg_fill_pix(&g, win / sizeof(pixel));
                for (int y0 = 0; y0 + n <= pic_h; y0 += n)
                    for (int x0 = 0; x0 + n <= pic_w; x0 += n) {
                        char what[80];

                        snprintf(what, sizeof(what),
                                 "intra_refs %d at (%d,%d) ctb %d, %s edge",
                                 n, x0, y0, 1 << ctb_log2,
                                 tail ? "far" : "near");
                        ca_pg_arm(what);
                        refs_opt(refs, recon, stride, x0, y0, log2n, pic_w,
                                 pic_h, ctb_log2);
                        ca_pg_disarm();
                    }
                ca_pg_free(&g);
            }
}

/* ---- bench ----------------------------------------------------------- */

struct pctx {
    pixel *dst;
    const pixel *refs;
    int ds, log2n, mode;
};

static void pred_ref_call(void *v)
{
    struct pctx *c = v;
    n265_intra_pred_ref(c->dst, c->ds, c->refs, c->log2n, c->mode, 0);
}

static void pred_opt_call(void *v)
{
    struct pctx *c = v;
    pred_opt(c->dst, c->ds, c->refs, c->log2n, c->mode, 0);
}

struct fctx {
    pixel *out;
    const pixel *in;
    int log2n;
};

static void filt_ref_call(void *v)
{
    struct fctx *c = v;
    n265_filter_refs_ref(c->out, c->in, c->log2n);
}

static void filt_opt_call(void *v)
{
    struct fctx *c = v;
    filt_opt(c->out, c->in, c->log2n);
}

struct rctx {
    pixel *refs;
    const pixel *recon;
    int stride, x0, y0, log2n, pic_w, pic_h, ctb_log2;
};

static void refs_spec_call(void *v)
{
    struct rctx *c = v;
    n265_intra_refs_spec(c->refs, c->recon, c->stride, c->x0, c->y0,
                         c->log2n, c->pic_w, c->pic_h, c->ctb_log2);
}

static void refs_opt_call(void *v)
{
    struct rctx *c = v;
    refs_opt(c->refs, c->recon, c->stride, c->x0, c->y0, c->log2n,
             c->pic_w, c->pic_h, c->ctb_log2);
}

/* planar, DC, an angle with the top as the main reference in each sign,
 * and one with the left, which is the arm that has to transpose */
static const int bench_modes[] = { 0, 1, 29, 22, 13 };
static const char *const bench_names[] = {
    "planar", "dc", "ang29 +top", "ang22 -top", "ang13 -left"
};

static void bench_intra(void)
{
    pixel refs[4 * 32 + 1], dst[36 * 32], out[4 * 32 + 1];
    struct pctx pc;
    struct fctx fc;

    for (int log2n = 2; log2n <= 5; log2n++) {
        int n = 1 << log2n;

        fill_refs(refs, n, 0);
        pc.dst = dst;
        pc.refs = refs;
        pc.ds = n;
        pc.log2n = log2n;
        for (int m = 0; m < 5; m++) {
            char name[40];

            pc.mode = bench_modes[m];
            snprintf(name, sizeof(name), "%s %d", bench_names[m], n);
#if N265_NEON_INTRA
            bench(name, pred_ref_call, pred_ref_call, pred_opt_call, &pc,
                  200000);
#else
            bench(name, pred_ref_call, pred_opt_call, NULL, &pc, 200000);
#endif
        }
    }
    for (int log2n = 2; log2n <= 5; log2n++) {
        char name[40];

        fill_refs(refs, 1 << log2n, 3);      /* the flat arm at 32 */
        fc.out = out;
        fc.in = refs;
        fc.log2n = log2n;
        snprintf(name, sizeof(name), "filter_refs %d", 1 << log2n);
#if N265_NEON_INTRA
        bench(name, filt_ref_call, filt_ref_call, filt_opt_call, &fc,
              400000);
#else
        bench(name, filt_ref_call, filt_opt_call, NULL, &fc, 400000);
#endif
    }
    {
        int pic_w = 352, pic_h = 288, stride = 373;
        pixel *recon = malloc((size_t)stride * pic_h * sizeof(pixel));
        struct rctx rc;

        if (!recon)
            return;
        for (size_t i = 0; i < (size_t)stride * pic_h; i++)
            recon[i] = ca_pix();
        rc.refs = refs;
        rc.recon = recon;
        rc.stride = stride;
        rc.pic_w = pic_w;
        rc.pic_h = pic_h;
        rc.ctb_log2 = 6;
        for (int log2n = 2; log2n <= 5; log2n++) {
            char name[40];

            /* an interior position: everything available, which is the
             * common case and the one the C pays the most zorder calls
             * for */
            rc.log2n = log2n;
            rc.x0 = 160;
            rc.y0 = 128;
            snprintf(name, sizeof(name), "intra_refs %d", 1 << log2n);
            /* no NEON arm: the ref column is the spec walked literally
             * and the c column is the prefix search the encoder runs, on
             * every target. */
            bench(name, refs_spec_call, refs_opt_call, NULL, &rc, 200000);
        }
        free(recon);
    }
}

int check_intra(void)
{
    int bad;

    window_pred();
    window_filter();
    window_refs();
    bad = check_pred();
    if (bad)
        return bad;
    bad = check_filter();
    if (bad)
        return bad;
    bad = check_refs();
    if (bad)
        return bad;
    bench_intra();
    return 0;
}
