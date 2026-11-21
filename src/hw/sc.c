/*
 * sc.c - the hardware mode's scene-cut (sc.h).
 *
 * The lookahead's lowres costs and its cut rule, on one frame against the
 * one before it: a half-size plane, an 8x8 intra cost (DC, vertical or
 * horizontal) and an 8x8 inter cost from a small diamond seeded by the
 * neighbours' vectors. A frame cuts when its inter-to-intra ratio is high
 * and spikes over its running level, or when the share inter loses does;
 * either must also be a jump over the three frames before it. There is no
 * frame of lookahead, so no flash guard.
 * Copyright (c) 2026, the yah265 authors. GPL-2.0-or-later; see LICENSE.
 */

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "common/pixel.h"
#include "hw/sc.h"

/* the lookahead's defaults */
#define SC_BAR        0.75
#define SC_SPIKE      1.8
#define SC_FAILW      0.40
#define SC_FAILW_SPK  1.6
#define SC_JUMP       0.10

struct n265_hwsc {
    int w, h, lw, lh, bw, bh, keyint;
    uint8_t *lo[2];
    int16_t *mv[2];
    int cur, have_prev, since_key;
    double ema, failw_ema;
    double rhist[3];
    int rn;
};

struct n265_hwsc *n265_hwsc_new(int width, int height, int keyint)
{
    struct n265_hwsc *s = calloc(1, sizeof *s);
    if (!s)
        return NULL;
    s->w = width;
    s->h = height;
    s->keyint = keyint;
    int pw = (width + 7) & ~7, ph = (height + 7) & ~7;
    s->lw = pw / 2 < 8 ? 8 : pw / 2;
    s->lh = ph / 2 < 8 ? 8 : ph / 2;
    s->bw = (s->lw + 7) / 8;
    s->bh = (s->lh + 7) / 8;
    for (int i = 0; i < 2; i++) {
        s->lo[i] = malloc((size_t)s->lw * s->lh);
        s->mv[i] = calloc((size_t)s->bw * s->bh * 2, sizeof(int16_t));
        if (!s->lo[i] || !s->mv[i]) {
            n265_hwsc_free(s);
            return NULL;
        }
    }
    return s;
}

void n265_hwsc_free(struct n265_hwsc *s)
{
    if (!s)
        return;
    for (int i = 0; i < 2; i++) {
        free(s->lo[i]);
        free(s->mv[i]);
    }
    free(s);
}

static void downscale(const struct n265_hwsc *s, uint8_t *lo,
                      const pixel *src, int stride)
{
    for (int y = 0; y < s->lh; y++)
        for (int x = 0; x < s->lw; x++) {
            int sy = 2 * y < s->h ? 2 * y : s->h - 1;
            int sx = 2 * x < s->w ? 2 * x : s->w - 1;
            int h1 = sy + 1 < s->h ? sy + 1 : sy;
            int w1 = sx + 1 < s->w ? sx + 1 : sx;
            int v = src[(size_t)sy * stride + sx] + src[(size_t)sy * stride + w1] +
                    src[(size_t)h1 * stride + sx] + src[(size_t)h1 * stride + w1];
            lo[(size_t)y * s->lw + x] =
                (uint8_t)((v + (2 << BIT_DEPTH_SHIFT)) >> (2 + BIT_DEPTH_SHIFT));
        }
}

static uint32_t sad8(const uint8_t *a, const uint8_t *b, int st)
{
    uint32_t sad = 0;
    for (int y = 0; y < 8; y++, a += st, b += st)
        for (int x = 0; x < 8; x++) {
            int d = a[x] - b[x];
            sad += (uint32_t)(d < 0 ? -d : d);
        }
    return sad;
}

static uint32_t intra8(const uint8_t *blk, int lw, int x0, int y0)
{
    uint32_t sv = UINT32_MAX, sh = UINT32_MAX, sd = 0;
    int dcs = 0, dcn = 0;
    if (y0 > 0) {
        sv = 0;
        for (int y = 0; y < 8; y++)
            for (int x = 0; x < 8; x++) {
                int d = blk[y * lw + x] - blk[-lw + x];
                sv += (uint32_t)(d < 0 ? -d : d);
            }
        for (int x = 0; x < 8; x++)
            dcs += blk[-lw + x];
        dcn += 8;
    }
    if (x0 > 0) {
        sh = 0;
        for (int y = 0; y < 8; y++)
            for (int x = 0; x < 8; x++) {
                int d = blk[y * lw + x] - blk[y * lw - 1];
                sh += (uint32_t)(d < 0 ? -d : d);
            }
        for (int y = 0; y < 8; y++)
            dcs += blk[y * lw - 1];
        dcn += 8;
    }
    if (!dcn) {
        for (int y = 0; y < 8; y++)
            for (int x = 0; x < 8; x++)
                dcs += blk[y * lw + x];
        dcn = 64;
    }
    int dc = (dcs + dcn / 2) / dcn;
    for (int y = 0; y < 8; y++)
        for (int x = 0; x < 8; x++) {
            int d = blk[y * lw + x] - dc;
            sd += (uint32_t)(d < 0 ? -d : d);
        }
    uint32_t ic = sd < sv ? sd : sv;
    return (ic < sh ? ic : sh) + 64;
}

static int med3(int a, int b, int c)
{
    return a > b ? (b > c ? b : a > c ? c : a) : (a > c ? a : b > c ? c : b);
}

int n265_hwsc_push(struct n265_hwsc *s, const n265_picture *pic)
{
    if (!s || !pic)
        return 0;
    int cur = s->cur, prv = cur ^ 1;
    downscale(s, s->lo[cur], pic->plane[0], pic->stride[0]);
    int force = 0;
    if (s->have_prev) {
        const int lw = s->lw, lh = s->lh;
        const uint8_t *lo = s->lo[cur], *pl = s->lo[prv];
        int16_t *mv = s->mv[cur];
        const int16_t *pmv = s->mv[prv];
        uint64_t isum = 0, psum = 0, ifail = 0;
        for (int by = 0; by < s->bh; by++)
            for (int bx = 0; bx < s->bw; bx++) {
                int b = by * s->bw + bx;
                int x0 = bx * 8 < lw - 8 ? bx * 8 : lw - 8;
                int y0 = by * 8 < lh - 8 ? by * 8 : lh - 8;
                const uint8_t *blk = lo + (size_t)y0 * lw + x0;
                uint32_t ic = intra8(blk, lw, x0, y0);
                int mx = 0, my = 0;
                uint32_t best = sad8(blk, pl + (size_t)y0 * lw + x0, lw);
                int cand[5][2], nc = 0;
                int hl = bx > 0, ht = by > 0, htr = by > 0 && bx + 1 < s->bw;
                if (hl) { cand[nc][0] = mv[2 * (b - 1)]; cand[nc][1] = mv[2 * (b - 1) + 1]; nc++; }
                if (ht) { cand[nc][0] = mv[2 * (b - s->bw)]; cand[nc][1] = mv[2 * (b - s->bw) + 1]; nc++; }
                if (htr) { cand[nc][0] = mv[2 * (b - s->bw + 1)]; cand[nc][1] = mv[2 * (b - s->bw + 1) + 1]; nc++; }
                if (hl && ht && htr) {
                    cand[nc][0] = med3(cand[0][0], cand[1][0], cand[2][0]);
                    cand[nc][1] = med3(cand[0][1], cand[1][1], cand[2][1]);
                    nc++;
                }
                cand[nc][0] = pmv[2 * b];
                cand[nc][1] = pmv[2 * b + 1];
                nc++;
                for (int k = 0; k < nc; k++) {
                    int cx = cand[k][0], cy = cand[k][1];
                    if ((!cx && !cy) || x0 + cx < 0 || y0 + cy < 0 ||
                        x0 + cx + 8 > lw || y0 + cy + 8 > lh)
                        continue;
                    uint32_t sd = sad8(blk, pl + (size_t)(y0 + cy) * lw + x0 + cx, lw);
                    if (sd < best) {
                        best = sd;
                        mx = cx;
                        my = cy;
                    }
                }
                for (int it = 0; it < 8; it++) {
                    static const int d[4][2] = { { 1, 0 }, { -1, 0 }, { 0, 1 }, { 0, -1 } };
                    int bmx = mx, bmy = my;
                    for (int k = 0; k < 4; k++) {
                        int tx = mx + d[k][0], ty = my + d[k][1];
                        if (x0 + tx < 0 || y0 + ty < 0 || x0 + tx + 8 > lw ||
                            y0 + ty + 8 > lh)
                            continue;
                        uint32_t sd = sad8(blk, pl + (size_t)(y0 + ty) * lw + x0 + tx, lw);
                        if (sd < best) {
                            best = sd;
                            bmx = tx;
                            bmy = ty;
                        }
                    }
                    if (bmx == mx && bmy == my)
                        break;
                    mx = bmx;
                    my = bmy;
                }
                uint32_t pc = best + 16;
                if (ic > 65535) ic = 65535;
                if (pc > 65535) pc = 65535;
                mv[2 * b] = (int16_t)mx;
                mv[2 * b + 1] = (int16_t)my;
                isum += ic;
                psum += pc;
                if (pc >= ic)
                    ifail += ic;
            }
        double r = (double)psum / (double)(isum ? isum : 1);
        double fw = isum ? (double)ifail / (double)isum : 0.0;
        int cut = r >= SC_BAR && (s->ema == 0.0 || r >= SC_SPIKE * s->ema);
        if (!cut && fw >= SC_FAILW &&
            (s->failw_ema == 0.0 || fw >= SC_FAILW_SPK * s->failw_ema))
            cut = 1;
        if (cut && s->rn > 0) {
            double m = s->rhist[0];
            for (int k = 1; k < s->rn; k++)
                if (s->rhist[k] > m)
                    m = s->rhist[k];
            cut = r - m >= SC_JUMP || 1.0 - m >= 2.0 * (1.0 - r);
        }
        if (isum > 0) {
            s->rhist[2] = s->rhist[1];
            s->rhist[1] = s->rhist[0];
            s->rhist[0] = r;
            if (s->rn < 3)
                s->rn++;
        }
        /* a repeated frame is not a measurement */
        if (psum >= (uint64_t)s->bw * s->bh * 17) {
            s->ema = s->ema == 0.0 ? r : 0.5 * s->ema + 0.5 * r;
            s->failw_ema = s->failw_ema == 0.0 ? fw : 0.5 * s->failw_ema + 0.5 * fw;
        }
        s->since_key++;
        force = cut || (s->keyint > 0 && s->since_key >= s->keyint);
    } else {
        memset(s->mv[cur], 0, (size_t)s->bw * s->bh * 2 * sizeof(int16_t));
    }
    if (force || !s->have_prev)
        s->since_key = 0;
    s->have_prev = 1;
    s->cur = prv;
    return force;
}
