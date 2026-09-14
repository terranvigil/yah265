/*
 * Unit tests: the two-pass stats parser and the whole-file rate-factor
 * solve (src/encoder/twopass.c).
 *
 * The solve is the one piece of this item that decides every frame's
 * quantiser before a single frame is coded, so it is also the whole of the
 * determinism claim - there is nothing in it a thread could reach. That
 * makes it exactly the kind of thing to check as algebra rather than
 * through an encode: what is checked here is that the plan SUMS TO THE
 * TARGET, that it reproduces pass 1 where it should, that qcomp moves the
 * quantiser in x264's direction and no further than the clamp, and that a
 * file the parser should refuse is refused instead of half-read.
 *
 * Copyright (c) 2026, the yah265 authors. GPL-2.0-or-later; see LICENSE.
 */

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "encoder/twopass.h"

static int fails;

#define CHECK(cond, ...) do { \
    if (!(cond)) { fails++; printf("FAIL %s:%d: ", __FILE__, __LINE__); \
                   printf(__VA_ARGS__); printf("\n"); } \
} while (0)

#define NEAR(a, b, tol) (fabs((a) - (b)) <= (tol))

/* write `body` under a header and hand back the path (static buffer) */
static const char *tmpfile_with(const char *body)
{
    static char path[] = "/tmp/n265_2p_XXXXXX";
    static char used[sizeof path];
    strcpy(path, "/tmp/n265_2p_XXXXXX");
    int fd = mkstemp(path);
    if (fd < 0)
        return NULL;
    FILE *f = fdopen(fd, "w");
    fputs(body, f);
    fclose(f);
    strcpy(used, path);
    return used;
}

static const char *hdr =
    "#n265-2pass 1\n"
    "#geom w=352 h=288 fps=30000/1001 nblk=396\n"
    "#enc test\n"
    "#cols coding display type nonref flat qp bits cplx icost pcost prop ctf\n";

/* ---- the parser ---------------------------------------------------- */

static void test_parse(void)
{
    char body[4096];
    snprintf(body, sizeof body, "%s"
             "0 0 2 0 0 23.500000 80000 360000 360000 360000 2000.000 -4.300000\n"
             "\n"
             "1 8 1 0 0 27.000000 15000 88000 361000 88000 900.000 -2.900000\n"
             "2 4 0 1 1 30.250000 5000 72000 358000 72000 1600.000 -3.900000\n",
             hdr);
    const char *p = tmpfile_with(body);
    n265_2p tp;
    char err[256] = { 0 };
    n265_2p_init(&tp);
    int n = n265_2p_read(&tp, p, err, sizeof err);
    CHECK(n == 3, "read %d frames (%s)", n, err);
    CHECK(tp.nblk == 396, "nblk %d", tp.nblk);
    CHECK(tp.width == 352 && tp.height == 288, "geom %dx%d", tp.width,
          tp.height);
    CHECK(tp.fr[0].slice_type == 2 && tp.fr[0].display == 0,
          "frame 0 type %d display %d", tp.fr[0].slice_type,
          tp.fr[0].display);
    CHECK(NEAR(tp.fr[1].qp, 27.0, 1e-9) && NEAR(tp.fr[1].bits, 15000.0, 1e-9),
          "frame 1 qp %g bits %g", tp.fr[1].qp, tp.fr[1].bits);
    CHECK(tp.fr[2].nonref == 1 && tp.fr[2].flat == 1, "frame 2 flags");
    CHECK(NEAR(tp.fr[2].ctf, -3.9, 1e-9), "frame 2 ctf %g", tp.fr[2].ctf);
    /* the lookup is by DISPLAY index and the file is in coding order */
    n265_2p_solve(&tp, 1e5, 1.0, 6.0, 0.0, 0.0, 51.0);
    CHECK(n265_2p_get(&tp, 8) == &tp.fr[1], "display 8 is the second line");
    CHECK(n265_2p_get(&tp, 4) == &tp.fr[2], "display 4 is the third line");
    CHECK(n265_2p_get(&tp, 3) == NULL, "display 3 is not in the file");
    CHECK(n265_2p_get(&tp, 99) == NULL, "display 99 is off the end");
    unlink(p);
    n265_2p_free(&tp);
}

static void test_parse_refusals(void)
{
    n265_2p tp;
    char err[256];
    const char *p;

    /* a version this build does not write */
    p = tmpfile_with("#n265-2pass 99\n#geom w=1 h=1 fps=1/1 nblk=4\n"
                     "0 0 2 0 0 23 80000 360000 360000 360000 0 0\n");
    n265_2p_init(&tp);
    err[0] = 0;
    CHECK(n265_2p_read(&tp, p, err, sizeof err) < 0 && err[0],
          "a foreign version is refused");
    unlink(p);
    n265_2p_free(&tp);

    /* a truncated line: 11 columns, not 12 */
    char body[1024];
    snprintf(body, sizeof body, "%s0 0 2 0 0 23.5 80000 360000 360000 360000 2000\n",
             hdr);
    p = tmpfile_with(body);
    n265_2p_init(&tp);
    err[0] = 0;
    CHECK(n265_2p_read(&tp, p, err, sizeof err) < 0 && err[0],
          "a short line is refused, not padded");
    unlink(p);
    n265_2p_free(&tp);

    /* no geometry: the complexity normalisation has no divisor */
    p = tmpfile_with("#n265-2pass 1\n"
                     "0 0 2 0 0 23.5 80000 360000 360000 360000 0 0\n");
    n265_2p_init(&tp);
    err[0] = 0;
    CHECK(n265_2p_read(&tp, p, err, sizeof err) < 0 && err[0],
          "a header with no nblk is refused");
    unlink(p);
    n265_2p_free(&tp);

    /* a file of nothing but comments */
    p = tmpfile_with(hdr);
    n265_2p_init(&tp);
    err[0] = 0;
    CHECK(n265_2p_read(&tp, p, err, sizeof err) < 0 && err[0],
          "a file with no frames is refused");
    unlink(p);
    n265_2p_free(&tp);

    n265_2p_init(&tp);
    err[0] = 0;
    CHECK(n265_2p_read(&tp, "/nonexistent/n265/stats", err, sizeof err) < 0,
          "a missing file is refused");
    n265_2p_free(&tp);
}

/* ---- the solve ------------------------------------------------------ */

/* build a plan in memory, bypassing the file: the solve is what is under
 * test here and the parser has its own cases above */
static void mk(n265_2p *tp, int n, const double *bits, const double *qp,
               const double *cplx)
{
    n265_2p_init(tp);
    tp->fr = calloc((size_t)n, sizeof *tp->fr);
    tp->n = tp->cap = n;
    tp->nblk = 396;
    for (int i = 0; i < n; i++) {
        tp->fr[i].coding = tp->fr[i].display = i;
        tp->fr[i].bits = bits[i];
        tp->fr[i].qp = qp[i];
        tp->fr[i].cplx = cplx[i];
        tp->fr[i].slice_type = i == 0 ? 2 : 0;
    }
}

static double plan_sum(const n265_2p *tp)
{
    double s = 0.0;
    for (int i = 0; i < tp->n; i++)
        s += tp->fr[i].plan_bits;
    return s;
}

/* The plan sums to the target. It is the one property the whole item rests
 * on: the rate factor is solved so it does, and if it did not the second
 * pass would be a first pass with extra steps. */
static void test_sums_to_target(void)
{
    double bits[]  = { 80000, 15000, 5000, 2400, 9000, 3000 };
    double qp[]    = { 23.5, 27.0, 30.25, 31.0, 28.5, 30.0 };
    double cplx[]  = { 360000, 88000, 72000, 73000, 91000, 70000 };
    n265_2p tp;
    for (double qc = 0.0; qc <= 1.0001; qc += 0.25) {
        for (double tgt = 20000.0; tgt < 2e6; tgt *= 7.0) {
            mk(&tp, 6, bits, qp, cplx);
            CHECK(n265_2p_solve(&tp, tgt, qc, 6.0, 0.0, 0.0, 51.0) == 0,
                  "solve qcomp %g target %g", qc, tgt);
            CHECK(NEAR(plan_sum(&tp), tgt, tgt * 1e-9),
                  "qcomp %g target %g: plan sums to %g", qc, tgt,
                  plan_sum(&tp));
            CHECK(NEAR(tp.plan_bits, plan_sum(&tp), 1e-6),
                  "the reported total is the plan's own");
            /* every frame's plan is its own rate curve read at its own
             * planned quantiser - no second model anywhere */
            for (int i = 0; i < tp.n; i++)
                CHECK(NEAR(tp.fr[i].plan_bits,
                           tp.fr[i].x / n265_2p_qscale(tp.fr[i].plan_qp),
                           tp.fr[i].plan_bits * 1e-9),
                      "frame %d plan is off its own curve", i);
            n265_2p_free(&tp);
        }
    }
}

/* At qcomp 1 the plan is a pure RE-LEVEL: every frame keeps the quality
 * relationship pass 1 gave it and the whole file moves by one constant.
 * This is the arm the qcomp allocation is measured against, so it has to
 * be exactly that and not approximately. */
static void test_qcomp_one_is_flat(void)
{
    double bits[] = { 80000, 15000, 5000, 9000 };
    double qp[]   = { 23.5, 27.0, 30.25, 28.5 };
    double cplx[] = { 360000, 88000, 72000, 910000 };
    n265_2p tp;
    mk(&tp, 4, bits, qp, cplx);
    n265_2p_solve(&tp, 8e4, 1.0, 6.0, 0.0, 0.0, 51.0);
    for (int i = 0; i < tp.n; i++) {
        CHECK(NEAR(tp.fr[i].d, 0.0, 1e-12), "frame %d offset %g at qcomp 1",
              i, tp.fr[i].d);
        CHECK(NEAR(tp.fr[i].plan_qp, tp.rf, 1e-9),
              "frame %d rides the rate factor", i);
    }
    n265_2p_free(&tp);
}

/* ...and the file coded at ONE quantiser, asked for exactly the bits it
 * already spent, is planned at that same quantiser. The anchor is a
 * measurement, so the solve has to be the identity here. */
static void test_reproduces_pass1(void)
{
    double bits[] = { 80000, 15000, 5000, 9000, 2400 };
    double qp[]   = { 28.0, 28.0, 28.0, 28.0, 28.0 };
    double cplx[] = { 360000, 88000, 72000, 91000, 73000 };
    double total = 0.0;
    for (int i = 0; i < 5; i++)
        total += bits[i];
    n265_2p tp;
    mk(&tp, 5, bits, qp, cplx);
    n265_2p_solve(&tp, total, 1.0, 6.0, 0.0, 0.0, 51.0);
    CHECK(NEAR(tp.rf, 28.0, 1e-9), "rate factor %g, expected 28", tp.rf);
    for (int i = 0; i < tp.n; i++)
        CHECK(NEAR(tp.fr[i].plan_bits, bits[i], bits[i] * 1e-9),
              "frame %d planned %g, spent %g", i, tp.fr[i].plan_bits,
              bits[i]);
    n265_2p_free(&tp);
}

/* qcomp's direction is x264's and it is the whole of the allocation: a
 * frame harder than the file's geometric mean is coded COARSER. Getting
 * this backwards would be a quality-levelling encoder that still hit its
 * rate, which is the refusal docs/phase7-shot-design.md already records
 * one level up. */
static void test_qcomp_direction_and_clamp(void)
{
    double bits[] = { 20000, 20000, 20000, 20000 };
    double qp[]   = { 28.0, 28.0, 28.0, 28.0 };
    double cplx[] = { 10000, 40000, 160000, 640000 };
    n265_2p tp;
    mk(&tp, 4, bits, qp, cplx);
    n265_2p_solve(&tp, 8e4, 0.6, 100.0, 0.0, -12.0, 51.0);
    for (int i = 1; i < tp.n; i++)
        CHECK(tp.fr[i].plan_qp > tp.fr[i - 1].plan_qp,
              "frame %d (cplx %g) is not coarser than frame %d", i, cplx[i],
              i - 1);
    /* each step is a factor of four in complexity: 6 * (1 - 0.6) * 2 QP */
    CHECK(NEAR(tp.fr[1].d - tp.fr[0].d, 4.8, 1e-9),
          "the step is %g QP, expected 4.8", tp.fr[1].d - tp.fr[0].d);
    /* the offsets are measured against the GEOMETRIC mean, so they sum to
     * zero and the plan is a pure re-allocation with no level in it */
    double s = 0.0;
    for (int i = 0; i < tp.n; i++)
        s += tp.fr[i].d;
    CHECK(NEAR(s, 0.0, 1e-9), "the offsets sum to %g, not zero", s);
    n265_2p_free(&tp);

    /* ...and the clamp bounds them, on both sides */
    mk(&tp, 4, bits, qp, cplx);
    n265_2p_solve(&tp, 8e4, 0.6, 2.0, 0.0, -12.0, 51.0);
    for (int i = 0; i < tp.n; i++)
        CHECK(fabs(tp.fr[i].d) <= 2.0 + 1e-9, "frame %d offset %g past the "
              "clamp", i, tp.fr[i].d);
    CHECK(NEAR(tp.fr[0].d, -2.0, 1e-9) && NEAR(tp.fr[3].d, 2.0, 1e-9),
          "the extremes sit on the clamp");
    n265_2p_free(&tp);
}

/* A target the QP range cannot reach. The plan may not answer with a
 * quantiser that does not exist, and the frames it pins have to stay
 * pinned - the walk that takes them out of the solve is finite only
 * because pinning is one-way. */
static void test_qp_range(void)
{
    double bits[] = { 80000, 15000, 5000, 9000 };
    double qp[]   = { 28.0, 28.0, 28.0, 28.0 };
    double cplx[] = { 360000, 88000, 72000, 91000 };
    n265_2p tp;

    mk(&tp, 4, bits, qp, cplx);
    n265_2p_solve(&tp, 10.0, 1.0, 6.0, 0.0, 0.0, 51.0);      /* absurdly small */
    for (int i = 0; i < tp.n; i++)
        CHECK(NEAR(tp.fr[i].plan_qp, 51.0, 1e-9),
              "frame %d planned at %g, not the coarsest quantiser there is",
              i, tp.fr[i].plan_qp);
    CHECK(plan_sum(&tp) > 10.0, "a target below what QP 51 costs is missed, "
          "not met by an invented quantiser");
    n265_2p_free(&tp);

    /* ...and the case the pinning walk exists for: the qcomp offset takes
     * ONE frame past a bound while the rest still have room. That frame is
     * fixed there, its bits come off the target, and the others are
     * re-solved around them - so the plan still sums to the target exactly,
     * which is the property a bound could otherwise quietly break. The
     * floor here is a made-up 19 rather than QpBdOffsetY, because what is
     * under test is the walk and not which number bounds it. */
    double wide[] = { 1000.0, 4000.0, 16000.0, 64000.0 };
    mk(&tp, 4, bits, qp, wide);
    n265_2p_solve(&tp, 266180.0, 0.6, 100.0, 0.0, 19.0, 51.0);
    CHECK(tp.fr[0].pinned && NEAR(tp.fr[0].plan_qp, 19.0, 1e-9),
          "the easiest frame is pinned at the floor (%g, pinned %d)",
          tp.fr[0].plan_qp, tp.fr[0].pinned);
    for (int i = 1; i < tp.n; i++)
        CHECK(!tp.fr[i].pinned, "frame %d should still be in the solve", i);
    CHECK(NEAR(plan_sum(&tp), 266180.0, 266180.0 * 1e-9),
          "and the plan still sums to the target: %g", plan_sum(&tp));
    n265_2p_free(&tp);

    mk(&tp, 4, bits, qp, cplx);
    n265_2p_solve(&tp, 1e12, 1.0, 6.0, 0.0, 0.0, 51.0);      /* absurdly large */
    for (int i = 0; i < tp.n; i++)
        CHECK(tp.fr[i].plan_qp >= -1e-9 && tp.fr[i].plan_qp <= 51.0 + 1e-9,
              "frame %d planned at %g", i, tp.fr[i].plan_qp);
    n265_2p_free(&tp);

    /* the 8-bit floor is 0 and Main10's is -12; both are the caller's */
    mk(&tp, 4, bits, qp, cplx);
    n265_2p_solve(&tp, 1e12, 1.0, 6.0, 0.0, -12.0, 51.0);
    for (int i = 0; i < tp.n; i++)
        CHECK(tp.fr[i].plan_qp >= -12.0 - 1e-9,
              "frame %d planned below QpBdOffsetY at %g", i,
              tp.fr[i].plan_qp);
    n265_2p_free(&tp);
}

/* A degenerate but reachable file: one frame, and a frame whose complexity
 * is the floor n265_rc_cplx clips at. Neither may produce a NaN plan. */
static void test_degenerate(void)
{
    double bits[] = { 80000 };
    double qp[]   = { 28.0 };
    double cplx[] = { 360000 };
    n265_2p tp;
    mk(&tp, 1, bits, qp, cplx);
    CHECK(n265_2p_solve(&tp, 40000.0, 0.6, 6.0, 0.0, 0.0, 51.0) == 0, "one frame");
    CHECK(NEAR(tp.fr[0].d, 0.0, 1e-12), "one frame IS the mean");
    CHECK(NEAR(tp.fr[0].plan_bits, 40000.0, 1e-3), "and takes the target");
    CHECK(tp.fr[0].plan_qp == tp.fr[0].plan_qp, "plan is a number");
    n265_2p_free(&tp);

    double b2[] = { 800, 80000 };
    double q2[] = { 28.0, 28.0 };
    double c2[] = { 1.0, 360000 };      /* the rc_cplx floor */
    mk(&tp, 2, b2, q2, c2);
    CHECK(n265_2p_solve(&tp, 40000.0, 0.6, 6.0, 0.0, 0.0, 51.0) == 0, "floor");
    for (int i = 0; i < tp.n; i++)
        CHECK(tp.fr[i].plan_qp == tp.fr[i].plan_qp &&
              tp.fr[i].plan_bits == tp.fr[i].plan_bits,
              "frame %d plan is a number", i);
    CHECK(NEAR(plan_sum(&tp), 40000.0, 1e-3), "and still sums");
    n265_2p_free(&tp);

    /* a target of zero is not a plan */
    mk(&tp, 1, bits, qp, cplx);
    CHECK(n265_2p_solve(&tp, 0.0, 0.6, 6.0, 0.0, 0.0, 51.0) < 0, "zero target");
    n265_2p_free(&tp);
}

/* Solving twice gives the same numbers. The plan is fixed before coding
 * starts and t1 == t12 rests on that, so a solve that carried state
 * between calls would be the one way this item could lose determinism. */
static void test_idempotent(void)
{
    double bits[] = { 80000, 15000, 5000, 9000, 2400, 300 };
    double qp[]   = { 23.5, 27.0, 30.25, 28.5, 31.0, 33.0 };
    double cplx[] = { 360000, 88000, 72000, 91000, 73000, 4000 };
    n265_2p a, b;
    mk(&a, 6, bits, qp, cplx);
    n265_2p_solve(&a, 6e4, 0.6, 6.0, 0.0, 0.0, 51.0);
    mk(&b, 6, bits, qp, cplx);
    n265_2p_solve(&b, 6e4, 0.6, 6.0, 0.0, 0.0, 51.0);
    n265_2p_solve(&b, 6e4, 0.6, 6.0, 0.0, 0.0, 51.0);    /* and again, in place */
    CHECK(memcmp(&a.rf, &b.rf, sizeof a.rf) == 0,
          "the rate factor is a pure function of the file");
    for (int i = 0; i < a.n; i++)
        CHECK(a.fr[i].plan_qp == b.fr[i].plan_qp &&
              a.fr[i].plan_bits == b.fr[i].plan_bits,
              "frame %d differs on a second solve", i);
    n265_2p_free(&a);
    n265_2p_free(&b);
}

int main(void)
{
    test_parse();
    test_parse_refusals();
    test_sums_to_target();
    test_qcomp_one_is_flat();
    test_reproduces_pass1();
    test_qcomp_direction_and_clamp();
    test_qp_range();
    test_degenerate();
    test_idempotent();
    if (fails)
        printf("twopass: %d failure(s)\n", fails);
    else
        printf("twopass: all checks pass\n");
    return fails ? 1 : 0;
}
