/*
 * Coding-tree analysis: decisions, reconstruction, and record commit.
 * No entropy coding happens here; the trailing pass in entropy.c replays
 * the record stream (docs/threading-design.md).
 * Copyright (c) 2026, the yah265 authors. GPL-2.0-or-later; see LICENSE.
 */

#include <assert.h>
#include <math.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "encoder/encoder.h"
#include "encoder/est.h"
#include "encoder/intra.h"
#include "encoder/la.h"
#include "encoder/mc.h"
#include "encoder/mv.h"
#include "encoder/records.h"
#include "encoder/residual.h"
#include "encoder/rdoq.h"
#include "encoder/psy.h"
#include "encoder/satd.h"
#include "encoder/transform.h"
#include "encoder/preset.h"

static int *g_hist;
static int g_nxn, g_2nx2n_min;

/* Cached env knobs live at file scope so n265_ctu_warm() can initialize
 * every one of them on the driver before any worker exists
 * (docs/threading-design.md, T0 audit item 2). */
static int mode_hist_on(void)
{
    static int v = -1;
    if (v < 0) v = n265_env("N265_MODE_HIST") != NULL;
    return v;
}

/* N265_CHROMA_W: the chroma distortion weight in the RD sums.
 *
 * Chroma is quantised at n265_chroma_qp(qp_y), which above luma QP 29 is
 * a FINER quantiser than luma's (Table 8-10, no offsets). A cost that
 * adds chroma SSD to luma SSD beside one luma lambda therefore
 * under-prices chroma: a unit of chroma error costs the same J as a unit
 * of luma error while the bits that would remove it are cheaper. The
 * factor that makes the two quantisers comparable is the ratio of their
 * squared step sizes, 2^((qp_y - qp_c) / 3) - and RDOQ already prices a
 * chroma TB's coefficients against lambda(qp_c), which is
 * lambda(qp_y) divided by exactly that ratio, so the mode decision is
 * the one place in this encoder where the two quantisers are not
 * reconciled with each other. Derived here from the two quantisers.
 *
 * Q8 and exact: qp_y - qp_c is 0..6 by construction, and the weight is
 * inert (256) wherever the two QPs agree, which is every luma QP below
 * 30 and so every high-rate point on the ladder. N265_CHROMA_W_PCT
 * scales the weight's EXCESS over 1 (100 = the full derived weight), so
 * a partial arm stays a single rounding of the same integer table.
 * Unset, or 0, is main byte for byte. */
static int chroma_w_on(void)
{
    static int v = -1;
    if (v < 0) { const char *s = n265_env("N265_CHROMA_W"); v = s ? atoi(s) : 0; }
    return v;
}
static int chroma_w_pct(void)
{
    static int v = -1;
    if (v < 0) { const char *s = n265_env("N265_CHROMA_W_PCT"); v = s ? atoi(s) : 100; }
    return v;
}
/* 256 * 2^(d / 3), d = qp_y - qp_c */
static const uint16_t chroma_w_tab[7] = { 256, 323, 406, 512, 645, 813, 1024 };

/* the CTU's chroma weight, Q8. One integer per CTU, read where the sums
 * are built rather than per call. */
static inline uint32_t chroma_w_q8(const struct n265_frame_work *e)
{
    return e->cur_chroma_w_q8;
}
static uint32_t chroma_w_for_qp(int qp_y, int chroma_idc)
{
    if (!chroma_w_on())
        return 256;
    int d = qp_y - n265_chroma_qp(qp_y, chroma_idc);
    if (d < 0) d = 0;
    if (d > 6) d = 6;
    uint32_t w = chroma_w_tab[d];
    int pct = chroma_w_pct();
    if (pct != 100)
        w = 256 + (uint32_t)(((int64_t)((int)w - 256) * pct) / 100);
    return w;
}
/* N265_CHROMA_PRED (phase 7): how much of the candidate CHOICE carries a
 * chroma term. The decomposition in docs/phase7-psy-design.md placed two
 * thirds of bus's 0.8-0.9 dB chroma deficit in the chroma PREDICTION -
 * chroma residual is under 1% of that stream, so the reconstruction IS
 * the prediction - and every motion candidate in this encoder is chosen
 * on luma alone. This adds the 4:2:0 planes' cost to the last few
 * candidates of each arbitration, never to the search.
 *
 *   0  the pre-item encoder, byte for byte (the escape)
 *   1  the merge shortlist, and the AMVP-vs-merge comparison with it
 *   2  also a quarter-pel chroma re-rank of the search's final vector
 *   3  also the uni/bi arbitration
 *
 * **1 is the medium default since 2026-09-12**, under N265_CPRED_GATE=2
 * which is what made it affordable. The presets are pinned to 0 and stay
 * byte for byte the pre-item encoder.
 *
 * The weight is 1.0 per chroma sample, i.e. the Cb and Cr costs are
 * simply added to the luma one. N265_CHROMA_W's table is the ratio of
 * the two quantisers' SQUARED step sizes and belongs to an SSD sum; a
 * SATD or SAD is a magnitude, not a squared error, and the ratio does
 * not transfer to it. The measurement that would set a weight here is
 * already in the record: "The weight from each component's measured rate
 * slope" reads the marginal exchange rate at 0.8-1.5 on every natural
 * clip, nearest 1.0 where the gap is, and refuses a per-QP table by its
 * own spread. 1.0 is what that leaves. */
static int chroma_pred(void)
{
    static int v = -1;
    if (v < 0) { const char *s = n265_env("N265_CHROMA_PRED"); v = s ? atoi(s) : 1; }
    return v;
}

/* The component index the INTRA FILTERING and SCAN clauses read.
 * 8.4.4.2.1 invokes the reference-sample filtering when
 * `cIdx == 0 || ChromaArrayType == 3`, and 7.4.9.11 gives the 8x8 block
 * the mode-dependent scan under the same condition, so at 4:4:4 a chroma
 * block takes the luma answer to both. Every other use of c_idx - the
 * plane it addresses, the residual-coding contexts, the DC and angular
 * BOUNDARY filters (which stay `cIdx == 0` at every format) - keeps the
 * real index. At 4:2:0 this is the identity. */
static inline int c_filt(const struct n265_frame_work *e, int c_idx)
{
    return e->c444 ? 0 : c_idx;
}

/* ---- the chroma transform UNIT, and how many blocks it is ------------
 *
 * A luma transform block of size N presents a chroma block of
 * (N >> csx) x (N >> csy). At 4:2:0 and 4:4:4 that is square and is one
 * transform block. At 4:2:2 it is (N/2) x N - twice as tall as it is wide
 * - and the spec does not transform it: 7.3.8.8 codes TWO cbfs for it
 * (`cbf_cb[x0][y0]` and `cbf_cb[x0][y0 + (1 << (log2TrafoSize - 1))]`)
 * and 7.3.8.10 codes two residual blocks, each SQUARE at
 * log2TrafoSizeC = log2TrafoSize - 1 and stacked vertically.
 *
 * So `log2c = log2ts - csx` keeps meaning "the chroma block's log2 size"
 * at every format, and what 4:2:2 adds is a second block at
 * `cy0 + (1 << log2c)` carrying its own cbf, its own scan and its own
 * residual. One tb_ctx carries the pair; `nsub_c` is how many there are
 * and `tb_any` is "this unit coded anything", which is what every
 * rqt_root_cbf and every OR up the tree wants. */
static inline int nsub_c(const struct n265_frame_work *e, int c_idx)
{
    return c_idx && e->c422 ? 2 : 1;
}

static int no_gates_on(void)
{
    static int v = -1;
    if (v < 0) v = n265_env("N265_NO_GATES") != NULL;
    return v;
}
static int no_amvp_on(void)
{
    static int v = -1;
    if (v < 0) v = n265_env("N265_NO_AMVP") != NULL;
    return v;
}
static int no_split_on(void)
{
    static int v = -1;
    if (v < 0) v = n265_env("N265_NO_SPLIT") != NULL;
    return v;
}
static int no_earlyskip_on(void)
{
    static int v = -1;
    if (v < 0) v = n265_env("N265_NO_EARLYSKIP") != NULL;
    return v;
}
static int cb64_trace_on(void)
{
    static int v = -1;
    if (v < 0) v = n265_env("N265_CB64_TRACE") != NULL;
    return v;
}
static void print_hist(void)
{
    fprintf(stderr, "mode_hist:");
    for (int i = 0; i < 35; i++)
        fprintf(stderr, " %d:%d", i, g_hist[i]);
    fprintf(stderr, "\nmin_cb: nxn %d, 2nx2n %d\n", g_nxn, g_2nx2n_min);
}
static void atexit_hist(int *h)
{
    g_hist = h;
    atexit(print_hist);
}

static void set_blk(struct n265_frame_work *e, uint8_t *map, int x0, int y0,
                    int n, int v)
{
    for (int y = 0; y < n; y += 4)
        for (int x = 0; x < n; x += 4)
            map[((y0 + y) >> 2) * e->map_stride + ((x0 + x) >> 2)] =
                (uint8_t)v;
}

static void set_mode_map(struct n265_frame_work *e, int x0, int y0, int n,
                         int mode)
{
    set_blk(e, e->intra_mode, x0, y0, n, mode);
}

/* CU and TU boundaries: filtered edges that are also TRANSFORM edges,
 * so the cbf leg of the bS derivation applies to them (edge_map bits
 * 2/3, deblock.c) */
static void mark_cu_edges(struct n265_frame_work *e, int x0, int y0, int n)
{
    for (int y = 0; y < n; y += 4)
        e->edge_map[((y0 + y) >> 2) * e->map_stride + (x0 >> 2)] |= 1 | 4;
    for (int x = 0; x < n; x += 4)
        e->edge_map[(y0 >> 2) * e->map_stride + ((x0 + x) >> 2)] |= 2 | 8;
}

/* the internal PU boundary of a split CU is a prediction edge and, on
 * its own, NOT a transform edge (8.7.2.4 gates the cbf leg on the latter),
 * so it sets only the filtered-edge bit. The deblock walk visits the 8x8
 * grid, so an AMP or 8x8-CU cut at a 4-offset is marked and correctly
 * never filtered. */
static void mark_pu_edge(struct n265_frame_work *e, int part, int x0, int y0,
                         int n)
{
    if (part == PART_2Nx2N)
        return;
    int px, py, pw, ph;
    n265_pu_rect(part, 1, x0, y0, n, &px, &py, &pw, &ph);
    if (PART_IS_HORIZ(part))
        for (int x = 0; x < n; x += 4)
            e->edge_map[(py >> 2) * e->map_stride + ((x0 + x) >> 2)] |= 2;
    else
        for (int y = 0; y < n; y += 4)
            e->edge_map[((y0 + y) >> 2) * e->map_stride + (px >> 2)] |= 1;
}

/* one component TB: predict + transform + quant (+SBH). recon and record
 * are separate so the inter skip/merge decision can look at nz first. */
typedef struct tb_ctx {
    /* A chroma tb_ctx at 4:2:2 holds the transform unit's TWO stacked
     * square blocks (7.3.8.10's tIdx loop), one after the other at stride
     * `n` - which is the same thing as one n x 2n rectangle, and is why
     * every buffer here is indexed with the sub-block's offset added
     * rather than resized. A 4:2:2 chroma TB is at most 16x16, so the
     * pair is 512 samples against the 1024 a 4:4:4 32x32 block needs and
     * the bounds below are unmoved. */
    pixel pred[32 * 32];
    pixel recon[32 * 32];
    int16_t level[32 * 32];
    int nz;
    /* the second stacked block's cbf, 0 at every format but 4:2:2 */
    int nz2;
    /* --lossless: this block took 8.6.2's transquant-bypass branch, so
     * `level` holds the residual SAMPLES and not coefficients. Carried on
     * the block rather than re-read from the encoder because
     * tb_make_recon is handed a tb_ctx and nothing else - and a block
     * that says what it is cannot be reconstructed the wrong way. */
    int bypass;
} tb_ctx;

/* "this transform unit coded anything", which is what every
 * rqt_root_cbf and every OR up the transform tree wants. At 4:2:2 it is
 * the pair; everywhere else it is the one cbf it always was. */
static inline int tb_any(const tb_ctx *tb)
{
    return tb->nz || tb->nz2;
}

/* one PU's motion decision */
typedef struct pu_dec {
    mot_t mot;
    int merge;
    int midx;
    int mvp_idx[2];
    mv_t mvp[2][2];     /* the AMVP predictor pair, per list */
} pu_dec;

/* a partition candidate carried from its RD trial to its commit */
typedef struct part_cand {
    int part;
    int any;                    /* rqt_root_cbf */
    pu_dec d[2];
    tb_ctx tl, tcb, tcr;
    pixel py[32 * 32];          /* the PU-by-PU prediction of the CU */
    /* the chroma halves of it. 16x16 covers a 32x32 rectangular CU at
     * 4:2:0; at 4:4:4 the chroma block IS the luma block, so 32x32. */
    pixel pc[2][32 * 32];
} part_cand;

/* full encoder-state snapshot of a CU area, for split-vs-nosplit trials */
typedef struct qt_snap {
    /* one CTB of all three planes: 3/2 the luma area at 4:2:0 and 3 times
     * it at 4:4:4, so the worst case is sized here and the 4:2:0 encoder
     * simply uses half of it */
    pixel recon[64 * 64 * 3];
    uint8_t maps[8][16 * 16];
    int16_t mv[16 * 16 * 4];
    int8_t ref[16 * 16 * 2];    /* the reference-index map (multi-ref, 2026-09-06) */
    cabac_ctx_t est[CTX_COUNT];
    int ent_qp_coded;
    size_t rec_len;
    /* the records this arm appended beyond rec_base (the other arm
     * overwrites the shared tail, so content must be kept too) */
    size_t rec_base;
    size_t rec_copy_len;
    /* An intra CB64's full record: the levels are int16 per coefficient,
     * so one CTU is 8 KB of luma plus the chroma - 4 KB at 4:2:0 (two
     * 32x32 blocks) and 16 KB at 4:4:4 (two 64x64 worth), plus the flag
     * bytes. 40 KB covers the worst format with room; the assert below is
     * what says so. */
    uint8_t rec_copy[40960];
} qt_snap;

/* Per-row-lane trial scratch (docs/threading-design.md sec 4). These
 * were ~180 KB of function-local `static` buffers, which is exactly what
 * a row wavefront cannot have. The layout keeps each old static as its
 * own field so the SHARING is unchanged: analyze_cu is re-entered
 * recursively from quadtree and its candidate TBs were already shared
 * across depths, and one instance per lane preserves that. */
struct n265_scratch {
    tb_ctx itt_l[4], itt_c[4][2];   /* commit_intra_tt's split arrangement */
    /* tt_analyze's chroma save/split recon. They live here rather than on
     * that function's stack because they have to hold 4:4:4's 32x32 chroma
     * block and 4:2:0 only ever writes 16x16 of each: as locals they were
     * 4 KB of frame that the 4:2:0 encoder reserves on every call and
     * three quarters never touches, and that alone was 0.5% of
     * single-thread wall (docs/phase8-plan.md, "Phase 8 speed"). Sharing
     * one instance per lane is safe on the same argument itt_l already
     * makes: analyze_cu calls tt_analyze and returns through it, so a
     * second invocation is never live while the first one is. */
    pixel tt_save_c[2 * 32 * 32], tt_split_c[2 * 32 * 32];
    tb_ctx ctt_l[4], ctt_c[4][2];   /* commit_inter_tt's split arrangement */
    tb_ctx ird_l, ird_cb, ird_cr;   /* intra_rd_pick's candidate TBs */
    tb_ctx ird_bl, ird_bcb, ird_bcr;   /* ...and the winner it keeps */
    tb_ctx m_tl, m_tcb, m_tcr;      /* analyze_cu: merge candidate TBs */
    tb_ctx a_tl, a_tcb, a_tcr;      /* ...amvp candidate TBs */
    tb_ctx i_tl, i_tcb, i_tcr;      /* ...intra candidate TBs */
    part_cand pca, pcb;             /* ...the two partition candidates */
    /* the quadtree's split-vs-nosplit snapshots, one set per recursion
     * depth. These were `static` too, and they are the ones the T0 audit
     * missed: with them shared, two encode rows in flight corrupt each
     * other's record stream no matter how large the CTB lag is. */
    qt_snap qt_pre[4], qt_won[4];
    qt_snap qt_fpre, qt_fwon;   /* the depth-bound arm, depth 0 only */
};

struct n265_scratch *n265_scratch_new(void)
{
    return calloc(1, sizeof(struct n265_scratch));
}

void n265_scratch_free(struct n265_scratch *sc)
{
    free(sc);
}

/* uni MC from one list at any quarter-pel MV into dst (w x h at ds) */
static void mc_pred_uni(struct n265_frame_work *e, int list, int ref, int c_idx, int x0,
                        int y0, int w, int h, mv_t mv, pixel *dst, int ds)
{
    struct n265_refpic *r = e->refl[list][ref];
    int rs = r->stride[c_idx];
    pixel win[N265_REF_WIN_SAMPLES];
    if (c_idx) {
        /* 8.5.3.2.9: mvCLX = mvLX * 2 / SubWidthC, so the chroma vector is
         * the luma one at 4:2:0 (both in eighths of a chroma sample) and
         * TWICE it at 4:4:4 - where a chroma sample is a luma sample, so
         * the reachable fractional positions are the EVEN entries of
         * Table 8-12 and the filter runs at quarter-pel on a full-size
         * plane. *2 rather than << 1 because the vector goes negative and
         * shifting a negative is undefined. */
        int mvcx = e->csx ? mv.x : mv.x * 2;
        int mvcy = e->csy ? mv.y : mv.y * 2;
        int dx = mvcx >> 3, dy = mvcy >> 3;
        /* G2 entry point. The chroma 4-tap reaches two chroma lines
         * below the block; the bound is asked in LUMA lines because that
         * is what the watermark counts, so a chroma line becomes
         * (1 << csy) luma lines and the last one of them is the bound.
         * Multiplication, not a shift: an unclamped chroma MV can put
         * this line above the picture and shifting a negative is
         * undefined (UBSan catches it on park_joy at t12).
         * n265_ref_need clamps it. */
        int cl = 1 << e->csy;
        int need = n265_ref_wait(e, r, (y0 + dy + h + 2) * cl + cl - 1);
        N265_REF_GUARD(e, r, need, c_idx, NULL);
        int ws, wclip;
        const pixel *rp = n265_ref_win(r->plane[c_idx], rs,
                                         e->pad_w >> e->csx,
                                         e->pad_h >> e->csy,
                                         n265_ref_pad_x(c_idx, e->csx),
                                         n265_ref_pad_y(c_idx, e->csy),
                                         x0 + dx, y0 + dy, w, h, 1, 2,
                                         win, &ws, &wclip);
        n265_mc_chroma_wh(dst, ds, w, h, rp, ws, mvcx & 7, mvcy & 7);
        N265_REF_UNGUARD();
    } else {
        int ix = x0 + (mv.x >> 2), iy = y0 + (mv.y >> 2);
        int need = n265_ref_wait(e, r, iy + h + 4);     /* G2 entry point */
        const int16_t *hf = n265_ref_hf(r, ix, iy, w, h, mv.x & 3);
        const pixel *p2 = n265_ref_2d(r, ix, iy, w, h, mv.x & 3, mv.y & 3);
        n265_hf2_tally(e, r, ix, iy, w, h, mv.x & 3, mv.y & 3, 0);
        N265_REF_GUARD2(e, r, need, 0, n265_ref_hf_base(r, mv.x & 3),
                        n265_ref_2d_base(r, mv.x & 3, mv.y & 3));
        if (p2) {
            n265_mc_luma_2d(dst, ds, w, h, p2, r->hf2_stride);
        } else {
            int ws, wclip;
            const pixel *rp = n265_ref_win(r->plane[0], rs, e->pad_w,
                                             e->pad_h, N265_REF_PAD,
                                             N265_REF_PAD, ix, iy,
                                             w, h, 3, 4, win, &ws, &wclip);
            n265_mc_luma_hf(dst, ds, w, h, rp, ws, wclip ? NULL : hf,
                            r->hf_stride, mv.x & 3, mv.y & 3);
        }
        N265_REF_UNGUARD();
    }
}

static void mc_pred14(struct n265_frame_work *e, int list, int ref, int c_idx, int x0,
                      int y0, int w, int h, mv_t mv, int16_t *dst)
{
    struct n265_refpic *r = e->refl[list][ref];
    int rs = r->stride[c_idx];
    pixel win[N265_REF_WIN_SAMPLES];
    if (c_idx) {
        /* the chroma vector of 8.5.3.2.9; see mc_pred_uni */
        int mvcx = e->csx ? mv.x : mv.x * 2;
        int mvcy = e->csy ? mv.y : mv.y * 2;
        int dx = mvcx >> 3, dy = mvcy >> 3;
        int cl = 1 << e->csy;
        int need = n265_ref_wait(e, r, (y0 + dy + h + 2) * cl + cl - 1);
        N265_REF_GUARD(e, r, need, c_idx, NULL);
        int ws, wclip;
        const pixel *rp = n265_ref_win(r->plane[c_idx], rs,
                                         e->pad_w >> e->csx,
                                         e->pad_h >> e->csy,
                                         n265_ref_pad_x(c_idx, e->csx),
                                         n265_ref_pad_y(c_idx, e->csy),
                                         x0 + dx, y0 + dy, w, h, 1, 2,
                                         win, &ws, &wclip);
        n265_mc_chroma_14_wh(dst, w, w, h, rp, ws, mvcx & 7, mvcy & 7);
        N265_REF_UNGUARD();
    } else {
        int ix = x0 + (mv.x >> 2), iy = y0 + (mv.y >> 2);
        int need = n265_ref_wait(e, r, iy + h + 4);     /* G2 entry point */
        const int16_t *hf = n265_ref_hf(r, ix, iy, w, h, mv.x & 3);
        const int16_t *p2 = n265_ref_2d14(r, ix, iy, w, h, mv.x & 3,
                                          mv.y & 3);
        n265_hf2_tally(e, r, ix, iy, w, h, mv.x & 3, mv.y & 3, 1);
        N265_REF_GUARD3(e, r, need, 0, n265_ref_hf_base(r, mv.x & 3),
                        n265_ref_2d14_base(r, mv.x & 3, mv.y & 3));
        if (p2) {
            n265_mc_luma_2d_14(dst, w, w, h, p2, r->hf2_stride);
        } else {
            int ws, wclip;
            const pixel *rp = n265_ref_win(r->plane[0], rs, e->pad_w,
                                             e->pad_h, N265_REF_PAD,
                                             N265_REF_PAD, ix, iy,
                                             w, h, 3, 4, win, &ws, &wclip);
            n265_mc_luma_14_hf(dst, w, w, h, rp, ws, wclip ? NULL : hf,
                               r->hf_stride, mv.x & 3, mv.y & 3);
        }
        N265_REF_UNGUARD();
    }
}

/* full motion (uni either list, or bi with the default average) */
static void mc_pred_wh(struct n265_frame_work *e, int c_idx, int x0, int y0,
                       int w, int h, mot_t mot, pixel *dst, int ds)
{
    if (mot.pred == 3) {
        int16_t a[64 * 64], b[64 * 64];
        mc_pred14(e, 0, mot.ref[0], c_idx, x0, y0, w, h, mot.mv[0], a);
        mc_pred14(e, 1, mot.ref[1], c_idx, x0, y0, w, h, mot.mv[1], b);
        n265_bi_avg_wh(dst, ds, a, b, w, h);
    } else {
        int list = mot.pred == 2;
        mc_pred_uni(e, list, mot.ref[list], c_idx, x0, y0, w, h,
                    mot.mv[list], dst, ds);
    }
}

static void mc_pred(struct n265_frame_work *e, int c_idx, int x0, int y0,
                    int n, mot_t mot, pixel *dst)
{
    mc_pred_wh(e, c_idx, x0, y0, n, n, mot, dst, n);
}

/* The CTU's lambda: the QP table scaled by the cu-tree lambda multiplier
 * (N265_CT_LAMBDA), so mode decision, the partition search and RDOQ all
 * price against one number. 256 is the default and returns the table
 * value untouched. */
static inline uint32_t ctu_lam(const struct n265_frame_work *e, int qp)
{
    uint32_t l = n265_lambda_mode_q8(qp);
    if (e->cur_ctu_lam_q8 == 256)
        return l;
    return (uint32_t)(((uint64_t)l * e->cur_ctu_lam_q8) >> 8);
}

/* N265_CU_TRACE (diagnostic, output-neutral): the census phase 9 step 6a
 * is decided on - where the search spends its time on a clip that is
 * mostly still, and whether what it spends it on is separable from what
 * it gets back.
 *
 * Every column is a number the encoder had already computed (the merge
 * candidate's SAD, the static-skip bound, the forced skip's distortion,
 * the clean-skip bound, the lookahead's costs and cu-tree propagate) or
 * a cycle count taken AROUND code that runs either way. Nothing here is
 * read by a predicate and no branch depends on it, so the bitstream is
 * the same with the variable set and without it.
 *
 * The clock is `cntvct_el0` on aarch64, whose `cntfrq_el0` on this box
 * is 1 GHz - a tick is a nanosecond, and one 8x8 node is hundreds of
 * them - and the read is cheap enough that a traced encode and a plain
 * one are the same wall to the resolution of the harness
 * (docs/phase9-goals-plan.md sec 11.1). PMCCNTR_EL0 is not readable from
 * EL0 here, which is why this is the counter and not cycles; where the
 * timebase is coarser the SHARES still hold, because the difference of
 * two truncated reads is unbiased over the millions the census sums. */
static inline uint64_t cu_tick(void)
{
#if defined(__aarch64__)
    uint64_t v;
    __asm__ volatile("mrs %0, cntvct_el0" : "=r"(v));
    return v;
#elif defined(__x86_64__) || defined(__i386__)
    return (uint64_t)__builtin_ia32_rdtsc();
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
#endif
}

/* which arm of analyze_cu the TB analyses arriving now belong to. The
 * COMMIT's transform tree is charged to the arm that won it, which is
 * what makes the per-class counts read as "TB analyses by mode class". */
enum { CU_ARM_MERGE = 0, CU_ARM_AMVP, CU_ARM_INTRA, CU_ARM_PART };

#define CU_CUR(e) \
    ((e)->cu_rec && (e)->cu_sp > 0 ? (e)->cu_stk[(e)->cu_sp - 1] : NULL)

/* open this node's record and push it; NULL whenever the trace is off,
 * which is every site's test */
static struct n265_cu_rec *cu_open(struct n265_frame_work *e, int x0, int y0,
                                   int log2n, int depth)
{
    if (!e->cu_rec || e->cu_sp >= (int)(sizeof(e->cu_stk) /
                                        sizeof(e->cu_stk[0])))
        return NULL;
    int k = e->cu_ctu;
    unsigned i = e->cu_rec_n[k];
    if (i >= N265_CU_REC_MAX)
        return NULL;
    e->cu_rec_n[k] = (uint16_t)(i + 1);
    struct n265_cu_rec *r = &e->cu_rec[(size_t)k * N265_CU_REC_MAX + i];
    memset(r, 0, sizeof(*r));
    r->x = (uint16_t)x0;
    r->y = (uint16_t)y0;
    r->log2n = (uint8_t)log2n;
    r->depth = (uint8_t)depth;
    r->kind = N265_CUT_OUTSIDE;
    r->qp = (uint8_t)e->cur_ctu_qp;
    r->slice_type = (uint8_t)e->slice_type;
    if (e->flat)
        r->flags |= N265_CUF_FLAT;
    /* the lookahead's own view of the node: the 16x16 blocks it covers,
     * or the one block it sits inside */
    if (e->la_icost && e->la_bw) {
        int n = 1 << log2n, nb = n >> 4;
        if (nb < 1)
            nb = 1;
        uint64_t ic = 0, pc = 0;
        double pr = 0;
        int cnt = 0;
        for (int by = y0 >> 4; by < (y0 >> 4) + nb && by < e->la_bh; by++)
            for (int bx = x0 >> 4; bx < (x0 >> 4) + nb && bx < e->la_bw;
                 bx++) {
                int b = by * e->la_bw + bx;
                ic += e->la_icost[b];
                pc += e->la_pcost[b];
                if (e->la_prop)
                    pr += e->la_prop[b];
                cnt++;
            }
        r->icost = (uint32_t)ic;
        r->pcost = (uint32_t)pc;
        r->prop = cnt ? (float)(pr / cnt) : 0.0f;
    }
    e->cu_stk[e->cu_sp++] = r;
    return r;
}

static void cu_close(struct n265_frame_work *e, struct n265_cu_rec *r,
                     uint64_t t0)
{
    if (!r)
        return;
    r->cyc_tot = (uint32_t)(cu_tick() - t0);
    e->cu_sp--;
    if (e->cu_sp > 0)
        e->cu_stk[e->cu_sp - 1]->cyc_kids += r->cyc_tot;
}

/* N265_RDOQ_TRACE (diagnostic, output-neutral): the census phase 9 step 2
 * is decided on, before any commit-only lattice exists.
 *
 * The lattice is the largest block of the single-thread wall and it runs
 * on every TB of every mode-decision TRIAL. The commit-only form
 * (x265's `--rdoq-level 1`) divides that cost by the number of trials per
 * TB that is actually committed, and pays for it with whatever the plain
 * quantiser's levels get wrong in the trials. Both halves are counted
 * here on the SAME coefficients: the lattice's levels are the ones that
 * ship, so nothing below moves a bit.
 *
 * Per TB trial it prices the arm the trials would run under
 * `N265_RDOQ=2` - `n265_quant` plus `n265_sbh_adjust`, exactly the
 * `e->rdoq == 0` path - against the lattice's own answer, in the units
 * the CU decision compares: real pixel SSD against the source through
 * dequant/IDCT, plus lambda times the estimator's bits for the cbf and
 * residual_coding of those levels. What the census then reports is
 *
 *   (a) how often the two disagree at all, how often they disagree about
 *       the cbf (zero vs nonzero, the one difference a CU ranking cannot
 *       absorb), and how far apart their J is, bucketed - a trial whose
 *       J moves by less than a tenth of a percent cannot reorder a CU
 *       decision that is made on the sum of four to six of them;
 *   (b) trials per COMMITTED TB, which is the factor the commit-only form
 *       divides the lattice's cost by. Under 2 and the item has no
 *       ceiling worth the risk;
 *   (c) the share of lattice calls that end all-zero, which is the
 *       cheap pre-test the trial side could keep.
 *
 * The counters are touched only when the variable is set; `perf` on the
 * default path sees none of this. */
static atomic_ullong rt_trial[2][4], rt_commit[2][4];
/* the same disagreement columns per transform size: which sizes a
 * partial arm (N265_RDOQ_LAT_SZ) should keep the lattice in during
 * the trials is a question about where the two quantisers actually
 * part company, not about where the calls are */
static atomic_ullong rt_diff_sz[2][4], rt_dj_sz[2][4];
static atomic_ullong rt_n[2][7];
/* mean |dJ| / J is carried as a sum of parts per million so the report
 * needs no floating-point accumulator shared between workers */
static atomic_ullong rt_dj_ppm[2];
enum { RT_LATZ, RT_PLNZ, RT_NZFLIP, RT_DIFF, RT_LATBET, RT_D1K, RT_D100 };

static void rt_report(void)
{
    static const char *nm[7] = { "lat-zero", "plain-zero", "nz-flip",
                                 "levels-differ", "lattice-better",
                                 "dJ>0.1%", "dJ>1%" };
    for (int p = 0; p < 2; p++) {
        unsigned long long tr = 0, cm = 0;
        for (int k = 0; k < 4; k++) {
            tr += atomic_load(&rt_trial[p][k]);
            cm += atomic_load(&rt_commit[p][k]);
        }
        fprintf(stderr, "rdoqtrace %s: trials=%llu commits=%llu per=%.3f",
                p ? "chroma" : "luma", tr, cm,
                cm ? (double)tr / (double)cm : 0.0);
        for (int k = 0; k < 7; k++)
            fprintf(stderr, " %s=%llu", nm[k], atomic_load(&rt_n[p][k]));
        fprintf(stderr, " dJmean_ppm=%llu\n",
                tr ? atomic_load(&rt_dj_ppm[p]) / tr : 0ull);
        for (int k = 0; k < 4; k++) {
            unsigned long long t = atomic_load(&rt_trial[p][k]);
            unsigned long long c = atomic_load(&rt_commit[p][k]);
            fprintf(stderr, "rdoqtrace %s %2dx%-2d: trials=%llu commits=%llu"
                    " per=%.3f differ=%llu dJmean_ppm=%llu\n",
                    p ? "chroma" : "luma", 4 << k, 4 << k, t, c,
                    c ? (double)t / (double)c : 0.0,
                    atomic_load(&rt_diff_sz[p][k]),
                    t ? atomic_load(&rt_dj_sz[p][k]) / t : 0ull);
        }
    }
}

static int rdoq_trace(void)
{
    static int v = -1;
    if (v < 0) {
        v = n265_env("N265_RDOQ_TRACE") != NULL;
        if (v) atexit(rt_report);
    }
    return v;
}

/* the pixel SSD one arm's levels produce against the source, built the
 * way tb_make_recon_one builds the committed recon */
static uint64_t rt_ssd(int c_idx, int log2n, int qp, int use_dst,
                       const pixel *pred, const int16_t *lv, int nz,
                       const pixel *src, int sstride)
{
    (void)c_idx;
    int n = 1 << log2n;
    pixel rec[32 * 32];
    if (nz) {
        int16_t dq[32 * 32];
        int nzw, nzh;
        n265_dequant(dq, lv, log2n, qp, &nzw, &nzh);
        n265_idct_add(rec, n, dq, log2n, pred, n, use_dst, nzw, nzh);
    } else {
        memcpy(rec, pred, (size_t)n * n * sizeof(pixel));
    }
    return n265_ssd_wh(src, sstride, rec, n, n, n);
}

/* ...and the bits, on a private copy of the block's opening contexts:
 * the cbf this arm implies plus residual_coding of its levels. The cbf
 * context is the depth-0 one for both arms, which is the same bin on
 * both sides except where the arms disagree about it - and that
 * disagreement is exactly what the nz-flip column counts. */
static uint32_t rt_bits(struct n265_frame_work *e, int c_idx, int log2n,
                        int scan, const int16_t *lv, int nz)
{
    cabac_ctx_t local[CTX_COUNT];
    est_state st;
    res_ops ops;
    memcpy(local, e->est_ctx, sizeof(local));
    st.ctx = local;
    st.bits = 0;
    n265_est_ops(&ops, &st);
    st.bits += n265_est_bin(&local[(c_idx ? CTX_CBF_CHROMA : CTX_CBF_LUMA)],
                            nz != 0);
    if (nz)
        n265_residual_coding(&ops, lv, log2n, c_idx, scan, !e->lossless);
    return st.bits;
}

static void rt_tally(struct n265_frame_work *e, int c_idx, int log2n, int qp,
                     int intra, int use_dst, int scan, const int16_t *coef,
                     const pixel *pred, const int16_t *lat, int lat_nz,
                     const pixel *src, int sstride)
{
    int p = c_idx != 0, k = log2n - 2;
    int n = 1 << log2n;
    int16_t pln[32 * 32];

    atomic_fetch_add_explicit(&rt_trial[p][k], 1, memory_order_relaxed);
    if (!lat_nz)
        atomic_fetch_add_explicit(&rt_n[p][RT_LATZ], 1, memory_order_relaxed);

    /* the trial-side arm of N265_RDOQ=2, run exactly as the e->rdoq == 0
     * path runs it */
    int pln_nz = n265_quant(pln, coef, log2n, qp, intra);
    if (pln_nz)
        n265_sbh_adjust(pln, log2n, scan);
    if (!pln_nz)
        atomic_fetch_add_explicit(&rt_n[p][RT_PLNZ], 1, memory_order_relaxed);
    if ((lat_nz != 0) != (pln_nz != 0))
        atomic_fetch_add_explicit(&rt_n[p][RT_NZFLIP], 1,
                                  memory_order_relaxed);
    if (memcmp(lat, pln, (size_t)n * n * sizeof(int16_t))) {
        atomic_fetch_add_explicit(&rt_n[p][RT_DIFF], 1, memory_order_relaxed);
        atomic_fetch_add_explicit(&rt_diff_sz[p][k], 1, memory_order_relaxed);
    }

    uint32_t lam = ctu_lam(e, qp);
    uint64_t jl = rt_ssd(c_idx, log2n, qp, use_dst, pred, lat, lat_nz, src,
                         sstride)
        + (((uint64_t)rt_bits(e, c_idx, log2n, scan, lat, lat_nz) *
            lam) >> 16);
    uint64_t jp = rt_ssd(c_idx, log2n, qp, use_dst, pred, pln, pln_nz, src,
                         sstride)
        + (((uint64_t)rt_bits(e, c_idx, log2n, scan, pln, pln_nz) *
            lam) >> 16);
    if (jl < jp)
        atomic_fetch_add_explicit(&rt_n[p][RT_LATBET], 1,
                                  memory_order_relaxed);
    uint64_t d = jl > jp ? jl - jp : jp - jl;
    uint64_t base = jp ? jp : 1;
    uint64_t ppm = d > (UINT64_MAX / 1000000u) ? 1000000u
                                               : d * 1000000u / base;
    if (ppm > 1000000u)
        ppm = 1000000u;
    atomic_fetch_add_explicit(&rt_dj_ppm[p], ppm, memory_order_relaxed);
    atomic_fetch_add_explicit(&rt_dj_sz[p][k], ppm, memory_order_relaxed);
    if (ppm > 1000)
        atomic_fetch_add_explicit(&rt_n[p][RT_D1K], 1, memory_order_relaxed);
    if (ppm > 10000)
        atomic_fetch_add_explicit(&rt_n[p][RT_D100], 1, memory_order_relaxed);
}

/* The quantiser half of a TB analysis: which of the two quantisers this
 * call is entitled to, and the sign hiding that goes with it.
 *
 * `N265_RDOQ=2` matches what x265 documents for `--rdoq-level 1`: the
 * mode-decision TRIALS run the plain quantiser and the lattice runs once
 * per TB on the COMMIT path, so the encoder pays for the largest block of
 * its wall once per committed block instead of once per trial (the
 * census in docs/phase9-lattice.md: 2.26-2.50 luma trials per committed
 * TB at the pin). `e->rdoq_commit` is which of the two a call is; the
 * commit path sets it around its own analyses and re-quantises the
 * winners it was handed (tb_relattice below). At `N265_RDOQ` 0 and 1 the
 * flag is not read and the quantiser is what it always was, which is what
 * makes the default byte-identical.
 *
 * `N265_RDOQ=3` would also run the lattice in the final RD stage of the
 * shortlist winner; the census says 2's trial-side arm already agrees
 * with the lattice outright on 52-89% of luma trials and disagrees about
 * the cbf on under 2%, so 3 is not built until the band says 2 gives back
 * too much. */
/* N265_RDOQ_LAT_SZ: under N265_RDOQ=2, the transform sizes that KEEP the
 * lattice in the TRIALS as well - a bitmask over log2n - 2, so bit 0 is
 * 4x4, bit 1 8x8, bit 2 16x16 and bit 3 32x32. 0 is the whole commit-only
 * mode and is what N265_RDOQ=2 means on its own. It exists because the
 * two halves of the mode are not one trade: the per-size census says
 * where the calls are, where the time is (a 32x32 call is 64 times a 4x4
 * one) and where the two quantisers actually disagree, and those are
 * three different places. Inert unless N265_RDOQ=2. */
static int rdoq_lat_sz(void)
{
    static int v = -1;
    if (v < 0) { const char *s = n265_env("N265_RDOQ_LAT_SZ"); v = s ? atoi(s) : 0; }
    return v;
}

static void tb_quant_one(struct n265_frame_work *e, int c_idx, int log2n,
                         int qp, int intra, int scan, const int16_t *coef,
                         int16_t *level, int *out_nz)
{
    int lat = e->rdoq && (e->rdoq < 2 || e->rdoq_commit ||
                          ((rdoq_lat_sz() >> (log2n - 2)) & 1));
    int sbh_done = 0;
    if (lat) {
        *out_nz = n265_rdoq(level, coef, log2n, qp, intra, c_idx, scan,
                            e->est_ctx, ctu_lam(e, qp));
        sbh_done = n265_rdoq_sbh_on();      /* RDOQ hid the signs itself */
    } else {
        *out_nz = n265_quant(level, coef, log2n, qp, intra);
    }
    if (*out_nz && !sbh_done)
        n265_sbh_adjust(level, log2n, scan);
}

/* one SQUARE block of a transform unit: the whole of it at 4:2:0 and
 * 4:4:4, and one of the stacked pair at 4:2:2. `off` is where in the
 * tb_ctx's buffers this block sits (0 or n*n) and `out_nz` which cbf it
 * sets. */
static void tb_analyze_one(struct n265_frame_work *e, int c_idx, int x0,
                           int y0, int log2n, int qp, int mode, mot_t mot,
                           tb_ctx *tb, size_t off, int *out_nz)
{
    int inter = mode == MODE_INTER;
    /* N265_CU_TRACE: one TB analysis, charged to the arm that asked for
     * it. Luma and chroma both, which is what the CU's cost is made of. */
    {
        struct n265_cu_rec *cr = CU_CUR(e);
        if (cr) {
            uint16_t *p = e->cu_arm == CU_ARM_AMVP  ? &cr->nt_amvp
                        : e->cu_arm == CU_ARM_INTRA ? &cr->nt_intra
                        : e->cu_arm == CU_ARM_PART  ? &cr->nt_part
                                                    : &cr->nt_merge;
            if (*p < 65535)
                (*p)++;
        }
    }
    /* 8.6.4.2: the 4x4 DST-VII is LUMA-only at every chroma format, so a
     * 4x4 chroma block at 4:4:4 or 4:2:2 takes the DCT like every other. */
    int use_dst = !inter && c_idx == 0 && log2n == 2;
    int n = 1 << log2n;
    int cf = c_filt(e, c_idx);
    pixel *pred = tb->pred + off;
    int16_t *level = tb->level + off;
    pixel *recon = e->recon[c_idx];
    int stride = e->recon_stride[c_idx];
    const pixel *src = e->pad_plane[c_idx] + y0 * e->pad_stride[c_idx] + x0;
    int pic_w = c_idx ? e->pad_w >> e->csx : e->pad_w;
    int pic_h = c_idx ? e->pad_h >> e->csy : e->pad_h;
    int ctb_log2 = c_idx ? e->ctb_log2 - e->csx : e->ctb_log2;

    if (inter) {
        if (e->cu_pred[c_idx]) {
            /* a partitioned CU predicts PU by PU once; every TB inside it
             * crops that instead of running whole-TB MC on one motion */
            int shx = c_idx ? e->csx : 0, shy = c_idx ? e->csy : 0;
            int cs = e->cu_pred_stride[c_idx];
            const pixel *p = e->cu_pred[c_idx] +
                (y0 - (e->cu_pred_y0 >> shy)) * cs +
                (x0 - (e->cu_pred_x0 >> shx));
            for (int y = 0; y < n; y++)
                memcpy(pred + y * n, p + y * cs,
                       (size_t)n * sizeof(pixel));
        } else {
            mc_pred(e, c_idx, x0, y0, n, mot, pred);
        }
    } else {
        pixel refs[4 * 32 + 1], refs_f[4 * 32 + 1];
        if (c_idx && e->c422) {
            /* 8.4.4.2.2 asks 6.4.1 about the LUMA images of the chroma
             * candidates, and at 4:2:2 no chroma-space z-scan agrees with
             * that: the chroma CTB is half as wide as it is tall. This is
             * also what makes the lower of a unit's two stacked blocks
             * see the upper one - its luma image is inside the same luma
             * TB and earlier in the scan. */
            n265_zgeo z = { e->csx, e->csy, e->pad_w, e->ctb_log2 };
            n265_intra_refs_g(refs, recon, stride, x0, y0, log2n,
                              pic_w, pic_h, &z);
        } else {
            n265_intra_refs(refs, recon, stride, x0, y0, log2n, pic_w,
                            pic_h, ctb_log2);
        }
        if (n265_use_filtered_refs(mode, log2n, cf)) {
            n265_filter_refs(refs_f, refs, log2n, c_idx == 0);
            n265_intra_pred(pred, n, refs_f, log2n, mode, c_idx);
        } else {
            n265_intra_pred(pred, n, refs, log2n, mode, c_idx);
        }
    }

    int16_t resid[32 * 32], coef[32 * 32];
    for (int y = 0; y < n; y++)
        for (int x = 0; x < n; x++)
            resid[y * n + x] = (int16_t)(src[y * e->pad_stride[c_idx] + x] -
                                         pred[y * n + x]);
    /* transquant bypass (8.6.2): no transform, no quantiser, and no
     * sign-bit hiding (7.4.9.11 conditions it on the flag). The coded
     * levels ARE the residual samples in raster position, which is what
     * the decoder's `if (cu_transquant_bypass_flag) resSamples = r`
     * branch reads them back as. The magnitudes stay small - -255..255 at
     * 8 bits, -1023..1023 at Main10 - so nothing here comes near the
     * 16-bit coefficient range residual_coding is written for, and
     * coeff_abs_level_remaining's Golomb-Rice escape covers them.
     * scan_idx is unchanged: 7.3.8.11 derives it from the prediction
     * mode and the block size, bypass or not. */
    tb->bypass = e->lossless;
    if (e->lossless) {
        int nz = 0;
        for (int i = 0; i < n * n; i++) {
            level[i] = resid[i];
            nz += resid[i] != 0;
        }
        *out_nz = nz;
        return;
    }
    n265_fdct(coef, resid, log2n, use_dst);
    int scan = inter ? 0 : n265_scan_idx(mode, log2n, cf);
    tb_quant_one(e, c_idx, log2n, qp, !inter, scan, coef, level, out_nz);
    if (e->rdoq && rdoq_trace())
        rt_tally(e, c_idx, log2n, qp, !inter, use_dst, scan, coef, pred,
                 level, *out_nz, src, e->pad_stride[c_idx]);
}

/* reconstruct ONE block of the unit into the TB's scratch buffer */
static void tb_make_recon_one(int c_idx, int log2n, int qp, int mode,
                              tb_ctx *tb, size_t off, int nz)
{
    int inter = mode == MODE_INTER;
    int use_dst = !inter && c_idx == 0 && log2n == 2;
    int n = 1 << log2n;
    pixel *pred = tb->pred + off, *rec = tb->recon + off;
    int16_t *level = tb->level + off;
    if (tb->bypass) {
        /* 8.6.5 with resSamples straight from the levels: pred + residual
         * is the source sample, in range by construction (the residual is
         * src - pred and src is a legal sample), so the clip is a no-op
         * and is left out rather than written as a lie. A zero-cbf bypass
         * block is one whose residual was identically zero, so the copy
         * below is exact too - which is why `nz == 0` is safe to read as
         * "the prediction IS the source" everywhere else in this file. */
        for (int i = 0; i < n * n; i++)
            rec[i] = (pixel)(pred[i] + level[i]);
        return;
    }
    if (nz) {
        int16_t dq[32 * 32];
        int nzw, nzh;
        n265_dequant(dq, level, log2n, qp, &nzw, &nzh);
        n265_idct_add(rec, n, dq, log2n, pred, n, use_dst, nzw, nzh);
    } else {
        memcpy(rec, pred, (size_t)n * n * sizeof(pixel));
    }
}

/* the transform unit, whole: one block, or 4:2:2's stacked pair. */
static void tb_analyze(struct n265_frame_work *e, int c_idx, int x0, int y0,
                       int log2n, int qp, int mode, mot_t mot, tb_ctx *tb)
{
    int n = 1 << log2n;
    /* Table 8-3 (8.4.3): at ChromaArrayType 2 the mode derived for chroma
     * is REMAPPED, and IntraPredModeC is the mapped value - which is what
     * 8.4.4.2 predicts with and what 7.4.9.11's scan derivation reads.
     * MODE_INTER passes through. Identity at 4:2:0 and 4:4:4. */
    if (c_idx)
        mode = n265_c422_mode(e->c422, mode);
    tb->nz2 = 0;
    tb_analyze_one(e, c_idx, x0, y0, log2n, qp, mode, mot, tb, 0, &tb->nz);
    if (nsub_c(e, c_idx) == 1)
        return;

    /* The unit's second block. 8.4.4.1 decodes the two IN ORDER, so an
     * intra lower block's top reference row is the upper block's
     * RECONSTRUCTION and not the picture's stale contents there. It is
     * made here and lent to the plane for the gather, then taken back:
     * tb_analyze leaves the picture as it found it at every other format
     * and may not stop doing so here, because half its callers are
     * candidate trials that never commit. Inter needs none of this - the
     * two blocks predict from the reference picture and are independent.
     *
     * A 4:2:2 chroma block is at most 16x16, which is what `save` is. */
    if (mode != MODE_INTER) {
        pixel save[16 * 16];
        pixel *rec = e->recon[c_idx];
        int rs = e->recon_stride[c_idx];
        tb_make_recon_one(c_idx, log2n, qp, mode, tb, 0, tb->nz);
        for (int y = 0; y < n; y++) {
            memcpy(save + y * n, rec + (size_t)(y0 + y) * rs + x0,
                   (size_t)n * sizeof(pixel));
            memcpy(rec + (size_t)(y0 + y) * rs + x0, tb->recon + y * n,
                   (size_t)n * sizeof(pixel));
        }
        tb_analyze_one(e, c_idx, x0, y0 + n, log2n, qp, mode, mot, tb,
                       (size_t)n * n, &tb->nz2);
        for (int y = 0; y < n; y++)
            memcpy(rec + (size_t)(y0 + y) * rs + x0, save + y * n,
                   (size_t)n * sizeof(pixel));
        return;
    }
    tb_analyze_one(e, c_idx, x0, y0 + n, log2n, qp, mode, mot, tb,
                   (size_t)n * n, &tb->nz2);
}

static void tb_make_recon(const struct n265_frame_work *e, int c_idx,
                          int log2n, int qp, int mode, tb_ctx *tb)
{
    int n = 1 << log2n;
    if (c_idx)
        mode = n265_c422_mode(e->c422, mode);
    tb_make_recon_one(c_idx, log2n, qp, mode, tb, 0, tb->nz);
    if (nsub_c(e, c_idx) == 2)
        tb_make_recon_one(c_idx, log2n, qp, mode, tb, (size_t)n * n,
                          tb->nz2);
}

/* N265_RDOQ=2: one block of a WINNER carried out of a trial, re-quantised
 * with the lattice and reconstructed from what the lattice chose.
 *
 * The trial built `tb->level` with the plain quantiser and `tb->recon`
 * from those levels, so both are wrong for a block that is about to be
 * committed - the decoder reconstructs from the LEVELS, and a recon built
 * from other levels is what the deblock, SAO and every later prediction
 * would then read. The PREDICTION is not wrong: it is the same samples
 * whatever the quantiser did. So the residual and its transform are
 * re-derived from `tb->pred` and the source - one fdct, not a second
 * motion compensation or intra gather - the lattice runs once, and
 * tb_make_recon_one rebuilds the recon from its levels.
 *
 * INTER only, and that is not an implementation shortcut: at 4:2:2 an
 * INTRA unit's lower block predicts from the upper block's recon
 * (tb_analyze's lend-and-restore), so changing the upper block's levels
 * invalidates the lower block's stored prediction. The intra commit path
 * therefore re-ANALYSES under `e->rdoq_commit` instead of re-quantising,
 * which redoes the gather and is exact at every format. */
static void tb_relattice_one(struct n265_frame_work *e, int c_idx, int x0,
                             int y0, int log2n, int qp, int mode,
                             tb_ctx *tb, size_t off, int *out_nz)
{
    int inter = mode == MODE_INTER;
    int use_dst = !inter && c_idx == 0 && log2n == 2;
    int n = 1 << log2n;
    int ps = e->pad_stride[c_idx];
    const pixel *pred = tb->pred + off;
    int16_t *level = tb->level + off;
    const pixel *src = e->pad_plane[c_idx] + (size_t)y0 * ps + x0;
    int16_t resid[32 * 32], coef[32 * 32];

    for (int y = 0; y < n; y++)
        for (int x = 0; x < n; x++)
            resid[y * n + x] = (int16_t)(src[(size_t)y * ps + x] -
                                         pred[y * n + x]);
    n265_fdct(coef, resid, log2n, use_dst);
    int scan = inter ? 0 : n265_scan_idx(mode, log2n, c_filt(e, c_idx));
    int sv = e->rdoq_commit;
    e->rdoq_commit = 1;
    tb_quant_one(e, c_idx, log2n, qp, !inter, scan, coef, level, out_nz);
    e->rdoq_commit = sv;
    tb_make_recon_one(c_idx, log2n, qp, mode, tb, off, *out_nz);
}

static void tb_relattice(struct n265_frame_work *e, int c_idx, int x0, int y0,
                         int log2n, int qp, int mode, tb_ctx *tb)
{
    int n = 1 << log2n;
    if (e->lossless)
        return;                 /* 8.6.2: there is no quantiser to re-run */
    if (c_idx)
        mode = n265_c422_mode(e->c422, mode);
    tb_relattice_one(e, c_idx, x0, y0, log2n, qp, mode, tb, 0, &tb->nz);
    if (nsub_c(e, c_idx) == 2)
        tb_relattice_one(e, c_idx, x0, y0 + n, log2n, qp, mode, tb,
                         (size_t)n * n, &tb->nz2);
}

/* blit the scratch recon into the frame and mark the cbf map */
static void tb_commit(struct n265_frame_work *e, int c_idx, int x0, int y0,
                      int log2n, tb_ctx *tb)
{
    int n = 1 << log2n;
    int rows = n * nsub_c(e, c_idx);
    pixel *recon = e->recon[c_idx];
    int stride = e->recon_stride[c_idx];
    for (int y = 0; y < rows; y++)
        memcpy(recon + (size_t)(y0 + y) * stride + x0, tb->recon + y * n,
               (size_t)n * sizeof(pixel));
    if (c_idx == 0)
        set_blk(e, e->blk_cbf_y, x0, y0, n < 4 ? 4 : n, tb->nz != 0);
    /* the census denominator: a commit-only lattice runs once here, per
     * BLOCK, which at 4:2:2 is the stacked pair and everywhere else the
     * one block tb_analyze_one was called for */
    if (e->rdoq && rdoq_trace())
        atomic_fetch_add_explicit(&rt_commit[c_idx != 0][log2n - 2],
                                  nsub_c(e, c_idx), memory_order_relaxed);
}

static void tb_recon(struct n265_frame_work *e, int c_idx, int x0, int y0,
                     int log2n, int qp, int mode, tb_ctx *tb)
{
    tb_make_recon(e, c_idx, log2n, qp, mode, tb);
    tb_commit(e, c_idx, x0, y0, log2n, tb);
}

/* SSD of a TB scratch recon against the source, the whole unit - the
 * stacked pair together at 4:2:2, since the pair IS the n x 2n area the
 * luma block presents. */
static uint64_t tb_ssd(struct n265_frame_work *e, int c_idx, int x0, int y0,
                       int log2n, const tb_ctx *tb)
{
    int n = 1 << log2n;
    int rows = n * nsub_c(e, c_idx);
    const pixel *src = e->pad_plane[c_idx] + y0 * e->pad_stride[c_idx] + x0;
    uint64_t ssd = n265_ssd_wh(src, e->pad_stride[c_idx], tb->recon, n, n,
                               rows);
    /* the psychovisual term (psy.c): luma only, the same strength for every
     * candidate so it only ever re-ranks them */
    if (c_idx == 0 && e->psy > 0.0)
        ssd += n265_psy_cost(src, e->pad_stride[0], tb->recon, n, n,
                             ctu_lam(e, e->cur_ctu_qp), e->psy);
    /* the two quantisers reconciled (N265_CHROMA_W); inert at 256 */
    if (c_idx && chroma_w_q8(e) != 256)
        ssd = (ssd * chroma_w_q8(e)) >> 8;
    return ssd;
}

/* ---- one chroma unit's syntax, in the three places it is spent -------
 *
 * The cbfs, the estimator's bins and the levels all go one-for-one at
 * 4:2:0 and 4:4:4 and two-for-one at 4:2:2, in the order 7.3.8.8 and
 * 7.3.8.10 put them: cb's pair, then cr's pair, then cb's residuals and
 * cr's. Written as three helpers rather than inline at each of the two
 * dozen sites, because the record stream and the replay have to agree
 * BYTE for byte and a site that forgot the second block would show up as
 * a decoder mismatch twenty CTUs later. */
static void rec_cbf_c(struct n265_frame_work *e, const tb_ctx *tb)
{
    rec_u8(e, tb->nz != 0);
    if (e->c422)
        rec_u8(e, tb->nz2 != 0);
}

static void rec_levels_c(struct n265_frame_work *e, const tb_ctx *tb,
                         int log2c)
{
    size_t a = (size_t)1 << (2 * log2c);
    if (tb->nz)
        rec_levels(e, tb->level, a);
    if (e->c422 && tb->nz2)
        rec_levels(e, tb->level + a, a);
}

static void est_cbf_c(struct n265_frame_work *e, est_state *st, int depth,
                      const tb_ctx *tb)
{
    st->bits += n265_est_bin(&st->ctx[CTX_CBF_CHROMA + depth], tb->nz != 0);
    if (e->c422)
        st->bits += n265_est_bin(&st->ctx[CTX_CBF_CHROMA + depth],
                                 tb->nz2 != 0);
}

static void est_res_c(struct n265_frame_work *e, const res_ops *ops,
                      const tb_ctx *tb, int log2c, int c_idx, int scan)
{
    size_t a = (size_t)1 << (2 * log2c);
    if (tb->nz)
        n265_residual_coding(ops, tb->level, log2c, c_idx, scan,
                             !e->lossless);
    if (e->c422 && tb->nz2)
        n265_residual_coding(ops, tb->level + a, log2c, c_idx, scan,
                             !e->lossless);
}

/* the chroma residual's scan: IntraPredModeC, so Table 8-3's remap at
 * 4:2:2 - and 7.4.9.11 gives a 4x4 block the mode-dependent scan whatever
 * the component is, so the remap is load-bearing there and not only at
 * 4:4:4's 8x8 chroma. */
static inline int c_scan_idx(const struct n265_frame_work *e, int mode,
                             int log2c)
{
    return mode == MODE_INTER
        ? 0
        : n265_scan_idx(n265_c422_mode(e->c422, mode), log2c,
                        c_filt(e, 1));
}

static const mot_t mot_zero = { { { 0, 0 }, { 0, 0 } }, 1 };

static int tb_full(struct n265_frame_work *e, int c_idx, int x0, int y0,
                   int log2n, int qp, int mode, tb_ctx *tb)
{
    tb_analyze(e, c_idx, x0, y0, log2n, qp, mode, mot_zero, tb);
    tb_recon(e, c_idx, x0, y0, log2n, qp, mode, tb);
    return tb->nz != 0;
}

#define INTRA_RD_MAX 8

/* pick the luma mode by SAD over prediction from the given ref plane;
 * optionally reports the nk best-ranked modes for RD refinement, and
 * optionally ranks only the `nsub` modes in `sub` instead of all 35 */
static int search_luma_mode_sub(struct n265_frame_work *e, int x0, int y0,
                                int log2n, const pixel *ref_plane,
                                int ref_stride, uint32_t *cost_out,
                                int topk[], int nk,
                                const int *sub, int nsub)
{
    int n = 1 << log2n;
    const pixel *src = e->pad_plane[0] + y0 * e->pad_stride[0] + x0;
    pixel refs[4 * 32 + 1], refs_f[4 * 32 + 1];
    pixel pred[32 * 32];

    n265_intra_refs(refs, ref_plane, ref_stride, x0, y0, log2n,
                    e->pad_w, e->pad_h, e->ctb_log2);
    /* 8.4.4.2.3 never smooths a 4x4: n265_use_filtered_refs returns 0 for
     * every mode at log2n 2, so the filtered copy below is written and
     * never read. 4x4 is the size the mode search visits most (6336 of
     * these calls in 30 frames of foreman at --preset fast), so the whole
     * filter is skipped there. refs_f stays uninitialised in that case,
     * which is sound because the same predicate guards every read of it. */
    if (log2n > 2)
        n265_filter_refs(refs_f, refs, log2n, 1);

    int best_mode = 0;
    uint32_t best_cost = UINT32_MAX;
    uint32_t ck[INTRA_RD_MAX];
    int mk[INTRA_RD_MAX];
    int nkc = nk < 1 ? 1 : nk > INTRA_RD_MAX ? INTRA_RD_MAX : nk;
    for (int i = 0; i < nkc; i++) { ck[i] = UINT32_MAX; mk[i] = i < 2 ? i : 26; }
    for (int mi = 0; mi < (sub ? nsub : 35); mi++) {
        int mode = sub ? sub[mi] : mi;
        const pixel *r = n265_use_filtered_refs(mode, log2n, 0) ? refs_f
                                                                  : refs;
        n265_intra_pred(pred, n, r, log2n, mode, 0);
        /* rank by SATD (post-transform bits proxy); the reported cost
         * stays SAD-domain for the callers that compare across metrics */
        uint32_t cost = n265_satd(src, e->pad_stride[0], pred, n, n);
        if (topk && cost < ck[nkc - 1]) {
            int p = nkc - 1;
            while (p > 0 && cost < ck[p - 1]) {
                ck[p] = ck[p - 1]; mk[p] = mk[p - 1];
                p--;
            }
            ck[p] = cost; mk[p] = mode;
        }
        if (cost < best_cost) {
            best_cost = cost;
            best_mode = mode;
        }
    }
    if (topk)
        for (int i = 0; i < nkc; i++)
            topk[i] = mk[i];
    if (cost_out) {
        const pixel *r = n265_use_filtered_refs(best_mode, log2n, 0)
                               ? refs_f : refs;
        n265_intra_pred(pred, n, r, log2n, best_mode, 0);
        *cost_out = n265_sad_wh(src, e->pad_stride[0], pred, n, n, n);
    }
    static int hist[35];
    if (mode_hist_on()) {
        hist[best_mode]++;
        static int registered;
        if (!registered) {
            registered = 1;
            atexit_hist(hist);
        }
        /* diagnostic only: the counter itself is not thread-safe and is
         * never written under a shipping configuration */
    }
    return best_mode;
}

static int search_luma_mode_k(struct n265_frame_work *e, int x0, int y0,
                              int log2n, const pixel *ref_plane,
                              int ref_stride, uint32_t *cost_out,
                              int topk[], int nk)
{
    return search_luma_mode_sub(e, x0, y0, log2n, ref_plane, ref_stride,
                                cost_out, topk, nk, NULL, 0);
}

static int search_luma_mode(struct n265_frame_work *e, int x0, int y0,
                            int log2n, const pixel *ref_plane,
                            int ref_stride, uint32_t *cost_out)
{
    return search_luma_mode_sub(e, x0, y0, log2n, ref_plane, ref_stride,
                                cost_out, NULL, 0, NULL, 0);
}

/* SAD-domain lambda: sqrt(0.68 * 2^((q-12)/3)), Q4 fixed point, indexed
 * by q = Qp' = qp + QP_BD_OFFSET.
 *
 * A SAD is a MAGNITUDE, so it grows by 1 << BIT_DEPTH_SHIFT at Main10
 * and its lambda must too. sqrt(2^((q-12)/3)) doubles every six of q and
 * QpBdOffsetY is 12, so indexing the SAME formula by the offset index
 * multiplies it by exactly 2^(12/6) = 4 - the scale a 10-bit SAD wants -
 * and no number here is refitted. Entries 52..63 are that formula
 * continued; the first 52 are unchanged and are what the 8-bit build
 * reads, since Qp' is qp there. */
static int lambda_sad_q4(int qp)
{
    static const uint16_t tab[64] = {
        3, 4, 4, 5, 5, 6, 7, 7, 8, 9, 10, 12, 13, 15, 17, 19,
        21, 24, 26, 30, 33, 37, 42, 47, 53, 59, 66, 75, 84, 94, 106, 118,
        133, 149, 168, 188, 211, 237, 266, 299, 335, 376, 422, 474, 532, 597, 670, 752,
        844, 948, 1064, 1194,
        1340, 1505, 1689, 1896, 2128, 2388, 2681, 3009, 3378, 3791, 4256, 4777
    };
    int q = n265_qp_clip(qp) + QP_BD_OFFSET;
    return tab[q];
}

/* the SAD-domain lambda is the square root of the RD one, so the CTU's
 * multiplier enters it as its own square root (cur_ctu_lam_sad_q8) */
static inline int ctu_lam_sad_q4(const struct n265_frame_work *e)
{
    int l = lambda_sad_q4(e->cur_ctu_qp);
    if (e->cur_ctu_lam_sad_q8 == 256)
        return l;
    return (int)(((uint64_t)l * e->cur_ctu_lam_sad_q8) >> 8);
}

static uint32_t mot_sad(struct n265_frame_work *e, int x0, int y0, int log2n,
                        mot_t mot);
static void est_tt_bits(struct n265_frame_work *e, est_state *st, int log2n,
                        int mode, const tb_ctx *tl, const tb_ctx *tcb,
                        const tb_ctx *tcr);
static void set_mot_map(struct n265_frame_work *e, int x0, int y0, int n,
                        mot_t mot);

/* mode-decision lambda 0.68 * 2^((q-12)/3), Q8, indexed by q = Qp' =
 * qp + QP_BD_OFFSET (shared with SAO; N265_LAMBDA overrides the factor).
 *
 * The distortion this lambda is added to is SSD in NATIVE sample units,
 * which grows by 1 << (2 * BIT_DEPTH_SHIFT) at Main10 - 16 - and the
 * lambda has to grow with it or every decision in the encoder gets the
 * rate term four times too cheap. 2^((q-12)/3) quadruples every six of
 * q, so the offset index supplies exactly 2^(12/3) = 16: the same table
 * formula, twelve entries further along, and nothing refitted. The
 * 8-bit build indexes 0..51 as it always has. */
uint32_t n265_lambda_mode_q8(int qp)
{
    static uint32_t tab[N265_QP_TAB_N];
    if (!tab[N265_QP_TAB_N - 1]) {
        for (int q = 0; q < N265_QP_TAB_N; q++) {
            double l = (n265_env("N265_LAMBDA") ? atof(n265_env("N265_LAMBDA")) : 0.68) * pow(2.0, (q - 12) / 3.0);
            tab[q] = (uint32_t)(l * 256.0 + 0.5);
        }
    }
    return tab[n265_qp_clip(qp) + QP_BD_OFFSET];
}

/* J = D + lambda * bits, with bits in Q8 and lambda in Q8.
 *
 * --lossless makes this the one place the mode decision changes shape.
 * Every arrangement that CODES its residual reconstructs the source
 * exactly, so D is identically zero and J is bits alone. The arms that
 * do NOT code their residual are the exception: a skip, and the whole-CU
 * skip at CB64, reconstruct the prediction, and a prediction is only
 * lossless where it happens to be exact. Those are not expensive
 * candidates, they are ILLEGAL ones, so any nonzero D costs infinity
 * rather than lambda's worth of bits.
 *
 * A skip whose prediction IS the source (D == 0, which is
 * exactly the case tb_analyze reports as nz == 0) is still the cheapest
 * arm there is and still wins.
 *
 * Every caller is a comparison against another J or against UINT64_MAX,
 * and no caller sums two of these, so the sentinel cannot leak into an
 * arithmetic result. */
static uint64_t rd_j(const struct n265_frame_work *e, uint64_t ssd,
                     uint32_t bits_q8, uint32_t lam_q8)
{
    if (e->lossless && ssd)
        return UINT64_MAX;
    return ssd + (((uint64_t)bits_q8 * lam_q8) >> 16);
}

/* Eight times the per-pixel SAD a CU may carry and still be taken as a
 * static skip. The knob is stated in 8-BIT LUMA LEVELS, which is the unit
 * it was fitted in and the unit the docs quote; the SAD it is compared
 * against is in native samples, so the return is scaled up by
 * BIT_DEPTH_SHIFT (a no-op at 8 bits) rather than the knob being
 * redefined per depth. */
static int skip_thr8(void)
{
    static int thr8 = -1;
    if (thr8 < 0) {
        const char *v = n265_env("N265_SKIP_THR");
        thr8 = v ? atoi(v) : 2;     /* strict: ~0.25/pixel; anything
                                     * looser must earn skip through the
                                     * residual-quantizes-to-zero test */
    }
    return thr8 << BIT_DEPTH_SHIFT;
}

/* N265_STATIC_COMMIT (phase 11 item 4): the census's P3, the static
 * commit re-derived, as a MULTIPLE of the strict static-skip bound. 0 is
 * off and is the compiled-in default; the value is the multiplier the
 * census swept (docs/phase9-goals-plan.md sec 11.5, PRED_B).
 *
 * Family B, which is the thing to be careful about: this is asked BEFORE
 * the merge arm rather than after it, so it saves the WHOLE node - and
 * every CU it catches that would not have ended as a skip is a CHANGED
 * DECISION, not a candidate the RD lost. The strict exit above is safe
 * at a quarter of a luma level per pixel and never fires at the goal
 * preset's pins (0 of 2.9 million final CUs, sec 11.3); this one fires,
 * and it is the two extra conjuncts that are supposed to pay for the
 * bound being ten times looser:
 *
 *   - the co-located CU of the nearest RETIRED frame was all skip, and
 *   - the cu-tree propagate-in over the node is exactly zero: nothing
 *     downstream predicts from this block, so a decision taken wrongly
 *     here does not travel.
 *
 * Neither is free of the other. The co-located skip alone is the WEAKEST
 * feature the census measured and on in_to_tree_720p it is inverted (sec
 * 11.4); the propagate alone separates about twice as well and in the
 * same direction on all six clips. Together they are what takes the
 * bound's false-catch area on touchdown from 19.0% at 10x to 1.2%. */
static int static_commit(void)
{
    static int v = -1;
    if (v < 0) {
        const char *s = n265_env("N265_STATIC_COMMIT");
        v = s ? atoi(s) : 0;
        if (v < 0) v = 0;
    }
    return v;
}

int n265_static_commit(void)
{
    return static_commit();
}

/* N265_SC_CONJ: which of the two stand-downs the commit carries, as a
 * bitmask - 1 the cu-tree propagate, 2 the co-located skip. 3, both, is
 * the default and is P3 as the census wrote it. It exists because the
 * two are not priced apart anywhere: sec 11.5 ranks the CONJUNCTION and
 * the census's own map is one coding frame old, while the encoder can
 * only read a map old enough to be retired at every thread count. This
 * knob is what says which of those two facts the arm's catch rate is
 * made of. */
static int sc_conj(void)
{
    static int v = -1;
    if (v < 0) {
        const char *s = n265_env("N265_SC_CONJ");
        v = s ? atoi(s) : 3;
        if (v < 0 || v > 3) v = 3;
    }
    return v;
}

/* P1, the early-out trio (docs/phase4-ladder.md). All three are off by
 * default; each one reshapes a decision, so each is priced on the band
 * before it becomes a default. */

/* N265_ES_THR: percent scale on the clean-skip distortion bound that
 * ends the quadtree trial. 100 is the shipped rule. */
static int es_thr_pct(void)
{
    static int v = -1;
    if (v < 0) {
        const char *s = n265_env("N265_ES_THR");
        v = s ? atoi(s) : 100;
    }
    return v;
}

/* N265_RSKIP: stop the recursion where the no-split winner carried no
 * residual at all. 1 also requires that neither decoded neighbour went
 * deeper than this node; 2 drops that condition. Measured and held
 * twice: P1 refused it stacked on the CBF_FAST=3 it had just shipped,
 * and R12 re-priced it on the R7 base at +0.14 mean BD for 1.050x
 * foreman - which passes on the CQP band and fails at the pinned CRF
 * point, where it costs +0.71 with two clips past the per-clip bar
 * (docs/phase4-ladder.md R12).
 *
 * 2 is the default since 2026-09-11 (phase 6b, docs/phase6b-economy.md):
 * re-priced on the NEON binary it is 1.045x for +0.20 CQP / +0.61 CRF of
 * band median, and medium is placed at an explicit quality budget now
 * rather than at R12's price rule. 0 is the pre-P1 encoder and what
 * `--preset slow` sets. */
static int rskip_mode(void)
{
    static int v = -1;
    if (v < 0) {
        const char *s = n265_env("N265_RSKIP");
        v = s ? atoi(s) : 2;    /* the unconditional form (P1/R12) */
    }
    return v;
}

/* N265_CBF_FAST: once a candidate quantises to nothing, stop trying the
 * expensive ones. Two independent stages: the merge candidate's cbf
 * gating AMVP, intra and the partitions, and the AMVP candidate's cbf
 * gating intra and the partitions. 1 is the first stage, 2 is both,
 * 3 is the second alone: stage 1 costs three times what stage 2 does and
 * buys less per point (docs/phase4-ladder.md). 0 is the pre-P1 encoder.
 *
 * 2 is the default since 2026-09-11 (phase 6b): both stages read 1.079x
 * on the NEON binary for +0.37 CQP / +0.54 CRF of band median, the fifth
 * cheapest arm on the re-priced ladder. 3 is what `--preset slow` sets. */
static int cbf_fast(void)
{
    static int v = -1;
    if (v < 0) {
        const char *s = n265_env("N265_CBF_FAST");
        v = s ? atoi(s) : 2;    /* both stages (P1, re-priced 6b) */
    }
    return v;
}

/* N265_GATE_SCALE (phase 4, the ladder's P8): percent scale on the two
 * economy gates the encoder has always had - the merge candidate's
 * prediction SAD under n*n/8 prunes the AMVP search, under n*n/4 the
 * intra arm. Both sit at a value that almost never fires
 * (`N265_NO_GATES=1` costs 1.2% on foreman), so this is how they get
 * re-derived against the band. 100 is the shipped rule, byte for byte. */
static int gate_scale(void)
{
    static int v = -1;
    if (v < 0) {
        const char *s = n265_env("N265_GATE_SCALE");
        v = s ? atoi(s) : 100;
        if (v < 1) v = 1;
    }
    return v;
}

/* N265_SKIP_EXIT (phase 4): the merge and skip arms run before the AMVP
 * search, so where they have already ended in a skip whose distortion
 * is inside the quantiser floor, the CU's winner is decided and the
 * search - and the partitions behind it - are bought to lose. 1 and 2
 * are the bound in multiples of the clean-skip level; 3 drops the bound
 * and asks only that skip beat the merge candidate's own RD.
 *
 * Still 0, and phase 6b is the second refusal. Alone on the NEON binary
 * it is 1.088x; inside the placed medium it is **1.016x**, because
 * `CBF_FAST=2` stage 1 already gates the AMVP search on the merge
 * candidate reaching cbf 0 and `RSKIP=2` has stopped the recursion on
 * the same population. 1.6% of the wall for +0.72 of CRF band median is
 * a marginal price of 0.45, the worst trade on the ladder after
 * `SBH_RD` (docs/phase6b-economy.md, point C). `--preset fast` still
 * takes it: at fast's own operating point the population it gates is a
 * different one.
 *
 * Third refusal, phase 6b step 5, on the phase 7 default: **1.026x
 * foreman / 1.007x mobile** at `--threads 1`, reproducing step 2's
 * in-company 1.016x geomean to the digit two placements later. Under the
 * item's 1.03x bar on both clips, so no band was run. `RECT_RANK`,
 * `SUBME` and `NO_RECT` all delete work and none of them puts back the
 * work this gate needs to find; its increment on top of `INTRA_GATE=2`
 * is 1.036x / 1.000x. */
static int skip_exit(void)
{
    static int v = -1;
    if (v < 0) {
        const char *s = n265_env("N265_SKIP_EXIT");
        v = s ? atoi(s) : 0;
    }
    return v;
}

/* N265_INTRA_GATE (P3): the intra RD trial inside an inter slice is
 * 10-14% of the encode and is bought at every CU whether or not the
 * inter arms left anything for it to win. Each mode adds a disjunct to
 * the gate on top of 1, the 2Nx2N winner being the skip: 2 the winning
 * inter candidate's distortion already sitting at the quantiser floor,
 * 3 the decoded neighbourhood being inter with no luma coefficients.
 * 4 and 5 instead keep the 35-mode rank and gate only the RD trial,
 * on the ranked intra prediction being worse than the merge prediction
 * by 1.5x and by 1.0x in the SAD units both are measured in. 5 is the
 * default since R12 - P3 refused it on the per-clip bar (coastguard
 * +1.59) and on the base R7 left it improves the band's mean, median
 * and worst clip at both operating points; 4 is P3's shipped value and
 * 0 the pre-P3 encoder.
 *
 * **The family is not monotone in speed**, which phase 6b step 5
 * measured and nothing before it says: 1, 2 and 3 gate the whole arm and
 * run the full 35-mode rank AND the RD trial wherever the gate does not
 * fire, while 4 and 5 keep the rank and gate only the trial. So 1 is
 * SLOWER than the default - 0.881x foreman / 0.783x mobile - because the
 * skip disjunct alone does not fire often enough to pay for the
 * prefilter it gives up. 3 reads 1.159x / 1.081x against 2's 1.161x /
 * 1.085x while gating strictly more, so it is dominated.
 *
 * **2 is refused a second time, on the per-clip bar.** On the phase 7
 * default it is worth 1.154x / 1.082x (geomean 1.117x), two and a half
 * times step 1's 1.044x, because steps 2-4 deleted the inter work it
 * shares the encode with. It keeps both band medians inside the -8%
 * floor at 7/7 ahead (CQP -13.65 -> -12.46, CRF -9.65 -> -9.41) and
 * costs `foreman_cif` **5.33 points of CRF and 5.46 of CQP**, with
 * coastguard, stefan and bus also past two points. Same population
 * `NO_RECT` charged foreman 5.02 for in step 4: where the 2Nx2N vector
 * does not explain the CU, rect is already gone and the intra ARM is
 * what is left carrying it, so gating the arm leaves no answer at all.
 * docs/phase6b-economy.md, step 5.
 *
 * **6 to 9 are step 6**, the search step 5 left open: the room between
 * mode 5's margin and mode 2's disjunct. The oracle
 * (`N265_INTRA_ORACLE`) measured the population first, and it says three
 * things.
 *
 * One: **mode 2's quantiser-floor disjunct is not a filter.** It fires
 * on every CU the family reaches that the skip disjunct has not already
 * taken - 91.8% of foreman's population and 87.2% of coastguard's, with
 * 98% of those at or under HALF the bound. Mode 2 never meant "gate
 * where the inter side is at the floor"; it meant gate the arm
 * everywhere, and no tighter floor recovers anything because the
 * distribution has no shoulder to cut at.
 *
 * Two: **mode 5's prefilter is already the discriminator.** Split the
 * population by whether the prefilter would gate the trial anyway and
 * the intra trial wins 6.9% / 8.6% of the half it gates against 64.6% /
 * 72.5% of the half it keeps. Inside what is left the trial usually
 * WINS, so the ceiling of a trial-only gate is low: mode 6, which drops
 * every trial mode 5 keeps, is bitstream-identical to mode 2 and is only
 * 1.078x / 1.014x, under this item's own 1.05x bar.
 *
 * Three: **the slice type is the one feature that separates inside it**,
 * and neither of the two the brief proposed does. The inter winner's
 * kind reads 66.4% against 60.8% on foreman and the other way on
 * coastguard; a size floor is the wrong sign on coastguard outright.
 * The B slice reads 19.5% / 1.6% against P's 83.4% / 86.0%, because an
 * intra CU in a B also pays `N265_B_INTRA_MARGIN`'s 30% for cutting the
 * prediction chain.
 *
 * **7 is the default since 2026-09-11 (phase 6b step 6).** In the floor
 * population of a B slice, on a frame not typed flat-shaded, the intra
 * arm stands down - rank and trial both, because a condition that reads
 * only the slice type can be decided where mode 2's disjunct is decided,
 * above the rank, and the rank is a third of the arm's wall on this
 * encoder rather than the quarter this comment used to claim. That is
 * the whole of its margin over the ceiling: it gates 29% / 16% of the
 * trials mode 6 gates and is FASTER than mode 6 on mobile.
 * **1.065x foreman / 1.042x mobile, geomean 1.053x, for +0.02 CQP and
 * +0.01 CRF of band median** with 7/7 ahead in both modes and a worst
 * clip of foreman +0.31 CQP - one seventeenth of what mode 2 charged
 * the same clip. Cheapest rung on the ladder by points per percent.
 *
 * The flat-shaded exception is step 4's, for step 4's reason: on a drawn
 * outline over a fill the intra arm IS the answer, and without the
 * exception `sita_toon_720p` pays 1.94 CQP where the CIF seven pay
 * 0.02. With it, all three animation clips are byte-identical to the
 * pre-step-6 encoder and all twelve natural ones are byte-identical to
 * the gate with no exception (24 cells of `cmp`).
 *
 * 8 adds the trial wherever the ranked intra SAD is within an eighth of
 * the merge SAD: 1.058x geomean for +0.42 CQP of band mean and
 * `coastguard_cif` +1.27, a marginal price of 72 points per multiple
 * against `SBH_RD`'s 5. Refused on price. 9 gates the trial in B slices
 * at a quarter-margin; it cannot reach the rank, so it is strictly
 * slower AND cheaper than 7, and 7 costs 0.31 on its worst clip. 6 is
 * kept as the calibration point it is: it prices mode 2's quality
 * exactly. docs/phase6b-economy.md, step 6. */
static int intra_gate(void)
{
    static int v = -1;
    if (v < 0) {
        const char *s = n265_env("N265_INTRA_GATE");
        v = s ? atoi(s) : 7;    /* + the B-slice arm gate (6b step 6) */
    }
    return v;
}

/* mode 4's prefilter margin, and every mode above it, in eighths of the
 * merge candidate's SAD */
static int intra_pf_q8(void)
{
    return intra_gate() >= 5 ? 8 : 12;
}

/* N265_INTRA_ORACLE (trace, never a default; phase 6b step 6). Turns
 * the intra-gate family off and runs the full arm - rank and RD trial -
 * at every CU the family reaches, then prints one `IO,` line per CU on
 * which mode 2's disjuncts WOULD have fired. The line carries what a
 * gate could condition on (CU size, which inter candidate won, that
 * winner's distortion against the quantiser floor, the prefilter's two
 * SADs) next to the outcome the gate is trying to predict (whether the
 * intra trial won the 2Nx2N RD comparison, and by how much), so
 * P(intra wins | mode 2 fires) can be split by feature before a gate is
 * designed rather than after. It CHANGES the bitstream by construction
 * - it un-gates the shipped prefilter - so it is a measurement tool and
 * never a default. */
static int intra_oracle(void)
{
    static int v = -1;
    if (v < 0) {
        const char *s = n265_env("N265_INTRA_ORACLE");
        v = s ? atoi(s) : 0;
    }
    return v;
}

/* P3 mode 3: every decoded 4x4 along this CU's left and top edges is
 * inter and carried no luma coefficients. Both edges are fully
 * available at this point - the left column ends at A1 and the top row
 * at B1, the same neighbours the AMVP derivation reads. */
static int nb_inter_clean(struct n265_frame_work *e, int x0, int y0, int n)
{
    int cells = n >> 2;
    if (!x0 && !y0)
        return 0;
    if (x0 > 0) {
        int cx = (x0 - 1) >> 2;
        for (int i = 0; i < cells; i++) {
            int m = ((y0 >> 2) + i) * e->map_stride + cx;
            if (e->blk_intra[m] || e->blk_cbf_y[m])
                return 0;
        }
    }
    if (y0 > 0) {
        int cy = (y0 - 1) >> 2;
        for (int i = 0; i < cells; i++) {
            int m = cy * e->map_stride + ((x0 >> 2) + i);
            if (e->blk_intra[m] || e->blk_cbf_y[m])
                return 0;
        }
    }
    return 1;
}

/* N265_PART_GATE (P4): tighten entry to the rectangular and asymmetric
 * partition trials, which together are 18-21% of the encode and run a
 * complete PU search - five merge candidates at SATD, a full ME per
 * list, a bi refinement - for all four PUs of both orientations before
 * one orientation is thrown away. 1 makes AMP wait for the rectangular
 * winner to beat 2Nx2N by a margin; 2 also asks the 2Nx2N AMVP
 * candidate to have carried residual before rect is entered at all;
 * 3 also drops rect at the min CB, where its PUs are 8x4/4x8 and its
 * share per bit is highest; 4 is 3 with the AMP margin widened from 2%
 * to 8%. 0 is the pre-P4 encoder.
 *
 * 2 is the default since 2026-09-11 (phase 6b). P4 refused it on the C
 * binary at 1.020x; on the NEON binary it is 1.041x for +0.01 CQP /
 * +0.53 CRF of band median - the fourth cheapest arm on the re-priced
 * ladder. 0 is what `--preset slow` sets. */
static int part_gate(void)
{
    static int v = -1;
    if (v < 0) {
        const char *s = n265_env("N265_PART_GATE");
        v = s ? atoi(s) : 2;    /* rect only where AMVP left residual */
    }
    return v;
}

/* percent by which the rectangular winner must beat the 2Nx2N winner
 * before the AMP orientations are searched */
static int part_amp_margin(void)
{
    return part_gate() >= 4 ? 8 : 2;
}

/* N265_RECT_RANK (P5): the two rectangular orientations are ranked today
 * by running a complete PU search for each and throwing one away, and
 * P4 established that ranking the orientations is where the partition
 * share is, not entering them. The value is the margin in percent by
 * which the cheap proxy below has to separate the two before the loser's
 * search is skipped: a larger margin runs both more often, and 0 is the
 * pre-P5 encoder.
 *
 * Measured and refused at 5, 20 and 50 (docs/phase4-ladder.md, R1), and
 * refused a third time in phase 6b step 2 on `sita_toon_720p`: the
 * proxy reads the 2Nx2N residual's row-vs-column deviation and
 * animation's residual is edges on flat fills, where that statistic
 * separates nothing, so the arm alone took that clip from -0.44% to
 * +0.22% BD-VMAF-NEG under CQP - two thirds of its whole margin over
 * x265 - for a CQP band median that improved.
 *
 * **50 is the shipped default since phase 6b step 3.** What changed is
 * not this proxy but the margin it was spending: the animation item
 * gave `sita_toon_720p` 5.86 points of CQP room where it had 0.44, and
 * against that the arm costs 0.46 there, 0.24 on `sita_flat_720p` and
 * 0.08 on `sintel_720p`. The CIF seven read +0.13 CQP / +0.83 CRF of
 * band median for 1.036x of the single-thread wall and 1.038x at auto
 * threads (docs/phase6b-economy.md, point A). The third refusal was
 * right on its own numbers and is still the reason the proxy is never
 * to be trusted on flat fills; it was a margin call, and the margin
 * moved. 0 is the pre-P5 encoder. */
static int rect_rank(void)
{
    static int v = -1;
    if (v < 0) {
        const char *s = n265_env("N265_RECT_RANK");
        v = s ? atoi(s) : 50;
    }
    return v;
}

/* N265_RECT_FLAT: keep 2NxN/Nx2N on a frame the content gate typed
 * flat-shaded (la.h) even where N265_NO_RECT deletes them everywhere
 * else. Read once per frame on the driver (dispatch_frame), so the
 * answer is frozen before a row job exists and is the same at every
 * thread count; AMP is NOT brought back with it, since N265_NO_AMP
 * stays whatever the preset said. 1 ships with the step 4 default;
 * 0 is plain `N265_NO_RECT=1` with no exception.
 *
 * The type is what makes this an exception rather than a retreat. On
 * flat-shaded animation a CU on a drawn outline is literally two motion
 * fields - the line and the fill it crosses - and that is the one
 * population a two-PU split was invented for, while the fills either
 * side of it are skips that cost the trial nothing. Everywhere else on
 * the corpus the fills' own motion is the CU's motion and the split has
 * nothing to explain, which is why `NO_RECT` is 1.303x for 1.47 points
 * of CIF-7 median there and 3.91 points on `sita_flat_720p` alone.
 *
 * The gate is the animation item's, unchanged and re-read here: over
 * the fifteen-clip corpus at 120 frames it fires on 120/120 frames of
 * all three animation clips (per-frame flat share 45-100 on sintel,
 * 54-89 on sita_flat, 13-26 on sita_toon) and on 0/120 of every natural
 * clip, the closest being foreman at 6. So this exception costs nothing
 * on twelve of fifteen clips and hands the other three the pre-step-4
 * medium exactly, byte for byte (docs/phase6b-economy.md, step 4's
 * identity table - it is proved by cmp, not argued from the gate). */
int n265_rect_flat(void)
{
    static int v = -1;
    if (v < 0) {
        const char *s = n265_env("N265_RECT_FLAT");
        v = s ? atoi(s) : 1;
    }
    return v;
}

/* Which way a two-PU split would cut, read off the 2Nx2N winner's own
 * residual. A CU that wants a horizontal boundary carries a residual
 * whose energy varies down the rows and not across the columns, because
 * one motion field is right for one band of rows and wrong for the
 * other. The mean absolute deviation of the row sums against that of the
 * column sums is that statement; the two share a mean by construction,
 * so they are directly comparable, and it is one pass over the CU
 * against try_part's two full PU searches. */
static void rect_proxy(struct n265_frame_work *e, int x0, int y0, int n,
                       const pixel *pred, uint64_t *e_h, uint64_t *e_v)
{
    const pixel *src = e->pad_plane[0] + y0 * e->pad_stride[0] + x0;
    int ps = e->pad_stride[0];
    uint32_t row[32], col[32];
    uint64_t tot = 0;

    n265_sad_rowcol(src, ps, pred, n, n, row, col);
    for (int y = 0; y < n; y++)
        tot += row[y];
    uint32_t mean = (uint32_t)(tot / (unsigned)n);
    uint64_t mr = 0, mc = 0;
    for (int i = 0; i < n; i++) {
        mr += row[i] > mean ? row[i] - mean : mean - row[i];
        mc += col[i] > mean ? col[i] - mean : mean - col[i];
    }
    *e_h = mr;      /* rows disagree -> a horizontal cut -> 2NxN */
    *e_v = mc;      /* columns disagree -> a vertical cut -> Nx2N */
}

/* N265_DEPTH_BOUND (P2): bound the depth range the quadtree searches to
 * the range the decoded neighbours and the collocated CTU already used.
 * 0 off; 1 widens that range by one each way; 3 takes it as it stands.
 * 2 and 4 are 1 and 3 plus the whole-CTU prediction-only skip trial at
 * depth 0 even where the bound forbids the CTB depth. 5 and 6 keep only
 * the upper half of the bound and read it locally, off the 4x4 cells
 * immediately left of and above the node rather than off whole
 * neighbouring CTUs: 5 stops one level below the deepest of them, 6 at
 * their depth. */
static int depth_bound(void)
{
    static int v = -1;
    if (v < 0) {
        const char *s = n265_env("N265_DEPTH_BOUND");
        v = s ? atoi(s) : 0;
    }
    return v;
}

/* how far the bound is widened each way, per mode */
static int depth_bound_widen(void)
{
    return depth_bound() >= 3 ? 0 : 1;
}

/* modes 5 and 6: the CTU-range bound is replaced by a local one */
static int depth_bound_local(void)
{
    return depth_bound() >= 5;
}

/* The deepest the decoded 4x4 cells immediately left of and above this
 * node went. Both edges are available in decode order - the same
 * neighbours split_inc and the AMVP derivation read - and the split
 * trial restores the map, so this reads the committed tree only. With
 * no decoded neighbour at all it does not bound. */
static int nb_depth_max(struct n265_frame_work *e, int x0, int y0, int n)
{
    int cells = n >> 2, hi = -1;
    if (x0 > 0) {
        int cx = (x0 - 1) >> 2;
        for (int i = 0; i < cells; i++) {
            int d = e->ct_depth[((y0 >> 2) + i) * e->map_stride + cx];
            if (d > hi) hi = d;
        }
    }
    if (y0 > 0) {
        int cy = (y0 - 1) >> 2;
        for (int i = 0; i < cells; i++) {
            int d = e->ct_depth[cy * e->map_stride + ((x0 >> 2) + i)];
            if (d > hi) hi = d;
        }
    }
    return hi;
}

/* whether the CTB skip trial survives a bound that forbids depth 0 */
static int depth_bound_ctb_skip(void)
{
    return depth_bound() == 2 || depth_bound() == 4;
}

/* strict static-region check at the merge candidate: used by the split
 * gate; the CU decision proper re-derives */
static int want_skip(struct n265_frame_work *e, int x0, int y0, int log2n)
{
    if (e->slice_type == 2)
        return 0;
    /* Not under --lossless, for the same reason analyze_cu's static-skip
     * arm is not: this is a prediction-SAD bound, and a quarter of a luma
     * level per pixel is not zero. It matters more here than there,
     * because at the CB64 node it is the ONLY path - analyze_cu's inter
     * candidate set is `log2n <= 5` and a 64x64 CU that reaches it
     * without the static skip falls through to the intra arm. Left in,
     * this shortcut sent every quiet 64x64 block of a lossless P slice
     * through a 64x64 INTRA CU and cost akiyo_cif 2.8x its bits. Off, the
     * CB64 node runs its own skip / merge / AMVP arms and the split trial
     * and the RD arbitration picks among them, with a lossy skip priced
     * at infinity. */
    if (e->lossless)
        return 0;
    int n = 1 << log2n;
    mot_t cand = n265_merge_cand0(e, x0, y0, n, n);
    return mot_sad(e, x0, y0, log2n, cand) * 8u <=
           (uint32_t)(n * n * skip_thr8());
}

/* transform tree for a 2Nx2N CU (intra or inter): records, per node,
 * [cbf_cb][cbf_cr] then per leaf [cbf_l][levels l][levels cb][levels cr].
 * A 64x64 CB splits into four 32x32 TUs (maxTB). */
/* pre_l/pre_cb/pre_cr, when given, are the flat-TB analysis of this mode
 * at this position that the caller already ran (intra_rd_pick's winner).
 * The intra prediction reads e->recon and nothing between the two calls
 * writes it, so re-running tb_analyze here would compute the same three
 * blocks a second time - one fdct, one RDOQ and one inverse per
 * component. NULL means analyse them here. */
static void tt_analyze_inner(struct n265_frame_work *e, int mode, int x0,
                             int y0, int log2ts, tb_ctx *pre_l,
                             tb_ctx *pre_cb, tb_ctx *pre_cr)
{
    int qp_c = n265_chroma_qp(e->cur_ctu_qp, e->chroma_idc);
    int csx = e->csx, csy = e->csy;
    int split = log2ts > 5;
    tb_ctx tb;

    if (split) {
        /* children reconstruct sequentially (a child's chroma prediction
         * reads the previous child's chroma recon), so the root cbfs -
         * which precede the children in the record stream - are patched
         * in after the loop. */
        int h = 1 << (log2ts - 1);
        size_t root_off = e->rec_len;
        rec_u8(e, 0);
        rec_u8(e, 0);
        int root_cb = 0, root_cr = 0;
        tb_ctx tcb, tcr;
        for (int i = 0; i < 4; i++) {
            int x1 = x0 + (i & 1) * h, y1 = y0 + (i >> 1) * h;
            int cx1 = x1 >> csx, cy1 = y1 >> csy;
            int log2c = log2ts - 1 - csx;
            mark_cu_edges(e, x1, y1, h);
            tb_analyze(e, 1, cx1, cy1, log2c, qp_c, mode, mot_zero, &tcb);
            tb_analyze(e, 2, cx1, cy1, log2c, qp_c, mode, mot_zero, &tcr);
            root_cb |= tb_any(&tcb);
            root_cr |= tb_any(&tcr);
            rec_cbf_c(e, &tcb);
            rec_cbf_c(e, &tcr);
            int cbf_l = tb_full(e, 0, x1, y1, log2ts - 1, e->cur_ctu_qp, mode, &tb);
            rec_u8(e, (unsigned)cbf_l);
            if (cbf_l)
                rec_levels(e, tb.level, h * h);
            tb_recon(e, 1, cx1, cy1, log2c, qp_c, mode, &tcb);
            rec_levels_c(e, &tcb, log2c);
            tb_recon(e, 2, cx1, cy1, log2c, qp_c, mode, &tcr);
            rec_levels_c(e, &tcr, log2c);
        }
        e->rec_buf[root_off] = (uint8_t)root_cb;
        e->rec_buf[root_off + 1] = (uint8_t)root_cr;
        return;
    }

    /* <= 32: choose flat TU == CU vs four TUs with sequential intra
     * prediction (each TU predicts from the recon of the previous ones,
     * so the split arm commits as it goes and restores on loss) */
    int n = 1 << log2ts;
    mark_cu_edges(e, x0, y0, n);
    uint32_t lam = ctu_lam(e, e->cur_ctu_qp);
    int h = n >> 1;
    int cx0 = x0 >> csx, cy0 = y0 >> csy;
    int log2c = log2ts - csx;           /* this node's chroma TB */
    int cn_w = n >> csx, cn_h = n >> csy;
    /* Does each luma child carry its own chroma, or does the node keep
     * one chroma block for all four (7.3.8.10's blkIdx == 3 arm)? The
     * merge exists only because a 4:2:0 chroma child of an 8x8 luma node
     * would be 2x2, which is not a transform block; the condition is
     * exactly "the child's chroma would be smaller than 4x4", and at
     * 4:4:4 that never happens. Folds to `log2ts > 3` at 4:2:0. */
    int child_chroma = (log2ts - 1 - csx) >= 2;

    /* flat arrangement (no frame commits yet) */
    tb_ctx tcb, tcr;
    tb_ctx *ptb = &tb, *ptcb = &tcb, *ptcr = &tcr;
    if (pre_l) {
        ptb = pre_l; ptcb = pre_cb; ptcr = pre_cr;
    } else {
        tb_analyze(e, 1, cx0, cy0, log2c, qp_c, mode, mot_zero, ptcb);
        tb_analyze(e, 2, cx0, cy0, log2c, qp_c, mode, mot_zero, ptcr);
        tb_analyze(e, 0, x0, y0, log2ts, e->cur_ctu_qp, mode, mot_zero, ptb);
        tb_make_recon(e, 0, log2ts, e->cur_ctu_qp, mode, ptb);
        tb_make_recon(e, 1, log2c, qp_c, mode, ptcb);
        tb_make_recon(e, 2, log2c, qp_c, mode, ptcr);
    }
    uint64_t d_flat = tb_ssd(e, 0, x0, y0, log2ts, ptb) +
        tb_ssd(e, 1, cx0, cy0, log2c, ptcb) +
        tb_ssd(e, 2, cx0, cy0, log2c, ptcr);
    cabac_ctx_t local[CTX_COUNT];
    est_state st = { local, 0 };
    memcpy(local, e->est_ctx, sizeof(local));
    st.bits = 0;
    est_tt_bits(e, &st, log2ts, mode, ptb, ptcb, ptcr);
    uint64_t j_flat = rd_j(e, d_flat, st.bits, lam);

    /* split arrangement: sequential, committing recon TU by TU.
     *
     * The luma pair stays on the stack; the chroma pair is the lane's
     * (struct n265_scratch, and the note there), and the two planes are
     * PACKED at this node's own chroma size rather than given a 32x32 slot
     * each, so the copy loops below walk what they touch. */
    int cpl = cn_w * cn_h;              /* the packed chroma plane */
    pixel save_l[32 * 32], split_l[32 * 32];
    pixel *save_c = e->sc->tt_save_c, *split_c = e->sc->tt_split_c;
    for (int y = 0; y < n; y++)
        memcpy(save_l + y * n,
               e->recon[0] + (y0 + y) * e->recon_stride[0] + x0,
               (size_t)n * sizeof(pixel));
    for (int c = 0; c < 2; c++)
        for (int y = 0; y < cn_h; y++)
            memcpy(save_c + c * cpl + y * cn_w,
                   e->recon[c + 1] + (cy0 + y) * e->recon_stride[c + 1] + cx0,
                   (size_t)cn_w * sizeof(pixel));

    tb_ctx *s_l = e->sc->itt_l;
    tb_ctx (*s_c)[2] = e->sc->itt_c;
    uint64_t d_split = 0;
    int root_cb = 0, root_cr = 0;
    for (int i = 0; i < 4; i++) {
        int x1 = x0 + (i & 1) * h, y1 = y0 + (i >> 1) * h;
        tb_analyze(e, 0, x1, y1, log2ts - 1, e->cur_ctu_qp, mode, mot_zero,
                   &s_l[i]);
        tb_recon(e, 0, x1, y1, log2ts - 1, e->cur_ctu_qp, mode, &s_l[i]);
        d_split += tb_ssd(e, 0, x1, y1, log2ts - 1, &s_l[i]);
        if (child_chroma) {
            int kx = x1 >> csx, ky = y1 >> csy, kl = log2ts - 1 - csx;
            tb_analyze(e, 1, kx, ky, kl, qp_c, mode, mot_zero, &s_c[i][0]);
            tb_recon(e, 1, kx, ky, kl, qp_c, mode, &s_c[i][0]);
            tb_analyze(e, 2, kx, ky, kl, qp_c, mode, mot_zero, &s_c[i][1]);
            tb_recon(e, 2, kx, ky, kl, qp_c, mode, &s_c[i][1]);
            d_split += tb_ssd(e, 1, kx, ky, kl, &s_c[i][0]);
            d_split += tb_ssd(e, 2, kx, ky, kl, &s_c[i][1]);
            root_cb |= tb_any(&s_c[i][0]);
            root_cr |= tb_any(&s_c[i][1]);
        }
    }
    if (!child_chroma) {
        /* The 8x8 node's chroma stays at the PARENT, predicted after the
         * luma children per decode order: one 4x4 block at 4:2:0 and
         * 7.3.8.8's stacked 4x4 pair at 4:2:2 (its second cbf is coded at
         * this node even though the node is split - that is the
         * `log2TrafoSize == 3` half of the condition). */
        tb_analyze(e, 1, cx0, cy0, 2, qp_c, mode, mot_zero, &s_c[0][0]);
        tb_recon(e, 1, cx0, cy0, 2, qp_c, mode, &s_c[0][0]);
        tb_analyze(e, 2, cx0, cy0, 2, qp_c, mode, mot_zero, &s_c[0][1]);
        tb_recon(e, 2, cx0, cy0, 2, qp_c, mode, &s_c[0][1]);
        d_split += tb_ssd(e, 1, cx0, cy0, 2, &s_c[0][0]);
        d_split += tb_ssd(e, 2, cx0, cy0, 2, &s_c[0][1]);
        root_cb = tb_any(&s_c[0][0]);
        root_cr = tb_any(&s_c[0][1]);
    }
    /* keep the split recon, restore the pre-CU state */
    for (int y = 0; y < n; y++) {
        memcpy(split_l + y * n,
               e->recon[0] + (y0 + y) * e->recon_stride[0] + x0,
               (size_t)n * sizeof(pixel));
        memcpy(e->recon[0] + (y0 + y) * e->recon_stride[0] + x0,
               save_l + y * n, (size_t)n * sizeof(pixel));
    }
    for (int c = 0; c < 2; c++)
        for (int y = 0; y < cn_h; y++) {
            memcpy(split_c + c * cpl + y * cn_w,
                   e->recon[c + 1] + (cy0 + y) * e->recon_stride[c + 1] + cx0,
                   (size_t)cn_w * sizeof(pixel));
            memcpy(e->recon[c + 1] + (cy0 + y) * e->recon_stride[c + 1] + cx0,
                   save_c + c * cpl + y * cn_w, (size_t)cn_w * sizeof(pixel));
        }

    memcpy(local, e->est_ctx, sizeof(local));
    st.bits = 0;
    {
        res_ops ops;
        n265_est_ops(&ops, &st);
        st.bits += n265_est_bin(&local[CTX_TSPLIT + 5 - log2ts], 1);
        if (child_chroma) {
            /* one cbf per component at the parent: it gates the
             * children and is not a block of its own */
            st.bits += n265_est_bin(&local[CTX_CBF_CHROMA + 0], root_cb);
            st.bits += n265_est_bin(&local[CTX_CBF_CHROMA + 0], root_cr);
        } else {
            /* the 8x8 node keeps the chroma, so the cbfs coded here ARE
             * the unit's - the pair of them at 4:2:2 */
            est_cbf_c(e, &st, 0, &s_c[0][0]);
            est_cbf_c(e, &st, 0, &s_c[0][1]);
        }
        for (int i = 0; i < 4; i++) {
            if (child_chroma) {
                if (root_cb)
                    est_cbf_c(e, &st, 1, &s_c[i][0]);
                if (root_cr)
                    est_cbf_c(e, &st, 1, &s_c[i][1]);
            }
            st.bits += n265_est_bin(&local[CTX_CBF_LUMA + 0],
                                    s_l[i].nz != 0);
            if (s_l[i].nz)
                n265_residual_coding(&ops, s_l[i].level, log2ts - 1, 0,
                                     n265_scan_idx(mode, log2ts - 1, 0),
                                     !e->lossless);
            if (child_chroma) {
                int kl = log2ts - 1 - csx;
                int sc = c_scan_idx(e, mode, kl);
                est_res_c(e, &ops, &s_c[i][0], kl, 1, sc);
                est_res_c(e, &ops, &s_c[i][1], kl, 2, sc);
            } else if (i == 3) {
                int sc = c_scan_idx(e, mode, 2);
                est_res_c(e, &ops, &s_c[0][0], 2, 1, sc);
                est_res_c(e, &ops, &s_c[0][1], 2, 2, sc);
            }
        }
    }
    uint64_t j_split = rd_j(e, d_split, st.bits, lam);

    if (j_flat <= j_split) {
        rec_u8(e, 0);
        rec_cbf_c(e, ptcb);
        rec_cbf_c(e, ptcr);
        rec_u8(e, ptb->nz != 0);
        tb_commit(e, 0, x0, y0, log2ts, ptb);
        if (ptb->nz)
            rec_levels(e, ptb->level, n * n);
        tb_commit(e, 1, cx0, cy0, log2c, ptcb);
        rec_levels_c(e, ptcb, log2c);
        tb_commit(e, 2, cx0, cy0, log2c, ptcr);
        rec_levels_c(e, ptcr, log2c);
        return;
    }

    /* commit split: blit the saved split recon and write records */
    for (int y = 0; y < n; y++)
        memcpy(e->recon[0] + (y0 + y) * e->recon_stride[0] + x0,
               split_l + y * n, (size_t)n * sizeof(pixel));
    for (int c = 0; c < 2; c++)
        for (int y = 0; y < cn_h; y++)
            memcpy(e->recon[c + 1] + (cy0 + y) * e->recon_stride[c + 1] + cx0,
                   split_c + c * cpl + y * cn_w, (size_t)cn_w * sizeof(pixel));
    rec_u8(e, 1);
    if (child_chroma) {
        rec_u8(e, (unsigned)root_cb);
        rec_u8(e, (unsigned)root_cr);
    } else {
        rec_cbf_c(e, &s_c[0][0]);
        rec_cbf_c(e, &s_c[0][1]);
    }
    for (int i = 0; i < 4; i++) {
        int x1 = x0 + (i & 1) * h, y1 = y0 + (i >> 1) * h;
        mark_cu_edges(e, x1, y1, h);
        set_blk(e, e->blk_cbf_y, x1, y1, h, s_l[i].nz != 0);
        if (child_chroma) {
            rec_cbf_c(e, &s_c[i][0]);
            rec_cbf_c(e, &s_c[i][1]);
        }
        rec_u8(e, s_l[i].nz != 0);
        if (s_l[i].nz)
            rec_levels(e, s_l[i].level, h * h);
        if (child_chroma) {
            int kl = log2ts - 1 - csx;
            rec_levels_c(e, &s_c[i][0], kl);
            rec_levels_c(e, &s_c[i][1], kl);
        } else if (i == 3) {
            rec_levels_c(e, &s_c[0][0], 2);
            rec_levels_c(e, &s_c[0][1], 2);
        }
    }
}

/* Everything tt_analyze runs is on the commit path - both arrangements
 * are analysed here and one of them IS the coded block - so under
 * N265_RDOQ=2 the whole of it is lattice work. intra_rd_pick's winner
 * (`pre_l`) is not: it was analysed with the plain quantiser during the
 * mode decision, and a 4:2:2 unit's stored prediction cannot be
 * re-quantised in place (tb_relattice), so the flat arm re-analyses it
 * here instead of reusing it. */
static void tt_analyze(struct n265_frame_work *e, int mode, int x0, int y0,
                       int log2ts, tb_ctx *pre_l, tb_ctx *pre_cb,
                       tb_ctx *pre_cr)
{
    int sv = e->rdoq_commit;
    e->rdoq_commit = 1;
    if (e->rdoq >= 2)
        pre_l = pre_cb = pre_cr = NULL;
    tt_analyze_inner(e, mode, x0, y0, log2ts, pre_l, pre_cb, pre_cr);
    e->rdoq_commit = sv;
}

static void set_mot_map_wh(struct n265_frame_work *e, int x0, int y0, int w,
                           int h, mot_t mot)
{
    for (int y = 0; y < h; y += 4)
        for (int x = 0; x < w; x += 4) {
            int m = ((y0 + y) >> 2) * e->map_stride + ((x0 + x) >> 2);
            e->blk_mv[4 * m] = mot.mv[0].x;
            e->blk_mv[4 * m + 1] = mot.mv[0].y;
            e->blk_mv[4 * m + 2] = mot.mv[1].x;
            e->blk_mv[4 * m + 3] = mot.mv[1].y;
            e->blk_pred[m] = mot.pred;
            e->blk_ref[2 * m] = mot.ref[0];
            e->blk_ref[2 * m + 1] = mot.ref[1];
        }
}

static void set_mot_map(struct n265_frame_work *e, int x0, int y0, int n,
                        mot_t mot)
{
    for (int y = 0; y < n; y += 4)
        for (int x = 0; x < n; x += 4) {
            int m = ((y0 + y) >> 2) * e->map_stride + ((x0 + x) >> 2);
            e->blk_mv[4 * m] = mot.mv[0].x;
            e->blk_mv[4 * m + 1] = mot.mv[0].y;
            e->blk_mv[4 * m + 2] = mot.mv[1].x;
            e->blk_mv[4 * m + 3] = mot.mv[1].y;
            e->blk_pred[m] = mot.pred;
            e->blk_ref[2 * m] = mot.ref[0];
            e->blk_ref[2 * m + 1] = mot.ref[1];
        }
}

static int cpred_trace(void);
static void cpred_trace_pu(struct n265_frame_work *e, const pu_geo *g,
                           mot_t mot);

static void skip_cu(struct n265_frame_work *e, int x0, int y0, int log2n,
                    mot_t mot, int merge_idx)
{
    int n = 1 << log2n;
    rec_u8(e, (unsigned)merge_idx);
    pixel pred[64 * 64];
    for (int c = 0; c < 3; c++) {
        int shx = c ? e->csx : 0, shy = c ? e->csy : 0;
        int cw = n >> shx, ch = n >> shy;
        mc_pred_wh(e, c, x0 >> shx, y0 >> shy, cw, ch, mot, pred, cw);
        for (int y = 0; y < ch; y++)
            memcpy(e->recon[c] + ((y0 >> shy) + y) * e->recon_stride[c] +
                       (x0 >> shx),
                   pred + y * cw, (size_t)cw * sizeof(pixel));
    }
    set_blk(e, e->blk_intra, x0, y0, n, 0);
    set_blk(e, e->blk_skip, x0, y0, n, 1);
    set_blk(e, e->blk_cbf_y, x0, y0, n, 0);
    if (cpred_trace()) {
        pu_geo g = { x0, y0, n, x0, y0, n, n, 0, 0 };
        cpred_trace_pu(e, &g, mot);
    }
    set_mot_map(e, x0, y0, n, mot);
    set_mode_map(e, x0, y0, n, INTRA_DC);
}

/* SAD of the full-motion prediction (luma) */
static uint32_t mot_sad(struct n265_frame_work *e, int x0, int y0, int log2n,
                        mot_t mot)
{
    int n = 1 << log2n;
    pixel pred[64 * 64];
    mc_pred(e, 0, x0, y0, n, mot, pred);
    const pixel *src = e->pad_plane[0] + y0 * e->pad_stride[0] + x0;
    return n265_sad_wh(src, e->pad_stride[0], pred, n, n, n);
}

/* SATD of the full-motion prediction (luma): the decision metric that
 * actually tracks post-transform bits */
static uint32_t mot_satd_wh(struct n265_frame_work *e, int x0, int y0, int w,
                            int h, mot_t mot)
{
    pixel pred[64 * 64];
    mc_pred_wh(e, 0, x0, y0, w, h, mot, pred, w);
    return n265_satd_wh(e->pad_plane[0] + y0 * e->pad_stride[0] + x0,
                        e->pad_stride[0], pred, w, w, h);
}

/* CHROMA_NA: this PU has no chroma SATD to take. n265_satd_wh tiles 4x4
 * at best, so the CHROMA block needs both sides a multiple of 4 - which
 * at 4:2:0 is both luma sides a multiple of 8 (the 8x4 / 4x8 PUs give
 * chroma 4x2, the AMP PUs of a 16x16 CB give 8x2 / 2x8) and at 4:4:4 is
 * both luma sides a multiple of 4, i.e. every PU this encoder has. The
 * test is written on the chroma block so it says the same thing at both
 * formats; every caller below leaves the exceptions' choice luma-only. */
#define CHROMA_NA UINT32_MAX

/* SATD of the full-motion prediction summed over Cb and Cr, for a PU
 * whose LUMA block is w x h at (x0, y0). */
static uint32_t mot_satd_c(struct n265_frame_work *e, int x0, int y0, int w,
                           int h, mot_t mot)
{
    int cw = w >> e->csx, ch = h >> e->csy;
    int cx = x0 >> e->csx, cy = y0 >> e->csy;
    if ((cw & 3) || (ch & 3))
        return CHROMA_NA;
    pixel pred[64 * 64];
    uint32_t s = 0;
    for (int c = 1; c <= 2; c++) {
        mc_pred_wh(e, c, cx, cy, cw, ch, mot, pred, cw);
        s += n265_satd_wh(e->pad_plane[c] + cy * e->pad_stride[c] + cx,
                          e->pad_stride[c], pred, cw, cw, ch);
    }
    return s;
}

/* SAD twin of the above, for the arbitrations that rank in SAD. The
 * chroma term has to be in the metric its call site ranks in: a SATD is
 * two to four times a SAD on the same block, so mixing them would be a
 * weight and not a term. No shape guard is needed - every SAD site below
 * is a whole 2Nx2N coding block, so the chroma block is at least 4x4. */
static uint32_t mot_sad_c(struct n265_frame_work *e, int x0, int y0, int w,
                          int h, mot_t mot)
{
    int cw = w >> e->csx, ch = h >> e->csy;
    int cx = x0 >> e->csx, cy = y0 >> e->csy;
    pixel pred[64 * 64];
    uint32_t s = 0;
    for (int c = 1; c <= 2; c++) {
        mc_pred_wh(e, c, cx, cy, cw, ch, mot, pred, cw);
        s += n265_sad_wh(e->pad_plane[c] + cy * e->pad_stride[c] + cx,
                         e->pad_stride[c], pred, cw, cw, ch);
    }
    return s;
}

/* N265_CPRED_W: the chroma term's weight in percent, 100 = w 1.0. Its
 * reason for existing is 0, the LUMA-ONLY CONTROL: N265_CHROMA_PRED=2
 * also re-ranks the search's final vector against its quarter-pel
 * neighbours, and without this knob there is no way to say how much of
 * that arm is the chroma term and how much is the re-rank. It is also
 * the strength sweep if one is ever wanted. */
static int cpred_w_pct(void)
{
    static int v = -1;
    if (v < 0) { const char *s = n265_env("N265_CPRED_W"); v = s ? atoi(s) : 100; }
    return v;
}

/* N265_CPRED_GATE: which of the shortlist's candidates are allowed to
 * cost a chroma MC. The full form (0) gives one to every entry at every
 * quadtree depth on every trial, which is the 1.04-1.06x that refused
 * N265_CHROMA_PRED=1 on the clock; these are the cheaper forms.
 *
 *   0  every shortlist entry (the priced, refused form: 1.06x / 1.04x)
 *   1  the two best on luma only - a third candidate that is already
 *      behind on luma is not going to be rescued by a term worth a
 *      tenth to a fifth of the luma magnitude (1.031x / 1.021x)
 *   2  the two best, and only where they are within 2% of each other on
 *      luma: the chroma MC is bought only where it can change the
 *      answer, which the misalignment trace put at 5.5-7.6% of PUs
 *   3  the full form, skipped at the 8x8 coding block (1.040x / 1.026x)
 *
 * **2 ships, and it is both the cheapest and the best of the four**:
 * 1.010x on foreman and mobile against 0's 1.06x / 1.04x, and a BETTER
 * band than 0 in both modes (CIF-7 median -16.43 / -13.91 against 0's
 * -16.21 / -13.78). Spending the term only where luma has not already
 * decided is not only cheaper, it is also the term not being spent where
 * it can only add noise. Moot unless N265_CHROMA_PRED is set. */
static int cpred_gate(void)
{
    static int v = -1;
    if (v < 0) { const char *s = n265_env("N265_CPRED_GATE"); v = s ? atoi(s) : 2; }
    return v;
}

/* the two smallest entries of cost[0..k-1], first index winning a tie,
 * as the shortlist gates above need them. b[1] is k < 2's own b[0]. */
static void two_best(const uint32_t *cost, int k, int b[2])
{
    b[0] = 0;
    for (int i = 1; i < k; i++)
        if (cost[i] < cost[b[0]])
            b[0] = i;
    b[1] = b[0];
    for (int i = 0; i < k; i++)
        if (i != b[0] && (b[1] == b[0] || cost[i] < cost[b[1]]))
            b[1] = i;
}

/* the chroma term at w = 1.0, or 0 where the PU has no chroma tile */
static inline uint32_t cpred_satd(struct n265_frame_work *e, int x0, int y0,
                                  int w, int h, mot_t mot)
{
    int pct = cpred_w_pct();
    if (!pct)
        return 0;
    uint32_t c = mot_satd_c(e, x0, y0, w, h, mot);
    if (c == CHROMA_NA)
        return 0;
    return pct == 100 ? c : (uint32_t)(((uint64_t)c * pct) / 100);
}

/* the SAD twin of the above, for the sites that rank in SAD */
static inline uint32_t cpred_sad(struct n265_frame_work *e, int x0, int y0,
                                 int w, int h, mot_t mot)
{
    int pct = cpred_w_pct();
    if (!pct)
        return 0;
    uint32_t c = mot_sad_c(e, x0, y0, w, h, mot);
    return pct == 100 ? c : (uint32_t)(((uint64_t)c * pct) / 100);
}

/* --- estimator-side syntax costing (mirrors entropy.c) --- */

static void est_mpm_bits(struct n265_frame_work *e, est_state *st, int x0,
                         int y0, int mode)
{
    int a = INTRA_DC, b = INTRA_DC;
    if (x0 > 0 && e->blk_intra[(y0 >> 2) * e->map_stride + ((x0 - 1) >> 2)])
        a = e->intra_mode[(y0 >> 2) * e->map_stride + ((x0 - 1) >> 2)];
    if (y0 > 0 && (y0 & ((1 << e->ctb_log2) - 1)) != 0 &&
        e->blk_intra[((y0 - 1) >> 2) * e->map_stride + (x0 >> 2)])
        b = e->intra_mode[((y0 - 1) >> 2) * e->map_stride + (x0 >> 2)];
    int cand[3];
    if (a == b) {
        if (a < 2) { cand[0] = 0; cand[1] = 1; cand[2] = 26; }
        else { cand[0] = a; cand[1] = 2 + ((a + 29) % 32);
               cand[2] = 2 + ((a - 2 + 1) % 32); }
    } else {
        cand[0] = a; cand[1] = b;
        cand[2] = (a != 0 && b != 0) ? 0 : (a != 1 && b != 1) ? 1 : 26;
    }
    int idx = -1;
    for (int i = 0; i < 3; i++)
        if (cand[i] == mode)
            idx = i;
    st->bits += n265_est_bin(&st->ctx[CTX_PREV_INTRA], idx >= 0);
    st->bits += n265_est_bypass_bits(idx >= 0 ? (idx == 0 ? 1 : 2) : 5);
}

static int est_skip_inc(struct n265_frame_work *e, int x0, int y0)
{
    int inc = 0;
    if (x0 > 0)
        inc += e->blk_skip[(y0 >> 2) * e->map_stride + ((x0 - 1) >> 2)];
    if (y0 > 0)
        inc += e->blk_skip[((y0 - 1) >> 2) * e->map_stride + (x0 >> 2)];
    return inc;
}

/* mvd_coding (7.3.8.9): the two greater-flags on their own contexts, the
 * EG1 remainder and the sign in bypass. The contexts are advanced, so a
 * caller that only wants a PRICE passes copies. Q8 bits. */
static uint32_t mvd_cost_q8(cabac_ctx_t *g0, cabac_ctx_t *g1, int mx, int my)
{
    int ax = mx < 0 ? -mx : mx, ay = my < 0 ? -my : my;
    uint32_t b = 0;
    b += n265_est_bin(g0, ax > 0);
    b += n265_est_bin(g0, ay > 0);
    if (ax > 0)
        b += n265_est_bin(g1, ax > 1);
    if (ay > 0)
        b += n265_est_bin(g1, ay > 1);
    for (int c = 0; c < 2; c++) {
        int a = c ? ay : ax;
        if (a > 0) {
            if (a > 1) {
                int v = a - 2, bl = 1, bits = 1;
                while (v >= (1 << bl)) { v -= 1 << bl; bl++; bits += 2; }
                b += n265_est_bypass_bits(bits + bl - 1);
            }
            b += n265_est_bypass_bits(1);
        }
    }
    return b;
}

static void est_mvd(est_state *st, int mx, int my)
{
    st->bits += mvd_cost_q8(&st->ctx[CTX_MVD_GR0], &st->ctx[CTX_MVD_GR1],
                            mx, my);
}

/* --- the motion-candidate rate model (N265_MVD_BITS, N265_BI_REFINE) --- */

static int mvd_bits_on(void)
{
    static int on = -1;
    if (on < 0) {
        const char *v = n265_env("N265_MVD_BITS");
        on = v ? atoi(v) != 0 : 0;
    }
    return on;
}

/* Hadamard instead of SAD in the 2Nx2N reference ranking and the
 * uni/bi arbitration (the rect/AMP PU search already uses it) */
static int me_satd_on(void)
{
    static int on = -1;
    if (on < 0) {
        const char *v = n265_env("N265_ME_SATD");
        on = v ? atoi(v) != 0 : 0;
    }
    return on;
}

/* R3: the lookahead's inter-against-intra cost over this node, in
 * percent - sum(pcost) x 100 / sum(icost) over the lowres 8x8 blocks it
 * covers, the same grid cu-tree averages over. -1 where there is no
 * field. It is computed before any coding decision is taken, which is
 * the property P2's depth bound did not have. */
static int la_inter_pct(struct n265_frame_work *e, int x0, int y0, int n)
{
    if (!e->la_icost || !e->la_bw)
        return -1;
    int b0x = x0 >> 4, b0y = y0 >> 4, nb = n >> 4;
    uint64_t ic = 0, pc = 0;
    for (int by = b0y; by < b0y + nb && by < e->la_bh; by++)
        for (int bx = b0x; bx < b0x + nb && bx < e->la_bw; bx++) {
            int b = by * e->la_bw + bx;
            ic += e->la_icost[b];
            pc += e->la_pcost[b];
        }
    if (!ic)
        return -1;
    return (int)(pc * 100u / ic);
}

/* N265_CB64_GATE (R3): the AMVP arm at the 64x64 coding block runs a
 * whole-CTU PU search and, where that search prefers AMVP to merge, a
 * commit through the forced TU32 quad. It is 8% of the encode by A/B and
 * it exists for one case: a 64x64 region whose motion is not in the
 * merge list. Where the CB64 merge arms have ALREADY improved a long way
 * on the CU arm, the merge list evidently does have the region's motion,
 * and the search is bought to lose. 1 and 2 gate on that improvement
 * exceeding 60 and 75 percent; 3 gates instead on the lookahead calling
 * the region badly inter-predicted (pcost at or above 70% of icost),
 * where a single 64x64 motion field is not what it is missing. 0 is the
 * pre-R3 encoder, byte for byte.
 *
 * 1 is the default: -0.01 mean BD on the band - free to the resolution
 * of the instrument - for 1.033x foreman. 2 is the same rule at a bound
 * that fires too rarely to pay (1.005x). 3 never fires on CIF at all,
 * where the lookahead's ratio tops out near 70, and is measured and held
 * with its park_joy number. Numbers in docs/phase4-ladder.md. */
static int cb64_gate(void)
{
    static int v = -1;
    if (v < 0) {
        const char *s = n265_env("N265_CB64_GATE");
        v = s ? atoi(s) : 1;    /* the 60% improvement bound, which is
                                 * quality-free on the band (R3) */
    }
    return v;
}

/* AMVP 2Nx2N at the CB64 node (the quadtree's log2n == 6 arm set). It
 * was on from 2026-09-07 (-0.38 median BD-VMAF-NEG on the CIF band at
 * 1.06-1.09x) and is OFF by default since 2026-09-11 (phase 6b): on the
 * NEON binary it is the one arm on the whole ladder that reads free in
 * both modes - -0.01 mean BD over CQP and CRF, no clip past 0.2 - for
 * 1.053x on all three timed clips. `--preset slow` sets it back to 1.
 * Inert at `--ctu 32`, so the 1.053x is a medium-CTU number. */
static int cb64_amvp(void)
{
    static int on = -1;
    if (on < 0) {
        const char *v = n265_env("N265_CB64_AMVP");
        on = v ? atoi(v) != 0 : 0;
    }
    return on;
}

/* percent by which an intra CU must beat the best inter cost in a B
 * slice to be admitted; N265_B_INTRA_MARGIN=0 is the plain comparison.
 * 30 since 2026-09-07: -0.21 median BD-VMAF-NEG on the CIF band for no
 * measurable time. The optimum is broad - 15, 30 and 60 land within
 * 0.04 of each other - so this is a shape, not a tuned constant. */
static int b_intra_margin(void)
{
    static int mg = -1;
    if (mg < 0) {
        const char *v = n265_env("N265_B_INTRA_MARGIN");
        mg = v ? atoi(v) : 30;
        if (mg < 0)
            mg = 0;
    }
    return mg;
}

/* number of L0/L1 refinement sweeps. One sweep from 2026-09-07 (-0.29
 * mean BD-VMAF-NEG on the CIF band, 6/7 clips, bus flat, at 1.05x; a
 * second sweep buys another 0.02 mean for another 4%). ZERO since
 * 2026-09-11 (phase 6b): re-priced on the NEON binary the sweep is
 * 1.037x for +0.21 CQP / +0.30 CRF of band median, which medium at an
 * explicit budget no longer buys. `--preset slow` sets it back to 1. */
static int bi_refine_iters(void)
{
    static int it = -1;
    if (it < 0) {
        const char *v = n265_env("N265_BI_REFINE");
        it = v ? atoi(v) : 0;
        if (it > 4)
            it = 4;
    }
    return it;
}

/* N265_MERGE_RD_N (R5): how many of the merge list's five candidates get
 * a prediction and a cost in the prefilter that picks one for the RD
 * trial. The SIGNALLED list is untouched - MaxNumMergeCand stays 5 and
 * the merge_idx binarization with it - this is only how many are given
 * an MC and a SATD (or a SAD at the CU level) first. The list is derivation-ordered, so the ones dropped are the
 * temporal and combined candidates, not the spatial neighbours. */
static int merge_rd_n(void)
{
    static int v = -1;
    if (v < 0) {
        const char *s = n265_env("N265_MERGE_RD_N");
        v = s ? atoi(s) : MAX_MERGE;
        if (v < 1)
            v = 1;
        if (v > MAX_MERGE)
            v = MAX_MERGE;
    }
    return v;
}

/* N265_BI_GATE (R2): the refinement walks the L0/L1 pair downhill from a
 * starting point it does not choose - the two independent per-list
 * winners - and costs about eighteen SATDs against the one the plain
 * combined pair costs. Where that plain pair is already well behind the
 * better uni candidate, the walk is bought to lose: the square it steps
 * over is a quarter-pel wide and cannot close a gap the per-list
 * searches opened. Run it only where the plain pair is within this
 * margin of the better uni candidate. 0 is the pre-R2 encoder; 1, 2 and
 * 3 are margins of 0, 10 and 25 percent - the plain pair may be that far
 * BEHIND and still be refined - and 4 and 5 turn the margin the other
 * way and ask the plain pair to beat the better uni by 5 and 15 percent
 * first. 4 is the default: +0.10 mean BD for 1.041x foreman / 1.021x
 * mobile, and it improves the worst clip. The positive margins are worth
 * nothing (1 buys 1.025x foreman and 0.994x mobile) and 5 is priced out
 * at +0.24 for 1.061x. Numbers in docs/phase4-ladder.md. */
static int bi_gate(void)
{
    static int v = -1;
    if (v < 0) {
        const char *s = n265_env("N265_BI_GATE");
        v = s ? atoi(s) : 4;    /* the -5% margin, the one that priced */
    }
    return v;
}

static int bi_gate_margin(void)
{
    switch (bi_gate()) {
    case 2:  return 10;
    case 3:  return 25;
    case 4:  return -5;
    case 5:  return -15;
    default: return 0;
    }
}

/* L1 distance to the nearer of the two AMVP predictors: the historical
 * proxy's "one bit per quarter-pel" measure, and the mvp_lX_flag choice */
static int mvd_proxy(mv_t mv, const mv_t p[2])
{
    int c0 = abs(mv.x - p[0].x) + abs(mv.y - p[0].y);
    int c1 = abs(mv.x - p[1].x) + abs(mv.y - p[1].y);
    return c1 < c0 ? c1 : c0;
}

static int mvp_pick(mv_t mv, const mv_t p[2])
{
    return abs(mv.x - p[1].x) + abs(mv.y - p[1].y) <
           abs(mv.x - p[0].x) + abs(mv.y - p[0].y);
}

/* ref_idx_lX (truncated unary, two contexts then bypass) in Q8 bits */
static uint32_t ref_bits_q8(struct n265_frame_work *e, int list, int r)
{
    int cmax = e->nref[list] - 1;
    if (cmax < 1)
        return 0;
    cabac_ctx_t c0 = e->est_ctx[CTX_REF_IDX + 0];
    cabac_ctx_t c1 = e->est_ctx[CTX_REF_IDX + 1];
    uint32_t b = n265_est_bin(&c0, r > 0);
    if (r > 0 && cmax > 1)
        b += n265_est_bin(&c1, r > 1);
    if (r > 1 && cmax > 2)
        b += 256u * (uint32_t)((r < cmax ? r : cmax - 1) - 1);
    return b;
}

/* inter_pred_idc in Q8 bits (9.3.3.8: bin 0 on the CU depth context, the
 * uni-direction bin at index 4; an 8x4/4x8 PU codes only the latter) */
static uint32_t idc_bits_q8(struct n265_frame_work *e, int log2cb, int pw, int ph,
                            int pred)
{
    if (e->slice_type != 0)
        return 0;
    uint32_t b = 0;
    if (pw + ph != 12) {
        cabac_ctx_t c = e->est_ctx[CTX_INTER_IDC + (e->ctb_log2 - log2cb)];
        b += n265_est_bin(&c, pred == 3);
        if (pred == 3)
            return b;
    }
    cabac_ctx_t c4 = e->est_ctx[CTX_INTER_IDC + 4];
    b += n265_est_bin(&c4, pred >> 1);
    return b;
}

/* Motion syntax rate of one AMVP candidate, Q8 bits, for the SATD-domain
 * candidate ranking. With N265_MVD_BITS off this is the historical proxy
 * - one bit per quarter-pel of MVD distance plus two per reference index
 * plus `base` - written in Q8 so the call sites are shared; the two are
 * numerically identical there (a Q8 value scaled by lambda_sad >> 12 is
 * the old expression scaled >> 4). */
static uint32_t mot_rate_q8(struct n265_frame_work *e, const pu_geo *g,
                            const mot_t *m, mv_t mvp[2][2], int base)
{
    uint32_t b = 0;
    int real = mvd_bits_on();
    if (real)
        b += idc_bits_q8(e, 31 - __builtin_clz((unsigned)g->cbn), g->w, g->h,
                         m->pred);
    for (int l = 0; l < 2; l++) {
        if (!(m->pred & (1 << l)))
            continue;
        if (!real) {
            b += (uint32_t)(mvd_proxy(m->mv[l], mvp[l]) + 2 * m->ref[l] +
                            base) << 8;
            continue;
        }
        int k = mvp_pick(m->mv[l], mvp[l]);
        cabac_ctx_t g0 = e->est_ctx[CTX_MVD_GR0];
        cabac_ctx_t g1 = e->est_ctx[CTX_MVD_GR1];
        cabac_ctx_t mf = e->est_ctx[CTX_MVP_FLAG];
        b += mvd_cost_q8(&g0, &g1, m->mv[l].x - mvp[l][k].x,
                         m->mv[l].y - mvp[l][k].y);
        b += n265_est_bin(&mf, k);
        b += ref_bits_q8(e, l, m->ref[l]);
    }
    return b;
}

/* --- N265_SUBPEL_RATE (phase 10 item 1) --- */

/* The ladder's rate model is the CALLER's: `n265_me_search` is handed
 * this closure under N265_SUBPEL_RATE 2/3 so that a subpel square ranks
 * in the same units - the same mvd model, the same predictor pair, the
 * same lambda - the arbitration that will own the vector ranks in. The
 * scratch candidate's only moving part is the vector on `list`; the
 * reference-index and inter-direction terms it also prices are constant
 * across a square and cancel in the argmin, and are kept only so that
 * the number is the site's own `mot_rate_q8` and not a second model. */
struct me_rate_ctx {
    struct n265_frame_work *e;
    const pu_geo *g;
    mot_t u;
    mv_t up[2][2];
    int list;
    int base;
};

static uint32_t me_rate_bits(void *vctx, mv_t mv)
{
    struct me_rate_ctx *c = (struct me_rate_ctx *)vctx;
    c->u.mv[c->list] = mv;
    return mot_rate_q8(c->e, c->g, &c->u, c->up, c->base);
}

/* N265_SUBPEL_TRACE, the re-rank half: what arm 1 would cost, countable
 * with the arm OFF. `cu` is AMVP PUs whose vector reaches the re-rank
 * site, `lists` the per-list passes that site would make, `satd` the
 * luma SATDs it would buy (the centre plus the in-bounds neighbours),
 * `moved` how often it changes the vector when it is on. */
enum { RR_CU, RR_LISTS, RR_SATD, RR_MOVED, RR_N };
static atomic_ullong rr_n[RR_N];
static void rr_report(void);
static void rr_gate_report(void);
static int rr_trace(void)
{
    static int v = -1;
    if (v < 0) {
        v = n265_env("N265_SUBPEL_TRACE") != NULL;
        if (v) { atexit(rr_report); atexit(rr_gate_report); }
    }
    return v;
}
static void rr_report(void)
{
    unsigned long long cu = atomic_load(&rr_n[RR_CU]);
    double d = cu ? (double)cu : 1.0;
    fprintf(stderr, "subpeltrace rerank: amvp_pu=%llu lists=%llu satd=%llu"
            " moved=%llu | per_pu lists=%.3f satd=%.3f\n",
            cu, atomic_load(&rr_n[RR_LISTS]), atomic_load(&rr_n[RR_SATD]),
            atomic_load(&rr_n[RR_MOVED]),
            atomic_load(&rr_n[RR_LISTS]) / d, atomic_load(&rr_n[RR_SATD]) / d);
}
static inline void rr_bump(int k, unsigned long long v)
{
    atomic_fetch_add_explicit(&rr_n[k], v, memory_order_relaxed);
}

/* N265_SUBPEL_TRACE, the item-1b half: what the two cuts are worth,
 * countable in ONE run with the arm on and `N265_SUBPEL_GATE` at 0.
 * `free` is list passes whose five Hadamards are all already in
 * `n265_me_keep` - the re-rank costs nothing there and is never gated -
 * and `satd` is what the arm actually buys after the cache. The per-X
 * columns are the gate sweep: how many passes each candidate percentage
 * would remove, how many of those would have MOVED the vector (the
 * quality the gate spends), and the SATDs left after it. */
static const int rr_gx[4] = { 1, 2, 4, 8 };
enum { GN_LISTS, GN_FREE, GN_RAW, GN_PAID, GN_SPLIT, GN_PUREMOVED,
       GN_RATEMOVED,
       /* phase 11 item 2, the shadow cross-tab: uni passes the rung
        * could speak for, whether it fired, and whether the vector it
        * chose is the one the re-rank chose */
       GN_RUUNI, GN_RUFIRE, GN_RUSAME, GN_RUMOVED, GN_RUMSAME,
       /* the paid Hadamards split by uni/bi: a bi one is two MCs
        * and an average, so a count of SATDs is not a count of
        * work until it is split (item 1b sec 1b.1) */
       GN_PAIDUNI, GN_PAIDBI, GN_PASSUNI, GN_PASSBI, GN_N };
enum { RG_CUT, RG_CUTMOVED, RG_SATD, RG_ICUT, RG_ICUTMOVED,
       RG_ISATD, RG_N };
static atomic_ullong rr_gn[GN_N];
static atomic_ullong rr_g[4][RG_N];
static void rr_gate_report(void)
{
    unsigned long long li = atomic_load(&rr_gn[GN_LISTS]);
    double d = li ? (double)li : 1.0;
    unsigned long long s0 = atomic_load(&rr_gn[GN_RAW]);
    double ds = s0 ? (double)s0 : 1.0;
    unsigned long long mv = atomic_load(&rr_n[RR_MOVED]);
    double dm = mv ? (double)mv : 1.0;
    fprintf(stderr, "subpeltrace gate: lists=%llu free=%llu (%.1f%%)"
            " moved=%llu satd_ungated=%llu satd_cached=%llu (%.1f%%)\n",
            li, atomic_load(&rr_gn[GN_FREE]),
            100.0 * atomic_load(&rr_gn[GN_FREE]) / d, mv, s0,
            atomic_load(&rr_gn[GN_PAID]),
            100.0 * atomic_load(&rr_gn[GN_PAID]) / ds);
    {
        unsigned long long sp = atomic_load(&rr_gn[GN_SPLIT]);
        double dp = sp ? (double)sp : 1.0;
        fprintf(stderr, "subpeltrace split: passes=%llu pure_moved=%llu"
                " (%.1f%%) rate_moved=%llu (%.1f%%)\n", sp,
                atomic_load(&rr_gn[GN_PUREMOVED]),
                100.0 * atomic_load(&rr_gn[GN_PUREMOVED]) / dp,
                atomic_load(&rr_gn[GN_RATEMOVED]),
                100.0 * atomic_load(&rr_gn[GN_RATEMOVED]) / dp);
    }
    fprintf(stderr, "subpeltrace paid: uni_passes=%llu uni_satd=%llu"
            " bi_passes=%llu bi_satd=%llu (a bi Hadamard is two MCs)\n",
            atomic_load(&rr_gn[GN_PASSUNI]), atomic_load(&rr_gn[GN_PAIDUNI]),
            atomic_load(&rr_gn[GN_PASSBI]), atomic_load(&rr_gn[GN_PAIDBI]));
    if (n265_subme_rung()) {
        unsigned long long un = atomic_load(&rr_gn[GN_RUUNI]);
        unsigned long long fi = atomic_load(&rr_gn[GN_RUFIRE]);
        unsigned long long mo = atomic_load(&rr_gn[GN_RUMOVED]);
        double du = un ? (double)un : 1.0, dmo = mo ? (double)mo : 1.0;
        fprintf(stderr, "subpeltrace rung-vs-rerank: uni_passes=%llu"
                " rung_fired=%llu (%.1f%%) same_vector=%llu (%.1f%% of"
                " uni) rerank_moved=%llu same_on_moved=%llu (%.1f%% of"
                " moves)\n", un, fi, 100.0 * fi / du,
                atomic_load(&rr_gn[GN_RUSAME]),
                100.0 * atomic_load(&rr_gn[GN_RUSAME]) / du, mo,
                atomic_load(&rr_gn[GN_RUMSAME]),
                100.0 * atomic_load(&rr_gn[GN_RUMSAME]) / dmo);
    }
    for (int i = 0; i < 4; i++)
        for (int p = 0; p < 2; p++) {
            int c0 = p ? RG_ICUT : RG_CUT, c1 = p ? RG_ICUTMOVED : RG_CUTMOVED;
            int c2 = p ? RG_ISATD : RG_SATD;
            fprintf(stderr, "subpeltrace gate X=%s%d: cut=%llu (%.1f%% of"
                    " lists) cut_moved=%llu (%.1f%% of moves) satd=%llu"
                    " (%.1f%% of ungated)\n", p ? "-" : "+", rr_gx[i],
                    atomic_load(&rr_g[i][c0]),
                    100.0 * atomic_load(&rr_g[i][c0]) / d,
                    atomic_load(&rr_g[i][c1]),
                    100.0 * atomic_load(&rr_g[i][c1]) / dm,
                    atomic_load(&rr_g[i][c2]),
                    100.0 * atomic_load(&rr_g[i][c2]) / ds);
        }
}
static inline void gn_bump(int k, unsigned long long v)
{
    atomic_fetch_add_explicit(&rr_gn[k], v, memory_order_relaxed);
}

/* phase 11 item 2: has the rung already done this list pass's work? On
 * a UNI pass with no chroma term the rung ranked the same five positions
 * in the same model - luma SATD + the caller's lambda x mvd, whose only
 * difference from the re-rank's is a per-list constant that cancels in
 * the argmin - so the second pass buys the same answer twice, and where
 * the settled candidate is BI it buys it at two MCs a Hadamard. The
 * shadow census (`N265_SUBME_RUNG < 0`) is what says how often "the
 * same answer" is true rather than assumed. */
static int rung_owns(const struct n265_me_keep *k, int uni, int cpr)
{
    return n265_subme_rung() > 0 && n265_rung_rerank() == 0 &&
           uni && !cpr && k && k->rung_fired;
}

/* SATD of a bi prediction whose fixed list is already interpolated into
 * fix14: only the moving list pays an MC.
 *
 * The average is `a[i] + b[i]` (8.5.3.3.4.2, and the NEON kernel adds the
 * same way), so which of the two int16 buffers arrives as `a` cannot
 * change a sample: this is `mot_satd_wh` of the same `mot_t` for half the
 * interpolation, not an approximation of it. Phase 11 item 4 leans on
 * exactly that. */
static uint32_t bi_satd_fixed(struct n265_frame_work *e, int x0, int y0, int w,
                              int h, const int16_t *fix14, int list, int ref,
                              mv_t mv)
{
    int16_t a[64 * 64];
    pixel pred[64 * 64];
    mc_pred14(e, list, ref, 0, x0, y0, w, h, mv, a);
    n265_bi_avg_wh(pred, w, a, fix14, w, h);
    return n265_satd_wh(e->pad_plane[0] + y0 * e->pad_stride[0] + x0,
                        e->pad_stride[0], pred, w, w, h);
}

/* Arm 1's re-rank of one list's vector against its four quarter-pel
 * neighbours (phase 10 item 1, made affordable in item 1b). Both sites
 * that settle an AMVP vector call this, so the cache, the gate and the
 * census are one piece of code and cannot drift apart.
 *
 * `cen` is the centre's cost in the site's units, or UINT32_MAX to have
 * it built here. `keep` is what the ladder handed back on list `l`; its
 * SQUARE MARGIN is what the gate reads and is meaningful either way,
 * while its SATDs may be read as values only when `uni` - the settled
 * vector is still the uni winner the ladder returned, so `mot_satd_wh`
 * of this `mot_t` and `mc_satd` of that vector are the same number.
 *
 * A gated-off pass costs four clamps and nothing else - no Hadamard, no
 * chroma MC, not even the mvd costs - which is what makes the gate worth
 * having where the cache has not already made the pass free. */
static mv_t subpel_rerank(struct n265_frame_work *e, const pu_geo *g,
                          const mot_t *amot, mv_t mvp[2][2], int l, int lam4,
                          int cpr, const struct n265_me_keep *keep, int uni,
                          uint32_t cen, uint32_t *cost)
{
    const struct n265_me_keep *kv = uni ? keep : NULL;
    static const int st[4][2] = { { 1, 0 }, { -1, 0 }, { 0, 1 }, { 0, -1 } };
    int x0 = g->x, y0 = g->y, w = g->w, h = g->h;
    mot_t t[4];
    int ok[4];
    for (int i = 0; i < 4; i++) {
        mv_t v = { (int16_t)(amot->mv[l].x + st[i][0]),
                   (int16_t)(amot->mv[l].y + st[i][1]) };
        mv_t vc = v;
        n265_clamp_mv(e, x0, y0, w, h, &vc);
        ok[i] = vc.x == v.x && vc.y == v.y;
        if (!ok[i])
            continue;
        t[i] = *amot;
        t[i].mv[l] = v;
    }

    /* the SATDs this pass still has to buy, after the cache */
    int cached_c = kv && kv->satd != UINT32_MAX;
    int buy = cached_c ? 0 : 1;
    for (int i = 0; i < 4; i++)
        if (ok[i] && !(kv && kv->nb_valid && kv->nb[i] != UINT32_MAX))
            buy++;
    int free_pass = buy == 0;

    /* The gate: the ladder's last square's own flatness. A pass with no
     * margin to report, and a pass that costs nothing, are never cut.
     * `sep` is "the square SEPARATED its top two by more than |X|%"; a
     * positive knob cuts those (the CPRED_GATE shape, refused) and a
     * negative one cuts the rest. */
    int marg = keep && keep->sq_valid && !free_pass;
    int gate = n265_subpel_gate();
    int ax = gate < 0 ? -gate : gate;
    int sep = (uint64_t)(marg ? keep->sq_second : 0) * 100 >
              (uint64_t)(marg ? keep->sq_best : 0) * (uint64_t)(100 + ax);
    int cut = marg && (gate > 0 ? sep : !sep);
    if (rr_trace()) {
        gn_bump(GN_LISTS, 1);
        gn_bump(GN_RAW, (unsigned long long)
                (1 + !!ok[0] + !!ok[1] + !!ok[2] + !!ok[3]));
        if (free_pass)
            gn_bump(GN_FREE, 1);
        if (!(gate && cut)) {
            gn_bump(GN_PAID, (unsigned long long)buy);
            gn_bump(uni ? GN_PAIDUNI : GN_PAIDBI, (unsigned long long)buy);
            gn_bump(uni ? GN_PASSUNI : GN_PASSBI, 1);
        }
    }

    *cost = cen;
    if (gate && cut)
        /* the centre goes uncosted too. The vector does not move, and
         * the cost after an unmoved vector is the one the caller came in
         * with - the only one it is allowed to read. */
        return amot->mv[l];
    /* `pc` is the centre's cost WITHOUT the rate term, which the census
     * needs to say how much of this arm is the rate arbitration it was
     * built to be and how much is quarter-pel refinement the N265_SUBME=3
     * ladder skipped. Known only where this function built the centre. */
    uint32_t pc = UINT32_MAX;

    /* phase 11 item 4 arm (c): the square's FIXED list does not move.
     * Where the settled candidate is bi, all five positions predict from
     * the same interpolation of list !l and differ only in list l's MC
     * and the average, so `mot_satd_wh`'s second `mc_pred14` is the same
     * filter over the same samples five times. Buy it once.
     *
     * This is the shape `bi_satd_fixed` already had for N265_BI_REFINE
     * and which nothing on the re-rank path used (docs/phase11.md sec
     * 2.9). It is ARITHMETICALLY the same number, not a cheaper metric:
     * the item's claim is a cmp, not a band.
     *
     * `kv` is NULL on a bi pass, so the ladder's cache never supplies one
     * of these five and the saving is the whole four MCs. */
    int16_t fix[64 * 64];
    int fixl = amot->pred == 3 &&
        (cen == UINT32_MAX || ok[0] || ok[1] || ok[2] || ok[3]) ? !l : -1;
    if (fixl >= 0)
        mc_pred14(e, fixl, amot->ref[fixl], 0, x0, y0, w, h,
                  amot->mv[fixl], fix);

    if (cen == UINT32_MAX) {
        pc = (cached_c ? kv->satd
              : fixl >= 0 ? bi_satd_fixed(e, x0, y0, w, h, fix, l,
                                          amot->ref[l], amot->mv[l])
              : mot_satd_wh(e, x0, y0, w, h, *amot)) +
            (cpr ? cpred_satd(e, x0, y0, w, h, *amot) : 0);
        cen = pc + (uint32_t)(((uint64_t)mot_rate_q8(e, g, amot, mvp, 6)
                               * lam4) >> 12);
    }
    *cost = cen;

    uint32_t bq = cen, pbq = pc;
    mv_t bmv = amot->mv[l], pbmv = amot->mv[l];
    for (int i = 0; i < 4; i++) {
        if (!ok[i])
            continue;
        uint32_t s = (kv && kv->nb_valid && kv->nb[i] != UINT32_MAX)
            ? kv->nb[i]
            : fixl >= 0 ? bi_satd_fixed(e, x0, y0, w, h, fix, l,
                                        t[i].ref[l], t[i].mv[l])
            : mot_satd_wh(e, x0, y0, w, h, t[i]);
        uint32_t sp = s + (cpr ? cpred_satd(e, x0, y0, w, h, t[i]) : 0);
        uint32_t c = sp +
            (uint32_t)(((uint64_t)mot_rate_q8(e, g, &t[i], mvp, 6) * lam4)
                       >> 12);
        if (c < bq) {
            bq = c;
            bmv = t[i].mv[l];
        }
        if (pc != UINT32_MAX && sp < pbq) {
            pbq = sp;
            pbmv = t[i].mv[l];
        }
    }
    if (rr_trace()) {
        int moved = bmv.x != amot->mv[l].x || bmv.y != amot->mv[l].y;
        if (moved)
            rr_bump(RR_MOVED, 1);
        /* phase 11 item 2: on a uni pass the rung ranked the same five
         * positions in the same units one call earlier. Under
         * N265_SUBME_RUNG < 0 it did so without taking its answer, so
         * this counts how much of the shipped arm a rung would carry. */
        if (uni && keep) {
            gn_bump(GN_RUUNI, 1);
            if (keep->rung_fired)
                gn_bump(GN_RUFIRE, 1);
            int same = keep->rung_fired &&
                keep->rung_mv.x == bmv.x && keep->rung_mv.y == bmv.y;
            if (same)
                gn_bump(GN_RUSAME, 1);
            if (moved) {
                gn_bump(GN_RUMOVED, 1);
                if (same)
                    gn_bump(GN_RUMSAME, 1);
            }
        }
        if (pc != UINT32_MAX) {
            gn_bump(GN_SPLIT, 1);
            if (pbmv.x != amot->mv[l].x || pbmv.y != amot->mv[l].y)
                gn_bump(GN_PUREMOVED, 1);
            if (bmv.x != pbmv.x || bmv.y != pbmv.y)
                gn_bump(GN_RATEMOVED, 1);
        }
        for (int i = 0; i < 4; i++) {
            /* both polarities of the same margin, in one run */
            int si = marg && (uint64_t)keep->sq_second * 100 >
                             (uint64_t)keep->sq_best *
                             (uint64_t)(100 + rr_gx[i]);
            int pol[2] = { marg && si, marg && !si };   /* +X cut, -X cut */
            int col[2][3] = { { RG_CUT, RG_CUTMOVED, RG_SATD },
                              { RG_ICUT, RG_ICUTMOVED, RG_ISATD } };
            for (int p = 0; p < 2; p++) {
                if (pol[p]) {
                    atomic_fetch_add_explicit(&rr_g[i][col[p][0]], 1,
                                              memory_order_relaxed);
                    if (moved)
                        atomic_fetch_add_explicit(&rr_g[i][col[p][1]], 1,
                                                  memory_order_relaxed);
                } else {
                    atomic_fetch_add_explicit(&rr_g[i][col[p][2]],
                                              (unsigned long long)buy,
                                              memory_order_relaxed);
                }
            }
        }
    }
    *cost = bq;
    return bmv;
}

/* Bi-prediction refinement (N265_BI_REFINE). Pairing the two independent
 * per-list winners optimises each half against the SOURCE; the average
 * they form is never searched. Holding one list fixed makes the bi
 * prediction an affine function of the other, so the pair walks downhill
 * on the combined SATD under a small quarter-pel square pattern, one
 * list at a time. A zero-MV bi probe goes first: it is the one pair the
 * per-list winners cannot produce once either list has drifted, and it
 * costs one SATD.
 *
 * `bi` and `mvp` are updated in place; the caller keeps its own copies
 * until it knows the bi candidate won, because a probe that changes a
 * reference index changes the predictor pair with it and a stale mvp
 * would make the decoder reconstruct a different vector. */
static uint32_t bi_refine(struct n265_frame_work *e, const pu_geo *g, mot_t *bi,
                          mv_t mvp[2][2], int lam4, int base,
                          uint32_t uni_ref)
{
    int w = g->w, h = g->h, x0 = g->x, y0 = g->y;
    int16_t fix[64 * 64];
    uint32_t best = mot_satd_wh(e, x0, y0, w, h, *bi) +
        (uint32_t)(((uint64_t)mot_rate_q8(e, g, bi, mvp, base) * lam4) >> 12);

    /* R2: the plain pair is already this far behind the better uni
     * candidate, in the units the caller will arbitrate in. The zero
     * probe and the quarter-pel square cannot close that, so stop here
     * and hand back the cost the caller would have paid anyway. */
    if (uni_ref != UINT32_MAX &&
        (int64_t)best * 100 > (int64_t)uni_ref * (100 + bi_gate_margin()))
        return best;

    if (bi->mv[0].x || bi->mv[0].y || bi->mv[1].x || bi->mv[1].y ||
        bi->ref[0] || bi->ref[1]) {
        mot_t z = { { { 0, 0 }, { 0, 0 } }, 3, { 0, 0 } };
        mv_t zp[2][2];
        n265_amvp_pu(e, 0, 0, g, zp[0]);
        n265_amvp_pu(e, 1, 0, g, zp[1]);
        uint32_t c = mot_satd_wh(e, x0, y0, w, h, z) +
            (uint32_t)(((uint64_t)mot_rate_q8(e, g, &z, zp, base) * lam4) >> 12);
        if (c < best) {
            best = c;
            *bi = z;
            memcpy(mvp, zp, sizeof(zp));
        }
    }

    int iters = bi_refine_iters();
    for (int it = 0; it < iters; it++) {
        int moved = 0;
        for (int l = 0; l < 2; l++) {
            int o = !l;
            mc_pred14(e, o, bi->ref[o], 0, x0, y0, w, h, bi->mv[o], fix);
            mot_t t = *bi;
            mv_t bmv = bi->mv[l];
            for (int dy = -1; dy <= 1; dy++)
                for (int dx = -1; dx <= 1; dx++) {
                    if (!dx && !dy)
                        continue;
                    mv_t v = { (int16_t)(bi->mv[l].x + dx),
                               (int16_t)(bi->mv[l].y + dy) };
                    mv_t vc = v;
                    n265_clamp_mv(e, x0, y0, w, h, &vc);
                    if (vc.x != v.x || vc.y != v.y)
                        continue;
                    t.mv[l] = v;
                    uint32_t c = bi_satd_fixed(e, x0, y0, w, h, fix, l,
                                               bi->ref[l], v) +
                        (uint32_t)(((uint64_t)mot_rate_q8(e, g, &t, mvp, base)
                                    * lam4) >> 12);
                    if (c < best) {
                        best = c;
                        bmv = v;
                    }
                }
            if (bmv.x != bi->mv[l].x || bmv.y != bi->mv[l].y) {
                bi->mv[l] = bmv;
                moved = 1;
            }
        }
        if (!moved)
            break;
    }
    return best;
}

/* residual + cbf bits of a flat (TU == CU) transform set */
static void est_tt_bits(struct n265_frame_work *e, est_state *st, int log2n,
                        int mode, const tb_ctx *tl, const tb_ctx *tcb,
                        const tb_ctx *tcr)
{
    res_ops ops;
    n265_est_ops(&ops, st);
    int inter = mode == MODE_INTER;
    st->bits += n265_est_bin(&st->ctx[CTX_TSPLIT + 5 - log2n], 0);
    est_cbf_c(e, st, 0, tcb);
    est_cbf_c(e, st, 0, tcr);
    if (!(inter && !tb_any(tcb) && !tb_any(tcr)))
        st->bits += n265_est_bin(&st->ctx[CTX_CBF_LUMA + 1], tl->nz != 0);
    int log2c = log2n - e->csx;
    int scan_l = inter ? 0 : n265_scan_idx(mode, log2n, 0);
    int scan_c = c_scan_idx(e, mode, log2c);
    if (tl->nz)
        n265_residual_coding(&ops, tl->level, log2n, 0, scan_l, !e->lossless);
    est_res_c(e, &ops, tcb, log2c, 1, scan_c);
    est_res_c(e, &ops, tcr, log2c, 2, scan_c);
}

/* choose flat vs 4-TU split for an inter winner and commit records +
 * recon accordingly. Inter prediction is neighbor-free, so both
 * arrangements evaluate order-free. CB8 splits keep chroma at the
 * parent. */
static void commit_inter_tt_inner(struct n265_frame_work *e, mot_t mot,
                                  int x0, int y0, int log2n, tb_ctx *tl,
                                  tb_ctx *tcb, tb_ctx *tcr)
{
    int n = 1 << log2n;
    int qp_c = n265_chroma_qp(e->cur_ctu_qp, e->chroma_idc);
    uint32_t lam = ctu_lam(e, e->cur_ctu_qp);
    int h = n >> 1;
    int csx = e->csx, csy = e->csy;
    int cx0 = x0 >> csx, cy0 = y0 >> csy;
    int log2c = log2n - csx, log2cc = log2n - 1 - csx;
    /* see tt_analyze: the chroma stays at the 8x8 node, as one 4x4 block
     * at 4:2:0 and as 7.3.8.8's stacked pair at 4:2:2 */
    int child_chroma = log2cc >= 2;
    tb_ctx *s_l = e->sc->ctt_l;
    tb_ctx (*s_c)[2] = e->sc->ctt_c;

    /* split arrangement */
    uint64_t d_split = 0;
    int root_cb = 0, root_cr = 0;
    for (int i = 0; i < 4; i++) {
        int x1 = x0 + (i & 1) * h, y1 = y0 + (i >> 1) * h;
        tb_analyze(e, 0, x1, y1, log2n - 1, e->cur_ctu_qp, MODE_INTER, mot,
                   &s_l[i]);
        tb_make_recon(e, 0, log2n - 1, e->cur_ctu_qp, MODE_INTER, &s_l[i]);
        d_split += tb_ssd(e, 0, x1, y1, log2n - 1, &s_l[i]);
        if (child_chroma) {
            int kx = x1 >> csx, ky = y1 >> csy;
            tb_analyze(e, 1, kx, ky, log2cc, qp_c, MODE_INTER, mot,
                       &s_c[i][0]);
            tb_analyze(e, 2, kx, ky, log2cc, qp_c, MODE_INTER, mot,
                       &s_c[i][1]);
            tb_make_recon(e, 1, log2cc, qp_c, MODE_INTER, &s_c[i][0]);
            tb_make_recon(e, 2, log2cc, qp_c, MODE_INTER, &s_c[i][1]);
            d_split += tb_ssd(e, 1, kx, ky, log2cc, &s_c[i][0]);
            d_split += tb_ssd(e, 2, kx, ky, log2cc, &s_c[i][1]);
            root_cb |= tb_any(&s_c[i][0]);
            root_cr |= tb_any(&s_c[i][1]);
        }
    }
    if (!child_chroma) {
        root_cb = tb_any(tcb);
        root_cr = tb_any(tcr);
        d_split += tb_ssd(e, 1, cx0, cy0, log2c, tcb);
        d_split += tb_ssd(e, 2, cx0, cy0, log2c, tcr);
    }

    /* past maxTB the split is inferred and the flat arrangement does
     * not exist (CB64: forced 4x TU32) */
    int flat_ok = log2n <= 5;
    uint64_t d_flat = !flat_ok ? 0 :
        tb_ssd(e, 0, x0, y0, log2n, tl) +
        tb_ssd(e, 1, cx0, cy0, log2c, tcb) +
        tb_ssd(e, 2, cx0, cy0, log2c, tcr);

    /* bits, both arrangements, on estimator copies */
    cabac_ctx_t local[CTX_COUNT];
    est_state st;
    res_ops ops;

    uint64_t j_flat = UINT64_MAX;
    if (flat_ok) {
        memcpy(local, e->est_ctx, sizeof(local));
        st.ctx = local; st.bits = 0;
        n265_est_ops(&ops, &st);
        est_tt_bits(e, &st, log2n, MODE_INTER, tl, tcb, tcr);
        j_flat = rd_j(e, d_flat, st.bits, lam);
    }

    memcpy(local, e->est_ctx, sizeof(local));
    st.ctx = local; st.bits = 0;
    n265_est_ops(&ops, &st);
    if (flat_ok)
        st.bits += n265_est_bin(&local[CTX_TSPLIT + 5 - log2n], 1);
    if (child_chroma) {
        st.bits += n265_est_bin(&local[CTX_CBF_CHROMA + 0], root_cb);
        st.bits += n265_est_bin(&local[CTX_CBF_CHROMA + 0], root_cr);
    } else {
        est_cbf_c(e, &st, 0, tcb);
        est_cbf_c(e, &st, 0, tcr);
    }
    for (int i = 0; i < 4; i++) {
        if (child_chroma) {
            if (root_cb)
                est_cbf_c(e, &st, 1, &s_c[i][0]);
            if (root_cr)
                est_cbf_c(e, &st, 1, &s_c[i][1]);
        }
        st.bits += n265_est_bin(&local[CTX_CBF_LUMA + 0], s_l[i].nz != 0);
        if (s_l[i].nz)
            n265_residual_coding(&ops, s_l[i].level, log2n - 1, 0, 0,
                                 !e->lossless);
        if (child_chroma) {
            est_res_c(e, &ops, &s_c[i][0], log2cc, 1, 0);
            est_res_c(e, &ops, &s_c[i][1], log2cc, 2, 0);
        } else if (i == 3) {
            est_res_c(e, &ops, tcb, log2c, 1, 0);
            est_res_c(e, &ops, tcr, log2c, 2, 0);
        }
    }
    uint64_t j_split = rd_j(e, d_split, st.bits, lam);

    if (j_flat <= j_split) {
        rec_u8(e, 0);
        rec_cbf_c(e, tcb);
        rec_cbf_c(e, tcr);
        rec_u8(e, tl->nz != 0);
        tb_commit(e, 0, x0, y0, log2n, tl);
        if (tl->nz)
            rec_levels(e, tl->level, n * n);
        tb_commit(e, 1, cx0, cy0, log2c, tcb);
        rec_levels_c(e, tcb, log2c);
        tb_commit(e, 2, cx0, cy0, log2c, tcr);
        rec_levels_c(e, tcr, log2c);
        return;
    }

    /* commit split: record layout mirrors replay_tt (which reads no
     * split byte where the split is inferred past maxTB) */
    if (flat_ok)
        rec_u8(e, 1);
    if (child_chroma) {
        rec_u8(e, (unsigned)root_cb);
        rec_u8(e, (unsigned)root_cr);
    } else {
        rec_cbf_c(e, tcb);
        rec_cbf_c(e, tcr);
    }
    for (int i = 0; i < 4; i++) {
        int x1 = x0 + (i & 1) * h, y1 = y0 + (i >> 1) * h;
        mark_cu_edges(e, x1, y1, h);
        if (child_chroma) {
            rec_cbf_c(e, &s_c[i][0]);
            rec_cbf_c(e, &s_c[i][1]);
        }
        rec_u8(e, s_l[i].nz != 0);
        tb_commit(e, 0, x1, y1, log2n - 1, &s_l[i]);
        if (s_l[i].nz)
            rec_levels(e, s_l[i].level, h * h);
        if (child_chroma) {
            int kx = x1 >> csx, ky = y1 >> csy;
            tb_commit(e, 1, kx, ky, log2cc, &s_c[i][0]);
            rec_levels_c(e, &s_c[i][0], log2cc);
            tb_commit(e, 2, kx, ky, log2cc, &s_c[i][1]);
            rec_levels_c(e, &s_c[i][1], log2cc);
        } else if (i == 3) {
            tb_commit(e, 1, cx0, cy0, log2c, tcb);
            rec_levels_c(e, tcb, log2c);
            tb_commit(e, 2, cx0, cy0, log2c, tcr);
            rec_levels_c(e, tcr, log2c);
        }
    }
}

/* The split arrangement is analysed inside the commit, so under
 * N265_RDOQ=2 its four children are lattice work; the flat arm's TBs were
 * re-quantised by commit_inter_cu before it got here, so the two
 * arrangements are compared on the same quantiser and the winner is the
 * one that is coded. */
static void commit_inter_tt(struct n265_frame_work *e, mot_t mot, int x0,
                            int y0, int log2n, tb_ctx *tl, tb_ctx *tcb,
                            tb_ctx *tcr)
{
    int sv = e->rdoq_commit;
    e->rdoq_commit = 1;
    commit_inter_tt_inner(e, mot, x0, y0, log2n, tl, tcb, tcr);
    e->rdoq_commit = sv;
}

/* the three most probable modes for this position (9.3.3.6), the list the
 * mode's own syntax cost is built on */
static void mpm_cands(struct n265_frame_work *e, int x0, int y0, int cand[3])
{
    int a = INTRA_DC, b = INTRA_DC;
    if (x0 > 0 && e->blk_intra[(y0 >> 2) * e->map_stride + ((x0 - 1) >> 2)])
        a = e->intra_mode[(y0 >> 2) * e->map_stride + ((x0 - 1) >> 2)];
    if (y0 > 0 && (y0 & ((1 << e->ctb_log2) - 1)) != 0 &&
        e->blk_intra[((y0 - 1) >> 2) * e->map_stride + (x0 >> 2)])
        b = e->intra_mode[((y0 - 1) >> 2) * e->map_stride + (x0 >> 2)];
    if (a == b) {
        if (a < 2) { cand[0] = 0; cand[1] = 1; cand[2] = 26; }
        else { cand[0] = a; cand[1] = 2 + ((a + 29) % 32);
               cand[2] = 2 + ((a - 2 + 1) % 32); }
    } else {
        cand[0] = a; cand[1] = b;
        cand[2] = (a != 0 && b != 0) ? 0 : (a != 1 && b != 1) ? 1 : 26;
    }
}

/* shortlist width for the intra RD stage (N265_INTRA_RD_N, 3 by default)
 * and whether the MPMs join it (N265_INTRA_MPM) */
static int intra_rd_n(void)
{
    static int v = -1;
    if (v < 0) {
        const char *s = n265_env("N265_INTRA_RD_N");
        v = s ? atoi(s) : 3;
        if (v < 1) v = 1;
        if (v > INTRA_RD_MAX) v = INTRA_RD_MAX;
    }
    return v;
}

static int intra_rd_mpm(void)
{
    static int v = -1;
    if (v < 0) { const char *s = n265_env("N265_INTRA_MPM"); v = s ? atoi(s) : 0; }
    return v;
}

/* N265_I_INTRA (phase 4, rung R11): the intra economy inside I slices
 * only, where there is no inter arm to gate against and every CU pays
 * the full search. 1 and 2 narrow the RD shortlist to two modes and to
 * one; 3 ranks the four 4x4 NxN luma blocks over the 8x8 parent's own
 * neighbourhood instead of all 35 modes; 4 is 2 and 3 together. 0 is
 * the default: 3 is quality-free but the clean timing pass reads it at
 * 1.005x foreman against a byte-identical base, so there is nothing to
 * buy at this keyint (docs/phase4-ladder.md R11). */
static int i_intra(void)
{
    static int v = -1;
    if (v < 0) {
        const char *s = n265_env("N265_I_INTRA");
        v = s ? atoi(s) : 0;
    }
    return v;
}

/* the modes a 4x4 RMD looks at when N265_I_INTRA >= 3: the parent 8x8's
 * own winner and its two angular neighbours, planar and DC, and the two
 * most probable modes, which cost two bins where a remainder costs six */
static int intra_sub_cands(struct n265_frame_work *e, int x0, int y0,
                           int parent, int out[8])
{
    int n = 0;
    out[n++] = 0;
    out[n++] = 1;
    if (parent >= 2) {
        out[n++] = parent;
        out[n++] = 2 + ((parent - 2 + 31) % 32);
        out[n++] = 2 + ((parent - 2 + 1) % 32);
    }
    int mpm[3];
    mpm_cands(e, x0, y0, mpm);
    for (int i = 0; i < 3 && n < 8; i++) {
        int dup = 0;
        for (int q = 0; q < n; q++)
            if (out[q] == mpm[i])
                dup = 1;
        if (!dup)
            out[n++] = mpm[i];
    }
    return n;
}

/* RD-refine the intra luma mode over the SATD-ranked shortlist: only
 * mode-dependent bits (MPM, cbfs, residuals) enter the compare */
/* out_l/out_cb/out_cr receive the winning mode's flat-TB analysis, which
 * tt_analyze commits instead of computing again. */
static int intra_rd_pick(struct n265_frame_work *e, int x0, int y0, int log2n,
                         tb_ctx **out_l, tb_ctx **out_cb, tb_ctx **out_cr)
{
    int cands[INTRA_RD_MAX + 3];
    int nk = intra_rd_n();
    if (e->slice_type == 2 && (i_intra() == 1 || i_intra() == 2))
        nk = 3 - i_intra();     /* 1 -> two modes, 2 -> the rank's winner */
    if (e->slice_type == 2 && i_intra() == 4)
        nk = 1;
    int ncand = nk;
    search_luma_mode_k(e, x0, y0, log2n, e->recon[0], e->recon_stride[0],
                       NULL, cands, nk);
    if (intra_rd_mpm()) {
        /* an MPM costs two bits where a remainder costs six, so a mode the
         * SATD prefilter ranked just outside can still win on J */
        int mpm[3];
        mpm_cands(e, x0, y0, mpm);
        for (int i = 0; i < 3; i++) {
            int dup = 0;
            for (int q = 0; q < ncand; q++)
                if (cands[q] == mpm[i])
                    dup = 1;
            if (!dup)
                cands[ncand++] = mpm[i];
        }
    }
    int qp_c = n265_chroma_qp(e->cur_ctu_qp, e->chroma_idc);
    int cx0 = x0 >> e->csx, cy0 = y0 >> e->csy, log2c = log2n - e->csx;
    uint32_t lam = ctu_lam(e, e->cur_ctu_qp);
    tb_ctx *tl = &e->sc->ird_l, *tcb = &e->sc->ird_cb, *tcr = &e->sc->ird_cr;
    tb_ctx *wl = &e->sc->ird_bl, *wcb = &e->sc->ird_bcb,
           *wcr = &e->sc->ird_bcr;
    uint64_t best_j = UINT64_MAX;
    int best = cands[0];
    for (int k = 0; k < ncand; k++) {
        int mode = cands[k];
        int dup = 0;
        for (int q = 0; q < k; q++)
            if (cands[q] == mode)
                dup = 1;
        if (dup)
            continue;
        tb_analyze(e, 1, cx0, cy0, log2c, qp_c, mode, mot_zero, tcb);
        tb_analyze(e, 2, cx0, cy0, log2c, qp_c, mode, mot_zero, tcr);
        tb_analyze(e, 0, x0, y0, log2n, e->cur_ctu_qp, mode, mot_zero, tl);
        tb_make_recon(e, 0, log2n, e->cur_ctu_qp, mode, tl);
        tb_make_recon(e, 1, log2c, qp_c, mode, tcb);
        tb_make_recon(e, 2, log2c, qp_c, mode, tcr);
        uint64_t d = tb_ssd(e, 0, x0, y0, log2n, tl) +
            tb_ssd(e, 1, cx0, cy0, log2c, tcb) +
            tb_ssd(e, 2, cx0, cy0, log2c, tcr);
        cabac_ctx_t local[CTX_COUNT];
        memcpy(local, e->est_ctx, sizeof(local));
        est_state st = { local, 0 };
        res_ops ops;
        n265_est_ops(&ops, &st);
        est_mpm_bits(e, &st, x0, y0, mode);
        est_cbf_c(e, &st, 0, tcb);
        est_cbf_c(e, &st, 0, tcr);
        st.bits += n265_est_bin(&local[CTX_CBF_LUMA + 1], tl->nz != 0);
        if (tl->nz)
            n265_residual_coding(&ops, tl->level, log2n, 0,
                                 n265_scan_idx(mode, log2n, 0), !e->lossless);
        int sc = c_scan_idx(e, mode, log2c);
        est_res_c(e, &ops, tcb, log2c, 1, sc);
        est_res_c(e, &ops, tcr, log2c, 2, sc);
        uint64_t j = rd_j(e, d, st.bits, lam);
        if (j < best_j) {
            best_j = j;
            best = mode;
            /* swap rather than copy: the loser's buffer becomes the next
             * candidate's scratch */
            tb_ctx *t;
            t = wl; wl = tl; tl = t;
            t = wcb; wcb = tcb; tcb = t;
            t = wcr; wcr = tcr; tcr = t;
        }
    }
    *out_l = wl; *out_cb = wcb; *out_cr = wcr;
    return best;
}

static uint64_t cu_area_ssd(struct n265_frame_work *e, int x0, int y0, int n)
{
    uint64_t ssd = 0;
    if (e->psy > 0.0)       /* the same psy term the TB candidates carry */
        ssd += n265_psy_cost(e->pad_plane[0] + y0 * e->pad_stride[0] + x0, e->pad_stride[0],
                             e->recon[0] + y0 * e->recon_stride[0] + x0, e->recon_stride[0],
                             n, ctu_lam(e, e->cur_ctu_qp), e->psy);
    for (int c = 0; c < 3; c++) {
        int shx = c ? e->csx : 0, shy = c ? e->csy : 0;
        const pixel *src = e->pad_plane[c];
        const pixel *rec = e->recon[c];
        uint64_t s = n265_ssd_wh(src + (size_t)(y0 >> shy) * e->pad_stride[c] +
                                     (x0 >> shx), e->pad_stride[c],
                                 rec + (size_t)(y0 >> shy) * e->recon_stride[c] +
                                     (x0 >> shx), e->recon_stride[c],
                                 n >> shx, n >> shy);
        if (c && chroma_w_q8(e) != 256)
            s = (s * chroma_w_q8(e)) >> 8;
        ssd += s;
    }
    return ssd;
}

/* qstep^2 / 8: the per-pixel MSE below which a skip is "clean" and the
 * split trial cannot plausibly pay for its syntax */
/* An SSD in native sample units against the square of the quantiser
 * step, so the step is taken at Qp' = qp + QP_BD_OFFSET: at Main10 that
 * is four times the 8-bit step, its square is sixteen times, and the SSD
 * it is compared against is sixteen times too. */
static uint64_t skip_clean_thr(int qp, int n)
{
    double qstep = pow(2.0, (qp + QP_BD_OFFSET - 4) / 6.0);
    return (uint64_t)((double)n * n * qstep * qstep / 4.0 *
                      es_thr_pct() / 100.0);
}

/* --- rectangular and asymmetric inter partitions --- */


/* the CU's motion maps across a partition trial. The first PU's motion
 * has to be visible to the second PU's AMVP derivation - 6.4.2 makes a
 * neighbour inside the same coding block available, and A1 of a vertical
 * split (B1 of a horizontal one) is exactly that - so every trial writes
 * the maps and then puts them back. */
typedef struct mot_snap {
    int16_t mv[16 * 16 * 4];
    pixel pred[16 * 16];
    int8_t ref[16 * 16 * 2];
    pixel intra[16 * 16];
} mot_snap;

static void mot_save(struct n265_frame_work *e, int x0, int y0, int n,
                     mot_snap *sn)
{
    int cells = n >> 2;
    for (int y = 0; y < cells; y++) {
        int m = ((y0 >> 2) + y) * e->map_stride + (x0 >> 2);
        memcpy(sn->mv + y * cells * 4, e->blk_mv + 4 * m,
               (size_t)cells * 4 * sizeof(int16_t));
        memcpy(sn->pred + y * cells, e->blk_pred + m, (size_t)cells);
        memcpy(sn->ref + y * cells * 2, e->blk_ref + 2 * m,
               (size_t)cells * 2);
        memcpy(sn->intra + y * cells, e->blk_intra + m, (size_t)cells);
    }
}

static void mot_load(struct n265_frame_work *e, int x0, int y0, int n,
                     const mot_snap *sn)
{
    int cells = n >> 2;
    for (int y = 0; y < cells; y++) {
        int m = ((y0 >> 2) + y) * e->map_stride + (x0 >> 2);
        memcpy(e->blk_mv + 4 * m, sn->mv + y * cells * 4,
               (size_t)cells * 4 * sizeof(int16_t));
        memcpy(e->blk_pred + m, sn->pred + y * cells, (size_t)cells);
        memcpy(e->blk_ref + 2 * m, sn->ref + y * cells * 2,
               (size_t)cells * 2);
        memcpy(e->blk_intra + m, sn->intra + y * cells, (size_t)cells);
    }
}

/* one PU: the merge list against an AMVP search, both ranked by SATD of
 * the prediction plus lambda x a syntax proxy. The 2Nx2N winner's MVs
 * seed the search where they beat the AMVP predictor. */
static uint32_t search_pu(struct n265_frame_work *e, const pu_geo *g,
                          const mv_t start[2], pu_dec *d)
{
    int lam4 = ctu_lam_sad_q4(e);
    int w = g->w, h = g->h;
    int no_bi = w + h == 12;    /* 8x4 and 4x8 PUs stay uni-predicted */

    int cp = chroma_pred();

    mot_t mlist[MAX_MERGE];
    n265_merge_list_pu(e, g, mlist, MAX_MERGE);
    int best_m = 0;
    int nm = merge_rd_n();
    uint32_t ml[MAX_MERGE];         /* luma cost incl. the index rate */
    for (int i = 0; i < nm; i++) {
        ml[i] = mot_satd_wh(e, g->x, g->y, w, h, mlist[i]) +
                (uint32_t)(((i + 1) * lam4) >> 4);
        if (ml[i] < ml[best_m])
            best_m = i;
    }
    uint32_t best_mc = ml[best_m];
    int mg = cp >= 1 ? cpred_gate() : -1;
    if (mg == 3 && g->cbn == (1 << MIN_CB_LOG2))
        mg = -1;
    if (mg == 1 || mg == 2) {
        int b[2];
        two_best(ml, nm, b);
        if (b[1] != b[0] &&
            !(mg == 2 && (uint64_t)ml[b[1]] * 100 > (uint64_t)ml[b[0]] * 102)) {
            uint32_t c0 = ml[b[0]] + cpred_satd(e, g->x, g->y, w, h, mlist[b[0]]);
            uint32_t c1 = ml[b[1]] + cpred_satd(e, g->x, g->y, w, h, mlist[b[1]]);
            best_m = c1 < c0 ? b[1] : b[0];
            best_mc = c1 < c0 ? c1 : c0;
        } else {
            best_mc = ml[best_m] +
                cpred_satd(e, g->x, g->y, w, h, mlist[best_m]);
        }
    } else if (mg == 0) {
        best_mc = UINT32_MAX;
        for (int i = 0; i < nm; i++) {
            uint32_t c = ml[i] + cpred_satd(e, g->x, g->y, w, h, mlist[i]);
            if (c < best_mc) {
                best_mc = c;
                best_m = i;
            }
        }
    }

    mv_t me[2] = { { 0, 0 }, { 0, 0 } }, mvp[2][2];
    int8_t bref[2] = { 0, 0 };
    uint32_t side[2] = { UINT32_MAX, UINT32_MAX };
    int nl = e->slice_type == 0 ? 2 : 1;
    /* what the winning reference's ladder already costed (phase 10 item
     * 1b); this site's centre is free already - it is `acost` - so only
     * the four neighbours and the gate's scale come out of here. */
    int want_keep = n265_subpel_rate() == 1 || n265_subme_rung() != 0;
    struct n265_me_keep keep[2];
    keep[0].satd = keep[1].satd = UINT32_MAX;
    keep[0].nb_valid = keep[1].nb_valid = 0;
    keep[0].sq_valid = keep[1].sq_valid = 0;
    keep[0].rung_fired = keep[1].rung_fired = 0;
    for (int l = 0; l < 2; l++) {
        mvp[l][0].x = mvp[l][0].y = mvp[l][1].x = mvp[l][1].y = 0;
        if (l >= nl)
            continue;
        for (int r = 0; r < e->nref[l]; r++) {
            mv_t p[2], mv;
            n265_amvp_pu(e, l, r, g, p);
            mv_t seed = p[0];
            /* the seed choice is decided at full pel - n265_me_search
             * truncates whatever it is given - so where the 2Nx2N
             * winner and the predictor share a full-pel position the
             * two whole-PU SADs below decide nothing and are skipped */
            if (start && ((start[l].x >> 2) != (p[0].x >> 2) ||
                          (start[l].y >> 2) != (p[0].y >> 2)) &&
                n265_mc_sad(e, l, r, g->x, g->y, w, h, start[l]) <
                    n265_mc_sad(e, l, r, g->x, g->y, w, h, p[0]))
                seed = start[l];
            mot_t u;
            u.mv[0].x = u.mv[0].y = u.mv[1].x = u.mv[1].y = 0;
            u.ref[0] = u.ref[1] = 0;
            u.pred = (uint8_t)(1 << l);
            u.ref[l] = (int8_t)r;
            mv_t up[2][2] = { { { 0, 0 }, { 0, 0 } },
                              { { 0, 0 }, { 0, 0 } } };
            up[l][0] = p[0];
            up[l][1] = p[1];
            struct me_rate_ctx rcx;
            rcx.e = e; rcx.g = g; rcx.u = u;
            rcx.list = l; rcx.base = 6;
            memcpy(rcx.up, up, sizeof(up));
            struct n265_me_rate rc = { me_rate_bits, &rcx, (uint32_t)lam4 };
            struct n265_me_keep kp;
            n265_me_search(e, l, r, g->x, g->y, w, h, seed, &mv,
                           want_keep ? &kp : NULL,
                           n265_subpel_rate() >= 2 || n265_subme_rung()
                               ? &rc : NULL);
            u.mv[l] = mv;
            uint32_t c = mot_satd_wh(e, g->x, g->y, w, h, u) +
                (uint32_t)(((uint64_t)mot_rate_q8(e, g, &u, up, 6) * lam4)
                           >> 12);
            if (c < side[l]) {
                side[l] = c;
                me[l] = mv;
                bref[l] = (int8_t)r;
                mvp[l][0] = p[0];
                mvp[l][1] = p[1];
                if (want_keep)
                    keep[l] = kp;
            }
        }
    }

    mot_t amot;
    amot.mv[0] = me[0];
    amot.mv[1].x = amot.mv[1].y = 0;
    amot.ref[0] = bref[0];
    amot.ref[1] = 0;
    amot.pred = 1;
    uint32_t acost = side[0];
    /* the better uni candidate in LUMA, which is the unit bi_refine's
     * gate walks in: the chroma term must not reach it, or the gate
     * would hold a chroma-inclusive uni against a luma-only pair. */
    uint32_t uni_l = side[0];
    if (nl == 2 && side[1] < uni_l)
        uni_l = side[1];
    if (cp >= 3) {
        /* the uni/bi arbitration, chroma-aware on the last three
         * candidates only: the two per-list winners the reference
         * ranking already produced, and the pair they form. The ranking
         * that produced them stays luma - that is the search. */
        if (side[0] != UINT32_MAX)
            side[0] += cpred_satd(e, g->x, g->y, w, h, amot);
        if (nl == 2 && side[1] != UINT32_MAX) {
            mot_t u1;
            u1.mv[0].x = u1.mv[0].y = 0;
            u1.mv[1] = me[1];
            u1.ref[0] = 0;
            u1.ref[1] = bref[1];
            u1.pred = 2;
            side[1] += cpred_satd(e, g->x, g->y, w, h, u1);
        }
        acost = side[0];
    }
    if (nl == 2) {
        if (side[1] < acost) {
            amot.mv[0].x = amot.mv[0].y = 0;
            amot.ref[0] = 0;
            amot.mv[1] = me[1];
            amot.ref[1] = bref[1];
            amot.pred = 2;
            acost = side[1];
        }
        if (!no_bi && side[0] != UINT32_MAX && side[1] != UINT32_MAX) {
            mot_t bi;
            bi.mv[0] = me[0];
            bi.mv[1] = me[1];
            bi.ref[0] = bref[0];
            bi.ref[1] = bref[1];
            bi.pred = 3;
            mv_t bp[2][2];
            memcpy(bp, mvp, sizeof(bp));
            uint32_t c;
            if (bi_refine_iters()) {
                /* `acost` is the better uni candidate in exactly the
                 * units bi_refine costs the pair in */
                c = bi_refine(e, g, &bi, bp, lam4, 6,
                              bi_gate() ? uni_l : UINT32_MAX);
            } else {
                c = mot_satd_wh(e, g->x, g->y, w, h, bi) +
                    (uint32_t)(((uint64_t)mot_rate_q8(e, g, &bi, bp, 6) *
                                lam4) >> 12);
            }
            if (cp >= 3 && c != UINT32_MAX)
                c += cpred_satd(e, g->x, g->y, w, h, bi);
            if (c < acost) {
                amot = bi;
                acost = c;
                memcpy(mvp, bp, sizeof(bp));
            }
        }
    }

    if (cp >= 1 && cp < 3 && acost != UINT32_MAX)
        acost += cpred_satd(e, g->x, g->y, w, h, amot);

    if ((cp >= 2 || n265_subpel_rate() == 1) && acost != UINT32_MAX) {
        /* The search's last ranking, re-run. A merge candidate cannot
         * take this: its vector is what the index names, and a refined
         * one is no longer codable as a merge. So the quarter-pel step
         * belongs to the AMVP vector alone, and it is a CHOICE among
         * five positions rather than a search - the ladder inside
         * n265_me_search stays luma. `N265_SUBPEL_RATE == 1` is the same
         * five positions without the chroma term; `acost` carries one
         * only where `cp` put it there, so the two knobs compose. */
        for (int l = 0; l < 2; l++) {
            if (!(amot.pred & (1 << l)))
                continue;
            int uni = amot.pred == (1u << l) &&
                amot.mv[l].x == me[l].x && amot.mv[l].y == me[l].y &&
                keep[l].satd != UINT32_MAX;
            if (rung_owns(&keep[l], uni, cp >= 2))
                continue;
            uint32_t best_q;
            mv_t bmv = subpel_rerank(e, g, &amot, mvp, l, lam4, cp >= 2,
                                     want_keep ? &keep[l] : NULL, uni,
                                     acost, &best_q);
            if (bmv.x != amot.mv[l].x || bmv.y != amot.mv[l].y) {
                amot.mv[l] = bmv;
                acost = best_q;
            }
        }
    }

    if (best_mc <= acost) {
        d->merge = 1;
        d->midx = best_m;
        d->mot = mlist[best_m];
        d->mvp_idx[0] = d->mvp_idx[1] = 0;
        return best_mc;
    }
    d->merge = 0;
    d->midx = 0;
    d->mot = amot;
    for (int l = 0; l < 2; l++) {
        d->mvp[l][0] = mvp[l][0];
        d->mvp[l][1] = mvp[l][1];
        int c0 = abs(amot.mv[l].x - mvp[l][0].x) +
                 abs(amot.mv[l].y - mvp[l][0].y);
        int c1 = abs(amot.mv[l].x - mvp[l][1].x) +
                 abs(amot.mv[l].y - mvp[l][1].y);
        d->mvp_idx[l] = c1 < c0;
    }
    return acost;
}

/* rank one candidate partition: search both PUs in decode order, then
 * restore the maps the search wrote */
static uint32_t try_part(struct n265_frame_work *e, int x0, int y0, int n,
                         int part, const mv_t start[2], pu_dec d[2])
{
    mot_snap sn;
    mot_save(e, x0, y0, n, &sn);
    set_blk(e, e->blk_intra, x0, y0, n, 0);
    uint32_t tot = 0;
    memset(d, 0, 2 * sizeof(d[0]));
    for (int p = 0; p < 2; p++) {
        pu_geo g;
        g.cbx = x0; g.cby = y0; g.cbn = n;
        n265_pu_rect(part, p, x0, y0, n, &g.x, &g.y, &g.w, &g.h);
        g.excl_a1 = p == 1 && !PART_IS_HORIZ(part);
        g.excl_b1 = p == 1 && PART_IS_HORIZ(part);
        tot += search_pu(e, &g, start, &d[p]);
        set_mot_map_wh(e, g.x, g.y, g.w, g.h, d[p].mot);
    }
    mot_load(e, x0, y0, n, &sn);
    return tot;
}


static void set_cu_pred(struct n265_frame_work *e, int x0, int y0, int n,
                        part_cand *c)
{
    e->cu_pred[0] = c->py;
    e->cu_pred[1] = c->pc[0];
    e->cu_pred[2] = c->pc[1];
    e->cu_pred_stride[0] = n;
    e->cu_pred_stride[1] = e->cu_pred_stride[2] = n >> e->csx;
    e->cu_pred_x0 = x0;
    e->cu_pred_y0 = y0;
}

/* The same thing off an already-analysed whole-CU TB rather than a
 * partition candidate: tl/tcb/tcr hold the 2Nx2N prediction this CU was
 * priced with, and interpolation is position-invariant, so the RQT split
 * arm's four children are crops of it. Without this they each re-run the
 * 8-tap luma and 4-tap chroma filters over the same motion. */
static void set_cu_pred_tb(struct n265_frame_work *e, int x0, int y0, int n,
                           const tb_ctx *tl, const tb_ctx *tcb,
                           const tb_ctx *tcr)
{
    e->cu_pred[0] = tl->pred;
    e->cu_pred[1] = tcb->pred;
    e->cu_pred[2] = tcr->pred;
    e->cu_pred_stride[0] = n;
    e->cu_pred_stride[1] = e->cu_pred_stride[2] = n >> e->csx;
    e->cu_pred_x0 = x0;
    e->cu_pred_y0 = y0;
}

static void clear_cu_pred(struct n265_frame_work *e)
{
    e->cu_pred[0] = e->cu_pred[1] = e->cu_pred[2] = NULL;
}

/* CU-level inter syntax on the estimator: everything up to and including
 * rqt_root_cbf. Mirrors replay_cu's inter branch exactly - the 2Nx2N
 * candidates go through here too, so the two can't drift. */
static void est_inter_cu_bits(struct n265_frame_work *e, est_state *st, int x0,
                              int y0, int log2n, int part,
                              const pu_dec d[2], int any, int inc)
{
    cabac_ctx_t *local = st->ctx;
    int n = 1 << log2n;
    res_ops ops;
    n265_est_ops(&ops, st);
    st->bits += n265_est_bin(&local[CTX_CU_SKIP + inc], 0);
    st->bits += n265_est_bin(&local[CTX_PRED_MODE], 0);
    n265_write_part_mode(e, &ops, log2n, part);
    for (int p = 0; p < N265_NUM_PU(part); p++) {
        int px, py, pw, ph;
        n265_pu_rect(part, p, x0, y0, n, &px, &py, &pw, &ph);
        st->bits += n265_est_bin(&local[CTX_MERGE_FLAG], d[p].merge);
        if (d[p].merge) {
            st->bits += n265_est_bin(&local[CTX_MERGE_IDX], d[p].midx > 0);
            if (d[p].midx > 0 && MAX_MERGE > 2)
                st->bits += 256u * (uint32_t)(d[p].midx < MAX_MERGE - 1
                                              ? d[p].midx : d[p].midx - 1);
            continue;
        }
        int pred = d[p].mot.pred;
        if (e->slice_type == 0) {
            if (pw + ph != 12) {
                st->bits += n265_est_bin(
                    &local[CTX_INTER_IDC + (e->ctb_log2 - log2n)], pred == 3);
                if (pred != 3)
                    st->bits += n265_est_bin(&local[CTX_INTER_IDC + 4],
                                             pred >> 1);
            } else {
                st->bits += n265_est_bin(&local[CTX_INTER_IDC + 4], pred >> 1);
            }
        }
        for (int l = 0; l < 2; l++) {
            if (!(pred & (1 << l)))
                continue;
            if (e->nref[l] > 1) {      /* ref_idx_lX, truncated unary */
                int r = d[p].mot.ref[l], cmax = e->nref[l] - 1;
                st->bits += n265_est_bin(&local[CTX_REF_IDX + 0], r > 0);
                if (r > 0 && cmax > 1)
                    st->bits += n265_est_bin(&local[CTX_REF_IDX + 1], r > 1);
                if (r > 1 && cmax > 2)
                    st->bits += 256u *
                        (uint32_t)((r < cmax ? r : cmax - 1) - 1);
            }
            int k = d[p].mvp_idx[l];
            est_mvd(st, d[p].mot.mv[l].x - d[p].mvp[l][k].x,
                    d[p].mot.mv[l].y - d[p].mvp[l][k].y);
            st->bits += n265_est_bin(&local[CTX_MVP_FLAG], k);
        }
    }
    /* rqt_root_cbf is absent, and inferred 1, only for a merged 2Nx2N CU */
    if (!(part == PART_2Nx2N && d[0].merge))
        st->bits += n265_est_bin(&local[CTX_RQT_ROOT_CBF], any);
}

/* full RD of one partition: predict PU by PU, then cost the CU with a
 * flat transform set (the commit re-decides flat vs split, exactly as
 * the 2Nx2N candidates do) */
static uint64_t rd_part(struct n265_frame_work *e, int x0, int y0, int log2n,
                        int inc, part_cand *c)
{
    int n = 1 << log2n;
    int csx = e->csx, csy = e->csy;
    int hn = n >> csx;
    int cx0 = x0 >> csx, cy0 = y0 >> csy, log2c = log2n - csx;
    int qp_c = n265_chroma_qp(e->cur_ctu_qp, e->chroma_idc);
    for (int p = 0; p < N265_NUM_PU(c->part); p++) {
        int px, py, pw, ph;
        n265_pu_rect(c->part, p, x0, y0, n, &px, &py, &pw, &ph);
        mc_pred_wh(e, 0, px, py, pw, ph, c->d[p].mot,
                   c->py + (py - y0) * n + (px - x0), n);
        for (int ci = 1; ci <= 2; ci++)
            mc_pred_wh(e, ci, px >> csx, py >> csy, pw >> csx, ph >> csy,
                       c->d[p].mot,
                       c->pc[ci - 1] + ((py - y0) >> csy) * hn +
                           ((px - x0) >> csx),
                       hn);
    }
    set_cu_pred(e, x0, y0, n, c);
    tb_analyze(e, 0, x0, y0, log2n, e->cur_ctu_qp, MODE_INTER, mot_zero,
               &c->tl);
    tb_analyze(e, 1, cx0, cy0, log2c, qp_c, MODE_INTER, mot_zero, &c->tcb);
    tb_analyze(e, 2, cx0, cy0, log2c, qp_c, MODE_INTER, mot_zero, &c->tcr);
    tb_make_recon(e, 0, log2n, e->cur_ctu_qp, MODE_INTER, &c->tl);
    tb_make_recon(e, 1, log2c, qp_c, MODE_INTER, &c->tcb);
    tb_make_recon(e, 2, log2c, qp_c, MODE_INTER, &c->tcr);
    clear_cu_pred(e);
    c->any = c->tl.nz || tb_any(&c->tcb) || tb_any(&c->tcr);
    uint64_t d = tb_ssd(e, 0, x0, y0, log2n, &c->tl) +
        tb_ssd(e, 1, cx0, cy0, log2c, &c->tcb) +
        tb_ssd(e, 2, cx0, cy0, log2c, &c->tcr);

    cabac_ctx_t local[CTX_COUNT];
    memcpy(local, e->est_ctx, sizeof(local));
    est_state st = { local, 0 };
    est_inter_cu_bits(e, &st, x0, y0, log2n, c->part, c->d, c->any, inc);
    if (c->any)
        est_tt_bits(e, &st, log2n, MODE_INTER, &c->tl, &c->tcb, &c->tcr);
    return rd_j(e, d, st.bits, ctu_lam(e, e->cur_ctu_qp));
}

/* N265_CPRED_TRACE: the misalignment read this item opened with. For
 * every COMMITTED inter PU (the commit loop, so losing partitions and
 * losing candidates are not counted), cost the luma-chosen vector's
 * chroma SATD against the best chroma SATD over the same shortlist the
 * choice was made from - the merge candidates the prefilter ranks, plus
 * the four quarter-pel neighbours of the chosen vector - and print what
 * the better chroma alternative would cost in luma. One CSV row per PU
 * on stderr; run at --threads 1, the rows are not serialised. */
static int cpred_trace(void)
{
    static int v = -1;
    if (v < 0) v = n265_env("N265_CPRED_TRACE") != NULL;
    return v;
}

static void cpred_trace_pu(struct n265_frame_work *e, const pu_geo *g,
                           mot_t mot)
{
    int w = g->w, h = g->h;
    uint32_t cc = mot_satd_c(e, g->x, g->y, w, h, mot);
    if (cc == CHROMA_NA) {
        fprintf(stderr, "CPRED,%d,%d,%d,na,0,0,0,0,0,0,0,0,0\n", e->poc, w, h);
        return;
    }
    uint32_t lc = mot_satd_wh(e, g->x, g->y, w, h, mot);
    uint32_t bc = cc, bl = lc;
    int from = 0;       /* 1 = a merge candidate, 2 = a quarter-pel step */
    /* and the JOINT winner, which is what an arm would actually pick:
     * the shortlist entry minimising luma SATD + chroma SATD at w = 1 */
    uint64_t jbest = (uint64_t)lc + cc;
    uint32_t jl = lc, jc = cc;
    int jfrom = 0;

    mot_t cand[MAX_MERGE + 4];
    int nc = 0, ncmerge;
    n265_merge_list_pu(e, g, cand, MAX_MERGE);
    nc = ncmerge = merge_rd_n();
    static const int st[4][2] = { { 1, 0 }, { -1, 0 }, { 0, 1 }, { 0, -1 } };
    for (int i = 0; i < 4; i++) {
        mot_t t = mot;
        int ok = 1;
        for (int l = 0; l < 2; l++) {
            if (!(t.pred & (1 << l)))
                continue;
            mv_t v = { (int16_t)(t.mv[l].x + st[i][0]),
                       (int16_t)(t.mv[l].y + st[i][1]) };
            mv_t vc = v;
            n265_clamp_mv(e, g->x, g->y, w, h, &vc);
            if (vc.x != v.x || vc.y != v.y)
                ok = 0;
            t.mv[l] = v;
        }
        if (ok)
            cand[nc++] = t;
    }
    /* and the FREE read: the best chroma among the shortlist entries
     * that cost no more than 2% of luma SATD over the chosen vector -
     * the chroma this item could take without paying for it. */
    uint32_t kc = cc, kl = lc;
    uint64_t lcap = (uint64_t)lc * 102 / 100;
    for (int i = 0; i < nc; i++) {
        uint32_t c = mot_satd_c(e, g->x, g->y, w, h, cand[i]);
        uint32_t l = mot_satd_wh(e, g->x, g->y, w, h, cand[i]);
        int src = i < ncmerge ? 1 : 2;
        if (c < bc) {
            bc = c;
            bl = l;
            from = src;
        }
        if ((uint64_t)l + c < jbest) {
            jbest = (uint64_t)l + c;
            jl = l;
            jc = c;
            jfrom = src;
        }
        if (l <= lcap && c < kc) {
            kc = c;
            kl = l;
        }
    }
    fprintf(stderr, "CPRED,%d,%d,%d,%u,%u,%u,%u,%d,%u,%u,%d,%u,%u\n", e->poc,
            w, h, lc, cc, bc, bl, from, jl, jc, jfrom, kl, kc);
}

/* Commit an inter CU (any partition): maps, records, recon. Returns the
 * rqt_root_cbf it actually coded, which is the `any` it was handed except
 * under N265_RDOQ=2 - see below.
 *
 * Under N265_RDOQ=2 the TBs arriving here were quantised plainly by
 * whichever trial won (the merge and AMVP candidates, or rd_part's
 * partition), so the lattice runs on them HERE, before a single record
 * byte is written. Three things follow from that and all three are done
 * before `rec_u8`:
 *
 *  - `any` has to be re-derived. It can only fall: the lattice's first
 *    pass is the plain quantiser's own rounding, so a block the plain
 *    quantiser sent to zero the lattice sends to zero too (19 million
 *    trials of the census, no exception), and a block it kept the lattice
 *    may still drop. A stale `any = 1` over an all-zero transform tree
 *    would code cbf_luma = 0 into a leaf where 7.3.8.8 INFERS it to 1.
 *  - a merged 2Nx2N CU has no rqt_root_cbf at all (7.3.8.5 omits it and
 *    infers 1), so one whose residual the lattice took away is not a
 *    codable inter CU: it is a SKIP, and is coded as one.
 *  - the caller's `e->last_cu_nores` is the returned value's, not the
 *    trial's.
 */
static int commit_inter_cu(struct n265_frame_work *e, int x0, int y0,
                           int log2n, int part, const pu_dec d[2], int any,
                           tb_ctx *tl, tb_ctx *tcb, tb_ctx *tcr,
                           part_cand *pred_src)
{
    int n = 1 << log2n;
    if (e->rdoq >= 2 && any && tl) {
        int qp_c = n265_chroma_qp(e->cur_ctu_qp, e->chroma_idc);
        int cx0 = x0 >> e->csx, cy0 = y0 >> e->csy, log2c = log2n - e->csx;
        tb_relattice(e, 0, x0, y0, log2n, e->cur_ctu_qp, MODE_INTER, tl);
        tb_relattice(e, 1, cx0, cy0, log2c, qp_c, MODE_INTER, tcb);
        tb_relattice(e, 2, cx0, cy0, log2c, qp_c, MODE_INTER, tcr);
        any = tl->nz || tb_any(tcb) || tb_any(tcr);
        if (!any && part == PART_2Nx2N && d[0].merge) {
            skip_cu(e, x0, y0, log2n, d[0].mot, d[0].midx);
            return 0;
        }
    }
    set_blk(e, e->blk_intra, x0, y0, n, 0);
    set_blk(e, e->blk_skip, x0, y0, n, 0);
    set_mode_map(e, x0, y0, n, INTRA_DC);
    mark_cu_edges(e, x0, y0, n);
    mark_pu_edge(e, part, x0, y0, n);
    rec_u8(e, (unsigned)part);
    for (int p = 0; p < N265_NUM_PU(part); p++) {
        int px, py, pw, ph;
        n265_pu_rect(part, p, x0, y0, n, &px, &py, &pw, &ph);
        /* merge records carry the index in bits 3-5; the mvp bits belong
         * to AMVP records only (a stale mvp_idx[1] on a merge record
         * reads as index + 4 once five candidates are allowed; found
         * 2026-09-06 while widening MAX_MERGE) */
        unsigned f = d[p].merge
            ? (1u | ((unsigned)d[p].midx << 3))
            : (((unsigned)d[p].mvp_idx[0] << 1) |
               ((unsigned)(d[p].mot.pred - 1) << 3) |
               ((unsigned)d[p].mvp_idx[1] << 5));
        rec_u8(e, f);
        if (!d[p].merge)
            for (int l = 0; l < 2; l++) {
                if (!(d[p].mot.pred & (1 << l)))
                    continue;
                rec_u8(e, (unsigned)d[p].mot.ref[l]);
                int k = d[p].mvp_idx[l];
                int16_t mvd[2] = {
                    (int16_t)(d[p].mot.mv[l].x - d[p].mvp[l][k].x),
                    (int16_t)(d[p].mot.mv[l].y - d[p].mvp[l][k].y)
                };
                rec_levels(e, mvd, 2);
            }
        if (cpred_trace()) {
            pu_geo g = { x0, y0, n, px, py, pw, ph,
                         p == 1 && !PART_IS_HORIZ(part),
                         p == 1 && PART_IS_HORIZ(part) };
            cpred_trace_pu(e, &g, d[p].mot);
        }
        set_mot_map_wh(e, px, py, pw, ph, d[p].mot);
    }
    rec_u8(e, (unsigned)(any != 0));
    if (pred_src)
        set_cu_pred(e, x0, y0, n, pred_src);
    else if (any && log2n <= 5 && tl)
        set_cu_pred_tb(e, x0, y0, n, tl, tcb, tcr);
    if (any) {
        commit_inter_tt(e, d[0].mot, x0, y0, log2n, tl, tcb, tcr);
    } else {
        tb_commit(e, 0, x0, y0, log2n, tl);
        tb_commit(e, 1, x0 >> e->csx, y0 >> e->csy, log2n - e->csx, tcb);
        tb_commit(e, 2, x0 >> e->csx, y0 >> e->csy, log2n - e->csx, tcr);
    }
    clear_cu_pred(e);
    return any;
}

/* The 2Nx2N merge shortlist's pick: luma SAD as always, plus the chroma
 * SAD under N265_CHROMA_PRED. *luma comes back as the WINNER's luma-only
 * SAD, because that number is not just this ranking's - the static-skip
 * threshold, the economy gates and the intra prefilter all read it, and
 * a chroma-inflated one would move gates this item is not about. */
static int merge_pick_sad(struct n265_frame_work *e, int x0, int y0,
                          int log2n, const mot_t *mlist, uint32_t *luma)
{
    int n = 1 << log2n, cp = chroma_pred(), k = merge_rd_n();
    uint32_t sd[MAX_MERGE];
    int midx = 0;
    for (int i = 0; i < k; i++) {
        sd[i] = mot_sad(e, x0, y0, log2n, mlist[i]);
        if (sd[i] < sd[midx])
            midx = i;
    }
    *luma = sd[midx];
    if (cp < 1)
        return midx;

    int g = cpred_gate();
    if (g == 3 && log2n == MIN_CB_LOG2)
        return midx;
    if (g == 1 || g == 2) {
        int b[2];
        two_best(sd, k, b);
        if (b[1] == b[0])
            return midx;
        if (g == 2 && (uint64_t)sd[b[1]] * 100 > (uint64_t)sd[b[0]] * 102)
            return midx;        /* luma has already decided this one */
        uint32_t c0 = sd[b[0]] + cpred_sad(e, x0, y0, n, n, mlist[b[0]]);
        uint32_t c1 = sd[b[1]] + cpred_sad(e, x0, y0, n, n, mlist[b[1]]);
        /* the ties go the way the luma-only pick went */
        int w = c1 < c0 ? b[1] : b[0];
        *luma = sd[w];
        return w;
    }

    uint32_t best = UINT32_MAX;
    for (int i = 0; i < k; i++) {
        uint32_t c = sd[i] + cpred_sad(e, x0, y0, n, n, mlist[i]);
        if (c < best) {
            best = c;
            midx = i;
            *luma = sd[i];
        }
    }
    return midx;
}

/* N265_STATIC_COMMIT's predicate, the census's P3 (sec 11.5, family B).
 *
 * Asked at the same site as the strict static-skip exit, before the merge
 * arm, so a `1` here costs the node everything but the merge list and the
 * SAD pick that produced `sad`. The three conjuncts, cheapest first:
 *
 *   1. the merge candidate's SAD is inside the multiplier's bound. The
 *      bound is the strict one (`N265_SKIP_THR` eighths of a luma level
 *      per pixel) times the knob, so 10 is 2.5 levels a pixel at the
 *      default 2.
 *   2. the cu-tree propagate-in over the node is exactly zero: no later
 *      picture predicts from this block, so what this decision loses
 *      cannot propagate. Read off the lookahead's 16x16 grid, which the
 *      driver froze onto the frame before dispatch.
 *   3. the co-located area was ALL skip in the retired frame nearest
 *      this one in display order (sc_resolve picks it, on the driver at
 *      dispatch). "All" and not a share: the census's column is the
 *      share and its separation comes from the 100% bucket.
 *
 * The propagate test leads the map walk because it is O(1) at 32x32 and
 * the map walk is O(n^2 / 16); on the population that reaches here the
 * propagate conjunct is the one that fails most often. */
static int sc_prop_zero(const struct n265_frame_work *e, int x0, int y0, int n)
{
    int nb = n >> 4;
    if (nb < 1)
        nb = 1;
    for (int by = y0 >> 4; by < (y0 >> 4) + nb && by < e->la_bh; by++)
        for (int bx = x0 >> 4; bx < (x0 >> 4) + nb && bx < e->la_bw; bx++)
            if (!e->sc_prop[by * e->la_bw + bx])
                return 0;
    return 1;
}

static int sc_commit(const struct n265_frame_work *e, int x0, int y0, int n,
                     uint32_t sad)
{
    int k = static_commit();
    if (!k || !e->sc_map)
        return 0;
    if ((uint64_t)sad * 8u > (uint64_t)(n * n * skip_thr8()) * (unsigned)k)
        return 0;
    if ((sc_conj() & 1) && !sc_prop_zero(e, x0, y0, n))
        return 0;
    if (!(sc_conj() & 2))
        return 1;
    int cw = e->pad_w >> 2, ch = e->pad_h >> 2, seen = 0;
    for (int cy = y0 >> 2; cy < (y0 >> 2) + (n >> 2) && cy < ch; cy++)
        for (int cx = x0 >> 2; cx < (x0 >> 2) + (n >> 2) && cx < cw; cx++) {
            if (!e->sc_map[(size_t)cy * e->map_stride + cx])
                return 0;
            seen = 1;
        }
    return seen;
}

static uint64_t analyze_cu(struct n265_frame_work *e, int x0, int y0, int log2n)
{
    int n = 1 << log2n;
    int qp_c = n265_chroma_qp(e->cur_ctu_qp, e->chroma_idc);
    int csx = e->csx, csy = e->csy;
    int cx0 = x0 >> csx, cy0 = y0 >> csy, log2c = log2n - csx;
    e->last_skip_clean = 0;
    e->last_cu_nores = 0;
    mark_cu_edges(e, x0, y0, n);

    size_t pos0 = e->rec_len;

    struct n265_cu_rec *cr = CU_CUR(e);
    uint64_t cu_t0 = cr ? cu_tick() : 0;
    if (cr)
        e->cu_arm = CU_ARM_MERGE;

    if (e->slice_type != 2) {
        /* one instance per row lane; analyze_cu shares them across
         * quadtree depths exactly as the statics did */
        tb_ctx *m_tl = &e->sc->m_tl, *m_tcb = &e->sc->m_tcb, *m_tcr = &e->sc->m_tcr;
        tb_ctx *a_tl = &e->sc->a_tl, *a_tcb = &e->sc->a_tcb, *a_tcr = &e->sc->a_tcr;
        tb_ctx *i_tl = &e->sc->i_tl, *i_tcb = &e->sc->i_tcb, *i_tcr = &e->sc->i_tcr;
        mot_t mlist[MAX_MERGE];
        n265_merge_list(e, x0, y0, n, n, mlist, MAX_MERGE);
        uint32_t cand_plain_sad;
        int midx = merge_pick_sad(e, x0, y0, log2n, mlist, &cand_plain_sad);
        mot_t cand = mlist[midx];
        if (cr) {
            cr->cyc_pre = (uint32_t)(cu_tick() - cu_t0);
            cr->cand_sad = cand_plain_sad;
            /* both bounds are SQUARED sample quantities and both are
             * sixteen times larger at Main10, where thr_clean at Qp' 63
             * and n 64 reaches 3.4e9 - inside u32 by a fifth, and past
             * it at N265_ES_THR above 125. Saturate the way d_skip
             * below already does rather than wrap the column. */
            cr->static_thr = (uint32_t)(n * n * skip_thr8());
            uint64_t tc = skip_clean_thr(e->cur_ctu_qp, n);
            cr->thr_clean = (uint32_t)(tc > UINT32_MAX ? UINT32_MAX : tc);
        }

        /* strict static skip at the best merge candidate.
         *
         * Not under --lossless: this arm COMMITS a skip on a prediction
         * SAD bound and never looks at what it threw away, and a bound of
         * a quarter of a luma level per pixel is not zero. Every skip
         * there goes through the RD arbitration below instead, where a
         * prediction that is not exactly the source costs infinity; where
         * it IS exact the same skip is still what wins, at the price of
         * having priced the merge candidate first. */
        if (!e->lossless &&
            (cand_plain_sad * 8u <= (uint32_t)(n * n * skip_thr8()) ||
             sc_commit(e, x0, y0, n, cand_plain_sad))) {
            skip_cu(e, x0, y0, log2n, cand, midx);
            e->rec_pos = pos0;
            est_state adv = { e->est_ctx, 0 };
            n265_est_advance_cu(e, &adv, x0, y0, log2n);
            set_blk(e, e->blk_qp, x0, y0, n,
                    e->ent_qp_coded ? N265_QP_STORE(e->cur_ctu_qp) : N265_QP_INHERIT);
            uint64_t ssd = cu_area_ssd(e, x0, y0, n);
            e->last_skip_clean = ssd <= skip_clean_thr(e->cur_ctu_qp, n);
            e->last_cu_nores = 1;
            if (cr) {
                cr->kind = N265_CUT_SKIP;
                cr->flags |= N265_CUF_STATIC | N265_CUF_NORES |
                    (e->last_skip_clean ? N265_CUF_CLEAN : 0);
                cr->d_skip = (uint32_t)(ssd > UINT32_MAX ? UINT32_MAX : ssd);
                cr->cyc_merge = (uint32_t)(cu_tick() - cu_t0);
            }
            return rd_j(e, ssd, adv.bits, ctu_lam(e, e->cur_ctu_qp));
        }

        if (log2n <= 5) {
            uint32_t lam = ctu_lam(e, e->cur_ctu_qp);
            int inc = est_skip_inc(e, x0, y0);
            /* economy gates: a very good merge prediction prunes the
             * expensive candidates (classic early-skip shape). The bound
             * is a SAD per pixel - n*n/8 and n*n/4 in 8-bit luma levels -
             * so the right-hand side carries BIT_DEPTH_SHIFT; without it
             * neither gate could ever fire at Main10 and the 1.2% they
             * are worth would go quietly missing along with the decisions
             * they make. */
            int gate_amvp = !no_gates_on() &&
                (uint64_t)cand_plain_sad * 800u <
                    ((uint64_t)(n * n) * (unsigned)gate_scale()
                     << BIT_DEPTH_SHIFT);
            int gate_intra = !no_gates_on() &&
                (uint64_t)cand_plain_sad * 400u <
                    ((uint64_t)(n * n) * (unsigned)gate_scale()
                     << BIT_DEPTH_SHIFT);
            int gate_part = 0;

            /* merge candidate (and the forced-skip variant) */
            tb_analyze(e, 0, x0, y0, log2n, e->cur_ctu_qp, MODE_INTER, cand, m_tl);
            tb_analyze(e, 1, cx0, cy0, log2c, qp_c, MODE_INTER, cand, m_tcb);
            tb_analyze(e, 2, cx0, cy0, log2c, qp_c, MODE_INTER, cand, m_tcr);
            tb_make_recon(e, 0, log2n, e->cur_ctu_qp, MODE_INTER, m_tl);
            tb_make_recon(e, 1, log2c, qp_c, MODE_INTER, m_tcb);
            tb_make_recon(e, 2, log2c, qp_c, MODE_INTER, m_tcr);
            int m_any = m_tl->nz || tb_any(m_tcb) || tb_any(m_tcr);
            /* P1 CBF fast: the merge prediction already quantised to
             * nothing, so every remaining candidate can only re-buy the
             * same block with more syntax */
            if ((cbf_fast() == 1 || cbf_fast() == 2) && !m_any)
                gate_amvp = gate_intra = 1;
            uint64_t d_merge = tb_ssd(e, 0, x0, y0, log2n, m_tl) +
                tb_ssd(e, 1, cx0, cy0, log2c, m_tcb) +
                tb_ssd(e, 2, cx0, cy0, log2c, m_tcr);
            /* forced skip: prediction only */
            uint64_t d_skip;
            {
                tb_ctx t;
                size_t ca = (size_t)(n >> csx) * (n >> csy);
                memcpy(t.recon, m_tl->pred, (size_t)n * n * sizeof(pixel));
                d_skip = tb_ssd(e, 0, x0, y0, log2n, &t);
                memcpy(t.recon, m_tcb->pred, ca * sizeof(pixel));
                d_skip += tb_ssd(e, 1, cx0, cy0, log2c, &t);
                memcpy(t.recon, m_tcr->pred, ca * sizeof(pixel));
                d_skip += tb_ssd(e, 2, cx0, cy0, log2c, &t);
            }
            est_state st;
            cabac_ctx_t local[CTX_COUNT];

            /* skip bits */
            memcpy(local, e->est_ctx, sizeof(local));
            st.ctx = local; st.bits = 0;
            st.bits += n265_est_bin(&local[CTX_CU_SKIP + inc], 1);
            st.bits += n265_est_bin(&local[CTX_MERGE_IDX], midx > 0);
            if (midx > 0 && MAX_MERGE > 2)
                st.bits += 256u * (uint32_t)(midx < MAX_MERGE - 1 ? midx
                                                                  : midx - 1);
            uint64_t j_skip = rd_j(e, d_skip, st.bits, lam);

            /* merge bits */
            pu_dec pd[2];
            memset(pd, 0, sizeof(pd));
            pd[0].mot = cand;
            pd[0].merge = 1;
            pd[0].midx = midx;
            uint64_t j_merge = UINT64_MAX;
            if (m_any) {
                memcpy(local, e->est_ctx, sizeof(local));
                st.ctx = local; st.bits = 0;
                est_inter_cu_bits(e, &st, x0, y0, log2n, PART_2Nx2N, pd,
                                  m_any, inc);
                est_tt_bits(e, &st, log2n, MODE_INTER, m_tl, m_tcb,
                            m_tcr);
                j_merge = rd_j(e, d_merge, st.bits, lam);
            }

            /* the merge-first skip exit: both merge arms are priced by
             * now, and where the skip won on J with its distortion
             * inside the quantiser floor, this CU's winner is already
             * decided - the AMVP search cannot code less than nothing,
             * and the rect/AMP trials are never entered on a CU the
             * skip wins anyway. */
            if (skip_exit() && !gate_amvp && j_skip <= j_merge &&
                (skip_exit() >= 3 ||
                 d_skip * (skip_exit() == 1 ? 4u : 1u) <=
                     skip_clean_thr(e->cur_ctu_qp, n)))
                gate_amvp = 1;

            /* N265_CU_TRACE: the merge arm is over. Everything above is
             * work no early skip exit could avoid - it is what the exit
             * would be decided ON - and everything below is what one
             * would save. */
            uint64_t cu_ta = 0;
            if (cr) {
                cu_ta = cu_tick();
                if (j_skip <= j_merge)
                    cr->flags |= N265_CUF_SKIPWIN;
                cr->cyc_merge = (uint32_t)(cu_ta - cu_t0);
                cr->d_skip = (uint32_t)(d_skip > UINT32_MAX ? UINT32_MAX
                                                            : d_skip);
                if (gate_amvp)
                    cr->flags |= N265_CUF_GAMVP;
                e->cu_arm = CU_ARM_AMVP;
            }

            /* amvp candidate: per-list ME, then the best of uni-L0,
             * uni-L1, bi by prediction SAD carries to RD */
            mv_t mvp[2][2] = { { { 0, 0 }, { 0, 0 } },
                               { { 0, 0 }, { 0, 0 } } };
            mv_t me_mv[2] = { { 0, 0 }, { 0, 0 } };
            uint64_t j_amvp = UINT64_MAX;
            uint64_t d_amvp = UINT64_MAX;
            int a_any = 0;
            int mvp_idx[2] = { 0, 0 };
            mot_t amot = { { { 0, 0 }, { 0, 0 } }, 1 };
            int same_as_cand = 1;
            pu_geo pg = { x0, y0, n, x0, y0, n, n, 0, 0 };
            int lam4 = ctu_lam_sad_q4(e);
            /* what the winning reference's ladder already costed, for
             * the arm-1 re-rank below (phase 10 item 1b) */
            int want_keep = n265_subpel_rate() == 1 ||
                            n265_subme_rung() != 0;
            struct n265_me_keep keep[2];
            keep[0].satd = keep[1].satd = UINT32_MAX;
            keep[0].nb_valid = keep[1].nb_valid = 0;
            keep[0].sq_valid = keep[1].sq_valid = 0;
            keep[0].rung_fired = keep[1].rung_fired = 0;
            if (!gate_amvp) {
                /* per list: search every reference, keep the one whose
                 * SAD + lambda x (mvd + ref_idx) bits is lowest, and the
                 * predictors of that reference */
                for (int l = 0; l < (e->slice_type == 0 ? 2 : 1); l++) {
                    uint32_t best_c = UINT32_MAX;
                    for (int r = 0; r < e->nref[l]; r++) {
                        mv_t p[2], mv;
                        n265_amvp(e, l, r, x0, y0, n, n, p);
                        mot_t u;
                        u.mv[0].x = u.mv[0].y = u.mv[1].x = u.mv[1].y = 0;
                        u.ref[0] = u.ref[1] = 0;
                        u.pred = (uint8_t)(1 << l);
                        u.ref[l] = (int8_t)r;
                        mv_t up[2][2] = { { { 0, 0 }, { 0, 0 } },
                                          { { 0, 0 }, { 0, 0 } } };
                        up[l][0] = p[0];
                        up[l][1] = p[1];
                        struct me_rate_ctx rcx;
                        rcx.e = e; rcx.g = &pg; rcx.u = u;
                        rcx.list = l; rcx.base = 4;
                        memcpy(rcx.up, up, sizeof(up));
                        struct n265_me_rate rc = { me_rate_bits, &rcx,
                                                   (uint32_t)lam4 };
                        struct n265_me_keep kp;
                        uint32_t sad = n265_me_search(e, l, r, x0, y0, n, n,
                                                      p[0], &mv,
                                                      want_keep ? &kp : NULL,
                                                      n265_subpel_rate() >= 2
                                                      || n265_subme_rung()
                                                          ? &rc : NULL);
                        u.mv[l] = mv;
                        /* rate proxy (or the real syntax under
                         * N265_MVD_BITS), at lambda_sad. n265_me_search
                         * returns the FULL-PEL SAD, so the reference
                         * ranking never sees what the subpel round did
                         * to the winner; N265_ME_SATD re-costs it. */
                        uint32_t d = me_satd_on()
                            ? mot_satd_wh(e, x0, y0, n, n, u) : sad;
                        uint32_t c = d +
                            (uint32_t)(((uint64_t)mot_rate_q8(e, &pg, &u, up, 4)
                                        * lam4) >> 12);
                        if (c < best_c) { best_c = c; me_mv[l] = mv; mvp[l][0] = p[0]; mvp[l][1] = p[1]; amot.ref[l] = (int8_t)r; if (want_keep) keep[l] = kp; }
                    }
                }
                amot.mv[0] = me_mv[0];
                if (e->slice_type == 0) {
                    mot_t u1 = { { { 0, 0 }, me_mv[1] }, 2, { 0, amot.ref[1] } };
                    mot_t bi = { { me_mv[0], me_mv[1] }, 3, { amot.ref[0], amot.ref[1] } };
                    mv_t bp[2][2];
                    memcpy(bp, mvp, sizeof(bp));
                    /* SAD cannot tell the blur of a bi average from a
                     * misalignment, and this is the comparison that
                     * decides bi against uni; N265_ME_SATD makes it the
                     * Hadamard cost the rect/AMP PU search already uses */
                    int ms = me_satd_on();
                    int cp3 = chroma_pred() >= 3;
                    uint32_t s0 = ms ? mot_satd_wh(e, x0, y0, n, n, amot)
                                     : mot_sad(e, x0, y0, log2n, amot);
                    uint32_t s1 = ms ? mot_satd_wh(e, x0, y0, n, n, u1)
                                     : mot_sad(e, x0, y0, log2n, u1);
                    /* the luma-only better uni, which is what bi_gate's
                     * margin below is calibrated against */
                    if (mvd_bits_on()) {
                        s0 += (uint32_t)(((uint64_t)mot_rate_q8(e, &pg, &amot, mvp, 6) * lam4) >> 12);
                        s1 += (uint32_t)(((uint64_t)mot_rate_q8(e, &pg, &u1, mvp, 6) * lam4) >> 12);
                    }
                    /* R2: ask the gate in the units this arbitration
                     * runs in rather than bi_refine's, because those are
                     * SAD here by default and Hadamard inside. The plain
                     * pair's cost is the one number both the gate and
                     * the comparison below need. */
                    uint32_t sb = UINT32_MAX;
                    int refine = bi_refine_iters() != 0;
                    if (refine && bi_gate()) {
                        sb = ms ? mot_satd_wh(e, x0, y0, n, n, bi)
                                : mot_sad(e, x0, y0, log2n, bi);
                        if (mvd_bits_on())
                            sb += (uint32_t)(((uint64_t)mot_rate_q8(e, &pg, &bi, bp, 6) * lam4) >> 12);
                        uint32_t uni = s0 < s1 ? s0 : s1;
                        if ((int64_t)sb * 100 >
                            (int64_t)uni * (100 + bi_gate_margin()))
                            refine = 0;
                        else
                            sb = UINT32_MAX;    /* the walk will move it */
                    }
                    if (refine)
                        (void)bi_refine(e, &pg, &bi, bp, lam4, 6, UINT32_MAX);
                    if (sb == UINT32_MAX) {
                        sb = ms ? mot_satd_wh(e, x0, y0, n, n, bi)
                                : mot_sad(e, x0, y0, log2n, bi);
                        if (mvd_bits_on())
                            sb += (uint32_t)(((uint64_t)mot_rate_q8(e, &pg, &bi, bp, 6) * lam4) >> 12);
                    }
                    /* the chroma term goes on last, on the three
                     * candidates this arbitration ranks and nothing the
                     * gates above read - in the metric the site ranks in */
                    if (cp3) {
                        s0 += ms ? cpred_satd(e, x0, y0, n, n, amot)
                                 : cpred_sad(e, x0, y0, n, n, amot);
                        s1 += ms ? cpred_satd(e, x0, y0, n, n, u1)
                                 : cpred_sad(e, x0, y0, n, n, u1);
                        sb += ms ? cpred_satd(e, x0, y0, n, n, bi)
                                 : cpred_sad(e, x0, y0, n, n, bi);
                    }
                    if (s1 < s0 && s1 <= sb)
                        amot = u1;
                    else if (sb < s0) {
                        amot = bi;
                        memcpy(mvp, bp, sizeof(bp));
                    }
                }
                static const int st[4][2] = { { 1, 0 }, { -1, 0 },
                                              { 0, 1 }, { 0, -1 } };
                if (rr_trace()) {
                    /* the census of what the re-rank costs, countable
                     * with the arm OFF: the positions it would visit */
                    rr_bump(RR_CU, 1);
                    for (int l = 0; l < 2; l++) {
                        if (!(amot.pred & (1 << l)))
                            continue;
                        rr_bump(RR_LISTS, 1);
                        rr_bump(RR_SATD, 1);    /* the centre */
                        for (int i = 0; i < 4; i++) {
                            mv_t v = { (int16_t)(amot.mv[l].x + st[i][0]),
                                       (int16_t)(amot.mv[l].y + st[i][1]) };
                            mv_t vc = v;
                            n265_clamp_mv(e, x0, y0, n, n, &vc);
                            if (vc.x == v.x && vc.y == v.y)
                                rr_bump(RR_SATD, 1);
                        }
                    }
                }
                if (chroma_pred() >= 2 || n265_subpel_rate() == 1) {
                    /* The search's last ranking, re-run. Since phase 6b's
                     * B' the rect/AMP PU search (search_pu) is entered on
                     * flat-typed frames only, so on natural content THIS
                     * is where the AMVP vector is settled and where the
                     * quarter-pel step has to be taken. It is a choice
                     * among five positions, not a search: the ladder
                     * inside n265_me_search stays luma. Hadamard on both
                     * sides even though the arbitration above ranks in
                     * SAD - SAD cannot tell an interpolated phase's blur
                     * from a misalignment, which is the whole reason the
                     * subpel round is SATD-costed in mv.c.
                     *
                     * Two knobs reach this block and they are
                     * independent. `N265_CHROMA_PRED >= 2` (phase 7) puts
                     * the CHROMA cost of each position into the sum;
                     * `N265_SUBPEL_RATE == 1` (phase 10 item 1) is the
                     * same five positions on luma SATD + lambda x mvd
                     * alone. The chroma item measured the second by
                     * weighting the first's chroma term to zero
                     * (`N265_CPRED_W=0`) and found it the larger of the
                     * two; splitting it out is what makes it priceable
                     * on a preset that does not run the chroma arm. */
                    int cpr = chroma_pred() >= 2;
                    for (int l = 0; l < 2; l++) {
                        if (!(amot.pred & (1 << l)))
                            continue;
                        /* the ladder's Hadamards describe this centre
                         * only while the settled vector is still the uni
                         * winner it returned on this list; a bi centre
                         * is the AVERAGE of two predictions and none of
                         * them is its SATD. The scale the gate reads is
                         * that list's own ladder SATD either way. */
                        int uni = amot.pred == (1u << l) &&
                            amot.mv[l].x == me_mv[l].x &&
                            amot.mv[l].y == me_mv[l].y &&
                            keep[l].satd != UINT32_MAX;
                        if (rung_owns(&keep[l], uni, cpr))
                            continue;
                        uint32_t bq;
                        amot.mv[l] = subpel_rerank(e, &pg, &amot, mvp, l, lam4,
                                                   cpr,
                                                   want_keep ? &keep[l] : NULL,
                                                   uni, UINT32_MAX, &bq);
                        (void)bq;   /* this site re-costs the CU below */
                    }
                }
                same_as_cand = amot.pred == cand.pred &&
                    (!(amot.pred & 1) ||
                     (amot.mv[0].x == cand.mv[0].x &&
                      amot.mv[0].y == cand.mv[0].y && amot.ref[0] == cand.ref[0])) &&
                    (!(amot.pred & 2) ||
                     (amot.mv[1].x == cand.mv[1].x &&
                      amot.mv[1].y == cand.mv[1].y && amot.ref[1] == cand.ref[1]));
            }
            if (!gate_amvp && !same_as_cand && !no_amvp_on()) {
                tb_analyze(e, 0, x0, y0, log2n, e->cur_ctu_qp, MODE_INTER, amot,
                           a_tl);
                tb_analyze(e, 1, cx0, cy0, log2c, qp_c, MODE_INTER, amot,
                           a_tcb);
                tb_analyze(e, 2, cx0, cy0, log2c, qp_c, MODE_INTER, amot,
                           a_tcr);
                tb_make_recon(e, 0, log2n, e->cur_ctu_qp, MODE_INTER, a_tl);
                tb_make_recon(e, 1, log2c, qp_c, MODE_INTER, a_tcb);
                tb_make_recon(e, 2, log2c, qp_c, MODE_INTER, a_tcr);
                a_any = a_tl->nz || tb_any(a_tcb) || tb_any(a_tcr);
                uint64_t d = tb_ssd(e, 0, x0, y0, log2n, a_tl) +
                    tb_ssd(e, 1, cx0, cy0, log2c, a_tcb) +
                    tb_ssd(e, 2, cx0, cy0, log2c, a_tcr);
                d_amvp = d;
                /* the vector actually coded, not the per-list ME winner:
                 * the two are the same only until a bi refinement moves
                 * one of them, and a predictor picked for the other
                 * vector would make the decoder rebuild a different MV */
                for (int l = 0; l < 2; l++) {
                    if (!(amot.pred & (1 << l)))
                        continue;
                    mvp_idx[l] = mvp_pick(amot.mv[l], mvp[l]);
                }
                pd[1].mot = amot;
                pd[1].merge = 0;
                pd[1].midx = 0;
                for (int l = 0; l < 2; l++) {
                    pd[1].mvp_idx[l] = mvp_idx[l];
                    pd[1].mvp[l][0] = mvp[l][0];
                    pd[1].mvp[l][1] = mvp[l][1];
                }
                memcpy(local, e->est_ctx, sizeof(local));
                st.ctx = local; st.bits = 0;
                est_inter_cu_bits(e, &st, x0, y0, log2n, PART_2Nx2N, &pd[1],
                                  a_any, inc);
                if (a_any)
                    est_tt_bits(e, &st, log2n, MODE_INTER, a_tl, a_tcb,
                                a_tcr);
                j_amvp = rd_j(e, d, st.bits, lam);
                /* P1 CBF fast, second stage: the searched vector's
                 * residual quantised away too */
                if (cbf_fast() >= 2 && !a_any)
                    gate_intra = gate_part = 1;
            }
            if (cr) {
                uint64_t t = cu_tick();
                cr->cyc_amvp = (uint32_t)(t - cu_ta);
                cu_ta = t;
                e->cu_arm = CU_ARM_INTRA;
            }

            /* P3: what the inter arms already proved decides whether
             * the intra arm is worth its 35-mode rank and its
             * three-component RD trial. Never gated where the lookahead
             * calls the frame intra-heavy - sc_ratio at or above 0.6,
             * just under the 0.75 scenecut bar - because that is the
             * content whose intra CUs are the coding. */
            /* modes 6+ and the oracle carry the floor disjunct past the
             * arm gate to the trial gate below, where the 35-mode rank
             * has reported its own evidence. */
            int floor_only = 0, or_on = 0, or_d1 = 0, or_iwin = 0;
            uint64_t or_dinter = 0, or_thr = 0, or_jinter = 0;
            uint64_t iw_jinter = 0;
            /* ...and not under --lossless: every disjunct below is an
             * argument about the QUANTISER FLOOR - the winning inter
             * candidate's distortion already being inside qstep^2, so
             * intra has no distortion left to buy. A bypass CU has no
             * quantiser and no floor: its distortion is zero because it
             * codes the residual, not because the residual was small, and
             * the gate would fire on every CU of every B slice and delete
             * the intra arm from the encoder. */
            if (intra_gate() && !gate_intra && !e->lossless &&
                e->sc_ratio < 0.6f) {
                uint64_t j_inter = j_skip, d_inter = d_skip;
                int iwin = 0;
                if (j_merge < j_inter) { j_inter = j_merge; d_inter = d_merge; iwin = 1; }
                if (j_amvp < j_inter) { j_inter = j_amvp; d_inter = d_amvp; iwin = 2; }
                int g = 0;
                uint64_t thr = skip_clean_thr(e->cur_ctu_qp, n);
                /* the 2Nx2N winner is the skip: the neighbours' motion
                 * explains the block with no residual at all, and an
                 * intra CU there codes a prediction mode and a residual
                 * to say the same thing */
                int d1 = (j_inter == j_skip);
                /* the winning inter candidate's distortion is already at
                 * the quantiser floor (per-pixel MSE within qstep^2), so
                 * intra has no distortion left to buy and can only spend
                 * bits */
                int d2 = d_inter <= 4 * thr;
                if (d1)
                    g = 1;
                if ((intra_gate() == 2 || intra_gate() == 3) && !g && d2)
                    g = 1;
                /* and the neighbourhood agrees: no intra structure is
                 * arriving from either decoded edge */
                if (intra_gate() == 3 && !g && nb_inter_clean(e, x0, y0, n))
                    g = 1;
                floor_only = d2 && !d1;
                /* modes 7 and 8 decide the B half of their gate off the
                 * slice type alone, so it can stand where mode 2's
                 * disjunct stands - before the 35-mode rank - and take
                 * the rank with it. The bitstream is the same either
                 * way: the rank's output is read only by the RD trial. */
                if ((intra_gate() == 7 || intra_gate() == 8) && !g &&
                    floor_only && e->slice_type == 0 && !e->flat)
                    g = 1;
                /* 10 (step 3): 7's population, plus the same floor
                 * population of a frame the driver's adaptive rule has
                 * armed - an anchor whose recent peers got so little J
                 * out of the intra arm that the rank is not worth its
                 * wall. `iw_gate` is decided in dispatch_frame, off
                 * RETIRED frames only, so it is a property of the frame
                 * and not of the schedule. */
                if (intra_gate() == 10 && !g && floor_only && !e->flat &&
                    (e->slice_type == 0 || e->iw_gate))
                    g = 1;
                or_iwin = iwin;
                if (e->iw_count) {
                    e->iw_arm[0]++;         /* CUs the family reached  */
                    e->iw_arm[1] += (unsigned)(g != 0);
                    e->iw_arm[4] += j_inter;
                    iw_jinter = j_inter;
                }
                if (intra_oracle()) {
                    or_on = 1;
                    or_d1 = d1;
                    or_dinter = d_inter; or_thr = thr; or_jinter = j_inter;
                    g = 0;
                }
                gate_intra = g;
            }

            /* intra candidate (2Nx2N, SAD-ranked best mode) */
            uint64_t j_intra = UINT64_MAX;
            int imode = 1;
            uint32_t isad = UINT32_MAX;
            if (!gate_intra) {
            if (e->iw_count)
                e->iw_arm[2]++;             /* 35-mode ranks run       */
            imode = search_luma_mode(e, x0, y0, log2n, e->recon[0],
                                     e->recon_stride[0],
                                     intra_gate() >= 4 || intra_oracle()
                                         ? &isad : NULL);
            /* P3 modes 4 and up: the 35-mode rank is the cheaper half of
             * the intra arm - about a third of what the arm costs on the
             * post-NEON encoder, not the quarter P3 measured - and it
             * reports its winner's prediction SAD in the same units the
             * merge candidate was ranked in. Where the best intra
             * prediction is worse than the merge one by this margin, the
             * RD trial is bought to lose. */
            if (!intra_oracle() && intra_gate() >= 4 && e->sc_ratio < 0.6f &&
                (uint64_t)isad * 8u >
                    (uint64_t)cand_plain_sad * (unsigned)intra_pf_q8())
                gate_intra = 1;
            /* modes 6-9 (phase 6b step 6): the RD trial, and only the
             * trial, on the half of mode 2's population the prefilter
             * lets through. 6 is the ceiling - every such trial - and
             * 7/8/9 are the subsets the oracle found. */
            if (!intra_oracle() && intra_gate() >= 6 && floor_only &&
                !gate_intra && e->sc_ratio < 0.6f &&
                (intra_gate() == 6 || !e->flat)) {
                int take = 1;
                if (intra_gate() == 7 || intra_gate() == 10)
                    take = 0;       /* taken above, rank included */
                else if (intra_gate() == 8)
                    take = (uint64_t)isad * 8u >=
                        (uint64_t)cand_plain_sad * 7u;
                else if (intra_gate() == 9)
                    take = e->slice_type == 0 &&
                        (uint64_t)isad * 4u >= (uint64_t)cand_plain_sad * 3u;
                if (take)
                    gate_intra = 1;
            }
            }
            if (!gate_intra) {
            if (e->iw_count)
                e->iw_arm[3]++;             /* RD trials run           */
            tb_analyze(e, 1, cx0, cy0, log2c, qp_c, imode, mot_zero, i_tcb);
            tb_analyze(e, 2, cx0, cy0, log2c, qp_c, imode, mot_zero, i_tcr);
            tb_analyze(e, 0, x0, y0, log2n, e->cur_ctu_qp, imode, mot_zero, i_tl);
            tb_make_recon(e, 0, log2n, e->cur_ctu_qp, imode, i_tl);
            tb_make_recon(e, 1, log2c, qp_c, imode, i_tcb);
            tb_make_recon(e, 2, log2c, qp_c, imode, i_tcr);
            uint64_t d_intra = tb_ssd(e, 0, x0, y0, log2n, i_tl) +
                tb_ssd(e, 1, cx0, cy0, log2c, i_tcb) +
                tb_ssd(e, 2, cx0, cy0, log2c, i_tcr);
            memcpy(local, e->est_ctx, sizeof(local));
            st.ctx = local; st.bits = 0;
            st.bits += n265_est_bin(&local[CTX_CU_SKIP + inc], 0);
            st.bits += n265_est_bin(&local[CTX_PRED_MODE], 1);
            if (log2n == MIN_CB_LOG2)
                st.bits += n265_est_bin(&local[CTX_PART_MODE], 1);
            est_mpm_bits(e, &st, x0, y0, imode);
            st.bits += n265_est_bin(&local[CTX_CHROMA_MODE], 0);
            {
                res_ops ops;
                n265_est_ops(&ops, &st);
                est_cbf_c(e, &st, 0, i_tcb);
                est_cbf_c(e, &st, 0, i_tcr);
                st.bits += n265_est_bin(&local[CTX_CBF_LUMA + 1],
                                        i_tl->nz != 0);
                if (i_tl->nz)
                    n265_residual_coding(&ops, i_tl->level, log2n, 0,
                                         n265_scan_idx(imode, log2n, 0),
                                         !e->lossless);
                int sc = c_scan_idx(e, imode, log2c);
                est_res_c(e, &ops, i_tcb, log2c, 1, sc);
                est_res_c(e, &ops, i_tcr, log2c, 2, sc);
            }
            j_intra = rd_j(e, d_intra, st.bits, lam);
            }
            if (cr) {
                uint64_t t = cu_tick();
                cr->cyc_intra = (uint32_t)(t - cu_ta);
                cu_ta = t;
                if (gate_intra)
                    cr->flags |= N265_CUF_GINTRA;
                e->cu_arm = CU_ARM_PART;
            }

            /* pick the winner among the 2Nx2N candidates */
            uint64_t best = j_skip;
            int kind = 0;   /* 0 skip, 1 merge, 2 amvp, 3 intra, 4 split PU */
            if (j_merge < best) { best = j_merge; kind = 1; }
            if (j_amvp < best) { best = j_amvp; kind = 2; }
            /* Intra admission margin in B (N265_B_INTRA_MARGIN, percent).
             * An intra CU in a referenced B costs more than its own RD
             * says: it cuts the prediction chain the frames above it in
             * the pyramid ride on, and the local J cannot see that. Make
             * it win by a margin instead of by a hair. Zero = the plain
             * comparison. */
            uint64_t j_intra_adm = j_intra;
            {
                int mg = b_intra_margin();
                if (mg && e->slice_type == 0 && j_intra != UINT64_MAX)
                    j_intra_adm = j_intra * (uint64_t)(100 + mg) / 100;
            }
            if (j_intra_adm < best) { best = j_intra; kind = 3; }

            /* The J the intra arm SAVES in the population an adaptive
             * gate would take - the floor half a P slice keeps today -
             * against the J the inter arms would have settled for. The
             * oracle calibrated this against mode 2 at about one BD
             * point per percent of the frame's J, and it is the quantity
             * the area share of intra does not carry: a clip can code a
             * fifth of an anchor intra and win nothing by it. */
            if (e->iw_count && floor_only && kind == 3 &&
                iw_jinter > j_intra)
                e->iw_arm[5] += iw_jinter - j_intra;

            if (or_on) {
                /* IO,size,slice,codeidx,d1,iwin,d_inter,thr,isad,csad,
                 *    j_inter,j_intra,intra_won */
                fprintf(stderr,
                        "IO,%d,%d,%d,%d,%d,%llu,%llu,%u,%u,%llu,%llu,%d\n",
                        n, e->slice_type, e->code_idx, or_d1, or_iwin,
                        (unsigned long long)or_dinter,
                        (unsigned long long)or_thr,
                        isad, cand_plain_sad,
                        (unsigned long long)or_jinter,
                        (unsigned long long)j_intra, kind == 3);
            }

            /* rectangular and asymmetric partitions. Gated the way the
             * expensive candidates are: a static region (skip won, or the
             * merge prediction was already good enough to prune AMVP)
             * cannot be paying for two motion fields. Rank the two
             * rectangular splits by SATD, RD only the better; try AMP
             * only in the orientation a rectangular split already won,
             * and only above the min CB where the spec allows it. */
            part_cand *pca = &e->sc->pca, *pcb = &e->sc->pcb;
            part_cand *cur = pca, *pwin = NULL;
            /* P4: a two-PU split can only pay where the block's own
             * searched motion left something to code. Where the AMVP
             * candidate reproduced the merge vector, or its residual
             * quantised away, the neighbours' single motion field
             * already explains the block. */
            int rect_ok = 1;
            if (part_gate() >= 2)
                rect_ok = a_any;
            if (part_gate() >= 3 && log2n == MIN_CB_LOG2)
                rect_ok = 0;
            uint64_t best_2n = best;
            if (e->rect && kind != 0 && !gate_amvp && !gate_part && rect_ok) {
                mv_t start[2] = { me_mv[0], me_mv[1] };
                pu_dec dh[2], dv[2];
                /* P5: pre-rank the two orientations off the 2Nx2N
                 * winner's residual and search only the winner, unless
                 * the proxy cannot separate them by the margin. The
                 * AMVP candidate's residual is the evidence when it
                 * exists - it is the block's own best single motion, so
                 * what it leaves behind is what a second motion field
                 * would have to explain - and the merge candidate's
                 * otherwise. */
                int do_h = 1, do_v = 1;
                if (rect_rank()) {
                    uint64_t e_h, e_v;
                    rect_proxy(e, x0, y0, n,
                               j_amvp != UINT64_MAX ? a_tl->pred : m_tl->pred,
                               &e_h, &e_v);
                    unsigned mg = (unsigned)rect_rank();
                    if (e_h * 100 > e_v * (100 + mg))
                        do_v = 0;
                    else if (e_v * 100 > e_h * (100 + mg))
                        do_h = 0;
                }
                uint32_t ch = UINT32_MAX, cv = UINT32_MAX;
                if (do_h)
                    ch = try_part(e, x0, y0, n, PART_2NxN, start, dh);
                if (do_v)
                    cv = try_part(e, x0, y0, n, PART_Nx2N, start, dv);
                int horiz = ch <= cv;
                cur->part = horiz ? PART_2NxN : PART_Nx2N;
                memcpy(cur->d, horiz ? dh : dv, sizeof(cur->d));
                uint64_t jr = rd_part(e, x0, y0, log2n, inc, cur);
                if (jr < best) {
                    best = jr;
                    kind = 4;
                    pwin = cur;
                    cur = cur == pca ? pcb : pca;
                    if (e->amp && log2n > MIN_CB_LOG2 &&
                        (!part_gate() ||
                         jr * (100 + (unsigned)part_amp_margin()) <=
                             best_2n * 100)) {
                        int a0 = horiz ? PART_2NxnU : PART_nLx2N;
                        int a1 = horiz ? PART_2NxnD : PART_nRx2N;
                        pu_dec da[2], db[2];
                        uint32_t ca = try_part(e, x0, y0, n, a0, start, da);
                        uint32_t cb = try_part(e, x0, y0, n, a1, start, db);
                        cur->part = ca <= cb ? a0 : a1;
                        memcpy(cur->d, ca <= cb ? da : db, sizeof(cur->d));
                        uint64_t ja = rd_part(e, x0, y0, log2n, inc, cur);
                        if (ja < best) {
                            best = ja;
                            pwin = cur;
                        }
                    }
                }
            }

            if (cr) {
                cr->cyc_part = (uint32_t)(cu_tick() - cu_ta);
                cr->kind = (uint8_t)(kind == 0 ? N265_CUT_SKIP :
                                     kind == 1 ? N265_CUT_MERGE :
                                     kind == 2 ? N265_CUT_AMVP :
                                     kind == 3 ? N265_CUT_INTRA
                                               : N265_CUT_PART);
                /* the commit's transform tree belongs to the arm that
                 * won it */
                e->cu_arm = kind == 2 ? CU_ARM_AMVP :
                            kind == 3 ? CU_ARM_INTRA :
                            kind == 4 ? CU_ARM_PART : CU_ARM_MERGE;
            }
            if (kind == 0) {
                skip_cu(e, x0, y0, log2n, cand, midx);
                e->last_skip_clean = d_skip <= skip_clean_thr(e->cur_ctu_qp, n);
                e->last_cu_nores = 1;
            } else if (kind == 4) {
                /* the commit's own rqt_root_cbf, which is the trial's
                 * except where N265_RDOQ=2's lattice took the residual
                 * away at the commit */
                e->last_cu_nores =
                    !commit_inter_cu(e, x0, y0, log2n, pwin->part, pwin->d,
                                     pwin->any, &pwin->tl, &pwin->tcb,
                                     &pwin->tcr, pwin);
            } else if (kind == 1 || kind == 2) {
                tb_ctx *tl = kind == 1 ? m_tl : a_tl;
                tb_ctx *tcb = kind == 1 ? m_tcb : a_tcb;
                tb_ctx *tcr = kind == 1 ? m_tcr : a_tcr;
                int any = kind == 1 ? m_any : a_any;
                e->last_cu_nores =
                    !commit_inter_cu(e, x0, y0, log2n, PART_2Nx2N,
                                     kind == 1 ? &pd[0] : &pd[1], any, tl,
                                     tcb, tcr, NULL);
            }
            if (kind != 3) {
                if (cr)
                    cr->flags |= (e->last_skip_clean ? N265_CUF_CLEAN : 0) |
                        (e->last_cu_nores ? N265_CUF_NORES : 0);
                e->rec_pos = pos0;
                est_state adv = { e->est_ctx, 0 };
                n265_est_advance_cu(e, &adv, x0, y0, log2n);
                set_blk(e, e->blk_qp, x0, y0, n,
                        e->ent_qp_coded ? N265_QP_STORE(e->cur_ctu_qp) : N265_QP_INHERIT);
                return rd_j(e, cu_area_ssd(e, x0, y0, n), adv.bits,
                            ctu_lam(e, e->cur_ctu_qp));
            }
            /* intra wins: fall through to the intra section, which also
             * arbitrates NxN at min CB */
        }
    }

    /* intra */
    if (cr) {
        cr->kind = N265_CUT_INTRA;
        e->cu_arm = CU_ARM_INTRA;
    }
    set_blk(e, e->blk_intra, x0, y0, n, 1);
    set_blk(e, e->blk_skip, x0, y0, n, 0);

    int nxn = 0;
    int modes[4] = { 0, 0, 0, 0 };
    tb_ctx *pre_l = NULL, *pre_cb = NULL, *pre_cr = NULL;
    if (log2n == MIN_CB_LOG2) {
        uint32_t cost2n;
        modes[0] = search_luma_mode(e, x0, y0, log2n, e->recon[0],
                                    e->recon_stride[0], &cost2n);
        uint32_t costn = 0;
        int nmodes[4];
        /* I slices, N265_I_INTRA >= 3: the four 4x4 ranks run over the
         * 8x8 parent's own neighbourhood rather than all 35 modes. The
         * parent was just ranked at the same position off the same
         * reference samples, so its winner is the direction this block
         * is in; what a 4x4 adds is a step either side of it. */
        int sub[8], nsub = 0;
        if (e->slice_type == 2 && i_intra() >= 3)
            nsub = intra_sub_cands(e, x0, y0, modes[0], sub);
        for (int i = 0; i < 4; i++) {
            uint32_t c;
            nmodes[i] = search_luma_mode_sub(e, x0 + (i & 1) * 4,
                                             y0 + (i >> 1) * 4, log2n - 1,
                                             e->pad_plane[0],
                                             e->pad_stride[0], &c, NULL, 0,
                                             nsub ? sub : NULL, nsub);
            costn += c;
        }
        if (costn < cost2n) {
            nxn = 1;
            for (int i = 0; i < 4; i++)
                modes[i] = nmodes[i];
        }
        rec_u8(e, (unsigned)nxn);
        if (mode_hist_on()) {
            g_nxn += nxn;
            g_2nx2n_min += !nxn;
        }
    } else if (log2n <= 5) {
        modes[0] = intra_rd_pick(e, x0, y0, log2n, &pre_l, &pre_cb, &pre_cr);
    } else {
        modes[0] = search_luma_mode(e, x0, y0, 5, e->recon[0],
                                    e->recon_stride[0], NULL);
    }

    if (!nxn) {
        set_mode_map(e, x0, y0, n, modes[0]);
        tt_analyze(e, modes[0], x0, y0, log2n, pre_l, pre_cb, pre_cr);
        /* an I slice, or a node above 32x32 in an inter slice: the inter
         * arms never ran, so the whole node is the intra arm */
        if (cr && !cr->cyc_merge)
            cr->cyc_intra = (uint32_t)(cu_tick() - cu_t0);
        e->rec_pos = pos0;
        est_state adv = { e->est_ctx, 0 };
        n265_est_advance_cu(e, &adv, x0, y0, log2n);
        set_blk(e, e->blk_qp, x0, y0, n,
                e->ent_qp_coded ? N265_QP_STORE(e->cur_ctu_qp) : N265_QP_INHERIT);
        return rd_j(e, cu_area_ssd(e, x0, y0, n), adv.bits,
                    ctu_lam(e, e->cur_ctu_qp));
    }

    /* NxN, and the one place the two formats need different code rather
     * than a shifted constant.
     *
     * At 4:2:0 the four 4x4 luma TUs share ONE 4x4 chroma block at the
     * parent (7.3.8.10's blkIdx == 3 arm), with one intra_chroma_pred_mode
     * for the coding unit. At 4:4:4 each luma PB has its own chroma PB -
     * 7.3.8.5 signals FOUR intra_chroma_pred_mode, one per 4x4 - and each
     * of the four transform units codes its own 4x4 chroma pair with cbfs
     * conditioned on the node's. The root cbfs precede the children in the
     * record stream and are the OR of them, so they are patched in after
     * the loop, exactly as tt_analyze's split arm does.
     *
     * Every TB below is analysed and committed here, so the whole block is
     * commit-path work: under N265_RDOQ=2 it is the lattice's. */
    tb_ctx tcb, tcr, tl;
    e->rdoq_commit = 1;
    if (e->c444) {
        size_t root_off = e->rec_len;
        rec_u8(e, 0);
        rec_u8(e, 0);
        int root_cb = 0, root_cr = 0;
        for (int i = 0; i < 4; i++) {
            int px = x0 + (i & 1) * 4, py = y0 + (i >> 1) * 4;
            set_mode_map(e, px, py, 4, modes[i]);
            tb_analyze(e, 1, px, py, 2, qp_c, modes[i], mot_zero, &tcb);
            tb_analyze(e, 2, px, py, 2, qp_c, modes[i], mot_zero, &tcr);
            root_cb |= tcb.nz != 0;
            root_cr |= tcr.nz != 0;
            rec_u8(e, tcb.nz != 0);
            rec_u8(e, tcr.nz != 0);
            int cbf_l = tb_full(e, 0, px, py, 2, e->cur_ctu_qp, modes[i],
                                &tl);
            rec_u8(e, (unsigned)cbf_l);
            if (cbf_l)
                rec_levels(e, tl.level, 16);
            tb_recon(e, 1, px, py, 2, qp_c, modes[i], &tcb);
            if (tcb.nz)
                rec_levels(e, tcb.level, 16);
            tb_recon(e, 2, px, py, 2, qp_c, modes[i], &tcr);
            if (tcr.nz)
                rec_levels(e, tcr.level, 16);
            /* no second block here: 4:4:4 is the one format where a
             * chroma unit is a single square block per luma one */
        }
        e->rec_buf[root_off] = (uint8_t)root_cb;
        e->rec_buf[root_off + 1] = (uint8_t)root_cr;
    } else {
    /* 4:2:0 and 4:2:2: ONE intra_chroma_pred_mode for the coding unit
     * (7.3.8.5 signals four only at ChromaArrayType 3) and the chroma at
     * the parent - one 4x4 block at 4:2:0, 7.3.8.8's stacked 4x4 pair at
     * 4:2:2. The DM mode is the top-left PB's, which is modes[0], and at
     * 4:2:2 Table 8-3 remaps it inside tb_analyze. */
    tb_analyze(e, 1, cx0, cy0, 2, qp_c, modes[0], mot_zero, &tcb);
    tb_analyze(e, 2, cx0, cy0, 2, qp_c, modes[0], mot_zero, &tcr);
    rec_cbf_c(e, &tcb);
    rec_cbf_c(e, &tcr);
    for (int i = 0; i < 4; i++) {
        int px = x0 + (i & 1) * 4, py = y0 + (i >> 1) * 4;
        set_mode_map(e, px, py, 4, modes[i]);
        int cbf_l = tb_full(e, 0, px, py, 2, e->cur_ctu_qp, modes[i], &tl);
        rec_u8(e, (unsigned)cbf_l);
        if (cbf_l)
            rec_levels(e, tl.level, 16);
    }
    tb_recon(e, 1, cx0, cy0, 2, qp_c, modes[0], &tcb);
    rec_levels_c(e, &tcb, 2);
    tb_recon(e, 2, cx0, cy0, 2, qp_c, modes[0], &tcr);
    rec_levels_c(e, &tcr, 2);
    }
    e->rdoq_commit = 0;
    if (cr && !cr->cyc_merge)
        cr->cyc_intra = (uint32_t)(cu_tick() - cu_t0);
    e->rec_pos = pos0;
    est_state adv = { e->est_ctx, 0 };
    n265_est_advance_cu(e, &adv, x0, y0, log2n);
    set_blk(e, e->blk_qp, x0, y0, n,
            e->ent_qp_coded ? N265_QP_STORE(e->cur_ctu_qp) : N265_QP_INHERIT);
    return rd_j(e, cu_area_ssd(e, x0, y0, n), adv.bits,
                ctu_lam(e, e->cur_ctu_qp));
}

static int decide_split(struct n265_frame_work *e, int x0, int y0, int log2n)
{
    uint32_t parent;
    search_luma_mode(e, x0, y0, log2n > 5 ? 5 : log2n, e->pad_plane[0],
                     e->pad_stride[0], &parent);
    if (log2n > 5)
        parent *= 4;
    int h = 1 << (log2n - 1);
    uint32_t kids = 0;
    for (int i = 0; i < 4; i++) {
        uint32_t c;
        search_luma_mode(e, x0 + (i & 1) * h, y0 + (i >> 1) * h,
                         log2n - 1 > 5 ? 5 : log2n - 1, e->pad_plane[0],
                         e->pad_stride[0], &c);
        kids += c;
    }
    return kids + (kids >> 4) < parent;
}


/*
 * The snapshot's row copy, with its length made a constant (phase 13 A2).
 *
 * Every copy qt_save and qt_restore make is a run of rows whose LENGTH is
 * a power of two under 129 bytes: `cells` bytes for each of the eight
 * 4x4-cell maps, `cells * 8` for the motion vectors, `cells * 2` for the
 * reference indices, and `(n >> shx) * sizeof(pixel)` for a recon row.
 * Nothing told the compiler that, so each one was `memcpy` with a
 * run-time length - a call into `_platform_memmove` - and at a 64x64 CU
 * the map rows alone are 128 calls of SIXTEEN BYTES per save. The A2
 * census put `qt_save` and `qt_restore` together at 28-36% of the
 * encoder's memmove samples, second only to the transform's staging copy.
 *
 * The switch is outside the row loop, so it costs one well-predicted
 * branch per call and each arm copies a compile-time size that clang
 * emits in line. Identical bytes to identical addresses: this is the same
 * copy with the call removed, and the `default` arm is still the general
 * one.
 */
static inline void qt_rows(uint8_t *d, ptrdiff_t ds, const uint8_t *s,
                           ptrdiff_t ss, int rows, size_t bytes)
{
#define QT_ROWS_CASE(B)                                                 \
    case (B):                                                           \
        for (int y = 0; y < rows; y++)                                  \
            memcpy(d + (ptrdiff_t)y * ds, s + (ptrdiff_t)y * ss, (B));  \
        return;

    switch (bytes) {
    QT_ROWS_CASE(1)  QT_ROWS_CASE(2)  QT_ROWS_CASE(4)  QT_ROWS_CASE(8)
    QT_ROWS_CASE(16) QT_ROWS_CASE(32) QT_ROWS_CASE(64) QT_ROWS_CASE(128)
    default: break;
    }
#undef QT_ROWS_CASE

    for (int y = 0; y < rows; y++)
        memcpy(d + (ptrdiff_t)y * ds, s + (ptrdiff_t)y * ss, bytes);
}

static uint8_t *snap_maps(struct n265_frame_work *e, int i)
{
    uint8_t *m[8];
    m[0] = e->ct_depth; m[1] = e->intra_mode; m[2] = e->edge_map;
    m[3] = e->blk_intra; m[4] = e->blk_skip; m[5] = e->blk_cbf_y;
    m[6] = e->blk_pred; m[7] = e->blk_qp;
    return m[i];
}

static void qt_save(struct n265_frame_work *e, qt_snap *sn, int x0, int y0,
                    int n)
{
    pixel *p = sn->recon;
    for (int c = 0; c < 3; c++) {
        int shx = c ? e->csx : 0, shy = c ? e->csy : 0;
        int rows = ((y0 + n) >> shy) - (y0 >> shy);
        size_t rb = (size_t)(n >> shx) * sizeof(pixel);

        qt_rows((uint8_t *)p, (ptrdiff_t)rb,
                (const uint8_t *)(e->recon[c] +
                                  (size_t)(y0 >> shy) * e->recon_stride[c] +
                                  (x0 >> shx)),
                (ptrdiff_t)e->recon_stride[c] * (ptrdiff_t)sizeof(pixel),
                rows, rb);
        p += (size_t)rows * (n >> shx);
    }
    int cells = n >> 2;
    for (int i = 0; i < 8; i++)
        qt_rows(sn->maps[i], cells,
                snap_maps(e, i) + (size_t)(y0 >> 2) * e->map_stride +
                    (x0 >> 2),
                e->map_stride, cells, (size_t)cells);
    qt_rows((uint8_t *)sn->mv, (ptrdiff_t)cells * 4 * sizeof(int16_t),
            (const uint8_t *)(e->blk_mv +
                              ((size_t)(y0 >> 2) * e->map_stride +
                               (x0 >> 2)) * 4),
            (ptrdiff_t)e->map_stride * 4 * (ptrdiff_t)sizeof(int16_t),
            cells, (size_t)cells * 4 * sizeof(int16_t));
    qt_rows((uint8_t *)sn->ref, cells * 2,
            (const uint8_t *)(e->blk_ref +
                              ((size_t)(y0 >> 2) * e->map_stride +
                               (x0 >> 2)) * 2),
            (ptrdiff_t)e->map_stride * 2, cells, (size_t)cells * 2);
    memcpy(sn->est, e->est_ctx, sizeof(sn->est));
    sn->ent_qp_coded = e->ent_qp_coded;
    sn->rec_len = e->rec_len;
    sn->rec_copy_len = 0;
}

static void qt_save_records(struct n265_frame_work *e, qt_snap *sn,
                            size_t base)
{
    sn->rec_base = base;
    sn->rec_copy_len = e->rec_len - base;
    /* the bound above; an intra CB64 at 4:4:4 is the worst case */
    assert(sn->rec_copy_len <= sizeof(sn->rec_copy));
    memcpy(sn->rec_copy, e->rec_buf + base, sn->rec_copy_len);
}

static void qt_restore(struct n265_frame_work *e, const qt_snap *sn, int x0,
                       int y0, int n)
{
    const pixel *p = sn->recon;
    for (int c = 0; c < 3; c++) {
        int shx = c ? e->csx : 0, shy = c ? e->csy : 0;
        int rows = ((y0 + n) >> shy) - (y0 >> shy);
        size_t rb = (size_t)(n >> shx) * sizeof(pixel);

        qt_rows((uint8_t *)(e->recon[c] +
                            (size_t)(y0 >> shy) * e->recon_stride[c] +
                            (x0 >> shx)),
                (ptrdiff_t)e->recon_stride[c] * (ptrdiff_t)sizeof(pixel),
                (const uint8_t *)p, (ptrdiff_t)rb, rows, rb);
        p += (size_t)rows * (n >> shx);
    }
    int cells = n >> 2;
    for (int i = 0; i < 8; i++)
        qt_rows(snap_maps(e, i) + (size_t)(y0 >> 2) * e->map_stride +
                    (x0 >> 2),
                e->map_stride, sn->maps[i], cells, cells, (size_t)cells);
    qt_rows((uint8_t *)(e->blk_mv +
                        ((size_t)(y0 >> 2) * e->map_stride + (x0 >> 2)) * 4),
            (ptrdiff_t)e->map_stride * 4 * (ptrdiff_t)sizeof(int16_t),
            (const uint8_t *)sn->mv,
            (ptrdiff_t)cells * 4 * sizeof(int16_t), cells,
            (size_t)cells * 4 * sizeof(int16_t));
    qt_rows((uint8_t *)(e->blk_ref +
                        ((size_t)(y0 >> 2) * e->map_stride + (x0 >> 2)) * 2),
            (ptrdiff_t)e->map_stride * 2, (const uint8_t *)sn->ref,
            cells * 2, cells, (size_t)cells * 2);
    memcpy(e->est_ctx, sn->est, sizeof(sn->est));
    e->ent_qp_coded = sn->ent_qp_coded;
    e->rec_len = sn->rec_len;
    if (sn->rec_copy_len)
        memcpy(e->rec_buf + sn->rec_base, sn->rec_copy, sn->rec_copy_len);
}

static uint64_t quadtree(struct n265_frame_work *e, int x0, int y0, int log2n,
                         int depth);

/* N265_DUP_FAST (phase 11 item 1, docs/phase11.md): the whole of the arm.
 *
 * This frame's source is the source of the references in `e->dup_msk`,
 * which the driver resolved at dispatch, so the picture the decoder
 * should end up with is the one those references already hold: every CU
 * skip, the zero vector, no residual, no split. There is nothing here to
 * search and nothing to arbitrate - a split can only re-buy the same
 * prediction with four sets of syntax, an explicit vector can only spell
 * the zero one at greater length, and intra can only code from scratch
 * what a copy already has.
 *
 * The candidate taken is the FIRST in the merge list that predicts this
 * picture from copies of itself: a zero vector on every list it uses,
 * and every one of those lists pointing inside the duplicate set. That
 * admits the bi candidate where both sides are duplicates, which is not
 * a concession - averaging two independent quantisations of one source
 * is a better prediction than either, and it is what the encoder's own
 * RD picks on a long hold. Leaving it out made the arm decline every CU
 * of `sita_flat_720p` (docs/phase11.md sec 1.4). The ORDER is
 * 8.5.3.2's, so the candidate is whatever the neighbourhood is already
 * using, and on a picture coded this way throughout the search ends at
 * index 0.
 *
 * What it may not do is invent syntax: a skip carries a merge index and
 * nothing else, so where no candidate qualifies - the first CU of a
 * B picture, which has no spatial neighbour and whose zero fill is bi
 * toward references that may not both be duplicates - the node is handed
 * back to the full analysis rather than coded as something it is not.
 *
 * Returns 0 when it declined, leaving every encoder state untouched. */
static int dup_commit(struct n265_frame_work *e, int x0, int y0, int log2n,
                      int depth, int split_inc, uint64_t *j_out)
{
    int n = 1 << log2n;
    mot_t mlist[MAX_MERGE];
    int k = n265_merge_list(e, x0, y0, n, n, mlist, MAX_MERGE);
    int midx = -1;
    for (int i = 0; i < k && midx < 0; i++) {
        int ok = mlist[i].pred != 0;
        for (int l = 0; l < 2 && ok; l++) {
            if (!(mlist[i].pred & (1 << l)))
                continue;
            int r = mlist[i].ref[l];
            ok = r >= 0 && ((e->dup_msk[l] >> r) & 1) &&
                 mlist[i].mv[l].x == 0 && mlist[i].mv[l].y == 0;
        }
        if (ok)
            midx = i;
    }
    if (midx < 0)
        return 0;
    uint32_t lam = ctu_lam(e, e->cur_ctu_qp);
    /* the split flag this CU does not set, priced (and its context
     * advanced) exactly where the no-split arms price it */
    uint32_t b0 = log2n > MIN_CB_LOG2
                      ? n265_est_bin(&e->est_ctx[CTX_SPLIT_CU + split_inc], 0)
                      : 0;
    size_t pos0 = e->rec_len;
    mark_cu_edges(e, x0, y0, n);
    skip_cu(e, x0, y0, log2n, mlist[midx], midx);
    e->rec_pos = pos0;
    est_state adv = { e->est_ctx, 0 };
    n265_est_advance_cu(e, &adv, x0, y0, log2n);
    set_blk(e, e->blk_qp, x0, y0, n,
            e->ent_qp_coded ? N265_QP_STORE(e->cur_ctu_qp) : N265_QP_INHERIT);
    set_blk(e, e->ct_depth, x0, y0, n, depth);
    uint64_t ssd = cu_area_ssd(e, x0, y0, n);
    e->last_cu_nores = 1;
    e->last_skip_clean = ssd <= skip_clean_thr(e->cur_ctu_qp, n);
    *j_out = rd_j(e, ssd, adv.bits + b0, lam);
    return 1;
}

/* RD quadtree: at each inside node try no-split (one CU) and split (four
 * children, recursively), keep the cheaper tree. Returns the committed
 * subtree's J. Boundary nodes recurse without a flag. */
static uint64_t quadtree_inner(struct n265_frame_work *e, int x0, int y0,
                               int log2n, int depth)
{
    int n = 1 << log2n;
    int inside = x0 + n <= e->pad_w && y0 + n <= e->pad_h;
    uint32_t lam = ctu_lam(e, e->cur_ctu_qp);
    struct n265_cu_rec *cr = CU_CUR(e);

    if (!inside && log2n > MIN_CB_LOG2) {
        uint64_t j = 0;
        int h = n >> 1;
        if (cr)
            cr->flags |= N265_CUF_SPLIT;
        for (int i = 0; i < 4; i++) {
            int x1 = x0 + (i & 1) * h, y1 = y0 + (i >> 1) * h;
            if (x1 < e->pad_w && y1 < e->pad_h)
                j += quadtree(e, x1, y1, log2n - 1, depth + 1);
        }
        return j;
    }
    if (!inside)
        return 0;

    int split_inc = 0;
    if (x0 > 0)
        split_inc += e->ct_depth[(y0 >> 2) * e->map_stride +
                                 ((x0 - 1) >> 2)] > depth;
    if (y0 > 0)
        split_inc += e->ct_depth[((y0 - 1) >> 2) * e->map_stride +
                                 (x0 >> 2)] > depth;

    /* the duplicate-frame path: this whole picture is already in the
     * DPB, so the node is one skip and the recursion ends here */
    if ((e->dup_msk[0] | e->dup_msk[1]) && n265_dup_fast()) {
        uint64_t j;
        if (dup_commit(e, x0, y0, log2n, depth, split_inc, &j)) {
            if (cr) {
                cr->kind = N265_CUT_SKIP;
                cr->flags |= N265_CUF_STATIC | N265_CUF_NORES;
            }
            return j;
        }
    }

    if (log2n == MIN_CB_LOG2) {
        uint64_t j = analyze_cu(e, x0, y0, log2n);
        set_blk(e, e->ct_depth, x0, y0, n, depth);
        return j;
    }

    /* strict-skip regions never split: try only the CU */
    if (want_skip(e, x0, y0, log2n)) {
        uint32_t b = n265_est_bin(&e->est_ctx[CTX_SPLIT_CU + split_inc], 0);
        uint64_t j = analyze_cu(e, x0, y0, log2n) + ((uint64_t)b * lam >> 16);
        set_blk(e, e->ct_depth, x0, y0, n, depth);
        return j;
    }

    /* P2: this node is above the depth range the neighbourhood used, so
     * the no-split arm (and, at the CTB, its CB64 arms) never wins.
     * Recurse straight away. Modes 2 and 4 keep one arm here: the
     * whole-CTU prediction-only skip, which is cheap and is the one
     * thing a neighbourhood bound gets wrong when a moving picture
     * goes still. */
    if (depth_bound() && depth < e->db_lo) {
        qt_snap *fpre = &e->sc->qt_fpre, *fwon = &e->sc->qt_fwon;
        uint64_t j_cu2 = UINT64_MAX;
        size_t rec_base2 = e->rec_len;
        if (depth_bound_ctb_skip() && log2n == e->ctb_log2 &&
            e->slice_type != 2 && e->ref_valid) {
            qt_save(e, fpre, x0, y0, n);
            uint32_t b0 = n265_est_bin(&e->est_ctx[CTX_SPLIT_CU + split_inc],
                                       0);
            mot_t mlist[MAX_MERGE];
            n265_merge_list(e, x0, y0, n, n, mlist, MAX_MERGE);
            uint32_t sd0;      /* unused here; the pick is the point */
            int midx = merge_pick_sad(e, x0, y0, log2n, mlist, &sd0);
            (void)sd0;
            size_t pos0 = e->rec_len;
            mark_cu_edges(e, x0, y0, n);
            skip_cu(e, x0, y0, log2n, mlist[midx], midx);
            e->rec_pos = pos0;
            est_state adv = { e->est_ctx, 0 };
            n265_est_advance_cu(e, &adv, x0, y0, log2n);
            set_blk(e, e->blk_qp, x0, y0, n,
                    e->ent_qp_coded ? N265_QP_STORE(e->cur_ctu_qp) : N265_QP_INHERIT);
            set_blk(e, e->ct_depth, x0, y0, n, depth);
            j_cu2 = rd_j(e, cu_area_ssd(e, x0, y0, n), adv.bits + b0, lam);
            qt_save(e, fwon, x0, y0, n);
            qt_save_records(e, fwon, rec_base2);
            qt_restore(e, fpre, x0, y0, n);
        }
        uint32_t b1 = n265_est_bin(&e->est_ctx[CTX_SPLIT_CU + split_inc], 1);
        uint64_t js = (uint64_t)b1 * lam >> 16;
        int hh = n >> 1;
        for (int i = 0; i < 4; i++)
            js += quadtree(e, x0 + (i & 1) * hh, y0 + (i >> 1) * hh,
                           log2n - 1, depth + 1);
        if (j_cu2 <= js) {
            qt_restore(e, fwon, x0, y0, n);
            /* the CTB-skip arm of the depth bound: a prediction-only
             * whole-CTU skip that never entered analyze_cu */
            if (cr) {
                cr->kind = N265_CUT_SKIP;
                cr->flags |= N265_CUF_STATIC | N265_CUF_NORES;
            }
            return j_cu2;
        }
        if (cr)
            cr->flags |= N265_CUF_SPLIT;
        return js;
    }

    if (no_split_on()) {
        uint32_t b0s = n265_est_bin(&e->est_ctx[CTX_SPLIT_CU + split_inc], 0);
        uint64_t j = analyze_cu(e, x0, y0, log2n) +
                     ((uint64_t)b0s * lam >> 16);
        set_blk(e, e->ct_depth, x0, y0, n, depth);
        return j;
    }

    qt_snap *sp = &e->sc->qt_pre[depth], *sw = &e->sc->qt_won[depth];

    qt_save(e, sp, x0, y0, n);

    /* arm 1: no split */
    size_t rec_base = e->rec_len;
    uint32_t b0 = n265_est_bin(&e->est_ctx[CTX_SPLIT_CU + split_inc], 0);
    uint64_t j_cu = analyze_cu(e, x0, y0, log2n) + ((uint64_t)b0 * lam >> 16);
    set_blk(e, e->ct_depth, x0, y0, n, depth);
    /* early skip: a CLEAN whole-CU skip ends the trial (children could
     * only re-buy the same prediction with more syntax) */
    if (e->last_skip_clean && !no_earlyskip_on())
        return j_cu;
    /* P1 rskip: the no-split arm left no residual at all, so the four
     * children can only re-buy the same prediction with four sets of
     * syntax. Mode 1 also asks that neither decoded neighbour went
     * deeper than this node - where they did, this block is on a real
     * boundary and the split is worth trying anyway. Above 32 the CU
     * arm is intra-or-strict-skip and the CB64 arms below have not run,
     * so the rule stays where the full candidate set decided. */
    if (rskip_mode() && log2n <= 5 && e->last_cu_nores &&
        (rskip_mode() >= 2 || !split_inc))
        return j_cu;

    /* 64x64 whole-CU skip: prediction-only, so it is legal where the
     * flat-TB inter candidates are not (max TB 32). RD it against the
     * intra/static outcome of arm 1 before trying the split. */
    if (log2n == 6 && e->slice_type != 2) {
        qt_snap *ss = &e->sc->qt_won[depth];    /* reuse after the compare */
        qt_save(e, ss, x0, y0, n);
        qt_save_records(e, ss, rec_base);
        qt_restore(e, sp, x0, y0, n);
        e->rec_len = rec_base;
        /* the restore rewound the split-flag context mutation arm 1
         * made; re-apply it so analysis tracks the entropy pass */
        (void)n265_est_bin(&e->est_ctx[CTX_SPLIT_CU + split_inc], 0);

        mot_t mlist[MAX_MERGE];
        n265_merge_list(e, x0, y0, n, n, mlist, MAX_MERGE);
        uint32_t sd0;          /* unused here; the pick is the point */
        int midx = merge_pick_sad(e, x0, y0, log2n, mlist, &sd0);
        (void)sd0;
        size_t pos0 = e->rec_len;
        mark_cu_edges(e, x0, y0, n);
        skip_cu(e, x0, y0, log2n, mlist[midx], midx);
        e->rec_pos = pos0;
        est_state adv = { e->est_ctx, 0 };
        n265_est_advance_cu(e, &adv, x0, y0, log2n);
        set_blk(e, e->blk_qp, x0, y0, n,
                e->ent_qp_coded ? N265_QP_STORE(e->cur_ctu_qp) : N265_QP_INHERIT);
        set_blk(e, e->ct_depth, x0, y0, n, depth);
        uint64_t j_skip = rd_j(e, cu_area_ssd(e, x0, y0, n),
                               adv.bits + b0, lam);
        uint64_t j_arm1 = j_cu;         /* R3 reads how far the merge
                                         * arms improve on the CU arm */
        if (j_skip < j_cu) {
            if (cb64_trace_on())
                fprintf(stderr, "cb64skip poc=%d ctu=(%d,%d) midx=%d pred=%d mv0=(%d,%d) mv1=(%d,%d)\n",
                        e->poc, x0 >> 6, y0 >> 6, midx, mlist[midx].pred,
                        mlist[midx].mv[0].x, mlist[midx].mv[0].y,
                        mlist[midx].mv[1].x, mlist[midx].mv[1].y);
            j_cu = j_skip;      /* current state is the skip */
        } else {
            qt_restore(e, ss, x0, y0, n);   /* back to arm-1 outcome */
        }

        /* merge with residual (forced 4x TU32 tree) */
        qt_save(e, ss, x0, y0, n);
        qt_save_records(e, ss, rec_base);
        qt_restore(e, sp, x0, y0, n);
        (void)n265_est_bin(&e->est_ctx[CTX_SPLIT_CU + split_inc], 0);
        pu_dec pdm;
        memset(&pdm, 0, sizeof(pdm));
        pdm.mot = mlist[midx];
        pdm.merge = 1;
        pdm.midx = midx;
        commit_inter_cu(e, x0, y0, log2n, PART_2Nx2N, &pdm, 1, NULL, NULL,
                        NULL, NULL);
        e->rec_pos = pos0;
        est_state madv = { e->est_ctx, 0 };
        n265_est_advance_cu(e, &madv, x0, y0, log2n);
        set_blk(e, e->blk_qp, x0, y0, n,
                e->ent_qp_coded ? N265_QP_STORE(e->cur_ctu_qp) : N265_QP_INHERIT);
        set_blk(e, e->ct_depth, x0, y0, n, depth);
        uint64_t j_m = rd_j(e, cu_area_ssd(e, x0, y0, n), madv.bits + b0,
                            lam);
        if (j_m < j_cu)
            j_cu = j_m;
        else
            qt_restore(e, ss, x0, y0, n);

        /* AMVP 2Nx2N at 64x64 (N265_CB64_AMVP). The CB64 node had merge
         * and skip only, so a 64x64 region whose motion is not in the
         * merge list had to be split into four 32x32 CUs and pay four
         * motion fields plus the split flags for one. The transform is
         * the same forced TU32 quad the merge arm uses. Entered only
         * where the PU search prefers AMVP to merge - where merge wins
         * the arm above already has it. */
        int amvp_gated = 0;
        if (cb64_gate() == 3) {
            amvp_gated = la_inter_pct(e, x0, y0, n) >= 70;
        } else if (cb64_gate() && j_arm1 && j_arm1 != UINT64_MAX) {
            int imp = (int)(100 - (uint64_t)j_cu * 100 / j_arm1);
            amvp_gated = imp >= (cb64_gate() >= 2 ? 75 : 60);
        }
        if (cb64_amvp() && !amvp_gated) {
            qt_save(e, ss, x0, y0, n);
            qt_save_records(e, ss, rec_base);
            qt_restore(e, sp, x0, y0, n);
            (void)n265_est_bin(&e->est_ctx[CTX_SPLIT_CU + split_inc], 0);
            pu_geo g = { x0, y0, n, x0, y0, n, n, 0, 0 };
            pu_dec pda;
            memset(&pda, 0, sizeof(pda));
            (void)search_pu(e, &g, NULL, &pda);
            uint64_t j_a = UINT64_MAX;
            if (!pda.merge) {
                commit_inter_cu(e, x0, y0, log2n, PART_2Nx2N, &pda, 1, NULL,
                                NULL, NULL, NULL);
                e->rec_pos = pos0;
                est_state aadv = { e->est_ctx, 0 };
                n265_est_advance_cu(e, &aadv, x0, y0, log2n);
                set_blk(e, e->blk_qp, x0, y0, n,
                        e->ent_qp_coded ? N265_QP_STORE(e->cur_ctu_qp) : N265_QP_INHERIT);
                set_blk(e, e->ct_depth, x0, y0, n, depth);
                j_a = rd_j(e, cu_area_ssd(e, x0, y0, n), aadv.bits + b0, lam);
            }
            if (j_a < j_cu)
                j_cu = j_a;
            else
                qt_restore(e, ss, x0, y0, n);
        }
    }

    /* P2: the neighbourhood never went below this depth, so the split
     * arm is not worth its four candidate sets. The no-split arm has
     * run and its state stands; its J already carries the split flag. */
    if (depth_bound() && !depth_bound_local() && depth >= e->db_hi)
        return j_cu;
    if (depth_bound_local()) {
        int hi = nb_depth_max(e, x0, y0, n);
        if (hi >= 0 && depth >= hi + (depth_bound() == 5))
            return j_cu;
    }

    qt_save(e, sw, x0, y0, n);
    qt_save_records(e, sw, rec_base);

    /* arm 2: split */
    qt_restore(e, sp, x0, y0, n);
    uint32_t b1 = n265_est_bin(&e->est_ctx[CTX_SPLIT_CU + split_inc], 1);
    uint64_t j_split = (uint64_t)b1 * lam >> 16;
    int h = n >> 1;
    for (int i = 0; i < 4; i++)
        j_split += quadtree(e, x0 + (i & 1) * h, y0 + (i >> 1) * h,
                            log2n - 1, depth + 1);

    if (j_cu <= j_split) {
        qt_restore(e, sw, x0, y0, n);
        return j_cu;
    }
    if (cr)
        cr->flags |= N265_CUF_SPLIT;
    return j_split;
}

/* N265_CU_TRACE's node frame: one record per visited node, opened before
 * the node's work and closed after it, so `cyc_tot` is the subtree and
 * `cyc_kids` the part of it the split arm's children took. Off, this is
 * one predicted branch per node. */
static uint64_t quadtree(struct n265_frame_work *e, int x0, int y0, int log2n,
                         int depth)
{
    if (!e->cu_rec)
        return quadtree_inner(e, x0, y0, log2n, depth);
    struct n265_cu_rec *r = cu_open(e, x0, y0, log2n, depth);
    uint64_t t0 = cu_tick();
    uint64_t j = quadtree_inner(e, x0, y0, log2n, depth);
    cu_close(e, r, t0);
    return j;
}

/* P2: the depth range this CTU's quadtree may search, from the decoded
 * spatial neighbours (left, above, above-left, above-right) and, in an
 * inter slice, the collocated CTU of the nearest reference, widened by
 * one each way. With nothing decided yet the range is the whole tree. */
static void depth_bounds(struct n265_frame_work *e, int ctb_x, int ctb_y)
{
    int maxd = e->ctb_log2 - MIN_CB_LOG2;
    e->db_lo = 0;
    e->db_hi = maxd;
    if (!depth_bound() || depth_bound_local())
        return;
    static const int dx[4] = { -1, 0, -1, 1 };
    static const int dy[4] = { 0, -1, -1, -1 };
    int lo = maxd, hi = 0, have = 0;
    for (int i = 0; i < 4; i++) {
        int nx = ctb_x + dx[i], ny = ctb_y + dy[i];
        if (nx < 0 || ny < 0 || nx >= e->ctb_cols || ny >= e->ctb_rows)
            continue;
        int k = ny * e->ctb_cols + nx;
        if (e->ctu_dmin[k] == 0xff)
            continue;
        if (e->ctu_dmin[k] < lo) lo = e->ctu_dmin[k];
        if (e->ctu_dmax[k] > hi) hi = e->ctu_dmax[k];
        have = 1;
    }
    if (e->slice_type != 2 && e->ref_valid) {
        const struct n265_refpic *near = NULL;
        for (int l = 0; l < 2; l++)
            for (int r = 0; r < e->nref[l]; r++) {
                const struct n265_refpic *p = e->refl[l][r];
                if (!p || !p->ctu_dmin)
                    continue;
                /* p->poc is a live DPB slot's scalar */
                N265_SIDE_CHECK(e, p, e->ref_gen[l][r], -1, "depth near poc");
                if (!near || abs(p->poc - e->poc) < abs(near->poc - e->poc))
                    near = p;
            }
        int k = ctb_y * e->ctb_cols + ctb_x;
        if (near)       /* G2 entry point: the depth map publishes per row */
            n265_ref_wait(e, near, ctb_y << e->ctb_log2);
        if (near)
            N265_SIDE_CHECK(e, near, n265_gen_of(e, near), ctb_y,
                            "P2 depth map");
        if (near && near->ctu_dmin[k] != 0xff) {
            if (near->ctu_dmin[k] < lo) lo = near->ctu_dmin[k];
            if (near->ctu_dmax[k] > hi) hi = near->ctu_dmax[k];
            have = 1;
        }
    }
    if (!have)
        return;
    int w = depth_bound_widen();
    e->db_lo = lo > w ? lo - w : 0;
    e->db_hi = hi + w < maxd ? hi + w : maxd;
}

/* N265_DEPTH_DUMP: this CTU's pre-decision features, into its own record
 * slot. Every term comes off the lookahead's lowres maps, the frame's own
 * source luma or the AQ field, all frozen before the frame launched, so
 * the record is the same at every thread count and nothing here reads a
 * decided value. Called after the quadtree only because that is where the
 * target is known; the features do not depend on it.
 *
 * The variance and `fine` are recomputed here rather than read off the AQ
 * pass: that pass keeps `lv[]` on its own stack, its content depends on
 * N265_AQ_SIGNAL and N265_AQ_FLOOR, and it does not run at all with
 * `--aq 0`. Recomputing costs one pass over the CTU's luma and is paid
 * only when the instrument is on. The formulas are encoder.c's. */
static void depth_dump_features(struct n265_frame_work *e, int ctb_x,
                                int ctb_y)
{
    struct n265_depth_rec *r = &e->dd_rec[ctb_y * e->ctb_cols + ctb_x];
    int ctb = 1 << e->ctb_log2;
    int x0 = ctb_x << e->ctb_log2, y0 = ctb_y << e->ctb_log2;
    int w = x0 + ctb > e->pad_w ? e->pad_w - x0 : ctb;
    int h = y0 + ctb > e->pad_h ? e->pad_h - y0 : ctb;

    r->qp = (int8_t)e->cur_ctu_qp;
    r->ctqp = e->ctu_qp_raw ? e->ctu_qp_raw[ctb_y * e->ctb_cols + ctb_x] : 0;

    /* full-resolution variance, and the part of it the 2x-down plane does
     * not explain (`fine`, phase 7 M1') */
    int ps = e->pad_stride[0];
    int64_t s1 = 0, s2 = 0;
    for (int y = y0; y < y0 + h; y++)
        for (int x = x0; x < x0 + w; x++) {
            int v = e->pad_plane[0][(size_t)y * ps + x];
            s1 += v;
            s2 += (int64_t)v * v;
        }
    int np = w * h;
    double var = np ? (double)s2 / np - ((double)s1 / np) * ((double)s1 / np)
                    : 0.0;
    if (var < 0.0) var = 0.0;
    double fine = var;
    int hw = w >> 1, hh = h >> 1;
    if (hw > 0 && hh > 0) {
        int64_t t1 = 0, t2 = 0;
        for (int y = 0; y < hh; y++) {
            const pixel *p = e->pad_plane[0] + (size_t)(y0 + 2 * y) * ps + x0;
            for (int x = 0; x < hw; x++) {
                int d = (p[2 * x] + p[2 * x + 1] +
                         p[2 * x + ps] + p[2 * x + ps + 1] + 2) >> 2;
                t1 += d;
                t2 += (int64_t)d * d;
            }
        }
        int n2 = hw * hh;
        double v2 = (double)t2 / n2 - ((double)t1 / n2) * ((double)t1 / n2);
        fine -= v2 < 0.0 ? 0.0 : v2;
        if (fine < 0.0) fine = 0.0;
    }
    /* the +1 is ONE SQUARED 8-BIT LEVEL, as it is in encoder.c, so it
     * scales with the variance it floors: 1 at 8 bits, 16 at Main10.
     * Left at 1.0 the column read +4 against the 8-bit census of the
     * same content on every flat CTU - which is exactly the population
     * a flat gate fitted off this dump has to separate. Output-neutral;
     * N265_DEPTH_DUMP is the only reader. */
    r->lvar = (float)log2(var + (double)(1 << (2 * BIT_DEPTH_SHIFT)));
    r->fine = (float)log2(fine + (double)(1 << (2 * BIT_DEPTH_SHIFT)));

    /* the lowres 8x8 blocks under the CTU - 16 of them at CTB 64, since a
     * lowres block is 16 full-res pixels at any resolution */
    r->imean = r->imax = r->pmean = r->pmax = 0;
    r->ratio = -1.0f;
    r->mvmean = r->mvspread = 0.0f;
    if (e->la_icost && e->la_bw) {
        int b0x = x0 >> 4, b0y = y0 >> 4, nb = ctb >> 4;
        uint64_t ic = 0, pc = 0;
        unsigned imax = 0, pmax = 0, cnt = 0;
        uint64_t mvs = 0;
        unsigned mvlo = ~0u, mvhi = 0;
        for (int by = b0y; by < b0y + nb && by < e->la_bh; by++)
            for (int bx = b0x; bx < b0x + nb && bx < e->la_bw; bx++) {
                int b = by * e->la_bw + bx;
                unsigned i = e->la_icost[b], p = e->la_pcost[b];
                ic += i; pc += p;
                if (i > imax) imax = i;
                if (p > pmax) pmax = p;
                if (e->la_mv) {
                    int mx = e->la_mv[2 * b], my = e->la_mv[2 * b + 1];
                    unsigned m = (unsigned)(abs(mx) + abs(my));
                    mvs += m;
                    if (m < mvlo) mvlo = m;
                    if (m > mvhi) mvhi = m;
                }
                cnt++;
            }
        if (cnt) {
            uint64_t im = ic / cnt, pm = pc / cnt;
            r->imean = (uint16_t)(im > 65535 ? 65535 : im);
            r->pmean = (uint16_t)(pm > 65535 ? 65535 : pm);
            r->imax = (uint16_t)(imax > 65535 ? 65535 : imax);
            r->pmax = (uint16_t)(pmax > 65535 ? 65535 : pmax);
            if (ic)
                r->ratio = (float)((double)pc / (double)ic);
            if (e->la_mv) {
                r->mvmean = (float)((double)mvs / cnt);
                r->mvspread = (float)(mvhi - (mvlo == ~0u ? 0u : mvlo));
            }
        }
    }
}

void n265_analyze_ctu(struct n265_frame_work *e, int ctb_x, int ctb_y)
{
    int ctb_k = ctb_y * e->ctb_cols + ctb_x;
    int q = e->qp + e->ctu_qp_off[ctb_k];
    e->cur_ctu_qp = n265_qp_clip(q);
    e->cur_ctu_lam_q8 = e->ctu_lam_q8 ? e->ctu_lam_q8[ctb_k] : 256;
    e->cur_ctu_lam_sad_q8 = e->cur_ctu_lam_q8 == 256 ? 256 :
        (uint32_t)lrint(256.0 * sqrt((double)e->cur_ctu_lam_q8 / 256.0));
    e->cur_chroma_w_q8 = chroma_w_for_qp(e->cur_ctu_qp, e->chroma_idc);
    e->ent_ctu_qp = e->cur_ctu_qp;
    e->ent_qp_coded = 0;
    clear_cu_pred(e);
    if (e->cu_rec) {            /* N265_CU_TRACE: this CTU's own slots */
        e->cu_ctu = ctb_k;
        e->cu_rec_n[ctb_k] = 0;
        e->cu_sp = 0;
        e->cu_arm = CU_ARM_MERGE;
    }
    depth_bounds(e, ctb_x, ctb_y);
    (void)quadtree(e, ctb_x << e->ctb_log2, ctb_y << e->ctb_log2,
                   e->ctb_log2, 0);
    if (depth_bound() || e->dd_rec) {  /* what this CTU actually used */
        int x0 = ctb_x << e->ctb_log2, y0 = ctb_y << e->ctb_log2;
        int n = 1 << e->ctb_log2;
        int xe = (x0 + n) >> 2, ye = (y0 + n) >> 2;
        if (xe > (e->pad_w >> 2)) xe = e->pad_w >> 2;
        if (ye > (e->pad_h >> 2)) ye = e->pad_h >> 2;
        int lo = 0xff, hi = 0;
        for (int y = y0 >> 2; y < ye; y++)
            for (int x = x0 >> 2; x < xe; x++) {
                int d = e->ct_depth[y * e->map_stride + x];
                if (d < lo) lo = d;
                if (d > hi) hi = d;
            }
        int k = ctb_y * e->ctb_cols + ctb_x;
        if (depth_bound()) {
            e->ctu_dmin[k] = (uint8_t)(lo == 0xff ? 0 : lo);
            e->ctu_dmax[k] = (uint8_t)hi;
        }
        if (e->dd_rec) {
            e->dd_rec[k].dmin = (uint8_t)(lo == 0xff ? 0 : lo);
            e->dd_rec[k].dmax = (uint8_t)hi;
            depth_dump_features(e, ctb_x, ctb_y);
        }
    }
    if (e->ent_qp_coded)
        e->ent_qp_prev = e->cur_ctu_qp;
}

/* Initialize every cached knob and lazily-built table on the driver, at
 * encoder open, so a worker never performs the first write. Each of
 * these is idempotent, so a race could not change bits - but it is a
 * data race, and the TSan gate does not carry one. */
void n265_ctu_warm(void)
{
    (void)mode_hist_on(); (void)no_gates_on(); (void)no_amvp_on();
    (void)no_split_on(); (void)no_earlyskip_on(); (void)cb64_trace_on();
    (void)skip_thr8(); (void)es_thr_pct(); (void)rskip_mode();
    (void)cbf_fast(); (void)gate_scale(); (void)skip_exit();
    (void)static_commit(); (void)sc_conj();
    (void)intra_gate(); (void)intra_pf_q8(); (void)intra_oracle();
    (void)part_gate();
    (void)part_amp_margin(); (void)rect_rank(); (void)n265_rect_flat();
    (void)depth_bound();
    (void)depth_bound_widen(); (void)depth_bound_local();
    (void)depth_bound_ctb_skip(); (void)mvd_bits_on(); (void)me_satd_on();
    (void)cb64_gate(); (void)cb64_amvp(); (void)b_intra_margin();
    (void)bi_refine_iters(); (void)merge_rd_n(); (void)bi_gate();
    (void)bi_gate_margin(); (void)intra_rd_n(); (void)intra_rd_mpm();
    (void)i_intra(); (void)chroma_w_on(); (void)chroma_w_pct();
    (void)chroma_pred(); (void)cpred_trace(); (void)cpred_w_pct();
    (void)rr_trace();
    (void)cpred_gate(); (void)rdoq_trace(); (void)rdoq_lat_sz();
    for (int q = N265_QP_MIN; q <= N265_QP_MAX; q++)
        (void)n265_lambda_mode_q8(q);   /* fills its lazy table */
}
