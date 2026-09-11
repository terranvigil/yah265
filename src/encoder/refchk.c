/*
 * The N265_REF_CHECK guard. See refchk.h.
 * Copyright (c) 2026, the yah265 authors. GPL-2.0-or-later; see LICENSE.
 */

#include "encoder/refchk.h"
#include "encoder/encoder.h"

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
static _Thread_local const uint8_t *rc_p2;
static _Thread_local int rc_p2_stride;
static _Thread_local long rc_p2_last;
static _Thread_local const int16_t *rc_p2w;
static _Thread_local int rc_p2w_stride;
static _Thread_local long rc_p2w_last;

void n265_refchk_open(const uint8_t *plane, int stride, long last)
{
    rc_mode = RC_GUARD;
    rc_plane = plane;
    rc_stride = stride;
    rc_last = last;
    rc_hf = NULL;
    rc_p2 = NULL;
    rc_p2w = NULL;
}

void n265_refchk_open_hf(const int16_t *plane, int stride, long last)
{
    rc_hf = plane;
    rc_hf_stride = stride;
    rc_hf_last = last;
}

void n265_refchk_open_p2(const uint8_t *plane, int stride, long last)
{
    rc_p2 = plane;
    rc_p2_stride = stride;
    rc_p2_last = last;
}

void n265_refchk_open_p214(const int16_t *plane, int stride, long last)
{
    rc_p2w = plane;
    rc_p2w_stride = stride;
    rc_p2w_last = last;
}

void n265_refchk_close(void)
{
    rc_mode = RC_CLOSED;
    rc_plane = NULL;
    rc_hf = NULL;
    rc_p2 = NULL;
    rc_p2w = NULL;
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

void n265_refchk_touch_p2(const uint8_t *p, const char *fn)
{
    if (rc_mode == RC_OPEN)
        return;
    if (rc_mode != RC_GUARD || !rc_p2)
        die(fn, "UNGUARDED round-3 plane read", 0, -1);
    long line = line_of((long)(p - rc_p2), rc_p2_stride);
    if (line > rc_p2_last)
        die(fn, "round-3 plane read past the published watermark", line,
            rc_p2_last);
}

void n265_refchk_touch16_p2(const int16_t *p, const char *fn)
{
    if (rc_mode == RC_OPEN)
        return;
    if (rc_mode != RC_GUARD || !rc_p2w)
        die(fn, "UNGUARDED round-4 int16 plane read", 0, -1);
    long line = line_of((long)(p - rc_p2w), rc_p2w_stride);
    if (line > rc_p2w_last)
        die(fn, "round-4 int16 plane read past the published watermark",
            line, rc_p2w_last);
}

/* The side-data half. `row` is the CTB row that owns the array element
 * being read, derived from the element's own index. Two things must
 * hold, and neither of them goes through the call site's ref_wait
 * arithmetic:
 *
 *  1. the row is below the picture's published watermark, so its side
 *     data has been written at all;
 *  2. the generation that wrote it is the generation this frame froze
 *     when its reference lists were built - i.e. the slot has not been
 *     re-claimed by a later frame while this one was still reading it.
 *
 * The acquire load of final_rows happens first so that row_gen[] is read
 * on the far side of the publish task's release store. */
void n265_side_check(const n265_frame_work *e, const struct n265_refpic *pic,
                     unsigned gen, int row, const char *what)
{
    (void)e;
    int have = atomic_load_explicit((atomic_int *)&pic->final_rows,
                                    memory_order_acquire);
    if (gen == 0)
        die(what, "side-data read of a picture not in the frozen set",
            row, -1);
    if (row < 0) {      /* a scalar read (poc): generation only */
        if (pic->gen != gen)
            die(what, "scalar read of a RE-CLAIMED slot",
                (long)pic->gen, (long)gen);
        return;
    }
    if (row >= have)
        die(what, "side-data read above the published watermark", row,
            have - 1);
    if (!pic->row_gen || pic->row_gen[row] != gen)
        die(what, "side-data read of a RE-CLAIMED slot (generation moved)",
            (long)(pic->row_gen ? pic->row_gen[row] : 0), (long)gen);
    if (pic->gen != gen)
        die(what, "side-data read while the slot is being re-claimed",
            (long)pic->gen, (long)gen);
}

#endif
