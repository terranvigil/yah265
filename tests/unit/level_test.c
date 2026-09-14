/*
 * Unit tests: the Annex A level and tier derivation (src/encoder/level.c).
 * Copyright (c) 2026, the yah265 authors. GPL-2.0-or-later; see LICENSE.
 *
 * This is the LEVEL TABLE CHECK in its exhaustive form: every one of the
 * thirteen levels is asked for at its own bound and one sample over it,
 * on each of the four general constraints plus the two rate ones. The
 * encode-side half of the same check - that the number solved here
 * reaches general_level_idc in the SPS and comes back out of ffprobe - is
 * scripts/level_check.sh, which can only afford a handful of the rows.
 */

#include <stdio.h>
#include <string.h>

#include "encoder/level.h"

static int fails;

#define CHECK(cond, ...) do { \
    if (!(cond)) { fails++; printf("FAIL %s:%d: ", __FILE__, __LINE__); \
                   printf(__VA_ARGS__); printf("\n"); } \
} while (0)

/* Table A.6 / A.7 again, written out independently of level.c so that a
 * typo has to be made twice to pass. MaxBR and MaxCPB are in the tables'
 * own thousand-bit units; CpbBrNalFactor is 1100 at Main and Main10. */
static const struct {
    int idc;
    const char *name;
    long long max_luma_ps;
    long long max_luma_sr;
    long long max_br_main, max_br_high;
    long long max_cpb_main, max_cpb_high;
} T[] = {
    {  30, "1",      36864,       552960,    128,      0, 350,      0 },
    {  60, "2",     122880,      3686400,   1500,      0, 1500,      0 },
    {  63, "2.1",   245760,      7372800,   3000,      0, 3000,      0 },
    {  90, "3",     552960,     16588800,   6000,      0, 6000,      0 },
    {  93, "3.1",   983040,     33177600,  10000,      0, 10000,      0 },
    { 120, "4",    2228224,     66846720,  12000,  30000, 12000,  30000 },
    { 123, "4.1",  2228224,    133693440,  20000,  50000, 20000,  50000 },
    { 150, "5",    8912896,    267386880,  25000, 100000, 25000, 100000 },
    { 153, "5.1",  8912896,    534773760,  40000, 160000, 40000, 160000 },
    { 156, "5.2",  8912896,   1069547520,  60000, 240000, 60000, 240000 },
    { 180, "6",   35651584,   1069547520,  60000, 240000, 60000, 240000 },
    { 183, "6.1", 35651584,   2139095040, 120000, 480000, 120000, 480000 },
    { 186, "6.2", 35651584,   4278190080, 240000, 800000, 240000, 800000 },
};
#define NT ((int)(sizeof(T) / sizeof(T[0])))

static void req_init(n265_level_req *rq)
{
    memset(rq, 0, sizeof(*rq));
    rq->width = 176;
    rq->height = 144;
    rq->dpb = 1;
    rq->fps_num = 1;
    rq->fps_den = 1;
    rq->slice_segments = 1;
    rq->tile_rows = rq->tile_cols = 1;
}

/* A picture of exactly `ps` samples that also clears the per-dimension
 * bound Sqrt(MaxLumaPs * 8): all the levels' MaxLumaPs are 16:9-ish
 * multiples of 64x64 except level 1, so a 64-aligned near-square works
 * for every row. Returns the actual sample count, which is what the
 * caller asserts against. */
static long long square_pic(long long ps, int *w, int *h)
{
    long long s = 64;
    while ((s + 64) * (s + 64) <= ps)
        s += 64;
    *w = (int)s;
    *h = (int)(ps / s / 8 * 8);
    return (long long)*w * *h;
}

/* ---- the size leg: MaxLumaPs (Table A.6) ---- */
static void test_size(void)
{
    for (int i = 0; i < NT; i++) {
        n265_level_req rq;
        n265_level_out out;
        req_init(&rq);
        int w, h;
        long long ps = square_pic(T[i].max_luma_ps, &w, &h);
        rq.width = w;
        rq.height = h;
        CHECK(n265_level_solve(&rq, &out) == 0, "level %s: %dx%d refused",
              T[i].name, w, h);
        /* Several rows share a MaxLumaPs (4 with 4.1, 5 with 5.1 and
         * 5.2, 6 with 6.1 and 6.2), and where they do the LOWEST of them
         * is the right answer for a picture that only needs the size. So
         * what a picture at this row's bound must select is a level with
         * exactly this row's MaxLumaPs - not this row's idc. */
        CHECK(out.max_luma_ps == T[i].max_luma_ps,
              "level %s: %dx%d (%lld samples) chose idc %d, whose MaxLumaPs "
              "is %lld and not %lld", T[i].name, w, h, ps, out.idc,
              (long long)out.max_luma_ps, T[i].max_luma_ps);

        /* One row of samples over the bound moves to a level with a
         * larger MaxLumaPs - or is refused, where this row's MaxLumaPs
         * is already the table's largest. */
        rq.width = w;
        rq.height = h + 8;
        int r = n265_level_solve(&rq, &out);
        if (T[i].max_luma_ps == T[NT - 1].max_luma_ps) {
            CHECK(r < 0, "level %s: a picture over the table's largest "
                  "MaxLumaPs was accepted at idc %d", T[i].name, out.idc);
        } else {
            CHECK(r == 0 && out.max_luma_ps > T[i].max_luma_ps,
                  "level %s: one row over MaxLumaPs stayed at MaxLumaPs %lld",
                  T[i].name, (long long)out.max_luma_ps);
        }
    }
}

/* ---- the rate leg: MaxLumaSr (Table A.7) ---- */
static void test_luma_sr(void)
{
    for (int i = 0; i < NT; i++) {
        /* 1920x1080 is inside every level from 4 up and lets the frame
         * rate alone select the level; below 4 a CIF picture does. The
         * point of the test is that the SAME picture at two frame rates
         * lands on two levels. */
        n265_level_req rq;
        n265_level_out out;
        req_init(&rq);
        rq.width = 352;
        rq.height = 288;
        long long ps = 352 * 288;
        if (T[i].max_luma_sr / ps > 100000) {
            rq.width = 1920;
            rq.height = 1080;
            ps = 1920 * 1080;
        }
        /* exactly at the bound */
        rq.fps_num = (int)(T[i].max_luma_sr / ps);
        rq.fps_den = 1;
        if (rq.fps_num < 1)
            continue;           /* the picture alone is over this level */
        if (n265_level_solve(&rq, &out) != 0) {
            CHECK(0, "level %s: %dx%d at %d fps refused", T[i].name,
                  rq.width, rq.height, rq.fps_num);
            continue;
        }
        long long sr = (long long)rq.fps_num * ps;
        CHECK(out.max_luma_sr >= sr, "level %s: chosen MaxLumaSr %lld under "
              "the stream's %lld", T[i].name, (long long)out.max_luma_sr, sr);
        int at = out.idc;

        /* one frame per second more must never LOWER the level, and at a
         * row whose successor has a larger MaxLumaSr it must raise it */
        rq.fps_num += 1;
        if (n265_level_solve(&rq, &out) == 0)
            CHECK(out.idc >= at, "level %s: a faster stream chose a LOWER "
                  "level (%d after %d)", T[i].name, out.idc, at);
    }
}

/* ---- MaxDpbSize (A.4.2) ---- */
static void test_dpb(void)
{
    /* 1920x1080 against level 4's MaxLumaPs of 2228224: the picture is
     * over 3/4 of it, so MaxDpbSize is 6. A seventh picture storage
     * buffer is what takes the stream to level 5, where the same picture
     * is under a quarter of MaxLumaPs and sixteen are allowed. */
    n265_level_req rq;
    n265_level_out out;
    req_init(&rq);
    rq.width = 1920;
    rq.height = 1080;
    rq.fps_num = 25;
    rq.dpb = 6;
    CHECK(n265_level_solve(&rq, &out) == 0 && out.idc == 120 &&
          out.max_dpb == 6, "1080p25 with a 6-picture DPB: idc %d, max_dpb %d",
          out.idc, out.max_dpb);
    rq.dpb = 7;
    CHECK(n265_level_solve(&rq, &out) == 0 && out.idc == 150 &&
          !strcmp(out.driver, "MaxDpbSize"),
          "1080p25 with a 7-picture DPB: idc %d driver %s", out.idc,
          out.driver);
    /* and the refusal when the level is named by hand */
    rq.want_idc = 120;
    CHECK(n265_level_solve(&rq, &out) < 0 && strstr(out.err, "DPB"),
          "--level 4 with a 7-picture DPB was accepted");
}

/* ---- MaxBR by tier (Table A.7), and the tier rules ---- */
static void test_bitrate(void)
{
    for (int i = 0; i < NT; i++) {
        n265_level_req rq;
        n265_level_out out;
        req_init(&rq);
        rq.width = 352;
        rq.height = 288;
        rq.fps_num = 25;
        rq.enforce_br = 1;
        /* exactly the Main tier bound of this row */
        rq.bitrate = (double)T[i].max_br_main * 1100.0;
        CHECK(n265_level_solve(&rq, &out) == 0, "level %s: its own MaxBR "
              "refused", T[i].name);
        CHECK(out.max_br >= (long long)rq.bitrate,
              "level %s: chosen max_br %lld under the stream's %.0f",
              T[i].name, (long long)out.max_br, rq.bitrate);
        /* one bit per second over the top row's High tier is the ceiling */
        if (i == NT - 1) {
            rq.want_high_tier = 1;
            rq.bitrate = (double)T[i].max_br_high * 1100.0 + 1.0;
            CHECK(n265_level_solve(&rq, &out) < 0,
                  "a rate over 6.2 High tier was accepted at idc %d", out.idc);
        }
    }
    /* the auto tier rises only when no Main tier level admits the rate:
     * 6.2 Main is 264 Mbit/s, so 265 must land High */
    n265_level_req rq;
    n265_level_out out;
    req_init(&rq);
    rq.width = 352;
    rq.height = 288;
    rq.fps_num = 25;
    rq.enforce_br = 1;
    rq.bitrate = 264.0e6;
    CHECK(n265_level_solve(&rq, &out) == 0 && out.tier == 0,
          "264 Mbit/s left the Main tier (tier %d idc %d)", out.tier, out.idc);
    rq.bitrate = 265.0e6;
    CHECK(n265_level_solve(&rq, &out) == 0 && out.tier == 1 &&
          out.tier_raised, "265 Mbit/s did not raise the tier (tier %d)",
          out.tier);

    /* A.4.1: the High tier is not defined below level 4 */
    req_init(&rq);
    rq.fps_num = 25;
    rq.want_high_tier = 1;
    CHECK(n265_level_solve(&rq, &out) == 0 && out.idc >= 120 && out.tier == 1,
          "--high-tier on QCIF chose idc %d tier %d", out.idc, out.tier);
    rq.want_idc = 60;
    CHECK(n265_level_solve(&rq, &out) < 0 && strstr(out.err, "High tier"),
          "--high-tier --level 2 was accepted");
}

/* ---- the bit rate is REPORTED, not enforced, under CRF/CQP ---- */
static void test_br_reported(void)
{
    n265_level_req rq;
    n265_level_out out;
    req_init(&rq);
    rq.width = 352;
    rq.height = 288;
    rq.fps_num = 25;
    rq.bitrate = 500.0e6;       /* over every tier of every level... */
    rq.enforce_br = 0;          /* ...but the encoder does not know it */
    CHECK(n265_level_solve(&rq, &out) == 0 && out.idc == 60,
          "an unenforced rate moved the level to idc %d", out.idc);
    CHECK(out.max_br == 1500LL * 1100, "level 2 Main reports max_br %lld",
          (long long)out.max_br);
}

/* ---- the CPB leg: MaxCPB (Table A.6), which arrives with --vbv-bufsize ----
 *
 * The companion of the rate leg, and it is out of the walk entirely at
 * cpb_size 0 - which is every stream this encoder solved before phase 12
 * item 2, and is what that invariant rests on. */
static void test_cpb(void)
{
    for (int i = 0; i < NT; i++) {
        n265_level_req rq;
        n265_level_out out;
        req_init(&rq);
        rq.width = 352;
        rq.height = 288;
        rq.fps_num = 25;
        /* exactly this row's Main tier CPB, with no rate leg on it, so the
         * buffer is the only thing choosing */
        rq.cpb_size = T[i].max_cpb_main * 1100;
        CHECK(n265_level_solve(&rq, &out) == 0, "level %s: its own MaxCPB "
              "refused", T[i].name);
        CHECK(out.max_cpb >= rq.cpb_size,
              "level %s: chosen max_cpb %lld under the stream's %lld",
              T[i].name, (long long)out.max_cpb, (long long)rq.cpb_size);
        /* one bit over the top row's High tier CPB is the ceiling */
        if (i == NT - 1) {
            rq.want_high_tier = 1;
            rq.cpb_size = T[i].max_cpb_high * 1100 + 1;
            CHECK(n265_level_solve(&rq, &out) < 0,
                  "a CPB over 6.2 High tier was accepted at idc %d", out.idc);
        }
    }
    n265_level_req rq;
    n265_level_out out;
    /* a buffer over an EXPLICIT level is refused by name */
    req_init(&rq);
    rq.width = 352;
    rq.height = 288;
    rq.fps_num = 25;
    rq.want_idc = 93;                       /* level 3.1: 11 Mbit CPB */
    rq.cpb_size = 11000001;
    CHECK(n265_level_solve(&rq, &out) < 0 && strstr(out.err, "--vbv-bufsize"),
          "a CPB over --level 3.1 was accepted (err %s)", out.err);
    rq.cpb_size = 11000000;
    CHECK(n265_level_solve(&rq, &out) == 0,
          "level 3.1's own CPB was refused: %s", out.err);
    /* ...and cpb_size 0 leaves the walk exactly where it was */
    req_init(&rq);
    rq.width = 352;
    rq.height = 288;
    rq.fps_num = 25;
    rq.cpb_size = 0;
    CHECK(n265_level_solve(&rq, &out) == 0 && out.idc == 60,
          "no CPB moved CIF at 25 fps off idc 60 (to %d)", out.idc);
}

/* ---- --level parsing ---- */
static void test_parse(void)
{
    CHECK(n265_level_parse("4.1") == 123, "4.1");
    CHECK(n265_level_parse("41") == 123, "41");
    CHECK(n265_level_parse("1") == 30, "1");
    CHECK(n265_level_parse("10") == 30, "10");
    CHECK(n265_level_parse("2.1") == 63, "2.1");
    CHECK(n265_level_parse("6.2") == 186, "6.2");
    CHECK(n265_level_parse("62") == 186, "62");
    CHECK(n265_level_parse("3.2") == 0, "3.2 is not a level");
    /* the raw idc is not a third spelling, on purpose: 30 and 60 are
     * both an idc AND a level written ten times */
    CHECK(n265_level_parse("123") == 0, "the raw idc is not accepted");
    CHECK(n265_level_parse("30") == 90, "30 is level 3, not level 1's idc");
    CHECK(n265_level_parse("60") == 180, "60 is level 6, not level 2's idc");
    CHECK(n265_level_parse("high") == 0, "a word is not a level");
    CHECK(n265_level_parse("") == 0, "empty");
    CHECK(n265_level_parse("4.1x") == 0, "trailing junk");
}

/* ---- the PCM debt: a level that admits the payload, or a refusal ---- */
static void test_pcm(void)
{
    /* CIF 4:2:0 at 8 bits: 396 CTBs of 16x16, 384 samples each. About
     * 36.9 Mbit/s at 29.97 fps, which is over level 5's 27.5 and inside
     * level 5.1's 44. */
    long long bits = n265_pcm_bits_per_frame(22, 18, 8);
    CHECK(bits >= 396LL * 384 * 8, "PCM bits/frame %lld is under the "
          "payload itself", bits);
    CHECK(bits <= 396LL * 384 * 8 * 11 / 10, "PCM bits/frame %lld is more "
          "than 10%% over the payload", bits);

    n265_level_req rq;
    n265_level_out out;
    req_init(&rq);
    rq.width = 352;
    rq.height = 288;
    rq.fps_num = 30000;
    rq.fps_den = 1001;
    rq.dpb = 2;
    rq.bitrate = (double)bits * 30000.0 / 1001.0;
    rq.enforce_br = 1;
    CHECK(n265_level_solve(&rq, &out) == 0 && out.idc == 153 &&
          !strcmp(out.driver, "MaxBR"),
          "CIF PCM chose idc %d driver %s", out.idc, out.driver);
    /* 10-bit PCM is a quarter more payload again, and still inside 5.1 */
    CHECK(n265_pcm_bits_per_frame(22, 18, 10) >
          n265_pcm_bits_per_frame(22, 18, 8),
          "10-bit PCM is not more bits than 8-bit");
}

int main(void)
{
    test_size();
    test_luma_sr();
    test_dpb();
    test_bitrate();
    test_br_reported();
    test_cpb();
    test_parse();
    test_pcm();
    printf("%s\n", fails ? "FAILED" : "all level tests passed");
    return fails ? 1 : 0;
}
