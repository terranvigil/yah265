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
     * this picture's own dispatch step, cleared at the dispatch after
     * this picture's last use in the mini-GOP plan (`last_use`, and at
     * the mini-GOP end for the tail), both in coding order, so frame
     * k+1's dispatch sees
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
    /* The last coding index in the CURRENT mini-GOP plan that references
     * this picture, or INT_MAX for one the plan carries into the next
     * mini-GOP (an anchor inside ref_max). Driver-serial: rewritten for
     * every marked picture when a plan is built, and set on the picture
     * itself when it is marked. The dispatch-time retirement drops
     * anything whose last use is behind the frame being dispatched, so
     * neither the RPS nor the decoder's DPB carries a picture that
     * nothing left in the plan can read (2026-09-10). */
    int last_use;
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
    /* Phase 6 round 3: the phase plane FINISHED, one u8 plane per cached
     * fractional (fx, fy). E1 left the vertical 8-tap per candidate; the
     * census read every reference 1.1 to 67 times per cached pixel, so
     * the vertical pass runs once per reference and every uni candidate
     * that lands on a cached phase is a copy. Built on top of hf[] and
     * so NULL whenever that is, and NULL per plane for a phase this
     * picture does not cache - which since round 4 is a per-reference
     * decision taken from measured traffic (N265_HF2). */
    uint8_t *hf2_base[12];
    uint8_t *hf2[12];           /* (fy - 1) * 4 + fx */
    /* Round 4: the same plane for the 14-bit (bi) form, holding the
     * UNROUNDED int16 that n265_mc_luma_14_hf would have written. Twice
     * the bytes of the u8 plane for a fifth of the traffic, so the same
     * traffic rule arms it against a higher bar. Indexed like hf2[]. */
    int16_t *hf2w_base[12];
    int16_t *hf2w[12];
    int hf2_stride;
    /* The region the 2-D planes cover. Set on EVERY claim, armed or not:
     * the traffic tally asks "would a plane have served this window?"
     * and that question must have the same answer whether or not the
     * plane is there, or the measurement depends on the decision it
     * feeds. Empty (x0 == x1) when the E1 cache is off. */
    int hf2_x0, hf2_x1, hf2_y0, hf2_y1;
    /* P2: the coding-tree depth range this picture used, per CTU;
     * 0xff where nothing filled it. The collocated entry is the
     * temporal half of the depth bound. */
    uint8_t *ctu_dmin, *ctu_dmax;
#ifdef N265_REF_CHECK
    /* The side-data half of N265_REF_CHECK. `gen` counts claims of this
     * slot (driver-serial, bumped in claim_slot); `row_gen[r]` carries
     * the generation that published CTB row r's side data, 0 until it
     * does. A reader that froze the generation at dispatch then proves
     * BOTH halves independently of the call site's ref_wait: the row it
     * touches was written, and it was written by the occupant it thinks
     * it is reading. See n265_side_check. */
    unsigned gen;
    unsigned *row_gen;
#endif
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
 * The only pointers a row copy re-points are est_ctx, the record buffer,
 * the scratch and (round 4) the MC traffic line; see n265_row_begin. */
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
    /* Round 4: luma MC output samples this frame asked for at each
     * fractional (fx, fy) INSIDE the 2-D cache region, i.e. what a
     * phase plane would have served had one been armed - counted
     * whether or not it was, so the number is a property of the content
     * and not of the arming. [0..11] the 8-bit (uni) form, [12..23] the
     * 14-bit (bi) one, in hf2[]'s (fy - 1) * 4 + fx order.
     *
     * `hf2_tr` is the lane's slice of `hf2_tr_rows` - one 24-entry row
     * per CTB row plus one for the frame handle itself - so a row task
     * writes only its own and no two lanes touch a line. n265_row_begin
     * re-points it, the way it re-points est_ctx and the record buffer.
     * The driver sums them in row order at retirement. */
    uint64_t *hf2_tr;
    uint64_t *hf2_tr_rows;
    int code_idx;           /* this frame's coding index (driver-serial) */
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
#ifdef N265_REF_CHECK
    /* the claim generation of every picture this frame may read side
     * data from, frozen with the lists at dispatch */
    unsigned ref_gen[2][N265_MAX_REF];
    unsigned col_gen;
#endif

    /* lookahead hand-off, frozen before the frame launches */
    float sc_ratio;         /* lookahead pcost/icost (P3) */
    float la_mvabs;         /* lookahead mean |MV| per lowres block (N265_CT_TRACE) */
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
    double ct_mean_raw;     /* the cu-tree field's own frame mean, QP units */
    double ct_mean_used;    /* the mean the per-CTU map was centred on (0
                             * under CRF, where the map stays un-centred)  */
    double ct_level;        /* the lookahead window's own level: the mean over
                             * the frames in it that receive propagation      */
    uint16_t *ctu_lam_q8;   /* per-CTU lambda multiplier, Q8 (256 = inert) */
    uint8_t *ctu_dmin, *ctu_dmax;   /* P2: per-CTU depth range used */
    struct sao_params *sao; /* per-CTB SAO decisions */
    uint8_t *sao_tmp[3];    /* one CTB row's pristine deblocked band */
    uint8_t *sao_line[3];   /* the row above's last line, pre-SAO */

    /* ---- row-private once a row copy exists ---- */
    int cur_ctu_qp;         /* QP of the CTU being analyzed */
    uint32_t cur_ctu_lam_q8;     /* its lambda multiplier, Q8 (256 = inert) */
    uint32_t cur_ctu_lam_sad_q8; /* the same in the SAD domain (its sqrt) */
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
    int sao_mask;           /* SAO types this frame may pick (sao.h) */
    int flat_pct, flat;     /* the content type (la.h): the flat-block
                             * share and whether it clears the gate */
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

/* Round 4: the traffic window the 2-D phase planes are armed from, and
 * the ring that holds it. The window ENDS at coding index k - K (the
 * newest frame guaranteed retired when index k is dispatched) and runs
 * back N265_HF2_WIN indices; eight is a whole mini-GOP at the default
 * --bframes 7, so the reference / non-reference mix inside it is the
 * sequence's rather than one position in the pyramid. The ring must
 * outlive that span plus the K frames retired ahead of it, so it is
 * sized well past K + WIN. */
#define N265_HF2_WIN  8
#define N265_HF2_HIST 32

struct n265_encoder {
    n265_param param;
    int mode;
    int qp_base;            /* configured QP (P anchors) */
    int crf_on;             /* n265_set_crf: base QP from crf, not qp_base */
    double crf;
    int abr_on;             /* n265_set_bitrate: the base is the ABR rate factor */
    double abr_bitrate;     /* bits per second                          */
    int qp_frac;            /* carry the base QP's fraction and spread it
                             * over the CTUs (N265_QP_FRAC; on under ABR,
                             * and under CRF where the cu-tree frame term
                             * is live - phase 7 follow-up 1) */
    int qp_frac_flat;       /* ...on a flat-typed frame too, where that term
                             * is zero (N265_QP_FRAC_FLAT; refused arm)  */
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
    /* per entry, the picture's POC and the last coding index in this plan
     * that references it - INT_MAX where the plan carries it forward. The
     * lifetime is copied onto the DPB picture when the frame is marked;
     * see n265_refpic.last_use. */
    int plan_poc[LA_MAX], plan_lastuse[LA_MAX];
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
    /* n265_dpb_requirement, solved once at open and copied into every
     * frame_work: sps_max_num_reorder_pics, the picture storage buffer
     * count the level has to hold, and the largest marked-picture count
     * the reference structure can reach (which is what arms
     * `slot_prefer`). */
    int dpb_reorder, dpb_buffering, dpb_peak;
    int badapt;             /* --b-adapt: shorten the B run on high-motion spans */
    float cutree_str;       /* N265_CUTREE_STR: cu-tree offset strength (default 2.0) */
    float ct_qpw;           /* N265_CT_QPW: the share of the field that reaches the QP,
                               on cutree_str's scale (default 2.0 = all of it) */
    float ct_lambda;        /* N265_CT_LAMBDA: per-CTU lambda modulation from the same
                               field, in QP units at strength 1 (default 0 = off) */
    float ct_framew;        /* N265_CT_FRAMEW: the share of the field's FRAME MEAN that
                               reaches the base QP under CRF, on cutree_str's scale.
                               1.0 ships (half the field's mean); k_f == ct_qpw is the
                               pre-item encoder, byte for byte */
    float ct_framew_flat;   /* N265_CT_FRAMEW_FLAT: the same on a flat-typed frame,
                               where today's form (= ct_qpw) ships */
    float ct_frame_clamp;   /* N265_CT_FRAME_CLAMP: bound on the frame term, in QP
                               steps (0 = unbounded) */
    int ct_frame_shot;      /* N265_CT_FRAME_SHOT: take the window level over this
                               frame's side of the nearest scenecut only */
    float ct_frame_up;      /* N265_CT_FRAME_UP: bound on the term's positive side
                               alone, in QP steps (negative = unbounded) */
    float ct_up_gate;       /* N265_CT_UP_GATE: the content gate on that bound -
                               the raise is released on a frame whose lookahead
                               sc_ratio is above this. 0 = gate off */
    int pb_cascade_on;      /* N265_PB_CASCADE=r,l is in force (CRF/ABR only;
                               "auto" is the 1 + b_depth ladder) */
    int pb_cascade_r;       /* its referenced-B step */
    int pb_cascade_l;       /* its leaf step */
    int pb_cascade_flat_on; /* N265_PB_CASCADE_FLAT: the same on a flat-typed
                               frame, where today's ladder ships */
    int pb_cascade_flat_r, pb_cascade_flat_l;
    float aq_str;           /* N265_AQ_STR: per-CTU variance-AQ strength (default 0.7) */
    int aq_signal;          /* N265_AQ_SIGNAL: 0 = log2(CTU variance), the
                               shipped masking signal; 1 = log2(`fine`), the
                               part of the variance its 2x-down plane does not
                               explain (phase 7 M1') */
    float aq_flat_sign;     /* N265_AQ_FLAT_SIGN: 0 = the per-CTU term stays
                               off on a flat-typed frame (N265_AQ_CLAMP_FLAT);
                               non-zero = on there with the PROTECTING sign,
                               at this strength */
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
    /* A call can hand back a mini-GOP plus whatever the previous call
     * left in flight (A1), so this is sized for two mini-GOPs and K plus
     * the three headers. Overflowing it is not an error: emit_done stops
     * where it runs out and the rest is the next call's prefix. */
    n265_nal nal[2 * (N265_MAX_BFRAMES + 1) + N265_MAX_FRAME_WORK + 8];
    size_t nal_off[2 * (N265_MAX_BFRAMES + 1) + N265_MAX_FRAME_WORK + 8];
    /* A1/A2: output is a prefix of the completed frames and up to K stay
     * in flight across calls. Off at one worker, where nothing overlaps
     * and a call must return exactly what it returned before.
     * `api_defer` is N265_API_DEFER, the gate knob that pushes the
     * chunking to the far end of the contract (see n265_encoder_encode). */
    int defer_out, api_defer;
    /* T3/1: may claim_slot prefer an unmarked slot no frame in flight is
     * still using, over the lowest-index one? Only where the reference
     * structure CANNOT fill every slot, so that the claim never reaches
     * the marked-slot fallback. See claim_slot for why that condition is
     * exactly what makes the preference bit-neutral. */
    int slot_prefer;
    /* N265_STALL_TRACE's driver half: why the driver had to join a frame
     * before it wanted to. `k` is K itself (the frame_work ran out),
     * `slot` is D3's DPB-slot stall, `src` its lookahead-source twin,
     * `rc` the ABR observation lag. This is what says whether the
     * overlap is capped by K or by the DPB. */
    int join_k, join_slot, join_src, join_rc;
    /* N265_LA_PROF: driver wall seconds per lookahead stage, and the
     * encode's own wall, so the serial fraction is a ratio of two
     * measured numbers rather than a guess (T3/2). */
    int la_prof;
    double prof_t0, prof_push, prof_plan, prof_cutree;
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
    /* Round 4, the traffic history the 2-D phase planes are armed from.
     * One entry per CODING INDEX, not a running sum: the arming rule
     * reads a fixed window of coding indices, so nothing depends on the
     * order retirements happened to take or on how many frames were in
     * flight when the reference was claimed. Every entry the rule reads
     * is provably written, because a frame is dispatched only after
     * n_inflight < K, which leaves every coding index at or below
     * k - K joined. K does not depend on the thread count, so neither
     * does the window, so neither does the mask. Output identity does
     * not rest on that - a plane that is not armed is simply not there
     * and n265_ref_2d returns NULL, so no arming can move a bit - but
     * REPRODUCIBLE TIMING does, and a rule whose answer drifted with
     * --threads could not be measured. `cidx` is stamped at retirement
     * and checked on the read, so a ring slot not yet written reads as
     * absent. */
    struct n265_hf2_hist {
        int cidx;           /* the coding index this entry holds, or -1 */
        int is_ref;         /* this frame claimed a DPB slot */
        uint64_t samp[24];  /* fw->hf2_tr, summed over the rows */
    } hf2_hist[N265_HF2_HIST];
    int hf2_code_n;         /* coding indices dispatched so far */
    long hf2_area;          /* samples one 2-D plane covers */
    unsigned hf2_mode;      /* N265_HF2: 0 off, 1 row, 2 all, 3 traffic */
    int hf2_tpp;            /* N265_HF2_TPP, hundredths of a touch      */
    int hf2_tpp14;          /* N265_HF2_TPP14                           */
    int hf2_trace;
    /* N265_HF2_TRACE only: the whole-encode totals the ring's window
     * cannot carry, and how many references armed each plane. */
    uint64_t hf2_armed[24];
    uint64_t hf2_samp[24];
    uint64_t hf2_nref;
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

/* The cu-tree field's raw per-CTU value carries cutree_str already, so the
 * share of it that reaches the QP is the ratio of the two strengths: 1.0
 * at the default, which leaves the offset map exactly where it was. */
static inline double n265_ct_qp_scale(const struct n265_encoder *e)
{
    return e->cutree_str != 0.0f ? (double)e->ct_qpw / (double)e->cutree_str : 0.0;
}

/* The cu-tree FRAME term, in QP units, for one frame under CRF/ABR: k_f of
 * the field's own frame mean, less whatever share of that mean the per-CTU
 * map is still carrying (k_b of it, since under CRF the map is un-centred).
 * k_f == k_b makes the two halves the same expression on the same double and
 * the term exactly 0, which is why the default encoder is byte-identical.
 * N265_CT_FRAMEW_FLAT is k_f on a flat-typed frame: the field's cross-frame
 * reading is a content trade there (docs/phase7-modulation-design.md
 * "Step 2 result"), so that branch keeps today's form by default. */
static inline double n265_ct_frame_term(const struct n265_encoder *e,
                                        const struct n265_frame_work *fw)
{
    if (e->cutree_str == 0.0f)
        return 0.0;
    double kf = fw->flat ? (double)e->ct_framew_flat : (double)e->ct_framew;
    /* the weight DIFFERENCE, on the frame's deviation from the window level
     * (la.c): k_f re-weights the frame-type step and never the shot's own
     * level. Written as a difference so k_f == k_b is exactly 0.0 - the same
     * double, subtracted from itself - which is what makes the default
     * bitstream main's byte for byte. */
    double t = (kf / (double)e->cutree_str - n265_ct_qp_scale(e)) *
                   (fw->ct_mean_raw - fw->ct_level) +
               n265_ct_qp_scale(e) * fw->ct_mean_used;
    /* N265_CT_FRAME_CLAMP (phase 7 step 3): a bound in QP steps on how far
     * the term may move one frame. A leaf's term is proportional to its
     * shot's OWN level, so unbounded it discounts a busy shot's leaves
     * further than a quiet shot's and moves bits between shots in proportion
     * to a distortion-domain estimate - which is what the multi-shot gate
     * refused in step 2. 0 is unbounded, step 2's form. */
    if (e->ct_frame_clamp > 0.0f) {
        double c = (double)e->ct_frame_clamp;
        if (t > c) t = c;
        else if (t < -c) t = -c;
    }
    /* N265_CT_FRAME_UP bounds the POSITIVE side alone - how far the term may
     * RAISE a frame's QP. The trace's finding: the term is positive on
     * referenced frames and negative on leaves, and the shot that pays in the
     * multi-shot gate is the most static one, whose leaves cannot use a lower
     * QP and whose anchors carry the picture. 0 forbids the raise outright;
     * negative is unbounded, the step 2 form. */
    double up = (double)e->ct_frame_up;
    /* N265_CT_UP_GATE (phase 7 follow-up 2): the bound above is one answer for
     * the whole encode, and the raise is worth 0.6 of band median where it
     * does not cost. sc_ratio is the lookahead's own pcost/icost for this
     * frame, low where the previous picture already predicts it; above the
     * threshold the bound comes off, at or below it the bound stands.
     *
     * The threshold is NOT a static/moving split, which is what the item was
     * briefed to build and what its own trace suggested. A gate that releases
     * the raise on every shot but the most static one reads the same
     * across-shot cost as no gate at all. What the raise charges for is the
     * low-ratio FRAME wherever it occurs, which is why 0.24 still gates two
     * thirds of a shot as busy as foreman.
     *
     * A flat-typed frame keeps its branch: the term is identically zero there
     * by default, and no phase 7 term has moved that clip class. 0 = off. */
    if (e->ct_up_gate != 0.0f && !fw->flat && fw->sc_ratio > e->ct_up_gate)
        up = -1.0;
    if (up >= 0.0 && t > up)
        t = up;
    return t;
}

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

/* --------------------------------------------------------------------
 * The side-data half of the independent check (2026-09-10).
 *
 * N265_REF_CHECK guarded the reference PLANE and the E1 cache from
 * inside the kernels and left the side-data arrays - the TMVP motion
 * field, the P2 depth map - covered by nothing but the call site's own
 * ref_wait, which is exactly the thing under suspicion. These two
 * helpers close it from the data end: a read of side-data CTB row `row`
 * of `pic` must find that row published AND find it published by the
 * generation the reader froze at dispatch. The row index is computed
 * from the ARRAY INDEX being read, never from the line the call site
 * passed to ref_wait, so an off-by-one between the two shows up here.
 * ------------------------------------------------------------------ */
#ifdef N265_REF_CHECK
void n265_side_check(const n265_frame_work *e, const struct n265_refpic *pic,
                     unsigned gen, int row, const char *what);
#define N265_SIDE_CHECK(e, pic, gen, row, what) \
    n265_side_check((e), (pic), (gen), (row), (what))
/* the generation frozen at dispatch for a picture in this frame's lists */
static inline unsigned n265_gen_of(const n265_frame_work *e,
                                   const struct n265_refpic *pic)
{
    if (pic == e->col)
        return e->col_gen;
    for (int l = 0; l < 2; l++)
        for (int k = 0; k < e->nref[l]; k++)
            if (e->refl[l][k] == pic)
                return e->ref_gen[l][k];
    return 0;   /* not in the frozen set at all: n265_side_check aborts */
}
#else
#define N265_SIDE_CHECK(e, pic, gen, row, what) ((void)0)
#endif

#ifdef N265_REF_CHECK
#include "encoder/refchk.h"
/* Arm the independent check: the reader may touch lines up to the last
 * one `need` published rows cover, and no further. A read that reaches
 * past it - or one that never armed a guard at all - aborts. */
static inline void n265_ref_guard(const n265_frame_work *e,
                                  const struct n265_refpic *pic, int need,
                                  int c_idx, const int16_t *hf,
                                  const uint8_t *p2, const int16_t *p2w)
{
    long lastl = need >= e->ctb_rows
        ? 1L << 30                      /* whole picture plus its borders */
        : ((long)need << e->ctb_log2) - 1;
    n265_refchk_open(pic->plane[c_idx], pic->stride[c_idx],
                     c_idx ? (lastl >> 1) : lastl);
    n265_refchk_open_hf(hf, pic->hf_stride, lastl);
    /* lastl - 4: the round-3 plane lags the reference by the four lines
     * its vertical filter reaches below each output line, so a watermark
     * that covers line L covers the 2-D plane only to L - 4. Asserting
     * the tighter bound here is what proves the lag. Round 4's int16
     * plane is filled in the same pass over the same rows, so it carries
     * the same bound. */
    n265_refchk_open_p2(p2, pic->hf2_stride, lastl - 4);
    n265_refchk_open_p214(p2w, pic->hf2_stride, lastl - 4);
}
#define N265_REF_GUARD(e, pic, need, c, hf) \
    n265_ref_guard((e), (pic), (need), (c), (hf), NULL, NULL)
/* the three luma sites that can be served straight out of the round-3
 * u8 plane guard that plane too */
#define N265_REF_GUARD2(e, pic, need, c, hf, p2) \
    n265_ref_guard((e), (pic), (need), (c), (hf), (p2), NULL)
/* ...and the bi site, which is served out of round 4's int16 plane */
#define N265_REF_GUARD3(e, pic, need, c, hf, p2w) \
    n265_ref_guard((e), (pic), (need), (c), (hf), NULL, (p2w))
#define N265_REF_UNGUARD()               n265_refchk_close()
/* the publish path reads its OWN picture; those reads are not gated */
#define N265_REF_OPEN_ALL()              n265_refchk_open_all()
#else
#define N265_REF_GUARD(e, pic, need, c, hf)  ((void)(need))
#define N265_REF_GUARD2(e, pic, need, c, hf, p2)  ((void)(need))
#define N265_REF_GUARD3(e, pic, need, c, hf, p2w)  ((void)(need))
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
/* N265_RECT_FLAT (ctu.c): whether a flat-typed frame keeps the
 * rectangular inter partitions where N265_NO_RECT deletes them
 * everywhere else. Read on the driver, in dispatch_frame. */
int n265_rect_flat(void);
/* per-file knob warming, on the driver at open (T0 audit item 2) */
void n265_encoder_warm(void);
void n265_ctu_warm(void);
void n265_mv_warm(void);
void n265_sao_warm(void);
void n265_rdoq_warm(void);
/* ...and hand back what the frame has to keep: the record stream the row
 * appended to and the estimator state the next row continues from. */
void n265_row_end(n265_row_ctx *row, n265_frame_work *fw, int r);

/* encoder.c: the DPB the reference structure needs at this --bframes /
 * --ref, in the SPS's own units (see the definition). */
/* the QP a frame of this kind will be coded at, for the lookahead's bits
 * model (`--b-adapt 4`); defined beside dispatch_frame, which applies it */
int n265_planned_qp(const struct n265_encoder *e, int slice_type,
                    int b_depth, int nonref);

void n265_dpb_requirement(int bframes, int ref_max, int *reorder,
                          int *buffering, int *peak, int *markpeak);

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
 * reads 3 rows above and 4 below, so 60 covers every clamped MV. A
 * gathered window (n265_ref_win, for the unclamped merge and AMVP
 * candidates) always misses the cache and interpolates from the copy,
 * which is the same bytes. */
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

/* The finished block at luma phase (fx, fy) and integer position (x, y),
 * or NULL when the round-3 plane cannot serve it: off, a phase this run
 * does not cache, fy = 0, or a window outside the covered region. The plane holds
 * exactly what the vertical pass would have written, so a caller that
 * takes this pointer copies and is byte-identical to one that filters.
 *
 * The covered rows are three short at the top and four short at the
 * bottom of what hf[] covers, because that is the window the vertical
 * filter reads; n265_clamp_mv admits integer positions in
 * [-(N265_REF_PAD - 8), pic + N265_REF_PAD - 8) = [-56, pic + 56), which
 * fits inside it with a line to spare at each end. */
static inline const uint8_t *n265_ref_2d(const struct n265_refpic *r,
                                         int x, int y, int w, int h,
                                         int fx, int fy)
{
    if (!fy || !r->hf2[(fy - 1) * 4 + fx])
        return NULL;
    if (x < r->hf2_x0 || x + w > r->hf2_x1 ||
        y < r->hf2_y0 || y + h > r->hf2_y1)
        return NULL;
    return r->hf2[(fy - 1) * 4 + fx] + (ptrdiff_t)y * r->hf2_stride + x;
}

/* That plane's ORIGIN, for the REF_CHECK guard; NULL where there is none. */
static inline const uint8_t *n265_ref_2d_base(const struct n265_refpic *r,
                                              int fx, int fy)
{
    return fy ? r->hf2[(fy - 1) * 4 + fx] : NULL;
}

/* Round 4, part B: the same window in the 14-bit form, the unrounded
 * int16 the bi path averages. Same coverage test - the two planes are
 * filled over the same region from the same hf[] rows. */
static inline const int16_t *n265_ref_2d14(const struct n265_refpic *r,
                                           int x, int y, int w, int h,
                                           int fx, int fy)
{
    if (!fy || !r->hf2w[(fy - 1) * 4 + fx])
        return NULL;
    if (x < r->hf2_x0 || x + w > r->hf2_x1 ||
        y < r->hf2_y0 || y + h > r->hf2_y1)
        return NULL;
    return r->hf2w[(fy - 1) * 4 + fx] + (ptrdiff_t)y * r->hf2_stride + x;
}

static inline const int16_t *n265_ref_2d14_base(const struct n265_refpic *r,
                                                int fx, int fy)
{
    return fy ? r->hf2w[(fy - 1) * 4 + fx] : NULL;
}

/* Round 4: tally one luma MC request against the phase it lands on, for
 * the arming rule. The test is the COVERAGE test alone and never looks
 * at whether a plane exists, so the count a frame reports is the same
 * whatever this run armed - which is what stops the decision from being
 * a function of itself. Row-private storage, so no atomics and no order.
 *
 * `is14` selects the bi form's half of the array; the two forms are
 * counted apart because they want different planes and clear different
 * bars (the int16 plane is twice the bytes for the same fill). */
static inline void n265_hf2_tally(struct n265_frame_work *e,
                                  const struct n265_refpic *r,
                                  int x, int y, int w, int h,
                                  int fx, int fy, int is14)
{
    if (!fy || !e->hf2_tr)
        return;
    if (x < r->hf2_x0 || x + w > r->hf2_x1 ||
        y < r->hf2_y0 || y + h > r->hf2_y1)
        return;
    e->hf2_tr[(is14 ? 12 : 0) + (fy - 1) * 4 + fx] += (unsigned)(w * h);
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
/* round 4: resolve the 2-D phase-plane knobs into the encoder at open */
void n265_hf2_knobs(n265_encoder *e);

#endif
