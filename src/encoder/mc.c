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
 * neighbour and is NOT clamped - the spec does not clamp it and a
 * decoder does not have to - so a candidate can reach past the border,
 * and the bytes after it are the next row's left border rather than the
 * edge sample.
 *
 * Returns the block origin a filter may read `hl` samples left and above
 * and `hr` right and below of: the plane itself whenever that whole
 * window is inside the border (the common case, which copies nothing),
 * otherwise `tmp`, gathered with the spec's clipping. `tmp` must hold
 * (w + hl + hr) * (h + hl + hr) bytes.
 *
 * Under N265_REF_CHECK the gather reports the extreme lines it really
 * reads - which are the clipped ones, never past the guard - and then
 * opens the guard for the kernel, whose reads land in tmp. */
const uint8_t *n265_ref_win(const uint8_t *plane, int stride, int pic_w,
                            int pic_h, int pad, int ix, int iy, int w,
                            int h, int hl, int hr, uint8_t *tmp,
                            int *out_stride, int *clipped)
{
    if (ix - hl >= -pad && ix + w + hr <= pic_w + pad &&
        iy - hl >= -pad && iy + h + hr <= pic_h + pad) {
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
        const uint8_t *sp = plane + (ptrdiff_t)sy * stride;
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

static inline uint8_t clip1(int v)
{
    return (uint8_t)(v < 0 ? 0 : v > 255 ? 255 : v);
}

void n265_mc_luma_wh(uint8_t *dst, int ds, int w, int h,
                     const uint8_t *ref, int ref_stride, int fx, int fy)
{
    /* the extreme lines this call reads, reported from inside the filter
     * rather than from the caller's bound (refchk.h) */
    N265_RC_TOUCH(ref + (fy ? -3 : 0) * ref_stride);
    N265_RC_TOUCH(ref + (fy ? h + 3 : h - 1) * ref_stride);
#if N265_NEON
    if (n265_asm_on(N265_ASM_MC)) {
        n265_mc_luma_wh_neon(dst, ds, w, h, ref, ref_stride, fx, fy);
        return;
    }
#endif
    n265_mc_luma_wh_ref(dst, ds, w, h, ref, ref_stride, fx, fy);
}

void n265_mc_luma_wh_ref(uint8_t *dst, int ds, int w, int h,
                         const uint8_t *ref, int ref_stride, int fx, int fy)
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
            tmp[(y + 3) * w + x] = (int16_t)s;
        }
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            int s = 0;
            for (int k = 0; k < 8; k++)
                s += fv[k] * tmp[(y + k) * w + x];
            dst[y * ds + x] = clip1((s + 2048) >> 12);
        }
}

void n265_mc_luma_hplane(int16_t *dst, int ds, int w, int h,
                         const uint8_t *ref, int ref_stride, int fx)
{
    N265_RC_TOUCH(ref);
    N265_RC_TOUCH(ref + (h - 1) * ref_stride);
#if N265_NEON
    if (n265_asm_on(N265_ASM_MC)) {
        n265_mc_luma_hplane_neon(dst, ds, w, h, ref, ref_stride, fx);
        return;
    }
#endif
    n265_mc_luma_hplane_ref(dst, ds, w, h, ref, ref_stride, fx);
}

void n265_mc_luma_hplane_ref(int16_t *dst, int ds, int w, int h,
                             const uint8_t *ref, int ref_stride, int fx)
{
    const int8_t *f = luma_filt[fx];
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            int s = 0;
            for (int k = 0; k < 8; k++)
                s += f[k] * ref[y * ref_stride + x + k - 3];
            dst[y * ds + x] = (int16_t)s;
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
void n265_mc_luma_hf_ref(uint8_t *dst, int ds, int w, int h,
                         const int16_t *hf, int hs, int fy)
{
    if (!fy) {
        for (int y = 0; y < h; y++)
            for (int x = 0; x < w; x++)
                dst[y * ds + x] = clip1((hf[y * hs + x] + 32) >> 6);
        return;
    }
    const int8_t *fv = luma_filt[fy];
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            int s = 0;
            for (int k = 0; k < 8; k++)
                s += fv[k] * hf[(y + k - 3) * hs + x];
            dst[y * ds + x] = clip1((s + 2048) >> 12);
        }
}

/* Phase 6 round 3. No NEON dispatch: the row IS a copy, and a memcpy of
 * a width the compiler can see is a load and a store. The switch is
 * outside the row loop and covers the luma PU widths (4, 8, 12, 16, 24,
 * 32, 48, 64; 2 and 6 are chroma, which this plane does not serve). */
#define N265_2D_ROWS(n)                                                  \
    for (int y = 0; y < h; y++)                                          \
        memcpy(dst + (ptrdiff_t)y * ds, p2 + (ptrdiff_t)y * ps, (n))

void n265_mc_luma_2d(uint8_t *dst, int ds, int w, int h,
                     const uint8_t *p2, int ps)
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

void n265_mc_luma_hf(uint8_t *dst, int ds, int w, int h,
                     const uint8_t *ref, int ref_stride,
                     const int16_t *hf, int hs, int fx, int fy)
{
    if (!fx || !hf) {
        n265_mc_luma_wh(dst, ds, w, h, ref, ref_stride, fx, fy);
        return;
    }
    N265_RC_TOUCH16(hf + (fy ? -3 : 0) * hs);
    N265_RC_TOUCH16(hf + (fy ? h + 3 : h - 1) * hs);
#if N265_NEON
    if (n265_asm_on(N265_ASM_MC)) {
        n265_mc_luma_hf_neon(dst, ds, w, h, hf, hs, fy);
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
                        const uint8_t *ref, int ref_stride,
                        const int16_t *hf, int hs, int fx, int fy)
{
    if (!fx || !hf) {
        n265_mc_luma_14_wh(dst, ds, w, h, ref, ref_stride, fx, fy);
        return;
    }
    N265_RC_TOUCH16(hf + (fy ? -3 : 0) * hs);
    N265_RC_TOUCH16(hf + (fy ? h + 3 : h - 1) * hs);
#if N265_NEON
    if (n265_asm_on(N265_ASM_MC)) {
        n265_mc_luma_14_hf_neon(dst, ds, w, h, hf, hs, fy);
        return;
    }
#endif
    n265_mc_luma_14_hf_ref(dst, ds, w, h, hf, hs, fy);
}

void n265_mc_chroma_wh(uint8_t *dst, int ds, int w, int h,
                       const uint8_t *ref, int ref_stride, int fx, int fy)
{
    N265_RC_TOUCH(ref + (fy ? -1 : 0) * ref_stride);
    N265_RC_TOUCH(ref + (fy ? h + 1 : h - 1) * ref_stride);
#if N265_NEON
    if (n265_asm_on(N265_ASM_MC)) {
        n265_mc_chroma_wh_neon(dst, ds, w, h, ref, ref_stride, fx, fy);
        return;
    }
#endif
    n265_mc_chroma_wh_ref(dst, ds, w, h, ref, ref_stride, fx, fy);
}

void n265_mc_chroma_wh_ref(uint8_t *dst, int ds, int w, int h,
                           const uint8_t *ref, int ref_stride, int fx, int fy)
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
    int16_t tmp[(32 + 3) * 32];
    const int8_t *fh = chroma_filt[fx], *fv = chroma_filt[fy];
    for (int y = -1; y < h + 2; y++)
        for (int x = 0; x < w; x++) {
            int s = 0;
            for (int k = 0; k < 4; k++)
                s += fh[k] * ref[y * ref_stride + x + k - 1];
            tmp[(y + 1) * w + x] = (int16_t)s;
        }
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            int s = 0;
            for (int k = 0; k < 4; k++)
                s += fv[k] * tmp[(y + k) * w + x];
            dst[y * ds + x] = clip1((s + 2048) >> 12);
        }
}

/* 14-bit outputs: 1-D keeps sum<<0 (values are sum of coef*sample, i.e.
 * sample<<6 scale); full-pel is sample<<6; 2-D second stage >>6. */
void n265_mc_luma_14_wh(int16_t *dst, int ds, int w, int h,
                        const uint8_t *ref, int ref_stride, int fx, int fy)
{
    N265_RC_TOUCH(ref + (fy ? -3 : 0) * ref_stride);
    N265_RC_TOUCH(ref + (fy ? h + 3 : h - 1) * ref_stride);
#if N265_NEON
    if (n265_asm_on(N265_ASM_MC)) {
        n265_mc_luma_14_wh_neon(dst, ds, w, h, ref, ref_stride, fx, fy);
        return;
    }
#endif
    n265_mc_luma_14_wh_ref(dst, ds, w, h, ref, ref_stride, fx, fy);
}

void n265_mc_luma_14_wh_ref(int16_t *dst, int ds, int w, int h,
                            const uint8_t *ref, int ref_stride, int fx, int fy)
{
    if (!fx && !fy) {
        for (int y = 0; y < h; y++)
            for (int x = 0; x < w; x++)
                dst[y * ds + x] = (int16_t)(ref[y * ref_stride + x] << 6);
        return;
    }
    if (!fy) {
        const int8_t *f = luma_filt[fx];
        for (int y = 0; y < h; y++)
            for (int x = 0; x < w; x++) {
                int s = 0;
                for (int k = 0; k < 8; k++)
                    s += f[k] * ref[y * ref_stride + x + k - 3];
                dst[y * ds + x] = (int16_t)s;
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
                dst[y * ds + x] = (int16_t)s;
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
            tmp[(y + 3) * w + x] = (int16_t)s;
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
                          const uint8_t *ref, int ref_stride, int fx, int fy)
{
    N265_RC_TOUCH(ref + (fy ? -1 : 0) * ref_stride);
    N265_RC_TOUCH(ref + (fy ? h + 1 : h - 1) * ref_stride);
#if N265_NEON
    if (n265_asm_on(N265_ASM_MC)) {
        n265_mc_chroma_14_wh_neon(dst, ds, w, h, ref, ref_stride, fx, fy);
        return;
    }
#endif
    n265_mc_chroma_14_wh_ref(dst, ds, w, h, ref, ref_stride, fx, fy);
}

void n265_mc_chroma_14_wh_ref(int16_t *dst, int ds, int w, int h,
                              const uint8_t *ref, int ref_stride, int fx,
                              int fy)
{
    if (!fx && !fy) {
        for (int y = 0; y < h; y++)
            for (int x = 0; x < w; x++)
                dst[y * ds + x] = (int16_t)(ref[y * ref_stride + x] << 6);
        return;
    }
    if (!fy) {
        const int8_t *f = chroma_filt[fx];
        for (int y = 0; y < h; y++)
            for (int x = 0; x < w; x++) {
                int s = 0;
                for (int k = 0; k < 4; k++)
                    s += f[k] * ref[y * ref_stride + x + k - 1];
                dst[y * ds + x] = (int16_t)s;
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
                dst[y * ds + x] = (int16_t)s;
            }
        return;
    }
    int16_t tmp[(32 + 3) * 32];
    const int8_t *fh = chroma_filt[fx], *fv = chroma_filt[fy];
    for (int y = -1; y < h + 2; y++)
        for (int x = 0; x < w; x++) {
            int s = 0;
            for (int k = 0; k < 4; k++)
                s += fh[k] * ref[y * ref_stride + x + k - 1];
            tmp[(y + 1) * w + x] = (int16_t)s;
        }
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            int s = 0;
            for (int k = 0; k < 4; k++)
                s += fv[k] * tmp[(y + k) * w + x];
            dst[y * ds + x] = (int16_t)(s >> 6);
        }
}

void n265_mc_luma(uint8_t *dst, int n, const uint8_t *ref, int ref_stride,
                  int fx, int fy)
{
    n265_mc_luma_wh(dst, n, n, n, ref, ref_stride, fx, fy);
}

void n265_mc_chroma(uint8_t *dst, int n, const uint8_t *ref, int ref_stride,
                    int fx, int fy)
{
    n265_mc_chroma_wh(dst, n, n, n, ref, ref_stride, fx, fy);
}

void n265_mc_luma_14(int16_t *dst, int n, const uint8_t *ref, int ref_stride,
                     int fx, int fy)
{
    n265_mc_luma_14_wh(dst, n, n, n, ref, ref_stride, fx, fy);
}

void n265_mc_chroma_14(int16_t *dst, int n, const uint8_t *ref,
                       int ref_stride, int fx, int fy)
{
    n265_mc_chroma_14_wh(dst, n, n, n, ref, ref_stride, fx, fy);
}

/* the C reference the NEON kernel is checked against, kept callable so
 * checkasm can compare `_ref` with `_neon` directly rather than through
 * the dispatching entry point */
void n265_bi_avg_wh_ref(uint8_t *dst, int ds, const int16_t *a,
                        const int16_t *b, int w, int h)
{
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            int v = (a[y * w + x] + b[y * w + x] + 64) >> 7;
            dst[y * ds + x] = (uint8_t)(v < 0 ? 0 : v > 255 ? 255 : v);
        }
}

void n265_bi_avg_wh(uint8_t *dst, int ds, const int16_t *a, const int16_t *b,
                    int w, int h)
{
#if N265_NEON
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
    n265_bi_avg_wh_ref(dst, ds, a, b, w, h);
}

void n265_bi_avg(uint8_t *dst, const int16_t *a, const int16_t *b, int n)
{
    n265_bi_avg_wh(dst, n, a, b, n, n);
}
