/*
 * Sample adaptive offset: per-CTB analysis, syntax, application.
 * Copyright (c) 2026, the yah265 authors. GPL-2.0-or-later; see LICENSE.
 */

#ifndef N265_SAO_H
#define N265_SAO_H

#include "encoder/encoder.h"

enum { SAO_OFF = 0, SAO_BAND = 1, SAO_EDGE = 2 };

typedef struct sao_params {
    uint8_t merge;          /* 0 explicit, 1 left, 2 up */
    uint8_t type[3];        /* per component; [2] mirrors [1]'s type   */
    uint8_t class_or_pos[3];/* eo class (0..3) or band position (0..31) */
    int8_t offset[3][4];    /* band: signed; edge: category 1..4 magnitudes
                             * stored signed with the inferred sign applied */
} sao_params;

/* Is the filter enabled at all? N265_SAO_TYPES=0 disables it in the SPS,
 * so no slice flag and no sao() syntax is coded and neither the analysis
 * nor the application pass runs. */
int n265_sao_enabled(void);

/* Decide and apply one CTB row's offsets (docs/threading-design.md
 * sec 1). Analysis and application both see the picture pre-SAO: the
 * one line SAO has already rewritten - the last line of the row above -
 * comes from e->sao_line[]. */
void n265_sao_row(struct n265_frame_work *e, int cy);

/* Write one CTB's sao() syntax (7.3.8.3). */
void n265_sao_write(struct n265_frame_work *e, cabac_t *cb, int ctb_x,
                    int ctb_y);

#endif
