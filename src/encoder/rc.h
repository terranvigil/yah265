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
 * `cls` is one of the RC_* classes, `cplx` its lookahead complexity. */
double n265_rc_next(struct n265_encoder *e, int cls, double cplx);

/* account for the frame just coded */
void n265_rc_done(struct n265_encoder *e, int cls, double cplx, double rf_used,
                  double bits, double planned);

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
