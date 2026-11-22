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
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "threadpool/ntp.h"

/* N265_NO_THREADS: a build with no threads at all (the WebAssembly
 * playground, where a worker needs SharedArrayBuffer). The pool starts no
 * worker and the thread that would have waited runs the claimable tasks
 * itself, in the same claim order, so the stream is the --threads 1 one. */

/* Bounded spin before sleeping, in nanoseconds. A wavefront's waits are
 * short (the row above is two CTBs ahead, not two frames), so the wake
 * cost dominates unless the tail of the wait is spun. */
#define NTP_SPIN_NS 25000

enum { T_PENDING = 0, T_RUNNING, T_DONE };

struct jslot {
    ntp_job *job;
    unsigned char *state;
    /* the lane a YIELDED task keeps (-1: none). A yielded task's scratch
     * is its state, so it resumes on the lane it left. */
    int *lane_of;
    int cap;
    int ndone;
    atomic_int ndone_pub;       /* ndone, for ntp_run's spin outside `mu` */
    int active;
    /* T3/3: the lowest index that is still T_PENDING. `claim` starts its
     * scan here instead of at 0, so the done prefix of a job - which is
     * most of it for most of a frame's life - costs nothing. Tasks
     * complete roughly in index order, so this is the whole rescan. */
    int first_pending;
    uint64_t seq;               /* dispatch order; 0 is "the driver" */
};

/* T3/3: a per-worker wake slot (sec 1: "per-worker wake slots (a
 * pool-wide broadcast cost 5%)"). A worker with nothing to claim sleeps
 * on ITS OWN condvar and is woken one at a time off a LIFO stack, so
 * making one task claimable wakes one thread rather than eighteen. */
struct wslot {
    pthread_cond_t cv;
    int woken;
};

/* C4: one milestone wake channel. `nwait` is the waiters parked on `cv`;
 * it is the publisher's half of the store-load pair, so it is seq_cst on
 * both sides. One cache line each: a publisher of channel i must not
 * dirty the line channel j's publisher is reading. */
struct nchan {
    pthread_cond_t cv;
    atomic_int nwait;
    char pad[64];
};

/* The table is a power of two and is indexed by a multiplicative hash of
 * the caller's key, because the keys the encoder hands us are dense in
 * the row index and sparse in the frame. 256 channels against at most
 * K x ctb_rows live milestones (8 x 34 on 1080p at CTB 32) keeps the
 * collision rate low enough that the census reads one waiter per wake. */
#define NTP_NCHAN 256
#define NTP_CHAN_MASK (NTP_NCHAN - 1)

static inline unsigned nchan_idx(unsigned key)
{
    return (key * 2654435761u >> 8) & NTP_CHAN_MASK;
}

/* Phase 9 step 4: per-worker occupancy, one cache line each so the
 * accumulators of two workers never share one. Written only by the
 * worker they belong to, read only after ntp_close's join or by the
 * driver at close, so they need no atomics and no lock. */
struct wprof {
    double busy, stall, stall_ref, park;
    long tasks;
    /* C4's wake census: how much of the per-CTB publish reaches the
     * lock, and how much of the row lag gets past the bounded spin.
     * `pub` counts every ntp_publish/ntp_publish_task, `bcast` the
     * subset that took `mu` and broadcast, `waits` every milestone wait
     * and `blocked` the subset that parked on the condvar. */
    long pub, bcast, waits, blocked;
    char pad[64];
};

static double ntp_now(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (double)t.tv_sec + 1e-9 * (double)t.tv_nsec;
}

struct ntp_pool {
    pthread_t *th;
    int nthreads;
    pthread_mutex_t mu;
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

    /* Per-worker wake slots and the LIFO stack of idle worker indices.
     * LIFO because the most recently parked worker is the one whose
     * cache is warmest. */
    struct wslot *ws;
    int *idle;
    int nidle;

    /* The two park counts, kept apart because they are woken by
     * different things and a publish that nobody is waiting for must not
     * touch the lock - the row wavefront publishes once per CTB.
     *
     * `n_idle` is workers in the claim region: from the moment one
     * decides to look for work to the moment it has either found some or
     * parked. It is bumped BEFORE the readiness scan on purpose: that
     * and the seq_cst fence in ntp_publish_task are the two halves of a
     * store-load pair, so whichever comes first in the total order, one
     * side sees the other and no wake is lost.
     *
     * `n_wait` is workers inside a milestone wait (ntp_wait /
     * ntp_wait_older), which are woken by a broadcast because their
     * predicates are opaque to the pool.
     *
     * The driver is in neither: it is woken by the task-completion
     * broadcast, which always takes the lock, and counting it would pin
     * a count above zero for the whole job and defeat both checks. */
    atomic_int n_idle, n_wait;

    /* C4: the milestone wake CHANNELS. `ntp_publish` runs once per CTB
     * of every row but the first, and it used to take `mu` and broadcast
     * `done` whenever ANY worker anywhere was in a milestone wait - on
     * touchdown_1080p at eighteen threads that was 233 603 of 257 040
     * publishes, each waking every waiting worker to re-check a
     * predicate that was almost never its own.
     *
     * A per-CTB publish inside row r of one frame can satisfy exactly
     * one milestone: the two-CTB lag of row r+1 of THAT frame. The
     * caller knows that and now says so with a key; the pool hashes the
     * key into a fixed table of channels and wakes only that one. A
     * collision costs a spurious wake and nothing else, which is why the
     * table is fixed-size and needs no lifetime rules.
     *
     * The channel condvars pair with the SAME `mu` as `done`, so the
     * memory argument is the one that was already here: the publisher
     * fences seq_cst after its release store and reads the channel's
     * waiter count, the waiter bumps that count with a seq_cst RMW
     * before re-checking the predicate under `mu`, and in the single
     * total order one of the two sees the other. */
    struct nchan *ch;

    /* the instrument (ntp.h). `prof` is set before the first submit and
     * never cleared, so a worker reads a value that was published before
     * it had any work to do. */
    int prof;
    double t_open;
    struct wprof *wp;
    /* The decisive column of the step 4 census: wall seconds during which
     * EVERY worker is parked, i.e. the pool has nothing claimable at all.
     * That is the only state in which the driver's serial work is on the
     * critical path; a driver busy while the pool has queued work costs
     * nothing but its own core. Maintained under `mu`, where `nidle`
     * already is. */
    double allpark, allpark_t0;
    /* ntp_run: the caller's seconds inside it, and the part spent waiting
     * for tasks other threads took */
    double run_in, run_wait;
};

/* mu held. Call after every change to p->nidle. */
static void allpark_upd(ntp_pool *p, int was)
{
    if (!p->prof)
        return;
    int now = p->nidle >= p->nthreads;
    if (now && !was)
        p->allpark_t0 = ntp_now();
    else if (was && !now)
        p->allpark += ntp_now() - p->allpark_t0;
}

/* The job this thread is currently running a task of, in dispatch order.
 * 0 means "not inside a task" (the driver), and since every live job has
 * seq >= 1, a driver-side ntp_wait_older claims nothing. */
static _Thread_local uint64_t tls_seq;

/* Instrument-only thread state: which worker this is (-1 on the driver),
 * how deep inside tasks it is, and the seconds it has spent running
 * NESTED tasks, which is what a milestone wait subtracts to tell holding
 * a task apart from doing work in one. */
static _Thread_local int tls_me = -1;
static _Thread_local int tls_depth;
static _Thread_local double tls_nested;

#ifndef N265_NO_THREADS
struct warg { ntp_pool *p; int me; };
static void *worker(void *arg);
#endif

/* mu held. Wake exactly one idle worker, if there is one. This is the
 * whole of "per-worker wake slots": making a task claimable used to
 * broadcast to every parked thread, which then contended on this mutex
 * to discover that one of them had the work. */
static void wake_one(ntp_pool *p)
{
    if (p->nidle <= 0)
        return;
    int was = p->nidle >= p->nthreads;
    int w = p->idle[--p->nidle];
    allpark_upd(p, was);
    p->ws[w].woken = 1;
    pthread_cond_signal(&p->ws[w].cv);
}

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
    p->t_open = ntp_now();
    p->wp = calloc((size_t)nthreads, sizeof(*p->wp));
    pthread_mutex_init(&p->mu, NULL);
    pthread_cond_init(&p->done, NULL);
    p->njs = max_jobs;
    p->js = calloc((size_t)max_jobs, sizeof(*p->js));
    p->nlanes = max_lanes;
    p->lane_free = calloc((size_t)max_lanes, sizeof(int));
    p->th = calloc((size_t)nthreads, sizeof(pthread_t));
    p->ws = calloc((size_t)nthreads, sizeof(*p->ws));
    p->idle = calloc((size_t)nthreads, sizeof(int));
    p->ch = calloc((size_t)NTP_NCHAN, sizeof(*p->ch));
    if (p->ws)
        for (int i = 0; i < nthreads; i++)
            pthread_cond_init(&p->ws[i].cv, NULL);
    if (p->ch)
        for (int i = 0; i < NTP_NCHAN; i++)
            pthread_cond_init(&p->ch[i].cv, NULL);
    if (!p->js || !p->lane_free || !p->th || !p->ws || !p->idle || !p->wp ||
        !p->ch) {
        p->nthreads = 0;
        ntp_close(p);
        return NULL;
    }
    /* highest index first, so the stack hands out 0, 1, 2 ... in order */
    for (int i = 0; i < max_lanes; i++)
        p->lane_free[i] = max_lanes - 1 - i;
    p->nlane_free = max_lanes;
#ifndef N265_NO_THREADS
    for (int i = 0; i < nthreads; i++) {
        struct warg *a = malloc(sizeof(*a));
        if (!a) {
            p->nthreads = i;
            ntp_close(p);
            return NULL;
        }
        a->p = p;
        a->me = i;
        if (pthread_create(&p->th[i], NULL, worker, a) != 0) {
            free(a);
            p->nthreads = i;
            ntp_close(p);
            return NULL;
        }
    }
#endif
    return p;
}

void ntp_close(ntp_pool *p)
{
    if (!p)
        return;
    pthread_mutex_lock(&p->mu);
    p->quit = 1;
    for (int i = 0; i < p->nthreads; i++) {
        p->ws[i].woken = 1;
        pthread_cond_signal(&p->ws[i].cv);
    }
    {
        int was = p->nidle >= p->nthreads;
        p->nidle = 0;
        allpark_upd(p, was);
    }
    pthread_cond_broadcast(&p->done);
    pthread_mutex_unlock(&p->mu);
#ifndef N265_NO_THREADS
    for (int i = 0; i < p->nthreads; i++)
        pthread_join(p->th[i], NULL);
#endif
    pthread_mutex_destroy(&p->mu);
    pthread_cond_destroy(&p->done);
    for (int i = 0; p->ws && i < p->nthreads; i++)
        pthread_cond_destroy(&p->ws[i].cv);
    for (int i = 0; p->ch && i < NTP_NCHAN; i++)
        pthread_cond_destroy(&p->ch[i].cv);
    for (int i = 0; p->js && i < p->njs; i++) {
        free(p->js[i].state);
        free(p->js[i].lane_of);
    }
    free(p->js);
    free(p->lane_free);
    free(p->ws);
    free(p->idle);
    free(p->ch);
    free(p->wp);
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
    /* the class only orders top-level claims; a nested one is oldest-first */
    int cls = limit == UINT64_MAX;
    for (int i = 0; i < p->njs; i++) {
        struct jslot *s = &p->js[i];
        if (!s->active || s->seq >= limit)
            continue;
        if (best) {
            int a = cls ? s->job->prio : 0, b = cls ? best->job->prio : 0;
            if (a > b || (a == b && s->seq > best->seq))
                continue;
        }
        ntp_job *j = s->job;
        while (s->first_pending < j->ntask &&
               s->state[s->first_pending] != T_PENDING)
            s->first_pending++;
        for (int k = s->first_pending; k < j->ntask; k++)
            if (s->state[k] == T_PENDING &&
                (j->nolane || s->lane_of[k] >= 0 || p->nlane_free > 0) &&
                j->ready(j->ctx, k)) {
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
    ntp_job *j = js->job;
    int lane = js->lane_of[idx];
    if (lane >= 0)
        js->lane_of[idx] = -1;
    else if (!j->nolane)
        lane = lane_take(p);
    uint64_t saved = tls_seq;
    tls_seq = js->seq;
    /* the instrument: a TOP-LEVEL task span is the worker being busy; a
     * nested one is already inside the enclosing span, so it is charged
     * back to the wait that claimed it instead of counted twice. */
    int prof = p->prof && tls_me >= 0;
    int top = 0;
    double t0 = 0.0;
    if (prof) {
        top = tls_depth == 0;
        t0 = ntp_now();
        tls_depth++;
    }
    pthread_mutex_unlock(&p->mu);
    int yield = j->run(j->ctx, lane, idx);
    pthread_mutex_lock(&p->mu);
    if (prof) {
        tls_depth--;
        double d = ntp_now() - t0;
        if (top) {
            p->wp[tls_me].busy += d;
            p->wp[tls_me].tasks++;
        } else {
            tls_nested += d;
        }
    }
    tls_seq = saved;
    if (yield) {
        /* back to pending, lane kept. Nothing became claimable and no
         * milestone moved, so there is nobody to wake: the caller goes
         * straight back to its own claim. */
        js->state[idx] = T_PENDING;
        js->lane_of[idx] = lane;
        if (idx < js->first_pending)
            js->first_pending = idx;
        return;
    }
    js->state[idx] = T_DONE;
    js->ndone++;
    atomic_store_explicit(&js->ndone_pub, js->ndone, memory_order_release);
    if (lane >= 0)
        lane_give(p, lane);
    /* A finished task can unblock both a claimable task and an in-task
     * milestone wait. The claim side takes one worker (the chain in the
     * worker loop spreads it if there is more than one task ready); the
     * wait side is a broadcast because the predicates are opaque, and it
     * is also what wakes the driver in ntp_join. */
    wake_one(p);
    pthread_cond_broadcast(&p->done);
}

#ifdef N265_NO_THREADS
/* mu held. Nobody else will make `pred` true, so run claimable tasks of
 * jobs older than `limit` here until it is. */
static void solo_until(ntp_pool *p, uint64_t limit, ntp_pred_fn pred,
                       void *ctx)
{
    while (!pred(ctx)) {
        int idx = 0;
        struct jslot *js = claim(p, limit, &idx);
        if (!js) {
            fputs("ntp: nothing claimable on the one-thread schedule\n",
                  stderr);
            abort();
        }
        run_task(p, js, idx);
    }
}

static int run_done(void *ctx);
#else
static void *worker(void *arg)
{
    struct warg *a = arg;
    ntp_pool *p = a->p;
    int me = a->me;
    free(a);
    tls_me = me;

    pthread_mutex_lock(&p->mu);
    for (;;) {
        if (p->quit)
            break;
        /* Count in BEFORE testing readiness, so this and the fence in
         * ntp_publish_task are the two halves of a store-load pair:
         * whichever of the two seq_cst operations comes first in the
         * total order, one side sees the other, and no wake is lost. */
        atomic_fetch_add(&p->n_idle, 1);
        int idx = 0;
        struct jslot *js = claim(p, UINT64_MAX, &idx);
        if (!js) {
            p->ws[me].woken = 0;
            {
                int was = p->nidle >= p->nthreads;
                p->idle[p->nidle++] = me;
                allpark_upd(p, was);
            }
            /* The flag is captured HERE, not read again at the end: every
             * worker parks once before the encoder has had a chance to
             * enable the instrument, and an interval whose start was not
             * stamped must not be accumulated when its end is. */
            int pf = p->prof;
            double tp = pf ? ntp_now() : 0.0;
            while (!p->ws[me].woken && !p->quit)
                pthread_cond_wait(&p->ws[me].cv, &p->mu);
            if (pf)
                p->wp[me].park += ntp_now() - tp;
            atomic_fetch_sub(&p->n_idle, 1);
            continue;
        }
        atomic_fetch_sub(&p->n_idle, 1);
        /* Chain: a wake carries ONE worker, so a worker that found work
         * hands the next one on before it disappears into a task. Worst
         * case that is one spurious wake per task claimed, against one
         * broadcast per CTB before. */
        wake_one(p);
        run_task(p, js, idx);
    }
    pthread_mutex_unlock(&p->mu);
    return NULL;
}
#endif

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
        int *l = s ? realloc(js->lane_of, (size_t)job->ntask * sizeof(int))
                   : NULL;
        if (s)
            js->state = s;
        if (!l)
            js = NULL;
        else {
            js->lane_of = l;
            js->cap = job->ntask;
        }
    }
    if (!js) {      /* max_jobs contract broken, or out of memory: run it
                     * inline on this thread, which is still correct */
        pthread_mutex_unlock(&p->mu);
        /* index order, one thread: every task's producers are done
         * before it runs, so a yield cannot happen here; the loop is
         * only there so one could not be dropped */
        for (int i = 0; i < job->ntask; i++)
            while (job->run(job->ctx, 0, i))
                ;
        job->impl = NULL;
        return;
    }
    memset(js->state, T_PENDING, (size_t)job->ntask);
    for (int i = 0; i < job->ntask; i++)
        js->lane_of[i] = -1;
    js->ndone = 0;
    atomic_store_explicit(&js->ndone_pub, 0, memory_order_relaxed);
    js->first_pending = 0;
    js->job = job;
    js->seq = p->next_seq++;
    js->active = 1;
    job->impl = js;
    wake_one(p);
    pthread_mutex_unlock(&p->mu);
}

void ntp_join(ntp_pool *p, ntp_job *job)
{
    struct jslot *js = job->impl;
    if (!js)
        return;                 /* ran inline at submit */
    pthread_mutex_lock(&p->mu);
#ifdef N265_NO_THREADS
    solo_until(p, UINT64_MAX, run_done, js);
#else
    while (js->ndone < job->ntask)
        pthread_cond_wait(&p->done, &p->mu);
#endif
    js->active = 0;
    js->job = NULL;
    pthread_mutex_unlock(&p->mu);
    job->impl = NULL;
}

static int spin_for(ntp_pred_fn pred, void *ctx);

static int run_done(void *ctx)
{
    const struct jslot *js = ctx;
    return atomic_load_explicit(&js->ndone_pub, memory_order_acquire) >=
           js->job->ntask;
}

void ntp_run(ntp_pool *p, ntp_job *job)
{
    /* nobody free to help: skip the registration and its wakes */
    if (job->drain && atomic_load(&p->n_idle) == 0) {
        job->run(job->ctx, -1, 0);
        return;
    }
    double t0 = p->prof ? ntp_now() : 0.0;
    ntp_submit(p, job);
    struct jslot *js = job->impl;
    if (!js)
        return;                 /* ran inline at submit */
    pthread_mutex_lock(&p->mu);
    for (;;) {
        int idx = -1;
        while (js->first_pending < job->ntask &&
               js->state[js->first_pending] != T_PENDING)
            js->first_pending++;
        for (int k = js->first_pending; k < job->ntask; k++)
            if (js->state[k] == T_PENDING && job->ready(job->ctx, k)) {
                idx = k;
                break;
            }
        if (idx >= 0) {
            js->state[idx] = T_RUNNING;
            run_task(p, js, idx);
            if (job->drain)
                for (int k = js->first_pending; k < job->ntask; k++)
                    if (js->state[k] == T_PENDING) {
                        js->state[k] = T_DONE;
                        js->ndone++;
                        atomic_store_explicit(&js->ndone_pub, js->ndone,
                                              memory_order_relaxed);
                    }
            continue;
        }
        if (js->ndone >= job->ntask)
            break;
#ifdef N265_NO_THREADS
        {
            int k = 0;
            struct jslot *o = claim(p, UINT64_MAX, &k);
            if (!o) {
                fputs("ntp: nothing claimable on the one-thread schedule\n",
                      stderr);
                abort();
            }
            run_task(p, o, k);
            continue;
        }
#endif
        /* the tasks left are running, and a sleep and wake would cost more
         * than most of them have left to do: spin first */
        double tw = p->prof ? ntp_now() : 0.0;
        pthread_mutex_unlock(&p->mu);
        int fin = spin_for(run_done, js);
        pthread_mutex_lock(&p->mu);
        if (!fin && js->ndone < job->ntask)
            pthread_cond_wait(&p->done, &p->mu);
        if (p->prof)
            p->run_wait += ntp_now() - tw;
    }
    js->active = 0;
    js->job = NULL;
    if (p->prof)
        p->run_in += ntp_now() - t0;
    pthread_mutex_unlock(&p->mu);
    job->impl = NULL;
}

/* the bounded spin both wait forms share; 1 = the predicate came true */
static int spin_for(ntp_pred_fn pred, void *ctx)
{
    if (pred(ctx))
        return 1;
#ifdef N265_NO_THREADS
    return 0;
#endif
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

void ntp_wait(ntp_pool *p, unsigned key, ntp_pred_fn pred, void *ctx)
{
    /* This runs once per CTB of every row but the first, so the
     * instrument-off path must touch NO thread-local: reading one on
     * this path cost 5% of the one-thread wall when it was written
     * unconditionally. `p->prof` is set before the first task and never
     * cleared, so the short-circuit is stable for the whole encode. */
    int prof = p->prof;
    double t0 = 0.0, n0 = 0.0;
    if (prof) {
        prof = tls_me >= 0;
        if (prof) { t0 = ntp_now(); n0 = tls_nested; p->wp[tls_me].waits++; }
    }
    if (spin_for(pred, ctx)) {
        if (prof)
            p->wp[tls_me].stall += ntp_now() - t0;
        return;
    }
    if (prof)
        p->wp[tls_me].blocked++;
    /* C4: park on the key's OWN channel rather than on `done`. `done` is
     * broadcast by every task completion and by ntp_publish_task, so a
     * waiter here used to be woken by all of them; now it is woken by
     * the one publisher whose progress its predicate reads. The
     * predicate is still re-checked under `mu` before and after every
     * sleep, so a spurious wake from a hash collision is free. */
    struct nchan *c = &p->ch[nchan_idx(key)];
    pthread_mutex_lock(&p->mu);
#ifdef N265_NO_THREADS
    /* the producer is an earlier task of this job or an older one */
    solo_until(p, tls_seq ? tls_seq + 1 : UINT64_MAX, pred, ctx);
#endif
    atomic_fetch_add(&c->nwait, 1);
    while (!pred(ctx))
        pthread_cond_wait(&c->cv, &p->mu);
    atomic_fetch_sub(&c->nwait, 1);
    pthread_mutex_unlock(&p->mu);
    if (prof)
        p->wp[tls_me].stall += ntp_now() - t0 - (tls_nested - n0);
}

void ntp_wait_older(ntp_pool *p, ntp_pred_fn pred, void *ctx)
{
    int prof = p->prof;                 /* as in ntp_wait: no TLS when off */
    double t0 = 0.0, n0 = 0.0;
    if (prof) {
        prof = tls_me >= 0;
        if (prof) { t0 = ntp_now(); n0 = tls_nested; }
    }
    if (spin_for(pred, ctx)) {
        if (prof) {
            double d = ntp_now() - t0;
            p->wp[tls_me].stall += d;
            p->wp[tls_me].stall_ref += d;
        }
        return;
    }
    uint64_t limit = tls_seq;   /* 0 on the driver: claims nothing */
    pthread_mutex_lock(&p->mu);
#ifdef N265_NO_THREADS
    solo_until(p, limit ? limit : UINT64_MAX, pred, ctx);
#endif
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
            atomic_fetch_add(&p->n_wait, 1);
        pthread_cond_wait(&p->done, &p->mu);
        if (limit)
            atomic_fetch_sub(&p->n_wait, 1);
    }
    pthread_mutex_unlock(&p->mu);
    if (prof) {
        double d = ntp_now() - t0 - (tls_nested - n0);
        p->wp[tls_me].stall += d;
        p->wp[tls_me].stall_ref += d;
    }
}

/* Progress that can only satisfy a milestone predicate - the per-CTB
 * publish inside a row. The publisher's half of the store-load pair: the
 * progress store is a release, so a full fence is needed before reading
 * the park counts or the two could miss each other. With nobody in a
 * milestone wait this is two atomic operations and no lock, which is
 * what keeps the per-CTB cost off the 1-thread leg entirely. */
void ntp_publish(ntp_pool *p, unsigned key)
{
    struct nchan *c = &p->ch[nchan_idx(key)];
    atomic_thread_fence(memory_order_seq_cst);
    if (p->prof && tls_me >= 0)
        p->wp[tls_me].pub++;
    if (atomic_load(&c->nwait) == 0)
        return;
    if (p->prof && tls_me >= 0)
        p->wp[tls_me].bcast++;
    pthread_mutex_lock(&p->mu);
    pthread_cond_broadcast(&c->cv);
    pthread_mutex_unlock(&p->mu);
}

/* Progress that can also make a TASK claimable - a row watermark, or the
 * CTB that puts the row below over its lag. Both halves, and the claim
 * half wakes exactly one worker. Waking nobody on the claim side was
 * worth the whole wavefront once (T1): a row became claimable only when
 * the row above FINISHED, so twelve threads ran one row at a time. */
void ntp_publish_task(ntp_pool *p, unsigned key)
{
    struct nchan *ch = key == NTP_KEY_NONE ? NULL
                                           : &p->ch[nchan_idx(key)];
    atomic_thread_fence(memory_order_seq_cst);
    if (p->prof && tls_me >= 0)
        p->wp[tls_me].pub++;
    /* Three wakes, because this form is called where the progress can
     * satisfy all three: the key's milestone channel (the row below's
     * lag), `done` for the CROSS-frame waiters of ntp_wait_older, and
     * one idle worker for the task that has just become claimable. It
     * runs once or twice per ROW, so its herd costs nothing. */
    int m = ch && atomic_load(&ch->nwait) != 0;
    int w = atomic_load(&p->n_wait) != 0;
    int c = atomic_load(&p->n_idle) != 0;
    if (!m && !w && !c)
        return;
    if (p->prof && tls_me >= 0)
        p->wp[tls_me].bcast++;
    pthread_mutex_lock(&p->mu);
    if (m)
        pthread_cond_broadcast(&ch->cv);
    if (w)
        pthread_cond_broadcast(&p->done);
    if (c)
        wake_one(p);
    pthread_mutex_unlock(&p->mu);
}

void ntp_prof_enable(ntp_pool *p)
{
    if (!p)
        return;
    pthread_mutex_lock(&p->mu);
    p->prof = 1;
    /* the pool is all-parked at open, so the interval the census cares
     * about is already running when the encoder gets here */
    if (p->nidle >= p->nthreads)
        p->allpark_t0 = ntp_now();
    pthread_mutex_unlock(&p->mu);
}

void ntp_prof_run(const ntp_pool *p, double *in, double *wait)
{
    *in = p ? p->run_in : 0.0;
    *wait = p ? p->run_wait : 0.0;
}

void ntp_prof_counts(const ntp_pool *p, long *pub, long *bcast,
                     long *waits, long *blocked)
{
    long a = 0, b = 0, c = 0, d = 0;
    for (int i = 0; p && i < p->nthreads; i++) {
        a += p->wp[i].pub;
        b += p->wp[i].bcast;
        c += p->wp[i].waits;
        d += p->wp[i].blocked;
    }
    if (pub) *pub = a;
    if (bcast) *bcast = b;
    if (waits) *waits = c;
    if (blocked) *blocked = d;
}

void ntp_prof_read(const ntp_pool *p, double *busy, double *stall,
                   double *stall_ref, double *park, double *allpark,
                   double *wall, long *tasks)
{
    double b = 0, s = 0, sr = 0, k = 0;
    long n = 0;
    for (int i = 0; p && i < p->nthreads; i++) {
        b += p->wp[i].busy;
        s += p->wp[i].stall;
        sr += p->wp[i].stall_ref;
        k += p->wp[i].park;
        n += p->wp[i].tasks;
    }
    if (busy) *busy = b;
    if (stall) *stall = s;
    if (stall_ref) *stall_ref = sr;
    if (park) *park = k;
    if (allpark) {
        /* the read happens on the driver with the pool quiescent, so an
         * open interval is closed here rather than left out */
        double a = p ? p->allpark : 0.0;
        if (p && p->nidle >= p->nthreads)
            a += ntp_now() - p->allpark_t0;
        *allpark = a;
    }
    if (wall) *wall = p ? ntp_now() - p->t_open : 0.0;
    if (tasks) *tasks = n;
}
