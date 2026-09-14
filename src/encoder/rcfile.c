/*
 * --qpfile and --zones: the two grammars. See rcfile.h.
 * Copyright (c) 2026, the yah265 authors. GPL-2.0-or-later; see LICENSE.
 */

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "encoder/rcfile.h"

static int grow(void **buf, int *cap, int n, size_t elem)
{
    if (n < *cap)
        return 0;
    int c = *cap ? *cap * 2 : 16;
    void *p = realloc(*buf, (size_t)c * elem);
    if (!p)
        return -1;
    *buf = p;
    *cap = c;
    return 0;
}

static void say(char *err, size_t errn, const char *fmt, ...)
{
    if (!err || !errn)
        return;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(err, errn, fmt, ap);
    va_end(ap);
}

/* ---- --qpfile ----
 *
 * x264's and x265's format, one frame per line:
 *
 *     <display frame> <type> [<qp>]
 *
 * with `#` starting a comment and blank lines ignored. The type letter is
 * I or K (an IDR), i (a non-IDR intra picture), or '-' for "the encoder
 * decides"; P, B and b parse and are then refused, because what they ask
 * for is a mini-GOP shape and the run length belongs to the plan builder.
 * A qp of -1 (or an absent one) forces only the type. */
int n265_qpfile_parse(n265_qpfile *q, const char *path, char *err,
                      size_t errn)
{
    memset(q, 0, sizeof(*q));
    FILE *f = fopen(path, "r");
    if (!f) {
        say(err, errn, "cannot read %s", path);
        return -1;
    }
    char line[512];
    int lineno = 0;
    while (fgets(line, sizeof(line), f)) {
        lineno++;
        char *h = strchr(line, '#');
        if (h)
            *h = 0;
        int frame = -1;
        char t = 0;
        double qp = -1.0;
        int got = sscanf(line, " %d %c %lf", &frame, &t, &qp);
        if (got <= 0)
            continue;                   /* blank or comment */
        if (got < 2 || frame < 0) {
            say(err, errn, "%s line %d: expected \"<frame> <type> [<qp>]\"",
                path, lineno);
            goto fail;
        }
        int type;
        switch (t) {
        case 'I': case 'K': type = N265_QPF_IDR; break;
        case 'i':           type = N265_QPF_I;   break;
        case '-':           type = N265_QPF_AUTO; break;
        case 'P': case 'B': case 'b':
            say(err, errn, "%s line %d: frame type '%c' asks for a mini-GOP "
                "shape, and the run length belongs to the plan builder, not "
                "to a file. I, K, i and - are the types this encoder can "
                "honour", path, lineno, t);
            goto fail;
        default:
            say(err, errn, "%s line %d: '%c' is not a frame type (I, K, i "
                "or -)", path, lineno, t);
            goto fail;
        }
        if (got >= 3 && qp >= 0.0 && (qp < -12.0 || qp > 51.0)) {
            say(err, errn, "%s line %d: qp %g is outside -12..51", path,
                lineno, qp);
            goto fail;
        }
        if (got < 3)
            qp = -1.0;
        if (q->n && q->e[q->n - 1].frame >= frame) {
            say(err, errn, "%s line %d: frame %d is out of order or repeated "
                "(the file is read in display order)", path, lineno, frame);
            goto fail;
        }
        if (grow((void **)&q->e, &q->cap, q->n, sizeof(*q->e)) < 0) {
            say(err, errn, "out of memory");
            goto fail;
        }
        q->e[q->n].frame = frame;
        q->e[q->n].type = type;
        q->e[q->n].qp = qp;
        q->n++;
    }
    fclose(f);
    if (!q->n) {
        say(err, errn, "%s holds no entries", path);
        n265_qpfile_free(q);
        return -1;
    }
    return 0;
fail:
    fclose(f);
    n265_qpfile_free(q);
    return -1;
}

void n265_qpfile_free(n265_qpfile *q)
{
    free(q->e);
    memset(q, 0, sizeof(*q));
}

/* Sorted and usually walked in order, so a binary search is what it is:
 * correct at any access pattern, and the encoder's lookups are per frame
 * and not per block. */
const n265_qpent *n265_qpfile_at(const n265_qpfile *q, int frame)
{
    int lo = 0, hi = q->n - 1;
    while (lo <= hi) {
        int mid = lo + (hi - lo) / 2;
        if (q->e[mid].frame == frame)
            return &q->e[mid];
        if (q->e[mid].frame < frame)
            lo = mid + 1;
        else
            hi = mid - 1;
    }
    return NULL;
}

/* ---- --zones ----
 *
 * x265's syntax: `start,end,q=N` or `start,end,b=F`, several of them
 * separated by '/'. Overlaps are REFUSED rather than resolved by a
 * precedence the syntax does not state - two zones that both claim frame
 * 100 are a mistake in the command line, and picking one silently is the
 * kind of answer that reads as an encoder bug later. */
int n265_zones_parse(n265_zones *zs, const char *spec, char *err, size_t errn)
{
    memset(zs, 0, sizeof(*zs));
    const char *p = spec;
    while (*p) {
        const char *slash = strchr(p, '/');
        size_t len = slash ? (size_t)(slash - p) : strlen(p);
        char buf[256];
        if (len >= sizeof(buf)) {
            say(err, errn, "--zones: a zone is at most %zu characters",
                sizeof(buf) - 1);
            goto fail;
        }
        memcpy(buf, p, len);
        buf[len] = 0;
        int start = -1, end = -1;
        char opt[64] = "";
        if (sscanf(buf, "%d,%d,%63s", &start, &end, opt) != 3) {
            say(err, errn, "--zones: \"%s\" is not "
                "\"start,end,q=N\" or \"start,end,b=F\"", buf);
            goto fail;
        }
        if (start < 0 || end < start) {
            say(err, errn, "--zones: \"%s\" has no frames in it (start %d, "
                "end %d)", buf, start, end);
            goto fail;
        }
        n265_zone z;
        memset(&z, 0, sizeof(z));
        z.start = start;
        z.end = end;
        z.b = 1.0;
        if (opt[0] == 'q' && opt[1] == '=') {
            z.has_q = 1;
            z.q = atof(opt + 2);
            if (z.q < -12.0 || z.q > 51.0) {
                say(err, errn, "--zones: q=%g is outside -12..51", z.q);
                goto fail;
            }
        } else if (opt[0] == 'b' && opt[1] == '=') {
            z.b = atof(opt + 2);
            if (!(z.b > 0.0) || z.b > 64.0) {
                say(err, errn, "--zones: b=%s is not a bit multiplier in "
                    "(0, 64]", opt + 2);
                goto fail;
            }
        } else {
            say(err, errn, "--zones: \"%s\" is neither q=N nor b=F", opt);
            goto fail;
        }
        for (int i = 0; i < zs->n; i++)
            if (z.start <= zs->z[i].end && zs->z[i].start <= z.end) {
                say(err, errn, "--zones: %d,%d overlaps %d,%d; two zones "
                    "that claim the same frame are refused rather than "
                    "resolved by an order the syntax does not state",
                    z.start, z.end, zs->z[i].start, zs->z[i].end);
                goto fail;
            }
        if (grow((void **)&zs->z, &zs->cap, zs->n, sizeof(*zs->z)) < 0) {
            say(err, errn, "out of memory");
            goto fail;
        }
        zs->z[zs->n++] = z;
        p = slash ? slash + 1 : p + len;
    }
    if (!zs->n) {
        say(err, errn, "--zones: no zones given");
        return -1;
    }
    return 0;
fail:
    n265_zones_free(zs);
    return -1;
}

void n265_zones_free(n265_zones *zs)
{
    free(zs->z);
    memset(zs, 0, sizeof(*zs));
}

const n265_zone *n265_zones_at(const n265_zones *zs, int frame)
{
    for (int i = 0; i < zs->n; i++)
        if (frame >= zs->z[i].start && frame <= zs->z[i].end)
            return &zs->z[i];
    return NULL;
}
