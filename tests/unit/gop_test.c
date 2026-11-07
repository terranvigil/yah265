/*
 * Unit tests: the open-GOP prediction barrier and the reference-list
 * rule it sits in front of, src/encoder/encoder.c. Phase 14 item 3.
 * Copyright (c) 2026, the yah265 authors. GPL-2.0-or-later; see LICENSE.
 *
 * What is under test is the whole of what makes a CRA a random access
 * point. A CRA does not empty the decoder's DPB and does not reset the
 * POC line, so nothing STRUCTURAL stops a picture after it from
 * predicting out of a picture before it - and a decoder that started at
 * the CRA does not have that picture. 7.4.2.2 and 8.3.2 draw the line
 * instead, and `n265_cra_visible` is that line:
 *
 *   a picture BELOW the barrier may list a picture below the barrier;
 *   a picture AT OR ABOVE it may not.
 *
 * Below the barrier are the pictures that precede the CRA in decoding
 * order, plus its RASL pictures - which follow the CRA in decoding order
 * and precede it in output order, and which a decoder seeking to the CRA
 * therefore throws away. At or above it are the CRA and its trailing
 * pictures, which that decoder keeps. `--radl` cuts a second band out of
 * the first with the same one-way rule, and what it buys is that the
 * pictures in that band ARE decodable at the CRA.
 *
 * Three halves, and they fail on different bugs:
 *
 *  - the BARRIER, as a hand-written truth table over one CRA at POC 24
 *    with and without a RADL band. Every cell is a sentence out of
 *    7.4.2.2, written here as the pair of POCs it is about.
 *  - a HAND-COMPUTED mini-GOP straddling that CRA, with the reference
 *    lists of every picture in it worked out on paper and pasted in.
 *    A barrier that is right and a list rule that is wrong still passes
 *    the table above; it does not pass this.
 *  - a SECOND IMPLEMENTATION of the list rule, written here from the
 *    description ("L0 is the nearest past then, for a B, the nearest
 *    future; L1 is the future then the past; each cut to ref_max") and
 *    run against the library's over an exhaustive sweep of marked sets,
 *    current POCs, barriers and RADL bands. The library's version is
 *    optimized for being the mirror of n265_build_lists; this one is
 *    optimized for being obvious.
 *
 * What a unit test cannot check is whether a decoder agrees, and for
 * this feature that is the claim that matters: docs/phase14.md item 3
 * names the seek test - cut the stream at a CRA, feed both decoders,
 * and compare against a full decode from that CRA's output position -
 * which is the half that runs in scripts/conformance.sh.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>

#include "encoder/encoder.h"

static int fails;

static void chk(int ok, const char *what)
{
    if (!ok) {
        printf("FAIL %s\n", what);
        fails++;
    }
}

/* ---- 1. the barrier ------------------------------------------------ */

/* One CRA at POC 24. Its mini-GOP straddles it: the previous anchor is
 * at 16 and the leading pictures are 17..23. 25 and up are trailing.
 *
 * Every row is `cur` looking at `p`, and the expectation is the sentence
 * named beside it. */
static void test_barrier(void)
{
    const int CRA = 24;

    /* --radl 0: the RADL band is empty, so radl_poc == cra_poc and every
     * leading picture is a RASL. */
    struct { int cur, p, want; const char *why; } t0[] = {
        { 24, 16, 0, "the CRA is above its own barrier and predicts from "
                     "nothing - it is an IRAP. (It still KEEPS the pre-CRA "
                     "anchor: an intra picture consults no list rule, and "
                     "its RPS is every picture still marked, which is what "
                     "its RASL pictures will predict from.)" },
        { 20, 16, 1, "a RASL may reference across the CRA (7.4.2.2)" },
        { 20, 24, 1, "...and the CRA, which is its other anchor" },
        { 23, 17, 1, "a RASL may reference another RASL" },
        { 25, 24, 1, "a trailing picture may reference the CRA" },
        { 25, 16, 0, "a trailing picture may NOT reach across the CRA" },
        { 25, 23, 0, "...nor use a RASL, which a seek would have discarded" },
        { 32, 28, 1, "two trailing pictures are ordinary TRAIL pictures" },
        { 16, 8,  1, "below the barrier the rule is inert" },
    };
    for (size_t i = 0; i < sizeof t0 / sizeof *t0; i++)
        chk(n265_cra_visible(CRA, CRA, t0[i].cur, t0[i].p) == t0[i].want,
            t0[i].why);

    /* --radl 2: 22 and 23 are RADL pictures, so radl_poc is 22. */
    struct { int cur, p, want; const char *why; } t2[] = {
        { 22, 24, 1, "a RADL may reference the CRA - that is the point of it" },
        { 23, 22, 1, "...and another RADL" },
        { 22, 16, 0, "a RADL may NOT reach across the CRA (7.4.2.2)" },
        { 23, 21, 0, "...nor use a RASL, which a seek would have discarded" },
        { 21, 22, 1, "a RASL may reference a RADL: both survive a full decode, "
                     "and on a seek the RASL is the one thrown away" },
        { 21, 16, 1, "a RASL below the band still reaches across" },
        { 25, 22, 0, "a trailing picture may not use a RADL either" },
    };
    for (size_t i = 0; i < sizeof t2 / sizeof *t2; i++)
        chk(n265_cra_visible(CRA, CRA - 2, t2[i].cur, t2[i].p) == t2[i].want,
            t2[i].why);

    /* no CRA behind us: every --no-open-gop stream, and every stream
     * before the first CRA in an open-GOP one */
    chk(n265_cra_visible(INT_MIN, INT_MIN, 25, 16) == 1,
        "with no barrier down, nothing is hidden");
}

/* ---- 2. a hand-computed mini-GOP ----------------------------------- */

/* The same shape, coded: the anchor at 16 is already marked, the CRA at
 * 24 goes first, then the pyramid over 17..23 in the order plan_brange
 * emits - 20, 18, 17, 19, 22, 21, 23 - with 20, 18 and 22 marked as
 * references and 17, 19, 21, 23 leaves.
 *
 * At ref_max 2 and --radl 0, the lists are (past nearest first, then
 * future for a B):
 *
 *   picture  marked when it is coded      L0            L1
 *   24 CRA   16                           -             -       (intra)
 *   20       16, 24                       16, 24        24, 16
 *   18       16, 20, 24                   16, 20        20, 24
 *   17       16, 18, 20, 24               16, 18        18, 20
 *   19       16, 18, 20, 24               18, 16        20, 24
 *   22       16, 18, 20, 24               20, 18        24, 20
 *   21       16, 18, 20, 22, 24           20, 18        22, 24
 *   23       16, 18, 20, 22, 24           22, 20        24, 22
 *
 * `used` is the union of the two lists, which is what the retirement
 * reads; the table below is that union, hand-taken from the columns. */
static void test_minigop(void)
{
    const int CRA = 24;
    struct {
        int cur, is_b, nm; int m[8]; int used[8]; const char *what;
    } g[] = {
        /* the leading pictures, in coding order */
        { 20, 1, 2, { 16, 24 },                 { 1, 1 },
          "the first leaf under the CRA takes the anchor and the CRA" },
        { 18, 1, 3, { 16, 20, 24 },             { 1, 1, 1 },
          "18's L0 is 16, 20 and its L1 is 20, 24 - two futures, because "
          "there is only one picture behind it" },
        { 17, 1, 4, { 16, 18, 20, 24 },         { 1, 1, 1, 0 },
          "17's L1 reaches 20, so 24 is still out" },
        { 19, 1, 4, { 16, 18, 20, 24 },         { 1, 1, 1, 1 },
          "19 sits between 18 and 20 and its L1 runs 20, 24" },
        { 22, 1, 4, { 16, 18, 20, 24 },         { 0, 1, 1, 1 },
          "22's past is 20 then 18: the anchor at 16 falls out of reach, "
          "which is what lets the retirement drop it" },
        { 21, 1, 5, { 16, 18, 20, 22, 24 },     { 0, 1, 1, 1, 1 },
          "21 likewise" },
        { 23, 1, 5, { 16, 18, 20, 22, 24 },     { 0, 0, 1, 1, 1 },
          "23 is the deepest leaf and reaches nothing below 20" },
        /* ...and the first trailing picture of the next mini-GOP, which
         * is where the barrier does its work: 16 through 23 are all
         * below it, so a 32-anchor's only past reference is the CRA */
        { 32, 0, 4, { 16, 20, 22, 24 },         { 0, 0, 0, 1 },
          "the first trailing anchor has ONE past reference however much "
          "is still marked: everything but the CRA is behind the barrier, "
          "so a --ref 2 encode codes it the way a --ref 1 one would" },
        { 34, 0, 6, { 16, 20, 22, 24, 28, 32 }, { 0, 0, 0, 0, 1, 1 },
          "...and a later one fills its list from post-CRA pictures alone" },
    };
    for (size_t i = 0; i < sizeof g / sizeof *g; i++) {
        uint8_t used[8];
        n265_ref_used(CRA, CRA, g[i].m, g[i].nm, g[i].cur, g[i].is_b, 2, used);
        int ok = 1;
        for (int k = 0; k < g[i].nm; k++)
            ok &= used[k] == g[i].used[k];
        if (!ok) {
            printf("  cur=%d got", g[i].cur);
            for (int k = 0; k < g[i].nm; k++) printf(" %d", used[k]);
            printf(", want");
            for (int k = 0; k < g[i].nm; k++) printf(" %d", g[i].used[k]);
            printf("\n");
        }
        chk(ok, g[i].what);
    }

    /* the same mini-GOP at --radl 2. 22 and 23 are RADL now, and the
     * band is all they can see: 22 has no visible past at all and both
     * of its lists are the CRA, 23's past is 22 alone. */
    int m[5] = { 16, 18, 20, 22, 24 };
    uint8_t used[5];
    int want22[5] = { 0, 0, 0, 0, 1 };      /* the CRA and nothing else */
    int want23[5] = { 0, 0, 0, 1, 1 };      /* 22, then the CRA */
    n265_ref_used(CRA, CRA - 2, m, 5, 22, 1, 2, used);
    for (int k = 0; k < 5; k++)
        chk(used[k] == want22[k],
            "the first RADL has nothing in the band below it and predicts "
            "from the CRA alone - which is what it costs, and why --radl "
            "defaults to 0");
    n265_ref_used(CRA, CRA - 2, m, 5, 23, 1, 2, used);
    for (int k = 0; k < 5; k++)
        chk(used[k] == want23[k],
            "the second RADL reaches the first, and nothing further back");
}

/* ---- 3. a second implementation, swept -------------------------------
 *
 * Written from the prose and not from the code: collect what is visible,
 * split it at `cur`, sort each side by distance, and fill L0 = past then
 * future, L1 = future then past, each cut at ref_max. */
static void ref_used_plain(int cra, int radl, const int *m, int nm, int cur,
                           int is_b, int ref_max, uint8_t *used)
{
    int past[16], np = 0, fut[16], nf = 0;
    for (int i = 0; i < nm; i++) {
        used[i] = 0;
        if (cra != INT_MIN) {           /* the barrier, read off the prose */
            int below = m[i] < cra;
            if (below && cur >= cra) continue;
            if (below && cur < cra && cur >= radl && m[i] < radl) continue;
        }
        if (m[i] < cur) past[np++] = i;
        else if (m[i] > cur) fut[nf++] = i;
    }
    /* selection sort, nearest first on each side */
    for (int a = 0; a < np; a++)
        for (int b = a + 1; b < np; b++)
            if (m[past[b]] > m[past[a]]) { int t = past[a]; past[a] = past[b]; past[b] = t; }
    for (int a = 0; a < nf; a++)
        for (int b = a + 1; b < nf; b++)
            if (m[fut[b]] < m[fut[a]]) { int t = fut[a]; fut[a] = fut[b]; fut[b] = t; }
    int l0[16], n0 = 0, l1[16], n1 = 0;
    for (int i = 0; i < np && n0 < ref_max; i++) l0[n0++] = past[i];
    if (is_b) {
        for (int i = 0; i < nf && n0 < ref_max; i++) l0[n0++] = fut[i];
        for (int i = 0; i < nf && n1 < ref_max; i++) l1[n1++] = fut[i];
        for (int i = 0; i < np && n1 < ref_max; i++) l1[n1++] = past[i];
    }
    for (int i = 0; i < n0; i++) used[l0[i]] = 1;
    for (int i = 0; i < n1; i++) used[l1[i]] = 1;
}

/* One deterministic pseudo-random sweep, so a failure names a seed and
 * reproduces. No encoder, no DPB: the whole rule is POCs. */
static void test_sweep(void)
{
    uint32_t st = 0x9e3779b9u;
    int bad = 0;
    for (int it = 0; it < 200000 && !bad; it++) {
        st = st * 1664525u + 1013904223u;
        uint32_t r = st;
        int nm = 1 + (int)(r % 8); r /= 8;
        int ref_max = 1 + (int)(r % 3); r /= 3;
        int is_b = (int)(r & 1); r >>= 1;
        int m[8];
        for (int i = 0; i < nm; i++) {
            st = st * 1664525u + 1013904223u;
            m[i] = (int)(st % 64);
            for (int k = 0; k < i; k++)      /* POCs are unique when marked */
                if (m[k] == m[i]) { m[i] = -1; break; }
        }
        int n = 0;
        for (int i = 0; i < nm; i++) if (m[i] >= 0) m[n++] = m[i];
        nm = n;
        if (!nm) continue;
        st = st * 1664525u + 1013904223u;
        int cur = (int)(st % 64);
        st = st * 1664525u + 1013904223u;
        int cra = (st & 3) == 0 ? INT_MIN : (int)((st >> 2) % 64);
        st = st * 1664525u + 1013904223u;
        int radl = cra == INT_MIN ? INT_MIN : cra - (int)(st % 4);
        uint8_t a[8], b[8];
        n265_ref_used(cra, radl, m, nm, cur, is_b, ref_max, a);
        ref_used_plain(cra, radl, m, nm, cur, is_b, ref_max, b);
        if (memcmp(a, b, (size_t)nm)) {
            printf("  sweep it=%d cra=%d radl=%d cur=%d is_b=%d ref=%d m=",
                   it, cra, radl, cur, is_b, ref_max);
            for (int i = 0; i < nm; i++) printf(" %d", m[i]);
            printf("\n   lib ");
            for (int i = 0; i < nm; i++) printf("%d", a[i]);
            printf("  plain ");
            for (int i = 0; i < nm; i++) printf("%d", b[i]);
            printf("\n");
            bad = 1;
        }
    }
    chk(!bad, "the list rule agrees with a second implementation of it");
}

/* ---- 4. the one-way property --------------------------------------
 *
 * The barrier is only worth anything if it is asymmetric: below may look
 * below, at-or-above may not. Checked as a property rather than as
 * cells, because a barrier implemented as "hide pictures below it from
 * everyone" would pass every row of the table above that has a trailing
 * `cur` and would break every RASL. */
static void test_oneway(void)
{
    int ok = 1;
    for (int cra = 4; cra <= 40; cra += 4)
        for (int cur = 0; cur < 48; cur++)
            for (int p = 0; p < 48; p++) {
                int v = n265_cra_visible(cra, cra, cur, p);
                if (p >= cra)                 ok &= v == 1;
                else if (cur >= cra)          ok &= v == 0;
                else                          ok &= v == 1;
            }
    chk(ok, "the barrier is one-way at every CRA position");

    /* ...and the RADL band is the same shape one level down */
    ok = 1;
    for (int cra = 8; cra <= 40; cra += 8)
        for (int n = 1; n <= 4; n++)
            for (int cur = 0; cur < 48; cur++)
                for (int p = 0; p < 48; p++) {
                    int v = n265_cra_visible(cra, cra - n, cur, p);
                    int leading = cur < cra, rad = leading && cur >= cra - n;
                    int want = p >= cra ? 1
                             : !leading   ? 0
                             : rad        ? (p >= cra - n) : 1;
                    ok &= v == want;
                }
    chk(ok, "the RADL band is one-way inside the leading band");
}

int main(void)
{
    test_barrier();
    test_minigop();
    test_sweep();
    test_oneway();
    if (fails) {
        printf("gop: %d failure(s)\n", fails);
        return 1;
    }
    printf("gop: ok\n");
    return 0;
}
