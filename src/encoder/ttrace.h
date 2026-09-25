/*
 * N265_TTRACE: a compile-time task trace for the scheduler work of phase
 * 14 item 25. Built only with -DN265_TTRACE; the shipped build compiles
 * every macro here to nothing. With the macro on and N265_TASK_TRACE
 * naming a file, every task span, every CTB of every encode row, every
 * reference stall and every frame's dispatch and join is stamped into a
 * preallocated buffer and written out at close. It reads no state a
 * decision reads, so it cannot move a bit.
 *
 * Copyright (c) 2026, the yah265 authors. GPL-2.0-or-later; see LICENSE.
 */

#ifndef N265_TTRACE_H
#define N265_TTRACE_H

#ifdef N265_TTRACE
#include <stdatomic.h>
#include <stdio.h>

enum { TT_TASK = 1, TT_CTU, TT_RSTALL, TT_DISP, TT_JOIN, TT_REF, TT_LAG };

typedef struct {
    unsigned char type, kind;
    short row, cx, worker;
    int frame, aux;
    double t0, t1;
} tt_ev;

void tt_open(void);
void tt_close(void);
int tt_on(void);
double tt_now(void);
void tt_put(int type, int kind, int frame, int row, int cx, int aux,
            double t0, double t1);

#define TT_NOW() (tt_on() ? tt_now() : 0.0)
#define TT_PUT(ty, k, f, r, c, a, t0, t1) \
    do { if (tt_on()) tt_put(ty, k, f, r, c, a, t0, t1); } while (0)
#else
#define TT_NOW() 0.0
#define TT_PUT(ty, k, f, r, c, a, t0, t1) do { (void)(t0); (void)(t1); } while (0)
#endif

#endif
