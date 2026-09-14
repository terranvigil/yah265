/*
 * Encoder core. Two modes: N265_MODE_INTRA is the real path (DC intra,
 * CTB 32, TU == CU, fixed QP); N265_MODE_PCM is the phase-0 I_PCM path
 * kept for harness A/B. Every frame is an IDR either way.
 * Copyright (c) 2026, the yah265 authors. GPL-2.0-or-later; see LICENSE.
 */

#include <limits.h>
#include <math.h>
#include <stdio.h>
#include <time.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>

#include "common/cpu.h"
#include "encoder/deblock.h"
#include "encoder/encoder.h"
#include "encoder/est.h"
#include "encoder/la.h"
#include "encoder/mc.h"
#include "encoder/rc.h"
#include "encoder/satd.h"
#include "encoder/sao.h"
#include "encoder/preset.h"

/* N265_LA_PROF: the lookahead's SERIAL FRACTION, which is what T3/2 has
 * to move. Wall time the DRIVER spends inside each lookahead stage,
 * against the wall time of the whole encode. Driver-side only, so the
 * clock reads are a handful per frame and never inside a worker. */
static double prof_now(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (double)t.tv_sec + 1e-9 * (double)t.tv_nsec;
}

const char *n265_version_str(void)
{
    return "0.0.1";
}

void n265_param_default(n265_param *p)
{
    memset(p, 0, sizeof(*p));
    p->csp = N265_CSP_I420;
    p->fps_num = 25;
    p->fps_den = 1;
}

/* internal knobs the public API doesn't expose yet */
static int g_mode = N265_MODE_INTRA;
static int g_qp = 26;
static double g_crf = -1.0;      /* n265_set_crf's rate factor (a QpY) */
/* Whether n265_set_crf was called at all. It used to be `g_crf >= 0`, which
 * is why --crf could not reach the twelve QP steps below zero that Main10
 * pays for: the off sentinel and a legal operating point were the same
 * number (phase 8 item 5a). The rate factor is a QpY, QpY runs from
 * -QpBdOffsetY, and plan_qp clamps it there like any other. */
static int g_crf_on = 0;
static double g_bitrate = 0.0;   /* n265_set_bitrate, kbit/s; 0 = off */
static int g_ctb_log2 = 6;
static int g_keyint = 250;      /* x265 4.3's own default (2026-09-08) */

/* The full hierarchical run. 3 shipped from 2026-09-07 until the preset
 * ladder priced 7 inside both the fast and the slow chains and found it
 * 2.2 BD points better for nothing on the clock; the corpus read that
 * made it the default is the 2026-09-10 entry in
 * docs/programme-from-yah264.md. `--bframes 3` restores the old encoder
 * byte for byte. */
static int g_bframes = 7;         /* hierarchical B default (2026-09-10) */
static int g_ref = 1;            /* n265_set_ref */
static int g_badapt = 1;         /* adaptive B run length, default on */
static int g_aq = 0;
static int g_la_depth = 12;
static int g_cutree = 0;
static int g_shot_window = 0;   /* --shot-window; 0 = off */
/* --pass / --stats (phase 12 item 4, twopass.h). 0 is every encode that
 * does not ask for a second pass, which is every encode before this item. */
static int g_pass = 0;
static const char *g_stats_path = NULL;
static int g_threads = 0;       /* --threads; 0 = auto */
static int g_lossless = 0;      /* --lossless; n265_set_lossless */
/* --rdoq-level; -1 is "the flag was not given", which is every encode
 * before this item and every one that leaves the choice to the preset. */
static int g_rdoq_level = -1;
/* Annex A overrides (phase 8 item 7). 0 / 0 / 0 is "derive the level and
 * the tier from the stream and write no VUI", which is what every board,
 * band and gate runs. */
static int g_level_idc = 0;     /* --level, as general_level_idc            */
static int g_high_tier = 0;     /* --high-tier                              */
static int g_vui_timing = 1;    /* vui_timing_info in the SPS; on since
                                 * 2026-09-12 (owner), --no-vui-timing off */
/* --film-grain / --film-grain-denoise (phase 12 item 3). 0 / 0 is off, is
 * the default, is every preset and is every board and band leg: the
 * owner's standing rule is that an analysis mode is never a default, and
 * film grain signalling is one - it changes what a DECODER puts on the
 * screen, and it is only ever a win where the viewer wanted the grain
 * back. At 0 nothing here allocates, reads a picture or writes a byte. */
static double g_film_grain = 0.0;
static int g_fgc_denoise = 0;
/* The hypothetical decoder buffer (phase 12 item 2). 0 / 0 is off and is
 * every encode the board, the band and every gate before this item ran;
 * naming both halves arms the clamp, the level's MaxCPB leg and the HRD in
 * the VUI at once. */
static double g_vbv_maxrate = 0.0;  /* --vbv-maxrate, kbit/s */
static double g_vbv_bufsize = 0.0;  /* --vbv-bufsize, kbit   */
static double g_vbv_init = 0.9;     /* --vbv-init, a fraction of the buffer */
/* Pass-through signalling (phase 14 item 1): the decoded picture hash, the
 * VUI's colour description and the HDR10 static metadata. The whole struct
 * is OFF here - every field -1 or 0 - and off is what every board, band and
 * gate leg runs, which is what makes "no flag, no byte moves" the item's
 * identity claim. Nothing in it reaches a coding decision; see signal.h. */
static n265_signal g_sig = {
    .hash = 0, .sar_idc = -1, .sar_w = 0, .sar_h = 0, .overscan = -1,
    .videoformat = -1, .full_range = -1, .colorprim = -1, .transfer = -1,
    .colormatrix = -1, .chromaloc = -1, .win_on = 0,
    .have_md = 0, .have_cll = 0, .hdr10 = -1, .cll_on = 1, .atc = -1,
};

void n265_set_threads(int n)
{
    g_threads = n < 0 ? 0 : n > 256 ? 256 : n;
}

/* auto: one worker per online CPU */
static int auto_threads(void)
{
    long n = sysconf(_SC_NPROCESSORS_ONLN);
    return n > 0 ? (int)n : 1;
}

void n265_set_la(int depth, int cutree);
void n265_set_la(int depth, int cutree)
{
    /* The clamp is LA_DEPTH_MAX, not the ring size: T3/1 grew the ring by
     * the free run K needs, and the reachable lookahead must not move
     * with it (that would change bits). */
    g_la_depth = depth < 1 ? 1 : depth > LA_DEPTH_MAX ? LA_DEPTH_MAX : depth;
    g_cutree = cutree ? 1 : 0;
}

/* --shot-window N: the opt-in shot planner (phase 7 item 6 step 2).
 * 0 is off and is the default, the board and every preset; N buys the
 * lookahead queue N frames so the planner can see a whole shot, at N
 * frames of latency and N slots of memory. The clamp is the same
 * N265_MAX_SHOT_WINDOW the CLI names. */
void n265_set_shot_window(int n);
void n265_set_shot_window(int n)
{
    g_shot_window = n < 0 ? 0 : n > N265_MAX_SHOT_WINDOW
                                    ? N265_MAX_SHOT_WINDOW : n;
}

void n265_set_mode(int mode, int qp, int ctb_log2, int keyint, int bframes,
                   int aq);
void n265_set_crf(double crf) { g_crf = crf; g_crf_on = 1; }
/* --lossless. Declared in cli/n265.c the way the other CLI-only entry
 * points are: include/yah265.h is another item's file this round. */
void n265_set_lossless(int on);
void n265_set_lossless(int on) { g_lossless = on != 0; }
/* --rdoq-level <0|1|2>: the coefficient quantiser, in X265'S numbering,
 * because that is the number a user arrives with. Ours is `N265_RDOQ` and
 * it counts the same three encoders in a different order (the order they
 * were BUILT in: no lattice, the lattice everywhere, the lattice at the
 * commit). The map is here and nowhere else:
 *
 *   --rdoq-level 0   off, plain quantisation everywhere   N265_RDOQ=0
 *   --rdoq-level 1   the lattice on the commit path only  N265_RDOQ=2
 *   --rdoq-level 2   the lattice in every RD trial too    N265_RDOQ=1
 *
 * -1 is "not given", which is every encode that does not name the flag
 * and is what leaves the environment and the preset table in charge. */
void n265_set_rdoq_level(int lvl);
void n265_set_rdoq_level(int lvl) { g_rdoq_level = lvl; }

static int rdoq_level_map(int lvl)
{
    return lvl == 0 ? 0 : lvl == 1 ? 2 : 1;
}
/* --level / --high-tier / --vui-timing, declared the same way. `idc` is
 * general_level_idc (level * 30); 0 leaves the level derived. */
void n265_set_level(int idc, int high_tier);
void n265_set_level(int idc, int high_tier)
{
    g_level_idc = idc;
    g_high_tier = high_tier != 0;
}
void n265_set_vui_timing(int on);
void n265_set_vui_timing(int on) { g_vui_timing = on != 0; }
/* --film-grain <strength>: 0 off, 1.0 the estimate as measured, anything
 * else a multiplier on the estimated grain std. --film-grain-denoise
 * additionally cores the grain out of the picture the encoder codes, which
 * is the half that saves bits; the SEI alone only tells a decoder to add
 * grain to a picture that already has some. */
void n265_set_film_grain(double strength, int denoise);
void n265_set_film_grain(double strength, int denoise)
{
    g_film_grain = strength > 0.0 ? (strength > 8.0 ? 8.0 : strength) : 0.0;
    g_fgc_denoise = denoise != 0;
}

/* --vbv-maxrate / --vbv-bufsize / --vbv-init. `maxrate` is kbit/s and
 * `bufsize` kbit, the units x265 names them in; `init` is a fraction of the
 * buffer and is the only one with a default, because a buffer that starts
 * full is a claim about a decoder nobody made. Validation is at OPEN, where
 * the frame rate and the level are known and a refusal can name them. */
void n265_set_vbv(double maxrate, double bufsize, double init);
void n265_set_vbv(double maxrate, double bufsize, double init)
{
    g_vbv_maxrate = maxrate;
    g_vbv_bufsize = bufsize;
    g_vbv_init = init;
}
/* ---- the pass-through signalling setters (phase 14 item 1) ----
 *
 * Six of them, one per group of flags, and every argument is already the
 * SYNTAX ELEMENT: the CLI owns the string forms (signal.c's tables) and
 * the library never sees a name. -1 means "not given" in every field that
 * has an absence, which is what the SPS writer turns into a presence flag
 * of 0. None of these reaches a coding decision; see signal.h. */

/* --hash, in x265's numbering: 0 off, 1 MD5, 2 CRC, 3 checksum. */
void n265_set_hash(int type);
void n265_set_hash(int type)
{
    g_sig.hash = type >= 0 && type <= 3 ? type : 0;
}

/* the VUI's colour block: video_format, video_full_range_flag, the three
 * colour description fields, chroma_sample_loc_type and the overscan flag
 * (-1 for every one that was not given). */
void n265_set_vui_colour(int videoformat, int full_range, int colorprim,
                         int transfer, int colormatrix, int chromaloc,
                         int overscan);
void n265_set_vui_colour(int videoformat, int full_range, int colorprim,
                         int transfer, int colormatrix, int chromaloc,
                         int overscan)
{
    g_sig.videoformat = videoformat;
    g_sig.full_range = full_range;
    g_sig.colorprim = colorprim;
    g_sig.transfer = transfer;
    g_sig.colormatrix = colormatrix;
    g_sig.chromaloc = chromaloc;
    g_sig.overscan = overscan;
}

/* aspect_ratio_idc, and the explicit pair when it is 255 (EXTENDED_SAR). */
void n265_set_vui_sar(int idc, int w, int h);
void n265_set_vui_sar(int idc, int w, int h)
{
    g_sig.sar_idc = idc;
    g_sig.sar_w = w;
    g_sig.sar_h = h;
}

/* the default display window, in LUMA samples (the writer divides). */
void n265_set_vui_window(int on, int l, int t, int r, int b);
void n265_set_vui_window(int on, int l, int t, int r, int b)
{
    g_sig.win_on = on != 0;
    g_sig.win_l = l;
    g_sig.win_t = t;
    g_sig.win_r = r;
    g_sig.win_b = b;
}

/* the HDR10 static metadata. `md` is ten values in x265's own order -
 * G(x,y) B(x,y) R(x,y) WP(x,y) L(max,min) - or NULL for no mastering
 * display; cll < 0 is no content light level message. */
void n265_set_hdr_meta(const uint32_t *md, int cll, int fall);
void n265_set_hdr_meta(const uint32_t *md, int cll, int fall)
{
    g_sig.have_md = md != NULL;
    if (md) {
        for (int c = 0; c < 3; c++) {
            g_sig.md_prim[c][0] = (uint16_t)md[2 * c];
            g_sig.md_prim[c][1] = (uint16_t)md[2 * c + 1];
        }
        g_sig.md_wp[0] = (uint16_t)md[6];
        g_sig.md_wp[1] = (uint16_t)md[7];
        g_sig.md_max = md[8];
        g_sig.md_min = md[9];
    }
    g_sig.have_cll = cll >= 0;
    g_sig.maxcll = cll >= 0 ? cll : 0;
    g_sig.maxfall = fall >= 0 ? fall : 0;
}

/* --hdr10 / --no-hdr10 (-1 leaves the auto rule), --cll / --no-cll, and
 * --atc-sei (-1 for no message). */
void n265_set_hdr10(int hdr10, int cll_on, int atc);
void n265_set_hdr10(int hdr10, int cll_on, int atc)
{
    g_sig.hdr10 = hdr10;
    g_sig.cll_on = cll_on != 0;
    g_sig.atc = atc >= 0 && atc <= 255 ? atc : -1;
}

void n265_set_bitrate(double kbps) { g_bitrate = kbps > 0.0 ? kbps : 0.0; }
/* Two-pass: 1 writes the stats sidecar, 2 plans from it, 3 does both (the
 * plan from the file it is about to replace). The path is the caller's and
 * outlives the encoder, which is argv for the CLI. */
void n265_set_pass(int pass, const char *stats);
void n265_set_pass(int pass, const char *stats)
{
    g_pass = pass < 0 ? 0 : pass > 3 ? 3 : pass;
    g_stats_path = stats;
}
void n265_set_ref(int n) { g_ref = n < 1 ? 1 : n > N265_MAX_REF ? N265_MAX_REF : n; }
/* 0 off (fixed cadence), 1 the motion rule (the default), 2 the priced
 * mini-GOP, 3 the degradation ratio, 4 the priced mini-GOP in BITS.
 * 2, 3 and 4 are refused arms kept for re-pricing (docs/ideas.md). */
void n265_set_badapt(int on) { g_badapt = on < 0 ? 0 : on > 4 ? 4 : on; }

void n265_set_mode(int mode, int qp, int ctb_log2, int keyint, int bframes,
                   int aq)
{
    g_mode = mode;
    g_qp = qp;
    g_ctb_log2 = ctb_log2;
    g_keyint = keyint < 1 ? 1 : keyint;
    g_bframes = bframes < 0 ? 0 : bframes > N265_MAX_BFRAMES
                                      ? N265_MAX_BFRAMES : bframes;
    g_aq = aq ? 1 : 0;
}

void n265_encoder_set_dump(n265_encoder *e, FILE *f)
{
    e->dump_recon = f;
}

/* ---- --pass / --stats: the two-pass sidecar and the plan (twopass.h) ----
 *
 * Runs LAST at open, after n265_la_init has fixed the lowres geometry and
 * n265_rc_open has turned the bitrate into bits per frame: the plan needs
 * the first to normalise a complexity and the second to have a target at
 * all.
 *
 * Every refusal here is made at open and names its number. A second pass
 * that silently plans from the wrong file is the failure mode this whole
 * function exists to prevent - the encode would succeed, the rate would be
 * wrong, and nothing would say why. */
static int twopass_open(n265_encoder *e, const n265_param *p)
{
    n265_2p_init(&e->tp);
    if (g_pass <= 0)
        return 0;               /* the encoder main ships */
    if (!g_stats_path || !*g_stats_path) {
        fprintf(stderr, "n265: --pass %d needs --stats <file>\n", g_pass);
        return -1;
    }
    e->tp.pass = g_pass;
    int nblk = e->la.bw * e->la.bh;
    if (nblk < 1)
        nblk = 1;
    if (g_pass >= 2) {
        char err[512] = { 0 };
        if (n265_2p_read(&e->tp, g_stats_path, err, sizeof err) < 0) {
            fprintf(stderr, "n265: --pass %d: %s\n", g_pass, err);
            return -1;
        }
        /* The target is the whole file the stats describe, not the frames
         * this run happens to be handed: the plan is a statement about a
         * sequence, and a pass 2 given half of it lands on half the bits by
         * construction. Saying so is the caller's job; the encoder's is to
         * plan the file it was told about. */
        if (!e->abr_on || !(e->rc.bits_per_frame > 0.0)) {
            fprintf(stderr, "n265: --pass %d needs --bitrate: a plan is a "
                    "rate factor solved onto a target, and under --crf and "
                    "--qp there is no target to solve onto\n", g_pass);
            return -1;
        }
        if (e->tp.nblk != nblk) {
            fprintf(stderr, "n265: --pass %d: %s was written at %d lowres "
                    "blocks (%dx%d) and this encode has %d. The stats "
                    "describe a different picture size.\n", g_pass,
                    g_stats_path, e->tp.nblk, e->tp.width, e->tp.height,
                    nblk);
            return -1;
        }
        double qcomp = e->rc.qcomp;
        { const char *v = n265_env("N265_2P_QCOMP"); if (v) qcomp = atof(v); }
        double clamp = 6.0;
        { const char *v = n265_env("N265_2P_CLAMP"); if (v) clamp = atof(v); }
        if (clamp < 0.0)
            clamp = 0.0;
        /* The trust radius, in QP: how far the ALLOCATION may move a frame
         * from where pass 1 measured it (twopass.h). The level shift on top
         * is unbounded, so this is not a bound on landing the target. */
        double trust = 2.0;
        { const char *v = n265_env("N265_2P_TRUST"); if (v) trust = atof(v); }
        double target = e->rc.bits_per_frame * (double)e->tp.n;
        if (n265_2p_solve(&e->tp, target, qcomp, clamp, trust,
                          (double)N265_QP_MIN, 51.0) < 0) {
            fprintf(stderr, "n265: --pass %d: the plan would not solve "
                    "(%d frames, %.0f bits)\n", g_pass, e->tp.n, target);
            return -1;
        }
        if (n265_env("N265_RC_TRACE") || n265_env("N265_2P_TRACE"))
            fprintf(stderr, "2P plan frames=%d target=%.0f planned=%.0f "
                    "rf=%.3f qcomp=%.3f clamp=%.2f trust=%.2f gain=%.3f\n",
                    e->tp.n, e->tp.target_bits, e->tp.plan_bits, e->tp.rf,
                    qcomp, clamp, trust, e->rc.tp_gain);
    }
    if (g_pass == 1 || g_pass == 3) {
        if (n265_2p_open_write(&e->tp, g_stats_path, g_pass == 3,
                               p->width, p->height, p->fps_num, p->fps_den,
                               nblk, n265_version_str()) < 0)
            return -1;
    }
    return 0;
}

/* --shot-stats, which wins over N265_SHOT_STATS (the CLI is the explicit
 * one, and the env is there so a script can arm the sidecar without
 * rewriting the command). Called right after open, before any frame has
 * retired, so no shot can be half-accumulated here. */
void n265_encoder_set_shot_stats(n265_encoder *e, const char *path)
{
    if (!e)
        return;
    if (e->shot_f) {
        fclose(e->shot_f);
        e->shot_f = NULL;
    }
    if (path && *path) {
        e->shot_f = fopen(path, "w");
        if (!e->shot_f)
            fprintf(stderr, "n265: cannot write --shot-stats %s\n", path);
    }
}

void n265_encoder_set_dump_decode(n265_encoder *e, FILE *f);
void n265_encoder_set_dump_decode(n265_encoder *e, FILE *f)
{
    e->dump_decode = f;
}

/* The coding order plan_brange emits for a B run [lo, hi], appended to
 * `ord` as display offsets, counting the ones that are references. It
 * lives beside the SPS's DPB derivation on purpose: if the pyramid shape
 * ever changes, both have to move together or the SPS under-declares. */
static void gop_order(int lo, int hi, int *ord, int *n, int *nref)
{
    if (lo > hi)
        return;
    int mid = (lo + hi) / 2;
    if ((lo <= mid - 1) || (mid + 1 <= hi))
        (*nref)++;
    ord[(*n)++] = mid;
    gop_order(lo, mid - 1, ord, n, nref);
    gop_order(mid + 1, hi, ord, n, nref);
}

/* N265_DPB_HOLD: hold every reference until the mini-GOP ends, which is
 * what the encoder did before 2026-09-10, and declare the DPB by the old
 * bound. The escape for the dispatch-time retirement below; 1 restores
 * the pre-retirement encoder byte for byte. Default 0. */
static int dpb_hold(void)
{
    static int v = -1;
    if (v < 0) { const char *s = n265_env("N265_DPB_HOLD"); v = s ? atoi(s) != 0 : 0; }
    return v;
}

/* Which of the marked pictures at POCs `m[0..nm)` does a picture at POC
 * `cur` reference? The POC-level mirror of n265_build_lists - L0 = past
 * nearest first then, for a B, future nearest first; L1 = future then
 * past; each list cut to ref_max - and it has to move whenever that
 * function's list rule does, or the retirement below drops a picture a
 * later frame still reads. `used[i]` is set for every m[i] that lands in
 * either list. */
static void ref_used(const int *m, int nm, int cur, int is_b, int ref_max,
                     uint8_t *used)
{
    int past[N265_DPB_SLOTS + N265_MAX_BFRAMES + 2], np = 0;
    int fut[N265_DPB_SLOTS + N265_MAX_BFRAMES + 2], nf = 0;
    for (int i = 0; i < nm; i++) {
        used[i] = 0;
        if (m[i] < cur) past[np++] = i;
        else if (m[i] > cur) fut[nf++] = i;
    }
    for (int i = 1; i < np; i++)
        for (int k = i; k > 0 && m[past[k]] > m[past[k - 1]]; k--)
            { int t = past[k]; past[k] = past[k - 1]; past[k - 1] = t; }
    for (int i = 1; i < nf; i++)
        for (int k = i; k > 0 && m[fut[k]] < m[fut[k - 1]]; k--)
            { int t = fut[k]; fut[k] = fut[k - 1]; fut[k - 1] = t; }
    int n0 = 0;
    for (int i = 0; i < np && n0 < ref_max; i++, n0++) used[past[i]] = 1;
    if (is_b) {
        for (int i = 0; i < nf && n0 < ref_max; i++, n0++) used[fut[i]] = 1;
        int n1 = 0;
        for (int i = 0; i < nf && n1 < ref_max; i++, n1++) used[fut[i]] = 1;
        for (int i = 0; i < np && n1 < ref_max; i++, n1++) used[past[i]] = 1;
    }
}

/* ---- the DPB walk (Annex C.5.2) -------------------------------------
 *
 * `buffering` used to be a BOUND - (carried anchors + this anchor + the
 * mini-GOP's reference Bs) + reorder + 1 - which over-declares by one to
 * two buffers: it counts the referenced and the awaiting-output sets as
 * disjoint, and it prices a structure that held every reference to the
 * end of its mini-GOP. Since the retirement moved to dispatch the
 * declaration is a WALK instead: the exact peak DPB fullness a decoder
 * reaches on our streams.
 *
 * The walk is a breadth-first search over mini-GOP BOUNDARY STATES - the
 * encoder's marked anchors and the decoder's DPB, both keyed on POC
 * relative to the last anchor - with one transition per B-run length
 * b-adapt can pick. That makes it exact over every sequence of run
 * lengths rather than a sweep over a window of them, and it closes in a
 * couple of hundred states at the widest (bframes 7, ref 3). A scene cut
 * or a keyint edge only ever shortens a run or resets the DPB, so the
 * transitions below cover them.
 *
 * The walk models the encoder as it is, which is why it is only used
 * when the retirement is on: at --bframes 5 or more with --ref 3 the
 * pre-retirement marking wants seven slots out of a pool of six and the
 * claim then overwrites a picture that stays marked (the DPB-pressure
 * bug T0a recorded). N265_DPB_HOLD keeps the old bound with the old
 * behaviour. */
/* DPBW_ENT bounds the marked set: ref_max carried anchors plus this
 * mini-GOP's anchor and its reference Bs. DPBW_MAX bounds the decoder's
 * DPB, which is those plus the pictures held only for output - at most
 * `reorder` of them after bumping, plus the one just stored - so 18 is
 * comfortably over the 11 the widest shape can reach. */
#define DPBW_MAX (2 * (N265_MAX_BFRAMES + 2))
#define DPBW_ENT (N265_MAX_REF + N265_MAX_BFRAMES + 2)

/* The `fine` value, in squared 8-bit levels, at or above which a CTU counts
 * as carrying a drawn OUTLINE rather than a fill or a gradient - the content
 * census N265_AQ_FLAT_THR gates the flat-frame protecting AQ sign on (phase
 * 10 item 3). Four luma levels of within-2x2 standard deviation. It is not a
 * sweep: on the three animation clips at 120 frames the share of CTUs over it
 * separates the two content kinds with a factor of three of daylight between
 * them - sintel_720p reads a median 1.4% and never passes 4.3%, sita_flat
 * 20.2% and never falls below 10.5%, sita_toon 29.4% and never below 12.0% -
 * and every natural clip is over 70%. docs/phase10.md "Items 3-4". */
#define AQ_FLAT_HI 16.0

struct dpbw_state {
    int nanc, n;
    int anc[N265_MAX_REF + 1];      /* marked anchor POCs, ascending */
    int poc[DPBW_MAX];              /* the decoder's DPB, POC ascending */
    uint8_t ref[DPBW_MAX], out[DPBW_MAX];
};

/* the pyramid coding order plan_brange emits, as POC offsets from the
 * mini-GOP's own opening anchor, with the reference flag each entry
 * carries. The twin of plan_brange; they move together. */
static void dpbw_brange(int lo, int hi, int *poc, uint8_t *isref, int *n)
{
    if (lo > hi)
        return;
    int mid = (lo + hi) / 2;
    poc[*n] = 1 + mid;
    isref[*n] = (lo <= mid - 1) || (mid + 1 <= hi);
    (*n)++;
    dpbw_brange(lo, mid - 1, poc, isref, n);
    dpbw_brange(mid + 1, hi, poc, isref, n);
}

/* Code one mini-GOP of `a` B frames out of boundary state `s` (whose
 * previous anchor sits at POC 0), running the encoder's plan, its
 * dispatch-time retirement and the decoder's C.5.2.2 / C.5.2.3 on the
 * RPS that produces. Raises *peak (DPB fullness) and *mpeak (marked
 * pictures) and writes the next boundary state, re-based on the new
 * anchor. */
static void dpbw_gop(const struct dpbw_state *s, int a, int ref_max, int R,
                     struct dpbw_state *out, int *peak, int *mpeak)
{
    int ppoc[N265_MAX_BFRAMES + 2];
    uint8_t pref[N265_MAX_BFRAMES + 2], panc[N265_MAX_BFRAMES + 2];
    int pn = 0;
    memset(panc, 0, sizeof panc);
    ppoc[pn] = a + 1; pref[pn] = 1; panc[pn] = 1; pn++;
    dpbw_brange(0, a - 1, ppoc, pref, &pn);

    /* every picture this mini-GOP can hold marked: the anchors carried in
     * and the plan's own references */
    int epoc[DPBW_ENT], elast[DPBW_ENT];
    uint8_t eanc[DPBW_ENT];
    int pent[N265_MAX_BFRAMES + 2], ne = 0;
    for (int i = 0; i < s->nanc; i++) {
        epoc[ne] = s->anc[i]; eanc[ne] = 1; elast[ne] = -1; ne++;
    }
    for (int i = 0; i < pn; i++) {
        pent[i] = -1;
        if (!pref[i])
            continue;
        pent[i] = ne;
        epoc[ne] = ppoc[i]; eanc[ne] = panc[i]; elast[ne] = -1; ne++;
    }

    /* lifetimes: the last coding index whose lists reach each picture */
    int cur[DPBW_ENT], nc = 0;
    for (int i = 0; i < s->nanc; i++) cur[nc++] = i;
    for (int i = 0; i < pn; i++) {
        int m[DPBW_ENT];
        uint8_t used[DPBW_ENT];
        for (int k = 0; k < nc; k++) m[k] = epoc[cur[k]];
        ref_used(m, nc, ppoc[i], !panc[i], ref_max, used);
        for (int k = 0; k < nc; k++) if (used[k]) elast[cur[k]] = i;
        if (pent[i] >= 0) cur[nc++] = pent[i];
    }
    /* the anchors the mini-GOP end carries forward live to the end */
    for (int r = 0; r < ref_max; r++) {
        int best = -1;
        for (int i = 0; i < ne; i++)
            if (eanc[i] && elast[i] != INT_MAX &&
                (best < 0 || epoc[i] > epoc[best]))
                best = i;
        if (best < 0)
            break;
        elast[best] = INT_MAX;
    }

    /* the coding pass, with the decoder walking beside it */
    int dpoc[DPBW_MAX + 1], nd = s->n;
    uint8_t dref[DPBW_MAX + 1], dout[DPBW_MAX + 1];
    for (int i = 0; i < nd; i++) {
        dpoc[i] = s->poc[i]; dref[i] = s->ref[i]; dout[i] = s->out[i];
    }
    nc = 0;
    for (int i = 0; i < s->nanc; i++) cur[nc++] = i;
    for (int i = 0; i < pn; i++) {
        for (int k = 0; k < nc; )
            if (elast[cur[k]] < i) cur[k] = cur[--nc];
            else k++;
        /* the RPS is exactly the marked set; anything it omits leaves the
         * decoder's DPB as soon as it is no longer needed for output */
        for (int d = 0; d < nd; d++) {
            int in = 0;
            for (int k = 0; k < nc && !in; k++)
                if (epoc[cur[k]] == dpoc[d]) in = 1;
            if (!in) dref[d] = 0;
        }
        for (int d = 0; d < nd; )
            if (!dref[d] && !dout[d]) {
                dpoc[d] = dpoc[nd - 1]; dref[d] = dref[nd - 1];
                dout[d] = dout[--nd];
            } else d++;
        for (int pass = 0; pass < 2; pass++) {
            if (pass) {                 /* store the current picture */
                dpoc[nd] = ppoc[i]; dref[nd] = pref[i]; dout[nd] = 1; nd++;
                if (nd > *peak) *peak = nd;
            }
            for (;;) {                  /* bumping, C.5.2.2 then C.5.2.3 */
                int no = 0, b = -1;
                for (int d = 0; d < nd; d++)
                    if (dout[d]) { no++; if (b < 0 || dpoc[d] < dpoc[b]) b = d; }
                if (no <= R || b < 0)
                    break;
                dout[b] = 0;
                if (!dref[b]) {
                    dpoc[b] = dpoc[nd - 1]; dref[b] = dref[nd - 1];
                    dout[b] = dout[--nd];
                }
            }
        }
        if (pent[i] >= 0) cur[nc++] = pent[i];
        if (nc > *mpeak) *mpeak = nc;
    }

    /* the mini-GOP end: reference Bs die here, anchors trim to ref_max */
    for (int k = 0; k < nc; )
        if (!eanc[cur[k]]) cur[k] = cur[--nc];
        else k++;
    while (nc > ref_max) {
        int b = 0;
        for (int k = 1; k < nc; k++) if (epoc[cur[k]] < epoc[cur[b]]) b = k;
        cur[b] = cur[--nc];
    }
    memset(out, 0, sizeof *out);
    out->nanc = nc;
    for (int k = 0; k < nc; k++) out->anc[k] = epoc[cur[k]] - (a + 1);
    for (int k = 1; k < nc; k++)        /* ascending, so the state is canonical */
        for (int j = k; j > 0 && out->anc[j] < out->anc[j - 1]; j--)
            { int t = out->anc[j]; out->anc[j] = out->anc[j - 1]; out->anc[j - 1] = t; }
    out->n = nd;
    for (int d = 0; d < nd; d++) {
        out->poc[d] = dpoc[d] - (a + 1);
        out->ref[d] = dref[d]; out->out[d] = dout[d];
    }
    for (int k = 1; k < nd; k++)
        for (int j = k; j > 0 && out->poc[j] < out->poc[j - 1]; j--) {
            int t = out->poc[j]; out->poc[j] = out->poc[j - 1]; out->poc[j - 1] = t;
            uint8_t u = out->ref[j]; out->ref[j] = out->ref[j - 1]; out->ref[j - 1] = u;
            u = out->out[j]; out->out[j] = out->out[j - 1]; out->out[j - 1] = u;
        }
}

/* The pool this shape runs (`e->dpb_slots`). `marked` is the largest
 * marked-picture count the shape can reach under the live retirement,
 * `refs` the structural bound on it.
 *
 * The floor wins wherever the marked set fits in it, which is every
 * shape that shipped before the pool was sized - the deep window at
 * --ref 1 and --ref 2 included - and that is what keeps their slot
 * choices, and so their bitstreams, exactly where they were. Where the
 * floor does not cover the shape the pool is the STRUCTURAL peak and not
 * the walk's: the walk models one retirement, and a pool at the
 * structural bound leaves claim_slot's overwrite branch unreachable
 * under any of them. Claims see at most `refs - 1` marked pictures - the
 * bound counts the claiming picture itself - so a pool of `refs` always
 * has an unmarked slot to hand out.
 *
 * It is NOT clamped to N265_DPB_SLOTS: the array bound is the caller's
 * to check, and a pool that does not fit the array has to be refused
 * rather than silently run one slot short. */
static int dpb_pool(int refs, int marked)
{
    return marked > N265_DPB_POOL_MIN ? refs : N265_DPB_POOL_MIN;
}

/* What the reference structure asks of a decoder's DPB (Annex C.5.2).
 *
 * `reorder` is exact. A picture is only ever reordered against its own
 * mini-GOP - display order advances monotonically from one mini-GOP to
 * the next - so the deepest reorder is the largest count of pictures
 * that precede a picture in the plan's coding order and follow it in
 * display order, maximised over every B-run length b-adapt may pick.
 * The retirement does not touch it.
 *
 * `buffering` is the picture storage buffer count: the walk above under
 * the shipped retirement, and the pre-2026-09-10 bound under
 * N265_DPB_HOLD.
 *
 * `peak` stays the STRUCTURAL bound on the marked-picture count -
 * carried anchors, this anchor, the mini-GOP's reference Bs - and not
 * the walk's, deliberately. It arms claim_slot's slot preference, and
 * the retirement can only ever lower the true peak, so the old bound
 * stays sound; keeping it is what makes the retirement leave every slot
 * CHOICE exactly where it was. `markpeak`, optional, is what the walk
 * actually reaches.
 *
 * `slots`, optional, is the reference pool the shape runs (dpb_pool
 * below). It is an encoder-side number and reaches no syntax element.
 *
 * The declaration before the walk was N265_DPB_SLOTS with
 * sps_max_num_reorder_pics = bframes, which at --bframes 7 signals a
 * reorder depth LARGER than the buffer count. That is out of range by
 * 7.4.3.2.1 and dec265 refuses the stream. */
void n265_dpb_requirement(int bframes, int ref_max, int *reorder,
                          int *buffering, int *peak, int *markpeak,
                          int *slots)
{
    int R = 0, nrefb = 0;
    for (int a = 0; a <= bframes; a++) {
        int ord[N265_MAX_BFRAMES + 1], n = 0, nref = 0;
        ord[n++] = a;                   /* the anchor is coded first */
        gop_order(0, a - 1, ord, &n, &nref);
        for (int i = 0; i < n; i++) {
            int c = 0;
            for (int k = 0; k < i; k++)
                if (ord[k] > ord[i])
                    c++;
            if (c > R)
                R = c;
        }
        if (nref > nrefb)
            nrefb = nref;
    }
    *reorder = R;
    int refs = ref_max + 1 + nrefb;
    if (peak)
        *peak = refs;           /* uncapped: how many slots can be marked */
    /* Both used to be capped at the pool, which was a fixed six. The
     * pool is sized from them now, so they report what the structure
     * asks for and nothing clamps them. */
    if (markpeak)
        *markpeak = refs;
    int bound = refs + R + 1;
    if (dpb_hold()) {
        /* holding every reference to the mini-GOP end IS the structural
         * bound, so it sizes its own pool */
        if (slots)
            *slots = dpb_pool(refs, refs);
        *buffering = bound;
        return;
    }
    /* the BFS. 4096 boundary states is an order of magnitude over the
     * widest shape; overflowing it falls back to the bound, which is the
     * safe direction. */
    enum { CAP = 4096 };
    struct dpbw_state *seen = malloc((size_t)CAP * sizeof *seen);
    if (!seen) {
        if (slots)
            *slots = dpb_pool(refs, refs);
        *buffering = bound;
        return;
    }
    int nseen = 1, head = 0, pk = 1, mpk = 1, over = 0;
    memset(&seen[0], 0, sizeof seen[0]);
    seen[0].nanc = 1;                   /* the IDR, marked and pending output */
    seen[0].n = 1; seen[0].ref[0] = 1; seen[0].out[0] = 1;
    while (head < nseen && !over) {
        struct dpbw_state s = seen[head++];
        for (int a = 0; a <= bframes; a++) {
            struct dpbw_state nx;
            dpbw_gop(&s, a, ref_max, R, &nx, &pk, &mpk);
            int dup = 0;
            for (int i = 0; i < nseen && !dup; i++)
                if (!memcmp(&seen[i], &nx, sizeof nx)) dup = 1;
            if (dup)
                continue;
            if (nseen >= CAP) { over = 1; break; }
            seen[nseen++] = nx;
        }
    }
    free(seen);
    if (over) {                         /* unreached; the bound stands in */
        if (slots)
            *slots = dpb_pool(refs, refs);
        *buffering = bound;
        return;
    }
    if (markpeak)
        *markpeak = mpk;
    if (slots)
        *slots = dpb_pool(refs, mpk);
    *buffering = pk;
}

/* Copy the immutable geometry and config into a frame_work. The ONE
 * writer of these fields (T0 audit); everything below is read-only for
 * the life of the encoder, which is what lets a row lane hold a copy. */
static void fw_config(n265_encoder *e, n265_frame_work *fw)
{
    fw->enc = e;
    fw->param = e->param;
    fw->mode = e->mode;
    fw->ctb_log2 = e->ctb_log2;
    fw->chroma_idc = e->chroma_idc;
    fw->csx = e->csx;
    fw->csy = e->csy;
    fw->c444 = e->c444;
    fw->c422 = e->c422;
    fw->pad_w = e->pad_w;
    fw->pad_h = e->pad_h;
    fw->ctb_cols = e->ctb_cols;
    fw->ctb_rows = e->ctb_rows;
    fw->map_stride = e->map_stride;
    fw->rect = e->rect;
    fw->amp = e->amp;
    fw->rdoq = e->rdoq;
    fw->lossless = e->lossless;
    fw->qp_delta_on = e->qp_delta_on;
    fw->bframes = e->bframes;
    fw->dpb_reorder = e->dpb_reorder;
    fw->dpb_buffering = e->dpb_buffering;
    fw->level_idc = e->level.idc;
    fw->level_tier = e->level.tier;
    fw->vui_timing = e->vui_timing;
    fw->sig = e->sig;
    fw->hrd_on = e->hrd_on;
    fw->hrd_bit_rate_scale = e->hrd_bit_rate_scale;
    fw->hrd_bit_rate_value = e->hrd_bit_rate_value;
    fw->hrd_cpb_size_scale = e->hrd_cpb_size_scale;
    fw->hrd_cpb_size_value = e->hrd_cpb_size_value;
    fw->psy = e->psy;
    fw->dbf_beta = e->dbf_beta;
    fw->dbf_tc = e->dbf_tc;
    fw->cu_stats = e->cu_stats;
    fw->cbit_trace = e->cbit_trace;
    fw->iw_trace = e->iw_trace;
    fw->iw_count = e->iw_trace || e->iw_adapt;
    for (int i = 0; i < 3; i++)
        fw->pad_stride[i] = e->pad_stride[i];
}

/* N265_DEPTH_DUMP=file (phase 7 item 4, step 0): where the per-CTU
 * training records go. Read once on the driver at encoder open, with the
 * other warms, so no row job ever performs the first read. Unset - the
 * shipping encoder - leaves fw->dd_rec NULL and every site below is a
 * NULL test. */
static const char *depth_dump_file(void)
{
    static const char *f = (const char *)-1;
    if (f == (const char *)-1) {
        const char *s = n265_env("N265_DEPTH_DUMP");
        f = (s && *s) ? s : NULL;
    }
    return f;
}

/* N265_CU_TRACE=file (phase 9 step 6a): where the per-node census
 * records go, same shape and same reason as the depth dump's. Unset
 * leaves fw->cu_rec NULL and every site is a NULL test. */
static const char *cu_trace_file(void)
{
    static const char *f = (const char *)-1;
    if (f == (const char *)-1) {
        const char *s = n265_env("N265_CU_TRACE");
        f = (s && *s) ? s : NULL;
    }
    return f;
}

static int fw_open(n265_encoder *e, n265_frame_work *fw)
{
    fw_config(e, fw);
    size_t nblk = (size_t)e->map_stride * (e->pad_h >> 2);
    size_t nctu = (size_t)e->ctb_cols * e->ctb_rows;
    fw->est_ctx = fw->est_store;
    fw->ctu_dmin = malloc(nctu);
    fw->ctu_dmax = malloc(nctu);
    fw->ct_depth = calloc(nblk, 1);
    fw->intra_mode = calloc(nblk, 1);
    fw->edge_map = calloc(nblk, 1);
    fw->blk_intra = calloc(nblk, 1);
    fw->blk_skip = calloc(nblk, 1);
    fw->blk_cbf_y = calloc(nblk, 1);
    fw->blk_mv = calloc(nblk * 4, sizeof(int16_t));
    fw->blk_pred = calloc(nblk, 1);
    fw->blk_ref = calloc(nblk * 2, 1);
    fw->blk_qp = calloc(nblk, 1);
    fw->ctu_qp_off = calloc(nctu, 1);
    fw->ctu_qp_raw = calloc(nctu, sizeof(float));
    fw->ctu_lam_q8 = calloc(nctu, sizeof(uint16_t));
    fw->sao = calloc(nctu, sizeof(sao_params));
    if (depth_dump_file()) {
        fw->dd_rec = calloc(nctu, sizeof(struct n265_depth_rec));
        if (!fw->dd_rec)
            return -1;
    }
    if (cu_trace_file()) {
        fw->cu_rec = calloc(nctu * N265_CU_REC_MAX,
                            sizeof(struct n265_cu_rec));
        fw->cu_rec_n = calloc(nctu, sizeof(uint16_t));
        if (!fw->cu_rec || !fw->cu_rec_n)
            return -1;
    }
    if (!fw->ctu_dmin || !fw->ctu_dmax || !fw->ct_depth || !fw->intra_mode ||
        !fw->edge_map || !fw->blk_intra || !fw->blk_skip || !fw->blk_cbf_y ||
        !fw->blk_mv || !fw->blk_pred || !fw->blk_ref || !fw->blk_qp ||
        !fw->ctu_qp_off || !fw->ctu_qp_raw || !fw->ctu_lam_q8 || !fw->sao)
        return -1;
    fw->prog = calloc(1, sizeof(*fw->prog) +
                         (size_t)e->ctb_rows * sizeof(atomic_int));
    fw->rrec = calloc((size_t)e->ctb_rows, sizeof(*fw->rrec));
    fw->est_snap = calloc((size_t)e->ctb_rows * CTX_COUNT,
                          sizeof(cabac_ctx_t));
    /* One 24-entry traffic line per CTB row, plus one at the end for the
     * frame handle itself - which is what a tally taken off an uncopied
     * n265_frame_work lands in, so no call site has to know. */
    fw->hf2_tr_rows = calloc((size_t)(e->ctb_rows + 1) * 24,
                             sizeof(uint64_t));
    fw->iw_arm_rows = calloc((size_t)(e->ctb_rows + 1) * N265_IW_CNT,
                             sizeof(uint64_t));
    if (!fw->rrec || !fw->est_snap || !fw->prog || !fw->hf2_tr_rows ||
        !fw->iw_arm_rows)
        return -1;
    fw->hf2_tr = fw->hf2_tr_rows + (size_t)e->ctb_rows * 24;
    fw->iw_arm = fw->iw_arm_rows + (size_t)e->ctb_rows * N265_IW_CNT;
    for (int i = 0; i < 3; i++) {
        int w = i ? e->pad_w >> e->csx : e->pad_w;
        int bh = (1 << e->ctb_log2) >> (i ? e->csy : 0);
        fw->sao_tmp[i] = malloc((size_t)w * bh * sizeof(pixel));
        fw->sao_line[i] = malloc((size_t)w * sizeof(pixel));
        if (!fw->sao_tmp[i] || !fw->sao_line[i])
            return -1;
    }
    size_t frame_bytes = n265_frame_samples(e->pad_w, e->pad_h, e->csx,
                                            e->csy) * sizeof(pixel);
    if (bs_init(&fw->rbsp, frame_bytes + 4096) < 0)
        return -1;
    return 0;
}

static void fw_close(n265_frame_work *fw)
{
    free(fw->ctu_dmin);
    free(fw->ctu_dmax);
    free(fw->ct_depth);
    free(fw->intra_mode);
    free(fw->edge_map);
    free(fw->blk_intra);
    free(fw->blk_skip);
    free(fw->blk_cbf_y);
    free(fw->blk_mv);
    free(fw->blk_pred);
    free(fw->blk_ref);
    free(fw->blk_qp);
    free(fw->ctu_qp_off);
    free(fw->ctu_qp_raw);
    free(fw->ctu_lam_q8);
    free(fw->dd_rec);
    free(fw->cu_rec);
    free(fw->cu_rec_n);
    free(fw->sc_prop);          /* phase 11 item 4 */
    free(fw->sao);
    for (int i = 0; i < 3; i++) {
        free(fw->sao_tmp[i]);
        free(fw->sao_line[i]);
    }
    for (int r = 0; fw->rrec && r < fw->ctb_rows; r++)
        free(fw->rrec[r].buf);
    free(fw->rrec);
    free(fw->est_snap);
    free(fw->hf2_tr_rows);
    free(fw->iw_arm_rows);
    free(fw->prog);
    bs_free(&fw->rbsp);
}

/* A row lane starts from a shallow copy of the frame's coding context
 * and re-points what has to be row-private. Nothing crosses a row
 * boundary except through est_snap, which is a fixed position (sec 2):
 * row r's cost-model contexts start from the snapshot row r-1 published
 * after its CTB 1, and row 0 starts from the slice-init contexts. The
 * analysis-side qPY_PREV is row-local too - it only prices the
 * cu_qp_delta bin now, since the QP map itself is resolved afterwards by
 * n265_qpmap_row. */
void n265_row_begin(n265_row_ctx *row, n265_frame_work *fw, int r)
{
    row->c = *fw;
    memcpy(row->est, r > 0 ? fw->est_snap + (size_t)(r - 1) * CTX_COUNT
                           : fw->est_store,
           sizeof(row->est));
    row->c.est_ctx = row->est;
    row->c.ent_qp_prev = fw->qp;
    row->c.sc = row->sc;
    row->c.hf2_tr = fw->hf2_tr_rows + (size_t)r * 24;
    row->c.iw_arm = fw->iw_arm_rows + (size_t)r * N265_IW_CNT;
    row->c.rec_buf = fw->rrec[r].buf;
    row->c.rec_cap = fw->rrec[r].cap;
    row->c.rec_len = 0;
    row->c.rec_pos = 0;
}

void n265_row_end(n265_row_ctx *row, n265_frame_work *fw, int r)
{
    fw->rrec[r].buf = row->c.rec_buf;
    fw->rrec[r].cap = row->c.rec_cap;
    fw->rrec[r].len = row->c.rec_len;
}

n265_encoder *n265_encoder_open(const n265_param *p)
{
    if (!p || (p->width & 1) || (p->height & 1) ||
        (p->csp != N265_CSP_I420 && p->csp != N265_CSP_I422 &&
         p->csp != N265_CSP_I444))
        return NULL;

    n265_encoder *e = calloc(1, sizeof(*e));
    if (!e)
        return NULL;
    e->param = *p;
    e->mode = g_mode;
    /* The chroma format, once, from the caller's csp - which IS
     * chroma_format_idc (N265_CSP_I420 is 0 for source compatibility and
     * is mapped to 1 here). Everything downstream reads these four fields
     * and nothing re-derives the format from the picture. */
    e->chroma_idc = p->csp == N265_CSP_I420 ? 1 : p->csp;
    e->csx = N265_CSX_OF(e->chroma_idc);
    e->csy = N265_CSY_OF(e->chroma_idc);
    e->c444 = e->chroma_idc == 3;
    e->c422 = e->chroma_idc == 2;
    e->qp_base = g_qp;
    e->crf_on = g_crf_on;
    e->crf = g_crf;
    /* ABR overrides CRF: the rate factor is solved, not given. The
     * fractional base QP rides with it - see encode_frame. */
    e->abr_on = g_bitrate > 0.0 && e->mode != N265_MODE_PCM;
    e->abr_bitrate = g_bitrate * 1000.0;
    if (e->abr_on)
        e->crf_on = 0;
    e->ref_max = e->mode == N265_MODE_PCM ? 1 : g_ref;
    e->badapt = g_badapt;
    { const char *v = n265_env("N265_CUTREE_STR"); e->cutree_str = v ? (float)atof(v) : 2.0f; }
    { const char *v = n265_env("N265_CT_QPW"); e->ct_qpw = v ? (float)atof(v) : 2.0f; }
    { const char *v = n265_env("N265_CT_LAMBDA"); e->ct_lambda = v ? (float)atof(v) : 0.0f; }
    /* N265_CT_FRAMEW (k_f): the share of the cu-tree field's FRAME MEAN the
     * base QP carries under CRF, on cutree_str's scale, where N265_CT_QPW
     * (k_b) is the share the per-CTU map carries. 1.0 ships (phase 7 step 2):
     * half the field's mean, the other half being what the fixed I/P/B
     * cascade was already double-counting. k_f == k_b switches the term off
     * and is the pre-item encoder byte for byte, which is the escape. */
    { const char *v = n265_env("N265_CT_FRAMEW"); e->ct_framew = v ? (float)atof(v) : 1.0f; }
    /* ...and on a flat-typed frame (la.h's content gate), where the field's
     * cross-frame reading is a content trade and not a double count: today's
     * form ships there, which is k_f = k_b. */
    { const char *v = n265_env("N265_CT_FRAMEW_FLAT");
      e->ct_framew_flat = v ? (float)atof(v) : e->ct_qpw; }
    /* phase 7 step 3, both default-off until the multi-shot gate places
     * them: a bound on the term, and a level taken over one shot. */
    { const char *v = n265_env("N265_CT_FRAME_CLAMP");
      e->ct_frame_clamp = v ? (float)atof(v) : 0.0f; }
    { const char *v = n265_env("N265_CT_FRAME_SHOT");
      e->ct_frame_shot = v ? atoi(v) != 0 : 0; }
    /* 0 ships (phase 7 step 3): the term may lower a frame's QP and never
     * raise it. The raise is what the across-shot gate charges for - on the
     * most static shot the leaves are nearly free and the anchors carry the
     * picture - and forbidding it takes ms_cif_30 from +2.39% to +0.70%
     * while the band keeps every point of its gain. Negative is unbounded,
     * the step 2 form. */
    { const char *v = n265_env("N265_CT_FRAME_UP");
      e->ct_frame_up = v ? (float)atof(v) : 0.0f; }
    /* N265_CT_UP_GATE (phase 7 follow-up 2): the content gate on that bound.
     * The raise is released on a frame whose lookahead sc_ratio is ABOVE this
     * threshold - a frame the previous picture does not already predict almost
     * perfectly. 0 is the gate off, which is the bound above on its own; a
     * threshold above every frame's ratio is the same thing, and a negative
     * one releases the raise everywhere (that is N265_CT_FRAME_UP=1's
     * encoder).
     *
     * 0.24 ships, and where it landed is the item's finding. The per-shot CTF
     * trace on ms_cif_30 puts the static/moving split at 0.13 - the shot the
     * raise was thought to cost runs 0.060-0.126 and no frame of any other
     * shot reaches 0.114 - and a gate there reads the same across-shot cost as
     * no gate at all. The raise charges on the low-ratio frame wherever it
     * occurs, so the threshold that works sits well inside the moving shots
     * and still gates two thirds of foreman
     * (docs/phase7-modulation-design.md "Follow-up 2"). */
    { const char *v = n265_env("N265_CT_UP_GATE");
      e->ct_up_gate = v ? (float)atof(v) : 0.24f; }
    /* N265_PB_CASCADE=r,l places the B steps of the fixed cascade by hand
     * (referenced B, leaf) instead of the 1 + b_depth form. Unset is that
     * form, so the default is today's; CRF/ABR only, because the cascade
     * under CQP is the band's control and was calibrated there. */
    { const char *v = n265_env("N265_PB_CASCADE");
      if (!v) v = "auto";       /* the 1 + b_depth ladder. "r,l" names the two
                                 * B steps outright; REFUSED as a default,
                                 * kept for re-pricing (phase 7 step 2) */
      if (strcmp(v, "auto")) {
          char *end; e->pb_cascade_r = (int)strtol(v, &end, 10);
          e->pb_cascade_l = *end == ',' ? (int)strtol(end + 1, NULL, 10)
                                        : e->pb_cascade_r;
          e->pb_cascade_on = 1; } }
    { const char *v = n265_env("N265_PB_CASCADE_FLAT");
      if (v && strcmp(v, "auto")) {
          char *end; e->pb_cascade_flat_r = (int)strtol(v, &end, 10);
          e->pb_cascade_flat_l = *end == ',' ? (int)strtol(end + 1, NULL, 10)
                                             : e->pb_cascade_flat_r;
          e->pb_cascade_flat_on = 1; } }
    /* AQ 0.7 with the joint field rounded once (2026-09-07 sweep vs cu-tree
     * strength: 6/7 clips better, -0.9% mean BD-VMAF-NEG at matched rate;
     * AQ 0.4 and cu-tree 2.5 each bought median by giving bus back 2-3%).
     * N265_AQ_STR=1 N265_QP_ROUND1=0 restores the old field. */
    { const char *v = n265_env("N265_AQ_STR"); e->aq_str = v ? (float)atof(v) : 0.7f; }
    /* N265_AQ_SIGNAL / N265_AQ_FLAT_SIGN: phase 7 item 2's M1'. The signal
     * ships at 1 (2026-09-12) and `=0` is the pre-item encoder byte for
     * byte; the flat-frame sign is refused and stays off. The AQ field
     * below is where both act, and docs/phase7-psy-design.md "M1' result"
     * carries the tables. */
    { const char *v = n265_env("N265_AQ_SIGNAL"); e->aq_signal = v ? atoi(v) : 1; }
    { const char *v = n265_env("N265_AQ_FLAT_SIGN"); e->aq_flat_sign = v ? (float)atof(v) : 0.0f; }
    { const char *v = n265_env("N265_QP_ROUND1"); e->qp_round1 = v ? atoi(v) != 0 : 1; }
    /* rectangular (2NxN/Nx2N) and asymmetric (2NxnU/D, nLx2N, nRx2N)
     * inter partitions. AMP rides on rect: the AMP trial is only entered
     * where a rectangular split already beat 2Nx2N, so turning rect off
     * turns AMP off with it. N265_NO_RECT / N265_NO_AMP are the escapes. */
    /* Both read a VALUE since the preset ladder landed (2026-09-10):
     * `fast` turns rect off through the same table every other knob goes
     * through, and a knob a preset sets has to stay overridable, which a
     * presence-only reader cannot be. `N265_NO_RECT=1` is unchanged;
     * `N265_NO_RECT=0` now means what it says.
     *
     * N265_NO_AMP DEFAULTS TO 1 since 2026-09-11 (phase 6b): the
     * asymmetric orientations are 1.083x on the NEON binary for +0.18
     * CQP / +0.76 CRF of band median.
     *
     * N265_NO_RECT DEFAULTS TO 1 since 2026-09-11 (phase 6b step 4).
     * The rectangular block is the largest single economy left on the
     * ladder - 1.303x at one thread and 1.346x at auto - for +1.47 CQP
     * / +2.17 CRF of CIF-7 band median, which the budget covers. Step 3
     * refused it on `sita_flat_720p` alone, where it cost 3.91 points
     * of CQP BD and took the clip behind x265; that clip is flat-shaded
     * animation, and on flat-shaded animation a CU on a drawn outline
     * IS two motion fields, so the exception is N265_RECT_FLAT (ctu.c)
     * and not a retreat. `--preset slow` sets N265_NO_RECT=0 and is the
     * phase-5-exit medium; `--preset fast` still names it. */
    { const char *v = n265_env("N265_NO_RECT");
      e->rect = e->mode != N265_MODE_PCM && !(v ? atoi(v) != 0 : 1); }
    { const char *v = n265_env("N265_NO_AMP");
      e->amp = e->rect && !(v ? atoi(v) != 0 : 1); }
    e->ctb_log2 = e->mode == N265_MODE_PCM ? 4 : g_ctb_log2;
    e->keyint = e->mode == N265_MODE_PCM ? 1 : g_keyint;
    e->bframes = e->mode == N265_MODE_PCM ? 0 : g_bframes;
    {   /* the DPB the reference structure needs, solved once: the walk
         * is a BFS and costs a few hundred microseconds, so it must not
         * ride along in fw_config once per frame_work. */
        int markpeak = 0;
        n265_dpb_requirement(e->bframes, e->ref_max, &e->dpb_reorder,
                             &e->dpb_buffering, &e->dpb_peak, &markpeak,
                             &e->dpb_slots);
        if (n265_env("N265_DPB_TRACE"))
            fprintf(stderr, "DPB bframes=%d ref=%d reorder=%d buffering=%d "
                    "marked_peak=%d slot_peak=%d slots=%d hold=%d\n",
                    e->bframes, e->ref_max, e->dpb_reorder, e->dpb_buffering,
                    markpeak, e->dpb_peak, e->dpb_slots, dpb_hold());
        /* The pool used to be a fixed six pictures, and `--ref 3` above
         * `--bframes 7` was refused here: the marked-picture peak reaches
         * seven at bframes 8 and eight at bframes 10 and up, and past the
         * pool claim_slot's last branch overwrites a picture still in the
         * RPS (the DPB-pressure bug T0a recorded). The pool is sized from
         * the shape now, so the refusal is gone and only the ARRAY bound
         * is left to check - which the CLI's own ranges (--ref 1..3,
         * --bframes 0..15) already keep inside N265_DPB_SLOTS. It stays
         * as an assertion an API caller cannot walk past. */
        if (e->dpb_slots > N265_DPB_SLOTS) {
            fprintf(stderr, "n265: --bframes %d with --ref %d needs %d DPB "
                    "slots and the pool holds %d; lower --ref or --bframes\n",
                    e->bframes, e->ref_max, e->dpb_slots, N265_DPB_SLOTS);
            goto fail;
        }
    }
    e->no_sao = n265_env("N265_NO_SAO") != NULL;      /* diagnostic */
    e->cu_stats = n265_env("N265_CU_STATS") != NULL;  /* diagnostic */
    e->cbit_trace = n265_env("N265_CBIT_TRACE") != NULL;   /* diagnostic */
    e->iw_trace = n265_env("N265_INTRA_WIN_TRACE") != NULL; /* diagnostic */
    /* N265_INTRA_GATE=10 (step 3): the intra gate keyed on what the
     * encoder's own recent anchors got out of the intra arm. The mode
     * lives in ctu.c; what the driver owns is the window and the
     * threshold, and the per-frame answer computed in dispatch_frame. */
    { const char *v = n265_env("N265_INTRA_GATE");
      e->iw_adapt = v && atoi(v) == 10; }
    { const char *v = n265_env("N265_INTRA_ADAPT_K");
      int k = v ? atoi(v) : 32;
      e->iw_win = k < 1 ? 1 : k > N265_IW_HIST - 16 ? N265_IW_HIST - 16 : k; }
    { const char *v = n265_env("N265_INTRA_ADAPT_THR");
      e->iw_thr = v ? atoi(v) : 100; }   /* tenths of a percent */
    for (int i = 0; i < N265_IW_HIST; i++)
        e->iw_hist[i].cidx = -1;
    for (int i = 0; i < N265_SC_HIST; i++)
        e->sc_hist[i].cidx = -1;    /* phase 11 item 4 */
    /* N265_SHOT_STATS=file: the per-shot sidecar, the env half of
     * --shot-stats (which overrides it). Opened here, on the driver, so
     * no frame's retirement performs the first read. */
    { const char *v = n265_env("N265_SHOT_STATS");
      if (v && *v) e->shot_f = fopen(v, "w"); }
    /* RDOQ on by default (2026-09-06: -5.9% median BD-VMAF-NEG at matched
     * rate, 7/7 clips, worst -5.5%); N265_RDOQ=0 is the escape.
     *
     * `--rdoq-level` (n265_set_rdoq_level) is the user-facing half and it
     * WINS where it was given - over the environment and over the preset
     * table alike, the rule `--ctu` follows. It carries X265'S numbering,
     * not this knob's; `rdoq_level_map` above is the one place the two
     * meet. */
    { const char *v = n265_env("N265_RDOQ"); e->rdoq = v ? atoi(v) : 1;
      if (g_rdoq_level >= 0) e->rdoq = rdoq_level_map(g_rdoq_level); }
    { const char *v = n265_env("N265_PSY"); e->psy = v ? atof(v) : 0.0; }
    e->aq = g_aq || n265_env("N265_AQ") != NULL;      /* variance AQ */
    e->la_depth = e->mode == N265_MODE_PCM || !e->bframes ? g_la_depth
                                                          : g_la_depth;
    e->cutree = e->mode == N265_MODE_PCM ? 0 : g_cutree;
    /* ---- --lossless (phase 8 item 5b) ----
     *
     * transquant_bypass. A bypass CU has no quantiser at all (8.6.2: the
     * coded level IS the residual sample), so every tool whose job is to
     * decide what the quantiser throws away has nothing to decide and is
     * turned off HERE, at open, rather than being left to no-op its way
     * through the frame:
     *
     *   - rate control. ABR solves a rate factor and CRF a base QP; both
     *     control bits by moving QP, and QP moves nothing in a bypass CU.
     *     The rate is whatever the content costs, so both are refused and
     *     the encode runs at the (irrelevant) fixed QP.
     *   - RDOQ. It prices coefficients against lambda * the distortion
     *     rounding them introduces, and bypass rounds nothing.
     *   - sign-bit hiding. 7.4.9.11 makes signHidden depend on
     *     !cu_transquant_bypass_flag, so it is off in the syntax by the
     *     spec's own condition; the PPS flag goes down with it (every CU
     *     here is bypass) and the encoder-side hider is not called.
     *   - AQ, cu-tree and the fractional base QP: three ways of moving QP
     *     per CTU or per frame. With no quantiser they only spend bits on
     *     cu_qp_delta, so the PPS flag goes down too (qp_delta_on below
     *     reads exactly these three).
     *   - psy-rd. It rides on the SSD term, which is identically zero.
     *
     * PCM is the other lossless path and the two are exclusive: --pcm
     * wins the flag, because its whole CTU writer bypasses this one. */
    e->lossless = g_lossless && e->mode != N265_MODE_PCM;
    if (e->lossless) {
        e->abr_on = 0;
        e->crf_on = 0;
        e->rdoq = 0;
        e->psy = 0.0;
        e->aq = 0;
        e->cutree = 0;
    }
    /* --shot-window / N265_SHOT_WINDOW: the opt-in shot planner
     * (docs/phase7-shot-design.md step 2). The CLI wins over the env,
     * the way --shot-stats does, and the env half is what lets a script
     * arm an arm without rewriting the command.
     *
     * CRF ONLY, checked rather than assumed. The plan moves the base QP
     * across shots: under CQP the user named the quantiser and nothing
     * may move it, and under ABR the rate loop
     * already allocates across shots from measured bits and would spend
     * the rest of the shot undoing the offset. Asked for anywhere else
     * the mode says so and turns itself off, so nobody pays N frames of
     * latency for a term that is not running. */
    { int w = g_shot_window;
      if (w == 0) { const char *v = n265_env("N265_SHOT_WINDOW");
                    w = v ? atoi(v) : 0; }
      if (w < 0) w = 0;
      if (w > N265_MAX_SHOT_WINDOW) w = N265_MAX_SHOT_WINDOW;
      if (w > 0 && !e->crf_on) {
          fprintf(stderr, "n265: --shot-window needs --crf (the plan moves "
                  "the base QP); the shot planner is off\n");
          w = 0;
      }
      e->shot_window = w; }
    /* k, and the bound on what one shot's offset may be. The form is
     * rc.c's own complexity model read over a shot instead of a frame:
     * bits go as (complexity)^qcomp, so holding quality level across two
     * shots a factor r apart in complexity is (1 - qcomp) * log2(r) QP
     * steps, and k says how much of that to take. The clamp is what
     * keeps one anomalous shot - a black frame run, a hard cut into
     * noise - from moving the base further than the band was ever
     * measured over. */
    { const char *v = n265_env("N265_SHOT_K"); e->shot_k = v ? atof(v) : 1.0; }
    { const char *v = n265_env("N265_SHOT_CLAMP");
      e->shot_clamp = v ? atof(v) : 2.0; }
    /* How fast the level follows the shots already planned. It is a
     * STREAMING level - it never looks past the window - so the first
     * shot seeds it and takes no offset by construction. */
    { const char *v = n265_env("N265_SHOT_EMA"); e->shot_ema = v ? atof(v) : 0.5; }
    e->la_fill = e->shot_window > e->la_depth ? e->shot_window : e->la_depth;
    /* --film-grain. The estimator is opened only where the flag asked for
     * it, so an encode without the flag never reaches a line of grain.c.
     *
     * The chroma format gate is not a limitation of the SYNTAX - the SEI
     * is defined for every format - but of what can be measured: the only
     * synthesiser in this project's chain does 8-bit 4:2:0 and nothing
     * else (docs/phase12.md step 0), so a model written for anything else
     * would be a claim no gate in this tree can check. It is refused
     * loudly rather than written blind. */
    e->fgc_strength = g_film_grain;
    e->fgc_denoise = g_fgc_denoise;
    /* --lossless and --pcm code the source exactly; coring the source
     * first would make "lossless" a claim about a picture the user never
     * handed us. The SEI itself is still allowed there - it changes no
     * coded sample - but the denoise is refused, loudly, rather than
     * quietly redefining what the mode means. */
    if (e->fgc_denoise && (e->lossless || e->mode == N265_MODE_PCM)) {
        fprintf(stderr, "n265: --film-grain-denoise codes a FILTERED "
                "source, which --lossless/--pcm cannot mean; denoise off "
                "(the SEI is still written)\n");
        e->fgc_denoise = 0;
    }
    if (e->fgc_strength > 0.0 && (BIT_DEPTH != 8 || e->chroma_idc != 1)) {
        fprintf(stderr, "n265: --film-grain needs 8-bit 4:2:0 (the only "
                "shape the measurement chain can synthesise); off\n");
        e->fgc_strength = 0.0;
        e->fgc_denoise = 0;
    }
    if (e->fgc_strength > 0.0) {
        e->fgc_est = n265_grain_open(e->param.width, e->param.height,
                                     e->csx, e->csy);
        if (!e->fgc_est) {
            fprintf(stderr, "n265: --film-grain: out of memory; off\n");
            e->fgc_strength = 0.0;
            e->fgc_denoise = 0;
        }
    }
    /* N265_QP_FRAC: carry the base QP's fraction and dither it over the CTUs
     * (the block at the end of dispatch_frame), which makes the achieved rate
     * continuous in whatever moves the base. It has to be decided here rather
     * than next to abr_on, because its default reads e->cutree.
     *
     * ON under ABR since the rate control landed: rc.c solves a float rate
     * factor and an integer base throws 5-15% of a step away.
     *
     * ON under CRF since phase 7 follow-up 1, but only where the cu-tree
     * FRAME term is live. Under CRF the base carries three float quantities -
     * N265_CRF_SHIFT, the AQ_DC frame term and the cu-tree frame term - and
     * rounding them together makes the crf-to-rate mapping a staircase: with
     * the frame term's raise forbidden the whole frame ladder moves in ONE
     * integer step, so foreman at --crf 28 read 53326 bytes at shift 5.4 and
     * 47335 at 5.5 with nothing in between, and the SAME encoder read a CIF-7
     * band median of -10.66 at one shift and -9.65 at another. The fraction
     * makes the shift a continuous control again. Gated on the frame term
     * because that gate is also the item's escape: N265_CT_FRAMEW=2 turns the
     * term off AND this with it, and is main byte for byte (the preset tables
     * name it, so fast and slow are unchanged too).
     *
     * Under CQP the base is an integer with no term on it at all, so the
     * fraction would be exactly 0 - but the default stays false there, so CQP
     * is untouched by construction as well as by arithmetic. */
    { const char *v = n265_env("N265_QP_FRAC");
      /* the term is live only where k_f differs from k_b on some frame type
       * (n265_ct_frame_term is written as that difference, exactly 0 when
       * they match) */
      int ctf_on = e->cutree && e->cutree_str != 0.0f &&
                   (e->ct_framew != e->ct_qpw || e->ct_framew_flat != e->ct_qpw);
      e->qp_frac = v ? atoi(v) != 0 : (e->abr_on || (e->crf_on && ctf_on));
      if (e->lossless) e->qp_frac = 0; }
    /* N265_QP_FRAC_FLAT: whether a frame the content gate typed flat-shaded
     * carries the fraction too, where the frame term is zero by default and
     * there is nothing for the continuity to buy. 0 ships; 1 is the first
     * form of the follow-up and the arm the three animation clips refused
     * (sita_flat +1.52, sintel +1.23), kept for re-pricing. */
    { const char *v = n265_env("N265_QP_FRAC_FLAT");
      e->qp_frac_flat = v ? atoi(v) != 0 : 0; }
    e->qp_delta_on = e->mode != N265_MODE_PCM &&
                     (e->aq || e->cutree || e->qp_frac);
    e->scenecut = e->mode != N265_MODE_PCM;
    /* deblocking offsets: both clamped to the spec's +-6, and both zero by
     * default, which keeps the PPS control block out of the stream */
    { const char *v = n265_env("N265_DBF_BETA"); e->dbf_beta = v ? atoi(v) : 0; }
    { const char *v = n265_env("N265_DBF_TC"); e->dbf_tc = v ? atoi(v) : 0; }
    if (e->dbf_beta < -6) e->dbf_beta = -6;
    if (e->dbf_beta > 6) e->dbf_beta = 6;
    if (e->dbf_tc < -6) e->dbf_tc = -6;
    if (e->dbf_tc > 6) e->dbf_tc = 6;
    int align = e->mode == N265_MODE_PCM ? 16 : (1 << MIN_CB_LOG2);
    if (p->width < align || p->height < align)
        goto fail;
    e->pad_w = (p->width + align - 1) & ~(align - 1);
    e->pad_h = (p->height + align - 1) & ~(align - 1);
    int ctb = 1 << e->ctb_log2;
    e->ctb_cols = (e->pad_w + ctb - 1) >> e->ctb_log2;
    e->ctb_rows = (e->pad_h + ctb - 1) >> e->ctb_log2;

    /* ---- the hypothetical decoder buffer (phase 12 item 2) ----
     *
     * Configured here, before the level, because --vbv-maxrate is a rate
     * the encoder ENFORCES whatever the mode - which is exactly the
     * condition the level's rate leg asks for - and --vbv-bufsize is a CPB
     * the level has to admit.
     *
     * Every refusal is at open and every one of them names its number. A
     * VBV is a promise to a decoder, and the three ways to make it a
     * promise nothing keeps are to name half of it, to name it with no
     * quantiser the clamp can move (CQP, PCM, lossless), or to name a
     * buffer that starts fuller than it is. */
    {
        double fps = p->fps_den > 0 ? (double)p->fps_num / p->fps_den : 0.0;
        double maxrate = g_vbv_maxrate * 1000.0;
        double bufsize = g_vbv_bufsize * 1000.0;
        if ((g_vbv_maxrate > 0.0) != (g_vbv_bufsize > 0.0)) {
            fprintf(stderr, "n265: --vbv-maxrate and --vbv-bufsize are one "
                            "knob in two halves; name both or neither\n");
            goto fail;
        }
        if (g_vbv_maxrate < 0.0 || g_vbv_bufsize < 0.0) {
            fprintf(stderr, "n265: --vbv-maxrate and --vbv-bufsize are "
                            "positive or absent\n");
            goto fail;
        }
        if (maxrate > 0.0) {
            if (!e->abr_on && !e->crf_on) {
                fprintf(stderr, "n265: --vbv-maxrate needs a rate control "
                                "with a quantiser to move: use --bitrate "
                                "(ABR) or --crf (capped CRF). Under --qp, "
                                "--pcm and --lossless the buffer is whatever "
                                "the content costs.\n");
                goto fail;
            }
            if (!(g_vbv_init > 0.0) || g_vbv_init > 1.0) {
                fprintf(stderr, "n265: --vbv-init is a fraction of the "
                                "buffer in (0, 1]; %g is not\n", g_vbv_init);
                goto fail;
            }
            if (fps <= 0.0) {
                fprintf(stderr, "n265: --vbv-maxrate needs a frame rate: the "
                                "CPB's removal times are one per picture and "
                                "this source declares none\n");
                goto fail;
            }
            if (e->abr_on && maxrate < e->abr_bitrate) {
                fprintf(stderr, "n265: --vbv-maxrate %.0f is below --bitrate "
                                "%.0f: the buffer would be asked to hold a "
                                "stream that never fits in it\n",
                        maxrate / 1000.0, e->abr_bitrate / 1000.0);
                goto fail;
            }
        }
        /* N265_VBV_SAFE: the share of the strictly available bits a PLAN may
         * claim. The clamp is applied to the bits model's PREDICTION and the
         * coded frame is what actually leaves the buffer, so the gap is the
         * only thing between a model error and an underflow - and the model
         * is a per-class scale learned online, which is wrong by tens of
         * percent on the frames after a cut. See docs/phase12.md, Item 2,
         * for the corpus read the default is set from. */
        double safe = 0.85;
        { const char *v = n265_env("N265_VBV_SAFE"); if (v) safe = atof(v); }
        /* N265_VBV_LOOKAHEAD: the window's own plans are reserved for, so a
         * frame worth twenty frames of buffer seven frames away is saved
         * for rather than discovered. 1 ships; 0 leaves the immediate
         * bound alone and is the arm the cost of the reservation is read
         * against. */
        int vla = 1;
        { const char *v = n265_env("N265_VBV_LOOKAHEAD"); if (v) vla = atoi(v); }
        /* N265_VBV_RESERVE: the floor under that reservation, as a share of
         * what the buffer actually holds. The k = 0 bound is a FACT and is
         * never floored; the window's is a FORECAST about frames nobody has
         * coded, and where the content turns over it can be wrong by two
         * orders of magnitude in the expensive direction. See vbv.c. */
        double reserve = 0.2;
        { const char *v = n265_env("N265_VBV_RESERVE"); if (v) reserve = atof(v); }
        n265_vbv_init(&e->vbv, maxrate, bufsize, g_vbv_init, fps, safe, vla,
                      reserve);
        e->vbv_trace = n265_env("N265_VBV_TRACE") != NULL;
        { const char *v = n265_env("N265_VBV_HRD");
          e->vbv_hrd = v ? atoi(v) != 0 : 1; }
        e->vui_timing = g_vui_timing && p->fps_num > 0 && p->fps_den > 0;
        /* The pass-through signalling, as the setters left it (signal.h).
         * It is copied whole and validated nowhere: every field is already
         * a syntax element the CLI bounded, and there is nothing about the
         * STREAM that could refuse one - a colour primary is a claim about
         * the source, not about anything this encoder decides. */
        e->sig = g_sig;
        if (e->sig.hash) {
            /* the scratch one picture's hash message is built in: 64 bytes
             * holds the widest of the three (three 16-byte MD5s, the type
             * byte and the message header). */
            if (bs_init(&e->hash_bs, 64) < 0)
                goto fail;
        }
        /* hrd_parameters (E.2.2) in the VUI: the CPB schedule the clamp
         * enforces, scaled per E.2.3.
         *
         *   BitRate[0] = (bit_rate_value_minus1 + 1) << (6 + bit_rate_scale)
         *   CpbSize[0] = (cpb_size_value_minus1 + 1) << (4 + cpb_size_scale)
         *
         * Both are rounded UP. A declared BitRate below the arrival rate
         * the encoder planned against is a buffer that empties faster than
         * the encoder believed, and a declared CpbSize below the buffer it
         * planned against is one that overflows where the encoder capped -
         * so in both directions the safe rounding is the one that gives the
         * decoder at least what was assumed. The scale is the smallest that
         * keeps the value inside 32 bits, which is the most precise
         * representation E.2.3 has for the number, and it is a pure
         * function of the two knobs: the same VBV writes the same bits.
         *
         * It is written only WITH a VBV. Without one the encoder enforces
         * no schedule, and a stream is better carrying no HRD than an HRD
         * nothing behind it keeps. */
        e->hrd_on = e->vbv.on && e->vui_timing && e->vbv_hrd;
        if (e->hrd_on) {
            unsigned s = 0;
            double v = e->vbv.maxrate;
            while (s < 15 && ceil(v / (double)(1u << (6 + s))) > 2147483646.0)
                s++;
            e->hrd_bit_rate_scale = s;
            e->hrd_bit_rate_value =
                (unsigned)ceil(v / (double)(1u << (6 + s)));
            if (e->hrd_bit_rate_value == 0)
                e->hrd_bit_rate_value = 1;
            e->hrd_bit_rate_bits = (int64_t)e->hrd_bit_rate_value <<
                                   (6 + e->hrd_bit_rate_scale);
            s = 0;
            v = e->vbv.bufsize;
            while (s < 15 && ceil(v / (double)(1u << (4 + s))) > 2147483646.0)
                s++;
            e->hrd_cpb_size_scale = s;
            e->hrd_cpb_size_value =
                (unsigned)ceil(v / (double)(1u << (4 + s)));
            if (e->hrd_cpb_size_value == 0)
                e->hrd_cpb_size_value = 1;
            e->hrd_cpb_size_bits = (int64_t)e->hrd_cpb_size_value <<
                                   (4 + e->hrd_cpb_size_scale);
            /* the scratch the per-AU buffering_period + pic_timing pair is
             * built in (hrd.h). Twenty bytes is the whole message on an
             * IRAP; the writer grows it if a future one is longer. */
            if (bs_init(&e->hrd_bs, 64) < 0)
                goto fail;
            e->hrd_bp_code = -1;    /* no preceding buffering period yet */
            e->hrd_cvs_code = 0;
        }
    }

    /* ---- Annex A: the level and the tier (phase 8 item 7) ----
     *
     * Solved HERE, at open, and not in write_ptl, for two reasons: a
     * refusal is only useful before the first frame is coded, and the
     * numbers the trace prints are the ones the SPS will carry rather
     * than a re-derivation of them.
     *
     * The picture the level is bounded by is the CODED one - pad_w by
     * pad_h, which is what pic_width_in_luma_samples carries - and not
     * the conformance-cropped one write_ptl used to be handed. The two
     * differ only on a source that is not a multiple of the minimum CB,
     * and there the cropped size understates the samples a decoder has
     * to hold.
     *
     * The rate leg is a constraint only where the rate is KNOWN:
     *
     *   - ABR names it outright.
     *   - I_PCM has it by construction: every CTB is one 16x16 I_PCM CU,
     *     so the payload is the geometry times the sample width. This is
     *     the "PCM exceeds every tier's MaxBR" debt in docs/ideas.md, and
     *     what closes it is consulting Table A.7 instead of skipping it -
     *     CIF PCM at 29.97 fps is 36.7 Mbit/s, which level 5.1 admits.
     *   - Under CRF and CQP the rate is whatever the content costs. No
     *     level can be derived from a number the encoder does not have,
     *     so MaxBR is REPORTED (the trace's max_br) and not enforced,
     *     which is what --help says. */
    {
        n265_level_req rq;
        memset(&rq, 0, sizeof(rq));
        rq.width = e->pad_w;
        rq.height = e->pad_h;
        rq.dpb = e->dpb_buffering;
        rq.fps_num = p->fps_num;
        rq.fps_den = p->fps_den;
        rq.slice_segments = 1;          /* one slice segment per picture   */
        rq.tile_rows = rq.tile_cols = 1;   /* tiles_enabled_flag is 0      */
        rq.want_idc = g_level_idc;
        rq.want_high_tier = g_high_tier;
        if (e->mode == N265_MODE_PCM) {
            double fps = p->fps_den > 0 ? (double)p->fps_num / p->fps_den : 0.0;
            rq.bitrate = (double)n265_pcm_bits_per_frame(e->ctb_cols,
                                                         e->ctb_rows,
                                                         BIT_DEPTH) * fps;
            rq.enforce_br = 1;
        } else if (e->abr_on) {
            rq.bitrate = e->abr_bitrate;
            rq.enforce_br = 1;
        }
        /* ...and --vbv-maxrate, which is the strongest form of "the rate is
         * known": it is not a target the loop aims at but a ceiling the
         * clamp enforces per frame, so it is the rate Table A.7 has to
         * admit even under CRF, where nothing else about the rate is
         * knowable. It replaces an ABR target rather than joining it -
         * maxrate is never below the target (refused at open above), and
         * the stream can reach it. */
        if (e->vbv.on) {
            /* ...and it is the DECLARED pair that is bounded, not the raw
             * knobs. E.2.3 can only express a rate as a scaled integer, and
             * the HRD above rounds UP to stay safe, so a --vbv-maxrate
             * sitting exactly on a level's MaxBR would be signalled a few
             * hundred bit/s over it. A.4.2 bounds what the SPS SAYS, so the
             * level is solved against that number and the two can never
             * disagree. Without an HRD the raw knobs are what the clamp
             * enforces and they are what is bounded. */
            rq.bitrate = e->hrd_on ? (double)e->hrd_bit_rate_bits
                                   : e->vbv.maxrate;
            rq.enforce_br = 1;
            rq.cpb_size = e->hrd_on ? e->hrd_cpb_size_bits
                                    : (int64_t)e->vbv.bufsize;
        }
        if (n265_level_solve(&rq, &e->level) < 0) {
            fprintf(stderr, "n265: %s\n", e->level.err);
            goto fail;
        }
        if (n265_env("N265_LEVEL_TRACE")) {
            const n265_level_out *L = &e->level;
            fprintf(stderr,
                    "LEVEL idc=%d level=%s tier=%s src=%s driver=%s "
                    "pic=%dx%d ps=%lld max_luma_ps=%lld "
                    "dpb=%d max_dpb=%d fps=%d/%d luma_sr=%lld "
                    "max_luma_sr=%lld bitrate=%.0f max_br=%lld "
                    "max_cpb=%lld br=%s%s\n",
                    L->idc, L->name, L->tier ? "high" : "main",
                    g_level_idc ? "--level" : "auto", L->driver,
                    e->pad_w, e->pad_h, (long long)L->ps,
                    (long long)L->max_luma_ps,
                    e->dpb_buffering, L->max_dpb, p->fps_num, p->fps_den,
                    (long long)L->luma_sr, (long long)L->max_luma_sr,
                    rq.bitrate, (long long)L->max_br, (long long)L->max_cpb,
                    rq.enforce_br ? "enforced" : "reported",
                    L->tier_raised ? " tier_raised=1" : "");
        }
    }

    /* K, the frames in flight (sec 1). A pure scheduling knob: it moves
     * no bits, only how much of the mini-GOP overlaps. */
    { const char *v = n265_env("N265_MAX_FRAME_WORK");
      /* T3/4's sweep. K is bounded in practice by the DPB slot pool, not
       * by the frame_works: a frame in flight that is a REFERENCE holds a
       * slot until it retires, and when the claim runs out of unmarked
       * slots D3 stalls the driver. With a B pyramid only the anchors and
       * the pyramid mid-points are references - two of every four frames
       * at --bframes 3 - so six frames in flight fit; with --bframes 0
       * every frame is a reference and six do not (114 slot stalls in 120
       * frames, and 7% slower than four). Measured at eighteen threads,
       * 120 frames: K = 6 against K = 4 is -13% wall on park_joy_720p,
       * -14% on sintel_720p, -18% on sita_toon_720p, -6% on
       * crowd_run_1080p and -6% on foreman_cif, and +35 MB at 1080p
       * (198 -> 233, against sec 6's 300 MB ceiling). */
      int k = v ? atoi(v) : (e->bframes >= 2 ? 6 : 4);
      /* Phase 9 step 4. Six frames fill the pool only where one frame's
       * wavefront is wide enough to keep a fair share of the workers
       * busy. A CTB-row wavefront on an R x C grid cannot run more than
       * about min(R, (C + 1) / 2) rows at once (sec 1, and T1's scaling
       * table, where CIF stopped at 1.53x for exactly this reason), so
       * where TWO frames' worth of that bound still does not reach the
       * worker count, the pool is starved by the pictures and not by the
       * encoder: the step 4 census reads foreman_cif at 6.2 of 18 cores
       * held and 65% of the pool parked, against 17.5 of 18 on
       * touchdown_1080p.
       *
       * Eight frames in flight is where that ends - above it the DPB
       * slot pool binds instead (the JOIN census flips from `k` to
       * `slot` at 69-75 stalls in 120 frames and the wall stops moving),
       * which is T3/4's ceiling and is why the bump is to 8 and not
       * higher. Measured at eighteen threads, 120 frames, interleaved,
       * against K = 6: the CIF seven 0.72x to 0.99x (median 0.786x,
       * tempete 0.718x, foreman 0.784x), and the eight HD clips
       * 0.99x to 1.06x, i.e. the arm is a LOSS above the bound - sintel
       * +6.4%, sita_toon +1.9%. The predicate reproduces the sign on
       * every cell measured, including `--ctu 64` (foreman 0.728x and
       * bumped, sintel 1.050x and not) and `--threads 12` (foreman
       * 0.766x and bumped, sintel 1.034x and not), which a threshold on
       * the row count alone does not: at CTB 64 a 720p picture has
       * twelve rows and would have been bumped into its own regression.
       *
       * K is a pure scheduling knob wherever N265_RC_LAG is 0 (the
       * default), so this cannot move a bit; the identity set is the
       * claim and N265_MAX_FRAME_WORK still overrides. `--bframes < 2`
       * keeps 4: every frame is a reference there and the slot pool
       * binds at four already (T3/4). */
      if (!v && e->bframes >= 2) {
          int nt = g_threads > 0 ? g_threads : auto_threads();
          int cap = e->ctb_rows < (e->ctb_cols + 1) / 2
                        ? e->ctb_rows : (e->ctb_cols + 1) / 2;
          if (2 * cap <= nt)
              k = 8;
      }
      e->K = k < 1 ? 1 : k > N265_MAX_FRAME_WORK ? N265_MAX_FRAME_WORK : k; }
    { const char *v = n265_env("N265_RC_LAG");
      int d = v ? atoi(v) : 0;
      if (d < 0) d = 0;
      if (d > e->K - 1) d = e->K - 1;
      e->rc_lag = d; }

    for (int i = 0; i < 3; i++) {
        int w = i ? e->pad_w >> e->csx : e->pad_w;
        int h = i ? e->pad_h >> e->csy : e->pad_h;
        e->pad_stride[i] = w;
        /* the border, per axis: one number until 4:2:2 split csx from
         * csy (n265_ref_pad_x/y, and n265_ref_win's pair) */
        int px = n265_ref_pad_x(i, e->csx), py = n265_ref_pad_y(i, e->csy);
        /* only the slots this shape runs: a picture is the pool's whole
         * cost, so a shape that needs six allocates six however wide the
         * array is */
        for (int sl = 0; sl < e->dpb_slots + e->K; sl++) {
            struct n265_refpic *r = sl < e->dpb_slots
                ? &e->dpb[sl] : &e->scratch_pic[sl - e->dpb_slots];
            r->stride[i] = w + 2 * px;
            r->base[i] = malloc((size_t)r->stride[i] * (h + 2 * py)
                                * sizeof(pixel));
            if (!r->base[i])
                goto fail;
            r->plane[i] = r->base[i] + py * r->stride[i] + px;
        }
    }
    for (int sl = 0; sl < e->dpb_slots; sl++) {
        int c16 = ((e->pad_w + 15) >> 4) * ((e->pad_h + 15) >> 4);
        e->dpb[sl].col_pred = calloc((size_t)c16, 1);
        e->dpb[sl].col_mv = calloc((size_t)c16, 8);
        e->dpb[sl].col_ref = calloc((size_t)c16, 4);
        if (!e->dpb[sl].col_pred || !e->dpb[sl].col_mv || !e->dpb[sl].col_ref)
            goto fail;
        size_t nctu = (size_t)e->ctb_cols * e->ctb_rows;
        e->dpb[sl].ctu_dmin = malloc(nctu);
        e->dpb[sl].ctu_dmax = malloc(nctu);
        if (!e->dpb[sl].ctu_dmin || !e->dpb[sl].ctu_dmax)
            goto fail;
        memset(e->dpb[sl].ctu_dmin, 0xff, nctu);
        memset(e->dpb[sl].ctu_dmax, 0xff, nctu);
#ifdef N265_REF_CHECK
        e->dpb[sl].row_gen = calloc((size_t)e->ctb_rows, sizeof(unsigned));
        if (!e->dpb[sl].row_gen)
            goto fail;
#endif
    }
    e->map_stride = e->pad_w >> 2;
    for (int i = 0; i < e->K; i++)
        if (fw_open(e, &e->fw[i]) < 0)
            goto fail;
    {
        int nt = g_threads > 0 ? g_threads : auto_threads();
        /* Lanes, not workers: a worker that stalls on a reference row
         * runs an older frame's task NESTED (G3), and the nested run
         * needs its own row scratch. Nesting is bounded by the number of
         * live jobs, so nt * K lanes is the ceiling. Their scratch is
         * allocated on first use, because the nested lanes are rarely
         * touched. */
        e->pool = ntp_open(nt, e->K, nt * e->K);
    }
    if (!e->pool)
        goto fail;
    /* A1/A2: defer output only where there is something to overlap. */
    e->api_defer = n265_env("N265_API_DEFER") != NULL;
    {
        const char *v = n265_env("N265_SERIAL_PROF");
        e->sprof = v ? atoi(v) : 0;
    }
    e->la_prof = n265_env("N265_LA_PROF") != NULL || e->sprof;
    e->prof_t0 = e->la_prof ? prof_now() : 0.0;
    if (e->sprof)
        ntp_prof_enable(e->pool);
    {   /* T3/1: the slot preference, decided here from --bframes and
         * --ref alone. It is legal exactly while the marked set cannot
         * fill the pool, because then every claim finds an unmarked slot
         * - a picture in no RPS and no reference list - and which one it
         * takes is unobservable. At the shipping default (bframes 7,
         * ref 1) the peak is 5 of 6.
         *
         * Against the SIZED pool, and still the structural peak: where
         * the pool is the floor the test is the one it always was, and
         * where the pool is the peak itself the preference stays off -
         * which is what it was at those shapes before they had a pool
         * that fit them. Arming it there would cost one slot more. */
        e->slot_prefer = e->dpb_peak < e->dpb_slots;
    }
    e->defer_out = ntp_threads(e->pool) > 1 || e->api_defer;
    e->nlanes = ntp_lanes(e->pool);
    e->row = calloc((size_t)e->nlanes, sizeof(*e->row));
    if (!e->row)
        goto fail;
    for (int i = 0; i < ntp_threads(e->pool); i++)
        if (!(e->row[i].sc = n265_scratch_new()))
            goto fail;
    /* every cached knob and lazy table, initialized here, on this
     * thread, before a worker can be the one that writes it first */
    n265_tables_init();
    n265_tables_init_est();
    n265_encoder_warm();
    n265_la_warm();
    n265_ctu_warm();
    n265_mv_warm();
    n265_sao_warm();
    n265_rdoq_warm();
    n265_satd_warm();
    n265_entropy_warm();
    n265_deblock_warm();
    /* the CPU feature word and the N265_ASM_OFF ablation mask, resolved
     * here so the pthread_once and the n265_env read both happen on the
     * driver thread; every NEON dispatch site then sees only the inlined
     * acquire load */
    n265_cpu_flags();
    /* Phase 8 items 6a and 6b refused N265_ASM_MC as a class at every
     * format but 4:2:0: the MC family's chroma 2-D drivers held an
     * `int16_t tmp[(32 + 3) * 32]` - the widest chroma block a 64x64
     * luma PU can present when the plane is halved - while 4:4:4 reaches
     * 64x64 and 4:2:2 reaches 32x64, so a kernel's correctness rested on
     * a shape the caller no longer respected.
     *
     * Phase 11 item 3 takes the refusal back, and what it had to write
     * was the temp and nothing else. Every row driver under those
     * kernels already walks an arbitrary w and h down to a two-lane step
     * and a scalar remainder - that is what 4:2:0's w = 2 and w = 6
     * bought - so the wider blocks needed no new lane path, only a wider
     * temp, and it is now the format's rather than one size for all
     * three (mc_neon.c, MC_CTMP_SMALL / MC_CTMP_WIDE: 4:2:0 keeps the
     * frame it had and the wide arm is a function of its own). The class
     * runs at every chroma format and nothing here stands it down.
     *
     * The other five families never needed anything at either format:
     * transform, satd, cost, rdoq and intra take a width and a height or
     * see only coefficients. */
    n265_hf2_knobs(e);

    size_t frame_bytes = n265_frame_samples(e->pad_w, e->pad_h, e->csx,
                                            e->csy) * sizeof(pixel);
    /* A whole mini-GOP leaves per call, and the ring grows if it has to
     * (add_nal), so the payload pointers are patched from offsets at the
     * end of the call rather than being taken before a realloc. */
    e->out_cap = 3 * nal_pack_bound(frame_bytes + 4096) + 4096;
    e->out = malloc(e->out_cap);
    if (!e->out)
        goto fail;
    if (n265_la_init(e) < 0)
        goto fail;
    n265_rc_open(e);        /* after la_init: it needs the lowres geometry */
    if (twopass_open(e, p) < 0)
        goto fail;
    return e;

fail:
    n265_encoder_close(e);
    return NULL;
}

static void join_oldest(n265_encoder *e);
static void shot_fold_upto(n265_encoder *e, int upto, int hard);
static void shot_flush(n265_encoder *e);

void n265_encoder_close(n265_encoder *e)
{
    if (!e)
        return;
    /* A1: frames may still be in flight if the caller closed without
     * flushing. Their jobs own the frame_works and the pool. */
    while (e->n_inflight)
        join_oldest(e);
    /* --shot-stats: everything has retired, so the tail of the display
     * order can be folded and the last shot written. */
    if (e->shot_f) {
        shot_fold_upto(e, e->shot_high, 1);
        shot_flush(e);
        fclose(e->shot_f);
        e->shot_f = NULL;
    }
    /* --pass: the sidecar is finished (pass 3 renames its temporary onto
     * the file it read), and a pass-2 encode that outran its plan says so
     * once, here, rather than per frame. */
    n265_2p_close_write(&e->tp);
    if (e->tp.miss > 0)
        fprintf(stderr, "n265: --pass %d: %ld coded frames were not in the "
                "stats file (%d frames); those fell back to the one-pass "
                "loop\n", e->tp.pass, e->tp.miss, e->tp.n);
    n265_2p_free(&e->tp);
    if (e->la_prof) {
        double wall = prof_now() - e->prof_t0;
        double la = e->prof_push + e->prof_plan + e->prof_cutree;
        fprintf(stderr, "LAPROF wall=%.3f push=%.3f plan=%.3f cutree=%.3f "
                "la=%.3f serial=%.2f%%\n", wall, e->prof_push, e->prof_plan,
                e->prof_cutree, la, wall > 0 ? 100.0 * la / wall : 0.0);
    }
    /* N265_SERIAL_PROF: the two halves of the census. SPROF is the
     * DRIVER's own wall, split into the stages that are serial by
     * construction plus the time it is merely blocked in ntp_join;
     * SPOOL is what the workers did with the same wall. `serial` is
     * every driver column except the block, which is the quantity
     * Amdahl's rule is read off. */
    if (e->sprof) {
        double wall = prof_now() - e->prof_t0;
        double ser = e->prof_push + e->prof_plan + e->prof_cutree +
                     e->prof_dispatch + e->prof_retire + e->prof_emit;
        int nf = e->frames_done > 0 ? e->frames_done : 1;
        fprintf(stderr,
                "SPROF wall=%.3f frames=%d push=%.3f plan=%.3f cutree=%.3f "
                "dispatch=%.3f retire=%.3f emit=%.3f join=%.3f "
                "serial=%.3f serial_pct=%.2f ms_per_frame=%.3f\n",
                wall, e->frames_done, e->prof_push, e->prof_plan,
                e->prof_cutree, e->prof_dispatch, e->prof_retire,
                e->prof_emit, e->prof_join, ser,
                wall > 0 ? 100.0 * ser / wall : 0.0, 1000.0 * ser / nf);
        double busy = 0, stall = 0, sref = 0, park = 0, allpark = 0,
               pwall = 0;
        long tasks = 0;
        ntp_prof_read(e->pool, &busy, &stall, &sref, &park, &allpark,
                      &pwall, &tasks);
        int nt = ntp_threads(e->pool);
        /* `allpark` is the column the item turns on: wall seconds with
         * every worker parked, which is the ONLY state in which the
         * driver's serial work costs the encode anything. Driver work
         * done while the pool has queued tasks costs one core, not the
         * wall. */
        fprintf(stderr,
                "SPOOL threads=%d wall=%.3f busy=%.3f stall=%.3f "
                "stall_ref=%.3f park=%.3f allpark=%.3f tasks=%ld "
                "cores_held=%.2f cores_work=%.2f idle_pct=%.2f "
                "allpark_pct=%.2f\n",
                nt, pwall, busy, stall, sref, park, allpark, tasks,
                pwall > 0 ? busy / pwall : 0.0,
                pwall > 0 ? (busy - stall) / pwall : 0.0,
                pwall > 0 && nt ? 100.0 * (1.0 - busy / (pwall * nt)) : 0.0,
                pwall > 0 ? 100.0 * allpark / pwall : 0.0);
        fflush(stderr);
    }
    /* N265_HF2_TRACE: the round-4 traffic census, as the encoder itself
     * measured it. One row per phase: the samples that landed inside the
     * 2-D region in each form, the same as touches per cached pixel per
     * reference (the round-3 census's own quantity), and how many
     * references armed each plane. This is the instrument the arming
     * threshold is read off; it is output-neutral. */
    if (e->hf2_trace) {
        double area = (double)(e->hf2_area > 0 ? e->hf2_area : 1);
        double dn = (double)(e->hf2_nref ? e->hf2_nref : 1);
        fprintf(stderr, "HF2 area=%ld refs=%llu frames=%d\n", e->hf2_area,
                (unsigned long long)e->hf2_nref, e->hf2_code_n);
        fprintf(stderr, "HF2 fx fy samples8 tpp8 armed8 samples14 tpp14 "
                        "armed14\n");
        for (int i = 0; i < 12; i++)
            fprintf(stderr, "HF2ROW %d %d %llu %.3f %llu %llu %.3f %llu\n",
                    i & 3, (i >> 2) + 1,
                    (unsigned long long)e->hf2_samp[i],
                    (double)e->hf2_samp[i] / (area * dn),
                    (unsigned long long)e->hf2_armed[i],
                    (unsigned long long)e->hf2_samp[i + 12],
                    (double)e->hf2_samp[i + 12] / (area * dn),
                    (unsigned long long)e->hf2_armed[i + 12]);
        fflush(stderr);
    }
    if (n265_env("N265_DPB_TRACE"))
        fprintf(stderr, "DPB slots=%d overwrite=%ld slot_stall=%d\n",
                e->dpb_slots, e->slot_overwrite, e->join_slot);
    if (n265_env("N265_STALL_TRACE")) {
        fprintf(stderr, "JOIN k=%d slot=%d src=%d rc=%d\n",
                e->join_k, e->join_slot, e->join_src, e->join_rc);
        fprintf(stderr, "STALL frames=%ld stalls=%ld %.3f/frame\n",
                (long)atomic_load(&e->stall_frames),
                (long)atomic_load(&e->ref_stalls),
                atomic_load(&e->stall_frames)
                    ? (double)atomic_load(&e->ref_stalls) /
                      (double)atomic_load(&e->stall_frames) : 0.0);
    }
    if (e->cu_trace_f)
        fclose(e->cu_trace_f);
    n265_grain_close(e->fgc_est);
    free(e->cu_prev_skip);
    for (int i = 0; i < e->K; i++)
        fw_close(&e->fw[i]);
    for (int i = 0; i < e->nlanes; i++)
        n265_scratch_free(e->row[i].sc);
    free(e->row);
    ntp_close(e->pool);
    /* the frees walk the WHOLE array, not `e->dpb_slots`: the encoder is
     * calloc'd, an unused slot's pointers are NULL, and a close on the
     * open path's `fail` can land before the pool was sized at all. */
    for (int i = 0; i < 3; i++) {
        for (int sl = 0; sl < N265_DPB_SLOTS; sl++)
            free(e->dpb[sl].base[i]);
        for (int sl = 0; sl < N265_MAX_FRAME_WORK; sl++)
            free(e->scratch_pic[sl].base[i]);
    }
    for (int i = 0; i < 3; i++)
        free(e->hdr_rbsp[i]);
    for (size_t i = 0; i < sizeof(e->done) / sizeof(e->done[0]); i++)
        free(e->done[i].buf);
    for (int sl = 0; sl < N265_DPB_SLOTS; sl++) {
        free(e->dpb[sl].col_pred);
        free(e->dpb[sl].col_mv);
        free(e->dpb[sl].col_ref);
        free(e->dpb[sl].ctu_dmin);
        free(e->dpb[sl].ctu_dmax);
#ifdef N265_REF_CHECK
        free(e->dpb[sl].row_gen);
#endif
        for (int i = 0; i < 3; i++)
            free(e->dpb[sl].hf_base[i]);
        for (int i = 0; i < 12; i++) {
            free(e->dpb[sl].hf2_base[i]);
            free(e->dpb[sl].hf2w_base[i]);
        }
    }
    for (int i = 0; i < N265_SC_HIST; i++)
        free(e->sc_hist[i].map);   /* phase 11 item 4's skip-map ring */
    n265_la_free(e);
    bs_free(&e->hrd_bs);
    bs_free(&e->hash_bs);
    free(e->out);
    free(e);
}

/* D1: capture the short-term RPS on the driver, in coding order, right
 * after the reference lists are built and before anything else is
 * dispatched. Everything it reads - `marked`, `poc`, the just-built
 * lists - is driver-serial, so the snapshot is the set the SERIAL
 * encoder's header writer would have seen: nothing between dispatch and
 * the header write changes the marked set, which is the invariant T0a
 * already relied on for the slot claim.
 *
 * Keyed on POC throughout. The enumeration walks slots because that is
 * where the pictures live, but the ORDER is a full sort on POC and the
 * output carries POCs, so slot identity cannot reach the bitstream. */
static void snapshot_rps(n265_encoder *e, n265_frame_work *fw)
{
    struct n265_rps *r = &fw->rps;
    r->nneg = r->npos = 0;
    if (fw->idr)
        return;                 /* an IDR signals no short-term set */
    /* A non-IDR I signals one: it references nothing itself (every
     * `used` below comes out 0, its lists being empty), but the pictures
     * the rest of the plan still needs have to survive it, and a picture
     * leaves the decoder's DPB on the first RPS that omits it. */
    for (int sl = 0; sl < e->dpb_slots; sl++) {
        struct n265_refpic *p = &e->dpb[sl];
        if (!p->marked)         /* every kept picture is listed; eviction
                                 * is at mini-GOP ends */
            continue;
        int used = 0;
        for (int l = 0; l < 2; l++)
            for (int k = 0; k < fw->nref[l]; k++)
                if (fw->refl[l][k] == p)
                    used = 1;
        if (p->poc < fw->poc) {
            r->neg_poc[r->nneg] = p->poc;
            r->neg_used[r->nneg++] = (uint8_t)used;
        } else {
            r->pos_poc[r->npos] = p->poc;
            r->pos_used[r->npos++] = (uint8_t)used;
        }
    }
    /* nearest first: negatives by descending POC, positives by ascending.
     * POCs are unique among marked pictures (an IDR clears the set), so
     * there is no tie for slot order to break. */
    for (int i = 1; i < r->nneg; i++)
        for (int k = i; k > 0 && r->neg_poc[k] > r->neg_poc[k - 1]; k--) {
            int t = r->neg_poc[k]; r->neg_poc[k] = r->neg_poc[k - 1]; r->neg_poc[k - 1] = t;
            uint8_t u = r->neg_used[k]; r->neg_used[k] = r->neg_used[k - 1]; r->neg_used[k - 1] = u;
        }
    for (int i = 1; i < r->npos; i++)
        for (int k = i; k > 0 && r->pos_poc[k] < r->pos_poc[k - 1]; k--) {
            int t = r->pos_poc[k]; r->pos_poc[k] = r->pos_poc[k - 1]; r->pos_poc[k - 1] = t;
            uint8_t u = r->pos_used[k]; r->pos_used[k] = r->pos_used[k - 1]; r->pos_used[k - 1] = u;
        }
}

static void write_slice_header(n265_encoder *e, n265_frame_work *fw)
{
    (void)e;
    bitstream_t *bs = &fw->rbsp;
    bs_put_bits(bs, 1, 1);          /* first_slice_segment_in_pic_flag */
    if (fw->idr)
        bs_put_bits(bs, 0, 1);      /* no_output_of_prior_pics_flag (IRAP) */
    bs_put_ue(bs, 0);               /* slice_pic_parameter_set_id */
    bs_put_ue(bs, (uint32_t)fw->slice_type);
    /* 7.3.6.1 keys the POC and the RPS on the NAL type and not on the
     * slice type: a non-IDR I picture carries both, and its
     * slice_temporal_mvp_enabled_flag with them (an I slice parses no
     * collocated syntax, so the flag only says its own motion field may
     * be read later, and it is all-intra). */
    if (!fw->idr) {
        bs_put_bits(bs, (uint32_t)(fw->poc & 255), 8); /* pic_order_cnt_lsb */
        bs_put_bits(bs, 0, 1);      /* short_term_ref_pic_set_sps_flag */
        /* st_ref_pic_set(idx 0) inline, serialized from the dispatch-time
         * snapshot (D1). The writer does not touch the DPB: by the time
         * it runs, frames after this one may have been dispatched and
         * changed the marked set. */
        const struct n265_rps *rp = &fw->rps;
        bs_put_ue(bs, (uint32_t)rp->nneg);      /* num_negative_pics */
        bs_put_ue(bs, (uint32_t)rp->npos);      /* num_positive_pics */
        int prev = fw->poc;
        for (int i = 0; i < rp->nneg; i++) {
            bs_put_ue(bs, (uint32_t)(prev - rp->neg_poc[i] - 1)); /* delta_poc_s0_minus1 */
            bs_put_bits(bs, rp->neg_used[i], 1);   /* used_by_curr_pic_s0_flag */
            prev = rp->neg_poc[i];
        }
        prev = fw->poc;
        for (int i = 0; i < rp->npos; i++) {
            bs_put_ue(bs, (uint32_t)(rp->pos_poc[i] - prev - 1)); /* delta_poc_s1_minus1 */
            bs_put_bits(bs, rp->pos_used[i], 1);
            prev = rp->pos_poc[i];
        }
        bs_put_bits(bs, n265_env("N265_NO_TMVP") ? 0 : 1, 1);      /* slice_temporal_mvp_enabled_flag */
    }
    if (fw->mode != N265_MODE_PCM && !fw->lossless && n265_sao_seq_enabled()) {
        /* the content gate (N265_SAO_ANIM) answers per frame, so these
         * are the per-frame answer and the SPS flag above is only "it
         * may happen somewhere in this sequence" */
        uint32_t on = fw->sao_mask != 0;
        bs_put_bits(bs, on, 1);     /* slice_sao_luma_flag */
        bs_put_bits(bs, on, 1);     /* slice_sao_chroma_flag */
    }
    if (fw->slice_type != 2) {
        int override = fw->nref[0] != 1 || (fw->slice_type == 0 && fw->nref[1] != 1);
        bs_put_bits(bs, (uint32_t)override, 1);     /* num_ref_idx_active_override_flag */
        if (override) {
            bs_put_ue(bs, (uint32_t)(fw->nref[0] - 1));
            if (fw->slice_type == 0)
                bs_put_ue(bs, (uint32_t)(fw->nref[1] - 1));
        }
        if (fw->slice_type == 0)
            bs_put_bits(bs, 0, 1);  /* mvd_l1_zero_flag */
        if (!n265_env("N265_NO_TMVP")) {
            if (fw->slice_type == 0)
                bs_put_bits(bs, 1, 1);  /* collocated_from_l0_flag */
            if (fw->nref[0] > 1)
                bs_put_ue(bs, 0);       /* collocated_ref_idx: L0[0] */
        }
        bs_put_ue(bs, 5 - MAX_MERGE);   /* five_minus_max_num_merge_cand */
    }
    bs_put_se(bs, fw->qp - 26);      /* slice_qp_delta */
    /* 7.3.6.1: present only where some in-loop filter is actually
     * running - SAO on, or deblocking not disabled. PCM disables both in
     * the PPS and so does --lossless (8.7.2/8.7.3 would leave every
     * bypass sample unfiltered anyway), so both skip the flag. */
    if (fw->mode != N265_MODE_PCM && !fw->lossless)
        bs_put_bits(bs, 1, 1);      /* slice_loop_filter_across_slices_enabled
                                     * (present because deblocking is on) */
    bs_align_one_then_zero(bs);     /* byte_alignment() */
}

/* E1: the reference's cached horizontal first stage, three frame-sized
 * 8-tap passes once per stored reference. Every later subpel candidate
 * with a nonzero fx - which is most of them - then skips its own. Off
 * with N265_HFCACHE=0, which is the A/B arm and nothing else: the two
 * paths produce identical bytes. */
static int hf_cache_on(void)
{
    static int on = -1;
    if (on < 0) {
        const char *v = n265_env("N265_HFCACHE");
        on = v ? atoi(v) != 0 : 1;
    }
    return on;
}

/* Phase 6 round 3: finish the phase plane while the reference is being
 * published, so that every uni candidate landing on a cached phase
 * copies instead of running the vertical 8-tap. The round-3 census
 * (docs/phase6-neon.md) counted 0.2 to 67 touches per cached pixel
 * depending on clip and preset, against a break-even near 1.25 once the
 * copy that replaces the filter is a load and a store.
 *
 * Round 3 shipped the fy = 2 row because that was the set the WORST clip
 * admits, and held all twelve - worth 1.11-1.20x at --preset medium on
 * six clips of seven - because it reads 0.975x on sita_flat_720p. That
 * refusal was about the shape of the decision and not about the cache:
 * the fill is a fixed twelve passes per reference picture while the
 * traffic that amortises it is content, spanning 149 M samples to
 * 5820 M over the corpus at one preset. Round 4 makes the set a
 * per-reference decision taken from traffic the encoder measured on
 * frames it has already coded, so every clip gets the set its own
 * content pays for. N265_HF2 is still an A/B knob and nothing else:
 * every value writes the same bytes. Off with the E1 cache, which three
 * planes in four are filtered from.
 *
 * One u8 plane of the padded picture per cached phase, and one int16
 * plane at twice that for a phase whose 14-bit (bi) traffic clears its
 * own bar: at four u8 planes, 0.8 MB per reference at CIF, 4.7 at 720p,
 * 9.9 at 1080p - against the 6 bytes per padded pixel E1 already spends
 * on its three int16 planes. Above the cap the cache drops planes rather
 * than trade a working set for a copy. */
#define N265_HF2_MAX_BYTES (48u << 20)

/* ...and round 4's second bound, which is the one that bites. The first
 * is per reference, and there are `e->dpb_slots` of those (six at every
 * shape that shipped before the pool was sized, so the budget per
 * reference is the one round 4 measured): at 1080p
 * twelve u8 planes is 29.4 MB per reference and 176 MB over the pool,
 * which takes peak RSS from 241 MB to 410 (and with the int16 planes on
 * top, to 522) against sec 6's 300 MB ceiling. Measured, 20 frames of
 * crowd_run_1080p at one thread.
 *
 * So the cache gets a budget for the WHOLE pool, spent evenly: 64 MB,
 * which is what round 3's shipped default already cost at 1080p (four
 * planes per reference, 9.8 MB each over six slots). At 1080p that is
 * still four planes and the footprint does not move; at 720p the same
 * bytes buy nine; at CIF they buy every plane the traffic earns, int16
 * included, at 6.9 MB per reference. The budget is what makes the
 * arming rule two-sided - traffic says WHICH phases, the budget says
 * how many - and it is why a bigger picture gets fewer planes rather
 * than a bigger working set. */
#define N265_HF2_DPB_BYTES (64u << 20)

/* N265_HF2: 0 off, 1 the fy = 2 row (round 3's default), 2 all twelve
 * (round 3's held arm), 3 the traffic rule (the default since round 4).
 * Resolved into the encoder at open rather than into a static, because
 * the traffic rule reads two more knobs beside it and they belong
 * together; hf2_knobs is what n265_encoder_warm touches. */
static unsigned hf2_mode_env(void)
{
    static int m = -1;      /* an int with the -1 sentinel every other
                             * gate uses, so env_gate_audit.py sees it */
    if (m < 0) {
        const char *v = n265_env("N265_HF2");
        int k = v ? atoi(v) : 3;
        m = k < 0 ? 0 : k > 3 ? 3 : k;
    }
    return (unsigned)m;
}

/* The bar a phase has to clear to be filled, in HUNDREDTHS of a touch
 * per cached pixel, so the whole decision is integer.
 *
 * Break-even is 125, not 100: the fill costs one vertical pass per
 * cached pixel and a touch saves that pass MINUS the copy that replaces
 * it, which round 3 measured at about a fifth of it (1 / 0.8 = 1.25).
 * The round-3 census calibrates it at both ends - foreman at --preset
 * fast serves 1.48 touches per pixel across all twelve and read 1.008x,
 * sita_flat_720p serves 0.93 across the fy = 2 row and read 0.997x, and
 * 0.66 across all twelve and read 0.975x - so 125 is where the sign
 * changes and 150 is that with a fifth of margin on top. Taken from the
 * census table, never swept on the band.
 *
 * The 14-bit plane is twice the bytes for the same fill arithmetic, so
 * both the copy it saves and the store the fill makes are twice as wide:
 * the saving per touch falls to about two thirds of a pass and the bar
 * rises to 1 / 0.65 = 155, 190 with the same margin. */
static int hf2_tpp_env(void)
{
    static int t = -1;
    if (t < 0) {
        const char *v = n265_env("N265_HF2_TPP");
        int k = v ? atoi(v) : 150;
        t = k < 1 ? 1 : k;
    }
    return t;
}

static int hf2_tpp14_env(void)
{
    static int t = -1;
    if (t < 0) {
        const char *v = n265_env("N265_HF2_TPP14");
        int k = v ? atoi(v) : 190;
        t = k < 1 ? 1 : k;
    }
    return t;
}

void n265_hf2_knobs(n265_encoder *e)
{
    e->hf2_mode = hf2_mode_env();
    e->hf2_tpp = hf2_tpp_env();
    e->hf2_tpp14 = hf2_tpp14_env();
    e->hf2_trace = n265_env("N265_HF2_TRACE") != NULL;
    for (int i = 0; i < N265_HF2_HIST; i++)
        e->hf2_hist[i].cidx = -1;
}

/* Which planes to fill on the reference frame `k` is about to become, as
 * a bit per plane in the (fy - 1) * 4 + fx order the refpic stores them
 * in; bits 0..11 are the u8 planes and 12..23 the int16 ones.
 *
 * The window is the N265_HF2_WIN coding indices ending at k - K. Every
 * one of them is retired: a frame is dispatched only once n_inflight is
 * below K, so the joined prefix reaches at least k - K, and K is a
 * scheduling constant that does not move with the thread count. So the
 * set of frames this reads, and therefore the mask, is the same at one
 * worker and at twelve. The output would be identical either way - an
 * unarmed plane is simply absent and the caller filters instead - but
 * the WALL would not, and a rule whose answer drifted with --threads
 * could not be measured or reproduced.
 *
 * Inside the window, `samp` counts the luma MC output samples that fell
 * inside a 2-D plane's region, and `nref` the reference pictures the
 * window claimed. samp / (area * nref) is then touches per cached pixel
 * per reference over its lifetime - the round-3 census's own quantity,
 * since in steady state the references read inside a window and the ones
 * claimed in it are the same population. Nothing here is float: the test
 * is samp * 100 >= area * nref * bar.
 *
 * Before the window has an entry there is no measurement, and the answer
 * is round 3's shipped default - the fy = 2 row, the set that never lost
 * on any clip measured. */
/* N265_INTRA_GATE=10 (phase 9 step 3): does the frame about to be
 * dispatched at coding index `k` stand its intra arm down over the floor
 * population even though it is not a B slice?
 *
 * The window is the `iw_win` coding indices ending at k - K, the newest
 * index guaranteed retired when k is dispatched (hf2_traffic_mask's own
 * argument, and the reason there is no wait here): a frame is dispatched
 * only once n_inflight is below K, so the joined prefix reaches at least
 * k - K, and K is a scheduling constant that does not move with
 * --threads. Inside the window only frames of the SAME slice type that
 * actually RAN the arm are read - a gated frame measures nothing - and
 * the quantity is the J the intra arm saved over the population the rule
 * would gate, as a share of the J the inter arms would have settled for.
 * The share, not the count: docs/phase9-goals-plan.md sec 8 has the
 * census that says the intra SHARE of a coded anchor does not separate
 * the clips where the arm earns its wall from the ones where it does not,
 * and the oracle's own calibration was in J (docs/instruments.md,
 * N265_INTRA_ORACLE).
 *
 * With no armed frame of the type in the window there is no measurement
 * and the answer is mode 7's - which is also what keeps the rule honest
 * once it starts firing: a gated frame writes no measurement, so the
 * window empties, the rule stops firing, and the next frame of that type
 * measures again. The duty cycle is the window's own length and nothing
 * has to schedule a probe. */
static int iw_adapt_gate(n265_encoder *e, int k, int slice_type, int flat)
{
    uint64_t ji = 0, js = 0;
    if (!e->iw_adapt || slice_type == 2 || flat)
        return 0;
    for (int i = 0; i < e->iw_win; i++) {
        int c = k - e->K - i;
        if (c < 0)
            break;
        struct n265_iw_hist *ih = &e->iw_hist[c % N265_IW_HIST];
        if (ih->cidx != c || ih->slice_type != slice_type || !ih->armed)
            continue;
        ji += ih->j_inter;
        js += ih->j_saved;
    }
    if (!ji)
        return 0;
    return js * 1000 < ji * (uint64_t)e->iw_thr;
}

static unsigned hf2_traffic_mask(n265_encoder *e, int k)
{
    uint64_t samp[24] = { 0 };
    uint64_t nref = 0;
    int have = 0;
    for (int i = 0; i < N265_HF2_WIN; i++) {
        int c = k - e->K - i;
        if (c < 0)
            break;
        struct n265_hf2_hist *hh = &e->hf2_hist[c % N265_HF2_HIST];
        if (hh->cidx != c)      /* never written, or already overwritten */
            continue;
        have = 1;
        nref += (unsigned)hh->is_ref;
        for (int j = 0; j < 24; j++)
            samp[j] += hh->samp[j];
    }
    if (!have || !nref || e->hf2_area <= 0)
        return 0x0f0;
    /* The two forms are priced apart and decided apart: they are
     * independent stores of the same filter over the same hf[] rows, so
     * an int16 plane needs no u8 plane beside it and vice versa. */
    uint64_t bar = (uint64_t)e->hf2_area * nref;
    unsigned mask = 0;
    for (int j = 0; j < 24; j++) {
        int t = j < 12 ? e->hf2_tpp : e->hf2_tpp14;
        if (samp[j] * 100u >= bar * (unsigned)t)
            mask |= 1u << j;
    }
    return mask;
}

/* Arm the cache on a freshly claimed slot: the buffers and the region it
 * covers. Nothing is filled here - G4 makes the fill per row, inside the
 * publish task, so that a row's cache exists before final_rows advances
 * past it. Filling is safe there with no ref_wait of its own because the
 * horizontal 8-tap reads only along its OWN line: the cache for row r
 * publishes with row r, not with row r + 1. */
static void hf_arm(n265_encoder *e, struct n265_refpic *r, int code_idx)
{
    for (int i = 0; i < 3; i++)
        r->hf[i] = NULL;
    for (int i = 0; i < 12; i++) {
        r->hf2[i] = NULL;
        r->hf2w[i] = NULL;
    }
    /* The covered region, which the traffic tally tests against whether
     * or not anything is armed. Empty until the E1 cache proves out. */
    r->hf2_x0 = r->hf2_x1 = r->hf2_y0 = r->hf2_y1 = 0;
    if (!hf_cache_on())
        return;
    int w = e->pad_w + 2 * N265_HF_PAD, h = e->pad_h + 2 * N265_HF_PAD;
    r->hf_stride = w;
    r->hf2_stride = w;      /* set even when the 2-D planes do not arm:
                             * n265_ref_guard reads it unconditionally */
    for (int i = 0; i < 3; i++) {
        if (!r->hf_base[i]) {
            r->hf_base[i] = malloc((size_t)w * h * sizeof(int16_t));
            if (!r->hf_base[i]) {   /* no cache is a slow path, not a fail */
                for (int k = 0; k < 3; k++)
                    r->hf[k] = NULL;
                return;
            }
        }
        r->hf[i] = r->hf_base[i] + N265_HF_PAD * w + N265_HF_PAD;
    }
    r->hf_x0 = -N265_HF_PAD;
    r->hf_x1 = e->pad_w + N265_HF_PAD;
    r->hf_y0 = -N265_HF_PAD;
    r->hf_y1 = e->pad_h + N265_HF_PAD;

    /* Three rows short at the top and four at the bottom of hf[]'s
     * region: those are the lines the vertical filter reads, and there
     * is nothing above -N265_HF_PAD to read. Set before the planes are
     * armed, because the tally's question is about the region and not
     * about the planes. */
    r->hf2_x0 = -N265_HF_PAD;
    r->hf2_x1 = e->pad_w + N265_HF_PAD;
    r->hf2_y0 = -N265_HF_PAD + 3;
    r->hf2_y1 = e->pad_h + N265_HF_PAD - 4;
    e->hf2_area = (long)w * h;

    unsigned mask = e->hf2_mode == 0   ? 0u
                  : e->hf2_mode == 1   ? 0x0f0u
                  : e->hf2_mode == 2   ? 0xfffu
                  : hf2_traffic_mask(e, code_idx);
    /* The size gate: round 3's per-reference cap and round 4's pool
     * budget, whichever binds. It drops planes instead of refusing the
     * whole set, in a FIXED order - every u8 plane before any int16 one,
     * low phase index before high - so a picture whose traffic earns
     * everything still gets the u8 planes, which carry four to five
     * times the touches for half the bytes. The order is fixed and not a
     * function of the traffic, so the survivors stay a deterministic
     * function of the mask. */
    {
        uint64_t cap = N265_HF2_DPB_BYTES / (unsigned)e->dpb_slots;
        if (cap > N265_HF2_MAX_BYTES)
            cap = N265_HF2_MAX_BYTES;
        uint64_t bytes = 0;
        for (int i = 0; i < 24; i++) {
            if (!(mask & (1u << i)))
                continue;
            uint64_t sz = (uint64_t)w * h * (i >= 12 ? 2 : 1);
            if (bytes + sz > cap)
                mask &= ~(1u << i);
            else
                bytes += sz;
        }
    }
    /* Give back what this claim did NOT earn. The planes are kept across
     * claims so that a steady mask costs one malloc per plane per slot
     * and nothing after, but a mask that moves would otherwise leave the
     * union of every set the slot ever armed resident - which is how the
     * pool budget above gets spent twice. Freeing the unselected ones
     * makes the retained bytes exactly the armed bytes. */
    for (int i = 0; i < 12; i++) {
        if (!(mask & (1u << i)) && r->hf2_base[i]) {
            free(r->hf2_base[i]);
            r->hf2_base[i] = NULL;
        }
        if (!(mask & (1u << (i + 12))) && r->hf2w_base[i]) {
            free(r->hf2w_base[i]);
            r->hf2w_base[i] = NULL;
        }
    }
    for (int i = 0; i < 12; i++) {
        if (!(mask & (1u << i)))
            continue;
        if (!r->hf2_base[i]) {
            r->hf2_base[i] = malloc((size_t)w * h * sizeof(pixel));
            if (!r->hf2_base[i])    /* no cache is a slow path, not a fail */
                break;
        }
        r->hf2[i] = r->hf2_base[i] + N265_HF_PAD * w + N265_HF_PAD;
        e->hf2_armed[i]++;
    }
    for (int i = 0; i < 12; i++) {
        if (!(mask & (1u << (i + 12))))
            continue;
        if (!r->hf2w_base[i]) {
            r->hf2w_base[i] = malloc((size_t)w * h * sizeof(int16_t));
            if (!r->hf2w_base[i])
                break;
        }
        r->hf2w[i] = r->hf2w_base[i] + N265_HF_PAD * w + N265_HF_PAD;
        e->hf2_armed[i + 12]++;
    }
}

/* The E1 cache for the luma lines CTB row r owns, plus the padding rows
 * above (row 0) and below (the last row), which are replications of
 * lines this row has just border-extended. */
static void hf_fill_rows(n265_frame_work *fw, struct n265_refpic *r, int cy,
                         int last)
{
    if (!r->hf[0])
        return;
    int ctb = 1 << fw->ctb_log2;
    int w = fw->pad_w + 2 * N265_HF_PAD;
    int rs = r->stride[0];
    int y0 = cy * ctb, y1 = y0 + ctb;
    if (y1 > fw->pad_h)
        y1 = fw->pad_h;
    if (cy == 0)
        y0 = -N265_HF_PAD;
    if (last)
        y1 = fw->pad_h + N265_HF_PAD;
    const pixel *src = r->plane[0] + y0 * rs - N265_HF_PAD;
    N265_REF_OPEN_ALL();
    for (int i = 0; i < 3; i++)
        /* ptrdiff_t, not size_t: y0 is -N265_HF_PAD on the first row
         * and (size_t)(-60) * w wraps, which is UB even though the
         * wrapped result addresses the right sample. */
        n265_mc_luma_hplane(r->hf[i] + (ptrdiff_t)y0 * w - N265_HF_PAD, w,
                            w, y1 - y0, src, rs, i + 1);
    /* Round 3's vertical half, lagged four lines behind the horizontal
     * one. Output line y wants source lines y-3..y+4 and this call has
     * just reached line y1 - 1, so the rows that can be finished now end
     * at y1 - 4; line y therefore publishes with the CTB row that
     * contains y + 4. Every reader already waits for iy + h + 4 before
     * it looks (the 8-tap's own bound, E1's too), which is exactly the
     * row that finished line iy + h - 1 - and n265_ref_guard's
     * `lastl - 4` proves it from the other end under N265_REF_CHECK. */
    {
        int g0 = cy == 0 ? -N265_HF_PAD + 3 : y0 - 4;
        int g1 = y1 - 4;
        if (g1 > g0) {
            ptrdiff_t off = (ptrdiff_t)g0 * w - N265_HF_PAD;
            for (int i = 0; i < 12; i++) {
                int fx = i & 3, fy = (i >> 2) + 1;
                if (r->hf2[i]) {
                    if (!fx)  /* no horizontal stage to reuse: straight u8 */
                        n265_mc_luma_wh(r->hf2[i] + off, w, w, g1 - g0,
                                        r->plane[0] + (ptrdiff_t)g0 * rs
                                            - N265_HF_PAD, rs, 0, fy);
                    else
                        n265_mc_luma_hf(r->hf2[i] + off, w, w, g1 - g0,
                                        r->plane[0], rs,
                                        r->hf[fx - 1] + off, w, fx, fy);
                }
                /* Round 4's int16 twin: the same filter over the same
                 * rows, stopped one shift short of the u8 one. */
                if (r->hf2w[i]) {
                    if (!fx)
                        n265_mc_luma_14_wh(r->hf2w[i] + off, w, w, g1 - g0,
                                           r->plane[0] + (ptrdiff_t)g0 * rs
                                               - N265_HF_PAD, rs, 0, fy);
                    else
                        n265_mc_luma_14_hf(r->hf2w[i] + off, w, w, g1 - g0,
                                           r->plane[0], rs,
                                           r->hf[fx - 1] + off, w, fx, fy);
                }
            }
        }
    }
    N265_REF_UNGUARD();
}

static void write_pcm_ctu(n265_frame_work *e, cabac_t *cb, cabac_ctx_t *part_mode,
                          int ctb_x, int ctb_y)
{
    bitstream_t *bs = &e->rbsp;
    int ctb = 1 << e->ctb_log2;
    n265_cabac_bin(cb, part_mode, 1);
    n265_cabac_terminate(cb, 1);
    bs_align_zero(bs);              /* pcm_alignment_zero_bit */

    int x0 = ctb_x * ctb;
    int y0 = ctb_y * ctb;
    const pixel *l = e->pad_plane[0] + y0 * e->pad_stride[0] + x0;
    /* pcm_sample_bit_depth_luma/chroma: we code samples at full depth */
    for (int y = 0; y < ctb; y++)
        for (int x = 0; x < ctb; x++)
            bs_put_bits(bs, l[y * e->pad_stride[0] + x], BIT_DEPTH);
    for (int p = 1; p <= 2; p++) {
        const pixel *c = e->pad_plane[p] +
            (y0 >> e->csy) * e->pad_stride[p] + (x0 >> e->csx);
        for (int y = 0; y < ctb >> e->csy; y++)
            for (int x = 0; x < ctb >> e->csx; x++)
                bs_put_bits(bs, c[y * e->pad_stride[p] + x], BIT_DEPTH);
    }
    n265_cabac_start(cb, bs);
}

/* The deferred-NAL ring. Frames finish out of order under overlap, and a
 * resource stall can force one to be joined long before the API call
 * that owes it runs. So a joined frame's NALs are packed HERE, in coding
 * order, and copied into the caller's buffer only when that call comes
 * round; the driver is then free to run as far ahead as K allows without
 * the API returning frames early. */
static struct n265_done *done_slot(n265_encoder *e)
{
    int n = (int)(sizeof(e->done) / sizeof(e->done[0]));
    struct n265_done *d = &e->done[(e->done_head + e->n_done) % n];
    d->len = 0;
    d->nnal = 0;
    return d;
}

static void pack_nal(struct n265_done *d, int type, const uint8_t *rbsp,
                     size_t len)
{
    if (d->nnal >= (int)(sizeof(d->type) / sizeof(d->type[0])))
        return;
    size_t need = d->len + nal_pack_bound(len) + 64;
    if (need > d->cap) {
        uint8_t *b = realloc(d->buf, need);
        if (!b)
            return;
        d->buf = b;
        d->cap = need;
    }
    size_t sz = nal_pack(d->buf + d->len, d->cap - d->len, type, rbsp, len);
    d->type[d->nnal] = type;
    d->size[d->nnal] = (int)sz;
    d->nnal++;
    d->len += sz;
}

/* Copy up to `max` joined frames into the caller's buffer, oldest first.
 * The buffer grows, so payload POINTERS are patched from offsets at the
 * end of the call rather than taken before a realloc that may move them. */
static void emit_done(n265_encoder *e, int max, size_t *out_used, int *nnal)
{
    int n = (int)(sizeof(e->done) / sizeof(e->done[0]));
    int cap_nal = (int)(sizeof(e->nal) / sizeof(e->nal[0]));
    while (max-- > 0 && e->n_done > 0) {
        struct n265_done *d = &e->done[e->done_head];
        if (*nnal + d->nnal > cap_nal)
            break;
        if (*out_used + d->len > e->out_cap) {
            size_t want = *out_used + d->len;
            size_t cap = e->out_cap * 2 > want ? e->out_cap * 2 : want;
            uint8_t *o = realloc(e->out, cap);
            if (!o)
                break;
            e->out = o;
            e->out_cap = cap;
        }
        memcpy(e->out + *out_used, d->buf, d->len);
        size_t off = *out_used;
        for (int i = 0; i < d->nnal; i++) {
            e->nal[*nnal].type = d->type[i];
            e->nal[*nnal].payload = NULL;   /* patched at end of call */
            e->nal[*nnal].size = d->size[i];
            e->nal_off[*nnal] = off;
            off += (size_t)d->size[i];
            (*nnal)++;
        }
        *out_used += d->len;
        e->done_head = (e->done_head + 1) % n;
        e->n_done--;
    }
}

/* Stash a header RBSP written into fw->rbsp; it is packed when the frame
 * that carries it retires, so it lands ahead of that frame's slice. */
static void stash_header(n265_encoder *e, int type, n265_frame_work *fw)
{
    int i = e->nhdr;
    if (i >= 3)
        return;
    uint8_t *b = realloc(e->hdr_rbsp[i], fw->rbsp.bytes + 1);
    if (!b)
        return;
    memcpy(b, fw->rbsp.buf, fw->rbsp.bytes);
    e->hdr_rbsp[i] = b;
    e->hdr_len[i] = fw->rbsp.bytes;
    e->hdr_type[i] = type;
    e->nhdr = i + 1;
}

static void dump_planes(n265_frame_work *fw, FILE *f)
{
    /* PCM writes no reconstruction; its output IS the padded source, and
     * that plane set has the unpadded stride, not the DPB slot's. */
    int pcm = fw->mode == N265_MODE_PCM;
    for (int i = 0; i < 3; i++) {
        int w = i ? fw->param.width >> fw->csx : fw->param.width;
        int h = i ? fw->param.height >> fw->csy : fw->param.height;
        int stride = pcm ? fw->pad_stride[i] : fw->recon_stride[i];
        const pixel *pl = pcm ? fw->pad_plane[i] : fw->recon[i];
        for (int y = 0; y < h; y++)
            fwrite(pl + y * stride, sizeof(pixel), (size_t)w, f);
    }
}

/* The decoded picture hash of one finished frame (hash.h, D.3.19).
 *
 * It walks the CODED picture - pad_w x pad_h, the conformance window's
 * padding included - and not what --dump-recon writes, because that is
 * what pic_width_in_luma_samples says the picture is and what a decoder
 * will have reconstructed. The two differ on exactly the vectors whose
 * dimensions are not a multiple of the minimum coding block, and getting
 * it wrong there would be invisible everywhere else.
 *
 * PCM writes no reconstruction and its decoded picture IS the padded
 * source, which is the same exception dump_planes makes. */
static void hash_frame(n265_encoder *e, n265_frame_work *fw,
                       n265_pic_hash *h)
{
    int pcm = fw->mode == N265_MODE_PCM;
    memset(h, 0, sizeof *h);
    h->type = e->sig.hash - 1;      /* --hash is x265's numbering */
    for (int i = 0; i < 3; i++) {
        int w = i ? fw->pad_w >> fw->csx : fw->pad_w;
        int hgt = i ? fw->pad_h >> fw->csy : fw->pad_h;
        const pixel *pl = pcm ? fw->pad_plane[i] : fw->recon[i];
        int stride = pcm ? fw->pad_stride[i] : fw->recon_stride[i];
        n265_hash_plane(h, i, pl, stride, w, hgt);
    }
}

static void dump_recon_frame(n265_encoder *e, n265_frame_work *fw)
{
    if (e->dump_recon) {
        /* display order: frames land at their display index (B frames
         * are encoded out of order, so seek) */
        /* BYTES, and dump_planes writes sizeof(pixel) per sample, so the
         * sample width belongs in here: at Main10 a frame is twice as
         * long as its sample count, and without this every frame after
         * the first landed at half its offset and a reordered B frame
         * overwrote what was already there. The recon dump IS the
         * conformance gate's reference, so this one is load-bearing. */
        long frame_bytes =
            (long)n265_frame_samples(e->param.width, e->param.height,
                                     e->csx, e->csy) * (long)sizeof(pixel);
        fseek(e->dump_recon, (long)fw->display_idx * frame_bytes, SEEK_SET);
        dump_planes(fw, e->dump_recon);
    }
    if (e->dump_decode)
        dump_planes(fw, e->dump_decode);
}

/* The exact reference stall (G2). The fast path is inline in
 * encoder.h; this is what happens when it misses: count it, then run
 * ready tasks of strictly OLDER frames until the watermark arrives. */
struct ref_stall_ctx {
    const struct n265_refpic *pic;
    int need;
};

static int ref_stall_pred(void *ctx)
{
    struct ref_stall_ctx *w = ctx;
    return n265_ref_have(w->pic) >= w->need;
}

void n265_ref_stall(n265_frame_work *e, const struct n265_refpic *pic,
                    int need)
{
    n265_encoder *enc = e->enc;
    atomic_fetch_add_explicit(&enc->ref_stalls, 1, memory_order_relaxed);
    struct ref_stall_ctx w = { pic, need };
    ntp_wait_older(enc->pool, ref_stall_pred, &w);
}

/* Publish CTB row `cy` of this frame's picture (G4). Everything a later
 * frame can read from it is written HERE, before final_rows advances
 * past the row: the row's border extension (and, on the first and last
 * rows, the padding above and below it), the E1 half-pel cache for the
 * row's lines, the TMVP motion field for the 16x16 cells the row covers,
 * and the P2 per-CTU depth range. The whole-frame publish pass that used
 * to do all of this after entropy is gone, and that deletion is the
 * proof that nothing was missed: every write it did has a row that owns
 * it now. */
static void publish_row(n265_frame_work *fw, int cy)
{
    struct n265_refpic *slot = fw->slot;
    int ctb = 1 << fw->ctb_log2;
    int last = cy == fw->ctb_rows - 1;

    if (fw->mode != N265_MODE_PCM && !fw->nonref) {
        for (int i = 0; i < 3; i++) {
            int shx = i ? fw->csx : 0, shy = i ? fw->csy : 0;
            int w = fw->pad_w >> shx, h = fw->pad_h >> shy;
            int px = n265_ref_pad_x(i, fw->csx);
            int py = n265_ref_pad_y(i, fw->csy);
            int rs = slot->stride[i];
            int y0 = (cy * ctb) >> shy;
            int y1 = y0 + (ctb >> shy);
            if (y1 > h)
                y1 = h;
            for (int y = y0; y < y1; y++) {
                pixel *d = slot->plane[i] + y * rs;
                /* a SAMPLE fill: memset stops working the moment the
                 * pixel is wider than a byte */
                pixel lf = d[0], rt = d[w - 1];
                for (int x = 1; x <= px; x++) {
                    d[-x] = lf;
                    d[w - 1 + x] = rt;
                }
            }
            if (cy == 0)
                for (int y = 1; y <= py; y++)
                    memcpy(slot->plane[i] - y * rs - px,
                           slot->plane[i] - px, (size_t)rs * sizeof(pixel));
            if (last)
                for (int y = 1; y <= py; y++)
                    memcpy(slot->plane[i] + (h - 1 + y) * rs - px,
                           slot->plane[i] + (h - 1) * rs - px,
                           (size_t)rs * sizeof(pixel));
        }
        hf_fill_rows(fw, slot, cy, last);
        {
            int c16w = (fw->pad_w + 15) >> 4;
            int c16h = (fw->pad_h + 15) >> 4;
            int r0 = (cy * ctb) >> 4;
            int r1 = r0 + (ctb >> 4);
            if (r1 > c16h)
                r1 = c16h;
            for (int y16 = r0; y16 < r1; y16++)
                for (int cx = 0; cx < c16w; cx++) {
                    int m = (y16 * 4) * fw->map_stride + cx * 4;
                    int i16 = y16 * c16w + cx;
                    if (fw->slice_type == 2 || fw->blk_intra[m]) {
                        slot->col_pred[i16] = 0;
                        continue;
                    }
                    slot->col_pred[i16] = fw->blk_pred[m];
                    memcpy(slot->col_mv + 4 * i16, fw->blk_mv + 4 * m, 8);
                    for (int l = 0; l < 2; l++) {
                        if (fw->blk_pred[m] & (1 << l))
                            N265_SIDE_CHECK(fw,
                                fw->refl[l][fw->blk_ref[2 * m + l]],
                                fw->ref_gen[l][fw->blk_ref[2 * m + l]], -1,
                                "publish col_ref poc");
                        slot->col_ref[2 * i16 + l] = (fw->blk_pred[m] & (1 << l))
                            ? (int16_t)fw->refl[l][fw->blk_ref[2 * m + l]]->poc : 0;
                    }
                }
        }
        /* P2: carry this row's per-CTU depth range with the picture, so
         * a later frame can read the collocated range */
        memcpy(slot->ctu_dmin + (size_t)cy * fw->ctb_cols,
               fw->ctu_dmin + (size_t)cy * fw->ctb_cols,
               (size_t)fw->ctb_cols);
        memcpy(slot->ctu_dmax + (size_t)cy * fw->ctb_cols,
               fw->ctu_dmax + (size_t)cy * fw->ctb_cols,
               (size_t)fw->ctb_cols);
    }
#ifdef N265_REF_CHECK
    /* before the release store, so an acquire load of final_rows makes
     * this visible to the side-data check */
    if (slot->row_gen)
        slot->row_gen[cy] = slot->gen;
#endif
    atomic_store_explicit(&slot->final_rows, cy + 1, memory_order_release);
}

static void entropy_frame(n265_encoder *e, n265_frame_work *fw);

/* The frame's task table (docs/threading-design.md sec 1, G4). Indices:
 * [0, R)      encode row r
 * [R, 2R)     QP-map fill + deblock row r
 * [2R, 3R)    SAO row r (when the filter is on) then PUBLISH row r
 * 3R          entropy
 * The third kind exists whether or not SAO does: the filter is off by
 * default and the publish still has to happen per row. R is 0 on the PCM
 * path, which is the entropy task alone. */

/* Row r's CTB x may run when row r-1 has committed CTB x+1, the 2-CTB
 * WPP lag: intra's top-right neighbour and the estimator snapshot both
 * live under it. */
struct lag_wait {
    struct n265_prog *p;
    int r, need;
};

static int lag_pred(void *ctx)
{
    struct lag_wait *w = ctx;
    return atomic_load_explicit(&w->p->enc_prog[w->r - 1],
                                memory_order_acquire) >= w->need;
}

static int job_ready(void *ctx, int idx)
{
    n265_frame_work *fw = ctx;
    struct n265_prog *p = fw->prog;
    int R = fw->job_R;
    if (idx < R) {
        int r = idx;
        if (r > 0) {
            int need = fw->job_snap_x + 1 > 2 ? fw->job_snap_x + 1 : 2;
            if (need > fw->job_cols) need = fw->job_cols;
            if (atomic_load_explicit(&p->enc_prog[r - 1],
                                     memory_order_acquire) < need)
                return 0;
        }
        /* G1: sec 1's row-start gate, demoted to a scheduling hint. It
         * keeps a row from starting only to stall at its first motion
         * probe; it guarantees NOTHING (the T2/3 reach table), the
         * ref_wait at every read is what makes the row safe, and as a
         * hint it cannot change bits. */
        if (fw->ref_lag) {
            int nr = r + fw->ref_lag;
            if (nr > R) nr = R;
            for (int l = 0; l < 2; l++)
                for (int k = 0; k < fw->nref[l]; k++)
                    if (n265_ref_have(fw->refl[l][k]) < nr)
                        return 0;
        }
        return 1;
    }
    if (idx < 2 * R) {
        int r = idx - R;
        int need = r + 2 < R ? r + 2 : R;
        return atomic_load_explicit(&p->dbk_rows, memory_order_acquire) == r &&
               atomic_load_explicit(&p->enc_rows, memory_order_acquire) >= need;
    }
    if (idx < 3 * R) {
        int r = idx - 2 * R;
        int need = r + 2 < R ? r + 2 : R;
        return atomic_load_explicit(&p->pub_rows, memory_order_acquire) == r &&
               atomic_load_explicit(&p->dbk_rows, memory_order_acquire) >= need;
    }
    return atomic_load_explicit(&p->pub_rows, memory_order_acquire) == R;
}

static void job_run(void *ctx, int lane, int idx)
{
    n265_frame_work *fw = ctx;
    n265_encoder *e = fw->enc;
    struct n265_prog *p = fw->prog;
    int R = fw->job_R;

    if (idx < R) {
        int r = idx;
        if (e->sprof >= 2 && r == 0)
            fw->t_row0 = prof_now() - e->prof_t0;
        n265_row_ctx *row = &e->row[lane];
        if (!row->sc)       /* a nested lane, allocated on first use */
            row->sc = n265_scratch_new();
        n265_row_begin(row, fw, r);
        /* The CTB count that puts the row BELOW over its lag, i.e. the
         * one publish in this row that can make a task claimable
         * (job_ready's own expression). Every other CTB can only satisfy
         * a milestone wait, and saying so is what keeps the wake off the
         * per-CTB path (T3/3). */
        int open_at = fw->job_snap_x + 1 > 2 ? fw->job_snap_x + 1 : 2;
        if (open_at > fw->job_cols)
            open_at = fw->job_cols;
        for (int cx = 0; cx < fw->job_cols; cx++) {
            if (r > 0) {
                int need = cx + 2 < fw->job_cols ? cx + 2 : fw->job_cols;
                struct lag_wait w = { p, r, need };
                ntp_wait(e->pool, lag_pred, &w);
            }
            n265_analyze_ctu(&row->c, cx, r);
            if (cx == fw->job_snap_x)
                memcpy(fw->est_snap + (size_t)r * CTX_COUNT, row->c.est_ctx,
                       CTX_COUNT * sizeof(cabac_ctx_t));
            atomic_store_explicit(&p->enc_prog[r], cx + 1,
                                  memory_order_release);
            if (cx + 1 == open_at)
                ntp_publish_task(e->pool);
            else
                ntp_publish(e->pool);
        }
        n265_row_end(row, fw, r);
        /* rows complete in index order (a row's last CTB waits for the
         * row above to be complete), so the count IS the watermark */
        atomic_fetch_add_explicit(&p->enc_rows, 1, memory_order_release);
        ntp_publish_task(e->pool);
        return;
    }
    if (idx < 2 * R) {
        int r = idx - R;
        n265_qpmap_row(fw, r, &fw->qprev);
        n265_deblock_row(fw, r);
        atomic_store_explicit(&p->dbk_rows, r + 1, memory_order_release);
        ntp_publish_task(e->pool);
        return;
    }
    if (idx < 3 * R) {
        int r = idx - 2 * R;
        if (fw->job_sao_on) {
            if (e->no_sao)
                memset(&fw->sao[r * fw->ctb_cols], 0,
                       (size_t)fw->ctb_cols * sizeof(sao_params));
            n265_sao_row(fw, r);
        }
        publish_row(fw, r);
        /* This one also advances the picture's final_rows, which every
         * OTHER frame's row-start hint and reference stall reads. */
        atomic_store_explicit(&p->pub_rows, r + 1, memory_order_release);
        if (e->sprof >= 2 && r == R - 1)
            fw->t_rowlast = prof_now() - e->prof_t0;
        ntp_publish_task(e->pool);
        return;
    }
    if (e->sprof >= 2)
        fw->t_ent0 = prof_now() - e->prof_t0;
    entropy_frame(e, fw);
    if (e->sprof >= 2)
        fw->t_ent1 = prof_now() - e->prof_t0;
}

/* The QP a frame of this kind WILL be coded at, for the lookahead's bits
 * model (`--b-adapt 4`, n265_la_brun_bits): the same base and the same
 * fixed cascade dispatch_frame applies below, sitting next to it so the
 * two are read together and move together.
 *
 * What it leaves out is everything that does not exist before the frame
 * is typed - the cu-tree frame term, the AQ frame mean, the per-CTU
 * offsets, the base's carried fraction and the flat branch. That is
 * sound here because the model ranks candidate RUN LENGTHS against each
 * other and the omitted terms are frame properties the run length does
 * not choose; what the run length does choose is the CASCADE each leaf
 * sits at, and the cascade is the whole of what this returns. Under ABR
 * the base is the rate factor rc.c last solved, which is the only
 * forward estimate the driver has. */
int n265_planned_qp(const n265_encoder *e, int slice_type, int b_depth,
                    int nonref)
{
    double base = e->qp_base;
    if (e->abr_on) {
        base = e->rc_rf;
    } else if (e->crf_on) {
        double shift = 0.0;
        if (e->cutree) { const char *v = n265_env("N265_CRF_SHIFT"); shift = v ? atof(v) : 5.4; }
        base = e->crf + shift;
    }
    double pb = 1.0; { const char *v = n265_env("N265_PB_STEP"); if (v) pb = atof(v); }
    int cascade = slice_type == 2 ? -3
                : slice_type == 0 ? (int)lround(1 + b_depth * pb) : 0;
    if (slice_type == 0 && (e->crf_on || e->abr_on) && e->pb_cascade_on)
        cascade = nonref ? e->pb_cascade_l : e->pb_cascade_r;
    int q = (int)lround(base) + cascade;
    return n265_qp_clip(q);
}

/* The lowres samples one frame's distance is summed over, which is what
 * the per-pixel threshold is expressed in: the cost loop walks bw x bh
 * blocks of 64 lowres pixels and anchors the last row and column inside
 * the plane, so a few pixels are counted twice and the divisor is the
 * loop's own, not lw x lh. */
static uint64_t dup_samples(const n265_encoder *e)
{
    return (uint64_t)e->la.bw * e->la.bh * 64;
}

/* The lowres distance between this frame and a reference, bounded from
 * the chain's running totals (la.h): |cum_a - cum_b| is the sum of every
 * consecutive step between them, which by the triangle inequality is at
 * least their own distance. Exact (and zero) on an identical run. */
static uint64_t dup_bound(const n265_frame_work *fw,
                          const struct n265_refpic *p)
{
    return fw->dup_cum > p->dup_cum ? fw->dup_cum - p->dup_cum
                                    : p->dup_cum - fw->dup_cum;
}

/* N265_STATIC_COMMIT (phase 11 item 4): WHICH retired frame's coded-skip
 * map this picture's static commit reads, resolved on the driver at
 * dispatch and frozen before a row job exists - the same shape as
 * dup_resolve below and for the same reason.
 *
 * Two rules, and the second is the one that makes the first usable.
 *
 * ELIGIBLE is the CLOSED window k - 8 - 15 .. k - 8, in coding indices.
 * The upper end is the newest index retired at EVERY thread count;
 * `e->K` would be newer and is not allowed, because K moves with
 * --threads and this rule moves bits. The lower end is what keeps the
 * RING from leaking the thread count back in - encoder.h, at
 * N265_SC_WIN, has that argument, and it is the one this arm got wrong
 * first time.
 *
 * NEAREST IN DISPLAY ORDER among the eligible, because coding order at
 * --bframes 7 is not time. The census's feature is "the co-located CU of
 * the previous frame was skip" (docs/phase9-goals-plan.md sec 11.4) and
 * what makes that predict anything is temporal proximity, not the order
 * the encoder happened to code in. The oldest eligible coding index is
 * eight coding steps back; the nearest of the eligible set in display
 * order is usually one or two frames away. Ties go to the larger coding
 * index, so the answer is a pure function of the GOP structure. */
static void sc_resolve(n265_encoder *e, n265_frame_work *fw)
{
    fw->sc_map = NULL;
    if (!n265_static_commit() || fw->slice_type == 2 || !fw->ref_valid ||
        fw->mode == N265_MODE_PCM || fw->lossless || !e->cutree ||
        !fw->la_prop)
        return;
    int hi = fw->code_idx - N265_MAX_FRAME_WORK;
    int lo = hi - (N265_SC_WIN - 1);
    int best = -1, bd = 0;
    for (int i = 0; i < N265_SC_HIST; i++) {
        const struct n265_sc_hist *h = &e->sc_hist[i];
        if (h->cidx < lo || h->cidx > hi || !h->map)
            continue;
        int d = h->didx > fw->display_idx ? h->didx - fw->display_idx
                                          : fw->display_idx - h->didx;
        if (best < 0 || d < bd || (d == bd && h->cidx > e->sc_hist[best].cidx)) {
            best = i;
            bd = d;
        }
    }
    if (best < 0)
        return;
    /* the propagate conjunct, snapshotted here for the reason the field's
     * comment gives: the array it is read off is the lookahead's own and
     * the driver rewrites it as the window slides. */
    int nb = fw->la_bw * fw->la_bh;
    if (nb <= 0)
        return;
    if (fw->sc_prop_n != nb) {
        free(fw->sc_prop);
        fw->sc_prop = malloc((size_t)nb);
        fw->sc_prop_n = fw->sc_prop ? nb : 0;
    }
    if (!fw->sc_prop)
        return;
    for (int b = 0; b < nb; b++)
        fw->sc_prop[b] = fw->la_prop[b] == 0.0f;
    fw->sc_map = e->sc_hist[best].map;
}

/* WHICH references hold this frame's own source - the whole of the
 * decision, taken on the driver at dispatch (docs/phase11.md).
 *
 * The answer is a SET and not one reference. A picture in the middle of
 * a long hold is the duplicate of the anchor behind it AND of the one in
 * front, and the prediction that averages the two is not the same
 * prediction as either: it averages two independent quantisations of one
 * source, which is better than both and is what the encoder picks there
 * on its own. Naming a single reference made the arm decline every CU on
 * `sita_flat_720p`, where the leaves are bi (docs/phase11.md sec 1.4).
 *
 * EXACT membership first: a reference whose source IS this frame's
 * source, by the chain's run id. `N265_DUP_THR` then ADDS the references
 * whose bounded distance is inside the bar, so the threshold form is a
 * superset of the strict one and the strict one is what a bar of zero
 * leaves. `dup_l`/`dup_r` are the first member, for the census.
 *
 * A picture that is not itself in a run (`dup_grp` 0 - an unarmed
 * encode, or the first frame after a queue drain) matches nothing. */
static void dup_resolve(n265_encoder *e, n265_frame_work *fw)
{
    fw->dup_msk[0] = fw->dup_msk[1] = 0;
    fw->dup_l = fw->dup_r = -1;
    fw->dup_exact = 0;
    if (!n265_dup_on() || fw->slice_type == 2 || !fw->ref_valid ||
        !fw->dup_grp || fw->mode == N265_MODE_PCM || fw->lossless)
        return;
    uint64_t bar = (uint64_t)n265_dup_thr_q8() * dup_samples(e);
    for (int l = 0; l < 2; l++)
        for (int r = 0; r < fw->nref[l]; r++) {
            int exact = fw->refl[l][r]->dup_grp == fw->dup_grp;
            if (!exact && !(n265_dup_thr_q8() &&
                            dup_bound(fw, fw->refl[l][r]) * 256u <= bar))
                continue;
            fw->dup_msk[l] |= 1u << r;
            fw->dup_exact |= exact;
            if (fw->dup_l < 0) {
                fw->dup_l = l;
                fw->dup_r = r;
            }
        }
}

/* N265_DUP_TRACE, the dispatch half (the retirement half is in
 * join_oldest): one row per frame, output-neutral. `zsad` is the mean
 * absolute lowres difference from the PREVIOUS DISPLAY frame, in 8-bit
 * luma levels per pixel - the quantity the clip census is stated in -
 * and each `ref` field is a reference's list, index, POC and the bound
 * on its own distance from this frame, `X` where the two sources are
 * identical. */
static void dup_trace_dispatch(const n265_encoder *e,
                               const n265_frame_work *fw)
{
    double ns = (double)dup_samples(e);
    char refs[256];
    int o = 0;
    refs[0] = 0;
    for (int l = 0; l < 2; l++)
        for (int r = 0; r < fw->nref[l] && o < (int)sizeof refs - 40; r++) {
            const struct n265_refpic *p = fw->refl[l][r];
            int same = p->dup_grp == fw->dup_grp;
            o += snprintf(refs + o, sizeof refs - o, ",L%d[%d]poc%d:%s%.4f",
                          l, r, p->poc, same ? "X" : "",
                          (double)dup_bound(fw, p) / ns);
        }
    fprintf(stderr, "DUP,%d,%d,%d,%d,%d,%d,%d,%.4f,%d,%d,%d,%u,%u%s\n",
            fw->display_idx, fw->code_idx, fw->slice_type, fw->b_depth,
            fw->nonref, fw->qp, fw->dup_grp, (double)fw->dup_zsad / ns,
            fw->dup_l, fw->dup_r, fw->dup_exact, fw->dup_msk[0],
            fw->dup_msk[1], refs);
}

/* N265_DUP_TRACE, the retirement half: how the frame was ACTUALLY coded,
 * off the decision maps the row jobs left behind and the packed length.
 * Written on the driver in coding order, reading nothing a row job could
 * still be writing.
 *
 * `skip` is the share of the coded area with cu_skip_flag set; `zdup`
 * the share coded skip with the ZERO vector on every list it uses and
 * every one of those lists pointing at a duplicate - which is exactly
 * what the arm commits, so on an armed frame it is the share the arm
 * took and 1 - it is the share it handed back. On a frame with no
 * duplicate at all the set is empty and `zdup` is 0 by construction. */
static void dup_trace_retire(const n265_encoder *e,
                             const n265_frame_work *fw, size_t bytes)
{
    unsigned long cells = 0, skip = 0, zdup = 0;
    if (fw->slice_type != 2 && fw->blk_skip) {
        int cw = fw->pad_w >> 2, ch = fw->pad_h >> 2;
        for (int cy = 0; cy < ch; cy++)
            for (int cx = 0; cx < cw; cx++) {
                size_t m = (size_t)cy * fw->map_stride + cx;
                cells++;
                if (!fw->blk_skip[m])
                    continue;
                skip++;
                int pred = fw->blk_pred[m], ok = pred != 0;
                for (int l = 0; l < 2 && ok; l++) {
                    if (!(pred & (1 << l)))
                        continue;
                    int r = fw->blk_ref[2 * m + l];
                    ok = r >= 0 && (fw->dup_msk[l] >> r) & 1 &&
                         fw->blk_mv[4 * m + 2 * l] == 0 &&
                         fw->blk_mv[4 * m + 2 * l + 1] == 0;
                }
                zdup += ok;
            }
    }
    double n = cells ? (double)cells : 1.0;
    fprintf(stderr, "DUPR,%d,%d,%d,%d,%d,%d,%zu,%.4f,%.4f\n",
            fw->display_idx, fw->code_idx, fw->slice_type, fw->dup_l,
            fw->dup_r, fw->dup_exact, bytes, (double)skip / n,
            (double)zdup / n);
}

/* Dispatch one frame: everything that is driver-serial by construction
 * (the QP ladder, the AQ field, the headers, the record and estimator
 * reset), then the frame's job onto the pool. The driver does NOT wait
 * here - that is what makes frames overlap - and it does not touch the
 * frame again until retire_frame joins the job. */
static void dispatch_frame(n265_encoder *e, n265_frame_work *fw)
{
    if (e->sprof >= 2) {
        fw->t_disp = prof_now() - e->prof_t0;
        fw->t_row0 = fw->t_rowlast = fw->t_ent0 = fw->t_ent1 = 0.0;
    }
    int slice_type = fw->slice_type, b_depth = fw->b_depth;
    int nonref = fw->nonref, poc = fw->poc;
    /* P2: no CTU of this picture has a decided depth range yet */
    memset(fw->ctu_dmin, 0xff, (size_t)fw->ctb_cols * fw->ctb_rows);
    memset(fw->ctu_dmax, 0xff, (size_t)fw->ctb_cols * fw->ctb_rows);
    /* Type the frame's content here, on the driver, from its own source
     * luma - so every decision below is frozen before a single row job
     * exists and is the same at every thread count. It leads the QP ladder
     * because the cu-tree frame term branches on it. */
    fw->flat_pct = n265_flat_gate() ? n265_la_flat_pct(fw) : 0;
    fw->flat = n265_flat_gate() && fw->flat_pct >= n265_flat_gate();
    /* ...and whether this picture is a DUPLICATE of one of its own
     * references, which is the same kind of decision: driver-serial, off
     * data frozen before the frame launches, and read by every CTU. */
    dup_resolve(e, fw);
    /* ...and WHICH retired frame's coded-skip map the static commit reads
     * on this picture (phase 11 item 4). Same shape, same reason. */
    sc_resolve(e, fw);
    /* ...and, reading that type, whether the adaptive intra gate fires
     * on this frame. Driver-serial, off the retired window only, frozen
     * here for the same reason the type is. */
    fw->iw_gate = iw_adapt_gate(e, fw->code_idx, slice_type, fw->flat);
    /* The cu-tree FRAME term, computed here rather than where it lands,
     * because under ABR the rate control has to be told it BEFORE it answers:
     * the term is added to the base below, so a loop that has not seen it is
     * controlling a number the encoder does not code at (rc.c, the block that
     * takes it back off). Nothing between here and its use moves it -
     * it reads the lookahead's two means, the frame's type and the config -
     * so hoisting it is the same double, and under CQP and CRF it is the same
     * `if` producing the same value in the same order it always did. */
    double ctf = (e->crf_on || e->abr_on) && e->cutree
                     ? n265_ct_frame_term(e, fw) : 0.0;
    fw->rc_ctf = ctf;
    fw->rc_ioff = 0.0;
    fw->vbv_pred = fw->vbv_allow = fw->vbv_dqp = 0.0;
    if (e->abr_on) {
        /* D4's observation lag has already been drained by the caller; this
         * is the frame's own solve, moved here so it sees `ctf`. */
        e->rc.cur_icplx = fw->rc_icplx;
        /* --pass 2: the plan for THIS frame, looked up by display index -
         * the one order both passes agree on, since a pass 1 at a different
         * preset can pick different mini-GOP lengths (twopass.c). A frame
         * the stats do not describe leaves plan_ok at 0 and takes the
         * one-pass path, and is counted so close() can say it happened. */
        e->rc.plan_ok = 0;
        if (e->tp.pass >= 2) {
            const n265_2p_frame *pf = n265_2p_get(&e->tp, fw->display_idx);
            if (pf) {
                e->rc.plan_qp = pf->plan_qp;
                e->rc.plan_bits = pf->plan_bits;
                e->rc.plan_ok = 1;
            } else {
                e->tp.miss++;
            }
        }
        e->rc_rf = n265_rc_next(e, fw->rc_cls, fw->rc_cplx, ctf);
        fw->rc_planned = e->rc.last_planned;
        /* the planned intra offset is already inside the answer above (rc.c);
         * it is carried on the frame so the measured operating point can take
         * it back out when the frame retires */
        fw->rc_ioff = e->rc.last_ioff;
        /* ...and the VBV's three columns, which rc_next filled while it had
         * the window in hand. They are diagnostics only - the clamp is
         * already inside e->rc_rf - and they are what the trace prints when
         * the frame retires and its real size is known. */
        fw->vbv_pred = e->rc.last_vbv_pred;
        fw->vbv_allow = e->rc.last_vbv_allow;
        fw->vbv_dqp = e->rc.last_vbv_dqp;
    }
    /* frame-type QP ladder (the matched reference applies its ip/pb
     * ratios in CQP mode too): I anchors lower, non-ref B higher */
    /* CRF (yah264's final form, written for this encoder): a constant base per
     * frame, crf + the cu-tree operating-point shift. The cu-tree offsets are
     * left un-centred under CRF (a referenced region nets a lower QP, a leaf
     * region does not), so their mean-negative field is netted out by a
     * fixed shift and only the DIFFERENTIAL survives; centring them would
     * throw the cross-frame allocation away. N265_CRF_SHIFT nets what is
     * left of that mean after the frame term below has taken k_f of it:
     * 5.4 = (1 - qcomp) * 13.5 at qcomp 0.6 with the term off, which is the
     * default, and 0 without cu-tree. Any arm that arms k_f re-fits it (4.9
     * at k_f 1, foreman crf 28 52617 -> 52928); BD at matched achieved
     * bitrate does not depend on it, the crf-to-rate mapping does, and the
     * board's timed leg and every recorded crf number read that mapping.
     *
     * It STAYS 5.4 through phase 7 follow-up 2, which is a refusal and not an
     * oversight: with the base fractional the shift is no longer a neutral
     * control on the across-shot leg. The content gate's own re-fit candidates
     * all land foreman inside 1% (5.2 +0.32%, 5.25 +0.49%, 5.3 -0.97%) and
     * read +0.43, +0.30 and +0.68 on ms_cif_30 against main, where the gate
     * at 5.4 reads -0.05 - a 0.7% scatter, driven by the dither the base's
     * fraction spends, that is larger than the effect being measured. Holding
     * the operating point costs foreman 1.91% of its mapping and keeps every
     * other number in the tree meaning what it says.
     * The AQ frame-mean term lands on the base below, once the variances
     * are known. */
    double base = e->qp_base;
    if (e->abr_on) {
        /* ABR: the base is the rate factor rc.c solved for this frame from
         * the bits spent against the bits planned (docs/instruments.md). */
        base = e->rc_rf;
    } else if (e->crf_on) {
        double shift = 0.0;
        if (e->cutree) { const char *v = n265_env("N265_CRF_SHIFT"); shift = v ? atof(v) : 5.4; }
        base = e->crf + shift;
    }
    /* The SHOT plan (--shot-window, opt-in, CRF only): one offset for the
     * whole shot, priced when the shot entered the window (shot_plan_at).
     * It lands FIRST, before the cu-tree frame term and the AQ frame mean
     * read the base, so those two keep working inside the shot exactly as
     * they do at the default - the plan says where the shot sits, they
     * say where each frame sits inside it. Exactly 0 with the mode off,
     * so the default path is the arithmetic it has always been. */
    base += fw->shot_off;
    /* The cu-tree FRAME term (phase 7 step 2), derived from the field's own
     * frame mean (n265_la_cutree left it in fw): what the lookahead measured
     * this frame to be worth to the ones that reference it, where the
     * cascade below is a fixed guess. CRF/ABR only - under CQP the field is
     * centred and there is no cross-frame term to place. It lands before the
     * rounding, so with the fractional path on it is carried over the CTUs
     * and without it, it rounds with the base. */
    if ((e->crf_on || e->abr_on) && e->cutree) {
        base += ctf;
        /* N265_CT_TRACE (diagnostic, output-neutral): the term per frame,
         * with the two means it came from. This is the instrument the clamp
         * of step 3 was read off, per shot, rather than swept.
         *
         * sc_ratio and the mean lowres |MV| joined it in follow-up 2, which
         * is the read that CHOOSES a content gate: bucketed per shot, the
         * three columns say which of them separates the frames a term costs
         * on from the frames it pays on. On this corpus sc_ratio does, the
         * window level does not, and |MV| has no resolution at CIF at all. */
        if (n265_env("N265_CT_TRACE"))
            fprintf(stderr, "CTF,%d,%d,%d,%d,%.3f,%.3f,%+.3f,%.4f,%.3f\n",
                    fw->display_idx, slice_type, nonref, fw->flat,
                    fw->ct_mean_raw, fw->ct_level, ctf,
                    (double)fw->sc_ratio, (double)fw->la_mvabs);
    }
    /* CAPPED CRF (phase 12 item 2): the buffer's clamp, and the last thing
     * that may move the base.
     *
     * Under ABR the same clamp is inside n265_rc_next, where the loop's own
     * answer is; under CRF there is no loop, so it lands here - after the
     * shot plan and the cu-tree frame term, because `base` is only the
     * number n265_rc_done learns the class scales against once both are on
     * it, and the prediction has to be made in the units the observation is
     * taken in. Before the cascade and before the rounding, because those
     * are what turn a base into this frame's QP.
     *
     * It is one-sided: capped CRF never spends a full buffer on a finer
     * quantiser. The CRF value is the quality the user asked for and the
     * VBV is a ceiling on the damage, not a second target. */
    if (e->vbv.on && !e->abr_on) {
        base = n265_vbv_crf_base(e, fw->rc_cls, fw->rc_cplx, fw->rc_icplx,
                                 base, &fw->vbv_pred, &fw->vbv_allow,
                                 &fw->vbv_dqp);
    }
    /* the pyramid QP cascade: I anchors -3, P anchors +0, and each B layer
     * one step deeper than its parent (the reference B nearest the anchors
     * codes best, the leaves worst). N265_PB_STEP scales the per-layer
     * increment. N265_PB_CASCADE=r,l names the two B steps outright
     * (referenced, leaf) under CRF/ABR, which is the OTHER half of the
     * double count the frame term above addresses: under CRF the un-centred
     * field is already a frame-type cascade, so these fixed steps are paid
     * twice. Taking it here rather than out of the field is what keeps the
     * shot's own level intact, and a fixed step moves no bits between shots.
     * N265_PB_CASCADE_FLAT is the same on a flat-typed frame, where the
     * ladder is not double-counted the same way; today's form ships there. */
    double pb = 1.0; { const char *v = n265_env("N265_PB_STEP"); if (v) pb = atof(v); }   /* live in CQP and CRF alike */
    int cascade = slice_type == 2 ? -3 : slice_type == 0 ? (int)lround(1 + b_depth * pb) : 0;
    if (slice_type == 0 && (e->crf_on || e->abr_on)) {
        int on = fw->flat ? e->pb_cascade_flat_on : e->pb_cascade_on;
        if (on)
            cascade = fw->flat ? (nonref ? e->pb_cascade_flat_l : e->pb_cascade_flat_r)
                               : (nonref ? e->pb_cascade_l : e->pb_cascade_r);
    }
    /* The signalled base QP is an integer, so a control variable that only
     * moves it is quantised to 5-15% of the rate per step and cannot land on
     * a size. Carry the base's fraction instead and spend it over the CTUs
     * below; qfrac is exactly 0 when the fractional path is off, which is
     * what keeps CQP and CRF byte-identical to the pre-ABR encoder.
     *
     * Under CRF the path is armed per FRAME, not per encode (phase 7
     * follow-up 1): it is the cu-tree frame term that makes the base's
     * fraction worth carrying, and on a frame the content gate typed
     * flat-shaded the term is identically zero by default
     * (N265_CT_FRAMEW_FLAT = k_b). Spending a fraction there would buy no
     * continuity and still pay for the dither - measured, at +1.52 on
     * sita_flat_720p and +1.23 on sintel_720p, which is the 2026-09-09
     * refusal of QP_FRAC as a CRF default reappearing on exactly the clips
     * that refused it. So the flat branch keeps the integer base and stays
     * byte-identical to main, the way every other phase 7 term left it. */
    int frac_on = e->qp_frac;
    if (frac_on && e->crf_on && e->cutree && fw->flat && !e->qp_frac_flat &&
        (double)e->ct_framew_flat == (double)e->ct_qpw)
        frac_on = 0;
    int qi = (int)lround(base);
    double qfrac = frac_on ? base - (double)qi : 0.0;
    int q = qi + cascade;
    fw->qp = n265_qp_clip(q);
    if (n265_dup_trace())
        dup_trace_dispatch(e, fw);

    if (e->frames_done == 0) {
        /* The headers are a pure function of config, so they are written
         * here and packed when this frame retires: under overlap the
         * output ring is walked in coding order, not in completion
         * order, and the VPS/SPS/PPS have to lead the first slice. */
        fw->emit_headers = 1;
        e->nhdr = 0;
        n265_write_vps(fw); stash_header(e, N265_NAL_VPS, fw);
        n265_write_sps(fw); stash_header(e, N265_NAL_SPS, fw);
        n265_write_pps(fw); stash_header(e, N265_NAL_PPS, fw);
    }
    /* --film-grain: the SEI, in EVERY IRAP access unit. The persistence
     * flag makes one message hold for the coded video sequence it opens,
     * and an IDR opens a new one, so a stream with several key frames
     * needs the message repeated and not just carried. About twenty bytes
     * a key frame, and zero without the flag. */
    fw->sei_len = 0;
    if (e->fgc_strength > 0.0 && fw->idr && e->fgc_ready && e->fgc.present) {
        n265_write_fgc_sei(&fw->rbsp, &e->fgc);
        if (fw->rbsp.bytes <= sizeof fw->sei) {
            memcpy(fw->sei, fw->rbsp.buf, fw->rbsp.bytes);
            fw->sei_len = fw->rbsp.bytes;
        }
    }
    /* The HDR10 static metadata (phase 14 item 1), in EVERY IRAP access
     * unit for the reason the grain SEI is: the messages persist to the
     * end of the coded video sequence, an IDR opens a new one, and a
     * decoder that random-accesses at a mid-stream IDR would otherwise
     * have no mastering display to tone-map against. About 40 bytes a key
     * frame, and zero without the flags. */
    fw->hdr_sei_len = 0;
    if (fw->idr && (n265_hdr10_on(&e->sig) || e->sig.atc >= 0) &&
        n265_write_hdr_sei(&fw->rbsp, &e->sig) > 0) {
        if (fw->rbsp.bytes <= sizeof fw->hdr_sei) {
            memcpy(fw->hdr_sei, fw->rbsp.buf, fw->rbsp.bytes);
            fw->hdr_sei_len = fw->rbsp.bytes;
        }
    }
    fw->nal_type = fw->idr ? N265_NAL_IDR_W_RADL
                           : (nonref ? 0 /* TRAIL_N */
                                     : 1 /* TRAIL_R */);
    fw->rc_base = base;
    fw->job_R = 0;
    fw->ref_lag = 0;
    fw->sao_mask = fw->mode == N265_MODE_PCM || fw->lossless
                 ? 0 : n265_sao_frame_mask(fw);
    /* N265_RECT_FLAT (ctu.c): the second tool that reads the type. Both
     * branches are written, because a frame_work is pooled and fw_config
     * ran once. */
    fw->rect = e->rect || (fw->mode != N265_MODE_PCM && fw->flat &&
                           n265_rect_flat() != 0);
    /* N265_FLAT_TRACE (diagnostic, output-neutral): the per-frame
     * content type, which is what a clip-level census of the gate is
     * read off. */
    if (n265_env("N265_FLAT_TRACE"))
        fprintf(stderr, "FLT,%d,%d,%d,%d,%d\n", fw->display_idx,
                fw->slice_type, fw->flat_pct, fw->flat, fw->rect);
    if (fw->mode != N265_MODE_PCM) {
        memset(fw->edge_map, 0, (size_t)fw->map_stride * (fw->pad_h >> 2));
        memset(fw->blk_intra, 0, (size_t)fw->map_stride * (fw->pad_h >> 2));
        memset(fw->blk_skip, 0, (size_t)fw->map_stride * (fw->pad_h >> 2));
        memset(fw->blk_cbf_y, 0, (size_t)fw->map_stride * (fw->pad_h >> 2));
        memset(fw->blk_pred, 0, (size_t)fw->map_stride * (fw->pad_h >> 2));
        memset(fw->blk_mv, 0,
               (size_t)fw->map_stride * (fw->pad_h >> 2) * 4 * sizeof(int16_t));
        /* per-CTU QP offsets: cu-tree (filled by the scheduler) plus
         * variance AQ on top */
        if (!e->cutree) {
            memset(fw->ctu_qp_off, 0, (size_t)fw->ctb_cols * fw->ctb_rows);
            memset(fw->ctu_qp_raw, 0, (size_t)fw->ctb_cols * fw->ctb_rows * sizeof(float));
        }
        if (e->aq && n265_env("N265_AQ_FLAT"))
            memset(fw->ctu_qp_off, (uint8_t)atoi(n265_env("N265_AQ_FLAT")),
                   (size_t)fw->ctb_cols * fw->ctb_rows);
        else if (e->aq) {
            int ctb = 1 << fw->ctb_log2;
            int nctu = fw->ctb_cols * fw->ctb_rows;
            double *lv = malloc((size_t)nctu * sizeof(double));
            double avg = 0;
            /* The flat-frame protecting sign's second field and its content
             * census (N265_AQ_FLAT_THR, phase 10 item 3; see the flat branch
             * below). Both are built only when the sign is armed, so at the
             * shipped `N265_AQ_FLAT_SIGN=0` this is one pointer compare per
             * frame and the field below is the one it has always been. */
            double *lvv = lv;           /* log2(VARIANCE), the flat field */
            double avg_v = 0;
            long fine_hi = 0;           /* CTUs whose `fine` is outline-sized */
            if (e->aq_flat_sign != 0.0f && e->aq_signal) {
                lvv = malloc((size_t)nctu * sizeof(double));
                if (!lvv) lvv = lv;
            }
            /* N265_AQ_FLOOR: a floor on the CTU variance the AQ field is
             * built from. The term models masking, and masking is a
             * property of content the eye can see; below the coding noise
             * floor there is nothing to mask and nothing to hide, so a
             * CTU flatter than the floor is not flatter for this purpose.
             * Natural video has no such CTUs (film grain and sensor noise
             * put a variance floor in every block); synthetic animation is
             * full of them, and without a floor the field reads a
             * perfectly flat fill as maximally masking-free and drives it
             * many QP steps DOWN, buying bits for a region that codes to
             * nothing while the outlines that carry the picture pay. 4 is
             * a standard deviation of two luma levels: below it an 8-bit
             * picture is flat to within the smallest step the quantiser
             * reproduces. 0 is the pre-2026-09-09 behaviour and is
             * exactly inert.
             *
             * The floor reaches the per-CTU term ONLY. The frame mean below
             * it feeds a different term with a different job - the AQ_DC
             * across-shot allocation - and there the unfloored mean is the
             * right quantity: a black frame really is cheaper to code, and
             * flooring it makes the complexity proxy worse. Measured: with
             * the floor on both, the across-shot gate on ms_720p_50 went
             * -16.92% to -16.08%; with it on the per-CTU field alone the
             * gate holds. */
            /* A VARIANCE, so it scales by 1 << (2 * BIT_DEPTH_SHIFT) -
             * 4 at 8 bits, 64 at Main10 - and the knob keeps its 8-bit
             * meaning: two luma levels of standard deviation at whatever
             * depth is being coded. Left at 4 the floor would never bind
             * at 10 bits and the flat-fill pathology it exists for comes
             * straight back. */
            double vfloor = 4.0 * (double)(1 << (2 * BIT_DEPTH_SHIFT));
            { const char *v = n265_env("N265_AQ_FLOOR");
              if (v) vfloor = atof(v) * (double)(1 << (2 * BIT_DEPTH_SHIFT)); }
            double avg_dc = 0;      /* the same field WITHOUT the floor */
            for (int cy = 0; cy < fw->ctb_rows; cy++)
                for (int cx = 0; cx < fw->ctb_cols; cx++) {
                    int x0 = cx * ctb, y0 = cy * ctb;
                    int w = x0 + ctb > fw->pad_w ? fw->pad_w - x0 : ctb;
                    int h = y0 + ctb > fw->pad_h ? fw->pad_h - y0 : ctb;
                    int64_t s1 = 0, s2 = 0;
                    for (int y = y0; y < y0 + h; y++)
                        for (int x = x0; x < x0 + w; x++) {
                            int v = fw->pad_plane[0][y * fw->pad_stride[0] + x];
                            s1 += v;
                            s2 += v * v;
                        }
                    int np = w * h;
                    double var = (double)s2 / np -
                        ((double)s1 / np) * ((double)s1 / np);
                    /* the +1 keeps log2 finite at a perfectly flat CTU,
                     * and it is one SQUARED 8-bit level: scaling it with
                     * the variance is what makes the whole field shift by
                     * exactly 2 * BIT_DEPTH_SHIFT at Main10, which is
                     * what the anchor below then subtracts back off. */
                    avg_dc += log2((var < 0.0 ? 0.0 : var) +
                                   (double)(1 << (2 * BIT_DEPTH_SHIFT)));
                    /* The VARIANCE field the flat branch's protecting sign
                     * reads, taken here because two lines below `var` stops
                     * being the variance. Same floor, same +1, same units as
                     * `lv`; only the quantity differs. */
                    if (lvv != lv) {
                        double vv = var < 0.0 ? 0.0 : var;
                        if (vv < vfloor) vv = vfloor;
                        lvv[cy * fw->ctb_cols + cx] =
                            log2(vv + (double)(1 << (2 * BIT_DEPTH_SHIFT)));
                        avg_v += lvv[cy * fw->ctb_cols + cx];
                    }
                    /* N265_AQ_SIGNAL=1 (phase 7 M1'): the per-CTU signal
                     * becomes `fine` - the CTU's full-resolution variance
                     * minus what its 2x-down plane explains - in place of
                     * the variance itself. The loss map (step 0) ranks the
                     * metric's per-block loss at +0.42 / +0.52 with `fine`
                     * on foreman / mobile against log2-variance's +0.37 /
                     * +0.30, and `coarse` turned out to BE the variance
                     * (+0.93 to +0.998 on every clip), so this is the one
                     * signal in the design the shipped term does not
                     * already carry. The 2x-down plane is the lookahead's
                     * own 2x2 mean, (a+b+c+d+2)>>2 (la.c), recomputed here
                     * rather than read from f->lowres because that plane is
                     * built over the lookahead's own padded width and this
                     * has to land on the CTU grid; it is 1/4 of the reads
                     * the variance above already does, on the driver thread
                     * before the frame is launched, so the frame's
                     * decisions stay frozen (threading rule 4).
                     *
                     * The frame mean the AQ_DC term consumes (avg_dc above)
                     * is deliberately NOT switched: it is a frame-level
                     * complexity proxy, not a masking term, and the map
                     * does not speak to it. */
                    if (e->aq_signal) {
                        int hw = w >> 1, hh = h >> 1;
                        if (hw > 0 && hh > 0) {
                            int ps = fw->pad_stride[0];
                            int64_t t1 = 0, t2 = 0;
                            for (int y = 0; y < hh; y++) {
                                const pixel *p = fw->pad_plane[0] +
                                    (size_t)(y0 + 2 * y) * ps + x0;
                                for (int x = 0; x < hw; x++) {
                                    int d = (p[2 * x] + p[2 * x + 1] +
                                             p[2 * x + ps] + p[2 * x + ps + 1]
                                             + 2) >> 2;
                                    t1 += d;
                                    t2 += d * d;
                                }
                            }
                            int n2 = hw * hh;
                            double v2 = (double)t2 / n2 -
                                ((double)t1 / n2) * ((double)t1 / n2);
                            var -= v2 < 0.0 ? 0.0 : v2;
                        }
                        if (var < 0.0) var = 0.0;
                    }
                    /* The content census the flat gate acts on, read on the
                     * per-CTU signal itself and BEFORE the floor eats it:
                     * how many CTUs of this frame carry outline-sized fine
                     * detail. AQ_FLAT_HI is 16 squared 8-bit levels, four
                     * levels of within-2x2 standard deviation, scaled to the
                     * coded depth like every other variance here. It is a
                     * count and touches nothing; the gate below is the only
                     * reader. */
                    if (e->aq_flat_sign != 0.0f &&
                        var >= AQ_FLAT_HI * (double)(1 << (2 * BIT_DEPTH_SHIFT)))
                        fine_hi++;
                    if (var < vfloor) var = vfloor;
                    lv[cy * fw->ctb_cols + cx] =
                        log2(var + (double)(1 << (2 * BIT_DEPTH_SHIFT)));
                    avg += lv[cy * fw->ctb_cols + cx];
                }
            avg /= nctu;
            avg_dc /= nctu;
            if (lvv != lv) avg_v /= nctu;
            /* Under CRF the frame mean of the AQ field is the across-frame /
             * across-shot allocation term (yah264 row 10, 2026-09-05): a busy
             * frame codes a little cheaper, a quiet one a little better. It
             * goes on the BASE QP at its own strength (N265_AQ_DC, default
             * 1.0) against an absolute anchor in log2(variance) units
             * (N265_AQ_ANCHOR, default 6.4), so every base-keyed decision sees
             * it; the per-CTU part below stays centred on the frame.
             *
             * The anchor is in log2(variance), so the depth term is
             * ADDITIVE and not a scale: a variance sixteen times larger is
             * four further up a log2 axis. Without the +4 every CRF and
             * ABR frame of every 10-bit clip would take a flat +4 QP on
             * its base - the largest single mis-calibration this item had
             * to find, and one no conformance vector would have caught. */
            if (e->crf_on || e->abr_on) {
                const char *v = n265_env("N265_AQ_DC"), *a = n265_env("N265_AQ_ANCHOR");
                double anchor = (a ? atof(a) : 6.4) + 2.0 * BIT_DEPTH_SHIFT;
                double dc = (v ? atof(v) : 1.0) * (avg_dc - anchor);
                int dci = (int)lround(dc);
                int nq = fw->qp + dci;
                fw->qp = n265_qp_clip(nq);
                if (frac_on)
                    qfrac += dc - (double)dci;
            }
            /* per-CTU term at N265_AQ_STR. Default: round it on its own and
             * add it to the already-rounded cu-tree offset (two roundings);
             * N265_QP_ROUND1 sums the two float fields and rounds once, so
             * fractional strengths on either side survive. */
            /* The per-CTU term is a perceptual correction, not a rate
             * lever, and it is proportional to the frame's variance SPREAD,
             * which is unbounded: flat animation puts black CTUs and drawn
             * detail in the same frame and the term reaches +-4 QP, taxing
             * exactly the CTUs the metric reads. N265_AQ_CLAMP bounds it
             * before the cu-tree field is added. 12 is the old effective
             * bound (the offset byte's own clamp); 3 ships (2026-09-08) and
             * is inert wherever the spread is small - four of the seven CIF
             * clips are byte-identical under it and the worst moves 0.43. */
            double aqc = 3.0;
            { const char *v = n265_env("N265_AQ_CLAMP"); if (v) aqc = atof(v); }
            /* N265_AQ_CLAMP_FLAT: the same bound on a frame the content
             * gate typed flat-shaded (la.h), where the pathology the
             * clamp exists for is not an outlier but the whole frame.
             * The term models masking and is proportional to the frame's
             * log2-variance SPREAD; on flat-shaded animation that spread
             * is a constant fill against a drawn outline, so the term
             * taxes the outlines - the only thing carrying the picture -
             * to buy bits for fills that code to nothing. N265_AQ_FLOOR
             * fixed the half of this that was the fill reading as
             * maximally masking-free; the other half is that a flat fill
             * offers an outline no masking at all, so there is no
             * masking model left to apply. 0 ships: the per-CTU term is
             * OFF on such a frame (the cu-tree field, summed after the
             * clamp, is untouched). Set it to N265_AQ_CLAMP's value to
             * restore the ungated behaviour. Worth, on the three
             * animation clips at CQP/CRF against the SAO arm alone:
             * sintel +2.39/-5.55 -> -10.20/-17.78, sita_toon
             * -3.24/-4.62 -> -5.86/-7.06, sita_flat -2.41/-10.01 ->
             * -3.37/-10.07. A bound of 1 is the runner-up and is 6.1
             * CQP points worse on sintel. */
            double aqs = e->aq_str;
            /* which field the per-CTU term reads, and its mean. `lv` on
             * every frame the encoder has ever coded; the flat branch below
             * is the one place that can move it. */
            const double *fld = lv;
            double favg = avg;
            double fshare = nctu ? 100.0 * (double)fine_hi / nctu : 0.0;
            if (fw->flat) {
                aqc = 0.0;
                const char *v = n265_env("N265_AQ_CLAMP_FLAT");
                if (v) aqc = atof(v);
                /* N265_AQ_FLAT_SIGN (phase 7 M1'): the flat branch above is
                 * a patch over a SIGN, and the loss map says which sign the
                 * metric wants. On the animation clips the loss is
                 * concentrated - 61% of sita_flat's in the worst decile of
                 * blocks - and every signal ranks it above +0.8, so there is
                 * something here for a per-CTU term to redistribute; what
                 * the masking sign got wrong is that it TAXED the blocks
                 * carrying the loss. Non-zero turns the term back on for a
                 * flat-typed frame with the protecting sign (high signal ->
                 * lower QP) at that strength, bounded by N265_AQ_CLAMP
                 * rather than by N265_AQ_CLAMP_FLAT. 0 ships and is the
                 * branch above untouched. */
                /* N265_AQ_FLAT_THR (phase 10 item 3): the CONTENT signal the
                 * 2026-09-12 refusal asked for. The sign is worth six BD
                 * points on sintel_720p and costs sita_toon_720p four, which
                 * is a trade inside one content TYPE: both frames are flat
                 * by the gate's own count, and what differs is what the
                 * remaining area holds - sintel's is fill and soft gradient,
                 * where the loss is banding and a lower QP is the cure;
                 * sita_toon's is drawn OUTLINE, which the protecting sign
                 * feeds at the fills' expense. `fine` separates the two
                 * exactly (AQ_FLAT_HI above), so the sign applies only where
                 * the frame's outline share is under this threshold, in
                 * percent. The default is past 100 and is therefore the
                 * ungated arm: a threshold nothing can reach.
                 *
                 * The armed branch also switches the FIELD to the variance.
                 * That is not a second arm, it is the same root cause: on a
                 * flat-typed frame `fine` is at or under the floor almost
                 * everywhere (sintel's median CTU reads 0.01 squared levels),
                 * so the shipped signal has no spread left to protect
                 * anything WITH - which is why the refusal read +1.06 on
                 * sintel with `fine` and -6.20 with the variance. Where the
                 * gate does not fire the branch is the shipped one and the
                 * frame is byte-identical. */
                if (e->aq_flat_sign != 0.0f) {
                    double fthr = 1000.0;
                    const char *t = n265_env("N265_AQ_FLAT_THR");
                    if (t) fthr = atof(t);
                    if (fshare < fthr) {
                        aqs = -e->aq_flat_sign;
                        aqc = 3.0;
                        const char *c = n265_env("N265_AQ_CLAMP");
                        if (c) aqc = atof(c);
                        fld = lvv;
                        favg = lvv == lv ? avg : avg_v;
                    }
                }
            }
            if (n265_env("N265_AQ_TRACE")) {
                /* the field the per-CTU term is built from: how far the
                 * frame's log2-variance spread reaches, and how much of
                 * it the clamp is already eating */
                double mn = 1e9, mx = -1e9, sd = 0, lo = 0, hi = 0;
                for (int i = 0; i < nctu; i++) {
                    if (fld[i] < mn) mn = fld[i];
                    if (fld[i] > mx) mx = fld[i];
                    sd += (fld[i] - favg) * (fld[i] - favg);
                    double o = aqs * (fld[i] - favg);
                    lo += o < -aqc;
                    hi += o > aqc;
                }
                /* the last two columns joined on 2026-09-13: whether the
                 * frame is flat-typed, and the outline share the flat gate
                 * reads (0 unless N265_AQ_FLAT_SIGN arms the census) */
                fprintf(stderr,
                        "AQT,%d,%d,%d,%.3f,%.3f,%.3f,%.3f,%.4f,%.4f,%d,%.2f\n",
                        fw->display_idx, slice_type, nctu, favg, mn, mx,
                        sqrt(sd / nctu), lo / nctu, hi / nctu,
                        fw->flat ? 1 : 0, fshare);
            }
            /* N265_CT_QPW: the share of the cu-tree field that reaches the
             * QP, on cutree_str's scale. 1.0 at the default. */
            double ctqpw = n265_ct_qp_scale(e);
            for (int i = 0; i < nctu; i++) {
                double off = aqs * (fld[i] - favg);
                if (off > aqc) off = aqc;
                else if (off < -aqc) off = -aqc;
                int o;
                if (e->qp_round1) {
                    off += ctqpw * fw->ctu_qp_raw[i];
                    o = (int)(off > 0 ? off + 0.5 : off - 0.5);
                } else {
                    o = (int)(off > 0 ? off + 0.5 : off - 0.5) +
                        fw->ctu_qp_off[i];
                }
                fw->ctu_qp_off[i] = (int8_t)(o < -12 ? -12 : o > 12 ? 12 : o);
            }
            if (lvv != lv) free(lvv);
            free(lv);
        }

        /* Spend the base QP's fractional part over the CTUs: each one is
         * rounded against its own threshold from a low-discrepancy
         * sequence, so the frame's MEAN QP is the float base to within one
         * CTU and the achieved rate is continuous in the control variable.
         * The threshold is a pure function of the CTU raster index and the
         * frame counter - not of a running error - so it stays
         * position-independent and survives any future thread schedule. */
        if (frac_on && qfrac != 0.0) {
            int nctu = fw->ctb_cols * fw->ctb_rows;
            for (int i = 0; i < nctu; i++) {
                double d = (double)(i + 1) * 0.7548776662466927 +
                           (double)(e->frames_done + 1) * 0.5698402909980532;
                d -= floor(d);
                int o = fw->ctu_qp_off[i] + (int)floor(qfrac + d);
                fw->ctu_qp_off[i] = (int8_t)(o < -12 ? -12 : o > 12 ? 12 : o);
            }
        }

        /* N265_QP_MAP_TRACE=file: the FINAL per-CTU offset map, one line per
         * frame, after the AQ term, the cu-tree field and the
         * fractional-base dither have all landed in it - which is the map
         * the CTU, the deblock and SAO all read. Diagnostic and
         * output-neutral; dispatch_frame runs on the driver thread, so the
         * lines are serialised without a lock. scripts/vmaf_lossmap.py
         * joins its blocks to this. */
        {
            const char *qmf = n265_env("N265_QP_MAP_TRACE");
            if (qmf && *qmf) {
                FILE *f = fopen(qmf, "a");
                if (f) {
                    int nq = fw->ctb_cols * fw->ctb_rows;
                    fprintf(f, "QPM,%d,%d,%d,%d,%d,%d", fw->display_idx,
                            slice_type, fw->qp, 1 << fw->ctb_log2, fw->ctb_cols,
                            fw->ctb_rows);
                    for (int i = 0; i < nq; i++)
                        fprintf(f, ",%d", fw->ctu_qp_off[i]);
                    fprintf(f, "\n");
                    fclose(f);
                }
            }
        }

        if (n265_env("N265_QP_TRACE"))
            fprintf(stderr, "QPT disp=%d type=%d depth=%d nonref=%d qp=%d "
                    "qf=%+.3f\n", fw->display_idx, slice_type, fw->b_depth,
                    fw->nonref, fw->qp, qfrac);
        fw->ent_qp_prev = fw->qp;
        fw->ent_parent_chroma = 0;
        fw->rec_len = 0;
        fw->rec_pos = 0;
        for (int i = 0; i < CTX_COUNT; i++)
            n265_cabac_ctx_init(&fw->est_ctx[i],
                                fw->slice_type == 2 ? n265_ctx_init_i[i]
                                : fw->slice_type == 1 ? n265_ctx_init_p[i]
                                                     : n265_ctx_init_b[i],
                                fw->qp);
        /* The row wavefront of threading-design sec 1 on the pool, plus
         * the per-row publish (G4) and the entropy task. The readiness
         * predicates are the design's; every thread count and every K
         * runs the same task set in the same data order. */
        fw->job_R = fw->ctb_rows;
        fw->job_cols = fw->ctb_cols;
        fw->job_snap_x = fw->ctb_cols > 1 ? 1 : 0;  /* the WPP geometry */
        fw->job_sao_on = fw->sao_mask != 0;
        /* G1's hint: LAG = 1 + ceil((merange + 5) / ctb_size), sec 1's
         * own formula, kept as the row-start heuristic. N265_REF_LAG_HINT
         * overrides it in CTB rows (0 = start a row as soon as its
         * references have published anything at all, which is the
         * loosest legal hint); it is a readiness predicate and cannot
         * change bits, so it is a pure schedule knob for T3/4's sweep. */
        static int hint = -2;
        if (hint == -2) {
            const char *v = n265_env("N265_REF_LAG_HINT");
            hint = v ? atoi(v) : -1;
        }
        fw->ref_lag = fw->slice_type == 2 ? 0
            : hint >= 0 ? hint
            : 1 + ((57 + 5 + (1 << fw->ctb_log2) - 1) >> fw->ctb_log2);
        fw->qprev = fw->qp;
    }

    memset(fw->prog, 0, sizeof(*fw->prog) +
                        (size_t)fw->ctb_rows * sizeof(atomic_int));
    fw->job.ctx = fw;
    fw->job.ntask = 3 * fw->job_R + 1;      /* + the entropy task */
    fw->job.ready = job_ready;
    fw->job.run = job_run;
    fw->job.impl = NULL;
    fw->in_flight = 1;
    e->inflight[e->n_inflight++] = fw;
    (void)poc;
    ntp_submit(e->pool, &fw->job);
    e->frames_done++;
    atomic_fetch_add_explicit(&e->stall_frames, 1, memory_order_relaxed);
}

/* The per-frame entropy task: slice header, then the CTU syntax replayed
 * from the committed records through the real CABAC engine. Entropy
 * tasks of different frames are mutually independent (contexts
 * initialize per slice), so they parallelize across frames like any
 * other pool work; the output ORDER is restored by the deferred-NAL ring
 * at retirement, not here. */
static void entropy_frame(n265_encoder *e, n265_frame_work *fw)
{
    cabac_t cb;
    int last = fw->ctb_rows * fw->ctb_cols - 1;
    int nonref = fw->nonref;
    (void)nonref;
    fw->ent_qp_prev = fw->qp;
    fw->ent_parent_chroma = 0;
    fw->in_entropy = 1;
    if (fw->cu_stats) {
        memset(fw->cs_area, 0, sizeof fw->cs_area);
        memset(fw->cs_tu, 0, sizeof fw->cs_tu);
        memset(fw->cs_tu_cbf, 0, sizeof fw->cs_tu_cbf);
        fw->cs_coef = 0;
    }
    if (fw->cbit_trace) {
        memset(fw->cbit, 0, sizeof fw->cbit);
        memset(fw->cbf_c_n, 0, sizeof fw->cbf_c_n);
        memset(fw->cbf_c_cb, 0, sizeof fw->cbf_c_cb);
        memset(fw->cbf_c_cr, 0, sizeof fw->cbf_c_cr);
        fw->cbit_cnz = 0;
        fw->cbit_bucket = N265_CB_OTHER;
    }
    bs_reset(&fw->rbsp);
    write_slice_header(e, fw);
    n265_cabac_start(&cb, &fw->rbsp);
    for (int i = 0; i < CTX_COUNT; i++)
        n265_cabac_ctx_init(&fw->ctx[i],
                            fw->slice_type == 2 ? n265_ctx_init_i[i]
                            : fw->slice_type == 1 ? n265_ctx_init_p[i]
                                                 : n265_ctx_init_b[i],
                            fw->qp);

    for (int i = 0; i <= last; i++) {
        int cx = i % fw->ctb_cols, cy = i / fw->ctb_cols;
        if (fw->mode == N265_MODE_PCM) {
            write_pcm_ctu(fw, &cb, &fw->ctx[CTX_PART_MODE], cx, cy);
        } else {
            if (cx == 0) {      /* this row's record stream */
                fw->rec_buf = fw->rrec[cy].buf;
                fw->rec_pos = 0;
            }
            n265_entropy_ctu(fw, &cb, cx, cy);
        }
        n265_cabac_terminate(&cb, i == last);
    }
    fw->in_entropy = 0;
    bs_align_zero(&fw->rbsp);


}

/* ---- the shot sidecar (--shot-stats / N265_SHOT_STATS) ----
 *
 * A SHOT is the run of pictures between the intra pictures the lookahead
 * placed: an IDR (a scene cut it took, or the --keyint period) or a
 * non-IDR I (the scene cut the IDR bound refused, la_is_nonidr_i). The
 * first shot of the encode opens at "start" - its opener is the
 * encoder's own first IDR and says nothing about the content.
 *
 * Everything here runs on the driver inside join_oldest and at close,
 * reads only fields frozen at dispatch plus the frame's packed length,
 * and writes nothing the encoder reads back. Output-neutral by
 * construction, and the same file at every thread count.
 */
static void shot_flush(n265_encoder *e)
{
    static const char *const kind[] = { "start", "start", "idr", "intra" };
    if (!e->shot_f || e->shot_n == 0)
        return;
    fprintf(e->shot_f,
            "{\"v\":1,\"first\":%d,\"last\":%d,\"open\":\"%s\","
            "\"frames\":{\"i\":%d,\"p\":%d,\"b_ref\":%d,\"b_leaf\":%d},"
            "\"bytes\":%llu,\"qp_base_mean\":%.4f,\"complexity\":%.0f,"
            "\"flat_frames\":%d}\n",
            e->shot_first, e->shot_last, kind[e->shot_open],
            e->shot_cnt[0], e->shot_cnt[1], e->shot_cnt[2], e->shot_cnt[3],
            (unsigned long long)e->shot_bytes,
            e->shot_qp_sum / (double)e->shot_n, e->shot_cplx, e->shot_flat);
    fflush(e->shot_f);
    e->shot_n = 0;
}

/* Fold one frame, in display order, into the shot being accumulated. */
static void shot_fold(n265_encoder *e, const struct n265_shot_rec *r, int d)
{
    if (r->slice_type == 2 || e->shot_n == 0) {
        int open = r->slice_type != 2 ? 1 : r->idr ? 2 : 3;
        shot_flush(e);                  /* the previous shot ends here */
        e->shot_open = e->shot_open == 0 ? 1 : open;
        e->shot_first = d;
        memset(e->shot_cnt, 0, sizeof e->shot_cnt);
        e->shot_bytes = 0;
        e->shot_qp_sum = e->shot_cplx = 0.0;
        e->shot_flat = 0;
    }
    e->shot_cnt[r->slice_type == 2 ? 0 : r->slice_type == 1 ? 1
                                       : r->nonref ? 3 : 2]++;
    e->shot_bytes += r->bytes;
    e->shot_qp_sum += r->qp_base;
    e->shot_cplx += r->cplx;
    e->shot_flat += r->flat != 0;
    e->shot_last = d;
    e->shot_n++;
}

/* Fold every buffered record with a display index below `upto`. `hard`
 * folds across a hole (the drain at close); otherwise the walk stops at
 * the first index that has not retired yet, which is what keeps the
 * sidecar in display order. */
static void shot_fold_upto(n265_encoder *e, int upto, int hard)
{
    while (e->shot_next < upto) {
        struct n265_shot_rec *r = &e->shot_ring[e->shot_next % N265_SHOT_RING];
        if (!r->present) {
            if (!hard)
                return;
        } else {
            r->present = 0;
            shot_fold(e, r, e->shot_next);
        }
        e->shot_next++;
    }
}

/* The tap itself: one retired frame, with the bytes its NALs packed to
 * (the headers ride on the first frame, so the shot totals sum to the
 * file). */
static void shot_record(n265_encoder *e, const n265_frame_work *fw,
                        size_t bytes)
{
    int d = fw->display_idx;
    /* The reorder can never reach the ring's width, but a shortened ring
     * would silently overwrite a frame instead of failing, so the guard
     * folds what is there rather than trusting the bound. */
    if (d >= e->shot_next + N265_SHOT_RING)
        shot_fold_upto(e, d - N265_SHOT_RING + 1, 1);
    struct n265_shot_rec *r = &e->shot_ring[d % N265_SHOT_RING];
    r->slice_type = fw->slice_type;
    r->idr = fw->idr;
    r->nonref = fw->nonref;
    r->flat = fw->flat;
    r->qp_base = fw->rc_base;
    r->cplx = fw->rc_cplx;
    r->bytes = (uint64_t)bytes;
    r->present = 1;
    if (d >= e->shot_high)
        e->shot_high = d + 1;
    shot_fold_upto(e, e->shot_high, 0);
}

/* The share of a finished picture whose final mode was intra, as a pair
 * of 4x4-cell counts: `cells` every cell of the coded picture, `intra`
 * the ones a CU coded intra. Read from `blk_intra`, the mode map the
 * deblocker and the collocated publish already read, after the frame's
 * row jobs have been joined - so it is an integer pair that does not
 * move with the thread count. A PCM picture has no mode map and reports
 * nothing. */
static void n265_intra_share(n265_frame_work *fw, uint32_t *cells,
                             uint32_t *intra)
{
    uint32_t nc = 0, ni = 0;
    if (fw->mode != N265_MODE_PCM && fw->blk_intra) {
        int cw = fw->pad_w >> 2, ch = fw->pad_h >> 2;
        for (int y = 0; y < ch; y++) {
            const uint8_t *r = fw->blk_intra + (size_t)y * fw->map_stride;
            for (int x = 0; x < cw; x++)
                ni += (r[x] != 0);
        }
        nc = (uint32_t)cw * (uint32_t)ch;
    }
    *cells = nc;
    *intra = ni;
}

/* The buffering_period and pic_timing values for the access unit that is
 * about to be packed. hrd.h states the four derivations and why each is
 * what it is; this is where the plan's own numbers are spent.
 *
 * It runs on the DRIVER, inside join_oldest, in coding order, and reads
 * nothing else: the coding index and the POC were fixed at dispatch, the
 * reorder depth at open, and the CPB fill is the walk this same function
 * drives one access unit at a time. So the message is a pure function of
 * the plan and the coded sizes - byte-identical at one worker and at
 * twelve, which is the property the determinism gate reads.
 *
 * It is deliberately NOT built at dispatch, where the grain SEI is built.
 * The fill this AU is really removed against is only a number once every
 * earlier AU has retired, and while N265_RC_LAG is 0 that is also true at
 * dispatch, a lag would make the value stale there and correct here. */
static void hrd_sei_values(n265_encoder *e, const n265_frame_work *fw,
                           n265_hrd_sei *v)
{
    memset(v, 0, sizeof *v);
    /* Only an IRAP opens a buffering period, and the only IRAP this
     * encoder writes is the IDR (a non-IDR I is a trailing picture). */
    v->bp = fw->idr;
    if (v->bp)
        e->hrd_cvs_code = fw->code_idx;
    /* C.2.3: clock ticks from the removal of the access unit that carried
     * the most recent PRECEDING buffering period. One picture per tick, so
     * it is a difference of coding indices. */
    int64_t cpb = (int64_t)fw->code_idx - e->hrd_bp_code;
    if (cpb < 1)
        cpb = 1;
    if (cpb > N265_HRD_DELAY_MAX)
        cpb = N265_HRD_DELAY_MAX;
    v->cpb_delay = (uint32_t)cpb;
    int64_t dpb = (int64_t)e->dpb_reorder + fw->poc
                - ((int64_t)fw->code_idx - e->hrd_cvs_code);
    if (dpb < 0)                /* the reorder bound is what forbids this */
        dpb = 0;
    if (dpb > N265_HRD_DELAY_MAX)
        dpb = N265_HRD_DELAY_MAX;
    v->dpb_delay = (uint32_t)dpb;
    if (v->bp) {
        /* The initial delay is the CPB fill this access unit is removed
         * against, expressed as the time it takes to arrive at the
         * DECLARED BitRate: a decoder that starts here waits exactly that
         * long and then holds what the encoder's clamp assumed it held.
         * Rounded UP, so the wait is never shorter than the fill the
         * encoder planned against; the declared pair is itself rounded up
         * (E.2.3, at open), so both roundings point the same way.
         *
         * At the first access unit the fill is --vbv-init * --vbv-bufsize
         * exactly, which is where vbv.c starts its walk and what
         * docs/phase12.md item 2 wrote down as this message's value. At a
         * later IDR it is the fill the stream really arrives at, which is
         * the only honest thing to say to a decoder that random-accesses
         * there. */
        double rate = (double)e->hrd_bit_rate_bits;
        double fill = e->vbv.fill;
        if (fill < 0.0)         /* an underflow is a fact; a negative wait
                                 * is not. The cell is reported by
                                 * scripts/vbv_check.py either way. */
            fill = 0.0;
        double full = floor(90000.0 * (double)e->hrd_cpb_size_bits / rate);
        if (full < 1.0)
            full = 1.0;
        if (full > (double)N265_HRD_DELAY_MAX)
            full = (double)N265_HRD_DELAY_MAX;
        double del = ceil(90000.0 * fill / rate);
        if (del < 1.0)          /* D.3.2: the delay shall not be 0 */
            del = 1.0;
        if (del > full)
            del = full;
        v->init_delay = (uint32_t)del;
        /* D.3.2's pair sums to the time the CPB takes to fill, which is
         * the bound the delay itself is read against. */
        v->init_offset = (uint32_t)(full - del);
        e->hrd_bp_code = fw->code_idx;
    }
}

/* Join the oldest frame in flight: wait for its job, pack its NALs into
 * the deferred ring in CODING order, and hand its resources back. This
 * is the only place the driver blocks on a frame, and it is where D3's
 * stalls (a DPB slot, a lookahead source slot, a frame_work) resolve. */
static void join_oldest(n265_encoder *e)
{
    if (e->n_inflight == 0)
        return;
    n265_frame_work *fw = e->inflight[0];
    double t_j0 = e->la_prof ? prof_now() : 0.0;
    ntp_join(e->pool, &fw->job);
    double t_j1 = e->la_prof ? prof_now() : 0.0;
    if (e->la_prof) e->prof_join += t_j1 - t_j0;
    struct n265_done *d = done_slot(e);
    if (fw->emit_headers)
        for (int i = 0; i < e->nhdr; i++)
            pack_nal(d, e->hdr_type[i], e->hdr_rbsp[i], e->hdr_len[i]);
    /* 7.4.2.4.4: a prefix SEI follows the parameter sets of its access
     * unit and precedes the first VCL NAL of that unit. The HRD timing
     * pair leads the grain message, for the reason headers.c gives. */
    if (e->hrd_on) {
        n265_hrd_sei v;
        hrd_sei_values(e, fw, &v);
        n265_write_hrd_sei(&e->hrd_bs, &v);
        pack_nal(d, N265_NAL_PREFIX_SEI, e->hrd_bs.buf, e->hrd_bs.bytes);
    }
    if (fw->hdr_sei_len)
        pack_nal(d, N265_NAL_PREFIX_SEI, fw->hdr_sei, fw->hdr_sei_len);
    if (fw->sei_len)
        pack_nal(d, N265_NAL_PREFIX_SEI, fw->sei, fw->sei_len);
    pack_nal(d, fw->nal_type, fw->rbsp.buf, fw->rbsp.bytes);
    /* --hash: the decoded picture hash, D.2.19. A SUFFIX SEI, so it comes
     * AFTER the access unit's VCL NAL (7.4.2.4.4) - the only suffix NAL
     * this encoder writes - and it is computed HERE, on the driver, off
     * the finished reconstruction in coding order. That placement is the
     * whole determinism argument: the recon is byte-identical at every
     * thread count, the walk over it reads nothing else, so the message
     * is too. */
    if (e->sig.hash) {
        n265_pic_hash h;
        hash_frame(e, fw, &h);
        n265_write_hash_sei(&e->hash_bs, &h, 3);
        pack_nal(d, N265_NAL_SUFFIX_SEI, e->hash_bs.buf, e->hash_bs.bytes);
    }
    e->n_done++;
    dump_recon_frame(e, fw);
    /* N265_DEPTH_DUMP: the frame's per-CTU training records, in raster
     * order. The row jobs filled their own slots; the write happens here,
     * on the driver, in coding order, so the file is a pure function of
     * the encode and not of the schedule. */
    if (fw->dd_rec) {
        FILE *df = fopen(depth_dump_file(), "a");
        if (df) {
            for (int cy = 0; cy < fw->ctb_rows; cy++)
                for (int cx = 0; cx < fw->ctb_cols; cx++) {
                    const struct n265_depth_rec *r =
                        &fw->dd_rec[cy * fw->ctb_cols + cx];
                    fprintf(df,
                        "DD,%d,%d,%d,%d,%d,%d,%d,%d,%.3f,%.3f,%.3f,"
                        "%u,%u,%u,%u,%.4f,%.2f,%.2f,%d,%d\n",
                        fw->display_idx, fw->slice_type, fw->b_depth,
                        fw->nonref, fw->flat, cx, cy, r->qp,
                        r->ctqp, r->lvar, r->fine,
                        r->imean, r->imax, r->pmean, r->pmax,
                        r->ratio, r->mvmean, r->mvspread,
                        r->dmin, r->dmax);
                }
            fclose(df);
        }
    }
    /* N265_STATIC_COMMIT (phase 11 item 4): stamp this frame's coded-skip
     * map into the ring. Here, on the driver, where join_oldest has
     * already serialised coding order, so the ring's contents are a pure
     * function of the encode; sc_resolve then reads only the part of it
     * that no thread count can change. */
    if (n265_static_commit() && fw->blk_skip) {
        size_t cells = (size_t)fw->map_stride * (fw->pad_h >> 2);
        struct n265_sc_hist *sh = &e->sc_hist[fw->code_idx % N265_SC_HIST];
        if (!sh->map || e->sc_cells != cells) {
            /* the geometry is fixed for the encode; the test is here so a
             * short read can never write past a map sized for another */
            if (e->sc_cells != cells) {
                for (int i = 0; i < N265_SC_HIST; i++) {
                    free(e->sc_hist[i].map);
                    e->sc_hist[i].map = NULL;
                    e->sc_hist[i].cidx = -1;
                }
                e->sc_cells = cells;
                sh = &e->sc_hist[fw->code_idx % N265_SC_HIST];
            }
            sh->map = malloc(cells);
        }
        if (sh->map) {
            memcpy(sh->map, fw->blk_skip, cells);
            sh->cidx = fw->code_idx;
            sh->didx = fw->display_idx;
        }
    }
    /* N265_CU_TRACE: the frame's per-node census, in raster order, with
     * the co-located-skip column resolved here against the previous
     * frame in CODING order - which is the order join_oldest retires in,
     * so the column is the same integer at every thread count. */
    if (fw->cu_rec) {
        if (!e->cu_trace_f)
            e->cu_trace_f = fopen(cu_trace_file(), "ab");
        if (e->cu_trace_f) {
            struct n265_cu_rec h;
            memset(&h, 0, sizeof(h));
            h.kind = N265_CUT_FRAME;
            h.cyc_tot = (uint32_t)fw->code_idx;
            h.cyc_kids = (uint32_t)fw->poc;
            h.cyc_merge = (uint32_t)fw->ctb_cols;
            h.cyc_amvp = (uint32_t)fw->ctb_rows;
            h.cyc_intra = (uint32_t)fw->display_idx;
            h.slice_type = (uint8_t)fw->slice_type;
            h.qp = (uint8_t)fw->qp;
            h.log2n = (uint8_t)fw->ctb_log2;
            h.flags = (uint8_t)(fw->flat ? N265_CUF_FLAT : 0);
            fwrite(&h, sizeof(h), 1, e->cu_trace_f);
            int cw = fw->pad_w >> 2, ch = fw->pad_h >> 2;
            for (int k = 0; k < fw->ctb_cols * fw->ctb_rows; k++) {
                int cnt = fw->cu_rec_n[k];
                for (int i = 0; i < cnt; i++) {
                    struct n265_cu_rec *r =
                        &fw->cu_rec[k * N265_CU_REC_MAX + i];
                    r->colskip = 255;
                    if (e->cu_prev_valid && e->cu_prev_skip) {
                        int cells = 1 << (r->log2n - 2), got = 0, tot = 0;
                        for (int cy = r->y >> 2;
                             cy < (r->y >> 2) + cells && cy < ch; cy++)
                            for (int cx = r->x >> 2;
                                 cx < (r->x >> 2) + cells && cx < cw; cx++) {
                                got += e->cu_prev_skip[(size_t)cy *
                                                       fw->map_stride + cx] != 0;
                                tot++;
                            }
                        r->colskip = (uint8_t)(tot ? got * 100 / tot : 255);
                    }
                }
                if (cnt)
                    fwrite(&fw->cu_rec[k * N265_CU_REC_MAX],
                           sizeof(struct n265_cu_rec), (size_t)cnt,
                           e->cu_trace_f);
            }
            if (!e->cu_prev_skip)
                e->cu_prev_skip = malloc((size_t)fw->map_stride *
                                         (fw->pad_h >> 2));
            if (e->cu_prev_skip && fw->blk_skip) {
                memcpy(e->cu_prev_skip, fw->blk_skip,
                       (size_t)fw->map_stride * (fw->pad_h >> 2));
                e->cu_prev_valid = 1;
            }
        }
    }
    if (fw->cu_stats) {
        /* one CSV row per frame: what the entropy pass actually wrote.
         * Shares are of the coded luma area, so they sum to 1. Printed
         * here, on the driver, so the rows come out in coding order
         * however the entropy tasks were scheduled. */
        double a = 0;
        for (int i = 0; i < N265_CS_ZEROCBF; i++) a += (double)fw->cs_area[i];
        if (a != (double)fw->pad_w * fw->pad_h)
            fprintf(stderr, "CUS-BUG frame %d: area %.0f != %d\n",
                    fw->display_idx, a, fw->pad_w * fw->pad_h);
        if (a <= 0) a = 1;
        double tu = 0;
        for (int i = 0; i < 4; i++) tu += (double)fw->cs_tu[i];
        if (tu <= 0) tu = 1;
        fprintf(stderr,
                "CUS,%d,%d,%d,%d,%zu,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,"
                "%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,%llu\n",
                fw->display_idx, fw->slice_type, fw->b_depth, fw->qp,
                d->len,
                fw->cs_area[N265_CS_SKIP] / a, fw->cs_area[N265_CS_MERGE] / a,
                fw->cs_area[N265_CS_AMVP] / a, fw->cs_area[N265_CS_RECT] / a,
                fw->cs_area[N265_CS_AMP] / a, fw->cs_area[N265_CS_INTRA] / a,
                fw->cs_area[N265_CS_INTRA_NXN] / a,
                fw->cs_area[N265_CS_ZEROCBF] / a,
                fw->cs_tu[0] / tu, fw->cs_tu[1] / tu, fw->cs_tu[2] / tu,
                fw->cs_tu[3] / tu,
                (double)(fw->cs_tu_cbf[0] + fw->cs_tu_cbf[1] +
                         fw->cs_tu_cbf[2] + fw->cs_tu_cbf[3]) / tu,
                (unsigned long long)fw->cs_coef);
    }
    if (fw->cbit_trace) {
        /* one CSV row per frame: the coded size, and the bits inside it
         * that carry each component. Printed on the driver beside the CUS
         * row, in coding order, for the same reason. */
        fprintf(stderr,
                "CBIT,%d,%d,%d,%d,%d,%zu,%.1f,%.1f,%.1f,%.1f,%.1f,"
                "%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu\n",
                fw->display_idx, fw->slice_type, fw->b_depth, fw->nonref,
                fw->qp, d->len,
                fw->cbit[N265_CB_Y] / 256.0, fw->cbit[N265_CB_CB] / 256.0,
                fw->cbit[N265_CB_CR] / 256.0, fw->cbit[N265_CB_CBF] / 256.0,
                fw->cbit[N265_CB_OTHER] / 256.0,
                (unsigned long long)fw->cbit_cnz,
                (unsigned long long)fw->cbf_c_n[1],
                (unsigned long long)fw->cbf_c_cb[1],
                (unsigned long long)fw->cbf_c_cr[1],
                (unsigned long long)fw->cbf_c_n[2],
                (unsigned long long)fw->cbf_c_cb[2],
                (unsigned long long)fw->cbf_c_cr[2],
                (unsigned long long)fw->cbf_c_n[3],
                (unsigned long long)fw->cbf_c_cb[3],
                (unsigned long long)fw->cbf_c_cr[3]);
    }
    /* Round 4: the frame's traffic, summed over its row lanes and
     * stamped with its coding index. Row-private lines summed in row
     * order on the driver, so the total is the same integer at every
     * thread count whatever order the lanes ran in. */
    {
        struct n265_hf2_hist *hh =
            &e->hf2_hist[fw->code_idx % N265_HF2_HIST];
        hh->cidx = fw->code_idx;
        hh->is_ref = fw->mode != N265_MODE_PCM && !fw->nonref;
        memset(hh->samp, 0, sizeof hh->samp);
        for (int r = 0; r <= e->ctb_rows; r++)
            for (int j = 0; j < 24; j++)
                hh->samp[j] += fw->hf2_tr_rows[(size_t)r * 24 + j];
        e->hf2_nref += (unsigned)hh->is_ref;     /* N265_HF2_TRACE only */
        for (int j = 0; j < 24; j++)
            e->hf2_samp[j] += hh->samp[j];
    }
    /* Step 3: the frame's J pair, stamped with its coding index for the
     * adaptive gate to read back. Row-private counters summed in row
     * order on the driver, so the pair is the same integer at every
     * thread count whatever order the lanes ran in. */
    if (e->iw_adapt) {
        struct n265_iw_hist *ih = &e->iw_hist[fw->code_idx % N265_IW_HIST];
        uint64_t ji = 0, js = 0;
        for (int r = 0; r <= e->ctb_rows; r++) {
            ji += fw->iw_arm_rows[(size_t)r * N265_IW_CNT + 4];
            js += fw->iw_arm_rows[(size_t)r * N265_IW_CNT + 5];
        }
        ih->cidx = fw->code_idx;
        ih->slice_type = fw->slice_type;
        ih->armed = !fw->iw_gate && fw->mode != N265_MODE_PCM;
        ih->j_inter = ji;
        ih->j_saved = js;
    }
    /* Step 3: the frame's own intra share, counted on the driver from
     * the finished mode map. `blk_intra` is written by the row jobs and
     * every one of them has been joined by the time we are here, so the
     * pair of integers is the same at one worker and at twelve. */
    if (e->iw_trace) {
        uint32_t cells = 0, intra = 0;
        uint64_t arm[N265_IW_CNT] = { 0 };
        n265_intra_share(fw, &cells, &intra);
        for (int r = 0; r <= e->ctb_rows; r++)
            for (int j = 0; j < N265_IW_CNT; j++)
                arm[j] += fw->iw_arm_rows[(size_t)r * N265_IW_CNT + j];
        /* IW,display,coding,slice,depth,nonref,flat,cells,intra,pct,
         *    reached,gated,ranks,trials,j_inter,j_saved,j_saved_pct */
        fprintf(stderr, "IW,%d,%d,%d,%d,%d,%d,%u,%u,%.3f,"
                        "%llu,%llu,%llu,%llu,%llu,%llu,%.4f\n",
                fw->display_idx, fw->code_idx, fw->slice_type, fw->b_depth,
                fw->nonref, fw->flat, cells, intra,
                cells ? 100.0 * (double)intra / (double)cells : 0.0,
                (unsigned long long)arm[0], (unsigned long long)arm[1],
                (unsigned long long)arm[2], (unsigned long long)arm[3],
                (unsigned long long)arm[4], (unsigned long long)arm[5],
                arm[4] ? 100.0 * (double)arm[5] / (double)arm[4] : 0.0);
    }
    if (n265_dup_trace())
        dup_trace_retire(e, fw, d->len);
    /* --shot-stats: the frame joins its shot here, re-sorted into
     * display order; the shot's line is written when the next shot's
     * opener folds (or at close, for the last one). */
    if (e->shot_f)
        shot_record(e, fw, d->len);
    /* D4: the actual bits are applied on the driver, in coding order.
     * Capped CRF joins ABR here: the clamp predicts from the class scales
     * and the scales are learned nowhere else, so a CRF encode with a VBV
     * has to keep the same books an ABR one does. */
    if (e->abr_on || e->rc.trace || e->vbv.on)
        n265_rc_done(e, fw->rc_cls, fw->rc_cplx, fw->rc_base,
                     (double)d->len * 8.0, fw->rc_planned, fw->rc_ctf,
                     fw->rc_ioff);
    /* --pass 1 (and --pass 3, which refreshes what it planned from): one
     * line per frame in CODING order, which is retirement order. It reads
     * only fields frozen at dispatch plus the packed length, writes nothing
     * the encoder reads back, and runs on the driver - so the sidecar is
     * the same file at one worker and at twelve, which is what lets the
     * plan it feeds be a determinism claim rather than a hope. */
    if (e->tp.out) {
        n265_2p_frame r;
        memset(&r, 0, sizeof r);
        r.coding = fw->code_idx;
        r.display = fw->display_idx;
        r.slice_type = fw->slice_type;
        r.nonref = fw->nonref;
        r.flat = fw->flat;
        r.qp = fw->rc_base;
        r.bits = (double)d->len * 8.0;
        r.cplx = fw->rc_cplx;
        r.icost = fw->st_icost;
        r.pcost = fw->st_pcost;
        r.prop = fw->st_prop;
        r.ctf = fw->rc_ctf;
        n265_2p_write(&e->tp, &r);
    }
    /* The buffer, moved by the ACCESS UNIT that just retired - d->len is
     * the whole AU, parameter sets and all, which is what a CPB removes.
     * Driver-serial and in coding order, which is the removal order, so the
     * trajectory is the same at one worker and at twelve. */
    if (e->vbv.on) {
        double fb = 0.0, fa = 0.0;
        n265_vbv_update(&e->vbv, (double)d->len * 8.0, &fb, &fa);
        if (e->vbv_trace)
            /* VBV,coding,display,slice,pred,allow,bits,fill_in,fill_out,dqp */
            fprintf(stderr, "VBV,%d,%d,%d,%.0f,%.0f,%.0f,%.1f,%.1f,%+.3f\n",
                    fw->code_idx, fw->display_idx, fw->slice_type,
                    fw->vbv_pred, fw->vbv_allow, (double)d->len * 8.0,
                    fb, fa, fw->vbv_dqp);
    }
    fw->slot->in_use = 0;
    fw->emit_headers = 0;
    fw->in_flight = 0;
    for (int i = 1; i < e->n_inflight; i++)
        e->inflight[i - 1] = e->inflight[i];
    e->n_inflight--;
    if (e->la_prof) {
        double t = prof_now();
        e->prof_retire += t - t_j1;
        /* SFRM: the frame's critical path, one row per frame in CODING
         * order. Every column is seconds since encoder open: when the
         * driver dispatched it, when its first row started, when its
         * last publish row finished, the entropy pass, and when the
         * driver finished retiring it. */
        if (e->sprof >= 2)
            fprintf(stderr, "SFRM,%d,%d,%d,%d,%d,%.6f,%.6f,%.6f,%.6f,"
                            "%.6f,%.6f,%.6f\n",
                    fw->code_idx, fw->display_idx, fw->slice_type,
                    fw->b_depth, fw->nonref, fw->t_disp, fw->t_row0,
                    fw->t_rowlast, fw->t_ent0, fw->t_ent1,
                    t_j1 - e->prof_t0, t - e->prof_t0);
    }
}

/* Build the reference lists for the picture that follows POC `cur`
 * (or, with `future` set, for a B whose L1 starts at `future`). `ref_used`
 * near the top of this file is the POC-level mirror of the rule below and
 * is what the retirement's lifetimes are computed from; the two move
 * together. L0 = valid
 * DPB pictures before cur, nearest first, up to ref_max; L1 = the future
 * picture then the past ones. A caller coding a B passes the past anchor
 * POC as `cur` so the L0 head is that anchor. */
void n265_build_lists(n265_encoder *e, n265_frame_work *fw, int poc,
                      int is_b, struct n265_refpic *future)
{
    (void)future;
    /* every valid DPB picture, split at `poc`: past = poc < this, nearest
     * (largest POC) first; future = poc > this, nearest (smallest) first.
     * L0 = past then future (for B); L1 = future then past (B only). */
    struct n265_refpic *past[N265_DPB_SLOTS], *fut[N265_DPB_SLOTS];
    int np = 0, nf = 0;
    /* D2: `marked` is the DPB as the decoder sees it, and it is
     * driver-serial. The sort below is a total order on POC - POCs are
     * unique among marked pictures, since an IDR clears the set - so
     * slot index never breaks a tie and list order cannot depend on
     * which slot a picture occupies. */
    for (int sl = 0; sl < e->dpb_slots; sl++) {
        if (!e->dpb[sl].marked) continue;
        if (e->dpb[sl].poc < poc) past[np++] = &e->dpb[sl];
        else if (e->dpb[sl].poc > poc) fut[nf++] = &e->dpb[sl];
    }
    for (int i = 1; i < np; i++)
        for (int k = i; k > 0 && past[k]->poc > past[k - 1]->poc; k--) { struct n265_refpic *t = past[k]; past[k] = past[k - 1]; past[k - 1] = t; }
    for (int i = 1; i < nf; i++)
        for (int k = i; k > 0 && fut[k]->poc < fut[k - 1]->poc; k--) { struct n265_refpic *t = fut[k]; fut[k] = fut[k - 1]; fut[k - 1] = t; }
    fw->nref[0] = fw->nref[1] = 0;
    for (int i = 0; i < np && fw->nref[0] < e->ref_max; i++) fw->refl[0][fw->nref[0]++] = past[i];
    if (is_b) for (int i = 0; i < nf && fw->nref[0] < e->ref_max; i++) fw->refl[0][fw->nref[0]++] = fut[i];
    if (is_b) {
        for (int i = 0; i < nf && fw->nref[1] < e->ref_max; i++) fw->refl[1][fw->nref[1]++] = fut[i];
        for (int i = 0; i < np && fw->nref[1] < e->ref_max; i++) fw->refl[1][fw->nref[1]++] = past[i];
    }
}

/* The minimum distance between the last key frame and a SCENECUT key
 * frame, in display frames. `--keyint` still forces one on its own
 * period; this only bounds how close together the detector may place
 * them. Ours was 4 - one mini-GOP - which is a lower bound with no
 * derivation, and on cut-out animation the detector reaches it: the
 * lookahead's pcost/icost ratio sits near 1 on every frame that moves,
 * so every anchor becomes an IDR (sita_toon: 30 of 120 frames intra,
 * 65% of the file). x265's own default is auto = min(keyint/10, fps),
 * i.e. 24-25 at keyint 250, and the same argument applies here: an IDR
 * costs the whole prediction chain, so the detector has to be very sure
 * before it may place one close to the last. N265_MIN_KEYINT sets it;
 * 0 means the auto rule; 4 is the default and the pre-2026-09-09
 * behaviour.
 *
 * Refused at 0 on 2026-09-09 because the bound had nothing to put at a
 * suppressed cut, and REFUSED AGAIN on 2026-09-12 with the non-IDR
 * intra picture built and conformant underneath it: at the shipped
 * `--ref 1` a picture cannot cross the new kind either, the lists being
 * one deep, so all it moves is which side of the cut a leaf's L1 sits
 * on (sintel_720p +0.19 CQP / -0.08 CRF, sita_toon_720p +0.02 / -0.02,
 * the other thirteen clips byte-identical). At `--ref 2` the same arm
 * is worth -0.36 / -0.25 on sita_toon and -0.41 / -0.46 on sintel
 * (CQP / CRF), which is the re-pricing lead.
 * docs/phase7-gop-design.md "Step 2 result". */
/* the knob half, split out so the warm can reach it without an encoder */
static int min_keyint_env(void)
{
    static int v = -2;
    if (v == -2) { const char *s = n265_env("N265_MIN_KEYINT"); v = s ? atoi(s) : 4; }
    return v;
}

/* The same distance for an INTRA PICTURE rather than for an IDR:
 * N265_MIN_KEYINT bounds how close two IDRs may be, this bounds how
 * close two intra pictures may be, and the gap between the two is where
 * the non-IDR I lives. 4 - one mini-GOP - is the floor the IDR used to
 * carry, kept here because it is the one with a derivation behind it: a
 * cut closer to the last intra picture than the shortest run the
 * scheduler can plan has nothing to save. At or above N265_MIN_KEYINT
 * the non-IDR I can never be placed at all, which is why the shipped
 * pair (4, 4) is the pre-item encoder byte for byte and why the picture
 * kind reaches the bitstream only under `N265_MIN_KEYINT=0` or the
 * forced knob below. */
static int intra_min_env(void)
{
    static int v = -2;
    if (v == -2) { const char *s = n265_env("N265_INTRA_MIN"); v = s ? atoi(s) : 4; }
    return v;
}

/* N265_FORCE_NONIDR: diagnostic, and the conformance vectors' only way
 * in. A non-IDR intra picture at this DISPLAY index whatever the
 * detector says, so a synthetic source with no scene cut in it can still
 * carry the picture kind through both decoders. -1 (unset) is off. */
static int force_nonidr_env(void)
{
    static int v = -2;
    if (v == -2) { const char *s = n265_env("N265_FORCE_NONIDR"); v = s ? atoi(s) : -1; }
    return v;
}

/* This file's own cached knobs. The others are warmed per-file
 * above; these sit between a driver-side caller and a worker-side one
 * (hf_arm runs under the publish task), so they get the same treatment.
 * Defined here because min_keyint_env is, and declared in encoder.h so
 * n265_encoder_open can call it beside the rest. */
void n265_encoder_warm(void)
{
    (void)dpb_hold(); (void)hf_cache_on(); (void)hf2_mode_env();
    (void)hf2_tpp_env(); (void)hf2_tpp14_env();
    (void)min_keyint_env(); (void)intra_min_env(); (void)force_nonidr_env();
    (void)depth_dump_file();
}

static int min_keyint(n265_encoder *e)
{
    int m = min_keyint_env();
    if (m == 0) {
        int fps = e->param.fps_den > 0
                      ? (e->param.fps_num + e->param.fps_den / 2) / e->param.fps_den
                      : 25;
        m = e->keyint / 10;
        if (fps < m) m = fps;
    }
    return m < 1 ? 1 : m;
}

/* Is the lookahead frame at index i a forced key frame (IDR)? keyint,
 * a scene cut, or the encoder's first frame. */
static int la_is_idr(n265_encoder *e, int i)
{
    int d = e->la.f[i].display_idx;
    return e->mode == N265_MODE_PCM || e->keyint == 1 || (d % e->keyint) == 0 ||
           !e->ref_valid ||
           (e->scenecut && d - e->gop_start_display >= min_keyint(e) &&
            n265_la_scenecut(e, i));
}

/* Is the lookahead frame at index i a NON-IDR intra picture - an I slice
 * coded as a trailing reference, keeping the POC line and the DPB? It
 * takes the scene cuts the IDR bound above refuses, which is the whole
 * reason that bound can now stand at x265's value: the cut is coded
 * where the window sees it, and nothing before it is thrown away. Far
 * enough from the last intra picture to be worth an intra frame at all
 * (N265_INTRA_MIN), and never where an IDR is already going.
 *
 * Gated on ref_max >= 2 (2026-09-12, step 2 follow-up). The condition is
 * structural and not content: with one reference per list the nearest
 * past reference for everything after the cut is the intra picture
 * itself, so keeping the pre-cut pictures marked buys a longer RPS and
 * no prediction, and all the placement can move is which side of the cut
 * a leaf's L1 sits on - measured at sintel_720p +0.19 CQP, a loss. At
 * ref_max >= 2 a list is deep enough to hold a picture from the far side
 * of the cut and the same arm pays on every clip that moves. The forced
 * knob stays ABOVE the gate on purpose: it is a diagnostic and the
 * conformance vectors' only way in, and the ref-1 column of the
 * mechanism table is read through it. */
static int la_is_nonidr_i(n265_encoder *e, int i)
{
    if (e->mode == N265_MODE_PCM || la_is_idr(e, i))
        return 0;
    int d = e->la.f[i].display_idx;
    if (d == force_nonidr_env())
        return 1;
    if (e->ref_max < 2)
        return 0;
    return e->scenecut && d - e->last_intra_display >= intra_min_env() &&
           n265_la_scenecut(e, i);
}

/* ---- the shot plan (--shot-window, phase 7 item 6 step 2) ----
 *
 * The encoder allocates across FRAMES already: the cu-tree frame term
 * and the AQ_DC frame mean both move the base QP inside the streaming
 * window. What neither can do is allocate across a SHOT, because a shot
 * is longer than the window - so a hard shot and an easy one next to it
 * are each coded at the same CRF base whatever they are worth. The plan
 * is one number per shot: with the shot (or as much of it as
 * `--shot-window` holds) in the queue, price it with rc.c's own
 * complexity model and offset the whole shot's base QP.
 *
 * The model is the one in rc.c's header: bits go as
 * `(complexity / nblk)^qcomp`, so two shots a factor r apart in
 * complexity sit `(1 - qcomp) * log2(r)` QP steps apart under that
 * allocation, and `N265_SHOT_K` is the share of it taken. Positive on a
 * shot HARDER than the level, so a hard shot is coded coarser and an
 * easy one finer.
 *
 * That is x264's qcomp trade and NOT quality levelling, which is what
 * the item wanted and what step 2 measured it against: the term is worth
 * about 2% of across-shot BD-rate on both multi-shot suites and it makes
 * the per-shot quality spread WIDER, not narrower. The levelling
 * direction is the same expression at k < 0 and costs 3.0-3.4% of BD.
 * The numbers are in docs/phase7-shot-design.md "Step 2 result"; the
 * short version is that lookahead complexity is a bit-allocation signal
 * and does not rank shots the way the metric does.
 *
 * `cplx_ref` is a running level over the shots already planned, seeded
 * by the first one: nothing here looks past the window, so the mode is
 * streaming-compatible and the first shot's offset is exactly zero.
 *
 * Three things it deliberately does NOT do. It does not touch the window
 * anything else reads (n265_la_view): the queue is deeper, the cu-tree
 * horizon, the b-adapt run and the rate control's walk are not, so the
 * mode's only effect on the encoder is this offset. It does not reach
 * `n265_planned_qp`, the b-adapt bits model, for the same reason - the
 * run lengths it ranks are the ones the default picks. And it plans a
 * shot ONCE, when the front of the queue first reaches it, so a frame's
 * offset is a function of the queue at that moment and not of when the
 * frame happens to be dispatched.
 *
 * The mean is taken over the shot's INTER frames, which is w_avg's rule
 * in rc.c and for w_avg's reason: an intra frame is worth several
 * ordinary frames and folding one in would read the shot as harder than
 * its content is - the more so on a short shot, which is exactly where a
 * plan is being asked for. */
/* Does queued frame `i` OPEN a shot - is it an intra picture the
 * lookahead will place there?
 *
 * At the front of the queue that is la_is_idr / la_is_nonidr_i outright,
 * because the frame is the one about to be dispatched. Deeper in the
 * queue their PRESENT-tense clauses do not apply: `!e->ref_valid` says
 * the frame being dispatched has nothing to predict from, which is true
 * of the whole window at the head of an encode and would read every
 * queued frame as a cut - one-frame shots, and a level that is the
 * opening IDR's intra cost. That is the same distinction rc.c draws in
 * n265_rc_future_idr, which is the rule used here, plus the non-IDR I's
 * own (`--ref 2` and up, so inert at the shipped default). */
static int shot_is_opener(n265_encoder *e, int i)
{
    if (i == 0)
        return la_is_idr(e, 0) || la_is_nonidr_i(e, 0);
    if (e->mode == N265_MODE_PCM)
        return 1;
    if (n265_rc_future_idr(e, i))
        return 1;
    int d = e->la.f[i].display_idx;
    if (d == force_nonidr_env())
        return 1;
    return e->ref_max >= 2 && e->scenecut &&
           d - e->last_intra_display >= intra_min_env() &&
           n265_la_scenecut(e, i);
}

static double shot_cplx_mean(n265_encoder *e, int lo, int hi)
{
    /* The opener is an intra picture where it opens a shot at all, and
     * it is held out of the mean; a chunk of a shot longer than the
     * window opens on an inter frame and counts it. A one-frame shot is
     * the degenerate case where the opener IS the mean, and then it is
     * priced as the intra frame it is. */
    int opener = shot_is_opener(e, lo);
    int first = opener && hi - lo > 1 ? lo + 1 : lo;
    double sum = 0.0;
    int n = 0;
    for (int k = first; k < hi; k++) {
        sum += n265_rc_cplx(e, k, opener && k == lo ? RC_I : RC_P);
        n++;
    }
    return n > 0 ? sum / n : 0.0;
}

/* Plan the shot that starts at queue index `lo`: find where it ends -
 * the next intra picture the lookahead would place, or the end of what
 * the window holds, whichever comes first - price it, and write the
 * offset onto every one of its frames that is in the queue. */
static void shot_plan_at(n265_encoder *e, int lo)
{
    n265_la *la = &e->la;
    int hi = la->count;
    for (int k = lo + 1; k < la->count; k++)
        if (shot_is_opener(e, k)) { hi = k; break; }
    double cplx = shot_cplx_mean(e, lo, hi);
    double off = 0.0;
    if (e->shot_have_level && e->shot_level > 0.0 && cplx > 0.0) {
        off = e->shot_k * (1.0 - e->rc.qcomp) * log2(cplx / e->shot_level);
        if (off > e->shot_clamp)
            off = e->shot_clamp;
        else if (off < -e->shot_clamp)
            off = -e->shot_clamp;
    }
    if (cplx > 0.0) {
        if (!e->shot_have_level) {
            e->shot_level = cplx;
            e->shot_have_level = 1;
        } else {
            e->shot_level += (cplx - e->shot_level) * e->shot_ema;
        }
    }
    for (int k = lo; k < hi; k++) {
        la->f[k].shot_off = (float)off;
        la->f[k].shot_planned = 1;
    }
    if (n265_env("N265_SHOT_TRACE"))
        fprintf(stderr, "SHOT,%d,%d,%d,%.0f,%.0f,%+.4f\n",
                la->f[lo].display_idx, hi - lo, la->count, cplx,
                e->shot_level, off);
}

/* Plan far enough ahead that the mini-GOP about to be built is covered.
 * The front of the queue is normally all it takes; the loop is for the
 * plan whose anchor is the next shot's opening intra picture, where one
 * mini-GOP straddles a cut and two shots have to be priced before it can
 * be coded. */
static void shot_plan(n265_encoder *e)
{
    n265_la *la = &e->la;
    int need = e->bframes + 1;
    if (need > la->count)
        need = la->count;
    for (int guard = 0; guard < need; guard++) {
        int k = 0;
        while (k < need && la->f[k].shot_planned)
            k++;
        if (k >= need)
            return;
        shot_plan_at(e, k);
    }
}

/* The keyint/scene-cut half of la_is_idr, for the rate control's look at
 * the frames still queued (rc.h). The `!e->ref_valid` clause is left out on
 * purpose: it says the frame being dispatched has nothing to predict from,
 * which is a statement about the present and not about the window. */
int n265_rc_future_idr(n265_encoder *e, int i)
{
    if (i < 0 || i >= e->la.count)
        return 0;
    int d = e->la.f[i].display_idx;
    return e->keyint == 1 || (d % e->keyint) == 0 ||
           (e->scenecut && d - e->gop_start_display >= min_keyint(e) &&
            n265_la_scenecut(e, i));
}

/* Emit the pyramid coding order for the B lookahead indices [blo, bhi],
 * bracketed by already-coded references: the midpoint first (a reference if
 * it has children on either side), then its two halves. */
static void plan_brange(n265_encoder *e, int blo, int bhi, int depth)
{
    if (blo > bhi)
        return;
    int mid = (blo + bhi) / 2;
    int is_ref = (blo <= mid - 1) || (mid + 1 <= bhi);
    int k = e->plan_len++;
    e->plan_li[k] = mid; e->plan_kind[k] = 0; e->plan_depth[k] = depth;
    e->plan_nonref[k] = !is_ref;
    plan_brange(e, blo, mid - 1, depth + 1);
    plan_brange(e, mid + 1, bhi, depth + 1);
}

/* Build the coding-order plan for the mini-GOP at the front of the queue. */
static void build_plan_order(n265_encoder *e)
{
    n265_la *la = &e->la;
    e->plan_len = e->plan_pos = 0;
    if (la_is_idr(e, 0)) {
        e->gop_start_display = e->last_intra_display = la->f[0].display_idx;
        for (int sl = 0; sl < e->dpb_slots; sl++) e->dpb[sl].marked = 0;
        e->ref_valid = 0;
        e->plan_li[0] = 0; e->plan_kind[0] = 2; e->plan_depth[0] = 0; e->plan_nonref[0] = 0;
        e->plan_len = 1; e->plan_pop = 1;
        return;
    }
    /* The non-IDR I at the FRONT of the queue: a one-picture plan, the
     * shape the "next frame is a key frame" case already had. Nothing
     * else happens - the POC line continues (gop_start_display is still
     * the IDR's), the DPB is not flushed, and the pictures the following
     * mini-GOPs still want stay marked and ride this picture's RPS.
     * Inside the queue it is an ANCHOR instead, which is where its value
     * is: see the run below. */
    if (la_is_nonidr_i(e, 0)) {
        e->last_intra_display = la->f[0].display_idx;
        e->plan_li[0] = 0; e->plan_kind[0] = 3; e->plan_depth[0] = 0; e->plan_nonref[0] = 0;
        e->plan_len = 1; e->plan_pop = 1;
        return;
    }
    /* the anchor offset A: as many B frames as bframes, cut short by the
     * first forced key frame in the run (that frame opens the next GOP). */
    int maxa = e->bframes < la->count ? e->bframes : la->count - 1;
    int A = 0;
    /* adaptive B placement: extend the B run frame by frame, but end it at
     * the first frame whose lookahead motion x inter/intra ratio says a far
     * bidirectional prediction will code badly (high-motion pans hurt as
     * deep B; foliage/texture with low motion do not). N265_BADAPT_THR the
     * cutoff on (mean |lowres MV| per block) x (pcost/icost).
     *
     * The motion term is a count of LOWRES PIXELS, so the same physical pan
     * reads 3-5x larger at 720p/1080p than at the CIF the cutoff was
     * calibrated on, and the run collapses to one B frame on any HD clip
     * that moves. N265_BADAPT_REFW scales it back to a fraction of the
     * frame width: the term is multiplied by refw / lowres width, with
     * refw the width the cutoff is taken to hold at. The scale is CLAMPED
     * AT 1.0, so the normalisation can only ever relax the cutoff, never
     * tighten it: everything at or below refw - the whole CIF half of the
     * corpus, where the 0.35 was read - is byte-identical to the
     * pre-2026-09-08 encoder, and only the clips whose motion the term was
     * over-reading move. 0 turns it off (the pre-2026-09-08 encoder). */
    double bthr = 0.35; { const char *v = n265_env("N265_BADAPT_THR"); if (v) bthr = atof(v); }
    double refw = 128.0; { const char *v = n265_env("N265_BADAPT_REFW"); if (v) refw = atof(v); }
    double wscale = 1.0;
    if (refw > 0.0 && la->lw > 0 && (double)la->lw > refw)
        wscale = refw / (double)la->lw;
    int nb = la->bw * la->bh;
    int trace = n265_env("N265_BADAPT_TRACE") != NULL;
    /* How far the key-frame edge lets the run go at all. An IDR opens the
     * next plan, so the run has to stop BEFORE it; a non-IDR I is an
     * anchor of this one, so the run may stop ON it - and then the leaves
     * between the previous anchor and the cut carry it as their L1, which
     * is the whole reason not flushing the DPB is worth anything. At
     * --ref 1 it is the ONLY way a picture crosses the cut: the lists are
     * one deep, so the nearest past reference after the cut is the intra
     * picture itself whatever else is still marked. */
    int keymax = maxa, inon = -1;
    for (int i = 1; i <= maxa; i++) {
        if (la_is_idr(e, i)) { keymax = i - 1; break; }
        if (la_is_nonidr_i(e, i)) { keymax = inon = i; break; }
    }
    /* b-adapt 2: price the mini-GOP instead of ruling on motion. The run
     * length is the one whose amortised lowres cost is lowest - one anchor
     * predicted at distance A+1 plus A bidirectionally-predicted leaves,
     * over A+1 frames. It has no calibrated constant in it. */
    if (e->badapt == 3 && keymax >= 1) {
        double dthr = 1.6;
        { const char *v = n265_env("N265_BADAPT_DEG"); if (v) dthr = atof(v); }
        A = n265_la_degrade(e, keymax, dthr);
    } else if (e->badapt == 2 && keymax >= 1) {
        A = n265_la_brun(e, keymax);
    } else if (e->badapt == 4 && keymax >= 1) {
        /* b-adapt 4: the same trellis priced in BITS rather than residual,
         * every frame at its own planned QP, so a leaf's wider dead zone
         * is visible to the comparison (docs/phase7-gop-design.md D1). */
        A = n265_la_brun_bits(e, keymax);
    } else
    for (int i = 1; i <= maxa; i++) {
        if (la_is_idr(e, i)) break;     /* that frame opens the next plan */
        A = i;
        if (i == inon) break;           /* ...this one IS this plan's anchor */
        if (e->badapt) {
            double mv = (double)la->f[i].mvabs_sum / (nb ? nb : 1);
            double m = mv * wscale * (double)la->f[i].sc_ratio;
            if (trace)
                fprintf(stderr, "BADAPT d=%d i=%d mv=%.3f ratio=%.3f "
                        "scale=%.3f m=%.4f thr=%.4f %s\n",
                        la->f[i].display_idx, i, mv,
                        (double)la->f[i].sc_ratio, wscale, m, bthr,
                        m > bthr ? "END" : "extend");
            if (m > bthr) break;    /* this frame ends the run (it is the anchor) */
        }
    }
    /* N265_BRUN_TRACE under a rule that is NOT the bits model: price the
     * trellis anyway and throw the answer away. That is what lets the
     * model's per-frame prediction be read against the frames the encoder
     * actually coded, which is step 1's first measurement and the one
     * that can close D1. Output-neutral - it writes only the priced
     * b-adapt scratch, which no other rule reads - and it runs only with
     * the variable set. */
    if (e->badapt != 4 && keymax >= 1 && n265_brun_trace())
        (void)n265_la_brun_bits(e, keymax);

    if (A == 0) {                       /* next frame is a key frame: front is a plain P */
        e->plan_li[0] = 0; e->plan_kind[0] = 1; e->plan_depth[0] = 0; e->plan_nonref[0] = 0;
        e->plan_len = 1; e->plan_pop = 1;
        return;
    }
    /* the anchor first, then the B pyramid between the anchors. The
     * anchor is the intra picture itself where the run reached the cut,
     * and a P otherwise (including where the run stopped short of a cut,
     * which leaves the intra picture to open the next plan). */
    int akind = A == inon ? 3 : 1;
    if (akind == 3)
        e->last_intra_display = la->f[A].display_idx;
    e->plan_li[0] = A; e->plan_kind[0] = akind; e->plan_depth[0] = 0; e->plan_nonref[0] = 0;
    e->plan_len = 1;
    plan_brange(e, 0, A - 1, 1);
    e->plan_pop = A + 1;
}

/* Per picture, the last coding index in this plan whose reference lists
 * reach it: the lifetime the dispatch-time retirement reads. The plan for
 * the whole mini-GOP is known here, and every list in it is a function of
 * POC and marking alone, so the walk below is exact - it IS
 * n265_build_lists, run ahead of time over the plan's own marking.
 *
 * A picture the walk never lists is dead the moment the frame after its
 * last use is dispatched. The exceptions are the anchors the mini-GOP end
 * carries into the next plan (ref_max of them, this one's anchor
 * included): they get INT_MAX, because the plan that would price their
 * next use does not exist yet.
 *
 * Written onto the DPB pictures directly (they are driver-serial) and,
 * for the frames this plan will mark, into plan_lastuse. */
static void plan_lifetimes(n265_encoder *e)
{
    enum { EMAX = N265_DPB_SLOTS + N265_MAX_BFRAMES + 2 };
    int epoc[EMAX], elast[EMAX], eslot[EMAX], epidx[EMAX], ne = 0;
    uint8_t eanc[EMAX];

    for (int sl = 0; sl < e->dpb_slots; sl++) {
        if (!e->dpb[sl].marked)
            continue;
        epoc[ne] = e->dpb[sl].poc; eanc[ne] = (uint8_t)e->dpb[sl].is_anchor;
        eslot[ne] = sl; epidx[ne] = -1; elast[ne] = -1; ne++;
    }
    for (int k = 0; k < e->plan_len; k++) {
        e->plan_poc[k] = e->la.f[e->plan_li[k]].display_idx - e->gop_start_display;
        e->plan_lastuse[k] = -1;
        if (e->plan_nonref[k])
            continue;               /* a leaf: never marked, never listed */
        epoc[ne] = e->plan_poc[k]; eanc[ne] = e->plan_kind[k] != 0;
        eslot[ne] = -1; epidx[ne] = k; elast[ne] = -1; ne++;
    }

    int cur[EMAX], nc = 0;
    for (int i = 0; i < ne; i++)
        if (eslot[i] >= 0) cur[nc++] = i;
    for (int k = 0; k < e->plan_len; k++) {
        int m[EMAX];
        uint8_t used[EMAX];
        for (int j = 0; j < nc; j++) m[j] = epoc[cur[j]];
        if (e->plan_kind[k] >= 2)
            memset(used, 0, (size_t)nc);    /* an intra picture lists nothing */
        else
            ref_used(m, nc, e->plan_poc[k], e->plan_kind[k] == 0, e->ref_max, used);
        for (int j = 0; j < nc; j++)
            if (used[j]) elast[cur[j]] = k;
        /* the picture joins the DPB at the end of its own dispatch step,
         * which is why it can never appear in its own lists */
        for (int i = 0; i < ne; i++)
            if (epidx[i] == k) cur[nc++] = i;
    }
    /* what the mini-GOP end carries: the ref_max newest anchors */
    for (int r = 0; r < e->ref_max; r++) {
        int best = -1;
        for (int i = 0; i < ne; i++)
            if (eanc[i] && elast[i] != INT_MAX &&
                (best < 0 || epoc[i] > epoc[best]))
                best = i;
        if (best < 0)
            break;
        elast[best] = INT_MAX;
    }
    for (int i = 0; i < ne; i++) {
        if (eslot[i] >= 0) e->dpb[eslot[i]].last_use = elast[i];
        else e->plan_lastuse[epidx[i]] = elast[i];
    }
}

static void build_plan(n265_encoder *e)
{
    build_plan_order(e);
    plan_lifetimes(e);
}

static void join_oldest(n265_encoder *e);

/* D3: is this picture still being written, or still readable by a frame
 * that has not retired? The list is driver-owned, so this is a plain
 * read. */
static int slot_busy(n265_encoder *e, const struct n265_refpic *s)
{
    if (s->in_use)
        return 1;
    for (int i = 0; i < e->n_inflight; i++) {
        n265_frame_work *f = e->inflight[i];
        if (f->col == s)
            return 1;
        for (int l = 0; l < 2; l++)
            for (int k = 0; k < f->nref[l]; k++)
                if (f->refl[l][k] == s)
                    return 1;
    }
    return 0;
}

/* D3, the source half: is the lookahead slot whose luma plane starts at
 * `src` still aliased by a frame in flight? A frame_work's pad_plane
 * points straight at the queue slot, and analysis reads it until the
 * frame retires. */
static int la_src_busy(n265_encoder *e, const pixel *src)
{
    for (int i = 0; i < e->n_inflight; i++)
        if (e->inflight[i]->pad_plane[0] == src)
            return 1;
    return 0;
}

/* Claim the DPB slot this frame reconstructs into and point the
 * frame_work's recon planes at it. The picture goes in an UNMARKED slot
 * where one exists - a picture no RPS and no reference list can see -
 * and otherwise in the marked one with the oldest POC that is not in
 * this frame's lists, which is what the pre-T0a store did after entropy.
 *
 * WHICH unmarked slot is scheduling, not a coding decision: an unmarked
 * picture is in nobody's RPS and nobody's list, and D2 made every order
 * that feeds the bitstream a function of POC rather than of slot index.
 * T3/1 uses that (`e->slot_prefer`): it takes an unmarked slot no frame
 * in flight is still WRITING or READING in preference to the
 * lowest-index one, because the lowest-index rule handed frame k the
 * slot frame k-1 was still reconstructing into and D3's stall then drained
 * the pipeline on 39 of 40 frames at `--bframes 0`. When every unmarked
 * slot is busy the serial choice stands and D3 stalls on it: the rule
 * never re-routes to a MARKED slot to dodge a stall.
 *
 * The preference is armed only where the reference structure CANNOT fill
 * the pool (peak marked count < e->dpb_slots, decided at open). Under
 * DPB pressure the third branch below overwrites a picture that stays
 * `marked` - the pre-existing bug T0a recorded - and then slot identity
 * DOES reach the stream: at `--bframes 7 --ref 3` reversing the scan
 * order alone moves the bitstream, at one thread and at twelve alike.
 * The condition is exactly the one that keeps the third branch
 * unreachable, so it is what makes the preference provably bit-neutral
 * rather than merely observed to be.
 *
 * Every scan below is bounded by the SIZED pool and not by the array:
 * a shape that runs six slots scans the same six it always did, in the
 * same order, so its claims are the same claims. */
static void claim_slot(n265_encoder *e, n265_frame_work *fw)
{
    struct n265_refpic *slot = NULL;
    if (fw->nonref || fw->mode == N265_MODE_PCM)
        slot = &e->scratch_pic[fw - e->fw];
    if (e->slot_prefer)
        for (int sl = 0; sl < e->dpb_slots && !slot; sl++)
            if (!e->dpb[sl].marked && !slot_busy(e, &e->dpb[sl]))
                slot = &e->dpb[sl];
    for (int sl = 0; sl < e->dpb_slots && !slot; sl++)
        if (!e->dpb[sl].marked) slot = &e->dpb[sl];
    if (!slot) {
        /* nothing unmarked: the pressure branch. Counted, because the
         * pool sizing above claims it cannot happen on any shape the
         * pool covers and N265_DPB_TRACE is where that claim is read. */
        e->slot_overwrite++;
        for (int sl = 0; sl < e->dpb_slots && !slot; sl++) {
            int in_list = 0;
            for (int l = 0; l < 2; l++)
                for (int k = 0; k < fw->nref[l]; k++)
                    if (fw->refl[l][k] == &e->dpb[sl]) in_list = 1;
            if (!in_list && (!slot || e->dpb[sl].poc < slot->poc)) slot = &e->dpb[sl];
        }
    }
    if (!slot) slot = &e->dpb[0];
    /* D3: wait for the picture, never re-route to another one. */
    while (e->n_inflight && slot_busy(e, slot)) {
        e->join_slot++;
        join_oldest(e);
    }
    slot->in_use = 1;
    /* `marked` is NOT cleared: when the pool is exhausted the picture
     * being overwritten stays listed in this frame's RPS exactly as it
     * did before the claim moved to dispatch (it is replaced when this
     * frame marks itself). That is a pre-existing DPB-pressure bug at
     * --ref 3 --bframes 7, reproduced on purpose so the ladder changes
     * no bits. */
    /* Physical readiness starts empty: the sec 1 reference gate reads
     * this watermark and nothing else, so it must not carry the previous
     * occupant's. */
#ifdef N265_REF_CHECK
    /* a new occupant: every reader that froze the old generation is now
     * provably reading the wrong picture's side data */
    slot->gen++;
    if (slot->row_gen)
        memset(slot->row_gen, 0, (size_t)fw->ctb_rows * sizeof(unsigned));
#endif
    atomic_store_explicit(&slot->final_rows, 0, memory_order_release);
    /* E1: arm the half-pel cache here, on the driver; the publish row
     * task fills the lines it owns (G4). */
    if (fw->mode != N265_MODE_PCM && !fw->nonref)
        hf_arm(e, slot, fw->code_idx);
    fw->slot = slot;
    /* the source this picture is coded from, for the frames dispatched
     * after it (la.h, the duplicate chain) */
    slot->dup_grp = fw->dup_grp;
    slot->dup_cum = fw->dup_cum;
    for (int i = 0; i < 3; i++) {
        fw->recon[i] = slot->plane[i];
        fw->recon_stride[i] = slot->stride[i];
    }
}

/* Encode one frame from the plan (building it first if none is active).
 * Returns the number of source frames retired from the queue (0 until the
 * plan completes, then the whole mini-GOP). */
static int drive_scheduler(n265_encoder *e)
{
    n265_la *la = &e->la;
    if (la->count == 0)
        return 0;
    /* The dispatch column is this function's own wall MINUS the stages
     * that carry their own accumulator, so the columns partition the
     * driver's time instead of overlapping it. `ds_ret` is where the
     * single return below funnels through. */
    double ds_t0 = e->la_prof ? prof_now() : 0.0;
    double ds_sub0 = e->la_prof ? e->prof_plan + e->prof_cutree +
                                  e->prof_join + e->prof_retire : 0.0;
    int ds_ret = 0;
    /* Ahead of the mini-GOP plan, because both read la_is_idr and
     * build_plan_order moves what it reads (gop_start_display,
     * last_intra_display) as it decides. Inert with the mode off. */
    if (e->shot_window > 0)
        shot_plan(e);
    if (e->plan_pos >= e->plan_len)
    {
        double t = e->la_prof ? prof_now() : 0.0;
        build_plan(e);
        if (e->la_prof) e->prof_plan += prof_now() - t;
    }

    /* K: a free frame_work, by joining the oldest in flight. This and
     * the D3 stalls below are the only places the driver blocks. */
    while (e->n_inflight >= e->K) {
        e->join_k++;
        join_oldest(e);
    }

    int li = e->plan_li[e->plan_pos];
    int kind = e->plan_kind[e->plan_pos];
    la_frame *f = &la->f[li];
    int poc = f->display_idx - e->gop_start_display;
    n265_frame_work *fw = &e->fw[e->fw_next];
    e->fw_next = e->fw_next + 1 >= e->K ? 0 : e->fw_next + 1;

    /* the queue slot IS the padded source (threading-design sec 4) */
    for (int i = 0; i < 3; i++)
        fw->pad_plane[i] = f->src[i];

    memset(fw->ctu_qp_off, 0, (size_t)fw->ctb_cols * fw->ctb_rows);
    for (int k = 0; k < fw->ctb_cols * fw->ctb_rows; k++)
        fw->ctu_lam_q8[k] = 256;    /* inert unless cu-tree writes it */
    fw->ct_mean_raw = fw->ct_mean_used = fw->ct_level = 0.0;  /* no field, no frame term */
    if (e->cutree)
    {
        double t = e->la_prof ? prof_now() : 0.0;
        n265_la_cutree(e, fw, li);
        if (e->la_prof) e->prof_cutree += prof_now() - t;
    }
    /* Retire, here and driver-serially (D2), every marked picture no
     * frame at or after this one in the plan references. The decoder's
     * DPB is governed by the RPS we signal - a picture leaves it on the
     * first RPS that omits it - so doing this at dispatch rather than at
     * the mini-GOP end shortens both our occupancy and the signalled set,
     * and takes the declared buffering (and with it the level) down with
     * them.
     *
     * It moves syntax and not one decision. The lists built below are the
     * ref_max nearest marked pictures on each side; a picture no
     * remaining frame lists is, by construction, not among them for any
     * of those frames, and dropping a non-prefix element of a
     * nearest-first order leaves the prefix exactly where it was. D3
     * covers the other half: the claim below may now take the slot of a
     * picture a frame in flight is still reading, and `slot_busy` makes
     * the driver stall on it rather than re-route. */
    if (!dpb_hold())
        for (int sl = 0; sl < e->dpb_slots; sl++)
            if (e->dpb[sl].marked && e->dpb[sl].last_use < e->plan_pos)
                e->dpb[sl].marked = 0;
    /* reference lists from the DPB around this frame's POC (anchors have
     * only L0; B frames have both). An intra picture has none - which the
     * IDR got for free, its DPB having just been flushed, and the non-IDR
     * I has to be told. */
    int slice_type = kind >= 2 ? 2 : kind == 1 ? 1 : 0;
    if (slice_type == 2)
        fw->nref[0] = fw->nref[1] = 0;
    else
        n265_build_lists(e, fw, poc, kind == 0, NULL);
    fw->slice_type = slice_type;
    fw->idr = kind == 2;
    fw->poc = poc;
    fw->nonref = e->plan_nonref[e->plan_pos];
    fw->display_idx = f->display_idx;
    fw->b_depth = e->plan_depth[e->plan_pos];
    fw->ref_valid = e->ref_valid;
    fw->shot_off = e->shot_window > 0 ? (double)f->shot_off : 0.0;
    fw->sc_ratio = f->sc_ratio;     /* lookahead pcost/icost, P3 reads it */
    fw->la_mvabs = la->bw * la->bh
                       ? (float)f->mvabs_sum / (float)(la->bw * la->bh) : 0.0f;
    fw->la_icost = f->icost;        /* the same field per CTU, R3 reads it */
    fw->la_pcost = f->pcost;
    fw->la_mv = f->mv;              /* N265_DEPTH_DUMP only */
    fw->la_prop = f->prop;          /* N265_CU_TRACE only */
    fw->la_bw = la->bw;
    fw->la_bh = la->bh;
    fw->dup_grp = f->dup_grp;       /* the duplicate chain (la.h) */
    fw->dup_cum = f->dup_cum;
    fw->dup_zsad = f->zsad_sum;     /* N265_DUP_TRACE only */
    /* D1: freeze the RPS here, while the marked set is still the one the
     * serial encoder's header writer would have seen. Before the claim,
     * so the picture cannot appear in its own RPS. */
    snapshot_rps(e, fw);
    /* D2: the TMVP collocated picture is L0[0], frozen with the lists.
     * The coding path reads fw->col, never a live DPB slot's marking. */
    fw->col = (fw->slice_type != 2 && fw->nref[0] > 0) ? fw->refl[0][0] : NULL;
    fw->col_poc = fw->col ? fw->col->poc : 0;
#ifdef N265_REF_CHECK
    for (int l = 0; l < 2; l++)
        for (int k = 0; k < N265_MAX_REF; k++)
            fw->ref_gen[l][k] = k < fw->nref[l] ? fw->refl[l][k]->gen : 0;
    fw->col_gen = fw->col ? fw->col->gen : 0;
#endif
    /* Claim the DPB slot the picture reconstructs into, by the rule the
     * store used to apply after entropy. Nothing between here and there
     * changes the marked set, so the choice is the one the pre-T0a
     * encoder made; the slot is not marked until the end of this
     * dispatch step, so neither this frame's RPS nor its lists see it. */
    /* Round 4: the coding index this frame occupies. Driver-serial and
     * dense, so it indexes both the traffic history and the window the
     * arming rule reads back out of it. */
    fw->code_idx = e->hf2_code_n++;
    memset(fw->hf2_tr_rows, 0, (size_t)(e->ctb_rows + 1) * 24 *
                               sizeof(uint64_t));
    memset(fw->iw_arm_rows, 0, (size_t)(e->ctb_rows + 1) * N265_IW_CNT *
                               sizeof(uint64_t));
    claim_slot(e, fw);
    /* D2: mark now, on the driver, in coding order. Frame k+1's dispatch
     * has to see this picture in the DPB whether or not a row of it has
     * run; `final_rows` is what says whether the pixels exist. */
    if (fw->mode != N265_MODE_PCM && !fw->nonref) {
        fw->slot->poc = poc;
        fw->slot->is_anchor = slice_type != 0;  /* I/P; a reference B is not */
        fw->slot->last_use = e->plan_lastuse[e->plan_pos];
        fw->slot->marked = 1;
        e->ref_valid = 1;
    }
    /* the collocated picture's own reference POCs, driver state that
     * used to be written by the whole-frame publish pass */
    fw->slot->col_ref_poc[0] = fw->ref_valid && fw->nref[0] > 0
                                   ? fw->refl[0][0]->poc : 0;
    fw->slot->col_ref_poc[1] = (fw->slice_type == 0 && fw->nref[1] > 0)
                                   ? fw->refl[1][0]->poc
                                   : fw->slot->col_ref_poc[0];
    fw->rc_cls = n265_rc_class(slice_type, fw->nonref);
    fw->rc_cplx = n265_rc_cplx(e, li, fw->rc_cls);
    fw->rc_icplx = n265_rc_cplx(e, li, RC_I);
    /* --pass 1's lookahead columns, read HERE and not at retirement: the
     * queue slot is recycled behind a dispatched frame (la.h's free run),
     * so by the time this picture's size is known its lowres planes may
     * belong to a picture eight frames on. Driver-serial, after
     * n265_la_cutree has filled `prop` for the window, and gated on the
     * sidecar being armed so the default path does not walk the grid. */
    fw->st_icost = fw->st_pcost = fw->st_prop = 0.0;
    if (e->tp.pass == 1 || e->tp.pass == 3) {
        const la_frame *lf = &e->la.f[li];
        fw->st_icost = (double)lf->icost_sum;
        fw->st_pcost = (double)lf->pcost_sum;
        if (e->cutree && lf->prop) {
            double ps = 0.0;
            for (int b = 0; b < e->la.bw * e->la.bh; b++)
                ps += (double)lf->prop[b];
            fw->st_prop = ps;
        }
    }
    if (e->abr_on || e->vbv.on) {
        /* A VBV reads the buffer, and the buffer is only a number once
         * every earlier frame's SIZE is one. So capped CRF takes the same
         * drain ABR takes, for the same reason and with the same effect:
         * frame k is dispatched only after every frame with a lower coding
         * index has retired, the fill it clamps against is the fill a
         * decoder will really see, and t1 and t12 walk the same
         * trajectory. The cost is frame-level overlap, which capped CRF
         * gives up and the wavefront inside a picture does not - the
         * measured wall is in docs/phase12.md, Item 2.
         *
         * D4: frame k's rate factor is solved from the ACTUAL bits of
         * every frame with coding index <= k - 1 - D, so the driver
         * joins those first; rc_done runs inside join_oldest, on the
         * driver, in coding order. The frames still inside the lag
         * contribute their PLANNED bits instead. At D = 0 that is every
         * earlier frame, ABR does not overlap, and the loop is
         * bit-identical to the pre-T2 one; CQP and CRF launch
         * immediately and overlap fully either way.
         *
         * The solve itself is in dispatch_frame, one call below: it needs the
         * cu-tree frame term, which needs the frame's content type, and both
         * are driver-serial there. The DRAIN stays here, ahead of it, because
         * it is the lag's own statement about what the loop may observe. */
        while (e->n_inflight > e->rc_lag) {
            e->join_rc++;
            join_oldest(e);
        }
    }
    if (kind != 0)          /* the priced b-adapt prices against this plane */
        n265_la_set_anchor(e, li);
    dispatch_frame(e, fw);
    e->plan_pos++;

    if (e->plan_pos >= e->plan_len) {
        int pop = e->plan_pop;
        for (int i = 0; i < pop; i++) n265_la_pop(e);
        /* the mini-GOP is done: reference B frames live only within it, and
         * only ref_max anchors carry into the next one. Retire the rest so
         * the DPB (and the next slice's RPS) hold exactly what is reusable.
         * Since the retirement moved to dispatch this is the TAIL of the
         * same rule: the only pictures that can still be marked here are
         * the ones whose last use was the plan's final frame, plus the
         * carried anchors, and the trim below is a no-op on the carry set
         * (it is already ref_max wide). Under N265_DPB_HOLD both halves
         * are live and this is where every reference dies. */
        /* Retirement is driver-serial and keyed on POC: the trim below
         * drops the SMALLEST POC each time, and POCs are unique among
         * marked pictures, so slot index never decides who goes. */
        for (int sl = 0; sl < e->dpb_slots; sl++)
            if (e->dpb[sl].marked && !e->dpb[sl].is_anchor) e->dpb[sl].marked = 0;
        int nanchor = 0;
        for (int sl = 0; sl < e->dpb_slots; sl++) if (e->dpb[sl].marked) nanchor++;
        while (nanchor > e->ref_max) {
            int oldest = -1;
            for (int sl = 0; sl < e->dpb_slots; sl++)
                if (e->dpb[sl].marked && (oldest < 0 || e->dpb[sl].poc < e->dpb[oldest].poc)) oldest = sl;
            e->dpb[oldest].marked = 0; nanchor--;
        }
        ds_ret = pop;
    }
    if (e->la_prof)
        e->prof_dispatch += (prof_now() - ds_t0) -
                            (e->prof_plan + e->prof_cutree + e->prof_join +
                             e->prof_retire - ds_sub0);
    return ds_ret;
}

int n265_encoder_encode(n265_encoder *e, n265_nal **nal, int *nnal,
                        const n265_picture *pic)
{
    if (!e)
        return -1;
    *nal = e->nal;
    *nnal = 0;
    size_t out_used = 0;
    int count = 0;

    int ring = (int)(sizeof(e->done) / sizeof(e->done[0]));
    if (pic) {
        /* D3: n265_la_push takes the front of the queue's free run, and a
         * frame_work in flight may still be reading those source planes.
         * The stall is on THAT slot now, not on everything in flight: the
         * free run is longer than K, so this is a guard rather than a
         * drain (T3/1). */
        const pixel *reuse = n265_la_next_src(e);
        while (e->n_inflight && la_src_busy(e, reuse)) {
            e->join_src++;
            join_oldest(e);
        }
        const pixel *planes[3] = { pic->plane[0], pic->plane[1],
                                   pic->plane[2] };
        {
            double t = e->la_prof ? prof_now() : 0.0;
            n265_la_push(e, planes, pic->stride);
            if (e->la_prof) e->prof_push += prof_now() - t;
        }
        e->in_display_idx++;
        /* One call still codes a whole mini-GOP - the loop exits when the
         * mini-GOP-end pop brings the queue back under la_depth, and
         * plan building, la_pop and the cu-tree sit at exactly the point
         * in the dispatch sequence they sat at serially - but the call no
         * longer WAITS for it. A1: the return is a prefix of the frames
         * completed so far, in coding order, and up to K frames stay in
         * flight across the call boundary. */
        while (e->la.count > e->la_fill)
            drive_scheduler(e);
        /* A2: at one worker nothing overlaps, so nothing is deferred and
         * the call returns exactly what it returned before the API
         * change - which is what keeps goal 1's timing and every 1-thread
         * identity check untouched. */
        if (!e->defer_out)
            while (e->n_inflight)
                join_oldest(e);
        /* N265_API_DEFER holds the prefix back until the ring is half
         * full: the "including none" end of A1, and the bytes it hands
         * out must equal the default chunking's. */
        {
            double t = e->la_prof ? prof_now() : 0.0;
            emit_done(e, e->api_defer && e->n_done < ring / 2 ? 0 : INT_MAX,
                      &out_used, &count);
            if (e->la_prof) e->prof_emit += prof_now() - t;
        }
    } else {
        /* Flush drains in order (A1). It dispatches ahead up to K first,
         * so the tail of the sequence overlaps like the rest of it, then
         * returns every frame that has completed - at least one, so a
         * caller's drain loop terminates. */
        while (e->la.count > 0 && e->n_inflight < e->K &&
               e->n_done + e->n_inflight < ring - 2)
            drive_scheduler(e);
        if (e->n_done == 0 && e->n_inflight)
            join_oldest(e);
        {
            double t = e->la_prof ? prof_now() : 0.0;
            emit_done(e, e->defer_out ? INT_MAX : 1, &out_used, &count);
            if (e->la_prof) e->prof_emit += prof_now() - t;
        }
    }
    for (int i = 0; i < count; i++)
        e->nal[i].payload = e->out + e->nal_off[i];

    *nnal = count;
    return (int)out_used;
}
