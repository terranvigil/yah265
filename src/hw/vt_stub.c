/*
 * vt_stub.c - the hardware mode where there is no VideoToolbox.
 * Copyright (c) 2026, the yah265 authors. GPL-2.0-or-later; see LICENSE.
 */

#include <stdio.h>

#include "hw/vt.h"

struct n265_hw *n265_hw_open(const n265_hw_cfg *c, char *why, size_t whylen)
{
    (void)c;
    if (why && whylen)
        snprintf(why, whylen, "this build has no VideoToolbox (not macOS, "
                 "or -Dvideotoolbox=disabled)");
    return NULL;
}

int n265_hw_headers(struct n265_hw *h, n265_nal **nal, int *nnal)
{
    (void)h; (void)nal; (void)nnal;
    return -1;
}

int n265_hw_encode(struct n265_hw *h, n265_nal **nal, int *nnal,
                   const n265_picture *pic, int force_key)
{
    (void)h; (void)nal; (void)nnal; (void)pic; (void)force_key;
    return -1;
}

int n265_hw_access_units(const struct n265_hw *h, const n265_au **au)
{
    (void)h; (void)au;
    return -1;
}

int n265_hw_reorder_depth(const struct n265_hw *h)
{
    (void)h;
    return -1;
}

void n265_hw_close(struct n265_hw *h)
{
    (void)h;
}

const char *n265_hw_name(const struct n265_hw *h)
{
    (void)h;
    return "none";
}
