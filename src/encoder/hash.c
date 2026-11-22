/*
 * The decoded picture hash (D.3.19): MD5, CRC and checksum.
 * Copyright (c) 2026, the yah265 authors. GPL-2.0-or-later; see LICENSE.
 *
 * Three hashes of one thing. D.3.19 defines them all over the same array
 * of bytes - the decoded picture, row by row, one byte per sample at 8
 * bits and the low byte before the high one above it - so the plane walk
 * below is written once and the three accumulators differ only in what
 * they do with a byte.
 *
 * The CRC and the checksum are the spec's own arithmetic and not a
 * standard primitive. The CRC is bitwise, MSB first, over that same byte
 * sequence, with the usual sixteen zero bits shifted in at the end; the
 * checksum adds each byte XORed with a mask built from the sample's
 * coordinates, which is what makes it catch a picture whose samples are
 * right but in the wrong place. Neither is an implementation detail a
 * decoder may differ on: the hash is only worth anything if the two sides
 * agree bit for bit, which is what tests/unit/hash_test.c and
 * scripts/hash_check.py (against x265's own streams) are for.
 *
 * MD5 is RFC 1321's, written here rather than taken from a library
 * because this project links none and because one compact implementation
 * of a thirty-year-old algorithm is cheaper than a dependency. Its own
 * correctness is checked against RFC 1321's published test vectors in the
 * unit test, which is a claim no encode could make.
 */

#include <string.h>

#include "encoder/hash.h"

/* ---------------------------------------------------------------- MD5 */

/* K[i] = floor(2^32 * |sin(i + 1)|), i in radians: RFC 1321's table,
 * written out rather than computed so the library needs no sin() and the
 * constants cannot move with a libm. */
static const uint32_t md5_k[64] = {
    0xd76aa478u, 0xe8c7b756u, 0x242070dbu, 0xc1bdceeeu,
    0xf57c0fafu, 0x4787c62au, 0xa8304613u, 0xfd469501u,
    0x698098d8u, 0x8b44f7afu, 0xffff5bb1u, 0x895cd7beu,
    0x6b901122u, 0xfd987193u, 0xa679438eu, 0x49b40821u,
    0xf61e2562u, 0xc040b340u, 0x265e5a51u, 0xe9b6c7aau,
    0xd62f105du, 0x02441453u, 0xd8a1e681u, 0xe7d3fbc8u,
    0x21e1cde6u, 0xc33707d6u, 0xf4d50d87u, 0x455a14edu,
    0xa9e3e905u, 0xfcefa3f8u, 0x676f02d9u, 0x8d2a4c8au,
    0xfffa3942u, 0x8771f681u, 0x6d9d6122u, 0xfde5380cu,
    0xa4beea44u, 0x4bdecfa9u, 0xf6bb4b60u, 0xbebfbc70u,
    0x289b7ec6u, 0xeaa127fau, 0xd4ef3085u, 0x04881d05u,
    0xd9d4d039u, 0xe6db99e5u, 0x1fa27cf8u, 0xc4ac5665u,
    0xf4292244u, 0x432aff97u, 0xab9423a7u, 0xfc93a039u,
    0x655b59c3u, 0x8f0ccc92u, 0xffeff47du, 0x85845dd1u,
    0x6fa87e4fu, 0xfe2ce6e0u, 0xa3014314u, 0x4e0811a1u,
    0xf7537e82u, 0xbd3af235u, 0x2ad7d2bbu, 0xeb86d391u,
};

/* the per-round rotation amounts, four values repeated per round */
static const uint8_t md5_s[64] = {
    7, 12, 17, 22,  7, 12, 17, 22,  7, 12, 17, 22,  7, 12, 17, 22,
    5,  9, 14, 20,  5,  9, 14, 20,  5,  9, 14, 20,  5,  9, 14, 20,
    4, 11, 16, 23,  4, 11, 16, 23,  4, 11, 16, 23,  4, 11, 16, 23,
    6, 10, 15, 21,  6, 10, 15, 21,  6, 10, 15, 21,  6, 10, 15, 21,
};

typedef struct md5_ctx {
    uint32_t s[4];
    uint64_t len;           /* message length in BYTES, for the padding */
    uint8_t block[64];
    size_t have;            /* bytes buffered in `block` */
} md5_ctx;

static uint32_t rotl32(uint32_t v, int n)
{
    return (v << n) | (v >> (32 - n));
}

static void md5_block(md5_ctx *c, const uint8_t *p)
{
    uint32_t m[16];
    for (int i = 0; i < 16; i++)   /* the message block is LITTLE-endian */
        m[i] = (uint32_t)p[4 * i] | ((uint32_t)p[4 * i + 1] << 8) |
               ((uint32_t)p[4 * i + 2] << 16) | ((uint32_t)p[4 * i + 3] << 24);
    uint32_t a = c->s[0], b = c->s[1], d = c->s[2], e = c->s[3];
    for (int i = 0; i < 64; i++) {
        uint32_t f;
        int g;
        if (i < 16) {
            f = (b & d) | (~b & e);
            g = i;
        } else if (i < 32) {
            f = (e & b) | (~e & d);
            g = (5 * i + 1) & 15;
        } else if (i < 48) {
            f = b ^ d ^ e;
            g = (3 * i + 5) & 15;
        } else {
            f = d ^ (b | ~e);
            g = (7 * i) & 15;
        }
        uint32_t t = e;
        e = d;
        d = b;
        b = b + rotl32(a + f + md5_k[i] + m[g], md5_s[i]);
        a = t;
    }
    c->s[0] += a;
    c->s[1] += b;
    c->s[2] += d;
    c->s[3] += e;
}

static void md5_init(md5_ctx *c)
{
    c->s[0] = 0x67452301u;
    c->s[1] = 0xefcdab89u;
    c->s[2] = 0x98badcfeu;
    c->s[3] = 0x10325476u;
    c->len = 0;
    c->have = 0;
}

static void md5_update(md5_ctx *c, const uint8_t *p, size_t n)
{
    c->len += n;
    if (c->have) {
        size_t take = 64 - c->have < n ? 64 - c->have : n;
        memcpy(c->block + c->have, p, take);
        c->have += take;
        p += take;
        n -= take;
        if (c->have < 64)
            return;
        md5_block(c, c->block);
        c->have = 0;
    }
    while (n >= 64) {
        md5_block(c, p);
        p += 64;
        n -= 64;
    }
    memcpy(c->block, p, n);
    c->have = n;
}

static void md5_final(md5_ctx *c, uint8_t out[16])
{
    uint64_t bits = c->len * 8;
    uint8_t pad = 0x80;
    md5_update(c, &pad, 1);
    uint8_t zero = 0;
    while (c->have != 56)
        md5_update(c, &zero, 1);
    uint8_t len[8];
    for (int i = 0; i < 8; i++)
        len[i] = (uint8_t)(bits >> (8 * i));   /* little-endian */
    md5_update(c, len, 8);
    for (int i = 0; i < 4; i++)
        for (int j = 0; j < 4; j++)
            out[4 * i + j] = (uint8_t)(c->s[i] >> (8 * j));
}

void n265_md5(const uint8_t *data, size_t len, uint8_t out[16])
{
    md5_ctx c;
    md5_init(&c);
    md5_update(&c, data, len);
    md5_final(&c, out);
}

/* ----------------------------------------------------------- the plane */

/* D.3.19's bytes, a chunk at a time: the sample itself at 8 bits, the low
 * byte and then the high one above it. A CHUNK and not a row, so the
 * scratch is a fixed 2 KiB whatever the picture is wide and no width can
 * reach past it. */
#define HASH_CHUNK      1024

/* The CRC-16 polynomial is the spec's: x^16 + x^12 + x^5 + 1, run MSB
 * first over each byte, with sixteen zero bits shifted in at the end so
 * the last byte reaches every stage. */
static uint16_t crc_byte(uint16_t crc, uint32_t byte)
{
    for (int b = 7; b >= 0; b--) {
        uint32_t msb = (crc >> 15) & 1;
        uint32_t bit = (byte >> b) & 1;
        crc = (uint16_t)((((crc << 1) + bit) & 0xffff) ^ (msb * 0x1021u));
    }
    return crc;
}

void n265_hash_plane(n265_pic_hash *h, int c, const pixel *p, int stride,
                     int w, int hgt)
{
    const int wide = BIT_DEPTH > 8;
    if (w <= 0 || hgt <= 0)
        return;

    if (h->type == N265_HASH_MD5) {
        md5_ctx ctx;
        md5_init(&ctx);
        uint8_t chunk[HASH_CHUNK * 2];
        for (int y = 0; y < hgt; y++) {
            const pixel *s = p + (size_t)y * stride;
            for (int x0 = 0; x0 < w; x0 += HASH_CHUNK) {
                int n = w - x0 < HASH_CHUNK ? w - x0 : HASH_CHUNK;
                if (wide)
                    for (int i = 0; i < n; i++) {
                        chunk[2 * i] = (uint8_t)s[x0 + i];
                        chunk[2 * i + 1] =
                            (uint8_t)((uint32_t)s[x0 + i] >> 8);
                    }
                else
                    for (int i = 0; i < n; i++)
                        chunk[i] = (uint8_t)s[x0 + i];
                md5_update(&ctx, chunk, (size_t)n * (wide ? 2 : 1));
            }
        }
        md5_final(&ctx, h->md5[c]);
    } else if (h->type == N265_HASH_CRC) {
        uint16_t crc = 0xffff;
        for (int y = 0; y < hgt; y++) {
            const pixel *s = p + (size_t)y * stride;
            for (int x = 0; x < w; x++) {
                crc = crc_byte(crc, (uint32_t)s[x] & 0xff);
                if (wide)
                    crc = crc_byte(crc, ((uint32_t)s[x] >> 8) & 0xff);
            }
        }
        /* the sixteen zero bits that flush the register */
        for (int i = 0; i < 16; i++) {
            uint32_t msb = (crc >> 15) & 1;
            crc = (uint16_t)(((crc << 1) & 0xffff) ^ (msb * 0x1021u));
        }
        h->crc[c] = crc;
    } else {
        /* The XOR mask is built from the sample's own coordinates, which
         * is the whole difference between this and a plain sum: two
         * samples exchanged between positions change it. */
        uint32_t sum = 0;
        for (int y = 0; y < hgt; y++) {
            const pixel *s = p + (size_t)y * stride;
            for (int x = 0; x < w; x++) {
                uint32_t mask = (uint32_t)((x & 0xff) ^ (y & 0xff) ^
                                           (x >> 8) ^ (y >> 8));
                sum = (sum + (((uint32_t)s[x] & 0xff) ^ mask)) & 0xffffffffu;
                if (wide)
                    sum = (sum + ((((uint32_t)s[x] >> 8) & 0xff) ^ mask))
                          & 0xffffffffu;
            }
        }
        h->checksum[c] = sum;
    }
}
