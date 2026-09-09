/*
 * Sample adaptive offset (ITU-T H.265 8.7.3): stats-based per-CTB
 * decision (off / band / four edge classes by SSD gain), syntax writer,
 * and application after deblocking, with merge-left and merge-up priced
 * against the explicit parameters on the CTB's own statistics.
 *
 * OFF BY DEFAULT since R7 (N265_SAO_TYPES=0): measured, this filter wins
 * PSNR and loses VMAF-NEG at the same rate, and the whole of the loss is
 * the edge offset, whose sign 8.7.3.2 fixes so that it can only smooth.
 * Diagnosis, tables and the re-price triggers: docs/phase4-ladder.md R7.
 * Everything below still runs, correctly, whenever the knob says so.
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

/* One sample of the picture as SAO must see it: pre-SAO everywhere.
 * Inside the CTB row being processed and below it that is the recon
 * plane itself (rows below are deblocked and not yet SAO'd). The one
 * line SAO has already rewritten is the last line of the row above, so
 * `above` carries the copy taken before that row was applied
 * (docs/threading-design.md sec 1). `ytop` is the band's first line. */
static inline int sao_pix(const uint8_t *p, int stride, const uint8_t *above,
                          int ytop, int x, int y)
{
    return (above && y == ytop - 1) ? above[x] : p[y * stride + x];
}

static int eo_category(const uint8_t *p, int stride, const uint8_t *above,
                       int ytop, int x, int y, int cls)
{
    int c = sao_pix(p, stride, above, ytop, x, y);
    int a = sao_pix(p, stride, above, ytop, x + eo_dx[cls], y + eo_dy[cls]);
    int b = sao_pix(p, stride, above, ytop, x - eo_dx[cls], y - eo_dy[cls]);
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

static void collect_stats(const uint8_t *src, int src_stride,
                          const uint8_t *rec, int stride,
                          const uint8_t *above, int ytop, int x0, int y0,
                          int w, int h, int pw, int ph, sao_stats *st)
{
    memset(st, 0, sizeof(*st));
    for (int y = y0; y < y0 + h; y++)
        for (int x = x0; x < x0 + w; x++) {
            int r = rec[y * stride + x];
            st->bn[r >> 3]++;
            st->bd[r >> 3] += src[y * src_stride + x] - r;
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
                int cat = eo_category(rec, stride, above, ytop, x, y, cls);
                if (cat) {
                    st->en[cls][cat]++;
                    st->ed[cls][cat] +=
                        src[y * src_stride + x] - rec[y * stride + x];
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

/* N265_SAO_TYPES: bitmask of the types the per-CTB analysis may pick -
 * 1 band, 2 edge, 3 both, 0 the filter off in the SPS. 3 is HEAD. The
 * two types are not the same animal: the syntax constrains an edge
 * offset's SIGN by category (8.7.3.2 - categories 1 and 2, the local
 * minima, may only be raised, categories 3 and 4 only lowered), so edge
 * offset is a strict local smoother and cannot sharpen. A band offset
 * shifts every sample whose value lands in one of four bands whatever
 * its surroundings, which moves a level, not a gradient. */
static int sao_types(void)
{
    static int v = -1;
    if (v < 0) {
        const char *s = getenv("N265_SAO_TYPES");
        v = s ? atoi(s) : 0;    /* R7: the filter is OFF by default. It
                                 * wins PSNR and loses VMAF-NEG on every
                                 * band read; see docs/phase4-ladder.md
                                 * R7. 3 is the pre-R7 encoder. */
    }
    return v;
}

int n265_sao_enabled(void)
{
    return sao_types() != 0;
}

/* N265_SAO_QPLAM: 1 prices each CTB's candidates at that CTB's own
 * QP-derived lambda rather than the frame's; 0 is HEAD.
 * N265_SAO_BIAS: percent scale on the SAO pricing lambda, 100 = HEAD.
 * Both are documented in docs/instruments.md. */
static int sao_qplam(void)
{
    static int v = -1;
    if (v < 0) {
        const char *s = getenv("N265_SAO_QPLAM");
        v = s ? atoi(s) : 0;
    }
    return v;
}

static int sao_bias(void)
{
    static int v = -1;
    if (v < 0) {
        const char *s = getenv("N265_SAO_BIAS");
        v = s ? atoi(s) : 100;
    }
    return v;
}

/* one component's priced candidates, each J relative to SAO_OFF: syntax
 * bits beyond the type code itself (band pos 5 bits, edge class 2 bits,
 * offsets via best_offset) are inside these numbers. Keeping the four
 * edge classes apart is what lets the two chroma components, which must
 * share a type and an eo class, be decided together. */
typedef struct comp_cand {
    int64_t band_j;
    int band_pos;
    int8_t band_off[4];
    int64_t edge_j[4];
    int8_t edge_off[4][4];
} comp_cand;

static void analyze_cand(const sao_stats *st, uint32_t lam, comp_cand *c)
{
    const int64_t *bn = st->bn, *bd = st->bd;
    c->band_j = INT64_MAX;
    c->band_pos = 0;
    memset(c->band_off, 0, 4);
    for (int pos = 0; pos < 29; pos++) {
        int64_t g = ((int64_t)lam * 5) >> 8;
        int8_t o[4];
        for (int i = 0; i < 4; i++) {
            int64_t rd;
            o[i] = (int8_t)best_offset(bn[pos + i], bd[pos + i], -7, 7,
                                       lam, 1, &rd);
            g += rd;
        }
        if (g < c->band_j) {
            c->band_j = g;
            c->band_pos = pos;
            memcpy(c->band_off, o, 4);
        }
    }

    for (int cls = 0; cls < 4; cls++) {
        const int64_t *en = st->en[cls], *ed = st->ed[cls];
        int8_t *o = c->edge_off[cls];
        int64_t rd0, rd1, rd2, rd3;
        o[0] = (int8_t)best_offset(en[1], ed[1], 0, 7, lam, 0, &rd0);
        o[1] = (int8_t)best_offset(en[2], ed[2], 0, 7, lam, 0, &rd1);
        o[2] = (int8_t)best_offset(en[3], ed[3], -7, 0, lam, 0, &rd2);
        o[3] = (int8_t)best_offset(en[4], ed[4], -7, 0, lam, 0, &rd3);
        c->edge_j[cls] = rd0 + rd1 + rd2 + rd3 + (((int64_t)lam * 2) >> 8);
    }
}

/* pick the RD-best explicit params for one component from its priced
 * candidates; returns dSSD + lambda*(param bits) relative to OFF */
static int64_t pick_comp(const comp_cand *c, uint8_t *type, uint8_t *cop,
                         int8_t off[4])
{
    int64_t best_band = c->band_j, best_edge = INT64_MAX;
    int best_cls = 0;
    for (int cls = 0; cls < 4; cls++)
        if (c->edge_j[cls] < best_edge) {
            best_edge = c->edge_j[cls];
            best_cls = cls;
        }

    int allow = sao_types();
    if (!(allow & 1)) best_band = INT64_MAX;
    if (!(allow & 2)) best_edge = INT64_MAX;

    if (best_band < best_edge && best_band < 0) {
        *type = SAO_BAND;
        *cop = (uint8_t)c->band_pos;
        memcpy(off, c->band_off, 4);
        return best_band;
    }
    if (best_edge < 0) {
        *type = SAO_EDGE;
        *cop = (uint8_t)best_cls;
        memcpy(off, c->edge_off[best_cls], 4);
        return best_edge;
    }
    *type = SAO_OFF;
    *cop = 0;
    memset(off, 0, 4);
    return 0;
}

/* N265_SAO_CHROMA=1: decide cb and cr together.
 *
 * The syntax shares sao_type_idx_chroma - and, for edge, sao_eo_class -
 * between the two components, but carries a band position and four
 * offsets for each. HEAD picks each component's best type on its own and
 * then, when they disagree, forces cr onto cb's type with its offsets
 * ZEROED, while the J it already added to the CTB's total still claims
 * cr's own gain. So the explicit candidate is scored with a gain it does
 * not deliver, exactly on the CTBs where the two components disagree,
 * which biases explicit against both merge and OFF.
 *
 * The joint form prices the three legal shapes - both off, both band
 * (each with its own position), both edge on one shared class - and
 * takes the best. It is the same search, read the way the syntax is
 * shaped. */
static int64_t pick_chroma(const comp_cand *cb, const comp_cand *cr,
                           sao_params *p)
{
    int allow = sao_types();
    int64_t j_band = (allow & 1) ? cb->band_j + cr->band_j : INT64_MAX;
    int64_t j_edge = INT64_MAX;
    int best_cls = 0;
    if (allow & 2)
        for (int cls = 0; cls < 4; cls++) {
            int64_t g = cb->edge_j[cls] + cr->edge_j[cls];
            if (g < j_edge) {
                j_edge = g;
                best_cls = cls;
            }
        }

    if (j_band < j_edge && j_band < 0) {
        p->type[1] = p->type[2] = SAO_BAND;
        p->class_or_pos[1] = (uint8_t)cb->band_pos;
        p->class_or_pos[2] = (uint8_t)cr->band_pos;
        memcpy(p->offset[1], cb->band_off, 4);
        memcpy(p->offset[2], cr->band_off, 4);
        return j_band;
    }
    if (j_edge < 0) {
        p->type[1] = p->type[2] = SAO_EDGE;
        p->class_or_pos[1] = p->class_or_pos[2] = (uint8_t)best_cls;
        memcpy(p->offset[1], cb->edge_off[best_cls], 4);
        memcpy(p->offset[2], cr->edge_off[best_cls], 4);
        return j_edge;
    }
    p->type[1] = p->type[2] = SAO_OFF;
    p->class_or_pos[1] = p->class_or_pos[2] = 0;
    memset(p->offset[1], 0, 4);
    memset(p->offset[2], 0, 4);
    return 0;
}

static int sao_chroma_joint(void)
{
    static int v = -1;
    if (v < 0) {
        const char *s = getenv("N265_SAO_CHROMA");
        v = s ? atoi(s) : 1;    /* R7: the joint decision is the correct
                                 * one; 0 is the pre-R7 accounting. Moot
                                 * unless N265_SAO_TYPES is non-zero. */
    }
    return v;
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
static int ctb_cbf_share(struct n265_frame_work *e, int cx, int cy, int ctb)
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

/* N265_SAO_TRACE (diagnostic, output-neutral): the exact SSD delta this
 * parameter set produces on this CTB, computed the way n265_sao_apply
 * will produce it - clipping included, neighbours unfiltered - so it can
 * be held against the stats model's prediction. */
static int64_t true_dssd(const uint8_t *src, int src_stride,
                         const uint8_t *rec, int stride,
                         const uint8_t *above, int ytop, int x0, int y0,
                         int w, int h,
                         int pw, int ph, int type, int cop,
                         const int8_t off[4])
{
    if (type == SAO_OFF)
        return 0;
    int64_t d = 0;
    for (int y = y0; y < y0 + h; y++)
        for (int x = x0; x < x0 + w; x++) {
            int v = rec[y * stride + x], nv = v;
            if (type == SAO_BAND) {
                int band = (v >> 3) - cop;
                if (band >= 0 && band < 4)
                    nv = clip1(v + off[band]);
            } else if (y + eo_dy[cop] >= 0 && y - eo_dy[cop] >= 0 &&
                       y + eo_dy[cop] < ph && y - eo_dy[cop] < ph &&
                       x + eo_dx[cop] >= 0 && x - eo_dx[cop] >= 0 &&
                       x + eo_dx[cop] < pw && x - eo_dx[cop] < pw) {
                int cat = eo_category(rec, stride, above, ytop, x, y, cop);
                if (cat)
                    nv = clip1(v + off[cat - 1]);
            }
            int s = src[y * src_stride + x];
            d += (int64_t)(nv - s) * (nv - s) - (int64_t)(v - s) * (v - s);
        }
    return d;
}

static FILE *sao_trace(void)
{
    static FILE *f = (FILE *)-1;
    if (f == (FILE *)-1) {
        const char *s = getenv("N265_SAO_TRACE");
        f = s ? fopen(s, "w") : NULL;
        if (f)
            fprintf(f, "poc,qp,cqp,gated,merge,t0,t1,t2,jexpl,jleft,jup,"
                       "predd,actd,predd0,actd0\n");
    }
    return f;
}

static void sao_analyze_row(struct n265_frame_work *e, int cy)
{
    int ctb = 1 << e->ctb_log2;
    int qplam = sao_qplam(), bias = sao_bias();
    uint32_t frame_lam = n265_lambda_mode_q8(e->qp);
    sao_stats st[3];
    {
        for (int cx = 0; cx < e->ctb_cols; cx++) {
            sao_params *p = &e->sao[cy * e->ctb_cols + cx];
            uint32_t lam = frame_lam;
            if (qplam) {
                int q = e->qp + e->ctu_qp_off[cy * e->ctb_cols + cx];
                lam = n265_lambda_mode_q8(q < 0 ? 0 : q > 51 ? 51 : q);
            }
            if (bias != 100)
                lam = (uint32_t)(((uint64_t)lam * (uint32_t)bias) / 100u);
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
                    FILE *tf = sao_trace();
                    if (tf)
                        fprintf(tf, "%d,%d,%d,1,%d,0,0,0,0,0,0,0,0,0,0\n",
                                e->poc, e->qp,
                                e->qp + e->ctu_qp_off[cy * e->ctb_cols + cx],
                                p->merge);
                    continue;
                }
            }
            int64_t j_expl = 0;
            comp_cand cand[3];
            for (int c = 0; c < 3; c++) {
                int sh = c ? 1 : 0;
                int x0 = (cx * ctb) >> sh, y0 = (cy * ctb) >> sh;
                int pw = e->pad_w >> sh, ph = e->pad_h >> sh;
                int w = (x0 + (ctb >> sh) > pw) ? pw - x0 : ctb >> sh;
                int h = (y0 + (ctb >> sh) > ph) ? ph - y0 : ctb >> sh;
                collect_stats(e->pad_plane[c], e->pad_stride[c],
                              e->recon[c], e->recon_stride[c],
                              cy > 0 ? e->sao_line[c] : NULL, y0,
                              x0, y0, w, h, pw, ph, &st[c]);
                analyze_cand(&st[c], lam, &cand[c]);
            }
            if (sao_chroma_joint()) {
                j_expl += pick_comp(&cand[0], &p->type[0],
                                    &p->class_or_pos[0], p->offset[0]);
                j_expl += pick_chroma(&cand[1], &cand[2], p);
            } else {
                for (int c = 0; c < 3; c++)
                    j_expl += pick_comp(&cand[c], &p->type[c],
                                        &p->class_or_pos[c], p->offset[c]);
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
            FILE *tf = sao_trace();
            if (tf) {
                int64_t pd = 0, ad = 0, pd0 = 0, ad0 = 0;
                for (int c = 0; c < 3; c++) {
                    int sh = c ? 1 : 0;
                    int x0 = (cx * ctb) >> sh, y0 = (cy * ctb) >> sh;
                    int pw = e->pad_w >> sh, ph = e->pad_h >> sh;
                    int w = (x0 + (ctb >> sh) > pw) ? pw - x0 : ctb >> sh;
                    int h = (y0 + (ctb >> sh) > ph) ? ph - y0 : ctb >> sh;
                    int64_t a = true_dssd(e->pad_plane[c], e->pad_stride[c],
                                          e->recon[c], e->recon_stride[c],
                                          cy > 0 ? e->sao_line[c] : NULL, y0,
                                          x0, y0, w, h,
                                          pw, ph, p->type[c],
                                          p->class_or_pos[c], p->offset[c]);
                    pd += score_params(&st[c], p->type[c],
                                       p->class_or_pos[c], p->offset[c]);
                    ad += a;
                    if (c == 0) { pd0 = pd; ad0 = a; }
                }
                fprintf(tf, "%d,%d,%d,0,%d,%d,%d,%d,%lld,%lld,%lld,"
                            "%lld,%lld,%lld,%lld\n",
                        e->poc, e->qp,
                        e->qp + e->ctu_qp_off[cy * e->ctb_cols + cx],
                        p->merge, p->type[0], p->type[1], p->type[2],
                        (long long)j_expl,
                        (long long)(j_left == INT64_MAX ? 0 : j_left),
                        (long long)(j_up == INT64_MAX ? 0 : j_up),
                        (long long)pd, (long long)ad,
                        (long long)pd0, (long long)ad0);
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
}

static void write_offset_abs(cabac_t *cb, int abs_v)
{
    /* TR cMax 7, all bypass */
    for (int i = 0; i < abs_v; i++)
        n265_cabac_bypass(cb, 1);
    if (abs_v < 7)
        n265_cabac_bypass(cb, 0);
}

void n265_sao_write(struct n265_frame_work *e, cabac_t *cb, int ctb_x,
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

/* Apply row cy's decided offsets in place, then publish. The pristine
 * copy is only the band (SAO reads at most one line either side of a
 * sample): the line above comes from sao_line[], saved before the row
 * above was applied, and the line below is still pre-SAO in the recon
 * plane because sao(cy+1) has not run. */
static void sao_apply_row(struct n265_frame_work *e, int cy)
{
    int ctb = 1 << e->ctb_log2;
    for (int c = 0; c < 3; c++) {
        int sh = c ? 1 : 0;
        int pw = e->pad_w >> sh, ph = e->pad_h >> sh;
        int stride = e->recon_stride[c];
        int bh = ctb >> sh;
        int y0 = (cy * ctb) >> sh;
        int y1 = y0 + bh > ph ? ph : y0 + bh;
        const uint8_t *above = cy > 0 ? e->sao_line[c] : NULL;
        uint8_t *in = e->sao_tmp[c];        /* band copy, stride pw */
        uint8_t *out = e->recon[c];

        for (int y = y0; y < y1; y++)
            memcpy(in + (size_t)(y - y0) * pw, out + (size_t)y * stride,
                   (size_t)pw);

        for (int cx = 0; cx < e->ctb_cols; cx++) {
            const sao_params *p = &e->sao[cy * e->ctb_cols + cx];
            if (p->type[c] == SAO_OFF)
                continue;
            int x0 = (cx * ctb) >> sh;
            int w = (x0 + (ctb >> sh) > pw) ? pw - x0 : ctb >> sh;
            if (p->type[c] == SAO_BAND) {
                int pos = p->class_or_pos[c];
                for (int y = y0; y < y1; y++)
                    for (int x = x0; x < x0 + w; x++) {
                        int v = in[(size_t)(y - y0) * pw + x];
                        int band = (v >> 3) - pos;
                        if (band >= 0 && band < 4)
                            out[(size_t)y * stride + x] =
                                clip1(v + p->offset[c][band]);
                    }
            } else {
                int cls = p->class_or_pos[c];
                for (int y = y0; y < y1; y++) {
                    if (y + eo_dy[cls] < 0 || y - eo_dy[cls] < 0 ||
                        y + eo_dy[cls] >= ph || y - eo_dy[cls] >= ph)
                        continue;
                    for (int x = x0; x < x0 + w; x++) {
                        if (x + eo_dx[cls] < 0 || x - eo_dx[cls] < 0 ||
                            x + eo_dx[cls] >= pw || x - eo_dx[cls] >= pw)
                            continue;
                        /* neighbours: inside the band from the pristine
                         * copy, one line above from sao_line, one line
                         * below straight from the (still pre-SAO) recon */
                        int px[3], k = 0;
                        for (int d = -1; d <= 1; d++) {
                            int yy = y + d * eo_dy[cls];
                            int xx = x + d * eo_dx[cls];
                            if (d == 0) { yy = y; xx = x; }
                            px[k++] = (above && yy == y0 - 1) ? above[xx]
                                : (yy >= y0 && yy < y1)
                                    ? in[(size_t)(yy - y0) * pw + xx]
                                    : out[(size_t)yy * stride + xx];
                        }
                        /* the two neighbours are interchangeable in
                         * every rule below, so their order does not
                         * matter */
                        int n0 = px[0], cc = px[1], n1 = px[2];
                        int cat = 0;
                        if (cc < n0 && cc < n1) cat = 1;
                        else if ((cc < n0 && cc == n1) || (cc == n0 && cc < n1)) cat = 2;
                        else if ((cc > n0 && cc == n1) || (cc == n0 && cc > n1)) cat = 3;
                        else if (cc > n0 && cc > n1) cat = 4;
                        if (cat)
                            out[(size_t)y * stride + x] =
                                clip1(cc + p->offset[c][cat - 1]);
                    }
                }
            }
        }
        /* the next row's `above` is this band's last line, pre-SAO. It
         * has to be taken AFTER the band is applied: `above` aliases
         * this same buffer while the offsets are being applied. */
        memcpy(e->sao_line[c], in + (size_t)(y1 - 1 - y0) * pw, (size_t)pw);
    }
}

/* One CTB row of the filter stage: decide this row's params against the
 * deblocked picture, then apply them. Ready when deblock has covered
 * rows cy and cy+1 (sec 1). */
void n265_sao_row(struct n265_frame_work *e, int cy)
{
    sao_analyze_row(e, cy);
    sao_apply_row(e, cy);
}
