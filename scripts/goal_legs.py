#!/usr/bin/env python3
# Copyright (c) 2026, the yah265 authors
# SPDX-License-Identifier: GPL-2.0-or-later
"""The plan's goal legs, read at ONE pinned MATCHED-SIZE operating point.

docs/plan.md "Goals and legs" defines three goals against x265 4.3
`--preset medium`, each read at one matched operating point and each
passing five legs - these four, and the fifth below them:

    median speed     <= 1.00x of x265's wall
    worst-clip speed  < 1.15x, the clip NAMED
    quality           not more than 0.5 dVMAF-NEG BELOW x265, at equal bytes
    compression       not more than 1.0% LARGER than x265, at equal quality

The last two are ONE-SIDED and each is read at its own matched axis; see
"the legs" below for why both of those words are load-bearing.

A SIXTH reading became the fifth LEG on 2026-09-14 (the owner, on phase 13
B4): PSNR-Y, ours minus x265's in dB at OUR bytes, read the same one-sided
way -

    pixel accuracy   not more than 1.0 dB BELOW x265, at equal bytes

It is a FLOOR and not a decision metric. VMAF-NEG stays the metric the
campaign decides on; this one exists so that a VMAF-NEG gain can never be
bought by letting measurable pixel accuracy slide, which is the one
failure mode the band cannot see (B4: nine of fifteen clips have the two
metrics pointing opposite ways). 0 of 15 fail the 1.0 dB bar today. The
0.5 dB line is recorded as DEBT beside it rather than gated, because
nothing in the tree closes it today: sita_flat_720p -0.87 and bus_cif
-0.72, the two clips B4 says to believe the dB metrics on. SSIM-Y is
printed beside it with NO bar - it ranks the same clips with more
separation, and it costs a decode the memo may not have, so a point that
does not carry it prints `-` rather than paying for one.

A reading that is NOT a leg is printed and recorded beside all of them
(phase 13 B0): chroma PSNR per plane, ours minus x265's in dB at OUR
bytes, with the count of clips more than 0.3 dB below on EITHER plane.
VMAF-NEG is luma-only, so the speed and quality legs cannot see chroma at
all; this is the number the chroma items of phase 13 sec B ship against.
No goal passes or fails on it - the owner decides when it becomes a leg.

| goal | tier                    | threads |
|------|-------------------------|---------|
|  1   | pure C both sides       | 1       |
|  2   | pure C both sides       | auto    |
|  3   | as shipped (SIMD on)    | auto    |

No other script in the tree prints those legs. `board.py` times ours at
`--qp 27 --aq 0 --cutree 0` against x265 at `--crf 28`, which is 1.4-1.6x
the bytes on our side (foreman 132 009 against 95 008; mobile 445 154
against 274 137), so every multiple on the chart has us coding 40-60%
more bits than the encoder we are timed against. The dSIZE leg exists
precisely to forbid that trade in the other direction, so the goals
cannot be read off the board at all. This is the instrument that reads
them.

**Why matched SIZE and not a matched CRF number.** Our CRF axis is not
x265's and the offset is a clip-class property, not a constant (2 to 4
points below theirs on CIF, within one step on HD, ABOVE it on park_joy;
docs/board-operating-point-pin.md). There is no single CRF that names the
same point on both encoders, so the point has to be SOLVED per clip: both
sides are bisected onto one byte target - the band ladder's rung 2
(scripts/ladders.json), the same target `bd_vs_x265.py` uses - by
`bd_at_rate.py`, whose memo is shared, so a clip the band has already
read costs no encode here.

Usage (environment, the way the band tools take their arguments):

    PRESET=medium python3 scripts/goal_legs.py            # all 15 clips
    PRESET=fast CLIPS=foreman_cif,park_joy_720p python3 scripts/goal_legs.py
    GOAL=2 python3 scripts/goal_legs.py     # the pure-C leg at auto threads
    PRESET=faster X265PRESET=faster python3 scripts/goal_legs.py

    PRESET  medium | fast | slow ...  (default medium: the plan's goal preset)
    X265PRESET  the x265 preset the legs are read AGAINST (default medium,
                the plan's reference). Phase 13 C1: a rung of our ladder is
                placed against the SAME-NAMED x265 rung, because that is
                the comparison a user makes.
    GOAL    1 | 2 | 3                 (default 3)
    CLIPS   comma list                (default: every clip with a ladder)
    RUNG    1..4                      (default 2: which ladder rung is the pin)
    FRAMES  120       NRUNS  3        JOBS  3 (clips at a time, SOLVE only)
    OURFLAGS  extra flags for OUR side only, solved AND timed (default none)
    N265 / X265 / BAND_N265 / BD_CACHE
    LOADMAX 3.0       LOADWAIT 600    (the quiet-box bar, and how long to
                                       wait for the box to reach it)

Flags: --help, --no-csv (do not append to docs/data/goal-legs.csv),
--no-time (solve and score only; the wall columns stay empty),
--from-csv (re-print a run's tables and legs out of the CSV, reading
nothing else: no encode, no metric, no stopwatch, so a row can be re-read
while the box belongs to someone else). Anything else is refused, the way
board.py refuses it.

**The arm is a column.** A row is keyed on the commit, so a run under
`N265_DUP_FAST=1` used to be indistinguishable from the commit's default.
The last column, `arm`, is every `N265_*` in the environment, sorted
`K=V` joined by spaces and empty for the default; `--from-csv` re-prints
the block for the arm IT is invoked under, and rows written before the
column existed carry "", which is the default's own value.

**`X265PRESET` is the reference's name, not an arm of ours** (phase 13
C1). The ladder is placed rung against same-named rung, so the reference
side of every leg has to be nameable; `medium` is the default and is
spelled into the template exactly where `--preset medium` always stood,
so the default's command string - and therefore `bd_at_rate`'s encode
memo key, which IS that string - is character for character the one every
existing row was solved under. The key gains the preset only when the
preset is not medium, which is the only case where it must. It rides its
own CSV column rather than `arm`: `arm` is what WE ran, and this is who
we ran against.

**`OURFLAGS` is an arm too, and it has to be.** A knob that ships as a
CLI FLAG cannot be set through the environment, so an arm like
`--rdoq-level 0` would otherwise be unreachable here - and the reference
ladder must not move with it, which is why the string goes on OUR side
only, into the solve template and the timed command alike. It lands in
the `arm` column as `OURFLAGS=<string>`, so a flag arm and an env arm are
told apart the same way two env arms are, and a default run still writes
the empty arm it always wrote.

**Two binaries, on purpose.** The wall is read on `build-rel/n265`
(`-Dbuildtype=release`, worth 2-3%; `make build-rel`) and the SOLVE runs
the band's own `build/n265` so its memo keys match the band's and a clip
the board has read costs no encode. That is only sound while the two
trees are byte-identical, so it is not assumed: every clip re-encodes its
solved point with the timed binary and `cmp`s it against the solved
bitstream before a stopwatch is started. `N265` overrides the timed
binary, `BAND_N265` the solving one; setting `N265` alone points both at
it.

Timing (docs/instruments.md "Timing rules", docs/phase6b-economy.md
"Method"): input warmed on both sides, output to /dev/null, the two arms
interleaved with the order reversed on alternate rounds, median of NRUNS,
and a quiet box - the run refuses to start a timed leg above LOADMAX.

What each column is read from, because they are not all the same encode:
the CRF, byte and VMAF-NEG columns come from the band's single-threaded
solve (a bitstream property, identical at any thread count on our side by
the determinism gate); `dvmaf_eq_bytes` and `dsize_eq_vmaf_pct` come from
that solve plus the reference's cached ladder and cost nothing; the wall
columns come from each leg's own thread configuration. x265's bitstream
does move with `--frame-threads`, so at the auto legs its wall is its own
default configuration's at the SOLVED CRF number, which is the pinned
operating point.

**The solve is not free on a fresh worktree.** `bd_at_rate`'s encode memo
is keyed on a hash of the binary, and two builds of the same source are
different files, so only the metric half of the memo (keyed on the
bitstream) survives a rebuild. On the board's own tree a solved clip is
free; on a new one the bisection re-encodes about six points per clip and
the VMAF is still served.

**Instructions retired ride beside every wall.** Each timed run goes
through `boxlib.measure` (`/usr/bin/time -l`), and the median count per
arm lands in `our_instr` / `x265_instr`, printed as `instr x`. Load
barely moves them, so a row read on a busy box still says whether ours
does more or less work. INSTR=0 times with the bare stopwatch instead,
the way every row before the column was timed; the wrapper adds one
fork of `/usr/bin/time` to both arms alike. The timed leg holds the
timing lock (docs/instruments.md), and the solve waits while somebody
else's timed leg holds it.
"""
import csv
import datetime
import json
import math
import os
import re
import shlex
import shutil
import statistics
import subprocess
import sys
import tempfile
import time
from concurrent.futures import ThreadPoolExecutor

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "scripts"))
import bd_at_rate as bdr                                  # noqa: E402
import bdcompare as _bd                                   # noqa: E402
import boxlib                                             # noqa: E402

_bd.SUBSAMPLE = 1
# bd_at_rate's memo key is the command string with {src} filled, and the
# band fills it with a path RELATIVE to the tree root (`bd_vs_x265.py`
# runs its children with cwd=ROOT). An absolute path here would be a
# different key for the same encode and would miss every entry the board
# has already paid for, so this instrument stands where the band stands.
os.chdir(ROOT)

PRESET = os.environ.get("PRESET", "medium")
# C1: which x265 preset the legs are read against. medium is the plan's
# reference and the default, and the default must be spelled into the
# reference template exactly where the literal always was, or every memo
# entry and every CSV row written before this existed becomes unreachable.
X265PRESET = os.environ.get("X265PRESET", "medium")
GOAL = int(os.environ.get("GOAL", "3"))
FRAMES = int(os.environ.get("FRAMES", "120"))
NRUNS = int(os.environ.get("NRUNS", "3"))
JOBS = int(os.environ.get("JOBS", "3"))
RUNG = int(os.environ.get("RUNG", "2"))
# Extra flags for OUR encoder, in both the solve and the clock. Split the
# way a shell would - the templates are strings and the timed command is a
# list, so the two halves have to agree - and empty by default, which is
# every run before this existed.
OURFLAGS = os.environ.get("OURFLAGS", "").strip()
OURARGV = shlex.split(OURFLAGS)
SRCDIR = os.environ.get("SRCDIR", "tests/corpus")
X265 = os.environ.get("X265", "x265")
LOADMAX = float(os.environ.get("LOADMAX", "3.0"))
LOADWAIT = float(os.environ.get("LOADWAIT", "600"))
INSTR = os.environ.get("INSTR", "1") != "0"
CSV_PATH = os.environ.get("GOAL_LEGS_CSV",
                          os.path.join(ROOT, "docs/data/goal-legs.csv"))

# The ARM: every `N265_*` in the invoking environment, sorted, `K=V`
# joined by spaces, empty for the default. A row is keyed on the commit,
# and an arm is a knob set from OUTSIDE the binary, so without this
# column `N265_DUP_FAST=1 goal_legs.py` and a plain run at the same
# commit write rows that cannot be told apart and read as the commit's
# default. Snapshotted before anything here builds an environment (the
# per-goal `N265_ASM_OFF` goes into a COPY, and the `tier` column already
# names it), so this is exactly what the caller set. `N265` and
# `BAND_N265` are binary paths rather than knobs and do not match the
# prefix.
ARM = " ".join([f"{k}={v}" for k, v in sorted(os.environ.items())
                if k.startswith("N265_")]
               + ([f"OURFLAGS={' '.join(OURARGV)}"] if OURARGV else []))

# Columns added after rows had already been written: `append_csv`
# migrates a file that predates them instead of refusing, and every
# reader takes them with a default.
OPTIONAL_COLUMNS = ("arm", "dcb_eq_bytes", "dcr_eq_bytes", "x265_preset",
                    "dy_eq_bytes", "dssimy_eq_bytes", "our_instr",
                    "x265_instr")

# B0's chroma reading: how far below x265 either plane may sit at equal
# bytes before the clip is named. 0.3 dB is the level the record calls
# viewer-visible on saturated content (docs/phase13-plan.md sec B). It is
# a REPORTING bar, not a leg: nothing here passes or fails on it.
BAR_DCHROMA = -0.3

# the wall is read on the release tree; the solve runs the band's binary
# so the memo keys are the band's (see the docstring)
_REL = os.path.join(ROOT, "build-rel/n265")
_DBG = os.path.join(ROOT, "build/n265")
TIMED_N265 = os.environ.get("N265", _REL if os.path.exists(_REL) else _DBG)
BAND_N265 = os.environ.get("BAND_N265", os.environ.get("N265", "build/n265"))

# goal -> (thread configuration, pure C on both sides)
GOALS = {1: ("t1", True), 2: ("auto", True), 3: ("auto", False)}

# The four legs, in the plan's own words, and ONE-SIDED: a goal is "as
# good, at less time", so ahead on quality and smaller on size pass with
# margin. DVMAF is a FLOOR on (ours - theirs); DSIZE a CEILING on how much
# larger ours may be.
BAR_MEDIAN = 1.00
BAR_WORST = 1.15
BAR_DVMAF = -0.5
BAR_DSIZE = 1.0
# The fifth leg (2026-09-14, the owner, on phase 13 B4): PSNR-Y at equal
# bytes, ours minus x265's, one-sided the way the quality leg is. A FLOOR,
# not a decision metric - the band still decides on VMAF-NEG. BAR is the
# ratchet the item gate fails on; DEBT is the tighter line the two clips
# B4 names are under today and is REPORTED, never gated, because a leg
# that fails on arrival stops being read.
BAR_DY = -1.0
DEBT_DY = -0.5


def src_of(clip):
    """Relative to the tree root, which is where we stand: the band's own
    spelling, and therefore the band's memo key."""
    return os.path.join(SRCDIR, clip + ".y4m")


def fps_of(clip):
    with open(src_of(clip), "rb") as f:
        hdr = f.readline().decode("latin-1")
    m = re.search(r" F(\d+):(\d+)", hdr)
    return int(m.group(1)) / int(m.group(2)) if m else 30.0


def target_of(clip, ladders):
    """The pin: rung RUNG of the clip's band ladder, in BYTES, by
    bd_vs_x265.py's own conversion so the target string is the one its
    memo was filled under."""
    rungs = ladders[clip]
    if len(rungs) < RUNG:
        return None
    secs = FRAMES / fps_of(clip)
    return int(rungs[RUNG - 1] * 1000.0 / 8.0 * secs)


def preset_args():
    """`--preset medium` is the default, so medium says nothing: the
    template then IS the band's. OURFLAGS rides here, after the preset,
    because that is the order a user writes it in and the order the
    override rule is written for: the flag wins over the preset."""
    p = [] if PRESET == "medium" else ["--preset", PRESET]
    return p + OURARGV


# ---------------------------------------------------------------- the legs
#
# The solved point is a matched TARGET, not a matched encode: both sides
# bisect an INTEGER knob onto the same byte target and land on opposite
# sides of it by up to 12%. A dVMAF read across that gap is not the
# quality leg - it is "who spent more bits", which is the question the
# dSIZE leg exists to ask separately. fast's stefan_cif is the case that
# makes it plain: -1.60 dVMAF at -7.86% of the bytes.
#
# So each leg is read at ITS OWN matched axis, by interpolating the
# reference's four-rung ladder - which `bd_at_rate`'s memo already holds,
# at no cost - onto our solved point:
#
#   quality    dVMAF_eq_bytes  = our NEG - x265's NEG interpolated at OUR
#                                bytes. Ours ahead is positive.
#   size       dSIZE_eq_vmaf   = our bytes / x265's bytes interpolated at
#                                OUR VMAF-NEG - 1. Ours larger is positive.
#
# **Read as theirs-at-ours, not ours-at-theirs**, and the reason is the
# cache and not a preference: `bd_at_rate` keys its encode memo on a hash
# of the encoder BINARY, and every build of ours is a different file (the
# build path is in it), so our own four-rung ladder is only ever present
# for the binary that wrote it - the band's binary is already gone from
# this box - while x265 on PATH is one stable binary whose four rungs are
# in the memo for all fifteen clips. Theirs-at-ours therefore costs
# nothing on any box; ours-at-theirs would re-encode the whole ladder.
# Six cells had enough of our own cached points to read BOTH directions:
# they agree to a median of 0.05 dVMAF and 0.05% of size, worst 0.33 and
# 2.56% (ducks_720p, whose three points span too little rate to fix the
# quality-to-rate slope). The reference's four rungs are the better
# conditioned curve as well as the free one.
#
# Both legs are ONE-SIDED, as the plan writes them: a goal is "as good at
# less time", so being AHEAD on quality or SMALLER on size is a pass with
# margin, never a failure. The bars are our quality not more than 0.5
# below theirs, and our size not more than 1.0% above theirs.


def _interp(xs, ys, x):
    """Piecewise-linear y(x) on a monotone-x curve, extrapolating along
    the nearest segment outside the span (flagged by the caller)."""
    n = len(xs)
    if n < 2:
        return None, True
    if x <= xs[0]:
        i, out = 0, x < xs[0]
    elif x >= xs[-1]:
        i, out = n - 2, x > xs[-1]
    else:
        out = False
        i = max(j for j in range(n - 1) if xs[j] <= x)
    dx = xs[i + 1] - xs[i]
    if dx == 0:
        return ys[i], out
    return ys[i] + (ys[i + 1] - ys[i]) * (x - xs[i]) / dx, out


def ladder_points(tmpl, src):
    """Every (bytes, VMAF-NEG) this arm already has in the memo, sorted by
    bytes. CACHE-ONLY: it stats the encode blobs and reads the metric JSON
    beside them, and runs neither the encoder nor ffmpeg. The metric memo
    is keyed on the BITSTREAM (bd_at_rate._content_json), so a point keeps
    its VMAF across rebuilds even though the encode memo does not."""
    pts = {}
    for q in range(0, 52):
        cb, cj = bdr._cache_paths(tmpl, q, src)
        if not (cb and os.path.exists(cb)):
            continue
        neg = None
        for p in (cj, bdr._content_json(cb, src)):
            if not (p and os.path.exists(p)):
                continue
            try:
                d = json.load(open(p))
            except (OSError, ValueError):
                continue
            if d and "VMAF-NEG" in d:
                neg = d["VMAF-NEG"]
                break
        if neg is not None:
            pts[os.path.getsize(cb)] = neg
    return sorted(pts.items())


# --------------------------------------- the per-plane readings at our bytes
#
# Phase 13 B0. VMAF-NEG reads the Y plane and nothing else, and so does the
# PSNR-Y leg, so every leg above is blind to what the encoder does with the
# other two planes, and the record
# already names a LEVEL gap there rather than a regression: our chroma PSNR
# sits 0.8-0.9 dB under x265's on bus at every matched rate
# (docs/phase7-psy-design.md "The chroma gap"). This reads that level, per
# plane, at the same pinned point, by the same interpolation the quality leg
# uses - x265's Cb and Cr interpolated in LOG bytes onto OUR bytes - so the
# number is "how far behind on chroma at the bits we actually spent" and not
# "who spent more bits".
#
# It is INFORMATIONAL: it is printed and recorded, and no goal passes or
# fails on it (the owner decides when it becomes a leg). Every chroma item
# in sec B ships against it.
#
# Unlike the luma ladder, the plane split is not free: `chromaPSNR` in the
# memo is the MEAN of the two and a mean cannot say which plane is behind, so
# a point cached before B0 has to be re-read with ffmpeg once. Only the two
# rungs the interpolation actually lands between are read, which is exactly
# what `_interp` would have used out of the whole ladder, and the answer is
# written back into the memo so no later run pays for it again.
#
# Phase 13 B4 put PSNR-Y (the LEG) and SSIM-Y (a reading beside it) on the
# same interpolation, and the two are not the same kind of number to read:
#
#   PSNR-Y costs NOTHING. `lumaPSNR` comes off the very `psnr`-filter run
#   the chroma pair above already pays for - the filter prints y:, u: and
#   v: on one line - so every point that can answer dCb can answer dY.
#
#   SSIM-Y is MEMO-ONLY. It is a second ffmpeg run per point, and it is a
#   reading with no bar on it; a point that does not already carry `ssimY`
#   (only `bd_vs_x265.py --extra` writes it) prints `-` instead of being
#   decoded for it. Interpolated on the dB scale, `-10 log10(1 - SSIM)`,
#   which is the scale B4's tables are in and the one a rate-quality curve
#   is straight in.


def _ladder_blobs(tmpl, src):
    """[(bytes, blob, json)] for every point of this template already in the
    encode memo, sorted by bytes. Stats files; runs nothing."""
    out = {}
    for q in range(0, 52):
        cb, cj = bdr._cache_paths(tmpl, q, src)
        if cb and os.path.exists(cb):
            out[os.path.getsize(cb)] = (cb, cj)
    return [(b, v[0], v[1]) for b, v in sorted(out.items())]


def _planes_of(blob, cj, src):
    """(Y, Cb, Cr) dB for one cached bitstream, memoized beside it. Reads the
    json first; only a point that predates the plane split costs an ffmpeg
    run, and it pays it once for every later reader. All three planes come
    off that ONE run, so the luma the sixth leg reads is free wherever the
    chroma pair is."""
    ck = bdr._content_json(blob, src)
    d = None
    for p in (cj, ck):
        if not (p and os.path.exists(p)):
            continue
        try:
            j = json.load(open(p))
        except (OSError, ValueError):
            continue
        if j and all(k in j for k in (_bd.LUMA_KEY, _bd.CB_KEY, _bd.CR_KEY)):
            return j[_bd.LUMA_KEY], j[_bd.CB_KEY], j[_bd.CR_KEY]
        if j and d is None:
            d = j
    y, u, v = _bd.planes_psnr_of(blob, src)
    if u is None:
        return None, None, None
    d = dict(d or {})
    d[_bd.CB_KEY], d[_bd.CR_KEY] = u, v
    d[_bd.CHROMA_KEY] = (u + v) / 2.0
    if y is not None:
        d[_bd.LUMA_KEY] = y
    for p in (cj, ck):
        if p:
            try:
                bdr._write_json(p, d)
            except OSError:
                pass
    return y, u, v


def _ssimy_of(blob, cj, src):
    """SSIM-Y in dB for one cached bitstream, or None. MEMO-ONLY: it runs
    nothing, because it has no bar on it and a decode is not worth paying
    for a reading (see the note above). `bd_vs_x265.py --extra` is what
    writes `ssimY`, into the same two memo entries this reads."""
    for p in (cj, bdr._content_json(blob, src)):
        if not (p and os.path.exists(p)):
            continue
        try:
            j = json.load(open(p))
        except (OSError, ValueError):
            continue
        if j and j.get(_bd.SSIM_Y_KEY) is not None:
            return _bd.ssim_db(j[_bd.SSIM_Y_KEY])
    return None


def planes_at_bytes(tmpl, src, our_bytes):
    """x265's (Y, Cb, Cr, SSIM-Y) in dB interpolated onto OUR bytes off its
    cached ladder, in LOG bytes, the axis the quality leg interpolates on.
    SSIM-Y is None unless BOTH bracketing rungs carry it.

    Piecewise-linear interpolation reads ONE segment, so only the two rungs
    that bracket our point are resolved - which is the same pair `_interp`
    would have picked out of the full ladder, and the same answer."""
    none4 = (None, None, None, None)
    pts = _ladder_blobs(tmpl, src)
    if len(pts) < 2:
        return none4 + ("no ladder",)
    xs = [math.log(b) for b, _, _ in pts]
    x = math.log(our_bytes)
    if x <= xs[0]:
        i, out = 0, x < xs[0]
    elif x >= xs[-1]:
        i, out = len(xs) - 2, x > xs[-1]
    else:
        out = False
        i = max(j for j in range(len(xs) - 1) if xs[j] <= x)
    a = _planes_of(pts[i][1], pts[i][2], src)
    b = _planes_of(pts[i + 1][1], pts[i + 1][2], src)
    if a[1] is None or b[1] is None:
        return none4 + ("no psnr",)
    t = 0.0 if xs[i + 1] == xs[i] else (x - xs[i]) / (xs[i + 1] - xs[i])

    def lerp(u, v):
        return None if u is None or v is None else u + (v - u) * t

    sa = _ssimy_of(pts[i][1], pts[i][2], src)
    sb = _ssimy_of(pts[i + 1][1], pts[i + 1][2], src)
    return (lerp(a[0], b[0]), lerp(a[1], b[1]), lerp(a[2], b[2]),
            lerp(sa, sb), ("chroma-extrap" if out else ""))


def legs_at_matched_axes(our_bytes, our_neg, ref_pts):
    """(dvmaf_eq_bytes, dsize_eq_vmaf_pct, flags) from the reference's
    ladder. Rate is interpolated in LOG bytes, which is the axis a
    rate-quality curve is straight in and the one BD-rate integrates."""
    if len(ref_pts) < 2:
        return None, None, "no ladder"
    xb = [math.log(b) for b, _ in ref_pts]
    yv = [v for _, v in ref_pts]
    ref_neg, out_v = _interp(xb, yv, math.log(our_bytes))
    ref_lb, out_s = _interp(yv, xb, our_neg)
    flags = []
    if out_v:
        flags.append("vmaf-extrap")
    if out_s:
        flags.append("size-extrap")
    dv = our_neg - ref_neg
    ds = (our_bytes / math.exp(ref_lb) - 1.0) * 100.0
    return dv, ds, ",".join(flags)


def our_tmpl():
    """bd_vs_x265.py's `ours` template, verbatim, for the given preset."""
    p = " ".join(preset_args())
    p = (" " + p) if p else ""
    return (f"{BAND_N265} {{src}} --frames {FRAMES} --crf {{q}} "
            f"--aq 1 --cutree 1{p} -o {{out}}")


def x265_tmpl():
    """bd_vs_x265.py's `ref` template, verbatim - with the preset name
    where the `medium` literal stood, so X265PRESET=medium reproduces the
    old string, and with it the old memo key, character for character."""
    return (f"{X265} --preset {X265PRESET} --crf {{q}} --frames {FRAMES} "
            f"--pools none --frame-threads 1 --log-level none -o {{out}} "
            f"{{src}}")


def our_timed(q, src, t1, noasm):
    cmd = [TIMED_N265, src, "--frames", str(FRAMES), "--crf", str(q),
           "--aq", "1", "--cutree", "1"] + preset_args()
    if t1:
        cmd += ["--threads", "1"]
    cmd += ["-o", "/dev/null"]
    env = dict(os.environ)
    if noasm:
        env["N265_ASM_OFF"] = "all"
    else:
        env.pop("N265_ASM_OFF", None)
    return cmd, env


def x265_timed(q, src, t1, noasm):
    cmd = [X265, "--preset", X265PRESET, "--crf", str(q), "--frames",
           str(FRAMES), "--log-level", "none"]
    if t1:
        cmd += ["--pools", "none", "--frame-threads", "1"]
    if noasm:
        cmd += ["--no-asm"]
    cmd += ["-o", "/dev/null", src]
    return cmd, dict(os.environ)


def warm_file(path):
    """One untimed sequential read. x265 mmaps its input and we stream
    ours, so a cold file lands in the two wall clocks asymmetrically."""
    with open(path, "rb") as f:
        while f.read(1 << 22):
            pass


def wall(cmd, env):
    """(seconds, instructions retired or None) for one run."""
    if INSTR:
        r = boxlib.measure(cmd, env=env)
        if r["rc"] != 0:
            raise subprocess.CalledProcessError(r["rc"], cmd)
        return r["wall"], r["instr"]
    t0 = time.perf_counter()
    subprocess.run(cmd, env=env, stdout=subprocess.DEVNULL,
                   stderr=subprocess.DEVNULL, check=True)
    return time.perf_counter() - t0, None


def _med(xs):
    xs = [x for x in xs if x is not None]
    return int(statistics.median(xs)) if xs else None


def solve_clip(clip, target):
    """Both sides bisected onto `target` bytes, with bytes and VMAF-NEG
    off the resulting bitstreams. Everything here is memoized."""
    src = src_of(clip)
    work = tempfile.mkdtemp(prefix="goallegs.")
    out = {}
    try:
        for tag, tmpl in (("ours", our_tmpl()), ("x265", x265_tmpl())):
            o = os.path.join(work, tag + ".hevc")
            q = bdr.solve(tmpl, src, target, o, {})
            sz = bdr.enc(tmpl, q, src, o)
            v = bdr.vmaf_cached(tmpl, q, src, o, work)
            neg = v.get("VMAF-NEG") if v else None
            out[tag] = {"q": q, "bytes": sz, "neg": neg,
                        # B0: our own solved point's two chroma planes come
                        # out of the same memo entry the VMAF does, at no
                        # extra ffmpeg run on any point touched since the
                        # split landed. B4's luma rides the same entry off
                        # the same one run; its SSIM-Y is there only if the
                        # point has been touched in `--extra` mode, and a
                        # None here is what prints `-`.
                        "cb": v.get(_bd.CB_KEY) if v else None,
                        "cr": v.get(_bd.CR_KEY) if v else None,
                        "y": v.get(_bd.LUMA_KEY) if v else None,
                        "ssimy": (_bd.ssim_db(v[_bd.SSIM_Y_KEY])
                                  if v and v.get(_bd.SSIM_Y_KEY) is not None
                                  else None)}
            if tag == "ours":
                out["ours"]["bits_path"] = _keep(o, work)
        out["src"] = src
        out["work"] = work
        return clip, out
    except Exception as e:                       # noqa: BLE001
        return clip, {"error": f"{type(e).__name__}: {e}", "work": work}


def _keep(path, work):
    keep = os.path.join(work, "solved.hevc")
    if path != keep:
        os.replace(path, keep)
    return keep


def check_binaries(clip, r):
    """The timed binary must write the bitstream the solve priced. Costs
    one untimed encode per clip and is what licenses reading the clock on
    build-rel while the memo is filled by build/n265."""
    if os.path.realpath(TIMED_N265) == os.path.realpath(
            os.path.join(ROOT, BAND_N265)):
        return None
    ref = r["ours"].get("bits_path")
    if not ref or not os.path.exists(ref):
        return "solved bitstream missing"
    out = os.path.join(r["work"], "timedbin.hevc")
    # at auto threads because it is the faster of the two and the output
    # is the same either way (cross-thread-count identity is a standing
    # gate, and the 12-cell cmp that licensed the release switch covers
    # both thread counts)
    cmd, env = our_timed(r["ours"]["q"], r["src"], False, False)
    cmd = [c if c != "/dev/null" else out for c in cmd]
    subprocess.run(cmd, env=env, stdout=subprocess.DEVNULL,
                   stderr=subprocess.DEVNULL, check=True)
    p = subprocess.run(["cmp", "-s", ref, out])
    return None if p.returncode == 0 else "timed binary differs from solve"


def load1():
    return os.getloadavg()[0]


def main():
    known = {"--help", "--no-csv", "--no-time", "--from-csv"}
    for a in sys.argv[1:]:
        if a not in known:
            sys.exit(f"goal_legs.py: unknown argument {a!r}; flags: "
                     f"{' '.join(sorted(known))}. Everything else is an "
                     f"environment variable; see the docstring.")
    if "--help" in sys.argv:
        print(__doc__)
        return
    do_csv = "--no-csv" not in sys.argv
    do_time = "--no-time" not in sys.argv
    if GOAL not in GOALS:
        sys.exit(f"goal_legs.py: GOAL must be one of {sorted(GOALS)}")
    if "--from-csv" in sys.argv:
        from_csv()
        return
    tier, pure_c = GOALS[GOAL]
    t1 = tier == "t1"

    ladders = json.load(open(os.path.join(ROOT, "scripts/ladders.json")))
    # clips outside tests/corpus (the 4K clip) keep their rungs apart, so
    # the scripts that walk every ladders.json key never meet them
    ladders.update(json.load(open(os.path.join(ROOT, "scripts/ladders_extra.json"))))
    sel = os.environ.get("CLIPS", "")
    clips = sel.split(",") if sel else list(ladders)
    missing = [c for c in clips if c not in ladders
               or target_of(c, ladders) is None]
    clips = [c for c in clips if c not in missing]
    if not clips:
        sys.exit("goal_legs.py: no clip with a ladder rung to read")

    commit = subprocess.run(["git", "-C", ROOT, "rev-parse", "--short",
                             "HEAD"], capture_output=True, text=True
                            ).stdout.strip()
    print(f"goal {GOAL} ({'pure C' if pure_c else 'as shipped'}, "
          f"{'--threads 1' if t1 else 'auto threads'})   preset {PRESET} "
          f"vs x265 {X265PRESET}   "
          f"rung {RUNG}   {FRAMES} frames   commit {commit}")
    print(f"solve:  {our_tmpl()}")
    print(f"        {x265_tmpl()}")
    print(f"timed:  {TIMED_N265}   load {load1():.2f}")
    print(f"arm:    {ARM if ARM else '(default: no N265_* set)'}")
    if missing:
        print(f"no ladder rung {RUNG}: {', '.join(missing)} (skipped)")

    targets = {c: target_of(c, ladders) for c in clips}
    boxlib.wait_untimed("goal_legs.py")
    print(f"\nsolving {len(clips)} clip(s) onto rung {RUNG} at JOBS={JOBS} "
          f"(memoised)", flush=True)
    res = {}
    with ThreadPoolExecutor(max_workers=JOBS) as ex:
        for clip, r in ex.map(lambda c: solve_clip(c, targets[c]), clips):
            res[clip] = r
            if "error" in r:
                print(f"  {clip:<18} SOLVE FAILED: {r['error']}", flush=True)
            elif r["ours"]["neg"] is None or r["x265"]["neg"] is None:
                print(f"  {clip:<18} NO VMAF-NEG on one side (ours "
                      f"{r['ours']['neg']}, x265 {r['x265']['neg']}); skipped",
                      flush=True)
            else:
                print(f"  {clip:<18} target {targets[clip]:>9} B -> ours crf "
                      f"{r['ours']['q']:>2} {r['ours']['bytes']:>9} B NEG "
                      f"{r['ours']['neg']:6.2f} | x265 crf {r['x265']['q']:>2} "
                      f"{r['x265']['bytes']:>9} B NEG {r['x265']['neg']:6.2f}",
                      flush=True)
    clips = [c for c in clips if "error" not in res[c]
             and res[c]["ours"]["neg"] is not None
             and res[c]["x265"]["neg"] is not None]
    if not clips:
        sys.exit("goal_legs.py: nothing solved")

    if do_time:
        for c in clips:
            bad = check_binaries(c, res[c])
            if bad:
                sys.exit(f"goal_legs.py: {c}: {bad}. The timed binary and "
                         f"the solving binary must agree byte for byte; "
                         f"rebuild, or set N265 to one tree.")
        # The solve and the verification encodes are untimed work, but
        # they are still work, and the one-minute average is still full
        # of them when they finish. So the box is not asked whether it is
        # quiet until it has had a chance to become quiet again: poll,
        # up to LOADWAIT seconds, and refuse only if it never settles -
        # which is what "something else is running" looks like.
        lv = load1()
        t_wait = time.monotonic() + LOADWAIT
        while lv > LOADMAX and time.monotonic() < t_wait:
            print(f"  load {lv:.2f} > {LOADMAX}, settling...", flush=True)
            time.sleep(20)
            lv = load1()
        if lv > LOADMAX:
            sys.exit(f"goal_legs.py: load {lv:.2f} > LOADMAX {LOADMAX} after "
                     f"{LOADWAIT:.0f}s; a timed leg needs a quiet box. Re-run "
                     f"when it is quiet, or use --no-time.")
        print(f"\ntiming {len(clips)} clip(s), {NRUNS} interleaved rounds, "
              f"load {lv:.2f}", flush=True)
        lock = boxlib.timing_lock(f"goal_legs G{GOAL} {PRESET}")
        lock.__enter__()
        for c in clips:
            r = res[c]
            warm_file(r["src"])
            oc, oe = our_timed(r["ours"]["q"], r["src"], t1, pure_c)
            xc, xe = x265_timed(r["x265"]["q"], r["src"], t1, pure_c)
            ours, x = [], []
            for i in range(NRUNS):
                # order reversed on alternate rounds: whatever the box
                # does to the first arm of a pair, it does to both
                if i % 2 == 0:
                    ours.append(wall(oc, oe))
                    x.append(wall(xc, xe))
                else:
                    x.append(wall(xc, xe))
                    ours.append(wall(oc, oe))
            r["ours"]["wall"] = statistics.median(w for w, _ in ours)
            r["x265"]["wall"] = statistics.median(w for w, _ in x)
            r["ours"]["instr"] = _med(n for _, n in ours)
            r["x265"]["instr"] = _med(n for _, n in x)
            r["mult"] = r["ours"]["wall"] / r["x265"]["wall"]
            ir = (f"   instr {r['ours']['instr'] / r['x265']['instr']:5.2f}x"
                  if r["ours"]["instr"] and r["x265"]["instr"] else "")
            print(f"  {c:<18} ours {r['ours']['wall']:7.3f} s   x265 "
                  f"{r['x265']['wall']:7.3f} s   {r['mult']:5.2f}x{ir}",
                  flush=True)
        lock.__exit__(None, None, None)
        print(f"load after: {load1():.2f}")

    rows = []
    for c in clips:
        r = res[c]
        dsize = (r["ours"]["bytes"] / r["x265"]["bytes"] - 1.0) * 100.0
        dvmaf = r["ours"]["neg"] - r["x265"]["neg"]
        dv_eq, ds_eq, flags = legs_at_matched_axes(
            r["ours"]["bytes"], r["ours"]["neg"], ladder_points(x265_tmpl(),
                                                               r["src"]))
        # B0's chroma reading and B4's luma leg, on the same pinned point
        # and the same axis, off the same interpolation
        x_y, x_cb, x_cr, x_sy, _cf = planes_at_bytes(
            x265_tmpl(), r["src"], r["ours"]["bytes"])

        def _d(ours, theirs):
            return (ours - theirs
                    if ours is not None and theirs is not None else None)

        dcb = _d(r["ours"]["cb"], x_cb)
        dcr = _d(r["ours"]["cr"], x_cr)
        dy = _d(r["ours"]["y"], x_y)
        dsy = _d(r["ours"]["ssimy"], x_sy)
        rows.append({
            "date": datetime.date.today().isoformat(), "commit": commit,
            "preset": PRESET, "goal": GOAL, "tier": "C" if pure_c else "simd",
            "threads": "1" if t1 else "auto", "clip": c, "frames": FRAMES,
            "rung": RUNG, "target_bytes": targets[c],
            "our_crf": r["ours"]["q"], "x265_crf": r["x265"]["q"],
            "our_bytes": r["ours"]["bytes"], "x265_bytes": r["x265"]["bytes"],
            "dsize_pct": round(dsize, 2),
            "our_vmafneg": round(r["ours"]["neg"], 3),
            "x265_vmafneg": round(r["x265"]["neg"], 3),
            "dvmafneg": round(dvmaf, 3),
            "our_wall_s": round(r["ours"]["wall"], 4) if "wall" in r["ours"] else "",
            "x265_wall_s": round(r["x265"]["wall"], 4) if "wall" in r["x265"] else "",
            "mult": round(r["mult"], 4) if "mult" in r else "",
            # appended LAST so every column above keeps its position and
            # every row written before these existed stays readable
            "dvmaf_eq_bytes": round(dv_eq, 3) if dv_eq is not None else "",
            "dsize_eq_vmaf_pct": round(ds_eq, 3) if ds_eq is not None else "",
            # the N265_* environment this arm ran under, LAST for the same
            # reason the two columns above are: every older column keeps
            # its position and every older row stays readable
            "arm": ARM,
            # B0's chroma reading, appended after `arm` for that same
            # reason: ours minus x265's, per plane, in dB at OUR bytes.
            # Ours ahead is positive. Blank on a clip whose reference
            # ladder has no psnr to interpolate.
            "dcb_eq_bytes": round(dcb, 3) if dcb is not None else "",
            "dcr_eq_bytes": round(dcr, 3) if dcr is not None else "",
            # C1: WHO we were read against. Appended last for the reason
            # every column above it was. A row written before this existed
            # carries "", which reads as medium, because medium is what
            # every one of them was solved and timed against.
            "x265_preset": X265PRESET,
            # B4's leg, and the reading beside it, appended last for that
            # same reason: ours minus x265's PSNR-Y in dB at OUR bytes, and
            # the same difference on SSIM-Y's dB scale. Ours ahead positive.
            # `dssimy_eq_bytes` is blank on any point whose memo does not
            # carry `ssimY` - it is never decoded for.
            "dy_eq_bytes": round(dy, 3) if dy is not None else "",
            "dssimy_eq_bytes": round(dsy, 3) if dsy is not None else "",
            # instructions retired, median per arm, LAST for the same
            # reason: blank on a row timed with INSTR=0 or not timed
            "our_instr": r["ours"].get("instr") or "",
            "x265_instr": r["x265"].get("instr") or "",
        })

    report(rows, GOAL, PRESET, RUNG, FRAMES, X265PRESET)
    if do_csv:
        append_csv(rows)
    for c in res.values():
        shutil.rmtree(c.get("work", ""), ignore_errors=True)


def _f(w, k):
    """A CSV field as a float, or None when the column is blank."""
    v = w.get(k, "")
    if v == "" or v is None:
        return None
    return float(v)


def report(rows, goal, preset, rung, frames, xpreset="medium"):
    hdr = (f"{'clip':<18} {'ourCRF':>6} {'xCRF':>5} {'dVMAF@=B':>9} "
           f"{'dSIZE@=V':>9} {'dCb@=B':>7} {'dCr@=B':>7} {'dY@=B':>7} "
           f"{'dSSY@=B':>8} {'ours s':>8} {'x265 s':>8} {'mult':>7} "
           f"{'instr x':>7}")
    print(f"\n=== goal {goal}, preset {preset} vs x265 {xpreset}, rung "
          f"{rung}, {frames} frames, {len(rows)} clips ===")
    print("    dVMAF@=B: ours minus x265's VMAF-NEG interpolated at OUR "
          "bytes (ours ahead positive)")
    print("    dSIZE@=V: ours over x265's bytes interpolated at OUR "
          "VMAF-NEG (ours larger positive)")
    print("    dCb/dCr@=B: ours minus x265's chroma PSNR (dB) interpolated "
          "at OUR bytes (ours ahead positive)")
    print("    dY@=B: the same on PSNR-Y, which is the fifth leg; dSSY@=B "
          "the same on SSIM-Y in dB, which is a reading with no bar")
    print("    instr x: ours over x265's instructions retired, medians of "
          "the timed runs (load-proof; '-' on rows timed without it)")
    print(hdr)
    print("-" * len(hdr))
    for w in rows:
        m = _f(w, "mult")
        ow, xw = _f(w, "our_wall_s"), _f(w, "x265_wall_s")
        dv, ds = _f(w, "dvmaf_eq_bytes"), _f(w, "dsize_eq_vmaf_pct")
        dcb, dcr = _f(w, "dcb_eq_bytes"), _f(w, "dcr_eq_bytes")
        dy, dsy = _f(w, "dy_eq_bytes"), _f(w, "dssimy_eq_bytes")
        oi, xi = _f(w, "our_instr"), _f(w, "x265_instr")
        ix = oi / xi if oi and xi else None
        print(f"{w['clip']:<18} {int(w['our_crf']):>6} {int(w['x265_crf']):>5} "
              f"{f'{dv:+.2f}' if dv is not None else '-':>9} "
              f"{f'{ds:+.2f}' if ds is not None else '-':>9} "
              f"{f'{dcb:+.2f}' if dcb is not None else '-':>7} "
              f"{f'{dcr:+.2f}' if dcr is not None else '-':>7} "
              f"{f'{dy:+.2f}' if dy is not None else '-':>7} "
              f"{f'{dsy:+.2f}' if dsy is not None else '-':>8} "
              f"{f'{ow:.3f}' if ow is not None else '-':>8} "
              f"{f'{xw:.3f}' if xw is not None else '-':>8} "
              f"{f'{m:.2f}x' if m is not None else '-':>7} "
              f"{f'{ix:.2f}x' if ix is not None else '-':>7}")

    print("\n--- the legs (one-sided, as the plan writes them) ---")
    mults = [_f(w, "mult") for w in rows if _f(w, "mult") is not None]
    if mults:
        med = statistics.median(mults)
        worst = max((w for w in rows if _f(w, "mult") is not None),
                    key=lambda w: _f(w, "mult"))
        print(f"median speed   {med:5.2f}x  (bar <= {BAR_MEDIAN:.2f}x)  "
              f"{'PASS' if med <= BAR_MEDIAN else 'FAIL'}")
        print(f"worst clip     {_f(worst, 'mult'):5.2f}x  {worst['clip']}  "
              f"(bar < {BAR_WORST:.2f}x)  "
              f"{'PASS' if _f(worst, 'mult') < BAR_WORST else 'FAIL'}")
    else:
        print("median speed     -    not timed")
        print("worst clip       -    not timed")

    vs = [(w, _f(w, "dvmaf_eq_bytes")) for w in rows]
    ss = [(w, _f(w, "dsize_eq_vmaf_pct")) for w in rows]
    bad_v = [w for w, v in vs if v is not None and v < BAR_DVMAF]
    bad_s = [w for w, v in ss if v is not None and v > BAR_DSIZE]
    nv = [v for _, v in vs if v is not None]
    ns = [v for _, v in ss if v is not None]
    if nv:
        ahead = sum(1 for v in nv if v > 0)
        print(f"quality        {len(bad_v)}/{len(nv)} clips more than "
              f"{-BAR_DVMAF:.1f} BELOW x265 at equal bytes  "
              f"{'PASS' if not bad_v else 'FAIL'}"
              f"   (median {statistics.median(nv):+.2f}, {ahead}/{len(nv)} "
              f"ahead, worst {min(nv):+.2f})"
              + ("   " + ", ".join(f"{w['clip']} {_f(w,'dvmaf_eq_bytes'):+.2f}"
                                   for w in sorted(bad_v, key=lambda w:
                                                   _f(w, "dvmaf_eq_bytes"))[:6])
                 if bad_v else ""))
    if ns:
        smaller = sum(1 for v in ns if v < 0)
        print(f"compression    {len(bad_s)}/{len(ns)} clips more than "
              f"{BAR_DSIZE:.1f}% LARGER than x265 at equal quality  "
              f"{'PASS' if not bad_s else 'FAIL'}"
              f"   (median {statistics.median(ns):+.2f}%, {smaller}/{len(ns)} "
              f"smaller, worst {max(ns):+.2f}%)"
              + ("   " + ", ".join(
                  f"{w['clip']} {_f(w, 'dsize_eq_vmaf_pct'):+.1f}%"
                  for w in sorted(bad_s,
                                  key=lambda w: -_f(w, "dsize_eq_vmaf_pct"))[:6])
                 if bad_s else ""))

    # The fifth LEG (phase 13 B4, adopted by the owner 2026-09-14). Read
    # exactly like the quality leg - one-sided, at equal bytes, off the
    # reference's own ladder - and it PASSES or FAILS. The 0.5 dB line is
    # printed under it as DEBT and gates nothing. SSIM-Y goes on the same
    # line's medians with no bar at all, and is simply absent on a corpus
    # whose memo has not been through `--extra`.
    ys = [(w, _f(w, "dy_eq_bytes")) for w in rows]
    ys = [(w, v) for w, v in ys if v is not None]
    if ys:
        ny = [v for _, v in ys]
        bad_y = [(w, v) for w, v in ys if v < BAR_DY]
        debt_y = [(w, v) for w, v in ys if v < DEBT_DY]
        worst = min(ys, key=lambda t: t[1])
        ahead = sum(1 for v in ny if v > 0)
        print(f"psnr-y         {len(bad_y)}/{len(ny)} clips more than "
              f"{-BAR_DY:.1f} dB BELOW x265 at equal bytes  "
              f"{'PASS' if not bad_y else 'FAIL'}"
              f"   (median {statistics.median(ny):+.2f}, {ahead}/{len(ny)} "
              f"ahead, worst {worst[0]['clip']} {worst[1]:+.2f})"
              + ("   " + ", ".join(f"{w['clip']} {v:+.2f}"
                                   for w, v in sorted(bad_y,
                                                      key=lambda t: t[1])[:6])
                 if bad_y else ""))
        print(f"  debt         {len(debt_y)}/{len(ny)} clips below "
              f"{-DEBT_DY:.1f} dB (recorded, not gated)"
              + ("   " + ", ".join(f"{w['clip']} {v:+.2f}"
                                   for w, v in sorted(debt_y,
                                                      key=lambda t: t[1]))
                 if debt_y else ""))
        sy = [_f(w, "dssimy_eq_bytes") for w, _ in ys]
        sy = [v for v in sy if v is not None]
        if sy:
            print(f"  ssim-y       median {statistics.median(sy):+.2f} dB, "
                  f"{sum(1 for v in sy if v > 0)}/{len(sy)} ahead, worst "
                  f"{min(sy):+.2f}   (no bar)")
    else:
        print("psnr-y           -    not read")

    # The reading that is NOT a leg (phase 13 B0). INFORMATIONAL: no PASS,
    # no FAIL, and no goal turns on it. The bar is per PLANE and the count is
    # of clips
    # behind on EITHER, because a mean over two planes hides a clip that is
    # level on one and behind on the other.
    ch = [(w, _f(w, "dcb_eq_bytes"), _f(w, "dcr_eq_bytes")) for w in rows]
    ch = [(w, b, r) for w, b, r in ch if b is not None and r is not None]
    if ch:
        bad_c = [(w, min(b, r)) for w, b, r in ch if min(b, r) < BAR_DCHROMA]
        worst = min(ch, key=lambda t: min(t[1], t[2]))
        mcb = statistics.median([b for _, b, _ in ch])
        mcr = statistics.median([r for _, _, r in ch])
        print(f"chroma         {len(bad_c)}/{len(ch)} clips more than "
              f"{-BAR_DCHROMA:.1f} dB BELOW x265 on either plane   "
              f"(worst {worst[0]['clip']} {worst[1]:+.2f} / {worst[2]:+.2f}, "
              f"median {mcb:+.2f} / {mcr:+.2f})"
              + ("   " + ", ".join(
                  f"{w['clip']} {v:+.2f}"
                  for w, v in sorted(bad_c, key=lambda t: t[1])[:8])
                 if bad_c else ""))


def migrate_csv(missing):
    """Give every existing row a blank cell for `missing`, in place.

    Only ever called for OPTIONAL_COLUMNS, and it adds cells rather than
    touching any: every value already in the file survives, in its own
    column, in its own row. Written beside the file and renamed onto it
    so an interrupted migration leaves the old file whole."""
    with open(CSV_PATH, newline="") as f:
        rows = list(csv.DictReader(f))
        f.seek(0)
        have = next(csv.reader(f), None) or []
    fields = have + list(missing)
    d = os.path.dirname(CSV_PATH) or "."
    fd, tmp = tempfile.mkstemp(dir=d, prefix=".goal-legs.", suffix=".csv")
    try:
        with os.fdopen(fd, "w", newline="") as f:
            wr = csv.DictWriter(f, fieldnames=fields, extrasaction="ignore")
            wr.writeheader()
            for w in rows:
                wr.writerow({k: w.get(k, "") for k in fields})
        os.replace(tmp, CSV_PATH)
    except BaseException:
        os.unlink(tmp)
        raise
    print(f"migrated {os.path.relpath(CSV_PATH, ROOT)}: {len(rows)} row(s) "
          f"given a blank {', '.join(missing)}")
    return fields


def append_csv(rows):
    """Append, never rewrite - and never against a header that is not the
    file's own: an older file keeps its column order and simply carries
    blanks for columns it predates."""
    os.makedirs(os.path.dirname(CSV_PATH), exist_ok=True)
    fields = list(rows[0].keys())
    new = not os.path.exists(CSV_PATH)
    if not new:
        with open(CSV_PATH, newline="") as f:
            have = next(csv.reader(f), None)
        if have:
            fields = have + [k for k in rows[0] if k not in have]
            gap = [k for k in fields if k not in have]
            if gap and all(k in OPTIONAL_COLUMNS for k in gap):
                fields = migrate_csv(gap)
                gap = []
            if gap:
                sys.exit(f"goal_legs.py: {CSV_PATH} is missing columns "
                         f"{gap}; it predates them. Migrate the file (add "
                         f"the columns to the header and to every row) "
                         f"before appending.")
    with open(CSV_PATH, "a", newline="") as f:
        wr = csv.DictWriter(f, fieldnames=fields, extrasaction="ignore")
        if new:
            wr.writeheader()
        for w in rows:
            wr.writerow(w)
    print(f"\nappended {len(rows)} row(s) to "
          f"{os.path.relpath(CSV_PATH, ROOT)}")


def from_csv():
    """Re-print a run's tables from docs/data/goal-legs.csv: the most
    recent block for this PRESET and GOAL. Reads nothing but the file -
    no encode, no metric, no stopwatch - so a row can be re-read while
    the box belongs to somebody else.

    A block is (date, commit, ARM): two runs of the same commit under
    different `N265_*` are two runs, and rows written before the `arm`
    column existed carry "" for it, which is what a default run writes,
    so they re-print as they always did."""
    with open(CSV_PATH, newline="") as f:
        allrows = list(csv.DictReader(f))
    sel = [w for w in allrows
           if w["preset"] == PRESET and int(w["goal"]) == GOAL
           # C1: a blank cell is a row written before the column existed,
           # and every one of those was read against medium.
           and (w.get("x265_preset") or "medium") == X265PRESET]
    if not sel:
        sys.exit(f"goal_legs.py: no rows for preset {PRESET} goal {GOAL} vs "
                 f"x265 {X265PRESET} in {os.path.relpath(CSV_PATH, ROOT)}")
    arm = [w for w in sel if (w.get("arm") or "") == ARM]
    if not arm:
        sys.exit(f"goal_legs.py: no rows for preset {PRESET} goal {GOAL} at "
                 f"arm {ARM!r} in {os.path.relpath(CSV_PATH, ROOT)}; arms "
                 f"present: "
                 f"{sorted({(w.get('arm') or '') for w in sel})}")
    sel = arm
    last = (sel[-1]["date"], sel[-1]["commit"], sel[-1].get("arm") or "")
    sel = [w for w in sel
           if (w["date"], w["commit"], w.get("arm") or "") == last]
    print(f"from {os.path.relpath(CSV_PATH, ROOT)}: {len(sel)} row(s), "
          f"date {last[0]} commit {last[1]} arm "
          f"{last[2] if last[2] else '(default)'}")
    report(sel, GOAL, PRESET, int(sel[0]["rung"]), int(sel[0]["frames"]),
           sel[0].get("x265_preset") or "medium")


if __name__ == "__main__":
    main()
