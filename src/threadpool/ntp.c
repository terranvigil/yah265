/*
 * Worker pool implementation. See ntp.h for the contract.
 *
 * Shape: one worker set created at encoder open, one job at a time in
 * flight per caller. A worker claims the lowest-indexed pending task
 * whose predicate says ready, runs it on its own lane, marks it done and
 * broadcasts. Claim order is lowest index first, which is the encoder's
 * own priority order (row 0 before row 1, encode before filter), and it
 * cannot change bits because readiness decides WHEN a task runs, never
 * what it reads.
 *
 * Copyright (c) 2026, the yah265 authors. GPL-2.0-or-later; see LICENSE.
 */

#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "threadpool/ntp.h"

/* Bounded spin before sleeping, in nanoseconds. A wavefront's waits are
 * short (the row above is two CTBs ahead, not two frames), so the wake
 * cost dominates unless the tail of the wait is spun. */
#define NTP_SPIN_NS 25000

enum { T_PENDING = 0, T_RUNNING, T_DONE };

struct ntp_pool {
    pthread_t *th;
    int nthreads;
    pthread_mutex_t mu;
    pthread_cond_t work;        /* a task may have become claimable */
    pthread_cond_t done;        /* a task finished (job progress, waits) */
    int quit;

    /* the single job slot: one driver at a time */
    ntp_job *job;
    unsigned char *state;       /* ntask entries */
    int state_cap;
    int ndone;
    /* How many WORKERS are parked: on `work` with nothing claimable, or
     * on `done` inside a milestone wait. A progress publish that nobody
     * is parked for is a mutex round trip for nothing, and the row
     * wavefront publishes once per CTB. The driver is deliberately not
     * counted - it is woken by the task-completion broadcast, which
     * always takes the lock, and counting it would keep `parked` at 1
     * for the whole job and defeat the check. */
    atomic_int parked;
};

static void *worker(void *arg);

ntp_pool *ntp_open(int nthreads)
{
    if (nthreads < 1)
        nthreads = 1;
    ntp_pool *p = calloc(1, sizeof(*p));
    if (!p)
        return NULL;
    p->nthreads = nthreads;
    pthread_mutex_init(&p->mu, NULL);
    pthread_cond_init(&p->work, NULL);
    pthread_cond_init(&p->done, NULL);
    p->th = calloc((size_t)nthreads, sizeof(pthread_t));
    if (!p->th) {
        free(p);
        return NULL;
    }
    for (int i = 0; i < nthreads; i++) {
        /* the lane index is the worker's identity for its whole life, so
         * a row's scratch buffers are lane-local with no allocation on
         * the task path */
        long lane = i;
        struct { ntp_pool *p; long lane; } *a = malloc(sizeof(*a));
        if (!a) {
            p->nthreads = i;
            ntp_close(p);
            return NULL;
        }
        a->p = p;
        a->lane = lane;
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
    free(p->state);
    free(p->th);
    free(p);
}

int ntp_threads(const ntp_pool *p)
{
    return p ? p->nthreads : 1;
}

/* mu held. Lowest-indexed pending task whose predicate is ready, or -1. */
static int claim(ntp_pool *p)
{
    ntp_job *j = p->job;
    if (!j)
        return -1;
    for (int i = 0; i < j->ntask; i++)
        if (p->state[i] == T_PENDING && j->ready(j->ctx, i)) {
            p->state[i] = T_RUNNING;
            return i;
        }
    return -1;
}

static void *worker(void *arg)
{
    struct { ntp_pool *p; long lane; } *a = arg;
    ntp_pool *p = a->p;
    int lane = (int)a->lane;
    free(a);

    pthread_mutex_lock(&p->mu);
    for (;;) {
        if (p->quit)
            break;
        /* Park BEFORE testing readiness, so this and ntp_publish are the
         * two halves of a store-load pair: whichever of the two seq_cst
         * operations comes first in the total order, one side sees the
         * other, and no wake can be lost. */
        atomic_fetch_add(&p->parked, 1);
        int idx = claim(p);
        if (idx < 0) {
            pthread_cond_wait(&p->work, &p->mu);
            atomic_fetch_sub(&p->parked, 1);
            continue;
        }
        atomic_fetch_sub(&p->parked, 1);
        ntp_job *j = p->job;
        pthread_mutex_unlock(&p->mu);
        j->run(j->ctx, lane, idx);
        pthread_mutex_lock(&p->mu);
        p->state[idx] = T_DONE;
        p->ndone++;
        /* a finished task can unblock both a claimable task and an
         * in-task milestone wait */
        pthread_cond_broadcast(&p->work);
        pthread_cond_broadcast(&p->done);
    }
    pthread_mutex_unlock(&p->mu);
    return NULL;
}

void ntp_run_job(ntp_pool *p, ntp_job *job)
{
    pthread_mutex_lock(&p->mu);
    if (job->ntask > p->state_cap) {
        unsigned char *s = realloc(p->state, (size_t)job->ntask);
        if (!s) {           /* out of memory: run it inline, still correct */
            pthread_mutex_unlock(&p->mu);
            for (int i = 0; i < job->ntask; i++)
                job->run(job->ctx, 0, i);
            return;
        }
        p->state = s;
        p->state_cap = job->ntask;
    }
    memset(p->state, T_PENDING, (size_t)job->ntask);
    p->ndone = 0;
    p->job = job;
    pthread_cond_broadcast(&p->work);
    while (p->ndone < job->ntask)
        pthread_cond_wait(&p->done, &p->mu);
    p->job = NULL;
    pthread_mutex_unlock(&p->mu);
}

void ntp_wait(ntp_pool *p, ntp_pred_fn pred, void *ctx)
{
    if (pred(ctx))
        return;
    /* bounded spin: the producer is running and typically a CTB away */
    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    for (;;) {
        if (pred(ctx))
            return;
        clock_gettime(CLOCK_MONOTONIC, &t1);
        long ns = (long)((t1.tv_sec - t0.tv_sec) * 1000000000L +
                         (t1.tv_nsec - t0.tv_nsec));
        if (ns >= NTP_SPIN_NS)
            break;
    }
    pthread_mutex_lock(&p->mu);
    atomic_fetch_add(&p->parked, 1);
    while (!pred(ctx))
        pthread_cond_wait(&p->done, &p->mu);
    atomic_fetch_sub(&p->parked, 1);
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
