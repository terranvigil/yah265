/*
 * yah265: the preset ladder.
 * Copyright (c) 2026, the yah265 authors. GPL-2.0-or-later; see LICENSE.
 *
 * A preset is a source of DEFAULTS, not a mode. It names a value for
 * knobs the encoder already has; the environment overrides it, an
 * explicit front-end flag overrides it, and `medium`'s table is empty so
 * the default encoder is the one every gate has always measured, byte
 * for byte.
 *
 * The tables are the phase-4 ladder read backwards. Every entry below is
 * an arm that ladder priced (docs/phase4-ladder.md, docs/instruments.md);
 * nothing here is a value invented for the preset.
 */

#include <stdlib.h>
#include <string.h>

#include "yah265.h"
#include "encoder/preset.h"

enum { PRESET_FAST = 0, PRESET_MEDIUM = 1, PRESET_SLOW = 2 };

struct kv {
    const char *k;
    const char *v;
};

/* fast: the iso-quality point against x265 medium on the CIF band. The
 * tools it drops are the ones the ladder measured as our lead over
 * medium - the rectangular and asymmetric partitions, the second subpel
 * round, the 64x64 node - plus every gate the ladder priced and held for
 * costing more BD than the price rule allowed at our own operating
 * point. RDOQ stays on: it is the most BD-dense thing in the encoder and
 * the worst thing on the list to sell. At iso-quality the price rule is a different question: what is
 * being bought is not speed at a BD cost but the same BD as the
 * reference at less time. */
static const struct kv fast_tab[] = {
    /* the gates the ladder priced and held: each one costs BD at OUR
     * operating point and each buys more time per point than any tool
     * drop below it */
    { "N265_RSKIP",      "2" },   /* R12: +0.14 mean BD for 1.050x     */
    { "N265_CBF_FAST",   "2" },   /* P1 stage 1 as well as stage 2     */
    { "N265_INTRA_GATE", "2" },   /* P3: gate the intra ARM, not its RD */
    { "N265_SKIP_EXIT",  "1" },   /* R10, tightest bound               */
    { "N265_SUBME",      "3" },   /* R8: one subpel round, no qpel on
                                   * the small PUs                     */
    /* the two arms phase 3 shipped as quality and the ladder measured
     * at 8% and 5% of the encode */
    { "N265_CB64_AMVP",  "0" },
    { "N265_BI_REFINE",  "0" },
    /* the tools medium does not have. rect and AMP are 27% of the
     * encode for 4.5 mean BD - the best block trade on the ladder */
    { "N265_NO_RECT",    "1" },
    { "N265_NO_AMP",     "1" },
    /* the last points to the iso-quality line. RDOQ itself stays on:
     * turning the lattice off is 17.6 BD points for 21% of the time,
     * where the RD-decided sign-bit hiding on top of it is 6.9 points
     * for 3-8% - and at this end of the ladder 6.9 is what is left to
     * spend. */
    { "N265_SBH_RD",     "0" },
    { NULL, NULL }
};

/* slow: today's defaults plus the arms that were refused only on speed.
 * The table is EMPTY on purpose. Every knob-level candidate for it was
 * re-priced on this base and none of them buys quality any more - the
 * three shipped gates cost nothing to remove (`CBF_FAST=0` reads -15.99
 * median against the default's -16.83, and all three off together
 * -16.13), a wider intra RD shortlist is refused a third time
 * (`INTRA_RD_N=8`, -16.27), and a second bi-refinement sweep is a null
 * (`BI_REFINE=2`, -16.83 to the digit). What slow buys is structural,
 * and it is in slow_par below. Numbers in docs/phase4-ladder.md. */
static const struct kv slow_tab[] = {
    { NULL, NULL }
};

/* CLI-level structural parameters. Same rule: the front end's own flag
 * wins where the user gave one. */
static const struct kv fast_par[] = {
    /* 32x32 coding tree blocks. The 64x64 node's own arms (whole-CU
     * skip, merge-with-residual, the AMVP search and the depth-0 split
     * trial) are the most expensive nodes in the tree and the least
     * often taken at this resolution: 1.37-1.42x on the two CIF clips
     * for 0.10 of median BD. */
    { "ctu", "32" },
    { NULL, NULL }
};

static const struct kv slow_par[] = {
    /* A second reference per list (medium runs three), the full B
     * pyramid, and the lookahead window cu-tree propagates over. On the
     * CIF band, against the default: ref 2 is worth 1.40 median / 2.16
     * mean, the B run another 0.25 median / 2.59 mean with the worst
     * clip improving by five points, and the longer window 0.71 median.
     * Together -19.19 median / -21.61 mean against -16.83 / -16.59, at
     * about 1.5x the encode time. */
    { "ref",           "2" },
    { "bframes",       "7" },
    { "rc-lookahead", "24" },
    { NULL, NULL }
};

static int g_preset = PRESET_MEDIUM;

static const char *lookup(const struct kv *t, const char *k)
{
    for (; t->k; t++)
        if (!strcmp(t->k, k))
            return t->v;
    return NULL;
}

const char *n265_env(const char *name)
{
    const char *v = getenv(name);
    if (v)
        return v;
    if (g_preset == PRESET_FAST)
        return lookup(fast_tab, name);
    if (g_preset == PRESET_SLOW)
        return lookup(slow_tab, name);
    return NULL;
}

int n265_set_preset(const char *name)
{
    if (!name)
        return -1;
    if (!strcmp(name, "fast"))
        g_preset = PRESET_FAST;
    else if (!strcmp(name, "medium"))
        g_preset = PRESET_MEDIUM;
    else if (!strcmp(name, "slow"))
        g_preset = PRESET_SLOW;
    else
        return -1;
    return 0;
}

int n265_preset_param(const char *key, int fallback)
{
    const char *v = NULL;
    if (g_preset == PRESET_FAST)
        v = lookup(fast_par, key);
    else if (g_preset == PRESET_SLOW)
        v = lookup(slow_par, key);
    return v ? atoi(v) : fallback;
}
