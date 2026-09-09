#!/usr/bin/env python3
# Copyright (c) 2026, the yah265 authors
# SPDX-License-Identifier: GPL-2.0-or-later
"""How close a CRF solve can get to a byte target -- the dSIZE leg read as a
property of the CONTROL VARIABLE rather than of the encoder.

The band solves an integer CRF onto each ladder target, and one integer step
moves the achieved rate 5-15%, so the leg has never had a number. With
N265_QP_FRAC=1 the base QP carries its fraction and is spent over the CTUs, so
CRF becomes continuous; this bisects it (to 0.01 CRF) onto each target and
reports the achieved/target ratio it can reach.

    N265_QP_FRAC=1 CLIPS=foreman_cif python3 scripts/crf_size.py
    N265_QP_FRAC=0 python3 scripts/crf_size.py     # the integer control, for contrast
"""
import json, os, re, statistics, subprocess, tempfile
from concurrent.futures import ThreadPoolExecutor

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
HERE = os.path.join(ROOT, "scripts")
FRAMES = int(os.environ.get("FRAMES", "120"))
N = os.environ.get("N265", os.path.join(ROOT, "build", "n265"))
ARGS = os.environ.get("ARGS", "--aq 1 --cutree 1")
JOBS = int(os.environ.get("JOBS", "6"))
STEPS = int(os.environ.get("STEPS", "12"))
ladders = json.load(open(os.path.join(HERE, "ladders.json")))
sel = os.environ.get("CLIPS", "")
clips = sel.split(",") if sel else list(ladders)


def fps_of(clip):
    with open(os.path.join(ROOT, "tests", "corpus", clip + ".y4m"), "rb") as f:
        hdr = f.readline().decode("latin-1")
    m = re.search(r" F(\d+):(\d+)", hdr)
    return int(m.group(1)) / int(m.group(2)) if m else 30.0


def size_at(clip, crf):
    src = os.path.join(ROOT, "tests", "corpus", clip + ".y4m")
    fd, out = tempfile.mkstemp(suffix=".hevc"); os.close(fd)
    subprocess.run([N, src, "--frames", str(FRAMES), "--crf", "%.4f" % crf] +
                   ARGS.split() + ["-o", out], capture_output=True)
    s = os.path.getsize(out)
    os.unlink(out)
    return s


def one(clip, kbps):
    target = kbps * 1000.0 / 8.0 * (FRAMES / fps_of(clip))
    lo, hi = 0.0, 51.0                  # size is monotone DECREASING in crf
    n = 0
    best = None
    while n < STEPS:
        mid = (lo + hi) / 2
        s = size_at(clip, mid)
        if best is None or abs(s / target - 1) < abs(best[1] / target - 1):
            best = (mid, s)
        if s > target:
            lo = mid
        else:
            hi = mid
        n += 1
    return clip, kbps, best[0], best[1] / target


jobs = [(c, k) for c in clips for k in ladders[c]]
rows = []
with ThreadPoolExecutor(max_workers=JOBS) as ex:
    for r in ex.map(lambda a: one(*a), jobs):
        rows.append(r)
        print("  %-18s %8d kbps  crf %6.3f  achieved/target %6.4f"
              % r, flush=True)

err = [abs(r[3] - 1) * 100 for r in rows]
print("\n=== CRF solve onto the ladder targets (%s, N265_QP_FRAC=%s) ==="
      % (ARGS, os.environ.get("N265_QP_FRAC", "unset")))
for c in clips:
    rs = sorted([r for r in rows if r[0] == c], key=lambda r: r[1])
    print("| %s | %s | %.2f%% |" % (c, " | ".join("%.4f" % r[3] for r in rs),
                                    max(abs(r[3] - 1) * 100 for r in rs)))
print("per-point |error|: median %.2f%%  worst %.2f%%  within 1%%: %d/%d"
      % (statistics.median(err), max(err),
         sum(1 for x in err if x <= 1.0), len(err)))
