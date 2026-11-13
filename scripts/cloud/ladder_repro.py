#!/usr/bin/env python3
# Copyright (c) 2026, the yah265 authors
# SPDX-License-Identifier: GPL-2.0-or-later
"""ladder_repro.py -- the Mac's BD curves against their re-measurement on a box.

    ladder_repro.py RES.jsonl PLAN.json [--json OUT]

For each curve of the ladder plan: BD-VMAF v1 on the Mac (from
docs/data/parity-spec-ladders.json, the current binaries' rows) and on the
box (ladder_run.py's records), with the per-rung rates and scores, and
whether the two land within 0.2 BD points.
"""
import argparse
import json
import os
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(
    os.path.abspath(__file__))))
sys.path.insert(0, os.path.join(ROOT, "scripts"))
import bdcompare as bd                                    # noqa: E402

OURS_BIN, REF_BIN = "a9b880c8ad668ffa", "2ffa132bb74bb3a5"
TOL = 0.2


def mac_curve(lad, key, mode, rungs):
    def rec(arm, q):
        r = f"{q}" if mode == "crf" else f"k{q}"
        return lad[f"{key}|{mode}|{arm}|{r}|la20|{OURS_BIN if arm == 'ours' else REF_BIN}"]
    x = [rec("ref", q) for q in rungs]
    o = [rec("ours", q) for q in rungs]
    g = lambda r: r[f"v1_{r['v1_gate']}"]                 # noqa: E731
    return ([r["kbps"] for r in x], [g(r) for r in x],
            [r["kbps"] for r in o], [g(r) for r in o],
            [r.get("md5") for r in o], [r.get("md5") for r in x])


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("res")
    ap.add_argument("plan")
    ap.add_argument("--json")
    a = ap.parse_args()
    lad = json.load(open(os.path.join(ROOT, "docs/data/parity-spec-ladders.json")))
    plan = json.load(open(a.plan))
    recs = [json.loads(l) for l in open(a.res)]
    out = []
    for cv in plan["curves"]:
        c, mode, rungs = cv["cell"], cv["mode"], cv["rungs"]
        box = {}
        for r in recs:
            if r.get("cell") == c["name"] and r.get("goal") == f"lad_{mode}" \
                    and r.get("kind") == "run" and "v1" in r:
                box[(r["arm"], r["q"])] = r
        bx = [box.get(("ref", q)) for q in rungs]
        bo = [box.get(("ours", q)) for q in rungs]
        mx_r, mx_s, mo_r, mo_s, mo_md5, mx_md5 = mac_curve(lad, c["key"], mode, rungs)
        mac_bd = bd.bd_rate(mx_r, mx_s, mo_r, mo_s)
        row = {"cell": c["key"], "mode": mode, "rungs": rungs,
               "stride": cv["stride"], "model": cv["model"], "mac_bd": mac_bd}
        if all(bx) and all(bo):
            box_bd = bd.bd_rate([r["kbps"] for r in bx], [r["v1"] for r in bx],
                                [r["kbps"] for r in bo], [r["v1"] for r in bo])
            row.update(box_bd=box_bd, diff=box_bd - mac_bd,
                       within=abs(box_bd - mac_bd) <= TOL,
                       box_ref=[(round(r["kbps"], 1), round(r["v1"], 3)) for r in bx],
                       box_ours=[(round(r["kbps"], 1), round(r["v1"], 3)) for r in bo],
                       mac_ref=[(round(k, 1), round(s, 3)) for k, s in zip(mx_r, mx_s)],
                       mac_ours=[(round(k, 1), round(s, 3)) for k, s in zip(mo_r, mo_s)],
                       ours_md5_same_as_mac=sum(r["md5"] == m for r, m in zip(bo, mo_md5)),
                       ref_md5_same_as_mac=sum(r["md5"] == m for r, m in zip(bx, mx_md5)))
            if mode == "vbr":
                row["track_ours"] = [round(100 * (r["kbps"] / q - 1), 1) for r, q in zip(bo, rungs)]
                row["track_ref"] = [round(100 * (r["kbps"] / q - 1), 1) for r, q in zip(bx, rungs)]
        out.append(row)
        print(f"{c['key']} {mode.upper()}: Mac {mac_bd:+.2f}%  box "
              + (f"{row['box_bd']:+.2f}%  diff {row['diff']:+.2f}  "
                 f"{'within' if row['within'] else 'OUTSIDE'} {TOL}"
                 f"  (ours md5 = Mac on {row['ours_md5_same_as_mac']}/5)"
                 if "box_bd" in row else "incomplete"))
    if a.json:
        json.dump(out, open(a.json, "w"), indent=1)


if __name__ == "__main__":
    main()
