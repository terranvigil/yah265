#!/usr/bin/env python3
# Copyright (c) 2026, the yah265 authors
# SPDX-License-Identifier: GPL-2.0-or-later
"""tables_report.py -- the cloud boxes' records as the spec's tables.

Reads the records tables_run.py and ladder_run.py left on the hub (pulled
to RES), the plans that made them, and the Mac's data (points, windows,
runs, VBR walls, ladders) for the identity and reproduction checks, and
writes one report and one CSV per architecture in the Mac report's layout.

    tables_report.py --res DIR --plans DIR --date 2026-11-13 [--write]
"""
import argparse
import collections
import csv
import datetime
import glob
import json
import math
import os
import statistics
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(
    os.path.abspath(__file__))))
sys.path.insert(0, os.path.join(ROOT, "scripts"))
import bdcompare as bd                                    # noqa: E402

BAR_MED, BAR_WORST, TOL, WIN_VOID, FEED_MARGIN = 1.00, 1.15, 1.0, 5.0, 1.5
CLS = ("CIF", "720p", "1080p", "2160p")
TIERS = ("S", "M", "L")
COMBOS = [("CIF", "S"), ("720p", "S"), ("720p", "L"), ("1080p", "S"),
          ("1080p", "M"), ("1080p", "L"), ("2160p", "S"), ("2160p", "M"),
          ("2160p", "L")]
COLS = ("codec,date,commit,ref,ref_version,tier,class,title,size,goal,"
        "ours_wall_s,ref_wall_s,ratio,feed_fps,ours_bytes,ref_bytes,dsize_pct,"
        "ours_vmafneg,ref_vmafneg,dvmafneg,ours_psnr_y,ref_psnr_y,dpsnr_y,"
        "bd_vmafneg_pct,ours_vmafneg_hmean,ref_vmafneg_hmean,ours_vmafneg_min,"
        "ref_vmafneg_min,ours_vmafneg_low1,ref_vmafneg_low1,"
        "ours_vmafneg_first1s,ref_vmafneg_first1s,ours_vmaf,ref_vmaf,"
        "ours_psnr_yuv611,ref_psnr_yuv611,ours_ssim_yuv611,ref_ssim_yuv611,"
        "frames_decoded_ok,rounds,valid,note").split(",") + [
        "arch", "box_type", "ref512_wall_s", "ratio_ref512", "ours_md5",
        "mac_md5_same"]


def load_plans(d):
    cells, boxes = {}, {}
    for p in glob.glob(os.path.join(d, "plan_*.json")):
        pl = json.load(open(p))
        boxes[pl["box"]] = pl
        for c in pl["cells"]:
            cells[c["name"]] = c
    return cells, boxes


def arch_of(box):
    return "x86" if box.startswith("x86") else (
        "graviton3" if box.startswith("g3") else "graviton4")


def load_res(d):
    recs = collections.defaultdict(list)
    for p in glob.glob(os.path.join(d, "*.jsonl")):
        box = os.path.basename(p)[:-6]
        for ln in open(p):
            try:
                r = json.loads(ln)
            except ValueError:
                continue
            r["_box"] = box
            recs[arch_of(box)].append(r)
    return recs


def mac_runs():
    """(tier, title, goal) -> [(bytes, md5)] of ours, from the Mac's runs."""
    out = collections.defaultdict(list)
    for r in csv.DictReader(open(os.path.join(ROOT, "docs/data/parity-spec-runs.csv"))):
        if r["arm"] == "ours" and r["md5"] not in ("FAILED", "-"):
            out[(r["tier"], r["title"], r["goal"])].append(
                (int(r["bytes"] or 0), r["md5"], r["date"]))
    return out


def mac_vbr():
    out = {}
    for r in csv.DictReader(open(os.path.join(ROOT, "docs/data/parity-spec-vbrwall.csv"))):
        out[f"{r['tier']}/{r['class']}/{r['title']}"] = r
    return out


def mac_title(c, goal):
    if c["tier"] == "L":
        return f"{c['title']}_{c['cls']}"
    return c["title"]


def cell_rows(c, recs, arch, macr, macv):
    """One row per goal of the cell, from its records."""
    by = collections.defaultdict(list)
    feed = {}
    check = None
    # a (cell, goal) is read from one box: the last one that ran it (a box
    # reclaimed mid-goal leaves a partial set that the next box redoes whole)
    last = {}
    for r in recs:
        if r.get("cell") == c["name"] and r.get("rc") == 0 and "wall_s" in r:
            last[r["goal"]] = r.get("box")
    recs = [r for r in recs if r.get("cell") != c["name"] or "wall_s" not in r
            or r.get("box") == last.get(r.get("goal"))]
    for r in recs:
        if r.get("cell") != c["name"] or r.get("rc") != 0:
            continue
        if r.get("goal") == "prep":
            check = r
            continue
        if r.get("arm") == "feed":
            feed[r["goal"]] = r["feed_fps"]
            continue
        if r.get("kind") != "run" or "wall_s" not in r:
            continue
        by[(r["goal"], r["round"], r["arm"])].append(r)
    rows = {}
    for goal in ("G1", "G2", "G3", "VBR"):
        nwin = (len(c["windows"][goal]["spans"]) if goal in ("G1", "G2")
                and c.get("windows") else 1)
        walls = collections.defaultdict(list)
        sizes = collections.defaultdict(list)
        md5s = collections.defaultdict(list)
        for (g, rnd, arm), rs in by.items():
            if g != goal:
                continue
            ws = {r.get("win", 0): r for r in rs}
            if len(ws) != nwin:
                continue
            walls[arm].append(sum(r["wall_s"] for r in ws.values()))
            sizes[arm].append(sum(r["bytes"] for r in ws.values()))
            md5s[arm].append(tuple(ws[i]["md5"] for i in sorted(ws)))
        if not walls.get("ours") or not walls.get("ref"):
            continue
        mo, mr = statistics.median(walls["ours"]), statistics.median(walls["ref"])
        m5 = statistics.median(walls["ref512"]) if walls.get("ref512") else None
        ob, rb = statistics.median(sizes["ours"]), statistics.median(sizes["ref"])
        frames = sum(n for _, n in (c["windows"][goal]["spans"]
                                    if nwin > 1 or (goal in ("G1", "G2") and c.get("windows"))
                                    else [[0, c["frames"]]]))
        flags, notes = [], []
        ds = 100 * (ob / rb - 1)
        fps = feed.get(goal)
        fast = frames / min(min(walls["ours"]), min(walls["ref"]))
        if fps and goal != "G1" and fps < FEED_MARGIN * fast:
            flags.append("feeder-bound")
            notes.append(f"feeder {fps:.0f} fps < 1.5x the faster arm's {fast:.0f}")
        if goal != "VBR":
            lim = WIN_VOID if (goal in ("G1", "G2") and c.get("windows")) else TOL
            if abs(ds) > lim:
                flags.append("size-mismatch")
        rounds = min(len(walls["ours"]), len(walls["ref"]))
        if rounds < 2 and not (c["tier"] == "L" and goal in ("G1", "G2")):
            flags.append("single-run")
        if goal == "G1" and len(set(md5s["ours"])) > 1:
            notes.append("FAIL: --threads 1 run-to-run md5 mismatch (ours)")
        if goal == "G1" and len(set(md5s["ref"])) > 1:
            notes.append("FAIL: --threads 1 run-to-run md5 mismatch (x265)")
        if goal != "G1" and len(set(md5s["ours"])) > 1:
            notes.append("warning: ours md5 differs between rounds")
        # the Mac's stream for the same point, where the Mac recorded one
        same = None
        om = md5s["ours"][0]
        if goal in ("G1", "G2") and c.get("windows"):
            want = tuple(c["windows"][goal]["ours_md5"])
            same = om == want
        elif goal == "VBR":
            mv = macv.get(c["key"])
            same = mv is not None and om[0] == mv["ours_md5"]
        else:
            cand = macr.get((c["tier"], mac_title(c, goal), goal), [])
            cand = [m for b, m, d in cand if b and abs(b / ob - 1) < 0.03]
            pt = c["points"]["ft1" if goal == "G1" else "def"]
            if cand or pt.get("ours_md5"):
                same = om[0] in set(cand) | {pt.get("ours_md5")}
        rows[goal] = dict(mo=mo, mr=mr, m5=m5, ob=ob, rb=rb, ds=ds, fps=fps,
                          rounds=rounds, flags=flags, notes=notes,
                          ratio=None if "feeder-bound" in flags else mo / mr,
                          r512=None if "feeder-bound" in flags or not m5 else mo / m5,
                          frames=frames, md5=om[0], same=same,
                          kbps={a: statistics.median(sizes[a]) * 8 / (frames / c["fps"]) / 1000
                                for a in sizes})
    return rows, check, feed


def cname(c):
    sz = c["cls"].lower() if c["cls"] == "CIF" else c["cls"]
    return c["title"] if c["title"].endswith(sz) else f"{c['title']}_{sz}"


def f2(x):
    return "-" if x is None else f"{x:.2f}"


def med_worst(xs):
    xs = [x for x in xs if x is not None]
    if not xs:
        return None
    return statistics.median(xs), max(xs)


def write(arch, cells, recs, boxes, date, out_md, out_csv, extra):
    macr, macv = mac_runs(), mac_vbr()
    pts = json.load(open(os.path.join(ROOT, "docs/data/parity-spec-points.json")))
    allrows = {}
    checks = {}
    for name, c in cells.items():
        rows, chk, _ = cell_rows(c, recs, arch, macr, macv)
        if rows:
            allrows[name] = (c, rows)
        if chk:
            checks[name] = chk
    # a cell re-solved on this box (NAME_r) supersedes the Mac-point one
    for name in list(allrows):
        if name.endswith("_r") and name[:-2] in allrows:
            del allrows[name[:-2]]
    # a cell re-run from a staged raw copy (NAME_s) supersedes the goals it
    # re-ran: the decode-fed run was feeder-bound there
    for name in list(allrows):
        if name.endswith("_s") and name[:-2] in allrows:
            base = allrows[name[:-2]][1]
            for g, r in allrows[name][1].items():
                base[g] = r
            del allrows[name]
    lines = []
    A = lambda s="": lines.append(s)                       # noqa: E731
    env = extra.get("env", {})
    title = {"x86": "x86-64", "graviton4": "Graviton4",
             "graviton3": "Graviton3"}[arch]
    A(f"# yah265 parity report, {title}, {date}")
    A()
    for ln in extra.get("header", []):
        A(ln)
    A()
    # speed per class and tier
    speed = {}
    for cls, tier in COMBOS:
        for g in ("G1", "G2", "G3", "VBR"):
            rs = [(c, r[g]) for c, r in allrows.values()
                  if c["cls"] == cls and c["tier"] == tier and g in r]
            valid = [x for x in rs if not x[1]["flags"] or
                     x[1]["flags"] == ["single-run"]]
            speed[(cls, tier, g)] = (rs, valid)
    A("## Verdict")
    verdict = {}
    for g, nm in (("G1", "G1 pure C, 1 thread:  "), ("G2", "G2 pure C, all cores: "),
                  ("G3", "G3 SIMD, all cores:   ")):
        ps, op, nm_ = [], [], []
        for cls, tier in COMBOS:
            rs, valid = speed[(cls, tier, g)]
            mw = med_worst([x[1]["ratio"] for x in valid])
            want = expected(cells, cls, tier)
            if not mw:
                nm_.append(f"{cls}-{tier}")
                continue
            ok = mw[0] <= BAR_MED and mw[1] <= BAR_WORST
            part = f" ({len(valid)} of {want} cells)" if len(valid) < want else ""
            (ps if ok else op).append(f"{cls}-{tier}{part}")
            verdict[(cls, tier, g)] = ok
        A(f"{nm}" + "; ".join(x for x in (
            "PASS in " + ", ".join(ps) if ps else "",
            "OPEN in " + ", ".join(op) if op else "",
            "not measured in " + ", ".join(nm_) if nm_ else "") if x))
    for ln in extra.get("verdict_extra", []):
        A(ln)
    A()
    A("## Speed (median / worst ratio, per class and tier)")
    hdr = "| class | tier | G1 | G2 | G3 |" + (" G3 vs x265 --asm avx512 |" if arch == "x86" else "") + " VBR wall (G3) | cells |"
    A(hdr)
    A("|" + "---|" * (hdr.count("|") - 1))
    for cls, tier in COMBOS:
        row = f"| {cls} | {tier} |"
        cnt = []
        for g in ("G1", "G2", "G3"):
            rs, valid = speed[(cls, tier, g)]
            mw = med_worst([x[1]["ratio"] for x in valid])
            dag = "†" if any("single-run" in x[1]["flags"] for x in valid) else ""
            row += f" {f2(mw[0])} / {f2(mw[1])}{dag} |" if mw else " - |"
            cnt.append(f"{len(valid)}/{expected(cells, cls, tier)}")
        if arch == "x86":
            rs, valid = speed[(cls, tier, "G3")]
            mw = med_worst([x[1]["r512"] for x in valid])
            row += f" {f2(mw[0])} / {f2(mw[1])} |" if mw else " - |"
        rs, valid = speed[(cls, tier, "VBR")]
        mw = med_worst([x[1]["ratio"] for x in valid])
        row += f" {f2(mw[0])} / {f2(mw[1])} |" if mw else (" n/a |" if tier == "S" else " - |")
        row += " " + " ".join(cnt) + " |"
        A(row)
    A()
    A("Cells counted (G1 G2 G3) are valid timed cells of the class and tier's gating set.")
    A()
    for sec in extra.get("sections", []):
        for ln in sec:
            A(ln)
        A()
    # identity with the Mac's streams, per goal
    tally = collections.defaultdict(lambda: [0, 0, 0])
    for c, rows in allrows.values():
        for g, r in rows.items():
            tally[g][{True: 0, False: 1, None: 2}[r["same"]]] += 1
    if arch == "x86":
        tally = {}
    if tally:
        A("## Our streams against the Mac's")
        A("Each timed encode's md5 against the Mac's stream at the same point (the "
          "Mac's timed runs, its G1/G2 window solves, its VBR walls). Same / "
          "different / no Mac record:")
        A("| goal | same | different | no Mac record |")
        A("|---|---|---|---|")
        for g in ("G1", "G2", "G3", "VBR"):
            if g in tally:
                A(f"| {g} | {tally[g][0]} | {tally[g][1]} | {tally[g][2]} |")
        A()
    # the VBR wall, per cell
    vb = [(c, r["VBR"]) for c, r in allrows.values() if "VBR" in r]
    if vb:
        A("## VBR wall (G3 only: SIMD, all 16 vCPUs, one rung per cell)")
        A("The Mac's rung per cell (x265's CRF 28 rate on the Mac), maxrate 1.5x, "
          "buffer 2x, lookahead ours 20 / x265 20, 2 interleaved rounds, no size "
          "match. Tracking is the achieved bitrate against the target.")
        A("| cell | tier | target kbit/s | ratio | ours kbit/s | x265 kbit/s | feed fps | rounds |")
        A("|---|---|---|---|---|---|---|---|")
        for c, r in sorted(vb, key=lambda x: (TIERS.index(x[0]["tier"]), CLS.index(x[0]["cls"]), x[0]["title"])):
            k = c["vbr_k"]
            ko, kr = r["kbps"].get("ours", 0), r["kbps"].get("ref", 0)
            A(f"| {cname(c)} | {c['tier']} | {k} | {f2(r['ratio'])} | "
              f"{ko:.0f} ({100 * (ko / k - 1):+.1f}%) | {kr:.0f} ({100 * (kr / k - 1):+.1f}%) | "
              f"{r['fps'] or '-'} | {r['rounds']} |")
        A()
    # per-cell table
    A("## Every cell")
    A("| cell | tier | goal | ours s | x265 s | ratio | " + ("x265 avx512 s | ratio | " if arch == "x86" else "") + "dsize | feed fps | rounds | ours md5 = Mac | valid |")
    A("|---|---|---|---|---|---|" + ("---|---|" if arch == "x86" else "") + "---|---|---|---|---|")
    csvrows = []
    for name in sorted(allrows, key=lambda n: (TIERS.index(allrows[n][0]["tier"]), CLS.index(allrows[n][0]["cls"]), n)):
        c, rows = allrows[name]
        for g, r in rows.items():
            same = {True: "same", False: "DIFFERENT", None: "-"}[r["same"]]
            A(f"| {cname(c)} | {c['tier']} | {g} | {r['mo']:.2f} | {r['mr']:.2f} | {f2(r['ratio'])} | "
              + (f"{f2(r['m5'])} | {f2(r['r512'])} | " if arch == "x86" else "")
              + f"{r['ds']:+.2f}% | {r['fps'] or '-'} | {r['rounds']} | {same} | {r['flags'][0] if r['flags'] else 'yes'} |")
            pt = pts.get(f"{c['key']}/{'ft1' if g == 'G1' else 'def'}") or pts.get(f"{c['key']}/def") or {}
            note = list(r["notes"])
            if g == "VBR":
                note.append(f"VBR rung {c.get('vbr_k')} kbit/s: ours {r['kbps'].get('ours', 0):.0f}, x265 {r['kbps'].get('ref', 0):.0f}")
            csvrows.append({
                "codec": "yah265", "date": date, "commit": "504fdda",
                "ref": "x265 --preset medium" + (" --no-asm" if g in ("G1", "G2") else ""),
                "ref_version": "4.3+1-e9b8812", "tier": c["tier"], "class": c["cls"],
                "title": c["title"], "size": c["src"].get("hdr", "").split(" ")[1:3] and "x".join(x[1:] for x in c["src"].get("hdr", "x W0 H0").split()[1:3]) or "",
                "goal": g, "ours_wall_s": f"{r['mo']:.3f}", "ref_wall_s": f"{r['mr']:.3f}",
                "ratio": f2(r["ratio"]).replace("-", ""), "feed_fps": r["fps"] or "",
                "ours_bytes": int(r["ob"]), "ref_bytes": int(r["rb"]), "dsize_pct": f"{r['ds']:.2f}",
                "ours_vmafneg": f2(pt.get("ours_vmafneg")) if g != "VBR" else "",
                "ref_vmafneg": f2(pt.get("ref_vmafneg")) if g != "VBR" else "",
                "dvmafneg": f2(pt["ours_vmafneg"] - pt["ref_vmafneg"]) if pt and g != "VBR" else "",
                "ours_psnr_y": f2(pt.get("ours_psnr_y")) if g != "VBR" else "",
                "ref_psnr_y": f2(pt.get("ref_psnr_y")) if g != "VBR" else "",
                "dpsnr_y": f2(pt["ours_psnr_y"] - pt["ref_psnr_y"]) if pt and g != "VBR" else "",
                "rounds": r["rounds"], "valid": r["flags"][0] if r["flags"] else "yes",
                "note": "; ".join(note + ["quality columns are the Mac's at this point"
                                          + (" (streams identical here)" if r["same"] else " (not re-scored here)")]),
                "arch": arch, "box_type": extra.get("box_types", ""),
                "ref512_wall_s": f"{r['m5']:.3f}" if r["m5"] else "",
                "ratio_ref512": f2(r["r512"]) if r["r512"] else "",
                "ours_md5": r["md5"], "mac_md5_same": same})
    A()
    if checks:
        A("## Source frames checked against the Mac's")
        for n, chk in sorted(checks.items()):
            A(f"- {n}: {chk['frames']} frame(s), plane md5 {'same' if chk.get('same') else 'DIFFERENT'}")
        A()
    for ln in extra.get("tail", []):
        A(ln)
    if out_md:
        with open(out_md, "w") as f:
            f.write("\n".join(lines) + "\n")
        with open(out_csv, "w", newline="") as f:
            w = csv.DictWriter(f, fieldnames=COLS, extrasaction="ignore")
            w.writeheader()
            for r in csvrows:
                w.writerow({k: r.get(k, "") for k in COLS})
    else:
        print("\n".join(lines))
    return allrows, speed


GATING = {("CIF", "S"): 3, ("720p", "S"): 4, ("720p", "L"): 7, ("1080p", "S"): 3,
          ("1080p", "M"): 3, ("1080p", "L"): 7, ("2160p", "S"): 2,
          ("2160p", "M"): 3, ("2160p", "L"): 6}


def expected(cells, cls, tier):
    return GATING[(cls, tier)]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--res", required=True)
    ap.add_argument("--plans", required=True)
    ap.add_argument("--date", default=datetime.date.today().isoformat())
    ap.add_argument("--arch", default="x86,graviton4,graviton3")
    ap.add_argument("--extra", help="JSON with header/sections/tail per arch")
    ap.add_argument("--write", action="store_true")
    a = ap.parse_args()
    cells, boxes = load_plans(a.plans)
    recs = load_res(a.res)
    extra = json.load(open(a.extra)) if a.extra else {}
    for arch in a.arch.split(","):
        if arch not in recs:
            continue
        nm = {"x86": "x86", "graviton4": "graviton", "graviton3": "graviton3"}[arch]
        md = os.path.join(ROOT, f"docs/parity/PARITY-REPORT-yah265-{nm}-{a.date}.md") if a.write else None
        cv = md[:-3] + ".csv" if md else None
        write(arch, cells, recs[arch], boxes, a.date, md, cv, extra.get(arch, {}))


if __name__ == "__main__":
    main()
