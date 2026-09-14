/*
 * --mcstf. mcstf.h says what the filter is and where it sits; this file
 * is the queue, the motion search and the weighted average.
 *
 * Copyright (c) 2026, the yah265 authors. GPL-2.0-or-later; see LICENSE.
 */

#include <math.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "encoder/encoder.h"
#include "encoder/mcstf.h"
#include "encoder/preset.h"
#include "encoder/satd.h"

/* ---- knobs ---------------------------------------------------------- */

/* N265_MCSTF_SIGMA: the photometric width of the per-pixel weight, in
 * 8-BIT luma levels, Q8. A neighbour pixel that differs from the original
 * by sigma keeps 61% of its weight and one that differs by three sigma
 * keeps 1%, so this is the number that says "a difference this big is
 * noise; a difference bigger than this is the picture moving".
 *
 * 6.0. The corpus's own grain sits between 0.9 and 4.3 luma levels of
 * standard deviation (docs/phase12.md's estimator table over park_joy,
 * ducks, crowd_run, in_to_tree), so a width of six covers the noise on
 * every clip this filter is for with a margin, and at three sigma - 18
 * levels - anything the motion search got wrong is already worth a
 * hundredth of its weight. It is a knob because it is the one constant
 * here that a band could move and nothing else in the tree fixes. */
int n265_mcstf_sigma_q8(void)
{
    static int v = -1;
    if (v < 0) {
        const char *s = n265_env("N265_MCSTF_SIGMA");
        double d = s ? atof(s) : 6.0;
        if (d < 0.25)
            d = 0.25;
        if (d > 64.0)
            d = 64.0;
        v = (int)(d * 256.0 + 0.5);
    }
    return v;
}

/* N265_MCSTF_CLEAN: --selective-mcstf's bar, in 8-bit luma levels, Q8.
 * Below this reading the picture is called clean and is not filtered.
 *
 * 1.5, AND IT IS PLACED BY THE BAND. 0.75 shipped first, placed in a gap
 * the corpus has, and the band then said the gap was the wrong argument:
 * the census separates clips that have capture noise from clips that do
 * not, and what the filter needs to know is something narrower - where
 * there is enough noise for a temporal average to beat what the
 * quantiser was going to do anyway. Those are not the same line. At 0.75
 * the filter arms on foreman_cif and costs it 1.35 BD points; at 1.5 it
 * does not arm there at all, and on the five-clip read (docs/phase14.md,
 * item 6) the arm becomes three clips better, two byte-identical and
 * none worse, on VMAF-NEG and on PSNR-Y alike.
 *
 * The census is kept below because it is still the sanity check that the
 * CLEAN end is unambiguous, and because it says where the bar is not: at
 * 1.5, touchdown_1080p at 1.62 is the nearest clip above and is 8%
 * away, which is a thin margin and is worth re-reading if a clip like it
 * ever lands in the corpus.
 *
 * The reading (mcstf_noise) over all fifteen clips, three pictures each,
 * 8-bit:
 *
 *   sintel_720p     0.00  sita_flat_720p  0.00  sita_toon_720p  0.56
 *   akiyo_cif       0.62  stefan_cif      0.81-0.94
 *   park_joy_720p   1.06-1.12  foreman_cif  1.19-1.38  touchdown  1.62
 *   crowd_run_1080p 2.25-2.31  in_to_tree_720p 2.31  tempete_cif 2.56-2.75
 *   bus_cif         2.56-2.88  ducks_720p 2.88-2.94  coastguard 3.25-3.38
 *   mobile_cif      4.62-4.88
 *
 * At the shipped 1.5 the bar disarms the four animation and
 * head-and-shoulders clips outright plus stefan, park_joy and foreman -
 * seven of fifteen - and arms the eight that read above it, the two the
 * band shows a gain on (ducks 2.9, crowd_run 2.3) among them. The
 * reading is per PICTURE and not per clip, so a clip near the bar is
 * partly armed: park_joy reads 1.06-1.12 on these three and is still
 * worth -0.08 at 1.5 over 120 frames, which is the bar doing exactly
 * what a per-picture gate should.
 *
 * Read it honestly: on a clip that is textured EVERYWHERE, a percentile
 * of a high-pass is a detail floor and not a noise floor, and mobile_cif
 * at 4.8 is the calendar and not the sensor. What the gate is for is the
 * other end - a source with no capture noise in it at all - and there it
 * is unambiguous: the two clips that read exactly zero have none. */
int n265_mcstf_clean_q8(void)
{
    static int v = -1;
    if (v < 0) {
        const char *s = n265_env("N265_MCSTF_CLEAN");
        double d = s ? atof(s) : 1.5;
        if (d < 0.0)
            d = 0.0;
        if (d > 64.0)
            d = 64.0;
        v = (int)(d * 256.0 + 0.5);
    }
    return v;
}

/* N265_MCSTF_PERIOD: the cadence, in display indices. mcstf.h says why
 * it is 8 and not 1. */
int n265_mcstf_period(void)
{
    static int v = -1;
    if (v < 0) {
        const char *s = n265_env("N265_MCSTF_PERIOD");
        int d = s ? atoi(s) : N265_MCSTF_PERIOD;
        v = d < 1 ? 1 : d > 64 ? 64 : d;
    }
    return v;
}

int n265_mcstf_trace(void)
{
    static int v = -1;
    if (v < 0)
        v = n265_env("N265_MCSTF_TRACE") != NULL;
    return v;
}

/* ---- the queue ------------------------------------------------------ */

typedef struct mc_pic {
    pixel *buf[3];          /* the padded allocation                     */
    pixel *org[3];          /* sample (0, 0) of the displayed picture    */
    int stride[3];
    pixel *coarse;          /* padded 1/SUB luma, for the first ME stage */
    int cstride;
    int display_idx;
    int valid;
} mc_pic;

struct n265_mcstf {
    int w, h, csx, csy;
    int range, selective;
    int cw, ch;             /* coarse luma size                          */
    int cap;                /* 2 * range + 1                             */
    mc_pic *p;              /* the ring, in DISPLAY order p[0..n-1]      */
    int n;                  /* pictures held                             */
    int next_idx;           /* display index of the next push            */
    /* The ring is a SLIDING WINDOW and not a conveyor: a picture that
     * has been handed out stays in it, because it is the next picture's
     * past neighbour. `out_pos` is where in the window the next output
     * sits, and the window drops its oldest entry once it is `cap` long,
     * which is what holds out_pos at `range` in steady state. */
    int out_pos;
    /* the filtered picture, handed out by n265_mcstf_pop */
    pixel *out_buf[3];
    pixel *out[3];
    int out_stride[3];
    /* per-block motion, one entry per (neighbour, block) */
    int16_t *mv;
    uint16_t *berr;         /* the block's mean absolute error, Q4       */
    int bw, bh;
    int last_filtered, last_noise_q8;
    uint16_t gauss[1 << 9]; /* the photometric weight, Q8, by |d|        */
};

/* The temporal strengths, Q8, by distance in pictures. The further
 * away in time, the less a neighbour is
 * about this picture, and by the fourth the filter is mostly deciding
 * not to trust it. The ORIGINAL's own weight is 256, so at range 2 a
 * pixel whose four neighbours all agree perfectly is the mean of five
 * samples with the middle one counting hardest - noise down by a factor
 * near two, which is what the filter is for. */
static const int mc_tstr[N265_MCSTF_MAX_RANGE + 1] = { 256, 218, 154, 115, 90 };
/* The block-error half-life, in luma levels of mean absolute error. A
 * block that matched to this much keeps half its weight. 6 is the same
 * number the photometric width is, and for the same reason: past it, the
 * disagreement is the picture and not the noise. */
#define MC_BERR_HALF 6

static int plane_w(const n265_mcstf *m, int c)
{
    return c ? m->w >> m->csx : m->w;
}

static int plane_h(const n265_mcstf *m, int c)
{
    return c ? m->h >> m->csy : m->h;
}

static void pad_plane(pixel *org, int stride, int w, int h, int pad)
{
    for (int y = 0; y < h; y++) {
        pixel *r = org + (size_t)y * stride;
        for (int x = 1; x <= pad; x++) {
            r[-x] = r[0];
            r[w - 1 + x] = r[w - 1];
        }
    }
    for (int y = 1; y <= pad; y++) {
        memcpy(org - (ptrdiff_t)y * stride - pad, org - pad,
               (size_t)(w + 2 * pad) * sizeof(pixel));
        memcpy(org + (size_t)(h - 1 + y) * stride - pad,
               org + (size_t)(h - 1) * stride - pad,
               (size_t)(w + 2 * pad) * sizeof(pixel));
    }
}

n265_mcstf *n265_mcstf_open(int w, int h, int csx, int csy, int range,
                            int selective)
{
    const int pad = N265_MCSTF_PAD;
    n265_mcstf *m = calloc(1, sizeof(*m));
    if (!m)
        return NULL;
    m->w = w;
    m->h = h;
    m->csx = csx;
    m->csy = csy;
    m->range = range;
    m->selective = selective;
    m->cap = 2 * range + 1;
    m->cw = (w + N265_MCSTF_SUB - 1) / N265_MCSTF_SUB;
    m->ch = (h + N265_MCSTF_SUB - 1) / N265_MCSTF_SUB;
    m->bw = (w + N265_MCSTF_BLK - 1) / N265_MCSTF_BLK;
    m->bh = (h + N265_MCSTF_BLK - 1) / N265_MCSTF_BLK;
    m->p = calloc((size_t)m->cap, sizeof(*m->p));
    m->mv = calloc((size_t)m->bw * m->bh * 2 * m->cap, sizeof(*m->mv));
    m->berr = calloc((size_t)m->bw * m->bh * m->cap, sizeof(*m->berr));
    if (!m->p || !m->mv || !m->berr) {
        n265_mcstf_close(m);
        return NULL;
    }
    for (int i = 0; i < m->cap; i++) {
        for (int c = 0; c < 3; c++) {
            int pw = plane_w(m, c), ph = plane_h(m, c);
            int st = pw + 2 * pad;
            m->p[i].stride[c] = st;
            m->p[i].buf[c] = malloc((size_t)st * (ph + 2 * pad) *
                                    sizeof(pixel));
            if (!m->p[i].buf[c]) {
                n265_mcstf_close(m);
                return NULL;
            }
            m->p[i].org[c] = m->p[i].buf[c] + (size_t)pad * st + pad;
        }
        m->p[i].cstride = m->cw + 2 * pad;
        m->p[i].coarse = malloc((size_t)m->p[i].cstride *
                                (m->ch + 2 * pad) * sizeof(pixel));
        if (!m->p[i].coarse) {
            n265_mcstf_close(m);
            return NULL;
        }
    }
    for (int c = 0; c < 3; c++) {
        int pw = plane_w(m, c), ph = plane_h(m, c);
        m->out_stride[c] = pw + 2 * pad;
        m->out_buf[c] = malloc((size_t)m->out_stride[c] * (ph + 2 * pad) *
                               sizeof(pixel));
        if (!m->out_buf[c]) {
            n265_mcstf_close(m);
            return NULL;
        }
        m->out[c] = m->out_buf[c] + (size_t)pad * m->out_stride[c] + pad;
    }
    /* The photometric weight, tabulated once: 256 * exp(-d^2 / 2 sigma^2)
     * over |d| in the encoder's own sample units, so nothing in the inner
     * loop floats and the filtered picture is the same integer on every
     * machine. A difference past the table's end weighs nothing, which is
     * the same answer the exponential gives to four decimal places. */
    {
        double sigma = n265_mcstf_sigma_q8() / 256.0 * (1 << (BIT_DEPTH - 8));
        double k = -1.0 / (2.0 * sigma * sigma);
        for (int d = 0; d < (1 << 9); d++) {
            double g = 256.0 * exp(k * (double)d * (double)d);
            m->gauss[d] = (uint16_t)(g + 0.5);
        }
    }
    return m;
}

void n265_mcstf_close(n265_mcstf *m)
{
    if (!m)
        return;
    for (int i = 0; m->p && i < m->cap; i++) {
        for (int c = 0; c < 3; c++)
            free(m->p[i].buf[c]);
        free(m->p[i].coarse);
    }
    free(m->p);
    for (int c = 0; c < 3; c++)
        free(m->out_buf[c]);
    free(m->mv);
    free(m->berr);
    free(m);
}

/* the 1/SUB luma the coarse search stage works in: a box mean, which is
 * what a SAD wants - it is a cost proxy and never a picture */
static void build_coarse(n265_mcstf *m, mc_pic *p)
{
    const int pad = N265_MCSTF_PAD, s = N265_MCSTF_SUB;
    pixel *dst = p->coarse + (size_t)pad * p->cstride + pad;
    for (int y = 0; y < m->ch; y++)
        for (int x = 0; x < m->cw; x++) {
            int sy = y * s, sx = x * s;
            unsigned acc = 0;
            for (int j = 0; j < s; j++) {
                int yy = sy + j < m->h ? sy + j : m->h - 1;
                const pixel *r = p->org[0] + (size_t)yy * p->stride[0];
                for (int i = 0; i < s; i++) {
                    int xx = sx + i < m->w ? sx + i : m->w - 1;
                    acc += r[xx];
                }
            }
            dst[(size_t)y * p->cstride + x] =
                (pixel)((acc + s * s / 2) / (s * s));
        }
    pad_plane(dst, p->cstride, m->cw, m->ch, pad);
}

void n265_mcstf_push(n265_mcstf *m, const pixel *const plane[3],
                     const int stride[3])
{
    /* Make room first: the window is `cap` long and the oldest entry is
     * the one no later picture will want as a neighbour. Sliding here
     * rather than after the write is what keeps `m->n` a valid index. */
    if (m->n == m->cap) {
        mc_pic gone = m->p[0];
        for (int i = 0; i < m->cap - 1; i++)
            m->p[i] = m->p[i + 1];
        m->p[m->cap - 1] = gone;
        m->n--;
        m->out_pos--;
    }
    mc_pic *p = &m->p[m->n];
    for (int c = 0; c < 3; c++) {
        int pw = plane_w(m, c), ph = plane_h(m, c);
        for (int y = 0; y < ph; y++)
            memcpy(p->org[c] + (size_t)y * p->stride[c],
                   plane[c] + (size_t)y * stride[c],
                   (size_t)pw * sizeof(pixel));
        pad_plane(p->org[c], p->stride[c], pw, ph, N265_MCSTF_PAD);
    }
    p->display_idx = m->next_idx++;
    p->valid = 1;
    build_coarse(m, p);
    m->n++;
}

int n265_mcstf_ready(n265_mcstf *m, int flush)
{
    /* A picture may come out once it has every FUTURE neighbour it is
     * going to get - `range` of them, or, on a flush, however many the
     * end of the sequence left. The first pictures of an encode come out
     * with no past neighbours and the last with no future ones, which is
     * what any temporal filter's ends look like and what the weights
     * already handle: there is simply one term fewer in the average. */
    if (m->out_pos >= m->n)
        return 0;
    return flush || m->out_pos + m->range < m->n;
}

/* The noise reading --selective-mcstf gates on.
 *
 * Per pixel, |4v - (up + down + left + right)| / 4 - a Laplacian, which
 * is the cheapest high-pass that rejects a linear ramp - and the 25th
 * PERCENTILE over 8x8 blocks of that block's mean, because a percentile
 * is what separates noise from texture in a population where most blocks
 * are both (docs/phase12.md's grain estimator makes the same argument at
 * more length).
 *
 * The reading is the noise standard deviation to within a few percent,
 * and that is an identity rather than a fit: for white noise of standard
 * deviation s the Laplacian has variance 20 s^2, so its own standard
 * deviation is 4.47 s, a quarter of that is 1.118 s, and the mean
 * absolute value of a normal variable is sqrt(2/pi) = 0.798 of its
 * standard deviation. 1.118 * 0.798 = 0.892, so the reading is 0.89 s
 * and the threshold is in units of s to within that. Returns Q8, in
 * 8-bit luma levels whatever the coded depth is.
 *
 * The histogram is in SIXTEENTHS of a level over 0..32, not in 256ths
 * over 0..2: five of the fifteen corpus clips saturated the narrow form
 * outright (mobile, bus, tempete, coastguard and ducks all read exactly
 * 1.996), and a gate whose loud end is a plateau can say which clips are
 * clean but not which are noisy. A sixteenth of a level is still an
 * eighth of the bar. */
static int mcstf_noise(const n265_mcstf *m, const mc_pic *p)
{
    enum { NBIN = 512 };
    int hist[NBIN];
    int nblk = 0;
    memset(hist, 0, sizeof hist);
    for (int by = 0; by + 8 <= m->h; by += 8)
        for (int bx = 0; bx + 8 <= m->w; bx += 8) {
            const pixel *b = p->org[0] + (size_t)by * p->stride[0] + bx;
            int st = p->stride[0];
            unsigned acc = 0;
            for (int y = 0; y < 8; y++)
                for (int x = 0; x < 8; x++) {
                    const pixel *v = b + (size_t)y * st + x;
                    int d = 4 * (int)v[0] - (int)v[-st] - (int)v[st] -
                            (int)v[-1] - (int)v[1];
                    acc += (unsigned)(d < 0 ? -d : d);
                }
            /* mean over the block, /4 for the Laplacian's gain, brought
             * back to 8-bit levels, Q4 */
            unsigned q4 = (acc * 16u / 64u / 4u) >> (BIT_DEPTH - 8);
            hist[q4 < NBIN - 1 ? q4 : NBIN - 1]++;
            nblk++;
        }
    if (!nblk)
        return 0;
    int want = nblk / 4, run = 0;
    for (int i = 0; i < NBIN; i++) {
        run += hist[i];
        if (run > want)
            return i * 16;          /* a Q4 bin index, handed back as Q8 */
    }
    return (NBIN - 1) * 16;
}

/* One block's motion against one neighbour: the coarse stage on the
 * subsampled luma, then a square refinement at full resolution. Integer
 * samples only. A fractional stage would find a better match on real
 * motion, and it is deliberately not here: what this filter does with a
 * poor match is weigh it down, so the cost of not having subpel is that
 * the filter averages LESS on moving content - which is the safe
 * direction, and the one a source filter should fail in. */
static void mc_search(const n265_mcstf *m, const mc_pic *cur,
                      const mc_pic *ref, int bx, int by, int *mvx, int *mvy,
                      unsigned *err)
{
    const int blk = N265_MCSTF_BLK, sub = N265_MCSTF_SUB;
    int x0 = bx * blk, y0 = by * blk;
    int w = blk < m->w - x0 ? blk : m->w - x0;
    int h = blk < m->h - y0 ? blk : m->h - y0;
    int cx = x0 / sub, cy = y0 / sub, cb = blk / sub;
    const pixel *cc = cur->coarse + (size_t)(N265_MCSTF_PAD + cy) *
                      cur->cstride + N265_MCSTF_PAD + cx;
    const pixel *rc = ref->coarse + (size_t)(N265_MCSTF_PAD + cy) *
                      ref->cstride + N265_MCSTF_PAD + cx;
    unsigned best = ~0u;
    int bdx = 0, bdy = 0;

    for (int dy = -N265_MCSTF_COARSE; dy <= N265_MCSTF_COARSE; dy++)
        for (int dx = -N265_MCSTF_COARSE; dx <= N265_MCSTF_COARSE; dx++) {
            const pixel *r = rc + (ptrdiff_t)dy * ref->cstride + dx;
            unsigned s = n265_sad_wh(cc, cur->cstride, r, ref->cstride,
                                     cb, cb);
            /* Ties go to the SHORTER vector, and strictly - which is what
             * makes the answer a function of the pictures and not of the
             * scan order. The zero vector wins every tie it is in, so a
             * still picture is filtered against itself un-displaced. */
            if (s < best || (s == best &&
                             dx * dx + dy * dy < bdx * bdx + bdy * bdy)) {
                best = s;
                bdx = dx;
                bdy = dy;
            }
        }
    bdx *= sub;
    bdy *= sub;

    const pixel *co = cur->org[0] + (size_t)y0 * cur->stride[0] + x0;
    const pixel *ro = ref->org[0] + (size_t)y0 * ref->stride[0] + x0;
    best = ~0u;
    int fdx = bdx, fdy = bdy;
    for (int dy = bdy - N265_MCSTF_FINE; dy <= bdy + N265_MCSTF_FINE; dy++)
        for (int dx = bdx - N265_MCSTF_FINE; dx <= bdx + N265_MCSTF_FINE;
             dx++) {
            const pixel *r = ro + (ptrdiff_t)dy * ref->stride[0] + dx;
            unsigned s = n265_sad_wh(co, cur->stride[0], r, ref->stride[0],
                                     w, h);
            if (s < best || (s == best &&
                             dx * dx + dy * dy < fdx * fdx + fdy * fdy)) {
                best = s;
                fdx = dx;
                fdy = dy;
            }
        }
    *mvx = fdx;
    *mvy = fdy;
    /* the block's mean absolute error, Q4, in the coded depth's units */
    *err = (best * 16u + (unsigned)(w * h) / 2u) / (unsigned)(w * h);
}

/* the block-error weight, Q8: half at MC_BERR_HALF levels of mean
 * absolute error, and it never reaches zero, because a block that
 * matched badly everywhere still has pixels that matched - the
 * photometric weight is what decides those */
static int mc_berr_w(unsigned err_q4)
{
    unsigned half = (unsigned)(MC_BERR_HALF << 4) << (BIT_DEPTH - 8);
    return (int)((256u * half + (half + err_q4) / 2) / (half + err_q4));
}

/* Filter picture `ci` of the ring into m->out. */
static void mcstf_filter(n265_mcstf *m, int ci)
{
    const mc_pic *cur = &m->p[ci];
    int nb = 0;
    int nbi[2 * N265_MCSTF_MAX_RANGE];
    int nbd[2 * N265_MCSTF_MAX_RANGE];

    for (int d = 1; d <= m->range; d++) {
        if (ci - d >= 0) {
            nbi[nb] = ci - d;
            nbd[nb++] = d;
        }
        if (ci + d < m->n) {
            nbi[nb] = ci + d;
            nbd[nb++] = d;
        }
    }

    /* the motion, once per neighbour, on luma; chroma rides the same
     * vectors scaled by the subsampling, which is what a source filter
     * can afford and what the weights make safe */
    for (int k = 0; k < nb; k++) {
        int16_t *mv = m->mv + (size_t)k * m->bw * m->bh * 2;
        uint16_t *be = m->berr + (size_t)k * m->bw * m->bh;
        for (int by = 0; by < m->bh; by++)
            for (int bx = 0; bx < m->bw; bx++) {
                int vx, vy;
                unsigned e;
                mc_search(m, cur, &m->p[nbi[k]], bx, by, &vx, &vy, &e);
                mv[((size_t)by * m->bw + bx) * 2] = (int16_t)vx;
                mv[((size_t)by * m->bw + bx) * 2 + 1] = (int16_t)vy;
                be[(size_t)by * m->bw + bx] =
                    (uint16_t)(e > 65535u ? 65535u : e);
            }
    }

    const int gmax = (1 << 9) - 1;
    for (int c = 0; c < 3; c++) {
        int sx = c ? m->csx : 0, sy = c ? m->csy : 0;
        int pw = plane_w(m, c), ph = plane_h(m, c);
        int bszx = N265_MCSTF_BLK >> sx, bszy = N265_MCSTF_BLK >> sy;
        for (int y = 0; y < ph; y++) {
            int by = y / bszy;
            const pixel *o = cur->org[c] + (size_t)y * cur->stride[c];
            pixel *d = m->out[c] + (size_t)y * m->out_stride[c];
            for (int x = 0; x < pw; x++) {
                int bx = x / bszx;
                int org = o[x];
                int wsum = 256;
                int acc = 256 * org;
                for (int k = 0; k < nb; k++) {
                    const int16_t *mv = m->mv +
                        (size_t)k * m->bw * m->bh * 2 +
                        ((size_t)by * m->bw + bx) * 2;
                    const uint16_t *be = m->berr +
                        (size_t)k * m->bw * m->bh + (size_t)by * m->bw + bx;
                    const mc_pic *r = &m->p[nbi[k]];
                    int rv = r->org[c][(ptrdiff_t)(y + (mv[1] >> sy)) *
                                       r->stride[c] + x + (mv[0] >> sx)];
                    int diff = rv - org;
                    if (diff < 0)
                        diff = -diff;
                    if (diff > gmax)
                        continue;
                    int wq = mc_tstr[nbd[k]] * mc_berr_w(*be);  /* Q16 */
                    wq = (wq * m->gauss[diff]) >> 16;           /* Q8  */
                    if (!wq)
                        continue;
                    acc += wq * rv;
                    wsum += wq;
                }
                d[x] = (pixel)((acc + wsum / 2) / wsum);
            }
        }
        pad_plane(m->out[c], m->out_stride[c], pw, ph, N265_MCSTF_PAD);
    }
}

void n265_mcstf_pop(n265_mcstf *m, const pixel *plane[3], int stride[3])
{
    int ci = m->out_pos;
    const mc_pic *cur = &m->p[ci];
    int filt = (cur->display_idx % n265_mcstf_period()) == 0 && m->n > 1;
    m->last_noise_q8 = -1;
    if (filt && m->selective) {
        m->last_noise_q8 = mcstf_noise(m, cur);
        if (m->last_noise_q8 < n265_mcstf_clean_q8())
            filt = 0;
    }
    m->last_filtered = filt;
    if (filt) {
        mcstf_filter(m, ci);
        for (int c = 0; c < 3; c++) {
            plane[c] = m->out[c];
            stride[c] = m->out_stride[c];
        }
    } else {
        for (int c = 0; c < 3; c++) {
            plane[c] = cur->org[c];
            stride[c] = cur->stride[c];
        }
    }
    if (n265_mcstf_trace())
        fprintf(stderr, "mcstf %5d filt=%d noise=%.2f nb=%d\n",
                cur->display_idx, filt,
                m->last_noise_q8 < 0 ? -1.0 : m->last_noise_q8 / 256.0,
                m->n - 1);
    m->out_pos++;
}

int n265_mcstf_last_filtered(const n265_mcstf *m)
{
    return m->last_filtered;
}

int n265_mcstf_last_noise_q8(const n265_mcstf *m)
{
    return m->last_noise_q8;
}
