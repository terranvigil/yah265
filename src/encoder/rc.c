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
 * T = bits_per_frame * w_i / mean(w): constant quality inside the budget,
 * with the complexity model deciding only how the budget is split.
 *
 * The OPERATING POINT - the base rate factor the whole sequence rides,
 * which the fair share leaves untouched because the frame's complexity
 * cancels between w_i and T - is MEASURED rather than modelled: it is the
 * running mean of `bits x qscale(base)` over every coded frame, against
 * the per-frame budget. That product is the numerator of the budget solve
 * itself, so the point is an identity and not a fit, and it is what the
 * class scales cannot be trusted for on content where most frames cost
 * the same at any quantiser (see n265_rc_next). On top of it sits a
 * proportional and an integral correction on the cumulative error, spread
 * over a horizon of `horizon` frames, which is what actually lands the
 * file on its target size.
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
#include "encoder/preset.h"

static double qscale_of_qp(double q)
{
    return 0.85 * pow(2.0, (q - 12.0) / 6.0);
}

static double qp_of_qscale(double qs)
{
    if (qs <= 0.0)
        return (double)N265_QP_MIN;   /* the finest QP there is */
    return 12.0 + 6.0 * log2(qs / 0.85);
}

/* one tunable, read once at open. The name is spelled literally at every
 * call site so scripts/knob_census.py can see the knob and its default. */
static double abr_tunable(const char *name, double def)
{
    const char *v = n265_env(name);
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
    rc->fbup = abr_tunable("N265_ABR_FBUP", rc->fbmax);
    rc->planerr = (int)abr_tunable("N265_ABR_PLANERR", 0);
    rc->w_mem = abr_tunable("N265_ABR_WMEM", 1000.0);
    rc->meas = (int)abr_tunable("N265_ABR_MEAS", 1);
    rc->mguard = abr_tunable("N265_ABR_MGUARD", 0.0);
    rc->cplxr_mem = abr_tunable("N265_ABR_CMEM", 1000.0);
    rc->tcap = (int)abr_tunable("N265_ABR_TCAP", 1);
    rc->cplxr_seed = abr_tunable("N265_ABR_CSEED", 0.0);
    rc->copen = (int)abr_tunable("N265_ABR_COPEN", 1);
    rc->ibrake = (int)abr_tunable("N265_ABR_IBRAKE", 1);
    rc->fbstep = (int)abr_tunable("N265_ABR_FBSTEP", 1);
    rc->openi = (int)abr_tunable("N265_ABR_OPENI", 0);
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
    rc->trace = n265_env("N265_RC_TRACE") != NULL;
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
        /* the WINDOW, not the queue: `--shot-window` can hold far more
         * frames than `--rc-lookahead` asked for, and the opening fit is
         * a statement about the window (la.h) */
        int nview = n265_la_view(&e->la);
        for (int i = 0; i < nview; i++) {
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
            /* w_avg stays the INTER yardstick the fair share is measured
             * against. `N265_ABR_OPENI` additionally prices the window's
             * share of intra into the RATE-FACTOR solve, at one intra frame
             * per keyint - the doc's own argument for keeping intra OUT of
             * the per-frame yardstick, used to say how much of it belongs in
             * the solve. On sita_flat, whose three scene-cut IDRs are 67% of
             * a 120-frame encode, the inter-only fit opens 3.7 QP too fine.
             *
             * REFUSED and kept as an arm (2026-09-10): it opens where it
             * says it will and the clip is worse for it - measured on the
             * shipping loop, sita_flat reads 1.097 / 1.122 / 1.214 / 1.241
             * against the inter-only fit's 1.090 / 1.093 / 1.099 / 1.105.
             * Opening coarse banks a credit the flat frames cannot spend,
             * and the loop pays it back on the frames that can. */
            double Wo = W;
            if (rc->openi && cls == RC_I && e->keyint > 0)
                Wo += rc_weight(rc, RC_I, cplx) * (double)n / (double)e->keyint;
            double rf = qp_of_qscale(Wo / (rc->bits_per_frame * n));
            rc->rf = rf < (double)N265_QP_MIN ? (double)N265_QP_MIN :
                      rf > 51.0 ? 51.0 : rf;
        }
        rc->have_rf = 1;
        /* Seed the measured operating point from the fit, at the prior
         * weight in frames the window itself carries, so the first coded
         * frames move it and the model only holds the point until they do.
         * `N265_ABR_CSEED` overrides that weight: it is the shrinkage on a
         * mean whose variance is all in the intra frames, and at 60 / 120 /
         * 200 frames it buys back 3.7 / 4.7 / 5.1 points of ABR BD-rate on
         * akiyo, mobile and tempete and costs sita_flat 1.6 / 3.1 / 5.2
         * points of rate. It is not what ships, because holding the FORCED
         * opening IDR out of the mean until a second intra frame arrives
         * (see n265_rc_done) buys the same BD back and costs nothing. */
        rc->cplxr = qscale_of_qp(rc->rf) * rc->bits_per_frame;
        rc->cplxr_n = rc->cplxr_seed > 0.0 ? rc->cplxr_seed
                                           : (n > 0 ? (double)n : 1.0);
        if (rc->cplxr_n > rc->cplxr_mem)
            rc->cplxr_n = rc->cplxr_mem;
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
     * keyint frames; the mean has to describe the other 249.
     *
     * Its memory (N265_ABR_WMEM) is the sequence, not a window. A window
     * tracks the SHOT, and a yardstick that tracks the shot makes every
     * shot average out to the same bits per frame - the across-shot
     * allocation collapses. Worth 3.5% of BD-rate on the multi-shot gate
     * (2026-09-09) and inert on any clip shorter than the memory. */
    if (cls != RC_I || rc->w_n < 1.0) {
        rc->w_avg += (w - rc->w_avg) / (rc->w_n + 1.0);
        if (rc->w_n < rc->w_mem)
            rc->w_n += 1.0;
    }
    double w_avg = rc->w_avg > 0.0 ? rc->w_avg : w;

    double T = rc->bits_per_frame * w / w_avg;      /* the fair share */
    rc->planned_bits += T;
    rc->last_planned = T;

    /* Two errors, and the two feedback terms read different ones.
     *
     * `N265_ABR_PLANERR` points the PROPORTIONAL term at the error against
     * the PLAN - the fair shares the allocator handed out - rather than
     * against the flat budget, on the reasoning that a fast term aimed at
     * the flat budget fights the complexity model: a hard shot is given more
     * than the flat budget by design and a six-frame horizon takes it
     * straight back. It is REFUSED (2026-09-09): rate accuracy goes from
     * 0.57% median / 2.67% worst to 1.34% / 12.74%, and the dSIZE leg is
     * what this loop exists for. Kept as an arm because it is the right
     * shape for the across-shot problem and will be re-priced when VBV
     * gives the budget error somewhere else to live.
     *
     * The INTEGRAL term always reads the error against the flat BUDGET,
     * because that is the one whose zero is the file landing on its size. */
    /* D4: the frames inside the observation lag are counted at their
     * planned bits. At lag 0 pend_bits and pend_n are zero and this is
     * the expression it always was. */
    double seen = rc->total_bits + rc->pend_bits;
    double nseen = (double)rc->n_coded + (double)rc->pend_n;
    double err = seen - (rc->planerr ? rc->planned_bits
                                     : rc->bits_per_frame * nseen);
    double errb = seen - rc->bits_per_frame * nseen;
    /* The horizon is in FRAMES and the frame's share of it is `T / (bpf x
     * H)`, which the qscale solve then cancels - so the loop asks this one
     * frame to move `err * T / (bpf * H)` bits. On a frame worth more than
     * H frames of budget that share exceeds the whole error, and the
     * correction overshoots by exactly the ratio. sita_flat's third IDR is
     * planned at 223k bits against a 9.6k budget: at H = 6 the loop asks a
     * 64k deficit to be closed by a frame it has just given 23 frames of
     * budget to, drops the rate factor 6 QP, and spends 294k recovering
     * 64k. Flooring the horizon at the frame's own plan is the statement
     * that no single frame is asked to move more bits than the whole error
     * - it is inert wherever T <= bpf * H, which is every frame of every
     * clip in the corpus except the intra frames and the frames on a cut.
     *
     * The floor is taken over the whole LOOKAHEAD WINDOW and not over this
     * frame alone, because the rate factor is a walked state: the frames
     * between two IDRs on sita_flat cost 200 bits at any quantiser, so they
     * bank a deficit they cannot spend, walk the rate factor down six steps
     * chasing it, and the step bound then carries that position into the
     * IDR, which is the frame that can spend. Reading the window is the
     * statement that a deficit is not spendable while a frame worth twenty
     * frames of budget is still seven frames away - it is the same
     * reservation a VBV would make, made from the plan the allocator has
     * already written down. Inert wherever the window holds nothing bigger
     * than H frames of budget. N265_ABR_TCAP=0 restores the flat horizon. */
    double hbits = rc->bits_per_frame * rc->horizon;
    double pbits = hbits;
    if (rc->tcap) {
        double tmax = T;
        int nview = n265_la_view(&e->la);
        for (int i = 1; i < nview; i++) {
            int k = n265_rc_future_idr(e, i) ? RC_I : cadence_class(e, i);
            double ti = rc->bits_per_frame *
                        rc_weight(rc, k, n265_rc_cplx(e, i, k)) / w_avg;
            if (ti > tmax)
                tmax = ti;
        }
        if (tmax > pbits)
            pbits = tmax;
    }
    double corr = 1.0 + err / pbits;
    if (corr < 0.5)
        corr = 0.5;
    else if (corr > 2.0)
        corr = 2.0;
    double corrb = 1.0 + errb / hbits;
    if (corrb < 0.5)
        corrb = 0.5;
    else if (corrb > 2.0)
        corrb = 2.0;
    /* The proportional term above settles wherever the accumulated error is
     * large enough to cancel the bits model's own bias - a standing offset,
     * and the file misses its size by exactly that. The integral term
     * removes it: it moves only while the error is non-zero, so the loop's
     * only fixed point is a cumulative error of zero, which is the size
     * landing on the target. The proportional term is what damps it; an
     * integrator this slow oscillates on its own. */
    rc->rf_bias += rc->igain * 6.0 * log2(corrb);
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
    if (fb > rc->fbup)
        fb = rc->fbup;
    else if (fb < -rc->fbmax)
        fb = -rc->fbmax;

    /* The operating point, and the whole of the sita_flat defect.
     *
     * `w / T` is `w_avg / bits_per_frame` identically - the frame's own
     * complexity cancels between the fair share and the price of it - so the
     * base rate factor is one per-sequence quantity, and the only question
     * is what estimates it. Two things were wrong with estimating it from the
     * model (2026-09-10, the sita_flat round):
     *
     *   - `w_avg` excludes intra frames, so the point solved for is the one
     *     that lands the budget on a sequence with no I frames in it, and the
     *     miss is exactly the I frames' share of the file: 8% on foreman
     *     (0.7 QP, invisible under the feedback), 27% on akiyo, and 67% on
     *     sita_flat_720p, where three scene-cut IDRs land in 120 frames and
     *     the base runs 9.4 QP too fine.
     *   - `scale[cls]` is a mean of per-frame RATIOS, so a frame whose bits
     *     do not depend on the quantiser at all - 108 of sita_flat's 120
     *     frames cost about 200 bits at any QP - counts as much as one that
     *     carries a tenth of the file. Over the flat stretch the leaf-B scale
     *     falls from 1.7 to 0.04 and takes the base down with it, and the
     *     lower it goes the smaller the observation, which is the runaway.
     *
     * Both disappear in the same estimator, and it is the identity behind
     * them rather than a fit: the rate factor that lands the budget solves
     * `sum(bits) = budget`, and `sum(bits x qscale)` over the coded frames
     * measures the numerator of that directly, with no scale, no class and
     * no complexity in it. So the point is the running MEAN of
     * `bits x qscale(base)` over EVERY coded frame, against the per-frame
     * budget. Checked against the truth on four clips at a constant QP it
     * reproduces the rate factor that lands each ladder target to 0.01 QP,
     * where the modelled point reads 22.71 against 32.10 on sita_flat.
     *
     * The model is not thereby unemployed: it still fits the opening from
     * the lookahead window, it still hands out the fair shares, and its
     * class scales are what the window is priced at. It no longer sets the
     * operating point. `N265_ABR_MEAS=0` gives that back. */
    double base = qp_of_qscale(w / T);
    if (rc->meas && rc->cplxr > 0.0 && rc->bits_per_frame > 0.0) {
        double m = qp_of_qscale(rc->cplxr / rc->bits_per_frame);
        /* At MGUARD 0 the measured point IS the operating point. Above it
         * the model keeps the point and the measurement is only a rail on
         * how far the model may be wrong, which is inert on every clip whose
         * two readings agree to within the guard. */
        if (base < m - rc->mguard)
            base = m - rc->mguard;
        else if (base > m + rc->mguard)
            base = m + rc->mguard;
    }
    /* The per-frame step is bounded so the rate factor cannot chase a single
     * odd frame, but the bound is relaxed over the opening: the priors set
     * only the order of magnitude and the first few coded frames replace
     * them, so the loop has to be allowed to move while that happens.
     *
     * The bound belongs on the FEEDBACK and not on the rate factor. The two
     * were the same thing while the operating point was a per-sequence
     * constant; now that it is measured it moves on its own, slowly and on
     * evidence, and there is nothing to damp in it - while rate-limiting the
     * SUM makes the rate factor a walked state that carries the wrong
     * position into the wrong frame. On sita_flat the frames between two
     * IDRs cost 200 bits at any quantiser, the loop walks six steps down
     * over them chasing a deficit they cannot spend, and the IDR then
     * inherits the walk: at the third IDR the feedback asks for 0 and the
     * step bound still hands it 2 QP below the operating point.
     * N265_ABR_FBSTEP=0 puts the bound back on the rate factor. */
    double step = rc->maxstep + rc->open_step / ((double)rc->n_coded + 1.0);
    double rf;
    if (rc->fbstep) {
        if (fb > rc->fb_prev + step)
            fb = rc->fb_prev + step;
        else if (fb < rc->fb_prev - step)
            fb = rc->fb_prev - step;
        rc->fb_prev = fb;
    }
    /* On an INTRA frame the feedback may brake and may not spend.
     *
     * The two directions are not the same risk. Coarsening an I frame to
     * recover an overspend works and is bounded - the worst case is that it
     * saves less than hoped. Fining one to spend a surplus is the most
     * expensive act the loop can take and it cannot be undone: an I frame is
     * ten to fifty frames of budget, it is where the R-D curve is steepest
     * (sita_flat's third IDR moves 2.5x over 4 QP where the model's own
     * slope says 1.6x), and the surplus that provoked it is usually an
     * artefact of the FLAT budget - on sita_flat the loop reads an 88k
     * deficit at the third IDR, spends 4.3 QP of rate factor closing it, and
     * ends the clip 21% OVER. A surplus always has somewhere cheaper to go,
     * because nine frames in ten are inter. N265_ABR_IBRAKE=0 restores the
     * symmetric term. */
    if (rc->ibrake && cls == RC_I && fb < 0.0)
        fb = 0.0;
    rf = base + fb;
    if (!rc->fbstep) {
        if (rf > rc->rf + step)
            rf = rc->rf + step;
        else if (rf < rc->rf - step)
            rf = rc->rf - step;
    }
    if (rf < (double)N265_QP_MIN)
        rf = (double)N265_QP_MIN;
    else if (rf > 51.0)
        rf = 51.0;
    rc->rf = rf;
    rc->pend_bits += T;
    rc->pend_n++;
    if (rc->trace)
        fprintf(stderr, "RC n=%ld cls=%d cplx=%.0f w=%.3g wavg=%.3g "
                "T=%.0f corr=%.4f bias=%+.3f fb=%+.3f base=%.3f rf=%.3f "
                "err=%.0f\n",
                rc->n_coded, cls, cplx, w, rc->w_avg, T, corr, rc->rf_bias,
                fb, base, rf, err);
    return rf;
}

void n265_rc_done(struct n265_encoder *e, int cls, double cplx, double rf_used,
                  double bits, double planned)
{
    n265_rc *rc = &e->rc;
    if (rc->pend_n > 0) {
        rc->pend_bits -= planned;
        rc->pend_n--;
        if (rc->pend_n == 0)
            rc->pend_bits = 0.0;    /* no drift from the running subtraction */
    }
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
    /* The measured operating point. Every coded frame counts, intra
     * included, and each counts for the bits it actually carried rather than
     * once: the product summed here is the numerator of the budget solve, so
     * a 200-bit frame moves it by 200 bits' worth and a scene-cut IDR by
     * its own. Its memory is the sequence for the reason WMEM's is - a
     * window tracks the shot - and the running-mean form is w_avg's. */
    double c = bits * qscale_of_qp(rf_used);
    if (rc->copen && cls == RC_I && rc->n_coded == 0) {
        /* The opening IDR is HELD, not counted.
         *
         * It is forced - every sequence starts on one - so on its own it says
         * nothing about how often intra frames recur, and the mean it would
         * join is a mean over frames that intra frames dominate. Counting one
         * forced frame as 1 in n moves the operating point by the whole of
         * its share: on akiyo the opening IDR is 27% of a 120-frame file, the
         * point moves 2.6 QP coarse, and the clip pays 4 points of ABR
         * BD-rate for an allocation that used to give its one I frame - and
         * so the 119 frames that reference it - the finer quantiser.
         *
         * A SECOND intra frame is the evidence that was missing. When one
         * arrives the held observation is folded in beside it and the point
         * becomes the honest all-frames mean from there on, which is what
         * sita_flat needs and akiyo never asks for. */
        rc->copen_bits = c;
        rc->copen_held = 1;
    } else {
        if (rc->copen_held && cls == RC_I) {
            rc->cplxr += (rc->copen_bits - rc->cplxr) / (rc->cplxr_n + 1.0);
            if (rc->cplxr_n < rc->cplxr_mem)
                rc->cplxr_n += 1.0;
            rc->copen_held = 0;
        }
        rc->cplxr += (c - rc->cplxr) / (rc->cplxr_n + 1.0);
        if (rc->cplxr_n < rc->cplxr_mem)
            rc->cplxr_n += 1.0;
    }
    rc->total_bits += bits;
    rc->n_coded++;
    if (rc->trace)
        fprintf(stderr, "RC done cls=%d bits=%.0f rf=%.3f cplx=%.0f obs=%.4g "
                "scale=%.4g cplxr=%.4g total=%.0f\n",
                cls, bits, rf_used, cplx, obs, rc->scale[cls], rc->cplxr,
                rc->total_bits);
}
