/*
 * Fractional-bit CABAC estimator: cost model contexts that mirror the
 * real engine's state evolution and charge -log2(p) per bin, Q8.
 * Copyright (c) 2026, the yah265 authors. GPL-2.0-or-later; see LICENSE.
 */

#ifndef N265_EST_H
#define N265_EST_H

#include "encoder/cabac.h"
#include "encoder/tables.h"

/* The per-state cost and transition tables (est.c). Exposed so the
 * per-bin call below can be inlined: the mode decision runs it millions
 * of times per frame and the estimator was 5-14% of the encode with it
 * behind a cross-translation-unit call. */
extern uint16_t n265_est_bits_mps[64], n265_est_bits_lps[64];
extern const uint8_t n265_est_trans_mps[64], n265_est_trans_lps[64];

/* bits (Q8) for coding `bin` in context state; updates the state exactly
 * as the arithmetic engine would */
static inline uint32_t n265_est_bin(cabac_ctx_t *ctx, int bin)
{
    uint32_t b;
    if (bin == ctx->mps) {
        b = n265_est_bits_mps[ctx->state];
        ctx->state = n265_est_trans_mps[ctx->state];
    } else {
        b = n265_est_bits_lps[ctx->state];
        if (ctx->state == 0)
            ctx->mps ^= 1;
        ctx->state = n265_est_trans_lps[ctx->state];
    }
    return b;
}

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
struct n265_frame_work;
void n265_est_advance_cu(struct n265_frame_work *e, est_state *st, int x0,
                         int y0, int log2n);

/* entropy.c: part_mode for an inter CU (9.3.3.7, Table 9-34), shared by
 * the real emit and the mode decision's cost model */
void n265_write_part_mode(struct n265_frame_work *e, const res_ops *ops,
                          int log2n, int part);

#endif
