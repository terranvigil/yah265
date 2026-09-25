/*
 * N265_TTRACE's buffer (ttrace.h). Empty unless the macro is defined.
 *
 * Copyright (c) 2026, the yah265 authors. GPL-2.0-or-later; see LICENSE.
 */

#include "encoder/ttrace.h"

#ifdef N265_TTRACE
#include <stdlib.h>
#include <time.h>

static tt_ev *tt_buf;
static atomic_long tt_n;
static long tt_cap;
static double tt_t0;
static atomic_int tt_nw;
static _Thread_local int tt_me = -1;

int tt_on(void) { return tt_buf != NULL; }

double tt_now(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (double)t.tv_sec + 1e-9 * (double)t.tv_nsec - tt_t0;
}

void tt_open(void)
{
    if (!getenv("N265_TASK_TRACE") || tt_buf)
        return;
    tt_cap = 8L << 20;
    tt_buf = calloc((size_t)tt_cap, sizeof(tt_ev));
    tt_t0 = 0.0;
    tt_t0 = tt_now();
}

void tt_put(int type, int kind, int frame, int row, int cx, int aux,
            double t0, double t1)
{
    if (tt_me < 0)
        tt_me = atomic_fetch_add(&tt_nw, 1);
    long i = atomic_fetch_add(&tt_n, 1);
    if (i >= tt_cap)
        return;
    tt_ev *v = &tt_buf[i];
    v->type = (unsigned char)type;
    v->kind = (unsigned char)kind;
    v->frame = frame;
    v->row = (short)row;
    v->cx = (short)cx;
    v->worker = (short)tt_me;
    v->aux = aux;
    v->t0 = t0;
    v->t1 = t1;
}

void tt_close(void)
{
    if (!tt_buf)
        return;
    FILE *f = fopen(getenv("N265_TASK_TRACE"), "w");
    long n = atomic_load(&tt_n);
    if (n > tt_cap)
        n = tt_cap;
    for (long i = 0; f && i < n; i++) {
        tt_ev *v = &tt_buf[i];
        fprintf(f, "%d %d %d %d %d %d %d %.7f %.7f\n", v->type, v->kind,
                v->frame, v->row, v->cx, v->worker, v->aux, v->t0, v->t1);
    }
    if (f)
        fclose(f);
    free(tt_buf);
    tt_buf = NULL;
}
#else
typedef int n265_ttrace_empty;
#endif
