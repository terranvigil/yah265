/*
 * Worker pool: one shared worker set, a job table, gated wavefronts and
 * milestones (docs/threading-design.md sec 1). No progress getters: a
 * caller can ask whether a task may run, never how far another one has
 * got, so analysis cannot consume a raced completion count.
 *
 * The unit is a JOB - a set of tasks that belong to one frame - and a
 * TASK inside it. A task runs when its readiness predicate says so; the
 * predicate is a pure function of published state, never of thread count
 * or timing, which is the whole determinism argument.
 *
 * Copyright (c) 2026, the yah265 authors. GPL-2.0-or-later; see LICENSE.
 */

#ifndef N265_NTP_H
#define N265_NTP_H

#include <stddef.h>

typedef struct ntp_pool ntp_pool;

/* May task `idx` of this job start? Called with the job lock held, so it
 * must be cheap and must not call back into the pool. */
typedef int (*ntp_ready_fn)(void *ctx, int idx);

/* Run task `idx` on worker lane `lane` (0 .. ntp_threads-1). Called with
 * no lock held. May block on a milestone via ntp_task_wait. */
typedef void (*ntp_run_fn)(void *ctx, int lane, int idx);

typedef struct ntp_job {
    void *ctx;
    int ntask;
    ntp_ready_fn ready;
    ntp_run_fn run;
    /* set by ntp_run_job */
    void *impl;
} ntp_job;

/* nthreads >= 1. Returns NULL on failure. */
ntp_pool *ntp_open(int nthreads);
void ntp_close(ntp_pool *p);
int ntp_threads(const ntp_pool *p);

/* Run every task of `job` on the pool and return when all are done. The
 * calling thread blocks; it does not execute tasks, so lane indices stay
 * bounded by the worker count. */
void ntp_run_job(ntp_pool *p, ntp_job *job);

/* Inside a running task: block until `pred(ctx)` is true. Bounded
 * spin-then-sleep. The predicate must become true through work another
 * task is already committed to doing, or this deadlocks - the caller
 * guarantees that by only ever claiming a task whose producer is
 * running or finished. */
typedef int (*ntp_pred_fn)(void *ctx);
void ntp_wait(ntp_pool *p, ntp_pred_fn pred, void *ctx);

/* Inside a running task: publish progress and wake anything waiting.
 * The publish itself is the caller's (an atomic store); this only wakes. */
void ntp_publish(ntp_pool *p);

#endif
