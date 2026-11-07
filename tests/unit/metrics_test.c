/*
 * Unit tests: PSNR, SSIM and the CSV log (src/encoder/metrics.c, phase 14
 * item 4).
 * Copyright (c) 2026, the yah265 authors. GPL-2.0-or-later; see LICENSE.
 *
 * The two metric numbers here were NOT produced by this code. They come
 * from an independent walk of the same definitions - ffmpeg's vf_psnr
 * pooling and vf_ssim's 8x8-at-stride-4 window - over the same two
 * synthetic planes, done in Python and pasted in. A metric that agrees
 * with itself proves nothing; the encode-side half of this claim is
 * docs/instruments.md's check against ffmpeg on a real clip, which agreed
 * to four decimal places at both depths.
 */

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "encoder/metrics.h"

static int fails;

#define CHECK(c, ...) do { \
    if (!(c)) { printf("FAIL: " __VA_ARGS__); printf("\n"); fails++; } \
} while (0)

#define W 32
#define H 32

#if BIT_DEPTH == 8
#define REF_PSNR_Y 34.4067903455
#define REF_SSIM_Y 0.993744602472
#else
#define REF_PSNR_Y 34.4269052914
#define REF_SSIM_Y 0.993743199093
#endif

/* The two planes the reference numbers were taken over. The pattern is
 * deliberately high-variance, so SSIM is dominated by the error term and
 * not by a flat region's constants. */
static void make_planes(pixel *src, pixel *rec)
{
    for (int y = 0; y < H; y++) {
        for (int x = 0; x < W; x++) {
            int s = ((x * 7 + y * 13) & 0xff) << (BIT_DEPTH - 8);
            int d = ((x * 5 + y * 3) % 17 - 8) * (1 << (BIT_DEPTH - 8)); /* a multiply, not a shift of a negative (UBSan) */
            int r = s + d;
            r = r < 0 ? 0 : r > PIXEL_MAX ? PIXEL_MAX : r;
            src[y * W + x] = (pixel)s;
            rec[y * W + x] = (pixel)r;
        }
    }
}

int main(void)
{
    static pixel sy[W * H], ry[W * H], sc[W * H / 4], rc[W * H / 4];
    make_planes(sy, ry);
    memcpy(sc, sy, sizeof(sc));
    memcpy(rc, sc, sizeof(rc));       /* chroma coded losslessly */

    const pixel *src[3] = { sy, sc, sc };
    const pixel *rec[3] = { ry, rc, rc };
    int sstride[3] = { W, W / 2, W / 2 };
    int rstride[3] = { W, W / 2, W / 2 };

    /* ---- PSNR ----
     *
     * The luma error is the same integer pattern at every depth, scaled
     * by the depth shift, but the peak is 1023, not 4 x 255, and the
     * clip at the top lands on different samples, so each depth has its
     * own reference. Both were checked against an independent walk and
     * ffmpeg's psnr and ssim filters on the same planes (gray, gray10le). */
    n265_metrics m;
    memset(&m, 0, sizeof(m));
    m.psnr = 1;
    m.ssim = 1;
    n265_metrics_frame(&m, src, sstride, rec, rstride, W, H, 1, 1);

    CHECK(fabs(m.f_psnr[0] - REF_PSNR_Y) < 1e-6,
          "per-frame PSNR Y %.10f, reference %.10f", m.f_psnr[0], REF_PSNR_Y);
    CHECK(isinf(m.f_psnr[1]) && isinf(m.f_psnr[2]),
          "a lossless plane has no logarithm and must read as inf, not as "
          "a number derived from its size: %.4f / %.4f", m.f_psnr[1],
          m.f_psnr[2]);
    /* The weighted average is over the pixel-weighted MSE, so with the
     * two chroma planes exact it is the luma MSE diluted by 3/2, i.e.
     * 10*log10(1.5) = 1.7609 dB better than luma alone. */
    CHECK(fabs(m.f_psnr_avg - (m.f_psnr[0] + 10.0 * log10(1.5))) < 0.02,
          "the psnr average is not the pixel-weighted one: %.4f vs %.4f",
          m.f_psnr_avg, m.f_psnr[0] + 10.0 * log10(1.5));

    /* ---- SSIM ---- */
    CHECK(fabs(m.f_ssim - REF_SSIM_Y) < 1e-9,
          "per-frame SSIM-Y %.12f, reference %.12f", m.f_ssim, REF_SSIM_Y);

    /* Pooling: a second identical frame must move neither number, because
     * the squared error is summed and divided by the samples it was
     * summed over, and the SSIM mean over two equal frames is the frame. */
    double p1[6], p2[6];
    n265_metrics_pooled(&m, p1);
    n265_metrics_frame(&m, src, sstride, rec, rstride, W, H, 1, 1);
    n265_metrics_pooled(&m, p2);
    for (int i = 0; i < 6; i++)
        CHECK(isinf(p1[i]) ? isinf(p2[i]) : fabs(p1[i] - p2[i]) < 1e-12,
              "pooled[%d] moved on a repeated frame: %.12f -> %.12f", i,
              p1[i], p2[i]);
    CHECK(m.frames == 2, "frame count %ld", m.frames);
    CHECK(fabs(p2[0] - m.f_psnr[0]) < 1e-9,
          "pooled PSNR Y over two equal frames is not the frame's: %.10f",
          p2[0]);
    CHECK(fabs(p2[5] - 10.0 * log10(1.0 / (1.0 - p2[4]))) < 1e-9,
          "SSIM dB is not 10 log10(1/(1-ssim))");

    /* An identical pair is SSIM 1 exactly: the numerator and the
     * denominator are the same expression term for term. */
    n265_metrics m2;
    memset(&m2, 0, sizeof(m2));
    m2.ssim = 1;
    const pixel *same[3] = { sy, sc, sc };
    n265_metrics_frame(&m2, same, sstride, same, sstride, W, H, 1, 1);
    CHECK(fabs(m2.f_ssim - 1.0) < 1e-12, "SSIM of a plane with itself is "
          "%.12f, not 1", m2.f_ssim);

    /* A plane too small to hold one 8x8 window has no SSIM to report,
     * and must not read as a division by zero. */
    n265_metrics m3;
    memset(&m3, 0, sizeof(m3));
    m3.ssim = 1;
    n265_metrics_frame(&m3, same, sstride, same, sstride, 4, 4, 1, 1);
    CHECK(m3.f_ssim == 0.0, "a 4x4 picture holds no window: %.12f",
          m3.f_ssim);

    /* ---- the CSV ---- */
    char path[] = "/tmp/n265_csv_XXXXXX";
    int fd = mkstemp(path);
    if (fd < 0) {
        printf("FAIL: mkstemp\n");
        return 1;
    }
    close(fd);

    n265_csv c;
    CHECK(n265_csv_open(&c, path, 2, 1, 1) == 0, "csv open");
    n265_csv_row r;
    memset(&r, 0, sizeof(r));
    r.poc = 4;
    r.order = 2;
    r.type = 'b';
    r.b_depth = 3;
    r.qp = 34;
    r.bits = 6400;
    r.m = &m;
    r.planned_bits = 1234.0;
    r.vbv_fill = 5000.0;
    r.vbv_dqp = -1.5;
    r.flat = 1;
    n265_csv_frame(&c, &r);
    n265_csv_summary(&c, 2, 8000.0, 30000.0, 1001.0, &m);
    n265_csv_close(&c);

    char buf[4096];
    FILE *f = fopen(path, "r");
    size_t n = f ? fread(buf, 1, sizeof(buf) - 1, f) : 0;
    if (f)
        fclose(f);
    buf[n] = 0;
    unlink(path);

    CHECK(strstr(buf, "POC, Order, Type, Depth, QP, Bits, PSNR Y") == buf,
          "the level-2 header is not the level-1 header with columns "
          "appended:\n%.120s", buf);
    CHECK(strstr(buf, ", Planned Bits, VBV Fill, VBV dQP, Flat\n") != NULL,
          "the rate-control columns are missing from the level-2 header");
    CHECK(strstr(buf, "4, 2, b, 3, 34, 6400,") != NULL,
          "the frame row is wrong:\n%s", buf);
    CHECK(strstr(buf, ", 1234, 5000, -1.500, 1\n") != NULL,
          "the level-2 columns are wrong:\n%s", buf);
    /* 8000 bytes over 2 frames at 30000/1001 fps is 959.04 kbit/s. */
    CHECK(strstr(buf, "\n2, 8000, 959.04") != NULL,
          "the summary row's bitrate is wrong:\n%s", buf);

    /* Level 0 writes the summary row and nothing else. */
    char path0[] = "/tmp/n265_csv0_XXXXXX";
    fd = mkstemp(path0);
    close(fd);
    CHECK(n265_csv_open(&c, path0, 0, 0, 0) == 0, "csv open level 0");
    n265_csv_frame(&c, &r);             /* must be ignored */
    n265_csv_summary(&c, 2, 8000.0, 25.0, 1.0, &m);
    n265_csv_close(&c);
    f = fopen(path0, "r");
    n = f ? fread(buf, 1, sizeof(buf) - 1, f) : 0;
    if (f)
        fclose(f);
    buf[n] = 0;
    unlink(path0);
    CHECK(strcmp(buf, "Frames, Bytes, Bitrate (kbps)\n2, 8000, 800.00\n")
          == 0, "level 0 is not one summary row:\n%s", buf);

    printf(fails ? "metrics: %d FAILED\n" : "metrics: ok (%d failures)\n",
           fails);
    return fails != 0;
}
