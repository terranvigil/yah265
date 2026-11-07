#!/usr/bin/env python3
# Copyright (c) 2026, the yah265 authors
# SPDX-License-Identifier: GPL-2.0-or-later
"""The parity post's charts and numbers, from the data files.

Reads the newest parity report (docs/parity/PARITY-REPORT-yah265-<date>.csv
and the .md beside it) and docs/data/parity-history.csv. Writes three
charts into site/assets/blog/:

    speed.svg     times faster than x265, median per class and tier, G1-G3
    bdrate.svg    BD-rate on VMAF v1 for every ladder, CRF and VBR
    history.svg   the timed board's medians, commit by commit

and refills every `<!--gen:NAME-->...<!--/gen:NAME-->` span in
site/blog.md, so no figure on the page is typed by hand.

    scripts/blog_charts.py                  # newest report
    scripts/blog_charts.py --report docs/parity/PARITY-REPORT-yah265-2026-11-04.csv
    scripts/blog_charts.py --check          # exit 1 if the page or a chart is stale
"""
import argparse
import csv
import glob
import math
import os
import re
import statistics as st
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
POST = os.path.join(ROOT, "site/blog.md")
OUT = os.path.join(ROOT, "site/assets/blog")
HISTORY = os.path.join(ROOT, "docs/data/parity-history.csv")

CLASSES = ["CIF", "720p", "1080p", "2160p"]
TIERS = ["S", "M", "L", "C"]
GOALS = ["G1", "G2", "G3"]
GOAL_LABEL = {"G1": "G1: C, one thread", "G2": "G2: C, all cores",
              "G3": "G3: SIMD, all cores"}
TIER_LABEL = {"S": "short clips", "M": "2-min segments",
              "L": "10-min films", "C": "content types"}
RES = {"CIF": "CIF", "720p": "720p", "1080p": "1080p", "2160p": "4K"}
TITLE = {"chimera": "Chimera", "sollevante": "Sol Levante",
         "meridian": "Meridian", "tears": "Tears of Steel",
         "bbb": "Big Buck Bunny", "sintel": "Sintel",
         "cosmos": "Cosmos Laundromat", "stem2": "StEM2",
         "nocturne": "Nocturne", "lash1924": "Lash of the Whip"}
BD_BAR = 1.0

# board commits worth a label on the history chart: (commit, text)
EVENTS = [("8061928", "threads and NEON kernels"),
          ("358db2f", "medium's effort trimmed"),
          ("4e8fe2c", "medium becomes the goal preset")]


# ------------------------------------------------------------------ data

def newest_report():
    rs = sorted(glob.glob(os.path.join(ROOT, "docs/parity/PARITY-REPORT-yah265-*.csv")))
    if not rs:
        sys.exit("no parity report CSV under docs/parity/")
    return rs[-1]


def fnum(s):
    try:
        return float(s)
    except (TypeError, ValueError):
        return None


def speed_groups(rows):
    """(class, tier, goal) -> (median, worst, worst title) over valid rows,
    the same rule the report uses."""
    acc = {}
    for r in rows:
        v = fnum(r["ratio"])
        if r["valid"] != "yes" or v is None or "FAIL:" in r["note"]:
            continue
        acc.setdefault((r["class"], r["tier"], r["goal"]), []).append((v, r["title"]))
    return {k: (st.median(x for x, _ in vs), max(vs)[0], max(vs)[1])
            for k, vs in acc.items()}


def quality_groups(rows):
    """(class, tier) -> median dVMAF v1 at matched size, from the G3 rows
    (the default-threading anchor the report reads quality at)."""
    acc = {}
    for r in rows:
        v = fnum(r["dvmafneg"])
        if r["goal"] == "G3" and v is not None:
            acc.setdefault((r["class"], r["tier"]), []).append(v)
    return {k: st.median(v) for k, v in acc.items()}


BD_LINE = re.compile(r"^- (\S+) \(([A-Z])\): CRF BD-VMAF v1 ([-+]\d+\.\d+)%.*?"
                     r"VBR BD-VMAF v1 ([-+]\d+\.\d+)%")


def ladders(md_path):
    """[(title, size, tier, crf, vbr)] from the report's BD section."""
    out = []
    for line in open(md_path):
        m = BD_LINE.match(line)
        if m:
            title, size = m[1].rsplit("_", 1)
            out.append((title, size, m[2], float(m[3]), float(m[4])))
    return out


def credits(md_path):
    """(titles behind on their end credits, titles read) from the report's
    supplementary credits table."""
    rows, on = [], False
    for line in open(md_path):
        if line.startswith("## "):
            on = line.startswith("## End credits")
        elif on and line.startswith("| ") and not line.startswith("| title"):
            rows.append(line.rstrip().rstrip("|").split("|")[-1].strip())
    return sum(1 for r in rows if r.startswith("**behind")), len(rows)


def board_history():
    """One point per timed board on x265 4.3: the median over the corpus of
    our wall over x265's at G1 (pure C, one thread) and at G3 (shipped, all
    cores). Partial boards (under 13 clips) are left out."""
    groups = {}
    order = []
    for r in csv.DictReader(open(HISTORY)):
        if not r["x265"].startswith("4.3"):
            continue
        k = (r["date"], r["commit"])
        if k not in groups:
            groups[k] = []
            order.append(k)
        groups[k].append(r)
    pts = []
    for k in order:
        rs = groups[k]
        g1 = [fnum(r["mult_matched_g1"]) for r in rs if fnum(r["mult_matched_g1"])]
        g3 = [fnum(r["x265med_asm_fps_auto"]) / fnum(r["our_fps_auto"]) for r in rs
              if fnum(r["our_fps_auto"]) and fnum(r["x265med_asm_fps_auto"])]
        if len(g1) < 13:
            continue
        pts.append({"date": k[0], "commit": k[1], "n": len(g1),
                    "g1": st.median(g1), "g3": st.median(g3) if len(g3) >= 13 else None})
    return pts


# ------------------------------------------------------------------ charts

STYLE = """<style>
.bg{fill:#fcfcfb}.t1{fill:#0b0b0b}.t2{fill:#52514e}.grid{stroke:#e6e5e1}
.base{stroke:#8a8984}.ring{stroke:#fcfcfb}
.s1{fill:#2a78d6}.s2{fill:#eb6834}.s3{fill:#1baf7a}
.l1{stroke:#2a78d6}.l2{stroke:#eb6834}.l3{stroke:#1baf7a}.span{stroke:#e6e5e1}
text{font-family:-apple-system,"IBM Plex Sans",Helvetica,Arial,sans-serif}
@media (prefers-color-scheme:dark){
.bg{fill:#1a1a19}.t1{fill:#f0efec}.t2{fill:#c3c2b7}.grid{stroke:#2e2e2c}
.base{stroke:#8a8984}.ring{stroke:#1a1a19}
.s1{fill:#3987e5}.s2{fill:#d95926}.s3{fill:#199e70}
.l1{stroke:#3987e5}.l2{stroke:#d95926}.l3{stroke:#199e70}.span{stroke:#3a3a37}}
</style>"""


def esc(s):
    return str(s).replace("&", "&amp;").replace("<", "&lt;").replace(">", "&gt;")


def T(x, y, s, size=13, cls="t1", anchor="start", weight="normal"):
    w = "" if weight == "normal" else f" font-weight='{weight}'"
    return (f"<text x='{x:.1f}' y='{y:.1f}' font-size='{size}' class='{cls}' "
            f"text-anchor='{anchor}'{w}>{esc(s)}</text>")


def head(W, H, title, desc):
    return [f"<svg xmlns='http://www.w3.org/2000/svg' width='{W}' height='{H}' "
            f"viewBox='0 0 {W} {H}' role='img'>",
            f"<title>{esc(title)}</title><desc>{esc(desc)}</desc>", STYLE,
            f"<rect width='100%' height='100%' class='bg'/>"]


def marker(shape, x, y, cls, r=6):
    """A distinct shape per series, so identity never rests on color."""
    if shape == 0:
        return f"<circle cx='{x:.1f}' cy='{y:.1f}' r='{r}' class='{cls} ring' stroke-width='2'/>"
    if shape == 1:
        s = r * 0.9
        return (f"<rect x='{x - s:.1f}' y='{y - s:.1f}' width='{2 * s:.1f}' height='{2 * s:.1f}' "
                f"rx='1.5' class='{cls} ring' stroke-width='2'/>")
    s = r * 1.25
    return (f"<polygon points='{x:.1f},{y - s:.1f} {x + s:.1f},{y:.1f} {x:.1f},{y + s:.1f} "
            f"{x - s:.1f},{y:.1f}' class='{cls} ring' stroke-width='2'/>")


def legend(o, items, y=90, x=24, gap=24):
    for shape, cls, label in items:
        o.append(marker(shape, x + 6, y - 4, cls, 5.5))
        o.append(T(x + 18, y + 1, label, 13, "t2"))
        x += 18 + len(label) * 7.0 + gap


def speed_rows(sp):
    rows = []
    for tier in TIERS:
        for cls in CLASSES:
            if any((cls, tier, g) in sp for g in GOALS):
                rows.append((cls, tier))
    rows.sort(key=lambda r: (CLASSES.index(r[0]), TIERS.index(r[1])))
    return rows


def chart_speed(sp, date):
    rows = speed_rows(sp)
    vals = [1 / round(sp[(c, t, g)][0], 2) for c, t in rows for g in GOALS if (c, t, g) in sp]
    W, L, R, Tp, B = 900, 200, 40, 116, 62
    rowh = 40
    H = Tp + B + rowh * len(rows)
    x0 = 0.75
    x1 = max(2.0, int(max(vals) * 2 + 1.999) / 2)
    X = lambda v: L + (v - x0) / (x1 - x0) * (W - L - R)
    o = head(W, H, "yah265 is faster than x265 in every class",
             "Median speedup over x265 medium per class and tier, for the three goals.")
    o.append(T(24, 40, "yah265 is faster than x265 in every class and tier", 22, weight="600"))
    o.append(T(24, 64, f"Median speedup over x265 medium at the same file size. "
                       f"Apple M5 Max, 18 cores. Report of {date}.", 14, "t2"))
    legend(o, [(i, f"s{i + 1}", GOAL_LABEL[g]) for i, g in enumerate(GOALS)])
    v = 1.0
    while v <= x1 + 1e-9:
        cls = "base" if v == 1 else "grid"
        dash = " stroke-dasharray='4 3'" if v == 1 else ""
        o.append(f"<line x1='{X(v):.1f}' y1='{Tp - 8}' x2='{X(v):.1f}' y2='{H - B}' "
                 f"class='{cls}' stroke-width='{1.5 if v == 1 else 1}'{dash}/>")
        lab = "1x (x265)" if v == 1 else f"{v:g}x"
        o.append(T(X(v), H - B + 20, lab, 12, "t2", "middle"))
        v += 0.5
    o.append(T((L + W - R) / 2, H - 14, "times as fast as x265 (right is faster)", 13, "t2", "middle"))
    for i, (c, t) in enumerate(rows):
        y = Tp + rowh * (i + 0.5)
        o.append(T(L - 12, y + 4, f"{RES[c]} {TIER_LABEL[t]}", 13, "t1", "end"))
        got = {g: 1 / round(sp[(c, t, g)][0], 2) for g in GOALS if (c, t, g) in sp}
        if len(got) > 1:
            o.append(f"<line x1='{X(min(got.values())):.1f}' y1='{y}' "
                     f"x2='{X(max(got.values())):.1f}' y2='{y}' class='span' stroke-width='2'/>")
        for j, g in enumerate(GOALS):
            if g not in got:
                continue
            close = [k for k in GOALS if k in got and abs(got[k] - got[g]) < 0.06]
            dy = 0 if len(close) == 1 else (close.index(g) - (len(close) - 1) / 2) * 10
            o.append(marker(j, X(got[g]), y + dy, f"s{j + 1}"))
        miss = [g for g in GOALS if g not in got]
        if miss:
            o.append(T(W - R, y + 4, "not measured: " + ", ".join(miss), 11, "t2", "end"))
    o.append("</svg>")
    return "\n".join(o)


def cell_name(title, size, tier):
    res = {"2160p": "4K"}.get(size, size)
    dur = {"M": "2 min", "L": "10 min"}.get(tier, tier)
    return f"{TITLE.get(title, title)} {res} ({dur})"


def chart_bd(lad, date):
    rows = sorted(lad, key=lambda r: (r[3] + r[4]) / 2)
    allv = [r[3] for r in lad] + [r[4] for r in lad]
    W, L, R, Tp, B = 900, 250, 40, 116, 62
    rowh = 24
    H = Tp + B + rowh * len(rows)
    x0 = min(-5, int((min(allv) - 4.999) / 5) * 5)
    x1 = 5
    X = lambda v: L + (v - x0) / (x1 - x0) * (W - L - R)
    medv = st.median(allv)
    o = head(W, H, f"A median {abs(medv):.1f}% fewer bits for the same VMAF v1",
             "BD-rate against x265 medium on VMAF v1 for every rate ladder, CRF and capped VBR.")
    o.append(T(24, 40, f"A median {abs(medv):.1f}% fewer bits for the same quality", 22, weight="600"))
    o.append(T(24, 64, f"BD-rate against x265 medium on VMAF v1, {len(allv)} rate ladders on real "
                       f"titles. Left of zero needs fewer bits. Report of {date}.", 14, "t2"))
    legend(o, [(0, "s1", "CRF"), (1, "s2", "capped VBR")])
    bx = X(BD_BAR)
    for v in range(x0, x1 + 1, 5):
        cls = "base" if v == 0 else "grid"
        o.append(f"<line x1='{X(v):.1f}' y1='{Tp - 8}' x2='{X(v):.1f}' y2='{H - B}' "
                 f"class='{cls}' stroke-width='{1.5 if v == 0 else 1}'/>")
        o.append(T(X(v), H - B + 20, "0 (x265)" if v == 0 else f"{v:+d}%", 12, "t2", "middle"))
    o.append(f"<line x1='{bx:.1f}' y1='{Tp - 8}' x2='{bx:.1f}' y2='{H - B}' class='base' "
             f"stroke-width='1.5' stroke-dasharray='2 3'/>")
    o.append(T(bx + 4, Tp - 12, f"bar +{BD_BAR:g}%", 11, "t2"))
    o.append(T((L + W - R) / 2, H - 14, "bitrate change for the same VMAF v1 (left is better)",
               13, "t2", "middle"))
    for i, (title, size, tier, crf, vbr) in enumerate(rows):
        y = Tp + rowh * (i + 0.5)
        o.append(T(L - 12, y + 4, cell_name(title, size, tier), 12.5, "t1", "end"))
        o.append(f"<line x1='{X(min(crf, vbr)):.1f}' y1='{y}' x2='{X(max(crf, vbr)):.1f}' "
                 f"y2='{y}' class='span' stroke-width='2'/>")
        dy = 4 if abs(crf - vbr) < 0.8 else 0
        o.append(marker(1, X(vbr), y + dy, "s2", 5))
        o.append(marker(0, X(crf), y - dy, "s1", 5.5))
    o.append("</svg>")
    return "\n".join(o)


def chart_history(pts):
    W, H, L, R, Tp, B = 900, 500, 92, 100, 160, 64
    # y is log2 of the speed multiple (x265's wall over ours)
    ys = [math.log2(1 / p[k]) for p in pts for k in ("g1", "g3") if p[k]]
    y0, y1 = math.floor(min(ys)), max(1, math.ceil(max(ys)))
    n = len(pts)
    X = lambda i: L + (i + 0.5) / n * (W - L - R)
    Y = lambda v: Tp + (y1 - v) / (y1 - y0) * (H - Tp - B)
    first, last = pts[0], pts[-1]
    o = head(W, H, "From many times slower to faster than x265",
             "Median speed of yah265 against x265 medium on the timed board, one point per board run.")
    o.append(T(24, 40, "From many times slower to faster than x265", 22, weight="600"))
    o.append(T(24, 64, f"Median over the board's {first['n']}-{max(p['n'] for p in pts)} short clips, "
                       f"one point per timed board, {first['date']} to {last['date']}.", 14, "t2"))
    legend(o, [(0, "s1", "C, one thread"), (2, "s3", "SIMD, all cores")])
    for v in range(y0, y1 + 1):
        cls = "base" if v == 0 else "grid"
        o.append(f"<line x1='{L}' y1='{Y(v):.1f}' x2='{W - R}' y2='{Y(v):.1f}' class='{cls}' "
                 f"stroke-width='{1.5 if v == 0 else 1}'{' stroke-dasharray=\"4 3\"' if v == 0 else ''}/>")
        lab = "even" if v == 0 else (f"{2 ** v:g}x faster" if v > 0 else f"{2 ** -v:g}x slower")
        o.append(T(L - 10, Y(v) + 4, lab, 12, "t2", "end"))
    # a date under every sixth board and the last
    step = max(1, n // 5)
    for i in sorted(set(range(0, n, step)) | {n - 1}):
        if i != n - 1 and n - 1 - i < step / 2:
            continue
        o.append(f"<line x1='{X(i):.1f}' y1='{H - B}' x2='{X(i):.1f}' y2='{H - B + 5}' class='base'/>")
        o.append(T(X(i), H - B + 20, pts[i]["date"], 12, "t2", "middle"))
    o.append(T((L + W - R) / 2, H - 12, "timed board runs in commit order (not to time scale)",
               13, "t2", "middle"))
    for k, cls, shape in (("g1", "1", 0), ("g3", "3", 2)):
        seq = [(X(i), Y(math.log2(1 / p[k]))) for i, p in enumerate(pts) if p[k]]
        o.append(f"<polyline fill='none' class='l{cls}' stroke-width='2' points='"
                 + " ".join(f"{x:.1f},{y:.1f}" for x, y in seq) + "'/>")
        for x, y in seq:
            o.append(marker(shape, x, y, f"s{cls}", 4))
    for j, (commit, text) in enumerate(EVENTS):
        idx = next((i for i, p in enumerate(pts) if p["commit"] == commit), None)
        if idx is None:
            continue
        x, ty = X(idx), Tp - 40 + 15 * j
        o.append(f"<line x1='{x:.1f}' y1='{ty + 4}' x2='{x:.1f}' y2='{H - B}' class='base' "
                 f"stroke-width='1' stroke-dasharray='2 3'/>")
        o.append(T(x - 4, ty, text, 11.5, "t2", "end"))
    for k, cls in (("g1", "s1"), ("g3", "s3")):
        if last[k]:
            o.append(T(X(n - 1) + 10, Y(math.log2(1 / last[k])) + 4,
                       f"{1 / last[k]:.2f}x faster" if last[k] < 1 else f"{last[k]:.2f}x slower",
                       12, "t1"))
    o.append("</svg>")
    return "\n".join(o)


# ------------------------------------------------------------------ values

def fx(ratio):
    """Times as fast, from the ratio as the report prints it (2 places), so
    the page agrees with the results tables."""
    return f"{1 / round(ratio, 2):.2f}x"


def values(sp, q, lad, date, report_rel, hist, cred):
    V = {}
    for g in GOALS:
        meds = [v[0] for (c, t, gg), v in sp.items() if gg == g and t != "C"]
        V[f"{g.lower()}-range"] = f"{fx(max(meds))} to {fx(min(meds))}"
    worst = max(((v[1], v[2], c, t, g) for (c, t, g), v in sp.items() if t != "C"))
    close = max(((v[0], c, t, g) for (c, t, g), v in sp.items() if t != "C"))
    V["closest-group"] = f"{RES[close[1]]} {TIER_LABEL[close[2]]} on {close[3]}"
    base = re.sub(r"_(cif|\d+p)$", "", worst[1])
    V["slowest-cell"] = f"{TITLE.get(base, base)} at {RES[worst[2]]} on {worst[4]}"
    V["slowest-x"] = fx(worst[0])
    combos = [(c, t, g) for (c, t, g) in sp if t != "C"]
    passing = [k for k in combos if sp[k][0] <= 1.0 and sp[k][1] <= 1.15]
    V["combos"] = f"{len(passing)} of {len(combos)}"
    allv = [r[3] for r in lad] + [r[4] for r in lad]
    V["ladders"] = str(len(allv))
    V["ladders-ahead"] = str(sum(1 for v in allv if v < 0))
    V["ladders-in-bar"] = str(sum(1 for v in allv if v <= BD_BAR))
    V["bd-median"] = f"{abs(st.median(allv)):.1f}%"
    V["bd-crf"] = f"{abs(st.median(r[3] for r in lad)):.1f}%"
    V["bd-vbr"] = f"{abs(st.median(r[4] for r in lad)):.1f}%"
    tightest = max([(r[3], r[0], r[1], r[2], "CRF") for r in lad]
                   + [(r[4], r[0], r[1], r[2], "VBR") for r in lad])
    V["bd-tightest"] = f"{cell_name(tightest[1], tightest[2], tightest[3])} {tightest[4]}"
    V["bd-tightest-v"] = f"{tightest[0]:+.2f}%"
    qv = list(q.values())
    V["dv-min"] = f"{min(qv):+.2f}"
    V["dv-max"] = f"{max(qv):+.2f}"
    V["report-date"] = date
    V["report-link"] = f"[the parity report of {date}]({report_rel})"
    link = f"[The parity report of {date}]({report_rel})"
    sp_ok = len(passing) == len(combos)
    bd_ok = V["ladders-in-bar"] == V["ladders"]
    V["parity-line"] = (
        f"{link} has " + ("every speed combination passing" if sp_ok else
                          f"{len(passing)} of {len(combos)} speed combinations passing")
        + ". " + ("Every ladder is inside the bar." if bd_ok else
                  f"{V['ladders-in-bar']} of {V['ladders']} ladders are inside the bar."))
    behind, read = cred
    V["credits"] = (f"On {behind} of {read} titles yah265 needs more bytes than x265 there."
                    if behind else f"yah265 is level or ahead on all {read} titles there.")
    g3s = [p for p in hist if p["g3"]]
    V["hist-g3-first"] = f"{g3s[0]['g3']:.1f}x" if g3s else "-"
    V["hist-g3-first-date"] = g3s[0]["date"] if g3s else "-"
    V["hist-g1-first"] = f"{hist[0]['g1']:.1f}x" if hist else "-"
    V["hist-g1-first-date"] = hist[0]["date"] if hist else "-"
    V["hist-last-date"] = hist[-1]["date"] if hist else "-"
    V["speed-table"] = speed_table(sp, q)
    return V


def speed_table(sp, q):
    L = ["| class | tier | G1, C on one thread | G2, C on all cores | G3, SIMD on all cores | VMAF v1 at the same size |",
         "|---|---|--:|--:|--:|--:|"]
    for c, t in speed_rows(sp):
        cols = [fx(sp[(c, t, g)][0]) if (c, t, g) in sp else "not measured" for g in GOALS]
        qq = f"{q[(c, t)]:+.2f}" if (c, t) in q else "not measured"
        L.append(f"| {RES[c]} | {TIER_LABEL[t]} | " + " | ".join(cols) + f" | {qq} |")
    return "\n".join(L)


GEN = re.compile(r"<!--gen:([a-z0-9-]+)-->(.*?)<!--/gen:\1-->", re.S)


def fill(text, V):
    missing = sorted({m[1] for m in GEN.finditer(text) if m[1] not in V})
    if missing:
        sys.exit(f"site/blog.md: no value for {missing}")

    def sub(m):
        v = V[m[1]]
        if "\n" in v:
            return f"<!--gen:{m[1]}-->\n{v}\n<!--/gen:{m[1]}-->"
        return f"<!--gen:{m[1]}-->{v}<!--/gen:{m[1]}-->"
    return GEN.sub(sub, text)


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--report", help="a PARITY-REPORT-yah265-<date>.csv (default: the newest)")
    ap.add_argument("--check", action="store_true",
                    help="change nothing; exit 1 if the page or a chart is out of date")
    a = ap.parse_args()
    csv_path = os.path.abspath(a.report or newest_report())
    md_path = csv_path[:-4] + ".md"
    date = re.search(r"(\d{4}-\d\d-\d\d)", os.path.basename(csv_path))[1]
    rows = list(csv.DictReader(open(csv_path)))
    sp = speed_groups(rows)
    q = quality_groups(rows)
    lad = ladders(md_path)
    if not lad:
        sys.exit(f"{md_path}: no BD ladder lines found")
    hist = board_history()
    report_rel = os.path.relpath(md_path, ROOT)
    V = values(sp, q, lad, date, report_rel, hist, credits(md_path))
    charts = {"speed.svg": chart_speed(sp, date), "bdrate.svg": chart_bd(lad, date),
              "history.svg": chart_history(hist)}
    post = open(POST).read()
    new_post = fill(post, V)
    stale = []
    for name, svg in charts.items():
        p = os.path.join(OUT, name)
        old = open(p).read() if os.path.exists(p) else None
        if old != svg + "\n":
            stale.append(name)
            if not a.check:
                os.makedirs(OUT, exist_ok=True)
                with open(p, "w") as f:
                    f.write(svg + "\n")
    if new_post != post:
        stale.append("site/blog.md")
        if not a.check:
            with open(POST, "w") as f:
                f.write(new_post)
    print(f"report {report_rel}: {len(lad)} ladder cells, {len(sp)} speed groups, "
          f"{len(hist)} board points")
    if a.check:
        if stale:
            print("stale: " + ", ".join(stale))
            sys.exit(1)
        print("up to date")
    else:
        print("updated: " + (", ".join(stale) or "nothing changed"))


if __name__ == "__main__":
    main()
