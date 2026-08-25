/*
 * Encoder internals.
 * Copyright (c) 2026, the next265 authors. GPL-2.0-or-later; see LICENSE.
 */

#ifndef N265_ENCODER_H
#define N265_ENCODER_H

#include <stdio.h>

#include "next265.h"
#include "common/bitstream.h"
#include "encoder/cabac.h"
#include "encoder/tables.h"

struct sao_params;

enum {
    N265_MODE_INTRA = 0,    /* real coding: DC intra, CTB 32 */
    N265_MODE_PCM = 1,      /* phase-0 path: I_PCM, CTB 16 */
};

#define MIN_CB_LOG2 3

struct n265_encoder {
    n265_param param;
    int mode;
    int qp;
    int ctb_log2;

    int pad_w, pad_h;       /* dims padded up to a min-CB multiple  */
    int ctb_cols, ctb_rows;

    uint8_t *pad_plane[3];  /* edge-padded copies of the input       */
    int pad_stride[3];
    uint8_t *recon[3];      /* reconstruction planes (padded dims)   */
    int recon_stride[3];

    uint8_t *ct_depth;      /* 4x4-granularity coding-tree depth map */
    uint8_t *intra_mode;    /* 4x4-granularity luma intra mode map   */
    uint8_t *edge_map;      /* bit0 = CU left edge, bit1 = top edge  */
    int map_stride;         /* in 4x4 units                          */

    struct sao_params *sao; /* per-CTB SAO decisions                 */
    uint8_t *sao_tmp[3];    /* pristine deblocked planes during apply */

    cabac_ctx_t ctx[CTX_COUNT];

    bitstream_t rbsp;       /* per-NAL RBSP scratch                  */
    uint8_t *out;           /* Annex-B output for the current call   */
    size_t out_cap;

    n265_nal nal[8];
    int frames_done;
    FILE *dump_recon;       /* optional recon tap for the gate       */
};

/* headers.c */
void n265_write_vps(struct n265_encoder *e);
void n265_write_sps(struct n265_encoder *e);
void n265_write_pps(struct n265_encoder *e);

/* ctu.c: encode one CTU's coding_quadtree into the CABAC stream,
 * reconstructing as it goes. */
void n265_encode_ctu(struct n265_encoder *e, cabac_t *cb, int ctb_x,
                     int ctb_y);

/* encoder.c, shared with ctu.c */
void n265_encoder_set_dump(n265_encoder *e, FILE *f);

#endif
