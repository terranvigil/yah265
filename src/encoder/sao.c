/*
 * Sample adaptive offset (ITU-T H.265 8.7.3): stats-based per-CTB
 * decision (off / band / four edge classes by SSD gain), syntax writer,
 * and application after deblocking. Merge flags are always coded 0 for
 * now (explicit params everywhere); the merge path is on the ideas list.
 * Copyright (c) 2026, the yah265 authors. GPL-2.0-or-later; see LICENSE.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "encoder/sao.h"

static inline int clip3(int lo, int hi, int v)
{
    return v < lo ? lo : v > hi ? hi : v;
}
static inline uint8_t clip1(int v)
{
    return (uint8_t)(v < 0 ? 0 : v > 255 ? 255 : v);
}

/* edge-class neighbor steps: 0 horiz, 1 vert, 2 135deg, 3 45deg */
static const int eo_dx[4] = { -1, 0, -1, 1 };
static const int eo_dy[4] = { 0, -1, -1, -1 };

static int eo_category(const uint8_t *p, int stride, int x, int y, int cls)
{
    int c = p[y * stride + x];
    int a = p[(y + eo_dy[cls]) * stride + x + eo_dx[cls]];
    int b = p[(y - eo_dy[cls]) * stride + x - eo_dx[cls]];
    if (c < a && c < b) return 1;
    if ((c < a && c == b) || (c == a && c < b)) return 2;
    if ((c > a && c == b) || (c == a && c > b)) return 3;
    if (c > a && c > b) return 4;
    return 0;
}

/* SSD gain of applying offset o to a category with n samples and diff sum
 * d (sum of source-recon): delta_ssd = n*o^2 - 2*o*d (negative = better) */
static int64_t gain(int64_t n, int64_t d, int o)
{
    return n * o * o - 2 * o * d;
}

/* rate of one coded offset: TU magnitude (|o|+1 bits, capped at 7) plus
 * a sign bit when the syntax carries one (band offsets; edge signs are
 * implied by the category) */
static int off_bits(int o, int sign)
{
    int a = o < 0 ? -o : o;
    int b = a >= 7 ? 7 : a + 1;
    return a ? b + sign : b;
}

/* RD offset pick: minimize n*o^2 - 2*o*d + lam*bits over the magnitude
 * path toward the SSD-optimal offset */
static int best_offset(int64_t n, int64_t d, int min_o, int max_o,
                       uint32_t lam, int sign, int64_t *rd)
{
    int o = 0;
    if (n)
        o = clip3(min_o, max_o,
                  (int)((d + (n >> 1) * (d < 0 ? -1 : 1)) / n));
    int step = o > 0 ? 1 : -1;
    int best = 0;
    int64_t bj = ((int64_t)lam * off_bits(0, sign)) >> 8;
    for (int t = step; n && t != o + step; t += step) {
        int64_t j = gain(n, d, t) +
                    (((int64_t)lam * off_bits(t, sign)) >> 8);
        if (j < bj) {
            bj = j;
            best = t;
        }
    }
    *rd = bj;
    return best;
}

/* per-component CTB stats: enough to score any parameter set */
typedef struct sao_stats {
    int64_t bn[32], bd[32];         /* band: count, diff sum per band */
    int64_t en[4][5], ed[4][5];     /* edge: per class, per category */
} sao_stats;

static void collect_stats(const uint8_t *src, const uint8_t *rec,
                          int stride, int x0, int y0, int w, int h,
                          int pw, int ph, sao_stats *st)
{
    memset(st, 0, sizeof(*st));
    for (int y = y0; y < y0 + h; y++)
        for (int x = x0; x < x0 + w; x++) {
            int r = rec[y * stride + x];
            st->bn[r >> 3]++;
            st->bd[r >> 3] += src[y * stride + x] - r;
        }
    for (int cls = 0; cls < 4; cls++)
        for (int y = y0; y < y0 + h; y++) {
            if (y + eo_dy[cls] < 0 || y - eo_dy[cls] < 0 ||
                y + eo_dy[cls] >= ph || y - eo_dy[cls] >= ph)
                continue;
            for (int x = x0; x < x0 + w; x++) {
                if (x + eo_dx[cls] < 0 || x - eo_dx[cls] < 0 ||
                    x + eo_dx[cls] >= pw || x - eo_dx[cls] >= pw)
                    continue;
                int cat = eo_category(rec, stride, x, y, cls);
                if (cat) {
                    st->en[cls][cat]++;
                    st->ed[cls][cat] +=
                        src[y * stride + x] - rec[y * stride + x];
                }
            }
        }
}

/* SSD delta of applying a given parameter set to this CTB (no rate) */
static int64_t score_params(const sao_stats *st, int type, int cop,
                            const int8_t off[4])
{
    int64_t g = 0;
    if (type == SAO_BAND)
        for (int i = 0; i < 4; i++)
            g += gain(st->bn[(cop + i) & 31], st->bd[(cop + i) & 31],
                      off[i]);
    else if (type == SAO_EDGE)
        for (int i = 0; i < 4; i++)
            g += gain(st->en[cop][i + 1], st->ed[cop][i + 1], off[i]);
    return g;
}

/* pick the RD-best explicit params for one component from its stats;
 * returns dSSD + lambda*(param bits) relative to OFF */
static int64_t analyze_comp(const sao_stats *st, uint32_t lam,
                            uint8_t *type, uint8_t *cop, int8_t off[4])
{
    const int64_t *bn = st->bn, *bd = st->bd;
    /* candidate J values are relative to SAO_OFF: syntax bits beyond the
     * type code itself (type is ~2 bins either way) are priced here -
     * band pos 5 bits, edge class 2 bits, offsets via best_offset */
    int best_pos = 0;
    int8_t best_boff[4] = { 0, 0, 0, 0 };
    int64_t best_band = INT64_MAX;
    for (int pos = 0; pos < 29; pos++) {
        int64_t g = ((int64_t)lam * 5) >> 8;
        int8_t o[4];
        for (int i = 0; i < 4; i++) {
            int64_t rd;
            o[i] = (int8_t)best_offset(bn[pos + i], bd[pos + i], -7, 7,
                                       lam, 1, &rd);
            g += rd;
        }
        if (g < best_band) {
            best_band = g;
            best_pos = pos;
            memcpy(best_boff, o, 4);
        }
    }

    /* edge classes from the stats */
    int64_t best_edge = INT64_MAX;
    int best_cls = 0;
    int8_t best_eoff[4] = { 0, 0, 0, 0 };
    for (int cls = 0; cls < 4; cls++) {
        const int64_t *en = st->en[cls], *ed = st->ed[cls];
        int8_t o[4];
        int64_t rd0, rd1, rd2, rd3;
        o[0] = (int8_t)best_offset(en[1], ed[1], 0, 7, lam, 0, &rd0);
        o[1] = (int8_t)best_offset(en[2], ed[2], 0, 7, lam, 0, &rd1);
        o[2] = (int8_t)best_offset(en[3], ed[3], -7, 0, lam, 0, &rd2);
        o[3] = (int8_t)best_offset(en[4], ed[4], -7, 0, lam, 0, &rd3);
        int64_t g = rd0 + rd1 + rd2 + rd3 + (((int64_t)lam * 2) >> 8);
        if (g < best_edge) {
            best_edge = g;
            best_cls = cls;
            memcpy(best_eoff, o, 4);
        }
    }

    if (best_band < best_edge && best_band < 0) {
        *type = SAO_BAND;
        *cop = (uint8_t)best_pos;
        memcpy(off, best_boff, 4);
        return best_band;
    }
    if (best_edge < 0) {
        *type = SAO_EDGE;
        *cop = (uint8_t)best_cls;
        memcpy(off, best_eoff, 4);
        return best_edge;
    }
    *type = SAO_OFF;
    *cop = 0;
    memset(off, 0, 4);
    return 0;
}

/* N265_SAO_GATE (R6): the statistics pass is one fused walk per
 * component per CTB and 3-4.5% of the encode, and it runs on every CTB
 * whether or not SAO has anything to correct there. Where the whole CTB
 * carried no luma residual at all - every CU a skip, or a candidate
 * whose residual quantised entirely away - the reconstruction IS the
 * motion-compensated prediction, so the error left in it is the
 * reference picture's own, and that picture's own SAO already corrected
 * it. 1 gates on that; 2 to 6 loosen it to a CTB where at most one, two,
 * four, eight or twelve of every sixteen 4x4 cells carry a luma
 * coefficient. 0 is the pre-R6 encoder, byte for byte.
 *
 * 2 is the default: it IMPROVES the band by 0.24 mean BD-VMAF-NEG while
 * buying 1.038x foreman / 1.017x mobile. That the quality moves the
 * right way is not this rung's doing - the band keeps improving all the
 * way to SAO switched off entirely (-11.06 mean against the base's
 * -9.33), so our per-CTB SAO decision is turning the filter on where it
 * loses, and 3 to 6 measure that gradient rather than a gate. They are
 * held, not shipped: a threshold tuned against a defect would have to be
 * re-derived the moment the defect is fixed. Full sweep and root cause
 * in docs/phase4-ladder.md R6. */
static int sao_gate(void)
{
    static int v = -1;
    if (v < 0) {
        const char *s = getenv("N265_SAO_GATE");
        v = s ? atoi(s) : 2;    /* nearly-clean CTBs, the mechanism-true
                                 * bound; see docs/phase4-ladder.md R6 */
    }
    return v;
}

/* the loosest cbf share, in sixteenths, this mode still gates */
static int sao_gate_share(void)
{
    switch (sao_gate()) {
    case 2:  return 1;
    case 3:  return 2;
    case 4:  return 4;
    case 5:  return 8;
    case 6:  return 12;
    default: return 0;
    }
}

/* the share of this CTB's 4x4 cells carrying a luma coefficient, in
 * sixteenths, saturating at 16 */
static int ctb_cbf_share(struct n265_encoder *e, int cx, int cy, int ctb)
{
    int x0 = cx * ctb, y0 = cy * ctb;
    int cw = (x0 + ctb > e->pad_w ? e->pad_w - x0 : ctb) >> 2;
    int ch = (y0 + ctb > e->pad_h ? e->pad_h - y0 : ctb) >> 2;
    if (cw <= 0 || ch <= 0)
        return 16;
    int nz = 0;
    for (int j = 0; j < ch; j++) {
        const uint8_t *row = e->blk_cbf_y +
            ((y0 >> 2) + j) * e->map_stride + (x0 >> 2);
        for (int i = 0; i < cw; i++)
            nz += row[i] != 0;
    }
    int tot = cw * ch;
    int q = nz * 16 / tot;
    return nz && !q ? 1 : q;    /* never report clean when it is not */
}

/* every component of this CTB's params is SAO_OFF */
static int params_all_off(const sao_params *p)
{
    return p->type[0] == SAO_OFF && p->type[1] == SAO_OFF &&
           p->type[2] == SAO_OFF;
}

void n265_sao_analyze(struct n265_encoder *e)
{
    int ctb = 1 << e->ctb_log2;
    uint32_t lam = n265_lambda_mode_q8(e->qp);
    sao_stats st[3];
    for (int cy = 0; cy < e->ctb_rows; cy++)
        for (int cx = 0; cx < e->ctb_cols; cx++) {
            sao_params *p = &e->sao[cy * e->ctb_cols + cx];
            memset(p, 0, sizeof(*p));
            if (sao_gate()) {
                int share = ctb_cbf_share(e, cx, cy, ctb);
                if (share <= sao_gate_share()) {
                    /* SAO_OFF everywhere, which memset already wrote.
                     * Say it by merging where a neighbour already says
                     * it - same result, fewer bins - and explicitly
                     * otherwise. */
                    const sao_params *l = cx > 0
                        ? &e->sao[cy * e->ctb_cols + cx - 1] : NULL;
                    const sao_params *u = cy > 0
                        ? &e->sao[(cy - 1) * e->ctb_cols + cx] : NULL;
                    if (l && params_all_off(l))
                        p->merge = 1;
                    else if (u && params_all_off(u))
                        p->merge = 2;
                    continue;
                }
            }
            int64_t j_expl = 0;
            for (int c = 0; c < 3; c++) {
                int sh = c ? 1 : 0;
                int x0 = (cx * ctb) >> sh, y0 = (cy * ctb) >> sh;
                int pw = e->pad_w >> sh, ph = e->pad_h >> sh;
                int w = (x0 + (ctb >> sh) > pw) ? pw - x0 : ctb >> sh;
                int h = (y0 + (ctb >> sh) > ph) ? ph - y0 : ctb >> sh;
                collect_stats(e->pad_plane[c], e->recon[c],
                              e->recon_stride[c], x0, y0, w, h, pw, ph,
                              &st[c]);
                j_expl += analyze_comp(&st[c], lam, &p->type[c],
                                       &p->class_or_pos[c], p->offset[c]);
            }
            /* chroma shares one type (and eo class): coded once for cb;
             * force cr to cb's type/class */
            if (p->type[2] != p->type[1] ||
                (p->type[1] == SAO_EDGE &&
                 p->class_or_pos[2] != p->class_or_pos[1])) {
                if (p->type[1] == SAO_OFF) {
                    p->type[2] = SAO_OFF;
                    memset(p->offset[2], 0, 4);
                } else {
                    p->type[2] = p->type[1];
                    p->class_or_pos[2] = p->class_or_pos[1];
                    memset(p->offset[2], 0, 4);
                }
            }
            /* type-code rate for the explicit choice (luma + cb sets) */
            for (int c = 0; c < 2; c++)
                j_expl += ((int64_t)lam *
                           (p->type[c] == SAO_OFF ? 1 : 2)) >> 8;

            /* merge candidates: neighbor params scored on our stats.
             * Bin counts: left = 1; up = (left exists) + 1; explicit
             * pays the same merge bins as zeros, folded in uniformly. */
            const sao_params *lp = cx > 0
                ? &e->sao[cy * e->ctb_cols + cx - 1] : NULL;
            const sao_params *up = cy > 0
                ? &e->sao[(cy - 1) * e->ctb_cols + cx] : NULL;
            int64_t j_left = INT64_MAX, j_up = INT64_MAX;
            if (lp) {
                j_left = ((int64_t)lam * 1) >> 8;
                for (int c = 0; c < 3; c++)
                    j_left += score_params(&st[c], lp->type[c],
                                           lp->class_or_pos[c],
                                           lp->offset[c]);
            }
            if (up) {
                j_up = ((int64_t)lam * ((lp ? 1 : 0) + 1)) >> 8;
                for (int c = 0; c < 3; c++)
                    j_up += score_params(&st[c], up->type[c],
                                         up->class_or_pos[c],
                                         up->offset[c]);
            }
            if (j_left <= j_expl && j_left <= j_up) {
                uint8_t m = 1;
                *p = *lp;
                p->merge = m;
            } else if (j_up < j_expl) {
                uint8_t m = 2;
                *p = *up;
                p->merge = m;
            } else {
                p->merge = 0;
            }
            static int hist_on = -1;
            if (hist_on < 0)
                hist_on = getenv("N265_MODE_HIST") != NULL;
            if (hist_on) {
                static int cnt[3][3];
                for (int c = 0; c < 3; c++)
                    cnt[c][p->type[c]]++;
                fprintf(stderr, "sao: l %d/%d/%d cb %d/%d/%d cr %d/%d/%d\r",
                        cnt[0][0], cnt[0][1], cnt[0][2],
                        cnt[1][0], cnt[1][1], cnt[1][2],
                        cnt[2][0], cnt[2][1], cnt[2][2]);
            }
        }
}

static void write_offset_abs(cabac_t *cb, int abs_v)
{
    /* TR cMax 7, all bypass */
    for (int i = 0; i < abs_v; i++)
        n265_cabac_bypass(cb, 1);
    if (abs_v < 7)
        n265_cabac_bypass(cb, 0);
}

void n265_sao_write(struct n265_encoder *e, cabac_t *cb, int ctb_x,
                    int ctb_y)
{
    const sao_params *p = &e->sao[ctb_y * e->ctb_cols + ctb_x];

    if (ctb_x > 0)
        n265_cabac_bin(cb, &e->ctx[CTX_SAO_MERGE], p->merge == 1);
    if (p->merge != 1 && ctb_y > 0)
        n265_cabac_bin(cb, &e->ctx[CTX_SAO_MERGE], p->merge == 2);
    if (p->merge)
        return;

    for (int c = 0; c < 3; c++) {
        if (c == 0 || c == 1) {
            /* sao_type_idx_(luma|chroma): bin0 ctx, bin1 bypass */
            int t = p->type[c];
            n265_cabac_bin(cb, &e->ctx[CTX_SAO_TYPE], t != SAO_OFF);
            if (t != SAO_OFF)
                n265_cabac_bypass(cb, t == SAO_EDGE);
        }
        int t = p->type[c];
        if (t == SAO_OFF)
            continue;
        for (int i = 0; i < 4; i++)
            write_offset_abs(cb, abs(p->offset[c][i]));
        if (t == SAO_BAND) {
            for (int i = 0; i < 4; i++)
                if (p->offset[c][i])
                    n265_cabac_bypass(cb, p->offset[c][i] < 0);
            n265_cabac_bypass_bits(cb, p->class_or_pos[c], 5);
        } else if (c == 0 || c == 1) {
            n265_cabac_bypass_bits(cb, p->class_or_pos[c], 2);
        }
    }
}

void n265_sao_apply(struct n265_encoder *e)
{
    int ctb = 1 << e->ctb_log2;
    for (int c = 0; c < 3; c++) {
        int sh = c ? 1 : 0;
        int pw = e->pad_w >> sh, ph = e->pad_h >> sh;
        int stride = e->recon_stride[c];
        size_t sz = (size_t)pw * ph;
        memcpy(e->sao_tmp[c], e->recon[c], sz);
        const uint8_t *in = e->sao_tmp[c];
        uint8_t *out = e->recon[c];

        for (int cy = 0; cy < e->ctb_rows; cy++)
            for (int cx = 0; cx < e->ctb_cols; cx++) {
                const sao_params *p = &e->sao[cy * e->ctb_cols + cx];
                if (p->type[c] == SAO_OFF)
                    continue;
                int x0 = (cx * ctb) >> sh, y0 = (cy * ctb) >> sh;
                int w = (x0 + (ctb >> sh) > pw) ? pw - x0 : ctb >> sh;
                int h = (y0 + (ctb >> sh) > ph) ? ph - y0 : ctb >> sh;
                if (p->type[c] == SAO_BAND) {
                    int pos = p->class_or_pos[c];
                    for (int y = y0; y < y0 + h; y++)
                        for (int x = x0; x < x0 + w; x++) {
                            int v = in[y * stride + x];
                            int band = (v >> 3) - pos;
                            if (band >= 0 && band < 4)
                                out[y * stride + x] =
                                    clip1(v + p->offset[c][band]);
                        }
                } else {
                    int cls = p->class_or_pos[c];
                    for (int y = y0; y < y0 + h; y++) {
                        if (y + eo_dy[cls] < 0 || y - eo_dy[cls] < 0 ||
                            y + eo_dy[cls] >= ph || y - eo_dy[cls] >= ph)
                            continue;
                        for (int x = x0; x < x0 + w; x++) {
                            if (x + eo_dx[cls] < 0 || x - eo_dx[cls] < 0 ||
                                x + eo_dx[cls] >= pw || x - eo_dx[cls] >= pw)
                                continue;
                            int cat = eo_category(in, stride, x, y, cls);
                            if (cat)
                                out[y * stride + x] = clip1(
                                    in[y * stride + x] +
                                    p->offset[c][cat - 1]);
                        }
                    }
                }
            }
    }
}
