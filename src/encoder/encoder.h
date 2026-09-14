/*
 * Encoder internals.
 * Copyright (c) 2026, the yah265 authors. GPL-2.0-or-later; see LICENSE.
 */

#ifndef N265_ENCODER_H
#define N265_ENCODER_H

#include <stdatomic.h>
#include <stdio.h>

#include "yah265.h"
#include "common/pixel.h"
#include "threadpool/ntp.h"
#include "common/bitstream.h"
#include "encoder/cabac.h"
#include "encoder/grain.h"
#include "encoder/hash.h"
#include "encoder/hrd.h"
#include "encoder/la.h"
#include "encoder/metrics.h"
#include "encoder/mcstf.h"
#include "encoder/nr.h"
#include "encoder/signal.h"
#include "encoder/level.h"
#include "encoder/rc.h"
#include "encoder/rcfile.h"
#include "encoder/twopass.h"
#include "encoder/vbv.h"
#include "encoder/wp.h"
#include "encoder/tables.h"

struct sao_params;

enum {
    N265_MODE_INTRA = 0,    /* real coding: DC intra, CTB 32 */
    N265_MODE_PCM = 1,      /* phase-0 path: I_PCM, CTB 16 */
};

#define MIN_CB_LOG2 3
/* ---- the chroma format, a RUNTIME property (phase 8 item 6a) ---------
 *
 * `chroma_idc` is the SPS's chroma_format_idc itself: 1 for 4:2:0, 2 for
 * 4:2:2 and 3 for 4:4:4 (N265_CSP_* in the public header carry the same
 * numbers). Unlike the sample width, the chroma format does NOT change
 * the pixel type, so it is a field and not a build option and one library
 * codes all three.
 *
 * `csx` / `csy` are log2(SubWidthC) / log2(SubHeightC): a chroma position
 * is `luma >> csx`, a chroma plane is `pad_w >> csx` wide, and a chroma
 * transform block is `log2 luma - csx` on a side. At 4:2:0 both are 1 and
 * every expression that reads them folds back to the `/ 2` and `- 1` the
 * encoder wrote before this item, which is what makes the 4:2:0 output
 * byte-identical. 4:2:2 is the first format where the two DIFFER (1 and
 * 0), so anything that read one for both axes - the reference border, the
 * chroma CTB's z-scan - had to split them; item 6a named each of those.
 *
 * `c444` is ChromaArrayType == 3 and `c422` is ChromaArrayType == 2, and
 * they are SEPARATE predicates on purpose: the spec conditions its
 * format-dependent clauses on ChromaArrayType and not on the subsampling
 * factors. The transform tree's chroma leaf (7.3.8.8/7.3.8.10), the
 * chroma QP mapping (8.6.1), the scan derivation (7.4.9.11), the
 * reference-sample filtering (8.4.4.2.3) and Table 8-3's chroma mode
 * remap all read one or the other.
 */
#define N265_CSX_OF(idc) ((idc) == 3 ? 0 : 1)
#define N265_CSY_OF(idc) ((idc) == 1 ? 1 : 0)

/* Samples in one picture of this geometry: 3/2 * w * h at 4:2:0 and
 * 3 * w * h at 4:4:4. Every buffer bound that used to be written
 * `w * h * 3 / 2` reads this instead, the recon dump's frame stride
 * included - that one is load-bearing, since the dump IS the conformance
 * gate's reference and a frame landing at the wrong offset is a silent
 * mismatch (phase 8 item 2 learned it at the sample WIDTH). */
static inline size_t n265_frame_samples(int w, int h, int csx, int csy)
{
    return (size_t)w * (size_t)h +
           2u * (size_t)(w >> csx) * (size_t)(h >> csy);
}

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
/* The reference-picture pool, as an ARRAY BOUND. 11 since 2026-09-12
 * (item dpbpool): the widest shape the CLI allows - `--ref 3 --bframes
 * 15` - can hold eleven pictures marked at once (`n265_dpb_requirement`'s
 * structural peak: ref_max carried anchors, this anchor, and the
 * mini-GOP's seven reference Bs), and a pool smaller than the marked set
 * sends `claim_slot`'s last branch over a picture still in the RPS.
 *
 * It is NOT how many slots an encode allocates or scans - that is
 * `e->dpb_slots`, solved at open, and it is six wherever six suffice:
 * every shape that shipped before this item, the deep window at `--ref 1`
 * and `2` included. Those encodes run the same six-slot pool they always
 * did, bit for bit and byte for byte of RSS. */
#define N265_DPB_SLOTS 11
/* The pool every shape ran before it was sized. Kept as the FLOOR so no
 * shape that fit in six moves a slot choice. */
#define N265_DPB_POOL_MIN 6
/* blk_qp marker: this CU coded no cu_qp_delta, so its QP is whatever the
 * frame-serial qPY_PREV chain holds when the map fill reaches it
 * (n265_qpmap_row). Analysis cannot know that value under a row
 * wavefront; the fill can, and it is a pure function of the QP map and
 * the markers. */
#define N265_QP_INHERIT 0xff
/* ...and what it stores otherwise: the map is uint8_t and QpY goes
 * NEGATIVE at Main10 (down to -QpBdOffsetY), so a block carries Qp' =
 * QpY + QP_BD_OFFSET, 0..63, which leaves 0xff free at both depths and is
 * the bare QpY at 8 bits. Everything that READS the map converts back,
 * because the two consumers - deblocking's Table 8-12 index and the
 * prediction of the next quantisation group - both want QpY.
 * A bias is safe under the averaging deblock.c does: it is constant, and
 * ((a+B) + (b+B) + 1) >> 1 is ((a+b+1) >> 1) + B exactly. */
#define N265_QP_STORE(q) ((q) + QP_BD_OFFSET)
#define N265_QP_LOAD(v)  ((int)(v) - QP_BD_OFFSET)
#define N265_MAX_REF 3
/* --bframes clamp; also sizes the mini-GOP walk the SPS's DPB derivation
 * runs. 15 since 2026-09-12 (phase 7 item 3, D3): a mini-GOP of 16 is
 * what the opt-in deep window is for. The default is still 7 and the
 * pyramid ladder is still `1 + b_depth`, which extends from 2/3/4 to
 * 2/3/4/5 on its own. */
#define N265_MAX_BFRAMES 15

/* The DPB picture: a padded, border-extended, fully filtered
 * reconstruction plus the side data a later frame reads from it. The
 * frame being coded reconstructs DIRECTLY into its claimed slot
 * (docs/threading-design.md sec 4), so there is no commit copy; the
 * slot stays `valid = 0` until publish, which is what keeps the picture
 * out of its own RPS and out of its own reference lists. */
struct n265_refpic {
    pixel *base[3];
    pixel *plane[3];    /* origin inside the border */
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
    pixel *hf2_base[12];
    pixel *hf2[12];             /* (fy - 1) * 4 + fx */
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
    /* The SOURCE this picture was coded from, as the duplicate chain
     * identifies it (la.h): the run id, and the running lowres total the
     * threshold form measures a distance against. Written at claim, on
     * the driver, so a frame dispatched later reads the occupant its
     * lists name. 0 is a slot no armed encode has claimed and never
     * matches anything. */
    int dup_grp;
    uint64_t dup_cum;
    /* --weightp / --weightb: this occupant's own lowres SOURCE planes,
     * the three the weight estimator reads. Written at claim on the
     * driver, for the same reason `dup_grp` is: the lookahead slot that
     * held them is recycled behind a dispatched frame, so a frame that
     * names this picture in a reference list three mini-GOPs later would
     * otherwise be reading whatever source has since moved in. NULL
     * unless one of the two flags is on, which is what keeps the default
     * encoder's allocation exactly what it was. */
    pixel *wp_low[3];
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

/* N265_CBIT_TRACE buckets: the frame's coded bits charged to the syntax
 * that carries each component (docs/phase7-psy-design.md, the chroma gap) */
enum {
    N265_CB_Y = 0,          /* luma residual_coding()                 */
    N265_CB_CB,             /* cb residual_coding()                   */
    N265_CB_CR,             /* cr residual_coding()                   */
    N265_CB_CBF,            /* cbf_cb / cbf_cr                        */
    N265_CB_OTHER,          /* everything else the replay writes      */
    N265_CB_COUNT
};

/* N265_CU_TRACE (phase 9 step 6a): one record per CODING-TREE NODE the
 * quadtree visits, so that where the search spends its time can be read
 * against what the CU it was spent on ended up being.
 *
 * The instrument only READS - every field below is a number the encoder
 * had already computed, or a cycle count taken around code that runs
 * either way - so the encode is byte-identical with the variable set and
 * without it, which the item's gate verifies by `cmp` on three clips at
 * both rate modes and both thread counts.
 *
 * Written by the row job into the node's own slot under its CTU, dumped
 * on the driver at retirement in coding order, so the FILE is a pure
 * function of the encode except for the cycle columns, which are what
 * the census reads its shares off.
 *
 * `kind` is what analyze_cu decided at THIS node; whether the node is a
 * final CU is the quadtree's business and `flags` bit 0 carries it, so
 * the census reconstructs the tree from (x, y, log2n) in visit order.
 *
 * The layout is fixed and padding-free on purpose: scripts read it with
 * one struct format and no C compiler. */
struct n265_cu_rec {
    uint32_t cyc_tot;       /* ticks in this node, children included   */
    uint32_t cyc_kids;      /* ...of which the split arm's children    */
    uint32_t cyc_merge;     /* the merge list, the pick and both merge
                             * arms' TB analyses - the work no early
                             * skip exit could avoid                   */
    uint32_t cyc_amvp;      /* the AMVP search and its TB trial        */
    uint32_t cyc_intra;     /* the 35-mode rank and the intra RD trial */
    uint32_t cyc_part;      /* rect / AMP                              */
    uint32_t cyc_pre;       /* the merge LIST and the SAD pick, inside
                             * cyc_merge: the only work a static skip
                             * commit still has to do, so it is what a
                             * before-the-merge-arm predicate's saving
                             * has to be net of                        */
    uint32_t cand_sad;      /* the merge candidate's plain luma SAD    */
    uint32_t static_thr;    /* n*n*N265_SKIP_THR, the static-skip bound,
                             * in the eighths cand_sad*8 is tested
                             * against                                 */
    uint32_t d_skip;        /* the forced skip's three-component SSD   */
    uint32_t thr_clean;     /* skip_clean_thr(qp, n), its bound        */
    uint32_t icost, pcost;  /* the lookahead's sums over the node      */
    float prop;             /* the cu-tree propagate-in, mean per block */
    uint16_t x, y;          /* the node's luma origin in the picture   */
    uint16_t nt_merge, nt_amvp, nt_intra, nt_part;  /* TB analyses run */
    uint8_t log2n, depth;
    uint8_t kind;           /* N265_CUT_*                              */
    uint8_t flags;          /* N265_CUF_*                              */
    uint8_t colskip;        /* percent of the node's 4x4 cells the
                             * PREVIOUS FRAME IN CODING ORDER coded skip
                             * at the same position; 255 = no previous */
    uint8_t qp, slice_type, rsv;
};

enum {
    N265_CUT_SKIP = 0,      /* the CU ends as a skip                   */
    N265_CUT_MERGE,         /* merge with residual                     */
    N265_CUT_AMVP,
    N265_CUT_INTRA,
    N265_CUT_PART,          /* a rectangular or asymmetric partition   */
    N265_CUT_OUTSIDE,       /* the node is off the picture             */
    N265_CUT_FRAME = 0xff   /* the per-frame header record             */
};

enum {
    N265_CUF_SPLIT   = 1u,  /* the quadtree kept the SPLIT arm here    */
    N265_CUF_STATIC  = 2u,  /* the strict static-skip exit took the CU
                             * before a single TB was analysed         */
    N265_CUF_GAMVP   = 4u,  /* the AMVP arm was gated away             */
    N265_CUF_GINTRA  = 8u,  /* the intra arm was gated away            */
    N265_CUF_CLEAN   = 16u, /* the winner was a CLEAN skip             */
    N265_CUF_NORES   = 32u, /* the winner carried rqt_root_cbf 0       */
    N265_CUF_FLAT    = 64u, /* the frame is typed flat                 */
    N265_CUF_SKIPWIN = 128u /* j_skip <= j_merge at the point the
                             * merge-first skip exit asks - the conjunct
                             * N265_SKIP_EXIT carries, without which a
                             * bound on d_skip alone reads far looser
                             * than the gate it would go into           */
};

/* nodes per CTU: 1 + 4 + 16 + 64 covers a 64x64 coding tree down to the
 * 8x8 minimum, which is every shape the quadtree can visit */
#define N265_CU_REC_MAX 85

/* N265_DEPTH_DUMP (phase 7 item 4, step 0): the per-CTU training record.
 * Everything above `dmin` is a PRE-DECISION feature - it comes off the
 * lookahead's lowres maps, the frame's own source luma or the AQ field,
 * all of which are frozen before the CTU is analysed - and `dmin`/`dmax`
 * are the depth range the unbounded quadtree actually used, which is the
 * thing a bound has to predict. The instrument is inert: it only reads. */
struct n265_depth_rec {
    float ctqp;             /* the cu-tree field, QP units, before rounding */
    float lvar;             /* log2(CTU source luma variance + 1)           */
    float fine;             /* log2(var - var of the 2x-down plane + 1)     */
    float ratio;            /* sum(pcost) / sum(icost) over the 16 blocks   */
    float mvmean, mvspread; /* lowres |MV| mean and (max - min) over them   */
    uint16_t imean, imax;   /* lowres intra cost, mean and max              */
    uint16_t pmean, pmax;   /* lowres inter cost, mean and max              */
    int8_t qp;              /* the CTU's final QP (QpY, negative below 0 at Main10) */
    uint8_t dmin, dmax;     /* the target                                   */
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
    /* chroma_format_idc and its derived geometry; see N265_CSX_OF */
    int chroma_idc;
    int csx, csy;
    int c444;
    int c422;
    int pad_w, pad_h;       /* dims padded up to a min-CB multiple */
    int ctb_cols, ctb_rows;
    int map_stride;         /* in 4x4 units */
    int rect;               /* rectangular inter PUs, SMP (N265_NO_RECT off) */
    int amp;                /* asymmetric inter PUs; also the SPS flag,
                             * which changes part_mode's binarization
                             * whether or not an AMP mode is chosen */
    int rdoq;               /* N265_RDOQ: RD-optimised quantisation */
    /* N265_RDOQ=2: the lattice runs on the COMMIT path only, and this is
     * the predicate that says which path a tb_analyze_one call is on. It
     * is set and cleared around the commit-side analyses in ctu.c, is
     * per-frame-work state that never crosses a worker, and so splits
     * trial from commit the same way on one thread and on twelve.
     * Meaningless at N265_RDOQ 0 and 1, where the quantiser does not
     * depend on it. */
    int rdoq_commit;
    /* --lossless: transquant_bypass_enabled_flag in the PPS and
     * cu_transquant_bypass_flag on every CU, so the residual IS the coded
     * level (8.6.2's bypass branch). The ONE predicate every site reads -
     * the TB path, the entropy writer, the sign-hiding argument, both
     * loop filters. */
    int lossless;
    int qp_delta_on;        /* cu_qp_delta_enabled_flag: the ONE predicate
                             * the PPS and the entropy writer both read */
    int bframes;
    /* What the reference structure asks of a decoder's DPB, derived from
     * --bframes and --ref at open (n265_dpb_requirement). `dpb_reorder`
     * is sps_max_num_reorder_pics; `dpb_buffering` is the picture storage
     * buffer count, i.e. sps_max_dec_pic_buffering_minus1 + 1, and the
     * level is picked to hold it. */
    int dpb_reorder, dpb_buffering;
    /* general_level_idc and general_tier_flag, solved once at open from
     * the whole of Annex A (src/encoder/level.c) and carried here so
     * write_ptl writes a number rather than re-deriving one. */
    int level_idc, level_tier;
    int vui_timing;         /* --vui-timing: vui_timing_info in the SPS */
    /* The pass-through signalling (signal.h): the VUI's colour
     * description, the aspect ratio, the display window and the HDR10
     * metadata. Copied here at dispatch like every other config field, so
     * the SPS writer and the SEI builder read one struct and no global. */
    n265_signal sig;
    /* hrd_parameters (E.2.2) in the VUI: the CPB schedule the VBV clamp
     * actually enforces, so a decoder can check it. 0 - the value every
     * stream before phase 12 item 2 carried - writes
     * vui_hrd_parameters_present_flag 0 and nothing after it. */
    int hrd_on;
    unsigned hrd_bit_rate_scale, hrd_bit_rate_value;    /* E.2.3's BitRate */
    unsigned hrd_cpb_size_scale, hrd_cpb_size_value;    /* ...and CpbSize  */
    int hrd_cbr;            /* --strict-cbr: E.2.3's cbr_flag[0]        */
    int tlayers_minus1;     /* --temporal-layers: sps_max_sub_layers_m1 */
    int tid;                /* this picture's TemporalId (0, or 1 on a
                             * non-reference B leaf under the flag)     */
    double psy;             /* N265_PSY: psychovisual energy strength */
    int dbf_beta, dbf_tc;   /* slice_beta/tc_offset_div2, see n265_encoder */
    /* --cbqpoffs / --crqpoffs: pps_cb_qp_offset / pps_cr_qp_offset, the
     * PPS half of the chroma quantiser's offset. See n265_encoder. */
    int cb_qp_off, cr_qp_off;

    /* ---- per-frame: source, recon, slice ---- */
    const pixel *pad_plane[3];      /* the lookahead slot's padded source */
    int pad_stride[3];
    pixel *recon[3];        /* aliases slot->plane[] */
    int recon_stride[3];
    struct n265_refpic *slot;       /* the DPB slot being reconstructed into */

    int qp;                 /* this frame's slice QP */
    int qp_forced;          /* --qpfile / --zones q=N pinned it: the
                             * cascade, the base's fraction and the AQ
                             * frame mean are all off, because a user who
                             * writes q=30 means thirty */
    int poc;
    int slice_type;         /* 2 = I, 1 = P, 0 = B */
    /* Is this picture an IRAP (an IDR)? Every I slice was one until the
     * non-IDR intra picture landed, so the four places that mean IRAP -
     * the RPS, the two IDR-only slice-header fields and the NAL type -
     * read THIS, and everything else keeps reading `slice_type == 2`,
     * which still means "coded without prediction". */
    int idr;
    /* ...and the other IRAP this encoder writes since phase 14 item 3:
     * a CRA (7.4.2.2). `idr` and `cra` are exclusive and `irap` is the
     * disjunction, which is what the syntax keys on wherever the spec
     * says IRAP rather than IDR - no_output_of_prior_pics_flag, the
     * buffering period, the per-IRAP metadata SEIs. What a CRA does NOT
     * do is reset the POC or empty the DPB, which is the whole of open
     * GOP; the prediction barrier in encoder.c is what replaces the
     * flush. */
    int cra;
    int irap;
    /* This picture is a LEADING picture of the last CRA: it follows that
     * CRA in decoding order and precedes it in output order. 0 neither,
     * 1 RASL (may reference across the CRA, discarded on a seek to it),
     * 2 RADL (may not, and so is decodable there). Frozen at dispatch
     * with the lists, and it picks the NAL type and nothing else. */
    int leading;
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
    /* N265_CBIT_TRACE: the entropy pass's own bits (Q8) charged per
     * component, plus the chroma cbf population by transform-tree node
     * size. The cost is read off the REAL context state before the real
     * engine updates it, so the attribution is the estimator's -log2(p)
     * of exactly the bins that were written. Diagnostic, output-neutral,
     * off unless the variable is set. */
    int cbit_trace;
    int cbit_bucket;                /* bucket the next bin lands in */
    uint64_t cbit[N265_CB_COUNT];
    uint64_t cbf_c_n[4], cbf_c_cb[4], cbf_c_cr[4];  /* by log2ts - 2 */
    uint64_t cbit_cnz;              /* nonzero chroma levels coded */
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
    /* N265_INTRA_WIN_TRACE only, and laid out exactly like the traffic
     * line above: four counters per CTB row - the CUs the intra-gate
     * family reached, the ones where the arm stood down, the 35-mode
     * ranks that ran and the RD trials that ran - so the arm's work can
     * be split by slice type without a profiler and without the wall.
     * Zero cost when the trace is off: the increments sit behind it. */
    uint64_t *iw_arm;
    uint64_t *iw_arm_rows;
    int iw_trace;
    /* --nr-intra / --nr-inter (phase 14 item 6). Laid out exactly like
     * the two lines above and for the same reason: `nr_tally` is the
     * LANE's slice of `nr_rows` - one tally per CTB row plus one for the
     * frame handle - so a row task writes only its own and no two lanes
     * touch a cache line, and the driver sums them in ROW order at
     * retirement. Integer sums, so the total is the same at one worker
     * and at twelve whatever order the lanes ran in.
     *
     * `nr_off` is this frame's FROZEN offsets, a copy taken at dispatch:
     * the driver's own table moves under every dispatch and a frame in
     * flight must not see it move. `nr_on` is the single predicate every
     * quantiser call reads, and it is 0 unless a strength was named. */
    int nr_on;
    uint16_t *nr_off;
    n265_nr_tally *nr_tally;
    n265_nr_tally *nr_rows;
    /* the counters are also what N265_INTRA_GATE=10 measures itself
     * with, so they run whenever either is on */
    int iw_count;
    /* N265_INTRA_GATE=10: decided on the driver at dispatch, from the
     * retired window only, and frozen before a row job exists. 1 = this
     * frame's floor population stands the intra arm down even though
     * the slice is not a B. */
    int iw_gate;
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
    /* 7.3.6.3's table, derived at dispatch (src/encoder/wp.c).
     *
     * `wp_on` is the PPS flag THAT APPLIES TO THIS SLICE TYPE -
     * weighted_pred_flag for a P slice, weighted_bipred_flag for a B -
     * and it is what says whether the slice header carries a
     * pred_weight_table and whether 8.5.3.3.4.3 is the prediction. It is
     * NOT `wp_any`: with the flag on and every weight at its identity
     * the decoder still runs the explicit clause, which is arithmetically
     * the default one (mc.h), so the samples agree and the syntax does
     * not. `wp_any` says only whether anything actually fired, and the
     * MC path reads it to skip the weighted kernels altogether. */
    int wp_on, wp_any;
    int wp_ldenom, wp_cdenom;
    /* the two PPS flags themselves, copied from the encoder with the
     * rest of the per-frame config: the PPS writer takes a frame_work */
    int weightp, weightb;
    n265_wp wp[2][N265_MAX_REF];
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
    const int16_t *la_mv;   /* lowres MV pairs, same grid (N265_DEPTH_DUMP) */
    const float *la_prop;   /* cu-tree propagate-in, same grid (N265_CU_TRACE
                             * and N265_STATIC_COMMIT's second conjunct) */
    int la_bw, la_bh;
    /* N265_STATIC_COMMIT (phase 11 item 4): the coded-skip map of the
     * retired frame nearest this one in DISPLAY order, resolved on the
     * driver at dispatch and frozen before a row job exists. NULL where
     * the arm is off or no entry qualifies, which is every CU's test. */
    const uint8_t *sc_map;
    /* ...and the propagate conjunct, as one byte per lookahead block:
     * 1 where the cu-tree propagate-in is exactly zero.
     *
     * This is a COPY and it has to be. `la_prop` points into
     * `la->f[i].prop`, and n265_la_cutree zeroes and recomputes every
     * frame in the view each time the window slides - so a frame that is
     * still coding when the next one is prepared has its propagate
     * rewritten under it, and what a CU reads depends on how far ahead
     * the driver got. At one worker it reads the frame's own values and
     * at twelve it reads a later window's, which is a bitstream that
     * moves with `--threads`. (The existing N265_CU_TRACE `prop` column
     * reads the live array and has the same exposure; it is diagnostic
     * and no branch depends on it, so it has never mattered.) */
    uint8_t *sc_prop;
    int sc_prop_n;
    /* The DUPLICATE path (phase 11 item 1, docs/phase11.md). `dup_grp`
     * and `dup_cum` are this frame's own entries in the lookahead's
     * duplicate chain (la.h).
     *
     * `dup_msk[l]` is the SET of references in list l whose source is
     * this frame's source, one bit per reference index, resolved on the
     * driver at dispatch out of the lists and the slots' own ids - so
     * every CTU reads a decision frozen before a row job existed. A set
     * and not one reference, because a picture inside a long hold has
     * duplicates on BOTH sides and the prediction that averages two of
     * them is a real candidate the rule must be able to take (the
     * census: sita_flat_720p is 105 duplicate transitions in seven runs,
     * and its leaves are coded bi).
     *
     * `dup_l` / `dup_r` name the first of them, which is what the census
     * measures the coded area against, and are -1 when the set is empty.
     * `dup_exact` says at least one of them compared equal sample for
     * sample rather than inside the threshold form's bound. */
    int dup_grp;
    uint64_t dup_cum, dup_zsad;
    uint32_t dup_msk[2];
    int dup_l, dup_r, dup_exact;

    /* N265_DEPTH_DUMP: one record per coded CTU, filled by the row job
     * into its own slot and written out on the driver at retirement, so
     * the file is in raster order at every thread count. Allocated only
     * when the knob is set; NULL is the shipping encoder. */
    struct n265_depth_rec *dd_rec;

    /* N265_CU_TRACE: N265_CU_REC_MAX slots per CTU plus a count each,
     * filled by the row job that owns the CTU and written out on the
     * driver at retirement. NULL is the shipping encoder and every site
     * that touches them is a NULL test. */
    struct n265_cu_rec *cu_rec;
    uint16_t *cu_rec_n;

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
    pixel *sao_tmp[3];      /* one CTB row's pristine deblocked band */
    pixel *sao_line[3];     /* the row above's last line, pre-SAO */

    /* ---- row-private once a row copy exists ---- */
    int cur_ctu_qp;         /* QP of the CTU being analyzed */
    uint32_t cur_ctu_lam_q8;     /* its lambda multiplier, Q8 (256 = inert) */
    uint32_t cur_ctu_lam_sad_q8; /* the same in the SAD domain (its sqrt) */
    /* N265_CHROMA_W: this CTU's chroma distortion weight, Q8 (256 =
     * inert). Derived once per CTU from cur_ctu_qp and its chroma QP;
     * see chroma_w_for_qp in ctu.c. */
    uint32_t cur_chroma_w_q8;
    int db_lo, db_hi;       /* P2: depth range this quadtree may search */
    const pixel *cu_pred[3];        /* PU-by-PU MC result a partitioned
                                     * inter CU's TBs crop from; NULL for
                                     * 2Nx2N */
    int cu_pred_stride[3];
    int cu_pred_x0, cu_pred_y0;     /* luma origin of those planes */
    int last_skip_clean;    /* analyze_cu: skip won with low distortion */
    int last_cu_nores;      /* analyze_cu: the winner had rqt_root_cbf 0 */
    /* N265_CU_TRACE, row-private: the CTU whose slots this lane is
     * filling, the open node stack inside its quadtree, and which arm of
     * analyze_cu the TB analyses arriving now belong to. */
    int cu_ctu, cu_sp, cu_arm;
    struct n265_cu_rec *cu_stk[8];
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
    /* The film grain SEI this access unit carries, already an RBSP, or
     * length 0. Prefix SEI, so it is packed after the parameter sets and
     * before this frame's slice. */
    size_t sei_len;
    uint8_t sei[N265_FGC_SEI_MAX];
    /* ...and the HDR10 static metadata beside it (phase 14 item 1): the
     * mastering display, the content light level and the alternative
     * transfer characteristic, all in ONE prefix SEI RBSP, in every IRAP
     * access unit. 64 bytes holds the three messages with their headers
     * three times over. */
    size_t hdr_sei_len;
    uint8_t hdr_sei[N265_HDR_SEI_MAX];
    int nal_type;
    /* rate control hand-off, frozen at dispatch (sec 3 rule 4) */
    int rc_cls;
    double rc_cplx, rc_base;
    /* the frame's INTRA cost, for the VBV's predictor alone. n265_rc_cplx
     * caps an inter frame's complexity at its intra cost, so the two are
     * EQUAL exactly on a frame the motion search does no better than intra
     * on - a shot change coded as a P - and that is the frame the per-class
     * scale prices six times too cheap. rc.c reads the pair. */
    double rc_icplx;
    double rc_planned;      /* D4: the fair share rc_next handed this frame */
    double rc_ctf;          /* the cu-tree frame term inside rc_base: the loop
                             * is told it going in and reads it back out of
                             * the coded frame (rc.c). 0 with cu-tree off. */
    /* --pass 1's three lookahead columns, frozen here with everything else
     * and spent at retirement. Summed at dispatch rather than read from the
     * queue later, because the slot is recycled: by the time the frame
     * retires its lowres planes may belong to a picture eight frames on.
     * All three are 0 unless --pass 1 armed the sidecar. */
    double st_icost, st_pcost, st_prop;
    double rc_ioff;         /* the planned intra QP offset inside rc_base
                             * (N265_ABR_IOFF), which the measured operating
                             * point takes back out. 0 on inter frames, 0 at
                             * the default, 0 whenever ABR is off. */
    /* The VBV's hand-off, frozen at dispatch with everything else here
     * and spent at retirement, where the coded size is known and the
     * trace line is written. All three are 0 with the mode off, which is
     * every encode that does not name --vbv-maxrate. */
    double vbv_pred;        /* bits the model predicted for this frame at the
                             * base it was about to be coded at, BEFORE the
                             * clamp */
    double vbv_allow;       /* the most the buffer could pay for */
    double vbv_dqp;         /* the QP the clamp added to the base; 0 where the
                             * plan already fitted */
    double shot_off;      /* --shot-window: this frame's shot plan, in QP
                             * steps, already inside rc_base. 0 with the
                             * mode off, which is every default encode. */

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
    /* N265_SERIAL_PROF=2: the frame's own milestones, seconds since the
     * encoder opened. Each is written by exactly one task (row 0, the
     * last publish row, the entropy task) or by the driver, so they race
     * with nothing and are never read by a predicate. LAST in the struct
     * on purpose: an instrument must not move a hot field's offset. */
    double t_disp, t_row0, t_rowlast, t_ent0, t_ent1;
} n265_frame_work;

int n265_chroma_qp(int qp_y, int chroma_idc);   /* encoder/transform.h */

/* QpC for ONE chroma component (8.6.1).
 *
 * The chroma offsets go into qPi BEFORE Table 8-10 and never after it -
 * qPiCb = Clip3(-QpBdOffsetC, 57, QpY + pps_cb_qp_offset + ...), then
 * QpCb = the table of that - so an offset does not simply shift the
 * chroma QP by its own value: inside the table's flat stretch two
 * different qPi map to one QpC. n265_chroma_qp does the Clip3 itself, so
 * the sum is handed to it unclamped.
 *
 * `comp` is 1 for Cb and 2 for Cr, the component numbering every caller
 * in ctu.c already passes to tb_analyze. The two planes take DIFFERENT
 * QPs the moment the two offsets differ, which is why the coding path
 * carries two values where it used to carry one. At the default offsets
 * this is n265_chroma_qp(qp_y, chroma_idc), expression for expression. */
static inline int n265_chroma_qp_of(const n265_frame_work *e, int qp_y,
                                    int comp)
{
    return n265_chroma_qp(qp_y + (comp == 1 ? e->cb_qp_off : e->cr_qp_off),
                          e->chroma_idc);
}

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

/* The shot sidecar's re-sort window: how far a retiring frame's display
 * index may sit ahead of the oldest one not yet folded. Coding order
 * reorders by at most a mini-GOP, and the whole lookahead window plus
 * the frames in flight bounds that from well above; this is the `done`
 * ring's own size, for the same reason. */
#define N265_SHOT_RING (LA_MAX + N265_MAX_FRAME_WORK + 4)

/* Round 4: the traffic window the 2-D phase planes are armed from, and
 * the ring that holds it. The window ENDS at coding index k - K (the
 * newest frame guaranteed retired when index k is dispatched) and runs
 * back N265_HF2_WIN indices; eight is a whole mini-GOP at the default
 * --bframes 7, so the reference / non-reference mix inside it is the
 * sequence's rather than one position in the pyramid. The ring must
 * outlive that span plus the K frames retired ahead of it, so it is
 * sized well past K + WIN. */
/* N265_INTRA_WIN_TRACE's per-row counters: reached, gated, ranks,
 * trials, the inter J over the population and the J the intra arm saved
 * inside the gateable half of it. */
#define N265_IW_CNT   6

/* The adaptive intra gate's own history (N265_INTRA_GATE=10). One entry
 * per coding index, stamped at retirement, read at dispatch over the
 * window ending at k - K - the newest index guaranteed retired - exactly
 * the way hf2_traffic_mask reads its traffic window and for exactly the
 * same reason: K is a scheduling constant that does not move with
 * --threads, so the set of frames the rule reads is the same at one
 * worker and at twelve and the bitstream cannot drift with the thread
 * count. The ring must outlive K + the window. */
#define N265_IW_HIST  128

#define N265_HF2_WIN  8
#define N265_HF2_HIST 32

/* N265_STATIC_COMMIT's ring of coded-skip maps (phase 11 item 4), and
 * the window inside it the rule may read.
 *
 * The window is closed at BOTH ends, and the lower end is not
 * bookkeeping - it is half the determinism argument. A ring keyed on
 * `cidx % N265_SC_HIST` loses an old entry the moment the entry 32
 * coding indices later is written, and HOW MANY entries have been
 * written when frame k is dispatched depends on the thread count: at one
 * worker the retired prefix runs to k - 1, at twelve it may stop at
 * k - 8. An open-ended "everything at or below k - 8" therefore reads a
 * DIFFERENT SET at the two, because the newest writes have destroyed
 * different old entries - which is exactly what the first build of this
 * arm did, and t1 and t12 disagreed on every clip.
 *
 * So the rule reads coding indices k - 8 - 15 .. k - 8 and nothing else.
 * Every one of those is retired at every thread count (K <=
 * N265_MAX_FRAME_WORK), and none of them can have been overwritten,
 * because the entry that would do it is k + 9 and frame k has not been
 * coded yet. N265_SC_HIST >= N265_SC_WIN + N265_MAX_FRAME_WORK + 1 is
 * that second half, stated as a number. */
#define N265_SC_HIST 32
#define N265_SC_WIN  16

/* --nr-intra / --nr-inter's ring of finished-frame coefficient tallies
 * (phase 14 item 6). The rule is sc_resolve's, shortened: a frame's
 * tally is FOLDED into the running estimate at the dispatch of coding
 * index cidx + N265_NR_LAG, and at that moment cidx is retired at every
 * thread count, because a frame is dispatched only once n_inflight is
 * below K and K never exceeds N265_MAX_FRAME_WORK. The fold is therefore
 * in coding order, once per frame, over the same set of frames at one
 * worker and at twelve - which is the whole of why the offsets a frame
 * quantises with do not depend on --threads.
 *
 * The ring only has to hold an entry from its own retirement to that
 * dispatch, which is at most LAG entries deep; 16 is the same power of
 * two the rings above use and is twice what is needed. `cidx` is stamped
 * and checked, so an entry that was never written reads as absent rather
 * than as a stale frame's coefficients. */
#define N265_NR_LAG  N265_MAX_FRAME_WORK
#define N265_NR_HIST 16

struct n265_encoder {
    n265_param param;
    int mode;
    int chroma_idc;         /* chroma_format_idc: 1 = 420, 2 = 422, 3 = 444 */
    int csx, csy;           /* log2 SubWidthC / SubHeightC */
    int c444;               /* ChromaArrayType == 3 */
    int c422;               /* ChromaArrayType == 2 */
    int qp_base;            /* configured QP (P anchors) */
    int crf_on;             /* n265_set_crf: base QP from crf, not qp_base */
    double crf;
    int abr_on;             /* n265_set_bitrate: the base is the ABR rate factor */
    double abr_bitrate;     /* bits per second                          */
    /* --vbv-maxrate / --vbv-bufsize / --vbv-init: the hypothetical decoder
     * buffer the encode is held inside (vbv.h). `on` is 0 unless both of
     * the first two were named, and that zero is what every encode before
     * phase 12 item 2 runs - the clamp, the level's CPB leg and the HRD in
     * the VUI are all gated on it. Rides ABR and CRF alike: under ABR it
     * is a second constraint on a loop that already has a target, under
     * CRF it is x265's capped CRF, where the rate factor stands until the
     * buffer says otherwise. */
    n265_vbv vbv;
    /* --pass / --stats (twopass.h, phase 12 item 4). `pass` 0 is every
     * encode before this item and every encode that does not ask: nothing
     * below reads a plan, nothing writes a sidecar, and the encoder is the
     * one main ships. */
    n265_2p tp;
    int vbv_trace;          /* N265_VBV_TRACE: one row per frame, coding
                             * order, written at retirement */
    int vbv_hrd;            /* N265_VBV_HRD: write hrd_parameters in the VUI
                             * (default 1, and inert without the VUI) */
    /* E.2.3's two scaled pairs, solved once at open from the VBV the clamp
     * enforces and rounded UP - a declared BitRate below what the encoder
     * assumed, or a declared CpbSize below the buffer it planned against,
     * would be a claim the stream does not honour. */
    int hrd_on;
    unsigned hrd_bit_rate_scale, hrd_bit_rate_value;
    unsigned hrd_cpb_size_scale, hrd_cpb_size_value;
    /* ...and the same two decoded back out of E.2.3. That is what the SPS
     * CLAIMS, so it is what A.4.2's MaxBR and MaxCPB have to admit - the
     * rounding above can only ever push it up, and a --vbv-maxrate sitting
     * exactly on a level's bound would otherwise be signalled over it. */
    int64_t hrd_bit_rate_bits, hrd_cpb_size_bits;
    /* The HRD timing SEIs (hrd.h, phase 12 item 5), and the two coding
     * indices they are counted from. Both are driver state, moved in
     * CODING order inside join_oldest, which is what makes the messages a
     * function of the plan and not of the schedule: `hrd_bp_code` is the
     * coding index of the access unit that carried the most recent
     * buffering period (-1 before the first), `hrd_cvs_code` the coding
     * index of the IDR that opened the current coded video sequence. */
    bitstream_t hrd_bs;     /* the scratch this AU's message is built in */
    int hrd_bp_code, hrd_cvs_code;
    int qp_frac;            /* carry the base QP's fraction and spread it
                             * over the CTUs (N265_QP_FRAC; on under ABR,
                             * and under CRF where the cu-tree frame term
                             * is live - phase 7 follow-up 1) */
    int qp_frac_flat;       /* ...on a flat-typed frame too, where that term
                             * is zero (N265_QP_FRAC_FLAT; refused arm)  */
    int qp_delta_on;        /* cu_qp_delta_enabled_flag */
    int lossless;           /* --lossless (n265_set_lossless)           */
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
    /* --cbqpoffs / --crqpoffs (x265's names): pps_cb_qp_offset and
     * pps_cr_qp_offset, -12..12 each, 0 by default and 0 at every preset.
     * The spec puts them into qPi BEFORE Table 8-10 (8.6.1), so what they
     * move is the chroma QUANTISER itself and not a cost - which is the
     * whole difference between these and N265_CHROMA_W. At 0 the
     * derivation is the one that ran before they existed, expression for
     * expression, and the PPS writes the same two se(v) zeroes it always
     * wrote. */
    int cb_qp_off, cr_qp_off;

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
    /* --weightp / --weightb: weighted_pred_flag and
     * weighted_bipred_flag. Resolved at open from the setter, then
     * N265_WEIGHTP / N265_WEIGHTB, then the preset table. x265 ships
     * weightp on at medium and weightb off, and so do we. */
    int weightp, weightb;
    /* the lowres planes of the frame being dispatched, built once on the
     * driver and copied onto its DPB slot at claim. Sized like the
     * lookahead's lowres luma (la->lw x la->lh) and, for chroma, a 2x2
     * mean of the chroma source. NULL with both flags off. */
    pixel *wp_cur[3];
    int wp_cw, wp_ch;

    struct n265_refpic scratch_pic[N265_MAX_FRAME_WORK];
    int rect;
    int amp;
    int ref_max;            /* --ref: references per list wanted (1..N265_MAX_REF) */
    /* n265_dpb_requirement, solved once at open and copied into every
     * frame_work: sps_max_num_reorder_pics, the picture storage buffer
     * count the level has to hold, and the largest marked-picture count
     * the reference structure can reach (which is what arms
     * `slot_prefer`). `dpb_slots` is the pool this shape runs: the floor
     * (six) wherever the marked set fits in it, the structural peak
     * where it does not. Every slot scan and every slot allocation in
     * encoder.c is bounded by it, and N265_DPB_SLOTS only sizes arrays. */
    int dpb_reorder, dpb_buffering, dpb_peak, dpb_slots;
    /* Annex A, solved once at open (src/encoder/level.c): the level and
     * the tier the SPS and the VPS carry, and every bound that drove
     * them, which is what N265_LEVEL_TRACE prints. */
    n265_level_out level;
    int vui_timing;         /* --vui-timing: vui_timing_info in the SPS */
    /* The pass-through signalling flags (signal.h), as the setters left
     * them at open. Copied into every frame_work at dispatch; nothing
     * here is read on a worker. */
    n265_signal sig;
    bitstream_t hash_bs;    /* the scratch the picture hash is built in */
    /* --repeat-headers / --aud / --eos / --eob / --info (phase 14 item 4):
     * the NALs an access unit may carry besides its own. Every one of them
     * is OFF by default, `--info` included, so the default bitstream is
     * byte-for-byte the one this encoder wrote before they existed - which
     * is what keeps an identity cmp against an older binary meaningful.
     * x265 writes its --info SEI by DEFAULT; the audit's row says so. */
    int repeat_hdr, aud, eos, eob, info;
    /* --idr-recovery-sei / --temporal-layers (phase 14 item 4). The
     * recovery point is one prefix SEI per IRAP; the temporal layer puts
     * the non-reference B leaves in sub-layer 1, which is the only
     * bitstream change in this group that touches a VCL NAL HEADER. */
    int recovery_sei, tlayers, hrd_concat;
    bitstream_t rec_bs;
    /* The rate-control exposure (phase 14 item 4). Every one of these is
     * inert at its default: the QP bounds are the spec's own range and
     * are only APPLIED when the user narrowed one, the CRF bounds are
     * only read under --crf, the AU cap is a fraction of 1, and strict
     * CBR needs --bitrate and a VBV before it exists at all. */
    int qpmin, qpmax, qp_bounds;
    double crf_min, crf_max;
    int crf_bounds;
    int strict_cbr;
    double ausize_factor;
    n265_qpfile qpf;
    n265_zones zones;
    int qpf_on, zones_on;
    bitstream_t fd_bs;      /* the filler data RBSP, rebuilt per AU */
    long filler_bytes;      /* what strict CBR had to pad, in total */
    long filler_aus;
    bitstream_t aud_bs;     /* one byte, rewritten per access unit */
    bitstream_t info_bs;    /* the version SEI, built once at open */
    int tail_done;          /* the trailing EOS/EOB have been emitted */
    long frames_out;        /* access units packed, for "not the first" */
    /* --film-grain / --film-grain-denoise (phase 12 item 3). OPT-IN and
     * nothing but: strength 0 is the default, and at 0 the estimator is
     * never opened, no picture is touched and no SEI is written, which is
     * what makes a knob-off encode byte-identical to one built without
     * this code. The model is estimated once, on the driver, and written
     * into every IRAP access unit with the persistence flag set. */
    double fgc_strength;
    int fgc_denoise;
    int fgc_ready;          /* the model has been finalised */
    int fgc_nacc;           /* pictures folded into the estimate */
    n265_grain_est *fgc_est;
    n265_grain_model fgc;
    /* --nr-intra / --nr-inter (phase 14 item 6): the coefficient-domain
     * noise reduction. Both default 0 and at 0 nothing below is
     * allocated, no frame carries an offset table and tb_quant_one takes
     * the branch it always took - which is what makes the escape
     * byte-identical rather than merely equal.
     *
     * `nr` is the running estimate, owned by the driver. `nr_hist` is the
     * ring above: a finished frame's tally lands in it at retirement and
     * is folded LAG coding indices later. Neither is ever read by a row
     * worker; the frame gets a frozen COPY of the offsets (fw->nr_off),
     * because the driver rewrites `nr->off` at every dispatch while
     * frames are still quantising. */
    /* --mcstf / --mcstf-ref-range / --selective-mcstf (phase 14 item 6).
     * NULL unless the flag was named; when it is, it is a queue in FRONT
     * of the lookahead and the only thing in the encoder that knows about
     * it is the input path (mcstf.h). Off is the default, x265's default
     * too, and off is a NULL pointer and one predicate. */
    n265_mcstf *mcstf;
    int mcstf_range, mcstf_selective;
    int nr_intra, nr_inter;
    n265_nr *nr;
    n265_nr_hist *nr_hist;
    /* N265_DPB_TRACE: claims that found no unmarked slot and overwrote a
     * marked one (the T0a DPB-pressure branch). Driver-serial. The pool
     * sizing above is what makes it zero on every shape it covers. */
    long slot_overwrite;
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
    /* How many frames the queue FILLS to before the driver dispatches:
     * `la_depth` at the default, `shot_window` where that is deeper. The
     * two are different numbers only in the opt-in shot-planning mode,
     * whose whole point is to hold a shot the 20-frame window cannot
     * (la.h, docs/phase7-shot-design.md step 2). */
    int la_fill;
    /* --shot-window N / N265_SHOT_WINDOW: 0 = off, and off is the
     * default, the board and every preset. Opt-in, CRF only, N frames of
     * latency and N queue slots of memory. */
    int shot_window;
    double shot_k;          /* N265_SHOT_K: the offset's gain */
    double shot_clamp;      /* N265_SHOT_CLAMP: |offset| bound, QP steps */
    double shot_ema;        /* N265_SHOT_EMA: how fast the level follows */
    double shot_level;      /* the running complexity level, per frame */
    int shot_have_level;    /* seeded by the first shot planned */
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
    int gop_start_display;  /* display index of the current KEY FRAME */
    /* The POC origin: the display index the POC line is measured from.
     * It moves at an IDR and at NOTHING ELSE, because 8.3.1 resets
     * PicOrderCntMsb at an IDR and at nothing else. Until open GOP
     * landed every key frame was an IDR and this was gop_start_display;
     * a CRA moves that one (the min-keyint bound and the cu-tree parity
     * both measure from the last key frame) and leaves this one alone,
     * which IS the continuous POC open GOP is about. */
    int poc_origin_display;
    /* The prediction barrier the last CRA put down, in POC, or INT_MIN
     * before the first one. A picture with POC below it may be listed
     * only by a picture with POC below it - that is 8.3.2's RASL rule
     * and, read the other way, the guarantee that no trailing picture
     * of the CRA references anything the decoder does not have after a
     * seek to it. Driver-serial, read by n265_build_lists and by the
     * retirement's POC mirror, so it is a function of POC and marking
     * alone like every other list rule here. */
    int cra_poc;
    /* The same two, as the LIST RULE reads them. They are cra_poc and
     * radl_poc, except under N265_OPENGOP_NOBARRIER, which leaves them
     * at INT_MIN and so writes a stream whose NAL types claim an open
     * GOP while its trailing pictures predict across the CRA anyway.
     * That is the exact non-conformance this item exists to avoid, and
     * it is the negative control the seek check is armed against
     * (docs/phase14.md item 3). Diagnostic only; never a shipped path. */
    int cra_bar_poc, cra_bar_radl;
    int open_gop;           /* --open-gop: a keyint picture is a CRA */
    int radl;               /* --radl: leading pictures coded RADL   */
    /* ...and the second barrier `--radl` puts down inside the first: a
     * picture whose POC is at or above `radl_poc` and below `cra_poc`
     * is a RADL, and may list only pictures at or above `radl_poc`.
     * Equal to cra_poc (an empty band) when --radl is 0. */
    int radl_poc;
    /* display index of the last INTRA picture, IDR or not: what the
     * minimum distance between intra pictures (N265_INTRA_MIN) is
     * measured from, where N265_MIN_KEYINT is measured from the IDR
     * above. Equal to gop_start_display until a non-IDR I is placed. */
    int last_intra_display;

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
        /* SIXTEEN. pack_nal DROPS a NAL past this bound and says
         * nothing, so a bound one short does not fail loudly - it
         * silently loses the IDR slice and the stream decodes as "could
         * not find ref with POC 0". It was eight, and it was found too
         * short TWICE by exactly that symptom: once at phase 14 item 1
         * (VPS, SPS, PPS, the HRD timing pair, the HDR metadata, the
         * grain SEI, the slice, the hash), and again at item 4, whose
         * flags add five more NALs to the same access unit - the end of
         * sequence, the access unit delimiter, the recovery point, the
         * version SEI, and strict CBR's filler.
         *
         * The worst IRAP this encoder can write is now: EOS, AUD, VPS,
         * SPS, PPS, recovery point, the HRD timing pair, the HDR
         * metadata, the version SEI, the grain SEI, the slice, the hash
         * suffix SEI, filler, EOB - fourteen. Sixteen leaves room for two
         * more before the next item has to count again, and the cost is
         * 64 bytes per ring slot.
         *
         * If a third item makes this list longer, make the bound a
         * derived constant instead of raising it: the failure is silent
         * by construction and no gate that runs one flag at a time can
         * see it. The 10-bit all-flags smoke of item 4 is what found it. */
        int type[16];
        int size[16];
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

    /* --psnr / --ssim / --csv (phase 14 item 4). Another pure tap on
     * join_oldest, in coding order, on the driver: the metrics read the
     * reconstruction and the source and write nothing the encoder reads
     * back, so the bitstream is byte-identical with any of them on. */
    n265_metrics met;
    n265_csv csv;
    double met_bytes;       /* coded bytes, for the CSV's summary row */
    long met_frames;

    /* N265_CU_STATS: read once at open, copied into every frame_work.
     * The counters themselves are per-frame (see n265_frame_work). */
    int cu_stats;
    int cbit_trace;         /* N265_CBIT_TRACE, the same way */
    /* N265_CU_TRACE: the census's output file, and the skip map of the
     * PREVIOUS FRAME IN CODING ORDER. Both are driver-only - the map is
     * copied out of each frame at retirement, where join_oldest has
     * already serialised coding order - so the co-located-skip column is
     * the same integer at one worker and at twelve. */
    FILE *cu_trace_f;
    uint8_t *cu_prev_skip;
    int cu_prev_valid;
    /* N265_INTRA_GATE=10's history. `armed` says the frame ran the intra
     * arm over its floor population, so its J pair is a MEASUREMENT; a
     * frame the rule gated reports nothing and is skipped on the read,
     * which is what keeps the statistic from being blinded by its own
     * gate: once the window holds no armed frame of the type the rule
     * cannot fire, the next frame of that type measures again, and the
     * duty cycle is the window's own length. */
    struct n265_iw_hist {
        int cidx;           /* the coding index this entry holds, or -1 */
        int slice_type;
        int armed;
        uint64_t j_inter, j_saved;
    } iw_hist[N265_IW_HIST];
    int iw_adapt;           /* N265_INTRA_GATE == 10 */
    int iw_win;             /* N265_INTRA_ADAPT_K, coding indices  */
    int iw_thr;             /* N265_INTRA_ADAPT_THR, tenths of a % */
    /* N265_STATIC_COMMIT (phase 11 item 4): the coded-skip maps its
     * co-located conjunct reads. One entry per coding index, written at
     * retirement - where join_oldest has already serialised coding order
     * - and read at DISPATCH, over the entries whose coding index is at
     * or below k - N265_MAX_FRAME_WORK.
     *
     * The lag is the COMPILE-TIME maximum in flight and not `e->K`,
     * which is the whole determinism argument and the one place this
     * rule differs from hf2_traffic_mask's window. `e->K` moves with
     * `--threads` (encoder.c, the T3/4 bump), and that is allowed there
     * only because the traffic mask decides which half-pel planes to
     * cache and a cache cannot move a bit. This rule DOES move bits, so
     * it may only read a set that the thread count cannot change: at
     * most K - 1 frames are in flight when a frame is dispatched and
     * K <= N265_MAX_FRAME_WORK always, so coding index k - 8 is retired
     * at every thread count and every K. */
    struct n265_sc_hist {
        int cidx;           /* the coding index this entry holds, or -1 */
        int didx;           /* its display index, for the nearest pick  */
        uint8_t *map;       /* per 4x4 cell, cu_skip_flag               */
    } sc_hist[N265_SC_HIST];
    size_t sc_cells;        /* map_stride * (pad_h >> 2), 0 until known */
    /* N265_INTRA_WIN_TRACE: one CSV row per frame at retirement carrying
     * the share of the picture whose final mode was intra, which is the
     * quantity an adaptive intra gate would condition on. Driver-side
     * and output-neutral - unlike N265_INTRA_ORACLE, which un-gates the
     * arm and changes the bitstream by construction. */
    int iw_trace;

    /* --shot-stats / N265_SHOT_STATS: the per-shot JSON Lines sidecar
     * (docs/phase7-shot-design.md step 1). Frames retire in CODING
     * order and a non-IDR I is its own mini-GOP's anchor, so a shot's
     * opener can retire before leaves that belong to the shot before
     * it; the ring below re-sorts retired frames into DISPLAY order and
     * the accumulator folds them there. Nothing here is read back by
     * the encoder: it is a pure tap on join_oldest, and the bitstream
     * is byte-identical with the sidecar open or not. */
    FILE *shot_f;
    struct n265_shot_rec {
        int present;
        int slice_type, idr, cra, nonref, flat;
        double qp_base, cplx;
        uint64_t bytes;
    } shot_ring[N265_SHOT_RING];
    int shot_next;          /* next display index to fold */
    int shot_high;          /* one past the highest display index retired */
    int shot_open;          /* 0 none yet, 1 "start", 2 "idr", 3 "intra",
                             * 4 "cra" */
    int shot_first, shot_last;
    int shot_cnt[4];        /* i, p, b_ref, b_leaf */
    int shot_n, shot_flat;
    uint64_t shot_bytes;
    double shot_qp_sum, shot_cplx;
    /* N265_SERIAL_PROF (phase 9 step 4): the rest of the driver's own
     * wall, so the serial share at auto threads is a measured number and
     * not the residue of one. `prof_join` is the driver BLOCKED in
     * ntp_join and is therefore NOT serial work - it is the pool's wall
     * seen from the driver - and every other column is. LAST in the
     * struct on purpose: an instrument must not move a hot field's
     * offset. */
    int sprof;
    double prof_dispatch, prof_join, prof_retire, prof_emit;
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
                                  const pixel *p2, const int16_t *p2w)
{
    long lastl = need >= e->ctb_rows
        ? 1L << 30                      /* whole picture plus its borders */
        : ((long)need << e->ctb_log2) - 1;
    n265_refchk_open(pic->plane[c_idx], pic->stride[c_idx],
                     c_idx ? (lastl >> e->csy) : lastl);
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
/* N265_STATIC_COMMIT (ctu.c): the static commit's bound, as a multiple
 * of the strict static-skip one; 0 is off. Read on the driver too, in
 * sc_resolve, which is what decides whether the ring is kept at all. */
int n265_static_commit(void);
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
                          int *buffering, int *peak, int *markpeak,
                          int *slots);

/* headers.c */
void n265_write_vps(n265_frame_work *e);
void n265_write_fgc_sei(bitstream_t *bs, const n265_grain_model *m);
void n265_write_hrd_sei(bitstream_t *bs, const n265_hrd_sei *v);
size_t n265_write_hdr_sei(bitstream_t *bs, const n265_signal *s);
void n265_write_hash_sei(bitstream_t *bs, const n265_pic_hash *h, int ncomp);
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
/* The open-GOP prediction barrier and the POC-level mirror of the list
 * rule (phase 14 item 3). Both are pure functions of POC - no encoder,
 * no DPB, no dispatch order - which is what lets tests/unit/gop_test.c
 * read them against a hand-computed mini-GOP, and what makes the
 * retirement's answer provably the list builder's. encoder.c is their
 * only caller inside the library. */
int n265_cra_visible(int cra_poc, int radl_poc, int cur, int p);
void n265_ref_used(int cra_poc, int radl_poc, const int *m, int nm, int cur,
                   int is_b, int ref_max, uint8_t *used);
uint64_t n265_zorder(int x, int y, int pic_w, int ctb_log2);
#define N265_REF_PAD 64     /* reference plane border, luma samples */
/* ...and what that border is on ONE plane, per axis. It was a single
 * number until 4:2:2, where SubWidthC and SubHeightC differ: a chroma
 * plane there is half as wide as the luma one and exactly as tall, so its
 * border is half horizontally and whole vertically. n265_ref_win takes
 * the pair for the same reason. At 4:2:0 and 4:4:4 the two are equal and
 * this is the one expression that was written before. */
static inline int n265_ref_pad_x(int c_idx, int csx)
{
    return c_idx ? N265_REF_PAD >> csx : N265_REF_PAD;
}
static inline int n265_ref_pad_y(int c_idx, int csy)
{
    return c_idx ? N265_REF_PAD >> csy : N265_REF_PAD;
}
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
static inline const pixel *n265_ref_2d(const struct n265_refpic *r,
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
static inline const pixel *n265_ref_2d_base(const struct n265_refpic *r,
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
/* --shot-stats: the sidecar path, overriding N265_SHOT_STATS. NULL or
 * the empty string turns it off. */
void n265_encoder_set_shot_stats(n265_encoder *e, const char *path);
/* --csv <file> --csv-log-level <0|1|2> (phase 14 item 4). Called right
 * after open, like the shot sidecar above. */
void n265_encoder_set_csv(n265_encoder *e, const char *path, int level);

/* --qpmin / --qpmax, then the spec's own clip. The user's bounds are
 * APPLIED only where one of them narrowed QpY's range, so at the default
 * this is n265_qp_clip and the compiler sees that. Every place that
 * finishes a frame's QP goes through here. */
static inline int n265_qp_bound(const struct n265_encoder *e, int q)
{
    if (e->qp_bounds) {
        if (q < e->qpmin)
            q = e->qpmin;
        if (q > e->qpmax)
            q = e->qpmax;
    }
    return n265_qp_clip(q);
}
/* access_unit_delimiter_rbsp (7.3.2.5) and the user data unregistered SEI
 * that identifies the encoder (D.2.6). headers.c. */
void n265_write_aud(bitstream_t *bs, int pic_type);
/* recovery_point (D.2.8), the message a random-access player looks for. */
void n265_write_recovery_sei(bitstream_t *bs);
void n265_write_info_sei(bitstream_t *bs, const char *ver);
/* The pooled metrics, for the caller's end-of-run line: psnr Y/U/V, the
 * pixel-weighted psnr average, SSIM-Y and SSIM in dB. Returns the number
 * of frames measured, 0 when neither metric was asked for. */
long n265_encoder_metrics(n265_encoder *e, double out[6]);
/* round 4: resolve the 2-D phase-plane knobs into the encoder at open */
void n265_hf2_knobs(n265_encoder *e);

/* The explicit weighting that applies to this (list, reference,
 * component) - or NULL, which every call takes on a frame the estimator
 * declined and on every frame at all with --weightp off. `wp_any` is the
 * one predicate in front of the array read, so a clip with no fade pays
 * a load and a branch per prediction and nothing else.
 *
 * NULL means "the identity", not "no weighting is signalled": under
 * weighted_pred_flag the decoder runs 8.5.3.3.4.3 for this block either
 * way, and at the identity that clause IS the default prediction (mc.h),
 * so taking the plain filter here is bit-exact and not an approximation. */
static inline const n265_wp *n265_wp_of(const struct n265_frame_work *fw,
                                        int list, int ref, int c_idx)
{
    if (!fw->wp_any)
        return NULL;
    const n265_wp *w = &fw->wp[list][ref];
    return w->on[c_idx] ? w : NULL;
}

/* 7.3.6.3's writer, in encoder.c next to the slice header it belongs to.
 * Exposed so tests/unit/wp_test.c can read one back field by field. */
void n265_write_pred_weight_table(bitstream_t *bs,
                                  const struct n265_frame_work *fw);

/* the denominator the table carries for this component */
static inline int n265_wp_denom(const struct n265_frame_work *fw, int c_idx)
{
    return c_idx ? fw->wp_cdenom : fw->wp_ldenom;
}

#endif
