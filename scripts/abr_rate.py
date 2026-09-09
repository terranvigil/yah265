#!/usr/bin/env python3
# Copyright (c) 2026, the yah265 authors
# SPDX-License-Identifier: GPL-2.0-or-later
"""ABR rate accuracy: achieved/target at the band ladder's four points.

The dSIZE leg's instrument. For every clip in scripts/ladders.json, encode
FRAMES frames at each ladder rate with `--bitrate <kbps>` and report the
achieved size against the target. `opening` is the same ratio over the first
OPEN frames in CODING order, read from the encoder's own per-frame accounting
(N265_RC_TRACE), which is what says whether the lookahead-fitted opening is
doing its job.

    CLIPS=foreman_cif,bus_cif JOBS=6 python3 scripts/abr_rate.py
    ARGS='--aq 1 --cutree 1' python3 scripts/abr_rate.py
"""
import json, os, re, statistics, subprocess, sys, tempfile
from concurrent.futures import ThreadPoolExecutor

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
HERE = os.path.join(ROOT, "scripts")
FRAMES = int(os.environ.get("FRAMES", "120"))
OPEN = int(os.environ.get("OPEN", "30"))
N = os.environ.get("N265", os.path.join(ROOT, "build", "n265"))
ARGS = os.environ.get("ARGS", "--aq 1 --cutree 1")
JOBS = int(os.environ.get("JOBS", "6"))
ladders = json.load(open(os.path.join(HERE, "ladders.json")))
sel = os.environ.get("CLIPS", "")
clips = sel.split(",") if sel else list(ladders)


def fps_of(clip):
    with open(os.path.join(ROOT, "tests", "corpus", clip + ".y4m"), "rb") as f:
        hdr = f.readline().decode("latin-1")
    m = re.search(r" F(\d+):(\d+)", hdr)
    return int(m.group(1)) / int(m.group(2)) if m else 30.0


def one(clip, kbps):
    fps = fps_of(clip)
    src = os.path.join(ROOT, "tests", "corpus", clip + ".y4m")
    fd, out = tempfile.mkstemp(suffix=".hevc"); os.close(fd)
    env = dict(os.environ, N265_RC_TRACE="1")
    cmd = [N, src, "--frames", str(FRAMES), "--bitrate", str(kbps)] + \
        ARGS.split() + ["-o", out]
    r = subprocess.run(cmd, capture_output=True, text=True, env=env)
    size = os.path.getsize(out)
    os.unlink(out)
    bits = [float(m.group(1))
            for m in re.finditer(r"^RC done .*? bits=(\d+)", r.stderr, re.M)]
    # the clip may be shorter than FRAMES (stefan_cif is 90), so the target
    # is what the encoder was actually asked to fill, not what was requested
    nf = len(bits) if bits else FRAMES
    target = kbps * 1000.0 / 8.0 * (nf / fps)
    bpf = kbps * 1000.0 / fps
    opening = sum(bits[:OPEN]) / (bpf * min(OPEN, len(bits))) if bits else float("nan")
    return clip, kbps, size / target, opening


jobs = [(c, k) for c in clips for k in ladders[c]]
rows = []
with ThreadPoolExecutor(max_workers=JOBS) as ex:
    for res in ex.map(lambda a: one(*a), jobs):
        rows.append(res)
        print("  %-18s %8d kbps  achieved/target %6.3f  opening %6.3f"
              % (res[0], res[1], res[2], res[3]), flush=True)

print("\n=== ABR rate accuracy, %d frames, %s ===" % (FRAMES, ARGS))
print("| clip | " + " | ".join(str(i + 1) for i in range(4)) +
      " | worst |err| | opening worst |")
allerr = []
for c in clips:
    rs = [r for r in rows if r[0] == c]
    rs.sort(key=lambda r: r[1])
    e = [abs(r[2] - 1) * 100 for r in rs]
    o = [abs(r[3] - 1) * 100 for r in rs]
    allerr += e
    print("| %s | %s | %+.2f%% | %+.2f%% |" %
          (c, " | ".join("%.3f" % r[2] for r in rs), max(e), max(o)))
if allerr:
    print("\nper-point |error|: median %.2f%%  mean %.2f%%  worst %.2f%%  "
          "within 1%%: %d/%d" % (statistics.median(allerr),
                                 statistics.mean(allerr), max(allerr),
                                 sum(1 for x in allerr if x <= 1.0), len(allerr)))
    ao = [abs(r[3] - 1) * 100 for r in rows]
    print("opening (first %d frames) |error|: median %.2f%%  worst %.2f%%"
          % (OPEN, statistics.median(ao), max(ao)))
