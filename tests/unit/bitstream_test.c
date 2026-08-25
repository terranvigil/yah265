/*
 * Unit tests: bit writer, Exp-Golomb, emulation prevention.
 * Copyright (c) 2026, the next265 authors. GPL-2.0-or-later; see LICENSE.
 */

#include <stdio.h>
#include <string.h>

#include "common/bitstream.h"

static int fails;

#define CHECK(cond, ...) do { \
    if (!(cond)) { fails++; printf("FAIL %s:%d: ", __FILE__, __LINE__); \
                   printf(__VA_ARGS__); printf("\n"); } \
} while (0)

static void test_bits(void)
{
    bitstream_t bs;
    bs_init(&bs, 16);
    bs_put_bits(&bs, 0xA, 4);
    bs_put_bits(&bs, 0x5, 4);
    CHECK(bs.bytes == 1 && bs.buf[0] == 0xA5, "msb-first packing");

    bs_reset(&bs);
    bs_put_bits(&bs, 1, 1);
    bs_align_zero(&bs);
    CHECK(bs.bytes == 1 && bs.buf[0] == 0x80, "align zero");

    bs_reset(&bs);
    bs_put_bits(&bs, 0, 3);
    bs_rbsp_trailing(&bs);
    CHECK(bs.bytes == 1 && bs.buf[0] == 0x10, "rbsp trailing");
    bs_free(&bs);
}

static void test_ue(void)
{
    /* golden pairs from working the 9.2 tables by hand:
     * 0 -> 1, 1 -> 010, 2 -> 011, 3 -> 00100, 8 -> 0001001 */
    struct { uint32_t v; uint8_t byte; int bits; } cases[] = {
        { 0, 0x80, 1 }, { 1, 0x40, 3 }, { 2, 0x60, 3 },
        { 3, 0x20, 5 }, { 8, 0x12, 7 },
    };
    for (unsigned i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        bitstream_t bs;
        bs_init(&bs, 16);
        bs_put_ue(&bs, cases[i].v);
        CHECK(bs.acc_bits == cases[i].bits % 8, "ue(%u) bit count", cases[i].v);
        bs_align_zero(&bs);
        CHECK(bs.buf[0] == cases[i].byte, "ue(%u) = 0x%02x got 0x%02x",
              cases[i].v, cases[i].byte, bs.buf[0]);
        bs_free(&bs);
    }

    /* se(v): 1 -> ue(1), -1 -> ue(2), 2 -> ue(3), -2 -> ue(4) */
    bitstream_t a, b;
    bs_init(&a, 16); bs_init(&b, 16);
    bs_put_se(&a, -1); bs_put_ue(&b, 2);
    bs_align_zero(&a); bs_align_zero(&b);
    CHECK(a.bytes == b.bytes && !memcmp(a.buf, b.buf, a.bytes), "se(-1)=ue(2)");
    bs_free(&a); bs_free(&b);
}

static void test_ep(void)
{
    /* 00 00 00 -> 00 00 03 00 ; 00 00 01 -> 00 00 03 01 ;
     * 00 00 04 stays */
    uint8_t out[64];
    const uint8_t in1[] = { 0x00, 0x00, 0x00 };
    size_t n = nal_pack(out, sizeof(out), 33, in1, sizeof(in1));
    const uint8_t want1[] = { 0,0,0,1, 0x42,0x01, 0x00,0x00,0x03,0x00 };
    CHECK(n == sizeof(want1) && !memcmp(out, want1, n), "ep 000000");

    const uint8_t in2[] = { 0x00, 0x00, 0x04, 0x00, 0x00, 0x02 };
    n = nal_pack(out, sizeof(out), 34, in2, sizeof(in2));
    const uint8_t want2[] = { 0,0,0,1, 0x44,0x01,
                              0x00,0x00,0x04,0x00,0x00,0x03,0x02 };
    CHECK(n == sizeof(want2) && !memcmp(out, want2, n), "ep mixed");

    /* four zeros: EP after the first pair, zero count restarts */
    const uint8_t in3[] = { 0x00, 0x00, 0x00, 0x00, 0x01 };
    n = nal_pack(out, sizeof(out), 32, in3, sizeof(in3));
    const uint8_t want3[] = { 0,0,0,1, 0x40,0x01,
                              0x00,0x00,0x03,0x00,0x00,0x03,0x01 };
    CHECK(n == sizeof(want3) && !memcmp(out, want3, n), "ep 4 zeros");
}

int main(void)
{
    test_bits();
    test_ue();
    test_ep();
    if (fails) {
        printf("%d failure(s)\n", fails);
        return 1;
    }
    printf("all bitstream tests passed\n");
    return 0;
}
