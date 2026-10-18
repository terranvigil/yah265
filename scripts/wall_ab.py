#!/usr/bin/env python3
# Copyright (c) 2026, the yah265 authors
# SPDX-License-Identifier: GPL-2.0-or-later
"""Interleaved wall A/B for env- or flag-gated arms of ONE binary.

Ported from yah264's bench/wall_ab.py; the loop and its rules are in
scripts/abtime.py. The baseline is the binary's own shipped default, so
this measures a flip, not a build difference (scripts/bin_ab.py is that).

  ARM='N265_SKIP_EXIT=1' python3 scripts/wall_ab.py         # default vs escape
  ARM='N265_X=1 | N265_X=2 | --rdoq-level 0' CLIPS=foreman,pjoy python3 scripts/wall_ab.py
  ARM='...' python3 scripts/wall_ab.py --no-md5

Several arms separated by `|` run round-robin inside ONE batch: comparing
arm A from one invocation with arm B from the next reads the box's drift
as a difference between them. A token NAME=VALUE is an environment
setting, anything else a CLI flag for that arm.

Environment:
  ARM      the arm(s); required
  CLIPS    comma list of cells (default foreman,mobile); scripts/abtime.py
           has the table
  THREADS  1 (default)      RUNS 5      SPREAD 1.15     PRESET medium
  ASM      1 (default, as shipped) or 0 (N265_ASM_OFF=all on every arm)
  COMMON   NAME=VALUE settings for EVERY arm, base and ctrl included
  ARGS     CLI flags for every arm
  FRAMES   override every cell's frame count
  N265     binary (default build-rel/n265, else build/n265)
  QUIET_GATE=1   refuse unless the 3 x 10 s box-CPU check passes

Every N265_* in the caller's environment is cleared first: base is the
shipped default, and COMMON is the only way to set a knob on all arms.
md5 per cell is on by default (one untimed encode per arm): SAME says
the flip only moved the schedule, DIFFERS says the number needs a band
gate as well. The run holds the timing lock.
"""
import os
import shlex
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import abtime                                             # noqa: E402

ROOT = abtime.ROOT


def main():
    if "--help" in sys.argv or "-h" in sys.argv:
        print(__doc__)
        return
    bad = [a for a in sys.argv[1:] if a != "--no-md5"]
    if bad:
        sys.exit(f"wall_ab.py: unknown argument(s) {bad}; see --help")
    spec = os.environ.get("ARM", "")
    if not spec.strip():
        sys.exit("wall_ab.py: set ARM='N265_VAR=val [...] [| ...]', the "
                 "flip(s) to price")
    rel = os.path.join(ROOT, "build-rel/n265")
    binary = os.environ.get("N265", rel if os.path.exists(rel)
                            else os.path.join(ROOT, "build/n265"))
    threads = os.environ.get("THREADS", "1")
    runs = int(os.environ.get("RUNS", "5"))
    spread = float(os.environ.get("SPREAD", "1.15"))
    preset = os.environ.get("PRESET", "medium")
    cells = os.environ.get("CLIPS", "foreman,mobile").split(",")
    asm = os.environ.get("ASM", "1") == "1"

    env0, gone = abtime.strip_knobs(dict(os.environ))
    cenv, cargs = abtime.parse_arm(os.environ.get("COMMON", ""))
    if cargs:
        sys.exit(f"wall_ab.py: COMMON takes NAME=VALUE only, got {cargs}")
    env0.update(cenv)
    if not asm:
        env0["N265_ASM_OFF"] = "all"
    args = shlex.split(os.environ.get("ARGS", ""))

    specs = [t.strip() for t in spec.split("|") if t.strip()]
    arms = [("base", binary, env0, args), ("ctrl", binary, env0, args)]
    for i, sp in enumerate(specs):
        e, a = abtime.parse_arm(sp)
        arms.append((f"arm{i + 1}" if len(specs) > 1 else "arm", binary,
                     dict(env0, **e), args + a))

    print(f"-- {binary}")
    print(f"-- threads {threads} runs {runs} preset {preset} tier "
          f"{'as shipped' if asm else 'pure C (N265_ASM_OFF=all)'}"
          f"{'  common ' + os.environ['COMMON'] if cenv else ''}"
          f"{'  args ' + ' '.join(args) if args else ''}")
    if gone:
        print(f"-- cleared from the environment: {' '.join(gone)}")
    abtime.preflight("wall_ab.py")
    abtime.run(arms, cells, threads, preset, runs, spread,
               "--no-md5" not in sys.argv, "wall_ab.py")
    abtime.legend()


if __name__ == "__main__":
    main()
