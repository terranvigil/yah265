#!/usr/bin/env python3
# Copyright (c) 2026, the yah265 authors
# SPDX-License-Identifier: GPL-2.0-or-later
"""The results page's charts and tables, from the data files.

Reads the newest parity report for each machine with its CSV, the
all-architecture summary and the rate ladders in
docs/data/parity-spec-ladders.json. Writes seven charts into
site/assets/charts/results/:

    speed.svg      the Mac's encode time over x265's, per class, tier and goal
    machines.svg   the same medians on the Mac, the Xeon and Graviton4
    quality.svg    VMAF v1 lead at the matched file size, per class and tier
    curves.svg     VMAF v1 against bitrate for every real-title cell
    tracking.svg   each VBR rung's achieved rate against its target
    lookahead.svg  BD-rate at lookahead 12 and 30 against 20
    tierc.svg      BD-rate per content-type clip

and refills every `<!--gen:NAME-->...<!--/gen:NAME-->` span in
site/results.md, so no table row or chart caption there is typed by hand.

    scripts/charts_results.py            # newest reports
    scripts/charts_results.py --check    # exit 1 if the page or a chart is stale
"""
import argparse
import csv
import json
import math
import os
import re
import statistics as st
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import blog_charts as B  # noqa: E402

ROOT = B.ROOT
PAGE = os.path.join(ROOT, "site/results.md")
OUT = os.path.join(ROOT, "site/assets/charts/results")
REL = "assets/charts/results"
LADDERS_REL = "docs/data/parity-spec-ladders.json"

CLASSES, TIERS, GOALS, MACHINES = B.CLASSES, B.TIERS, B.GOALS, B.MACHINES
RES, TITLE = B.RES, B.TITLE
TIER_LABEL = {"S": "short clips", "M": "2-minute segments", "L": "10-minute films"}
GOAL_LABEL = {"G1": "G1: C, one thread", "G2": "G2: C, all cores", "G3": "G3: SIMD, all cores"}
MEDIAN_BAR, WORST_BAR = 1.00, 1.15
DV_MEDIAN, DV_CELL = -0.5, -1.0
BD_BAR, TRACK_BAR = B.BD_BAR, 10.0
T, esc, marker = B.T, B.esc, B.marker

# yah265 wears the logo's orange-red and x265 a neutral gray. Slots 2 and 3
# are for a second and third series of our own (machines, rate modes).
# Checked with the dataviz validator on both surfaces.
STYLE = B.STYLE


# ------------------------------------------------------------------ data

def md_tables(path):
    """heading text -> the markdown tables under it, each a list of rows of
    cells with the header row first."""
    out, head, cur = {}, "", None
    for line in open(path):
        line = line.rstrip("\n")
        if line.startswith("#"):
            head, cur = line.lstrip("# ").strip(), None
        elif line.startswith("|"):
            cells = [c.strip() for c in line.strip().strip("|").split("|")]
            if all(re.fullmatch(r":?-+:?", c) for c in cells):
                continue
            if cur is None:
                cur = []
                out.setdefault(head, []).append(cur)
            cur.append(cells)
        else:
            cur = None
    return out


def table(tabs, start, n=0):
    for h, ts in tabs.items():
        if h.startswith(start) and len(ts) > n:
            return ts[n]
    sys.exit(f"no table {n} under a heading starting '{start}'")


def split_cell(name):
    """'tears_1080p' -> ('tears', '1080p')."""
    title, size = name.rsplit("_", 1)
    return title, {"cif": "CIF"}.get(size, size)


def nice(title, cls):
    base = re.sub(r"_(cif|\d+p)$", "", title)
    return f"{TITLE.get(base, base)} {RES.get(cls, cls)}"


def load_ladders():
    """cell -> mode -> (arm, depth) -> {rung: (kbps, VMAF v1)} for the
    binaries under test."""
    out = {}
    for k, r in json.load(open(B.LADDERS)).items():
        cell, mode, arm, q, la, b = k.split("|")
        if mode not in ("crf", "vbr") or b != (B.OURS_BIN if arm == "ours" else B.REF_BIN):
            continue
        g = r.get("v1_gate")
        if g and r.get(f"v1_{g}") is not None:
            out.setdefault(cell, {}).setdefault(mode, {}).setdefault((arm, la), {})[q] = \
                (r["kbps"], r[f"v1_{g}"])
    return out


def curve(lad, cell, mode, depth="la20"):
    """(ours, x265) as sorted [(kbps, score)] on the rungs both sides ran."""
    m = lad[cell][mode]
    ours, ref = m[("ours", depth)], m[("ref", "la20")]
    keys = [q for q in ours if q in ref]
    if len(keys) != 5:
        sys.exit(f"{cell} {mode} {depth}: {len(keys)} shared rungs, want 5")
    return sorted(ours[q] for q in keys), sorted(ref[q] for q in keys)


def bd(lad, cell, mode, depth="la20"):
    """BD-rate on VMAF v1, or None when the two curves share no score range."""
    ours, ref = curve(lad, cell, mode, depth)
    if max(min(s for _, s in ours), min(s for _, s in ref)) >= \
            min(max(s for _, s in ours), max(s for _, s in ref)):
        return None
    return B.bd_rate(ref, ours)


def tracking(lad, cell):
    """(ours, x265) rate errors in percent of the target, one per VBR rung."""
    m = lad[cell]["vbr"]
    ours, ref = m[("ours", "la20")], m[("ref", "la20")]
    keys = sorted((q for q in ours if q in ref), key=lambda q: int(q[1:]))
    err = lambda d: [(d[q][0] / int(q[1:]) - 1) * 100 for q in keys]
    return err(ours), err(ref)


def real_cells(lad_lines):
    """The report's real-title ladder cells in its order: (key, title, size, tier)."""
    return [(f"{tier}/{size}/{title}", title, size, tier) for title, size, tier, _, _ in lad_lines]


def quality_rows(rows):
    """(class, tier) -> (median, smallest lead, its cell) of dVMAF v1 at the
    matched size, over the Mac's G3 rows."""
    acc = {}
    for r in rows:
        v = B.fnum(r["dvmafneg"])
        if r["goal"] == "G3" and v is not None and r["tier"] in TIERS:
            acc.setdefault((r["class"], r["tier"]), []).append((v, r["title"]))
    return {k: (st.median(v for v, _ in vs), min(vs)[0], min(vs)[1]) for k, vs in acc.items()}


# ------------------------------------------------------------------ charts

def head(W, H, title, sub, desc):
    o = [f"<svg xmlns='http://www.w3.org/2000/svg' width='{W}' height='{H}' "
         f"viewBox='0 0 {W} {H}' role='img'>",
         f"<title>{esc(title)}</title><desc>{esc(desc)}</desc>", STYLE,
         "<rect width='100%' height='100%' class='bg'/>",
         T(24, 40, title, 22, weight="600"), T(24, 64, sub, 14, "t2")]
    return o


def vline(x, y0, y1, cls="grid", w=1, dash=""):
    d = f" stroke-dasharray='{dash}'" if dash else ""
    return f"<line x1='{x:.1f}' y1='{y0:.1f}' x2='{x:.1f}' y2='{y1:.1f}' class='{cls}' stroke-width='{w}'{d}/>"


def hline(x0, x1, y, cls="grid", w=1):
    return f"<line x1='{x0:.1f}' y1='{y:.1f}' x2='{x1:.1f}' y2='{y:.1f}' class='{cls}' stroke-width='{w}'/>"


def row_label(c, t):
    return f"{RES[c]} {TIER_LABEL[t]}"


def chart_ratio(title, sub, desc, panels, rows, series, whisker):
    """Small multiples of one dot plot. panels: [(heading, {(row, series
    index): (median, slowest)})]. The ratio axis is shared and the bars are
    drawn on it."""
    W, L, Tp, B_ = 900, 196, 150, 78
    rowh = 30
    H = Tp + B_ + rowh * len(rows)
    n = len(panels)
    gap = 22
    pw = (W - L - 24 - gap * (n - 1)) / n
    x1 = 1.25 if whisker else 1.1
    o = head(W, H, title, sub, desc)
    if len(series) > 1:
        B.legend(o, [(i, f"s{i + 1}", lab) for i, lab in enumerate(series)])
    for i, (c, t) in enumerate(rows):
        y = Tp + rowh * (i + 0.5)
        if i % 2 == 0:
            o.append(f"<rect x='24' y='{y - rowh / 2:.1f}' width='{W - 48}' height='{rowh}' class='band'/>")
        o.append(T(L - 12, y + 4, row_label(c, t), 13, "t1", "end"))
    for p, (heading, vals) in enumerate(panels):
        px = L + p * (pw + gap)
        X = lambda v: px + v / x1 * pw
        o.append(T(px, Tp - 26, heading, 14, "t1", weight="600"))
        for v, lab in ((0.25, "4x"), (0.5, "2x"), (0.75, "1.33x")):
            o.append(vline(X(v), Tp - 6, H - B_))
            o.append(T(X(v), H - B_ + 18, f"{v:g}", 12, "t2", "middle"))
            o.append(T(X(v), H - B_ + 33, lab, 11, "t2", "middle"))
        o.append(vline(X(0), Tp - 6, H - B_, "base"))
        o.append(vline(X(MEDIAN_BAR), Tp - 6, H - B_, "base", 1.5))
        o.append(T(X(MEDIAN_BAR), H - B_ + 18, "1.00", 12, "t1", "middle"))
        o.append(T(X(MEDIAN_BAR), H - B_ + 33, "even", 11, "t2", "middle"))
        if whisker:
            o.append(vline(X(WORST_BAR), Tp - 6, H - B_, "base", 1.5, "2 3"))
            o.append(T(X(WORST_BAR) + 3, Tp - 10, f"{WORST_BAR:.2f}", 11, "t2", "middle"))
        for i, r in enumerate(rows):
            y = Tp + rowh * (i + 0.5)
            got = [(j, vals[(r, j)]) for j in range(len(series)) if (r, j) in vals]
            for j, (med, worst) in got:
                dy = (j - (len(series) - 1) / 2) * 8 if len(series) > 1 else 0
                if whisker and worst is not None:
                    o.append(f"<line x1='{X(med):.1f}' y1='{y + dy:.1f}' x2='{X(worst):.1f}' "
                             f"y2='{y + dy:.1f}' class='l{j + 1}' stroke-width='2'/>")
                    o.append(f"<line x1='{X(worst):.1f}' y1='{y + dy - 5:.1f}' x2='{X(worst):.1f}' "
                             f"y2='{y + dy + 5:.1f}' class='l{j + 1}' stroke-width='2'/>")
                o.append(marker(j, X(med), y + dy, f"s{j + 1}", 5))
    o.append(T(W / 2, H - 12, "yah265's encode time over x265's at the same file size (left is "
             "faster). The lower labels read the same ratio as times faster.",
             12.5, "t2", "middle"))
    o.append("</svg>")
    return "\n".join(o)


def chart_speed(sp):
    rows = B.speed_rows([sp])
    meds = [v[0] for v in sp.values()]
    worst = max(v[1] for v in sp.values())
    title = (f"yah265 takes {round(min(meds), 2) * 100:.0f}% to {round(max(meds), 2) * 100:.0f}% "
             f"of x265's encode time on the Mac")
    panels = [(GOAL_LABEL[g], {((c, t), 0): sp[(c, t, g)][:2] for c, t in rows if (c, t, g) in sp})
              for g in GOALS]
    return title, chart_ratio(
        title, f"Dot: the median over a group's clips. Tick: its slowest clip, {worst:.2f} at "
               f"most. The bars are {MEDIAN_BAR:.2f} for the median and {WORST_BAR:.2f} for the "
               f"slowest clip.",
        "Dot plot in three panels, one per goal, of the Mac's median speed ratio against x265 "
        "medium for each class and tier, with a whisker to the slowest clip.",
        panels, rows, ["Mac"], True)


def chart_machines(sps):
    rows = B.speed_rows(sps.values())
    meds = [v[0] for sp in sps.values() for v in sp.values()]
    title = (f"On all three machines every group's median is {round(max(meds), 2):.2f} or lower")
    panels = [(GOAL_LABEL[g], {((c, t), j): sps[m][(c, t, g)][:2] for j, m in enumerate(MACHINES)
                               for c, t in rows if (c, t, g) in sps[m]}) for g in GOALS]
    return title, chart_ratio(
        title, "Median speed ratio per class and tier. Each machine is timed against x265 on "
               "the same machine.",
        "Dot plot in three panels, one per goal, of the median speed ratio against x265 medium "
        "for each class and tier on an Apple M5 Max, a Xeon 8488C and a Graviton4.",
        panels, rows, [B.MACHINE_LABEL[m] for m in MACHINES], False)


def chart_quality(q):
    rows = sorted(q, key=lambda r: (CLASSES.index(r[0]), TIERS.index(r[1])))
    W, L, R, Tp, B_ = 900, 190, 40, 110, 62
    rowh = 34
    H = Tp + B_ + rowh * len(rows)
    lo = min(-1.5, math.floor(min(v[1] for v in q.values()) * 2) / 2)
    hi = math.ceil(max(v[0] for v in q.values()) * 2) / 2 + 0.5
    X = lambda v: L + (v - lo) / (hi - lo) * (W - L - R)
    ahead = sum(1 for v in q.values() if v[0] > 0)
    title = (f"yah265 scores higher than x265 at the same file size in "
             f"{'every' if ahead == len(q) else f'{ahead} of {len(q)}'} class and tier")
    o = head(W, H, title, "VMAF v1 at the matched file size, yah265 minus x265. Dot: the "
             "group's median. Tick: the clip with the smallest lead.",
             "Dot plot of the median VMAF v1 difference at matched file size per class and "
             "tier, with a whisker to the smallest lead.")
    v = math.ceil(lo)
    while v <= hi + 1e-9:
        o.append(vline(X(v), Tp - 6, H - B_, "base" if v == 0 else "grid", 1.5 if v == 0 else 1))
        o.append(T(X(v), H - B_ + 20, "0 (x265)" if v == 0 else f"{v:+g}", 12, "t2", "middle"))
        v += 1
    for bar, lab in ((DV_MEDIAN, f"median bar {DV_MEDIAN:g}"), (DV_CELL, f"clip bar {DV_CELL:g}")):
        o.append(vline(X(bar), Tp - 6, H - B_, "base", 1.5, "2 3"))
        o.append(T(X(bar) - 4, Tp - 10, lab, 11, "t2", "end" if bar == DV_CELL else "start"))
    for i, (c, t) in enumerate(rows):
        y = Tp + rowh * (i + 0.5)
        med, worst, _ = q[(c, t)]
        o.append(T(L - 12, y + 4, row_label(c, t), 13, "t1", "end"))
        o.append(f"<line x1='{X(worst):.1f}' y1='{y}' x2='{X(med):.1f}' y2='{y}' class='l1' stroke-width='2'/>")
        o.append(f"<line x1='{X(worst):.1f}' y1='{y - 5}' x2='{X(worst):.1f}' y2='{y + 5}' class='l1' stroke-width='2'/>")
        o.append(marker(0, X(med), y, "s1", 5.5))
        o.append(T(X(med) + 10, y + 4, f"{med:+.2f}", 12, "t2"))
    o.append(T((L + W - R) / 2, H - 14, "VMAF v1 points ahead of x265 (right is better)", 13,
               "t2", "middle"))
    o.append("</svg>")
    return title, "\n".join(o)


def chart_curves(lad, cells, bds):
    cols = 4
    W, Tp = 900, 112
    pw, ph = (W - 48) / cols, 168
    nrow = math.ceil(len(cells) / cols)
    H = Tp + ph * nrow + 34
    above = sum(1 for k, *_ in cells if bds[k][0] < 0)
    title = f"yah265 needs fewer bits than x265 on {above} of {len(cells)} real-title cells at CRF"
    o = head(W, H, title, "VMAF v1 against bitrate on five CRF rungs per encoder (CRF 20 to 32). "
             "Up and left is better. Each panel has its own scales.",
             "Small multiples of VMAF v1 against bitrate for every two-minute and ten-minute "
             "cell, yah265 and x265 at preset medium.")
    o.append(marker(0, 30, 86, "s1", 5.5))
    o.append(T(42, 91, "yah265", 13, "t2"))
    o.append(marker(1, 116, 86, "sx", 5.5))
    o.append(T(128, 91, "x265", 13, "t2"))
    for i, (key, title_, size, tier) in enumerate(cells):
        ours, ref = curve(lad, key, "crf")
        px0 = 24 + (i % cols) * pw + 30
        px1 = px0 + pw - 30 - 14
        py0 = Tp + (i // cols) * ph + 40
        py1 = py0 + ph - 40 - 30
        allk = [k for k, _ in ours + ref]
        alls = [s for _, s in ours + ref]
        lk0, lk1 = math.log(min(allk) * 0.88), math.log(max(allk) * 1.12)
        step = 5 if max(alls) - min(alls) <= 20 else 10
        s0 = math.floor(min(alls) / step) * step
        s1 = math.ceil(max(alls) / step) * step
        X = lambda k: px0 + (math.log(k) - lk0) / (lk1 - lk0) * (px1 - px0)
        Y = lambda s: py0 + (s1 - s) / (s1 - s0) * (py1 - py0)
        o.append(T(px0 - 30, py0 - 24, B.cell_name(title_, size, tier), 11.5, "t1", weight="600"))
        o.append(T(px0 - 30, py0 - 9, f"BD-rate {bds[key][0]:+.2f}%", 11, "t2"))
        for s in range(s0, s1 + 1, step):
            o.append(hline(px0, px1, Y(s), "base" if s == s0 else "grid"))
            o.append(T(px0 - 5, Y(s) + 4, f"{s}", 10.5, "t2", "end"))
        ticks = [tk for tk in (100, 200, 500, 1000, 2000, 5000, 10000, 20000, 50000)
                 if math.exp(lk0) <= tk <= math.exp(lk1)]
        for tk in ticks:
            o.append(vline(X(tk), py1, py1 + 4, "base"))
            o.append(T(X(tk), py1 + 16, f"{tk / 1000:g}M" if tk >= 1000 else f"{tk}k", 10.5,
                       "t2", "middle"))
        for pts, ln, cls, shape in ((ref, "lx", "sx", 1), (ours, "l1", "s1", 0)):
            o.append(f"<polyline fill='none' class='{ln}' stroke-width='2' points='"
                     + " ".join(f"{X(k):.1f},{Y(s):.1f}" for k, s in pts) + "'/>")
            for k, s in pts:
                o.append(marker(shape, X(k), Y(s), cls, 3.2))
    o.append(T(W / 2, H - 12, "bitrate on a log scale (k = kbit/s, M = Mbit/s), VMAF v1 up the side",
               12.5, "t2", "middle"))
    o.append("</svg>")
    return title, "\n".join(o)


def chart_tracking(cells, tr):
    W, L, R, Tp, B_ = 900, 250, 40, 116, 62
    rowh = 24
    H = Tp + B_ + rowh * len(cells)
    allo = [e for k, *_ in cells for e in tr[k][0]]
    allr = [e for k, *_ in cells for e in tr[k][1]]
    x0 = min(-15, math.floor(min(allo + allr) / 5) * 5)
    x1 = max(15, math.ceil(max(allo + allr) / 5) * 5)
    X = lambda v: L + (v - x0) / (x1 - x0) * (W - L - R)
    inside = sum(1 for e in allo if abs(e) <= TRACK_BAR)
    title = (f"{'All' if inside == len(allo) else f'{inside} of'} {len(allo)} of yah265's VBR "
             f"rungs come within {TRACK_BAR:g}% of their target")
    o = head(W, H, title, "Achieved bitrate against the target over the whole cell, five rungs "
             f"per encoder. The shaded band is yah265's ±{TRACK_BAR:g}% bar.",
             "Dot plot of VBR rate error per rung for every real-title cell, yah265 and x265.")
    B.legend(o, [(0, "s1", "yah265"), (1, "sx", "x265")])
    o.append(f"<rect x='{X(-TRACK_BAR):.1f}' y='{Tp - 8}' width='{X(TRACK_BAR) - X(-TRACK_BAR):.1f}' "
             f"height='{H - B_ - Tp + 8}' class='band'/>")
    for v in range(x0, x1 + 1, 5):
        o.append(vline(X(v), Tp - 8, H - B_, "base" if v == 0 else "grid", 1.5 if v == 0 else 1))
        o.append(T(X(v), H - B_ + 20, "on target" if v == 0 else f"{v:+d}%", 12, "t2", "middle"))
    for i, (key, title_, size, tier) in enumerate(cells):
        y = Tp + rowh * (i + 0.5)
        o.append(T(L - 12, y + 4, B.cell_name(title_, size, tier), 12.5, "t1", "end"))
        for e in tr[key][1]:
            o.append(marker(1, X(e), y + 4, "sx", 3.4))
        for e in tr[key][0]:
            o.append(marker(0, X(e), y - 4, "s1", 3.8))
    o.append(T((L + W - R) / 2, H - 14, "achieved bitrate against the target (left is under)",
               13, "t2", "middle"))
    o.append("</svg>")
    return title, "\n".join(o)


def chart_lookahead(sweep):
    """sweep: [(key, label, mode, {depth: bd})]. Each line is one cell's
    BD-rate against its own value at depth 20."""
    W, H, Tp, B_ = 900, 420, 130, 70
    depths = [12, 20, 30]
    deltas = {(k, m): {d: v[d] - v[20] for d in depths} for k, _, m, v in sweep}
    lo = math.floor(min(min(d.values()) for d in deltas.values()))
    hi = math.ceil(max(max(d.values()) for d in deltas.values()))
    med = {m: {d: st.median(v[d] for (k, mm), v in deltas.items() if mm == m) for d in depths}
           for m in ("crf", "vbr")}
    worst12 = max(v[12] for v in deltas.values())
    gain30 = st.median(v[30] for v in deltas.values())
    title = (f"A lookahead of 12 costs up to {worst12:.1f} BD-rate points against 20. "
             f"Going to 30 moves the median {gain30:+.2f}")
    o = head(W, H, title.split(". ")[0], "BD-rate on VMAF v1 at each depth, less the same cell's at 20. x265 "
             "stays at its own 20. Below zero is better than 20.",
             "Two line charts, CRF and VBR, of the change in BD-rate at lookahead depths 12, 20 "
             "and 30 for six cells, with the median drawn heavier.")
    o.append(f"<line x1='24' y1='86' x2='44' y2='86' class='l1' stroke-width='3'/>")
    o.append(T(50, 91, "median of the six cells", 13, "t2"))
    o.append(f"<line x1='230' y1='86' x2='250' y2='86' class='lx' stroke-width='1.5'/>")
    o.append(T(256, 91, "each cell", 13, "t2"))
    pw = (W - 48 - 40) / 2
    for p, m in enumerate(("crf", "vbr")):
        px0 = 24 + 70 + p * (pw + 40)
        px1 = px0 + pw - 70 - 104
        X = lambda d: px0 + (d - 12) / 18 * (px1 - px0)
        Y = lambda v: Tp + (hi - v) / (hi - lo) * (H - Tp - B_)
        o.append(T(px0 - 70, Tp - 14, "CRF" if m == "crf" else "capped VBR", 14, "t1", weight="600"))
        for v in range(lo, hi + 1):
            o.append(hline(px0, px1, Y(v), "base" if v == 0 else "grid", 1.5 if v == 0 else 1))
            o.append(T(px0 - 8, Y(v) + 4, "same as 20" if v == 0 else f"{v:+d}", 11.5, "t2", "end"))
        for d in depths:
            o.append(T(X(d), H - B_ + 20, f"{d} frames", 12, "t2", "middle"))
        far = max((k for (k, mm) in deltas if mm == m), key=lambda k: deltas[(k, m)][12])
        for k, lab, mm, _ in sweep:
            if mm != m:
                continue
            v = deltas[(k, m)]
            o.append(f"<polyline fill='none' class='lx' stroke-width='1.5' points='"
                     + " ".join(f"{X(d):.1f},{Y(v[d]):.1f}" for d in depths) + "'/>")
            if k == far:
                o.append(T(X(12) + 6, Y(v[12]) - 6, lab, 11.5, "t2"))
        o.append(f"<polyline fill='none' class='l1' stroke-width='3' points='"
                 + " ".join(f"{X(d):.1f},{Y(med[m][d]):.1f}" for d in depths) + "'/>")
        for d in depths:
            o.append(marker(0, X(d), Y(med[m][d]), "s1", 4.5))
        o.append(T(X(30) + 10, Y(med[m][30]) + 4, f"median {med[m][30]:+.2f}", 12, "t1"))
    o.append(T(W / 2, H - 14, "yah265's lookahead depth across, BD-rate points against depth 20 up the side",
               13, "t2", "middle"))
    o.append("</svg>")
    return title, "\n".join(o)


def chart_tierc(tc):
    """tc: [(label, content, crf bd, vbr bd or None)]."""
    W, L, R, Tp, B_ = 900, 250, 40, 116, 62
    rowh = 34
    H = Tp + B_ + rowh * len(tc)
    allv = [v for r in tc for v in r[2:] if v is not None]
    x0, x1 = math.floor(min(allv) / 10) * 10, 10
    X = lambda v: L + (v - x0) / (x1 - x0) * (W - L - R)
    inside = sum(1 for v in allv if v < 0)
    title = (f"{'All' if inside == len(allv) else f'{inside} of'} {len(allv)} content-type "
             f"curves with a BD-rate need fewer bits than x265")
    o = head(W, H, title, "BD-rate against x265 medium on VMAF v1 for film grain, sports and "
             "screen content. Left of zero needs fewer bits.",
             "Dot plot of BD-rate on VMAF v1 for nine content-type clips at CRF and capped VBR.")
    B.legend(o, [(0, "s1", "CRF"), (1, "s2", "capped VBR")])
    for v in range(x0, x1 + 1, 10):
        o.append(vline(X(v), Tp - 8, H - B_, "base" if v == 0 else "grid", 1.5 if v == 0 else 1))
        o.append(T(X(v), H - B_ + 20, "0 (x265)" if v == 0 else f"{v:+d}%", 12, "t2", "middle"))
    o.append(vline(X(BD_BAR), Tp - 8, H - B_, "base", 1.5, "2 3"))
    o.append(T(X(BD_BAR) + 4, Tp - 12, f"bar +{BD_BAR:.1f}%", 11, "t2"))
    for i, (lab, content, crf, vbr) in enumerate(tc):
        y = Tp + rowh * (i + 0.5)
        o.append(T(L - 12, y, lab, 12.5, "t1", "end"))
        o.append(T(L - 12, y + 14, content, 11, "t2", "end"))
        if vbr is None:
            o.append(T(X(0) - 10, y + 15, "VBR: no shared score range, yah265 above at every rung",
                       11, "t2", "end"))
        else:
            o.append(f"<line x1='{X(min(crf, vbr)):.1f}' y1='{y}' x2='{X(max(crf, vbr)):.1f}' "
                     f"y2='{y}' class='span' stroke-width='2'/>")
            o.append(marker(1, X(vbr), y + (4 if abs(crf - vbr) < 1.5 else 0), "s2", 5))
        o.append(marker(0, X(crf), y - (4 if vbr is not None and abs(crf - vbr) < 1.5 else 0),
                        "s1", 5.5))
    o.append(T((L + W - R) / 2, H - 14, "bitrate change for the same VMAF v1 (left is better)",
               13, "t2", "middle"))
    o.append("</svg>")
    return title, "\n".join(o)


# ------------------------------------------------------------------ tables

def mdt(header, align, rows):
    return "\n".join(["| " + " | ".join(header) + " |", "|" + "|".join(align) + "|"]
                     + ["| " + " | ".join(str(c) for c in r) + " |" for r in rows])


# BD-rate in the tables; the ladder file and the dated reports keep two places
BD_PLACES = 1


def pct(v, places=BD_PLACES):
    return f"{v:+.{places}f}%"


def rng(vs):
    return f"{min(vs):+.1f}% to {max(vs):+.1f}%"


def figure(name, alt, source):
    return (f'<figure class="chart"><img src="{REL}/{name}" alt="{esc(alt)}.">'
            f"<figcaption>Data: {source}.</figcaption></figure>")


def link(path, text):
    return f'<a href="{path}">{text}</a>'


def values(D):
    sps, rows, lad = D["sps"], D["rows"], D["lad"]
    cells, bds, tr, q = D["cells"], D["bds"], D["tr"], D["q"]
    isa, mac = D["isa"], D["mac"]
    V = {}
    L, R, C = "---", "--:", "---"

    V["fig-frames"] = B.frames_figure(3, B.FRAMES_ALL)
    V["frames-player"] = B.FRAMES_PLAYER

    # the verdict
    combos = [(m, k) for m in MACHINES for k in sps[m]]
    ok = [1 for m, k in combos if sps[m][k][0] <= MEDIAN_BAR and sps[m][k][1] <= WORST_BAR]
    wall = table(isa, "VBR wall")[1:]
    wall_n = sum(int(r[5]) for r in wall) * len(MACHINES)
    wall_ok = sum(int(r[5]) for r in wall for c in r[2:5] if float(c.split("/")[1]) < MEDIAN_BAR)
    allbd = [v for k, *_ in cells for v in bds[k]]
    tc = table(isa, "Tier C on the Mac")[1:]
    trk = [e for k, *_ in cells for e in tr[k][0]]
    pl = D["pl"]
    V["verdict"] = mdt(["what was measured", "result"], [L, R], [
        [f"median speed over the {pl['n']} cells timed on the Mac", f"{B.times(pl['median'])} faster"],
        [f"median VMAF v1 lead at the same file size, {pl['dv_n']} cells", f"{pl['dv']:+.2f} points"],
        [f"median bits saved at equal VMAF v1, {len(allbd)} rate ladders",
         f"{abs(st.median(allbd)):.1f}%"],
        ["speed groups inside the bar, three goals on three machines",
         f"{len(ok)} of {len(combos)}"],
        [f"rate ladders inside the +{BD_BAR:.1f}% BD-rate bar",
         f"{sum(1 for v in allbd if v <= BD_BAR)} of {len(allbd)}"],
        ["VBR cells faster than x265, on three machines", f"{wall_ok} of {wall_n}"],
        [f"VBR rungs within ±{TRACK_BAR:g}% of their target",
         f"{sum(1 for e in trk if abs(e) <= TRACK_BAR)} of {len(trk)}"],
        ["content-type clips passing on quality",
         f"{sum(1 for r in tc if r[-1] == 'PASS')} of {len(tc)}"]])
    V["goal-table"] = B.goal_table(sps)

    # how to read, the test set
    V["goal-defs"] = mdt(["goal", "yah265", "x265", "threads"], [L, L, L, L], [
        ["G1", "plain C (`N265_ASM_OFF=all`)", "plain C (`--no-asm`)",
         "one. x265 gets it from `--pools none --frame-threads 1`"],
        ["G2", "plain C", "plain C", "each encoder's default"],
        ["G3", "as shipped, with SIMD", "as shipped, with its assembly", "each encoder's default"]])
    sets = {}
    for r in rows["mac"]:
        if r["tier"] in TIERS:
            sets.setdefault(r["tier"], {}).setdefault(r["class"], set()).add(r["title"])
    how = {"S": "each clip whole", "M": "from the first non-black frame",
           "L": "600 seconds past the opening card, stopped before the end credits",
           "C": "seven short clips and one ten-minute film scan at two sizes"}
    trow = []
    for t in TIERS:
        per = ", ".join(f"{len(sets[t][c])} at {RES[c]}" for c in CLASSES if c in sets[t])
        names = sorted({TITLE.get(re.sub(r"_(cif|\d+p)$", "", x), re.sub(r"_(cif|\d+p)$", "", x))
                        for c in sets[t] for x in sets[t][c]})
        trow.append([f"{t}, {TIER_LABEL[t]}", per, ", ".join(names), how[t]])
    kinds = {}
    for r in tc:
        kinds.setdefault(r[1], []).append(r[0])
    cper = {}
    for r in tc:
        cper[split_cell(r[0])[1]] = cper.get(split_cell(r[0])[1], 0) + 1
    trow.append(["C, content types", ", ".join(f"{cper[c]} at {RES[c]}" for c in CLASSES if c in cper),
                 ", ".join(f"{len(v)} {k}" for k, v in kinds.items()), how["C"]])
    V["test-set"] = mdt(["tier", "cells", "what's in it", "how it's cut"], [L, L, L, L], trow)

    # speed on the Mac
    sp = sps["mac"]
    srows = []
    for c, t in B.speed_rows([sp]):
        g = [f"{sp[(c, t, x)][0]:.2f} / {sp[(c, t, x)][1]:.2f}" for x in GOALS]
        srows.append([RES[c], TIER_LABEL[t]] + g + [B.fx(sp[(c, t, "G3")][0])])
    V["speed-mac"] = mdt(["class", "tier", "G1", "G2", "G3", "G3 as times faster"],
                         [L, L, R, R, R, R], srows)
    frows = []
    for line in open(D["mac_md"]):
        m = re.match(r"- (\S+) \(end frame (\d+)\): G1 (.+?); G2 (.+?); G3 (.+)$", line.rstrip())
        if m:
            g = ["void" if "feeder-bound" in x else x for x in m.groups()[2:]]
            frows.append([nice(*split_cell(m[1])), m[2]] + g)
    V["films"] = mdt(["film", "last story frame", "G1", "G2", "G3"], [L, R, R, R, R], frows)

    # by machine: the all-architecture report's own tables, gating rows only
    for g, start in (("g1", "G1 speed"), ("g2", "G2 speed")):
        V[f"machines-{g}"] = mdt(["class", "tier", "Mac", "x86-64", "Graviton4"], [L, L, R, R, R],
                                 [[RES[r[0]], TIER_LABEL[r[1]]] + r[2:5]
                                  for r in table(isa, start)[1:] if r[1] in TIERS])
    g3 = table(isa, "G3 speed")[1:]
    V["machines-g3"] = mdt(
        ["class", "tier", "Mac", "x86-64", "x86-64 against x265 at `--asm avx512`", "Graviton4"],
        [L, L, R, R, R, R],
        [[RES[r[0]], TIER_LABEL[r[1]]] + r[2:6] for r in g3 if r[1] in TIERS])
    # Graviton3 ran the short clips only; its own count wins where it names one
    n3 = lambda r: (re.search(r"\((\d+) of", r[6]) or re.match(r"(\d+)", r[7]))[1]
    V["graviton3"] = mdt(["class", "Graviton3", "clips"], [L, R, R],
                         [[RES[r[0]], r[6].split("(")[0].strip(), n3(r)]
                          for r in g3 if r[1] in TIERS and r[6] != "-"])
    V["capped"] = mdt(["class", "both at SSE4.2", "both at AVX2", "control: yah265 at AVX-512, x265 at AVX2",
                       "clips"], [L, R, R, R, R],
                      [[RES[r[0]]] + r[2:6] for r in table(isa, "G3 speed", 1)[1:]])
    V["vbr-machines"] = mdt(["class", "tier", "Mac", "x86-64", "Graviton4", "cells"],
                            [L, L, R, R, R, R],
                            [[RES[r[0]], TIER_LABEL[r[1]]] + r[2:6] for r in wall])
    for m in MACHINES:
        for (c, t, g), v in sps[m].items():
            col = {"mac": 2, "x86": 3, "g4": 5 if g == "G3" else 4}[m]
            rep = next(r for r in table(isa, f"{g} speed")[1:] if (r[0], r[1]) == (c, t))[col]
            if [x.strip() for x in rep.split("/")] != [f"{y:.2f}" for y in v[:2]]:
                sys.exit(f"{m} {c} {t} {g}: the CSV gives {v[0]:.2f} / {v[1]:.2f}, "
                         f"the all-architecture report {rep}")

    # quality at the same size
    how_q = {(r[0], r[1]): r[5:7] for r in table(isa, "Quality on each machine")[1:]}
    V["quality"] = mdt(["class", "tier", "median", "smallest lead", "on"], [L, L, R, R, L],
                       [[RES[c], TIER_LABEL[t], f"{q[(c, t)][0]:+.2f}", f"{q[(c, t)][1]:+.2f}",
                         nice(q[(c, t)][2], c)]
                        for c, t in sorted(q, key=lambda r: (CLASSES.index(r[0]), TIERS.index(r[1])))])
    xrows = []
    for (c, t), (x86, _) in how_q.items():
        m = re.match(r"reproduced: (\w+) (.+)$", x86)
        if m:
            xrows += [[f"{nice(m[1], c)}, {TIER_LABEL[t]}", mode, moved]
                      for mode, moved in re.findall(r"(CRF|VBR) ([+-][\d.]+)", m[2])]
    V["quality-xeon"] = mdt(["ladder run again on the Xeon", "mode", "BD-rate points from the Mac's curve"],
                            [L, L, R], xrows)

    # ladders
    V["bd"] = mdt(["cell", "tier", "CRF BD-rate", "VBR BD-rate"], [L, L, R, R],
                  [[nice(ti, s), TIER_LABEL[t], pct(bds[k][0]), pct(bds[k][1])]
                   for k, ti, s, t in cells])
    tight = max((v, k, i) for k, *_ in cells for i, v in enumerate(bds[k]))
    _, ti, s, t = next(c for c in cells if c[0] == tight[1])
    V["bd-summary"] = mdt(["over the ladders", "value"], [L, R], [
        ["median BD-rate, CRF", pct(st.median(bds[k][0] for k, *_ in cells), 1)],
        ["median BD-rate, capped VBR", pct(st.median(bds[k][1] for k, *_ in cells), 1)],
        ["median BD-rate, both", pct(st.median(allbd), 1)],
        ["ladders where yah265 needs fewer bits", f"{sum(1 for v in allbd if v < 0)} of {len(allbd)}"],
        [f"closest to the bar: {nice(ti, s)}, {TIER_LABEL[t]}, {'VBR' if tight[2] else 'CRF'}",
         pct(tight[0])]])
    V["vbr-track"] = mdt(["cell", "tier", "yah265, five rungs", "x265, five rungs"], [L, L, R, R],
                         [[nice(ti, s), TIER_LABEL[t], rng(tr[k][0]), rng(tr[k][1])]
                          for k, ti, s, t in cells])
    V["vbr-wall"] = mdt(["cell", "tier", "target kbit/s", "ratio", "yah265 against target",
                         "x265 against target"], [L, L, R, R, R, R],
                        [[nice(*split_cell(r[0])), TIER_LABEL[r[1]], r[2], r[3],
                          re.search(r"\((.+?)\)", r[4])[1], re.search(r"\((.+?)\)", r[5])[1]]
                         for r in table(mac, "VBR wall")[1:]])
    V["lookahead"] = mdt(["cell", "mode", "yah265 at 12", "yah265 at 20", "yah265 at 30"],
                         [L, L, R, R, R],
                         [[lab, "CRF" if m == "crf" else "VBR"] + [pct(v[d]) for d in (12, 20, 30)]
                          for _, lab, m, v in D["sweep"]])

    # tier C
    V["tier-c"] = mdt(["clip", "content", "VMAF v1 lead at the same size", "CRF BD-rate",
                       "VBR BD-rate", "VBR rate against target", "G3 ratio"],
                      [L, L, R, R, R, R, R],
                      [[nice(*split_cell(r[0])), r[1], r[2].split("/")[0].strip(),
                        pct(crf), "no shared range, yah265 above" if vbr is None else pct(vbr),
                        rng(D["ctr"][r[0]]), r[5].split()[0] + (" (earlier binary)" if "(" in r[5] else "")]
                       for r, (_, _, crf, vbr) in zip(tc, D["tc"])])

    # figures
    mac_csv = link(os.path.relpath(D["paths"]["mac"], ROOT), "the Mac report's rows")
    all_csv = ", ".join(link(os.path.relpath(D["paths"][m], ROOT), f"the {B.MACHINE_SHORT[m]} rows")
                        for m in MACHINES)
    ladf = link(LADDERS_REL, "the recorded ladders")
    for name, src in (("speed", mac_csv), ("machines", all_csv), ("quality", mac_csv),
                      ("curves", ladf), ("tracking", ladf), ("lookahead", ladf), ("tierc", ladf)):
        V[f"fig-{name}"] = figure(f"{name}.svg", D["titles"][name], src)
    V["report-mac"] = f"[the Mac report]({os.path.relpath(D['mac_md'], ROOT)})"
    V["report-all"] = f"[the all-architecture report]({os.path.relpath(D['isa_path'], ROOT)})"
    V["report-x86"] = f"[the x86-64 report]({os.path.relpath(D['paths']['x86'][:-4] + '.md', ROOT)})"
    V["report-g4"] = f"[the Graviton report]({os.path.relpath(D['paths']['g4'][:-4] + '.md', ROOT)})"
    return V


GEN = B.GEN


def fill(text, V):
    missing = sorted({m[1] for m in GEN.finditer(text) if m[1] not in V})
    if missing:
        sys.exit(f"site/results.md: no value for {missing}")

    def sub(m):
        v = V[m[1]]
        if "\n" in v or v.startswith("<figure"):
            return f"<!--gen:{m[1]}-->\n{v}\n<!--/gen:{m[1]}-->"
        return f"<!--gen:{m[1]}-->{v}<!--/gen:{m[1]}-->"
    return GEN.sub(sub, text)


def gather():
    Dt = r"\d{4}-\d\d-\d\d"
    mac_csv = B.newest(rf"PARITY-REPORT-yah265-{Dt}\.csv")
    paths = {"mac": mac_csv,
             "x86": B.newest(rf"PARITY-REPORT-yah265-x86-{Dt}\.csv"),
             "g4": B.newest(rf"PARITY-REPORT-yah265-graviton-{Dt}\.csv")}
    isa_path = B.newest(rf"PARITY-REPORT-yah265-all-isa-{Dt}\.md")
    mac_md = mac_csv[:-4] + ".md"
    rows = {m: list(csv.DictReader(open(p))) for m, p in paths.items()}
    D = {"paths": paths, "isa_path": isa_path, "mac_md": mac_md, "rows": rows,
         "sps": {m: B.speed_groups(rs) for m, rs in rows.items()},
         "q": quality_rows(rows["mac"]), "isa": md_tables(isa_path), "mac": md_tables(mac_md),
         "pl": B.pooled(rows["mac"], mac_md)}
    lad = D["lad"] = load_ladders()
    lines = B.ladders(mac_md)
    D["cells"] = real_cells(lines)
    D["bds"] = {}
    for (key, *_), (_, _, _, crf, vbr) in zip(D["cells"], lines):
        got = (bd(lad, key, "crf"), bd(lad, key, "vbr"))
        if abs(got[0] - crf) > 0.01 or abs(got[1] - vbr) > 0.01:
            sys.exit(f"{key}: BD {got} from the ladder file, the report says {crf}, {vbr}")
        D["bds"][key] = got
    D["tr"] = {k: tracking(lad, k) for k, *_ in D["cells"]}
    # the lookahead sweep: every cell that also ran at 12 and 30
    D["sweep"] = []
    for m in ("crf", "vbr"):
        for key, ti, s, t in D["cells"]:
            if ("ours", "la12") in lad[key][m] and ("ours", "la30") in lad[key][m]:
                D["sweep"].append((key, f"{nice(ti, s)}, {TIER_LABEL[t]}", m,
                                   {d: bd(lad, key, m, f"la{d}") for d in (12, 20, 30)}))
    rep = {(r[0], r[1]): r[2:5] for r in table(D["mac"], "Lookahead depth sweep")[1:]}
    for key, _, m, v in D["sweep"]:
        t, s, ti = key.split("/")
        want = rep[(f"{ti}_{s} ({t})", m.upper())]
        if any(abs(v[d] - float(w.split("%")[0])) > 0.01 for d, w in zip((12, 20, 30), want)):
            sys.exit(f"{key} {m}: sweep {v} from the ladder file, the report says {want}")
    # tier C, from the report's table and checked against the ladder file
    D["tc"], D["ctr"] = [], {}
    for r in table(D["isa"], "Tier C on the Mac")[1:]:
        ti, s = split_cell(r[0])
        key = f"C/{s}/{ti}"
        crf, vbr = bd(lad, key, "crf"), bd(lad, key, "vbr")
        if abs(crf - float(r[3].rstrip("%"))) > 0.01 or \
                (vbr is None) != ("no overlap" in r[4]) or \
                (vbr is not None and abs(vbr - float(r[4].rstrip("%"))) > 0.01):
            sys.exit(f"{key}: BD {crf}, {vbr} from the ladder file, the report says {r[3]}, {r[4]}")
        D["tc"].append((nice(ti, s), r[1], crf, vbr))
        D["ctr"][r[0]] = tracking(lad, key)[0]
    return D


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--check", action="store_true",
                    help="change nothing; exit 1 if the page or a chart is out of date")
    a = ap.parse_args()
    D = gather()
    made = {"speed": chart_speed(D["sps"]["mac"]), "machines": chart_machines(D["sps"]),
            "quality": chart_quality(D["q"]),
            "curves": chart_curves(D["lad"], D["cells"], D["bds"]),
            "tracking": chart_tracking(D["cells"], D["tr"]),
            "lookahead": chart_lookahead(D["sweep"]), "tierc": chart_tierc(D["tc"])}
    D["titles"] = {k: v[0] for k, v in made.items()}
    V = values(D)
    stale = []
    for name, (_, svg) in made.items():
        p = os.path.join(OUT, name + ".svg")
        old = open(p).read() if os.path.exists(p) else None
        if old != svg + "\n":
            stale.append(name + ".svg")
            if not a.check:
                os.makedirs(OUT, exist_ok=True)
                with open(p, "w") as f:
                    f.write(svg + "\n")
    page = open(PAGE).read()
    new = fill(page, V)
    if new != page:
        stale.append("site/results.md")
        if not a.check:
            with open(PAGE, "w") as f:
                f.write(new)
    print(f"{len(D['cells'])} real-title cells, {len(D['tc'])} content-type clips, "
          f"{sum(len(s) for s in D['sps'].values())} speed groups, {len(made)} charts")
    if a.check:
        if stale:
            print("stale: " + ", ".join(stale))
            sys.exit(1)
        print("up to date")
    else:
        print("updated: " + (", ".join(stale) or "nothing changed"))


if __name__ == "__main__":
    main()
