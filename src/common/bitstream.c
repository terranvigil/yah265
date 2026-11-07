/*
 * MSB-first bit writer, Exp-Golomb, and RBSP -> NAL packaging with
 * emulation prevention (ITU-T H.265 7.3.1, 7.4.2).
 * Copyright (c) 2026, the yah265 authors. GPL-2.0-or-later; see LICENSE.
 */

#include <assert.h>
#include <stdlib.h>
#include <string.h>

#include "common/bitstream.h"

int bs_init(bitstream_t *bs, size_t capacity)
{
    bs->buf = malloc(capacity);
    if (!bs->buf)
        return -1;
    bs->cap = capacity;
    bs_reset(bs);
    return 0;
}

void bs_free(bitstream_t *bs)
{
    free(bs->buf);
    bs->buf = NULL;
    bs->cap = 0;
}

void bs_reset(bitstream_t *bs)
{
    bs->bytes = 0;
    bs->acc = 0;
    bs->acc_bits = 0;
    bs->failed = 0;
}

static int bs_grow(bitstream_t *bs, size_t need)
{
    size_t cap = bs->cap ? bs->cap : 256;
    while (cap < need)
        cap *= 2;
    uint8_t *p = realloc(bs->buf, cap);
    if (!p)
        return -1;
    bs->buf = p;
    bs->cap = cap;
    return 0;
}

void bs_put_bits(bitstream_t *bs, uint32_t val, int n)
{
    assert(n >= 0 && n <= 32);
    assert(n == 32 || (val >> n) == 0);
    bs->acc = (bs->acc << n) | val;
    bs->acc_bits += n;
    while (bs->acc_bits >= 8) {
        if (bs->bytes + 1 > bs->cap && bs_grow(bs, bs->bytes + 1) < 0) {
            /* out of memory: drop bits; caller sees short output and the
             * conformance gate fails loudly. never silently truncate a
             * byte boundary mid-stream in a way that parses. */
            bs->acc_bits = 0;
            bs->failed = 1;
            return;
        }
        bs->acc_bits -= 8;
        bs->buf[bs->bytes++] = (uint8_t)(bs->acc >> bs->acc_bits);
    }
    bs->acc &= ((uint64_t)1 << bs->acc_bits) - 1;
}

void bs_put_ue(bitstream_t *bs, uint32_t val)
{
    /* codeNum val: leadingZeroBits zeros, 1, then val+1-2^lz in lz bits */
    uint32_t vp1 = val + 1;
    int bits = 32 - __builtin_clz(vp1);       /* position of MSB, 1-based */
    bs_put_bits(bs, 0, bits - 1);
    bs_put_bits(bs, vp1, bits);
}

void bs_put_se(bitstream_t *bs, int32_t val)
{
    /* 7.4.9.1: positive v -> 2v-1, non-positive v -> -2v */
    uint32_t code = val > 0 ? (uint32_t)val * 2 - 1 : (uint32_t)(-val) * 2;
    bs_put_ue(bs, code);
}

void bs_align_zero(bitstream_t *bs)
{
    if (bs->acc_bits)
        bs_put_bits(bs, 0, 8 - bs->acc_bits);
}

void bs_align_one_then_zero(bitstream_t *bs)
{
    bs_put_bits(bs, 1, 1);
    bs_align_zero(bs);
}

void bs_rbsp_trailing(bitstream_t *bs)
{
    /* rbsp_stop_one_bit then alignment zeros (7.3.2.11) */
    bs_align_one_then_zero(bs);
}

size_t nal_pack(uint8_t *dst, size_t dst_cap, int nal_type,
                const uint8_t *rbsp, size_t rbsp_size)
{
    return nal_pack_tid(dst, dst_cap, nal_type, 0, rbsp, rbsp_size);
}

size_t nal_pack_tid(uint8_t *dst, size_t dst_cap, int nal_type, int tid,
                    const uint8_t *rbsp, size_t rbsp_size)
{
    /* 4-byte start code + 2-byte NAL header + EP'd payload.
     * NAL header: forbidden_zero_bit(1)=0, nal_unit_type(6),
     * nuh_layer_id(6)=0, nuh_temporal_id_plus1(3)=tid+1. */
    size_t o = 0;
    (void)dst_cap;
    dst[o++] = 0; dst[o++] = 0; dst[o++] = 0; dst[o++] = 1;
    dst[o++] = (uint8_t)((nal_type & 0x3f) << 1);
    dst[o++] = (uint8_t)((tid & 0x07) + 1);

    int zeros = 0;
    for (size_t i = 0; i < rbsp_size; i++) {
        uint8_t b = rbsp[i];
        if (zeros >= 2 && b <= 0x03) {
            dst[o++] = 0x03;
            zeros = 0;
        }
        dst[o++] = b;
        zeros = (b == 0) ? zeros + 1 : 0;
    }
    assert(o <= dst_cap);
    return o;
}

size_t nal_pack_bound(size_t rbsp_size)
{
    /* worst case: EP byte every 2 payload bytes, plus start code + header */
    return 6 + rbsp_size + rbsp_size / 2 + 2;
}
