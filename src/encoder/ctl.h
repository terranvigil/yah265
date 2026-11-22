/*
 * Live control: the event queue behind n265_encoder_control and its
 * three landing points (the lookahead push, the rate decision, the
 * flush). Everything here runs on the thread that calls encode.
 * Copyright (c) 2026, the yah265 authors. GPL-2.0-or-later; see LICENSE.
 */

#ifndef N265_CTL_H
#define N265_CTL_H

#include <stdint.h>

#include "yah265.h"

struct n265_encoder;
struct n265_frame_work;
struct la_frame;

struct n265_ctl_zone {
    int64_t first, last;
    double off;
};

struct n265_ctl_slot {
    int64_t disp;
    double t_in;
    int emitted;
};

typedef struct n265_ctl {
    int used;                   /* an event was ever accepted */
    int64_t in, out, applied;
    /* pending, each sorted by named frame, ties in acceptance order:
     * fq lands at the push (QP_LIMITS, KEYFRAME, ZONE), rq at the rate
     * decision (RATE, CRF) */
    n265_control_t *fq, *rq, *done, *echo;
    int nfq, cfq, nrq, crq, ndone, cdone, necho, cecho;
    int echo_on;
    struct n265_ctl_zone *zones;
    int nz, cz;
    int qp_min, qp_max;         /* in force at the next push */
    /* what the accepted events leave in force, for validation */
    double q_kbps, q_maxrate, q_bufsize;
    int cbr;                    /* opened with maxrate equal to the bitrate */
    double fps;
    struct n265_ctl_slot *slot;
    int ring;
    int64_t oldest;
} n265_ctl;

n265_ctl *n265_ctl_new(const struct n265_encoder *e);
void n265_ctl_free(n265_ctl *c);

void n265_ctl_hand_in(struct n265_encoder *e);
void n265_ctl_push(struct n265_encoder *e, struct la_frame *f, int64_t d);
int n265_ctl_rate_due(const struct n265_encoder *e, int64_t d);
void n265_ctl_rate_apply(struct n265_encoder *e, int64_t d);
void n265_ctl_emit(struct n265_encoder *e, int64_t disp);
void n265_ctl_flush_end(struct n265_encoder *e);

/* The echo strings for display frame d, as one JSON array body (without
 * the brackets) into buf; 0 when the frame applied no event. */
int n265_ctl_echo_take(struct n265_encoder *e, int64_t d, char *buf,
                       size_t n);

#endif
