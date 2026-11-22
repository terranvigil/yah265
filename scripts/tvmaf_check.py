#!/usr/bin/env python3
# Copyright (c) 2026, the yah265 authors
# SPDX-License-Identifier: GPL-2.0-or-later
"""tvmaf_check.py - how close --target-vmaf lands, and whether a capped run
stays inside its buffer.

For each clip and target: y265 --target-vmaf T with the given flags, its
JSON report read back, and one row printed: the chosen CRF, the trials,
the sample, the search's predicted score, the final file's measured score,
the error, the bytes and the search time.

    scripts/tvmaf_check.py --targets 93,85 clip.y4m...
    scripts/tvmaf_check.py --targets 93 --args "--vbv-maxrate 4500 \\
        --vbv-bufsize 6000" clip.y4m
    scripts/tvmaf_check.py --recheck clip.y4m   # re-score the output in Python

With a VBV in --args, the output's access units are walked against the
HRD SEI it carries (scripts/vbv_check.py's decoder-side walk) and the
underflows are printed; any underflow fails the run.

--recheck scores the final file again through scripts/parity_long.py's
own chain (decode, 10-bit, the display-size scale, the v1 gate model) as
a check on the C scorer. SDR only.

Takes ~/src/yah265/local/BOX_LOCK (retrying every 60 s) around each run,
unless --no-lock.

env: BIN (build/y265), THREADS (8), CSV (append rows here)
"""
import argparse
import csv
import json
import os
import shlex
import subprocess
import sys
import tempfile
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "scripts"))
BIN = os.environ.get("BIN", os.path.join(ROOT, "build", "y265"))
THREADS = os.environ.get("THREADS", "8")
LOCK = os.path.expanduser("~/src/yah265/local/BOX_LOCK")
V1_BIN = os.path.expanduser(os.environ.get("N265_VMAF_BIN",
                                           "~/.local/opt/vmaf-v1/bin/vmaf"))
V1_DIR = os.path.expanduser(os.environ.get("N265_VMAF_MODEL_DIR",
                                           "~/.local/share/vmaf/model"))


def lock(use):
    if not use:
        return
    while True:
        try:
            os.mkdir(LOCK)
            return
        except FileExistsError:
            time.sleep(60)


def unlock(use):
    if use:
        try:
            os.rmdir(LOCK)
        except OSError:
            pass


def geom(src):
    with open(src, "rb") as f:
        h = f.readline().split()
    g = {t[:1]: t[1:] for t in h[1:]}
    fps = g.get(b"F", b"25:1").split(b":")
    return int(g[b"W"]), int(g[b"H"]), int(fps[0]) / int(fps[1])


def recheck(src, bs, frames):
    """parity_long's v1 chain, the gate model, every frame."""
    w, h, fps = geom(src)
    hfr = fps >= 49.9
    m = "1d5h_2160" if w * h > 1920 * 1080 else "3d0h"
    model = (f"{V1_DIR}/vmaf_v1.0.16_hfr/vmaf_v1.0.16_hfr_{m}.json" if hfr
             else f"{V1_DIR}/vmaf_v1.0.16/vmaf_v1.0.16_{m}.json")
    scale = ("scale=1920:1080:flags=bicubic:force_original_aspect_ratio"
             "=decrease:force_divisible_by=2")
    up = w < 1920 and h < 1080
    tail = ["-pix_fmt", "yuv420p10le", "-strict", "-1", "-f", "yuv4mpegpipe"]
    with tempfile.TemporaryDirectory() as wd:
        fifo = os.path.join(wd, "d.fifo")
        os.mkfifo(fifo)
        j = os.path.join(wd, "v.json")
        dec = subprocess.Popen(["ffmpeg", "-v", "error", "-y", "-threads", "2",
                                "-i", bs] + (["-vf", scale] if up else []) +
                               tail + [fifo], stdout=subprocess.DEVNULL)
        ref = subprocess.Popen(["ffmpeg", "-v", "error", "-i", src,
                                "-frames:v", str(frames)] +
                               (["-vf", scale] if up else []) + tail + ["-"],
                               stdout=subprocess.PIPE)
        subprocess.run([V1_BIN, "--threads", THREADS, "-r", "/dev/stdin",
                        "-d", fifo, "--json", "-o", j, "-q", "--model",
                        f"path={model}:name=g:cambi.enc_bitdepth=8"
                        f":cambi.enc_width={w}:cambi.enc_height={h}"],
                       stdin=ref.stdout, check=True,
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        ref.wait()
        dec.wait()
        jj = json.load(open(j))
        return jj["pooled_metrics"]["g"]["mean"], len(jj["frames"])


def hrd_check(bs, args, fps):
    import vbv_check as vc
    a = shlex.split(args)
    kbps = float(a[a.index("--vbv-maxrate") + 1])
    kbit = float(a[a.index("--vbv-bufsize") + 1])
    aus = vc.au_walk(bs)
    br, cpb = vc.declared_pair(kbps * 1000.0, kbit * 1000.0)
    minfill, under, mono = vc.hrd_walk(aus, br, cpb, 1.0 / fps)
    rows = [dict(bits=au["bytes"] * 8.0, fill_in=None, fill_out=None)
            for au in aus]
    init = float(a[a.index("--vbv-init") + 1]) if "--vbv-init" in a else 0.9
    _, under2, _ = vc.walk(rows, kbps * 1000.0, kbit * 1000.0, init, fps)
    return under, under2, 100.0 * minfill / cpb, mono


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("clips", nargs="+")
    ap.add_argument("--targets", default="93")
    ap.add_argument("--args", default="")
    ap.add_argument("--recheck", action="store_true")
    ap.add_argument("--no-lock", action="store_true")
    ap.add_argument("--keep", help="directory to keep the streams in")
    a = ap.parse_args()
    out_csv = os.environ.get("CSV")
    rows = []
    bad = 0
    print(f"{'clip':28s} {'target':>6s} {'crf':>6s} {'n':>2s} {'sample':>11s} "
          f"{'pred':>6s} {'meas':>6s} {'err':>6s} {'kbps':>9s} {'search':>7s}"
          f"  status extra", flush=True)
    for src in a.clips:
        name = os.path.splitext(os.path.basename(src))[0]
        w, h, fps = geom(src)
        for t in [float(x) for x in a.targets.split(",")]:
            with tempfile.TemporaryDirectory() as wd:
                bs = os.path.join(a.keep or wd, f"{name}_tv{t:g}.hevc")
                js = os.path.join(wd, "r.json")
                cmd = [BIN, src, "--threads", THREADS, "--target-vmaf",
                       f"{t:g}", "--target-vmaf-json", js, "-o", bs] + \
                      shlex.split(a.args)
                lock(not a.no_lock)
                try:
                    r = subprocess.run(cmd, capture_output=True, text=True)
                    rc = None
                    if r.returncode == 0 and a.recheck:
                        rep = json.load(open(js))
                        rc = recheck(src, bs, rep["frames"])
                finally:
                    unlock(not a.no_lock)
                if r.returncode != 0:
                    print(f"{name}: FAILED\n{r.stderr[-800:]}")
                    bad += 1
                    continue
                rep = json.load(open(js))
                extra = ""
                if rc:
                    extra += f"recheck {rc[0]:.3f} ({rc[1]} fr) "
                if "--vbv-maxrate" in a.args:
                    u, u2, mpct, mono = hrd_check(bs, a.args, fps)
                    extra += (f"hrd underflow {u} (min {mpct:.1f}%) "
                              f"walk {u2}{' NONMONO' if mono else ''}")
                    bad += u + u2 + mono
                meas = rep["measured"]
                err = meas - t if meas is not None else float("nan")
                print(f"{name:28s} {t:6.2f} {rep['crf']:6.2f} "
                      f"{len(rep['trials']):2d} "
                      f"{rep['sample_frames']:5d}/{rep['frames']:<5d} "
                      f"{rep['predicted']:6.2f} "
                      f"{meas if meas is not None else float('nan'):6.2f} "
                      f"{err:+6.2f} {rep['kbps']:9.1f} {rep['search_s']:6.1f}s"
                      f"  {rep['status'].split(':')[0]} {extra}", flush=True)
                row = dict(clip=name, w=w, h=h, fps=round(fps, 3), target=t,
                           args=a.args, **{k: rep[k] for k in (
                               "crf", "status", "model", "predicted",
                               "measured", "same_bytes", "bytes", "kbps",
                               "frames", "sample_frames", "search_s",
                               "final_s", "measure_s")},
                           trials=len(rep["trials"]), err=err,
                           recheck=rc[0] if rc else None, extra=extra)
                rows.append(row)
    if out_csv and rows:
        new = not os.path.exists(out_csv)
        with open(out_csv, "a", newline="") as f:
            wr = csv.DictWriter(f, fieldnames=list(rows[0]))
            if new:
                wr.writeheader()
            wr.writerows(rows)
    sys.exit(1 if bad else 0)


if __name__ == "__main__":
    main()
