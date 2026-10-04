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
    /* The INTRA QP offset the allocator plans for (N265_ABR_IOFF, phase 10
     * item 2). An intra frame is coded `ioff` QP FINER than the rate factor
     * the loop would otherwise hand it, and the plan is written knowing it:
     * the frame's weight is multiplied by `2^(ioff/6)`, which is exactly the
     * bits the finer quantiser costs, so the fair share, the TCAP horizon and
     * the measured operating point all price the frame at what it will really
     * spend. That is the difference between this and the cu-tree frame term
     * it replaces - the term moved the control variable behind the loop's
     * back, and every QP of it was open-loop error.
     *
     * `ioff_ct` is the second arm: the offset derived from the frame's own
     * cu-tree term instead of being a constant, since the term's whole claim
     * was that it knows which frames are worth referencing. `ioff_max` caps
     * both. */
    double ioff, ioff_ct, ioff_max;
    double last_ioff;           /* the offset inside the last answer */
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
    /* N265_VBR_LAG_PEND: the frames inside the lag counted at their
     * calibrated predicted sizes (encoder.c, vbv_plan_fill's walk) and not
     * at their plans. Set by the encoder before each solve; on wherever
     * the lag is, under a VBV. */
    int pend_use_est;
    double pend_est;
    double last_planned;        /* the T the last rc_next handed out */
    /* The VBV's three columns for the last answer (vbv.h): the model's
     * predicted bits at the base the frame was about to be coded at, the
     * bits the buffer could pay for, and the QP the clamp added. All zero
     * with --vbv-maxrate unset, which is every encode the mode is off in. */
    double last_vbv_pred, last_vbv_allow, last_vbv_dqp;
    /* The VBV's OWN per-class scale, and the whole reason it is separate
     * from `scale` above.
     *
     * `scale` is an ALLOCATOR's estimate and it is corrected by the shared
     * LEVEL on every coded frame, whatever that frame's class - which is
     * right for allocation, because the level is what the priors get most
     * wrong and one frame is evidence about it. It is wrong for a buffer.
     * sita_flat_720p codes 38 near-free leaf B frames between two scene-cut
     * IDRs, and they drag scale[RC_I] from 30.3 to 10.6; the second IDR is
     * then predicted at a third of what it costs, the clamp does not fire,
     * and 627 kbit lands in a 282 kbit buffer.
     *
     * So this one is per class and per class ONLY, and it attacks fast and
     * decays slowly: an expensive frame raises its class's scale at once,
     * and cheap frames of OTHER classes cannot lower it at all. That is the
     * asymmetry a buffer wants - being wrong high costs a quantiser step,
     * being wrong low costs the stream. */
    double vscale[RC_CLASSES];
    double vdecay;              /* N265_VBV_DECAY: the slow side's rate */
    /* The brake (encoder.c, vbv_brake): ABR with a buffer only. The
     * calibration is the retired frames' coded bits per estimated bit,
     * per class, learned in coding order on the driver. */
    int brake;                  /* N265_VBV_BRAKE */
    double flatc;               /* N265_ABR_FLATC: a frame whose lookahead
                                 * complexity per lowres block is under this
                                 * is not evidence about the operating point
                                 * (ABR with a buffer only) */
    double brk_fill;            /* N265_VBV_BRAKE_FILL: the share of the
                                 * buffer's fill the brake holds a picture
                                 * to - a hard limit, not the clamp's plan */
    double brk_ratio[RC_CLASSES];
    double brk_gain;            /* N265_VBV_BRAKE_K0: prior weight on the
                                 * plan in the projection, as a share */
    double scpred;              /* N265_VBV_SCPRED: an inter frame whose
                                 * complexity is at least this share of its
                                 * intra cost is priced as intra, for the
                                 * buffer, under ABR */
    double cur_icplx;           /* the dispatching frame's intra cost, for
                                 * the intra-dominated test below */
    /* --pass 2's plan for the frame being dispatched (twopass.h), stashed
     * by the caller the way cur_icplx is rather than widening this
     * function's argument list for a mode most encodes do not run.
     *
     * `plan_ok` 0 IS the one-pass loop, to the bit: every branch below
     * that reads these is guarded on it, and it is 0 on every frame of
     * every encode that did not ask for a second pass - and on a frame in
     * a pass-2 encode that pass 1 never described, which falls back rather
     * than inventing a plan.
     *
     * What the plan replaces is exactly two things: the fair share `T`,
     * which becomes the frame's own planned bits, and the base, which
     * becomes the quantiser the whole-file solve chose for it instead of
     * the measured operating point. The class scales, the VBV's scales,
     * the cu-tree term coming off the answer and the step bound are all
     * untouched - the second pass moves where the base comes from, not
     * what a base means. */
    int plan_ok;
    double plan_qp, plan_bits;
    double tp_gain;             /* N265_2P_GAIN: how much of the loop's own
                                 * feedback a planned frame still carries */
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
 * `ctf` the cu-tree frame term inside that base, and `ioff` the planned intra
 * offset inside it - the one term the operating point has to take back OUT,
 * because the point is the mean of `bits x qscale(base)` and the offset is a
 * deliberate discount the plan has already paid for. */
void n265_rc_done(struct n265_encoder *e, int cls, double cplx, double rf_used,
                  double bits, double planned, double ctf, double ioff);

/* the class of a frame, from the fields encode_frame already carries */
int n265_rc_class(int slice_type, int nonref);

/* the lookahead complexity of queued frame `i` for class `cls` */
double n265_rc_cplx(struct n265_encoder *e, int i, int cls);

/* Capped CRF's half of the VBV clamp: the base the frame may be coded at,
 * given the base the CRF path assembled. Never finer than what it was
 * given. `pred`, `allow` and `dqp` are the trace's columns. Under ABR the
 * same clamp lives inside n265_rc_next, where the loop's own answer is. */
double n265_vbv_crf_base(struct n265_encoder *e, int cls, double cplx,
                         double icplx, double base, double *pred,
                         double *allow, double *dqp);

/* Will queued frame `i` be coded as an IDR? The keyint and scene-cut half
 * of encode_frame's own test, without the "no reference yet" clause, which
 * is about the frame being dispatched and not about the ones behind it.
 * Defined in encoder.c beside the test it mirrors; the rate control reads
 * it to see the intra frames coming before it spends against them. */
int n265_rc_future_idr(struct n265_encoder *e, int i);

#endif
