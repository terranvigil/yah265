/*
 * Encoder internals.
 * Copyright (c) 2026, the next265 authors. GPL-2.0-or-later; see LICENSE.
 */

#ifndef N265_ENCODER_H
#define N265_ENCODER_H

#include "next265.h"
#include "common/bitstream.h"
#include "encoder/cabac.h"

/* Phase 0 coding configuration: CTB == min CB == PCM CB == 16 so the
 * quadtree needs no split flags and every CU is one I_PCM block. */
#define N265_CTB_LOG2 4
#define N265_CTB_SIZE (1 << N265_CTB_LOG2)

struct n265_encoder {
    n265_param param;

    int pad_w, pad_h;       /* dims padded up to a CTB multiple      */
    int ctb_cols, ctb_rows;

    uint8_t *pad_plane[3];  /* edge-padded copies of the input       */
    int pad_stride[3];

    bitstream_t rbsp;       /* per-NAL RBSP scratch                  */
    uint8_t *out;           /* Annex-B output for the current call   */
    size_t out_cap;

    n265_nal nal[8];
    int frames_done;
};

/* headers.c: each writes one RBSP into e->rbsp (reset first by callee). */
void n265_write_vps(struct n265_encoder *e);
void n265_write_sps(struct n265_encoder *e);
void n265_write_pps(struct n265_encoder *e);

#endif
