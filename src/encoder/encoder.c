/*
 * Encoder core. Two modes: N265_MODE_INTRA is the real path (DC intra,
 * CTB 32, TU == CU, fixed QP); N265_MODE_PCM is the phase-0 I_PCM path
 * kept for harness A/B. Every frame is an IDR either way.
 * Copyright (c) 2026, the yah265 authors. GPL-2.0-or-later; see LICENSE.
 */

#include <limits.h>
#include <math.h>
#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>

#include "encoder/deblock.h"
#include "encoder/encoder.h"
#include "encoder/est.h"
#include "encoder/la.h"
#include "encoder/mc.h"
#include "encoder/rc.h"
#include "encoder/sao.h"

const char *n265_version_str(void)
{
    return "0.0.1";
}

void n265_param_default(n265_param *p)
{
    memset(p, 0, sizeof(*p));
    p->csp = N265_CSP_I420;
    p->fps_num = 25;
    p->fps_den = 1;
}

/* internal knobs the public API doesn't expose yet */
static int g_mode = N265_MODE_INTRA;
static int g_qp = 26;
static double g_crf = -1.0;      /* n265_set_crf; < 0 = CQP */
static double g_bitrate = 0.0;   /* n265_set_bitrate, kbit/s; 0 = off */
static int g_ctb_log2 = 6;
static int g_keyint = 250;      /* x265 4.3's own default (2026-09-08) */

static int g_bframes = 3;         /* hierarchical B default (2026-09-07) */
static int g_ref = 1;            /* n265_set_ref */
static int g_badapt = 1;         /* adaptive B run length, default on */
static int g_aq = 0;
static int g_la_depth = 12;
static int g_cutree = 0;
static int g_threads = 0;       /* --threads; 0 = auto */

void n265_set_threads(int n)
{
    g_threads = n < 0 ? 0 : n > 256 ? 256 : n;
}

/* auto: one worker per online CPU */
static int auto_threads(void)
{
    long n = sysconf(_SC_NPROCESSORS_ONLN);
    return n > 0 ? (int)n : 1;
}

void n265_set_la(int depth, int cutree);
void n265_set_la(int depth, int cutree)
{
    g_la_depth = depth < 1 ? 1 : depth > LA_MAX - 4 ? LA_MAX - 4 : depth;
    g_cutree = cutree ? 1 : 0;
}

void n265_set_mode(int mode, int qp, int ctb_log2, int keyint, int bframes,
                   int aq);
void n265_set_crf(double crf) { g_crf = crf; }
void n265_set_bitrate(double kbps) { g_bitrate = kbps > 0.0 ? kbps : 0.0; }
void n265_set_ref(int n) { g_ref = n < 1 ? 1 : n > N265_MAX_REF ? N265_MAX_REF : n; }
/* 0 off (fixed cadence), 1 the motion rule, 2 the priced mini-GOP */
void n265_set_badapt(int on) { g_badapt = on < 0 ? 0 : on > 3 ? 3 : on; }

void n265_set_mode(int mode, int qp, int ctb_log2, int keyint, int bframes,
                   int aq)
{
    g_mode = mode;
    g_qp = qp;
    g_ctb_log2 = ctb_log2;
    g_keyint = keyint < 1 ? 1 : keyint;
    g_bframes = bframes < 0 ? 0 : bframes > N265_MAX_BFRAMES
                                      ? N265_MAX_BFRAMES : bframes;
    g_aq = aq ? 1 : 0;
}

void n265_encoder_set_dump(n265_encoder *e, FILE *f)
{
    e->dump_recon = f;
}

void n265_encoder_set_dump_decode(n265_encoder *e, FILE *f);
void n265_encoder_set_dump_decode(n265_encoder *e, FILE *f)
{
    e->dump_decode = f;
}

/* The coding order plan_brange emits for a B run [lo, hi], appended to
 * `ord` as display offsets, counting the ones that are references. It
 * lives beside the SPS's DPB derivation on purpose: if the pyramid shape
 * ever changes, both have to move together or the SPS under-declares. */
static void gop_order(int lo, int hi, int *ord, int *n, int *nref)
{
    if (lo > hi)
        return;
    int mid = (lo + hi) / 2;
    if ((lo <= mid - 1) || (mid + 1 <= hi))
        (*nref)++;
    ord[(*n)++] = mid;
    gop_order(lo, mid - 1, ord, n, nref);
    gop_order(mid + 1, hi, ord, n, nref);
}

/* What the reference structure asks of a decoder's DPB (Annex C.5.2).
 *
 * `reorder` is exact. A picture is only ever reordered against its own
 * mini-GOP - display order advances monotonically from one mini-GOP to
 * the next - so the deepest reorder is the largest count of pictures
 * that precede a picture in the plan's coding order and follow it in
 * display order, maximised over every B-run length b-adapt may pick.
 *
 * `buffering` bounds the peak DPB occupancy, the union of the pictures
 * marked for reference and the pictures still waiting to be output: at
 * most (carried anchors + this mini-GOP's anchor + its reference Bs),
 * capped by the encoder's own slot count, plus the reorder + 1 pictures
 * the bumping process can leave pending. That is one to two buffers
 * above the occupancy a C.5.2.2 walk of the actual coding order reaches
 * at every (bframes, ref) pair, and over-declaring is the safe
 * direction: it only means the reorder condition rather than DPB
 * fullness is what triggers output.
 *
 * The old declaration was N265_DPB_SLOTS with sps_max_num_reorder_pics =
 * bframes, which at --bframes 7 signals a reorder depth LARGER than the
 * buffer count. That is out of range by 7.4.3.2.1 and dec265 refuses the
 * stream. */
void n265_dpb_requirement(int bframes, int ref_max, int *reorder,
                          int *buffering)
{
    int R = 0, nrefb = 0;
    for (int a = 0; a <= bframes; a++) {
        int ord[N265_MAX_BFRAMES + 1], n = 0, nref = 0;
        ord[n++] = a;                   /* the anchor is coded first */
        gop_order(0, a - 1, ord, &n, &nref);
        for (int i = 0; i < n; i++) {
            int c = 0;
            for (int k = 0; k < i; k++)
                if (ord[k] > ord[i])
                    c++;
            if (c > R)
                R = c;
        }
        if (nref > nrefb)
            nrefb = nref;
    }
    int refs = ref_max + 1 + nrefb;
    if (refs > N265_DPB_SLOTS)
        refs = N265_DPB_SLOTS;
    *reorder = R;
    *buffering = refs + R + 1;
}

/* Copy the immutable geometry and config into a frame_work. The ONE
 * writer of these fields (T0 audit); everything below is read-only for
 * the life of the encoder, which is what lets a row lane hold a copy. */
static void fw_config(n265_encoder *e, n265_frame_work *fw)
{
    fw->enc = e;
    fw->param = e->param;
    fw->mode = e->mode;
    fw->ctb_log2 = e->ctb_log2;
    fw->pad_w = e->pad_w;
    fw->pad_h = e->pad_h;
    fw->ctb_cols = e->ctb_cols;
    fw->ctb_rows = e->ctb_rows;
    fw->map_stride = e->map_stride;
    fw->rect = e->rect;
    fw->amp = e->amp;
    fw->rdoq = e->rdoq;
    fw->qp_delta_on = e->qp_delta_on;
    fw->bframes = e->bframes;
    n265_dpb_requirement(e->bframes, e->ref_max, &fw->dpb_reorder,
                         &fw->dpb_buffering);
    fw->psy = e->psy;
    for (int i = 0; i < 3; i++)
        fw->pad_stride[i] = e->pad_stride[i];
}

static int fw_open(n265_encoder *e, n265_frame_work *fw)
{
    fw_config(e, fw);
    size_t nblk = (size_t)e->map_stride * (e->pad_h >> 2);
    size_t nctu = (size_t)e->ctb_cols * e->ctb_rows;
    fw->est_ctx = fw->est_store;
    fw->ctu_dmin = malloc(nctu);
    fw->ctu_dmax = malloc(nctu);
    fw->ct_depth = calloc(nblk, 1);
    fw->intra_mode = calloc(nblk, 1);
    fw->edge_map = calloc(nblk, 1);
    fw->blk_intra = calloc(nblk, 1);
    fw->blk_skip = calloc(nblk, 1);
    fw->blk_cbf_y = calloc(nblk, 1);
    fw->blk_mv = calloc(nblk * 4, sizeof(int16_t));
    fw->blk_pred = calloc(nblk, 1);
    fw->blk_ref = calloc(nblk * 2, 1);
    fw->blk_qp = calloc(nblk, 1);
    fw->ctu_qp_off = calloc(nctu, 1);
    fw->ctu_qp_raw = calloc(nctu, sizeof(float));
    fw->sao = calloc(nctu, sizeof(sao_params));
    if (!fw->ctu_dmin || !fw->ctu_dmax || !fw->ct_depth || !fw->intra_mode ||
        !fw->edge_map || !fw->blk_intra || !fw->blk_skip || !fw->blk_cbf_y ||
        !fw->blk_mv || !fw->blk_pred || !fw->blk_ref || !fw->blk_qp ||
        !fw->ctu_qp_off || !fw->ctu_qp_raw || !fw->sao)
        return -1;
    fw->prog = calloc(1, sizeof(*fw->prog) +
                         (size_t)e->ctb_rows * sizeof(atomic_int));
    fw->rrec = calloc((size_t)e->ctb_rows, sizeof(*fw->rrec));
    fw->est_snap = calloc((size_t)e->ctb_rows * CTX_COUNT,
                          sizeof(cabac_ctx_t));
    if (!fw->rrec || !fw->est_snap || !fw->prog)
        return -1;
    for (int i = 0; i < 3; i++) {
        int w = i ? e->pad_w / 2 : e->pad_w;
        int bh = (1 << e->ctb_log2) >> (i ? 1 : 0);
        fw->sao_tmp[i] = malloc((size_t)w * bh);
        fw->sao_line[i] = malloc((size_t)w);
        if (!fw->sao_tmp[i] || !fw->sao_line[i])
            return -1;
    }
    size_t frame_bytes = (size_t)e->pad_w * e->pad_h * 3 / 2;
    if (bs_init(&fw->rbsp, frame_bytes + 4096) < 0)
        return -1;
    return 0;
}

static void fw_close(n265_frame_work *fw)
{
    free(fw->ctu_dmin);
    free(fw->ctu_dmax);
    free(fw->ct_depth);
    free(fw->intra_mode);
    free(fw->edge_map);
    free(fw->blk_intra);
    free(fw->blk_skip);
    free(fw->blk_cbf_y);
    free(fw->blk_mv);
    free(fw->blk_pred);
    free(fw->blk_ref);
    free(fw->blk_qp);
    free(fw->ctu_qp_off);
    free(fw->ctu_qp_raw);
    free(fw->sao);
    for (int i = 0; i < 3; i++) {
        free(fw->sao_tmp[i]);
        free(fw->sao_line[i]);
    }
    for (int r = 0; fw->rrec && r < fw->ctb_rows; r++)
        free(fw->rrec[r].buf);
    free(fw->rrec);
    free(fw->est_snap);
    free(fw->prog);
    bs_free(&fw->rbsp);
}

/* A row lane starts from a shallow copy of the frame's coding context
 * and re-points what has to be row-private. Nothing crosses a row
 * boundary except through est_snap, which is a fixed position (sec 2):
 * row r's cost-model contexts start from the snapshot row r-1 published
 * after its CTB 1, and row 0 starts from the slice-init contexts. The
 * analysis-side qPY_PREV is row-local too - it only prices the
 * cu_qp_delta bin now, since the QP map itself is resolved afterwards by
 * n265_qpmap_row. */
void n265_row_begin(n265_row_ctx *row, n265_frame_work *fw, int r)
{
    row->c = *fw;
    memcpy(row->est, r > 0 ? fw->est_snap + (size_t)(r - 1) * CTX_COUNT
                           : fw->est_store,
           sizeof(row->est));
    row->c.est_ctx = row->est;
    row->c.ent_qp_prev = fw->qp;
    row->c.sc = row->sc;
    row->c.rec_buf = fw->rrec[r].buf;
    row->c.rec_cap = fw->rrec[r].cap;
    row->c.rec_len = 0;
    row->c.rec_pos = 0;
}

void n265_row_end(n265_row_ctx *row, n265_frame_work *fw, int r)
{
    fw->rrec[r].buf = row->c.rec_buf;
    fw->rrec[r].cap = row->c.rec_cap;
    fw->rrec[r].len = row->c.rec_len;
}

n265_encoder *n265_encoder_open(const n265_param *p)
{
    if (!p || (p->width & 1) || (p->height & 1) || p->csp != N265_CSP_I420)
        return NULL;

    n265_encoder *e = calloc(1, sizeof(*e));
    if (!e)
        return NULL;
    e->param = *p;
    e->mode = g_mode;
    e->qp_base = g_qp;
    e->crf_on = g_crf >= 0.0;
    e->crf = g_crf;
    /* ABR overrides CRF: the rate factor is solved, not given. The
     * fractional base QP rides with it - see encode_frame. */
    e->abr_on = g_bitrate > 0.0 && e->mode != N265_MODE_PCM;
    e->abr_bitrate = g_bitrate * 1000.0;
    { const char *v = getenv("N265_QP_FRAC");
      e->qp_frac = v ? atoi(v) != 0 : e->abr_on; }
    if (e->abr_on)
        e->crf_on = 0;
    e->ref_max = e->mode == N265_MODE_PCM ? 1 : g_ref;
    e->badapt = g_badapt;
    { const char *v = getenv("N265_CUTREE_STR"); e->cutree_str = v ? (float)atof(v) : 2.0f; }
    /* AQ 0.7 with the joint field rounded once (2026-09-07 sweep vs cu-tree
     * strength: 6/7 clips better, -0.9% mean BD-VMAF-NEG at matched rate;
     * AQ 0.4 and cu-tree 2.5 each bought median by giving bus back 2-3%).
     * N265_AQ_STR=1 N265_QP_ROUND1=0 restores the old field. */
    { const char *v = getenv("N265_AQ_STR"); e->aq_str = v ? (float)atof(v) : 0.7f; }
    { const char *v = getenv("N265_QP_ROUND1"); e->qp_round1 = v ? atoi(v) != 0 : 1; }
    /* rectangular (2NxN/Nx2N) and asymmetric (2NxnU/D, nLx2N, nRx2N)
     * inter partitions. AMP rides on rect: the AMP trial is only entered
     * where a rectangular split already beat 2Nx2N, so turning rect off
     * turns AMP off with it. N265_NO_RECT / N265_NO_AMP are the escapes. */
    e->rect = e->mode != N265_MODE_PCM && !getenv("N265_NO_RECT");
    e->amp = e->rect && !getenv("N265_NO_AMP");
    e->ctb_log2 = e->mode == N265_MODE_PCM ? 4 : g_ctb_log2;
    e->keyint = e->mode == N265_MODE_PCM ? 1 : g_keyint;
    e->bframes = e->mode == N265_MODE_PCM ? 0 : g_bframes;
    e->no_sao = getenv("N265_NO_SAO") != NULL;      /* diagnostic */
    /* RDOQ on by default (2026-09-06: -5.9% median BD-VMAF-NEG at matched
     * rate, 7/7 clips, worst -5.5%); N265_RDOQ=0 is the escape. */
    { const char *v = getenv("N265_RDOQ"); e->rdoq = v ? atoi(v) : 1; }
    { const char *v = getenv("N265_PSY"); e->psy = v ? atof(v) : 0.0; }
    e->aq = g_aq || getenv("N265_AQ") != NULL;      /* variance AQ */
    e->la_depth = e->mode == N265_MODE_PCM || !e->bframes ? g_la_depth
                                                          : g_la_depth;
    e->cutree = e->mode == N265_MODE_PCM ? 0 : g_cutree;
    e->qp_delta_on = e->mode != N265_MODE_PCM &&
                     (e->aq || e->cutree || e->qp_frac);
    e->scenecut = e->mode != N265_MODE_PCM;
    int align = e->mode == N265_MODE_PCM ? 16 : (1 << MIN_CB_LOG2);
    if (p->width < align || p->height < align)
        goto fail;
    e->pad_w = (p->width + align - 1) & ~(align - 1);
    e->pad_h = (p->height + align - 1) & ~(align - 1);
    int ctb = 1 << e->ctb_log2;
    e->ctb_cols = (e->pad_w + ctb - 1) >> e->ctb_log2;
    e->ctb_rows = (e->pad_h + ctb - 1) >> e->ctb_log2;

    /* K, the frames in flight (sec 1). A pure scheduling knob: it moves
     * no bits, only how much of the mini-GOP overlaps. */
    { const char *v = getenv("N265_MAX_FRAME_WORK");
      int k = v ? atoi(v) : 4;
      e->K = k < 1 ? 1 : k > N265_MAX_FRAME_WORK ? N265_MAX_FRAME_WORK : k; }

    for (int i = 0; i < 3; i++) {
        int w = i ? e->pad_w / 2 : e->pad_w;
        int h = i ? e->pad_h / 2 : e->pad_h;
        e->pad_stride[i] = w;
        int pad = i ? N265_REF_PAD / 2 : N265_REF_PAD;
        for (int sl = 0; sl < N265_DPB_SLOTS + e->K; sl++) {
            struct n265_refpic *r = sl < N265_DPB_SLOTS
                ? &e->dpb[sl] : &e->scratch_pic[sl - N265_DPB_SLOTS];
            r->stride[i] = w + 2 * pad;
            r->base[i] = malloc((size_t)r->stride[i] * (h + 2 * pad));
            if (!r->base[i])
                goto fail;
            r->plane[i] = r->base[i] + pad * r->stride[i] + pad;
        }
    }
    for (int sl = 0; sl < N265_DPB_SLOTS; sl++) {
        int c16 = ((e->pad_w + 15) >> 4) * ((e->pad_h + 15) >> 4);
        e->dpb[sl].col_pred = calloc((size_t)c16, 1);
        e->dpb[sl].col_mv = calloc((size_t)c16, 8);
        e->dpb[sl].col_ref = calloc((size_t)c16, 4);
        if (!e->dpb[sl].col_pred || !e->dpb[sl].col_mv || !e->dpb[sl].col_ref)
            goto fail;
        size_t nctu = (size_t)e->ctb_cols * e->ctb_rows;
        e->dpb[sl].ctu_dmin = malloc(nctu);
        e->dpb[sl].ctu_dmax = malloc(nctu);
        if (!e->dpb[sl].ctu_dmin || !e->dpb[sl].ctu_dmax)
            goto fail;
        memset(e->dpb[sl].ctu_dmin, 0xff, nctu);
        memset(e->dpb[sl].ctu_dmax, 0xff, nctu);
    }
    e->map_stride = e->pad_w >> 2;
    for (int i = 0; i < e->K; i++)
        if (fw_open(e, &e->fw[i]) < 0)
            goto fail;
    {
        int nt = g_threads > 0 ? g_threads : auto_threads();
        /* Lanes, not workers: a worker that stalls on a reference row
         * runs an older frame's task NESTED (G3), and the nested run
         * needs its own row scratch. Nesting is bounded by the number of
         * live jobs, so nt * K lanes is the ceiling. Their scratch is
         * allocated on first use, because the nested lanes are rarely
         * touched. */
        e->pool = ntp_open(nt, e->K, nt * e->K);
    }
    if (!e->pool)
        goto fail;
    e->nlanes = ntp_lanes(e->pool);
    e->row = calloc((size_t)e->nlanes, sizeof(*e->row));
    if (!e->row)
        goto fail;
    for (int i = 0; i < ntp_threads(e->pool); i++)
        if (!(e->row[i].sc = n265_scratch_new()))
            goto fail;
    /* every cached knob and lazy table, initialized here, on this
     * thread, before a worker can be the one that writes it first */
    n265_tables_init();
    n265_tables_init_est();
    n265_ctu_warm();
    n265_mv_warm();
    n265_sao_warm();
    n265_rdoq_warm();

    size_t frame_bytes = (size_t)e->pad_w * e->pad_h * 3 / 2;
    /* A whole mini-GOP leaves per call, and the ring grows if it has to
     * (add_nal), so the payload pointers are patched from offsets at the
     * end of the call rather than being taken before a realloc. */
    e->out_cap = 3 * nal_pack_bound(frame_bytes + 4096) + 4096;
    e->out = malloc(e->out_cap);
    if (!e->out)
        goto fail;
    if (n265_la_init(e) < 0)
        goto fail;
    n265_rc_open(e);        /* after la_init: it needs the lowres geometry */
    return e;

fail:
    n265_encoder_close(e);
    return NULL;
}

void n265_encoder_close(n265_encoder *e)
{
    if (!e)
        return;
    if (getenv("N265_STALL_TRACE"))
        fprintf(stderr, "STALL frames=%ld stalls=%ld %.3f/frame\n",
                (long)atomic_load(&e->stall_frames),
                (long)atomic_load(&e->ref_stalls),
                atomic_load(&e->stall_frames)
                    ? (double)atomic_load(&e->ref_stalls) /
                      (double)atomic_load(&e->stall_frames) : 0.0);
    for (int i = 0; i < e->K; i++)
        fw_close(&e->fw[i]);
    for (int i = 0; i < e->nlanes; i++)
        n265_scratch_free(e->row[i].sc);
    free(e->row);
    ntp_close(e->pool);
    for (int i = 0; i < 3; i++) {
        for (int sl = 0; sl < N265_DPB_SLOTS; sl++)
            free(e->dpb[sl].base[i]);
        for (int sl = 0; sl < N265_MAX_FRAME_WORK; sl++)
            free(e->scratch_pic[sl].base[i]);
    }
    for (int i = 0; i < 3; i++)
        free(e->hdr_rbsp[i]);
    for (size_t i = 0; i < sizeof(e->done) / sizeof(e->done[0]); i++)
        free(e->done[i].buf);
    for (int sl = 0; sl < N265_DPB_SLOTS; sl++) {
        free(e->dpb[sl].col_pred);
        free(e->dpb[sl].col_mv);
        free(e->dpb[sl].col_ref);
        free(e->dpb[sl].ctu_dmin);
        free(e->dpb[sl].ctu_dmax);
        for (int i = 0; i < 3; i++)
            free(e->dpb[sl].hf_base[i]);
    }
    n265_la_free(e);
    free(e->out);
    free(e);
}

/* D1: capture the short-term RPS on the driver, in coding order, right
 * after the reference lists are built and before anything else is
 * dispatched. Everything it reads - `marked`, `poc`, the just-built
 * lists - is driver-serial, so the snapshot is the set the SERIAL
 * encoder's header writer would have seen: nothing between dispatch and
 * the header write changes the marked set, which is the invariant T0a
 * already relied on for the slot claim.
 *
 * Keyed on POC throughout. The enumeration walks slots because that is
 * where the pictures live, but the ORDER is a full sort on POC and the
 * output carries POCs, so slot identity cannot reach the bitstream. */
static void snapshot_rps(n265_encoder *e, n265_frame_work *fw)
{
    struct n265_rps *r = &fw->rps;
    r->nneg = r->npos = 0;
    if (fw->slice_type == 2)
        return;                 /* an IDR signals no short-term set */
    for (int sl = 0; sl < N265_DPB_SLOTS; sl++) {
        struct n265_refpic *p = &e->dpb[sl];
        if (!p->marked)         /* every kept picture is listed; eviction
                                 * is at mini-GOP ends */
            continue;
        int used = 0;
        for (int l = 0; l < 2; l++)
            for (int k = 0; k < fw->nref[l]; k++)
                if (fw->refl[l][k] == p)
                    used = 1;
        if (p->poc < fw->poc) {
            r->neg_poc[r->nneg] = p->poc;
            r->neg_used[r->nneg++] = (uint8_t)used;
        } else {
            r->pos_poc[r->npos] = p->poc;
            r->pos_used[r->npos++] = (uint8_t)used;
        }
    }
    /* nearest first: negatives by descending POC, positives by ascending.
     * POCs are unique among marked pictures (an IDR clears the set), so
     * there is no tie for slot order to break. */
    for (int i = 1; i < r->nneg; i++)
        for (int k = i; k > 0 && r->neg_poc[k] > r->neg_poc[k - 1]; k--) {
            int t = r->neg_poc[k]; r->neg_poc[k] = r->neg_poc[k - 1]; r->neg_poc[k - 1] = t;
            uint8_t u = r->neg_used[k]; r->neg_used[k] = r->neg_used[k - 1]; r->neg_used[k - 1] = u;
        }
    for (int i = 1; i < r->npos; i++)
        for (int k = i; k > 0 && r->pos_poc[k] < r->pos_poc[k - 1]; k--) {
            int t = r->pos_poc[k]; r->pos_poc[k] = r->pos_poc[k - 1]; r->pos_poc[k - 1] = t;
            uint8_t u = r->pos_used[k]; r->pos_used[k] = r->pos_used[k - 1]; r->pos_used[k - 1] = u;
        }
}

static void write_slice_header(n265_encoder *e, n265_frame_work *fw)
{
    (void)e;
    bitstream_t *bs = &fw->rbsp;
    bs_put_bits(bs, 1, 1);          /* first_slice_segment_in_pic_flag */
    if (fw->slice_type == 2)
        bs_put_bits(bs, 0, 1);      /* no_output_of_prior_pics_flag (IRAP) */
    bs_put_ue(bs, 0);               /* slice_pic_parameter_set_id */
    bs_put_ue(bs, (uint32_t)fw->slice_type);
    if (fw->slice_type != 2) {
        bs_put_bits(bs, (uint32_t)(fw->poc & 255), 8); /* pic_order_cnt_lsb */
        bs_put_bits(bs, 0, 1);      /* short_term_ref_pic_set_sps_flag */
        /* st_ref_pic_set(idx 0) inline, serialized from the dispatch-time
         * snapshot (D1). The writer does not touch the DPB: by the time
         * it runs, frames after this one may have been dispatched and
         * changed the marked set. */
        const struct n265_rps *rp = &fw->rps;
        bs_put_ue(bs, (uint32_t)rp->nneg);      /* num_negative_pics */
        bs_put_ue(bs, (uint32_t)rp->npos);      /* num_positive_pics */
        int prev = fw->poc;
        for (int i = 0; i < rp->nneg; i++) {
            bs_put_ue(bs, (uint32_t)(prev - rp->neg_poc[i] - 1)); /* delta_poc_s0_minus1 */
            bs_put_bits(bs, rp->neg_used[i], 1);   /* used_by_curr_pic_s0_flag */
            prev = rp->neg_poc[i];
        }
        prev = fw->poc;
        for (int i = 0; i < rp->npos; i++) {
            bs_put_ue(bs, (uint32_t)(rp->pos_poc[i] - prev - 1)); /* delta_poc_s1_minus1 */
            bs_put_bits(bs, rp->pos_used[i], 1);
            prev = rp->pos_poc[i];
        }
        bs_put_bits(bs, getenv("N265_NO_TMVP") ? 0 : 1, 1);      /* slice_temporal_mvp_enabled_flag */
    }
    if (fw->mode != N265_MODE_PCM && n265_sao_enabled()) {
        bs_put_bits(bs, 1, 1);      /* slice_sao_luma_flag */
        bs_put_bits(bs, 1, 1);      /* slice_sao_chroma_flag */
    }
    if (fw->slice_type != 2) {
        int override = fw->nref[0] != 1 || (fw->slice_type == 0 && fw->nref[1] != 1);
        bs_put_bits(bs, (uint32_t)override, 1);     /* num_ref_idx_active_override_flag */
        if (override) {
            bs_put_ue(bs, (uint32_t)(fw->nref[0] - 1));
            if (fw->slice_type == 0)
                bs_put_ue(bs, (uint32_t)(fw->nref[1] - 1));
        }
        if (fw->slice_type == 0)
            bs_put_bits(bs, 0, 1);  /* mvd_l1_zero_flag */
        if (!getenv("N265_NO_TMVP")) {
            if (fw->slice_type == 0)
                bs_put_bits(bs, 1, 1);  /* collocated_from_l0_flag */
            if (fw->nref[0] > 1)
                bs_put_ue(bs, 0);       /* collocated_ref_idx: L0[0] */
        }
        bs_put_ue(bs, 5 - MAX_MERGE);   /* five_minus_max_num_merge_cand */
    }
    bs_put_se(bs, fw->qp - 26);      /* slice_qp_delta */
    if (fw->mode != N265_MODE_PCM)
        bs_put_bits(bs, 1, 1);      /* slice_loop_filter_across_slices_enabled
                                     * (present because deblocking is on) */
    bs_align_one_then_zero(bs);     /* byte_alignment() */
}

/* E1: the reference's cached horizontal first stage, three frame-sized
 * 8-tap passes once per stored reference. Every later subpel candidate
 * with a nonzero fx - which is most of them - then skips its own. Off
 * with N265_HFCACHE=0, which is the A/B arm and nothing else: the two
 * paths produce identical bytes. */
static int hf_cache_on(void)
{
    static int on = -1;
    if (on < 0) {
        const char *v = getenv("N265_HFCACHE");
        on = v ? atoi(v) != 0 : 1;
    }
    return on;
}

/* Arm the cache on a freshly claimed slot: the buffers and the region it
 * covers. Nothing is filled here - G4 makes the fill per row, inside the
 * publish task, so that a row's cache exists before final_rows advances
 * past it. Filling is safe there with no ref_wait of its own because the
 * horizontal 8-tap reads only along its OWN line: the cache for row r
 * publishes with row r, not with row r + 1. */
static void hf_arm(n265_encoder *e, struct n265_refpic *r)
{
    for (int i = 0; i < 3; i++)
        r->hf[i] = NULL;
    if (!hf_cache_on())
        return;
    int w = e->pad_w + 2 * N265_HF_PAD, h = e->pad_h + 2 * N265_HF_PAD;
    r->hf_stride = w;
    for (int i = 0; i < 3; i++) {
        if (!r->hf_base[i]) {
            r->hf_base[i] = malloc((size_t)w * h * sizeof(int16_t));
            if (!r->hf_base[i]) {   /* no cache is a slow path, not a fail */
                for (int k = 0; k < 3; k++)
                    r->hf[k] = NULL;
                return;
            }
        }
        r->hf[i] = r->hf_base[i] + N265_HF_PAD * w + N265_HF_PAD;
    }
    r->hf_x0 = -N265_HF_PAD;
    r->hf_x1 = e->pad_w + N265_HF_PAD;
    r->hf_y0 = -N265_HF_PAD;
    r->hf_y1 = e->pad_h + N265_HF_PAD;
}

/* The E1 cache for the luma lines CTB row r owns, plus the padding rows
 * above (row 0) and below (the last row), which are replications of
 * lines this row has just border-extended. */
static void hf_fill_rows(n265_frame_work *fw, struct n265_refpic *r, int cy,
                         int last)
{
    if (!r->hf[0])
        return;
    int ctb = 1 << fw->ctb_log2;
    int w = fw->pad_w + 2 * N265_HF_PAD;
    int rs = r->stride[0];
    int y0 = cy * ctb, y1 = y0 + ctb;
    if (y1 > fw->pad_h)
        y1 = fw->pad_h;
    if (cy == 0)
        y0 = -N265_HF_PAD;
    if (last)
        y1 = fw->pad_h + N265_HF_PAD;
    const uint8_t *src = r->plane[0] + y0 * rs - N265_HF_PAD;
    N265_REF_OPEN_ALL();
    for (int i = 0; i < 3; i++)
        n265_mc_luma_hplane(r->hf[i] + (size_t)y0 * w - N265_HF_PAD, w,
                            w, y1 - y0, src, rs, i + 1);
    N265_REF_UNGUARD();
}

static void write_pcm_ctu(n265_frame_work *e, cabac_t *cb, cabac_ctx_t *part_mode,
                          int ctb_x, int ctb_y)
{
    bitstream_t *bs = &e->rbsp;
    int ctb = 1 << e->ctb_log2;
    n265_cabac_bin(cb, part_mode, 1);
    n265_cabac_terminate(cb, 1);
    bs_align_zero(bs);              /* pcm_alignment_zero_bit */

    int x0 = ctb_x * ctb;
    int y0 = ctb_y * ctb;
    const uint8_t *l = e->pad_plane[0] + y0 * e->pad_stride[0] + x0;
    for (int y = 0; y < ctb; y++)
        for (int x = 0; x < ctb; x++)
            bs_put_bits(bs, l[y * e->pad_stride[0] + x], 8);
    for (int p = 1; p <= 2; p++) {
        const uint8_t *c = e->pad_plane[p] +
            (y0 / 2) * e->pad_stride[p] + x0 / 2;
        for (int y = 0; y < ctb / 2; y++)
            for (int x = 0; x < ctb / 2; x++)
                bs_put_bits(bs, c[y * e->pad_stride[p] + x], 8);
    }
    n265_cabac_start(cb, bs);
}

/* The deferred-NAL ring. Frames finish out of order under overlap, and a
 * resource stall can force one to be joined long before the API call
 * that owes it runs. So a joined frame's NALs are packed HERE, in coding
 * order, and copied into the caller's buffer only when that call comes
 * round; the driver is then free to run as far ahead as K allows without
 * the API returning frames early. */
static struct n265_done *done_slot(n265_encoder *e)
{
    int n = (int)(sizeof(e->done) / sizeof(e->done[0]));
    struct n265_done *d = &e->done[(e->done_head + e->n_done) % n];
    d->len = 0;
    d->nnal = 0;
    return d;
}

static void pack_nal(struct n265_done *d, int type, const uint8_t *rbsp,
                     size_t len)
{
    if (d->nnal >= (int)(sizeof(d->type) / sizeof(d->type[0])))
        return;
    size_t need = d->len + nal_pack_bound(len) + 64;
    if (need > d->cap) {
        uint8_t *b = realloc(d->buf, need);
        if (!b)
            return;
        d->buf = b;
        d->cap = need;
    }
    size_t sz = nal_pack(d->buf + d->len, d->cap - d->len, type, rbsp, len);
    d->type[d->nnal] = type;
    d->size[d->nnal] = (int)sz;
    d->nnal++;
    d->len += sz;
}

/* Copy up to `max` joined frames into the caller's buffer, oldest first.
 * The buffer grows, so payload POINTERS are patched from offsets at the
 * end of the call rather than taken before a realloc that may move them. */
static void emit_done(n265_encoder *e, int max, size_t *out_used, int *nnal)
{
    int n = (int)(sizeof(e->done) / sizeof(e->done[0]));
    int cap_nal = (int)(sizeof(e->nal) / sizeof(e->nal[0]));
    while (max-- > 0 && e->n_done > 0) {
        struct n265_done *d = &e->done[e->done_head];
        if (*nnal + d->nnal > cap_nal)
            break;
        if (*out_used + d->len > e->out_cap) {
            size_t want = *out_used + d->len;
            size_t cap = e->out_cap * 2 > want ? e->out_cap * 2 : want;
            uint8_t *o = realloc(e->out, cap);
            if (!o)
                break;
            e->out = o;
            e->out_cap = cap;
        }
        memcpy(e->out + *out_used, d->buf, d->len);
        size_t off = *out_used;
        for (int i = 0; i < d->nnal; i++) {
            e->nal[*nnal].type = d->type[i];
            e->nal[*nnal].payload = NULL;   /* patched at end of call */
            e->nal[*nnal].size = d->size[i];
            e->nal_off[*nnal] = off;
            off += (size_t)d->size[i];
            (*nnal)++;
        }
        *out_used += d->len;
        e->done_head = (e->done_head + 1) % n;
        e->n_done--;
    }
}

/* Stash a header RBSP written into fw->rbsp; it is packed when the frame
 * that carries it retires, so it lands ahead of that frame's slice. */
static void stash_header(n265_encoder *e, int type, n265_frame_work *fw)
{
    int i = e->nhdr;
    if (i >= 3)
        return;
    uint8_t *b = realloc(e->hdr_rbsp[i], fw->rbsp.bytes + 1);
    if (!b)
        return;
    memcpy(b, fw->rbsp.buf, fw->rbsp.bytes);
    e->hdr_rbsp[i] = b;
    e->hdr_len[i] = fw->rbsp.bytes;
    e->hdr_type[i] = type;
    e->nhdr = i + 1;
}

static void dump_planes(n265_frame_work *fw, FILE *f)
{
    /* PCM writes no reconstruction; its output IS the padded source, and
     * that plane set has the unpadded stride, not the DPB slot's. */
    int pcm = fw->mode == N265_MODE_PCM;
    for (int i = 0; i < 3; i++) {
        int w = i ? fw->param.width / 2 : fw->param.width;
        int h = i ? fw->param.height / 2 : fw->param.height;
        int stride = pcm ? fw->pad_stride[i] : fw->recon_stride[i];
        const uint8_t *pl = pcm ? fw->pad_plane[i] : fw->recon[i];
        for (int y = 0; y < h; y++)
            fwrite(pl + y * stride, 1, (size_t)w, f);
    }
}

static void dump_recon_frame(n265_encoder *e, n265_frame_work *fw)
{
    if (e->dump_recon) {
        /* display order: frames land at their display index (B frames
         * are encoded out of order, so seek) */
        long frame_bytes =
            (long)e->param.width * e->param.height * 3 / 2;
        fseek(e->dump_recon, (long)fw->display_idx * frame_bytes, SEEK_SET);
        dump_planes(fw, e->dump_recon);
    }
    if (e->dump_decode)
        dump_planes(fw, e->dump_decode);
}

/* The exact reference stall (G2). The fast path is inline in
 * encoder.h; this is what happens when it misses: count it, then run
 * ready tasks of strictly OLDER frames until the watermark arrives. */
struct ref_stall_ctx {
    const struct n265_refpic *pic;
    int need;
};

static int ref_stall_pred(void *ctx)
{
    struct ref_stall_ctx *w = ctx;
    return n265_ref_have(w->pic) >= w->need;
}

void n265_ref_stall(n265_frame_work *e, const struct n265_refpic *pic,
                    int need)
{
    n265_encoder *enc = e->enc;
    atomic_fetch_add_explicit(&enc->ref_stalls, 1, memory_order_relaxed);
    struct ref_stall_ctx w = { pic, need };
    ntp_wait_older(enc->pool, ref_stall_pred, &w);
}

/* Publish CTB row `cy` of this frame's picture (G4). Everything a later
 * frame can read from it is written HERE, before final_rows advances
 * past the row: the row's border extension (and, on the first and last
 * rows, the padding above and below it), the E1 half-pel cache for the
 * row's lines, the TMVP motion field for the 16x16 cells the row covers,
 * and the P2 per-CTU depth range. The whole-frame publish pass that used
 * to do all of this after entropy is gone, and that deletion is the
 * proof that nothing was missed: every write it did has a row that owns
 * it now. */
static void publish_row(n265_frame_work *fw, int cy)
{
    struct n265_refpic *slot = fw->slot;
    int ctb = 1 << fw->ctb_log2;
    int last = cy == fw->ctb_rows - 1;

    if (fw->mode != N265_MODE_PCM && !fw->nonref) {
        for (int i = 0; i < 3; i++) {
            int sh = i ? 1 : 0;
            int w = fw->pad_w >> sh, h = fw->pad_h >> sh;
            int pad = i ? N265_REF_PAD / 2 : N265_REF_PAD;
            int rs = slot->stride[i];
            int y0 = (cy * ctb) >> sh;
            int y1 = y0 + (ctb >> sh);
            if (y1 > h)
                y1 = h;
            for (int y = y0; y < y1; y++) {
                uint8_t *d = slot->plane[i] + y * rs;
                memset(d - pad, d[0], (size_t)pad);
                memset(d + w, d[w - 1], (size_t)pad);
            }
            if (cy == 0)
                for (int y = 1; y <= pad; y++)
                    memcpy(slot->plane[i] - y * rs - pad,
                           slot->plane[i] - pad, (size_t)rs);
            if (last)
                for (int y = 1; y <= pad; y++)
                    memcpy(slot->plane[i] + (h - 1 + y) * rs - pad,
                           slot->plane[i] + (h - 1) * rs - pad, (size_t)rs);
        }
        hf_fill_rows(fw, slot, cy, last);
        {
            int c16w = (fw->pad_w + 15) >> 4;
            int c16h = (fw->pad_h + 15) >> 4;
            int r0 = (cy * ctb) >> 4;
            int r1 = r0 + (ctb >> 4);
            if (r1 > c16h)
                r1 = c16h;
            for (int y16 = r0; y16 < r1; y16++)
                for (int cx = 0; cx < c16w; cx++) {
                    int m = (y16 * 4) * fw->map_stride + cx * 4;
                    int i16 = y16 * c16w + cx;
                    if (fw->slice_type == 2 || fw->blk_intra[m]) {
                        slot->col_pred[i16] = 0;
                        continue;
                    }
                    slot->col_pred[i16] = fw->blk_pred[m];
                    memcpy(slot->col_mv + 4 * i16, fw->blk_mv + 4 * m, 8);
                    for (int l = 0; l < 2; l++)
                        slot->col_ref[2 * i16 + l] = (fw->blk_pred[m] & (1 << l))
                            ? (int16_t)fw->refl[l][fw->blk_ref[2 * m + l]]->poc : 0;
                }
        }
        /* P2: carry this row's per-CTU depth range with the picture, so
         * a later frame can read the collocated range */
        memcpy(slot->ctu_dmin + (size_t)cy * fw->ctb_cols,
               fw->ctu_dmin + (size_t)cy * fw->ctb_cols,
               (size_t)fw->ctb_cols);
        memcpy(slot->ctu_dmax + (size_t)cy * fw->ctb_cols,
               fw->ctu_dmax + (size_t)cy * fw->ctb_cols,
               (size_t)fw->ctb_cols);
    }
    atomic_store_explicit(&slot->final_rows, cy + 1, memory_order_release);
}

static void entropy_frame(n265_encoder *e, n265_frame_work *fw);

/* The frame's task table (docs/threading-design.md sec 1, G4). Indices:
 * [0, R)      encode row r
 * [R, 2R)     QP-map fill + deblock row r
 * [2R, 3R)    SAO row r (when the filter is on) then PUBLISH row r
 * 3R          entropy
 * The third kind exists whether or not SAO does: the filter is off by
 * default and the publish still has to happen per row. R is 0 on the PCM
 * path, which is the entropy task alone. */

/* Row r's CTB x may run when row r-1 has committed CTB x+1, the 2-CTB
 * WPP lag: intra's top-right neighbour and the estimator snapshot both
 * live under it. */
struct lag_wait {
    struct n265_prog *p;
    int r, need;
};

static int lag_pred(void *ctx)
{
    struct lag_wait *w = ctx;
    return atomic_load_explicit(&w->p->enc_prog[w->r - 1],
                                memory_order_acquire) >= w->need;
}

static int job_ready(void *ctx, int idx)
{
    n265_frame_work *fw = ctx;
    struct n265_prog *p = fw->prog;
    int R = fw->job_R;
    if (idx < R) {
        int r = idx;
        if (r > 0) {
            int need = fw->job_snap_x + 1 > 2 ? fw->job_snap_x + 1 : 2;
            if (need > fw->job_cols) need = fw->job_cols;
            if (atomic_load_explicit(&p->enc_prog[r - 1],
                                     memory_order_acquire) < need)
                return 0;
        }
        /* G1: sec 1's row-start gate, demoted to a scheduling hint. It
         * keeps a row from starting only to stall at its first motion
         * probe; it guarantees NOTHING (the T2/3 reach table), the
         * ref_wait at every read is what makes the row safe, and as a
         * hint it cannot change bits. */
        if (fw->ref_lag) {
            int nr = r + fw->ref_lag;
            if (nr > R) nr = R;
            for (int l = 0; l < 2; l++)
                for (int k = 0; k < fw->nref[l]; k++)
                    if (n265_ref_have(fw->refl[l][k]) < nr)
                        return 0;
        }
        return 1;
    }
    if (idx < 2 * R) {
        int r = idx - R;
        int need = r + 2 < R ? r + 2 : R;
        return atomic_load_explicit(&p->dbk_rows, memory_order_acquire) == r &&
               atomic_load_explicit(&p->enc_rows, memory_order_acquire) >= need;
    }
    if (idx < 3 * R) {
        int r = idx - 2 * R;
        int need = r + 2 < R ? r + 2 : R;
        return atomic_load_explicit(&p->pub_rows, memory_order_acquire) == r &&
               atomic_load_explicit(&p->dbk_rows, memory_order_acquire) >= need;
    }
    return atomic_load_explicit(&p->pub_rows, memory_order_acquire) == R;
}

static void job_run(void *ctx, int lane, int idx)
{
    n265_frame_work *fw = ctx;
    n265_encoder *e = fw->enc;
    struct n265_prog *p = fw->prog;
    int R = fw->job_R;

    if (idx < R) {
        int r = idx;
        n265_row_ctx *row = &e->row[lane];
        if (!row->sc)       /* a nested lane, allocated on first use */
            row->sc = n265_scratch_new();
        n265_row_begin(row, fw, r);
        for (int cx = 0; cx < fw->job_cols; cx++) {
            if (r > 0) {
                int need = cx + 2 < fw->job_cols ? cx + 2 : fw->job_cols;
                struct lag_wait w = { p, r, need };
                ntp_wait(e->pool, lag_pred, &w);
            }
            n265_analyze_ctu(&row->c, cx, r);
            if (cx == fw->job_snap_x)
                memcpy(fw->est_snap + (size_t)r * CTX_COUNT, row->c.est_ctx,
                       CTX_COUNT * sizeof(cabac_ctx_t));
            atomic_store_explicit(&p->enc_prog[r], cx + 1,
                                  memory_order_release);
            ntp_publish(e->pool);
        }
        n265_row_end(row, fw, r);
        /* rows complete in index order (a row's last CTB waits for the
         * row above to be complete), so the count IS the watermark */
        atomic_fetch_add_explicit(&p->enc_rows, 1, memory_order_release);
        ntp_publish(e->pool);
        return;
    }
    if (idx < 2 * R) {
        int r = idx - R;
        n265_qpmap_row(fw, r, &fw->qprev);
        n265_deblock_row(fw, r);
        atomic_store_explicit(&p->dbk_rows, r + 1, memory_order_release);
        ntp_publish(e->pool);
        return;
    }
    if (idx < 3 * R) {
        int r = idx - 2 * R;
        if (fw->job_sao_on) {
            if (e->no_sao)
                memset(&fw->sao[r * fw->ctb_cols], 0,
                       (size_t)fw->ctb_cols * sizeof(sao_params));
            n265_sao_row(fw, r);
        }
        publish_row(fw, r);
        atomic_store_explicit(&p->pub_rows, r + 1, memory_order_release);
        ntp_publish(e->pool);
        return;
    }
    entropy_frame(e, fw);
}

/* Dispatch one frame: everything that is driver-serial by construction
 * (the QP ladder, the AQ field, the headers, the record and estimator
 * reset), then the frame's job onto the pool. The driver does NOT wait
 * here - that is what makes frames overlap - and it does not touch the
 * frame again until retire_frame joins the job. */
static void dispatch_frame(n265_encoder *e, n265_frame_work *fw)
{
    int slice_type = fw->slice_type, b_depth = fw->b_depth;
    int nonref = fw->nonref, poc = fw->poc;
    /* P2: no CTU of this picture has a decided depth range yet */
    memset(fw->ctu_dmin, 0xff, (size_t)fw->ctb_cols * fw->ctb_rows);
    memset(fw->ctu_dmax, 0xff, (size_t)fw->ctb_cols * fw->ctb_rows);
    /* frame-type QP ladder (the matched reference applies its ip/pb
     * ratios in CQP mode too): I anchors lower, non-ref B higher */
    /* CRF (yah264's final form, written for this encoder): a constant base per
     * frame, crf + the cu-tree operating-point shift. The cu-tree offsets are
     * left un-centred under CRF (a referenced region nets a lower QP, a leaf
     * region does not), so their mean-negative field is netted out by a
     * fixed shift and only the DIFFERENTIAL survives; centring them would
     * throw the cross-frame allocation away. N265_CRF_SHIFT (default 5.4 =
     * (1 - qcomp) * 13.5 at qcomp 0.6, 0 without cu-tree). The AQ frame-mean
     * term lands on the base below, once the variances are known. */
    double base = e->qp_base;
    if (e->abr_on) {
        /* ABR: the base is the rate factor rc.c solved for this frame from
         * the bits spent against the bits planned (docs/instruments.md). */
        base = e->rc_rf;
    } else if (e->crf_on) {
        double shift = 0.0;
        if (e->cutree) { const char *v = getenv("N265_CRF_SHIFT"); shift = v ? atof(v) : 5.4; }
        base = e->crf + shift;
    }
    /* the pyramid QP cascade: I anchors -3, P anchors +0, and each B layer
     * one step deeper than its parent (the reference B nearest the anchors
     * codes best, the leaves worst). N265_PB_STEP scales the per-layer
     * increment. */
    double pb = 1.0; { const char *v = getenv("N265_PB_STEP"); if (v) pb = atof(v); }   /* live in CQP and CRF alike */
    int cascade = slice_type == 2 ? -3 : slice_type == 0 ? (int)lround(1 + b_depth * pb) : 0;
    /* The signalled base QP is an integer, so a control variable that only
     * moves it is quantised to 5-15% of the rate per step and cannot land on
     * a size. Carry the base's fraction instead and spend it over the CTUs
     * below; qfrac is exactly 0 when the fractional path is off, which is
     * what keeps CQP and CRF byte-identical to the pre-ABR encoder. */
    int qi = (int)lround(base);
    double qfrac = e->qp_frac ? base - (double)qi : 0.0;
    int q = qi + cascade;
    fw->qp = q < 0 ? 0 : q > 51 ? 51 : q;

    if (e->frames_done == 0) {
        /* The headers are a pure function of config, so they are written
         * here and packed when this frame retires: under overlap the
         * output ring is walked in coding order, not in completion
         * order, and the VPS/SPS/PPS have to lead the first slice. */
        fw->emit_headers = 1;
        e->nhdr = 0;
        n265_write_vps(fw); stash_header(e, N265_NAL_VPS, fw);
        n265_write_sps(fw); stash_header(e, N265_NAL_SPS, fw);
        n265_write_pps(fw); stash_header(e, N265_NAL_PPS, fw);
    }
    fw->nal_type = fw->slice_type == 2 ? N265_NAL_IDR_W_RADL
                                       : (nonref ? 0 /* TRAIL_N */
                                                 : 1 /* TRAIL_R */);
    fw->rc_base = base;
    fw->job_R = 0;
    fw->ref_lag = 0;
    if (fw->mode != N265_MODE_PCM) {
        memset(fw->edge_map, 0, (size_t)fw->map_stride * (fw->pad_h >> 2));
        memset(fw->blk_intra, 0, (size_t)fw->map_stride * (fw->pad_h >> 2));
        memset(fw->blk_skip, 0, (size_t)fw->map_stride * (fw->pad_h >> 2));
        memset(fw->blk_cbf_y, 0, (size_t)fw->map_stride * (fw->pad_h >> 2));
        memset(fw->blk_pred, 0, (size_t)fw->map_stride * (fw->pad_h >> 2));
        memset(fw->blk_mv, 0,
               (size_t)fw->map_stride * (fw->pad_h >> 2) * 4 * sizeof(int16_t));
        /* per-CTU QP offsets: cu-tree (filled by the scheduler) plus
         * variance AQ on top */
        if (!e->cutree) {
            memset(fw->ctu_qp_off, 0, (size_t)fw->ctb_cols * fw->ctb_rows);
            memset(fw->ctu_qp_raw, 0, (size_t)fw->ctb_cols * fw->ctb_rows * sizeof(float));
        }
        if (e->aq && getenv("N265_AQ_FLAT"))
            memset(fw->ctu_qp_off, (uint8_t)atoi(getenv("N265_AQ_FLAT")),
                   (size_t)fw->ctb_cols * fw->ctb_rows);
        else if (e->aq) {
            int ctb = 1 << fw->ctb_log2;
            int nctu = fw->ctb_cols * fw->ctb_rows;
            double *lv = malloc((size_t)nctu * sizeof(double));
            double avg = 0;
            for (int cy = 0; cy < fw->ctb_rows; cy++)
                for (int cx = 0; cx < fw->ctb_cols; cx++) {
                    int x0 = cx * ctb, y0 = cy * ctb;
                    int w = x0 + ctb > fw->pad_w ? fw->pad_w - x0 : ctb;
                    int h = y0 + ctb > fw->pad_h ? fw->pad_h - y0 : ctb;
                    int64_t s1 = 0, s2 = 0;
                    for (int y = y0; y < y0 + h; y++)
                        for (int x = x0; x < x0 + w; x++) {
                            int v = fw->pad_plane[0][y * fw->pad_stride[0] + x];
                            s1 += v;
                            s2 += v * v;
                        }
                    int np = w * h;
                    double var = (double)s2 / np -
                        ((double)s1 / np) * ((double)s1 / np);
                    lv[cy * fw->ctb_cols + cx] = log2(var + 1.0);
                    avg += lv[cy * fw->ctb_cols + cx];
                }
            avg /= nctu;
            /* Under CRF the frame mean of the AQ field is the across-frame /
             * across-shot allocation term (yah264 row 10, 2026-09-05): a busy
             * frame codes a little cheaper, a quiet one a little better. It
             * goes on the BASE QP at its own strength (N265_AQ_DC, default
             * 1.0) against an absolute anchor in log2(variance) units
             * (N265_AQ_ANCHOR, default 6.4), so every base-keyed decision sees
             * it; the per-CTU part below stays centred on the frame. */
            if (e->crf_on || e->abr_on) {
                const char *v = getenv("N265_AQ_DC"), *a = getenv("N265_AQ_ANCHOR");
                double dc = (v ? atof(v) : 1.0) * (avg - (a ? atof(a) : 6.4));
                int dci = (int)lround(dc);
                int nq = fw->qp + dci;
                fw->qp = nq < 0 ? 0 : nq > 51 ? 51 : nq;
                if (e->qp_frac)
                    qfrac += dc - (double)dci;
            }
            /* per-CTU term at N265_AQ_STR. Default: round it on its own and
             * add it to the already-rounded cu-tree offset (two roundings);
             * N265_QP_ROUND1 sums the two float fields and rounds once, so
             * fractional strengths on either side survive. */
            /* The per-CTU term is a perceptual correction, not a rate
             * lever, and it is proportional to the frame's variance SPREAD,
             * which is unbounded: flat animation puts black CTUs and drawn
             * detail in the same frame and the term reaches +-4 QP, taxing
             * exactly the CTUs the metric reads. N265_AQ_CLAMP bounds it
             * before the cu-tree field is added. 12 is the old effective
             * bound (the offset byte's own clamp); 3 ships (2026-09-08) and
             * is inert wherever the spread is small - four of the seven CIF
             * clips are byte-identical under it and the worst moves 0.43. */
            double aqc = 3.0;
            { const char *v = getenv("N265_AQ_CLAMP"); if (v) aqc = atof(v); }
            for (int i = 0; i < nctu; i++) {
                double off = e->aq_str * (lv[i] - avg);
                if (off > aqc) off = aqc;
                else if (off < -aqc) off = -aqc;
                int o;
                if (e->qp_round1) {
                    off += fw->ctu_qp_raw[i];
                    o = (int)(off > 0 ? off + 0.5 : off - 0.5);
                } else {
                    o = (int)(off > 0 ? off + 0.5 : off - 0.5) +
                        fw->ctu_qp_off[i];
                }
                fw->ctu_qp_off[i] = (int8_t)(o < -12 ? -12 : o > 12 ? 12 : o);
            }
            free(lv);
        }

        /* Spend the base QP's fractional part over the CTUs: each one is
         * rounded against its own threshold from a low-discrepancy
         * sequence, so the frame's MEAN QP is the float base to within one
         * CTU and the achieved rate is continuous in the control variable.
         * The threshold is a pure function of the CTU raster index and the
         * frame counter - not of a running error - so it stays
         * position-independent and survives any future thread schedule. */
        if (e->qp_frac && qfrac != 0.0) {
            int nctu = fw->ctb_cols * fw->ctb_rows;
            for (int i = 0; i < nctu; i++) {
                double d = (double)(i + 1) * 0.7548776662466927 +
                           (double)(e->frames_done + 1) * 0.5698402909980532;
                d -= floor(d);
                int o = fw->ctu_qp_off[i] + (int)floor(qfrac + d);
                fw->ctu_qp_off[i] = (int8_t)(o < -12 ? -12 : o > 12 ? 12 : o);
            }
        }

        if (getenv("N265_QP_TRACE"))
            fprintf(stderr, "QPT disp=%d type=%d qp=%d qf=%+.3f\n",
                    fw->display_idx, slice_type, fw->qp, qfrac);
        fw->ent_qp_prev = fw->qp;
        fw->ent_parent_chroma = 0;
        fw->rec_len = 0;
        fw->rec_pos = 0;
        for (int i = 0; i < CTX_COUNT; i++)
            n265_cabac_ctx_init(&fw->est_ctx[i],
                                fw->slice_type == 2 ? n265_ctx_init_i[i]
                                : fw->slice_type == 1 ? n265_ctx_init_p[i]
                                                     : n265_ctx_init_b[i],
                                fw->qp);
        /* The row wavefront of threading-design sec 1 on the pool, plus
         * the per-row publish (G4) and the entropy task. The readiness
         * predicates are the design's; every thread count and every K
         * runs the same task set in the same data order. */
        fw->job_R = fw->ctb_rows;
        fw->job_cols = fw->ctb_cols;
        fw->job_snap_x = fw->ctb_cols > 1 ? 1 : 0;  /* the WPP geometry */
        fw->job_sao_on = n265_sao_enabled();
        /* G1's hint: LAG = 1 + ceil((merange + 5) / ctb_size), sec 1's
         * own formula, kept as the row-start heuristic. */
        fw->ref_lag = fw->slice_type == 2 ? 0
            : 1 + ((57 + 5 + (1 << fw->ctb_log2) - 1) >> fw->ctb_log2);
        fw->qprev = fw->qp;
    }

    memset(fw->prog, 0, sizeof(*fw->prog) +
                        (size_t)fw->ctb_rows * sizeof(atomic_int));
    fw->job.ctx = fw;
    fw->job.ntask = 3 * fw->job_R + 1;      /* + the entropy task */
    fw->job.ready = job_ready;
    fw->job.run = job_run;
    fw->job.impl = NULL;
    fw->in_flight = 1;
    e->inflight[e->n_inflight++] = fw;
    (void)poc;
    ntp_submit(e->pool, &fw->job);
    e->frames_done++;
    atomic_fetch_add_explicit(&e->stall_frames, 1, memory_order_relaxed);
}

/* The per-frame entropy task: slice header, then the CTU syntax replayed
 * from the committed records through the real CABAC engine. Entropy
 * tasks of different frames are mutually independent (contexts
 * initialize per slice), so they parallelize across frames like any
 * other pool work; the output ORDER is restored by the deferred-NAL ring
 * at retirement, not here. */
static void entropy_frame(n265_encoder *e, n265_frame_work *fw)
{
    cabac_t cb;
    int last = fw->ctb_rows * fw->ctb_cols - 1;
    int nonref = fw->nonref;
    (void)nonref;
    fw->ent_qp_prev = fw->qp;
    fw->ent_parent_chroma = 0;
    fw->in_entropy = 1;
    bs_reset(&fw->rbsp);
    write_slice_header(e, fw);
    n265_cabac_start(&cb, &fw->rbsp);
    for (int i = 0; i < CTX_COUNT; i++)
        n265_cabac_ctx_init(&fw->ctx[i],
                            fw->slice_type == 2 ? n265_ctx_init_i[i]
                            : fw->slice_type == 1 ? n265_ctx_init_p[i]
                                                 : n265_ctx_init_b[i],
                            fw->qp);

    for (int i = 0; i <= last; i++) {
        int cx = i % fw->ctb_cols, cy = i / fw->ctb_cols;
        if (fw->mode == N265_MODE_PCM) {
            write_pcm_ctu(fw, &cb, &fw->ctx[CTX_PART_MODE], cx, cy);
        } else {
            if (cx == 0) {      /* this row's record stream */
                fw->rec_buf = fw->rrec[cy].buf;
                fw->rec_pos = 0;
            }
            n265_entropy_ctu(fw, &cb, cx, cy);
        }
        n265_cabac_terminate(&cb, i == last);
    }
    fw->in_entropy = 0;
    bs_align_zero(&fw->rbsp);


}

/* Join the oldest frame in flight: wait for its job, pack its NALs into
 * the deferred ring in CODING order, and hand its resources back. This
 * is the only place the driver blocks on a frame, and it is where D3's
 * stalls (a DPB slot, a lookahead source slot, a frame_work) resolve. */
static void join_oldest(n265_encoder *e)
{
    if (e->n_inflight == 0)
        return;
    n265_frame_work *fw = e->inflight[0];
    ntp_join(e->pool, &fw->job);
    struct n265_done *d = done_slot(e);
    if (fw->emit_headers)
        for (int i = 0; i < e->nhdr; i++)
            pack_nal(d, e->hdr_type[i], e->hdr_rbsp[i], e->hdr_len[i]);
    pack_nal(d, fw->nal_type, fw->rbsp.buf, fw->rbsp.bytes);
    e->n_done++;
    dump_recon_frame(e, fw);
    /* D4: the actual bits are applied on the driver, in coding order. */
    if (e->abr_on || e->rc.trace)
        n265_rc_done(e, fw->rc_cls, fw->rc_cplx, fw->rc_base,
                     (double)d->len * 8.0);
    fw->slot->in_use = 0;
    fw->emit_headers = 0;
    fw->in_flight = 0;
    for (int i = 1; i < e->n_inflight; i++)
        e->inflight[i - 1] = e->inflight[i];
    e->n_inflight--;
}

/* Build the reference lists for the picture that follows POC `cur`
 * (or, with `future` set, for a B whose L1 starts at `future`): L0 = valid
 * DPB pictures before cur, nearest first, up to ref_max; L1 = the future
 * picture then the past ones. A caller coding a B passes the past anchor
 * POC as `cur` so the L0 head is that anchor. */
void n265_build_lists(n265_encoder *e, n265_frame_work *fw, int poc,
                      int is_b, struct n265_refpic *future)
{
    (void)future;
    /* every valid DPB picture, split at `poc`: past = poc < this, nearest
     * (largest POC) first; future = poc > this, nearest (smallest) first.
     * L0 = past then future (for B); L1 = future then past (B only). */
    struct n265_refpic *past[N265_DPB_SLOTS], *fut[N265_DPB_SLOTS];
    int np = 0, nf = 0;
    /* D2: `marked` is the DPB as the decoder sees it, and it is
     * driver-serial. The sort below is a total order on POC - POCs are
     * unique among marked pictures, since an IDR clears the set - so
     * slot index never breaks a tie and list order cannot depend on
     * which slot a picture occupies. */
    for (int sl = 0; sl < N265_DPB_SLOTS; sl++) {
        if (!e->dpb[sl].marked) continue;
        if (e->dpb[sl].poc < poc) past[np++] = &e->dpb[sl];
        else if (e->dpb[sl].poc > poc) fut[nf++] = &e->dpb[sl];
    }
    for (int i = 1; i < np; i++)
        for (int k = i; k > 0 && past[k]->poc > past[k - 1]->poc; k--) { struct n265_refpic *t = past[k]; past[k] = past[k - 1]; past[k - 1] = t; }
    for (int i = 1; i < nf; i++)
        for (int k = i; k > 0 && fut[k]->poc < fut[k - 1]->poc; k--) { struct n265_refpic *t = fut[k]; fut[k] = fut[k - 1]; fut[k - 1] = t; }
    fw->nref[0] = fw->nref[1] = 0;
    for (int i = 0; i < np && fw->nref[0] < e->ref_max; i++) fw->refl[0][fw->nref[0]++] = past[i];
    if (is_b) for (int i = 0; i < nf && fw->nref[0] < e->ref_max; i++) fw->refl[0][fw->nref[0]++] = fut[i];
    if (is_b) {
        for (int i = 0; i < nf && fw->nref[1] < e->ref_max; i++) fw->refl[1][fw->nref[1]++] = fut[i];
        for (int i = 0; i < np && fw->nref[1] < e->ref_max; i++) fw->refl[1][fw->nref[1]++] = past[i];
    }
}

/* Is the lookahead frame at index i a forced key frame (IDR)? keyint,
 * a scene cut, or the encoder's first frame. */
static int la_is_idr(n265_encoder *e, int i)
{
    int d = e->la.f[i].display_idx;
    return e->mode == N265_MODE_PCM || e->keyint == 1 || (d % e->keyint) == 0 ||
           !e->ref_valid ||
           (e->scenecut && d - e->gop_start_display >= 4 && n265_la_scenecut(e, i));
}

/* Emit the pyramid coding order for the B lookahead indices [blo, bhi],
 * bracketed by already-coded references: the midpoint first (a reference if
 * it has children on either side), then its two halves. */
static void plan_brange(n265_encoder *e, int blo, int bhi, int depth)
{
    if (blo > bhi)
        return;
    int mid = (blo + bhi) / 2;
    int is_ref = (blo <= mid - 1) || (mid + 1 <= bhi);
    int k = e->plan_len++;
    e->plan_li[k] = mid; e->plan_kind[k] = 0; e->plan_depth[k] = depth;
    e->plan_nonref[k] = !is_ref;
    plan_brange(e, blo, mid - 1, depth + 1);
    plan_brange(e, mid + 1, bhi, depth + 1);
}

/* Build the coding-order plan for the mini-GOP at the front of the queue. */
static void build_plan(n265_encoder *e)
{
    n265_la *la = &e->la;
    e->plan_len = e->plan_pos = 0;
    if (la_is_idr(e, 0)) {
        e->gop_start_display = la->f[0].display_idx;
        for (int sl = 0; sl < N265_DPB_SLOTS; sl++) e->dpb[sl].marked = 0;
        e->ref_valid = 0;
        e->plan_li[0] = 0; e->plan_kind[0] = 2; e->plan_depth[0] = 0; e->plan_nonref[0] = 0;
        e->plan_len = 1; e->plan_pop = 1;
        return;
    }
    /* the anchor offset A: as many B frames as bframes, cut short by the
     * first forced key frame in the run (that frame opens the next GOP). */
    int maxa = e->bframes < la->count ? e->bframes : la->count - 1;
    int A = 0;
    /* adaptive B placement: extend the B run frame by frame, but end it at
     * the first frame whose lookahead motion x inter/intra ratio says a far
     * bidirectional prediction will code badly (high-motion pans hurt as
     * deep B; foliage/texture with low motion do not). N265_BADAPT_THR the
     * cutoff on (mean |lowres MV| per block) x (pcost/icost).
     *
     * The motion term is a count of LOWRES PIXELS, so the same physical pan
     * reads 3-5x larger at 720p/1080p than at the CIF the cutoff was
     * calibrated on, and the run collapses to one B frame on any HD clip
     * that moves. N265_BADAPT_REFW scales it back to a fraction of the
     * frame width: the term is multiplied by refw / lowres width, with
     * refw the width the cutoff is taken to hold at. The scale is CLAMPED
     * AT 1.0, so the normalisation can only ever relax the cutoff, never
     * tighten it: everything at or below refw - the whole CIF half of the
     * corpus, where the 0.35 was read - is byte-identical to the
     * pre-2026-09-08 encoder, and only the clips whose motion the term was
     * over-reading move. 0 turns it off (the pre-2026-09-08 encoder). */
    double bthr = 0.35; { const char *v = getenv("N265_BADAPT_THR"); if (v) bthr = atof(v); }
    double refw = 128.0; { const char *v = getenv("N265_BADAPT_REFW"); if (v) refw = atof(v); }
    double wscale = 1.0;
    if (refw > 0.0 && la->lw > 0 && (double)la->lw > refw)
        wscale = refw / (double)la->lw;
    int nb = la->bw * la->bh;
    int trace = getenv("N265_BADAPT_TRACE") != NULL;
    /* how far the key-frame edge lets the run go at all */
    int keymax = maxa;
    for (int i = 1; i <= maxa; i++) {
        if (la_is_idr(e, i)) { keymax = i - 1; break; }
    }
    /* b-adapt 2: price the mini-GOP instead of ruling on motion. The run
     * length is the one whose amortised lowres cost is lowest - one anchor
     * predicted at distance A+1 plus A bidirectionally-predicted leaves,
     * over A+1 frames. It has no calibrated constant in it. */
    if (e->badapt == 3 && keymax >= 1) {
        double dthr = 1.6;
        { const char *v = getenv("N265_BADAPT_DEG"); if (v) dthr = atof(v); }
        A = n265_la_degrade(e, keymax, dthr);
    } else if (e->badapt == 2 && keymax >= 1) {
        A = n265_la_brun(e, keymax);
    } else
    for (int i = 1; i <= maxa; i++) {
        if (la_is_idr(e, i)) break;
        A = i;
        if (e->badapt) {
            double mv = (double)la->f[i].mvabs_sum / (nb ? nb : 1);
            double m = mv * wscale * (double)la->f[i].sc_ratio;
            if (trace)
                fprintf(stderr, "BADAPT d=%d i=%d mv=%.3f ratio=%.3f "
                        "scale=%.3f m=%.4f thr=%.4f %s\n",
                        la->f[i].display_idx, i, mv,
                        (double)la->f[i].sc_ratio, wscale, m, bthr,
                        m > bthr ? "END" : "extend");
            if (m > bthr) break;    /* this frame ends the run (it is the anchor) */
        }
    }
    if (A == 0) {                       /* next frame is a key frame: front is a plain P */
        e->plan_li[0] = 0; e->plan_kind[0] = 1; e->plan_depth[0] = 0; e->plan_nonref[0] = 0;
        e->plan_len = 1; e->plan_pop = 1;
        return;
    }
    /* the anchor first, then the B pyramid between the anchors */
    e->plan_li[0] = A; e->plan_kind[0] = 1; e->plan_depth[0] = 0; e->plan_nonref[0] = 0;
    e->plan_len = 1;
    plan_brange(e, 0, A - 1, 1);
    e->plan_pop = A + 1;
}

static void join_oldest(n265_encoder *e);

/* D3: is this picture still being written, or still readable by a frame
 * that has not retired? The list is driver-owned, so this is a plain
 * read. */
static int slot_busy(n265_encoder *e, const struct n265_refpic *s)
{
    if (s->in_use)
        return 1;
    for (int i = 0; i < e->n_inflight; i++) {
        n265_frame_work *f = e->inflight[i];
        if (f->col == s)
            return 1;
        for (int l = 0; l < 2; l++)
            for (int k = 0; k < f->nref[l]; k++)
                if (f->refl[l][k] == s)
                    return 1;
    }
    return 0;
}

/* Claim the DPB slot this frame reconstructs into and point the
 * frame_work's recon planes at it. Same rule the pre-T0a store applied
 * after entropy: an unmarked slot, else the marked one with the oldest
 * POC that is not in this frame's reference lists. The rule sees only
 * `marked`, which is driver-serial, so the CHOICE is the serial
 * encoder's; D3 then STALLS (retires frames) rather than re-routing, so
 * slot identity still cannot reach the bitstream. */
static void claim_slot(n265_encoder *e, n265_frame_work *fw)
{
    struct n265_refpic *slot = NULL;
    if (fw->nonref || fw->mode == N265_MODE_PCM)
        slot = &e->scratch_pic[fw - e->fw];
    for (int sl = 0; sl < N265_DPB_SLOTS && !slot; sl++)
        if (!e->dpb[sl].marked) slot = &e->dpb[sl];
    for (int sl = 0; sl < N265_DPB_SLOTS && !slot; sl++) {
        int in_list = 0;
        for (int l = 0; l < 2; l++)
            for (int k = 0; k < fw->nref[l]; k++)
                if (fw->refl[l][k] == &e->dpb[sl]) in_list = 1;
        if (!in_list && (!slot || e->dpb[sl].poc < slot->poc)) slot = &e->dpb[sl];
    }
    if (!slot) slot = &e->dpb[0];
    /* D3: wait for the picture, never re-route to another one. */
    while (e->n_inflight && slot_busy(e, slot))
        join_oldest(e);
    slot->in_use = 1;
    /* `marked` is NOT cleared: when the pool is exhausted the picture
     * being overwritten stays listed in this frame's RPS exactly as it
     * did before the claim moved to dispatch (it is replaced when this
     * frame marks itself). That is a pre-existing DPB-pressure bug at
     * --ref 3 --bframes 7, reproduced on purpose so the ladder changes
     * no bits. */
    /* Physical readiness starts empty: the sec 1 reference gate reads
     * this watermark and nothing else, so it must not carry the previous
     * occupant's. */
    atomic_store_explicit(&slot->final_rows, 0, memory_order_release);
    /* E1: arm the half-pel cache here, on the driver; the publish row
     * task fills the lines it owns (G4). */
    if (fw->mode != N265_MODE_PCM && !fw->nonref)
        hf_arm(e, slot);
    fw->slot = slot;
    for (int i = 0; i < 3; i++) {
        fw->recon[i] = slot->plane[i];
        fw->recon_stride[i] = slot->stride[i];
    }
}

/* Encode one frame from the plan (building it first if none is active).
 * Returns the number of source frames retired from the queue (0 until the
 * plan completes, then the whole mini-GOP). */
static int drive_scheduler(n265_encoder *e)
{
    n265_la *la = &e->la;
    if (la->count == 0)
        return 0;
    if (e->plan_pos >= e->plan_len)
        build_plan(e);

    /* K: a free frame_work, by joining the oldest in flight. This and
     * the D3 stalls below are the only places the driver blocks. */
    while (e->n_inflight >= e->K)
        join_oldest(e);

    int li = e->plan_li[e->plan_pos];
    int kind = e->plan_kind[e->plan_pos];
    la_frame *f = &la->f[li];
    int poc = f->display_idx - e->gop_start_display;
    n265_frame_work *fw = &e->fw[e->fw_next];
    e->fw_next = e->fw_next + 1 >= e->K ? 0 : e->fw_next + 1;

    /* the queue slot IS the padded source (threading-design sec 4) */
    for (int i = 0; i < 3; i++)
        fw->pad_plane[i] = f->src[i];

    memset(fw->ctu_qp_off, 0, (size_t)fw->ctb_cols * fw->ctb_rows);
    if (e->cutree)
        n265_la_cutree(e, fw, li);
    /* reference lists from the DPB around this frame's POC (anchors have
     * only L0; B frames have both) */
    n265_build_lists(e, fw, poc, kind == 0, NULL);
    int slice_type = kind == 2 ? 2 : kind == 1 ? 1 : 0;
    fw->slice_type = slice_type;
    fw->poc = poc;
    fw->nonref = e->plan_nonref[e->plan_pos];
    fw->display_idx = f->display_idx;
    fw->b_depth = e->plan_depth[e->plan_pos];
    fw->ref_valid = e->ref_valid;
    fw->sc_ratio = f->sc_ratio;     /* lookahead pcost/icost, P3 reads it */
    fw->la_icost = f->icost;        /* the same field per CTU, R3 reads it */
    fw->la_pcost = f->pcost;
    fw->la_bw = la->bw;
    fw->la_bh = la->bh;
    /* D1: freeze the RPS here, while the marked set is still the one the
     * serial encoder's header writer would have seen. Before the claim,
     * so the picture cannot appear in its own RPS. */
    snapshot_rps(e, fw);
    /* D2: the TMVP collocated picture is L0[0], frozen with the lists.
     * The coding path reads fw->col, never a live DPB slot's marking. */
    fw->col = (fw->slice_type != 2 && fw->nref[0] > 0) ? fw->refl[0][0] : NULL;
    fw->col_poc = fw->col ? fw->col->poc : 0;
    /* Claim the DPB slot the picture reconstructs into, by the rule the
     * store used to apply after entropy. Nothing between here and there
     * changes the marked set, so the choice is the one the pre-T0a
     * encoder made; the slot is not marked until the end of this
     * dispatch step, so neither this frame's RPS nor its lists see it. */
    claim_slot(e, fw);
    /* D2: mark now, on the driver, in coding order. Frame k+1's dispatch
     * has to see this picture in the DPB whether or not a row of it has
     * run; `final_rows` is what says whether the pixels exist. */
    if (fw->mode != N265_MODE_PCM && !fw->nonref) {
        fw->slot->poc = poc;
        fw->slot->is_anchor = slice_type != 0;  /* I/P; a reference B is not */
        fw->slot->marked = 1;
        e->ref_valid = 1;
    }
    /* the collocated picture's own reference POCs, driver state that
     * used to be written by the whole-frame publish pass */
    fw->slot->col_ref_poc[0] = fw->ref_valid && fw->nref[0] > 0
                                   ? fw->refl[0][0]->poc : 0;
    fw->slot->col_ref_poc[1] = (fw->slice_type == 0 && fw->nref[1] > 0)
                                   ? fw->refl[1][0]->poc
                                   : fw->slot->col_ref_poc[0];
    fw->rc_cls = n265_rc_class(slice_type, fw->nonref);
    fw->rc_cplx = n265_rc_cplx(e, li, fw->rc_cls);
    if (e->abr_on) {
        /* D4 at D = 0: frame k's rate factor is solved from the ACTUAL
         * bits of every frame before it, so the driver applies those
         * first (rc_done runs in retire_frame, in coding order). ABR
         * therefore does not overlap frames at D = 0 - by construction,
         * and identically at every thread count - while CQP and CRF
         * launch immediately and overlap fully. */
        while (e->n_inflight)
            join_oldest(e);
        e->rc_rf = n265_rc_next(e, fw->rc_cls, fw->rc_cplx);
    }
    if (kind != 0)          /* the priced b-adapt prices against this plane */
        n265_la_set_anchor(e, li);
    dispatch_frame(e, fw);
    e->plan_pos++;

    if (e->plan_pos >= e->plan_len) {
        int pop = e->plan_pop;
        for (int i = 0; i < pop; i++) n265_la_pop(e);
        /* the mini-GOP is done: reference B frames live only within it, and
         * only ref_max anchors carry into the next one. Retire the rest so
         * the DPB (and the next slice's RPS) hold exactly what is reusable. */
        /* Retirement is driver-serial and keyed on POC: the trim below
         * drops the SMALLEST POC each time, and POCs are unique among
         * marked pictures, so slot index never decides who goes. */
        for (int sl = 0; sl < N265_DPB_SLOTS; sl++)
            if (e->dpb[sl].marked && !e->dpb[sl].is_anchor) e->dpb[sl].marked = 0;
        int nanchor = 0;
        for (int sl = 0; sl < N265_DPB_SLOTS; sl++) if (e->dpb[sl].marked) nanchor++;
        while (nanchor > e->ref_max) {
            int oldest = -1;
            for (int sl = 0; sl < N265_DPB_SLOTS; sl++)
                if (e->dpb[sl].marked && (oldest < 0 || e->dpb[sl].poc < e->dpb[oldest].poc)) oldest = sl;
            e->dpb[oldest].marked = 0; nanchor--;
        }
        return pop;
    }
    return 0;
}

int n265_encoder_encode(n265_encoder *e, n265_nal **nal, int *nnal,
                        const n265_picture *pic)
{
    if (!e)
        return -1;
    *nal = e->nal;
    *nnal = 0;
    size_t out_used = 0;
    int count = 0;

    if (pic) {
        /* D3: n265_la_push is about to overwrite a recycled queue slot,
         * and a frame_work aliases its source planes. Nothing may still
         * be reading it. */
        while (e->n_inflight)
            join_oldest(e);
        const uint8_t *planes[3] = { pic->plane[0], pic->plane[1],
                                     pic->plane[2] };
        n265_la_push(e, planes, pic->stride);
        e->in_display_idx++;
        /* One call codes a whole mini-GOP (the loop only exits when the
         * mini-GOP-end pop brings the queue back under la_depth), so the
         * overlap window is the mini-GOP, capped at K. */
        while (e->la.count > e->la_depth)
            drive_scheduler(e);
        while (e->n_inflight)
            join_oldest(e);
        emit_done(e, INT_MAX, &out_used, &count);
    } else {
        /* Flush: the serial encoder took one scheduler step per call and
         * returned that frame. So does this one - but it DISPATCHES
         * ahead, up to K, so the tail of the sequence overlaps like the
         * rest of it. Dispatching ahead is confined to the plan the
         * driver has already built: plan building and la_pop sit at
         * exactly the same point in the dispatch sequence they sat at
         * serially, so every b-adapt, scenecut and cu-tree decision sees
         * the queue it saw before. */
        int ring = (int)(sizeof(e->done) / sizeof(e->done[0]));
        while (e->la.count > 0 && e->n_inflight < e->K &&
               e->n_done + e->n_inflight < ring - 2)
            drive_scheduler(e);
        if (e->n_done == 0 && e->n_inflight)
            join_oldest(e);
        emit_done(e, 1, &out_used, &count);
    }
    for (int i = 0; i < count; i++)
        e->nal[i].payload = e->out + e->nal_off[i];

    *nnal = count;
    return (int)out_used;
}
