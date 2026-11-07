#!/usr/bin/env python3
# Copyright (c) 2026, the yah265 authors
# SPDX-License-Identifier: GPL-2.0-or-later
"""The per-component rate-distortion SLOPE, and the chroma weight it implies.

The mode decision's chroma weight `N265_CHROMA_W` was derived from the ratio of
the two quantizers' squared steps, `2^((qp_y - qp_c)/3)`. That prices chroma
DISTORTION against luma's and assumes chroma RATE behaves like luma's. This
measures the assumption: per component, the coded bits (N265_CBIT_TRACE, exact
attribution) and the SSD of the encoder's own reconstruction against the
source, at a contiguous run of integer CRFs spanning the band's ladder. From
adjacent points,

    lambda_x(qp) = -dR_x / dD_x        (bits per unit of SSD removed)
    w(qp)        = lambda_y / lambda_c

is the weight a chroma SSD term should carry so that it is traded for bits at
CHROMA's own exchange rate rather than at luma's.

    CLIPS=bus_cif,stefan_cif FRAMES=120 JOBS=3 python3 scripts/chroma_slope.py
    python3 scripts/chroma_slope.py --analyze out.csv

Writes one CSV row per (clip, crf). numpy only.
"""
import csv, json, os, re, subprocess, sys, tempfile
from concurrent.futures import ThreadPoolExecutor

import numpy as np

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
HERE = os.path.join(ROOT, "scripts")
FRAMES = int(os.environ.get("FRAMES", "120"))
N = os.environ.get("N265", os.path.join(ROOT, "build", "n265"))
ARGS = os.environ.get("ARGS", "--aq 1 --cutree 1")
THREADS = os.environ.get("THREADS", "4")
JOBS = int(os.environ.get("JOBS", "3"))
OUT = os.environ.get("OUT", os.path.join(ROOT, "local", "chroma_slope.csv"))
TMP = os.environ.get("TMPDIR", "/tmp")
ladders = json.load(open(os.path.join(HERE, "ladders.json")))
sel = os.environ.get("CLIPS", "")
CLIPS = sel.split(",") if sel else list(ladders)


def src_of(clip):
    return os.path.join(ROOT, "tests", "corpus", clip + ".y4m")


def y4m_geom(clip):
    with open(src_of(clip), "rb") as f:
        hdr = f.readline().decode("latin-1")
    w = int(re.search(r" W(\d+)", hdr).group(1))
    h = int(re.search(r" H(\d+)", hdr).group(1))
    m = re.search(r" F(\d+):(\d+)", hdr)
    fps = int(m.group(1)) / int(m.group(2)) if m else 30.0
    return w, h, fps, len(hdr)


def encode(clip, crf, recon=None, trace=False):
    """One encode. Returns (bytes, CBIT rows) with the recon written if asked."""
    fd, out = tempfile.mkstemp(suffix=".hevc", dir=TMP)
    os.close(fd)
    cmd = [N, src_of(clip), "--frames", str(FRAMES), "--crf", str(crf),
           "--threads", THREADS] + ARGS.split() + ["-o", out]
    if recon:
        cmd += ["--dump-recon", recon]
    env = dict(os.environ)
    if trace:
        env["N265_CBIT_TRACE"] = "1"
    p = subprocess.run(cmd, capture_output=True, env=env)
    n = os.path.getsize(out)
    os.unlink(out)
    rows = []
    for line in p.stderr.decode("latin-1").splitlines():
        if line.startswith("CBIT,"):
            f = line.split(",")
            rows.append(dict(idx=int(f[1]), stype=int(f[2]), depth=int(f[3]),
                             nonref=int(f[4]), qp=int(f[5]), bytes=int(f[6]),
                             by=float(f[7]), bcb=float(f[8]), bcr=float(f[9]),
                             bcbf=float(f[10]), bother=float(f[11])))
    return n, rows


def crf_for_target(clip, kbps):
    """Smallest integer CRF whose size is at or under the ladder target."""
    w, h, fps, _ = y4m_geom(clip)
    target = kbps * 1000.0 / 8.0 * (FRAMES / fps)
    lo, hi = 8, 46
    while lo < hi:
        mid = (lo + hi) // 2
        n, _ = encode(clip, mid)
        if n > target:
            lo = mid + 1
        else:
            hi = mid
    return lo


def ssd(clip, recon, per_frame=False):
    """Per-plane SSD of the recon (raw I420, display order) against the source."""
    w, h, _, hdrlen = y4m_geom(clip)
    cw, ch = w // 2, h // 2
    fsz = w * h + 2 * cw * ch
    tot = np.zeros(3, dtype=np.int64)
    each = []
    with open(src_of(clip), "rb") as s, open(recon, "rb") as r:
        s.seek(hdrlen)
        for _ in range(FRAMES):
            fh = s.readline()                       # per-frame FRAME header
            if not fh.startswith(b"FRAME"):
                break
            a = np.frombuffer(s.read(fsz), dtype=np.uint8)
            b = np.frombuffer(r.read(fsz), dtype=np.uint8)
            if a.size < fsz or b.size < fsz:
                break
            off = 0
            one = [0, 0, 0]
            for i, n in enumerate((w * h, cw * ch, cw * ch)):
                d = a[off:off + n].astype(np.int32) - b[off:off + n].astype(np.int32)
                one[i] = int(np.dot(d, d))
                tot[i] += one[i]
                off += n
            each.append(one)
    return (tot, each) if per_frame else tot


def one_point(clip, crf):
    fd, rec = tempfile.mkstemp(suffix=".yuv", dir=TMP)
    os.close(fd)
    try:
        n, rows = encode(clip, crf, recon=rec, trace=True)
        d, each = ssd(clip, rec, per_frame=True)
    finally:
        os.unlink(rec)
    for r in rows:                  # CBIT is coding order, the recon display
        if r["idx"] < len(each):
            r["ssd_y"], r["ssd_cb"], r["ssd_cr"] = each[r["idx"]]
    FRAME_ROWS.extend(dict(clip=clip, crf=crf, **r) for r in rows)
    b = {k: sum(r[k] for r in rows) for k in ("by", "bcb", "bcr", "bcbf", "bother")}
    wq = sum(r["qp"] * r["bytes"] for r in rows) / max(1, sum(r["bytes"] for r in rows))
    return dict(clip=clip, crf=crf, bytes=n,
                qp_mean=sum(r["qp"] for r in rows) / max(1, len(rows)),
                qp_bw=wq,
                bits_y=b["by"], bits_cb=b["bcb"], bits_cr=b["bcr"],
                bits_cbf=b["bcbf"], bits_other=b["bother"],
                ssd_y=int(d[0]), ssd_cb=int(d[1]), ssd_cr=int(d[2]))


FRAME_ROWS = []
FRAME_FIELDS = ["clip", "crf", "idx", "stype", "depth", "nonref", "qp", "bytes",
                "by", "bcb", "bcr", "bcbf", "bother", "ssd_y", "ssd_cb", "ssd_cr"]

FIELDS = ["clip", "crf", "bytes", "qp_mean", "qp_bw", "bits_y", "bits_cb",
          "bits_cr", "bits_cbf", "bits_other", "ssd_y", "ssd_cb", "ssd_cr"]


def measure():
    pinned = json.loads(os.environ.get("PTS", "{}"))
    jobs = []
    for clip in CLIPS:
        lad = ladders[clip]
        if clip in pinned:
            pts = pinned[clip]
        else:
            lo = crf_for_target(clip, max(lad))  # highest rate -> lowest CRF
            hi = crf_for_target(clip, min(lad))
            pts = list(range(lo - 1, hi + 2))
        print("%-18s points %s" % (clip, pts), flush=True)
        jobs += [(clip, c) for c in pts]
    rows = []
    with ThreadPoolExecutor(max_workers=JOBS) as ex:
        for r in ex.map(lambda a: one_point(*a), jobs):
            rows.append(r)
            print("  %-18s crf %2d  %8d B  qp %5.2f  Y %.0f Cb %.0f Cr %.0f bits"
                  % (r["clip"], r["crf"], r["bytes"], r["qp_bw"],
                     r["bits_y"], r["bits_cb"], r["bits_cr"]), flush=True)
    rows.sort(key=lambda r: (r["clip"], r["crf"]))
    os.makedirs(os.path.dirname(OUT), exist_ok=True)
    with open(OUT, "w", newline="") as f:
        wcsv = csv.DictWriter(f, FIELDS)
        wcsv.writeheader()
        for r in rows:
            wcsv.writerow(r)
    fout = OUT.replace(".csv", "_frames.csv")
    FRAME_ROWS.sort(key=lambda r: (r["clip"], r["crf"], r["idx"]))
    with open(fout, "w", newline="") as f:
        wcsv = csv.DictWriter(f, FRAME_FIELDS, extrasaction="ignore")
        wcsv.writeheader()
        for r in FRAME_ROWS:
            wcsv.writerow(r)
    print("wrote", OUT, "and", fout)
    analyze(OUT)


def chroma_qp(qp):
    """Table 8-10 with no offsets, as n265_chroma_qp."""
    m = [29, 30, 31, 32, 33, 33, 34, 34, 35, 35, 36, 36, 37, 37]
    if qp < 30:
        return qp
    if qp <= 43:
        return m[qp - 30]
    return qp - 6


def analyze(path):
    rows = list(csv.DictReader(open(path)))
    for r in rows:
        for k in FIELDS[1:]:
            r[k] = float(r[k])
    by = {}
    for r in rows:
        by.setdefault(r["clip"], []).append(r)

    def rate(r, comp):
        if comp == "y":
            return r["bits_y"]
        return r["bits_cb"] + r["bits_cr"] + r["bits_cbf"]

    def dist(r, comp):
        return r["ssd_y"] if comp == "y" else r["ssd_cb"] + r["ssd_cr"]

    print("\nPer-component slope lambda = -dR/dD (bits per unit SSD), and")
    print("w = lambda_y / lambda_c, against the squared-step derivation")
    print("2^((qp_y - qp_c)/3). Chroma rate = cb + cr residual + chroma cbf.")
    print("\nAdjacent-point differences:\n")
    print("%-18s %7s %7s %10s %10s %7s %8s" %
          ("clip", "crf", "qp", "lam_y", "lam_c", "w", "derived"))
    raw = []
    for clip, rs in sorted(by.items()):
        rs.sort(key=lambda r: r["crf"])
        for a, b in zip(rs, rs[1:]):
            dRy, dDy = rate(a, "y") - rate(b, "y"), dist(b, "y") - dist(a, "y")
            dRc, dDc = rate(a, "c") - rate(b, "c"), dist(b, "c") - dist(a, "c")
            if min(dRy, dDy, dRc, dDc) <= 0:
                continue
            lam_y, lam_c = dRy / dDy, dRc / dDc
            qp = (a["qp_bw"] + b["qp_bw"]) / 2
            q = int(round(qp))
            der = 2.0 ** ((q - chroma_qp(q)) / 3.0)
            raw.append((clip, qp, lam_y / lam_c, der))
            print("%-18s %3d-%-3d %7.2f %10.4g %10.4g %7.3f %8.3f" %
                  (clip, a["crf"], b["crf"], qp, lam_y, lam_c,
                   lam_y / lam_c, der))

    # The same slope read off a quadratic fit of ln R against ln D per
    # component, which is what the adjacent differences are a noisy sample
    # of: lambda = -(d ln R / d ln D) * (R / D).
    print("\nQuadratic ln R / ln D fit, w at each ladder point:\n")
    print("%-18s %5s %7s %10s %10s %7s %8s" %
          ("clip", "crf", "qp", "lam_y", "lam_c", "w", "derived"))
    fitted = []
    for clip, rs in sorted(by.items()):
        rs.sort(key=lambda r: r["crf"])
        co = {}
        for comp in ("y", "c"):
            lr = np.log([rate(r, comp) for r in rs])
            ld = np.log([dist(r, comp) for r in rs])
            co[comp] = (np.polyfit(ld, lr, 2), ld, lr)
        for i, r in enumerate(rs):
            lam = {}
            for comp in ("y", "c"):
                c2, ld, lr = co[comp]
                slope = 2 * c2[0] * ld[i] + c2[1]
                lam[comp] = -slope * rate(r, comp) / dist(r, comp)
            if lam["y"] <= 0 or lam["c"] <= 0:
                continue
            q = int(round(r["qp_bw"]))
            der = 2.0 ** ((q - chroma_qp(q)) / 3.0)
            w = lam["y"] / lam["c"]
            fitted.append((clip, r["qp_bw"], w, der))
            print("%-18s %5d %7.2f %10.4g %10.4g %7.3f %8.3f" %
                  (clip, r["crf"], r["qp_bw"], lam["y"], lam["c"], w, der))

    nat = os.environ.get("NATURAL", "bus_cif,stefan_cif,tempete_cif,foreman_cif").split(",")
    print("\nw by integer QP, natural clips (%s):\n" % ",".join(nat))
    buck = {}
    for clip, qp, w, der in fitted:
        if clip in nat:
            buck.setdefault(int(round(qp)), []).append((clip, w))
    print("%4s %6s %6s %6s %6s %5s   %s" %
          ("qp", "med", "min", "max", "spread", "deriv", "clips"))
    for q in sorted(buck):
        ws = [w for _, w in buck[q]]
        med = float(np.median(ws))
        spread = (max(ws) - min(ws)) / med if med else 0
        print("%4d %6.3f %6.3f %6.3f %6.1f%% %5.3f   %s" %
              (q, med, min(ws), max(ws), 100 * spread,
               2.0 ** ((q - chroma_qp(q)) / 3.0),
               " ".join("%s=%.2f" % (c.replace("_cif", ""), w)
                        for c, w in buck[q])))
    print("\nFlat-typed clips:\n")
    for clip, qp, w, der in fitted:
        if clip not in nat:
            print("%-18s qp %5.2f  w %6.3f  derived %5.3f" % (clip, qp, w, der))
    return raw, fitted


def analyze_frames(path):
    """The same slope split by the frame's role in the pyramid.

    Read the caveat before the numbers: a leaf B's chroma SSD is mostly what
    it INHERITED from its references, while its chroma bits are its own, so
    dD_c on the low-bit types is not the work those bits did. The confound
    inflates w exactly where the bit count is smallest, which is why the
    B rows below are an upper bound and not a table.
    """
    import collections
    rows = list(csv.DictReader(open(path)))
    for r in rows:
        for k in r:
            if k != "clip":
                r[k] = float(r[k])
    g = collections.defaultdict(list)
    for r in rows:
        t = ("I" if r["stype"] == 2 else "P" if r["stype"] == 1 else
             "refB" if r["nonref"] == 0 else "leafB")
        g[(r["clip"], t, r["crf"])].append(r)
    print("\nSlope by frame type (inherited distortion confounds the B rows)\n")
    print("%-16s %-6s %5s %6s %10s %10s %7s %8s" %
          ("clip", "type", "crf", "qp", "lam_y", "lam_c", "w", "derived"))
    for clip in sorted(set(r["clip"] for r in rows)):
        for t in ("P", "refB", "leafB"):
            crfs = sorted(c for (cl, tt, c) in g if cl == clip and tt == t)
            agg = {}
            for c in crfs:
                fr = g[(clip, t, c)]
                agg[c] = dict(
                    R_y=sum(x["by"] for x in fr),
                    R_c=sum(x["bcb"] + x["bcr"] + x["bcbf"] for x in fr),
                    D_y=sum(x["ssd_y"] for x in fr),
                    D_c=sum(x["ssd_cb"] + x["ssd_cr"] for x in fr),
                    qp=float(np.mean([x["qp"] for x in fr])))
            for a, b in zip(crfs, crfs[1:]):
                A, B = agg[a], agg[b]
                dRy, dDy = A["R_y"] - B["R_y"], B["D_y"] - A["D_y"]
                dRc, dDc = A["R_c"] - B["R_c"], B["D_c"] - A["D_c"]
                if min(dRy, dDy, dRc, dDc) <= 0:
                    continue
                lam_y, lam_c = dRy / dDy, dRc / dDc
                qp = (A["qp"] + B["qp"]) / 2
                q = int(round(qp))
                print("%-16s %-6s %2d-%-2d %6.2f %10.4g %10.4g %7.3f %8.3f" %
                      (clip, t, a, b, qp, lam_y, lam_c, lam_y / lam_c,
                       2.0 ** ((q - chroma_qp(q)) / 3.0)))


if __name__ == "__main__":
    if len(sys.argv) > 2 and sys.argv[1] == "--frames":
        analyze_frames(sys.argv[2])
    elif len(sys.argv) > 2 and sys.argv[1] == "--analyze":
        analyze(sys.argv[2])
    else:
        measure()
