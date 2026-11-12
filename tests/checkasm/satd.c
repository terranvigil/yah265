/*
 * checkasm/satd.c - the SATD/SAD family (class N265_ASM_SATD).
 * Split out of checkasm.c on 2026-09-10; see checkasm.h for the harness
 * and the convention.
 *
 * The N265_SATD8 arm: n265_satd_wh picks its tiling from a cached env
 * read, so a process can only ever see one of the two. The reference here
 * models whichever arm n265_satd8_mixed() reports, which means the SECOND
 * arm is covered by running the binary again with the variable set -
 * `N265_SATD8=1 build/tests/checkasm satd` is part of this family's item
 * gate for exactly that reason. The arm changes the ranking metric, so
 * "bit-exact" has to hold under it separately.
 *
 * Copyright (c) 2026, the yah265 authors. GPL-2.0-or-later; see LICENSE.
 */

#include "checkasm.h"

#include "common/cpu.h"
#include "encoder/psy.h"
#include "encoder/satd.h"

/* ---- input generators ---------------------------------------------
 *
 * Random is the weakest of these. The ones that matter are the extremes:
 * kind 1 puts every sample at a clip end, and kind 4 builds the exact
 * worst case for the Hadamard, a block whose difference is PIXEL_MAX
 * times one 2D basis function, which puts a single coefficient at
 * PIXEL_MAX * n * n - 16320 for the 8x8 tile and 4080 for the 4x4 at 8
 * bits, 65472 and 16368 at Main10, which is exactly where satd_neon.c's
 * second pass has to leave int16 behind. If a lane were one bit too
 * narrow that is the input that says so, and it is the same input at
 * both depths.
 */
#define NKIND 6

/* natural-order Hadamard sign: H[k][j] = (-1)^popcount(k & j) */
static int had_sign(int k, int j)
{
    return (__builtin_popcount(k & j) & 1) ? -1 : 1;
}

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
            case 1:     /* both ends of the range, every sample */
                va = ca_pix_edge();
                vb = ca_pix_edge();
                break;
            case 2:     /* identical: the near-skip case, sum 0 */
                va = ca_pix();
                vb = va;
                break;
            case 3: {   /* encoder-typical: a small residual on a ramp */
                int base = (x * 7 + y * 11 + (int)(rnd() & 15)) & PIXEL_MAX;
                int d = (int)(rnd() % 9) - 4;
                int v = base + d;
                va = (pixel)base;
                vb = (pixel)(v < 0 ? 0 : v > PIXEL_MAX ? PIXEL_MAX : v);
                break;
            }
            case 4: {   /* +/-PIXEL_MAX on one 2D Hadamard basis function */
                int s = had_sign(ky, y & 7) * had_sign(kx, x & 7);
                va = (pixel)(s > 0 ? PIXEL_MAX : 0);
                vb = (pixel)(PIXEL_MAX - va);
                break;
            }
            default:    /* solid maximum difference */
                va = (pixel)PIXEL_MAX;
                vb = 0;
                break;
            }
            a[(size_t)y * as + x] = va;
            b[(size_t)y * bs + x] = vb;
        }
}

/* ---- SATD ---------------------------------------------------------- */

/* Every w x h n265_satd_wh was instrumented to see on a CIF encode and on
 * a 720p one (both presets, --ctu 16 and 64, N265_SATD8 both ways): the
 * squares 4..64, the rectangular halves, and the AMP quarters whose 4 and
 * 12 sides fall to the 4x4 tiling. The shapes below that the encoder does
 * NOT currently present - 48-sided, 64x16, 12x4 - are here because they
 * are what a CB64 AMP partition would ask for the day it is enabled, and
 * a shape table that only lists today's call sites is how the scaffold's
 * first kernel got its chroma widths wrong.
 *
 * Every satd caller is LUMA (mv.c mc_satd, ctu.c mot_satd_wh and the
 * intra ranking), so unlike bi_avg there is no 4:2:0 halving here and no
 * width of 2 or 6 to cover: grep says n265_satd_wh is never handed
 * pad_plane[1] or [2]. The SAD and SSD kernels below are a different
 * story and their tables say so.
 */
static const int satd_shapes[][2] = {
    { 4, 4 }, { 8, 8 }, { 16, 16 }, { 32, 32 }, { 64, 64 },
    { 8, 4 }, { 4, 8 }, { 16, 8 }, { 8, 16 }, { 32, 16 }, { 16, 32 },
    { 16, 4 }, { 4, 16 }, { 12, 16 }, { 16, 12 }, { 24, 32 }, { 32, 24 },
    { 8, 32 }, { 32, 8 }, { 4, 12 }, { 12, 4 }, { 12, 12 }, { 24, 24 },
    { 48, 48 }, { 48, 64 }, { 64, 48 }, { 16, 64 }, { 64, 16 },
    { 32, 64 }, { 64, 32 }, { 4, 32 }, { 32, 4 }, { 4, 64 }, { 64, 4 },
};
#define NSATD_SH ((int)(sizeof(satd_shapes) / sizeof(satd_shapes[0])))

/* the C entry point's tiling, in the loop-nest tiles, both arms */
static uint32_t satd_ref_wh(const pixel *a, int as, const pixel *b,
                            int bs, int w, int h)
{
    uint32_t sum = 0;

    if ((w & 7) || (h & 7)) {
        int w8 = n265_satd8_mixed() ? (w & ~7) : 0;
        int h8 = n265_satd8_mixed() ? (h & ~7) : 0;
        for (int y = 0; y < h8; y += 8)
            for (int x = 0; x < w8; x += 8)
                sum += n265_satd8_ref(a + (size_t)y * as + x, as,
                                      b + (size_t)y * bs + x, bs);
        for (int y = 0; y < h8; y += 4)
            for (int x = w8; x < w; x += 4)
                sum += n265_satd4_ref(a + (size_t)y * as + x, as,
                                      b + (size_t)y * bs + x, bs);
        for (int y = h8; y < h; y += 4)
            for (int x = 0; x < w; x += 4)
                sum += n265_satd4_ref(a + (size_t)y * as + x, as,
                                      b + (size_t)y * bs + x, bs);
        return sum;
    }
    for (int y = 0; y < h; y += 8)
        for (int x = 0; x < w; x += 8)
            sum += n265_satd8_ref(a + (size_t)y * as + x, as,
                                  b + (size_t)y * bs + x, bs);
    return sum;
}

static uint32_t satd_opt_wh(const pixel *a, int as, const pixel *b,
                            int bs, int w, int h)
{
#if CA_SIMD_SATD
    return CA_SIMD(n265_satd_wh)(a, as, b, bs, w, h);
#else
    return n265_satd_wh(a, as, b, bs, w, h);
#endif
}

/* ---- read-window checks --------------------------------------------
 *
 * A cost kernel writes nothing, so the guard border cannot catch it the
 * way it catches a store: the only way it can be wrong at a block edge is
 * by READING outside the w x h window it was given, and the way to see
 * that is to change what is outside and watch the answer move.
 *
 * Two nets, because the two overreads look different:
 *   - past the allocation (a row too far down, a byte before the top):
 *     ca_guard_scramble on the borders.
 *   - into the row padding of a strided block (a vector load of eight
 *     where four were declared): the padding is interior, so the border
 *     scramble cannot see it and it gets its own pass.
 */
static void scramble_pad(pixel *p, int stride, int w, int h)
{
    for (int y = 0; y < h; y++)
        for (int x = w; x < stride; x++)
            p[(size_t)y * stride + x] = ca_pix();
}

static int check_satd_wh(void)
{
    int bad = 0;

    for (int s = 0; s < NSATD_SH && !bad; s++) {
        int w = satd_shapes[s][0], h = satd_shapes[s][1];
        /* two strides per shape: tight, where the guard border sits
         * immediately past the last sample, and padded by an amount that
         * is not a multiple of the vector width, where an eight-wide load
         * on a four-wide tail lands in padding this can scramble */
        for (int is = 0; is < 2 && !bad; is++) {
            int as = is ? w + 5 : w;
            int bs = is ? w + 13 : w;
            size_t abytes = (size_t)as * h * sizeof(pixel);
            size_t bbytes = (size_t)bs * h * sizeof(pixel);
            pixel *a = ca_guard_alloc(abytes);
            pixel *b = ca_guard_alloc(bbytes);
            char what[48];

            snprintf(what, sizeof(what), "satd %dx%d s%d", w, h, is);
            for (int kind = 0; kind < NKIND && !bad; kind++) {
                for (int rep = 0; rep < 16 && !bad; rep++) {
                    uint32_t r, o, o2;

                    memset(a, CA_POISON, abytes);
                    memset(b, CA_POISON, bbytes);
                    fill_pair(a, as, b, bs, w, h, kind);
                    ca_guard_poison(a, abytes);
                    ca_guard_poison(b, bbytes);

                    r = satd_ref_wh(a, as, b, bs, w, h);
                    o = satd_opt_wh(a, as, b, bs, w, h);
                    if (r != o) {
                        printf("    %s MISMATCH kind %d: ref %u opt %u\n",
                               what, kind, r, o);
                        bad++;
                        break;
                    }
                    /* The unrolled C butterfly against the same
                     * reference. Under N265_ASM_OFF=satd this is the only
                     * comparison that runs a C tile at all, and it is the
                     * one that was here before the NEON landed. */
                    if (r != n265_satd_wh_c(a, as, b, bs, w, h)) {
                        printf("    %s C MISMATCH kind %d\n", what, kind);
                        bad++;
                        break;
                    }
                    /* a cost kernel must not write at all */
                    bad += ca_guard_check(a, abytes, what);
                    bad += ca_guard_check(b, bbytes, what);
                    if (bad)
                        break;

                    ca_guard_scramble(a, abytes);
                    ca_guard_scramble(b, bbytes);
                    scramble_pad(a, as, w, h);
                    scramble_pad(b, bs, w, h);
                    o2 = satd_opt_wh(a, as, b, bs, w, h);
                    if (o2 != o) {
                        printf("    %s: result depends on bytes OUTSIDE "
                               "the block (kind %d): %u then %u\n",
                               what, kind, o, o2);
                        bad++;
                    }
                }
            }
            ca_guard_free(a);
            ca_guard_free(b);
        }
    }
    return bad;
}

/* the two tiles on their own, against their own loop-nest references, so
 * a tile bug is reported as a tile bug and not as a 64x64 sum that is
 * off by something */
static int check_satd_tiles(void)
{
    int bad = 0;
#if CA_SIMD_SATD
    for (int t = 0; t < 2 && !bad; t++) {
        int n = t ? 8 : 4;
        for (int is = 0; is < 2 && !bad; is++) {
            int as = is ? n + 5 : n, bs = is ? n + 13 : n;
            size_t abytes = (size_t)as * n * sizeof(pixel);
            size_t bbytes = (size_t)bs * n * sizeof(pixel);
            pixel *a = ca_guard_alloc(abytes);
            pixel *b = ca_guard_alloc(bbytes);
            char what[48];

            snprintf(what, sizeof(what), "satd%d tile s%d", n, is);
            for (int kind = 0; kind < NKIND && !bad; kind++) {
                for (int rep = 0; rep < 64 && !bad; rep++) {
                    uint32_t r, o, o2;

                    memset(a, CA_POISON, abytes);
                    memset(b, CA_POISON, bbytes);
                    fill_pair(a, as, b, bs, n, n, kind);
                    ca_guard_poison(a, abytes);
                    ca_guard_poison(b, bbytes);
                    r = n == 8 ? n265_satd8_ref(a, as, b, bs)
                               : n265_satd4_ref(a, as, b, bs);
                    o = n == 8 ? CA_SIMD(n265_satd8)(a, as, b, bs)
                               : CA_SIMD(n265_satd4)(a, as, b, bs);
                    if (r != o) {
                        printf("    %s MISMATCH kind %d: ref %u simd %u\n",
                               what, kind, r, o);
                        bad++;
                        break;
                    }
                    bad += ca_guard_check(a, abytes, what);
                    bad += ca_guard_check(b, bbytes, what);
                    if (bad)
                        break;
                    ca_guard_scramble(a, abytes);
                    ca_guard_scramble(b, bbytes);
                    scramble_pad(a, as, n, n);
                    scramble_pad(b, bs, n, n);
                    o2 = n == 8 ? CA_SIMD(n265_satd8)(a, as, b, bs)
                                : CA_SIMD(n265_satd4)(a, as, b, bs);
                    if (o2 != o) {
                        printf("    %s: result depends on bytes OUTSIDE "
                               "the tile (kind %d)\n", what, kind);
                        bad++;
                    }
                }
            }
            ca_guard_free(a);
            ca_guard_free(b);
        }
    }
#endif
    return bad;
}

/* ---- bench ---------------------------------------------------------- */

struct sctx {
    const pixel *a, *b;
    int as, bs, w, h;
    uint32_t sink;
};

static void satd_ref_call(void *p)
{
    struct sctx *c = p;
    c->sink += satd_ref_wh(c->a, c->as, c->b, c->bs, c->w, c->h);
}

static void satd_c_call(void *p)
{
    struct sctx *c = p;
    c->sink += n265_satd_wh_c(c->a, c->as, c->b, c->bs, c->w, c->h);
}

#if CA_SIMD_SATD
static void satd_neon_call(void *p)
{
    struct sctx *c = p;
    c->sink += CA_SIMD(n265_satd_wh)(c->a, c->as, c->b, c->bs, c->w, c->h);
}
#endif

/* the shapes worth a line: the two tile sizes on their own, the four
 * commonest PU shapes by call count on a CIF encode, and the big squares */
static const int satd_bench_shapes[][2] = {
    { 4, 4 }, { 8, 4 }, { 4, 8 }, { 8, 8 }, { 16, 8 }, { 12, 16 },
    { 16, 16 }, { 32, 32 }, { 24, 32 }, { 64, 64 },
};

static void bench_satd(void)
{
    static pixel a[72 * 72], b[72 * 72];
    struct sctx c;
    int n = (int)(sizeof(satd_bench_shapes) / sizeof(satd_bench_shapes[0]));

    fill_pair(a, 72, b, 72, 72, 72, 0);
    c.a = a;
    c.b = b;
    c.as = 72;
    c.bs = 72;
    c.sink = 0;
    for (int i = 0; i < n; i++) {
        char name[32];
        c.w = satd_bench_shapes[i][0];
        c.h = satd_bench_shapes[i][1];
        snprintf(name, sizeof(name), "satd %dx%d", c.w, c.h);
#if CA_SIMD_SATD
        bench(name, satd_ref_call, satd_c_call, satd_neon_call, &c, 40000);
#else
        bench(name, satd_ref_call, satd_c_call, NULL, &c, 40000);
#endif
    }
}


/* ---- SAD ------------------------------------------------------------
 *
 * The shape set, instrumented off the encoder rather than read off the PU
 * table: n265_mc_sad (both arms) presents every luma PU shape the search
 * reaches - 4, 8, 12, 16, 24, 32 and 64 on each side as the partition
 * rules allow - the intra ranking presents the squares 4..32, mot_sad the
 * squares 8..64, and the lookahead a flat 8x8 on the lowres plane. Every
 * one of those is luma, so 4:2:0 puts nothing narrower than 4 here today.
 *
 * The odd widths below (1, 2, 3, 5, 6, 7) are here anyway, because THIS
 * kernel is the one a later item points at a chroma plane, and a scalar
 * tail that has never been run is a scalar tail that does not work. They
 * cost a few milliseconds and they are the difference between the vector
 * loop covering the block and the vector loop covering what it assumed.
 */
static const int sad_shapes[][2] = {
    { 4, 4 }, { 8, 8 }, { 16, 16 }, { 32, 32 }, { 64, 64 },
    { 8, 4 }, { 4, 8 }, { 16, 8 }, { 8, 16 }, { 32, 16 }, { 16, 32 },
    { 16, 4 }, { 4, 16 }, { 12, 16 }, { 16, 12 }, { 24, 32 }, { 32, 24 },
    { 8, 32 }, { 32, 8 }, { 48, 48 }, { 48, 64 }, { 64, 48 }, { 64, 16 },
    { 12, 12 }, { 24, 24 }, { 20, 8 }, { 28, 12 }, { 36, 4 }, { 44, 20 },
    { 1, 1 }, { 2, 2 }, { 3, 5 }, { 5, 3 }, { 6, 8 }, { 7, 9 }, { 2, 16 },
    { 6, 16 }, { 9, 7 }, { 13, 3 }, { 17, 2 }, { 33, 1 }, { 64, 1 },
    /* item 39's u16-accumulator arm: odd heights at 16 (the two-row
     * step's tail) and the tallest block at each of its three widths */
    { 16, 1 }, { 16, 5 }, { 16, 64 }, { 32, 64 }, { 32, 3 }, { 64, 7 },
    /* the AVX-512 narrow arms: four rows a vector, and the rows past
     * a multiple of four */
    { 8, 7 }, { 4, 6 }, { 8, 64 }, { 4, 64 }, { 12, 9 },
};
#define NSAD_SH ((int)(sizeof(sad_shapes) / sizeof(sad_shapes[0])))

/* the SSD callers: tb_ssd on a TB (luma 4..32, chroma 4..16) and
 * cu_area_ssd on a whole CU (luma 8..64, chroma 4..32), both square. The
 * non-square and narrow entries are the same insurance as above. */
static const int ssd_shapes[][2] = {
    { 4, 4 }, { 8, 8 }, { 16, 16 }, { 32, 32 }, { 64, 64 },
    { 2, 2 }, { 6, 6 }, { 12, 12 }, { 24, 24 }, { 48, 48 },
    { 8, 16 }, { 16, 8 }, { 3, 7 }, { 7, 3 }, { 1, 64 }, { 64, 1 },
    { 20, 20 }, { 33, 5 },
    /* the AVX-512 arms: two or four rows a vector, odd and short heights */
    { 9, 7 }, { 16, 5 }, { 13, 3 }, { 8, 6 }, { 4, 5 }, { 8, 64 }, { 4, 12 },
    { 16, 64 }, { 12, 1 },
};
#define NSSD_SH ((int)(sizeof(ssd_shapes) / sizeof(ssd_shapes[0])))

/* SAD and SSD are the same test with a different reduction, so one body:
 * `which` picks it, and the two arms of SAD (base, DotProd) are two
 * values of `arm` on the same body. */
enum { RED_SAD, RED_SSD, RED_SSD_DOT };

static uint64_t red_ref(int which, const pixel *a, int as, const pixel *b,
                        int bs, int w, int h)
{
    if (which == RED_SAD)
        return n265_sad_wh_c(a, as, b, bs, w, h);
    return n265_ssd_wh_c(a, as, b, bs, w, h);
}

static uint64_t red_opt(int which, const pixel *a, int as, const pixel *b,
                        int bs, int w, int h)
{
#if CA_SIMD_SATD
#if BIT_DEPTH == 8 && defined(__ARM_FEATURE_DOTPROD)
    if (which == RED_SSD_DOT)
        return n265_ssd_wh_dot_neon(a, as, b, bs, w, h);
#endif
    if (which == RED_SSD)
        return CA_SIMD(n265_ssd_wh)(a, as, b, bs, w, h);
    return CA_SIMD(n265_sad_wh)(a, as, b, bs, w, h);
#else
    if (which == RED_SAD)
        return n265_sad_wh(a, as, b, bs, w, h);
    return n265_ssd_wh(a, as, b, bs, w, h);
#endif
}

static int check_reduction(const char *tag, int which, const int (*shapes)[2],
                           int nsh)
{
    int bad = 0;

    for (int s = 0; s < nsh && !bad; s++) {
        int w = shapes[s][0], h = shapes[s][1];
        for (int is = 0; is < 2 && !bad; is++) {
            int as = is ? w + 5 : w;
            int bs = is ? w + 13 : w;
            size_t abytes = (size_t)as * h * sizeof(pixel);
            size_t bbytes = (size_t)bs * h * sizeof(pixel);
            pixel *a = ca_guard_alloc(abytes);
            pixel *b = ca_guard_alloc(bbytes);
            char what[56];

            snprintf(what, sizeof(what), "%s %dx%d s%d", tag, w, h, is);
            for (int kind = 0; kind < NKIND && !bad; kind++) {
                for (int rep = 0; rep < 16 && !bad; rep++) {
                    uint64_t r, o, o2;

                    memset(a, CA_POISON, abytes);
                    memset(b, CA_POISON, bbytes);
                    fill_pair(a, as, b, bs, w, h, kind);
                    ca_guard_poison(a, abytes);
                    ca_guard_poison(b, bbytes);

                    r = red_ref(which, a, as, b, bs, w, h);
                    o = red_opt(which, a, as, b, bs, w, h);
                    if (r != o) {
                        printf("    %s MISMATCH kind %d: ref %llu opt %llu\n",
                               what, kind, (unsigned long long)r,
                               (unsigned long long)o);
                        bad++;
                        break;
                    }
                    bad += ca_guard_check(a, abytes, what);
                    bad += ca_guard_check(b, bbytes, what);
                    if (bad)
                        break;

                    ca_guard_scramble(a, abytes);
                    ca_guard_scramble(b, bbytes);
                    scramble_pad(a, as, w, h);
                    scramble_pad(b, bs, w, h);
                    o2 = red_opt(which, a, as, b, bs, w, h);
                    if (o2 != o) {
                        printf("    %s: result depends on bytes OUTSIDE "
                               "the block (kind %d)\n", what, kind);
                        bad++;
                    }
                }
            }
            ca_guard_free(a);
            ca_guard_free(b);
        }
    }
    return bad;
}

/* ---- per-row and per-column SAD ------------------------------------
 *
 * n265_sad_rowcol is the only kernel in this family that WRITES, so the
 * guard buffers go around its two outputs as well as its two inputs: a
 * column accumulator stored one vector too far is the failure mode the
 * cost kernels cannot have, and the arrays are exactly n long with
 * poison hard against the end of each.
 *
 * The shape set is the call site's, instrumented on foreman and park_joy
 * at the shipping default: rect_proxy runs under the quadtree's
 * log2n <= 5 arm on the 2Nx2N winner of an inter CU, so n is 8, 16 or
 * 32 and nothing else. 4 and 64 are here because the kernel's strip
 * ladder has a rung for each, and the odd widths because a vector loop
 * that assumed a multiple of its own width is the classic way to be
 * wrong on the one shape a caller adds later.
 */
static const int rowcol_shapes[] = {
    8, 16, 32, 4, 64, 12, 20, 24, 48, 1, 2, 3, 5, 7, 9, 13, 17, 33,
};
#define NROWCOL_SH ((int)(sizeof(rowcol_shapes) / sizeof(rowcol_shapes[0])))

static void rowcol_ref(const pixel *a, int as, const pixel *b, int bs,
                       int n, uint32_t *row, uint32_t *col)
{
    n265_sad_rowcol_ref(a, as, b, bs, n, row, col);
}

static void rowcol_opt(const pixel *a, int as, const pixel *b, int bs,
                       int n, uint32_t *row, uint32_t *col)
{
#if CA_SIMD_SATD
    CA_SIMD(n265_sad_rowcol)(a, as, b, bs, n, row, col);
#else
    n265_sad_rowcol(a, as, b, bs, n, row, col);
#endif
}

static int rowcol_cmp(const char *what, int kind, const char *tag,
                      const uint32_t *r, const uint32_t *o, int n)
{
    for (int i = 0; i < n; i++)
        if (r[i] != o[i]) {
            printf("    %s MISMATCH kind %d: %s[%d] ref %u opt %u\n",
                   what, kind, tag, i, r[i], o[i]);
            return 1;
        }
    return 0;
}

static int check_rowcol(void)
{
    int bad = 0;

    for (int s = 0; s < NROWCOL_SH && !bad; s++) {
        int n = rowcol_shapes[s];
        for (int is = 0; is < 2 && !bad; is++) {
            int as = is ? n + 5 : n;
            int bs = is ? n + 13 : n;
            size_t abytes = (size_t)as * n * sizeof(pixel);
            size_t bbytes = (size_t)bs * n * sizeof(pixel);
            size_t obytes = (size_t)n * sizeof(uint32_t);
            pixel *a = ca_guard_alloc(abytes);
            pixel *b = ca_guard_alloc(bbytes);
            uint32_t *rr = ca_guard_alloc(obytes);
            uint32_t *rc = ca_guard_alloc(obytes);
            uint32_t *orow = ca_guard_alloc(obytes);
            uint32_t *ocol = ca_guard_alloc(obytes);
            uint32_t *o2row = ca_guard_alloc(obytes);
            uint32_t *o2col = ca_guard_alloc(obytes);
            char what[56];

            snprintf(what, sizeof(what), "sad rowcol %dx%d s%d", n, n, is);
            for (int kind = 0; kind < NKIND && !bad; kind++) {
                for (int rep = 0; rep < 16 && !bad; rep++) {
                    memset(a, CA_POISON, abytes);
                    memset(b, CA_POISON, bbytes);
                    fill_pair(a, as, b, bs, n, n, kind);
                    ca_guard_poison(a, abytes);
                    ca_guard_poison(b, bbytes);
                    ca_guard_poison(orow, obytes);
                    ca_guard_poison(ocol, obytes);

                    rowcol_ref(a, as, b, bs, n, rr, rc);
                    rowcol_opt(a, as, b, bs, n, orow, ocol);
                    bad += rowcol_cmp(what, kind, "row", rr, orow, n);
                    bad += rowcol_cmp(what, kind, "col", rc, ocol, n);
                    if (bad)
                        break;
                    bad += ca_guard_check(a, abytes, what);
                    bad += ca_guard_check(b, bbytes, what);
                    bad += ca_guard_check(orow, obytes, what);
                    bad += ca_guard_check(ocol, obytes, what);
                    if (bad)
                        break;

                    /* the read window: nothing outside the block may
                     * reach either output */
                    ca_guard_scramble(a, abytes);
                    ca_guard_scramble(b, bbytes);
                    scramble_pad(a, as, n, n);
                    scramble_pad(b, bs, n, n);
                    ca_guard_poison(o2row, obytes);
                    ca_guard_poison(o2col, obytes);
                    rowcol_opt(a, as, b, bs, n, o2row, o2col);
                    if (memcmp(o2row, orow, obytes) ||
                        memcmp(o2col, ocol, obytes)) {
                        printf("    %s: result depends on bytes OUTSIDE "
                               "the block (kind %d)\n", what, kind);
                        bad++;
                    }
                }
            }
            ca_guard_free(a);
            ca_guard_free(b);
            ca_guard_free(rr);
            ca_guard_free(rc);
            ca_guard_free(orow);
            ca_guard_free(ocol);
            ca_guard_free(o2row);
            ca_guard_free(o2col);
        }
    }
    return bad;
}

/* ---- the page-guard pass -------------------------------------------
 *
 * A cost kernel writes nothing and returns one number, which is what makes
 * it the worst case for the scramble check above: a load that grabs eight
 * samples where four were declared moves the answer only if the extra
 * lanes reach the reduction. Zero them, mask them, or throw the high half
 * of the vector away - all of which a Hadamard tail does - and the read is
 * invisible to every net in checkasm.h and to the sanitizer both.
 *
 * So every kernel in this family also runs against a mapping sized to
 * ca_pg_window() of each input with an unmapped page hard against it, at
 * the tight stride and the padded one, and in both orientations. The value
 * population is not an axis here: none of these kernels branches on a
 * sample, so one fill per shape covers the paths that exist.
 */
enum { PG_SATD4, PG_SATD8, PG_SATD_WH, PG_ROWCOL, PG_RED };

/* the row/col kernel's outputs; the page guard is about its INPUTS */
static uint32_t pg_row[64], pg_col[64];

static void pg_call(int which, const pixel *a, int as, const pixel *b,
                    int bs, int w, int h)
{
    volatile uint64_t sink = 0;

    switch (which) {
#if CA_SIMD_SATD
    case PG_SATD4: sink = CA_SIMD(n265_satd4)(a, as, b, bs); break;
    case PG_SATD8: sink = CA_SIMD(n265_satd8)(a, as, b, bs); break;
#endif
    case PG_SATD_WH: sink = satd_opt_wh(a, as, b, bs, w, h); break;
    case PG_ROWCOL:
        rowcol_opt(a, as, b, bs, w, pg_row, pg_col);
        sink = pg_row[0] + pg_col[0];
        break;
    default: sink = red_opt(which - PG_RED, a, as, b, bs, w, h); break;
    }
    (void)sink;
}

static void check_window(const char *tag, int which, int w, int h)
{
    for (int is = 0; is < 2; is++) {
        int as = is ? w + 5 : w, bs = is ? w + 13 : w;
        size_t abytes = ca_pg_window_pix(as, w, h);
        size_t bbytes = ca_pg_window_pix(bs, w, h);

        for (int tail = 0; tail < 2; tail++) {
            ca_pg ga, gb;
            pixel *a = ca_pg_alloc(&ga, abytes, tail);
            pixel *b = ca_pg_alloc(&gb, bbytes, tail);
            char what[80];

            ca_pg_fill_pix(&ga, abytes / sizeof(pixel));
            ca_pg_fill_pix(&gb, bbytes / sizeof(pixel));
            snprintf(what, sizeof(what), "%s %dx%d s%d, %s edge", tag, w, h,
                     is, tail ? "far" : "near");
            ca_pg_arm(what);
            pg_call(which, a, as, b, bs, w, h);
            ca_pg_disarm();
            ca_pg_free(&ga);
            ca_pg_free(&gb);
        }
    }
}

static void check_windows(void)
{
#if CA_SIMD_SATD
    check_window("satd4 tile", PG_SATD4, 4, 4);
    check_window("satd8 tile", PG_SATD8, 8, 8);
#endif
    for (int s = 0; s < NSATD_SH; s++)
        check_window("satd", PG_SATD_WH, satd_shapes[s][0],
                     satd_shapes[s][1]);
    for (int s = 0; s < NSAD_SH; s++)
        check_window("sad", PG_RED + RED_SAD, sad_shapes[s][0],
                     sad_shapes[s][1]);
    for (int s = 0; s < NSSD_SH; s++)
        check_window("ssd", PG_RED + RED_SSD, ssd_shapes[s][0],
                     ssd_shapes[s][1]);
    for (int s = 0; s < NROWCOL_SH; s++)
        check_window("sad rowcol", PG_ROWCOL, rowcol_shapes[s],
                     rowcol_shapes[s]);
#if CA_SIMD_SATD && BIT_DEPTH == 8 && defined(__ARM_FEATURE_DOTPROD)
    if (n265_cpu_flags() & N265_CPU_DOTPROD)
        for (int s = 0; s < NSSD_SH; s++)
            check_window("ssd/dot", PG_RED + RED_SSD_DOT, ssd_shapes[s][0],
                         ssd_shapes[s][1]);
#endif
}

/* The maximum a 64x64 SSD can reach - every sample at the opposite clip -
 * is 4096 * 65025 = 266342400 at 8 bits, where the kernel's claim is that
 * the u32 accumulator holds it, and 4096 * 1046529 = 4286222784 at
 * Main10, where it is inside 2^32 by a fifth of a percent and the kernel
 * therefore folds into u64 once a row. That second number is the whole
 * reason the 16-bit form is shaped differently, so check the number
 * itself rather than trusting the arithmetic in a comment. */
static int check_ssd_extreme(void)
{
    static pixel a[64 * 64], b[64 * 64];
    uint64_t want = (uint64_t)64 * 64 * PIXEL_MAX * PIXEL_MAX;
    uint64_t got;

    for (int i = 0; i < 64 * 64; i++) {
        a[i] = (pixel)PIXEL_MAX;
        b[i] = 0;
    }
    got = red_opt(RED_SSD, a, 64, b, 64, 64, 64);
    if (got != want) {
        printf("    ssd 64x64 saturating: want %llu got %llu\n",
               (unsigned long long)want, (unsigned long long)got);
        return 1;
    }
#if CA_SIMD_SATD && BIT_DEPTH == 8 && defined(__ARM_FEATURE_DOTPROD)
    if (n265_cpu_flags() & N265_CPU_DOTPROD) {
        got = red_opt(RED_SSD_DOT, a, 64, b, 64, 64, 64);
        if (got != want) {
            printf("    ssd/dot 64x64 saturating: want %llu got %llu\n",
                   (unsigned long long)want, (unsigned long long)got);
            return 1;
        }
    }
#endif
    if (red_ref(RED_SSD, a, 64, b, 64, 64, 64) != want) {
        printf("    ssd 64x64 saturating: the C reference is wrong too\n");
        return 1;
    }
    return 0;
}

/* ---- bench, SAD and SSD --------------------------------------------- */

struct rctx {
    const pixel *a, *b;
    int as, bs, w, h, which;
    uint64_t sink;
};

static void red_ref_call(void *p)
{
    struct rctx *c = p;
    c->sink += red_ref(c->which, c->a, c->as, c->b, c->bs, c->w, c->h);
}

static void red_opt_call(void *p)
{
    struct rctx *c = p;
    c->sink += red_opt(c->which, c->a, c->as, c->b, c->bs, c->w, c->h);
}

static const int red_bench_shapes[][2] = {
    { 4, 4 }, { 8, 4 }, { 4, 8 }, { 8, 8 }, { 16, 8 }, { 12, 16 },
    { 16, 16 }, { 32, 32 }, { 64, 64 },
};

static void bench_reduction(const char *tag, int which)
{
    static pixel a[72 * 72], b[72 * 72];
    struct rctx c;
    int n = (int)(sizeof(red_bench_shapes) / sizeof(red_bench_shapes[0]));

    fill_pair(a, 72, b, 72, 72, 72, 0);
    c.a = a;
    c.b = b;
    c.as = 72;
    c.bs = 72;
    c.which = which;
    c.sink = 0;
    for (int i = 0; i < n; i++) {
        char name[40];

        c.w = red_bench_shapes[i][0];
        c.h = red_bench_shapes[i][1];
        snprintf(name, sizeof(name), "%s %dx%d", tag, c.w, c.h);
        /* The reference IS the C path for these two - there is no third
         * form - so the middle column is a control on the measurement and
         * should read like the first. */
#if CA_SIMD_SATD
        bench(name, red_ref_call, red_ref_call, red_opt_call, &c, 200000);
#else
        bench(name, red_ref_call, red_opt_call, NULL, &c, 200000);
#endif
    }
}

struct rcctx {
    const pixel *a, *b;
    int as, bs, n;
    uint32_t *row, *col;
};

static void rowcol_ref_call(void *p)
{
    struct rcctx *c = p;
    rowcol_ref(c->a, c->as, c->b, c->bs, c->n, c->row, c->col);
}

static void rowcol_opt_call(void *p)
{
    struct rcctx *c = p;
    rowcol_opt(c->a, c->as, c->b, c->bs, c->n, c->row, c->col);
}

/* the call site's three shapes, and the two rungs above and below them */
static const int rowcol_bench_shapes[] = { 4, 8, 16, 32, 64 };

static void bench_rowcol(void)
{
    static pixel a[72 * 72], b[72 * 72];
    static uint32_t row[64], col[64];
    struct rcctx c;
    int n = (int)(sizeof(rowcol_bench_shapes) /
                  sizeof(rowcol_bench_shapes[0]));

    fill_pair(a, 72, b, 72, 72, 72, 0);
    c.a = a;
    c.b = b;
    c.as = 72;
    c.bs = 72;
    c.row = row;
    c.col = col;
    for (int i = 0; i < n; i++) {
        char name[40];

        c.n = rowcol_bench_shapes[i];
        snprintf(name, sizeof(name), "sad rowcol %dx%d", c.n, c.n);
        /* the reference IS the C path here too: the middle column is a
         * control on the measurement and should read like the first */
#if CA_SIMD_SATD
        bench(name, rowcol_ref_call, rowcol_ref_call, rowcol_opt_call, &c,
              200000);
#else
        bench(name, rowcol_ref_call, rowcol_opt_call, NULL, &c, 200000);
#endif
    }
}

/* ---- psy energy and the 16-wide sample sums ---------------------------
 *
 * Both read one plane, so fill_pair's `a` is the input; kinds 1 and 4
 * put every sample at a clip end, which is the range edge for both. */
static int check_psy_sums(void)
{
    int bad = 0;
#if CA_SIMD_SATD
    static const int psy_n[] = { 4, 8, 16, 32, 64 };

    for (int i = 0; i < 5 && !bad; i++) {
        int n = psy_n[i];
        for (int is = 0; is < 2 && !bad; is++) {
            int st = is ? n + 5 : n;
            size_t bytes = (size_t)st * n * sizeof(pixel);
            pixel *a = ca_guard_alloc(bytes);
            pixel *b = ca_guard_alloc(bytes);
            char what[48];

            snprintf(what, sizeof(what), "psy energy %dx%d s%d", n, n, is);
            for (int kind = 0; kind < NKIND && !bad; kind++)
                for (int rep = 0; rep < 16 && !bad; rep++) {
                    memset(a, CA_POISON, bytes);
                    fill_pair(a, st, b, st, n, n, kind);
                    ca_guard_poison(a, bytes);
                    uint32_t r = n265_psy_energy_c(a, st, n);
                    uint32_t o = CA_SIMD(n265_psy_energy)(a, st, n);
                    if (r != o) {
                        printf("    %s MISMATCH kind %d: ref %u simd %u\n",
                               what, kind, r, o);
                        bad++;
                        break;
                    }
                    bad += ca_guard_check(a, bytes, what);
                    ca_guard_scramble(a, bytes);
                    scramble_pad(a, st, n, n);
                    if (!bad && CA_SIMD(n265_psy_energy)(a, st, n) != o) {
                        printf("    %s: result depends on bytes OUTSIDE "
                               "the block (kind %d)\n", what, kind);
                        bad++;
                    }
                }
            ca_guard_free(a);
            ca_guard_free(b);
        }
    }
    for (int h = 1; h <= 16 && !bad; h++) {
        for (int is = 0; is < 2 && !bad; is++) {
            int st = is ? 16 + 5 : 16;
            size_t bytes = (size_t)st * h * sizeof(pixel);
            pixel *a = ca_guard_alloc(bytes);
            pixel *b = ca_guard_alloc(bytes);
            char what[48];

            snprintf(what, sizeof(what), "sum_sq 16x%d s%d", h, is);
            for (int kind = 0; kind < NKIND && !bad; kind++)
                for (int rep = 0; rep < 16 && !bad; rep++) {
                    uint64_t rq, oq, oq2;
                    memset(a, CA_POISON, bytes);
                    fill_pair(a, st, b, st, 16, h, kind);
                    ca_guard_poison(a, bytes);
                    uint32_t r = n265_sum_sq_wh_c(a, st, 16, h, &rq);
                    uint32_t o = CA_SIMD(n265_sum_sq16)(a, st, h, &oq);
                    if (r != o || rq != oq) {
                        printf("    %s MISMATCH kind %d: ref %u/%llu simd "
                               "%u/%llu\n", what, kind, r,
                               (unsigned long long)rq, o,
                               (unsigned long long)oq);
                        bad++;
                        break;
                    }
                    bad += ca_guard_check(a, bytes, what);
                    ca_guard_scramble(a, bytes);
                    scramble_pad(a, st, 16, h);
                    if (!bad && (CA_SIMD(n265_sum_sq16)(a, st, h, &oq2) != o ||
                                 oq2 != oq)) {
                        printf("    %s: result depends on bytes OUTSIDE "
                               "the block (kind %d)\n", what, kind);
                        bad++;
                    }
                }
            ca_guard_free(a);
            ca_guard_free(b);
        }
    }
#endif
    return bad;
}

int check_satd(void)
{
    int bad;

    check_windows();
    bad = check_satd_tiles();

    if (bad)
        return bad;
    bad = check_satd_wh();
    if (bad)
        return bad;
    bad = check_reduction("sad", RED_SAD, sad_shapes, NSAD_SH);
    if (bad)
        return bad;
    bad = check_reduction("ssd", RED_SSD, ssd_shapes, NSSD_SH);
    if (bad)
        return bad;
#if CA_SIMD_SATD && BIT_DEPTH == 8 && defined(__ARM_FEATURE_DOTPROD)
    /* the second arm of the SSD, on a host that can run it. Both arms are
     * held to the same C reference; a host without the extension reaches
     * neither this check nor the arm. */
    if (n265_cpu_flags() & N265_CPU_DOTPROD) {
        bad = check_reduction("ssd/dot", RED_SSD_DOT, ssd_shapes, NSSD_SH);
        if (bad)
            return bad;
    }
#endif
    bad = check_rowcol();
    if (bad)
        return bad;
    bad = check_ssd_extreme();
    if (bad)
        return bad;
    bad = check_psy_sums();
    if (bad)
        return bad;
    bench_satd();
    bench_reduction("sad", RED_SAD);
    bench_reduction("ssd", RED_SSD);
    bench_rowcol();
#if CA_SIMD_SATD && BIT_DEPTH == 8 && defined(__ARM_FEATURE_DOTPROD)
    if (n265_cpu_flags() & N265_CPU_DOTPROD)
        bench_reduction("ssd/dot", RED_SSD_DOT);
#endif
    return 0;
}
