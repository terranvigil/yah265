#!/usr/bin/env python3
# Copyright (c) 2026, the yah265 authors
# SPDX-License-Identifier: GPL-2.0-or-later
"""BD-rate (VMAF-NEG) of one of our configurations against x265 medium at
MATCHED ACHIEVED BITRATE on the band ladders (scripts/ladders.json): the
quality leg the way yah264 reads it. ARGS = our extra flags; CLIPS, FRAMES,
JOBS as band_at_rate.py.

    ARGS='--aq 1 --cutree 1' python3 scripts/bd_vs_x265.py
    ARGS='--crf {q} --aq 1 --cutree 1' KNOB=crf python3 scripts/bd_vs_x265.py
"""
import json, os, re, statistics, subprocess, sys
from concurrent.futures import ThreadPoolExecutor
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
HERE = os.path.join(ROOT, "scripts")
FRAMES = int(os.environ.get("FRAMES", "120"))
N = os.environ.get("N265", "build/n265")
X = os.environ.get("X265", "x265")
ARGS = os.environ.get("ARGS", "--aq 1 --cutree 1")
KNOB = os.environ.get("KNOB", "qp")            # qp | crf: which knob bd_at_rate bisects
JOBS = int(os.environ.get("JOBS", "4"))
ladders = json.load(open(os.path.join(HERE, "ladders.json")))
sel = os.environ.get("CLIPS", "")
clips = sel.split(",") if sel else list(ladders)
def fps_of(clip):
    with open(os.path.join(ROOT, "tests", "corpus", clip + ".y4m"), "rb") as f:
        hdr = f.readline().decode("latin-1")
    m = re.search(r" F(\d+):(\d+)", hdr)
    return int(m.group(1)) / int(m.group(2)) if m else 30.0
def targets_of(clip):
    secs = FRAMES / fps_of(clip)
    return [int(kbps * 1000.0 / 8.0 * secs) for kbps in ladders[clip]]
ours = f"{N} {{src}} --frames {FRAMES} " + (ARGS if "{q}" in ARGS else f"--{KNOB} {{q}} {ARGS}") + " -o {out}"
ref = f"{X} --preset medium --crf {{q}} --frames {FRAMES} --pools none --frame-threads 1 --log-level none -o {{out}} {{src}}"
def run(clip):
    cmd = ["python3", "scripts/bd_at_rate.py", "--clip", clip, "--frames", str(FRAMES),
           "--targets", ",".join(str(t) for t in targets_of(clip)),
           "--label-a", "ours", "--label-b", "x265", "--a", ours, "--b", ref]
    r = subprocess.run(cmd, cwd=ROOT, capture_output=True, text=True)
    return clip, r.stdout + r.stderr
print(f"ours: {ours}\nx265: {ref}\n")
res = {}
with ThreadPoolExecutor(max_workers=JOBS) as ex:
    for clip, out in ex.map(run, clips):
        print(out.rstrip()); m = re.search(r"BD-rate\(VMAF-NEG\) ours vs x265: ([+-][\d.]+)%", out)
        if m: res[clip] = float(m.group(1))
v = list(res.values())
if v:
    print(f"\n=== BD-rate(VMAF-NEG) ours vs x265 medium at matched achieved bitrate ===")
    for c in clips:
        if c in res: print(f"  {c:<18} {res[c]:+7.2f}%")
    print(f"  median {statistics.median(v):+.2f}%   mean {statistics.mean(v):+.2f}%   worst {max(v):+.2f}%   ahead {sum(1 for x in v if x < 0)}/{len(v)}")
