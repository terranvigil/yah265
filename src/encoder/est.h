/*
 * Fractional-bit CABAC estimator: cost model contexts that mirror the
 * real engine's state evolution and charge -log2(p) per bin, Q8.
 * Copyright (c) 2026, the next265 authors. GPL-2.0-or-later; see LICENSE.
 */

#ifndef N265_EST_H
#define N265_EST_H

#include "encoder/cabac.h"
#include "encoder/tables.h"

/* bits (Q8) for coding `bin` in context state; updates the state exactly
 * as the arithmetic engine would */
uint32_t n265_est_bin(cabac_ctx_t *ctx, int bin);

static inline uint32_t n265_est_bypass_bits(int n)
{
    return (uint32_t)n << 8;
}

/* emitter backend: the shared syntax walkers push bins through this */
typedef struct res_ops {
    void *h;
    void (*bin)(void *h, int ctx_idx, int bin);
    void (*bypass)(void *h, int bin);
    void (*bypass_bits)(void *h, uint32_t v, int n);
} res_ops;

/* estimator backend state */
typedef struct est_state {
    cabac_ctx_t *ctx;       /* CTX_COUNT array */
    uint32_t bits;          /* Q8 accumulator */
} est_state;

void n265_est_ops(res_ops *ops, est_state *st);

void n265_tables_init_est(void);

/* entropy.c: replay one CU's committed records through the estimator */
struct n265_encoder;
void n265_est_advance_cu(struct n265_encoder *e, est_state *st, int x0,
                         int y0, int log2n);

#endif
