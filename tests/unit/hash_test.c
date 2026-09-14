/*
 * Unit tests: the decoded picture hash (D.3.19) and the signalling SEI
 * writers, src/encoder/hash.c and src/encoder/headers.c. Phase 14 item 1.
 * Copyright (c) 2026, the yah265 authors. GPL-2.0-or-later; see LICENSE.
 *
 * Four halves, and they fail on different bugs:
 *
 *  - RFC 1321's own published test vectors, against the MD5 primitive.
 *    An MD5 that is wrong everywhere still produces sixteen plausible
 *    bytes and still agrees with itself; only a published vector says
 *    it is MD5 and not something else.
 *  - a KNOWN PLANE, hashed all three ways. The expected MD5 came out of
 *    openssl(1) over the same bytes, and the CRC and the checksum out of
 *    scripts/hash_check.py, which is itself checked against x265's own
 *    streams (docs/phase14.md). So none of the three expected values was
 *    produced by the code under test.
 *  - the checksum's COORDINATE MASK, which is the only part of D.3.19
 *    that a plain sum would not have: two samples exchanged between
 *    positions leave the sum alone and must move the checksum.
 *  - the three messages' SYNTAX, byte for byte against a hand-assembled
 *    reference and then through a hand-written reader over the same
 *    syntax table. A writer that puts the right fields in the wrong
 *    ORDER, or at the wrong WIDTH, still parses as something; it does
 *    not survive a byte comparison.
 *
 * What a unit test cannot check is whether a decoder agrees. For this
 * message that matters more than usual, because the hash is a claim
 * ABOUT the decoder: docs/phase14.md names ffmpeg's own MD5 verification
 * (live, with the mismatch reproduced on purpose) and the three-way
 * agreement with x265 through scripts/hash_check.py.
 */

#include <stdio.h>
#include <string.h>

#include "common/bitstream.h"
#include "encoder/hash.h"
#include "encoder/signal.h"

void n265_write_hash_sei(bitstream_t *bs, const n265_pic_hash *h, int ncomp);
size_t n265_write_hdr_sei(bitstream_t *bs, const n265_signal *s);

static int fails;

#define CHECK(cond, ...) do { \
    if (!(cond)) { fails++; printf("FAIL %s:%d: ", __FILE__, __LINE__); \
                   printf(__VA_ARGS__); printf("\n"); } \
} while (0)

static const char *hex16(const uint8_t *b)
{
    static char s[33];
    for (int i = 0; i < 16; i++)
        snprintf(s + 2 * i, 3, "%02x", b[i]);
    return s;
}

/* RFC 1321, section A.5: the suite the algorithm is published with. */
static void test_md5_vectors(void)
{
    static const struct { const char *in, *out; } v[] = {
        { "", "d41d8cd98f00b204e9800998ecf8427e" },
        { "a", "0cc175b9c0f1b6a831c399e269772661" },
        { "abc", "900150983cd24fb0d6963f7d28e17f72" },
        { "message digest", "f96b697d7cb7938d525a2f31aaf161d0" },
        { "abcdefghijklmnopqrstuvwxyz", "c3fcd3d76192e4007dfb496cca67e13b" },
        { "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789",
          "d174ab98d277d9f5a5611c2c9f419d9f" },
        { "123456789012345678901234567890123456789012345678901234567890"
          "12345678901234567890", "57edf4a22be3c955ac49da2e2107b67a" },
    };
    for (size_t i = 0; i < sizeof v / sizeof v[0]; i++) {
        uint8_t md[16];
        n265_md5((const uint8_t *)v[i].in, strlen(v[i].in), md);
        CHECK(!strcmp(hex16(md), v[i].out),
              "RFC 1321 vector %zu: %s, want %s", i, hex16(md), v[i].out);
    }
    /* the block boundary itself: 55, 56 and 64 bytes are the three cases
     * the padding rule branches on, and a length field written at the
     * wrong end is invisible below 56. */
    static const struct { int n; const char *out; } len[] = {
        { 55, "ef1772b6dff9a122358552954ad0df65" },
        { 56, "3b0c8ac703f828b04c6c197006d17218" },
        { 64, "014842d480b571495a4a0363793f7367" },
    };
    for (size_t i = 0; i < sizeof len / sizeof len[0]; i++) {
        uint8_t buf[64], md[16];
        memset(buf, 'a', sizeof buf);
        n265_md5(buf, (size_t)len[i].n, md);
        CHECK(!strcmp(hex16(md), len[i].out),
              "md5 of %d 'a's: %s, want %s", len[i].n, hex16(md),
              len[i].out);
    }
}

/* The known plane: 8x8, sample (x, y) = (17x + 5y) & 0xff. Every value
 * fits in eight bits at BOTH depths, so the SAMPLES are the same picture
 * and only D.3.19's byte serialisation differs - which is exactly the
 * thing the second expected set exists to check. */
#define PW 8
#define PH 8

static void fill_plane(pixel *p)
{
    for (int y = 0; y < PH; y++)
        for (int x = 0; x < PW; x++)
            p[y * PW + x] = (pixel)((x * 17 + y * 5) & 0xff);
}

static void test_known_plane(void)
{
    pixel p[PW * PH];
    fill_plane(p);

#if N265_BIT_DEPTH == 8
    const char *want_md5 = "d6b228a0eafe78a4295f4b5acb1cedbb";
    uint16_t want_crc = 0xbdad;
    uint32_t want_chk = 0x000012e0;
#else
    const char *want_md5 = "107c4d23a1e304bed59b00a4cd29f897";
    uint16_t want_crc = 0x2bb9;
    uint32_t want_chk = 0x000013c0;
#endif

    n265_pic_hash h;
    memset(&h, 0, sizeof h);
    h.type = N265_HASH_MD5;
    n265_hash_plane(&h, 0, p, PW, PW, PH);
    CHECK(!strcmp(hex16(h.md5[0]), want_md5), "plane md5 %s, want %s",
          hex16(h.md5[0]), want_md5);

    h.type = N265_HASH_CRC;
    n265_hash_plane(&h, 1, p, PW, PW, PH);
    CHECK(h.crc[1] == want_crc, "plane crc %04x, want %04x", h.crc[1],
          want_crc);

    h.type = N265_HASH_CHECKSUM;
    n265_hash_plane(&h, 2, p, PW, PW, PH);
    CHECK(h.checksum[2] == want_chk, "plane checksum %08x, want %08x",
          h.checksum[2], want_chk);

    /* a stride wider than the plane must change nothing: the walk reads
     * w samples of each row and no more */
    pixel wide[PW * 2 * PH];
    memset(wide, 0xa5, sizeof wide);
    for (int y = 0; y < PH; y++)
        memcpy(wide + y * PW * 2, p + y * PW, PW * sizeof(pixel));
    n265_pic_hash h2;
    memset(&h2, 0, sizeof h2);
    h2.type = N265_HASH_MD5;
    n265_hash_plane(&h2, 0, wide, PW * 2, PW, PH);
    CHECK(!strcmp(hex16(h2.md5[0]), want_md5),
          "strided plane md5 %s, want %s", hex16(h2.md5[0]), want_md5);
}

/* D.3.19's checksum XORs each byte with a mask built from the sample's
 * own coordinates. Exchange two samples and the SUM is unchanged, so a
 * checksum that ignored the mask would not move. */
static void test_checksum_is_positional(void)
{
    pixel a[PW * PH], b[PW * PH];
    fill_plane(a);
    memcpy(b, a, sizeof b);
    pixel t = b[0];
    b[0] = b[1];
    b[1] = t;

    n265_pic_hash ha, hb;
    memset(&ha, 0, sizeof ha);
    memset(&hb, 0, sizeof hb);
    ha.type = hb.type = N265_HASH_CHECKSUM;
    n265_hash_plane(&ha, 0, a, PW, PW, PH);
    n265_hash_plane(&hb, 0, b, PW, PW, PH);
    CHECK(ha.checksum[0] != hb.checksum[0],
          "two exchanged samples left the checksum at %08x",
          ha.checksum[0]);
#if N265_BIT_DEPTH == 8
    CHECK(hb.checksum[0] == 0x000012e2, "exchanged checksum %08x, want "
          "000012e2", hb.checksum[0]);
#endif
}

/* ------------------------------------------------------ the SEI syntax */

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

/* sei_message(): the ff-coded type and size. */
static uint32_t sei_hdr(BR *r, uint32_t *type)
{
    uint32_t t = 0, s = 0, b;
    do { b = u(r, 8); t += b; } while (b == 255);
    do { b = u(r, 8); s += b; } while (b == 255);
    *type = t;
    return s;
}

static void test_hash_sei_syntax(void)
{
    n265_pic_hash h;
    memset(&h, 0, sizeof h);
    h.type = N265_HASH_CRC;
    h.crc[0] = 0x1234;
    h.crc[1] = 0xabcd;
    h.crc[2] = 0x0001;

    bitstream_t bs;
    bs_init(&bs, 64);
    n265_write_hash_sei(&bs, &h, 3);

    /* Hand-assembled: payload type 132, payload size 7 (the type byte and
     * three u(16)s), then the payload, then the RBSP stop bit. */
    static const uint8_t want[] = {
        132, 7, 0x01, 0x12, 0x34, 0xab, 0xcd, 0x00, 0x01, 0x80
    };
    CHECK(bs.bytes == sizeof want, "hash SEI is %zu bytes, want %zu",
          bs.bytes, sizeof want);
    if (bs.bytes == sizeof want)
        CHECK(!memcmp(bs.buf, want, sizeof want),
              "hash SEI bytes differ from the hand-assembled message");

    /* ...and the MD5 form, read back through the syntax table */
    memset(&h, 0, sizeof h);
    h.type = N265_HASH_MD5;
    for (int c = 0; c < 3; c++)
        for (int i = 0; i < 16; i++)
            h.md5[c][i] = (uint8_t)(c * 16 + i);
    n265_write_hash_sei(&bs, &h, 3);

    BR r = { bs.buf, bs.bytes, 0, 0 };
    uint32_t type, size = sei_hdr(&r, &type);
    CHECK(type == 132, "payload type %u, want 132", type);
    CHECK(size == 49, "payload size %u, want 49 (1 + 3 * 16)", size);
    CHECK(u(&r, 8) == 0, "hash_type, want 0 (MD5)");
    for (int c = 0; c < 3; c++)
        for (int i = 0; i < 16; i++) {
            uint32_t got = u(&r, 8);
            CHECK(got == (uint32_t)(c * 16 + i),
                  "picture_md5[%d][%d] = %u", c, i, got);
        }
    CHECK(!r.over, "the reader ran off the end of the message");
    CHECK(r.pos == (size + 2) * 8, "the message ended at bit %zu, the "
          "syntax at %u", r.pos, (size + 2) * 8);
    bs_free(&bs);
}

static void test_hdr_sei_syntax(void)
{
    n265_signal s;
    memset(&s, 0, sizeof s);
    s.hdr10 = -1;
    s.cll_on = 1;
    s.atc = -1;
    s.have_md = 1;
    /* P3D65 at 1000 nits, x265's own P3D65x1000n0005 values */
    const uint16_t prim[3][2] = { { 13250, 34500 }, { 7500, 3000 },
                                  { 34000, 16000 } };
    memcpy(s.md_prim, prim, sizeof prim);
    s.md_wp[0] = 15635;
    s.md_wp[1] = 16450;
    s.md_max = 10000000;
    s.md_min = 5;
    s.have_cll = 1;
    s.maxcll = 1000;
    s.maxfall = 400;

    bitstream_t bs;
    bs_init(&bs, 128);
    n265_write_hdr_sei(&bs, &s);

    /* Hand-assembled, in the order the writer emits them: type 137 with
     * 24 payload bytes (eight u(16) and two u(32)), then type 144 with 4,
     * then the stop bit. Every number below is the value above written
     * big-endian, which is what u(n) means. */
    static const uint8_t want[] = {
        137, 24,
        0x33, 0xc2, 0x86, 0xc4,     /* G x, y */
        0x1d, 0x4c, 0x0b, 0xb8,     /* B x, y */
        0x84, 0xd0, 0x3e, 0x80,     /* R x, y */
        0x3d, 0x13, 0x40, 0x42,     /* white point x, y */
        0x00, 0x98, 0x96, 0x80,     /* max luminance, 1000 cd/m2 */
        0x00, 0x00, 0x00, 0x05,     /* min luminance, 0.0005 cd/m2 */
        144, 4,
        0x03, 0xe8, 0x01, 0x90,     /* max CLL 1000, max FALL 400 */
        0x80
    };
    CHECK(bs.bytes == sizeof want, "HDR SEI is %zu bytes, want %zu",
          bs.bytes, sizeof want);
    if (bs.bytes == sizeof want)
        CHECK(!memcmp(bs.buf, want, sizeof want),
              "HDR SEI bytes differ from the hand-assembled messages");

    /* --no-cll drops the second message and keeps the first */
    s.cll_on = 0;
    n265_write_hdr_sei(&bs, &s);
    CHECK(bs.bytes == 27, "with --no-cll the NAL is %zu bytes, want 27",
          bs.bytes);
    CHECK(bs.bytes > 1 && bs.buf[0] == 137, "the mastering display is gone");

    /* --no-hdr10 drops both, and --atc-sei is NOT part of that packet */
    s.cll_on = 1;
    s.hdr10 = 0;
    s.atc = 18;                 /* arib-std-b67, HLG */
    n265_write_hdr_sei(&bs, &s);
    static const uint8_t want_atc[] = { 147, 1, 18, 0x80 };
    CHECK(bs.bytes == sizeof want_atc &&
          !memcmp(bs.buf, want_atc, sizeof want_atc),
          "with --no-hdr10 and --atc-sei 18 the NAL is %zu bytes",
          bs.bytes);

    /* --max-cll with --no-cll and nothing else asks for NO message, and
     * an empty RBSP is still a NAL whose one trailing byte reads as an
     * SEI of payload type 128. The writer says 0 and the caller writes
     * nothing. */
    n265_signal none;
    memset(&none, 0, sizeof none);
    none.hdr10 = -1;
    none.cll_on = 0;
    none.atc = -1;
    none.have_cll = 1;
    none.maxcll = 1;
    CHECK(n265_write_hdr_sei(&bs, &none) == 0,
          "an empty HDR configuration wrote %zu bytes", bs.bytes);

    /* the auto rule: values alone arm the packet, and nothing at all
     * writes nothing at all */
    n265_signal empty;
    memset(&empty, 0, sizeof empty);
    empty.hdr10 = -1;
    empty.cll_on = 1;
    empty.atc = -1;
    CHECK(!n265_hdr10_on(&empty), "--hdr10 armed with no values");
    empty.have_cll = 1;
    CHECK(n265_hdr10_on(&empty), "--max-cll did not arm the packet");
    empty.hdr10 = 0;
    CHECK(!n265_hdr10_on(&empty), "--no-hdr10 did not disarm the packet");
    bs_free(&bs);
}

/* The value tables: index IS the code, and a dropped "reserved" entry
 * would shift everything after it. These are the codes of Tables E.2 to
 * E.5 read off the spec, not off the table under test. */
static void test_value_tables(void)
{
    static const struct { int field; const char *name; int code; } v[] = {
        { N265_VUI_COLORPRIM,   "bt709",        1 },
        { N265_VUI_COLORPRIM,   "unknown",      2 },
        { N265_VUI_COLORPRIM,   "bt2020",       9 },
        { N265_VUI_COLORPRIM,   "smpte432",    12 },
        { N265_VUI_TRANSFER,    "bt709",        1 },
        { N265_VUI_TRANSFER,    "bt2020-10",   14 },
        { N265_VUI_TRANSFER,    "smpte2084",   16 },
        { N265_VUI_TRANSFER,    "arib-std-b67",18 },
        { N265_VUI_COLORMATRIX, "gbr",          0 },
        { N265_VUI_COLORMATRIX, "bt709",        1 },
        { N265_VUI_COLORMATRIX, "bt2020nc",     9 },
        { N265_VUI_COLORMATRIX, "ictcp",       14 },
        { N265_VUI_VIDEOFORMAT, "component",    0 },
        { N265_VUI_VIDEOFORMAT, "pal",          1 },
        { N265_VUI_VIDEOFORMAT, "unknown",      5 },
        { N265_VUI_RANGE,       "limited",      0 },
        { N265_VUI_RANGE,       "full",         1 },
        { N265_VUI_OVERSCAN,    "show",         0 },
        { N265_VUI_OVERSCAN,    "crop",         1 },
        { N265_VUI_OVERSCAN,    "unknown",      2 },
    };
    for (size_t i = 0; i < sizeof v / sizeof v[0]; i++) {
        int got = n265_vui_parse(v[i].field, v[i].name);
        CHECK(got == v[i].code, "%s parses to %d, want %d", v[i].name, got,
              v[i].code);
    }
    /* the numeric form x265 also takes, and its bounds */
    CHECK(n265_vui_parse(N265_VUI_TRANSFER, "18") == 18, "numeric transfer");
    CHECK(n265_vui_parse(N265_VUI_TRANSFER, "255") == 255, "u(8) bound");
    CHECK(n265_vui_parse(N265_VUI_TRANSFER, "256") == -1, "u(8) overflow");
    CHECK(n265_vui_parse(N265_VUI_VIDEOFORMAT, "8") == -1, "u(3) overflow");
    CHECK(n265_vui_parse(N265_VUI_COLORPRIM, "bt709x") == -1,
          "a name with a tail");

    /* the compound syntaxes */
    int idc = -1, w = 0, h = 0;
    CHECK(n265_sar_parse("16:9", &idc, &w, &h) == 0 && idc == 255 &&
          w == 16 && h == 9, "--sar 16:9 -> EXTENDED_SAR 16 9");
    CHECK(n265_sar_parse("14", &idc, &w, &h) == 0 && idc == 14,
          "--sar 14 -> the Table E.1 index");
    CHECK(n265_sar_parse("17", &idc, &w, &h) == -1, "--sar 17 is not a row");
    CHECK(n265_sar_parse("16:", &idc, &w, &h) == -1, "--sar 16: is not a pair");

    int l, t, r, b;
    CHECK(n265_window_parse("4,2,6,8", &l, &t, &r, &b) == 0 && l == 4 &&
          t == 2 && r == 6 && b == 8, "--display-window l,t,r,b");
    CHECK(n265_window_parse("4,2,6", &l, &t, &r, &b) == -1, "three of four");

    uint32_t md[10];
    CHECK(n265_master_display_parse(
              "G(13250,34500)B(7500,3000)R(34000,16000)"
              "WP(15635,16450)L(10000000,5)", md) == 0 &&
          md[0] == 13250 && md[5] == 16000 && md[8] == 10000000 &&
          md[9] == 5, "--master-display in x265's syntax");
    CHECK(n265_master_display_parse("G(1,2)B(3,4)R(5,6)WP(7,8)", md) == -1,
          "a mastering display with no luminance");
    CHECK(n265_master_display_parse(
              "G(70000,1)B(3,4)R(5,6)WP(7,8)L(9,10)", md) == -1,
          "a chromaticity past u(16)");

    int cll, fall;
    CHECK(n265_cll_parse("1000,400", &cll, &fall) == 0 && cll == 1000 &&
          fall == 400, "--max-cll cll,fall");
    CHECK(n265_cll_parse("1000", &cll, &fall) == -1, "--max-cll needs both");
}

int main(void)
{
    test_md5_vectors();
    test_known_plane();
    test_checksum_is_positional();
    test_hash_sei_syntax();
    test_hdr_sei_syntax();
    test_value_tables();
    if (fails)
        printf("hash_test: %d failure(s)\n", fails);
    else
        printf("hash_test: ok\n");
    return fails != 0;
}
