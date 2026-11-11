/*
 * Sample adaptive offset: per-CTB analysis, syntax, application.
 * Copyright (c) 2026, the yah265 authors. GPL-2.0-or-later; see LICENSE.
 */

#ifndef N265_SAO_H
#define N265_SAO_H

#include "encoder/encoder.h"

enum { SAO_OFF = 0, SAO_BAND = 1, SAO_EDGE = 2 };

typedef struct sao_params {
    uint8_t merge;          /* 0 explicit, 1 left, 2 up */
    uint8_t type[3];        /* per component; [2] mirrors [1]'s type   */
    uint8_t class_or_pos[3];/* eo class (0..3) or band position (0..31) */
    int8_t offset[3][4];    /* band: signed; edge: category 1..4 magnitudes
                             * stored signed with the inferred sign applied */
} sao_params;

/* May the filter be used ANYWHERE in this sequence? This is the SPS
 * flag: N265_SAO_TYPES=0 with the content gate disarmed clears it, and
 * then no slice flag and no sao() syntax is coded and neither the
 * analysis nor the application pass ever runs. With the gate armed the
 * flag is set even for a sequence no frame of which turns out to be
 * flat, because nothing here can see the whole sequence in advance; the
 * two slice flags below then carry the per-frame answer. */
int n265_sao_seq_enabled(void);

/* The SAO type mask this frame may pick from - 0 for the filter off
 * over the whole frame. N265_SAO_TYPES everywhere, plus N265_SAO_ANIM
 * on a frame the content gate (la.h) typed flat-shaded. */
int n265_sao_frame_mask(const struct n265_frame_work *e);

/* The sequence's two type masks, for the resolved-configuration table
 * (phase 14 item 13): what every frame may pick from, and what the
 * content gate adds on a flat-typed frame (0 where the gate is off). */
void n265_sao_masks(int *base, int *flat);

/* Decide and apply one CTB row's offsets (docs/threading-design.md
 * sec 1). Analysis and application both see the picture pre-SAO: the
 * one line SAO has already rewritten - the last line of the row above -
 * comes from e->sao_line[]. */
void n265_sao_row(struct n265_frame_work *e, int cy);

/* One CTB component's BAND statistics: for every sample of the w x h
 * block the band is `rec >> 3`, and the sample adds 1 to bn[band] and
 * `src - rec` to bd[band]. `bn` and `bd` are 32 entries each and are
 * ACCUMULATED into, not cleared, so a caller may split a block by rows.
 * This is the whole of the SAO analysis pass on a band-only frame, which
 * since R7 is every frame the filter runs on at all. Class N265_ASM_SAO. */
void n265_sao_band_stats(const pixel *src, int src_stride,
                         const pixel *rec, int rec_stride, int w, int h,
                         int64_t bn[32], int64_t bd[32]);

/* One row of the edge offset (class N265_ASM_SAO). Sample x of `cur` has
 * its two neighbors at a[x] and b[x]; the caller points `a` and `b` at the
 * rows and columns the class names. The stats add each categorized
 * sample's count to en[1..4] and `src - cur` to ed[1..4]. The apply writes
 * dst[x] = clip1(cur[x] + off[cat - 1]) where the category is nonzero and
 * leaves the rest of `dst` alone. */
void n265_sao_eo_stats(const pixel *src, const pixel *cur, const pixel *a,
                       const pixel *b, int n, int64_t en[5], int64_t ed[5]);
void n265_sao_eo_apply(pixel *dst, const pixel *cur, const pixel *a,
                       const pixel *b, int n, const int8_t off[4]);

/* One row of the band offset: where in[x]'s band less `pos` is 0..3,
 * dst[x] = clip1(in[x] + off[band - pos]); elsewhere `dst` is untouched. */
void n265_sao_bo_apply(pixel *dst, const pixel *in, int n, int pos,
                       const int8_t off[4]);

/* the C references of the four kernels above */
void n265_sao_band_stats_ref(const pixel *src, int src_stride,
                             const pixel *rec, int rec_stride, int w, int h,
                             int64_t bn[32], int64_t bd[32]);
void n265_sao_eo_stats_ref(const pixel *src, const pixel *cur,
                           const pixel *a, const pixel *b, int n,
                           int64_t en[5], int64_t ed[5]);
void n265_sao_eo_apply_ref(pixel *dst, const pixel *cur, const pixel *a,
                           const pixel *b, int n, const int8_t off[4]);
void n265_sao_bo_apply_ref(pixel *dst, const pixel *in, int n, int pos,
                           const int8_t off[4]);

/* Write one CTB's sao() syntax (7.3.8.3). */
void n265_sao_write(struct n265_frame_work *e, cabac_t *cb, int ctb_x,
                    int ctb_y);

#endif
