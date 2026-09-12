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

/* QpBdOffsetY (7-5). The CLI does not see src/common/pixel.h, so the one
 * derivation it needs is repeated here off the public N265_BIT_DEPTH. */
#define N265_QP_BD_OFF  (6 * (N265_BIT_DEPTH - 8))
#define N265_SAMPLE_MAX ((1 << N265_BIT_DEPTH) - 1)

typedef struct y4m_t {
    FILE *f;
    int w, h, fps_num, fps_den;
    int depth;              /* sample width the FILE carries: 8 or 10 */
    size_t frame_bytes;     /* the y4m payload, as it sits on disk */
    uint8_t *frame;
    /* What the library is handed. At 8 bits it IS `frame` - no copy, no
     * second allocation, nothing in the read path - and at 10 it is a
     * separate plane the reader assembles the little-endian pairs into. */
    n265_pixel *pix;
    long clipped;           /* samples above (1 << depth) - 1 in the file */
} y4m_t;

/* The C tag's depth, or -1 for a chroma sampling or sample width this
 * encoder does not do. 4:2:0 only, and within it 8 bits (the plain tag
 * and the three siting variants, which differ in chroma POSITION and not
 * in width) or 10 - `C420p10`, which is what ffmpeg writes for
 * yuv420p10le; `C420p10le` is taken as the same thing, since some writers
 * spell it that way. 420p12/p16 and 4:2:2 / 4:4:4 are later items, and
 * they are REFUSED rather than read as 8-bit, which is what the old
 * `strncmp(csp, "420", 3)` would have done with a C420p10 file. */
static int y4m_csp_depth(const char *csp)
{
    if (!strcmp(csp, "420") || !strcmp(csp, "420jpeg") ||
        !strcmp(csp, "420mpeg2") || !strcmp(csp, "420paldv"))
        return 8;
    if (!strcmp(csp, "420p10") || !strcmp(csp, "420p10le"))
        return 10;
    return -1;
}

static int y4m_open(y4m_t *y, FILE *f)
{
    char line[1024];
    memset(y, 0, sizeof(*y));
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
    y->depth = y4m_csp_depth(csp);
    if (y->depth < 0) {
        fprintf(stderr, "n265: unsupported input format C%s "
                "(4:2:0 at 8 or 10 bits only)\n", csp);
        return -1;
    }
    /* One library per sample width (docs/instruments.md, "Builds and the
     * bit depth"), so the depth has to MATCH. The 8-bit encoder cannot
     * read 10-bit samples, and the Main10 encoder is no longer handed
     * 8-bit ones silently upshifted the way item 1's smoke needed:
     * that would let a 10-bit measurement be taken on SDR content nobody
     * asked to convert, and it would hide the conversion from the record.
     * Converting is one ffmpeg away; choosing the library from the input,
     * and --output-depth, are phase 8 item 5's. */
    if (y->depth != N265_BIT_DEPTH) {
        fprintf(stderr, "n265: this is the %d-bit encoder and the input is "
                "%d-bit (C%s).\n", N265_BIT_DEPTH, y->depth, csp);
        if (N265_BIT_DEPTH == 8)
            fprintf(stderr, "n265: encode it with n265-10, the Main10 "
                    "binary.\n");
        else
            fprintf(stderr, "n265: convert it first - scripts/upshift10.sh "
                    "in.y4m out.y4m, or ffmpeg -i in.y4m -pix_fmt "
                    "yuv420p10le -strict -1 out.y4m.\n");
        return -1;
    }
    y->frame_bytes = (size_t)y->w * y->h * 3 / 2 * (y->depth > 8 ? 2 : 1);
    y->frame = malloc(y->frame_bytes);
    if (!y->frame)
        return -1;
#if N265_BIT_DEPTH == 8
    y->pix = y->frame;
#else
    y->pix = malloc((size_t)y->w * y->h * 3 / 2 * sizeof(*y->pix));
    if (!y->pix)
        return -1;
#endif
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
#if N265_BIT_DEPTH != 8
    /* 16-bit little-endian samples, assembled by hand rather than cast:
     * the byte order is the FILE's and not the host's, and the payload is
     * not guaranteed to be aligned for a uint16_t load. A sample above
     * the declared depth is out of spec - the encoder clips its own
     * reconstruction to PIXEL_MAX, so a source value it cannot represent
     * would read as a permanent residual and as a recon mismatch against
     * every decoder - so it is clamped here and counted, and the count is
     * reported once at the end rather than per sample. */
    size_t nsamp = (size_t)y->w * y->h * 3 / 2;
    for (size_t i = 0; i < nsamp; i++) {
        unsigned v = (unsigned)y->frame[2 * i] |
                     ((unsigned)y->frame[2 * i + 1] << 8);
        if (v > N265_SAMPLE_MAX) {
            v = N265_SAMPLE_MAX;
            y->clipped++;
        }
        y->pix[i] = (n265_pixel)v;
    }
#endif
    return 1;
}

static void usage(void)
{
    fprintf(stderr,
        "usage: n265 [options] <input.y4m|->\n"
        "  -o <file>       output Annex-B bitstream (default: out.hevc)\n"
        "  --frames <n>    encode at most n frames\n"
        "  --preset <fast|medium|slow>  speed/quality point (default medium).\n"
        "                  Sets the DEFAULTS of the tuning knobs; any flag or\n"
        "                  N265_* variable you give yourself still wins.\n"
        "  --qp <n>        fixed QP (default 26). Range 0..51 at 8 bits,\n"
        "                  -12..51 at Main10 (QpBdOffsetY, 7.4.3.3)\n"
        "  --crf <f>       constant rate factor (base QP from f; overrides --qp)\n"
        "  --bitrate <k>   single-pass ABR at k kbit/s (overrides --crf)\n"
        "  --ref <n>       references per list, 1..3 (default 1)\n"
        "  --b-adapt <0-4> adaptive B run length from the lookahead: 0 fixed,\n"
        "                  1 the motion rule (default), 2/3 refused priced arms,\n"
        "                  4 the bits model (docs/phase7-gop-design.md)\n"
        "  (RDOQ is on; N265_RDOQ=0 turns it off, N265_PSY=F adds the psy term)\n"
        "  --pcm           phase-0 I_PCM mode (lossless, huge)\n"
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
        "  --version       print version\n");
}

int main(int argc, char **argv)
{
    const char *in_path = NULL, *out_path = "out.hevc";
    const char *dump_path = NULL, *shot_path = NULL;
    long max_frames = -1;
    int qp = 26, pcm = 0, ctu = 64, keyint = 250, bframes = 7, aq = 1;
    double crf = -1.0, bitrate = 0.0;
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
            crf = atof(argv[++i]);
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
        } else if (!strcmp(argv[i], "--pcm")) {
            pcm = 1;
        } else if (!strcmp(argv[i], "--dump-recon") && i + 1 < argc) {
            dump_path = argv[++i];
        } else if (!strcmp(argv[i], "--shot-window") && i + 1 < argc) {
            shot_window = atoi(argv[++i]);
        } else if (!strcmp(argv[i], "--shot-stats") && i + 1 < argc) {
            shot_path = argv[++i];
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

    FILE *fin = strcmp(in_path, "-") ? fopen(in_path, "rb") : stdin;
    if (!fin) {
        fprintf(stderr, "n265: cannot open %s: %s\n", in_path, strerror(errno));
        return 1;
    }
    y4m_t y;
    if (y4m_open(&y, fin) < 0) {
        fprintf(stderr, "n265: bad y4m input\n");
        return 1;
    }

    /* QpY runs -QpBdOffsetY..51 (7.4.3.3): 0..51 at 8 bits, -12..51 at
     * Main10, where the twelve extra steps are the finer quantisers the
     * two extra sample bits pay for and are the whole point of the
     * profile at the top of the ladder. */
    if (qp < -N265_QP_BD_OFF || qp > 51) {
        fprintf(stderr, "n265: qp out of range %d..51\n", -N265_QP_BD_OFF);
        return 2;
    }
    /* The preset is a default source and nothing else: it is selected
     * before anything reads a knob, and every value it offers here is
     * taken only where the command line was silent. */
    if (preset && n265_set_preset(preset) < 0) {
        fprintf(stderr, "n265: --preset must be fast, medium or slow\n");
        return 2;
    }
    if (!had_ref)     nref = n265_preset_param("ref", nref);
    if (!had_bframes) bframes = n265_preset_param("bframes", bframes);
    if (!had_badapt)  badapt = n265_preset_param("b-adapt", badapt);
    if (!had_la)      lookahead = n265_preset_param("rc-lookahead", lookahead);
    if (!had_ctu)     ctu = n265_preset_param("ctu", ctu);
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
    n265_set_mode(pcm ? 1 : 0, qp, ctb_log2, keyint, bframes, aq);
    if (crf >= 0) n265_set_crf(crf);
    if (bitrate > 0) n265_set_bitrate(bitrate);
    n265_set_ref(nref);
    n265_set_badapt(badapt);
    n265_set_la(lookahead, cutree);
    n265_set_shot_window(shot_window);
    n265_set_threads(threads);
    n265_param p;
    n265_param_default(&p);
    p.width = y.w;
    p.height = y.h;
    p.fps_num = y.fps_num;
    p.fps_den = y.fps_den;
    n265_encoder *enc = n265_encoder_open(&p);
    if (!enc) {
        fprintf(stderr, "n265: encoder_open failed (dims %dx%d)\n", y.w, y.h);
        return 1;
    }

    if (shot_path)
        n265_encoder_set_shot_stats(enc, shot_path);
    FILE *fdump = NULL;
    if (dump_path) {
        fdump = fopen(dump_path, "wb");
        if (!fdump) {
            fprintf(stderr, "n265: cannot open %s: %s\n", dump_path,
                    strerror(errno));
            return 1;
        }
        n265_encoder_set_dump(enc, fdump);
    }
    FILE *fdump2 = NULL;
    if (dump_decode_path) {
        fdump2 = fopen(dump_decode_path, "wb");
        if (fdump2)
            n265_encoder_set_dump_decode(enc, fdump2);
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
            .plane = { y.pix,
                       y.pix + (size_t)y.w * y.h,
                       y.pix + (size_t)y.w * y.h * 5 / 4 },
            .stride = { y.w, y.w / 2, y.w / 2 },
            .pts = n,
        };
        n265_nal *nal;
        int nnal;
        int bytes = n265_encoder_encode(enc, &nal, &nnal, &pic);
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
        int r2 = n265_encoder_encode(enc, &nal, &nnal, NULL);
        if (r2 <= 0 && nnal == 0)
            break;
        for (int i = 0; i < nnal; i++)
            fwrite(nal[i].payload, 1, (size_t)nal[i].size, fout);
    }
    n265_encoder_close(enc);
    if (fdump)
        fclose(fdump);
    if (fdump2)
        fclose(fdump2);
#if N265_BIT_DEPTH != 8
    free(y.pix);
#endif
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
    fprintf(stderr, "n265: %ld frames, %dx%d %d-bit\n", n, y.w, y.h, y.depth);
    return 0;
}
