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
            float v = cnt ? -2.0f * acc / cnt : 0;
            raw[cy * e->ctb_cols + cx] = v;
            mean += v;
        }
    mean /= nctu;
    if (e->crf_on)
        mean = 0;           /* CRF: un-centred, the shift in the base nets the mean */
    for (int k = 0; k < nctu; k++) {
        float v = raw[k] - mean;
        int off = (int)(v > 0 ? v + 0.5f : v - 0.5f);
        e->ctu_qp_off[k] = (int8_t)(off < -12 ? -12 : off > 12 ? 12 : off);
    }
    if (getenv("N265_CT_TRACE")) {
        int nz = 0, mn = 0;
        for (int k = 0; k < e->ctb_cols * e->ctb_rows; k++) {
            nz += e->ctu_qp_off[k] != 0;
            if (e->ctu_qp_off[k] < mn)
                mn = e->ctu_qp_off[k];
        }
        fprintf(stderr, "[ct d%d cnt%d nz%d min%d]", f->display_idx,
                la->count, nz, mn);
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
