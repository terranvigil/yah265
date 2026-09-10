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

#include "encoder/satd.h"
#include "encoder/arm/neon.h"   /* self-guarded; the family kernels land here */

/* ---- input generators ---------------------------------------------
 *
 * Random is the weakest of these. The ones that matter are the extremes:
 * kind 1 puts every sample at a clip end, and kind 4 builds the exact
 * worst case for the Hadamard, a block whose difference is 255 times one
 * 2D basis function, which puts a single coefficient at 255 * n * n -
 * 16320 for the 8x8 tile and 4080 for the 4x4 - the top of the int16
 * range claim in satd_neon.c. If a lane were one bit too narrow that is
 * the input that says so.
 */
#define NKIND 6

/* natural-order Hadamard sign: H[k][j] = (-1)^popcount(k & j) */
static int had_sign(int k, int j)
{
    return (__builtin_popcount(k & j) & 1) ? -1 : 1;
}

static void fill_pair(uint8_t *a, int as, uint8_t *b, int bs, int w, int h,
                      int kind)
{
    int ky = (int)(rnd() & 7), kx = (int)(rnd() & 7);

    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            uint8_t va, vb;
            switch (kind) {
            case 0:
                va = (uint8_t)(rnd() >> 24);
                vb = (uint8_t)(rnd() >> 24);
                break;
            case 1:     /* both ends of the range, every sample */
                va = (uint8_t)((rnd() & 1) ? 255 : 0);
                vb = (uint8_t)((rnd() & 1) ? 255 : 0);
                break;
            case 2:     /* identical: the near-skip case, sum 0 */
                va = (uint8_t)(rnd() >> 24);
                vb = va;
                break;
            case 3: {   /* encoder-typical: a small residual on a ramp */
                int base = (x * 7 + y * 11 + (int)(rnd() & 15)) & 255;
                int d = (int)(rnd() % 9) - 4;
                int v = base + d;
                va = (uint8_t)base;
                vb = (uint8_t)(v < 0 ? 0 : v > 255 ? 255 : v);
                break;
            }
            case 4: {   /* +/-255 on one 2D Hadamard basis function */
                int s = had_sign(ky, y & 7) * had_sign(kx, x & 7);
                va = (uint8_t)(s > 0 ? 255 : 0);
                vb = (uint8_t)(255 - va);
                break;
            }
            default:    /* solid maximum difference */
                va = 255;
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
static uint32_t satd_ref_wh(const uint8_t *a, int as, const uint8_t *b,
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

static uint32_t satd_opt_wh(const uint8_t *a, int as, const uint8_t *b,
                            int bs, int w, int h)
{
#if N265_NEON
    return n265_satd_wh_neon(a, as, b, bs, w, h);
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
static void scramble_pad(uint8_t *p, int stride, int w, int h)
{
    for (int y = 0; y < h; y++)
        for (int x = w; x < stride; x++)
            p[(size_t)y * stride + x] = (uint8_t)(rnd() >> 24);
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
            size_t abytes = (size_t)as * h, bbytes = (size_t)bs * h;
            uint8_t *a = ca_guard_alloc(abytes);
            uint8_t *b = ca_guard_alloc(bbytes);
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
#if N265_NEON
    for (int t = 0; t < 2 && !bad; t++) {
        int n = t ? 8 : 4;
        for (int is = 0; is < 2 && !bad; is++) {
            int as = is ? n + 5 : n, bs = is ? n + 13 : n;
            size_t abytes = (size_t)as * n, bbytes = (size_t)bs * n;
            uint8_t *a = ca_guard_alloc(abytes);
            uint8_t *b = ca_guard_alloc(bbytes);
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
                    o = n == 8 ? n265_satd8_neon(a, as, b, bs)
                               : n265_satd4_neon(a, as, b, bs);
                    if (r != o) {
                        printf("    %s MISMATCH kind %d: ref %u neon %u\n",
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
                    o2 = n == 8 ? n265_satd8_neon(a, as, b, bs)
                                : n265_satd4_neon(a, as, b, bs);
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
    const uint8_t *a, *b;
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

#if N265_NEON
static void satd_neon_call(void *p)
{
    struct sctx *c = p;
    c->sink += n265_satd_wh_neon(c->a, c->as, c->b, c->bs, c->w, c->h);
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
    static uint8_t a[72 * 72], b[72 * 72];
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
#if N265_NEON
        bench(name, satd_ref_call, satd_c_call, satd_neon_call, &c, 40000);
#else
        bench(name, satd_ref_call, satd_c_call, NULL, &c, 40000);
#endif
    }
}


int check_satd(void)
{
    int bad = check_satd_tiles();

    if (bad)
        return bad;
    bad = check_satd_wh();
    if (bad)
        return bad;
    bench_satd();
    return 0;
}
