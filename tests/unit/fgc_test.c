/*
 * Unit tests: the film_grain_characteristics SEI writer (H.265 D.2.20,
 * src/encoder/headers.c) and the model's own invariants.
 * Copyright (c) 2026, the yah265 authors. GPL-2.0-or-later; see LICENSE.
 *
 * The writer is checked against a HAND-PARSED reference: a bit reader
 * written here, from the syntax table and not from the writer, walks the
 * RBSP field by field and the test asserts every value and - just as
 * important - that the reader ends exactly where the writer ended. A
 * writer that puts a field in the wrong order, or writes one bit too few,
 * passes a check that only compares values; it cannot pass this one.
 *
 * The one thing a unit test cannot check is whether a DECODER agrees. That
 * is the conformance gate's job, and phase 12 item 3's record names the
 * two decoders the message went through.
 */

#include <stdio.h>
#include <string.h>

#include "common/bitstream.h"
#include "encoder/grain.h"

void n265_write_fgc_sei(bitstream_t *bs, const n265_grain_model *m);

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
    return (k & 1) ? (int32_t)((k + 1) / 2) : -(int32_t)(k / 2);
}

/* Walk one written SEI and check it says what the model said. */
static void parse_and_check(const bitstream_t *bs, const n265_grain_model *m)
{
    BR r = { bs->buf, bs->bytes, 0, 0 };
    CHECK(bs->bytes >= 4, "SEI too short: %zu bytes", bs->bytes);

    /* sei_message(): payload type, then payload size, both ff-coded */
    uint32_t type = 0, b;
    do { b = u(&r, 8); type += b; } while (b == 255);
    CHECK(type == 19, "payload type %u, want 19 (film grain)", type);
    uint32_t size = 0;
    do { b = u(&r, 8); size += b; } while (b == 255);
    size_t pay_start = r.pos / 8;
    CHECK(r.pos % 8 == 0, "payload does not start on a byte");

    CHECK(u(&r, 1) == 0, "cancel_flag must be 0 for a model we wrote");
    CHECK(u(&r, 2) == 0, "model_id must be 0 (frequency filtering)");
    CHECK(u(&r, 1) == 0, "separate_colour_description must be 0");
    CHECK(u(&r, 2) == 0, "blending_mode_id must be 0 (additive)");
    uint32_t log2 = u(&r, 4);
    CHECK((int)log2 == m->log2_scale, "log2_scale_factor %u, want %d",
          log2, m->log2_scale);
    int present[3];
    for (int c = 0; c < 3; c++) {
        present[c] = (int)u(&r, 1);
        CHECK(present[c] == (m->comp[c] != 0),
              "comp_model_present[%d] %d, want %d", c, present[c],
              m->comp[c] != 0);
    }
    for (int c = 0; c < 3; c++) {
        if (!present[c])
            continue;
        uint32_t niv = u(&r, 8) + 1;
        uint32_t nval = u(&r, 3) + 1;
        CHECK((int)niv == m->nival[c], "comp %d intervals %u, want %d",
              c, niv, m->nival[c]);
        CHECK(nval == N265_FGC_NVALS, "comp %d model values %u, want %d",
              c, nval, N265_FGC_NVALS);
        int prev_hi = -1;
        for (uint32_t i = 0; i < niv && !r.over; i++) {
            uint32_t lo = u(&r, 8), hi = u(&r, 8);
            CHECK(lo == m->lo[c][i] && hi == m->hi[c][i],
                  "comp %d interval %u [%u,%u], want [%u,%u]", c, i, lo, hi,
                  m->lo[c][i], m->hi[c][i]);
            /* The decoder takes the FIRST interval containing a block's
             * mean, so the set has to be ordered, disjoint and complete:
             * an overlap silently shadows a later interval and a gap
             * silently synthesizes nothing there. */
            CHECK((int)lo == prev_hi + 1,
                  "comp %d interval %u starts at %u, previous ended at %d "
                  "(intervals must be contiguous and ordered)",
                  c, i, lo, prev_hi);
            CHECK(hi >= lo, "comp %d interval %u is empty", c, i);
            prev_hi = (int)hi;
            int32_t v0 = se(&r), v1 = se(&r), v2 = se(&r);
            CHECK(v0 == m->scale[c][i], "comp %d [%u] scale %d, want %d",
                  c, i, v0, m->scale[c][i]);
            CHECK(v1 == m->hcut[c][i] && v2 == m->vcut[c][i],
                  "comp %d [%u] cut-offs %d/%d, want %d/%d", c, i, v1, v2,
                  m->hcut[c][i], m->vcut[c][i]);
            /* 8.5 of H.274 clips the cut-off to 2..14 and the synthesis
             * holds the grain in an int8, so scale * 127 >> (log2 + 6)
             * has to stay inside a signed byte. */
            CHECK(v1 >= 2 && v1 <= 14 && v2 >= 2 && v2 <= 14,
                  "comp %d [%u] cut-off %d/%d outside 2..14", c, i, v1, v2);
            CHECK(v0 >= 0 && (long)v0 * 127 >> (log2 + 6) <= 127,
                  "comp %d [%u] scale %d overflows the int8 grain at "
                  "log2_scale_factor %u", c, i, v0, log2);
        }
        CHECK(prev_hi == 255, "comp %d intervals stop at %d, not 255: a "
              "block brighter than that would get no grain", c, prev_hi);
    }
    CHECK(u(&r, 1) == 1, "persistence_flag must be 1");
    /* D.2.1: pad to a byte with a one then zeros, and only when needed */
    if (r.pos % 8) {
        CHECK(u(&r, 1) == 1, "payload_bit_equal_to_one missing");
        while (r.pos % 8)
            CHECK(u(&r, 1) == 0, "payload_bit_equal_to_zero is not zero");
    }
    CHECK(!r.over, "reader ran off the end of the SEI");
    CHECK(r.pos / 8 - pay_start == size,
          "payload_size says %u bytes, the payload parsed as %zu",
          size, r.pos / 8 - pay_start);
    /* rbsp_trailing_bits */
    CHECK(r.pos / 8 + 1 == bs->bytes, "%zu bytes after the payload, want 1",
          bs->bytes - r.pos / 8);
    CHECK(bs->buf[bs->bytes - 1] == 0x80, "rbsp trailing byte 0x%02x, want 0x80",
          bs->buf[bs->bytes - 1]);
}

static void set_ival(n265_grain_model *m, int c, int i, int lo, int hi,
                     int scale, int cut)
{
    m->lo[c][i] = (uint8_t)lo;
    m->hi[c][i] = (uint8_t)hi;
    m->scale[c][i] = (int16_t)scale;
    m->hcut[c][i] = (int8_t)cut;
    m->vcut[c][i] = (int8_t)cut;
}

int main(void)
{
    bitstream_t bs;
    if (bs_init(&bs, 4096) < 0) {
        printf("FAIL: bs_init\n");
        return 1;
    }

    /* 1. One luma interval covering the range: the smallest legal model,
     *    and the one whose payload lands on an awkward bit count. */
    n265_grain_model m;
    memset(&m, 0, sizeof m);
    m.present = 1;
    m.log2_scale = 0;
    m.comp[0] = 1;
    m.nival[0] = 1;
    set_ival(&m, 0, 0, 0, 255, 40, 8);
    n265_write_fgc_sei(&bs, &m);
    parse_and_check(&bs, &m);
    size_t one_luma = bs.bytes;

    /* 2. Three components, several intervals each, a non-zero
     *    log2_scale_factor, and a scale that needs more than eight bits
     *    of se(v). */
    memset(&m, 0, sizeof m);
    m.present = 1;
    m.log2_scale = 5;
    for (int c = 0; c < 3; c++) {
        m.comp[c] = 1;
        m.nival[c] = 4;
        for (int i = 0; i < 4; i++)
            set_ival(&m, c, i, i * 64, i * 64 + 63,
                     100 + 300 * i + 7 * c, 2 + 3 * i + c);
    }
    n265_write_fgc_sei(&bs, &m);
    parse_and_check(&bs, &m);
    CHECK(bs.bytes > one_luma, "the three-component message is not larger "
          "than the one-interval one");

    /* 3. Sixteen intervals on luma alone: the widest model the estimator
     *    can produce, and the one that has to stay inside the fixed
     *    per-frame buffer it is copied into. */
    memset(&m, 0, sizeof m);
    m.present = 1;
    m.log2_scale = 3;
    for (int c = 0; c < 3; c++) {
        m.comp[c] = 1;
        m.nival[c] = N265_FGC_BINS;
        for (int i = 0; i < N265_FGC_BINS; i++)
            set_ival(&m, c, i, i * 256 / N265_FGC_BINS,
                     (i + 1) * 256 / N265_FGC_BINS - 1, 8 * i + 1, 9);
    }
    n265_write_fgc_sei(&bs, &m);
    parse_and_check(&bs, &m);
    CHECK(bs.bytes <= N265_FGC_SEI_MAX,
          "the widest message is %zu bytes, over the %d the frame work "
          "holds", bs.bytes, N265_FGC_SEI_MAX);

    /* 4. A chroma-only model: comp_model_present_flag 0 for luma has to
     *    leave luma's whole block out, not write an empty one. */
    memset(&m, 0, sizeof m);
    m.present = 1;
    m.log2_scale = 1;
    m.comp[1] = 1;
    m.nival[1] = 2;
    set_ival(&m, 1, 0, 0, 127, 12, 4);
    set_ival(&m, 1, 1, 128, 255, 20, 4);
    n265_write_fgc_sei(&bs, &m);
    parse_and_check(&bs, &m);

    bs_free(&bs);
    printf(fails ? "fgc: %d failure(s)\n" : "fgc: ok\n", fails);
    return fails ? 1 : 0;
}
