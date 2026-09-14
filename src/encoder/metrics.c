/*
 * PSNR, SSIM and the CSV log (phase 14 item 4). See metrics.h for why
 * the reconstruction is a legitimate stand-in for a decode here.
 * Copyright (c) 2026, the yah265 authors. GPL-2.0-or-later; see LICENSE.
 */

#include <math.h>
#include <string.h>

#include "encoder/metrics.h"

/* One plane's summed squared error. Read in a fixed raster order on the
 * driver thread, so the sum is a pure function of the two pictures and
 * not of the schedule. The accumulator is uint64 and not double: at
 * 1080p, 10 bits and a bad frame the per-frame total is about 2^41, so
 * the exact integer is free and the rounding question does not arise. */
static double plane_ssd(const pixel *a, int as, const pixel *b, int bs,
                        int w, int h)
{
    uint64_t ssd = 0;
    for (int y = 0; y < h; y++) {
        const pixel *pa = a + (size_t)y * as;
        const pixel *pb = b + (size_t)y * bs;
        for (int x = 0; x < w; x++) {
            int d = (int)pa[x] - (int)pb[x];
            ssd += (uint64_t)(d * d);
        }
    }
    return (double)ssd;
}

/* ffmpeg's vf_ssim, to the arithmetic: 8x8 windows at stride 4 over the
 * plane, the two stabilising constants scaled by the window's 64 samples
 * (and, for the variance term, by its 63 degrees of freedom), and the
 * plain mean over windows. The plane is truncated to a multiple of four
 * in each direction the way that filter truncates it, which is why a
 * width of 354 gives the same answer as 352.
 *
 * The integer sums are exact at both depths - at 10 bits the largest is
 * `ss`, 64 * 2 * 1023^2 = 2^27 - so the only floating-point step is the
 * final ratio, and that is where ffmpeg puts it too. */
static double ssim_plane(const pixel *m, int ms, const pixel *r, int rs,
                         int w, int h)
{
    const double max = (double)PIXEL_MAX;
    const int64_t c1 = (int64_t)(.01 * .01 * max * max * 64 + .5);
    const int64_t c2 = (int64_t)(.03 * .03 * max * max * 64 * 63 + .5);
    int bw = w >> 2, bh = h >> 2;
    if (bw < 2 || bh < 2)
        return 0.0;
    double sum = 0.0;
    for (int by = 0; by < bh - 1; by++) {
        for (int bx = 0; bx < bw - 1; bx++) {
            const pixel *a = m + (size_t)(4 * by) * ms + 4 * bx;
            const pixel *b = r + (size_t)(4 * by) * rs + 4 * bx;
            int64_t s1 = 0, s2 = 0, ss = 0, s12 = 0;
            for (int j = 0; j < 8; j++) {
                for (int i = 0; i < 8; i++) {
                    int64_t pa = a[(size_t)j * ms + i];
                    int64_t pb = b[(size_t)j * rs + i];
                    s1 += pa;
                    s2 += pb;
                    ss += pa * pa + pb * pb;
                    s12 += pa * pb;
                }
            }
            int64_t vars = ss * 64 - s1 * s1 - s2 * s2;
            int64_t covar = s12 * 64 - s1 * s2;
            sum += (double)(2 * s1 * s2 + c1) * (double)(2 * covar + c2) /
                   ((double)(s1 * s1 + s2 * s2 + c1) * (double)(vars + c2));
        }
    }
    return sum / (double)((bw - 1) * (bh - 1));
}

static double psnr_db(double ssd, double npix)
{
    if (npix <= 0.0)
        return 0.0;
    double mse = ssd / npix;
    /* A perfect plane has no logarithm, and every substitute for one is
     * worse than saying so: a value derived from the plane's SIZE (one
     * unit of error over it, say) makes the pooled number move when a
     * second lossless frame arrives, which is a metric that depends on
     * how much of the clip you measured. ffmpeg prints "inf" here and so
     * does this. */
    if (mse <= 0.0)
        return INFINITY;
    double max = (double)PIXEL_MAX;
    return 10.0 * log10(max * max / mse);
}

void n265_metrics_frame(n265_metrics *m,
                        const pixel *const src[3], const int sstride[3],
                        const pixel *const rec[3], const int rstride[3],
                        int w, int h, int csx, int csy)
{
    double ssd[3], npix[3], total = 0.0, wmse = 0.0;
    for (int p = 0; p < 3; p++) {
        int pw = p ? w >> csx : w;
        int ph = p ? h >> csy : h;
        npix[p] = (double)pw * (double)ph;
        ssd[p] = m->psnr ? plane_ssd(src[p], sstride[p], rec[p], rstride[p],
                                     pw, ph)
                         : 0.0;
        total += npix[p];
    }
    if (m->psnr) {
        for (int p = 0; p < 3; p++) {
            m->ssd[p] += ssd[p];
            m->npix[p] += npix[p];
            m->f_psnr[p] = psnr_db(ssd[p], npix[p]);
            /* the average is over the pixel-WEIGHTED mean squared error,
             * ffmpeg's definition; averaging the three dB values instead
             * would weight a chroma plane like a luma one */
            wmse += ssd[p];
        }
        m->f_psnr_avg = psnr_db(wmse, total);
    }
    if (m->ssim) {
        m->f_ssim = ssim_plane(src[0], sstride[0], rec[0], rstride[0], w, h);
        m->ssim_sum += m->f_ssim;
    }
    m->frames++;
}

void n265_metrics_pooled(const n265_metrics *m, double out[6])
{
    for (int i = 0; i < 6; i++)
        out[i] = 0.0;
    if (m->frames <= 0)
        return;
    if (m->psnr) {
        double ssd = 0.0, npix = 0.0;
        for (int p = 0; p < 3; p++) {
            out[p] = psnr_db(m->ssd[p], m->npix[p]);
            ssd += m->ssd[p];
            npix += m->npix[p];
        }
        out[3] = psnr_db(ssd, npix);
    }
    if (m->ssim) {
        double s = m->ssim_sum / (double)m->frames;
        out[4] = s;
        /* ffmpeg's dB form, infinite at 1 for the reason above. */
        out[5] = s >= 1.0 ? INFINITY : 10.0 * log10(1.0 / (1.0 - s));
    }
}

/* ---- the CSV ----
 *
 * x265's two shapes: level 0 is one row describing the whole run, level 1
 * a row per coded frame, level 2 the same rows with the rate control's
 * own columns on the end. A level-2 file is therefore readable by
 * anything that reads a level-1 one, which is why the new columns go
 * last rather than beside the ones they belong to.
 *
 * What is deliberately NOT here is x265's elapsed-time and fps columns.
 * A clock in a log makes two runs of the same encode differ, and every
 * other sidecar this encoder writes (--shot-stats, --pass) is a pure
 * function of the input on purpose. Timing is what the board measures. */
int n265_csv_open(n265_csv *c, const char *path, int level, int psnr,
                  int ssim)
{
    memset(c, 0, sizeof(*c));
    if (!path || !*path)
        return 0;
    c->f = fopen(path, "w");
    if (!c->f)
        return -1;
    c->level = level;
    c->psnr = psnr;
    c->ssim = ssim;
    if (level >= 1) {
        fprintf(c->f, "POC, Order, Type, Depth, QP, Bits");
        if (psnr)
            fprintf(c->f, ", PSNR Y, PSNR U, PSNR V, PSNR Avg");
        if (ssim)
            fprintf(c->f, ", SSIM Y, SSIM (dB)");
        if (level >= 2)
            fprintf(c->f, ", Planned Bits, VBV Fill, VBV dQP, Flat");
        fprintf(c->f, "\n");
    }
    return 0;
}

void n265_csv_frame(n265_csv *c, const n265_csv_row *r)
{
    if (!c->f || c->level < 1)
        return;
    fprintf(c->f, "%d, %d, %c, %d, %d, %ld", r->poc, r->order, r->type,
            r->b_depth, r->qp, r->bits);
    if (c->psnr) {
        const n265_metrics *m = r->m;
        fprintf(c->f, ", %.4f, %.4f, %.4f, %.4f", m->f_psnr[0], m->f_psnr[1],
                m->f_psnr[2], m->f_psnr_avg);
    }
    if (c->ssim) {
        double s = r->m->f_ssim;
        fprintf(c->f, ", %.6f, %.4f", s,
                s >= 1.0 ? INFINITY : 10.0 * log10(1.0 / (1.0 - s)));
    }
    if (c->level >= 2)
        fprintf(c->f, ", %.0f, %.0f, %+.3f, %d", r->planned_bits, r->vbv_fill,
                r->vbv_dqp, r->flat);
    fprintf(c->f, "\n");
    c->rows++;
}

void n265_csv_summary(n265_csv *c, long frames, double bytes,
                      double fps_num, double fps_den,
                      const n265_metrics *m)
{
    if (!c->f)
        return;
    if (c->level >= 1)
        fprintf(c->f, "\n");
    double kbps = 0.0;
    if (frames > 0 && fps_den > 0.0)
        kbps = bytes * 8.0 * (fps_num / fps_den) / (double)frames / 1000.0;
    fprintf(c->f, "Frames, Bytes, Bitrate (kbps)");
    if (c->psnr)
        fprintf(c->f, ", PSNR Y, PSNR U, PSNR V, PSNR Avg");
    if (c->ssim)
        fprintf(c->f, ", SSIM Y, SSIM (dB)");
    fprintf(c->f, "\n%ld, %.0f, %.2f", frames, bytes, kbps);
    if (c->psnr || c->ssim) {
        double p[6];
        n265_metrics_pooled(m, p);
        if (c->psnr)
            fprintf(c->f, ", %.4f, %.4f, %.4f, %.4f", p[0], p[1], p[2], p[3]);
        if (c->ssim)
            fprintf(c->f, ", %.6f, %.4f", p[4], p[5]);
    }
    fprintf(c->f, "\n");
}

void n265_csv_close(n265_csv *c)
{
    if (c->f)
        fclose(c->f);
    c->f = NULL;
}
