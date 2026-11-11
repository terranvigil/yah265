/*
 * In-loop deblocking filter.
 * Copyright (c) 2026, the yah265 authors. GPL-2.0-or-later; see LICENSE.
 */

#ifndef N265_DEBLOCK_H
#define N265_DEBLOCK_H

#include <stddef.h>
#include <stdint.h>

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

/* The filter kernels (class N265_ASM_DBK). `pix` is the first q0 sample.
 * The luma pair filters `nseg` four-line segments of one edge: a
 * vertical edge's segments run down the column, a horizontal edge's run
 * along the row. `beta` and `tc` are per segment, and a segment whose tc
 * is 0 is not touched. The chroma pair filters `n` lines of an edge, each
 * with its own tc. Reads stay within p3..q3 (luma) or p1..q1 (chroma) of
 * the lines named. */
void n265_dbk_luma_v(pixel *pix, ptrdiff_t stride, const int16_t *beta,
                     const int16_t *tc, int nseg);
void n265_dbk_luma_h(pixel *pix, ptrdiff_t stride, const int16_t *beta,
                     const int16_t *tc, int nseg);
void n265_dbk_chroma_v(pixel *pix, ptrdiff_t stride, const int16_t *tc,
                       int n);
void n265_dbk_chroma_h(pixel *pix, ptrdiff_t stride, const int16_t *tc,
                       int n);

/* the C references of the above, for checkasm and the kernels' tails */
void n265_dbk_luma_v_ref(pixel *pix, ptrdiff_t stride, const int16_t *beta,
                         const int16_t *tc, int nseg);
void n265_dbk_luma_h_ref(pixel *pix, ptrdiff_t stride, const int16_t *beta,
                         const int16_t *tc, int nseg);
void n265_dbk_chroma_v_ref(pixel *pix, ptrdiff_t stride, const int16_t *tc,
                           int n);
void n265_dbk_chroma_h_ref(pixel *pix, ptrdiff_t stride, const int16_t *tc,
                           int n);

/* deblock's cached env knob, resolved on the driver thread at open with
 * the other warms */
void n265_deblock_warm(void);

#endif
