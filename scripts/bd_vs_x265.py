#!/usr/bin/env python3
# Copyright (c) 2026, the yah265 authors
# SPDX-License-Identifier: GPL-2.0-or-later
"""BD-rate (VMAF-NEG) of one of our configurations against x265 medium at
MATCHED ACHIEVED BITRATE on the band ladders (scripts/ladders.json): the
quality leg the way yah264 reads it. ARGS = our extra flags; CLIPS, FRAMES,
JOBS as band_at_rate.py.

    ARGS='--aq 1 --cutree 1' python3 scripts/bd_vs_x265.py
    ARGS='--crf {q} --aq 1 --cutree 1' KNOB=crf python3 scripts/bd_vs_x265.py

DEPTH=10 runs the same leg at Main10: our `build/n265` on the upshifted
corpus in `local/corpus10/` (scripts/upshift10.sh), x265 with
`--output-depth 10`, and both sides scored on 10-bit planes against the
10-bit source. SRCDIR/N265 override the two paths it picks.

A second column carries BD-rate on chroma PSNR off the same points: the metric
is luma-only, so that column is the only thing that says whether an arm bought
its luma number out of the chroma planes. The rule and the CIF-7 baseline are
in docs/instruments.md under the band tool.
"""
import json, os, re, statistics, subprocess, sys
from concurrent.futures import ThreadPoolExecutor
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
HERE = os.path.join(ROOT, "scripts")
FRAMES = int(os.environ.get("FRAMES", "120"))
# DEPTH=10 puts the whole leg at Main10: our binary is the 10-bit one, the
# source is the upshifted corpus scripts/upshift10.sh writes, x265 is the
# same brew build asked for a Main10 stream, and the metrics read 10-bit
# planes on both sides (bd_at_rate --depth). docs/instruments.md, "The band
# at 10 bits". Nothing about the 8-bit leg moves.
DEPTH = int(os.environ.get("DEPTH", "8"))
SRCDIR = os.environ.get("SRCDIR", "tests/corpus" if DEPTH == 8
                        else "local/corpus10")
N = os.environ.get("N265", "build/n265")   # one binary, both depths (item 5a)
X = os.environ.get("X265", "x265")
ARGS = os.environ.get("ARGS", "--aq 1 --cutree 1")
KNOB = os.environ.get("KNOB", "qp")            # qp | crf: which knob bd_at_rate bisects
JOBS = int(os.environ.get("JOBS", "4"))
ladders = json.load(open(os.path.join(HERE, "ladders.json")))
sel = os.environ.get("CLIPS", "")
clips = sel.split(",") if sel else list(ladders)
def fps_of(clip):
    with open(os.path.join(ROOT, SRCDIR, clip + ".y4m"), "rb") as f:
        hdr = f.readline().decode("latin-1")
    m = re.search(r" F(\d+):(\d+)", hdr)
    return int(m.group(1)) / int(m.group(2)) if m else 30.0
def targets_of(clip):
    secs = FRAMES / fps_of(clip)
    return [int(kbps * 1000.0 / 8.0 * secs) for kbps in ladders[clip]]
ours = f"{N} {{src}} --frames {FRAMES} " + (ARGS if "{q}" in ARGS else f"--{KNOB} {{q}} {ARGS}") + " -o {out}"
# x265 reads the depth of a y4m off its header, so --input-depth is a
# no-op here and is written anyway to say what the arm is; --output-depth
# is the one that picks the Main10 encoder out of the 8+10+12 brew build.
xdepth = "" if DEPTH == 8 else f" --input-depth {DEPTH} --output-depth {DEPTH}"
ref = (f"{X} --preset medium --crf {{q}} --frames {FRAMES} --pools none "
       f"--frame-threads 1 --log-level none{xdepth} -o {{out}} {{src}}")
def run(clip):
    cmd = ["python3", "scripts/bd_at_rate.py", "--clip", clip, "--frames", str(FRAMES),
           "--targets", ",".join(str(t) for t in targets_of(clip)),
           "--srcdir", SRCDIR, "--depth", str(DEPTH),
           "--label-a", "ours", "--label-b", "x265", "--a", ours, "--b", ref]
    r = subprocess.run(cmd, cwd=ROOT, capture_output=True, text=True)
    return clip, r.stdout + r.stderr
print(f"depth: {DEPTH}  src: {SRCDIR}\nours: {ours}\nx265: {ref}\n")
res = {}
chroma = {}
with ThreadPoolExecutor(max_workers=JOBS) as ex:
    for clip, out in ex.map(run, clips):
        print(out.rstrip()); m = re.search(r"BD-rate\(VMAF-NEG\) ours vs x265: ([+-][\d.]+)%", out)
        if m: res[clip] = float(m.group(1))
        # The chroma guard (docs/phase7-psy-design.md M3): VMAF is luma-only,
        # so this column is what says whether an arm bought its luma number
        # out of the chroma planes. A reading, never the flip.
        mc = re.search(r"chromaPSNR ([+-][\d.]+)%", out)
        if mc: chroma[clip] = float(mc.group(1))
v = list(res.values())
if v:
    tag = "" if DEPTH == 8 else f" (Main10, {DEPTH}-bit)"
    print(f"\n=== BD-rate(VMAF-NEG) ours vs x265 medium at matched achieved bitrate{tag} ===")
    for c in clips:
        if c in res:
            cs = f"   chromaPSNR {chroma[c]:+7.2f}%" if c in chroma else ""
            print(f"  {c:<18} {res[c]:+7.2f}%{cs}")
    print(f"  median {statistics.median(v):+.2f}%   mean {statistics.mean(v):+.2f}%   worst {max(v):+.2f}%   ahead {sum(1 for x in v if x < 0)}/{len(v)}")
    cv = [chroma[c] for c in clips if c in chroma]
    if cv:
        print(f"  chromaPSNR   median {statistics.median(cv):+.2f}%   mean {statistics.mean(cv):+.2f}%   worst {max(cv):+.2f}%   ahead {sum(1 for x in cv if x < 0)}/{len(cv)}")
