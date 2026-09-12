#!/usr/bin/env python3
# Copyright (c) 2026, the yah265 authors
# SPDX-License-Identifier: GPL-2.0-or-later
"""What does coding SDR content at Main10 buy or cost, with THIS encoder?

Our 8-bit encoder on the 8-bit source against our 10-bit encoder on the
upshifted source, at matched achieved bytes on the band ladders, both
scored in the 8-BIT DOMAIN: one reference (the original 8-bit y4m), and
the Main10 arm's decode downshifted by the exact inverse of
scripts/upshift10.sh's left shift by two (`-sws_dither none`; dither is
noise the encoder did not write). Negative means the Main10 arm spends
fewer bits for the same 8-bit picture - the gain x265 users know as
Main10-for-SDR.

It is deliberately NOT bd_vs_x265.py with a flag. The band engine has one
source per clip and serves both arms from it; here the two arms read
different files and the METRIC reads a third thing, and folding that into
the engine's memo keys would put a trap in the instrument every later
item has to step over.

    CLIPS=akiyo_cif,foreman_cif JOBS=3 python3 scripts/bd_8v10.py
    KNOB=crf ARGS='--crf {q} --aq 1 --cutree 1' python3 scripts/bd_8v10.py

Env: N265 / N265_10 (the two binaries), SRC8 / SRC10 (the two corpus
directories), CLIPS, FRAMES, JOBS, KNOB, ARGS - as bd_vs_x265.py.
"""
import json, os, re, statistics, sys, tempfile
from concurrent.futures import ThreadPoolExecutor

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "scripts"))
import bdcompare as bd
import bd_at_rate as bar

FRAMES = int(os.environ.get("FRAMES", "120"))
N8 = os.environ.get("N265", "build/n265")
N10 = os.environ.get("N265_10", "build10/n265-10")
SRC8 = os.environ.get("SRC8", "tests/corpus")
SRC10 = os.environ.get("SRC10", "local/corpus10")
ARGS = os.environ.get("ARGS", "--aq 1 --cutree 1")
KNOB = os.environ.get("KNOB", "qp")
JOBS = int(os.environ.get("JOBS", "3"))

# The metric domain is 8 bits on BOTH arms: one reference, one scale, and
# the Main10 decode narrowed to it. This is the whole point of the leg.
bd.SUBSAMPLE = 1
bd.DEPTH = 8

ladders = json.load(open(os.path.join(ROOT, "scripts", "ladders.json")))
sel = os.environ.get("CLIPS", "")
clips = sel.split(",") if sel else list(ladders)


def fps_of(path):
    with open(path, "rb") as f:
        hdr = f.readline().decode("latin-1")
    m = re.search(r" F(\d+):(\d+)", hdr)
    return int(m.group(1)) / int(m.group(2)) if m else 30.0


def targets_of(clip):
    secs = FRAMES / fps_of(os.path.join(ROOT, SRC8, clip + ".y4m"))
    return [int(kbps * 1000.0 / 8.0 * secs) for kbps in ladders[clip]]


def tmpl_for(binary):
    q = ARGS if "{q}" in ARGS else f"--{KNOB} {{q}} {ARGS}"
    return f"{binary} {{src}} --frames {FRAMES} {q} -o {{out}}"


def curve(tmpl, enc_src, ref_src, targets, work, label):
    """(bytes, VMAF-NEG) at each target. `enc_src` is what the encoder
    reads; `ref_src` is what the metric compares against, and the two
    differ on the Main10 arm by exactly the upshift."""
    out = os.path.join(work, label + ".hevc")
    sizes, pts = {}, []
    for t in targets:
        q = bar.solve(tmpl, enc_src, t, out, sizes)
        sz = bar.enc(tmpl, q, enc_src, out)
        v = bd.vmaf_of(out, ref_src, work)
        if not v or "VMAF-NEG" not in v:
            return None
        pts.append((sz, v["VMAF-NEG"]))
        print(f"    {label:<10} target {t:>9} -> q {q:3d}  {sz:>9} B  "
              f"NEG {v['VMAF-NEG']:6.2f}", flush=True)
    return pts


def run(clip):
    src8 = os.path.join(ROOT, SRC8, clip + ".y4m")
    src10 = os.path.join(ROOT, SRC10, clip + ".y4m")
    if not os.path.exists(src10):
        return clip, None, f"{clip}: no {src10} (scripts/upshift10.sh)"
    work = tempfile.mkdtemp(prefix="bd8v10.")
    targets = targets_of(clip)
    a = curve(tmpl_for(N10), src10, src8, targets, work, "main10")
    b = curve(tmpl_for(N8), src8, src8, targets, work, "8bit")
    if not a or not b:
        return clip, None, f"{clip}: encode/VMAF failed"
    r = bd.bd_rate([p[0] for p in b], [p[1] for p in b],
                   [p[0] for p in a], [p[1] for p in a])
    if not isinstance(r, float):
        return clip, None, f"{clip}: {r}"
    return clip, r, f"  {clip:<15} BD-rate(VMAF-NEG) main10 vs 8bit: {r:+.2f}%"


print(f"main10: {tmpl_for(N10)}   src {SRC10}\n"
      f"8bit:   {tmpl_for(N8)}   src {SRC8}\n"
      f"metric: 8-bit domain, reference {SRC8}\n")
res = {}
with ThreadPoolExecutor(max_workers=JOBS) as ex:
    for clip, r, line in ex.map(run, clips):
        print(line, flush=True)
        if r is not None:
            res[clip] = r
v = list(res.values())
if v:
    print("\n=== BD-rate(VMAF-NEG) our Main10 on the upshifted source vs our "
          "8-bit encoder, scored at 8 bits ===")
    for c in clips:
        if c in res:
            print(f"  {c:<18} {res[c]:+7.2f}%")
    print(f"  median {statistics.median(v):+.2f}%   "
          f"mean {statistics.mean(v):+.2f}%   "
          f"best {min(v):+.2f}%   worst {max(v):+.2f}%   "
          f"ahead {sum(1 for x in v if x < 0)}/{len(v)}")
