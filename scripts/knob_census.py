#!/usr/bin/env python3
# Copyright (c) 2026, the yah265 authors
# SPDX-License-Identifier: GPL-2.0-or-later
# Ported from yah264 (codec-agnostic instrument; the encoder binary, its flags
# and the reference encoder are the only changes).
"""knob_census.py - the N265_* knob census (docs/knobs.md), generated + checked.

The 2026-08-20 census round's standing instrument. Two modes:

  python3 scripts/knob_census.py            # regenerate docs/knobs.md
  python3 scripts/knob_census.py --check    # CI mode: exit 1 if the tree and
                                            # the catalog disagree, or a
                                            # default-comment contradiction
                                            # appears

WHAT IT ENFORCES
- Every env knob read in src/ + cli/ appears in docs/knobs.md (no dark knobs:
  a knob nobody can find is a knob the next session rebuilds).
- No catalog entry survives its reader (no ghost entries).
- No STALE DEFAULT COMMENT: a comment within 40 lines above a reader claiming
  "default OFF/0" while the parsed default is nonzero (or the reverse). This
  exact staleness baited a wasted rebuild once (the phase-B kernel, 08-18)
  and misdescribed two knobs after the 08-20 flip; it is the one comment
  class worth policing mechanically.

TIERS (heuristic, then hand-corrected in the OVERRIDES table below):
  default    - resolves nonzero/nonempty with no env: a shipped default; its
               escape is the env
  instrument - measurement/probe/oracle/dump machinery (name pattern or
               listed in docs/instruments.md)
  arm        - default-off env-gated feature or kept refuted arm (the flip-
               first lesson: these have re-pricing value; they stay)
"""
import os, re, sys, collections

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT = os.path.join(ROOT, "docs", "knobs.md")
SRC_DIRS = ["src", "cli"]
INSTR_PAT = re.compile(
    r"(STAT|PROF|LOG|TRACE|DUMP|LED|CENSUS|PROBE|ORACLE|_REC$|_PLAY$|UNSAFE|"
    r"ETSTAT|SPLIT$|NOISE|DBG)")
# getenv("N265_X"), the tunable helper abr_tunable("N265_X", default) and
# n265_env("N265_X") - the preset-aware read every encoder site uses since
# 2026-09-10 - are all knob reads; abr_tunable's default is its second
# argument. n265_env returns the environment first and the selected
# preset's table only where the variable is unset, so the default parsed
# out of the C line below is still the shipped (medium) default.
GETENV = re.compile(r'(?:getenv|abr_tunable|n265_env)\("(N265_[A-Z0-9_]+)"')
TUNDEF = re.compile(r'abr_tunable\("N265_[A-Z0-9_]+",\s*(-?[0-9.]+)')
# the common default idioms:  v = e ? atoi(e) : D;   and the guarded form
# v = e ? (atoi(e) ? 1 : 0) : D;  -- in both, D is the value after the LAST
# colon on the statement, so take the final numeric-after-colon on the line.
DEFVAL = re.compile(r':\s*(-?[0-9.]+)\s*;')

# THE FOUR FORMS THE COLON IDIOM ABOVE CANNOT READ (2026-09-14). The audit
# in docs/x265-feature-audit.md read sixty defaults off their call sites by
# hand because the census printed `?` for them, so the forms are named here
# and read mechanically instead:
#   T  ternary on the env VALUE:  v ? (float)atof(v) : 0.7f
#      -- the f suffix and the missing semicolon both defeat DEFVAL, and the
#      literal can close on ')' or ',' instead. The true branch must actually
#      CONSUME the env string (atof/atoi/strtod), which is what separates a
#      value read from a presence test written as a ternary
#      (`n265_env("N265_NO_TMVP") ? 0 : 1` is the flag's value, not the
#      knob's default).
#   G  guarded assignment:  double aqc = 3.0; ... if (v) aqc = atof(v);
#      -- the default is the target's own initialization, above the read or
#      earlier on the same line. The nearest assignment wins and a
#      non-literal one stops the walk (that is a symbolic default: an
#      override's job).
#   M  a macro literal after the colon:  v = s ? atoi(s) : MAX_MERGE
#   P  presence: the read is a boolean (`!= NULL`, an if/&&/|| operand), so
#      unset is OFF and any value arms it -- and its string cousin, where
#      the value names a file (fopen / `v && *v`), printed as `unset`.
NUMLIT = r'-?(?:\d+\.?\d*|\.\d+)'
VALUE_READ = re.compile(r'\bato[fi]\b|\bstrto[dl]\b')
MACRO_DEF = re.compile(r'^\s*#\s*define\s+([A-Z][A-Z0-9_]*)\s+\(?(-?[0-9]+)\)?\s*(?:/|$)')
# "was default off" / "old default" are HISTORY, not claims about today
OFFCLAIM = re.compile(r'(?<!was )(?<!old )[Dd]efault(?:s)?\s+(?:is\s+|stays\s+)?'
                      r'(OFF|off|0(?![.\d])|ON|on)\b')

# hand-tier corrections where the heuristic is wrong; extend as needed
# defaults the DEFVAL idiom cannot read: a value taken from another knob, a
# presence-only reader, a string enum, or a literal that is not the last
# number on the line.
DEFAULT_OVERRIDES = {
    # phase 14 item 59: on unless 0, read only on a PQ or HLG medium
    "N265_HDR_FULL": "1 on PQ and HLG at medium",
    # `if (v && !g_cb_qp_off) e->cb_qp_off = atoi(v)`: the flag wins, and
    # 0 is what both doors leave shut
    "N265_CBQPOFFS": "0", "N265_CRQPOFFS": "0",
    "N265_TP_CORR": "= tp_plan_on",      # follows the 2-pass plan switch
    "N265_TP_DBG": "presence",           # any value arms it
    "N265_SKIP_ORACLE_AT": "pre",        # pre | post | postr
    "N265_NTP_SPIN": "25",               # microseconds x 1000 inside
    "N265_HW": "off",                    # off | auto | videotoolbox (a string)
    # item vbrspeed: 7 under --bitrate with a VBV above one worker, 0 at
    # one worker and in every other mode
    "N265_VBR_LAG": "7 (VBR, threads > 1)",
    "N265_VBR_LAG_WARM": "2 * (lag + 1)",   # frames at lag 0 first
    # phase 7 follow-up 1: on with ABR, and under CRF on every frame the
    # cu-tree frame term is live on (so off under CQP, off under either
    # escape, and off on a flat-typed frame, where the term is zero)
    "N265_QP_FRAC": "= --bitrate or CRF+frame term",
    "N265_QP_FRAC_FLAT": "0",            # refused arm: the fraction on
                                         # a flat-typed frame too
    "N265_AQ_FLOOR": "4",                # `if (v) vfloor = atof(v)`: the
                                         # literal is not last on the line
    # phase 7 item 2 M2: unset is the shipped margin itself, through a
    # sentinel the DEFVAL idiom cannot read
    "N265_DECIMATE_GRAD": "= N265_DECIMATE",
    # phase 7 M1': the float literal `0.0f` is not what the numeric idiom
    # picks up; the signal's own `: 1` parses correctly and ships
    "N265_AQ_FLAT_SIGN": "0",
    # phase 10 item 3: the content gate on that sign. Unset is a threshold
    # no frame's outline share can reach, i.e. the ungated arm - a sentinel
    # the DEFVAL idiom cannot read.
    "N265_AQ_FLAT_THR": "= ungated",
    # phase 7 step 2. k_f ships at 1.0 (the literal carries an f suffix);
    # its flat-frame twin follows k_b, which is today's form; and the two
    # cascade knobs take "r,l" or "auto", not a number.
    "N265_CT_FRAMEW": "1.0",
    "N265_CT_FRAMEW_FLAT": "= N265_CT_QPW",
    "N265_CT_FRAME_CLAMP": "0",          # unbounded; the literal carries an f
    "N265_CT_FRAME_UP": "0",             # no raise; likewise
    # phase 7 follow-up 2: the content gate that releases that bound on a
    # frame the previous picture does not already predict. Also an f literal.
    "N265_CT_UP_GATE": "0.24",
    "N265_PB_CASCADE": "auto",
    "N265_PB_CASCADE_FLAT": "auto",
    "N265_ASM_OFF": "unset",             # a class-name list, not a number
    # the rect/AMP pair: guarded readers of the form
    # `!(v ? atoi(v) != 0 : D)`, where D is not the last number on the
    # line. AMP is OFF by default since the phase 6b placement and a
    # census that renders it "?" hides the biggest of the new defaults.
    "N265_NO_RECT": "1",                 # rect off since step 4, except
                                         # on a flat-typed frame - the
                                         # exception is N265_RECT_FLAT
    "N265_NO_AMP": "1",                  # AMP off (phase 6b)
    # phase 12 item 2, both read as `v ? ... : D` through a separate `if (v)`
    # the numeric idiom cannot see
    "N265_VBV_SAFE": "0.85",             # the clamp's safety fraction
    "N265_VBV_LOOKAHEAD": "1",           # the window's reservation, on
    "N265_VBV_RESERVE": "0.2",           # the floor under that reservation
    # symbolic defaults: the unset value is ANOTHER knob's, so no literal
    # exists at the call site to read (2026-09-14).
    "N265_2P_QCOMP": "= N265_ABR_QCOMP",  # `double qcomp = e->rc.qcomp;`
    # phase 14 item 29: the VBR profile. Under --bitrate with a VBV the two
    # take their own defaults; plain ABR keeps the old ones.
    "N265_ABR_FBUP": "= N265_VBR_FBUP under capped VBR (item 37), 1.5 under another VBV, else = N265_ABR_FBMAX",
    "N265_VBR_FBUP": "= N265_ABR_FBMAX (capped VBR only)",
    "N265_ABR_IOFF_CT": "1.0 under a VBV without N265_VBR_CTKEEP, else 0",
    # phase 14 item 37: capped VBR (maxrate above the target, not
    # --strict-cbr) rides the CRF path with a slow level
    "N265_VBR_FBW": "2.0 under capped VBR, else 0",
    "N265_VBR_OVW": "0.75 under capped VBR, else 0",
    "N265_VBR_CTKEEP": "1 under capped VBR, else 0",
    "N265_VBV_OWN": "3 under capped VBR, else 0",
    # phase 14 item 29 step 2: the brake and the flat-frame rule, ABR with
    # a VBV only; the cut prediction, ABR only.
    "N265_VBV_BRAKE": "1 under --bitrate with a VBV, else 0",
    "N265_ABR_FLATC": "17.0 under a VBV, else 0",
    "N265_VBV_SCPRED": "0.5 under --bitrate, else 1.0",
    # phase 14 item 47: capped CRF (--crf with a VBV) takes the brake, a
    # guard margin on the frame alone, and an uncalibrated brake ratio
    "N265_CCRF_BRAKE": "1 under capped CRF, else unread",
    "N265_CCRF_BRAKE_FILL": "0.8 under capped CRF, else unread",
    "N265_CCRF_BRK_R0": "1.25",
    "N265_CCRF_GUARD": "1.3 under capped CRF, else 0",
    "N265_CCRF_SKIP": "1 under capped CRF, else 0",
    "N265_CCRF_SKIP_FILL": "0.8", "N265_CCRF_SKIP_SATQ": "51.0",
    "N265_CCRF_SKIP_WARM": "4.0",
    # `int k = v ? atoi(v) : (e->bframes >= 2 ? 6 : 4);`, nine lines under
    # the read: two defaults, and which one ships depends on a flag
    "N265_MAX_FRAME_WORK": "6 with --bframes >= 2, else 4",
    # phase 14 item 25: computed at open from the picture and the thread
    # count, and the extra slots follow the frames in flight
    "N265_ROW_YIELD": "5000 where 2 * min(rows, (cols + 1) / 2) > threads > 1, else 0",
    "N265_TREE_SLOTS": "K - 6 (with the switched HD or UHD tree only)",
    "N265_TASK_TRACE": "unset",
    # phase 14 item 42: --gop-parallel's segment length and reader window
    "N265_GOP_LEN": "= --keyint",
    "N265_GOP_WINDOW": "(N + 1.5) x N265_GOP_LEN",
    "N265_GOP_CUTS": "1", "N265_GOP_VBV_IN": "0.5",
    "N265_GOP_VBV_OUT": "0.6", "N265_GOP_TRACE": "presence",
    # phase 14 item 45: each instance seeks its own records on a file
    "N265_GOP_SEEK": "1", "N265_GOP_RELIEF": "1",
    "N265_GOP_K": "(--threads / N) + 1, at least 3",
}

OVERRIDES = {
    "N265_HDR_FULL": "default",          # item 59: the env is the escape
    # phase 14 item 42: moot unless --gop-parallel armed the mode, so they
    # are that opt-in mode's parameters and not shipped defaults
    "N265_GOP_LEN": "arm", "N265_GOP_CUTS": "arm", "N265_GOP_WINDOW": "arm",
    "N265_GOP_VBV_IN": "arm", "N265_GOP_VBV_OUT": "arm", "N265_GOP_SEEK": "arm",
    "N265_GOP_RELIEF": "arm", "N265_GOP_K": "arm",
    # item vbrspeed: shipped above one worker under VBR; the env is the
    # escape (0 is main's stream).
    "N265_VBR_LAG": "default", "N265_VBR_LAG_CAL": "default",
    "N265_VBR_LAG_PEND": "default", "N265_VBR_LAG_WARM": "default",
    "N265_VBR_LAG_LOW": "default",
    # item 33: shipped (1.2 s); the env 0 is the escape. The guard is an
    # arm, off.
    "N265_VBR_LAG_BUFS": "default",
    # item 37: shipped under capped VBR; the env 0 is the escape. The
    # post-intra lag-0 rule is an arm, off.
    "N265_VBR_FBW": "default", "N265_VBR_OVW": "default",
    "N265_VBR_CTKEEP": "default", "N265_VBV_OWN": "default",
    "N265_VBR_FBUP": "default",
    # item 47: shipped under capped CRF; N265_CCRF_BRAKE=0
    # N265_CCRF_GUARD=1 is main byte for byte.
    "N265_CCRF_BRAKE": "default", "N265_CCRF_BRAKE_FILL": "default",
    "N265_CCRF_BRK_R0": "default", "N265_CCRF_GUARD": "default",
    "N265_CCRF_SKIP": "default",
    "N265_CCRF_SKIP_FILL": "default", "N265_CCRF_SKIP_SATQ": "default",
    "N265_CCRF_SKIP_WARM": "default",
    # phase 14 item 30: shipped by medium's table (compiled-in 0, so the
    # other presets don't move); the env is the escape.
    "N265_MERGE_PICK_RATE": "default",
    # phase 14 item 36: the hexagon walk, shipped by medium's table
    # (compiled-in 0, so the other presets don't move); the env is the
    # escape.
    "N265_ME_PATTERN": "default",
    # phase 14 item 40: the coarse intra rank (medium) and the HD/UHD 8x8
    # inter split, shipped by medium's tables (compiled-in 0); the env is
    # the escape. N265_TT_SKIP_INTRA and N265_SUBPEL_AXIS are refused arms.
    "N265_INTRA_FAST": "default", "N265_TT_SKIP_INTER": "default",
    "N265_INTRA_FAST_W": "default",
    "N265_TT_SKIP_INTRA": "arm", "N265_SUBPEL_AXIS": "arm",
    "N265_INTRA_FAST_QP": "arm", "N265_INTRA_FAST_STEP": "arm",
    "N265_INTRA_FAST_FLAT": "arm", "N265_TT_SKIP_QP": "arm",
    # phase 14 item 25: the schedule of the switched tree. Shipped values
    # whose env is the escape; the task trace is compiled only under
    # -DN265_TTRACE and is an instrument.
    "N265_TREE_K": "default", "N265_TREE_SLOTS": "default",
    "N265_ROW_YIELD": "default", "N265_CLAIM_PRIO": "default",
    "N265_TASK_TRACE": "instrument",
    # phase 14 item 11b: cataloged in docs/instruments.md because the CRF
    # axis section has to say what the mapping IS, which the heuristic then
    # reads as "machinery". Three of them are the shipped mapping - the
    # duration exponent, the rate it is zero at, and the level's memory -
    # and the legacy switch is the escape, which is an arm.
    "N265_CRF_DUR": "default", "N265_CRF_DUR_FPS": "default",
    "N265_CRF_DC_MEM": "default", "N265_CRF_LEGACY": "arm",
    # phase 14 item 6: cataloged in docs/instruments.md because --mcstf's
    # own section has to say what its constants are, which the heuristic
    # then reads as "machinery". They are not: each one is the SHIPPED
    # value of a filter constant and the env is its escape. Only the
    # trace is an instrument.
    "N265_MCSTF_SIGMA": "default", "N265_MCSTF_CLEAN": "default",
    "N265_MCSTF_PERIOD": "default",
    # phase 14 item 31: the per-block weighting's constants are --mcstf's
    # shipped values (the env is the escape), like the three above. The
    # environment door onto the filter and its range is an arm (off by
    # default at every preset), and so is the coupling (a QP bonus and a
    # cu-tree multiplier on filtered and scene-cut anchors): built, inert
    # at its default, not yet screened.
    "N265_MCSTF_MODEL": "default", "N265_MCSTF_SHARP": "default",
    "N265_MCSTF_SMIN": "default", "N265_MCSTF_S0": "default",
    "N265_MCSTF_PICCAP": "default", "N265_MCSTF_RHO0": "default",
    "N265_MCSTF_RHO1": "default", "N265_MCSTF_SUBPEL": "default",
    "N265_MCSTF_CUT": "default", "N265_MCSTF_STR": "default",
    "N265_MCSTF_QPREF": "default", "N265_MCSTF_QPSLOPE": "default",
    "N265_MCSTF": "arm", "N265_MCSTF_RANGE": "arm",
    "N265_MCSTF_QPB": "arm", "N265_MCSTF_QPB_I": "arm",
    "N265_MCSTF_CTF": "arm", "N265_MCSTF_CTSC": "arm",
    # the block's mean offset taken into the prediction: fixes fade_cif,
    # costs xfade_720p 0.11 more; refused, kept as an arm
    "N265_MCSTF_DC": "arm",
    # phase 13 B1: the environment door onto --cbqpoffs / --crqpoffs. Both
    # are arms, not shipped defaults - 0 is the default at every preset and
    # the measurement that keeps it there is in docs/phase13.md.
    "N265_CBQPOFFS": "arm", "N265_CRQPOFFS": "arm",
    # phase 14 item 20: the four consumer refits. Each is cataloged in
    # docs/instruments.md because the item's account has to say what its
    # constant IS, which the heuristic then reads as "machinery". They are
    # not: each one is the SHIPPED value of a decision the lookahead makes
    # - the b-adapt statistic's saturation and its motion gate, cu-tree's
    # price for the motion it propagates along, and the scenecut's spike
    # guard - and the env is the escape to the pre-item-20 rule.
    "N265_BADAPT_MVCAP": "default", "N265_BADAPT_MVGATE": "default",
    "N265_CT_MVRATE": "default", "N265_SC_SPIKE": "default",
    # phase 14 item 21, and the same case as the four above: the scenecut's
    # failure-share trigger with its own level guard, and the ABR model's
    # price for the motion a prediction is fetched with. Each is the
    # SHIPPED value of a decision, cataloged in docs/instruments.md
    # because the item has to say what the constant is.
    "N265_SC_FAILW": "default", "N265_SC_FAILW_SPIKE": "default",
    "N265_RC_MVRATE": "default",
    # ...and the two instruments the item is measured with, which resolve
    # unset and would otherwise read as arms
    "N265_LA_MVSTAT": "instrument", "N265_SC_FORCE": "instrument",
    # phase 12 item 2. Both are moot unless --vbv-maxrate armed the mode,
    # but inside it they are the SHIPPED clamp and not arms waiting to be
    # re-priced: the safety fraction is what buys the zero-underflow gate
    # and the reservation is what pays for an intra frame before it lands.
    "N265_VBV_SAFE": "default", "N265_VBV_LOOKAHEAD": "default",
    "N265_VBV_RESERVE": "default", "N265_VBV_DECAY": "default",
    "N265_MBT_REC": "instrument", "N265_MBT_PLAY": "instrument",
    "N265_REFENC_CACHE": "instrument",
    # both exist so a thread-count sweep never needs a rebuild (encoder.c
    # ~3339): a pin and a ceiling over the resolved auto budget, not a
    # default-off feature waiting to be re-priced
    "N265_AUTO_THREADS": "instrument", "N265_AUTO_THREADS_MAX": "instrument",
    # moot unless N265_CHROMA_W is set, so it is that arm's parameter and
    # not a shipped default of its own
    "N265_CHROMA_W_PCT": "arm",
    # phase 13 A1: cataloged in docs/instruments.md because it has a
    # result, but it is a default-off MODE refused on the band and kept
    # for re-pricing at the lower rungs - an arm, not machinery
    "N265_EST_MODE": "arm",
    # same shape: moot unless N265_CHROMA_PRED is set. Its 0 is the
    # luma-only CONTROL for that arm, which is what it exists for.
    "N265_CPRED_W": "arm",
    # -1 is "no forced picture", which reads nonzero to the heuristic. It is
    # the diagnostic that puts a non-IDR I at a chosen display index, so the
    # conformance vectors can carry the picture kind through both decoders
    # on a source with no scene cut in it.
    "N265_FORCE_NONIDR": "instrument",
    # the shot planner's three parameters (phase 7 item 6 step 2). Each
    # resolves nonzero with no env and every one of them is moot unless
    # --shot-window / N265_SHOT_WINDOW armed the mode, so they are that
    # opt-in arm's parameters and not shipped defaults of their own -
    # N265_CHROMA_W_PCT's case exactly.
    "N265_SHOT_K": "arm", "N265_SHOT_CLAMP": "arm", "N265_SHOT_EMA": "arm",
    # phase 10 item 2: the bound on the planned intra QP offset. It resolves
    # nonzero and is moot unless N265_ABR_IOFF / N265_ABR_IOFF_CT armed the
    # offset, so it is that refused arm's parameter - N265_CHROMA_W_PCT's
    # case exactly.
    "N265_ABR_IOFF_MAX": "arm",
    # item 32: the size model's constants (an arm, off) and the refused
    # opening's
    "N265_VBR_SZM_Z": "arm",
    "N265_VBR_SZM_ZLAG": "arm", "N265_VBR_SZM_LAM": "arm",
    "N265_VBR_SZM_STATIC": "arm", "N265_VBR_SZM_SIMPLE": "arm",
    "N265_VBR_SZM_GMIN": "arm",
    "N265_VBR_OPEN_AI": "arm",
    "N265_VBR_OPEN_AP": "arm",
    "N265_VBR_OPEN_ABR": "arm",
    "N265_VBR_OPEN_AB": "arm",
    "N265_VBR_OPEN_BI": "arm",
    "N265_VBR_OPEN_BP": "arm",
    "N265_VBR_OPEN_BBR": "arm",
    "N265_VBR_OPEN_BB": "arm",
    "N265_VBR_OPEN_G": "arm",
    # phase 10 item 1: three forms of lambda x mvd on the subpel vector, all
    # priced and all refused (the re-rank on the clock, the in-ladder form on
    # bus's CRF cell). Default 0 and kept for re-pricing, which is what an
    # arm is; the TRACE beside it is the instrument.
    "N265_SUBPEL_RATE": "arm",
    # phase 10 item 3: moot unless N265_AQ_FLAT_SIGN arms the sign, so it is
    # that arm's parameter - N265_CHROMA_W_PCT's case exactly.
    "N265_AQ_FLAT_THR": "arm",
    # phase 11 item 2: the subpel rung, refused on the band at all three
    # fire predicates and kept for re-pricing. N265_RUNG_GATE resolves
    # nonzero (-2, item 1b's shipped sign) but is moot unless the rung is
    # armed, so it is the arm's parameter and not a shipped default;
    # N265_RUNG_RERANK likewise.
    "N265_SUBME_RUNG": "arm",
    "N265_RUNG_GATE": "arm",
    "N265_RUNG_RERANK": "arm",
    # phase 11 item 4: the static commit and its conjunct mask. SC_CONJ
    # resolves nonzero, so the heuristic calls it a shipped default; it is
    # moot unless N265_STATIC_COMMIT is armed, which makes it the arm's
    # parameter and not a default of its own - the same correction
    # N265_RUNG_GATE takes above.
    "N265_STATIC_COMMIT": "arm",
    "N265_SC_CONJ": "arm",
    # the lattice IS the shipped default (1) and the env is its escape; it
    # is cataloged in docs/instruments.md because value 2 - the
    # commit-only form, phase 9 step 2 - is an arm with a census behind
    # it, and being named there is what the heuristic reads as
    # "instrument".
    "N265_RDOQ": "default",
    # b-adapt 3's degrade threshold, readable since 2026-09-14 (1.6) and so
    # no longer typed "arm" by a "?" default. It is moot unless --b-adapt 3
    # armed the priced run-length rule, which is a refused arm, so it is
    # that arm's parameter - N265_CHROMA_W_PCT's case exactly.
    "N265_BADAPT_DEG": "arm",
}

# knobs whose nearest default-claim is correct but beyond the detector:
#   SUBPEL         -1 is a "follow the preset" sentinel; the comment's
#                  "default 0 (square)" describes the resolved fallback
#   ABR_CGUARD     conditional default (ON iff the rf model is armed)
#   GPU_RANGE      the flagged "Default OFF" belongs to N265_SATDX4's block
#                  directly above (comment adjacency, not staleness)
#   PART_IMPORTANT sub-knob of the parked PART_EARLYTERM=2 mode; its numeric
#                  default is that mode's tuning constant, inert otherwise
#   PART_SLACK_X4  same block: the "PARKED, default OFF" verdict comment
#                  describes that mode, not this tuning constant (default 4)
CLAIM_OK = {"N265_SUBPEL", "N265_ABR_CGUARD", "N265_GPU_RANGE",
            "N265_PART_IMPORTANT", "N265_PART_SLACK_X4"}


def macro_table():
    """#define NAME <int> across the tree: form M's lookup."""
    out = {}
    for d in SRC_DIRS + ["include"]:
        root = os.path.join(ROOT, d)
        if not os.path.isdir(root):
            continue
        for base, _, files in os.walk(root):
            for f in files:
                if not f.endswith((".c", ".h")):
                    continue
                for ln in open(os.path.join(base, f), errors="replace"):
                    m = MACRO_DEF.match(ln)
                    if m:
                        out.setdefault(m.group(1), m.group(2))
    return out


def env_var(line, name, pos):
    """the C variable the env pointer lands in, if the site names one."""
    m = re.search(r'(\w+)\s*=\s*n265_env\("%s"\)' % re.escape(name),
                  line[:pos + len(name) + 24])
    return m.group(1) if m else None


def ternary_default(win, name, var, macros):
    """form T and form M: <cond> ? <reads the env> : <literal>."""
    conds = []
    if var:
        conds.append(r'\b%s\b' % re.escape(var))
    conds.append(r'n265_env\("%s"\)' % re.escape(name))
    pat = (r'\s*\?(?P<t>[^;]*?):\s*\(?\s*'
           r'(?P<d>' + NUMLIT + r'|[A-Z][A-Z0-9_]{2,})[fFuUlL]*\s*(?=[;,)}\]]|$)')
    for c in conds:
        for m in re.finditer(c + pat, win):
            if not VALUE_READ.search(m.group("t")):
                continue          # a presence test wearing a ternary
            d = m.group("d")
            if re.fullmatch(NUMLIT, d):
                return d
            if d in macros:
                return macros[d]
    return None


def guarded_default(lines, i, pos, var):
    """form G: `<type> t = LIT; ... if (v) t = atof(v);` - the default is the
    target's nearest initialization, which may sit earlier on the read's own
    line. A non-literal initialization stops the walk: that is a symbolic
    default and belongs in DEFAULT_OVERRIDES."""
    if not var:
        return None
    win = " ".join(lines[i:i + 3])
    m = re.search(r'if\s*\(\s*%s\s*\)\s*\{?\s*([A-Za-z_][\w.>-]*)\s*=\s*'
                  r'ato[fi]\s*\(\s*%s\s*\)' % (re.escape(var), re.escape(var)),
                  win)
    if not m:
        return None
    tgt = re.escape(m.group(1))
    texts = [lines[i][:pos]] + [lines[j] for j in range(i - 1, max(-1, i - 16), -1)]
    for text in texts:
        for mm in re.finditer(r'\b%s\s*=\s*([^;]+);' % tgt, text):
            rhs = mm.group(1).strip().rstrip("fFuUlL")
            return rhs if re.fullmatch(NUMLIT, rhs) else None
    return None


def presence_default(lines, i, name, var):
    """form P: unset is off. `unset` where the value names a file or a
    string, `presence` where the read is a plain boolean."""
    win = " ".join(lines[i:i + 3])
    if var:
        v = re.escape(var)
        if (re.search(r'fopen\s*\(\s*%s\b' % v, win)
                or re.search(r'\b%s\s*&&\s*\*\s*%s\b' % (v, v), win)
                or re.search(r'strcmp\s*\(\s*%s\b' % v, win)):
            return "unset"
    env = r'n265_env\("%s"\)' % re.escape(name)
    ln = lines[i]
    if (re.search(env + r'\s*(?:!=\s*NULL|\|\||&&|\?)', ln)
            or re.search(r'!\s*' + env, ln)
            or re.search(r'(?:if|while)\s*\([^;]*' + env, ln)
            or re.search(r'(?:\|\||&&)\s*' + env, ln)):
        return "presence"
    return None


def rank(d):
    return 0 if d == "?" else (1 if d in ("presence", "unset") else 2)


def parse_default(lines, i, name, pos, macros):
    ln = lines[i]
    tm = TUNDEF.search(ln)
    if tm:
        return tm.group(1)
    var = env_var(ln, name, pos)
    win = ln[pos:] + " " + " ".join(lines[i + 1:i + 3])
    d = ternary_default(win, name, var, macros)
    if d is None:
        d = guarded_default(lines, i, pos, var)
    if d is None:
        d = presence_default(lines, i, name, var)
    if d is None:                       # the original colon idiom, last
        dms = DEFVAL.findall(ln)
        if not dms and i + 1 < len(lines):
            dms = DEFVAL.findall(lines[i + 1])
        d = dms[-1] if dms else "?"
    return d


def scan():
    knobs = {}
    macros = macro_table()
    for d in SRC_DIRS:
        for base, _, files in os.walk(os.path.join(ROOT, d)):
            # relative to ROOT: a tree that itself lives under a .claude
            # worktree directory must still scan its own sources
            if ".claude" in os.path.relpath(base, ROOT).split(os.sep):
                continue
            for f in files:
                if not f.endswith((".c", ".h", ".m")):
                    continue
                path = os.path.join(base, f)
                rel = os.path.relpath(path, ROOT)
                lines = open(path, errors="replace").read().splitlines()
                for i, ln in enumerate(lines):
                    for m in GETENV.finditer(ln):     # two reads on one line are two knobs
                        name = m.group(1)
                        default = parse_default(lines, i, name, m.start(), macros)
                        default = DEFAULT_OVERRIDES.get(name, default)
                        # nearest default-claim comment above (stale check). The
                        # scan stops at the previous knob's getenv AND at any
                        # comment line naming a DIFFERENT knob, so a neighbor's
                        # "default OFF" is never charged to this one.
                        claim = None
                        for j in range(i - 1, max(-1, i - 40), -1):
                            others = [n for n in GETENV.findall(lines[j])
                                      if n != name]
                            named = re.findall(r"N265_[A-Z0-9_]+", lines[j])
                            if others or (named and name not in named):
                                break            # another knob's territory
                            cm = OFFCLAIM.search(lines[j])
                            if cm:
                                claim = (cm.group(1).upper(), j + 1)
                                break
                        e = knobs.setdefault(name, {"sites": [], "default": default,
                                                    "claim": claim})
                        e["sites"].append(f"{rel}:{i+1}")
                        # a site that reads a VALUE outranks one that only
                        # tests presence, which outranks no read at all
                        if rank(default) > rank(e["default"]):
                            e["default"] = default
    return knobs


def tier_of(name, default, instr_doc):
    if name in OVERRIDES:
        return OVERRIDES[name]
    if INSTR_PAT.search(name) or name in instr_doc:
        return "instrument"
    try:
        if float(default) != 0:
            return "default"
    except ValueError:
        pass
    return "arm"


def stale_claims(knobs):
    bad = []
    for name, e in sorted(knobs.items()):
        if name in CLAIM_OK or not e["claim"] or e["default"] == "?":
            continue
        claim, line = e["claim"]
        try:
            dv = float(e["default"])
        except ValueError:
            continue
        if claim in ("OFF", "0") and dv != 0:
            bad.append(f"{name}: comment near {e['sites'][0]} (line {line}) "
                       f"claims default {claim}, code default is {e['default']}")
        if claim == "ON" and dv == 0:
            bad.append(f"{name}: comment near {e['sites'][0]} (line {line}) "
                       f"claims default ON, code default is 0")
    return bad


def generate(knobs, instr_doc):
    tiers = collections.defaultdict(list)
    for name, e in sorted(knobs.items()):
        tiers[tier_of(name, e["default"], instr_doc)].append((name, e))
    with open(OUT, "w") as f:
        f.write("# The N265_* knob census (generated -- edit "
                "scripts/knob_census.py OVERRIDES, not this file)\n\n"
                "Regenerate: `python3 scripts/knob_census.py`. CI check: "
                "`--check`.\n\nTier meanings: **default** = shipped behavior, "
                "the env is its escape; **instrument** = measurement machinery "
                "(cataloged in docs/instruments.md when it has produced a "
                "result); **arm** = default-off gate or kept refuted arm -- "
                "kept deliberately because a refuted arm retains its "
                "re-pricing value.\n\nIn the default column, **presence** "
                "means the reader is a boolean: unset is off and any value "
                "arms it. **unset** means the value names a file or a "
                "string and unset is off. `= N265_X` means the default is "
                "another knob's resolved value.\n")
        for tier in ("default", "instrument", "arm"):
            f.write(f"\n## {tier} ({len(tiers[tier])})\n\n")
            f.write("| knob | default | reader |\n|---|---|---|\n")
            for name, e in tiers[tier]:
                f.write(f"| `{name}` | {e['default']} | {e['sites'][0]} |\n")
    print(f"wrote {OUT}: " + ", ".join(
        f"{t} {len(tiers[t])}" for t in ("default", "instrument", "arm")))


def main():
    instr_doc = set()
    ipath = os.path.join(ROOT, "docs", "instruments.md")
    if os.path.exists(ipath):
        instr_doc = set(re.findall(r"N265_[A-Z0-9_]+", open(ipath).read()))
    knobs = scan()
    bad = stale_claims(knobs)
    if "--help" in sys.argv or "-h" in sys.argv:
        print(__doc__); sys.exit(0)
    if "--check" in sys.argv:
        rc = 0
        if os.path.exists(OUT):
            cat = set(re.findall(r"`(N265_[A-Z0-9_]+)`", open(OUT).read()))
            dark = sorted(set(knobs) - cat)
            ghost = sorted(cat - set(knobs))
            if dark:
                print("UNCATALOGUED (regenerate docs/knobs.md):", *dark); rc = 1
            if ghost:
                print("CATALOG GHOSTS (reader removed):", *ghost); rc = 1
        else:
            print("docs/knobs.md missing"); rc = 1
        for b in bad:
            print("STALE DEFAULT COMMENT:", b); rc = 1
        sys.exit(rc)
    generate(knobs, instr_doc)
    for b in bad:
        print("STALE DEFAULT COMMENT:", b)


if __name__ == "__main__":
    main()
