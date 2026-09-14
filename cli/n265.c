/*
 * n265: Y4M in, Annex-B HEVC out.
 * Copyright (c) 2026, the yah265 authors. GPL-2.0-or-later; see LICENSE.
 */

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "yah265.h"

/* internal knobs, resolved from the library (not yet public API) */
void n265_encoder_set_dump_decode(n265_encoder *e, FILE *f);
void n265_encoder_set_dump(n265_encoder *e, FILE *f);
void n265_encoder_set_shot_stats(n265_encoder *e, const char *path);
void n265_set_shot_window(int n);
/* Two-pass (phase 12 item 4): 1 writes the stats sidecar, 2 plans from it,
 * 3 does both. The path outlives the encoder - it is argv. */
void n265_set_pass(int pass, const char *stats);
void n265_set_lossless(int on);
/* --rdoq-level <0|1|2> (x265's numbering; docs/instruments.md says how it
 * maps onto N265_RDOQ). -1 is "not given". */
void n265_set_rdoq_level(int lvl);
/* --cbqpoffs / --crqpoffs <-12..12>: pps_cb_qp_offset and
 * pps_cr_qp_offset, x265's names for the two PPS chroma QP offsets.
 * 0 / 0 is the default and is every stream before phase 13 B1. */
void n265_set_chroma_qpoffs(int cb, int cr);
/* Annex A overrides (phase 8 item 7). `idc` is general_level_idc
 * (level * 30); 0 leaves the level derived from the stream. */
void n265_set_level(int idc, int high_tier);
void n265_set_vui_timing(int on);
void n265_set_film_grain(double strength, int denoise);
/* The hypothetical decoder buffer (phase 12 item 2): kbit/s, kbit, and a
 * fraction of the buffer. 0 / 0 is the mode off. */
void n265_set_vbv(double maxrate, double bufsize, double init);
/* "4.1", "41" or "123" -> 123; 0 for a string that is not a level. Pure
 * arithmetic over the Annex A table, so the 8-bit copy serves both
 * libraries and the dispatch table does not carry it. */
int n265_level_parse(const char *s);
/* The pass-through signalling flags' STRING forms (phase 14 item 1,
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
void n265_set_chroma_qpoffs_10(int cb, int cr);
void n265_set_level_10(int idc, int high_tier);
void n265_set_vui_timing_10(int on);
void n265_set_film_grain_10(double strength, int denoise);
void n265_set_vbv_10(double maxrate, double bufsize, double init);
void n265_set_hash_10(int type);
void n265_set_vui_colour_10(int videoformat, int full_range, int colorprim,
                            int transfer, int colormatrix, int chromaloc,
                            int overscan);
void n265_set_vui_sar_10(int idc, int w, int h);
void n265_set_vui_window_10(int on, int l, int t, int r, int b);
void n265_set_hdr_meta_10(const uint32_t *md, int cll, int fall);
void n265_set_hdr10_10(int hdr10, int cll_on, int atc);
int  n265_set_preset_10(const char *name);
int  n265_preset_param_10(const char *key, int fallback);
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
    const char *(*preset_nearest)(const char *);
    void (*set_shot_window)(int);
    void (*set_pass)(int, const char *);
    void (*set_lossless)(int);
    void (*set_level)(int, int);
    void (*set_vui_timing)(int);
    void (*set_film_grain)(double, int);
    void (*set_vbv)(double, double, double);
    void (*set_rdoq_level)(int);
    void (*set_chroma_qpoffs)(int, int);
    /* the pass-through signalling setters (phase 14 item 1). They take
     * syntax elements, never names: the tables above turn the command
     * line's spellings into numbers once, before the dispatch. */
    void (*set_hash)(int);
    void (*set_vui_colour)(int, int, int, int, int, int, int);
    void (*set_vui_sar)(int, int, int);
    void (*set_vui_window)(int, int, int, int, int);
    void (*set_hdr_meta)(const uint32_t *, int, int);
    void (*set_hdr10)(int, int, int);
    n265_encoder *(*encoder_open)(const n265_param *);
    int  (*encoder_encode)(n265_encoder *, n265_nal **, int *,
                           const n265_picture *);
    void (*encoder_close)(n265_encoder *);
    const char *(*version_str)(void);
    void (*set_dump)(n265_encoder *, FILE *);
    void (*set_dump_decode)(n265_encoder *, FILE *);
    void (*set_shot_stats)(n265_encoder *, const char *);
} n265_api;

static const n265_api api8 = {
    8, n265_param_default, n265_set_mode, n265_set_la, n265_set_crf,
    n265_set_bitrate, n265_set_ref, n265_set_badapt, n265_set_threads,
    n265_set_preset, n265_preset_param, n265_preset_nearest,
    n265_set_shot_window, n265_set_pass,
    n265_set_lossless, n265_set_level, n265_set_vui_timing,
    n265_set_film_grain, n265_set_vbv, n265_set_rdoq_level,
    n265_set_chroma_qpoffs,
    n265_set_hash, n265_set_vui_colour, n265_set_vui_sar,
    n265_set_vui_window, n265_set_hdr_meta, n265_set_hdr10,
    n265_encoder_open, n265_encoder_encode, n265_encoder_close,
    n265_version_str, n265_encoder_set_dump, n265_encoder_set_dump_decode,
    n265_encoder_set_shot_stats,
};

static const n265_api api10 = {
    10, n265_param_default_10, n265_set_mode_10, n265_set_la_10,
    n265_set_crf_10, n265_set_bitrate_10, n265_set_ref_10,
    n265_set_badapt_10, n265_set_threads_10, n265_set_preset_10,
    n265_preset_param_10, n265_preset_nearest_10,
    n265_set_shot_window_10, n265_set_pass_10,
    n265_set_lossless_10, n265_set_level_10, n265_set_vui_timing_10,
    n265_set_film_grain_10, n265_set_vbv_10, n265_set_rdoq_level_10,
    n265_set_chroma_qpoffs_10,
    n265_set_hash_10, n265_set_vui_colour_10, n265_set_vui_sar_10,
    n265_set_vui_window_10, n265_set_hdr_meta_10, n265_set_hdr10_10,
    n265_encoder_open_10,
    n265_encoder_encode_10, n265_encoder_close_10, n265_version_str_10,
    n265_encoder_set_dump_10, n265_encoder_set_dump_decode_10,
    n265_encoder_set_shot_stats_10,
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
    size_t nsamp;           /* samples in one picture, all three planes */
    const void *plane[3];   /* what the library is handed, once */
    long clipped;           /* samples above (1 << depth) - 1 in the file */
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
     * The reverse is refused. There is no downshift here: dropping two bits
     * is a dither-or-truncate choice that changes the picture before the
     * encoder ever sees it, and a silent one would make a 10-bit source's
     * 8-bit numbers look like a property of the encoder. Convert on
     * purpose with ffmpeg if that is what is wanted. */
    if (y->out_depth == 0)
        y->out_depth = y->depth;
    if (y->out_depth < y->depth) {
        fprintf(stderr, "n265: --output-depth %d on %d-bit input (C%s): "
                "there is no downshift path.\n", y->out_depth, y->depth, csp);
        fprintf(stderr, "n265: dropping samples is a dither-or-truncate "
                "choice about the PICTURE, not about the encoder, so it is "
                "made outside - ffmpeg -i in.y4m -pix_fmt yuv420p out.y4m.\n");
        return -2;
    }
    size_t luma = (size_t)y->w * (size_t)y->h;
    size_t chroma = (size_t)(y->w >> y->csx) * (size_t)(y->h >> y->csy);
    size_t nsamp = luma + 2 * chroma;
    y->frame_bytes = nsamp * (y->depth > 8 ? 2 : 1);
    y->frame = malloc(y->frame_bytes);
    if (!y->frame)
        return -1;
    if (y->out_depth == 8) {
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

/* returns 1 on frame read, 0 on EOF, -1 on error */
static int y4m_read(y4m_t *y)
{
    char line[256];
    if (!fgets(line, sizeof(line), y->f))
        return 0;
    if (strncmp(line, "FRAME", 5) != 0)
        return -1;
    if (fread(y->frame, 1, y->frame_bytes, y->f) != y->frame_bytes)
        return -1;
    if (y->out_depth == 8)
        return 1;               /* 8 in, 8 out: the file IS the picture */
    size_t nsamp = y->nsamp;
    if (y->depth == 8) {
        /* The --output-depth 10 upshift: a PURE LEFT SHIFT BY TWO, the
         * same arithmetic scripts/upshift10.sh gets out of ffmpeg's
         * format filter (sample * 4, not sample * 1023/255). It is what a
         * Main10 stream of SDR content is, and it is what makes the
         * Main10 encode of an 8-bit clip comparable with its 8-bit encode
         * at the same QP: the residual is exactly four times, the forward
         * transform sheds exactly two bits, and the coefficients that
         * reach the quantiser are the same integers. It costs one pass
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

static void usage(void)
{
    fprintf(stderr,
        "usage: n265 [options] <input.y4m|->\n"
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
        "  --preset <name>  speed/quality point, in x265's names.\n"
        "                  Rungs, fastest first: fast, medium (default),\n"
        "                  slow, slower. x265's other six names are\n"
        "                  known and refused, naming the nearest rung\n"
        "                  we have; `faster` and `veryfast` were built,\n"
        "                  measured against the x265 preset of the same\n"
        "                  name and refused (docs/phase13.md, C2).\n"
        "                  (default medium, the point the project's goals are\n"
        "                  read at: at or ahead of x265 medium on every\n"
        "                  corpus clip, in less time).\n"
        "                  Sets the DEFAULTS of the tuning knobs; any flag or\n"
        "                  N265_* variable you give yourself still wins.\n"
        "  --qp <n>        fixed QP (default 26). Range 0..51 at 8 bits,\n"
        "                  -12..51 at Main10 (QpBdOffsetY, 7.4.3.3)\n"
        "  --crf <f>       constant rate factor (base QP from f; overrides\n"
        "                  --qp). Same range as --qp, negative included at\n"
        "                  Main10: giving the flag at all is what turns CRF\n"
        "                  on, so --crf -6 is a rate factor and not an off\n"
        "                  switch\n"
        "  --bitrate <k>   single-pass ABR at k kbit/s (overrides --crf)\n"
        "  --ref <n>       references per list, 1..3 (default 1)\n"
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
        "  --cbqpoffs <n>  pps_cb_qp_offset, -12..12, default 0\n"
        "  --crqpoffs <n>  pps_cr_qp_offset, -12..12, default 0\n"
        "                  The chroma quantiser's offset from luma's, added\n"
        "                  into qPi before Table 8-10 (8.6.1). NEGATIVE\n"
        "                  spends more on chroma; positive moves those bits\n"
        "                  to luma. Both stay 0 at every preset: measured at\n"
        "                  the pin, our chroma-to-luma split is within half a\n"
        "                  QP step of x265's on all fifteen corpus clips, so\n"
        "                  there is nothing for a default to correct\n"
        "                  (docs/phase13.md).\n"
        "  --pcm           phase-0 I_PCM mode (lossless, huge)\n"
        "  --lossless      mathematically lossless: transquant bypass on\n"
        "                  every CU, so the reconstruction IS the source.\n"
        "                  Turns off rate control, RDOQ, sign-bit hiding,\n"
        "                  AQ, cu-tree, psy-rd and both loop filters (all\n"
        "                  of them price against a quantiser there is not).\n"
        "                  --qp/--crf/--bitrate are ignored; prediction and\n"
        "                  mode decision still run, on bits alone.\n"
        "  --ctu <16|32|64>  coding tree block size (default 64)\n"
        "  --keyint <n>    IDR period in frames (default 250)\n"
        "  --bframes <0..15> B frames between anchors, hierarchical (default 7).\n"
        "                  Above 7 the window has to hold the mini-GOP, so\n"
        "                  --rc-lookahead must be at least --bframes + 1.\n"
        "  --aq <0|1>      variance adaptive quantization (default 1)\n"
        "  --rc-lookahead <1..32>  lookahead depth in frames (default 20,\n"
        "                  x265 medium's own; 12 before 2026-09-12).\n"
        "                  Latency is the lookahead depth in frames. The deep\n"
        "                  window (--rc-lookahead 24 --bframes 15) is an\n"
        "                  opt-in mode: 24 frames of latency and a deeper\n"
        "                  DPB, for static or drawn content\n"
        "                  (docs/phase7-gop-design.md).\n"
        "  --cutree <0|1>  lookahead propagation QP offsets (default 1)\n"
        "  --threads <n>   worker threads; 0 = one per online CPU (default 0).\n"
        "                  Output is byte-identical at every value.\n"
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
        "  NOTE the ENCODER-SIDE half of x265's HDR options is deliberately\n"
        "  absent: --hdr10-opt is a block-level QP decision tuned to PQ\n"
        "  content, and this item signals without deciding.\n"
        "  --verbose, -v   one line at open with the level and tier chosen\n"
        "                  and every Annex A bound that drove them\n"
        "                  (N265_LEVEL_TRACE=1 is the same line)\n"
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
    double film_grain = 0.0;    /* --film-grain: 0 = off, and off is the default */
    int fgc_denoise = 0;        /* --film-grain-denoise */
    double crf = -1.0, bitrate = 0.0;
    /* --vbv-maxrate / --vbv-bufsize / --vbv-init; 0/0 is the mode off,
     * which is every encode before phase 12 item 2. */
    double vbv_maxrate = 0.0, vbv_bufsize = 0.0, vbv_init = 0.9;
    /* Pass-through signalling (phase 14 item 1). -1 is "the flag was not
     * given" in every field that has an absence; give none of them and
     * nothing below reaches the stream. */
    int hash = 0;
    int sig_videoformat = -1, sig_range = -1, sig_overscan = -1;
    int sig_colorprim = -1, sig_transfer = -1, sig_colormatrix = -1;
    int sig_chromaloc = -1;
    int sar_idc = -1, sar_w = 0, sar_h = 0;
    int win_on = 0, win_l = 0, win_t = 0, win_r = 0, win_b = 0;
    uint32_t md[10];
    int have_md = 0, maxcll = -1, maxfall = -1;
    int hdr10 = -1, cll_on = 1, atc = -1;
    int had_crf = 0;            /* --crf given: the ON switch, not crf >= 0 */
    int out_depth = 0;          /* --output-depth; 0 = the input's */
    int nref = 1, badapt = 1;
    int lookahead = 20, cutree = 1;   /* 20 since 2026-09-12 (x265 medium's
                                     * own depth): the last window that
                                     * clears every gate rule, docs/
                                     * phase7-gop-design.md step 4 */   /* on by default since 2026-09-12: every
                                     * measured medium passes both, and the
                                     * bare CLI used to hand out the tools-only
                                     * encoder (docs/instruments.md, board) */
    int threads = 0;
    int shot_window = 0;        /* --shot-window; 0 = off, the default */
    /* --rdoq-level: -1 is "not given", which leaves the preset (and the
     * environment) in charge - the flag is an override, not a default. */
    int rdoq_level = -1, had_rdoq = 0;
    /* --cbqpoffs / --crqpoffs: 0 is both the default and "not given",
     * and they are the same thing here - the offsets have no preset
     * entry to defer to, so there is nothing for an unset flag to mean
     * except zero. */
    int cbqpoffs = 0, crqpoffs = 0;
    const char *dump_decode_path = NULL;
    const char *preset = NULL;
    /* which structural flags the user gave: a preset supplies the rest */
    int had_ref = 0, had_badapt = 0, had_bframes = 0, had_la = 0, had_ctu = 0;

    for (int i = 1; i < argc; i++) {
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
        } else if (!strcmp(argv[i], "--bframes") && i + 1 < argc) {
            bframes = atoi(argv[++i]); had_bframes = 1;
        } else if (!strcmp(argv[i], "--aq") && i + 1 < argc) {
            aq = atoi(argv[++i]);
        } else if (!strcmp(argv[i], "--rc-lookahead") && i + 1 < argc) {
            lookahead = atoi(argv[++i]); had_la = 1;
        } else if (!strcmp(argv[i], "--cutree") && i + 1 < argc) {
            cutree = atoi(argv[++i]);
        } else if (!strcmp(argv[i], "--dump-recon-decode") && i + 1 < argc) {
            dump_decode_path = argv[++i];
        } else if (!strcmp(argv[i], "--threads") && i + 1 < argc) {
            threads = atoi(argv[++i]);
        } else if (!strcmp(argv[i], "--ctu") && i + 1 < argc) {
            ctu = atoi(argv[++i]); had_ctu = 1;
        } else if (!strcmp(argv[i], "--rdoq-level") && i + 1 < argc) {
            rdoq_level = atoi(argv[++i]); had_rdoq = 1;
        } else if (!strcmp(argv[i], "--cbqpoffs") && i + 1 < argc) {
            cbqpoffs = atoi(argv[++i]);
        } else if (!strcmp(argv[i], "--crqpoffs") && i + 1 < argc) {
            crqpoffs = atoi(argv[++i]);
        } else if (!strcmp(argv[i], "--pcm")) {
            pcm = 1;
        } else if (!strcmp(argv[i], "--lossless")) {
            lossless = 1;
        } else if (!strcmp(argv[i], "--level") && i + 1 < argc) {
            level_str = argv[++i];
        } else if (!strcmp(argv[i], "--high-tier")) {
            high_tier = 1;
        } else if (!strcmp(argv[i], "--verbose") || !strcmp(argv[i], "-v")) {
            verbose = 1;
        } else if (!strcmp(argv[i], "--vui-timing")) {
            vui_timing = 1;
        } else if (!strcmp(argv[i], "--no-vui-timing")) {
            vui_timing = 0;
        } else if (!strcmp(argv[i], "--film-grain") && i + 1 < argc) {
            film_grain = atof(argv[++i]);
        } else if (!strcmp(argv[i], "--film-grain-denoise") && i + 1 < argc) {
            fgc_denoise = atoi(argv[++i]);
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
    y4m_t y;
    y.out_depth = out_depth;
    int yr = y4m_open(&y, fin);
    if (yr < 0) {
        if (yr == -1)
            fprintf(stderr, "n265: bad y4m input\n");
        return 1;
    }

    /* The dispatch, and it is the whole of it: one table per library,
     * selected by the width the pictures will carry. Everything after
     * this point calls through `a` and does not know which encoder it
     * is configuring. */
    const n265_api *a = y.out_depth == 10 ? &api10 : &api8;
    const int qp_bd_off = N265_QP_BD_OFF(a->depth);

    /* QpY runs -QpBdOffsetY..51 (7.4.3.3): 0..51 at 8 bits, -12..51 at
     * Main10, where the twelve extra steps are the finer quantisers the
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
    if (!had_ref)     nref = a->preset_param("ref", nref);
    if (!had_bframes) bframes = a->preset_param("bframes", bframes);
    if (!had_badapt)  badapt = a->preset_param("b-adapt", badapt);
    if (!had_la)      lookahead = a->preset_param("rc-lookahead", lookahead);
    if (!had_ctu)     ctu = a->preset_param("ctu", ctu);
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
    if (lookahead < 1 || lookahead > 32) {
        fprintf(stderr, "n265: --rc-lookahead out of range 1..32\n");
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
                "--rc-lookahead must be at least %d\n",
                lookahead, bframes, bframes + 1);
        return 2;
    }
    int ctb_log2 = ctu == 16 ? 4 : ctu == 32 ? 5 : 6;
    a->set_mode(pcm ? 1 : 0, qp, ctb_log2, keyint, bframes, aq);
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
    a->set_chroma_qpoffs(cbqpoffs, crqpoffs);
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
    a->set_vbv(vbv_maxrate, vbv_bufsize, vbv_init);
    /* ---- the pass-through signalling (phase 14 item 1) ---- */
    if (hash < 0 || hash > 3) {
        fprintf(stderr, "n265: --hash is 0 (off), 1 (MD5), 2 (CRC) or "
                "3 (checksum)\n");
        return 2;
    }
    a->set_hash(hash);
    /* The default display window is signalled in CHROMA units (7.4.3.2.1)
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
    a->set_vui_colour(sig_videoformat, sig_range, sig_colorprim,
                      sig_transfer, sig_colormatrix, sig_chromaloc,
                      sig_overscan);
    a->set_vui_sar(sar_idc, sar_w, sar_h);
    a->set_vui_window(win_on, win_l, win_t, win_r, win_b);
    a->set_hdr_meta(have_md ? md : NULL, maxcll, maxfall);
    a->set_hdr10(hdr10, cll_on, atc);
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
    n265_encoder *enc = a->encoder_open(&p);
    if (!enc) {
        fprintf(stderr, "n265: encoder_open failed (dims %dx%d)\n", y.w, y.h);
        return 1;
    }

    if (shot_path)
        a->set_shot_stats(enc, shot_path);
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
    while ((max_frames < 0 || n < max_frames) && (r = y4m_read(&y)) > 0) {
        n265_picture pic = {
            .plane = { y.plane[0], y.plane[1], y.plane[2] },
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
        for (int i = 0; i < nnal; i++)
            fwrite(nal[i].payload, 1, (size_t)nal[i].size, fout);
        n++;
    }
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
    a->encoder_close(enc);
    if (fdump)
        fclose(fdump);
    if (fdump2)
        fclose(fdump2);
    free(y.pix16);
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
                "(upshifted by 2)\n", n, y.w, y.h, fmt, y.depth,
                y.out_depth);
    else
        fprintf(stderr, "n265: %ld frames, %dx%d %s %d-bit\n", n, y.w, y.h,
                fmt, y.depth);
    return 0;
}
