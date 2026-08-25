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
    int no_sao;

    int pad_w, pad_h;       /* dims padded up to a min-CB multiple  */
    int ctb_cols, ctb_rows;

    uint8_t *pad_plane[3];  /* edge-padded copies of the input       */
    int pad_stride[3];
    uint8_t *recon[3];      /* reconstruction planes (padded dims)   */
    int recon_stride[3];
    /* two-slot DPB of padded, border-extended, fully filtered pictures */
    struct n265_refpic {
        uint8_t *base[3];
        uint8_t *plane[3];  /* origin inside the border */
        int stride[3];
        int poc;
        int valid;
    } dpb[2];
    struct n265_refpic *ref_l0, *ref_l1;    /* current frame's lists */
    int ref_valid;

    uint8_t *ct_depth;      /* 4x4-granularity coding-tree depth map */
    uint8_t *intra_mode;    /* 4x4-granularity luma intra mode map   */
    uint8_t *edge_map;      /* bit0 = CU left edge, bit1 = top edge  */
    uint8_t *blk_intra;     /* per 4x4: CU coded intra               */
    uint8_t *blk_skip;      /* per 4x4: cu_skip_flag (ctx + bS)      */
    uint8_t *blk_cbf_y;     /* per 4x4: luma TB had coefficients     */
    int16_t *blk_mv;        /* per 4x4: L0 then L1 (x,y) qpel pairs  */
    uint8_t *blk_pred;      /* per 4x4: pred flags (1=L0, 2=L1)      */
    int map_stride;         /* in 4x4 units                          */

    struct sao_params *sao; /* per-CTB SAO decisions                 */
    uint8_t *sao_tmp[3];    /* pristine deblocked planes during apply */

    cabac_ctx_t ctx[CTX_COUNT];
    cabac_ctx_t est_ctx[CTX_COUNT];     /* RD cost-model contexts */

    bitstream_t rbsp;       /* per-NAL RBSP scratch                  */
    uint8_t *out;           /* Annex-B output for the current call   */
    size_t out_cap;

    uint8_t *rec_buf;       /* CTU record stream (records.h)         */
    size_t rec_len, rec_cap, rec_pos;

    n265_nal nal[8];
    int frames_done;
    int poc;
    int keyint;
    int bframes;            /* 0 or 1 for now                        */
    int slice_type;         /* 2 = I, 1 = P, 0 = B (current frame)   */
    int nonref;             /* current frame is not a reference      */
    int display_idx;        /* current frame's global display index  */

    /* one-deep latency queue for the held B source */
    uint8_t *held_src[3];
    int held_valid;
    int held_display_idx;
    int in_display_idx;     /* next arriving picture's display index */
    int gop_start_display;  /* display index of the current IDR      */

    FILE *dump_decode;      /* decode-order recon tap (dec265 leg)   */
    FILE *dump_recon;       /* optional recon tap for the gate       */
};

/* headers.c */
void n265_write_vps(struct n265_encoder *e);
void n265_write_sps(struct n265_encoder *e);
void n265_write_pps(struct n265_encoder *e);

/* ctu.c: analyze one CTU (decisions + recon + records, no entropy) */
void n265_analyze_ctu(struct n265_encoder *e, int ctb_x, int ctb_y);

/* entropy.c: replay one CTU's records through the CABAC engine */
void n265_entropy_ctu(struct n265_encoder *e, cabac_t *cb, int ctb_x,
                      int ctb_y);

#define MODE_INTER (-1)     /* mode value selecting MC prediction */

/* z-scan decode-order key (intra.c) */
uint64_t n265_zorder(int x, int y, int pic_w, int ctb_log2);
#define N265_REF_PAD 64     /* reference plane border, luma samples */

/* encoder.c, shared with ctu.c */
void n265_encoder_set_dump(n265_encoder *e, FILE *f);

#endif
