/*
 * The coefficient-domain noise reduction estimator (phase 14 item 6),
 * against a hand-written reference of the same model.
 *
 * What this checks is the part that has no oracle anywhere else: the map
 * from a population of coefficients to a per-position offset, the bound
 * on it, the fold, the halving, and the attenuation's own arithmetic
 * (toward zero, stopping at zero, sign preserved). The encoder's own use
 * of it is checked by the conformance vectors and by the escape cmp; what
 * cannot be checked there is whether the number is the number it should
 * be, because nothing downstream prints it.
 *
 * Copyright (c) 2026, the yah265 authors. GPL-2.0-or-later; see LICENSE.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "encoder/nr.h"

static int fails;
#define CHECK(cond, ...) do { \
    if (!(cond)) { fails++; printf("FAIL %s:%d: ", __FILE__, __LINE__); \
                   printf(__VA_ARGS__); printf("\n"); } \
} while (0)

/* the model, spelled out a second time and independently: offset is
 * strength divided by the position's mean magnitude plus one, never more
 * than that mean, never more than a coefficient can be */
static int ref_offset(int strength, uint64_t sum, uint64_t cnt)
{
    double m, o;
    if (strength <= 0 || cnt == 0)
        return 0;
    m = (double)sum / (double)cnt;
    o = (double)strength / (m + 1.0);
    if (o > (double)(uint64_t)(sum / cnt))
        o = (double)(uint64_t)(sum / cnt);
    if (o > 32767.0)
        o = 32767.0;
    return (int)(o + 0.5);
}

/* ---- the layout: the four groups and the four sizes do not overlap --- */
static void test_layout(void)
{
    int seen[N265_NR_SLOTS];
    memset(seen, 0, sizeof seen);
    for (int intra = 0; intra < 2; intra++)
        for (int chroma = 0; chroma < 2; chroma++)
            for (int log2n = 2; log2n <= 5; log2n++) {
                int base = n265_nr_base(intra, chroma, log2n);
                int n = 1 << (2 * log2n);
                int cat = n265_nr_cat(intra, chroma, log2n);
                CHECK(cat >= 0 && cat < N265_NR_CATS, "cat %d out of range",
                      cat);
                CHECK(base >= 0 && base + n <= N265_NR_SLOTS,
                      "base %d + %d past %d", base, n, N265_NR_SLOTS);
                for (int i = 0; i < n; i++) {
                    CHECK(!seen[base + i], "slot %d claimed twice "
                          "(intra %d chroma %d log2n %d)", base + i, intra,
                          chroma, log2n);
                    seen[base + i] = 1;
                }
            }
    int n = 0;
    for (int i = 0; i < N265_NR_SLOTS; i++)
        n += seen[i];
    CHECK(n == N265_NR_SLOTS, "%d of %d slots used", n, N265_NR_SLOTS);
}

/* ---- the offset itself, over a wide span of means and strengths ------ */
static void test_offsets(void)
{
    n265_nr *nr = calloc(1, sizeof(*nr));
    n265_nr_tally *t = calloc(1, sizeof(*t));
    const int log2n = 3, base = n265_nr_base(1, 0, log2n);
    const int cat = n265_nr_cat(1, 0, log2n);
    /* position i gets a mean magnitude of i * 3, from 0 to 189: a whole
     * 8x8 block's worth of "this position is silent" through "this
     * position carries the picture" */
    const uint64_t blocks = 1000;
    for (int i = 0; i < 64; i++)
        t->sum[base + i] = blocks * (uint64_t)(i * 3);
    t->cnt[cat] = blocks;
    n265_nr_fold(nr, t);
    CHECK(nr->cnt[cat] == blocks, "fold lost the count: %llu",
          (unsigned long long)nr->cnt[cat]);

    static const int strengths[] = { 1, 100, 300, 1000, 2000 };
    for (unsigned k = 0; k < sizeof strengths / sizeof *strengths; k++) {
        int str = strengths[k];
        n265_nr_offsets(nr, str, 0);
        for (int i = 0; i < 64; i++) {
            int want = ref_offset(str, nr->sum[base + i], nr->cnt[cat]);
            int got = nr->off[base + i];
            CHECK(abs(got - want) <= 1, "strength %d position %d (mean %d): "
                  "offset %d, reference %d", str, i, i * 3, got, want);
        }
        /* The SHAPE of the answer, not just its value, because the shape
         * is the argument. Read along rising mean, the offset climbs
         * while the bound is what decides it (offset = mean: everything
         * here is silent enough to delete outright) and falls once the
         * reciprocal takes over (offset = strength / mean: this position
         * carries the picture). One turning point, and it sits where the
         * two branches cross, at a mean of about sqrt(strength). */
        int turn = -1;
        for (int i = 1; i < 64; i++) {
            if (nr->off[base + i] > nr->off[base + i - 1] && turn >= 0)
                CHECK(0, "strength %d: offset rose again at position %d "
                      "after turning at %d", str, i, turn);
            if (nr->off[base + i] < nr->off[base + i - 1] && turn < 0)
                turn = i - 1;
        }
        if (turn > 0) {
            double peak = turn * 3.0, want = 1.0;
            while (want * want < (double)str)
                want += 1.0;
            CHECK(peak <= 2.0 * want + 6.0 && peak * 2.0 + 6.0 >= want,
                  "strength %d: the offset peaks at mean %.0f, sqrt is "
                  "%.0f", str, peak, want);
        }
        /* ...and the INTER half of the table is untouched, because this
         * call named strength 0 for it. That is the whole of what makes
         * --nr-intra alone mean --nr-intra alone. */
        int ibase = n265_nr_base(0, 0, log2n);
        for (int i = 0; i < 64; i++)
            CHECK(nr->off[ibase + i] == 0, "inter position %d is %d at "
                  "--nr-inter 0", i, nr->off[ibase + i]);
    }
    /* a silent position saturates at the strength and does not blow up */
    n265_nr_offsets(nr, 2000, 0);
    CHECK(nr->off[base] == 0, "a position with mean 0 must take 0 off, "
          "not %d", nr->off[base]);
    free(nr);
    free(t);
}

/* ---- the fold is additive and the halving keeps the ratio ------------ */
static void test_fold_and_halve(void)
{
    n265_nr *a = calloc(1, sizeof(*a));
    n265_nr *b = calloc(1, sizeof(*b));
    n265_nr_tally *t = calloc(1, sizeof(*t));
    const int log2n = 4, base = n265_nr_base(0, 0, log2n);
    const int cat = n265_nr_cat(0, 0, log2n);

    /* three folds of X equal one fold of 3X: what makes the estimate
     * independent of how the frames were partitioned */
    for (int i = 0; i < 256; i++)
        t->sum[base + i] = 40 * (uint64_t)(i + 1);
    t->cnt[cat] = 40;
    for (int k = 0; k < 3; k++)
        n265_nr_fold(a, t);
    for (int i = 0; i < 256; i++)
        t->sum[base + i] = 120 * (uint64_t)(i + 1);
    t->cnt[cat] = 120;
    n265_nr_fold(b, t);
    n265_nr_offsets(a, 0, 500);
    n265_nr_offsets(b, 0, 500);
    CHECK(!memcmp(a->off, b->off, sizeof a->off),
          "three folds and one fold of the same total disagree");

    /* the halving: cross the bound and the ratio the offset is built
     * from must be the one it was, so every offset stands still */
    n265_nr *c = calloc(1, sizeof(*c));
    for (int i = 0; i < 256; i++)
        c->sum[base + i] = (N265_NR_HALVE - 1) * (uint64_t)(i + 1);
    c->cnt[cat] = N265_NR_HALVE - 1;
    n265_nr_offsets(c, 0, 500);
    uint16_t before[256];
    memcpy(before, c->off + base, sizeof before);
    memset(t, 0, sizeof *t);
    for (int i = 0; i < 256; i++)
        t->sum[base + i] = (uint64_t)(i + 1);
    t->cnt[cat] = 1;
    n265_nr_fold(c, t);                 /* crosses N265_NR_HALVE */
    CHECK(c->cnt[cat] == N265_NR_HALVE / 2, "halving left count %llu",
          (unsigned long long)c->cnt[cat]);
    n265_nr_offsets(c, 0, 500);
    for (int i = 0; i < 256; i++)
        CHECK(abs((int)c->off[base + i] - (int)before[i]) <= 1,
              "the halving moved position %d: %d -> %d", i, before[i],
              c->off[base + i]);
    free(a);
    free(b);
    free(c);
    free(t);
}

/* ---- the attenuation: toward zero, stopping there, sign kept -------- */
static void test_block(void)
{
    uint16_t off[N265_NR_SLOTS];
    n265_nr_tally *t = calloc(1, sizeof(*t));
    int16_t in[16], out[16];
    const int log2n = 2, base = n265_nr_base(1, 0, log2n);
    const int cat = n265_nr_cat(1, 0, log2n);

    memset(off, 0, sizeof off);
    for (int i = 0; i < 16; i++)
        off[base + i] = (uint16_t)(i * 2);      /* 0, 2, 4, ... 30 */
    static const int16_t vals[16] = { 100, -100, 3, -3, 0, 7, -7, 40,
                                      -40, 1, -1, 31, -31, 32767, -32768, 15 };
    memcpy(in, vals, sizeof in);
    n265_nr_block(off, t, out, in, log2n, 1, 0);

    for (int i = 0; i < 16; i++) {
        int o = i * 2, v = vals[i];
        int a = v < 0 ? -v : v;
        int want = a - o < 0 ? 0 : a - o;
        int got = out[i];
        int ga = got < 0 ? -got : got;
        CHECK(ga == want, "position %d: |%d| - %d should be %d, got %d",
              i, v, o, want, ga);
        CHECK(got == 0 || (got > 0) == (v > 0),
              "position %d: sign flipped, %d -> %d", i, v, got);
        CHECK(ga <= a, "position %d: |%d| grew to |%d|", i, v, got);
    }
    /* the tally is of what ARRIVED, not of what was written */
    for (int i = 0; i < 16; i++) {
        int a = vals[i] < 0 ? -vals[i] : vals[i];
        CHECK(t->sum[base + i] == (uint64_t)(unsigned)a,
              "position %d tallied %llu, source magnitude is %d", i,
              (unsigned long long)t->sum[base + i], a);
    }
    CHECK(t->cnt[cat] == 1, "one block tallied as %llu",
          (unsigned long long)t->cnt[cat]);

    /* an all-zero offset table is the identity, which is what the escape
     * at --nr-intra 0 rests on */
    memset(off, 0, sizeof off);
    memset(t, 0, sizeof *t);
    n265_nr_block(off, t, out, in, log2n, 1, 0);
    CHECK(!memcmp(out, vals, sizeof out),
          "a zero offset table changed the block");
    free(t);
}

int main(void)
{
    test_layout();
    test_offsets();
    test_fold_and_halve();
    test_block();
    printf(fails ? "nr: %d failure(s)\n" : "nr: ok\n", fails);
    return fails ? 1 : 0;
}
