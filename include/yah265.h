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

/* Chroma sampling of input pictures. Only 4:2:0 8-bit for now. */
enum {
    N265_CSP_I420 = 0,
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
};

typedef struct n265_nal {
    int type;               /* nal_unit_type                            */
    const uint8_t *payload; /* Annex-B: start code + EP'd payload       */
    int size;               /* bytes, start code included               */
} n265_nal;

typedef struct n265_picture {
    const uint8_t *plane[3];
    int stride[3];          /* in bytes */
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
