/*
 * Two-pass rate control: the stats sidecar and the whole-file rate-factor
 * solve (docs/phase12.md, Item 4).
 *
 * Pure arithmetic and one text format: no encoder, no environment, no
 * pixels - the same split vbv.c takes, and for the same reason. It lets
 * tests/unit/twopass_test.c check the parser and the solve outright,
 * without an encode, and it keeps the one thing this item has to get
 * exactly right - the plan, which is fixed before a frame is coded and is
 * therefore the whole of the determinism claim - in a file a reader can
 * hold in their head.
 *
 * Copyright (c) 2026, the yah265 authors. GPL-2.0-or-later; see LICENSE.
 *
 * THE MODEL
 *
 * Pass 1 is an ordinary encode that writes down, per frame, what it cost
 * and what quantiser it cost that at. Pass 2 reads those pairs and plans
 * the whole file before coding anything.
 *
 * Each frame gets its own rate curve, ANCHORED ON THE MEASUREMENT rather
 * than on the class priors:
 *
 *     X_i        = bits_i * qscale(qp_i)          (bits x qscale)
 *     bits_i(q)  = X_i / qscale(q)
 *
 * which reproduces pass 1 exactly at q = qp_i and is the same invariant
 * the one-pass loop's measured operating point is a running mean of
 * (rc.c, `cplxr`). It is measured per frame instead of averaged, which is
 * the only thing a second pass really buys: the class scale, the shared
 * level and the opening fit are all estimators of a number pass 1 already
 * knows for each frame separately.
 *
 * On top of that curve sits the ONE modelled term, and it is x264's
 * qcomp: a frame harder than the file's own geometric mean is coded at a
 * COARSER quantiser, by
 *
 *     d_i = 6 * (1 - qcomp) * log2( (cplx_i / nblk) / gmean )
 *
 * clamped to N265_2P_CLAMP. This is the term that makes a second pass an
 * ALLOCATOR and not just a relabelling. Today's one-pass loop has no such
 * term at all, which is easy to miss: `w / T` is `w_avg / bits_per_frame`
 * identically (rc.c), so the frame's complexity cancels and every frame
 * rides the same base - qcomp reaches the one-pass base nowhere. At
 * N265_2P_QCOMP = 1 every d_i is zero and pass 2 is exactly "re-level the
 * file", which is the arm the qcomp allocation is measured against.
 *
 * The file then has one free parameter, the rate factor, and it is CLOSED
 * FORM because qscale(rf) factors straight out:
 *
 *     sum_i X_i / (qscale(rf) * 2^(d_i/6)) = target
 *     qscale(rf) = ( sum_i X_i * 2^(-d_i/6) ) / target
 *
 * And then the one thing the corpus forced: a TRUST RADIUS. The rate
 * curve is a measurement extrapolated by 1/qscale, and far from the
 * anchor that extrapolation is not merely inaccurate, it is the wrong
 * shape - akiyo_cif codes display 8 at QP 28.7 for 720 bits and the same
 * frame at QP 20.3 costs 12 328, seventeen times where the model says
 * 2.65. So the ALLOCATION may move a frame at most N265_2P_TRUST QP from
 * where pass 1 measured it, and the LEVEL is then solved on top of that,
 * unbounded, in the same closed form (`qscale(q + u)` is
 * `qscale(q) 2^(u/6)`, so `u = 6 log2(plan / target)` lands the file
 * exactly). The radius bounds the re-allocation; it does not bound
 * hitting the target.
 *
 * No bisection, no iteration to a tolerance, no floating-point search
 * whose stopping rule would have to be part of the determinism claim.
 * The only iteration is over frames the QP RANGE pins (a plan below
 * QpBdOffsetY or above 51 is not a plan): those are fixed at the bound,
 * their bits come out of the target, and the rest are re-levelled around
 * them. It is a finite walk - a frame never unpins - and 32 bounds it.
 */

#ifndef N265_TWOPASS_H
#define N265_TWOPASS_H

#include <math.h>
#include <stdio.h>

/* The format version written into the header line and required on read.
 * A stats file is an encoder artefact and not an interchange format: two
 * different builds' numbers must not be mixed, and the version is what
 * says so out loud instead of producing a plausible wrong plan. */
#define N265_2P_VER 1

/* the classic quantiser scale, the axis rc.c and this file share */
static inline double n265_2p_qscale(double q)
{
    return 0.85 * pow(2.0, (q - 12.0) / 6.0);
}

static inline double n265_2p_qp(double qs)
{
    if (!(qs > 0.0))
        return -1000.0;     /* the caller clips; this only keeps log2 finite */
    return 12.0 + 6.0 * log2(qs / 0.85);
}

typedef struct n265_2p_frame {
    /* what pass 1 wrote down */
    int coding, display;
    int slice_type;         /* 2 = I, 1 = P, 0 = B (fw->slice_type)  */
    int nonref;
    int flat;
    double qp;              /* the BASE it was coded at (fw->rc_base) */
    double bits;
    double cplx;            /* n265_rc_cplx: the SATD-domain complexity
                             * the RC loop models the frame with          */
    double icost, pcost;    /* the lookahead's own sums at dispatch       */
    double prop;            /* ...and its cu-tree propagate accumulator   */
    double ctf;             /* the cu-tree frame term inside `qp`         */
    /* what the solve wrote */
    double x;               /* bits * qscale(qp): the frame's rate curve  */
    double d;               /* its qcomp offset from the rate factor, QP  */
    double plan_qp;         /* the base pass 2 plans to code it at        */
    double plan_bits;       /* and what that base is expected to cost     */
    int pinned;             /* the QP range, not the solve, set plan_qp   */
} n265_2p_frame;

typedef struct n265_2p {
    int pass;               /* 0 off, 1 write, 2 read, 3 read AND rewrite */
    FILE *out;              /* the sink, for pass 1 and pass 3            */
    char *out_path;         /* pass 3 renames onto this at close          */
    char *out_tmp;
    n265_2p_frame *fr;
    int n, cap;
    int *disp;              /* display index -> entry, -1 where absent    */
    int ndisp;
    /* the geometry the stats were written at, so pass 2 can refuse a file
     * that describes a different encode rather than plan from it */
    int nblk, width, height;
    int solved;
    double rf;              /* the solved rate factor, QP units           */
    double qcomp, clamp, trust;
    double target_bits;     /* what the plan was solved onto              */
    double plan_bits;       /* what the plan actually sums to             */
    long miss;              /* frames coded that the stats do not describe */
} n265_2p;

void n265_2p_init(n265_2p *tp);
void n265_2p_free(n265_2p *tp);

/* Open the sidecar for writing and put the header down. `rewrite` is pass
 * 3: the bytes go to a temporary and land on `path` only at close, so a
 * pass that reads and writes the same file cannot leave a half-written
 * plan behind a crash. Returns 0, or -1 with the reason on stderr. */
int n265_2p_open_write(n265_2p *tp, const char *path, int rewrite,
                       int width, int height, int fps_num, int fps_den,
                       int nblk, const char *ver);
void n265_2p_write(n265_2p *tp, const n265_2p_frame *f);
void n265_2p_close_write(n265_2p *tp);

/* Read a sidecar. Returns the frame count, or -1 with a reason in `err`. */
int n265_2p_read(n265_2p *tp, const char *path, char *err, size_t errn);

/* The solve above, over every frame read. Returns 0, or -1 on a file with
 * no frames or a target that is not a number of bits. */
int n265_2p_solve(n265_2p *tp, double target_bits, double qcomp,
                  double clamp, double trust, double qp_min, double qp_max);

/* The plan for the frame at display index `d`, or NULL: a pass-2 encode
 * that reaches a frame pass 1 never described falls back to the one-pass
 * loop for it, and says so at close. */
const n265_2p_frame *n265_2p_get(const n265_2p *tp, int d);

#endif
