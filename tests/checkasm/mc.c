/*
 * checkasm/mc.c - the motion-compensation family (class N265_ASM_MC).
 * See checkasm.h for the harness and the convention.
 * Copyright (c) 2026, the yah265 authors. GPL-2.0-or-later; see LICENSE.
 */

#include "checkasm.h"

#include "common/cpu.h"
#include "encoder/mc.h"
#include "encoder/arm/neon.h"   /* self-guarded; the family kernels land here */

/* The page guard is checkasm.h's, and this family is why it exists: a
 * four-sample tail written with an eight-lane load and a four-lane store
 * reads four samples it never uses, passes the scramble check because the
 * output cannot move, and passes the sanitiser because the bytes are still
 * inside the allocation. On a padded reference plane it is harmless. On
 * n265_ref_win's gathered `tmp` - which the clipped-MV path takes, and
 * which holds only the (w + 7) * (h + 7) bytes it actually gathered - it
 * reads stack the gather never wrote, so the filter output is whatever was
 * left there; at 64x64 the gather fills the caller's array exactly and the
 * read is past the array as well.
 */

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

/* bi_avg's declared window is exactly w*h int16 of each input, so the
 * page-guard pass gives each of them a mapping that size with an unmapped
 * page hard against it. This is the net the scramble check cannot be: a
 * load that reads four samples too far and throws them away leaves the
 * output alone. (It found nothing here - the scaffold's four-lane tail is
 * a real four-lane load - but it is the check that has to exist before
 * the next kernel in this file claims the same.) */
static void check_bi_avg_window(int w, int h)
{
    size_t nb = (size_t)w * h * sizeof(int16_t);
    int ds = w + BI_PAD;
    uint8_t *dst = malloc((size_t)ds * h);

    for (int tail = 0; tail < 2; tail++) {
        ca_pg ga, gb;
        int16_t *a = ca_pg_alloc(&ga, nb, tail);
        int16_t *b = ca_pg_alloc(&gb, nb, tail);
        char what[96];

        bi_fill(a, b, w * h, 0);
        snprintf(what, sizeof(what), "bi_avg %dx%d, %s edge", w, h,
                 tail ? "far" : "near");
        ca_pg_arm(what);
        bi_opt(dst, ds, a, b, w, h);
        ca_pg_disarm();
        ca_pg_free(&ga);
        ca_pg_free(&gb);
    }
    free(dst);
}

static int check_bi_avg(void)
{
    int bad = 0;

    for (int iw = 0; iw < NBI_DIM; iw++)
        for (int ih = 0; ih < NBI_DIM; ih++)
            check_bi_avg_window(bi_dims[iw], bi_dims[ih]);

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

/* ---- the shape set -------------------------------------------------- */

/* Every luma inter PU shape. The first block is what a CIF and a 720p
 * encode were INSTRUMENTED to emit, at --preset medium and at --preset
 * fast (mc.c's entry points printed the distinct (w, h, fx, fy, hf)
 * tuples they really saw); the second is the CB64 partitions those two
 * pictures never chose but a larger one will, kept because the kernels
 * are generic in w and h and a shape table is only ever wrong in the
 * direction of omission. 4x4 is absent on purpose: HEVC forbids a 4x4
 * inter PU, and the census confirms none reaches the filter. */
static const int luma_shapes[][2] = {
    { 4, 8 }, { 4, 16 }, { 8, 4 }, { 8, 8 }, { 8, 16 }, { 8, 32 },
    { 12, 16 }, { 16, 4 }, { 16, 8 }, { 16, 12 }, { 16, 16 }, { 16, 32 },
    { 24, 32 }, { 32, 8 }, { 32, 16 }, { 32, 24 }, { 32, 32 }, { 64, 64 },
    { 16, 64 }, { 32, 64 }, { 48, 64 }, { 64, 16 }, { 64, 32 }, { 64, 48 },
};
#define NLUMA_SHAPE ((int)(sizeof(luma_shapes) / sizeof(luma_shapes[0])))

/* ---- luma_hf / luma_14_hf ------------------------------------------- */

/* The cached first stage is an int16 plane, so the guarded buffer holds
 * the WHOLE declared read window and nothing else: h + 7 rows (3 above
 * the block, 4 below) of `hs` int16, with the block origin at row 3. A
 * read above or below that window lands in the guard border; a read past
 * column w - 1 lands in the row padding, which hf_scramble() refills with
 * fresh garbage between two otherwise identical runs. Between them the
 * two nets cover every direction a load can run off in. */
#define HF_HALO_UP   3
#define HF_HALO_DOWN 4

static size_t hf_bytes(int hs, int h)
{
    return (size_t)hs * (h + HF_HALO_UP + HF_HALO_DOWN) * sizeof(int16_t);
}

/* Table 8-11 again, a third copy - the encoder's, the kernel's and this
 * one. Here it is not arithmetic but a TARGETING device: kinds 3 and 4
 * lay the extreme sample down where the tap's sign will send the vertical
 * sum furthest, which is the only way to hit the 14-bit form's overflow
 * on purpose instead of by luck. */
static const int8_t hf_taps[4][8] = {
    {  0, 0,   0, 64,  0,   0, 0,  0 },
    { -1, 4, -10, 58, 17,  -5, 1,  0 },
    { -1, 4, -11, 40, 40, -11, 4, -1 },
    {  0, 1,  -5, 17, 58, -10, 4, -1 },
};

/* fill rows -3 .. h+3, columns 0 .. w-1: the declared window */
static void hf_fill(int16_t *hf, int hs, int w, int h, int kind, int fy,
                    int rep)
{
    int nrow = h + HF_HALO_UP + HF_HALO_DOWN;
    for (int r = 0; r < nrow; r++) {
        int16_t *p = hf + (ptrdiff_t)(r - HF_HALO_UP) * hs;
        /* Buffer row r is the (r % 8)-th tap of output row r - r % 8, so
         * signing the row by that tap makes at least one whole column
         * reach the extreme of the vertical sum. rep parity takes both
         * ends. kinds 3 and 4 differ only in what "extreme" means:
         *   3  the ends of int16, which the ENCODER cannot present but
         *      the kernel must still agree on: s = 3669928 and
         *      (int16_t)(s >> 6) wraps.
         *   4  the ends the horizontal stage really produces, [-6120,
         *      22440]: s = 2121600 and s >> 6 = 33150, so the wrap is
         *      not a synthetic case - it is reachable on real pictures.
         */
        int neg = (hf_taps[fy][r % 8] < 0) ^ (rep & 1);
        int16_t rowv = kind == 3 ? (int16_t)(neg ? -32768 : 32767)
                                 : (int16_t)(neg ? -6120 : 22440);
        for (int x = 0; x < w; x++) {
            switch (kind) {
            case 0:     /* uniform int16 */
                p[x] = (int16_t)(rnd() >> 16);
                break;
            case 1:     /* the ends of the range, per sample */
                p[x] = (int16_t)((rnd() & 1) ? 32767 : -32768);
                break;
            case 2:     /* what the horizontal stage really produces:
                         * 8-tap sums of u8 against Table 8-11, whose
                         * widest column (filt[2]) gives [-6120, 22440] */
                p[x] = (int16_t)((int)(rnd() % 28561u) - 6120);
                break;
            default:
                p[x] = rowv;
                break;
            }
        }
    }
}

/* refill everything in the buffer that is NOT the declared window: the
 * guard border on both sides and the padding right of every row */
static void hf_scramble(int16_t *hf, int hs, int w, int h)
{
    int nrow = h + HF_HALO_UP + HF_HALO_DOWN;
    ca_guard_scramble(hf - (ptrdiff_t)HF_HALO_UP * hs, hf_bytes(hs, h));
    for (int r = 0; r < nrow; r++) {
        int16_t *p = hf + (ptrdiff_t)(r - HF_HALO_UP) * hs;
        for (int x = w; x < hs; x++)
            p[x] = (int16_t)(rnd() >> 16);
    }
}

static void hf_opt(uint8_t *dst, int ds, int w, int h, const int16_t *hf,
                   int hs, int fy)
{
#if N265_NEON
    n265_mc_luma_hf_neon(dst, ds, w, h, hf, hs, fy);
#else
    n265_mc_luma_hf_ref(dst, ds, w, h, hf, hs, fy);
#endif
}

static void hf14_opt(int16_t *dst, int ds, int w, int h, const int16_t *hf,
                     int hs, int fy)
{
#if N265_NEON
    n265_mc_luma_14_hf_neon(dst, ds, w, h, hf, hs, fy);
#else
    n265_mc_luma_14_hf_ref(dst, ds, w, h, hf, hs, fy);
#endif
}

/* the destination stride is wider than the block and not by a multiple of
 * four, so a store one vector too far lands in padding ca_check_pad sees
 * rather than in the next row where it would hide */
#define MC_PAD 5

static int mc_cmp8(const uint8_t *ref, const uint8_t *got, int ds, int w,
                   int h, const char *what, int kind)
{
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++)
            if (ref[(ptrdiff_t)y * ds + x] != got[(ptrdiff_t)y * ds + x]) {
                printf("    %s MISMATCH kind %d at (%d,%d): ref %d neon %d\n",
                       what, kind, x, y, ref[(ptrdiff_t)y * ds + x],
                       got[(ptrdiff_t)y * ds + x]);
                return 1;
            }
    return 0;
}

static int mc_cmp16(const int16_t *ref, const int16_t *got, int ds, int w,
                    int h, const char *what, int kind)
{
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++)
            if (ref[(ptrdiff_t)y * ds + x] != got[(ptrdiff_t)y * ds + x]) {
                printf("    %s MISMATCH kind %d at (%d,%d): ref %d neon %d\n",
                       what, kind, x, y, ref[(ptrdiff_t)y * ds + x],
                       got[(ptrdiff_t)y * ds + x]);
                return 1;
            }
    return 0;
}

static int check_luma_hf_one(int w, int h, int hs_extra, int bits14)
{
    int hs = w + hs_extra, ds = w + MC_PAD;
    size_t hb = hf_bytes(hs, h);
    size_t esz = bits14 ? sizeof(int16_t) : sizeof(uint8_t);
    size_t db = (size_t)ds * h * esz;
    int16_t *base = ca_guard_alloc(hb);
    int16_t *hf = base + (ptrdiff_t)HF_HALO_UP * hs;
    uint8_t *dst = ca_guard_alloc(db);
    uint8_t *ref = ca_guard_alloc(db);
    uint8_t *alt = ca_guard_alloc(db);
    char what[64];
    int bad = 0;

    snprintf(what, sizeof(what), "luma%s_hf %dx%d hs%+d",
             bits14 ? "_14" : "", w, h, hs_extra);

    for (int fy = 0; fy < 4 && !bad; fy++) {
        for (int kind = 0; kind < 5 && !bad; kind++) {
            for (int rep = 0; rep < 12 && !bad; rep++) {
                hf_fill(hf, hs, w, h, kind, fy, rep);
                hf_scramble(hf, hs, w, h);
                ca_guard_poison(base, hb);
                memset(ref, CA_POISON, db);
                memset(dst, CA_POISON, db);
                ca_guard_poison(dst, db);

                if (bits14) {
                    n265_mc_luma_14_hf_ref((int16_t *)ref, ds, w, h, hf, hs,
                                           fy);
                    hf14_opt((int16_t *)dst, ds, w, h, hf, hs, fy);
                    bad += mc_cmp16((int16_t *)ref, (int16_t *)dst, ds, w, h,
                                    what, kind);
                } else {
                    n265_mc_luma_hf_ref(ref, ds, w, h, hf, hs, fy);
                    hf_opt(dst, ds, w, h, hf, hs, fy);
                    bad += mc_cmp8(ref, dst, ds, w, h, what, kind);
                }
                if (bad)
                    break;
                bad += ca_guard_check(dst, db, what);
                bad += ca_check_pad(dst, (int)(ds * esz), (int)(w * esz), h,
                                    what);
                bad += ca_check_pad(ref, (int)(ds * esz), (int)(w * esz), h,
                                    "luma_hf ref");
                bad += ca_guard_check(base, hb, what);
                if (bad)
                    break;

                /* Overread: the declared window is rows -3 .. h+3,
                 * columns 0 .. w-1. Run twice with different garbage
                 * everywhere else; an output that moves read something it
                 * was not given. This is the check that would have caught
                 * an eight-lane load at the end of a 4- or 12-wide row. */
                memset(dst, CA_POISON, db);
                if (bits14)
                    hf14_opt((int16_t *)dst, ds, w, h, hf, hs, fy);
                else
                    hf_opt(dst, ds, w, h, hf, hs, fy);
                hf_scramble(hf, hs, w, h);
                memset(alt, CA_POISON, db);
                if (bits14)
                    hf14_opt((int16_t *)alt, ds, w, h, hf, hs, fy);
                else
                    hf_opt(alt, ds, w, h, hf, hs, fy);
                if (memcmp(dst, alt, db)) {
                    printf("    %s: output depends on samples OUTSIDE the "
                           "declared window (fy %d kind %d)\n", what, fy,
                           kind);
                    bad++;
                }
            }
        }
    }
    ca_guard_free(base);
    ca_guard_free(dst);
    ca_guard_free(ref);
    ca_guard_free(alt);
    return bad;
}

/* The page-guard pass. hs == w and the mapping is sized to the window the
 * C entry point DECLARES for this fy - rows 0..h-1 when fy is zero (the
 * arm that only copies the cached row) and rows -3..h+3 otherwise, which
 * is what n265_mc_luma_hf's two N265_RC_TOUCH16 lines report. Sizing it
 * to the larger window for both would leave four spare rows below the
 * fy == 0 block and the whole point would be lost. */
static void check_luma_hf_window(int w, int h, int bits14)
{
    int ds = w + MC_PAD;
    uint8_t *dst = malloc((size_t)ds * h * (bits14 ? 2 : 1));

    for (int fy = 0; fy < 4; fy++) {
        int up = fy ? HF_HALO_UP : 0, down = fy ? HF_HALO_DOWN : 0;
        size_t hb = (size_t)w * (h + up + down) * sizeof(int16_t);

        for (int tail = 0; tail < 2; tail++) {
            ca_pg g;
            int16_t *buf = ca_pg_alloc(&g, hb, tail);
            int16_t *hf = buf + (ptrdiff_t)up * w;
            char what[96];

            for (size_t i = 0; i < hb / sizeof(int16_t); i++)
                buf[i] = (int16_t)(rnd() >> 16);
            snprintf(what, sizeof(what), "luma%s_hf %dx%d fy%d, %s edge",
                     bits14 ? "_14" : "", w, h, fy, tail ? "far" : "near");
            ca_pg_arm(what);
            if (bits14)
                hf14_opt((int16_t *)dst, ds, w, h, hf, w, fy);
            else
                hf_opt(dst, ds, w, h, hf, w, fy);
            ca_pg_disarm();
            ca_pg_free(&g);
        }
    }
    free(dst);
}

static int check_luma_hf(void)
{
    int bad = 0;

    for (int i = 0; i < NLUMA_SHAPE; i++) {
        check_luma_hf_window(luma_shapes[i][0], luma_shapes[i][1], 0);
        check_luma_hf_window(luma_shapes[i][0], luma_shapes[i][1], 1);
    }
    for (int i = 0; i < NLUMA_SHAPE && !bad; i++) {
        int w = luma_shapes[i][0], h = luma_shapes[i][1];
        /* two strides per shape: hs == w makes the buffer exactly the
         * window, so a read past the last column of the LAST row hits the
         * guard; hs == w + 7 is unaligned and gives every row its own
         * scrambled padding, so a read past the last column of ANY row
         * moves the output. */
        for (int e = 0; e <= 7 && !bad; e += 7)
            for (int b = 0; b < 2 && !bad; b++)
                bad += check_luma_hf_one(w, h, e, b);
    }
    return bad;
}

struct hfctx {
    uint8_t *dst;
    int16_t *dst16;
    const int16_t *hf;
    int w, h, ds, hs, fy;
};

static void hf_ref_call(void *p)
{
    struct hfctx *c = p;
    n265_mc_luma_hf_ref(c->dst, c->ds, c->w, c->h, c->hf, c->hs, c->fy);
}

static void hf_opt_call(void *p)
{
    struct hfctx *c = p;
    hf_opt(c->dst, c->ds, c->w, c->h, c->hf, c->hs, c->fy);
}

static void hf14_ref_call(void *p)
{
    struct hfctx *c = p;
    n265_mc_luma_14_hf_ref(c->dst16, c->ds, c->w, c->h, c->hf, c->hs, c->fy);
}

static void hf14_opt_call(void *p)
{
    struct hfctx *c = p;
    hf14_opt(c->dst16, c->ds, c->w, c->h, c->hf, c->hs, c->fy);
}

/* the shapes worth a line: the widths whose tails differ (4, 12), the
 * common squares, and the two the profile says dominate */
static const int mc_bench_shapes[][2] = {
    { 4, 8 }, { 8, 8 }, { 12, 16 }, { 16, 16 }, { 24, 32 }, { 32, 32 },
    { 64, 64 },
};
#define NMC_BENCH ((int)(sizeof(mc_bench_shapes) / sizeof(mc_bench_shapes[0])))

static void bench_luma_hf(void)
{
    enum { HS = 128 };
    static int16_t plane[HS * (64 + 8)];
    static uint8_t dst[64 * 70];
    static int16_t dst16[64 * 70];
    struct hfctx c;

    for (size_t i = 0; i < sizeof(plane) / sizeof(plane[0]); i++)
        plane[i] = (int16_t)((int)(rnd() % 28561u) - 6120);
    c.hf = plane + HS * HF_HALO_UP;
    c.hs = HS;
    c.dst = dst;
    c.dst16 = dst16;
    for (int i = 0; i < NMC_BENCH; i++) {
        char name[40];
        c.w = mc_bench_shapes[i][0];
        c.h = mc_bench_shapes[i][1];
        c.ds = c.w + MC_PAD;
        for (int fy = 0; fy < 4; fy += 2) {   /* the copy arm and a filter arm */
            c.fy = fy;
            snprintf(name, sizeof(name), "luma_hf %dx%d fy%d", c.w, c.h, fy);
#if N265_NEON
            bench(name, hf_ref_call, hf_ref_call, hf_opt_call, &c, 20000);
#else
            bench(name, hf_ref_call, hf_opt_call, NULL, &c, 20000);
#endif
            snprintf(name, sizeof(name), "luma_14_hf %dx%d fy%d", c.w, c.h,
                     fy);
#if N265_NEON
            bench(name, hf14_ref_call, hf14_ref_call, hf14_opt_call, &c,
                  20000);
#else
            bench(name, hf14_ref_call, hf14_opt_call, NULL, &c, 20000);
#endif
        }
    }
}

/* ---- luma_wh / luma_14_wh / luma_hplane ------------------------------
 *
 * One driver for the three, because they differ only in what a row of the
 * same two stages is narrowed to. The interesting part is the READ
 * WINDOW, which is not one shape but four:
 *
 *   fx = fy = 0   the block, and nothing else
 *   fy = 0        columns -3 .. w+3 of the block's own rows
 *   fx = 0        rows -3 .. h+3 of the block's own columns
 *   both          both halos, because the horizontal stage runs over
 *                 h + 7 rows before the vertical stage sees them
 *
 * The reference plane is sized to exactly that window per case, so an
 * eight-lane load at the end of a four-wide row, or a horizontal pass
 * that runs one row too far, is a fault and not a silent pass.
 */
enum { MC_K_WH, MC_K_14WH, MC_K_HPLANE, MC_K_CWH, MC_K_C14WH };

static const char *mc_kind_name[5] = { "luma_wh", "luma_14_wh",
                                       "luma_hplane", "chroma_wh",
                                       "chroma_14_wh" };

/* the chroma 4-tap reaches 1 left/above and 2 right/below, the luma 8-tap
 * 3 and 4 - and only on the axis its phase is nonzero on */
static void mc_halo(int kind, int fx, int fy, int *hxl, int *hxr, int *hyl,
                    int *hyr)
{
    int l = 3, r = 4;

    if (kind == MC_K_CWH || kind == MC_K_C14WH) {
        l = 1;
        r = 2;
    }
    if (kind == MC_K_HPLANE) {          /* horizontal only, always */
        *hxl = l; *hxr = r; *hyl = 0; *hyr = 0;
        return;
    }
    *hxl = fx ? l : 0;
    *hxr = fx ? r : 0;
    *hyl = fy ? l : 0;
    *hyr = fy ? r : 0;
}

static int mc_is_chroma(int kind)
{
    return kind == MC_K_CWH || kind == MC_K_C14WH;
}

static void mc_call_ref(int kind, void *dst, int ds, int w, int h,
                        const uint8_t *ref, int rs, int fx, int fy)
{
    if (kind == MC_K_WH)
        n265_mc_luma_wh_ref(dst, ds, w, h, ref, rs, fx, fy);
    else if (kind == MC_K_14WH)
        n265_mc_luma_14_wh_ref(dst, ds, w, h, ref, rs, fx, fy);
    else if (kind == MC_K_CWH)
        n265_mc_chroma_wh_ref(dst, ds, w, h, ref, rs, fx, fy);
    else if (kind == MC_K_C14WH)
        n265_mc_chroma_14_wh_ref(dst, ds, w, h, ref, rs, fx, fy);
    else
        n265_mc_luma_hplane_ref(dst, ds, w, h, ref, rs, fx);
}

static void mc_call_opt(int kind, void *dst, int ds, int w, int h,
                        const uint8_t *ref, int rs, int fx, int fy)
{
#if N265_NEON
    if (kind == MC_K_WH)
        n265_mc_luma_wh_neon(dst, ds, w, h, ref, rs, fx, fy);
    else if (kind == MC_K_14WH)
        n265_mc_luma_14_wh_neon(dst, ds, w, h, ref, rs, fx, fy);
    else if (kind == MC_K_CWH)
        n265_mc_chroma_wh_neon(dst, ds, w, h, ref, rs, fx, fy);
    else if (kind == MC_K_C14WH)
        n265_mc_chroma_14_wh_neon(dst, ds, w, h, ref, rs, fx, fy);
    else
        n265_mc_luma_hplane_neon(dst, ds, w, h, ref, rs, fx);
#else
    mc_call_ref(kind, dst, ds, w, h, ref, rs, fx, fy);
#endif
}

/* Fill the declared window. kind 3 is the targeting one: a sample is 255
 * where the horizontal AND vertical taps that will meet it agree in sign
 * and 0 where they do not, which drives the 2-D sum to its extreme -
 * 88 * 22440 + 24 * 6120 = 2121600 - and makes the 14-bit form's
 * (int16_t)(s >> 6) = 33150 actually wrap. Column c meets tap
 * fh[c mod 8] at the outputs with x = 3 (mod 8), row r meets fv[r mod 8]
 * likewise, so every block wide or tall enough has one. */
static const int8_t cf_taps[8][4] = {
    {  0, 64,  0,  0 }, { -2, 58, 10, -2 }, { -4, 54, 16, -2 },
    { -6, 46, 28, -4 }, { -4, 36, 36, -4 }, { -4, 28, 46, -6 },
    { -2, 16, 54, -4 }, { -2, 10, 58, -2 },
};

static void mc_plane_fill(uint8_t *org, int rs, int w, int h, int hxl,
                          int hxr, int hyl, int hyr, int fx, int fy, int k,
                          int chroma)
{
    const int8_t *fh = chroma ? cf_taps[fx] : hf_taps[fx];
    const int8_t *fv = chroma ? cf_taps[fy] : hf_taps[fy];
    int nt = chroma ? 4 : 8;

    for (int r = -hyl; r < h + hyr; r++) {
        uint8_t *p = org + (ptrdiff_t)r * rs;
        for (int c = -hxl; c < w + hxr; c++) {
            int sh = fh[((c % nt) + nt) % nt] < 0 ? -1 : 1;
            int sv = fv[((r % nt) + nt) % nt] < 0 ? -1 : 1;
            switch (k) {
            case 0: p[c] = (uint8_t)(rnd() >> 24); break;
            case 1: p[c] = (uint8_t)((rnd() & 1) ? 255 : 0); break;
            case 2: p[c] = (uint8_t)(rnd() & 1 ? 254 : 1); break;
            default: p[c] = (uint8_t)(sh * sv > 0 ? 255 : 0); break;
            }
        }
    }
}

/* Refill the guard border and every byte of the buffer outside the
 * declared window. In buffer coordinates the window is columns
 * [0, hxl + w + hxr) of every row, so what is left over is the row
 * padding the extra stride adds - which is where a load that ran one
 * vector past the last column of a row lands. */
static void mc_plane_scramble(uint8_t *base, size_t bytes, int rs, int w,
                              int h, int hxl, int hxr, int hyl, int hyr)
{
    int nrow = h + hyl + hyr, wcols = hxl + w + hxr;

    ca_guard_scramble(base, bytes);
    for (int r = 0; r < nrow; r++)
        for (int c = wcols; c < rs; c++)
            base[(ptrdiff_t)r * rs + c] = (uint8_t)(rnd() >> 24);
}

static int check_luma_plane_one(int kind, int w, int h, int fx, int fy,
                                int rs_extra)
{
    int hxl, hxr, hyl, hyr;
    mc_halo(kind, fx, fy, &hxl, &hxr, &hyl, &hyr);
    int rs = w + hxl + hxr + rs_extra;
    int nrow = h + hyl + hyr;
    size_t pb = (size_t)rs * nrow;
    size_t esz = (kind == MC_K_WH || kind == MC_K_CWH)
                     ? 1 : sizeof(int16_t);
    int ds = w + MC_PAD;
    size_t db = (size_t)ds * h * esz;
    uint8_t *base = ca_guard_alloc(pb);
    uint8_t *org = base + (ptrdiff_t)hyl * rs + hxl;
    uint8_t *dst = ca_guard_alloc(db);
    uint8_t *ref = ca_guard_alloc(db);
    uint8_t *alt = ca_guard_alloc(db);
    char what[80];
    int bad = 0;

    snprintf(what, sizeof(what), "%s %dx%d fx%d fy%d rs%+d",
             mc_kind_name[kind], w, h, fx, fy, rs_extra);

    /* More passes on the small blocks. A wrong rounding term shows up
     * only where the sum sits on a multiple of 64, so at w = 2 six passes
     * is a handful of scalar samples and a +1 in the rounding survives
     * it; at 24 it does not, and the block is small enough that the extra
     * passes cost nothing. */
    int reps = (long)w * h <= 256 ? 24 : 6;

    for (int k = 0; k < 4 && !bad; k++) {
        for (int rep = 0; rep < reps && !bad; rep++) {
            mc_plane_fill(org, rs, w, h, hxl, hxr, hyl, hyr, fx, fy, k,
                          mc_is_chroma(kind));
            mc_plane_scramble(base, pb, rs, w, h, hxl, hxr, hyl, hyr);
            ca_guard_poison(base, pb);
            memset(ref, CA_POISON, db);
            memset(dst, CA_POISON, db);
            ca_guard_poison(dst, db);

            mc_call_ref(kind, ref, ds, w, h, org, rs, fx, fy);
            mc_call_opt(kind, dst, ds, w, h, org, rs, fx, fy);
            if (esz == 1)
                bad += mc_cmp8(ref, dst, ds, w, h, what, k);
            else
                bad += mc_cmp16((int16_t *)ref, (int16_t *)dst, ds, w, h,
                                what, k);
            if (bad)
                break;
            bad += ca_guard_check(dst, db, what);
            bad += ca_check_pad(dst, (int)(ds * esz), (int)(w * esz), h, what);
            bad += ca_check_pad(ref, (int)(ds * esz), (int)(w * esz), h,
                                "ref");
            bad += ca_guard_check(base, pb, what);
            if (bad)
                break;

            memset(dst, CA_POISON, db);
            mc_call_opt(kind, dst, ds, w, h, org, rs, fx, fy);
            mc_plane_scramble(base, pb, rs, w, h, hxl, hxr, hyl, hyr);
            memset(alt, CA_POISON, db);
            mc_call_opt(kind, alt, ds, w, h, org, rs, fx, fy);
            if (memcmp(dst, alt, db)) {
                printf("    %s: output depends on samples OUTSIDE the "
                       "declared window (kind %d)\n", what, k);
                bad++;
            }
        }
    }
    ca_guard_free(base);
    ca_guard_free(dst);
    ca_guard_free(ref);
    ca_guard_free(alt);
    return bad;
}

/* the page-guard pass: the mapping IS the window for this (fx, fy) */
static void check_luma_plane_window(int kind, int w, int h, int fx, int fy)
{
    int hxl, hxr, hyl, hyr;
    mc_halo(kind, fx, fy, &hxl, &hxr, &hyl, &hyr);
    int rs = w + hxl + hxr;
    size_t pb = (size_t)rs * (h + hyl + hyr);
    int ds = w + MC_PAD;
    void *dst = malloc((size_t)ds * h *
                       ((kind == MC_K_WH || kind == MC_K_CWH) ? 1 : 2));

    for (int tail = 0; tail < 2; tail++) {
        ca_pg g;
        uint8_t *base = ca_pg_alloc(&g, pb, tail);
        uint8_t *org = base + (ptrdiff_t)hyl * rs + hxl;
        char what[96];

        for (size_t i = 0; i < pb; i++)
            base[i] = (uint8_t)(rnd() >> 24);
        snprintf(what, sizeof(what), "%s %dx%d fx%d fy%d, %s edge",
                 mc_kind_name[kind], w, h, fx, fy, tail ? "far" : "near");
        ca_pg_arm(what);
        mc_call_opt(kind, dst, ds, w, h, org, rs, fx, fy);
        ca_pg_disarm();
        ca_pg_free(&g);
    }
    free(dst);
}

/* hplane runs over a whole padded plane strip, not a PU: the widths are
 * pad_w + 2 * N265_HF_PAD, which the instrumented encode showed as 472 on
 * CIF and 1400 on 720p, and the heights are CTB rows. The short widths
 * are here because nothing in the caller promises the width is a multiple
 * of anything and the tails have to be exercised. */
static const int hplane_w[] = { 4, 5, 7, 8, 11, 12, 16, 20, 32, 64, 472,
                                1400 };
static const int hplane_h[] = { 1, 2, 8, 32 };

/* Every kernel here ends its row with a scalar remainder, and on the
 * shapes the encoder presents nothing reaches it: luma widths are all
 * multiples of four and the chroma ones are all multiples of two, which
 * the two-lane step covers. An untested guard is a guard that is wrong
 * the day something does reach it, so the odd widths get their own short
 * list rather than a "cannot happen" comment. (2 and 6 are here as well,
 * though they are real chroma widths and covered below - the list costs
 * a second and overlapping is not a fault.) hplane's widths above carry
 * 5, 7 and 11 for the same reason: nothing in its caller promises a
 * multiple of anything. */
static const int guard_w[] = { 1, 2, 3, 5, 6, 7, 9, 11 };

/* Every chroma block shape. 4:2:0 halves the luma PU in each dimension,
 * so this is the luma table halved - which is where 2, 6 and h = 2 come
 * from, the widths no luma shape has and the ones a four-lane-minimum
 * kernel gets wrong. The first three rows are what the instrumented CIF
 * and 720p encodes really emitted at both presets; the last row is the
 * halves of the CB64 partitions those pictures did not choose. */
static const int chroma_shapes[][2] = {
    { 2, 4 }, { 2, 8 }, { 4, 2 }, { 4, 4 }, { 4, 8 }, { 4, 16 },
    { 6, 8 }, { 8, 2 }, { 8, 4 }, { 8, 6 }, { 8, 8 }, { 8, 16 },
    { 12, 16 }, { 16, 4 }, { 16, 8 }, { 16, 12 }, { 16, 16 }, { 32, 32 },
    { 8, 32 }, { 16, 32 }, { 24, 32 }, { 32, 8 }, { 32, 16 }, { 32, 24 },
};
#define NCHROMA_SHAPE                                                        \
    ((int)(sizeof(chroma_shapes) / sizeof(chroma_shapes[0])))

static int check_chroma(void)
{
    int bad = 0;

    for (int i = 0; i < NCHROMA_SHAPE; i++)
        for (int fx = 0; fx < 8; fx++)
            for (int fy = 0; fy < 8; fy++) {
                check_luma_plane_window(MC_K_CWH, chroma_shapes[i][0],
                                        chroma_shapes[i][1], fx, fy);
                check_luma_plane_window(MC_K_C14WH, chroma_shapes[i][0],
                                        chroma_shapes[i][1], fx, fy);
            }
    for (int i = 0; i < NCHROMA_SHAPE && !bad; i++)
        for (int fx = 0; fx < 8 && !bad; fx++)
            for (int fy = 0; fy < 8 && !bad; fy++)
                for (int e = 0; e <= 7 && !bad; e += 7) {
                    bad += check_luma_plane_one(MC_K_CWH, chroma_shapes[i][0],
                                                chroma_shapes[i][1], fx, fy,
                                                e);
                    bad += check_luma_plane_one(MC_K_C14WH,
                                                chroma_shapes[i][0],
                                                chroma_shapes[i][1], fx, fy,
                                                e);
                }
    /* widths no 4:2:0 halving can produce, for the scalar remainder */
    for (size_t i = 0; i < sizeof(guard_w) / sizeof(guard_w[0]) && !bad; i++)
        for (int fx = 0; fx < 8 && !bad; fx += 3)
            for (int fy = 0; fy < 8 && !bad; fy += 3) {
                check_luma_plane_window(MC_K_CWH, guard_w[i], 2, fx, fy);
                check_luma_plane_window(MC_K_C14WH, guard_w[i], 2, fx, fy);
                bad += check_luma_plane_one(MC_K_CWH, guard_w[i], 2, fx, fy,
                                            7);
                bad += check_luma_plane_one(MC_K_C14WH, guard_w[i], 2, fx, fy,
                                            7);
            }
    return bad;
}

static int check_luma_plane(void)
{
    int bad = 0;

    for (int i = 0; i < NLUMA_SHAPE; i++)
        for (int fx = 0; fx < 4; fx++)
            for (int fy = 0; fy < 4; fy++) {
                check_luma_plane_window(MC_K_WH, luma_shapes[i][0],
                                        luma_shapes[i][1], fx, fy);
                check_luma_plane_window(MC_K_14WH, luma_shapes[i][0],
                                        luma_shapes[i][1], fx, fy);
            }
    for (size_t i = 0; i < sizeof(hplane_w) / sizeof(hplane_w[0]); i++)
        for (size_t j = 0; j < sizeof(hplane_h) / sizeof(hplane_h[0]); j++)
            for (int fx = 0; fx < 4; fx++)
                check_luma_plane_window(MC_K_HPLANE, hplane_w[i],
                                        hplane_h[j], fx, 0);

    for (size_t i = 0; i < sizeof(guard_w) / sizeof(guard_w[0]) && !bad; i++)
        for (int fx = 0; fx < 4 && !bad; fx++)
            for (int fy = 0; fy < 4 && !bad; fy++) {
                check_luma_plane_window(MC_K_WH, guard_w[i], 4, fx, fy);
                check_luma_plane_window(MC_K_14WH, guard_w[i], 4, fx, fy);
                bad += check_luma_plane_one(MC_K_WH, guard_w[i], 4, fx, fy, 7);
                bad += check_luma_plane_one(MC_K_14WH, guard_w[i], 4, fx, fy,
                                            7);
            }

    for (int i = 0; i < NLUMA_SHAPE && !bad; i++)
        for (int fx = 0; fx < 4 && !bad; fx++)
            for (int fy = 0; fy < 4 && !bad; fy++)
                for (int e = 0; e <= 7 && !bad; e += 7) {
                    bad += check_luma_plane_one(MC_K_WH, luma_shapes[i][0],
                                                luma_shapes[i][1], fx, fy, e);
                    bad += check_luma_plane_one(MC_K_14WH, luma_shapes[i][0],
                                                luma_shapes[i][1], fx, fy, e);
                }
    for (size_t i = 0; i < sizeof(hplane_w) / sizeof(hplane_w[0]) && !bad; i++)
        for (size_t j = 0;
             j < sizeof(hplane_h) / sizeof(hplane_h[0]) && !bad; j++)
            for (int fx = 0; fx < 4 && !bad; fx++)
                for (int e = 0; e <= 7 && !bad; e += 7)
                    bad += check_luma_plane_one(MC_K_HPLANE, hplane_w[i],
                                                hplane_h[j], fx, 0, e);
    return bad;
}

struct pctx {
    void *dst;
    const uint8_t *ref;
    int kind, w, h, ds, rs, fx, fy;
};

static void p_ref_call(void *v)
{
    struct pctx *c = v;
    mc_call_ref(c->kind, c->dst, c->ds, c->w, c->h, c->ref, c->rs, c->fx,
                c->fy);
}

static void p_opt_call(void *v)
{
    struct pctx *c = v;
    mc_call_opt(c->kind, c->dst, c->ds, c->w, c->h, c->ref, c->rs, c->fx,
                c->fy);
}

static void bench_luma_plane(void)
{
    enum { RS = 1536 };
    static uint8_t plane[RS * 120];
    static uint8_t dst8[64 * 70];
    static int16_t dst16[1400 * 34];
    struct pctx c;
    /* (fx, fy) picks the arm: 0,0 full-pel; 2,0 horizontal; 0,2 vertical;
     * 2,2 the 2-D pair the profile spends its time in */
    static const int arms[4][2] = { { 0, 0 }, { 2, 0 }, { 0, 2 }, { 2, 2 } };

    for (size_t i = 0; i < sizeof(plane); i++)
        plane[i] = (uint8_t)(rnd() >> 24);
    c.ref = plane + 8 * RS + 8;
    c.rs = RS;
    for (int i = 0; i < NMC_BENCH; i++) {
        char name[48];
        c.w = mc_bench_shapes[i][0];
        c.h = mc_bench_shapes[i][1];
        c.ds = c.w + MC_PAD;
        for (int a = 0; a < 4; a++) {
            c.fx = arms[a][0];
            c.fy = arms[a][1];
            c.kind = MC_K_WH;
            c.dst = dst8;
            snprintf(name, sizeof(name), "luma_wh %dx%d f%d%d", c.w, c.h,
                     c.fx, c.fy);
#if N265_NEON
            bench(name, p_ref_call, p_ref_call, p_opt_call, &c, 20000);
#else
            bench(name, p_ref_call, p_opt_call, NULL, &c, 20000);
#endif
            c.kind = MC_K_14WH;
            c.dst = dst16;
            snprintf(name, sizeof(name), "luma_14_wh %dx%d f%d%d", c.w, c.h,
                     c.fx, c.fy);
#if N265_NEON
            bench(name, p_ref_call, p_ref_call, p_opt_call, &c, 20000);
#else
            bench(name, p_ref_call, p_opt_call, NULL, &c, 20000);
#endif
        }
    }
    c.kind = MC_K_HPLANE;
    c.dst = dst16;
    c.fy = 0;
    c.fx = 2;
    /* the heights the instrumented encode showed are CTB rows - 32 to
     * 124 - not two, and at two the call overhead is most of the line */
    for (int i = 0; i < 2; i++) {
        char name[48];
        c.w = i ? 1400 : 472;
        c.h = 32;
        c.ds = c.w;
        snprintf(name, sizeof(name), "luma_hplane %dx32", c.w);
#if N265_NEON
        bench(name, p_ref_call, p_ref_call, p_opt_call, &c, 20000);
#else
        bench(name, p_ref_call, p_opt_call, NULL, &c, 20000);
#endif
    }
}

static const int chroma_bench_shapes[][2] = {
    { 2, 8 }, { 4, 4 }, { 6, 8 }, { 8, 8 }, { 16, 16 }, { 32, 32 },
};

static void bench_chroma(void)
{
    enum { RS = 96 };
    static uint8_t plane[RS * 48];
    static uint8_t dst8[32 * 40];
    static int16_t dst16[32 * 40];
    struct pctx c;
    static const int arms[3][2] = { { 3, 0 }, { 0, 3 }, { 3, 5 } };
    int n = (int)(sizeof(chroma_bench_shapes) / sizeof(chroma_bench_shapes[0]));

    for (size_t i = 0; i < sizeof(plane); i++)
        plane[i] = (uint8_t)(rnd() >> 24);
    c.ref = plane + 4 * RS + 4;
    c.rs = RS;
    for (int i = 0; i < n; i++) {
        char name[48];
        c.w = chroma_bench_shapes[i][0];
        c.h = chroma_bench_shapes[i][1];
        c.ds = c.w + MC_PAD;
        for (int a = 0; a < 3; a++) {
            c.fx = arms[a][0];
            c.fy = arms[a][1];
            c.kind = MC_K_CWH;
            c.dst = dst8;
            snprintf(name, sizeof(name), "chroma_wh %dx%d f%d%d", c.w, c.h,
                     c.fx, c.fy);
#if N265_NEON
            bench(name, p_ref_call, p_ref_call, p_opt_call, &c, 20000);
#else
            bench(name, p_ref_call, p_opt_call, NULL, &c, 20000);
#endif
            c.kind = MC_K_C14WH;
            c.dst = dst16;
            snprintf(name, sizeof(name), "chroma_14_wh %dx%d f%d%d", c.w, c.h,
                     c.fx, c.fy);
#if N265_NEON
            bench(name, p_ref_call, p_ref_call, p_opt_call, &c, 20000);
#else
            bench(name, p_ref_call, p_opt_call, NULL, &c, 20000);
#endif
        }
    }
}

int check_mc(void)
{
    int bad = check_bi_avg();
    if (!bad)
        bad = check_luma_hf();
    if (!bad)
        bad = check_luma_plane();
    if (!bad)
        bad = check_chroma();
    if (bad)
        return bad;
    bench_bi_avg();
    bench_luma_hf();
    bench_luma_plane();
    bench_chroma();
    return 0;
}
