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

/* past any real title's metadata: a two-hour HDR10+ JSON, pretty-printed
 * the way hdr10plus_tool writes it, is about 130 MB */
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
    /* read to the end rather than asking for the size first, so a pipe
     * (`<(...)`) works as well as a file */
    char *buf = NULL;
    size_t n = 0, cap = 0;
    int big = 0, oom = 0;
    for (;;) {
        if (n == cap) {
            if (cap >= (size_t)DYN_FILE_MAX) {
                big = 1;
                break;
            }
            cap = cap ? cap * 2 : 1 << 16;
            char *b = realloc(buf, cap + 1);
            if (!b) {
                oom = 1;
                break;
            }
            buf = b;
        }
        size_t got = fread(buf + n, 1, cap - n, f);
        n += got;
        if (got == 0)
            break;
    }
    int bad = ferror(f) || big || oom || n == cap;
    fclose(f);
    if (bad || !buf) {
        fail(err, errlen, oom ? "out of memory reading %s"
                          : big ? "%s is too large" : "cannot read %s", path);
        free(buf);
        return NULL;
    }
    buf[n] = '\0';
    *size = (long)n;
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
 * checked, not decoded), since the keys and the profile letter this file
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
    int full;               /* a value hit J_NODE_MAX */
    int oom;
    const char *dup;        /* a key the loader reads appears twice */
} jdoc;

static void j_ws(jdoc *d)
{
    while (d->p < d->end && (*d->p == ' ' || *d->p == '\t' ||
                             *d->p == '\n' || *d->p == '\r'))
        d->p++;
}

/* nodes in one value: a scene is about 40, so this is a dense file with no
 * scenes in it, and refusing it keeps the tree from growing to gigabytes */
#define J_NODE_MAX  (1 << 20)

static int j_new(jdoc *d, int type)
{
    if (d->cnt >= J_NODE_MAX) {
        d->full = 1;
        return -1;
    }
    if (d->cnt == d->cap) {
        int cap = d->cap ? d->cap * 2 : 1024;
        jnode *n = realloc(d->n, (size_t)cap * sizeof *n);
        if (!n) {
            d->oom = 1;
            return -1;
        }
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
        if (*d->p == '\\') {
            if (++d->p >= d->end)
                return -1;
            if (*d->p == 'u') {
                for (int i = 1; i <= 4; i++) {
                    char h = d->p + i < d->end ? d->p[i] : 0;
                    if (!((h >= '0' && h <= '9') || (h >= 'a' && h <= 'f') ||
                          (h >= 'A' && h <= 'F')))
                        return -1;
                }
                d->p += 4;
            } else if (!strchr("\"\\/bfnrt", *d->p) || !*d->p) {
                return -1;
            }
        }
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
        /* RFC 8259's grammar first, because strtod also takes hex, "inf"
         * and leading zeros */
        const char *q = d->p + (c == '-');
        if (q >= d->end || *q < '0' || *q > '9' ||
            (*q == '0' && q + 1 < d->end && q[1] >= '0' && q[1] <= '9'))
            return -1;
        while (q < d->end && *q >= '0' && *q <= '9')
            q++;
        if (q < d->end && *q == '.') {
            if (++q >= d->end || *q < '0' || *q > '9')
                return -1;
            while (q < d->end && *q >= '0' && *q <= '9')
                q++;
        }
        if (q < d->end && (*q == 'e' || *q == 'E')) {
            q++;
            if (q < d->end && (*q == '+' || *q == '-'))
                q++;
            if (q >= d->end || *q < '0' || *q > '9')
                return -1;
            while (q < d->end && *q >= '0' && *q <= '9')
                q++;
        }
        char *e;
        double x = strtod(d->p, &e);
        if (e != q)
            return -1;
        if ((v = j_new(d, J_NUM)) >= 0) {
            d->n[v].num = x;
            d->p = e;
        }
    }
    d->depth--;
    return v;
}

/* RFC 8259 leaves a repeated name undefined, so a key the loader reads
 * that appears twice is refused rather than resolved one way or the other:
 * it reads as absent here, and the loader reports it. */
static int j_get(const jdoc *cd, int obj, const char *key)
{
    jdoc *d = (jdoc *)cd;
    if (obj < 0 || d->n[obj].type != J_OBJ)
        return -1;
    size_t kl = strlen(key);
    int hit = -1;
    for (int k = d->n[obj].kid; k >= 0; k = d->n[k].next)
        if (d->n[k].klen == kl && !memcmp(d->n[k].key, key, kl)) {
            if (hit >= 0) {
                d->dup = key;
                return -1;
            }
            hit = k;
        }
    return hit;
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
    if (!(n >= 0 && n < (double)(1u << bits)) || n != (double)(uint32_t)n)
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

static int scene(const jdoc *d, int s, int k, n265_h10p_frame *f,
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
                    "TargetedSystemDisplayMaximumLuminance must be 0 "
                    "without a Bezier curve (profile A)", k);
    }
    return 0;
}

/* growable lists for the loaders, before the frame count is known */
typedef struct dyn_list {
    int n, cap;
    uint8_t **buf;
    size_t *len;
    uint32_t *idx;
    char *curve;
} dyn_list;

static int list_add(dyn_list *l, uint8_t *buf, size_t len, uint32_t idx,
                    int curve)
{
    if (l->n == l->cap) {
        int cap = l->cap ? l->cap * 2 : 1024;
        uint8_t **b = realloc(l->buf, (size_t)cap * sizeof *b);
        if (b)
            l->buf = b;
        size_t *n = realloc(l->len, (size_t)cap * sizeof *n);
        if (n)
            l->len = n;
        uint32_t *x = realloc(l->idx, (size_t)cap * sizeof *x);
        if (x)
            l->idx = x;
        char *c = realloc(l->curve, (size_t)cap);
        if (c)
            l->curve = c;
        if (!b || !n || !x || !c)
            return -1;
        l->cap = cap;
    }
    l->buf[l->n] = buf;
    l->len[l->n] = len;
    l->idx[l->n] = idx;
    l->curve[l->n] = (char)curve;
    l->n++;
    return 0;
}

static void list_free(dyn_list *l, int keep_bufs)
{
    if (!keep_bufs)
        for (int i = 0; i < l->n; i++)
            free(l->buf[i]);
    free(l->buf);
    free(l->len);
    free(l->idx);
    free(l->curve);
}

/* The top-level object is walked by hand and the SceneInfo list parsed
 * one entry at a time, each entry's tree dropped once its payload is
 * built. A two-hour title is ~170,000 entries, so memory then follows the
 * 64-byte payloads rather than the file's half a million nodes a minute. */
int n265_h10p_load(const char *path, n265_dyn_tab *t, char *err,
                   size_t errlen)
{
    memset(t, 0, sizeof *t);
    long size;
    char *text = slurp(path, &size, err, errlen);
    if (!text)
        return -1;
    jdoc d = { NULL, 0, 0, text, text + size, 0, 0, 0, NULL };
    dyn_list l = { 0 };
    int rc = -1, prof = 0, seen = 0, seen_info = 0, k = 0;
    j_ws(&d);
    if (d.p >= d.end || *d.p != '{')
        goto bad;
    d.p++;
    j_ws(&d);
    if (d.p < d.end && *d.p == '}')
        d.p++;
    else
        for (;;) {
            const char *key;
            size_t klen;
            j_ws(&d);
            if (j_str(&d, &key, &klen) < 0)
                goto bad;
            j_ws(&d);
            if (d.p >= d.end || *d.p++ != ':')
                goto bad;
            j_ws(&d);
            if (klen == 9 && !memcmp(key, "SceneInfo", 9)) {
                if (seen++ || d.p >= d.end || *d.p != '[')
                    goto bad;
                d.p++;
                j_ws(&d);
                if (d.p >= d.end)
                    goto bad;
                int more = *d.p != ']';
                if (!more)
                    d.p++;
                while (more) {
                    d.cnt = 0;
                    d.depth = 2;
                    int sc = j_value(&d);
                    if (sc < 0)
                        goto bad;
                    uint32_t idx = (uint32_t)k;
                    int fi = j_get(&d, sc, "SequenceFrameIndex");
                    if (fi >= 0 && whole(&d, fi, 31, &idx) < 0) {
                        fail(err, errlen, "SceneInfo[%d]: SequenceFrameIndex "
                             "is not a frame number", k);
                        goto out;
                    }
                    n265_h10p_frame f;
                    uint8_t pl[128], *copy;
                    size_t len;
                    int got = scene(&d, sc, k, &f, err, errlen);
                    if (d.dup) {
                        fail(err, errlen, "SceneInfo[%d] has the key %s "
                             "twice", k, d.dup);
                        goto out;
                    }
                    if (got < 0)
                        goto out;
                    if (!(len = n265_h10p_payload(&f, pl, sizeof pl)) ||
                        !(copy = malloc(len))) {
                        fail(err, errlen, "out of memory");
                        goto out;
                    }
                    memcpy(copy, pl, len);
                    if (list_add(&l, copy, len, idx, f.tone_mapping) < 0) {
                        free(copy);
                        fail(err, errlen, "out of memory");
                        goto out;
                    }
                    k++;
                    j_ws(&d);
                    if (d.p < d.end && *d.p == ',') {
                        d.p++;
                        continue;
                    }
                    if (d.p < d.end && *d.p == ']') {
                        d.p++;
                        break;
                    }
                    goto bad;
                }
            } else {
                d.cnt = 0;
                d.depth = 1;
                int v = j_value(&d);
                if (v < 0)
                    goto bad;
                if (klen == 8 && !memcmp(key, "JSONInfo", 8)) {
                    int pv = j_get(&d, v, "HDR10plusProfile");
                    if (seen_info++ || d.dup) {
                        fail(err, errlen, "%s has JSONInfo or its "
                             "HDR10plusProfile twice", path);
                        goto out;
                    }
                    if (pv >= 0) {
                        if (d.n[pv].type != J_STR || d.n[pv].slen != 1 ||
                            (d.n[pv].str[0] != 'A' && d.n[pv].str[0] != 'B')) {
                            fail(err, errlen, "%s: HDR10plusProfile must be "
                                 "\"A\" or \"B\"", path);
                            goto out;
                        }
                        prof = d.n[pv].str[0];
                    }
                }
            }
            j_ws(&d);
            if (d.p < d.end && *d.p == ',') {
                d.p++;
                continue;
            }
            if (d.p < d.end && *d.p == '}') {
                d.p++;
                break;
            }
            goto bad;
        }
    j_ws(&d);
    if (d.p != d.end)
        goto bad;
    if (!l.n) {
        fail(err, errlen, "%s has no SceneInfo list", path);
        goto out;
    }
    /* the profile can come after the list, so it is checked here */
    for (int i = 0; i < l.n; i++)
        if (prof && l.curve[i] != (prof == 'B')) {
            fail(err, errlen, prof == 'A'
                 ? "SceneInfo[%d] has BezierCurveData, which profile A does "
                   "not carry"
                 : "SceneInfo[%d] has no BezierCurveData, which profile B "
                   "needs", i);
            goto out;
        }
    if (tab_alloc(t, l.n) < 0) {
        fail(err, errlen, "out of memory");
        goto out;
    }
    for (int i = 0; i < l.n; i++) {
        uint32_t idx = l.idx[i];
        if (idx >= (uint32_t)l.n) {
            fail(err, errlen, "SceneInfo[%d]: SequenceFrameIndex %u is not a "
                 "frame of this file (%d entries)", i, idx, l.n);
            goto out;
        }
        if (t->buf[idx]) {
            fail(err, errlen, "SceneInfo[%d]: frame %u is described twice",
                 i, idx);
            goto out;
        }
        t->buf[idx] = l.buf[i];
        t->len[idx] = l.len[i];
        l.buf[i] = NULL;
    }
    rc = 0;
    goto out;
bad:
    if (d.oom)
        fail(err, errlen, "out of memory");
    else if (d.full)
        fail(err, errlen, "%s has a value of over %d items near byte %ld, "
             "which is not HDR10+ metadata", path, J_NODE_MAX,
             (long)(d.p - text));
    else
        fail(err, errlen, "%s is not valid JSON (near byte %ld)", path,
             (long)(d.p - text));
out:
    if (rc < 0)
        n265_dyn_free(t);
    list_free(&l, 0);
    free(d.n);
    free(text);
    return rc;
}

/* ---- the RPU file ------------------------------------------------------ */

/* rpu_data_header() as far as the profile: vdr_rpu_profile 1 is profile
 * 8 unless the enhancement layer's resampling and residual flags make it 7
 * (12-bit) or 4, and 0 is profile 5. Returns 0 for anything else. */
typedef struct bitr {
    const uint8_t *p;
    size_t n, pos;
} bitr;

static uint32_t get(bitr *r, int bits)
{
    uint32_t v = 0;
    for (int i = 0; i < bits; i++) {
        size_t byte = r->pos >> 3;
        int bit = byte < r->n ? (r->p[byte] >> (7 - (r->pos & 7))) & 1 : 0;
        v = v << 1 | (uint32_t)bit;
        r->pos++;
    }
    return v;
}

static uint32_t get_ue(bitr *r)
{
    int z = 0;
    while (z < 31 && !get(r, 1) && (r->pos >> 3) < r->n)
        z++;
    return z ? ((1u << z) - 1) + get(r, z) : 0;
}

int n265_rpu_profile(const uint8_t *rbsp, size_t len)
{
    if (len < 4 || rbsp[0] != 0x19)
        return 0;
    bitr r = { rbsp + 1, len - 1, 0 };
    if (get(&r, 6) != 2)                /* rpu_type */
        return 0;
    uint32_t format = get(&r, 11);
    uint32_t profile = get(&r, 4);      /* vdr_rpu_profile */
    get(&r, 4);                         /* vdr_rpu_level */
    int el_resample = 0, no_residual = 0, vdr_depth = 0, full = 0;
    if (get(&r, 1)) {                   /* vdr_seq_info_present_flag */
        get(&r, 1);     /* chroma_resampling_explicit_filter_flag */
        if (get(&r, 2) == 0)            /* coefficient_data_type */
            get_ue(&r);                 /* coefficient_log2_denom */
        get(&r, 2);                     /* vdr_rpu_normalized_idc */
        full = (int)get(&r, 1);         /* bl_video_full_range_flag */
        if (!(format & 0x700)) {
            get_ue(&r);                 /* bl_bit_depth_minus8 */
            get_ue(&r);                 /* el_bit_depth_minus8 */
            vdr_depth = (int)get_ue(&r);    /* vdr_bit_depth_minus8 */
            get(&r, 1);                 /* spatial_resampling_filter_flag */
            get(&r, 3);                 /* reserved_zero_3bits */
            el_resample = (int)get(&r, 1);
            no_residual = (int)get(&r, 1);
        }
    }
    if ((r.pos + 7) >> 3 > r.n)
        return 0;
    if (profile == 0)
        return full ? 5 : 0;
    if (profile == 1)
        return el_resample && !no_residual ? (vdr_depth == 4 ? 7 : 4) : 8;
    return 0;
}

/* MPEG-2's CRC32: polynomial 0x04C11DB7, not reflected, from all ones */
static uint32_t rpu_crc32(const uint8_t *p, size_t n)
{
    uint32_t c = 0xffffffffu;
    for (size_t i = 0; i < n; i++) {
        c ^= (uint32_t)p[i] << 24;
        for (int k = 0; k < 8; k++)
            c = c & 0x80000000u ? (c << 1) ^ 0x04c11db7u : c << 1;
    }
    return c;
}

/* dovi_tool writes one RPU per frame, in display order, each behind a
 * start code. Any NAL header goes (the library writes its own) and the
 * emulation prevention comes out, because the library puts it back as it
 * packs. That gives the same bytes only for an entry escaped exactly where
 * the rule requires and nowhere else, so every entry is re-escaped here and
 * refused if it differs: "copied unread" then holds for every file that is
 * accepted. Each entry is checked as it is found. */
int n265_rpu_load(const char *path, n265_dyn_tab *t, char *err,
                  size_t errlen)
{
    memset(t, 0, sizeof *t);
    long size;
    char *text = slurp(path, &size, err, errlen);
    if (!text)
        return -1;
    const uint8_t *b = (const uint8_t *)text;
    dyn_list l = { 0 };
    int rc = -1;
    /* Annex B lets zero bytes run ahead of the first start code */
    long s = 0;
    while (s < size && !b[s])
        s++;
    if (s >= 2 && s < size && b[s] == 1)
        s++;
    else {
        fail(err, errlen, "%s is not an RPU file (no start code at the "
             "front)", path);
        goto out;
    }
    for (int k = 0; s <= size; k++) {
        long e = s, next = size + 1;
        for (; e + 2 < size; e++)
            if (!b[e] && !b[e + 1] && b[e + 2] == 1) {
                next = e + 3;
                break;
            }
        if (next > size)
            e = size;
        long end = e;
        while (end > s && b[end - 1] == 0)  /* a four-byte code's zero */
            end--;
        if (end == s && next > size && k > 0)   /* a start code at the end */
            break;
        /* dovi_tool writes the RPU with no NAL header, starting at its
         * 0x19 prefix byte; a file cut from a stream has the type-62
         * header in front, which goes. */
        long at = s;
        if (end - at >= 3 && b[at] == 0x7c && b[at + 1] == 0x01)
            at += 2;
        if (end - at < 2 || b[at] != 0x19) {
            fail(err, errlen, "%s: entry %d is not a Dolby Vision RPU (no "
                 "0x19 prefix)", path, k);
            goto out;
        }
        uint8_t *r = malloc((size_t)(end - at));
        if (!r) {
            fail(err, errlen, "out of memory");
            goto out;
        }
        size_t m = 0;
        int zeros = 0, same = 1;
        long o = at;    /* where re-escaping the RBSP has got to */
        for (long i = at; i < end; i++) {
            if (zeros >= 2 && b[i] == 3) {
                zeros = 0;
                continue;
            }
            zeros = b[i] == 0 ? zeros + 1 : 0;
            r[m++] = b[i];
        }
        zeros = 0;
        for (size_t i = 0; i < m && same; i++) {
            if (zeros >= 2 && r[i] <= 3) {
                same &= o < end && b[o++] == 3;
                zeros = 0;
            }
            same &= o < end && b[o++] == r[i];
            zeros = r[i] == 0 ? zeros + 1 : 0;
        }
        same &= o == end;
        /* An RPU ends in a CRC32 of everything after its prefix byte, then
         * the 0x80 stop byte. The body is copied unread, but a file cut
         * short or damaged on the way fails here instead of becoming a
         * corrupt RPU in the stream. */
        int whole_rpu = m >= 6 && r[m - 1] == 0x80 &&
                        rpu_crc32(r + 1, m - 6) ==
                            ((uint32_t)r[m - 5] << 24 | (uint32_t)r[m - 4] << 16 |
                             (uint32_t)r[m - 3] << 8 | r[m - 2]);
        int prof = same && whole_rpu ? n265_rpu_profile(r, m) : 0;
        const char *why = !same ? "isn't escaped the way a stream carries it, "
                                  "so it can't be copied as it is"
                        : !whole_rpu ? "fails its CRC: the file is cut short "
                                       "or damaged"
                        : NULL;
        if (why || prof != 8 || list_add(&l, r, m, (uint32_t)k, 0) < 0) {
            free(r);
            if (why)
                fail(err, errlen, "%s: entry %d %s", path, k, why);
            else if (prof == 8)
                fail(err, errlen, "out of memory");
            else if (prof)
                fail(err, errlen, "%s: entry %d is a profile %d RPU; only "
                     "profile 8 is supported", path, k, prof);
            else
                fail(err, errlen, "%s: entry %d is not a Dolby Vision RPU "
                     "this encoder can read", path, k);
            goto out;
        }
        s = next;
    }
    if (tab_alloc(t, l.n) < 0) {
        fail(err, errlen, "out of memory");
        goto out;
    }
    for (int i = 0; i < l.n; i++) {
        t->buf[i] = l.buf[i];
        t->len[i] = l.len[i];
        l.buf[i] = NULL;
    }
    rc = 0;
out:
    if (rc < 0)
        n265_dyn_free(t);
    list_free(&l, 0);
    free(text);
    return rc;
}
