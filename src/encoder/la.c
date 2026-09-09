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
    for (int i = 0; i < LA_MAX; i++) {
        la_frame *f = &la->f[i];
        for (int c = 0; c < 3; c++) {
            int w = c ? e->pad_w / 2 : e->pad_w;
            int h = c ? e->pad_h / 2 : e->pad_h;
            f->src[c] = malloc((size_t)w * h);
            if (!f->src[c])
                return -1;
        }
        if (i == 0) {
            la->anchor_lowres = malloc((size_t)la->lw * la->lh);
            if (!la->anchor_lowres)
                return -1;
            la->anchor_valid = 0;
            size_t mvb = (size_t)la->bw * la->bh * 2 * sizeof(int16_t);
            for (int k = 0; k < 8; k++)
                if (!(la->pc_mv[k] = malloc(mvb)))
                    return -1;
            if (!(la->pc_bmv = malloc(mvb)))
                return -1;
        }
        f->lowres = malloc((size_t)la->lw * la->lh);
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
    for (int i = 0; i < LA_MAX; i++) {
        la_frame *f = &e->la.f[i];
        for (int c = 0; c < 3; c++)
            free(f->src[c]);
        free(f->lowres);
        free(f->icost);
        free(f->pcost);
        free(f->mv);
        free(f->prop);
    }
    free(e->la.anchor_lowres);
    e->la.anchor_lowres = NULL;
    for (int k = 0; k < 8; k++) {
        free(e->la.pc_mv[k]);
        e->la.pc_mv[k] = NULL;
    }
    free(e->la.pc_bmv);
    e->la.pc_bmv = NULL;
}

static uint32_t sad8(const uint8_t *a, int as, const uint8_t *b, int bs)
{
    uint32_t s = 0;
    for (int y = 0; y < 8; y++)
        for (int x = 0; x < 8; x++) {
            int d = a[y * as + x] - b[y * bs + x];
            s += (uint32_t)(d < 0 ? -d : d);
        }
    return s;
}

void n265_la_push(struct n265_encoder *e, const uint8_t *const plane[3],
                  const int stride[3])
{
    n265_la *la = &e->la;
    la_frame *f = &la->f[la->count];
    la_frame *prev = la->count > 0 ? &la->f[la->count - 1] : NULL;
    f->valid = 1;
    f->display_idx = e->in_display_idx;

    for (int c = 0; c < 3; c++) {
        int w = c ? e->param.width / 2 : e->param.width;
        int h = c ? e->param.height / 2 : e->param.height;
        int dst_stride = c ? e->pad_w / 2 : e->pad_w;
        for (int y = 0; y < h; y++)
            memcpy(f->src[c] + y * dst_stride, plane[c] + y * stride[c],
                   (size_t)w);
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
            const uint8_t *s = f->src[0];
            int ps = e->pad_w;
            f->lowres[y * lw + x] = (uint8_t)((s[sy * ps + sx] +
                s[sy * ps + w1] + s[h1 * ps + sx] + s[h1 * ps + w1] + 2) >>
                2);
        }

    /* per-8x8 costs */
    f->icost_sum = f->pcost_sum = f->mvabs_sum = 0;
    for (int by = 0; by < la->bh; by++)
        for (int bx = 0; bx < la->bw; bx++) {
            int b = by * la->bw + bx;
            int x0 = bx * 8, y0 = by * 8;
            if (x0 + 8 > lw)
                x0 = lw - 8;
            if (y0 + 8 > lh)
                y0 = lh - 8;
            const uint8_t *blk = f->lowres + y0 * lw + x0;
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
     * byte-identical under either value. */
    static int hold = -1;
    if (hold < 0) { const char *v = getenv("N265_SC_HOLD"); hold = v ? atoi(v) : 1; }
    int nblk = la->bw * la->bh;
    int held = hold && f->pcost_sum < (uint64_t)nblk * 17;
    if (!held)
        la->sc_ema = la->sc_ema == 0.0f
                         ? f->sc_ratio
                         : 0.5f * la->sc_ema + 0.5f * f->sc_ratio;
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
    if (getenv("N265_SC_TRACE"))
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
    memcpy(la->anchor_lowres, la->f[i].lowres, (size_t)la->lw * la->lh);
    la->anchor_valid = 1;
}

static uint32_t blk_sad_at(const n265_la *la, const uint8_t *cur,
                           const uint8_t *ref, int x0, int y0, int mx, int my)
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
 * these numbers are comparable with each other and with pcost_sum. */
static uint64_t pair_cost(const n265_la *la, const uint8_t *cur,
                          const uint8_t *ref, int lo, int hi, int sign,
                          int16_t *mvout)
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
            sum += (uint64_t)best + 16;
        }
    return sum;
}

/* Cost of the same plane predicted BIDIRECTIONALLY, at the two searches'
 * own winning vectors: the leaves of a mini-GOP are coded that way, and a
 * unidirectional cost is the wrong price for them. No search of its own -
 * one pass over the blocks averaging the two predictions. */
static uint64_t bi_cost(const n265_la *la, const uint8_t *cur,
                        const uint8_t *ra, const int16_t *mva,
                        const uint8_t *rb, const int16_t *mvb)
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
            const uint8_t *c = cur + y0 * la->lw + x0;
            const uint8_t *pa = ra + ay * la->lw + ax;
            const uint8_t *pb = rb + by2 * la->lw + bx2;
            uint32_t s = 0;
            for (int y = 0; y < 8; y++)
                for (int x = 0; x < 8; x++) {
                    int p = (pa[y * la->lw + x] + pb[y * la->lw + x] + 1) >> 1;
                    int d = c[y * la->lw + x] - p;
                    s += (uint32_t)(d < 0 ? -d : d);
                }
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
    int trace = getenv("N265_BADAPT_TRACE") != NULL;
    int A = 0;
    for (int i = 1; i <= maxa; i++) {
        A = i;
        uint64_t far = pair_cost(la, la->f[i].lowres, la->anchor_lowres,
                                 0, i, 1, NULL);
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
    if (!la->anchor_valid || maxa >= la->count || maxa > 7)
        return maxa;
    uint64_t fwd[8];
    for (int k = 0; k <= maxa; k++)
        fwd[k] = pair_cost(la, la->f[k].lowres, la->anchor_lowres, 0, k, 1,
                           la->pc_mv[k]);
    int trace = getenv("N265_BADAPT_TRACE") != NULL;
    int best_a = 1;
    double best_c = 0;
    for (int A = 1; A <= maxa; A++) {
        uint64_t c = fwd[A];
        for (int k = 0; k < A; k++) {
            uint64_t bwd = pair_cost(la, la->f[k].lowres, la->f[A].lowres,
                                     k + 1, A, -1, la->pc_bmv);
            uint64_t bi = bi_cost(la, la->f[k].lowres, la->anchor_lowres,
                                  la->pc_mv[k], la->f[A].lowres, la->pc_bmv);
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

void n265_la_cutree(struct n265_encoder *e, int i)
{
    n265_la *la = &e->la;
    int nb = la->bw * la->bh;
    for (int k = 0; k < la->count; k++)
        memset(la->f[k].prop, 0, (size_t)nb * sizeof(float));

    /* propagate along the real reference edges in reverse encode order:
     * anchors (even GOP parity) push into the previous anchor (mv x2,
     * linear-motion assumption), B frames push half into each of their
     * two anchors (-mv for the future one). Non-reference B frames never
     * RECEIVE propagation. */
    int order[LA_MAX];
    int m = 0;
    for (int k = 0; k < la->count; k++) {
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
            if (k + 1 < la->count) {
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
            const char *v = getenv("N265_CT_CENTRE");
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
                for (int k = 0; k < la->count; k++) {
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
    for (int k = 0; k < nctu; k++) {
        float v = raw[k] - mean;
        e->ctu_qp_raw[k] = v;
        int off = (int)(v > 0 ? v + 0.5f : v - 0.5f);
        e->ctu_qp_off[k] = (int8_t)(off < -12 ? -12 : off > 12 ? 12 : off);
    }
    if (getenv("N265_CT_TRACE")) {
        int nz = 0, mn = 0;
        double fm = 0;
        for (int k = 0; k < e->ctb_cols * e->ctb_rows; k++) {
            nz += e->ctu_qp_off[k] != 0;
            fm += e->ctu_qp_raw[k];
            if (e->ctu_qp_off[k] < mn)
                mn = e->ctu_qp_off[k];
        }
        fprintf(stderr, "[ct d%d cnt%d nz%d min%d raw%.2f fieldmean%.2f]",
                f->display_idx, la->count, nz, mn, mean, fm / nctu);
    }
}

void n265_la_pop(struct n265_encoder *e)
{
    n265_la *la = &e->la;
    la_frame tmp = la->f[0];
    memmove(&la->f[0], &la->f[1],
            (size_t)(la->count - 1) * sizeof(la_frame));
    la->f[la->count - 1] = tmp;     /* recycle the buffers */
    la->f[la->count - 1].valid = 0;
    la->count--;
}
