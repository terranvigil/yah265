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

/* the rung index lives in `rungs` below; nothing outside this file
 * names a preset by number any more */

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

/* faster: `fast` with the mode decision's transform trials taken away
 * (phase 13 A4/C2, 2026-09-14).
 *
 * The table is `fast_tab` spelled out again - not shared, for the reason
 * every other table here is spelled out - plus exactly two rows, and the
 * two are the mechanism this rung exists for:
 *
 *   N265_RD_PROXY 1   the inter shortlist ranked on SA8D + estimated
 *                     bits, one transform per CU instead of two
 *                     (docs/phase13.md, the A4 census)
 *   N265_EST_MODE 1   the frozen-context residual bit estimate, which
 *                     the band refused at medium and which belongs
 *                     here: a rung already spending band for speed can
 *                     afford it, and the cheaper the search the larger
 *                     the coefficient path's share of it
 *                     (docs/phase13.md, A1)
 *
 * Nothing else moves. `--ref` is 1 at every rung from `slow` down (the
 * compiled-in default), so x265's "ref 2-3 at faster" has no lever here
 * to pull. */
static const struct kv faster_tab[] = {
    { "N265_RSKIP",      "2" },
    { "N265_CBF_FAST",   "2" },
    { "N265_SKIP_EXIT",  "1" },
    { "N265_SUBME",      "3" },
    { "N265_CB64_AMVP",  "0" },
    { "N265_BI_REFINE",  "0" },
    { "N265_CHROMA_PRED", "0" },
    { "N265_NO_RECT",    "1" },
    { "N265_NO_AMP",     "1" },
    { "N265_RECT_FLAT",  "0" },
    { "N265_INTRA_GATE", "2" },
    { "N265_SBH_RD",     "0" },
    { "N265_RD_PROXY",   "1" },
    { "N265_EST_MODE",   "1" },
    /* ...and the RE-PLACEMENT (2026-09-14). The first table above left
     * fourteen of the fifteen clips ahead of x265 faster on the CRF band
     * and one behind it - sintel_720p at +2.02% - and the rule is every
     * clip. These two are medium's AQ-flat rows, and they are the
     * narrowest thing in the encoder that reaches that clip: the term is
     * armed only on a flat-typed frame whose outline share is under 3%.
     * Verified as such HERE rather than assumed from medium's record -
     * with the pair on and off at this preset, fourteen of the fifteen
     * clips are byte for byte and sintel_720p is the one that moves. Not
     * a value invented for this rung either: `medium` has carried it
     * since phase 10 item 3. */
    { "N265_AQ_FLAT_SIGN", "1.0" },
    { "N265_AQ_FLAT_THR",  "3" },
    { NULL, NULL }
};

/* veryfast: `faster` with the subpel ladder cut to its half-pel square,
 * the intra rank in a B slice cut to its two non-directional modes, and
 * the pyramid cut to four B frames. x265's veryfast drops b-intra
 * outright and takes `ctu 32`; we are already at 32 everywhere, and
 * keeping the arm with its cheap half is the version that can still win
 * a CU where a B really does want intra. */
static const struct kv vfast_tab[] = {
    { "N265_RSKIP",      "2" },
    { "N265_CBF_FAST",   "2" },
    { "N265_SKIP_EXIT",  "1" },
    { "N265_SUBME",      "7" },   /* one round, half-pel square only  */
    { "N265_CB64_AMVP",  "0" },
    { "N265_BI_REFINE",  "0" },
    { "N265_CHROMA_PRED", "0" },
    { "N265_NO_RECT",    "1" },
    { "N265_NO_AMP",     "1" },
    { "N265_RECT_FLAT",  "0" },
    { "N265_INTRA_GATE", "2" },
    { "N265_SBH_RD",     "0" },
    { "N265_RD_PROXY",   "1" },
    { "N265_EST_MODE",   "1" },
    { "N265_INTRA_B_MODES", "1" },
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
    /* the depth-0 residual gate at the 64x64 node: 0 here because at
     * this rung the coding tree is 32 below HD and there is no 64x64
     * node to gate. The UHD overlay turns it on with the tree (item 17),
     * and the compiled-in default is 0, so fast, slow and slower are
     * untouched */
    { "N265_CB64_SKIPGATE", "0" },
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
    /* phase 11 item 1: a picture whose source is already in the DPB is
     * coded all-skip toward it with the zero vector instead of being
     * discovered CU by CU. Content-gated by construction - a clip with
     * no duplicate frame never arms it and stays byte for byte what it
     * was (twelve of the fifteen, five of them cmp-verified) - and on
     * the three that do arm it, it is faster AND ahead on the band:
     * sita_toon_720p 0.962x at one thread for -1.76 -> -2.21 CRF,
     * sita_flat_720p 0.879x for -3.85 -> -4.08. The compiled-in default
     * is 0, so fast, slow and slower are untouched and `N265_DUP_FAST=0`
     * is the escape. docs/phase11.md. */
    { "N265_DUP_FAST",   "1" },
    /* parity item 3: the intra search an inter slice buys where it does
     * not pay. NxN (four 4x4 luma PBs, picked on a SAD sum) is dropped
     * at the 8x8 intra CU of a P or B slice, which is faster and ahead
     * on every read (Sol Levante pooled -1.56%, Meridian -0.92%, CIF-7
     * CRF median -1.10); and a 16x16 node whose no-split winner was
     * intra ends its recursion on a frame the lookahead calls
     * intra-heavy (sc_ratio >= 0.6), where the split wins 0.0-0.4% of
     * the time. The CIF-7 band never reaches that ratio. Compiled-in
     * defaults are 0, so fast, slow and slower are untouched and
     * `N265_INTER_NXN=0 N265_INTRA_RSKIP=0` is the escape.
     * docs/phase14.md "Item 23". */
    { "N265_INTER_NXN",  "2" },
    { "N265_INTRA_RSKIP", "3" },
    /* phase 14 item 2: weighted prediction in P slices, x265's own
     * default at this rung and above. It is a CONTENT tool and the
     * estimator is what keeps it free where the content is not there:
     * over the fifteen non-fade corpus clips the gain test declines on
     * every frame of eleven of them, and the CRF band moves by at most
     * 0.04 of a point on any clip that is not a fade or a dissolve. What
     * it buys is the two clips that are: fade_cif +11.47% -> -13.15%
     * against x265 medium, a 24.6-point swing and the largest
     * single-clip deficit this encoder had, and sintel_720p - a trailer,
     * so real dissolves - -12.55% -> -17.33%. `N265_WEIGHTB` needs no
     * entry: it is priced in docs/phase14.md and stays off, as it does
     * in x265, because it costs 0.31 of a point on akiyo_cif for the
     * table it writes in every B slice header and takes xfade_720p
     * backwards. The compiled-in default is 0, so fast and faster are
     * untouched and `--no-weightp` is the escape. */
    { "N265_WEIGHTP",    "1" },
    /* There is NO `N265_NR_INTRA` / `N265_NR_INTER` row, and item 6b is
     * why that is worth saying out loud here. Item 6 recommended
     * `--nr-intra 100 --nr-inter 100` for this table off a five-clip
     * band; 6b built the arm, read it on the seventeen, and refused it.
     * It clears all five goal-3 legs with room (0.73x median against
     * today's 0.69x, every other reading equal or better) and it moves
     * the 17-clip CRF median -7.81 -> -8.33 - and it costs
     * `sintel_720p` **1.44** points and `sita_flat_720p` 0.35 against
     * the item's 0.3 per-clip bar, monotonically in the strength, which
     * is the tool doing what it does to content that has no noise in it
     * to remove. It is also not free on this tree: 1.046x foreman_cif /
     * 1.051x park_joy_720p / 1.095x touchdown_1080p at one thread.
     *
     * The two knobs exist so the next re-pricing is a row here and not a
     * rebuild; the compiled-in default behind them is 0, which is off
     * and is x265's default. docs/phase14.md, "Item 6b". */
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
    /* phase 14 item 2, and the FIRST named exception to the paragraph
     * above: `--preset slow` is no longer f243b33's medium byte for
     * byte, because weighted prediction is a coding tool this encoder
     * did not have then and a quality rung above the goal preset should
     * not be missing what the goal preset has. The identity is one flag
     * away - `--preset slow --no-weightp` is the old encoder, and on the
     * eleven non-fade corpus clips it is that encoder anyway, the
     * estimator having declined on every frame. */
    { "N265_WEIGHTP",    "1" },
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
    { "N265_WEIGHTP",    "1" },   /* phase 14 item 2; see med_tab */
    { NULL, NULL }
};

/* veryslow: `slower` plus the ONE lever of x265's veryslow that measured
 * as a gain here - the transform tree's third level. NOT PLACED; the
 * table is what the measurement left, and docs/phase14.md "Item 5" is
 * why.
 *
 * The table below is `slower_tab` spelled out again - not shared, for the
 * reason every other table here is spelled out - plus two rows that name
 * values `slower` already inherits from the compiled-in defaults, so that
 * a later move of a default cannot quietly take the top rung's quantiser
 * or its merge list away. Everything else x265 spends at this name was
 * built, measured on the CRF band at `--preset slower`, and DROPPED:
 *
 *   --ref 4 / --ref 5   1.62x / 1.92x of the foreman single-thread wall
 *                       and 1.43x / 1.76x of park_joy's, for a CIF-7 band
 *                       median that gets WORSE (+0.16% / +0.25%, three of
 *                       seven clips ahead). Only foreman gains. The phase
 *                       7 record said the same thing one reference lower.
 *   --bframes 8 with
 *   --rc-lookahead 40   about four points of CIF-7 band median against
 *                       x265 veryslow (+2.11% -> +6.37%). The deep
 *                       window's own note already says the longer run
 *                       costs band on top of the deeper window, and
 *                       docs/instruments.md says depth is priced per
 *                       preset and 24 already broke a clip's bar at
 *                       medium. 40 is well past anything measured.
 *   N265_SUBPEL_RATE 1
 *   ungated             +0.18 of band median. Medium buys it GATED for a
 *                       reason and this rung has no reason to buy it
 *                       ungated.
 *   --b-adapt 2         x265's own b-adapt at this name, and a refused
 *                       arm here since phase 7.
 */
static const struct kv veryslow_tab[] = {
    { "N265_CHROMA_PRED", "0" },
    { "N265_CB64_AMVP",  "1" },
    { "N265_RSKIP",      "0" },
    { "N265_BI_REFINE",  "1" },
    { "N265_CBF_FAST",   "3" },
    { "N265_NO_AMP",     "0" },
    { "N265_PART_GATE",  "0" },
    { "N265_RECT_RANK",  "0" },
    { "N265_SUBME",      "0" },
    { "N265_NO_RECT",    "0" },
    { "N265_MIN_KEYINT", "0" },
    { "N265_RDOQ",       "1" },   /* the lattice in EVERY RD trial; the
                                   * compiled-in default, named here     */
    { "N265_MERGE_RD_N", "5" },   /* the whole merge list gets a trial;
                                   * MAX_MERGE, named for the same reason */
    { NULL, NULL }
};

/* placebo: veryslow with the transform tree's FOURTH level, which is the
 * only thing left on x265's placebo column that this encoder has a lever
 * for - `--merange` does not exist here, `--max-merge 5` and
 * `--limit-refs 0` are already what we do, and `--ref 5` and the deeper
 * window are the two the measurement refused at `veryslow`. NOT PLACED
 * either. Depth 4 is the one lever on this rung that is NOT free: it is
 * worth more than depth 3 on the CIF-7 median (-0.78% against -0.53%)
 * and it takes mobile_cif backwards by 0.16%, which is what a 4x4
 * transform of a busy inter residual costs in syntax. */
static const struct kv placebo_tab[] = {
    { "N265_CHROMA_PRED", "0" },
    { "N265_CB64_AMVP",  "1" },
    { "N265_RSKIP",      "0" },
    { "N265_BI_REFINE",  "1" },
    { "N265_CBF_FAST",   "3" },
    { "N265_NO_AMP",     "0" },
    { "N265_PART_GATE",  "0" },
    { "N265_RECT_RANK",  "0" },
    { "N265_SUBME",      "0" },
    { "N265_NO_RECT",    "0" },
    { "N265_MIN_KEYINT", "0" },
    { "N265_RDOQ",       "1" },
    { "N265_MERGE_RD_N", "5" },
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

/* faster's structure IS fast's: the rung was bought with the mode
 * decision, not with the coding tree or the window. */
static const struct kv faster_par[] = {
    { "ctu", "32" },
    { "rc-lookahead", "12" },
    { NULL, NULL }
};

/* veryfast: the same tree and window, four B frames instead of seven.
 * `ref` needs no entry - 1 is the compiled-in default and is what every
 * rung from `slow` down already runs. */
static const struct kv vfast_par[] = {
    { "ctu", "32" },
    { "rc-lookahead", "12" },
    { "bframes", "4" },
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

/* ...except the coding tree on a UHD-class picture (phase 14 item 12,
 * shipped with item 17's gate 2026-09-17). The row above was priced at
 * CIF, 720p and 1080p and is right there; it was never read at 2160p,
 * and at 2160p it is wrong. The reason is the tree's reach in PICTURE
 * terms: a 32x32 tree over a 3840x2160 frame is 8160 trees whose largest
 * node covers 0.02% of the picture, and 4K content's flat regions are
 * far larger than that, so every one of them is re-signalled tree by
 * tree. Item 16 counted the consequence: at the bottom rung our 2160p B
 * slices are 99.5% skipped at 32x32 and `merge_idx` alone is 19-29% of
 * the stream.
 *
 * Measured on two 4K sources - Meridian 600 frames and bbb_sunflower 300
 * frames - the numbers are in docs/phase14.md "Item 12" for the tree and
 * "Item 17" for the gate it ships with. Everything else about `medium`
 * is untouched, this rung included: the window stays 12 at every
 * resolution, and below the gate the bitstream is what main wrote, byte
 * for byte.
 *
 * THE RULE is on luma AREA, not on height: `w * h >= 3840 * 1600`. A 4K
 * scope master is delivered both ways - 3840x2160 with the bars coded
 * and 3840x1608 with them cropped - and both are the same picture, so a
 * height test would take the tree off the second one. 2560x1440 (3.7 M)
 * and 1920x1080 (2.1 M) are well below it and are the 1080p refusal
 * phase 9 step 1 made and item 12 re-read and kept. The measurement
 * itself is at 3840x2160 only; the allowance is the same picture with
 * its bars removed and nothing wider than that.
 *
 * Since phase 14 item 22 the same row also answers on an HD-class
 * picture (luma w*h >= 1920*800), with its own rate switch
 * (N265_HD_QP). Item 12's reason holds one octave down too: on
 * Meridian 1080p the 32x32 tree codes x265's crf-32 bytes at 63.0
 * VMAF-NEG where the gated 64x64 tree codes them at 80.7. What held the
 * row back at 1080p was the clock, and the switch is what pays it. */
static const struct kv med_par_uhd[] = {
    { "ctu", "64" },
    { NULL, NULL }
};

/* ...and the knob that makes that tree affordable (phase 14 item 17).
 * Item 12 refused the 64x64 node on the WALL: fifteen points of
 * BD-VMAF-NEG for 1.29x at equal bytes. The gate runs the node's
 * cheapest arm - one merge candidate, its motion compensation and the
 * SSD after it - before any of the others, and where that depth-0
 * residual is under a sixteenth of the clean-skip bound the node is
 * decided there: no CU arm, no merge-with-residual, no AMVP search, and
 * no split trial into four 32x32 trees.
 *
 * 64 is the bound the EQUAL-BYTES wall picked, and it is the only one of
 * the three that cleared both bounds there: at --crf 19 against today's
 * medium at --crf 21 on Meridian 600, 16 reads 1.136x at one thread and
 * 1.160x at auto, 32 reads 1.053x and 1.139x, and 64 reads 0.945x and
 * 1.017x. 16 and 32 keep more of the quality - 15.3 and 12.7 of item
 * 12's fifteen points against 64's 5.0 - and neither can be afforded.
 *
 * What makes 64's quality cost payable is that the tree is no longer
 * taken across the whole band: the rate switch in encoder.c only takes
 * it at or above N265_UHD_QP, which is the half of the band where the
 * per-rung read says the tree is ahead. Below it the encode is
 * `--ctu 32` and this row is unreachable.
 *
 * The same row rides with the HD-class tree (item 22): the equal-bytes
 * wall on Meridian 1080p reads it at 1.037x at x265 crf 23's bytes and
 * 0.689x at crf 32's.
 *
 * It rides with the tree and only with the tree. Below the HD rule the
 * coding tree is 32, there is no 64x64 node and the knob is inert; at
 * `slow` and `slower`, where the tree IS 64 at every resolution, there
 * is no overlay and those two rungs are unmoved. The value is the bound
 * in 1024ths of skip_clean_thr(qp, 64); ctu.c's N265_CB64_SKIPGATE
 * comment is the rule. */
static const struct kv med_tab_uhd[] = {
    { "N265_CB64_SKIPGATE", "64" },
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


/* veryslow's and placebo's structure IS `slower`'s, plus the transform
 * tree. Every other row x265 moves at these names - a fourth and fifth
 * reference, eight B frames, a forty- and sixty-frame window - was built,
 * measured and dropped; see veryslow_tab's comment for the numbers. */
static const struct kv veryslow_par[] = {
    { "ctu",             "64" },
    { "ref",             "2" },
    { "bframes",         "7" },
    { "rc-lookahead",    "25" },
    { "tu-inter-depth",  "3" },
    { "tu-intra-depth",  "3" },
    { NULL, NULL }
};

static const struct kv placebo_par[] = {
    { "ctu",             "64" },
    { "ref",             "2" },
    { "bframes",         "7" },
    { "rc-lookahead",    "25" },
    { "tu-inter-depth",  "4" },
    { "tu-intra-depth",  "4" },
    { NULL, NULL }
};

/* ---- the ladder, and x265's names for it (phase 13 C2/C3) ------------
 *
 * x265 prints ten preset names and users compare preset to preset, so
 * every name we do not have is a comparison we lose by default. The two
 * arrays below are the whole of the naming: `rungs` is what is PLACED -
 * a rung earns a row here only by passing the four legs against the
 * x265 preset of the same name, on every one of the fifteen corpus clips
 * (docs/phase9-goals-plan.md sec 4, the rule medium was placed by) - and
 * `x265_names` is the ladder those names sit on, fastest first, so that
 * a name we know and have not placed can be refused with the nearest
 * rung that IS placed rather than with "unknown".
 *
 * Adding a name here without the legs behind it would be the one thing
 * this ladder is for undoing. */
struct rung {
    const char *name;
    const struct kv *tab;
    /* Knob rows that REPLACE `tab`'s on a UHD-class picture - the env
     * half of what par_uhd below is for the CLI's own parameters (phase
     * 14 item 17). NULL where the rung has none. Read only after the
     * front end has told this file the picture's size. */
    const struct kv *tab_uhd;
    const struct kv *par;
    /* Structural rows that REPLACE `par`'s on a UHD-class picture, or
     * NULL where the rung has none (phase 14 item 12). It is an overlay
     * and not a second table: a key absent here falls through to `par`,
     * so a rung says only what resolution moves. */
    const struct kv *par_uhd;
    int placed;             /* 0 = built, measured and REFUSED */
};

/* The UHD rule, in luma samples. See med_par_uhd for why it is an area
 * and not a height. */
#define N265_UHD_SAMPLES (3840L * 1600L)
/* The HD rule (phase 14 item 22), the same allowance one octave down:
 * 1920x1080 with its bars coded and 1920x800 with them cropped are the
 * same picture. 1280x720 (0.92 M) is under it and stays on the 32x32
 * tree whatever the rate. The overlays are the SAME rows at both classes;
 * what differs is the planned QP the rate switch in encoder.c takes the
 * tree at, so the class is kept rather than folded to a flag. */
#define N265_HD_SAMPLES (1920L * 800L)

/* 0 below HD, 1 HD-class, 2 UHD-class; 0 when either side is unknown */
static int res_class(int width, int height)
{
    long a = (long)width * (long)height;
    if (width <= 0 || height <= 0)
        return 0;
    return a >= N265_UHD_SAMPLES ? 2 : a >= N265_HD_SAMPLES ? 1 : 0;
}

static const struct rung rungs[] = {
    /* `veryfast` is present and NOT placed. It was built, measured and
     * refused on 2026-09-14: on the CRF band against x265's own veryfast
     * it reads a median of -1.70% with only 8 of the 15 clips ahead, and
     * the placement rule is EVERY clip at or ahead. The seven behind are
     * in_to_tree_720p +16.85%, mobile_cif +15.13%, stefan_cif +14.80%,
     * bus_cif +10.72%, sintel_720p +6.29%, tempete_cif +5.33% and
     * coastguard_cif +2.14%. It keeps its row
     * rather than being deleted because a refuted arm keeps its
     * re-pricing value - the levers are one line from being re-tried
     * without the one the evidence points at - and because a row here is
     * what lets the refusal say which rung was tried and failed rather
     * than pretending the name is unknown. docs/phase13.md, "C2". */
    { "veryfast", vfast_tab, NULL,  vfast_par, NULL,  0 },
    /* `faster` is present and NOT placed either, and for a different
     * reason from veryfast's. It PASSES the band on every one of the
     * fifteen - median -9.96% BD-VMAF-NEG against x265 faster, 15/15
     * ahead - and it fails three of the four legs against that same
     * preset, two of them on one clip:
     *
     *   median speed 0.81x            PASS
     *   worst clip   touchdown_1080p 1.44x   (bar 1.15x)   FAIL
     *   quality      bus_cif -0.56 dVMAF     (bar -0.5)    FAIL
     *   compression  bus_cif +2.64%          (bar +1.0%)   FAIL
     *
     * docs/phase13.md, "C2". The table stays for the same reason
     * veryfast's does, and the two binding clips are the same two the
     * project already has open items on. */
    { "faster",   faster_tab, NULL, faster_par, NULL, 0 },
    { "fast",     fast_tab, NULL,   fast_par, NULL,   1 },
    { "medium",   med_tab, med_tab_uhd, med_par, med_par_uhd, 1 },
    { "slow",     slow_tab, NULL,   slow_par, NULL,   1 },
    { "slower",   slower_tab, NULL, slower_par, NULL, 1 },
    /* `veryslow` and `placebo` are present and NOT placed (2026-09-14,
     * phase 14 item 5). The binding leg is the BAND, and it binds a long
     * way above them: on the CRF band against the x265 preset of their
     * own name, the best composition either could be given reads +2.11%
     * and +3.78% of CIF-7 median with two of seven clips ahead, and the
     * rule is every clip.
     *
     * The reason is not in these two tables. `--preset slow` reads
     * +10.24% against x265 `slow` and `--preset slower` +1.18% against
     * x265 `slower`, both with `placed = 1` they were given before C1's
     * instrument existed and against x265 MEDIUM. Our ladder's top does
     * not earn its own names yet, and a rung built on top of it inherits
     * that. `--preset medium` still reads -6.65% with all seven ahead,
     * which is the control this item ran to be sure the instrument and
     * the corpus had not moved. docs/phase14.md, "Item 5". */
    { "veryslow", veryslow_tab, NULL, veryslow_par, NULL, 0 },
    { "placebo",  placebo_tab, NULL,  placebo_par, NULL,  0 },
};
#define N_RUNGS ((int)(sizeof(rungs) / sizeof(rungs[0])))

/* x265 4.3's own ladder, fastest first */
static const char *const x265_names[] = {
    "ultrafast", "superfast", "veryfast", "faster", "fast",
    "medium", "slow", "slower", "veryslow", "placebo",
};
#define N_X265 ((int)(sizeof(x265_names) / sizeof(x265_names[0])))

static int g_preset = 3;        /* medium: rungs[3] */

/* The resolution class of the picture this run is encoding (res_class:
 * 0 below HD, 1 HD, 2 UHD). Set once by the front end (n265_preset_uhd)
 * before the encoder is opened, because the geometry is the front end's
 * to know; zero until it is, which is the resolution-blind answer every
 * caller got before item 17. */
static int g_uhd;

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
    if (g_uhd && rungs[g_preset].tab_uhd) {
        v = lookup(rungs[g_preset].tab_uhd, name);
        if (v)
            return v;
    }
    return lookup(rungs[g_preset].tab, name);
}

/* The picture's size, told to the preset table once (phase 14 item 17).
 * Some of what a rung offers is resolution-aware and the front end is
 * where the geometry is known; this is the knob half of what
 * n265_preset_param_res is for the CLI's own parameters, and it is a
 * SETTER for the reason that one is not: a knob is read from deep inside
 * the encoder, where the picture is not an argument. Zero for either
 * dimension puts the table back to resolution-blind. */
void n265_preset_uhd(int width, int height)
{
    g_uhd = res_class(width, height);
}

int n265_preset_res_class(void)
{
    return g_uhd;
}

/* ---- reading the selected rung back out (phase 14 item 13) ----------
 *
 * `--show-config` and `--verbose` print what the run RESOLVED, and the
 * knob half of that is this file's table. Both readers below are pure:
 * they name the rung that is selected and walk its table in place. No row
 * moves and no table is copied - a getter that reordered the ladder to
 * print it would be a second source of truth for what a preset sets. */
const char *n265_preset_selected(void)
{
    return rungs[g_preset].name;
}

/* Row `i` of the selected rung's table, 1 while there is one. `env`
 * receives the environment's value for that key, or NULL where the
 * environment is silent - which is exactly the condition n265_env uses to
 * fall through to the table, so the caller can say which of the two won
 * without re-deriving the rule. */
int n265_preset_row(int i, const char **key, const char **val,
                    const char **env)
{
    const struct kv *t = rungs[g_preset].tab;
    for (int n = 0; t[n].k; n++) {
        if (n != i)
            continue;
        *key = t[n].k;
        *val = t[n].v;
        if (g_uhd && rungs[g_preset].tab_uhd) {
            const char *u = lookup(rungs[g_preset].tab_uhd, t[n].k);
            if (u)
                *val = u;       /* what the run RESOLVED (item 17) */
        }
        *env = getenv(t[n].k);
        return 1;
    }
    return 0;
}

/* the index of a PLACED rung by name, or -1 */
static int rung_of(const char *name)
{
    for (int i = 0; i < N_RUNGS; i++)
        if (rungs[i].placed && !strcmp(rungs[i].name, name))
            return i;
    return -1;
}

/* The placed rung nearest `name` on x265's ladder, or NULL when the name
 * is not one of x265's at all. Ties - a name equidistant from a placed
 * rung either side - go to the SLOWER one, because a user who asked for
 * a speed we do not have is better served by being told about quality we
 * do have than by being pointed at a rung that is not the speed either. */
const char *n265_preset_nearest(const char *name)
{
    int at = -1;
    if (!name)
        return NULL;
    for (int i = 0; i < N_X265; i++)
        if (!strcmp(x265_names[i], name))
            at = i;
    if (at < 0)
        return NULL;
    for (int d = 0; d < N_X265; d++) {
        if (at + d < N_X265 && rung_of(x265_names[at + d]) >= 0)
            return x265_names[at + d];
        if (at - d >= 0 && rung_of(x265_names[at - d]) >= 0)
            return x265_names[at - d];
    }
    return NULL;
}

/* 0 placed, -1 not a name at all, -2 one of x265's names with no table.
 * The caller tells the last two apart in its message. */
int n265_set_preset(const char *name)
{
    int i;
    if (!name)
        return -1;
    i = rung_of(name);
    if (i >= 0) {
        g_preset = i;
        return 0;
    }
    return n265_preset_nearest(name) ? -2 : -1;
}

int n265_preset_param(const char *key, int fallback)
{
    const char *v = lookup(rungs[g_preset].par, key);
    return v ? atoi(v) : fallback;
}

/* The same question asked about a picture of a known size (phase 14
 * item 12). A rung's `par_uhd` overlay answers first where the picture
 * is UHD-class and the overlay names the key; everything else falls
 * through to the resolution-blind table above, so a caller that does not
 * know its geometry - or one asking about a rung with no overlay - gets
 * the answer it always got. Nothing here is a mode: this is still a
 * source of DEFAULTS and the front end's own flag still wins. */
int n265_preset_param_res(const char *key, int fallback, int width,
                          int height)
{
    const struct kv *uhd = rungs[g_preset].par_uhd;
    if (uhd && res_class(width, height)) {
        const char *v = lookup(uhd, key);
        if (v)
            return atoi(v);
    }
    return n265_preset_param(key, fallback);
}

/* ---- -t/--tune: one accepted name and four named refusals -----------
 *
 * Phase 14 item 15. x265 has five tunes and they are not one feature:
 * one of them is a composition of flags this encoder already owns, and
 * the other four each ask for something this project has not got or has
 * decided against. A single verdict would flatten that, so each name is
 * answered by name.
 *
 *   zerolatency - SHIPPED. Nothing in it is new: no B frames, a window
 *                 of one, no adaptive run length, no cu-tree propagation
 *                 (there is no window to propagate over), and one frame
 *                 in flight. The last is bit-neutral by gate - output is
 *                 byte-identical at every thread count - and is here
 *                 because latency is what the name promises.
 *   psnr, ssim  - REFUSED. Both are metric fitting: x265's psnr turns AQ
 *                 and psy off because those tools cost PSNR while
 *                 improving the picture. docs/phase7-psy-design.md
 *                 closes that by argument and the band decides on
 *                 VMAF-NEG, so a tune that trades the metric we read for
 *                 one we do not would be shipping a worse encoder under
 *                 a better number.
 *   grain       - REFUSED, and it is NOT --film-grain: x265's tune grain
 *                 is a rate-control shape (--ipratio, --pbratio, --qpstep,
 *                 AQ and cu-tree off, SAO off, psy-rd 4.0, psy-rdoq
 *                 10.0, constant VBV) built around --rc-grain, whose
 *                 whole content is damping constants. Those come from a
 *                 band read on a grain clip, which is phase 13 item D.
 *   fastdecode  - REFUSED. Two of its five legs are tools this encoder
 *                 does not let a user switch: there is no deblocking
 *                 disable at all, and SAO has only a diagnostic env
 *                 knob. A tune that silently delivered three of five
 *                 would be worse than one that says which two are
 *                 missing.
 *
 * The composition is here rather than in the CLI so it can be unit
 * tested: a name table that expands to the wrong numbers is exactly the
 * kind of thing no encode notices.
 */
static const struct tune_row {
    const char *name, *alias;
    int bframes, lookahead, badapt, cutree, framework;
    const char *why;            /* NULL where the tune is accepted */
} tunes[] = {
    { "zerolatency", "zero-latency", 0, 1, 0, 0, 1, NULL },
    { "psnr", NULL, -1, -1, -1, -1, -1,
      "it is metric fitting: x265's psnr turns adaptive quantisation and "
      "psy off because both cost PSNR while improving the picture. This "
      "encoder's band decides on VMAF-NEG and "
      "docs/phase7-psy-design.md closes metric fitting by argument, so "
      "the tune would ship a worse encoder under a better number." },
    { "ssim", NULL, -1, -1, -1, -1, -1,
      "same reason as psnr: it fits the encoder to a metric this project "
      "does not decide on (docs/phase7-psy-design.md). Name --ssim if "
      "what you want is the number reported." },
    { "grain", NULL, -1, -1, -1, -1, -1,
      "it is not --film-grain. x265's tune grain is a rate-control shape "
      "around --rc-grain, whose whole content is damping constants - how "
      "much upward QP movement a grainy frame is allowed per frame - and "
      "those come from a band read on a grain clip, which is phase 13 "
      "item D. --film-grain (the SEI) and --film-grain-denoise are here "
      "today and are a different tool." },
    { "fastdecode", "fast-decode", -1, -1, -1, -1, -1,
      "two of its five legs are not flags this encoder owns: there is no "
      "deblocking-filter disable, and SAO has only a diagnostic env "
      "knob, not a user switch. The three it could deliver "
      "(--no-weightp, --no-weightb, no intra in B) would be a tune that "
      "half worked. Name those directly if you want them." },
    { NULL, NULL, 0, 0, 0, 0, 0, NULL }
};

const char *n265_tune_name(int i)
{
    if (i < 0 || i >= (int)(sizeof tunes / sizeof tunes[0]) - 1)
        return NULL;
    return tunes[i].name;
}

int n265_tune_parse(const char *name, int *t, const char **why)
{
    if (why)
        *why = NULL;
    if (!name || !t)
        return N265_TUNE_UNKNOWN;
    const struct tune_row *r = NULL;
    for (int i = 0; tunes[i].name; i++)
        if (!strcmp(name, tunes[i].name) ||
            (tunes[i].alias && !strcmp(name, tunes[i].alias)))
            r = &tunes[i];
    if (!r)
        return N265_TUNE_UNKNOWN;
    if (r->why) {
        if (why)
            *why = r->why;
        return N265_TUNE_REFUSED;
    }
    t[N265_TUNE_BFRAMES]   = r->bframes;
    t[N265_TUNE_LOOKAHEAD] = r->lookahead;
    t[N265_TUNE_BADAPT]    = r->badapt;
    t[N265_TUNE_CUTREE]    = r->cutree;
    t[N265_TUNE_FRAMEWORK] = r->framework;
    return N265_TUNE_OK;
}
