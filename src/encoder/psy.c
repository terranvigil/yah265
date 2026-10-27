/*
 * Psychovisual distortion: the energy a reconstruction loses.
 *
 * SSD alone prefers a blurred reconstruction (it costs fewer bits and less
 * squared error than one that keeps texture), which viewers read as
 * softness. The term here charges a candidate for the AC energy its
 * reconstruction sheds against the source: the sum over 4x4 blocks of the
 * absolute Hadamard coefficients with the DC removed, compared source
 * against reconstruction, |E_src - E_rec|. It is added to the candidate's
 * cost scaled by the mode lambda and a strength (N265_PSY), so it competes
 * in the same units as the rate term. yah264 shipped the same idea at a
 * flat strength gated by the band; the metric that decides is VMAF-NEG,
 * which penalises sharpening, so a win here is texture retained and not
 * noise added.
 *
 * Copyright (c) 2026, the yah265 authors. GPL-2.0-or-later; see LICENSE.
 */
#include <stdlib.h>

#include "common/cpu.h"
#include "encoder/psy.h"
#include "encoder/arm/neon.h"   /* self-guarded */

static uint32_t hadamard4_ac(const pixel *p, int stride)
{
    int m[16], t[16];
    for (int y = 0; y < 4; y++) {
        int a = p[y * stride + 0], b = p[y * stride + 1], c = p[y * stride + 2], d = p[y * stride + 3];
        int s0 = a + d, s1 = b + c, d0 = a - d, d1 = b - c;
        m[y * 4 + 0] = s0 + s1; m[y * 4 + 1] = d0 + d1; m[y * 4 + 2] = s0 - s1; m[y * 4 + 3] = d0 - d1;
    }
    for (int x = 0; x < 4; x++) {
        int a = m[0 + x], b = m[4 + x], c = m[8 + x], d = m[12 + x];
        int s0 = a + d, s1 = b + c, d0 = a - d, d1 = b - c;
        t[0 + x] = s0 + s1; t[4 + x] = d0 + d1; t[8 + x] = s0 - s1; t[12 + x] = d0 - d1;
    }
    uint32_t sum = 0;
    for (int i = 1; i < 16; i++)
        sum += (uint32_t)abs(t[i]);
    return sum;
}

uint32_t n265_psy_energy_c(const pixel *p, int stride, int n)
{
    uint32_t e = 0;
    for (int y = 0; y < n; y += 4)
        for (int x = 0; x < n; x += 4)
            e += hadamard4_ac(p + y * stride + x, stride);
    return e;
}

uint32_t n265_psy_energy(const pixel *p, int stride, int n)
{
#if N265_NEON_SATD
    if (n265_asm_on(N265_ASM_SATD))
        return n265_psy_energy_neon(p, stride, n);
#endif
    return n265_psy_energy_c(p, stride, n);
}

uint64_t n265_psy_cost(const pixel *src, int src_stride, const pixel *rec,
                       int rec_stride, int n, uint32_t lam_q8, double strength)
{
    if (strength <= 0.0)
        return 0;
    return n265_psy_scale(n265_psy_energy(src, src_stride, n),
                          n265_psy_energy(rec, rec_stride, n), lam_q8,
                          strength);
}

uint64_t n265_psy_scale(uint32_t es, uint32_t er, uint32_t lam_q8,
                        double strength)
{
    uint32_t d = es > er ? es - er : er - es;
    /* the Hadamard AC sum is ~ pixel-difference units x 2 per coefficient;
     * scaled by lambda (Q8 -> real) like a rate term, at the strength.
     *
     * The term joins an SSD sum, which grows by 1 << (2 * BIT_DEPTH_SHIFT)
     * at Main10. `d` is a MAGNITUDE, so it carries one of those shifts,
     * and lam_q8 - being a squared-domain lambda - carries both, so the
     * product arrives one shift too strong. Taking it back out is what
     * keeps a given N265_PSY the same perceptual weight at both depths;
     * it folds away at 8 bits. */
    return (uint64_t)((double)d * (double)lam_q8 / 256.0 * strength / 8.0)
           >> BIT_DEPTH_SHIFT;
}
