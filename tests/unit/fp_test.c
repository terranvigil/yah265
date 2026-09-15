/*
 * Unit tests: the stream fingerprint's signature and its site keying
 * (src/encoder/fp.c, phase 14 item 10).
 * Copyright (c) 2026, the yah265 authors. GPL-2.0-or-later; see LICENSE.
 *
 * Two halves, and they fail for different reasons.
 *
 * The signature is checked against the identification SEI's UUID spelled
 * out here, byte by byte, rather than against the encoder's own constant:
 * the whole point of the scheme is that the two marks name one encoder,
 * so a UUID that moves in headers.c and not in fp.c has to fail
 * somewhere, and this is the somewhere.
 *
 * The site keying is checked as a ROUND TRIP against the same arithmetic
 * the verifier does in Python (scripts/fingerprint_check.py), written
 * out here a second time from the description rather than called: a
 * reader and a writer that share one function agree by construction and
 * prove nothing. What a decoder actually reads back is the conformance
 * gate's business and the verifier's.
 */

#include <stdio.h>
#include <stdint.h>

#include "encoder/fp.h"
#include "yah265.h"

static int fails;

#define CHECK(cond, ...) do { \
    if (!(cond)) { fails++; printf("FAIL %s:%d: ", __FILE__, __LINE__); \
                   printf(__VA_ARGS__); printf("\n"); } \
} while (0)

/* n265_write_info_sei's UUID (src/encoder/headers.c), copied here on
 * purpose. */
static const uint8_t uuid[16] = {
    0x7a, 0xa4, 0x16, 0x3d, 0xc8, 0x5e, 0x4f, 0x71,
    0x9d, 0x2c, 0x6b, 0x08, 0xe3, 0x51, 0xa7, 0x94,
};

/* scripts/fingerprint_check.py's fp_position, in C. */
static unsigned ref_pos(int poc_lsb, int x0, int y0)
{
    uint32_t z = (uint32_t)(poc_lsb & 255) * 0x9e3779b1u;
    z ^= (uint32_t)(x0 & 0xffff) * 0x85ebca6bu;
    z ^= (uint32_t)(y0 & 0xffff) * 0xc2b2ae35u;
    z ^= z >> 15;
    z *= 0x2545f491u;
    z ^= z >> 13;
    return z & (N265_FP_BITS - 1);
}

int main(void)
{
    uint32_t sig = n265_fp_sig();

    /* the match key IS the SEI's first three UUID bytes */
    CHECK((sig & 0xff) == uuid[0], "sig byte 0 %02x != %02x",
          sig & 0xff, uuid[0]);
    CHECK(((sig >> 8) & 0xff) == uuid[1], "sig byte 1 %02x != %02x",
          (sig >> 8) & 0xff, uuid[1]);
    CHECK(((sig >> 16) & 0xff) == uuid[2], "sig byte 2 %02x != %02x",
          (sig >> 16) & 0xff, uuid[2]);

    /* the version rides above it, packed 3:3:2 */
    uint32_t want = (uint32_t)(((YAH265_VERSION_MAJOR & 7) << 5) |
                               ((YAH265_VERSION_MINOR & 7) << 2) |
                               (YAH265_VERSION_PATCH & 3));
    CHECK(((sig >> 24) & 0xff) == want, "version byte %02x != %02x",
          (sig >> 24) & 0xff, want);

    /* A reader that can only see the 1 bits needs the key half to HAVE
     * some: a signature whose low 24 bits were all zero would be
     * unreadable by construction, and one that was all ones would not
     * discriminate. Both ends are asserted so a future UUID cannot break
     * the scheme silently. */
    int ones = 0;
    for (int i = 0; i < 24; i++)
        ones += (int)((sig >> i) & 1);
    CHECK(ones >= 6, "only %d one bits in the match key", ones);
    CHECK(ones <= 18, "%d one bits in the match key: too few zeros to "
                      "tell a fingerprint from an accident", ones);

    /* the keying: every site's bit is the signature bit its position
     * selects, and the selection is the verifier's arithmetic */
    int checked = 0;
    for (int poc = 0; poc < 256; poc += 7)
        for (int y = 0; y < 1088; y += 16)
            for (int x = 0; x < 1920; x += 16) {
                unsigned p = ref_pos(poc, x, y);
                int want_bit = (int)((sig >> p) & 1);
                int got = n265_fp_bit(poc, x, y);
                if (got != want_bit) {
                    CHECK(0, "poc %d (%d,%d): bit %d != %d (position %u)",
                          poc, x, y, got, want_bit, p);
                    return 1;   /* one report is enough */
                }
                checked++;
            }
    CHECK(checked > 100000, "only %d positions checked", checked);

    /* and the positions must actually spread: a keying that sent every
     * site of a picture to one bit would read as a fingerprint on one
     * position and nothing else */
    int hit[N265_FP_BITS] = { 0 };
    for (int y = 0; y < 288; y += 8)
        for (int x = 0; x < 352; x += 8)
            hit[ref_pos(3, x, y)]++;
    int covered = 0;
    for (int i = 0; i < N265_FP_BITS; i++)
        covered += hit[i] != 0;
    CHECK(covered == N265_FP_BITS,
          "one CIF picture's sites cover only %d of %d positions",
          covered, N265_FP_BITS);

    printf("%s: fp\n", fails ? "FAIL" : "PASS");
    return fails ? 1 : 0;
}
