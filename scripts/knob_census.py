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
# "was default off" / "old default" are HISTORY, not claims about today
OFFCLAIM = re.compile(r'(?<!was )(?<!old )[Dd]efault(?:s)?\s+(?:is\s+|stays\s+)?'
                      r'(OFF|off|0(?![.\d])|ON|on)\b')

# hand-tier corrections where the heuristic is wrong; extend as needed
# defaults the DEFVAL idiom cannot read: a value taken from another knob, a
# presence-only reader, a string enum, or a literal that is not the last
# number on the line.
DEFAULT_OVERRIDES = {
    "N265_TP_CORR": "= tp_plan_on",      # follows the 2-pass plan switch
    "N265_TP_DBG": "presence",           # any value arms it
    "N265_SKIP_ORACLE_AT": "pre",        # pre | post | postr
    "N265_NTP_SPIN": "25",               # microseconds x 1000 inside
    "N265_HW": "off",                    # off | auto | videotoolbox (a string)
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
}

OVERRIDES = {
    "N265_MBT_REC": "instrument", "N265_MBT_PLAY": "instrument",
    "N265_REFENC_CACHE": "instrument",
    # both exist so a thread-count sweep never needs a rebuild (encoder.c
    # ~3339): a pin and a ceiling over the resolved auto budget, not a
    # default-off feature waiting to be re-priced
    "N265_AUTO_THREADS": "instrument", "N265_AUTO_THREADS_MAX": "instrument",
    # moot unless N265_CHROMA_W is set, so it is that arm's parameter and
    # not a shipped default of its own
    "N265_CHROMA_W_PCT": "arm",
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
    # phase 10 item 1: three forms of lambda x mvd on the subpel vector, all
    # priced and all refused (the re-rank on the clock, the in-ladder form on
    # bus's CRF cell). Default 0 and kept for re-pricing, which is what an
    # arm is; the TRACE beside it is the instrument.
    "N265_SUBPEL_RATE": "arm",
    # phase 10 item 3: moot unless N265_AQ_FLAT_SIGN arms the sign, so it is
    # that arm's parameter - N265_CHROMA_W_PCT's case exactly.
    "N265_AQ_FLAT_THR": "arm",
    # the lattice IS the shipped default (1) and the env is its escape; it
    # is catalogued in docs/instruments.md because value 2 - the
    # commit-only form, phase 9 step 2 - is an arm with a census behind
    # it, and being named there is what the heuristic reads as
    # "instrument".
    "N265_RDOQ": "default",
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


def scan():
    knobs = {}
    for d in SRC_DIRS:
        for base, _, files in os.walk(os.path.join(ROOT, d)):
            if ".claude" in base:
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
                        dms = DEFVAL.findall(ln)
                        if not dms and i + 1 < len(lines):
                            dms = DEFVAL.findall(lines[i + 1])
                        default = dms[-1] if dms else "?"
                        tm = TUNDEF.search(ln)
                        if tm:
                            default = tm.group(1)
                        default = DEFAULT_OVERRIDES.get(name, default)
                        # nearest default-claim comment above (stale check). The
                        # scan stops at the previous knob's getenv AND at any
                        # comment line naming a DIFFERENT knob, so a neighbour's
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
                        if e["default"] == "?" and default != "?":
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
                "`--check`.\n\nTier meanings: **default** = shipped behaviour, "
                "the env is its escape; **instrument** = measurement machinery "
                "(catalogued in docs/instruments.md when it has produced a "
                "result); **arm** = default-off gate or kept refuted arm -- "
                "kept deliberately because a refuted arm retains its "
                "re-pricing value.\n")
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
