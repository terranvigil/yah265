/*
 * Per-picture HDR metadata files for the command line; see hdr_dyn.h.
 * Copyright (c) 2026, the yah265 authors. GPL-2.0-or-later; see LICENSE.
 *
 * Nothing here is measured or decided. Both files already describe the
 * content picture by picture, and this turns them into the bytes the
 * library places: an ST 2094-40 message per picture for HDR10+, and the
 * RPU's own RBSP per picture for Dolby Vision, which is copied unread.
 */

#include "hdr_dyn.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* far past any real title's metadata (a two-hour HDR10+ JSON is ~60 MB) */
#define DYN_FILE_MAX    (512L << 20)

static int fail(char *err, size_t errlen, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(err, errlen, fmt, ap);
    va_end(ap);
    return -1;
}

static char *slurp(const char *path, long *size, char *err, size_t errlen)
{
    FILE *f = fopen(path, "rb");
    if (!f) {
        fail(err, errlen, "cannot open %s", path);
        return NULL;
    }
    char *buf = NULL;
    long n = -1;
    if (fseek(f, 0, SEEK_END) == 0 && (n = ftell(f)) >= 0 &&
        n <= DYN_FILE_MAX && fseek(f, 0, SEEK_SET) == 0) {
        buf = malloc((size_t)n + 1);
        if (buf && fread(buf, 1, (size_t)n, f) != (size_t)n) {
            free(buf);
            buf = NULL;
        }
    }
    fclose(f);
    if (!buf) {
        fail(err, errlen, n > DYN_FILE_MAX ? "%s is too large"
                                           : "cannot read %s", path);
        return NULL;
    }
    buf[n] = '\0';
    *size = n;
    return buf;
}

static int tab_alloc(n265_dyn_tab *t, int n)
{
    t->n = n;
    t->buf = calloc((size_t)n, sizeof *t->buf);
    t->len = calloc((size_t)n, sizeof *t->len);
    return t->buf && t->len ? 0 : -1;
}

void n265_dyn_free(n265_dyn_tab *t)
{
    if (t->buf)
        for (int i = 0; i < t->n; i++)
            free(t->buf[i]);
    free(t->buf);
    free(t->len);
    memset(t, 0, sizeof *t);
}

/* ---- a small JSON reader ------------------------------------------------
 *
 * Enough of RFC 8259 for a metadata file: every value is parsed and
 * checked, numbers are kept as doubles, and strings are kept raw (escapes
 * skipped, not decoded), since the keys and the profile letter this file
 * reads are plain ASCII. */

enum { J_NULL, J_BOOL, J_NUM, J_STR, J_ARR, J_OBJ };

typedef struct jnode {
    int type;
    double num;
    const char *str, *key;
    size_t slen, klen;
    int kid, next;          /* first child, next sibling; -1 for none */
} jnode;

typedef struct jdoc {
    jnode *n;
    int cnt, cap;
    const char *p, *end;
    int depth;
} jdoc;

static void j_ws(jdoc *d)
{
    while (d->p < d->end && (*d->p == ' ' || *d->p == '\t' ||
                             *d->p == '\n' || *d->p == '\r'))
        d->p++;
}

static int j_new(jdoc *d, int type)
{
    if (d->cnt == d->cap) {
        int cap = d->cap ? d->cap * 2 : 1024;
        jnode *n = realloc(d->n, (size_t)cap * sizeof *n);
        if (!n)
            return -1;
        d->n = n;
        d->cap = cap;
    }
    jnode *v = &d->n[d->cnt];
    memset(v, 0, sizeof *v);
    v->type = type;
    v->kid = v->next = -1;
    return d->cnt++;
}

static int j_str(jdoc *d, const char **s, size_t *len)
{
    if (d->p >= d->end || *d->p != '"')
        return -1;
    const char *b = ++d->p;
    while (d->p < d->end && *d->p != '"') {
        if ((unsigned char)*d->p < 0x20)
            return -1;
        if (*d->p == '\\' && ++d->p >= d->end)
            return -1;
        d->p++;
    }
    if (d->p >= d->end)
        return -1;
    *s = b;
    *len = (size_t)(d->p - b);
    d->p++;
    return 0;
}

static int j_value(jdoc *d);

static int j_list(jdoc *d, int self, int obj)
{
    char close = obj ? '}' : ']';
    d->p++;
    j_ws(d);
    if (d->p < d->end && *d->p == close) {
        d->p++;
        return self;
    }
    int last = -1;
    for (;;) {
        const char *key = NULL;
        size_t klen = 0;
        j_ws(d);
        if (obj) {
            if (j_str(d, &key, &klen) < 0)
                return -1;
            j_ws(d);
            if (d->p >= d->end || *d->p++ != ':')
                return -1;
        }
        int v = j_value(d);
        if (v < 0)
            return -1;
        d->n[v].key = key;
        d->n[v].klen = klen;
        if (last < 0)
            d->n[self].kid = v;
        else
            d->n[last].next = v;
        last = v;
        j_ws(d);
        if (d->p < d->end && *d->p == ',') {
            d->p++;
            continue;
        }
        if (d->p < d->end && *d->p == close) {
            d->p++;
            return self;
        }
        return -1;
    }
}

static int j_value(jdoc *d)
{
    j_ws(d);
    if (d->p >= d->end || ++d->depth > 64)
        return -1;
    int v = -1;
    char c = *d->p;
    if (c == '{' || c == '[') {
        v = j_new(d, c == '{' ? J_OBJ : J_ARR);
        if (v >= 0)
            v = j_list(d, v, c == '{');
    } else if (c == '"') {
        const char *s;
        size_t len;
        if ((v = j_new(d, J_STR)) >= 0) {
            if (j_str(d, &s, &len) < 0)
                v = -1;
            else {
                d->n[v].str = s;
                d->n[v].slen = len;
            }
        }
    } else if (!strncmp(d->p, "true", 4) || !strncmp(d->p, "false", 5)) {
        int t = *d->p == 't';
        if ((v = j_new(d, J_BOOL)) >= 0) {
            d->n[v].num = t;
            d->p += t ? 4 : 5;
        }
    } else if (!strncmp(d->p, "null", 4)) {
        if ((v = j_new(d, J_NULL)) >= 0)
            d->p += 4;
    } else if (c == '-' || (c >= '0' && c <= '9')) {
        char *e;
        double x = strtod(d->p, &e);
        if (e == d->p || e > d->end)
            return -1;
        if ((v = j_new(d, J_NUM)) >= 0) {
            d->n[v].num = x;
            d->p = e;
        }
    }
    d->depth--;
    return v;
}

static int j_get(const jdoc *d, int obj, const char *key)
{
    if (obj < 0 || d->n[obj].type != J_OBJ)
        return -1;
    size_t kl = strlen(key);
    for (int k = d->n[obj].kid; k >= 0; k = d->n[k].next)
        if (d->n[k].klen == kl && !memcmp(d->n[k].key, key, kl))
            return k;
    return -1;
}

static int j_count(const jdoc *d, int arr)
{
    int n = 0;
    for (int k = d->n[arr].kid; k >= 0; k = d->n[k].next)
        n++;
    return n;
}

/* ---- the ST 2094-40 payload ------------------------------------------- */

typedef struct bitw {
    uint8_t *p;
    size_t cap, pos;        /* pos in bits */
    int over;
} bitw;

static void put(bitw *w, uint32_t v, int bits)
{
    for (int b = bits - 1; b >= 0; b--) {
        size_t byte = w->pos >> 3;
        if (byte >= w->cap) {
            w->over = 1;
            return;
        }
        if (!(w->pos & 7))
            w->p[byte] = 0;
        if ((v >> b) & 1)
            w->p[byte] |= (uint8_t)(0x80 >> (w->pos & 7));
        w->pos++;
    }
}

/* One window (the whole picture), the shape every HDR10+ title uses: the
 * per-picture targeted-display and mastering-display peak tables are
 * absent, there is no fraction of bright pixels and no colour saturation
 * mapping, which are the fields hdr10plus_tool's JSON has no place for. */
size_t n265_h10p_payload(const n265_h10p_frame *f, uint8_t *out, size_t cap)
{
    bitw w = { out, cap, 0, 0 };
    put(&w, 0xB5, 8);           /* itu_t_t35_country_code: United States */
    put(&w, 0x003C, 16);        /* itu_t_t35_terminal_provider_code */
    put(&w, 0x0001, 16);        /* itu_t_t35_terminal_provider_oriented_code */
    put(&w, 4, 8);              /* application_identifier */
    put(&w, 1, 8);              /* application_version */
    put(&w, 1, 2);              /* num_windows */
    put(&w, f->target_max, 27); /* targeted_system_display_maximum_luminance */
    put(&w, 0, 1);  /* targeted_system_display_actual_peak_luminance_flag */
    for (int c = 0; c < 3; c++)
        put(&w, f->maxscl[c], 17);
    put(&w, f->avg_maxrgb, 17);
    put(&w, (uint32_t)f->npct, 4);  /* num_distribution_maxrgb_percentiles */
    for (int i = 0; i < f->npct; i++) {
        put(&w, f->pct[i], 7);      /* distribution_maxrgb_percentages */
        put(&w, f->pct_val[i], 17); /* distribution_maxrgb_percentiles */
    }
    put(&w, 0, 10);                 /* fraction_bright_pixels */
    put(&w, 0, 1);  /* mastering_display_actual_peak_luminance_flag */
    put(&w, f->tone_mapping ? 1 : 0, 1);
    if (f->tone_mapping) {
        put(&w, f->knee_x, 12);
        put(&w, f->knee_y, 12);
        put(&w, (uint32_t)f->nanchor, 4);
        for (int i = 0; i < f->nanchor; i++)
            put(&w, f->anchor[i], 10);
    }
    put(&w, 0, 1);                  /* color_saturation_mapping_flag */
    if (w.pos & 7)
        put(&w, 0, 8 - (int)(w.pos & 7));
    return w.over ? 0 : w.pos >> 3;
}

/* node `x` as a whole number in [0, 2^bits) */
static int whole(const jdoc *d, int x, int bits, uint32_t *v)
{
    if (x < 0 || d->n[x].type != J_NUM)
        return -1;
    double n = d->n[x].num;
    if (n < 0 || n >= (double)(1u << bits) || n != (double)(uint32_t)n)
        return -1;
    *v = (uint32_t)n;
    return 0;
}

/* an array of whole numbers, each under 2^bits, at most `max` long */
static int whole_list(const jdoc *d, int x, int bits, int max,
                      uint32_t *v, int *n)
{
    if (x < 0 || d->n[x].type != J_ARR)
        return -1;
    *n = 0;
    for (int k = d->n[x].kid; k >= 0; k = d->n[k].next) {
        if (*n == max || whole(d, k, bits, &v[*n]) < 0)
            return -1;
        (*n)++;
    }
    return 0;
}

static int scene(const jdoc *d, int s, int k, int prof, n265_h10p_frame *f,
                 char *err, size_t errlen)
{
    uint32_t nw;
    memset(f, 0, sizeof *f);
    if (whole(d, j_get(d, s, "NumberOfWindows"), 2, &nw) < 0 || nw != 1)
        return fail(err, errlen, "SceneInfo[%d]: NumberOfWindows must be 1 "
                    "(more windows are not supported)", k);
    if (whole(d, j_get(d, s, "TargetedSystemDisplayMaximumLuminance"), 27,
              &f->target_max) < 0)
        return fail(err, errlen, "SceneInfo[%d]: "
                    "TargetedSystemDisplayMaximumLuminance is missing or "
                    "out of range", k);
    int lp = j_get(d, s, "LuminanceParameters");
    int nscl = 0;
    if (whole_list(d, j_get(d, lp, "MaxScl"), 17, 3, f->maxscl, &nscl) < 0 ||
        nscl != 3)
        return fail(err, errlen, "SceneInfo[%d]: MaxScl must be three "
                    "values under 131072", k);
    if (whole(d, j_get(d, lp, "AverageRGB"), 17, &f->avg_maxrgb) < 0)
        return fail(err, errlen, "SceneInfo[%d]: AverageRGB is missing or "
                    "out of range", k);
    int ld = j_get(d, lp, "LuminanceDistributions");
    int ni = 0, nv = 0;
    if (whole_list(d, j_get(d, ld, "DistributionIndex"), 7, 15, f->pct,
                   &ni) < 0 ||
        whole_list(d, j_get(d, ld, "DistributionValues"), 17, 15, f->pct_val,
                   &nv) < 0 || ni != nv)
        return fail(err, errlen, "SceneInfo[%d]: LuminanceDistributions "
                    "needs DistributionIndex (under 128) and "
                    "DistributionValues (under 131072), the same length, "
                    "at most 15", k);
    f->npct = ni;
    /* The HDR10+ profiles narrow ST 2094-40's ranges: luminances to
     * 100000 (10000 cd/m2 in units of 0.1), the target display to 10000
     * cd/m2, the percentiles to the two standard index lists, and the
     * curve to 9 anchors. Profile A has no curve and no target display,
     * profile B has both. A file outside them is refused, because players
     * built to the profiles drop such a message. */
    static const uint32_t idx9[9] = { 1, 5, 10, 25, 50, 75, 90, 95, 99 };
    static const uint32_t idx10[10] = { 1, 5, 10, 25, 50, 75, 90, 95, 98,
                                        99 };
    const uint32_t *want = ni == 9 ? idx9 : ni == 10 ? idx10 : NULL;
    if (!want || memcmp(f->pct, want, (size_t)ni * sizeof *want))
        return fail(err, errlen, "SceneInfo[%d]: DistributionIndex must be "
                    "1,5,10,25,50,75,90,95,99 or 1,5,10,25,50,75,90,95,98,99",
                    k);
    int big = f->avg_maxrgb > 100000;
    for (int c = 0; c < 3; c++)
        big |= f->maxscl[c] > 100000;
    for (int i = 0; i < ni; i++)
        big |= f->pct_val[i] > 100000;
    if (big)
        return fail(err, errlen, "SceneInfo[%d]: MaxScl, AverageRGB and "
                    "DistributionValues are at most 100000", k);
    if (f->target_max > 10000)
        return fail(err, errlen, "SceneInfo[%d]: "
                    "TargetedSystemDisplayMaximumLuminance is at most 10000",
                    k);
    int bz = j_get(d, s, "BezierCurveData");
    if (prof == 'A' && bz >= 0)
        return fail(err, errlen, "SceneInfo[%d] has BezierCurveData, which "
                    "profile A does not carry", k);
    if (prof == 'B' && bz < 0)
        return fail(err, errlen, "SceneInfo[%d] has no BezierCurveData, "
                    "which profile B needs", k);
    if (bz >= 0) {
        f->tone_mapping = 1;
        if (whole(d, j_get(d, bz, "KneePointX"), 12, &f->knee_x) < 0 ||
            whole(d, j_get(d, bz, "KneePointY"), 12, &f->knee_y) < 0 ||
            whole_list(d, j_get(d, bz, "Anchors"), 10, 9, f->anchor,
                       &f->nanchor) < 0 || !f->nanchor)
            return fail(err, errlen, "SceneInfo[%d]: BezierCurveData needs "
                        "KneePointX and KneePointY (under 4096) and 1 to 9 "
                        "Anchors (under 1024)", k);
        if (!f->target_max)
            return fail(err, errlen, "SceneInfo[%d]: a Bezier curve needs "
                        "a TargetedSystemDisplayMaximumLuminance", k);
    } else if (f->target_max) {
        return fail(err, errlen, "SceneInfo[%d]: "
                    "TargetedSystemDisplayMaximumLuminance is 0 without a "
                    "Bezier curve (profile A)", k);
    }
    return 0;
}

int n265_h10p_load(const char *path, n265_dyn_tab *t, char *err,
                   size_t errlen)
{
    memset(t, 0, sizeof *t);
    long size;
    char *text = slurp(path, &size, err, errlen);
    if (!text)
        return -1;
    jdoc d = { NULL, 0, 0, text, text + size, 0 };
    int root = j_value(&d), rc = -1;
    j_ws(&d);
    if (root < 0 || d.p != d.end) {
        fail(err, errlen, "%s is not valid JSON (near byte %ld)", path,
             (long)(d.p - text));
        goto out;
    }
    int si = j_get(&d, root, "SceneInfo");
    if (si < 0 || d.n[si].type != J_ARR || !j_count(&d, si)) {
        fail(err, errlen, "%s has no SceneInfo list", path);
        goto out;
    }
    int prof = 0;
    int pv = j_get(&d, j_get(&d, root, "JSONInfo"), "HDR10plusProfile");
    if (pv >= 0) {
        if (d.n[pv].type != J_STR || d.n[pv].slen != 1 ||
            (d.n[pv].str[0] != 'A' && d.n[pv].str[0] != 'B')) {
            fail(err, errlen, "%s: HDR10plusProfile must be \"A\" or \"B\"",
                 path);
            goto out;
        }
        prof = d.n[pv].str[0];
    }
    int n = j_count(&d, si);
    if (tab_alloc(t, n) < 0) {
        fail(err, errlen, "out of memory");
        goto out;
    }
    int k = 0;
    for (int s = d.n[si].kid; s >= 0; s = d.n[s].next, k++) {
        uint32_t idx = (uint32_t)k;
        int fi = j_get(&d, s, "SequenceFrameIndex");
        if (fi >= 0 && (whole(&d, fi, 31, &idx) < 0 || idx >= (uint32_t)n)) {
            fail(err, errlen, "SceneInfo[%d]: SequenceFrameIndex is not a "
                 "frame of this file", k);
            goto out;
        }
        if (t->buf[idx]) {
            fail(err, errlen, "SceneInfo[%d]: frame %u is described twice",
                 k, idx);
            goto out;
        }
        n265_h10p_frame f;
        uint8_t pl[128];
        size_t len;
        if (scene(&d, s, k, prof, &f, err, errlen) < 0)
            goto out;
        if (!(len = n265_h10p_payload(&f, pl, sizeof pl)) ||
            !(t->buf[idx] = malloc(len))) {
            fail(err, errlen, "out of memory");
            goto out;
        }
        memcpy(t->buf[idx], pl, len);
        t->len[idx] = len;
    }
    rc = 0;
out:
    if (rc < 0)
        n265_dyn_free(t);
    free(d.n);
    free(text);
    return rc;
}

/* ---- the RPU file ------------------------------------------------------
 *
 * dovi_tool writes one RPU per frame, in display order, each behind a
 * four-byte start code. Any NAL header goes (the library writes its own)
 * and the emulation prevention comes out, because the library puts it back
 * as it packs. Removing and re-adding it gives the same bytes, since both
 * sides insert exactly where the escape rule requires. */
int n265_rpu_load(const char *path, n265_dyn_tab *t, char *err,
                  size_t errlen)
{
    memset(t, 0, sizeof *t);
    long size;
    char *text = slurp(path, &size, err, errlen);
    if (!text)
        return -1;
    const uint8_t *b = (const uint8_t *)text;
    int rc = -1, n = 0, cap = 0;
    /* start of each NAL's payload, after its start code */
    long *at = NULL;
    for (long i = 0; i + 2 < size; i++)
        if (b[i] == 0 && b[i + 1] == 0 && b[i + 2] == 1) {
            if (n == cap) {
                cap = cap ? cap * 2 : 4096;
                long *p = realloc(at, (size_t)cap * sizeof *p);
                if (!p) {
                    fail(err, errlen, "out of memory");
                    goto out;
                }
                at = p;
            }
            at[n++] = i + 3;
            i += 2;
        }
    if (!n || at[0] > 4) {
        fail(err, errlen, "%s is not an RPU file (no start code at the "
             "front)", path);
        goto out;
    }
    if (tab_alloc(t, n) < 0) {
        fail(err, errlen, "out of memory");
        goto out;
    }
    for (int k = 0; k < n; k++) {
        long s = at[k], e = k + 1 < n ? at[k + 1] - 3 : size;
        while (e > s && b[e - 1] == 0)  /* a four-byte code's leading zero */
            e--;
        /* dovi_tool writes the RPU with no NAL header, starting at its
         * 0x19 prefix byte; a file cut from a stream has the type-62
         * header in front, which goes. */
        if (e - s >= 3 && b[s] == 0x7c && b[s + 1] == 0x01)
            s += 2;
        if (e - s < 2 || b[s] != 0x19) {
            fail(err, errlen, "%s: entry %d is not a Dolby Vision RPU (no "
                 "0x19 prefix)", path, k);
            goto out;
        }
        uint8_t *r = malloc((size_t)(e - s));
        if (!r) {
            fail(err, errlen, "out of memory");
            goto out;
        }
        size_t m = 0;
        int zeros = 0;
        for (long i = s; i < e; i++) {
            if (zeros >= 2 && b[i] == 3) {
                zeros = 0;
                continue;
            }
            zeros = b[i] == 0 ? zeros + 1 : 0;
            r[m++] = b[i];
        }
        t->buf[k] = r;
        t->len[k] = m;
    }
    rc = 0;
out:
    if (rc < 0)
        n265_dyn_free(t);
    free(at);
    free(text);
    return rc;
}
