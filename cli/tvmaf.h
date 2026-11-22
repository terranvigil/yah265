/*
 * Target-quality encoding (--target-vmaf): the CRF search and the VMAF v1
 * scorer it calls. The trial encodes themselves live in y265.c.
 * Copyright (c) 2026, the yah265 authors. GPL-2.0-or-later; see LICENSE.
 */
#ifndef N265_TVMAF_H
#define N265_TVMAF_H

#include <stdio.h>

typedef struct {
    double vmaf;
    double kbps;
    long bytes;
} tv_score;

/* encode at `crf` and score it; 0 or -1 (the callee has said why) */
typedef int (*tv_score_fn)(void *ctx, double crf, tv_score *out);

typedef struct {
    double target;
    double tol;                 /* acceptance window, VMAF points */
    double crf_lo, crf_hi;      /* best and worst quality searched */
    double crf_start;
    double step;                /* the CRF grid */
    double prior_slope;         /* d logit(VMAF) / d CRF before two points exist */
    int floor;                  /* 1: never below target; 0: closest */
} tv_params;

typedef enum {
    TV_CONVERGED,               /* a score inside the window */
    TV_BRACKETED,               /* two adjacent grid points straddle it */
    TV_EXCEEDED,                /* the worst-quality bound still beats it */
    TV_UNREACHABLE,             /* the best-quality bound still misses it */
    TV_PLATEAU,                 /* spending bits stopped buying quality (a VBV cap) */
} tv_status;

typedef struct {
    double crf;
    tv_score s;
    tv_status status;
    int trials;
} tv_result;

void tv_params_default(tv_params *p);
const char *tv_status_name(tv_status s);
int tv_search(const tv_params *p, tv_score_fn fn, void *ctx, tv_result *out);

/* the external VMAF chain: v1 as scripts/parity_long.py scores it, or
 * HDR-VMAF as scripts/hdr_bd.py does */
typedef struct {
    const char *vmaf_bin, *ffmpeg_bin, *model_dir;
    int threads;
    int w, h, enc_depth;
    double fps;
    int hdr, hlg;
} tv_scorer;

/* writes the reference as a y4m stream; 0 or -1 */
typedef int (*tv_ref_fn)(void *ctx, FILE *to);

const char *tv_model_name(const tv_scorer *s);
int tv_upscaled(int w, int h);
int tv_model_path(const tv_scorer *s, char *buf, size_t n);
/* pooled v1 mean of `bitstream` against the reference `ref` writes;
 * `wd` is a private scratch directory */
int tv_score_stream(const tv_scorer *s, const char *wd, const char *bitstream,
                    tv_ref_fn ref, void *ref_ctx, double *vmaf, long *frames);

#endif
