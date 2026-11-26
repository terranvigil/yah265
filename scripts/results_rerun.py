#!/usr/bin/env python3
# Copyright (c) 2026, the yah265 authors
# SPDX-License-Identifier: GPL-2.0-or-later
"""Rerun the Mac's results from scratch: the parity harness, its report,
then every page script and the strict site build. `make results` runs it.

    scripts/results_rerun.py              run every step in order
    scripts/results_rerun.py --dry-run    check every step resolves, print
                                          the plan and the harness's own
                                          estimate; nothing is encoded
    scripts/results_rerun.py --list       number the steps
    scripts/results_rerun.py --from N     start at step N (after a stop)

The harness takes many hours of box time. Its timed steps take the shared
timing lock and wait for a quiet box themselves. Its untimed steps wait
out anyone else's timed leg. The run stops at the first step that fails.

The x86 and Graviton rows come from cloud runs this doesn't repeat. The
all-architecture summary and the launch post's prose are written by hand
from the reports. The page scripts check the new data against both, and
against the builds blog_charts.OURS_BIN and REF_BIN name as the ones whose
ladders count. After a rerun on a new binary those checks stop the site
steps and name what disagrees. Fix them, then resume with --from.
"""
import argparse
import os
import shlex
import shutil
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
PS = "scripts/parity_spec.py"

# (what it does, the command); the commands run from the repository root
STEPS = [
    ("build the release binary", ["make", "build-rel"]),
    ("match tier S on size and score it", [PS, "--solve", "--tier", "S"]),
    ("match tier M", [PS, "--solve", "--tier", "M"]),
    ("match tier L", [PS, "--solve", "--tier", "L"]),
    ("match tier C", [PS, "--solve", "--tier", "C"]),
    ("match the G1 and G2 windows", [PS, "--windows"]),
    ("the CRF and VBR ladders", [PS, "--ladder"]),
    ("the lookahead sweep", [PS, "--sweep"]),
    ("the end-credits rows", [PS, "--credits"]),
    ("time tier S", [PS, "--time", "--tier", "S"]),
    ("time tier M", [PS, "--time", "--tier", "M"]),
    ("time tier L", [PS, "--time", "--tier", "L"]),
    ("time tier C", [PS, "--time", "--tier", "C"]),
    ("time the G1 and G2 windows", [PS, "--time-windows"]),
    ("time the VBR leg", [PS, "--vbr-wall"]),
    ("write the Mac report", [PS, "--report"]),
    ("fill the x265 commands, CRF map and corpus table", ["scripts/site_trust.py"]),
    ("fill the results page", ["scripts/charts_results.py"]),
    ("fill the launch post", ["scripts/blog_charts.py"]),
    ("fill the home and options pages", ["scripts/site_front.py"]),
    ("fill the architecture charts", ["scripts/charts_internals.py"]),
    ("draw the share cards", ["scripts/share_cards.py"]),
    ("build the site and run every check", ["scripts/site.sh", "--strict"]),
]
FIRST_SITE = next(i for i, (_, c) in enumerate(STEPS, 1) if c[0] != PS and c[0] != "make")


def show(i, what, cmd):
    print(f"{i:2}. {what}\n      {shlex.join(cmd)}")


def argv(cmd):
    # not every page script is executable, so python runs them all
    return [sys.executable] + cmd if cmd[0].endswith(".py") else cmd


def resolves(cmd):
    """None when the step would start, else why it wouldn't."""
    if cmd[0] == "make":
        r = subprocess.run(["make", "-n"] + cmd[1:], cwd=ROOT, capture_output=True, text=True)
        return None if r.returncode == 0 else f"make -n {cmd[1]} failed: {r.stderr.strip()}"
    if not os.path.isfile(os.path.join(ROOT, cmd[0])):
        return f"{cmd[0]} is missing"
    if cmd[0].endswith(".sh"):
        return None if os.access(os.path.join(ROOT, cmd[0]), os.X_OK) else f"{cmd[0]} isn't executable"
    r = subprocess.run(argv([cmd[0], "--help"]), cwd=ROOT, capture_output=True, text=True)
    if r.returncode:
        return f"{cmd[0]} --help exited {r.returncode}: {r.stderr.strip()[-300:]}"
    unknown = [f for f in cmd[1:] if f.startswith("--") and f not in r.stdout.split()]
    return f"{cmd[0]} --help has no {', '.join(unknown)}" if unknown else None


def pins():
    """The pinned binaries against the ones the harness would run."""
    sys.path.insert(0, os.path.join(ROOT, "scripts"))
    import blog_charts as bc
    from bd_at_rate import _bin_hash
    import parity_spec as ps
    out = []
    for name, exe, pin in (("ours", ps.N265, bc.OURS_BIN), ("x265", ps.X265, bc.REF_BIN)):
        found = shutil.which(exe) or exe
        h = _bin_hash(exe) if os.path.isfile(found) else "not built"
        out.append(f"{name}: {exe} is {h}, blog_charts pins {pin}"
                   + ("" if h == pin else " (the site steps will stop until the pin moves)"))
    return out


def dry_run(start):
    bad = 0
    for i, (what, cmd) in enumerate(STEPS, 1):
        if i < start:
            continue
        show(i, what, cmd)
        why = resolves(cmd)
        print(f"      {'ok' if why is None else 'FAILS: ' + why}")
        bad += why is not None
    print("\nbinary pins:")
    for line in pins():
        print("  " + line)
    if start <= FIRST_SITE - 1:
        print(f"\nthe harness's own plan ({PS} --plan), summary:")
        r = subprocess.run(argv([PS, "--plan"]), cwd=ROOT, capture_output=True, text=True)
        if r.returncode:
            print(r.stdout[-2000:] + r.stderr[-2000:])
            bad += 1
        else:
            lines = r.stdout.splitlines()
            missing = [x for x in lines if "source missing" in x]
            tail = next((n for n in range(len(lines)) if lines[n].startswith("## ")), len(lines))
            for x in missing + lines[tail:]:
                print("  " + x)
            bad += bool(missing)
    print(f"\n{len(STEPS) - start + 1} steps checked, {bad} problem(s)")
    return 1 if bad else 0


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--dry-run", action="store_true",
                    help="check that every step resolves and print the plan; run nothing")
    ap.add_argument("--list", action="store_true", help="number the steps and stop")
    ap.add_argument("--from", dest="start", type=int, default=1, metavar="N",
                    help="start at step N")
    a = ap.parse_args()
    if not 1 <= a.start <= len(STEPS):
        sys.exit(f"--from takes 1 to {len(STEPS)}")
    if a.list:
        for i, (what, cmd) in enumerate(STEPS, 1):
            show(i, what, cmd)
        return 0
    if a.dry_run:
        return dry_run(a.start)
    for i, (what, cmd) in enumerate(STEPS, 1):
        if i < a.start:
            continue
        print(f"\n=== step {i} of {len(STEPS)}: {what}\n    {shlex.join(cmd)}", flush=True)
        r = subprocess.run(argv(cmd), cwd=ROOT)
        if r.returncode:
            print(f"\nstep {i} failed with exit {r.returncode}. Resume with "
                  f"scripts/results_rerun.py --from {i}", file=sys.stderr)
            return r.returncode
    print("\nthe results page is rebuilt from the new data")
    return 0


if __name__ == "__main__":
    sys.exit(main())
