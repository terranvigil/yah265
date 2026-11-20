#!/usr/bin/env python3
# Copyright (c) 2026, the yah265 authors
# SPDX-License-Identifier: GPL-2.0-or-later
"""Charts and tables for the threading and architectures pages, from data.

Reads the newest per-machine parity CSVs (docs/parity/), the x86 report
beside its CSV, the all-architecture summary, and the capped-tier run's
records (docs/data/cloud-tables-*/records/x86T.jsonl). Writes four charts
into site/assets/charts/internals/:

    machines.svg   times faster than x265 at G1, G2 and G3, per class and tier,
                   on the three machines
    simd.svg       what SIMD buys each encoder: all-core C wall over all-core
                   SIMD wall on the short clips, per class and machine
    capped.svg     both encoders held to one x86 instruction set, per class
    threads.svg    all cores against one thread in pure C on the Mac's short
                   clips, both encoders

and refills every `<!--gen:NAME-->...<!--/gen:NAME-->` span in
site/architectures.md and site/threading.md. It also checks that the
"Supported today" table on the architectures page has the same rows, in
the same order, as the summary report's coverage table.

    scripts/charts_internals.py            # newest reports
    scripts/charts_internals.py --check    # exit 1 if a page or a chart is stale
"""
import argparse
import csv
import glob
import json
import os
import re
import statistics as st
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import blog_charts as bc  # noqa: E402

ROOT = bc.ROOT
OUT = os.path.join(ROOT, "site/assets/charts/internals")
PAGES = [os.path.join(ROOT, "site/architectures.md"), os.path.join(ROOT, "site/threading.md")]
ARCH_PAGE = PAGES[0]
GEN = re.compile(r"<!--gen:([a-z0-9-]+)-->(.*?)<!--/gen:\1-->", re.S)

GOAL_LABEL = {"G1": "One thread, plain C", "G2": "All cores, plain C", "G3": "All cores, SIMD"}
PAIRS = [("sse42", "ours@sse42", "ref@sse42", "both at SSE4.2"),
         ("avx2", "ours@avx2", "ref@avx2", "both at AVX2"),
         ("avx512", "ours", "ref@avx2", "yah265 at AVX-512, x265 at AVX2")]
# how far two encodes' x265 byte counts may differ and still be the same
# span of the same clip (the one-thread x265 arm codes slightly other pictures)
SAME_SPAN = 0.05


# yah265 is drawn in its logo's orange-red and x265 in a neutral gray, so
# these charts keep their own colors and only borrow blog_charts' shapes.
# Slots s1-s3 passed the dataviz validator on both surfaces; the gray is
# below its chroma floor on purpose, and no chart sets it beside another gray.
STYLE = bc.STYLE


def head(W, H, title, desc):
    return [f"<svg xmlns='http://www.w3.org/2000/svg' width='{W}' height='{H}' "
            f"viewBox='0 0 {W} {H}' role='img'>",
            f"<title>{bc.esc(title)}</title><desc>{bc.esc(desc)}</desc>", STYLE,
            "<rect width='100%' height='100%' class='bg'/>"]


def faster(ratio):
    """Times faster, from a wall ratio rounded as the reports print it."""
    return 1 / round(ratio, 2)


# ------------------------------------------------------------------ data

def short_cells(rows):
    """title -> {goal: row} for the valid tier S rows that have all three
    goals over the same span."""
    by = {}
    for r in rows:
        if r["tier"] != "S" or r["valid"] != "yes" or r["goal"] not in bc.GOALS:
            continue
        if bc.fnum(r["ours_wall_s"]) is None or bc.fnum(r["ref_wall_s"]) is None:
            continue
        by.setdefault((r["class"], r["title"]), {})[r["goal"]] = r
    out = {}
    for k, g in by.items():
        if len(g) != 3:
            continue
        b = [float(g[x]["ref_bytes"]) for x in bc.GOALS]
        if max(b) / min(b) - 1 > SAME_SPAN:
            sys.exit(f"{k}: the three goals' x265 sizes differ by more than "
                     f"{SAME_SPAN:.0%}, so they aren't one span")
        out[k] = g
    return out


def wall_ratio(cells, num, den):
    """class -> (ours, x265, cells): the median over a class's short clips of
    the wall at goal `num` over the wall at goal `den`, per encoder."""
    acc = {}
    for (cls, _), g in cells.items():
        o = float(g[num]["ours_wall_s"]) / float(g[den]["ours_wall_s"])
        x = float(g[num]["ref_wall_s"]) / float(g[den]["ref_wall_s"])
        acc.setdefault(cls, []).append((o, x))
    return {c: (st.median(o for o, _ in v), st.median(x for _, x in v), len(v))
            for c, v in acc.items()}


def capped_run(path, report_md):
    """class -> {pair: (median ratio, slowest ratio, cells)} from the capped
    run's records: per cell and arm the median wall over its rounds. Checked
    against the table the x86 report prints."""
    walls = {}
    for line in open(path):
        r = json.loads(line)
        if r.get("goal") != "G3" or "wall_s" not in r or r.get("rc") != 0:
            continue
        walls.setdefault((r["cell"], r["arm"]), []).append(r["wall_s"])
    cells = sorted({c for c, _ in walls})
    acc = {}
    for cell in cells:
        cls = cell.split("_")[1]
        for key, a, b, _ in PAIRS:
            ratio = round(st.median(walls[(cell, a)]) / st.median(walls[(cell, b)]), 2)
            acc.setdefault(cls, {}).setdefault(key, []).append(ratio)
    out = {c: {k: (st.median(v), max(v), len(v)) for k, v in d.items()} for c, d in acc.items()}
    printed = bc.capped(report_md)
    for c, d in printed.items():
        for k in ("sse42", "avx2"):
            for mine, theirs in zip(out[c][k][:2], d[k]):
                if abs(mine - theirs) > 0.011:
                    sys.exit(f"capped run, {c} {k}: {mine:.2f} from the records, "
                             f"{theirs:.2f} in {os.path.basename(report_md)}")
    return out


def coverage_rows(md_path):
    """First column of the summary report's instruction-set coverage table."""
    rows, on = [], False
    for line in open(md_path):
        if line.startswith("## "):
            on = line.startswith("## Instruction-set coverage")
        elif on and line.startswith("| ") and not line.startswith(("| instruction set", "|---")):
            rows.append(line.strip().strip("|").split("|")[0].strip())
    if not rows:
        sys.exit(f"{md_path}: no instruction-set coverage table")
    return rows


def page_coverage_rows(text):
    m = re.search(r"^## Supported today\n(.*?)^## ", text, re.S | re.M)
    if not m:
        sys.exit("site/architectures.md: no 'Supported today' section")
    rows = []
    for line in m[1].splitlines():
        if line.startswith("| ") and not line.startswith(("| instruction set", "|---")):
            rows.append(line.strip().strip("|").split("|")[0].strip())
    return rows


# ------------------------------------------------------------------ charts

def chart_machines(sps):
    rows = bc.speed_rows(sps.values())
    for m, sp in sps.items():
        slow = [k for k, v in sp.items() if v[0] > 1.0]
        if slow:
            sys.exit(f"{m}: median over 1.00 in {slow}; the chart's title no longer holds")
    vals = [faster(v[0]) for sp in sps.values() for v in sp.values()]
    W, L, R, Tp, B = 900, 150, 24, 148, 62
    gap, rowh = 22, 34
    pw = (W - L - R - 2 * gap) / 3
    H = Tp + B + rowh * len(rows)
    x0, x1 = 1.0, max(2.0, int(max(vals) + 0.999))
    o = head(W, H, "yah265 is faster than x265 at all three goals on all three machines",
                "Median times faster than x265 medium at the same file size, per class and "
                "tier, for one thread in C, all cores in C and all cores with SIMD, on an "
                "Apple M5 Max, an x86-64 Xeon and a Graviton4.")
    o.append(bc.T(24, 40, "Faster than x265 at all three goals, on all three machines", 22,
                  weight="600"))
    o.append(bc.T(24, 64, "Median times faster than x265 medium at the same file size, per "
                          "class and clip length.", 14, "t2"))
    bc.legend(o, [(i, f"s{i + 1}", bc.MACHINE_LABEL[m]) for i, m in enumerate(bc.MACHINES)])
    for p, goal in enumerate(bc.GOALS):
        px = L + p * (pw + gap)
        X = lambda v: px + 10 + (v - x0) / (x1 - x0) * (pw - 20)
        o.append(bc.T(px + 10, Tp - 16, GOAL_LABEL[goal], 14, "t1", weight="600"))
        v = x0
        while v <= x1 + 1e-9:
            cls = "base" if v == 1 else "grid"
            dash = " stroke-dasharray='4 3'" if v == 1 else ""
            o.append(f"<line x1='{X(v):.1f}' y1='{Tp - 6}' x2='{X(v):.1f}' y2='{H - B}' "
                     f"class='{cls}'{dash}/>")
            o.append(bc.T(X(v), H - B + 18, f"{v:g}x", 12, "t2", "middle"))
            v += 1
        for i, (c, t) in enumerate(rows):
            y = Tp + rowh * (i + 0.5)
            got = {m: faster(sps[m][(c, t, goal)][0]) for m in bc.MACHINES
                   if (c, t, goal) in sps[m]}
            if len(got) > 1:
                o.append(f"<line x1='{X(min(got.values())):.1f}' y1='{y}' "
                         f"x2='{X(max(got.values())):.1f}' y2='{y}' class='span' "
                         f"stroke-width='2'/>")
            for j, m in enumerate(bc.MACHINES):
                if m not in got:
                    continue
                close = [k for k in bc.MACHINES if k in got and abs(got[k] - got[m]) < 0.12]
                dy = 0 if len(close) == 1 else (close.index(m) - (len(close) - 1) / 2) * 9
                o.append(bc.marker(j, X(got[m]), y + dy, f"s{j + 1}", 5))
    for i, (c, t) in enumerate(rows):
        y = Tp + rowh * (i + 0.5)
        o.append(bc.T(L - 8, y - 1, bc.RES[c], 13, "t1", "end"))
        o.append(bc.T(L - 8, y + 12, bc.TIER_LABEL[t], 11, "t2", "end"))
    o.append(bc.T((L + W - R) / 2, H - 14, "times faster than x265 (1x is level, right is faster)",
                  13, "t2", "middle"))
    o.append("</svg>")
    return "\n".join(o)


def paired_panel(o, px, pw, y0, title, data, xmax, rowh=46, barh=13):
    """One panel of paired bars per class: yah265 over x265."""
    L = 58
    X = lambda v: px + L + v / xmax * (pw - L - 52)
    classes = [c for c in bc.CLASSES if c in data]
    yb = y0 + rowh * len(classes)
    o.append(bc.T(px, y0 - 14, title, 14, "t1", weight="600"))
    v = 0
    while v <= xmax + 1e-9:
        cls = "base" if v == 0 else "grid"
        o.append(f"<line x1='{X(v):.1f}' y1='{y0 - 4}' x2='{X(v):.1f}' y2='{yb}' class='{cls}'/>")
        o.append(bc.T(X(v), yb + 16, f"{v:g}x", 12, "t2", "middle"))
        v += bc.nice_step(xmax, 4)
    for i, c in enumerate(classes):
        ours, ref, _ = data[c]
        ry = y0 + rowh * i + (rowh - 2 * barh - 2) / 2
        o.append(bc.T(px + L - 10, ry + barh + 5, bc.RES[c], 13, "t1", "end"))
        o.append(bc.bar(X(0), X(ours), ry, barh, "s1"))
        o.append(bc.bar(X(0), X(ref), ry + barh + 2, barh, "sx"))
        o.append(bc.T(X(ours) + 6, ry + barh - 2, f"{ours:.1f}x", 12, "t1"))
        o.append(bc.T(X(ref) + 6, ry + 2 * barh, f"{ref:.1f}x", 12, "t2"))
    return yb


def pair_legend(o, y=90):
    o.append(f"<rect x='24' y='{y - 10}' width='12' height='12' rx='2' class='s1'/>")
    o.append(bc.T(42, y + 1, "yah265", 13, "t2"))
    o.append(f"<rect x='110' y='{y - 10}' width='12' height='12' rx='2' class='sx'/>")
    o.append(bc.T(128, y + 1, "x265", 13, "t2"))


def chart_simd(gain):
    for m, d in gain.items():
        if any(o >= x for o, x, _ in d.values()):
            sys.exit(f"{m}: SIMD buys yah265 as much as x265 in some class; "
                     "the chart's title no longer holds")
    W, Tp = 900, 140
    pw = (W - 48 - 2 * 20) / 3
    xmax = int(max(x for d in gain.values() for _, x, _ in d.values()) + 0.999)
    H = Tp + 46 * 4 + 56
    o = head(W, H, "SIMD speeds x265 up more than it speeds yah265 up",
                "All-core wall time in plain C over all-core wall time with SIMD, median "
                "over each class's short clips, for yah265 and x265 on three machines.")
    o.append(bc.T(24, 40, "SIMD speeds x265 up more than it speeds yah265 up", 22, weight="600"))
    o.append(bc.T(24, 64, "All-core wall in plain C over all-core wall with SIMD. Median over "
                          "each class's short clips, same file size.", 14, "t2"))
    pair_legend(o)
    for p, m in enumerate(bc.MACHINES):
        yb = paired_panel(o, 24 + p * (pw + 20), pw, Tp, bc.MACHINE_LABEL[m], gain[m], xmax)
    o.append(bc.T(W / 2, yb + 42, "times faster with SIMD than without (longer is a bigger gain)",
                  13, "t2", "middle"))
    o.append("</svg>")
    return "\n".join(o)


def chart_threads(scale, cores):
    W, Tp = 720, 140
    top = max(max(o, x) for o, x, _ in scale.values())
    step = bc.nice_step(top, 4)
    xmax = step * int(top / step + 0.999)
    H = Tp + 46 * len(scale) + 56
    o = head(W, H, f"What {cores} cores buy each encoder in plain C",
                "One-thread wall time over all-core wall time in plain C, median over each "
                "class's short clips on an Apple M5 Max, for yah265 and x265.")
    o.append(bc.T(24, 40, f"What {cores} cores buy each encoder in plain C", 22, weight="600"))
    o.append(bc.T(24, 64, "One-thread wall over all-core wall, SIMD off. Median over each "
                          "class's short clips on the Mac.", 14, "t2"))
    pair_legend(o)
    yb = paired_panel(o, 24, W - 48, Tp, "Apple M5 Max, plain C", scale, xmax)
    o.append(bc.T(W / 2, yb + 42, "times faster on all cores than on one thread", 13, "t2",
                  "middle"))
    o.append("</svg>")
    return "\n".join(o)


def chart_capped(cap):
    worst = max(v[1] for d in cap.values() for v in d.values())
    if worst > 1.0:
        sys.exit("capped run: a cell is slower than x265; the chart's title no longer holds")
    classes = [c for c in bc.CLASSES if c in cap]
    vals = [faster(v[0]) for d in cap.values() for v in d.values()]
    W, L, R, Tp, B, rowh = 900, 110, 40, 116, 62, 44
    H = Tp + B + rowh * len(classes)
    x0, x1 = 0.75, max(2.0, int(max(vals) * 2 + 1.999) / 2)
    X = lambda v: L + (v - x0) / (x1 - x0) * (W - L - R)
    o = head(W, H, "yah265 stays ahead when both encoders are held to one instruction set",
                "Median times faster than x265 on the short clips with both encoders capped "
                "at SSE4.2, both at AVX2, and yah265 at AVX-512 against x265 at AVX2, per "
                "class on one Xeon.")
    o.append(bc.T(24, 40, "Ahead of x265 with both encoders held to one instruction set", 22,
                  weight="600"))
    o.append(bc.T(24, 64, "Median times faster than x265 medium on the short clips, SIMD on 16 "
                          "vCPUs of one Xeon 8488C.", 14, "t2"))
    bc.legend(o, [(i, f"s{i + 1}", lab) for i, (_, _, _, lab) in enumerate(PAIRS)])
    v = 1.0
    while v <= x1 + 1e-9:
        cls = "base" if v == 1 else "grid"
        dash = " stroke-dasharray='4 3'" if v == 1 else ""
        o.append(f"<line x1='{X(v):.1f}' y1='{Tp - 8}' x2='{X(v):.1f}' y2='{H - B}' "
                 f"class='{cls}'{dash}/>")
        o.append(bc.T(X(v), H - B + 20, "1x (x265)" if v == 1 else f"{v:g}x", 12, "t2", "middle"))
        v += 0.5
    o.append(bc.T((L + W - R) / 2, H - 14, "times faster than x265 (right is faster)", 13, "t2",
                  "middle"))
    for i, c in enumerate(classes):
        y = Tp + rowh * (i + 0.5)
        o.append(bc.T(L - 12, y + 4, bc.RES[c], 13, "t1", "end"))
        got = {k: faster(cap[c][k][0]) for k, *_ in PAIRS}
        o.append(f"<line x1='{X(min(got.values())):.1f}' y1='{y}' "
                 f"x2='{X(max(got.values())):.1f}' y2='{y}' class='span' stroke-width='2'/>")
        keys = [k for k, *_ in PAIRS]
        for j, k in enumerate(keys):
            close = [q for q in keys if abs(got[q] - got[k]) < 0.04]
            dy = 0 if len(close) == 1 else (close.index(k) - (len(close) - 1) / 2) * 10
            o.append(bc.marker(j, X(got[k]), y + dy, f"s{j + 1}"))
    o.append("</svg>")
    return "\n".join(o)


# ------------------------------------------------------------------ tables

def table_capped(cap):
    out = ["| class | both at SSE4.2 | both at AVX2 | yah265 at AVX-512, x265 at AVX2 | clips |",
           "|---|--:|--:|--:|--:|"]
    for c in bc.CLASSES:
        if c not in cap:
            continue
        cells = " | ".join(f"{faster(cap[c][k][0]):.2f}x / {faster(cap[c][k][1]):.2f}x"
                           for k, *_ in PAIRS)
        out.append(f"| {bc.RES[c]} | {cells} | {cap[c]['avx2'][2]} |")
    return "\n".join(out)


def table_simd(gain):
    out = ["| class | " + " | ".join(f"{bc.MACHINE_SHORT[m]}: yah265 / x265" for m in bc.MACHINES)
           + " | clips |", "|---|" + "--:|" * (len(bc.MACHINES) + 1)]
    for c in bc.CLASSES:
        cells = " | ".join(f"{gain[m][c][0]:.2f}x / {gain[m][c][1]:.2f}x" for m in bc.MACHINES)
        out.append(f"| {bc.RES[c]} | {cells} | {gain['mac'][c][2]} |")
    return "\n".join(out)


def table_threads(scale):
    out = ["| class | yah265 | x265 | clips |", "|---|--:|--:|--:|"]
    for c in bc.CLASSES:
        if c in scale:
            o, x, n = scale[c]
            out.append(f"| {bc.RES[c]} | {o:.1f}x | {x:.1f}x | {n} |")
    return "\n".join(out)


def fill(path, text, V):
    missing = sorted({m[1] for m in GEN.finditer(text) if m[1] not in V})
    if missing:
        sys.exit(f"{os.path.relpath(path, ROOT)}: no value for {missing}")

    def sub(m):
        v = V[m[1]]
        if "\n" in v:
            return f"<!--gen:{m[1]}-->\n{v}\n<!--/gen:{m[1]}-->"
        return f"<!--gen:{m[1]}-->{v}<!--/gen:{m[1]}-->"
    return GEN.sub(sub, text)


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--check", action="store_true",
                    help="change nothing; exit 1 if a page or a chart is out of date")
    a = ap.parse_args()
    D = r"\d{4}-\d\d-\d\d"
    paths = {"mac": bc.newest(rf"PARITY-REPORT-yah265-{D}\.csv"),
             "x86": bc.newest(rf"PARITY-REPORT-yah265-x86-{D}\.csv"),
             "g4": bc.newest(rf"PARITY-REPORT-yah265-graviton-{D}\.csv")}
    summary = bc.newest(rf"PARITY-REPORT-yah265-all-isa-{D}\.md")
    records = sorted(glob.glob(os.path.join(ROOT, "docs/data/cloud-tables-*/records/x86T.jsonl")))
    if not records:
        sys.exit("no capped-tier records (docs/data/cloud-tables-*/records/x86T.jsonl)")
    rows = {m: list(csv.DictReader(open(p))) for m, p in paths.items()}
    sps = {m: bc.speed_groups(rs) for m, rs in rows.items()}
    cells = {m: short_cells(rs) for m, rs in rows.items()}
    gain = {m: wall_ratio(c, "G2", "G3") for m, c in cells.items()}
    scale = wall_ratio(cells["mac"], "G1", "G2")
    cap = capped_run(records[-1], paths["x86"][:-4] + ".md")
    cores = re.search(r"^- Mac: .*?(\d+) cores", open(summary).read(), re.M)
    if not cores:
        sys.exit(f"{summary}: no Mac core count")

    charts = {"machines.svg": chart_machines(sps),
              "simd.svg": chart_simd(gain),
              "capped.svg": chart_capped(cap),
              "threads.svg": chart_threads(scale, cores[1])}
    V = {"capped-table": table_capped(cap),
         "simd-table": table_simd(gain),
         "threads-table": table_threads(scale)}

    stale = []
    want = coverage_rows(summary)
    have = page_coverage_rows(open(ARCH_PAGE).read())
    if want != have:
        print("site/architectures.md: 'Supported today' rows differ from the report's "
              f"coverage table\n  report: {want}\n  page:   {have}", file=sys.stderr)
        stale.append("site/architectures.md (coverage rows)")
    os.makedirs(OUT, exist_ok=True)
    for name, svg in charts.items():
        p = os.path.join(OUT, name)
        old = open(p).read() if os.path.exists(p) else None
        if old != svg + "\n":
            stale.append(os.path.relpath(p, ROOT))
            if not a.check:
                open(p, "w").write(svg + "\n")
    for page in PAGES:
        text = open(page).read()
        new = fill(page, text, V)
        if new != text:
            stale.append(os.path.relpath(page, ROOT))
            if not a.check:
                open(page, "w").write(new)
    if a.check:
        if stale:
            print("stale: " + ", ".join(stale), file=sys.stderr)
            return 1
        print("charts_internals: pages and charts are current")
        return 0
    if any("coverage" in s for s in stale):
        return 1
    print("wrote " + (", ".join(s for s in stale) or "nothing (all current)"))
    return 0


if __name__ == "__main__":
    sys.exit(main())
