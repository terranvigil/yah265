/*
 * y265: Y4M in, Annex-B HEVC out.
 * Copyright (c) 2026, the yah265 authors. GPL-2.0-or-later; see LICENSE.
 */

#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#ifdef __APPLE__
#include <mach-o/dyld.h>
#include <malloc/malloc.h>
#endif
#include <limits.h>
#include <sys/types.h>
#include <unistd.h>

#include "yah265.h"
#include "ctl_log.h"
#include "hdr_dyn.h"

/* internal knobs, resolved from the library (not yet public API) */
void n265_encoder_set_dump_decode(n265_encoder *e, FILE *f);
void n265_encoder_set_dump(n265_encoder *e, FILE *f);
void n265_encoder_set_shot_stats(n265_encoder *e, const char *path);
void n265_set_shot_window(int n);
/* --shot-list / --shot-scan (src/encoder/shotlist.h). The list helpers and
 * the scan are depth-blind and always run on the 8-bit library. */
int n265_encoder_set_shot_list(n265_encoder *e, const int *starts, int n);
#include "encoder/shotlist.h"
typedef struct n265_shotscan n265_shotscan;
n265_shotscan *n265_shotscan_open(const n265_param *src);
int n265_shotscan_push(n265_shotscan *s, const uint8_t *luma, int w, int h,
                       int depth);
int n265_shotscan_finish(n265_shotscan *s, n265_shotlist *out);
void n265_shotscan_close(n265_shotscan *s);
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
 * configuration, one `y265 [info]: key : value` row at a time. Printed
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
void n265_encoder_set_frame_stats(n265_encoder *e, const char *path);
long n265_encoder_metrics(n265_encoder *e, double out[6]);
long n265_encoder_dyn_lost(const n265_encoder *e);
/* Two-pass (phase 12 item 4): 1 writes the stats sidecar, 2 plans from it,
 * 3 does both. The path outlives the encoder - it is argv. */
void n265_set_pass(int pass, const char *stats);
void n265_set_lossless(int on);
/* --rdoq-level <0|1|2>, numbered as x265's flag. -1 is "not given". */
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
 * command lines. y265 links both archives and picks the encoder from the
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
n265_config *n265_config_new_10(void);
void n265_config_free_10(n265_config *c);
int  n265_config_parse_10(n265_config *c, const char *name,
                          const char *value);
void n265_config_note_10(n265_config *c, const char *name,
                         const char *value);
int  n265_config_apply_10(n265_config *c, n265_param *p);
int  n265_config_get_10(const n265_config *c, const char *key, double *v);
const char *n265_config_get_str_10(const n265_config *c, const char *key);
void n265_encoder_set_dump_decode_10(n265_encoder *e, FILE *f);
void n265_encoder_set_dump_10(n265_encoder *e, FILE *f);
void n265_encoder_set_shot_stats_10(n265_encoder *e, const char *path);
int n265_encoder_set_shot_list_10(n265_encoder *e, const int *starts, int n);
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
void n265_encoder_set_frame_stats_10(n265_encoder *e, const char *path);
void n265_control_init_10(n265_control_t *ev, int kind);
const char *n265_control_strerror_10(int err);
int  n265_encoder_control_10(n265_encoder *e, n265_control_t *ev);
int  n265_encoder_control_check_10(n265_encoder *e, const n265_control_t *ev,
                                   int n, int *bad_index);
int  n265_encoder_status_10(n265_encoder *e, n265_status_t *st);
int  n265_encoder_controls_applied_10(n265_encoder *e, n265_control_t *out,
                                      int max);
long n265_encoder_metrics_10(n265_encoder *e, double out[6]);
long n265_encoder_dyn_lost_10(const n265_encoder *e);
const char *n265_encoder_backend_10(const n265_encoder *e);
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
    int (*set_shot_list)(n265_encoder *, const int *, int);
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
    const char *(*backend)(const n265_encoder *);
    /* the coding options, parsed and resolved by the library */
    n265_config *(*config_new)(void);
    void (*config_free)(n265_config *);
    int  (*config_parse)(n265_config *, const char *, const char *);
    void (*config_note)(n265_config *, const char *, const char *);
    int  (*config_apply)(n265_config *, n265_param *);
    int  (*config_get)(const n265_config *, const char *, double *);
    const char *(*config_get_str)(const n265_config *, const char *);
    /* --frame-stats and live control */
    void (*set_frame_stats)(n265_encoder *, const char *);
    void (*control_init)(n265_control_t *, int);
    const char *(*control_strerror)(int);
    int  (*encoder_control)(n265_encoder *, n265_control_t *);
    int  (*control_check)(n265_encoder *, const n265_control_t *, int, int *);
    int  (*controls_applied)(n265_encoder *, n265_control_t *, int);
    int  (*status)(n265_encoder *, n265_status_t *);
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
    n265_encoder_set_shot_stats, n265_encoder_set_shot_list,
    n265_set_metrics, n265_set_nal_opts, n265_set_info_opts,
    n265_set_rc_bounds,
    n265_set_sei_opts, n265_set_qpfile, n265_set_zones,
    n265_encoder_set_csv, n265_encoder_metrics, n265_set_fingerprint,
    n265_show_config, n265_encoder_dyn_lost, n265_encoder_backend,
    n265_config_new, n265_config_free, n265_config_parse, n265_config_note,
    n265_config_apply, n265_config_get, n265_config_get_str,
    n265_encoder_set_frame_stats, n265_control_init, n265_control_strerror,
    n265_encoder_control, n265_encoder_control_check,
    n265_encoder_controls_applied, n265_encoder_status,
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
    n265_encoder_set_shot_stats_10, n265_encoder_set_shot_list_10,
    n265_set_metrics_10, n265_set_nal_opts_10, n265_set_info_opts_10,
    n265_set_rc_bounds_10,
    n265_set_sei_opts_10, n265_set_qpfile_10, n265_set_zones_10,
    n265_encoder_set_csv_10, n265_encoder_metrics_10,
    n265_set_fingerprint_10, n265_show_config_10,
    n265_encoder_dyn_lost_10, n265_encoder_backend_10,
    n265_config_new_10, n265_config_free_10, n265_config_parse_10,
    n265_config_note_10, n265_config_apply_10, n265_config_get_10,
    n265_config_get_str_10,
    n265_encoder_set_frame_stats_10, n265_control_init_10,
    n265_control_strerror_10, n265_encoder_control_10,
    n265_encoder_control_check_10, n265_encoder_controls_applied_10,
    n265_encoder_status_10,
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
        fprintf(stderr, "y265: %dx%d: the encoder needs even dimensions\n",
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
        fprintf(stderr, "y265: --output-depth %d on %d-bit input (%s): the "
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
        fprintf(stderr, "y265: unsupported input format C%s "
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

static void usage(FILE *out)
{
    fputs(
        "usage: y265 [options] <input.y4m|input.yuv|->\n"
        "       y265 bench <video> [options]   compare with x265 on your clip\n",
        out);
    fputs(
        "\n"
        "The input picks the profile: 8-bit 4:2:0 is coded Main, 10-bit 4:2:0\n"
        "Main10, 4:4:4 Main 4:4:4 (or 4:4:4 10), and 4:2:2 Main 4:2:2 10 (HEVC\n"
        "has no 8-bit 4:2:2 profile). --output-depth codes at another depth.\n",
        out);
    fputs(
        "\n"
        "Input and output\n"
        "  -o <file>             output Annex B bitstream (default out.hevc)\n"
        "  --frames <n>          encode at most n frames\n"
        "  --seek <n>            skip the first n input frames\n"
        "  --output-depth <8|10> coded bit depth (default: the input's). 10 on 8-bit\n"
        "                        input shifts samples up two bits; 8 on 10-bit input\n"
        "                        drops two bits, truncated unless --dither is given\n"
        "  --dither              with --output-depth 8 on 10-bit input, use a 2x2\n"
        "                        ordered dither instead of truncation\n"
        "  --dump-recon <file>   write the reconstructed pictures as raw YUV\n",
        out);
    fputs(
        "\n"
        "Raw input, selected by --input-res or a .yuv name. Give all of it: a raw\n"
        "file has no header.\n"
        "  --input-res <WxH>     picture size, e.g. 352x288\n"
        "  --input-csp <c>       i420, i422 or i444 (or 1, 2, 3); i400 is not supported\n"
        "  --fps <N[/D]>         frame rate: 30, 30000/1001 or 29.97\n"
        "  --input-depth <8|10>  bits per sample (default 8); 10-bit is 16-bit\n"
        "                        little-endian, as ffmpeg's yuv420p10le\n",
        out);
    fputs(
        "\n"
        "Presets and threads\n"
        "  --preset <name>       fast, medium (default), slow or slower. x265's other\n"
        "                        preset names are refused with the nearest one here.\n"
        "                        Any option you give overrides the preset.\n"
        "  -t, --tune <name>     zerolatency: no B frames, a one-frame lookahead and\n"
        "                        no cu-tree. psnr, ssim, grain and fastdecode are\n"
        "                        refused; each refusal says why.\n"
        "  --threads <n>         worker threads, 0 for one per CPU (default 0).\n"
        "                        --threads 1 writes the same stream on every run.\n"
        "                        Above one thread the stream can differ between runs\n"
        "                        and thread counts.\n"
        "  --gop-parallel <n>    code the input as segments on n encoders at once,\n"
        "                        joined at IDRs (default 0, off). Needs more than one\n"
        "                        thread; not with --pass, --qpfile, --zones, --csv,\n"
        "                        --frame-stats, --shot-stats, --hrd-concat or live\n"
        "                        control. The joined stream carries no HRD timing\n"
        "                        SEI. With --shot-list the segments start on\n"
        "                        listed shots.\n",
        out);
    fputs(
        "\n"
        "Hardware\n"
        "  --hw <mode>           off (default), auto or videotoolbox: encode with the\n"
        "                        Mac's HEVC hardware through VideoToolbox. The stream\n"
        "                        is the hardware's. Options it can't honor are listed\n"
        "                        once at open. auto falls back to this encoder with one\n"
        "                        line; videotoolbox fails instead.\n"
        "  --hw-strict           with --hw, refuse instead of listing those options\n",
        out);
    fputs(
        "\n"
        "Rate control\n"
        "  --qp <n>              constant QP (default 26): 0..51, or -12..51 at 10-bit\n"
        "  --crf <f>             constant rate factor, same range as --qp; overrides\n"
        "                        --qp. It accounts for frame rate: 1.8 QP coarser per\n"
        "                        doubling, neutral at 25 fps.\n"
        "  --bitrate <k>         average bitrate in kbit/s, one pass; overrides --crf\n"
        "  --vbv-maxrate <k>     decoder buffer fill rate in kbit/s. With --vbv-bufsize\n"
        "                        it caps --bitrate or --crf and writes HRD parameters.\n"
        "                        Not with --qp, --pcm or --lossless.\n"
        "  --vbv-bufsize <k>     decoder buffer size in kbit\n"
        "  --vbv-init <f>        starting buffer fill, a fraction in (0, 1]\n"
        "                        (default 0.9)\n"
        "  --strict-cbr          constant rate under --bitrate with a VBV: underspent\n"
        "                        access units are padded with filler data\n"
        "  --max-ausize-factor <f>  cap one access unit at f of the buffer, 0.5..1.0\n"
        "                        (default 1.0, no cap). Needs a VBV.\n"
        "  --crf-min <f>         lowest rate factor --crf may code at\n"
        "  --crf-max <f>         highest rate factor --crf may code at\n"
        "  --qpmin <n>           lowest frame QP the rate control may pick\n"
        "  --qpmax <n>           highest frame QP the rate control may pick\n"
        "  --pass <1|2|3>        two-pass: 1 writes --stats, 2 reads it and plans the\n"
        "                        whole file (needs --bitrate), 3 does both\n"
        "  --stats <file>        the two-pass stats file\n"
        "  --qpfile <file>       force frame types and QPs, one \"<frame> <type> [<qp>]\"\n"
        "                        per line in display order, # for comments. Types: I\n"
        "                        or K (key frame), i (non-IDR intra), - (encoder's\n"
        "                        choice). P, B and b are refused: the encoder picks\n"
        "                        the GOP structure.\n"
        "  --zones <spec>        \"start,end,q=N\" or \"start,end,b=F\" zones joined by /,\n"
        "                        in display order: q=N fixes the QP, b=F scales the\n"
        "                        bits by F. Zones may not overlap.\n"
        "  --aq <0|1>            adaptive quantization (default 1)\n"
        "  --cutree <0|1>        lookahead-driven QP offsets for referenced blocks\n"
        "                        (default 1)\n"
        "  --rc-lookahead <n>    lookahead depth, 1..60 frames. Default: 12 at fast;\n"
        "                        20 at medium from 720p up and 12 below; 20 at slow;\n"
        "                        25 at slower\n"
        "  --shot-window <n>     shot planner for --crf: buffer up to n frames (max\n"
        "                        300) and balance the base QP across shots. Adds n\n"
        "                        frames of latency and memory. Default 0, off.\n",
        out);
    fputs(
        "\n"
        "Structure\n"
        "  --keyint <n>          maximum key frame interval (default 250). A scene cut\n"
        "                        restarts the count.\n"
        "  --shot-scan <file>    scan the input for shots and write a shot list, then\n"
        "                        exit. The scan runs at one size, so every resolution\n"
        "                        of a source gets the same list.\n"
        "  --shot-list <file>    put a key frame at the start of each listed shot.\n"
        "                        The encoder's own cut detector is off. The list is\n"
        "                        refused on an input it wasn't made from.\n"
        "  --shot-keys           put a key frame on each cut the encoder finds\n"
        "  --open-gop            code periodic key frames as CRA (default);\n"
        "                        --no-open-gop codes IDR. Scene cuts are always IDR.\n"
        "  --radl <n>            leading pictures of a CRA coded as RADL (default 0)\n"
        "  --bframes <0..15>     B frames between anchors, hierarchical (default 7).\n"
        "                        Above 7, --rc-lookahead must be at least bframes + 1.\n"
        "  --b-adapt <0-4>       B run length: 0 fixed, 1 by motion (default),\n"
        "                        2 to 4 experimental cost models\n"
        "  --ref <n>             references per list, 1..5 (default 1; medium uses 2\n"
        "                        at 2160p, slower 2). The level rises to fit the DPB.\n"
        "  --ctu <16|32|64>      coding tree block size. Default: 32 at fast and at\n"
        "                        medium below 1080p; 64 at medium from 1080p up, slow\n"
        "                        and slower\n"
        "  --tu-inter-depth <1..4>  transform tree levels in inter CUs (default 2)\n"
        "  --tu-intra-depth <1..4>  transform tree levels in intra CUs (default 2)\n"
        "  --rdoq-level <0|1|2>  rate-distortion optimized quantization: 0 off, 1 in\n"
        "                        the final encode only (medium), 2 in every mode\n"
        "                        decision (fast, slow, slower)\n"
        "  --cbqpoffs <n>        Cb QP offset from luma, -12..12. Negative spends more\n"
        "                        on chroma. Default 0, except medium: -1, or -2 at\n"
        "                        720p below 1080p, or -6 on HDR.\n"
        "  --crqpoffs <n>        Cr QP offset, the same way\n"
        "  -w, --weightp         weighted prediction in P slices, for fades and\n"
        "                        lighting changes (default on, off at fast;\n"
        "                        --no-weightp)\n"
        "  --weightb             weighted prediction in B slices (default off)\n"
        "  --dup-threshold <f>   code a frame within f luma levels per lowres pixel\n"
        "                        of a reference as all-skip, for telecined or\n"
        "                        frame-doubled content (default 0, exact repeats only)\n"
        "  --pcm                 code every CU as I_PCM (lossless, very large)\n"
        "  --lossless            lossless coding: transquant bypass on every CU.\n"
        "                        Rate control, RDOQ, AQ, psy and loop filters are off.\n"
        "  --intra-refresh       no key frame after the first: a column of intra\n"
        "                        blocks sweeps the picture every --keyint frames,\n"
        "                        so a decoder can join at any sweep. P frames only\n"
        "  --scaling-list <s>    not supported; the refusal says why\n",
        out);
    fputs(
        "\n"
        "Filters\n"
        "  --film-grain <s>      estimate the source's film grain and write a film\n"
        "                        grain characteristics SEI, so a decoder that\n"
        "                        supports it adds grain on output. s scales the\n"
        "                        estimate (1.0 as measured, 0 off). 8-bit 4:2:0 only.\n"
        "  --film-grain-denoise <0|1>  with --film-grain, also remove the grain from\n"
        "                        the coded picture to save bits (default 0)\n"
        "  --nr-intra <0-2000>   noise reduction on intra transform coefficients\n"
        "                        (default 0, off)\n"
        "  --nr-inter <0-2000>   the same on inter blocks (default 0, off)\n"
        "  --mcstf               motion-compensated temporal filter on the pictures\n"
        "                        others predict from, for noisy sources (default off)\n"
        "  --mcstf-ref-range <1-4>  neighbor frames per side for --mcstf (default 2)\n"
        "  --selective-mcstf     skip --mcstf on pictures that measure clean. Use it\n"
        "                        with --mcstf on mixed content: flat animation loses\n"
        "                        quality to the filter.\n",
        out);
    fputs(
        "\n"
        "Headers and SEI (all off by default unless noted)\n"
        "  --repeat-headers      VPS, SPS and PPS before every key frame\n"
        "  --aud                 access unit delimiter NAL before every access unit\n"
        "  --eos                 end of sequence NAL before every IDR after the first\n"
        "                        and at the end\n"
        "  --eob                 end of bitstream NAL at the end\n"
        "  --idr-recovery-sei    recovery point SEI at every key frame\n"
        "  --pic-struct <n>      pic_timing SEI pic_struct, 0..12 (0 frame, 7 frame\n"
        "                        doubling, 8 frame tripling). Needs a VBV.\n"
        "  --hrd-concat          mark the first buffering period SEI as a splice\n"
        "                        point. Needs a VBV.\n"
        "  --no-hrd              leave the HRD timing out of the stream (written by\n"
        "                        default with a VBV and a frame rate). A live encode\n"
        "                        that raises its rate needs it.\n"
        "  --temporal-layers     put non-reference B frames in temporal sub-layer 1,\n"
        "                        so a player can drop them to halve the frame rate\n"
        "  --hash <0-3>          decoded picture hash SEI: 0 off (default), 1 MD5,\n"
        "                        2 CRC, 3 checksum\n"
        "  --no-info             drop the SEI naming the encoder, its version and its\n"
        "                        options (on by default; --info)\n"
        "  --no-fingerprint      drop the stream fingerprint, a mark in an unused\n"
        "                        merge index choice that costs no bits and changes no\n"
        "                        sample (on by default)\n"
        "  --vui-timing          write the frame rate in the SPS VUI (default on;\n"
        "                        --no-vui-timing)\n"
        "  --level <l>           signal this level (1 .. 6.2; \"41\" is 4.1). Refused\n"
        "                        if the stream exceeds it. Default: derived.\n"
        "  --high-tier           signal the High tier (level 4 and up)\n",
        out);
    fputs(
        "\n"
        "Colour and HDR metadata (signalling only; no coded sample changes)\n"
        "  --video-signal-type-preset <name[:volume]>  set the color flags at once,\n"
        "                        using x265's names: BT601_525, BT601_626, BT709_YCC,\n"
        "                        BT709_RGB, BT2020_YCC_NCL, BT2020_RGB, BT2100_PQ_YCC,\n"
        "                        BT2100_PQ_ICTCP, BT2100_PQ_RGB, BT2100_HLG_YCC,\n"
        "                        BT2100_HLG_RGB, FR709_RGB, FR2020_RGB, FRP3D65_YCC.\n"
        "                        A color flag given beside it wins. The PQ systems\n"
        "                        take :P3D65x1000n0005 or :P3D65x4000n005.\n"
        "  --colorprim <p>       color primaries: bt709, unknown, bt470m, bt470bg,\n"
        "                        smpte170m, smpte240m, film, bt2020, smpte428,\n"
        "                        smpte431, smpte432\n"
        "  --transfer <t>        transfer characteristics: bt709, unknown, bt470m,\n"
        "                        bt470bg, smpte170m, smpte240m, linear, log100,\n"
        "                        log316, iec61966-2-4, bt1361e, iec61966-2-1,\n"
        "                        bt2020-10, bt2020-12, smpte2084, smpte428,\n"
        "                        arib-std-b67\n"
        "  --colormatrix <m>     matrix coefficients: gbr, bt709, unknown, fcc,\n"
        "                        bt470bg, smpte170m, smpte240m, ycgco, bt2020nc,\n"
        "                        bt2020c, smpte2085, chroma-derived-nc,\n"
        "                        chroma-derived-c, ictcp, ipt-pq-c2\n"
        "  --range <r>           limited or full\n"
        "  --videoformat <f>     component, pal, ntsc, secam, mac or unknown\n"
        "  --chromaloc <0..5>    chroma sample location (2 for BT.2020 and BT.2100)\n"
        "  --overscan <o>        unknown (default), show or crop\n"
        "  --sar <w:h|n>         sample aspect ratio, as a ratio or a table index 0..16\n"
        "  --display-window <l,t,r,b>  display cropping offsets in pixels\n"
        "  --master-display <s>  mastering display SEI: G(x,y)B(x,y)R(x,y)WP(x,y)\n"
        "                        L(max,min), chromaticities in 0.00002 units and\n"
        "                        luminance in 0.0001 cd/m2\n"
        "  --max-cll <c,f>       content light level SEI: \"maxcll,maxfall\" in cd/m2\n"
        "  --hdr10 / --no-hdr10  force the two SEIs above on or off (default: on when\n"
        "                        either is given). Written at every key frame.\n"
        "  --no-cll              drop the content light level SEI\n"
        "  --atc-sei <t>         alternative transfer characteristics SEI; values as\n"
        "                        --transfer\n"
        "  --dhdr10-info <json>  HDR10+ dynamic metadata per picture, from the JSON\n"
        "                        hdr10plus_tool writes. Needs a 10-bit HDR10 encode.\n"
        "  --dolby-vision-rpu <file>  Dolby Vision profile 8 RPU per picture, from\n"
        "                        the file dovi_tool writes. Needs\n"
        "                        --dolby-vision-profile.\n"
        "  --dolby-vision-profile <p>  8.1 (HDR10 base), 8.2 (SDR BT.709 base) or 8.4\n"
        "                        (HLG base); the color flags must match\n"
        "  --hdr-opt / --no-hdr-opt  medium's HDR tuning. On by default when\n"
        "                        --transfer or --atc-sei names smpte2084 or\n"
        "                        arib-std-b67. --hdr10-opt is the same switch.\n"
        "  Metadata files are read in input frame order and --seek skips entries\n"
        "  with frames. A file shorter than the encode fails the run.\n",
        out);
    fputs(
        "\n"
        "Reporting\n"
        "  --psnr                report PSNR at the end, per plane and averaged\n"
        "  --ssim                report luma SSIM at the end\n"
        "  --csv <file>          write statistics as CSV\n"
        "  --csv-log-level <0|1|2>  0 one row per run (default), 1 adds a row per\n"
        "                        frame, 2 adds rate control columns\n"
        "  --shot-stats <file>   write per-shot statistics as JSON Lines\n"
        "  --frame-stats <file>  write one JSON line per coded frame, with the live\n"
        "                        control events that took effect on it\n"
        "  --verbose, -v         print the resolved configuration, level and tier,\n"
        "                        then encode\n"
        "  --show-config         print the resolved configuration and exit\n"
        "  --version             print the version\n",
        out);
    fputs(
        "\n"
        "Live control\n"
        "  --control <file>      apply the events in a control log, one per line:\n"
        "                        \"<frame> rate|crf|qp|key|zone <name>=<value>...\"\n"
        "  --control-fd <n>      read the same lines live from descriptor n, before\n"
        "                        each frame; \"next\" names the next frame\n"
        "  --control-out <file>  write every event that took effect, as a log that\n"
        "                        replays to the same stream through --control\n",
        out);
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
    const int *ls;              /* --shot-list starts, input display order */
    int nls;
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
            fprintf(stderr, "y265: ERROR %s covers %ld of the %ld frames "
                    "encoded; the last %ld have none\n", flag[k], have,
                    frames, frames - have);
            rc = 1;
        } else if (have > frames) {
            fprintf(stderr, "y265: WARNING %s covers %ld frames and %ld "
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
        if (j->nls) {
            int *rel = malloc((size_t)j->nls * sizeof *rel), nr = 0;
            for (int k = 0; rel && k < j->nls; k++)
                if (j->ls[k] > start && j->ls[k] < end)
                    rel[nr++] = j->ls[k] - start;
            if (!rel || a->set_shot_list(e, rel, nr) != 0)
                s->err = 1;
            free(rel);
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
            fprintf(stderr, "y265: cannot read %s at frame %d\n", j->path,
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
                        fprintf(stderr, "y265: cannot read frame %d of "
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
                fprintf(stderr, "y265: encode failed on frame %d\n", i);
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
    int lo = j->nls ? b + 1 : b + (L * 6 + 9) / 10;
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
                  double met[6], long *nmet, int *nseg_out, int *k_out,
                  const int *ls, int nls)
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
    j->ls = ls;
    j->nls = nls;
    if (nls)
        j->cuts = 1;
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
    int sc_ok = j->cuts && !nls && gp_sc_init(&sc, y->w, y->h) == 0;
    if (!sc_ok && !nls)
        j->cuts = 0;
    int lk = 0;                 /* the next listed start to reach */
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
            fprintf(stderr, "y265: truncated input after frame %ld\n", n);
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
            if (nls) {
                while (lk < nls && ls[lk] < n)
                    lk++;
                cut[n] = lk < nls && ls[lk] == n;
            } else if (j->cuts) {
                const void *lu = gp_scan_luma(j, y, n);
                if (!lu) {
                    fprintf(stderr, "y265: cannot read frame %ld\n", n);
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
            if (nls) {
                while (lk < nls && ls[lk] < n)
                    lk++;
                cut[n] = lk < nls && ls[lk] == n;
            } else if (j->cuts) {
                cut[n] = (unsigned char)gp_sc_cut(&sc,
                              gp_sc_push(&sc, f, j->pw[0], j->bps));
            }
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

/* What the command line handles itself: the input, the outputs, the
 * reports and the schedules. Every coding option goes to the library's
 * parser (n265_config_parse), which the ffmpeg wrapper calls too. */
typedef struct {
    const char *in_path, *out_path, *dump_path, *shot_path;
    const char *dump_decode_path, *csv_path;
    const char *shot_scan, *shot_list;
    const char *fstats_path, *ctl_path, *ctl_out_path;
    int ctl_fd;
    long max_frames, seek;
    int out_depth, in_w, in_h, in_csp, in_depth, in_fps_num, in_fps_den;
    int dither, csv_level, gop_par, verbose, show_config;
    int threads, want_psnr, want_ssim;
} cli_opts;

/* -1 to go on, or the exit code */
static int cli_parse(const n265_api *a, n265_config *cfg, int argc,
                     char **argv, cli_opts *o)
{
    memset(o, 0, sizeof *o);
    o->out_path = "out.hevc";
    o->max_frames = -1;
    o->in_csp = -1;
    o->csv_level = -1;
    o->ctl_fd = -1;
    for (int i = 1; i < argc; i++) {
        const int argi = i;
        int mine = 1;
        if (!strcmp(argv[i], "-o") && i + 1 < argc) {
            o->out_path = argv[++i];
        } else if (!strcmp(argv[i], "--frames") && i + 1 < argc) {
            o->max_frames = atol(argv[++i]);
        } else if (!strcmp(argv[i], "--output-depth") && i + 1 < argc) {
            o->out_depth = atoi(argv[++i]);
        } else if (!strcmp(argv[i], "--input-res") && i + 1 < argc) {
            const char *v = argv[++i];
            if (sscanf(v, "%dx%d", &o->in_w, &o->in_h) != 2 || o->in_w <= 0 ||
                o->in_h <= 0) {
                fprintf(stderr, "y265: --input-res is WxH, e.g. 352x288\n");
                return 2;
            }
        } else if (!strcmp(argv[i], "--input-csp") && i + 1 < argc) {
            int mono = 0;
            o->in_csp = input_csp_parse(argv[++i], &mono);
            if (o->in_csp < 0) {
                fprintf(stderr, mono
                        ? "y265: --input-csp i400 (monochrome) is not a "
                          "format this encoder codes yet; it is refused "
                          "rather than read as 4:2:0\n"
                        : "y265: --input-csp is i420, i422 or i444 (or the "
                          "chroma_format_idc itself: 1, 2, 3)\n");
                return 2;
            }
        } else if (!strcmp(argv[i], "--input-depth") && i + 1 < argc) {
            o->in_depth = atoi(argv[++i]);
        } else if (!strcmp(argv[i], "--fps") && i + 1 < argc) {
            if (fps_parse(argv[++i], &o->in_fps_num, &o->in_fps_den) < 0) {
                fprintf(stderr, "y265: --fps is N, N/D or a decimal "
                        "(30, 30000/1001, 29.97)\n");
                return 2;
            }
        } else if (!strcmp(argv[i], "--csv") && i + 1 < argc) {
            o->csv_path = argv[++i];
        } else if (!strcmp(argv[i], "--frame-stats") && i + 1 < argc) {
            o->fstats_path = argv[++i];
        } else if (!strcmp(argv[i], "--control") && i + 1 < argc) {
            o->ctl_path = argv[++i];
        } else if (!strcmp(argv[i], "--control-out") && i + 1 < argc) {
            o->ctl_out_path = argv[++i];
        } else if (!strcmp(argv[i], "--control-fd") && i + 1 < argc) {
            char *end;
            long fd = strtol(argv[++i], &end, 10);
            if (*end || fd < 0 || fd > 1000000) {
                fprintf(stderr, "y265: --control-fd takes a descriptor "
                        "number\n");
                return 2;
            }
            o->ctl_fd = (int)fd;
        } else if (!strcmp(argv[i], "--csv-log-level") && i + 1 < argc) {
            o->csv_level = atoi(argv[++i]);
        } else if (!strcmp(argv[i], "--seek") && i + 1 < argc) {
            o->seek = atol(argv[++i]);
            if (o->seek < 0) {
                fprintf(stderr, "y265: --seek takes a frame count\n");
                return 2;
            }
        } else if (!strcmp(argv[i], "--dump-recon-decode") && i + 1 < argc) {
            o->dump_decode_path = argv[++i];
        } else if (!strcmp(argv[i], "--gop-parallel") && i + 1 < argc) {
            o->gop_par = atoi(argv[++i]);
        } else if (!strcmp(argv[i], "--verbose") || !strcmp(argv[i], "-v")) {
            o->verbose = 1;
        } else if (!strcmp(argv[i], "--show-config")) {
            o->show_config = 1;
        } else if (!strcmp(argv[i], "--dither")) {
            o->dither = 1;
        } else if (!strcmp(argv[i], "--no-dither")) {
            o->dither = 0;
        } else if (!strcmp(argv[i], "--dump-recon") && i + 1 < argc) {
            o->dump_path = argv[++i];
        } else if (!strcmp(argv[i], "--shot-stats") && i + 1 < argc) {
            o->shot_path = argv[++i];
        } else if (!strcmp(argv[i], "--shot-scan") && i + 1 < argc) {
            o->shot_scan = argv[++i];
        } else if (!strcmp(argv[i], "--shot-list") && i + 1 < argc) {
            o->shot_list = argv[++i];
        } else if (!strcmp(argv[i], "--version")) {
            printf("y265 %s\n", a->version_str());
            return 0;
        } else if (!strcmp(argv[i], "-h") || !strcmp(argv[i], "--help")) {
            usage(stdout);
            return 0;
        } else if (argv[i][0] != '-' || !strcmp(argv[i], "-")) {
            o->in_path = argv[i];
            continue;
        } else {
            mine = 0;
        }
        if (mine) {
            /* still named in the identification SEI, in order */
            a->config_note(cfg, argv[argi], i > argi ? argv[i] : NULL);
            continue;
        }
        /* a coding option, by its long name (-w and -t are the two short
         * spellings) */
        const char *name = !strncmp(argv[i], "--", 2) ? argv[i] + 2
                         : !strcmp(argv[i], "-w") || !strcmp(argv[i], "-t")
                           ? argv[i] + 1 : NULL;
        int tv = name ? n265_config_takes_value(name) : -1;
        if (tv < 0 || (tv && i + 1 >= argc)) {
            fprintf(stderr, "y265: unknown option %s\n", argv[i]);
            usage(stderr);
            return 2;
        }
        const char *val = tv ? argv[++i] : NULL;
        if (a->config_parse(cfg, name, val) != 0)
            return 2;
        if (!strcmp(name, "threads"))
            o->threads = atoi(val);
        else if (!strcmp(name, "psnr"))
            o->want_psnr = 1;
        else if (!strcmp(name, "ssim"))
            o->want_ssim = 1;
    }
    if (!o->in_path) {
        usage(stderr);
        return 2;
    }
    return -1;
}

/* ---- --shot-scan and --shot-list ---- */

/* The fingerprint of the frames this run would code: from where the
 * reader stands (past --seek), --frames of them at most. The three
 * sampled frames are read by seeking, so the input has to be a file of
 * fixed-size records; the reader is left where it stood. */
static int shot_fp_input(y4m_t *y, long max_frames, n265_shot_fp *fp)
{
    off_t base, rec;
    long total;
    if (!gp_seekable(y, &base, &rec, &total))
        return -1;
    long n = max_frames >= 0 && max_frames < total ? max_frames : total;
    size_t lb = (size_t)y->w * y->h * (y->depth > 8 ? 2 : 1);
    uint8_t *buf = malloc(lb);
    if (!buf || n < 1) {
        free(buf);
        return -1;
    }
    memset(fp, 0, sizeof *fp);
    fp->have = 1;
    fp->frames = n;
    fp->w = y->w;
    fp->h = y->h;
    fp->depth = y->depth;
    long pick[3] = { 0, n / 2, n - 1 };
    uint64_t h[3];
    int rc = 0;
    for (int k = 0; k < 3 && !rc; k++) {
        off_t at = base + (off_t)pick[k] * rec + (y->raw ? 0 : 6);
        if (fseeko(y->f, at, SEEK_SET) != 0 || fread(buf, 1, lb, y->f) != lb)
            rc = -1;
        else
            n265_shot_fp_frame(buf, y->w, y->h, y->depth, &h[k],
                               fp->thumb[k]);
    }
    free(buf);
    if (fseeko(y->f, base, SEEK_SET) != 0)
        rc = -1;
    fp->luma = rc ? 0 : n265_shot_fp_combine(h);
    return rc;
}

/* --shot-scan: every frame from where the reader stands through the scan,
 * and the list to `path`. The input is read once and in order, so a pipe
 * works; the fingerprint keeps one hash and one thumbnail per frame. */
static int shot_scan_run(y4m_t *y, long max_frames, const char *path)
{
    n265_param sp = { y->w, y->h, N265_CSP_I420, y->fps_num, y->fps_den };
    n265_shotscan *ss = n265_shotscan_open(&sp);
    if (!ss) {
        fprintf(stderr, "y265: --shot-scan: cannot start the scan\n");
        return 1;
    }
    uint64_t *hs = NULL;
    uint8_t (*th)[64] = NULL;
    long n = 0, cap = 0;
    int r = 0, rc = 0;
    while ((max_frames < 0 || n < max_frames) && (r = y4m_read(y)) > 0) {
        if (n == cap) {
            long nc = cap ? 2 * cap : 1024;
            uint64_t *nh = realloc(hs, (size_t)nc * sizeof *nh);
            if (nh)
                hs = nh;
            uint8_t (*nt)[64] = realloc(th, (size_t)nc * sizeof *nt);
            if (nt)
                th = nt;
            if (!nh || !nt) {
                rc = 1;
                break;
            }
            cap = nc;
        }
        n265_shot_fp_frame(y->frame, y->w, y->h, y->depth, &hs[n], th[n]);
        if (n265_shotscan_push(ss, y->frame, y->w, y->h, y->depth) != 0) {
            rc = 1;
            break;
        }
        n++;
    }
    if (r < 0) {
        fprintf(stderr, "y265: truncated input after frame %ld\n", n);
        rc = 1;
    }
    n265_shotlist l;
    memset(&l, 0, sizeof l);
    if (!rc && (n < 1 || n265_shotscan_finish(ss, &l) != 0)) {
        fprintf(stderr, "y265: --shot-scan: no frames to scan\n");
        rc = 1;
    }
    n265_shotscan_close(ss);
    if (!rc) {
        long pick[3] = { 0, n / 2, n - 1 };
        uint64_t h3[3];
        l.fp.have = 1;
        l.fp.w = y->w;
        l.fp.h = y->h;
        l.fp.depth = y->depth;
        for (int k = 0; k < 3; k++) {
            h3[k] = hs[pick[k]];
            memcpy(l.fp.thumb[k], th[pick[k]], 64);
        }
        l.fp.luma = n265_shot_fp_combine(h3);
        FILE *f = fopen(path, "w");
        if (!f || n265_shotlist_write(f, &l) != 0 || fclose(f) != 0) {
            fprintf(stderr, "y265: --shot-scan: cannot write %s\n", path);
            rc = 1;
        } else {
            fprintf(stderr, "y265: --shot-scan: %d shot%s in %ld frames "
                    "(scanned at %dx%d), written to %s\n", l.n,
                    l.n == 1 ? "" : "s", n, l.scan_w, l.scan_h, path);
        }
    }
    n265_shotlist_free(&l);
    free(hs);
    free(th);
    return rc;
}

/* --shot-list: read the list, refuse it on the wrong input, and hand back
 * the starts after frame 0 */
static int shot_list_load(y4m_t *y, long max_frames, const char *path,
                          int **starts, int *n)
{
    n265_shotlist l;
    char err[256];
    if (n265_shotlist_parse(&l, path, err, sizeof err) != 0) {
        fprintf(stderr, "y265: --shot-list: %s\n", err);
        return 2;
    }
    n265_shot_fp have;
    if (shot_fp_input(y, max_frames, &have) != 0) {
        fprintf(stderr, "y265: --shot-list needs an input file it can seek "
                "in (raw, or a y4m whose frame headers carry no "
                "parameters)\n");
        n265_shotlist_free(&l);
        return 2;
    }
    if (n265_shotlist_check(&l, &have, err, sizeof err) != 0) {
        fprintf(stderr, "y265: --shot-list: %s; refusing it\n", err);
        n265_shotlist_free(&l);
        return 2;
    }
    *n = l.n - 1;
    *starts = malloc((size_t)(l.n > 1 ? l.n - 1 : 1) * sizeof **starts);
    if (!*starts) {
        n265_shotlist_free(&l);
        return 1;
    }
    for (int k = 1; k < l.n; k++)
        (*starts)[k - 1] = l.s[k].first;
    n265_shotlist_free(&l);
    return 0;
}

/* main returns from many places while the encoder still reads the
 * config's strings, so the config is freed once, at exit */
static n265_config *exit_cfg;
static const n265_api *exit_api;

static void free_exit_cfg(void)
{
    if (exit_cfg)
        exit_api->config_free(exit_cfg);
}


/* ---- live control: --control, --control-fd, --control-out ---- */
typedef struct {
    const n265_api *a;
    n265_encoder *enc;
    int fd, eof;
    long line;
    char buf[8192];
    size_t len;
    FILE *out;
    /* applied events for --control-out, written sorted by named frame
     * and, within a frame, in the order handed in (`seq`) */
    struct ctl_rec { int64_t frame; long seq; char text[256]; } *rec;
    int nrec, crec;
    /* every accepted event, in hand-in order, to find an applied one's */
    n265_control_t *sub;
    char *used;
    int nsub, csub;
    /* the --control file, fed as frames go by from fnext on */
    const char *fpath;
    n265_control_t *fev;
    long *fline;
    int fn, fnext;
    /* frames named by the --control-fd events accepted, for the reserve */
    int64_t *fdf;
    int nfdf, cfdf;
} ctl_cli;

/* --control events wait in the CLI and go to the encoder as frames go by:
 * every event for the frame about to be handed in, then more ahead of it
 * while the encoder holds fewer than CTL_FEED_WINDOW. CTL_FD_RESERVE more
 * are kept for --control-fd lines. An event has landed once its frame is
 * CTL_LAG_SPAN behind the next one in (the lookahead, the shot window and
 * the frames in flight are far shorter), so the file events naming any
 * CTL_LAG_SPAN frames together may be at most what the encoder's bound of
 * 4096 pending leaves. */
#define CTL_FEED_WINDOW  1024
#define CTL_FD_RESERVE   1024
#define CTL_LAG_SPAN     1024
#define CTL_SPAN_MAX     (4096 - CTL_FEED_WINDOW - CTL_FD_RESERVE)

static int ctl_send(ctl_cli *c, n265_control_t *v)
{
    int r = c->a->encoder_control(c->enc, v);
    if (r < 0 || !c->out)
        return r;
    if (c->nsub == c->csub) {
        int nc = c->csub ? 2 * c->csub : 64;
        n265_control_t *s = realloc(c->sub, (size_t)nc * sizeof *s);
        char *u = s ? realloc(c->used, (size_t)nc) : NULL;
        if (s)
            c->sub = s;
        if (!u)
            return r;
        c->used = u;
        c->csub = nc;
    }
    c->sub[c->nsub] = *v;
    c->used[c->nsub++] = 0;
    return r;
}

static int ctl_same(const n265_control_t *a, const n265_control_t *b)
{
    return a->kind == b->kind && a->frame == b->frame &&
           a->bitrate_kbps == b->bitrate_kbps &&
           a->vbv_maxrate_kbps == b->vbv_maxrate_kbps &&
           a->vbv_bufsize_kbit == b->vbv_bufsize_kbit && a->crf == b->crf &&
           a->qp_min == b->qp_min && a->qp_max == b->qp_max &&
           a->last == b->last && a->qp_offset == b->qp_offset;
}

/* the hand-in position of an applied event; equal events are
 * interchangeable, so the first unclaimed match is the one */
static long ctl_seq(ctl_cli *c, const n265_control_t *v)
{
    for (int i = 0; i < c->nsub; i++)
        if (!c->used[i] && ctl_same(&c->sub[i], v)) {
            c->used[i] = 1;
            return i;
        }
    return c->nsub;
}

static void ctl_from_log(const n265_api *a, const struct ctl_log_event *l,
                         n265_control_t *v)
{
    a->control_init(v, l->kind);
    v->frame = l->frame;
    v->bitrate_kbps = l->bitrate_kbps;
    v->vbv_maxrate_kbps = l->vbv_maxrate_kbps;
    v->vbv_bufsize_kbit = l->vbv_bufsize_kbit;
    v->crf = l->crf;
    v->qp_min = l->qp_min;
    v->qp_max = l->qp_max;
    v->last = l->last;
    v->qp_offset = l->qp_offset;
    v->fps_num = l->fps_num;
    v->fps_den = l->fps_den;
    memcpy(v->preset, l->preset, sizeof v->preset);
    v->width = l->width;
    v->height = l->height;
}

static void ctl_to_log(const n265_control_t *v, struct ctl_log_event *l)
{
    memset(l, 0, sizeof *l);
    l->kind = v->kind;
    l->frame = v->frame;
    l->bitrate_kbps = v->bitrate_kbps;
    l->vbv_maxrate_kbps = v->vbv_maxrate_kbps;
    l->vbv_bufsize_kbit = v->vbv_bufsize_kbit;
    l->crf = v->crf;
    l->qp_min = v->qp_min;
    l->qp_max = v->qp_max;
    l->last = v->last;
    l->qp_offset = v->qp_offset;
    l->fps_num = v->fps_num;
    l->fps_den = v->fps_den;
    memcpy(l->preset, v->preset, sizeof l->preset);
    l->width = v->width;
    l->height = v->height;
}

typedef struct {
    struct ctl_log_event *ev;
    long *line;
    int n;
} ctl_file;

/* the whole --control file, parsed before the encoder opens */
static int ctl_load(const char *path, ctl_file *cf)
{
    FILE *f = fopen(path, "r");
    if (!f) {
        fprintf(stderr, "y265: cannot read --control %s: %s\n", path,
                strerror(errno));
        return -1;
    }
    char ln[1100], err[160];
    long no = 0;
    int cap = 0, rc = 0;
    memset(cf, 0, sizeof *cf);
    while (fgets(ln, sizeof ln, f)) {
        no++;
        if (no == 1) {
            int v = ctl_log_header(ln);
            if (v < 0 || v > CTL_LOG_VERSION) {
                fprintf(stderr, "y265: --control %s:1: a #livectl header "
                        "this reader doesn't know\n", path);
                rc = -1;
                break;
            }
        }
        struct ctl_log_event ev;
        int r = ctl_log_parse(ln, 0, &ev, err, sizeof err);
        if (r < 0) {
            fprintf(stderr, "y265: --control %s:%ld: %s\n", path, no, err);
            rc = -1;
            break;
        }
        if (r == 0)
            continue;
        if (cf->n && ev.frame < cf->ev[cf->n - 1].frame) {
            fprintf(stderr, "y265: --control %s:%ld: frame %lld comes "
                    "before the line above it\n", path, no,
                    (long long)ev.frame);
            rc = -1;
            break;
        }
        if (cf->n == cap) {
            cap = cap ? 2 * cap : 64;
            void *a = realloc(cf->ev, (size_t)cap * sizeof *cf->ev);
            void *b = a ? realloc(cf->line, (size_t)cap * sizeof *cf->line)
                        : NULL;
            if (a)
                cf->ev = a;
            if (!a || !b) {
                fprintf(stderr, "y265: --control: out of memory\n");
                rc = -1;
                break;
            }
            cf->line = b;
        }
        cf->ev[cf->n] = ev;
        cf->line[cf->n++] = no;
    }
    fclose(f);
    return rc;
}

/* the whole --control file against the open encoder, before any output:
 * every event as the encoder will see it, and the feed's room */
static int ctl_check_file(ctl_cli *c, const char *path, const ctl_file *cf)
{
    c->fpath = path;
    c->fev = malloc((size_t)(cf->n ? cf->n : 1) * sizeof *c->fev);
    c->fline = malloc((size_t)(cf->n ? cf->n : 1) * sizeof *c->fline);
    if (!c->fev || !c->fline) {
        fprintf(stderr, "y265: --control: out of memory\n");
        return -1;
    }
    for (int i = 0; i < cf->n; i++) {
        ctl_from_log(c->a, &cf->ev[i], &c->fev[i]);
        c->fline[i] = cf->line[i];
    }
    c->fn = cf->n;
    int bad = -1;
    int r = c->a->control_check(c->enc, c->fev, c->fn, &bad);
    if (r != 0) {
        if (bad >= 0)
            fprintf(stderr, "y265: --control %s:%ld: %s\n", path,
                    c->fline[bad], c->a->control_strerror(r));
        else
            fprintf(stderr, "y265: --control %s: %s\n", path,
                    c->a->control_strerror(r));
        return -1;
    }
    /* frames never decrease down the file: one pass */
    for (int i = 0, lo = 0; i < c->fn; i++) {
        while (c->fev[i].frame - c->fev[lo].frame >= CTL_LAG_SPAN)
            lo++;
        if (i - lo + 1 > CTL_SPAN_MAX) {
            fprintf(stderr, "y265: --control %s:%ld: more than %d events "
                    "within %d frames: the encoder's queue can't hold "
                    "them\n", path, c->fline[i], CTL_SPAN_MAX,
                    CTL_LAG_SPAN);
            return -1;
        }
    }
    return 0;
}

static int ctl_status(ctl_cli *c, int64_t *pending, int64_t *next)
{
    n265_status_t st;
    memset(&st, 0, sizeof st);
    st.size = (int)sizeof st;
    if (c->a->status(c->enc, &st) < 0)
        return -1;
    *pending = st.controls_pending;
    *next = st.next_frame;
    return 0;
}

/* before each hand-in. The file was checked whole, and every --control-fd
 * event against what is left of it, so only EFULL can come back, and only
 * ahead of the frame about to go in. */
static int ctl_feed_file(ctl_cli *c)
{
    int64_t q, next;
    if (c->fnext >= c->fn || ctl_status(c, &q, &next) < 0)
        return 0;
    while (c->fnext < c->fn) {
        int must = c->fev[c->fnext].frame <= next;
        if (!must && q >= CTL_FEED_WINDOW)
            break;
        n265_control_t v = c->fev[c->fnext];
        int r = ctl_send(c, &v);
        if (r == N265_CTL_EFULL && !must)
            break;
        if (r < 0) {
            fprintf(stderr, "y265: --control %s:%ld: %s\n", c->fpath,
                    c->fline[c->fnext], c->a->control_strerror(r));
            return -1;
        }
        c->fnext++;
        q++;
    }
    return 0;
}

/* an fd event, then the file events not fed yet that it could meet, under
 * the encoder's own check. *bad is the file event that would be refused,
 * or -1 when it is the fd event or nothing. */
static int ctl_fd_check(ctl_cli *c, const n265_control_t *ev, int *bad)
{
    int kind = ev->kind;
    int dep = kind == N265_CTL_RATE || kind == N265_CTL_QP_LIMITS ||
              kind == N265_CTL_ZONE;
    int n = 1;
    if (dep)
        for (int i = c->fnext; i < c->fn; i++)
            n += c->fev[i].kind == kind;
    n265_control_t *v = malloc((size_t)n * sizeof *v);
    int *at = malloc((size_t)n * sizeof *at);
    *bad = -1;
    if (!v || !at) {
        free(v);
        free(at);
        return N265_CTL_ENOMEM;
    }
    v[0] = *ev;
    at[0] = -1;
    for (int i = c->fnext, k = 1; dep && i < c->fn; i++)
        if (c->fev[i].kind == kind) {
            at[k] = i;
            v[k++] = c->fev[i];
        }
    int b = -1;
    int r = c->a->control_check(c->enc, v, n, &b);
    if (r != 0 && b > 0)
        *bad = at[b];
    free(v);
    free(at);
    return r;
}

/* the reserve: fd events whose frame may not have landed yet */
static int ctl_fd_room(ctl_cli *c, int64_t next)
{
    int w = 0;
    for (int i = 0; i < c->nfdf; i++)
        if (c->fdf[i] > next - CTL_LAG_SPAN)
            c->fdf[w++] = c->fdf[i];
    c->nfdf = w;
    return c->nfdf < CTL_FD_RESERVE;
}

static void ctl_fd_note(ctl_cli *c, int64_t frame)
{
    if (c->nfdf == c->cfdf) {
        int nc = c->cfdf ? 2 * c->cfdf : 64;
        int64_t *f = realloc(c->fdf, (size_t)nc * sizeof *f);
        if (!f)
            return;
        c->fdf = f;
        c->cfdf = nc;
    }
    c->fdf[c->nfdf++] = frame;
}

/* one --control-fd line: -1 stops the encode */
static int ctl_fd_line(ctl_cli *c, const char *ln)
{
    char err[160];
    c->line++;
    if (c->line == 1) {
        int v = ctl_log_header(ln);
        if (v < 0 || v > CTL_LOG_VERSION) {
            fprintf(stderr, "y265: --control-fd line 1: a #livectl header "
                    "this reader doesn't know\n");
            return -1;
        }
    }
    struct ctl_log_event ev;
    int r = ctl_log_parse(ln, 1, &ev, err, sizeof err);
    if (r < 0) {
        fprintf(stderr, "y265: --control-fd line %ld: %s\n", c->line, err);
        return -1;
    }
    if (r == 0)
        return 0;
    n265_control_t v;
    ctl_from_log(c->a, &ev, &v);
    int64_t q, next;
    int bad = -1;
    if (ctl_status(c, &q, &next) < 0 || !ctl_fd_room(c, next))
        r = N265_CTL_EFULL;
    else
        r = ctl_fd_check(c, &v, &bad);
    if (r == 0) {
        r = ctl_send(c, &v);
        if (r == 0)
            ctl_fd_note(c, v.frame);
    }
    if (bad >= 0)
        fprintf(stderr, "y265: --control-fd line %ld skipped: %s line %ld "
                "would then be refused: %s\n", c->line, c->fpath,
                c->fline[bad], c->a->control_strerror(r));
    else if (r == N265_CTL_ELATE)
        fprintf(stderr, "y265: --control-fd line %ld skipped: frame %lld "
                "has already been handed in\n", c->line,
                (long long)ev.frame);
    else if (r < 0)
        fprintf(stderr, "y265: --control-fd line %ld skipped: %s\n",
                c->line, c->a->control_strerror(r));
    return 0;
}

/* the complete lines the descriptor has now, at most 64 a frame */
static int ctl_fd_poll(ctl_cli *c)
{
    int n = 0;
    while (c->fd >= 0) {
        char *nl;
        while (n < 64 && (nl = memchr(c->buf, '\n', c->len))) {
            *nl = 0;
            int r = ctl_fd_line(c, c->buf);
            c->len -= (size_t)(nl + 1 - c->buf);
            memmove(c->buf, nl + 1, c->len);
            n++;
            if (r < 0)
                return -1;
        }
        if (n >= 64)
            break;
        if (c->eof) {
            if (c->len) {
                c->buf[c->len] = 0;
                c->len = 0;
                if (ctl_fd_line(c, c->buf) < 0)
                    return -1;
            }
            c->fd = -1;
            break;
        }
        if (c->len == sizeof c->buf - 1) {
            fprintf(stderr, "y265: --control-fd line %ld: line too long\n",
                    c->line + 1);
            return -1;
        }
        ssize_t k = read(c->fd, c->buf + c->len, sizeof c->buf - 1 - c->len);
        if (k < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
            break;
        if (k < 0 && errno == EINTR)
            continue;
        if (k <= 0)
            c->eof = 1;
        else
            c->len += (size_t)k;
    }
    return 0;
}

static void ctl_drain(ctl_cli *c)
{
    n265_control_t ev[16];
    int n;
    do {
        for (int i = 0; i < 16; i++)
            c->a->control_init(&ev[i], 0);
        n = c->a->controls_applied(c->enc, ev, 16);
        if (n < 0) {
            fprintf(stderr, "y265: the encoder dropped applied control "
                    "records: --control-out is incomplete\n");
            n = 16;
            continue;
        }
        for (int i = 0; i < n; i++) {
            struct ctl_log_event l;
            char line[256];
            ctl_to_log(&ev[i], &l);
            ctl_log_format(&l, line, sizeof line);
            if (ev[i].applied_frame < 0) {
                fprintf(stderr, "y265: control \"%s\" never landed: the "
                        "stream ended first\n", line);
                continue;
            }
            if (!c->out)
                continue;
            if (c->nrec == c->crec) {
                int nc = c->crec ? 2 * c->crec : 64;
                struct ctl_rec *r = realloc(c->rec, (size_t)nc * sizeof *r);
                if (!r)
                    continue;
                c->rec = r;
                c->crec = nc;
            }
            c->rec[c->nrec].frame = ev[i].frame;
            c->rec[c->nrec].seq = ctl_seq(c, &ev[i]);
            memcpy(c->rec[c->nrec++].text, line, sizeof line);
        }
    } while (n == 16);
}

/* Application order can run backwards in frame (a rate event lands at a
 * later anchor than a key named after it), and --control wants frames
 * that never decrease. */
static int ctl_write_out(ctl_cli *c)
{
    for (int i = 1; i < c->nrec; i++)
        for (int k = i; k > 0 && (c->rec[k].frame < c->rec[k - 1].frame ||
                                  (c->rec[k].frame == c->rec[k - 1].frame &&
                                   c->rec[k].seq < c->rec[k - 1].seq)); k--) {
            struct ctl_rec t = c->rec[k];
            c->rec[k] = c->rec[k - 1];
            c->rec[k - 1] = t;
        }
    for (int i = 0; i < c->nrec; i++)
        fprintf(c->out, "%s\n", c->rec[i].text);
    free(c->rec);
    free(c->sub);
    free(c->used);
    return fclose(c->out);
}

/* `y265 bench ...` runs the y265-bench helper (scripts/y265_bench.py),
 * telling it which binary to time */
static int self_path(const char *argv0, char *out)
{
    char raw[PATH_MAX];
#ifdef __APPLE__
    uint32_t n = sizeof raw;
    if (_NSGetExecutablePath(raw, &n) == 0 && realpath(raw, out))
        return 0;
#else
    ssize_t n = readlink("/proc/self/exe", raw, sizeof raw - 1);
    if (n > 0) {
        raw[n] = 0;
        if (realpath(raw, out))
            return 0;
    }
#endif
    return realpath(argv0, out) ? 0 : -1;
}

static int run_bench(int argc, char **argv)
{
    char self[PATH_MAX], helper[PATH_MAX + 32];
    const char *env = getenv("Y265_BENCH");
    int found = 0;
    if (env && *env) {
        snprintf(helper, sizeof helper, "%s", env);
        found = 1;
    } else if (self_path(argv[0], self) == 0) {
        char *slash = strrchr(self, '/');
        static const char *const rel[] = { "y265-bench",
                                           "../scripts/y265_bench.py" };
        for (int k = 0; k < 2 && !found && slash; k++) {
            snprintf(helper, sizeof helper, "%.*s/%s",
                     (int)(slash - self), self, rel[k]);
            found = access(helper, R_OK) == 0;
        }
        setenv("Y265_BIN", self, 1);
    }
    if (!found) {
        fprintf(stderr, "y265: bench needs the y265-bench helper beside the "
                "y265 binary (or Y265_BENCH set to its path)\n");
        return 2;
    }
    char **av = calloc((size_t)argc + 2, sizeof *av);
    if (!av)
        return 1;
    av[0] = "python3";
    av[1] = helper;
    for (int i = 2; i < argc; i++)
        av[i] = argv[i];
    execv(helper, av + 1);
    execvp("python3", av);
    fprintf(stderr, "y265: bench needs Python 3 on the PATH (%s)\n",
            strerror(errno));
    free(av);
    return 2;
}

int main(int argc, char **argv)
{
    if (argc > 1 && !strcmp(argv[1], "bench"))
        return run_bench(argc, argv);
    cli_opts o;
    n265_config *cfg = api8.config_new();
    if (!cfg)
        return 1;
    exit_cfg = cfg;
    exit_api = &api8;
    atexit(free_exit_cfg);
    {
        int rc = cli_parse(&api8, cfg, argc, argv, &o);
        if (rc >= 0)
            return rc;
    }
    const char *in_path = o.in_path, *out_path = o.out_path;
    const char *dump_path = o.dump_path, *shot_path = o.shot_path;
    const char *dump_decode_path = o.dump_decode_path;
    const char *csv_path = o.csv_path;
    long max_frames = o.max_frames, seek = o.seek;
    int out_depth = o.out_depth, dither = o.dither;
    int in_w = o.in_w, in_h = o.in_h, in_csp = o.in_csp, in_depth = o.in_depth;
    int in_fps_num = o.in_fps_num, in_fps_den = o.in_fps_den;
    int csv_level = o.csv_level, gop_par = o.gop_par;
    int verbose = o.verbose, show_config = o.show_config;
    int threads = o.threads, want_psnr = o.want_psnr, want_ssim = o.want_ssim;
    if (out_depth != 0 && out_depth != 8 && out_depth != 10) {
        fprintf(stderr, "y265: --output-depth must be 8 or 10\n");
        return 2;
    }

    FILE *fin = strcmp(in_path, "-") ? fopen(in_path, "rb") : stdin;
    if (!fin) {
        fprintf(stderr, "y265: cannot open %s: %s\n", in_path, strerror(errno));
        return 1;
    }
    /* RAW or y4m, and the decision is made from the FLAGS and the name,
     * never by sniffing: a raw file has no magic to sniff for, and a
     * reader that guessed would code garbage silently on the one input
     * that cannot say what it is. `--input-res` is the switch - x265
     * takes the same decision off the `.yuv` extension, and that is
     * honored too so a plain `y265 clip.yuv --input-res ...` and the
     * bare extension agree. */
    if (is_y4m_name(in_path) &&
        (in_w > 0 || in_csp >= 0 || in_depth || in_fps_num > 0)) {
        fprintf(stderr, "y265: --input-res / --input-csp / --input-depth / "
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
            fprintf(stderr, "y265: raw input carries no header, so its "
                    "geometry has to be given:");
            for (int k = 0; k < nmiss; k++)
                fprintf(stderr, " %s%s", miss[k], k + 1 < nmiss ? "," : "");
            fprintf(stderr, "\n");
            return 2;
        }
        if (in_depth == 0)
            in_depth = 8;
        if (in_depth != 8 && in_depth != 10) {
            fprintf(stderr, "y265: --input-depth must be 8 or 10\n");
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
            fprintf(stderr, "y265: --input-res / --input-csp / "
                    "--input-depth / --fps describe RAW input; a y4m "
                    "carries its own geometry\n");
            return 2;
        }
        yr = y4m_open(&y, fin);
    }
    if (yr < 0) {
        if (yr == -1)
            fprintf(stderr, want_raw ? "y265: cannot size the raw input\n"
                                     : "y265: bad y4m input\n");
        return 1;
    }
    /* --dither, once the source's own depth is known. It is set AFTER
     * the open because both openers memset the struct, and it is refused
     * rather than ignored anywhere it would do nothing: a flag that is
     * silently inert is the one thing about a command line that cannot
     * be debugged from the output. */
    if (dither && !(y.depth == 10 && y.out_depth == 8)) {
        fprintf(stderr, "y265: --dither is the 10-bit to 8-bit reduction's "
                "own choice; it needs --output-depth 8 on a 10-bit source "
                "(this one is %d-bit in, %d-bit out)\n", y.depth,
                y.out_depth);
        return 2;
    }
    y.dither = dither;
    /* --seek, after the open (the y4m header has to be read either way)
     * and before anything is encoded. */
    if (seek > 0 && src_skip(&y, seek) < 0) {
        fprintf(stderr, "y265: --seek %ld runs past the end of %s\n",
                seek, in_path);
        return 1;
    }

    /* --shot-scan: the 8-bit library at the analysis size, whatever the
     * input's depth, so every rung's options resolve the same way; the
     * scan writes its list and the run ends */
    if (o.shot_scan) {
        n265_param sp;
        api8.param_default(&sp);
        sp.width = N265_SHOTSCAN_W;
        sp.height = n265_shotscan_h(y.w, y.h);
        sp.csp = N265_CSP_I420;
        sp.fps_num = y.fps_num;
        sp.fps_den = y.fps_den;
        if (api8.config_apply(cfg, &sp) != 0)
            return 2;
        int src = shot_scan_run(&y, max_frames, o.shot_scan);
        free(y.pix16);
        free(y.pix8);
        free(y.frame);
        if (fin != stdin)
            fclose(fin);
        return src;
    }
    int *shot_starts = NULL, shot_n = 0;
    if (o.shot_list) {
        int lrc = shot_list_load(&y, max_frames, o.shot_list, &shot_starts,
                                 &shot_n);
        if (lrc)
            return lrc;
    }

    /* The dispatch, and it is the whole of it: one table per library,
     * selected by the width the pictures will carry. Everything after
     * this point calls through `a` and does not know which encoder it
     * is configuring. */
    const n265_api *a = y.out_depth == 10 ? &api10 : &api8;
    if (a != &api8) {
        /* the same options into the Main10 library's parser; the first
         * pass already refused anything it would */
        cli_opts again;
        api8.config_free(cfg);
        cfg = a->config_new();
        exit_cfg = cfg;
        exit_api = a;
        if (!cfg || cli_parse(a, cfg, argc, argv, &again) >= 0)
            return 2;
    }
    n265_param p;
    a->param_default(&p);
    p.width = y.w;
    p.height = y.h;
    p.csp = y.csp;
    p.fps_num = y.fps_num;
    p.fps_den = y.fps_den;
    if (a->config_apply(cfg, &p) != 0)
        return 2;
    double cv;
#define CFG(k) (a->config_get(cfg, (k), &cv) == 0 ? cv : 0.0)
    const int keyint = (int)CFG("keyint"), pass = (int)CFG("pass");
    const int hrd_concat = (int)CFG("hrd-concat");
    const int repeat_hdr = (int)CFG("repeat-headers");
    const double bitrate = CFG("bitrate");
    const double vbv_maxrate = CFG("vbv-maxrate");
    const double vbv_bufsize = CFG("vbv-bufsize");
    const int hw = (int)CFG("hw");
    const int intra_refresh = (int)CFG("intra-refresh");
#undef CFG
    const char *qpfile_path = a->config_get_str(cfg, "qpfile");
    const char *zones_spec = a->config_get_str(cfg, "zones");
    const char *h10p_path = a->config_get_str(cfg, "dhdr10-info");
    const char *rpu_path = a->config_get_str(cfg, "dolby-vision-rpu");
    /* --csv-log-level without --csv does nothing, so it is refused */
    if (csv_level >= 0 && !csv_path) {
        fprintf(stderr, "y265: --csv-log-level needs --csv <file>\n");
        return 2;
    }
    if (csv_level < 0)
        csv_level = 0;
    if (csv_level > 2) {
        fprintf(stderr, "y265: --csv-log-level is 0 (one summary row for "
                "the run), 1 (a row per frame) or 2 (that, plus the rate "
                "control's columns)\n");
        return 2;
    }
    /* with the video on stdin, a metadata file there would read the Y4M
     * as metadata */
    if (in_path && !strcmp(in_path, "-")) {
        const char *f[2] = { h10p_path, rpu_path };
        for (int k = 0; k < 2; k++)
            if (f[k] && (!strcmp(f[k], "/dev/stdin") ||
                         !strcmp(f[k], "/dev/fd/0") || !strcmp(f[k], "-"))) {
                fprintf(stderr, "y265: the input is on stdin, so %s can't "
                        "be read from there too\n",
                        k ? "--dolby-vision-rpu" : "--dhdr10-info");
                return 2;
            }
    }
    char dyn_err[256];
    if (h10p_path &&
        n265_h10p_load(h10p_path, &g_h10p_tab, dyn_err, sizeof dyn_err) < 0) {
        fprintf(stderr, "y265: --dhdr10-info: %s\n", dyn_err);
        return 2;
    }
    if (rpu_path &&
        n265_rpu_load(rpu_path, &g_rpu_tab, dyn_err, sizeof dyn_err) < 0) {
        fprintf(stderr, "y265: --dolby-vision-rpu: %s\n", dyn_err);
        return 2;
    }
    g_dyn_seek = seek;
    dyn_apply(a, 0);
    /* --verbose is N265_LEVEL_TRACE by another name */
    if (verbose)
        setenv("N265_LEVEL_TRACE", "1", 0);
    if (hw) {
        const char *why = dump_path ? "--dump-recon"
                        : dump_decode_path ? "--dump-recon-decode"
                        : csv_path ? "--csv"
                        : o.fstats_path ? "--frame-stats"
                        : shot_path ? "--shot-stats"
                        : o.shot_list ? "--shot-list" : NULL;
        if (why) {
            fprintf(stderr, "y265: --hw: %s has no hardware path (the "
                    "hardware keeps its reconstruction and its decisions)\n",
                    why);
            return 2;
        }
        if (gop_par > 1)
            fprintf(stderr, "y265: --gop-parallel %d: coding serially, "
                    "since --hw has its own parallelism\n", gop_par);
        gop_par = 0;
    }
    /* --gop-parallel (phase 14 item 42): above one thread only, and not
     * with the options that need one encoder to see the whole input */
    if (gop_par > 1 && intra_refresh) {
        fprintf(stderr, "y265: --gop-parallel cannot be combined with "
                "--intra-refresh, which has no key frames to cut at\n");
        return 2;
    }
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
            : o.fstats_path ? "--frame-stats is one encoder's log"
            : (o.ctl_path || o.ctl_fd >= 0)
              ? "live control numbers frames from the stream's start"
            : shot_path ? "--shot-stats is one encoder's log"
            : dump_decode_path ? "--dump-recon-decode is one encoder's log"
            : hrd_concat ? "--hrd-concat splices one encoder's stream"
            : vbv_on && hrd && atoi(hrd) ? "N265_VBV_HRD asks for the HRD "
              "timing SEI, which a joined stream does not carry"
            : NULL;
        if (why) {
            fprintf(stderr, "y265: --gop-parallel %d: coding serially, "
                    "since %s\n", gop_par, why);
        } else {
            FILE *gout = fopen(out_path, "wb");
            if (!gout) {
                fprintf(stderr, "y265: cannot open %s: %s\n", out_path,
                        strerror(errno));
                return 1;
            }
            FILE *gdump = NULL;
            if (dump_path) {
                gdump = fopen(dump_path, "wb");
                if (!gdump) {
                    fprintf(stderr, "y265: cannot open %s: %s\n", dump_path,
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
                             &gn, gmet, &gnm, &gseg, &gk,
                             shot_starts, o.shot_list ? shot_n : 0);
            if (gdump)
                fclose(gdump);
            if (fclose(gout) != 0 && !grc) {
                fprintf(stderr, "y265: write failed: %s\n", strerror(errno));
                grc = 1;
            }
            if (grc)
                return 1;
            if (gnm > 0 && want_psnr)
                fprintf(stderr, "y265: PSNR over %ld frames: y %.4f u %.4f "
                        "v %.4f avg %.4f dB\n", gnm, gmet[0], gmet[1],
                        gmet[2], gmet[3]);
            if (gnm > 0 && want_ssim)
                fprintf(stderr, "y265: SSIM-Y over %ld frames: %.6f (%.4f "
                        "dB)\n", gnm, gmet[4], gmet[5]);
            fprintf(stderr, "y265: gop-parallel: %d segment%s, %d instance%s "
                    "x %d thread%s\n", gseg, gseg == 1 ? "" : "s",
                    gseg > 1 ? (gop_par < nt ? gop_par : (int)nt) : 1,
                    gseg > 1 ? "s" : "", gk, gk == 1 ? "" : "s");
            free(y.pix16);
            free(y.pix8);
            free(y.frame);
            if (fin != stdin)
                fclose(fin);
            if (y.clipped)
                fprintf(stderr, "y265: WARNING %ld input samples were above "
                        "the declared %d-bit range and were clamped\n",
                        y.clipped, y.depth);
            const char *gfmt = y.csp == N265_CSP_I444 ? "4:4:4"
                             : y.csp == N265_CSP_I422 ? "4:2:2" : "4:2:0";
            fprintf(stderr, "y265: %ld frames, %dx%d %s %d-bit\n", gn, y.w,
                    y.h, gfmt, y.depth);
            return dyn_check(gn);
        }
    }
    ctl_file cfile = { 0 };
    if (o.ctl_path && ctl_load(o.ctl_path, &cfile) < 0)
        return 2;
    n265_encoder *enc = a->encoder_open(&p);
    if (!enc) {
        fprintf(stderr, "y265: encoder_open failed (dims %dx%d)\n", y.w, y.h);
        return 1;
    }
    if (hw && strcmp(a->backend(enc), "yah265"))
        fprintf(stderr, "y265: encoder: %s\n", a->backend(enc));
    ctl_cli cc = { 0 };
    cc.a = a;
    cc.enc = enc;
    cc.fd = o.ctl_fd;
    if (o.ctl_path) {
        int r = ctl_check_file(&cc, o.ctl_path, &cfile);
        free(cfile.ev);
        free(cfile.line);
        if (r < 0) {
            a->encoder_close(enc);
            return 2;
        }
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
    if (o.shot_list && a->set_shot_list(enc, shot_starts, shot_n) != 0) {
        fprintf(stderr, "y265: --shot-list: the encoder refused the list\n");
        return 1;
    }
    free(shot_starts);
    if (csv_path)
        a->set_csv(enc, csv_path, csv_level);
    if (o.fstats_path)
        a->set_frame_stats(enc, o.fstats_path);
    if (o.ctl_out_path) {
        cc.out = fopen(o.ctl_out_path, "w");
        if (!cc.out) {
            fprintf(stderr, "y265: cannot open %s: %s\n", o.ctl_out_path,
                    strerror(errno));
            return 1;
        }
        fprintf(cc.out, "#livectl %d\n", CTL_LOG_VERSION);
    }
    if (cc.fd >= 0) {
        int fl = fcntl(cc.fd, F_GETFL);
        if (fl < 0 || fcntl(cc.fd, F_SETFL, fl | O_NONBLOCK) < 0) {
            fprintf(stderr, "y265: --control-fd %d: %s\n", cc.fd,
                    strerror(errno));
            return 2;
        }
    }
    FILE *fdump = NULL;
    if (dump_path) {
        fdump = fopen(dump_path, "wb");
        if (!fdump) {
            fprintf(stderr, "y265: cannot open %s: %s\n", dump_path,
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
        fprintf(stderr, "y265: cannot open %s: %s\n", out_path, strerror(errno));
        return 1;
    }

    long n = 0;
    int r = 0;
    rd_ring rq;
    int nslot = rd_slots(&y, threads);
    if (nslot && rd_open(&rq, &y, nslot, max_frames) != 0) {
        fprintf(stderr, "y265: cannot start the input reader\n");
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
        if (ctl_feed_file(&cc) < 0)
            return 2;
        if (ctl_fd_poll(&cc) < 0)
            return 1;
        int bytes = a->encoder_encode(enc, &nal, &nnal, &pic);
        if (bytes < 0) {
            fprintf(stderr, "y265: encode failed on frame %ld\n", n);
            return 1;
        }
        ctl_drain(&cc);
        if (nslot)
            rd_release(&rq);
        for (int i = 0; i < nnal; i++)
            fwrite(nal[i].payload, 1, (size_t)nal[i].size, fout);
        n++;
    }
    if (nslot)
        rd_close(&rq, &y);
    if (r < 0) {
        fprintf(stderr, "y265: truncated input after frame %ld\n", n);
        return 1;
    }

    /* flush the lookahead queue */
    for (;;) {
        n265_nal *nal;
        int nnal;
        int r2 = a->encoder_encode(enc, &nal, &nnal, NULL);
        ctl_drain(&cc);
        if (r2 <= 0 && nnal == 0)
            break;
        for (int i = 0; i < nnal; i++)
            fwrite(nal[i].payload, 1, (size_t)nal[i].size, fout);
    }
    for (; cc.fnext < cc.fn; cc.fnext++) {
        struct ctl_log_event l;
        char line[256];
        ctl_to_log(&cc.fev[cc.fnext], &l);
        ctl_log_format(&l, line, sizeof line);
        fprintf(stderr, "y265: control \"%s\" never landed: the stream "
                "ended first\n", line);
    }
    if (cc.out && ctl_write_out(&cc) != 0) {
        fprintf(stderr, "y265: write failed: %s\n", o.ctl_out_path);
        return 1;
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
            fprintf(stderr, "y265: PSNR over %ld frames: y %.4f u %.4f "
                    "v %.4f avg %.4f dB\n", nmet, met[0], met[1], met[2],
                    met[3]);
        if (nmet > 0 && want_ssim)
            fprintf(stderr, "y265: SSIM-Y over %ld frames: %.6f (%.4f dB)\n",
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
        fprintf(stderr, "y265: write failed: %s\n", strerror(errno));
        return 1;
    }
    if (y.clipped)
        fprintf(stderr, "y265: WARNING %ld input samples were above the "
                "declared %d-bit range and were clamped\n", y.clipped,
                y.depth);
    const char *fmt = y.csp == N265_CSP_I444 ? "4:4:4"
                    : y.csp == N265_CSP_I422 ? "4:2:2" : "4:2:0";
    if (y.out_depth != y.depth)
        fprintf(stderr, "y265: %ld frames, %dx%d %s %d-bit in, coded %d-bit "
                "(%s)\n", n, y.w, y.h, fmt, y.depth, y.out_depth,
                y.out_depth > y.depth ? "upshifted by 2"
                : y.dither ? "ordered dither" : "truncated by 2");
    else
        fprintf(stderr, "y265: %ld frames, %dx%d %s %d-bit\n", n, y.w, y.h,
                fmt, y.depth);
    return dyn_check(n);
}
