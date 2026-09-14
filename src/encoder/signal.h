/*
 * Pass-through signalling: the VUI's colour description, the sample
 * aspect ratio, the default display window, and the HDR10 static
 * metadata SEIs. Phase 14 item 1.
 * Copyright (c) 2026, the yah265 authors. GPL-2.0-or-later; see LICENSE.
 *
 * Everything here is SIGNALLING and nothing here is a decision. No field
 * below reaches a quantiser, a mode, a lambda or a rate, and none of them
 * changes a single coded sample: a stream encoded with the whole set
 * given carries the same VCL NALs as one encoded without it, with
 * different bytes in the SPS and a couple of SEI messages in front. That
 * is the property the item's identity gate reads, and it is why these
 * flags are safe to add to a measured encoder mid-campaign.
 *
 * The VALUES are the syntax elements of E.2.1 and D.2.28 / D.2.35, not
 * names: the CLI parses x265's spellings (and the bare numbers x265 also
 * takes) into these, so the library never sees a string. -1 means the
 * field was not given, and every presence flag in the VUI is derived from
 * what was - which is the one place this differs from x265, where a
 * colour field set to its own "unspecified" value suppresses the
 * description. Asking for `--colorprim unknown` here writes the
 * description with colour_primaries 2 in it, because a user who names the
 * flag has said something.
 */

#ifndef N265_SIGNAL_H
#define N265_SIGNAL_H

#include <stdint.h>

/* The three HDR10 messages share one prefix SEI RBSP: 24 + 4 + 1 payload
 * bytes, two header bytes each and the trailing byte. 64 is triple. */
#define N265_HDR_SEI_MAX    64

typedef struct n265_signal {
    /* --hash: 0 off, else hash_type + 1 (x265's numbering: 1 MD5, 2 CRC,
     * 3 checksum). One suffix SEI per picture, D.2.19. */
    int hash;

    /* ---- vui_parameters (E.2.1). -1 is "not given" throughout. ---- */
    int sar_idc;            /* aspect_ratio_idc; 255 is EXTENDED_SAR   */
    int sar_w, sar_h;       /* ...and its explicit pair                */
    int overscan;           /* overscan_appropriate_flag, -1 absent    */
    int videoformat;        /* video_format (Table E.2)                */
    int full_range;         /* video_full_range_flag                   */
    int colorprim;          /* colour_primaries (Table E.3)            */
    int transfer;           /* transfer_characteristics (Table E.4)    */
    int colormatrix;        /* matrix_coeffs (Table E.5)               */
    int chromaloc;          /* chroma_sample_loc_type_*_field, 0..5    */
    int win_on;             /* default_display_window_flag             */
    int win_l, win_t, win_r, win_b;     /* LUMA samples; the syntax is in
                                         * SubWidthC / SubHeightC units
                                         * and the writer divides */

    /* ---- the HDR10 static metadata SEIs ---- */
    int have_md;            /* --master-display was given              */
    uint16_t md_prim[3][2]; /* display_primaries_[xy][c], G B R order,
                             * in units of 0.00002                     */
    uint16_t md_wp[2];      /* white_point_x, white_point_y            */
    uint32_t md_max, md_min;/* mastering luminance, units of 0.0001 cd/m2 */
    int have_cll;           /* --max-cll was given                     */
    int maxcll, maxfall;    /* max_content_light_level, max_pic_average */
    int hdr10;              /* --hdr10 / --no-hdr10; -1 = auto (on iff
                             * either message has values)              */
    int cll_on;             /* --cll / --no-cll: the CLL message alone  */
    int atc;                /* --atc-sei: preferred_transfer_characteristics,
                             * -1 = no message                         */
} n265_signal;

#endif
