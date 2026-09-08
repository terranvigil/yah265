/*
 * Encoder internals.
 * Copyright (c) 2026, the yah265 authors. GPL-2.0-or-later; see LICENSE.
 */

#ifndef N265_ENCODER_H
#define N265_ENCODER_H

#include <stdio.h>

#include "yah265.h"
#include "common/bitstream.h"
#include "encoder/cabac.h"
#include "encoder/la.h"
#include "encoder/tables.h"

struct sao_params;

enum {
    N265_MODE_INTRA = 0,    /* real coding: DC intra, CTB 32 */
    N265_MODE_PCM = 1,      /* phase-0 path: I_PCM, CTB 16 */
};

#define MIN_CB_LOG2 3
#define MAX_MERGE 5     /* MaxNumMergeCand (5 since 2026-09-06: -0.36% mean, 4 of 4 clips at matched rate) */

/* inter PartMode (7.4.9.5), in the order part_mode's binarization walks
 * them. NxN is intra-only here: at our min CB (8) the spec forbids inter
 * NxN, and above it the quadtree split already covers the shape. */
enum {
    PART_2Nx2N = 0,
    PART_2NxN,
    PART_Nx2N,
    PART_2NxnU,
    PART_2NxnD,
    PART_nLx2N,
    PART_nRx2N,
    PART_COUNT
};

#define PART_IS_HORIZ(p) ((p) == PART_2NxN || (p) == PART_2NxnU || \
                          (p) == PART_2NxnD)
#define PART_IS_AMP(p)   ((p) >= PART_2NxnU)
#define N265_DPB_SLOTS 6
#define N265_MAX_REF 3

struct n265_encoder {
    n265_param param;
    int mode;
    int qp;                 /* current frame's QP */
    int qp_base;            /* configured QP (P anchors) */
    int crf_on;             /* n265_set_crf: base QP from crf, not qp_base */
    double crf;
    int b_depth;            /* current B frame's pyramid layer (0 = anchor) */
    /* the current mini-GOP's coding-order plan (pyramid): each entry is a
     * lookahead index with its kind (0 B, 1 P, 2 IDR), pyramid depth and
     * reference flag. Built once per mini-GOP, then drained one per call. */
    int plan_li[LA_MAX], plan_depth[LA_MAX];
    uint8_t plan_kind[LA_MAX], plan_nonref[LA_MAX];
    int plan_len, plan_pos, plan_pop;
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
        int is_anchor;      /* 1 for I/P anchors, 0 for reference B (pyramid lifetime) */
        /* collocated motion for TMVP, 16x16 granularity (top-left 4x4
         * of each region, spec mv storage reduction) */
        uint8_t *col_pred;
        int16_t *col_mv;    /* 4 per cell: L0 x,y then L1 x,y */
        int col_ref_poc[2]; /* POC of that picture's L0/L1 ref (idx 0) */
        int16_t *col_ref;   /* per 16x16 cell: the block's own L0/L1 ref POCs */
        /* E1: the horizontal half of 8.4.4.3.2, cached per luma phase
         * (fx = 1,2,3) once per stored reference. Every subpel candidate
         * landing on the same fx then needs only the vertical pass, over
         * the same int16 intermediates the on-the-fly path would have
         * built - so the prediction stays bit-identical. Origin aligned
         * with plane[0]; NULL when the cache is off. */
        int16_t *hf_base[3];
        int16_t *hf[3];
        int hf_stride;
        int hf_x0, hf_x1, hf_y0, hf_y1;     /* positions it covers */
    } dpb[N265_DPB_SLOTS];
    /* the current frame's reference lists: refl[list][idx], nref[list]
     * entries (docs/refstruct-design.md); ref_valid = at least one */
    struct n265_refpic *refl[2][N265_MAX_REF];
    int nref[2];
    int rect;               /* rectangular inter PUs, SMP (N265_NO_RECT off) */
    int amp;                /* asymmetric inter PUs; also the SPS flag,
                             * which changes part_mode's binarization
                             * whether or not an AMP mode is chosen */
    int ref_max;            /* --ref: references per list wanted (1..N265_MAX_REF) */
    int badapt;             /* --b-adapt: shorten the B run on high-motion spans */
    float cutree_str;       /* N265_CUTREE_STR: cu-tree offset strength (default 2.0) */
    float aq_str;           /* N265_AQ_STR: per-CTU variance-AQ strength (default 0.7) */
    int qp_round1;          /* N265_QP_ROUND1: round the cu-tree + AQ field once (default on) */
    int ref_valid;

    uint8_t *ct_depth;      /* 4x4-granularity coding-tree depth map */
    uint8_t *intra_mode;    /* 4x4-granularity luma intra mode map   */
    uint8_t *edge_map;      /* bit0 = CU left edge, bit1 = top edge  */
    uint8_t *blk_intra;     /* per 4x4: CU coded intra               */
    uint8_t *blk_skip;      /* per 4x4: cu_skip_flag (ctx + bS)      */
    uint8_t *blk_cbf_y;     /* per 4x4: luma TB had coefficients     */
    int16_t *blk_mv;        /* per 4x4: L0 then L1 (x,y) qpel pairs  */
    uint8_t *blk_pred;      /* per 4x4: pred flags (1=L0, 2=L1)      */
    int8_t  *blk_ref;       /* per 4x4: reference index, L0 then L1  */
    int map_stride;         /* in 4x4 units                          */

    /* prediction override for a partitioned inter CU: the PU-by-PU MC
     * result, which every TB inside the CU crops from instead of running
     * its own whole-TB MC. NULL for 2Nx2N. */
    const uint8_t *cu_pred[3];
    int cu_pred_stride[3];
    int cu_pred_x0, cu_pred_y0;     /* luma origin of those planes */

    struct sao_params *sao; /* per-CTB SAO decisions                 */
    uint8_t *sao_tmp[3];    /* pristine deblocked planes during apply */

    cabac_ctx_t ctx[CTX_COUNT];
    cabac_ctx_t est_ctx[CTX_COUNT];     /* RD cost-model contexts */
    int last_skip_clean;    /* analyze_cu: skip won with low distortion */

    int aq;                 /* adaptive quantization on/off            */
    int8_t *ctu_qp_off;     /* per-CTU QP offset map (AQ/cu-tree feed) */
    float *ctu_qp_raw;      /* per-CTU cu-tree offset before rounding  */
    uint8_t *blk_qp;        /* per-4x4 actual QP (deblock, prediction) */
    int cur_ctu_qp;         /* QP of the CTU being analyzed            */
    int ent_qp_prev;        /* entropy-side qPY_PREV chain             */
    int ent_qp_coded;       /* IsCuQpDeltaCoded for the current CTU    */
    int ent_ctu_qp;         /* current CTU's target QP (entropy pass)  */
    int ent_parent_chroma;
    int in_entropy;  /* CB8-split TU3 sees parent chroma cbfs   */

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

    n265_la la;             /* lookahead queue + stats               */
    int la_depth;
    int cutree;
    int rdoq;               /* N265_RDOQ: RD-optimised quantisation (rdoq.c) */
    double psy;             /* N265_PSY: psychovisual energy term strength (psy.c), 0 = off */
    int scenecut;
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

/* PU rectangle idx (0 or 1) of `part` inside an n x n CU at (x0, y0).
 * Quarter offsets are the AMP ones; the split is always into two. */
static inline void n265_pu_rect(int part, int idx, int x0, int y0, int n,
                                int *px, int *py, int *pw, int *ph)
{
    int q = n >> 2, half = n >> 1;
    int cut;    /* the first PU's extent along the split axis */
    switch (part) {
    case PART_2NxnU: case PART_nLx2N: cut = q; break;
    case PART_2NxnD: case PART_nRx2N: cut = n - q; break;
    default:                          cut = half; break;
    }
    if (part == PART_2Nx2N) {
        *px = x0; *py = y0; *pw = n; *ph = n;
    } else if (PART_IS_HORIZ(part)) {
        *px = x0; *pw = n;
        *py = idx ? y0 + cut : y0;
        *ph = idx ? n - cut : cut;
    } else {
        *py = y0; *ph = n;
        *px = idx ? x0 + cut : x0;
        *pw = idx ? n - cut : cut;
    }
}

#define N265_NUM_PU(part) ((part) == PART_2Nx2N ? 1 : 2)

#define MODE_INTER (-1)     /* mode value selecting MC prediction */

/* z-scan decode-order key (intra.c) */
uint32_t n265_lambda_mode_q8(int qp);
void n265_build_lists(struct n265_encoder *e, int cur, int is_b, struct n265_refpic *future);
uint64_t n265_zorder(int x, int y, int pic_w, int ctb_log2);
#define N265_REF_PAD 64     /* reference plane border, luma samples */
/* E1 cache border. n265_clamp_mv keeps integer positions inside
 * [-(N265_REF_PAD - 8), pic + N265_REF_PAD - 8), and the vertical pass
 * reads 3 rows above and 4 below, so 60 covers every clamped MV. */
#define N265_HF_PAD 60

/* The cached horizontal intermediate for luma phase fx at integer
 * position (x, y) of a w x h block, or NULL when the cache cannot serve
 * it (off, or the block reaches outside the covered region and the
 * caller must interpolate from the plane itself). */
static inline const int16_t *n265_ref_hf(const struct n265_refpic *r,
                                         int x, int y, int w, int h, int fx)
{
    if (!fx || !r->hf[fx - 1])
        return NULL;
    if (x < r->hf_x0 || x + w > r->hf_x1 ||
        y - 3 < r->hf_y0 || y + h + 4 > r->hf_y1)
        return NULL;
    return r->hf[fx - 1] + y * r->hf_stride + x;
}

/* encoder.c, shared with ctu.c */
void n265_encoder_set_dump(n265_encoder *e, FILE *f);

#endif
