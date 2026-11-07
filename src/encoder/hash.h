/*
 * The decoded picture hash (D.2.19 / D.3.19): MD5, CRC or checksum over
 * one reconstructed picture, per color component.
 * Copyright (c) 2026, the yah265 authors. GPL-2.0-or-later; see LICENSE.
 *
 * D.3.19 hashes the DECODED PICTURE, which is the coded picture and not
 * the cropped output: the array is pic_width_in_luma_samples wide, so a
 * stream with a conformance window hashes the padding columns too. Our
 * reconstruction is exactly that array (pad_w x pad_h), which is why the
 * hash is taken from fw->recon and not from what --dump-recon writes.
 *
 * The sample-to-byte rule is the same for all three: one byte per sample
 * at 8 bits, and at a greater depth the LOW byte first and then the high
 * one. That is a property of the spec's pictureData array, not of this
 * machine, so the writer below is endian-independent by construction.
 *
 * The hash is a function of the reconstruction and of nothing else, and
 * the reconstruction is byte-identical at every thread count, so the
 * message is too. It is computed on the DRIVER at retirement (encoder.c,
 * join_oldest), in coding order, for the same reason the HRD messages are.
 */

#ifndef N265_HASH_H
#define N265_HASH_H

#include <stddef.h>
#include <stdint.h>

#include "common/pixel.h"

/* hash_type, D.2.19: 0 MD5, 1 CRC, 2 checksum. The CLI's --hash is
 * x265's numbering, which is this plus one, and 0 there is "off". */
enum {
    N265_HASH_MD5      = 0,
    N265_HASH_CRC      = 1,
    N265_HASH_CHECKSUM = 2,
};

typedef struct n265_pic_hash {
    int type;               /* N265_HASH_*; the SEI's hash_type        */
    uint8_t md5[3][16];
    uint16_t crc[3];
    uint32_t checksum[3];
} n265_pic_hash;

/* Hash one plane of the decoded picture into component slot `c`. `w` and
 * `h` are that plane's size in samples - the CODED picture's, divided by
 * SubWidthC / SubHeightC for chroma - and `stride` is in samples. Only
 * the field `h->type` names is written. */
void n265_hash_plane(n265_pic_hash *h, int c, const pixel *p, int stride,
                     int w, int hgt);

/* The MD5 of an arbitrary byte string, which is what tests/unit/
 * hash_test.c checks the primitive itself against (RFC 1321's own
 * vectors) before it checks a plane. */
void n265_md5(const uint8_t *data, size_t len, uint8_t out[16]);

#endif
