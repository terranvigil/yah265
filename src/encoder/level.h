/*
 * Annex A: profiles, tiers and levels. The solver, not the writer -
 * src/encoder/headers.c writes general_level_idc and general_tier_flag,
 * and this is where the two numbers come from.
 * Copyright (c) 2026, the yah265 authors. GPL-2.0-or-later; see LICENSE.
 */

#ifndef N265_LEVEL_H
#define N265_LEVEL_H

#include <stdint.h>

/* What the stream is, in the terms Annex A constrains. Everything here
 * is known at encoder open: the coded picture size, the DPB the
 * reference structure declares, the source frame rate, and - under ABR
 * or PCM - the rate the stream will actually carry. */
typedef struct n265_level_req {
    int width, height;      /* pic_width/height_in_luma_samples: the CODED
                             * (padded) size, which is what A.4.1 bounds  */
    int dpb;                /* sps_max_dec_pic_buffering_minus1 + 1       */
    int fps_num, fps_den;   /* the source frame rate, from the y4m        */
    double bitrate;         /* bits/s the stream carries, 0 = not known   */
    int enforce_br;         /* 1: the rate is a CONSTRAINT (ABR, PCM, and
                             * --vbv-maxrate, which is a rate the encoder
                             * enforces whatever the mode);
                             * 0: it is reported and not enforced (CRF,
                             * CQP - the encoder does not know the rate)  */
    /* --vbv-bufsize in bits: the CPB the encode is planned against and the
     * HRD then declares, which Table A.6's MaxCPB bounds at the chosen
     * tier. 0 - no VBV - leaves the leg out, which is how every stream
     * before phase 12 item 2 was solved. */
    int64_t cpb_size;
    int slice_segments;     /* slice segments per picture (1 today)       */
    int tile_rows, tile_cols;   /* 1, 1 today: tiles_enabled_flag is 0    */
    int want_idc;           /* --level as an idc (level * 30); 0 = auto   */
    int want_high_tier;     /* --high-tier                                */
} n265_level_req;

/* The answer, and every bound that produced it - the trace prints this
 * struct and nothing else. */
typedef struct n265_level_out {
    int idc;                /* general_level_idc                          */
    int tier;               /* general_tier_flag: 0 main, 1 high          */
    const char *name;       /* "4.1"                                      */
    int64_t ps;             /* PicSizeInSamplesY                          */
    int64_t luma_sr;        /* the stream's samples per second            */
    int64_t max_luma_ps;    /* Table A.6, at the chosen level             */
    int64_t max_luma_sr;    /* Table A.7                                  */
    int64_t max_br;         /* Table A.7 x CpbBrNalFactor, bits/s, at the
                             * chosen TIER                                */
    int64_t max_cpb;        /* Table A.6 x CpbBrNalFactor, bits           */
    int max_dpb;            /* MaxDpbSize (A.4.2) at the chosen level     */
    const char *driver;     /* the bound that forced this level and not a
                             * lower one                                  */
    int tier_raised;        /* the auto tier went high to admit the rate  */
    char err[256];          /* the refusal, empty on success              */
} n265_level_out;

/* 0 on success; -1 with out->err set when no level admits the stream or
 * when an explicit --level / --high-tier cannot be honoured. */
int n265_level_solve(const n265_level_req *rq, n265_level_out *out);

/* "4.1", "41" or "123" -> 123. 0 for a string that is not a level. */
int n265_level_parse(const char *s);

/* The bits an I_PCM stream of this geometry costs per picture: every CTB
 * is one 16x16 I_PCM CU, so the payload is fixed and the level is known
 * before a frame is coded. `depth` is the sample width. */
int64_t n265_pcm_bits_per_frame(int ctb_cols, int ctb_rows, int depth);

#endif
