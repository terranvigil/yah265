/*
 * Lookahead: input queue, lowres statistics, scenecut, cu-tree
 * (docs/lookahead-design.md L0-L2, serial form).
 * Copyright (c) 2026, the yah265 authors. GPL-2.0-or-later; see LICENSE.
 */

#ifndef N265_LA_H
#define N265_LA_H

#include <stdint.h>

#define LA_MAX 24
#define LA_MAX_CTUS 8160    /* 4K at CTB 32 */

struct n265_encoder;

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
} la_frame;

typedef struct n265_la {
    la_frame f[LA_MAX];
    int count;              /* queued, in display order f[0..count-1] */
    int lw, lh;             /* lowres dims */
    int bw, bh;             /* lowres 8x8 block grid */
    int depth;              /* target queue depth before draining */
    float sc_ema;           /* running pcost/icost EMA */
} n265_la;

int n265_la_init(struct n265_encoder *e);
void n265_la_free(struct n265_encoder *e);

/* copy the picture in, build lowres + costs vs the previous queued frame */
void n265_la_push(struct n265_encoder *e, const uint8_t *const plane[3],
                  const int stride[3]);

/* scenecut test for queued frame index i (display order) */
int n265_la_scenecut(struct n265_encoder *e, int i);

/* run cu-tree over the queued window and fill e->ctu_qp_off for queued
 * frame index i */
void n265_la_cutree(struct n265_encoder *e, int i);

/* drop the front frame */
void n265_la_pop(struct n265_encoder *e);

#endif
