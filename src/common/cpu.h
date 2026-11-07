/*
 * cpu.h - runtime CPU feature detection and the per-class SIMD ablation
 * knob that every NEON dispatch site branches on.
 *
 * Adapted from yah264's src/common/cpu.{h,c} (our own tooling under
 * the CONTRIBUTING.md carve-out). aarch64 has the NEON tier; x86-64 has
 * the AVX2 tier (docs/x86-plan.md). The C path stays first-class on both.
 * Copyright (c) 2026, the yah265 authors. GPL-2.0-or-later; see LICENSE.
 */

#ifndef N265_CPU_H
#define N265_CPU_H

#include <stdatomic.h>
#include <stdint.h>

#include "common/pixel.h"

/* Feature bits. NEON is architecturally mandatory on aarch64, so its bit
 * doubles as "this is an aarch64 host with a usable SIMD path". DotProd
 * and I8MM are optional extensions a kernel may want a second variant
 * for; nothing dispatches on them yet. */
enum {
    N265_CPU_NEON    = 1u << 0,
    N265_CPU_DOTPROD = 1u << 1,   /* FEAT_DotProd (udot/sdot) */
    N265_CPU_I8MM    = 1u << 2,   /* FEAT_I8MM */
    /* x86-64. AVX2 is set only with FMA3, BMI2 and the OS saving the YMM
     * state, because the AVX2 tier is built with all three -m flags and
     * may use any of them. */
    N265_CPU_SSE42   = 1u << 8,
    N265_CPU_AVX2    = 1u << 9,
    N265_CPU_AVX512  = 1u << 10,  /* F + BW + VL, ZMM state; no kernel yet */
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
/* Classes the ENCODER refuses for its own configuration, as opposed to
 * the ones a measurement asked for. Set on the driver at encoder open,
 * after n265_cpu_flags() has published the feature word and before any
 * worker exists, which is the same discipline n265_asm_off_ has.
 *
 * Phase 8 item 6a was its first user and phase 11 item 3 removed it
 * again: a 4:4:4 or 4:2:2 encode stood the MC class down because that
 * family's chroma kernels held 4:2:0's temp, and those kernels now size
 * the temp by the block they are handed. The word therefore has no
 * writer at the moment, and the mechanism is kept: it is the one place a
 * configuration can decline a whole class, which is what keeps
 * N265_ASM_OFF measuring what a family is worth instead of a shape gate
 * hidden inside a shared kernel. Its cost is the second load in
 * n265_asm_on, priced at nothing in phase 8 (docs/phase8-plan.md,
 * "Three arms measured to nothing"). */
extern uint32_t n265_asm_refuse_;
void n265_asm_refuse(uint32_t cls);
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
/* ---- which families this build has kernels for ----------------------
 *
 * meson defines one N265_NEON_<FAMILY> per family it compiles in, and
 * the list is per DEPTH (phase 8 item 3): a family reaches a build only
 * once its 16-bit lanes are proved bit-exact at that depth under a
 * checkasm built at the same depth. Normalised to 0/1 here so a C
 * expression can read them as well as the preprocessor can. */
#ifndef N265_NEON
#define N265_NEON 0
#endif
#ifndef N265_NEON_TRANSFORM
#define N265_NEON_TRANSFORM 0
#endif
#ifndef N265_NEON_SATD
#define N265_NEON_SATD 0
#endif
#ifndef N265_NEON_MC
#define N265_NEON_MC 0
#endif
#ifndef N265_NEON_RDOQ
#define N265_NEON_RDOQ 0
#endif
#ifndef N265_NEON_COST
#define N265_NEON_COST 0
#endif
#ifndef N265_NEON_INTRA
#define N265_NEON_INTRA 0
#endif

/* The classes any compiled-in family carries. A class is WIDER than a
 * family - N265_ASM_QUANT is dequant/quant in the transform file, the
 * RDOQ marshalling and the coefficient costing, three files - so this
 * mask is not the gate. The gate is the per-site `#if N265_NEON_<FAMILY>`,
 * which is a family-exact compile-time fact; this is the belt to that
 * brace, so that a dispatch site which ever lost its guard still takes
 * the C path in a build whose family was never ported, rather than
 * feeding 16-bit samples to a kernel written for 16 lanes of 8. */
#define N265_ASM_PORTED ( \
    (N265_NEON_TRANSFORM ? (N265_ASM_DCT | N265_ASM_QUANT) : 0u) | \
    (N265_NEON_SATD      ? (uint32_t)N265_ASM_SATD : 0u) | \
    (N265_NEON_MC        ? (uint32_t)N265_ASM_MC : 0u) | \
    (N265_NEON_RDOQ      ? (uint32_t)N265_ASM_QUANT : 0u) | \
    (N265_NEON_COST      ? (uint32_t)N265_ASM_QUANT : 0u) | \
    (N265_NEON_INTRA     ? (uint32_t)N265_ASM_INTRA : 0u))

static inline int n265_asm_on(uint32_t cls)
{
    if (!(cls & N265_ASM_PORTED))
        return 0;
    return (n265_cpu_flags() & N265_CPU_NEON) &&
           !((n265_asm_off_ | n265_asm_refuse_) & cls);
}

/* ---- the x86-64 AVX2 tier --------------------------------------------
 *
 * The same shape as the NEON half above: meson defines one
 * N265_AVX2_<FAMILY> per family it compiles into THIS library (8-bit
 * only for now; the Main10 library is C on x86), the kernels live in
 * src/encoder/x86/<family>_avx2.c, and a dispatch site reads
 *
 *     #if N265_AVX2_SATD
 *         if (n265_avx2_on(N265_ASM_SATD))
 *             return n265_foo_avx2(...);
 *     #endif
 *
 * beside its NEON twin. The two never coexist in one build. */
#ifndef N265_X86
#define N265_X86 0
#endif
#ifndef N265_AVX2_TRANSFORM
#define N265_AVX2_TRANSFORM 0
#endif
#ifndef N265_AVX2_SATD
#define N265_AVX2_SATD 0
#endif
#ifndef N265_AVX2_MC
#define N265_AVX2_MC 0
#endif
#ifndef N265_AVX2_RDOQ
#define N265_AVX2_RDOQ 0
#endif
#ifndef N265_AVX2_COST
#define N265_AVX2_COST 0
#endif
#ifndef N265_AVX2_INTRA
#define N265_AVX2_INTRA 0
#endif

#define N265_ASM_PORTED_AVX2 ( \
    (N265_AVX2_TRANSFORM ? (N265_ASM_DCT | N265_ASM_QUANT) : 0u) | \
    (N265_AVX2_SATD      ? (uint32_t)N265_ASM_SATD : 0u) | \
    (N265_AVX2_MC        ? (uint32_t)N265_ASM_MC : 0u) | \
    (N265_AVX2_RDOQ      ? (uint32_t)N265_ASM_QUANT : 0u) | \
    (N265_AVX2_COST      ? (uint32_t)N265_ASM_QUANT : 0u) | \
    (N265_AVX2_INTRA     ? (uint32_t)N265_ASM_INTRA : 0u))

static inline int n265_avx2_on(uint32_t cls)
{
    if (!(cls & N265_ASM_PORTED_AVX2))
        return 0;
    return (n265_cpu_flags() & N265_CPU_AVX2) &&
           !((n265_asm_off_ | n265_asm_refuse_) & cls);
}

/* Human-readable list of the active features, into `buf` (NUL-terminated). */
void n265_cpu_name(uint32_t flags, char *buf, int size);

#endif /* N265_CPU_H */
