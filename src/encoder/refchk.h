/*
 * N265_REF_CHECK: the independent proof that the exact reference stalls
 * of G2 cover every reference read.
 *
 * ref_wait computes the bottom line a read will touch and waits for the
 * reference's watermark to cover it. That computation lives at the CALL
 * SITE, so it can be wrong in exactly the way the T2/3 refusal found the
 * sec 1 gate to be wrong. This checker closes the loop from the other
 * end: the guard says "lines up to L of this plane, and nothing else",
 * and the interpolation and SAD kernels themselves - which own the
 * filter taps - report the extreme lines they actually read. A read past
 * L aborts, and so does a reference read with no guard armed at all,
 * which is what would happen if an entry point were missed.
 *
 * Compiled only into a build configured with -DN265_REF_CHECK, so the
 * shipping binary carries none of it. The identity set is run against
 * that build at t12.
 *
 * Copyright (c) 2026, the yah265 authors. GPL-2.0-or-later; see LICENSE.
 */

#ifndef N265_REFCHK_H
#define N265_REFCHK_H

#ifdef N265_REF_CHECK

#include <stdint.h>

/* Arm the guard for the current reference read. `last` is the last line
 * index of that plane the reader may touch; anything above the plane
 * origin (the top border) is always allowed, because a picture with any
 * published row has published its top border. */
void n265_refchk_open(const uint8_t *plane, int stride, long last);
void n265_refchk_open_hf(const int16_t *plane, int stride, long last);
/* Back to "no reference read is allowed on this thread". */
void n265_refchk_close(void);
/* The publish path reads its OWN picture (border extension, the E1 cache
 * fill); those reads are not gated and are bracketed by these. */
void n265_refchk_open_all(void);

void n265_refchk_touch(const uint8_t *p, const char *fn);
void n265_refchk_touch16(const int16_t *p, const char *fn);

#define N265_RC_TOUCH(p)    n265_refchk_touch((const uint8_t *)(p), __func__)
#define N265_RC_TOUCH16(p)  n265_refchk_touch16((const int16_t *)(p), __func__)

#else

#define N265_RC_TOUCH(p)    ((void)0)
#define N265_RC_TOUCH16(p)  ((void)0)

#endif

#endif
