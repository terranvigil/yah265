/*
 * Sample adaptive offset: per-CTB analysis, syntax, application.
 * Copyright (c) 2026, the next265 authors. GPL-2.0-or-later; see LICENSE.
 */

#ifndef N265_SAO_H
#define N265_SAO_H

#include "encoder/encoder.h"

enum { SAO_OFF = 0, SAO_BAND = 1, SAO_EDGE = 2 };

typedef struct sao_params {
    uint8_t type[3];        /* per component; [2] mirrors [1]'s type   */
    uint8_t class_or_pos[3];/* eo class (0..3) or band position (0..31) */
    int8_t offset[3][4];    /* band: signed; edge: category 1..4 magnitudes
                             * stored signed with the inferred sign applied */
} sao_params;

/* Analyze the deblocked recon against the source and fill e->sao[]. */
void n265_sao_analyze(struct n265_encoder *e);

/* Write one CTB's sao() syntax (7.3.8.3). */
void n265_sao_write(struct n265_encoder *e, cabac_t *cb, int ctb_x,
                    int ctb_y);

/* Apply the chosen offsets: reads a pristine copy of the deblocked
 * planes, writes into e->recon. */
void n265_sao_apply(struct n265_encoder *e);

#endif
