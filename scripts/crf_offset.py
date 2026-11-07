#!/usr/bin/env python3
# Copyright (c) 2026, the yah265 authors
# SPDX-License-Identifier: GPL-2.0-or-later
"""Where our `--crf N` lands against x265's `--crf N`, clip by clip.

This is a MAPPING instrument, not a quality one: it asks only what the crf
NUMBER buys in bytes on each side, which is what a user who knows ffmpeg's
knob expects to transfer. Quality is the band's question and is read by
scripts/bd_vs_x265.py at matched achieved bitrate, where the crf axis
cancels; nothing here says anything about it.

For each clip it encodes us at every crf in CRFS, encodes x265 on an
integer-step LADDER over the same range, and reports

  ratio   bytes(ours) / bytes(x265) at the SAME crf number
  offset  the x265 crf whose bytes equal ours, minus our crf
          (log-linear between the ladder's rungs)

A positive offset means we are coarser than the label: our crf 26 buys
what x265's crf 26 + offset buys. The per-clip SPREAD of the offset is
the number that says whether the two scales can be aligned by a mapping
at all - a constant offset is a relabel, a spread is content.

    CLIPS=foreman_cif,akiyo_cif python3 scripts/crf_offset.py
    CLIPS=/path/to/big.y4m FRAMES=600 LO=12 HI=34 python3 scripts/crf_offset.py
    OURENV='N265_AQ_DC=0' TAG=dc0 python3 scripts/crf_offset.py

Encodes are memoized under ~/.cache/yah265-crf (CRF_CACHE=dir, =0 off),
keyed by the command, the source and a hash of our binary, the way
bd_at_rate.py memoizes the band's.

x265 runs at its DEFAULT thread configuration here, because the question
is what a user's own `x265 --crf N` produces. That costs 0.4-3.5% of the
reference bytes against the band's `--pools none --frame-threads 1` arm
(measured 2026-09-16 on akiyo/mobile/sintel), which is up to 0.3 crf
points of offset on the flattest clip; XTHREADS='--pools none
--frame-threads 1' reads the band's arm instead.
"""
import hashlib, json, math, os, statistics, subprocess, sys, tempfile
from concurrent.futures import ThreadPoolExecutor

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
N265 = os.environ.get("N265", os.path.join(ROOT, "build-rel/n265"))
X265 = os.environ.get("X265", "x265")
SRCDIR = os.environ.get("SRCDIR", os.path.join(ROOT, "tests/corpus"))
FRAMES = int(os.environ.get("FRAMES", "120"))
PRESET = os.environ.get("PRESET", "medium")
OURARGS = os.environ.get("OURARGS", "")
OURENV = os.environ.get("OURENV", "")       # "N265_FOO=1 N265_BAR=2"
TAG = os.environ.get("TAG", "")             # names the arm in the cache key
XTHREADS = os.environ.get("XTHREADS", "")   # "" = x265's own default
# our thread count is not in the cache key on purpose: our output is
# byte-identical across thread counts and this is a BYTE instrument.
OURTHREADS = os.environ.get("OURTHREADS", "2")
JOBS = int(os.environ.get("JOBS", "4"))
CRFS = [float(x) for x in os.environ.get("CRFS", "18,22,26,30,34").split(",")]
LO = float(os.environ.get("LO", "10"))
HI = float(os.environ.get("HI", "44"))
STEP = float(os.environ.get("STEP", "2"))
CIF7 = "akiyo_cif,bus_cif,coastguard_cif,foreman_cif,mobile_cif,stefan_cif,tempete_cif"
clips = os.environ.get("CLIPS", CIF7).split(",")

CACHE = os.environ.get("CRF_CACHE", os.path.expanduser("~/.cache/yah265-crf"))
if CACHE != "0":
    os.makedirs(CACHE, exist_ok=True)
BINHASH = hashlib.sha256(open(N265, "rb").read()).hexdigest()[:16]

def srcpath(clip):
    return clip if "/" in clip else os.path.join(SRCDIR, clip + ".y4m")

def _bytes(cmd, env=None, key=None):
    if CACHE != "0" and key:
        f = os.path.join(CACHE, hashlib.sha256(key.encode()).hexdigest() + ".json")
        if os.path.exists(f):
            return json.load(open(f))["bytes"]
    fd, out = tempfile.mkstemp(suffix=".hevc"); os.close(fd)
    try:
        cmd = [c.replace("{out}", out) for c in cmd]
        r = subprocess.run(cmd, capture_output=True, text=True, env=env)
        if not os.path.exists(out) or os.path.getsize(out) == 0:
            raise SystemExit("encode failed: %s\n%s" % (" ".join(cmd), r.stderr[-2000:]))
        n = os.path.getsize(out)
    finally:
        if os.path.exists(out):
            os.remove(out)
    if CACHE != "0" and key:
        json.dump({"bytes": n, "cmd": key}, open(f, "w"))
    return n

def ours(clip, crf):
    cmd = [N265, srcpath(clip), "--preset", PRESET, "--crf", "%g" % crf,
           "--frames", str(FRAMES), "--threads", OURTHREADS] + \
          (OURARGS.split() if OURARGS else []) + ["-o", "{out}"]
    env = dict(os.environ)
    for kv in OURENV.split():
        k, _, v = kv.partition("=")
        env[k] = v
    key = "ours|%s|%s|%s|%s" % (BINHASH, OURENV, " ".join(cmd[1:]), srcstamp(clip))
    return _bytes(cmd, env, key)

def x265(clip, crf):
    cmd = [X265, "--preset", PRESET, "--crf", "%g" % crf, "--frames", str(FRAMES),
           "--log-level", "none"] + (XTHREADS.split() if XTHREADS else []) + \
          ["-o", "{out}", srcpath(clip)]
    return _bytes(cmd, None, "x265|%s|%s" % (" ".join(cmd[1:]), srcstamp(clip)))

def srcstamp(clip):
    st = os.stat(srcpath(clip))
    return "%d:%d" % (st.st_size, int(st.st_mtime))

def match_crf(lad, target):
    """the x265 crf whose bytes equal `target`, log-linear between rungs."""
    pts = sorted(lad)
    for i in range(len(pts) - 1):
        q0, b0 = pts[i]; q1, b1 = pts[i + 1]
        if b0 >= target >= b1 or b0 <= target <= b1:
            if b0 == b1:
                return q0
            t = (math.log(target) - math.log(b0)) / (math.log(b1) - math.log(b0))
            return q0 + t * (q1 - q0)
    q0, b0, q1, b1 = (pts[0] + pts[1] if target > pts[0][1] else pts[-2] + pts[-1])
    t = (math.log(target) - math.log(b0)) / (math.log(b1) - math.log(b0))
    return q0 + t * (q1 - q0)

def main():
    rungs = []
    q = LO
    while q <= HI + 1e-9:
        rungs.append(round(q, 3)); q += STEP
    rows = {}
    for clip in clips:
        with ThreadPoolExecutor(JOBS) as ex:
            list(ex.map(lambda t: x265(clip, t), rungs))
            list(ex.map(lambda t: ours(clip, t), CRFS))
        lad = [(t, x265(clip, t)) for t in rungs]
        rows[clip] = [(t, ours(clip, t), x265(clip, t) if t in rungs else None,
                       match_crf(lad, ours(clip, t)) - t) for t in CRFS]
    name = lambda c: os.path.basename(c).replace(".y4m", "")[:20]
    print("ours: %s --preset %s %s %s | x265 threads %s | %d frames"
          % (os.path.basename(N265), PRESET, OURENV, OURARGS,
             XTHREADS or "default", FRAMES))
    hdr = "clip".ljust(21) + "".join(("crf%g" % t).rjust(9) for t in CRFS)
    for what, idx, fmt in (("bytes ratio ours/x265 at the same crf", 2, "%.3f"),
                           ("offset: x265 crf matching our bytes, minus ours", 3, "%+.2f")):
        print("\n-- %s --\n%s" % (what, hdr))
        for c in clips:
            line = name(c).ljust(21)
            for r in rows[c]:
                if idx == 2:
                    line += (fmt % (r[1] / r[2])).rjust(9) if r[2] else "        -"
                else:
                    line += (fmt % r[3]).rjust(9)
            print(line)
    offs = {t: [r[3] for c in clips for r in rows[c] if r[0] == t] for t in CRFS}
    for lab, fn in (("MEDIAN", lambda v: statistics.median(v)),
                    ("median|offset|", lambda v: statistics.median([abs(x) for x in v])),
                    ("SPREAD", lambda v: max(v) - min(v))):
        print(lab.ljust(21) + "".join(("%+.2f" % fn(offs[t])).rjust(9) for t in CRFS))

if __name__ == "__main__":
    main()
