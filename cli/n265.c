/*
 * n265: Y4M in, Annex-B HEVC out.
 * Copyright (c) 2026, the yah265 authors. GPL-2.0-or-later; see LICENSE.
 */

#include <errno.h>
#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#ifdef __APPLE__
#include <malloc/malloc.h>
#endif
#include <sys/types.h>
#include <unistd.h>

#include "yah265.h"
#include "hdr_dyn.h"

/* internal knobs, resolved from the library (not yet public API) */
void n265_encoder_set_dump_decode(n265_encoder *e, FILE *f);
void n265_encoder_set_dump(n265_encoder *e, FILE *f);
void n265_encoder_set_shot_stats(n265_encoder *e, const char *path);
void n265_set_shot_window(int n);
/* --psnr / --ssim / --csv (phase 14 item 4). The metrics are a read-only
 * tap at retirement, so a stream coded with them on is byte-for-byte the
 * one coded without; the pooled numbers come back through
 * n265_encoder_metrics for the end-of-run line. */
void n265_set_metrics(int psnr, int ssim);
/* --repeat-headers / --aud / --eos / --eob / --info: the five NAL-level
 * switches. Each adds or repeats a NAL and none touches a coded sample;
 * all five are off by default. */
void n265_set_nal_opts(int repeat_headers, int aud, int eos, int eob,
                       int info);
/* The options clause of that SEI, as the CLI resolved it. */
void n265_set_info_opts(const char *opts);
/* --no-fingerprint: the stream fingerprint (phase 14 item 10). ON by
 * default, like --info and for the same reason, except that this one
 * spends no bits and moves no sample at all. */
void n265_set_fingerprint(int on);
/* --show-config / --verbose (phase 14 item 13): the resolved
 * configuration, one `n265 [info]: key : value` row at a time. Printed
 * by the LIBRARY, which is where the preset table, the environment and
 * Annex A have already been resolved into the values that will code. */
void n265_show_config(const n265_encoder *e, FILE *f, int in_depth);
/* --crf-min / --crf-max / --qpmin / --qpmax / --strict-cbr /
 * --max-ausize-factor, and the two files. The sentinels are the widest
 * legal values, so "not given" costs nothing. */
void n265_set_rc_bounds(double crf_min, double crf_max, int qpmin, int qpmax,
                        int strict_cbr, double ausize_factor);
/* --idr-recovery-sei / --pic-struct / --hrd-concat / --temporal-layers.
 * pic_struct is -1 for the flag absent. */
void n265_set_sei_opts(int recovery_sei, int pic_struct, int hrd_concat,
                       int temporal_layers);
void n265_set_qpfile(const char *path);
void n265_set_zones(const char *spec);
void n265_encoder_set_csv(n265_encoder *e, const char *path, int level);
long n265_encoder_metrics(n265_encoder *e, double out[6]);
long n265_encoder_dyn_lost(const n265_encoder *e);
/* Two-pass (phase 12 item 4): 1 writes the stats sidecar, 2 plans from it,
 * 3 does both. The path outlives the encoder - it is argv. */
void n265_set_pass(int pass, const char *stats);
void n265_set_lossless(int on);
/* --rdoq-level <0|1|2> (x265's numbering; docs/instruments.md says how it
 * maps onto N265_RDOQ). -1 is "not given". */
void n265_set_rdoq_level(int lvl);
/* --tu-inter-depth / --tu-intra-depth <1..4> (x265's names and x265's
 * numbering: the number of transform-tree LEVELS a 2Nx2N CU may use,
 * which is the SPS's max_transform_hierarchy_depth_* plus one). -1 is
 * "not given", which leaves the preset - and the compiled-in 2 / 2 that
 * every stream before phase 14 item 5 carried - in charge. */
void n265_set_tu_depth(int inter, int intra);
/* --cbqpoffs / --crqpoffs <-12..12>: pps_cb_qp_offset and
 * pps_cr_qp_offset, x265's names for the two PPS chroma QP offsets.
 * 0 / 0 is the default and is every stream before phase 13 B1. */
void n265_set_chroma_qpoffs(int cb, int cr);
/* Annex A overrides (phase 8 item 7). `idc` is general_level_idc
 * (level * 30); 0 leaves the level derived from the stream. */
void n265_set_level(int idc, int high_tier);
void n265_set_vui_timing(int on);
void n265_set_film_grain(double strength, int denoise);
/* --nr-intra / --nr-inter (phase 14 item 6): the coefficient-domain
 * noise reduction, 0..2000 each, 0 off. */
void n265_set_nr(int intra, int inter);
/* --mcstf / --mcstf-ref-range / --selective-mcstf (phase 14 item 6). */
void n265_set_mcstf(int on, int range, int selective);
/* The hypothetical decoder buffer (phase 12 item 2): kbit/s, kbit, and a
 * fraction of the buffer. 0 / 0 is the mode off. */
void n265_set_vbv(double maxrate, double bufsize, double init);
/* "4.1", "41" or "123" -> 123; 0 for a string that is not a level. Pure
 * arithmetic over the Annex A table, so the 8-bit copy serves both
 * libraries and the dispatch table does not carry it. */
int n265_level_parse(const char *s);
/* The pass-through signaling flags' STRING forms (phase 14 item 1,
 * src/encoder/signal.h): the value tables of Annex E with x265's
 * spellings, and the four compound syntaxes. Depth-independent for the
 * same reason n265_level_parse is, so the 8-bit copy serves both
 * libraries and only the numeric SETTERS ride the dispatch table. */
enum {
    N265_VUI_COLORPRIM = 0,
    N265_VUI_TRANSFER,
    N265_VUI_COLORMATRIX,
    N265_VUI_VIDEOFORMAT,
    N265_VUI_RANGE,
    N265_VUI_OVERSCAN,
};
const char * const *n265_vui_names(int field);
int n265_vui_parse(int field, const char *s);
int n265_sar_parse(const char *s, int *idc, int *w, int *h);
int n265_window_parse(const char *s, int *l, int *t, int *r, int *b);
int n265_master_display_parse(const char *s, uint32_t v[10]);
int n265_cll_parse(const char *s, int *cll, int *fall);

/* --video-signal-type-preset (phase 14 item 15): the name table for a
 * whole color signaling set, in src/encoder/signal.c beside the value
 * tables it expands into. `v` is indexed by N265_VUI_* and by
 * N265_VSP_CHROMALOC; -1 is a field the preset leaves alone. */
#define N265_VSP_CHROMALOC  6           /* N265_VUI_NFIELD */
#define N265_VSP_NFIELD     7
enum {
    N265_VSP_OK             =  0,
    N265_VSP_BAD_SYSTEM     = -1,
    N265_VSP_BAD_VOLUME     = -2,
    N265_VSP_VOLUME_SYSTEM  = -3,
    N265_VSP_VOLUME_REFUSED = -4,
};
int n265_vsp_parse(const char *s, int *v, uint32_t *md, int *have_md,
                   const char **why);
const char *n265_vsp_name(int i);
const char *n265_vsp_volume_name(int i);

/* -t/--tune (phase 14 item 15): one accepted name and four named
 * refusals, in src/encoder/preset.c beside the presets. `t` is filled by
 * index; -1 is a knob the tune does not touch, and the CLI applies a
 * value only where the command line did not name one. */
enum {
    N265_TUNE_BFRAMES = 0,
    N265_TUNE_LOOKAHEAD,
    N265_TUNE_BADAPT,
    N265_TUNE_CUTREE,
    N265_TUNE_FRAMEWORK,
    N265_TUNE_NFIELD
};
enum {
    N265_TUNE_OK      =  0,
    N265_TUNE_UNKNOWN = -1,
    N265_TUNE_REFUSED = -2,
};
int n265_tune_parse(const char *name, int *t, const char **why);
const char *n265_tune_name(int i);

/* ---------------------------------------------------------------------
 * Both libraries, one command line (phase 8 item 5a).
 *
 * The sample width is a compile-time type, so there are two encoder
 * libraries and there always will be; what there is no longer is two
 * command lines. n265 links both archives and picks the encoder from the
 * INPUT: a C420 y4m goes to the 8-bit library, a C420p10 one to Main10,
 * and --output-depth 10 puts 8-bit input through Main10 after an explicit
 * upshift. The two are told apart by name - the Main10 library's symbols
 * carry a _10 suffix (include/yah265.h's N265_API, and a force-included
 * header for the internals) - and by nothing else: same header, same
 * structs, same argument lists.
 *
 * This file is compiled at N265_BIT_DEPTH 8, so the header hands it the
 * 8-bit names unsuffixed; the Main10 ones it declares here. n265_picture
 * carries its planes as `const void *` for exactly this reason, so one
 * struct and one dispatch table serve both widths.
 * ------------------------------------------------------------------ */
void n265_param_default_10(n265_param *p);
void n265_set_mode_10(int mode, int qp, int ctb_log2, int keyint,
                      int bframes, int aq);
void n265_set_la_10(int depth, int cutree);
void n265_set_crf_10(double crf);
void n265_set_bitrate_10(double kbps);
void n265_set_ref_10(int n);
void n265_set_badapt_10(int on);
void n265_set_threads_10(int n);
void n265_set_lossless_10(int on);
void n265_set_rdoq_level_10(int lvl);
void n265_set_tu_depth_10(int inter, int intra);
void n265_set_chroma_qpoffs_10(int cb, int cr);
void n265_set_level_10(int idc, int high_tier);
void n265_set_vui_timing_10(int on);
void n265_set_film_grain_10(double strength, int denoise);
void n265_set_nr_10(int intra, int inter);
void n265_set_mcstf_10(int on, int range, int selective);
void n265_set_vbv_10(double maxrate, double bufsize, double init);
void n265_set_hash_10(int type);
void n265_set_weight_10(int weightp, int weightb);
void n265_set_dup_thr_10(double thr);
void n265_set_gop_10(int open_gop, int radl);
void n265_set_vui_colour_10(int videoformat, int full_range, int colorprim,
                            int transfer, int colormatrix, int chromaloc,
                            int overscan);
void n265_set_vui_sar_10(int idc, int w, int h);
void n265_set_vui_window_10(int on, int l, int t, int r, int b);
void n265_set_hdr_meta_10(const uint32_t *md, int cll, int fall);
void n265_set_hdr10_10(int hdr10, int cll_on, int atc);
void n265_set_hdr10plus_10(int n, const uint8_t *const *t35,
                           const size_t *len);
void n265_set_dovi_rpu_10(int n, const uint8_t *const *rbsp,
                          const size_t *len);
void n265_set_hdr_opt_10(int on);
int  n265_set_preset_10(const char *name);
int  n265_preset_param_10(const char *key, int fallback);
int  n265_preset_param_res_10(const char *key, int fallback, int w, int h);
void n265_preset_uhd_10(int w, int h);
void n265_set_uhd_tree_10(int on);
const char *n265_preset_nearest_10(const char *name);
n265_encoder *n265_encoder_open_10(const n265_param *p);
int  n265_encoder_encode_10(n265_encoder *e, n265_nal **nal, int *nnal,
                            const n265_picture *pic);
void n265_encoder_close_10(n265_encoder *e);
const char *n265_version_str_10(void);
void n265_encoder_set_dump_decode_10(n265_encoder *e, FILE *f);
void n265_encoder_set_dump_10(n265_encoder *e, FILE *f);
void n265_encoder_set_shot_stats_10(n265_encoder *e, const char *path);
void n265_set_shot_window_10(int n);
void n265_set_pass_10(int pass, const char *stats);
void n265_set_metrics_10(int psnr, int ssim);
void n265_set_nal_opts_10(int repeat_headers, int aud, int eos, int eob,
                          int info);
void n265_set_info_opts_10(const char *opts);
void n265_set_rc_bounds_10(double crf_min, double crf_max, int qpmin,
                           int qpmax, int strict_cbr, double ausize_factor);
void n265_set_fingerprint_10(int on);
void n265_set_sei_opts_10(int recovery_sei, int pic_struct, int hrd_concat,
                          int temporal_layers);
void n265_set_qpfile_10(const char *path);
void n265_set_zones_10(const char *spec);
void n265_encoder_set_csv_10(n265_encoder *e, const char *path, int level);
long n265_encoder_metrics_10(n265_encoder *e, double out[6]);
long n265_encoder_dyn_lost_10(const n265_encoder *e);
void n265_show_config_10(const n265_encoder *e, FILE *f, int in_depth);

typedef struct n265_api {
    int depth;                  /* the samples this table's library takes */
    void (*param_default)(n265_param *);
    void (*set_mode)(int, int, int, int, int, int);
    void (*set_la)(int, int);
    void (*set_crf)(double);
    void (*set_bitrate)(double);
    void (*set_ref)(int);
    void (*set_badapt)(int);
    void (*set_threads)(int);
    int  (*set_preset)(const char *);
    int  (*preset_param)(const char *, int);
    /* the same question about a picture of a known size (item 12):
     * `medium`'s coding tree is 64 on a UHD-class picture and 32 below
     * it, and the CLI is where the geometry is known. */
    int  (*preset_param_res)(const char *, int, int, int);
    /* ...and the same geometry told to the KNOB half of the table (item
     * 17), which the encoder reads for itself. */
    void (*preset_uhd)(int, int);
    /* ...and whether the tree the preset just offered is the RESOLUTION
     * rule's rather than the user's, which is what the rate switch at
     * encoder open is allowed to move (item 17). */
    void (*set_uhd_tree)(int);
    const char *(*preset_nearest)(const char *);
    void (*set_shot_window)(int);
    void (*set_pass)(int, const char *);
    void (*set_lossless)(int);
    void (*set_level)(int, int);
    void (*set_vui_timing)(int);
    void (*set_film_grain)(double, int);
    void (*set_vbv)(double, double, double);
    void (*set_rdoq_level)(int);
    void (*set_tu_depth)(int, int);
    void (*set_chroma_qpoffs)(int, int);
    /* --weightp / --weightb (phase 14 item 2): weighted_pred_flag and
     * weighted_bipred_flag. -1 in either leaves that half to the preset. */
    void (*set_weight)(int, int);
    /* --dup-threshold (item 15): the near-duplicate bound, negative for
     * "not given". */
    void (*set_dup_thr)(double);
    void (*set_nr)(int, int);       /* --nr-intra / --nr-inter (item 6) */
    void (*set_mcstf)(int, int, int);   /* --mcstf and its two (item 6) */
    /* the pass-through signaling setters (phase 14 item 1). They take
     * syntax elements, never names: the tables above turn the command
     * line's spellings into numbers once, before the dispatch. */
    void (*set_hash)(int);
    void (*set_gop)(int, int);      /* --open-gop / --radl (item 3) */
    void (*set_vui_colour)(int, int, int, int, int, int, int);
    void (*set_vui_sar)(int, int, int);
    void (*set_vui_window)(int, int, int, int, int);
    void (*set_hdr_meta)(const uint32_t *, int, int);
    void (*set_hdr10)(int, int, int);
    void (*set_hdr10plus)(int, const uint8_t *const *, const size_t *);
    void (*set_dovi_rpu)(int, const uint8_t *const *, const size_t *);
    void (*set_hdr_opt)(int);        /* --hdr-opt (item 56) */
    n265_encoder *(*encoder_open)(const n265_param *);
    int  (*encoder_encode)(n265_encoder *, n265_nal **, int *,
                           const n265_picture *);
    void (*encoder_close)(n265_encoder *);
    const char *(*version_str)(void);
    void (*set_dump)(n265_encoder *, FILE *);
    void (*set_dump_decode)(n265_encoder *, FILE *);
    void (*set_shot_stats)(n265_encoder *, const char *);
    /* --psnr / --ssim / --csv (phase 14 item 4) */
    void (*set_metrics)(int, int);
    void (*set_nal_opts)(int, int, int, int, int);
    void (*set_info_opts)(const char *);    /* item 9 */
    void (*set_rc_bounds)(double, double, int, int, int, double);
    void (*set_sei_opts)(int, int, int, int);
    void (*set_qpfile)(const char *);
    void (*set_zones)(const char *);
    void (*set_csv)(n265_encoder *, const char *, int);
    long (*metrics)(n265_encoder *, double *);
    void (*set_fingerprint)(int);   /* --no-fingerprint (item 10) */
    /* --show-config / --verbose (item 13): the resolved configuration,
     * printed by the library because the library is where it is resolved.
     * The third argument is the INPUT file's sample width, which is the
     * one row the encoder cannot know. */
    void (*show_config)(const n265_encoder *, FILE *, int);
    /* NALs an allocation failure dropped, cut short or never handed out,
     * HDR10+ and Dolby Vision metadata included (the name is older) */
    long (*dyn_lost)(const n265_encoder *);
} n265_api;

static const n265_api api8 = {
    8, n265_param_default, n265_set_mode, n265_set_la, n265_set_crf,
    n265_set_bitrate, n265_set_ref, n265_set_badapt, n265_set_threads,
    n265_set_preset, n265_preset_param, n265_preset_param_res,
    n265_preset_uhd, n265_set_uhd_tree, n265_preset_nearest,
    n265_set_shot_window, n265_set_pass,
    n265_set_lossless, n265_set_level, n265_set_vui_timing,
    n265_set_film_grain, n265_set_vbv, n265_set_rdoq_level,
    n265_set_tu_depth, n265_set_chroma_qpoffs, n265_set_weight,
    n265_set_dup_thr, n265_set_nr,
    n265_set_mcstf,
    n265_set_hash, n265_set_gop, n265_set_vui_colour, n265_set_vui_sar,
    n265_set_vui_window, n265_set_hdr_meta, n265_set_hdr10,
    n265_set_hdr10plus, n265_set_dovi_rpu, n265_set_hdr_opt,
    n265_encoder_open, n265_encoder_encode, n265_encoder_close,
    n265_version_str, n265_encoder_set_dump, n265_encoder_set_dump_decode,
    n265_encoder_set_shot_stats,
    n265_set_metrics, n265_set_nal_opts, n265_set_info_opts,
    n265_set_rc_bounds,
    n265_set_sei_opts, n265_set_qpfile, n265_set_zones,
    n265_encoder_set_csv, n265_encoder_metrics, n265_set_fingerprint,
    n265_show_config, n265_encoder_dyn_lost,
};

static const n265_api api10 = {
    10, n265_param_default_10, n265_set_mode_10, n265_set_la_10,
    n265_set_crf_10, n265_set_bitrate_10, n265_set_ref_10,
    n265_set_badapt_10, n265_set_threads_10, n265_set_preset_10,
    n265_preset_param_10, n265_preset_param_res_10,
    n265_preset_uhd_10, n265_set_uhd_tree_10, n265_preset_nearest_10,
    n265_set_shot_window_10, n265_set_pass_10,
    n265_set_lossless_10, n265_set_level_10, n265_set_vui_timing_10,
    n265_set_film_grain_10, n265_set_vbv_10, n265_set_rdoq_level_10,
    n265_set_tu_depth_10, n265_set_chroma_qpoffs_10, n265_set_weight_10,
    n265_set_dup_thr_10, n265_set_nr_10, n265_set_mcstf_10,
    n265_set_hash_10, n265_set_gop_10, n265_set_vui_colour_10,
    n265_set_vui_sar_10,
    n265_set_vui_window_10, n265_set_hdr_meta_10, n265_set_hdr10_10,
    n265_set_hdr10plus_10, n265_set_dovi_rpu_10, n265_set_hdr_opt_10,
    n265_encoder_open_10,
    n265_encoder_encode_10, n265_encoder_close_10, n265_version_str_10,
    n265_encoder_set_dump_10, n265_encoder_set_dump_decode_10,
    n265_encoder_set_shot_stats_10,
    n265_set_metrics_10, n265_set_nal_opts_10, n265_set_info_opts_10,
    n265_set_rc_bounds_10,
    n265_set_sei_opts_10, n265_set_qpfile_10, n265_set_zones_10,
    n265_encoder_set_csv_10, n265_encoder_metrics_10,
    n265_set_fingerprint_10, n265_show_config_10,
    n265_encoder_dyn_lost_10,
};

/* QpBdOffsetY (7-5), and the clip bound of Clip1Y. Runtime now, off the
 * depth the ENCODER was selected at - the CLI no longer has a depth of
 * its own. */
#define N265_QP_BD_OFF(d)  (6 * ((d) - 8))
#define N265_SAMPLE_MAX(d) ((1 << (d)) - 1)

typedef struct y4m_t {
    FILE *f;
    int w, h, fps_num, fps_den;
    int depth;              /* sample width the FILE carries: 8 or 10 */
    int out_depth;          /* sample width the ENCODER takes: 8 or 10 */
    int csp;                /* N265_CSP_*, i.e. chroma_format_idc      */
    int csx, csy;           /* log2 SubWidthC / SubHeightC             */
    size_t frame_bytes;     /* the y4m payload, as it sits on disk */
    uint8_t *frame;
    /* The 16-bit plane, where one is needed: the assembled little-endian
     * pairs of a C420p10 file, or an 8-bit file upshifted for Main10. At
     * 8 in and 8 out it is NULL and the library is handed `frame`
     * itself - no copy, no second allocation, nothing in the read path,
     * which is what keeps the 8-bit encoder byte-for-byte and
     * cycle-for-cycle the one that was measured. */
    uint16_t *pix16;
    /* The 8-bit plane on the DOWNSHIFT path (--output-depth 8 on 10-bit
     * input, phase 14 item 6): the file carries 16-bit samples and the
     * encoder takes 8-bit ones, so unlike every other combination there
     * is no way to hand the library the bytes it was given. `dither`
     * picks which of the two reductions runs (see y4m_read). */
    uint8_t *pix8;
    int dither;
    size_t nsamp;           /* samples in one picture, all three planes */
    const void *plane[3];   /* what the library is handed, once */
    long clipped;           /* samples above (1 << depth) - 1 in the file */
    /* RAW input (--input-res): no container and no per-frame header, so
     * the geometry a y4m header would have carried comes off the command
     * line and a frame is exactly `frame_bytes` of planes. Everything
     * after the open is shared with the y4m path - the same allocation,
     * the same upshift, the same clamp - which is why this is a flag on
     * the reader and not a second reader. */
    int raw;
} y4m_t;

/* The C tag's depth, or -1 for a chroma sampling or sample width this
 * encoder does not do; `*fmt` receives the chroma format (N265_CSP_*,
 * which is chroma_format_idc).
 *
 * 4:2:0, 4:2:2 and 4:4:4, each at 8 bits (the plain tag, and for 4:2:0
 * the three siting variants, which differ in chroma POSITION and not in
 * width) or 10 - `C420p10` / `C422p10` / `C444p10`, which is what ffmpeg
 * writes for yuv420p10le / yuv422p10le / yuv444p10le; the `le` spelling
 * is taken as the same thing, since some writers use it. Monochrome and
 * 12/16-bit are later items and are REFUSED rather than read as
 * something else, which is what a prefix match would have done. */
static int y4m_csp_depth(const char *csp, int *fmt)
{
    *fmt = N265_CSP_I420;
    if (!strcmp(csp, "420") || !strcmp(csp, "420jpeg") ||
        !strcmp(csp, "420mpeg2") || !strcmp(csp, "420paldv"))
        return 8;
    if (!strcmp(csp, "420p10") || !strcmp(csp, "420p10le"))
        return 10;
    *fmt = N265_CSP_I422;
    if (!strcmp(csp, "422"))
        return 8;
    if (!strcmp(csp, "422p10") || !strcmp(csp, "422p10le"))
        return 10;
    *fmt = N265_CSP_I444;
    if (!strcmp(csp, "444"))
        return 8;
    if (!strcmp(csp, "444p10") || !strcmp(csp, "444p10le"))
        return 10;
    *fmt = N265_CSP_I420;
    return -1;
}

/* --input-csp: x265's spellings for the raw reader, and the bare number
 * (chroma_format_idc) too, since that is what the value IS. Returns
 * N265_CSP_*, or -1. i400 (monochrome) is refused by NAME rather than by
 * falling off the end, because it is a format this encoder will
 * eventually have and a shrug would read as a typo. */
static int input_csp_parse(const char *s, int *mono)
{
    *mono = 0;
    if (!strcmp(s, "i420") || !strcmp(s, "420") || !strcmp(s, "1"))
        return N265_CSP_I420;
    if (!strcmp(s, "i422") || !strcmp(s, "422") || !strcmp(s, "2"))
        return N265_CSP_I422;
    if (!strcmp(s, "i444") || !strcmp(s, "444") || !strcmp(s, "3"))
        return N265_CSP_I444;
    if (!strcmp(s, "i400") || !strcmp(s, "400") || !strcmp(s, "0"))
        *mono = 1;
    return -1;
}

/* x265 picks the raw reader off the `.yuv` extension; so does this, as a
 * convenience on top of --input-res, which is the real switch. "-" is
 * stdin and has no name to read. */
static int is_raw_name(const char *path)
{
    size_t n = strlen(path);
    return n >= 4 && (!strcmp(path + n - 4, ".yuv") ||
                      !strcmp(path + n - 4, ".YUV"));
}

static int is_y4m_name(const char *path)
{
    size_t n = strlen(path);
    return n >= 4 && (!strcmp(path + n - 4, ".y4m") ||
                      !strcmp(path + n - 4, ".Y4M"));
}

/* --fps: "30", "29.97", "30000/1001" or "30000:1001". The rational forms
 * are kept EXACTLY - 30000/1001 is not 29.97 to a decoder reading
 * vui_time_scale - and a decimal is turned into one over a power of ten
 * rather than into the nearest broadcast rate, because guessing that a
 * user who typed 29.97 meant 30000/1001 is a guess about their content.
 * Returns 0, or -1. */
static int fps_parse(const char *s, int *num, int *den)
{
    const char *sep = strpbrk(s, "/:");
    if (sep) {
        long n = atol(s), d = atol(sep + 1);
        if (n <= 0 || d <= 0)
            return -1;
        *num = (int)n;
        *den = (int)d;
        return 0;
    }
    const char *dot = strchr(s, '.');
    if (!dot) {
        long n = atol(s);
        if (n <= 0)
            return -1;
        *num = (int)n;
        *den = 1;
        return 0;
    }
    int digits = 0;
    long n = 0, d = 1;
    for (const char *p = s; *p; p++) {
        if (*p == '.')
            continue;
        if (*p < '0' || *p > '9')
            return -1;
        if (n > 100000000L)
            return -1;
        n = n * 10 + (*p - '0');
        if (p > dot) {
            d *= 10;
            digits++;
        }
    }
    if (n <= 0 || digits > 6)
        return -1;
    *num = (int)n;
    *den = (int)d;
    return 0;
}

/* The half of the open that does not care where the geometry came from:
 * w/h/csp/depth/fps are already in *y, and `desc` names the format for
 * the one refusal that has to print it. `y->out_depth` is read (the
 * --output-depth request, 0 for "the input's") and then resolved.
 * Returns 0, -1 on an allocation failure, -2 when it has already printed
 * the reason itself. */
static int src_geom(y4m_t *y, const char *desc)
{
    /* log2 SubWidthC / SubHeightC. 4:2:2 is the format where the two
     * differ: half the width, all of the height. */
    y->csx = y->csp == N265_CSP_I444 ? 0 : 1;
    y->csy = y->csp == N265_CSP_I420 ? 1 : 0;
    if ((y->w & 1) || (y->h & 1)) {
        /* n265_encoder_open's own condition, checked here so the refusal
         * names the picture instead of reading as an open failure. */
        fprintf(stderr, "n265: %dx%d: the encoder needs even dimensions\n",
                y->w, y->h);
        return -2;
    }
    /* The output depth defaults to the input's, which is the whole of the
     * dispatch: a C420 file gets the 8-bit library and a C420p10 file gets
     * Main10, and neither is a decision anybody has to type. --output-depth
     * 10 is the one deliberate override - SDR content coded as a Main10
     * stream, what x265's flag of the same name does - and it upshifts in
     * the read loop below.
     *
     * The REVERSE - 10-bit input coded at 8 - is the one x265's --dither
     * exists for, and it landed here in phase 14 item 6. It is still a
     * choice about the PICTURE and not about the encoder, so it is still
     * only ever made when it is asked for by name: --output-depth 8 is
     * required, there is no default that reaches it, and which of the two
     * reductions runs is --dither's own answer. The audit's row said the
     * prerequisite had to land before --dither could mean anything; this
     * is that prerequisite. */
    if (y->out_depth == 0)
        y->out_depth = y->depth;
    if (y->out_depth < y->depth && (y->out_depth != 8 || y->depth != 10)) {
        fprintf(stderr, "n265: --output-depth %d on %d-bit input (%s): the "
                "only downshift here is 10 to 8.\n", y->out_depth,
                y->depth, desc);
        return -2;
    }
    size_t luma = (size_t)y->w * (size_t)y->h;
    size_t chroma = (size_t)(y->w >> y->csx) * (size_t)(y->h >> y->csy);
    size_t nsamp = luma + 2 * chroma;
    y->frame_bytes = nsamp * (y->depth > 8 ? 2 : 1);
    y->frame = malloc(y->frame_bytes);
    if (!y->frame)
        return -1;
    if (y->out_depth == 8 && y->depth > 8) {
        /* the downshift: the file's samples are 16-bit and the library's
         * are 8-bit, so there is a second plane and a conversion in the
         * read loop. The only combination where that is true. */
        y->pix8 = malloc(nsamp);
        if (!y->pix8)
            return -1;
        y->plane[0] = y->pix8;
        y->plane[1] = y->pix8 + luma;
        y->plane[2] = y->pix8 + luma + chroma;
    } else if (y->out_depth == 8) {
        y->plane[0] = y->frame;
        y->plane[1] = y->frame + luma;
        y->plane[2] = y->frame + luma + chroma;
    } else {
        y->pix16 = malloc(nsamp * sizeof(*y->pix16));
        if (!y->pix16)
            return -1;
        y->plane[0] = y->pix16;
        y->plane[1] = y->pix16 + luma;
        y->plane[2] = y->pix16 + luma + chroma;
    }
    y->nsamp = nsamp;
    return 0;
}

/* `y->out_depth` is read (the --output-depth request, 0 for "the
 * input's") and then resolved; everything else in *y is set here.
 * Returns 0, -1 for a malformed file (the caller says so), or -2 when it
 * has already printed the reason itself. */
static int y4m_open(y4m_t *y, FILE *f)
{
    char line[1024];
    int want_depth = y->out_depth;
    memset(y, 0, sizeof(*y));
    y->out_depth = want_depth;
    y->f = f;
    y->fps_num = 25;
    y->fps_den = 1;
    if (!fgets(line, sizeof(line), f))
        return -1;
    if (strncmp(line, "YUV4MPEG2", 9) != 0)
        return -1;
    char csp[64] = "420";
    for (char *tok = strtok(line + 9, " \n"); tok; tok = strtok(NULL, " \n")) {
        switch (tok[0]) {
        case 'W': y->w = atoi(tok + 1); break;
        case 'H': y->h = atoi(tok + 1); break;
        case 'F': sscanf(tok + 1, "%d:%d", &y->fps_num, &y->fps_den); break;
        case 'C': snprintf(csp, sizeof(csp), "%s", tok + 1); break;
        default: break; /* interlace/aspect/params ignored */
        }
    }
    if (y->w <= 0 || y->h <= 0)
        return -1;
    y->depth = y4m_csp_depth(csp, &y->csp);
    if (y->depth < 0) {
        fprintf(stderr, "n265: unsupported input format C%s "
                "(4:2:0, 4:2:2 or 4:4:4, at 8 or 10 bits)\n", csp);
        return -2;
    }
    char desc[72];
    snprintf(desc, sizeof(desc), "C%s", csp);
    return src_geom(y, desc);
}

/* RAW planar input: the geometry is the caller's, checked by the caller,
 * and there is nothing in the file to read before the first sample. */
static int raw_open(y4m_t *y, FILE *f, int w, int h, int csp, int depth,
                    int fps_num, int fps_den)
{
    int want_depth = y->out_depth;
    memset(y, 0, sizeof(*y));
    y->out_depth = want_depth;
    y->f = f;
    y->raw = 1;
    y->w = w;
    y->h = h;
    y->csp = csp;
    y->depth = depth;
    y->fps_num = fps_num;
    y->fps_den = fps_den;
    const char *fmt = csp == N265_CSP_I444 ? "i444"
                    : csp == N265_CSP_I422 ? "i422" : "i420";
    char desc[72];
    snprintf(desc, sizeof(desc), "raw %s at %d bits", fmt, depth);
    return src_geom(y, desc);
}

/* --seek: drop the first `n` frames of the source without converting
 * them. A regular file is stepped over with fseek and a pipe is read and
 * thrown away, which is the only thing a pipe allows; either way the
 * picture the encoder sees first is frame `n` of the source and its pts
 * starts at 0, so the coded stream is the one you would get by trimming
 * the input with ffmpeg. Returns 0, or -1 if the source ran out first
 * (the caller says so). */
static int src_skip(y4m_t *y, long n)
{
    char line[256];
    for (long i = 0; i < n; i++) {
        if (!y->raw) {
            if (!fgets(line, sizeof(line), y->f))
                return -1;
            if (strncmp(line, "FRAME", 5) != 0)
                return -1;
        }
        if (fseek(y->f, (long)y->frame_bytes, SEEK_CUR) != 0) {
            /* not seekable (a pipe, or a file bigger than long): read it */
            size_t left = y->frame_bytes;
            while (left) {
                size_t want = left < y->frame_bytes ? left : y->frame_bytes;
                size_t got = fread(y->frame, 1, want, y->f);
                if (got == 0)
                    return -1;
                left -= got;
            }
        }
    }
    /* fseek past the end of a regular file SUCCEEDS, so the loop above
     * cannot tell "skipped n frames" from "skipped off the end". One
     * character answers it, and putting it back costs nothing. */
    if (n > 0) {
        int c = fgetc(y->f);
        if (c == EOF)
            return -1;
        ungetc(c, y->f);
    }
    return 0;
}

/* returns 1 on frame read, 0 on EOF, -1 on error */
static int y4m_read(y4m_t *y)
{
    char line[256];
    if (!y->raw) {
        if (!fgets(line, sizeof(line), y->f))
            return 0;
        if (strncmp(line, "FRAME", 5) != 0)
            return -1;
    }
    size_t got = fread(y->frame, 1, y->frame_bytes, y->f);
    if (got != y->frame_bytes)
        return got == 0 && y->raw ? 0 : -1;
    if (y->out_depth == 8 && y->depth == 8)
        return 1;               /* 8 in, 8 out: the file IS the picture */
    if (y->out_depth == 8) {
        /* The DOWNSHIFT, 10 to 8 (phase 14 item 6).
         *
         * Plain, it is a right shift by two - the exact inverse of the
         * upshift below, so a clip taken 8 -> 10 -> 8 comes back the
         * picture it was, sample for sample. What it costs is the four
         * input levels that map to one output level: a slow gradient
         * that crossed them smoothly arrives as a staircase, and the
         * eye finds a staircase on a flat wall at once.
         *
         * --dither is the answer to that, and it is the ORDERED one: a
         * 2x2 matrix carrying each of the four discarded residues
         * exactly once, added before the shift. Over any 2x2 it is
         * EXACTLY mean-preserving - sum over k of floor((v + k) / 4) for
         * k = 0..3 is v, identically - so the staircase becomes a
         * dot pattern at the level the gradient really had, and no
         * energy is added or lost. An error-diffusion dither would
         * spread the residue further and look better still; it is also
         * a serial dependence across the whole picture, which is a
         * strange thing to put in front of an encoder that is otherwise
         * a pure function of its input block by block.
         *
         * Both paths clamp: 1023 + 3 is 256 after the shift. */
        static const unsigned bayer2[2][2] = { { 0, 2 }, { 3, 1 } };
        size_t off = 0;
        for (int c = 0; c < 3; c++) {
            int pw = c ? y->w >> y->csx : y->w;
            int ph = c ? y->h >> y->csy : y->h;
            for (int py = 0; py < ph; py++) {
                const uint8_t *r = y->frame + 2 * (off + (size_t)py * pw);
                uint8_t *d = y->pix8 + off + (size_t)py * pw;
                for (int px = 0; px < pw; px++) {
                    unsigned v = (unsigned)r[2 * px] |
                                 ((unsigned)r[2 * px + 1] << 8);
                    if (v > 1023u) {
                        v = 1023u;
                        y->clipped++;
                    }
                    if (y->dither)
                        v += bayer2[py & 1][px & 1];
                    v >>= 2;
                    d[px] = (uint8_t)(v > 255u ? 255u : v);
                }
            }
            off += (size_t)pw * ph;
        }
        return 1;
    }
    size_t nsamp = y->nsamp;
    if (y->depth == 8) {
        /* The --output-depth 10 upshift: a PURE LEFT SHIFT BY TWO, the
         * same arithmetic scripts/upshift10.sh gets out of ffmpeg's
         * format filter (sample * 4, not sample * 1023/255). It is what a
         * Main10 stream of SDR content is, and it is what makes the
         * Main10 encode of an 8-bit clip comparable with its 8-bit encode
         * at the same QP: the residual is exactly four times, the forward
         * transform sheds exactly two bits, and the coefficients that
         * reach the quantizer are the same integers. It costs one pass
         * over the frame and no latency - the picture is converted where
         * it is read, not queued. */
        for (size_t i = 0; i < nsamp; i++)
            y->pix16[i] = (uint16_t)(y->frame[i] << 2);
        return 1;
    }
    /* 16-bit little-endian samples, assembled by hand rather than cast:
     * the byte order is the FILE's and not the host's, and the payload is
     * not guaranteed to be aligned for a uint16_t load. A sample above
     * the declared depth is out of spec - the encoder clips its own
     * reconstruction to PIXEL_MAX, so a source value it cannot represent
     * would read as a permanent residual and as a recon mismatch against
     * every decoder - so it is clamped here and counted, and the count is
     * reported once at the end rather than per sample. */
    for (size_t i = 0; i < nsamp; i++) {
        unsigned v = (unsigned)y->frame[2 * i] |
                     ((unsigned)y->frame[2 * i + 1] << 8);
        if (v > (unsigned)N265_SAMPLE_MAX(y->depth)) {
            v = (unsigned)N265_SAMPLE_MAX(y->depth);
            y->clipped++;
        }
        y->pix16[i] = (uint16_t)v;
    }
    return 1;
}

/* READ-AHEAD (phase 14 item 55). Above one thread the input is read on
 * its own thread into a ring of frames, so a slow feeder overlaps the
 * encode instead of stalling the driver between pictures. The samples are
 * the ones y4m_read gives inline, so the stream is the same. */
typedef struct rd_ring {
    y4m_t *slot;                /* slot 0 is the caller's reader */
    int n, head, count;
    int done, last;             /* the reader stopped; y4m_read's result */
    int stop;
    long limit;                 /* --frames, -1 for none */
    pthread_mutex_t m;
    pthread_cond_t cv;
    pthread_t t;
} rd_ring;

static void *rd_main(void *arg)
{
    rd_ring *q = arg;
    long got = 0;
    for (;;) {
        pthread_mutex_lock(&q->m);
        while (q->count == q->n && !q->stop)
            pthread_cond_wait(&q->cv, &q->m);
        int s = (q->head + q->count) % q->n;
        int stop = q->stop;
        pthread_mutex_unlock(&q->m);
        int r = 0;
        if (!stop && (q->limit < 0 || got < q->limit))
            r = y4m_read(&q->slot[s]);
        pthread_mutex_lock(&q->m);
        if (r > 0) {
            q->count++;
            got++;
        } else {
            q->done = 1;
            q->last = r;
        }
        pthread_cond_broadcast(&q->cv);
        pthread_mutex_unlock(&q->m);
        if (r <= 0)
            return NULL;
    }
}

/* the slot count, or 0 for the inline read */
static int rd_slots(const y4m_t *y, int threads)
{
    const char *e = getenv("N265_READAHEAD_MB");
    long mb = e ? atol(e) : 64;
    if (mb <= 0 || threads == 1)
        return 0;
    size_t per = y->frame_bytes + (y->pix8 ? y->nsamp : 0) +
                 (y->pix16 ? y->nsamp * sizeof(uint16_t) : 0);
    long n = (long)(((size_t)mb << 20) / per);
    return n < 2 ? 2 : n > 64 ? 64 : (int)n;
}

static int rd_open(rd_ring *q, y4m_t *y, int n, long limit)
{
    memset(q, 0, sizeof(*q));
    q->slot = calloc((size_t)n, sizeof(*q->slot));
    if (!q->slot)
        return -1;
    q->n = n;
    q->limit = limit;
    for (int i = 0; i < n; i++) {
        y4m_t *s = &q->slot[i];
        *s = *y;
        if (i == 0)
            continue;
        s->clipped = 0;
        s->frame = malloc(y->frame_bytes);
        s->pix8 = y->pix8 ? malloc(y->nsamp) : NULL;
        s->pix16 = y->pix16 ? malloc(y->nsamp * sizeof(uint16_t)) : NULL;
        if (!s->frame || (y->pix8 && !s->pix8) || (y->pix16 && !s->pix16))
            return -1;
        const uint8_t *b0 = y->pix8 ? y->pix8 : y->pix16 ?
                            (const uint8_t *)y->pix16 : y->frame;
        const uint8_t *b1 = s->pix8 ? s->pix8 : s->pix16 ?
                            (const uint8_t *)s->pix16 : s->frame;
        for (int c = 0; c < 3; c++)
            s->plane[c] = b1 + ((const uint8_t *)y->plane[c] - b0);
    }
    pthread_mutex_init(&q->m, NULL);
    pthread_cond_init(&q->cv, NULL);
    if (pthread_create(&q->t, NULL, rd_main, q) != 0)
        return -1;
    return 0;
}

/* the next frame's reader, NULL at the end (*r: 0 EOF, -1 error) */
static y4m_t *rd_next(rd_ring *q, int *r)
{
    pthread_mutex_lock(&q->m);
    while (q->count == 0 && !q->done)
        pthread_cond_wait(&q->cv, &q->m);
    y4m_t *s = q->count ? &q->slot[q->head] : NULL;
    if (!s)
        *r = q->last;
    pthread_mutex_unlock(&q->m);
    return s;
}

static void rd_release(rd_ring *q)
{
    pthread_mutex_lock(&q->m);
    q->head = (q->head + 1) % q->n;
    q->count--;
    pthread_cond_broadcast(&q->cv);
    pthread_mutex_unlock(&q->m);
}

/* joins the reader and folds the slots' clip counts into *y */
static void rd_close(rd_ring *q, y4m_t *y)
{
    pthread_mutex_lock(&q->m);
    q->stop = 1;
    pthread_cond_broadcast(&q->cv);
    pthread_mutex_unlock(&q->m);
    pthread_join(q->t, NULL);
    y->clipped = 0;
    for (int i = 0; i < q->n; i++) {
        y->clipped += q->slot[i].clipped;
        if (i) {
            free(q->slot[i].frame);
            free(q->slot[i].pix8);
            free(q->slot[i].pix16);
        }
    }
    free(q->slot);
    pthread_mutex_destroy(&q->m);
    pthread_cond_destroy(&q->cv);
}

static void usage(void)
{
    fprintf(stderr,
        "usage: n265 [options] <input.y4m|input.yuv|->\n"
        "  One binary, every profile: the encoder is chosen from the input.\n"
        "  A C420 y4m is coded Main (8-bit), a C420p10 one Main10, a C444\n"
        "  one Main 4:4:4, a C444p10 one Main 4:4:4 10, and a C422 or\n"
        "  C422p10 one Main 4:2:2 10 (Table A.2 has no 8-bit 4:2:2 profile)\n"
        "  - no flag says so. The chroma format is a runtime SPS property;\n"
        "  only the sample width picks a library. See --output-depth.\n"
        "  -o <file>       output Annex-B bitstream (default: out.hevc)\n"
        "  --frames <n>    encode at most n frames\n"
        "  --output-depth <8|10>  code at this sample width (default: the\n"
        "                  input's). 10 on 8-bit input is the deliberate\n"
        "                  case: a Main10 stream of SDR content, upshifted\n"
        "                  by a pure left shift of two as it is read - one\n"
        "                  pass per frame, no added latency. 8 on 10-bit\n"
        "                  input is refused: there is no downshift here.\n"
        "  --seek <n>      drop the first n frames of the input. The\n"
        "                  picture the encoder sees first is frame n of\n"
        "                  the source and its pts starts at 0, so the\n"
        "                  stream is the one you get by trimming the\n"
        "                  input first. Both readers; a pipe is read\n"
        "                  through, a file is seeked over.\n"
        "\n"
        "  RAW planar input, for a source with no y4m header. The reader is\n"
        "  selected by --input-res (or by a .yuv name), and then the\n"
        "  geometry a header would have carried has to be given in full -\n"
        "  size, chroma format and frame rate - because a raw file says\n"
        "  nothing about itself and a guess codes garbage without a word.\n"
        "  --input-res <WxH>  picture size, e.g. 352x288\n"
        "  --input-csp <c>  i420, i422 or i444 (or the chroma_format_idc\n"
        "                  itself: 1, 2, 3). i400 is refused by name.\n"
        "  --fps <N[/D]>   frame rate: 30, 30000/1001 or 29.97. A ratio is\n"
        "                  kept exactly; a decimal becomes N/10^k and is\n"
        "                  NOT rounded to the nearest broadcast rate.\n"
        "  --input-depth <8|10>  sample width of the raw file (default 8).\n"
        "                  10-bit is 16-bit little-endian samples, as\n"
        "                  ffmpeg's yuv420p10le writes them.\n"
        "\n"
        "  --preset <name>  speed/quality point, in x265's names.\n"
        "                  Rungs, fastest first: fast, medium (default),\n"
        "                  slow, slower. x265's other six names are\n"
        "                  known and refused, naming the nearest rung\n"
        "                  we have; `faster` and `veryfast` were built,\n"
        "                  measured against the x265 preset of the same\n"
        "                  name and refused (docs/phase13.md, C2), and so\n"
        "                  were `veryslow` and `placebo` (docs/phase14.md,\n"
        "                  item 5).\n"
        "                  (default medium, the point the project's goals are\n"
        "                  read at: at or ahead of x265 medium on every\n"
        "                  corpus clip, in less time).\n"
        "                  Sets the DEFAULTS of the tuning knobs; any flag or\n"
        "                  N265_* variable you give yourself still wins.\n"
        "  -t, --tune <name>  x265's five tune names, answered one at a\n"
        "                  time. ACCEPTED: zerolatency (--bframes 0\n"
        "                  --rc-lookahead 1 --b-adapt 0 --cutree 0 and one\n"
        "                  frame in flight - a composition of knobs this\n"
        "                  encoder already has, nothing new). REFUSED BY\n"
        "                  NAME, each printing its own reason: psnr and\n"
        "                  ssim (metric fitting, and the band decides on\n"
        "                  VMAF-NEG), grain (it is --rc-grain's damping\n"
        "                  constants, not --film-grain), fastdecode (two\n"
        "                  of its five legs are not flags we own). A flag\n"
        "                  you name yourself beats the tune, and the tune\n"
        "                  beats the preset.\n"
        "  --qp <n>        fixed QP (default 26). Range 0..51 at 8 bits,\n"
        "                  -12..51 at Main10 (QpBdOffsetY, 7.4.3.3)\n"
        "  --crf <f>       constant rate factor (base QP from f; overrides\n"
        "                  --qp). Same range as --qp, negative included at\n"
        "                  Main10: giving the flag at all is what turns CRF\n"
        "                  on, so --crf -6 is a rate factor and not an off\n"
        "                  switch. THE AXIS MOVED ON 2026-09-16: the number\n"
        "                  carries the FRAME RATE (1.8 QP per doubling of\n"
        "                  it, zero at 25 fps) and no longer carries the\n"
        "                  clip's absolute energy, so it transfers between\n"
        "                  clips the way a user expects. A crf number\n"
        "                  recorded before that date reproduces under\n"
        "                  N265_CRF_LEGACY=1\n"
        "  --bitrate <k>   single-pass ABR at k kbit/s (overrides --crf)\n"
        "  --ref <n>       references per list, 1..5 (default 1;\n"
        "                  `slower` runs 2, `veryslow` and `placebo` more).\n"
        "                  The DPB the shape needs is solved from it and\n"
        "                  the level raised to hold it; a shape the pool\n"
        "                  cannot hold is refused and says so.\n"
        "  --b-adapt <0-4> adaptive B run length from the lookahead: 0 fixed,\n"
        "                  1 the motion rule (default), 2/3 refused priced arms,\n"
        "                  4 the bits model (docs/phase7-gop-design.md)\n"
        "  --rdoq-level <0|1|2>  how much rate-distortion optimised\n"
        "                  quantisation to run. x265's numbering, so that a\n"
        "                  number carries over; ours is N265_RDOQ and counts\n"
        "                  the same three encoders in another order\n"
        "                  (docs/instruments.md).\n"
        "                  0 off, plain quantisation everywhere: 16%% off our\n"
        "                    wall, and about 18 BD points of quality given\n"
        "                    back (only 2 of 15 corpus clips stay ahead of\n"
        "                    x265 medium). It is what puts our worst clip\n"
        "                    under x265's own wall.\n"
        "                  1 the final encode only (medium's default): the\n"
        "                    lattice once per coded block, at the commit.\n"
        "                  2 every RD decision (slow's and slower's default):\n"
        "                    8%% more wall for about 3 BD points.\n"
        "                  Left off, the preset picks; given, it wins over\n"
        "                  the preset and over N265_RDOQ.\n"
        "  (N265_PSY=F adds the psy term to the quantiser's distortion)\n"
        "  --tu-inter-depth <1..4>  transform-tree levels an inter CU\n"
        "                  may use; --tu-intra-depth the same for intra.\n"
        "                  x265's names and x265's numbering, which is\n"
        "                  the SPS max_transform_hierarchy_depth plus one.\n"
        "                  1 codes no split flag at all (a partitioned\n"
        "                  inter CU still infers one); 2 is our default\n"
        "                  and every stream written before 2026-09-14;\n"
        "                  3 and 4 are what x265 veryslow and placebo run.\n"
        "                  Left off, the preset picks.\n"
        "  --cbqpoffs <n>  pps_cb_qp_offset, -12..12 (the preset's if not given)\n"
        "  --crqpoffs <n>  pps_cr_qp_offset, -12..12 (the same)\n"
        "                  The chroma quantiser's offset from luma's, added\n"
        "                  into qPi before Table 8-10 (8.6.1). NEGATIVE\n"
        "                  spends more on chroma; positive moves those bits\n"
        "                  to luma. medium names -1 for both from the 720p\n"
        "                  class up, where VMAF v1 scores chroma, and 0\n"
        "                  below it; every other preset names 0\n"
        "                  (docs/phase14.md item 51). A flag given, 0\n"
        "                  included, wins over the preset.\n"
        "  --pcm           phase-0 I_PCM mode (lossless, huge)\n"
        "  --lossless      mathematically lossless: transquant bypass on\n"
        "                  every CU, so the reconstruction IS the source.\n"
        "                  Turns off rate control, RDOQ, sign-bit hiding,\n"
        "                  AQ, cu-tree, psy-rd and both loop filters (all\n"
        "                  of them price against a quantiser there is not).\n"
        "                  --qp/--crf/--bitrate are ignored; prediction and\n"
        "                  mode decision still run, on bits alone.\n"
        "  --ctu <16|32|64>  coding tree block size. The compiled-in\n"
        "                  default is 64 and every preset offers its own:\n"
        "                  32 from `veryfast` through `medium`, 64 at\n"
        "                  `slow` and above. `medium`'s 32 is\n"
        "                  RESOLUTION-AWARE - on a UHD-class picture (luma\n"
        "                  w*h at or above 3840*1600, so 2160p and the\n"
        "                  scope crops of it) it offers 64 instead, and\n"
        "                  with it the depth-0 residual gate that makes\n"
        "                  the 64x64 node affordable there\n"
        "                  (N265_CB64_SKIPGATE, docs/instruments.md): the\n"
        "                  pair is worth four to twelve points of\n"
        "                  BD-VMAF-NEG at 2160p for the same clock at one\n"
        "                  thread and up to 1.10x at auto on a cheap clip.\n"
        "                  The tree is taken only where the planned base\n"
        "                  QP is at or above N265_UHD_QP (31); below it\n"
        "                  medium codes its 32x32 tree. The gate rides with the\n"
        "                  tree and only on that rung - `slow` and\n"
        "                  `slower` code a 64x64 tree at every resolution\n"
        "                  and are not gated. This flag overrides the\n"
        "                  tree, at every resolution, and never the gate.\n"
        "  --keyint <n>    key frame period in frames (default 250)\n"
        "  --open-gop      code the --keyint picture as a CRA, keeping the\n"
        "                  pyramid that straddles it as leading pictures\n"
        "                  (default; --no-open-gop for an IDR there). A\n"
        "                  scene cut is an IDR either way.\n"
        "  --radl <n>      of the CRA's leading pictures, how many are RADL\n"
        "                  rather than RASL - decodable when the decode\n"
        "                  starts at that CRA (default 0)\n"
        "  --bframes <0..15> B frames between anchors, hierarchical (default 7).\n"
        "                  Above 7 the window has to hold the mini-GOP, so\n"
        "                  --rc-lookahead must be at least --bframes + 1.\n"
        "  --aq <0|1>      variance adaptive quantization (default 1)\n"
        "  --rc-lookahead <1..60>  lookahead depth in frames (default 20,\n"
        "                  x265 medium's own; 12 before 2026-09-12).\n"
        "                  Latency is the lookahead depth in frames. The deep\n"
        "                  window (--rc-lookahead 24 --bframes 15) is an\n"
        "                  opt-in mode: 24 frames of latency and a deeper\n"
        "                  DPB, for static or drawn content\n"
        "                  (docs/phase7-gop-design.md).\n"
        "  --cutree <0|1>  lookahead propagation QP offsets (default 1)\n"
        "  --dup-threshold <f>  NEAR-duplicate bound for the frame\n"
        "                  duplicate path, in 8-bit luma levels per lowres\n"
        "                  pixel (default 0). 0 is the strict form and is\n"
        "                  every stream this encoder has written: only a\n"
        "                  source already in the DPB arms the path. Above\n"
        "                  0 a frame within that distance of a reference\n"
        "                  is coded all-skip toward it, which is what\n"
        "                  reaches telecined and frame-doubled content,\n"
        "                  where the repeat is not bit-exact. The distance\n"
        "                  is NOT x265's PSNR threshold and its numbers do\n"
        "                  not carry over; 0.25 and 0.5 are what the\n"
        "                  corpus was measured at (docs/instruments.md).\n"
        "  --threads <n>   worker threads; 0 = one per online CPU (default 0).\n"
        "                  Output is byte-identical at every value.\n"
        "  --gop-parallel <n>  code a long input as segments on n encoder\n"
        "                  instances at once, --threads / n threads each,\n"
        "                  joined at IDRs into one stream (0 = off, the\n"
        "                  default). Only above one thread and past one\n"
        "                  segment (--keyint frames); the stream is not\n"
        "                  the serial one. No HRD timing SEI on a joined\n"
        "                  stream; not with --pass, --qpfile, --zones,\n"
        "                  --csv, --shot-stats or --hrd-concat. A file is\n"
        "                  read by each instance; a pipe is held in a\n"
        "                  window of (n + 1.5) x --keyint frames.\n"
        "  --dump-recon <file>  write reconstruction as raw YUV (the\n"
        "                  conformance gate compares decoders against it)\n"
        "  --shot-window <n>  OPT-IN shot planner: buffer up to n frames so\n"
        "                  the encoder can price a whole shot and offset its\n"
        "                  base QP against the shots before it (0 = off, the\n"
        "                  default; --crf only). LATENCY IS n FRAMES - 2.5 s\n"
        "                  at 60 and 24 fps, 1 s at 60 and 60 fps - and the\n"
        "                  queue holds n more source pictures, about 0.17\n"
        "                  MiB each at CIF, 1.6 at 720p and 3.6 at 1080p, so\n"
        "                  --shot-window 120 takes crowd_run_1080p from 357\n"
        "                  to 717 MB of peak RSS. Max 300. What it buys and\n"
        "                  what it costs the per-shot quality spread:\n"
        "                  docs/phase7-shot-design.md.\n"
        "  Cheap SEIs and sub-layers. Signalling, except the last, which puts a\n"
        "  temporal id in a VCL NAL header. All off by default.\n"
        "  --idr-recovery-sei  a recovery point SEI (D.2.8) at every IRAP:\n"
        "                  recovery_poc_cnt 0, exact_match 1. It is what a\n"
        "                  random-access player looks for.\n"
        "  --pic-struct <n>  Table D.2's pic_struct in the pic_timing SEI\n"
        "                  (0 frame, 7 frame doubling, 8 frame tripling),\n"
        "                  and with it frame_field_info_present_flag in the\n"
        "                  VUI. It carries pulldown and field repetition.\n"
        "                  Needs a buffer: pic_timing is written only where\n"
        "                  the HRD is.\n"
        "  --hrd-concat    the concatenation flag in the first key frame's\n"
        "                  buffering period SEI: this access unit was\n"
        "                  spliced onto another stream, so C.2.3 reads the\n"
        "                  removal delay delta rather than a clock the\n"
        "                  splice broke. Needs a buffer.\n"
        "  --temporal-layers  put the NON-REFERENCE B leaves in temporal\n"
        "                  sub-layer 1: nuh_temporal_id_plus1 2 on their\n"
        "                  slice NALs, sps/vps_max_sub_layers_minus1 1, and\n"
        "                  the profile_tier_level and hrd_parameters tails\n"
        "                  that go with them. A player can then drop the\n"
        "                  sub-layer and halve the frame rate without\n"
        "                  parsing a slice. The leaves are the only\n"
        "                  pictures that CAN go there - 7.4.2.2 forbids a\n"
        "                  lower layer from referencing a higher one, and\n"
        "                  a leaf is referenced by nothing.\n"
        "  --intra-refresh  NOT IMPLEMENTED and refused by name: a\n"
        "                  periodic intra refresh is worth its recovery\n"
        "                  point only if nothing in the refreshed region\n"
        "                  reads an unrefreshed sample, and that reaches\n"
        "                  mode decision (merge and AMVP candidates are\n"
        "                  inherited, not clamped), constrained intra\n"
        "                  prediction (which we do not have) and the plan\n"
        "                  builder. Priced in docs/phase14.md item 15 and\n"
        "                  left for its own item; the refusal prints the\n"
        "                  whole reason.\n"
        "  --scaling-list <s>  NOT IMPLEMENTED and refused by name; the\n"
        "                  message says why (it is a quantiser item, not a\n"
        "                  signalling one).\n"
        "\n"
        "  Rate control, exposed. Every one of these is inert unless you name\n"
        "  it, and each is refused rather than ignored where the mode it\n"
        "  needs is absent.\n"
        "  --crf-min <f>   floor the rate factor --crf may be coded at.\n"
        "  --crf-max <f>   ...and cap it. Both bound the rate factor AFTER\n"
        "                  the buffer's own clamp, which is what makes them\n"
        "                  bounds: under --vbv-maxrate a --crf-max low\n"
        "                  enough to fight the buffer is a way to ask for\n"
        "                  an underflow, and this encoder reports that\n"
        "                  rather than quietly coding past the bound.\n"
        "                  --crf only; under --bitrate use --qpmin/--qpmax.\n"
        "  --qpmin <n>     hard bounds on the frame QP the rate control may\n"
        "  --qpmax <n>     choose, in QpY: 0..51 at 8 bits, -12..51 at\n"
        "                  Main10. They bound\n"
        "                  the LOOP's own answer too, so the integral winds\n"
        "                  against a rate factor it can reach instead of\n"
        "                  against a clamp applied behind its back.\n"
        "  --strict-cbr    hold a CONSTANT rate under --bitrate with a VBV:\n"
        "                  cbr_flag is signalled in the HRD and an access\n"
        "                  unit that came in under its share is padded with\n"
        "                  filler data (NAL 38) rather than by making the\n"
        "                  picture worse. The padding is reported at the end\n"
        "                  of the run; a run that needed none held the rate\n"
        "                  on its own.\n"
        "  --max-ausize-factor <f>  bound ONE access unit at f of the CPB,\n"
        "                  0.5..1.0 (default 1.0, no bound). An oversized\n"
        "                  AU is a hard failure in a hardware decoder\n"
        "                  whatever the buffer says about its neighbours.\n"
        "                  Needs --vbv-maxrate and --vbv-bufsize.\n"
        "  --qpfile <file>  force frame types and QPs from a file, x264's\n"
        "                  format: \"<display frame> <type> [<qp>]\" per\n"
        "                  line, # for a comment. The types are I or K (a\n"
        "                  key frame), i (a non-IDR intra picture) and - .\n"
        "                  P, B and b are REFUSED by name: what they ask\n"
        "                  for is a mini-GOP shape, and the run length\n"
        "                  belongs to the plan builder, not to a file.\n"
        "                  A forced QP is the FRAME's QP, not a base the\n"
        "                  pyramid cascade then moves.\n"
        "  --zones <spec>  x265's syntax: zones separated by /, each\n"
        "                  \"start,end,q=N\" or \"start,end,b=F\", in DISPLAY\n"
        "                  order. q=N pins the frame QP over the range;\n"
        "                  b=F is a bit multiplier and lands as the QP\n"
        "                  offset that buys the same bits, -6 log2 F -\n"
        "                  it moves the quantiser, not the plan, so the\n"
        "                  rest of the file does not give the bits back.\n"
        "                  Overlapping zones are refused.\n"
        "\n"
        "  Bitstream-level NALs. None of them changes a coded sample and all\n"
        "  of them are OFF by default; every one adds or repeats a NAL.\n"
        "  --repeat-headers  VPS/SPS/PPS lead EVERY IRAP access unit, not\n"
        "                  just the first, so a receiver joining mid-stream\n"
        "                  (broadcast, RTP, an HLS segment) has them.\n"
        "  --aud           access unit delimiter NAL (type 35) at the head\n"
        "                  of every access unit; some transport muxers want\n"
        "                  the boundary spelled out rather than inferred.\n"
        "  --eos           end of sequence NAL (36) at the end of every\n"
        "                  coded video sequence: before each IDR after the\n"
        "                  first, and at the end of the stream.\n"
        "  --eob           end of bitstream NAL (37), the last NAL there is.\n"
        "  --info / --no-info  a user data unregistered SEI (D.2.6) naming\n"
        "                  this encoder, its version, its licence, its URL\n"
        "                  and the options this run resolved, wherever the\n"
        "                  parameter sets go - so once per stream, and\n"
        "                  under --repeat-headers once per IRAP, which is\n"
        "                  what x265 does with the same pair. ON by\n"
        "                  default, as x265's is. The payload carries no\n"
        "                  clock, no path and no thread count, so one\n"
        "                  command line writes one stream whatever it is\n"
        "                  run on. --no-info drops the message, and that\n"
        "                  is what a bitstream cmp against a binary older\n"
        "                  than this default is run with. Under --qp and\n"
        "                  --crf the recon is unmoved and --dump-recon is\n"
        "                  a second escape; under --bitrate the message is\n"
        "                  bits the rate control spends, so it is not.\n"
        "  --no-fingerprint  drop the stream fingerprint. It is ON by\n"
        "                  default and it is the identifying mark that\n"
        "                  survives having the SEIs stripped: a signature\n"
        "                  in a merge_idx choice the syntax leaves open,\n"
        "                  costing no bits and moving no sample.\n"
        "                  scripts/fingerprint_check.py reads it back.\n"
        "  --psnr          report PSNR at the end of the run, pooled over\n"
        "                  every coded frame: per plane and a\n"
        "                  pixel-weighted average. Measured on the\n"
        "                  encoder's OWN reconstruction against the\n"
        "                  picture it coded - no decode - which is the\n"
        "                  same samples a decoder puts on the screen,\n"
        "                  because the conformance gate holds the two\n"
        "                  bit-exact. The squared error is pooled over\n"
        "                  frames and the logarithm taken once, which is\n"
        "                  ffmpeg's pooling, so the number matches\n"
        "                  `ffmpeg -lavfi psnr` to a hundredth of a dB.\n"
        "                  Output-neutral: the bitstream is byte-identical\n"
        "                  with it on.\n"
        "  --ssim          the same for SSIM on the luma plane: 8x8\n"
        "                  windows at stride 4, averaged over windows and\n"
        "                  then over frames, which is ffmpeg's ssim\n"
        "                  filter's arithmetic. Also reported in dB.\n"
        "  --csv <file>    per-run or per-frame statistics as CSV.\n"
        "  --csv-log-level <0|1|2>  0 (the default) writes one summary row\n"
        "                  for the run; 1 adds a row per coded frame in\n"
        "                  CODING order (POC, order, type, depth, QP,\n"
        "                  bits, and the metrics above where they were\n"
        "                  asked for); 2 adds the rate control's columns\n"
        "                  (planned bits, VBV fill, VBV dQP, flat). The\n"
        "                  bits column counts the whole access unit,\n"
        "                  parameter sets and SEIs included. No elapsed\n"
        "                  time or fps column, unlike x265's: a clock in\n"
        "                  a log makes two runs of one encode differ.\n"
        "  --shot-stats <file>  per-shot JSON Lines sidecar, one object per\n"
        "                  shot in display order (N265_SHOT_STATS=file does\n"
        "                  the same). Output-neutral: the bitstream is\n"
        "                  byte-identical with it on. Fields and the shot\n"
        "                  definition: docs/instruments.md.\n"
        "  --pass <n>      TWO-PASS, opt-in, with --stats: 1 codes the clip\n"
        "                  and writes a per-frame stats sidecar, 2 reads it\n"
        "                  and plans the whole file before coding a frame,\n"
        "                  3 does both (the plan comes from the file it then\n"
        "                  replaces). Pass 2 needs --bitrate and the same\n"
        "                  source; pass 1 may run any mode and any preset.\n"
        "                  WHOLE-FILE ANALYSIS, so it is opt-in and absent\n"
        "                  the encoder is byte-for-byte the one-pass one.\n"
        "  --stats <file>  the sidecar --pass writes or reads.\n"
        "  --level <l>     signal this level instead of the derived one:\n"
        "                  1, 2, 2.1 ... 6.2 (\"41\" also spells 4.1; the raw\n"
        "                  idc is not a spelling). REFUSED when the stream\n"
        "                  does not fit it - the picture size, the\n"
        "                  per-dimension bound, the declared DPB, the frame\n"
        "                  rate, or the bit rate where one is known. Left\n"
        "                  off, the level is derived from all five (Annex A).\n"
        "  --high-tier     signal the High tier. Only defined from level 4\n"
        "                  up (A.4.1), so on a smaller picture it is what\n"
        "                  takes the level to 4. Under --bitrate the tier\n"
        "                  also rises on its own when no Main tier level\n"
        "                  admits the target.\n"
        "                  NOTE the bit-rate bound is ENFORCED only under\n"
        "                  --bitrate and --pcm, where the encoder knows the\n"
        "                  rate. Under --qp and --crf the rate is whatever\n"
        "                  the content costs, so the level comes from size,\n"
        "                  DPB and frame rate and MaxBR is only REPORTED\n"
        "                  (--verbose prints it).\n"
        "  --vui-timing    write vui_timing_info in the SPS from the y4m (the\n"
        "                  default since 2026-09-12); --no-vui-timing omits it\n"
        "                  frame rate, so a decoder can check the level's\n"
        "                  MaxLumaSr leg against the rate it was derived\n"
        "                  from. Off by default: it adds about a dozen\n"
        "                  bytes to every SPS and Annex A does not ask for it.\n"
        "  --film-grain <s>  estimate the source's film grain and write the\n"
        "                  film_grain_characteristics SEI (H.265 D.2.20), so a\n"
        "                  decoder that implements H.274 re-synthesises grain\n"
        "                  on output. OPT-IN and never a default: it changes\n"
        "                  what a decoder puts on the screen. s is a multiplier\n"
        "                  on the estimated grain std; 1.0 is the estimate as\n"
        "                  measured, 0 is off. 8-bit 4:2:0 only - the only\n"
        "                  synthesiser this project can measure through does\n"
        "                  that shape and no other - and refused loudly\n"
        "                  elsewhere. Latency: none. About twenty bytes per\n"
        "                  key frame.\n"
        "  --film-grain-denoise <0|1>  with --film-grain, also core the grain\n"
        "                  out of the picture the encoder CODES (default 0).\n"
        "                  This is the half that saves bits: the encoder\n"
        "                  spends nothing on grain and the decoder puts it\n"
        "                  back. Without it the SEI only adds grain to a\n"
        "                  picture that already has some.\n"
        "  --nr-intra <0-2000>  coefficient-domain noise reduction in intra\n"
        "                  blocks (--preset medium 100; fast, slow and\n"
        "                  slower 0, off). A per-coefficient-\n"
        "                  POSITION offset, learned from the magnitudes this\n"
        "                  encode has already produced, taken off every\n"
        "                  coefficient toward zero before it is quantised. A\n"
        "                  position that is usually quiet is noise and is\n"
        "                  attenuated; a position that is usually loud is\n"
        "                  structure and is left nearly alone. Opt-in: it\n"
        "                  changes the picture, and what it is worth depends\n"
        "                  on how noisy the source is (docs/phase14.md).\n"
        "                  It overrides the preset and N265_NR_INTRA, so\n"
        "                  --nr-intra 0 --nr-inter 0 is medium's escape.\n"
        "  --nr-inter <0-2000>  the same in inter blocks (medium 100).\n"
        "  --mcstf         motion-compensated temporal filter on the SOURCE\n"
        "                  (default off). Every eighth picture - the ones a\n"
        "                  mini-GOP predicts from - is replaced by a weighted\n"
        "                  average of itself and its motion-compensated\n"
        "                  neighbours, so its own noise averages down while\n"
        "                  its content does not. A neighbour counts less the\n"
        "                  further away in time it is, less where its block\n"
        "                  did not match, and less at any pixel where it\n"
        "                  disagrees by more than noise could explain.\n"
        "                  Costs --mcstf-ref-range frames of latency and a\n"
        "                  full-resolution motion search on the filtered\n"
        "                  pictures. For NOISY sources; on clean content the\n"
        "                  average has nothing to take out.\n"
        "  --mcstf-ref-range <1-4>  neighbours per side (default 2).\n"
        "  --selective-mcstf  skip the filter on a picture whose measured\n"
        "                  noise floor is under N265_MCSTF_CLEAN (default\n"
        "                  1.5 luma levels, placed by the band). Default\n"
        "                  off, as x265's is - but --mcstf without it loses\n"
        "                  8 BD points on flat-shaded animation, so if you\n"
        "                  are filtering a mixed library, name both.\n"
        "  --dither        on the 10-bit-in, 8-bit-out path only: add a 2x2\n"
        "                  ordered dither before dropping the two bits,\n"
        "                  instead of truncating them. Exactly mean-\n"
        "                  preserving over any 2x2, so a gradient arrives as\n"
        "                  a dot pattern at the level it really had rather\n"
        "                  than as a staircase. Needs --output-depth 8 on a\n"
        "                  10-bit source; anywhere else it is refused by\n"
        "                  name, because there is nothing to dither.\n"
        "  --vbv-maxrate <k>  hypothetical decoder buffer: the CPB arrival\n"
        "                  rate in kbit/s. With --vbv-bufsize it holds the\n"
        "                  encode inside a decoder buffer that never\n"
        "                  underflows - under --bitrate as a second\n"
        "                  constraint on the ABR loop, under --crf as\n"
        "                  capped CRF, where the rate factor stands until\n"
        "                  the buffer says otherwise. Both halves or\n"
        "                  neither; refused under --qp, --pcm and\n"
        "                  --lossless, which have no quantiser to move.\n"
        "                  The rate and the buffer are ENFORCED against\n"
        "                  the level's MaxBR and MaxCPB, and the schedule\n"
        "                  is written as hrd_parameters in the VUI.\n"
        "  --vbv-bufsize <k>  the CPB size in kbit (the other half).\n"
        "  --vbv-init <f>  how full the buffer starts, as a fraction of\n"
        "                  --vbv-bufsize in (0, 1]. Default 0.9.\n"
        "  -w, --weightp / --no-weightp\n"
        "                  weighted prediction in P slices: the encoder\n"
        "                  derives one explicit weight per reference from\n"
        "                  the lookahead and signals it where it beats the\n"
        "                  unweighted prediction. It is a tool for FADES\n"
        "                  and lighting changes and does nothing elsewhere -\n"
        "                  the estimator declines, and the cost is the\n"
        "                  table's own handful of bits. On at --preset\n"
        "                  medium and above, x265's default.\n"
        "  --weightb / --no-weightb\n"
        "                  the same in B slices. OFF by default, as in\n"
        "                  x265: see docs/phase14.md for what it is worth\n"
        "                  on the fade clips and what it costs elsewhere.\n"
        "  --hash <0-3>    decoded picture hash SEI, one suffix message per\n"
        "                  picture (H.265 D.2.19): 0 off (the default),\n"
        "                  1 MD5, 2 CRC, 3 checksum - x265's numbering. The\n"
        "                  hash is of the CODED picture, padding included,\n"
        "                  so any decoder can self-check our streams:\n"
        "                  `ffmpeg -err_detect crccheck -v debug` verifies\n"
        "                  the MD5 form and says so per picture.\n"
        "\n"
        "  VUI colour signalling and HDR10 static metadata. All of it is\n"
        "  PASS-THROUGH: it describes the pictures you handed in, changes no\n"
        "  coded sample, and is absent unless you name the flag. Every value\n"
        "  flag takes x265's name or the bare number Annex E gives it.\n"
        "  --video-signal-type-preset <name[:volume]>  one name for a\n"
        "                  whole colour signalling set, in x265's\n"
        "                  spellings and with x265's values: BT601_525,\n"
        "                  BT601_626 (BT601_625 is an alias), BT709_YCC,\n"
        "                  BT709_RGB, BT2020_YCC_NCL, BT2020_RGB,\n"
        "                  BT2100_PQ_YCC, BT2100_PQ_ICTCP, BT2100_PQ_RGB,\n"
        "                  BT2100_HLG_YCC, BT2100_HLG_RGB, FR709_RGB,\n"
        "                  FR2020_RGB, FRP3D65_YCC. It is a NAME TABLE and\n"
        "                  nothing else: it expands into the flags below\n"
        "                  and writes no syntax of its own. An individual\n"
        "                  flag you give beside it WINS over the preset's\n"
        "                  value (x265 discards it instead - a deliberate\n"
        "                  divergence, docs/phase14.md). The optional\n"
        "                  :volume is a mastering display, on the three\n"
        "                  BT2100_PQ_* systems only: P3D65x1000n0005 or\n"
        "                  P3D65x4000n005. x265's third volume is refused\n"
        "                  by name - its red primary does not fit the\n"
        "                  u(16) of ST 2086 - and the refusal says so.\n"
        "  --colorprim <p> colour primaries: bt709, unknown, bt470m,\n"
        "                  bt470bg, smpte170m, smpte240m, film, bt2020,\n"
        "                  smpte428, smpte431, smpte432 (Table E.3)\n"
        "  --transfer <t>  transfer characteristics: bt709, unknown,\n"
        "                  bt470m, bt470bg, smpte170m, smpte240m, linear,\n"
        "                  log100, log316, iec61966-2-4, bt1361e,\n"
        "                  iec61966-2-1, bt2020-10, bt2020-12, smpte2084\n"
        "                  (PQ), smpte428, arib-std-b67 (HLG) (Table E.4)\n"
        "  --colormatrix <m>  matrix coefficients: gbr, bt709, unknown,\n"
        "                  fcc, bt470bg, smpte170m, smpte240m, ycgco,\n"
        "                  bt2020nc, bt2020c, smpte2085, chroma-derived-nc,\n"
        "                  chroma-derived-c, ictcp, ipt-pq-c2 (Table E.5)\n"
        "  --range <r>     limited or full luma and chroma range. Full-range\n"
        "                  content decoded as limited is washed out, and\n"
        "                  nothing but this flag says which it is.\n"
        "  --videoformat <f>  component, pal, ntsc, secam, mac or unknown\n"
        "  --chromaloc <n> chroma sample location 0..5 (E.2.1). 2 is what\n"
        "                  BT.2020 and BT.2100 ask for.\n"
        "  --overscan <o>  unknown (write nothing, the default), show or\n"
        "                  crop: whether the cropped region is appropriate\n"
        "                  to display using overscan\n"
        "  --sar <w:h|n>   sample aspect ratio. A RATIO is written as\n"
        "                  EXTENDED_SAR with the pair after it; a NUMBER is\n"
        "                  the Table E.1 index 0..16. Anamorphic content\n"
        "                  decodes at the wrong shape without it.\n"
        "  --display-window <l,t,r,b>  overscan cropping region in PIXELS.\n"
        "                  The syntax is in chroma samples, so the offsets\n"
        "                  must be multiples of SubWidthC / SubHeightC and\n"
        "                  are divided on the way out - x265 takes the same\n"
        "                  flag in pixels and writes them unshifted, which\n"
        "                  at 4:2:0 signals twice the region it was given.\n"
        "                  Distinct from the conformance window, which is\n"
        "                  this encoder's own padding crop.\n"
        "  --master-display <s>  SMPTE ST 2086 mastering display colour\n"
        "                  volume SEI (D.2.28), in x265's syntax:\n"
        "                  G(x,y)B(x,y)R(x,y)WP(x,y)L(max,min). The eight\n"
        "                  chromaticities are in units of 0.00002 (50000 is\n"
        "                  1.0) and the two luminances in 0.0001 cd/m2, so\n"
        "                  P3D65 at 1000 nits is\n"
        "                  G(13250,34500)B(7500,3000)R(34000,16000)\n"
        "                  WP(15635,16450)L(10000000,5).\n"
        "  --max-cll <c,f> content light level SEI (D.2.35), \"cll,fall\" in\n"
        "                  cd/m2. It says what the CONTENT reaches; nothing\n"
        "                  here measures it, so both numbers are yours.\n"
        "  --hdr10 / --no-hdr10  force the two messages above on or off.\n"
        "                  Left alone, naming either one's values arms them,\n"
        "                  which is x265's rule. They are written in EVERY\n"
        "                  IRAP access unit, so a decoder joining at a\n"
        "                  mid-stream key frame has them.\n"
        "  --no-cll        drop the content light level message and keep\n"
        "                  the mastering display\n"
        "  --atc-sei <t>   alternative transfer characteristics SEI\n"
        "                  (D.2.38): the transfer an SDR decoder should\n"
        "                  assume instead. Same value table as --transfer;\n"
        "                  it is how an HLG stream tells a display it\n"
        "                  cannot reach what to do.\n"
        "  --dhdr10-info <json>  HDR10+ (SMPTE ST 2094-40) dynamic\n"
        "                  metadata, one message per picture, from the JSON\n"
        "                  hdr10plus_tool writes (profile A or B, one\n"
        "                  window). Needs a 10-bit HDR10 encode: --transfer\n"
        "                  smpte2084 --colorprim bt2020 --colormatrix\n"
        "                  bt2020nc.\n"
        "  --dolby-vision-rpu <file>  a Dolby Vision profile 8 RPU per\n"
        "                  picture, from the file dovi_tool writes, copied\n"
        "                  unread as the access unit's last NAL. Each RPU's\n"
        "                  escaping, CRC and profile are checked. Needs\n"
        "                  --dolby-vision-profile, and warns without a VBV.\n"
        "  --dolby-vision-profile <p>  8.1 (an HDR10 base layer: PQ,\n"
        "                  BT.2020), 8.2 (an SDR BT.709 base layer) or 8.4\n"
        "                  (an HLG base layer). It checks the colour flags\n"
        "                  match and the encode is 10-bit 4:2:0 at limited\n"
        "                  range.\n"
        "  Both files are read in display order: entry i goes with input\n"
        "  frame i, and --seek skips entries as it skips frames. A file\n"
        "  that covers fewer frames than were encoded fails the run, after\n"
        "  the stream is written.\n"
        "  --hdr-opt / --no-hdr-opt  medium's HDR tuning on or off. It is\n"
        "                  on by itself where --transfer (or --atc-sei)\n"
        "                  names smpte2084 or arib-std-b67; --hdr-opt\n"
        "                  turns the PQ tuning on for a stream that names\n"
        "                  neither. --hdr10-opt is the same switch.\n"
        "  --verbose, -v   the RESOLVED configuration at open, one\n"
        "                  \"n265 [info]: key : value\" row at a time - what\n"
        "                  the preset, the flags and the N265_* environment\n"
        "                  settled on, x265's log shape so the two read side\n"
        "                  by side - and with it the level and tier chosen\n"
        "                  and every Annex A bound that drove them\n"
        "                  (N265_LEVEL_TRACE=1 is that second line alone)\n"
        "  --show-config   the same table, then exit 0 without encoding.\n"
        "                  Both go to stderr and neither moves a byte of the\n"
        "                  stream; --show-config writes no output file.\n"
        "  --version       print version\n");
}

/* One VUI value flag, parsed the way x265 parses it: a name from the
 * field's own table, or the bare number. A refusal prints the table
 * rather than the shrug a lookup failure would otherwise be - the table
 * lives in the library (src/encoder/signal.c) and is not copied here. */
#define SIGARG(field, var) do { \
    const char *v_ = argv[++i]; \
    (var) = n265_vui_parse((field), v_); \
    if ((var) < 0) { \
        fprintf(stderr, "n265: %s %s is not a value of that field. The " \
                "names are:", argv[i - 1], v_); \
        const char * const *n_ = n265_vui_names(field); \
        for (int k_ = 0; n_ && n_[k_]; k_++) \
            fprintf(stderr, " %s", n_[k_]); \
        fprintf(stderr, " - or the number itself (Annex E's tables " \
                "number them in that order).\n"); \
        return 2; \
    } \
} while (0)

/* ---- the options clause of the identification SEI (phase 14 item 9) ----
 *
 * What goes in the payload is the preset, the rate control the CLI
 * settled on, and the flags this run was actually given, in the order
 * they were given. What stays out is anything that would make one
 * command line write two different streams:
 *
 *   - no PATHS. An input name is not a coding option and would put a
 *     working directory in the bitstream.
 *   - no THREAD COUNT. `--threads 1` and `--threads 12` must produce the
 *     same bytes; that identity is a gate (CLAUDE.md), and printing the
 *     count would break it on every clip.
 *   - no CLOCK, no environment. The env knobs are measurement ARMS and
 *     the preset already names the ones a preset sets.
 *   - nothing byte-neutral that only reports (`--psnr`, `--csv`,
 *     `--dump-recon`): those are documented as costing the stream
 *     nothing, and they must keep costing it nothing.
 *
 * The flags are collected from argv itself rather than from a table of
 * option names, so this cannot drift as flags are added: the parse loop
 * records how many tokens each iteration consumed, which is exactly the
 * knowledge of "does this one take a value" the loop already has. */
typedef struct {
    char s[640];
    size_t n;
} optstr;

static void opt_put(optstr *o, const char *key, const char *val)
{
    int n = snprintf(o->s + o->n, sizeof o->s - o->n, "%s%s%s%s",
                     o->n ? " " : "", key, val ? "=" : "", val ? val : "");
    if (n > 0 && (size_t)n < sizeof o->s - o->n)
        o->n += (size_t)n;
    else                        /* full: keep what fits and stop growing */
        o->s[o->n] = '\0';
}

/* the three filters, by the option's name with its dashes stripped */
static int opt_drop(const char *k)
{
    static const char * const drop[] = {
        "o", "dump-recon", "dump-recon-decode", "csv", "csv-log-level",
        "shot-stats", "psnr", "ssim", "verbose", "v", "threads",
        /* a schedule, like --threads: the identification SEI must not
         * change with it */
        "gop-parallel",
        "show-config", "version", "help", "h",
        /* the switch for this very message: naming a default must not
         * change the stream, so `--info` and a default run write the
         * same bytes and `--no-info` writes no message to name itself
         * in */
        "info", "no-info",
        /* named in the resolved head instead, so they are not echoed
         * twice */
        "preset", "qp", "crf", "bitrate", NULL
    };
    for (int i = 0; drop[i]; i++)
        if (!strcmp(k, drop[i]))
            return 1;
    return 0;
}

static int opt_drop_value(const char *k)
{
    /* the flag shapes the stream and its value is a file name */
    return !strcmp(k, "qpfile") || !strcmp(k, "stats") ||
           !strcmp(k, "dhdr10-info") || !strcmp(k, "dolby-vision-rpu");
}

/* one iteration of the parse loop: argv[from] is the option and the loop
 * left `to` at the last token it consumed for it. */
static void opt_note(optstr *o, char **argv, int from, int to)
{
    const char *a = argv[from];
    if (a[0] != '-' || !a[1])   /* the input file */
        return;
    const char *k = a + 1;
    if (*k == '-')
        k++;
    if (!strcmp(k, "w"))        /* the two short aliases that are coding
                                 * options; print the long names */
        k = "weightp";
    if (!strcmp(k, "t"))
        k = "tune";
    if (opt_drop(k))
        return;
    if (to > from && !opt_drop_value(k))
        opt_put(o, k, argv[to]);
    else
        opt_put(o, k, NULL);
}

/* ---------------------------------------------------------------------
 * GOP-parallel encoding (phase 14 item 42, parity plan C10).
 *
 * `--gop-parallel N` cuts a long input into SEGMENTS, codes each one on
 * its own encoder instance - N of them at a time, each with --threads / N
 * worker threads - and joins their streams in order. Every segment opens
 * on an IDR, because it is the first picture its instance codes, so the
 * join is a closed GOP and the joined stream is one conformant bitstream:
 * a decoder crossing it sees an IDR and nothing before it is referenced.
 * Inside a segment nothing changes, open GOP included.
 *
 * Where a segment ends. A segment is at most `N265_GOP_LEN` frames (the
 * keyint by default), so an instance never reaches its own keyint and
 * the stream carries about as many intra pictures as the serial encode
 * would. Inside the last 40% of that span the boundary goes on the
 * strongest scene cut the reader finds there, which is where the serial
 * encoder would have coded an IDR anyway; without one it falls on the
 * span's end. The detector is the lookahead's shape on a coarser plane:
 * the ratio of the motion-searched inter cost to the intra cost, a cut
 * where inter fails to undercut intra, spikes over its running level and
 * jumps over the three frames before it.
 *
 * The rate control. CRF and CQP are open loop: a segment is coded at the
 * same rate factor the serial encode would use, and nothing else is
 * needed. Under --bitrate one rate factor is carried across instances:
 * segment k continues segment k - N's controller (its scales, its
 * operating point, its rate factor), and its ledger is the bits really
 * spent by every segment up to k - N plus the target for the segments
 * still in flight between them, so the file still lands on its size.
 * Under a VBV each segment after the first assumes `N265_GOP_VBV_IN` of
 * the buffer at its first picture, and each one before the last must
 * leave `N265_GOP_VBV_OUT` (at least as much) when it ends; the fill
 * arithmetic is monotone, so the joined stream cannot underflow where
 * every segment met its floor (src/encoder/vbv.h).
 *
 * What changes at a join. The parameter sets repeat (each instance writes
 * its own, the same bytes); the identification SEI is written once, by
 * the first segment, as in a serial stream; an --eob is kept on the last
 * segment only. The HRD timing SEI is NOT written on a joined stream:
 * each instance would count its removal delays from its own start.
 * The buffer itself is honored and scripts/vbv_check.py walks it from
 * the stream's access unit sizes.
 *
 * When it engages. Only above one thread, and only when the input runs
 * past one segment: an input that ends inside the first segment is coded
 * by one instance with every thread, which is the serial encode byte for
 * byte. Off unless asked for.
 *
 * Memory (phase 14 item 45). A regular file whose frames are records of
 * one size (raw, or a y4m whose every frame header is a bare "FRAME") is
 * read by each instance itself: it opens the file, seeks to its
 * segment's first record and reads one frame at a time, so no picture is
 * held outside the encoders. The boundary search then only needs the
 * luma plane, which the reader reads on its own and downsamples; it keeps
 * a byte per frame. A pipe, or a y4m with per-frame parameters, can only
 * be read in order while N instances consume it out of order, so there
 * the reader holds a window of input frames, `(N + 1.5) x` the segment
 * length by default (N265_GOP_WINDOW overrides): the segments in flight
 * plus the look-ahead the next boundary needs. `N265_GOP_SEEK=0` takes
 * the window on a file too. The two read the same samples, so they code
 * the same stream. */

void n265_encoder_set_segment(n265_encoder *e, double fill_in,
                              double floor_out, int nframes);
size_t n265_encoder_rc_size(void);
int n265_encoder_rc_export(const n265_encoder *e, void *buf);
int n265_encoder_rc_import(n265_encoder *e, const void *buf,
                           double bits_before, long frames_before);
int n265_encoder_rc_spent(const n265_encoder *e, double *bits, long *frames);
void n265_encoder_set_segment_10(n265_encoder *e, double fill_in,
                                 double floor_out, int nframes);
size_t n265_encoder_rc_size_10(void);
int n265_encoder_rc_export_10(const n265_encoder *e, void *buf);
int n265_encoder_rc_import_10(n265_encoder *e, const void *buf,
                              double bits_before, long frames_before);
int n265_encoder_rc_spent_10(const n265_encoder *e, double *bits,
                             long *frames);
int n265_encoder_crf_anchor(const n265_encoder *e, double *lvl);
int n265_encoder_set_crf_anchor(n265_encoder *e, double lvl);
int n265_encoder_crf_anchor_10(const n265_encoder *e, double *lvl);
int n265_encoder_set_crf_anchor_10(n265_encoder *e, double lvl);

typedef struct gp_api {
    void (*set_segment)(n265_encoder *, double, double, int);
    size_t (*rc_size)(void);
    int (*rc_export)(const n265_encoder *, void *);
    int (*rc_import)(n265_encoder *, const void *, double, long);
    int (*rc_spent)(const n265_encoder *, double *, long *);
    int (*crf_anchor)(const n265_encoder *, double *);
    int (*set_crf_anchor)(n265_encoder *, double);
} gp_api;

static const gp_api gp8 = {
    n265_encoder_set_segment, n265_encoder_rc_size, n265_encoder_rc_export,
    n265_encoder_rc_import, n265_encoder_rc_spent, n265_encoder_crf_anchor,
    n265_encoder_set_crf_anchor,
};
static const gp_api gp10 = {
    n265_encoder_set_segment_10, n265_encoder_rc_size_10,
    n265_encoder_rc_export_10, n265_encoder_rc_import_10,
    n265_encoder_rc_spent_10, n265_encoder_crf_anchor_10,
    n265_encoder_set_crf_anchor_10,
};

typedef struct gp_seg {
    int start, end;             /* input frames [start, end) */
    int has_next;               /* a segment follows this one */
    int threads;                /* the instance's worker threads */
    uint8_t *buf;               /* the segment's Annex B bytes */
    size_t sz, cap;
    FILE *recon;                /* --dump-recon's per-segment planes */
    double met[6];
    long nmet;
    unsigned char *rc;          /* the controller after the flush */
    double rc_own;              /* bits this segment spent itself */
    int done, err, written;
} gp_seg;

typedef struct gp_job {
    const n265_api *a;
    const gp_api *g;
    n265_param p;
    int W, K, T;                /* instances, threads each, total */
    int L;                      /* the segment's most frames */
    int cuts;                   /* boundaries on scene cuts */
    long window;                /* input frames held at most */
    int bps;                    /* bytes per sample handed to the library */
    int pw[3], ph[3];           /* plane geometry, samples */
    size_t psz[3];              /* plane bytes */
    int abr, vbv, repeat_hdr, want_dump, want_met;
    double bpf;                 /* ABR target bits per frame */
    double vbv_in, vbv_out;
    pthread_mutex_t lock;       /* everything below */
    pthread_cond_t cv;
    pthread_mutex_t open_lock;  /* encoder open/close, one at a time */
    uint8_t **frm;              /* frame i's planes, or NULL */
    long frm_cap;
    long n_read, resident;
    int eof;
    gp_seg **seg;               /* each its own allocation: a worker
                                 * holds its segment across a grow */
    int nseg, seg_cap, final;   /* `final`: the last segment is known */
    int next;                   /* the next segment to hand out */
    int abort_;
    /* the CRF opening level, published by segment 0 (0: not yet, 1: in
     * `anchor`, -1: none to carry) */
    int anchor_state;
    double anchor;
    /* the seekable reader (item 45): each instance reads its own records */
    int seek;
    const char *path;
    off_t base, rec;            /* the first record's offset, a record */
    long total;                 /* records in the file past `base` */
    const y4m_t *src;           /* the geometry an instance's reader copies */
    long clipped;               /* the instances' clamp counts */
    int relief;                 /* N265_GOP_RELIEF */
    int kcap;                   /* N265_GOP_K: frames in flight, at most */
} gp_job;

/* --dhdr10-info and --dolby-vision-rpu, read at start-up and indexed from
 * the input's first frame. An encoder numbers its pictures from 0, so
 * --seek, and each --gop-parallel segment on top of it, hands the library
 * the part of each table that starts at its own first picture. */
static n265_dyn_tab g_h10p_tab, g_rpu_tab;
static long g_dyn_seek;
static long g_dyn_lost;     /* every encoder's NAL losses, under open_lock */

static void dyn_apply(const n265_api *a, long from)
{
    long at = g_dyn_seek + from;
    const n265_dyn_tab *h = &g_h10p_tab, *r = &g_rpu_tab;
    if (h->n > at)
        a->set_hdr10plus((int)(h->n - at),
                         (const uint8_t *const *)h->buf + at, h->len + at);
    else
        a->set_hdr10plus(0, NULL, NULL);
    if (r->n > at)
        a->set_dovi_rpu((int)(r->n - at),
                        (const uint8_t *const *)r->buf + at, r->len + at);
    else
        a->set_dovi_rpu(0, NULL, NULL);
}

/* After the encode. A NAL the library lost fails the run, and so does a
 * picture with no metadata, which breaks HDR10+ or Dolby Vision playback
 * at that picture (the stream is still written either way). Metadata left
 * over is only a warning: a --frames run over part of a title is the usual
 * reason. */
static int dyn_check(long frames)
{
    const n265_dyn_tab *t[2] = { &g_h10p_tab, &g_rpu_tab };
    const char *flag[2] = { "--dhdr10-info", "--dolby-vision-rpu" };
    int rc = g_dyn_lost > 0;    /* the library has said what was lost */
    for (int k = 0; k < 2; k++) {
        if (!t[k]->n)
            continue;
        long have = t[k]->n - g_dyn_seek;
        if (have < 0)
            have = 0;
        if (have < frames) {
            fprintf(stderr, "n265: ERROR %s covers %ld of the %ld frames "
                    "encoded; the last %ld have none\n", flag[k], have,
                    frames, frames - have);
            rc = 1;
        } else if (have > frames) {
            fprintf(stderr, "n265: WARNING %s covers %ld frames and %ld "
                    "were encoded\n", flag[k], have, frames);
        }
    }
    return rc;
}

/* the knobs are read by name at the call, so the census can find them */
static int gp_env_int(const char *v, int def)
{
    return v ? atoi(v) : def;
}

static double gp_env_num(const char *v, double def)
{
    return v ? atof(v) : def;
}

/* ---- the boundary detector (the reader's, on a coarse luma plane) ---- */

typedef struct gp_sc {
    int f, lw, lh, bw, bh;      /* downsample factor, plane, 8x8 blocks */
    uint8_t *cur, *prev;
    int have_prev;
    double ema;                 /* the running level of the ratio */
    double rprev[3];
    int rn;
} gp_sc;

static int gp_sc_init(gp_sc *s, int w, int h)
{
    memset(s, 0, sizeof *s);
    s->f = w > 2048 ? 8 : 4;
    s->lw = w / s->f;
    s->lh = h / s->f;
    s->bw = s->lw / 8;
    s->bh = s->lh / 8;
    if (s->bw < 1 || s->bh < 1)
        return -1;
    s->cur = malloc((size_t)s->lw * s->lh);
    s->prev = malloc((size_t)s->lw * s->lh);
    return s->cur && s->prev ? 0 : -1;
}

static void gp_sc_free(gp_sc *s)
{
    free(s->cur);
    free(s->prev);
}

static uint32_t gp_sad8(const uint8_t *a, const uint8_t *b, int st)
{
    uint32_t s = 0;
    for (int y = 0; y < 8; y++, a += st, b += st)
        for (int x = 0; x < 8; x++)
            s += (uint32_t)abs((int)a[x] - (int)b[x]);
    return s;
}

/* The cut ratio of the frame just pushed: inter cost over intra cost,
 * both summed over the 8x8 blocks of the coarse plane, the inter cost
 * per block never above the intra one (an encoder would code it intra).
 * -1 for the first frame and for a picture with nothing to measure. */
static double gp_sc_push(gp_sc *s, const void *luma, int stride, int bps)
{
    int f = s->f;
    for (int y = 0; y < s->lh; y++)
        for (int x = 0; x < s->lw; x++) {
            uint32_t acc = 0;
            for (int j = 0; j < f; j++) {
                if (bps == 1) {
                    const uint8_t *r = (const uint8_t *)luma +
                                       (size_t)(y * f + j) * stride + x * f;
                    for (int i = 0; i < f; i++)
                        acc += r[i];
                } else {
                    const uint16_t *r = (const uint16_t *)luma +
                                        (size_t)(y * f + j) * stride + x * f;
                    for (int i = 0; i < f; i++)
                        acc += r[i] >> 2;
                }
            }
            s->cur[y * s->lw + x] = (uint8_t)(acc / (uint32_t)(f * f));
        }
    double ratio = -1.0;
    if (s->have_prev) {
        uint64_t ic = 0, pc = 0;
        int st = s->lw;
        for (int by = 0; by < s->bh; by++)
            for (int bx = 0; bx < s->bw; bx++) {
                int x0 = bx * 8, y0 = by * 8;
                const uint8_t *c = s->cur + y0 * st + x0;
                uint32_t sum = 0;
                for (int y = 0; y < 8; y++)
                    for (int x = 0; x < 8; x++)
                        sum += c[y * st + x];
                int m = (int)((sum + 32) >> 6);
                uint32_t icb = 0;
                for (int y = 0; y < 8; y++)
                    for (int x = 0; x < 8; x++)
                        icb += (uint32_t)abs((int)c[y * st + x] - m);
                uint32_t best = UINT32_MAX;
                for (int dy = -3; dy <= 3; dy++)
                    for (int dx = -3; dx <= 3; dx++) {
                        int px = x0 + dx, py = y0 + dy;
                        if (px < 0 || py < 0 || px + 8 > s->lw ||
                            py + 8 > s->lh)
                            continue;
                        uint32_t d = gp_sad8(c, s->prev + py * st + px, st);
                        if (d < best)
                            best = d;
                    }
                ic += icb + 64;     /* a block's floor, so flat is not 0/0 */
                pc += (best < icb ? best : icb) + 64;
            }
        ratio = (double)pc / (double)ic;
    }
    uint8_t *t = s->prev;
    s->prev = s->cur;
    s->cur = t;
    s->have_prev = 1;
    return ratio;
}

/* the lookahead's three tests, on the ratio just measured */
static int gp_sc_cut(gp_sc *s, double r)
{
    if (r < 0.0)
        return 0;
    int cut = r >= 0.75 && (s->ema == 0.0 || r >= 1.8 * s->ema);
    if (cut && s->rn > 0) {
        double m = s->rprev[0];
        for (int k = 1; k < s->rn; k++)
            if (s->rprev[k] > m)
                m = s->rprev[k];
        if (!(r - m >= 0.10 || 1.0 - m >= 2.0 * (1.0 - r)))
            cut = 0;
    }
    /* a cut does not set the level it was measured against */
    if (!cut)
        s->ema = s->ema == 0.0 ? r : s->ema + 0.1 * (r - s->ema);
    s->rprev[2] = s->rprev[1];
    s->rprev[1] = s->rprev[0];
    s->rprev[0] = r;
    if (s->rn < 3)
        s->rn++;
    return cut;
}

/* ---- the seekable reader (item 45) ---- */

/* Is the input a file of fixed-size records from where it stands? Raw
 * input is when its length divides; a y4m is when its first frame header
 * is a bare "FRAME" and the rest divides by that record. Each instance's
 * y4m_read still checks every header it reads, so a file that divides by
 * accident fails loudly instead of coding the wrong samples. */
static int gp_seekable(const y4m_t *y, off_t *base, off_t *rec, long *total)
{
    struct stat st;
    int fd = fileno(y->f);
    if (fd < 0 || fstat(fd, &st) != 0 || !S_ISREG(st.st_mode))
        return 0;
    off_t pos = ftello(y->f);
    if (pos < 0 || st.st_size <= pos)
        return 0;
    off_t h = 0;
    if (!y->raw) {
        char line[256];
        if (!fgets(line, sizeof line, y->f))
            return 0;
        int ok = !strcmp(line, "FRAME\n");
        if (fseeko(y->f, pos, SEEK_SET) != 0 || !ok)
            return 0;
        h = 6;
    }
    off_t r = h + (off_t)y->frame_bytes;
    if ((st.st_size - pos) % r)
        return 0;
    *base = pos;
    *rec = r;
    *total = (long)((st.st_size - pos) / r);
    return 1;
}

/* An instance's own reader: the source's geometry and conversion, its
 * own buffers and its own handle, standing at record `start`. */
static int gp_reader_open(const gp_job *j, y4m_t *r, int start)
{
    const y4m_t *y = j->src;
    memset(r, 0, sizeof *r);
    r->w = y->w;
    r->h = y->h;
    r->fps_num = y->fps_num;
    r->fps_den = y->fps_den;
    r->depth = y->depth;
    r->out_depth = y->out_depth;
    r->csp = y->csp;
    r->raw = y->raw;
    r->dither = y->dither;
    r->f = fopen(j->path, "rb");
    if (!r->f)
        return -1;
    if (src_geom(r, "") != 0)
        return -1;
    return fseeko(r->f, j->base + (off_t)start * j->rec, SEEK_SET);
}

static void gp_reader_close(y4m_t *r)
{
    free(r->frame);
    free(r->pix16);
    free(r->pix8);
    if (r->f)
        fclose(r->f);
    memset(r, 0, sizeof *r);
}

/* The luma plane of record `i`, converted as y4m_read converts it (so the
 * boundary search sees the samples the window path saw), into y's own
 * buffers; returns the plane, or NULL. The clamp is counted by the
 * instances, which read every plane. */
static const void *gp_scan_luma(const gp_job *j, y4m_t *y, long i)
{
    size_t luma = (size_t)y->w * y->h;
    size_t lb = luma * (y->depth > 8 ? 2 : 1);
    if (fseeko(y->f, j->base + (off_t)i * j->rec, SEEK_SET) != 0)
        return NULL;
    if (!y->raw) {
        char line[8];
        if (fread(line, 1, 6, y->f) != 6 || memcmp(line, "FRAME\n", 6))
            return NULL;
    }
    if (fread(y->frame, 1, lb, y->f) != lb)
        return NULL;
    if (y->out_depth == 8 && y->depth == 8)
        return y->frame;
    if (y->out_depth == 8) {
        static const unsigned bayer2[2][2] = { { 0, 2 }, { 3, 1 } };
        for (int py = 0; py < y->h; py++) {
            const uint8_t *r = y->frame + 2 * (size_t)py * y->w;
            uint8_t *d = y->pix8 + (size_t)py * y->w;
            for (int px = 0; px < y->w; px++) {
                unsigned v = (unsigned)r[2 * px] |
                             ((unsigned)r[2 * px + 1] << 8);
                if (v > 1023u)
                    v = 1023u;
                if (y->dither)
                    v += bayer2[py & 1][px & 1];
                v >>= 2;
                d[px] = (uint8_t)(v > 255u ? 255u : v);
            }
        }
        return y->pix8;
    }
    if (y->depth == 8) {
        for (size_t k = 0; k < luma; k++)
            y->pix16[k] = (uint16_t)(y->frame[k] << 2);
        return y->pix16;
    }
    for (size_t k = 0; k < luma; k++) {
        unsigned v = (unsigned)y->frame[2 * k] |
                     ((unsigned)y->frame[2 * k + 1] << 8);
        if (v > (unsigned)N265_SAMPLE_MAX(y->depth))
            v = (unsigned)N265_SAMPLE_MAX(y->depth);
        y->pix16[k] = (uint16_t)v;
    }
    return y->pix16;
}

/* ---- the segment table ---- */

static int gp_seg_push(gp_job *j, int start, int end, int has_next)
{
    if (j->nseg == j->seg_cap) {
        int nc = j->seg_cap ? 2 * j->seg_cap : 64;
        gp_seg **t = realloc(j->seg, (size_t)nc * sizeof *t);
        if (!t)
            return -1;
        j->seg = t;
        j->seg_cap = nc;
    }
    gp_seg *s = calloc(1, sizeof *s);
    if (!s)
        return -1;
    j->seg[j->nseg] = s;
    s->threads = j->K;
    s->start = start;
    s->end = end;
    s->has_next = has_next;
    j->nseg++;
    return 0;
}

static void gp_append(gp_seg *s, const uint8_t *d, size_t n)
{
    if (s->sz + n > s->cap) {
        size_t nc = s->cap ? s->cap : 1 << 20;
        while (s->sz + n > nc)
            nc *= 2;
        uint8_t *b = realloc(s->buf, nc);
        if (!b) {
            s->err = 1;
            return;
        }
        s->buf = b;
        s->cap = nc;
    }
    memcpy(s->buf + s->sz, d, n);
    s->sz += n;
}

/* the NAL header's type, past an Annex B start code of 3 or 4 bytes */
static int gp_nal_type(const n265_nal *n, int *hdr)
{
    const uint8_t *b = n->payload;
    int o = n->size > 3 && b[2] == 1 ? 3 : 4;
    *hdr = o;
    return o < n->size ? (b[o] >> 1) & 0x3f : -1;
}

static void gp_put_nals(gp_job *j, gp_seg *s, int seg_idx, const n265_nal *nal,
                        int nnal, int *first_au)
{
    for (int i = 0; i < nnal; i++) {
        int o, t = gp_nal_type(&nal[i], &o);
        /* the identification SEI, once per stream (or once per IRAP under
         * --repeat-headers, which every segment then keeps): a prefix SEI
         * whose first message is user_data_unregistered */
        if (seg_idx > 0 && *first_au && !j->repeat_hdr &&
            t == N265_NAL_PREFIX_SEI && o + 2 < nal[i].size &&
            nal[i].payload[o + 2] == 5)
            continue;
        if (t == N265_NAL_EOB && s->has_next)
            continue;
        if (t >= 0 && t < 32)
            *first_au = 0;
        gp_append(s, nal[i].payload, (size_t)nal[i].size);
    }
}

/* Hand a closed instance's memory back. The allocator keeps what an
 * encoder frees in the magazines of the threads that freed it, and the
 * next instance's threads allocate fresh, so without this every segment
 * after the first N adds its encoder's footprint to the peak. */
static void gp_relief(int on)
{
#ifdef __APPLE__
    if (on)
        malloc_zone_pressure_relief(NULL, 0);
#endif
}

typedef struct gp_arg {
    gp_job *j;
} gp_arg;

static void *gp_worker(void *arg)
{
    gp_job *j = ((gp_arg *)arg)->j;
    const n265_api *a = j->a;
    size_t rcsz = j->g->rc_size();
    for (;;) {
        pthread_mutex_lock(&j->lock);
        while (!j->abort_ && j->next >= j->nseg && !j->final)
            pthread_cond_wait(&j->cv, &j->lock);
        if (j->abort_ || j->next >= j->nseg) {
            pthread_mutex_unlock(&j->lock);
            break;
        }
        int g = j->next++;
        gp_seg *s = j->seg[g];
        int start = s->start, end = s->end, has_next = s->has_next;
        int nthr = s->threads;
        /* ABR: continue segment g - W's controller, after every segment
         * up to it has finished, so the ledger holds their real bits */
        int pred = j->abr ? g - j->W : -1;
        double bits_before = 0.0;
        if (pred >= 0) {
            for (int k = 0; k <= pred; k++)
                while (!j->abort_ && !j->seg[k]->done)
                    pthread_cond_wait(&j->cv, &j->lock);
            for (int k = 0; k <= pred; k++)
                bits_before += j->seg[k]->rc_own;
            for (int k = pred + 1; k < g; k++)
                bits_before += j->bpf * (j->seg[k]->end - j->seg[k]->start);
        }
        gp_seg *ps = pred >= 0 ? j->seg[pred] : NULL;
        int abort_now = j->abort_;
        pthread_mutex_unlock(&j->lock);
        if (abort_now)
            break;

        pthread_mutex_lock(&j->open_lock);
        a->set_threads(nthr);
        dyn_apply(a, start);
        n265_encoder *e = a->encoder_open(&j->p);
        pthread_mutex_unlock(&j->open_lock);
        if (!e) {
            pthread_mutex_lock(&j->lock);
            s->err = 1;
            s->done = 1;
            j->abort_ = 1;
            pthread_cond_broadcast(&j->cv);
            pthread_mutex_unlock(&j->lock);
            break;
        }
        if (j->vbv && (g > 0 || has_next))
            j->g->set_segment(e, g > 0 ? j->vbv_in : -1.0,
                              has_next ? j->vbv_out : 0.0, end - start);
        if (ps && ps->rc)
            j->g->rc_import(e, ps->rc, bits_before, start);
        if (j->want_dump) {
            s->recon = tmpfile();
            if (s->recon)
                a->set_dump(e, s->recon);
            else
                s->err = 1;
        }
        /* CRF: the stream's opening level, from segment 0 (see above) */
        if (g > 0) {
            pthread_mutex_lock(&j->lock);
            while (!j->abort_ && j->anchor_state == 0)
                pthread_cond_wait(&j->cv, &j->lock);
            int st = j->anchor_state;
            double lvl = j->anchor;
            pthread_mutex_unlock(&j->lock);
            if (st > 0)
                j->g->set_crf_anchor(e, lvl);
        }
        int first_au = 1;
        y4m_t rd;
        memset(&rd, 0, sizeof rd);
        if (j->seek && gp_reader_open(j, &rd, start) != 0) {
            fprintf(stderr, "n265: cannot read %s at frame %d\n", j->path,
                    start);
            s->err = 1;
        }
        for (int i = start; i < end && !s->err; i++) {
            n265_picture pic = {
                .stride = { j->pw[0], j->pw[1], j->pw[2] },
                .pts = i - start,
            };
            if (j->seek) {
                /* every record in [start, end) exists: a segment is only
                 * placed over frames the reader has counted */
                pthread_mutex_lock(&j->lock);
                int ab = j->abort_;
                pthread_mutex_unlock(&j->lock);
                if (ab || y4m_read(&rd) <= 0) {
                    if (!ab)
                        fprintf(stderr, "n265: cannot read frame %d of "
                                "%s\n", i, j->path);
                    s->err = 1;
                    break;
                }
                for (int c = 0; c < 3; c++)
                    pic.plane[c] = rd.plane[c];
            } else {
                pthread_mutex_lock(&j->lock);
                while (!j->abort_ && j->n_read <= i)
                    pthread_cond_wait(&j->cv, &j->lock);
                uint8_t *f = j->frm[i];
                int ab = j->abort_;
                pthread_mutex_unlock(&j->lock);
                if (ab || !f) {
                    s->err = 1;
                    break;
                }
                pic.plane[0] = f;
                pic.plane[1] = f + j->psz[0];
                pic.plane[2] = f + j->psz[0] + j->psz[1];
            }
            n265_nal *nal;
            int nnal;
            if (a->encoder_encode(e, &nal, &nnal, &pic) < 0) {
                fprintf(stderr, "n265: encode failed on frame %d\n", i);
                s->err = 1;
                break;
            }
            gp_put_nals(j, s, g, nal, nnal, &first_au);
            double lvl = 0.0;
            int ast = g == 0 && j->anchor_state == 0
                          ? j->g->crf_anchor(e, &lvl) : 0;
            pthread_mutex_lock(&j->lock);
            if (ast) {
                j->anchor_state = ast;
                j->anchor = lvl;
            }
            if (!j->seek) {
                free(j->frm[i]);
                j->frm[i] = NULL;
                j->resident--;
            }
            pthread_cond_broadcast(&j->cv);
            pthread_mutex_unlock(&j->lock);
        }
        if (j->seek) {
            pthread_mutex_lock(&j->lock);
            j->clipped += rd.clipped;
            pthread_mutex_unlock(&j->lock);
            gp_reader_close(&rd);
        }
        while (!s->err) {
            n265_nal *nal;
            int nnal;
            int r2 = a->encoder_encode(e, &nal, &nnal, NULL);
            if (r2 <= 0 && nnal == 0)
                break;
            gp_put_nals(j, s, g, nal, nnal, &first_au);
        }
        if (g == 0) {
            /* the window can close in the flush, or never (a segment
             * shorter than it): publish whatever there is */
            double lvl = 0.0;
            int ast = j->g->crf_anchor(e, &lvl);
            pthread_mutex_lock(&j->lock);
            if (j->anchor_state == 0) {
                j->anchor_state = ast > 0 ? 1 : -1;
                j->anchor = lvl;
            }
            pthread_cond_broadcast(&j->cv);
            pthread_mutex_unlock(&j->lock);
        }
        if (j->want_met)
            s->nmet = a->metrics(e, s->met);
        if (j->abr) {
            s->rc = malloc(rcsz);
            double spent = 0.0;
            long fr = 0;
            if (s->rc && j->g->rc_export(e, s->rc) < 0) {
                free(s->rc);
                s->rc = NULL;
            }
            if (j->g->rc_spent(e, &spent, &fr) == 0)
                s->rc_own = spent - (pred >= 0 ? bits_before : 0.0);
        }
        pthread_mutex_lock(&j->open_lock);
        /* an aborted segment skipped its drain and fails the run already;
         * its frames still in flight are not a loss to report */
        if (!s->err)
            g_dyn_lost += a->dyn_lost(e);
        a->encoder_close(e);
        gp_relief(j->relief);
        pthread_mutex_unlock(&j->open_lock);
        pthread_mutex_lock(&j->lock);
        s->done = 1;
        if (s->err)
            j->abort_ = 1;
        pthread_cond_broadcast(&j->cv);
        pthread_mutex_unlock(&j->lock);
    }
    return NULL;
}

/* write every finished segment that is next in order; the lock is held
 * on entry and on return, and dropped around the writes */
static int gp_flush(gp_job *j, int *next_out, FILE *fout, FILE *fdump)
{
    int bad = 0;
    while (*next_out < j->nseg && j->seg[*next_out]->done) {
        gp_seg *s = j->seg[*next_out];
        pthread_mutex_unlock(&j->lock);
        if (s->err)
            bad = 1;
        if (!bad && s->sz && fwrite(s->buf, 1, s->sz, fout) != s->sz)
            bad = 1;
        free(s->buf);
        s->buf = NULL;
        if (s->recon) {
            if (!bad && fdump) {
                char tmp[1 << 16];
                size_t got;
                rewind(s->recon);
                while ((got = fread(tmp, 1, sizeof tmp, s->recon)) > 0)
                    if (fwrite(tmp, 1, got, fdump) != got) {
                        bad = 1;
                        break;
                    }
            }
            fclose(s->recon);
            s->recon = NULL;
        }
        pthread_mutex_lock(&j->lock);
        s->written = 1;
        (*next_out)++;
        if (bad) {
            j->abort_ = 1;
            pthread_cond_broadcast(&j->cv);
            break;
        }
    }
    return bad;
}

/* Decide the segment that starts at `b`, if the frames read allow it.
 * `cut[i]` is 1 where frame i opens a new shot. Returns 1 when a segment
 * was pushed. */
static int gp_place(gp_job *j, const unsigned char *cut, int b)
{
    int L = j->L;
    int lo = b + (L * 6 + 9) / 10;
    long need = (long)b + L + L / 4 + 1;
    if (!j->eof && j->n_read < need)
        return 0;
    if (j->eof && j->n_read - b <= L + L / 4) {
        if (j->n_read > b)
            gp_seg_push(j, b, (int)j->n_read, 0);
        j->final = 1;
        return 1;
    }
    int nb = b + L, at_cut = 0;
    if (j->cuts)
        for (int i = b + L; i >= lo; i--)
            if (cut[i]) {
                nb = i;
                at_cut = 1;
                break;
            }
    gp_seg_push(j, b, nb, 1);
    if (getenv("N265_GOP_TRACE"))
        fprintf(stderr, "GOP,%d,%d,%d,%s\n", j->nseg - 1, b, nb,
                at_cut ? "cut" : "span");
    return 1;
}

static int gp_run(const n265_api *a, y4m_t *y, const char *in_path,
                  const n265_param *p, int W,
                  int T, int threads_arg, int keyint, long max_frames, int abr_on,
                  double bitrate_kbps, int vbv_on, int repeat_hdr,
                  FILE *fout, FILE *fdump, int want_met, long *nframes,
                  double met[6], long *nmet, int *nseg_out, int *k_out)
{
    gp_job jb, *j = &jb;
    memset(j, 0, sizeof *j);
    j->a = a;
    j->g = a->depth == 10 ? &gp10 : &gp8;
    j->p = *p;
    j->T = T;
    j->W = W > T ? T : W;
    j->K = T / j->W > 0 ? T / j->W : 1;
    j->L = gp_env_int(getenv("N265_GOP_LEN"), keyint > 0 ? keyint : 250);
    if (j->L < 16)
        j->L = 16;
    j->cuts = gp_env_int(getenv("N265_GOP_CUTS"), 1);
    long win = (long)((j->W + 1.5) * j->L);
    j->window = gp_env_int(getenv("N265_GOP_WINDOW"), (int)win);
    if (j->window < 2L * j->L)
        j->window = 2L * j->L;
    j->bps = y->out_depth == 8 ? 1 : 2;
    for (int c = 0; c < 3; c++) {
        j->pw[c] = c ? y->w >> y->csx : y->w;
        j->ph[c] = c ? y->h >> y->csy : y->h;
        j->psz[c] = (size_t)j->pw[c] * j->ph[c] * j->bps;
    }
    j->abr = abr_on;
    j->vbv = vbv_on;
    j->repeat_hdr = repeat_hdr;
    j->want_dump = fdump != NULL;
    j->want_met = want_met;
    double fps = p->fps_den > 0 ? (double)p->fps_num / p->fps_den : 25.0;
    j->bpf = bitrate_kbps * 1000.0 / (fps > 0.0 ? fps : 25.0);
    j->vbv_in = gp_env_num(getenv("N265_GOP_VBV_IN"), 0.5);
    j->vbv_out = gp_env_num(getenv("N265_GOP_VBV_OUT"), 0.6);
    if (j->vbv_out < j->vbv_in)
        j->vbv_out = j->vbv_in;
    pthread_mutex_init(&j->lock, NULL);
    pthread_cond_init(&j->cv, NULL);
    pthread_mutex_init(&j->open_lock, NULL);
    j->src = y;
    j->path = in_path;
    j->relief = gp_env_int(getenv("N265_GOP_RELIEF"), 1);
    j->kcap = gp_env_int(getenv("N265_GOP_K"), j->K + 1 > 3 ? j->K + 1 : 3);
    j->seek = in_path && gp_env_int(getenv("N265_GOP_SEEK"), 1) &&
              gp_seekable(y, &j->base, &j->rec, &j->total);
    if (getenv("N265_GOP_TRACE"))
        fprintf(stderr, "GOP,reader,%s\n", j->seek ? "seek" : "window");

    gp_sc sc;
    int sc_ok = j->cuts && gp_sc_init(&sc, y->w, y->h) == 0;
    if (!sc_ok)
        j->cuts = 0;
    unsigned char *cut = NULL;
    long cut_cap = 0;
    size_t fbytes = j->psz[0] + j->psz[1] + j->psz[2];
    pthread_t *tid = calloc((size_t)j->W, sizeof *tid);
    gp_arg ga = { j };
    int started = 0, rc = 0, next_out = 0, b = 0, r = 0;
    long n = 0;

    while (!rc) {
        if (max_frames >= 0 && n >= max_frames)
            r = 0;
        else if (j->seek)
            r = n < j->total;
        else
            r = y4m_read(y);
        if (r < 0) {
            fprintf(stderr, "n265: truncated input after frame %ld\n", n);
            rc = 1;
            break;
        }
        pthread_mutex_lock(&j->lock);
        if (r > 0 && j->seek) {
            /* the instances read the pictures; the boundary search needs
             * the luma plane and keeps one byte per frame */
            if (n >= cut_cap) {
                long nc = cut_cap ? 2 * cut_cap : 4096;
                unsigned char *ncut = realloc(cut, (size_t)nc);
                if (!ncut) {
                    pthread_mutex_unlock(&j->lock);
                    rc = 1;
                    break;
                }
                memset(ncut + cut_cap, 0, (size_t)(nc - cut_cap));
                cut = ncut;
                cut_cap = nc;
            }
            pthread_mutex_unlock(&j->lock);
            if (j->cuts) {
                const void *lu = gp_scan_luma(j, y, n);
                if (!lu) {
                    fprintf(stderr, "n265: cannot read frame %ld\n", n);
                    rc = 1;
                    break;
                }
                cut[n] = (unsigned char)gp_sc_cut(&sc,
                              gp_sc_push(&sc, lu, j->pw[0], j->bps));
            }
            pthread_mutex_lock(&j->lock);
            n++;
            j->n_read = n;
        } else if (r > 0) {
            while (!j->abort_ && j->resident >= j->window) {
                gp_flush(j, &next_out, fout, fdump);
                if (j->resident >= j->window && !j->abort_)
                    pthread_cond_wait(&j->cv, &j->lock);
            }
            if (j->abort_) {
                pthread_mutex_unlock(&j->lock);
                rc = 1;
                break;
            }
            if (n >= j->frm_cap || n >= cut_cap) {
                long nc = j->frm_cap ? 2 * j->frm_cap : 4096;
                uint8_t **nf = realloc(j->frm, (size_t)nc * sizeof *nf);
                unsigned char *ncut = realloc(cut, (size_t)nc);
                if (!nf || !ncut) {
                    pthread_mutex_unlock(&j->lock);
                    rc = 1;
                    break;
                }
                memset(nf + j->frm_cap, 0,
                       (size_t)(nc - j->frm_cap) * sizeof *nf);
                memset(ncut + j->frm_cap, 0, (size_t)(nc - j->frm_cap));
                j->frm = nf;
                cut = ncut;
                j->frm_cap = cut_cap = nc;
            }
            pthread_mutex_unlock(&j->lock);
            uint8_t *f = malloc(fbytes);
            if (!f) {
                rc = 1;
                break;
            }
            /* the reader's planes are contiguous per plane, at the
             * strides the serial path hands the library */
            uint8_t *d = f;
            for (int c = 0; c < 3; c++) {
                memcpy(d, y->plane[c], j->psz[c]);
                d += j->psz[c];
            }
            if (j->cuts)
                cut[n] = (unsigned char)gp_sc_cut(&sc,
                              gp_sc_push(&sc, f, j->pw[0], j->bps));
            pthread_mutex_lock(&j->lock);
            j->frm[n] = f;
            j->resident++;
            n++;
            j->n_read = n;
        } else {
            j->eof = 1;
        }
        /* place every segment the frames read so far decide */
        int placed = 0;
        while (!j->final && gp_place(j, cut, b)) {
            placed = 1;
            if (j->nseg)
                b = j->seg[j->nseg - 1]->end;
        }
        if (placed && !started && j->nseg + j->final > 0) {
            /* one segment and the input is over: the serial encode, on
             * one instance at the thread count it was given; otherwise
             * the HRD timing SEI comes off (see above), before any other
             * thread exists to read the environment, and the workers
             * start */
            int single = j->final && j->nseg <= 1;
            if (single && j->nseg)
                j->seg[0]->threads = threads_arg;
            if (!single && vbv_on)
                setenv("N265_VBV_HRD", "0", 1);
            /* item 45: an instance's frames in flight follow its own
             * workers, not the whole box's (a scheduling ceiling only) */
            if (!single && j->kcap > 0) {
                char kb[16];
                snprintf(kb, sizeof kb, "%d", j->kcap);
                setenv("N265_K_CAP", kb, 0);
            }
            int nw = single ? 1 : j->W;
            for (int t = 0; t < nw; t++)
                if (pthread_create(&tid[t], NULL, gp_worker, &ga) == 0)
                    started++;
        }
        pthread_cond_broadcast(&j->cv);
        if (gp_flush(j, &next_out, fout, fdump))
            rc = 1;
        int done_reading = j->eof;
        pthread_mutex_unlock(&j->lock);
        if (done_reading)
            break;
    }
    pthread_mutex_lock(&j->lock);
    if (rc) {
        j->abort_ = 1;
        j->final = 1;
    }
    pthread_cond_broadcast(&j->cv);
    while (!rc && next_out < j->nseg) {
        if (gp_flush(j, &next_out, fout, fdump)) {
            rc = 1;
            break;
        }
        if (next_out < j->nseg)
            pthread_cond_wait(&j->cv, &j->lock);
    }
    pthread_mutex_unlock(&j->lock);
    for (int t = 0; t < started; t++)
        pthread_join(tid[t], NULL);

    double sse[3] = { 0, 0, 0 }, ssim = 0.0;
    long nm = 0;
    for (int k = 0; k < j->nseg; k++) {
        gp_seg *s = j->seg[k];
        if (s->err)
            rc = 1;
        if (s->nmet > 0) {
            for (int c = 0; c < 3; c++)
                sse[c] += s->nmet * pow(10.0, -s->met[c] / 10.0);
            ssim += s->nmet * s->met[4];
            nm += s->nmet;
        }
        free(s->buf);
        free(s->rc);
        if (s->recon)
            fclose(s->recon);
        free(s);
    }
    /* the pooled numbers: the per-plane mean squared error, pooled over
     * frames and logged once, which is what one instance reports too;
     * the average weighs each plane by its samples, as the library's own
     * pooling does */
    if (nm > 0) {
        for (int c = 0; c < 3; c++)
            met[c] = -10.0 * log10(sse[c] / nm);
        double w0 = (double)j->psz[0], w1 = (double)j->psz[1],
               w2 = (double)j->psz[2];
        met[3] = -10.0 * log10((w0 * sse[0] + w1 * sse[1] + w2 * sse[2]) /
                               ((w0 + w1 + w2) * nm));
        met[4] = ssim / nm;
        met[5] = met[4] < 1.0 ? -10.0 * log10(1.0 - met[4]) : 100.0;
    }
    *nmet = nm;
    for (long i = 0; i < n && j->frm; i++)
        free(j->frm[i]);
    free(j->frm);
    y->clipped += j->clipped;
    free(cut);
    free(tid);
    free(j->seg);
    if (sc_ok)
        gp_sc_free(&sc);
    *nframes = n;
    *nseg_out = j->nseg;
    *k_out = j->nseg > 1 ? j->K : T;
    return rc;
}

int main(int argc, char **argv)
{
    const char *in_path = NULL, *out_path = "out.hevc";
    const char *dump_path = NULL, *shot_path = NULL;
    const char *stats_path = NULL;
    int pass = 0;               /* --pass; 0 = off, the default */
    long max_frames = -1;
    int qp = 26, pcm = 0, ctu = 64, keyint = 250, bframes = 7, aq = 1;
    int lossless = 0;
    const char *level_str = NULL;   /* --level; NULL = derive it */
    int high_tier = 0, vui_timing = 1, verbose = 0;   /* VUI timing on since 2026-09-12 */
    /* --show-config (phase 14 item 13): print the resolved configuration
     * and stop, for a script that wants the table and not an encode. The
     * table itself is what --verbose prints on its way past. */
    int show_config = 0;
    double film_grain = 0.0;    /* --film-grain: 0 = off, and off is the default */
    int fgc_denoise = 0;        /* --film-grain-denoise */
    /* --nr-intra / --nr-inter: -1 is "not given", which leaves the preset
     * to choose - medium 100 / 100 since item 6b, fast/slow/slower 0 - so
     * this is an override and not a default (the shape --rdoq-level has).
     * `--nr-intra 0 --nr-inter 0` is still off and still wins. */
    int nr_intra = -1, nr_inter = -1;
    int had_nr_intra = 0, had_nr_inter = 0;
    /* --mcstf and its two: -1 is "no flag given", which leaves on/off and
     * the range to the preset (phase 14 item 31); not selective */
    int mcstf = -1, mcstf_range = -1, mcstf_selective = 0;
    int dither = 0;             /* --dither, only on the 10 -> 8 downshift */
    double crf = -1.0, bitrate = 0.0;
    /* --vbv-maxrate / --vbv-bufsize / --vbv-init; 0/0 is the mode off,
     * which is every encode before phase 12 item 2. */
    double vbv_maxrate = 0.0, vbv_bufsize = 0.0, vbv_init = 0.9;
    /* Pass-through signaling (phase 14 item 1). -1 is "the flag was not
     * given" in every field that has an absence; give none of them and
     * nothing below reaches the stream. */
    int hash = 0;
    /* --weightp / --weightb; -1 is "no flag given", which leaves the
     * value to N265_WEIGHTP / N265_WEIGHTB and then to the preset */
    int weightp = -1, weightb = -1;
    /* open GOP (phase 14 item 3): on, which is x265's default too. */
    int open_gop = 1, radl = 0;
    int sig_videoformat = -1, sig_range = -1, sig_overscan = -1;
    int sig_colorprim = -1, sig_transfer = -1, sig_colormatrix = -1;
    int sig_chromaloc = -1;
    /* --video-signal-type-preset: what the NAME asked for, kept apart
     * from the nine individual flags above so the rule "an explicit flag
     * wins" holds whichever order the two were given in. */
    int vsp[N265_VSP_NFIELD], vsp_md_on = 0, had_vsp = 0;
    uint32_t vsp_md[10];
    /* -t/--tune: the composition, applied after the loop and only where
     * the command line named nothing. -1 is a knob it does not touch. */
    int tune[N265_TUNE_NFIELD] = { -1, -1, -1, -1, -1 };
    int had_tune = 0, la_from_tune = 0;
    /* --dup-threshold: negative is "not given", which leaves
     * N265_DUP_THR and then the strict form in charge. */
    double dup_thr = -1.0;
    int sar_idc = -1, sar_w = 0, sar_h = 0;
    int win_on = 0, win_l = 0, win_t = 0, win_r = 0, win_b = 0;
    uint32_t md[10];
    int have_md = 0, maxcll = -1, maxfall = -1;
    int hdr10 = -1, cll_on = 1, atc = -1;
    int hdr_opt = -1;
    const char *h10p_path = NULL, *rpu_path = NULL;
    int dv_profile = 0;     /* --dolby-vision-profile: 81, 82 or 84 */
    int had_crf = 0;            /* --crf given: the ON switch, not crf >= 0 */
    int out_depth = 0;          /* --output-depth; 0 = the input's */
    /* RAW input (--input-res / --input-csp / --fps / --input-depth) and
     * --seek. Every one of them is 0 / -1 for "not given": a y4m carries
     * its own geometry and none of this is read on that path. */
    int in_w = 0, in_h = 0, in_csp = -1, in_depth = 0;
    int in_fps_num = 0, in_fps_den = 0;
    long seek = 0;
    /* --psnr / --ssim / --csv / --csv-log-level. Off is the default and
     * off is byte-for-byte the encoder without them. */
    /* --repeat-headers / --aud / --eos / --eob / --info. The first four
     * are off; --info is ON since phase 14 item 9, which is x265's
     * default too - a stream says what wrote it unless it is asked not
     * to. `--no-info` is the escape, and an identity cmp against a
     * pre-item binary is run with it (CLAUDE.md, docs/instruments.md). */
    int repeat_hdr = 0, aud = 0, eos = 0, eob = 0, info = 1;
    int fingerprint = 1;        /* on unless --no-fingerprint says not */
    /* The rate-control exposure. The sentinels ARE the widest legal
     * values, so an unnamed flag and a flag named at its widest are the
     * same thing and neither is applied. */
    /* --idr-recovery-sei / --pic-struct / --hrd-concat /
     * --temporal-layers. All absent by default; pic_struct is -1 for
     * "the flag was not given". */
    int recovery_sei = 0, pic_struct = -1, hrd_concat = 0, tlayers = 0;
    double crf_lo = -1e9, crf_hi = 1e9;
    int qpmin = 0, qpmax = 51, strict_cbr = 0, had_qpmin = 0, had_qpmax = 0;
    double ausize_factor = 1.0;
    const char *qpfile_path = NULL, *zones_spec = NULL;
    int want_psnr = 0, want_ssim = 0, csv_level = -1;
    const char *csv_path = NULL;
    int nref = 1, badapt = 1;
    int lookahead = 20, cutree = 1;   /* 20 since 2026-09-12 (x265 medium's
                                     * own depth): the last window that
                                     * clears every gate rule, docs/
                                     * phase7-gop-design.md step 4 */   /* on by default since 2026-09-12: every
                                     * measured medium passes both, and the
                                     * bare CLI used to hand out the tools-only
                                     * encoder (docs/instruments.md, board) */
    int threads = 0;
    int gop_par = 0;            /* --gop-parallel; 0 = off, the default */
    int shot_window = 0;        /* --shot-window; 0 = off, the default */
    /* --rdoq-level: -1 is "not given", which leaves the preset (and the
     * environment) in charge - the flag is an override, not a default. */
    int rdoq_level = -1, had_rdoq = 0;
    /* --tu-inter-depth / --tu-intra-depth: -1 is "not given", the same
     * override-not-default shape --rdoq-level has. */
    int tu_inter = -1, tu_intra = -1, had_tui = 0, had_tua = 0;
    /* --cbqpoffs / --crqpoffs: "not given" is its own state, because
     * medium names an offset from the 720p class up (phase 14 item 51)
     * and an explicit 0 has to be able to undo it. */
    int cbqpoffs = 0, crqpoffs = 0, had_cbq = 0, had_crq = 0;
    const char *dump_decode_path = NULL;
    const char *preset = NULL;
    /* which structural flags the user gave: a preset supplies the rest */
    int had_ref = 0, had_badapt = 0, had_bframes = 0, had_la = 0, had_ctu = 0;
    int had_cutree = 0;

    optstr info_opts = { .s = "", .n = 0 };

    for (int i = 1; i < argc; i++) {
        const int argi = i;     /* where this option started: the branch
                                 * below moves `i` past its value, if it
                                 * has one, and that is what opt_note
                                 * reads instead of a table (item 9) */
        if (!strcmp(argv[i], "-o") && i + 1 < argc) {
            out_path = argv[++i];
        } else if (!strcmp(argv[i], "--frames") && i + 1 < argc) {
            max_frames = atol(argv[++i]);
        } else if (!strcmp(argv[i], "--qp") && i + 1 < argc) {
            qp = atoi(argv[++i]);
        } else if (!strcmp(argv[i], "--crf") && i + 1 < argc) {
            crf = atof(argv[++i]); had_crf = 1;
        } else if (!strcmp(argv[i], "--output-depth") && i + 1 < argc) {
            out_depth = atoi(argv[++i]);
        } else if (!strcmp(argv[i], "--input-res") && i + 1 < argc) {
            const char *v = argv[++i];
            if (sscanf(v, "%dx%d", &in_w, &in_h) != 2 || in_w <= 0 ||
                in_h <= 0) {
                fprintf(stderr, "n265: --input-res is WxH, e.g. 352x288\n");
                return 2;
            }
        } else if (!strcmp(argv[i], "--input-csp") && i + 1 < argc) {
            int mono = 0;
            in_csp = input_csp_parse(argv[++i], &mono);
            if (in_csp < 0) {
                fprintf(stderr, mono
                        ? "n265: --input-csp i400 (monochrome) is not a "
                          "format this encoder codes yet; it is refused "
                          "rather than read as 4:2:0\n"
                        : "n265: --input-csp is i420, i422 or i444 (or the "
                          "chroma_format_idc itself: 1, 2, 3)\n");
                return 2;
            }
        } else if (!strcmp(argv[i], "--input-depth") && i + 1 < argc) {
            in_depth = atoi(argv[++i]);
        } else if (!strcmp(argv[i], "--fps") && i + 1 < argc) {
            if (fps_parse(argv[++i], &in_fps_num, &in_fps_den) < 0) {
                fprintf(stderr, "n265: --fps is N, N/D or a decimal "
                        "(30, 30000/1001, 29.97)\n");
                return 2;
            }
        } else if (!strcmp(argv[i], "--idr-recovery-sei")) {
            recovery_sei = 1;
        } else if (!strcmp(argv[i], "--no-idr-recovery-sei")) {
            recovery_sei = 0;
        } else if (!strcmp(argv[i], "--pic-struct") && i + 1 < argc) {
            pic_struct = atoi(argv[++i]);
            if (pic_struct < 0 || pic_struct > 12) {
                fprintf(stderr, "n265: --pic-struct is Table D.2's value "
                        "0..12 (0 frame, 7 frame doubling, 8 frame "
                        "tripling)\n");
                return 2;
            }
        } else if (!strcmp(argv[i], "--hrd-concat")) {
            hrd_concat = 1;
        } else if (!strcmp(argv[i], "--temporal-layers")) {
            tlayers = 1;
        } else if (!strcmp(argv[i], "--no-temporal-layers")) {
            tlayers = 0;
        } else if (!strcmp(argv[i], "--scaling-list") && i + 1 < argc) {
            /* Parsed and REFUSED, by name, with the reason. The SPS flag
             * is two bits; what follows it is not. With
             * scaling_list_enabled_flag set, a decoder dequantizes with
             * the default matrices of Tables 7-5 and 7-6, so this
             * encoder's quantizer has to apply them too or every
             * reconstruction stops matching every decoder - and that
             * reaches the forward quantizer, the inverse, RDOQ's per
             * coefficient rate-distortion and the NEON kernels of both.
             * It is a quantizer item with a checkasm leg, not a cheap
             * signaling one, and shipping the flag alone would fail the
             * recon-match gate on its first vector. */
            fprintf(stderr, "n265: --scaling-list is not implemented, and "
                    "not because the SPS flag is hard: with "
                    "scaling_list_enabled_flag set a decoder dequantises "
                    "with the spec's default matrices, so the encoder's "
                    "own quantiser has to apply them or the "
                    "reconstruction stops matching every decoder. That "
                    "reaches the forward quantiser, the inverse, RDOQ and "
                    "the NEON kernels of both - a quantiser item with a "
                    "checkasm leg. See docs/x265-feature-audit.md.\n");
            return 2;
        } else if (!strcmp(argv[i], "--crf-min") && i + 1 < argc) {
            crf_lo = atof(argv[++i]);
        } else if (!strcmp(argv[i], "--crf-max") && i + 1 < argc) {
            crf_hi = atof(argv[++i]);
        } else if (!strcmp(argv[i], "--qpmin") && i + 1 < argc) {
            qpmin = atoi(argv[++i]); had_qpmin = 1;
        } else if (!strcmp(argv[i], "--qpmax") && i + 1 < argc) {
            qpmax = atoi(argv[++i]); had_qpmax = 1;
        } else if (!strcmp(argv[i], "--strict-cbr")) {
            strict_cbr = 1;
        } else if (!strcmp(argv[i], "--max-ausize-factor") && i + 1 < argc) {
            ausize_factor = atof(argv[++i]);
        } else if (!strcmp(argv[i], "--qpfile") && i + 1 < argc) {
            qpfile_path = argv[++i];
        } else if (!strcmp(argv[i], "--zones") && i + 1 < argc) {
            zones_spec = argv[++i];
        } else if (!strcmp(argv[i], "--repeat-headers")) {
            repeat_hdr = 1;
        } else if (!strcmp(argv[i], "--no-repeat-headers")) {
            repeat_hdr = 0;
        } else if (!strcmp(argv[i], "--aud")) {
            aud = 1;
        } else if (!strcmp(argv[i], "--no-aud")) {
            aud = 0;
        } else if (!strcmp(argv[i], "--eos")) {
            eos = 1;
        } else if (!strcmp(argv[i], "--no-eos")) {
            eos = 0;
        } else if (!strcmp(argv[i], "--eob")) {
            eob = 1;
        } else if (!strcmp(argv[i], "--no-eob")) {
            eob = 0;
        } else if (!strcmp(argv[i], "--info")) {
            info = 1;
        } else if (!strcmp(argv[i], "--no-info")) {
            info = 0;
        } else if (!strcmp(argv[i], "--fingerprint")) {
            fingerprint = 1;
        } else if (!strcmp(argv[i], "--no-fingerprint")) {
            fingerprint = 0;
        } else if (!strcmp(argv[i], "--psnr")) {
            want_psnr = 1;
        } else if (!strcmp(argv[i], "--ssim")) {
            want_ssim = 1;
        } else if (!strcmp(argv[i], "--csv") && i + 1 < argc) {
            csv_path = argv[++i];
        } else if (!strcmp(argv[i], "--csv-log-level") && i + 1 < argc) {
            csv_level = atoi(argv[++i]);
        } else if (!strcmp(argv[i], "--seek") && i + 1 < argc) {
            seek = atol(argv[++i]);
            if (seek < 0) {
                fprintf(stderr, "n265: --seek takes a frame count\n");
                return 2;
            }
        } else if (!strcmp(argv[i], "--bitrate") && i + 1 < argc) {
            bitrate = atof(argv[++i]);
        } else if (!strcmp(argv[i], "--ref") && i + 1 < argc) {
            nref = atoi(argv[++i]); had_ref = 1;
        } else if (!strcmp(argv[i], "--b-adapt") && i + 1 < argc) {
            badapt = atoi(argv[++i]); had_badapt = 1;
        } else if (!strcmp(argv[i], "--preset") && i + 1 < argc) {
            preset = argv[++i];
        } else if (!strcmp(argv[i], "--keyint") && i + 1 < argc) {
            keyint = atoi(argv[++i]);
        } else if (!strcmp(argv[i], "--open-gop")) {
            open_gop = 1;
        } else if (!strcmp(argv[i], "--no-open-gop")) {
            open_gop = 0;
        } else if (!strcmp(argv[i], "--radl") && i + 1 < argc) {
            radl = atoi(argv[++i]);
        } else if (!strcmp(argv[i], "--bframes") && i + 1 < argc) {
            bframes = atoi(argv[++i]); had_bframes = 1;
        } else if (!strcmp(argv[i], "--aq") && i + 1 < argc) {
            aq = atoi(argv[++i]);
        } else if (!strcmp(argv[i], "--rc-lookahead") && i + 1 < argc) {
            lookahead = atoi(argv[++i]); had_la = 1;
        } else if (!strcmp(argv[i], "--cutree") && i + 1 < argc) {
            cutree = atoi(argv[++i]); had_cutree = 1;
        } else if (!strcmp(argv[i], "--dump-recon-decode") && i + 1 < argc) {
            dump_decode_path = argv[++i];
        } else if (!strcmp(argv[i], "--threads") && i + 1 < argc) {
            threads = atoi(argv[++i]);
        } else if (!strcmp(argv[i], "--gop-parallel") && i + 1 < argc) {
            gop_par = atoi(argv[++i]);
        } else if (!strcmp(argv[i], "--ctu") && i + 1 < argc) {
            ctu = atoi(argv[++i]); had_ctu = 1;
        } else if (!strcmp(argv[i], "--rdoq-level") && i + 1 < argc) {
            rdoq_level = atoi(argv[++i]); had_rdoq = 1;
        } else if (!strcmp(argv[i], "--tu-inter-depth") && i + 1 < argc) {
            tu_inter = atoi(argv[++i]); had_tui = 1;
        } else if (!strcmp(argv[i], "--tu-intra-depth") && i + 1 < argc) {
            tu_intra = atoi(argv[++i]); had_tua = 1;
        } else if (!strcmp(argv[i], "--cbqpoffs") && i + 1 < argc) {
            cbqpoffs = atoi(argv[++i]); had_cbq = 1;
        } else if (!strcmp(argv[i], "--crqpoffs") && i + 1 < argc) {
            crqpoffs = atoi(argv[++i]); had_crq = 1;
        } else if (!strcmp(argv[i], "--pcm")) {
            pcm = 1;
        } else if (!strcmp(argv[i], "--lossless")) {
            lossless = 1;
        } else if (!strcmp(argv[i], "--weightp") || !strcmp(argv[i], "-w")) {
            weightp = 1;
        } else if (!strcmp(argv[i], "--no-weightp")) {
            weightp = 0;
        } else if (!strcmp(argv[i], "--weightb")) {
            weightb = 1;
        } else if (!strcmp(argv[i], "--no-weightb")) {
            weightb = 0;
        } else if (!strcmp(argv[i], "--level") && i + 1 < argc) {
            level_str = argv[++i];
        } else if (!strcmp(argv[i], "--high-tier")) {
            high_tier = 1;
        } else if (!strcmp(argv[i], "--verbose") || !strcmp(argv[i], "-v")) {
            verbose = 1;
        } else if (!strcmp(argv[i], "--show-config")) {
            show_config = 1;
        } else if (!strcmp(argv[i], "--vui-timing")) {
            vui_timing = 1;
        } else if (!strcmp(argv[i], "--no-vui-timing")) {
            vui_timing = 0;
        } else if (!strcmp(argv[i], "--film-grain") && i + 1 < argc) {
            film_grain = atof(argv[++i]);
        } else if (!strcmp(argv[i], "--film-grain-denoise") && i + 1 < argc) {
            fgc_denoise = atoi(argv[++i]);
        } else if (!strcmp(argv[i], "--nr-intra") && i + 1 < argc) {
            nr_intra = atoi(argv[++i]); had_nr_intra = 1;
        } else if (!strcmp(argv[i], "--nr-inter") && i + 1 < argc) {
            nr_inter = atoi(argv[++i]); had_nr_inter = 1;
        } else if (!strcmp(argv[i], "--mcstf")) {
            mcstf = 1;
        } else if (!strcmp(argv[i], "--no-mcstf")) {
            mcstf = 0;
        } else if (!strcmp(argv[i], "--mcstf-ref-range") && i + 1 < argc) {
            mcstf_range = atoi(argv[++i]);
        } else if (!strcmp(argv[i], "--selective-mcstf")) {
            mcstf_selective = 1;
        } else if (!strcmp(argv[i], "--no-selective-mcstf")) {
            mcstf_selective = 0;
        } else if (!strcmp(argv[i], "--dither")) {
            dither = 1;
        } else if (!strcmp(argv[i], "--no-dither")) {
            dither = 0;
        } else if (!strcmp(argv[i], "--vbv-maxrate") && i + 1 < argc) {
            vbv_maxrate = atof(argv[++i]);
        } else if (!strcmp(argv[i], "--vbv-bufsize") && i + 1 < argc) {
            vbv_bufsize = atof(argv[++i]);
        } else if (!strcmp(argv[i], "--vbv-init") && i + 1 < argc) {
            vbv_init = atof(argv[++i]);
        } else if (!strcmp(argv[i], "--hash") && i + 1 < argc) {
            hash = atoi(argv[++i]);
        } else if (!strcmp(argv[i], "--colorprim") && i + 1 < argc) {
            SIGARG(N265_VUI_COLORPRIM, sig_colorprim);
        } else if (!strcmp(argv[i], "--transfer") && i + 1 < argc) {
            SIGARG(N265_VUI_TRANSFER, sig_transfer);
        } else if (!strcmp(argv[i], "--colormatrix") && i + 1 < argc) {
            SIGARG(N265_VUI_COLORMATRIX, sig_colormatrix);
        } else if (!strcmp(argv[i], "--videoformat") && i + 1 < argc) {
            SIGARG(N265_VUI_VIDEOFORMAT, sig_videoformat);
        } else if (!strcmp(argv[i], "--range") && i + 1 < argc) {
            SIGARG(N265_VUI_RANGE, sig_range);
        } else if (!strcmp(argv[i], "--overscan") && i + 1 < argc) {
            SIGARG(N265_VUI_OVERSCAN, sig_overscan);
            /* "unknown" is the absence of the field and not a value of
             * it, and the table's last entry is what says so. */
            if (sig_overscan == 2)
                sig_overscan = -1;
        } else if (!strcmp(argv[i], "--video-signal-type-preset") &&
                   i + 1 < argc) {
            const char *why = NULL;
            const char *v = argv[++i];
            int rc = n265_vsp_parse(v, vsp, vsp_md, &vsp_md_on, &why);
            if (rc != N265_VSP_OK) {
                if (rc == N265_VSP_BAD_SYSTEM) {
                    fprintf(stderr, "n265: --video-signal-type-preset %s "
                            "is not a system-id. The names are:", v);
                    for (int k = 0; n265_vsp_name(k); k++)
                        fprintf(stderr, " %s", n265_vsp_name(k));
                    fprintf(stderr, " - each of them x265's spelling and "
                            "x265's values, and each expands to the nine "
                            "colour flags, which override it.\n");
                } else if (rc == N265_VSP_VOLUME_SYSTEM) {
                    fprintf(stderr, "n265: --video-signal-type-preset %s "
                            "takes no colour volume: a mastering display "
                            "volume is carried only by the three "
                            "BT2100_PQ_* systems, as in x265. Name "
                            "--master-display if the content was graded "
                            "on one anyway.\n", v);
                } else if (rc == N265_VSP_BAD_VOLUME) {
                    fprintf(stderr, "n265: --video-signal-type-preset: no "
                            "such colour volume. The names are:");
                    for (int k = 0; n265_vsp_volume_name(k); k++)
                        fprintf(stderr, " %s", n265_vsp_volume_name(k));
                    fprintf(stderr, "\n");
                } else {
                    fprintf(stderr, "n265: --video-signal-type-preset: "
                            "that colour volume is refused by name - %s\n",
                            why ? why : "no reason recorded");
                }
                return 2;
            }
            had_vsp = 1;
        } else if ((!strcmp(argv[i], "--tune") || !strcmp(argv[i], "-t")) &&
                   i + 1 < argc) {
            const char *why = NULL;
            const char *v = argv[++i];
            int rc = n265_tune_parse(v, tune, &why);
            if (rc == N265_TUNE_REFUSED) {
                fprintf(stderr, "n265: --tune %s is refused BY NAME, and "
                        "here is why: %s\n", v, why);
                return 2;
            }
            if (rc != N265_TUNE_OK) {
                fprintf(stderr, "n265: --tune %s is not one of x265's "
                        "five names. They are:", v);
                for (int k = 0; n265_tune_name(k); k++)
                    fprintf(stderr, " %s", n265_tune_name(k));
                fprintf(stderr, " - and only zerolatency is accepted here; "
                        "ask for one of the other four by name and it "
                        "prints its own reason.\n");
                return 2;
            }
            had_tune = 1;
        } else if (!strcmp(argv[i], "--intra-refresh")) {
            /* Parsed and REFUSED, by name, with the reason - the same
             * shape as --scaling-list above and for the same kind of
             * reason: the flag is one bit and what stands behind it is
             * not. Priced in phase 14 item 15 and left for its own item;
             * docs/phase14.md has the whole reading.
             *
             * A refresh that is only half enforced is WORSE than no
             * refresh: the stream would claim a recovery point that a
             * decoder joining there cannot honor, and nothing in this
             * encoder's gates would say so, because every one of them
             * decodes from the start of the stream. */
            fprintf(stderr, "n265: --intra-refresh is not implemented, "
                    "and not because the intra column is hard. The "
                    "refresh is only worth its recovery point if NOTHING "
                    "in the refreshed region reads an unrefreshed "
                    "sample, and three of this encoder's parts have to "
                    "change for that: (1) the motion search's clamp "
                    "bounds MC to the padded reference, but a merge or "
                    "AMVP candidate is INHERITED and is deliberately not "
                    "clamped (src/encoder/mc.c), so the bound has to "
                    "move into mode decision, where it meets the merge "
                    "index the stream fingerprint writes; (2) "
                    "constrained intra prediction, which the refresh "
                    "needs and which we do not have, makes reference "
                    "availability depend on each neighbour's prediction "
                    "mode - it is a PREFIX today, and the closed-form "
                    "substitution and binary search in "
                    "src/encoder/intra.c exist only because it is; "
                    "(3) the plan builder has one kind of key picture, "
                    "and a refresh period is a second. It is an item "
                    "with its own gate - decode from the recovery point, "
                    "not from the start - and not a flag. See "
                    "docs/phase14.md item 15 and "
                    "docs/x265-feature-audit.md.\n");
            return 2;
        } else if (!strcmp(argv[i], "--dup-threshold") && i + 1 < argc) {
            dup_thr = atof(argv[++i]);
            if (dup_thr < 0.0 || dup_thr > 255.0) {
                fprintf(stderr, "n265: --dup-threshold is 0..255, in "
                        "8-bit luma levels per lowres pixel; 0 (the "
                        "default) is the strict form, where only an "
                        "identical source arms the path\n");
                return 2;
            }
        } else if (!strcmp(argv[i], "--chromaloc") && i + 1 < argc) {
            sig_chromaloc = atoi(argv[++i]);
            if (sig_chromaloc < 0 || sig_chromaloc > 5) {
                fprintf(stderr, "n265: --chromaloc is 0..5 (E.2.1's "
                        "chroma_sample_loc_type)\n");
                return 2;
            }
        } else if (!strcmp(argv[i], "--sar") && i + 1 < argc) {
            if (n265_sar_parse(argv[++i], &sar_idc, &sar_w, &sar_h) < 0) {
                fprintf(stderr, "n265: --sar takes a ratio (\"16:9\") or a "
                        "Table E.1 index 0..16\n");
                return 2;
            }
        } else if (!strcmp(argv[i], "--display-window") && i + 1 < argc) {
            if (n265_window_parse(argv[++i], &win_l, &win_t, &win_r,
                                  &win_b) < 0) {
                fprintf(stderr, "n265: --display-window is "
                        "\"left,top,right,bottom\" in pixels\n");
                return 2;
            }
            win_on = 1;
        } else if (!strcmp(argv[i], "--master-display") && i + 1 < argc) {
            if (n265_master_display_parse(argv[++i], md) < 0) {
                fprintf(stderr, "n265: --master-display is "
                        "G(x,y)B(x,y)R(x,y)WP(x,y)L(max,min); the eight "
                        "chromaticities are 0..65535 in units of "
                        "0.00002\n");
                return 2;
            }
            have_md = 1;
        } else if (!strcmp(argv[i], "--max-cll") && i + 1 < argc) {
            if (n265_cll_parse(argv[++i], &maxcll, &maxfall) < 0) {
                fprintf(stderr, "n265: --max-cll is \"cll,fall\", both "
                        "0..65535 cd/m2\n");
                return 2;
            }
        } else if (!strcmp(argv[i], "--hdr-opt") ||
                   !strcmp(argv[i], "--hdr10-opt")) {
            hdr_opt = 1;
        } else if (!strcmp(argv[i], "--no-hdr-opt") ||
                   !strcmp(argv[i], "--no-hdr10-opt")) {
            hdr_opt = 0;
        } else if (!strcmp(argv[i], "--hdr10")) {
            hdr10 = 1;
        } else if (!strcmp(argv[i], "--no-hdr10")) {
            hdr10 = 0;
        } else if (!strcmp(argv[i], "--cll")) {
            cll_on = 1;
        } else if (!strcmp(argv[i], "--no-cll")) {
            cll_on = 0;
        } else if (!strcmp(argv[i], "--atc-sei") && i + 1 < argc) {
            atc = n265_vui_parse(N265_VUI_TRANSFER, argv[++i]);
            if (atc < 0) {
                fprintf(stderr, "n265: --atc-sei takes a transfer "
                        "characteristic, by name or by number\n");
                return 2;
            }
        } else if (!strcmp(argv[i], "--dhdr10-info") && i + 1 < argc) {
            h10p_path = argv[++i];
        } else if (!strcmp(argv[i], "--dolby-vision-rpu") && i + 1 < argc) {
            rpu_path = argv[++i];
        } else if (!strcmp(argv[i], "--dolby-vision-profile") &&
                   i + 1 < argc) {
            const char *v = argv[++i];
            dv_profile = !strcmp(v, "8.1") || !strcmp(v, "81") ? 81
                       : !strcmp(v, "8.2") || !strcmp(v, "82") ? 82
                       : !strcmp(v, "8.4") || !strcmp(v, "84") ? 84 : -1;
            if (dv_profile < 0) {
                fprintf(stderr, "n265: --dolby-vision-profile is 8.1 (HDR10 "
                        "base layer), 8.2 (SDR BT.709) or 8.4 (HLG)\n");
                return 2;
            }
        } else if (!strcmp(argv[i], "--dump-recon") && i + 1 < argc) {
            dump_path = argv[++i];
        } else if (!strcmp(argv[i], "--shot-window") && i + 1 < argc) {
            shot_window = atoi(argv[++i]);
        } else if (!strcmp(argv[i], "--shot-stats") && i + 1 < argc) {
            shot_path = argv[++i];
        } else if (!strcmp(argv[i], "--pass") && i + 1 < argc) {
            pass = atoi(argv[++i]);
        } else if (!strcmp(argv[i], "--stats") && i + 1 < argc) {
            stats_path = argv[++i];
        } else if (!strcmp(argv[i], "--version")) {
            printf("n265 %s\n", n265_version_str());
            return 0;
        } else if (!strcmp(argv[i], "-h") || !strcmp(argv[i], "--help")) {
            usage();
            return 0;
        } else if (argv[i][0] != '-' || !strcmp(argv[i], "-")) {
            in_path = argv[i];
        } else {
            fprintf(stderr, "n265: unknown option %s\n", argv[i]);
            usage();
            return 2;
        }
        opt_note(&info_opts, argv, argi, i);
    }
    if (!in_path) {
        usage();
        return 2;
    }

    if (out_depth != 0 && out_depth != 8 && out_depth != 10) {
        fprintf(stderr, "n265: --output-depth must be 8 or 10\n");
        return 2;
    }

    FILE *fin = strcmp(in_path, "-") ? fopen(in_path, "rb") : stdin;
    if (!fin) {
        fprintf(stderr, "n265: cannot open %s: %s\n", in_path, strerror(errno));
        return 1;
    }
    /* RAW or y4m, and the decision is made from the FLAGS and the name,
     * never by sniffing: a raw file has no magic to sniff for, and a
     * reader that guessed would code garbage silently on the one input
     * that cannot say what it is. `--input-res` is the switch - x265
     * takes the same decision off the `.yuv` extension, and that is
     * honored too so a plain `n265 clip.yuv --input-res ...` and the
     * bare extension agree. */
    if (is_y4m_name(in_path) &&
        (in_w > 0 || in_csp >= 0 || in_depth || in_fps_num > 0)) {
        fprintf(stderr, "n265: --input-res / --input-csp / --input-depth / "
                "--fps describe RAW input; %s carries its own geometry\n",
                in_path);
        return 2;
    }
    int want_raw = in_w > 0 || is_raw_name(in_path);
    y4m_t y;
    y.out_depth = out_depth;
    int yr;
    if (want_raw) {
        /* A raw file says NOTHING about itself, so every property the y4m
         * header would have carried has to be on the command line. Two of
         * the three have no defensible default - a wrong size reads the
         * planes at the wrong offsets and a wrong frame rate mis-states
         * the level's MaxLumaSr leg - and the third, the chroma format,
         * changes the size of a frame. Missing means refused, one message
         * naming all of what is missing. */
        const char *miss[4];
        int nmiss = 0;
        if (in_w <= 0)        miss[nmiss++] = "--input-res WxH";
        if (in_csp < 0)       miss[nmiss++] = "--input-csp i420|i422|i444";
        if (in_fps_num <= 0)  miss[nmiss++] = "--fps N[/D]";
        if (nmiss) {
            fprintf(stderr, "n265: raw input carries no header, so its "
                    "geometry has to be given:");
            for (int k = 0; k < nmiss; k++)
                fprintf(stderr, " %s%s", miss[k], k + 1 < nmiss ? "," : "");
            fprintf(stderr, "\n");
            return 2;
        }
        if (in_depth == 0)
            in_depth = 8;
        if (in_depth != 8 && in_depth != 10) {
            fprintf(stderr, "n265: --input-depth must be 8 or 10\n");
            return 2;
        }
        yr = raw_open(&y, fin, in_w, in_h, in_csp, in_depth,
                      in_fps_num, in_fps_den);
    } else {
        /* On a y4m the same four flags describe a file that describes
         * itself, so they are refused rather than silently ignored: a
         * --input-res that disagrees with the header is a mistake worth
         * a message. */
        if (in_w > 0 || in_csp >= 0 || in_depth || in_fps_num > 0) {
            fprintf(stderr, "n265: --input-res / --input-csp / "
                    "--input-depth / --fps describe RAW input; a y4m "
                    "carries its own geometry\n");
            return 2;
        }
        yr = y4m_open(&y, fin);
    }
    if (yr < 0) {
        if (yr == -1)
            fprintf(stderr, want_raw ? "n265: cannot size the raw input\n"
                                     : "n265: bad y4m input\n");
        return 1;
    }
    /* --dither, once the source's own depth is known. It is set AFTER
     * the open because both openers memset the struct, and it is refused
     * rather than ignored anywhere it would do nothing: a flag that is
     * silently inert is the one thing about a command line that cannot
     * be debugged from the output. */
    if (dither && !(y.depth == 10 && y.out_depth == 8)) {
        fprintf(stderr, "n265: --dither is the 10-bit to 8-bit reduction's "
                "own choice; it needs --output-depth 8 on a 10-bit source "
                "(this one is %d-bit in, %d-bit out)\n", y.depth,
                y.out_depth);
        return 2;
    }
    y.dither = dither;
    /* --seek, after the open (the y4m header has to be read either way)
     * and before anything is encoded. */
    if (seek > 0 && src_skip(&y, seek) < 0) {
        fprintf(stderr, "n265: --seek %ld runs past the end of %s\n",
                seek, in_path);
        return 1;
    }

    /* The dispatch, and it is the whole of it: one table per library,
     * selected by the width the pictures will carry. Everything after
     * this point calls through `a` and does not know which encoder it
     * is configuring. */
    const n265_api *a = y.out_depth == 10 ? &api10 : &api8;
    const int qp_bd_off = N265_QP_BD_OFF(a->depth);

    /* QpY runs -QpBdOffsetY..51 (7.4.3.3): 0..51 at 8 bits, -12..51 at
     * Main10, where the twelve extra steps are the finer quantizers the
     * two extra sample bits pay for and are the whole point of the
     * profile at the top of the ladder. */
    if (qp < -qp_bd_off || qp > 51) {
        fprintf(stderr, "n265: qp out of range %d..51 at %d bits\n",
                -qp_bd_off, a->depth);
        return 2;
    }
    /* The rate factor is a QpY too, so it has the same floor. Below it the
     * RC would clamp and quietly encode something else; above zero at 8
     * bits this is the range it always had. */
    if (had_crf && crf < -qp_bd_off) {
        fprintf(stderr, "n265: --crf %g is below the %d-bit floor of %d "
                "(QpBdOffsetY, 7.4.3.3)\n", crf, a->depth, -qp_bd_off);
        return 2;
    }
    /* The preset is a default source and nothing else: it is selected
     * before anything reads a knob, and every value it offers here is
     * taken only where the command line was silent. */
    if (preset) {
        int pr = a->set_preset(preset);
        if (pr == -2) {
            /* one of x265's ten names, with no table behind it: the rung
             * has not been PLACED (docs/phase13.md C2), and naming a
             * preset we have not measured against the x265 preset of the
             * same name is the one thing the ladder exists to stop. Point
             * at the nearest rung that has been. */
            fprintf(stderr, "n265: --preset %s is one of x265's names but "
                            "is not a placed rung of this encoder; the "
                            "nearest placed rung is %s\n",
                    preset, a->preset_nearest(preset));
            return 2;
        }
        if (pr < 0) {
            fprintf(stderr, "n265: --preset must be one of x265's names: "
                            "ultrafast, superfast, veryfast, faster, fast, "
                            "medium, slow, slower, veryslow, placebo (the "
                            "placed rungs are printed by --help)\n");
            return 2;
        }
    }
    /* -t/--tune, applied HERE: after the command line has been read, so
     * an explicitly named flag keeps its value, and before the preset
     * fallbacks below, so the tune beats the preset. The `had_` flags it
     * sets are what carry both halves of that rule. */
    if (had_tune) {
        if (!had_bframes && tune[N265_TUNE_BFRAMES] >= 0) {
            bframes = tune[N265_TUNE_BFRAMES]; had_bframes = 1;
        }
        if (!had_la && tune[N265_TUNE_LOOKAHEAD] >= 0) {
            lookahead = tune[N265_TUNE_LOOKAHEAD]; had_la = 1;
            la_from_tune = 1;
        }
        if (!had_badapt && tune[N265_TUNE_BADAPT] >= 0) {
            badapt = tune[N265_TUNE_BADAPT]; had_badapt = 1;
        }
        if (!had_cutree && tune[N265_TUNE_CUTREE] >= 0)
            cutree = tune[N265_TUNE_CUTREE];
        /* --mcstf holds --mcstf-ref-range pictures back, which is
         * latency, so the tune takes the preset's filter off unless the
         * filter was named (phase 14 item 31) */
        if (mcstf < 0 && tune[N265_TUNE_FRAMEWORK] >= 0)
            mcstf = 0;
        /* The frames in flight. setenv does NOT overwrite, so an
         * explicit N265_MAX_FRAME_WORK still wins - and this is the one
         * leg of the tune that moves no bit: output is byte-identical at
         * every thread count and at every K, which is a gate, so what it
         * buys is latency and nothing else. */
        if (tune[N265_TUNE_FRAMEWORK] >= 0) {
            char k[16];
            snprintf(k, sizeof k, "%d", tune[N265_TUNE_FRAMEWORK]);
            setenv("N265_MAX_FRAME_WORK", k, 0);
        }
    }
    /* The picture the preset is being asked about. Some of what it
     * offers is resolution-aware (phase 14 items 12 and 17) and this is
     * the only place that knows the geometry: the source has been opened,
     * so the size is the FILE's, before any padding to the coding tree -
     * which is the size the rule is written in and the one a user
     * names. */
    a->preset_uhd(y.w, y.h);
    if (!had_ref)     nref = a->preset_param_res("ref", nref, y.w, y.h);
    if (!had_bframes) bframes = a->preset_param_res("bframes", bframes,
                                                    y.w, y.h);
    if (!had_badapt)  badapt = a->preset_param_res("b-adapt", badapt,
                                                   y.w, y.h);
    if (!had_la)      lookahead = a->preset_param_res("rc-lookahead",
                                                      lookahead, y.w, y.h);
    /* item 17: remember whether the RESOLUTION rule is what moved the
     * coding tree, by asking the same rung the resolution-blind way. A
     * tree the user named is the user's and the rate switch leaves it
     * alone; one the rule offered is the rule's and the switch may take
     * it back down. */
    int ctu_blind = a->preset_param("ctu", ctu), uhd_tree = 0;
    if (!had_ctu) {
        ctu = a->preset_param_res("ctu", ctu, y.w, y.h);
        uhd_tree = ctu != ctu_blind;
    }
    if (!had_tui)     tu_inter = a->preset_param_res("tu-inter-depth",
                                                     tu_inter, y.w, y.h);
    if (!had_tua)     tu_intra = a->preset_param_res("tu-intra-depth",
                                                     tu_intra, y.w, y.h);
    if (ctu != 16 && ctu != 32 && ctu != 64) {
        fprintf(stderr, "n265: --ctu must be 16, 32 or 64\n");
        return 2;
    }
    /* Structural ranges, checked AFTER the preset has filled in what the
     * command line left out, because a preset can set either of them.
     * The numbers are N265_MAX_BFRAMES and LA_DEPTH_MAX; the CLI does not
     * see the internal headers, so they are repeated here and in usage(). */
    if (bframes < 0 || bframes > 15) {
        fprintf(stderr, "n265: --bframes out of range 0..15\n");
        return 2;
    }
    /* 60 since 2026-09-14 (phase 14 item 5), where 32 was the ceiling
     * that stopped `veryslow` and `placebo` naming x265's own windows of
     * 40 and 60. The knob's cost is unchanged and is linear in the depth:
     * N frames of input held before anything is coded (N/fps of latency)
     * and `depth + 2 + K + bframes` slots of full-resolution source,
     * about 3.6 MB a frame at 1080p. docs/instruments.md, "The deep
     * window". A ceiling is not a recommendation: what each rung sets is
     * priced in docs/phase14.md. */
    if (lookahead < 1 || lookahead > 60) {
        fprintf(stderr, "n265: --rc-lookahead out of range 1..60\n");
        return 2;
    }
    /* N265_MAX_SHOT_WINDOW, repeated here for the same reason the two
     * above are: the CLI does not see the internal headers. */
    /* --pass and --stats are one knob in two halves, the way the VBV's
     * two are: a pass with no file is a pass that cannot do its job, and a
     * file with no pass is a flag that does nothing. Both refusals are made
     * here because neither needs the stream. */
    if (pass < 0 || pass > 3) {
        fprintf(stderr, "n265: --pass is 1 (write the stats), 2 (plan from "
                "them) or 3 (both)\n");
        return 2;
    }
    if (pass && !stats_path) {
        fprintf(stderr, "n265: --pass %d needs --stats <file>\n", pass);
        return 2;
    }
    if (stats_path && !pass) {
        fprintf(stderr, "n265: --stats needs --pass 1, 2 or 3\n");
        return 2;
    }
    if (shot_window < 0 || shot_window > 300) {
        fprintf(stderr, "n265: --shot-window out of range 0..300\n");
        return 2;
    }
    /* The window has to CONTAIN the run it is asked to plan: a mini-GOP
     * of `bframes` leaves plus its anchor is bframes + 1 pictures, and
     * the planner only ever extends a run as far as the queue reaches
     * (`maxa = min(bframes, count - 1)`, build_plan_order). A window
     * shallower than that silently shortens every mini-GOP instead of
     * refusing, which is the one failure mode of a deep --bframes that
     * looks like a quality result. */
    if (lookahead < bframes + 1) {
        fprintf(stderr, "n265: --rc-lookahead %d is too shallow for "
                "--bframes %d: the window must hold the whole mini-GOP, so "
                "--rc-lookahead must be at least %d%s\n",
                lookahead, bframes, bframes + 1,
                la_from_tune ? " (the window of 1 is --tune zerolatency's; "
                               "name --rc-lookahead yourself to widen it)"
                             : "");
        return 2;
    }
    int ctb_log2 = ctu == 16 ? 4 : ctu == 32 ? 5 : 6;
    a->set_mode(pcm ? 1 : 0, qp, ctb_log2, keyint, bframes, aq);
    /* item 17, and it has to be after set_mode, which clears the flag */
    a->set_uhd_tree(uhd_tree);
    /* The flag decides, not the value: `crf >= 0` used to double as the
     * off sentinel here, which is why the twelve QP steps Main10 pays for
     * were reachable through --qp and not through --crf. */
    if (had_crf) a->set_crf(crf);
    if (bitrate > 0) a->set_bitrate(bitrate);
    a->set_ref(nref);
    a->set_badapt(badapt);
    a->set_la(lookahead, cutree);
    a->set_shot_window(shot_window);
    a->set_pass(pass, stats_path);
    a->set_threads(threads);
    /* --pcm is the phase-0 lossless path and --lossless the coded one;
     * asking for both is a contradiction, not a stack. */
    if (pcm && lossless) {
        fprintf(stderr, "n265: --pcm and --lossless are two different "
                "lossless modes; pick one\n");
        return 2;
    }
    a->set_lossless(lossless);
    /* --rdoq-level. Range-checked here because nothing but the string is
     * needed for it, and passed down as x265's number: the library owns
     * the map onto its own knob (src/encoder/encoder.c,
     * n265_set_rdoq_level). Left off, the preset chooses - medium 1,
     * fast/slow/slower 2 - which is what makes this an override and not
     * a default. */
    if (had_rdoq && (rdoq_level < 0 || rdoq_level > 2)) {
        fprintf(stderr, "n265: --rdoq-level must be 0 (off), 1 (final "
                "encode only) or 2 (all RD decisions)\n");
        return 2;
    }
    a->set_rdoq_level(rdoq_level);
    /* --tu-inter-depth / --tu-intra-depth. x265's range is 1..4 and so is
     * this one; a value outside it is refused rather than clamped, for
     * the reason the chroma offsets are. Left off, the preset chooses -
     * 2 / 2 everywhere up to `slower`, which IS the tree every stream
     * this project wrote before the flag existed. */
    if ((tu_inter > 0 && (tu_inter < 1 || tu_inter > 4)) ||
        (tu_intra > 0 && (tu_intra < 1 || tu_intra > 4))) {
        fprintf(stderr, "n265: --tu-inter-depth / --tu-intra-depth must be "
                "1..4 (levels of transform tree; 2 is the default)\n");
        return 2;
    }
    /* ...and the coding tree's own bound. 7.4.3.2.1 puts
     * max_transform_hierarchy_depth_* (which is the flag minus one) at
     * CtbLog2SizeY - MinTbLog2SizeY, so `--ctu 16` admits 3 levels and
     * `--ctu 32` four; ffmpeg refuses the SPS outright past that, which is
     * how this was found. A depth the USER named is refused here rather
     * than clamped, for the reason --cbqpoffs is; one a PRESET named is
     * clamped at encoder open, so `--ctu 16 --preset placebo` still
     * codes. */
    {
        int ctu_log2 = ctu == 16 ? 4 : ctu == 32 ? 5 : 6;
        int tu_cap = ctu_log2 - 1;
        if ((had_tui && tu_inter > tu_cap) || (had_tua && tu_intra > tu_cap)) {
            fprintf(stderr, "n265: --tu-inter-depth / --tu-intra-depth above "
                    "%d needs a coding tree larger than --ctu %d\n",
                    tu_cap, ctu);
            return 2;
        }
    }
    a->set_tu_depth(tu_inter, tu_intra);
    /* --cbqpoffs / --crqpoffs. 7.4.3.3.1 bounds pps_cb_qp_offset and
     * pps_cr_qp_offset at +-12 and x265 takes the same range, so a value
     * outside it is refused rather than clamped: a clamp would encode
     * something the user did not ask for and say nothing about it.
     *
     * NEGATIVE spends MORE on chroma; positive moves bits to luma, which
     * the band's metric cannot see at all and which docs/instruments.md
     * closes as a DEFAULT by design. As a user knob both directions are
     * reachable, and this is the one place that says so. */
    if (cbqpoffs < -12 || cbqpoffs > 12 || crqpoffs < -12 || crqpoffs > 12) {
        fprintf(stderr, "n265: --cbqpoffs and --crqpoffs take -12..12 "
                "(7.4.3.3.1); negative spends more on chroma\n");
        return 2;
    }
    /* a flag not given passes N265_QPOFFS_UNSET, so the preset's row
     * (medium names -1 from 720p up, phase 14 item 51) stays in charge
     * and an explicit 0 is an escape from it */
    a->set_chroma_qpoffs(had_cbq ? cbqpoffs : 100, had_crq ? crqpoffs : 100);
    /* --level / --high-tier. The refusals that need the STREAM - a level
     * too low for the picture, the rate or the DPB - are made at encoder
     * open, where the derivation lives; the one made here is the only one
     * that needs nothing but the string. */
    {
        int idc = 0;
        if (level_str) {
            idc = n265_level_parse(level_str);
            if (!idc) {
                fprintf(stderr, "n265: --level %s is not a level of Annex A "
                        "(1, 2, 2.1, 3, 3.1, 4, 4.1, 5, 5.1, 5.2, 6, 6.1, "
                        "6.2; \"41\" spells 4.1). The raw idc is not a "
                        "spelling: 30 and 60 are each an idc AND a level "
                        "written ten times.\n", level_str);
                return 2;
            }
        }
        a->set_level(idc, high_tier);
    }
    a->set_vui_timing(vui_timing);
    a->set_film_grain(film_grain, fgc_denoise);
    /* Range-checked only where the user actually typed it: the -1 the two
     * carry otherwise is this file's "not given" and is the value that
     * leaves the choice to the preset (medium names 100 / 100 since item
     * 6b). A negative the USER typed is still refused. */
    if ((had_nr_intra && (nr_intra < 0 || nr_intra > 2000)) ||
        (had_nr_inter && (nr_inter < 0 || nr_inter > 2000))) {
        fprintf(stderr, "n265: --nr-intra/--nr-inter take 0..2000 "
                "(x265's range); 0 is off\n");
        return 2;
    }
    a->set_nr(nr_intra, nr_inter);
    /* x265 spells the range 0..4 and its 0 means the filter off. Here 0
     * is refused by name instead: --mcstf with no neighbors is an
     * identity filter that still costs the latency and the queue, and a
     * flag that silently does nothing is the thing this project keeps
     * refusing to ship. */
    if (mcstf_range != -1 && (mcstf_range < 1 || mcstf_range > 4)) {
        fprintf(stderr, "n265: --mcstf-ref-range takes 1..4 neighbours "
                "per side (x265 spells its 0 as --no-mcstf; so do we)\n");
        return 2;
    }
    a->set_mcstf(mcstf, mcstf_range, mcstf_selective);
    a->set_vbv(vbv_maxrate, vbv_bufsize, vbv_init);
    a->set_weight(weightp, weightb);
    /* ---- the pass-through signaling (phase 14 item 1) ---- */
    if (hash < 0 || hash > 3) {
        fprintf(stderr, "n265: --hash is 0 (off), 1 (MD5), 2 (CRC) or "
                "3 (checksum)\n");
        return 2;
    }
    /* --csv-log-level without --csv is a flag that does nothing, and
     * --csv without a level gets x265's own default of 0 (one summary
     * row). The pair is refused the way --pass / --stats is. */
    if (csv_level >= 0 && !csv_path) {
        fprintf(stderr, "n265: --csv-log-level needs --csv <file>\n");
        return 2;
    }
    if (csv_level < 0)
        csv_level = 0;
    if (csv_level > 2) {
        fprintf(stderr, "n265: --csv-log-level is 0 (one summary row for "
                "the run), 1 (a row per frame) or 2 (that, plus the rate "
                "control's columns)\n");
        return 2;
    }
    a->set_metrics(want_psnr, want_ssim);
    /* The head of the options clause is what the run RESOLVED, ahead of
     * the flags it was given: the preset (which is the whole of the
     * knob state a preset sets - the payload names no env knob) and the
     * rate control that won, --bitrate over --crf over --qp, with
     * --lossless and --pcm having no quantizer to name at all. */
    {
        optstr head = { .s = "", .n = 0 };
        char v[32];
        opt_put(&head, "preset", preset ? preset : "medium"); /* the effective preset: medium is the default, and provenance names what ran */
        if (pcm) {
            opt_put(&head, "rc", "pcm");
        } else if (lossless) {
            opt_put(&head, "rc", "lossless");
        } else if (bitrate > 0.0) {
            opt_put(&head, "rc", "abr");
            snprintf(v, sizeof v, "%g", bitrate);
            opt_put(&head, "bitrate", v);
        } else if (had_crf) {
            opt_put(&head, "rc", "crf");
            snprintf(v, sizeof v, "%.2f", crf);
            opt_put(&head, "crf", v);
        } else {
            opt_put(&head, "rc", "cqp");
            snprintf(v, sizeof v, "%d", qp);
            opt_put(&head, "qp", v);
        }
        char all[768];
        snprintf(all, sizeof all, "%s%s%s", head.s, info_opts.n ? " " : "",
                 info_opts.s);
        a->set_info_opts(all);
    }
    a->set_nal_opts(repeat_hdr, aud, eos, eob, info);
    a->set_fingerprint(fingerprint);
    /* The QP bounds are QpY values, so they carry the depth's floor the
     * way --qp and --crf do. The rest of the refusals need the STREAM -
     * whether there is a rate control to bound, whether there is a buffer
     * to bound an access unit against - and are made at encoder open. */
    /* Unnamed, the pair IS the depth's own QpY range, which is what
     * makes it inert: the library applies the bounds only where one of
     * them was narrowed. The floor is -QpBdOffsetY and not 0, so a
     * Main10 encode's twelve extra steps stay reachable. */
    if (!had_qpmin) qpmin = -qp_bd_off;
    if (!had_qpmax) qpmax = 51;
    if (qpmin < -qp_bd_off || qpmin > 51 || qpmax < -qp_bd_off ||
        qpmax > 51) {
        fprintf(stderr, "n265: --qpmin and --qpmax are QpY values, %d..51 "
                "at %d bits (QpBdOffsetY, 7.4.3.3)\n", -qp_bd_off,
                a->depth);
        return 2;
    }
    a->set_sei_opts(recovery_sei, pic_struct, hrd_concat, tlayers);
    a->set_rc_bounds(crf_lo, crf_hi, qpmin, qpmax, strict_cbr,
                     ausize_factor);
    a->set_qpfile(qpfile_path);
    a->set_zones(zones_spec);
    a->set_hash(hash);
    /* --open-gop / --radl (phase 14 item 3). A RADL count past the
     * mini-GOP cannot be honored - there are only ever `--bframes`
     * leading pictures - so it is refused rather than silently capped. */
    if (radl < 0 || radl > bframes) {
        fprintf(stderr, "n265: --radl is 0..%d here (a CRA has at most "
                "--bframes leading pictures)\n", bframes);
        return 2;
    }
    if (radl && !open_gop) {
        fprintf(stderr, "n265: --radl needs --open-gop (RADL pictures are "
                "the leading pictures of a CRA)\n");
        return 2;
    }
    a->set_gop(open_gop, radl);
    /* The default display window is signaled in CHROMA units (7.4.3.2.1)
     * and the flag takes pixels, so an offset that is not a whole number
     * of chroma samples cannot be written. x265 documents the same flag in
     * pixels and then writes them unshifted, which at 4:2:0 signals twice
     * the region it was given; refusing the odd offset is the honest half
     * of matching that spelling. */
    if (win_on) {
        int sx = y.csx, sy = y.csy;
        if ((win_l & ((1 << sx) - 1)) || (win_r & ((1 << sx) - 1)) ||
            (win_t & ((1 << sy) - 1)) || (win_b & ((1 << sy) - 1))) {
            fprintf(stderr, "n265: --display-window offsets are signalled "
                    "in chroma samples, so at this chroma format the "
                    "horizontal ones must be multiples of %d and the "
                    "vertical ones of %d\n", 1 << sx, 1 << sy);
            return 2;
        }
        if (win_l + win_r >= y.w || win_t + win_b >= y.h) {
            fprintf(stderr, "n265: --display-window %d,%d,%d,%d leaves no "
                    "picture inside %dx%d\n", win_l, win_t, win_r, win_b,
                    y.w, y.h);
            return 2;
        }
    }
    /* --video-signal-type-preset expands LAST and only into fields the
     * command line left alone. x265 has the opposite rule - there the
     * preset discards an individual flag given beside it - and this is a
     * deliberate divergence, recorded in docs/phase14.md: a flag the
     * user typed and the encoder then dropped is the worse of the two
     * surprises, and the preset is a shorthand for those same flags. */
    if (had_vsp) {
        if (sig_colorprim < 0)   sig_colorprim   = vsp[N265_VUI_COLORPRIM];
        if (sig_transfer < 0)    sig_transfer    = vsp[N265_VUI_TRANSFER];
        if (sig_colormatrix < 0) sig_colormatrix = vsp[N265_VUI_COLORMATRIX];
        if (sig_range < 0)       sig_range       = vsp[N265_VUI_RANGE];
        if (sig_chromaloc < 0)   sig_chromaloc   = vsp[N265_VSP_CHROMALOC];
        if (!have_md && vsp_md_on) {
            memcpy(md, vsp_md, sizeof md);
            have_md = 1;
        }
    }
    a->set_vui_colour(sig_videoformat, sig_range, sig_colorprim,
                      sig_transfer, sig_colormatrix, sig_chromaloc,
                      sig_overscan);
    a->set_dup_thr(dup_thr);
    a->set_vui_sar(sar_idc, sar_w, sar_h);
    a->set_vui_window(win_on, win_l, win_t, win_r, win_b);
    a->set_hdr_meta(have_md ? md : NULL, maxcll, maxfall);
    a->set_hdr10(hdr10, cll_on, atc);
    a->set_hdr_opt(hdr_opt);
    /* The dynamic metadata describes PQ or HLG pictures, so the stream has
     * to say that's what it holds, or a player that reads the color
     * description first never looks for the metadata. */
    if (h10p_path && (sig_transfer != 16 || sig_colorprim != 9 ||
                      sig_colormatrix != 9 || a->depth != 10)) {
        fprintf(stderr, "n265: --dhdr10-info is HDR10+ metadata, which "
                "rides on HDR10: a 10-bit encode (or --output-depth 10) "
                "with --transfer smpte2084 --colorprim bt2020 "
                "--colormatrix bt2020nc\n");
        return 2;
    }
    if (!rpu_path != !dv_profile) {
        fprintf(stderr, "n265: --dolby-vision-rpu and "
                "--dolby-vision-profile go together\n");
        return 2;
    }
    if (rpu_path) {
        int hlg = sig_transfer == 18 || (sig_transfer == 14 && atc == 18);
        if (a->depth != 10 || y.csp != N265_CSP_I420 || sig_range == 1) {
            fprintf(stderr, "n265: Dolby Vision profile 8 is 10-bit 4:2:0 "
                    "at limited range: give a 4:2:0 input, 10-bit or with "
                    "--output-depth 10, and no --range full\n");
            return 2;
        }
        int ok = dv_profile == 82
                     ? sig_colorprim == 1 && sig_colormatrix == 1 &&
                       sig_transfer == 1
                     : sig_colorprim == 9 && sig_colormatrix == 9 &&
                       (dv_profile == 81 ? sig_transfer == 16 : hlg);
        if (!ok) {
            fprintf(stderr, "n265: Dolby Vision profile %s needs %s\n",
                    dv_profile == 81 ? "8.1" : dv_profile == 82 ? "8.2" : "8.4",
                    dv_profile == 81
                    ? "--colorprim bt2020 --colormatrix bt2020nc --transfer "
                      "smpte2084"
                    : dv_profile == 82
                    ? "--colorprim bt709 --colormatrix bt709 --transfer bt709"
                    : "--colorprim bt2020 --colormatrix bt2020nc and "
                      "--transfer arib-std-b67 (or --transfer bt2020-10 with "
                      "--atc-sei arib-std-b67)");
            return 2;
        }
    }
    /* with the video on stdin, a metadata file there would read the Y4M
     * as metadata */
    if (in_path && !strcmp(in_path, "-")) {
        const char *f[2] = { h10p_path, rpu_path };
        for (int k = 0; k < 2; k++)
            if (f[k] && (!strcmp(f[k], "/dev/stdin") ||
                         !strcmp(f[k], "/dev/fd/0") || !strcmp(f[k], "-"))) {
                fprintf(stderr, "n265: the input is on stdin, so %s can't "
                        "be read from there too\n",
                        k ? "--dolby-vision-rpu" : "--dhdr10-info");
                return 2;
            }
    }
    /* Dolby Vision deliveries are normally HRD-constrained, but a CRF
     * encode is still a valid stream and is what testing and archiving
     * want, so this only says so. */
    if (rpu_path && !(vbv_maxrate > 0.0 && vbv_bufsize > 0.0))
        fprintf(stderr, "n265: WARNING Dolby Vision is normally delivered "
                "under a VBV: consider --vbv-maxrate and --vbv-bufsize\n");
    /* 8.1's base layer is meant to be a whole HDR10 stream */
    if (dv_profile == 81 && !have_md)
        fprintf(stderr, "n265: WARNING a Dolby Vision 8.1 base layer is "
                "HDR10, which carries a mastering display: consider "
                "--master-display\n");
    char dyn_err[256];
    if (h10p_path &&
        n265_h10p_load(h10p_path, &g_h10p_tab, dyn_err, sizeof dyn_err) < 0) {
        fprintf(stderr, "n265: --dhdr10-info: %s\n", dyn_err);
        return 2;
    }
    if (rpu_path &&
        n265_rpu_load(rpu_path, &g_rpu_tab, dyn_err, sizeof dyn_err) < 0) {
        fprintf(stderr, "n265: --dolby-vision-rpu: %s\n", dyn_err);
        return 2;
    }
    g_dyn_seek = seek;
    dyn_apply(a, 0);
    /* --verbose is N265_LEVEL_TRACE by another name: one code path, one
     * line, and a script that wants it without a flag sets the variable.
     * setenv does not overwrite, so an explicit N265_LEVEL_TRACE=0 in the
     * environment still turns it on - presence is what the encoder reads,
     * as it does for every other trace. */
    if (verbose)
        setenv("N265_LEVEL_TRACE", "1", 0);
    n265_param p;
    a->param_default(&p);
    p.width = y.w;
    p.height = y.h;
    p.csp = y.csp;
    p.fps_num = y.fps_num;
    p.fps_den = y.fps_den;
    /* --gop-parallel (phase 14 item 42): above one thread only, and not
     * with the options that need one encoder to see the whole input */
    if (gop_par > 1 && !show_config) {
        long nt = threads > 0 ? threads : sysconf(_SC_NPROCESSORS_ONLN);
        const char *hrd = getenv("N265_VBV_HRD");
        int vbv_on = vbv_maxrate > 0.0 && vbv_bufsize > 0.0;
        const char *why =
            nt < 2 ? "it needs more than one thread"
            : pass ? "--pass plans the whole input on one encoder"
            : qpfile_path ? "--qpfile numbers frames from the stream's start"
            : zones_spec ? "--zones numbers frames from the stream's start"
            : csv_path ? "--csv is one encoder's log"
            : shot_path ? "--shot-stats is one encoder's log"
            : dump_decode_path ? "--dump-recon-decode is one encoder's log"
            : hrd_concat ? "--hrd-concat splices one encoder's stream"
            : vbv_on && hrd && atoi(hrd) ? "N265_VBV_HRD asks for the HRD "
              "timing SEI, which a joined stream does not carry"
            : NULL;
        if (why) {
            fprintf(stderr, "n265: --gop-parallel %d: coding serially, "
                    "since %s\n", gop_par, why);
        } else {
            FILE *gout = fopen(out_path, "wb");
            if (!gout) {
                fprintf(stderr, "n265: cannot open %s: %s\n", out_path,
                        strerror(errno));
                return 1;
            }
            FILE *gdump = NULL;
            if (dump_path) {
                gdump = fopen(dump_path, "wb");
                if (!gdump) {
                    fprintf(stderr, "n265: cannot open %s: %s\n", dump_path,
                            strerror(errno));
                    return 1;
                }
            }
            long gn = 0, gnm = 0;
            double gmet[6] = { 0, 0, 0, 0, 0, 0 };
            int gseg = 0, gk = 0;
            int grc = gp_run(a, &y, fin != stdin ? in_path : NULL, &p,
                             gop_par, (int)nt, threads, keyint,
                             max_frames, bitrate > 0.0, bitrate, vbv_on,
                             repeat_hdr, gout, gdump, want_psnr || want_ssim,
                             &gn, gmet, &gnm, &gseg, &gk);
            if (gdump)
                fclose(gdump);
            if (fclose(gout) != 0 && !grc) {
                fprintf(stderr, "n265: write failed: %s\n", strerror(errno));
                grc = 1;
            }
            if (grc)
                return 1;
            if (gnm > 0 && want_psnr)
                fprintf(stderr, "n265: PSNR over %ld frames: y %.4f u %.4f "
                        "v %.4f avg %.4f dB\n", gnm, gmet[0], gmet[1],
                        gmet[2], gmet[3]);
            if (gnm > 0 && want_ssim)
                fprintf(stderr, "n265: SSIM-Y over %ld frames: %.6f (%.4f "
                        "dB)\n", gnm, gmet[4], gmet[5]);
            fprintf(stderr, "n265: gop-parallel: %d segment%s, %d instance%s "
                    "x %d thread%s\n", gseg, gseg == 1 ? "" : "s",
                    gseg > 1 ? (gop_par < nt ? gop_par : (int)nt) : 1,
                    gseg > 1 ? "s" : "", gk, gk == 1 ? "" : "s");
            free(y.pix16);
            free(y.pix8);
            free(y.frame);
            if (fin != stdin)
                fclose(fin);
            if (y.clipped)
                fprintf(stderr, "n265: WARNING %ld input samples were above "
                        "the declared %d-bit range and were clamped\n",
                        y.clipped, y.depth);
            const char *gfmt = y.csp == N265_CSP_I444 ? "4:4:4"
                             : y.csp == N265_CSP_I422 ? "4:2:2" : "4:2:0";
            fprintf(stderr, "n265: %ld frames, %dx%d %s %d-bit\n", gn, y.w,
                    y.h, gfmt, y.depth);
            return dyn_check(gn);
        }
    }
    n265_encoder *enc = a->encoder_open(&p);
    if (!enc) {
        fprintf(stderr, "n265: encoder_open failed (dims %dx%d)\n", y.w, y.h);
        return 1;
    }

    /* --show-config / --verbose (phase 14 item 13). Here, because the
     * table is what the encoder RESOLVED and the level, the tier and the
     * DPB are solved inside the open above; before the output file is
     * touched, because --show-config must not truncate the `-o` of a run
     * it is not going to perform. stderr, because stdout may be the
     * bitstream. */
    if (verbose || show_config)
        a->show_config(enc, stderr, y.depth);
    if (show_config) {
        a->encoder_close(enc);
        if (fin != stdin)
            fclose(fin);
        free(y.pix16);
        free(y.pix8);
        free(y.frame);
        return 0;
    }

    if (shot_path)
        a->set_shot_stats(enc, shot_path);
    if (csv_path)
        a->set_csv(enc, csv_path, csv_level);
    FILE *fdump = NULL;
    if (dump_path) {
        fdump = fopen(dump_path, "wb");
        if (!fdump) {
            fprintf(stderr, "n265: cannot open %s: %s\n", dump_path,
                    strerror(errno));
            return 1;
        }
        a->set_dump(enc, fdump);
    }
    FILE *fdump2 = NULL;
    if (dump_decode_path) {
        fdump2 = fopen(dump_decode_path, "wb");
        if (fdump2)
            a->set_dump_decode(enc, fdump2);
    }
    FILE *fout = fopen(out_path, "wb");
    if (!fout) {
        fprintf(stderr, "n265: cannot open %s: %s\n", out_path, strerror(errno));
        return 1;
    }

    long n = 0;
    int r = 0;
    rd_ring rq;
    int nslot = rd_slots(&y, threads);
    if (nslot && rd_open(&rq, &y, nslot, max_frames) != 0) {
        fprintf(stderr, "n265: cannot start the input reader\n");
        return 1;
    }
    for (;;) {
        const y4m_t *src = &y;
        if (nslot) {
            if (!(src = rd_next(&rq, &r)))
                break;
        } else if ((max_frames >= 0 && n >= max_frames) ||
                   (r = y4m_read(&y)) <= 0) {
            break;
        }
        n265_picture pic = {
            .plane = { src->plane[0], src->plane[1], src->plane[2] },
            .stride = { y.w, y.w >> y.csx, y.w >> y.csx },
            .pts = n,
        };
        n265_nal *nal;
        int nnal;
        int bytes = a->encoder_encode(enc, &nal, &nnal, &pic);
        if (bytes < 0) {
            fprintf(stderr, "n265: encode failed on frame %ld\n", n);
            return 1;
        }
        if (nslot)
            rd_release(&rq);
        for (int i = 0; i < nnal; i++)
            fwrite(nal[i].payload, 1, (size_t)nal[i].size, fout);
        n++;
    }
    if (nslot)
        rd_close(&rq, &y);
    if (r < 0) {
        fprintf(stderr, "n265: truncated input after frame %ld\n", n);
        return 1;
    }

    /* flush the lookahead queue */
    for (;;) {
        n265_nal *nal;
        int nnal;
        int r2 = a->encoder_encode(enc, &nal, &nnal, NULL);
        if (r2 <= 0 && nnal == 0)
            break;
        for (int i = 0; i < nnal; i++)
            fwrite(nal[i].payload, 1, (size_t)nal[i].size, fout);
    }
    /* --psnr / --ssim: the pooled numbers, before the encoder is closed
     * and while it still holds them. The pooling is ffmpeg's - the mean
     * squared error is summed over frames and the logarithm taken once -
     * so these are the numbers `ffmpeg -i out.hevc -i source -lavfi psnr`
     * prints, to a hundredth of a dB. */
    if (want_psnr || want_ssim) {
        double met[6];
        long nmet = a->metrics(enc, met);
        if (nmet > 0 && want_psnr)
            fprintf(stderr, "n265: PSNR over %ld frames: y %.4f u %.4f "
                    "v %.4f avg %.4f dB\n", nmet, met[0], met[1], met[2],
                    met[3]);
        if (nmet > 0 && want_ssim)
            fprintf(stderr, "n265: SSIM-Y over %ld frames: %.6f (%.4f dB)\n",
                    nmet, met[4], met[5]);
    }
    g_dyn_lost += a->dyn_lost(enc);
    a->encoder_close(enc);
    if (fdump)
        fclose(fdump);
    if (fdump2)
        fclose(fdump2);
    free(y.pix16);
    free(y.pix8);
    free(y.frame);
    if (fin != stdin)
        fclose(fin);
    if (fclose(fout) != 0) {
        fprintf(stderr, "n265: write failed: %s\n", strerror(errno));
        return 1;
    }
    if (y.clipped)
        fprintf(stderr, "n265: WARNING %ld input samples were above the "
                "declared %d-bit range and were clamped\n", y.clipped,
                y.depth);
    const char *fmt = y.csp == N265_CSP_I444 ? "4:4:4"
                    : y.csp == N265_CSP_I422 ? "4:2:2" : "4:2:0";
    if (y.out_depth != y.depth)
        fprintf(stderr, "n265: %ld frames, %dx%d %s %d-bit in, coded %d-bit "
                "(%s)\n", n, y.w, y.h, fmt, y.depth, y.out_depth,
                y.out_depth > y.depth ? "upshifted by 2"
                : y.dither ? "ordered dither" : "truncated by 2");
    else
        fprintf(stderr, "n265: %ld frames, %dx%d %s %d-bit\n", n, y.w, y.h,
                fmt, y.depth);
    return dyn_check(n);
}
