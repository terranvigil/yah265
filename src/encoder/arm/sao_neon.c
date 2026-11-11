/*
 * sao_neon.c - the SAO kernels (class N265_ASM_SAO).
 *
 * The band histogram has no vector form as a histogram, since NEON has no
 * scatter. What it has is a mask per band: a strip of rows whose samples
 * span at most SAO_NB_MAX bands is summed band by band under a compare
 * mask, the count by subtracting the mask and the difference as the
 * masked sums of `src` and `rec`. A flat fill spans one band, which is
 * the content the filter runs on. A strip that spans more goes to the C
 * reference, and so do the columns past the last whole vector.
 *
 * The edge-offset kernels take the category as sign(c - a) + sign(c - b),
 * -2..2, and sum or look up per category the same way.
 *
 * Range, 8-bit: a strip is at most SAO_STRIP rows of a block at most 64
 * wide, so a count lane sees at most 32 hits (u8) and a sum lane at most
 * 64 samples of 255 (u16). The edge row kernel flushes its lanes every
 * SAO_EO_RUN vectors for the same reason. Main10 counts in u16 and sums
 * in u32.
 *
 * Copyright (c) 2026, the yah265 authors. GPL-2.0-or-later; see LICENSE.
 */

#include <arm_neon.h>

#include "encoder/arm/neon.h"
#include "encoder/sao.h"

#define SAO_STRIP   8
#define SAO_NB_MAX  6
#define SAO_EO_RUN  64

#if BIT_DEPTH == 8

void n265_sao_band_stats_neon(const pixel *src, int src_stride,
                              const pixel *rec, int rec_stride, int w, int h,
                              int64_t bn[32], int64_t bd[32])
{
    int wv = w & ~15;
    if (wv < w)
        n265_sao_band_stats_ref(src + wv, src_stride, rec + wv, rec_stride,
                                w - wv, h, bn, bd);
    for (int y0 = 0; wv && y0 < h; y0 += SAO_STRIP) {
        int sh = h - y0 < SAO_STRIP ? h - y0 : SAO_STRIP;
        const pixel *s = src + (size_t)y0 * src_stride;
        const pixel *r = rec + (size_t)y0 * rec_stride;

        uint8x16_t mn = vdupq_n_u8(255), mx = vdupq_n_u8(0);
        for (int y = 0; y < sh; y++)
            for (int x = 0; x < wv; x += 16) {
                uint8x16_t v = vld1q_u8(r + (size_t)y * rec_stride + x);
                mn = vminq_u8(mn, v);
                mx = vmaxq_u8(mx, v);
            }
        int b0 = vminvq_u8(mn) >> 3, b1 = vmaxvq_u8(mx) >> 3;
        if (b1 - b0 >= SAO_NB_MAX) {
            n265_sao_band_stats_ref(s, src_stride, r, rec_stride, wv, sh,
                                    bn, bd);
            continue;
        }
        for (int b = b0; b <= b1; b++) {
            uint8x16_t bv = vdupq_n_u8((uint8_t)b);
            uint8x16_t cnt = vdupq_n_u8(0);
            uint16x8_t ssum = vdupq_n_u16(0), rsum = vdupq_n_u16(0);
            for (int y = 0; y < sh; y++)
                for (int x = 0; x < wv; x += 16) {
                    uint8x16_t rv = vld1q_u8(r + (size_t)y * rec_stride + x);
                    uint8x16_t sv = vld1q_u8(s + (size_t)y * src_stride + x);
                    uint8x16_t m = vceqq_u8(vshrq_n_u8(rv, 3), bv);
                    cnt = vsubq_u8(cnt, m);
                    ssum = vpadalq_u8(ssum, vandq_u8(sv, m));
                    rsum = vpadalq_u8(rsum, vandq_u8(rv, m));
                }
            bn[b] += vaddlvq_u8(cnt);
            bd[b] += (int64_t)vaddlvq_u16(ssum) - (int64_t)vaddlvq_u16(rsum);
        }
    }
}

/* sign(c - a) + sign(c - b), as int8 lanes in -2..2 */
static inline int8x16_t eo_cat2(uint8x16_t c, uint8x16_t a, uint8x16_t b)
{
    int8x16_t sa = vsubq_s8(vreinterpretq_s8_u8(vcltq_u8(c, a)),
                            vreinterpretq_s8_u8(vcgtq_u8(c, a)));
    int8x16_t sb = vsubq_s8(vreinterpretq_s8_u8(vcltq_u8(c, b)),
                            vreinterpretq_s8_u8(vcgtq_u8(c, b)));
    return vaddq_s8(sa, sb);
}

void n265_sao_eo_stats_neon(const pixel *src, const pixel *cur,
                            const pixel *a, const pixel *b, int n,
                            int64_t en[5], int64_t ed[5])
{
    static const int8_t sgn[4] = { -2, -1, 1, 2 };   /* categories 1..4 */
    int nv = n & ~15;
    for (int x0 = 0; x0 < nv; x0 += 16 * SAO_EO_RUN) {
        int x1 = x0 + 16 * SAO_EO_RUN < nv ? x0 + 16 * SAO_EO_RUN : nv;
        uint8x16_t cnt[4];
        uint16x8_t ssum[4], csum[4];
        for (int k = 0; k < 4; k++) {
            cnt[k] = vdupq_n_u8(0);
            ssum[k] = csum[k] = vdupq_n_u16(0);
        }
        for (int x = x0; x < x1; x += 16) {
            uint8x16_t c = vld1q_u8(cur + x), sv = vld1q_u8(src + x);
            int8x16_t s = eo_cat2(c, vld1q_u8(a + x), vld1q_u8(b + x));
            for (int k = 0; k < 4; k++) {
                uint8x16_t m = vceqq_s8(s, vdupq_n_s8(sgn[k]));
                cnt[k] = vsubq_u8(cnt[k], m);
                ssum[k] = vpadalq_u8(ssum[k], vandq_u8(sv, m));
                csum[k] = vpadalq_u8(csum[k], vandq_u8(c, m));
            }
        }
        for (int k = 0; k < 4; k++) {
            en[k + 1] += vaddlvq_u8(cnt[k]);
            ed[k + 1] += (int64_t)vaddlvq_u16(ssum[k]) -
                         (int64_t)vaddlvq_u16(csum[k]);
        }
    }
    if (nv < n)
        n265_sao_eo_stats_ref(src + nv, cur + nv, a + nv, b + nv, n - nv,
                              en, ed);
}

void n265_sao_eo_apply_neon(pixel *dst, const pixel *cur, const pixel *a,
                            const pixel *b, int n, const int8_t off[4])
{
    /* indexed by the category sum + 2 */
    const int8_t t[16] = { off[0], off[1], 0, off[2], off[3] };
    int8x16_t tab = vld1q_s8(t);
    int nv = n & ~15;
    for (int x = 0; x < nv; x += 16) {
        uint8x16_t c = vld1q_u8(cur + x);
        int8x16_t s = eo_cat2(c, vld1q_u8(a + x), vld1q_u8(b + x));
        int8x16_t o = vqtbl1q_s8(tab, vreinterpretq_u8_s8(vaddq_s8(s, vdupq_n_s8(2))));
        uint8x16_t res = vsqaddq_u8(c, o);
        vst1q_u8(dst + x, vbslq_u8(vtstq_s8(s, s), res, vld1q_u8(dst + x)));
    }
    if (nv < n)
        n265_sao_eo_apply_ref(dst + nv, cur + nv, a + nv, b + nv, n - nv, off);
}

void n265_sao_bo_apply_neon(pixel *dst, const pixel *in, int n, int pos,
                            const int8_t off[4])
{
    int8_t t[32] = { 0 };
    for (int i = 0; i < 4 && pos + i < 32; i++)
        t[pos + i] = off[i];
    int8x16x2_t tab = { { vld1q_s8(t), vld1q_s8(t + 16) } };
    uint8x16_t pv = vdupq_n_u8((uint8_t)pos), four = vdupq_n_u8(4);
    int nv = n & ~15;
    for (int x = 0; x < nv; x += 16) {
        uint8x16_t v = vld1q_u8(in + x);
        uint8x16_t band = vshrq_n_u8(v, 3);
        uint8x16_t m = vcltq_u8(vsubq_u8(band, pv), four);
        uint8x16_t res = vsqaddq_u8(v, vqtbl2q_s8(tab, band));
        vst1q_u8(dst + x, vbslq_u8(m, res, vld1q_u8(dst + x)));
    }
    if (nv < n)
        n265_sao_bo_apply_ref(dst + nv, in + nv, n - nv, pos, off);
}

#else /* Main10: eight u16 lanes */

void n265_sao_band_stats_neon(const pixel *src, int src_stride,
                              const pixel *rec, int rec_stride, int w, int h,
                              int64_t bn[32], int64_t bd[32])
{
    int wv = w & ~7;
    if (wv < w)
        n265_sao_band_stats_ref(src + wv, src_stride, rec + wv, rec_stride,
                                w - wv, h, bn, bd);
    for (int y0 = 0; wv && y0 < h; y0 += SAO_STRIP) {
        int sh = h - y0 < SAO_STRIP ? h - y0 : SAO_STRIP;
        const pixel *s = src + (size_t)y0 * src_stride;
        const pixel *r = rec + (size_t)y0 * rec_stride;

        uint16x8_t mn = vdupq_n_u16(PIXEL_MAX), mx = vdupq_n_u16(0);
        for (int y = 0; y < sh; y++)
            for (int x = 0; x < wv; x += 8) {
                uint16x8_t v = vld1q_u16(r + (size_t)y * rec_stride + x);
                mn = vminq_u16(mn, v);
                mx = vmaxq_u16(mx, v);
            }
        int b0 = vminvq_u16(mn) >> (BIT_DEPTH - 5);
        int b1 = vmaxvq_u16(mx) >> (BIT_DEPTH - 5);
        if (b1 - b0 >= SAO_NB_MAX) {
            n265_sao_band_stats_ref(s, src_stride, r, rec_stride, wv, sh,
                                    bn, bd);
            continue;
        }
        for (int b = b0; b <= b1; b++) {
            uint16x8_t bv = vdupq_n_u16((uint16_t)b);
            uint16x8_t cnt = vdupq_n_u16(0);
            uint32x4_t ssum = vdupq_n_u32(0), rsum = vdupq_n_u32(0);
            for (int y = 0; y < sh; y++)
                for (int x = 0; x < wv; x += 8) {
                    uint16x8_t rv = vld1q_u16(r + (size_t)y * rec_stride + x);
                    uint16x8_t sv = vld1q_u16(s + (size_t)y * src_stride + x);
                    uint16x8_t m = vceqq_u16(vshrq_n_u16(rv, BIT_DEPTH - 5), bv);
                    cnt = vsubq_u16(cnt, m);
                    ssum = vpadalq_u16(ssum, vandq_u16(sv, m));
                    rsum = vpadalq_u16(rsum, vandq_u16(rv, m));
                }
            bn[b] += vaddlvq_u16(cnt);
            bd[b] += (int64_t)vaddlvq_u32(ssum) - (int64_t)vaddlvq_u32(rsum);
        }
    }
}

static inline int16x8_t eo_cat2(uint16x8_t c, uint16x8_t a, uint16x8_t b)
{
    int16x8_t sa = vsubq_s16(vreinterpretq_s16_u16(vcltq_u16(c, a)),
                             vreinterpretq_s16_u16(vcgtq_u16(c, a)));
    int16x8_t sb = vsubq_s16(vreinterpretq_s16_u16(vcltq_u16(c, b)),
                             vreinterpretq_s16_u16(vcgtq_u16(c, b)));
    return vaddq_s16(sa, sb);
}

void n265_sao_eo_stats_neon(const pixel *src, const pixel *cur,
                            const pixel *a, const pixel *b, int n,
                            int64_t en[5], int64_t ed[5])
{
    static const int16_t sgn[4] = { -2, -1, 1, 2 };
    int nv = n & ~7;
    for (int x0 = 0; x0 < nv; x0 += 8 * SAO_EO_RUN) {
        int x1 = x0 + 8 * SAO_EO_RUN < nv ? x0 + 8 * SAO_EO_RUN : nv;
        uint16x8_t cnt[4];
        uint32x4_t ssum[4], csum[4];
        for (int k = 0; k < 4; k++) {
            cnt[k] = vdupq_n_u16(0);
            ssum[k] = csum[k] = vdupq_n_u32(0);
        }
        for (int x = x0; x < x1; x += 8) {
            uint16x8_t c = vld1q_u16(cur + x), sv = vld1q_u16(src + x);
            int16x8_t s = eo_cat2(c, vld1q_u16(a + x), vld1q_u16(b + x));
            for (int k = 0; k < 4; k++) {
                uint16x8_t m = vceqq_s16(s, vdupq_n_s16(sgn[k]));
                cnt[k] = vsubq_u16(cnt[k], m);
                ssum[k] = vpadalq_u16(ssum[k], vandq_u16(sv, m));
                csum[k] = vpadalq_u16(csum[k], vandq_u16(c, m));
            }
        }
        for (int k = 0; k < 4; k++) {
            en[k + 1] += vaddlvq_u16(cnt[k]);
            ed[k + 1] += (int64_t)vaddlvq_u32(ssum[k]) -
                         (int64_t)vaddlvq_u32(csum[k]);
        }
    }
    if (nv < n)
        n265_sao_eo_stats_ref(src + nv, cur + nv, a + nv, b + nv, n - nv,
                              en, ed);
}

static inline int16x8_t clip1(int16x8_t v)
{
    return vmaxq_s16(vminq_s16(v, vdupq_n_s16(PIXEL_MAX)), vdupq_n_s16(0));
}

void n265_sao_eo_apply_neon(pixel *dst, const pixel *cur, const pixel *a,
                            const pixel *b, int n, const int8_t off[4])
{
    int16x8_t o0 = vdupq_n_s16(off[0]), o1 = vdupq_n_s16(off[1]);
    int16x8_t o2 = vdupq_n_s16(off[2]), o3 = vdupq_n_s16(off[3]);
    int nv = n & ~7;
    for (int x = 0; x < nv; x += 8) {
        uint16x8_t c = vld1q_u16(cur + x);
        int16x8_t s = eo_cat2(c, vld1q_u16(a + x), vld1q_u16(b + x));
        int16x8_t o = vbslq_s16(vceqq_s16(s, vdupq_n_s16(-2)), o0,
                      vbslq_s16(vceqq_s16(s, vdupq_n_s16(-1)), o1,
                      vbslq_s16(vceqq_s16(s, vdupq_n_s16(1)), o2, o3)));
        uint16x8_t res = vreinterpretq_u16_s16(
            clip1(vaddq_s16(vreinterpretq_s16_u16(c), o)));
        vst1q_u16(dst + x, vbslq_u16(vtstq_s16(s, s), res, vld1q_u16(dst + x)));
    }
    if (nv < n)
        n265_sao_eo_apply_ref(dst + nv, cur + nv, a + nv, b + nv, n - nv, off);
}

void n265_sao_bo_apply_neon(pixel *dst, const pixel *in, int n, int pos,
                            const int8_t off[4])
{
    int16x8_t o0 = vdupq_n_s16(off[0]), o1 = vdupq_n_s16(off[1]);
    int16x8_t o2 = vdupq_n_s16(off[2]), o3 = vdupq_n_s16(off[3]);
    uint16x8_t pv = vdupq_n_u16((uint16_t)pos);
    int nv = n & ~7;
    for (int x = 0; x < nv; x += 8) {
        uint16x8_t v = vld1q_u16(in + x);
        uint16x8_t i = vsubq_u16(vshrq_n_u16(v, BIT_DEPTH - 5), pv);
        int16x8_t o = vbslq_s16(vceqq_u16(i, vdupq_n_u16(0)), o0,
                      vbslq_s16(vceqq_u16(i, vdupq_n_u16(1)), o1,
                      vbslq_s16(vceqq_u16(i, vdupq_n_u16(2)), o2, o3)));
        uint16x8_t res = vreinterpretq_u16_s16(
            clip1(vaddq_s16(vreinterpretq_s16_u16(v), o)));
        vst1q_u16(dst + x, vbslq_u16(vcltq_u16(i, vdupq_n_u16(4)), res,
                                     vld1q_u16(dst + x)));
    }
    if (nv < n)
        n265_sao_bo_apply_ref(dst + nv, in + nv, n - nv, pos, off);
}

#endif
