/*
 * sc.h - our scene-cut for the hardware mode, run causally on each
 * picture as it arrives (docs/hardware-mode.md).
 * Copyright (c) 2026, the yah265 authors. GPL-2.0-or-later; see LICENSE.
 */

#ifndef N265_HW_SC_H
#define N265_HW_SC_H

#include "yah265.h"

struct n265_hwsc;

/* keyint <= 0: no interval of our own */
struct n265_hwsc *n265_hwsc_new(int width, int height, int keyint);
/* 1 when this picture should be a key frame */
int n265_hwsc_push(struct n265_hwsc *s, const n265_picture *pic);
void n265_hwsc_free(struct n265_hwsc *s);

#endif
