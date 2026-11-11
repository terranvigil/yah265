/*
 * checkasm.h - the shared harness: rng, timer, the bench line, and the
 * guard-buffer and page-guard helpers every family check runs its
 * optimized path through.
 *
 * Layout (split out of the single-file checkasm.c on 2026-09-10, phase 6):
 *
 *   checkasm.h    this file: rng, bench(), guard buffers, page guard
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
 * Under `#if CA_SIMD_<FAMILY>` a family compares `n265_x_ref` against
 * `CA_SIMD(n265_x)` DIRECTLY, never through the dispatching entry point: the
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

#include "common/pixel.h"   /* pixel, BIT_DEPTH, PIXEL_MAX */
#include "common/cpu.h"

/* ---- the ISA under test ---------------------------------------------
 *
 * One binary checks every SIMD tier the machine has: NEON (and its
 * DotProd arm) on aarch64, SSE4.2 and AVX2 on x86-64. main() runs the
 * families once per tier with the feature word capped to that tier, so
 * the dispatched entry points take that tier's path too. The family files
 * name a kernel as CA_SIMD(n265_foo) and test for its presence with
 * CA_SIMD_<FAMILY>; on x86-64 CA_SIMD picks the tier's twin at run time.
 * The tiers export the same entry points per family
 * (src/encoder/arm/neon.h, src/encoder/x86/avx2.h, x86/sse42.h); that is
 * what makes this a rename and not a second harness. */
#if N265_NEON
#include "encoder/arm/neon.h"
#define CA_SIMD(f) f##_neon
#define CA_SIMD_TRANSFORM N265_NEON_TRANSFORM
#define CA_SIMD_SATD      N265_NEON_SATD
#define CA_SIMD_MC        N265_NEON_MC
#define CA_SIMD_RDOQ      N265_NEON_RDOQ
#define CA_SIMD_COST      N265_NEON_COST
#define CA_SIMD_INTRA     N265_NEON_INTRA
#define CA_SIMD_DEBLOCK   N265_NEON_DEBLOCK
#define CA_SIMD_SAO       N265_NEON_SAO
#elif N265_X86
#include "encoder/x86/avx2.h"
#include "encoder/x86/sse42.h"
#define CA_SIMD(f) ((n265_cpu_flags() & N265_CPU_AVX2) ? f##_avx2 : f##_sse42)
#define CA_SIMD_TRANSFORM N265_AVX2_TRANSFORM
#define CA_SIMD_SATD      N265_AVX2_SATD
#define CA_SIMD_MC        N265_AVX2_MC
#define CA_SIMD_RDOQ      N265_AVX2_RDOQ
#define CA_SIMD_COST      N265_AVX2_COST
#define CA_SIMD_INTRA     N265_AVX2_INTRA
#define CA_SIMD_DEBLOCK   N265_AVX2_DEBLOCK
#define CA_SIMD_SAO       N265_AVX2_SAO
/* A family with no SSE4.2 kernels is skipped at that tier, and these
 * names only let it link. */
#if !N265_SSE42_SATD
#define n265_satd4_sse42       n265_satd4_avx2
#define n265_satd8_sse42       n265_satd8_avx2
#define n265_satd_wh_sse42     n265_satd_wh_avx2
#define n265_sad_wh_sse42      n265_sad_wh_avx2
#define n265_ssd_wh_sse42      n265_ssd_wh_avx2
#define n265_sad_rowcol_sse42  n265_sad_rowcol_avx2
#define n265_psy_energy_sse42  n265_psy_energy_avx2
#define n265_sum_sq16_sse42    n265_sum_sq16_avx2
#endif
#if !N265_SSE42_MC
#define n265_bi_avg_wh_sse42       n265_bi_avg_wh_avx2
#define n265_wp_uni_wh_sse42       n265_wp_uni_wh_avx2
#define n265_wp_bi_wh_sse42        n265_wp_bi_wh_avx2
#define n265_wp_pix_wh_sse42       n265_wp_pix_wh_avx2
#define n265_mc_luma_hf_sse42      n265_mc_luma_hf_avx2
#define n265_mc_luma_14_hf_sse42   n265_mc_luma_14_hf_avx2
#define n265_mc_luma_wh_sse42      n265_mc_luma_wh_avx2
#define n265_mc_luma_14_wh_sse42   n265_mc_luma_14_wh_avx2
#define n265_mc_luma_hplane_sse42  n265_mc_luma_hplane_avx2
#define n265_mc_chroma_wh_sse42    n265_mc_chroma_wh_avx2
#define n265_mc_chroma_14_wh_sse42 n265_mc_chroma_14_wh_avx2
#endif
#if !N265_SSE42_TRANSFORM
#define n265_fdct_sse42      n265_fdct_avx2
#define n265_idct_add_sse42  n265_idct_add_avx2
#define n265_dequant_sse42   n265_dequant_avx2
#define n265_quant_sse42     n265_quant_avx2
#endif
#if !N265_SSE42_RDOQ
#define n265_rdoq_rtn_sse42    n265_rdoq_rtn_avx2
#define n265_rdoq_sb_nz_sse42  n265_rdoq_sb_nz_avx2
#endif
#if !N265_SSE42_COST
#define n265_csbf_map_sse42     n265_csbf_map_avx2
#define n265_coeff_group_sse42  n265_coeff_group_avx2
#define n265_sbh_adjust_sse42   n265_sbh_adjust_avx2
#endif
#if !N265_SSE42_INTRA
#define n265_intra_pred_sse42   n265_intra_pred_avx2
#define n265_filter_refs_sse42  n265_filter_refs_avx2
#endif
#if !N265_SSE42_DEBLOCK
#define n265_dbk_luma_v_sse42    n265_dbk_luma_v_avx2
#define n265_dbk_luma_h_sse42    n265_dbk_luma_h_avx2
#define n265_dbk_chroma_v_sse42  n265_dbk_chroma_v_avx2
#define n265_dbk_chroma_h_sse42  n265_dbk_chroma_h_avx2
#endif
#if !N265_SSE42_SAO
#define n265_sao_band_stats_sse42  n265_sao_band_stats_avx2
#define n265_sao_eo_stats_sse42    n265_sao_eo_stats_avx2
#define n265_sao_eo_apply_sse42    n265_sao_eo_apply_avx2
#define n265_sao_bo_apply_sse42    n265_sao_bo_apply_avx2
#endif
#else
#define CA_SIMD_TRANSFORM 0
#define CA_SIMD_SATD      0
#define CA_SIMD_MC        0
#define CA_SIMD_RDOQ      0
#define CA_SIMD_COST      0
#define CA_SIMD_INTRA     0
#define CA_SIMD_DEBLOCK   0
#define CA_SIMD_SAO       0
#endif

/* the tier the families are running at, for the bench line */
extern const char *ca_isa;

/* set by main() from the CLI; families bench only when it is nonzero */
extern int ca_bench;

/* xorshift32, one stream for the whole run so a failure reproduces */
uint32_t rnd(void);
double now_ns(void);

/* ---- samples -------------------------------------------------------
 *
 * The harness is built at both depths since phase 8 item 3, and a
 * SAMPLE is `pixel` - one byte at 8 bits, two at Main10 - so a family
 * fills its blocks through these rather than with a byte at a time. The
 * domain is the build's: 0 .. PIXEL_MAX, because that is all a kernel is
 * ever handed and it is what the lane-headroom arguments in
 * the arm kernels are proved against. Taking the TOP BIT_DEPTH bits
 * of the stream means the 8-bit harness draws exactly the samples it
 * drew before this existed (`rnd() >> 24`), so no 8-bit check changes
 * what it tests. */
static inline pixel ca_pix(void)
{
    return (pixel)(rnd() >> (32 - BIT_DEPTH));
}

static inline void ca_fill_pix(pixel *p, size_t n)
{
    for (size_t i = 0; i < n; i++)
        p[i] = ca_pix();
}

/* a sample at one end of the domain, for the adversarial fills */
static inline pixel ca_pix_edge(void)
{
    return (rnd() & 1) ? (pixel)PIXEL_MAX : (pixel)0;
}

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
 * grabs eight samples where four were declared. The sanitizer finds those
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
 * border check cannot see because the padding is interior.
 *
 * It works in BYTES, so a pixel block passes `stride * sizeof(pixel)` and
 * `w * sizeof(pixel)`; ca_check_pad_pix does that for you. A poisoned
 * pixel is 0xa5a5 at Main10 and 0xa5 at 8 bits, and either way every
 * byte of it is CA_POISON, so the byte-wise check is exact at both. */
int ca_check_pad(const uint8_t *blk, int stride, int w, int h,
                 const char *what);

static inline int ca_check_pad_pix(const pixel *blk, int stride, int w, int h,
                                   const char *what)
{
    return ca_check_pad((const uint8_t *)blk, (int)(stride * sizeof(pixel)),
                        (int)(w * sizeof(pixel)), h, what);
}

/* ---- the page guard -------------------------------------------------
 *
 * The two nets above between them catch a read whose VALUE reaches the
 * output. They do not catch one whose value is thrown away, and every
 * family in this directory produces exactly that bug: a four-sample tail
 * written as "load 4, duplicate, run the 8-lane op, store 4" compiles to
 * a plain 16-byte `ldr q`, so the kernel reads four samples past the row,
 * discards them, and passes the scramble check because the output cannot
 * move. The sanitizer is quiet too, because the bytes are still inside
 * the allocation.
 *
 * The page guard is the net that does not need the value: a mapping sized
 * to EXACTLY the declared window with an unmapped page hard against it, so
 * a read one byte too far faults whether or not anything is done with it.
 * Each pass runs twice - `tail = 1` puts the window's last byte against
 * the far page (an overread faults) and `tail = 0` puts its first byte
 * against the near page (an underread faults).
 *
 * The window has to be the one the kernel DECLARES, sized per call: give
 * it a spare row and the check is worth nothing. For a block read at a
 * stride that window is ca_pg_window(), which stops at the last sample of
 * the last row rather than at the end of that row - a read into an
 * INTERIOR row's padding stays the scramble check's job, since the padding
 * is mapped either way.
 *
 * A fault is a hard stop rather than a counted failure, so arm the handler
 * with the shape's name and it is printed on the way out.
 */
typedef struct ca_pg {
    uint8_t *map;
    size_t maplen;
    void *user;         /* the buffer the kernel is given */
} ca_pg;

/* `bytes` of read-write memory with PROT_NONE on both sides, placed so
 * that the buffer ENDS at the far guard (tail = 1) or STARTS at the near
 * one (tail = 0) */
void *ca_pg_alloc(ca_pg *g, size_t bytes, int tail);
void ca_pg_free(ca_pg *g);
/* random bytes over the `bytes` of buffer the kernel is given */
void ca_pg_fill(ca_pg *g, size_t bytes);
/* the same for a sample buffer: `n` pixels in 0 .. PIXEL_MAX */
static inline void ca_pg_fill_pix(ca_pg *g, size_t n)
{
    ca_fill_pix((pixel *)g->user, n);
}
/* catch SIGSEGV/SIGBUS and name `what` on the way out */
void ca_pg_arm(const char *what);
void ca_pg_disarm(void);

/* the bytes a w x h block read at `stride` declares: through the last
 * sample of the last row, and not one byte of that row's padding */
static inline size_t ca_pg_window(int stride, int w, int h)
{
    return (size_t)(h - 1) * (size_t)stride + (size_t)w;
}

/* the same window in BYTES for a block of samples */
static inline size_t ca_pg_window_pix(int stride, int w, int h)
{
    return ca_pg_window(stride, w, h) * sizeof(pixel);
}

#endif /* N265_CHECKASM_H */
