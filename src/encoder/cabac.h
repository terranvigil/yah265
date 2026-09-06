/*
 * CABAC arithmetic encoder.
 * Copyright (c) 2026, the yah265 authors. GPL-2.0-or-later; see LICENSE.
 */

#ifndef N265_CABAC_H
#define N265_CABAC_H

#include <stdint.h>

#include "common/bitstream.h"

typedef struct cabac_ctx_t {
    uint8_t state;  /* pStateIdx 0..63 */
    uint8_t mps;    /* valMps          */
} cabac_ctx_t;

typedef struct cabac_t {
    bitstream_t *bs;
    uint32_t low;       /* 10-bit window */
    uint32_t range;     /* 9-bit         */
    int outstanding;
    int first_bit;
} cabac_t;

/* 9.3.2.2 context variable init from the 8-bit initValue and SliceQpY. */
void n265_cabac_ctx_init(cabac_ctx_t *ctx, uint8_t init_value, int slice_qp);

/* (Re)start the arithmetic engine; contexts are untouched. Also used for
 * the re-init after PCM sample writing. */
void n265_cabac_start(cabac_t *cb, bitstream_t *bs);

void n265_cabac_bin(cabac_t *cb, cabac_ctx_t *ctx, int bin);

/* end_of_slice_segment_flag / pcm_flag: terminate bin. bin==1 flushes the
 * engine; the caller then byte-aligns the bitstream (alignment zeros). */
void n265_cabac_terminate(cabac_t *cb, int bin);

void n265_cabac_bypass(cabac_t *cb, int bin);
void n265_cabac_bypass_bits(cabac_t *cb, uint32_t val, int n);

#endif
