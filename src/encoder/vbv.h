/*
 * The hypothetical decoder buffer (Annex C), as a leaky bucket the encoder
 * plans against. Pure arithmetic: no encoder, no environment, no pixels -
 * which is what lets tests/unit/vbv_test.c check the fill algebra outright
 * and scripts/vbv_check.py reproduce it from the trace alone.
 * Copyright (c) 2026, the yah265 authors. GPL-2.0-or-later; see LICENSE.
 *
 * The model is C.2.2's type II CPB with one scheduler, written the way an
 * encoder uses it. Bits arrive at `maxrate` and the buffer holds at most
 * `bufsize`; at each access unit's removal time the whole coded picture
 * leaves at once. So the state that matters is the fill AT A REMOVAL:
 *
 *     fill(0)     = init * bufsize
 *     fill(k + 1) = min(fill(k) - bits(k) + maxrate / fps, bufsize)
 *
 * and the stream conforms exactly when `fill(k) - bits(k) >= 0` for every
 * k. The cap is the `cbr_flag = 0` half of E.2.3: arrival STOPS when the
 * buffer is full rather than forcing filler data in, which is what makes
 * this a one-sided constraint an encoder can meet by raising a quantiser.
 *
 * The fill is deliberately NOT clamped at zero from below. An underflow is
 * a fact about the stream, not a state the encoder may forget: carrying the
 * deficit is what keeps the next frames clamped until the buffer has really
 * recovered, and it is what makes the encoder's own number agree with the
 * checker's, which re-derives the whole trajectory from the coded sizes.
 */

#ifndef N265_VBV_H
#define N265_VBV_H

typedef struct n265_vbv {
    int on;
    double maxrate;         /* bits/s: the CPB arrival rate (BitRate)     */
    double bufsize;         /* bits: the CPB size (CpbSize)               */
    double init;            /* --vbv-init, as a fraction of bufsize       */
    double frame_dur;       /* seconds between two removals               */
    double inflow;          /* maxrate * frame_dur, bits per frame period */
    double safe;            /* the share of the strictly available bits a
                             * PLAN may claim (N265_VBV_SAFE). A plan is an
                             * estimate and the coded frame is the truth,
                             * so the gap is the only thing standing
                             * between a model error and an underflow. */
    int lookahead;          /* reserve for the window's own plans too     */
    double fill;            /* bits in the CPB at the next removal        */
    double min_fill;        /* the least post-removal fill seen           */
    long underflows;        /* removals that took the fill below zero     */
    long clamped;           /* frames whose plan the clamp cut            */
} n265_vbv;

/* The floor under an allowance. A coded picture is never free - a slice
 * header alone is tens of bits - so an allowance of zero is not a budget,
 * it is a division by zero in whatever converts bits back to a quantiser.
 * The QP clip is what actually bounds the answer; this only keeps the
 * logarithm finite. */
#define N265_VBV_MIN_ALLOW 1.0

void n265_vbv_init(n265_vbv *v, double maxrate, double bufsize, double init,
                   double fps, double safe, int lookahead);

/* The most bits the frame at the next removal may spend.
 *
 * `future` holds the PLANNED bits of the frames queued behind it, in the
 * order they will be removed, and `n` how many of them are worth believing.
 * The constraint is cumulative - over the k removals after this one the CPB
 * can deliver `fill + k * inflow` bits in total - so the answer is the
 * tightest of those k bounds, which at k = 0 is the fill itself and beyond
 * it is the reservation an expensive frame in the window makes for itself.
 * `n` of 0 (or `lookahead` off) leaves the k = 0 bound alone. */
double n265_vbv_allow(const n265_vbv *v, const double *future, int n);

/* The frame was coded at `bits`: remove it, then let one frame period of
 * arrival in. `fill_before` and `fill_after` are the trace's two columns -
 * the fill at this removal and the fill at the next one. */
void n265_vbv_update(n265_vbv *v, double bits, double *fill_before,
                     double *fill_after);

#endif
