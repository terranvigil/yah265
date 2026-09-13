/*
 * Single-pass average-bitrate rate control (docs/programme-from-yah264.md
 * step A.1). Re-derived here; see rc.c for the model.
 * Copyright (c) 2026, the yah265 authors. GPL-2.0-or-later; see LICENSE.
 */

#ifndef N265_RC_H
#define N265_RC_H

struct n265_encoder;

/* frame classes the per-type complexity scales are learned per */
enum {
    RC_I = 0,
    RC_P = 1,
    RC_BREF = 2,        /* referenced B (pyramid depth >= 1) */
    RC_B = 3,           /* non-referenced B (leaf) */
    RC_CLASSES = 4
};

typedef struct n265_rc {
    double bits_per_frame;      /* the ABR budget, bits per coded frame  */
    double rf;                  /* the rate factor, in QP units          */
    int have_rf;                /* the opening fit has run               */
    double scale[RC_CLASSES];   /* bits x qscale per unit complexity^qcomp */
    double scale_n[RC_CLASSES]; /* observation weight behind each scale  */
    double w_avg;               /* running mean of the per-frame weight  */
    double w_n;
    /* The MEASURED operating point: a running mean over EVERY coded frame
     * of `bits x qscale(base rate factor)`. Same product the class scales
     * are learned from, summed over frames instead of averaged as a
     * per-frame ratio, and with the intra frames in it. See rc.c. */
    double cplxr;
    double cplxr_n;
    double cplxr_mem;
    double cplxr_seed;
    int copen;                  /* the forced opening IDR is not evidence */
    double copen_bits;          /* ...until a second intra frame arrives  */
    int copen_held;
    int meas;                   /* 0 restores the modelled operating point */
    double mguard;              /* QP of model-vs-measurement slack     */
    int tcap;                   /* horizon floored at the frame's own plan */
    int ibrake;                 /* an intra frame's feedback may not spend */
    int fbstep;                 /* the step bound rides the feedback, not rf */
    int openi;                  /* the opening solve carries its intra share */
    /* The cu-tree FRAME term (encoder.h, n265_ct_frame_term) lands on the base
     * AFTER this loop has answered, so a loop that has not been told it is
     * controlling a number the encoder does not code at. At the default the
     * term comes off the rate factor whole and the frame is coded at the base
     * this loop solved; the measured LEVEL below is what the kept arms are
     * written in. See n265_rc_next.
     *
     * `ctf_x` is the running mean of `bits x qscale(base) x 2^(-term/6)` and
     * `ctf_c` the same mean of `bits x qscale(base)` alone, over the frames
     * cplxr's mean is over, so `-6 log2(ctf_x / ctf_c)` is the level weighted
     * by each frame's own share of the file - the identity that makes the
     * spread cost no bits, rather than a mean over frames that a leaf costing
     * nothing votes in. They carry their OWN observation count, which opens at
     * one frame and not at the window's twenty: the level is the one quantity
     * here that the first coded frame really does know better than the fit. */
    double ctf_x, ctf_c;
    double copen_x, copen_c;    /* the held opening IDR's halves of them */
    double ctf_avg;             /* the frame-COUNT mean: the refused arm */
    double ctf_lvl_n;           /* its own count */
    double ctf_n;               /* observation weight behind ctf_x / ctf_c */
    int ctf_comp;               /* 1 = the term off the base (ships); 0 = the
                                   pre-fix loop, 2 = its level only, 3 = that
                                   level as a frame-count mean */
    double fb_prev;             /* the feedback the last frame was given  */
    double total_bits;          /* bits emitted so far                   */
    double planned_bits;        /* bits the allocator planned for them   */
    long n_coded;
    /* D4's observation lag. A frame priced by n265_rc_next but whose
     * n265_rc_done has not arrived yet contributes its PLANNED bits -
     * the fair share the allocator handed it - to both feedback terms,
     * instead of the actual bits nobody knows yet. At N265_RC_LAG = 0
     * these are always zero, because rc_done runs before the next
     * rc_next, and the loop is bit-identical to the pre-T2 one. */
    double pend_bits;
    long pend_n;
    double last_planned;        /* the T the last rc_next handed out */
    /* constants (docs/instruments.md) */
    double qcomp, horizon, maxstep, scale_mem, w_mem;
    double rf_bias;             /* the integral term, in QP units      */
    double lvl_gain;            /* how fast the shared level follows  */
    double igain, imax;         /* integral gain and its bound        */
    double fbmax;               /* bound on both feedback terms       */
    double fbup;                /* and its coarser-side rail          */
    int planerr;                /* P term reads the plan, not the budget */
    double open_step;           /* extra step allowed over the opening */
    int nblk;                   /* lowres 8x8 blocks per frame        */
    int trace;
} n265_rc;

/* configure from the encoder's params (called once, at open) */
void n265_rc_open(struct n265_encoder *e);

/* the base rate factor for the frame about to be coded, in QP units.
 * `cls` is one of the RC_* classes, `cplx` its lookahead complexity, and
 * `ctf` the cu-tree frame term the caller will add to the base as soon as
 * this returns - the loop answers in the units the frame is actually coded
 * in, which is what makes the answer a control variable at all. */
double n265_rc_next(struct n265_encoder *e, int cls, double cplx, double ctf);

/* account for the frame just coded. `rf_used` is the base it was CODED at,
 * `ctf` the cu-tree frame term inside that base. */
void n265_rc_done(struct n265_encoder *e, int cls, double cplx, double rf_used,
                  double bits, double planned, double ctf);

/* the class of a frame, from the fields encode_frame already carries */
int n265_rc_class(int slice_type, int nonref);

/* the lookahead complexity of queued frame `i` for class `cls` */
double n265_rc_cplx(struct n265_encoder *e, int i, int cls);

/* Will queued frame `i` be coded as an IDR? The keyint and scene-cut half
 * of encode_frame's own test, without the "no reference yet" clause, which
 * is about the frame being dispatched and not about the ones behind it.
 * Defined in encoder.c beside the test it mirrors; the rate control reads
 * it to see the intra frames coming before it spends against them. */
int n265_rc_future_idr(struct n265_encoder *e, int i);

#endif
