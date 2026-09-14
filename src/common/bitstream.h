/*
 * MSB-first bit writer, Exp-Golomb, NAL packaging.
 * Copyright (c) 2026, the yah265 authors. GPL-2.0-or-later; see LICENSE.
 */

#ifndef N265_BITSTREAM_H
#define N265_BITSTREAM_H

#include <stddef.h>
#include <stdint.h>

typedef struct bitstream_t {
    uint8_t *buf;
    size_t cap;
    size_t bytes;       /* complete bytes written                */
    uint64_t acc;       /* pending bits, MSB-first, low-aligned  */
    int acc_bits;       /* number of pending bits (< 8 after put) */
} bitstream_t;

int  bs_init(bitstream_t *bs, size_t capacity);
void bs_free(bitstream_t *bs);
void bs_reset(bitstream_t *bs);
void bs_put_bits(bitstream_t *bs, uint32_t val, int n);
void bs_put_ue(bitstream_t *bs, uint32_t val);
void bs_put_se(bitstream_t *bs, int32_t val);
void bs_align_zero(bitstream_t *bs);
void bs_align_one_then_zero(bitstream_t *bs);
void bs_rbsp_trailing(bitstream_t *bs);

/* Wrap a finished RBSP into an Annex-B NAL (start code, header, emulation
 * prevention). Returns bytes written to dst. Caller sizes dst with
 * nal_pack_bound(). */
/* `tid` is nuh_temporal_id_plus1 - 1, i.e. the TemporalId of the NAL. It
 * is 0 on everything unless --temporal-layers put a non-reference B leaf
 * in sub-layer 1; a parameter set, an AUD, an EOS and an EOB are all
 * TemporalId 0 by 7.4.2.2 whatever the picture around them is. */
size_t nal_pack_tid(uint8_t *dst, size_t dst_cap, int nal_type, int tid,
                    const uint8_t *rbsp, size_t rbsp_size);
size_t nal_pack(uint8_t *dst, size_t dst_cap, int nal_type,
                const uint8_t *rbsp, size_t rbsp_size);
size_t nal_pack_bound(size_t rbsp_size);

#endif
