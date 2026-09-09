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
 * Several jobs are live at once (T2, gate decision G3). They are
 * registered in DISPATCH order and claimed oldest-job-first, which is
 * the encoder's own priority order. A task that has to wait on another
 * FRAME's progress calls ntp_wait_older, which runs ready tasks of
 * strictly older jobs rather than parking the worker: waits point
 * backwards in dispatch order, the oldest live job has no such wait it
 * cannot satisfy from its own rows, so the oldest always progresses and
 * the pool cannot deadlock. Nesting is bounded by the number of live
 * jobs, and lanes come from a free-list rather than the worker index,
 * because a nested task must not reuse the outer task's scratch.
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

/* Run task `idx` on lane `lane` (0 .. ntp_lanes-1). Called with no lock
 * held. May block on a milestone via ntp_wait / ntp_wait_older. */
typedef void (*ntp_run_fn)(void *ctx, int lane, int idx);

typedef struct ntp_job {
    void *ctx;
    int ntask;
    ntp_ready_fn ready;
    ntp_run_fn run;
    /* set by ntp_submit */
    void *impl;
} ntp_job;

/* nthreads >= 1. `max_jobs` jobs may be live at once; `max_lanes` scratch
 * lanes, which must be at least nthreads * max_jobs because that bounds
 * the nesting depth. Returns NULL on failure. */
ntp_pool *ntp_open(int nthreads, int max_jobs, int max_lanes);
void ntp_close(ntp_pool *p);
int ntp_threads(const ntp_pool *p);
int ntp_lanes(const ntp_pool *p);

/* Register `job` and return at once. The caller must ntp_join it later
 * and must not exceed max_jobs live registrations. Registration order is
 * the dispatch order the claim and the nested-run rule use. */
void ntp_submit(ntp_pool *p, ntp_job *job);

/* Wait for every task of `job`, then unregister it. The calling thread
 * does NOT execute tasks: it is the driver, and letting it run work
 * would make `--threads 1` two threads wide. */
void ntp_join(ntp_pool *p, ntp_job *job);

/* Inside a running task: block until `pred(ctx)` is true. Bounded
 * spin-then-sleep. The predicate must become true through work another
 * task is already committed to doing, or this deadlocks - the caller
 * guarantees that by only ever claiming a task whose producer is
 * running or finished. This is the IN-FRAME form (the row wavefront). */
typedef int (*ntp_pred_fn)(void *ctx);
void ntp_wait(ntp_pool *p, ntp_pred_fn pred, void *ctx);

/* The CROSS-FRAME form: block until `pred(ctx)` is true, running ready
 * tasks of strictly older jobs for as long as it is not. Safe to call
 * from the driver, where it degenerates to a plain park because the
 * driver runs no tasks. */
void ntp_wait_older(ntp_pool *p, ntp_pred_fn pred, void *ctx);

/* Inside a running task: publish progress and wake anything waiting.
 * The publish itself is the caller's (an atomic store); this only wakes. */
void ntp_publish(ntp_pool *p);

#endif
