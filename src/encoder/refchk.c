/*
 * The N265_REF_CHECK guard. See refchk.h.
 * Copyright (c) 2026, the yah265 authors. GPL-2.0-or-later; see LICENSE.
 */

#include "encoder/refchk.h"

/* ISO C wants a declaration even when the checker is compiled out. */
extern int n265_refchk_compiled_out;
int n265_refchk_compiled_out;

#ifdef N265_REF_CHECK

#include <stdio.h>
#include <stdlib.h>

enum { RC_CLOSED = 0, RC_OPEN, RC_GUARD };

static _Thread_local int rc_mode;
static _Thread_local const uint8_t *rc_plane;
static _Thread_local int rc_stride;
static _Thread_local long rc_last;
static _Thread_local const int16_t *rc_hf;
static _Thread_local int rc_hf_stride;
static _Thread_local long rc_hf_last;

void n265_refchk_open(const uint8_t *plane, int stride, long last)
{
    rc_mode = RC_GUARD;
    rc_plane = plane;
    rc_stride = stride;
    rc_last = last;
    rc_hf = NULL;
}

void n265_refchk_open_hf(const int16_t *plane, int stride, long last)
{
    rc_hf = plane;
    rc_hf_stride = stride;
    rc_hf_last = last;
}

void n265_refchk_close(void)
{
    rc_mode = RC_CLOSED;
    rc_plane = NULL;
    rc_hf = NULL;
}

void n265_refchk_open_all(void)
{
    rc_mode = RC_OPEN;
}

/* floor division: a read three lines above the plane origin is line -1,
 * not 0, and the top border is legal wherever the guard is armed */
static long line_of(long delta, int stride)
{
    long q = delta / stride;
    if (delta % stride < 0)
        q--;
    return q;
}

static void die(const char *fn, const char *what, long line, long last)
{
    fprintf(stderr,
            "N265_REF_CHECK: %s in %s: line %ld, guard allows up to %ld\n",
            what, fn ? fn : "?", line, last);
    fflush(stderr);
    abort();
}

void n265_refchk_touch(const uint8_t *p, const char *fn)
{
    if (rc_mode == RC_OPEN)
        return;
    if (rc_mode != RC_GUARD || !rc_plane)
        die(fn, "UNGUARDED reference read", 0, -1);
    long line = line_of((long)(p - rc_plane), rc_stride);
    if (line > rc_last)
        die(fn, "reference read past the published watermark", line, rc_last);
}

void n265_refchk_touch16(const int16_t *p, const char *fn)
{
    if (rc_mode == RC_OPEN)
        return;
    if (rc_mode != RC_GUARD || !rc_hf)
        die(fn, "UNGUARDED E1 cache read", 0, -1);
    long line = line_of((long)(p - rc_hf), rc_hf_stride);
    if (line > rc_hf_last)
        die(fn, "E1 cache read past the published watermark", line,
            rc_hf_last);
}

#endif
