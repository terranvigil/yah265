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

/* `key` names the MILESTONE this wait is on, and it is the caller's
 * promise that the only progress which can make `pred` true is published
 * under the same key (phase 13 C4). The pool hashes it into a fixed
 * channel table and wakes that channel alone; a hash collision costs a
 * spurious wake and nothing else, and a key that does not match its
 * publisher's is a hang, not a wrong answer. */
void ntp_wait(ntp_pool *p, unsigned key, ntp_pred_fn pred, void *ctx);

/* The CROSS-FRAME form: block until `pred(ctx)` is true, running ready
 * tasks of strictly older jobs for as long as it is not. Safe to call
 * from the driver, where it degenerates to a plain park because the
 * driver runs no tasks. */
void ntp_wait_older(ntp_pool *p, ntp_pred_fn pred, void *ctx);

/* Inside a running task: publish progress and wake what can use it. The
 * publish itself is the caller's (an atomic store); these only wake.
 *
 * Two entry points, because the two wake channels cost very different
 * amounts and the caller knows which one it needs. ntp_publish is for
 * progress that can only satisfy a MILESTONE predicate - the per-CTB
 * publish inside a row - and it wakes milestone waiters only.
 * ntp_publish_task is for progress that can also make a TASK claimable -
 * a row watermark, or the CTB that puts the row below over its lag - and
 * it wakes one idle worker as well. Calling ntp_publish where
 * ntp_publish_task is needed loses parallelism (a task sits claimable
 * with nobody awake to take it until the next task completes); calling
 * ntp_publish_task everywhere is correct but pays a wake per CTB.
 * Neither can change what a task reads, so neither can change bits. */
/* The key a publish that can satisfy NO milestone predicate passes -
 * the row watermarks, which only make tasks claimable. */
#define NTP_KEY_NONE ((unsigned)-1)
void ntp_publish(ntp_pool *p, unsigned key);
void ntp_publish_task(ntp_pool *p, unsigned key);

/* Phase 9 step 4's instrument: per-worker occupancy, output-neutral.
 * Off unless enabled, and when off it costs one predictable branch per
 * TASK (not per CTB) and nothing at all on the publish path. Nothing
 * here is ever read by a readiness predicate, so it cannot change bits.
 *
 * Enable before the first submit. The read returns seconds summed over
 * workers: `busy` inside a top-level task, `stall` inside a milestone
 * wait that was not running a nested task, `park` asleep with nothing
 * claimable, and `wall` since ntp_open. stall is a SUBSET of busy - a
 * worker waiting on the row above still holds its task - so the work
 * actually done is busy - stall. */
void ntp_prof_enable(ntp_pool *p);
/* C4's wake census: publishes, the subset that reached the lock, and
 * milestone waits with the subset that parked. Instrument only. */
void ntp_prof_counts(const ntp_pool *p, long *pub, long *bcast,
                     long *waits, long *blocked);
void ntp_prof_read(const ntp_pool *p, double *busy, double *stall,
                   double *stall_ref, double *park, double *allpark,
                   double *wall, long *tasks);

#endif
