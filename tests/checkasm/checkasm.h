/*
 * checkasm.h - the shared harness: rng, timer, the bench line, and the
 * guard-buffer helpers every family check runs its optimized path through.
 *
 * Layout (split out of the single-file checkasm.c on 2026-09-10, phase 6):
 *
 *   checkasm.h    this file: rng, bench(), guard buffers
 *   main.c        the test table and the CLI
 *   transform.c   check_transform()
 *   satd.c        check_satd()
 *   mc.c          check_mc()
 *
 * Adding a family: write tests/checkasm/<family>.c exporting
 * `int check_<family>(void)` (0 = pass), add it to `tests` in main.c and to
 * `checkasm_sources` in tests/meson.build. Nothing else moves, which is the
 * point: three kernel worktrees can add three families without touching one
 * another's files.
 *
 * A family check does correctness unconditionally and benches only when
 * `ca_bench` is set, so `make test` stays fast and `checkasm --bench` is
 * where the tables come from.
 *
 * Under `#if N265_NEON` a family compares `n265_x_ref` against
 * `n265_x_neon` DIRECTLY, never through the dispatching entry point: the
 * entry point can be ablated by N265_ASM_OFF, and a harness that could be
 * silently turned into a C-against-C tautology is not a harness.
 *
 * Copyright (c) 2026, the yah265 authors. GPL-2.0-or-later; see LICENSE.
 */

#ifndef N265_CHECKASM_H
#define N265_CHECKASM_H

#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* set by main() from the CLI; families bench only when it is nonzero */
extern int ca_bench;

/* xorshift32, one stream for the whole run so a failure reproduces */
uint32_t rnd(void);
double now_ns(void);

/* The benchmark line. `neon` may be NULL, which prints the two-column form
 * kernels with no NEON yet have always printed; with it, the line is
 * ref / c / neon ns per call plus the neon-over-c multiple.
 *
 * Where a family has no C fast path distinct from its reference, pass the
 * reference for both `ref` and `opt`: the middle column is then a control
 * on the measurement itself and the multiple is still the number that
 * matters. */
void bench(const char *what, void (*ref)(void *), void (*opt)(void *),
           void (*neon)(void *), void *ctx, int iters);

/* ---- guard buffers -------------------------------------------------
 *
 * Overreading and overwriting past a block edge is THE classic NEON bug -
 * the tail of a width the vector loop does not cover, or a load that
 * grabs eight samples where four were declared. The sanitiser finds those
 * only when the block happens to sit at the end of a heap allocation; this
 * finds them every call.
 *
 * A guarded block is a plain malloc with CA_GUARD bytes of border on each
 * side. Two checks, and a family wants both:
 *
 *   OVERWRITE  ca_guard_poison() before the call, ca_guard_check() after:
 *              any byte the kernel wrote outside its block is caught.
 *   OVERREAD   ca_guard_scramble() the border, run, keep the output, then
 *              scramble again with different bytes and run again. If the
 *              two outputs differ, the kernel read outside its declared
 *              window.
 */
#define CA_GUARD  64
#define CA_POISON 0xa5

void *ca_guard_alloc(size_t bytes);
void ca_guard_free(void *blk);
/* refill both borders with the fixed pattern */
void ca_guard_poison(void *blk, size_t bytes);
/* refill both borders with fresh random bytes */
void ca_guard_scramble(void *blk, size_t bytes);
/* 0 = clean; prints and returns nonzero if a border byte moved */
int ca_guard_check(const void *blk, size_t bytes, const char *what);

/* A destination written at `stride` leaves padding to the right of every
 * row that no kernel may touch. Pre-fill the whole block with CA_POISON,
 * run, then call this: it reports a write into the padding, which a plain
 * border check cannot see because the padding is interior. */
int ca_check_pad(const uint8_t *blk, int stride, int w, int h,
                 const char *what);

#endif /* N265_CHECKASM_H */
