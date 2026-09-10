/*
 * Encoder internals.
 * Copyright (c) 2026, the yah265 authors. GPL-2.0-or-later; see LICENSE.
 */

#ifndef N265_ENCODER_H
#define N265_ENCODER_H

#include <stdatomic.h>
#include <stdio.h>

#include "yah265.h"
#include "threadpool/ntp.h"
#include "common/bitstream.h"
#include "encoder/cabac.h"
#include "encoder/la.h"
#include "encoder/rc.h"
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
/* blk_qp marker: this CU coded no cu_qp_delta, so its QP is whatever the
 * frame-serial qPY_PREV chain holds when the map fill reaches it
 * (n265_qpmap_row). Analysis cannot know that value under a row
 * wavefront; the fill can, and it is a pure function of the QP map and
 * the markers. */
#define N265_QP_INHERIT 0xff
#define N265_MAX_REF 3
#define N265_MAX_BFRAMES 7      /* --bframes clamp; also sizes the mini-GOP
                                 * walk the SPS's DPB derivation runs */

/* The DPB picture: a padded, border-extended, fully filtered
 * reconstruction plus the side data a later frame reads from it. The
 * frame being coded reconstructs DIRECTLY into its claimed slot
 * (docs/threading-design.md sec 4), so there is no commit copy; the
 * slot stays `valid = 0` until publish, which is what keeps the picture
 * out of its own RPS and out of its own reference lists. */
struct n265_refpic {
    uint8_t *base[3];
    uint8_t *plane[3];  /* origin inside the border */
    int stride[3];
    int poc;
    /* D2 (T2 decision): DPB membership as the DECODER sees it - the RPS,
     * the reference lists, eviction. Driver-serial: set at the end of
     * this picture's own dispatch step, cleared by the mini-GOP-end
     * retirement, both in coding order, so frame k+1's dispatch sees
     * frame k marked whether or not a single row of k has run. It says
     * nothing about whether the pixels exist yet; `final_rows` does. */
    int marked;
    /* ...and the physical half: CTB rows of this picture that are
     * deblocked, SAO-filtered, border-extended and published. The sec 1
     * reference gate reads ONLY this. Reset at claim, advanced by the
     * SAO row task. */
    atomic_int final_rows;
    int in_use;         /* claimed by a frame in flight (not yet retired) */
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
    /* P2: the coding-tree depth range this picture used, per CTU;
     * 0xff where nothing filled it. The collocated entry is the
     * temporal half of the depth bound. */
    uint8_t *ctu_dmin, *ctu_dmax;
};

/* ctu.c-owned trial buffers (was ~180 KB of function-local statics). One
 * per row lane; see n265_scratch_new. */
struct n265_scratch;

/* D1 (T2 decision): the short-term RPS as it stood when the frame was
 * DISPATCHED. The driver captures it in coding order, right after the
 * reference lists are built and before anything else is dispatched;
 * write_slice_header serializes it and never reads the DPB. That is what
 * makes a slice header a pure function of the dispatch order rather than
 * of whenever the entropy pass happens to run.
 *
 * Both lists are in the order 7.3.7 wants: negative POCs nearest first,
 * then positive POCs nearest first. They are keyed on POC only - never
 * on slot index - so the order cannot depend on which slot a picture
 * happens to occupy. */
struct n265_rps {
    int nneg, npos;
    int neg_poc[N265_DPB_SLOTS], pos_poc[N265_DPB_SLOTS];
    uint8_t neg_used[N265_DPB_SLOTS], pos_used[N265_DPB_SLOTS];
};

/* N265_CU_STATS buckets: coded luma area by the CU kind actually written */
enum {
    N265_CS_SKIP = 0,       /* cu_skip_flag                          */
    N265_CS_MERGE,          /* merged 2Nx2N with residual            */
    N265_CS_AMVP,           /* 2Nx2N with explicit motion            */
    N265_CS_RECT,           /* 2NxN / Nx2N                           */
    N265_CS_AMP,            /* the four asymmetric partitions        */
    N265_CS_INTRA,          /* intra 2Nx2N                           */
    N265_CS_INTRA_NXN,      /* intra NxN at the min CB               */
    N265_CS_ZEROCBF,        /* of the above, inter CUs with no residual */
    N265_CS_COUNT
};

/* The coding context (docs/threading-design.md sec 4, T0 audit).
 *
 * One instance per frame in flight, and one SHALLOW COPY per row lane.
 * Fields divide three ways:
 *
 *  - immutable geometry and config, copied once by fw_config() at
 *    encoder open. Duplicated by value on purpose: a few hundred bytes
 *    per slot buys map_stride / pad_w / ctb_log2 out of an indirection
 *    in the hottest loops, and they are written by exactly one function.
 *  - pointers to frame-owned buffers. A row copy shares them, so a row
 *    task writes the frame's maps in place. The 2-CTB row lag is what
 *    makes every read of them final.
 *  - per-frame mutable scalars. A row copy owns its own, which is what
 *    makes cur_ctu_qp, the record cursor and the CU-scoped fields
 *    row-private for free.
 *
 * The only pointers a row copy re-points are est_ctx, the record buffer
 * and the scratch; see n265_row_begin. */
typedef struct n265_frame_work {
    struct n265_encoder *enc;

    /* ---- immutable: fw_config() ---- */
    n265_param param;
    int mode;
    int ctb_log2;
    int pad_w, pad_h;       /* dims padded up to a min-CB multiple */
    int ctb_cols, ctb_rows;
    int map_stride;         /* in 4x4 units */
    int rect;               /* rectangular inter PUs, SMP (N265_NO_RECT off) */
    int amp;                /* asymmetric inter PUs; also the SPS flag,
                             * which changes part_mode's binarization
                             * whether or not an AMP mode is chosen */
    int rdoq;               /* N265_RDOQ: RD-optimised quantisation */
    int qp_delta_on;        /* cu_qp_delta_enabled_flag: the ONE predicate
                             * the PPS and the entropy writer both read */
    int bframes;
    /* What the reference structure asks of a decoder's DPB, derived from
     * --bframes and --ref at open (n265_dpb_requirement). `dpb_reorder`
     * is sps_max_num_reorder_pics; `dpb_buffering` is the picture storage
     * buffer count, i.e. sps_max_dec_pic_buffering_minus1 + 1, and the
     * level is picked to hold it. */
    int dpb_reorder, dpb_buffering;
    double psy;             /* N265_PSY: psychovisual energy strength */
    int dbf_beta, dbf_tc;   /* slice_beta/tc_offset_div2, see n265_encoder */

    /* ---- per-frame: source, recon, slice ---- */
    const uint8_t *pad_plane[3];    /* the lookahead slot's padded source */
    int pad_stride[3];
    uint8_t *recon[3];      /* aliases slot->plane[] */
    int recon_stride[3];
    struct n265_refpic *slot;       /* the DPB slot being reconstructed into */

    int qp;                 /* this frame's slice QP */
    int poc;
    int slice_type;         /* 2 = I, 1 = P, 0 = B */
    int nonref;             /* this frame is not a reference */
    int display_idx;
    int b_depth;            /* pyramid layer (0 = anchor) */
    /* N265_CU_STATS: per-frame coded-area shares by CU kind and TU leaf
     * size, accumulated in the entropy pass (everything there is final).
     * Per-frame_work, because entropy is a pool task and K frames can be
     * in it at once; the CSV row is printed on the driver at retirement,
     * so the rows come out in coding order whatever the schedule did.
     * Output-neutral; off unless the variable is set. */
    int cu_stats;
    uint64_t cs_area[N265_CS_COUNT];
    uint64_t cs_tu[4], cs_tu_cbf[4];
    uint64_t cs_coef;       /* nonzero luma levels coded              */
    struct n265_refpic *refl[2][N265_MAX_REF];
    int nref[2];
    int ref_valid;
    struct n265_rps rps;    /* D1: snapshotted at dispatch */
    /* D2: the TMVP collocated picture, resolved at dispatch. The coding
     * path must not read `marked` off a live DPB slot - a later frame's
     * dispatch can retire the picture while this one is still analysing
     * it - so the choice and the POC it scales against are frozen here.
     * NULL when there is no usable collocated picture. */
    struct n265_refpic *col;
    int col_poc;

    /* lookahead hand-off, frozen before the frame launches */
    float sc_ratio;         /* lookahead pcost/icost (P3) */
    const uint16_t *la_icost, *la_pcost;
    int la_bw, la_bh;

    /* ---- decision maps, frame-owned ---- */
    uint8_t *ct_depth;      /* 4x4-granularity coding-tree depth map */
    uint8_t *intra_mode;    /* 4x4-granularity luma intra mode map   */
    uint8_t *edge_map;      /* bit0 = CU left edge, bit1 = top edge  */
    uint8_t *blk_intra;     /* per 4x4: CU coded intra               */
    uint8_t *blk_skip;      /* per 4x4: cu_skip_flag (ctx + bS)      */
    uint8_t *blk_cbf_y;     /* per 4x4: luma TB had coefficients     */
    int16_t *blk_mv;        /* per 4x4: L0 then L1 (x,y) qpel pairs  */
    uint8_t *blk_pred;      /* per 4x4: pred flags (1=L0, 2=L1)      */
    int8_t  *blk_ref;       /* per 4x4: reference index, L0 then L1  */
    uint8_t *blk_qp;        /* per-4x4 actual QP (deblock, prediction) */
    int8_t *ctu_qp_off;     /* per-CTU QP offset map (AQ/cu-tree feed) */
    float *ctu_qp_raw;      /* per-CTU cu-tree offset before rounding  */
    uint8_t *ctu_dmin, *ctu_dmax;   /* P2: per-CTU depth range used */
    struct sao_params *sao; /* per-CTB SAO decisions */
    uint8_t *sao_tmp[3];    /* one CTB row's pristine deblocked band */
    uint8_t *sao_line[3];   /* the row above's last line, pre-SAO */

    /* ---- row-private once a row copy exists ---- */
    int cur_ctu_qp;         /* QP of the CTU being analyzed */
    int db_lo, db_hi;       /* P2: depth range this quadtree may search */
    const uint8_t *cu_pred[3];      /* PU-by-PU MC result a partitioned
                                     * inter CU's TBs crop from; NULL for
                                     * 2Nx2N */
    int cu_pred_stride[3];
    int cu_pred_x0, cu_pred_y0;     /* luma origin of those planes */
    int last_skip_clean;    /* analyze_cu: skip won with low distortion */
    int last_cu_nores;      /* analyze_cu: the winner had rqt_root_cbf 0 */
    cabac_ctx_t *est_ctx;   /* RD cost-model contexts (row-private) */
    uint8_t *rec_buf;       /* CTU record stream (records.h) */
    size_t rec_len, rec_cap, rec_pos;
    struct n265_scratch *sc;

    /* Row progress, shared by every lane working on this frame (a
     * pointer, so the per-row shallow copy shares it rather than
     * copying it). Watermarks only: a task asks whether it MAY run,
     * never how far another one has got, so nothing here can reach a
     * coding decision. */
    struct n265_prog {
        atomic_int enc_rows;    /* CTB rows fully analyzed */
        atomic_int dbk_rows;    /* ...deblocked  */
        atomic_int pub_rows;    /* ...SAO-filtered, border-extended and
                                 * PUBLISHED (the row's TMVP field, P2
                                 * depths and E1 cache are written before
                                 * this advances; G4) */
        atomic_int enc_prog[];  /* CTBs committed per row */
    } *prog;
    int qprev;                  /* qPY_PREV carried by the QP-map fill */

    /* one record stream per CTB row: rows commit independently and the
     * entropy pass replays them in row order */
    struct n265_rrec {
        uint8_t *buf;
        size_t len, cap;
    } *rrec;                /* ctb_rows entries */
    /* the estimator state published after CTB 1 of each row, which the
     * row below initializes from (sec 2) */
    cabac_ctx_t *est_snap;  /* [ctb_rows][CTX_COUNT] */

    /* ---- the frame's job on the pool (T2/3) ---- */
    ntp_job job;
    int in_flight;          /* dispatched, not yet retired */
    int job_R;              /* CTB rows the task table covers (0 for PCM) */
    int job_cols, job_snap_x, job_sao_on;
    int ref_lag;            /* G1's row-start hint, in CTB rows */
    int emit_headers;       /* this frame carries the VPS/SPS/PPS */
    int nal_type;
    /* rate control hand-off, frozen at dispatch (sec 3 rule 4) */
    int rc_cls;
    double rc_cplx, rc_base;
    double rc_planned;      /* D4: the fair share rc_next handed this frame */

    /* ---- entropy, frame-level ---- */
    cabac_ctx_t ctx[CTX_COUNT];
    int ent_qp_prev;        /* entropy-side qPY_PREV chain */
    int ent_qp_coded;       /* IsCuQpDeltaCoded for the current CTU */
    int ent_ctu_qp;         /* current CTU's target QP (entropy pass) */
    int ent_parent_chroma;
    int in_entropy;         /* CB8-split TU3 sees parent chroma cbfs */
    bitstream_t rbsp;       /* per-NAL RBSP scratch */

    /* frame-owned backing store the pointers above point at when this
     * instance is the FRAME's (a row copy re-points them) */
    cabac_ctx_t est_store[CTX_COUNT];   /* the slice-init contexts */
} n265_frame_work;

/* One row lane. `c` is the coding handle the CTU walk is given: a
 * shallow copy of the frame's n265_frame_work with the row-private
 * pointers swapped for this lane's storage. */
typedef struct n265_row_ctx {
    n265_frame_work c;
    cabac_ctx_t est[CTX_COUNT];
    struct n265_scratch *sc;
} n265_row_ctx;

/* K of sec 1: how many frames may be in flight at once. The array is
 * sized at the maximum; the live value is e->K, from N265_MAX_FRAME_WORK
 * (default 4). It is a scheduling knob and cannot change bits. */
#define N265_MAX_FRAME_WORK 8

struct n265_encoder {
    n265_param param;
    int mode;
    int qp_base;            /* configured QP (P anchors) */
    int crf_on;             /* n265_set_crf: base QP from crf, not qp_base */
    double crf;
    int abr_on;             /* n265_set_bitrate: the base is the ABR rate factor */
    double abr_bitrate;     /* bits per second                          */
    int qp_frac;            /* carry the base QP's fraction and spread it
                             * over the CTUs (N265_QP_FRAC; on under ABR) */
    int qp_delta_on;        /* cu_qp_delta_enabled_flag */
    n265_rc rc;             /* rc.c state                               */
    double rc_rf;           /* the base rate factor for the current frame */
    double rc_cplx;         /* its lookahead complexity                 */
    int rc_cls;             /* its RC_* class                           */
    /* the current mini-GOP's coding-order plan (pyramid): each entry is a
     * lookahead index with its kind (0 B, 1 P, 2 IDR), pyramid depth and
     * reference flag. Built once per mini-GOP, then drained one per call. */
    int plan_li[LA_MAX], plan_depth[LA_MAX];
    uint8_t plan_kind[LA_MAX], plan_nonref[LA_MAX];
    int plan_len, plan_pos, plan_pop;
    int ctb_log2;
    int no_sao;
    /* N265_DBF_BETA / N265_DBF_TC: slice_beta_offset_div2 and
     * slice_tc_offset_div2, signalled through the PPS. Both shift the
     * index into Table 8-12, so a negative offset filters less: fewer
     * segments pass the d < beta test at all, and the ones that do are
     * clipped to a smaller tC. Zero is the shipped default and the
     * control block stays out of the PPS entirely. */
    int dbf_beta, dbf_tc;

    int pad_w, pad_h;       /* dims padded up to a min-CB multiple  */
    int ctb_cols, ctb_rows;
    int map_stride;
    int pad_stride[3];

    struct n265_refpic dpb[N265_DPB_SLOTS];
    /* Recon pictures for non-reference frames. They reconstruct into
     * the same padded shape as a DPB slot (so every stride and every
     * read is the reference path's), but they are never `valid` and so
     * never enter an RPS, a reference list, or the eviction rule - which
     * is what keeps the DPB pool pressure exactly what it was before the
     * current picture started claiming a slot. */
    struct n265_refpic scratch_pic[N265_MAX_FRAME_WORK];
    int rect;
    int amp;
    int ref_max;            /* --ref: references per list wanted (1..N265_MAX_REF) */
    int badapt;             /* --b-adapt: shorten the B run on high-motion spans */
    float cutree_str;       /* N265_CUTREE_STR: cu-tree offset strength (default 2.0) */
    float aq_str;           /* N265_AQ_STR: per-CTU variance-AQ strength (default 0.7) */
    int qp_round1;          /* N265_QP_ROUND1: round the cu-tree + AQ field once */
    int ref_valid;

    int aq;                 /* adaptive quantization on/off            */
    int rdoq;
    double psy;
    int keyint;
    int bframes;
    int scenecut;
    int la_depth;
    int cutree;
    n265_la la;             /* lookahead queue + stats               */

    uint8_t *out;           /* Annex-B output for the current call   */
    size_t out_cap;
    /* A whole mini-GOP can leave in one call, and the ring may grow, so
     * the NAL payload pointers are patched from these offsets after the
     * last frame of the call has been packed. */
    n265_nal nal[N265_MAX_BFRAMES + 9];
    size_t nal_off[N265_MAX_BFRAMES + 9];
    int frames_done;
    int in_display_idx;     /* next arriving picture's display index */
    int gop_start_display;  /* display index of the current IDR      */

    n265_frame_work fw[N265_MAX_FRAME_WORK];
    int K;                  /* live frames in flight (N265_MAX_FRAME_WORK) */
    /* D4: the ABR observation lag, N265_RC_LAG, clamped to K - 1. This
     * one IS bitstream-affecting - it changes which frames' actual bits
     * frame k's rate factor is solved from - and it is the only reason K
     * can reach the bitstream at all: under ABR, K bounds the lag. At
     * the default 0, ABR frames do not overlap and K is inert. */
    int rc_lag;
    int fw_next;            /* the frame_work slot the next dispatch takes */
    /* the frames dispatched and not yet retired, in DISPATCH ORDER. The
     * driver owns this list; it is what the deferred-NAL ring walks, and
     * what D3's slot and source stalls test against. */
    n265_frame_work *inflight[N265_MAX_FRAME_WORK];
    int n_inflight;
    /* the VPS/SPS/PPS RBSPs, generated at the first frame's dispatch and
     * packed at its retirement so the NAL order is the serial one */
    uint8_t *hdr_rbsp[3];
    size_t hdr_len[3];
    int hdr_type[3];
    int nhdr;
    /* The deferred-NAL ring proper: a frame is JOINED (its job finished,
     * its NALs packed here, its slot and frame_work handed back) as soon
     * as a resource stall needs it, but EMITTED only when the API call
     * that owes it runs. That split is what lets n265_encoder_encode
     * return exactly the frames the serial encoder returned per call
     * while the driver runs as far ahead as K allows. */
    struct n265_done {
        uint8_t *buf;
        size_t cap, len;
        int nnal;
        int type[4];
        int size[4];
    } done[LA_MAX + N265_MAX_FRAME_WORK + 4];
    int n_done, done_head;
    /* T2 stall census (N265_STALL_TRACE): reference reads that had to
     * block, and frames dispatched, so the doc's table is per frame. */
    atomic_long ref_stalls, stall_frames;
    ntp_pool *pool;
    n265_row_ctx *row;      /* one lane per worker, plus the nested lanes */
    int nlanes;

    FILE *dump_decode;      /* decode-order recon tap (dec265 leg)   */
    FILE *dump_recon;       /* optional recon tap for the gate       */

    /* N265_CU_STATS: read once at open, copied into every frame_work.
     * The counters themselves are per-frame (see n265_frame_work). */
    int cu_stats;
};

/* --------------------------------------------------------------------
 * The exact reference stall (T2 gate decision G2).
 *
 * Every read of a reference picture is guarded by ref_wait on the bottom
 * LUMA line it touches, filter taps included. The row-start gate of
 * sec 1 is only a scheduling hint now (G1); this is what makes the read
 * safe, and it is why the threaded encoder can keep the design's "no
 * vertical MV clamp" promise against a motion search whose reach
 * composes along the raster (see the T2/3 refusal and its reach table).
 *
 * The fast path is one atomic load. It is an ACQUIRE load rather than a
 * relaxed one on purpose: the pixels behind the watermark are published
 * by the release store in the publish row task, and only the
 * release/acquire pair makes reading them defined - a relaxed load here
 * is a data race that TSan reports and that the gate does not carry.
 * On aarch64 that is one LDAPR/LDAR, which is what the 1-thread delta
 * measures.
 * ------------------------------------------------------------------ */

/* The published-row index a read whose bottom luma line is `line` needs.
 * Lines above the picture come from the top border, written when row 0
 * publishes; lines below it from the bottom border, written when the
 * last row publishes. Clamping covers both. */
static inline int n265_ref_need(const n265_frame_work *e, int line)
{
    if (line < 0)
        line = 0;
    else if (line >= e->pad_h)
        line = e->pad_h - 1;
    return (line >> e->ctb_log2) + 1;
}

static inline int n265_ref_have(const struct n265_refpic *p)
{
    return atomic_load_explicit((atomic_int *)&p->final_rows,
                                memory_order_acquire);
}

/* encoder.c: the slow half - count the stall and run older work. */
void n265_ref_stall(n265_frame_work *e, const struct n265_refpic *pic,
                    int need);

/* Guard a read of `pic` whose bottom luma line is `line`. Returns the
 * published-row count the read is now covered by, which is what the
 * REF_CHECK build turns into a hard bound on the lines the reader may
 * actually touch. */
static inline int n265_ref_wait(n265_frame_work *e,
                                const struct n265_refpic *pic, int line)
{
    int need = n265_ref_need(e, line);
    if (n265_ref_have(pic) < need)
        n265_ref_stall(e, pic, need);
    return need;
}

#ifdef N265_REF_CHECK
#include "encoder/refchk.h"
/* Arm the independent check: the reader may touch lines up to the last
 * one `need` published rows cover, and no further. A read that reaches
 * past it - or one that never armed a guard at all - aborts. */
static inline void n265_ref_guard(const n265_frame_work *e,
                                  const struct n265_refpic *pic, int need,
                                  int c_idx, const int16_t *hf)
{
    long lastl = need >= e->ctb_rows
        ? 1L << 30                      /* whole picture plus its borders */
        : ((long)need << e->ctb_log2) - 1;
    n265_refchk_open(pic->plane[c_idx], pic->stride[c_idx],
                     c_idx ? (lastl >> 1) : lastl);
    n265_refchk_open_hf(hf, pic->hf_stride, lastl);
}
#define N265_REF_GUARD(e, pic, need, c, hf) \
    n265_ref_guard((e), (pic), (need), (c), (hf))
#define N265_REF_UNGUARD()               n265_refchk_close()
/* the publish path reads its OWN picture; those reads are not gated */
#define N265_REF_OPEN_ALL()              n265_refchk_open_all()
#else
#define N265_REF_GUARD(e, pic, need, c, hf)  ((void)(need))
#define N265_REF_UNGUARD()                   ((void)0)
#define N265_REF_OPEN_ALL()                  ((void)0)
#endif

/* ctu.c: per-lane trial scratch */
struct n265_scratch *n265_scratch_new(void);
void n265_scratch_free(struct n265_scratch *sc);

/* encoder.c: start a row lane on this frame. Copies the frame's coding
 * context, re-points the row-private storage, and seeds the estimator
 * contexts from the frame's carry-over state. */
void n265_row_begin(n265_row_ctx *row, n265_frame_work *fw, int r);
/* per-file knob warming, on the driver at open (T0 audit item 2) */
void n265_ctu_warm(void);
void n265_mv_warm(void);
void n265_sao_warm(void);
void n265_rdoq_warm(void);
/* ...and hand back what the frame has to keep: the record stream the row
 * appended to and the estimator state the next row continues from. */
void n265_row_end(n265_row_ctx *row, n265_frame_work *fw, int r);

/* encoder.c: the DPB the reference structure needs at this --bframes /
 * --ref, in the SPS's own units (see the definition). */
void n265_dpb_requirement(int bframes, int ref_max, int *reorder,
                          int *buffering);

/* headers.c */
void n265_write_vps(n265_frame_work *e);
void n265_write_sps(n265_frame_work *e);
void n265_write_pps(n265_frame_work *e);

/* ctu.c: analyze one CTU (decisions + recon + records, no entropy) */
void n265_analyze_ctu(n265_frame_work *e, int ctb_x, int ctb_y);

/* entropy.c: replay one CTU's records through the CABAC engine */
void n265_entropy_ctu(n265_frame_work *e, cabac_t *cb, int ctb_x,
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
void n265_build_lists(struct n265_encoder *e, n265_frame_work *fw, int cur,
                      int is_b, struct n265_refpic *future);
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

/* The phase-fx cache plane's ORIGIN (picture (0,0)), which is what the
 * REF_CHECK guard measures lines against; n265_ref_hf returns a pointer
 * into it at the block. */
static inline const int16_t *n265_ref_hf_base(const struct n265_refpic *r,
                                              int fx)
{
    return fx ? r->hf[fx - 1] : NULL;
}

/* encoder.c, shared with ctu.c */
void n265_encoder_set_dump(n265_encoder *e, FILE *f);

#endif
