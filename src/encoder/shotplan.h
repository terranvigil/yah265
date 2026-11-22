/*
 * --shot-plan: one base-QP offset per shot, from the scan's features.
 * Copyright (c) 2026, the yah265 authors. GPL-2.0-or-later; see LICENSE.
 */

#ifndef N265_SHOTPLAN_H
#define N265_SHOTPLAN_H

#include <stddef.h>

#include "encoder/shotlist.h"

/* the model's version, written to the shot-stats sidecar */
#define N265_SHOTPLAN_MODEL 1
/* no shot moves further than this from the title's level */
#define N265_SHOTPLAN_CLAMP 4.0
/* a shot shorter than this takes the offset of the shot before it */
#define N265_SHOTPLAN_MINLEN 24

/* Fills off[0..l->n-1]. A list whose every shot
 * gives qp= is taken as written; otherwise every shot needs the scan's
 * features. -1 with the reason in `err` when the list carries neither. */
int n265_shotplan(const n265_shotlist *l, double *off,
                  char *err, size_t cap);

#endif
