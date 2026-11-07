#!/usr/bin/env python3
# Copyright (c) 2026, the yah265 authors
# SPDX-License-Identifier: GPL-2.0-or-later
"""Where VMAF-NEG's loss actually sits, per 32x32 block.

Phase 7 item 2 step 0 (docs/phase7-psy-design.md): the oracle the scale-split
AQ term is derived from, or refuted by. It takes a source y4m and a decode of
one of our bitstreams and computes, per block per frame, the metric's own
information terms at the metric's own scales, the design's `fine` / `coarse`
signals, the shipped AQ signal `log2(variance)`, and - joined by CTU - the
encoder's FINAL per-CTU QP offset (`N265_QP_MAP_TRACE`, encoder.c).

    python3 scripts/vmaf_lossmap.py --src tests/corpus/foreman_cif.y4m \
        --dec out.hevc --qpmap qpmap.txt --csv fm.csv

What is computed, per scale s in 0..3 (the plane 2x2-mean-downscaled s times,
the same rounded mean the lookahead builds its lowres with, block coordinates
scaled with it):

  vif_num[s], vif_den[s]  libvmaf's float VIF in its own form - local mean and
      variance over a separable Gaussian window, sigma_nsq = 2 on the 8-bit
      scale, the numerator's gain CLIPPED AT 1 (the NEG rule). Pooled per block
      by summing numerator and denominator inside it, so (den - num) is the
      block's share of the frame's VIF loss and den its share of the frame's
      information.

  dlm_num, dlm_den  a DLM-SHAPED detail-loss term, not libvmaf's DLM: a
      two-level orthonormal Haar decomposition of both planes, each restored
      detail coefficient clipped to the source's magnitude and zeroed where its
      sign disagrees (the NEG rule applied to a coefficient), then |restored|
      and |source| summed per block over the six detail subbands with the
      level-2 bands weighted DLM_W2 against the level-1 bands' 1.0. It is
      meant to RANK blocks the way DLM does, not to reproduce its value.

  fine    the block's full-resolution variance minus what the 2x-down plane
          explains (exactly the mean of the within-2x2 variances)
  coarse  the variance of the 4x-down plane over the block
  log2var log2(full-resolution variance + 1), the shipped AQ signal

VIF's per-frame frame-level ratio (sum num / sum den at each scale) is what
`--vmaf-json` validates against libvmaf's own per-frame `integer_vif_scaleN`
features: rank correlation per scale, which must clear 0.9 before any reading
is taken off the map.

numpy only; scipy is not a dependency of this tree.
"""
import argparse
import json
import math
import os
import subprocess
import sys
import tempfile

import numpy as np

SIGMA_NSQ = 2.0
EPS = 1e-10
DLM_W2 = 2.0            # level-2 (coarser) detail bands against level-1's 1.0


# ---------------------------------------------------------------- y4m / decode

def _y4m_open(path):
    f = open(path, "rb")
    hdr = f.readline().decode("latin-1")
    if not hdr.startswith("YUV4MPEG2"):
        sys.exit(f"vmaf_lossmap: {path} is not a y4m")
    w = h = None
    for tok in hdr.split():
        if tok.startswith("W"):
            w = int(tok[1:])
        elif tok.startswith("H"):
            h = int(tok[1:])
        elif tok.startswith("C") and not tok.startswith("C420"):
            sys.exit(f"vmaf_lossmap: {path} is {tok}, only 8-bit 420 is read")
    return f, w, h


def y4m_frames(path, frames=None):
    """Yield luma planes as uint8 (h, w) arrays, in the file's own order."""
    f, w, h = _y4m_open(path)
    ysz, csz = w * h, (w // 2) * (h // 2) * 2
    n = 0
    try:
        while frames is None or n < frames:
            ln = f.readline()
            if not ln:
                return
            if not ln.startswith(b"FRAME"):
                return
            buf = f.read(ysz)
            if len(buf) < ysz:
                return
            f.read(csz)
            yield np.frombuffer(buf, dtype=np.uint8).reshape(h, w)
            n += 1
    finally:
        f.close()


def as_y4m(path, work):
    """A y4m for `path`, decoding a bitstream through ffmpeg the way
    scripts/bdcompare.py does (display order)."""
    if path.endswith(".y4m"):
        return path
    out = os.path.join(work, "dec.y4m")
    r = subprocess.run(["ffmpeg", "-v", "error", "-y", "-i", path,
                        "-pix_fmt", "yuv420p", out], capture_output=True)
    if r.returncode != 0 or not os.path.exists(out):
        sys.exit(f"vmaf_lossmap: ffmpeg could not decode {path}")
    return out


# ------------------------------------------------------------------ primitives

def down2(x):
    """The lookahead's 2x2 mean, rounding and all (la.c: (a+b+c+d+2)>>2)."""
    h, w = x.shape
    h, w = h & ~1, w & ~1
    a = x[0:h:2, 0:w:2].astype(np.int32)
    b = x[0:h:2, 1:w:2].astype(np.int32)
    c = x[1:h:2, 0:w:2].astype(np.int32)
    d = x[1:h:2, 1:w:2].astype(np.int32)
    return ((a + b + c + d + 2) >> 2).astype(np.uint8)


def _gauss(n, sd):
    k = np.exp(-0.5 * ((np.arange(n) - (n - 1) / 2.0) / sd) ** 2)
    return k / k.sum()


_K = _gauss(9, 1.8)     # VIF's scale-1 window; applied in each scaled domain


def blur(x, k=_K):
    """Separable Gaussian with reflected edges, no scipy."""
    r = len(k) // 2
    y = np.pad(x, ((0, 0), (r, r)), mode="reflect")
    acc = np.zeros_like(x, dtype=np.float64)
    for i, c in enumerate(k):
        acc += c * y[:, i:i + x.shape[1]]
    y = np.pad(acc, ((r, r), (0, 0)), mode="reflect")
    acc = np.zeros_like(x, dtype=np.float64)
    for i, c in enumerate(k):
        acc += c * y[i:i + x.shape[0], :]
    return acc


def vif_terms(ref, dis):
    """Per-pixel VIF numerator and denominator in libvmaf's float form, with
    the NEG enhancement-gain clip at 1."""
    r = ref.astype(np.float64)
    d = dis.astype(np.float64)
    mu1, mu2 = blur(r), blur(d)
    s1 = np.maximum(blur(r * r) - mu1 * mu1, 0.0)
    s2 = np.maximum(blur(d * d) - mu2 * mu2, 0.0)
    s12 = blur(r * d) - mu1 * mu2
    g = s12 / (s1 + EPS)
    sv = s2 - g * s12
    # libvmaf's own degenerate cases, in its order
    lowr = s1 < EPS
    g = np.where(lowr, 0.0, g)
    sv = np.where(lowr, s2, sv)
    s1 = np.where(lowr, 0.0, s1)
    lowd = s2 < EPS
    g = np.where(lowd, 0.0, g)
    sv = np.where(lowd, 0.0, sv)
    neg = g < 0
    sv = np.where(neg, s2, sv)
    g = np.where(neg, 0.0, g)
    sv = np.maximum(sv, EPS)
    g = np.minimum(g, 1.0)                      # the NEG rule
    num = np.log2(1.0 + g * g * s1 / (sv + SIGMA_NSQ))
    den = np.log2(1.0 + s1 / SIGMA_NSQ)
    return num, den


def block_sum(x, b, nby, nbx):
    """Sum x over a nby x nbx grid of b x b blocks anchored at the origin."""
    return x[:nby * b, :nbx * b].reshape(nby, b, nbx, b).sum(axis=(1, 3))


def haar(x):
    """One orthonormal 2-D Haar level: (LL, LH, HL, HH)."""
    h, w = x.shape
    h, w = h & ~1, w & ~1
    a = x[0:h:2, 0:w:2]
    b = x[0:h:2, 1:w:2]
    c = x[1:h:2, 0:w:2]
    d = x[1:h:2, 1:w:2]
    return ((a + b + c + d) * 0.5, (a + b - c - d) * 0.5,
            (a - b + c - d) * 0.5, (a - b - c + d) * 0.5)


def dlm_terms(ref, dis, block, nby, nbx):
    """The DLM-shaped restored/source detail energy per block."""
    r = ref.astype(np.float64)
    d = dis.astype(np.float64)
    num = np.zeros((nby, nbx))
    den = np.zeros((nby, nbx))
    for lvl in (1, 2):
        rll, *rd = haar(r)
        dll, *dd = haar(d)
        w = 1.0 if lvl == 1 else DLM_W2
        bl = block >> lvl
        for sb_r, sb_d in zip(rd, dd):
            keep = np.sign(sb_r) == np.sign(sb_d)
            rest = np.where(keep, np.minimum(np.abs(sb_d), np.abs(sb_r)), 0.0)
            num += w * block_sum(rest, bl, nby, nbx)
            den += w * block_sum(np.abs(sb_r), bl, nby, nbx)
        r, d = rll, dll
    return num, den


# --------------------------------------------------------------------- ranking

def rankdata(x):
    """Average ranks, ties included (scipy.stats.rankdata, written out)."""
    x = np.asarray(x, dtype=np.float64)
    order = np.argsort(x, kind="mergesort")
    ranks = np.empty(len(x), dtype=np.float64)
    ranks[order] = np.arange(1, len(x) + 1, dtype=np.float64)
    s = x[order]
    i = 0
    while i < len(s):
        j = i
        while j + 1 < len(s) and s[j + 1] == s[i]:
            j += 1
        if j > i:
            ranks[order[i:j + 1]] = (i + 1 + j + 1) / 2.0
        i = j + 1
    return ranks


def spearman(a, b):
    ra, rb = rankdata(a), rankdata(b)
    ra -= ra.mean()
    rb -= rb.mean()
    den = math.sqrt(float((ra * ra).sum()) * float((rb * rb).sum()))
    return float((ra * rb).sum() / den) if den > 0 else float("nan")


# ----------------------------------------------------------------- the QP map

def read_qpmap(path):
    """display_idx -> (ctb, cols, rows, offsets) from N265_QP_MAP_TRACE."""
    m = {}
    if not path:
        return m
    for ln in open(path):
        if not ln.startswith("QPM,"):
            continue
        f = ln.strip().split(",")
        disp, _st, _qp, ctb, cols, rows = (int(v) for v in f[1:7])
        m[disp] = (ctb, cols, rows,
                   np.array([int(v) for v in f[7:]], dtype=np.int32))
    return m


def qp_block_map(entry, block, nby, nbx):
    ctb, cols, rows, off = entry
    off = off.reshape(rows, cols)
    by = np.minimum((np.arange(nby) * block) // ctb, rows - 1)
    bx = np.minimum((np.arange(nbx) * block) // ctb, cols - 1)
    return off[np.ix_(by, bx)].astype(np.float64)


# ------------------------------------------------------------------- the frame

def frame_row(ref, dis, block, nscales):
    """Per-block terms for one frame."""
    h, w = ref.shape
    nby, nbx = h // block, w // block
    out = {}
    r, d = ref, dis
    for s in range(nscales):
        num, den = vif_terms(r, d)
        b = block >> s
        out[f"vif_num{s}"] = block_sum(num, b, nby, nbx)
        out[f"vif_den{s}"] = block_sum(den, b, nby, nbx)
        out[f"_fnum{s}"] = float(num.sum())
        out[f"_fden{s}"] = float(den.sum())
        if s + 1 < nscales:
            r, d = down2(r), down2(d)
    dn, dd = dlm_terms(ref, dis, block, nby, nbx)
    out["dlm_num"], out["dlm_den"] = dn, dd
    # squared error over the same blocks: not a VMAF term, the contrast the
    # reading needs (how much flatter than SSD the metric's own loss is)
    e = ref.astype(np.float64) - dis.astype(np.float64)
    out["ssd"] = block_sum(e * e, block, nby, nbx)
    # the three signals, all off the SOURCE
    f = ref.astype(np.float64)
    n = float(block * block)
    s1 = block_sum(f, block, nby, nbx)
    s2 = block_sum(f * f, block, nby, nbx)
    var = np.maximum(s2 / n - (s1 / n) ** 2, 0.0)
    d2 = down2(ref).astype(np.float64)
    n2 = float((block // 2) ** 2)
    t1 = block_sum(d2, block // 2, nby, nbx)
    t2 = block_sum(d2 * d2, block // 2, nby, nbx)
    var2 = np.maximum(t2 / n2 - (t1 / n2) ** 2, 0.0)
    d4 = down2(down2(ref)).astype(np.float64)
    n4 = float((block // 4) ** 2)
    u1 = block_sum(d4, block // 4, nby, nbx)
    u2 = block_sum(d4 * d4, block // 4, nby, nbx)
    out["fine"] = np.maximum(var - var2, 0.0)
    out["coarse"] = np.maximum(u2 / n4 - (u1 / n4) ** 2, 0.0)
    out["log2var"] = np.log2(var + 1.0)
    return out, nby, nbx


# ------------------------------------------------------------------ the tables

COLS = ["frame", "by", "bx"]


def summarise(rows, nscales, label, w=None):
    """The design's three questions, printed as one table per clip."""
    fr = rows["frame"]
    nfr = int(fr.max()) + 1 if len(fr) else 0
    # per-frame normalized loss share, per scale and combined
    share = {}
    for s in range(nscales):
        lo = rows[f"vif_den{s}"] - rows[f"vif_num{s}"]
        share[f"vif{s}"] = _per_frame_share(lo, fr, nfr)
    share["vif"] = sum(share[f"vif{s}"] for s in range(nscales)) / nscales
    share["dlm"] = _per_frame_share(rows["dlm_den"] - rows["dlm_num"], fr, nfr)
    share["ssd"] = _per_frame_share(rows["ssd"], fr, nfr)

    out = []
    out.append(f"#### {label}")
    out.append("")
    out.append("Concentration (share of the frame's loss in the top blocks, "
               "mean over frames):")
    out.append("")
    out.append("| loss term | top 10% | top 25% | bottom 50% |")
    out.append("|---|---|---|---|")
    for k, nm in ([(f"vif{s}", f"VIF scale {s}") for s in range(nscales)]
                  + [("vif", "VIF, 4 scales"), ("dlm", "DLM-shaped"),
                     ("ssd", "squared error")]):
        t10, t25, b50 = _conc(share[k], fr, nfr)
        out.append(f"| {nm} | {t10 * 100:.1f}% | {t25 * 100:.1f}% "
                   f"| {b50 * 100:.1f}% |")
    out.append("")
    sig = [("fine", rows["fine"]), ("coarse", rows["coarse"]),
           ("log2var", rows["log2var"])]
    if "aq_off" in rows:
        sig.append(("AQ offset", rows["aq_off"]))
    out.append("Rank correlation of the block's loss share with each signal "
               "(Spearman, all blocks of all frames):")
    out.append("")
    out.append("| loss term | " + " | ".join(n for n, _ in sig) + " |")
    out.append("|---" * (len(sig) + 1) + "|")
    for k, nm in [("vif", "VIF, 4 scales"), ("vif0", "VIF scale 0"),
                  ("vif2", "VIF scale 2"), ("vif3", "VIF scale 3"),
                  ("dlm", "DLM-shaped"), ("ssd", "squared error")]:
        vals = " | ".join(f"{spearman(share[k], v):+.3f}" for _, v in sig)
        out.append(f"| {nm} | {vals} |")
    out.append("")
    out.append("How much the three signals are the same signal on this clip "
               "(Spearman between them):")
    out.append("")
    out.append("| pair | rank corr |")
    out.append("|---|---|")
    for an, bn in (("fine", "coarse"), ("fine", "log2var"),
                   ("coarse", "log2var")):
        out.append(f"| {an} vs {bn} | "
                   f"{spearman(rows[an], rows[bn]):+.3f} |")
    out.append("")
    out.append("Loss by quadrant of the frame's own medians of `fine` and "
               "`coarse` (share of the frame's VIF loss; 25% would be flat):")
    out.append("")
    out.append("| quadrant | blocks | VIF loss | DLM loss | SSD | mean "
               "log2var | mean AQ off |")
    out.append("|---|---|---|---|---|---|---|")
    fm = _per_frame_median(rows["fine"], fr, nfr)
    cm = _per_frame_median(rows["coarse"], fr, nfr)
    hf = rows["fine"] > fm
    hc = rows["coarse"] > cm
    for nmf, mf in (("low fine", ~hf), ("high fine", hf)):
        for nmc, mc in (("low coarse", ~hc), ("high coarse", hc)):
            m = mf & mc
            if not m.any():
                continue
            aq = (f"{rows['aq_off'][m].mean():+.2f}"
                  if "aq_off" in rows else "-")
            out.append(f"| {nmf} / {nmc} | {m.mean() * 100:.1f}% "
                       f"| {share['vif'][m].sum() / nfr * 100:.1f}% "
                       f"| {share['dlm'][m].sum() / nfr * 100:.1f}% "
                       f"| {share['ssd'][m].sum() / nfr * 100:.1f}% "
                       f"| {rows['log2var'][m].mean():.2f} | {aq} |")
    out.append("")
    return "\n".join(out)


def _per_frame_share(lo, fr, nfr):
    tot = np.zeros(nfr)
    np.add.at(tot, fr, lo)
    tot[tot <= 0] = 1.0
    return lo / tot[fr]


def _per_frame_median(v, fr, nfr):
    med = np.zeros(nfr)
    for i in range(nfr):
        m = fr == i
        med[i] = np.median(v[m]) if m.any() else 0.0
    return med[fr]


def _conc(share, fr, nfr):
    t10 = t25 = b50 = 0.0
    for i in range(nfr):
        s = np.sort(share[fr == i])[::-1]
        n = len(s)
        if n == 0:
            continue
        t10 += s[:max(1, int(round(n * 0.10)))].sum()
        t25 += s[:max(1, int(round(n * 0.25)))].sum()
        b50 += s[int(round(n * 0.50)):].sum()
    return t10 / nfr, t25 / nfr, b50 / nfr


# ------------------------------------------------------------------ validation

def validate(frame_ratio, js, nscales):
    """Per-frame frame-level VIF ratio against libvmaf's own features."""
    d = json.load(open(js))
    out = ["| scale | ours (mean) | libvmaf (mean) | rank corr | Pearson |",
           "|---|---|---|---|---|"]
    ok = True
    for s in range(nscales):
        key = None
        for cand in (f"integer_vif_scale{s}_egl_1", f"integer_vif_scale{s}",
                     f"vif_scale{s}"):
            if cand in d["frames"][0]["metrics"]:
                key = cand
                break
        if key is None:
            out.append(f"| {s} | - | libvmaf feature missing | - | - |")
            ok = False
            continue
        lib = np.array([f["metrics"][key] for f in d["frames"]])
        mine = np.array(frame_ratio[s])
        n = min(len(lib), len(mine))
        lib, mine = lib[:n], mine[:n]
        rc = spearman(mine, lib)
        pc = float(np.corrcoef(mine, lib)[0, 1])
        out.append(f"| {s} | {mine.mean():.4f} | {lib.mean():.4f} "
                   f"| {rc:+.3f} | {pc:+.3f} |")
        ok = ok and rc > 0.9
    return "\n".join(out), ok


# ------------------------------------------------------------------------ main

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--src", required=True)
    ap.add_argument("--dec", required=True, help="decoded y4m or a bitstream")
    ap.add_argument("--block", type=int, default=32)
    ap.add_argument("--scales", type=int, default=4)
    ap.add_argument("--frames", type=int, default=120)
    ap.add_argument("--qpmap", default=None,
                    help="N265_QP_MAP_TRACE file from the same encode")
    ap.add_argument("--csv", default=None)
    ap.add_argument("--vmaf-json", default=None,
                    help="libvmaf --json output for the same pair; validates "
                         "the instrument and exits nonzero if it fails")
    ap.add_argument("--label", default=None)
    args = ap.parse_args()

    work = tempfile.mkdtemp(prefix="lossmap.")
    dec = as_y4m(args.dec, work)
    qpm = read_qpmap(args.qpmap)

    cols = {}
    frame_ratio = [[] for _ in range(args.scales)]
    nfr = 0
    for i, (ref, dis) in enumerate(zip(y4m_frames(args.src, args.frames),
                                       y4m_frames(dec, args.frames))):
        if ref.shape != dis.shape:
            sys.exit("vmaf_lossmap: source and decode differ in size")
        r, nby, nbx = frame_row(ref, dis, args.block, args.scales)
        for s in range(args.scales):
            frame_ratio[s].append(r[f"_fnum{s}"] / max(r[f"_fden{s}"], EPS))
        flat = {k: v.reshape(-1) for k, v in r.items()
                if not k.startswith("_")}
        flat["frame"] = np.full(nby * nbx, i)
        flat["by"] = np.repeat(np.arange(nby), nbx)
        flat["bx"] = np.tile(np.arange(nbx), nby)
        if i in qpm:
            flat["aq_off"] = qp_block_map(qpm[i], args.block,
                                          nby, nbx).reshape(-1)
        for k, v in flat.items():
            cols.setdefault(k, []).append(v)
        nfr = i + 1
    if not nfr:
        sys.exit("vmaf_lossmap: no frames read")
    rows = {k: np.concatenate(v) for k, v in cols.items()}

    if args.vmaf_json:
        tbl, ok = validate(frame_ratio, args.vmaf_json, args.scales)
        print(tbl)
        print("validation: " + ("PASSED" if ok else "FAILED (rank corr <= 0.9)"))
        if not ok:
            sys.exit(1)

    if args.csv:
        order = ([c for c in COLS]
                 + [f"vif_num{s}" for s in range(args.scales)]
                 + [f"vif_den{s}" for s in range(args.scales)]
                 + ["dlm_num", "dlm_den", "ssd", "fine", "coarse", "log2var"]
                 + (["aq_off"] if "aq_off" in rows else []))
        with open(args.csv, "w") as f:
            f.write(",".join(order) + "\n")
            data = np.stack([rows[c].astype(np.float64) for c in order], 1)
            for r in data:
                f.write(",".join(f"{v:.6g}" for v in r) + "\n")

    print(summarise(rows, args.scales,
                    args.label or os.path.basename(args.src)))


if __name__ == "__main__":
    main()
