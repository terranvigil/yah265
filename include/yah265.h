/*
 * yah265: an HEVC/H.265 encoder built from scratch.
 * Copyright (c) 2026, the yah265 authors. GPL-2.0-or-later; see LICENSE.
 *
 * Public API. The shape follows the conventions H.26x encoder libraries
 * share (a params struct, pictures in, NAL units out); every line here is
 * original to this project. C99-compatible on purpose.
 */

#ifndef YAH265_H
#define YAH265_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Everything this header declares is exported; everything else in the
 * library is not. The shared build compiles with hidden visibility so
 * internal calls stay direct rather than routing through the export
 * table. */
#if defined(__GNUC__) || defined(__clang__)
#  define YAH265_API __attribute__((visibility("default")))
#else
#  define YAH265_API
#endif

#define YAH265_VERSION_MAJOR 0
#define YAH265_VERSION_MINOR 0
#define YAH265_VERSION_PATCH 1

/* The sample width this library was built for: 8 (the default) or 10.
 * There is one library per depth - meson's `bit_depth` option defines
 * this project-wide and suffixes the 10-bit library and binary
 * (`n265-10`) - so a caller includes this header once per library it
 * links, and the picture it hands in carries samples of that width.
 * Nothing here is defined when a consumer builds against the installed
 * 8-bit header, which is why 8 is the default. */
#ifndef N265_BIT_DEPTH
#define N265_BIT_DEPTH 8
#endif

#if N265_BIT_DEPTH == 8
typedef uint8_t n265_pixel;
#elif N265_BIT_DEPTH == 10
typedef uint16_t n265_pixel;
#else
#error "N265_BIT_DEPTH must be 8 or 10"
#endif

/* The per-depth symbol namespace (phase 8 item 5a). Since one command
 * line links BOTH libraries - n265 picks the encoder from the input's
 * bit depth - the two cannot both export `n265_encoder_open`, so the
 * Main10 library's public names carry a `_10` suffix.
 *
 * An external consumer sees NO change: it writes `n265_encoder_open`
 * either way, and this header renames the call for it according to the
 * N265_BIT_DEPTH it compiled with (nothing, for the installed 8-bit
 * header). A consumer that wants BOTH libraries in one program is in
 * n265's position and declares the suffixed entry points itself; that
 * is what N265_API is published for.
 *
 * The library's internal symbols are namespaced the same way, by a
 * force-included header that is not installed. */
#define N265_API_PASTE(a, b) a ## b
#define N265_API_EVAL(a, b)  N265_API_PASTE(a, b)
#if N265_BIT_DEPTH == 8
#  define N265_API(name) name
#else
#  define N265_API(name) N265_API_EVAL(name, N265_API_EVAL(_, N265_BIT_DEPTH))
#endif

#if N265_BIT_DEPTH != 8
#define n265_param_default    N265_API(n265_param_default)
#define n265_set_mode         N265_API(n265_set_mode)
#define n265_set_la           N265_API(n265_set_la)
#define n265_set_crf          N265_API(n265_set_crf)
#define n265_set_bitrate      N265_API(n265_set_bitrate)
#define n265_set_ref          N265_API(n265_set_ref)
#define n265_set_badapt       N265_API(n265_set_badapt)
#define n265_set_threads      N265_API(n265_set_threads)
#define n265_set_hash         N265_API(n265_set_hash)
#define n265_set_weight       N265_API(n265_set_weight)
#define n265_set_vui_colour   N265_API(n265_set_vui_colour)
#define n265_set_vui_sar      N265_API(n265_set_vui_sar)
#define n265_set_vui_window   N265_API(n265_set_vui_window)
#define n265_set_hdr_meta     N265_API(n265_set_hdr_meta)
#define n265_set_hdr10        N265_API(n265_set_hdr10)
#define n265_set_preset       N265_API(n265_set_preset)
#define n265_preset_param     N265_API(n265_preset_param)
#define n265_preset_nearest   N265_API(n265_preset_nearest)
#define n265_encoder_open     N265_API(n265_encoder_open)
#define n265_encoder_encode   N265_API(n265_encoder_encode)
#define n265_encoder_close    N265_API(n265_encoder_close)
#define n265_version_str      N265_API(n265_version_str)
#endif

/* Chroma sampling of input pictures. The value IS chroma_format_idc
 * (7.4.3.2), so the SPS field and the plane geometry read one number: 1 is
 * 4:2:0, 2 is 4:2:2 and 3 is 4:4:4. It is a RUNTIME property of the
 * encoder and not a build option - unlike the sample width, the chroma
 * format does not change the pixel type, so one library codes all three.
 *
 * N265_CSP_I420 stays 0 as well, because it was the old enum's only value
 * and a caller that memsets its n265_param must keep getting 4:2:0. */
enum {
    N265_CSP_I420 = 0,
    N265_CSP_I422 = 2,
    N265_CSP_I444 = 3,
};

typedef struct n265_param {
    int width;      /* luma width in samples; must be even  */
    int height;     /* luma height in samples; must be even */
    int csp;        /* N265_CSP_*                            */
    int fps_num;    /* informational for now                 */
    int fps_den;
} n265_param;

/* NAL unit types we emit (subset of ITU-T H.265 Table 7-1). VCL types
 * are < 32; a caller splitting output into coded frames can cut after
 * each VCL unit. */
enum {
    N265_NAL_TRAIL_N    = 0,
    N265_NAL_TRAIL_R    = 1,
    N265_NAL_IDR_W_RADL = 19,
    N265_NAL_VPS        = 32,
    N265_NAL_SPS        = 33,
    N265_NAL_PPS        = 34,
    N265_NAL_PREFIX_SEI = 39,
    N265_NAL_SUFFIX_SEI = 40,
};

typedef struct n265_nal {
    int type;               /* nal_unit_type                            */
    const uint8_t *payload; /* Annex-B: start code + EP'd payload       */
    int size;               /* bytes, start code included               */
} n265_nal;

/* The planes are `void *` and not `n265_pixel *` so that ONE picture
 * struct serves both libraries: the sample width is the library's, not
 * the header's, and a caller that links both (n265 does) hands 8-bit
 * planes to one and 16-bit planes to the other through the same type.
 * A caller that links one library writes `n265_pixel *` as before and
 * the conversion to void * is implicit in C. */
typedef struct n265_picture {
    const void *plane[3];   /* n265_pixel samples, the library's width */
    int stride[3];          /* in SAMPLES, not bytes */
    int64_t pts;
} n265_picture;

typedef struct n265_encoder n265_encoder;

/* Fill defaults; returns 0 on success. */
YAH265_API void n265_param_default(n265_param *p);

/* v0 configuration, set before n265_encoder_open (process-global; one
 * encoder per process for now): mode 0 normal / 1 PCM, CQP qp, CTB size
 * as log2 (4..6), keyframe interval, consecutive B frames (0/1),
 * variance AQ on/off; lookahead depth in frames and cu-tree on/off.
 * Encode latency is `lookahead depth + bframes` frames. */
YAH265_API void n265_set_mode(int mode, int qp, int ctb_log2, int keyint,
                               int bframes, int aq);
YAH265_API void n265_set_la(int depth, int cutree);
/* Constant rate factor: the per-frame base QP comes from `crf` (plus the
 * frame-type ladder, the cu-tree operating-point shift and the AQ frame-mean
 * term) instead of the fixed QP. Open loop: no bits feedback.
 *
 * CALLING IT AT ALL turns CRF on, at any value (phase 8 item 5a). It used
 * to be gated on `crf >= 0`, which made the twelve QP steps below zero
 * that Main10 pays for unreachable through CRF; the rate factor is a QpY
 * and QpY runs from -QpBdOffsetY, so a negative one is a legal operating
 * point and the RC clamps it to that floor like any other. CQP is what
 * you get by NOT calling this. */
YAH265_API void n265_set_crf(double crf);
/* Average bitrate, in kbit/s: single-pass ABR. The base QP per frame comes
 * from a rate factor the encoder solves from the bits it has spent against
 * the bits it planned, with the lookahead's per-frame complexity and
 * online per-frame-type scales deciding the split. Overrides crf. 0 turns
 * it off (the default). */
YAH265_API void n265_set_bitrate(double kbps);
/* references per list (1..3), P and B alike; more references cost search time. */
YAH265_API void n265_set_ref(int n);
/* adaptive B run length: with bframes > 1, shorten the run on high-motion
 * spans (measured from the lookahead) instead of always coding the maximum. */
YAH265_API void n265_set_badapt(int on);

/* Worker threads. 0 = one per online CPU. Output is byte-identical at
 * every value (docs/threading-design.md). */
YAH265_API void n265_set_threads(int n);

/* ---- pass-through signalling (phase 14 item 1) ----
 *
 * Six setters, and NONE of them changes a coded sample: they fill in the
 * VUI's colour description, the sample aspect ratio, the default display
 * window, the HDR10 static metadata SEIs and the decoded picture hash.
 * Give none of them and the stream is byte for byte the one this encoder
 * wrote before they existed.
 *
 * Every argument is already the SYNTAX ELEMENT of ITU-T H.265 Annex E or
 * Annex D - a colour primary is Table E.3's number, not a name - so a
 * caller that has a name parses it itself or uses n265 the binary, which
 * accepts x265's spellings. -1 means "not given" in every field that has
 * an absence, and an absent field's presence flag is 0.
 *
 * The decoded picture hash is one suffix SEI per picture, D.2.19, taken
 * over the CODED picture: 0 off, then x265's numbering - 1 MD5, 2 CRC,
 * 3 checksum. It costs a pass over the reconstruction at retirement. */
/* ---- weighted prediction (phase 14 item 2) ----
 *
 * weighted_pred_flag and weighted_bipred_flag, and with them the whole
 * tool: the encoder derives one explicit weight per (list, reference)
 * from the lookahead and writes a pred_weight_table where it beats the
 * unweighted prediction. -1 in either argument leaves that half at its
 * default, which is the preset's - x265 ships weightp on at medium and
 * weightb off, and so does this. The P half is the one that costs a
 * default; the B half is priced and off. */
YAH265_API void n265_set_weight(int weightp, int weightb);

YAH265_API void n265_set_hash(int type);
YAH265_API void n265_set_vui_colour(int videoformat, int full_range,
                                     int colorprim, int transfer,
                                     int colormatrix, int chromaloc,
                                     int overscan);
/* aspect_ratio_idc, and the pair that follows it when it is 255
 * (EXTENDED_SAR). -1 writes no aspect ratio at all. */
YAH265_API void n265_set_vui_sar(int idc, int w, int h);
/* the default display window, in LUMA samples; the writer converts to the
 * chroma units the syntax is in. */
YAH265_API void n265_set_vui_window(int on, int l, int t, int r, int b);
/* `md` is ten values in x265's own --master-display order: G(x,y) B(x,y)
 * R(x,y) WP(x,y) L(max,min), chromaticities in units of 0.00002 and
 * luminances in 0.0001 cd/m2. NULL writes no mastering display; cll < 0
 * writes no content light level. */
YAH265_API void n265_set_hdr_meta(const uint32_t *md, int cll, int fall);
/* hdr10: 1 forces the HDR10 messages on, 0 off, -1 leaves x265's rule
 * (on iff either message has values). cll_on is the content light level
 * message alone. atc is the alternative transfer characteristic (D.2.38),
 * -1 for no message. */
YAH265_API void n265_set_hdr10(int hdr10, int cll_on, int atc);

/* Speed/quality preset: "fast", "medium" (the default), "slow" or
 * "slower". Returns 0, or -1 for a name it does not know, in which case
 * nothing changes. A preset only supplies DEFAULTS: it is read where a
 * knob's environment variable is unset and where the caller has set no
 * explicit value, so an explicit configuration always wins. medium is
 * the shipped encoder byte for byte.
 *
 * The ladder shifted by role on 2026-09-13: "medium" is the point the
 * project's goals are read at - at or ahead of x265 medium on every
 * corpus clip, in less time - "slow" is what "medium" named before that
 * date, and "slower" is what "slow" named.
 *
 * Structural parameters (coding tree size, references, B frames,
 * adaptive B, lookahead depth) belong to the caller, not to the library,
 * so the preset only offers a value for them: ask with
 * n265_preset_param() and pass what comes back to the setter if the user
 * gave no flag of their own. Keys: "ctu", "ref", "bframes", "b-adapt",
 * "rc-lookahead". */
YAH265_API int n265_set_preset(const char *name);
YAH265_API int n265_preset_param(const char *key, int fallback);

/* The PLACED rung nearest `name` on x265's ten-name ladder, or NULL when
 * `name` is not one of x265's at all. n265_set_preset returns -2 rather
 * than -1 for a name this can answer, so a front end can refuse an
 * unplaced rung by pointing at the nearest one we have. */
YAH265_API const char *n265_preset_nearest(const char *name);

/* NULL on invalid params or allocation failure. */
YAH265_API n265_encoder *n265_encoder_open(const n265_param *p);

/* Encode one picture. On success returns the number of bytes of Annex-B
 * output produced and sets nal/nnal to an array valid until the next
 * call on this encoder. pic == NULL flushes (no-op for now: zero-latency
 * all-intra). Returns < 0 on error, 0 when flushing with nothing left. */
YAH265_API int n265_encoder_encode(n265_encoder *e, n265_nal **nal,
                                    int *nnal, const n265_picture *pic);

YAH265_API void n265_encoder_close(n265_encoder *e);

YAH265_API const char *n265_version_str(void);

#ifdef __cplusplus
}
#endif

#endif /* YAH265_H */
