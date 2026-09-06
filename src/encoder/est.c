/*
 * Estimator: the CABAC probability model's per-state entropies. pLPS of
 * state s is 0.5 * alpha^s with alpha = (0.01875/0.5)^(1/63) (the
 * published design constant of the H.26x arithmetic coder), so the cost
 * of a bin is -log2(p) of whichever symbol was coded, and the state
 * transition matches Table 9-47.
 * Copyright (c) 2026, the yah265 authors. GPL-2.0-or-later; see LICENSE.
 */

#include <math.h>

#include "encoder/est.h"

static uint16_t bits_mps[64], bits_lps[64];
static const uint8_t trans_mps[64] = {
     1, 2, 3, 4, 5, 6, 7, 8, 9,10,11,12,13,14,15,16,
    17,18,19,20,21,22,23,24,25,26,27,28,29,30,31,32,
    33,34,35,36,37,38,39,40,41,42,43,44,45,46,47,48,
    49,50,51,52,53,54,55,56,57,58,59,60,61,62,62,63
};
static const uint8_t trans_lps[64] = {
     0, 0, 1, 2, 2, 4, 4, 5, 6, 7, 8, 9, 9,11,11,12,
    13,13,15,15,16,16,18,18,19,19,21,21,22,22,23,24,
    24,25,26,26,27,27,28,29,29,30,30,30,31,32,32,33,
    33,33,34,34,35,35,35,36,36,36,37,37,37,38,38,63
};

void n265_tables_init_est(void)
{
    static int done;
    if (done)
        return;
    double alpha = pow(0.01875 / 0.5, 1.0 / 63.0);
    for (int s = 0; s < 64; s++) {
        double plps = 0.5 * pow(alpha, s);
        bits_lps[s] = (uint16_t)lround(-log2(plps) * 256.0);
        bits_mps[s] = (uint16_t)lround(-log2(1.0 - plps) * 256.0);
    }
    done = 1;
}

uint32_t n265_est_bin(cabac_ctx_t *ctx, int bin)
{
    uint32_t b;
    if (bin == ctx->mps) {
        b = bits_mps[ctx->state];
        ctx->state = trans_mps[ctx->state];
    } else {
        b = bits_lps[ctx->state];
        if (ctx->state == 0)
            ctx->mps ^= 1;
        ctx->state = trans_lps[ctx->state];
    }
    return b;
}

static void est_bin_cb(void *h, int ctx_idx, int bin)
{
    est_state *st = h;
    st->bits += n265_est_bin(&st->ctx[ctx_idx], bin);
}

static void est_bypass_cb(void *h, int bin)
{
    (void)bin;
    ((est_state *)h)->bits += 256;
}

static void est_bypass_bits_cb(void *h, uint32_t v, int n)
{
    (void)v;
    ((est_state *)h)->bits += (uint32_t)n << 8;
}

void n265_est_ops(res_ops *ops, est_state *st)
{
    ops->h = st;
    ops->bin = est_bin_cb;
    ops->bypass = est_bypass_cb;
    ops->bypass_bits = est_bypass_bits_cb;
}
