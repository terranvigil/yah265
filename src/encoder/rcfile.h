/*
 * --qpfile and --zones (phase 14 item 4): the two ways a user pins a
 * quantiser onto a range of the timeline from outside the rate control.
 *
 * Pure parsing, no encoder and no pixels, so tests/unit/rcfile_test.c can
 * check both grammars outright - including every refusal, which is half
 * of what a file format is.
 *
 * Both index the timeline in DISPLAY order, which is the order the user
 * watched the clip in and the only order a frame number means anything
 * in. The encoder looks them up by `display_idx`.
 *
 * Copyright (c) 2026, the yah265 authors. GPL-2.0-or-later; see LICENSE.
 */

#ifndef N265_RCFILE_H
#define N265_RCFILE_H

#include <stddef.h>

/* The frame types a qpfile may force. x264's and x265's letters; the
 * three that name a B or P frame are PARSED and then REFUSED, because
 * what they ask for is a different mini-GOP and the run length belongs to
 * the plan builder, not to a file. Refusing by name is the point: a
 * silently ignored `P` reads as a qpfile that did not work. */
enum {
    N265_QPF_AUTO = 0,      /* '-': the encoder decides */
    N265_QPF_IDR,           /* 'I' or 'K' */
    N265_QPF_I,             /* 'i': a non-IDR intra picture */
};

typedef struct n265_qpent {
    int frame;              /* display index */
    int type;               /* N265_QPF_* */
    double qp;              /* < 0 means "type only, leave the QP alone" */
} n265_qpent;

typedef struct n265_qpfile {
    n265_qpent *e;
    int n, cap;
} n265_qpfile;

/* 0, or -1 with a one-line reason in `err`. The entries come back sorted
 * by frame and a duplicate frame number is a refusal. */
int n265_qpfile_parse(n265_qpfile *q, const char *path, char *err,
                      size_t errn);
void n265_qpfile_free(n265_qpfile *q);
/* the entry for a display index, or NULL */
const n265_qpent *n265_qpfile_at(const n265_qpfile *q, int frame);

typedef struct n265_zone {
    int start, end;         /* display indices, inclusive */
    int has_q;              /* q=N was given */
    double q;
    double b;               /* b=F; 1.0 when it was not given */
} n265_zone;

typedef struct n265_zones {
    n265_zone *z;
    int n, cap;
} n265_zones;

/* x265's syntax: zones separated by '/', each `start,end,q=N` or
 * `start,end,b=F`. Overlapping zones are refused rather than resolved by
 * an order the syntax does not state. */
int n265_zones_parse(n265_zones *zs, const char *spec, char *err,
                     size_t errn);
void n265_zones_free(n265_zones *zs);
const n265_zone *n265_zones_at(const n265_zones *zs, int frame);

#endif
