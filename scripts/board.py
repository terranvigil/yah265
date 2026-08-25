#!/usr/bin/env python3
"""Parity board: measure next265 against pinned x265 and append one row
per clip to docs/data/parity-history.csv so progress charts over time.

Two x265 arms, single-threaded:
- "matched": x265 configured down to our current featureset (kept in
  X265_MATCHED below; grows with the encoder until it equals medium) -
  the like-for-like progress line, and the BD-rate anchor.
- "medium": x265 pure defaults - the goal line the legs are defined on.
Speed multiple = our_time / x265_time (1.00 = parity, lower = faster).
BD-rate (Y-PSNR) vs the matched arm; negative = we spend fewer bits at
equal PSNR.

Timing rules (docs/instruments.md): output to /dev/null, median of
NRUNS, sizes measured in a separate untimed pass.

usage: scripts/board.py [--quick] [--note TEXT]
"""

import csv
import datetime
import math
import os
import re
import subprocess
import sys
import tempfile
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
N265 = os.environ.get("N265", os.path.join(ROOT, "build/n265"))
X265 = os.environ.get("X265", "x265")
FFMPEG = os.environ.get("FFMPEG", "ffmpeg")
HISTORY = os.path.join(ROOT, "docs/data/parity-history.csv")
NRUNS = 3
QPS = [22, 27, 32, 37]

# x265 args mirroring our current featureset (update as features land;
# the goal is for this list to shrink to nothing)
X265_MATCHED = ["--keyint", "60", "--bframes", "0", "--rc-lookahead", "0",
                "--ref", "1", "--no-scenecut", "--no-open-gop"]

CLIPS = [
    # deterministic synthetic material until the real corpus lands
    ("synth_detail_cif", "352x288", 30,
     "testsrc2=size=352x288:rate=25,noise=alls=8:allf=t:all_seed=7"),
    ("synth_smooth_cif", "352x288", 30,
     "gradients=size=352x288:rate=25:seed=11"),
    ("synth_detail_720", "1280x720", 10,
     "testsrc2=size=1280x720:rate=25,noise=alls=6:allf=t:all_seed=7"),
]


def run(cmd, **kw):
    return subprocess.run(cmd, check=True, capture_output=True, **kw)


def gen_clip(tmp, name, filt, frames):
    path = os.path.join(tmp, name + ".y4m")
    run([FFMPEG, "-loglevel", "error", "-f", "lavfi", "-i", filt,
         "-frames:v", str(frames), "-pix_fmt", "yuv420p", path, "-y"])
    return path


def time_cmd(cmd):
    best = []
    for _ in range(NRUNS):
        t0 = time.perf_counter()
        run(cmd)
        best.append(time.perf_counter() - t0)
    best.sort()
    return best[len(best) // 2]


def psnr_y(dist, ref_y4m, size):
    p = subprocess.run(
        [FFMPEG, "-loglevel", "info",
         "-f", "rawvideo", "-pix_fmt", "yuv420p", "-video_size", size,
         "-i", dist, "-i", ref_y4m,
         "-lavfi", "psnr", "-f", "null", "-"],
        capture_output=True, text=True)
    m = re.search(r"psnr.*y:([0-9.]+|inf)", p.stderr)
    v = m.group(1) if m else "0"
    return 99.0 if v == "inf" else float(v)


def bd_rate(anchor, test):
    """Classic BD-rate over (bits, psnr) points; % bitrate delta of test
    vs anchor at equal quality. Negative = test better."""
    def fit(points):
        xs = [p[1] for p in points]           # psnr
        ys = [math.log(p[0]) for p in points]  # log bits
        n = len(xs)
        # cubic fit via least squares (Vandermonde, tiny n)
        import itertools
        A = [[x ** k for k in range(4)] for x in xs]
        # normal equations
        AT = list(zip(*A))
        M = [[sum(a * b for a, b in zip(r1, r2)) for r2 in AT] for r1 in AT]
        v = [sum(a * y for a, y in zip(r, ys)) for r in AT]
        # gaussian elim
        for i in range(4):
            piv = max(range(i, 4), key=lambda r: abs(M[r][i]))
            M[i], M[piv] = M[piv], M[i]
            v[i], v[piv] = v[piv], v[i]
            for r in range(i + 1, 4):
                f = M[r][i] / M[i][i]
                for c in range(i, 4):
                    M[r][c] -= f * M[i][c]
                v[r] -= f * v[i]
        coef = [0.0] * 4
        for i in range(3, -1, -1):
            coef[i] = (v[i] - sum(M[i][c] * coef[c]
                                  for c in range(i + 1, 4))) / M[i][i]
        return coef

    lo = max(min(p[1] for p in anchor), min(p[1] for p in test))
    hi = min(max(p[1] for p in anchor), max(p[1] for p in test))
    if hi <= lo:
        return float("nan")
    ca, ct = fit(anchor), fit(test)

    def integ(c, a, b):
        return sum(c[k] / (k + 1) * (b ** (k + 1) - a ** (k + 1))
                   for k in range(4))
    da = integ(ca, lo, hi) / (hi - lo)
    dt = integ(ct, lo, hi) / (hi - lo)
    return (math.exp(dt - da) - 1) * 100


def measure_clip(tmp, name, size, frames, filt, quick):
    y4m = gen_clip(tmp, name, filt, frames)
    row = {}

    # ours: speed at qp 27 (timing), then the sweep for rate/quality
    t_ours = time_cmd([N265, y4m, "-o", "/dev/null", "--qp", "27"])
    ours_pts = []
    for qp in QPS:
        out = os.path.join(tmp, f"n_{name}_{qp}.hevc")
        rec = os.path.join(tmp, f"n_{name}_{qp}.yuv")
        run([N265, y4m, "-o", out, "--qp", str(qp), "--dump-recon", rec])
        ours_pts.append((os.path.getsize(out) * 8, psnr_y(rec, y4m, size)))

    st = ["--pools", "none", "--frame-threads", "1", "--log-level", "error"]
    matched = [X265, "--input", y4m] + st + X265_MATCHED
    medium = [X265, "--input", y4m] + st
    t_m_noasm = time_cmd(matched + ["--no-asm", "--qp", "27", "-o",
                                    "/dev/null"])
    t_m_asm = time_cmd(matched + ["--qp", "27", "-o", "/dev/null"])
    t_med_noasm = time_cmd(medium + ["--no-asm", "--crf", "28", "-o",
                                     "/dev/null"])
    t_med_asm = time_cmd(medium + ["--crf", "28", "-o", "/dev/null"])
    x_pts = []
    for qp in QPS:
        out = os.path.join(tmp, f"x_{name}_{qp}.hevc")
        run(matched + ["--qp", str(qp), "-o", out])
        dec = os.path.join(tmp, f"x_{name}_{qp}.yuv")
        run([FFMPEG, "-loglevel", "error", "-i", out, "-f", "rawvideo",
             dec, "-y"])
        x_pts.append((os.path.getsize(out) * 8, psnr_y(dec, y4m, size)))

    row["clip"] = name
    row["frames"] = frames
    row["our_fps"] = round(frames / t_ours, 2)
    row["x265m_noasm_fps"] = round(frames / t_m_noasm, 2)
    row["x265m_asm_fps"] = round(frames / t_m_asm, 2)
    row["mult_matched_g1"] = round(t_ours / t_m_noasm, 3)
    row["mult_matched_g3"] = round(t_ours / t_m_asm, 3)
    row["x265med_noasm_fps"] = round(frames / t_med_noasm, 2)
    row["x265med_asm_fps"] = round(frames / t_med_asm, 2)
    row["mult_medium_g1"] = round(t_ours / t_med_noasm, 3)
    row["mult_medium_g3"] = round(t_ours / t_med_asm, 3)
    row["bd_rate_psnr_pct"] = round(bd_rate(x_pts, ours_pts), 2)
    return row


def main():
    quick = "--quick" in sys.argv
    note = ""
    if "--note" in sys.argv:
        note = sys.argv[sys.argv.index("--note") + 1]
    commit = run(["git", "-C", ROOT, "rev-parse", "--short", "HEAD"]
                 ).stdout.decode().strip()
    xver = subprocess.run([X265, "--version"], capture_output=True,
                          text=True)
    m = re.search(r"HEVC encoder version ([^\s]+)", xver.stderr + xver.stdout)
    xv = m.group(1) if m else "?"

    fields = ["date", "commit", "x265", "clip", "frames", "our_fps",
              "x265m_noasm_fps", "x265m_asm_fps", "mult_matched_g1",
              "mult_matched_g3", "x265med_noasm_fps", "x265med_asm_fps",
              "mult_medium_g1", "mult_medium_g3", "bd_rate_psnr_pct",
              "note"]
    os.makedirs(os.path.dirname(HISTORY), exist_ok=True)
    new = not os.path.exists(HISTORY)

    clips = CLIPS[:1] if quick else CLIPS
    with tempfile.TemporaryDirectory() as tmp, \
         open(HISTORY, "a", newline="") as f:
        wr = csv.DictWriter(f, fieldnames=fields)
        if new:
            wr.writeheader()
        for name, size, frames, filt in clips:
            row = measure_clip(tmp, name, size, frames, filt, quick)
            row.update(date=datetime.date.today().isoformat(),
                       commit=commit, x265=xv, note=note)
            wr.writerow(row)
            print("{clip}: ours {our_fps} fps | matched g1 "
                  "{mult_matched_g1}x g3 {mult_matched_g3}x | medium g1 "
                  "{mult_medium_g1}x g3 {mult_medium_g3}x | BD "
                  "{bd_rate_psnr_pct}%".format(**row))
    print(f"appended to {os.path.relpath(HISTORY, ROOT)}")


if __name__ == "__main__":
    main()
