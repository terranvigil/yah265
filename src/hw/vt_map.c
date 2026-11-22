/*
 * vt_map.c - the hardware mode's option map (vt_map.h).
 * Copyright (c) 2026, the yah265 authors. GPL-2.0-or-later; see LICENSE.
 */

#include <stddef.h>
#include <string.h>

#include "hw/vt_map.h"

static const n265_hw_opt table[] = {
    { "keyint",         N265_HWC_MAPS,    "maximum key frame interval" },
    { "bitrate",        N265_HWC_MAPS,    "average bitrate" },
    { "sar",            N265_HWC_MAPS,    "pixel aspect ratio" },
    { "colorprim",      N265_HWC_MAPS,    "color primaries" },
    { "transfer",       N265_HWC_MAPS,    "transfer function" },
    { "colormatrix",    N265_HWC_MAPS,    "YCbCr matrix" },
    { "range",          N265_HWC_MAPS,    "video or full range buffers" },
    { "master-display", N265_HWC_MAPS,    "mastering display SEI" },
    { "max-cll",        N265_HWC_MAPS,    "content light level SEI" },
    { "repeat-headers", N265_HWC_MAPS,    "VPS, SPS and PPS before every key frame" },
    { "vbv-maxrate",    N265_HWC_APPROX,  "a data-rate limit over a window of bufsize / maxrate" },
    { "vbv-bufsize",    N265_HWC_APPROX,  "a data-rate limit over a window of bufsize / maxrate" },
    { "crf",            N265_HWC_APPROX,  "the hardware's quality key through a rate-matched table" },
    { "qp",             N265_HWC_APPROX,  "pins the frame QP range; the hardware may still move QP inside a frame" },
    { "bframes",        N265_HWC_APPROX,  "frame reordering on or off; the count is the hardware's" },
    { "tune",           N265_HWC_APPROX,  "zerolatency is the hardware's real-time mode; the rest are ignored" },
    { "t",              N265_HWC_APPROX,  "zerolatency is the hardware's real-time mode; the rest are ignored" },
    { "preset",         N265_HWC_IGNORED, "the hardware has no presets" },
    { "threads",        N265_HWC_MAPS,    "a schedule; the hardware has its own" },
    { "level",          N265_HWC_IGNORED, "the hardware picks the level" },
    { "high-tier",      N265_HWC_IGNORED, "the hardware picks the tier" },
    { "ref",            N265_HWC_IGNORED, "the reference count is the hardware's" },
    { "b-adapt",        N265_HWC_IGNORED, NULL },
    { "open-gop",       N265_HWC_IGNORED, "the GOP structure is the hardware's" },
    { "no-open-gop",    N265_HWC_IGNORED, "the GOP structure is the hardware's" },
    { "radl",           N265_HWC_IGNORED, "the GOP structure is the hardware's" },
    { "no-scenecut",    N265_HWC_IGNORED, "the hardware places its own key frames" },
    { "rc-lookahead",   N265_HWC_IGNORED, NULL },
    { "cutree",         N265_HWC_IGNORED, NULL },
    { "aq",             N265_HWC_IGNORED, NULL },
    { "ctu",            N265_HWC_IGNORED, "the hardware picks its own coding tree" },
    { "rdoq-level",     N265_HWC_IGNORED, NULL },
    { "tu-inter-depth", N265_HWC_IGNORED, NULL },
    { "tu-intra-depth", N265_HWC_IGNORED, NULL },
    { "cbqpoffs",       N265_HWC_IGNORED, NULL },
    { "crqpoffs",       N265_HWC_IGNORED, NULL },
    { "weightp",        N265_HWC_IGNORED, NULL },
    { "w",              N265_HWC_IGNORED, NULL },
    { "no-weightp",     N265_HWC_IGNORED, NULL },
    { "weightb",        N265_HWC_IGNORED, NULL },
    { "no-weightb",     N265_HWC_IGNORED, NULL },
    { "nr-intra",       N265_HWC_IGNORED, NULL },
    { "nr-inter",       N265_HWC_IGNORED, NULL },
    { "mcstf",          N265_HWC_IGNORED, NULL },
    { "mcstf-ref-range", N265_HWC_IGNORED, NULL },
    { "selective-mcstf", N265_HWC_IGNORED, NULL },
    { "film-grain",     N265_HWC_IGNORED, NULL },
    { "film-grain-denoise", N265_HWC_IGNORED, NULL },
    { "dup-threshold",  N265_HWC_IGNORED, NULL },
    { "shot-window",    N265_HWC_IGNORED, NULL },
    { "hdr-opt",        N265_HWC_IGNORED, NULL },
    { "hdr10-opt",      N265_HWC_IGNORED, NULL },
    { "crf-min",        N265_HWC_IGNORED, "the hardware's rate loop is its own" },
    { "crf-max",        N265_HWC_IGNORED, "the hardware's rate loop is its own" },
    { "qpmin",          N265_HWC_IGNORED, "the hardware's rate loop is its own" },
    { "qpmax",          N265_HWC_IGNORED, "the hardware's rate loop is its own" },
    { "vbv-init",       N265_HWC_IGNORED, "the hardware's buffer loop is its own" },
    { "strict-cbr",     N265_HWC_IGNORED, "the hardware's buffer loop is its own" },
    { "max-ausize-factor", N265_HWC_IGNORED, NULL },
    { "qpfile",         N265_HWC_IGNORED, "no per-frame type or QP on the hardware" },
    { "zones",          N265_HWC_IGNORED, "no per-range rate on the hardware" },
    { "pass",           N265_HWC_IGNORED, "no two-pass on the hardware" },
    { "stats",          N265_HWC_IGNORED, "no two-pass on the hardware" },
    { "hash",           N265_HWC_IGNORED, "the hardware writes no picture hash" },
    { "aud",            N265_HWC_IGNORED, NULL },
    { "eos",            N265_HWC_IGNORED, NULL },
    { "eob",            N265_HWC_IGNORED, NULL },
    { "idr-recovery-sei", N265_HWC_IGNORED, NULL },
    { "pic-struct",     N265_HWC_IGNORED, NULL },
    { "hrd-concat",     N265_HWC_IGNORED, NULL },
    { "temporal-layers", N265_HWC_IGNORED, NULL },
    { "videoformat",    N265_HWC_IGNORED, NULL },
    { "overscan",       N265_HWC_IGNORED, NULL },
    { "chromaloc",      N265_HWC_IGNORED, NULL },
    { "display-window", N265_HWC_IGNORED, NULL },
    { "atc-sei",        N265_HWC_IGNORED, NULL },
    { "info",           N265_HWC_IGNORED, "the hardware stream carries none of our SEI" },
    { "fingerprint",    N265_HWC_IGNORED, "the hardware stream carries no fingerprint" },
    { "vui-timing",     N265_HWC_IGNORED, NULL },
    { "no-vui-timing",  N265_HWC_IGNORED, NULL },
    { "pcm",            N265_HWC_FATAL,   "no PCM mode on the hardware" },
    { "lossless",       N265_HWC_FATAL,   "no lossless mode on the hardware" },
    { "psnr",           N265_HWC_FATAL,   "no reconstruction to measure" },
    { "ssim",           N265_HWC_FATAL,   "no reconstruction to measure" },
    { "intra-refresh",  N265_HWC_FATAL,   "no intra refresh on the hardware" },
    { "dhdr10-info",    N265_HWC_FATAL,   "no per-picture HDR10+ on the hardware" },
    { "dolby-vision-rpu", N265_HWC_FATAL, "no per-picture RPU on the hardware" },
    { "dolby-vision-profile", N265_HWC_FATAL, "no per-picture RPU on the hardware" },
    { NULL, N265_HWC_MAPS, NULL }
};

enum n265_hw_class n265_hw_option_class(const char *name, const char **note)
{
    for (const n265_hw_opt *o = table; o->name; o++)
        if (!strcmp(o->name, name)) {
            if (note)
                *note = o->note;
            return o->cls;
        }
    if (note)
        *note = NULL;
    return N265_HWC_MAPS;
}

const n265_hw_opt *n265_hw_option_table(void)
{
    return table;
}
