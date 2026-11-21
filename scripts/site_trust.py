#!/usr/bin/env python3
# Copyright (c) 2026, the yah265 authors
# SPDX-License-Identifier: GPL-2.0-or-later
"""Fills the pages a skeptical reader checks first, from tracked data.

  site/x265-run.md    how x265 was run: its version and build, the command
                      for each goal, threads, lookahead, instruction sets
  site/from-x265.md   the CRF translation table and chart, and a check of
                      the option map against the captured y265 help
  site/results.md     the worst frames beside the averages
  site/blog.md        the per-frame trace of the Tears of Steel clip

A page keeps each generated part between <!--trust:NAME--> marks. The
commands come from the harness's own builders (scripts/parity_long.py),
the versions and instruction sets from the parity reports, the CRF
translation from the recorded ladders and matched points, and the tail
figures from the Mac report's rows and a per-frame score file.

  scripts/site_trust.py                    fill the pages, draw the charts
  scripts/site_trust.py --check            change nothing; exit 1 if stale
  scripts/site_trust.py --capture-x265     write docs/data/x265-version.txt
  scripts/site_trust.py --capture-tears D  copy the clip's per-frame scores
                                           and facts out of a compare folder

It runs no encode and no scorer.
"""
import argparse
import csv
import json
import math
import os
import re
import shutil
import statistics as st
import subprocess
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import blog_charts as bc  # noqa: E402

ROOT = bc.ROOT
OUT = os.path.join(ROOT, "site/assets/charts/trust")
REL = "assets/charts/trust"
X265_VERSION = os.path.join(ROOT, "docs/data/x265-version.txt")
TEARS_FRAMES = os.path.join(ROOT, "docs/data/tears-perframe.csv")
TEARS_CLIP = os.path.join(ROOT, "docs/data/tears-clip.json")
POINTS = os.path.join(ROOT, "docs/data/parity-spec-points.json")
VBRWALL = os.path.join(ROOT, "docs/data/parity-spec-vbrwall.csv")
HELP = os.path.join(ROOT, "docs/data/cli-help.txt")
PAGES = ["site/x265-run.md", "site/from-x265.md", "site/results.md", "site/blog.md"]
MAP_PAGE = os.path.join(ROOT, "site/from-x265.md")
GEN = re.compile(r"<!--trust:([a-z0-9-]+)-->(.*?)<!--/trust:\1-->", re.S)

# the cell whose own numbers fill the example commands
EXAMPLE = ("L", "1080p", "tears")
TAIL_SHARE = 0.05
LADDER_CRFS = (23, 26, 29, 32)


def die(msg):
    sys.exit(f"site_trust: {msg}")


def mdt(header, align, rows):
    out = ["| " + " | ".join(header) + " |", "|" + "|".join(align) + "|"]
    return "\n".join(out + ["| " + " | ".join(str(c) for c in r) + " |" for r in rows])


# ------------------------------------------------------- how x265 was run

def shown_cmd(argv, env_add=""):
    """A harness command as a reader would type it: the two binaries by
    their names, reading the pipe."""
    words = []
    for i, a in enumerate(argv):
        if i == 0:
            a = "x265" if os.path.basename(a) == "x265" else "y265"
        words.append(a)
    return (env_add + " " if env_add else "") + " ".join(words)


def example_point(mac_rows):
    tier, cls, title = EXAMPLE
    r = next((r for r in mac_rows if (r["tier"], r["class"], r["title"], r["goal"]) ==
              (tier, cls, title, "G3")), None)
    m = r and re.search(r"ours --crf ([\d.]+) \(solved\)", r["note"])
    if not m:
        die(f"no solved CRF for the example cell {EXAMPLE} in the Mac report's rows")
    v = next((v for v in csv.DictReader(open(VBRWALL))
              if (v["tier"], v["class"], v["title"]) == EXAMPLE), None)
    if not v:
        die(f"no VBR wall row for the example cell {EXAMPLE}")
    return m[1], int(v["k"])


def commands(mac_rows):
    """One block per goal, from the builders the harness timed with."""
    try:
        import numpy  # noqa: F401
    except ImportError:
        # the harness loads its scene scanner at import, which needs numpy.
        # the command builders don't, and the site's venv has no numpy
        import types
        sys.modules.setdefault("media_traits", types.ModuleType("media_traits"))
    import parity_long as pl
    crf, kbps = example_point(mac_rows)
    feed = "cat tears_1080p.y4m | "

    def pair(goal, mode, q_ref, q_ours):
        ref, _ = pl.timed_cmd("ref", goal, mode, q_ref, "x265.hevc")
        ours, env = pl.timed_cmd("ours", goal, mode, q_ours, "yah265.hevc")
        add = "N265_ASM_OFF=all" if env.get("N265_ASM_OFF") == "all" else ""
        return feed + shown_cmd(ref) + "\n" + feed + shown_cmd(ours, add)

    blocks = [
        ("G3: both encoders as shipped", pair(3, "crf", pl.ps.REF_CRF, float(crf))),
        ("G2: plain C on every core", pair(2, "crf", pl.ps.REF_CRF, float(crf))),
        ("G1: plain C on one thread", pair(1, "crf", pl.ps.REF_CRF, float(crf))),
        ("Capped VBR as shipped", pair("VBR", "vbr", kbps, kbps)),
        ("The untimed rate ladders and size solve",
         feed + shown_cmd(pl.untimed_cmd("ref", "crf", 26, "x265.hevc")) + "\n"
         + feed + shown_cmd(pl.untimed_cmd("ours", "crf", 26, "yah265.hevc"))),
    ]
    out = []
    for title, body in blocks:
        out.append(f"**{title}**\n\n```sh\n{body}\n```")
    facts = {"threads": pl.THREADS, "maxr": pl.MAXR, "bufr": pl.BUFR, "ref_crf": pl.ps.REF_CRF,
             "crf": crf, "kbps": kbps,
             "g1_ref": " ".join(pl.REF_THREADS["t1"]),
             "solve_ref": f"--pools {pl.THREADS} " + " ".join(pl.REF_THREADS["solve"])}
    return "\n\n".join(out), facts


def report_line(path, start):
    for line in open(path):
        if line.startswith(start) or line.startswith("- " + start):
            return line.strip()
    die(f"{os.path.basename(path)}: no line starting '{start}'")


def need(pattern, text, what):
    m = re.search(pattern, text)
    if not m:
        die(f"{what}: the report no longer says /{pattern}/")
    return m


def builds(paths, mac_rows):
    """The x265 each machine ran and the instruction sets each encoder
    used, read out of the three reports' headers."""
    vers = {r["ref_version"] for r in mac_rows}
    if len(vers) != 1:
        die(f"the Mac rows name more than one x265 version: {sorted(vers)}")
    ver = vers.pop()
    if not os.path.exists(X265_VERSION):
        die("docs/data/x265-version.txt is missing; run --capture-x265")
    cap = open(X265_VERSION).read()
    if ver not in cap:
        die(f"docs/data/x265-version.txt isn't x265 {ver}; run --capture-x265")
    build = need(r"build info (.+)", cap, "x265 version capture")[1].strip()
    caps = need(r"cpu capabilities: (.+)", cap, "x265 version capture")[1].strip()
    mac_md, x86_md, g4_md = (paths[m][:-4] + ".md" for m in bc.MACHINES)
    need(rf"x265 {re.escape(ver)} \(Homebrew\)", report_line(mac_md, "Reference binaries:"),
         "Mac report")
    machine = need(r"Machine: (Apple [^,]+), (\d+) cores", report_line(mac_md, "Machine:"),
                   "Mac report")
    rows = [[f"{machine[1]}, {machine[2]} cores", "Homebrew",
             caps.replace("Neon_DotProd", "DotProd").replace("Neon_I8MM", "I8MM").replace(" ", ", "),
             "NEON"]]
    x86 = report_line(x86_md, "Encoder:")
    need(rf"x265 {re.escape(ver)} built on each box from the 4\.3 release tarball "
         r"\(clang 18, 8-bit, default assembly\)", x86, "x86 report")
    simd = report_line(x86_md, "SIMD:")
    need(r"ours dispatches its AVX-512 tier", simd, "x86 report")
    need(r"x265 runs its default assembly, which on this CPU stops at AVX2", simd, "x86 report")
    need(r"an arm with x265 `--asm avx512`", simd, "x86 report")
    rows.append(["Intel Xeon 8488C, 16 vCPUs", "the 4.3 release tarball, clang 18, 8-bit",
                 "its default, which stops at AVX2 on this CPU. A second arm ran `--asm avx512`",
                 "AVX-512 over AVX2"])
    g4 = report_line(g4_md, "Encoder:")
    need(rf"x265 {re.escape(ver)} built on each box from the 4\.3 release tarball \(clang 18, 8-bit\)",
         g4, "Graviton report")
    simd = report_line(g4_md, "SIMD:")
    need(r"ours dispatches NEON, dotprod, SVE and SVE2", simd, "Graviton report")
    need(r"x265 runs its full aarch64 assembly: NEON, SVE, SVE2, dotprod, i8mm and SVE2 bitperm",
         simd, "Graviton report")
    rows.append(["AWS Graviton4, 16 cores", "the 4.3 release tarball, clang 18, 8-bit",
                 "NEON, SVE, SVE2, DotProd, I8MM, SVE2 BitPerm", "NEON, DotProd, SVE, SVE2"])
    for md in (x86_md, g4_md):
        pin = report_line(md, "Pinning:")
        need(r"ours `--threads 16` and x265 `--pools 16`", pin, os.path.basename(md))
    capped = open(x86_md).read()
    need(r"Ours ran under `N265_CPU_MAX=sse42` and `N265_CPU_MAX=avx2`\. x265 ran at "
         r"`--asm sse4\.2`", capped, "x86 report")
    look = need(r"Lookahead: ours (\d+) at 720p and up, (\d+) at CIF / x265 (\d+)",
                open(mac_md).read(), "Mac report")
    table = mdt(["machine", "x265 build", "x265's instruction sets", "yah265's instruction sets"],
                ["---"] * 4, rows)
    table += f"\n\nThe Homebrew build reports `{build}`."
    return ver, table, look


def threads_table(facts, look):
    t = facts["threads"]
    rows = [
        ["G3", "its default: one worker per CPU", "its defaults for pools and frame threads",
         "on"],
        ["G2", "its default: one worker per CPU", "its defaults for pools and frame threads",
         "off: `N265_ASM_OFF=all` and `--no-asm`"],
        ["G1", "`--threads 1`", f"`{facts['g1_ref']}`", "off: `N265_ASM_OFF=all` and `--no-asm`"],
        ["capped VBR", "its default: one worker per CPU", "its defaults for pools and frame threads",
         "on"],
        ["rate ladders and the size solve", f"`--threads {t}`", f"`{facts['solve_ref']}`", "on"],
        ["cloud machines, G3, G2 and capped VBR", "`--threads 16` on 16 CPUs",
         "`--pools 16` on the same 16 CPUs", "as the goal says"],
        ["Xeon held to one tier", "`N265_CPU_MAX=sse42` or `avx2`", "`--asm sse4.2` or `--asm avx2`",
         "on, capped"],
    ]
    table = mdt(["reading", "yah265 threads", "x265 threads", "SIMD"], ["---"] * 4, rows)
    la = mdt(["encoder", "lookahead at 720p and up", "lookahead at CIF", "set by"], ["---", "--:", "--:", "---"],
             [["yah265", look[1], look[2], "its `medium`"],
              ["x265", look[3], look[3], "its `medium`"]])
    return table, la


# -------------------------------------------------------- CRF translation

def crf_points(mac_rows):
    """x265 CRF -> [(yah265 CRF, cell)] at the same file size. CRF 28 is the
    matched point every size-matched Mac cell was solved to. The other
    rungs place x265's ladder bytes on yah265's ladder by a straight line
    in log bytes between the two rungs around them, and a cell is left out
    of a rung its ladder doesn't bracket."""
    out = {}
    for r in mac_rows:
        m = re.search(r"ours --crf ([\d.]+) \(solved\)", r["note"])
        if r["goal"] == "G3" and r["valid"] == "yes" and r["tier"] in bc.TIERS and m \
                and abs(float(r["dsize_pct"])) <= 1.0:
            out.setdefault(28, []).append(
                (float(m[1]), bc.clip_name(r["title"], r["class"], r["tier"]), r["tier"]))
    lad = {}
    for k, r in json.load(open(bc.LADDERS)).items():
        cell, mode, arm, q, la, b = k.split("|")
        if mode == "crf" and la == "la20" and b == (bc.OURS_BIN if arm == "ours" else bc.REF_BIN) \
                and cell[0] in "ML" and bc.shown(cell.split("/")[2]):
            lad.setdefault(cell, {}).setdefault(arm, {})[float(q)] = r["bytes"]
    for cell, v in sorted(lad.items()):
        if "ours" not in v or "ref" not in v:
            continue
        tier, cls, title = cell.split("/")
        qs = sorted(v["ours"])
        for q in LADDER_CRFS:
            target = v["ref"].get(float(q))
            for a, b in zip(qs, qs[1:]):
                if target and v["ours"][a] >= target >= v["ours"][b]:
                    la, lb, lt = (math.log(x) for x in (v["ours"][a], v["ours"][b], target))
                    out.setdefault(q, []).append(
                        (a + (b - a) * (la - lt) / (la - lb), bc.clip_name(title, cls, tier), tier))
                    break
    return out


def crf_table(cp):
    rows = []
    for q in sorted(cp):
        v = [x for x, *_ in cp[q]]
        rows.append([q, f"{st.median(v):.1f}", f"{min(v):.1f} to {max(v):.1f}", len(v)])
    return mdt(["x265 `--crf`", "`y265 --crf`, median", "range over the cells", "cells"],
               ["--:", "--:", "--:", "--:"], rows)


def chart_crf(cp):
    W, H = bc.width(), bc.pick(520, 500)
    L, R, Tp, B = bc.pick(86, 60), bc.pick(150, 26), 116, 64
    off = st.median(x for x, *_ in cp[28]) - 28
    title = f"For the same file size, yah265's CRF is about {abs(off):.0f} {'below' if off < 0 else 'above'} x265's"
    lo, hi = 18, 34
    X = lambda q: L + (q - 22) / (33 - 22) * (W - L - R)
    Y = lambda q: Tp + (hi - q) / (hi - lo) * (H - Tp - B)
    o = bc.head(W, H, title, "The yah265 CRF that writes the same file size as each x265 CRF, "
                             "one faint line per film cell and the median over the cells.")
    o.append(bc.T(24, 40, title, 22, weight="600"))
    o.append(bc.T(24, 64, "Each faint line is one film at one size. Both encoders at medium.", 14, "t2"))
    bc.legend(o, [(0, "s1", "median yah265 CRF"), (1, "sx", "one cell")])
    for q in range(lo, hi + 1, 2):
        o.append(f"<line x1='{L}' y1='{Y(q):.1f}' x2='{W - R}' y2='{Y(q):.1f}' class='grid'/>")
        o.append(bc.T(L - 10, Y(q) + 4, q, 12, "t2", "end"))
    for q in sorted(cp):
        o.append(f"<line x1='{X(q):.1f}' y1='{H - B}' x2='{X(q):.1f}' y2='{H - B + 5}' class='base'/>")
        o.append(bc.T(X(q), H - B + 20, q, 12, "t2", "middle"))
    o.append(bc.T((L + W - R) / 2, H - 14, "x265 --crf", 13, "t2", "middle"))
    o.append(f"<text x='22' y='{(Tp + H - B) / 2:.1f}' font-size='13' class='t2' text-anchor='middle' "
             f"transform='rotate(-90 22 {(Tp + H - B) / 2:.1f})'>y265 --crf for the same size</text>")
    o.append(f"<line x1='{X(22):.1f}' y1='{Y(22):.1f}' x2='{X(33):.1f}' y2='{Y(33):.1f}' class='base' "
             f"stroke-dasharray='4 3'/>")
    o.append(bc.T(X(33) - 4, Y(33) - 8, "same number", 11.5, "t2", "end"))
    cells = {}
    for q, v in cp.items():
        for x, name, tier in v:
            if tier != "S":
                cells.setdefault(name, []).append((q, x))
    for name, pts in sorted(cells.items()):
        pts.sort()
        o.append("<polyline fill='none' class='lx' stroke-width='1' opacity='0.55' points='"
                 + " ".join(f"{X(q):.1f},{Y(x):.1f}" for q, x in pts) + "'/>")
    for x, _, tier in cp[28]:
        if tier == "S":
            o.append(f"<rect x='{X(28) - 2.5:.1f}' y='{Y(x) - 2.5:.1f}' width='5' height='5' "
                     f"class='sx' opacity='0.7'/>")
    med = [(q, st.median(x for x, *_ in cp[q])) for q in sorted(cp)]
    o.append("<polyline fill='none' class='l1' stroke-width='2.5' points='"
             + " ".join(f"{X(q):.1f},{Y(m):.1f}" for q, m in med) + "'/>")
    for q, m in med:
        o.append(bc.marker(0, X(q), Y(m), "s1", 5))
        o.append(bc.T(X(q), Y(m) + 22, f"{m:.1f}", 12, "t1", "middle", "600"))
    if not bc.NARROW:
        o.append(bc.T(W - R + 14, Y(med[-1][1]) + 4, "median", 12, "t1"))
        o.append(bc.T(W - R + 14, Y(hi) + 30, "Squares at 28 are", 11.5, "t2"))
        o.append(bc.T(W - R + 14, Y(hi) + 45, "the short clips.", 11.5, "t2"))
    o.append("</svg>")
    return "\n".join(o)


# ------------------------------------------------------------ tail data

def tail_of(v):
    s = sorted(v)
    k = max(1, round(len(s) * TAIL_SHARE))
    return st.mean(v), st.mean(s[:k]), s[0], k


def tears():
    if not (os.path.exists(TEARS_FRAMES) and os.path.exists(TEARS_CLIP)):
        die("docs/data/tears-perframe.csv or tears-clip.json is missing; run --capture-tears")
    rows = list(csv.DictReader(open(TEARS_FRAMES)))
    sc = {k: [float(r[k]) for r in rows] for k in ("yah265", "x265")}
    clip = json.load(open(TEARS_CLIP))
    for k in sc:
        if len(sc[k]) != clip["window"]["frames"] or \
                abs(st.mean(sc[k]) - clip["outputs"][k]["vmaf"]["mean"]) > 0.001:
            die(f"tears-perframe.csv and tears-clip.json disagree on {k}")
    return sc, clip


def tears_table(sc, clip):
    a, b = tail_of(sc["yah265"]), tail_of(sc["x265"])
    lab = [f"all {len(sc['x265'])} frames", f"worst {TAIL_SHARE:.0%} of frames ({a[3]} frames)",
           "lowest frame"]
    rows = [[n, f"{x:.2f}", f"{y:.2f}", f"{x - y:+.2f}"] for n, x, y in zip(lab, a, b)]
    o = clip["outputs"]
    rows.append(["file size", f"{o['yah265']['bytes'] / 1e6:.2f} MB", f"{o['x265']['bytes'] / 1e6:.2f} MB",
                 f"{(o['yah265']['bytes'] / o['x265']['bytes'] - 1) * 100:+.1f}%"])
    return mdt(["VMAF v1 over", "yah265", "x265", "yah265's lead"], ["---", "--:", "--:", "--:"], rows)


def chart_trace(sc, clip):
    W, H = bc.width(), bc.pick(470, 440)
    L, R, Tp, B = 60, bc.pick(176, 20), 116, 58
    fps = clip["window"]["fps"]
    n = len(sc["x265"])
    lo = math.floor(min(min(v) for v in sc.values()) / 2) * 2
    hi = 100
    X = lambda i: L + i / (n - 1) * (W - L - R)
    Y = lambda v: Tp + (hi - v) / (hi - lo) * (H - Tp - B)
    ta, tb = tail_of(sc["yah265"]), tail_of(sc["x265"])
    title = (f"On the clip's worst {TAIL_SHARE:.0%} of frames yah265 scores "
             f"{ta[1] - tb[1]:.1f} VMAF v1 points higher")
    o = bc.head(W, H, title, "VMAF v1 of every frame of a 90-second Tears of Steel clip at 1080p, "
                             "for two encodes of the same size. The yah265 line sits above the "
                             "x265 line through most of the low stretches.")
    o.append(bc.T(24, 40, title, 22, weight="600"))
    o.append(bc.T(24, 64, f"Every frame of {clip['clip_label']}, from two files of the same size.",
                  14, "t2"))
    bc.legend(o, [(0, "s1", "yah265"), (1, "sx", "x265")])
    for v in range(lo, hi + 1, 2):
        o.append(f"<line x1='{L}' y1='{Y(v):.1f}' x2='{W - R}' y2='{Y(v):.1f}' class='grid'/>")
        if v % 4 == 0:
            o.append(bc.T(L - 10, Y(v) + 4, v, 12, "t2", "end"))
    for s in range(0, int(n / fps) + 1, bc.pick(10, 15)):
        x = X(min(n - 1, s * fps))
        o.append(f"<line x1='{x:.1f}' y1='{H - B}' x2='{x:.1f}' y2='{H - B + 5}' class='base'/>")
        o.append(bc.T(x, H - B + 20, f"{s} s", 12, "t2", "middle"))
    o.append(bc.T(22, Tp - 12, "VMAF v1", 12, "t2"))
    for k, cls, w in (("x265", "lx", 1.4), ("yah265", "l1", 1.4)):
        o.append(f"<polyline fill='none' class='{cls}' stroke-width='{w}' stroke-linejoin='round' points='"
                 + " ".join(f"{X(i):.1f},{Y(v):.1f}" for i, v in enumerate(sc[k])) + "'/>")
    for (m, t, _, _), cls, name in ((ta, "l1", "yah265"), (tb, "lx", "x265")):
        o.append(f"<line x1='{L}' y1='{Y(t):.1f}' x2='{W - R}' y2='{Y(t):.1f}' class='{cls}' "
                 f"stroke-width='1.2' stroke-dasharray='5 4'/>")
        if not bc.NARROW:
            o.append(bc.T(W - R + 8, Y(t) + 4, f"{name} worst {TAIL_SHARE:.0%}: {t:.1f}", 11.5, "t1"))
    if bc.NARROW:
        o.append(bc.T(L, H - 10, f"Dashes: mean of each encode's worst {TAIL_SHARE:.0%}.", 11.5, "t2"))
    o.append("</svg>")
    return "\n".join(o)


def low_frames(mac_rows):
    """[(class, tier, title, mean lead, lowest-frame lead)] for the
    size-matched Mac cells whose row records a lowest frame."""
    out = []
    for r in mac_rows:
        a, b = bc.fnum(r["ours_vmafneg_min"]), bc.fnum(r["ref_vmafneg_min"])
        if r["goal"] == "G3" and r["valid"] == "yes" and r["tier"] in bc.TIERS and a is not None \
                and b is not None:
            out.append((r["class"], r["tier"], r["title"], float(r["dvmafneg"]), a - b, a, b))
    return out


def low_table(lf):
    rows = []
    for c in bc.CLASSES:
        v = [r for r in lf if r[0] == c]
        if v:
            rows.append([bc.RES[c], len(v), f"{st.median(r[3] for r in v):+.2f}",
                         f"{st.median(r[4] for r in v):+.2f}",
                         f"{sum(1 for r in v if r[4] > 0)} of {len(v)}"])
    rows.append(["**all**", f"**{len(lf)}**", f"**{st.median(r[3] for r in lf):+.2f}**",
                 f"**{st.median(r[4] for r in lf):+.2f}**",
                 f"{sum(1 for r in lf if r[4] > 0)} of {len(lf)}"])
    return mdt(["size", "cells", "median lead, all frames", "median lead, lowest frame",
                "cells ahead on their lowest frame"], ["---", "--:", "--:", "--:", "--:"], rows)


def figure(name, alt):
    return f'<figure class="chart"><img src="{REL}/{name}.svg" alt="{bc.esc(alt)}"></figure>'


# ------------------------------------------------------------ option map

def check_map():
    """Every y265 option the map names is in the captured help, every
    option it lists as not accepted is absent from it, and every x265
    option it names is one the installed x265 knows."""
    if not os.path.exists(MAP_PAGE):
        return []
    text = open(MAP_PAGE).read()
    # an option the help names anywhere, in a heading or as another's twin
    have = set(re.findall(r"(?<![\w-])(--[a-z][\w-]+)", open(HELP).read()))
    have |= {"--no-" + f[2:] for f in have}
    bad = []
    part = lambda a, b: text[text.index(a):text.index(b)] if a in text and b in text else ""
    accepted = part("<!--map:accepted-->", "<!--/map:accepted-->")
    renamed = part("<!--map:renamed-->", "<!--/map:renamed-->")
    refused = part("<!--map:refused-->", "<!--/map:refused-->")
    if not (accepted and renamed and refused):
        return ["site/from-x265.md: a map section's marks are missing"]
    flags = lambda s: re.findall(r"`(--[\w-]+)", s)
    for f in flags(accepted):
        if f not in have:
            bad.append(f"site/from-x265.md: {f} is listed as accepted and isn't in the y265 help")
    for line in renamed.splitlines():
        cols = line.split("|")
        if len(cols) > 3 and cols[1].strip().startswith("`"):
            for f in flags(cols[2]):
                if f not in have:
                    bad.append(f"site/from-x265.md: {f} is given as the y265 name and isn't in its help")
    for line in refused.splitlines():
        cols = line.split("|")
        if len(cols) > 2 and cols[1].strip().startswith("`"):
            for f in flags(cols[1]):
                if f in have and f not in ("--preset", "--tune", "--intra-refresh", "--scaling-list"):
                    bad.append(f"site/from-x265.md: {f} is listed as not accepted and is in the y265 help")
    x265 = shutil.which("x265")
    if x265:
        theirs = subprocess.run([x265, "--fullhelp"], capture_output=True, text=True)
        known = set(re.findall(r"--(?:\[no-\]-?)?([\w.-]+)", theirs.stdout + theirs.stderr))
        for sect, col in ((accepted, None), (renamed, 1), (refused, 1)):
            for line in sect.splitlines():
                cols = line.split("|")
                src = line if col is None else (cols[col] if len(cols) > col else "")
                for f in flags(src):
                    name = f[2:]
                    if name not in known and re.sub(r"^no-", "", name) not in known:
                        bad.append(f"site/from-x265.md: x265 --fullhelp has no {f}")
    return bad


# ---------------------------------------------------------------- driver

def capture_x265():
    r = subprocess.run(["x265", "--version"], capture_output=True, text=True)
    text = r.stderr + r.stdout
    if "HEVC encoder version" not in text:
        die("x265 --version printed no version")
    open(X265_VERSION, "w").write(text)
    print(f"wrote {os.path.relpath(X265_VERSION, ROOT)}")


def capture_tears(d):
    log = {k: json.load(open(os.path.join(d, f"{k}.vmaf.json"))) for k in ("yah265", "x265")}
    sc = {k: [f["metrics"]["v1"] for f in v["frames"]] for k, v in log.items()}
    if len(sc["yah265"]) != len(sc["x265"]):
        die("the two score logs have different frame counts")
    with open(TEARS_FRAMES, "w") as f:
        f.write("frame,yah265,x265\n")
        for i, (a, b) in enumerate(zip(sc["yah265"], sc["x265"])):
            f.write(f"{i},{a:.6f},{b:.6f}\n")
    m = json.load(open(os.path.join(d, "manifest.json")))
    enc = {k: {x: v[x] for x in ("library", "version", "preset", "crf", "threads", "tune")}
           for k, v in m["encoders"].items()}
    keep = {
        "title": m["title"], "licence": m["licence"], "credit": m["credit"],
        "clip_label": "a 90-second Tears of Steel clip at 1080p",
        "source_url": m["source"]["url"], "window": m["window"], "encoders": enc,
        "match": {k: m["match"][k] for k in ("rule", "tol", "size_ratio_x265_over_yah265")},
        "outputs": {k: {"bytes": m["outputs"][k]["bytes"], "sha256": m["outputs"][k]["sha256"],
                        "vmaf": m["outputs"][k]["vmaf"]} for k in ("yah265", "x265")},
        "vmaf": {k: m["vmaf"][k] for k in ("model", "model_sha256", "method")},
    }
    open(TEARS_CLIP, "w").write(json.dumps(keep, indent=1) + "\n")
    print(f"wrote {os.path.relpath(TEARS_FRAMES, ROOT)} and {os.path.relpath(TEARS_CLIP, ROOT)}")


def fill(rel, text, V):
    missing = sorted({m[1] for m in GEN.finditer(text) if m[1] not in V})
    if missing:
        die(f"{rel}: no value for {missing}")

    def sub(m):
        v = V[m[1]]
        if "\n" in v or v.startswith("<figure"):
            return f"<!--trust:{m[1]}-->\n{v}\n<!--/trust:{m[1]}-->"
        return f"<!--trust:{m[1]}-->{v}<!--/trust:{m[1]}-->"
    return GEN.sub(sub, text)


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--check", action="store_true",
                    help="change nothing; exit 1 if a page or a chart is out of date")
    ap.add_argument("--capture-x265", action="store_true",
                    help="write docs/data/x265-version.txt from the x265 on PATH")
    ap.add_argument("--capture-tears", metavar="DIR",
                    help="copy per-frame scores and clip facts out of a compare folder")
    a = ap.parse_args()
    if a.capture_x265:
        return capture_x265()
    if a.capture_tears:
        return capture_tears(a.capture_tears)
    D = r"\d{4}-\d\d-\d\d"
    paths = {"mac": bc.newest(rf"PARITY-REPORT-yah265-{D}\.csv"),
             "x86": bc.newest(rf"PARITY-REPORT-yah265-x86-{D}\.csv"),
             "g4": bc.newest(rf"PARITY-REPORT-yah265-graviton-{D}\.csv")}
    mac_rows = bc.report_rows(paths["mac"])
    cmds, facts = commands(mac_rows)
    ver, build_table, look = builds(paths, mac_rows)
    thr, la = threads_table(facts, look)
    cp = crf_points(mac_rows)
    sc, clip = tears()
    lf = low_frames(mac_rows)
    ta, tb = tail_of(sc["yah265"]), tail_of(sc["x265"])
    charts = bc.render({"crf-map.svg": lambda: chart_crf(cp),
                        "tail-trace.svg": lambda: chart_trace(sc, clip)})
    V = {
        "x265-version": ver,
        "x265-builds": build_table,
        "x265-commands": cmds,
        "x265-threads": thr,
        "x265-lookahead": la,
        "x265-ref-crf": str(facts["ref_crf"]),
        "crf-table": crf_table(cp),
        "fig-crf": figure("crf-map", "Line chart of the yah265 CRF that writes the same file size as "
                                     "each x265 CRF from 23 to 32. The median line runs about two "
                                     "below the same-number diagonal. A few animated films are on or "
                                     "above it."),
        "fig-trace": figure("tail-trace", "Line chart of VMAF v1 for every frame of a 90-second clip, "
                                          "one line per encoder. The yah265 line is above the x265 "
                                          "line in most of the low stretches."),
        "tears-table": tears_table(sc, clip),
        "tears-crfs": (f"`-crf {clip['encoders']['yah265']['crf']}` for yah265 and "
                       f"`-crf {clip['encoders']['x265']['crf']}` for x265"),
        "low-table": low_table(lf),
    }
    stale = bc.publish_data(OUT, "site_trust.py", {
        "crf-map": ([paths["mac"], bc.LADDERS],
                    ["x265_crf", "yah265_crf_same_size", "cell", "tier"],
                    [[q, x, n, t] for q in sorted(cp) for x, n, t in cp[q]]),
        "tail-trace": ([TEARS_FRAMES], ["frame", "yah265_vmaf_v1", "x265_vmaf_v1"],
                       [[i, x, y] for i, (x, y) in enumerate(zip(sc["yah265"], sc["x265"]))]),
    }, a.check)
    for name, svg in charts.items():
        p = os.path.join(OUT, name)
        if (open(p).read() if os.path.exists(p) else None) != svg + "\n":
            stale.append(name)
            if not a.check:
                open(p, "w").write(svg + "\n")
    for rel in PAGES:
        p = os.path.join(ROOT, rel)
        if not os.path.exists(p):
            die(f"{rel} is missing")
        old = open(p).read()
        new = fill(rel, old, V)
        if new != old:
            stale.append(rel)
            if not a.check:
                open(p, "w").write(new)
    bad = check_map()
    for b in bad:
        print(b, file=sys.stderr)
    print(f"x265 {ver}; CRF translation over {len(cp[28])} matched cells; "
          f"Tears clip worst {TAIL_SHARE:.0%}: yah265 {ta[1]:.2f}, x265 {tb[1]:.2f}; "
          f"{len(lf)} cells with a lowest frame")
    if a.check:
        if stale or bad:
            print("stale: " + ", ".join(stale))
            sys.exit(1)
        print("up to date")
    else:
        print("updated: " + (", ".join(stale) or "nothing changed"))
        if bad:
            sys.exit(1)


if __name__ == "__main__":
    main()
