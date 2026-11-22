/*
 * Unit tests: the shot list (src/encoder/shotlist.c): the parser, the
 * writer and the fingerprint refusals.
 * Copyright (c) 2026, the yah265 authors. GPL-2.0-or-later; see LICENSE.
 *
 * A list is applied to an input by frame number, so a list that reaches
 * the wrong input codes key frames in the wrong places and nothing else
 * notices. Every refusal is a case here.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "encoder/shotlist.h"
#include "encoder/shotplan.h"

static int fails;

#define CHECK(c, ...) do { \
    if (!(c)) { printf("FAIL: " __VA_ARGS__); printf("\n"); fails++; } \
} while (0)

static void parse_ok(void)
{
    n265_shotlist l;
    char err[256] = "";
    const char *t =
        "yah265-shots 1\n"
        "# a hand list: first frames only, comments and blank lines\n"
        "\n"
        "frames 300\n"
        "shots 3\n"
        "0\n"
        "  100   # a comment\n"
        "250 299 cut 0.5\n"
        "end\n";
    CHECK(n265_shotlist_parse_str(&l, t, err, sizeof err) == 0,
          "a hand list was refused: %s", err);
    CHECK(l.n == 3 && !l.fp.have && l.fp.frames == 300, "n %d", l.n);
    if (l.n == 3) {
        CHECK(l.s[0].first == 0 && l.s[0].last == 99 &&
              l.s[0].kind == N265_SHOT_START, "shot 0");
        CHECK(l.s[1].first == 100 && l.s[1].last == 249 &&
              l.s[1].kind == N265_SHOT_HAND && l.s[1].conf < 0, "shot 1");
        CHECK(l.s[2].first == 250 && l.s[2].last == 299 &&
              l.s[2].kind == N265_SHOT_CUT && l.s[2].conf == 0.5, "shot 2");
    }
    n265_shotlist_free(&l);

    /* no frame count: the last shot stays open until the input closes it */
    CHECK(n265_shotlist_parse_str(&l, "yah265-shots 1\nshots 2\n0\n40\nend\n",
                                  err, sizeof err) == 0, "open list: %s", err);
    CHECK(l.n == 2 && l.s[1].last == -1, "the open shot was closed");
    n265_shot_fp have = { 0, 90, 64, 64, 8, 0, { { 0 } } };
    CHECK(n265_shotlist_check(&l, &have, err, sizeof err) == 0,
          "an open list was refused: %s", err);
    CHECK(l.s[1].last == 89, "check did not close the last shot");
    n265_shotlist_free(&l);
}

static void parse_refused(void)
{
    static const struct { const char *text, *why; } bad[] = {
        { "", "empty" },
        { "yah264-scan 1\nshots 1\n0\nend\n", "another tool's magic" },
        { "yah265-shots 3\nshots 1\n0\nend\n", "a later version" },
        { "yah265-shots 1\nshots 1\n0 qp=1\nend\n", "qp= in version 1" },
        { "yah265-shots 2\nshots 1\n0 f=1,2,3,4,5,6,7,8,9,10,11\nend\n",
          "features without a features line" },
        { "yah265-shots 2\nfeatures 1\nshots 1\n0 f=1,2,3\nend\n",
          "too few features" },
        { "yah265-shots 2\nfeatures 1\nshots 1\n"
          "0 f=1,2,3,4,5,6,7,8,9,10,11,12\nend\n", "too many features" },
        { "yah265-shots 2\nshots 1\n0 qp=13\nend\n", "an offset past 12" },
        { "yah265-shots 2\nshots 1\n0 qp=x\nend\n", "a word for an offset" },
        { "yah265-shots 2\nshots 1\n0 qp=1 qp=2\nend\n", "two offsets" },
        { "yah265-shots 2\nshots 1\n0 qp=1 99\nend\n",
          "a field after an offset" },
        { "yah265-shots 2\nshots 1\nqp=1\nend\n", "an offset alone" },
        { "yah265-shots 2\nshots 1\n0 w=1\nend\n", "an unknown field" },
        { "yah265-shots 1\nshots 1\n5\nend\n", "first shot not at 0" },
        { "yah265-shots 1\nshots 2\n0\n0\nend\n", "a repeated start" },
        { "yah265-shots 1\nshots 3\n0\n50\n20\nend\n", "out of order" },
        { "yah265-shots 1\nshots 2\n0 40\n50\nend\n", "a gap between shots" },
        { "yah265-shots 1\nshots 2\n0 60\n50\nend\n", "overlapping shots" },
        { "yah265-shots 1\nshots 1\n0 -1\nend\n", "a last before its first" },
        { "yah265-shots 1\nshots 2\n0\n10 - flash\nend\n", "an unknown kind" },
        { "yah265-shots 1\nshots 2\n0\n10 - cut x\nend\n", "a bad confidence" },
        { "yah265-shots 1\nshots 2\n0\nten\nend\n", "a word for a frame" },
        { "yah265-shots 1\nshots 1\n0 a b c d\nend\n", "too many fields" },
        { "yah265-shots 1\nshots 3\n0\n10\nend\n", "fewer shots than said" },
        { "yah265-shots 1\nshots 1\n0\n", "no end" },
        { "yah265-shots 1\nshots 1\n0\nend\n5\n", "text after end" },
        { "yah265-shots 1\nwidth 5\nshots 1\n0\nend\n", "an unknown key" },
        { "yah265-shots 1\n0\nend\n", "no shots line" },
        { "yah265-shots 1\nframes 10\nshots 2\n0\n10\nend\n",
          "a start past the frame count" },
        { "yah265-shots 1\nframes 10\nshots 1\n0 5\nend\n",
          "a last shot that stops short" },
        { "yah265-shots 1\nframes 3\nshots 4\n0\n1\n2\n3\nend\n",
          "more shots than frames" },
        { "yah265-shots 1\nframes 10\nluma 12\nshots 1\n0\nend\n",
          "half a fingerprint" },
        { "yah265-shots 1\nframes 10\nsource 64x64 depth=12\nshots 1\n0\nend\n",
          "a 12-bit source" },
    };
    for (size_t k = 0; k < sizeof bad / sizeof bad[0]; k++) {
        n265_shotlist l;
        char err[256] = "";
        int r = n265_shotlist_parse_str(&l, bad[k].text, err, sizeof err);
        CHECK(r != 0, "accepted %s", bad[k].why);
        CHECK(r != 0 ? l.s == NULL && l.n == 0 : 1, "%s left a list behind",
              bad[k].why);
        CHECK(r != 0 ? err[0] != '\0' : 1, "%s refused without a reason",
              bad[k].why);
        if (r == 0)
            n265_shotlist_free(&l);
    }
}

/* a w x h 8-bit plane, a horizontal ramp plus `bias` */
static uint8_t *plane8(int w, int h, int bias)
{
    uint8_t *p = malloc((size_t)w * h);
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            int v = x * 255 / (w - 1) + bias;
            p[(size_t)y * w + x] = (uint8_t)(v > 255 ? 255 : v < 0 ? 0 : v);
        }
    return p;
}

static void fingerprint(n265_shot_fp *fp, int w, int h, int bias)
{
    uint8_t *p = plane8(w, h, bias);
    uint64_t hs[3];
    memset(fp, 0, sizeof *fp);
    fp->have = 1;
    fp->frames = 120;
    fp->w = w;
    fp->h = h;
    fp->depth = 8;
    for (int k = 0; k < 3; k++)
        n265_shot_fp_frame(p, w, h, 8, &hs[k], fp->thumb[k]);
    fp->luma = n265_shot_fp_combine(hs);
    free(p);
}

static void roundtrip_and_refusals(void)
{
    n265_shotlist l;
    memset(&l, 0, sizeof l);
    n265_shot s[3] = {
        { .first = 0, .last = 39, .kind = N265_SHOT_START, .conf = -1.0 },
        { .first = 40, .last = 89, .kind = N265_SHOT_CUT, .conf = 0.912 },
        { .first = 90, .last = 119, .kind = N265_SHOT_HAND, .conf = -1.0 } };
    l.s = s;
    l.n = 3;
    fingerprint(&l.fp, 640, 360, 0);
    l.scan_w = 640;
    l.scan_h = 360;
    l.scan_mk = 4;
    char path[] = "/tmp/n265_shots_XXXXXX";
    int fd = mkstemp(path);
    FILE *f = fd >= 0 ? fdopen(fd, "w") : NULL;
    CHECK(f && n265_shotlist_write(f, &l) == 0, "write failed");
    if (f)
        fclose(f);
    n265_shotlist r;
    char err[256] = "";
    CHECK(n265_shotlist_parse(&r, path, err, sizeof err) == 0,
          "the writer's own list was refused: %s", err);
    unlink(path);
    if (r.n != 3)
        return;
    CHECK(r.fp.have && r.fp.luma == l.fp.luma && r.fp.frames == 120 &&
          r.fp.w == 640 && r.fp.h == 360 &&
          !memcmp(r.fp.thumb, l.fp.thumb, sizeof l.fp.thumb),
          "the fingerprint did not survive the round trip");
    CHECK(r.scan_w == 640 && r.scan_h == 360 && r.scan_mk == 4, "scan line");
    for (int k = 0; k < 3; k++)
        CHECK(r.s[k].first == s[k].first && r.s[k].last == s[k].last &&
              r.s[k].kind == s[k].kind, "shot %d changed", k);
    CHECK(r.s[1].conf > 0.9115 && r.s[1].conf < 0.9125, "confidence");

    /* the same input passes */
    n265_shot_fp have;
    fingerprint(&have, 640, 360, 0);
    CHECK(n265_shotlist_check(&r, &have, err, sizeof err) == 0,
          "refused its own input: %s", err);
    /* another rung of it passes on the thumbnails */
    fingerprint(&have, 1280, 720, 0);
    CHECK(n265_shotlist_check(&r, &have, err, sizeof err) == 0,
          "refused a rung of its own input: %s", err);
    fingerprint(&have, 426, 240, 1);
    CHECK(n265_shotlist_check(&r, &have, err, sizeof err) == 0,
          "refused a small rung one level off: %s", err);

    /* and the refusals */
    fingerprint(&have, 640, 360, 0);
    have.frames = 119;
    CHECK(n265_shotlist_check(&r, &have, err, sizeof err) != 0,
          "accepted a different frame count");
    fingerprint(&have, 640, 480, 0);
    CHECK(n265_shotlist_check(&r, &have, err, sizeof err) != 0,
          "accepted another shape");
    fingerprint(&have, 640, 360, 3);
    CHECK(n265_shotlist_check(&r, &have, err, sizeof err) != 0,
          "accepted other pictures at the same size");
    fingerprint(&have, 1280, 720, 40);
    CHECK(n265_shotlist_check(&r, &have, err, sizeof err) != 0,
          "accepted other pictures at another size");
    n265_shotlist_free(&r);

    /* a list without a fingerprint is held to the frame count alone */
    CHECK(n265_shotlist_parse_str(&r, "yah265-shots 1\nshots 2\n0\n100\nend\n",
                                  err, sizeof err) == 0, "%s", err);
    fingerprint(&have, 64, 64, 0);
    have.frames = 100;
    CHECK(n265_shotlist_check(&r, &have, err, sizeof err) != 0,
          "accepted a start past the input's last frame");
    n265_shotlist_free(&r);
    CHECK(n265_shotlist_parse_str(&r, "yah265-shots 1\nshots 2\n0\n50 89\nend\n",
                                  err, sizeof err) == 0, "%s", err);
    CHECK(n265_shotlist_check(&r, &have, err, sizeof err) != 0,
          "accepted a last shot that ends before the input");
    n265_shotlist_free(&r);
}

static void frame_math(void)
{
    /* the thumbnail is the 8x8 grid of block means, 10-bit scaled to 8 */
    int w = 16, h = 16;
    uint8_t p8[256], p10[512];
    for (int k = 0; k < 256; k++) {
        p8[k] = (uint8_t)k;
        unsigned v = (unsigned)k << 2;
        p10[2 * k] = (uint8_t)(v & 0xff);
        p10[2 * k + 1] = (uint8_t)(v >> 8);
    }
    uint64_t h8, h10;
    uint8_t t8[64], t10[64];
    n265_shot_fp_frame(p8, w, h, 8, &h8, t8);
    n265_shot_fp_frame(p10, w, h, 10, &h10, t10);
    /* block (0,0) holds 0, 1, 16, 17: mean 8.5, rounded up */
    CHECK(t8[0] == 9, "block mean %d, wanted 9", t8[0]);
    CHECK(t8[63] == 247, "last block mean %d, wanted 247", t8[63]);
    CHECK(!memcmp(t8, t10, 64), "10-bit thumbnail differs from 8-bit");
    CHECK(h8 != h10, "the luma hash ignores the sample width");
    uint64_t a[3] = { 1, 2, 3 }, b[3] = { 1, 3, 2 };
    CHECK(n265_shot_fp_combine(a) != n265_shot_fp_combine(b),
          "the combined hash ignores order");

    /* the analysis size keeps the aspect, at an even height */
    CHECK(n265_shotscan_h(1920, 1080) == 360, "1080p -> %d",
          n265_shotscan_h(1920, 1080));
    CHECK(n265_shotscan_h(960, 540) == 360, "540p -> %d",
          n265_shotscan_h(960, 540));
    CHECK(n265_shotscan_h(352, 288) == 524, "CIF -> %d",
          n265_shotscan_h(352, 288));
    CHECK(n265_shotscan_h(4096, 64) == 16, "the floor");

    /* the resample: a flat plane stays flat, and a 2:1 box averages */
    uint8_t src[4 * 2] = { 10, 20, 30, 40, 50, 60, 70, 80 }, dst[2];
    n265_shotscan_resample(src, 4, 2, 8, dst, 2, 1);
    CHECK(dst[0] == 35 && dst[1] == 55, "box %d %d, wanted 35 55", dst[0],
          dst[1]);
    uint8_t up[4];
    n265_shotscan_resample(src, 4, 2, 8, up, 4, 1);
    CHECK(up[0] == 30 && up[3] == 60, "column box %d %d", up[0], up[3]);
}

/* version 2: the scan's features and a hand offset, and the plan */
static void plan_fields(void)
{
    n265_shotlist l;
    char err[256] = "";
    const char *t =
        "yah265-shots 2\n"
        "frames 100\n"
        "features 1\n"
        "shots 3\n"
        "0 39 start - f=9,8,0.3,1,0.2,3,6,0.1,0,0.5,5.32\n"
        "40 49 cut 2.0 f=10,9,0.5,2,0.2,3,7,0,0,0.4,3.32\n"
        "50 99 cut 1.5 f=8,6,0.1,0.5,0.1,3,5,0.3,0.2,0.3,5.64\n"
        "end\n";
    CHECK(n265_shotlist_parse_str(&l, t, err, sizeof err) == 0,
          "a scan's list was refused: %s", err);
    CHECK(l.n == 3 && l.feat_ver == 1 && l.s[2].has_feat && !l.s[2].has_qp &&
          l.s[1].feat[0] == 10.0 && l.s[2].feat[10] == 5.64, "features");
    double off[3] = { 9, 9, 9 };
    CHECK(n265_shotplan(&l, off, err, sizeof err) == 0,
          "the plan refused features: %s", err);
    double m = 0.0;
    for (int k = 0; k < 3; k++) {
        CHECK(off[k] >= -N265_SHOTPLAN_CLAMP && off[k] <= N265_SHOTPLAN_CLAMP,
              "shot %d's offset %f is past the clamp", k, off[k]);
        m += off[k] * (l.s[k].last - l.s[k].first + 1);
    }
    CHECK(m > -1e-9 && m < 1e-9, "the offsets are not centred (%f)", m);
    CHECK(off[1] == off[0], "a 10-frame shot did not take the offset of the "
          "shot before it");

    /* the writer keeps the features */
    char path[] = "/tmp/n265_plan_XXXXXX";
    int fd = mkstemp(path);
    FILE *f = fd >= 0 ? fdopen(fd, "w") : NULL;
    CHECK(f && n265_shotlist_write(f, &l) == 0, "write failed");
    if (f)
        fclose(f);
    n265_shotlist r;
    CHECK(n265_shotlist_parse(&r, path, err, sizeof err) == 0,
          "the writer's list was refused: %s", err);
    unlink(path);
    if (r.n == 3) {
        double o2[3];
        CHECK(n265_shotplan(&r, o2, err, sizeof err) == 0 &&
              !memcmp(o2, off, sizeof off), "the plan moved on a round trip");
        n265_shotlist_free(&r);
    }
    n265_shotlist_free(&l);

    /* hand offsets are taken as written */
    CHECK(n265_shotlist_parse_str(&l, "yah265-shots 2\nshots 2\n0 qp=-1.5\n"
                                  "30 - hand - qp=2\nend\n", err,
                                  sizeof err) == 0, "%s", err);
    CHECK(n265_shotplan(&l, off, err, sizeof err) == 0 &&
          off[0] == -1.5 && off[1] == 2.0, "hand offsets moved");
    n265_shotlist_free(&l);

    /* and refused when the list says too little */
    CHECK(n265_shotlist_parse_str(&l, "yah265-shots 2\nshots 2\n0 qp=1\n"
                                  "30\nend\n", err, sizeof err) == 0, "%s",
          err);
    CHECK(n265_shotplan(&l, off, err, sizeof err) != 0 && err[0],
          "accepted offsets on some shots and not others");
    n265_shotlist_free(&l);
    CHECK(n265_shotlist_parse_str(&l, "yah265-shots 1\nshots 2\n0\n30\n"
                                  "end\n", err, sizeof err) == 0, "%s", err);
    err[0] = '\0';
    CHECK(n265_shotplan(&l, off, err, sizeof err) != 0 && err[0],
          "planned a list with no features");
    n265_shotlist_free(&l);
}

int main(void)
{
    parse_ok();
    plan_fields();
    parse_refused();
    roundtrip_and_refusals();
    frame_math();
    if (fails) {
        printf("%d failure(s)\n", fails);
        return 1;
    }
    printf("shotlist: all checks passed\n");
    return 0;
}
