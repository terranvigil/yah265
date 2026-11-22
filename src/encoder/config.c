/*
 * config.c - the command line's option set, as a library call.
 * Copyright (c) 2026, the yah265 authors. GPL-2.0-or-later; see LICENSE.
 *
 * Every coding option n265 takes is parsed and resolved here, so the CLI
 * and any other front end (the ffmpeg wrapper) configure the encoder
 * through one code path and write the same stream for the same options.
 * n265_config_parse takes one option at a time, by its CLI name without
 * the dashes; n265_config_apply resolves them against the picture (the
 * preset's resolution rules, the tune, the range checks) and calls the
 * process-global setters in the order the CLI always called them.
 *
 * Messages go to stderr with the "y265: " prefix the CLI printed.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "yah265.h"
#include "encoder/level.h"
#include "encoder/preset.h"
#include "encoder/signal.h"
#include "hw/vt_map.h"

void n265_set_shot_window(int n);
void n265_set_shot_keys(int on);
void n265_set_metrics(int psnr, int ssim);
void n265_set_nal_opts(int repeat_headers, int aud, int eos, int eob,
                       int info);
void n265_set_info_opts(const char *opts);
void n265_set_fingerprint(int on);
void n265_set_hrd(int on);
void n265_set_rc_bounds(double crf_min, double crf_max, int qpmin, int qpmax,
                        int strict_cbr, double ausize_factor);
void n265_set_sei_opts(int recovery_sei, int pic_struct, int hrd_concat,
                       int temporal_layers);
void n265_set_qpfile(const char *path);
void n265_set_zones(const char *spec);
void n265_set_pass(int pass, const char *stats);
void n265_set_lossless(int on);
void n265_set_rdoq_level(int lvl);
void n265_set_tu_depth(int inter, int intra);
void n265_set_chroma_qpoffs(int cb, int cr);
void n265_set_level(int idc, int high_tier);
void n265_set_vui_timing(int on);
void n265_set_film_grain(double strength, int denoise);
void n265_set_nr(int intra, int inter);
void n265_set_mcstf(int on, int range, int selective);
void n265_set_vbv(double maxrate, double bufsize, double init);
int n265_hw_mode(void);
void n265_set_hw_opts(int veto, const char *warn);
void n265_set_intra_refresh(int on);
void n265_set_scenecut(int on);

#define QP_BD_OFF (6 * (N265_BIT_DEPTH - 8))

/* the identification SEI's options clause */
typedef struct {
    char s[640];
    size_t n;
} optstr;

/* name, and whether it takes a value. The parse below handles exactly
 * these; a front end asks n265_config_takes_value before it reads a
 * value off its own argument list. */
static const struct { const char *name; int val; } opt_tab[] = {
    { "qp", 1 }, { "crf", 1 }, { "bitrate", 1 }, { "preset", 1 },
    { "idr-recovery-sei", 0 }, { "no-idr-recovery-sei", 0 },
    { "pic-struct", 1 }, { "hrd-concat", 0 },
    { "temporal-layers", 0 }, { "no-temporal-layers", 0 },
    { "scaling-list", 1 }, { "crf-min", 1 }, { "crf-max", 1 },
    { "qpmin", 1 }, { "qpmax", 1 }, { "strict-cbr", 0 },
    { "max-ausize-factor", 1 }, { "qpfile", 1 }, { "zones", 1 },
    { "repeat-headers", 0 }, { "no-repeat-headers", 0 },
    { "aud", 0 }, { "no-aud", 0 }, { "eos", 0 }, { "no-eos", 0 },
    { "eob", 0 }, { "no-eob", 0 }, { "info", 0 }, { "no-info", 0 },
    { "fingerprint", 0 }, { "no-fingerprint", 0 }, { "no-hrd", 0 },
    { "psnr", 0 }, { "ssim", 0 }, { "threads", 1 },
    { "ref", 1 }, { "b-adapt", 1 }, { "keyint", 1 },
    { "open-gop", 0 }, { "no-open-gop", 0 }, { "radl", 1 },
    { "scenecut", 0 }, { "no-scenecut", 0 },
    { "bframes", 1 }, { "aq", 1 }, { "rc-lookahead", 1 }, { "cutree", 1 },
    { "ctu", 1 }, { "rdoq-level", 1 }, { "tu-inter-depth", 1 },
    { "tu-intra-depth", 1 }, { "cbqpoffs", 1 }, { "crqpoffs", 1 },
    { "pcm", 0 }, { "lossless", 0 },
    { "weightp", 0 }, { "w", 0 }, { "no-weightp", 0 },
    { "weightb", 0 }, { "no-weightb", 0 },
    { "level", 1 }, { "high-tier", 0 },
    { "vui-timing", 0 }, { "no-vui-timing", 0 },
    { "film-grain", 1 }, { "film-grain-denoise", 1 },
    { "nr-intra", 1 }, { "nr-inter", 1 },
    { "mcstf", 0 }, { "no-mcstf", 0 }, { "mcstf-ref-range", 1 },
    { "selective-mcstf", 0 }, { "no-selective-mcstf", 0 },
    { "vbv-maxrate", 1 }, { "vbv-bufsize", 1 }, { "vbv-init", 1 },
    { "hash", 1 }, { "colorprim", 1 }, { "transfer", 1 },
    { "colormatrix", 1 }, { "videoformat", 1 }, { "range", 1 },
    { "overscan", 1 }, { "video-signal-type-preset", 1 },
    { "tune", 1 }, { "t", 1 }, { "intra-refresh", 0 },
    { "dup-threshold", 1 }, { "chromaloc", 1 }, { "sar", 1 },
    { "display-window", 1 }, { "master-display", 1 }, { "max-cll", 1 },
    { "hdr-opt", 0 }, { "hdr10-opt", 0 },
    { "no-hdr-opt", 0 }, { "no-hdr10-opt", 0 },
    { "hdr10", 0 }, { "no-hdr10", 0 }, { "cll", 0 }, { "no-cll", 0 },
    { "atc-sei", 1 }, { "dhdr10-info", 1 }, { "dolby-vision-rpu", 1 },
    { "dolby-vision-profile", 1 }, { "shot-window", 1 }, { "shot-keys", 0 },
    { "pass", 1 }, { "stats", 1 }, { "hw", 1 }, { "hw-strict", 0 },
};

#define N_OPTS (sizeof opt_tab / sizeof opt_tab[0])

struct n265_config {
    int qp, pcm, ctu, keyint, bframes, aq;
    int lossless;
    char *level_str;
    int high_tier, vui_timing;
    double film_grain;
    int fgc_denoise;
    int nr_intra, nr_inter, had_nr_intra, had_nr_inter;
    int mcstf, mcstf_range, mcstf_selective;
    double crf, bitrate;
    double vbv_maxrate, vbv_bufsize, vbv_init;
    int hash;
    int weightp, weightb;
    int open_gop, radl, intra_refresh, scenecut;
    int sig_videoformat, sig_range, sig_overscan;
    int sig_colorprim, sig_transfer, sig_colormatrix, sig_chromaloc;
    int res_transfer;           /* after the signal-type preset */
    int vsp[N265_VSP_NFIELD], vsp_md_on, had_vsp;
    uint32_t vsp_md[10];
    int tune[N265_TUNE_NFIELD];
    int had_tune, la_from_tune;
    double dup_thr;
    int sar_idc, sar_w, sar_h;
    int win_on, win_l, win_t, win_r, win_b;
    uint32_t md[10];
    int have_md, maxcll, maxfall;
    int hdr10, cll_on, atc, hdr_opt;
    char *h10p_path, *rpu_path;
    int dv_profile;
    int had_crf;
    int repeat_hdr, aud, eos, eob, info, fingerprint, no_hrd;
    int recovery_sei, pic_struct, hrd_concat, tlayers;
    double crf_lo, crf_hi;
    int qpmin, qpmax, strict_cbr, had_qpmin, had_qpmax;
    double ausize_factor;
    char *qpfile_path, *zones_spec;
    int want_psnr, want_ssim;
    int nref, badapt, lookahead, cutree;
    int threads, shot_window, shot_keys;
    int pass;
    char *stats_path;
    int rdoq_level, had_rdoq;
    int tu_inter, tu_intra, had_tui, had_tua;
    int cbqpoffs, crqpoffs, had_cbq, had_crq;
    char *preset;
    int had_ref, had_badapt, had_bframes, had_la, had_ctu, had_cutree;
    int applied;
    int hw, hw_strict, hw_on;
    unsigned char seen[N_OPTS];
    optstr info_opts;
    char info_all[768];
};

static const char *strip_dashes(const char *k)
{
    if (k[0] == '-')
        k++;
    if (k[0] == '-')
        k++;
    return k;
}

int n265_config_takes_value(const char *name)
{
    if (!name)
        return -1;
    name = strip_dashes(name);
    for (size_t i = 0; i < sizeof opt_tab / sizeof opt_tab[0]; i++)
        if (!strcmp(name, opt_tab[i].name))
            return opt_tab[i].val;
    return -1;
}

n265_config *n265_config_new(void)
{
    n265_config *c = calloc(1, sizeof *c);
    if (!c)
        return NULL;
    c->qp = 26; c->ctu = 64; c->keyint = 250; c->bframes = 7; c->aq = 1;
    c->vui_timing = 1;
    c->nr_intra = -1; c->nr_inter = -1;
    c->mcstf = -1; c->mcstf_range = -1;
    c->crf = -1.0;
    c->vbv_init = 0.9;
    c->weightp = -1; c->weightb = -1;
    c->open_gop = 1;
    c->scenecut = 1;
    c->sig_videoformat = c->sig_range = c->sig_overscan = -1;
    c->sig_colorprim = c->sig_transfer = c->sig_colormatrix = -1;
    c->sig_chromaloc = -1;
    for (int i = 0; i < N265_TUNE_NFIELD; i++)
        c->tune[i] = -1;
    c->dup_thr = -1.0;
    c->sar_idc = -1;
    c->maxcll = -1; c->maxfall = -1;
    c->hdr10 = -1; c->cll_on = 1; c->atc = -1; c->hdr_opt = -1;
    c->info = 1; c->fingerprint = 1;
    c->pic_struct = -1;
    c->crf_lo = -1e9; c->crf_hi = 1e9;
    c->qpmax = 51;
    c->ausize_factor = 1.0;
    c->nref = 1; c->badapt = 1;
    c->lookahead = 20; c->cutree = 1;
    c->rdoq_level = -1;
    c->tu_inter = -1; c->tu_intra = -1;
    return c;
}

void n265_config_free(n265_config *c)
{
    if (!c)
        return;
    free(c->level_str);
    free(c->h10p_path);
    free(c->rpu_path);
    free(c->qpfile_path);
    free(c->zones_spec);
    free(c->stats_path);
    free(c->preset);
    free(c);
}

static void opt_put(optstr *o, const char *key, const char *val)
{
    int n = snprintf(o->s + o->n, sizeof o->s - o->n, "%s%s%s%s",
                     o->n ? " " : "", key, val ? "=" : "", val ? val : "");
    if (n > 0 && (size_t)n < sizeof o->s - o->n)
        o->n += (size_t)n;
    else                        /* full: keep what fits and stop growing */
        o->s[o->n] = '\0';
}

/* Kept out of the SEI: paths, the thread count and other schedules,
 * byte-neutral reports, the SEI's own switch, and what the resolved head
 * names already. One command line must write one stream. */
static int opt_drop(const char *k)
{
    static const char * const drop[] = {
        "o", "dump-recon", "dump-recon-decode", "csv", "csv-log-level",
        "shot-stats", "shot-scan", "psnr", "ssim", "verbose", "v", "threads",
        "gop-parallel", "frame-stats", "control", "control-fd",
        "control-out",
        "show-config", "version", "help", "h",
        "info", "no-info",
        "preset", "qp", "crf", "bitrate", "hw", "hw-strict", NULL
    };
    for (int i = 0; drop[i]; i++)
        if (!strcmp(k, drop[i]))
            return 1;
    return 0;
}

/* the flag shapes the stream and its value is a file name */
static int opt_drop_value(const char *k)
{
    return !strcmp(k, "qpfile") || !strcmp(k, "stats") ||
           !strcmp(k, "shot-list") ||
           !strcmp(k, "dhdr10-info") || !strcmp(k, "dolby-vision-rpu");
}

static const char *long_name(const char *k)
{
    return !strcmp(k, "w") ? "weightp" : !strcmp(k, "t") ? "tune" : k;
}

void n265_config_note(n265_config *c, const char *name, const char *value)
{
    if (!c || !name)
        return;
    const char *k = long_name(strip_dashes(name));
    if (!*k || opt_drop(k))
        return;
    opt_put(&c->info_opts, k, value && !opt_drop_value(k) ? value : NULL);
}

static int set_str(char **dst, const char *v)
{
    char *s = strdup(v);
    if (!s)
        return N265_CONFIG_BAD_VALUE;
    free(*dst);
    *dst = s;
    return 0;
}

static int truthy(const char *v)
{
    return !strcmp(v, "1") || !strcmp(v, "true") || !strcmp(v, "yes") ||
           !strcmp(v, "on");
}

static int falsy(const char *v)
{
    return !strcmp(v, "0") || !strcmp(v, "false") || !strcmp(v, "no") ||
           !strcmp(v, "off");
}

static int vui_arg(const char *flag, int field, const char *v, int *out)
{
    *out = n265_vui_parse(field, v);
    if (*out >= 0)
        return 0;
    fprintf(stderr, "y265: --%s %s is not a value of that field. The "
            "names are:", flag, v);
    const char * const *n = n265_vui_names(field);
    for (int k = 0; n && n[k]; k++)
        fprintf(stderr, " %s", n[k]);
    fprintf(stderr, " - or the number itself (Annex E's tables "
            "number them in that order).\n");
    return N265_CONFIG_BAD_VALUE;
}

static int parse_one(n265_config *c, const char *k, const char *v);

int n265_config_parse(n265_config *c, const char *name, const char *value)
{
    if (!c || !name)
        return N265_CONFIG_UNKNOWN;
    const char *k = strip_dashes(name);
    int tv = n265_config_takes_value(k);
    if (tv < 0)
        return N265_CONFIG_UNKNOWN;
    if (tv && !value)
        return N265_CONFIG_NO_VALUE;
    if (!tv && value && *value && !truthy(value)) {
        /* a flag given a value, as key=value lists spell it: 0 is its
         * negation */
        char neg[64];
        if (!falsy(value))
            return N265_CONFIG_BAD_VALUE;
        if (!strncmp(k, "no-", 3))
            snprintf(neg, sizeof neg, "%s", k + 3);
        else
            snprintf(neg, sizeof neg, "no-%s", k);
        if (n265_config_takes_value(neg) != 0)
            return N265_CONFIG_BAD_VALUE;
        return n265_config_parse(c, neg, NULL);
    }
    int r = parse_one(c, k, tv ? value : NULL);
    if (r == 0) {
        n265_config_note(c, k, tv ? value : NULL);
        for (size_t i = 0; i < N_OPTS; i++)
            if (!strcmp(k, opt_tab[i].name))
                c->seen[i] = 1;
    }
    return r;
}

static int parse_one(n265_config *c, const char *k, const char *v)
{
#define IS(s) (!strcmp(k, (s)))
    if (IS("qp")) {
        c->qp = atoi(v);
    } else if (IS("crf")) {
        c->crf = atof(v); c->had_crf = 1;
    } else if (IS("idr-recovery-sei")) {
        c->recovery_sei = 1;
    } else if (IS("no-idr-recovery-sei")) {
        c->recovery_sei = 0;
    } else if (IS("pic-struct")) {
        c->pic_struct = atoi(v);
        if (c->pic_struct < 0 || c->pic_struct > 12) {
            fprintf(stderr, "y265: --pic-struct is Table D.2's value "
                    "0..12 (0 frame, 7 frame doubling, 8 frame "
                    "tripling)\n");
            return N265_CONFIG_BAD_VALUE;
        }
    } else if (IS("hrd-concat")) {
        c->hrd_concat = 1;
    } else if (IS("temporal-layers")) {
        c->tlayers = 1;
    } else if (IS("no-temporal-layers")) {
        c->tlayers = 0;
    } else if (IS("scaling-list")) {
        /* refused: the flag needs the default matrices in the quantizer,
         * the inverse, RDOQ and their kernels, or no decoder matches */
        fprintf(stderr, "y265: --scaling-list is not supported: the "
                "quantizer does not apply scaling matrices, so a stream "
                "that signalled them would not decode to the encoder's "
                "pictures.\n");
        return N265_CONFIG_BAD_VALUE;
    } else if (IS("crf-min")) {
        c->crf_lo = atof(v);
    } else if (IS("crf-max")) {
        c->crf_hi = atof(v);
    } else if (IS("qpmin")) {
        c->qpmin = atoi(v); c->had_qpmin = 1;
    } else if (IS("qpmax")) {
        c->qpmax = atoi(v); c->had_qpmax = 1;
    } else if (IS("strict-cbr")) {
        c->strict_cbr = 1;
    } else if (IS("max-ausize-factor")) {
        c->ausize_factor = atof(v);
    } else if (IS("qpfile")) {
        return set_str(&c->qpfile_path, v);
    } else if (IS("zones")) {
        return set_str(&c->zones_spec, v);
    } else if (IS("repeat-headers")) {
        c->repeat_hdr = 1;
    } else if (IS("no-repeat-headers")) {
        c->repeat_hdr = 0;
    } else if (IS("aud")) {
        c->aud = 1;
    } else if (IS("no-aud")) {
        c->aud = 0;
    } else if (IS("eos")) {
        c->eos = 1;
    } else if (IS("no-eos")) {
        c->eos = 0;
    } else if (IS("eob")) {
        c->eob = 1;
    } else if (IS("no-eob")) {
        c->eob = 0;
    } else if (IS("info")) {
        c->info = 1;
    } else if (IS("no-info")) {
        c->info = 0;
    } else if (IS("fingerprint")) {
        c->fingerprint = 1;
    } else if (IS("no-fingerprint")) {
        c->fingerprint = 0;
    } else if (IS("no-hrd")) {
        c->no_hrd = 1;
    } else if (IS("psnr")) {
        c->want_psnr = 1;
    } else if (IS("ssim")) {
        c->want_ssim = 1;
    } else if (IS("threads")) {
        c->threads = atoi(v);
    } else if (IS("bitrate")) {
        c->bitrate = atof(v);
    } else if (IS("ref")) {
        c->nref = atoi(v); c->had_ref = 1;
    } else if (IS("b-adapt")) {
        c->badapt = atoi(v); c->had_badapt = 1;
    } else if (IS("preset")) {
        return set_str(&c->preset, v);
    } else if (IS("keyint")) {
        c->keyint = atoi(v);
    } else if (IS("open-gop")) {
        c->open_gop = 1;
    } else if (IS("no-open-gop")) {
        c->open_gop = 0;
    } else if (IS("scenecut")) {
        c->scenecut = 1;
    } else if (IS("no-scenecut")) {
        c->scenecut = 0;
    } else if (IS("radl")) {
        c->radl = atoi(v);
    } else if (IS("bframes")) {
        c->bframes = atoi(v); c->had_bframes = 1;
    } else if (IS("aq")) {
        c->aq = atoi(v);
    } else if (IS("rc-lookahead")) {
        c->lookahead = atoi(v); c->had_la = 1;
    } else if (IS("cutree")) {
        c->cutree = atoi(v); c->had_cutree = 1;
    } else if (IS("ctu")) {
        c->ctu = atoi(v); c->had_ctu = 1;
    } else if (IS("rdoq-level")) {
        c->rdoq_level = atoi(v); c->had_rdoq = 1;
    } else if (IS("tu-inter-depth")) {
        c->tu_inter = atoi(v); c->had_tui = 1;
    } else if (IS("tu-intra-depth")) {
        c->tu_intra = atoi(v); c->had_tua = 1;
    } else if (IS("cbqpoffs")) {
        c->cbqpoffs = atoi(v); c->had_cbq = 1;
    } else if (IS("crqpoffs")) {
        c->crqpoffs = atoi(v); c->had_crq = 1;
    } else if (IS("pcm")) {
        c->pcm = 1;
    } else if (IS("lossless")) {
        c->lossless = 1;
    } else if (IS("weightp") || IS("w")) {
        c->weightp = 1;
    } else if (IS("no-weightp")) {
        c->weightp = 0;
    } else if (IS("weightb")) {
        c->weightb = 1;
    } else if (IS("no-weightb")) {
        c->weightb = 0;
    } else if (IS("level")) {
        return set_str(&c->level_str, v);
    } else if (IS("high-tier")) {
        c->high_tier = 1;
    } else if (IS("vui-timing")) {
        c->vui_timing = 1;
    } else if (IS("no-vui-timing")) {
        c->vui_timing = 0;
    } else if (IS("film-grain")) {
        c->film_grain = atof(v);
    } else if (IS("film-grain-denoise")) {
        c->fgc_denoise = atoi(v);
    } else if (IS("nr-intra")) {
        c->nr_intra = atoi(v); c->had_nr_intra = 1;
    } else if (IS("nr-inter")) {
        c->nr_inter = atoi(v); c->had_nr_inter = 1;
    } else if (IS("mcstf")) {
        c->mcstf = 1;
    } else if (IS("no-mcstf")) {
        c->mcstf = 0;
    } else if (IS("mcstf-ref-range")) {
        c->mcstf_range = atoi(v);
    } else if (IS("selective-mcstf")) {
        c->mcstf_selective = 1;
    } else if (IS("no-selective-mcstf")) {
        c->mcstf_selective = 0;
    } else if (IS("vbv-maxrate")) {
        c->vbv_maxrate = atof(v);
    } else if (IS("vbv-bufsize")) {
        c->vbv_bufsize = atof(v);
    } else if (IS("vbv-init")) {
        c->vbv_init = atof(v);
    } else if (IS("hash")) {
        c->hash = atoi(v);
    } else if (IS("colorprim")) {
        return vui_arg(k, N265_VUI_COLORPRIM, v, &c->sig_colorprim);
    } else if (IS("transfer")) {
        return vui_arg(k, N265_VUI_TRANSFER, v, &c->sig_transfer);
    } else if (IS("colormatrix")) {
        return vui_arg(k, N265_VUI_COLORMATRIX, v, &c->sig_colormatrix);
    } else if (IS("videoformat")) {
        return vui_arg(k, N265_VUI_VIDEOFORMAT, v, &c->sig_videoformat);
    } else if (IS("range")) {
        return vui_arg(k, N265_VUI_RANGE, v, &c->sig_range);
    } else if (IS("overscan")) {
        int r = vui_arg(k, N265_VUI_OVERSCAN, v, &c->sig_overscan);
        /* "unknown" is the absence of the field, not a value of it */
        if (c->sig_overscan == 2)
            c->sig_overscan = -1;
        return r;
    } else if (IS("video-signal-type-preset")) {
        const char *why = NULL;
        int rc = n265_vsp_parse(v, c->vsp, c->vsp_md, &c->vsp_md_on, &why);
        if (rc != N265_VSP_OK) {
            if (rc == N265_VSP_BAD_SYSTEM) {
                fprintf(stderr, "y265: --video-signal-type-preset %s "
                        "is not a system-id. The names are:", v);
                for (int i = 0; n265_vsp_name(i); i++)
                    fprintf(stderr, " %s", n265_vsp_name(i));
                fprintf(stderr, "\n");
            } else if (rc == N265_VSP_VOLUME_SYSTEM) {
                fprintf(stderr, "y265: --video-signal-type-preset %s "
                        "takes no colour volume: only the BT2100_PQ_* "
                        "systems carry one. Use --master-display "
                        "instead.\n", v);
            } else if (rc == N265_VSP_BAD_VOLUME) {
                fprintf(stderr, "y265: --video-signal-type-preset: no "
                        "such colour volume. The names are:");
                for (int i = 0; n265_vsp_volume_name(i); i++)
                    fprintf(stderr, " %s", n265_vsp_volume_name(i));
                fprintf(stderr, "\n");
            } else {
                fprintf(stderr, "y265: --video-signal-type-preset: "
                        "that colour volume is not supported: %s\n",
                        why ? why : "no reason recorded");
            }
            return N265_CONFIG_BAD_VALUE;
        }
        c->had_vsp = 1;
    } else if (IS("tune") || IS("t")) {
        const char *why = NULL;
        int rc = n265_tune_parse(v, c->tune, &why);
        if (rc == N265_TUNE_REFUSED) {
            fprintf(stderr, "y265: --tune %s is not supported: %s\n",
                    v, why);
            return N265_CONFIG_BAD_VALUE;
        }
        if (rc != N265_TUNE_OK) {
            fprintf(stderr, "y265: --tune %s is not a tune. The names "
                    "are:", v);
            for (int i = 0; n265_tune_name(i); i++)
                fprintf(stderr, " %s", n265_tune_name(i));
            fprintf(stderr, ". Only zerolatency is supported.\n");
            return N265_CONFIG_BAD_VALUE;
        }
        c->had_tune = 1;
    } else if (IS("intra-refresh")) {
        c->intra_refresh = 1;
    } else if (IS("dup-threshold")) {
        c->dup_thr = atof(v);
        if (c->dup_thr < 0.0 || c->dup_thr > 255.0) {
            fprintf(stderr, "y265: --dup-threshold is 0..255, in "
                    "8-bit luma levels per lowres pixel; 0 (the "
                    "default) is the strict form, where only an "
                    "identical source arms the path\n");
            return N265_CONFIG_BAD_VALUE;
        }
    } else if (IS("chromaloc")) {
        c->sig_chromaloc = atoi(v);
        if (c->sig_chromaloc < 0 || c->sig_chromaloc > 5) {
            fprintf(stderr, "y265: --chromaloc is 0..5 (E.2.1's "
                    "chroma_sample_loc_type)\n");
            return N265_CONFIG_BAD_VALUE;
        }
    } else if (IS("sar")) {
        if (n265_sar_parse(v, &c->sar_idc, &c->sar_w, &c->sar_h) < 0) {
            fprintf(stderr, "y265: --sar takes a ratio (\"16:9\") or a "
                    "Table E.1 index 0..16\n");
            return N265_CONFIG_BAD_VALUE;
        }
    } else if (IS("display-window")) {
        if (n265_window_parse(v, &c->win_l, &c->win_t, &c->win_r,
                              &c->win_b) < 0) {
            fprintf(stderr, "y265: --display-window is "
                    "\"left,top,right,bottom\" in pixels\n");
            return N265_CONFIG_BAD_VALUE;
        }
        c->win_on = 1;
    } else if (IS("master-display")) {
        if (n265_master_display_parse(v, c->md) < 0) {
            fprintf(stderr, "y265: --master-display is "
                    "G(x,y)B(x,y)R(x,y)WP(x,y)L(max,min); the eight "
                    "chromaticities are 0..65535 in units of "
                    "0.00002\n");
            return N265_CONFIG_BAD_VALUE;
        }
        c->have_md = 1;
    } else if (IS("max-cll")) {
        if (n265_cll_parse(v, &c->maxcll, &c->maxfall) < 0) {
            fprintf(stderr, "y265: --max-cll is \"cll,fall\", both "
                    "0..65535 cd/m2\n");
            return N265_CONFIG_BAD_VALUE;
        }
    } else if (IS("hdr-opt") || IS("hdr10-opt")) {
        c->hdr_opt = 1;
    } else if (IS("no-hdr-opt") || IS("no-hdr10-opt")) {
        c->hdr_opt = 0;
    } else if (IS("hdr10")) {
        c->hdr10 = 1;
    } else if (IS("no-hdr10")) {
        c->hdr10 = 0;
    } else if (IS("cll")) {
        c->cll_on = 1;
    } else if (IS("no-cll")) {
        c->cll_on = 0;
    } else if (IS("atc-sei")) {
        c->atc = n265_vui_parse(N265_VUI_TRANSFER, v);
        if (c->atc < 0) {
            fprintf(stderr, "y265: --atc-sei takes a transfer "
                    "characteristic, by name or by number\n");
            return N265_CONFIG_BAD_VALUE;
        }
    } else if (IS("dhdr10-info")) {
        return set_str(&c->h10p_path, v);
    } else if (IS("dolby-vision-rpu")) {
        return set_str(&c->rpu_path, v);
    } else if (IS("dolby-vision-profile")) {
        c->dv_profile = !strcmp(v, "8.1") || !strcmp(v, "81") ? 81
                      : !strcmp(v, "8.2") || !strcmp(v, "82") ? 82
                      : !strcmp(v, "8.4") || !strcmp(v, "84") ? 84 : -1;
        if (c->dv_profile < 0) {
            fprintf(stderr, "y265: --dolby-vision-profile is 8.1 (HDR10 "
                    "base layer), 8.2 (SDR BT.709) or 8.4 (HLG)\n");
            return N265_CONFIG_BAD_VALUE;
        }
    } else if (IS("shot-window")) {
        c->shot_window = atoi(v);
    } else if (IS("shot-keys")) {
        c->shot_keys = 1;
    } else if (IS("pass")) {
        c->pass = atoi(v);
    } else if (IS("stats")) {
        return set_str(&c->stats_path, v);
    } else if (IS("hw")) {
        c->hw = !strcmp(v, "off") ? N265_HW_OFF
              : !strcmp(v, "auto") ? N265_HW_AUTO
              : !strcmp(v, "videotoolbox") || !strcmp(v, "vt")
                ? N265_HW_VIDEOTOOLBOX : -1;
        if (c->hw < 0) {
            fprintf(stderr, "y265: --hw is off, auto or videotoolbox\n");
            return N265_CONFIG_BAD_VALUE;
        }
    } else if (IS("hw-strict")) {
        c->hw_strict = 1;
    } else {
        return N265_CONFIG_UNKNOWN;
    }
    return 0;
#undef IS
}

int n265_config_parse_list(n265_config *c, const char *list)
{
    if (!c)
        return N265_CONFIG_UNKNOWN;
    if (!list)
        return 0;
    char *dup = strdup(list);
    if (!dup)
        return N265_CONFIG_BAD_VALUE;
    int rc = 0;
    char *save = NULL;
    for (char *tok = strtok_r(dup, ":", &save); tok;
         tok = strtok_r(NULL, ":", &save)) {
        char *eq = strchr(tok, '=');
        if (eq)
            *eq = '\0';
        rc = n265_config_parse(c, tok, eq ? eq + 1 : NULL);
        if (rc == N265_CONFIG_UNKNOWN)
            fprintf(stderr, "y265: unknown option %s\n", tok);
        else if (rc == N265_CONFIG_NO_VALUE)
            fprintf(stderr, "y265: %s needs a value (%s=...)\n", tok, tok);
        else if (rc == N265_CONFIG_BAD_VALUE && eq &&
                 n265_config_takes_value(tok) == 0)
            fprintf(stderr, "y265: %s is a switch: give it no value, or "
                    "0 or 1\n", tok);
        if (rc)
            break;
    }
    free(dup);
    return rc;
}

/* The hardware mode's half of apply: the options the hardware can't
 * honour, named once (at open, if a session opens), and the ones it has
 * no path for, which refuse --hw videotoolbox and put auto in software. */
static int hw_resolve(n265_config *c)
{
    n265_set_hw(c->hw);
    n265_set_hw_opts(0, NULL);
    int mode = n265_hw_mode();
    c->hw_on = 0;
    if (mode == N265_HW_OFF)
        return 0;
    char warn[2048];
    size_t wn = 0;
    int nwarn = 0;
    warn[0] = '\0';
    for (size_t i = 0; i < N_OPTS; i++) {
        if (!c->seen[i])
            continue;
        const char *note = NULL;
        enum n265_hw_class cls = n265_hw_option_class(opt_tab[i].name, &note);
        if (cls == N265_HWC_FATAL) {
            if (mode == N265_HW_VIDEOTOOLBOX) {
                fprintf(stderr, "y265: --hw videotoolbox: --%s has no "
                        "hardware path (%s)\n", opt_tab[i].name, note);
                return -1;
            }
            fprintf(stderr, "y265: --hw auto: --%s has no hardware path "
                    "(%s); using the software encoder\n", opt_tab[i].name,
                    note);
            n265_set_hw_opts(1, NULL);
            return 0;
        }
        if (cls != N265_HWC_IGNORED && cls != N265_HWC_APPROX)
            continue;
        nwarn++;
        int n = snprintf(warn + wn, sizeof warn - wn, "y265: hw:   --%s (%s%s%s)\n",
                         opt_tab[i].name,
                         cls == N265_HWC_IGNORED ? "ignored" : "approximated",
                         note ? ": " : "", note ? note : "");
        if (n > 0 && (size_t)n < sizeof warn - wn)
            wn += (size_t)n;
    }
    if (nwarn && c->hw_strict) {
        fprintf(stderr, "y265: --hw-strict: %d option%s the hardware won't "
                "honour as given:\n%s", nwarn, nwarn == 1 ? "" : "s", warn);
        return -1;
    }
    if (nwarn) {
        char all[2200];
        snprintf(all, sizeof all, "y265: hw: %d option%s not honoured as "
                 "given by the hardware encoder:\n%s", nwarn,
                 nwarn == 1 ? "" : "s", warn);
        n265_set_hw_opts(0, all);
    }
    c->hw_on = 1;
    return 0;
}

int n265_config_apply(n265_config *c, n265_param *p)
{
    if (!c || !p)
        return N265_CONFIG_BAD_VALUE;
    const int depth = N265_BIT_DEPTH;
    const int qp_bd_off = QP_BD_OFF;
    const int w = p->width, h = p->height, csp = p->csp;
    const int csx = csp == N265_CSP_I444 ? 0 : 1;
    const int csy = csp == N265_CSP_I420 ? 1 : 0;
#define FAIL return N265_CONFIG_BAD_VALUE

    /* QpY runs -QpBdOffsetY..51 (7.4.3.3); the rate factor shares it */
    if (c->qp < -qp_bd_off || c->qp > 51) {
        fprintf(stderr, "y265: qp out of range %d..51 at %d bits\n",
                -qp_bd_off, depth);
        FAIL;
    }
    if (c->had_crf && c->crf < -qp_bd_off) {
        fprintf(stderr, "y265: --crf %g is below the %d-bit floor of %d "
                "(QpBdOffsetY, 7.4.3.3)\n", c->crf, depth, -qp_bd_off);
        FAIL;
    }
    /* the preset only fills in what the options left out */
    if (c->preset) {
        int pr = n265_set_preset(c->preset);
        if (pr == -2) {
            fprintf(stderr, "y265: --preset %s is not supported; the "
                            "nearest preset is %s\n",
                    c->preset, n265_preset_nearest(c->preset));
            FAIL;
        }
        if (pr < 0) {
            fprintf(stderr, "y265: --preset must be fast, medium, slow "
                            "or slower\n");
            FAIL;
        }
    }
    /* the tune: after the options, so a named flag wins, and before the
     * preset fallbacks, so the tune beats the preset */
    int bframes = c->bframes, lookahead = c->lookahead, badapt = c->badapt;
    int cutree = c->cutree, mcstf = c->mcstf;
    int had_bframes = c->had_bframes, had_la = c->had_la;
    int had_badapt = c->had_badapt, la_from_tune = 0;
    if (c->had_tune) {
        if (!had_bframes && c->tune[N265_TUNE_BFRAMES] >= 0) {
            bframes = c->tune[N265_TUNE_BFRAMES]; had_bframes = 1;
        }
        if (!had_la && c->tune[N265_TUNE_LOOKAHEAD] >= 0) {
            lookahead = c->tune[N265_TUNE_LOOKAHEAD]; had_la = 1;
            la_from_tune = 1;
        }
        if (!had_badapt && c->tune[N265_TUNE_BADAPT] >= 0) {
            badapt = c->tune[N265_TUNE_BADAPT]; had_badapt = 1;
        }
        if (!c->had_cutree && c->tune[N265_TUNE_CUTREE] >= 0)
            cutree = c->tune[N265_TUNE_CUTREE];
        /* the filter holds pictures back, which is latency */
        if (mcstf < 0 && c->tune[N265_TUNE_FRAMEWORK] >= 0)
            mcstf = 0;
        /* frames in flight: a schedule, so it moves no bit; an explicit
         * N265_MAX_FRAME_WORK still wins */
        if (c->tune[N265_TUNE_FRAMEWORK] >= 0) {
            char kv[16];
            snprintf(kv, sizeof kv, "%d", c->tune[N265_TUNE_FRAMEWORK]);
            setenv("N265_MAX_FRAME_WORK", kv, 0);
        }
    }
    /* the resolution-aware half of the preset, asked about the source's
     * own size */
    int nref = c->nref, ctu = c->ctu;
    int tu_inter = c->tu_inter, tu_intra = c->tu_intra;
    n265_preset_uhd(w, h);
    if (!c->had_ref) nref = n265_preset_param_res("ref", nref, w, h);
    if (!had_bframes) bframes = n265_preset_param_res("bframes", bframes,
                                                      w, h);
    if (!had_badapt)  badapt = n265_preset_param_res("b-adapt", badapt,
                                                     w, h);
    if (!had_la)      lookahead = n265_preset_param_res("rc-lookahead",
                                                        lookahead, w, h);
    /* a tree the resolution rule moved may be switched back at open; a
     * named one may not */
    int ctu_blind = n265_preset_param("ctu", ctu), uhd_tree = 0;
    if (!c->had_ctu) {
        ctu = n265_preset_param_res("ctu", ctu, w, h);
        uhd_tree = ctu != ctu_blind;
    }
    if (!c->had_tui) tu_inter = n265_preset_param_res("tu-inter-depth",
                                                      tu_inter, w, h);
    if (!c->had_tua) tu_intra = n265_preset_param_res("tu-intra-depth",
                                                      tu_intra, w, h);
    if (ctu != 16 && ctu != 32 && ctu != 64) {
        fprintf(stderr, "y265: --ctu must be 16, 32 or 64\n");
        FAIL;
    }
    /* N265_MAX_BFRAMES and LA_DEPTH_MAX, checked after the preset since
     * a preset can set either */
    if (bframes < 0 || bframes > 15) {
        fprintf(stderr, "y265: --bframes out of range 0..15\n");
        FAIL;
    }
    if (lookahead < 1 || lookahead > 60) {
        fprintf(stderr, "y265: --rc-lookahead out of range 1..60\n");
        FAIL;
    }
    if (c->pass < 0 || c->pass > 3) {
        fprintf(stderr, "y265: --pass is 1 (write the stats), 2 (plan from "
                "them) or 3 (both)\n");
        FAIL;
    }
    if (c->pass && !c->stats_path) {
        fprintf(stderr, "y265: --pass %d needs --stats <file>\n", c->pass);
        FAIL;
    }
    if (c->stats_path && !c->pass) {
        fprintf(stderr, "y265: --stats needs --pass 1, 2 or 3\n");
        FAIL;
    }
    if (c->shot_window < 0 || c->shot_window > 300) {
        fprintf(stderr, "y265: --shot-window out of range 0..300\n");
        FAIL;
    }
    /* a shallower window would silently shorten every mini-GOP */
    if (lookahead < bframes + 1) {
        fprintf(stderr, "y265: --rc-lookahead %d is too shallow for "
                "--bframes %d: the window must hold the whole mini-GOP, so "
                "--rc-lookahead must be at least %d%s\n",
                lookahead, bframes, bframes + 1,
                la_from_tune ? " (the window of 1 is --tune zerolatency's; "
                               "name --rc-lookahead yourself to widen it)"
                             : "");
        FAIL;
    }
    /* --intra-refresh: P frames only, 4:2:0 only, and none of the options
     * that need key frames or code every block alike */
    int open_gop = c->open_gop;
    if (c->intra_refresh) {
        int named_ogop = 0;
        for (size_t i = 0; i < N_OPTS; i++)
            if (c->seen[i] && !strcmp(opt_tab[i].name, "open-gop"))
                named_ogop = 1;
        const char *why =
            had_bframes && bframes > 0 ? "B frames; drop --bframes or set "
                                         "it to 0"
            : named_ogop ? "--open-gop, a different random-access scheme"
            : c->keyint < 2 ? "--keyint below 2, which leaves no sweep"
            : c->pcm ? "--pcm"
            : csp != N265_CSP_I420 ? "4:2:2 or 4:4:4 input; it is 4:2:0 only"
            : NULL;
        if (why) {
            fprintf(stderr, "y265: --intra-refresh cannot be combined with "
                    "%s\n", why);
            FAIL;
        }
        bframes = 0;
        open_gop = 0;
    }
    int ctb_log2 = ctu == 16 ? 4 : ctu == 32 ? 5 : 6;
    n265_set_mode(c->pcm ? 1 : 0, c->qp, ctb_log2, c->keyint, bframes,
                  c->aq);
    /* after set_mode, which clears it */
    n265_set_uhd_tree(uhd_tree);
    if (c->had_crf) n265_set_crf(c->crf);
    if (c->bitrate > 0) n265_set_bitrate(c->bitrate);
    n265_set_ref(nref);
    n265_set_badapt(badapt);
    n265_set_la(lookahead, cutree);
    n265_set_shot_window(c->shot_window);
    if (c->shot_keys && !c->scenecut) {
        fprintf(stderr, "y265: --shot-keys puts a key frame on each cut and "
                "--no-scenecut keeps them off cuts; pick one\n");
        FAIL;
    }
    n265_set_shot_keys(c->shot_keys);
    n265_set_pass(c->pass, c->stats_path);
    n265_set_threads(c->threads);
    if (c->pcm && c->lossless) {
        fprintf(stderr, "y265: --pcm and --lossless are two different "
                "lossless modes; pick one\n");
        FAIL;
    }
    n265_set_lossless(c->lossless);
    if (c->had_rdoq && (c->rdoq_level < 0 || c->rdoq_level > 2)) {
        fprintf(stderr, "y265: --rdoq-level must be 0 (off), 1 (final "
                "encode only) or 2 (all RD decisions)\n");
        FAIL;
    }
    n265_set_rdoq_level(c->rdoq_level);
    if ((tu_inter > 0 && (tu_inter < 1 || tu_inter > 4)) ||
        (tu_intra > 0 && (tu_intra < 1 || tu_intra > 4))) {
        fprintf(stderr, "y265: --tu-inter-depth / --tu-intra-depth must be "
                "1..4 (levels of transform tree; 2 is the default)\n");
        FAIL;
    }
    /* 7.4.3.2.1 bounds the transform tree by the coding tree; a named
     * depth past it is refused, a preset's is clamped at open */
    {
        int tu_cap = ctb_log2 - 1;
        if ((c->had_tui && tu_inter > tu_cap) ||
            (c->had_tua && tu_intra > tu_cap)) {
            fprintf(stderr, "y265: --tu-inter-depth / --tu-intra-depth above "
                    "%d needs a coding tree larger than --ctu %d\n",
                    tu_cap, ctu);
            FAIL;
        }
    }
    n265_set_tu_depth(tu_inter, tu_intra);
    if (c->cbqpoffs < -12 || c->cbqpoffs > 12 || c->crqpoffs < -12 ||
        c->crqpoffs > 12) {
        fprintf(stderr, "y265: --cbqpoffs and --crqpoffs take -12..12 "
                "(7.4.3.3.1); negative spends more on chroma\n");
        FAIL;
    }
    /* 100 is "not given": the preset's row stays in charge */
    n265_set_chroma_qpoffs(c->had_cbq ? c->cbqpoffs : 100,
                           c->had_crq ? c->crqpoffs : 100);
    {
        int idc = 0;
        if (c->level_str) {
            idc = n265_level_parse(c->level_str);
            if (!idc) {
                fprintf(stderr, "y265: --level %s is not a level of Annex A "
                        "(1, 2, 2.1, 3, 3.1, 4, 4.1, 5, 5.1, 5.2, 6, 6.1, "
                        "6.2; \"41\" spells 4.1). The raw idc is not a "
                        "spelling: 30 and 60 are each an idc AND a level "
                        "written ten times.\n", c->level_str);
                FAIL;
            }
        }
        n265_set_level(idc, c->high_tier);
    }
    n265_set_vui_timing(c->vui_timing);
    n265_set_film_grain(c->film_grain, c->fgc_denoise);
    if ((c->had_nr_intra && (c->nr_intra < 0 || c->nr_intra > 2000)) ||
        (c->had_nr_inter && (c->nr_inter < 0 || c->nr_inter > 2000))) {
        fprintf(stderr, "y265: --nr-intra/--nr-inter take 0..2000; "
                "0 is off\n");
        FAIL;
    }
    n265_set_nr(c->nr_intra, c->nr_inter);
    if (c->mcstf_range != -1 && (c->mcstf_range < 1 || c->mcstf_range > 4)) {
        fprintf(stderr, "y265: --mcstf-ref-range takes 1..4 neighbours "
                "per side; --no-mcstf turns the filter off\n");
        FAIL;
    }
    n265_set_mcstf(mcstf, c->mcstf_range, c->mcstf_selective);
    n265_set_vbv(c->vbv_maxrate, c->vbv_bufsize, c->vbv_init);
    n265_set_weight(c->weightp, c->weightb);
    if (c->hash < 0 || c->hash > 3) {
        fprintf(stderr, "y265: --hash is 0 (off), 1 (MD5), 2 (CRC) or "
                "3 (checksum)\n");
        FAIL;
    }
    n265_set_metrics(c->want_psnr, c->want_ssim);
    /* the SEI's head: what the run resolved, then the options in the
     * order they were given */
    {
        optstr head = { .s = "", .n = 0 };
        char v[32];
        opt_put(&head, "preset", c->preset ? c->preset : "medium");
        if (c->pcm) {
            opt_put(&head, "rc", "pcm");
        } else if (c->lossless) {
            opt_put(&head, "rc", "lossless");
        } else if (c->bitrate > 0.0) {
            opt_put(&head, "rc", "abr");
            snprintf(v, sizeof v, "%g", c->bitrate);
            opt_put(&head, "bitrate", v);
        } else if (c->had_crf) {
            opt_put(&head, "rc", "crf");
            snprintf(v, sizeof v, "%.2f", c->crf);
            opt_put(&head, "crf", v);
        } else {
            opt_put(&head, "rc", "cqp");
            snprintf(v, sizeof v, "%d", c->qp);
            opt_put(&head, "qp", v);
        }
        snprintf(c->info_all, sizeof c->info_all, "%s%s%s", head.s,
                 c->info_opts.n ? " " : "", c->info_opts.s);
        n265_set_info_opts(c->info_all);
    }
    n265_set_nal_opts(c->repeat_hdr, c->aud, c->eos, c->eob, c->info);
    n265_set_fingerprint(c->fingerprint);
    n265_set_hrd(c->no_hrd ? 0 : -1);
    /* unnamed, the bounds are the depth's own QpY range and inert */
    int qpmin = c->had_qpmin ? c->qpmin : -qp_bd_off;
    int qpmax = c->had_qpmax ? c->qpmax : 51;
    if (qpmin < -qp_bd_off || qpmin > 51 || qpmax < -qp_bd_off ||
        qpmax > 51) {
        fprintf(stderr, "y265: --qpmin and --qpmax are QpY values, %d..51 "
                "at %d bits (QpBdOffsetY, 7.4.3.3)\n", -qp_bd_off, depth);
        FAIL;
    }
    n265_set_sei_opts(c->recovery_sei, c->pic_struct, c->hrd_concat,
                      c->tlayers);
    n265_set_rc_bounds(c->crf_lo, c->crf_hi, qpmin, qpmax, c->strict_cbr,
                       c->ausize_factor);
    n265_set_qpfile(c->qpfile_path);
    n265_set_zones(c->zones_spec);
    n265_set_hash(c->hash);
    if (c->radl < 0 || c->radl > bframes) {
        fprintf(stderr, "y265: --radl is 0..%d here (a CRA has at most "
                "--bframes leading pictures)\n", bframes);
        FAIL;
    }
    if (c->radl && !c->open_gop) {
        fprintf(stderr, "y265: --radl needs --open-gop (RADL pictures are "
                "the leading pictures of a CRA)\n");
        FAIL;
    }
    n265_set_gop(open_gop, c->radl);
    n265_set_intra_refresh(c->intra_refresh);
    n265_set_scenecut(c->scenecut);
    /* the window is signaled in chroma units (7.4.3.2.1) */
    if (c->win_on) {
        if ((c->win_l & ((1 << csx) - 1)) || (c->win_r & ((1 << csx) - 1)) ||
            (c->win_t & ((1 << csy) - 1)) || (c->win_b & ((1 << csy) - 1))) {
            fprintf(stderr, "y265: --display-window offsets are signalled "
                    "in chroma samples, so at this chroma format the "
                    "horizontal ones must be multiples of %d and the "
                    "vertical ones of %d\n", 1 << csx, 1 << csy);
            FAIL;
        }
        if (c->win_l + c->win_r >= w || c->win_t + c->win_b >= h) {
            fprintf(stderr, "y265: --display-window %d,%d,%d,%d leaves no "
                    "picture inside %dx%d\n", c->win_l, c->win_t, c->win_r,
                    c->win_b, w, h);
            FAIL;
        }
    }
    /* the signal-type preset fills only the fields left alone */
    int sig_colorprim = c->sig_colorprim, sig_transfer = c->sig_transfer;
    int sig_colormatrix = c->sig_colormatrix, sig_range = c->sig_range;
    int sig_chromaloc = c->sig_chromaloc, have_md = c->have_md;
    uint32_t md[10];
    memcpy(md, c->md, sizeof md);
    if (c->had_vsp) {
        if (sig_colorprim < 0)   sig_colorprim   = c->vsp[N265_VUI_COLORPRIM];
        if (sig_transfer < 0)    sig_transfer    = c->vsp[N265_VUI_TRANSFER];
        if (sig_colormatrix < 0) sig_colormatrix = c->vsp[N265_VUI_COLORMATRIX];
        if (sig_range < 0)       sig_range       = c->vsp[N265_VUI_RANGE];
        if (sig_chromaloc < 0)   sig_chromaloc   = c->vsp[N265_VSP_CHROMALOC];
        if (!have_md && c->vsp_md_on) {
            memcpy(md, c->vsp_md, sizeof md);
            have_md = 1;
        }
    }
    c->res_transfer = sig_transfer;
    n265_set_vui_colour(c->sig_videoformat, sig_range, sig_colorprim,
                        sig_transfer, sig_colormatrix, sig_chromaloc,
                        c->sig_overscan);
    n265_set_dup_thr(c->dup_thr);
    n265_set_vui_sar(c->sar_idc, c->sar_w, c->sar_h);
    n265_set_vui_window(c->win_on, c->win_l, c->win_t, c->win_r, c->win_b);
    n265_set_hdr_meta(have_md ? md : NULL, c->maxcll, c->maxfall);
    n265_set_hdr10(c->hdr10, c->cll_on, c->atc);
    n265_set_hdr_opt(c->hdr_opt);
    /* dynamic metadata rides on a stream that says it is PQ or HLG */
    if (c->h10p_path && (sig_transfer != 16 || sig_colorprim != 9 ||
                         sig_colormatrix != 9 || depth != 10)) {
        fprintf(stderr, "y265: --dhdr10-info is HDR10+ metadata, which "
                "rides on HDR10: a 10-bit encode (or --output-depth 10) "
                "with --transfer smpte2084 --colorprim bt2020 "
                "--colormatrix bt2020nc\n");
        FAIL;
    }
    if (!c->rpu_path != !c->dv_profile) {
        fprintf(stderr, "y265: --dolby-vision-rpu and "
                "--dolby-vision-profile go together\n");
        FAIL;
    }
    if (c->rpu_path) {
        int hlg = sig_transfer == 18 || (sig_transfer == 14 && c->atc == 18);
        if (depth != 10 || csp != N265_CSP_I420 || sig_range == 1) {
            fprintf(stderr, "y265: Dolby Vision profile 8 is 10-bit 4:2:0 "
                    "at limited range: give a 4:2:0 input, 10-bit or with "
                    "--output-depth 10, and no --range full\n");
            FAIL;
        }
        int dv = c->dv_profile;
        int ok = dv == 82
                     ? sig_colorprim == 1 && sig_colormatrix == 1 &&
                       sig_transfer == 1
                     : sig_colorprim == 9 && sig_colormatrix == 9 &&
                       (dv == 81 ? sig_transfer == 16 : hlg);
        if (!ok) {
            fprintf(stderr, "y265: Dolby Vision profile %s needs %s\n",
                    dv == 81 ? "8.1" : dv == 82 ? "8.2" : "8.4",
                    dv == 81
                    ? "--colorprim bt2020 --colormatrix bt2020nc --transfer "
                      "smpte2084"
                    : dv == 82
                    ? "--colorprim bt709 --colormatrix bt709 --transfer bt709"
                    : "--colorprim bt2020 --colormatrix bt2020nc and "
                      "--transfer arib-std-b67 (or --transfer bt2020-10 with "
                      "--atc-sei arib-std-b67)");
            FAIL;
        }
    }
    if (c->rpu_path && !(c->vbv_maxrate > 0.0 && c->vbv_bufsize > 0.0))
        fprintf(stderr, "y265: WARNING Dolby Vision is normally delivered "
                "under a VBV: consider --vbv-maxrate and --vbv-bufsize\n");
    if (c->dv_profile == 81 && !have_md)
        fprintf(stderr, "y265: WARNING a Dolby Vision 8.1 base layer is "
                "HDR10, which carries a mastering display: consider "
                "--master-display\n");

    /* 8-bit PQ or HLG is legal but bands badly; the input was likely
     * meant for Main10 */
    if (depth == 8 && (sig_transfer == 16 || sig_transfer == 18 ||
                       c->atc == 18))
        fprintf(stderr, "y265: WARNING %s at 8 bits will show banding: "
                "give a 10-bit input or add --output-depth 10\n",
                sig_transfer == 16 ? "PQ" : "HLG");
    /* HDR10 metadata on a stream that says neither PQ nor HLG is written
     * but means nothing to a player */
    if ((have_md || c->maxcll >= 0) &&
        !(sig_transfer == 16 || sig_transfer == 18 || c->atc == 18))
        fprintf(stderr, "y265: WARNING --master-display and --max-cll "
                "describe HDR, but the stream isn't marked PQ or HLG: add "
                "--transfer smpte2084 or arib-std-b67\n");

    if (hw_resolve(c) != 0)
        FAIL;

    n265_param q;
    n265_param_default(&q);
    q.width = w;
    q.height = h;
    q.csp = csp;
    q.fps_num = p->fps_num;
    q.fps_den = p->fps_den;
    *p = q;

    c->bframes = bframes;
    c->lookahead = lookahead;
    c->nref = nref;
    c->ctu = ctu;
    c->applied = 1;
    return 0;
#undef FAIL
}

int n265_config_get(const n265_config *c, const char *key, double *v)
{
    if (!c || !key || !v)
        return -1;
    key = strip_dashes(key);
#define K(s, x) if (!strcmp(key, (s))) { *v = (double)(x); return 0; }
    K("keyint", c->keyint)
    K("bframes", c->bframes)
    K("ref", c->nref)
    K("rc-lookahead", c->lookahead)
    K("ctu", c->ctu)
    K("bitrate", c->bitrate)
    K("crf", c->had_crf ? c->crf : -1.0)
    K("qp", c->qp)
    K("vbv-maxrate", c->vbv_maxrate)
    K("vbv-bufsize", c->vbv_bufsize)
    K("pass", c->pass)
    K("hrd-concat", c->hrd_concat)
    K("repeat-headers", c->repeat_hdr)
    K("threads", c->threads)
    K("psnr", c->want_psnr)
    K("ssim", c->want_ssim)
    K("open-gop", c->open_gop)
    K("intra-refresh", c->intra_refresh)
    K("scenecut", c->scenecut)
    K("dolby-vision-profile", c->dv_profile)
    K("depth", N265_BIT_DEPTH)
    K("hw", c->hw_on ? n265_hw_mode() : N265_HW_OFF)
    K("transfer", c->applied ? c->res_transfer : c->sig_transfer)
    K("atc-sei", c->atc)
    K("hdr-opt", c->hdr_opt)
    K("lossless", c->lossless)
#undef K
    return -1;
}

const char *n265_config_get_str(const n265_config *c, const char *key)
{
    if (!c || !key)
        return NULL;
    key = strip_dashes(key);
    if (!strcmp(key, "preset"))           return c->preset;
    if (!strcmp(key, "qpfile"))           return c->qpfile_path;
    if (!strcmp(key, "zones"))            return c->zones_spec;
    if (!strcmp(key, "stats"))            return c->stats_path;
    if (!strcmp(key, "dhdr10-info"))      return c->h10p_path;
    if (!strcmp(key, "dolby-vision-rpu")) return c->rpu_path;
    if (!strcmp(key, "info"))             return c->applied ? c->info_all
                                                            : NULL;
    return NULL;
}
