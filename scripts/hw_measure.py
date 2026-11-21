#!/usr/bin/env python3
# Copyright (c) 2026, the yah265 authors
# SPDX-License-Identifier: GPL-2.0-or-later
"""hw_measure.py - the hardware mode against our encoder at the same bitrate
(docs/hardware-mode.md).

For each clip: our medium at --crf 25 and auto threads, then --hw
videotoolbox at the average bitrate that encode landed on. Each side is
timed RUNS times (median wall, median user+sys from /usr/bin/time -l) and
scored once on VMAF v1 by the project's rule: vmaf_v1.0.16 3d0h at 1080p
and below, scaled up to fit 1920x1080 when smaller in both dimensions,
1d5h_2160 above 1080p, the _hfr models at 50 fps and up, 10-bit fifos.

    scripts/hw_measure.py [--frames 300] [--runs 3] [--csv out.csv] clip.y4m...

env: BIN (default build-rel/y265), V1_BIN, THREADS (libvmaf threads, 8).
Waits while ~/.cache/enclab/TIMING_ACTIVE exists.
"""
import argparse
import csv
import json
import os
import statistics
import subprocess
import sys
import tempfile
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BIN = os.environ.get("BIN", os.path.join(ROOT, "build-rel", "y265"))
V1 = os.path.expanduser(os.environ.get("V1_BIN", "~/.local/opt/vmaf-v1/bin/vmaf"))
MD = os.path.expanduser("~/.local/share/vmaf/model")
THR = os.environ.get("THREADS", "8")
LOCK = os.path.expanduser("~/.cache/enclab/TIMING_ACTIVE")
DISP = (1920, 1080)


def geom(src):
    with open(src, "rb") as f:
        h = f.readline().split()
    g = {t[:1]: t[1:] for t in h[1:]}
    fps = g.get(b"F", b"25:1").split(b":")
    return int(g[b"W"]), int(g[b"H"]), int(fps[0]) / int(fps[1])


def timed(cmd):
    """(wall s, user+sys s) of one run"""
    while os.path.exists(LOCK):
        time.sleep(30)
    t0 = time.monotonic()
    p = subprocess.run(["/usr/bin/time", "-l"] + cmd, stdout=subprocess.DEVNULL,
                       stderr=subprocess.PIPE, text=True)
    wall = time.monotonic() - t0
    if p.returncode:
        sys.exit("failed: %s\n%s" % (" ".join(cmd), p.stderr))
    # x265-style progress lines end in a carriage return
    err = p.stderr.replace("\r", "\n").split()
    cpu = 0.0
    for i, w in enumerate(err):
        if w in ("user", "sys") and i > 0:
            try:
                cpu += float(err[i - 1])
            except ValueError:
                pass
    return wall, cpu


def ff_y4m(inp, frames, fit, fifo):
    vf = ["-vf", "scale=%d:%d:flags=bicubic:force_original_aspect_ratio=decrease"
          ":force_divisible_by=2" % DISP] if fit else []
    fr = ["-frames:v", str(frames)] if frames else []
    return subprocess.Popen(["ffmpeg", "-v", "error", "-y", "-threads", "2", "-i", inp]
                            + fr + vf + ["-fps_mode", "passthrough", "-pix_fmt",
                                         "yuv420p10le", "-strict", "-1", "-f",
                                         "yuv4mpegpipe", fifo],
                            stdin=subprocess.DEVNULL)


def vmaf_v1(src, bs, frames, wd):
    w, h, fps = geom(src)
    sub = ("vmaf_v1.0.16_hfr", "vmaf_v1.0.16_hfr_") if fps >= 49.9 else \
          ("vmaf_v1.0.16", "vmaf_v1.0.16_")
    model = os.path.join(MD, sub[0], sub[1] + ("1d5h_2160" if h > 1080 else "3d0h") + ".json")
    fit = w < DISP[0] and h < DISP[1]
    rf, df, vj = (os.path.join(wd, n) for n in ("r.fifo", "d.fifo", "v.json"))
    for f in (rf, df):
        if os.path.exists(f):
            os.remove(f)
        os.mkfifo(f)
    pr = ff_y4m(src, frames, fit, rf)
    pd = ff_y4m(bs, None, fit, df)
    subprocess.run([V1, "--threads", THR, "-r", rf, "-d", df, "--json", "-o", vj, "-q",
                    "--model", "path=%s:name=v1:cambi.enc_bitdepth=8:cambi.enc_width=%d"
                    ":cambi.enc_height=%d" % (model, w, h)],
                   check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    pr.wait()
    pd.wait()
    fr = json.load(open(vj))["frames"]
    return sum(x["metrics"]["v1"] for x in fr) / len(fr), len(fr), os.path.basename(model)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("clips", nargs="+")
    ap.add_argument("--frames", type=int, default=300)
    ap.add_argument("--runs", type=int, default=3)
    ap.add_argument("--crf", type=float, default=25.0)
    ap.add_argument("--csv", default=os.path.join(ROOT, "docs", "data", "hw-videotoolbox.csv"))
    a = ap.parse_args()
    rows = []
    with tempfile.TemporaryDirectory() as wd:
        for src in a.clips:
            name = os.path.splitext(os.path.basename(src))[0]
            w, h, fps = geom(src)
            sw_bs, hw_bs = os.path.join(wd, "sw.hevc"), os.path.join(wd, "hw.hevc")
            base = [BIN, src, "--frames", str(a.frames), "--preset", "medium"]
            sw_cmd = base + ["--crf", str(a.crf), "-o", sw_bs]
            sw_t = [timed(sw_cmd) for _ in range(a.runs)]
            sw_bytes = os.path.getsize(sw_bs)
            sw_v, n_sw, model = vmaf_v1(src, sw_bs, a.frames, wd)
            kbps = sw_bytes * 8 / 1000 / (n_sw / fps)
            hw_cmd = base + ["--hw", "videotoolbox", "--bitrate", "%.0f" % kbps, "-o", hw_bs]
            hw_t = [timed(hw_cmd) for _ in range(a.runs)]
            hw_bytes = os.path.getsize(hw_bs)
            hw_v, n_hw, _ = vmaf_v1(src, hw_bs, a.frames, wd)
            if n_sw != n_hw:
                sys.exit("%s: scored %d frames ours, %d hardware" % (name, n_sw, n_hw))
            r = {
                "clip": name, "width": w, "height": h, "fps": round(fps, 3),
                "frames": n_sw, "model": model, "target_kbps": round(kbps),
                "ours_wall_s": round(statistics.median(t[0] for t in sw_t), 3),
                "hw_wall_s": round(statistics.median(t[0] for t in hw_t), 3),
                "ours_cpu_s": round(statistics.median(t[1] for t in sw_t), 3),
                "hw_cpu_s": round(statistics.median(t[1] for t in hw_t), 3),
                "ours_bytes": sw_bytes, "hw_bytes": hw_bytes,
                "ours_vmaf_v1": round(sw_v, 3), "hw_vmaf_v1": round(hw_v, 3),
            }
            r["wall_ratio_hw_over_ours"] = round(r["hw_wall_s"] / r["ours_wall_s"], 3)
            r["cpu_ratio_ours_over_hw"] = round(r["ours_cpu_s"] / r["hw_cpu_s"], 2)
            r["size_ratio_hw_over_ours"] = round(hw_bytes / sw_bytes, 3)
            r["vmaf_hw_minus_ours"] = round(hw_v - sw_v, 2)
            rows.append(r)
            print(json.dumps(r), flush=True)
    with open(a.csv, "w", newline="") as f:
        wr = csv.DictWriter(f, fieldnames=list(rows[0]))
        wr.writeheader()
        wr.writerows(rows)
    med = lambda k: statistics.median(r[k] for r in rows)
    print("median over %d clips: wall hw/ours %.2f, CPU ours/hw %.1f, VMAF hw-ours %+.2f, "
          "size hw/ours %.2f" % (len(rows), med("wall_ratio_hw_over_ours"),
                                 med("cpu_ratio_ours_over_hw"), med("vmaf_hw_minus_ours"),
                                 med("size_ratio_hw_over_ours")))


if __name__ == "__main__":
    main()
