/*
 * mc_neon.c - NEON motion-compensation kernels (class N265_ASM_MC).
 * One file per kernel family; see src/encoder/arm/neon.h for the dispatch
 * convention every kernel here follows.
 * Copyright (c) 2026, the yah265 authors. GPL-2.0-or-later; see LICENSE.
 */

#if N265_NEON_MC

#include <arm_neon.h>
#include <stddef.h>
#include <string.h>

#include "encoder/arm/neon.h"

/* The shifts of 8.5.3.3.3.2, the same expressions src/encoder/mc.c
 * writes, duplicated here for the reason the tap tables are: the two
 * translation units are meant to be independently readable. At 8 bits
 * they fold to the literals this file used to spell out (0, 6, 32, 12,
 * 2048) and at Main10 they are 2, 4, 8, 10, 512.
 *
 * Two of the folds are DEPTH-INVARIANT and are what let most of this
 * file stay one body: shift1 + shift3 is 6 at every depth, so a
 * one-stage filter's `(s + 32) >> 6` never moves, and a 2-D filter whose
 * intermediate already carries shift1 finishes with `>> 6` into 14 bits
 * at every depth too. What DOES move is the sample end of each pass:
 * the accumulator width in (a horizontal 8-tap over 10-bit samples
 * reaches 90024 and int16 will not hold it), and the clip out. */
#define MC_SHIFT1   (BIT_DEPTH - 8)         /* 0 at 8 bits */
#define MC_SHIFT3   (14 - BIT_DEPTH)        /* 6 at 8 bits */
#define MC_ROUND3   (1 << (MC_SHIFT3 - 1))  /* 32 */
#define MC_SHIFT23  (6 + MC_SHIFT3)         /* 12 */
#define MC_ROUND23  (1 << (MC_SHIFT23 - 1)) /* 2048 */
/* 8.5.3.3.4.2 default bi-prediction: shift = 15 - BitDepth, 7 at 8 bits */
#define MC_SHIFT_BI (15 - BIT_DEPTH)

/* The C form is
 *
 *     v = (a + b + (1 << (14 - BitDepth))) >> (15 - BitDepth);
 *     dst = clip3(0, PIXEL_MAX, v)
 *
 * on int16 inputs, so the sum needs 17 bits and the widening add is not
 * optional at either depth. The narrow reproduces the C EXACTLY rather than approximately:
 *
 *   vqrshrun_n_s32(sum, 7) is (sum + (1 << 6)) >> 7 as an arithmetic shift,
 *   i.e. the C expression verbatim, then saturated into u16. Its lower
 *   saturation at 0 is the C's `v < 0 ? 0`; its upper saturation at 65535
 *   is unreachable here (the widest sum is 32767 + 32767, giving 512), so
 *   it cannot mask a value the C would have kept.
 *
 *   vqmovn_u16 then saturates u16 -> u8 at 255, which is the C's
 *   `v > 255 ? 255`. Both clips are therefore the instruction's own
 *   saturation and not a separate min/max, which is why the two paths agree
 *   on int16 min/max input and not merely on random input.
 */
void n265_bi_avg_wh_neon(pixel *dst, int ds, const int16_t *a,
                         const int16_t *b, int w, int h)
{
    for (int y = 0; y < h; y++) {
        const int16_t *pa = a + (ptrdiff_t)y * w;
        const int16_t *pb = b + (ptrdiff_t)y * w;
        pixel *pd = dst + (ptrdiff_t)y * ds;
        int x = 0;

        /* Phase 14 item 39: the sum without the widen. vhaddq_s16 is
         * floor((a + b) / 2) with no int16 overflow, and for s = a + b
         * and r = 2^(SHIFT_BI - 1),
         *
         *     floor((s + r) / 2r) == floor((floor(s / 2) + r / 2) / r)
         *
         * because when s is odd the dropped half cannot carry an integer
         * k + r/2 across a multiple of r. The right side is the rounding
         * shift by SHIFT_BI - 1 of the halved sum, so one int16 lane does
         * what the widened pair did. */
        for (; x + 8 <= w; x += 8) {
            int16x8_t hs = vhaddq_s16(vld1q_s16(pa + x), vld1q_s16(pb + x));
#if BIT_DEPTH == 8
            /* sat_u8((h + 32) >> 6): the C's two clips are the narrow's */
            vst1_u8(pd + x, vqrshrun_n_s16(hs, MC_SHIFT_BI - 1));
#else
            int16x8_t v = vrshrq_n_s16(hs, MC_SHIFT_BI - 1);

            v = vminq_s16(vmaxq_s16(v, vdupq_n_s16(0)),
                          vdupq_n_s16(PIXEL_MAX));
            vst1q_u16(pd + x, vreinterpretq_u16_s16(v));
#endif
        }
        /* w = 4 and w = 12 leave a four-sample tail. Load and store FOUR,
         * never eight, or the guard border pays for it. */
        if (x + 4 <= w) {
            int32x4_t s = vaddl_s16(vld1_s16(pa + x), vld1_s16(pb + x));
            uint16x4_t r = vqrshrun_n_s32(s, MC_SHIFT_BI);
#if BIT_DEPTH == 8
            uint8x8_t o = vqmovn_u16(vcombine_u16(r, r));
            uint32_t four = vget_lane_u32(vreinterpret_u32_u8(o), 0);
            memcpy(pd + x, &four, 4);   /* dst has no alignment guarantee */
#else
            vst1_u16(pd + x, vmin_u16(r, vdup_n_u16(PIXEL_MAX)));
#endif
            x += 4;
        }
        /* CHROMA is why this scalar tail exists. The luma PU widths are
         * all multiples of four, but 4:2:0 halves them: an 8x4 luma PU
         * gives a 4x2 chroma block and the AMP 4x16 / 12x16 shapes give
         * 2x8 and 6x8, so w = 2 and w = 6 reach this kernel. A four-lane
         * tail at w = 2 reads and writes two samples too many, which is
         * an encode that decodes to a different picture and not a
         * theoretical hazard - it is what the CIF conformance cells
         * caught the first time this landed. */
        for (; x < w; x++) {
            int v = (pa[x] + pb[x] + (1 << (MC_SHIFT_BI - 1))) >> MC_SHIFT_BI;
            pd[x] = (pixel)(v < 0 ? 0 : v > PIXEL_MAX ? PIXEL_MAX : v);
        }
    }
}

/* ---- the interpolation taps -------------------------------------------
 *
 * ITU-T H.265 Table 8-11, the same constants src/encoder/mc.c holds. They
 * are duplicated rather than exported because the two translation units
 * are meant to be independently readable, and a typo here cannot hide: a
 * wrong tap is a wrong sample on the very first checkasm shape.
 */
static const int16_t mc_luma_filt[4][8] = {
    {  0, 0,   0, 64,  0,   0, 0,  0 },
    { -1, 4, -10, 58, 17,  -5, 1,  0 },
    { -1, 4, -11, 40, 40, -11, 4, -1 },
    {  0, 1,  -5, 17, 58, -10, 4, -1 },
};

/* ---- the cached-first-stage vertical pass ------------------------------
 *
 * Three callers run the 8-tap VERTICAL filter over int16 rows: the two
 * E1 forms over the cached `hf` plane, and the plain 2-D forms further
 * down over the horizontal temp they build first. Only the final narrow
 * differs, so the accumulation is written once.
 *
 * RANGE. The C accumulates in `int`:
 *
 *     s = sum_k fv[k] * hf[(y + k - 3) * hs + x]
 *
 * The cached values are themselves 8-tap horizontal sums of u8 samples.
 * Against Table 8-11 the widest column sums are filt[2]'s: its positive
 * taps total 88 and its negative taps 24, so a cached value lies in
 * [-24 * 255, 88 * 255] = [-6120, 22440]. Feeding those to the vertical
 * pass gives |s| <= 88 * 22440 + 24 * 6120 = 2121600, far inside int32 -
 * and even an ADVERSARIAL int16 input (which the encoder cannot produce
 * but checkasm does feed) only reaches 88 * 32767 + 24 * 32768 = 3669928,
 * still inside int32. So a lane-wise int32 accumulator holds every partial
 * sum exactly and there is nothing to prove about wrapping here.
 *
 * vmlal_n_s16 / vmlal_high_n_s16 widen s16 * s16 into those s32 lanes,
 * which is the C's own int multiply, so the accumulator agrees term for
 * term and not merely in the total.
 */
/* One 8-column by 2-ROW block. The two output rows overlap in seven of
 * their eight source rows, so nine loads serve sixteen outputs where a
 * row-at-a-time loop would take sixteen; on a 64x64 block that is 3.5k
 * loads saved. Everything else about the arithmetic is unchanged - a0/a1
 * are the low and high halves of output row y, a2/a3 of row y + 1. */
static inline void mc_vacc8x2(const int16_t *p, int hs, const int16_t *f,
                              int32x4_t a[4])
{
    int16x8_t r[9];

    #pragma GCC unroll 16
    for (int k = 0; k < 9; k++)
        r[k] = vld1q_s16(p + (ptrdiff_t)(k - 3) * hs);
    a[0] = a[1] = a[2] = a[3] = vdupq_n_s32(0);
    #pragma GCC unroll 16
    for (int k = 0; k < 8; k++) {
        a[0] = vmlal_n_s16(a[0], vget_low_s16(r[k]), f[k]);
        a[1] = vmlal_high_n_s16(a[1], r[k], f[k]);
        a[2] = vmlal_n_s16(a[2], vget_low_s16(r[k + 1]), f[k]);
        a[3] = vmlal_high_n_s16(a[3], r[k + 1], f[k]);
    }
}

/* The same for the four-sample tail: of the luma PU widths, 4 and 12 are
 * the two that leave one. FOUR lanes, never eight - an eight-lane load
 * here reaches four samples past the last column, which is harmless on a
 * padded plane and reads stack n265_ref_win never gathered on the
 * clipped-MV path. */
static inline void mc_vacc4x2(const int16_t *p, int hs, const int16_t *f,
                              int32x4_t a[2])
{
    int16x4_t r[9];

    #pragma GCC unroll 16
    for (int k = 0; k < 9; k++)
        r[k] = vld1_s16(p + (ptrdiff_t)(k - 3) * hs);
    a[0] = a[1] = vdupq_n_s32(0);
    #pragma GCC unroll 16
    for (int k = 0; k < 8; k++) {
        a[0] = vmlal_n_s16(a[0], r[k], f[k]);
        a[1] = vmlal_n_s16(a[1], r[k + 1], f[k]);
    }
}

static inline void mc_vacc8(const int16_t *p, int hs, const int16_t *f,
                            int32x4_t a[2])
{
    a[0] = a[1] = vdupq_n_s32(0);
    #pragma GCC unroll 16
    for (int k = 0; k < 8; k++) {
        int16x8_t v = vld1q_s16(p + (ptrdiff_t)(k - 3) * hs);
        a[0] = vmlal_n_s16(a[0], vget_low_s16(v), f[k]);
        a[1] = vmlal_high_n_s16(a[1], v, f[k]);
    }
}

static inline int32x4_t mc_vacc4(const int16_t *p, int hs, const int16_t *f)
{
    int32x4_t a = vdupq_n_s32(0);
    #pragma GCC unroll 16
    for (int k = 0; k < 8; k++)
        a = vmlal_n_s16(a, vld1_s16(p + (ptrdiff_t)(k - 3) * hs), f[k]);
    return a;
}

/* the two narrows, written once so the pair of kernels below cannot
 * drift apart; see the block comment above each kernel for why each one
 * is exact */
#define MC_NARROW_S16(a0, a1)                                                \
    vcombine_s16(vshrn_n_s32((a0), 6), vshrn_n_s32((a1), 6))

/* Store four bytes to an unaligned destination. `dst` is a row inside a
 * prediction buffer and carries no alignment guarantee, so the lane goes
 * out through memcpy rather than a u32 store. */
static inline void mc_store4(uint8_t *dst, uint8x8_t v)
{
    uint32_t four = vget_lane_u32(vreinterpret_u32_u8(v), 0);
    memcpy(dst, &four, 4);
}

/* The SAMPLE-side stores of the 2-D tail, clip1((s + ROUND23) >> SHIFT23)
 * from int32 accumulators. Written as macros because the two depths differ
 * in the store width as well as in the clip, and every caller below
 * wants both arms without a second copy of its loop.
 *
 * At 8 bits both clips are an instruction's own saturation - the s32
 * narrow saturates at 0 (the C's `< 0 ? 0`), the u16 -> u8 narrow at 255
 * - which is why the two paths agree at the ends of the range and not
 * only in the middle. At Main10 the lower clip is still the narrow's;
 * the upper one is PIXEL_MAX, which no narrow's own bound is, so it is a
 * vmin. The u16 saturation at 65535 cannot bite first: the widest
 * accumulator here is 3669928 and (s + 512) >> 10 of it is 3584. */
#if BIT_DEPTH == 8
#define MC_ST8_V(d, a0, a1)                                                  \
    vst1_u8((d), vqmovn_u16(vcombine_u16(vqrshrun_n_s32((a0), MC_SHIFT23),   \
                                         vqrshrun_n_s32((a1), MC_SHIFT23))))
#define MC_ST4_V(d, a) do {                                                  \
        uint16x4_t r_ = vqrshrun_n_s32((a), MC_SHIFT23);                     \
        mc_store4((d), vqmovn_u16(vcombine_u16(r_, r_)));                    \
    } while (0)
#define MC_ST2_V(d, a) do {                                                  \
        uint16x4_t r_ = vqrshrun_n_s32((a), MC_SHIFT23);                     \
        uint8x8_t o_ = vqmovn_u16(vcombine_u16(r_, r_));                     \
        uint16_t two_ = vget_lane_u16(vreinterpret_u16_u8(o_), 0);           \
        memcpy((d), &two_, 2);                                               \
    } while (0)
#else
#define MC_ST8_V(d, a0, a1)                                                  \
    vst1q_u16((d), vminq_u16(vcombine_u16(vqrshrun_n_s32((a0), MC_SHIFT23),  \
                                          vqrshrun_n_s32((a1), MC_SHIFT23)), \
                             vdupq_n_u16(PIXEL_MAX)))
#define MC_ST4_V(d, a)                                                       \
    vst1_u16((d), vmin_u16(vqrshrun_n_s32((a), MC_SHIFT23),                  \
                           vdup_n_u16(PIXEL_MAX)))
#define MC_ST2_V(d, a) do {                                                  \
        uint16x4_t r_ = vmin_u16(vqrshrun_n_s32((a), MC_SHIFT23),            \
                                 vdup_n_u16(PIXEL_MAX));                     \
        uint32_t two_ = vget_lane_u32(vreinterpret_u32_u16(r_), 0);          \
        memcpy((d), &two_, 4);                                               \
    } while (0)
#endif

/* the scalar guard both kernels end a row with. Every luma PU width is a
 * multiple of four, so this is a guard and not a path; it costs nothing
 * and it stops a future caller from silently getting a short row. */
static inline int mc_vscalar(const int16_t *p, int hs, const int16_t *f,
                             int x)
{
    int s = 0;
    for (int k = 0; k < 8; k++)
        s += f[k] * p[(ptrdiff_t)(k - 3) * hs + x];
    return s;
}

static inline pixel mc_clip1(int v)
{
    return (pixel)(v < 0 ? 0 : v > PIXEL_MAX ? PIXEL_MAX : v);
}

/* clip1((hf + 32) >> 6), and the 8-tap vertical form clip1((s + 2048) >> 12).
 *
 * NARROWS. Both are the instruction's own saturation, not a separate
 * min/max, which is why the two paths agree on adversarial input:
 *
 *   vqrshrun_n_s16(v, 6)  = sat_u8((v + 32) >> 6), arithmetic shift.
 *       That IS clip1((v + 32) >> 6): the low saturation at 0 is the C's
 *       `< 0 ? 0` and the high one at 255 is its `> 255 ? 255`.
 *
 *   vqrshrun_n_s32(s, 12) = sat_u16((s + 2048) >> 12), then vqmovn_u16
 *       saturates u16 -> u8 at 255. The intermediate u16 saturation cannot
 *       bite before the u8 one does: the largest s above is 3669928, so
 *       (s + 2048) >> 12 <= 896, well under 65535. The pair therefore
 *       computes clip1((s + 2048) >> 12) exactly.
 */
/* Four output rows by eight columns of the vertical pass, the taps a
 * constant (phase 14 item 39). Eleven row loads serve four output rows
 * where the two-row form takes nine for two, and with `fy` a constant at
 * each instance the zero tap of the quarter positions drops out and the
 * first tap is a multiply rather than an add to zero. The sum is the
 * C's: int32 lanes, exact, and in any order (RANGE above). */
#define MC_V_ATTR static inline __attribute__((always_inline))
MC_V_ATTR void mc_v4x8(const int16_t *p, int hs, const int fy,
                       int32x4_t a[8])
{
    const int16_t *f = mc_luma_filt[fy];
    int16x8_t r[11];

    #pragma GCC unroll 16
    for (int k = 0; k < 11; k++)
        r[k] = vld1q_s16(p + (ptrdiff_t)(k - 3) * hs);
    #pragma GCC unroll 16
    for (int o = 0; o < 4; o++) {
        int32x4_t lo = vdupq_n_s32(0), hi = vdupq_n_s32(0);
        int first = 1;

        #pragma GCC unroll 16
        for (int k = 0; k < 8; k++) {
            if (!f[k])
                continue;
            if (first) {
                lo = vmull_n_s16(vget_low_s16(r[o + k]), f[k]);
                hi = vmull_high_n_s16(r[o + k], f[k]);
                first = 0;
            } else {
                lo = vmlal_n_s16(lo, vget_low_s16(r[o + k]), f[k]);
                hi = vmlal_high_n_s16(hi, r[o + k], f[k]);
            }
        }
        a[2 * o] = lo;
        a[2 * o + 1] = hi;
    }
}

/* the sample-side and 14-bit-side walks over w x h, both multiples of
 * the block (8 and 4) - every luma PU shape but the 4- and 12-wide ones */
MC_V_ATTR void mc_v_pix(pixel *dst, int ds, int w, int h, const int16_t *hf,
                        int hs, const int fy)
{
    for (int y = 0; y < h; y += 4)
        for (int x = 0; x < w; x += 8) {
            int32x4_t a[8];
            pixel *d = dst + (ptrdiff_t)y * ds + x;

            mc_v4x8(hf + (ptrdiff_t)y * hs + x, hs, fy, a);
            MC_ST8_V(d, a[0], a[1]);
            MC_ST8_V(d + ds, a[2], a[3]);
            MC_ST8_V(d + 2 * ds, a[4], a[5]);
            MC_ST8_V(d + 3 * ds, a[6], a[7]);
        }
}

MC_V_ATTR void mc_v_s16(int16_t *dst, int ds, int w, int h,
                        const int16_t *hf, int hs, const int fy)
{
    for (int y = 0; y < h; y += 4)
        for (int x = 0; x < w; x += 8) {
            int32x4_t a[8];
            int16_t *d = dst + (ptrdiff_t)y * ds + x;

            mc_v4x8(hf + (ptrdiff_t)y * hs + x, hs, fy, a);
            vst1q_s16(d, MC_NARROW_S16(a[0], a[1]));
            vst1q_s16(d + ds, MC_NARROW_S16(a[2], a[3]));
            vst1q_s16(d + 2 * ds, MC_NARROW_S16(a[4], a[5]));
            vst1q_s16(d + 3 * ds, MC_NARROW_S16(a[6], a[7]));
        }
}

void n265_mc_luma_hf_neon(pixel *dst, int ds, int w, int h,
                          const int16_t *hf, int hs, int fy)
{
    if (fy && !(w & 7) && !(h & 3)) {
        switch (fy) {
        case 1:  mc_v_pix(dst, ds, w, h, hf, hs, 1); return;
        case 2:  mc_v_pix(dst, ds, w, h, hf, hs, 2); return;
        default: mc_v_pix(dst, ds, w, h, hf, hs, 3); return;
        }
    }
    if (!fy) {
        /* Sixteen at a time, not eight. The body is three instructions
         * per eight samples, so at eight the loop's own increment and
         * branch are a third of the work and the C's auto-vectorized
         * version - which clang unrolls - wins. */
        for (int y = 0; y < h; y++) {
            const int16_t *p = hf + (ptrdiff_t)y * hs;
            pixel *pd = dst + (ptrdiff_t)y * ds;
            int x = 0;
#if BIT_DEPTH == 8
            for (; x + 16 <= w; x += 16)
                vst1q_u8(pd + x,
                         vcombine_u8(vqrshrun_n_s16(vld1q_s16(p + x),
                                                    MC_SHIFT3),
                                     vqrshrun_n_s16(vld1q_s16(p + x + 8),
                                                    MC_SHIFT3)));
            for (; x + 8 <= w; x += 8)
                vst1_u8(pd + x, vqrshrun_n_s16(vld1q_s16(p + x), MC_SHIFT3));
#else
            /* clip1((hf + 8) >> 4). vrshrq_n_s16 IS that rounding shift,
             * in the lane, and the clip is the max/min pair because
             * neither bound is a narrow's own at this width. The cached
             * value is an int16 either way, so nothing widens. */
            for (; x + 8 <= w; x += 8) {
                int16x8_t t = vrshrq_n_s16(vld1q_s16(p + x), MC_SHIFT3);

                t = vminq_s16(vmaxq_s16(t, vdupq_n_s16(0)),
                              vdupq_n_s16(PIXEL_MAX));
                vst1q_u16(pd + x, vreinterpretq_u16_s16(t));
            }
#endif
            if (x + 4 <= w) {
                /* The four-lane tail goes through the s32 narrow, not
                 * vqrshrun_n_s16 on a vcombine_s16(v, v). The s16 form
                 * reads well - duplicate four lanes, narrow eight, keep
                 * four - and it COMPILES to `ldr q`: clang sees the upper
                 * four lanes of the duplicate feed a dead half of the
                 * store, rewrites the duplicate as a plain 16-byte load,
                 * and the kernel then reads four samples past the row.
                 * That is invisible to a value-comparing test (the bytes
                 * are discarded), invisible to the sanitizer (they are
                 * inside the allocation), and a genuine out-of-bounds
                 * read on n265_ref_win's gathered tmp. Here every lane
                 * that reaches the store is live, so there is no dead
                 * half to widen into. vqrshrun_n_s32(v, 6) is
                 * sat_u16((v + 32) >> 6) and vqmovn_u16 caps at 255,
                 * which is the same clip1((v + 32) >> 6). */
#if BIT_DEPTH == 8
                int32x4_t v = vmovl_s16(vld1_s16(p + x));
                uint16x4_t r = vqrshrun_n_s32(v, MC_SHIFT3);
                mc_store4(pd + x, vqmovn_u16(vcombine_u16(r, r)));
#else
                /* four lanes loaded, four stored: at this width the
                 * four-sample load IS a d-register and there is no dead
                 * half for clang to widen into */
                int16x4_t t = vrshr_n_s16(vld1_s16(p + x), MC_SHIFT3);

                t = vmin_s16(vmax_s16(t, vdup_n_s16(0)),
                             vdup_n_s16(PIXEL_MAX));
                vst1_u16(pd + x, vreinterpret_u16_s16(t));
#endif
                x += 4;
            }
            for (; x < w; x++)
                pd[x] = mc_clip1((p[x] + MC_ROUND3) >> MC_SHIFT3);
        }
        return;
    }

    const int16_t *f = mc_luma_filt[fy];
    int y = 0;
    for (; y + 2 <= h; y += 2) {
        const int16_t *p = hf + (ptrdiff_t)y * hs;
        pixel *d0 = dst + (ptrdiff_t)y * ds, *d1 = d0 + ds;
        int x = 0;
        for (; x + 8 <= w; x += 8) {
            int32x4_t a[4];
            mc_vacc8x2(p + x, hs, f, a);
            MC_ST8_V(d0 + x, a[0], a[1]);
            MC_ST8_V(d1 + x, a[2], a[3]);
        }
        if (x + 4 <= w) {
            int32x4_t a[2];
            mc_vacc4x2(p + x, hs, f, a);
            MC_ST4_V(d0 + x, a[0]);
            MC_ST4_V(d1 + x, a[1]);
            x += 4;
        }
        for (; x < w; x++) {
            d0[x] = mc_clip1((mc_vscalar(p, hs, f, x) + MC_ROUND23)
                             >> MC_SHIFT23);
            d1[x] = mc_clip1((mc_vscalar(p + hs, hs, f, x) + MC_ROUND23)
                             >> MC_SHIFT23);
        }
    }
    /* every luma PU height is even, so this is the same kind of guard as
     * the scalar column tail */
    for (; y < h; y++) {
        const int16_t *p = hf + (ptrdiff_t)y * hs;
        pixel *pd = dst + (ptrdiff_t)y * ds;
        int x = 0;
        for (; x + 8 <= w; x += 8) {
            int32x4_t a[2];
            mc_vacc8(p + x, hs, f, a);
            MC_ST8_V(pd + x, a[0], a[1]);
        }
        if (x + 4 <= w) {
            MC_ST4_V(pd + x, mc_vacc4(p + x, hs, f));
            x += 4;
        }
        for (; x < w; x++)
            pd[x] = mc_clip1((mc_vscalar(p, hs, f, x) + MC_ROUND23)
                             >> MC_SHIFT23);
    }
}

/* The 14-bit twin. fy == 0 is a plain copy of the cached row; fy != 0 is
 * the same accumulator narrowed by `(int16_t)(s >> 6)`.
 *
 * That cast MATTERS and vshrn_n_s32 is the reason this is exact rather
 * than approximately exact. s >> 6 does NOT always fit int16: at the
 * adversarial end above, 3669928 >> 6 = 57342, and the C's conversion to
 * int16_t wraps modulo 2^16 (the compiler's documented behavior for the
 * out-of-range case). vshrn_n_s32 is "shift right narrow": it keeps the
 * LOW 16 bits of the arithmetic shift and does not saturate, so it
 * reproduces the wrap. vqshrn_n_s32 would clamp to 32767 and disagree.
 * On the values the encoder really produces (|s| <= 2121600, so
 * s >> 6 <= 33150) the wrap is reachable too, which is why this is not a
 * theoretical distinction - and a deliberately saturating narrow was put
 * in to confirm checkasm catches it.
 */
void n265_mc_luma_14_hf_neon(int16_t *dst, int ds, int w, int h,
                             const int16_t *hf, int hs, int fy)
{
    if (fy && !(w & 7) && !(h & 3)) {
        switch (fy) {
        case 1:  mc_v_s16(dst, ds, w, h, hf, hs, 1); return;
        case 2:  mc_v_s16(dst, ds, w, h, hf, hs, 2); return;
        default: mc_v_s16(dst, ds, w, h, hf, hs, 3); return;
        }
    }
    if (!fy) {
        /* An explicit vector copy, not memcpy per row: at w = 8 the call
         * is not inlined and the copy arm loses to the C outright. */
        for (int y = 0; y < h; y++) {
            const int16_t *p = hf + (ptrdiff_t)y * hs;
            int16_t *pd = dst + (ptrdiff_t)y * ds;
            int x = 0;
            for (; x + 32 <= w; x += 32) {
                int16x8_t a = vld1q_s16(p + x), b = vld1q_s16(p + x + 8);
                int16x8_t c = vld1q_s16(p + x + 16);
                int16x8_t d = vld1q_s16(p + x + 24);
                vst1q_s16(pd + x, a);
                vst1q_s16(pd + x + 8, b);
                vst1q_s16(pd + x + 16, c);
                vst1q_s16(pd + x + 24, d);
            }
            for (; x + 16 <= w; x += 16) {
                vst1q_s16(pd + x, vld1q_s16(p + x));
                vst1q_s16(pd + x + 8, vld1q_s16(p + x + 8));
            }
            for (; x + 8 <= w; x += 8)
                vst1q_s16(pd + x, vld1q_s16(p + x));
            if (x + 4 <= w) {
                vst1_s16(pd + x, vld1_s16(p + x));
                x += 4;
            }
            for (; x < w; x++)
                pd[x] = p[x];
        }
        return;
    }

    const int16_t *f = mc_luma_filt[fy];
    int y = 0;
    for (; y + 2 <= h; y += 2) {
        const int16_t *p = hf + (ptrdiff_t)y * hs;
        int16_t *d0 = dst + (ptrdiff_t)y * ds, *d1 = d0 + ds;
        int x = 0;
        for (; x + 8 <= w; x += 8) {
            int32x4_t a[4];
            mc_vacc8x2(p + x, hs, f, a);
            vst1q_s16(d0 + x, MC_NARROW_S16(a[0], a[1]));
            vst1q_s16(d1 + x, MC_NARROW_S16(a[2], a[3]));
        }
        if (x + 4 <= w) {
            int32x4_t a[2];
            mc_vacc4x2(p + x, hs, f, a);
            vst1_s16(d0 + x, vshrn_n_s32(a[0], 6));
            vst1_s16(d1 + x, vshrn_n_s32(a[1], 6));
            x += 4;
        }
        for (; x < w; x++) {
            d0[x] = (int16_t)(mc_vscalar(p, hs, f, x) >> 6);
            d1[x] = (int16_t)(mc_vscalar(p + hs, hs, f, x) >> 6);
        }
    }
    for (; y < h; y++) {
        const int16_t *p = hf + (ptrdiff_t)y * hs;
        int16_t *pd = dst + (ptrdiff_t)y * ds;
        int x = 0;
        for (; x + 8 <= w; x += 8) {
            int32x4_t a[2];
            mc_vacc8(p + x, hs, f, a);
            vst1q_s16(pd + x, MC_NARROW_S16(a[0], a[1]));
        }
        if (x + 4 <= w) {
            vst1_s16(pd + x, vshrn_n_s32(mc_vacc4(p + x, hs, f), 6));
            x += 4;
        }
        for (; x < w; x++)
            pd[x] = (int16_t)(mc_vscalar(p, hs, f, x) >> 6);
    }
}

/* The sign pattern of Table 8-11, which is the same for all three
 * luma phases and is what lets one kernel serve every phase at
 * either depth. */
static const int mc_tap_pos[4] = { 1, 3, 4, 6 };
static const int mc_tap_neg[4] = { 0, 2, 5, 7 };

#if BIT_DEPTH == 8
/* ---- the u8 stages: horizontal, vertical, and the 2-D pair ------------
 *
 * The 8-tap over u8 samples is done in u16 lanes, not s16: the taps are
 * split by SIGN into two unsigned accumulators and subtracted at the end.
 *
 * That works without a per-tap branch and without a per-phase kernel
 * because the sign pattern of Table 8-11 is the SAME for all three luma
 * phases - `- + - + + - + -` - and the two taps that are zero (fx = 1's
 * last, fx = 3's first) contribute zero to whichever accumulator the
 * pattern puts them in. One function therefore serves every phase, with
 * the magnitudes carried in registers.
 *
 * RANGE. The positive taps of the widest phase (filt[2]) total 88 and the
 * negative ones 24, so the two accumulators reach 88 * 255 = 22440 and
 * 24 * 255 = 6120: both fit u16 with room, and neither can wrap. Their
 * difference lies in [-6120, 22440], which fits int16 exactly, and
 * vsubq_u16 is modular, so reinterpreting the u16 difference as s16 gives
 * the two's-complement value the C's `int` accumulator holds. This is the
 * one place the kernel leans on modular lane arithmetic, and it is exact
 * because the FINAL value fits the lane even where a partial sum would
 * not have.
 */

/* the eight tap magnitudes, splatted once per call rather than once per
 * eight samples */
typedef struct { uint8x16_t q[8]; } mc_taps;

static inline void mc_taps_init(mc_taps *t, const int16_t *f)
{
    for (int k = 0; k < 8; k++)
        t->q[k] = vdupq_n_u8((uint8_t)(f[k] < 0 ? -f[k] : f[k]));
}

/* exactly four bytes, splatted. Never vld1_u8 for a four-sample tail:
 * that reads eight and the four it throws away are past the block. */
static inline uint8x8_t mc_ld4(const uint8_t *p)
{
    uint32_t v;
    memcpy(&v, p, 4);
    return vreinterpret_u8_u32(vdup_n_u32(v));
}

/* and exactly two, for the 2- and 6-wide chroma blocks. Four would be two
 * samples too many at the last tap offset in both passes. */
static inline uint8x8_t mc_ld2(const uint8_t *p)
{
    uint16_t v;
    memcpy(&v, p, 2);
    return vreinterpret_u8_u16(vdup_n_u16(v));
}

/* The tap loop, shared by the four one-dimensional helpers below. LD is
 * the load width and OFF turns a tap index into an offset, which is what
 * makes one body serve the horizontal (columns) and the vertical (rows)
 * pass; `p`, `t`, `P` and `N` come from the enclosing function, which is
 * the whole reason this is a macro and not another inline. */
#define MC_HSUM(LD, OFF)                                                     \
    do {                                                                     \
        for (int i = 0; i < 4; i++) {                                        \
            int kp = mc_tap_pos[i], kn = mc_tap_neg[i];                      \
            uint8x8_t vp = LD(p + OFF(kp));                                  \
            uint8x8_t vn = LD(p + OFF(kn));                                  \
            P = vmlal_u8(P, vp, vget_low_u8(t->q[kp]));                      \
            N = vmlal_u8(N, vn, vget_low_u8(t->q[kn]));                      \
        }                                                                    \
    } while (0)

#define MC_HCOL(k) (k)
#define MC_VROW(k) ((ptrdiff_t)((k) - 3) * rs)

static inline int16x8_t mc_h8(const uint8_t *p, const mc_taps *t)
{
    uint16x8_t P = vdupq_n_u16(0), N = vdupq_n_u16(0);
    MC_HSUM(vld1_u8, MC_HCOL);
    return vreinterpretq_s16_u16(vsubq_u16(P, N));
}

static inline int16x4_t mc_h4(const uint8_t *p, const mc_taps *t)
{
    uint16x8_t P = vdupq_n_u16(0), N = vdupq_n_u16(0);
    MC_HSUM(mc_ld4, MC_HCOL);
    return vreinterpret_s16_u16(vsub_u16(vget_low_u16(P), vget_low_u16(N)));
}

static inline void mc_h16(const uint8_t *p, const mc_taps *t, int16x8_t *lo,
                          int16x8_t *hi)
{
    uint16x8_t PL = vdupq_n_u16(0), NL = vdupq_n_u16(0);
    uint16x8_t PH = vdupq_n_u16(0), NH = vdupq_n_u16(0);

    for (int i = 0; i < 4; i++) {
        int kp = mc_tap_pos[i], kn = mc_tap_neg[i];
        uint8x16_t vp = vld1q_u8(p + kp), vn = vld1q_u8(p + kn);
        PL = vmlal_u8(PL, vget_low_u8(vp), vget_low_u8(t->q[kp]));
        PH = vmlal_high_u8(PH, vp, t->q[kp]);
        NL = vmlal_u8(NL, vget_low_u8(vn), vget_low_u8(t->q[kn]));
        NH = vmlal_high_u8(NH, vn, t->q[kn]);
    }
    *lo = vreinterpretq_s16_u16(vsubq_u16(PL, NL));
    *hi = vreinterpretq_s16_u16(vsubq_u16(PH, NH));
}

static inline int16x8_t mc_v8(const uint8_t *p, int rs, const mc_taps *t)
{
    uint16x8_t P = vdupq_n_u16(0), N = vdupq_n_u16(0);
    MC_HSUM(vld1_u8, MC_VROW);
    return vreinterpretq_s16_u16(vsubq_u16(P, N));
}

static inline int16x4_t mc_v4(const uint8_t *p, int rs, const mc_taps *t)
{
    uint16x8_t P = vdupq_n_u16(0), N = vdupq_n_u16(0);
    MC_HSUM(mc_ld4, MC_VROW);
    return vreinterpret_s16_u16(vsub_u16(vget_low_u16(P), vget_low_u16(N)));
}

static inline void mc_v16(const uint8_t *p, int rs, const mc_taps *t,
                          int16x8_t *lo, int16x8_t *hi)
{
    uint16x8_t PL = vdupq_n_u16(0), NL = vdupq_n_u16(0);
    uint16x8_t PH = vdupq_n_u16(0), NH = vdupq_n_u16(0);

    for (int i = 0; i < 4; i++) {
        int kp = mc_tap_pos[i], kn = mc_tap_neg[i];
        uint8x16_t vp = vld1q_u8(p + MC_VROW(kp));
        uint8x16_t vn = vld1q_u8(p + MC_VROW(kn));
        PL = vmlal_u8(PL, vget_low_u8(vp), vget_low_u8(t->q[kp]));
        PH = vmlal_high_u8(PH, vp, t->q[kp]);
        NL = vmlal_u8(NL, vget_low_u8(vn), vget_low_u8(t->q[kn]));
        NH = vmlal_high_u8(NH, vn, t->q[kn]);
    }
    *lo = vreinterpretq_s16_u16(vsubq_u16(PL, NL));
    *hi = vreinterpretq_s16_u16(vsubq_u16(PH, NH));
}

static inline int mc_hscalar(const uint8_t *p, const int16_t *f)
{
    int s = 0;
    for (int k = 0; k < 8; k++)
        s += f[k] * p[k];
    return s;
}

static inline int mc_vscalar_u8(const uint8_t *p, int rs, const int16_t *f)
{
    int s = 0;
    for (int k = 0; k < 8; k++)
        s += f[k] * p[(ptrdiff_t)(k - 3) * rs];
    return s;
}

/* The four one-dimensional drivers take a whole BLOCK, not a row. The
 * row loop has to be inside them: with a row-at-a-time helper clang
 * stops inlining once the translation unit grows, and the out-of-line
 * form reloads and re-splats the eight tap magnitudes on every row - the
 * 14-bit 2-D form measured 0.50x against the C that way and 1.04x with
 * the loop moved in. One call per block, taps splatted once. */

/* the horizontal stage, int16 out and unrounded: the value
 * n265_mc_luma_hplane caches and the value the 2-D temp holds */
static void mc_hblock_s16(int16_t *d, int ds, const uint8_t *p, int rs,
                          int w, int h, const mc_taps *t, const int16_t *f)
{
  for (int y = 0; y < h; y++, d += ds, p += rs) {
    int x = 0;
    for (; x + 16 <= w; x += 16) {
        int16x8_t lo, hi;
        mc_h16(p + x - 3, t, &lo, &hi);
        vst1q_s16(d + x, lo);
        vst1q_s16(d + x + 8, hi);
    }
    for (; x + 8 <= w; x += 8)
        vst1q_s16(d + x, mc_h8(p + x - 3, t));
    if (x + 4 <= w) {
        vst1_s16(d + x, mc_h4(p + x - 3, t));
        x += 4;
    }
    for (; x < w; x++)
        d[x] = (int16_t)mc_hscalar(p + x - 3, f);
  }
}

/* the same rounded and clipped: clip1((s + 32) >> 6). vqrshrun_n_s16
 * is sat_u8((s + 32) >> 6) - the arithmetic shift with the rounding term
 * the C writes, and both of the C's clips as the instruction's own
 * saturation rather than a separate min/max, which is what makes the two
 * agree at the ends of the range and not only in the middle. */
static void mc_hblock_pix(uint8_t *d, int ds, const uint8_t *p, int rs,
                         int w, int h, const mc_taps *t, const int16_t *f)
{
  for (int y = 0; y < h; y++, d += ds, p += rs) {
    int x = 0;
    for (; x + 16 <= w; x += 16) {
        int16x8_t lo, hi;
        mc_h16(p + x - 3, t, &lo, &hi);
        vst1q_u8(d + x, vcombine_u8(vqrshrun_n_s16(lo, 6),
                                    vqrshrun_n_s16(hi, 6)));
    }
    for (; x + 8 <= w; x += 8)
        vst1_u8(d + x, vqrshrun_n_s16(mc_h8(p + x - 3, t), 6));
    if (x + 4 <= w) {
        int32x4_t v = vmovl_s16(mc_h4(p + x - 3, t));
        uint16x4_t r = vqrshrun_n_s32(v, 6);
        mc_store4(d + x, vqmovn_u16(vcombine_u16(r, r)));
        x += 4;
    }
    for (; x < w; x++)
        d[x] = mc_clip1((mc_hscalar(p + x - 3, f) + 32) >> 6);
  }
}

static void mc_vblock_s16(int16_t *d, int ds, const uint8_t *p, int rs,
                          int w, int h, const mc_taps *t, const int16_t *f)
{
  for (int y = 0; y < h; y++, d += ds, p += rs) {
    int x = 0;
    for (; x + 16 <= w; x += 16) {
        int16x8_t lo, hi;
        mc_v16(p + x, rs, t, &lo, &hi);
        vst1q_s16(d + x, lo);
        vst1q_s16(d + x + 8, hi);
    }
    for (; x + 8 <= w; x += 8)
        vst1q_s16(d + x, mc_v8(p + x, rs, t));
    if (x + 4 <= w) {
        vst1_s16(d + x, mc_v4(p + x, rs, t));
        x += 4;
    }
    for (; x < w; x++)
        d[x] = (int16_t)mc_vscalar_u8(p + x, rs, f);
  }
}

static void mc_vblock_pix(uint8_t *d, int ds, const uint8_t *p, int rs,
                         int w, int h, const mc_taps *t, const int16_t *f)
{
  for (int y = 0; y < h; y++, d += ds, p += rs) {
    int x = 0;
    for (; x + 16 <= w; x += 16) {
        int16x8_t lo, hi;
        mc_v16(p + x, rs, t, &lo, &hi);
        vst1q_u8(d + x, vcombine_u8(vqrshrun_n_s16(lo, 6),
                                    vqrshrun_n_s16(hi, 6)));
    }
    for (; x + 8 <= w; x += 8)
        vst1_u8(d + x, vqrshrun_n_s16(mc_v8(p + x, rs, t), 6));
    if (x + 4 <= w) {
        int32x4_t v = vmovl_s16(mc_v4(p + x, rs, t));
        uint16x4_t r = vqrshrun_n_s32(v, 6);
        mc_store4(d + x, vqmovn_u16(vcombine_u16(r, r)));
        x += 4;
    }
    for (; x < w; x++)
        d[x] = mc_clip1((mc_vscalar_u8(p + x, rs, f) + 32) >> 6);
  }
}

/* The full-pel arm. An explicit vector loop, not memcpy per row: the call
 * costs more than the copy at these row lengths (measured 0.56x against
 * the C at 16x16 with memcpy, 1.27x with this). */
static inline void mc_copy_pix(uint8_t *d, int ds, const uint8_t *p, int rs,
                              int w, int h)
{
    for (int y = 0; y < h; y++, d += ds, p += rs) {
        int x = 0;
        for (; x + 16 <= w; x += 16)
            vst1q_u8(d + x, vld1q_u8(p + x));
        for (; x + 8 <= w; x += 8)
            vst1_u8(d + x, vld1_u8(p + x));
        if (x + 4 <= w) {
            uint32_t four;
            memcpy(&four, p + x, 4);
            memcpy(d + x, &four, 4);
            x += 4;
        }
        for (; x < w; x++)
            d[x] = p[x];
    }
}

/* the 14-bit full-pel arm: (int16_t)(sample << 6). 255 << 6 = 16320, so
 * vshll_n_u8(v, 6) - which widens to u16 and shifts - lands inside int16
 * and the reinterpret is the C's cast with nothing to truncate. */
static inline void mc_copy_s16_shl6(int16_t *d, int ds, const uint8_t *p,
                                    int rs, int w, int h)
{
    for (int y = 0; y < h; y++, d += ds, p += rs) {
        int x = 0;
        for (; x + 16 <= w; x += 16) {
            uint8x16_t v = vld1q_u8(p + x);
            vst1q_s16(d + x, vreinterpretq_s16_u16(
                                 vshll_n_u8(vget_low_u8(v), 6)));
            vst1q_s16(d + x + 8, vreinterpretq_s16_u16(
                                     vshll_high_n_u8(v, 6)));
        }
        for (; x + 8 <= w; x += 8)
            vst1q_s16(d + x,
                      vreinterpretq_s16_u16(vshll_n_u8(vld1_u8(p + x), 6)));
        if (x + 4 <= w) {
            uint16x8_t v = vshll_n_u8(mc_ld4(p + x), 6);
            vst1_s16(d + x, vreinterpret_s16_u16(vget_low_u16(v)));
            x += 4;
        }
        for (; x < w; x++)
            d[x] = (int16_t)(p[x] << 6);
    }
}

#else   /* Main10: the same three stages, one lane width up ------------- */

/* THE ACCUMULATOR IS THE WHOLE DIFFERENCE. A horizontal 8-tap over
 * 10-bit samples reaches 88 * 1023 = 90024 on the positive taps and
 * 24 * 1023 = 24552 on the negative ones, so the sign-split trick that
 * carries the 8-bit stage in u16 lanes carries this one in u32 lanes:
 * vmlal_u16 instead of vmlal_u8, four outputs to a vector instead of
 * eight. The difference lies in [-24552, 90024], which int32 holds with
 * a factor of 20000 to spare, so nothing here leans on modular
 * arithmetic the way the 8-bit u16 subtraction does - the value simply
 * fits.
 *
 * That is the cost of the depth in this family: two vectors where the
 * 8-bit stage has one, and a narrow on the way out instead of a store.
 * Everything downstream - the int16 intermediate, the vertical pass over
 * it, and both of its narrows - is unchanged, because the intermediate
 * is the SPEC's 14-bit value at both depths (shift1 sheds exactly the
 * extra sample bits).
 */
typedef struct { uint16x8_t q[8]; } mc_taps;

static inline void mc_taps_init(mc_taps *t, const int16_t *f)
{
    for (int k = 0; k < 8; k++)
        t->q[k] = vdupq_n_u16((uint16_t)(f[k] < 0 ? -f[k] : f[k]));
}

/* exactly four samples (a d-register) and exactly two (four bytes). At
 * this width vld1_u16 IS the four-sample load, so only the two-sample
 * form needs the memcpy spelling. */
static inline uint16x4_t mc_ld2_16(const pixel *p)
{
    uint32_t v;

    memcpy(&v, p, 4);
    return vreinterpret_u16_u32(vdup_n_u32(v));
}

#define MC_HSUM16(OFF)                                                       \
    do {                                                                     \
        for (int i = 0; i < 4; i++) {                                        \
            int kp = mc_tap_pos[i], kn = mc_tap_neg[i];                      \
            uint16x8_t vp = vld1q_u16(p + OFF(kp));                          \
            uint16x8_t vn = vld1q_u16(p + OFF(kn));                          \
            PL = vmlal_u16(PL, vget_low_u16(vp), vget_low_u16(t->q[kp]));    \
            PH = vmlal_high_u16(PH, vp, t->q[kp]);                           \
            NL = vmlal_u16(NL, vget_low_u16(vn), vget_low_u16(t->q[kn]));    \
            NH = vmlal_high_u16(NH, vn, t->q[kn]);                           \
        }                                                                    \
        *lo = vreinterpretq_s32_u32(vsubq_u32(PL, NL));                      \
        *hi = vreinterpretq_s32_u32(vsubq_u32(PH, NH));                      \
    } while (0)

#define MC_HSUM16_4(LD, OFF)                                                 \
    do {                                                                     \
        for (int i = 0; i < 4; i++) {                                        \
            int kp = mc_tap_pos[i], kn = mc_tap_neg[i];                      \
            P = vmlal_u16(P, LD(p + OFF(kp)), vget_low_u16(t->q[kp]));       \
            N = vmlal_u16(N, LD(p + OFF(kn)), vget_low_u16(t->q[kn]));       \
        }                                                                    \
        return vreinterpretq_s32_u32(vsubq_u32(P, N));                       \
    } while (0)

#define MC_HCOL(k) (k)
#define MC_VROW(k) ((ptrdiff_t)((k) - 3) * rs)

static inline void mc_h8_32(const pixel *p, const mc_taps *t,
                            int32x4_t *lo, int32x4_t *hi)
{
    uint32x4_t PL = vdupq_n_u32(0), PH = PL, NL = PL, NH = PL;
    MC_HSUM16(MC_HCOL);
}

static inline int32x4_t mc_h4_32(const pixel *p, const mc_taps *t)
{
    uint32x4_t P = vdupq_n_u32(0), N = P;
    MC_HSUM16_4(vld1_u16, MC_HCOL);
}

static inline void mc_v8_32(const pixel *p, int rs, const mc_taps *t,
                            int32x4_t *lo, int32x4_t *hi)
{
    uint32x4_t PL = vdupq_n_u32(0), PH = PL, NL = PL, NH = PL;
    MC_HSUM16(MC_VROW);
}

static inline int32x4_t mc_v4_32(const pixel *p, int rs, const mc_taps *t)
{
    uint32x4_t P = vdupq_n_u32(0), N = P;
    MC_HSUM16_4(vld1_u16, MC_VROW);
}

static inline int mc_hscalar(const pixel *p, const int16_t *f)
{
    int s = 0;
    for (int k = 0; k < 8; k++)
        s += f[k] * p[k];
    return s;
}

static inline int mc_vscalar_u8(const pixel *p, int rs, const int16_t *f)
{
    int s = 0;
    for (int k = 0; k < 8; k++)
        s += f[k] * p[(ptrdiff_t)(k - 3) * rs];
    return s;
}

/* the four drivers, block at a time for the reason the 8-bit ones are:
 * a row-at-a-time helper stops inlining and re-splats the taps per row */
#define MC_BLOCK16(GET8, GET4, SCALAR, ST8, ST4, STS)                        \
  do {                                                                       \
    for (int y = 0; y < h; y++, d += ds, p += rs) {                          \
        int x = 0;                                                           \
        for (; x + 8 <= w; x += 8) {                                         \
            int32x4_t lo, hi;                                                \
            GET8;                                                            \
            ST8;                                                             \
        }                                                                    \
        if (x + 4 <= w) {                                                    \
            int32x4_t v = GET4;                                              \
            ST4;                                                             \
            x += 4;                                                          \
        }                                                                    \
        for (; x < w; x++) {                                                 \
            int s = SCALAR;                                                  \
            STS;                                                             \
        }                                                                    \
    }                                                                        \
  } while (0)

/* the horizontal stage, int16 out: the SPEC's 14-bit intermediate,
 * (s >> shift1), which is what n265_mc_luma_hplane caches and what the
 * 2-D temp holds. vshrn_n_s32 is the arithmetic shift and the narrow in
 * one, and the value provably fits int16: 90024 >> 2 = 22506. */
static void mc_hblock_s16(int16_t *d, int ds, const pixel *p, int rs,
                          int w, int h, const mc_taps *t, const int16_t *f)
{
    MC_BLOCK16(mc_h8_32(p + x - 3, t, &lo, &hi), mc_h4_32(p + x - 3, t),
               mc_hscalar(p + x - 3, f),
               vst1q_s16(d + x, vcombine_s16(vshrn_n_s32(lo, MC_SHIFT1),
                                             vshrn_n_s32(hi, MC_SHIFT1))),
               vst1_s16(d + x, vshrn_n_s32(v, MC_SHIFT1)),
               d[x] = (int16_t)(s >> MC_SHIFT1));
}

/* the same rounded and clipped: clip1((s + 32) >> 6), the fold that is
 * depth-invariant (shift1 + shift3 = 6 at every depth) */
static void mc_hblock_pix(pixel *d, int ds, const pixel *p, int rs,
                          int w, int h, const mc_taps *t, const int16_t *f)
{
    MC_BLOCK16(mc_h8_32(p + x - 3, t, &lo, &hi), mc_h4_32(p + x - 3, t),
               mc_hscalar(p + x - 3, f),
               vst1q_u16(d + x, vminq_u16(
                   vcombine_u16(vqrshrun_n_s32(lo, 6),
                                vqrshrun_n_s32(hi, 6)),
                   vdupq_n_u16(PIXEL_MAX))),
               vst1_u16(d + x, vmin_u16(vqrshrun_n_s32(v, 6),
                                        vdup_n_u16(PIXEL_MAX))),
               d[x] = mc_clip1((s + 32) >> 6));
}

static void mc_vblock_s16(int16_t *d, int ds, const pixel *p, int rs,
                          int w, int h, const mc_taps *t, const int16_t *f)
{
    MC_BLOCK16(mc_v8_32(p + x, rs, t, &lo, &hi), mc_v4_32(p + x, rs, t),
               mc_vscalar_u8(p + x, rs, f),
               vst1q_s16(d + x, vcombine_s16(vshrn_n_s32(lo, MC_SHIFT1),
                                             vshrn_n_s32(hi, MC_SHIFT1))),
               vst1_s16(d + x, vshrn_n_s32(v, MC_SHIFT1)),
               d[x] = (int16_t)(s >> MC_SHIFT1));
}

static void mc_vblock_pix(pixel *d, int ds, const pixel *p, int rs,
                          int w, int h, const mc_taps *t, const int16_t *f)
{
    MC_BLOCK16(mc_v8_32(p + x, rs, t, &lo, &hi), mc_v4_32(p + x, rs, t),
               mc_vscalar_u8(p + x, rs, f),
               vst1q_u16(d + x, vminq_u16(
                   vcombine_u16(vqrshrun_n_s32(lo, 6),
                                vqrshrun_n_s32(hi, 6)),
                   vdupq_n_u16(PIXEL_MAX))),
               vst1_u16(d + x, vmin_u16(vqrshrun_n_s32(v, 6),
                                        vdup_n_u16(PIXEL_MAX))),
               d[x] = mc_clip1((s + 32) >> 6));
}

/* The full-pel arm, and at this width it is a memcpy per row rather than
 * a vector loop - the opposite of the 8-bit form's choice, and measured
 * both ways: a 10-bit row is twice the bytes, so the eight-sample loop
 * runs twice as many iterations and reads 0.56x against the C at 32x32,
 * where memcpy of the same row reads 1.4x. (The 8-bit comment records
 * the same experiment coming out the other way at its width; the C this
 * is held to is a row loop clang turns into a wide copy either way.) */
static inline void mc_copy_pix(pixel *d, int ds, const pixel *p, int rs,
                               int w, int h)
{
    for (int y = 0; y < h; y++, d += ds, p += rs)
        for (int x = 0; x < w; x++)
            d[x] = p[x];
}

/* The 14-bit full-pel arm: (int16_t)(sample << shift3), 1023 << 4 =
 * 16368, so the shifted sample lands inside int16 with nothing to
 * truncate - the same claim the 8-bit form makes about 255 << 6.
 *
 * BOTH full-pel arms are written as the C loop at Main10, and that is a
 * measured choice rather than laziness: at this sample width the arm is
 * memory-bound, and nothing hand-written beats what clang emits for a
 * row copy. The hand-vectorized form read 1.3-1.85x at 4 and 16 wide and
 * 0.56-0.59x at 32 and 64, a memcpy per row read 0.63-0.78x everywhere,
 * and the plain loop reads 0.99-1.22x across the whole shape set. The
 * arm stays in the kernel so the dispatch is still one branch and the
 * caller never has to know which shapes are worth it. */
static inline void mc_copy_s16_shl6(int16_t *d, int ds, const pixel *p,
                                    int rs, int w, int h)
{
    for (int y = 0; y < h; y++, d += ds, p += rs)
        for (int x = 0; x < w; x++)
            d[x] = (int16_t)(p[x] << MC_SHIFT3);
}

#endif  /* BIT_DEPTH */

/* The 2-D form: the horizontal stage into an int16 temp of h + 7 rows,
 * then the vertical stage of the cached-first-stage kernels above over
 * exactly those rows. Same temp shape and same stride (w) as the C, so
 * the intermediate values are the identical int16 the C stores and the
 * only thing left to match is the narrow. */
#define MC_TMP_ROWS (64 + 7)

/* The 2-D drivers hold a (64 + 7) x 64 int16 temp - 9 KB of stack - and
 * at Main10 clang inlines them into the entry point and then spills
 * around it: measured on the checkasm bench, luma_14_wh 16x16 f22 reads
 * 0.70x inlined and 1.26x out of line, and 32x32 0.88x against 1.33x.
 * The 8-bit build does not show it and keeps the attribute off, because
 * this file's 8-bit half may not move. */
#if BIT_DEPTH == 8
#define MC_2D_ATTR
#else
#define MC_2D_ATTR __attribute__((noinline))
#endif

static MC_2D_ATTR void mc_luma_2d_u8(pixel *dst, int ds, int w, int h,
                          const pixel *ref, int rs, int fx, int fy)
{
    int16_t tmp[MC_TMP_ROWS * 64];
    const int16_t *fh = mc_luma_filt[fx];
    mc_taps th;

    mc_taps_init(&th, fh);
    mc_hblock_s16(tmp, w, ref - (ptrdiff_t)3 * rs, rs, w, h + 7, &th, fh);
    n265_mc_luma_hf_neon(dst, ds, w, h, tmp + (ptrdiff_t)3 * w, w, fy);
}

static MC_2D_ATTR void mc_luma_2d_s16(int16_t *dst, int ds, int w, int h,
                           const pixel *ref, int rs, int fx, int fy)
{
    int16_t tmp[MC_TMP_ROWS * 64];
    const int16_t *fh = mc_luma_filt[fx];
    mc_taps th;

    mc_taps_init(&th, fh);
    mc_hblock_s16(tmp, w, ref - (ptrdiff_t)3 * rs, rs, w, h + 7, &th, fh);
    n265_mc_luma_14_hf_neon(dst, ds, w, h, tmp + (ptrdiff_t)3 * w, w, fy);
}

void n265_mc_luma_wh_neon(pixel *dst, int ds, int w, int h,
                          const pixel *ref, int rs, int fx, int fy)
{
    if (!fx && !fy) {
        mc_copy_pix(dst, ds, ref, rs, w, h);
        return;
    }
    if (!fy) {
        const int16_t *f = mc_luma_filt[fx];
        mc_taps t;
        mc_taps_init(&t, f);
        mc_hblock_pix(dst, ds, ref, rs, w, h, &t, f);
        return;
    }
    if (!fx) {
        const int16_t *f = mc_luma_filt[fy];
        mc_taps t;
        mc_taps_init(&t, f);
        mc_vblock_pix(dst, ds, ref, rs, w, h, &t, f);
        return;
    }
    mc_luma_2d_u8(dst, ds, w, h, ref, rs, fx, fy);
}

void n265_mc_luma_14_wh_neon(int16_t *dst, int ds, int w, int h,
                             const pixel *ref, int rs, int fx, int fy)
{
    if (!fx && !fy) {
        mc_copy_s16_shl6(dst, ds, ref, rs, w, h);
        return;
    }
    if (!fy) {
        const int16_t *f = mc_luma_filt[fx];
        mc_taps t;
        mc_taps_init(&t, f);
        mc_hblock_s16(dst, ds, ref, rs, w, h, &t, f);
        return;
    }
    if (!fx) {
        const int16_t *f = mc_luma_filt[fy];
        mc_taps t;
        mc_taps_init(&t, f);
        mc_vblock_s16(dst, ds, ref, rs, w, h, &t, f);
        return;
    }
    mc_luma_2d_s16(dst, ds, w, h, ref, rs, fx, fy);
}

/* The whole-plane first stage. Same arithmetic as the horizontal arm
 * above; what differs is only the shape - w is the padded picture width
 * plus twice N265_HF_PAD (472 and 1400 on the two clips the encoder was
 * instrumented on, both multiples of sixteen) and h is a CTB row. The
 * tails are kept anyway: nothing in the caller promises the width. */
void n265_mc_luma_hplane_neon(int16_t *dst, int ds, int w, int h,
                              const pixel *ref, int rs, int fx)
{
    const int16_t *f = mc_luma_filt[fx];
    mc_taps t;

    mc_taps_init(&t, f);
    mc_hblock_s16(dst, ds, ref, rs, w, h, &t, f);
}

/* ---- chroma (Table 8-12, 4 taps at eighth-pel) ------------------------
 *
 * The same shape as the luma stages, with three things that differ.
 *
 * SIGN PATTERN. Every one of the seven fractional chroma phases is
 * `- + + -`; only the magnitudes change. So the split into an unsigned
 * positive and an unsigned negative accumulator needs no branch and no
 * per-phase kernel here either, and phase 0 ({0, 64, 0, 0}) falls into it
 * harmlessly.
 *
 * RANGE. The widest phase is filt[3] / filt[5], whose positive taps total
 * 74 and negative 10: the accumulators reach 74 * 255 = 18870 and
 * 10 * 255 = 2550, both inside u16, and their difference lies in
 * [-2550, 18870], inside int16. The 2-D vertical stage over those values
 * reaches 74 * 18870 + 10 * 2550 = 1421880, and 74 * 32767 + 10 * 32768 =
 * 2752438 on adversarial int16 - both inside int32, so the s32
 * accumulator is exact there too.
 *
 * WIDTH. 4:2:0 halves every luma width, so w = 2 and w = 6 reach these
 * kernels (an 8x4 luma PU gives 4x2 chroma, the AMP 4x16 and 12x16 give
 * 2x8 and 6x8), and h = 2 with them. A four-lane tail is not enough, and
 * a scalar one is slow, so there is a two-lane step below it - loading
 * exactly two bytes, because four would reach two samples past the row
 * at the last tap offset in either pass.
 */
static const int16_t mc_chroma_filt[8][4] = {
    {  0, 64,  0,  0 },
    { -2, 58, 10, -2 },
    { -4, 54, 16, -2 },
    { -6, 46, 28, -4 },
    { -4, 36, 36, -4 },
    { -4, 28, 46, -6 },
    { -2, 16, 54, -4 },
    { -2, 10, 58, -2 },
};

/* the 4-tap vertical over int16 rows, for the 2-D second stage */
static inline void mc_cvacc8(const int16_t *p, int hs, const int16_t *f,
                             int32x4_t a[2])
{
    a[0] = a[1] = vdupq_n_s32(0);
    for (int k = 0; k < 4; k++) {
        int16x8_t v = vld1q_s16(p + (ptrdiff_t)(k - 1) * hs);
        a[0] = vmlal_n_s16(a[0], vget_low_s16(v), f[k]);
        a[1] = vmlal_high_n_s16(a[1], v, f[k]);
    }
}

static inline int32x4_t mc_cvacc4(const int16_t *p, int hs, const int16_t *f)
{
    int32x4_t a = vdupq_n_s32(0);
    for (int k = 0; k < 4; k++)
        a = vmlal_n_s16(a, vld1_s16(p + (ptrdiff_t)(k - 1) * hs), f[k]);
    return a;
}

/* two lanes, for what a 6-wide block has left after the four-lane step
 * and for a 2-wide one from the start. Two int16 loaded and splatted, not
 * four loaded and half discarded: the temp holds exactly (h + 3) * w
 * values and the last row's tail would be past it. */
static inline int32x4_t mc_cvacc2(const int16_t *p, int hs, const int16_t *f)
{
    int32x4_t a = vdupq_n_s32(0);
    for (int k = 0; k < 4; k++) {
        int32_t two;
        memcpy(&two, p + (ptrdiff_t)(k - 1) * hs, sizeof two);
        a = vmlal_n_s16(a, vreinterpret_s16_s32(vdup_n_s32(two)), f[k]);
    }
    return a;
}

static inline int mc_cvscalar16(const int16_t *p, int hs, const int16_t *f)
{
    int s = 0;
    for (int k = 0; k < 4; k++)
        s += f[k] * p[(ptrdiff_t)(k - 1) * hs];
    return s;
}

static inline void mc_emit2_s16(int16_t *d, int16x4_t v)
{
    uint32_t two = vget_lane_u32(vreinterpret_u32_s16(v), 0);
    memcpy(d, &two, 4);
}

#if BIT_DEPTH == 8
typedef struct { uint8x16_t q[4]; } mc_ctaps;

static inline void mc_ctaps_init(mc_ctaps *t, const int16_t *f)
{
    for (int k = 0; k < 4; k++)
        t->q[k] = vdupq_n_u8((uint8_t)(f[k] < 0 ? -f[k] : f[k]));
}

#define MC_CSUM(LD, OFF)                                                     \
    do {                                                                     \
        P = vmull_u8(LD(p + OFF(1)), vget_low_u8(t->q[1]));                  \
        P = vmlal_u8(P, LD(p + OFF(2)), vget_low_u8(t->q[2]));               \
        N = vmull_u8(LD(p + OFF(0)), vget_low_u8(t->q[0]));                  \
        N = vmlal_u8(N, LD(p + OFF(3)), vget_low_u8(t->q[3]));               \
    } while (0)

#define MC_CCOL(k) (k)
#define MC_CROW(k) ((ptrdiff_t)((k) - 1) * rs)

static inline int16x8_t mc_ch8(const uint8_t *p, const mc_ctaps *t)
{
    uint16x8_t P, N;
    MC_CSUM(vld1_u8, MC_CCOL);
    return vreinterpretq_s16_u16(vsubq_u16(P, N));
}

static inline int16x4_t mc_ch4(const uint8_t *p, const mc_ctaps *t)
{
    uint16x8_t P, N;
    MC_CSUM(mc_ld4, MC_CCOL);
    return vreinterpret_s16_u16(vsub_u16(vget_low_u16(P), vget_low_u16(N)));
}

static inline int16x4_t mc_ch2(const uint8_t *p, const mc_ctaps *t)
{
    uint16x8_t P, N;
    MC_CSUM(mc_ld2, MC_CCOL);
    return vreinterpret_s16_u16(vsub_u16(vget_low_u16(P), vget_low_u16(N)));
}

static inline int16x8_t mc_cv8(const uint8_t *p, int rs, const mc_ctaps *t)
{
    uint16x8_t P, N;
    MC_CSUM(vld1_u8, MC_CROW);
    return vreinterpretq_s16_u16(vsubq_u16(P, N));
}

static inline int16x4_t mc_cv4(const uint8_t *p, int rs, const mc_ctaps *t)
{
    uint16x8_t P, N;
    MC_CSUM(mc_ld4, MC_CROW);
    return vreinterpret_s16_u16(vsub_u16(vget_low_u16(P), vget_low_u16(N)));
}

static inline int16x4_t mc_cv2(const uint8_t *p, int rs, const mc_ctaps *t)
{
    uint16x8_t P, N;
    MC_CSUM(mc_ld2, MC_CROW);
    return vreinterpret_s16_u16(vsub_u16(vget_low_u16(P), vget_low_u16(N)));
}

#define MC_CSUM16(OFF)                                                       \
    do {                                                                     \
        uint8x16_t v1 = vld1q_u8(p + OFF(1)), v2 = vld1q_u8(p + OFF(2));     \
        uint8x16_t v0 = vld1q_u8(p + OFF(0)), v3 = vld1q_u8(p + OFF(3));     \
        uint16x8_t PL = vmull_u8(vget_low_u8(v1), vget_low_u8(t->q[1]));     \
        uint16x8_t PH = vmull_high_u8(v1, t->q[1]);                          \
        uint16x8_t NL = vmull_u8(vget_low_u8(v0), vget_low_u8(t->q[0]));     \
        uint16x8_t NH = vmull_high_u8(v0, t->q[0]);                          \
        PL = vmlal_u8(PL, vget_low_u8(v2), vget_low_u8(t->q[2]));            \
        PH = vmlal_high_u8(PH, v2, t->q[2]);                                 \
        NL = vmlal_u8(NL, vget_low_u8(v3), vget_low_u8(t->q[3]));            \
        NH = vmlal_high_u8(NH, v3, t->q[3]);                                 \
        *lo = vreinterpretq_s16_u16(vsubq_u16(PL, NL));                      \
        *hi = vreinterpretq_s16_u16(vsubq_u16(PH, NH));                      \
    } while (0)

static inline void mc_ch16(const uint8_t *p, const mc_ctaps *t,
                           int16x8_t *lo, int16x8_t *hi)
{
    MC_CSUM16(MC_CCOL);
}

static inline void mc_cv16(const uint8_t *p, int rs, const mc_ctaps *t,
                           int16x8_t *lo, int16x8_t *hi)
{
    MC_CSUM16(MC_CROW);
}

/* the four-lane rounded store, kept out of line of the macro above: the
 * s32 narrow rather than vqrshrun_n_s16 on a duplicated half, for the
 * reason spelled out at the luma four-lane tail */
static inline void mc_emit4_pix(uint8_t *d, int16x4_t v)
{
    uint16x4_t r = vqrshrun_n_s32(vmovl_s16(v), 6);
    mc_store4(d, vqmovn_u16(vcombine_u16(r, r)));
}

static inline void mc_emit2_pix(uint8_t *d, int16x4_t v)
{
    uint16x4_t r = vqrshrun_n_s32(vmovl_s16(v), 6);
    uint8x8_t o = vqmovn_u16(vcombine_u16(r, r));
    uint16_t two = vget_lane_u16(vreinterpret_u16_u8(o), 0);
    memcpy(d, &two, 2);
}

/* ---- the chroma row drivers ------------------------------------------
 *
 * Each ends in a SCALAR remainder rather than a four-lane one, because
 * w = 2 and w = 6 are real: at w = 2 even the four-byte load at tap 3
 * reaches p[x + 2 .. x + 5] where the window ends at p[x + 3].
 */
/* The four drivers differ in two axes - column pass or row pass, u8 out
 * or int16 out - so the loop structure is written once and the four
 * combinations pass in their own sum and their own store. `w`, `h`, `d`,
 * `ds`, `p`, `rs` and the loop's `x` come from the enclosing function.
 *
 * There is a TWO-lane step below the four-lane one, and it is the
 * difference between this family being worth shipping on chroma and not.
 * w = 2 and w = 6 are real - the AMP 4x16 and 12x16 luma PUs present 2x8
 * and 6x8 - and running their remainder scalar costs about 5ns a SAMPLE:
 * the u8 store and the u8 loads are both character types, nothing in the
 * C rules stops the store from hitting the reference, and each iteration
 * therefore waits for the one before it. 6x8 vertical measured 0.36x
 * against the C that way and 1.49x with two real lanes; the all-scalar
 * 2x8 went from 0.99x to 4.96x. The taps are copied into t0..t3 for the
 * remainder that is left, which is odd widths only - a guard path, since
 * 4:2:0 halving cannot produce one.
 */
#define MC_CROW_BODY(V8, V16, V4, V2, VS, EMIT8, EMIT16, EMIT4, EMIT2,       \
                     EMITS)                                                  \
  do {                                                                       \
    const int t0 = f[0], t1 = f[1], t2 = f[2], t3 = f[3];                    \
    for (int y = 0; y < h; y++, d += ds, p += rs) {                          \
        int x = 0;                                                           \
        for (; x + 16 <= w; x += 16) {                                       \
            int16x8_t lo, hi;                                                \
            V16;                                                             \
            EMIT16;                                                          \
        }                                                                    \
        for (; x + 8 <= w; x += 8) {                                         \
            int16x8_t v = V8;                                                \
            EMIT8;                                                           \
        }                                                                    \
        if (x + 4 <= w) {                                                    \
            int16x4_t v = V4;                                                \
            EMIT4;                                                           \
            x += 4;                                                          \
        }                                                                    \
        if (x + 2 <= w) {                                                    \
            int16x4_t v = V2;                                                \
            EMIT2;                                                           \
            x += 2;                                                          \
        }                                                                    \
        for (; x < w; x++) {                                                 \
            int s = VS;                                                      \
            EMITS;                                                           \
        }                                                                    \
    }                                                                        \
  } while (0)

#define MC_CH_SCALAR                                                         \
    (t0 * p[x - 1] + t1 * p[x] + t2 * p[x + 1] + t3 * p[x + 2])
#define MC_CV_SCALAR                                                         \
    (t0 * p[x - rs] + t1 * p[x] + t2 * p[x + rs] + t3 * p[x + 2 * rs])

static void mc_chblock_pix(uint8_t *d, int ds, const uint8_t *p, int rs,
                          int w, int h, const mc_ctaps *t, const int16_t *f)
{
    MC_CROW_BODY(mc_ch8(p + x - 1, t), mc_ch16(p + x - 1, t, &lo, &hi),
                 mc_ch4(p + x - 1, t), mc_ch2(p + x - 1, t), MC_CH_SCALAR,
                 vst1_u8(d + x, vqrshrun_n_s16(v, 6)),
                 vst1q_u8(d + x, vcombine_u8(vqrshrun_n_s16(lo, 6),
                                             vqrshrun_n_s16(hi, 6))),
                 mc_emit4_pix(d + x, v), mc_emit2_pix(d + x, v),
                 d[x] = mc_clip1((s + 32) >> 6));
}

static void mc_chblock_s16(int16_t *d, int ds, const uint8_t *p, int rs,
                           int w, int h, const mc_ctaps *t, const int16_t *f)
{
    MC_CROW_BODY(mc_ch8(p + x - 1, t), mc_ch16(p + x - 1, t, &lo, &hi),
                 mc_ch4(p + x - 1, t), mc_ch2(p + x - 1, t), MC_CH_SCALAR,
                 vst1q_s16(d + x, v),
                 (vst1q_s16(d + x, lo), vst1q_s16(d + x + 8, hi)),
                 vst1_s16(d + x, v), mc_emit2_s16(d + x, v),
                 d[x] = (int16_t)s);
}

static void mc_cvblock_pix(uint8_t *d, int ds, const uint8_t *p, int rs,
                          int w, int h, const mc_ctaps *t, const int16_t *f)
{
    MC_CROW_BODY(mc_cv8(p + x, rs, t), mc_cv16(p + x, rs, t, &lo, &hi),
                 mc_cv4(p + x, rs, t), mc_cv2(p + x, rs, t), MC_CV_SCALAR,
                 vst1_u8(d + x, vqrshrun_n_s16(v, 6)),
                 vst1q_u8(d + x, vcombine_u8(vqrshrun_n_s16(lo, 6),
                                             vqrshrun_n_s16(hi, 6))),
                 mc_emit4_pix(d + x, v), mc_emit2_pix(d + x, v),
                 d[x] = mc_clip1((s + 32) >> 6));
}

static void mc_cvblock_s16(int16_t *d, int ds, const uint8_t *p, int rs,
                           int w, int h, const mc_ctaps *t, const int16_t *f)
{
    MC_CROW_BODY(mc_cv8(p + x, rs, t), mc_cv16(p + x, rs, t, &lo, &hi),
                 mc_cv4(p + x, rs, t), mc_cv2(p + x, rs, t), MC_CV_SCALAR,
                 vst1q_s16(d + x, v),
                 (vst1q_s16(d + x, lo), vst1q_s16(d + x + 8, hi)),
                 vst1_s16(d + x, v), mc_emit2_s16(d + x, v),
                 d[x] = (int16_t)s);
}

#else   /* Main10 chroma: u32 accumulators, four outputs to a vector ---- */

typedef struct { uint16x8_t q[4]; } mc_ctaps;

static inline void mc_ctaps_init(mc_ctaps *t, const int16_t *f)
{
    for (int k = 0; k < 4; k++)
        t->q[k] = vdupq_n_u16((uint16_t)(f[k] < 0 ? -f[k] : f[k]));
}

/* RANGE. The widest phase's positive taps total 74 and its negative ones
 * 10, so the two accumulators reach 74 * 1023 = 75702 and 10230 and the
 * difference lies in [-10230, 75702] - int32, with the same room the
 * luma stage has. */
#define MC_CSUM16_8(OFF)                                                     \
    do {                                                                     \
        uint16x8_t v1 = vld1q_u16(p + OFF(1)), v2 = vld1q_u16(p + OFF(2));   \
        uint16x8_t v0 = vld1q_u16(p + OFF(0)), v3 = vld1q_u16(p + OFF(3));   \
        uint32x4_t PL = vmull_u16(vget_low_u16(v1), vget_low_u16(t->q[1]));  \
        uint32x4_t PH = vmull_high_u16(v1, t->q[1]);                         \
        uint32x4_t NL = vmull_u16(vget_low_u16(v0), vget_low_u16(t->q[0]));  \
        uint32x4_t NH = vmull_high_u16(v0, t->q[0]);                         \
        PL = vmlal_u16(PL, vget_low_u16(v2), vget_low_u16(t->q[2]));         \
        PH = vmlal_high_u16(PH, v2, t->q[2]);                                \
        NL = vmlal_u16(NL, vget_low_u16(v3), vget_low_u16(t->q[3]));         \
        NH = vmlal_high_u16(NH, v3, t->q[3]);                                \
        *lo = vreinterpretq_s32_u32(vsubq_u32(PL, NL));                      \
        *hi = vreinterpretq_s32_u32(vsubq_u32(PH, NH));                      \
    } while (0)

#define MC_CSUM16_4(LD, OFF)                                                 \
    do {                                                                     \
        uint32x4_t P = vmull_u16(LD(p + OFF(1)), vget_low_u16(t->q[1]));     \
        uint32x4_t N = vmull_u16(LD(p + OFF(0)), vget_low_u16(t->q[0]));     \
        P = vmlal_u16(P, LD(p + OFF(2)), vget_low_u16(t->q[2]));             \
        N = vmlal_u16(N, LD(p + OFF(3)), vget_low_u16(t->q[3]));             \
        return vreinterpretq_s32_u32(vsubq_u32(P, N));                       \
    } while (0)

#define MC_CCOL(k) (k)
#define MC_CROW(k) ((ptrdiff_t)((k) - 1) * rs)

static inline void mc_ch8_32(const pixel *p, const mc_ctaps *t,
                             int32x4_t *lo, int32x4_t *hi)
{
    MC_CSUM16_8(MC_CCOL);
}

static inline int32x4_t mc_ch4_32(const pixel *p, const mc_ctaps *t)
{
    MC_CSUM16_4(vld1_u16, MC_CCOL);
}

/* two samples, four bytes: at the last tap offset a four-sample load
 * would reach two past the row, which is the bug the 8-bit form's
 * mc_ld2 exists to avoid and is just as real here. */
static inline int32x4_t mc_ch2_32(const pixel *p, const mc_ctaps *t)
{
    MC_CSUM16_4(mc_ld2_16, MC_CCOL);
}

static inline void mc_cv8_32(const pixel *p, int rs, const mc_ctaps *t,
                             int32x4_t *lo, int32x4_t *hi)
{
    MC_CSUM16_8(MC_CROW);
}

static inline int32x4_t mc_cv4_32(const pixel *p, int rs, const mc_ctaps *t)
{
    MC_CSUM16_4(vld1_u16, MC_CROW);
}

static inline int32x4_t mc_cv2_32(const pixel *p, int rs, const mc_ctaps *t)
{
    MC_CSUM16_4(mc_ld2_16, MC_CROW);
}

/* the two-sample stores, which are what makes this family worth shipping
 * on chroma at all: 4:2:0 halving presents w = 2 and w = 6 (the AMP 4x16
 * and 12x16 luma PUs give 2x8 and 6x8), and phase 6 measured the scalar
 * remainder at about 5 ns a sample. */
static inline void mc_emit2_pix(pixel *d, int32x4_t v)
{
    uint16x4_t r = vmin_u16(vqrshrun_n_s32(v, 6), vdup_n_u16(PIXEL_MAX));
    uint32_t two = vget_lane_u32(vreinterpret_u32_u16(r), 0);

    memcpy(d, &two, 4);
}

static inline void mc_emit2_s16_32(int16_t *d, int32x4_t v)
{
    int16x4_t n = vshrn_n_s32(v, MC_SHIFT1);
    uint32_t two = vget_lane_u32(vreinterpret_u32_s16(n), 0);

    memcpy(d, &two, 4);
}

#define MC_CBLOCK16(GET8, GET4, GET2, SCALAR, ST8, ST4, ST2, STS)            \
  do {                                                                       \
    const int t0 = f[0], t1 = f[1], t2 = f[2], t3 = f[3];                    \
    for (int y = 0; y < h; y++, d += ds, p += rs) {                          \
        int x = 0;                                                           \
        for (; x + 8 <= w; x += 8) {                                         \
            int32x4_t lo, hi;                                                \
            GET8;                                                            \
            ST8;                                                             \
        }                                                                    \
        if (x + 4 <= w) {                                                    \
            int32x4_t v = GET4;                                              \
            ST4;                                                             \
            x += 4;                                                          \
        }                                                                    \
        if (x + 2 <= w) {                                                    \
            int32x4_t v = GET2;                                              \
            ST2;                                                             \
            x += 2;                                                          \
        }                                                                    \
        for (; x < w; x++) {                                                 \
            int s = SCALAR;                                                  \
            STS;                                                             \
        }                                                                    \
    }                                                                        \
  } while (0)

#define MC_CH_SCALAR                                                         \
    (t0 * p[x - 1] + t1 * p[x] + t2 * p[x + 1] + t3 * p[x + 2])
#define MC_CV_SCALAR                                                         \
    (t0 * p[x - rs] + t1 * p[x] + t2 * p[x + rs] + t3 * p[x + 2 * rs])

#define MC_CST8_PIX                                                          \
    vst1q_u16(d + x, vminq_u16(vcombine_u16(vqrshrun_n_s32(lo, 6),           \
                                            vqrshrun_n_s32(hi, 6)),          \
                               vdupq_n_u16(PIXEL_MAX)))
#define MC_CST4_PIX                                                          \
    vst1_u16(d + x, vmin_u16(vqrshrun_n_s32(v, 6), vdup_n_u16(PIXEL_MAX)))
#define MC_CST8_S16                                                          \
    vst1q_s16(d + x, vcombine_s16(vshrn_n_s32(lo, MC_SHIFT1),                \
                                  vshrn_n_s32(hi, MC_SHIFT1)))
#define MC_CST4_S16 vst1_s16(d + x, vshrn_n_s32(v, MC_SHIFT1))

static void mc_chblock_pix(pixel *d, int ds, const pixel *p, int rs,
                           int w, int h, const mc_ctaps *t, const int16_t *f)
{
    MC_CBLOCK16(mc_ch8_32(p + x - 1, t, &lo, &hi), mc_ch4_32(p + x - 1, t),
                mc_ch2_32(p + x - 1, t), MC_CH_SCALAR,
                MC_CST8_PIX, MC_CST4_PIX, mc_emit2_pix(d + x, v),
                d[x] = mc_clip1((s + 32) >> 6));
}

static void mc_chblock_s16(int16_t *d, int ds, const pixel *p, int rs,
                           int w, int h, const mc_ctaps *t, const int16_t *f)
{
    MC_CBLOCK16(mc_ch8_32(p + x - 1, t, &lo, &hi), mc_ch4_32(p + x - 1, t),
                mc_ch2_32(p + x - 1, t), MC_CH_SCALAR,
                MC_CST8_S16, MC_CST4_S16, mc_emit2_s16_32(d + x, v),
                d[x] = (int16_t)(s >> MC_SHIFT1));
}

static void mc_cvblock_pix(pixel *d, int ds, const pixel *p, int rs,
                           int w, int h, const mc_ctaps *t, const int16_t *f)
{
    MC_CBLOCK16(mc_cv8_32(p + x, rs, t, &lo, &hi), mc_cv4_32(p + x, rs, t),
                mc_cv2_32(p + x, rs, t), MC_CV_SCALAR,
                MC_CST8_PIX, MC_CST4_PIX, mc_emit2_pix(d + x, v),
                d[x] = mc_clip1((s + 32) >> 6));
}

static void mc_cvblock_s16(int16_t *d, int ds, const pixel *p, int rs,
                           int w, int h, const mc_ctaps *t, const int16_t *f)
{
    MC_CBLOCK16(mc_cv8_32(p + x, rs, t, &lo, &hi), mc_cv4_32(p + x, rs, t),
                mc_cv2_32(p + x, rs, t), MC_CV_SCALAR,
                MC_CST8_S16, MC_CST4_S16, mc_emit2_s16_32(d + x, v),
                d[x] = (int16_t)(s >> MC_SHIFT1));
}

#endif  /* BIT_DEPTH */

/* ---- the chroma 2-D stage, and its two temps -------------------------
 *
 * The horizontal stage writes h + 3 rows at stride w, so the temp's
 * bound is the widest CHROMA block the format presents: 32x32 while the
 * plane is halved, 32x64 at 4:2:2 (4:2:0's widths, the luma heights) and
 * 64x64 at 4:4:4, where a chroma block IS the luma block.
 *
 * There are two of them rather than one because a frame costs what it
 * RESERVES and not what it touches: a single (64 + 3) x 64 temp would put 8 KB
 * on the stack of every 4:2:0 call, which is the format twelve of the fifteen
 * corpus clips are in. The common arm therefore keeps 4:2:0's temp and the
 * wide one is its own out-of-line function with its own frame, reached only by
 * a block no halved plane can produce. Both arms run the same body and are
 * bit-exact with each other and with the C by construction.
 */
#define MC_CTMP_SMALL ((32 + 3) * 32)
#define MC_CTMP_WIDE  ((64 + 3) * 64)
#define MC_CHROMA_WIDE(w, h) ((w) > 32 || (h) > 32)

static inline void mc_chroma_2d_pix(pixel *dst, int ds, int w, int h,
                                    const pixel *ref, int rs, int fx,
                                    int fy, int16_t *tmp)
{
    const int16_t *fh = mc_chroma_filt[fx], *fv = mc_chroma_filt[fy];
    mc_ctaps th;

    mc_ctaps_init(&th, fh);
    mc_chblock_s16(tmp, w, ref - (ptrdiff_t)rs, rs, w, h + 3, &th, fh);

    const int16_t *base = tmp + (ptrdiff_t)w;
    for (int y = 0; y < h; y++) {
        const int16_t *p = base + (ptrdiff_t)y * w;
        pixel *d = dst + (ptrdiff_t)y * ds;
        int x = 0;
        for (; x + 8 <= w; x += 8) {
            int32x4_t a[2];
            mc_cvacc8(p + x, w, fv, a);
            MC_ST8_V(d + x, a[0], a[1]);
        }
        if (x + 4 <= w) {
            MC_ST4_V(d + x, mc_cvacc4(p + x, w, fv));
            x += 4;
        }
        if (x + 2 <= w) {
            MC_ST2_V(d + x, mc_cvacc2(p + x, w, fv));
            x += 2;
        }
        for (; x < w; x++)
            d[x] = mc_clip1((mc_cvscalar16(p + x, w, fv) + MC_ROUND23)
                            >> MC_SHIFT23);
    }
}

static inline void mc_chroma_2d_s16(int16_t *dst, int ds, int w, int h,
                                    const pixel *ref, int rs, int fx,
                                    int fy, int16_t *tmp)
{
    const int16_t *fh = mc_chroma_filt[fx], *fv = mc_chroma_filt[fy];
    mc_ctaps th;

    mc_ctaps_init(&th, fh);
    mc_chblock_s16(tmp, w, ref - (ptrdiff_t)rs, rs, w, h + 3, &th, fh);

    const int16_t *base = tmp + (ptrdiff_t)w;
    for (int y = 0; y < h; y++) {
        const int16_t *p = base + (ptrdiff_t)y * w;
        int16_t *d = dst + (ptrdiff_t)y * ds;
        int x = 0;
        for (; x + 8 <= w; x += 8) {
            int32x4_t a[2];
            mc_cvacc8(p + x, w, fv, a);
            vst1q_s16(d + x, MC_NARROW_S16(a[0], a[1]));
        }
        if (x + 4 <= w) {
            vst1_s16(d + x, vshrn_n_s32(mc_cvacc4(p + x, w, fv), 6));
            x += 4;
        }
        if (x + 2 <= w) {
            mc_emit2_s16(d + x, vshrn_n_s32(mc_cvacc2(p + x, w, fv), 6));
            x += 2;
        }
        for (; x < w; x++)
            d[x] = (int16_t)(mc_cvscalar16(p + x, w, fv) >> 6);
    }
}

static __attribute__((noinline)) void
mc_chroma_2d_pix_wide(pixel *dst, int ds, int w, int h, const pixel *ref,
                      int rs, int fx, int fy)
{
    int16_t tmp[MC_CTMP_WIDE];
    mc_chroma_2d_pix(dst, ds, w, h, ref, rs, fx, fy, tmp);
}

static __attribute__((noinline)) void
mc_chroma_2d_s16_wide(int16_t *dst, int ds, int w, int h, const pixel *ref,
                      int rs, int fx, int fy)
{
    int16_t tmp[MC_CTMP_WIDE];
    mc_chroma_2d_s16(dst, ds, w, h, ref, rs, fx, fy, tmp);
}

void n265_mc_chroma_wh_neon(pixel *dst, int ds, int w, int h,
                            const pixel *ref, int rs, int fx, int fy)
{
    if (!fx && !fy) {
        mc_copy_pix(dst, ds, ref, rs, w, h);
        return;
    }
    if (!fy) {
        const int16_t *f = mc_chroma_filt[fx];
        mc_ctaps t;
        mc_ctaps_init(&t, f);
        mc_chblock_pix(dst, ds, ref, rs, w, h, &t, f);
        return;
    }
    if (!fx) {
        const int16_t *f = mc_chroma_filt[fy];
        mc_ctaps t;
        mc_ctaps_init(&t, f);
        mc_cvblock_pix(dst, ds, ref, rs, w, h, &t, f);
        return;
    }
    /* 2-D: the horizontal stage into an int16 temp of h + 3 rows at
     * stride w - the same shape and the same values the C's tmp holds -
     * then the vertical stage over exactly those rows. The temp is the
     * format's; see MC_CTMP_SMALL above. */
    if (MC_CHROMA_WIDE(w, h)) {
        mc_chroma_2d_pix_wide(dst, ds, w, h, ref, rs, fx, fy);
        return;
    }
    int16_t tmp[MC_CTMP_SMALL];
    mc_chroma_2d_pix(dst, ds, w, h, ref, rs, fx, fy, tmp);
}

void n265_mc_chroma_14_wh_neon(int16_t *dst, int ds, int w, int h,
                               const pixel *ref, int rs, int fx, int fy)
{
    if (!fx && !fy) {
        mc_copy_s16_shl6(dst, ds, ref, rs, w, h);
        return;
    }
    if (!fy) {
        const int16_t *f = mc_chroma_filt[fx];
        mc_ctaps t;
        mc_ctaps_init(&t, f);
        mc_chblock_s16(dst, ds, ref, rs, w, h, &t, f);
        return;
    }
    if (!fx) {
        const int16_t *f = mc_chroma_filt[fy];
        mc_ctaps t;
        mc_ctaps_init(&t, f);
        mc_cvblock_s16(dst, ds, ref, rs, w, h, &t, f);
        return;
    }
    /* the format's temp; see MC_CTMP_SMALL above */
    if (MC_CHROMA_WIDE(w, h)) {
        mc_chroma_2d_s16_wide(dst, ds, w, h, ref, rs, fx, fy);
        return;
    }
    int16_t tmp[MC_CTMP_SMALL];
    mc_chroma_2d_s16(dst, ds, w, h, ref, rs, fx, fy, tmp);
}

/* ---- 8.5.3.3.4.3, explicit weighted sample prediction ---------------
 *
 * Three kernels, one map: `(v * wt + round) >> shift` with an offset and
 * a clip. The shift is a RUNTIME value here - log2Wd is the slice's own
 * denominator plus shift1, and the denominator is whatever the estimator
 * chose - so none of the narrowing intrinsics with an immediate shift
 * can be used and the shift rides in a vector. vrshlq_s32 with a
 * negative count is exactly the C's `(x + (1 << (s-1))) >> s` including
 * on a negative x, and vshlq_s32 with a negative count is the plain
 * arithmetic shift the bi form wants, its rounding term being folded
 * into the offset already.
 *
 * The narrow out is vqmovun, whose saturation IS the C's lower clip at
 * zero; the upper clip is PIXEL_MAX and is a separate vmin, because no
 * narrow's own bound is PIXEL_MAX at either depth.
 *
 * The scalar tails are not optional. 4:2:0 halves every luma PU width,
 * so w = 2 and w = 6 reach these kernels from the AMP shapes, and a
 * four-lane tail at w = 2 writes two samples of the next block.
 */

static inline uint16x8_t wp_clip_u16(int32x4_t lo, int32x4_t hi)
{
    return vminq_u16(vcombine_u16(vqmovun_s32(lo), vqmovun_s32(hi)),
                     vdupq_n_u16(PIXEL_MAX));
}

static inline void wp_store8(pixel *pd, uint16x8_t r)
{
#if BIT_DEPTH == 8
    vst1_u8(pd, vqmovn_u16(r));
#else
    vst1q_u16(pd, r);
#endif
}

static inline void wp_store4(pixel *pd, uint16x4_t r)
{
#if BIT_DEPTH == 8
    uint8x8_t o = vqmovn_u16(vcombine_u16(r, r));
    uint32_t four = vget_lane_u32(vreinterpret_u32_u8(o), 0);
    memcpy(pd, &four, 4);       /* dst has no alignment guarantee */
#else
    vst1_u16(pd, r);
#endif
}

static inline int wp_scalar(int v, int wt, int shift, int o)
{
    int r = shift >= 1 ? ((v * wt + (1 << (shift - 1))) >> shift) + o
                       : v * wt + o;
    return r < 0 ? 0 : r > PIXEL_MAX ? PIXEL_MAX : r;
}

void n265_wp_uni_wh_neon(pixel *dst, int ds, const int16_t *src, int w, int h,
                         int wt, int off, int denom)
{
    int log2Wd = denom + MC_SHIFT3;
    int o = off * (1 << BIT_DEPTH_SHIFT);
    int16x4_t vw = vdup_n_s16((int16_t)wt);
    int32x4_t vsh = vdupq_n_s32(-log2Wd);
    int32x4_t vo = vdupq_n_s32(o);

    for (int y = 0; y < h; y++) {
        const int16_t *ps = src + (ptrdiff_t)y * w;
        pixel *pd = dst + (ptrdiff_t)y * ds;
        int x = 0;
        for (; x + 8 <= w; x += 8) {
            int16x8_t v = vld1q_s16(ps + x);
            int32x4_t lo = vmull_s16(vget_low_s16(v), vw);
            int32x4_t hi = vmull_high_s16(v, vcombine_s16(vw, vw));
            lo = vaddq_s32(vrshlq_s32(lo, vsh), vo);
            hi = vaddq_s32(vrshlq_s32(hi, vsh), vo);
            wp_store8(pd + x, wp_clip_u16(lo, hi));
        }
        if (x + 4 <= w) {
            int32x4_t v = vmull_s16(vld1_s16(ps + x), vw);
            v = vaddq_s32(vrshlq_s32(v, vsh), vo);
            wp_store4(pd + x, vmin_u16(vqmovun_s32(v),
                                       vdup_n_u16(PIXEL_MAX)));
            x += 4;
        }
        for (; x < w; x++)
            pd[x] = (pixel)wp_scalar(ps[x], wt, log2Wd, o);
    }
}

void n265_wp_bi_wh_neon(pixel *dst, int ds, const int16_t *a, const int16_t *b,
                        int w, int h, int w0, int o0, int w1, int o1,
                        int denom)
{
    int log2Wd = denom + MC_SHIFT3;
    /* a MULTIPLY, not a shift: the bracket goes negative and shifting
     * a negative left is undefined. See mc.c's twin. */
    int add = ((o0 + o1) * (1 << BIT_DEPTH_SHIFT) + 1) * (1 << log2Wd);
    int16x4_t v0 = vdup_n_s16((int16_t)w0), v1 = vdup_n_s16((int16_t)w1);
    int16x8_t q0 = vcombine_s16(v0, v0), q1 = vcombine_s16(v1, v1);
    int32x4_t vadd = vdupq_n_s32(add);
    int32x4_t vsh = vdupq_n_s32(-(log2Wd + 1));

    for (int y = 0; y < h; y++) {
        const int16_t *pa = a + (ptrdiff_t)y * w;
        const int16_t *pb = b + (ptrdiff_t)y * w;
        pixel *pd = dst + (ptrdiff_t)y * ds;
        int x = 0;
        for (; x + 8 <= w; x += 8) {
            int16x8_t va = vld1q_s16(pa + x), vb = vld1q_s16(pb + x);
            int32x4_t lo = vmull_s16(vget_low_s16(va), v0);
            lo = vmlal_s16(lo, vget_low_s16(vb), v1);
            int32x4_t hi = vmull_high_s16(va, q0);
            hi = vmlal_high_s16(hi, vb, q1);
            lo = vshlq_s32(vaddq_s32(lo, vadd), vsh);
            hi = vshlq_s32(vaddq_s32(hi, vadd), vsh);
            wp_store8(pd + x, wp_clip_u16(lo, hi));
        }
        if (x + 4 <= w) {
            int32x4_t s = vmull_s16(vld1_s16(pa + x), v0);
            s = vmlal_s16(s, vld1_s16(pb + x), v1);
            s = vshlq_s32(vaddq_s32(s, vadd), vsh);
            wp_store4(pd + x, vmin_u16(vqmovun_s32(s),
                                       vdup_n_u16(PIXEL_MAX)));
            x += 4;
        }
        for (; x < w; x++) {
            int v = (pa[x] * w0 + pb[x] * w1 + add) >> (log2Wd + 1);
            pd[x] = (pixel)(v < 0 ? 0 : v > PIXEL_MAX ? PIXEL_MAX : v);
        }
    }
}

void n265_wp_pix_wh_neon(pixel *dst, int ds, const pixel *src, int ss, int w,
                         int h, int wt, int off, int denom)
{
    int o = off * (1 << BIT_DEPTH_SHIFT);
    int16x4_t vw = vdup_n_s16((int16_t)wt);
    int16x8_t qw = vcombine_s16(vw, vw);
    int32x4_t vsh = vdupq_n_s32(-denom);
    int32x4_t vo = vdupq_n_s32(o);

    for (int y = 0; y < h; y++) {
        const pixel *ps = src + (ptrdiff_t)y * ss;
        pixel *pd = dst + (ptrdiff_t)y * ds;
        int x = 0;
        for (; x + 8 <= w; x += 8) {
#if BIT_DEPTH == 8
            int16x8_t v = vreinterpretq_s16_u16(vmovl_u8(vld1_u8(ps + x)));
#else
            int16x8_t v = vreinterpretq_s16_u16(vld1q_u16(ps + x));
#endif
            int32x4_t lo = vmull_s16(vget_low_s16(v), vw);
            int32x4_t hi = vmull_high_s16(v, qw);
            /* denom can be 0, and vrshlq by 0 is the identity, which is
             * what the C's `denom >= 1` arm degenerates to */
            lo = vaddq_s32(vrshlq_s32(lo, vsh), vo);
            hi = vaddq_s32(vrshlq_s32(hi, vsh), vo);
            wp_store8(pd + x, wp_clip_u16(lo, hi));
        }
        if (x + 4 <= w) {
#if BIT_DEPTH == 8
            uint32_t four;
            memcpy(&four, ps + x, 4);
            uint16x4_t u = vget_low_u16(vmovl_u8(vreinterpret_u8_u32(
                                            vdup_n_u32(four))));
            int16x4_t v = vreinterpret_s16_u16(u);
#else
            int16x4_t v = vreinterpret_s16_u16(vld1_u16(ps + x));
#endif
            int32x4_t s = vaddq_s32(vrshlq_s32(vmull_s16(v, vw), vsh), vo);
            wp_store4(pd + x, vmin_u16(vqmovun_s32(s),
                                       vdup_n_u16(PIXEL_MAX)));
            x += 4;
        }
        for (; x < w; x++)
            pd[x] = (pixel)wp_scalar(ps[x], wt, denom, o);
    }
}

#else
typedef int n265_mc_neon_unused_;   /* C requires a non-empty translation unit */

#endif /* N265_NEON_MC */
