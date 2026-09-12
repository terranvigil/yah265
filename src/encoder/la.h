/*
 * Lookahead: input queue, lowres statistics, scenecut, cu-tree
 * (docs/lookahead-design.md L0-L2, serial form).
 * Copyright (c) 2026, the yah265 authors. GPL-2.0-or-later; see LICENSE.
 */

#ifndef N265_LA_H
#define N265_LA_H

#include <stdint.h>

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
#define LA_MAX_CTUS 8160    /* 4K at CTB 32 */
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
    float sc_ema;           /* running ratio EMA before this frame */
    int valid;
    int display_idx;
    uint8_t *src[3];        /* full-res source copy (pad_stride layout) */
    uint8_t *lowres;        /* half-res luma                            */
    uint16_t *icost;        /* per lowres-8x8 block intra cost proxy    */
    uint16_t *pcost;        /* inter cost vs previous display frame     */
    int16_t *mv;            /* lowres MV pairs (full-pel lowres units)  */
    float *prop;            /* cu-tree propagate-in accumulator         */
    uint64_t icost_sum, pcost_sum;
    uint64_t mvabs_sum;     /* sum |lowres MV| over blocks (motion magnitude) */
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
    uint8_t *anchor_lowres; /* lowres luma of the last coded anchor */
    int anchor_valid;
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

/* copy the picture in, build lowres + costs vs the previous queued frame */
void n265_la_push(struct n265_encoder *e, const uint8_t *const plane[3],
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
const uint8_t *n265_la_next_src(struct n265_encoder *e);

/* remember this queued frame's lowres plane as the last coded anchor */
void n265_la_set_anchor(struct n265_encoder *e, int i);

/* the B-run length the priced mini-GOP comparison prefers, 1..maxa: the
 * amortised lowres cost of coding `A` B frames under one anchor at
 * distance A+1, minimised over A. Needs a valid anchor plane. */
int n265_la_brun(struct n265_encoder *e, int maxa);

/* the same trellis priced in BITS rather than residual: each frame's
 * blocks costed max(0, log2(sad / dead zone at that frame's planned QP)),
 * which is the term that lets a long run win (docs/phase7-gop-design.md
 * D1). Needs a valid anchor plane. */
int n265_la_brun_bits(struct n265_encoder *e, int maxa);

/* the B-run length the anchor-prediction degradation rule allows */
int n265_la_degrade(struct n265_encoder *e, int maxa, double thr);

#endif
