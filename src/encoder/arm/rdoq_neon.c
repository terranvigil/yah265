/*
 * rdoq_neon.c - the elementwise half of the RDOQ lattice (N265_ASM_QUANT).
 *
 * The lattice itself (rdoq.c) is sequential: every coefficient's decision
 * is priced against the greater1/greater2/rice state the one before it
 * left behind, so it stays on C and stays scalar. What surrounds it is
 * not. Two pieces are pure functions of the block:
 *
 *   n265_rdoq_rtn      the round-to-nearest level of every coefficient
 *                      (the candidate set's upper bound), the block's
 *                      all-zero coefficient energy, and the 4x4 sub-block
 *                      occupancy of the levels it produced;
 *   n265_rdoq_sb_nz    that same occupancy over an arbitrary packed
 *                      block, which is what the sign-bit-hiding pass
 *                      needs over the FINAL levels.
 *
 * The C form of the first cannot be auto-vectorised and clang does not
 * try: the energy is a running `double` sum, and reassociating a
 * floating-point reduction is not something a compiler may do without
 * -ffast-math. That is why it was measured at a third of rdoq's time on
 * foreman for a loop that is otherwise two multiplies.
 *
 * Bit-exactness, term by term:
 *
 *   ENERGY. Every term is coef^2 with |coef| <= 32768 at every depth, so
 *   0 <= term <=
 *   2^30 and it is exact in int32. The block has at most 1024 of them, so
 *   the total is at most 2^40 - every partial sum, in any order, is an
 *   exact integer below 2^53 and therefore exactly representable in a
 *   double. Floating-point addition of exactly representable integers
 *   whose sum is also exactly representable is exact and associative, so
 *   the pairwise vector reduction here gives the SAME double as the C's
 *   raster-order running sum, and the final uint64 -> double conversion
 *   is exact for the same reason. (The C's comment about keeping the
 *   raster order was insurance, not a requirement; this is the proof it
 *   was not needed.)
 *
 *   LEVELS. qbits = 29 - BitDepth + qP/6 - log2n with qP = Qp' in
 *   [0, 51 + QpBdOffset] and log2n in [2, 5], so qbits is in [16, 27] at
 *   8 bits and [14, 27] at Main10 - the same top, because Qp's twelve
 *   extra steps buy exactly the two bits BitDepth costs. add =
 *   (1 << qbits) / 3 or / 6, so add < 2^27/3 < 4.5e7 at both. |coef| <=
 *   32768 (the forward transform clips to int16 at every depth) and
 *   q = quant_scale[qP % 6] <= 26214, so |coef| * q <= 8.592e8 and
 *   |coef| * q + add <= 9.04e8 < 2^31. The C's int64 product never needed
 *   64 bits: the whole expression fits int32 lanes, four at a time
 *   instead of two, AND IT FITS AT BOTH DEPTHS - this family never sees a
 *   sample, only the int16 coefficients a depth-invariant transform
 *   produced, so phase 8 item 3 moved no code in it at all.
 *
 *   THE DEADZONE BRANCH. The C skips the multiply when |coef| < czero,
 *   where czero = ceil(((1 << qbits) - add) / q). For |coef| <= czero - 1
 *   we have |coef| * q < (1 << qbits) - add, so the shift yields 0, which
 *   is exactly what the C stores; and for |coef| >= czero it yields at
 *   least 1. The branch is a speed optimisation over an expression that
 *   is already correct, so the vector form computes it unconditionally
 *   and `czero` is unused here. It stays in the signature because the
 *   kernel and its C entry point have the same one.
 *
 * Copyright (c) 2026, the yah265 authors. GPL-2.0-or-later; see LICENSE.
 */

#include <arm_neon.h>

#include "encoder/rdoq.h"
#include "encoder/arm/neon.h"

/* |c| * q + add, shifted and clipped: four coefficients per call. `vsh`
 * is a splat of -qbits, so vshlq_s32 is the arithmetic right shift; the
 * operand is non-negative, so it is the C's `>>` on a non-negative int64
 * exactly. vmovl_s16 before the abs is what makes -32768 come out as
 * +32768 rather than saturating: the C's `-coef[i]` promotes to int. */
static inline int32x4_t rtn_level4(int16x4_t c16, int32x4_t vq,
                                   int32x4_t vadd, int32x4_t vsh,
                                   int32x4_t vmax)
{
    int32x4_t c = vabsq_s32(vmovl_s16(c16));
    return vminq_s32(vshlq_s32(vmlaq_s32(vadd, c, vq), vsh), vmax);
}

int n265_rdoq_rtn_neon(int16_t *rtn, double *all_zero, uint64_t *sb_nz,
                       const int16_t *coef, int log2n, int q, int64_t add,
                       int qbits, int czero)
{
    const int n = 1 << log2n, sb_dim = n >> 2;
    const int32x4_t vq = vdupq_n_s32(q);
    const int32x4_t vadd = vdupq_n_s32((int32_t)add);
    const int32x4_t vsh = vdupq_n_s32(-qbits);
    const int32x4_t vmax = vdupq_n_s32(32767);
    uint64x2_t eacc = vdupq_n_u64(0);
    uint64_t m = 0;

    (void)czero;
    if (n == 4) {
        /* one sub-block, so the occupancy bit IS "any level nonzero" and
         * the 16 coefficients are contiguous */
        int32x4_t any = vdupq_n_s32(0);
        for (int i = 0; i < 16; i += 8) {
            int16x8_t v = vld1q_s16(coef + i);
            int16x4_t vl = vget_low_s16(v), vh = vget_high_s16(v);
            int32x4_t l0 = rtn_level4(vl, vq, vadd, vsh, vmax);
            int32x4_t l1 = rtn_level4(vh, vq, vadd, vsh, vmax);
            eacc = vpadalq_u32(eacc,
                               vreinterpretq_u32_s32(vmull_s16(vl, vl)));
            eacc = vpadalq_u32(eacc,
                               vreinterpretq_u32_s32(vmull_s16(vh, vh)));
            vst1q_s16(rtn + i, vcombine_s16(vmovn_s32(l0), vmovn_s32(l1)));
            any = vorrq_s32(any, vorrq_s32(l0, l1));
        }
        m = vmaxvq_u32(vreinterpretq_u32_s32(any)) ? 1 : 0;
    } else {
        /* four rows at a time so the eight columns of one load are two
         * whole sub-blocks of one sub-block row: the occupancy falls out
         * of the levels already in registers and costs no second pass */
        for (int y = 0; y < n; y += 4) {
            for (int x = 0; x < n; x += 8) {
                int32x4_t or0 = vdupq_n_s32(0), or1 = vdupq_n_s32(0);
                int sb = (y >> 2) * sb_dim + (x >> 2);

                for (int r = 0; r < 4; r++) {
                    const int idx = (y + r) * n + x;
                    int16x8_t v = vld1q_s16(coef + idx);
                    int16x4_t vl = vget_low_s16(v), vh = vget_high_s16(v);
                    int32x4_t l0 = rtn_level4(vl, vq, vadd, vsh, vmax);
                    int32x4_t l1 = rtn_level4(vh, vq, vadd, vsh, vmax);

                    eacc = vpadalq_u32(
                        eacc, vreinterpretq_u32_s32(vmull_s16(vl, vl)));
                    eacc = vpadalq_u32(
                        eacc, vreinterpretq_u32_s32(vmull_s16(vh, vh)));
                    vst1q_s16(rtn + idx,
                              vcombine_s16(vmovn_s32(l0), vmovn_s32(l1)));
                    or0 = vorrq_s32(or0, l0);
                    or1 = vorrq_s32(or1, l1);
                }
                if (vmaxvq_u32(vreinterpretq_u32_s32(or0)))
                    m |= (uint64_t)1 << sb;
                if (vmaxvq_u32(vreinterpretq_u32_s32(or1)))
                    m |= (uint64_t)1 << (sb + 1);
            }
        }
    }

    *all_zero = (double)(vgetq_lane_u64(eacc, 0) + vgetq_lane_u64(eacc, 1));
    *sb_nz = m;
    return m != 0;
}

/* Bitwise OR of the four rows of a sub-block row, then one horizontal max
 * per half: a sub-block is occupied exactly when the OR of its sixteen
 * int16 patterns is nonzero, which needs no arithmetic and no care about
 * -32768. */
uint64_t n265_rdoq_sb_nz_neon(const int16_t *blk, int log2n)
{
    const int n = 1 << log2n, sb_dim = n >> 2;
    uint64_t m = 0;

    if (n == 4) {
        uint16x4_t v0 = vreinterpret_u16_s16(vld1_s16(blk));
        uint16x4_t v1 = vreinterpret_u16_s16(vld1_s16(blk + 4));
        uint16x4_t v2 = vreinterpret_u16_s16(vld1_s16(blk + 8));
        uint16x4_t v3 = vreinterpret_u16_s16(vld1_s16(blk + 12));
        return vmaxv_u16(vorr_u16(vorr_u16(v0, v1), vorr_u16(v2, v3))) != 0;
    }
    for (int y = 0; y < n; y += 4) {
        for (int x = 0; x < n; x += 8) {
            const int16_t *p = blk + y * n + x;
            uint16x8_t v0 = vreinterpretq_u16_s16(vld1q_s16(p));
            uint16x8_t v1 = vreinterpretq_u16_s16(vld1q_s16(p + n));
            uint16x8_t v2 = vreinterpretq_u16_s16(vld1q_s16(p + 2 * n));
            uint16x8_t v3 = vreinterpretq_u16_s16(vld1q_s16(p + 3 * n));
            uint16x8_t v = vorrq_u16(vorrq_u16(v0, v1), vorrq_u16(v2, v3));
            int sb = (y >> 2) * sb_dim + (x >> 2);

            if (vmaxv_u16(vget_low_u16(v)))
                m |= (uint64_t)1 << sb;
            if (vmaxv_u16(vget_high_u16(v)))
                m |= (uint64_t)1 << (sb + 1);
        }
    }
    return m;
}
