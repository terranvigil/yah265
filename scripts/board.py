#!/usr/bin/env python3
"""Parity board: measure yah265 against pinned x265 and append one row
per clip to docs/data/parity-history.csv so progress charts over time.

Two x265 arms, single-threaded:
Three of our own legs since 2026-09-10: the default (medium) preset for
both the timed and the BD columns, and `--preset fast` for one timed
column and one BD column, so goal 1 can also be read AT ISO-QUALITY -
`our_fps_fast` against `x265m_noasm_fps`, with
`bd_rate_vmafneg_fast_pct` saying what that speed cost. Empty on every
row written before the columns existed.

- "matched": x265 configured down to our current featureset (kept in
  X265_MATCHED below; grows with the encoder until it equals medium) -
  the like-for-like progress line, and the BD-rate anchor.
- "medium": x265 pure defaults - the goal line the legs are defined on.
Speed multiple = our_time / x265_time (1.00 = parity, lower = faster).
BD-rate (Y-PSNR) vs the matched arm; negative = we spend fewer bits at
equal PSNR.

Goal 1 is the single-threaded leg: ours at --threads 1 against the
matched arm's --no-asm run. Goal 2 is the utilization leg, our_fps_auto
against x265med_asm_fps_auto - both encoders at their own default thread
configuration (ours auto since the T3 merge, x265 at default pools and
frame threads). The BD encodes also run at our default, which the
determinism gate makes bit-neutral.

Timing rules (docs/instruments.md): output to /dev/null, median of
NRUNS, sizes measured in a separate untimed pass.

usage: scripts/board.py [--quick] [--bd-only] [--synth] [--clips a,b] [--note TEXT]
  --bd-only  skip timing (loaded machine); BD columns only
  --synth    force the synthetic clips even when tests/corpus exists
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

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__))))
import bdcompare as _bd
_bd.SUBSAMPLE = 1

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
N265 = os.environ.get("N265", os.path.join(ROOT, "build/n265"))
X265 = os.environ.get("X265", "x265")
FFMPEG = os.environ.get("FFMPEG", "ffmpeg")
HISTORY = os.environ.get("BOARD_HISTORY", os.path.join(ROOT, "docs/data/parity-history.csv"))
NRUNS = 3
QPS = [22, 27, 32, 37]

# x265 args mirroring our current featureset (update as features land;
# the goal is for this list to shrink to nothing)
# x265 configured DOWN to our current featureset, for the like-for-like BD
# anchor. Grows as the encoder does; now: hierarchical B (bframes 3, fast
# adaptive), one reference, rectangular and asymmetric inter partitions
# (both landed 2026-09-07, so the --no-rect/--no-amp handicaps are gone).
# What still separates this from plain medium: --ref 1 and the lookahead
# depth. (2026-09-07). keyint moved 60 -> 250 with our own default on
# 2026-09-09, so the matched arm tracks it. (2026-09-09)
# bframes tracks our own default, which moved 3 -> 7 on 2026-09-10; the
# arm mirrors OUR featureset, so it moves with it. (2026-09-10)
X265_MATCHED = ["--keyint", "250", "--bframes", "7", "--b-adapt", "1",
                "--rc-lookahead", "12", "--ref", "1", "--no-open-gop"]

SYNTH_CLIPS = [
    # deterministic synthetic fallback (--synth, or no corpus fetched)
    ("synth_detail_cif", "352x288", 30,
     "testsrc2=size=352x288:rate=25,noise=alls=8:allf=t:all_seed=7"),
    ("synth_smooth_cif", "352x288", 30,
     "gradients=size=352x288:rate=25:seed=11"),
    ("synth_detail_720", "1280x720", 10,
     "testsrc2=size=1280x720:rate=25,noise=alls=6:allf=t:all_seed=7"),
]

CORPUS = os.path.join(ROOT, "tests/corpus")
CORPUS_FRAMES = 120     # truncate long clips; plenty for a 4-point sweep


def corpus_clips():
    """(name, size, frames, path) from tests/corpus/CLASSES, or None."""
    manifest = os.path.join(CORPUS, "CLASSES")
    if not os.path.exists(manifest):
        return None
    out = []
    for line in open(manifest):
        name = line.split()[0]
        path = os.path.join(CORPUS, name + ".y4m")
        if not os.path.exists(path):
            continue
        hdr = open(path, "rb").readline().decode()
        w = re.search(r" W(\d+)", hdr).group(1)
        h = re.search(r" H(\d+)", hdr).group(1)
        out.append((name, f"{w}x{h}", CORPUS_FRAMES, path))
    return out or None


def run(cmd, **kw):
    return subprocess.run(cmd, check=True, capture_output=True, **kw)


def gen_clip(tmp, name, filt, frames):
    path = os.path.join(tmp, name + ".y4m")
    run([FFMPEG, "-loglevel", "error", "-f", "lavfi", "-i", filt,
         "-frames:v", str(frames), "-pix_fmt", "yuv420p", path, "-y"])
    return path


def warm_file(path):
    """Read the input into page cache before timing. x265 mmaps its
    input while we stream ours, so a cold file lands in the two wall
    clocks asymmetrically (yah264 lesson: ~49% of a profile could be
    __mmap on a cold multi-GB y4m). One untimed sequential read puts
    both encoders on the same warm footing."""
    with open(path, "rb") as f:
        while f.read(1 << 22):
            pass


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
         # frame-index alignment: corpus y4m rates (29.97) differ from the
         # rawvideo default; timestamp sync would compare wrong frames.
         # shortest=1 stops at the truncated encode length.
         "-lavfi", "[0:v]settb=1/25,setpts=N[d];[1:v]settb=1/25,"
                   "setpts=N[r];[d][r]psnr=shortest=1",
         "-f", "null", "-"],
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


def measure_clip(tmp, name, size, frames, src, bd_only):
    if os.path.exists(str(src)):
        y4m = src
    else:
        y4m = gen_clip(tmp, name, src, frames)
    lim = ["--frames", str(frames)]
    row = {"clip": name, "frames": frames}

    ours_pts = []
    ours_neg = []       # (bits, VMAF-NEG): the leg the quality decisions use
    for qp in QPS:
        out = os.path.join(tmp, f"n_{name}_{qp}.hevc")
        rec = os.path.join(tmp, f"n_{name}_{qp}.yuv")
        run([N265, y4m, "-o", out, "--qp", str(qp),
             "--dump-recon", rec] + lim)
        ours_pts.append((os.path.getsize(out) * 8, psnr_y(rec, y4m, size)))
        v = _bd.vmaf_of(out, y4m, tmp)
        ours_neg.append((os.path.getsize(out) * 8, v["VMAF-NEG"] if v and "VMAF-NEG" in v else None))

    # the fast preset's own BD leg: the same four QPs against the same
    # matched anchor, so the two BD columns differ only by the preset
    fast_neg = []
    for qp in QPS:
        out = os.path.join(tmp, f"nf_{name}_{qp}.hevc")
        run([N265, y4m, "-o", out, "--qp", str(qp), "--preset", "fast"] + lim)
        v = _bd.vmaf_of(out, y4m, tmp)
        fast_neg.append((os.path.getsize(out) * 8,
                         v["VMAF-NEG"] if v and "VMAF-NEG" in v else None))

    st = ["--pools", "none", "--frame-threads", "1", "--log-level", "error"]
    matched = [X265, "--input", y4m] + st + X265_MATCHED + lim
    medium = [X265, "--input", y4m] + st + lim
    x_pts = []
    x_neg = []
    for qp in QPS:
        out = os.path.join(tmp, f"x_{name}_{qp}.hevc")
        run(matched + ["--qp", str(qp), "-o", out])
        dec = os.path.join(tmp, f"x_{name}_{qp}.yuv")
        run([FFMPEG, "-loglevel", "error", "-i", out, "-f", "rawvideo",
             dec, "-y"])
        x_pts.append((os.path.getsize(out) * 8, psnr_y(dec, y4m, size)))
        v = _bd.vmaf_of(out, y4m, tmp)
        x_neg.append((os.path.getsize(out) * 8, v["VMAF-NEG"] if v and "VMAF-NEG" in v else None))
    row["bd_rate_psnr_pct"] = round(bd_rate(x_pts, ours_pts), 2)
    # BD-rate on VMAF-NEG against the same matched arm. PSNR and VMAF-NEG
    # can disagree on purpose (SAO buys PSNR and pays VMAF-NEG, 2026-09-09);
    # the band's decisions are made on VMAF-NEG, so the chart carries both.
    if all(p[1] is not None for p in ours_neg + x_neg):
        r = _bd.bd_rate([p[0] for p in x_neg], [p[1] for p in x_neg],
                        [p[0] for p in ours_neg], [p[1] for p in ours_neg])
        # The board's fixed QPs are not band-calibrated, so VMAF-NEG can
        # saturate and the cubic fit degenerate (bus_cif read 1e56%, then
        # 1e81% at bframes 7). Since 2026-09-10 bdcompare.bd_rate catches
        # that itself and integrates a monotone fit instead, so the row is
        # charted rather than blanked; this bar is the last-resort guard
        # only. The band instrument is still the authority on the leg -
        # the board's QPs put bus's top two points inside a hundredth of a
        # VMAF-NEG point of each other, and no integrator recovers
        # information that is not there.
        ok = isinstance(r, float) and math.isfinite(r) and abs(r) < 200
        row["bd_rate_vmafneg_pct"] = round(r, 2) if ok else ""
    else:
        row["bd_rate_vmafneg_pct"] = ""
    if all(p[1] is not None for p in fast_neg + x_neg):
        r = _bd.bd_rate([p[0] for p in x_neg], [p[1] for p in x_neg],
                        [p[0] for p in fast_neg], [p[1] for p in fast_neg])
        ok = isinstance(r, float) and math.isfinite(r) and abs(r) < 200
        row["bd_rate_vmafneg_fast_pct"] = round(r, 2) if ok else ""
    else:
        row["bd_rate_vmafneg_fast_pct"] = ""

    if bd_only:
        # timing skipped (loaded machine); columns stay empty
        for k in ("our_fps", "x265m_noasm_fps", "x265m_asm_fps",
                  "mult_matched_g1", "mult_matched_g3", "x265med_noasm_fps",
                  "x265med_asm_fps", "mult_medium_g1", "mult_medium_g3",
                  "our_fps_auto", "x265med_asm_fps_auto", "our_fps_fast"):
            row[k] = ""
        return row

    warm_file(y4m)
    # Goal 1 is single-thread by definition, so the timed leg says so
    # explicitly - our default is auto since the T3 merge (2026-09-09) and
    # x265's arm is already --pools none --frame-threads 1.
    t_ours = time_cmd([N265, y4m, "-o", "/dev/null", "--qp", "27",
                       "--threads", "1"] + lim)
    # goal 1 at iso-quality: the same leg with the fast preset, whose BD
    # column above says what it gave up to get there
    t_ours_fast = time_cmd([N265, y4m, "-o", "/dev/null", "--qp", "27",
                            "--threads", "1", "--preset", "fast"] + lim)
    t_m_noasm = time_cmd(matched + ["--no-asm", "--qp", "27", "-o",
                                    "/dev/null"])
    t_m_asm = time_cmd(matched + ["--qp", "27", "-o", "/dev/null"])
    t_med_noasm = time_cmd(medium + ["--no-asm", "--crf", "28", "-o",
                                     "/dev/null"])
    t_med_asm = time_cmd(medium + ["--crf", "28", "-o", "/dev/null"])
    row["our_fps"] = round(frames / t_ours, 2)
    row["our_fps_fast"] = round(frames / t_ours_fast, 2)
    row["x265m_noasm_fps"] = round(frames / t_m_noasm, 2)
    row["x265m_asm_fps"] = round(frames / t_m_asm, 2)
    row["mult_matched_g1"] = round(t_ours / t_m_noasm, 3)
    row["mult_matched_g3"] = round(t_ours / t_m_asm, 3)
    row["x265med_noasm_fps"] = round(frames / t_med_noasm, 2)
    row["x265med_asm_fps"] = round(frames / t_med_asm, 2)
    row["mult_medium_g1"] = round(t_ours / t_med_noasm, 3)
    row["mult_medium_g3"] = round(t_ours / t_med_asm, 3)

    # Goal 2, the utilization leg: both encoders at their own default
    # thread configuration - ours at --threads 0 (one worker per online
    # CPU), x265 medium at default pools and frame threads. Read as a wall
    # ratio, x265med_asm_fps_auto / our_fps_auto. Determinism makes our
    # auto run bit-identical to the --threads 1 one, so this measures
    # scheduling only.
    med_auto = [X265, "--input", y4m, "--log-level", "error"] + lim
    t_ours_auto = time_cmd([N265, y4m, "-o", "/dev/null", "--qp", "27"] + lim)
    t_med_auto = time_cmd(med_auto + ["--crf", "28", "-o", "/dev/null"])
    row["our_fps_auto"] = round(frames / t_ours_auto, 2)
    row["x265med_asm_fps_auto"] = round(frames / t_med_auto, 2)
    return row


def main():
    quick = "--quick" in sys.argv
    bd_only = "--bd-only" in sys.argv
    synth = "--synth" in sys.argv
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
              "mult_medium_g1", "mult_medium_g3", "our_fps_auto",
              "x265med_asm_fps_auto", "our_fps_fast", "bd_rate_psnr_pct",
              "bd_rate_vmafneg_pct", "bd_rate_vmafneg_fast_pct", "note"]
    os.makedirs(os.path.dirname(HISTORY), exist_ok=True)
    new = not os.path.exists(HISTORY)

    clips = None if synth else corpus_clips()
    if clips is None:
        clips = SYNTH_CLIPS
    if quick:
        clips = clips[:1]
    if "--clips" in sys.argv:
        only = sys.argv[sys.argv.index("--clips") + 1].split(",")
        clips = [c for c in clips if c[0] in only]
    with tempfile.TemporaryDirectory() as tmp, \
         open(HISTORY, "a", newline="") as f:
        wr = csv.DictWriter(f, fieldnames=fields)
        if new:
            wr.writeheader()
        for name, size, frames, src in clips:
            row = measure_clip(tmp, name, size, frames, src, bd_only)
            row.update(date=datetime.date.today().isoformat(),
                       commit=commit, x265=xv, note=note)
            wr.writerow(row)
            if bd_only:
                print("{clip}: BD-PSNR {bd_rate_psnr_pct}% BD-VMAF-NEG "
                      "{bd_rate_vmafneg_pct}% | fast {bd_rate_vmafneg_fast_pct}%".format(**row))
            else:
                print("{clip}: ours {our_fps} fps | matched g1 "
                      "{mult_matched_g1}x g3 {mult_matched_g3}x | medium g1 "
                      "{mult_medium_g1}x g3 {mult_medium_g3}x | auto "
                      "{our_fps_auto} vs {x265med_asm_fps_auto} fps | fast "
                      "{our_fps_fast} fps NEG {bd_rate_vmafneg_fast_pct}% | BD "
                      "{bd_rate_psnr_pct}% / NEG {bd_rate_vmafneg_pct}%".format(**row))
    print(f"appended to {os.path.relpath(HISTORY, ROOT)}")


if __name__ == "__main__":
    main()
