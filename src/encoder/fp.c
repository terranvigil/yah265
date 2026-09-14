/* Stream fingerprint: the signature, and which bit a site carries.
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Phase 14 item 10; docs/stream-fingerprint.md is the design and
 * docs/stream-fingerprint-census.md is the measurement it rests on. The
 * short version: the identification SEI is the first thing a remux or a
 * one-line NAL filter drops, so a second mark lives in a choice the
 * syntax leaves open - two codings that decode to the same picture for
 * the same number of bits. HEVC has exactly one such choice at a useful
 * rate, and this file supplies the payload that rides it.
 *
 * Nothing here touches a sample or a bit count. What it decides is which
 * of two equal-length merge_idx values gets written at a site where the
 * merge candidate list holds the same motion at both of the last two
 * indices (src/encoder/entropy.c, fp_merge_idx).
 */

#include "fp.h"
#include "../../include/yah265.h"

/* The first three bytes of n265_write_info_sei's UUID (headers.c), in
 * the order that writer emits them, and then the version. One table for
 * both marks: a reader that knows the SEI knows the fingerprint.
 *
 * The sibling encoders take this same 24-and-8 layout with their OWN
 * UUID's first three bytes, so the three schemes are one scheme. */
#define FP_UUID0 0x7a
#define FP_UUID1 0xa4
#define FP_UUID2 0x16

/* major.minor.patch packed 3:3:2. It moves with the release, which is
 * why it sits above the match key and not inside it. */
#define FP_VER ((uint32_t)(((YAH265_VERSION_MAJOR & 7) << 5) | \
                           ((YAH265_VERSION_MINOR & 7) << 2) | \
                           (YAH265_VERSION_PATCH & 3)))

uint32_t n265_fp_sig(void)
{
    return (uint32_t)FP_UUID0 | ((uint32_t)FP_UUID1 << 8) |
           ((uint32_t)FP_UUID2 << 16) | (FP_VER << 24);
}

int n265_fp_bit(int poc_lsb, int x0, int y0)
{
    /* An integer mix, not a cryptographic one: all it has to do is stop
     * the position from correlating with the picture, so that a clip's
     * sites do not all land on the same few signature bits. Written in
     * uint32_t arithmetic with explicit masking so that the verifier's
     * Python reproduces it exactly. */
    uint32_t z = (uint32_t)(poc_lsb & 255) * 0x9e3779b1u;
    z ^= (uint32_t)(x0 & 0xffff) * 0x85ebca6bu;
    z ^= (uint32_t)(y0 & 0xffff) * 0xc2b2ae35u;
    z ^= z >> 15;
    z *= 0x2545f491u;
    z ^= z >> 13;
    return (int)((n265_fp_sig() >> (z & (N265_FP_BITS - 1))) & 1u);
}
