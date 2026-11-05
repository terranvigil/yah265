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

/* Whole RPUs, escaped as they sit in a file: dovi_tool's
 * assets/tests/profile8.bin and profile5.bin (MIT). */
static const uint8_t rpu8[163] = {
    0x19, 0x08, 0x09, 0x08, 0x40, 0x61, 0x36, 0x50, 0x6f, 0x00, 0x3f, 0xf8,
    0x01, 0xff, 0xc0, 0x0f, 0xff, 0xd0, 0x00, 0x00, 0x08, 0x00, 0x00, 0x06,
    0x80, 0x00, 0x00, 0x40, 0x00, 0x00, 0x34, 0x00, 0x00, 0x03, 0x02, 0x00,
    0x00, 0x03, 0x01, 0xa2, 0x56, 0x60, 0x00, 0x03, 0x5e, 0xa2, 0x56, 0x6f,
    0x9f, 0xce, 0xb1, 0xc2, 0x56, 0x64, 0x4c, 0xa0, 0x00, 0x00, 0x10, 0x00,
    0x00, 0x03, 0x00, 0x80, 0x00, 0x00, 0x03, 0x00, 0x80, 0x00, 0x00, 0x03,
    0x01, 0xc3, 0x62, 0x24, 0x30, 0x18, 0x60, 0xa5, 0xe3, 0x08, 0xe0, 0x51,
    0x40, 0x00, 0x00, 0x1a, 0x63, 0xe5, 0xaf, 0xff, 0xf0, 0x00, 0x00, 0x03,
    0x00, 0x00, 0x03, 0x00, 0x00, 0x03, 0x00, 0x06, 0x02, 0x07, 0xdc, 0xe0,
    0x15, 0x18, 0x30, 0x08, 0x01, 0x69, 0xb9, 0x99, 0x80, 0xc0, 0x28, 0x21,
    0x80, 0x08, 0x00, 0x80, 0x05, 0xb6, 0x85, 0x41, 0x00, 0x02, 0x02, 0x00,
    0x00, 0x03, 0x00, 0x08, 0x05, 0x00, 0x00, 0x03, 0x00, 0x00, 0x03, 0x00,
    0x00, 0x03, 0x00, 0x12, 0x0c, 0x1f, 0x40, 0x00, 0x64, 0x00, 0x00, 0x03,
    0x00, 0x00, 0xd2, 0xd3, 0xf7, 0x22, 0x80,
};

static const uint8_t rpu5[162] = {
    0x19, 0x08, 0x09, 0x00, 0x40, 0x61, 0xb6, 0x50, 0x6f, 0x00, 0x3f, 0xf8,
    0x01, 0xff, 0xc0, 0x0f, 0xff, 0xd0, 0x00, 0x00, 0x10, 0x00, 0x00, 0x1b,
    0x00, 0x00, 0x03, 0x02, 0x00, 0x00, 0x03, 0x03, 0x60, 0x00, 0x00, 0x40,
    0x00, 0x00, 0x68, 0x80, 0x00, 0x0c, 0x7c, 0x1a, 0x44, 0x80, 0x03, 0xf1,
    0x6c, 0x11, 0x0c, 0x80, 0x00, 0x04, 0x2f, 0xa9, 0x5c, 0x00, 0x00, 0x03,
    0x00, 0x00, 0x20, 0x00, 0x00, 0x03, 0x00, 0x20, 0x00, 0x00, 0x03, 0x01,
    0x0a, 0xe7, 0xfa, 0x8f, 0xfa, 0x8f, 0xfa, 0x8d, 0x0a, 0xe7, 0xfa, 0x8f,
    0xfa, 0x8f, 0xfa, 0x8d, 0x0a, 0xe7, 0xff, 0xfc, 0x00, 0x00, 0x03, 0x00,
    0x00, 0x03, 0x00, 0x00, 0x03, 0x00, 0x01, 0x90, 0x81, 0xf7, 0x38, 0x05,
    0x46, 0x30, 0x08, 0x01, 0x69, 0xb9, 0x99, 0x80, 0xc0, 0x28, 0x21, 0x80,
    0x08, 0x00, 0x80, 0x05, 0xb6, 0x85, 0x41, 0x00, 0x02, 0x02, 0x00, 0x00,
    0x03, 0x00, 0x08, 0x05, 0x00, 0x00, 0x03, 0x00, 0x00, 0x03, 0x00, 0x00,
    0x03, 0x00, 0x12, 0x0c, 0x1f, 0x40, 0x00, 0x64, 0x00, 0x00, 0x03, 0x00,
    0x00, 0x7b, 0x68, 0xc4, 0x84, 0x80,
};

static char *tmp_file(const void *data, size_t n)
{
    static char path[1024];
    const char *dir = getenv("TMPDIR");
    snprintf(path, sizeof path, "%s/n265_hdr_dyn_XXXXXX",
             dir && *dir ? dir : "/tmp");
    int fd = mkstemp(path);
    if (fd < 0)
        return NULL;
    int ok = write(fd, data, n) == (ssize_t)n;
    close(fd);
    if (!ok) {
        unlink(path);
        return NULL;
    }
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

    /* An RPU file: the profile 8 RPU behind a four-byte start code with a
     * type-62 header, then again behind a three-byte one without, with
     * leading zero bytes and a trailing start code, as Annex B allows. */
    uint8_t f8[2 * sizeof rpu8 + 16], *q = f8;
    static const uint8_t sc4[] = { 0, 0, 0, 0, 1, 0x7c, 0x01 };
    memcpy(q, sc4, sizeof sc4), q += sizeof sc4;
    memcpy(q, rpu8, sizeof rpu8), q += sizeof rpu8;
    *q++ = 0, *q++ = 0, *q++ = 1;
    memcpy(q, rpu8, sizeof rpu8), q += sizeof rpu8;
    *q++ = 0, *q++ = 0, *q++ = 0, *q++ = 1;
    p = tmp_file(f8, (size_t)(q - f8));
    CHECK(p != NULL);
    if (p) {
        CHECK(n265_rpu_load(p, &t, err, sizeof err) == 0);
        CHECK(t.n == 2);
        if (t.n == 2) {
            CHECK(t.len[0] == 150 && t.len[1] == 150 &&
                  !memcmp(t.buf[0], t.buf[1], 150));
            CHECK(n265_rpu_profile(t.buf[0], t.len[0]) == 8);
        }
        n265_dyn_free(&t);
        unlink(p);
    }

    /* the same file cut one byte short fails the CRC */
    p = tmp_file(f8, sizeof sc4 + sizeof rpu8 - 1);
    CHECK(p && n265_rpu_load(p, &t, err, sizeof err) < 0 &&
          strstr(err, "CRC"));
    if (p)
        unlink(p);

    /* a profile 5 RPU is refused by its header */
    uint8_t f5[4 + sizeof rpu5] = { 0, 0, 0, 1 };
    memcpy(f5 + 4, rpu5, sizeof rpu5);
    p = tmp_file(f5, sizeof f5);
    CHECK(p && n265_rpu_load(p, &t, err, sizeof err) < 0 &&
          strstr(err, "profile 5"));
    if (p)
        unlink(p);
    CHECK(n265_rpu_profile(rpu8, 3) == 0);

    /* refusals that each get their own reason */
    static const struct { const char *json, *why; } bad_json[] = {
        { "{\"Scene\\u0049nfo\":[]}", "escape" },
        { "{\"SceneInfo\":[{\"Number\\u004ffWindows\":1}]}", "escape" },
        { "\xef\xbb\xbf{}", "byte order mark" },
        { "{\"SceneInfo\":[],\"SceneInfo\":[]}", "twice" },
    };
    for (size_t i = 0; i < sizeof bad_json / sizeof bad_json[0]; i++) {
        p = tmp_file(bad_json[i].json, strlen(bad_json[i].json));
        CHECK(p && n265_h10p_load(p, &t, err, sizeof err) < 0 &&
              strstr(err, bad_json[i].why));
        if (p)
            unlink(p);
    }
    char deep[200] = "{\"a\":";
    for (int i = 0; i < 70; i++)
        strcat(deep, "[");
    p = tmp_file(deep, strlen(deep));
    CHECK(p && n265_h10p_load(p, &t, err, sizeof err) < 0 &&
          strstr(err, "deeper"));
    if (p)
        unlink(p);

    /* an RPU cut from a stream at TemporalId 2 keeps its header off */
    uint8_t ft[6 + sizeof rpu8] = { 0, 0, 0, 1, 0x7c, 0x03 };
    memcpy(ft + 6, rpu8, sizeof rpu8);
    p = tmp_file(ft, sizeof ft);
    CHECK(p && n265_rpu_load(p, &t, err, sizeof err) == 0 && t.n == 1);
    n265_dyn_free(&t);
    if (p)
        unlink(p);

    /* an entry whose CRC checks out but whose header doesn't fit in the
     * body the CRC covers */
    static const uint8_t tiny[] = { 0, 0, 0, 1, 0x19, 0x08, 0x00, 0x8a, 0xff,
                                    0x27, 0x2f, 0x80 };
    p = tmp_file(tiny, sizeof tiny);
    CHECK(p && n265_rpu_load(p, &t, err, sizeof err) < 0);
    if (p)
        unlink(p);

    if (fails)
        fprintf(stderr, "hdr_dyn: %d check(s) failed\n", fails);
    return fails != 0;
}
