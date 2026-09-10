/*
 * cost_neon.c - the coefficient-costing kernels (class N265_ASM_QUANT).
 *
 * residual_coding()'s rate estimate walks the levels of a transform block
 * twice: once over EVERY 4x4 group to decide which groups are coded at
 * all (`n265_csbf_map`), and once per coded group to fetch its sixteen
 * levels in scan order as magnitude, sign and significance
 * (`n265_coeff_group`). Both are elementwise over a group and neither
 * touches the arithmetic coder, so both vectorise; the bin-by-bin context
 * walk between them does not, and stays in C.
 *
 * The 2026-09-10 shape survey (foreman_cif, 60 frames, qp 30, aq/cutree
 * on, one thread) is what sized them: 178k residual_coding calls scanned
 * 2.80M sub-blocks in the csbf pass and coded 376k of them, so 86% of the
 * first pass is a group that is never looked at again, and the pass is
 * 26% of the function's self time.
 *
 * Copyright (c) 2026, the yah265 authors. GPL-2.0-or-later; see LICENSE.
 */

#include <arm_neon.h>

#include "encoder/arm/neon.h"

/* ---- n265_csbf_map --------------------------------------------------
 *
 * out[sby * (n >> 2) + sbx] = 1 iff any of the sixteen levels of that 4x4
 * group is nonzero. The group's CONTENT is scan-independent (all three
 * scans are permutations of the same sixteen positions), so this needs no
 * scan table at all: four contiguous rows ORed together, then a
 * within-group reduction.
 *
 * The reduction is `vpmax` on the u16 reinterpretation of the OR. Max is
 * a legal "any nonzero" reduction on unsigned lanes - the max of a set of
 * u16 is zero iff every member is - and the pairwise form lands the
 * groups already in raster order, so no lane shuffling is needed:
 * vpmaxq(v0, v1) gives v0's four pairs then v1's four, and a second
 * vpmax over that gives the four groups those two vectors span.
 *
 * Range: no narrowing arithmetic anywhere. The only vmovn narrows a lane
 * that has already been masked to 0 or 1, so nothing can be truncated.
 * Reads are exactly the n*n int16 of the block and writes exactly the
 * (n>>2)^2 bytes of the map; there is no tail and no halo either way.
 */

/* 0/1 bytes from eight u16 lanes: nonzero -> 1 */
static inline uint8x8_t csbf_bytes8(uint16x8_t v)
{
    return vmovn_u16(vandq_u16(vtstq_u16(v, v), vdupq_n_u16(1)));
}

void n265_csbf_map_neon(const int16_t *level, int log2n, uint8_t *out)
{
    const uint16_t *l = (const uint16_t *)level;
    int n = 1 << log2n;
    int sb_dim = n >> 2;

    if (log2n == 2) {
        uint16x4_t v = vorr_u16(vorr_u16(vld1_u16(l), vld1_u16(l + 4)),
                                vorr_u16(vld1_u16(l + 8), vld1_u16(l + 12)));
        out[0] = vmaxv_u16(v) != 0;
        return;
    }

    for (int b = 0; b < sb_dim; b++) {
        const uint16_t *r0 = l + (size_t)(b << 2) * n;
        const uint16_t *r1 = r0 + n, *r2 = r1 + n, *r3 = r2 + n;

#define BAND_OR(off) \
        vorrq_u16(vorrq_u16(vld1q_u16(r0 + (off)), vld1q_u16(r1 + (off))), \
                  vorrq_u16(vld1q_u16(r2 + (off)), vld1q_u16(r3 + (off))))

        if (log2n == 3) {           /* one 8-lane row, two groups */
            uint16x8_t v = BAND_OR(0);
            uint16x4_t q = vpmax_u16(vpmax_u16(vget_low_u16(v),
                                               vget_high_u16(v)),
                                     vdup_n_u16(0));
            vst1_lane_u16((uint16_t *)(out + b * sb_dim),
                          vreinterpret_u16_u8(csbf_bytes8(vcombine_u16(q, q))),
                          0);
        } else if (log2n == 4) {    /* two 8-lane rows, four groups */
            uint16x8_t a = vpmaxq_u16(BAND_OR(0), BAND_OR(8));
            uint16x4_t q = vpmax_u16(vget_low_u16(a), vget_high_u16(a));
            vst1_lane_u32((uint32_t *)(out + b * sb_dim),
                          vreinterpret_u32_u8(csbf_bytes8(vcombine_u16(q, q))),
                          0);
        } else {                    /* four 8-lane rows, eight groups */
            uint16x8_t a = vpmaxq_u16(BAND_OR(0), BAND_OR(8));
            uint16x8_t c = vpmaxq_u16(BAND_OR(16), BAND_OR(24));
            vst1_u8(out + b * sb_dim, csbf_bytes8(vpmaxq_u16(a, c)));
        }
#undef BAND_OR
    }
}

/* ---- n265_coeff_group -----------------------------------------------
 *
 * One 4x4 group gathered into `scan_idx` scan order: |level| per scan
 * position, the sign mask and the significance mask.
 *
 * The four rows are 8 bytes each, so the whole group is 32 bytes and fits
 * a `vqtbl2q_u8` table exactly. The scan is then a byte permutation of
 * that table - the small-table lookup the family was scoped around - with
 * one index vector per output half. The index vectors are the spec scan
 * (6.5.3) doubled to byte pairs: position (x, y) of the group lives at
 * byte y * 8 + x * 2, and checkasm proves them against the runtime
 * n265_scan_coef rather than trusting the literal.
 *
 * Range: `vabsq_s16` is ARM ABS, which leaves -32768 as -32768; the
 * reference does `(uint16_t)(v < 0 ? -v : v)`, which is 0x8000 for the
 * same input, and the two agree bit for bit because the output is u16.
 * That is why the magnitudes come back UNSIGNED: the caller reads them as
 * an int, and 32768 is what residual_coding's scalar form computed too.
 * The masks are built by ANDing a lane-weight vector into the comparison
 * and reducing with vaddvq_u16 - no narrowing, no saturation.
 *
 * Reads: four rows of four int16 at `stride`, and not one element more.
 */

/* byte indices into the 32-byte group, per scan and per output half */
static const uint8_t scan_gidx[3][2][16] = {
    { /* diag */
        {  0,  1,  8,  9,  2,  3, 16, 17, 10, 11,  4,  5, 24, 25, 18, 19 },
        { 12, 13,  6,  7, 26, 27, 20, 21, 14, 15, 28, 29, 22, 23, 30, 31 },
    },
    { /* horizontal */
        {  0,  1,  2,  3,  4,  5,  6,  7,  8,  9, 10, 11, 12, 13, 14, 15 },
        { 16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27, 28, 29, 30, 31 },
    },
    { /* vertical */
        {  0,  1,  8,  9, 16, 17, 24, 25,  2,  3, 10, 11, 18, 19, 26, 27 },
        {  4,  5, 12, 13, 20, 21, 28, 29,  6,  7, 14, 15, 22, 23, 30, 31 },
    },
};

static const uint16_t lane_bit[8] = { 1, 2, 4, 8, 16, 32, 64, 128 };

uint32_t n265_coeff_group_neon(const int16_t *grp, ptrdiff_t stride,
                               int scan_idx, uint16_t *abs_out,
                               uint32_t *neg_mask)
{
    uint8x16x2_t tbl;
    int16x8_t v0, v1;
    uint16x8_t w = vld1q_u16(lane_bit);
    uint32_t sig, neg;

    tbl.val[0] = vreinterpretq_u8_s16(vcombine_s16(vld1_s16(grp),
                                                   vld1_s16(grp + stride)));
    tbl.val[1] = vreinterpretq_u8_s16(
        vcombine_s16(vld1_s16(grp + 2 * stride), vld1_s16(grp + 3 * stride)));

    v0 = vreinterpretq_s16_u8(
        vqtbl2q_u8(tbl, vld1q_u8(scan_gidx[scan_idx][0])));
    v1 = vreinterpretq_s16_u8(
        vqtbl2q_u8(tbl, vld1q_u8(scan_gidx[scan_idx][1])));

    vst1q_u16(abs_out, vreinterpretq_u16_s16(vabsq_s16(v0)));
    vst1q_u16(abs_out + 8, vreinterpretq_u16_s16(vabsq_s16(v1)));

    sig = vaddvq_u16(vandq_u16(vtstq_s16(v0, v0), w)) |
          ((uint32_t)vaddvq_u16(vandq_u16(vtstq_s16(v1, v1), w)) << 8);
    neg = vaddvq_u16(vandq_u16(vreinterpretq_u16_s16(vshrq_n_s16(v0, 15)), w)) |
          ((uint32_t)vaddvq_u16(vandq_u16(
              vreinterpretq_u16_s16(vshrq_n_s16(v1, 15)), w)) << 8);
    *neg_mask = neg;
    return sig;
}
