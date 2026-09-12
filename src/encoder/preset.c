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
 * The tables are the economy ladder read outwards from medium: fast is
 * phase 4's ladder read forwards, slow is phase 6b's placement read
 * backwards. Every entry below is an arm one of those ladders priced
 * (docs/phase4-ladder.md, docs/phase6b-economy.md, docs/instruments.md);
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
    /* Five of the entries below are the shipped default too: phase 6b
     * placed medium in step 2 (RSKIP, CBF_FAST, CB64_AMVP, BI_REFINE)
     * and re-walked the ladder in step 3 (SUBME), so those lookups now
     * return the value the default would have returned anyway. They
     * stay: a preset table says what the preset IS, not how it differs
     * from whatever medium happens to be this month, and a redundant
     * entry costs one string compare. Step 4 made NO_RECT a default
     * too, so what fast still adds over the placed medium is
     * SKIP_EXIT=1, INTRA_GATE=2, RECT_FLAT=0, SBH_RD=0 and `--ctu 32`
     * in fast_par - and SKIP_EXIT is an arm the placement measured and
     * refused AT MEDIUM's operating point, not everywhere. Step 5 then
     * re-priced SKIP_EXIT and INTRA_GATE against the room phase 7 opened
     * and refused both there again, on price and on the per-clip bar, so
     * these two entries are what fast adds by twice-taken measurement
     * rather than by inheritance.
     *
     * fast inherits two defaults it does not name: `N265_PART_GATE=2`
     * from step 2 and `N265_RECT_RANK=50` from step 3. Both are inert
     * under NO_RECT by construction - one gates entry to the
     * rectangular trials and the other ranks them, and NO_RECT deletes
     * both - and the cmp of `--preset fast` against the pre-placement
     * binary proves that rather than arguing it. That cmp is also what
     * `N265_RECT_FLAT=0` below keeps true. */
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
    /* the chroma term in the candidate choice is medium's, priced at
     * 1.010x there (docs/phase7-psy-design.md, "The chroma prediction").
     * fast has not been priced for it and is pinned off, which is what
     * keeps its cmp against the pre-item binary true */
    { "N265_CHROMA_PRED", "0" },
    /* the tools medium does not have. rect and AMP are 27% of the
     * encode for 4.5 mean BD - the best block trade on the ladder */
    { "N265_NO_RECT",    "1" },
    { "N265_NO_AMP",     "1" },
    /* ...and since step 4 medium HAS `NO_RECT`, what fast adds here is
     * the ABSENCE of the flat-shaded exception. fast is the speed end
     * and its own board columns (`our_fps_fast`,
     * `bd_rate_vmafneg_fast_pct`) are charted at every milestone; the
     * exception is worth 1.18x of the animation clips' wall there, so
     * turning it on in fast is a quality change wanting its own band
     * read and is not step 4's to make. Naming 0 is what keeps
     * `--preset fast` byte-identical across step 4. */
    { "N265_RECT_FLAT",  "0" },
    /* the last points to the iso-quality line. RDOQ itself stays on:
     * turning the lattice off is 17.6 BD points for 21% of the time,
     * where the RD-decided sign-bit hiding on top of it is 6.9 points
     * for 3-8% - and at this end of the ladder 6.9 is what is left to
     * spend. */
    { "N265_SBH_RD",     "0" },
    /* ...and the cu-tree frame term phase 7 made a medium default. fast has
     * its own board columns (`our_fps_fast`, `bd_rate_vmafneg_fast_pct`) and
     * its own operating point, so a quality change there wants its own band
     * read and is not this item's to make. k_f = k_b switches the term off,
     * which is what keeps `--preset fast` byte-identical to main; the
     * positive-side bound and its content gate need no entry, both being
     * inert with the term off, and the operating point never moved. */
    { "N265_CT_FRAMEW",  "2" },
    /* phase 7 M1': the per-CTU AQ signal moved to `fine` at medium; fast
     * keeps the variance signal so it stays the measurement anchor it is
     * - the speed point, with its own charted board columns. slow kept
     * the same pin until 2026-09-12 and inherits `fine` now */
    { "N265_AQ_SIGNAL",  "0" },
    { NULL, NULL }
};

/* slow: the quality preset, re-placed 2026-09-12.
 *
 * The table was empty until phase 6b, because every knob-level candidate
 * for slow had been re-priced and none of them bought quality any more
 * (the three shipped gates cost nothing to remove, `INTRA_RD_N=8` was
 * refused a third time, a second bi-refinement sweep was a null); what
 * slow bought was structural and it is in slow_par below.
 *
 * Phase 6b filled it from the other side. Placing medium nine knobs down
 * the economy ladder in four steps made the OLD medium worth keeping
 * reachable, so slow named the pre-6b value of every knob the placement
 * moved and `--preset slow` WAS the phase-5-exit medium, byte for byte.
 * That anchor priced every rung of the ladder, and it also cost slow the
 * three things that pay only at two references per list - which is
 * slow's own structure and no other preset's: the non-IDR intra
 * placement, x265's slow window of 25, and the phase 7 quality
 * mechanisms the table had to pin OFF (the cu-tree frame term, the
 * `fine` AQ signal). So the anchor moved out of this table and into a
 * recipe - docs/instruments.md, "The phase-5 medium recipe", twelve
 * environment assignments plus `--ref 2 --bframes 7 --rc-lookahead 20`,
 * cmp-verified on bitstream and --dump-recon against the old preset
 * before this table was touched and again on the binary that ships it -
 * and slow is the quality preset again.
 *
 * What that is worth on the CIF seven against x265 4.3 medium:
 * -21.48 CQP / -21.84 CRF of band median against the old slow's
 * -19.19 / -18.34 and today's medium's -16.11 / -13.46, 7/7 in both
 * modes, HD-6 median -19.18 against -16.45 with all six ahead of medium
 * where the old slow was 7.97 points behind it on touchdown_1080p, and
 * ms_720p_50 4.72% ahead of medium where the old slow was 0.57%. It is
 * 0.986x the old slow's single-thread wall, so none of it was bought
 * with time. docs/phase7-gop-design.md, "slow re-placed".
 *
 * Order follows the ladder in docs/phase6b-economy.md: step 2's six,
 * then step 3's two and step 4's one. Step 6's intra gate is no longer
 * named - 5 and the inherited 7 are a tie the band cannot call at this
 * operating point (median -21.45 against -21.48 CQP, -21.88 against
 * -21.84 CRF) and 7 is 1.018x cheaper, so the pin went with the
 * others. */
static const struct kv slow_tab[] = {
    { "N265_CHROMA_PRED", "0" },  /* medium's chroma term, not priced
                                   * here: slow runs rect, AMP and the
                                   * CB64 arm, so its shortlist count is
                                   * a different encoder. Pinned off,
                                   * which keeps its cmp true          */
    { "N265_CB64_AMVP",  "1" },   /* the 64x64 AMVP arm back on        */
    { "N265_RSKIP",      "0" },   /* recursion never stopped early     */
    { "N265_BI_REFINE",  "1" },   /* one L0/L1 sweep                   */
    { "N265_CBF_FAST",   "3" },   /* stage 2 alone                     */
    { "N265_NO_AMP",     "0" },   /* asymmetric partitions back on     */
    { "N265_PART_GATE",  "0" },   /* rect entered unconditionally      */
    { "N265_RECT_RANK",  "0" },   /* both orientations fully searched  */
    { "N265_SUBME",      "0" },   /* the two-round subpel ladder       */
    { "N265_NO_RECT",    "0" },   /* step 4: the rectangular splits on
                                   * every frame, not the flat-typed
                                   * ones alone. N265_RECT_FLAT needs no
                                   * entry - it is inert wherever rect
                                   * is already on                     */
    /* the non-IDR intra picture, placed at a cut the IDR bound refuses.
     * It is reachable only at two references per list, which is slow's
     * own structure and no other preset's (docs/phase7-gop-design.md,
     * "Step 2 follow-up"). `N265_INTRA_MIN` needs no entry: 4, the
     * floor the IDR bound used to carry, is the one the placement
     * wants                                                          */
    { "N265_MIN_KEYINT", "0" },
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
    { "rc-lookahead", "12" },  /* fast keeps the 12-frame window until its own
                                 * read (item fastla); medium moved to 20 */
    { NULL, NULL }
};

static const struct kv slow_par[] = {
    /* A second reference per list, the full B pyramid, and the lookahead
     * window cu-tree propagates over. The reference count was re-priced
     * at MEDIUM on 2026-09-12 and refused there (1.103x the wall for a
     * median that improves and a mean that does not); it stays here
     * because two of the three things the re-placement took are only
     * reachable behind it - `la_is_nonidr_i` gates the intra placement
     * on `ref_max >= 2`, and the deeper window has a second list entry
     * to act through. slow's whole arm was re-measured with it:
     * docs/phase7-gop-design.md, "slow re-placed". */
    { "ref",           "2" },
    { "bframes",       "7" },   /* the default since 2026-09-10; kept
                                 * so slow's structure reads whole     */
    /* 25, x265's own slow depth, and the one rung of the owner's ladder
     * (fast 15, medium 20, slow 25) that measured well enough to ship:
     * 20 -> 25 is 0.83 CQP / 0.62 CRF of CIF-7 band median and 0.38 of
     * the HD-6 median for 1.002x the single-thread wall, with one clip
     * behind its 20-frame arm (sita_toon_720p, -16.16 against -16.45)
     * and multishot a wash. It is 25 frames of latency, which is what a
     * quality preset is for. This line read 20 until the re-placement
     * and 24 before that, when LA_DEPTH_MAX clamped the 24 away. */
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
