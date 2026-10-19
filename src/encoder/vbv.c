/*
 * The hypothetical decoder buffer: the fill arithmetic. See vbv.h for the
 * model and for why the fill is not clamped at zero.
 * Copyright (c) 2026, the yah265 authors. GPL-2.0-or-later; see LICENSE.
 */

#include "encoder/vbv.h"

void n265_vbv_init(n265_vbv *v, double maxrate, double bufsize, double init,
                   double fps, double safe, int lookahead, double reserve)
{
    v->on = maxrate > 0.0 && bufsize > 0.0;
    v->maxrate = maxrate;
    v->bufsize = bufsize;
    v->init = init;
    v->frame_dur = fps > 0.0 ? 1.0 / fps : 0.0;
    v->inflow = maxrate * v->frame_dur;
    v->safe = safe;
    v->lookahead = lookahead;
    v->reserve = reserve;
    /* The buffer is `init` full at the first removal, which is the same
     * statement as "the decoder waited init * bufsize / maxrate seconds
     * before decoding the first picture" - and it is the number the
     * buffering period SEI would carry as initial_cpb_removal_delay if
     * this encoder wrote one (see docs/phase12.md, Item 2). */
    v->fill = init * bufsize;
    if (v->fill > bufsize)
        v->fill = bufsize;
    v->min_fill = v->fill;
    v->underflows = 0;
    v->clamped = 0;
    /* --max-ausize-factor's ceiling is set by the CALLER after this
     * returns, so it is cleared here: every field of the struct has to be
     * written by this function or a caller with one on the stack (the
     * unit test) inherits whatever was there. */
    v->au_cap = 0.0;
    v->seg_left = -1;
    v->seg_floor = 0.0;
}

double n265_vbv_allow(const n265_vbv *v, const double *future, int n)
{
    double lim = v->fill;               /* k = 0: this removal alone */
    /* --max-ausize-factor. It bounds the SINGLE access unit and nothing
     * else, so it lands before the cumulative window bound and can only
     * tighten it: an oversized AU is a hard failure in a hardware decoder
     * whatever the buffer says about the frames around it. */
    if (v->au_cap > 0.0 && lim > v->au_cap)
        lim = v->au_cap;
    if (v->lookahead && future) {
        double cum = 0.0;
        double w = lim;
        for (int k = 1; k <= n; k++) {
            cum += future[k - 1];
            double l = v->fill + (double)k * v->inflow - cum;
            if (l < w)
                w = l;
        }
        /* The reservation has a FLOOR, and it is not decoration. The window's
         * plans are estimates of frames nobody has coded, and where the
         * content turns over they can be wrong by two orders of magnitude in
         * the expensive direction: sintel_720p's black opening is priced at
         * 200 kbit a frame against an 826 kbit buffer, twenty frames of it
         * sum to five buffers, and without this the allowance floors and
         * thirteen frames are coded at QP 51 to save for frames that cost
         * 312 bits each. The k = 0 bound is a FACT about the buffer and is
         * never floored; this one is a forecast, and a forecast may not take
         * more than `reserve` of what the buffer actually holds. */
        double lo = v->reserve * v->fill;
        if (w < lo)
            w = lo;
        if (w < lim)
            lim = w;
    }
    /* The segment's exit (vbv.h). The removals between here and the
     * segment's end are the window's own plans, and the join wants
     * `seg_floor` left over after them, so this is the cumulative bound
     * at k = seg_left with the floor as one more frame to pay for. It is
     * NOT floored the way the window's forecast is: a join that lands
     * under the floor is an underflow in the next segment's opening IDR,
     * and the next segment cannot see it. Only once the end is inside the
     * window - beyond it the plans that would price it are not there. */
    if (v->seg_left >= 0 && v->seg_floor > 0.0 &&
        (v->seg_left == 0 || (future && v->seg_left <= n))) {
        double cum = 0.0;
        for (int k = 0; k < v->seg_left; k++)
            cum += future[k];
        double l = v->fill + (double)(v->seg_left + 1) * v->inflow
                 - v->seg_floor - cum;
        if (l < lim)
            lim = l;
    }
    lim *= v->safe;
    if (!(lim > N265_VBV_MIN_ALLOW))    /* also catches a NaN plan */
        lim = N265_VBV_MIN_ALLOW;
    return lim;
}

void n265_vbv_update(n265_vbv *v, double bits, double *fill_before,
                     double *fill_after)
{
    if (fill_before)
        *fill_before = v->fill;
    v->fill -= bits;
    if (v->fill < v->min_fill)
        v->min_fill = v->fill;
    if (v->fill < 0.0)
        v->underflows++;
    v->fill += v->inflow;
    if (v->fill > v->bufsize)
        v->fill = v->bufsize;
    if (fill_after)
        *fill_after = v->fill;
}
