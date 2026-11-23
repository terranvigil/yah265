/*
 * Single-pass average-bitrate rate control.
 *
 * The control variable is one rate factor in QP units, the same axis CRF
 * rides on, and every per-frame term the CRF path already applies (the
 * pyramid cascade, the AQ frame-mean, the per-CTU AQ and cu-tree fields)
 * lands on top of it unchanged. So ABR is CRF with the base solved from
 * bits instead of given.
 *
 * The model:
 *
 *   qscale(q)   = 0.85 * 2^((q - 12) / 6)     the classic quantizer scale
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
 * frame's real QP - the cascade, the AQ frame mean, the un-centered cu-tree
 * field - and none of those has to be modeled twice.
 *
 * The allocator gives frame i its fair share of the budget,
 * T = bits_per_frame * w_i / mean(w): constant quality inside the budget,
 * with the complexity model deciding only how the budget is split.
 *
 * The OPERATING POINT - the base rate factor the whole sequence rides,
 * which the fair share leaves untouched because the frame's complexity
 * cancels between w_i and T - is MEASURED rather than modeled: it is the
 * running mean of `bits x qscale(base)` over every coded frame, against
 * the per-frame budget. That product is the numerator of the budget solve
 * itself, so the point is an identity and not a fit, and it is what the
 * class scales cannot be trusted for on content where most frames cost
 * the same at any quantizer (see n265_rc_next). On top of it sits a
 * proportional and an integral correction on the cumulative error, spread
 * over a horizon of `horizon` frames, which is what actually lands the
 * file on its target size.
 *
 * One per-frame term does NOT land on top of it unchanged, and it is the
 * exception that proves the rule: the cu-tree FRAME term (phase 7) is added
 * to the base by the caller AFTER this loop has answered, so it moves the
 * control variable rather than riding it, and no measurement in here absorbs
 * it. The loop is told it and takes it back off, which is both the fix and a
 * refusal - what the term was buying under ABR is real. See the block by
 * `rf`.
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

/* N265_RC_MVRATE: what this model charges for the motion the frame's
 * prediction is fetched with, in cost units per bit (phase 14 item 21).
 *
 * `pcost_sum` is a bare lowres SAD and the priors this model opens with -
 * `N265_ABR_SI/SP/SBR/SB`, geometric means of `bits x qscale` per
 * compressed complexity read off CQP traces - were fitted against a search
 * that could not reach past eight lowres pixels. Item 19's predictors buy
 * distortion with VECTOR LENGTH, so the same content now reports a smaller
 * complexity while costing the coder the same bits, and the two halves of
 * the model are no longer in the same units. It is not a uniform shift that
 * a learned scale absorbs, and the asymmetry is the damage: `icost_sum` did
 * not move, so the I-against-inter balance the priors carry is read in one
 * domain on one side and another on the other, and on a clip whose two
 * intra frames never let `scale[RC_I]` adapt it is never corrected.
 * sita_flat_720p's opening window shows it in ONE frame - display 2 reads
 * 1 661 907 against the old search and 143 129 against the honest one,
 * eleven times cheaper, and the lookahead-fitted opening drops 1.14 QP with
 * it on a clip whose flat frames cannot give the bits back.
 *
 * The frame the coder really codes pays for its motion. So the complexity
 * is priced on SAD PLUS the vector's own bits on the exp-Golomb shape, the
 * same D + lambda R `N265_CT_MVRATE` prices cu-tree's propagate fraction
 * with and the same one every full-resolution search here uses. It is
 * self-limiting: on content whose honest vectors are short - every clip the
 * old search could already measure - it changes nothing, and it charges
 * exactly the frames whose complexity fell because the search reached
 * further.
 *
 * The INTRA complexity is untouched, because nothing fetches an intra
 * prediction.
 *
 * 32 ships and the value is a THRESHOLD, not a slope: the term only ever
 * moves a frame across the cap, and 32 is where sita_flat's display 2 goes
 * back over its own intra cost (at 16 it does not, and the clip reads its
 * unrefitted 1.713). 0 is the pre-item-21 complexity. */
static int rc_mvrate(void)
{
    static int v = -1;
    if (v < 0) { const char *e = n265_env("N265_RC_MVRATE"); v = e ? atoi(e) : 32; }
    return v;
}

double n265_rc_cplx(struct n265_encoder *e, int i, int cls)
{
    const la_frame *f = &e->la.f[i];
    double ic = (double)f->icost_sum;
    double pc = (double)f->pcost_sum +
                (double)rc_mvrate() * (double)f->mvbits_sum;
    if (cls == RC_I)
        return ic > 1.0 ? ic : 1.0;
    /* an inter frame whose motion search does worse than intra will be
     * coded largely as intra, so intra caps it */
    /* The charge lands on the CAP TEST and nowhere else. The cap is a
     * CLASSIFICATION - "this frame will be coded largely as intra" - and a
     * classification has to be made on the cost the coder really pays, the
     * motion included. The VALUE, where the frame is not capped, stays the
     * bare distortion the priors were fitted on, so the term is inert on
     * every frame whose class it does not change: at rate 32 the whole
     * seventeen-clip ABR table is main's to the digit except the two cells
     * this item is about. Charging it into the value as well was measured
     * and is not shipped. */
    double c = pc < ic ? (double)f->pcost_sum : ic;
    /* --intra-refresh: the column is coded intra */
    int j = n265_ir_pos(e, f->display_idx);
    if (j >= 0 && e->ctb_cols > 0) {
        long C = e->ctb_cols, P = e->keyint;
        double s = (double)((j + 1) * C / P - j * C / P) / (double)C;
        c = (1.0 - s) * c + s * ic;
    }
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
    /* THE VBR PROFILE (phase 14 item 29): under a buffer, two of this
     * loop's constants take different defaults, and nothing else moves.
     *
     * The loop was tuned on 120-frame clips, where the file has to land
     * inside a few seconds and a 6-frame horizon with a 6 QP rail is what
     * does it. On a real title at 60 fps that horizon is a tenth of a
     * second, and every heavy stretch pins the coarse rail: Meridian
     * 720p at 181k codes ten seconds of its opening at QP 50, where the
     * bits no longer answer the quantizer (240 frames of that content
     * cost 59 kB at QP 51 against 81 kB at 45) and VMAF-NEG falls to 40.
     * Under a VBV the buffer is what bounds a local overspend, so the
     * coarse rail can be short: 1.5 QP over the measured operating point.
     * The error it no longer brakes on is paid later, where the content
     * is lighter and a QP is cheaper.
     *
     * The second is the intra offset from the frame's own cu-tree term
     * (N265_ABR_IOFF_CT, below), which the allocator prices. It was
     * refused for ABR on the CIF-7 BD, and it stays off there; on the
     * long segments it is the part of the term's allocation the loop can
     * have without losing the file size.
     *
     * Without --vbv-maxrate both keep their ABR defaults, so plain ABR,
     * and CRF and CQP (which never reach this function's answer), are
     * byte-identical to main. */
    int vbr = e->abr_on && e->vbv.on;
    /* capped VBR: a cap above the target, not a CBR-shaped VBV (item 37) */
    int cvbr = vbr && !e->strict_cbr &&
               e->vbv.maxrate > e->abr_bitrate * 1.0001;
    /* ...and there, from item 37, the coarse rail is the fine one again:
     * the slow feedback below rarely reaches it on long content, and on a
     * 120-frame clip 1.5 QP was too little to bring the opening's
     * overspend back (median rate error on the seventeen corpus clips at
     * 1.5x / 2 s: 5.3% at 1.5, 3.1% at 6; main 6.7%) */
    rc->fbup = abr_tunable("N265_ABR_FBUP",
                           cvbr ? abr_tunable("N265_VBR_FBUP", rc->fbmax)
                                : vbr ? 1.5 : rc->fbmax);
    rc->planerr = (int)abr_tunable("N265_ABR_PLANERR", 0);
    /* Phase 14 item 37: capped VBR rides the CRF path with a slow level.
     * Four changes, on together by default where the cap is above the
     * target (maxrate over the bitrate, and not --strict-cbr), because a
     * buffer with headroom above the target is what lets the level move
     * slowly; a CBR-shaped VBV keeps the horizon loop it was tuned with.
     *
     *   N265_VBR_FBW 2     the feedback over a window that grows with the
     *                      square root of the coded duration (n265_rc_next)
     *   N265_VBR_OVW 0.75  ...capped at that share of the buffer, widened
     *                      past six seconds in proportion to the time
     *   N265_VBR_CTKEEP 1  the cu-tree frame term rides the coded base, as
     *                      under CRF; the ledger reads the frame without it
     *   N265_VBV_OWN 3     the clamp plans the frame and its window at each
     *                      class's own scale, and guards the frame alone
     *                      at the pessimistic one against the fill
     *   N265_VBR_FBUP 6    (above) the item 29 rail back to the fine one;
     *                      N265_ABR_FBUP, when set, still wins
     *
     * On the dev set's VBR cells they read
     * -7.8% / -11.2% / -2.2% BD-VMAF-NEG against main on chimera 720p's
     * two-minute segment, meridian 2160p and sollevante 1080p.
     * `N265_VBR_FBW=0 N265_VBR_OVW=0 N265_VBR_CTKEEP=0 N265_VBV_OWN=0
     * N265_VBR_FBUP=1.5` is main byte for byte. Plain ABR, a VBV with
     * maxrate at the target, CRF, capped CRF and CQP never read them. */
    rc->fps = fps;
    rc->fbw = vbr ? abr_tunable("N265_VBR_FBW", cvbr ? 2.0 : 0.0) : 0.0;
    rc->fbt0 = abr_tunable("N265_VBR_FBT0", 2.0);
    rc->fbg = abr_tunable("N265_VBR_FBG", 16.0);
    rc->fbgup = abr_tunable("N265_VBR_FBGUP", 12.0);
    rc->ovw = vbr ? abr_tunable("N265_VBR_OVW", cvbr ? 0.75 : 0.0) : 0.0;
    rc->ctkeep = vbr ? (int)abr_tunable("N265_VBR_CTKEEP", cvbr ? 1 : 0) : 0;
    rc->vown = vbr ? (int)abr_tunable("N265_VBV_OWN", cvbr ? 3 : 0) : 0;
    /* Phase 14 item 38: where a capped-VBR surplus is spent. Two changes,
     * on by default under capped VBR only.
     *
     *   N265_VBR_FBFINE 3     the finer-side rail of the feedback, 3 QP
     *                         under the level instead of 6
     *   N265_VBR_SPENDFILL 0.4  the finer-side feedback scales from whole
     *                         at 0.8 of the buffer (N265_VBR_SPENDHI) to
     *                         none at 0.4
     *
     * On chimera 720p's two-minute segment the slow feedback banked the
     * surplus of an easy stretch, rode the 6 QP rail into the hard scene
     * after it and ran the buffer to 0.7%. Every rung lands where it did
     * or up to 1.2% lower; D5v reads -2.9% BD-VMAF-NEG against main and
     * +6.30% against x265 (+9.73% before), D2 and D4 within 0.2.
     * `N265_VBR_FBFINE=6
     * N265_VBR_SPENDFILL=0` is main byte for byte.
     *
     * N265_VBR_SPEND, the same feedback scaled by the frame's complexity
     * over its running mean, is refused and kept as an arm (off). */
    rc->fbspend = cvbr ? abr_tunable("N265_VBR_SPEND", 0.0) : 0.0;
    rc->spend_lo = cvbr ? abr_tunable("N265_VBR_SPENDFILL", 0.4) : 0.0;
    rc->spend_hi = abr_tunable("N265_VBR_SPENDHI", 0.8);
    rc->fbfine = cvbr ? abr_tunable("N265_VBR_FBFINE", 3.0) : rc->fbmax;
    /* Phase 14 item 48: the finer-side rail widens with the deficit
     * (rc_next, N265_VBR_FBTRK). Capped VBR only; 0 is off. */
    rc->fbtrk = cvbr ? abr_tunable("N265_VBR_FBTRK", 0.06) : 0.0;
    rc->fbtrk_w = abr_tunable("N265_VBR_FBTRK_W", 0.04);
    rc->fbtrk_max = abr_tunable("N265_VBR_FBTRK_MAX", 6.0);
    /* N265_VBR_DEFPT (item 50): past this deficit, and only after
     * `defpt_t0` seconds, the loop latches onto the short-memory operating
     * point where it is finer, until the deficit is back to `defpt_rel`;
     * its finer move is paced (_SLEW, QP/s) and capped (_CAP), and it
     * stands aside for _HOLD seconds after a floor-complexity frame
     * (n265_rc_next). Capped VBR only; 0 is off. */
    rc->defpt = cvbr ? abr_tunable("N265_VBR_DEFPT", 0.08) : 0.0;
    rc->defpt_w = abr_tunable("N265_VBR_DEFPT_W", 0.02);
    rc->defpt_rel = abr_tunable("N265_VBR_DEFPT_REL", 0.05);
    rc->defpt_on = 0;
    rc->defpt_rail = (int)abr_tunable("N265_VBR_DEFPT_RAIL", 1);
    rc->defpt_t0 = abr_tunable("N265_VBR_DEFPT_T0", 60.0);
    rc->defpt_slew = abr_tunable("N265_VBR_DEFPT_SLEW", 0.25);
    rc->defpt_cap = abr_tunable("N265_VBR_DEFPT_CAP", 6.0);
    rc->defpt_dq = 0.0;
    rc->defpt_railq = 0.0;
    rc->defpt_hold = abr_tunable("N265_VBR_DEFPT_HOLD", 2.0);
    rc->defpt_spend = abr_tunable("N265_VBR_DEFPT_SPEND", 0.0);
    rc->dlc_avg = rc->dlc_n = 0.0;
    rc->defpt_nf = 1e9;
    rc->cplxr_smem = abr_tunable("N265_VBR_DEFPT_MEM", 1000.0);
    rc->cplxr_s = rc->cplxr_sn = 0.0;
    /* N265_VBR_VLVL (item 49): the share of each frame's clamp demand a
     * running clamp level takes, capped VBR only; 0 is off (n265_rc_next). */
    rc->vlvl_a = cvbr ? abr_tunable("N265_VBR_VLVL", 0.1) : 0.0;
    rc->vlvl = 0.0;
    /* N265_VBR_FILLQ (item 49): below `fillgoal` of the buffer every frame
     * is raised in proportion to the shortfall, `fillq` QP at empty. With
     * the whole-encode level (N265_VBR_CMEM) the loop no longer coarsens
     * on its own over a hard minute, and the clamp's 20-frame window let
     * Meridian 720p at 133k drain 397 kbit to 33 kbit over 200 frames and
     * then meet a strobe of intra pictures at QP 51: 61 underflows at
     * 1.5x / 2 s where main read none. At 6 it reads none, and the cell's
     * BD improves (-3.95% -> -4.72%). 0 is off. */
    rc->fillq = cvbr ? abr_tunable("N265_VBR_FILLQ", 6.0) : 0.0;
    rc->fillgoal = abr_tunable("N265_VBR_FILLGOAL", 0.5);
    /* N265_VBR_CTLPAY / N265_VBR_CTLSEG (n265_rc_next): after a live RATE
     * step, the pace its carried balance is repaid at as a share of the
     * new rate, and whether the slow feedback counts from the step. 0 is
     * off for each. */
    rc->ctlpay = cvbr ? abr_tunable("N265_VBR_CTLPAY", 0.02) : 0.0;
    rc->ctlseg = cvbr ? (int)abr_tunable("N265_VBR_CTLSEG", 1) : 0;
    rc->stepped = 0;
    rc->carry = rc->step_n0 = 0.0;
    rc->w_mem = abr_tunable("N265_ABR_WMEM", 1000.0);
    rc->meas = (int)abr_tunable("N265_ABR_MEAS", 1);
    rc->mguard = abr_tunable("N265_ABR_MGUARD", 0.0);
    /* N265_VBR_CMEM (phase 14 item 49): under capped VBR the operating
     * point's memory is the whole encode. At 1000 frames it is 17 s of a
     * 60 fps title, so a ten-minute excerpt spent its bits evenly over
     * every 17 s and lost what CRF's level holds across scenes: Meridian
     * 720p's ten minutes read +2.41% BD-VMAF-NEG against x265 at 1000
     * and +0.31% here. The slow feedback still lands the file.
     * N265_ABR_CMEM, when set, wins; `N265_VBR_CMEM=1000` is main. */
    rc->cplxr_mem = abr_tunable("N265_ABR_CMEM",
                                cvbr ? abr_tunable("N265_VBR_CMEM", 1e6)
                                     : 1000.0);
    rc->tcap = (int)abr_tunable("N265_ABR_TCAP", 1);
    rc->cplxr_seed = abr_tunable("N265_ABR_CSEED", 0.0);
    rc->copen = (int)abr_tunable("N265_ABR_COPEN", 1);
    rc->ibrake = (int)abr_tunable("N265_ABR_IBRAKE", 1);
    rc->fbstep = (int)abr_tunable("N265_ABR_FBSTEP", 1);
    rc->openi = (int)abr_tunable("N265_ABR_OPENI", 0);
    rc->ctf_comp = (int)abr_tunable("N265_ABR_CTF", 1);
    rc->ioff = abr_tunable("N265_ABR_IOFF", 0.0);
    rc->ioff_ct = abr_tunable("N265_ABR_IOFF_CT",
                                  vbr && !rc->ctkeep ? 1.0 : 0.0);
    rc->ioff_max = abr_tunable("N265_ABR_IOFF_MAX", 6.0);
    /* How much of this loop's own feedback a PLANNED frame still carries
     * (twopass.h). The plan is written from what pass 1 really spent, so
     * the second pass is not guessing and the feedback is a correction on
     * a model error rather than the control itself - at gain 1 the loop
     * fights its own plan, chasing a cumulative error the plan already
     * accounts for, and the trajectory is the one-pass trajectory with
     * extra steps. At 0 the plan is open loop and whatever the model gets
     * wrong lands on the file size. */
    rc->tp_gain = abr_tunable("N265_2P_GAIN", 0.25);
    /* Prior per-class scales: bits x qscale per lowres block per
     * (per-block complexity)^qcomp. Geometric means over 11 corpus clips x
     * QP 24/30/36 x 40 frames, read off N265_RC_TRACE on CQP encodes.
     * They seed the opening
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
    /* the VBV's own per-class scale opens at the same priors and diverges
     * from there (rc.h); its decay is per OBSERVATION OF ITS OWN CLASS, so
     * on a 4-class cadence sixteen of them is about sixty frames */
    rc->vdecay = abr_tunable("N265_VBV_DECAY", 0.0625);
    /* the cut prediction is ABR only; the brake was ABR-with-a-buffer only
     * until phase 14 item 47 */
    rc->brake = e->abr_on && e->vbv.on
                    ? (int)abr_tunable("N265_VBV_BRAKE", 1) : 0;
    /* Phase 14 item 47: capped CRF's buffer. The clamp prices a frame on
     * the class scales, which a clip's opening I frame has not observed
     * yet; a 2.9x miss there (chimera 720p's first picture, 1.18 Mbit
     * against a 0.92 Mbit buffer) was an underflow nothing after it could
     * undo. Four parts, on together under capped CRF:
     *
     *   N265_CCRF_BRAKE 1        the in-picture brake, which reads the
     *                            picture as it is coded
     *   N265_CCRF_BRAKE_FILL 0.8 ...holding it to that share of the fill
     *   N265_CCRF_BRK_R0 1.25    its calibration before any class has one
     *   N265_CCRF_GUARD 1.3      the frame alone priced at that margin
     *                            over the model against the fill (rc.c,
     *                            n265_vbv_crf_base)
     *   N265_CCRF_SKIP 1         an inter CTU the brake cannot pay for at
     *                            its last QP is coded as one skip
     *
     * `N265_CCRF_BRAKE=0 N265_CCRF_GUARD=1` is main byte for byte. */
    if (!e->abr_on && e->vbv.on)
        rc->brake = (int)abr_tunable("N265_CCRF_BRAKE", 1);
    rc->ccrf_r0 = abr_tunable("N265_CCRF_BRK_R0", 1.25);
    rc->ccrf_skip = !e->abr_on && e->vbv.on
                        ? (int)abr_tunable("N265_CCRF_SKIP", 1) : 0;
    rc->ccrf_skfill = abr_tunable("N265_CCRF_SKIP_FILL", 0.8);
    rc->ccrf_satq = abr_tunable("N265_CCRF_SKIP_SATQ", 51.0);
    rc->ccrf_skwarm = abr_tunable("N265_CCRF_SKIP_WARM", 4.0);
    rc->ccrf_guard = !e->abr_on && e->vbv.on
                         ? abr_tunable("N265_CCRF_GUARD", 1.3) : 0.0;
    rc->brk_gain = abr_tunable("N265_VBV_BRAKE_K0", 0.05);
    rc->flatc = e->abr_on && e->vbv.on ? abr_tunable("N265_ABR_FLATC", 17.0)
                                       : 0.0;
    rc->brk_fill = abr_tunable("N265_VBV_BRAKE_FILL", 0.9);
    if (!e->abr_on && e->vbv.on)
        rc->brk_fill = abr_tunable("N265_CCRF_BRAKE_FILL", 0.8);
    rc->scpred = e->abr_on ? abr_tunable("N265_VBV_SCPRED", 0.5) : 1.0;
    for (int i = 0; i < RC_CLASSES; i++)
        rc->vscale[i] = rc->scale[i];
    /* the size model (rc.h): ABR with a buffer only, like the brake.
     * 2 feeds it to the lag's books alone (the walk the clamp and the
     * brake plan against, and the loop's count of the frames in flight);
     * inert at one worker, where the lag is 0. 1 also prices the clamp
     * and the brake's plan on it, and is refused: +1.94% BD-VMAF-NEG on
     * Chimera 720p. Both are arms, off, which is main. */
    rc->szm = e->abr_on && e->vbv.on
                  ? (int)abr_tunable("N265_VBR_SZM", 0) : 0;
    rc->szm_lam = abr_tunable("N265_VBR_SZM_LAM", 0.9);
    rc->szm_z = abr_tunable("N265_VBR_SZM_Z", 2.0);
    /* one deviation in the lag's walk: at 0.5 the 0.5 s buffer cells
     * of scripts/vbv_check.py underflow 10 times at two workers (main's
     * calibrated walk: 15), at 1.0 none do; 1.0 costs Meridian 720p's
     * low rungs 0.1-0.4 VMAF-NEG against 0.5 */
    rc->szm_zlag = abr_tunable("N265_VBR_SZM_ZLAG", 1.0);
    rc->szm_gmin = abr_tunable("N265_VBR_SZM_GMIN", 0.7);
    rc->szm_static = abr_tunable("N265_VBR_SZM_STATIC", 17.0);
    rc->szm_simple = abr_tunable("N265_VBR_SZM_SIMPLE", 150.0);
    rc->cur_szk = -1;
    /* the opening's corpus prior (step 3, REFUSED at the model and kept
     * off): log2(bits / nblk) = a + b log2(cost / nblk) + g base / 6, per
     * class, least squares over the seventeen corpus clips at three VBR
     * rates each, static frames left out. On the three 720p segments,
     * held out, it opens 7.0 QP from where the loop settles against the
     * allocator prior's 7.8, and moves Chimera, which the prior opens
     * right, 6 QP fine. */
    rc->vopen = e->abr_on && e->vbv.on
                    ? (int)abr_tunable("N265_VBR_OPEN", 0) : 0;
    rc->op_a[RC_I] = abr_tunable("N265_VBR_OPEN_AI", -7.027);
    rc->op_a[RC_P] = abr_tunable("N265_VBR_OPEN_AP", 2.084);
    rc->op_a[RC_BREF] = abr_tunable("N265_VBR_OPEN_ABR", -0.806);
    rc->op_a[RC_B] = abr_tunable("N265_VBR_OPEN_AB", -1.238);
    rc->op_b[RC_I] = abr_tunable("N265_VBR_OPEN_BI", 2.157);
    rc->op_b[RC_P] = abr_tunable("N265_VBR_OPEN_BP", 1.249);
    rc->op_b[RC_BREF] = abr_tunable("N265_VBR_OPEN_BBR", 1.374);
    rc->op_b[RC_B] = abr_tunable("N265_VBR_OPEN_BB", 1.224);
    rc->op_g = abr_tunable("N265_VBR_OPEN_G", -1.429);
    rc->trace = n265_env("N265_RC_TRACE") != NULL;
    rc->rf = 26.0;
    /* live control's large step (contract v1.3). N265_LS_VBVUP: a
     * buffered step up takes it under capped VBR at 1, under CBR too
     * at 2, never at 0 */
    rc->ls_on = (int)abr_tunable("N265_LS", 1);
    rc->ls_vbvup = (int)abr_tunable("N265_LS_VBVUP", 1);
    /* the slope a refit accepts (N265_LS_AMIN), and the first move's
     * divisor by the starting base: 1 at N265_LS_QLO and below, ramping
     * to 1.5 at N265_LS_QHI (N265_LS_QRAMP 0: 1.5 everywhere). Sintel's
     * 10x step down misses by +64% in its first second at 0, +31% with
     * the base read off the operating point (1), and +7% with it read
     * off group A's coded base (2): the feedback had held the coded base
     * 6 QP under the point.
     * N265_LS_RAIL widens the feedback's rails during the procedure
     * (0: off) */
    rc->ls_amin = abr_tunable("N265_LS_AMIN", 1.0);
    rc->ls_qramp = (int)abr_tunable("N265_LS_QRAMP", 2);
    rc->ls_qlo = abr_tunable("N265_LS_QLO", 15.0);
    rc->ls_qhi = abr_tunable("N265_LS_QHI", 21.0);
    rc->ls_rail = abr_tunable("N265_LS_RAIL", 0.0);
    /* N265_LS_OPEN: a run A reaching into the first that many seconds
     * coded is missing (0: off). On with no buffer: plain ABR's step down
     * at 1 s goes 9.5% -> 5.0% in its first second. Under a buffer it
     * cost capped VBR's step at 1 s (18.7% -> 26.1%), so it is off there.
     * N265_LS_CAP: while refitting, the carried balance is held to that
     * many seconds of the target, inside rule (f)'s one. With no buffer
     * it takes ABR's step up after 12 s low from 39.7% to 20.8% in the
     * second second and moves nothing else the wrong way; under a buffer
     * (N265_LS_CAP_VBV) it costs capped VBR's steps up 4-10 points in
     * the first second, so it is off there. */
    rc->ls_open = abr_tunable("N265_LS_OPEN", e->vbv.on ? 0.0 : 1.0);
    rc->ls_cap = e->vbv.on ? abr_tunable("N265_LS_CAP_VBV", 0.0)
                           : abr_tunable("N265_LS_CAP", 0.25);
}

/* the predicted bits x qscale of a frame. The complexity is normalized per
 * lowres block before it is compressed, so `scale` is a per-block constant
 * and the priors carry across resolutions; without that the same content at
 * 720p reads five times the CIF scale purely from the block count. */
static double rc_weight(n265_rc *rc, int cls, double cplx)
{
    return rc->scale[cls] * rc->nblk * pow(cplx / rc->nblk, rc->qcomp);
}

/* the same weight at the VBV's own scale (rc.h): same model, same
 * complexity, an estimator that a cheap frame of another class cannot pull
 * down. Never below the allocator's, so the clamp sees the worse of the
 * two rather than a second opinion it might like better. */
static double rc_vweight(n265_rc *rc, int cls, double cplx, double icplx)
{
    /* An inter frame the motion search does no better than intra on will be
     * coded largely AS intra, which n265_rc_cplx says in the only way it
     * can - by capping the complexity at the intra cost. Pricing that frame
     * at its own class's scale is the single largest under-prediction this
     * model makes: sita_flat_720p's shot change at display 18 is a P, it is
     * predicted at 25 kbit and costs 156 kbit, and the whole factor of six
     * is scale[RC_I] / scale[RC_P]. For the BUFFER, and only for the
     * buffer, such a frame is priced as the intra frame it will be. The
     * allocator is left alone: there the cap is the right answer, because
     * what it allocates is a share and not a bound. */
    if (cls != RC_I && icplx > 0.0 && cplx >= rc->scpred * icplx)
        cls = RC_I;
    double w = rc->vscale[cls] * rc->nblk * pow(cplx / rc->nblk, rc->qcomp);
    double a = rc_weight(rc, cls, cplx);
    return w > a ? w : a;
}

/* The same weight at the allocator's own scale, with the intra-dominated
 * test kept (N265_VBV_OWN, item 37). */
static double rc_oweight(n265_rc *rc, int cls, double cplx, double icplx)
{
    if (cls != RC_I && icplx > 0.0 && cplx >= rc->scpred * icplx)
        cls = RC_I;
    return rc_weight(rc, cls, cplx);
}

/* The INTRA QP offset, in QP, for a frame of class `cls` whose cu-tree frame
 * term is (or is estimated at) `ctf`. Positive means FINER: the frame is coded
 * at `rf - ioff`.
 *
 * Zero on every inter frame, and zero at the compiled default, which is what
 * makes this knob inert until it is armed. Two arms:
 *
 *   N265_ABR_IOFF     a constant, in QP. The honest form of the I-frame bonus
 *                     the cu-tree frame term used to hand the opening IDR by
 *                     accident: every intra frame, the same discount,
 *                     and the allocator prices it.
 *   N265_ABR_IOFF_CT  a share of the frame's own cu-tree term instead, taken
 *                     on the FINER side only. The term's claim was that it
 *                     knows which intra frames are worth referencing and by
 *                     how much; this is that claim priced rather than stolen.
 *                     The two add, so IOFF is the floor under the derived
 *                     form when both are set.
 *
 * N265_ABR_IOFF_MAX bounds the sum, because an intra frame is ten to fifty
 * frames of budget and a discount the model got wrong there is the most
 * expensive mistake this loop can make (the argument N265_ABR_IBRAKE already
 * makes about spending on intra). */
static double rc_ioff(n265_rc *rc, int cls, double ctf)
{
    if (cls != RC_I)
        return 0.0;
    double o = rc->ioff;
    if (rc->ioff_ct != 0.0 && ctf < 0.0)
        o += rc->ioff_ct * -ctf;
    if (o < 0.0)
        o = 0.0;
    if (o > rc->ioff_max)
        o = rc->ioff_max;
    return o;
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

/* The VBV's per-frame clamp, in one place for both modes.
 *
 * `q_code` is the base the frame is ABOUT to be coded at - not the rate
 * factor, the base: under ABR the encoder adds the cu-tree frame term back
 * on after this loop answers, and the class scales are learned against the
 * base, so the base is the only quantizer the bits model is calibrated in.
 * The model's prediction there is `weight / qscale(base)`, and the buffer's
 * answer is how many bits it can actually pay for at this removal and at
 * every planned removal in the window behind it.
 *
 * When the prediction is over the allowance the base rises by exactly the
 * ratio, `6 log2(pred / allow)`, which is the quantizer step that makes the
 * model's own prediction equal the allowance. It is one step and not an
 * iteration: the window's plans were priced at the OLD base, so raising it
 * only leaves them conservative - every future frame will be cheaper than
 * the reservation assumed, which errs on the side the buffer cares about.
 *
 * The window is walked in LOOKAHEAD order and removed in CODING order, and
 * those differ inside a mini-GOP. The cumulative bound is what absorbs it:
 * over a whole mini-GOP the two orders carry the same bits, so the
 * reservation is exact at the GOP boundaries and off by at most a
 * re-ordering inside one, which is smaller than the model error it is
 * already carrying.
 *
 * Returns the QP to ADD to the base; *pred and *allow are the trace's. */
/* The clamp on the size model (N265_VBR_SZM, item 32). The frame is
 * priced by its own fit, raised by `szm_z` residual deviations because a
 * buffer wants the bad side of a forecast; the window behind it at its
 * class fit, unbiased, because a reservation wants the mean. An inter
 * frame whose complexity reaches `scpred` of its intra cost is priced at
 * the worse of its own fit and the intra fit, as the old clamp prices it.
 * The QP it adds is the fit's own: `6 log2(p / a)` over the fitted slope,
 * so content whose bits stop answering the quantizer is raised by what it
 * takes, not by the textbook step. */
static double vbv_dqp_szm(struct n265_encoder *e, int cls, double cplx,
                          double icplx, double q_code, double *pred,
                          double *allow)
{
    n265_rc *rc = &e->rc;
    double slope, sd;
    double p = n265_szm_pred(rc, cls, rc->cur_szk, cplx, q_code, &slope,
                             &sd);
    if (cls != RC_I && icplx > 0.0 && cplx >= rc->scpred * icplx) {
        double s2, d2;
        double pi = n265_szm_pred(rc, RC_I, -1, cplx, q_code, &s2, &d2);
        if (pi > p) {
            p = pi;
            slope = s2;
            sd = d2;
        }
    }
    p *= exp2(rc->szm_z * sd);
    double fut[LA_MAX];
    int nf = 0;
    int nview = n265_la_view(&e->la);
    for (int i = 1; i < nview && nf < LA_MAX; i++) {
        int k = n265_rc_future_idr(e, i) ? RC_I : cadence_class(e, i);
        double c = n265_rc_cplx(e, i, k), ic = n265_rc_cplx(e, i, RC_I);
        double f = n265_szm_pred(rc, k, -1, c, q_code, NULL, NULL);
        if (k != RC_I && ic > 0.0 && c >= rc->scpred * ic) {
            double fi = n265_szm_pred(rc, RC_I, -1, c, q_code, NULL, NULL);
            if (fi > f)
                f = fi;
        }
        fut[nf++] = f;
    }
    double a = n265_vbv_allow(&e->vbv, fut, nf);
    *pred = p;
    *allow = a;
    if (!(p > a))
        return 0.0;
    e->vbv.clamped++;
    double g = -slope;
    if (g < rc->szm_gmin)
        g = rc->szm_gmin;
    else if (g > 1.5)
        g = 1.5;
    return 6.0 * log2(p / a) / g;
}

static double vbv_dqp(struct n265_encoder *e, int cls, double cplx,
                      double icplx, double q_code, double *pred,
                      double *allow)
{
    n265_rc *rc = &e->rc;
    if (rc->szm == 1)
        return vbv_dqp_szm(e, cls, cplx, icplx, q_code, pred, allow);
    /* N265_VBV_OWN (item 37): price at the class's own measured scale and
     * not at the buffer's pessimistic one. 1: the window behind the frame;
     * 2: the frame as well. */
    double p = (rc->vown >= 2 ? rc_oweight(rc, cls, cplx, icplx)
                              : rc_vweight(rc, cls, cplx, icplx)) /
               qscale_of_qp(q_code);
    double fut[LA_MAX];
    int nf = 0;
    int nview = n265_la_view(&e->la);
    for (int i = 1; i < nview && nf < LA_MAX; i++) {
        int k = n265_rc_future_idr(e, i) ? RC_I : cadence_class(e, i);
        /* the window is priced at the ALLOCATOR's scale and not at the
         * pessimistic one. The two estimators answer different questions:
         * "can this frame be paid for NOW" wants the worse of them, and
         * being wrong high there costs one quantizer step; "how much must
         * be saved for the frames behind it" wants the mean, and being
         * wrong high there reserves the whole buffer against frames that
         * do not exist. On sintel_720p's black opening the pessimistic
         * scale prices twenty queued frames at 200 kbit each against an
         * 826 kbit buffer, the allowance floors, and thirteen frames are
         * coded at QP 51 to save for frames that cost 312 bits. */
        fut[nf++] = (rc->vown ? rc_oweight(rc, k, n265_rc_cplx(e, i, k),
                                           n265_rc_cplx(e, i, RC_I))
                              : rc_vweight(rc, k, n265_rc_cplx(e, i, k),
                                           n265_rc_cplx(e, i, RC_I))) /
                    qscale_of_qp(q_code);
    }
    double a = n265_vbv_allow(&e->vbv, fut, nf);
    *pred = p;
    *allow = a;
    double d = p > a ? 6.0 * log2(p / a) : 0.0;
    /* 3: the plan and the guard apart. The frame and its window are priced
     * at the class's own scale against the window's allowance - a
     * trajectory, which wants the mean - and the frame alone at the
     * pessimistic scale against what the buffer holds now - the underflow
     * guard, which wants the bad side. The larger raise wins. */
    if (rc->vown >= 3) {
        double pg = rc_vweight(rc, cls, cplx, icplx) / qscale_of_qp(q_code);
        double ag = n265_vbv_allow(&e->vbv, NULL, 0);
        double dg = pg > ag ? 6.0 * log2(pg / ag) : 0.0;
        if (dg > d) {
            d = dg;
            *pred = pg;
            *allow = ag;
        }
    }
    if (!(d > 0.0))
        return 0.0;
    e->vbv.clamped++;
    return d;
}

/* The same clamp under CRF, where there is no loop to fold it into: the
 * rate factor stands as given and the buffer is the only thing that may
 * move it, which is exactly x265's capped CRF. `base` is what the encoder
 * has assembled so far - the CRF, the operating-point shift, the shot plan
 * and the cu-tree frame term - because that is the number n265_rc_done
 * learns the class scales against, and the prediction has to be made in the
 * units the observation is taken in. The answer only ever goes UP: capped
 * CRF never spends a fuller buffer on a finer quantizer, or the cap would
 * be a target. */
double n265_vbv_crf_base(struct n265_encoder *e, int cls, double cplx,
                         double icplx, double base, double *pred,
                         double *allow, double *dqp)
{
    double d = vbv_dqp(e, cls, cplx, icplx, base, pred, allow);
    /* N265_CCRF_GUARD (item 47): the frame alone, priced at the guard's
     * margin over the model, against what the buffer holds now. Under
     * CRF the buffer runs near empty for as long as the content wants
     * more than the cap, and there a frame the model reads 1.2-1.7x low
     * (a B picture behind a cut's I frame) is one the brake cannot hold:
     * its +12 per CTU ran out on D5v's display 422 at 0.5 s. The window
     * is left at the mean; only the frame that would empty the buffer
     * pays the margin. */
    if (e->rc.ccrf_guard > 1.0) {
        n265_rc *rc = &e->rc;
        double pg = rc_vweight(rc, cls, cplx, icplx) / qscale_of_qp(base) *
                    rc->ccrf_guard;
        double ag = n265_vbv_allow(&e->vbv, NULL, 0);
        double dg = pg > ag ? 6.0 * log2(pg / ag) : 0.0;
        if (dg > d) {
            if (!(d > 0.0))
                e->vbv.clamped++;
            d = dg;
            *pred = pg;
            *allow = ag;
        }
    }
    /* `d` is never negative, so the only bound that can bite is the coarse
     * end - and a base already below N265_QP_MIN is a Main10 operating
     * point the ordinary QP clip handles, not something for a buffer to
     * have an opinion about. */
    double q = base + d;
    /* item 47: where no quantizer makes the plan, the skip holds the
     * frame to an allowance, and that one is solved with the window
     * priced at the QP the frames will really get near the top, 51, and
     * not at this frame's base: priced at the base, a window that is
     * over the cap reserves against frames several times dearer than
     * they will be coded, and the skip spent D7 bbb's 0.5 s cell 1.5
     * VMAF-NEG on that. */
    e->rc.ccrf_sat_allow = 0.0;
    if (q > 51.0) {
        q = 51.0;
        /* ...and only once every inter class has been observed a few
         * times and the I class once: before that the window's prices
         * are the corpus priors, and on D7 bbb's opening they had the
         * skip take the first P picture down to 3.6 kbit with no
         * underflow coming */
        int warm = e->rc.scale_n[RC_I] >= 3.0;
        for (int c = RC_P; c < RC_CLASSES; c++)
            if (e->rc.scale_n[c] < 2.0 + e->rc.ccrf_skwarm)
                warm = 0;
        if (e->rc.ccrf_skip && warm) {
            double p51, a51;
            int keep = e->vbv.clamped;
            vbv_dqp(e, cls, cplx, icplx, e->rc.ccrf_satq, &p51, &a51);
            e->vbv.clamped = keep;
            e->rc.ccrf_sat_allow = a51;
        }
    }
    *dqp = q - base;
    return q;
}

/* ---- live control's large RATE step (contract v1.3, rule (b)) ----
 *
 * The bits-versus-quantizer law the operating point is written in,
 * bits x qscale constant, holds over a few QP and not over a decade of
 * rate: 10x fewer bits takes 10-13 QP on the corpus, not the law's 20.
 * So a large step moves the point by two thirds of the law's answer and
 * then reads the slope off the coded frames: whole groups of an anchor
 * and its B frames before the step against as many after, a quarter
 * second at least each side, twice at most. */

static int ls_anchor(int cls)
{
    return cls == RC_I || cls == RC_P;
}

static long ls_lo(const n265_rc *rc)
{
    return rc->hist_cnt > RC_HIST ? rc->hist_cnt - RC_HIST : 0;
}

/* mean bits and base of [p, end], intra frames left out */
static int ls_group(const n265_rc *rc, long p, long end, double *b, double *q)
{
    double sb = 0.0, sq = 0.0;
    int n = 0;
    for (long i = p; i <= end; i++) {
        const struct n265_rc_obs *o = &rc->hist[i % RC_HIST];
        if (o->cls == RC_I)
            continue;
        sb += o->bits;
        sq += o->q;
        n++;
    }
    if (n == 0)
        return 0;
    *b = sb / n;
    *q = sq / n;
    return 1;
}

static int ls_cut(const n265_rc *rc, long lo, long hi)
{
    for (long i = lo; i <= hi; i++)
        if (rc->hist[i % RC_HIST].cut)
            return 1;
    return 0;
}

/* the operating point, which the base is solved from, by dq QP */
static void ls_scale(n265_rc *rc, double dq)
{
    double k = exp2(dq / 6.0);
    rc->cplxr *= k;
    rc->cplxr_s *= k;
    rc->ctf_x *= k;
    rc->ctf_c *= k;
    rc->copen_bits *= k;
    rc->copen_x *= k;
    rc->copen_c *= k;
}

static void ls_end(n265_rc *rc)
{
    int n = (int)lround(rc->fps);
    rc->ls_mem_n = n < 1 ? 1 : n;
    rc->ls_mem_end = rc->hist_cnt + 2L * rc->ls_mem_n;
    rc->ls_state = 2;
}

double n265_rc_rate_step(struct n265_encoder *e, double r, int take)
{
    n265_rc *rc = &e->rc;
    double law = -6.0 * log2(r);
    if (!(r < 1.0 / 3.0 || r > 3.0) || !rc->ls_on || !take) {
        rc->ls_state = 0;
        return law;
    }
    rc->ls_nref = 0;
    long land = rc->hist_cnt;
    int s = e->bframes + 1;
    if (rc->pend_n > s)
        s = (int)rc->pend_n;
    rc->ls_s = s < 40 ? s : 40;
    int m = (int)lround(0.25 * rc->fps);
    rc->ls_m = m > rc->ls_s ? m : rc->ls_s;
    /* group A: whole groups back from the last one whose last frame is s
     * frames or more before the landing (inclusive), until it holds
     * ls_m frames */
    long anc[RC_HIST];
    int na = 0;
    for (long i = ls_lo(rc); i < land; i++)
        if (ls_anchor(rc->hist[i % RC_HIST].cls))
            anc[na++] = i;
    long pa = -1, ea = -1;
    int j1 = -1;
    for (int j = 0; j + 1 < na; j++)
        if (anc[j + 1] - 1 <= land - rc->ls_s)
            j1 = j;
    if (j1 >= 0) {
        int j0 = j1;
        while (j0 > 0 && anc[j1 + 1] - anc[j0] < rc->ls_m)
            j0--;
        if (anc[j1 + 1] - anc[j0] >= rc->ls_m) {
            pa = anc[j0];
            ea = anc[j1 + 1] - 1;
        }
    }
    rc->ls_state = 1;
    /* a group A reaching into the opening (the first N265_LS_OPEN
     * seconds coded, where the point is still the prior's) is missing */
    int have_a = pa >= 0 && pa >= lround(rc->ls_open * rc->fps) &&
                 ls_group(rc, pa, ea, &rc->ls_ba, &rc->ls_qa);
    /* N265_LS_QRAMP: the first move by the base before the step, where
     * bits fall near the law at a low one. 1 reads the operating point,
     * 2 the base group A was coded at (the point where there is none) */
    double a0 = 1.5;
    if (rc->ls_qramp && rc->cplxr > 0.0) {
        double q0 = rc->ls_qramp == 2 && have_a
                        ? rc->ls_qa
                        : qp_of_qscale(rc->cplxr / (rc->bits_per_frame / r));
        double t = (q0 - rc->ls_qlo) / (rc->ls_qhi - rc->ls_qlo);
        a0 = 1.0 + 0.5 * (t < 0.0 ? 0.0 : t > 1.0 ? 1.0 : t);
    }
    double dq = law / a0;
    ls_scale(rc, dq - law);     /* the new budget already moved it by law */
    rc->ls_a = a0;
    if (!have_a)
        ls_end(rc);
    rc->ls_astart = pa;
    rc->ls_bmin = land + rc->ls_s;
    if (rc->trace)
        fprintf(stderr, "RC step r=%.4g a0=%.3f move=%+.3f s=%d A=[%ld,%ld] "
                "b=%.0f q=%.3f\n", r, a0, dq, rc->ls_s, pa, ea, rc->ls_ba,
                rc->ls_qa);
    return dq;
}

/* at an anchor's decision: refit once all of group B is counted */
static void ls_poll(struct n265_encoder *e)
{
    n265_rc *rc = &e->rc;
    long R = rc->hist_cnt;
    long p = -1, n = -1;
    /* group B: whole groups on from the first anchor at ls_bmin until it
     * holds ls_m frames; whole once the anchor after it is reached */
    for (long i = rc->ls_bmin > ls_lo(rc) ? rc->ls_bmin : ls_lo(rc); i < R;
         i++) {
        if (!ls_anchor(rc->hist[i % RC_HIST].cls))
            continue;
        if (p < 0) {
            p = i;
        } else if (i - p >= rc->ls_m) {
            n = i;
            break;
        }
    }
    if (p < 0)
        return;
    if (n < 0) {
        /* this anchor ends it only if it is the next frame */
        if (rc->pend_n > 0 || R - p < rc->ls_m)
            return;
        n = R;
    }
    double bb, qb;
    if (rc->ls_astart < ls_lo(rc) || !ls_group(rc, p, n - 1, &bb, &qb) ||
        ls_cut(rc, rc->ls_astart, n - 1)) {
        ls_end(rc);
        return;
    }
    /* a slope outside [1, 3] is noise between unlike groups, not a
     * reading: the last one stands */
    double dqq = qb - rc->ls_qa;
    if (fabs(dqq) >= 3.0) {
        double a = -6.0 * log2(bb / rc->ls_ba) / dqq;
        if (a >= rc->ls_amin && a <= 3.0)
            rc->ls_a = a;
    }
    double dq = 6.0 / rc->ls_a * log2(bb / rc->bits_per_frame);
    ls_scale(rc, dq);
    if (!rc->fbstep)
        rc->rf += dq;
    if (rc->trace)
        fprintf(stderr, "RC refit %d B=[%ld,%ld] b=%.0f q=%.3f a=%.3f "
                "move=%+.3f\n", rc->ls_nref + 1, p, n - 1, bb, qb, rc->ls_a,
                dq);
    if (++rc->ls_nref == 2) {
        ls_end(rc);
        return;
    }
    rc->ls_astart = p;
    rc->ls_qa = qb;
    rc->ls_ba = bb;
    rc->ls_bmin = R + rc->pend_n + rc->ls_s;
}

void n265_rc_note(struct n265_encoder *e, int cls, double rf_used,
                  double ctf, double bits, int cut)
{
    n265_rc *rc = &e->rc;
    struct n265_rc_obs *o = &rc->hist[rc->hist_cnt % RC_HIST];
    o->cls = cls;
    o->cut = cut;
    o->q = rf_used - (rc->ctkeep ? ctf : 0.0);
    o->bits = bits;
    rc->hist_cnt++;
    if (rc->ls_state != 2)
        return;
    if (rc->hist_cnt > rc->ls_mem_end) {
        rc->ls_state = 0;
        return;
    }
    /* short memory: the long means and the balance follow the last second */
    double m = (double)rc->ls_mem_n;
    if (rc->cplxr_n > m)
        rc->cplxr_n = m;
    if (rc->cplxr_sn > m)
        rc->cplxr_sn = m;
    if (rc->ctf_n > m)
        rc->ctf_n = m;
    if (rc->w_n > m)
        rc->w_n = m;
    double k = 1.0 - exp(-1.0 / m);
    double seen = rc->total_bits + rc->pend_bits;
    double owed = rc->bits_per_frame * ((double)rc->n_coded + rc->pend_n);
    double pbal = rc->planned_bits - seen;
    rc->total_bits += (owed - seen) * k;
    rc->planned_bits = rc->total_bits + rc->pend_bits + pbal * (1.0 - k);
}

double n265_rc_next(struct n265_encoder *e, int cls, double cplx, double ctf)
{
    n265_rc *rc = &e->rc;

    /* The frame-COUNT mean of the term: the first form of the level below,
     * kept as the arm it was refused as. It leaves a third of the defect in
     * place, because the term is ZERO on a leaf B - nothing propagates into a
     * frame nothing references - and several QP negative on the frames that
     * carry the file, so a mean over frames is dragged toward zero by the
     * frames whose bits it does not describe: sita_toon_720p reads 0.979 /
     * 0.982 / 0.983 / 0.983 here against the weighted level's numbers, with
     * the feedback still pinned to its rail for eighty frames. */
    if (rc->ctf_comp == 3) {
        rc->ctf_avg += (ctf - rc->ctf_avg) / (rc->ctf_lvl_n + 1.0);
        if (rc->ctf_lvl_n < 1000.0)
            rc->ctf_lvl_n += 1.0;
    }

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
            /* N265_VBR_OPEN (item 32, step 3): under a buffer the opening
             * is solved on the size model's corpus prior instead, so the
             * window's lookahead costs are priced on a curve fitted to
             * what frames of that cost really spend. The allocator's
             * prior compresses cost by qcomp, which reads a light opening
             * as dearer than it is and a heavy one as cheaper: Meridian
             * 720p opened 8 QP coarse of where its loop settled, Sol
             * Levante 6 QP fine. */
            if (rc->vopen) {
                double S = 0.0;
                int m = 0;
                for (int i = 0; i < nview; i++) {
                    int k = i == 0 ? cls : cadence_class(e, i);
                    if (k == RC_I)
                        continue;
                    double c = n265_rc_cplx(e, i, k) / rc->nblk;
                    S += rc->nblk * exp2(rc->op_a[k] +
                                         rc->op_b[k] * log2(c > 1.0 ? c : 1.0));
                    m++;
                }
                if (m == 0) {
                    double c = cplx / rc->nblk;
                    S = rc->nblk * exp2(rc->op_a[cls] +
                                        rc->op_b[cls] * log2(c > 1.0 ? c : 1.0));
                    m = 1;
                }
                if (S > 0.0 && rc->op_g < 0.0)
                    rf = 6.0 * log2(rc->bits_per_frame * m / S) / rc->op_g;
            }
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
        /* The term's LEVEL is seeded at zero and at a weight of ONE frame.
         * It is inert at the shipping default - the term comes off whole, so
         * nothing reads the level - and it is the arms that made both halves
         * of this choice. Zero, rather than this frame's own term: seeding
         * from frame 0 cancels the term exactly on the opening IDR, which is
         * the frame carrying the most negative term in the clip and the one
         * the other 119 all reference (on akiyo_cif that codes the IDR at
         * 28.9 instead of 22.6, 33k bits instead of 59k of a 233k file, and
         * 11 points of ABR BD-rate). One frame, rather than the window's n:
         * a level is what one coded frame is genuinely evidence about, which
         * is the argument N265_ABR_LVL already makes for the class scales,
         * and at the window's weight the opening runs twenty frames at a
         * level it has the evidence to correct - 30% to 80% of opening error.
         */
        rc->ctf_x = rc->ctf_c = rc->cplxr;
        rc->ctf_n = 1.0;
        rc->cplxr_n = rc->cplxr_seed > 0.0 ? rc->cplxr_seed
                                           : (n > 0 ? (double)n : 1.0);
        if (rc->cplxr_n > rc->cplxr_mem)
            rc->cplxr_n = rc->cplxr_mem;
        if (rc->trace)
            fprintf(stderr, "RC open window=%d bpf=%.0f rf0=%.3f\n",
                    n, rc->bits_per_frame, rc->rf);
    }
    if (rc->ls_state == 1 && (cls == RC_I || cls == RC_P))
        ls_poll(e);
    if (rc->ls_state == 1 && rc->ls_cap > 0.0) {
        double lim = rc->ls_cap * rc->bits_per_frame * rc->fps;
        double owed = rc->bits_per_frame * ((double)rc->n_coded + rc->pend_n);
        double seen = rc->total_bits + rc->pend_bits;
        if (owed - seen > lim)
            rc->total_bits = owed - lim - rc->pend_bits;
        else if (seen - owed > lim)
            rc->total_bits = owed + lim - rc->pend_bits;
    }

    /* the term's measured LEVEL, in QP: `-6 log2` of the file-weighted mean of
     * `2^(-term/6)` (rc.h). Nothing in the shipped path rides on it - the
     * default takes the whole term off - and it is what the kept arms are
     * written in. It is also the only estimate the loop has of a FUTURE
     * frame's term, which is what the derived arm's horizon floor prices with
     * below. */
    double lvl_now = rc->ctf_x > 0.0 && rc->ctf_c > 0.0
                         ? -6.0 * log2(rc->ctf_x / rc->ctf_c) : 0.0;
    /* The intra offset, and the gain the PLAN is written at.
     *
     * The frame will be coded `ioff` QP finer than the rate factor this loop
     * answers with, so it will cost `2^(ioff/6)` times the bits the weight
     * would otherwise predict. Multiplying the weight by exactly that is what
     * "the allocator plans for it" means, and it is the whole difference
     * between this and the cu-tree frame term: `w` is what prices the fair
     * share T, the TCAP horizon and (through n265_rc_done) the measured
     * operating point, so all three see the discount before it is spent. The
     * base itself does not move - `w / T` is `w_avg / bits_per_frame`
     * identically, and the gain cancels between them - which is the statement
     * that the bonus is paid out of the OTHER frames' shares and not out of
     * the loop's control of the file size. */
    /* THE SECOND PASS, and the whole of what it moves in here.
     *
     * `plan_ok` says a whole-file plan describes this frame (rc.h). Where
     * it does, two quantities come from the plan instead of from the
     * model: the frame's target, which is its own planned bits rather than
     * a fair share of a flat budget, and the base, which is the quantizer
     * the closed-form solve chose for it rather than the measured
     * operating point. Everything else in this function runs unchanged,
     * including the weight and the running means - they cost nothing, the
     * trace reads them, and the VBV's predictor is learned off the same
     * books either way.
     *
     * `N265_ABR_IOFF` is inert under a plan and that is deliberate: the
     * intra frames' share of the file is not modeled in the second pass,
     * it is measured, so an intra discount the allocator plans for is a
     * correction to an estimate that is no longer being made. */
    int twop = rc->plan_ok;
    double ioff = twop ? 0.0 : rc_ioff(rc, cls, ctf);
    double w = rc_weight(rc, cls, cplx) * exp2(ioff / 6.0);
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

    double T = twop ? rc->plan_bits
                    : rc->bits_per_frame * w / w_avg;  /* the fair share */
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
    double seen = rc->total_bits +
                  (rc->pend_use_est && rc->pend_n > 0 ? rc->pend_est
                                                      : rc->pend_bits);
    double nseen = (double)rc->n_coded + (double)rc->pend_n;
    /* Under a plan BOTH terms read the error against the PLAN, and this is
     * the one place where `N265_ABR_PLANERR`'s refused shape is the right
     * one. The flat budget is the reference in one pass because it is the
     * only thing the loop knows whose zero is the file landing on its size;
     * in two passes the plan is that thing - it sums to the target by
     * construction - and it is the reference that does NOT fight the
     * allocator, because the deviation from flat is the allocation. Aiming
     * the feedback at the flat budget here would take back, over six
     * frames, exactly what the whole-file solve just handed a hard shot.
     *
     * `planned_bits` already carries this frame's own T (added above), and
     * the error is over the frames ALREADY accounted - the same nseen the
     * budget form counts - so this frame's plan comes back off. */
    double plan_seen = rc->planned_bits - T;
    double err = seen - (twop ? plan_seen
                              : (rc->planerr ? rc->planned_bits
                                             : rc->bits_per_frame * nseen));
    double errb = seen - (twop ? plan_seen : rc->bits_per_frame * nseen);
    /* N265_VBR_CTLPAY: the balance a RATE step carries over is repaid at
     * `ctlpay` of the new rate. Answered by the feedback, it was repaid in
     * a second or two: foreman's step back up landed 13-22% under. */
    if (rc->carry != 0.0 && !twop) {
        double left = fabs(rc->carry) -
                      rc->ctlpay * rc->bits_per_frame * (nseen - rc->step_n0);
        if (left > 0.0)
            errb -= rc->carry > 0.0 ? left : -left;
        else
            rc->carry = 0.0;
    }
    /* N265_VBR_CTLSEG: the slow window and the deficit rail count from the
     * step. Counted from frame 0, chimera 720p's step back up never won
     * back a black stretch and landed 11% under. */
    double nseg = rc->stepped && rc->ctlseg ? nseen - rc->step_n0 : nseen;
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
     * between two IDRs on sita_flat cost 200 bits at any quantizer, so they
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
    if (rc->tcap && twop) {
        /* The same reservation, made from the plan instead of from a
         * cadence guess. In one pass the window has to be TYPED before it
         * can be priced - the frames behind this one have not been through
         * the typing walk - and the guess is what N265_ABR_TCAP is written
         * around. In two passes there is nothing to guess: every queued
         * frame's planned bits are already on the table, intra frames
         * included, at their real coding type. */
        double tmax = T;
        int nview = n265_la_view(&e->la);
        for (int i = 1; i < nview; i++) {
            const n265_2p_frame *pf = n265_2p_get(&e->tp,
                                                  e->la.f[i].display_idx);
            if (pf && pf->plan_bits > tmax)
                tmax = pf->plan_bits;
        }
        if (tmax > pbits)
            pbits = tmax;
    } else if (rc->tcap) {
        double tmax = T;
        int nview = n265_la_view(&e->la);
        for (int i = 1; i < nview; i++) {
            int k = n265_rc_future_idr(e, i) ? RC_I : cadence_class(e, i);
            /* the future intra frame's plan carries its offset too, priced at
             * the measured level for the derived arm - the loop has no other
             * estimate of a term it has not computed yet, and the floor this
             * feeds is a reservation, not a payment */
            double ti = rc->bits_per_frame *
                        rc_weight(rc, k, n265_rc_cplx(e, i, k)) *
                        exp2(rc_ioff(rc, k, lvl_now) / 6.0) / w_avg;
            if (ti > tmax)
                tmax = ti;
        }
        if (tmax > pbits)
            pbits = tmax;
    }
    /* N265_LS_RAIL: a large step's feedback may reach further while
     * the refit and its short memory run */
    int ls_wide = rc->ls_rail > 0.0 && rc->ls_state != 0;
    double chi = ls_wide ? exp2(rc->ls_rail / 6.0) : 2.0;
    double corr = 1.0 + err / pbits;
    if (corr < 1.0 / chi)
        corr = 1.0 / chi;
    else if (corr > chi)
        corr = chi;
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
     * is simply wrong about can otherwise drag the quantizer anywhere: the
     * black frames that open sintel_720p cost 200 bits at any QP, so the
     * loop reads eleven frames of unspent budget and walks the rate factor
     * down twenty steps chasing bits that do not exist. The bound is what
     * keeps the recovery from being worse than the deficit. */
    double fb = 6.0 * log2(corr) + rc->rf_bias;
    /* N265_VBR_FBW (phase 14 item 37): the SLOW feedback. The horizon loop
     * above answers the cumulative error over six frames of budget, a
     * quarter of a second, so a VBR encode lands on its target at every
     * instant and spends a two-minute title's bits wherever its next six
     * frames happen to be. Here the same error is answered over a window
     * that grows with the square root of the coded duration: an early
     * error is answered within seconds, the same error late in a long
     * encode is a small share of what has been spent and is answered
     * gently. The level the loop rides is the measured operating point
     * below, which moves slowly on its own; the integral is not used.
     * Under a VBV the window may be capped to a share of the buffer
     * (N265_VBR_OVW), widened past six seconds in proportion to the time
     * coded, so the buffer, not the window, is what bounds a local
     * overspend early on. */
    if (rc->fbw > 0.0 && !twop) {
        double bps = rc->bits_per_frame * rc->fps;
        double t = rc->fps > 0.0 ? nseg / rc->fps : 0.0;
        double win = rc->fbw * bps * sqrt(t > rc->fbt0 ? t : rc->fbt0);
        if (rc->ovw > 0.0 && e->vbv.on) {
            double cap = rc->ovw * e->vbv.bufsize;
            if (t > 6.0)
                cap *= t / 6.0;
            if (win > cap)
                win = cap;
        }
        fb = win > 0.0 ? (errb > 0.0 ? rc->fbgup : rc->fbg) * errb / win
                       : 0.0;
        rc->rf_bias = 0.0;
    }
    /* ...and under a plan the whole feedback is scaled before the rails,
     * not after: what is being bounded is how far the loop may depart from
     * a plan it has good reason to believe. The integrator keeps winding
     * at its own gain, so a standing model error still gets closed - it
     * just takes 1/gain as long to spend its rail. */
    if (twop)
        fb *= rc->tp_gain;
    /* N265_VBR_FBTRK (item 48): the finer-side rail widens back toward the
     * coarse rail's width once the file is a real share under its target.
     * At 3 QP a long easy stretch with the buffer full can leave a 4K
     * encode 20% short a minute in, and the rail then spends the rest of
     * the file walking back: Chimera 2160p's top rung landed 13% under.
     * Below `fbtrk` of deficit the rail is item 38's; at `fbtrk + fbtrk_w`
     * and beyond it is `fbtrk_max`. Only after the opening's two seconds,
     * whose ratio is noise. 0 is item 38's rail at every deficit. */
    double fine = rc->fbfine;
    if (rc->fbtrk > 0.0 && rc->fbtrk_max > fine && rc->fps > 0.0 &&
        nseg > 2.0 * rc->fps && rc->bits_per_frame > 0.0) {
        double def = -errb / (rc->bits_per_frame * nseg);
        double s = (def - rc->fbtrk) / (rc->fbtrk_w > 0.0 ? rc->fbtrk_w : 1e-9);
        s = s < 0.0 ? 0.0 : s > 1.0 ? 1.0 : s;
        fine += s * (rc->fbtrk_max - fine);
    }
    /* Item 50's hold. A run of floor-complexity frames (black, a still
     * card) can't spend a surplus at any QP, so the deficit grows over it
     * and the finer rail opens to 6 QP. The first pictures after the
     * stretch then code 6 QP finer than the content's level, priced by a
     * size model the stretch has walked down: stem2's still minute ends
     * with B pictures predicted at 50 kbit coding 280-800 kbit, and the
     * buffer underflows. While the deficit point is latched, for
     * `defpt_hold` seconds after the last floor frame, neither the wide
     * rail nor the point's move applies. It reads the frame's own
     * lookahead complexity against the floor N265_ABR_FLATC keeps out of
     * the operating point. */
    if (rc->flatc > 0.0 && cplx < rc->flatc * rc->nblk)
        rc->defpt_nf = 0.0;
    else
        rc->defpt_nf += 1.0;
    int defpt_held = rc->defpt_on && rc->defpt_hold > 0.0 &&
                     rc->defpt_nf < rc->defpt_hold * rc->fps;
    if (defpt_held && fine > rc->fbfine)
        fine = rc->fbfine;
    /* ...and toward the whole of it while the deficit point (item 50)
     * holds, at the same pace as the point's own move */
    if (rc->defpt_on && !defpt_held && rc->defpt_rail &&
        rc->fbtrk_max > fine) {
        double r = rc->defpt_railq + (rc->defpt_slew > 0.0 && rc->fps > 0.0
                                      ? rc->defpt_slew / rc->fps : 1e9);
        if (r < fine)
            r = fine;
        if (r > rc->fbtrk_max)
            r = rc->fbtrk_max;
        rc->defpt_railq = r;
        fine = r;
    } else {
        rc->defpt_railq = fine;
    }
    double up = rc->fbup;
    if (ls_wide) {
        up = up > rc->ls_rail ? up : rc->ls_rail;
        fine = fine > rc->ls_rail ? fine : rc->ls_rail;
    }
    if (fb > up)
        fb = up;
    else if (fb < -fine)
        fb = -fine;

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
     *     do not depend on the quantizer at all - 108 of sita_flat's 120
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
     * where the modeled point reads 22.71 against 32.10 on sita_flat.
     *
     * The model is not thereby unemployed: it still fits the opening from
     * the lookahead window, it still hands out the fair shares, and its
     * class scales are what the window is priced at. It no longer sets the
     * operating point. `N265_ABR_MEAS=0` gives that back. */
    double base = twop ? rc->plan_qp : qp_of_qscale(w / T);
    if (!twop && rc->meas && rc->cplxr > 0.0 && rc->bits_per_frame > 0.0) {
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
    /* N265_VBR_DEFPT (item 50): the whole-encode point (N265_VBR_CMEM) is
     * the right level while the content keeps its mix, and a stale one
     * when a title gets easier for minutes on end. stem2's second half
     * codes at a point its harder first half set, the feedback sits on
     * its 3 QP rail with the buffer full the whole way, and the file ends
     * 8-12% short. Past `defpt` of deficit, after `defpt_t0` seconds, the
     * loop latches: the base moves toward the 1000-frame point when that
     * one is finer, in full from `defpt_rel + defpt_w` of deficit and
     * fading to nothing at `defpt_rel`, where the latch lets go. While it
     * holds, the finer-side rail opens to item 48's widest, at the same
     * pace, and both stand aside after a floor-complexity frame (the hold,
     * above). An encode that never runs `defpt` under after `defpt_t0`
     * never latches and is the stream it was. */
    double defpt_dq = 0.0;
    if (rc->defpt > 0.0 && !twop && rc->fps > 0.0 && rc->cplxr_sn > 0.0 &&
        rc->bits_per_frame > 0.0 && nseen > rc->defpt_t0 * rc->fps) {
        double def = -errb / (rc->bits_per_frame * nseen);
        if (def > rc->defpt)
            rc->defpt_on = 1;
        else if (def <= rc->defpt_rel)
            rc->defpt_on = 0;
        double want = 0.0;
        if (rc->defpt_on) {
            double s = (def - rc->defpt_rel) /
                       (rc->defpt_w > 0.0 ? rc->defpt_w : 1e-9);
            double ms = qp_of_qscale(rc->cplxr_s / rc->bits_per_frame);
            if (ms < base)
                want = (s > 1.0 ? 1.0 : s) * (ms - base);
        }
        /* The finer side moves at most `defpt_slew` QP a second and at
         * most `defpt_cap` QP in all; the coarser side at once, so a
         * latch never steps the base in one frame. */
        if (defpt_held)
            want = 0.0;
        if (rc->defpt_cap > 0.0 && want < -rc->defpt_cap)
            want = -rc->defpt_cap;
        if (rc->defpt_slew > 0.0) {
            double lo = rc->defpt_dq - rc->defpt_slew / rc->fps;
            if (want < lo)
                want = lo;
        }
        rc->defpt_dq = want;
        defpt_dq = want;
        base += defpt_dq;
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
     * IDRs cost 200 bits at any quantizer, the loop walks six steps down
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
     * artifact of the FLAT budget - on sita_flat the loop reads an 88k
     * deficit at the third IDR, spends 4.3 QP of rate factor closing it, and
     * ends the clip 21% OVER. A surplus always has somewhere cheaper to go,
     * because nine frames in ten are inter. N265_ABR_IBRAKE=0 restores the
     * symmetric term. */
    if (rc->ibrake && cls == RC_I && fb < 0.0)
        fb = 0.0;
    /* N265_VBR_SPEND (item 38): a surplus is spent where the content is
     * hard enough to use it. Below the running mean of the per-block
     * complexity the finer-side feedback is scaled by the ratio to that
     * mean, raised to this exponent; at or above it the frame takes it
     * whole. An easy stretch then banks its surplus instead of spending it
     * on pictures whose quality has already saturated. */
    /* N265_VBR_SPENDFILL (item 38): and only while the buffer has the room
     * to pay for it. The finer-side feedback is scaled from whole at
     * `spend_hi` of the buffer down to none at this share, so a surplus is
     * not spent into a stretch the buffer then has to clamp. */
    if (rc->spend_lo > 0.0 && fb < 0.0 && e->vbv.on && e->vbv.bufsize > 0.0) {
        double f = e->vbv.fill / e->vbv.bufsize;
        double s = (f - rc->spend_lo) / (rc->spend_hi - rc->spend_lo);
        fb *= s < 0.0 ? 0.0 : s > 1.0 ? 1.0 : s;
    }
    if (rc->fbspend > 0.0 && cls != RC_I) {
        double lc = log(cplx / rc->nblk > 1.0 ? cplx / rc->nblk : 1.0);
        if (rc->lcx_n > 0.0 && fb < 0.0 && lc < rc->lcx_avg)
            fb *= exp(rc->fbspend * (lc - rc->lcx_avg));
        rc->lcx_avg += (lc - rc->lcx_avg) / (rc->lcx_n + 1.0);
        if (rc->lcx_n < rc->w_mem)
            rc->lcx_n += 1.0;
    }
    /* N265_VBR_DEFPT_SPEND (item 50): what the latch spends goes where
     * the content can use it. Below the running mean of the per-block
     * complexity, the latch's finer move and the finer-side feedback are
     * scaled by the ratio to that mean, raised to this exponent. The mean
     * runs on every inter frame; it is read only while latched. */
    if (rc->defpt_spend > 0.0 && cls != RC_I) {
        double lc = log(cplx / rc->nblk > 1.0 ? cplx / rc->nblk : 1.0);
        if (rc->defpt_on && rc->dlc_n > 0.0 && lc < rc->dlc_avg) {
            double wgt = exp(rc->defpt_spend * (lc - rc->dlc_avg));
            if (fb < 0.0)
                fb *= wgt;
            base -= defpt_dq * (1.0 - wgt);
            defpt_dq *= wgt;
        }
        rc->dlc_avg += (lc - rc->dlc_avg) / (rc->dlc_n + 1.0);
        if (rc->dlc_n < rc->cplxr_smem)
            rc->dlc_n += 1.0;
    }
    /* The cu-tree frame term, taken off the answer.
     *
     * Everything above solves for the BASE the frame is coded at: the
     * operating point is measured as `bits x qscale(that base)`, the opening
     * fit prices the window at it, and the feedback closes the error against
     * it. The encoder then adds the cu-tree frame term to what this returns
     * (encoder.c, dispatch_frame), so without this line the loop's answer and
     * the frame's base are two different numbers and the difference is a pure
     * OPEN-LOOP error. It is not small and it is not zero-mean: under ABR the
     * per-CTU field is CENTERED (la.c centers unless crf_on) and the term is
     * then the field's whole frame mean, which on animation is -4 to -7.5 QP.
     * sita_toon_720p opened at a fitted 20.60, coded its first IDR at 13.12 -
     * 23 frames of budget on one frame - and then sat on its +6 feedback rail
     * for the remaining 119 frames and still landed 48% over. (2026-09-12.
     * The term reached ABR with the phase 7 frame term in d59e23f and grew
     * again when --rc-lookahead went to 20 in 3d3d261, which deepens the
     * propagation the mean is taken over. No item gate between the two ran the
     * ABR leg.)
     *
     * The default takes the WHOLE term off, so the frame is coded at the rate
     * factor this loop answered with and the ABR base is the one the ABR round
     * measured and shipped. That is a refusal as much as a fix: the term's
     * cross-frame allocation is worth real quality under ABR - main reads 6.6
     * points more of CIF-7 ABR BD-rate with it - and it cannot be had at this
     * loop's hands, because every form that keeps it (the arms below) either
     * keeps the open-loop error or trades the same quality back through a
     * noisier QP trajectory. Under CRF the term stays exactly as phase 7
     * shipped it; what is refused is an ABR base nothing nets.
     *
     * `N265_ABR_CTF`, all four arms measured on this item:
     *
     *   0 - the pre-fix loop: the term lands on the base and nothing in here
     *       has heard of it. The regression, kept as the arm that reproduces
     *       it: sita_toon_720p 1.485 of target, ducks_720p 1.105, sita_flat
     *       2.349, a 43% opening median over the corpus.
     *   1 - the default, above.
     *   2 - only the term's LEVEL comes off, each frame keeping its deviation:
     *       the allocation survives and the standing error does not. Refused
     *       on the measurement, at both level seeds: 1.4% to 3.1% of rate
     *       against the default's 0.7% to 1.2%, for a CIF-7 ABR BD of -3.75
     *       against -4.56 (-5.86 seeding the level at frame 0's own term,
     *       which cancels the opening IDR's allocation) - a spread inside this
     *       instrument's own noise on four ABR points. What the deviation buys
     *       is a QP trajectory 13 QP wide against 10.7, because the allocator
     *       still prices the frame at `w` while the encoder codes it at
     *       `rf + ctf`; pricing the plan at `w x 2^(-ctf/6)` too was built and
     *       is worse on both legs, because N265_ABR_TCAP then floors the
     *       horizon at the IDR's own inflated plan.
     *   3 - the level as a frame-COUNT mean rather than a file-weighted one.
     *       Refused twice over: the term is exactly zero on a leaf B (nothing
     *       propagates into a frame nothing references), so a leaf costing 300
     *       bits votes as loudly as the anchor carrying the file and the mean
     *       reads a third of the level. */
    double ctf_off = 0.0;
    if (rc->ctkeep)
        ctf_off = 0.0;      /* the term rides the coded base (item 37) */
    else if (rc->ctf_comp == 1)
        ctf_off = ctf;
    else if (rc->ctf_comp == 2)
        ctf_off = lvl_now;
    else if (rc->ctf_comp == 3)
        ctf_off = rc->ctf_avg;
    /* ...and the intra offset, taken off LAST and outside the feedback. It is
     * not a correction and the loop is not chasing it: the plan above was
     * written at `w x 2^(ioff/6)`, so the bits it costs are already inside
     * the fair share, inside the horizon floor and (through n265_rc_done)
     * inside the operating point. N265_ABR_IBRAKE still governs the feedback
     * alone, which is the right split - the loop may not SPEND on an intra
     * frame to chase a surplus, and this is not the loop spending. */
    rf = base + fb - ctf_off - ioff;
    /* --shot-plan under capped VBR: where the feedback and the shot's
     * offset (added after this returns) push the same way, the frame takes
     * the larger of the two, not their sum. Stacked, a finer shot coded
     * while the loop spends a surplus went far past where v1 still pays. */
    if (rc->splan && fb != 0.0 && rc->cur_soff != 0.0 &&
        (fb < 0.0) == (rc->cur_soff < 0.0))
        rf -= fabs(fb) < fabs(rc->cur_soff) ? fb : rc->cur_soff;
    double rf_want = rf;
    if (!rc->fbstep) {
        if (rf > rc->rf + step)
            rf = rc->rf + step;
        else if (rf < rc->rf - step)
            rf = rc->rf - step;
    }
    /* --qpmin / --qpmax narrow this: the loop's answer is bounded where
     * the user bounded it, so the integral winds against a rate factor it
     * can actually reach instead of against a clamp applied behind its
     * back. Absent the flags the pair IS the spec's own range and this is
     * the clip it always was. */
    double rf_lo = (double)N265_QP_MIN, rf_hi = 51.0;
    if (e->qp_bounds) {
        rf_lo = (double)e->qpmin;
        rf_hi = (double)e->qpmax;
    }
    if (rf < rf_lo)
        rf = rf_lo;
    else if (rf > rf_hi)
        rf = rf_hi;
    /* The VBV, LAST and one-sided.
     *
     * Everything above answers the question "what lands the file on its
     * size"; this answers "what the decoder's buffer can pay for at this
     * removal", and the second may only ever make the frame coarser. It is
     * not folded into the feedback for the reason N265_ABR_IOFF is not: the
     * loop is not chasing it, and the bits it does not spend here are a
     * surplus the integral term will find and spend somewhere the buffer
     * can afford - which is the whole point of a buffer.
     *
     * `rf + ctf` and not `rf`: the encoder adds the cu-tree frame term back
     * on to get the base it codes at (encoder.c, dispatch_frame), and the
     * bits model is calibrated in that base. At the shipping default
     * `ctf_off` is the whole term, so this is `base + fb - ioff` either
     * way; with N265_ABR_CTF 0 it is the term the loop never took off.
     *
     * Inert to the bit with --vbv-maxrate unset: `on` is 0 and nothing
     * below this line runs. */
    rc->last_vbv_pred = rc->last_vbv_allow = rc->last_vbv_dqp = 0.0;
    if (e->vbv.on) {
        /* N265_VBR_VLVL (item 49): where the buffer runs near empty for a
         * stretch, the clamp below makes each frame fit on its own, and the
         * frame it raises most is the one predicted dearest: the P anchor.
         * Meridian 720p's hard minutes coded their anchors 16 QP over the
         * leaf B pictures that reference them, at VMAF-NEG 40-50 where our
         * own CRF codes the same bytes at 77. The level is a running mean
         * of what the clamp asks of the loop's answer, laid on every frame
         * before the clamp, so a stretch the buffer can't pay for is paid
         * for by every picture at once and the cascade survives. The clamp
         * still answers what the level doesn't. */
        if (rc->vlvl_a > 0.0) {
            int nclamp = e->vbv.clamped;
            double p0, a0;
            double dq0 = vbv_dqp(e, cls, cplx, rc->cur_icplx,
                                 rf + ctf + rc->cur_soff, &p0, &a0);
            e->vbv.clamped = nclamp;
            rc->vlvl += rc->vlvl_a * (dq0 - rc->vlvl);
            rf += rc->vlvl;
            if (rf > 51.0)
                rf = 51.0;
        }
        if (rc->fillq > 0.0 && rc->fillgoal > 0.0 && e->vbv.bufsize > 0.0) {
            double sh = 1.0 - e->vbv.fill / (rc->fillgoal * e->vbv.bufsize);
            if (sh > 0.0) {
                rf += rc->fillq * (sh > 1.0 ? 1.0 : sh);
                if (rf > 51.0)
                    rf = 51.0;
            }
        }
        /* the clamp's own raise only: the brake and the lag's estimate
         * price the frame as `pred x 2^(-dqp/6)`, and pred is already at
         * the level */
        double rf0 = rf;
        double dq = vbv_dqp(e, cls, cplx, rc->cur_icplx,
                            rf + ctf + rc->cur_soff, &rc->last_vbv_pred,
                            &rc->last_vbv_allow);
        rf += dq;           /* never negative: the clamp is one-sided */
        if (rf > 51.0)
            rf = 51.0;
        rc->last_vbv_dqp = rf - rf0;
    }
    /* what the frame will actually be coded at is what the operating point has
     * to take the offset back out of, so a bound that ate part of the discount
     * hands back only what survived it */
    rc->last_ioff = ioff - (rf - rf_want);
    if (rc->last_ioff < 0.0)
        rc->last_ioff = 0.0;
    else if (rc->last_ioff > ioff)
        rc->last_ioff = ioff;
    rc->rf = rf;
    rc->pend_bits += T;
    rc->pend_n++;
    if (rc->trace)
        fprintf(stderr, "RC n=%ld cls=%d cplx=%.0f w=%.3g wavg=%.3g "
                "T=%.0f corr=%.4f bias=%+.3f fb=%+.3f base=%.3f rf=%.3f "
                "ctf=%+.3f ctflvl=%+.3f ioff=%.3f err=%.0f plan=%d dpt=%+.3f\n",
                rc->n_coded, cls, cplx, w, rc->w_avg, T, corr, rc->rf_bias,
                fb, base, rf, ctf, lvl_now, rc->last_ioff, err, twop,
                defpt_dq);
    return rf;
}

void n265_rc_done(struct n265_encoder *e, int cls, double cplx, double rf_used,
                  double bits, double planned, double ctf, double ioff,
                  double soff)
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
        /* ...and the VBV's, which takes neither the shared level nor a
         * running mean: up at once, down slowly, and only from its own
         * class (rc.h). */
        if (obs > rc->vscale[cls])
            rc->vscale[cls] = obs;
        else
            rc->vscale[cls] += (obs - rc->vscale[cls]) * rc->vdecay;
    }
    /* The measured operating point. Every coded frame counts, intra
     * included, and each counts for the bits it actually carried rather than
     * once: the product summed here is the numerator of the budget solve, so
     * a 200-bit frame moves it by 200 bits' worth and a scene-cut IDR by
     * its own. Its memory is the sequence for the reason WMEM's is - a
     * window tracks the shot - and the running-mean form is w_avg's. */
    /* The measured operating point, with the planned intra offset taken back
     * OUT of the base it is read at.
     *
     * The point is the rate factor that lands the budget: it solves
     * `sum(bits) = N x bpf` where a frame coded at `rf - ioff` costs
     * `X / qscale(rf - ioff)` = `X x 2^(ioff/6) / qscale(rf)`. So the mean
     * this estimator wants is of `bits x qscale(rf)`, not of
     * `bits x qscale(rf - ioff)` - the latter is the SAME number the frame
     * would have produced with no offset at all, which is exactly the failure
     * mode: an operating point blind to the discount reports that nothing has
     * been spent, and the integral term spends the next hundred frames
     * discovering otherwise. `qscale(rf_used + ioff)` is `qscale(rf)`, so this
     * one addition is the whole of "the loop pays for it".
     *
     * The class scale above is NOT corrected the same way, and must not be:
     * `obs` is the raw price of this frame's content at the quantizer it was
     * really coded at, and the offset is put back on at weight-formation time
     * in n265_rc_next. Correcting both would count it twice. */
    /* N265_VBR_CTKEEP (item 37): the frame was coded at the loop's level
     * plus its cu-tree frame term, as a CRF frame is, so the ledger reads
     * it at the level - its coded base less the term - and converges on
     * the term-free level that spends the target. */
    double c = bits * qscale_of_qp(rf_used + ioff - (rc->ctkeep ? ctf : 0.0)
                                   - soff);
    /* the same mean with the frame's own cu-tree term folded in, so the ratio
     * of the two is the term's file-weighted level (rc.h, n265_rc_next). It
     * rides every branch below, hold included, because a level over a
     * different frame set than cplxr's is not a level at all. */
    double cx = c * exp2(-ctf / 6.0);
    /* A frame at the lookahead's complexity floor - black, a still card -
     * costs its few hundred bits of overhead at any quantizer, so `bits x
     * qscale` there measures the quantizer and not the content. Chimera
     * opens on five seconds of black: counted, they walk the operating
     * point from 21.7 to 1.3 and the first picture after the cut is
     * planned at QP 13 (phase 14 item 29). They still count as spending
     * in the budget error below; they are only kept out of the point. */
    int flat = rc->flatc > 0.0 && cplx < rc->flatc * rc->nblk;
    if (flat) {
        /* not evidence */
    } else if (rc->copen && cls == RC_I && rc->n_coded == 0) {
        /* The opening IDR is HELD, not counted.
         *
         * It is forced - every sequence starts on one - so on its own it says
         * nothing about how often intra frames recur, and the mean it would
         * join is a mean over frames that intra frames dominate. Counting one
         * forced frame as 1 in n moves the operating point by the whole of
         * its share: on akiyo the opening IDR is 27% of a 120-frame file, the
         * point moves 2.6 QP coarse, and the clip pays 4 points of ABR
         * BD-rate for an allocation that used to give its one I frame - and
         * so the 119 frames that reference it - the finer quantizer.
         *
         * A SECOND intra frame is the evidence that was missing. When one
         * arrives the held observation is folded in beside it and the point
         * becomes the honest all-frames mean from there on, which is what
         * sita_flat needs and akiyo never asks for. */
        rc->copen_bits = c;
        rc->copen_x = cx;
        rc->copen_c = c;
        rc->copen_held = 1;
    } else {
        if (rc->copen_held && cls == RC_I) {
            rc->cplxr += (rc->copen_bits - rc->cplxr) / (rc->cplxr_n + 1.0);
            if (rc->cplxr_n < rc->cplxr_mem)
                rc->cplxr_n += 1.0;
            rc->ctf_x += (rc->copen_x - rc->ctf_x) / (rc->ctf_n + 1.0);
            rc->ctf_c += (rc->copen_c - rc->ctf_c) / (rc->ctf_n + 1.0);
            if (rc->ctf_n < rc->cplxr_mem)
                rc->ctf_n += 1.0;
            rc->copen_held = 0;
        }
        rc->cplxr += (c - rc->cplxr) / (rc->cplxr_n + 1.0);
        if (rc->cplxr_n < rc->cplxr_mem)
            rc->cplxr_n += 1.0;
        /* the short-memory twin (N265_VBR_DEFPT); read nowhere else */
        rc->cplxr_s += (c - rc->cplxr_s) / (rc->cplxr_sn + 1.0);
        if (rc->cplxr_sn < rc->cplxr_smem)
            rc->cplxr_sn += 1.0;
        rc->ctf_x += (cx - rc->ctf_x) / (rc->ctf_n + 1.0);
        rc->ctf_c += (c - rc->ctf_c) / (rc->ctf_n + 1.0);
        if (rc->ctf_n < rc->cplxr_mem)
            rc->ctf_n += 1.0;
    }
    rc->total_bits += bits;
    rc->n_coded++;
    if (rc->trace)
        fprintf(stderr, "RC done cls=%d bits=%.0f rf=%.3f cplx=%.0f obs=%.4g "
                "scale=%.4g cplxr=%.4g total=%.0f\n",
                cls, bits, rf_used, cplx, obs, rc->scale[cls], rc->cplxr,
                rc->total_bits);
}

/* ---- the size model (rc.h, N265_VBR_SZM) ---- */

int n265_szm_key(int slice_type, int depth, int nonref)
{
    if (slice_type == 2)
        return 0;
    if (slice_type == 1)
        return 1;
    int d = depth < 1 ? 1 : depth > 4 ? 4 : depth;
    return 2 + 2 * (d - 1) + (nonref ? 1 : 0);
}

static int szm_content(const n265_rc *rc, double cplx)
{
    double c = cplx / rc->nblk;
    return c < rc->szm_static ? 0 : c < rc->szm_simple ? 1 : 2;
}

static void szm_x(const n265_rc *rc, double cplx, double q, double x[3])
{
    double c = cplx / rc->nblk;
    x[0] = 1.0;
    x[1] = log2(c > 1.0 ? c : 1.0);
    x[2] = q / 6.0;
}

/* (A + diag(P)) t = b + diag(P) t0, by Gaussian elimination with partial
 * pivoting; returns 0 and leaves t at t0 if the system is singular */
static int szm_solve(const n265_szfit *f, const double P[3],
                     const double t0[3], double t[3])
{
    double M[3][4];
    for (int i = 0; i < 3; i++) {
        for (int j = 0; j < 3; j++)
            M[i][j] = f->A[3 * i + j] + (i == j ? P[i] : 0.0);
        M[i][3] = f->b[i] + P[i] * t0[i];
    }
    for (int c = 0; c < 3; c++) {
        int pr = c;
        for (int r = c + 1; r < 3; r++)
            if (fabs(M[r][c]) > fabs(M[pr][c]))
                pr = r;
        if (!(fabs(M[pr][c]) > 1e-12)) {
            for (int i = 0; i < 3; i++)
                t[i] = t0[i];
            return 0;
        }
        if (pr != c)
            for (int j = 0; j < 4; j++) {
                double tmp = M[c][j]; M[c][j] = M[pr][j]; M[pr][j] = tmp;
            }
        for (int r = 0; r < 3; r++) {
            if (r == c)
                continue;
            double k = M[r][c] / M[c][c];
            for (int j = c; j < 4; j++)
                M[r][j] -= k * M[c][j];
        }
    }
    for (int i = 0; i < 3; i++)
        t[i] = M[i][3] / M[i][i];
    return 1;
}

/* the class fit: a ridge toward the allocator's own model at the class's
 * current scale, so a class with no frames coded yet predicts what the
 * allocator would */
static void szm_parent(const n265_rc *rc, int cls, double t[3])
{
    double t0[3] = { log2(rc->scale[cls]) - log2(0.85) + 2.0, rc->qcomp,
                     -1.0 };
    if (rc->vopen) {        /* step 3: the corpus prior (rc_open) */
        t0[0] = rc->op_a[cls];
        t0[1] = rc->op_b[cls];
        t0[2] = rc->op_g;
    }
    static const double P[3] = { 0.01, 50.0, 50.0 };
    const n265_szfit *f = &rc->szp[cls];
    if (f->n < 1.0) {
        for (int i = 0; i < 3; i++)
            t[i] = t0[i];
        return;
    }
    szm_solve(f, P, t0, t);
}

static const n265_szfit *szm_child(const n265_rc *rc, int key, double cplx)
{
    if (key < 0 || key >= RC_SZK)
        return NULL;
    const n265_szfit *f = &rc->szc[key][szm_content(rc, cplx)];
    return f->n >= 1.0 ? f : NULL;
}

static void szm_theta(const n265_rc *rc, int cls, int key, double cplx,
                      double t[3], double *r2)
{
    double tp[3];
    szm_parent(rc, cls, tp);
    const n265_szfit *f = szm_child(rc, key, cplx);
    *r2 = rc->szp[cls].n >= 1.0 ? rc->szp[cls].r2 : 1.0;
    if (!f) {
        for (int i = 0; i < 3; i++)
            t[i] = tp[i];
        return;
    }
    static const double P[3] = { 0.0, 1.0, 1.0 };
    szm_solve(f, P, tp, t);
    if (f->n >= 4.0)
        *r2 = f->r2;
}

double n265_szm_pred(const n265_rc *rc, int cls, int key, double cplx,
                     double q, double *slope, double *sd)
{
    double t[3], x[3], r2;
    szm_theta(rc, cls, key, cplx, t, &r2);
    szm_x(rc, cplx, q, x);
    double y = t[0] * x[0] + t[1] * x[1] + t[2] * x[2];
    if (y > 40.0)
        y = 40.0;
    if (slope)
        *slope = t[2];
    if (sd)
        *sd = sqrt(r2);
    return rc->nblk * exp2(y);
}

static void szm_fold(n265_szfit *f, double lam, const double x[3], double y,
                     double res)
{
    for (int i = 0; i < 3; i++) {
        for (int j = 0; j < 3; j++)
            f->A[3 * i + j] = lam * f->A[3 * i + j] + x[i] * x[j];
        f->b[i] = lam * f->b[i] + x[i] * y;
    }
    /* the residual is out of sample: the fit's error before it saw the
     * frame, which is what a prediction will carry */
    f->r2 = f->n >= 1.0 ? f->r2 + (1.0 - lam) * (res * res - f->r2)
                        : res * res;
    f->n += 1.0;
}

void n265_szm_add(n265_rc *rc, int cls, int key, double cplx, double q,
                  double bits)
{
    if (!(bits > 0.0) || cls < 0 || cls >= RC_CLASSES)
        return;
    double x[3];
    szm_x(rc, cplx, q, x);
    double y = log2(bits / rc->nblk);
    double tp[3], t[3], r2;
    szm_parent(rc, cls, tp);
    double rp = y - (tp[0] * x[0] + tp[1] * x[1] + tp[2] * x[2]);
    szm_theta(rc, cls, key, cplx, t, &r2);
    double rc_ = y - (t[0] * x[0] + t[1] * x[1] + t[2] * x[2]);
    szm_fold(&rc->szp[cls], rc->szm_lam, x, y, rp);
    if (key >= 0 && key < RC_SZK)
        szm_fold(&rc->szc[key][szm_content(rc, cplx)], rc->szm_lam, x, y,
                 rc_);
}

/* knob warming, on the driver at open; see n265_ctu_warm */
void n265_rc_warm(void)
{
    (void)rc_mvrate();
}
