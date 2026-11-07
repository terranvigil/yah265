/*
 * Film grain: estimating a model from the source, and the denoise that
 * pairs with it. The SEI SYNTAX is headers.c's, with the rest of the
 * high-level syntax; what lives here is the measurement.
 *
 * Copyright (c) 2026, the yah265 authors. GPL-2.0-or-later; see LICENSE.
 */

#ifndef N265_GRAIN_H
#define N265_GRAIN_H

#include <stdint.h>

#include "common/pixel.h"

/* Intensity bins the estimate is taken in, before merging. 16 bins of 16
 * levels: fine enough that a clip whose grain lives in the shadows is not
 * averaged against its highlights, coarse enough that every bin on a CIF
 * frame holds hundreds of 8x8 blocks. */
#define N265_FGC_BINS       16
/* Model values per interval. THREE: scale, horizontal cut-off, vertical
 * cut-off. The spec allows six; values 3..5 are the LOW cut-off pair and
 * an aspect term, they default to 0 when absent, and the only synthesizer
 * in this project's measurement chain reads none of them (docs/phase12.md,
 * step 0). Writing three instead of six is 24 fewer bits per interval for
 * bit-identical synthesis. */
#define N265_FGC_NVALS      3
/* The largest grain std, in 8-bit luma levels, the writer will signal.
 * Past this the model is not describing grain any more. */
#define N265_FGC_SIGMA_MAX  24.0
/* The RBSP a finished SEI can need: three components, sixteen intervals
 * each, two bound bytes and three se(v) per interval, plus the message
 * header and the trailing bits, rounded well up. Fixed rather than
 * allocated because it sits in the per-frame work item. */
#define N265_FGC_SEI_MAX    1024

/* The model, as it goes into the SEI. Intervals are non-overlapping and
 * cover 0..255 in order, which is what lets a decoder take the first one
 * that contains a block's mean. */
typedef struct n265_grain_model {
    int present;                    /* any component carries grain */
    int log2_scale;                 /* log2_scale_factor */
    int comp[3];                    /* comp_model_present_flag */
    int nival[3];
    uint8_t lo[3][N265_FGC_BINS];
    uint8_t hi[3][N265_FGC_BINS];
    int16_t scale[3][N265_FGC_BINS];
    int8_t  hcut[3][N265_FGC_BINS];
    int8_t  vcut[3][N265_FGC_BINS];
    /* what the estimate SAID, before it was turned into syntax: the grain
     * std per bin in 8-bit levels, which is also what the denoise cores
     * against. Indexed by the raw bin, not by the merged interval. */
    float sigma[3][N265_FGC_BINS];
} n265_grain_model;

/* The accumulator. One per encode, on the driver; every number in it is
 * an integer sum taken in display order, so the model is a pure function
 * of the source and of nothing else. */
typedef struct n265_grain_est n265_grain_est;

n265_grain_est *n265_grain_open(int w, int h, int csx, int csy);
void n265_grain_close(n265_grain_est *g);

/* Fold one source picture into the estimate. `plane` is the padded source
 * and `w`/`h` the DISPLAYED luma size; the pad is not read. */
void n265_grain_accum(n265_grain_est *g, const pixel *const plane[3],
                      const int stride[3]);

/* Turn the accumulation into a model. `strength` scales the estimated
 * sigma (the --film-grain argument); `chroma` enables the chroma
 * components. Sets m->present = 0 when nothing clears the floor. */
void n265_grain_finish(const n265_grain_est *g, n265_grain_model *m,
                       double strength, int chroma);

/* Core the picture against the model, in place, over the displayed area
 * only; the caller re-replicates the pad. This is --film-grain-denoise:
 * the samples the SEI promises to put back are the ones taken out here. */
void n265_grain_denoise(const n265_grain_model *m, pixel *plane,
                        int stride, int w, int h, int comp);

/* Knobs (docs/knobs.md). Accessors, not inline getenv: a warm can only
 * reach a gate through a callable function (scripts/env_gate_audit.py). */
int n265_fgc_pct(void);         /* N265_FGC_PCT: the flat-block percentile */
int n265_fgc_frames(void);      /* N265_FGC_FRAMES: pictures estimated on */
int n265_fgc_chroma(void);      /* N265_FGC_CHROMA: signal chroma grain */
double n265_fgc_core(void);     /* N265_FGC_CORE: denoise threshold, in sigma */
double n265_fgc_ratio(void);    /* N265_FGC_RATIO: bin-to-quietest-bin cap */
double n265_fgc_min(void);      /* N265_FGC_MIN: the no-SEI floor, 8-bit levels */
int n265_fgc_trace(void);       /* N265_FGC_TRACE: print the model */

#endif
