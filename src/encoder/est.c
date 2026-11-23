/*
 * Estimator: the CABAC probability model's per-state entropies. pLPS of
 * state s is 0.5 * alpha^s with alpha = (0.01875/0.5)^(1/63) (the
 * published design constant of the H.26x arithmetic coder), so the cost
 * of a bin is -log2(p) of whichever symbol was coded, and the state
 * transition matches Table 9-47.
 * Copyright (c) 2026, the yah265 authors. GPL-2.0-or-later; see LICENSE.
 */

#include <math.h>
#include <stdlib.h>

#include "encoder/est.h"
#include "encoder/preset.h"
#include "encoder/residual.h"

int n265_est_mode;

/* Resolved once, on the driver thread, with the other warms: every
 * estimator call reads it and a getenv per trial would be the item's own
 * cost twice over. */
void n265_est_warm(void)
{
    const char *v = n265_env("N265_EST_MODE");
    int m = v ? atoi(v) : 0;
    /* another encoder in this process may be reading it */
    if (n265_est_mode != m)
        n265_est_mode = m;
}

uint16_t n265_est_bits_mps[64], n265_est_bits_lps[64];
uint32_t n265_est_step[64][2];
const uint8_t n265_est_trans_mps[64] = {
     1, 2, 3, 4, 5, 6, 7, 8, 9,10,11,12,13,14,15,16,
    17,18,19,20,21,22,23,24,25,26,27,28,29,30,31,32,
    33,34,35,36,37,38,39,40,41,42,43,44,45,46,47,48,
    49,50,51,52,53,54,55,56,57,58,59,60,61,62,62,63
};
const uint8_t n265_est_trans_lps[64] = {
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
        n265_est_bits_lps[s] = (uint16_t)lround(-log2(plps) * 256.0);
        n265_est_bits_mps[s] = (uint16_t)lround(-log2(1.0 - plps) * 256.0);
    }
    /* and the packed step the inlined per-bin form reads: cost | next
     * state | the state-0 LPS symbol swap, one word per (state, symbol) */
    for (int s = 0; s < 64; s++) {
        n265_est_step[s][0] = (uint32_t)n265_est_bits_mps[s] |
                              ((uint32_t)n265_est_trans_mps[s] << 16);
        n265_est_step[s][1] = (uint32_t)n265_est_bits_lps[s] |
                              ((uint32_t)n265_est_trans_lps[s] << 16) |
                              (s == 0 ? 1u << 24 : 0u);
    }
    /* the estimator's other constant table: residual_coding's per-group
     * sig_coeff_flag patterns, which are derived from the scan tables
     * n265_tables_init() has already built when the driver thread runs
     * this at encoder open */
    n265_residual_tables_init();
    done = 1;
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
    ops->est = st;
    ops->frozen = n265_est_mode;
}
