/* Stream fingerprint (phase 14 item 10). SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef N265_FP_H
#define N265_FP_H

#include <stdint.h>

/* The signature is 32 bits and each usable site carries ONE of them,
 * chosen by where the site is rather than by how many sites came before
 * it. Two things fall out of that and both matter:
 *
 *  - nothing depends on a running count, so the stream is the same at
 *    any thread count and a reader can start anywhere; and
 *  - a reader that can only SEE half the sites still reads the mark.
 *    That is not hypothetical: the site is a merge_idx of
 *    MaxNumMergeCand-1 against MaxNumMergeCand-2, and a decoder derives
 *    only as many merge candidates as the index it read asks for, so
 *    the pair is visible from the high index and not from the low one.
 *    Position keying turns that into "the positions carrying a 1 are
 *    exactly the signature's 1 bits", which is a test.
 */
#define N265_FP_BITS 32

/* Bits 0..23 are the first three bytes of the identification SEI's UUID
 * (src/encoder/headers.c) and are FIXED for the life of the format; bits
 * 24..31 are the encoder version and move with it. A reader matches on
 * the first 24 and reports the last 8. */
uint32_t n265_fp_sig(void);

/* The signature bit the site at (x0, y0) of the picture with this
 * pic_order_cnt_lsb carries. PU top-left positions are unique within a
 * picture - the coding quadtree is a partition - so this is well
 * defined, and both sides compute it from public syntax alone. */
int n265_fp_bit(int poc_lsb, int x0, int y0);

#endif /* N265_FP_H */
