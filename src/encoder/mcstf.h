/*
 * --mcstf: the motion-compensated spatio-temporal filter on the SOURCE.
 *
 * A picture's own noise is uncorrelated with its neighbours' and its
 * CONTENT is not, so the average of a few motion-compensated neighbours
 * is the same picture with less noise in it - and a picture with less
 * noise in it is cheaper to code at the same apparent quality. That is
 * the whole claim, and it is a claim about NOISY SOURCES only: on clean
 * content the average has nothing to remove and can only blur.
 *
 * Three things keep it from blurring anyway, and they are the three
 * weights below: a neighbour counts less the further away in time it is,
 * less where its block did not match, and less at any pixel where it
 * disagrees with the original by more than noise could explain. A moving
 * edge the motion search missed therefore contributes almost nothing
 * rather than smearing, which is the failure mode a plain temporal
 * average has.
 *
 * WHERE IT SITS. Ahead of the lookahead, not inside it: the filter needs
 * the frames on BOTH sides of the one it is filtering, and the lookahead
 * has already built the lowres plane and the costs by the time a later
 * frame arrives. So this is a queue in front of n265_la_push - the
 * encoder hands pictures in here and takes filtered ones out `range`
 * pictures later - and everything downstream (the lowres costs, the
 * scenecut, cu-tree, mode decision, the coded picture) sees exactly one
 * picture, the filtered one. It costs `range` frames of latency and
 * nothing else.
 *
 * Driver-serial, integer, display order: the filtered picture is a pure
 * function of the source at every thread count.
 *
 * Copyright (c) 2026, the yah265 authors. GPL-2.0-or-later; see LICENSE.
 */

#ifndef N265_MCSTF_H
#define N265_MCSTF_H

#include <stdint.h>

#include "common/pixel.h"

/* x265's own range for --mcstf-ref-range: neighbours on EACH side. 0 is
 * the filter doing nothing, which is not the same as the filter being
 * off (it still costs the latency), so the CLI refuses 0 by name. */
#define N265_MCSTF_MAX_RANGE 4
/* The cadence, in display indices. x265 filters the GOP's own anchors
 * rather than every picture, which is what makes it affordable: the
 * frames a mini-GOP predicts from are the ones whose noise propagates,
 * and a leaf that predicts from two filtered anchors inherits most of
 * the benefit for none of the cost. 8 is x265's default GOP length and
 * ours at --bframes 7. */
#define N265_MCSTF_PERIOD 8
/* The block the motion search works in, at full resolution. 16 is large
 * enough that a SAD over it separates a real match from a lucky one on
 * noisy content - which is the content this filter is for - and small
 * enough to follow a face or a hand. */
#define N265_MCSTF_BLK 16
/* Pad, in samples, on every side of every stored plane. It has to cover
 * the furthest the search can reach out of the picture: the coarse stage
 * reaches 8 coarse steps (32 samples), the refinement 4 more, and a
 * block is 16 wide. 48 covers it with room and keeps the rows aligned. */
#define N265_MCSTF_PAD 48
/* The coarse stage's subsampling, and its search radius IN COARSE STEPS.
 * 4 and 8 is +-32 samples, which at 720p is a fifth of the picture width
 * a second at 25 fps - past what a source filter has any business
 * following, and past what the weights would credit if it did. */
#define N265_MCSTF_SUB 4
#define N265_MCSTF_COARSE 8
/* The refinement's radius at full resolution, around the coarse winner. */
#define N265_MCSTF_FINE 4

typedef struct n265_mcstf n265_mcstf;

/* Open the queue. `w`/`h` are the displayed luma size, `csx`/`csy` the
 * chroma shift, `range` the neighbours per side (1..4), `selective`
 * arms --selective-mcstf. Returns NULL on an allocation failure. */
n265_mcstf *n265_mcstf_open(int w, int h, int csx, int csy, int range,
                            int selective);
void n265_mcstf_close(n265_mcstf *m);

/* Hand one source picture in, in the caller's own layout. It is copied,
 * so the caller's buffer is free the moment this returns. */
void n265_mcstf_push(n265_mcstf *m, const pixel *const plane[3],
                     const int stride[3]);

/* Is a filtered picture ready to be taken out? After the last push, call
 * this with `flush` nonzero to drain the queue: the trailing pictures
 * are filtered against the neighbours that exist, which is fewer on the
 * future side and is exactly what the leading pictures already do. */
int n265_mcstf_ready(n265_mcstf *m, int flush);

/* Take the ready picture out. Fills `plane` and `stride` with the
 * filtered picture's DISPLAYED area inside the padded storage; both stay
 * valid until the next n265_mcstf_pop. */
void n265_mcstf_pop(n265_mcstf *m, const pixel *plane[3], int stride[3]);

/* Whether the last popped picture was filtered at all, and what the
 * selective gate read on it. Diagnostic; the encoder prints them under
 * N265_MCSTF_TRACE. */
int n265_mcstf_last_filtered(const n265_mcstf *m);
int n265_mcstf_last_noise_q8(const n265_mcstf *m);

/* Knobs (docs/knobs.md). Accessors and not inline getenv: a warm can
 * only reach a gate through a callable function
 * (scripts/env_gate_audit.py). */
int n265_mcstf_sigma_q8(void);  /* N265_MCSTF_SIGMA: the photometric width */
int n265_mcstf_clean_q8(void);  /* N265_MCSTF_CLEAN: --selective's bar     */
int n265_mcstf_period(void);    /* N265_MCSTF_PERIOD: the cadence          */
int n265_mcstf_trace(void);     /* N265_MCSTF_TRACE: one line per picture  */

#endif
