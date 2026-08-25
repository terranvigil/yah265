/*
 * CABAC arithmetic encoder (ITU-T H.265 9.3: 9-bit range, 10-bit low,
 * outstanding-bit carry handling per the spec's informative encoder).
 * Copyright (c) 2026, the next265 authors. GPL-2.0-or-later; see LICENSE.
 */

#include "encoder/cabac.h"

/* Table 9-46: rangeTabLPS[pStateIdx][qRangeIdx] (spec constants). */
static const uint8_t range_tab_lps[64][4] = {
    {128,176,208,240},{128,167,197,227},{128,158,187,216},{123,150,178,205},
    {116,142,169,195},{111,135,160,185},{105,128,152,175},{100,122,144,166},
    { 95,116,137,158},{ 90,110,130,150},{ 85,104,123,142},{ 81, 99,117,135},
    { 77, 94,111,128},{ 73, 89,105,122},{ 69, 85,100,116},{ 66, 80, 95,110},
    { 62, 76, 90,104},{ 59, 72, 86, 99},{ 56, 69, 81, 94},{ 53, 65, 77, 89},
    { 51, 62, 73, 85},{ 48, 59, 69, 80},{ 46, 56, 66, 76},{ 43, 53, 63, 72},
    { 41, 50, 59, 69},{ 39, 48, 56, 65},{ 37, 45, 54, 62},{ 35, 43, 51, 59},
    { 33, 41, 48, 56},{ 32, 39, 46, 53},{ 30, 37, 43, 50},{ 29, 35, 41, 48},
    { 27, 33, 39, 45},{ 26, 31, 37, 43},{ 24, 30, 35, 41},{ 23, 28, 33, 39},
    { 22, 27, 32, 37},{ 21, 26, 30, 35},{ 20, 24, 29, 33},{ 19, 23, 27, 31},
    { 18, 22, 26, 30},{ 17, 21, 25, 28},{ 16, 20, 23, 27},{ 15, 19, 22, 25},
    { 14, 18, 21, 24},{ 14, 17, 20, 23},{ 13, 16, 19, 22},{ 12, 15, 18, 21},
    { 12, 14, 17, 20},{ 11, 14, 16, 19},{ 11, 13, 15, 18},{ 10, 12, 15, 17},
    { 10, 12, 14, 16},{  9, 11, 13, 15},{  9, 11, 12, 14},{  8, 10, 12, 14},
    {  8,  9, 11, 13},{  7,  9, 11, 12},{  7,  9, 10, 12},{  7,  8, 10, 11},
    {  6,  8,  9, 11},{  6,  7,  9, 10},{  6,  7,  8,  9},{  2,  2,  2,  2}
};

/* Table 9-47: state transitions. */
static const uint8_t trans_idx_mps[64] = {
     1, 2, 3, 4, 5, 6, 7, 8, 9,10,11,12,13,14,15,16,
    17,18,19,20,21,22,23,24,25,26,27,28,29,30,31,32,
    33,34,35,36,37,38,39,40,41,42,43,44,45,46,47,48,
    49,50,51,52,53,54,55,56,57,58,59,60,61,62,62,63
};
static const uint8_t trans_idx_lps[64] = {
     0, 0, 1, 2, 2, 4, 4, 5, 6, 7, 8, 9, 9,11,11,12,
    13,13,15,15,16,16,18,18,19,19,21,21,22,22,23,24,
    24,25,26,26,27,27,28,29,29,30,30,30,31,32,32,33,
    33,33,34,34,35,35,35,36,36,36,37,37,37,38,38,63
};

static inline int clip3(int lo, int hi, int v)
{
    return v < lo ? lo : v > hi ? hi : v;
}

void n265_cabac_ctx_init(cabac_ctx_t *ctx, uint8_t init_value, int slice_qp)
{
    /* 9.3.2.2 */
    int m = (init_value >> 4) * 5 - 45;
    int n = ((init_value & 15) << 3) - 16;
    int pre = clip3(1, 126, ((m * clip3(0, 51, slice_qp)) >> 4) + n);
    if (pre <= 63) {
        ctx->state = (uint8_t)(63 - pre);
        ctx->mps = 0;
    } else {
        ctx->state = (uint8_t)(pre - 64);
        ctx->mps = 1;
    }
}

void n265_cabac_start(cabac_t *cb, bitstream_t *bs)
{
    cb->bs = bs;
    cb->low = 0;
    cb->range = 510;
    cb->outstanding = 0;
    cb->first_bit = 1;
}

static void put_bit(cabac_t *cb, int b)
{
    if (cb->first_bit)
        cb->first_bit = 0;
    else
        bs_put_bits(cb->bs, (uint32_t)b, 1);
    while (cb->outstanding > 0) {
        bs_put_bits(cb->bs, (uint32_t)(1 - b), 1);
        cb->outstanding--;
    }
}

static void renorm(cabac_t *cb)
{
    while (cb->range < 256) {
        if (cb->low >= 512) {
            put_bit(cb, 1);
            cb->low -= 512;
        } else if (cb->low < 256) {
            put_bit(cb, 0);
        } else {
            cb->outstanding++;
            cb->low -= 256;
        }
        cb->low <<= 1;
        cb->range <<= 1;
    }
}

void n265_cabac_bin(cabac_t *cb, cabac_ctx_t *ctx, int bin)
{
    uint32_t lps = range_tab_lps[ctx->state][(cb->range >> 6) & 3];
    cb->range -= lps;
    if (bin != ctx->mps) {
        cb->low += cb->range;
        cb->range = lps;
        if (ctx->state == 0)
            ctx->mps ^= 1;
        ctx->state = trans_idx_lps[ctx->state];
    } else {
        ctx->state = trans_idx_mps[ctx->state];
    }
    renorm(cb);
}

void n265_cabac_terminate(cabac_t *cb, int bin)
{
    cb->range -= 2;
    if (bin) {
        cb->low += cb->range;
        cb->range = 2;
        renorm(cb);
        /* flush (spec informative encoder): emit the decided bits of low;
         * the final forced 1 doubles as the RBSP stop bit / the bit the
         * decoder's terminate handling expects before byte alignment. */
        put_bit(cb, (int)((cb->low >> 9) & 1));
        bs_put_bits(cb->bs, ((cb->low >> 7) & 3) | 1, 2);
    } else {
        renorm(cb);
    }
}
