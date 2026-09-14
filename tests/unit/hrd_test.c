/*
 * Unit tests: the buffering_period (D.2.2) and pic_timing (D.2.3) SEI
 * writer, src/encoder/headers.c. Phase 12 item 5.
 * Copyright (c) 2026, the yah265 authors. GPL-2.0-or-later; see LICENSE.
 *
 * Two halves, and they fail on different bugs:
 *
 *  - a HAND-COMPUTED message. One value set, written out bit by bit from
 *    the syntax table on paper and pasted here as bytes. A writer that
 *    puts the right fields in the right order at the wrong WIDTH still
 *    parses; it does not survive a byte comparison.
 *  - a hand-written READER over the same syntax, which walks every field
 *    and then checks it ended exactly where the writer ended. A writer
 *    that drops a field, or writes one bit too many, passes a check that
 *    only compares values; it cannot pass this one.
 *
 * What a unit test cannot check is whether a decoder agrees, and for a
 * message no decoder needs in order to produce a picture that matters
 * more here than it did for the grain SEI: an HRD message is only ever
 * read by something checking the stream. docs/phase12.md item 5 names the
 * two decoders and the ffmpeg CBS trace the syntax was read back with.
 */

#include <stdio.h>
#include <string.h>

#include "common/bitstream.h"
#include "encoder/hrd.h"

void n265_write_hrd_sei(bitstream_t *bs, const n265_hrd_sei *v);

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

/* sei_message(): the ff-coded type and size, then the payload's first
 * byte. Returns the payload size and leaves the reader on it. */
static uint32_t sei_hdr(BR *r, uint32_t *type)
{
    uint32_t t = 0, s = 0, b;
    do { b = u(r, 8); t += b; } while (b == 255);
    do { b = u(r, 8); s += b; } while (b == 255);
    *type = t;
    return s;
}

/* Walk one written message and check it says what the values said. */
static void parse_and_check(const bitstream_t *bs, const n265_hrd_sei *v)
{
    BR r = { bs->buf, bs->bytes, 0, 0 };

    if (v->bp) {
        uint32_t type, size = sei_hdr(&r, &type);
        CHECK(type == 0, "payload type %u, want 0 (buffering period)", type);
        CHECK(size == 10, "buffering period payload %u bytes, want 10", size);
        size_t start = r.pos;
        CHECK(ue(&r) == 0, "bp_seq_parameter_set_id must be 0");
        CHECK(u(&r, 1) == 0, "irap_cpb_params_present_flag must be 0");
        CHECK(u(&r, 1) == (uint32_t)(v->concat != 0),
              "concatenation_flag must follow --hrd-concat");
        CHECK(u(&r, N265_HRD_DELAY_BITS) == 0,
              "au_cpb_removal_delay_delta_minus1 must be 0");
        uint32_t d = u(&r, N265_HRD_DELAY_BITS);
        uint32_t o = u(&r, N265_HRD_DELAY_BITS);
        CHECK(d == v->init_delay, "initial_cpb_removal_delay %u, want %u",
              d, v->init_delay);
        CHECK(o == v->init_offset, "initial_cpb_removal_offset %u, want %u",
              o, v->init_offset);
        /* D.2.1's payload alignment: a one, then zeros, and only because
         * 75 bits is not a whole number of bytes. */
        CHECK(u(&r, 1) == 1, "payload alignment must start with a one");
        while (r.pos % 8)
            CHECK(u(&r, 1) == 0, "payload alignment must pad with zeros");
        CHECK(r.pos - start == (size_t)size * 8,
              "buffering period read %zu bits, payload says %u",
              r.pos - start, size * 8);
    }

    uint32_t type, size = sei_hdr(&r, &type);
    CHECK(type == 1, "payload type %u, want 1 (pic timing)", type);
    /* --pic-struct adds pic_struct u(4), source_scan_type u(2) and
     * duplicate_flag u(1): seven bits, and the payload is byte-aligned
     * afterwards only because D.2.1's alignment pads it. */
    CHECK(size == (v->ff_info ? 7u : 6u),
          "pic timing payload %u bytes, want %u", size, v->ff_info ? 7u : 6u);
    size_t start = r.pos;
    if (v->ff_info) {
        uint32_t ps = u(&r, 4), scan = u(&r, 2), dup = u(&r, 1);
        CHECK(ps == (uint32_t)v->pic_struct, "pic_struct %u, want %d", ps,
              v->pic_struct);
        CHECK(scan == 1, "source_scan_type must be 1 (progressive), got %u",
              scan);
        CHECK(dup == 0, "duplicate_flag must be 0, got %u", dup);
    }
    uint32_t cpb = u(&r, N265_HRD_DELAY_BITS) + 1;
    uint32_t dpb = u(&r, N265_HRD_DELAY_BITS);
    CHECK(cpb == v->cpb_delay, "au_cpb_removal_delay %u, want %u",
          cpb, v->cpb_delay);
    CHECK(dpb == v->dpb_delay, "pic_dpb_output_delay %u, want %u",
          dpb, v->dpb_delay);
    /* 48 bits is already byte-aligned, so there is NO alignment field
     * here: writing one would be a spurious byte the size does not cover.
     * With --pic-struct it is 55, and then there IS one. */
    if (v->ff_info) {
        CHECK(u(&r, 1) == 1, "payload alignment must start with a one");
        while (r.pos % 8)
            CHECK(u(&r, 1) == 0, "payload alignment must pad with zeros");
    }
    CHECK(r.pos - start == (size_t)size * 8,
          "pic timing read %zu bits, payload says %u", r.pos - start,
          size * 8);

    /* rbsp_trailing_bits, and then the writer is done. */
    CHECK(u(&r, 1) == 1, "rbsp_stop_one_bit");
    while (r.pos % 8)
        CHECK(u(&r, 1) == 0, "rbsp_alignment_zero_bit");
    CHECK(!r.over, "the reader ran off the end of the message");
    CHECK(r.pos == bs->bytes * 8,
          "the reader ended at bit %zu, the writer at %zu",
          r.pos, bs->bytes * 8);
}

/* The hand-computed message. --vbv-maxrate 400 --vbv-bufsize 400
 * --vbv-init 0.9 at the first access unit of the stream:
 *
 *   BitRate = 400000, CpbSize = 400000 (E.2.3, exact at scale 0)
 *   initial_cpb_removal_delay  = 90000 * 0.9 * 400000 / 400000 = 81000
 *   initial_cpb_removal_offset = 90000 * 400000 / 400000 - 81000 = 9000
 *   au_cpb_removal_delay_minus1 = 0 (no preceding buffering period)
 *   pic_dpb_output_delay = sps_max_num_reorder_pics = 3 for an IDR
 *
 * written out field by field:
 *
 *   00        payload type 0, buffering period
 *   0a        payload size, 10 bytes
 *   1         bp_seq_parameter_set_id, ue(0)
 *   0 0       irap_cpb_params_present_flag, concatenation_flag
 *   000000    au_cpb_removal_delay_delta_minus1, 24 bits
 *   01 3c 68  81000, 24 bits
 *   00 23 28  9000, 24 bits
 *   1 0000    the payload's alignment: 75 bits is not a whole byte
 *   01        payload type 1, pic timing
 *   06        payload size, 6 bytes
 *   000000    au_cpb_removal_delay_minus1, 24 bits
 *   000003    pic_dpb_output_delay, 24 bits
 *   80        rbsp_trailing_bits
 */
static const uint8_t want_bp[] = {
    0x00, 0x0a,
    0x80, 0x00, 0x00, 0x00, 0x27, 0x8d, 0x00, 0x04, 0x65, 0x10,
    0x01, 0x06,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x03,
    0x80,
};

/* The same access unit one frame later: pic_timing alone. */
static const uint8_t want_pt[] = {
    0x01, 0x06,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x0a,
    0x80,
};

static void check_bytes(const bitstream_t *bs, const uint8_t *want,
                        size_t n, const char *what)
{
    CHECK(bs->bytes == n, "%s: wrote %zu bytes, hand-computed %zu",
          what, bs->bytes, n);
    if (bs->bytes != n)
        return;
    for (size_t i = 0; i < n; i++)
        CHECK(bs->buf[i] == want[i], "%s: byte %zu is %02x, want %02x",
              what, i, bs->buf[i], want[i]);
}

int main(void)
{
    bitstream_t bs;
    if (bs_init(&bs, 64) < 0) {
        printf("FAIL: bs_init\n");
        return 1;
    }

    /* 1. the hand-computed IRAP message, both ways */
    n265_hrd_sei v = { 1, 81000, 9000, 1, 3 };
    n265_write_hrd_sei(&bs, &v);
    check_bytes(&bs, want_bp, sizeof want_bp, "buffering period AU");
    parse_and_check(&bs, &v);

    /* 2. the access unit after it: no buffering period, one tick of
     * removal delay, and a picture coded before eight it is output
     * after (POC 8 at coding index 1, reorder 3: 3 + 8 - 1). */
    n265_hrd_sei p = { 0, 0, 0, 1, 10 };
    n265_write_hrd_sei(&bs, &p);
    check_bytes(&bs, want_pt, sizeof want_pt, "pic timing AU");
    parse_and_check(&bs, &p);

    /* 3. the message is the SAME SIZE whatever the values are. Every
     * field is fixed-width u(v), which is what lets the CPB walk that
     * produces the values run at the access unit they describe without
     * the size of the message it is about to add being unknown. */
    n265_hrd_sei big = { 1, N265_HRD_DELAY_MAX, N265_HRD_DELAY_MAX,
                         N265_HRD_DELAY_MAX, N265_HRD_DELAY_MAX };
    n265_write_hrd_sei(&bs, &big);
    CHECK(bs.bytes == sizeof want_bp,
          "an all-ones buffering period AU is %zu bytes, not %zu",
          bs.bytes, sizeof want_bp);
    parse_and_check(&bs, &big);
    n265_hrd_sei bigp = { 0, 0, 0, N265_HRD_DELAY_MAX, N265_HRD_DELAY_MAX };
    n265_write_hrd_sei(&bs, &bigp);
    CHECK(bs.bytes == sizeof want_pt,
          "an all-ones pic timing AU is %zu bytes, not %zu",
          bs.bytes, sizeof want_pt);
    parse_and_check(&bs, &bigp);

    /* 3b. phase 14 item 4's two additions, checked against the same
     * hand-written parser: the concatenation flag in the buffering
     * period, and the three frame/field fields in pic_timing. The second
     * changes the payload SIZE, which is what makes it the one addition
     * here that a decoder could mis-parse the rest of the message on. */
    n265_hrd_sei cc = { 1, 81000, 9000, 1, 3, 1, 0, 0 };
    n265_write_hrd_sei(&bs, &cc);
    parse_and_check(&bs, &cc);
    n265_hrd_sei ps = { 0, 0, 0, 1, 10, 0, 1, 7 };
    n265_write_hrd_sei(&bs, &ps);
    parse_and_check(&bs, &ps);
    CHECK(bs.bytes == sizeof want_pt + 1,
          "a pic timing AU with the frame/field fields is %zu bytes, not %zu",
          bs.bytes, sizeof want_pt + 1);
    /* ...and a ZEROED struct writes neither: the presence of the three
     * fields is its own flag and not a sentinel value of pic_struct, so a
     * caller that memsets gets the message it had before this item. */
    n265_hrd_sei zero;
    memset(&zero, 0, sizeof zero);
    zero.cpb_delay = 1;
    zero.dpb_delay = 10;
    n265_write_hrd_sei(&bs, &zero);
    check_bytes(&bs, want_pt, sizeof want_pt, "a zeroed struct is the old message");

    /* 4. a delay of one is the smallest the field can carry: the coded
     * value is minus1, so 1 is coded as 0 and 0 would be unrepresentable
     * - which is why hrd_sei_values floors the count at one tick. */
    n265_hrd_sei one = { 0, 0, 0, 1, 0 };
    n265_write_hrd_sei(&bs, &one);
    parse_and_check(&bs, &one);

    bs_free(&bs);
    printf(fails ? "hrd_test: %d FAILURES\n" : "hrd_test: OK (%d failures)\n",
           fails);
    return fails ? 1 : 0;
}
