/*
 * vt.h - the hardware mode: Apple's fixed-function HEVC encoder through
 * VideoToolbox, behind the n265 calls (docs/hardware-mode.md). The stream
 * is the hardware's, not ours, so none of the recon, conformance or
 * determinism gates apply to it. Builds without VideoToolbox compile
 * vt_stub.c, whose open fails with a reason.
 * Copyright (c) 2026, the yah265 authors. GPL-2.0-or-later; see LICENSE.
 */

#ifndef N265_HW_VT_H
#define N265_HW_VT_H

#include <stddef.h>
#include <stdint.h>

#include "yah265.h"

/* what the session is opened with; -1 is "not given" where it applies */
typedef struct n265_hw_cfg {
    int width, height, csp, depth;
    int fps_num, fps_den;
    int keyint, bframes, realtime;
    int crf_on;
    double crf;
    int qp;
    double bitrate, vbv_maxrate, vbv_bufsize;   /* kbit/s, kbit */
    int colorprim, transfer, colormatrix, full_range;
    int sar_w, sar_h;
    int have_md;
    uint16_t md_prim[3][2], md_wp[2];
    uint32_t md_max, md_min;
    int have_cll, maxcll, maxfall;
    int repeat_headers;
} n265_hw_cfg;

struct n265_hw;

/* NULL with a short reason in `why` when there is no hardware session.
 * VideoToolbox's software encoder is never accepted. */
struct n265_hw *n265_hw_open(const n265_hw_cfg *c, char *why, size_t whylen);
int n265_hw_headers(struct n265_hw *h, n265_nal **nal, int *nnal);
/* pic == NULL flushes. Bytes out, or -1. NALs are Annex B in decode
 * order, valid until the next call. */
int n265_hw_encode(struct n265_hw *h, n265_nal **nal, int *nnal,
                   const n265_picture *pic, int force_key);
int n265_hw_access_units(const struct n265_hw *h, const n265_au **au);
int n265_hw_reorder_depth(const struct n265_hw *h);
void n265_hw_close(struct n265_hw *h);
const char *n265_hw_name(const struct n265_hw *h);

#endif
