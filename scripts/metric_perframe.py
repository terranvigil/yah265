#!/usr/bin/env python3
# Copyright (c) 2026, the yah265 authors
# SPDX-License-Identifier: GPL-2.0-or-later
"""Where VMAF-NEG and PSNR-Y disagree on one clip, frame by frame.

One clip, one arm of ours, at the band's rung-2 byte target; x265 medium's
CRF is then bisected in 0.05 steps onto OUR bytes, so the comparison is at
EQUAL BYTES and not at equal-ish rungs. Per-frame VMAF-NEG comes off
libvmaf's own json and per-frame PSNR-Y off ffmpeg's psnr stats_file, both
in display order, joined to the frame type ffmpeg's decoder reports for
each stream (showinfo, the same decode order the y4m goes out in).

  python3 scripts/metric_perframe.py bus_cif stefan_cif
  TAG=aq0 ARM='--crf {q} --aq 0 --cutree 1' \
      python3 scripts/metric_perframe.py bus_cif

Built and read for phase 13 B4 (docs/phase13.md "B4"), which is where the
readings and the caveats are. The one caveat worth repeating here: the two
encoders choose DIFFERENT GOPs, so a per-type split that ignores that is a
GOP-alignment artifact. The `both typed` rows are the honest ones, and the
row that says how many frames the split could not read is printed beside
them.

It shares bd_at_rate's encode memo, so the point it reads is the band's own
point and not a fresh encode. x265's CRF is bisected in 0.05 steps, which
IS a fresh encode ladder on the reference side - run it under the box's
clock lock like any other band work.
"""
import json, os, re, subprocess, sys, tempfile

sys.path.insert(0, "scripts")
import bdcompare as bd, bd_at_rate as bar

FR = 120
# Decodes and per-frame json/stats land here. WORK names a directory to
# keep them in (handy for a second look); otherwise a temp dir.
WORK = os.environ.get("WORK") or tempfile.mkdtemp(prefix="perframe.")
TAG = os.environ.get("TAG", "base")
ARM = os.environ.get("ARM", "--crf {q} --aq 1 --cutree 1")
os.makedirs(WORK, exist_ok=True)


def sh(c):
    return subprocess.run(c, shell=True, capture_output=True, text=True)


def fps_of(c):
    h = open("tests/corpus/%s.y4m" % c, "rb").readline().decode("latin-1")
    m = re.search(r" F(\d+):(\d+)", h)
    return int(m.group(1)) / int(m.group(2))


def cut(clip):
    p = os.path.join(WORK, clip + ".src.y4m")
    if not os.path.exists(p):
        sh(f'ffmpeg -v error -y -i tests/corpus/{clip}.y4m -frames:v {FR} "{p}"')
    return p


def dec(bs, clip, tag):
    p = os.path.join(WORK, f"{clip}.{tag}.y4m")
    sh(f'ffmpeg -v error -y -export_side_data film_grain -i "{bs}" '
       f'-pix_fmt yuv420p "{p}"')
    return p


def per_frame_vmaf(src, decy4m, out):
    sh(f'vmaf -r "{src}" -d "{decy4m}" --json -o "{out}" '
       f'--model version=vmaf_v0.6.1neg:name=vmaf_neg --threads 6')
    return [f["metrics"]["vmaf_neg"] for f in json.load(open(out))["frames"]]


def per_frame_psnr(src, decy4m, out):
    sh(f'ffmpeg -v error -y -i "{decy4m}" -i "{src}" '
       f'-lavfi "[0:v][1:v]psnr=stats_file={out}" -f null -')
    ys = []
    for ln in open(out):
        m = re.search(r"psnr_y:([0-9.]+|inf)", ln)
        if m:
            ys.append(99.0 if m.group(1) == "inf" else float(m.group(1)))
    return ys


def types(bs):
    r = sh(f'ffmpeg -v info -i "{bs}" -vf showinfo -f null -')
    return re.findall(r"type:([IPB])", r.stderr)


def x265_at_bytes(clip, src, want):
    out = os.path.join(WORK, clip + ".x.hevc")
    tmpl = ("x265 --preset medium --crf {q} --frames %d --pools none "
            "--frame-threads 1 --log-level none -o {out} {src}" % FR)
    lo, hi = 5.0, 51.0
    while hi - lo > 0.05:
        mid = round((lo + hi) / 2, 2)
        if bar.enc(tmpl, mid, src, out) > want:
            lo = mid
        else:
            hi = mid
    best = min((abs(bar.enc(tmpl, q, src, out) - want), q) for q in (lo, hi))[1]
    return out, best, bar.enc(tmpl, best, src, out)


def pct(xs, p):
    s = sorted(xs)
    return s[min(len(s) - 1, int(p * len(s)))]


for clip in sys.argv[1:]:
    src = cut(clip)
    full = "tests/corpus/%s.y4m" % clip
    lad = json.load(open("scripts/ladders.json"))[clip]
    target = int(lad[1] * 1000.0 / 8.0 * (FR / fps_of(clip)))     # rung 2
    ourt = "build/n265 {src} --frames %d %s -o {out}" % (FR, ARM)
    obs = os.path.join(WORK, f"{clip}.{TAG}.hevc")
    oq = bar.solve(ourt, full, target, obs, {})
    osz = bar.enc(ourt, oq, full, obs)
    xbs, xq, xsz = x265_at_bytes(clip, full, osz)
    print(f"\n### {clip} [{TAG}: {ARM}]\n    ours crf {oq} {osz} B   "
          f"x265 crf {xq} {xsz} B  ({100.0*(xsz-osz)/osz:+.2f}% bytes)")

    res = {}
    for tag, bs in (("ours", obs), ("x265", xbs)):
        dy = dec(bs, clip, tag)
        res[tag] = (per_frame_vmaf(src, dy, os.path.join(WORK, f"{clip}.{tag}.json")),
                    per_frame_psnr(src, dy, os.path.join(WORK, f"{clip}.{tag}.psnr")),
                    types(bs))
        os.unlink(dy)
    ov, op, ot = res["ours"]
    xv, xp, xt = res["x265"]
    n = min(len(ov), len(xv), len(op), len(xp), len(ot), len(xt))
    dv = [ov[i] - xv[i] for i in range(n)]
    dp = [op[i] - xp[i] for i in range(n)]
    print(f"    {n} frames.  pooled VMAF-NEG ours {sum(ov[:n])/n:.3f} vs "
          f"x265 {sum(xv[:n])/n:.3f}   ({sum(dv)/n:+.3f})")
    print(f"                 pooled PSNR-Y    ours {sum(op[:n])/n:.3f} vs "
          f"x265 {sum(xp[:n])/n:.3f}   ({sum(dp)/n:+.3f} dB)")
    print(f"    our GOP {''.join(ot[:n])[:48]}")
    print(f"    x265    {''.join(xt[:n])[:48]}")

    def grp(sel, name):
        idx = [i for i in range(n) if sel(i)]
        if not idx:
            return
        print(f"    {name:<26} n={len(idx):>3}   dVMAF-NEG "
              f"{sum(dv[i] for i in idx)/len(idx):+7.3f}   dPSNR-Y "
              f"{sum(dp[i] for i in idx)/len(idx):+7.3f} dB")

    grp(lambda i: True, "all frames")
    grp(lambda i: i == 0, "frame 0 (both I)")
    grp(lambda i: i > 0, "all but frame 0")
    # The type split is only honest where BOTH encoders chose the same
    # type at that display index: the two GOPs differ, so "ours typed P"
    # against whatever x265 put there is a GOP-alignment artifact.
    for t in ("P", "B"):
        grp(lambda i, t=t: ot[i] == t and xt[i] == t, f"both typed {t}")
    grp(lambda i: ot[i] != xt[i], "types differ (not a reading)")
    q = max(1, n // 4)
    for k in range(4):
        grp(lambda i, k=k: k * q <= i < (k + 1) * q, f"quarter {k+1}")
    print(f"    dPSNR-Y spread: p10 {pct(dp,0.10):+.3f}  median "
          f"{pct(dp,0.50):+.3f}  p90 {pct(dp,0.90):+.3f} dB;   ours ahead on "
          f"{sum(1 for x in dp if x>0)}/{n} frames")
    print(f"    dVMAF-NEG spread: p10 {pct(dv,0.10):+.3f}  median "
          f"{pct(dv,0.50):+.3f}  p90 {pct(dv,0.90):+.3f};   ours ahead on "
          f"{sum(1 for x in dv if x>0)}/{n} frames")
    # do the two deltas even rank the frames the same way?
    try:
        import numpy as np
        rv = np.argsort(np.argsort(dv)).astype(float)
        rp = np.argsort(np.argsort(dp)).astype(float)
        print(f"    Spearman(dVMAF-NEG, dPSNR-Y) over frames: "
              f"{np.corrcoef(rv, rp)[0,1]:+.3f}")
    except ImportError:
        pass
