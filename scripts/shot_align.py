#!/usr/bin/env python3
# Copyright (c) 2026, the yah265 authors
# SPDX-License-Identifier: GPL-2.0-or-later
"""Key frames on the same frames at every rung, checked from the streams.

A ladder is one stream per rung, and a player can change rung only where
every stream has a key frame. This scans a source once (--shot-scan),
scales it to each rung, encodes every rung under the one list
(--shot-list) and reads the IDR display indices back out of each stream.
It passes when every rung has the same IDR set and every listed start is
in it.

  shot_align.py SRC.y4m [--heights 1080,720,360] [--frames N] [--seek N]
                [--args "--crf 26"] [--bin build/n265] [--work DIR]
  shot_align.py --idr STREAM.hevc       print one stream's IDR frames

The IDRs come from the stream itself: ffprobe gives each output frame's
packet offset, and the NAL type at that offset says IDR (19, 20) or not.
It also reports whether a scan of each rung finds the list the source
scan found, which is the fixed analysis size's claim.
"""
import argparse
import bisect
import os
import shlex
import subprocess
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
IDR = (19, 20)


def vcl_at(data):
    """(offset of the start code, nal type) for each first slice segment."""
    out = []
    i = 0
    n = len(data)
    while True:
        j = data.find(b"\x00\x00\x01", i)
        if j < 0 or j + 5 > n:
            break
        t = (data[j + 3] >> 1) & 0x3F
        if t <= 31 and data[j + 5] & 0x80:
            sc = j - 1 if j > 0 and data[j - 1] == 0 else j
            out.append((sc, t))
        i = j + 3
    return out


def idr_frames(path, ffprobe="ffprobe"):
    """IDR display indices of an Annex-B HEVC stream, and its frame count."""
    data = open(path, "rb").read()
    pics = vcl_at(data)
    r = subprocess.run([ffprobe, "-v", "error", "-f", "hevc", "-select_streams",
                        "v:0", "-show_entries", "frame=pkt_pos", "-of",
                        "csv=p=0", path], capture_output=True, text=True,
                       check=True)
    pos = [int(x.strip().rstrip(",")) for x in r.stdout.split() if x.strip()]
    idr = []
    starts = [p for p, _ in pics]
    for d, p in enumerate(pos):
        k = bisect.bisect_left(starts, p)
        if k < len(pics) and pics[k][1] in IDR:
            idr.append(d)
    return idr, len(pos)


def list_starts(path):
    starts, on = [], False
    for ln in open(path):
        ln = ln.split("#")[0].strip()
        if not ln:
            continue
        if ln.startswith("shots "):
            on = True
            continue
        if ln == "end":
            break
        if on:
            starts.append(int(ln.split()[0]))
    return starts


def shots_body(path):
    out, on = [], False
    for ln in open(path):
        if ln.startswith("shots "):
            on = True
        if on:
            out.append(" ".join(ln.split()[:3]))
    return out


def run(cmd):
    r = subprocess.run(cmd, capture_output=True, text=True)
    if r.returncode:
        sys.exit(f"shot_align: failed: {shlex.join(cmd)}\n{r.stderr[-800:]}")
    return r


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("src", nargs="?")
    ap.add_argument("--idr", help="print one stream's IDR display indices")
    ap.add_argument("--heights", default="1080,720,360")
    ap.add_argument("--frames", type=int, default=0)
    ap.add_argument("--seek", type=int, default=0)
    ap.add_argument("--args", default="--crf 26")
    ap.add_argument("--threads", default="4")
    ap.add_argument("--bin", default=os.path.join(ROOT, "build", "n265"))
    ap.add_argument("--ffmpeg", default=os.environ.get("FFMPEG", "ffmpeg"))
    ap.add_argument("--ffprobe", default=os.environ.get("FFPROBE", "ffprobe"))
    ap.add_argument("--work")
    a = ap.parse_args()
    if a.idr:
        idr, n = idr_frames(a.idr, a.ffprobe)
        print(f"{n} frames, IDR at {' '.join(map(str, idr))}")
        return 0
    if not a.src:
        ap.error("a source, or --idr")
    work = a.work or tempfile.mkdtemp(prefix="shotalign")
    os.makedirs(work, exist_ok=True)
    name = os.path.splitext(os.path.basename(a.src))[0]
    # every rung, the source size included, is a trimmed y4m file: the list
    # needs an input it can seek in, and the same frames on every rung
    rungs = []
    for h in [int(x) for x in a.heights.split(",") if x]:
        out = os.path.join(work, f"{name}_{h}p.y4m")
        vf = f"scale=-2:{h}:flags=bicubic"
        if a.seek:
            vf = f"select=gte(n\\,{a.seek}),setpts=N/FRAME_RATE/TB," + vf
        cmd = [a.ffmpeg, "-loglevel", "error", "-y", "-i", a.src, "-vf", vf,
               "-pix_fmt", "yuv420p"]
        if a.frames:
            cmd += ["-frames:v", str(a.frames)]
        run(cmd + ["-strict", "-1", out])
        rungs.append((h, out))
    lst = os.path.join(work, f"{name}.shots")
    run([a.bin, rungs[0][1], "--threads", a.threads, "--shot-scan", lst])
    want = list_starts(lst)
    body = shots_body(lst)
    print(f"{name}: {len(want)} shots from the {rungs[0][0]}p scan: "
          f"{' '.join(map(str, want))}")
    sets, ok = {}, True
    for h, y in rungs:
        rl = os.path.join(work, f"{name}_{h}p.shots")
        run([a.bin, y, "--threads", a.threads, "--shot-scan", rl])
        agree = shots_body(rl) == body
        st = os.path.join(work, f"{name}_{h}p.hevc")
        run([a.bin, y, "-o", st, "--threads", a.threads, "--shot-list", lst]
            + shlex.split(a.args))
        idr, n = idr_frames(st, a.ffprobe)
        sets[h] = idr
        miss = [s for s in want if s not in idr]
        print(f"  {h}p: {n} frames, {len(idr)} IDR, listed starts missing: "
              f"{miss or 'none'}; own scan {'agrees' if agree else 'differs'}")
        ok &= not miss
    ref = sets[rungs[0][0]]
    same = all(v == ref for v in sets.values())
    print(f"{name}: IDR sets {'identical' if same else 'DIFFER'} across "
          f"{len(sets)} rungs: {' '.join(map(str, ref))}")
    ok &= same
    print(f"{name}: {'PASS' if ok else 'FAIL'}")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
