#!/usr/bin/env python3
# Copyright (c) 2026, the yah265 authors
# SPDX-License-Identifier: GPL-2.0-or-later
"""BD-rate (VMAF-NEG) of our ABR against x265 medium ABR at the same targets.

scripts/bd_at_rate.py bisects a QP-like knob onto a byte target; ABR is told
the rate, so there is nothing to bisect. Both sides are simply run at the four
ladder rates and BD is taken over the resulting (bytes, VMAF-NEG) pairs, which
is the same estimator on the same ladder.

    JOBS=6 python3 scripts/abr_bd.py
    OURS='--bitrate {k} --aq 1 --cutree 1' CLIPS=foreman_cif python3 scripts/abr_bd.py
"""
import json, os, re, statistics, subprocess, sys, tempfile
from concurrent.futures import ThreadPoolExecutor

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
HERE = os.path.join(ROOT, "scripts")
sys.path.insert(0, HERE)
import bdcompare as bd
bd.SUBSAMPLE = 1

FRAMES = int(os.environ.get("FRAMES", "120"))
N = os.environ.get("N265", os.path.join(ROOT, "build", "n265"))
X = os.environ.get("X265", "x265")
OURS = os.environ.get("OURS", "--bitrate {k} --aq 1 --cutree 1")
JOBS = int(os.environ.get("JOBS", "6"))
ladders = json.load(open(os.path.join(HERE, "ladders.json")))
sel = os.environ.get("CLIPS", "")
clips = sel.split(",") if sel else list(ladders)
WORK = tempfile.mkdtemp(prefix="abrbd_")


def cut(clip):
    full = os.path.join(ROOT, "tests", "corpus", clip + ".y4m")
    dst = os.path.join(WORK, clip + ".y4m")
    if not os.path.exists(dst):
        bd.sh(f'ffmpeg -v error -y -i "{full}" -frames:v {FRAMES} "{dst}"')
    return dst


def enc(clip, kbps, side):
    src = cut(clip)
    out = os.path.join(WORK, f"{clip}_{side}_{kbps}.hevc")
    if side == "a":
        cmd = [N, src, "--frames", str(FRAMES)] + \
            OURS.format(k=kbps).split() + ["-o", out]
    else:
        cmd = [X, "--preset", "medium", "--bitrate", str(kbps), "--frames",
               str(FRAMES), "--pools", "none", "--frame-threads", "1",
               "--log-level", "none", "-o", out, src]
    subprocess.run(cmd, capture_output=True)
    m = bd.vmaf_of(out, src, WORK)
    return clip, side, kbps, os.path.getsize(out), m["VMAF-NEG"]


jobs = [(c, k, s) for c in clips for k in ladders[c] for s in ("a", "b")]
rows = []
with ThreadPoolExecutor(max_workers=JOBS) as ex:
    for r in ex.map(lambda a: enc(*a), jobs):
        rows.append(r)

res = {}
for clip in clips:
    pts = {}
    for side in ("a", "b"):
        rs = sorted([r for r in rows if r[0] == clip and r[1] == side],
                    key=lambda r: r[2])
        pts[side] = ([r[3] for r in rs], [r[4] for r in rs])
        print(f"{clip:18} {'ours' if side=='a' else 'x265'}: " +
              " ".join(f"{s//1000}k/{v:.2f}" for s, v in zip(*pts[side])))
    b = bd.bd_rate(*pts["b"], *pts["a"])
    if b is not None:
        res[clip] = b
    print(f"{clip:18} BD-rate(VMAF-NEG) ours vs x265 ABR: " +
          (f"{b:+.2f}%" if b is not None else "no quality overlap"))

v = list(res.values())
if v:
    print("\n=== ABR BD-rate(VMAF-NEG) ours vs x265 medium ABR, same targets ===")
    for c in clips:
        if c in res:
            print(f"  {c:<18} {res[c]:+7.2f}%")
    print(f"  median {statistics.median(v):+.2f}%   mean {statistics.mean(v):+.2f}%"
          f"   worst {max(v):+.2f}%   ahead {sum(1 for x in v if x < 0)}/{len(v)}")
