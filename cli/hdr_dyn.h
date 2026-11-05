/*
 * Per-picture HDR metadata files for the command line: HDR10+ (SMPTE ST
 * 2094-40) from the JSON hdr10plus_tool writes, and Dolby Vision RPUs from
 * the file dovi_tool writes. Both are read whole at start-up and handed to
 * the library as one entry per picture in display order.
 * Copyright (c) 2026, the yah265 authors. GPL-2.0-or-later; see LICENSE.
 */

#ifndef N265_CLI_HDR_DYN_H
#define N265_CLI_HDR_DYN_H

#include <stddef.h>
#include <stdint.h>

typedef struct n265_dyn_tab {
    int n;
    uint8_t **buf;      /* entry i is picture i's payload, or NULL */
    size_t *len;
} n265_dyn_tab;

/* Read an HDR10+ JSON file into one T.35 payload per frame. 0 on success;
 * otherwise -1 with the reason in `err`. */
int n265_h10p_load(const char *path, n265_dyn_tab *t, char *err, size_t errlen);

/* Read a Dolby Vision RPU file (Annex B NAL units of type 62) into one
 * RBSP per frame. 0 on success; otherwise -1 with the reason in `err`. */
int n265_rpu_load(const char *path, n265_dyn_tab *t, char *err, size_t errlen);

/* Build one ST 2094-40 payload, with its T.35 header, from the fields of
 * one frame. Exposed for the unit test. Returns the byte count, 0 if `out`
 * is too small. */
typedef struct n265_h10p_frame {
    uint32_t target_max;        /* targeted_system_display_maximum_luminance */
    uint32_t maxscl[3];
    uint32_t avg_maxrgb;
    int npct;
    uint32_t pct[15], pct_val[15];
    int tone_mapping;
    uint32_t knee_x, knee_y;
    int nanchor;
    uint32_t anchor[15];
} n265_h10p_frame;

size_t n265_h10p_payload(const n265_h10p_frame *f, uint8_t *out, size_t cap);

/* The Dolby Vision profile an RPU's header says (4, 5, 7 or 8), or 0 when
 * it isn't one. Exposed for the unit test. */
int n265_rpu_profile(const uint8_t *rbsp, size_t len);

void n265_dyn_free(n265_dyn_tab *t);

#endif
