/*
 * In-loop deblocking filter.
 * Copyright (c) 2026, the yah265 authors. GPL-2.0-or-later; see LICENSE.
 */

#ifndef N265_DEBLOCK_H
#define N265_DEBLOCK_H

#include "encoder/encoder.h"

/* Filter the whole frame's recon in place (8.7.2): all vertical edges,
 * then all horizontal edges. All-intra: every CU-boundary edge on the
 * 8x8 grid has bS 2. Runs after the last CTU of the frame; safe in place
 * because every frame is an IDR and intra prediction already consumed the
 * unfiltered samples. */
void n265_deblock_frame(struct n265_frame_work *e);

/* One CTB row's share of the same filter, in wavefront order: the band's
 * vertical edges, then its top-boundary and internal horizontal edges.
 * Same output as n265_deblock_frame run over all rows. */
void n265_deblock_row(struct n265_frame_work *e, int r);

/* Resolve the N265_QP_INHERIT markers analysis left in blk_qp for one
 * CTB row, carrying the frame-serial qPY_PREV chain in *prev (seeded
 * with the slice QP). Runs before deblock(r). */
void n265_qpmap_row(struct n265_frame_work *e, int r, int *prev);

/* deblock's cached env knob, resolved on the driver thread at open with
 * the other warms */
void n265_deblock_warm(void);

#endif
