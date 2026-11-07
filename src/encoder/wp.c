/*
 * Weighted prediction: the estimator behind 7.3.6.3's table.
 * Copyright (c) 2026, the yah265 authors. GPL-2.0-or-later; see LICENSE.
 *
 * WHAT IS DERIVED, AND FROM WHAT
 *
 * For each (list, reference) pair the table carries one linear map per
 * component, `s -> (s * w + round) >> d) + o`, and the job here is to
 * find the (w, o) that makes the REFERENCE look like the CURRENT
 * PICTURE. Under a fade that map is most of the frame-to-frame
 * difference, which is why the tool exists: a fade with the right weight
 * codes like a still, and without one every block carries a DC residual.
 *
 * It is derived from the LOWRES SOURCE of both pictures and from nothing
 * else - not from the reference's reconstruction, which does not exist
 * yet at dispatch and whose readiness is a thread-order property. The
 * first moment fixes the offset and the second fixes the scale
 * (`sqrt(varF / varR)`), then a SAD over the same
 * two planes says whether the map is worth signaling. Nothing here
 * reads a coded sample, so the whole table is a pure function of the
 * input pictures and is identical at every thread count.
 *
 * WHY A SAD CHECK AT ALL
 *
 * The moments always produce a map, and on a clip with no fade the map
 * is noise fitted to noise: it moves bits into the slice header and the
 * residual nowhere. The gain test is what keeps `--weightp` from costing
 * anything on the fourteen corpus clips that are not fades - the
 * estimator has to BEAT the unweighted prediction by a margin on the
 * lowres plane before a flag is written. The firing rate per clip is the
 * measurement that says whether the margin is right (N265_WP_TRACE).
 */

#include <math.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#include "encoder/encoder.h"
#include "encoder/la.h"
#include "encoder/preset.h"
#include "encoder/wp.h"

int n265_weightp_on(const struct n265_encoder *e) { return e->weightp; }
int n265_weightb_on(const struct n265_encoder *e) { return e->weightb; }

int n265_wp_trace(void)
{
    static int v = -1;
    if (v < 0) {
        const char *s = n265_env("N265_WP_TRACE");
        v = s ? atoi(s) != 0 : 0;
    }
    return v;
}

/* The gain a weighted map has to show on the lowres plane before it is
 * signaled, as a right shift of the unweighted SAD: 4 is 1/16, about
 * 6.3%. A LARGER number is a smaller margin and fires more often.
 *
 * 4 rather than the 6 this shipped with first, and the evidence is a
 * separation in the estimator's OWN statistic rather than a sweep
 * against the band. Over 120 frames at medium, the gains of the maps
 * that fired were: foreman_cif 1.7 / 3.1 / 4.5%, park_joy_720p 1.6 /
 * 1.7 / 2.6 / 3.2 / 4.2% - and sintel_720p, which has real dissolves,
 * 11.4% at its SMALLEST of twenty-three. Nothing on a clip without an
 * illumination change reached 5% and nothing on a clip with one fell
 * below 11%, so the margin belongs in the gap. At 1/64 the two grain
 * and motion clips were signaling weights worth a couple of percent of
 * lowres SAD and costing 0.6-0.8 of a BD point, which is the whole
 * failure mode this test exists to prevent: a near-identity weight
 * moves every sample by a level and turns blocks that would have been
 * SKIP into blocks with a residual, and a lowres SAD cannot see that. */
static int wp_gain_shift(void)
{
    static int v = -1;
    if (v < 0) {
        const char *s = n265_env("N265_WP_GAIN");
        v = s ? atoi(s) : 4;
        if (v < 0) v = 0;
        if (v > 20) v = 20;
    }
    return v;
}

/* The share of 8x8 lowres blocks a map must IMPROVE before it is
 * signaled, in percent. See block_win_pct for what it is for.
 *
 * 40, and the calibration is two-sided from the only two-sided evidence
 * the corpus has. Over 120 frames at medium, the win share of every map
 * that fired: sita_flat_720p 31% - the one map in the corpus that HURT,
 * and by ten BD points - against xfade_720p 43 and 47, sintel_720p 47 to
 * 71, touchdown_1080p 88 to 94 and fade_cif 74 to 100. The gap is 31 to
 * 43 and it is not wide; what makes 40 the right side of it is that the
 * two numbers mean different things - below half the blocks, the map is
 * not describing the picture's illumination at all. */
static int wp_block_win(void)
{
    static int v = -1;
    if (v < 0) {
        const char *s = n265_env("N265_WP_WIN");
        v = s ? atoi(s) : 40;
        if (v < 0) v = 0;
        if (v > 100) v = 100;
    }
    return v;
}

/* How many references per list the estimator looks at. The table can
 * carry a weight for every one, but the second and third references of a
 * list are a second and third analysis for a picture the search rarely
 * chooses; 2 is x265's practical reach and 3 is our --ref ceiling. */
static int wp_max_refs(void)
{
    static int v = -1;
    if (v < 0) {
        const char *s = n265_env("N265_WP_REFS");
        v = s ? atoi(s) : N265_MAX_REF;
        if (v < 1) v = 1;
        if (v > N265_MAX_REF) v = N265_MAX_REF;
    }
    return v;
}

/* ---- the lowres planes ------------------------------------------- */

/* 2x2 mean, the lookahead's own downscale (la.c), so the luma plane the
 * estimator reads IS `la_frame::lowres` and the chroma planes are built
 * the same way rather than a different way that happens to look similar.
 * Brought back to 8-bit magnitudes at every depth for the same reason the
 * lookahead does it: this is a cost proxy, never a predictor. */
static void downscale(pixel *dst, int dw, int dh, const pixel *src,
                      int ss, int sw, int sh)
{
    for (int y = 0; y < dh; y++) {
        int sy = 2 * y, sy1;
        if (sy >= sh) sy = sh - 1;
        sy1 = sy + 1 < sh ? sy + 1 : sy;
        for (int x = 0; x < dw; x++) {
            int sx = 2 * x, sx1;
            if (sx >= sw) sx = sw - 1;
            sx1 = sx + 1 < sw ? sx + 1 : sx;
            dst[y * dw + x] = (pixel)((src[sy * ss + sx] + src[sy * ss + sx1] +
                                       src[sy1 * ss + sx] + src[sy1 * ss + sx1] +
                                       (2 << BIT_DEPTH_SHIFT))
                                      >> (2 + BIT_DEPTH_SHIFT));
        }
    }
}

void n265_wp_build_cur(n265_encoder *e, const la_frame *f)
{
    if (!e->wp_cur[0])
        return;
    memcpy(e->wp_cur[0], f->lowres,
           (size_t)e->la.lw * e->la.lh * sizeof(pixel));
    for (int c = 1; c < 3; c++)
        downscale(e->wp_cur[c], e->wp_cw, e->wp_ch, f->src[c],
                  e->pad_w >> e->csx, e->param.width >> e->csx,
                  e->param.height >> e->csy);
}

void n265_wp_slot_store(n265_encoder *e, struct n265_refpic *slot)
{
    if (!e->wp_cur[0] || !slot->wp_low[0])
        return;
    memcpy(slot->wp_low[0], e->wp_cur[0],
           (size_t)e->la.lw * e->la.lh * sizeof(pixel));
    for (int c = 1; c < 3; c++)
        memcpy(slot->wp_low[c], e->wp_cur[c],
               (size_t)e->wp_cw * e->wp_ch * sizeof(pixel));
}

/* ---- the derivation ---------------------------------------------- */

static void plane_moments(const pixel *p, int n, double *mean, double *var)
{
    uint64_t s = 0, sq = 0;
    for (int i = 0; i < n; i++) {
        unsigned v = p[i];
        s += v;
        sq += (uint64_t)v * v;
    }
    double m = (double)s / n;
    *mean = m;
    /* the population variance, and it is allowed to come out slightly
     * negative on a constant plane through the cancellation; clamp */
    double vr = (double)sq / n - m * m;
    *var = vr > 0.0 ? vr : 0.0;
}

/* The largest denominator 0..7 at which `scale` is representable: the
 * syntax element is the DELTA from 1 << d and is bounded at -128..127
 * (7.4.7.3), so the weight itself lives in [(1<<d) - 128, (1<<d) + 127].
 * d = 0 admits any scale in [-127, 128], so this never fails. */
int n265_wp_denom_for(double scale)
{
    for (int d = 7; d > 0; d--) {
        long w = lround(scale * (double)(1 << d));
        if (w >= (1 << d) - 128 && w <= (1 << d) + 127)
            return d;
    }
    return 0;
}

int n265_wp_quant(double scale, int d)
{
    long w = lround(scale * (double)(1 << d));
    if (w < (1 << d) - 128) w = (1 << d) - 128;
    if (w > (1 << d) + 127) w = (1 << d) + 127;
    return (int)w;
}

/* The weighted lowres sample, in the lowres plane's own 8-bit units.
 * This is NOT 8.5.3.3.4.3 - that one runs on the 14-bit intermediate -
 * but it is the same map: with a full-pel predictor the two agree
 * exactly, because shift1 cancels out of ((s << shift1) * w + (1 <<
 * (log2Wd-1))) >> log2Wd. Deriving on the cheaper form is therefore not
 * an approximation of the decision, only of the subpel rounding. */
static inline int wp_map8(int v, int w, int d, int o)
{
    int r = d ? ((v * w + (1 << (d - 1))) >> d) : v * w;
    r += o;
    return r < 0 ? 0 : r > 255 ? 255 : r;
}

/* SAD of the current plane against the reference plane, unweighted and
 * at five candidate offsets around the derived one, in ONE walk. The
 * offsets are searched because the second moment fixes the scale and the
 * first then has to absorb whatever the scale got wrong, and a lowres
 * mean is a coarse instrument for that. */
struct sad_set {
    uint64_t un;
    uint64_t w[5];
};

static void sad_sweep(const pixel *cur, const pixel *ref, int n,
                      int w, int d, int o0, struct sad_set *out)
{
    /* The map has 256 possible inputs - the lowres planes carry 8-bit
     * values at every depth (la.h) - so five candidates over it is a
     * table built once, against a multiply, a shift, an add and two
     * clamps per sample per candidate otherwise. The plane is half a
     * million samples at 1080p and the table is not, which is the whole
     * argument. It is sized by the PIXEL type rather than by 256 so that
     * a value outside the range the downscale guarantees would be a
     * wrong answer and not an out-of-bounds read. */
    static const int LN = PIXEL_MAX + 1;
    uint8_t lut[5][PIXEL_MAX + 1];
    for (int k = 0; k < 5; k++)
        for (int v = 0; v < LN; v++)
            lut[k][v] = (uint8_t)wp_map8(v > 255 ? 255 : v, w, d,
                                         o0 + k - 2);
    memset(out, 0, sizeof(*out));
    for (int i = 0; i < n; i++) {
        int c = cur[i], r = ref[i];
        int du = c - r;
        out->un += (unsigned)(du < 0 ? -du : du);
        for (int k = 0; k < 5; k++) {
            int dv = c - lut[k][r];
            out->w[k] += (unsigned)(dv < 0 ? -dv : dv);
        }
    }
}

/* The share of 8x8 lowres blocks the map actually IMPROVES, in percent.
 *
 * This is the test that separates an illumination change from a
 * coincidence, and the total SAD cannot do it. An illumination change is
 * GLOBAL: it moves every part of the picture the same way, so the right
 * weight helps almost everywhere. A global linear map fitted across a
 * SHOT CHANGE helps in a few places - it can align a large flat fill in
 * one shot with a large flat fill in the other - and hurts in the rest,
 * and it can still win the total while doing it, because the few places
 * are large. On sita_flat_720p, which is flat-shaded animation with a
 * cut at frame 2, exactly that map fired: scale 0.56, offset +37, a 14%
 * gain in total lowres SAD, and 5.2% MORE bits for the clip at the same
 * CRF. It made unrelated fills match, the mode decision believed the
 * SATD, and blocks that should have been intra went inter with a DC
 * residual everywhere.
 *
 * One extra pass over the two planes, after the offset sweep has chosen
 * its candidate, and only on a map that has already passed the total. */
static int block_win_pct(const pixel *cur, const pixel *ref, int lw, int lh,
                         int w, int d, int o)
{
    int nb = 0, win = 0;
    for (int by = 0; by + 8 <= lh; by += 8)
        for (int bx = 0; bx + 8 <= lw; bx += 8) {
            uint32_t su = 0, sw = 0;
            for (int y = 0; y < 8; y++) {
                const pixel *pc = cur + (size_t)(by + y) * lw + bx;
                const pixel *pr = ref + (size_t)(by + y) * lw + bx;
                for (int x = 0; x < 8; x++) {
                    int du = pc[x] - pr[x];
                    int dv = pc[x] - wp_map8(pr[x], w, d, o);
                    su += (unsigned)(du < 0 ? -du : du);
                    sw += (unsigned)(dv < 0 ? -dv : dv);
                }
            }
            nb++;
            if (sw < su)
                win++;
        }
    return nb ? (int)((100L * win) / nb) : 100;
}

/* One (component, reference) candidate, before the denominator is
 * fixed: the real-valued map and the plane it was measured on. */
struct cand {
    double scale, off;
    int valid;
};

/* The fit, with the CURRENT picture's moments already in hand: they are
 * the same three numbers for every reference and every list, and on a P
 * frame with three references the naive form walks the current lowres
 * plane three times to get them again. */
static void fit_from(double mf, double vf, const pixel *ref, int n,
                     double *scale, double *off)
{
    double mr, vr;
    plane_moments(ref, n, &mr, &vr);
    /* A reference with no variance carries no scale information: the
     * offset is the whole of the map, and asking sqrt(vf / vr) for one
     * would divide by the noise floor. 1.0 is a real answer here, not a
     * fallback - a flat picture scaled by anything is still flat. */
    double s = vr > 1.0 ? sqrt(vf / vr) : 1.0;
    if (s < 0.0) s = 0.0;
    if (s > 127.0) s = 127.0;
    *scale = s;
    *off = mf - s * mr;
}

void n265_wp_fit(const pixel *cur, const pixel *ref, int n,
                 double *scale, double *off)
{
    double mf, vf;
    plane_moments(cur, n, &mf, &vf);
    fit_from(mf, vf, ref, n, scale, off);
}

static void derive_cand(const double *cm, const pixel *ref, int n,
                        struct cand *c)
{
    fit_from(cm[0], cm[1], ref, n, &c->scale, &c->off);
    c->valid = 1;
}

void n265_wp_analyse(n265_encoder *e, n265_frame_work *fw)
{
    fw->wp_any = 0;
    fw->wp_ldenom = 0;
    fw->wp_cdenom = 0;
    memset(fw->wp, 0, sizeof(fw->wp));
    if (!fw->wp_on || !e->wp_cur[0])
        return;

    int nl = e->la.lw * e->la.lh;
    int nc = e->wp_cw * e->wp_ch;
    int maxref = wp_max_refs();
    int lists = fw->slice_type == 0 ? 2 : 1;    /* B has L1, P does not */

    /* the current picture's own moments, once for the whole table */
    double cm[3][2];
    plane_moments(e->wp_cur[0], nl, &cm[0][0], &cm[0][1]);
    for (int c = 0; c < 2; c++)
        plane_moments(e->wp_cur[c + 1], nc, &cm[c + 1][0], &cm[c + 1][1]);

    /* pass 1: the real-valued maps, so the shared denominators can be
     * chosen before anything is quantized */
    struct cand cl[2][N265_MAX_REF], cc[2][N265_MAX_REF][2];
    memset(cl, 0, sizeof(cl));
    memset(cc, 0, sizeof(cc));
    for (int l = 0; l < lists; l++)
        for (int r = 0; r < fw->nref[l] && r < maxref; r++) {
            const struct n265_refpic *rp = fw->refl[l][r];
            if (!rp->wp_low[0] || rp->poc == fw->poc)
                continue;
            derive_cand(cm[0], rp->wp_low[0], nl, &cl[l][r]);
            for (int c = 0; c < 2; c++)
                derive_cand(cm[c + 1], rp->wp_low[c + 1], nc, &cc[l][r][c]);
        }

    /* 7.3.6.3 carries ONE luma_log2_weight_denom and one
     * ChromaLog2WeightDenom for the whole table, both lists included, so
     * the denominator is the finest that every candidate can be written
     * at. Nothing is refused for being unrepresentable: d = 0 admits any
     * scale the clamp above allows. */
    int ld = 7, cd = 7;
    for (int l = 0; l < lists; l++)
        for (int r = 0; r < N265_MAX_REF; r++) {
            if (cl[l][r].valid) {
                int d = n265_wp_denom_for(cl[l][r].scale);
                if (d < ld) ld = d;
            }
            for (int c = 0; c < 2; c++)
                if (cc[l][r][c].valid) {
                    int d = n265_wp_denom_for(cc[l][r][c].scale);
                    if (d < cd) cd = d;
                }
        }
    fw->wp_ldenom = ld;
    fw->wp_cdenom = cd;

    /* pass 2: quantize, then let the SAD decide. */
    for (int l = 0; l < lists; l++)
        for (int r = 0; r < fw->nref[l] && r < maxref; r++) {
            n265_wp *wp = &fw->wp[l][r];
            const struct n265_refpic *rp = fw->refl[l][r];
            wp->w[0] = (int16_t)(1 << ld);
            wp->w[1] = wp->w[2] = (int16_t)(1 << cd);
            if (!cl[l][r].valid)
                continue;

            /* luma */
            int16_t w = (int16_t)n265_wp_quant(cl[l][r].scale, ld);
            long ol = lround(cl[l][r].off);
            if (ol < -128) ol = -128;
            if (ol > 127) ol = 127;
            struct sad_set ss;
            sad_sweep(e->wp_cur[0], rp->wp_low[0], nl, w, ld, (int)ol, &ss);
            int bk = -1;
            uint64_t bs = UINT64_MAX;
            for (int k = 0; k < 5; k++) {
                int o = (int)ol + k - 2;
                if (o < -128 || o > 127)
                    continue;
                if (ss.w[k] < bs) { bs = ss.w[k]; bk = k; }
            }
            int fired = 0, bwin = -1;
            if (bk >= 0 && bs + (ss.un >> wp_gain_shift()) < ss.un) {
                int o = (int)ol + bk - 2;
                bwin = block_win_pct(e->wp_cur[0], rp->wp_low[0],
                                     e->la.lw, e->la.lh, w, ld, o);
                if (bwin >= wp_block_win() && (w != (1 << ld) || o != 0)) {
                    wp->on[0] = 1;
                    wp->w[0] = w;
                    wp->o[0] = (int16_t)o;
                    fw->wp_any = 1;
                    fired = 1;
                }
            }
            if (n265_wp_trace())
                fprintf(stderr,
                        "WP poc=%d %s l%d r%d(poc=%d) sc=%.3f luma scale=%.4f off=%.2f"
                        " -> w=%d/%d o=%d sad %llu -> %llu win %d%%  %s\n",
                        fw->poc, fw->slice_type == 0 ? "B" : "P", l, r,
                        rp->poc, (double)fw->sc_ratio,
                        cl[l][r].scale, cl[l][r].off, (int)w,
                        1 << ld, (int)(bk >= 0 ? ol + bk - 2 : ol),
                        (unsigned long long)ss.un, (unsigned long long)bs,
                        bwin, fired ? "FIRE" : "-");

            /* chroma. ONE flag covers Cb and Cr (7.3.6.3), so the two
             * are decided together on the sum of their SADs; signaling
             * a good Cb map and a bad Cr one is not a thing the syntax
             * can express. */
            if (!cc[l][r][0].valid)
                continue;
            int16_t cw[2];
            int co[2];
            uint64_t cun = 0, cwd = 0;
            int ok = 1;
            for (int c = 0; c < 2; c++) {
                cw[c] = (int16_t)n265_wp_quant(cc[l][r][c].scale, cd);
                long o = lround(cc[l][r][c].off);
                if (o < -128) o = -128;
                if (o > 127) o = 127;
                co[c] = (int)o;
                struct sad_set cs;
                sad_sweep(e->wp_cur[c + 1], rp->wp_low[c + 1], nc, cw[c],
                          cd, co[c], &cs);
                int k2 = -1;
                uint64_t b2 = UINT64_MAX;
                for (int k = 0; k < 5; k++) {
                    int oo = co[c] + k - 2;
                    if (oo < -128 || oo > 127)
                        continue;
                    if (cs.w[k] < b2) { b2 = cs.w[k]; k2 = k; }
                }
                if (k2 < 0) { ok = 0; break; }
                co[c] += k2 - 2;
                cun += cs.un;
                cwd += b2;
            }
            if (!ok)
                continue;
            /* the same block test as the luma half, over BOTH chroma
             * planes together - one flag covers the pair, so one answer
             * has to. Without it sita_flat_720p's shot-change map came
             * back through the chroma door with the luma one refused. */
            int cwin = -1;
            if (cwd + (cun >> wp_gain_shift()) < cun) {
                int n0 = block_win_pct(e->wp_cur[1], rp->wp_low[1],
                                       e->wp_cw, e->wp_ch, cw[0], cd, co[0]);
                int n1 = block_win_pct(e->wp_cur[2], rp->wp_low[2],
                                       e->wp_cw, e->wp_ch, cw[1], cd, co[1]);
                cwin = (n0 + n1) / 2;
            }
            if (cwin >= wp_block_win() &&
                (cw[0] != (1 << cd) || cw[1] != (1 << cd) || co[0] || co[1])) {
                wp->on[1] = wp->on[2] = 1;
                wp->w[1] = cw[0];
                wp->w[2] = cw[1];
                wp->o[1] = (int16_t)co[0];
                wp->o[2] = (int16_t)co[1];
                fw->wp_any = 1;
                if (n265_wp_trace())
                    fprintf(stderr,
                            "WP poc=%d %s l%d r%d chroma w=%d,%d/%d o=%d,%d"
                            " sad %llu -> %llu win %d%%  FIRE\n", fw->poc,
                            fw->slice_type == 0 ? "B" : "P", l, r,
                            cw[0], cw[1], 1 << cd, co[0], co[1],
                            (unsigned long long)cun,
                            (unsigned long long)cwd, cwin);
            }
        }
}

/* knob warming, on the driver at open; see n265_ctu_warm */
void n265_wp_warm(void)
{
    (void)wp_gain_shift();
    (void)wp_block_win();
    (void)wp_max_refs();
}
