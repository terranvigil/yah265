/*
 * Film grain estimation and the denoise that pairs with it.
 *
 * Phase 12 item 3. The picture the encoder codes and the picture the
 * viewer sees are not the same picture when the film_grain_characteristics
 * SEI is written: the encoder may code a cored picture and the decoder
 * puts synthesised grain back on output. Everything about the SECOND
 * picture is decided here, in one place, on the driver thread, out of
 * integer sums taken in display order - so the model is a pure function
 * of the source and the whole thing stays byte-reproducible across
 * thread counts.
 *
 * What the model has to be is fixed by the only synthesiser this project
 * can measure through (docs/phase12.md, step 0): H.274's frequency
 * filtering model, in which the decoder puts
 *
 *     grain = (scale * pattern[h][v]) >> (log2_scale_factor + 6)
 *
 * on each 8x8 block, choosing the interval by the block's MEAN. So the
 * grain's standard deviation is linear in the signalled scale with a
 * constant that depends only on the cut-off pair, and the estimator's
 * whole job is to produce, per intensity interval, a sigma and a grain
 * size. The measured constants are K[] below.
 *
 * Copyright (c) 2026, the yah265 authors. GPL-2.0-or-later; see LICENSE.
 */

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "encoder/grain.h"
#include "encoder/preset.h"

/* sigma per unit of signalled scale at log2_scale_factor 0, indexed by the
 * cut-off value h (== v), 2..14. Measured end to end through the
 * synthesiser on a flat plate coded --lossless, not derived: the pattern
 * is a 64x64 integer transform of a fixed Gaussian LUT with an edge
 * attenuation on top, so its variance is a property of the tables and
 * there is nothing to derive it from but the tables. docs/phase12.md,
 * "The synthesiser, calibrated". Index 0 and 1 are unreachable (the
 * decoder clips the cut-off to 2..14) and are filled so an index error
 * would not read as a plausible grain. */
static const float K[15] = {
    0.0f, 0.0f, 0.158f, 0.210f, 0.273f, 0.335f, 0.377f, 0.450f,
    0.473f, 0.569f, 0.615f, 0.658f, 0.727f, 0.787f, 0.821f
};

/* What the estimator's 3x3 high-pass PASSES of grain of a given cut-off:
 * the residual std over the grain's own std, measured on the same flat
 * plate through the same synthesiser. The estimator reads a residual and
 * has to report a grain, so this is the division it does. It is strongly
 * cut-off dependent - coarse grain is almost entirely inside the
 * low-pass - which is why the size has to be estimated BEFORE the
 * amplitude can be. Same index convention as K. */
static const float G[15] = {
    0.0f, 0.0f, 0.154f, 0.174f, 0.215f, 0.256f, 0.334f, 0.382f,
    0.454f, 0.511f, 0.565f, 0.641f, 0.670f, 0.708f, 0.754f
};

/* What a PERCENTILE of the per-block residual reads against the mean. A
 * block's sample std is itself a random variable, so the 25th percentile
 * of the population sits under the truth even when every block in it is
 * pure grain; the normal quantile puts that at 0.94 for 64 samples and the
 * measurement puts it at 0.90, the difference being the grain's spatial
 * correlation cutting the effective degrees of freedom. Measured, not
 * derived: on synthetic grain of known sigma the estimator reads 0.89-0.92
 * of the truth over a 4x span of amplitude and the whole usable cut-off
 * range (docs/phase12.md, "The estimator, against ground truth"), so one
 * constant covers it. It is calibrated AT the default N265_FGC_PCT and
 * moves with it. */
#define EST_BIAS  1.11f

/* The cut-off the residual's lag-1 autocorrelation reports. rho is the
 * normalised correlation of neighbouring residual samples over the two
 * axes, on FLAT blocks only; the finer the grain, the less two
 * neighbours agree, so rho falls as the cut-off rises. The thresholds are
 * the midpoints of rho measured on synthesised grain of known cut-off
 * (scratch/rho_calib.py; the table is in docs/phase12.md).
 *
 * The map is monotone only from h = 5 up, and that is not a defect of the
 * measurement: below 5 the grain is so smooth that a 3x3 high-pass of it
 * is down at the rounding floor, and the reading turns back on itself
 * (rho peaks at 0.545 around h = 4-5 and falls away again to 0.397 at
 * h = 2). So the estimate is taken on the decreasing branch alone and
 * saturates at 5. Nothing is lost: the whole branch below 5 is grain
 * coarser than an 8x8 block, which this corpus does not contain and
 * which the amplitude estimate could not see either. */
static int cut_of_rho(double rho)
{
    static const struct { double rho; int h; } tab[] = {
        { 0.498, 5 }, { 0.427, 6 }, { 0.345, 7 }, { 0.247, 8 },
        { 0.159, 9 }, { 0.067, 10 }, { -0.010, 11 }, { -0.068, 12 },
        { -0.124, 13 }, { -1e9, 14 }
    };
    for (unsigned i = 0; i < sizeof tab / sizeof tab[0]; i++)
        if (rho >= tab[i].rho)
            return tab[i].h;
    return 14;
}

/* ------------------------------------------------------------------ */

int n265_fgc_pct(void)
{
    static int v = -1;
    if (v < 0) {
        const char *s = n265_env("N265_FGC_PCT");
        v = s ? atoi(s) : 25;
        if (v < 1) v = 1;
        if (v > 99) v = 99;
    }
    return v;
}

int n265_fgc_frames(void)
{
    static int v = -1;
    if (v < 0) {
        const char *s = n265_env("N265_FGC_FRAMES");
        v = s ? atoi(s) : 1;
        if (v < 1) v = 1;
        if (v > 32) v = 32;
    }
    return v;
}

int n265_fgc_chroma(void)
{
    static int v = -1;
    if (v < 0) {
        const char *s = n265_env("N265_FGC_CHROMA");
        v = s ? atoi(s) : 1;
    }
    return v;
}

double n265_fgc_core(void)
{
    static double v = -1.0;
    if (v < 0.0) {
        const char *s = n265_env("N265_FGC_CORE");
        v = s ? atof(s) : 1.0;
        if (v < 0.0) v = 0.0;
        if (v > 4.0) v = 4.0;
    }
    return v;
}

/* The most an intensity bin may report over the QUIETEST bin. Film grain
 * IS intensity dependent and the SEI's interval structure exists to carry
 * that, but a residual is grain plus texture and only the quiet end of
 * the population is evidence about grain at all. Measured on the corpus
 * (docs/phase12.md): on park_joy the per-bin 25th percentile runs 0.98 in
 * the shadows to 8.38 in the midtones, and the midtone reading is
 * foliage - there is not one flat block at that exposure in the whole
 * frame. Uncapped, the estimator would signal that foliage as grain and
 * the decoder would put noise on the screen that was never in the source.
 * So a bin may rise over the quietest one by this factor and no further,
 * which is the direction that under-reads rather than the one that
 * invents. */
double n265_fgc_ratio(void)
{
    static double v = -1.0;
    if (v < 0.0) {
        const char *s = n265_env("N265_FGC_RATIO");
        v = s ? atof(s) : 2.0;
        if (v < 1.0) v = 1.0;
    }
    return v;
}

/* The grain std, in 8-bit levels, below which a component carries no
 * grain worth signalling. The control the item is priced against is a
 * CLEAN clip, and on one the right SEI is no SEI at all. */
double n265_fgc_min(void)
{
    static double v = -1.0;
    if (v < 0.0) {
        const char *s = n265_env("N265_FGC_MIN");
        v = s ? atof(s) : 1.0;
        if (v < 0.0) v = 0.0;
    }
    return v;
}

int n265_fgc_trace(void)
{
    static int v = -1;
    if (v < 0)
        v = n265_env("N265_FGC_TRACE") != NULL;
    return v;
}

/* ------------------------------------------------------------------ */

/* Per-8x8-block statistics of one plane of one picture. The residual is
 * the source minus a 3x3 binomial low-pass of it; what is kept is its
 * energy and its two lag-1 products, all as integer sums. */
struct blk {
    int32_t bin;        /* intensity bin of the block's mean */
    int64_t ssq;        /* sum of r^2 over the block */
    int64_t ah, avv;    /* sum of r[x]r[x+1] and r[y]r[y+1] */
    int64_t dh, dv;     /* the matching sums of r^2 over each pair support */
};

struct n265_grain_est {
    int w, h, csx, csy;
    int nframes;                    /* pictures folded in so far */
    struct blk *blk;                /* scratch, one plane at a time */
    int64_t *sortbuf;
    int nblk_max;
    /* per component, per bin: the running sum of the percentile sigma and
     * the count of frames that had a populated bin */
    double sig_sum[3][N265_FGC_BINS];
    int    sig_n[3][N265_FGC_BINS];
    /* per component: the flat-block correlation sums */
    int64_t rho_num[3], rho_den[3];
};

n265_grain_est *n265_grain_open(int w, int h, int csx, int csy)
{
    n265_grain_est *g = calloc(1, sizeof *g);
    if (!g)
        return NULL;
    g->w = w;
    g->h = h;
    g->csx = csx;
    g->csy = csy;
    g->nblk_max = (w / 8 + 1) * (h / 8 + 1);
    g->blk = malloc((size_t)g->nblk_max * sizeof *g->blk);
    g->sortbuf = malloc((size_t)g->nblk_max * sizeof *g->sortbuf);
    if (!g->blk || !g->sortbuf) {
        n265_grain_close(g);
        return NULL;
    }
    return g;
}

void n265_grain_close(n265_grain_est *g)
{
    if (!g)
        return;
    free(g->blk);
    free(g->sortbuf);
    free(g);
}

/* The intensity bin of a sample value, in the 8-BIT domain: the SEI's
 * interval bounds are 8-bit numbers whatever the coded depth is, and the
 * only synthesiser in the chain is 8-bit anyway. */
static int bin_of(int mean8)
{
    int b = mean8 * N265_FGC_BINS / 256;
    return b < 0 ? 0 : b >= N265_FGC_BINS ? N265_FGC_BINS - 1 : b;
}

/* One 3x3 binomial low-pass sample, clamped at the plane edge. */
static inline int lp3(const pixel *p, int stride, int x, int y, int w, int h)
{
    int xm = x > 0 ? x - 1 : 0, xp = x + 1 < w ? x + 1 : w - 1;
    int ym = y > 0 ? y - 1 : 0, yp = y + 1 < h ? y + 1 : h - 1;
    const pixel *r0 = p + (size_t)ym * stride;
    const pixel *r1 = p + (size_t)y * stride;
    const pixel *r2 = p + (size_t)yp * stride;
    int s = r0[xm] + 2 * r0[x] + r0[xp]
          + 2 * (r1[xm] + 2 * r1[x] + r1[xp])
          + r2[xm] + 2 * r2[x] + r2[xp];
    return (s + 8) >> 4;
}

static int cmp_i64(const void *a, const void *b)
{
    int64_t x = *(const int64_t *)a, y = *(const int64_t *)b;
    return x < y ? -1 : x > y ? 1 : 0;
}

/* Fold one plane. */
static void accum_plane(n265_grain_est *g, int c, const pixel *p, int stride,
                        int w, int h)
{
    int bw = w / 8, bh = h / 8;
    if (bw < 1 || bh < 1)
        return;
    int nb = 0;
    int r[8][8];
    for (int by = 0; by < bh; by++) {
        for (int bx = 0; bx < bw; bx++) {
            int x0 = bx * 8, y0 = by * 8;
            int sum = 0;
            for (int y = 0; y < 8; y++)
                for (int x = 0; x < 8; x++) {
                    int v = p[(size_t)(y0 + y) * stride + x0 + x];
                    sum += v;
                    r[y][x] = v - lp3(p, stride, x0 + x, y0 + y, w, h);
                }
            struct blk *b = &g->blk[nb++];
            /* the block mean in the 8-bit domain, which is what the
             * decoder's own avg_8x8 would read on an 8-bit stream */
            b->bin = bin_of((sum >> 6) >> BIT_DEPTH_SHIFT);
            b->ssq = b->ah = b->avv = b->dh = b->dv = 0;
            for (int y = 0; y < 8; y++)
                for (int x = 0; x < 8; x++)
                    b->ssq += (int64_t)r[y][x] * r[y][x];
            for (int y = 0; y < 8; y++)
                for (int x = 0; x < 7; x++) {
                    b->ah += (int64_t)r[y][x] * r[y][x + 1];
                    b->dh += (int64_t)r[y][x] * r[y][x];
                }
            for (int y = 0; y < 7; y++)
                for (int x = 0; x < 8; x++) {
                    b->avv += (int64_t)r[y][x] * r[y + 1][x];
                    b->dv += (int64_t)r[y][x] * r[y][x];
                }
        }
    }

    /* Per bin, the p-th percentile of the block residual energy. THIS is
     * the whole separation of grain from detail: a block with an edge in
     * it has a residual an order of magnitude above the grain floor, and
     * a low percentile of the population inside one intensity bin picks
     * the flat blocks without ever having to classify one. On a clip with
     * no flat blocks at all the estimate is an over-read, and that is the
     * direction that costs bits rather than the one that invents them. */
    int pct = n265_fgc_pct();
    for (int bin = 0; bin < N265_FGC_BINS; bin++) {
        int n = 0;
        for (int i = 0; i < nb; i++)
            if (g->blk[i].bin == bin)
                g->sortbuf[n++] = g->blk[i].ssq;
        if (n < 16)         /* too thin a population to read a percentile */
            continue;
        qsort(g->sortbuf, (size_t)n, sizeof *g->sortbuf, cmp_i64);
        int k = (n - 1) * pct / 100;
        int64_t thr = g->sortbuf[k];
        double sig = sqrt((double)thr / 64.0);
        /* back to 8-bit magnitudes: the SEI's scale is an 8-bit quantity */
        sig /= (double)(1 << BIT_DEPTH_SHIFT);
        g->sig_sum[c][bin] += sig;
        g->sig_n[c][bin]++;
        /* the grain SIZE, off the same flat population: one reading per
         * component rather than per bin, because grain size is a property
         * of the stock and not of the exposure */
        for (int i = 0; i < nb; i++) {
            const struct blk *b = &g->blk[i];
            if (b->bin != bin || b->ssq > thr)
                continue;
            g->rho_num[c] += b->ah + b->avv;
            g->rho_den[c] += b->dh + b->dv;
        }
    }
}

void n265_grain_accum(n265_grain_est *g, const pixel *const plane[3],
                      const int stride[3])
{
    if (!g || g->nframes >= n265_fgc_frames())
        return;
    accum_plane(g, 0, plane[0], stride[0], g->w, g->h);
    if (n265_fgc_chroma())
        for (int c = 1; c < 3; c++)
            accum_plane(g, c, plane[c], stride[c],
                        g->w >> g->csx, g->h >> g->csy);
    g->nframes++;
}

/* The cut-off a component is SIGNALLED at, given the one its plane was
 * measured at. The decoder's 4:2:0 adaptation doubles a chroma cut-off
 * (clipping at 14) and halves a chroma scale before it synthesises
 * anything - measured, docs/phase12.md - so a chroma component is
 * signalled at half of what its plane should end up with. */
static int sig_cut(int c, int eff)
{
    int v = c ? (eff + 1) / 2 : eff;
    return v < 2 ? 2 : v > 14 ? 14 : v;
}

/* ...and what that signalled value actually synthesises with, which is
 * the index the scale has to be solved against. Not the same number for
 * chroma, and not always 2x the signalled one either: the doubling clips. */
static int act_cut(int c, int sgn)
{
    int v = c ? 2 * sgn : sgn;
    return v > 14 ? 14 : v;
}

/* The signalled scale that lands `sigma` on a component's plane. */
static double scale_for(int c, double sigma, int sgn, int log2)
{
    double need = sigma / K[act_cut(c, sgn)] * (double)(1 << log2);
    return c ? need * 2.0 : need;   /* the decoder halves a chroma scale */
}

void n265_grain_finish(const n265_grain_est *g, n265_grain_model *m,
                       double strength, int chroma)
{
    memset(m, 0, sizeof *m);
    if (!g || g->nframes == 0)
        return;

    /* One log2_scale_factor for the whole SEI, picked so the largest
     * scale any interval needs lands in (32, 64]: the synthesised grain
     * is held in an int8_t, so scale * 127 >> (log2 + 6) has to stay
     * inside 127 - i.e. scale <= 64 << log2 - and using the top of that
     * range is what buys the low-sigma intervals their resolution. */
    double smax = 0.0;
    float sig[3][N265_FGC_BINS];
    int cut[3];
    int ncomp = chroma ? 3 : 1;
    double ratio = n265_fgc_ratio();
    for (int c = 0; c < 3; c++) {
        double rho = g->rho_den[c] > 0
                   ? (double)g->rho_num[c] / (double)g->rho_den[c] : 0.0;
        cut[c] = cut_of_rho(rho);
        /* The SIZE first, then the amplitude: what the high-pass passes of
         * the grain depends on how coarse the grain is, so there is no
         * reading of the amplitude until the cut-off is known. */
        double gain = G[cut[c] < 2 ? 2 : cut[c]];
        double qmin = 1e30;
        for (int b = 0; b < N265_FGC_BINS; b++)
            if (g->sig_n[c][b]) {
                double s = g->sig_sum[c][b] / g->sig_n[c][b];
                if (s < qmin)
                    qmin = s;
            }
        for (int b = 0; b < N265_FGC_BINS; b++) {
            double s = g->sig_n[c][b]
                     ? g->sig_sum[c][b] / g->sig_n[c][b] : 0.0;
            if (s > qmin * ratio)       /* the cap; see n265_fgc_ratio */
                s = qmin * ratio;
            s /= gain > 0.0 ? gain : 1.0;
            s *= EST_BIAS;
            s *= strength;
            if (s > N265_FGC_SIGMA_MAX)
                s = N265_FGC_SIGMA_MAX;
            sig[c][b] = (float)s;
        }
    }
    /* A bin nobody populated inherits its nearest populated neighbour:
     * the decoder picks an interval for EVERY block, so a hole in the
     * middle of the range would be a hole in the synthesis. */
    for (int c = 0; c < ncomp; c++) {
        int any = 0;
        for (int b = 0; b < N265_FGC_BINS; b++)
            any |= g->sig_n[c][b] != 0;
        if (!any)
            continue;
        for (int b = 0; b < N265_FGC_BINS; b++) {
            if (g->sig_n[c][b])
                continue;
            int best = -1, bd = 1 << 30;
            for (int k = 0; k < N265_FGC_BINS; k++)
                if (g->sig_n[c][k] && abs(k - b) < bd) {
                    bd = abs(k - b);
                    best = k;
                }
            sig[c][b] = sig[c][best];
        }
    }
    for (int c = 0; c < ncomp; c++)
        for (int b = 0; b < N265_FGC_BINS; b++) {
            double need = scale_for(c, sig[c][b], sig_cut(c, cut[c]), 0);
            if (need > smax)
                smax = need;
        }
    if (smax <= 0.0)
        return;
    int log2 = 0;
    while (log2 < 15 && smax * (1 << (log2 + 1)) <= 64.0)
        log2++;
    m->log2_scale = log2;

    for (int c = 0; c < ncomp; c++) {
        int hc = sig_cut(c, cut[c]);
        int n = 0;
        for (int b = 0; b < N265_FGC_BINS; b++) {
            int sc = (int)lround(scale_for(c, sig[c][b], hc, log2));
            if (sc > (64 << log2))
                sc = 64 << log2;
            if (sc < 0)
                sc = 0;
            m->sigma[c][b] = sig[c][b];
            /* merge with the interval below when the SYNTAX would be the
             * same: fewer intervals, bit-identical synthesis */
            if (n > 0 && m->scale[c][n - 1] == sc) {
                m->hi[c][n - 1] = (uint8_t)((b + 1) * 256 / N265_FGC_BINS - 1);
                continue;
            }
            m->lo[c][n] = (uint8_t)(b * 256 / N265_FGC_BINS);
            m->hi[c][n] = (uint8_t)((b + 1) * 256 / N265_FGC_BINS - 1);
            m->scale[c][n] = (int16_t)sc;
            m->hcut[c][n] = (int8_t)hc;
            m->vcut[c][n] = (int8_t)hc;
            n++;
        }
        /* A component with no grain worth signalling is left out of the
         * SEI: one whose every interval quantises to zero scale, and one
         * whose loudest interval is under the floor. The second is the
         * clean-clip case the item is controlled on - the right SEI on a
         * clip with no grain is no SEI at all. */
        int nonzero = 0;
        double peak = 0.0;
        for (int i = 0; i < n; i++)
            nonzero |= m->scale[c][i] != 0;
        for (int b = 0; b < N265_FGC_BINS; b++)
            if (sig[c][b] > peak)
                peak = sig[c][b];
        if (!nonzero || peak < n265_fgc_min())
            continue;
        m->comp[c] = 1;
        m->nival[c] = n;
        m->present = 1;
    }

    if (n265_fgc_trace()) {
        fprintf(stderr, "FGC: frames %d log2_scale %d present %d\n",
                g->nframes, m->log2_scale, m->present);
        for (int c = 0; c < 3; c++) {
            if (!m->comp[c])
                continue;
            double rho = g->rho_den[c] > 0
                       ? (double)g->rho_num[c] / (double)g->rho_den[c] : 0.0;
            fprintf(stderr, "FGC: comp %d rho %.4f cut %d intervals %d\n",
                    c, rho, m->hcut[c][0], m->nival[c]);
            for (int i = 0; i < m->nival[c]; i++)
                fprintf(stderr, "FGC:   [%3u,%3u] scale %4d  sigma %.3f\n",
                        m->lo[c][i], m->hi[c][i], m->scale[c][i],
                        (double)m->scale[c][i] * K[act_cut(c, m->hcut[c][i])]
                        / (double)(1 << m->log2_scale) / (c ? 2.0 : 1.0));
        }
    }
}

/* ------------------------------------------------------------------ */

void n265_grain_denoise(const n265_grain_model *m, pixel *plane, int stride,
                        int w, int h, int comp)
{
    if (!m->present || !m->comp[comp])
        return;
    double k = n265_fgc_core();
    if (k <= 0.0)
        return;
    /* the coring threshold per intensity bin, in native sample units */
    int thr[N265_FGC_BINS];
    for (int b = 0; b < N265_FGC_BINS; b++) {
        double t = m->sigma[comp][b] * k * (double)(1 << BIT_DEPTH_SHIFT);
        thr[b] = (int)lround(t);
        if (thr[b] < 0)
            thr[b] = 0;
    }
    /* One pass, block by block, reading the SOURCE and writing the
     * result into the same plane: the low-pass is taken from a two-row
     * window copied out first, so a written sample is never read again.
     *
     * The filter is a soft threshold on the 3x3 high-pass, not the
     * low-pass itself. Coring is the whole point: it takes out exactly
     * the sub-sigma fluctuation the SEI promises to put back, and leaves
     * any structure above the grain floor alone. Blurring the picture
     * would take out detail the decoder has no way to restore. */
    int bw = (w + 7) / 8;
    uint8_t *bmean = malloc((size_t)bw * ((h + 7) / 8));
    pixel *prev = malloc((size_t)w * 2 * sizeof(pixel));
    if (!prev || !bmean) {
        free(prev);
        free(bmean);
        return;
    }
    /* The interval the decoder will pick for a block is chosen by that
     * block's MEAN, so the threshold this pass cores against has to be
     * chosen the same way - per block, not per sample. Taken from the
     * source before any sample is written. */
    for (int by = 0; by * 8 < h; by++)
        for (int bx = 0; bx < bw; bx++) {
            int x0 = bx * 8, y0 = by * 8, n = 0, s = 0;
            for (int y = y0; y < y0 + 8 && y < h; y++)
                for (int x = x0; x < x0 + 8 && x < w; x++) {
                    s += plane[(size_t)y * stride + x];
                    n++;
                }
            bmean[by * bw + bx] =
                (uint8_t)((s / (n ? n : 1)) >> BIT_DEPTH_SHIFT);
        }
    pixel *pm1 = prev, *pm0 = prev + w;
    for (int y = 0; y < h; y++) {
        /* pm1 holds row y-1 as it was BEFORE this pass touched it */
        const pixel *row = plane + (size_t)y * stride;
        memcpy(pm0, row, (size_t)w * sizeof(pixel));
        for (int x = 0; x < w; x++) {
            int xm = x > 0 ? x - 1 : 0, xp = x + 1 < w ? x + 1 : w - 1;
            const pixel *up = y > 0 ? pm1 : pm0;
            const pixel *dn = y + 1 < h ? plane + (size_t)(y + 1) * stride
                                        : pm0;
            int s = up[xm] + 2 * up[x] + up[xp]
                  + 2 * (pm0[xm] + 2 * pm0[x] + pm0[xp])
                  + dn[xm] + 2 * dn[x] + dn[xp];
            int lp = (s + 8) >> 4;
            int r = (int)pm0[x] - lp;
            int t = thr[bin_of(bmean[(y / 8) * bw + x / 8])];
            int a = r < 0 ? -r : r;
            a -= t;
            if (a < 0)
                a = 0;
            int out = lp + (r < 0 ? -a : a);
            if (out < 0) out = 0;
            if (out > PIXEL_MAX) out = PIXEL_MAX;
            plane[(size_t)y * stride + x] = (pixel)out;
        }
        pixel *sw = pm1; pm1 = pm0; pm0 = sw;
    }
    free(prev);
    free(bmean);
}
