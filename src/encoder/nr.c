/*
 * Coefficient-domain noise reduction. nr.h says what it is; this file is
 * the estimator and the subtraction.
 *
 * Copyright (c) 2026, the yah265 authors. GPL-2.0-or-later; see LICENSE.
 */

#include <string.h>

#include "encoder/nr.h"

void n265_nr_fold(n265_nr *nr, const n265_nr_tally *t)
{
    for (int i = 0; i < N265_NR_SLOTS; i++)
        nr->sum[i] += t->sum[i];
    for (int c = 0; c < N265_NR_CATS; c++)
        nr->cnt[c] += t->cnt[c];
    /* Halve a category that has seen enough blocks to stop being about
     * this content. Both halves move together so the RATIO the offset is
     * built from is unchanged at the moment of the halving - the estimate
     * does not jump, it only starts forgetting. Driver-serial and keyed
     * on an exact integer count, so it fires on the same block on one
     * worker and on twelve. */
    for (int c = 0; c < N265_NR_CATS; c++) {
        if (nr->cnt[c] < N265_NR_HALVE)
            continue;
        int g = c >> 2, sz = c & 3;
        static const int soff[4] = { 0, 16, 80, 336 };
        int base = g * N265_NR_GROUP + soff[sz];
        int n = 16 << (2 * sz);
        for (int i = 0; i < n; i++)
            nr->sum[base + i] >>= 1;
        nr->cnt[c] >>= 1;
    }
}

void n265_nr_offsets(n265_nr *nr, int intra, int inter)
{
    static const int soff[4] = { 0, 16, 80, 336 };
    for (int c = 0; c < N265_NR_CATS; c++) {
        int g = c >> 2, sz = c & 3;
        int base = g * N265_NR_GROUP + soff[sz];
        int n = 16 << (2 * sz);
        /* groups 0 and 1 are the intra pair (luma, chroma); 2 and 3 the
         * inter pair. n265_nr_cat() builds the index. */
        int str = g < 2 ? intra : inter;
        uint64_t cnt = nr->cnt[c];
        if (str <= 0 || cnt == 0) {
            memset(nr->off + base, 0, (size_t)n * sizeof(uint16_t));
            continue;
        }
        for (int i = 0; i < n; i++) {
            uint64_t s = nr->sum[base + i];
            /* The whole model. `m = s / cnt` is this position's mean
             * coefficient magnitude over the frames already folded, and
             * the offset is `strength / (m + 1)` - carried as
             * `strength * cnt / (s + cnt)` so nothing divides twice and
             * nothing floats. A position that is usually loud gets
             * almost nothing taken off it; a position that is usually
             * near zero gets the strength itself.
             *
             * The +1 in the denominator is not a guard against division
             * by zero (s and cnt are integers and cnt > 0 here); it is
             * what makes a position with NO energy saturate at `strength`
             * instead of at infinity. */
            uint64_t o = ((uint64_t)str * cnt + (s + cnt) / 2) / (s + cnt);
            /* ...and the one bound: never take off more than the position
             * carries on average. Without it a strength of 2000 on a
             * position whose mean is a third of a level asks for an
             * offset of six thousand, which is not "attenuate the noise"
             * but "delete the band", and the difference between the two
             * would stop being a strength and start being a cliff. Where
             * the bound binds, the offset IS the mean, which zeroes a
             * typical coefficient there - that is the strongest thing
             * this filter is allowed to mean.
             *
             * It costs nothing this corpus can show. park_joy_720p, 24
             * frames, --qp 30, both strengths at 300 and at 1000: with
             * the bound 674988 and 669718 bytes, without it 674349 and
             * 668391 - a tenth of a percent of an effect that is itself
             * about one percent. What it buys is the shot change, where
             * a position's mean has not caught up with the content and
             * the reciprocal is briefly enormous. */
            uint64_t m = s / cnt;
            if (o > m)
                o = m;
            if (o > 32767)
                o = 32767;
            nr->off[base + i] = (uint16_t)o;
        }
    }
}

void n265_nr_block(const uint16_t *off, n265_nr_tally *t, int16_t *out,
                   const int16_t *coef, int log2n, int intra, int chroma)
{
    int base = n265_nr_base(intra, chroma, log2n);
    int cat = n265_nr_cat(intra, chroma, log2n);
    int n2 = 1 << (2 * log2n);
    const uint16_t *o = off + base;
    uint64_t *s = t->sum + base;

    for (int i = 0; i < n2; i++) {
        int v = coef[i];
        int a = v < 0 ? -v : v;
        s[i] += (uint64_t)(unsigned)a;
        a -= o[i];
        if (a < 0)
            a = 0;
        out[i] = (int16_t)(v < 0 ? -a : a);
    }
    t->cnt[cat]++;
}
