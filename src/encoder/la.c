/*
 * Lookahead implementation: 2x2-mean lowres, per-8x8 SATD-lite intra
 * proxy and small-diamond inter cost, scenecut by cost ratio, MB-tree
 * style cu-tree propagation over the queued window feeding the per-CTU QP
 * offset map. Serial and deterministic; the threading ladder moves this
 * onto the pool at T3/L3.
 * Copyright (c) 2026, the yah265 authors. GPL-2.0-or-later; see LICENSE.
 */

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "encoder/encoder.h"
#include "encoder/la.h"
#include "encoder/preset.h"
#include "encoder/satd.h"

/* Share of whole 16x16 source luma blocks with variance under one 8-BIT
 * luma level, in percent. A variance under one level is a standard
 * deviation under a single luma level: a block that is one flat colour to
 * within the smallest step the quantiser reproduces. The walk is over the
 * DISPLAYED picture,
 * not the coded one - conformance padding is replicated and therefore
 * perfectly flat, and a picture 722 samples tall would otherwise get a
 * row of blocks that are 14/16 padding and vote flat whatever the
 * content is. Integer throughout: this has to be a bit-exact function of
 * the source on every box.
 *
 * Per frame over the fifteen-clip corpus, 120 frames each (2026-09-11),
 * the flattest natural frame reads 6.1% (foreman_cif) and the least flat
 * animation frame reads 13.8% (sita_toon_720p), with the three animation
 * clips' medians at 23.5 / 67.8 / 79.0 against 4.0 for the flattest
 * natural median. The default threshold of 10 sits in that gap with no
 * frame of any clip closer than a factor of 1.4 on the wrong side, which
 * is what makes this a content TYPE and not a threshold fitted to a
 * curve.
 *
 * At Main10 the UNIT is kept, not the number: "one luma level" stays one
 * 8-BIT level, which is four sample steps at 10 bits, so the variance
 * bound scales by 1 << (2 * BIT_DEPTH_SHIFT) and the range reject - which
 * is a magnitude - by 1 << BIT_DEPTH_SHIFT. That is the only reading that
 * makes an upshifted clip type the same frames flat as its 8-bit
 * original, which is the property the corpus numbers above were measured
 * under and the one that keeps the SAO content gate deciding the same
 * way at both depths. Rescaling by the wrong power, or leaving the
 * literals alone, would have made every 10-bit frame flat (the bound
 * sixteen times too loose) or none of them (sixteen times too tight). */
#define FLAT_RANGE_MAX  (32 << BIT_DEPTH_SHIFT)      /* 32 at 8, 128 at 10 */
#define FLAT_VAR_BOUND  (65536ull << (2 * BIT_DEPTH_SHIFT))

int n265_la_flat_pct(const struct n265_frame_work *fw)
{
    const pixel *p = fw->pad_plane[0];
    int stride = fw->pad_stride[0];
    int bw = fw->param.width >> 4, bh = fw->param.height >> 4;
    if (bw <= 0 || bh <= 0)
        return 0;
    int flat = 0;
    for (int by = 0; by < bh; by++)
        for (int bx = 0; bx < bw; bx++) {
            const pixel *b = p + (by << 4) * stride + (bx << 4);
            uint32_t s1 = 0, s2 = 0;
            int lo = PIXEL_MAX, hi = 0;
            for (int y = 0; y < 16; y++) {
                const pixel *r = b + y * stride;
                for (int x = 0; x < 16; x++) {
                    int v = r[x];
                    s1 += (uint32_t)v;
                    s2 += (uint32_t)(v * v);
                    if (v < lo) lo = v;
                    if (v > hi) hi = v;
                }
                /* Exact early reject, and on natural video it fires on
                 * the first row of nearly every block. For any mean,
                 * one of the two extremes is at least (hi - lo) / 2
                 * away from it, so 256 * var >= ((hi - lo) / 2)^2 and a
                 * range of 32 (of 128 at 10 bits) or more cannot be a
                 * variance under one 8-bit level. */
                if (hi - lo >= FLAT_RANGE_MAX)
                    break;
            }
            /* var < T  <=>  256*s2 - s1^2 < 256*256*T, T one 8-bit level
             * squared in native sample units */
            if (hi - lo < FLAT_RANGE_MAX &&
                (uint64_t)s2 * 256u - (uint64_t)s1 * s1 < FLAT_VAR_BOUND)
                flat++;
        }
    return flat * 100 / (bw * bh);
}

/* N265_FLAT_GATE: the flat-block share, in percent, at or above which a
 * frame is typed flat-shaded. 0 disables the type entirely and is the
 * pre-2026-09-11 encoder, byte for byte; a value over 100 arms the SPS
 * flag without ever firing, which is what the conformance vector for
 * "SAO allowed in the sequence, off in every slice" uses. */
int n265_flat_gate(void)
{
    static int v = -1;
    if (v < 0) {
        const char *s = n265_env("N265_FLAT_GATE");
        v = s ? atoi(s) : 10;
        if (v < 0) v = 0;
    }
    return v;
}

int n265_la_init(struct n265_encoder *e)
{
    n265_la *la = &e->la;
    /* The cost loops read whole 8x8 lowres blocks (anchored at lw - 8 on the
     * right and bottom edges), so the plane is never smaller than one
     * block; the downscale below edge-replicates into the padding. A 8x8
     * input overran a 4x4 plane here (the sanitiser matrix, 2026-09-06). */
    la->lw = e->pad_w / 2 < 8 ? 8 : e->pad_w / 2;
    la->lh = e->pad_h / 2 < 8 ? 8 : e->pad_h / 2;
    la->bw = (la->lw + 7) / 8;
    la->bh = (la->lh + 7) / 8;
    la->depth = e->la_depth;
    /* fill + 1 live slots, plus a free run longer than the slots the
     * frames in flight can hold - K rounded up to a mini-GOP, because
     * pops happen at mini-GOP ends (see la.h). `e->la_fill` is the
     * lookahead depth at the default and `--shot-window` where that is
     * deeper: the SLOT ARRAY is sized from the shape the encoder was
     * opened with rather than declared statically at the worst case, so
     * the opt-in mode pays for its own queue and every default
     * allocation is exactly the one it has always been. */
    la->cap = e->la_fill + 2 + e->K + e->bframes;
    la->f = calloc((size_t)la->cap, sizeof(la_frame));
    if (!la->f)
        return -1;
    for (int i = 0; i < la->cap; i++) {
        la_frame *f = &la->f[i];
        for (int c = 0; c < 3; c++) {
            int w = c ? e->pad_w >> e->csx : e->pad_w;
            int h = c ? e->pad_h >> e->csy : e->pad_h;
            f->src[c] = malloc((size_t)w * h * sizeof(pixel));
            if (!f->src[c])
                return -1;
        }
        if (i == 0) {
            la->anchor_lowres = malloc((size_t)la->lw * la->lh
                                       * sizeof(pixel));
            if (!la->anchor_lowres)
                return -1;
            la->anchor_valid = 0;
            size_t mvb = (size_t)la->bw * la->bh * 2 * sizeof(int16_t);
            size_t sb = (size_t)la->bw * la->bh * sizeof(uint32_t);
            /* one per candidate run length. The floor of 8 is what the
             * default shape (bframes 7) has always allocated, so its
             * footprint does not move; only --bframes above 7 pays. */
            la->pc_n = e->bframes + 1 < 8 ? 8 : e->bframes + 1;
            if (la->pc_n > N265_MAX_BFRAMES + 1)
                la->pc_n = N265_MAX_BFRAMES + 1;
            for (int k = 0; k < la->pc_n; k++) {
                if (!(la->pc_mv[k] = malloc(mvb)))
                    return -1;
                if (!(la->pc_sad[k] = malloc(sb)))
                    return -1;
            }
            if (!(la->pc_bmv = malloc(mvb)))
                return -1;
            if (!(la->pc_bsad = malloc(sb)) || !(la->pc_bisad = malloc(sb)))
                return -1;
        }
        f->lowres = malloc((size_t)la->lw * la->lh * sizeof(pixel));
        int nb = la->bw * la->bh;
        f->icost = malloc((size_t)nb * sizeof(uint16_t));
        f->pcost = malloc((size_t)nb * sizeof(uint16_t));
        f->mv = malloc((size_t)nb * 2 * sizeof(int16_t));
        f->prop = malloc((size_t)nb * sizeof(float));
        if (!f->lowres || !f->icost || !f->pcost || !f->mv || !f->prop)
            return -1;
    }
    return 0;
}

void n265_la_free(struct n265_encoder *e)
{
    /* Reached from the open path's failure label too, where the slot
     * array may be absent and the slots it holds half-filled: it is
     * calloc'd, so every pointer below is either live or NULL. */
    for (int i = 0; e->la.f && i < e->la.cap; i++) {
        la_frame *f = &e->la.f[i];
        for (int c = 0; c < 3; c++)
            free(f->src[c]);
        free(f->lowres);
        free(f->icost);
        free(f->pcost);
        free(f->mv);
        free(f->prop);
    }
    free(e->la.f);
    e->la.f = NULL;
    free(e->la.anchor_lowres);
    e->la.anchor_lowres = NULL;
    for (int k = 0; k < N265_MAX_BFRAMES + 1; k++) {
        free(e->la.pc_mv[k]);
        e->la.pc_mv[k] = NULL;
        free(e->la.pc_sad[k]);
        e->la.pc_sad[k] = NULL;
    }
    free(e->la.pc_bmv);
    e->la.pc_bmv = NULL;
    free(e->la.pc_bsad);
    e->la.pc_bsad = NULL;
    free(e->la.pc_bisad);
    e->la.pc_bisad = NULL;
}

/* the lookahead's own name for the block SAD; the loop moved to
 * n265_sad_wh (satd.c) when the family got a NEON form */
static uint32_t sad8(const pixel *a, int as, const pixel *b, int bs)
{
    return n265_sad_wh(a, as, b, bs, 8, 8);
}

/* N265_SC_HOLD: a HELD frame does not set the running level.
 * sc_ema is the level the ratio has to spike over, and it is meant to
 * be the shot's own inter difficulty. Limited animation holds a frame
 * for two or three frames and then jumps, and a held frame predicts
 * exactly - its pcost is the per-block floor, its ratio is near zero,
 * and it carries no evidence at all about how hard the shot is to
 * predict. Averaging those into the level drags it to a fraction of
 * the moving frames' own ratio, after which every frame that moves
 * clears 1.3x the level and reads as a cut. A frame whose mean lowres
 * SAD is under one unit per 8x8 block is a repeat, not a measurement.
 * On by default since 2026-09-09; 0 is the behaviour before it, and
 * every clip in the corpus that is single-shot at 120 frames is
 * byte-identical under either value.
 *
 * An accessor and not an inline static inside n265_la_push, because a
 * warm can only reach a gate through a callable function. */
static int sc_hold(void)
{
    static int hold = -1;
    if (hold < 0) { const char *v = n265_env("N265_SC_HOLD"); hold = v ? atoi(v) : 1; }
    return hold;
}

static void log2q16_init(void);
static int32_t brun_logT_q16(int qp);

/* N265_BRUN_TRACE, hoisted into an accessor rather than read inline: a
 * warm can only reach a gate through a callable function
 * (scripts/env_gate_audit.py), and the trellis is priced per mini-GOP,
 * where an uncached getenv is what the phase 6 profile charged for. */
int n265_brun_trace(void)
{
    static int v = -1;
    if (v < 0)
        v = n265_env("N265_BRUN_TRACE") != NULL;
    return v;
}

/* The duplicate path's knobs; la.h says what each one is. The arm and
 * the census both need the detector, so either one arms it. */
int n265_dup_fast(void)
{
    static int v = -1;
    if (v < 0) {
        const char *s = n265_env("N265_DUP_FAST");
        v = s ? atoi(s) : 0;
    }
    return v;
}

int n265_dup_trace(void)
{
    static int v = -1;
    if (v < 0)
        v = n265_env("N265_DUP_TRACE") != NULL;
    return v;
}

/* --dup-threshold, when the command line gave one: 1/256 of a luma level
 * per lowres pixel, negative for "not given". It is read before the env
 * and is NOT cached, so a setter call cannot land behind a stale static;
 * the CLI calls it once at open, before any worker exists. */
static int g_dup_thr_q8 = -1;

void n265_set_dup_thr(double thr)
{
    if (thr < 0.0) {
        g_dup_thr_q8 = -1;
        return;
    }
    if (thr > 255.0)
        thr = 255.0;
    g_dup_thr_q8 = (int)(thr * 256.0 + 0.5);
}

/* the bound in 1/256 of an 8-bit luma level per lowres pixel */
int n265_dup_thr_q8(void)
{
    if (g_dup_thr_q8 >= 0)
        return g_dup_thr_q8;
    static int v = -1;
    if (v < 0) {
        const char *s = n265_env("N265_DUP_THR");
        double t = s ? atof(s) : 0.0;
        if (t < 0.0)
            t = 0.0;
        if (t > 255.0)
            t = 255.0;
        v = (int)(t * 256.0 + 0.5);
    }
    return v;
}

int n265_dup_on(void)
{
    return n265_dup_fast() || n265_dup_trace();
}

/* the driver resolves this file's knobs at open, before a worker can be
 * the one that writes them first; see n265_ctu_warm */
void n265_la_warm(void)
{
    (void)sc_hold();
    (void)n265_flat_gate();
    log2q16_init();
    (void)brun_logT_q16(32);
    (void)n265_brun_trace();
    (void)n265_dup_on();
    (void)n265_dup_thr_q8();
    /* --film-grain's gates. They are read on the driver and nowhere else,
     * so nothing races them even unwarmed; they are warmed here because
     * the audit's rule is about the CLASS and not about this knob's luck
     * (scripts/env_gate_audit.py). */
    (void)n265_fgc_pct();
    (void)n265_fgc_frames();
    (void)n265_fgc_chroma();
    (void)n265_fgc_core();
    (void)n265_fgc_ratio();
    (void)n265_fgc_min();
    (void)n265_fgc_trace();
}

/* the step the duplicate chain's running total takes where a frame had
 * no predecessor to be measured against; see n265_la_push */
#define DUP_BREAK ((uint64_t)1 << 42)

/* Do two queued frames carry the same source, sample for sample? The
 * padded planes are compared rather than the displayed ones: the padding
 * is an edge replication of the source, so the two are equal exactly
 * when the sources are, and the padded plane is one contiguous block.
 * Called only where the lowres zero-vector SAD is already exactly zero,
 * which on a clip without duplicates never happens. */
static int src_identical(const struct n265_encoder *e, const la_frame *a,
                         const la_frame *b)
{
    for (int c = 0; c < 3; c++) {
        int w = c ? e->pad_w >> e->csx : e->pad_w;
        int h = c ? e->pad_h >> e->csy : e->pad_h;
        if (memcmp(a->src[c], b->src[c], (size_t)w * h * sizeof(pixel)))
            return 0;
    }
    return 1;
}

void n265_la_push(struct n265_encoder *e, const pixel *const plane[3],
                  const int stride[3])
{
    n265_la *la = &e->la;
    la_frame *f = &la->f[la->count];
    la_frame *prev = la->count > 0 ? &la->f[la->count - 1] : NULL;
    f->valid = 1;
    f->display_idx = e->in_display_idx;
    /* a recycled slot carries the last frame's plan; this one has none
     * until the shot planner reaches it */
    f->shot_off = 0.0f;
    f->shot_planned = 0;

    /* Edge-replicate into the padded geometry here, once. encode_frame
     * used to re-pad this into a second frame-sized buffer every frame;
     * the queue slot IS the padded source now (threading-design sec 4). */
    for (int c = 0; c < 3; c++) {
        int w = c ? e->param.width >> e->csx : e->param.width;
        int h = c ? e->param.height >> e->csy : e->param.height;
        int dw = c ? e->pad_w >> e->csx : e->pad_w;
        int dh = c ? e->pad_h >> e->csy : e->pad_h;
        for (int y = 0; y < dh; y++) {
            const pixel *s = plane[c] + (y < h ? y : h - 1) * stride[c];
            pixel *d = f->src[c] + y * dw;
            memcpy(d, s, (size_t)w * sizeof(pixel));
            for (int x = w; x < dw; x++)     /* a sample fill, not a byte one */
                d[x] = s[w - 1];
        }
    }

    /* --film-grain (phase 12 item 3). The estimate is taken HERE, on the
     * source as it arrived and before anything else has read it, and the
     * denoise - which is the half that saves bits - is applied in the same
     * place, so everything downstream (the lowres plane, the costs, the
     * cu-tree, mode decision) sees exactly the picture that will be coded.
     * Driver-serial and integer, so the model is a pure function of the
     * source at every thread count.
     *
     * Off unless --film-grain asked for it, and at 0 this block is a
     * single predicate on a field that is 0. */
    if (e->fgc_strength > 0.0) {
        const pixel *pl[3] = { f->src[0], f->src[1], f->src[2] };
        int st[3] = { e->pad_w, e->pad_w >> e->csx, e->pad_w >> e->csx };
        n265_grain_accum(e->fgc_est, pl, st);
        if (!e->fgc_ready && ++e->fgc_nacc >= n265_fgc_frames()) {
            n265_grain_finish(e->fgc_est, &e->fgc, e->fgc_strength,
                              n265_fgc_chroma());
            e->fgc_ready = 1;
        }
        if (e->fgc_denoise && e->fgc_ready && e->fgc.present) {
            for (int c = 0; c < 3; c++) {
                int w = c ? e->param.width >> e->csx : e->param.width;
                int h = c ? e->param.height >> e->csy : e->param.height;
                int dw = c ? e->pad_w >> e->csx : e->pad_w;
                int dh = c ? e->pad_h >> e->csy : e->pad_h;
                n265_grain_denoise(&e->fgc, f->src[c], dw, w, h, c);
                /* the pad is an edge replication of the CODED picture, and
                 * the coded picture just moved: redo it rather than leave
                 * the filter's own edge behaviour standing in for it */
                for (int y = 0; y < h; y++) {
                    pixel *d = f->src[c] + (size_t)y * dw;
                    for (int x = w; x < dw; x++)
                        d[x] = d[w - 1];
                }
                for (int y = h; y < dh; y++)
                    memcpy(f->src[c] + (size_t)y * dw,
                           f->src[c] + (size_t)(h - 1) * dw,
                           (size_t)dw * sizeof(pixel));
            }
        }
    }

    /* lowres: 2x2 mean over the padded width (edge rows repeat source) */
    int lw = la->lw, lh = la->lh;
    for (int y = 0; y < lh; y++)
        for (int x = 0; x < lw; x++) {
            int sy = 2 * y, sx = 2 * x;
            int h1 = sy + 1 < e->param.height ? sy + 1 : sy;
            int w1 = sx + 1 < e->param.width ? sx + 1 : sx;
            if (sy >= e->param.height)
                sy = h1 = e->param.height - 1;
            if (sx >= e->param.width)
                sx = w1 = e->param.width - 1;
            const pixel *s = f->src[0];
            int ps = e->pad_w;
            /* the mean, brought back to 8-bit magnitudes: the shift is
             * 2 at 8 bits, which is the plain 2x2 mean it has always
             * been (la.h says why the proxy stays narrow) */
            f->lowres[y * lw + x] = (pixel)((s[sy * ps + sx] +
                s[sy * ps + w1] + s[h1 * ps + sx] + s[h1 * ps + w1] +
                (2 << BIT_DEPTH_SHIFT)) >> (2 + BIT_DEPTH_SHIFT));
        }

    /* per-8x8 costs */
    f->icost_sum = f->pcost_sum = f->mvabs_sum = 0;
    f->zsad_sum = 0;
    for (int by = 0; by < la->bh; by++)
        for (int bx = 0; bx < la->bw; bx++) {
            int b = by * la->bw + bx;
            int x0 = bx * 8, y0 = by * 8;
            if (x0 + 8 > lw)
                x0 = lw - 8;
            if (y0 + 8 > lh)
                y0 = lh - 8;
            const pixel *blk = f->lowres + y0 * lw + x0;
            /* intra estimate: best of V/H/DC prediction from decoded-side
             * neighbors (the AC-energy proxy undercosts structure and
             * fires false scenecuts on grain). Falls back to AC energy
             * in the top-left corner. */
            uint32_t ic;
            {
                uint32_t sv = UINT32_MAX, sh = UINT32_MAX, sd;
                int have_t = y0 > 0, have_l = x0 > 0;
                if (have_t) {
                    sv = 0;
                    for (int y = 0; y < 8; y++)
                        for (int x = 0; x < 8; x++) {
                            int d = blk[y * lw + x] - blk[-lw + x];
                            sv += (uint32_t)(d < 0 ? -d : d);
                        }
                }
                if (have_l) {
                    sh = 0;
                    for (int y = 0; y < 8; y++)
                        for (int x = 0; x < 8; x++) {
                            int d = blk[y * lw + x] - blk[y * lw - 1];
                            sh += (uint32_t)(d < 0 ? -d : d);
                        }
                }
                int dcs = 0, dcn = 0;
                if (have_t) {
                    for (int x = 0; x < 8; x++)
                        dcs += blk[-lw + x];
                    dcn += 8;
                }
                if (have_l) {
                    for (int y = 0; y < 8; y++)
                        dcs += blk[y * lw - 1];
                    dcn += 8;
                }
                if (!dcn) {     /* corner: block mean */
                    for (int y = 0; y < 8; y++)
                        for (int x = 0; x < 8; x++)
                            dcs += blk[y * lw + x];
                    dcn = 64;
                }
                int dc = (dcs + dcn / 2) / dcn;
                sd = 0;
                for (int y = 0; y < 8; y++)
                    for (int x = 0; x < 8; x++) {
                        int d = blk[y * lw + x] - dc;
                        sd += (uint32_t)(d < 0 ? -d : d);
                    }
                ic = sd;
                if (sv < ic)
                    ic = sv;
                if (sh < ic)
                    ic = sh;
                ic += 64;       /* mode/signal floor */
            }
            f->icost[b] = (uint16_t)(ic > 65535 ? 65535 : ic);

            if (!prev) {
                f->pcost[b] = f->icost[b];
                f->mv[2 * b] = f->mv[2 * b + 1] = 0;
            } else {
                /* small diamond at lowres, start at zero */
                int mx = 0, my = 0;
                uint32_t best = sad8(blk, lw,
                                     prev->lowres + y0 * lw + x0, lw);
                /* the search's own starting cost IS the zero-vector
                 * distance the duplicate chain is built on: taken here,
                 * before the diamond moves off it, for one add a block */
                f->zsad_sum += best;
                for (int it = 0; it < 8; it++) {
                    int bmx = mx, bmy = my;
                    uint32_t bs = best;
                    static const int d[4][2] = {
                        { 1, 0 }, { -1, 0 }, { 0, 1 }, { 0, -1 }
                    };
                    for (int k = 0; k < 4; k++) {
                        int tx = mx + d[k][0], ty = my + d[k][1];
                        if (x0 + tx < 0 || y0 + ty < 0 ||
                            x0 + tx + 8 > lw || y0 + ty + 8 > lh)
                            continue;
                        uint32_t s = sad8(blk, lw,
                            prev->lowres + (y0 + ty) * lw + x0 + tx, lw);
                        if (s < bs) {
                            bs = s;
                            bmx = tx;
                            bmy = ty;
                        }
                    }
                    if (bmx == mx && bmy == my)
                        break;
                    mx = bmx;
                    my = bmy;
                    best = bs;
                }
                uint32_t pc = best + 16;
                f->pcost[b] = (uint16_t)(pc > 65535 ? 65535 : pc);
                f->mv[2 * b] = (int16_t)mx;
                f->mv[2 * b + 1] = (int16_t)my;
            }
            f->icost_sum += f->icost[b];
            f->pcost_sum += f->pcost[b];
            f->mvabs_sum += abs(f->mv[2*b]) + abs(f->mv[2*b+1]);
        }
    f->sc_ratio = (float)f->pcost_sum /
                  (float)(f->icost_sum ? f->icost_sum : 1);
    f->sc_ema = la->sc_ema;
    int nblk = la->bw * la->bh;
    int held = sc_hold() && f->pcost_sum < (uint64_t)nblk * 17;
    if (!held)
        la->sc_ema = la->sc_ema == 0.0f
                         ? f->sc_ratio
                         : 0.5f * la->sc_ema + 0.5f * f->sc_ratio;
    /* The duplicate chain (la.h). Only under the knob, and the source
     * compare only where the lowres distance is already exactly zero -
     * so a clip with no duplicate frames pays one branch per frame and a
     * clip with them pays one memcmp per duplicate, against a whole
     * picture's analysis. The running totals live on the queue so a
     * queue that drained to empty does not restart them; a frame with no
     * predecessor in the queue simply opens a new run, which is the
     * conservative answer. */
    f->dup_grp = 0;
    /* A frame with no predecessor IN THE QUEUE was not measured against
     * the one before it, so the running total may not be carried across
     * the gap: the bound it licenses would read zero - "identical" - for
     * a pair nothing compared. DUP_BREAK is a step larger than any bar
     * `N265_DUP_THR` can express (255 levels over a 4K lowres plane is
     * under 2^40), so every bound ACROSS the gap fails and every bound
     * on one side of it is the offset-free one it was. */
    f->dup_cum = la->dup_cum_last + (prev ? f->zsad_sum : DUP_BREAK);
    if (n265_dup_on()) {
        if (prev && f->zsad_sum == 0 && prev->dup_grp &&
            src_identical(e, f, prev))
            f->dup_grp = prev->dup_grp;
        else
            f->dup_grp = ++la->dup_grp_next;
    }
    la->dup_cum_last = f->dup_cum;
    la->count++;
}

int n265_la_scenecut(struct n265_encoder *e, int i)
{
    n265_la *la = &e->la;
    if (i >= la->count || la->f[i].display_idx == 0)
        return 0;
    const la_frame *f = &la->f[i];
    /* cut when inter fails to undercut intra AND the ratio spikes over
     * its running level - a steady high ratio (grain, foliage) is not a
     * scene change */
    int cut = f->sc_ratio >= 0.75f &&
              (f->sc_ema == 0.0f || f->sc_ratio >= 1.3f * f->sc_ema);
    if (n265_env("N265_SC_TRACE"))
        fprintf(stderr, "sc d=%d p=%llu i=%llu ratio=%.2f cut=%d\n",
                f->display_idx, (unsigned long long)f->pcost_sum,
                (unsigned long long)f->icost_sum,
                (double)f->pcost_sum / (double)(f->icost_sum ? f->icost_sum : 1),
                cut);
    return cut;
}

void n265_la_set_anchor(struct n265_encoder *e, int i)
{
    n265_la *la = &e->la;
    if (i < 0 || i >= la->count || !la->anchor_lowres)
        return;
    memcpy(la->anchor_lowres, la->f[i].lowres,
           (size_t)la->lw * la->lh * sizeof(pixel));
    la->anchor_valid = 1;
}

static uint32_t blk_sad_at(const n265_la *la, const pixel *cur,
                           const pixel *ref, int x0, int y0, int mx, int my)
{
    int tx = x0 + mx, ty = y0 + my;
    if (tx < 0 || ty < 0 || tx + 8 > la->lw || ty + 8 > la->lh)
        return UINT32_MAX;
    return sad8(cur + y0 * la->lw + x0, la->lw,
                ref + ty * la->lw + tx, la->lw);
}

/* Cost of predicting lowres plane `cur` from `ref` over the queued frame
 * span [lo, hi]: each block starts at `sign` x the co-located sum of the
 * per-frame lowres MVs across that span (the linear-motion assumption the
 * cu-tree propagation already makes) and is refined by the same small
 * diamond n265_la_push uses. The +16 per-block floor matches pcost, so
 * these numbers are comparable with each other and with pcost_sum.
 *
 * `sadout`, when given, takes the winning SAD PER BLOCK without that
 * floor: the bits model of `--b-adapt 4` is a per-block non-linearity and
 * cannot be applied to a sum. */
static uint64_t pair_cost(const n265_la *la, const pixel *cur,
                          const pixel *ref, int lo, int hi, int sign,
                          int16_t *mvout, uint32_t *sadout)
{
    uint64_t sum = 0;
    for (int by = 0; by < la->bh; by++)
        for (int bx = 0; bx < la->bw; bx++) {
            int b = by * la->bw + bx;
            int x0 = bx * 8, y0 = by * 8;
            if (x0 + 8 > la->lw)
                x0 = la->lw - 8;
            if (y0 + 8 > la->lh)
                y0 = la->lh - 8;
            int sx = 0, sy = 0;
            for (int t = lo; t <= hi; t++) {
                sx += la->f[t].mv[2 * b];
                sy += la->f[t].mv[2 * b + 1];
            }
            int mx = sign * sx, my = sign * sy;
            uint32_t best = blk_sad_at(la, cur, ref, x0, y0, mx, my);
            if (best == UINT32_MAX) {
                mx = my = 0;
                best = blk_sad_at(la, cur, ref, x0, y0, 0, 0);
            } else if (mx || my) {
                uint32_t z = blk_sad_at(la, cur, ref, x0, y0, 0, 0);
                if (z < best) {
                    best = z;
                    mx = my = 0;
                }
            }
            for (int it = 0; it < 8; it++) {
                int bmx = mx, bmy = my;
                uint32_t bs = best;
                static const int d[4][2] = {
                    { 1, 0 }, { -1, 0 }, { 0, 1 }, { 0, -1 }
                };
                for (int k = 0; k < 4; k++) {
                    uint32_t s = blk_sad_at(la, cur, ref, x0, y0,
                                            mx + d[k][0], my + d[k][1]);
                    if (s < bs) {
                        bs = s;
                        bmx = mx + d[k][0];
                        bmy = my + d[k][1];
                    }
                }
                if (bmx == mx && bmy == my)
                    break;
                mx = bmx;
                my = bmy;
                best = bs;
            }
            if (mvout) {
                mvout[2 * b] = (int16_t)mx;
                mvout[2 * b + 1] = (int16_t)my;
            }
            if (sadout)
                sadout[b] = best;
            sum += (uint64_t)best + 16;
        }
    return sum;
}

/* Cost of the same plane predicted BIDIRECTIONALLY, at the two searches'
 * own winning vectors: the leaves of a mini-GOP are coded that way, and a
 * unidirectional cost is the wrong price for them. No search of its own -
 * one pass over the blocks averaging the two predictions. */
static uint64_t bi_cost(const n265_la *la, const pixel *cur,
                        const pixel *ra, const int16_t *mva,
                        const pixel *rb, const int16_t *mvb,
                        uint32_t *sadout)
{
    uint64_t sum = 0;
    for (int by = 0; by < la->bh; by++)
        for (int bx = 0; bx < la->bw; bx++) {
            int b = by * la->bw + bx;
            int x0 = bx * 8, y0 = by * 8;
            if (x0 + 8 > la->lw)
                x0 = la->lw - 8;
            if (y0 + 8 > la->lh)
                y0 = la->lh - 8;
            int ax = x0 + mva[2 * b], ay = y0 + mva[2 * b + 1];
            int bx2 = x0 + mvb[2 * b], by2 = y0 + mvb[2 * b + 1];
            if (ax < 0 || ay < 0 || ax + 8 > la->lw || ay + 8 > la->lh ||
                bx2 < 0 || by2 < 0 || bx2 + 8 > la->lw || by2 + 8 > la->lh)
                return UINT64_MAX;      /* out of range: no bi price */
            const pixel *c = cur + y0 * la->lw + x0;
            const pixel *pa = ra + ay * la->lw + ax;
            const pixel *pb = rb + by2 * la->lw + bx2;
            uint32_t s = 0;
            for (int y = 0; y < 8; y++)
                for (int x = 0; x < 8; x++) {
                    int p = (pa[y * la->lw + x] + pb[y * la->lw + x] + 1) >> 1;
                    int d = c[y * la->lw + x] - p;
                    s += (uint32_t)(d < 0 ? -d : d);
                }
            if (sadout)
                sadout[b] = s;
            sum += (uint64_t)s + 16;
        }
    return sum;
}

/* How far the run can go before the anchor's own prediction falls apart,
 * measured rather than guessed: extend while the lowres cost of predicting
 * the candidate anchor from the LAST CODED ANCHOR stays within `thr` times
 * that frame's own distance-1 cost. The quantity is a ratio of two costs of
 * the same frame, so it carries no units and does not scale with resolution
 * the way the mean |MV| rule does. */
int n265_la_degrade(struct n265_encoder *e, int maxa, double thr)
{
    n265_la *la = &e->la;
    if (maxa < 1)
        return 0;
    if (!la->anchor_valid || maxa >= la->count)
        return maxa;
    int trace = n265_env("N265_BADAPT_TRACE") != NULL;
    int A = 0;
    for (int i = 1; i <= maxa; i++) {
        A = i;
        uint64_t far = pair_cost(la, la->f[i].lowres, la->anchor_lowres,
                                 0, i, 1, NULL, NULL);
        uint64_t near = la->f[i].pcost_sum ? la->f[i].pcost_sum : 1;
        double r = (double)far / (double)near;
        if (trace)
            fprintf(stderr, "BDEG d=%d i=%d far=%llu near=%llu r=%.3f "
                    "thr=%.3f %s\n", la->f[i].display_idx, i,
                    (unsigned long long)far, (unsigned long long)near, r, thr,
                    r > thr ? "END" : "extend");
        if (r > thr)
            break;
    }
    return A;
}

int n265_la_brun(struct n265_encoder *e, int maxa)
{
    n265_la *la = &e->la;
    if (maxa < 1)
        return 0;
    if (!la->anchor_valid || maxa >= la->count || maxa >= la->pc_n)
        return maxa;
    uint64_t fwd[N265_MAX_BFRAMES + 1];
    for (int k = 0; k <= maxa; k++)
        fwd[k] = pair_cost(la, la->f[k].lowres, la->anchor_lowres, 0, k, 1,
                           la->pc_mv[k], NULL);
    int trace = n265_env("N265_BADAPT_TRACE") != NULL;
    int best_a = 1;
    double best_c = 0;
    for (int A = 1; A <= maxa; A++) {
        uint64_t c = fwd[A];
        for (int k = 0; k < A; k++) {
            uint64_t bwd = pair_cost(la, la->f[k].lowres, la->f[A].lowres,
                                     k + 1, A, -1, la->pc_bmv, NULL);
            uint64_t bi = bi_cost(la, la->f[k].lowres, la->anchor_lowres,
                                  la->pc_mv[k], la->f[A].lowres, la->pc_bmv,
                                  NULL);
            uint64_t leaf = fwd[k] < bwd ? fwd[k] : bwd;
            if (bi < leaf)
                leaf = bi;
            c += leaf;
            if (trace)
                fprintf(stderr, "  BRUNleaf A=%d k=%d fwd=%llu bwd=%llu "
                        "bi=%llu\n", A, k, (unsigned long long)fwd[k],
                        (unsigned long long)bwd, (unsigned long long)bi);
        }
        double per = (double)c / (double)(A + 1);
        /* ties go to the longer run: same predicted cost over more frames
         * means fewer anchors for it */
        if (A == 1 || per <= best_c) {
            best_c = per;
            best_a = A;
        }
        if (trace)
            fprintf(stderr, "BRUN d=%d A=%d anchor=%llu per=%.1f\n",
                    la->f[0].display_idx, A,
                    (unsigned long long)fwd[A], per);
    }
    if (trace)
        fprintf(stderr, "BRUN d=%d pick A=%d\n", la->f[0].display_idx, best_a);
    return best_a;
}

/* --- the bits model (`--b-adapt 4`, docs/phase7-gop-design.md D1) --- */

/* log2 in Q16, table-driven off the top eight mantissa bits. Integer end
 * to end on purpose: the sum below runs over every lowres block of every
 * candidate run of every mini-GOP, and an integer sum is both faster than
 * libm and reproducible without depending on a libm's rounding. The
 * truncation costs at most 2^-9 of a bit per block. */
static int32_t g_log2q16[257];

static void log2q16_init(void)
{
    if (g_log2q16[256])
        return;                     /* log2(2) x 65536, never zero */
    for (int i = 0; i <= 256; i++)
        g_log2q16[i] = (int32_t)lround(log2(1.0 + i / 256.0) * 65536.0);
}

static int32_t log2_q16(uint32_t x)
{
    int e = 31 - __builtin_clz(x);              /* x >= 1 */
    uint32_t m = e >= 8 ? (x >> (e - 8)) : (x << (8 - e));   /* 256..511 */
    return (int32_t)(e << 16) + g_log2q16[m - 256];
}

/* The dead zone the model prices against, as log2 of a lowres 8x8 SAD.
 *
 * ctu.c admits a CLEAN SKIP - no residual coded at all - when a CU's
 * distortion is under qstep^2/4 per pixel, scaled by N265_ES_THR percent
 * (`skip_clean_thr`, read from the same env with the same default rather
 * than reached into). A block the encoder would skip outright is a block
 * this model must price at zero, so that admission IS the threshold; it
 * only has to be carried into the lookahead's units:
 *
 *   per-pixel MSE bound     m = qstep^2 / 4 x pct/100
 *   per-pixel |residual|    r = sqrt(m) = qstep/2 x sqrt(pct/100)
 *   a lowres 8x8 block is 64 samples, each the 2x2 MEAN of the full-res
 *   picture, and the residual a dead zone rules on is smooth at the
 *   scale of a 2x2, so the mean carries it through undiminished:
 *   T(qp) = 64 r = 32 x qstep x sqrt(pct/100).
 *
 * qstep = 2^((qp - 4)/6), ctu.c's own. Taking the 2x2 mean as lossless is
 * the optimistic end of the conversion: a white residual would shrink by
 * two and halve T. That factor is NOT free - it does not cancel, because
 * the max(0, .) is the whole term - and it is the one number in the model
 * that is asserted rather than measured. It is asserted from the
 * encoder's own admission at the frame's own QP, which is the most the
 * lookahead can know before the frame is coded. */
/* The `qp` here is the 8-BIT-DOMAIN QpY and must stay that way at
 * Main10: the SADs this threshold is compared against are lowres ones,
 * and the lowres plane carries 8-bit values at every depth (la.c's 2x2
 * mean sheds BIT_DEPTH - 8 on the way in). Indexing qstep by Qp' here -
 * which is right everywhere the quantity is a native sample - would make
 * the dead zone four times too wide and collapse every --b-adapt 4 run. */
static int32_t brun_logT_q16(int qp)
{
    static int pct = -1;
    if (pct < 0) { const char *v = n265_env("N265_ES_THR"); pct = v ? atoi(v) : 100; }
    double qstep = pow(2.0, (qp - 4) / 6.0);
    double T = 32.0 * qstep * sqrt((double)pct / 100.0);
    if (T < 1.0)
        T = 1.0;
    return (int32_t)lround(log2(T) * 65536.0);
}

/* Frame bits in Q16: sum over blocks of max(0, log2(sad / T)). A block
 * under the dead zone is free, which is the term `--b-adapt 2` did not
 * have; above it the price grows with the log of the residual, which is
 * what a quantiser followed by an entropy coder does. A per-block syntax
 * floor is deliberately absent: the block count is the same for every
 * candidate and every frame, so a constant per block adds the same
 * amount to every candidate's amortised price and cannot move the pick. */
static uint64_t blk_bits(const uint32_t *sad, int nb, int32_t logT)
{
    uint64_t sum = 0;
    for (int b = 0; b < nb; b++) {
        uint32_t s = sad[b];
        if (!s)
            continue;
        int32_t v = log2_q16(s) - logT;
        if (v > 0)
            sum += (uint64_t)v;
    }
    return sum;
}

/* The depth plan_brange (encoder.c) gives each leaf of a run of A, and
 * whether it is referenced: the same midpoint recursion, mirrored here
 * because the planner builds its arrays on the encoder and this runs
 * before the plan exists. The leaf's depth is what sets its QP, and its
 * QP is what sets its dead zone - the asymmetry the whole model is for. */
static void brun_depths(int blo, int bhi, int depth, int *d, int *nref)
{
    if (blo > bhi)
        return;
    int mid = (blo + bhi) / 2;
    d[mid] = depth;
    nref[mid] = !((blo <= mid - 1) || (mid + 1 <= bhi));
    brun_depths(blo, mid - 1, depth + 1, d, nref);
    brun_depths(mid + 1, bhi, depth + 1, d, nref);
}

/* `--b-adapt 4`: the b-adapt 2 trellis with the per-block cost replaced
 * by the bits proxy above, each frame priced at the QP it will actually
 * be coded at (n265_planned_qp, encoder.c). The anchor sits at distance
 * A + 1 and pays P's cascade; every leaf pays its own depth's, which is
 * two to four QP higher, so its dead zone is wider and more of its blocks
 * are free. That is the term that lets a long run win on static content
 * where a residual sum always prefers the shortest prediction distance.
 * Amortised over A + 1 frames and minimised over A, ties to the longer
 * run, exactly as `--b-adapt 2`. N265_BRUN_TRACE prints the per-frame
 * prediction for every candidate. */
int n265_la_brun_bits(struct n265_encoder *e, int maxa)
{
    n265_la *la = &e->la;
    if (maxa < 1)
        return 0;
    if (!la->anchor_valid || maxa >= la->count || maxa >= la->pc_n)
        return maxa;
    int nb = la->bw * la->bh;
    log2q16_init();
    for (int k = 0; k <= maxa; k++)
        pair_cost(la, la->f[k].lowres, la->anchor_lowres, 0, k, 1,
                  la->pc_mv[k], la->pc_sad[k]);
    int trace = n265_brun_trace();
    int qp_p = n265_planned_qp(e, 1, 0, 0);
    int32_t lt_p = brun_logT_q16(qp_p);
    int best_a = 1;
    double best_c = 0;
    for (int A = 1; A <= maxa; A++) {
        int dep[N265_MAX_BFRAMES + 1], nref[N265_MAX_BFRAMES + 1];
        brun_depths(0, A - 1, 1, dep, nref);
        uint64_t anchor = blk_bits(la->pc_sad[A], nb, lt_p);
        uint64_t c = anchor;
        if (trace)
            fprintf(stderr, "BRUNB d=%d A=%d k=%d disp=%d type=1 qp=%d "
                    "bits=%.2f\n", la->f[0].display_idx, A, A,
                    la->f[A].display_idx, qp_p, anchor / 65536.0);
        for (int k = 0; k < A; k++) {
            int qp_b = n265_planned_qp(e, 0, dep[k], nref[k]);
            int32_t lt_b = brun_logT_q16(qp_b);
            pair_cost(la, la->f[k].lowres, la->f[A].lowres, k + 1, A, -1,
                      la->pc_bmv, la->pc_bsad);
            uint64_t bi = bi_cost(la, la->f[k].lowres, la->anchor_lowres,
                                  la->pc_mv[k], la->f[A].lowres, la->pc_bmv,
                                  la->pc_bisad);
            uint64_t bf = blk_bits(la->pc_sad[k], nb, lt_b);
            uint64_t bb = blk_bits(la->pc_bsad, nb, lt_b);
            uint64_t leaf = bf < bb ? bf : bb;
            if (bi != UINT64_MAX) {
                uint64_t bbi = blk_bits(la->pc_bisad, nb, lt_b);
                if (bbi < leaf)
                    leaf = bbi;
            }
            c += leaf;
            if (trace)
                fprintf(stderr, "BRUNB d=%d A=%d k=%d disp=%d type=0 qp=%d "
                        "dep=%d nref=%d fwd=%.2f bwd=%.2f bi=%.2f "
                        "bits=%.2f\n", la->f[0].display_idx, A, k,
                        la->f[k].display_idx, qp_b, dep[k], nref[k],
                        bf / 65536.0, bb / 65536.0,
                        bi == UINT64_MAX ? -1.0
                                         : blk_bits(la->pc_bisad, nb, lt_b) / 65536.0,
                        leaf / 65536.0);
        }
        double per = (double)c / 65536.0 / (double)(A + 1);
        if (A == 1 || per <= best_c) {
            best_c = per;
            best_a = A;
        }
        if (trace)
            fprintf(stderr, "BRUNBA d=%d A=%d total=%.2f per=%.2f\n",
                    la->f[0].display_idx, A, c / 65536.0, per);
    }
    if (trace)
        fprintf(stderr, "BRUNB d=%d pick A=%d\n", la->f[0].display_idx, best_a);
    return best_a;
}

void n265_la_cutree(struct n265_encoder *e, n265_frame_work *fw, int i)
{
    n265_la *la = &e->la;
    int nb = la->bw * la->bh;
    /* the WINDOW, not the queue: `--shot-window` holds more frames than
     * `--rc-lookahead` asked for and the propagation horizon must not
     * move with them (la.h) */
    int nview = n265_la_view(la);
    for (int k = 0; k < nview; k++)
        memset(la->f[k].prop, 0, (size_t)nb * sizeof(float));

    /* propagate along the real reference edges in reverse encode order:
     * anchors (even GOP parity) push into the previous anchor (mv x2,
     * linear-motion assumption), B frames push half into each of their
     * two anchors (-mv for the future one). Non-reference B frames never
     * RECEIVE propagation. */
    int order[LA_MAX];
    int m = 0;
    for (int k = 0; k < nview; k++) {
        int rel = la->f[k].display_idx - e->gop_start_display;
        if ((rel & 1) == 0 || !e->bframes) {
            order[m++] = k;
            if (e->bframes && k - 1 >= 0 &&
                ((la->f[k - 1].display_idx - e->gop_start_display) & 1))
                order[m++] = k - 1;
        }
    }
    for (int oi = m - 1; oi >= 0; oi--) {
        int k = order[oi];
        if (k <= i)
            continue;
        la_frame *f = &la->f[k];
        int rel = f->display_idx - e->gop_start_display;
        int is_b = e->bframes && (rel & 1);
        struct prop_tgt { int idx; float w; int neg; int scale; } tgt[2];
        int nt = 0;
        if (is_b) {
            if (k - 1 >= 0) {
                struct prop_tgt t = { k - 1, 0.5f, 0, 1 };
                tgt[nt++] = t;
            }
            if (k + 1 < nview) {
                struct prop_tgt t = { k + 1, 0.5f, 1, 1 };
                tgt[nt++] = t;
            }
        } else {
            int pk = k - (e->bframes ? 2 : 1);
            if (pk >= 0) {
                struct prop_tgt t = { pk, 1.0f, 0, e->bframes ? 2 : 1 };
                tgt[nt++] = t;
            }
        }
        if (!nt)
            continue;
        for (int b = 0; b < nb; b++) {
            float ic = f->icost[b], pc = f->pcost[b];
            if (pc >= ic)
                continue;
            float amount = (ic + f->prop[b]) * (1.0f - pc / ic);
            int bx = b % la->bw, by = b / la->bw;
            for (int t = 0; t < nt; t++) {
                la_frame *ref = &la->f[tgt[t].idx];
                int mvx = f->mv[2 * b] * tgt[t].scale;
                int mvy = f->mv[2 * b + 1] * tgt[t].scale;
                if (tgt[t].neg) {
                    mvx = -mvx;
                    mvy = -mvy;
                }
                int px = bx * 8 + mvx, py = by * 8 + mvy;
                int gx = px >> 3, gy = py >> 3;
                int fx = px & 7, fy = py & 7;
                float wx1 = fx / 8.0f, wy1 = fy / 8.0f;
                float aw = amount * tgt[t].w;
                for (int dy = 0; dy < 2; dy++)
                    for (int dx = 0; dx < 2; dx++) {
                        int tx = gx + dx, ty = gy + dy;
                        if (tx < 0 || ty < 0 || tx >= la->bw ||
                            ty >= la->bh)
                            continue;
                        float w = (dx ? wx1 : 1 - wx1) *
                                  (dy ? wy1 : 1 - wy1);
                        ref->prop[ty * la->bw + tx] += aw * w;
                    }
            }
        }
    }

    /* offsets for frame i: -strength * log2((intra + prop) / intra),
     * averaged per CTU and ZERO-MEANED across the frame (without rate
     * control an un-centred map is just an operating-point shift plus
     * delta bits), then clamped */
    la_frame *f = &la->f[i];
    int ctb = 1 << e->ctb_log2;
    int blocks_per_ctb = ctb / 16;      /* lowres 8 = full 16 */
    int nctu = e->ctb_cols * e->ctb_rows;
    float raw[LA_MAX_CTUS];
    float mean = 0;
    for (int cy = 0; cy < e->ctb_rows; cy++)
        for (int cx = 0; cx < e->ctb_cols; cx++) {
            float acc = 0;
            int cnt = 0;
            for (int dy = 0; dy < blocks_per_ctb; dy++)
                for (int dx = 0; dx < blocks_per_ctb; dx++) {
                    int bx = cx * blocks_per_ctb + dx;
                    int by = cy * blocks_per_ctb + dy;
                    if (bx >= la->bw || by >= la->bh)
                        continue;
                    int b = by * la->bw + bx;
                    float ic = f->icost[b];
                    acc += log2f((ic + f->prop[b]) / ic);
                    cnt++;
                }
            float v = cnt ? -e->cutree_str * acc / cnt : 0;
            raw[cy * e->ctb_cols + cx] = v;
            mean += v;
        }
    mean /= nctu;
    float field_mean = mean;    /* the frame term reads this, before centring eats it */
    /* CRF leaves the field un-centred so the cross-frame differential
     * survives, and a fixed shift in the base nets "the" mean. There is no
     * one mean: a NON-REFERENCED frame receives no propagation at all, so
     * its field is identically zero, where a referenced frame's mean is
     * strongly negative and content-keyed. The difference lands on top of
     * the pyramid QP cascade as a second, uncalibrated cascade.
     * N265_CT_CENTRE 1 centres every frame's field on its own mean under
     * CRF as well (the CQP form), leaving the cascade the only frame-type
     * term; that removes the step and the cross-frame differential with it.
     * 2 removes only the step: it nets each PROPAGATED-INTO frame against
     * the mean over the frames in the lookahead window that receive
     * propagation at all, so what survives is the frame's deviation from
     * the window's own level, and a frame with no propagation keeps its
     * zero. 0 is the pre-2026-09-08 behaviour. */
    if (e->crf_on) {
        static int centre = -1;
        if (centre < 0) {
            const char *v = n265_env("N265_CT_CENTRE");
            centre = v ? atoi(v) : 0;
        }
        if (!centre) {
            mean = 0;       /* un-centred: the shift in the base nets the mean */
        } else if (centre >= 2) {
            if (mean == 0.0f) {
                /* no propagation into this frame: it IS the operating point */
            } else {
                double sum = 0;
                int n = 0;
                for (int k = 0; k < nview; k++) {
                    const la_frame *g = &la->f[k];
                    double acc = 0;
                    int cnt = 0;
                    for (int b = 0; b < nb; b++) {
                        if (g->prop[b] == 0.0f)
                            continue;
                        float ic = g->icost[b];
                        acc += log2f((ic + g->prop[b]) / ic);
                        cnt++;
                    }
                    if (!cnt)
                        continue;   /* leaf frame: not part of the level */
                    sum += -(double)e->cutree_str * acc / nb;
                    n++;
                }
                mean = n ? (float)(sum / n) : mean;
            }
        }
    }
    /* The window LEVEL: the mean over the frames in the lookahead window
     * that receive propagation at all - the same quantity N265_CT_CENTRE=2
     * nets against, computed here for the frame term instead.
     *
     * It exists because the field's frame mean carries two things at once.
     * The deviation from this level is the frame-TYPE step (a leaf receives
     * nothing, so its mean is zero and its deviation is the whole level),
     * and that is what the fixed I/P/B cascade double-counts. The level
     * itself is the SHOT's own complexity, which nothing else in the
     * encoder supplies: scaling it down moves bits between shots, and the
     * multi-shot gate refuses that (docs/phase7-modulation-design.md
     * "Step 2 result"). k_f therefore re-weights the deviation and leaves
     * the level alone. N265_CT_FRAME_LEVEL=0 is the whole-mean form, which
     * is the arm that gate refused. */
    double level = 0.0;
    if ((e->crf_on || e->abr_on) && e->ct_framew != e->ct_qpw) {
        static int use_level = -1;
        if (use_level < 0) {
            const char *v = n265_env("N265_CT_FRAME_LEVEL");
            use_level = v ? atoi(v) : 1;
        }
        if (use_level) {
            /* N265_CT_FRAME_SHOT: take the level over this frame's side of
             * the nearest scenecut in the window only. A window that
             * straddles a cut otherwise averages two shots' levels into one
             * term, so the frame is placed against content it does not
             * reference - the propagation walk already stops at the cut. */
            int lo = 0, hi = nview;
            if (e->ct_frame_shot) {
                for (int k = i; k > 0; k--)
                    if (n265_la_scenecut(e, k)) { lo = k; break; }
                for (int k = i + 1; k < nview; k++)
                    if (n265_la_scenecut(e, k)) { hi = k; break; }
            }
            double sum = 0, all = 0;
            int n = 0, nall = 0;
            for (int k = lo; k < hi; k++) {
                const la_frame *g = &la->f[k];
                double acc = 0;
                int cnt = 0;
                for (int b = 0; b < nb; b++) {
                    if (g->prop[b] == 0.0f)
                        continue;
                    float ic = g->icost[b];
                    acc += log2f((ic + g->prop[b]) / ic);
                    cnt++;
                }
                all += -(double)e->cutree_str * acc / nb;   /* a leaf adds 0 */
                nall++;
                if (!cnt)
                    continue;   /* leaf frame: not part of the level */
                sum += -(double)e->cutree_str * acc / nb;
                n++;
            }
            /* 2 (phase 7 step 3) takes the level over EVERY frame in the
             * window, a leaf's zero included, instead of over the
             * propagated-into ones. That makes the frame term zero-mean
             * across the window by construction: it redistributes QP between
             * the frames of a mini-GOP and moves not one bit between windows,
             * and so not one between shots. 1 is step 2's form, whose
             * window-average term is content-keyed and is what the
             * multi-shot gate charged for. */
            level = use_level >= 2 ? (nall ? all / nall : 0.0)
                                   : (n ? sum / n : 0.0);
        }
    }
    fw->ct_level = level;
    /* N265_CT_QPW is the share of the field that reaches the QP. It is on
     * cutree_str's scale, so the default 2.0 is a scale of 1 and the
     * offset map is byte-for-byte what it was. (This rounding is the
     * AQ-off path; with AQ on, encoder.c scales the raw field the same
     * way and rounds it once with the variance term.) */
    double qpw = n265_ct_qp_scale(e);
    /* The frame term's two inputs (phase 7 step 2). Under CRF the field is
     * left un-centred, so the per-CTU offsets below already carry k_b of the
     * field's own frame mean - a second frame-type cascade on top of the
     * fixed I/P/B steps - and one strength was scaling the cross-frame
     * allocation and the within-frame spread together. Handing the two means
     * to encoder.c lets it place the cross-frame half on purpose (k_f), where
     * the frame's content type is known; it forms the term as a DIFFERENCE
     * against what the map still carries, which is what makes k_f == k_b
     * exactly zero - same expression, same double - so the default bitstream
     * is main's byte for byte, rounding and the offset map's own +-12 clamp
     * included. */
    fw->ct_mean_raw = field_mean;
    fw->ct_mean_used = mean;
    for (int k = 0; k < nctu; k++) {
        float v = raw[k] - mean;
        fw->ctu_qp_raw[k] = v;
        double qv = qpw * v;
        int off = (int)(qv > 0 ? qv + 0.5 : qv - 0.5);
        fw->ctu_qp_off[k] = (int8_t)(off < -12 ? -12 : off > 12 ? 12 : off);
    }
    /* N265_CT_LAMBDA reads the SAME field as a per-CTU lambda multiplier,
     * at strength 1 and centred on the frame so it is independent of what
     * the QP path above did with it. dev < 0 is a CTU the field wants at a
     * lower QP; it gets the lambda that QP step would have carried, on the
     * same 2^(d/3) slope, bounded by the offset map's own +-12. Frozen
     * here, before the frame is scheduled, and read-only after it is
     * (docs/threading-design.md rule 4). */
    if (e->ct_lambda != 0.0f && e->cutree_str != 0.0f) {
        double fm = 0;
        for (int k = 0; k < nctu; k++)
            fm += fw->ctu_qp_raw[k];
        fm /= nctu;
        for (int k = 0; k < nctu; k++) {
            double d = e->ct_lambda *
                ((double)fw->ctu_qp_raw[k] - fm) / (double)e->cutree_str;
            if (d < -12.0) d = -12.0;
            else if (d > 12.0) d = 12.0;
            fw->ctu_lam_q8[k] = (uint16_t)lrint(256.0 * pow(2.0, d / 3.0));
        }
    } else {
        for (int k = 0; k < nctu; k++)
            fw->ctu_lam_q8[k] = 256;
    }
    if (n265_env("N265_CT_TRACE")) {
        int nz = 0, mn = 0;
        int lmn = 65535, lmx = 0;
        double fm = 0;
        for (int k = 0; k < e->ctb_cols * e->ctb_rows; k++) {
            nz += fw->ctu_qp_off[k] != 0;
            fm += fw->ctu_qp_raw[k];
            if (fw->ctu_qp_off[k] < mn)
                mn = fw->ctu_qp_off[k];
            if (fw->ctu_lam_q8[k] < lmn) lmn = fw->ctu_lam_q8[k];
            if (fw->ctu_lam_q8[k] > lmx) lmx = fw->ctu_lam_q8[k];
        }
        fprintf(stderr, "[ct d%d cnt%d nz%d min%d raw%.2f fieldmean%.2f "
                        "lam%d..%d]",
                f->display_idx, la->count, nz, mn, mean, fm / nctu, lmn, lmx);
    }
}

void n265_la_pop(struct n265_encoder *e)
{
    n265_la *la = &e->la;
    la_frame tmp = la->f[0];
    /* Shift the WHOLE ring, live frames and free run alike, so the
     * recycled slot lands at the BACK of the free run rather than under
     * the next push. A frame that was just dispatched is still reading
     * its source planes (T3/1), and reusing them here is what used to
     * force the driver to drain before every la_push. */
    memmove(&la->f[0], &la->f[1], (size_t)(la->cap - 1) * sizeof(la_frame));
    tmp.valid = 0;
    la->f[la->cap - 1] = tmp;
    la->count--;
}

const pixel *n265_la_next_src(struct n265_encoder *e)
{
    return e->la.f[e->la.count].src[0];
}
