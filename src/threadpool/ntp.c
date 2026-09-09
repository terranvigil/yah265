/*
 * Worker pool implementation. See ntp.h for the contract.
 *
 * Shape: one worker set created at encoder open, several live jobs in
 * dispatch order. A worker claims the lowest-indexed pending task whose
 * predicate says ready, in the OLDEST job that has one, runs it on a
 * lane from the free-list, marks it done and broadcasts. Claim order is
 * oldest job first then lowest task index, which is the encoder's own
 * priority order (frame k before frame k+1, row 0 before row 1, encode
 * before filter), and it cannot change bits because readiness decides
 * WHEN a task runs, never what it reads.
 *
 * Copyright (c) 2026, the yah265 authors. GPL-2.0-or-later; see LICENSE.
 */

#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "threadpool/ntp.h"

/* Bounded spin before sleeping, in nanoseconds. A wavefront's waits are
 * short (the row above is two CTBs ahead, not two frames), so the wake
 * cost dominates unless the tail of the wait is spun. */
#define NTP_SPIN_NS 25000

enum { T_PENDING = 0, T_RUNNING, T_DONE };

struct jslot {
    ntp_job *job;
    unsigned char *state;
    int cap;
    int ndone;
    int active;
    uint64_t seq;               /* dispatch order; 0 is "the driver" */
};

struct ntp_pool {
    pthread_t *th;
    int nthreads;
    pthread_mutex_t mu;
    pthread_cond_t work;        /* a task may have become claimable */
    pthread_cond_t done;        /* a task finished (job progress, waits) */
    int quit;

    struct jslot *js;
    int njs;
    uint64_t next_seq;

    /* lane free-list. A nested run (ntp_wait_older) needs a second lane,
     * because the outer task still owns its row scratch. */
    int nlanes;
    int *lane_free;
    int nlane_free;

    /* How many WORKERS are parked: on `work` with nothing claimable, or
     * on `done` inside a milestone wait. A progress publish that nobody
     * is parked for is a mutex round trip for nothing, and the row
     * wavefront publishes once per CTB. The driver is deliberately not
     * counted - it is woken by the task-completion broadcast, which
     * always takes the lock, and counting it would keep `parked` at 1
     * for the whole job and defeat the check. */
    atomic_int parked;
};

/* The job this thread is currently running a task of, in dispatch order.
 * 0 means "not inside a task" (the driver), and since every live job has
 * seq >= 1, a driver-side ntp_wait_older claims nothing. */
static _Thread_local uint64_t tls_seq;

static void *worker(void *arg);

ntp_pool *ntp_open(int nthreads, int max_jobs, int max_lanes)
{
    if (nthreads < 1)
        nthreads = 1;
    if (max_jobs < 1)
        max_jobs = 1;
    if (max_lanes < nthreads)
        max_lanes = nthreads;
    ntp_pool *p = calloc(1, sizeof(*p));
    if (!p)
        return NULL;
    p->nthreads = nthreads;
    p->next_seq = 1;
    pthread_mutex_init(&p->mu, NULL);
    pthread_cond_init(&p->work, NULL);
    pthread_cond_init(&p->done, NULL);
    p->njs = max_jobs;
    p->js = calloc((size_t)max_jobs, sizeof(*p->js));
    p->nlanes = max_lanes;
    p->lane_free = calloc((size_t)max_lanes, sizeof(int));
    p->th = calloc((size_t)nthreads, sizeof(pthread_t));
    if (!p->js || !p->lane_free || !p->th) {
        p->nthreads = 0;
        ntp_close(p);
        return NULL;
    }
    /* highest index first, so the stack hands out 0, 1, 2 ... in order */
    for (int i = 0; i < max_lanes; i++)
        p->lane_free[i] = max_lanes - 1 - i;
    p->nlane_free = max_lanes;
    for (int i = 0; i < nthreads; i++) {
        ntp_pool **a = malloc(sizeof(*a));
        if (!a) {
            p->nthreads = i;
            ntp_close(p);
            return NULL;
        }
        *a = p;
        if (pthread_create(&p->th[i], NULL, worker, a) != 0) {
            free(a);
            p->nthreads = i;
            ntp_close(p);
            return NULL;
        }
    }
    return p;
}

void ntp_close(ntp_pool *p)
{
    if (!p)
        return;
    pthread_mutex_lock(&p->mu);
    p->quit = 1;
    pthread_cond_broadcast(&p->work);
    pthread_mutex_unlock(&p->mu);
    for (int i = 0; i < p->nthreads; i++)
        pthread_join(p->th[i], NULL);
    pthread_mutex_destroy(&p->mu);
    pthread_cond_destroy(&p->work);
    pthread_cond_destroy(&p->done);
    for (int i = 0; p->js && i < p->njs; i++)
        free(p->js[i].state);
    free(p->js);
    free(p->lane_free);
    free(p->th);
    free(p);
}

int ntp_threads(const ntp_pool *p)
{
    return p ? p->nthreads : 1;
}

int ntp_lanes(const ntp_pool *p)
{
    return p ? p->nlanes : 1;
}

/* mu held */
static int lane_take(ntp_pool *p)
{
    if (p->nlane_free <= 0)
        return 0;               /* cannot happen: max_lanes bounds nesting */
    return p->lane_free[--p->nlane_free];
}

/* mu held */
static void lane_give(ntp_pool *p, int lane)
{
    if (p->nlane_free < p->nlanes)
        p->lane_free[p->nlane_free++] = lane;
}

/* mu held. The lowest-indexed ready pending task of the OLDEST live job
 * with seq < limit, or NULL. limit == UINT64_MAX means "any job". */
static struct jslot *claim(ntp_pool *p, uint64_t limit, int *out_idx)
{
    struct jslot *best = NULL;
    for (int i = 0; i < p->njs; i++) {
        struct jslot *s = &p->js[i];
        if (!s->active || s->seq >= limit)
            continue;
        if (best && s->seq > best->seq)
            continue;
        ntp_job *j = s->job;
        for (int k = 0; k < j->ntask; k++)
            if (s->state[k] == T_PENDING && j->ready(j->ctx, k)) {
                best = s;
                *out_idx = k;
                break;
            }
    }
    if (best)
        best->state[*out_idx] = T_RUNNING;
    return best;
}

/* mu held on entry and on return. Runs the task with the lock dropped. */
static void run_task(ntp_pool *p, struct jslot *js, int idx)
{
    int lane = lane_take(p);
    ntp_job *j = js->job;
    uint64_t saved = tls_seq;
    tls_seq = js->seq;
    pthread_mutex_unlock(&p->mu);
    j->run(j->ctx, lane, idx);
    pthread_mutex_lock(&p->mu);
    tls_seq = saved;
    js->state[idx] = T_DONE;
    js->ndone++;
    lane_give(p, lane);
    /* a finished task can unblock both a claimable task and an in-task
     * milestone wait */
    pthread_cond_broadcast(&p->work);
    pthread_cond_broadcast(&p->done);
}

static void *worker(void *arg)
{
    ntp_pool *p = *(ntp_pool **)arg;
    free(arg);

    pthread_mutex_lock(&p->mu);
    for (;;) {
        if (p->quit)
            break;
        /* Park BEFORE testing readiness, so this and ntp_publish are the
         * two halves of a store-load pair: whichever of the two seq_cst
         * operations comes first in the total order, one side sees the
         * other, and no wake can be lost. */
        atomic_fetch_add(&p->parked, 1);
        int idx = 0;
        struct jslot *js = claim(p, UINT64_MAX, &idx);
        if (!js) {
            pthread_cond_wait(&p->work, &p->mu);
            atomic_fetch_sub(&p->parked, 1);
            continue;
        }
        atomic_fetch_sub(&p->parked, 1);
        run_task(p, js, idx);
    }
    pthread_mutex_unlock(&p->mu);
    return NULL;
}

void ntp_submit(ntp_pool *p, ntp_job *job)
{
    pthread_mutex_lock(&p->mu);
    struct jslot *js = NULL;
    for (int i = 0; i < p->njs; i++)
        if (!p->js[i].active) {
            js = &p->js[i];
            break;
        }
    if (js && job->ntask > js->cap) {
        unsigned char *s = realloc(js->state, (size_t)job->ntask);
        if (!s)
            js = NULL;
        else {
            js->state = s;
            js->cap = job->ntask;
        }
    }
    if (!js) {      /* max_jobs contract broken, or out of memory: run it
                     * inline on this thread, which is still correct */
        pthread_mutex_unlock(&p->mu);
        for (int i = 0; i < job->ntask; i++)
            job->run(job->ctx, 0, i);
        job->impl = NULL;
        return;
    }
    memset(js->state, T_PENDING, (size_t)job->ntask);
    js->ndone = 0;
    js->job = job;
    js->seq = p->next_seq++;
    js->active = 1;
    job->impl = js;
    pthread_cond_broadcast(&p->work);
    pthread_mutex_unlock(&p->mu);
}

void ntp_join(ntp_pool *p, ntp_job *job)
{
    struct jslot *js = job->impl;
    if (!js)
        return;                 /* ran inline at submit */
    pthread_mutex_lock(&p->mu);
    while (js->ndone < job->ntask)
        pthread_cond_wait(&p->done, &p->mu);
    js->active = 0;
    js->job = NULL;
    pthread_mutex_unlock(&p->mu);
    job->impl = NULL;
}

/* the bounded spin both wait forms share; 1 = the predicate came true */
static int spin_for(ntp_pred_fn pred, void *ctx)
{
    if (pred(ctx))
        return 1;
    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    for (;;) {
        if (pred(ctx))
            return 1;
        clock_gettime(CLOCK_MONOTONIC, &t1);
        long ns = (long)((t1.tv_sec - t0.tv_sec) * 1000000000L +
                         (t1.tv_nsec - t0.tv_nsec));
        if (ns >= NTP_SPIN_NS)
            return 0;
    }
}

void ntp_wait(ntp_pool *p, ntp_pred_fn pred, void *ctx)
{
    if (spin_for(pred, ctx))
        return;
    pthread_mutex_lock(&p->mu);
    atomic_fetch_add(&p->parked, 1);
    while (!pred(ctx))
        pthread_cond_wait(&p->done, &p->mu);
    atomic_fetch_sub(&p->parked, 1);
    pthread_mutex_unlock(&p->mu);
}

void ntp_wait_older(ntp_pool *p, ntp_pred_fn pred, void *ctx)
{
    if (spin_for(pred, ctx))
        return;
    uint64_t limit = tls_seq;   /* 0 on the driver: claims nothing */
    pthread_mutex_lock(&p->mu);
    for (;;) {
        if (pred(ctx))
            break;
        int idx = 0;
        struct jslot *js = limit ? claim(p, limit, &idx) : NULL;
        if (js) {
            run_task(p, js, idx);
            continue;
        }
        if (limit)
            atomic_fetch_add(&p->parked, 1);
        pthread_cond_wait(&p->done, &p->mu);
        if (limit)
            atomic_fetch_sub(&p->parked, 1);
    }
    pthread_mutex_unlock(&p->mu);
}

void ntp_publish(ntp_pool *p)
{
    /* Nobody is parked, so nobody can miss this. With one worker that is
     * always true - a task cannot become claimable to a thread that is
     * busy running this one, and no milestone wait can be outstanding -
     * so the whole per-CTB cost disappears at --threads 1, which is the
     * leg the ladder has to keep within noise of the serial encoder. */
    /* the publisher's half of the pair: its progress store is a release,
     * so a full fence is needed before reading `parked` or the two could
     * both miss each other */
    atomic_thread_fence(memory_order_seq_cst);
    if (atomic_load(&p->parked) == 0)
        return;
    pthread_mutex_lock(&p->mu);
    /* Both: progress unblocks in-task milestone waits (`done`) AND can
     * make the next task claimable (`work`). Waking only `done` here was
     * worth the whole wavefront - a row became claimable only when the
     * row above FINISHED, so twelve threads ran one row at a time. */
    pthread_cond_broadcast(&p->done);
    pthread_cond_broadcast(&p->work);
    pthread_mutex_unlock(&p->mu);
}
