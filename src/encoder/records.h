/*
 * CTU record stream: the analysis walk appends decisions and coefficient
 * levels; the trailing entropy pass replays them in the same order.
 * Structure (splits, skip, modes)
 * lives in the frame maps; the stream holds what maps can't: part flags,
 * cbfs at every transform-tree node, and levels.
 * Copyright (c) 2026, the yah265 authors. GPL-2.0-or-later; see LICENSE.
 */

#ifndef N265_RECORDS_H
#define N265_RECORDS_H

#include <stdlib.h>
#include <string.h>

#include "encoder/encoder.h"

static inline void rec_reset(struct n265_frame_work *e)
{
    e->rec_len = 0;
    e->rec_pos = 0;
}

static inline void rec_grow(struct n265_frame_work *e, size_t need)
{
    if (e->rec_len + need <= e->rec_cap)
        return;
    size_t cap = e->rec_cap ? e->rec_cap : 1 << 16;
    while (cap < e->rec_len + need)
        cap *= 2;
    e->rec_buf = realloc(e->rec_buf, cap);
    e->rec_cap = cap;
}

static inline void rec_u8(struct n265_frame_work *e, unsigned v)
{
    rec_grow(e, 1);
    e->rec_buf[e->rec_len++] = (uint8_t)v;
}

static inline void rec_levels(struct n265_frame_work *e, const int16_t *l,
                              int count)
{
    rec_grow(e, (size_t)count * 2 + 1);
    e->rec_len += e->rec_len & 1;   /* keep int16 payloads 2-aligned */
    memcpy(e->rec_buf + e->rec_len, l, (size_t)count * 2);
    e->rec_len += (size_t)count * 2;
}

static inline unsigned rd_u8(struct n265_frame_work *e)
{
    return e->rec_buf[e->rec_pos++];
}

static inline const int16_t *rd_levels(struct n265_frame_work *e, int count)
{
    e->rec_pos += e->rec_pos & 1;
    const void *p = e->rec_buf + e->rec_pos;
    e->rec_pos += (size_t)count * 2;
    return (const int16_t *)p;
}

#endif
