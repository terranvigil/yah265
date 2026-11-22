#!/usr/bin/env python3
# Copyright (c) 2026, the yah265 authors
# SPDX-License-Identifier: GPL-2.0-or-later
"""Per-shot rate-quality points on VMAF v1, and each shot's convex hull
across every rung at every resolution.

Give it a source and several encodes of it, at any mix of resolutions. It
scores every encode against the source on VMAF v1, cuts the per-frame
scores and the per-frame bytes on one shot grid, and prints per shot
(bytes, v1) for every encode and the shot's upper convex hull over all of
them, naming the resolution and rate each hull point came from. That is
what a ladder builder picks rungs with. A title-level hull comes out of
the same data.

  scripts/shot_hull.py --src local/corpus/ms_cif_30.y4m \\
      --shots shots.txt \\
      --enc cif_crf26=work/c26.hevc:work/c26.jsonl \\
      --enc cif_crf32=work/c32.hevc:work/c32.jsonl \\
      --enc qcif_crf24=work/q24.hevc:work/q24.jsonl@176x144 \\
      --csv shots.csv

`--enc` is NAME=BITSTREAM[:SIDECAR][@WxH]. The sidecar is the encode's
`--shot-stats` file. The size is read from the stream when it isn't given.
A number after "crf", "qp" or "q" in NAME goes in the CSV's q column.

SHOT GRID. `--shots FILE` (or `--shot-list FILE`) takes the grid from a
shot list: the `--shot-scan` output, a multishot .cuts file, or a list
edited by hand. A line's first integer is the shot's first frame and a
second integer, when there is one, its last. Lines starting with "#" and
lines with no leading integer are skipped, so a header passes. A JSON line
with "first" (and "last") works too. Without a list the grid is the first
encode's sidecar, as before.

BYTES. A shot's bytes are the packet sizes of its pictures in display
order, which sum to the file. Where an encode's sidecar runs nest inside
the grid's shots, their bytes are checked against those sums, and a run
that straddles a listed start is reported.

SCORING is scripts/v1score.py, the scorer the parity harness uses. The
source is the reference. Every decode is scaled to the source's display
size: 1920x1080-fit below 1080p, the source's own size at 1080p and up (a
4K source's rungs at 3840x2160). The model follows the source, CAMBI is
told each rung's coded size and depth, and every frame is scored. A shot's
score is the mean of the whole-file per-frame array over the shot's
frames. It has to come off the whole-file run, because v1's temporal
features make a frame's score depend on its neighbors. That is also what
multishot_bd.py averages per shot, so the two agree. The per-frame arrays
are memoized by v1score.py (V1S_CACHE).

CSV. `--csv` writes one row per (shot, encode): title, shot, first, last,
frames, enc, width, height, q, bytes, kbps, v1, v1_min, model, display,
open, qp_base_mean, complexity, hull (1 on the shot's hull). It's the
per-shot (bytes, v1) curve the shot planner's oracle is solved from.

`--selftest` checks the hull, the list parser and the byte fold, with no
media.
"""
import argparse, csv, json, os, re, sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import v1score as v1s  # noqa: E402

THREADS = os.environ.get("VMAF_THREADS", "4")


def parse_enc(spec):
    """NAME=BITSTREAM[:SIDECAR][@WxH] -> (name, bitstream, sidecar, (w, h))."""
    name, _, rest = spec.partition("=")
    size = None
    m = re.search(r"@(\d+)x(\d+)$", rest)
    if m:
        size = (int(m.group(1)), int(m.group(2)))
        rest = rest[:m.start()]
    bs, side = rest, None
    if ":" in rest:
        bs, _, side = rest.rpartition(":")
    if not name or not bs:
        sys.exit(f"shot_hull: --enc wants NAME=BITSTREAM[:SIDECAR][@WxH], "
                 f"got {spec}")
    return name, bs, side or None, size


def q_of(name):
    m = re.search(r"(?:crf|qp|q)[_=-]?(\d+(?:\.\d+)?)", name, re.I)
    return m.group(1) if m else ""


def read_sidecar(path):
    shots = [json.loads(ln) for ln in open(path) if ln.strip()]
    if not shots:
        sys.exit(f"shot_hull: {path} has no shots")
    return shots


def parse_shot_list(lines, nframes=None):
    """[(first, last)] from shot-list lines. A missing last runs to the next
    shot's first, and the final shot to nframes - 1."""
    starts = []
    for ln in lines:
        t = ln.strip()
        if not t or t.startswith("#"):
            continue
        if t.startswith("{"):
            j = json.loads(t)
            if "first" in j:
                starts.append((int(j["first"]),
                               int(j["last"]) if "last" in j else None))
            continue
        nums = []
        for tok in re.split(r"[\s,]+", t):
            if re.fullmatch(r"-?\d+", tok):
                nums.append(int(tok))
            else:
                break
        if nums:
            starts.append((nums[0], nums[1] if len(nums) > 1 else None))
    starts.sort()
    out = []
    for i, (f, last) in enumerate(starts):
        if last is None:
            if i + 1 < len(starts):
                last = starts[i + 1][0] - 1
            elif nframes is not None:
                last = nframes - 1
            else:
                raise ValueError(f"the last shot (from {f}) has no end and "
                                 "the frame count is unknown")
        if last < f or (out and f <= out[-1][1]):
            raise ValueError(f"shot {f}..{last} is empty or overlaps")
        out.append((f, last))
    if not out:
        raise ValueError("no shots")
    return out


def fold(grid, sizes):
    """The sum of per-frame values over each (first, last) of the grid."""
    return [sum(sizes[f:last + 1]) for f, last in grid]


def nest(grid, runs):
    """Which grid shot each sidecar run sits in, or the runs that straddle
    a grid start: (index per run, straddlers)."""
    idx, bad = [], []
    for r in runs:
        k = next((i for i, (f, last) in enumerate(grid)
                  if f <= r["first"] and r["last"] <= last), None)
        idx.append(k)
        if k is None:
            bad.append((r["first"], r["last"]))
    return idx, bad


def hull(points):
    """The upper-left convex hull of (bytes, score, name) points: the rungs
    a ladder builder would keep. Dominated points go first (more bytes for
    no more quality), then the upper convex envelope of what is left, so a
    point under the chord between its neighbors is dropped too."""
    pts = sorted(points, key=lambda p: (p[0], -p[1]))
    front, best = [], None
    for b, s, name in pts:
        if best is None or s > best:
            front.append((b, s, name))
            best = s
    h = []
    for p in front:
        while len(h) >= 2:
            (x1, y1, _), (x2, y2, _) = h[-2], h[-1]
            if (y2 - y1) * (p[0] - x1) <= (p[1] - y1) * (x2 - x1):
                h.pop()
            else:
                break
        h.append(p)
    return h


def selftest():
    bad = 0

    def check(what, got, want):
        nonlocal bad
        if got != want:
            print(f"  FAIL {what}: got {got}, want {want}")
            bad += 1

    pts = [(100, 50, "a"), (200, 70, "b"), (300, 75, "c"), (400, 90, "d"),
           (250, 60, "e"), (500, 90, "f"), (150, 40, "g")]
    # c sits under the chord b -> d; e and g are dominated; f buys nothing
    check("hull", [n for _, _, n in hull(pts)], ["a", "b", "d"])
    check("hull, one point", hull([(1, 2, "x")]), [(1, 2, "x")])
    # a point exactly on the chord adds nothing and goes
    check("hull, collinear", [n for _, _, n in hull(
        [(0, 0, "p"), (1, 1, "q"), (2, 2, "r")])], ["p", "r"])
    # equal bytes: only the better score can be on the hull
    check("hull, tie on bytes", [n for _, _, n in hull(
        [(10, 5, "lo"), (10, 7, "hi"), (20, 8, "z")])], ["hi", "z"])
    # concave chain: every point is on the hull
    check("hull, concave", [n for _, _, n in hull(
        [(1, 10, "a"), (2, 15, "b"), (4, 18, "c"), (8, 19, "d")])],
        ["a", "b", "c", "d"])
    check("list, cuts", parse_shot_list(
        ["0 foreman_cif", "150 bus_cif", "300 stefan_cif"], 400),
        [(0, 149), (150, 299), (300, 399)])
    check("list, scan form", parse_shot_list(
        ["# shots v1 frames=60 352x288", "0 19 start 1.00",
         "20 44 cut 0.83", "45 59 cut 0.91"]),
        [(0, 19), (20, 44), (45, 59)])
    check("list, json", parse_shot_list(
        ['{"first":0,"last":9}', '{"first":10}'], 30), [(0, 9), (10, 29)])
    try:
        parse_shot_list(["0 10", "5 20"])
        check("list, overlap refused", "accepted", "refused")
    except ValueError:
        pass
    grid = [(0, 3), (4, 9)]
    check("fold", fold(grid, [1, 2, 3, 4, 5, 6, 7, 8, 9, 10]), [10, 45])
    check("nest", nest(grid, [{"first": 0, "last": 3}, {"first": 4, "last": 6},
                             {"first": 7, "last": 9}]), ([0, 1, 1], []))
    check("nest, straddle", nest(grid, [{"first": 0, "last": 5},
                                        {"first": 6, "last": 9}])[1], [(0, 5)])
    check("enc spec", parse_enc("a=x.hevc:x.jsonl@176x144"),
          ("a", "x.hevc", "x.jsonl", (176, 144)))
    check("enc spec, bare", parse_enc("a=x.hevc"), ("a", "x.hevc", None, None))
    check("q of name", (q_of("1080p_crf22.5"), q_of("qcif_qp30"), q_of("x")),
          ("22.5", "30", ""))
    print("shot_hull selftest: " + ("FAILED" if bad else "passed"))
    return 1 if bad else 0


def main():
    if "--selftest" in sys.argv:
        sys.exit(selftest())
    ap = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--src", required=True, help="source y4m")
    ap.add_argument("--enc", action="append", required=True,
                    metavar="NAME=BITSTREAM[:SIDECAR][@WxH]")
    ap.add_argument("--shots", "--shot-list", dest="shots", metavar="FILE",
                    help="take the shot grid from this list")
    ap.add_argument("--csv", help="write the per-shot rows here")
    ap.add_argument("--check-shots", metavar="CUTS",
                    help="a multishot .cuts file: report where the grid and "
                         "the sequence's known cuts disagree")
    ap.add_argument("--threads", default=THREADS, help="libvmaf threads")
    ap.add_argument("--selftest", action="store_true")
    a = ap.parse_args()

    sw, sh, fps, _ = v1s.y4m_geom(a.src)
    title = os.path.splitext(os.path.basename(a.src))[0]
    encs = [parse_enc(s) for s in a.enc]
    if len({e[0] for e in encs}) != len(encs):
        sys.exit("shot_hull: two encodes share a name")

    data = []
    for name, bs, side, size in encs:
        sizes = v1s.frame_sizes(bs)
        if size is None:
            size = v1s.probe(bs)[:2]
        r = v1s.score(a.src, bs, sw, sh, fps, enc_w=size[0], enc_h=size[1],
                      frames=len(sizes), threads=a.threads, cache=True)
        pf = [fr["metrics"]["v1"] for fr in r["frames"]]
        if len(pf) != len(sizes):
            sys.exit(f"shot_hull: {name} scored {len(pf)} frames of "
                     f"{len(sizes)}")
        runs = read_sidecar(side) if side else None
        data.append(dict(name=name, bs=bs, size=size, sizes=sizes, v1=pf,
                         runs=runs, disp="%dx%d" % tuple(r["disp"])))
    model = os.path.basename(v1s.gate_model(sw, sh, fps)[1])[:-5]
    disp = data[0]["disp"]

    nframes = min(len(d["sizes"]) for d in data)
    if a.shots:
        try:
            grid = parse_shot_list(open(a.shots), nframes)
        except ValueError as e:
            sys.exit(f"shot_hull: {a.shots}: {e}")
        gsrc = os.path.basename(a.shots)
    else:
        if data[0]["runs"] is None:
            sys.exit("shot_hull: no --shots and the first encode has no sidecar")
        grid = [(s["first"], s["last"]) for s in data[0]["runs"]]
        gsrc = f"{data[0]['name']}'s sidecar"
    if grid[-1][1] >= nframes:
        sys.exit(f"shot_hull: the grid runs to frame {grid[-1][1]}, past the "
                 f"{nframes} frames every encode has")
    print(f"{title}: {sw}x{sh}, {len(grid)} shots from {gsrc}; v1 {model} "
          f"at {disp}, every frame\n")

    rows = []
    for d in data:
        byts = fold(grid, d["sizes"])
        extra = [{} for _ in grid]
        if d["runs"]:
            idx, bad = nest(grid, d["runs"])
            if bad:
                print(f"  {d['name']}: sidecar runs straddle the grid at "
                      f"{bad}; bytes from the packet sizes")
            else:
                for i in range(len(grid)):
                    rs = [r for r, k in zip(d["runs"], idx) if k == i]
                    if not rs:
                        continue
                    sb = sum(r["bytes"] for r in rs)
                    if sb != byts[i]:
                        print(f"  {d['name']} shot {i}: sidecar {sb} B, "
                              f"packets {byts[i]} B")
                    n = sum(r["last"] - r["first"] + 1 for r in rs)
                    extra[i] = dict(
                        open=rs[0].get("open", ""),
                        qp_base_mean=round(sum(
                            r.get("qp_base_mean", 0) * (r["last"] - r["first"] + 1)
                            for r in rs) / n, 4),
                        complexity=sum(r.get("complexity", 0) for r in rs))
        for i, (f, last) in enumerate(grid):
            seg = d["v1"][f:last + 1]
            n = last - f + 1
            rows.append(dict(
                title=title, shot=i, first=f, last=last, frames=n,
                enc=d["name"], width=d["size"][0], height=d["size"][1],
                q=q_of(d["name"]), bytes=byts[i],
                kbps=round(byts[i] * 8 / 1000 / (n / fps), 3),
                v1=sum(seg) / n, v1_min=min(seg), model=model,
                display=d["disp"], **extra[i]))

    on_hull = set()
    for i, (f, last) in enumerate(grid):
        rs = [r for r in rows if r["shot"] == i]
        print(f"shot {i:2d} [{f}..{last}] {last - f + 1:4d} frames")
        for r in sorted(rs, key=lambda r: r["bytes"]):
            print(f"    {r['enc']:<18} {r['width']:>5}x{r['height']:<5} "
                  f"{r['bytes']:>10} B   v1 {r['v1']:6.2f}")
        h = hull([(r["bytes"], r["v1"], r["enc"]) for r in rs])
        on_hull |= {(i, n) for _, _, n in h}
        print("    hull: " + " -> ".join(
            f"{n} ({next(r for r in rs if r['enc'] == n)['width']}w, "
            f"{b} B, {s:.2f})" for b, s, n in h))
        print(flush=True)
    for r in rows:
        r["hull"] = 1 if (r["shot"], r["enc"]) in on_hull else 0

    tot = []
    for d in data:
        rs = [r for r in rows if r["enc"] == d["name"]]
        b = sum(r["bytes"] for r in rs)
        n = sum(r["frames"] for r in rs)
        tot.append((b, sum(r["v1"] * r["frames"] for r in rs) / n, d["name"]))
    print("title: " + "   ".join(f"{n} {b} B v1 {s:.2f}"
                                 for b, s, n in sorted(tot)))
    print("title hull: " + " -> ".join(
        f"{n} ({b} B, {s:.2f})" for b, s, n in hull(tot)))

    if a.csv:
        cols = ["title", "shot", "first", "last", "frames", "enc", "width",
                "height", "q", "bytes", "kbps", "v1", "v1_min", "model",
                "display", "open", "qp_base_mean", "complexity", "hull"]
        with open(a.csv, "w", newline="") as f:
            w = csv.DictWriter(f, fieldnames=cols, extrasaction="ignore")
            w.writeheader()
            for r in rows:
                w.writerow(r)
        print(f"wrote {a.csv}")

    if a.check_shots:
        cuts = [f for f, _ in parse_shot_list(open(a.check_shots), nframes)]
        got = [f for f, _ in grid]
        print(f"shot boundaries: grid  {got}")
        print(f"                 .cuts {cuts}")
        extra = [f for f in got if f not in cuts]
        missing = [f for f in cuts if f not in got]
        if extra:
            print(f"  the grid SPLITS a listed shot at {extra}")
        if missing:
            print(f"  the grid MERGES across a listed cut at {missing}")
        if not extra and not missing:
            print("  the grid's shots are the sequence's shots")


if __name__ == "__main__":
    main()
