#!/usr/bin/env python3
# Copyright (c) 2026, the yah265 authors
# SPDX-License-Identifier: GPL-2.0-or-later
"""ctl_vbv_check.py - does a live-rate stream fit its buffer?

vbv_check.py with a schedule that moves. The encode runs with a control
log, --frame-stats and N265_VBV_TRACE. Each RATE event's echo names the
frame whose decision applied it, and that frame's coding index is where
the buffer switches: a new size scales the fill before that removal, a
new maxrate fills from the next. The CPB is re-derived from the coded
sizes under that schedule and checked against the encoder's own fill
columns. Where the stream carries HRD timing SEI, the declared schedule
is walked too (vbv_check.hrd_walk), at the open-time rate and size.

    scripts/ctl_vbv_check.py --log step.log tests/corpus/foreman_cif.y4m \\
        --frames 300 -- --bitrate 600 --vbv-maxrate 900 --vbv-bufsize 900

Exit status is 1 on any underflow or desync.
"""
import argparse
import json
import os
import re
import subprocess
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import vbv_check as vc  # noqa: E402

RATE = re.compile(r"^(\d+) rate((?: \w+=\d+)+)$")


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("clip")
    ap.add_argument("--log", required=True, help="the control log")
    ap.add_argument("--enc", default=vc.N)
    ap.add_argument("--frames", type=int, default=300)
    ap.add_argument("--init", type=float, default=0.9)
    ap.add_argument("args", nargs="*", help="encoder options, after --")
    a = ap.parse_args()
    opt = {k: v for k, v in zip(a.args, a.args[1:]) if k.startswith("--vbv")}
    maxrate = float(opt["--vbv-maxrate"]) * 1000.0
    bufsize = float(opt["--vbv-bufsize"]) * 1000.0
    with open(a.clip, "rb") as f:
        m = re.search(r" F(\d+):(\d+)", f.readline().decode("latin-1"))
    fps = int(m[1]) / int(m[2]) if m else 30.0
    with tempfile.TemporaryDirectory() as td:
        out, fst = os.path.join(td, "o.hevc"), os.path.join(td, "f.jsonl")
        env = dict(os.environ, N265_VBV_TRACE="1")
        cmd = [a.enc, a.clip, "--frames", str(a.frames), "--vbv-init",
               str(a.init), "--control", a.log, "--frame-stats", fst,
               "-o", out] + a.args
        r = subprocess.run(cmd, capture_output=True, text=True, env=env)
        if r.returncode:
            sys.exit(r.stderr.strip()[-400:])
        lines = [json.loads(x) for x in open(fst)]
        try:
            aus, sei_err = vc.au_walk(out), None
        except (ValueError, IndexError) as e:
            aus, sei_err = [], str(e)
        size = os.path.getsize(out)
    rows = sorted((dict(code=int(m[1]), bits=float(m[6]), fill_in=float(m[7]),
                        fill_out=float(m[8])) for m in vc.ROW.finditer(r.stderr)),
                  key=lambda x: x["code"])
    sw = {}
    for ln in lines:
        for s in ln.get("control", []):
            m = RATE.match(s)
            if m:
                kv = dict(x.split("=") for x in m[2].split())
                sw.setdefault(ln["cidx"], {}).update(kv)
    fill = min(a.init * bufsize, bufsize)
    under = desync = 0
    minpct = 100.0
    seg, segs = [0.0, 0, maxrate], []
    for row in rows:
        k = row["code"]
        if k in sw:
            if seg[1]:
                segs.append(seg)
            if "bufsize" in sw[k]:
                nb = float(sw[k]["bufsize"]) * 1000.0
                fill *= nb / bufsize
                bufsize = nb
            if "maxrate" in sw[k]:
                maxrate = float(sw[k]["maxrate"]) * 1000.0
            seg = [0.0, 0, maxrate]
        if abs(fill - row["fill_in"]) > 1.0:
            desync += 1
        fill -= row["bits"]
        minpct = min(minpct, 100.0 * fill / bufsize)
        under += fill < 0.0
        fill = min(fill + maxrate / fps, bufsize)
        if abs(fill - row["fill_out"]) > 1.0:
            desync += 1
        seg[0] += row["bits"]
        seg[1] += 1
    segs.append(seg)
    print(f"{len(rows)} frames, {size} bytes, {len(sw)} switches, "
          f"underflows {under}, desync {desync}, min fill {minpct:.1f}%")
    for bits, n, mr in segs:
        print(f"  segment: {n} frames at maxrate {mr / 1000:.0f}k, "
              f"{bits * fps / n / 1000:.0f} kbit/s")
    sei_under = 0
    if aus and any(x["bp"] for x in aus):
        br, cpb = vc.declared_pair(float(opt["--vbv-maxrate"]) * 1000.0,
                                   float(opt["--vbv-bufsize"]) * 1000.0)
        _, sei_under, mono = vc.hrd_walk(aus, br, cpb, 1.0 / fps)
        print(f"  declared HRD walk: underflows {sei_under}, non-monotone {mono}")
    elif sei_err:
        print(f"  no HRD walk: {sei_err}")
    return 1 if under or desync or sei_under else 0


if __name__ == "__main__":
    sys.exit(main())
