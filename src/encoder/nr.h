/*
 * Coefficient-domain noise reduction: --nr-intra / --nr-inter.
 *
 * A per-coefficient-POSITION offset, learned from the running magnitude
 * statistics of the coefficients this encode has already produced, and
 * subtracted toward zero before the block is quantized. It is the x264 /
 * x265 "noise reduction" knob and it is a pre-filter in the TRANSFORM
 * domain, not in the source domain - which is what separates it from
 * --film-grain-denoise (grain.h), the source-domain half.
 *
 * The rule is one line: a position whose coefficients are usually SMALL
 * is a position where the signal is noise, and it is attenuated; a
 * position whose coefficients are usually LARGE carries structure and is
 * left nearly alone. Both fall out of the same reciprocal, so there is
 * one constant (the strength) and no classification anywhere.
 *
 * Copyright (c) 2026, the yah265 authors. GPL-2.0-or-later; see LICENSE.
 */

#ifndef N265_NR_H
#define N265_NR_H

#include <stdint.h>

/* The statistics are kept per CATEGORY, and a category is what x265's two
 * flags already imply plus the two splits an offset cannot be shared
 * across: intra and inter (the two flags), luma and chroma (different
 * quantizers and wildly different magnitudes), and the four transform
 * sizes (a 4x4's position 3 and a 32x32's position 3 are different
 * frequencies). 2 x 2 x 4 = 16. */
#define N265_NR_CATS    16
/* The flattened per-position arrays. One group is 16 + 64 + 256 + 1024 =
 * 1360 positions - the four square transform sizes - and there are four
 * groups (intra/inter x luma/chroma). */
#define N265_NR_GROUP   1360
#define N265_NR_SLOTS   (4 * N265_NR_GROUP)
/* x265's own range, carried because the flag is x265's. */
#define N265_NR_MAX     2000
/* Where a category's statistics are halved. Past this the running total
 * is an average over more content than the estimate is about, and the
 * point of the estimator is to track the CLIP and not the corpus; below
 * it the halving never fires on a 120-frame band point. Also the only
 * thing that bounds the sums, which are otherwise exact uint64. */
#define N265_NR_HALVE   ((uint64_t)1 << 22)

/* Position of (intra, chroma, log2n)'s first coefficient in the flat
 * arrays, and the category index that goes with it. The two are separate
 * because the sums are per POSITION and the block count is per CATEGORY. */
static inline int n265_nr_cat(int intra, int chroma, int log2n)
{
    return ((intra ? 0 : 1) * 2 + (chroma ? 1 : 0)) * 4 + (log2n - 2);
}

static inline int n265_nr_base(int intra, int chroma, int log2n)
{
    /* 0, 16, 80, 336: the running start of 16 + 64 + 256 + 1024 */
    static const int soff[4] = { 0, 16, 80, 336 };
    return ((intra ? 0 : 1) * 2 + (chroma ? 1 : 0)) * N265_NR_GROUP +
           soff[log2n - 2];
}

/* The encoder-wide running state. Lives on the driver and is touched by
 * nothing else: a frame's own statistics reach it through the ring in
 * encoder.h, folded at a FIXED coding-index lag (see n265_nr_fold's
 * caller), which is what makes the offsets a frame is handed independent
 * of how many workers were running. */
typedef struct n265_nr {
    uint64_t sum[N265_NR_SLOTS];    /* running total of |coef| per position */
    uint64_t cnt[N265_NR_CATS];     /* blocks quantized per category        */
    uint16_t off[N265_NR_SLOTS];    /* what the next dispatch hands a frame */
} n265_nr;

/* One frame's contribution, and one row lane's slice of it. Same shape;
 * the lane slices are summed in ROW order on the driver at retirement,
 * which is exact for integers and therefore the same total whatever
 * order the lanes ran in. */
typedef struct n265_nr_tally {
    uint64_t sum[N265_NR_SLOTS];
    uint64_t cnt[N265_NR_CATS];
} n265_nr_tally;

/* One ring entry: a retired frame's tally, stamped with the coding index
 * it belongs to. The stamp is checked on the way out, so an entry that
 * was never written - the first LAG frames of an encode - reads as absent
 * rather than as some other frame's coefficients. */
typedef struct n265_nr_hist {
    int cidx;
    int valid;
    n265_nr_tally t;
} n265_nr_hist;

/* Fold one finished frame's tally into the running state. */
void n265_nr_fold(n265_nr *nr, const n265_nr_tally *t);

/* Re-solve every offset from the running state. `intra` and `inter` are
 * the two flag values in 0..2000; a category whose strength is 0, or
 * whose block count is still 0, gets offsets of 0 and is therefore
 * exactly the quantizer without this feature. */
void n265_nr_offsets(n265_nr *nr, int intra, int inter);

/* Attenuate one transform block and tally it.
 *
 * `coef` is the forward-transformed block in RASTER order (n x n, the
 * layout n265_quant and n265_rdoq take), `out` receives the attenuated
 * copy and may not alias it. Every coefficient moves TOWARD ZERO by its
 * position's offset and stops there - a coefficient smaller than its
 * offset becomes 0 and does not change sign.
 *
 * The tally is taken on the coefficients as they ARRIVED, before the
 * subtraction: the estimator has to describe the content, not the
 * picture the previous estimate already made. */
void n265_nr_block(const uint16_t *off, n265_nr_tally *t, int16_t *out,
                   const int16_t *coef, int log2n, int intra, int chroma);

#endif
