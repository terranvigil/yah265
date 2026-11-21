#!/usr/bin/env python3
# Copyright (c) 2026, the yah265 authors
# SPDX-License-Identifier: GPL-2.0-or-later
"""The launch post's charts and numbers, from the data files.

Reads the newest parity reports for each machine
(docs/parity/PARITY-REPORT-yah265-<date>.csv for the Mac, -x86-<date> and
-graviton-<date> for the cloud boxes, with the .md beside each), the
all-architecture summary (PARITY-REPORT-yah265-all-isa-<date>.md), the
rate ladders in docs/data/parity-spec-ladders.json, the content-type rows
of docs/data/parity-spec.csv, the ffmpeg runs in
docs/data/blog-ffmpeg-examples.json and docs/data/parity-history.csv.
Writes six charts and the social card into site/assets/blog/:

    films.svg     wall time per ten-minute film at 1080p and 4K, both encoders
    cells.svg     every timed cell behind the headline, as times faster than x265
    arch.svg      times faster than x265 at G3 on each machine, per class and tier
    curves.svg    VMAF v1 against bitrate for three titles, both encoders
    bdrate.svg    BD-rate on VMAF v1 for every ladder, CRF and VBR
    history.svg   the timed board's medians, commit by commit
    card.png      the 1200x627 link card (drawn with Pillow; --check reads
                  the figures stamped into it and needs no Pillow)

and refills every `<!--gen:NAME-->...<!--/gen:NAME-->` span in
site/blog.md, so no figure on the page is typed by hand.

    scripts/blog_charts.py            # newest reports
    scripts/blog_charts.py --check    # exit 1 if the page or a chart is stale
"""
import argparse
import csv
import glob
import json
import math
import os
import re
import statistics as st
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
POST = os.path.join(ROOT, "site/blog.md")
OUT = os.path.join(ROOT, "site/assets/blog")
HISTORY = os.path.join(ROOT, "docs/data/parity-history.csv")
LADDERS = os.path.join(ROOT, "docs/data/parity-spec-ladders.json")
FFMPEG = os.path.join(ROOT, "docs/data/blog-ffmpeg-examples.json")
REPRO = os.path.join(ROOT, "docs/data/cloud-tables-2026-11-13/repro_x86.json")
PARITY = os.path.join(ROOT, "docs/parity")
SPEC = os.path.join(ROOT, "docs/data/parity-spec.csv")
LOGO = os.path.join(ROOT, "assets/yah265-logo-light.png")
CARD = "card.png"

CLASSES = ["CIF", "720p", "1080p", "2160p"]
TIERS = ["S", "M", "L"]
GOALS = ["G1", "G2", "G3"]
MACHINES = ["mac", "x86", "g4"]
MACHINE_LABEL = {"mac": "Mac, NEON", "x86": "x86-64, AVX-512", "g4": "Graviton4, SVE2"}
MACHINE_LONG = {"mac": "Apple M5 Max (NEON)", "x86": "Xeon 8488C (AVX-512)",
                "g4": "Graviton4 (SVE2)"}
MACHINE_SHORT = {"mac": "Mac", "x86": "Xeon", "g4": "Graviton4"}
TIER_LABEL = {"S": "short clips", "M": "2-min segments", "L": "10-min films"}
RES = {"CIF": "CIF", "720p": "720p", "1080p": "1080p", "2160p": "4K"}
TITLE = {"chimera": "Chimera", "sollevante": "Sol Levante",
         "meridian": "Meridian", "tears": "Tears of Steel",
         "bbb": "Big Buck Bunny", "sintel": "Sintel",
         "cosmos": "Cosmos Laundromat", "stem2": "StEM2",
         "nocturne": "Nocturne", "lash1924": "Lash of the Whip",
         "worldcup_far": "World Cup, far camera", "worldcup": "World Cup",
         "skater227": "Skater", "ducks": "Ducks take off",
         "crowd_run": "Crowd run", "park_joy": "Park joy"}
GOAL_ORDER = ["G3", "G1", "G2"]
GOAL_LONG = {"G3": "G3: as shipped, SIMD on all cores",
             "G1": "G1: algorithm only, plain C on one thread",
             "G2": "G2: algorithm only, plain C on all cores"}
BD_BAR = 1.0

# Titles held out of every figure, chart and table on the site. Every
# generator reads its rows through report_rows() or asks shown(), so this
# list is the only place a title goes in or comes out.
EXCLUDED = ("meridian", "chimera")

# the binaries whose ladders count: main's release build and brew's x265.
# The script checks each curve's BD against the report before it draws it.
OURS_BIN = "3ee7824ee8159f24"
REF_BIN = "2ffa132bb74bb3a5"

# board commits worth a label on the history chart: (commit, text)
EVENTS = [("8061928", "threads and NEON kernels"),
          ("358db2f", "medium's effort trimmed"),
          ("4e8fe2c", "medium becomes the goal preset")]


# ------------------------------------------------------------------ data

def newest(pattern):
    rs = sorted(p for p in glob.glob(os.path.join(PARITY, "PARITY-REPORT-yah265-*"))
                if re.fullmatch(pattern, os.path.basename(p)))
    if not rs:
        sys.exit(f"no report matching {pattern} under docs/parity/")
    return rs[-1]


def fnum(s):
    try:
        return float(s)
    except (TypeError, ValueError):
        return None


def base_title(title):
    return re.sub(r"_(cif|\d+p)$", "", title)


def shown(title):
    """False for a title on the exclusion list, at any picture size."""
    return base_title(title) not in EXCLUDED


def report_rows(path, everything=False):
    """A parity CSV's rows without the excluded titles. everything=True
    keeps them, for checking this script's medians against a report's."""
    return [r for r in csv.DictReader(open(path)) if everything or shown(r["title"])]


def speed_groups(rows, goals=GOALS, ref="ref_wall_s"):
    """(class, tier, goal) -> (median, worst, worst title) over valid rows,
    the same rule the reports use. ref names the x265 wall column, for the
    cloud rows that timed a second x265 arm."""
    acc = {}
    for r in rows:
        v = fnum(r["ratio" if ref == "ref_wall_s" else "ratio_ref512"])
        if r["valid"] != "yes" or v is None or "FAIL:" in r["note"] or r["tier"] not in TIERS \
                or r["goal"] not in goals:
            continue
        # the cloud reports take the median before rounding and the Mac's
        # takes it over the two-place column, so each is rebuilt its own way
        u = v
        if r.get("arch"):
            u = fnum(r["ours_wall_s"]) / fnum(r[ref])
        acc.setdefault((r["class"], r["tier"], r["goal"]), []).append((v, r["title"], u))
    return {k: (st.median(u for *_, u in vs), max(vs)[0], max(vs)[1])
            for k, vs in acc.items()}


def quality_groups(rows):
    """(class, tier) -> median dVMAF v1 at matched size, from the G3 rows."""
    acc = {}
    for r in rows:
        v = fnum(r["dvmafneg"])
        if r["goal"] == "G3" and v is not None and r["tier"] in TIERS:
            acc.setdefault((r["class"], r["tier"]), []).append(v)
    return {k: st.median(v) for k, v in acc.items()}


def films(rows):
    """class -> [(title, ours s, x265 s, ratio, dVMAF v1) or (title, None...)]
    for the Mac's tier L G3 rows."""
    out = {}
    for r in rows:
        if r["tier"] != "L" or r["goal"] != "G3":
            continue
        ok = r["valid"] == "yes" and fnum(r["ratio"]) is not None
        out.setdefault(r["class"], []).append(
            (r["title"], fnum(r["ours_wall_s"]) if ok else None,
             fnum(r["ref_wall_s"]) if ok else None, fnum(r["ratio"]) if ok else None,
             fnum(r["dvmafneg"])))
    return out


def vbr_walls(md_path):
    """[(cell, tier, ratio)] from the report's "VBR wall" table: one capped
    VBR rung per real-title cell, both encoders at one target rate."""
    out, on = [], False
    for line in open(md_path):
        if line.startswith("## "):
            on = line.startswith("## VBR wall")
        elif on and line.startswith("| ") and not line.startswith("| cell"):
            c = [x.strip() for x in line.strip().strip("|").split("|")]
            if fnum(c[3]) is not None and shown(c[0].rsplit("_", 1)[0]):
                out.append((c[0], c[1], float(c[3])))
    if not out:
        sys.exit(f"{md_path}: no VBR wall table found")
    return out


def content_rows():
    """The newest valid G3 row of each content-type (tier C) clip."""
    last = {}
    for r in csv.DictReader(open(SPEC)):
        if r["tier"] == "C" and r["goal"] == "G3" and r["valid"] == "yes" \
                and fnum(r["ratio"]) is not None:
            k = (r["class"], r["title"])
            if k not in last or r["date"] >= last[k]["date"]:
                last[k] = r
    return list(last.values())


def clip_name(title, cls, tier=None):
    base = re.sub(r"_(cif|\d+p)$", "", title)
    name = TITLE.get(base, base.replace("_", " ").capitalize())
    dur = {"M": ", 2 min", "L": ", 10 min"}.get(tier, "")
    return f"{name} {RES.get(cls, cls)}{dur}"


def vbr_groups(mac_md, cloud):
    """(class, tier) -> {machine: (median, worst)} and the cell count, for
    the capped VBR walls: the Mac's from its report's table, the cloud
    machines' from their CSV rows."""
    acc = {}
    for cell, tier, v in vbr_walls(mac_md):
        acc.setdefault((cell.rsplit("_", 1)[1], tier), []).append(v)
    out = {k: {"mac": (st.median(v), max(v))} for k, v in acc.items()}
    n = {k: len(v) for k, v in acc.items()}
    for m, rows in cloud.items():
        for (c, t, _), v in speed_groups(rows, ("VBR",)).items():
            out.setdefault((c, t), {})[m] = v[:2]
    return out, n


def record_walls(path):
    """(cell, arm) -> the G3 wall of every round, from a cloud run's records."""
    walls = {}
    for line in open(path):
        r = json.loads(line)
        if r.get("goal") == "G3" and "wall_s" in r and r.get("rc") == 0:
            walls.setdefault((r["cell"], r["arm"]), []).append((r["wall_s"], r.get("bytes")))
    return walls


def record_title(cell):
    """'S_2160p_bbb_2160p' -> ('2160p', 'bbb_2160p')."""
    _, cls, title = cell.split("_", 2)
    return cls, title


def graviton3(path, size_bar=0.01):
    """class -> (median, slowest, clips) at G3 on the short clips, from the
    Graviton3 run's records: per clip, the median wall over its rounds. A
    clip whose two files differ by more than the size bar is left out, as
    in the Graviton report."""
    walls = record_walls(path)
    acc = {}
    for cell in sorted({c for c, _ in walls}):
        cls, title = record_title(cell)
        ours, ref = walls[(cell, "ours")], walls[(cell, "ref")]
        if not shown(title) or abs(ours[0][1] / ref[0][1] - 1) > size_bar:
            continue
        acc.setdefault(cls, []).append(
            round(st.median(w for w, _ in ours) / st.median(w for w, _ in ref), 2))
    return {c: (st.median(v), max(v), len(v)) for c, v in acc.items()}


CAPPED_PAIRS = [("sse42", "ours@sse42", "ref@sse42", "both at SSE4.2"),
                ("avx2", "ours@avx2", "ref@avx2", "both at AVX2")]


def capped_records():
    rs = sorted(glob.glob(os.path.join(ROOT, "docs/data/cloud-tables-*/records/x86T.jsonl")))
    if not rs:
        sys.exit("no capped-tier records (docs/data/cloud-tables-*/records/x86T.jsonl)")
    return rs[-1]


def capped_run(path, report_md):
    """class -> {pair: (median ratio, slowest ratio, cells)} from the capped
    run's records: per cell and arm the median wall over its rounds. The
    read is first checked, every title in, against the table the x86 report
    prints."""
    walls = record_walls(path)
    out = {}
    for everything in (True, False):
        acc = {}
        for cell in sorted({c for c, _ in walls}):
            cls, title = record_title(cell)
            if not everything and not shown(title):
                continue
            for key, a, b, _ in CAPPED_PAIRS:
                ratio = round(st.median(w for w, _ in walls[(cell, a)])
                              / st.median(w for w, _ in walls[(cell, b)]), 2)
                acc.setdefault(cls, {}).setdefault(key, []).append(ratio)
        out = {c: {k: (st.median(v), max(v), len(v)) for k, v in d.items()}
               for c, d in acc.items()}
        if everything:
            for c, d in capped_printed(report_md).items():
                for k in ("sse42", "avx2"):
                    for mine, theirs in zip(out[c][k][:2], d[k]):
                        if abs(mine - theirs) > 0.011:
                            sys.exit(f"capped run, {c} {k}: {mine:.2f} from the records, "
                                     f"{theirs:.2f} in {os.path.basename(report_md)}")
    return out


def headline_cells(mac_rows):
    """The site's headline, and the only place it is computed. It covers the
    size-matched cells timed on the Mac as shipped: the valid G3 rows of
    tiers S, M and L, where yah265's CRF is solved onto x265's file size
    within 1% and both encoders run at medium with SIMD on all cores. The
    capped VBR walls share a target rate and no size, and the content-type
    clips sit outside the speed verdict, so neither is in it. Speed and the
    VMAF v1 lead are medians over the same cells. Returns the cells per
    class as [(ratio, name)] with the two medians."""
    cells = [r for r in mac_rows if r["goal"] == "G3" and r["valid"] == "yes"
             and fnum(r["ratio"]) is not None and "FAIL:" not in r["note"]
             and r["tier"] in TIERS and shown(r["title"])]
    if any(fnum(r["dvmafneg"]) is None or abs(float(r["dsize_pct"])) > 1.0 for r in cells):
        sys.exit("a headline cell has no VMAF v1 score or isn't size-matched within 1%")
    pools = {c: [(float(r["ratio"]), clip_name(r["title"], r["class"], r["tier"]))
                 for r in cells if r["class"] == c] for c in CLASSES}
    allr = [float(r["ratio"]) for r in cells]
    dv = [float(r["dvmafneg"]) for r in cells]
    return {"pools": pools, "n": len(allr), "median": st.median(allr),
            "faster": sum(1 for v in allr if v < 1), "dv": st.median(dv), "dv_n": len(dv),
            "dv_ahead": sum(1 for v in dv if v > 0)}


def times(ratio):
    """Times faster from an unrounded ratio, for the pooled medians."""
    return f"{1 / ratio:.2f}x"


def headline_times(pl):
    """The headline speed, to one decimal: run-to-run spread is wider
    than the second one, and one decimal reads at a glance."""
    return f"{1 / pl['median']:.1f}x"


BD_LINE = re.compile(r"^- (\S+) \(([A-Z])\): CRF BD-VMAF v1 ([-+]\d+\.\d+)%.*?"
                     r"VBR BD-VMAF v1 ([-+]\d+\.\d+)%")


def ladders(md_path):
    """[(title, size, tier, crf, vbr)] from the report's BD section."""
    out = []
    for line in open(md_path):
        m = BD_LINE.match(line)
        if m:
            title, size = m[1].rsplit("_", 1)
            if shown(title):
                out.append((title, size, m[2], float(m[3]), float(m[4])))
    return out


def credits(md_path):
    """(titles behind on their end credits, titles read, titles pending)."""
    rows, on = [], False
    for line in open(md_path):
        if line.startswith("## "):
            on = line.startswith("## End credits")
        elif on and line.startswith("| ") and not line.startswith("| title") \
                and shown(line.split("|")[1].strip()):
            rows.append(line.rstrip().rstrip("|").split("|")[-1].strip())
    read = [r for r in rows if r]
    return (sum(1 for r in read if r.startswith("**behind")), len(read),
            len(rows) - len(read))


def capped_printed(md_path):
    """class -> {'sse42': (median, worst), 'avx2': (median, worst)} from the
    x86 report's table of both encoders held to one instruction set."""
    out, on = {}, False
    for line in open(md_path):
        if line.startswith("## "):
            on = "held to SSE4.2 and to AVX2" in line
        elif on and line.startswith("| ") and not line.startswith("| class"):
            c = [x.strip() for x in line.strip().strip("|").split("|")]
            if c[0] in CLASSES:
                pair = lambda s: tuple(float(x) for x in s.split("/"))
                out[c[0]] = {"sse42": pair(c[1]), "avx2": pair(c[2])}
    if not out:
        sys.exit(f"{md_path}: no capped SSE4.2/AVX2 table found")
    return out


def _polyfit3(xs, ys):
    """Least-squares cubic, coefficients highest power first."""
    n = 4
    A = [[sum(x ** (i + j) for x in xs) for j in range(n)] for i in range(n)]
    b = [sum(y * x ** i for x, y in zip(xs, ys)) for i in range(n)]
    for c in range(n):
        p = max(range(c, n), key=lambda r: abs(A[r][c]))
        A[c], A[p], b[c], b[p] = A[p], A[c], b[p], b[c]
        for r in range(c + 1, n):
            f = A[r][c] / A[c][c]
            A[r] = [a - f * q for a, q in zip(A[r], A[c])]
            b[r] -= f * b[c]
    x = [0.0] * n
    for r in reversed(range(n)):
        x[r] = (b[r] - sum(A[r][k] * x[k] for k in range(r + 1, n))) / A[r][r]
    return x[::-1]


def bd_rate(ref, ours):
    """Bjontegaard rate delta of ours against ref over [(kbps, score)]: the
    cubic fit of log rate on score, averaged over the shared score range."""
    f = [_polyfit3([s for _, s in pts], [math.log(k) for k, _ in pts]) for pts in (ref, ours)]
    lo = max(min(s for _, s in ref), min(s for _, s in ours))
    hi = min(max(s for _, s in ref), max(s for _, s in ours))
    ev = lambda c, x: ((c[0] * x + c[1]) * x + c[2]) * x + c[3]
    xs = [lo + (hi - lo) * i / 99 for i in range(100)]
    d = sum(ev(f[1], x) - ev(f[0], x) for x in xs) / len(xs)
    return (math.exp(d) - 1) * 100


def curve_picks(lad):
    """The three ladders the post draws, by rule: the largest CRF saving,
    the median one and the smallest, as the ladder file keys them."""
    by = sorted(lad, key=lambda r: r[3])
    return [f"{tier}/{size}/{title}" for title, size, tier, *_ in
            (by[0], by[len(by) // 2], by[-1])]


def curves(lad):
    """cell -> (ours [(kbps, v1)], x265 [(kbps, v1)], model) at CRF, with
    the BD checked against the report's figure for that cell."""
    data = json.load(open(LADDERS))
    want = {(f"{t}_{s}", tier): crf for t, s, tier, crf, _ in lad}
    out = {}
    for cell in curve_picks(lad):
        pts = {"ours": [], "ref": []}
        model = None
        for k, r in data.items():
            c, mode, arm, q, la, b = k.split("|")
            if c != cell or mode != "crf" or la != "la20":
                continue
            if (arm, b) in (("ours", OURS_BIN), ("ref", REF_BIN)):
                g = r["v1_gate"]
                pts[arm].append((r["kbps"], r[f"v1_{g}"]))
                model = g
        if len(pts["ours"]) != 5 or len(pts["ref"]) != 5:
            sys.exit(f"{LADDERS}: {cell} needs five CRF rungs per arm")
        tier, cls, title = cell.split("/")
        bd = bd_rate(pts["ref"], pts["ours"])
        rep = want.get((f"{title}_{cls}", tier))
        if rep is None or abs(bd - rep) > 0.01:
            sys.exit(f"{cell}: BD {bd:+.2f}% from the ladder file, report says {rep}")
        out[cell] = (sorted(pts["ours"]), sorted(pts["ref"]), model, rep)
    return out


def board_history():
    """One point per timed board on x265 4.3: the median over the corpus of
    our wall over x265's at G1 and at G3. Partial boards are left out."""
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


# ------------------------------------------------------- chart data files

SOURCE_LABEL = [("PARITY-REPORT-yah265-x86-", "the Xeon parity report"),
                ("PARITY-REPORT-yah265-graviton-", "the Graviton4 parity report"),
                ("PARITY-REPORT-yah265-all-isa-", "the all-machines parity report"),
                ("PARITY-REPORT-yah265-", "the Mac parity report"),
                ("parity-spec-ladders.json", "the recorded rate ladders"),
                ("parity-spec-points.json", "the recorded matched points"),
                ("parity-history.csv", "the timed board's history"),
                ("hdr-results.json", "the HDR results"),
                ("x86T.jsonl", "the capped run's records"),
                ("tears-perframe.csv", "the per-frame scores of the Tears of Steel clip")]


def source(path):
    """(repository path, the name a caption shows) for a data file."""
    rel = os.path.relpath(path, ROOT) if os.path.isabs(path) else path
    base = os.path.basename(rel)
    for key, label in SOURCE_LABEL:
        if base.startswith(key) or base == key:
            return rel, label + (" (rows)" if rel.endswith(".csv") and "PARITY" in base else "")
    sys.exit(f"no caption name for the data file {rel}")


def cell_text(v):
    if isinstance(v, float):
        return f"{v:.4f}".rstrip("0").rstrip(".")
    return "" if v is None else str(v)


def speed_csv(sps, goals=GOALS):
    """Rows of the speed groups: machine, class, tier, goal, then the median
    and the slowest clip as times faster than x265."""
    head = ["machine", "class", "tier", "goal", "median_times_faster",
            "slowest_times_faster", "slowest_clip"]
    rows = []
    for m in MACHINES:
        for (c, t, g), (med, worst, title) in sorted(
                sps.get(m, {}).items(),
                key=lambda kv: (CLASSES.index(kv[0][0]), TIERS.index(kv[0][1]), kv[0][2])):
            if g in goals:
                rows.append([MACHINE_SHORT[m], c, t, g, 1 / med, 1 / worst, title])
    return head, rows


def publish_data(out_dir, script, entries, check):
    """Writes each chart's numbers as name.csv beside name.svg, and
    provenance.json with each chart's sources and script. The site builder
    turns that into the line under the chart. entries is
    {chart: ([data file, ...], header, rows)}. Returns what was stale."""
    stale, prov = [], {}
    for name, (sources, header, rows) in entries.items():
        lines = [",".join(header)]
        for r in rows:
            cells = [cell_text(v) for v in r]
            if len(cells) != len(header):
                sys.exit(f"{name}.csv: a row has {len(cells)} cells, the header {len(header)}")
            for c in cells:
                if any(x in c.lower() for x in EXCLUDED):
                    sys.exit(f"{name}.csv: a row names an excluded title ({c})")
            lines.append(",".join(f'"{c}"' if "," in c else c for c in cells))
        text = "\n".join(lines) + "\n"
        p = os.path.join(out_dir, name + ".csv")
        if (open(p).read() if os.path.exists(p) else None) != text:
            stale.append(name + ".csv")
            if not check:
                os.makedirs(out_dir, exist_ok=True)
                open(p, "w").write(text)
        prov[name] = {"script": "scripts/" + script, "csv": name + ".csv",
                      "sources": [list(source(x)) for x in sources]}
    p = os.path.join(out_dir, "provenance.json")
    text = json.dumps(prov, indent=1, sort_keys=True) + "\n"
    if (open(p).read() if os.path.exists(p) else None) != text:
        stale.append("provenance.json")
        if not check:
            open(p, "w").write(text)
    return stale


# ------------------------------------------------------------------ charts

# yah265 is the logo's orange-red (.s1) and x265, the comparator, a neutral
# gray (.sx). Where every series is yah265's own (machines, rate modes,
# goals) slots 2 and 3 are aqua and violet. Each set passes the dataviz
# validator's CVD and normal-vision checks on both surfaces, and every
# series also has its own marker shape. Text stays in the ink colors.
BRAND = "#ff4d1a"
NEUTRAL = "#868e96"
STYLE = """<style>
.bg{fill:#ffffff}.t1{fill:#212529}.t2{fill:#495057}.grid{stroke:#e9ecef}
.base{stroke:#868e96}.ring{stroke:#ffffff}.med{stroke:#212529}.band{fill:#f1f3f5}
.s1{fill:#ff4d1a}.s2{fill:#00919a}.s3{fill:#6250d6}.sx{fill:#868e96}
.l1{stroke:#ff4d1a}.l2{stroke:#00919a}.l3{stroke:#6250d6}.lx{stroke:#868e96}.span{stroke:#dee2e6}
.note{fill:#b8330a}.wash{fill:#ffe8dc}
text{font-family:system-ui,-apple-system,"Segoe UI",Roboto,Helvetica,Arial,sans-serif}
@media (prefers-color-scheme:dark){
.bg{fill:#1b1c1e}.t1{fill:#e9ecee}.t2{fill:#b6bcc1}.grid{stroke:#2e3135}
.base{stroke:#8f959a}.ring{stroke:#1b1c1e}.med{stroke:#e9ecee}.band{fill:#23262a}
.s1{fill:#ff4d1a}.s2{fill:#12a3ad}.s3{fill:#9085e9}.sx{fill:#8f959a}
.l1{stroke:#ff4d1a}.l2{stroke:#12a3ad}.l3{stroke:#9085e9}.lx{stroke:#8f959a}.span{stroke:#3a3e43}
.note{fill:#ff8a5c}.wash{fill:#45200f}}
</style>"""


# Every chart is drawn twice: on a 900-unit canvas, and on a 480-unit one
# for phones (name-narrow.svg), where panels stack and the title wraps.
# The chart functions read the width from here.
NARROW = False
NARROW_W = 480


def width(wide=900):
    return NARROW_W if NARROW else wide


def pick(wide, narrow):
    return narrow if NARROW else wide


def wrap(s, size, maxw, bold=False):
    """Greedy line breaks for text of one size, by an average glyph width."""
    per = size * (0.57 if bold else 0.52)
    lines, cur = [], ""
    for w in str(s).split(" "):
        if cur and (len(cur) + 1 + len(w)) * per > maxw:
            lines.append(cur)
            cur = w
        else:
            cur = f"{cur} {w}".strip()
    return lines + [cur] if cur else lines


def note(o, x, y, s, size=12, cls="t2", maxw=None, anchor="start", lh=1.35):
    """A caption that wraps on the narrow canvas. Returns the height used."""
    lines = wrap(s, size, maxw) if (maxw and NARROW) else [s]
    for i, ln in enumerate(lines):
        o.append(T(x, y + i * size * lh, ln, size, cls, anchor))
    return (len(lines) - 1) * size * lh


_HEAD = re.compile(r"<text x='24\.0' y='(\d+)\.0' font-size='([\d.]+)' class='(t1|t2)' "
                   r"text-anchor='start'( font-weight='600')?>(.*?)</text>\n?")


def fit_narrow(svg):
    """Restack a narrow chart's heading: the title and the line under it
    wrap to the canvas, and everything below moves down by what they took."""
    W = NARROW_W
    found = {}
    for m in _HEAD.finditer(svg):
        y, size = int(m[1]), float(m[2])
        if "title" not in found and size >= 21 and y <= 40:
            found["title"] = m
        elif "sub" not in found and size == 14 and y in (62, 64) and m[3] == "t2":
            found["sub"] = m
    if "title" not in found:
        sys.exit("fit_narrow: a chart has no title line")
    out, y = [], 30
    for ln in wrap(found["title"][5], 18, W - 40, True):
        out.append(f"<text x='20' y='{y}' font-size='18' class='t1' font-weight='600'>{ln}</text>")
        y += 23
    y -= 3
    if "sub" in found:
        for ln in wrap(found["sub"][5], 12.5, W - 40):
            out.append(f"<text x='20' y='{y}' font-size='12.5' class='t2'>{ln}</text>")
            y += 17
    dy = max(0, y - 17 - 64)
    for m in found.values():
        svg = svg.replace(m[0], "", 1)
    mark = "class='bg'/>"
    i = svg.index(mark) + len(mark)
    body = svg[i:]
    j = body.rindex("</svg>")
    svg = (svg[:i] + "\n" + "\n".join(out) + f"\n<g transform='translate(0 {dy})'>"
           + body[:j] + "</g>\n</svg>")
    h = re.search(r"viewBox='0 0 (\d+) ([\d.]+)'", svg)
    H = float(h[2]) + dy
    svg = svg.replace(h[0], f"viewBox='0 0 {h[1]} {H:g}'", 1)
    return re.sub(r" width='\d+' height='[\d.]+' viewBox", f" width='{h[1]}' height='{H:g}' viewBox",
                  svg, count=1)


def render(charts):
    """{name.svg: function} -> {name.svg: wide svg, name-narrow.svg: narrow}."""
    global NARROW
    out = {}
    for name, fn in charts.items():
        NARROW = False
        out[name] = fn()
        NARROW = True
        try:
            out[name.replace(".svg", "") + "-narrow" + (".svg" if name.endswith(".svg") else "")] \
                = fit_narrow(fn())
        finally:
            NARROW = False
    return out


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
    size, per, gap = pick((13, 7.0, gap), (11.5, 5.9, 12))
    for shape, cls, label in items:
        o.append(marker(shape, x + 6, y - 4, cls, 5.5))
        o.append(T(x + 18, y + 1, label, size, "t2"))
        x += 18 + len(label) * per + gap


def bar(x0, x1, y, h, cls):
    """A horizontal bar from the baseline x0, square there, 4px round at the tip."""
    r = min(4, h / 2, max(0, x1 - x0))
    return (f"<path d='M{x0:.1f},{y:.1f} H{x1 - r:.1f} Q{x1:.1f},{y:.1f} {x1:.1f},{y + r:.1f} "
            f"V{y + h - r:.1f} Q{x1:.1f},{y + h:.1f} {x1 - r:.1f},{y + h:.1f} H{x0:.1f} Z' "
            f"class='{cls}'/>")


def nice_step(top, n=5):
    raw = top / n
    mag = 10 ** math.floor(math.log10(raw))
    for m in (1, 2, 2.5, 5, 10):
        if raw <= m * mag:
            return m * mag
    return 10 * mag


def fmin(s):
    return f"{s / 60:.1f} min"


WORDS = ["zero", "one", "two", "three", "four", "five", "six", "seven", "eight", "nine", "ten"]


def word(n):
    return WORDS[n] if n < len(WORDS) else str(n)


def chart_films(fl):
    """Paired bars: minutes per film for both encoders, one panel per class."""
    panels = [c for c in ("1080p", "2160p") if c in fl]
    order = [t for t, *_ in sorted(fl["2160p"], key=lambda r: -(r[2] or 0))]
    order += sorted({t for c in panels for t, *_ in fl[c]} - set(order))
    W = width()
    L, R = pick((180, 210), (138, 136))
    rowh, barh, gap = 40, 12, 2
    Tp = 112
    # a panel lists the films it timed
    timed = {c: [t for t in order if any(r[0] == t and r[1] for r in fl[c])] for c in panels}
    panel_h = {c: 34 + rowh * len(timed[c]) + 34 for c in panels}
    H = Tp + sum(panel_h.values()) + 16 * (len(panels) - 1) + 14
    tot = {c: (sum(r[1] for r in fl[c] if r[1]), sum(r[2] for r in fl[c] if r[2]))
           for c in panels}
    title = (f"x265 took {tot['2160p'][1] / 60:.0f} minutes for {word(len(timed['2160p']))} 4K "
             f"films. yah265 took {tot['2160p'][0] / 60:.0f}.")
    o = head(W, H, title,
             "Paired bars of encode wall time per ten-minute film at 1080p and 4K, "
             "yah265 against x265 at the same file size.")
    o.append(T(24, 40, title, 22, weight="600"))
    o.append(T(24, 64, "Wall time per ten-minute film at the same file size. Both at preset "
                       "medium with SIMD on all 18 cores of an Apple M5 Max.", 14, "t2"))
    o.append(f"<rect x='24' y='{90 - 10}' width='12' height='12' rx='2' class='s1'/>")
    o.append(T(42, 91, "yah265", 13, "t2"))
    o.append(f"<rect x='110' y='{90 - 10}' width='12' height='12' rx='2' class='sx'/>")
    o.append(T(128, 91, "x265", 13, "t2"))
    y = Tp
    for c in panels:
        rows = {t: r for t, *r in fl[c]}
        top = max(r[1] for r in rows.values() if r[1])
        step = nice_step(top / 60) * 60
        xmax = math.ceil(top / step) * step
        X = lambda v: L + v / xmax * (W - L - R)
        o.append(T(24, y + 18, f"{RES[c]}", 15, "t1", weight="600"))
        o.append(T(24 + 8 * len(RES[c]) + 12, y + 18,
                   f"{len([1 for r in rows.values() if r[0]])} films, "
                   f"{tot[c][1] / 60:.0f} min for x265, {tot[c][0] / 60:.0f} min for yah265",
                   13, "t2"))
        y0 = y + 34
        yb = y0 + rowh * len(timed[c])
        v = 0
        while v <= xmax + 1e-9:
            cls = "base" if v == 0 else "grid"
            o.append(f"<line x1='{X(v):.1f}' y1='{y0 - 4}' x2='{X(v):.1f}' y2='{yb}' class='{cls}'/>")
            o.append(T(X(v), yb + 16, f"{v / 60:g} min" if v else "0", 12, "t2", "middle"))
            v += step
        for i, t in enumerate(timed[c]):
            ry = y0 + rowh * i + (rowh - 2 * barh - gap) / 2
            o.append(T(L - 12, ry + barh + 5, TITLE.get(t, t), 13, "t1", "end"))
            ours, ref, ratio, _ = rows[t]
            o.append(bar(X(0), X(ours), ry, barh, "s1"))
            o.append(bar(X(0), X(ref), ry + barh + gap, barh, "sx"))
            lab = fmin(ours)
            o.append(T(X(ours) + 6, ry + barh - 2, lab, 12, "t1", weight="600"))
            o.append(T(X(ours) + 12 + 6.6 * len(lab), ry + barh - 2,
                       f"{1 / round(ratio, 2):.2f}x faster", 12, "note", weight="600"))
            o.append(T(X(ref) + 6, ry + 2 * barh + gap - 2, fmin(ref), 12, "t2"))
        y = yb + 34 + 16
    o.append("</svg>")
    return "\n".join(o)


def swarm(xs, d):
    """Vertical offsets that keep dots at the x positions xs at least d
    apart: each dot takes the offset nearest the row's center that is free."""
    placed = []
    for x in xs:
        k = 0
        while True:
            cands = [0] if k == 0 else [k * d / 2, -k * d / 2]
            hit = next((dy for dy in cands
                        if all((x - px) ** 2 + (dy - py) ** 2 >= d * d for px, py in placed)), None)
            if hit is not None:
                placed.append((x, hit))
                break
            k += 1
    return [dy for _, dy in placed]


def chart_cells(pl):
    """One dot per timed cell behind the headline, a row per picture size."""
    W, Tp, B = width(), 104, 62
    L, R = pick((190, 40), (172, 22))
    d = 11.5
    pools = [(c, RES[c]) for c in CLASSES if pl["pools"][c]]
    vals = {k: sorted((1 / v, n) for v, n in pl["pools"][k]) for k, _ in pools}
    top = max(v for vs in vals.values() for v, _ in vs)
    x0, x1 = 0.75, math.ceil(top * 2) / 2
    X = lambda v: L + (v - x0) / (x1 - x0) * (W - L - R)
    offs = {k: swarm([X(v) for v, _ in vs], d) for k, vs in vals.items()}
    rowh = {k: max(80, 2 * max(abs(o) for o in offs[k]) + 56) for k in offs}
    H = Tp + sum(rowh.values()) + B
    title = (f"All {pl['n']} size-matched cells run faster than x265, "
             f"{headline_times(pl)} at the median"
             if pl["faster"] == pl["n"]
             else f"{pl['faster']} of {pl['n']} size-matched cells run faster than x265")
    o = head(W, H, title, "One dot per timed cell: times faster than x265 medium at the same "
                          "file size on an Apple M5 Max with SIMD on all cores, in a row per "
                          "picture size, with each row's median marked.")
    o.append(T(24, 40, title, 22, weight="600"))
    o.append(T(24, 64, "One dot per CRF encode at the same file size, with a line at each row's "
                       "median. Preset medium on all cores of an Apple M5 Max.", 14, "t2"))
    v = 1.0
    while v <= x1 + 1e-9:
        o.append(f"<line x1='{X(v):.1f}' y1='{Tp - 8}' x2='{X(v):.1f}' y2='{H - B}' "
                 f"class='{'base' if v == 1 else 'grid'}' stroke-width='{1.5 if v == 1 else 1}'/>")
        o.append(T(X(v), H - B + 20, "1x (x265)" if v == 1 else f"{v:g}x", 12, "t2", "middle"))
        v += 0.5
    o.append(T((L + W - R) / 2, H - 14, "times faster than x265 (right is faster)", 13, "t2",
               "middle"))
    lo = min((v, n, k) for k, vs in vals.items() for v, n in vs)
    hi = max((v, n, k) for k, vs in vals.items() for v, n in vs)
    y = Tp
    for k, label in pools:
        yc = y + rowh[k] / 2
        med = 1 / st.median(v for v, _ in pl["pools"][k])
        o.append(T(24, yc - 3, label, 14, "t1", weight="600"))
        o.append(T(24, yc + 15, f"{len(vals[k])} cells, median {med:.2f}x", 12.5, "t2"))
        for (v, n), dy in zip(vals[k], offs[k]):
            o.append(f"<circle cx='{X(v):.1f}' cy='{yc + dy:.1f}' r='4.75' class='s1 ring' "
                     f"stroke-width='1.5'><title>{esc(n)}: {v:.2f}x faster</title></circle>")
        o.append(f"<line x1='{X(med):.1f}' y1='{y + 22:.1f}' x2='{X(med):.1f}' "
                 f"y2='{y + rowh[k] - 12:.1f}' class='med' stroke-width='2'/>")
        for (v, n, kk), anchor, dx in ((lo, "start", -5), (hi, "end", 5)):
            if kk == k:
                o.append(T(X(v) + dx, y + 14, f"{n}: {v:.2f}x", 12, "t1", anchor))
        y += rowh[k]
    o.append("</svg>")
    return "\n".join(o)


def speed_rows(sps):
    rows = {(c, t) for sp in sps for (c, t, g) in sp}
    return sorted(rows, key=lambda r: (CLASSES.index(r[0]), TIERS.index(r[1])))


def chart_arch(sps, date):
    rows = speed_rows(sps.values())
    vals = [1 / round(sp[(c, t, "G3")][0], 2) for sp in sps.values() for c, t in rows
            if (c, t, "G3") in sp]
    W, Tp, B = width(), 116, 62
    L, R = pick((200, 40), (156, 22))
    rowh = 40
    H = Tp + B + rowh * len(rows)
    x0 = 0.75
    x1 = max(2.0, int(max(vals) * 2 + 1.999) / 2)
    X = lambda v: L + (v - x0) / (x1 - x0) * (W - L - R)
    o = head(W, H, "yah265 is faster than x265 on all three machines",
             "Median times faster than x265 medium at G3, SIMD on all cores, per class and "
             "tier, on an Apple M5 Max, an x86-64 Xeon and a Graviton4.")
    o.append(T(24, 40, "Faster than x265 on all three machines, in every class", 22, weight="600"))
    o.append(T(24, 64, f"Median times faster than x265 medium at the same file size, SIMD on "
                       f"all cores (G3). Parity report of {date}.", 14, "t2"))
    legend(o, [(i, f"s{i + 1}", MACHINE_LABEL[m]) for i, m in enumerate(MACHINES)])
    v = 1.0
    while v <= x1 + 1e-9:
        cls = "base" if v == 1 else "grid"
        dash = " stroke-dasharray='4 3'" if v == 1 else ""
        o.append(f"<line x1='{X(v):.1f}' y1='{Tp - 8}' x2='{X(v):.1f}' y2='{H - B}' "
                 f"class='{cls}' stroke-width='{1.5 if v == 1 else 1}'{dash}/>")
        o.append(T(X(v), H - B + 20, "1x (x265)" if v == 1 else f"{v:g}x", 12, "t2", "middle"))
        v += 0.5
    o.append(T((L + W - R) / 2, H - 14, "times faster than x265 (right is faster)", 13, "t2", "middle"))
    for i, (c, t) in enumerate(rows):
        y = Tp + rowh * (i + 0.5)
        o.append(T(L - 12, y + 4, f"{RES[c]} {TIER_LABEL[t]}", pick(13, 12), "t1", "end"))
        got = {m: 1 / round(sps[m][(c, t, "G3")][0], 2) for m in MACHINES
               if (c, t, "G3") in sps[m]}
        if len(got) > 1:
            o.append(f"<line x1='{X(min(got.values())):.1f}' y1='{y}' "
                     f"x2='{X(max(got.values())):.1f}' y2='{y}' class='span' stroke-width='2'/>")
        for j, m in enumerate(MACHINES):
            if m not in got:
                continue
            close = [k for k in MACHINES if k in got and abs(got[k] - got[m]) < 0.06]
            dy = 0 if len(close) == 1 else (close.index(m) - (len(close) - 1) / 2) * 10
            o.append(marker(j, X(got[m]), y + dy, f"s{j + 1}"))
        miss = [MACHINE_LABEL[m] for m in MACHINES if m not in got]
        if miss:
            o.append(T(W - R, y + 16, "not measured: " + ", ".join(miss), 10.5, "t2", "end"))
    o.append("</svg>")
    return "\n".join(o)


def cell_name(title, size, tier):
    res = {"2160p": "4K"}.get(size, size)
    dur = {"M": "2 min", "L": "10 min"}.get(tier, tier)
    return f"{TITLE.get(title, title)} {res} ({dur})"


def prose_name(title, size, tier):
    dur = {"M": "two-minute segment", "L": "ten-minute film"}.get(tier, "clip")
    return f"the {dur} of {TITLE.get(title, title)} at {RES.get(size, size)}"


def chart_curves(cv):
    W, Tp, B = width(), 120, 64
    cols = pick(len(cv), 1)
    nrow = math.ceil(len(cv) / cols)
    ph = pick(246, 236)
    H = Tp + ph * nrow + B
    pw, pl = (W - 48) / cols, 44
    o = head(W, H, "yah265's rate curves sit above x265's",
             "VMAF v1 against bitrate on five CRF rungs for three titles, yah265 and x265 "
             "at preset medium.")
    o.append(T(24, 40, "The largest, the median and the smallest saving among the ladders", 22,
               weight="600"))
    o.append(T(24, 64, "VMAF v1 against bitrate, five CRF rungs per encoder (CRF 20 to 32). "
                       "Up and left is better.", 14, "t2"))
    o.append(marker(0, 30, 86, "s1", 5.5))
    o.append(T(42, 91, "yah265", 13, "t2"))
    o.append(marker(1, 116, 86, "sx", 5.5))
    o.append(T(128, 91, "x265", 13, "t2"))
    for p, (cell, (ours, ref, model, bd)) in enumerate(cv.items()):
        tier, cls, title = cell.split("/")
        px0 = 24 + pl + (p % cols) * pw
        px1 = px0 + pw - pl - 12
        py0 = Tp + (p // cols) * ph
        py1 = py0 + ph - 36
        allk = [k for k, _ in ours + ref]
        alls = [s for _, s in ours + ref]
        lk0, lk1 = math.log(min(allk) * 0.9), math.log(max(allk) * 1.1)
        s0 = math.floor(min(alls) / 5) * 5
        s1 = math.ceil(max(alls) / 5) * 5
        X = lambda k: px0 + (math.log(k) - lk0) / (lk1 - lk0) * (px1 - px0)
        Y = lambda s: py0 + 30 + (s1 - s) / (s1 - s0) * (py1 - py0 - 30)
        o.append(T(px0 - pl + 4, py0 + 2, cell_name(title, cls, tier), 14, "t1", weight="600"))
        o.append(T(px0 - pl + 4, py0 + 20, f"BD-rate {bd:+.2f}% on VMAF v1", 12.5, "t2"))
        for s in range(s0, s1 + 1, 5):
            o.append(f"<line x1='{px0:.1f}' y1='{Y(s):.1f}' x2='{px1:.1f}' y2='{Y(s):.1f}' "
                     f"class='{'base' if s == s0 else 'grid'}'/>")
            o.append(T(px0 - 6, Y(s) + 4, f"{s}", 11.5, "t2", "end"))
        for tk in (100, 200, 300, 500, 1000, 2000, 3000, 5000, 10000, 20000, 30000):
            if math.exp(lk0) <= tk <= math.exp(lk1):
                o.append(f"<line x1='{X(tk):.1f}' y1='{py1:.1f}' x2='{X(tk):.1f}' "
                         f"y2='{py1 + 4:.1f}' class='base'/>")
                lab = f"{tk / 1000:g}M" if tk >= 1000 else f"{tk}k"
                o.append(T(X(tk), py1 + 18, lab, 11.5, "t2", "middle"))
        for pts, ln, cls, shape in ((ref, "lx", "sx", 1), (ours, "l1", "s1", 0)):
            o.append(f"<polyline fill='none' class='{ln}' stroke-width='2' points='"
                     + " ".join(f"{X(k):.1f},{Y(s):.1f}" for k, s in pts) + "'/>")
            for k, s in pts:
                o.append(marker(shape, X(k), Y(s), cls, 4.5))
    note(o, W / 2, H - 28, "bitrate, log scale (k = kbit/s, M = Mbit/s); VMAF v1 up the side",
         pick(13, 12), "t2", W - 40, "middle")
    o.append("</svg>")
    return "\n".join(o)


def chart_bd(lad, date):
    rows = sorted(lad, key=lambda r: (r[3] + r[4]) / 2)
    allv = [r[3] for r in lad] + [r[4] for r in lad]
    W, Tp, B = width(), 116, 62
    L, R = pick((250, 40), (206, 22))
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
    o.append(T(pick((L + W - R) / 2, W / 2), H - 14, "bitrate change for the same VMAF v1 (left is better)",
               13, "t2", "middle"))
    for i, (title, size, tier, crf, vbr) in enumerate(rows):
        y = Tp + rowh * (i + 0.5)
        o.append(T(L - 12, y + 4, cell_name(title, size, tier), pick(12.5, 11), "t1", "end"))
        o.append(f"<line x1='{X(min(crf, vbr)):.1f}' y1='{y}' x2='{X(max(crf, vbr)):.1f}' "
                 f"y2='{y}' class='span' stroke-width='2'/>")
        dy = 4 if abs(crf - vbr) < 0.8 else 0
        o.append(marker(1, X(vbr), y + dy, "s2", 5))
        o.append(marker(0, X(crf), y - dy, "s1", 5.5))
    o.append("</svg>")
    return "\n".join(o)


def chart_history(pts):
    W, H, L, R, Tp, B = width(), pick(500, 440), 92, pick(100, 84), 160, 64
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
    step = max(1, n // pick(5, 3))
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
        left = x - 4 - len(text) * 6.2 < 20
        o.append(T(x + 4 if left else x - 4, ty, text, 11.5, "t2", "start" if left else "end"))
    for k, cls in (("g1", "s1"), ("g3", "s3")):
        if last[k]:
            o.append(T(X(n - 1) + 10, Y(math.log2(1 / last[k])) + 4,
                       f"{1 / last[k]:.2f}x faster" if last[k] < 1 else f"{last[k]:.2f}x slower",
                       12, "t1"))
    o.append("</svg>")
    return "\n".join(o)


# ------------------------------------------------------------------ values

def fx(ratio):
    """Times faster, from the ratio as the reports print it (2 places), so
    the page agrees with the results tables."""
    return f"{1 / round(ratio, 2):.2f}x"


def frange(meds):
    return f"{fx(max(meds))} to {fx(min(meds))}"


def headline(pl):
    """The site's one headline sentence, from headline_cells(). The home
    page, the README, the launch post, the results page and the social card
    all take their figures from the same dict."""
    return (f"On an Apple M5 Max, yah265 encodes a median {headline_times(pl)} faster than "
            f"x265 at the same file size. Its VMAF v1 score is a median {pl['dv']:.2f} points "
            f"higher. Those medians cover the {pl['n']} CRF encodes of the test set where the "
            "two files match in size within 1%, from short clips to ten-minute films at CIF "
            "to 4K. Both encoders ran at preset medium with SIMD on all cores.")


def headline_table(pl):
    L = ["| size-matched cells on the Mac | count | median | slowest cell |", "|---|--:|--:|---|"]
    for c in CLASSES:
        vs = pl["pools"][c]
        if not vs:
            continue
        w = max(vs)
        L.append(f"| {RES[c]} | {len(vs)} | {times(st.median(v for v, _ in vs))} faster | "
                 f"{w[1]}, {times(w[0])} faster |")
    allv = [x for vs in pl["pools"].values() for x in vs]
    w = max(allv)
    L.append(f"| **all** | **{pl['n']}** | **{headline_times(pl)} faster** | "
             f"{w[1]}, {times(w[0])} faster |")
    return "\n".join(L)


def films_table(fl):
    L = ["| class | film | x265 | yah265 | faster | VMAF v1 ahead |", "|---|---|--:|--:|--:|--:|"]
    for c in ("1080p", "2160p"):
        rows = sorted(fl[c], key=lambda r: -(r[2] or 0))
        for t, ours, ref, ratio, dv in rows:
            if ours is None:
                continue
            L.append(f"| {RES[c]} | {TITLE.get(t, t)} | {fmin(ref)} | {fmin(ours)} | "
                     f"{fx(ratio)} | {dv:+.2f} |")
        ok = [r for r in rows if r[1]]
        ours, ref = sum(r[1] for r in ok), sum(r[2] for r in ok)
        L.append(f"| {RES[c]} | **all {len(ok)} timed** | **{ref / 60:.0f} min** | "
                 f"**{ours / 60:.0f} min** | **{ref / ours:.2f}x** | |")
    return "\n".join(L)


def capped_table(cap):
    L = ["| class | both held to SSE4.2 | both held to AVX2 |", "|---|--:|--:|"]
    for c in CLASSES:
        if c in cap:
            L.append(f"| {RES[c]} | {fx(cap[c]['sse42'][0])} | {fx(cap[c]['avx2'][0])} |")
    return "\n".join(L)


def quality_table(lad):
    L = ["| rate mode | ladders | median bits saved | ahead of x265 | inside the bar | thinnest margin |",
         "|---|--:|--:|--:|--:|---|"]
    sets = [("CRF", [(r[3], r) for r in lad]), ("capped VBR", [(r[4], r) for r in lad])]
    sets.append(("both", sets[0][1] + sets[1][1]))
    for label, vs in sets:
        v, r = max(vs, key=lambda x: x[0])
        L.append(f"| {label} | {len(vs)} | {-st.median(x for x, _ in vs):.1f}% | "
                 f"{sum(1 for x, _ in vs if x < 0)} | {sum(1 for x, _ in vs if x <= BD_BAR)} | "
                 f"{clip_name(r[0], r[1], r[2])}, {v:+.2f}% |")
    return "\n".join(L)


def curves_table(cv):
    L = ["| panel | bits saved at the same VMAF v1 |", "|---|--:|"]
    for cell, (_, _, _, bd) in cv.items():
        tier, cls, title = cell.split("/")
        L.append(f"| {clip_name(title, cls, tier)} | {-bd:.1f}% |")
    return "\n".join(L)


def offmac_table(rows, repro):
    g4 = [r for r in rows["g4"] if r["goal"] in GOALS and r["tier"] in TIERS]
    same = sum(1 for r in g4 if r["mac_md5_same"] == "same")
    return "\n".join([
        "| machine | check | result |", "|---|---|---|",
        f"| Graviton4 | timed encodes that are the Mac's streams byte for byte | "
        f"{same} of {len(g4)} |",
        f"| x86-64 | ladders run again, largest difference from the Mac's BD-rate | "
        f"{len(repro)} ladders, {max(abs(r['diff']) for r in repro):.2f} points |"])


def board_table(hist):
    def cell(v):
        return f"{1 / v:.2f}x faster" if v < 1 else f"{v:.1f}x x265's time"
    g3 = [p for p in hist if p["g3"]]
    return "\n".join([
        "| board median | first timed board | last timed board |", "|---|--:|--:|",
        f"| C on one thread | {cell(hist[0]['g1'])} ({hist[0]['date']}) | "
        f"{cell(hist[-1]['g1'])} ({hist[-1]['date']}) |",
        f"| SIMD on all cores | {cell(g3[0]['g3'])} ({g3[0]['date']}) | "
        f"{cell(g3[-1]['g3'])} ({g3[-1]['date']}) |"])


FRAMES = os.path.join(ROOT, "site/assets/frames/tears.json")
FRAME_W, FRAME_H = 1920, 858
FRAME_CREDIT = ('Tears of Steel: (CC) Blender Foundation, '
                '<a href="https://mango.blender.org">mango.blender.org</a>, CC-BY 3.0.')


def frames_figure(picks, caption):
    """The same-frame comparison, from the record scripts/frame_crops.sh
    wrote beside the crops. picks: which of its picks to show, by index."""
    d = json.load(open(FRAMES))
    enc, zoom = d["encodes"], d["zoom"]
    out = ['<figure class="compare">']
    for k, p in enumerate(d["picks"][i] for i in picks):
        n, c = p["frame"], p["crop"]
        box = (f"left:{c['x'] / FRAME_W * 100:.2f}%;top:{c['y'] / FRAME_H * 100:.2f}%;"
               f"width:{c['w'] / FRAME_W * 100:.2f}%;height:{c['h'] / FRAME_H * 100:.2f}%")
        at = f"{int(p['at_s'] // 60)}:{int(p['at_s'] % 60):02d}"
        out.append('<div class="cmp">')
        out.append(f'<div class="cmp-head"><p class="cmp-title">'
                   f'{at} into the clip<span>VMAF v1 on this frame: yah265 {p["yah265"]:.1f}, '
                   f'x265 {p["x265"]:.1f}. The crop is shown at {zoom}x.</span></p>'
                   f'<div class="cmp-where"><img loading="lazy" decoding="async" '
                   f'width="640" height="286" src="assets/frames/tears-{n}-frame.webp" '
                   f'alt="The whole frame, with the cropped window outlined.">'
                   f'<i style="{box}"></i></div></div>')
        for arm, name, cls, first in (("reference", "Reference", "", ""),
                                      ("yah265", "yah265", " ours", " data-first"),
                                      ("x265", "x265", "", "")):
            if arm == "reference":
                alt, tag = "The reference frame, cropped.", f"Reference · {zoom}x zoom"
            else:
                alt = f"The same crop from {name}'s encode."
                tag = (f"<b>{name}</b> · VMAF v1 {p[arm]:.1f} on this frame · "
                       f"{enc[arm]['bytes'] / 1e6:.2f} MB file · {zoom}x zoom")
            out.append(f'<div class="cmp-pane" data-name="{name}"{first}><img loading="lazy" '
                       f'decoding="async" width="{c["w"] * zoom}" height="{c["h"] * zoom}" '
                       f'src="assets/frames/tears-{n}-{arm}.webp" alt="{alt}">'
                       f'<span class="cmp-tag{cls}">{tag}</span></div>')
        out.append("</div>")
    out.append(f"<figcaption>{caption} {FRAME_CREDIT}</figcaption>")
    out.append("</figure>")
    return "\n".join(out)


# Three sentences of FRAMES_ALL say what the crops look like. Look at the
# crops again and rewrite them whenever scripts/frame_crops.sh picks new ones.
FRAMES_ONE = ("A complex frame of Tears of Steel clip at 1080p, where both "
              "encoders score lowest. The crop shows where the two differ most. yah265 keeps "
              "more of the texture. Both files are the same size within 0.5%. "
              '<a href="results.html#side-by-side-at-the-same-size">Results</a> has more.')
# the crops results.md shows, by index into the picks frame_crops.sh wrote
RESULTS_PICKS = (0, 2)
FRAMES_ALL = ("Two hard moments of the clip, where both encoders score low. Each crop shows "
              "where the two differ most. yah265 keeps more of the texture in the first. "
              "The second is close. <code>scripts/frame_crops.sh</code> makes the crops.")


def claims(pl, fl, ff):
    """Sentences in the post that carry no figure still rest on the data.
    Stop here when one of them stops being true."""
    bad = []
    if pl["faster"] != pl["n"]:
        bad.append("not every timed cell is faster than x265")
    for c in ("1080p", "2160p"):
        for t, ours, ref, ratio, dv in fl[c]:
            if ours is not None and not (ours < ref and (dv or 0) > 0):
                bad.append(f"{t} at {c} is not both faster and ahead on VMAF v1")
    for e in ff:
        if not (e["ours"]["wall_s"] < e["ref"]["wall_s"]
                and e["ours"]["vmaf_mean"] > e["ref"]["vmaf_mean"]):
            bad.append(f"ffmpeg run {e['job']} is not both faster and ahead")
    if bad:
        sys.exit("site/blog.md says otherwise: " + "; ".join(bad))


CARD_LAYOUT = 3  # bump when draw_card's wording or layout changes


def card_key(pl, fl):
    ok = [r for r in fl["2160p"] if r[1]]
    return (f"v{CARD_LAYOUT}|{headline_times(pl)}|{pl['dv']:+.2f}|{pl['n']}|{len(ok)}|"
            f"{sum(r[2] for r in ok) / 60:.0f}|{sum(r[1] for r in ok) / 60:.0f}")


def card_stamp(path):
    """(width, height, stamped key) of a PNG, read from its chunks."""
    try:
        b = open(path, "rb").read()
    except OSError:
        return None
    if b[:8] != b"\x89PNG\r\n\x1a\n":
        return None
    w, h = int.from_bytes(b[16:20], "big"), int.from_bytes(b[20:24], "big")
    i, key = 8, None
    while i + 8 <= len(b):
        n, kind = int.from_bytes(b[i:i + 4], "big"), b[i + 4:i + 8]
        if kind == b"tEXt" and b[i + 8:i + 8 + n].startswith(b"yah265-card\0"):
            key = b[i + 20:i + 8 + n].decode("latin-1")
        i += 12 + n
    return w, h, key


def draw_card(path, pl, fl):
    """The link card: the headline, and the 4K films' total time as two bars."""
    from PIL import Image, ImageDraw, ImageFont, PngImagePlugin

    def font(size, bold=False):
        for name, idx in (("/System/Library/Fonts/HelveticaNeue.ttc", 1 if bold else 0),
                          ("/System/Library/Fonts/Helvetica.ttc", 1 if bold else 0),
                          ("DejaVuSans-Bold.ttf" if bold else "DejaVuSans.ttf", 0)):
            try:
                return ImageFont.truetype(name, size, index=idx)
            except OSError:
                continue
        return ImageFont.load_default()

    W, H, M = 1200, 627, 72
    ink, ink2, bg = "#0b0b0b", "#52514e", "#fcfcfb"
    im = Image.new("RGB", (W, H), bg)
    d = ImageDraw.Draw(im)
    if os.path.exists(LOGO):
        logo = Image.open(LOGO).convert("RGBA")
        logo = logo.resize((round(logo.width * 64 / logo.height), 64), Image.LANCZOS)
        im.paste(logo, (M, 56), logo)
    else:
        d.text((M, 60), "yah265", font=font(52, True), fill=ink)
    # the main line carries the machine and the metric with the two figures
    main = [f"{headline_times(pl)} faster than x265 on the Mac,",
            f"{pl['dv']:+.2f} VMAF v1 at the same file size"]
    size = 92
    while max(d.textlength(t, font=font(size, True)) for t in main) > W - 2 * M:
        size -= 2
    for i, t in enumerate(main):
        d.text((M, 150 + i * round(size * 1.18)), t, font=font(size, True), fill=ink)
    ok = [r for r in fl["2160p"] if r[1]]
    ours, ref = sum(r[1] for r in ok), sum(r[2] for r in ok)
    d.text((M, 368), f"{len(ok)} ten-minute films at 4K, preset medium, same file sizes",
           font=font(26), fill=ink2)
    x0, full = M + 130, W - M - 130 - 200
    for y, name, v, color in ((424, "x265", ref, NEUTRAL), (484, "yah265", ours, BRAND)):
        x1 = x0 + full * v / ref
        d.text((M, y + 4), name, font=font(28, name == "yah265"), fill=ink)
        d.rounded_rectangle((x0 - 8, y, x1, y + 40), radius=8, fill=color)
        d.rectangle((x0 - 8, y, x0, y + 40), fill=color)
        d.text((x1 + 16, y + 4), f"{v / 60:.0f} min", font=font(28, True), fill=ink)
    d.text((M, 562), f"Medians over {pl['n']} size-matched CRF encodes, CIF to 4K, both at "
                     f"preset medium. Apple M5 Max, all cores.", font=font(22), fill=ink2)
    meta = PngImagePlugin.PngInfo()
    meta.add_text("yah265-card", card_key(pl, fl))
    im.save(path, pnginfo=meta, optimize=True)


def values(sps, q, lad, fl, cap, cv, ff, rows, repro, date, report_rel, hist, cred, pl):
    V = {}
    V["headline"] = headline(pl)
    V["fig-frames"] = frames_figure((0,), FRAMES_ONE)
    V["headline-table"] = headline_table(pl)
    V["dv-cells"] = f"{pl['dv_ahead']} of the {pl['dv_n']}"
    V["films-table"] = films_table(fl)
    V["capped-table"] = capped_table(cap)
    V["quality-table"] = quality_table(lad)
    V["curves-table"] = curves_table(cv)
    V["offmac-table"] = offmac_table(rows, repro)
    V["board-table"] = board_table(hist)
    # quality off the Mac: Graviton4 by stream identity, x86 by reproduction
    g4 = [r for r in rows["g4"] if r["goal"] in GOALS and r["tier"] in TIERS]
    V["g4-same"] = f"{sum(1 for r in g4 if r['mac_md5_same'] == 'same')} of {len(g4)}"
    V["x86-repro-n"] = str(len(repro))
    V["x86-repro-max"] = f"{max(abs(r['diff']) for r in repro):.2f}"
    # the films
    for c, key in (("1080p", "hd"), ("2160p", "uhd")):
        rows = [r for r in fl[c] if r[1]]
        ours, ref = sum(r[1] for r in rows), sum(r[2] for r in rows)
        V[f"films-{key}-n"] = str(len(rows))
        V[f"films-{key}-ours"] = f"{ours / 60:.0f} minutes"
        V[f"films-{key}-ref"] = f"{ref / 60:.0f} minutes"
        V[f"films-{key}-x"] = f"{ref / ours:.2f}x"
        V[f"films-{key}-better"] = (f"all {len(rows)}" if all((r[4] or 0) > 0 for r in rows)
                                    else f"{sum(1 for r in rows if (r[4] or 0) > 0)} of {len(rows)}")
    longest = max((r for r in fl["2160p"] if r[1]), key=lambda r: r[2])
    V["longest-title"] = TITLE.get(longest[0], longest[0])
    V["longest-ref"] = fmin(longest[2]).replace(" min", " minutes")
    V["longest-ours"] = fmin(longest[1]).replace(" min", " minutes")
    # speed on every machine
    for m, sp in sps.items():
        for g in GOALS:
            meds = [v[0] for (c, t, gg), v in sp.items() if gg == g]
            V[f"{g.lower()}-{m}"] = frange(meds)
    g3 = [v[0] for sp in sps.values() for (c, t, g), v in sp.items() if g == "G3"]
    V["g3-all"] = frange(g3)
    combos = [(m, k) for m, sp in sps.items() for k in sp]
    passing = [(m, k) for m, k in combos if sps[m][k][0] <= 1.0 and sps[m][k][1] <= 1.15]
    V["combos"] = f"{len(passing)} of {len(combos)}"
    worst = max((sp[k][1], sp[k][2], m, k) for m, sp in sps.items() for k in sp if k[2] == "G3")
    base = re.sub(r"_(cif|\d+p)$", "", worst[1])
    V["slowest-cell"] = (f"{TITLE.get(base, base.capitalize())} at {RES[worst[3][0]]} "
                         f"on the {MACHINE_SHORT[worst[2]]}")
    V["slowest-x"] = fx(worst[0])
    V["combos-line"] = (f"That's {V['combos']} combinations of machine, group and goal inside "
                        f"the bar. {V['slowest-cell']} is the slowest single clip at G3 "
                        f"on any machine and still runs {V['slowest-x']} faster.")
    V["goal-table"] = goal_table(sps)
    V["arch-table"] = arch_table(sps, q)
    V["avx2-range"] = frange([v["avx2"][0] for v in cap.values()])
    V["sse42-range"] = frange([v["sse42"][0] for v in cap.values()])
    V["avx2-worst"] = fx(max(v["avx2"][1] for v in cap.values()))
    # quality
    allv = [r[3] for r in lad] + [r[4] for r in lad]
    V["ladders"] = str(len(allv))
    V["ladders-ahead"] = str(sum(1 for v in allv if v < 0))
    V["ladders-in-bar"] = str(sum(1 for v in allv if v <= BD_BAR))
    V["bd-median"] = f"{abs(st.median(allv)):.1f}%"
    V["bd-crf"] = f"{abs(st.median(r[3] for r in lad)):.1f}%"
    V["bd-vbr"] = f"{abs(st.median(r[4] for r in lad)):.1f}%"
    tightest = max([(r[3], r[0], r[1], r[2], "CRF") for r in lad]
                   + [(r[4], r[0], r[1], r[2], "VBR") for r in lad])
    V["bd-tightest"] = f"{prose_name(tightest[1], tightest[2], tightest[3])} under {tightest[4]}"
    V["bd-tightest-v"] = f"{tightest[0]:+.2f}%"
    qv = list(q.values())
    V["dv-min"] = f"{min(qv):+.2f}"
    V["dv-max"] = f"{max(qv):+.2f}"
    for i, (cell, (_, _, _, bd)) in enumerate(cv.items()):
        tier, cls, title = cell.split("/")
        V[f"curve{i + 1}"] = prose_name(title, cls, tier)
        V[f"curve{i + 1}-bd"] = f"{abs(bd):.1f}%"
    # ffmpeg
    V["ffmpeg-table"] = ffmpeg_table(ff)
    best = max(ff, key=lambda e: e["ref"]["wall_s"] / e["ours"]["wall_s"])
    V["ff-best"] = f"{best['title']} at {best['variant']}"
    V["ff-best-ours"] = f"{best['ours']['wall_s']:.1f} s"
    V["ff-best-ref"] = f"{best['ref']['wall_s']:.1f} s"
    V["ff-tol"] = f"{max(e['size_tolerance'] for e in ff) * 100:g}%"
    V["ff-n"] = str(len(ff))
    V["ff-cmd"] = ffmpeg_cmd(best)
    V["ff-clip"] = f"{best['seconds']} seconds of {best['title']} at {best['variant']}"
    # the rest
    V["report-date"] = date
    V["report-link"] = f"[the all-architecture parity report of {date}]({report_rel})"
    behind, read, pending = cred
    V["credits"] = (f"On {behind} of {read} titles yah265 needs more bytes than x265 there."
                    if behind else f"yah265 is level or ahead on all {read} titles there.")
    if pending:
        V["credits"] += (f" The other {pending} {'is' if pending == 1 else 'are'}"
                         " still pending.")
    g3s = [p for p in hist if p["g3"]]
    V["hist-g3-first"] = f"{g3s[0]['g3']:.1f}x" if g3s else "-"
    V["hist-g1-first"] = f"{hist[0]['g1']:.1f}x" if hist else "-"
    return V


def goal_table(sps):
    L = ["| goal | " + " | ".join(MACHINE_LONG[m] for m in MACHINES) + " |",
         "|---|" + "--:|" * len(MACHINES)]
    lab = GOAL_LONG
    for g in GOAL_ORDER:
        cells = []
        for m in MACHINES:
            meds = [v[0] for (c, t, gg), v in sps[m].items() if gg == g]
            cells.append(frange(meds) if meds else "not measured")
        L.append(f"| {lab[g]} | " + " | ".join(cells) + " |")
    return "\n".join(L)


def arch_table(sps, q):
    L = ["| class | tier | " + " | ".join(MACHINE_LABEL[m] for m in MACHINES)
         + " | VMAF v1 ahead at the same size |",
         "|---|---|" + "--:|" * (len(MACHINES) + 1)]
    for c, t in speed_rows(sps.values()):
        cols = [fx(sps[m][(c, t, "G3")][0]) if (c, t, "G3") in sps[m] else "not measured"
                for m in MACHINES]
        qq = f"{q[(c, t)]:+.2f}" if (c, t) in q else "not measured"
        L.append(f"| {RES[c]} | {TIER_LABEL[t]} | " + " | ".join(cols) + f" | {qq} |")
    return "\n".join(L)


def ffmpeg_table(ff):
    L = ["| clip | encoder | crf | size | encode time | VMAF v1 |",
         "|---|---|--:|--:|--:|--:|"]
    for e in ff:
        name = f"{e['title']} {e['variant']}, {e['seconds']} s"
        for arm in ("ours", "ref"):
            a = e[arm]
            mb = a["bytes"] / 1e6
            t = f"**{a['wall_s']:.1f} s**" if arm == "ours" else f"{a['wall_s']:.1f} s"
            L.append(f"| {name if arm == 'ours' else ''} | {a['encoder']} | {a['crf']:g} | "
                     f"{mb:.2f} MB | {t} | {a['vmaf_mean']:.2f} |")
    return "\n".join(L)


def ffmpeg_cmd(e):
    """The two encodes of one ffmpeg run, and the same encode through the
    y265 command line, as a fenced block a reader can paste."""
    src = f"-ss {e['start_s']:g} -i {e['source']} -frames:v {e['frames']} -an"
    vf = f"-vf scale={e['width']}:{e['height']}:flags=bicubic,format=yuv420p"
    o, r = e["ours"], e["ref"]
    return "\n".join([
        "```sh",
        f"ffmpeg {src} {vf} \\",
        f"  -c:v libyah265 -preset medium -crf {o['crf']:g} -tag:v hvc1 out.yah265.mp4",
        f"ffmpeg {src} {vf} \\",
        f"  -c:v libx265 -preset medium -crf {r['crf']:g} -tag:v hvc1 out.x265.mp4",
        "",
        "# the same yah265 encode through its own command line",
        f"ffmpeg {src} {vf} -f yuv4mpegpipe - | \\",
        f"  build-rel/y265 - -o out.hevc --preset medium --crf {o['crf']:g}",
        "```"])


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
    ap.add_argument("--check", action="store_true",
                    help="change nothing; exit 1 if the page or a chart is out of date")
    ap.add_argument("--card", action="store_true", help="draw the link card again")
    a = ap.parse_args()
    D = r"\d{4}-\d\d-\d\d"
    mac_csv = newest(rf"PARITY-REPORT-yah265-{D}\.csv")
    paths = {"mac": mac_csv,
             "x86": newest(rf"PARITY-REPORT-yah265-x86-{D}\.csv"),
             "g4": newest(rf"PARITY-REPORT-yah265-graviton-{D}\.csv")}
    summary = newest(rf"PARITY-REPORT-yah265-all-isa-{D}\.md")
    date = re.search(f"({D})", os.path.basename(summary))[1]
    rows = {m: report_rows(p) for m, p in paths.items()}
    sps = {m: speed_groups(rs) for m, rs in rows.items()}
    q = quality_groups(rows["mac"])
    mac_md = mac_csv[:-4] + ".md"
    lad = ladders(mac_md)
    if not lad:
        sys.exit(f"{mac_md}: no BD ladder lines found")
    fl = films(rows["mac"])
    cap = capped_run(capped_records(), paths["x86"][:-4] + ".md")
    cv = curves(lad)
    ff = [e for e in json.load(open(FFMPEG))["examples"] if e["title"].lower() not in EXCLUDED]
    hist = board_history()
    report_rel = os.path.relpath(summary, ROOT)
    repro = [r for r in json.load(open(REPRO)) if shown(r["cell"].split("/")[2])]
    pl = headline_cells(rows["mac"])
    claims(pl, fl, ff)
    V = values(sps, q, lad, fl, cap, cv, ff, rows, repro, date, report_rel, hist,
               credits(mac_md), pl)
    mac_date = re.search(f"({D})", os.path.basename(mac_csv))[1]
    charts = render({"films.svg": lambda: chart_films(fl), "cells.svg": lambda: chart_cells(pl),
                     "arch.svg": lambda: chart_arch(sps, date),
                     "curves.svg": lambda: chart_curves(cv),
                     "bdrate.svg": lambda: chart_bd(lad, mac_date),
                     "history.svg": lambda: chart_history(hist)})
    post = open(POST).read()
    new_post = fill(post, V)
    stale = publish_data(OUT, "blog_charts.py", {
        "films": ([mac_csv], ["class", "title", "yah265_wall_s", "x265_wall_s", "times_faster",
                              "vmaf_v1_lead"],
                  [[c, t, o, x, 1 / r, dv] for c in CLASSES for t, o, x, r, dv in fl.get(c, []) if r]),
        "cells": ([mac_csv], ["class", "cell", "times_faster"],
                  [[c, n, 1 / r] for c in CLASSES for r, n in pl["pools"][c]]),
        "arch": (list(paths.values()), *speed_csv(sps, ("G3",))),
        "curves": ([LADDERS], ["cell", "encoder", "kbps", "vmaf_v1", "bd_rate_pct"],
                   [[cell, enc, k, v, bd] for cell, (o, x, _, bd) in cv.items()
                    for enc, pts in (("yah265", o), ("x265", x)) for k, v in pts]),
        "bdrate": ([mac_md], ["title", "size", "tier", "bd_rate_crf_pct", "bd_rate_vbr_pct"],
                   [list(r) for r in lad]),
        "history": ([HISTORY], ["date", "commit", "clips", "g1_wall_over_x265", "g3_wall_over_x265"],
                    [[h["date"], h["commit"], h["n"], h["g1"], h["g3"]] for h in hist]),
    }, a.check)
    for name, svg in charts.items():
        p = os.path.join(OUT, name)
        old = open(p).read() if os.path.exists(p) else None
        if old != svg + "\n":
            stale.append(name)
            if not a.check:
                os.makedirs(OUT, exist_ok=True)
                with open(p, "w") as f:
                    f.write(svg + "\n")
    card = os.path.join(OUT, CARD)
    if card_stamp(card) != (1200, 627, card_key(pl, fl)) or a.card:
        if a.check:
            stale.append(CARD)
        else:
            try:
                draw_card(card, pl, fl)
                stale.append(CARD)
            except ImportError:
                sys.exit(f"{CARD} is out of date and drawing it needs Pillow")
    if new_post != post:
        stale.append("site/blog.md")
        if not a.check:
            with open(POST, "w") as f:
                f.write(new_post)
    print(f"headline: {V['headline']}")
    print(f"reports {', '.join(os.path.relpath(p, ROOT) for p in paths.values())}: "
          f"{len(lad)} ladder cells, {sum(len(s) for s in sps.values())} speed groups, "
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
