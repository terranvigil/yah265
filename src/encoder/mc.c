/*
 * Interpolation filters (ITU-T H.265 Table 8-11/8-12): 8-tap luma at
 * quarter-pel, 4-tap chroma at eighth-pel. Uni-prediction rounding:
 * 14-bit intermediate, (v + 32) >> 6 at the end.
 * Copyright (c) 2026, the yah265 authors. GPL-2.0-or-later; see LICENSE.
 */

#include <stddef.h>
#include <string.h>

#include "common/cpu.h"
#include "encoder/arm/neon.h"
#include "encoder/arm/sve.h"    /* self-guarded */
#include "encoder/x86/avx2.h"   /* self-guarded */
#include "encoder/x86/sse42.h"   /* self-guarded */
#include "encoder/mc.h"
#include "encoder/refchk.h"

static const int8_t luma_filt[4][8] = {
    {  0, 0,   0, 64,  0,   0, 0,  0 },
    { -1, 4, -10, 58, 17,  -5, 1,  0 },
    { -1, 4, -11, 40, 40, -11, 4, -1 },
    {  0, 1,  -5, 17, 58, -10, 4, -1 },
};
static const int8_t chroma_filt[8][4] = {
    {  0, 64,  0,  0 },
    { -2, 58, 10, -2 },
    { -4, 54, 16, -2 },
    { -6, 46, 28, -4 },
    { -4, 36, 36, -4 },
    { -4, 28, 46, -6 },
    { -2, 16, 54, -4 },
    { -2, 10, 58, -2 },
};

/* 8.5.3.3.2 fetches every reference sample at Clip3(0, pic - 1, ...):
 * the edge sample repeats without limit. A picture here carries a finite
 * N265_REF_PAD border instead, which is the same thing only while the
 * read stays inside it. The motion SEARCH is clamped to that border
 * (n265_clamp_mv), but a merge or AMVP candidate is inherited from a
 * neighbor and is NOT clamped - the spec does not clamp it and a
 * decoder does not have to - so a candidate can reach past the border,
 * and the samples after it are the next row's left border rather than the
 * edge sample.
 *
 * Returns the block origin a filter may read `hl` samples left and above
 * and `hr` right and below of: the plane itself whenever that whole
 * window is inside the border (the common case, which copies nothing),
 * otherwise `tmp`, gathered with the spec's clipping. `tmp` must hold
 * (w + hl + hr) * (h + hl + hr) samples.
 *
 * Under N265_REF_CHECK the gather reports the extreme lines it really
 * reads - which are the clipped ones, never past the guard - and then
 * opens the guard for the kernel, whose reads land in tmp. */
const pixel *n265_ref_win(const pixel *plane, int stride, int pic_w,
                            int pic_h, int pad_x, int pad_y, int ix, int iy,
                            int w, int h, int hl, int hr, pixel *tmp,
                            int *out_stride, int *clipped)
{
    if (ix - hl >= -pad_x && ix + w + hr <= pic_w + pad_x &&
        iy - hl >= -pad_y && iy + h + hr <= pic_h + pad_y) {
        *out_stride = stride;
        *clipped = 0;
        return plane + (ptrdiff_t)iy * stride + ix;
    }
    int tw = w + hl + hr, th = h + hl + hr;
    int y_lo = iy - hl, y_hi = iy + h + hr - 1;
    y_lo = y_lo < 0 ? 0 : y_lo > pic_h - 1 ? pic_h - 1 : y_lo;
    y_hi = y_hi < 0 ? 0 : y_hi > pic_h - 1 ? pic_h - 1 : y_hi;
    N265_RC_TOUCH(plane + (ptrdiff_t)y_lo * stride);
    N265_RC_TOUCH(plane + (ptrdiff_t)y_hi * stride);
    for (int y = 0; y < th; y++) {
        int sy = iy - hl + y;
        sy = sy < 0 ? 0 : sy > pic_h - 1 ? pic_h - 1 : sy;
        const pixel *sp = plane + (ptrdiff_t)sy * stride;
        for (int x = 0; x < tw; x++) {
            int sx = ix - hl + x;
            sx = sx < 0 ? 0 : sx > pic_w - 1 ? pic_w - 1 : sx;
            tmp[y * tw + x] = sp[sx];
        }
    }
    N265_REF_OPEN_ALL_KERNEL();
    *out_stride = tw;
    *clipped = 1;
    return tmp + (ptrdiff_t)hl * tw + hl;
}

/* 8.5.3.3.3.2's three shifts, as constants rather than as the literals
 * the 8-bit encoder wrote out. shift1 is what the horizontal stage sheds
 * on the way into the 14-bit intermediate, shift2 (6, depth-independent)
 * what the vertical stage sheds, and shift3 what the final rounding
 * sheds on the way back to a sample.
 *
 * Two of the folds below survive the widening untouched, which is why
 * the 8-bit code could write them as literals in the first place:
 * shift1 + shift3 == 6 at EVERY depth, so a one-stage filter's
 * (s + 32) >> 6 is depth-independent, and so is a 2-D filter's
 * (s + 2048) >> 12 when its intermediate has NOT been shifted. Folding
 * two truncating shifts into one is exact - ((x >> a) + (1 << (b-1))) >> b
 * equals (x + (1 << (a+b-1))) >> (a+b) for every x - so nothing here is
 * an approximation. What forces shift1 to be spelled out at all is int16:
 * an intermediate that is STORED has to be the 14-bit value the spec
 * defines or it overflows at 10 bits, and everything downstream of a
 * stored intermediate then shifts by the remainder. */
#define MC_SHIFT1   (BIT_DEPTH - 8)         /* 0 at 8 bits */
#define MC_SHIFT3   (14 - BIT_DEPTH)        /* 6 at 8 bits */
#define MC_ROUND3   (1 << (MC_SHIFT3 - 1))  /* 32 */
/* shift2 + shift3: the tail of a 2-D filter whose intermediate carries
 * shift1 already. 12 and 2048 at 8 bits, which is what was written. */
#define MC_SHIFT23  (6 + MC_SHIFT3)
#define MC_ROUND23  (1 << (MC_SHIFT23 - 1))

static inline pixel clip1(int v)
{
    return (pixel)(v < 0 ? 0 : v > PIXEL_MAX ? PIXEL_MAX : v);
}

void n265_mc_luma_wh(pixel *dst, int ds, int w, int h,
                     const pixel *ref, int ref_stride, int fx, int fy)
{
    /* the extreme lines this call reads, reported from inside the filter
     * rather than from the caller's bound (refchk.h) */
    N265_RC_TOUCH(ref + (fy ? -3 : 0) * ref_stride);
    N265_RC_TOUCH(ref + (fy ? h + 3 : h - 1) * ref_stride);
#if N265_NEON_MC
    if (n265_asm_on(N265_ASM_MC)) {
        n265_mc_luma_wh_neon(dst, ds, w, h, ref, ref_stride, fx, fy);
        return;
    }
#endif
#if N265_AVX2_MC
    if (n265_avx2_on(N265_ASM_MC)) {
        n265_mc_luma_wh_avx2(dst, ds, w, h, ref, ref_stride, fx, fy);
        return;
    }
#endif
#if N265_SSE42_MC
    if (n265_sse42_on(N265_ASM_MC)) {
        n265_mc_luma_wh_sse42(dst, ds, w, h, ref, ref_stride, fx, fy);
        return;
    }
#endif
    n265_mc_luma_wh_ref(dst, ds, w, h, ref, ref_stride, fx, fy);
}

void n265_mc_luma_wh_ref(pixel *dst, int ds, int w, int h,
                         const pixel *ref, int ref_stride, int fx, int fy)
{
    if (!fx && !fy) {
        for (int y = 0; y < h; y++)
            for (int x = 0; x < w; x++)
                dst[y * ds + x] = ref[y * ref_stride + x];
        return;
    }
    if (!fy) {          /* horizontal only: (sum + 32) >> 6 */
        const int8_t *f = luma_filt[fx];
        for (int y = 0; y < h; y++)
            for (int x = 0; x < w; x++) {
                int s = 0;
                for (int k = 0; k < 8; k++)
                    s += f[k] * ref[y * ref_stride + x + k - 3];
                dst[y * ds + x] = clip1((s + 32) >> 6);
            }
        return;
    }
    if (!fx) {          /* vertical only */
        const int8_t *f = luma_filt[fy];
        for (int y = 0; y < h; y++)
            for (int x = 0; x < w; x++) {
                int s = 0;
                for (int k = 0; k < 8; k++)
                    s += f[k] * ref[(y + k - 3) * ref_stride + x];
                dst[y * ds + x] = clip1((s + 32) >> 6);
            }
        return;
    }
    /* 2-D: horizontal to 16-bit temp rows (3 above, 4 below), then
     * vertical with >> 12 */
    int16_t tmp[(64 + 7) * 64];
    const int8_t *fh = luma_filt[fx], *fv = luma_filt[fy];
    for (int y = -3; y < h + 4; y++)
        for (int x = 0; x < w; x++) {
            int s = 0;
            for (int k = 0; k < 8; k++)
                s += fh[k] * ref[y * ref_stride + x + k - 3];
            tmp[(y + 3) * w + x] = (int16_t)(s >> MC_SHIFT1);
        }
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            int s = 0;
            for (int k = 0; k < 8; k++)
                s += fv[k] * tmp[(y + k) * w + x];
            dst[y * ds + x] = clip1((s + MC_ROUND23) >> MC_SHIFT23);
        }
}

void n265_mc_luma_hplane(int16_t *dst, int ds, int w, int h,
                         const pixel *ref, int ref_stride, int fx)
{
    N265_RC_TOUCH(ref);
    N265_RC_TOUCH(ref + (h - 1) * ref_stride);
#if N265_NEON_MC
    if (n265_asm_on(N265_ASM_MC)) {
        n265_mc_luma_hplane_neon(dst, ds, w, h, ref, ref_stride, fx);
        return;
    }
#endif
#if N265_AVX2_MC
    if (n265_avx2_on(N265_ASM_MC)) {
        n265_mc_luma_hplane_avx2(dst, ds, w, h, ref, ref_stride, fx);
        return;
    }
#endif
#if N265_SSE42_MC
    if (n265_sse42_on(N265_ASM_MC)) {
        n265_mc_luma_hplane_sse42(dst, ds, w, h, ref, ref_stride, fx);
        return;
    }
#endif
    n265_mc_luma_hplane_ref(dst, ds, w, h, ref, ref_stride, fx);
}

void n265_mc_luma_hplane_ref(int16_t *dst, int ds, int w, int h,
                             const pixel *ref, int ref_stride, int fx)
{
    const int8_t *f = luma_filt[fx];
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            int s = 0;
            for (int k = 0; k < 8; k++)
                s += f[k] * ref[y * ref_stride + x + k - 3];
            dst[y * ds + x] = (int16_t)(s >> MC_SHIFT1);
        }
}

/* The cached-first-stage forms. The plain path's horizontal-only branch
 * emits clip1((s + 32) >> 6) of exactly the value cached here, and its
 * 2-D branch runs the vertical filter over exactly these int16 rows, so
 * both arms below reproduce it bit for bit.
 *
 * The `_ref` twins below carry the arithmetic and take `hf` alone: by the
 * time the entry point reaches them the reference plane, its stride and
 * fx have all been consumed by the fall-through above, and the kernel is
 * a function of the cached rows and fy only. That is the one place this
 * family departs from neon.h's "same signature as the entry point" - it
 * would mean three dead arguments on the hottest call in the encoder. */
void n265_mc_luma_hf_ref(pixel *dst, int ds, int w, int h,
                         const int16_t *hf, int hs, int fy)
{
    if (!fy) {
        for (int y = 0; y < h; y++)
            for (int x = 0; x < w; x++)
                dst[y * ds + x] =
                    clip1((hf[y * hs + x] + MC_ROUND3) >> MC_SHIFT3);
        return;
    }
    const int8_t *fv = luma_filt[fy];
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            int s = 0;
            for (int k = 0; k < 8; k++)
                s += fv[k] * hf[(y + k - 3) * hs + x];
            dst[y * ds + x] = clip1((s + MC_ROUND23) >> MC_SHIFT23);
        }
}

/* Phase 6 round 3. No NEON dispatch: the row IS a copy, and a memcpy of
 * a width the compiler can see is a load and a store. The switch is
 * outside the row loop and covers the luma PU widths (4, 8, 12, 16, 24,
 * 32, 48, 64; 2 and 6 are chroma, which this plane does not serve). */
#define N265_2D_ROWS(n)                                                  \
    for (int y = 0; y < h; y++)                                          \
        memcpy(dst + (ptrdiff_t)y * ds, p2 + (ptrdiff_t)y * ps,          \
               (size_t)(n) * sizeof(pixel))

void n265_mc_luma_2d(pixel *dst, int ds, int w, int h,
                     const pixel *p2, int ps)
{
    N265_RC_TOUCH_P2(p2);
    N265_RC_TOUCH_P2(p2 + (ptrdiff_t)(h - 1) * ps);
    switch (w) {
    case 4:  N265_2D_ROWS(4);  return;
    case 8:  N265_2D_ROWS(8);  return;
    case 12: N265_2D_ROWS(12); return;
    case 16: N265_2D_ROWS(16); return;
    case 24: N265_2D_ROWS(24); return;
    case 32: N265_2D_ROWS(32); return;
    case 48: N265_2D_ROWS(48); return;
    case 64: N265_2D_ROWS(64); return;
    default: N265_2D_ROWS((size_t)w); return;
    }
}

/* Round 4: the same copy out of the int16 plane, for the bi path. Twice
 * the bytes per row, so the constant lengths are 2 * w; the reason for
 * the switch is round 3's, that a memcpy whose length the compiler
 * cannot see is a call into libc's general mover. */
#define N265_2D_ROWS14(n)                                                \
    for (int y = 0; y < h; y++)                                          \
        memcpy(dst + (ptrdiff_t)y * ds, p2 + (ptrdiff_t)y * ps, (n))

void n265_mc_luma_2d_14(int16_t *dst, int ds, int w, int h,
                        const int16_t *p2, int ps)
{
    N265_RC_TOUCH16_P2(p2);
    N265_RC_TOUCH16_P2(p2 + (ptrdiff_t)(h - 1) * ps);
    switch (w) {
    case 4:  N265_2D_ROWS14(8);   return;
    case 8:  N265_2D_ROWS14(16);  return;
    case 12: N265_2D_ROWS14(24);  return;
    case 16: N265_2D_ROWS14(32);  return;
    case 24: N265_2D_ROWS14(48);  return;
    case 32: N265_2D_ROWS14(64);  return;
    case 48: N265_2D_ROWS14(96);  return;
    case 64: N265_2D_ROWS14(128); return;
    default: N265_2D_ROWS14((size_t)w * 2); return;
    }
}

void n265_mc_luma_hf(pixel *dst, int ds, int w, int h,
                     const pixel *ref, int ref_stride,
                     const int16_t *hf, int hs, int fx, int fy)
{
    if (!fx || !hf) {
        n265_mc_luma_wh(dst, ds, w, h, ref, ref_stride, fx, fy);
        return;
    }
    N265_RC_TOUCH16(hf + (fy ? -3 : 0) * hs);
    N265_RC_TOUCH16(hf + (fy ? h + 3 : h - 1) * hs);
#if N265_NEON_MC
    if (n265_asm_on(N265_ASM_MC)) {
#if N265_SVE_MC
        if (N265_SVE_MCV_OK(n265_cpu_word(), w, h, fy)) {
            n265_mc_luma_hf_sve(dst, ds, w, h, hf, hs, fy);
            return;
        }
#endif
        n265_mc_luma_hf_neon(dst, ds, w, h, hf, hs, fy);
        return;
    }
#endif
#if N265_AVX2_MC
    if (n265_avx2_on(N265_ASM_MC)) {
        n265_mc_luma_hf_avx2(dst, ds, w, h, hf, hs, fy);
        return;
    }
#endif
#if N265_SSE42_MC
    if (n265_sse42_on(N265_ASM_MC)) {
        n265_mc_luma_hf_sse42(dst, ds, w, h, hf, hs, fy);
        return;
    }
#endif
    n265_mc_luma_hf_ref(dst, ds, w, h, hf, hs, fy);
}

void n265_mc_luma_14_hf_ref(int16_t *dst, int ds, int w, int h,
                            const int16_t *hf, int hs, int fy)
{
    if (!fy) {
        for (int y = 0; y < h; y++)
            for (int x = 0; x < w; x++)
                dst[y * ds + x] = hf[y * hs + x];
        return;
    }
    const int8_t *fv = luma_filt[fy];
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            int s = 0;
            for (int k = 0; k < 8; k++)
                s += fv[k] * hf[(y + k - 3) * hs + x];
            dst[y * ds + x] = (int16_t)(s >> 6);
        }
}

void n265_mc_luma_14_hf(int16_t *dst, int ds, int w, int h,
                        const pixel *ref, int ref_stride,
                        const int16_t *hf, int hs, int fx, int fy)
{
    if (!fx || !hf) {
        n265_mc_luma_14_wh(dst, ds, w, h, ref, ref_stride, fx, fy);
        return;
    }
    N265_RC_TOUCH16(hf + (fy ? -3 : 0) * hs);
    N265_RC_TOUCH16(hf + (fy ? h + 3 : h - 1) * hs);
#if N265_NEON_MC
    if (n265_asm_on(N265_ASM_MC)) {
#if N265_SVE_MC
        if (N265_SVE_MCV14_OK(n265_cpu_word(), w, h, fy)) {
            n265_mc_luma_14_hf_sve(dst, ds, w, h, hf, hs, fy);
            return;
        }
#endif
        n265_mc_luma_14_hf_neon(dst, ds, w, h, hf, hs, fy);
        return;
    }
#endif
#if N265_AVX2_MC
    if (n265_avx2_on(N265_ASM_MC)) {
        n265_mc_luma_14_hf_avx2(dst, ds, w, h, hf, hs, fy);
        return;
    }
#endif
#if N265_SSE42_MC
    if (n265_sse42_on(N265_ASM_MC)) {
        n265_mc_luma_14_hf_sse42(dst, ds, w, h, hf, hs, fy);
        return;
    }
#endif
    n265_mc_luma_14_hf_ref(dst, ds, w, h, hf, hs, fy);
}

void n265_mc_chroma_wh(pixel *dst, int ds, int w, int h,
                       const pixel *ref, int ref_stride, int fx, int fy)
{
    N265_RC_TOUCH(ref + (fy ? -1 : 0) * ref_stride);
    N265_RC_TOUCH(ref + (fy ? h + 1 : h - 1) * ref_stride);
#if N265_NEON_MC
    if (n265_asm_on(N265_ASM_MC)) {
        n265_mc_chroma_wh_neon(dst, ds, w, h, ref, ref_stride, fx, fy);
        return;
    }
#endif
#if N265_AVX2_MC
    if (n265_avx2_on(N265_ASM_MC)) {
        n265_mc_chroma_wh_avx2(dst, ds, w, h, ref, ref_stride, fx, fy);
        return;
    }
#endif
#if N265_SSE42_MC
    if (n265_sse42_on(N265_ASM_MC)) {
        n265_mc_chroma_wh_sse42(dst, ds, w, h, ref, ref_stride, fx, fy);
        return;
    }
#endif
    n265_mc_chroma_wh_ref(dst, ds, w, h, ref, ref_stride, fx, fy);
}

void n265_mc_chroma_wh_ref(pixel *dst, int ds, int w, int h,
                           const pixel *ref, int ref_stride, int fx, int fy)
{
    if (!fx && !fy) {
        for (int y = 0; y < h; y++)
            for (int x = 0; x < w; x++)
                dst[y * ds + x] = ref[y * ref_stride + x];
        return;
    }
    if (!fy) {
        const int8_t *f = chroma_filt[fx];
        for (int y = 0; y < h; y++)
            for (int x = 0; x < w; x++) {
                int s = 0;
                for (int k = 0; k < 4; k++)
                    s += f[k] * ref[y * ref_stride + x + k - 1];
                dst[y * ds + x] = clip1((s + 32) >> 6);
            }
        return;
    }
    if (!fx) {
        const int8_t *f = chroma_filt[fy];
        for (int y = 0; y < h; y++)
            for (int x = 0; x < w; x++) {
                int s = 0;
                for (int k = 0; k < 4; k++)
                    s += f[k] * ref[(y + k - 1) * ref_stride + x];
                dst[y * ds + x] = clip1((s + 32) >> 6);
            }
        return;
    }
    /* The widest CHROMA block this encoder presents: 32x32 when the
     * plane is halved (a 64x64 luma PU at 4:2:0) and 64x64 at 4:4:4,
     * where a chroma block IS the luma block. The 4-tap vertical stage
     * reads one row above and two below, so the temp is (h + 3) rows. */
    int16_t tmp[(64 + 3) * 64];
    const int8_t *fh = chroma_filt[fx], *fv = chroma_filt[fy];
    for (int y = -1; y < h + 2; y++)
        for (int x = 0; x < w; x++) {
            int s = 0;
            for (int k = 0; k < 4; k++)
                s += fh[k] * ref[y * ref_stride + x + k - 1];
            tmp[(y + 1) * w + x] = (int16_t)(s >> MC_SHIFT1);
        }
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            int s = 0;
            for (int k = 0; k < 4; k++)
                s += fv[k] * tmp[(y + k) * w + x];
            dst[y * ds + x] = clip1((s + MC_ROUND23) >> MC_SHIFT23);
        }
}

/* 14-bit outputs: 1-D keeps sum<<0 (values are sum of coef*sample, i.e.
 * sample<<6 scale); full-pel is sample<<6; 2-D second stage >>6. */
void n265_mc_luma_14_wh(int16_t *dst, int ds, int w, int h,
                        const pixel *ref, int ref_stride, int fx, int fy)
{
    N265_RC_TOUCH(ref + (fy ? -3 : 0) * ref_stride);
    N265_RC_TOUCH(ref + (fy ? h + 3 : h - 1) * ref_stride);
#if N265_NEON_MC
    if (n265_asm_on(N265_ASM_MC)) {
        n265_mc_luma_14_wh_neon(dst, ds, w, h, ref, ref_stride, fx, fy);
        return;
    }
#endif
#if N265_AVX2_MC
    if (n265_avx2_on(N265_ASM_MC)) {
        n265_mc_luma_14_wh_avx2(dst, ds, w, h, ref, ref_stride, fx, fy);
        return;
    }
#endif
#if N265_SSE42_MC
    if (n265_sse42_on(N265_ASM_MC)) {
        n265_mc_luma_14_wh_sse42(dst, ds, w, h, ref, ref_stride, fx, fy);
        return;
    }
#endif
    n265_mc_luma_14_wh_ref(dst, ds, w, h, ref, ref_stride, fx, fy);
}

void n265_mc_luma_14_wh_ref(int16_t *dst, int ds, int w, int h,
                            const pixel *ref, int ref_stride, int fx, int fy)
{
    if (!fx && !fy) {
        for (int y = 0; y < h; y++)
            for (int x = 0; x < w; x++)
                dst[y * ds + x] =
                    (int16_t)(ref[y * ref_stride + x] << MC_SHIFT3);
        return;
    }
    if (!fy) {
        const int8_t *f = luma_filt[fx];
        for (int y = 0; y < h; y++)
            for (int x = 0; x < w; x++) {
                int s = 0;
                for (int k = 0; k < 8; k++)
                    s += f[k] * ref[y * ref_stride + x + k - 3];
                dst[y * ds + x] = (int16_t)(s >> MC_SHIFT1);
            }
        return;
    }
    if (!fx) {
        const int8_t *f = luma_filt[fy];
        for (int y = 0; y < h; y++)
            for (int x = 0; x < w; x++) {
                int s = 0;
                for (int k = 0; k < 8; k++)
                    s += f[k] * ref[(y + k - 3) * ref_stride + x];
                dst[y * ds + x] = (int16_t)(s >> MC_SHIFT1);
            }
        return;
    }
    int16_t tmp[(64 + 7) * 64];
    const int8_t *fh = luma_filt[fx], *fv = luma_filt[fy];
    for (int y = -3; y < h + 4; y++)
        for (int x = 0; x < w; x++) {
            int s = 0;
            for (int k = 0; k < 8; k++)
                s += fh[k] * ref[y * ref_stride + x + k - 3];
            tmp[(y + 3) * w + x] = (int16_t)(s >> MC_SHIFT1);
        }
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            int s = 0;
            for (int k = 0; k < 8; k++)
                s += fv[k] * tmp[(y + k) * w + x];
            dst[y * ds + x] = (int16_t)(s >> 6);
        }
}

void n265_mc_chroma_14_wh(int16_t *dst, int ds, int w, int h,
                          const pixel *ref, int ref_stride, int fx, int fy)
{
    N265_RC_TOUCH(ref + (fy ? -1 : 0) * ref_stride);
    N265_RC_TOUCH(ref + (fy ? h + 1 : h - 1) * ref_stride);
#if N265_NEON_MC
    if (n265_asm_on(N265_ASM_MC)) {
        n265_mc_chroma_14_wh_neon(dst, ds, w, h, ref, ref_stride, fx, fy);
        return;
    }
#endif
#if N265_AVX2_MC
    if (n265_avx2_on(N265_ASM_MC)) {
        n265_mc_chroma_14_wh_avx2(dst, ds, w, h, ref, ref_stride, fx, fy);
        return;
    }
#endif
#if N265_SSE42_MC
    if (n265_sse42_on(N265_ASM_MC)) {
        n265_mc_chroma_14_wh_sse42(dst, ds, w, h, ref, ref_stride, fx, fy);
        return;
    }
#endif
    n265_mc_chroma_14_wh_ref(dst, ds, w, h, ref, ref_stride, fx, fy);
}

void n265_mc_chroma_14_wh_ref(int16_t *dst, int ds, int w, int h,
                              const pixel *ref, int ref_stride, int fx,
                              int fy)
{
    if (!fx && !fy) {
        for (int y = 0; y < h; y++)
            for (int x = 0; x < w; x++)
                dst[y * ds + x] =
                    (int16_t)(ref[y * ref_stride + x] << MC_SHIFT3);
        return;
    }
    if (!fy) {
        const int8_t *f = chroma_filt[fx];
        for (int y = 0; y < h; y++)
            for (int x = 0; x < w; x++) {
                int s = 0;
                for (int k = 0; k < 4; k++)
                    s += f[k] * ref[y * ref_stride + x + k - 1];
                dst[y * ds + x] = (int16_t)(s >> MC_SHIFT1);
            }
        return;
    }
    if (!fx) {
        const int8_t *f = chroma_filt[fy];
        for (int y = 0; y < h; y++)
            for (int x = 0; x < w; x++) {
                int s = 0;
                for (int k = 0; k < 4; k++)
                    s += f[k] * ref[(y + k - 1) * ref_stride + x];
                dst[y * ds + x] = (int16_t)(s >> MC_SHIFT1);
            }
        return;
    }
    int16_t tmp[(64 + 3) * 64];     /* the widest chroma block; see above */
    const int8_t *fh = chroma_filt[fx], *fv = chroma_filt[fy];
    for (int y = -1; y < h + 2; y++)
        for (int x = 0; x < w; x++) {
            int s = 0;
            for (int k = 0; k < 4; k++)
                s += fh[k] * ref[y * ref_stride + x + k - 1];
            tmp[(y + 1) * w + x] = (int16_t)(s >> MC_SHIFT1);
        }
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            int s = 0;
            for (int k = 0; k < 4; k++)
                s += fv[k] * tmp[(y + k) * w + x];
            dst[y * ds + x] = (int16_t)(s >> 6);
        }
}

void n265_mc_luma(pixel *dst, int n, const pixel *ref, int ref_stride,
                  int fx, int fy)
{
    n265_mc_luma_wh(dst, n, n, n, ref, ref_stride, fx, fy);
}

void n265_mc_chroma(pixel *dst, int n, const pixel *ref, int ref_stride,
                    int fx, int fy)
{
    n265_mc_chroma_wh(dst, n, n, n, ref, ref_stride, fx, fy);
}

void n265_mc_luma_14(int16_t *dst, int n, const pixel *ref, int ref_stride,
                     int fx, int fy)
{
    n265_mc_luma_14_wh(dst, n, n, n, ref, ref_stride, fx, fy);
}

void n265_mc_chroma_14(int16_t *dst, int n, const pixel *ref,
                       int ref_stride, int fx, int fy)
{
    n265_mc_chroma_14_wh(dst, n, n, n, ref, ref_stride, fx, fy);
}

/* the C reference the NEON kernel is checked against, kept callable so
 * checkasm can compare `_ref` with `_neon` directly rather than through
 * the dispatching entry point */
void n265_bi_avg_wh_ref(pixel *dst, int ds, const int16_t *a,
                        const int16_t *b, int w, int h)
{
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            /* 8.5.3.3.4.2 default bi: shift = 15 - BitDepth, 7 at 8 */
            int v = (a[y * w + x] + b[y * w + x] + (1 << (14 - BIT_DEPTH)))
                    >> (15 - BIT_DEPTH);
            dst[y * ds + x] = clip1(v);
        }
}

void n265_bi_avg_wh(pixel *dst, int ds, const int16_t *a, const int16_t *b,
                    int w, int h)
{
#if N265_NEON_MC
    /* Dispatch at the top of the entry point, after any N265_RC_TOUCH -
     * there are none here, a and b being the caller's own int16
     * intermediates rather than a guarded reference plane. See
     * src/encoder/arm/neon.h for the convention.
     *
     * The shape test joins the class test because the kernel has no
     * vector path below four samples wide: the 2-wide chroma blocks run
     * its scalar tail and pay an out-of-line call for it, measured at
     * 0.75x on 2x8. Both paths are bit-exact, so this moves time only. */
    if (w >= 4 && n265_asm_on(N265_ASM_MC)) {
        n265_bi_avg_wh_neon(dst, ds, a, b, w, h);
        return;
    }
#endif
#if N265_AVX2_MC
    if (w >= 4 && n265_avx2_on(N265_ASM_MC)) {
        n265_bi_avg_wh_avx2(dst, ds, a, b, w, h);
        return;
    }
#endif
#if N265_SSE42_MC
    if (w >= 4 && n265_sse42_on(N265_ASM_MC)) {
        n265_bi_avg_wh_sse42(dst, ds, a, b, w, h);
        return;
    }
#endif
    n265_bi_avg_wh_ref(dst, ds, a, b, w, h);
}

void n265_bi_avg(pixel *dst, const int16_t *a, const int16_t *b, int n)
{
    n265_bi_avg_wh(dst, n, a, b, n, n);
}

/* ---- 8.5.3.3.4.3 ---------------------------------------------------
 *
 * log2Wd = denom + shift1, and o = offset << WpOffsetBdShift with
 * sps_high_precision_offsets_enabled_flag 0 - which is what this encoder
 * writes, so the offset is in 8-bit levels at every depth and the shift
 * brings it to the sample's.
 *
 * The clause spells the uni form twice, once per list, and the two
 * differ only in which weight is read; one function serves both, the
 * caller having already chosen the list. The `log2Wd < 1` arm is
 * unreachable at 8 and 10 bits - shift1 is 6 and 4 and denom is never
 * negative - and is written anyway, because leaving it out would make
 * the code read as if the condition were not in the spec. */
void n265_wp_uni_wh_ref(pixel *dst, int ds, const int16_t *src, int w, int h,
                        int wt, int off, int denom)
{
    int log2Wd = denom + MC_SHIFT3;
    int o = off * (1 << BIT_DEPTH_SHIFT);
    if (log2Wd >= 1) {
        int rnd = 1 << (log2Wd - 1);
        for (int y = 0; y < h; y++)
            for (int x = 0; x < w; x++)
                dst[y * ds + x] =
                    clip1(((src[y * w + x] * wt + rnd) >> log2Wd) + o);
    } else {
        for (int y = 0; y < h; y++)
            for (int x = 0; x < w; x++)
                dst[y * ds + x] = clip1(src[y * w + x] * wt + o);
    }
}

void n265_wp_uni_wh(pixel *dst, int ds, const int16_t *src, int w, int h,
                    int wt, int off, int denom)
{
#if N265_NEON_MC
    /* the shape test joins the class test for n265_bi_avg_wh's reason:
     * below four samples wide the kernel is its own scalar tail and pays
     * an out-of-line call for it */
    if (w >= 4 && n265_asm_on(N265_ASM_MC)) {
        n265_wp_uni_wh_neon(dst, ds, src, w, h, wt, off, denom);
        return;
    }
#endif
#if N265_AVX2_MC
    if (w >= 4 && n265_avx2_on(N265_ASM_MC)) {
        n265_wp_uni_wh_avx2(dst, ds, src, w, h, wt, off, denom);
        return;
    }
#endif
#if N265_SSE42_MC
    if (w >= 4 && n265_sse42_on(N265_ASM_MC)) {
        n265_wp_uni_wh_sse42(dst, ds, src, w, h, wt, off, denom);
        return;
    }
#endif
    n265_wp_uni_wh_ref(dst, ds, src, w, h, wt, off, denom);
}

void n265_wp_bi_wh_ref(pixel *dst, int ds, const int16_t *a, const int16_t *b,
                       int w, int h, int w0, int o0, int w1, int o1,
                       int denom)
{
    int log2Wd = denom + MC_SHIFT3;
    /* ((o0 + o1 + 1) << log2Wd) of the clause, with the depth scale on
     * the OFFSETS and not on the 1: the +1 is the rounding term of the
     * >> (log2Wd + 1) that follows, not part of either offset. */
    /* the shift is written as a MULTIPLY: the bracket is negative
     * whenever the two offsets sum below -1, and shifting a negative
     * left is undefined (UBSan catches it in checkasm-asan). */
    int add = ((o0 + o1) * (1 << BIT_DEPTH_SHIFT) + 1) * (1 << log2Wd);
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            int v = a[y * w + x] * w0 + b[y * w + x] * w1 + add;
            dst[y * ds + x] = clip1(v >> (log2Wd + 1));
        }
}

void n265_wp_bi_wh(pixel *dst, int ds, const int16_t *a, const int16_t *b,
                   int w, int h, int w0, int o0, int w1, int o1, int denom)
{
#if N265_NEON_MC
    if (w >= 4 && n265_asm_on(N265_ASM_MC)) {
        n265_wp_bi_wh_neon(dst, ds, a, b, w, h, w0, o0, w1, o1, denom);
        return;
    }
#endif
#if N265_AVX2_MC
    if (w >= 4 && n265_avx2_on(N265_ASM_MC)) {
        n265_wp_bi_wh_avx2(dst, ds, a, b, w, h, w0, o0, w1, o1, denom);
        return;
    }
#endif
#if N265_SSE42_MC
    if (w >= 4 && n265_sse42_on(N265_ASM_MC)) {
        n265_wp_bi_wh_sse42(dst, ds, a, b, w, h, w0, o0, w1, o1, denom);
        return;
    }
#endif
    n265_wp_bi_wh_ref(dst, ds, a, b, w, h, w0, o0, w1, o1, denom);
}

/* The search's pixel-domain map; mc.h says why a cost function is
 * allowed this and a reconstruction is not. */
void n265_wp_pix_wh(pixel *dst, int ds, const pixel *src, int ss, int w,
                    int h, int wt, int off, int denom)
{
#if N265_NEON_MC
    if (w >= 4 && n265_asm_on(N265_ASM_MC)) {
        n265_wp_pix_wh_neon(dst, ds, src, ss, w, h, wt, off, denom);
        return;
    }
#endif
#if N265_AVX2_MC
    if (w >= 4 && n265_avx2_on(N265_ASM_MC)) {
        n265_wp_pix_wh_avx2(dst, ds, src, ss, w, h, wt, off, denom);
        return;
    }
#endif
#if N265_SSE42_MC
    if (w >= 4 && n265_sse42_on(N265_ASM_MC)) {
        n265_wp_pix_wh_sse42(dst, ds, src, ss, w, h, wt, off, denom);
        return;
    }
#endif
    n265_wp_pix_wh_ref(dst, ds, src, ss, w, h, wt, off, denom);
}

void n265_wp_pix_wh_ref(pixel *dst, int ds, const pixel *src, int ss, int w,
                        int h, int wt, int off, int denom)
{
    int o = off * (1 << BIT_DEPTH_SHIFT);
    if (denom >= 1) {
        int rnd = 1 << (denom - 1);
        for (int y = 0; y < h; y++)
            for (int x = 0; x < w; x++)
                dst[y * ds + x] =
                    clip1(((src[y * ss + x] * wt + rnd) >> denom) + o);
    } else {
        for (int y = 0; y < h; y++)
            for (int x = 0; x < w; x++)
                dst[y * ds + x] = clip1(src[y * ss + x] * wt + o);
    }
}
