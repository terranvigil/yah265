/*
 * mc_neon.c - NEON motion-compensation kernels (class N265_ASM_MC).
 * One file per kernel family; see src/encoder/arm/neon.h for the dispatch
 * convention every kernel here follows.
 * Copyright (c) 2026, the yah265 authors. GPL-2.0-or-later; see LICENSE.
 */

#if N265_NEON

#include <arm_neon.h>
#include <stddef.h>
#include <string.h>

#include "encoder/arm/neon.h"

/* The C form is
 *
 *     v = (a + b + 64) >> 7;  dst = clip3(0, 255, v)
 *
 * on int16 inputs, so the sum needs 17 bits and the widening add is not
 * optional. The narrow reproduces the C EXACTLY rather than approximately:
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
void n265_bi_avg_wh_neon(uint8_t *dst, int ds, const int16_t *a,
                         const int16_t *b, int w, int h)
{
    for (int y = 0; y < h; y++) {
        const int16_t *pa = a + (ptrdiff_t)y * w;
        const int16_t *pb = b + (ptrdiff_t)y * w;
        uint8_t *pd = dst + (ptrdiff_t)y * ds;
        int x = 0;

        for (; x + 8 <= w; x += 8) {
            int16x8_t va = vld1q_s16(pa + x);
            int16x8_t vb = vld1q_s16(pb + x);
            int32x4_t lo = vaddl_s16(vget_low_s16(va), vget_low_s16(vb));
            int32x4_t hi = vaddl_high_s16(va, vb);
            uint16x8_t r = vcombine_u16(vqrshrun_n_s32(lo, 7),
                                        vqrshrun_n_s32(hi, 7));
            vst1_u8(pd + x, vqmovn_u16(r));
        }
        /* w = 4 and w = 12 leave a four-sample tail. Load and store FOUR,
         * never eight, or the guard border pays for it. */
        if (x + 4 <= w) {
            int32x4_t s = vaddl_s16(vld1_s16(pa + x), vld1_s16(pb + x));
            uint16x4_t r = vqrshrun_n_s32(s, 7);
            uint8x8_t o = vqmovn_u16(vcombine_u16(r, r));
            uint32_t four = vget_lane_u32(vreinterpret_u32_u8(o), 0);
            memcpy(pd + x, &four, 4);   /* dst has no alignment guarantee */
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
            int v = (pa[x] + pb[x] + 64) >> 7;
            pd[x] = (uint8_t)(v < 0 ? 0 : v > 255 ? 255 : v);
        }
    }
}

#else
typedef int n265_mc_neon_unused_;   /* C requires a non-empty translation unit */
#endif /* N265_NEON */
