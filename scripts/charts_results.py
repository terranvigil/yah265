#!/usr/bin/env python3
# Copyright (c) 2026, the yah265 authors
# SPDX-License-Identifier: GPL-2.0-or-later
"""The results page's charts and tables, from the data files.

Reads the newest parity report for each machine with its CSV, the
all-architecture summary and the rate ladders in
docs/data/parity-spec-ladders.json. Writes seven charts into
site/assets/charts/results/:

    speed.svg      times faster than x265 on the Mac, per class, tier and goal
    machines.svg   the same medians on the Mac, the Xeon and Graviton4
    quality.svg    VMAF v1 lead at the matched file size, per class and tier
    curves.svg     VMAF v1 against bitrate for every real-title cell
    tracking.svg   each VBR rung's achieved rate against its target
    lookahead.svg  BD-rate at lookahead 12 and 30 against 20
    tierc.svg      BD-rate per content-type clip

and refills every `<!--gen:NAME-->...<!--/gen:NAME-->` span in
site/results.md, so no table row or chart caption there is typed by hand.
Rows come through blog_charts.report_rows(), which leaves out the titles
on blog_charts.EXCLUDED, so every median here is taken again from the
per-cell rows. The reading code is first checked, every title in, against
the all-architecture report's own tables. Each chart is written twice, as
name.svg and as name-narrow.svg for phones.

    scripts/charts_results.py            # newest reports
    scripts/charts_results.py --check    # exit 1 if the page or a chart is stale
"""
import argparse
import glob
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
GOAL_LABEL = {"G3": "G3: SIMD, all cores", "G1": "G1: C, one thread", "G2": "G2: C, all cores"}
GOAL_ORDER = B.GOAL_ORDER
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
    return f"{RES[c]} {B.pick(TIER_LABEL, B.TIER_LABEL)[t]}"


def tf(ratio):
    """Times faster, from a wall ratio rounded as the reports print it."""
    return 1 / round(ratio, 2)


def pair(med, worst):
    return f"{tf(med):.2f}x / {tf(worst):.2f}x"


def chart_faster(title, sub, desc, panels, rows, series, whisker):
    """Small multiples of one dot plot. panels: [(heading, {(row, series
    index): (median, slowest)})] as wall ratios. They're drawn as times
    faster than x265 on one shared axis, so right is faster. The panels sit
    side by side on the wide canvas and stack on the narrow one."""
    nw = B.NARROW
    W, L, Tp, B_ = B.width(), B.pick(196, 150), 150, 62
    rowh, gap = 30, 22
    n = len(panels)
    body = rowh * len(rows)
    step = body + 86
    H = Tp + (step * n - 24 if nw else body + B_)
    pw = W - L - 24 if nw else (W - L - 24 - gap * (n - 1)) / n
    top = max(tf(m) for _, vals in panels for m, _ in vals.values())
    x0, x1 = 0.5, max(2.0, math.ceil(top * 2) / 2)
    ticks = [v for v in (1, 2, 3, 4, 5, 6) if v <= x1] if x1 > 2.5 else [1, 1.5, 2, 2.5]
    o = head(W, H, title, sub, desc)
    if len(series) > 1:
        B.legend(o, [(i, f"s{i + 1}", lab) for i, lab in enumerate(series)])
    for p, (heading, vals) in enumerate(panels):
        px = L if nw else L + p * (pw + gap)
        py = Tp + (p * step if nw else 0)
        X = lambda v: px + (v - x0) / (x1 - x0) * pw
        if nw or p == 0:
            for i, (c, t) in enumerate(rows):
                y = py + rowh * (i + 0.5)
                if i % 2 == 0:
                    o.append(f"<rect x='24' y='{y - rowh / 2:.1f}' width='{W - 48}' "
                             f"height='{rowh}' class='band'/>")
                o.append(T(L - 12, y + 4, row_label(c, t), B.pick(13, 12), "t1", "end"))
        o.append(T(24 if nw else px, py - B.pick(26, 14), heading, 14, "t1", weight="600"))
        for v in ticks:
            if v == 1:
                continue
            o.append(vline(X(v), py - 6, py + body))
            o.append(T(X(v), py + body + 18, f"{v:g}x", 12, "t2", "middle"))
        o.append(vline(X(1), py - 6, py + body, "base", 1.5))
        o.append(T(X(1), py + body + 18, "1x", 12, "t1", "middle"))
        o.append(T(X(1), py + body + 32, "x265", 11, "t2", "middle"))
        if whisker:
            o.append(vline(X(1 / WORST_BAR), py - 6, py + body, "base", 1.5, "2 3"))
        for i, r in enumerate(rows):
            y = py + rowh * (i + 0.5)
            got = [(j, vals[(r, j)]) for j in range(len(series)) if (r, j) in vals]
            for j, (med, worst) in got:
                dy = (j - (len(series) - 1) / 2) * 8 if len(series) > 1 else 0
                if whisker and worst is not None:
                    o.append(f"<line x1='{X(tf(med)):.1f}' y1='{y + dy:.1f}' "
                             f"x2='{X(tf(worst)):.1f}' y2='{y + dy:.1f}' class='l{j + 1}' "
                             f"stroke-width='2'/>")
                    o.append(f"<line x1='{X(tf(worst)):.1f}' y1='{y + dy - 5:.1f}' "
                             f"x2='{X(tf(worst)):.1f}' y2='{y + dy + 5:.1f}' class='l{j + 1}' "
                             f"stroke-width='2'/>")
                o.append(marker(j, X(tf(med)), y + dy, f"s{j + 1}", 5))
    o.append(T(W / 2, H - 12, "times faster than x265 at the same file size",
               12.5, "t2", "middle"))
    o.append("</svg>")
    return "\n".join(o)


def chart_speed(sp):
    rows = B.speed_rows([sp])
    g3 = [v[0] for k, v in sp.items() if k[2] == "G3"]
    title = (f"As shipped, yah265 runs {tf(max(g3)):.2f}x to {tf(min(g3)):.2f}x faster than "
             f"x265 on the Mac")
    panels = [(GOAL_LABEL[g], {((c, t), 0): sp[(c, t, g)][:2] for c, t in rows if (c, t, g) in sp})
              for g in GOAL_ORDER]
    return title, chart_faster(
        title, f"Dot: a row's median. Tick: its slowest clip. The limit is 1x for "
               f"the median and the dashed {1 / WORST_BAR:.2f}x for the slowest clip.",
        "Dot plot in three panels, one per goal, of how many times faster than x265 medium "
        "yah265 runs on the Mac for each picture size and clip length, with a whisker to the "
        "slowest clip.",
        panels, rows, ["Mac"], True)


def chart_machines(sps):
    rows = B.speed_rows(sps.values())
    meds = [v[0] for sp in sps.values() for v in sp.values()]
    title = f"Against x265 as shipped, the lowest median is {tf(max(meds)):.2f}x faster"
    panels = [(GOAL_LABEL[g], {((c, t), j): sps[m][(c, t, g)][:2] for j, m in enumerate(MACHINES)
                               for c, t in rows if (c, t, g) in sps[m]}) for g in GOAL_ORDER]
    return title, chart_faster(
        title, "Median times faster per picture size and clip length. Each machine is timed "
               "against x265 on the same machine.",
        "Dot plot in three panels, one per goal, of the median times faster than x265 medium "
        "for each picture size and clip length on an Apple M5 Max, a Xeon 8488C and a "
        "Graviton4.",
        panels, rows, [B.MACHINE_LABEL[m] for m in MACHINES], False)


def chart_quality(q):
    rows = sorted(q, key=lambda r: (CLASSES.index(r[0]), TIERS.index(r[1])))
    W, Tp, B_ = B.width(), 110, 62
    L, R = B.pick((190, 40), (150, 44))
    rowh = 34
    H = Tp + B_ + rowh * len(rows)
    lo = min(-1.5, math.floor(min(v[1] for v in q.values()) * 2) / 2)
    hi = math.ceil(max(v[0] for v in q.values()) * 2) / 2 + 0.5
    X = lambda v: L + (v - lo) / (hi - lo) * (W - L - R)
    ahead = sum(1 for v in q.values() if v[0] > 0)
    title = (f"yah265 scores higher than x265 at "
             f"{'every' if ahead == len(q) else f'{ahead} of {len(q)}'} picture size and clip length")
    o = head(W, H, title, "VMAF v1 at the matched file size, yah265 minus x265. Dot: a "
             "row's median. Tick: the clip with the smallest lead.",
             "Dot plot of the median VMAF v1 difference at matched file size per picture size "
             "and clip length, with a whisker to the smallest lead.")
    v = math.ceil(lo)
    while v <= hi + 1e-9:
        o.append(vline(X(v), Tp - 6, H - B_, "base" if v == 0 else "grid", 1.5 if v == 0 else 1))
        o.append(T(X(v), H - B_ + 20, "0 (x265)" if v == 0 else f"{v:+g}", 12, "t2", "middle"))
        v += 1
    for bar, lab in ((DV_MEDIAN, B.pick(f"median limit {DV_MEDIAN:g}", f"median {DV_MEDIAN:g}")),
                     (DV_CELL, B.pick(f"clip limit {DV_CELL:g}", f"clip {DV_CELL:g}"))):
        o.append(vline(X(bar), Tp - 6, H - B_, "base", 1.5, "2 3"))
        o.append(T(X(bar) - 4, Tp - 10, lab, 11, "t2", "end" if bar == DV_CELL else "start"))
    for i, (c, t) in enumerate(rows):
        y = Tp + rowh * (i + 0.5)
        med, worst, _ = q[(c, t)]
        o.append(T(L - 12, y + 4, row_label(c, t), B.pick(13, 12), "t1", "end"))
        o.append(f"<line x1='{X(worst):.1f}' y1='{y}' x2='{X(med):.1f}' y2='{y}' class='l1' stroke-width='2'/>")
        o.append(f"<line x1='{X(worst):.1f}' y1='{y - 5}' x2='{X(worst):.1f}' y2='{y + 5}' class='l1' stroke-width='2'/>")
        o.append(marker(0, X(med), y, "s1", 5.5))
        o.append(T(X(med) + 10, y + 4, f"{med:+.2f}", 12, "t2"))
    o.append(T((L + W - R) / 2, H - 14, "VMAF v1 points ahead of x265 (right is better)", 13,
               "t2", "middle"))
    o.append("</svg>")
    return title, "\n".join(o)


def chart_curves(lad, cells, bds):
    cols = B.pick(4, 2)
    W, Tp = B.width(), 112
    pw, ph = (W - 48) / cols, 168
    nrow = math.ceil(len(cells) / cols)
    H = Tp + ph * nrow + 34
    above = sum(1 for k, *_ in cells if bds[k][0] < 0)
    title = f"yah265 needs fewer bits than x265 on {above} of {len(cells)} real-title cells at CRF"
    o = head(W, H, title, "VMAF v1 against bitrate on five CRF rungs per encoder (CRF 20 to 32). "
             "Up and left is better. Each panel has its own scales.",
             "Small multiples of VMAF v1 against bitrate for the two-minute and ten-minute "
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
        o.append(T(px0 - 30, py0 - 24, B.cell_name(title_, size, tier), B.pick(11.5, 11), "t1",
                   weight="600"))
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
               B.pick(12.5, 11.5), "t2", "middle"))
    o.append("</svg>")
    return title, "\n".join(o)


def chart_tracking(cells, tr):
    W, Tp, B_ = B.width(), 116, 62
    L, R = B.pick((250, 40), (206, 22))
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
             f"per encoder. The shaded band is yah265's ±{TRACK_BAR:g}% limit.",
             "Dot plot of VBR rate error per rung for the real-title cells, yah265 and x265.")
    B.legend(o, [(0, "s1", "yah265"), (1, "sx", "x265")])
    o.append(f"<rect x='{X(-TRACK_BAR):.1f}' y='{Tp - 8}' width='{X(TRACK_BAR) - X(-TRACK_BAR):.1f}' "
             f"height='{H - B_ - Tp + 8}' class='band'/>")
    for v in range(x0, x1 + 1, 5):
        o.append(vline(X(v), Tp - 8, H - B_, "base" if v == 0 else "grid", 1.5 if v == 0 else 1))
        if B.NARROW and v % 10:
            continue
        o.append(T(X(v), H - B_ + 20, B.pick("on target", "target") if v == 0 else f"{v:+d}%",
                   12, "t2", "middle"))
    for i, (key, title_, size, tier) in enumerate(cells):
        y = Tp + rowh * (i + 0.5)
        o.append(T(L - 12, y + 4, B.cell_name(title_, size, tier), B.pick(12.5, 11), "t1", "end"))
        for e in tr[key][1]:
            o.append(marker(1, X(e), y + 4, "sx", 3.4))
        for e in tr[key][0]:
            o.append(marker(0, X(e), y - 4, "s1", 3.8))
    o.append(T(B.pick((L + W - R) / 2, W / 2), H - 14, "achieved bitrate against the target (left is under)",
               13, "t2", "middle"))
    o.append("</svg>")
    return title, "\n".join(o)


def chart_lookahead(sweep):
    """sweep: [(key, label, mode, {depth: bd})]. Each line is one cell's
    BD-rate against its own value at depth 20."""
    nw = B.NARROW
    W, Tp, B_ = B.width(), 130, 70
    ph = 220
    H = Tp + (2 * ph + 60 if nw else ph) + B_
    depths = [12, 20, 30]
    deltas = {(k, m): {d: v[d] - v[20] for d in depths} for k, _, m, v in sweep}
    lo = math.floor(min(min(d.values()) for d in deltas.values()))
    hi = math.ceil(max(max(d.values()) for d in deltas.values()))
    med = {m: {d: st.median(v[d] for (k, mm), v in deltas.items() if mm == m) for d in depths}
           for m in ("crf", "vbr")}
    worst12 = max(v[12] for v in deltas.values())
    gain30 = st.median(v[30] for v in deltas.values())
    ncell = len({k for k, _ in deltas})
    title = (f"A lookahead of 12 costs up to {worst12:.1f} BD-rate points against 20. "
             f"Going to 30 moves the median {gain30:+.2f}")
    o = head(W, H, title.split(". ")[0], "BD-rate on VMAF v1 at each depth, less the same cell's at 20. x265 "
             "stays at its own 20. Below zero is better than 20.",
             f"Two line charts, CRF and VBR, of the change in BD-rate at lookahead depths 12, 20 "
             f"and 30 for {B.word(ncell)} cells, with the median drawn heavier.")
    o.append(f"<line x1='24' y1='86' x2='44' y2='86' class='l1' stroke-width='3'/>")
    o.append(T(50, 91, f"median of the {B.word(ncell)} cells", 13, "t2"))
    o.append(f"<line x1='230' y1='86' x2='250' y2='86' class='lx' stroke-width='1.5'/>")
    o.append(T(256, 91, "each cell", 13, "t2"))
    pw = W - 48 if nw else (W - 48 - 40) / 2
    for p, m in enumerate(("crf", "vbr")):
        px0 = 24 + 70 + (0 if nw else p * (pw + 40))
        px1 = px0 + pw - 70 - 104
        py = Tp + (p * (ph + 60) if nw else 0)
        X = lambda d: px0 + (d - 12) / 18 * (px1 - px0)
        Y = lambda v: py + (hi - v) / (hi - lo) * ph
        o.append(T(px0 - 70, py - 14, "CRF" if m == "crf" else "capped VBR", 14, "t1", weight="600"))
        for v in range(lo, hi + 1):
            o.append(hline(px0, px1, Y(v), "base" if v == 0 else "grid", 1.5 if v == 0 else 1))
            o.append(T(px0 - 8, Y(v) + 4, "same as 20" if v == 0 else f"{v:+d}", 11.5, "t2", "end"))
        for d in depths:
            o.append(T(X(d), py + ph + 20, f"{d} frames", 12, "t2", "middle"))
        far = max((k for (k, mm) in deltas if mm == m), key=lambda k: deltas[(k, m)][12])
        for k, lab, mm, _ in sweep:
            if mm != m:
                continue
            v = deltas[(k, m)]
            o.append(f"<polyline fill='none' class='lx' stroke-width='1.5' points='"
                     + " ".join(f"{X(d):.1f},{Y(v[d]):.1f}" for d in depths) + "'/>")
            if k == far:
                o.append(T(X(12) + 6, Y(v[12]) - 6, lab.split(",")[0], 11.5, "t2"))
        o.append(f"<polyline fill='none' class='l1' stroke-width='3' points='"
                 + " ".join(f"{X(d):.1f},{Y(med[m][d]):.1f}" for d in depths) + "'/>")
        for d in depths:
            o.append(marker(0, X(d), Y(med[m][d]), "s1", 4.5))
        o.append(T(X(30) + 10, Y(med[m][30]) + 4, f"median {med[m][30]:+.2f}", 12, "t1"))
    B.note(o, W / 2, H - 28, "yah265's lookahead depth across, BD-rate points against depth 20 "
           "up the side", B.pick(13, 12), "t2", W - 40, "middle")
    o.append("</svg>")
    return title, "\n".join(o)


def chart_tierc(tc):
    """tc: [(label, content, crf bd, vbr bd or None)]."""
    W, Tp, B_ = B.width(), 116, 62
    L, R = B.pick((250, 40), (186, 22))
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
        if B.NARROW and v % 20:
            continue
        o.append(T(X(v), H - B_ + 20, "0 (x265)" if v == 0 else f"{v:+d}%", 12, "t2", "middle"))
    o.append(vline(X(BD_BAR), Tp - 8, H - B_, "base", 1.5, "2 3"))
    o.append(T(X(BD_BAR) + B.pick(4, -4), Tp - 12, f"limit +{BD_BAR:.1f}%", 11, "t2",
               B.pick("start", "end")))
    for i, (lab, content, crf, vbr) in enumerate(tc):
        y = Tp + rowh * (i + 0.5)
        o.append(T(L - 12, y, lab, 12.5, "t1", "end"))
        o.append(T(L - 12, y + 14, content, 11, "t2", "end"))
        if vbr is None:
            o.append(T(X(0) - 10, y + 15, B.pick("VBR: no shared score range, yah265 above at "
                       "each rung", "VBR: no shared range, yah265 above"), 11, "t2", "end"))
        else:
            o.append(f"<line x1='{X(min(crf, vbr)):.1f}' y1='{y}' x2='{X(max(crf, vbr)):.1f}' "
                     f"y2='{y}' class='span' stroke-width='2'/>")
            o.append(marker(1, X(vbr), y + (4 if abs(crf - vbr) < 1.5 else 0), "s2", 5))
        o.append(marker(0, X(crf), y - (4 if vbr is not None and abs(crf - vbr) < 1.5 else 0),
                        "s1", 5.5))
    o.append(T(B.pick((L + W - R) / 2, W / 2), H - 14, "bitrate change for the same VMAF v1 (left is better)",
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
    return f'<figure class="chart"><img src="{REL}/{name}" alt="{esc(alt)}."></figure>'


def link(path, text):
    return f'<a href="{path}">{text}</a>'


def values(D):
    sps, rows, lad = D["sps"], D["rows"], D["lad"]
    cells, bds, tr, q = D["cells"], D["bds"], D["tr"], D["q"]
    isa, mac = D["isa"], D["mac"]
    V = {}
    L, R, C = "---", "--:", "---"

    V["fig-frames"] = B.frames_figure(B.RESULTS_PICKS, B.FRAMES_ALL)

    # the verdict
    combos = [(m, k) for m in MACHINES for k in sps[m]]
    ok = [1 for m, k in combos if sps[m][k][0] <= MEDIAN_BAR and sps[m][k][1] <= WORST_BAR]
    vbr, vbr_n = D["vbr"]
    walls = [v for _, _, v in B.vbr_walls(D["mac_md"])]
    walls += [B.fnum(r["ratio"]) for m in ("x86", "g4") for r in rows[m]
              if r["goal"] == "VBR" and r["valid"] == "yes"]
    allbd = [v for k, *_ in cells for v in bds[k]]
    tc = table(isa, "Tier C on the Mac")[1:]
    trk = [e for k, *_ in cells for e in tr[k][0]]
    pl = D["pl"]
    V["verdict"] = mdt(["what was measured", "result"], [L, R], [
        [f"median speed on the Mac at the same file size, {pl['n']} CRF cells",
         f"{B.headline_times(pl)} faster"],
        [f"median VMAF v1 lead at the same file size, the same {pl['dv_n']} cells",
         f"{pl['dv']:+.2f} points"],
        [f"median bits saved at equal VMAF v1, {len(allbd)} rate ladders",
         f"{abs(st.median(allbd)):.1f}%"],
        ["speed passes, per picture size and clip length, three goals on three machines",
         f"{len(ok)} of {len(combos)}"],
        [f"rate ladders inside the +{BD_BAR:.1f}% BD-rate limit",
         f"{sum(1 for v in allbd if v <= BD_BAR)} of {len(allbd)}"],
        ["capped VBR cells faster than x265 at the same target bitrate, on three machines",
         f"{sum(1 for v in walls if v < MEDIAN_BAR)} of {len(walls)}"],
        [f"VBR rungs within ±{TRACK_BAR:g}% of their target",
         f"{sum(1 for e in trk if abs(e) <= TRACK_BAR)} of {len(trk)}"],
        ["content-type clips passing on quality",
         f"{sum(1 for r in tc if r[-1] == 'PASS')} of {len(tc)}"]])
    V["goal-table"] = B.goal_table(sps)

    # how to read, the test set
    V["goal-defs"] = mdt(["goal", "yah265", "x265", "threads"], [L, L, L, L], [
        [B.glink("G3"), "as shipped, with SIMD", "as shipped, with its assembly", "each encoder's default"],
        [B.glink("G1"), "plain C (`N265_ASM_OFF=all`)", "plain C (`--no-asm`)",
         "one. x265 gets it from `--pools none --frame-threads 1`"],
        [B.glink("G2"), "plain C", "plain C", "each encoder's default"]])
    sets = {}
    for r in rows["mac"]:
        if r["tier"] in TIERS:
            sets.setdefault(r["tier"], {}).setdefault(r["class"], set()).add(r["title"])
    how = {"S": "each clip whole", "M": "from the first non-black frame",
           "L": "600 seconds past the opening card, stopped before the end credits",
           "C": "seven short clips and one ten-minute film scan at two sizes"}
    trow = []
    for t in TIERS:
        per = [len(sets[t][c]) if c in sets[t] else "" for c in CLASSES]
        names = sorted({TITLE.get(B.base_title(x), B.base_title(x))
                        for c in sets[t] for x in sets[t][c]}, key=str.lower)
        trow.append([f"{t}, {TIER_LABEL[t]}", *per, ", ".join(names), how[t]])
    kinds = {}
    for r in tc:
        kinds.setdefault(r[1], []).append(r[0])
    cper = {}
    for r in tc:
        cper[split_cell(r[0])[1]] = cper.get(split_cell(r[0])[1], 0) + 1
    trow.append(["C, content types", *[cper.get(c, "") for c in CLASSES],
                 ", ".join(f"{len(v)} {k}" for k, v in kinds.items()), how["C"]])
    V["test-set"] = mdt(["tier", *[RES[c] for c in CLASSES], "what's in it", "how it's cut"],
                        [L, R, R, R, R, L, L], trow)

    # speed on the Mac
    sp = sps["mac"]
    srows = []
    for c, t in B.speed_rows([sp]):
        srows.append([RES[c], TIER_LABEL[t]] + [pair(*sp[(c, t, x)][:2]) for x in GOAL_ORDER])
    V["speed-mac"] = mdt(["picture size", "clip length", *map(B.glink, GOAL_ORDER)], [L, L, R, R, R], srows)
    frows = []
    for line in open(D["mac_md"]):
        m = re.match(r"- (\S+) \(end frame (\d+)\): G1 (.+?); G2 (.+?); G3 (.+)$", line.rstrip())
        if m and B.shown(split_cell(m[1])[0]):
            g1, g2, g3 = (f"{tf(float(x)):.2f}x" for x in m.groups()[2:])
            frows.append([nice(*split_cell(m[1])), m[2], g3, g1, g2])
    V["films"] = mdt(["film", "last story frame", *map(B.glink, GOAL_ORDER)], [L] + [R] * (len(B.CAPPED_PAIRS) + 1), frows)

    # by machine, from each machine's rows
    def mrow(c, t, cols):
        return [RES[c], TIER_LABEL[t]] + [pair(*v[:2]) if v else "not measured" for v in cols]
    grid = B.speed_rows(sps.values())
    for g in ("G1", "G2"):
        V[f"machines-{g.lower()}"] = mdt(
            ["picture size", "clip length", "Mac", "x86-64", "Graviton4"], [L, L, R, R, R],
            [mrow(c, t, [sps[m].get((c, t, g)) for m in MACHINES]) for c, t in grid])
    s512 = D["s512"]
    V["machines-g3"] = mdt(
        ["picture size", "clip length", "Mac", "x86-64", "x86-64 against x265 at `--asm avx512`",
         "Graviton4"],
        [L, L, R, R, R, R],
        [mrow(c, t, [sps["mac"].get((c, t, "G3")), sps["x86"].get((c, t, "G3")),
                     s512.get((c, t, "G3")), sps["g4"].get((c, t, "G3"))]) for c, t in grid])
    V["graviton3"] = mdt(["picture size", "Graviton3", "clips"], [L, R, R],
                         [[RES[c], pair(*D["g3"][c][:2]), D["g3"][c][2]]
                          for c in CLASSES if c in D["g3"]])
    cap = D["cap"]
    V["capped"] = mdt(["picture size"] + [lab for *_, lab in B.CAPPED_PAIRS] + ["clips"], [L, R, R, R, R],
                      [[RES[c]] + [pair(*cap[c][k][:2]) for k, *_ in B.CAPPED_PAIRS]
                       + [cap[c]["avx2"][2]] for c in CLASSES if c in cap])
    vkeys = sorted(vbr, key=lambda r: (CLASSES.index(r[0]), TIERS.index(r[1])))
    V["vbr-machines"] = mdt(["picture size", "clip length", "Mac", "x86-64", "Graviton4", "cells"],
                            [L, L, R, R, R, R],
                            [mrow(c, t, [vbr[(c, t)].get(m) for m in MACHINES]) + [vbr_n[(c, t)]]
                             for c, t in vkeys])

    # quality at the same size
    how_q = {(r[0], r[1]): r[5:7] for r in table(isa, "Quality on each machine")[1:]}
    V["quality"] = mdt(["picture size", "clip length", "median", "smallest lead", "on"], [L, L, R, R, L],
                       [[RES[c], TIER_LABEL[t], f"{q[(c, t)][0]:+.2f}", f"{q[(c, t)][1]:+.2f}",
                         nice(q[(c, t)][2], c)]
                        for c, t in sorted(q, key=lambda r: (CLASSES.index(r[0]), TIERS.index(r[1])))])
    xrows = []
    for (c, t), (x86, _) in how_q.items():
        m = re.match(r"reproduced: (\w+) (.+)$", x86)
        if m and B.shown(m[1]):
            xrows += [[f"{nice(m[1], c)}, {TIER_LABEL[t]}", mode, moved]
                      for mode, moved in re.findall(r"(CRF|VBR) ([+-][\d.]+)", m[2])]
    V["quality-xeon"] = mdt(["ladder run again on the Xeon", "mode", "BD-rate points from the Mac's curve"],
                            [L, L, R], xrows)

    # ladders
    V["bd"] = mdt(["cell", "clip length", "CRF BD-rate", "VBR BD-rate"], [L, L, R, R],
                  [[nice(ti, s), TIER_LABEL[t], pct(bds[k][0]), pct(bds[k][1])]
                   for k, ti, s, t in cells])
    tight = max((v, k, i) for k, *_ in cells for i, v in enumerate(bds[k]))
    _, ti, s, t = next(c for c in cells if c[0] == tight[1])
    V["bd-summary"] = mdt(["over the ladders", "value"], [L, R], [
        ["median BD-rate, CRF", pct(st.median(bds[k][0] for k, *_ in cells), 1)],
        ["median BD-rate, capped VBR", pct(st.median(bds[k][1] for k, *_ in cells), 1)],
        ["median BD-rate, both", pct(st.median(allbd), 1)],
        ["ladders where yah265 needs fewer bits", f"{sum(1 for v in allbd if v < 0)} of {len(allbd)}"],
        [f"closest to the quality limit: {nice(ti, s)}, {TIER_LABEL[t]}, {'VBR' if tight[2] else 'CRF'}",
         pct(tight[0])]])
    V["vbr-track"] = mdt(["cell", "clip length", "yah265, five rungs", "x265, five rungs"], [L, L, R, R],
                         [[nice(ti, s), TIER_LABEL[t], rng(tr[k][0]), rng(tr[k][1])]
                          for k, ti, s, t in cells])
    V["vbr-wall"] = mdt(["cell", "clip length", "target kbit/s", "speed", "yah265 against target",
                         "x265 against target"], [L, L, R, R, R, R],
                        [[nice(*split_cell(r[0])), TIER_LABEL[r[1]], r[2], f"{tf(float(r[3])):.2f}x",
                          re.search(r"\((.+?)\)", r[4])[1], re.search(r"\((.+?)\)", r[5])[1]]
                         for r in table(mac, "VBR wall")[1:] if B.shown(split_cell(r[0])[0])])
    V["lookahead"] = mdt(["cell", "mode", "yah265 at 12", "yah265 at 20", "yah265 at 30"],
                         [L, L, R, R, R],
                         [[lab, "CRF" if m == "crf" else "VBR"] + [pct(v[d]) for d in (12, 20, 30)]
                          for _, lab, m, v in D["sweep"]])

    # tier C
    V["tier-c"] = mdt(["clip", "content", "VMAF v1 lead", "CRF BD-rate", "VBR BD-rate",
                       "yah265 VBR rate against target", B.glink("G3") + " speed"],
                      [L, L, R, R, R, R, R],
                      [[nice(*split_cell(r[0])), r[1], r[2].split("/")[0].strip(),
                        pct(crf), "no shared range" if vbr is None else pct(vbr),
                        rng(D["ctr"][r[0]]), f"{tf(float(r[5].split()[0])):.2f}x"
                        + (" \\*" if "(" in r[5] else "")]
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
    rows = {m: B.report_rows(p) for m, p in paths.items()}
    D = {"paths": paths, "isa_path": isa_path, "mac_md": mac_md, "rows": rows,
         "sps": {m: B.speed_groups(rs) for m, rs in rows.items()},
         "q": quality_rows(rows["mac"]), "isa": md_tables(isa_path), "mac": md_tables(mac_md),
         "pl": B.headline_cells(rows["mac"])}
    D["s512"] = B.speed_groups(rows["x86"], ("G3",), "ref512_wall_s")
    D["vbr"] = B.vbr_groups(mac_md, {m: rows[m] for m in ("x86", "g4")})
    D["cap"] = B.capped_run(B.capped_records(), paths["x86"][:-4] + ".md")
    g3 = sorted(glob.glob(os.path.join(ROOT, "docs/data/cloud-tables-*/records/g3S.jsonl")))
    if not g3:
        sys.exit("no Graviton3 records (docs/data/cloud-tables-*/records/g3S.jsonl)")
    D["g3"] = B.graviton3(g3[-1])
    check_reading(D["isa"], paths, mac_md, g3[-1])
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


def check_reading(isa, paths, mac_md, g3_path):
    """This script's medians, taken with every title in, against the tables
    the all-architecture report prints. A mismatch means the reading code
    and the report have parted, so stop."""
    def same(what, mine, theirs):
        if [x.strip() for x in theirs.split("(")[0].split("/")] != [f"{y:.2f}" for y in mine[:2]]:
            sys.exit(f"{what}: {mine[0]:.2f} / {mine[1]:.2f} from the rows, "
                     f"the all-architecture report says {theirs}")
    full = {m: B.report_rows(p, everything=True) for m, p in paths.items()}
    for m in MACHINES:
        for (c, t, g), v in B.speed_groups(full[m]).items():
            col = {"mac": 2, "x86": 3, "g4": 5 if g == "G3" else 4}[m]
            rep = next(r for r in table(isa, f"{g} speed")[1:] if (r[0], r[1]) == (c, t))
            same(f"{m} {c} {t} {g}", v, rep[col])
    g3rows = table(isa, "G3 speed")[1:]
    for (c, t, _), v in B.speed_groups(full["x86"], ("G3",), "ref512_wall_s").items():
        same(f"x86 avx512 {c} {t}", v, next(r for r in g3rows if (r[0], r[1]) == (c, t))[4])
    for m, col in (("x86", 3), ("g4", 4)):
        for (c, t, _), v in B.speed_groups(full[m], ("VBR",)).items():
            same(f"{m} VBR {c} {t}", v,
                 next(r for r in table(isa, "VBR wall")[1:] if (r[0], r[1]) == (c, t))[col])


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--check", action="store_true",
                    help="change nothing; exit 1 if the page or a chart is out of date")
    a = ap.parse_args()
    D = gather()
    fns = {"speed": lambda: chart_speed(D["sps"]["mac"]),
           "machines": lambda: chart_machines(D["sps"]),
           "quality": lambda: chart_quality(D["q"]),
           "curves": lambda: chart_curves(D["lad"], D["cells"], D["bds"]),
           "tracking": lambda: chart_tracking(D["cells"], D["tr"]),
           "lookahead": lambda: chart_lookahead(D["sweep"]),
           "tierc": lambda: chart_tierc(D["tc"])}
    D["titles"] = {k: fn()[0] for k, fn in fns.items()}
    made = B.render({k: (lambda fn=fn: fn()[1]) for k, fn in fns.items()})
    V = values(D)
    mac, lf = D["paths"]["mac"], B.LADDERS
    crv = []
    for key, ti, s, t in D["cells"]:
        for enc, pts in zip(("yah265", "x265"), curve(D["lad"], key, "crf")):
            crv += [[nice(ti, s), t, enc, k, v, D["bds"][key][0]] for k, v in pts]
    trk = []
    for key, ti, s, t in D["cells"]:
        o, x = D["tr"][key]
        trk += [[nice(ti, s), t, i + 1, a_, b_] for i, (a_, b_) in enumerate(zip(o, x))]
    stale = B.publish_data(OUT, "charts_results.py", {
        "speed": ([mac], *B.speed_csv({"mac": D["sps"]["mac"]})),
        "machines": (list(D["paths"].values()), *B.speed_csv(D["sps"])),
        "quality": ([mac], ["class", "tier", "median_vmaf_v1_lead", "smallest_lead", "smallest_on"],
                    [[c, t, *D["q"][(c, t)]] for c in CLASSES for t in TIERS if (c, t) in D["q"]]),
        "curves": ([lf], ["cell", "tier", "encoder", "kbps", "vmaf_v1", "bd_rate_crf_pct"], crv),
        "tracking": ([lf], ["cell", "tier", "rung", "yah265_rate_error_pct", "x265_rate_error_pct"],
                     trk),
        "lookahead": ([lf], ["cell", "mode", "bd_rate_pct_la12", "bd_rate_pct_la20",
                             "bd_rate_pct_la30"],
                      [[name, m, v[12], v[20], v[30]] for _, name, m, v in D["sweep"]]),
        "tierc": ([lf], ["clip", "content", "bd_rate_crf_pct", "bd_rate_vbr_pct"],
                  [list(r) for r in D["tc"]]),
    }, a.check)
    for name, svg in made.items():
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
          f"{sum(len(s) for s in D['sps'].values())} speed groups, {len(fns)} charts")
    if a.check:
        if stale:
            print("stale: " + ", ".join(stale))
            sys.exit(1)
        print("up to date")
    else:
        print("updated: " + (", ".join(stale) or "nothing changed"))


if __name__ == "__main__":
    main()
