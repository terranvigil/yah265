/*
 * Unit tests: weighted prediction (phase 14 item 2). The pred_weight_table
 * writer (7.3.6.3), the explicit weighted sample prediction (8.5.3.3.4.3)
 * and the weight derivation.
 * Copyright (c) 2026, the yah265 authors. GPL-2.0-or-later; see LICENSE.
 *
 * Four halves, and they fail on different bugs:
 *
 *  - a hand-written READER over 7.3.6.3, which walks every field of a
 *    table the encoder wrote and then checks it ended exactly where the
 *    writer ended. A writer that drops a field, orders the two flag runs
 *    wrongly, or writes the chroma offset as the OFFSET rather than as
 *    7.4.7.3's delta, survives a value check and does not survive this.
 *  - the chroma offset's round trip through the DECODER's derivation,
 *    written out here from the clause rather than from the encoder, so
 *    the two sides are not the same arithmetic read twice.
 *  - 8.5.3.3.4.3 at hand-computed samples, uni and bi.
 *  - the IDENTITY: at w = 1 << denom and o = 0 the explicit clause has to
 *    reproduce the default prediction exactly, both forms. That is the
 *    claim the whole feature's escape rests on - turning the PPS flag on
 *    moves the syntax and not one coded sample - and it is arithmetic,
 *    so it is checked as arithmetic rather than by encoding something.
 *
 * What a unit test cannot check is whether a decoder agrees; that is
 * docs/phase14.md's ffmpeg and dec265 legs on the new conformance
 * vectors.
 */

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "common/bitstream.h"
#include "encoder/encoder.h"
#include "encoder/mc.h"
#include "encoder/wp.h"

static int fails;

#define CHECK(cond, ...) do { \
    if (!(cond)) { fails++; printf("FAIL %s:%d: ", __FILE__, __LINE__); \
                   printf(__VA_ARGS__); printf("\n"); } \
} while (0)

/* An MSB-first reader, written against the syntax table. */
typedef struct { const uint8_t *b; size_t n; size_t pos; int over; } BR;

static uint32_t u(BR *r, int nbits)
{
    uint32_t v = 0;
    for (int i = 0; i < nbits; i++) {
        if (r->pos >= r->n * 8) { r->over = 1; return v; }
        v = (v << 1) | ((r->b[r->pos >> 3] >> (7 - (r->pos & 7))) & 1);
        r->pos++;
    }
    return v;
}

static uint32_t ue(BR *r)
{
    int lz = 0;
    while (!u(r, 1)) {
        if (r->over) return 0;
        if (++lz > 31) { r->over = 1; return 0; }
    }
    uint32_t v = lz ? u(r, lz) : 0;
    return ((uint32_t)1 << lz) - 1 + v;
}

static int32_t se(BR *r)
{
    uint32_t k = ue(r);
    return (k & 1) ? (int32_t)((k + 1) >> 1) : -(int32_t)(k >> 1);
}

/* 7.4.7.3's own derivation of ChromaOffsetLX from the syntax element,
 * written from the clause and not from the encoder's inverse of it. */
static int chroma_offset_of(int delta, int w, int d)
{
    int half = 128;                 /* WpOffsetHalfRangeC, no high precision */
    int v = half + delta - ((half * w) >> d);
    return v < -half ? -half : v > half - 1 ? half - 1 : v;
}

/* ---- 1. the table, written and read back ------------------------- */

static void parse_table(const bitstream_t *bs, size_t bits,
                        const n265_frame_work *fw)
{
    BR r = { bs->buf, bs->bytes, 0, 0 };
    int lists = fw->slice_type == 0 ? 2 : 1;

    int ld = (int)ue(&r);
    int cd = ld + se(&r);
    CHECK(ld == fw->wp_ldenom, "luma_log2_weight_denom %d != %d",
          ld, fw->wp_ldenom);
    CHECK(cd == fw->wp_cdenom, "ChromaLog2WeightDenom %d != %d",
          cd, fw->wp_cdenom);
    CHECK(ld >= 0 && ld <= 7, "luma_log2_weight_denom out of range: %d", ld);
    CHECK(cd >= 0 && cd <= 7, "ChromaLog2WeightDenom out of range: %d", cd);

    for (int l = 0; l < lists; l++) {
        int lf[N265_MAX_REF], cf[N265_MAX_REF];
        /* the two flag runs are SEPARATE loops in the syntax, all the
         * luma flags and then all the chroma ones - not interleaved */
        for (int i = 0; i < fw->nref[l]; i++) {
            lf[i] = (int)u(&r, 1);
            CHECK(lf[i] == (fw->wp[l][i].on[0] != 0),
                  "luma_weight_l%d_flag[%d] %d", l, i, lf[i]);
        }
        for (int i = 0; i < fw->nref[l]; i++) {
            cf[i] = (int)u(&r, 1);
            CHECK(cf[i] == (fw->wp[l][i].on[1] != 0),
                  "chroma_weight_l%d_flag[%d] %d", l, i, cf[i]);
        }
        for (int i = 0; i < fw->nref[l]; i++) {
            const n265_wp *wp = &fw->wp[l][i];
            if (lf[i]) {
                int dw = se(&r), o = se(&r);
                CHECK(dw >= -128 && dw <= 127,
                      "delta_luma_weight out of range: %d", dw);
                CHECK(o >= -128 && o <= 127,
                      "luma_offset out of range: %d", o);
                CHECK((1 << ld) + dw == wp->w[0],
                      "LumaWeight l%d[%d] %d != %d", l, i,
                      (1 << ld) + dw, wp->w[0]);
                CHECK(o == wp->o[0], "luma_offset l%d[%d] %d != %d", l, i,
                      o, wp->o[0]);
            }
            if (cf[i])
                for (int j = 0; j < 2; j++) {
                    int dw = se(&r), dof = se(&r);
                    CHECK(dw >= -128 && dw <= 127,
                          "delta_chroma_weight out of range: %d", dw);
                    CHECK((1 << cd) + dw == wp->w[j + 1],
                          "ChromaWeight l%d[%d][%d] %d != %d", l, i, j,
                          (1 << cd) + dw, wp->w[j + 1]);
                    /* the decoder's own derivation, and it has to land
                     * back on the offset the encoder meant */
                    int got = chroma_offset_of(dof, (1 << cd) + dw, cd);
                    CHECK(got == wp->o[j + 1],
                          "ChromaOffset l%d[%d][%d] %d != %d", l, i, j,
                          got, wp->o[j + 1]);
                }
        }
    }
    CHECK(!r.over, "reader ran off the end of the table");
    CHECK(r.pos == bits, "table is %zu bits, reader consumed %zu", bits,
          r.pos);
}

static void test_table(void)
{
    n265_frame_work *fw = calloc(1, sizeof(*fw));
    if (!fw) { printf("FAIL out of memory\n"); fails++; return; }
    bitstream_t bs;
    bs_init(&bs, 256);

    /* a B slice with three references on L0 and two on L1, and every
     * shape of entry in it: luma alone, luma and chroma, chroma alone
     * (which the syntax permits and the estimator does not produce -
     * writing it here is how the reader proves the two flag runs are
     * independent), and one reference with nothing at all */
    fw->slice_type = 0;
    fw->nref[0] = 3;
    fw->nref[1] = 2;
    fw->wp_ldenom = 6;
    fw->wp_cdenom = 5;
    fw->wp[0][0].on[0] = 1;
    fw->wp[0][0].w[0] = 70;  fw->wp[0][0].o[0] = -12;
    fw->wp[0][1].on[0] = 1;  fw->wp[0][1].on[1] = fw->wp[0][1].on[2] = 1;
    fw->wp[0][1].w[0] = 64;  fw->wp[0][1].o[0] = 3;
    fw->wp[0][1].w[1] = 40;  fw->wp[0][1].o[1] = 17;
    fw->wp[0][1].w[2] = 33;  fw->wp[0][1].o[2] = -128;
    /* fw->wp[0][2] stays entirely off */
    fw->wp[1][0].on[1] = fw->wp[1][0].on[2] = 1;
    fw->wp[1][0].w[1] = 32;  fw->wp[1][0].o[1] = 0;
    fw->wp[1][0].w[2] = 159; fw->wp[1][0].o[2] = 127;
    fw->wp[1][1].on[0] = 1;
    /* 191 is (1 << 6) + 127, the largest weight denom 6 can WRITE - and
     * exactly what n265_wp_quant clamps to there. 192 is one past it and
     * the reader's range check is what says so. */
    fw->wp[1][1].w[0] = 191; fw->wp[1][1].o[0] = -1;

    bs_reset(&bs);
    n265_write_pred_weight_table(&bs, fw);
    /* the writer leaves the last partial byte pending; take the count
     * before the align that flushes it */
    size_t bits = bs.bytes * 8 + (size_t)bs.acc_bits;
    bs_align_zero(&bs);
    parse_table(&bs, bits, fw);

    /* ...and a P slice, whose table has NO L1 half. The reader's
     * end-of-table check is what catches a writer that emits one. */
    fw->slice_type = 1;
    fw->nref[0] = 1;
    fw->wp_ldenom = 7;
    fw->wp_cdenom = 7;
    memset(fw->wp, 0, sizeof(fw->wp));
    fw->wp[0][0].on[0] = fw->wp[0][0].on[1] = fw->wp[0][0].on[2] = 1;
    fw->wp[0][0].w[0] = 128; fw->wp[0][0].o[0] = 0;
    fw->wp[0][0].w[1] = 255; fw->wp[0][0].o[1] = -5;
    fw->wp[0][0].w[2] = 1;   fw->wp[0][0].o[2] = 5;
    bs_reset(&bs);
    n265_write_pred_weight_table(&bs, fw);
    bits = bs.bytes * 8 + (size_t)bs.acc_bits;
    bs_align_zero(&bs);
    parse_table(&bs, bits, fw);

    bs_free(&bs);
    free(fw);
}

/* ---- 2. 8.5.3.3.4.3 at hand values -------------------------------- */

static void test_predict(void)
{
    /* shift1 = 14 - BitDepth, so log2Wd = denom + 6 at 8 bits and
     * denom + 4 at 10. Both arms are written out by hand below from the
     * clause, at the depth this build actually is. */
    const int shift1 = 14 - BIT_DEPTH;
    int16_t a[4], b[4];
    pixel out[4];

    /* uni: pred 1000, w 80, denom 6, offset -5.
     * log2Wd = 6 + shift1; ((1000 * 80 + (1 << (log2Wd-1))) >> log2Wd)
     * + (-5 << (BitDepth - 8)). */
    a[0] = 1000;
    n265_wp_uni_wh(out, 4, a, 1, 1, 80, -5, 6);
    {
        int log2Wd = 6 + shift1;
        int want = ((1000 * 80 + (1 << (log2Wd - 1))) >> log2Wd)
                   + (-5 * (1 << (BIT_DEPTH - 8)));
        if (want < 0) want = 0;
        if (want > PIXEL_MAX) want = PIXEL_MAX;
        CHECK(out[0] == want, "uni: %d != %d", (int)out[0], want);
    }

    /* bi: 1000 and 1200, weights 80 and 70, offsets -5 and 3, denom 6 */
    a[0] = 1000; b[0] = 1200;
    n265_wp_bi_wh(out, 4, a, b, 1, 1, 80, -5, 70, 3, 6);
    {
        int log2Wd = 6 + shift1;
        /* multiplies, not shifts: both brackets go negative here and a
         * negative left shift is undefined - which is the same bug
         * UBSan found in the kernel this line is checking */
        int add = ((-5 + 3) * (1 << (BIT_DEPTH - 8)) + 1) * (1 << log2Wd);
        int want = (1000 * 80 + 1200 * 70 + add) >> (log2Wd + 1);
        if (want < 0) want = 0;
        if (want > PIXEL_MAX) want = PIXEL_MAX;
        CHECK(out[0] == want, "bi: %d != %d", (int)out[0], want);
    }

    /* the clip is real: a weight that overshoots has to saturate, not
     * wrap, and a negative result has to floor at 0 */
    a[0] = 16000;
    n265_wp_uni_wh(out, 4, a, 1, 1, 255, 127, 7);
    CHECK(out[0] == PIXEL_MAX, "uni high clip: %d", (int)out[0]);
    a[0] = 0;
    n265_wp_uni_wh(out, 4, a, 1, 1, 64, -128, 6);
    CHECK(out[0] == 0, "uni low clip: %d", (int)out[0]);
}

/* ---- 3. the identity ---------------------------------------------- */

static void test_identity(void)
{
    /* The whole feature's escape rests on this: with the PPS flag on and
     * a reference carrying no flag, the decoder runs the EXPLICIT clause
     * at w = 1 << denom and o = 0, and that has to be the default
     * prediction to the sample. If it were not, `--weightp` would move
     * every coded picture of every clip and the tool would be a
     * regression everywhere it does not fire. */
    enum { W = 16, H = 8 };
    int16_t a[W * H], b[W * H];
    pixel got[W * H], want[W * H];
    unsigned seed = 12345;
    for (int i = 0; i < W * H; i++) {
        seed = seed * 1103515245u + 12345u;
        /* the 14-bit intermediate's real range: the 8-tap can undershoot
         * and overshoot the sample range at both ends */
        a[i] = (int16_t)((int)(seed >> 17) - 8192);
        seed = seed * 1103515245u + 12345u;
        b[i] = (int16_t)((int)(seed >> 17) - 8192);
    }
    for (int d = 0; d <= 7; d++) {
        /* uni against the plain rounding of 8.5.3.3.3.2 */
        n265_wp_uni_wh(got, W, a, W, H, 1 << d, 0, d);
        for (int i = 0; i < W * H; i++) {
            int v = (a[i] + (1 << (13 - BIT_DEPTH))) >> (14 - BIT_DEPTH);
            want[i] = (pixel)(v < 0 ? 0 : v > PIXEL_MAX ? PIXEL_MAX : v);
        }
        CHECK(!memcmp(got, want, sizeof(want)),
              "uni identity differs at denom %d", d);

        /* bi against n265_bi_avg_wh itself */
        n265_wp_bi_wh(got, W, a, b, W, H, 1 << d, 0, 1 << d, 0, d);
        n265_bi_avg_wh(want, W, a, b, W, H);
        CHECK(!memcmp(got, want, sizeof(want)),
              "bi identity differs at denom %d", d);
    }
}

/* ---- 4. the derivation -------------------------------------------- */

static void test_derive(void)
{
    /* A reference that IS the current picture through a known map: the
     * fit has to find that map back. The plane is a ramp with structure
     * so the second moment is not degenerate. */
    /* The planes the fit really reads are the LOWRES ones, which carry
     * 8-BIT magnitudes at every depth (la.h), so the values here are
     * 8-bit at every depth too and the answer is depth-independent -
     * which is the whole reason the offset can be written in the
     * syntax's own 8-bit units and shifted at use. */
    enum { N = 64 * 64 };
    static pixel cur[N], ref[N];
    for (int i = 0; i < N; i++) {
        int v = (i * 7 + (i / 64) * 3) & 255;
        ref[i] = (pixel)v;
        int c = (v * 128 + 64) / 256 + 20;   /* scale 0.5, offset +20 */
        cur[i] = (pixel)(c < 0 ? 0 : c > 255 ? 255 : c);
    }
    double scale, off;
    n265_wp_fit(cur, ref, N, &scale, &off);
    CHECK(fabs(scale - 0.5) < 0.02, "scale %.4f, wanted 0.5", scale);
    CHECK(fabs(off - 20.0) < 2.0, "offset %.2f, wanted 20", off);

    /* the denominator is the finest one that can WRITE the scale */
    CHECK(n265_wp_denom_for(1.0) == 7, "denom(1.0) = %d",
          n265_wp_denom_for(1.0));
    CHECK(n265_wp_denom_for(0.5) == 7, "denom(0.5) = %d",
          n265_wp_denom_for(0.5));
    CHECK(n265_wp_quant(0.5, 7) == 64, "quant(0.5, 7) = %d",
          n265_wp_quant(0.5, 7));
    CHECK(n265_wp_quant(1.0, 6) == 64, "quant(1.0, 6) = %d",
          n265_wp_quant(1.0, 6));
    /* 1.99 * 128 = 255, which is (1 << 7) + 127 exactly: the last scale
     * denom 7 can hold. 2.0 cannot be written there and has to fall */
    CHECK(n265_wp_denom_for(1.99) == 7, "denom(1.99) = %d",
          n265_wp_denom_for(1.99));
    CHECK(n265_wp_denom_for(2.5) == 6, "denom(2.5) = %d",
          n265_wp_denom_for(2.5));
    CHECK(n265_wp_denom_for(100.0) == 0, "denom(100.0) = %d",
          n265_wp_denom_for(100.0));
    /* every denominator this can return has to actually hold the weight
     * it was chosen for, or the syntax element goes out of range */
    for (double s = 0.02; s < 100.0; s *= 1.3) {
        int d = n265_wp_denom_for(s);
        int w = n265_wp_quant(s, d);
        CHECK(d >= 0 && d <= 7, "denom(%.3f) = %d", s, d);
        CHECK(w - (1 << d) >= -128 && w - (1 << d) <= 127,
              "delta at scale %.3f denom %d is %d", s, d, w - (1 << d));
    }

    /* a flat reference carries no scale information, so the map is the
     * offset alone and nothing divides by the noise floor */
    static pixel flat[N];
    for (int i = 0; i < N; i++)
        flat[i] = (pixel)100;
    n265_wp_fit(cur, flat, N, &scale, &off);
    CHECK(scale == 1.0, "flat reference scale %.4f, wanted 1.0", scale);
}

int main(void)
{
    test_table();
    test_predict();
    test_identity();
    test_derive();
    if (fails) {
        printf("%d failure(s)\n", fails);
        return 1;
    }
    printf("all weighted-prediction tests passed\n");
    return 0;
}
