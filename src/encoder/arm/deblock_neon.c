/*
 * deblock_neon.c - the deblocking filter kernels (class N265_ASM_DBK).
 *
 * Every sample is one int16 lane at both depths. A luma vector is two
 * four-line segments side by side, so a segment's decisions, taken from
 * its lines 0 and 3, are broadcast to its four lanes with one table
 * lookup. Every filter output is computed for every lane and the decisions
 * pick between them, which is the C's arithmetic lane by lane.
 *
 * Range: the largest intermediate is 9 * (q0 - p0) - 3 * (q1 - p1), at
 * most 12 * PIXEL_MAX = 12276 at Main10, and the strong filter's sums are
 * at most 8 * PIXEL_MAX + 4. Both fit int16.
 *
 * A vertical edge's eight lines are transposed into lanes and back, and
 * stored whole, so p3 and q3 are rewritten with the values they held.
 * An odd segment or a chroma tail under eight lines goes to the C
 * reference.
 *
 * Copyright (c) 2026, the yah265 authors. GPL-2.0-or-later; see LICENSE.
 */

#include <arm_neon.h>

#include "encoder/arm/neon.h"
#include "encoder/deblock.h"

#if BIT_DEPTH == 8
static inline int16x8_t ld8(const pixel *p)
{
    return vreinterpretq_s16_u16(vmovl_u8(vld1_u8(p)));
}

static inline void st8(pixel *p, int16x8_t v)
{
    vst1_u8(p, vmovn_u16(vreinterpretq_u16_s16(v)));
}
#else
static inline int16x8_t ld8(const pixel *p)
{
    return vreinterpretq_s16_u16(vld1q_u16(p));
}

static inline void st8(pixel *p, int16x8_t v)
{
    vst1q_u16(p, vreinterpretq_u16_s16(v));
}
#endif

static inline int16x8_t clamp3(int16x8_t v, int16x8_t lo, int16x8_t hi)
{
    return vmaxq_s16(vminq_s16(v, hi), lo);
}

static inline int16x8_t clip1(int16x8_t v)
{
    return clamp3(v, vdupq_n_s16(0), vdupq_n_s16(PIXEL_MAX));
}

static inline int16x8_t pick(uint8x16_t idx, int16x8_t v)
{
    return vreinterpretq_s16_u8(vqtbl1q_u8(vreinterpretq_u8_s16(v), idx));
}

static inline uint16x8_t pick_m(uint8x16_t idx, uint16x8_t v)
{
    return vreinterpretq_u16_u8(vqtbl1q_u8(vreinterpretq_u8_u16(v), idx));
}

static inline int16x8_t seg_pair(const int16_t *v)
{
    return vcombine_s16(vdup_n_s16(v[0]), vdup_n_s16(v[1]));
}

/* 8x8 transpose of int16 lanes, its own inverse */
static inline void transpose8(int16x8_t r[8])
{
    int16x8_t a[8], b[8];
    for (int i = 0; i < 8; i += 2) {
        a[i] = vtrn1q_s16(r[i], r[i + 1]);
        a[i + 1] = vtrn2q_s16(r[i], r[i + 1]);
    }
    for (int i = 0; i < 8; i += 4)
        for (int j = 0; j < 2; j++) {
            int32x4_t x = vreinterpretq_s32_s16(a[i + j]);
            int32x4_t y = vreinterpretq_s32_s16(a[i + j + 2]);
            b[i + j] = vreinterpretq_s16_s32(vtrn1q_s32(x, y));
            b[i + j + 2] = vreinterpretq_s16_s32(vtrn2q_s32(x, y));
        }
    for (int j = 0; j < 4; j++) {
        int64x2_t x = vreinterpretq_s64_s16(b[j]);
        int64x2_t y = vreinterpretq_s64_s16(b[j + 4]);
        r[j] = vreinterpretq_s16_s64(vtrn1q_s64(x, y));
        r[j + 4] = vreinterpretq_s16_s64(vtrn2q_s64(x, y));
    }
}

/* lanes of each segment's line 0 and line 3 */
static const uint8_t line0_idx[16] = { 0, 1, 0, 1, 0, 1, 0, 1,
                                       8, 9, 8, 9, 8, 9, 8, 9 };
static const uint8_t line3_idx[16] = { 6, 7, 6, 7, 6, 7, 6, 7,
                                       14, 15, 14, 15, 14, 15, 14, 15 };

/* v[0..7] = p3..q3 across the edge, one line per lane; rewrites p2..q2.
 * Returns 0, touching nothing, when neither segment is filtered. */
static inline int luma_pair(int16x8_t v[8], int16x8_t beta, int16x8_t tc)
{
    int16x8_t p3 = v[0], p2 = v[1], p1 = v[2], p0 = v[3];
    int16x8_t q0 = v[4], q1 = v[5], q2 = v[6], q3 = v[7];
    uint8x16_t i0 = vld1q_u8(line0_idx), i3 = vld1q_u8(line3_idx);

    int16x8_t dp = vabsq_s16(vsubq_s16(vaddq_s16(p2, p0), vshlq_n_s16(p1, 1)));
    int16x8_t dq = vabsq_s16(vsubq_s16(vaddq_s16(q2, q0), vshlq_n_s16(q1, 1)));
    int16x8_t dp03 = vaddq_s16(pick(i0, dp), pick(i3, dp));
    int16x8_t dq03 = vaddq_s16(pick(i0, dq), pick(i3, dq));
    uint16x8_t on = vcltq_s16(vaddq_s16(dp03, dq03), beta);
    if (!vmaxvq_u16(on))
        return 0;

    /* 8.7.2.5.6 per line, then both of a segment's lines must agree */
    uint16x8_t sl = vandq_u16(
        vandq_u16(vcltq_s16(vshlq_n_s16(vaddq_s16(dp, dq), 1),
                            vshrq_n_s16(beta, 2)),
                  vcltq_s16(vaddq_s16(vabdq_s16(p3, p0), vabdq_s16(q0, q3)),
                            vshrq_n_s16(beta, 3))),
        vcltq_s16(vabdq_s16(p0, q0),
                  vshrq_n_s16(vmlaq_n_s16(vdupq_n_s16(1), tc, 5), 1)));
    uint16x8_t strong = vandq_u16(vandq_u16(pick_m(i0, sl), pick_m(i3, sl)),
                                  on);
    int16x8_t side = vshrq_n_s16(vaddq_s16(beta, vshrq_n_s16(beta, 1)), 3);
    uint16x8_t dep = vcltq_s16(dp03, side), deq = vcltq_s16(dq03, side);

    /* strong filter */
    int16x8_t tc2 = vshlq_n_s16(tc, 1);
    int16x8_t pq0 = vaddq_s16(p0, q0);
    int16x8_t sp0 = vrshrq_n_s16(vaddq_s16(vaddq_s16(p2, q1),
                                           vshlq_n_s16(vaddq_s16(p1, pq0), 1)), 3);
    int16x8_t sp1 = vrshrq_n_s16(vaddq_s16(vaddq_s16(p2, p1), pq0), 2);
    int16x8_t sp2 = vrshrq_n_s16(vaddq_s16(vmlaq_n_s16(vshlq_n_s16(p3, 1), p2, 3),
                                           vaddq_s16(p1, pq0)), 3);
    int16x8_t sq0 = vrshrq_n_s16(vaddq_s16(vaddq_s16(q2, p1),
                                           vshlq_n_s16(vaddq_s16(q1, pq0), 1)), 3);
    int16x8_t sq1 = vrshrq_n_s16(vaddq_s16(vaddq_s16(q2, q1), pq0), 2);
    int16x8_t sq2 = vrshrq_n_s16(vaddq_s16(vmlaq_n_s16(vshlq_n_s16(q3, 1), q2, 3),
                                           vaddq_s16(q1, pq0)), 3);
    sp0 = clip1(clamp3(sp0, vsubq_s16(p0, tc2), vaddq_s16(p0, tc2)));
    sp1 = clip1(clamp3(sp1, vsubq_s16(p1, tc2), vaddq_s16(p1, tc2)));
    sp2 = clip1(clamp3(sp2, vsubq_s16(p2, tc2), vaddq_s16(p2, tc2)));
    sq0 = clip1(clamp3(sq0, vsubq_s16(q0, tc2), vaddq_s16(q0, tc2)));
    sq1 = clip1(clamp3(sq1, vsubq_s16(q1, tc2), vaddq_s16(q1, tc2)));
    sq2 = clip1(clamp3(sq2, vsubq_s16(q2, tc2), vaddq_s16(q2, tc2)));

    /* normal filter */
    int16x8_t ntc = vnegq_s16(tc);
    int16x8_t delta = vrshrq_n_s16(
        vmlsq_n_s16(vmulq_n_s16(vsubq_s16(q0, p0), 9), vsubq_s16(q1, p1), 3), 4);
    uint16x8_t nok = vcltq_s16(vabsq_s16(delta), vmulq_n_s16(tc, 10));
    delta = clamp3(delta, ntc, tc);
    int16x8_t np0 = clip1(vaddq_s16(p0, delta));
    int16x8_t nq0 = clip1(vsubq_s16(q0, delta));
    int16x8_t tch = vshrq_n_s16(tc, 1), ntch = vnegq_s16(tch);
    int16x8_t np1 = clip1(vaddq_s16(p1, clamp3(vshrq_n_s16(
        vaddq_s16(vsubq_s16(vrhaddq_s16(p2, p0), p1), delta), 1), ntch, tch)));
    int16x8_t nq1 = clip1(vaddq_s16(q1, clamp3(vshrq_n_s16(
        vsubq_s16(vsubq_s16(vrhaddq_s16(q2, q0), q1), delta), 1), ntch, tch)));

    uint16x8_t nm = vbicq_u16(vandq_u16(on, nok), strong);
    v[1] = vbslq_s16(strong, sp2, p2);
    v[2] = vbslq_s16(strong, sp1, vbslq_s16(vandq_u16(nm, dep), np1, p1));
    v[3] = vbslq_s16(strong, sp0, vbslq_s16(nm, np0, p0));
    v[4] = vbslq_s16(strong, sq0, vbslq_s16(nm, nq0, q0));
    v[5] = vbslq_s16(strong, sq1, vbslq_s16(vandq_u16(nm, deq), nq1, q1));
    v[6] = vbslq_s16(strong, sq2, q2);
    return 1;
}

void n265_dbk_luma_v_neon(pixel *pix, ptrdiff_t stride, const int16_t *beta,
                          const int16_t *tc, int nseg)
{
    int k = 0;
    for (; k + 2 <= nseg; k += 2) {
        if (!(tc[k] | tc[k + 1]))
            continue;
        pixel *b = pix + 4 * k * stride - 4;
        int16x8_t r[8];
        for (int i = 0; i < 8; i++)
            r[i] = ld8(b + i * stride);
        transpose8(r);
        if (!luma_pair(r, seg_pair(beta + k), seg_pair(tc + k)))
            continue;
        transpose8(r);
        for (int i = 0; i < 8; i++)
            st8(b + i * stride, r[i]);
    }
    if (k < nseg)
        n265_dbk_luma_v_ref(pix + 4 * k * stride, stride, beta + k, tc + k,
                            nseg - k);
}

void n265_dbk_luma_h_neon(pixel *pix, ptrdiff_t stride, const int16_t *beta,
                          const int16_t *tc, int nseg)
{
    int k = 0;
    for (; k + 2 <= nseg; k += 2) {
        if (!(tc[k] | tc[k + 1]))
            continue;
        pixel *b = pix + 4 * k;
        int16x8_t r[8];
        for (int i = 0; i < 8; i++)
            r[i] = ld8(b + (i - 4) * stride);
        if (!luma_pair(r, seg_pair(beta + k), seg_pair(tc + k)))
            continue;
        for (int i = 1; i < 7; i++)
            st8(b + (i - 4) * stride, r[i]);
    }
    if (k < nseg)
        n265_dbk_luma_h_ref(pix + 4 * k, stride, beta + k, tc + k, nseg - k);
}

/* 8.7.2.5.5 on eight lines: returns p0' and q0' */
static inline void chroma8(int16x8_t p1, int16x8_t *p0, int16x8_t *q0,
                           int16x8_t q1, int16x8_t tc)
{
    int16x8_t d = vrshrq_n_s16(
        vsubq_s16(vaddq_s16(vshlq_n_s16(vsubq_s16(*q0, *p0), 2), p1), q1), 3);
    d = clamp3(d, vnegq_s16(tc), tc);
    *p0 = clip1(vaddq_s16(*p0, d));
    *q0 = clip1(vsubq_s16(*q0, d));
}

void n265_dbk_chroma_h_neon(pixel *pix, ptrdiff_t stride, const int16_t *tc,
                            int n)
{
    int i = 0;
    for (; i + 8 <= n; i += 8) {
        int16x8_t t = vld1q_s16(tc + i);
        if (!vmaxvq_s16(t))
            continue;
        pixel *b = pix + i;
        int16x8_t p1 = ld8(b - 2 * stride), p0 = ld8(b - stride);
        int16x8_t q0 = ld8(b), q1 = ld8(b + stride);
        chroma8(p1, &p0, &q0, q1, t);
        st8(b - stride, p0);
        st8(b, q0);
    }
    if (i < n)
        n265_dbk_chroma_h_ref(pix + i, stride, tc + i, n - i);
}

#if BIT_DEPTH == 8
#define CV_LD(j) v = vld4_lane_u8(b + (j) * stride - 2, v, j)
#define CV_ST(j) vst2_lane_u8(b + (j) * stride - 1, o, j)
#else
#define CV_LD(j) v = vld4q_lane_u16(b + (j) * stride - 2, v, j)
#define CV_ST(j) vst2q_lane_u16(b + (j) * stride - 1, o, j)
#endif

void n265_dbk_chroma_v_neon(pixel *pix, ptrdiff_t stride, const int16_t *tc,
                            int n)
{
    int i = 0;
    for (; i + 8 <= n; i += 8) {
        int16x8_t t = vld1q_s16(tc + i);
        if (!vmaxvq_s16(t))
            continue;
        pixel *b = pix + i * stride;
#if BIT_DEPTH == 8
        uint8x8x4_t v = { { vdup_n_u8(0), vdup_n_u8(0), vdup_n_u8(0),
                            vdup_n_u8(0) } };
#else
        uint16x8x4_t v = { { vdupq_n_u16(0), vdupq_n_u16(0), vdupq_n_u16(0),
                             vdupq_n_u16(0) } };
#endif
        CV_LD(0); CV_LD(1); CV_LD(2); CV_LD(3);
        CV_LD(4); CV_LD(5); CV_LD(6); CV_LD(7);
#if BIT_DEPTH == 8
        int16x8_t p1 = vreinterpretq_s16_u16(vmovl_u8(v.val[0]));
        int16x8_t p0 = vreinterpretq_s16_u16(vmovl_u8(v.val[1]));
        int16x8_t q0 = vreinterpretq_s16_u16(vmovl_u8(v.val[2]));
        int16x8_t q1 = vreinterpretq_s16_u16(vmovl_u8(v.val[3]));
        chroma8(p1, &p0, &q0, q1, t);
        uint8x8x2_t o = { { vmovn_u16(vreinterpretq_u16_s16(p0)),
                            vmovn_u16(vreinterpretq_u16_s16(q0)) } };
#else
        int16x8_t p1 = vreinterpretq_s16_u16(v.val[0]);
        int16x8_t p0 = vreinterpretq_s16_u16(v.val[1]);
        int16x8_t q0 = vreinterpretq_s16_u16(v.val[2]);
        int16x8_t q1 = vreinterpretq_s16_u16(v.val[3]);
        chroma8(p1, &p0, &q0, q1, t);
        uint16x8x2_t o = { { vreinterpretq_u16_s16(p0),
                             vreinterpretq_u16_s16(q0) } };
#endif
        CV_ST(0); CV_ST(1); CV_ST(2); CV_ST(3);
        CV_ST(4); CV_ST(5); CV_ST(6); CV_ST(7);
    }
    if (i < n)
        n265_dbk_chroma_v_ref(pix + i * stride, stride, tc + i, n - i);
}
