/*
 * The shot list: parse, write, fingerprint and the scan's resample.
 * Copyright (c) 2026, the yah265 authors. GPL-2.0-or-later; see LICENSE.
 */

#include <ctype.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

#include "encoder/shotlist.h"

/* the mean absolute thumbnail difference, in 8-bit levels, under which two
 * inputs of different size are taken as the same source. A rescale moves
 * a block mean by well under a level; another title moves it by tens. */
#define THUMB_TOL 4.0

static void seterr(char *err, size_t cap, const char *fmt, ...)
{
    if (!err || !cap)
        return;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(err, cap, fmt, ap);
    va_end(ap);
}

void n265_shotlist_free(n265_shotlist *l)
{
    if (!l)
        return;
    free(l->s);
    memset(l, 0, sizeof *l);
}

static int hexval(int c)
{
    return c >= '0' && c <= '9' ? c - '0'
         : c >= 'a' && c <= 'f' ? c - 'a' + 10
         : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
}

static int parse_thumb(const char *v, uint8_t t[64])
{
    for (int k = 0; k < 64; k++) {
        int a = hexval((unsigned char)v[2 * k]);
        int b = a < 0 ? -1 : hexval((unsigned char)v[2 * k + 1]);
        if (a < 0 || b < 0)
            return -1;
        t[k] = (uint8_t)(a << 4 | b);
    }
    v += 128;
    while (*v == ' ' || *v == '\t')
        v++;
    return *v ? -1 : 0;
}

/* the next line of `*p` into `ln`, comments and trailing space removed;
 * -1 at the end of the text */
static int next_line(const char **p, char *ln, size_t cap, int *lineno)
{
    while (**p) {
        const char *e = strchr(*p, '\n');
        size_t n = e ? (size_t)(e - *p) : strlen(*p);
        const char *s = *p;
        *p = e ? e + 1 : s + n;
        (*lineno)++;
        if (n >= cap)
            n = cap - 1;
        memcpy(ln, s, n);
        ln[n] = '\0';
        char *h = strchr(ln, '#');
        if (h)
            *h = '\0';
        size_t k = strlen(ln);
        while (k && isspace((unsigned char)ln[k - 1]))
            ln[--k] = '\0';
        char *b = ln;
        while (*b == ' ' || *b == '\t')
            b++;
        if (b != ln)
            memmove(ln, b, strlen(b) + 1);
        if (*ln)
            return 0;
    }
    return -1;
}

static int kind_parse(const char *s)
{
    return !strcmp(s, "start") ? N265_SHOT_START
         : !strcmp(s, "cut") ? N265_SHOT_CUT
         : !strcmp(s, "hand") ? N265_SHOT_HAND : -1;
}

/* one key=value field of a shot line: f= (the scan's features, which need
 * the features line before them) or qp= */
static int shot_kv(n265_shot *s, const char *t, int feat_ver)
{
    char *e;
    if (!strncmp(t, "qp=", 3) && !s->has_qp) {
        s->qp = strtod(t + 3, &e);
        if (e == t + 3 || *e || !(s->qp >= -12.0 && s->qp <= 12.0))
            return -1;
        s->has_qp = 1;
        return 0;
    }
    if (!strncmp(t, "f=", 2) && feat_ver > 0 && !s->has_feat) {
        const char *p = t + 2;
        for (int k = 0; k < N265_SHOTFEAT_N; k++) {
            s->feat[k] = strtod(p, &e);
            if (e == p || *e != (k < N265_SHOTFEAT_N - 1 ? ',' : '\0'))
                return -1;
            p = e + 1;
        }
        s->has_feat = 1;
        return 0;
    }
    return -1;
}

int n265_shotlist_parse_str(n265_shotlist *l, const char *text, char *err,
                            size_t cap)
{
    memset(l, 0, sizeof *l);
    const char *p = text;
    char ln[1024];
    int lineno = 0, ver = 0, nshots = -1, nthumb = 0;
    char magic[32];
    if (next_line(&p, ln, sizeof ln, &lineno) != 0 ||
        sscanf(ln, "%31s %d", magic, &ver) != 2 ||
        strcmp(magic, N265_SHOTLIST_MAGIC) != 0) {
        seterr(err, cap, "not a shot list (the first line is not \"%s %d\")",
               N265_SHOTLIST_MAGIC, N265_SHOTLIST_VERSION);
        return -1;
    }
    if (ver < 1 || ver > N265_SHOTLIST_VERSION) {
        seterr(err, cap, "shot list version %d; this build reads versions 1 "
               "to %d", ver, N265_SHOTLIST_VERSION);
        return -1;
    }
    int have_luma = 0, have_src = 0;
    for (;;) {
        if (next_line(&p, ln, sizeof ln, &lineno) != 0) {
            seterr(err, cap, "no \"shots\" line");
            goto bad;
        }
        char key[16];
        int off = 0;
        if (sscanf(ln, "%15s %n", key, &off) < 1)
            goto malformed;
        const char *v = ln + off;
        if (!strcmp(key, "frames")) {
            char *e;
            l->fp.frames = strtol(v, &e, 10);
            if (e == v || *e || l->fp.frames < 1)
                goto malformed;
        } else if (!strcmp(key, "source")) {
            if (sscanf(v, "%dx%d depth=%d", &l->fp.w, &l->fp.h,
                       &l->fp.depth) != 3 || l->fp.w < 1 || l->fp.h < 1 ||
                (l->fp.depth != 8 && l->fp.depth != 10))
                goto malformed;
            have_src = 1;
        } else if (!strcmp(key, "luma")) {
            char *e;
            l->fp.luma = strtoull(v, &e, 16);
            if (e == v || *e)
                goto malformed;
            have_luma = 1;
        } else if (!strcmp(key, "thumb")) {
            if (nthumb >= 3 || strlen(v) < 128 ||
                parse_thumb(v, l->fp.thumb[nthumb]) != 0)
                goto malformed;
            nthumb++;
        } else if (!strcmp(key, "scan")) {
            if (sscanf(v, "%dx%d min-keyint=%d", &l->scan_w, &l->scan_h,
                       &l->scan_mk) != 3)
                goto malformed;
        } else if (!strcmp(key, "features") && ver >= 2) {
            char *e;
            l->feat_ver = (int)strtol(v, &e, 10);
            if (e == v || *e || l->feat_ver < 1)
                goto malformed;
        } else if (!strcmp(key, "shots")) {
            char *e;
            nshots = (int)strtol(v, &e, 10);
            if (e == v || *e || nshots < 1)
                goto malformed;
            break;
        } else {
            seterr(err, cap, "line %d: unknown key \"%s\"", lineno, key);
            goto bad;
        }
    }
    /* a fingerprint is all three parts or none: half of one would refuse
     * nothing and look as if it did */
    if (have_luma || have_src || nthumb) {
        if (!(have_luma && have_src && nthumb == 3 && l->fp.frames > 0)) {
            seterr(err, cap, "an incomplete fingerprint: it needs frames, "
                   "source, luma and three thumb lines");
            goto bad;
        }
        l->fp.have = 1;
    }
    if (l->fp.frames > 0 && nshots > l->fp.frames) {
        seterr(err, cap, "%d shots in %ld frames", nshots, l->fp.frames);
        goto bad;
    }
    l->s = calloc((size_t)nshots, sizeof *l->s);
    if (!l->s) {
        seterr(err, cap, "out of memory");
        goto bad;
    }
    for (int k = 0; k < nshots; k++) {
        if (next_line(&p, ln, sizeof ln, &lineno) != 0) {
            seterr(err, cap, "the list ends after %d of %d shots", k, nshots);
            goto bad;
        }
        n265_shot *s = &l->s[k];
        /* up to four positional fields, then key=value fields */
        char a[4][32];
        int nf = 0;
        char *save = NULL;
        for (char *t = strtok_r(ln, " \t", &save); t;
             t = strtok_r(NULL, " \t", &save)) {
            if (strchr(t, '=')) {
                if (ver < 2 || !nf || shot_kv(s, t, l->feat_ver) != 0)
                    goto malformed;
                continue;
            }
            if (nf == 4 || s->has_feat || s->has_qp || strlen(t) >= 32)
                goto malformed;
            strcpy(a[nf++], t);
        }
        if (nf < 1)
            goto malformed;
        char *e;
        long first = strtol(a[0], &e, 10);
        if (e == a[0] || *e || first < 0 || first > 0x3fffffff)
            goto malformed;
        s->first = (int)first;
        s->last = -1;
        s->kind = k == 0 ? N265_SHOT_START : N265_SHOT_HAND;
        s->conf = -1.0;
        if (nf >= 2 && strcmp(a[1], "-")) {
            long last = strtol(a[1], &e, 10);
            if (e == a[1] || *e || last < first || last > 0x3fffffff)
                goto malformed;
            s->last = (int)last;
        }
        if (nf >= 3 && (s->kind = kind_parse(a[2])) < 0) {
            seterr(err, cap, "line %d: shot kind \"%s\" is not start, cut "
                   "or hand", lineno, a[2]);
            goto bad;
        }
        if (nf >= 4 && strcmp(a[3], "-")) {
            s->conf = strtod(a[3], &e);
            if (e == a[3] || *e || s->conf < 0.0)
                goto malformed;
        }
        if (k == 0 && s->first != 0) {
            seterr(err, cap, "line %d: the first shot starts at frame %d, "
                   "not 0", lineno, s->first);
            goto bad;
        }
        if (k > 0) {
            n265_shot *q = &l->s[k - 1];
            if (s->first <= q->first) {
                seterr(err, cap, "line %d: shot %d starts at %d, not after "
                       "shot %d's %d", lineno, k, s->first, k - 1, q->first);
                goto bad;
            }
            if (q->last >= 0 && q->last != s->first - 1) {
                seterr(err, cap, "line %d: shot %d ends at %d and shot %d "
                       "starts at %d; shots must be contiguous", lineno,
                       k - 1, q->last, k, s->first);
                goto bad;
            }
            q->last = s->first - 1;
        }
        if (l->fp.frames > 0 && s->first >= l->fp.frames) {
            seterr(err, cap, "line %d: shot %d starts at %d, past the "
                   "list's %ld frames", lineno, k, s->first, l->fp.frames);
            goto bad;
        }
    }
    l->n = nshots;
    if (next_line(&p, ln, sizeof ln, &lineno) != 0 || strcmp(ln, "end")) {
        seterr(err, cap, "no \"end\" after %d shots", nshots);
        goto bad;
    }
    if (next_line(&p, ln, sizeof ln, &lineno) == 0) {
        seterr(err, cap, "line %d: text after \"end\"", lineno);
        goto bad;
    }
    n265_shot *t = &l->s[l->n - 1];
    if (l->fp.frames > 0) {
        if (t->last >= 0 && t->last != l->fp.frames - 1) {
            seterr(err, cap, "the last shot ends at %d, not at frame %ld",
                   t->last, l->fp.frames - 1);
            goto bad;
        }
        t->last = (int)l->fp.frames - 1;
    }
    return 0;
malformed:
    seterr(err, cap, "line %d is malformed: \"%s\"", lineno, ln);
bad:
    n265_shotlist_free(l);
    return -1;
}

int n265_shotlist_parse(n265_shotlist *l, const char *path, char *err,
                        size_t cap)
{
    memset(l, 0, sizeof *l);
    FILE *f = fopen(path, "rb");
    if (!f) {
        seterr(err, cap, "cannot read %s", path);
        return -1;
    }
    size_t n = 0, sz = 4096;
    char *buf = malloc(sz);
    for (;;) {
        if (!buf) {
            fclose(f);
            seterr(err, cap, "out of memory");
            return -1;
        }
        n += fread(buf + n, 1, sz - 1 - n, f);
        if (n < sz - 1)
            break;
        char *nb = realloc(buf, sz * 2);
        if (!nb)
            free(buf);
        buf = nb;
        sz *= 2;
    }
    fclose(f);
    buf[n] = '\0';
    if (memchr(buf, '\0', n)) {
        free(buf);
        seterr(err, cap, "%s is not text", path);
        return -1;
    }
    int r = n265_shotlist_parse_str(l, buf, err, cap);
    free(buf);
    return r;
}

int n265_shotlist_write(FILE *f, const n265_shotlist *l)
{
    static const char *const kind[] = { "start", "cut", "hand" };
    int v2 = 0;
    for (int k = 0; k < l->n; k++)
        v2 |= l->s[k].has_feat || l->s[k].has_qp;
    fprintf(f, "%s %d\n", N265_SHOTLIST_MAGIC, v2 ? 2 : 1);
    if (l->fp.frames > 0)
        fprintf(f, "frames %ld\n", l->fp.frames);
    if (l->fp.have) {
        fprintf(f, "source %dx%d depth=%d\nluma %016llx\n", l->fp.w, l->fp.h,
                l->fp.depth, (unsigned long long)l->fp.luma);
        for (int t = 0; t < 3; t++) {
            fputs("thumb ", f);
            for (int k = 0; k < 64; k++)
                fprintf(f, "%02x", l->fp.thumb[t][k]);
            fputc('\n', f);
        }
    }
    if (l->scan_w > 0)
        fprintf(f, "scan %dx%d min-keyint=%d\n", l->scan_w, l->scan_h,
                l->scan_mk);
    if (v2 && l->feat_ver > 0)
        fprintf(f, "features %d\n", l->feat_ver);
    fprintf(f, "shots %d\n", l->n);
    for (int k = 0; k < l->n; k++) {
        const n265_shot *s = &l->s[k];
        if (s->last >= 0)
            fprintf(f, "%d %d %s", s->first, s->last, kind[s->kind]);
        else
            fprintf(f, "%d - %s", s->first, kind[s->kind]);
        if (s->conf >= 0.0)
            fprintf(f, " %.3f", s->conf);
        else
            fputs(" -", f);
        if (s->has_feat && l->feat_ver > 0)
            for (int j = 0; j < N265_SHOTFEAT_N; j++)
                fprintf(f, "%s%.6g", j ? "," : " f=", s->feat[j]);
        if (s->has_qp)
            fprintf(f, " qp=%.4f", s->qp);
        fputc('\n', f);
    }
    fputs("end\n", f);
    return ferror(f) ? -1 : 0;
}

int n265_shotlist_check(n265_shotlist *l, const n265_shot_fp *have,
                        char *err, size_t cap)
{
    long nf = have->frames;
    if (l->fp.frames > 0 && nf != l->fp.frames) {
        seterr(err, cap, "the list covers %ld frames and this input has %ld",
               l->fp.frames, nf);
        return -1;
    }
    for (int k = 0; k < l->n; k++)
        if (l->s[k].first >= nf) {
            seterr(err, cap, "shot %d starts at frame %d and this input has "
                   "%ld frames", k, l->s[k].first, nf);
            return -1;
        }
    if (l->fp.have) {
        const n265_shot_fp *a = &l->fp;
        double x = (double)a->w * have->h, y = (double)have->w * a->h;
        if (x > 1.01 * y || y > 1.01 * x) {
            seterr(err, cap, "the list was made from a %dx%d source and this "
                   "input is %dx%d, another shape", a->w, a->h, have->w,
                   have->h);
            return -1;
        }
        if (a->w == have->w && a->h == have->h && a->depth == have->depth) {
            if (a->luma != have->luma) {
                seterr(err, cap, "the input's pictures are not the ones the "
                       "list was made from (luma %016llx, list %016llx)",
                       (unsigned long long)have->luma,
                       (unsigned long long)a->luma);
                return -1;
            }
        } else {
            double d = 0.0;
            for (int t = 0; t < 3; t++)
                for (int k = 0; k < 64; k++)
                    d += abs((int)a->thumb[t][k] - (int)have->thumb[t][k]);
            d /= 192.0;
            if (d > THUMB_TOL) {
                seterr(err, cap, "the input's pictures do not look like the "
                       "ones the list was made from (they differ by %.1f "
                       "levels on average)", d);
                return -1;
            }
        }
    }
    n265_shot *t = &l->s[l->n - 1];
    if (t->last >= 0 && t->last != nf - 1) {
        seterr(err, cap, "the last shot ends at %d and this input ends at "
               "%ld", t->last, nf - 1);
        return -1;
    }
    t->last = (int)(nf - 1);
    return 0;
}

static unsigned luma_at(const uint8_t *p, size_t i, int depth)
{
    if (depth == 8)
        return p[i];
    unsigned v = (unsigned)p[2 * i] | ((unsigned)p[2 * i + 1] << 8);
    unsigned mx = (1u << depth) - 1;
    return v > mx ? mx : v;
}

void n265_shot_fp_frame(const uint8_t *luma, int w, int h, int depth,
                        uint64_t *hash, uint8_t thumb[64])
{
    size_t bytes = (size_t)w * h * (depth > 8 ? 2 : 1);
    uint64_t x = 1469598103934665603ULL;
    for (size_t b = 0; b < bytes; b++) {
        x ^= luma[b];
        x *= 1099511628211ULL;
    }
    *hash = x;
    for (int cy = 0; cy < 8; cy++)
        for (int cx = 0; cx < 8; cx++) {
            int x0 = cx * w / 8, x1 = (cx + 1) * w / 8;
            int y0 = cy * h / 8, y1 = (cy + 1) * h / 8;
            if (x1 <= x0) x1 = x0 + 1;
            if (y1 <= y0) y1 = y0 + 1;
            uint64_t sum = 0;
            for (int y = y0; y < y1; y++)
                for (int xx = x0; xx < x1; xx++)
                    sum += luma_at(luma, (size_t)y * w + xx, depth);
            uint64_t cnt = (uint64_t)(x1 - x0) * (uint64_t)(y1 - y0);
            uint64_t m = (sum + cnt / 2) / cnt;
            m = depth > 8 ? (m + (1u << (depth - 9))) >> (depth - 8) : m;
            thumb[cy * 8 + cx] = (uint8_t)(m > 255 ? 255 : m);
        }
}

uint64_t n265_shot_fp_combine(const uint64_t h[3])
{
    uint64_t x = 1469598103934665603ULL;
    for (int k = 0; k < 3; k++)
        for (int b = 0; b < 8; b++) {
            x ^= (h[k] >> (8 * b)) & 0xff;
            x *= 1099511628211ULL;
        }
    return x;
}

int n265_shotscan_h(int w, int h)
{
    int ah = (int)(((long long)h * N265_SHOTSCAN_W + w) / (2LL * w)) * 2;
    return ah < 16 ? 16 : ah;
}

void n265_shotscan_resample(const uint8_t *luma, int w, int h, int depth,
                            uint8_t *dst, int dw, int dh)
{
    int sh = depth - 8;
    for (int y = 0; y < dh; y++) {
        int y0 = (int)((long long)y * h / dh);
        int y1 = (int)((long long)(y + 1) * h / dh);
        if (y1 <= y0) y1 = y0 + 1;
        if (y1 > h) y1 = h;
        for (int x = 0; x < dw; x++) {
            int x0 = (int)((long long)x * w / dw);
            int x1 = (int)((long long)(x + 1) * w / dw);
            if (x1 <= x0) x1 = x0 + 1;
            if (x1 > w) x1 = w;
            uint32_t sum = 0;
            for (int yy = y0; yy < y1; yy++)
                for (int xx = x0; xx < x1; xx++)
                    sum += luma_at(luma, (size_t)yy * w + xx, depth);
            uint32_t cnt = (uint32_t)((x1 - x0) * (y1 - y0));
            uint32_t m = (sum + cnt / 2) / cnt;
            m = sh > 0 ? (m + (1u << (sh - 1))) >> sh : m;
            dst[(size_t)y * dw + x] = (uint8_t)(m > 255 ? 255 : m);
        }
    }
}
