#!/usr/bin/env python3
# Copyright (c) 2026, the yah265 authors
# SPDX-License-Identifier: GPL-2.0-or-later
"""tierc_report.py -- the tier C (content types) G3 tables from the cloud
records, as the section the x86 and Graviton reports carry.

The ratios come from blog_charts.content_cloud, the read the site uses,
so the report and the results page can't part.

    tierc_report.py RECORDS_DIR x86|graviton
"""
import json
import os
import statistics as st
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path.insert(0, os.path.join(ROOT, "scripts"))
import blog_charts as B                                   # noqa: E402

ORDER = ["lash1924_720p", "lash1924_1080p", "worldcup_1080p", "worldcup_far_1080p",
         "skater227_1080p", "boxing_2160p", "wikipedia_1080p", "debugging_1080p",
         "spreadsheet_1080p"]


def box_table(rec_dir, box, lab, cc):
    path = os.path.join(rec_dir, f"{box}.jsonl")
    walls = B.content_walls(path)
    solve, feed = {}, {}
    for ln in open(path):
        r = json.loads(ln)
        if r.get("rc") != 0:
            continue
        cell = r.get("cell", "").removesuffix("_r")
        if r.get("kind") == "solve" and r.get("anchor") == "def":
            solve[cell] = r
        elif r.get("goal") == "G3" and r.get("arm") == "feed":
            feed[cell] = r["feed_fps"]
    walls = {(c.removesuffix("_r"), a): v for (c, a), v in walls.items()}
    x86 = any(a == "ref512" for _, a in walls)
    hdr = ["cell", f"{lab} ours s", "x265 s", f"{lab} ratio"]
    if x86:
        hdr += ["x265 --asm avx512 s", "ratio vs avx512"]
    hdr += ["dsize", "ours crf (solved here)", "feed fps", "rounds", "valid"]
    out = ["| " + " | ".join(hdr) + " |", "|" + "---|" * len(hdr)]
    missing = []
    for name in ORDER:
        title, cls = name.rsplit("_", 1)
        cell = f"C_{cls}_{title}"
        ours, ref = walls.get((cell, "ours"), []), walls.get((cell, "ref"), [])
        if not B.paired(ours, ref):
            missing.append(name)
            out.append("| " + " | ".join([name] + ["unmeasured"] * (len(hdr) - 2) + ["no"]) + " |")
            continue
        mo = st.median(w for w, _ in ours)
        mr = st.median(w for w, _ in ref)
        ds = 100 * (st.median(b for _, b in ours) / st.median(b for _, b in ref) - 1)
        got = cc[box].get((cls, title))
        row = [name, f"{mo:.2f}", f"{mr:.2f}", f"{got[0]:.2f}" if got else "-"]
        if x86:
            r5 = walls.get((cell, "ref512"))
            row += [f"{st.median(w for w, _ in r5):.2f}" if r5 else "-",
                    f"{got[1]:.2f}" if got and got[1] is not None else "-"]
        sv = solve.get(cell, {})
        row += [f"{ds:+.2f}%", f"{sv.get('ours_crf', '-')}", f"{feed.get(cell, '-')}",
                str(len(ours)), "yes" if got else "size-mismatch"]
        out.append("| " + " | ".join(row) + " |")
    return out, missing


def main():
    rec_dir, which = sys.argv[1], sys.argv[2]
    cc = B.content_cloud(rec_dir)
    boxes = [(b, lab) for b, lab in B.CONTENT_BOXES
             if (b == "g4C") == (which == "graviton")]
    lines = []
    for b, lab in boxes:
        table, missing = box_table(rec_dir, b, lab, cc)
        rs = {f"{t}_{c}": v[0] for (c, t), v in cc[b].items()}
        worst = max(rs, key=rs.get)
        head = (f"{lab}: median {st.median(rs.values()):.2f}, worst {rs[worst]:.2f} "
                f"({worst}), over {len(rs)} clips.")
        if missing:
            head += f" Unmeasured: {', '.join(missing)}."
        lines += [head, ""] + table + [""]
    print("\n".join(lines))


if __name__ == "__main__":
    main()
