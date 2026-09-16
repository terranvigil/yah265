/*
 * Unit tests: the two NAME TABLES of phase 14 item 15 -
 * --video-signal-type-preset (src/encoder/signal.c) and -t/--tune
 * (src/encoder/preset.c).
 * Copyright (c) 2026, the yah265 authors. GPL-2.0-or-later; see LICENSE.
 *
 * A name table is the one kind of code that ships with a typo and no
 * symptom. Nothing downstream reads a colour description back: a stream
 * tagged BT.709 when the caller asked for BT.2100 decodes, conforms,
 * matches both decoders bit for bit and is wrong. The same is true of a
 * tune - a composition that silently dropped one of its four knobs would
 * cost speed or quality that nobody could attribute. So the expectations
 * below are written as NUMBERS, taken from Annex E's tables and from the
 * reference encoder's own --help, and not by calling the same parser the
 * table is built from.
 *
 * The refusals are under test too, and for the same reason: a refusal
 * that stops printing its reason is a refusal that reads as a bug.
 */

#include <stdio.h>
#include <string.h>

#include "encoder/signal.h"
#include "encoder/preset.h"

static int fails;

static void chk(int ok, const char *what)
{
    if (!ok) {
        printf("FAIL %s\n", what);
        fails++;
    }
}

/* ---- 1. --video-signal-type-preset ---------------------------------
 *
 * Every system-id, as the syntax element values of E.2.1 written out by
 * hand: colour_primaries (Table E.3), transfer_characteristics (E.4),
 * matrix_coeffs (E.5), video_full_range_flag, chroma_sample_loc_type.
 * -1 is a field the preset leaves absent.
 *
 * Two rows carry x265's values rather than the ones the NAME implies,
 * which is the deliberate part of this table and the part most likely to
 * be "fixed" by a later reader: BT2020_YCC_NCL names matrix 1 (bt709)
 * where the name says 9, and BT2020_RGB names transfer 16 (PQ) with
 * matrix 9 where an RGB set says 0. They are asserted here with a
 * comment so that changing them is a deliberate act with a failing test
 * in front of it.
 */
static const struct {
    const char *name;
    int prim, trc, mtx, range, cloc;
} sys_expect[] = {
    { "BT601_525",       6,  6,  6, 0,  0 },
    { "BT601_626",       5,  6,  5, 0,  0 },
    { "BT601_625",       5,  6,  5, 0,  0 },   /* the alias */
    { "BT709_YCC",       1,  1,  1, 0,  0 },
    { "BT709_RGB",       1,  1,  0, 0, -1 },
    { "BT2020_YCC_NCL",  9, 14,  1, 0,  2 },   /* x265's matrix, not 9 */
    { "BT2020_RGB",      9, 16,  9, 0, -1 },   /* x265's transfer/matrix */
    { "BT2100_PQ_YCC",   9, 16,  9, 0,  2 },
    { "BT2100_PQ_ICTCP", 9, 16, 14, 0,  2 },
    { "BT2100_PQ_RGB",   9, 16,  0, 0, -1 },
    { "BT2100_HLG_YCC",  9, 18,  9, 0,  2 },
    { "BT2100_HLG_RGB",  9, 18,  0, 0, -1 },
    { "FR709_RGB",       1,  1,  0, 1, -1 },
    { "FR2020_RGB",      9, 14,  0, 1, -1 },
    { "FRP3D65_YCC",    12,  1,  6, 1,  1 },
    { NULL, 0, 0, 0, 0, 0 }
};

static void test_vsp_values(void)
{
    for (int i = 0; sys_expect[i].name; i++) {
        int v[N265_VSP_NFIELD];
        uint32_t md[10];
        int have_md = 1;        /* poisoned: the parser must clear it */
        const char *why = "poison";
        int rc = n265_vsp_parse(sys_expect[i].name, v, md, &have_md, &why);
        chk(rc == N265_VSP_OK, sys_expect[i].name);
        chk(v[N265_VUI_COLORPRIM] == sys_expect[i].prim, "colour_primaries");
        chk(v[N265_VUI_TRANSFER] == sys_expect[i].trc, "transfer");
        chk(v[N265_VUI_COLORMATRIX] == sys_expect[i].mtx, "matrix_coeffs");
        chk(v[N265_VUI_RANGE] == sys_expect[i].range, "full_range_flag");
        chk(v[N265_VSP_CHROMALOC] == sys_expect[i].cloc, "chroma_loc");
        /* no system-id sets these two, and a table that started to would
         * be writing syntax the name never promised */
        chk(v[N265_VUI_VIDEOFORMAT] == -1, "video_format absent");
        chk(v[N265_VUI_OVERSCAN] == -1, "overscan absent");
        chk(have_md == 0, "no volume, no mastering display");
        chk(why == NULL, "no refusal, no reason");
    }
}

/* The table the CLI prints in its refusal must be the table the parser
 * reads, entry for entry and in order. A second copy that drifted would
 * offer a name that does not parse. */
static void test_vsp_names(void)
{
    int n = 0;
    for (; n265_vsp_name(n); n++) {
        chk(sys_expect[n].name != NULL, "name table is longer than expected");
        if (!sys_expect[n].name)
            break;
        chk(!strcmp(n265_vsp_name(n), sys_expect[n].name), "name order");
    }
    chk(sys_expect[n].name == NULL, "name table is shorter than expected");
    chk(n265_vsp_name(-1) == NULL, "name(-1)");
    chk(n265_vsp_name(n + 5) == NULL, "name past the end");
}

/* The colour volume: its ten ST 2086 values, as the P3-D65 1000-nit
 * --master-display string gives them. Order is G, B, R, WP, then max and min luminance. */
static void test_vsp_volume(void)
{
    static const uint32_t p3d65_1000[10] = {
        13250, 34500, 7500, 3000, 34000, 16000, 15635, 16450,
        10000000, 5
    };
    static const uint32_t p3d65_4000[10] = {
        13250, 34500, 7500, 3000, 34000, 16000, 15635, 16450,
        40000000, 50
    };
    int v[N265_VSP_NFIELD];
    uint32_t md[10];
    int have_md = 0;
    const char *why = NULL;

    chk(n265_vsp_parse("BT2100_PQ_YCC:P3D65x1000n0005", v, md, &have_md,
                       &why) == N265_VSP_OK, "PQ + P3D65 1000");
    chk(have_md == 1, "volume arms the mastering display");
    chk(!memcmp(md, p3d65_1000, sizeof md), "P3D65x1000n0005 values");
    /* the system-id's own fields are still set beside the volume */
    chk(v[N265_VUI_TRANSFER] == 16, "volume does not eat the system-id");

    chk(n265_vsp_parse("BT2100_PQ_RGB:P3D65x4000n005", v, md, &have_md,
                       &why) == N265_VSP_OK, "PQ RGB + P3D65 4000");
    chk(!memcmp(md, p3d65_4000, sizeof md), "P3D65x4000n005 values");

    /* the three that take a volume, and one that does not */
    chk(n265_vsp_parse("BT2100_PQ_ICTCP:P3D65x1000n0005", v, md, &have_md,
                       &why) == N265_VSP_OK, "PQ ICTCP takes a volume");
    chk(n265_vsp_parse("BT2100_HLG_YCC:P3D65x1000n0005", v, md, &have_md,
                       &why) == N265_VSP_VOLUME_SYSTEM, "HLG takes none");
    chk(n265_vsp_parse("BT709_YCC:P3D65x1000n0005", v, md, &have_md,
                       &why) == N265_VSP_VOLUME_SYSTEM, "BT709 takes none");

    /* the volume we refuse BY NAME, with its reason */
    why = NULL;
    chk(n265_vsp_parse("BT2100_PQ_YCC:BT2100x108n0005", v, md, &have_md,
                       &why) == N265_VSP_VOLUME_REFUSED, "BT2100 volume");
    chk(why && strlen(why) > 40, "the refusal carries its reason");

    /* and the volume names the refusal prints */
    chk(n265_vsp_volume_name(0) && n265_vsp_volume_name(1) &&
        n265_vsp_volume_name(2), "three volume names");
    chk(n265_vsp_volume_name(3) == NULL, "volume names terminate");
}

static void test_vsp_refusals(void)
{
    int v[N265_VSP_NFIELD];
    uint32_t md[10];
    int have_md = 0;
    const char *why = NULL;

    chk(n265_vsp_parse("bogus", v, md, &have_md, &why) ==
        N265_VSP_BAD_SYSTEM, "unknown system-id");
    /* case matters: x265 spells these in capitals and a lower-case
     * spelling that quietly worked here would move between the two
     * encoders and stop working */
    chk(n265_vsp_parse("bt709_ycc", v, md, &have_md, &why) ==
        N265_VSP_BAD_SYSTEM, "lower case is not the name");
    chk(n265_vsp_parse("", v, md, &have_md, &why) ==
        N265_VSP_BAD_SYSTEM, "empty");
    chk(n265_vsp_parse(NULL, v, md, &have_md, &why) ==
        N265_VSP_BAD_SYSTEM, "null");
    chk(n265_vsp_parse("BT709_YCC:", v, md, &have_md, &why) ==
        N265_VSP_VOLUME_SYSTEM, "a bare colon is a volume on BT709");
    chk(n265_vsp_parse("BT2100_PQ_YCC:", v, md, &have_md, &why) ==
        N265_VSP_BAD_VOLUME, "an empty volume is not a volume");
    chk(n265_vsp_parse("BT2100_PQ_YCC:nope", v, md, &have_md, &why) ==
        N265_VSP_BAD_VOLUME, "unknown volume");
    /* a name that is a PREFIX of a real one is not that name */
    chk(n265_vsp_parse("BT709", v, md, &have_md, &why) ==
        N265_VSP_BAD_SYSTEM, "a prefix is not a name");
}

/* ---- 2. -t/--tune -------------------------------------------------- */

static void test_tune(void)
{
    int t[N265_TUNE_NFIELD];
    const char *why = "poison";

    for (int i = 0; i < N265_TUNE_NFIELD; i++)
        t[i] = -99;
    chk(n265_tune_parse("zerolatency", t, &why) == N265_TUNE_OK,
        "zerolatency is accepted");
    chk(why == NULL, "an accepted tune carries no reason");
    /* the composition, written out: no B frames, a window of one, no
     * adaptive run length, no cu-tree, one frame in flight */
    chk(t[N265_TUNE_BFRAMES] == 0, "zerolatency bframes");
    chk(t[N265_TUNE_LOOKAHEAD] == 1, "zerolatency rc-lookahead");
    chk(t[N265_TUNE_BADAPT] == 0, "zerolatency b-adapt");
    chk(t[N265_TUNE_CUTREE] == 0, "zerolatency cutree");
    chk(t[N265_TUNE_FRAMEWORK] == 1, "zerolatency frames in flight");

    /* x265 takes the hyphenated spelling too */
    int u[N265_TUNE_NFIELD];
    chk(n265_tune_parse("zero-latency", u, &why) == N265_TUNE_OK,
        "zero-latency alias");
    chk(!memcmp(t, u, sizeof t), "the alias is the same composition");

    /* the four refusals, each by name and each with a reason */
    static const char * const refused[] = { "psnr", "ssim", "grain",
                                            "fastdecode", "fast-decode",
                                            NULL };
    for (int i = 0; refused[i]; i++) {
        why = NULL;
        for (int k = 0; k < N265_TUNE_NFIELD; k++)
            u[k] = -99;
        chk(n265_tune_parse(refused[i], u, &why) == N265_TUNE_REFUSED,
            refused[i]);
        chk(why && strlen(why) > 40, "the refusal carries its reason");
        /* a refused tune composes nothing: the CLI stops, but a caller
         * that did not would otherwise apply a half-filled array */
        for (int k = 0; k < N265_TUNE_NFIELD; k++)
            chk(u[k] == -99, "a refusal writes no knob");
    }

    chk(n265_tune_parse("bogus", u, &why) == N265_TUNE_UNKNOWN, "unknown");
    chk(n265_tune_parse(NULL, u, &why) == N265_TUNE_UNKNOWN, "null");
    chk(n265_tune_parse("", u, &why) == N265_TUNE_UNKNOWN, "empty");

    /* the names the refusal prints: x265's five, in x265's order */
    static const char * const five[] = { "zerolatency", "psnr", "ssim",
                                         "grain", "fastdecode", NULL };
    int n = 0;
    for (; n265_tune_name(n); n++) {
        chk(five[n] != NULL, "more tune names than x265 has");
        if (!five[n])
            break;
        chk(!strcmp(n265_tune_name(n), five[n]), "tune name order");
    }
    chk(five[n] == NULL, "fewer tune names than x265 has");
    chk(n265_tune_name(-1) == NULL && n265_tune_name(n + 5) == NULL,
        "tune names terminate");
}

int main(void)
{
    test_vsp_values();
    test_vsp_names();
    test_vsp_volume();
    test_vsp_refusals();
    test_tune();
    if (fails) {
        printf("name: %d failure(s)\n", fails);
        return 1;
    }
    printf("name: ok\n");
    return 0;
}
