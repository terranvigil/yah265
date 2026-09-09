/*
 * Single-pass average-bitrate rate control.
 *
 * The control variable is one rate factor in QP units, the same axis CRF
 * rides on, and every per-frame term the CRF path already applies (the
 * pyramid cascade, the AQ frame-mean, the per-CTU AQ and cu-tree fields)
 * lands on top of it unchanged. So ABR is CRF with the base solved from
 * bits instead of given.
 *
 * The model, re-derived (docs/programme-from-yah264.md A.1):
 *
 *   qscale(q)   = 0.85 * 2^((q - 12) / 6)     the classic quantiser scale
 *   w(frame)    = scale[class] * nblk * (complexity / nblk)^qcomp
 *   bits(frame) = w / qscale(rate factor)
 *
 * `complexity` is the lookahead's own cost sum for the frame (intra cost
 * for an I frame, the lesser of inter and intra for the rest), compressed
 * by qcomp so a hard frame gets more bits but not proportionally more.
 * `scale[class]` prices P, referenced B and leaf B against I and is
 * LEARNED ONLINE from what each coded frame actually spent, which is what
 * lets the same constants work on content the priors were never fitted to.
 * Because the observation is taken against the BASE rate factor, the class
 * scale absorbs everything the encoder does between the base and the
 * frame's real QP - the cascade, the AQ frame mean, the un-centred cu-tree
 * field - and none of those has to be modelled twice.
 *
 * The allocator gives frame i its fair share of the budget,
 * T = bits_per_frame * w_i / mean(w), which makes the rate factor
 * mean(w)/bits_per_frame: constant quality inside the budget, with the
 * complexity model deciding only how the budget is split. On top of that
 * sits one integral correction on the cumulative error, spread over a
 * horizon of `horizon` frames, which is what actually lands the file on
 * its target size.
 *
 * The opening is fitted from the lookahead window rather than guessed: at
 * the first frame the window's complexities are typed by the coding
 * cadence, priced with the prior scales, and the rate factor is solved so
 * the window as a whole lands on budget. Without it the first frames run
 * at whatever the prior says and the integrator spends the rest of the
 * clip undoing it.
 *
 * Copyright (c) 2026, the yah265 authors. GPL-2.0-or-later; see LICENSE.
 */

#include <math.h>
#include <stdio.h>
#include <stdlib.h>

#include "encoder/encoder.h"
#include "encoder/rc.h"

static double qscale_of_qp(double q)
{
    return 0.85 * pow(2.0, (q - 12.0) / 6.0);
}

static double qp_of_qscale(double qs)
{
    if (qs <= 0.0)
        return 0.0;
    return 12.0 + 6.0 * log2(qs / 0.85);
}

/* one tunable, read once at open. The name is spelled literally at every
 * call site so scripts/knob_census.py can see the knob and its default. */
static double abr_tunable(const char *name, double def)
{
    const char *v = getenv(name);
    return v ? atof(v) : def;
}

int n265_rc_class(int slice_type, int nonref)
{
    if (slice_type == 2)
        return RC_I;
    if (slice_type == 1)
        return RC_P;
    return nonref ? RC_B : RC_BREF;
}

double n265_rc_cplx(struct n265_encoder *e, int i, int cls)
{
    const la_frame *f = &e->la.f[i];
    double ic = (double)f->icost_sum;
    double pc = (double)f->pcost_sum;
    if (cls == RC_I)
        return ic > 1.0 ? ic : 1.0;
    /* an inter frame whose motion search does worse than intra will be
     * coded largely as intra, so intra caps it */
    double c = pc < ic ? pc : ic;
    return c > 1.0 ? c : 1.0;
}

void n265_rc_open(struct n265_encoder *e)
{
    n265_rc *rc = &e->rc;
    double fps = e->param.fps_den > 0
                     ? (double)e->param.fps_num / (double)e->param.fps_den
                     : 25.0;
    if (fps <= 0.0)
        fps = 25.0;
    rc->bits_per_frame = e->abr_bitrate / fps;
    rc->qcomp = abr_tunable("N265_ABR_QCOMP", 0.6);
    rc->horizon = abr_tunable("N265_ABR_HORIZON", 6.0);
    if (rc->horizon < 1.0)
        rc->horizon = 1.0;
    rc->maxstep = abr_tunable("N265_ABR_STEP", 1.0);
    rc->scale_mem = abr_tunable("N265_ABR_SMEM", 16.0);
    rc->open_step = abr_tunable("N265_ABR_OPENSTEP", 6.0);
    rc->igain = abr_tunable("N265_ABR_IGAIN", 0.085);
    rc->imax = abr_tunable("N265_ABR_IMAX", 8.0);
    rc->fbmax = abr_tunable("N265_ABR_FBMAX", 6.0);
    rc->w_mem = abr_tunable("N265_ABR_WMEM", 120.0);
    /* Prior per-class scales: bits x qscale per lowres block per
     * (per-block complexity)^qcomp. Geometric means over 11 corpus clips x
     * QP 24/30/36 x 40 frames, read off N265_RC_TRACE on CQP encodes
     * (docs/programme-from-yah264.md, the ABR round). They seed the opening
     * only: the first coded frame of a clip replaces the shared level and
     * the first few of each class replace the rest. The RATIOS are what
     * they mostly carry - 1.91 / 1 / 0.30 / 0.13 - and those are far
     * steadier across the corpus than the level is. */
    rc->scale[RC_I] = abr_tunable("N265_ABR_SI", 25.4);
    rc->scale[RC_P] = abr_tunable("N265_ABR_SP", 13.3);
    rc->scale[RC_BREF] = abr_tunable("N265_ABR_SBR", 4.0);
    rc->scale[RC_B] = abr_tunable("N265_ABR_SB", 1.7);
    rc->lvl_gain = abr_tunable("N265_ABR_LVL", 2.0);
    rc->nblk = e->la.bw * e->la.bh;
    if (rc->nblk < 1)
        rc->nblk = 1;
    for (int i = 0; i < RC_CLASSES; i++)
        rc->scale_n[i] = 2.0;       /* prior weight, in frames */
    rc->trace = getenv("N265_RC_TRACE") != NULL;
    rc->rf = 26.0;
}

/* the predicted bits x qscale of a frame. The complexity is normalised per
 * lowres block before it is compressed, so `scale` is a per-block constant
 * and the priors carry across resolutions; without that the same content at
 * 720p reads five times the CIF scale purely from the block count. */
static double rc_weight(n265_rc *rc, int cls, double cplx)
{
    return rc->scale[cls] * rc->nblk * pow(cplx / rc->nblk, rc->qcomp);
}

/* the class the coding cadence will give queued frame `i`, for the opening
 * fit only (the typing walk has not run on the window yet) */
static int cadence_class(struct n265_encoder *e, int i)
{
    int bf = e->bframes;
    if (bf <= 0)
        return RC_P;
    int m = i % (bf + 1);
    if (m == 0)
        return RC_P;                /* the anchor of the mini-GOP */
    return m == (bf + 1) / 2 ? RC_BREF : RC_B;
}

double n265_rc_next(struct n265_encoder *e, int cls, double cplx)
{
    n265_rc *rc = &e->rc;

    if (!rc->have_rf) {
        /* The lookahead-fitted opening: price the queued window at the prior
         * scales and solve one rate factor onto its budget.
         *
         * Fit from the window's INTER frames: they are the steady state the
         * budget has to average out to. An IDR in the window is not - it
         * recurs every keyint frames, not every window - and pricing the
         * window as if it did would set a rate factor several QP too coarse
         * for the whole clip. The opening I frame rides the fitted factor
         * and its excess is what the integral term is for. */
        double W = 0.0;
        int n = 0;
        for (int i = 0; i < e->la.count; i++) {
            int k = i == 0 ? cls : cadence_class(e, i);
            if (k == RC_I)
                continue;
            W += rc_weight(rc, k, n265_rc_cplx(e, i, k));
            n++;
        }
        if (n == 0) {       /* window of one, or all-intra */
            W = rc_weight(rc, cls, cplx);
            n = 1;
        }
        if (n > 0 && rc->bits_per_frame > 0.0) {
            rc->w_avg = W / n;
            rc->w_n = n;
            double rf = qp_of_qscale(W / (rc->bits_per_frame * n));
            rc->rf = rf < 0.0 ? 0.0 : rf > 51.0 ? 51.0 : rf;
        }
        rc->have_rf = 1;
        if (rc->trace)
            fprintf(stderr, "RC open window=%d bpf=%.0f rf0=%.3f\n",
                    n, rc->bits_per_frame, rc->rf);
    }

    double w = rc_weight(rc, cls, cplx);
    /* The running mean of the frame weight is taken over INTER frames only.
     * It is the yardstick every frame's fair share is measured against, and
     * an I frame is worth several ordinary frames: folding one into the mean
     * would raise the yardstick for the whole GOP that follows it and hand
     * the I frame a share smaller than a P frame's. An IDR recurs every
     * keyint frames; the mean has to describe the other 249. */
    if (cls != RC_I || rc->w_n < 1.0) {
        rc->w_avg += (w - rc->w_avg) / (rc->w_n + 1.0);
        if (rc->w_n < rc->w_mem)
            rc->w_n += 1.0;
    }
    double w_avg = rc->w_avg > 0.0 ? rc->w_avg : w;

    double T = rc->bits_per_frame * w / w_avg;      /* the fair share */

    /* the feedback: the cumulative error against the flat budget */
    double err = rc->total_bits - rc->bits_per_frame * (double)rc->n_coded;
    double corr = 1.0 + err / (rc->bits_per_frame * rc->horizon);
    if (corr < 0.5)
        corr = 0.5;
    else if (corr > 2.0)
        corr = 2.0;
    /* The proportional term above settles wherever the accumulated error is
     * large enough to cancel the bits model's own bias - a standing offset,
     * and the file misses its size by exactly that. The integral term
     * removes it: it moves only while the error is non-zero, so the loop's
     * only fixed point is a cumulative error of zero, which is the size
     * landing on the target. The proportional term is what damps it; an
     * integrator this slow oscillates on its own. */
    rc->rf_bias += rc->igain * 6.0 * log2(corr);
    if (rc->rf_bias > rc->imax)
        rc->rf_bias = rc->imax;
    else if (rc->rf_bias < -rc->imax)
        rc->rf_bias = -rc->imax;

    /* Both feedback terms together are bounded, because a stretch the model
     * is simply wrong about can otherwise drag the quantiser anywhere: the
     * black frames that open sintel_720p cost 200 bits at any QP, so the
     * loop reads eleven frames of unspent budget and walks the rate factor
     * down twenty steps chasing bits that do not exist. The bound is what
     * keeps the recovery from being worse than the deficit. */
    double fb = 6.0 * log2(corr) + rc->rf_bias;
    if (fb > rc->fbmax)
        fb = rc->fbmax;
    else if (fb < -rc->fbmax)
        fb = -rc->fbmax;

    double rf = qp_of_qscale(w / T) + fb;
    /* The per-frame step is bounded so the rate factor cannot chase a single
     * odd frame, but the bound is relaxed over the opening: the priors set
     * only the order of magnitude and the first few coded frames replace
     * them, so the loop has to be allowed to move while that happens. */
    double step = rc->maxstep + rc->open_step / ((double)rc->n_coded + 1.0);
    if (rf > rc->rf + step)
        rf = rc->rf + step;
    else if (rf < rc->rf - step)
        rf = rc->rf - step;
    if (rf < 0.0)
        rf = 0.0;
    else if (rf > 51.0)
        rf = 51.0;
    rc->rf = rf;
    rc->planned_bits += T;
    if (rc->trace)
        fprintf(stderr, "RC n=%ld cls=%d cplx=%.0f w=%.3g wavg=%.3g "
                "T=%.0f corr=%.4f bias=%+.3f fb=%+.3f rf=%.3f err=%.0f\n",
                rc->n_coded, cls, cplx, w, rc->w_avg, T, corr, rc->rf_bias,
                fb, rf, err);
    return rf;
}

void n265_rc_done(struct n265_encoder *e, int cls, double cplx, double rf_used,
                  double bits)
{
    n265_rc *rc = &e->rc;
    double obs = bits * qscale_of_qp(rf_used) /
                 (rc->nblk * pow(cplx / rc->nblk, rc->qcomp));
    if (obs > 0.0) {
        /* One coded frame is evidence about every class, not only its own:
         * what the priors get wrongest is the shared LEVEL (content and
         * resolution), and the ratios between classes are far steadier than
         * the level is. So the level correction goes on all four scales,
         * with a gain that decays as real per-class observations arrive,
         * and the per-class term keeps only the residual. */
        double g = rc->lvl_gain / (rc->lvl_gain + (double)rc->n_coded);
        double f = pow(obs / rc->scale[cls], g);
        if (f > 0.0 && f < 1e6)
            for (int k = 0; k < RC_CLASSES; k++)
                rc->scale[k] *= f;
        rc->scale[cls] += (obs - rc->scale[cls]) / (rc->scale_n[cls] + 1.0);
        if (rc->scale_n[cls] < rc->scale_mem)
            rc->scale_n[cls] += 1.0;
    }
    rc->total_bits += bits;
    rc->n_coded++;
    if (rc->trace)
        fprintf(stderr, "RC done cls=%d bits=%.0f rf=%.3f cplx=%.0f obs=%.4g "
                "scale=%.4g total=%.0f\n",
                cls, bits, rf_used, cplx, obs, rc->scale[cls], rc->total_bits);
}
