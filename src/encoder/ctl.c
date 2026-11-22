/*
 * Live control (include/yah265.h, the shared contract v1). See ctl.h.
 * Copyright (c) 2026, the yah265 authors. GPL-2.0-or-later; see LICENSE.
 */

#include <math.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "cli/ctl_log.h"
#include "encoder/ctl.h"
#include "encoder/encoder.h"
#include "encoder/la.h"

/* the first version's structs end at their last v1 field */
#define CTL_V1_SIZE  ((int)(offsetof(n265_control_t, height) + sizeof(int)))
#define STAT_V1_SIZE ((int)(offsetof(n265_status_t, controls_applied) + \
                            sizeof(int64_t)))

static double now_s(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + 1e-9 * (double)ts.tv_nsec;
}

n265_ctl *n265_ctl_new(const n265_encoder *e)
{
    n265_ctl *c = calloc(1, sizeof *c);
    if (!c)
        return NULL;
    c->ring = 256;
    c->slot = calloc((size_t)c->ring, sizeof *c->slot);
    if (!c->slot) {
        free(c);
        return NULL;
    }
    for (int i = 0; i < c->ring; i++)
        c->slot[i].disp = -1;
    c->qp_min = e->qpmin;
    c->qp_max = e->qpmax;
    c->q_kbps = e->abr_on ? e->abr_bitrate / 1000.0 : 0.0;
    c->q_maxrate = e->vbv.on ? e->vbv.maxrate / 1000.0 : 0.0;
    c->q_bufsize = e->vbv.on ? e->vbv.bufsize / 1000.0 : 0.0;
    c->cbr = e->abr_on && e->vbv.on &&
             !(e->vbv.maxrate > e->abr_bitrate * 1.0001);
    c->fps = e->param.fps_num > 0 && e->param.fps_den > 0
                 ? (double)e->param.fps_num / e->param.fps_den : 25.0;
    return c;
}

void n265_ctl_free(n265_ctl *c)
{
    if (!c)
        return;
    free(c->fq);
    free(c->rq);
    free(c->done);
    free(c->echo);
    free(c->zones);
    free(c->slot);
    free(c);
}

static int grow(n265_control_t **q, int n, int *cap)
{
    if (n < *cap)
        return 0;
    int nc = *cap ? 2 * *cap : 16;
    n265_control_t *a = realloc(*q, (size_t)nc * sizeof *a);
    if (!a)
        return -1;
    *q = a;
    *cap = nc;
    return 0;
}

/* sorted by frame, a tie behind the events already queued */
static int q_insert(n265_control_t **q, int *n, int *cap,
                    const n265_control_t *ev)
{
    if (grow(q, *n, cap) < 0)
        return -1;
    int i = *n;
    while (i > 0 && (*q)[i - 1].frame > ev->frame) {
        (*q)[i] = (*q)[i - 1];
        i--;
    }
    (*q)[i] = *ev;
    (*n)++;
    return 0;
}

static void q_pop_front(n265_control_t *q, int *n)
{
    (*n)--;
    memmove(q, q + 1, (size_t)*n * sizeof *q);
}

static void record(n265_ctl *c, const n265_control_t *ev, int64_t at)
{
    if (grow(&c->done, c->ndone, &c->cdone) == 0) {
        c->done[c->ndone] = *ev;
        c->done[c->ndone++].applied_frame = at;
    }
    if (at < 0)
        return;
    c->applied++;
    if (c->echo_on && grow(&c->echo, c->necho, &c->cecho) == 0) {
        c->echo[c->necho] = *ev;
        c->echo[c->necho++].applied_frame = at;
    }
}

void n265_ctl_hand_in(n265_encoder *e)
{
    n265_ctl *c = e->ctl;
    if (c->in - c->oldest >= c->ring) {
        int nr = 2 * c->ring;
        struct n265_ctl_slot *s = calloc((size_t)nr, sizeof *s);
        if (s) {
            for (int i = 0; i < nr; i++)
                s[i].disp = -1;
            for (int64_t d = c->oldest; d < c->in; d++)
                s[d & (nr - 1)] = c->slot[d & (c->ring - 1)];
            free(c->slot);
            c->slot = s;
            c->ring = nr;
        }
    }
    struct n265_ctl_slot *s = &c->slot[c->in & (c->ring - 1)];
    s->disp = c->in;
    s->t_in = now_s();
    s->emitted = 0;
    c->in++;
}

void n265_ctl_push(n265_encoder *e, la_frame *f, int64_t d)
{
    n265_ctl *c = e->ctl;
    f->ctl_key = 0;
    f->ctl_zone = 0;
    f->ctl_zoff = 0.0;
    if (c->used) {
        while (c->nfq > 0 && c->fq[0].frame <= d) {
            const n265_control_t *ev = &c->fq[0];
            if (ev->kind == N265_CTL_QP_LIMITS) {
                if (ev->qp_min >= 0)
                    c->qp_min = ev->qp_min;
                if (ev->qp_max >= 0)
                    c->qp_max = ev->qp_max;
            } else if (ev->kind == N265_CTL_KEYFRAME) {
                f->ctl_key = 1;
            }
            record(c, ev, d);
            q_pop_front(c->fq, &c->nfq);
        }
        int w = 0;
        for (int i = 0; i < c->nz; i++) {
            if (c->zones[i].last < d)
                continue;
            if (c->zones[i].first <= d) {
                f->ctl_zone = 1;
                f->ctl_zoff = c->zones[i].off;
            }
            c->zones[w++] = c->zones[i];
        }
        c->nz = w;
    }
    f->ctl_qpmin = c->qp_min;
    f->ctl_qpmax = c->qp_max;
}

int n265_ctl_rate_due(const n265_encoder *e, int64_t d)
{
    const n265_ctl *c = e->ctl;
    return c && c->nrq > 0 && c->rq[0].frame <= d;
}

/* The RATE retarget (contract section 5). Everything before this frame
 * is coded and counted, so the ledger's running sums are the coded
 * frames' and both scale by r; the content models stay. */
static void retarget(n265_encoder *e, const n265_control_t *ev)
{
    n265_rc *rc = &e->rc;
    if (ev->bitrate_kbps > 0 && e->abr_on && rc->bits_per_frame > 0.0) {
        double nbps = (double)ev->bitrate_kbps * 1000.0;
        double nb = nbps / rc->fps;
        double r = nb / rc->bits_per_frame;
        rc->planned_bits = r * (rc->planned_bits - rc->pend_bits) +
                           rc->pend_bits;
        rc->total_bits *= r;
        if (!rc->fbstep)
            rc->rf -= 6.0 * log2(r);
        rc->bits_per_frame = nb;
        e->abr_bitrate = nbps;
    }
    if (!e->vbv.on)
        return;
    int ring = 2 * N265_MAX_FRAME_WORK;
    if (ev->vbv_bufsize_kbit > 0) {
        double ns = (double)ev->vbv_bufsize_kbit * 1000.0;
        double k = ns / e->vbv.bufsize;
        e->vbv.fill *= k;
        e->vbv.min_fill *= k;
        for (int i = 0; i < ring; i++)
            e->vbv_fa[i] *= k;
        e->vbv.bufsize = ns;
        if (e->ausize_factor < 1.0)
            e->vbv.au_cap = e->ausize_factor * ns;
    }
    if (ev->vbv_maxrate_kbps > 0) {
        e->vbv.maxrate = (double)ev->vbv_maxrate_kbps * 1000.0;
        e->vbv.inflow = e->vbv.maxrate * e->vbv.frame_dur;
    }
}

void n265_ctl_rate_apply(n265_encoder *e, int64_t d)
{
    n265_ctl *c = e->ctl;
    while (c->nrq > 0 && c->rq[0].frame <= d) {
        const n265_control_t *ev = &c->rq[0];
        if (ev->kind == N265_CTL_RATE)
            retarget(e, ev);
        else
            e->crf = ev->crf;
        record(c, ev, d);
        q_pop_front(c->rq, &c->nrq);
    }
}

void n265_ctl_emit(n265_encoder *e, int64_t disp)
{
    n265_ctl *c = e->ctl;
    struct n265_ctl_slot *s = &c->slot[disp & (c->ring - 1)];
    if (s->disp == disp)
        s->emitted = 1;
    c->out++;
    while (c->oldest < c->in) {
        const struct n265_ctl_slot *o = &c->slot[c->oldest & (c->ring - 1)];
        if (o->disp != c->oldest || !o->emitted)
            break;
        c->oldest++;
    }
}

void n265_ctl_flush_end(n265_encoder *e)
{
    n265_ctl *c = e->ctl;
    for (int i = 0; i < c->nfq; i++)
        record(c, &c->fq[i], -1);
    for (int i = 0; i < c->nrq; i++)
        record(c, &c->rq[i], -1);
    c->nfq = c->nrq = 0;
}

static void to_log(const n265_control_t *v, struct ctl_log_event *l)
{
    memset(l, 0, sizeof *l);
    l->kind = v->kind;
    l->frame = v->frame;
    l->bitrate_kbps = v->bitrate_kbps;
    l->vbv_maxrate_kbps = v->vbv_maxrate_kbps;
    l->vbv_bufsize_kbit = v->vbv_bufsize_kbit;
    l->crf = v->crf;
    l->qp_min = v->qp_min;
    l->qp_max = v->qp_max;
    l->last = v->last;
    l->qp_offset = v->qp_offset;
    l->fps_num = v->fps_num;
    l->fps_den = v->fps_den;
    memcpy(l->preset, v->preset, sizeof l->preset);
    l->preset[sizeof l->preset - 1] = 0;
    l->width = v->width;
    l->height = v->height;
}

int n265_ctl_echo_take(n265_encoder *e, int64_t d, char *buf, size_t n)
{
    n265_ctl *c = e->ctl;
    size_t o = 0;
    int k = 0, w = 0;
    buf[0] = 0;
    for (int i = 0; i < c->necho; i++) {
        if (c->echo[i].applied_frame != d) {
            c->echo[w++] = c->echo[i];
            continue;
        }
        struct ctl_log_event l;
        char line[256];
        to_log(&c->echo[i], &l);
        ctl_log_format(&l, line, sizeof line);
        int m = snprintf(buf + o, n - o, "%s\"%s\"", k ? ", " : "", line);
        if (m > 0 && o + (size_t)m < n)
            o += (size_t)m;
        k++;
    }
    c->necho = w;
    return k;
}

/* ---- the public calls ---- */

void n265_control_init(n265_control_t *ev, int kind)
{
    if (!ev)
        return;
    memset(ev, 0, sizeof *ev);
    ev->size = (int)sizeof *ev;
    ev->kind = kind;
    ev->frame = N265_CTL_NEXT;
    ev->applied_frame = -1;
    ev->qp_min = ev->qp_max = -1;
    ev->last = -1;
}

const char *n265_control_strerror(int err)
{
    switch (err) {
    case N265_CTL_OK:           return "accepted";
    case N265_CTL_EINVAL:       return "invalid event: bad size, field or value";
    case N265_CTL_EUNSUPPORTED: return "this event kind is not supported";
    case N265_CTL_ELATE:        return "the landing frame has already been handed to the encoder";
    case N265_CTL_EMODE:        return "the event does not fit this encode's rate-control mode";
    case N265_CTL_EHRD:         return "the change would break the stream's signalled HRD (see --no-hrd)";
    case N265_CTL_ELEVEL:       return "the change exceeds the stream's declared level";
    case N265_CTL_ECONFLICT:    return "the zone overlaps a zone already set";
    case N265_CTL_ENOMEM:       return "out of memory";
    default:                    return "unknown error";
    }
}

static int zone_overlaps(const n265_encoder *e, int64_t first, int64_t last)
{
    const n265_ctl *c = e->ctl;
    for (int i = 0; i < c->nz; i++)
        if (first <= c->zones[i].last && c->zones[i].first <= last)
            return 1;
    if (e->zones_on)
        for (int i = 0; i < e->zones.n; i++)
            if (first <= e->zones.z[i].end && e->zones.z[i].start <= last)
                return 1;
    return 0;
}

static int check(n265_encoder *e, n265_control_t *v)
{
    n265_ctl *c = e->ctl;
    int twopass = e->tp.pass != 0;
    switch (v->kind) {
    case 0:
        return N265_CTL_EINVAL;
    case N265_CTL_RATE: {
        if (v->bitrate_kbps < 0 || v->vbv_maxrate_kbps < 0 ||
            v->vbv_bufsize_kbit < 0)
            return N265_CTL_EINVAL;
        if (!v->bitrate_kbps && !v->vbv_maxrate_kbps && !v->vbv_bufsize_kbit)
            return N265_CTL_EINVAL;
        if (twopass)
            return N265_CTL_EMODE;
        if (v->bitrate_kbps && !e->abr_on)
            return N265_CTL_EMODE;
        if ((v->vbv_maxrate_kbps || v->vbv_bufsize_kbit) && !e->vbv.on)
            return N265_CTL_EMODE;
        /* a CBR declaration pads the stream back up to its rate */
        if (e->hrd_on && e->strict_cbr)
            return N265_CTL_EHRD;
        double nk = v->bitrate_kbps ? v->bitrate_kbps : c->q_kbps;
        double nm = v->vbv_maxrate_kbps ? v->vbv_maxrate_kbps : c->q_maxrate;
        double nb = v->vbv_bufsize_kbit ? v->vbv_bufsize_kbit : c->q_bufsize;
        if (c->cbr && v->bitrate_kbps && !v->vbv_maxrate_kbps)
            nm = nk;
        if (e->abr_on && e->vbv.on) {
            int cbr_now = !(nm > nk * 1.0001);
            if (c->cbr ? !cbr_now || nm < nk * 0.9999 : cbr_now)
                return N265_CTL_EMODE;
        }
        if (e->hrd_on && (nm * 1000.0 > (double)e->hrd_bit_rate_bits ||
                          nk * 1000.0 > (double)e->hrd_bit_rate_bits ||
                          nb * 1000.0 > (double)e->hrd_cpb_size_bits))
            return N265_CTL_EHRD;
        double br = (nk > nm ? nk : nm) * 1000.0;
        if ((e->level.max_br > 0 && br > (double)e->level.max_br) ||
            (e->vbv.on && e->level.max_cpb > 0 &&
             nb * 1000.0 > (double)e->level.max_cpb))
            return N265_CTL_ELEVEL;
        /* the event carries what it resolves to, so the echo replays it */
        if (c->cbr && v->bitrate_kbps && !v->vbv_maxrate_kbps)
            v->vbv_maxrate_kbps = (int)nm;
        return N265_CTL_OK;
    }
    case N265_CTL_CRF:
        if (!e->crf_on || twopass)
            return N265_CTL_EMODE;
        if (!(v->crf >= (double)N265_QP_MIN && v->crf <= 51.0) ||
            v->crf < e->crf_min || v->crf > e->crf_max)
            return N265_CTL_EINVAL;
        return N265_CTL_OK;
    case N265_CTL_QP_LIMITS: {
        if (!e->abr_on && !e->crf_on)
            return N265_CTL_EMODE;
        if (v->qp_min < -1 || v->qp_min > 51 || v->qp_max < -1 ||
            v->qp_max > 51 || (v->qp_min < 0 && v->qp_max < 0))
            return N265_CTL_EINVAL;
        int lo = v->qp_min >= 0 ? v->qp_min : c->qp_min;
        int hi = v->qp_max >= 0 ? v->qp_max : c->qp_max;
        for (int i = 0; i < c->nfq; i++)
            if (c->fq[i].kind == N265_CTL_QP_LIMITS &&
                c->fq[i].frame <= v->frame) {
                if (v->qp_min < 0 && c->fq[i].qp_min >= 0)
                    lo = c->fq[i].qp_min;
                if (v->qp_max < 0 && c->fq[i].qp_max >= 0)
                    hi = c->fq[i].qp_max;
            }
        if (lo > hi)
            return N265_CTL_EINVAL;
        /* the buffer may need the QPs above a ceiling */
        if (e->hrd_on && v->qp_max >= 0 && v->qp_max < 51)
            return N265_CTL_EHRD;
        return N265_CTL_OK;
    }
    case N265_CTL_KEYFRAME:
        return twopass ? N265_CTL_EMODE : N265_CTL_OK;
    case N265_CTL_ZONE:
        if (v->last < v->frame)
            return N265_CTL_EINVAL;
        if (!(v->qp_offset >= -51.0 && v->qp_offset <= 51.0))
            return N265_CTL_EINVAL;
        if (zone_overlaps(e, v->frame, v->last))
            return N265_CTL_ECONFLICT;
        return N265_CTL_OK;
    default:
        return N265_CTL_EUNSUPPORTED;
    }
}

int n265_encoder_control(n265_encoder *e, n265_control_t *ev)
{
    if (!e || !ev || ev->size < CTL_V1_SIZE ||
        ev->size > (int)sizeof(n265_control_t))
        return N265_CTL_EINVAL;
    if (e->hw || !e->ctl)
        return N265_CTL_EUNSUPPORTED;
    n265_ctl *c = e->ctl;
    n265_control_t v;
    n265_control_init(&v, 0);
    memcpy(&v, ev, (size_t)ev->size);
    v.size = (int)sizeof v;
    if (v.frame == N265_CTL_NEXT)
        v.frame = c->in;
    else if (v.frame < 0)
        return N265_CTL_EINVAL;
    else if (v.frame < c->in)
        return N265_CTL_ELATE;
    v.applied_frame = -1;
    int r = check(e, &v);
    if (r != N265_CTL_OK)
        return r;
    if (v.kind == N265_CTL_RATE || v.kind == N265_CTL_CRF) {
        if (q_insert(&c->rq, &c->nrq, &c->crq, &v) < 0)
            return N265_CTL_ENOMEM;
        if (v.kind == N265_CTL_RATE) {
            if (v.bitrate_kbps)
                c->q_kbps = v.bitrate_kbps;
            if (v.vbv_maxrate_kbps)
                c->q_maxrate = v.vbv_maxrate_kbps;
            if (v.vbv_bufsize_kbit)
                c->q_bufsize = v.vbv_bufsize_kbit;
        }
    } else {
        if (v.kind == N265_CTL_ZONE) {
            if (c->nz == c->cz) {
                int nc = c->cz ? 2 * c->cz : 8;
                struct n265_ctl_zone *z = realloc(c->zones,
                                                  (size_t)nc * sizeof *z);
                if (!z)
                    return N265_CTL_ENOMEM;
                c->zones = z;
                c->cz = nc;
            }
            c->zones[c->nz++] = (struct n265_ctl_zone){ v.frame, v.last,
                                                        v.qp_offset };
        }
        if (q_insert(&c->fq, &c->nfq, &c->cfq, &v) < 0) {
            if (v.kind == N265_CTL_ZONE)
                c->nz--;
            return N265_CTL_ENOMEM;
        }
    }
    c->used = 1;
    ev->frame = v.frame;
    if (v.kind == N265_CTL_RATE &&
        ev->size >= (int)(offsetof(n265_control_t, vbv_maxrate_kbps) +
                          sizeof(int)))
        ev->vbv_maxrate_kbps = v.vbv_maxrate_kbps;
    return N265_CTL_OK;
}

int n265_encoder_controls_applied(n265_encoder *e, n265_control_t *out,
                                  int max)
{
    if (!e || !out || max < 0)
        return -1;
    n265_ctl *c = e->ctl;
    if (!c)
        return 0;
    for (int i = 0; i < max; i++)
        if (out[i].size < CTL_V1_SIZE ||
            out[i].size > (int)sizeof(n265_control_t))
            return -1;
    int n = c->ndone < max ? c->ndone : max;
    for (int i = 0; i < n; i++) {
        int sz = out[i].size;
        memcpy(&out[i], &c->done[i], (size_t)sz);
        out[i].size = sz;
    }
    c->ndone -= n;
    memmove(c->done, c->done + n, (size_t)c->ndone * sizeof *c->done);
    return n;
}

int n265_encoder_status(n265_encoder *e, n265_status_t *st)
{
    if (!e || !st || st->size < STAT_V1_SIZE ||
        st->size > (int)sizeof(n265_status_t))
        return -1;
    n265_status_t s;
    memset(&s, 0, sizeof s);
    n265_ctl *c = e->ctl;
    if (c) {
        s.frames_in = c->in;
        s.frames_out = c->out;
        s.queue_depth = (int)(c->in - c->out);
        s.next_frame = c->in;
        s.queue_ms = s.queue_depth * 1000.0 / c->fps;
        if (c->oldest < c->in) {
            const struct n265_ctl_slot *o =
                &c->slot[c->oldest & (c->ring - 1)];
            if (o->disp == c->oldest)
                s.oldest_age_ms = (now_s() - o->t_in) * 1000.0;
        }
        s.target_kbps = e->abr_on ? e->abr_bitrate / 1000.0 : 0.0;
        if (e->vbv.on) {
            s.vbv_maxrate_kbps = e->vbv.maxrate / 1000.0;
            s.vbv_bufsize_kbit = e->vbv.bufsize / 1000.0;
            s.vbv_fill_bits = e->vbv.fill;
        }
        if (e->abr_on)
            s.rate_balance_bits =
                e->rc.bits_per_frame * (double)e->rc.n_coded -
                e->rc.total_bits;
        s.budget_bits = e->vbv.on ? s.vbv_fill_bits : s.rate_balance_bits;
        s.controls_pending = c->nfq + c->nrq;
        s.controls_applied = c->applied;
    }
    int sz = st->size;
    memcpy(st, &s, (size_t)sz);
    st->size = sz;
    return 0;
}
