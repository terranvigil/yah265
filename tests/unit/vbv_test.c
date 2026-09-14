/*
 * Unit tests: the hypothetical decoder buffer's fill arithmetic
 * (src/encoder/vbv.c).
 * Copyright (c) 2026, the yah265 authors. GPL-2.0-or-later; see LICENSE.
 *
 * The encode-side half of the same check is scripts/vbv_check.py, which
 * re-derives the whole trajectory from the coded picture sizes and must
 * agree with the encoder's own columns to the bit. What is checked here is
 * the algebra those two share: that the fill walks the way C.2.2 says, that
 * the allowance is exactly the bound that keeps it non-negative, and that
 * the cumulative bound over the window is what an expensive frame behind
 * this one reserves for itself.
 */

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "encoder/vbv.h"

static int fails;

#define CHECK(cond, ...) do { \
    if (!(cond)) { fails++; printf("FAIL %s:%d: ", __FILE__, __LINE__); \
                   printf(__VA_ARGS__); printf("\n"); } \
} while (0)

#define NEAR(a, b) (fabs((a) - (b)) < 1e-6)

/* 1 Mbit/s, a one-second buffer, 25 fps: 40 000 bits arrive per frame
 * period and the buffer holds a million. */
static void v_init(n265_vbv *v, double init, double safe, int la)
{
    n265_vbv_init(v, 1e6, 1e6, init, 25.0, safe, la);
}

static void test_open(void)
{
    n265_vbv v;
    v_init(&v, 0.9, 1.0, 1);
    CHECK(v.on, "a positive maxrate and bufsize is on");
    CHECK(NEAR(v.inflow, 40000.0), "inflow %g", v.inflow);
    CHECK(NEAR(v.fill, 900000.0), "fill %g", v.fill);
    CHECK(v.underflows == 0 && v.clamped == 0, "counters open at zero");

    /* --vbv-init 1.5 would be a buffer fuller than it can be */
    n265_vbv_init(&v, 1e6, 1e6, 1.5, 25.0, 1.0, 1);
    CHECK(NEAR(v.fill, 1e6), "init is capped at the buffer: %g", v.fill);

    /* either half missing is the mode off */
    n265_vbv_init(&v, 0.0, 1e6, 0.9, 25.0, 1.0, 1);
    CHECK(!v.on, "no maxrate is off");
    n265_vbv_init(&v, 1e6, 0.0, 0.9, 25.0, 1.0, 1);
    CHECK(!v.on, "no bufsize is off");
}

/* The walk of C.2.2, one frame at a time, with the cap on the way up. */
static void test_walk(void)
{
    n265_vbv v;
    double before = 0.0, after = 0.0;
    v_init(&v, 0.9, 1.0, 1);

    /* a frame that costs exactly one period of arrival leaves the fill
     * where it was */
    n265_vbv_update(&v, 40000.0, &before, &after);
    CHECK(NEAR(before, 900000.0), "before %g", before);
    CHECK(NEAR(after, 900000.0), "after %g", after);
    CHECK(NEAR(v.min_fill, 860000.0), "the min is POST-removal: %g",
          v.min_fill);

    /* a cheap frame fills it, and the cap is the buffer and not more */
    for (int i = 0; i < 10; i++)
        n265_vbv_update(&v, 0.0, NULL, &after);
    CHECK(NEAR(after, 1e6), "the fill saturates at bufsize: %g", after);
    CHECK(v.underflows == 0, "no underflow on the way up");

    /* ...and one frame bigger than the whole buffer underflows, once, and
     * carries the deficit rather than forgetting it */
    n265_vbv_update(&v, 1.2e6, &before, &after);
    CHECK(v.underflows == 1, "underflows %ld", v.underflows);
    CHECK(NEAR(v.min_fill, -200000.0), "min_fill %g", v.min_fill);
    CHECK(NEAR(after, -160000.0), "the deficit is carried: %g", after);
    n265_vbv_update(&v, 0.0, &before, &after);
    CHECK(v.underflows == 2, "a removal from a negative fill underflows too");
    CHECK(NEAR(after, -120000.0), "and recovers one period at a time: %g",
          after);
}

/* The allowance IS the bound that keeps the fill non-negative: spending
 * exactly it lands the post-removal fill on zero, and one bit more is an
 * underflow. That equivalence is the whole claim the gate rests on. */
static void test_allow_is_the_bound(void)
{
    n265_vbv v, w;
    double after = 0.0;
    v_init(&v, 0.9, 1.0, 1);
    double a = n265_vbv_allow(&v, NULL, 0);
    CHECK(NEAR(a, 900000.0), "allow %g", a);

    w = v;
    n265_vbv_update(&w, a, NULL, &after);
    CHECK(w.underflows == 0, "spending the allowance does not underflow");
    CHECK(NEAR(w.min_fill, 0.0), "...and lands on zero: %g", w.min_fill);

    w = v;
    n265_vbv_update(&w, a + 1.0, NULL, &after);
    CHECK(w.underflows == 1, "one bit over the allowance underflows");

    /* the safety fraction is a fraction of exactly that bound */
    v_init(&v, 0.9, 0.9, 1);
    CHECK(NEAR(n265_vbv_allow(&v, NULL, 0), 810000.0), "safe scales it");

    /* an empty buffer still allows something, so the bits-to-QP solve
     * stays finite */
    n265_vbv_init(&v, 1e6, 1e6, 0.0, 25.0, 1.0, 1);
    CHECK(n265_vbv_allow(&v, NULL, 0) >= N265_VBV_MIN_ALLOW,
          "the floor holds at an empty buffer");
}

/* The window's reservation. Three frames of 400 000 bits behind this one
 * cannot be paid for out of a buffer that only refills 40 000 a frame, so
 * the cumulative bound bites at the k that is tightest and not at k = 0. */
static void test_window_reservation(void)
{
    n265_vbv v;
    v_init(&v, 0.9, 1.0, 1);
    double fut[3] = { 400000.0, 400000.0, 400000.0 };

    /* k = 1: 900000 + 40000 - 400000 = 540000
     * k = 2: 900000 + 80000 - 800000 = 180000
     * k = 3: 900000 + 120000 - 1200000 = -180000 -> the floor */
    CHECK(NEAR(n265_vbv_allow(&v, fut, 1), 540000.0), "k=1");
    CHECK(NEAR(n265_vbv_allow(&v, fut, 2), 180000.0), "k=2");
    CHECK(NEAR(n265_vbv_allow(&v, fut, 3), N265_VBV_MIN_ALLOW), "k=3");

    /* a window of cheap frames reserves nothing: the k = 0 bound stands */
    double cheap[4] = { 1000.0, 1000.0, 1000.0, 1000.0 };
    CHECK(NEAR(n265_vbv_allow(&v, cheap, 4), 900000.0), "cheap window");

    /* ...and with the lookahead leg off, neither does an expensive one */
    v_init(&v, 0.9, 1.0, 0);
    CHECK(NEAR(n265_vbv_allow(&v, fut, 3), 900000.0), "lookahead off");
}

/* Spending the allowance every frame, with the window told the truth, can
 * never underflow - which is the induction the per-frame clamp is. */
static void test_never_underflows_on_plan(void)
{
    n265_vbv v;
    v_init(&v, 0.5, 1.0, 1);
    double fut[8];
    for (int i = 0; i < 8; i++)
        fut[i] = 60000.0;           /* every future frame over budget */
    for (int k = 0; k < 200; k++) {
        double a = n265_vbv_allow(&v, fut, 8);
        n265_vbv_update(&v, a, NULL, NULL);
    }
    CHECK(v.underflows == 0, "underflows %ld over 200 frames", v.underflows);
    CHECK(v.min_fill >= -1e-6, "min_fill %g", v.min_fill);
}

int main(void)
{
    test_open();
    test_walk();
    test_allow_is_the_bound();
    test_window_reservation();
    test_never_underflows_on_plan();
    if (fails)
        printf("%d failure(s)\n", fails);
    else
        printf("vbv: all checks passed\n");
    return fails ? 1 : 0;
}
