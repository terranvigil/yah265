/*
 * cpu.h - runtime CPU feature detection and the per-class SIMD ablation
 * knob that every NEON dispatch site branches on.
 *
 * Adapted from yah264's src/common/cpu.{h,c} (our own tooling under
 * the CONTRIBUTING.md carve-out), with the x86 half dropped: yah265 targets
 * aarch64 for phase 6 and the C path stays first-class everywhere else.
 * Copyright (c) 2026, the yah265 authors. GPL-2.0-or-later; see LICENSE.
 */

#ifndef N265_CPU_H
#define N265_CPU_H

#include <stdatomic.h>
#include <stdint.h>

/* Feature bits. NEON is architecturally mandatory on aarch64, so its bit
 * doubles as "this is an aarch64 host with a usable SIMD path". DotProd
 * and I8MM are optional extensions a kernel may want a second variant
 * for; nothing dispatches on them yet. */
enum {
    N265_CPU_NEON    = 1u << 0,
    N265_CPU_DOTPROD = 1u << 1,   /* FEAT_DotProd (udot/sdot) */
    N265_CPU_I8MM    = 1u << 2,   /* FEAT_I8MM */
};

/* Kernel classes, one bit each. A class is the unit the ablation knob and
 * the campaign's accounting both work in: one family of kernels whose
 * wall delta under ablation is what that family's NEON bought. */
enum {
    N265_ASM_DCT   = 1u << 0,   /* fdct/idct, forward and inverse transforms */
    N265_ASM_SATD  = 1u << 1,   /* satd/sad and the ME cost kernels */
    N265_ASM_MC    = 1u << 2,   /* luma/chroma interpolation, bi average */
    N265_ASM_INTRA = 1u << 3,   /* intra prediction and its filters */
    N265_ASM_SAO   = 1u << 4,   /* SAO statistics and application */
    N265_ASM_QUANT = 1u << 5,   /* quant/dequant and the RDOQ marshalling */
    N265_ASM_ALL   = 0x3fu,
};

/* The detected feature word. The fast path inlines to an acquire load of
 * the ready flag plus a plain read of the cache: dispatch sites call this
 * once per kernel invocation, and yah264 measured the out-of-line
 * pthread_once walk at ~1% of encode self-time. The acquire pairs with the
 * release store cpu.c makes after filling both caches, so a first call
 * from a worker thread is race-free and TSan-clean.
 *
 * The plain `if (done) return cached;` guard this replaces was a real data
 * race on first use; do not reintroduce it. */
extern _Atomic int n265_cpu_ready_;
extern uint32_t n265_cpu_cached_;
extern uint32_t n265_asm_off_;
uint32_t n265_cpu_flags_slow(void);

static inline uint32_t n265_cpu_flags(void)
{
    if (atomic_load_explicit(&n265_cpu_ready_, memory_order_acquire))
        return n265_cpu_cached_;
    return n265_cpu_flags_slow();
}

/* Per-class SIMD ablation, a MEASUREMENT HOOK and nothing else.
 * N265_ASM_OFF is a comma-separated class list
 * (`dct,satd,mc,intra,sao,quant`, or `all`) that forces those dispatch
 * sites onto their C fallback while the rest of the encoder stays
 * as-shipped. Every NEON kernel is bit-exact with its C reference, so the
 * output is unchanged by construction and a class's wall delta under
 * ablation is exactly what that class's NEON buys.
 *
 * Read through n265_env() once on the slow path and published by the same
 * release store as the feature word, so reading it after n265_cpu_flags()
 * has returned is race-free. */
static inline int n265_asm_on(uint32_t cls)
{
    return (n265_cpu_flags() & N265_CPU_NEON) && !(n265_asm_off_ & cls);
}

/* Human-readable list of the active features, into `buf` (NUL-terminated). */
void n265_cpu_name(uint32_t flags, char *buf, int size);

#endif /* N265_CPU_H */
