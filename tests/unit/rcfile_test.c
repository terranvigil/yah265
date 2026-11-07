/*
 * Unit tests: the --qpfile and --zones grammars (src/encoder/rcfile.c,
 * phase 14 item 4).
 * Copyright (c) 2026, the yah265 authors. GPL-2.0-or-later; see LICENSE.
 *
 * Half of what a file format is, is what it REFUSES, so every refusal
 * here is a case: an unknown type letter, a frame type that asks for a
 * mini-GOP shape, a repeated or out-of-order frame number, a qp outside
 * QpY, a zone that is neither q= nor b=, and two zones claiming the same
 * frame. A parser tested only on well-formed input is a parser that
 * accepts anything.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "encoder/rcfile.h"

static int fails;

#define CHECK(c, ...) do { \
    if (!(c)) { printf("FAIL: " __VA_ARGS__); printf("\n"); fails++; } \
} while (0)

/* write `body` to a temporary file and hand back the path */
static const char *tmpfile_with(const char *body)
{
    static char path[] = "/tmp/n265_qpf_XXXXXX";
    strcpy(path, "/tmp/n265_qpf_XXXXXX");
    int fd = mkstemp(path);
    if (fd < 0)
        return NULL;
    FILE *f = fdopen(fd, "w");
    fputs(body, f);
    fclose(f);
    return path;
}

static void qpfile_ok(void)
{
    const char *p = tmpfile_with(
        "# a comment, and a blank line follows\n"
        "\n"
        "0 I 26\n"
        "  12   i   31.5   # trailing comment\n"
        "30 K\n"
        "45 - 20\n");
    n265_qpfile q;
    char err[256] = "";
    CHECK(n265_qpfile_parse(&q, p, err, sizeof(err)) == 0,
          "a well-formed qpfile was refused: %s", err);
    unlink(p);
    CHECK(q.n == 4, "%d entries, expected 4", q.n);
    if (q.n != 4)
        return;
    CHECK(q.e[0].frame == 0 && q.e[0].type == N265_QPF_IDR &&
          q.e[0].qp == 26.0, "entry 0 wrong");
    CHECK(q.e[1].frame == 12 && q.e[1].type == N265_QPF_I &&
          q.e[1].qp == 31.5, "entry 1 wrong (a fractional qp is legal)");
    /* a type with no qp forces the type and leaves the quantizer alone */
    CHECK(q.e[2].frame == 30 && q.e[2].type == N265_QPF_IDR &&
          q.e[2].qp < 0.0, "entry 2 should carry no qp");
    CHECK(q.e[3].type == N265_QPF_AUTO && q.e[3].qp == 20.0,
          "'-' is a qp with no type");

    CHECK(n265_qpfile_at(&q, 12) == &q.e[1], "lookup of a present frame");
    CHECK(n265_qpfile_at(&q, 13) == NULL, "lookup of an absent frame");
    CHECK(n265_qpfile_at(&q, 0) == &q.e[0], "lookup of the first frame");
    CHECK(n265_qpfile_at(&q, 45) == &q.e[3], "lookup of the last frame");
    CHECK(n265_qpfile_at(&q, 1000) == NULL, "lookup past the end");
    n265_qpfile_free(&q);
}

static void qpfile_refuse(const char *body, const char *what)
{
    const char *p = tmpfile_with(body);
    n265_qpfile q;
    char err[256] = "";
    int r = n265_qpfile_parse(&q, p, err, sizeof(err));
    unlink(p);
    CHECK(r < 0, "%s was ACCEPTED", what);
    CHECK(err[0] != 0, "%s was refused without a reason", what);
    if (r == 0)
        n265_qpfile_free(&q);
}

static void zones_ok(void)
{
    n265_zones zs;
    char err[256] = "";
    CHECK(n265_zones_parse(&zs, "0,99,q=20/200,299,b=2.5", err,
                           sizeof(err)) == 0,
          "a well-formed --zones was refused: %s", err);
    CHECK(zs.n == 2, "%d zones, expected 2", zs.n);
    if (zs.n != 2)
        return;
    CHECK(zs.z[0].has_q && zs.z[0].q == 20.0 && zs.z[0].b == 1.0,
          "q= zone wrong");
    CHECK(!zs.z[1].has_q && zs.z[1].b == 2.5, "b= zone wrong");
    CHECK(n265_zones_at(&zs, 0) == &zs.z[0], "the first frame is in zone 0");
    CHECK(n265_zones_at(&zs, 99) == &zs.z[0], "the range is inclusive");
    CHECK(n265_zones_at(&zs, 100) == NULL, "the gap belongs to nobody");
    CHECK(n265_zones_at(&zs, 250) == &zs.z[1], "zone 1");
    CHECK(n265_zones_at(&zs, 300) == NULL, "past the last zone");
    n265_zones_free(&zs);

    /* one zone, no slash */
    CHECK(n265_zones_parse(&zs, "5,5,q=0", err, sizeof(err)) == 0,
          "a one-frame zone was refused: %s", err);
    CHECK(zs.n == 1 && zs.z[0].start == 5 && zs.z[0].end == 5,
          "a one-frame zone is start == end");
    n265_zones_free(&zs);
}

static void zones_refuse(const char *spec, const char *what)
{
    n265_zones zs;
    char err[256] = "";
    int r = n265_zones_parse(&zs, spec, err, sizeof(err));
    CHECK(r < 0, "%s was ACCEPTED (\"%s\")", what, spec);
    CHECK(err[0] != 0, "%s was refused without a reason", what);
    if (r == 0)
        n265_zones_free(&zs);
}

int main(void)
{
    qpfile_ok();
    qpfile_refuse("0 P 26\n", "a P frame type");
    qpfile_refuse("0 B 26\n", "a B frame type");
    qpfile_refuse("0 b 26\n", "a non-reference B frame type");
    qpfile_refuse("0 Z 26\n", "an unknown type letter");
    qpfile_refuse("0 I 26\n0 I 26\n", "a repeated frame number");
    qpfile_refuse("9 I 26\n2 I 26\n", "frames out of display order");
    qpfile_refuse("0 I 99\n", "a qp above 51");
    qpfile_refuse("-3 I 26\n", "a negative frame number");
    qpfile_refuse("0\n", "a line with no type");
    qpfile_refuse("# nothing but a comment\n", "a file with no entries");

    zones_ok();
    zones_refuse("0,99", "a zone with no option");
    zones_refuse("0,99,x=3", "an option that is neither q= nor b=");
    zones_refuse("99,0,q=20", "a zone that ends before it starts");
    zones_refuse("-1,99,q=20", "a negative start");
    zones_refuse("0,99,q=99", "a q outside QpY");
    zones_refuse("0,99,b=0", "a bit multiplier of zero");
    zones_refuse("0,99,b=-2", "a negative bit multiplier");
    zones_refuse("0,99,q=20/50,150,q=30", "two zones claiming frame 50");
    zones_refuse("", "an empty spec");

    printf(fails ? "rcfile: %d FAILED\n" : "rcfile: ok (%d failures)\n",
           fails);
    return fails != 0;
}
