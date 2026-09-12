/*
 * pixel.h - the sample type and everything derived from its width.
 *
 * Phase 8 item 1. The pixel is a COMPILE-TIME type: one encoder library
 * per bit depth, selected by the meson option `bit_depth` (8 or 10),
 * which defines N265_BIT_DEPTH project-wide. The 8-bit library compiles
 * to exactly the code it compiled to before this typedef existed -
 * `pixel` IS `uint8_t` there and every constant below folds to the
 * literal that used to be written out - which is what lets the campaign
 * carry the standing invariant that no 8-bit byte moves.
 *
 * Rules for the retype:
 *   - A buffer that holds SAMPLES is `pixel *`. Coefficients, residuals,
 *     the 14-bit MC intermediates, costs, SADs, and every decision map
 *     keep the types they have.
 *   - Where a width matters - an allocation, a memcpy, a file write -
 *     it reads `sizeof(pixel)`. A `stride` is in SAMPLES, always, and
 *     pointer arithmetic on `pixel *` scales it.
 *   - Anything the spec derives from the bit depth reads BIT_DEPTH
 *     rather than a literal 8 (or 255, or 6 * 0).
 *
 * Copyright (c) 2026, the yah265 authors. GPL-2.0-or-later; see LICENSE.
 */

#ifndef N265_PIXEL_H
#define N265_PIXEL_H

#include <stdint.h>

#include "yah265.h"     /* n265_pixel: the same choice, public side */

/* The sample type. n265_pixel is decided in the installed header so a
 * caller filling n265_picture sees the same width the library does. */
typedef n265_pixel pixel;

/* BitDepthY == BitDepthC: 4:2:0 8-bit or Main10 only, so one constant
 * serves both. Item 2 splits them only if a profile ever needs it. */
#define BIT_DEPTH       N265_BIT_DEPTH

/* (1 << BitDepth) - 1: the clipping bound of Clip1Y/Clip1C (8.5.3). */
#define PIXEL_MAX       ((1 << BIT_DEPTH) - 1)

/* QpBdOffsetY = 6 * bit_depth_luma_minus8 (7-5). Zero at 8-bit, so
 * every `qp + QP_BD_OFFSET` below is the bare `qp` the 8-bit encoder
 * has always indexed with. */
#define QP_BD_OFFSET    (6 * (BIT_DEPTH - 8))

/* The extra precision a sample carries over 8 bits. It is the shift an
 * 8-bit-calibrated proxy (the lookahead's lowres plane) is brought down
 * by, and the one a deblocking tc/beta is scaled up by. */
#define BIT_DEPTH_SHIFT (BIT_DEPTH - 8)

/* ---------------------------------------------------------------------
 * The QP convention (phase 8 item 2), because getting it wrong is a bug
 * that only shows at the bottom of the ladder.
 *
 * EVERY `qp` variable in this encoder is the spec's QpY (or QpC): the
 * 8-BIT-DOMAIN quantiser index, which runs `-QpBdOffsetY .. 51` and is
 * therefore NEGATIVE below QP 0 at Main10. That is the number the user
 * types at `--qp`, the number `slice_qp_delta` carries (7.4.7.1), and
 * the number the deblocking filter indexes Table 8-12 with (8.7.2.5.3,
 * where `qPL` is built from QpY and not from Qp'Y).
 *
 * Anything whose quantity scales with the QUANTISER STEP indexes its
 * table with `qp + QP_BD_OFFSET` instead - the spec's Qp'Y, 0..63 - and
 * that single substitution is exactly right for all of them, because a
 * step, a lambda and a squared step are all powers of two in QP/6,
 * QP/3 and QP/6 and QpBdOffset is 12 = 2*6 = 4*3. So the 8-bit table,
 * extended upward by QP_BD_OFFSET entries of the same formula, IS the
 * 10-bit table: no second calibration, no rescaling of a fitted number.
 * ---------------------------------------------------------------- */

/* QpY's inclusive range (7.4.3.3: init_qp_minus26 is in
 * -(26 + QpBdOffsetY) .. +25, so SliceQpY reaches -QpBdOffsetY). */
#define N265_QP_MIN     (-QP_BD_OFFSET)
#define N265_QP_MAX     51

/* Entries in a table indexed by Qp'Y = QpY + QP_BD_OFFSET: 52 at 8 bits,
 * 64 at Main10. */
#define N265_QP_TAB_N   (N265_QP_MAX + 1 + QP_BD_OFFSET)

/* clip to QpY's range; the 8-bit build folds it to `q < 0 ? 0 : ...`. */
static inline int n265_qp_clip(int q)
{
    return q < N265_QP_MIN ? N265_QP_MIN : q > N265_QP_MAX ? N265_QP_MAX : q;
}

#endif /* N265_PIXEL_H */
