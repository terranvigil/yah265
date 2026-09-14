/*
 * yah265: the preset ladder.
 * Copyright (c) 2026, the yah265 authors. GPL-2.0-or-later; see LICENSE.
 *
 * A preset is a source of DEFAULTS, not a mode. It names a value for
 * knobs the encoder already has; the environment overrides it and an
 * explicit front-end flag overrides it.
 *
 * The ladder shifted by role on 2026-09-13 (phase 9 step 1,
 * docs/phase9-goals-plan.md sec 4). The plan's goals are read against
 * x265 `--preset medium`, so the preset that carries them has to be
 * called `medium`: a user comparing two encoders compares preset to
 * preset, and a medium-versus-medium run has to be the honest one - same
 * name, same quality class, less time. Every name therefore moved down
 * one rung:
 *
 *   fast    today's fast with the two free phase-7 tools unpinned
 *   medium  that, plus the two rungs the per-clip quality leg needed
 *   slow    today's medium, named out so it stays reachable
 *   slower  today's slow
 *
 * `medium`'s table is no longer empty, so the DEFAULT encoder moved with
 * the name. The compiled-in defaults did NOT move: `--preset slow` names
 * every one of them and is today's medium byte for byte, which is the
 * identity the rename is verified on (docs/phase9-goals-plan.md,
 * "Step 1 result", "The identities").
 *
 * Every entry below is an arm one of the ladders priced
 * (docs/phase4-ladder.md, docs/phase6b-economy.md,
 * docs/phase9-goals-plan.md, docs/instruments.md); nothing here is a
 * value invented for the preset.
 */

#include <stdlib.h>
#include <string.h>

#include "yah265.h"
#include "encoder/preset.h"

enum { PRESET_FAST = 0, PRESET_MEDIUM = 1, PRESET_SLOW = 2, PRESET_SLOWER = 3 };

struct kv {
    const char *k;
    const char *v;
};

/* fast: the rung UNDER the goal preset, and the speed end of the ladder.
 *
 * It is the old `fast_tab` minus two pins, `N265_CT_FRAMEW=2` and
 * `N265_AQ_SIGNAL=0`. Those were phase 7 mechanisms held OFF here so that
 * `--preset fast` stayed the byte-identical measurement anchor its board
 * columns were charted on - not because they cost anything. Step 1
 * measured that: the pair is 1.002x park_joy_720p / 1.037x
 * touchdown_1080p / 0.995x foreman_cif at auto threads, inside the
 * instrument, and it is worth 3.06 points of CIF-7 CRF band median and
 * 21-23 points on in_to_tree_720p and touchdown_1080p. The anchor was
 * costing five points of free quality and the rename is the moment to
 * stop paying for it. With them on, fast reads -2.32 CIF-7 / -2.76 HD-6
 * CRF against x265 medium where it read +0.74 / +5.50. */
static const struct kv fast_tab[] = {
    /* the gates the ladder priced and held: each one costs BD at OUR
     * operating point and each buys more time per point than any tool
     * drop below it */
    { "N265_RSKIP",      "2" },   /* R12: +0.14 mean BD for 1.050x     */
    { "N265_CBF_FAST",   "2" },   /* P1 stage 1 as well as stage 2     */
    { "N265_SKIP_EXIT",  "1" },   /* R10, tightest bound               */
    { "N265_SUBME",      "3" },   /* R8: one subpel round, no qpel on
                                   * the small PUs                     */
    /* the two arms phase 3 shipped as quality and the ladder measured
     * at 8% and 5% of the encode */
    { "N265_CB64_AMVP",  "0" },
    { "N265_BI_REFINE",  "0" },
    /* the chroma term in the candidate choice, priced at 1.010x where it
     * ships. Step 1 priced it on this encoder and refused it there too:
     * it costs 1.1 points of CIF-7 CRF band median (-2.32 -> -1.21) and
     * takes bus_cif backwards, to buy 0.8 of a chroma-PSNR point the
     * guard does not need - the guard is already 22 points better than
     * the old fast on the HD median without it */
    { "N265_CHROMA_PRED", "0" },
    /* the tools `slower` has. rect and AMP are 27% of the encode for 4.5
     * mean BD - the best block trade on the ladder */
    { "N265_NO_RECT",    "1" },
    { "N265_NO_AMP",     "1" },
    /* ...and the ABSENCE of the flat-shaded exception, which is worth
     * 1.18x of the animation clips' wall. Step 1 re-read it on the
     * animation clip that wants it most and it buys 1.68 BD points there
     * against a rung that buys 5.16 for less time, so it stays off at
     * both ends of the ladder */
    { "N265_RECT_FLAT",  "0" },
    /* the two rungs below are the whole difference between this preset
     * and the goal preset. */
    /* the whole-arm intra gate on the inter side's own distortion. It is
     * a DIFFERENT gate from modes 4-7, not more of one
     * (docs/phase6b-economy.md, "The intra-gate family is not monotone in
     * speed"), and at 1.10-1.26x of the wall at auto it is the largest
     * single thing fast still has that medium does not. */
    { "N265_INTRA_GATE", "2" },
    /* the RD-domain sign-bit decision off: 1.01-1.03x of the wall for
     * 4.6-5.3 BD points, the worst trade in the encoder (6b step 1),
     * which is why medium buys it back and the speed end keeps selling
     * it. */
    { "N265_SBH_RD",     "0" },
    { NULL, NULL }
};

/* medium: THE GOAL PRESET (phase 9 step 1, 2026-09-13). Today's fast
 * re-placed upward until every one of the fifteen corpus clips is at or
 * ahead of x265 4.3 medium in CRF BD-VMAF-NEG - the per-clip leg read
 * against THE REFERENCE, not against our own previous default.
 *
 * It is `fast` plus two rungs, so the table below is fast_tab without
 * `N265_INTRA_GATE` and `N265_SBH_RD`:
 *
 *   N265_SBH_RD     0 -> 2   1.01-1.03x at auto for +5.0 of CIF-7 CRF
 *                            band median
 *   N265_INTRA_GATE 2 -> 7   1.10-1.26x at auto for +5.3 more, and it is
 *                            the only arm measured that brings
 *                            sintel_720p (+2.40 -> -9.29) and
 *                            sita_toon_720p (+1.48 -> -3.06) ahead of
 *                            the reference
 *
 * ...plus, since 2026-09-13 (phase 9 step 7, the owner's call), the two
 * knobs that close plan goal 3's worst-clip leg:
 *
 *   N265_SKIP_EXIT  1 -> 2   the loosest static-skip exit
 *   N265_RDOQ       1 -> 2   the commit-only lattice
 *
 * Priced to the end as a composition in step 5 (docs/phase9-goals-plan.md
 * sec 10) and shipped in step 7 (sec 12). The pair is the only thing
 * measured in phase 9 that puts every one of the fifteen clips under the
 * 1.15x worst-clip bar - touchdown_1080p 1.24x -> 1.11x at a 0.76x
 * median - with the plan's quality and compression legs still passing
 * (worst dVMAF -0.06, worst dSIZE +0.31%, both on bus_cif). What it
 * costs, and what the owner decided to spend, is band margin on the two
 * CIF clips that had the least: stefan_cif and bus_cif go from 5-6
 * points ahead of x265 medium to about a point behind, the 15-clip CRF
 * median goes -12.63% -> -6.67%, and multishot is 5.3-5.9% behind this
 * preset's previous recipe (still 1.7% and 16.9% ahead of x265 across
 * shots). That recipe is one env list away and is named in
 * docs/instruments.md.
 *
 * The step-1 recipe, the one the two rows above replace, read -12.62%
 * CIF-7 / -14.39% HD-6 / -12.63% of 15-clip CRF band median against x265
 * 4.3 medium with all fifteen ahead, -15.29% CIF-7 CQP, multishot 7.8%
 * (CIF) and 18.3% (720p) ahead of fast, and the chroma guard better than
 * fast on every clip of the fifteen.
 *
 * What it does NOT take, measured and refused in the same item: the
 * resolution-aware coding tree (`--ctu 64` at 720p and above) with the
 * 20-frame window, which the plan expected to be the HD fix. It is not.
 * At auto threads that pair costs 1.39x touchdown_1080p, 1.70x
 * crowd_run_1080p, 1.90x sita_toon_720p and 2.04x park_joy_720p - the
 * whole distance back to today's medium - for HD quality the two rungs
 * above already deliver at 1.10-1.19x. The HD deficit at fast was never
 * `--ctu 32`; it was the two free tools and these two rungs. So the
 * coding tree stays 32 and the window 12 at every resolution (med_par),
 * which is also what keeps a CIF picture's wavefront at nine rows
 * instead of five. */
static const struct kv med_tab[] = {
    { "N265_RSKIP",      "2" },
    { "N265_CBF_FAST",   "2" },
    { "N265_SKIP_EXIT",  "2" },   /* step 7: the loosest exit bound    */
    { "N265_RDOQ",       "2" },   /* step 7: the lattice on the commit
                                   * path only. The compiled-in default
                                   * is 1 and did not move, so fast,
                                   * slow and slower are untouched     */
    { "N265_SUBME",      "3" },
    { "N265_SUBPEL_RATE", "1" },  /* item 1b: the post-search re-rank,
                                   * which item 1 refused at 1.0305x    */
    { "N265_SUBPEL_GATE", "-2" }, /* ...bought only where the ladder's
                                   * last square SEPARATED its top two
                                   * by more than 2%. The SIGN is the
                                   * item: +2, the CPRED_GATE shape,
                                   * gives back 95% of the band for the
                                   * same clock. Compiled-in defaults
                                   * are 0 and 0, so fast, slow and
                                   * slower are untouched               */
    { "N265_CB64_AMVP",  "0" },
    { "N265_BI_REFINE",  "0" },
    { "N265_CHROMA_PRED", "0" },
    { "N265_NO_RECT",    "1" },
    { "N265_NO_AMP",     "1" },
    { "N265_RECT_FLAT",  "0" },
    /* phase 10 item 3: the per-CTU AQ term back ON, with the PROTECTING
     * sign and on the variance field, on a flat-typed frame whose outline
     * share is under 3% - the content signal the 2026-09-12 refusal asked
     * for. It reaches exactly one corpus clip (sintel_720p, -6.20 -> -12.21
     * CRF and +0.84 -> -7.02 CQP) and leaves the other fourteen byte for
     * byte, `sita_toon_720p` - the clip the ungated sign cost four points -
     * included. The compiled-in defaults are 0 and ungated, so fast, slow
     * and slower are untouched and `N265_AQ_FLAT_SIGN=0` is the escape.
     * docs/phase10.md "Items 3-4". */
    { "N265_AQ_FLAT_SIGN", "1.0" },
    { "N265_AQ_FLAT_THR",  "3" },
    { NULL, NULL }
};

/* slow: TODAY'S MEDIUM, named out. Every entry is the compiled-in default
 * as it stood at f243b33, so `--preset slow` is the encoder every board
 * row up to phase 9 was measured on, byte for byte - the same device
 * phase 6b used to keep the pre-6b medium priceable, verified the same
 * way (8 cmp cells on bitstream and --dump-recon). The values are named
 * rather than left to fall through so that a later move of a default
 * cannot silently move this preset. */
static const struct kv slow_tab[] = {
    { "N265_RSKIP",      "2" },
    { "N265_CBF_FAST",   "2" },
    { "N265_SKIP_EXIT",  "0" },   /* the AMVP search always runs       */
    { "N265_SUBME",      "3" },
    { "N265_CB64_AMVP",  "0" },
    { "N265_BI_REFINE",  "0" },
    { "N265_CHROMA_PRED", "1" },  /* the chroma term in the shortlist  */
    { "N265_NO_RECT",    "1" },
    { "N265_NO_AMP",     "1" },
    { "N265_RECT_FLAT",  "1" },   /* rect on flat-typed frames         */
    { "N265_INTRA_GATE", "7" },
    { "N265_SBH_RD",     "2" },
    { NULL, NULL }
};

/* slower: today's slow, the quality preset, unchanged - it was re-placed
 * on 2026-09-12 and nothing in step 1 touches it
 * (docs/phase7-gop-design.md, "slow re-placed"). Order follows the ladder
 * in docs/phase6b-economy.md: step 2's six, then step 3's two and step
 * 4's one, then the non-IDR intra placement.
 *
 * The table names no value for `N265_INTRA_GATE`, `N265_SBH_RD`,
 * `N265_SKIP_EXIT`, `N265_CT_FRAMEW` or `N265_AQ_SIGNAL`, and it does not
 * need to: a table that misses a key falls through to the compiled-in
 * default and never to another preset's table, and the compiled-in
 * defaults did not move in the rename. */
static const struct kv slower_tab[] = {
    { "N265_CHROMA_PRED", "0" },  /* not priced in this company: slower
                                   * runs rect, AMP and the CB64 arm, so
                                   * its shortlist is a different
                                   * encoder. Pinned off, which keeps its
                                   * cmp true                          */
    { "N265_CB64_AMVP",  "1" },   /* the 64x64 AMVP arm back on        */
    { "N265_RSKIP",      "0" },   /* recursion never stopped early     */
    { "N265_BI_REFINE",  "1" },   /* one L0/L1 sweep                   */
    { "N265_CBF_FAST",   "3" },   /* stage 2 alone                     */
    { "N265_NO_AMP",     "0" },   /* asymmetric partitions back on     */
    { "N265_PART_GATE",  "0" },   /* rect entered unconditionally      */
    { "N265_RECT_RANK",  "0" },   /* both orientations fully searched  */
    { "N265_SUBME",      "0" },   /* the two-round subpel ladder       */
    { "N265_NO_RECT",    "0" },   /* the rectangular splits on every
                                   * frame, not the flat-typed ones
                                   * alone. N265_RECT_FLAT needs no
                                   * entry - it is inert wherever rect
                                   * is already on                     */
    /* the non-IDR intra picture, placed at a cut the IDR bound refuses.
     * It is reachable only at two references per list, which is this
     * preset's own structure and no other's (docs/phase7-gop-design.md,
     * "Step 2 follow-up"). `N265_INTRA_MIN` needs no entry: 4, the floor
     * the IDR bound already carried, is the one the placement wants   */
    { "N265_MIN_KEYINT", "0" },
    { NULL, NULL }
};

/* CLI-level structural parameters. Same rule: the front end's own flag
 * wins where the user gave one. */

/* 32x32 coding tree blocks and the 12-frame cu-tree window. The 64x64
 * node's own arms (whole-CU skip, merge-with-residual, the AMVP search
 * and the depth-0 split trial) are the most expensive nodes in the tree
 * and the least often taken below 720p: 1.37-1.42x on the two CIF clips
 * for 0.10 of median BD when phase 4 priced them. Step 1 read the other
 * end of the same trade at HD and the answer was the same
 * (see med_tab). */
static const struct kv fast_par[] = {
    { "ctu", "32" },
    { "rc-lookahead", "12" },
    { NULL, NULL }
};

/* medium's structure IS fast's: the goal preset was bought with two
 * knobs, not with the coding tree or the window. Spelled out rather than
 * shared with fast_par so that a later move of one is not silently a
 * move of both. */
static const struct kv med_par[] = {
    { "ctu", "32" },
    { "rc-lookahead", "12" },
    { NULL, NULL }
};

/* today's medium's structure, which is the front end's own defaults */
static const struct kv slow_par[] = {
    { "ctu", "64" },
    { "rc-lookahead", "20" },
    { NULL, NULL }
};

static const struct kv slower_par[] = {
    /* A second reference per list, the full B pyramid, and the lookahead
     * window cu-tree propagates over. The reference count was re-priced
     * at today's medium on 2026-09-12 and refused there (1.103x the wall
     * for a median that improves and a mean that does not); it stays here
     * because two of the three things the re-placement took are only
     * reachable behind it - `la_is_nonidr_i` gates the intra placement on
     * `ref_max >= 2`, and the deeper window has a second list entry to
     * act through. docs/phase7-gop-design.md, "slow re-placed". */
    { "ref",           "2" },
    { "bframes",       "7" },   /* the default since 2026-09-10; kept
                                 * so the structure reads whole        */
    /* 25, x265's own slow depth: 20 -> 25 is 0.83 CQP / 0.62 CRF of
     * CIF-7 band median and 0.38 of the HD-6 median for 1.002x the
     * single-thread wall. It is 25 frames of latency, which is what a
     * quality preset is for. */
    { "rc-lookahead", "25" },
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
    if (g_preset == PRESET_MEDIUM)
        return lookup(med_tab, name);
    if (g_preset == PRESET_SLOW)
        return lookup(slow_tab, name);
    return lookup(slower_tab, name);
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
    else if (!strcmp(name, "slower"))
        g_preset = PRESET_SLOWER;
    else
        return -1;
    return 0;
}

int n265_preset_param(const char *key, int fallback)
{
    const char *v = NULL;
    if (g_preset == PRESET_FAST)
        v = lookup(fast_par, key);
    else if (g_preset == PRESET_MEDIUM)
        v = lookup(med_par, key);
    else if (g_preset == PRESET_SLOW)
        v = lookup(slow_par, key);
    else
        v = lookup(slower_par, key);
    return v ? atoi(v) : fallback;
}
