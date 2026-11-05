/*
 * The command line's HDR10+ and Dolby Vision readers (cli/hdr_dyn.c).
 *
 * The ST 2094-40 writer is checked against a message another encoder
 * wrote: the first frame of hdr10plus_tool's Tears of Steel sample
 * (assets/metadata/ToS-s01.h265), a profile B payload with a Bezier curve.
 * Its fields are the ones hdr10plus_tool extracts from that frame, so the
 * bytes below are what any HDR10+ reader already accepts.
 * Copyright (c) 2026, the yah265 authors. GPL-2.0-or-later; see LICENSE.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "hdr_dyn.h"

static int fails;

#define CHECK(c) do { if (!(c)) { \
    fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #c); fails++; } \
} while (0)

static const uint8_t tos0[64] = {
    0xb5, 0x00, 0x3c, 0x00, 0x01, 0x04, 0x01, 0x40, 0x00, 0x0c, 0x80, 0x8b,
    0x4c, 0x41, 0xff, 0x1b, 0xd6, 0x01, 0x03, 0x64, 0x08, 0x00, 0x0c, 0x28,
    0xdb, 0x20, 0x50, 0x00, 0xac, 0xc8, 0x00, 0xe1, 0x90, 0x03, 0x6e, 0x58,
    0x10, 0x32, 0xd0, 0x2a, 0x6a, 0xf8, 0x48, 0xf3, 0x18, 0xe1, 0xb4, 0x00,
    0x40, 0x44, 0x10, 0x25, 0x09, 0xa6, 0xae, 0x5c, 0x83, 0x50, 0xdd, 0xf9,
    0x8e, 0xc7, 0xbd, 0x00,
};

static const char *tos0_json =
    "{\"JSONInfo\":{\"HDR10plusProfile\":\"B\",\"Version\":\"1.0\"},"
    "\"SceneInfo\":[{\"BezierCurveData\":{\"Anchors\":[265,666,741,800,848,"
    "887,920,945,957],\"KneePointX\":17,\"KneePointY\":64},"
    "\"LuminanceParameters\":{\"AverageRGB\":1037,\"LuminanceDistributions\":"
    "{\"DistributionIndex\":[1,5,10,25,50,75,90,95,99],\"DistributionValues\":"
    "[3,14024,43,56,219,1036,2714,4668,14445]},\"MaxScl\":[17830,16895,"
    "14252]},\"NumberOfWindows\":1,\"TargetedSystemDisplayMaximumLuminance\":"
    "400,\"SceneFrameIndex\":0,\"SceneId\":0,\"SequenceFrameIndex\":0}]}";

static char *tmp_file(const void *data, size_t n)
{
    static char path[64];
    snprintf(path, sizeof path, "/tmp/n265_hdr_dyn_XXXXXX");
    int fd = mkstemp(path);
    if (fd < 0)
        return NULL;
    if (write(fd, data, n) != (ssize_t)n) {
        close(fd);
        return NULL;
    }
    close(fd);
    return path;
}

int main(void)
{
    /* the writer, field by field */
    n265_h10p_frame f = {
        .target_max = 400, .maxscl = { 17830, 16895, 14252 },
        .avg_maxrgb = 1037, .npct = 9,
        .pct = { 1, 5, 10, 25, 50, 75, 90, 95, 99 },
        .pct_val = { 3, 14024, 43, 56, 219, 1036, 2714, 4668, 14445 },
        .tone_mapping = 1, .knee_x = 17, .knee_y = 64, .nanchor = 9,
        .anchor = { 265, 666, 741, 800, 848, 887, 920, 945, 957 },
    };
    uint8_t out[128];
    size_t n = n265_h10p_payload(&f, out, sizeof out);
    CHECK(n == sizeof tos0 && !memcmp(out, tos0, sizeof tos0));
    CHECK(n265_h10p_payload(&f, out, 20) == 0);

    /* the same frame through the JSON reader */
    char err[256];
    n265_dyn_tab t;
    char *p = tmp_file(tos0_json, strlen(tos0_json));
    CHECK(p != NULL);
    if (p) {
        CHECK(n265_h10p_load(p, &t, err, sizeof err) == 0);
        CHECK(t.n == 1 && t.len[0] == sizeof tos0 &&
              !memcmp(t.buf[0], tos0, sizeof tos0));
        n265_dyn_free(&t);
        unlink(p);
    }

    /* profile B with no target display is refused */
    char bad[2048];
    snprintf(bad, sizeof bad, "%s", tos0_json);
    char *at = strstr(bad, "Luminance\":400");
    if (at)
        memcpy(at + 11, "  0", 3);
    p = tmp_file(bad, strlen(bad));
    CHECK(at && p && n265_h10p_load(p, &t, err, sizeof err) < 0);
    if (p)
        unlink(p);

    /* An RPU file: one entry with a type-62 header and an escape, one
     * without a header, as dovi_tool writes them. The bytes are the head of
     * a real profile 8 RPU (dovi_tool's assets/tests/profile8.bin), which
     * is as far as the reader looks. */
#define P8 0x19, 0x08, 0x09, 0x08, 0x40, 0x61, 0x36, 0x50, 0x6f, 0x00, 0x3f, \
           0xf8, 0x01, 0xff, 0xc0, 0x0f, 0xff, 0xd0
    static const uint8_t rpu[] = {
        0, 0, 0, 1, 0x7c, 0x01, P8, 0x00, 0x00, 0x03, 0x01, 0x80,
        0, 0, 0, 1, P8, 0x80,
    };
    static const uint8_t e0[] = { P8, 0x00, 0x00, 0x01, 0x80 };
    static const uint8_t e1[] = { P8, 0x80 };
    p = tmp_file(rpu, sizeof rpu);
    CHECK(p != NULL);
    if (p) {
        CHECK(n265_rpu_load(p, &t, err, sizeof err) == 0);
        CHECK(t.n == 2);
        if (t.n == 2) {
            CHECK(t.len[0] == sizeof e0 && !memcmp(t.buf[0], e0, sizeof e0));
            CHECK(t.len[1] == sizeof e1 && !memcmp(t.buf[1], e1, sizeof e1));
        }
        n265_dyn_free(&t);
        unlink(p);
    }

    /* the profile from the header: profile 5 (assets/tests/profile5.bin)
     * differs from profile 8 in vdr_rpu_profile and the full range flag */
    static const uint8_t p5[] = {
        0x19, 0x08, 0x09, 0x00, 0x40, 0x61, 0xb6, 0x50, 0x6f, 0x00, 0x3f,
        0xf8, 0x01, 0xff, 0xc0, 0x0f, 0xff, 0xd0, 0x80,
    };
    CHECK(n265_rpu_profile(e1, sizeof e1) == 8);
    CHECK(n265_rpu_profile(p5, sizeof p5) == 5);
    CHECK(n265_rpu_profile(e1, 3) == 0);
    uint8_t f5[4 + sizeof p5] = { 0, 0, 0, 1 };
    memcpy(f5 + 4, p5, sizeof p5);
    p = tmp_file(f5, sizeof f5);
    CHECK(p && n265_rpu_load(p, &t, err, sizeof err) < 0 &&
          strstr(err, "profile 5"));
    if (p)
        unlink(p);

    if (fails)
        fprintf(stderr, "hdr_dyn: %d check(s) failed\n", fails);
    return fails != 0;
}
