#!/usr/bin/env python3
# Copyright (c) 2026, the yah265 authors
# SPDX-License-Identifier: GPL-2.0-or-later
"""Interleaved wall A/B between TWO BINARIES, for a change no env knob
gates (a refactor, a layout change, a compiler flag).

Ported from yah264's bench/bin_ab.py; the loop and its rules are in
scripts/abtime.py. Both binaries run in every round with a rotated start,
and a second run of the reference rides along as `ctrl`.

  REF=../yah265-ref/build-rel/n265 NEW=build-rel/n265 python3 scripts/bin_ab.py
  CELLS=allI,foreman,pjoy RUNS=7 python3 scripts/bin_ab.py

Two traps it exists to avoid, both of which cost yah264 a round:
  - a reference tree set up with plain `meson setup` is debugoptimized
    (-O2) while build-rel is release (-O3), which reads as a 2-3% "win"
    no code change made. Both trees' meson optimization level is printed
    and the run REFUSES when they differ (FORCE_OPT=1 overrides, for the
    day the flag itself is the change).
  - md5 identity is the gate for a change that claims to be
    byte-identical, so it is checked per cell, not argued.

Environment: REF (default build-ref/n265), NEW (default build-rel/n265),
CELLS (default allI,foreman,mobile,pjoy), THREADS 1, RUNS 5, SPREAD 1.15,
PRESET medium, ASM 1 (0 = N265_ASM_OFF=all on both), ARGS (CLI flags for
both), COMMON (NAME=VALUE for both), ARGS_NEW (flags for NEW only, e.g.
--no-info against an older reference), FRAMES, QUIET_GATE=1. Every
inherited N265_* is cleared first. The run holds the timing lock.
"""
import json
import os
import shlex
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import abtime                                             # noqa: E402

ROOT = abtime.ROOT


def optlevel(binary):
    """meson's optimization level for the tree a binary was built in: the
    binary's own directory or its parent's (n265 sits at the tree root,
    a test binary one level down)."""
    d = os.path.dirname(os.path.abspath(binary))
    for cand in (d, os.path.dirname(d)):
        p = os.path.join(cand, "meson-info", "intro-buildoptions.json")
        if os.path.exists(p):
            opts = {o["name"]: o["value"] for o in json.load(open(p))}
            return str(opts.get("optimization", "?")), \
                str(opts.get("buildtype", "?"))
    return "?", "?"


def main():
    if "--help" in sys.argv or "-h" in sys.argv:
        print(__doc__)
        return
    bad = [a for a in sys.argv[1:] if a != "--no-md5"]
    if bad:
        sys.exit(f"bin_ab.py: unknown argument(s) {bad}; see --help")
    ref = os.environ.get("REF", os.path.join(ROOT, "build-ref/n265"))
    new = os.environ.get("NEW", os.path.join(ROOT, "build-rel/n265"))
    for b in (ref, new):
        if not os.access(b, os.X_OK):
            sys.exit(f"bin_ab.py: not an executable: {b}")
    (ro, rt), (no, nt) = optlevel(ref), optlevel(new)
    if ro != no and os.environ.get("FORCE_OPT") != "1":
        sys.exit(f"bin_ab.py: build options differ: ref optimization={ro} "
                 f"({rt}), new optimization={no} ({nt}). Reconfigure the "
                 f"reference (meson configure -Dbuildtype={nt}) before "
                 f"believing any number, or FORCE_OPT=1 if the flag is the "
                 f"change.")

    threads = os.environ.get("THREADS", "1")
    runs = int(os.environ.get("RUNS", "5"))
    spread = float(os.environ.get("SPREAD", "1.15"))
    preset = os.environ.get("PRESET", "medium")
    cells = os.environ.get("CELLS", "allI,foreman,mobile,pjoy").split(",")
    asm = os.environ.get("ASM", "1") == "1"
    env0, gone = abtime.strip_knobs(dict(os.environ))
    cenv, cargs = abtime.parse_arm(os.environ.get("COMMON", ""))
    if cargs:
        sys.exit(f"bin_ab.py: COMMON takes NAME=VALUE only, got {cargs}")
    env0.update(cenv)
    if not asm:
        env0["N265_ASM_OFF"] = "all"
    args = shlex.split(os.environ.get("ARGS", ""))
    args_new = shlex.split(os.environ.get("ARGS_NEW", ""))

    arms = [("ref", ref, env0, args), ("ctrl", ref, env0, args),
            ("new", new, env0, args + args_new)]
    print(f"-- ref {ref}  (optimization={ro}, {rt})")
    print(f"-- new {new}  (optimization={no}, {nt})")
    print(f"-- threads {threads} runs {runs} preset {preset} tier "
          f"{'as shipped' if asm else 'pure C (N265_ASM_OFF=all)'}"
          f"{'  args ' + ' '.join(args) if args else ''}"
          f"{'  new also ' + ' '.join(args_new) if args_new else ''}")
    if gone:
        print(f"-- cleared from the environment: {' '.join(gone)}")
    abtime.preflight("bin_ab.py")
    abtime.run(arms, cells, threads, preset, runs, spread,
               "--no-md5" not in sys.argv, "bin_ab.py", base="ref")
    abtime.legend("ref")


if __name__ == "__main__":
    main()
