/*
 * The value tables and the string forms of the pass-through signalling
 * flags. Phase 14 item 1.
 * Copyright (c) 2026, the yah265 authors. GPL-2.0-or-later; see LICENSE.
 *
 * The tables ARE the spec's, Tables E.2 to E.5 of ITU-T H.265, with the
 * spellings x265 gives them so a command line moves between the two
 * encoders unchanged. Index equals code everywhere, including the
 * "reserved" entries: dropping them would shift every value after them,
 * which is the one way a colour table can be wrong without anything
 * failing to parse.
 *
 * Bare numbers are accepted too, as x265 accepts them, and are bounded by
 * the syntax element's own width rather than by the table's length - a
 * transfer characteristic of 19 is a value this encoder has no name for
 * and a decoder is entitled to see, and refusing it would make the
 * numeric form less useful than the named one instead of more.
 *
 * Nothing in this file is bit-depth dependent, but it is compiled into
 * BOTH libraries and namespaced like everything else (symbols10.h); the
 * CLI calls the 8-bit copy, which is what src/encoder/level.c's parser
 * does for the same reason.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "encoder/signal.h"

/* Table E.3, colour_primaries. Index is the code; 0 and 3 are reserved
 * and 2 is "unspecified", which x265 spells "unknown". */
static const char * const colorprim_names[] = {
    "reserved", "bt709", "unknown", "reserved", "bt470m", "bt470bg",
    "smpte170m", "smpte240m", "film", "bt2020", "smpte428", "smpte431",
    "smpte432", NULL
};

/* Table E.4, transfer_characteristics. 16 is PQ (SMPTE ST 2084) and 18 is
 * HLG (ARIB STD-B67): the two values without which no HDR stream can be
 * tagged at all, which is what put this item in front of the HDR corpus. */
static const char * const transfer_names[] = {
    "reserved", "bt709", "unknown", "reserved", "bt470m", "bt470bg",
    "smpte170m", "smpte240m", "linear", "log100", "log316",
    "iec61966-2-4", "bt1361e", "iec61966-2-1", "bt2020-10", "bt2020-12",
    "smpte2084", "smpte428", "arib-std-b67", NULL
};

/* Table E.5, matrix_coeffs. 0 is the identity (GBR), not a reserved
 * value, which is why this table starts differently from the two above. */
static const char * const colormatrix_names[] = {
    "gbr", "bt709", "unknown", "reserved", "fcc", "bt470bg", "smpte170m",
    "smpte240m", "ycgco", "bt2020nc", "bt2020c", "smpte2085",
    "chroma-derived-nc", "chroma-derived-c", "ictcp", "ipt-pq-c2", NULL
};

/* Table E.2, video_format. "unknown" is 5 and is the default the syntax
 * infers when the field is absent. */
static const char * const videoformat_names[] = {
    "component", "pal", "ntsc", "secam", "mac", "unknown", NULL
};

/* video_full_range_flag. */
static const char * const range_names[] = { "limited", "full", NULL };

/* --overscan. "unknown" is not a VALUE - it is the absence of the field,
 * and the parser hands back 2 for it so the caller can tell "do not write
 * overscan_info" from "write it as 0". */
static const char * const overscan_names[] = { "show", "crop", "unknown",
                                               NULL };

static const struct {
    const char * const *names;
    int max;                /* the widest value the syntax element holds */
} vui_field[N265_VUI_NFIELD] = {
    { colorprim_names,   255 },
    { transfer_names,    255 },
    { colormatrix_names, 255 },
    { videoformat_names,   7 },     /* u(3) */
    { range_names,         1 },
    { overscan_names,      2 },     /* 2 is this parser's "absent" */
};

const char * const *n265_vui_names(int field)
{
    if (field < 0 || field >= N265_VUI_NFIELD)
        return NULL;
    return vui_field[field].names;
}

int n265_vui_parse(int field, const char *s)
{
    if (field < 0 || field >= N265_VUI_NFIELD || !s || !*s)
        return -1;
    const char * const *n = vui_field[field].names;
    for (int i = 0; n[i]; i++)
        if (!strcmp(s, n[i]))
            return i;
    /* the numeric form. strtol and not atoi, so "bt709x" is a refusal
     * rather than a silent 0. */
    char *end;
    long v = strtol(s, &end, 10);
    if (end == s || *end || v < 0 || v > vui_field[field].max)
        return -1;
    return (int)v;
}

/* Table E.1 numbers sixteen defined ratios; 0 is Unspecified and 255 is
 * EXTENDED_SAR, which carries an explicit pair instead.
 *
 * The two spellings mean different things, here as in x265: a NUMBER is
 * the Table E.1 index and goes on the wire as itself, and a `w:h` pair is
 * EXTENDED_SAR with that pair after it - `--sar 16:9` writes 255, 16, 9
 * and not the idc of some table row that happens to reduce the same way.
 * Verified against x265 on the wire before this was written. */
#define N265_SAR_IDC_MAX    16

int n265_sar_parse(const char *s, int *idc, int *w, int *h)
{
    if (!s || !*s)
        return -1;
    const char *colon = strchr(s, ':');
    if (colon) {
        char *end;
        long a = strtol(s, &end, 10);
        if (end != colon || a <= 0 || a > 65535)
            return -1;
        long b = strtol(colon + 1, &end, 10);
        if (end == colon + 1 || *end || b <= 0 || b > 65535)
            return -1;
        *idc = 255;             /* EXTENDED_SAR */
        *w = (int)a;
        *h = (int)b;
        return 0;
    }
    char *end;
    long v = strtol(s, &end, 10);
    if (end == s || *end || v < 0 || v > N265_SAR_IDC_MAX)
        return -1;
    *idc = (int)v;
    *w = *h = 0;
    return 0;
}

int n265_window_parse(const char *s, int *l, int *t, int *r, int *b)
{
    int v[4];
    const char *p = s;
    for (int i = 0; i < 4; i++) {
        char *end;
        long x = strtol(p, &end, 10);
        if (end == p || x < 0 || x > (1 << 20))
            return -1;
        v[i] = (int)x;
        p = end;
        if (i < 3) {
            if (*p != ',')
                return -1;
            p++;
        }
    }
    if (*p)
        return -1;
    *l = v[0];
    *t = v[1];
    *r = v[2];
    *b = v[3];
    return 0;
}

/* One "NAME(a,b)" group of x265's mastering display string. Returns the
 * character after the group, or NULL. */
static const char *md_group(const char *p, const char *tag, uint32_t *a,
                            uint32_t *b)
{
    size_t n = strlen(tag);
    while (*p == ' ')
        p++;
    if (strncmp(p, tag, n))
        return NULL;
    p += n;
    if (*p++ != '(')
        return NULL;
    char *end;
    unsigned long x = strtoul(p, &end, 10);
    if (end == p || *end != ',')
        return NULL;
    p = end + 1;
    unsigned long y = strtoul(p, &end, 10);
    if (end == p || *end != ')')
        return NULL;
    *a = (uint32_t)x;
    *b = (uint32_t)y;
    return end + 1;
}

int n265_master_display_parse(const char *s, uint32_t v[10])
{
    if (!s)
        return -1;
    static const char * const tag[5] = { "G", "B", "R", "WP", "L" };
    const char *p = s;
    for (int i = 0; i < 5; i++) {
        p = md_group(p, tag[i], &v[2 * i], &v[2 * i + 1]);
        if (!p)
            return -1;
    }
    while (*p == ' ')
        p++;
    if (*p)
        return -1;
    /* The four chromaticity pairs are u(16) in units of 0.00002, so 50000
     * is 1.0 and anything above 65535 is not representable at all. The
     * two luminances are u(32) in units of 0.0001 cd/m2, and a bound
     * there would be a bound on the display and not on the syntax. */
    for (int i = 0; i < 8; i++)
        if (v[i] > 65535)
            return -1;
    return 0;
}

int n265_cll_parse(const char *s, int *cll, int *fall)
{
    if (!s)
        return -1;
    char *end;
    long a = strtol(s, &end, 10);
    if (end == s || *end != ',' || a < 0 || a > 65535)
        return -1;
    const char *p = end + 1;
    long b = strtol(p, &end, 10);
    if (end == p || *end || b < 0 || b > 65535)
        return -1;
    *cll = (int)a;
    *fall = (int)b;
    return 0;
}

int n265_hdr10_on(const n265_signal *s)
{
    /* x265's rule, and it is a sensible one: naming either message's
     * values is what turns the packet on, and --hdr10 / --no-hdr10 is the
     * override in both directions. With neither message's values given
     * there is nothing to write and --hdr10 writes nothing. */
    if (s->hdr10 == 0)
        return 0;
    return s->have_md || s->have_cll;
}

int n265_vui_needed(const n265_signal *s)
{
    return s->sar_idc >= 0 || s->overscan >= 0 || s->videoformat >= 0 ||
           s->full_range >= 0 || s->colorprim >= 0 || s->transfer >= 0 ||
           s->colormatrix >= 0 || s->chromaloc >= 0 || s->win_on;
}
