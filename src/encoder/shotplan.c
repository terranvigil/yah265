/*
 * --shot-plan: one base-QP offset per shot, from the scan's features.
 * Copyright (c) 2026, the yah265 authors. GPL-2.0-or-later; see LICENSE.
 *
 * The target is constant slope: each shot sits where one more bit buys
 * the same frame-weighted VMAF v1 as in every other shot of the title.
 * The model is a ridge fit to oracle offsets solved from per-shot rate
 * and v1 curves (docs/shot-plan-design.md). It reads each feature as the
 * shot's distance from the title's frame-weighted mean, so it says how a
 * shot differs from its title and never moves the title as a whole.
 */

#include <math.h>
#include <stdio.h>
#include <stdarg.h>

#include "encoder/shotplan.h"

/* The scan's features (shotlist.c writes them in this order):
 *   0 mean log2 intra cost per lowres block
 *   1 mean log2 best (intra or inter) cost per block
 *   2 inter share: best cost over intra cost, first frame left out
 *   3 log2 of 1 + mean motion length
 *   4 frame-to-frame spread of feature 1
 *   5 spread of log2 block variance (the AQ field's spread)
 *   6 mean log2 block variance
 *   7 flat share (block variance under 4)
 *   8 dark-smooth share (mean under 64, variance under 16)
 *   9 mean luma, 0..1
 *  10 log2 shot length in frames
 * Each term is coef * (f - title mean) / scale. Model 1 reads 0, 1, 2
 * and 7; the other terms and a slope in the CRF earned nothing out of
 * sample. */
static const double SCALE[N265_SHOTFEAT_N] = {
    0.650348909, 0.803670808, 0.0796842934, 0.746521206, 0.176524356,
    0.540853096, 1.64422019, 0.179737988, 0.174192513, 0.1087615,
    0.993845098,
};
static const double COEF[N265_SHOTFEAT_N] = {
    0.608911295, 1.18011215, 0.148969267, 0, 0, 0, 0, 1.03336604, 0, 0, 0,
};

static void seterr(char *err, size_t cap, const char *fmt, ...)
{
    if (!err || !cap)
        return;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(err, cap, fmt, ap);
    va_end(ap);
}

static double clampd(double v, double lo, double hi)
{
    return v < lo ? lo : v > hi ? hi : v;
}

static double frames_of(const n265_shot *s)
{
    return (double)(s->last - s->first + 1);
}

int n265_shotplan(const n265_shotlist *l, double *off,
                  char *err, size_t cap)
{
    int nqp = 0, nfeat = 0;
    for (int k = 0; k < l->n; k++) {
        nqp += l->s[k].has_qp;
        nfeat += l->s[k].has_feat;
    }
    if (nqp == l->n) {
        for (int k = 0; k < l->n; k++)
            off[k] = l->s[k].qp;
        return 0;
    }
    if (nqp) {
        seterr(err, cap, "%d of %d shots give qp=; give it on every shot "
               "or on none", nqp, l->n);
        return -1;
    }
    if (nfeat != l->n || l->feat_ver != N265_SHOTFEAT_VER) {
        seterr(err, cap, "the list carries no features this build reads; "
               "make it with --shot-scan");
        return -1;
    }
    double mean[N265_SHOTFEAT_N] = { 0 }, tot = 0.0;
    for (int k = 0; k < l->n; k++) {
        double w = frames_of(&l->s[k]);
        tot += w;
        for (int j = 0; j < N265_SHOTFEAT_N; j++)
            mean[j] += w * l->s[k].feat[j];
    }
    for (int j = 0; j < N265_SHOTFEAT_N; j++)
        mean[j] /= tot;
    for (int k = 0; k < l->n; k++) {
        double d = 0.0;
        for (int j = 0; j < N265_SHOTFEAT_N; j++) {
            double z = (l->s[k].feat[j] - mean[j]) / SCALE[j];
            d += COEF[j] * z;
        }
        off[k] = clampd(d, -N265_SHOTPLAN_CLAMP, N265_SHOTPLAN_CLAMP);
    }
    /* a short shot is too few frames to price; it rides with its
     * neighbour, the one before it, or the one after for the first */
    for (int k = 1; k < l->n; k++)
        if (frames_of(&l->s[k]) < N265_SHOTPLAN_MINLEN)
            off[k] = off[k - 1];
    if (l->n > 1 && frames_of(&l->s[0]) < N265_SHOTPLAN_MINLEN) {
        int k = 1;
        while (k < l->n - 1 && frames_of(&l->s[k]) < N265_SHOTPLAN_MINLEN)
            k++;
        off[0] = off[k];
    }
    double m = 0.0;
    for (int k = 0; k < l->n; k++)
        m += frames_of(&l->s[k]) * off[k];
    m /= tot;
    for (int k = 0; k < l->n; k++)
        off[k] = clampd(off[k] - m, -N265_SHOTPLAN_CLAMP, N265_SHOTPLAN_CLAMP);
    return 0;
}
