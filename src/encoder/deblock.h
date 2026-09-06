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
void n265_deblock_frame(struct n265_encoder *e);

#endif
