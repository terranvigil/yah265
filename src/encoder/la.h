/*
 * Lookahead: input queue, lowres statistics, scenecut, cu-tree
 * (docs/lookahead-design.md L0-L2, serial form).
 * Copyright (c) 2026, the yah265 authors. GPL-2.0-or-later; see LICENSE.
 */

#ifndef N265_LA_H
#define N265_LA_H

#include <stdint.h>

#include "common/pixel.h"

/* The queue holds `depth + 1` live frames plus a FREE RUN of recycled
 * slots that a dispatched frame may still be reading: T3/1 leaves frames
 * in flight across API calls and a frame_work aliases its lookahead
 * slot's source planes. n265_la_pop returns a slot to the BACK of that
 * run and n265_la_push takes one off the front, so a slot is not reused
 * until every other free one has been - which is what keeps D3's source
 * stall from firing on the frame that was just dispatched.
 *
 * The run has to be longer than the number of slots the frames in flight
 * hold. That is not K but K rounded up to a mini-GOP, because pops
 * happen at mini-GOP ends: K + bframes bounds it. LA_MAX sizes the VIEW
 * - the frames the decision machinery may read - for the worst case; the
 * slot array itself is `la->cap` entries, allocated at open from the
 * shape the encoder was opened with.
 *
 * The queue's FILL and its VIEW are two different numbers since the shot
 * planner (2026-09-12, phase 7 item 6 step 2). `--shot-window N` makes
 * the queue hold N frames so the planner can see a whole shot, while the
 * view stays `depth + 1` - what `--rc-lookahead` asks for - so cu-tree,
 * b-adapt and the rate control read exactly the window they read at the
 * default, and the only thing the mode moves is the shot's own base-QP
 * offset. At the default fill IS depth, the view IS the whole queue, and
 * `n265_la_view` is `la->count` identically. */
/* The clamp is 32 since 2026-09-12 (phase 7 item 3, D3): the deep-window
 * mode wants `--rc-lookahead 24 --bframes 15`, and 20 is what the
 * streaming default of 12 was sized against. Nothing reaches past 12
 * without asking - the default and the board never do - and raising the
 * clamp moved exactly one shipped value: `--preset slow` asked for 24
 * and had been silently getting 20 ever since it was written, so it now
 * names 20 outright and stays the encoder it was measured as. */
#define LA_DEPTH_MAX 32     /* --rc-lookahead clamp (20 before 2026-09-12) */
#define LA_MAX (LA_DEPTH_MAX + 2 + 8 + 15)  /* + K + N265_MAX_BFRAMES */
/* `--shot-window`'s bound. Not a quality limit - it is a MEMORY one: the
 * queue holds one padded source picture per slot, which is 3.1 MB at
 * 1080p, so 300 slots is a gigabyte of lookahead and nothing about a
 * shot plan gets better for holding ten seconds of one. */
#define N265_MAX_SHOT_WINDOW 300

struct n265_encoder;
struct n265_frame_work;

/* The content type (2026-09-11). Two tools want to know whether a frame
 * is FLAT-SHADED - synthetic animation over large constant fills - and
 * both give a different answer for it than for natural video: SAO's band
 * offset, which is the only thing in the codec that addresses banding on
 * a fill, and the per-CTU variance-AQ term, whose masking model that
 * content breaks. The measure is the share of 16x16 source luma blocks
 * whose variance is under one, in percent; `n265_flat_gate` is the share
 * at or above which a frame is typed flat (N265_FLAT_GATE, 0 = never,
 * which is also the pre-2026-09-11 encoder byte for byte).
 *
 * It is a pure function of the frame's own source luma plane and is
 * evaluated on the driver before any row job for the frame exists, so
 * every decision it feeds is byte-identical at every thread count. */
int n265_la_flat_pct(const struct n265_frame_work *fw);
int n265_flat_gate(void);

typedef struct la_frame {
    float sc_ratio;         /* pcost/icost at push time */
    /* The share of the frame's INTRA cost that sits on blocks where the
     * lowres search failed to undercut intra at all (phase 14 item 21).
     * `sc_ratio` is a sum ratio, so a minority of blocks that match very
     * cheaply can carry a whole frame under the bar; this says how much
     * of the picture, weighted by what intra would pay for it, inter
     * prediction cannot do anything with - which is the question the
     * scenecut is actually asking. */
    float sc_failw;
    float failw_lvl;        /* the running level sc_failw is read against */
    float sc_ema;           /* running ratio EMA before this frame */
    /* N265_SC_JUMP / N265_SC_FLOOR (phase 14 item 35): the ratios of the
     * up to three frames pushed before this one, newest first, and the
     * intra sum of the one just before it (-1 when there is none) */
    float sc_rprev[3];
    int sc_rn;
    int64_t sc_prev_ic;
    int mcstf_filt;         /* --mcstf filtered this picture (item 31) */
    int valid;
    int display_idx;
    pixel *src[3];          /* full-res source copy (pad_stride layout) */
    /* Half-res luma, and a COST PROXY rather than a picture: nothing
     * here is ever predicted from or reconstructed into, it only feeds
     * SADs. So it carries 8-BIT VALUES at every bit depth - the 2x2 mean
     * sheds BitDepth - 8 on the way in - and every constant fitted to it
     * (the 8x8 intra floor, the dead-zone model's log2 SAD, the
     * scenecut and b-adapt thresholds) keeps the units it was fitted in.
     * The TYPE widens with the pixel so the plane can share the one set
     * of SAD kernels; only the values stay narrow. */
    pixel *lowres;
    uint16_t *icost;        /* per lowres-8x8 block intra cost proxy    */
    uint16_t *pcost;        /* inter cost vs previous display frame     */
    /* the ZERO VECTOR's cost for the same block, in pcost's own units
     * (SAD plus the per-block floor), so the pair says how much of the
     * block's static residual the search actually removed. `zsad_sum`
     * below is its frame total and was already taken for the duplicate
     * chain; this is the per-block form `N265_LA_MVSTAT` reads, and it
     * is what the item 21 take-apart's coherence and global-vector
     * columns are computed against. On a frame with no reference it is
     * the intra cost, which is what `pcost` is there too. */
    uint16_t *zsad;
    int16_t *mv;            /* lowres MV pairs (full-pel lowres units)  */
    float *prop;            /* cu-tree propagate-in accumulator         */
    float *ctfrac;          /* cu-tree's 1 - pcost/icost; < 0: none     */
    uint64_t icost_sum, pcost_sum;
    uint64_t mvabs_sum;     /* sum |lowres MV| over blocks (motion magnitude) */
    /* the frame's motion in BITS on the exp-Golomb shape, summed over
     * blocks: what the coder would pay to fetch the prediction this cost
     * was measured with. `pcost_sum` is a bare distortion and an honest
     * search buys distortion with vector length, so the rate control's
     * complexity reads the pair (N265_RC_MVRATE, phase 14 item 21). */
    uint64_t mvbits_sum;
    /* The same sum with each block's L1 magnitude SATURATED at the reach
     * the b-adapt run rule's cutoff was fitted at (N265_BADAPT_MVCAP,
     * phase 14 item 20). The rule asks how much of the picture is moving
     * fast, and a plain mean of an unbounded magnitude answers a
     * different question once the search can reach past its old cap.
     * N265_BADAPT_MVGATE drops the blocks whose vector bought nothing
     * out of the same sum. */
    uint64_t mvsat_sum;
    /* The DUPLICATE chain (phase 11 item 1). A frame identical to its
     * reference in coding order is a picture whose coding is all-skip
     * with the zero vector, but the lookahead only ever costs a frame
     * against the PREVIOUS DISPLAY frame, and a reference in coding
     * order is rarely that one. Identity is transitive, so the
     * consecutive test is enough: `dup_grp` is the id of the run of
     * byte-identical sources this frame belongs to - extended from the
     * previous frame's run whenever the two sources compare equal - and
     * two frames are identical iff they carry the same id. The id rides
     * onto the DPB slot at claim, so the test at dispatch is an integer
     * compare against each reference and costs nothing.
     *
     * `zsad_sum` is this frame's ZERO-VECTOR lowres SAD against the
     * previous display frame, which is the value the lowres motion
     * search starts from and is therefore already computed, and
     * `dup_cum` its running total since the encode began. The L1
     * triangle inequality makes the difference of two frames' totals an
     * upper bound on the lowres distance between them, which is what the
     * threshold form (N265_DUP_THR) tests; on an exact run the bound is
     * zero and agrees with the id.
     *
     * `dup_grp` 0 is "no run", which a DPB slot that was never claimed
     * carries and which therefore never matches. The full-resolution
     * compare that extends a run runs only when the detector is armed
     * and only where the lowres SAD is already exactly zero, so a clip
     * without duplicates never pays for it. */
    int dup_grp;
    uint64_t zsad_sum, dup_cum;
    /* --shot-window: the base-QP offset the shot plan gave this frame,
     * and whether a plan has seen it yet. Written on the driver when the
     * frame's shot is planned, read at dispatch, and carried by la_pop's
     * shift with the rest of the slot. Both are 0 with the mode off. */
    float shot_off;
    uint8_t shot_planned;
} la_frame;

typedef struct n265_la {
    la_frame *f;            /* `cap` slots, allocated at open */
    int count;              /* queued, in display order f[0..count-1] */
    int cap;                /* slots allocated: fill + 2 + K + bframes */
    int lw, lh;             /* lowres dims */
    int bw, bh;             /* lowres 8x8 block grid */
    int depth;              /* target queue depth before draining */
    float sc_ema;           /* running pcost/icost EMA */
    float failw_ema;        /* the same running level for sc_failw */
    float sc_rhist[3];      /* the last pushed frames' ratios, newest first */
    int sc_rn;
    int64_t sc_last_ic;     /* the last pushed frame's intra sum, -1 = none */
    pixel *anchor_lowres;   /* lowres luma of the last coded anchor */
    int anchor_valid;
    /* the duplicate chain's own state, kept on the queue rather than on
     * the slots so it survives a queue that drains to empty: the last id
     * handed out, and the running lowres total `dup_cum` continues from */
    int dup_grp_next;
    uint64_t dup_cum_last;
    /* One entry per candidate run length, so 1 + N265_MAX_BFRAMES of
     * them; only `pc_n` are allocated (bframes + 1, never below the 8 the
     * default shape has always taken, so the default footprint is the
     * one it was). */
    int pc_n;
    int16_t *pc_mv[16];     /* priced b-adapt: forward MVs per candidate */
    int16_t *pc_bmv;        /* and the backward search's scratch */
    uint32_t *pc_sad[16];   /* b-adapt 4: the same searches' PER-BLOCK SAD */
    uint32_t *pc_bsad;      /* backward scratch */
    uint32_t *pc_bisad;     /* bidirectional scratch */
} n265_la;

/* The frames the decision machinery may read - cu-tree, the b-adapt run
 * length, the rate control's window walk. It is `la->count` at every
 * setting the board runs (the driver drains to `depth` before it
 * dispatches, so the queue never holds more than depth + 1 there) and it
 * is what holds `--shot-window`'s deeper queue OUT of every window term:
 * the mode buys the planner a longer look and changes nothing else. */
static inline int n265_la_view(const n265_la *la)
{
    int v = la->depth + 1;
    return la->count < v ? la->count : v;
}

int n265_la_init(struct n265_encoder *e);
void n265_la_free(struct n265_encoder *e);

/* per-file knob warming, on the driver at open; see n265_ctu_warm */
void n265_la_warm(void);

/* N265_BRUN_TRACE: price the bits trellis and print it, whatever rule is
 * actually deciding the run (docs/instruments.md) */
int n265_brun_trace(void);

/* The duplicate-frame path's four knobs (phase 11 item 1).
 *
 *  n265_dup_on     the DETECTOR runs: the lowres zero-vector SAD is
 *                  tested for exact zero and, where it is, the sources
 *                  are compared outright to extend the run. Armed by
 *                  either of the two below, because the trace has to
 *                  measure what the arm would decide on.
 *  n265_dup_fast   N265_DUP_FAST: the ARM. A frame whose source is the
 *                  source of one of its references is coded all-skip
 *                  toward it with the zero vector.
 *  n265_dup_thr_q8 N265_DUP_THR, and --dup-threshold, which is the same
 *                  knob spelled on the command line and beats the
 *                  environment (phase 14 item 15): the looser form, a
 *                  bound on the lowres distance in 1/256 of an 8-bit
 *                  luma level PER LOWRES PIXEL. 0 (the default) is the
 *                  strict form, where only a byte-identical source arms
 *                  the path.
 *  n265_dup_trace  N265_DUP_TRACE: the census, output-neutral.
 */
int n265_dup_on(void);
int n265_dup_fast(void);
int n265_dup_thr_q8(void);
int n265_dup_trace(void);

/* copy the picture in, build lowres + costs vs the previous queued frame */
void n265_la_push(struct n265_encoder *e, const pixel *const plane[3],
                  const int stride[3]);

/* scenecut test for queued frame index i (display order) */
int n265_la_scenecut(struct n265_encoder *e, int i);

/* run cu-tree over the queued window and fill e->ctu_qp_off for queued
 * frame index i */
void n265_la_cutree(struct n265_encoder *e, struct n265_frame_work *fw,
                    int i);

/* drop the front frame; its buffers go to the back of the free run */
void n265_la_pop(struct n265_encoder *e);

/* the slot the next n265_la_push will write into (D3's source stall
 * tests an in-flight frame_work's pad_plane against it) */
const pixel *n265_la_next_src(struct n265_encoder *e);

/* remember this queued frame's lowres plane as the last coded anchor */
void n265_la_set_anchor(struct n265_encoder *e, int i);

/* the B-run length the priced mini-GOP comparison prefers, 1..maxa: the
 * amortized lowres cost of coding `A` B frames under one anchor at
 * distance A+1, minimized over A. Needs a valid anchor plane. */
int n265_la_brun(struct n265_encoder *e, int maxa);

/* the same trellis priced in BITS rather than residual: each frame's
 * blocks costed max(0, log2(sad / dead zone at that frame's planned QP)),
 * which is the term that lets a long run win (docs/phase7-gop-design.md
 * D1). Needs a valid anchor plane. */
int n265_la_brun_bits(struct n265_encoder *e, int maxa);

/* the B-run length the anchor-prediction degradation rule allows */
int n265_la_degrade(struct n265_encoder *e, int maxa, double thr);

#endif
