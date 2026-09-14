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

/* The three tables above folded into one, so a bin is a load and two
 * byte stores instead of a branch over three loads: [state][bin != mps]
 * packs the Q8 cost in bits 0-15, the next state in bits 16-23 and the
 * valMps flip in bit 24. The flip is set only on the LPS of state 0,
 * which is the one place 9.3.3.2.1 swaps the symbol, so the packed step
 * is the same arithmetic on every input and no estimate moves.
 * Built by n265_tables_init_est(). */
extern uint32_t n265_est_step[64][2];

/* bits (Q8) for coding `bin` in context state; updates the state exactly
 * as the arithmetic engine would */
static inline uint32_t n265_est_bin(cabac_ctx_t *ctx, int bin)
{
    uint32_t e = n265_est_step[ctx->state][bin != ctx->mps];
    ctx->state = (uint8_t)(e >> 16);
    ctx->mps ^= (uint8_t)(e >> 24);
    return e & 0xffffu;
}

/* The FROZEN form (phase 13 A1): the same Q8 price, read from the state
 * the context is in NOW and leaving it there. `n265_est_bin` is the
 * encoder's model of what the arithmetic engine will do next and adapts
 * on every bin; this one is a price read off a snapshot, which is what
 * `n265_rdoq`'s cost_mps / cost_lps already are for the lattice. Nothing
 * calls it unless N265_EST_MODE is 1, and then only inside the residual
 * walk of a TRIAL. See docs/phase13.md "A1". */
static inline uint32_t n265_est_cost(const cabac_ctx_t *ctx, int bin)
{
    return n265_est_step[ctx->state][bin != ctx->mps] & 0xffffu;
}

static inline uint32_t n265_est_bypass_bits(int n)
{
    return (uint32_t)n << 8;
}

/* emitter backend: the shared syntax walkers push bins through this.
 *
 * `est` is the estimator's own state when this backend IS the estimator
 * and NULL otherwise, including for the cbit wrapper that prices real
 * bins. residual_coding reads it to take a straight-line instance of the
 * same walk with the per-bin call inlined; nothing else may branch on
 * it, because every other walker has one shape for both backends.
 *
 * `frozen` is phase 13 A1's mode, and it is a property of the CALL and
 * not of the estimator: n265_est_ops() copies n265_est_mode into it, so
 * every trial that prices a TB gets the mode, and n265_est_advance_cu()
 * clears it, because the replay of a COMMITTED CU is what keeps the
 * estimator's contexts in step with the real coder's and must adapt
 * whatever the trials do. 0 in the real emitter, always. */
struct est_state;
typedef struct res_ops {
    void *h;
    void (*bin)(void *h, int ctx_idx, int bin);
    void (*bypass)(void *h, int bin);
    void (*bypass_bits)(void *h, uint32_t v, int n);
    struct est_state *est;
    int frozen;
} res_ops;

/* estimator backend state */
typedef struct est_state {
    cabac_ctx_t *ctx;       /* CTX_COUNT array */
    uint32_t bits;          /* Q8 accumulator */
} est_state;

void n265_est_ops(res_ops *ops, est_state *st);

/* N265_EST_MODE (phase 13 A1): 0 = the adapting estimator, which is
 * every encoder before 2026-09-14 and is byte-identical to it; 1 = the
 * frozen-context one. Resolved on the driver thread by n265_est_warm()
 * and read-only afterwards. */
extern int n265_est_mode;
void n265_est_warm(void);

void n265_tables_init_est(void);

/* entropy.c: its cached env knob, resolved on the driver thread at open
 * with the other warms */
void n265_entropy_warm(void);

/* entropy.c: replay one CU's committed records through the estimator */
struct n265_frame_work;
void n265_est_advance_cu(struct n265_frame_work *e, est_state *st, int x0,
                         int y0, int log2n);

/* entropy.c: part_mode for an inter CU (9.3.3.7, Table 9-34), shared by
 * the real emit and the mode decision's cost model */
void n265_write_part_mode(struct n265_frame_work *e, const res_ops *ops,
                          int log2n, int part);

#endif
