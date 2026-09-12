/*
 * Annex A: the level and tier a stream actually needs.
 * Copyright (c) 2026, the yah265 authors. GPL-2.0-or-later; see LICENSE.
 *
 * Until phase 8 item 7 the level was picked from the picture size and the
 * declared DPB alone (headers.c's pick_level_idc, eight of the thirteen
 * levels, no rate leg at all). Two of the four general constraints were
 * therefore never checked, and both of them bite on real content:
 *
 *   - MaxLumaSr (Table A.7). 1280x720 at 50 fps is 46 080 000 samples per
 *     second against level 3.1's 33 177 600, so every 720p50 clip in the
 *     corpus was signalling a level its OWN FRAME RATE violates. The
 *     picture fits; the rate does not.
 *   - MaxBR (Table A.7). A phase-0 I_PCM stream carries about 30 Mbit/s at
 *     CIF, and level 2.1 main tier admits 3.3. That is the "PCM exceeds
 *     every tier's MaxBR by construction" line in docs/ideas.md, and it
 *     was true only because nothing consulted the table: level 5.1 admits
 *     it, and 5.1 is what the stream says now.
 *
 * The tables below are Table A.6 (general tier and level limits) and
 * Table A.7 (the Main / Main 10 tier and level limits), in the spec's own
 * units. A.4.1's general constraints are `fits()`; A.4.2's MaxDpbSize is
 * max_dpb_size(). Nothing here reads a sample, so it is all decided at
 * encoder open, which is where a refusal can still be useful.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "encoder/level.h"

/* Table A.2, general_profile_idc 1 (Main) and 2 (Main 10): CpbBrVclFactor
 * 1000, CpbBrNalFactor 1100. What this encoder emits is a NAL unit
 * stream, so the NAL factor is the one that scales MaxBR and MaxCPB out
 * of the tables' thousand-bit units and into bits. The two factors are
 * equal at Main and Main10 (headers.c says the same), so the depth does
 * not reach this file. */
#define CPB_BR_NAL_FACTOR 1100

/* Table A.6 (MaxLumaPs, MaxCPB, MaxSliceSegmentsPerPicture, MaxTileRows,
 * MaxTileCols) joined with Table A.7 (MaxLumaSr, MaxBR) on the level.
 * MaxCPB and MaxBR are in units of 1000 bits and 1000 bits/s before
 * CPB_BR_NAL_FACTOR; a 0 in a High tier column is the tier not being
 * DEFINED at that level (A.4.1: general_tier_flag shall be 0 below
 * level 4), not a limit of zero. */
static const struct level_row {
    int idc;                    /* general_level_idc = level * 30 */
    const char *name;
    int64_t max_luma_ps;
    int64_t max_luma_sr;
    int64_t max_br_main, max_br_high;
    int64_t max_cpb_main, max_cpb_high;
    int max_slice_segments;
    int max_tile_rows, max_tile_cols;
} levels[] = {
    {  30, "1",      36864,        552960,    128,      0,    350,      0,  16,  1,  1 },
    {  60, "2",     122880,       3686400,   1500,      0,   1500,      0,  16,  1,  1 },
    {  63, "2.1",   245760,       7372800,   3000,      0,   3000,      0,  20,  1,  1 },
    {  90, "3",     552960,      16588800,   6000,      0,   6000,      0,  30,  2,  2 },
    {  93, "3.1",   983040,      33177600,  10000,      0,  10000,      0,  40,  3,  3 },
    { 120, "4",    2228224,      66846720,  12000,  30000,  12000,  30000,  75,  5,  5 },
    { 123, "4.1",  2228224,     133693440,  20000,  50000,  20000,  50000,  75,  5,  5 },
    { 150, "5",    8912896,     267386880,  25000, 100000,  25000, 100000, 200, 11, 10 },
    { 153, "5.1",  8912896,     534773760,  40000, 160000,  40000, 160000, 200, 11, 10 },
    { 156, "5.2",  8912896,    1069547520,  60000, 240000,  60000, 240000, 200, 11, 10 },
    { 180, "6",   35651584,    1069547520,  60000, 240000,  60000, 240000, 600, 22, 20 },
    { 183, "6.1", 35651584,    2139095040, 120000, 480000, 120000, 480000, 600, 22, 20 },
    { 186, "6.2", 35651584,    4278190080, 240000, 800000, 240000, 800000, 600, 22, 20 },
};
#define NLEVELS ((int)(sizeof(levels) / sizeof(levels[0])))

/* MaxDpbSize (A.4.2) with maxDpbPicBuf = 6. A picture well below the
 * level's MaxLumaPs is granted more picture storage buffers, which is how
 * a deep pyramid pays for itself: the level rises until the DPB the
 * reference structure needs fits inside it. Unmoved from headers.c. */
static int max_dpb_size(int64_t ps, int64_t max_luma_ps)
{
    if (ps <= max_luma_ps >> 2)
        return 16;
    if (ps <= max_luma_ps >> 1)
        return 12;
    if (ps <= (max_luma_ps * 3) >> 2)
        return 8;
    return 6;
}

static int64_t max_br_bits(const struct level_row *L, int tier)
{
    int64_t v = tier ? L->max_br_high : L->max_br_main;
    return v * CPB_BR_NAL_FACTOR;
}

static int64_t max_cpb_bits(const struct level_row *L, int tier)
{
    int64_t v = tier ? L->max_cpb_high : L->max_cpb_main;
    return v * CPB_BR_NAL_FACTOR;
}

/* The stream's luma sample rate, rounded UP: a level that admits the rate
 * only after the division truncates does not admit it. */
static int64_t luma_sample_rate(int64_t ps, int fps_num, int fps_den)
{
    if (fps_num <= 0 || fps_den <= 0)
        return 0;               /* no rate known: the MaxLumaSr leg is moot */
    return (ps * fps_num + fps_den - 1) / fps_den;
}

/* A.4.1's general constraints plus A.4.2's DPB and bit rate, at one level
 * and one tier. Returns 1 when the level admits the stream; otherwise 0
 * with *why naming the bound that refused it (the caller turns the FIRST
 * failure of the level below the winner into the trace's `driver`). */
static int fits(const struct level_row *L, int tier,
                const n265_level_req *rq, int64_t ps, int64_t sr,
                const char **why)
{
    if (tier && !L->max_br_high) {
        *why = "tier";          /* High tier is not defined below level 4 */
        return 0;
    }
    if (ps > L->max_luma_ps) {
        *why = "MaxLumaPs";
        return 0;
    }
    /* the per-dimension bound, Sqrt(MaxLumaPs * 8), squared so it stays
     * in integers */
    if ((int64_t)rq->width * rq->width > L->max_luma_ps * 8) {
        *why = "width";
        return 0;
    }
    if ((int64_t)rq->height * rq->height > L->max_luma_ps * 8) {
        *why = "height";
        return 0;
    }
    if (rq->dpb > max_dpb_size(ps, L->max_luma_ps)) {
        *why = "MaxDpbSize";
        return 0;
    }
    if (sr > L->max_luma_sr) {
        *why = "MaxLumaSr";
        return 0;
    }
    if (rq->slice_segments > L->max_slice_segments) {
        *why = "MaxSliceSegmentsPerPicture";
        return 0;
    }
    if (rq->tile_rows > L->max_tile_rows) {
        *why = "MaxTileRows";
        return 0;
    }
    if (rq->tile_cols > L->max_tile_cols) {
        *why = "MaxTileCols";
        return 0;
    }
    /* The rate leg is a constraint only where the encoder KNOWS the rate:
     * an ABR target, or an I_PCM payload that is fixed by the geometry.
     * Under CRF and CQP the rate is whatever the content costs and no
     * level can be picked from it, so it is reported (the trace's
     * max_br) and not enforced - which is what the help says. */
    if (rq->enforce_br && rq->bitrate > 0.0 &&
        rq->bitrate > (double)max_br_bits(L, tier)) {
        *why = "MaxBR";
        return 0;
    }
    *why = NULL;
    return 1;
}

static void fill(n265_level_out *out, const struct level_row *L, int tier,
                 int64_t ps, int64_t sr)
{
    out->idc = L->idc;
    out->tier = tier;
    out->name = L->name;
    out->ps = ps;
    out->luma_sr = sr;
    out->max_luma_ps = L->max_luma_ps;
    out->max_luma_sr = L->max_luma_sr;
    out->max_br = max_br_bits(L, tier);
    out->max_cpb = max_cpb_bits(L, tier);
    out->max_dpb = max_dpb_size(ps, L->max_luma_ps);
}

int n265_level_solve(const n265_level_req *rq, n265_level_out *out)
{
    memset(out, 0, sizeof(*out));
    out->driver = "minimum";
    int64_t ps = (int64_t)rq->width * rq->height;
    int64_t sr = luma_sample_rate(ps, rq->fps_num, rq->fps_den);

    if (rq->want_idc) {
        const struct level_row *L = NULL;
        for (int i = 0; i < NLEVELS; i++)
            if (levels[i].idc == rq->want_idc)
                L = &levels[i];
        if (!L) {
            snprintf(out->err, sizeof(out->err),
                     "--level: %d is not a level of Annex A (1, 2, 2.1, 3, "
                     "3.1, 4, 4.1, 5, 5.1, 5.2, 6, 6.1, 6.2)", rq->want_idc);
            return -1;
        }
        int tier = rq->want_high_tier;
        const char *why = NULL;
        if (!fits(L, tier, rq, ps, sr, &why)) {
            /* Name the bound AND the number, because "level 3.1 is too
             * low" without the frame rate is a refusal nobody can act
             * on. */
            fill(out, L, tier, ps, sr);
            if (!strcmp(why, "tier"))
                snprintf(out->err, sizeof(out->err),
                         "--high-tier at level %s: A.4.1 defines the High "
                         "tier only from level 4 up, so general_tier_flag "
                         "must be 0 here", L->name);
            else if (!strcmp(why, "MaxLumaPs"))
                snprintf(out->err, sizeof(out->err),
                         "--level %s admits %lld luma samples per picture "
                         "(Table A.6) and this one is %dx%d = %lld",
                         L->name, (long long)L->max_luma_ps,
                         rq->width, rq->height, (long long)ps);
            else if (!strcmp(why, "width") || !strcmp(why, "height"))
                snprintf(out->err, sizeof(out->err),
                         "--level %s bounds each picture dimension by "
                         "Sqrt(MaxLumaPs * 8) and %dx%d is over it",
                         L->name, rq->width, rq->height);
            else if (!strcmp(why, "MaxDpbSize"))
                snprintf(out->err, sizeof(out->err),
                         "--level %s allows a %d-picture DPB at this picture "
                         "size (A.4.2) and the reference structure declares "
                         "%d; lower --bframes or --ref, or raise the level",
                         L->name, max_dpb_size(ps, L->max_luma_ps), rq->dpb);
            else if (!strcmp(why, "MaxLumaSr"))
                snprintf(out->err, sizeof(out->err),
                         "--level %s admits %lld luma samples per second "
                         "(Table A.7) and %dx%d at %g fps is %lld",
                         L->name, (long long)L->max_luma_sr,
                         rq->width, rq->height,
                         rq->fps_den ? (double)rq->fps_num / rq->fps_den : 0.0,
                         (long long)sr);
            else if (!strcmp(why, "MaxBR"))
                snprintf(out->err, sizeof(out->err),
                         "--level %s %s tier admits %lld bit/s (Table A.7 x "
                         "CpbBrNalFactor 1100) and this stream carries %.0f",
                         L->name, tier ? "High" : "Main",
                         (long long)max_br_bits(L, tier), rq->bitrate);
            else
                snprintf(out->err, sizeof(out->err),
                         "--level %s refuses this stream on %s", L->name, why);
            return -1;
        }
        fill(out, L, tier, ps, sr);
        out->driver = "--level";
        return 0;
    }

    /* Auto. Walk the table upward at the requested tier and take the
     * first level that admits everything; the bound the level BELOW the
     * winner failed on is what drove the choice, and is what the trace
     * reports. The High tier is only entered on request, or - last
     * resort - when no Main tier level admits the rate. */
    int tier = rq->want_high_tier ? 1 : 0;
    for (int pass = 0; pass < 2; pass++) {
        const char *prev_why = "minimum";
        for (int i = 0; i < NLEVELS; i++) {
            const char *why = NULL;
            if (fits(&levels[i], tier, rq, ps, sr, &why)) {
                fill(out, &levels[i], tier, ps, sr);
                out->driver = prev_why;
                out->tier_raised = pass == 1;
                return 0;
            }
            /* `why` may be "tier", which is not a bound the stream
             * failed but a level at which the requested tier does not
             * exist. Reported as the driver all the same: under
             * --high-tier on a small picture, the tier IS what took the
             * level to 4. */
            prev_why = why;
        }
        if (tier)
            break;              /* already at High: nothing left to raise */
        tier = 1;               /* Main tier ran out; A.4.1 allows High
                                 * from level 4, and its MaxBR is 3.3x */
    }
    snprintf(out->err, sizeof(out->err),
             "no Annex A level admits %dx%d at %g fps with a %d-picture DPB"
             "%s: level 6.2 High tier is the ceiling",
             rq->width, rq->height,
             rq->fps_den ? (double)rq->fps_num / rq->fps_den : 0.0, rq->dpb,
             rq->enforce_br && rq->bitrate > 0.0 ? " and this bit rate" : "");
    return -1;
}

int n265_level_parse(const char *s)
{
    if (!s || !*s)
        return 0;
    char *end;
    double v = strtod(s, &end);
    if (end == s || *end)
        return 0;
    /* The two spellings in the wild, "4.1" and "41", and nothing else.
     * The raw idc is DELIBERATELY not a third: 30 is level 1's idc and
     * also level 3 written ten times, and 60 is level 2's idc and also
     * level 6, so a parser that took all three would resolve half the
     * table by a rule the user cannot see. The idc is an output (the
     * trace prints it, ffprobe prints it), not an input. */
    int ten = (int)(v < 10.0 ? v * 10.0 + 0.5 : v + 0.5);
    for (int i = 0; i < NLEVELS; i++) {
        /* the row's own x10 name, derived rather than stored: "5.1" is
         * 51 and "5" is 50 */
        int major = levels[i].name[0] - '0';
        int minor = levels[i].name[1] == '.' ? levels[i].name[2] - '0' : 0;
        if (ten == major * 10 + minor)
            return levels[i].idc;
    }
    return 0;
}

int64_t n265_pcm_bits_per_frame(int ctb_cols, int ctb_rows, int depth)
{
    /* In PCM mode the CTB is 16x16 and min CB is 16, so every CTB is
     * exactly one I_PCM CU: 256 luma plus 2 x 64 chroma samples of
     * pcm_sample() at the coded depth. The 32 bits on top are the CU
     * syntax, the pcm_flag and the pcm_alignment_zero_bits that precede
     * the payload, and the 4096 per picture are the slice header and the
     * parameter sets. Measured at CIF 8-bit the true cost is 16.6 bits
     * per CTB over the payload, so this is an upper bound by about 1% -
     * which is the direction a level claim has to err in. */
    return (int64_t)ctb_cols * ctb_rows * (384 * depth + 32) + 4096;
}
