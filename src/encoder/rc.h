/*
 * Single-pass average-bitrate rate control; see rc.c for the model.
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

/* The size model (N265_VBR_SZM, phase 14 item 32). A frame's bits are
 * predicted as
 *
 *   log2(bits / nblk) = a + b log2(cplx / nblk) + c (base / 6)
 *
 * with (a, b, c) fitted online, per frame type and pyramid depth and per
 * content kind (static, simple, complex), from the frames already coded.
 * Each fit is a ridge regression toward its parent's (the frame's RC
 * class), with exponential forgetting; the parents ride toward the
 * class's prior scale. `c` is the quantizer slope, fitted rather than
 * assumed to be -1: on heavy content at low rate the bits halve over 12
 * QP, not 6. */
#define RC_SZK 10                /* I, P, B depth 1-4 x referenced/leaf */
#define RC_SZC 3                 /* static, simple, complex */
#define RC_HIST 192              /* > two groups and twice the largest s */
typedef struct n265_szfit {
    double A[9], b[3];          /* decayed normal equations */
    double n;                   /* observations taken */
    double r2;                  /* decayed mean squared residual, log2 */
} n265_szfit;

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
    int meas;                   /* 0 restores the modeled operating point */
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
     * bits the finer quantizer costs, so the fair share, the TCAP horizon and
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
     * asymmetry a buffer wants - being wrong high costs a quantizer step,
     * being wrong low costs the stream. */
    double vscale[RC_CLASSES];
    double vdecay;              /* N265_VBV_DECAY: the slow side's rate */
    /* The brake (encoder.c, vbv_brake): ABR with a buffer, and capped CRF
     * since item 47. The calibration is the retired frames' coded bits per
     * estimated bit, per class, learned in coding order on the driver. */
    int brake;                  /* N265_VBV_BRAKE, N265_CCRF_BRAKE */
    double ccrf_r0;             /* N265_CCRF_BRK_R0: capped CRF's
                                 * calibration before any class has one */
    int ccrf_skip;              /* N265_CCRF_SKIP: capped CRF's skip */
    double ccrf_skfill;         /* N265_CCRF_SKIP_FILL: the share of the
                                 * fill the skip holds a picture to */
    double ccrf_satq;           /* N265_CCRF_SKIP_SATQ: the QP a saturated
                                 * frame's window is priced at */
    double ccrf_skwarm;         /* N265_CCRF_SKIP_WARM: observations of
                                 * each inter class before it */
    double ccrf_sat_allow;      /* that window's allowance, 0 where the
                                 * clamp was not saturated */
    double ccrf_guard;          /* N265_CCRF_GUARD: capped CRF's margin on
                                 * the frame alone against the fill */
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
     * becomes the quantizer the whole-file solve chose for it instead of
     * the measured operating point. The class scales, the VBV's scales,
     * the cu-tree term coming off the answer and the step bound are all
     * untouched - the second pass moves where the base comes from, not
     * what a base means. */
    int plan_ok;
    double plan_qp, plan_bits;
    double tp_gain;             /* N265_2P_GAIN: how much of the loop's own
                                 * feedback a planned frame still carries */
    /* the size model (above): on under --bitrate with a VBV only */
    int szm;
    double szm_lam;             /* N265_VBR_SZM_LAM: forgetting factor */
    double szm_z;               /* N265_VBR_SZM_Z: the clamp's margin, in
                                 * residual standard deviations */
    double szm_gmin;            /* N265_VBR_SZM_GMIN: the clamp's floor on
                                 * the fitted quantizer slope */
    double szm_zlag;            /* N265_VBR_SZM_ZLAG: the lag walk's */
    double szm_static, szm_simple;  /* content thresholds, cost per block */
    n265_szfit szp[RC_CLASSES];
    n265_szfit szc[RC_SZK][RC_SZC];
    int cur_szk;                /* the dispatching frame's type key */
    /* step 3, N265_VBR_OPEN: the opening solved on a corpus prior */
    int vopen;
    double op_a[RC_CLASSES], op_b[RC_CLASSES], op_g;
    /* constants */
    double qcomp, horizon, maxstep, scale_mem, w_mem;
    double rf_bias;             /* the integral term, in QP units      */
    double lvl_gain;            /* how fast the shared level follows  */
    double igain, imax;         /* integral gain and its bound        */
    double fbmax;               /* bound on both feedback terms       */
    double fbup;                /* and its coarser-side rail          */
    int planerr;                /* P term reads the plan, not the budget */
    /* Phase 14 item 37, VBR only (--bitrate with a VBV). The slow form of
     * the feedback: the cumulative error over a window that grows with the
     * square root of the coded duration, in place of the six-frame horizon
     * and its integral. 0 is the horizon loop. */
    double fps;
    double fbw;                 /* N265_VBR_FBW: window, seconds x sqrt(s) */
    double fbt0;                /* N265_VBR_FBT0: the window's floor on t */
    double fbg, fbgup;          /* gains under and over the target */
    double ovw;                 /* N265_VBR_OVW: the window's cap, as a
                                 * share of the buffer (0: no cap) */
    int ctkeep;                 /* N265_VBR_CTKEEP: the cu-tree frame term
                                 * rides the coded base, as under CRF, and
                                 * the ledger reads each frame at its coded
                                 * base less the term */
    int vown;                   /* N265_VBV_OWN: the clamp prices at the
                                 * allocator's scale (1 window, 2 frame) */
    /* Phase 14 item 38, capped VBR only: where a surplus is spent. */
    double fbspend;             /* N265_VBR_SPEND: exponent on the frame's
                                 * complexity over the running mean that
                                 * scales the finer-side feedback (0: off) */
    double fbfine;              /* N265_VBR_FBFINE: the finer-side rail */
    double fbtrk, fbtrk_w;      /* N265_VBR_FBTRK(_W): deficit where it widens */
    double fbtrk_max;           /* N265_VBR_FBTRK_MAX: how far */
    /* Phase 14 item 50, capped VBR only: a deep deficit takes the recent
     * operating point (rc_next, N265_VBR_DEFPT). */
    double defpt;               /* N265_VBR_DEFPT: deficit where it starts (0: off) */
    double defpt_rel;           /* N265_VBR_DEFPT_REL: deficit where it lets go */
    double defpt_w;             /* N265_VBR_DEFPT_W: span above that to full weight */
    int defpt_on;               /* ...latched: past defpt, until defpt_rel */
    int defpt_rail;             /* N265_VBR_DEFPT_RAIL: the finer rail is
                                 * fbtrk_max while latched */
    double defpt_t0;            /* N265_VBR_DEFPT_T0: seconds coded before it may */
    double defpt_slew;          /* N265_VBR_DEFPT_SLEW: finer-side pace, QP/s (0: none) */
    double defpt_cap;           /* N265_VBR_DEFPT_CAP: finer-side move at most (0: none) */
    double defpt_dq;            /* ...the move the last frame took */
    double defpt_railq;         /* ...and the rail it rode */
    double defpt_hold;          /* N265_VBR_DEFPT_HOLD: seconds after a floor
                                 * frame it stands aside (0: never) */
    double defpt_nf;            /* ...frames since the last floor frame */
    double defpt_spend;         /* N265_VBR_DEFPT_SPEND: exponent on the frame's
                                 * complexity under the mean (0: off) */
    double dlc_avg, dlc_n;      /* ...its running mean of log per-block
                                 * complexity, inter frames */
    double cplxr_s, cplxr_sn;   /* the operating point's short-memory twin */
    double cplxr_smem;          /* N265_VBR_DEFPT_MEM: its memory, frames */
    /* --shot-plan: on under capped VBR, and the frame's base-QP offset,
     * which the encoder adds to this loop's answer */
    int splan;
    double cur_soff;
    double vlvl_a;              /* N265_VBR_VLVL: the clamp level's rate (0.1), 0 off */
    double vlvl;                /* ...the level, in QP */
    double fillq, fillgoal;     /* N265_VBR_FILLQ / _FILLGOAL: the raise at an
                                 * empty buffer (6), and the fill share it starts at (0.5) */
    double spend_lo, spend_hi;  /* N265_VBR_SPENDFILL / _SPENDHI: the fill
                                 * shares between which a surplus is spent
                                 * from none to whole (0: off) */
    double lcx_avg, lcx_n;      /* running mean of log per-block complexity
                                 * over inter frames */
    /* live control, capped VBR only (n265_rc_next): after a RATE step the
     * carried balance is repaid at `ctlpay` of the new rate, and the slow
     * feedback's window and deficit rail count from the step */
    double ctlpay;              /* N265_VBR_CTLPAY: the share (0.02), 0 off */
    int ctlseg;                 /* N265_VBR_CTLSEG: count from the step (1), 0 off */
    int stepped;                /* ...a RATE step has landed */
    double carry;               /* ...the balance at the last step, bits */
    double step_n0;             /* ...the frames counted then */
    double open_step;           /* extra step allowed over the opening */
    int nblk;                   /* lowres 8x8 blocks per frame        */
    int trace;
    /* Live control's large RATE step (contract v1.2, rule (b)): a ring of
     * retired frames in coding order, and the refit procedure's state. */
    struct n265_rc_obs {
        int cls, cut;
        double q, bits;
    } hist[RC_HIST];
    long hist_cnt;              /* frames retired, = the next one's index */
    int ls_on, ls_vbvup;        /* N265_LS, N265_LS_VBVUP */
    int ls_state;               /* 0 idle, 1 refitting, 2 short memory */
    int ls_nref, ls_s, ls_m, ls_mem_n;
    long ls_astart, ls_bmin, ls_mem_end;
    double ls_qa, ls_ba, ls_a;
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
 * deliberate discount the plan has already paid for. `soff` is the shot
 * plan's offset inside it, taken out the same way. */
void n265_rc_done(struct n265_encoder *e, int cls, double cplx, double rf_used,
                  double bits, double planned, double ctf, double ioff,
                  double soff);

/* a frame retired, after its n265_rc_done, with n265_rc_done's `rf_used`
 * and `ctf`; `cut` is whether the lookahead cut there */
void n265_rc_note(struct n265_encoder *e, int cls, double rf_used,
                  double ctf, double bits, int cut);

/* Live control's RATE retarget, after the ledger has been scaled: a step
 * with r < 1/3 or r > 3 moves the operating point by 6 log2(1/r) / 1.5
 * and starts the refit (contract v1.2, rule (b)), unless `take` is 0;
 * any other step ends a procedure under way. Returns the QP the
 * operating point moved by. */
double n265_rc_rate_step(struct n265_encoder *e, double r, int take);

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

/* the size model's key for a frame type and pyramid depth */
int n265_szm_key(int slice_type, int depth, int nonref);
/* its predicted bits for a frame of class `cls` and key `key` (-1: the
 * class fit alone) at base `q`; *slope is d log2(bits) / d(q/6) and *sd
 * the fit's residual standard deviation in log2, either may be NULL */
double n265_szm_pred(const n265_rc *rc, int cls, int key, double cplx,
                     double q, double *slope, double *sd);
/* one coded frame */
void n265_szm_add(n265_rc *rc, int cls, int key, double cplx, double q,
                  double bits);

void n265_rc_warm(void);

#endif
