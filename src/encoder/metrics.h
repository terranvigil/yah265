/*
 * Distortion metrics and the CSV log (phase 14 item 4).
 *
 * PSNR and SSIM are measured on the encoder's OWN reconstruction against
 * the picture it coded - no decode, no second process - which is sound
 * here for one reason and it is checked on every vector: the conformance
 * gate holds our reconstruction bit-exact against ffmpeg's and
 * libde265's decode of the same stream, so "our recon" and "what a
 * decoder puts on the screen" are the same samples.
 *
 * Both metrics follow ffmpeg's filters to the arithmetic so the numbers
 * can be checked against an outside instrument rather than against
 * themselves: PSNR pools the mean squared error over frames and then
 * takes the logarithm (vf_psnr's summary), and SSIM is the 8x8 window at
 * stride 4 over the luma plane, averaged over windows and then over
 * frames (vf_ssim).
 *
 * Copyright (c) 2026, the yah265 authors. GPL-2.0-or-later; see LICENSE.
 */

#ifndef N265_METRICS_H
#define N265_METRICS_H

#include <stdio.h>
#include <stdint.h>

#include "common/pixel.h"

typedef struct n265_metrics {
    int psnr, ssim;             /* --psnr / --ssim: what was asked for */
    /* POOLED, over every frame measured. The squared error is summed and
     * divided at the end, which is ffmpeg's pooling and not the mean of
     * per-frame dB values; the two differ by a lot on a clip with one
     * easy frame in it. */
    double ssd[3];
    double npix[3];
    double ssim_sum;            /* per-frame SSIM-Y, summed over frames */
    long frames;
    /* the frame just measured, for the CSV's row */
    double f_psnr[3], f_psnr_avg, f_ssim;
} n265_metrics;

/* Measure one frame. `w`/`h` are the LUMA picture size; the planes are
 * the padded ones the encoder holds, so the strides are theirs and only
 * the picture inside them is read. Nothing here writes to the encoder,
 * so it is called on the driver in coding order and its sums are a pure
 * function of the encode. */
void n265_metrics_frame(n265_metrics *m,
                        const pixel *const src[3], const int sstride[3],
                        const pixel *const rec[3], const int rstride[3],
                        int w, int h, int csx, int csy);

/* The pooled numbers: psnr Y/U/V, the pixel-weighted psnr average, SSIM-Y
 * and SSIM in dB. Zero frames leaves every field 0. */
void n265_metrics_pooled(const n265_metrics *m, double out[6]);

/* --csv / --csv-log-level. Level 0 is one summary row for the run, 1 adds
 * a row per coded frame, 2 adds the rate control's columns to that row. */
typedef struct n265_csv {
    FILE *f;
    int level;
    int psnr, ssim;             /* which metric columns the header carries */
    int rows;
} n265_csv;

typedef struct n265_csv_row {
    int poc;                    /* display order */
    int order;                  /* coding order */
    char type;                  /* I, P, B (b = not a reference) */
    int b_depth;
    int qp;
    long bits;                  /* the whole access unit, headers included */
    const n265_metrics *m;      /* NULL when no metric was asked for */
    /* level 2 */
    double planned_bits, vbv_fill, vbv_dqp;
    int flat;
} n265_csv_row;

int  n265_csv_open(n265_csv *c, const char *path, int level, int psnr,
                   int ssim);
void n265_csv_frame(n265_csv *c, const n265_csv_row *r);
void n265_csv_summary(n265_csv *c, long frames, double bytes,
                      double fps_num, double fps_den,
                      const n265_metrics *m);
void n265_csv_close(n265_csv *c);

#endif
