#!/usr/bin/env python3
# Copyright (c) 2026, the yah265 authors
# SPDX-License-Identifier: GPL-2.0-or-later
"""Per-shot rate-quality points and their convex hull: the hull-assist read.

The offline half of `--shot-stats` (docs/phase7-shot-design.md step 1). The
encoder writes one JSON Lines object per shot -- where the shot is, what it
cost, what the lookahead thought of it -- and this scores each shot's decoded
pictures against the source and prints, per encode per shot, (bytes, VMAF-NEG).
Given two or more encodes of the same source it also prints the shot's convex
hull over those points and which encode each hull point came from, which is
the thing a ladder builder picks a rung with.

  scripts/shot_hull.py --src local/corpus/ms_cif_30.y4m \\
      --enc crf22=work/ms22.hevc:work/ms22.jsonl \\
      --enc crf26=work/ms26.hevc:work/ms26.jsonl \\
      --enc crf30=work/ms30.hevc:work/ms30.jsonl --csv shots.csv

Each `--enc` is `name=bitstream:sidecar`.

SCORING. The score is bdcompare.vmaf_of's own libvmaf call -- the band's, with
the band's models -- asked for its per-frame array, and a shot's score is the
mean of that array over the shot's display range. It has to come off the
WHOLE-SEQUENCE run: VMAF's temporal features make a frame's score depend on
its neighbors, so a shot cut out and scored as its own clip reads differently
from the same shot inside the sequence. Averaging the whole-file array is also
exactly what scripts/multishot_bd.py does per shot, which is what lets the two
be checked against each other (`--check-shots`).

MEMO. Keyed by what the bitstream IS, the way bd_at_rate.py's second memo is:
a rebuilt encoder that reproduces a bitstream byte for byte reads its scores
out of the cache. BD_CACHE names the directory (default ~/.cache/yah265-bd),
BD_CACHE=0 disables it.

LIMIT. Every encode is scored against the source at the source's resolution,
because that is the call the band makes. A ladder whose rungs are at different
resolutions needs the decode upscaled before this, which is not this step.
"""
import argparse, hashlib, json, os, sys, tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import bdcompare as bd

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

_CACHE = os.environ.get("BD_CACHE", os.path.expanduser("~/.cache/yah265-bd"))
if _CACHE == "0":
    _CACHE = None


def _frames_json(bitstream, src):
    """The memo path for one bitstream's per-frame scores. Same shape as
    bd_at_rate._content_json and a different directory: the payload is the
    per-frame array, not the band's pooled dict, and the two must not be
    served to each other."""
    if not _CACHE:
        return None
    h = hashlib.sha256()
    with open(bitstream, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    st = os.stat(src)
    h.update(f"|{src}|{st.st_size}|{bd.SUBSAMPLE}|{bd._vmaf_models()[0]}".encode())
    d = os.path.join(_CACHE, "vmaf-frames")
    os.makedirs(d, exist_ok=True)
    return os.path.join(d, h.hexdigest()[:24] + ".json")


def per_frame_neg(bitstream, src):
    """The per-frame VMAF-NEG array for one bitstream, memoized."""
    ck = _frames_json(bitstream, src)
    if ck and os.path.exists(ck):
        try:
            return json.load(open(ck))
        except (OSError, ValueError):
            pass
    work = tempfile.mkdtemp(prefix="shothull.")
    v = bd.vmaf_of(bitstream, src, work, per_frame=True)
    if not v or "frames" not in v:
        sys.exit(f"shot_hull: VMAF failed on {bitstream}")
    if ck:
        tmp = ck + f".{os.getpid()}.tmp"
        with open(tmp, "w") as f:
            json.dump(v["frames"], f)
        os.replace(tmp, ck)
    return v["frames"]


def read_sidecar(path):
    """One record per shot. A v1 line is a run between intra pictures, so a
    keyint key frame splits a shot there. A v2 line says which shot it
    belongs to, and the runs of one shot are joined back into it."""
    shots = []
    for ln in open(path):
        ln = ln.strip()
        if not ln:
            continue
        r = json.loads(ln)
        if r.get("v", 1) >= 2 and shots and r["shot"] == shots[-1]["shot"]:
            s = shots[-1]
            n0 = s["last"] - s["first"] + 1
            n1 = r["last"] - r["first"] + 1
            s["qp_base_mean"] = ((s["qp_base_mean"] * n0 + r["qp_base_mean"] * n1)
                                 / (n0 + n1))
            for k in ("bytes", "complexity", "flat_frames"):
                s[k] += r[k]
            for k in s["frames"]:
                s["frames"][k] += r["frames"][k]
            s["last"] = r["last"]
            continue
        shots.append(r)
    if not shots:
        sys.exit(f"shot_hull: {path} has no shots")
    return shots


def hull(points):
    """The upper-left convex hull of (bytes, score) points: the rungs a
    ladder builder would keep. Dominated points go first (more bytes for no
    more quality), then the upper convex envelope of what is left, so a point
    that sits under the chord between its neighbors is dropped too."""
    pts = sorted(points, key=lambda p: (p[0], -p[1]))
    front, best = [], None
    for b, s, name in pts:                  # Pareto: score strictly rising
        if best is None or s > best:
            front.append((b, s, name))
            best = s
    h = []
    for p in front:                         # monotone chain, slopes falling
        while len(h) >= 2:
            (x1, y1, _), (x2, y2, _) = h[-2], h[-1]
            # drop h[-1] if it is on or below the chord h[-2] -> p
            if (y2 - y1) * (p[0] - x1) <= (p[1] - y1) * (x2 - x1):
                h.pop()
            else:
                break
        h.append(p)
    return h


def main():
    ap = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--src", required=True, help="source y4m")
    ap.add_argument("--enc", action="append", required=True, metavar="NAME=BITSTREAM:SIDECAR")
    ap.add_argument("--csv", help="write the per-shot rows here")
    ap.add_argument("--check-shots", metavar="CUTS",
                    help="a multishot .cuts file: report where the encoder's "
                         "shots and the sequence's known cuts disagree")
    a = ap.parse_args()

    encs = []
    for spec in a.enc:
        name, _, rest = spec.partition("=")
        bs, _, side = rest.rpartition(":")
        if not name or not bs or not side:
            sys.exit(f"shot_hull: --enc wants NAME=BITSTREAM:SIDECAR, got {spec}")
        encs.append((name, bs, side))

    grid = None                             # the shot boundaries, from the first encode
    rows = []
    for name, bs, side in encs:
        shots = read_sidecar(side)
        key = [(s["first"], s["last"]) for s in shots]
        if grid is None:
            grid = key
            gname = name
        elif key != grid:
            print(f"shot_hull: {name} has different shot boundaries from {gname}; "
                  f"its points are printed but left out of the hull", flush=True)
        neg = per_frame_neg(bs, a.src)
        for i, s in enumerate(shots):
            lo, hi = s["first"], min(s["last"], len(neg) - 1)
            if hi < lo:
                sys.exit(f"shot_hull: {name} shot {i} is past the scored frames")
            sc = sum(neg[lo:hi + 1]) / (hi - lo + 1)
            rows.append({"enc": name, "shot": i, "first": s["first"],
                         "last": s["last"], "open": s["open"],
                         "frames": s["last"] - s["first"] + 1,
                         "bytes": s["bytes"], "vmaf_neg": sc,
                         "qp_base_mean": s["qp_base_mean"],
                         "complexity": s["complexity"],
                         "on_grid": key == grid})

    by_shot = {}
    for r in rows:
        by_shot.setdefault(r["shot"], []).append(r)
    hull_pts = {}
    for i in sorted(by_shot):
        rs = by_shot[i]
        r0 = rs[0]
        print(f"shot {i:2d} [{r0['first']}..{r0['last']}] {r0['frames']:4d} frames "
              f"open {r0['open']}")
        for r in rs:
            print(f"    {r['enc']:<16} {r['bytes']:>10} B   NEG {r['vmaf_neg']:6.2f}"
                  f"   qp_base {r['qp_base_mean']:6.2f}")
        if len(rs) > 1:
            h = hull([(r["bytes"], r["vmaf_neg"], r["enc"]) for r in rs if r["on_grid"]])
            hull_pts[i] = {n for _, _, n in h}
            print("    hull: " + " -> ".join(
                f"{n}({b} B, {s:.2f})" for b, s, n in h))
        print(flush=True)
    for r in rows:
        r["hull"] = 1 if r["enc"] in hull_pts.get(r["shot"], set()) else 0

    if a.csv:
        import csv
        cols = ["shot", "first", "last", "frames", "open", "enc", "bytes",
                "vmaf_neg", "qp_base_mean", "complexity", "hull"]
        with open(a.csv, "w", newline="") as f:
            w = csv.DictWriter(f, fieldnames=cols, extrasaction="ignore")
            w.writeheader()
            for r in rows:
                w.writerow(r)
        print(f"wrote {a.csv}")

    if a.check_shots:
        lines = [ln.split() for ln in open(a.check_shots) if ln.strip()]
        cuts = [int(c[0]) for c in lines]
        names = [c[1] if len(c) > 1 else f"shot{i}" for i, c in enumerate(lines)]
        got = [f for f, _ in grid]
        print(f"shot boundaries: sidecar {got}")
        print(f"                 .cuts   {cuts}")
        extra = [f for f in got if f not in cuts]
        missing = [f for f in cuts if f not in got]
        if extra:
            print(f"  encoder SPLITS a listed shot at {extra}")
        if missing:
            print(f"  encoder MERGES across a listed cut at {missing}")
        if not extra and not missing:
            print("  the encoder's shots are the sequence's shots")
        # The same scores folded onto the .cuts grid, which is the grid
        # scripts/multishot_bd.py prints on: a listed shot the encoder split
        # is the frame-weighted mean of its parts, and since every part's
        # score is the mean of the same per-frame array over its own range,
        # that IS the mean over the listed shot. Line these up against the
        # instrument's row to check the two agree.
        ends = cuts[1:] + [max(s["last"] for s in read_sidecar(encs[0][2])) + 1]
        for name, _, _ in encs:
            rs = [r for r in rows if r["enc"] == name]
            out = []
            for lo, hi in zip(cuts, ends):
                part = [r for r in rs if lo <= r["first"] and r["last"] < hi]
                n = sum(r["frames"] for r in part)
                out.append(sum(r["vmaf_neg"] * r["frames"] for r in part) / n)
            print(f"  on the .cuts grid, {name}: "
                  + " ".join(f"{s:5.1f}" for s in out)
                  + "   (" + ", ".join(names) + ")")


if __name__ == "__main__":
    main()
