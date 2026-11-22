/*
 * Lookahead implementation: 2x2-mean lowres, per-8x8 SATD-lite intra
 * proxy and small-diamond inter cost, scenecut by cost ratio, MB-tree
 * style cu-tree propagation over the queued window feeding the per-CTU QP
 * offset map. Serial and deterministic; the threading ladder moves this
 * onto the pool at T3/L3.
 * Copyright (c) 2026, the yah265 authors. GPL-2.0-or-later; see LICENSE.
 */

#include <limits.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "common/cpu.h"
#include "encoder/encoder.h"
#include "encoder/la.h"
#include "encoder/preset.h"
#include "encoder/satd.h"

/* Share of whole 16x16 source luma blocks with variance under one 8-BIT
 * luma level, in percent. A variance under one level is a standard
 * deviation under a single luma level: a block that is one flat color to
 * within the smallest step the quantizer reproduces. The walk is over the
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

struct flat_rows {
    const pixel *p;
    int stride, bw;
    int *flat;
};

static int flat_count(const struct flat_rows *c, int by)
{
    const pixel *p = c->p;
    int stride = c->stride;
    int flat = 0;
    for (int bx = 0; bx < c->bw; bx++) {
        const pixel *b = p + (by << 4) * stride + (bx << 4);
        uint32_t s1 = 0, s2 = 0;
        int lo = PIXEL_MAX, hi = 0;
        for (int y = 0; y < 16; y++) {
            const pixel *r = b + y * stride;
            uint32_t r1 = 0, r2 = 0;
            pixel rlo = r[0], rhi = r[0];
            N265_VECTORIZE
            for (int x = 0; x < 16; x++) {
                pixel v = r[x];
                r1 += v;
                r2 += (uint32_t)v * v;
                rlo = v < rlo ? v : rlo;
                rhi = v > rhi ? v : rhi;
            }
            s1 += r1;
            s2 += r2;
            if (rlo < lo) lo = rlo;
            if (rhi > hi) hi = rhi;
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
    return flat;
}

static void flat_row(void *ctx, int by)
{
    const struct flat_rows *c = ctx;
    c->flat[by] = flat_count(c, by);
}

int n265_la_flat_pct(const struct n265_frame_work *fw)
{
    int bw = fw->param.width >> 4, bh = fw->param.height >> 4;
    if (bw <= 0 || bh <= 0)
        return 0;
    int *rows = n265_par_width(fw->enc) > 1
              ? malloc((size_t)bh * sizeof(int)) : NULL;
    struct flat_rows c = { fw->pad_plane[0], fw->pad_stride[0], bw, rows };
    int flat = 0;
    if (rows) {
        n265_par_for(fw->enc, PAR_FLAT, bh, flat_row, &c);
        for (int by = 0; by < bh; by++)
            flat += rows[by];
        free(rows);
    } else {
        for (int by = 0; by < bh; by++)
            flat += flat_count(&c, by);
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
     * input overran a 4x4 plane here (the sanitizer matrix, 2026-09-06). */
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
    la->row_sum = malloc((size_t)la->bh * sizeof(*la->row_sum));
    if (!la->f || !la->row_sum)
        return -1;
    la->sc_rn = 0;
    la->sc_last_ic = -1;
    la->jh_n = 0;
    for (int i = 0; i < la->cap; i++) {
        la_frame *f = &la->f[i];
        for (int c = 0; c < 3; c++) {
            int w = c ? e->pad_w >> e->csx : e->pad_w;
            int h = c ? e->pad_h >> e->csy : e->pad_h;
            f->src[c] = n265_malloc_big((size_t)w * h * sizeof(pixel));
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
        f->mvy_lo = malloc((size_t)la->bh * sizeof(int16_t));
        f->mvy_hi = malloc((size_t)la->bh * sizeof(int16_t));
        if (!f->mvy_lo || !f->mvy_hi)
            return -1;
        int nb = la->bw * la->bh;
        f->icost = malloc((size_t)nb * sizeof(uint16_t));
        f->pcost = malloc((size_t)nb * sizeof(uint16_t));
        f->zsad = malloc((size_t)nb * sizeof(uint16_t));
        f->mv = malloc((size_t)nb * 2 * sizeof(int16_t));
        f->prop = malloc((size_t)nb * sizeof(float));
        f->ctfrac = malloc((size_t)nb * sizeof(float));
        if (!f->lowres || !f->icost || !f->pcost || !f->zsad || !f->mv ||
            !f->prop || !f->ctfrac)
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
        free(f->mvy_lo);
        free(f->mvy_hi);
        free(f->icost);
        free(f->pcost);
        free(f->zsad);
        free(f->mv);
        free(f->prop);
        free(f->ctfrac);
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
    free(e->la.row_sum);
    e->la.row_sum = NULL;
}

/* the lookahead's 8x8 block SAD, inline: through n265_sad_wh the call
 * cost more than the loop */
static inline uint32_t sad8(const pixel *a, int as, const pixel *b, int bs)
{
    uint32_t sad = 0;
    for (int y = 0; y < 8; y++, a += as, b += bs)
        for (int x = 0; x < 8; x++) {
            int d = a[x] - b[x];
            sad += (uint32_t)(d < 0 ? -d : d);
        }
    return sad;
}

/* N265_SCREEN (phase 14 item 61): type screen content per frame and give
 * it the scroll vote, the change-driven B run (encoder.c) and the lowres
 * motion seeds. 0 is off and is main's stream byte for byte. */
int n265_screen(void)
{
    static int v = -1;
    if (v < 0) {
        const char *s = n265_env("N265_SCREEN");
        v = s ? atoi(s) : 0;
    }
    return v;
}

/* A screen block is an edge (a range of 64 levels or more) drawn in a
 * handful of levels (16 or fewer) on a background that repeats exactly
 * (180 of its 240 horizontal neighbor pairs equal). Clipped highlights
 * pass the last test and fail the second. A frame is screen content when
 * SCR_SHARE percent of the tested blocks (every other row and column)
 * are. */
#define SCR_RANGE   (64 << BIT_DEPTH_SHIFT)
#define SCR_LEVELS  16
#define SCR_EQ      180
#define SCR_SHARE   3

static int scr_detect(const struct n265_encoder *e, const pixel *p, int stride)
{
    int bw = e->param.width >> 4, bh = e->param.height >> 4;
    int tested = 0, hit = 0;
    for (int by = 0; by < bh; by += 2)
        for (int bx = 0; bx < bw; bx += 2) {
            const pixel *b = p + (by << 4) * stride + (bx << 4);
            int eq = 0, y = 0;
            tested++;
            for (; y < 16; y++) {
                const pixel *r = b + y * stride;
                for (int x = 1; x < 16; x++)
                    eq += r[x] == r[x - 1];
                /* the rows left cannot reach SCR_EQ */
                if (eq + (15 - y) * 15 < SCR_EQ)
                    break;
            }
            if (y < 16)
                continue;
            uint64_t seen[(PIXEL_MAX + 64) / 64] = { 0 };
            int levels = 0;
            pixel lo = b[0], hi = b[0];
            for (y = 0; y < 16; y++)
                for (int x = 0; x < 16; x++) {
                    pixel v = b[y * stride + x];
                    lo = v < lo ? v : lo;
                    hi = v > hi ? v : hi;
                    uint64_t bit = 1ull << (v & 63);
                    levels += !(seen[v >> 6] & bit);
                    seen[v >> 6] |= bit;
                }
            hit += hi - lo >= SCR_RANGE && levels <= SCR_LEVELS;
        }
    return tested && hit * 100 >= tested * SCR_SHARE;
}

/* Vote up to two scroll vectors for a screen frame against the previous
 * one. A scroll is horizontal or vertical and longer than a diamond
 * walks, so each moved, textured block searches its own row and column
 * +-GMV_R lowres pels. */
#define GMV_R 128

struct scr_rows {
    const n265_la *la;
    const la_frame *f, *prev;
    int (*hist)[2][2 * GMV_R + 1];  /* per voting row: horizontal, vertical */
    int *voters;
};

static void scr_vote_row(void *ctx, int r)
{
    const struct scr_rows *c = ctx;
    const n265_la *la = c->la;
    int lw = la->lw, lh = la->lh;
    int *hx = c->hist[r][0], *hy = c->hist[r][1];
    int by = 1 + 2 * r;
    for (int bx = 1; bx + 1 < la->bw; bx += 2) {
        int x0 = bx * 8, y0 = by * 8;
        const pixel *blk = c->f->lowres + y0 * lw + x0;
        const pixel *ref = c->prev->lowres;
        int lo = 255, hi = 0;
        for (int y = 0; y < 8; y++)
            for (int x = 0; x < 8; x++) {
                int v = blk[y * lw + x];
                lo = v < lo ? v : lo;
                hi = v > hi ? v : hi;
            }
        if (hi - lo < 48)
            continue;
        uint32_t z = sad8(blk, lw, ref + y0 * lw + x0, lw);
        if (z < 64)
            continue;
        uint32_t bs = UINT32_MAX;
        int bdx = 0, bdy = 0, ties = 0;
        for (int d = -GMV_R; d <= GMV_R; d++) {
            if (!d)
                continue;
            if (x0 + d >= 0 && x0 + d + 8 <= lw) {
                uint32_t s = sad8(blk, lw, ref + y0 * lw + x0 + d, lw);
                if (s < bs) { bs = s; bdx = d; bdy = 0; ties = 0; }
                else if (s == bs) ties++;
            }
            if (y0 + d >= 0 && y0 + d + 8 <= lh) {
                uint32_t s = sad8(blk, lw, ref + (y0 + d) * lw + x0, lw);
                if (s < bs) { bs = s; bdx = 0; bdy = d; ties = 0; }
                else if (s == bs) ties++;
            }
        }
        c->voters[r]++;
        /* a repeating pattern has no one answer */
        if (ties || bs * 4 > z)
            continue;
        if (bdy == 0)
            hx[bdx + GMV_R]++;
        else
            hy[bdy + GMV_R]++;
    }
}

static void scr_vote(struct n265_encoder *e, la_frame *f, const la_frame *prev)
{
    const n265_la *la = &e->la;
    int rows = (la->bh - 1) / 2;
    if (rows < 1)
        return;
    struct scr_rows c = { la, f, prev,
                          calloc((size_t)rows, sizeof *c.hist),
                          calloc((size_t)rows, sizeof *c.voters) };
    if (!c.hist || !c.voters) {
        free(c.hist);
        free(c.voters);
        return;
    }
    n265_par_for(e, PAR_SCR_VOTE, rows, scr_vote_row, &c);
    int hx[2 * GMV_R + 1] = { 0 }, hy[2 * GMV_R + 1] = { 0 };
    int voters = 0;
    for (int r = 0; r < rows; r++) {
        voters += c.voters[r];
        for (int d = 0; d <= 2 * GMV_R; d++) {
            hx[d] += c.hist[r][0][d];
            hy[d] += c.hist[r][1][d];
        }
    }
    free(c.hist);
    free(c.voters);
    /* the two most voted, each carrying a tenth of the voters */
    for (int k = 0; k < 2; k++) {
        int best = 0, bi = -1, vert = 0;
        for (int d = 0; d <= 2 * GMV_R; d++) {
            if (hx[d] > best) { best = hx[d]; bi = d; vert = 0; }
            if (hy[d] > best) { best = hy[d]; bi = d; vert = 1; }
        }
        if (bi < 0 || best < 6 || best * 10 < voters)
            break;
        f->gmv[f->gmv_n][0] = (int16_t)(vert ? 0 : bi - GMV_R);
        f->gmv[f->gmv_n][1] = (int16_t)(vert ? bi - GMV_R : 0);
        f->gmv_n++;
        if (vert)
            hy[bi] = 0;
        else
            hx[bi] = 0;
    }
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
 * On by default since 2026-09-09; 0 is the behavior before it, and
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

/* N265_LA_MEPRED: start the lookahead's lowres search where the motion
 * already is, instead of at the zero vector every time.
 *
 * `n265_la_push` runs a four-point diamond from (0,0) capped at eight
 * iterations, so it reaches EIGHT LOWRES PIXELS - sixteen luma pixels -
 * and no further. That is enough for CIF and for 60p video of ordinary
 * motion, and it is not enough for 2160p, still less for 2160p carrying
 * 24p motion in a 60p container: on `src_meridian_600` the exhaustive
 * cost keeps falling out to +-32 lowres pixels, and until the search
 * gets there the inter cost reads above the intra cost and the scenecut
 * detector fires on a shot that never changed.
 *
 * The expensive answer is a wider pattern. The cheap one is a better
 * STARTING POINT, and three are already to hand when the block is
 * reached: the vector of the block to its LEFT, the vector of the block
 * ABOVE it, and the vector THIS block had in the previous lookahead
 * frame. Motion is spatially and temporally coherent, so each of them
 * is usually within a diamond step or two of the answer, and a search
 * seeded from one chains along the motion trail rather than walking out
 * to it one lowres pixel at a time. The reach stops being the iteration
 * cap and becomes the cap plus whatever the neighbors already found.
 * `pair_cost` has always seeded itself this way; this is the same idea
 * one level down.
 *
 * Every candidate is DEDUPLICATED against the zero vector and against
 * the candidates before it, which is what keeps the clock flat: on
 * static content, on a held frame and on a pulldown repeat the
 * neighbors are all (0,0), every candidate collapses onto the zero
 * vector that was already priced, and the block pays a handful of
 * integer compares and not one extra SAD. Blocks that DO move pay four
 * or five SADs against a diamond that was going to run its full eight
 * iterations - thirty-three SADs - on exactly those blocks.
 *
 * Raster order, driver-side, integer: the left and top vectors are
 * written before this block reads them and the temporal one belongs to
 * a frame already pushed, so the answer is a pure function of the
 * queued sources at every thread count. On by default; 0 is the
 * pre-2026-09-18 search byte for byte. */
/* the component-wise median of three, which is the spatial predictor
 * every block-based codec forms and the one HEVC's own AMVP is a
 * variation on. Written here rather than reached for because nothing
 * else in this encoder needed one. */
static inline int med3(int a, int b, int c)
{
    if (a > b) { int t = a; a = b; b = t; }
    if (b > c) b = c;
    return a > b ? a : b;
}

/* N265_BADAPT_MVCAP: the saturation point of the b-adapt run rule's
 * motion term, in lowres pixels of L1 block displacement (phase 14
 * item 20).
 *
 * The rule in `build_plan_order` ends a B run when the mean lowres |MV|
 * times pcost/icost clears `N265_BADAPT_THR`, and the 0.35 it compares
 * was fitted against the search item 19 replaced. That search was a
 * four-point diamond from the zero vector capped at eight iterations,
 * each iteration one lowres pixel in one component, so the statistic it
 * produced could NOT EXCEED EIGHT however fast the picture moved: the
 * cutoff was fitted on a saturating mean and says nothing above its
 * saturation point. The predictors removed the saturation, the mean
 * grew by two to five times on the clips whose motion the old search
 * could not reach (sintel 2.60 -> 13.62, sita_toon 0.75 -> 3.87), and a
 * rule fitted on the bounded statistic fired on the unbounded one:
 * xfade_720p went from 23 P frames to 35, and the `opengop_radl`
 * conformance vector's run of seven B frames collapsed to one.
 *
 * So the term is saturated back, per block, before the mean is taken.
 * That is not a scale factor on the cutoff: a scale moves the whole
 * distribution and would end runs earlier on the clips whose motion the
 * old search DID measure, where the rule was right. Saturation touches
 * only the blocks that moved further than the old search could see.
 *
 * The value is MEASURED, and only its FORM is derived. Eight is the old
 * diamond's own reach and the honest upper bound, but the old search
 * rarely got there - it had to improve on every one of eight steps - so
 * a cap at eight leaves most of the growth in. The P/B censuses against
 * main and the eleven-clip corpus both pick 4 from candidates 8 down
 * to 2; the clips
 * whose field the old search could measure are unmoved by any of them,
 * so the value is decided by the three 720p clips alone and the curve
 * through them is steep. 0 is no saturation, which is item 19's rule
 * and the pre-item-20 branch byte for byte.
 *
 * N265_BADAPT_MVGATE is the other half of this refit and answers the
 * other question - whether the block moved at all - which the cap on
 * its own cannot (the conformance vector needs a cap of 2 and
 * stefan_cif loses 9% below 3).
 *
 * The VECTORS are not capped. `mvabs_sum` keeps the honest field for
 * the trace, the cu-tree propagation still follows it, and the costs
 * are untouched: this is the b-adapt run rule's statistic and nothing
 * else's. */
static int badapt_mvcap(void)
{
    static int v = -1;
    if (v < 0) { const char *e = n265_env("N265_BADAPT_MVCAP"); v = e ? atoi(e) : 4; }
    return v;
}

/* N265_BADAPT_MVGATE: how much of a block's static residual its vector
 * has to remove before the b-adapt run rule counts it as motion, in
 * percent (phase 14 item 20).
 *
 * The cap above is a bound on how FAR a block may count. This is a
 * statement about whether it moved at all, and the two catch different
 * failures. A diamond that starts at the zero vector only leaves it by
 * improving at every step, so under the old search a block with a
 * nonzero vector had earned it. Seed the same diamond from a
 * neighbor's vector and a block can end up far from the origin on a
 * match that is worth almost nothing - a fit to noise, which on grain
 * or on a dissolve is most of the picture. The run rule then reads a
 * mean |MV| made of motion that is not there.
 *
 * So a block counts its displacement only if the search removed at
 * least this share of the SAD the zero vector already had. Nothing else
 * reads the gate: the vector stands, the cost stands, and only the
 * b-adapt statistic is filtered.
 *
 * 20 ships. The `opengop_radl` conformance vector is what fixes the
 * value: 26 frames of testsrc2 under `noise=alls=12`, where main's own
 * term sits at 0.3391 against a 0.35 cutoff - a 3% margin - and item
 * 19's search took it to 0.4553 and collapsed a run of seven B frames
 * to one. The gate has to reach 15 before the run comes back and the
 * seek to the CRA reads the pictures it recorded again; 20 is that with
 * a margin, and it is the value at which stefan_cif, a real pan whose
 * blocks all earn their vectors, is still unmoved. 0 is item 19's
 * statistic. */
static int badapt_mvgate(void)
{
    static int v = -1;
    if (v < 0) { const char *e = n265_env("N265_BADAPT_MVGATE"); v = e ? atoi(e) : 20; }
    return v;
}

/* N265_CT_MVRATE: what cu-tree's propagation charges for the motion it
 * propagates along, in cost units per bit (phase 14 item 20).
 *
 * The propagation deposits `(intra + in) * (1 - pcost/icost)` of a
 * block's cost into the reference it came from: the fraction is how
 * much of this block the reference supplies. `pcost` is a bare SAD, so
 * the fraction is what an ORACLE would inherit, and item 19's search
 * moved it a long way toward one - the propagated total rose 5% on
 * foreman, 22% on xfade_720p, and the map's frame mean went with it.
 * With cu-tree off item 19's regression on foreman, stefan and
 * coastguard is not there at all; it is the map, and the map alone.
 *
 * The frame the encoder will really code is not coded by an oracle. It
 * pays for its motion, and a block whose prediction needs a long vector
 * inherits less than its SAD says because the bits that fetch it are
 * part of its cost. So the fraction is priced on SAD PLUS the vector's
 * own bits, on the exp-Golomb shape, which is the same D + lambda R
 * every full-resolution search in this encoder already uses.
 *
 * Charged against the ORIGIN, not against a spatial predictor. The
 * choice is measured (a predictor-relative
 * term leaves xfade_720p at +7.04% where the origin-relative one reads
 * +5.79%) and it has a reason - a crossfade's field is coherent noise,
 * so it agrees with its neighbors and a predictor-relative term
 * charges it nothing, while the origin is the only reference a blend
 * cannot agree with. It also keeps the term monotone in |MV|, which is
 * what the propagation is being asked about.
 *
 * 16 ships: the per-block floor `pcost` already carries, so one bit of
 * motion is priced at one block's existence. 32 reads better on
 * foreman and worse on bus_cif; 0 is the pre-item-20 fraction. The
 * SEARCH is not touched - the vectors, the scenecut's ratio and the
 * rate control's complexity are item 19's, which is what keeps the 4K
 * reach this item is funded by. */
static int ct_mvrate(void)
{
    static int v = -1;
    if (v < 0) { const char *e = n265_env("N265_CT_MVRATE"); v = e ? atoi(e) : 16; }
    return v;
}

/* N265_SC_FAILW: the share of a frame's INTRA cost, sitting on blocks the
 * lowres search cannot undercut intra on at all, above which the detector
 * calls a cut whatever the sum ratio says (phase 14 item 21).
 *
 * `sc_ratio` is a ratio of SUMS, and that is what item 19's search broke.
 * A flat or repetitive picture gives a minority of blocks very cheap
 * matches a long way from home - a cartoon's flat fields match any other
 * flat field - and those few blocks carry the whole frame's sum under the
 * 0.75 bar even where the picture genuinely changed. sintel_720p's cut at
 * display 14 reads 1.16 against the old search and 0.76 against the honest
 * one, and the guard the honest ratio then fails is the spike.
 *
 * The share below does not average: it asks how much of the picture,
 * weighted by what intra would pay for it, inter prediction cannot do
 * anything with. Measured, it is the one statistic that separates a cut
 * from fast coherent motion once the search is honest:
 *
 *   Meridian's pulldown frames, which MUST NOT cut      0.063 - 0.073
 *   Meridian display 311, a real cut                    0.690
 *   sintel display 14, the cut item 19 lost             0.422
 *   sintel display 118, the cut it kept                 0.397
 *   sita_flat display 117, which must not cut           0.300
 *
 * The ABSOLUTE bar and the spike are untouched; this only ever ADDS a cut,
 * and the frames it adds are the ones a ratio of sums cannot see.
 *
 * 0.40 ships. It is set from BELOW by sintel's 0.422 and from ABOVE by
 * sita_flat_720p's display 117, which reads 0.300 and must not cut - main
 * does not, and the arm that cuts there reads 7 points worse on that clip.
 * Over the board's seventeen clips at `--crf 27` it moves ONE cut list:
 * sintel gains display 14, which main has and item 19 lost. 0 is off and is
 * the pre-item-21 detector. */
static double sc_failw_bar(void)
{
    static double v = -1.0;
    if (v < 0.0) { const char *e = n265_env("N265_SC_FAILW"); v = e ? atof(e) : 0.40; }
    return v;
}

/* N265_SC_FAILW_SPIKE: how far above its own running level the failure
 * share has to sit before the trigger above fires (phase 14 item 21).
 *
 * The same guard, for the same reason, the ratio's spike already carries: a
 * FADE fails inter prediction on more of the picture every frame, so a bare
 * bar on the share cuts in the middle of one (fade_cif gains cuts at
 * display 56 and 64 around the cut at 60 that is real). A cut is a STEP in
 * the share and a fade is a ramp, so the level separates them - the level
 * rises with the ramp and the quotient stays near one, where sintel's cut
 * at display 14 stands 2x over its own. */
static double sc_failw_spike(void)
{
    static double v = -1.0;
    if (v < 0.0) { const char *e = n265_env("N265_SC_FAILW_SPIKE"); v = e ? atof(e) : 1.6; }
    return v;
}

/* N265_SC_JUMP (default 0.10; 0 is the detector before it) and
 * N265_SC_FLOOR (an arm, off), phase 14 item 35, ported from yah264.
 * A cut must also be a JUMP: its ratio at least SC_JUMP above the
 * largest of the three frames pushed before it, or its unpredictable
 * share (1 - ratio) at most half of theirs. The max over three is for a
 * pulldown cadence, where every other frame repeats and reads near zero.
 *
 * Our detector has no threshold that falls with the GOP's age, which is
 * what admitted yah264's false cuts; ours cuts on a fixed bar (0.75) and
 * a spike over a running level (N265_SC_SPIKE). It places no cut on
 * sunflower or on chimera 2160p's first 300 frames. The spike still
 * admits one false cut in a 24p-in-60p cadence: Meridian display 434
 * reads 0.75 after 0.08 / 0.67 / 0.08, a car crossing the street with no
 * change of shot, and the EMA averages the cadence's zeros into a level
 * the 0.75 clears 1.8x over. The jump test takes it out (BD-VMAF-NEG
 * -5.36% on the 120-frame window around it) and moves nothing else on
 * the corpus, the multi-shot sequences or the tier S clips.
 *
 * SC_FLOOR is a minimum ratio, unless the frame's intra cost is over
 * twice its predecessor's (a scene rising out of black). It is refused:
 * our ratio is our own pcost/icost, and our real cuts read from 0.76
 * (sintel display 14) up past 3, so any floor that would add a guard
 * over the 0.75 bar removes a real cut. */
static double sc_jump(void)
{
    static double v = -1.0;
    if (v < 0.0) { const char *e = n265_env("N265_SC_JUMP"); v = e ? atof(e) : 0.10; if (v < 0.0) v = 0.0; }
    return v;
}

static double sc_floor(void)
{
    static double v = -1.0;
    if (v < 0.0) { const char *e = n265_env("N265_SC_FLOOR"); v = e ? atof(e) : 0.0; if (v < 0.0) v = 0.0; }
    return v;
}

/* 1 when a cut candidate passes both guards (or both are off) */
static int sc_jump_ok(const la_frame *f)
{
    double r = (double)f->sc_ratio;
    double fl = sc_floor();
    if (fl > 0.0 && r < fl && f->sc_prev_ic >= 0 &&
        2 * f->sc_prev_ic >= (int64_t)f->icost_sum)
        return 0;
    double j = sc_jump();
    if (j <= 0.0 || f->sc_rn <= 0)
        return 1;
    double m = (double)f->sc_rprev[0];
    for (int k = 1; k < f->sc_rn; k++)
        if ((double)f->sc_rprev[k] > m)
            m = (double)f->sc_rprev[k];
    /* or the unpredictable share at most half of theirs: a real cut out
     * of content already near intra cannot climb far, but its headroom
     * collapses */
    return r - m >= j || 1.0 - m >= 2.0 * (1.0 - r);
}

/* N265_SC_FORCE (instrument): a comma-separated list of DISPLAY indices the
 * detector must call a cut at, whatever it reads. It exists so a frame type
 * can be moved on its own, with nothing else in the encoder changed, which
 * is how item 21 priced the cut item 19 lost on sita_flat_720p. Never a
 * shipped path. */
static int sc_forced(int display_idx)
{
    const char *v = n265_env("N265_SC_FORCE");
    if (!v || display_idx <= 0)
        return 0;
    for (const char *p = v; *p; ) {
        int n = 0, got = 0;
        while (*p >= '0' && *p <= '9') { n = n * 10 + (*p++ - '0'); got = 1; }
        if (got && n == display_idx)
            return 1;
        while (*p && *p != ',')
            p++;
        if (*p == ',')
            p++;
    }
    return 0;
}

/* N265_SC_SPIKE: how far above the running level a frame's cost ratio
 * has to sit before the scenecut detector will call it a cut (phase 14
 * item 20).
 *
 * The detector cuts on `ratio >= 0.75 && ratio >= SPIKE * ema`. The
 * second test is the one that does the work, and the 1.3 it was fitted
 * with was fitted on a COMPRESSED scale. The old search's pcost carried
 * the search's own failure - a large additive term the content had no
 * say in - and an additive offset on both the frame and the level
 * squeezes their ratio toward one. On sita_toon_720p the level sat at
 * 1.0 and the hardest frame of the shot read 1.21, a spike of 1.21 that
 * did not clear 1.3. Honestly measured the same level is 0.55 and the
 * same frame 0.86, a spike of 1.56 - the frame did not change, the
 * offset came off. With the offset off, 1.3 stops guarding: the branch
 * cut at display 13, 25, 37, 49, 61, 73 and 85, the first frame
 * `--min-keyint` allowed every time, where main cut five times on
 * content.
 *
 * Solving `(r + c) / (L + c) = 1.3` for the honest r at sita_toon's own
 * offset gives 1.81, and 1.8 ships. The corpus-wide re-derivation is
 * weaker than that single clip - the offset is near zero on every clip
 * the old search could measure, where 1.8 and 1.3 are the same rule -
 * so the constant is carried by the clips that have an offset at all.
 * 1.3 is the pre-item-20 detector. The ABSOLUTE bar is untouched: it is
 * what takes 34 of Meridian's 38 intra pictures out, and it decides
 * nothing this constant is asked about. */
static double sc_spike(void)
{
    static double v = -1.0;
    if (v < 0.0) { const char *e = n265_env("N265_SC_SPIKE"); v = e ? atof(e) : 1.8; }
    return v;
}

/* the bit length of a magnitude on an exp-Golomb shape: two bits per
 * doubling, nothing for zero. The same curve `n265_mv_bits` charges at
 * full resolution, without the table. */
static inline int la_mvblen(int v)
{
    unsigned a = (unsigned)(v < 0 ? -v : v);
    return a ? 2 * (32 - __builtin_clz(a)) : 0;
}

static inline uint32_t la_mvcost(int w, int dx, int dy)
{
    return w ? (uint32_t)w * (uint32_t)(la_mvblen(dx) + la_mvblen(dy)) : 0u;
}

static int la_mepred(void)
{
    static int v = -1;
    if (v < 0) { const char *e = n265_env("N265_LA_MEPRED"); v = e ? atoi(e) : 1; }
    return v;
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

/* N265_JIT (phase 14 item 62): flags a frame whose last N265_JIT_WIN
 * frames show a handheld camera's jitter over fine texture. Every frame
 * of the window has a coherent field (at least N265_JIT_COH of its
 * blocks within two lowres pixels of the median vector), the median
 * vector's second difference averages at least N265_JIT_J lowres pixels
 * (each frame's saturated at 4), and the intra cost averages at least
 * N265_JIT_IC per lowres 8x8 block. The pans and follow shots of tiers
 * S, M and L move too smoothly to reach the jitter bar. 0 is off. */
static int jit_on(void)
{
    static int v = -1;
    if (v < 0) { const char *e = n265_env("N265_JIT"); v = e ? atoi(e) : 0; }
    return v;
}

static int jit_trace(void)
{
    static int v = -1;
    if (v < 0) v = n265_env("N265_JIT_TRACE") != NULL;
    return v;
}

static int jit_win(void)
{
    static int v = -1;
    if (v < 0) {
        const char *e = n265_env("N265_JIT_WIN");
        v = e ? atoi(e) : 24;
        if (v < 1) v = 1;
        if (v > 29) v = 29;
    }
    return v;
}

static double jit_j(void)
{
    static double v = -1.0;
    if (v < 0.0) { const char *e = n265_env("N265_JIT_J"); v = e ? atof(e) : 2.2; }
    return v;
}

static double jit_coh(void)
{
    static double v = -1.0;
    if (v < 0.0) { const char *e = n265_env("N265_JIT_COH"); v = e ? atof(e) : 0.75; }
    return v;
}

static double jit_ic(void)
{
    static double v = -1.0;
    if (v < 0.0) { const char *e = n265_env("N265_JIT_IC"); v = e ? atof(e) : 720.0; }
    return v;
}

/* the driver resolves this file's knobs at open, before a worker can be
 * the one that writes them first; see n265_ctu_warm */
void n265_la_warm(void)
{
    (void)sc_hold();
    (void)la_mepred();
    (void)badapt_mvcap();
    (void)badapt_mvgate();
    (void)ct_mvrate();
    (void)sc_spike();
    (void)sc_failw_bar();
    (void)sc_failw_spike();
    (void)sc_jump();
    (void)sc_floor();
    (void)jit_on();
    (void)jit_win();
    (void)jit_trace();
    (void)jit_j();
    (void)jit_coh();
    (void)jit_ic();
    (void)n265_flat_gate();
    (void)n265_screen();
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

/* n265_la_push's passes over the picture, one row per item */
struct push_rows {
    struct n265_encoder *e;
    la_frame *f;
    const la_frame *prev;
    const pixel *const *plane;
    const int *stride;
    int mvcap, mvgate, mepred, ctrate;
};

static void push_copy_row(void *ctx, int i)
{
    const struct push_rows *pr = ctx;
    const struct n265_encoder *e = pr->e;
    int dh0 = e->pad_h, dhc = e->pad_h >> e->csy;
    int c = i < dh0 ? 0 : i < dh0 + dhc ? 1 : 2;
    int y = c == 0 ? i : c == 1 ? i - dh0 : i - dh0 - dhc;
    int w = c ? e->param.width >> e->csx : e->param.width;
    int h = c ? e->param.height >> e->csy : e->param.height;
    int dw = c ? e->pad_w >> e->csx : e->pad_w;
    const pixel *s = pr->plane[c] + (y < h ? y : h - 1) * pr->stride[c];
    pixel *d = pr->f->src[c] + y * dw;
    memcpy(d, s, (size_t)w * sizeof(pixel));
    for (int x = w; x < dw; x++)     /* a sample fill, not a byte one */
        d[x] = s[w - 1];
}

static void push_lowres_row(void *ctx, int y)
{
    const struct push_rows *pr = ctx;
    const struct n265_encoder *e = pr->e;
    int lw = e->la.lw, lh = e->la.lh;
    int sw = e->param.width, sh = e->param.height;
    const pixel *s = pr->f->src[0];
    int ps = e->pad_w;
    pixel *lo = pr->f->lowres;
    /* the samples a step reads without the edge clamps */
    int xin = sw >> 1 < lw ? sw >> 1 : lw;
    int yin = sh >> 1 < lh ? sh >> 1 : lh;
    int x = 0;
    if (y < yin) {
        const pixel *restrict r0 = s + (size_t)(2 * y) * ps;
        const pixel *restrict r1 = r0 + ps;
        pixel *restrict d = lo + (size_t)y * lw;
        N265_VECTORIZE
        for (; x < xin; x++)
            d[x] = (pixel)((r0[2 * x] + r0[2 * x + 1] + r1[2 * x] +
                            r1[2 * x + 1] + (2 << BIT_DEPTH_SHIFT))
                           >> (2 + BIT_DEPTH_SHIFT));
    }
    for (; x < lw; x++) {
        int sy = 2 * y, sx = 2 * x;
        int h1 = sy + 1 < sh ? sy + 1 : sy;
        int w1 = sx + 1 < sw ? sx + 1 : sx;
        if (sy >= sh)
            sy = h1 = sh - 1;
        if (sx >= sw)
            sx = w1 = sw - 1;
        /* the mean, brought back to 8-bit magnitudes: the shift is
         * 2 at 8 bits, which is the plain 2x2 mean it has always
         * been (la.h says why the proxy stays narrow) */
        lo[y * lw + x] = (pixel)((s[sy * ps + sx] +
            s[sy * ps + w1] + s[h1 * ps + sx] + s[h1 * ps + w1] +
            (2 << BIT_DEPTH_SHIFT)) >> (2 + BIT_DEPTH_SHIFT));
    }
}

static void push_cost_row(void *ctx, n265_par *w, int by)
{
    const struct push_rows *c = ctx;
    const n265_la *la = &c->e->la;
    la_frame *f = c->f;
    const la_frame *prev = c->prev;
    int lw = la->lw, lh = la->lh;
    uint64_t *sum = la->row_sum[by];
    for (int k = 0; k < 7; k++)
        sum[k] = 0;
    for (int bx = 0; bx < la->bw; bx++) {
        if (w)
            n265_par_need(w, by, bx + 2 < la->bw ? bx + 2 : la->bw);
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

        uint32_t bzsad = 0, bsadf = 0;
        if (!prev) {
            f->pcost[b] = f->icost[b];
            f->zsad[b] = f->icost[b];
            f->mv[2 * b] = f->mv[2 * b + 1] = 0;
        } else {
            /* small diamond at lowres, start at zero */
            int mx = 0, my = 0;
            uint32_t best = sad8(blk, lw,
                                 prev->lowres + y0 * lw + x0, lw);
            bzsad = best;          /* the zero vector's own SAD */
            /* the search's own starting cost IS the zero-vector
             * distance the duplicate chain is built on: taken here,
             * before the diamond moves off it, for one add a block */
            sum[5] += best;
            /* N265_LA_MEPRED: the motion vector predictors. Left,
             * above, above-right, their component-wise median, and
             * this block's vector in the previous frame. The median
             * is formed only where all three spatial neighbors
             * exist; where they do not, the ones that do stand on
             * their own. */
            if (c->mepred) {
                int cand[7][2];
                int nc = 0;
                int have_l = bx > 0, have_t = by > 0;
                int have_tr = by > 0 && bx + 1 < la->bw;
                int lx = 0, ly = 0, tx0 = 0, ty0 = 0, rx = 0, ry = 0;
                if (have_l) {
                    lx = f->mv[2 * (b - 1)];
                    ly = f->mv[2 * (b - 1) + 1];
                    cand[nc][0] = lx; cand[nc][1] = ly; nc++;
                }
                if (have_t) {
                    tx0 = f->mv[2 * (b - la->bw)];
                    ty0 = f->mv[2 * (b - la->bw) + 1];
                    cand[nc][0] = tx0; cand[nc][1] = ty0; nc++;
                }
                if (have_tr) {
                    rx = f->mv[2 * (b - la->bw + 1)];
                    ry = f->mv[2 * (b - la->bw + 1) + 1];
                    cand[nc][0] = rx; cand[nc][1] = ry; nc++;
                }
                if (have_l && have_t && have_tr) {
                    cand[nc][0] = med3(lx, tx0, rx);
                    cand[nc][1] = med3(ly, ty0, ry);
                    nc++;
                }
                cand[nc][0] = prev->mv[2 * b];
                cand[nc][1] = prev->mv[2 * b + 1];
                nc++;
                for (int g = 0; g < f->gmv_n; g++) {
                    cand[nc][0] = f->gmv[g][0];
                    cand[nc][1] = f->gmv[g][1];
                    nc++;
                }
                for (int k = 0; k < nc; k++) {
                    int cx = cand[k][0], cy = cand[k][1];
                    /* the zero vector is already priced, and a
                     * candidate the list has seen is already priced
                     * too: on content that does not move this is
                     * every one of them */
                    if (!cx && !cy)
                        continue;
                    int seen = 0;
                    for (int j = 0; j < k; j++)
                        if (cand[j][0] == cx && cand[j][1] == cy) {
                            seen = 1;
                            break;
                        }
                    if (seen)
                        continue;
                    if (x0 + cx < 0 || y0 + cy < 0 ||
                        x0 + cx + 8 > lw || y0 + cy + 8 > lh)
                        continue;
                    uint32_t s = sad8(blk, lw,
                        prev->lowres + (y0 + cy) * lw + x0 + cx, lw);
                    if (s < best) {
                        best = s;
                        mx = cx;
                        my = cy;
                    }
                }
            }
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
            bsadf = best;
            uint32_t pc = best + 16;
            f->pcost[b] = (uint16_t)(pc > 65535 ? 65535 : pc);
            /* the same block priced with no search at all, in the same
             * units, for N265_LA_MVSTAT */
            uint32_t zc = bzsad + 16;
            f->zsad[b] = (uint16_t)(zc > 65535 ? 65535 : zc);
            f->mv[2 * b] = (int16_t)mx;
            f->mv[2 * b + 1] = (int16_t)my;
        }
        {
            /* the propagate fraction cu-tree reads, once per frame
             * rather than once per window it sits in */
            float ic = f->icost[b], pc = f->pcost[b];
            if (c->ctrate)
                pc += (float)la_mvcost(c->ctrate, f->mv[2 * b],
                                       f->mv[2 * b + 1]);
            f->ctfrac[b] = pc >= ic ? -1.0f : 1.0f - pc / ic;
        }
        sum[0] += f->icost[b];
        sum[1] += f->pcost[b];
        if (prev && f->pcost[b] >= f->icost[b])
            sum[6] += f->icost[b];
        {
            int l1 = abs(f->mv[2*b]) + abs(f->mv[2*b+1]);
            sum[2] += (uint64_t)l1;
            sum[3] += (uint64_t)(la_mvblen(f->mv[2*b]) +
                                        la_mvblen(f->mv[2*b+1]));
            if (c->mvcap > 0 && l1 > c->mvcap)
                l1 = c->mvcap;
            /* N265_BADAPT_MVGATE: a vector that bought nothing is
             * not motion. */
            if (c->mvgate > 0 && (uint64_t)(bzsad - bsadf) * 100u <
                                  (uint64_t)c->mvgate * bzsad)
                l1 = 0;
            sum[4] += (uint64_t)l1;
        }
        if (w)
            n265_par_post(w, by, bx + 1);
    }
    int lo = INT16_MAX, hi = INT16_MIN;
    for (int b = by * la->bw; b < (by + 1) * la->bw; b++) {
        if (f->mv[2 * b + 1] < lo)
            lo = f->mv[2 * b + 1];
        if (f->mv[2 * b + 1] > hi)
            hi = f->mv[2 * b + 1];
    }
    f->mvy_lo[by] = (int16_t)lo;
    f->mvy_hi[by] = (int16_t)hi;
}

/* the median of a histogram of n values offset by `off` */
static int hist_median(const int *h, int len, int n, int off)
{
    int c = 0;
    for (int v = 0; v < len; v++) {
        c += h[v];
        if (c > n / 2)
            return v - off;
    }
    return len - 1 - off;
}

static void la_jitter(n265_la *la, la_frame *f, const la_frame *prev)
{
    enum { R = 512, NR = 2 * R };
    int nb = la->bw * la->bh;
    int mx = 0, my = 0;
    float coh = 0.0f;
    if (prev && nb > 0) {
        int hx[NR] = { 0 }, hy[NR] = { 0 };
        for (int b = 0; b < nb; b++) {
            int x = f->mv[2 * b], y = f->mv[2 * b + 1];
            hx[x < -R ? 0 : x >= R ? NR - 1 : x + R]++;
            hy[y < -R ? 0 : y >= R ? NR - 1 : y + R]++;
        }
        mx = hist_median(hx, NR, nb, R);
        my = hist_median(hy, NR, nb, R);
        int n = 0;
        for (int b = 0; b < nb; b++)
            n += abs(f->mv[2 * b] - mx) + abs(f->mv[2 * b + 1] - my) <= 2;
        coh = (float)n / (float)nb;
    }
    int pos = la->jh_n % 32;
    la->jh[pos].mx = (int16_t)mx;
    la->jh[pos].my = (int16_t)my;
    la->jh[pos].coh = coh;
    la->jh[pos].ic = nb ? (float)f->icost_sum / (float)nb : 0.0f;
    la->jh_n++;
    int win = jit_win();
    if (la->jh_n < win + 2)
        return;
    double j = 0.0, ic = 0.0;
    for (int k = 0; k < win; k++) {
        int i0 = (la->jh_n - 1 - k) % 32;
        int i1 = (la->jh_n - 2 - k) % 32;
        int i2 = (la->jh_n - 3 - k) % 32;
        if ((double)la->jh[i0].coh < jit_coh())
            return;
        int a = abs(la->jh[i0].mx - 2 * la->jh[i1].mx + la->jh[i2].mx) +
                abs(la->jh[i0].my - 2 * la->jh[i1].my + la->jh[i2].my);
        j += a < 4 ? a : 4;
        ic += la->jh[i0].ic;
    }
    if (j < jit_j() * win || ic < jit_ic() * win)
        return;
    if (jit_trace())
        fprintf(stderr, "JIT d=%d j=%.3f ic=%.1f\n", f->display_idx,
                j / win, ic / win);
    /* the window's frames still queued take the flag too */
    for (int k = la->count - win + 1; k <= la->count; k++)
        if (k >= 0)
            la->f[k].jit = 1;
}

void n265_la_push(struct n265_encoder *e, const pixel *const plane[3],
                  const int stride[3])
{
    n265_la *la = &e->la;
    la_frame *f = &la->f[la->count];
    la_frame *prev = la->count > 0 ? &la->f[la->count - 1] : NULL;
    f->valid = 1;
    f->display_idx = e->in_display_idx;
    f->mcstf_filt = e->mcstf_next_filt;
    /* a recycled slot carries the last frame's plan; this one has none
     * until the shot planner reaches it */
    f->shot_off = 0.0f;
    f->shot_planned = 0;

    /* Edge-replicate into the padded geometry here, once. encode_frame
     * used to re-pad this into a second frame-sized buffer every frame;
     * the queue slot IS the padded source now (threading-design sec 4). */
    struct push_rows pr = { e, f, prev, plane, stride, badapt_mvcap(),
                            badapt_mvgate(), la_mepred(), ct_mvrate() };
    n265_par_for(e, PAR_COPY, e->pad_h + 2 * (e->pad_h >> e->csy),
                 push_copy_row, &pr);

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
                 * the filter's own edge behavior standing in for it */
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
    n265_par_for(e, PAR_LOWRES, la->lh, push_lowres_row, &pr);

    f->scr = n265_screen() && scr_detect(e, f->src[0], e->pad_w);
    f->gmv_n = 0;
    if (f->scr && prev)
        scr_vote(e, f, prev);

    /* per-8x8 costs. A block's vector predictors are its left, above and
     * above-right neighbors, so the rows run as a wavefront. */
    n265_par_wave(e, PAR_COST, la->bh, push_cost_row, &pr);
    uint64_t sums[7] = { 0 };
    for (int by = 0; by < la->bh; by++)
        for (int k = 0; k < 7; k++)
            sums[k] += la->row_sum[by][k];
    f->icost_sum = sums[0];
    f->pcost_sum = sums[1];
    f->mvabs_sum = sums[2];
    f->mvbits_sum = sums[3];
    f->mvsat_sum = sums[4];
    f->zsad_sum = sums[5];
    uint64_t ifail_sum = sums[6];   /* the intra cost of the blocks inter loses */
    /* N265_LA_MVSTAT (instrument, phase 14 item 21): the statistics a
     * scenecut rule could use to tell a true cut from fast COHERENT
     * motion, both of which read the same low cost ratio once the search
     * is honest. `coh` is the share of blocks whose vector sits within
     * two lowres pixels of the frame's own median vector; `gcost` is the
     * frame priced with every block forced onto that median (or onto the
     * zero vector, whichever is cheaper), which is what the cost would be
     * if the field had to be globally coherent. One extra SAD a block,
     * paid only when the instrument is armed. */
    if (prev && n265_env("N265_LA_MVSTAT")) {
        int nb2 = la->bw * la->bh;
        int16_t *xs = malloc((size_t)nb2 * sizeof(int16_t));
        int16_t *ys = malloc((size_t)nb2 * sizeof(int16_t));
        if (xs && ys) {
            for (int b = 0; b < nb2; b++) {
                xs[b] = f->mv[2 * b];
                ys[b] = f->mv[2 * b + 1];
            }
            for (int i = 1; i < nb2; i++) {     /* insertion sort: an
                                                 * instrument, not a path */
                int16_t vx = xs[i], vy = ys[i];
                int j = i - 1;
                while (j >= 0 && xs[j] > vx) { xs[j + 1] = xs[j]; j--; }
                xs[j + 1] = vx;
                j = i - 1;
                while (j >= 0 && ys[j] > vy) { ys[j + 1] = ys[j]; j--; }
                ys[j + 1] = vy;
            }
            int medx = xs[nb2 / 2], medy = ys[nb2 / 2];
            int coh = 0, fail = 0, smooth = 0;
            uint64_t failw = 0, icw = 0;
            uint64_t gcost = 0;
            for (int by = 0; by < la->bh; by++)
                for (int bx = 0; bx < la->bw; bx++) {
                    int b = by * la->bw + bx;
                    int dx = f->mv[2 * b] - medx, dy = f->mv[2 * b + 1] - medy;
                    if (abs(dx) + abs(dy) <= 2)
                        coh++;
                    /* the share of the picture inter prediction FAILS on,
                     * by count and weighted by what intra would pay: a
                     * true cut is unpredictable where it is expensive, and
                     * fast coherent motion is not */
                    icw += f->icost[b];
                    if (f->pcost[b] >= f->icost[b]) {
                        fail++;
                        failw += f->icost[b];
                    }
                    /* LOCAL smoothness: agreement with the neighbors that
                     * were already written, which is a different question
                     * from agreement with the frame's median */
                    if (bx > 0 && by > 0) {
                        int lx2 = f->mv[2 * (b - 1)], ly2 = f->mv[2 * (b - 1) + 1];
                        int tx2 = f->mv[2 * (b - la->bw)];
                        int ty2 = f->mv[2 * (b - la->bw) + 1];
                        int sx = f->mv[2 * b] - ((lx2 + tx2) >> 1);
                        int sy = f->mv[2 * b + 1] - ((ly2 + ty2) >> 1);
                        if (abs(sx) + abs(sy) <= 2)
                            smooth++;
                    }
                    int x0 = bx * 8, y0 = by * 8;
                    if (x0 + 8 > la->lw) x0 = la->lw - 8;
                    if (y0 + 8 > la->lh) y0 = la->lh - 8;
                    const pixel *blk = f->lowres + y0 * la->lw + x0;
                    uint32_t g = f->zsad[b] > 16 ? f->zsad[b] - 16u : 0u;
                    if (x0 + medx >= 0 && y0 + medy >= 0 &&
                        x0 + medx + 8 <= la->lw && y0 + medy + 8 <= la->lh) {
                        uint32_t sm = sad8(blk, la->lw,
                            prev->lowres + (y0 + medy) * la->lw + x0 + medx,
                            la->lw);
                        if (sm < g)
                            g = sm;
                    }
                    gcost += g + 16u;
                }
            fprintf(stderr, "mvstat d=%d p=%llu i=%llu z=%llu g=%llu "
                    "med=%d,%d coh=%.3f fail=%.3f failw=%.3f smooth=%.3f "
                    "mvabs=%.3f\n", f->display_idx,
                    (unsigned long long)f->pcost_sum,
                    (unsigned long long)f->icost_sum,
                    (unsigned long long)(f->zsad_sum + (uint64_t)nb2 * 16u),
                    (unsigned long long)gcost, medx, medy,
                    (double)coh / (double)nb2,
                    (double)fail / (double)nb2,
                    icw ? (double)failw / (double)icw : 0.0,
                    (double)smooth / (double)nb2,
                    (double)f->mvabs_sum / (double)nb2);
        }
        free(xs);
        free(ys);
    }
    f->jit = 0;
    if (jit_on())
        la_jitter(la, f, prev);
    f->sc_ratio = (float)f->pcost_sum /
                  (float)(f->icost_sum ? f->icost_sum : 1);
    f->sc_failw = f->icost_sum ? (float)((double)ifail_sum /
                                         (double)f->icost_sum) : 0.0f;
    f->sc_ema = la->sc_ema;
    f->failw_lvl = la->failw_ema;
    for (int k = 0; k < 3; k++)
        f->sc_rprev[k] = la->sc_rhist[k];
    f->sc_rn = la->sc_rn;
    f->sc_prev_ic = la->sc_last_ic;
    if (f->display_idx > 0 && f->icost_sum > 0) {
        la->sc_rhist[2] = la->sc_rhist[1];
        la->sc_rhist[1] = la->sc_rhist[0];
        la->sc_rhist[0] = f->sc_ratio;
        if (la->sc_rn < 3)
            la->sc_rn++;
    }
    la->sc_last_ic = (int64_t)f->icost_sum;
    int nblk = la->bw * la->bh;
    int held = sc_hold() && f->pcost_sum < (uint64_t)nblk * 17;
    if (!held) {
        la->sc_ema = la->sc_ema == 0.0f
                         ? f->sc_ratio
                         : 0.5f * la->sc_ema + 0.5f * f->sc_ratio;
        /* the same level, on the same frames, for the failure share: a
         * fade fails on more of the picture every frame, so its level
         * rises with it and the quotient stays near one */
        la->failw_ema = la->failw_ema == 0.0f
                            ? f->sc_failw
                            : 0.5f * la->failw_ema + 0.5f * f->sc_failw;
    }
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
    /* --shot-list: the list is the only source of cuts, so every rung of
     * a ladder cuts at the same frames */
    if (e->shotl_on)
        return n265_shotl_has(e, f->display_idx);
    /* cut when inter fails to undercut intra AND the ratio spikes over
     * its running level - a steady high ratio (grain, foliage) is not a
     * scene change */
    int cut = f->sc_ratio >= 0.75f &&
              (f->sc_ema == 0.0f ||
               (double)f->sc_ratio >= sc_spike() * (double)f->sc_ema);
    /* N265_SC_FAILW: the second, one-sided trigger. It may only ADD a cut
     * the ratio of sums cannot see. */
    double fwbar = sc_failw_bar();
    if (!cut && fwbar > 0.0 && (double)f->sc_failw >= fwbar &&
        (f->failw_lvl == 0.0f ||
         (double)f->sc_failw >= sc_failw_spike() * (double)f->failw_lvl))
        cut = 1;
    if (cut && !sc_jump_ok(f))
        cut = 0;
    if (!cut && sc_forced(f->display_idx))
        cut = 1;
    if (n265_env("N265_SC_TRACE"))
        fprintf(stderr, "sc d=%d p=%llu i=%llu ratio=%.2f failw=%.3f "
                "flvl=%.3f cut=%d\n",
                f->display_idx, (unsigned long long)f->pcost_sum,
                (unsigned long long)f->icost_sum,
                (double)f->pcost_sum / (double)(f->icost_sum ? f->icost_sum : 1),
                (double)f->sc_failw, (double)f->failw_lvl, cut);
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

/* --- the bits model (`--b-adapt 4`) --- */

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
 * what a quantizer followed by an entropy coder does. A per-block syntax
 * floor is deliberately absent: the block count is the same for every
 * candidate and every frame, so a constant per block adds the same
 * amount to every candidate's amortized price and cannot move the pick. */
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
 * Amortized over A + 1 frames and minimized over A, ties to the longer
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

/* The cu-tree propagation of one frame into its targets. Split over the
 * pool it runs by bands of target rows, each visiting in block order the
 * source rows whose motion can reach it, so every target sample takes its
 * adds in the order of the whole-frame loop. */
struct ct_prop {
    const n265_la *la;
    const la_frame *f;
    struct { int idx; float w; int neg; int scale; } tgt[2];
    int nt;
    int band;                   /* target rows per band */
};

/* source rows [by0, by1), adds into target rows [ty0, ty1) */
static void ct_prop_rows(const struct ct_prop *c, int by0, int by1, int ty0,
                         int ty1)
{
    const n265_la *la = c->la;
    const la_frame *f = c->f;
    int bw = la->bw;
    for (int by = by0; by < by1; by++)
    for (int bx = 0; bx < bw; bx++) {
        int b = by * bw + bx;
        /* N265_CT_MVRATE: the propagate fraction is priced on the
         * cost the CODER pays for this block - the vector's own bits
         * included - and not on the lookahead's bare distortion
         * (n265_la_push forms it). */
        float frac = f->ctfrac[b];
        if (frac < 0.0f)
            continue;
        float ic = f->icost[b];
        float amount = (ic + f->prop[b]) * frac;
        for (int t = 0; t < c->nt; t++) {
            la_frame *ref = &la->f[c->tgt[t].idx];
            int mvx = f->mv[2 * b] * c->tgt[t].scale;
            int mvy = f->mv[2 * b + 1] * c->tgt[t].scale;
            if (c->tgt[t].neg) {
                mvx = -mvx;
                mvy = -mvy;
            }
            int px = bx * 8 + mvx, py = by * 8 + mvy;
            int gx = px >> 3, gy = py >> 3;
            int fx = px & 7, fy = py & 7;
            float wx1 = fx / 8.0f, wy1 = fy / 8.0f;
            float aw = amount * c->tgt[t].w;
            if (gx >= 0 && gy >= ty0 && gx + 1 < bw && gy + 1 < ty1) {
                /* the loop below, with no edge to test */
                float *p0 = ref->prop + gy * bw + gx, *p1 = p0 + bw;
                float w;
                w = (1 - wx1) * (1 - wy1);
                p0[0] += aw * w;
                w = wx1 * (1 - wy1);
                p0[1] += aw * w;
                w = (1 - wx1) * wy1;
                p1[0] += aw * w;
                w = wx1 * wy1;
                p1[1] += aw * w;
                continue;
            }
            for (int dy = 0; dy < 2; dy++)
                for (int dx = 0; dx < 2; dx++) {
                    int tx = gx + dx, ty = gy + dy;
                    if (tx < 0 || ty < ty0 || tx >= bw || ty >= ty1)
                        continue;
                    float w = (dx ? wx1 : 1 - wx1) *
                              (dy ? wy1 : 1 - wy1);
                    ref->prop[ty * bw + tx] += aw * w;
                }
        }
    }
}

static void ct_prop_all(void *ctx)
{
    const struct ct_prop *c = ctx;
    ct_prop_rows(c, 0, c->la->bh, 0, c->la->bh);
}

static void ct_prop_band(void *ctx, int i)
{
    const struct ct_prop *c = ctx;
    const n265_la *la = c->la;
    int ty0 = i * c->band;
    int ty1 = ty0 + c->band < la->bh ? ty0 + c->band : la->bh;
    for (int by = 0; by < la->bh; by++) {
        int reach = 0;
        for (int t = 0; t < c->nt && !reach; t++) {
            int lo = c->f->mvy_lo[by] * c->tgt[t].scale;
            int hi = c->f->mvy_hi[by] * c->tgt[t].scale;
            if (c->tgt[t].neg) {
                int x = lo;
                lo = -hi;
                hi = -x;
            }
            reach = ((by * 8 + hi) >> 3) + 1 >= ty0 &&
                    (by * 8 + lo) >> 3 < ty1;
        }
        if (reach)
            ct_prop_rows(c, by, by + 1, ty0, ty1);
    }
}

/* n265_la_cutree's two reads of the propagated field, split over the
 * pool: each item writes its own slots and the sums stay serial */
struct ct_level {
    const n265_la *la;
    int lo;
    double *acc;
    int *cnt;
};

static void ct_level_frame(void *ctx, int i)
{
    const struct ct_level *c = ctx;
    const la_frame *g = &c->la->f[c->lo + i];
    int nb = c->la->bw * c->la->bh;
    double acc = 0;
    int cnt = 0;
    for (int b = 0; b < nb; b++) {
        if (g->prop[b] == 0.0f)
            continue;
        float ic = g->icost[b];
        acc += log2f((ic + g->prop[b]) / ic);
        cnt++;
    }
    c->acc[i] = acc;
    c->cnt[i] = cnt;
}

struct ct_field {
    const n265_la *la;
    const la_frame *f;
    float *raw;
    float str;
    int blocks_per_ctb, ctb_cols;
};

static void ct_field_row(void *ctx, int cy)
{
    const struct ct_field *c = ctx;
    const n265_la *la = c->la;
    const la_frame *f = c->f;
    int blocks_per_ctb = c->blocks_per_ctb;
    for (int cx = 0; cx < c->ctb_cols; cx++) {
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
        float v = cnt ? -c->str * acc / cnt : 0;
        c->raw[cy * c->ctb_cols + cx] = v;
    }
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
        struct ct_prop cp = { la, f, { { 0 } }, nt, la->bh };
        for (int t = 0; t < nt; t++) {
            cp.tgt[t].idx = tgt[t].idx;
            cp.tgt[t].w = tgt[t].w;
            cp.tgt[t].neg = tgt[t].neg;
            cp.tgt[t].scale = tgt[t].scale;
        }
        int bands = n265_par_width(e) / 2 > 2 ? n265_par_width(e) / 2 : 2;
        cp.band = (la->bh + bands - 1) / bands;
        n265_par_for_whole(e, PAR_CT_PROP, (la->bh + cp.band - 1) / cp.band,
                           ct_prop_band, &cp, ct_prop_all);
    }

    /* offsets for frame i: -strength * log2((intra + prop) / intra),
     * averaged per CTU and ZERO-MEANED across the frame (without rate
     * control an un-centered map is just an operating-point shift plus
     * delta bits), then clamped */
    la_frame *f = &la->f[i];
    int ctb = 1 << e->ctb_log2;
    int blocks_per_ctb = ctb / 16;      /* lowres 8 = full 16 */
    int nctu = e->ctb_cols * e->ctb_rows;
    /* built in the frame's own per-CTU map, which is sized for the picture:
     * a fixed stack array sized for 3840x2160 at CTB 32 overran on any
     * larger picture (4096x2160 is 8704 CTUs) and aborted. Nothing reads
     * the map before the centering below rewrites it in place. */
    float *raw = fw->ctu_qp_raw;
    float mean = 0;
    /* --mcstf's coupling (phase 14 item 31): a filtered picture carries
     * less noise into every picture that predicts from it, so what it is
     * worth to them is more than its lowres costs say, and a scene-cut
     * anchor is the root of everything after it. Both take a stronger
     * field. With the filter off `str` is cutree_str exactly. */
    float str = e->cutree_str;
    if (e->mcstf) {
        if (f->mcstf_filt)
            str *= e->mcstf_ctf;
        else if (n265_la_scenecut(e, i))
            str *= e->mcstf_ctsc;
    }
    struct ct_field cf = { la, f, raw, str, blocks_per_ctb, e->ctb_cols };
    n265_par_for(e, PAR_CT_FIELD, e->ctb_rows, ct_field_row, &cf);
    for (int k = 0; k < nctu; k++)
        mean += raw[k];
    mean /= nctu;
    float field_mean = mean;    /* the frame term reads this, before centering eats it */
    /* CRF leaves the field un-centered so the cross-frame differential
     * survives, and a fixed shift in the base nets "the" mean. There is no
     * one mean: a NON-REFERENCED frame receives no propagation at all, so
     * its field is identically zero, where a referenced frame's mean is
     * strongly negative and content-keyed. The difference lands on top of
     * the pyramid QP cascade as a second, uncalibrated cascade.
     * N265_CT_CENTRE 1 centers every frame's field on its own mean under
     * CRF as well (the CQP form), leaving the cascade the only frame-type
     * term; that removes the step and the cross-frame differential with it.
     * 2 removes only the step: it nets each PROPAGATED-INTO frame against
     * the mean over the frames in the lookahead window that receive
     * propagation at all, so what survives is the frame's deviation from
     * the window's own level, and a frame with no propagation keeps its
     * zero. 0 is the pre-2026-09-08 behavior. */
    if (e->crf_on) {
        static int centre = -1;
        if (centre < 0) {
            const char *v = n265_env("N265_CT_CENTRE");
            centre = v ? atoi(v) : 0;
        }
        if (!centre) {
            mean = 0;       /* un-centered: the shift in the base nets the mean */
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
     * multi-shot gate refuses that. k_f therefore re-weights the deviation and
     * leaves the level alone. N265_CT_FRAME_LEVEL=0 is the whole-mean form,
     * which is the arm that gate refused. */
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
            double accs[LA_MAX];
            int cnts[LA_MAX];
            struct ct_level lv = { la, lo, accs, cnts };
            n265_par_for(e, PAR_CT_LEVEL, hi - lo, ct_level_frame, &lv);
            for (int k = lo; k < hi; k++) {
                double acc = accs[k - lo];
                int cnt = cnts[k - lo];
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
     * left un-centered, so the per-CTU offsets below already carry k_b of the
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
        raw[k] = v;
        double qv = qpw * v;
        int off = (int)(qv > 0 ? qv + 0.5 : qv - 0.5);
        fw->ctu_qp_off[k] = (int8_t)(off < -12 ? -12 : off > 12 ? 12 : off);
    }
    /* N265_CT_LAMBDA reads the SAME field as a per-CTU lambda multiplier,
     * at strength 1 and centered on the frame so it is independent of what
     * the QP path above did with it. dev < 0 is a CTU the field wants at a
     * lower QP; it gets the lambda that QP step would have carried, on the
     * same 2^(d/3) slope, bounded by the offset map's own +-12. Frozen
     * here, before the frame is scheduled, and read-only after it is. */
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
