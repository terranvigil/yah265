#!/usr/bin/env python3
# Copyright (c) 2026, the yah265 authors
# SPDX-License-Identifier: GPL-2.0-or-later
"""The plan's goal legs, read at ONE pinned MATCHED-SIZE operating point.

docs/plan.md "Goals and legs" defines three goals against x265 4.3
`--preset medium`, each read at one matched operating point and each
passing four legs:

    median speed     <= 1.00x of x265's wall
    worst-clip speed  < 1.15x, the clip NAMED
    quality           not more than 0.5 dVMAF-NEG BELOW x265, at equal bytes
    compression       not more than 1.0% LARGER than x265, at equal quality

The last two are ONE-SIDED and each is read at its own matched axis; see
"the legs" below for why both of those words are load-bearing.

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

    PRESET  medium | fast | slow ...  (default medium: the plan's goal preset)
    GOAL    1 | 2 | 3                 (default 3)
    CLIPS   comma list                (default: every clip with a ladder)
    RUNG    1..4                      (default 2: which ladder rung is the pin)
    FRAMES  120       NRUNS  3        JOBS  3 (clips at a time, SOLVE only)
    N265 / X265 / BAND_N265 / BD_CACHE
    LOADMAX 3.0       LOADWAIT 600    (the quiet-box bar, and how long to
                                       wait for the box to reach it)

Flags: --help, --no-csv (do not append to docs/data/goal-legs.csv),
--no-time (solve and score only; the wall columns stay empty),
--from-csv (re-print a run's tables and legs out of the CSV, reading
nothing else: no encode, no metric, no stopwatch, so a row can be re-read
while the box belongs to someone else). Anything else is refused, the way
board.py refuses it.

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
"""
import csv
import datetime
import json
import math
import os
import re
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

_bd.SUBSAMPLE = 1
# bd_at_rate's memo key is the command string with {src} filled, and the
# band fills it with a path RELATIVE to the tree root (`bd_vs_x265.py`
# runs its children with cwd=ROOT). An absolute path here would be a
# different key for the same encode and would miss every entry the board
# has already paid for, so this instrument stands where the band stands.
os.chdir(ROOT)

PRESET = os.environ.get("PRESET", "medium")
GOAL = int(os.environ.get("GOAL", "3"))
FRAMES = int(os.environ.get("FRAMES", "120"))
NRUNS = int(os.environ.get("NRUNS", "3"))
JOBS = int(os.environ.get("JOBS", "3"))
RUNG = int(os.environ.get("RUNG", "2"))
SRCDIR = os.environ.get("SRCDIR", "tests/corpus")
X265 = os.environ.get("X265", "x265")
LOADMAX = float(os.environ.get("LOADMAX", "3.0"))
LOADWAIT = float(os.environ.get("LOADWAIT", "600"))
CSV_PATH = os.environ.get("GOAL_LEGS_CSV",
                          os.path.join(ROOT, "docs/data/goal-legs.csv"))

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
    template then IS the band's."""
    return [] if PRESET == "medium" else ["--preset", PRESET]


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
    """bd_vs_x265.py's `ref` template, verbatim."""
    return (f"{X265} --preset medium --crf {{q}} --frames {FRAMES} "
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
    cmd = [X265, "--preset", "medium", "--crf", str(q), "--frames",
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
    t0 = time.perf_counter()
    subprocess.run(cmd, env=env, stdout=subprocess.DEVNULL,
                   stderr=subprocess.DEVNULL, check=True)
    return time.perf_counter() - t0


def solve_clip(clip, target):
    """Both sides bisected onto `target` bytes, with bytes and VMAF-NEG
    off the resulting bitstreams. Everything here is memoised."""
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
            out[tag] = {"q": q, "bytes": sz, "neg": neg}
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
          f"{'--threads 1' if t1 else 'auto threads'})   preset {PRESET}   "
          f"rung {RUNG}   {FRAMES} frames   commit {commit}")
    print(f"solve:  {our_tmpl()}")
    print(f"        {x265_tmpl()}")
    print(f"timed:  {TIMED_N265}   load {load1():.2f}")
    if missing:
        print(f"no ladder rung {RUNG}: {', '.join(missing)} (skipped)")

    targets = {c: target_of(c, ladders) for c in clips}
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
            r["ours"]["wall"] = statistics.median(ours)
            r["x265"]["wall"] = statistics.median(x)
            r["mult"] = r["ours"]["wall"] / r["x265"]["wall"]
            print(f"  {c:<18} ours {r['ours']['wall']:7.3f} s   x265 "
                  f"{r['x265']['wall']:7.3f} s   {r['mult']:5.2f}x",
                  flush=True)
        print(f"load after: {load1():.2f}")

    rows = []
    for c in clips:
        r = res[c]
        dsize = (r["ours"]["bytes"] / r["x265"]["bytes"] - 1.0) * 100.0
        dvmaf = r["ours"]["neg"] - r["x265"]["neg"]
        dv_eq, ds_eq, flags = legs_at_matched_axes(
            r["ours"]["bytes"], r["ours"]["neg"], ladder_points(x265_tmpl(),
                                                               r["src"]))
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
        })

    report(rows, GOAL, PRESET, RUNG, FRAMES)
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


def report(rows, goal, preset, rung, frames):
    hdr = (f"{'clip':<18} {'ourCRF':>6} {'xCRF':>5} {'dVMAF@=B':>9} "
           f"{'dSIZE@=V':>9} {'ours s':>8} {'x265 s':>8} {'mult':>7}")
    print(f"\n=== goal {goal}, preset {preset}, rung {rung}, {frames} frames, "
          f"{len(rows)} clips ===")
    print("    dVMAF@=B: ours minus x265's VMAF-NEG interpolated at OUR "
          "bytes (ours ahead positive)")
    print("    dSIZE@=V: ours over x265's bytes interpolated at OUR "
          "VMAF-NEG (ours larger positive)")
    print(hdr)
    print("-" * len(hdr))
    for w in rows:
        m = _f(w, "mult")
        ow, xw = _f(w, "our_wall_s"), _f(w, "x265_wall_s")
        dv, ds = _f(w, "dvmaf_eq_bytes"), _f(w, "dsize_eq_vmaf_pct")
        print(f"{w['clip']:<18} {int(w['our_crf']):>6} {int(w['x265_crf']):>5} "
              f"{f'{dv:+.2f}' if dv is not None else '-':>9} "
              f"{f'{ds:+.2f}' if ds is not None else '-':>9} "
              f"{f'{ow:.3f}' if ow is not None else '-':>8} "
              f"{f'{xw:.3f}' if xw is not None else '-':>8} "
              f"{f'{m:.2f}x' if m is not None else '-':>7}")

    print("\n--- the four legs (one-sided, as the plan writes them) ---")
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
            if fields != have:
                sys.exit(f"goal_legs.py: {CSV_PATH} is missing columns "
                         f"{[k for k in fields if k not in have]}; it predates "
                         f"them. Migrate the file (add the columns to the "
                         f"header and to every row) before appending.")
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
    the box belongs to somebody else."""
    with open(CSV_PATH, newline="") as f:
        allrows = list(csv.DictReader(f))
    sel = [w for w in allrows
           if w["preset"] == PRESET and int(w["goal"]) == GOAL]
    if not sel:
        sys.exit(f"goal_legs.py: no rows for preset {PRESET} goal {GOAL} in "
                 f"{os.path.relpath(CSV_PATH, ROOT)}")
    last = (sel[-1]["date"], sel[-1]["commit"])
    sel = [w for w in sel if (w["date"], w["commit"]) == last]
    print(f"from {os.path.relpath(CSV_PATH, ROOT)}: {len(sel)} row(s), "
          f"date {last[0]} commit {last[1]}")
    report(sel, GOAL, PRESET, int(sel[0]["rung"]), int(sel[0]["frames"]))


if __name__ == "__main__":
    main()
