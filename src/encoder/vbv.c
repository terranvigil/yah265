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
}

double n265_vbv_allow(const n265_vbv *v, const double *future, int n)
{
    double lim = v->fill;               /* k = 0: this removal alone */
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
