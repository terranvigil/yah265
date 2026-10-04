/*
 * Weighted prediction (7.3.6.3 pred_weight_table, 8.5.3.3.4.3): the
 * per-reference weight estimate and the table it fills in.
 * Copyright (c) 2026, the yah265 authors. GPL-2.0-or-later; see LICENSE.
 */

#ifndef N265_WP_H
#define N265_WP_H

#include <stdint.h>

#include "common/pixel.h"

struct n265_encoder;
struct n265_frame_work;
struct n265_refpic;
struct la_frame;

/* One reference's explicit weights, per component (0 luma, 1 Cb, 2 Cr).
 *
 * `w` is the weight AT THE TABLE'S DENOMINATOR - LumaWeightLX /
 * ChromaWeightLX of 7.4.7.3, not the syntax element, which is the delta
 * from 1 << denom - and `o` is the offset in the syntax's own units,
 * which are 8-BIT LEVELS at every depth (WpOffsetBdShiftY scales it on
 * the way into 8.5.3.3.4.3, because sps_high_precision_offsets is 0).
 *
 * `on[c]` 0 is "this component carries no weight": the flag is written
 * 0 and the derivation infers w = 1 << denom, o = 0, which is exactly
 * the unweighted prediction. So an all-off table is a no-op on the
 * samples and costs only its own syntax. */
typedef struct n265_wp {
    uint8_t on[3];
    int16_t w[3];
    int16_t o[3];
} n265_wp;

/* Is the weighting for this (list, ref, component) anything but the
 * identity? The MC path asks per block and takes the plain filter where
 * the answer is no, so a frame with no weights costs nothing. */
static inline int n265_wp_active(const n265_wp *wp, int c)
{
    return wp && wp->on[c];
}

/* --weightp / --weightb, resolved at open: the PPS flags. */
int n265_weightp_on(const struct n265_encoder *e);
int n265_weightb_on(const struct n265_encoder *e);

/* N265_WP_TRACE: one line per (frame, list, ref) the estimator looked
 * at, saying what it derived and whether it fired. Output-neutral. */
int n265_wp_trace(void);
void n265_wp_warm(void);

/* The lowres planes the estimator reads. Built on the driver from the
 * lookahead slot's padded source, once per frame, into e->wp_cur[]; the
 * DPB slot keeps a copy of its own occupant's (n265_wp_slot_store) so
 * that a frame dispatched later can read the picture its lists name
 * WITHOUT touching a lookahead slot that has since been recycled. */
void n265_wp_build_cur(struct n265_encoder *e, const struct la_frame *f);
void n265_wp_slot_store(struct n265_encoder *e, struct n265_refpic *slot);

/* Derive this frame's whole pred_weight_table. Driver-serial, at
 * dispatch, after the reference lists are built and after
 * n265_wp_build_cur: everything it reads is frozen by then, so the table
 * is a pure function of the sources and is the same at every thread
 * count. Fills fw->wp / fw->wp_ldenom / fw->wp_cdenom / fw->wp_any. */
void n265_wp_analyse(struct n265_encoder *e, struct n265_frame_work *fw);

/* The three pieces of the derivation, exposed so tests/unit/wp_test.c
 * can check them against hand values rather than against an encode.
 *
 *  n265_wp_fit        the two planes' moments as a (scale, offset) map
 *  n265_wp_denom_for  the finest denominator 0..7 that can WRITE that
 *                     scale, the syntax element being a delta from
 *                     1 << d bounded at -128..127 (7.4.7.3)
 *  n265_wp_quant      the weight itself at a chosen denominator */
void n265_wp_fit(const pixel *cur, const pixel *ref, int n,
                 double *scale, double *off);
int n265_wp_denom_for(double scale);
int n265_wp_quant(double scale, int denom);

#endif /* N265_WP_H */
