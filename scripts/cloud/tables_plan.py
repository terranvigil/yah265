#!/usr/bin/env python3
# Copyright (c) 2026, the yah265 authors
# SPDX-License-Identifier: GPL-2.0-or-later
"""tables_plan.py -- one box's plan for tables_run.py, from the Mac's data.

Every cell carries the Mac's matched points (docs/data/parity-spec-points.json),
its G1/G2 windows and their points (parity-spec-windows.json), its VBR rung
(parity-spec-vbrwall.csv), and where its frames come from on the box:

  y4m   a tier S clip sent raw (CIF, 720p, 1080p)
  dec   a software decode of a master, from the cell's first master frame,
        under the Mac feeder's Y4M header; the frames equal the Mac's
        (H.264 and HEVC decoding is exact; checked per cell)
  ll    the Mac's own frames, sent lossless by media_pack.py

    tables_plan.py BOX CELLS --hub IP [--goals G1,G2,G3,VBR] [--itype T]
                   [--manifests DIR] [--batch N]
CELLS is a comma list of keys (S/CIF/foreman) or tier/class globs (S/*, L/2160p/*).
"""
import argparse
import csv
import fnmatch
import json
import os
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(
    os.path.abspath(__file__))))
SCR = os.environ.get("CT_SCRATCH", os.path.expanduser("~/src/yah265/local/cloudtab-2026-11-12"))

S_CELLS = [("CIF", "foreman", "foreman_cif"), ("CIF", "bus", "bus_cif"),
           ("CIF", "stefan", "stefan_cif"), ("720p", "ducks", "ducks_720p"),
           ("720p", "park_joy", "park_joy_720p"),
           ("720p", "fourpeople", "fourpeople_720p"),
           ("720p", "shields", "shields_720p"),
           ("1080p", "sunflower", "sunflower_1080p"),
           ("1080p", "pedestrian", "pedestrian_1080p"),
           ("1080p", "riverbed", "riverbed_1080p"),
           ("2160p", "meridian_2160p", None), ("2160p", "bbb_2160p", None)]
MASTER = {"tears": ("m/tearsofsteel_4k.mov", 1440),
          "meridian": ("m/Meridian_UHD4k5994_HDR_P3PQ.mp4", 1798),
          "stem2": ("m/stem2_uhd_hevc_q90.mp4", 5640),
          "sintel": ("m/Sintel.2010.4k.mkv", 4680),
          "bbb": ("m/bbb_sunflower_2160p_30fps_normal.mp4", 900)}
C_CLIPS = [("worldcup", "1080p", "WorldCup_1920x1080_30p.y4m"),
           ("worldcup_far", "1080p", "WorldCup_far_1920x1080_30p.y4m"),
           ("skater227", "1080p", "Skater227_1920x1080_30fps_8bit.y4m"),
           ("boxing", "2160p", "BoxingPractice_4096x2160_60fps_8bit.y4m"),
           ("wikipedia", "1080p", "Wikipedia_1920x1080p30.y4m"),
           ("debugging", "1080p", "Debugging_1920x1080_30fps_8bit_420.y4m"),
           ("spreadsheet", "1080p",
            "Spreadsheet_1920x1080_30fps_8bit_420_130f.y4m")]
# md5 of the whole Mac file (~/media/tier_c_int), checked after the unpack
C_MD5 = {"WorldCup_1920x1080_30p.y4m": "a001d0da92125138093dfb94931c1337",
         "WorldCup_far_1920x1080_30p.y4m": "6d87c49d487c1479d524bdbd2e6549dd",
         "Skater227_1920x1080_30fps_8bit.y4m": "340b38fff4e5c605ba10d840036f4dc2",
         "BoxingPractice_4096x2160_60fps_8bit.y4m":
             "3c7a6b15f1fe0b90d11bff73335ac371",
         "Wikipedia_1920x1080p30.y4m": "73f1a07a9205f4496f7dbd1b0cc3ab10",
         "Debugging_1920x1080_30fps_8bit_420.y4m":
             "894eebe772ff66c44e73ac6db5f64bc2",
         "Spreadsheet_1920x1080_30fps_8bit_420_130f.y4m":
             "c8656a9bd40eb3493cbf8300faa5c5a7"}
# lash1924's master from 38 s, stream-copied (ProRes is all intra), and the
# frames to drop before the excerpt's first (master frame 960, 40 s)
LASH_SPAN = ("ll/lash_38_604.mov", 48)


def jl(path):
    out = {}
    if os.path.exists(path):
        for ln in open(path):
            ln = ln.strip()
            if ln.startswith("{"):
                r = json.loads(ln)
                out[r["name"]] = r
    return out


def catalog(manifests):
    pts = json.load(open(os.path.join(ROOT, "docs/data/parity-spec-points.json")))
    wins = json.load(open(os.path.join(ROOT, "docs/data/parity-spec-windows.json")))
    vbr = {}
    for r in csv.DictReader(open(os.path.join(ROOT, "docs/data/parity-spec-vbrwall.csv"))):
        vbr[f"{r['tier']}/{r['class']}/{r['title']}"] = int(r["k"])
    vt = json.load(open(os.path.join(SCR, "vthdr.json")))
    mac = jl(os.path.join(SCR, "macmd5.jsonl"))
    cells = {}

    def pt(k, a):
        p = pts.get(f"{k}/{a}")
        return p and {"ours_crf": p["ours_crf"], "ref_crf": p["ref_crf"],
                      "ours_bytes": p["ours_bytes"], "ref_bytes": p["ref_bytes"],
                      "ours_md5": p.get("ours_md5"), "bin": p.get("bin")}

    def win(k):
        w = {}
        for g in ("G1", "G2"):
            x = wins.get(f"{k}/{g}")
            if x:
                w[g] = {"spans": x["windows"], "ours_crf": x["ours_crf"],
                        "ref_crf": x["ref_crf"], "ours_md5": x["ours_md5"],
                        "ref_md5": x["ref_md5"]}
        return w or None

    for cls, t, fn in S_CELLS:
        k = f"S/{cls}/{t}"
        p = pts[f"{k}/def"]
        c = {"key": k, "tier": "S", "cls": cls, "title": t,
             "name": f"S_{cls}_{t}", "frames": p["frames"], "fps": p["fps"],
             "points": {"ft1": pt(k, "ft1"), "def": pt(k, "def")},
             "goals": ["G1", "G2", "G3"]}
        if fn:
            c["src"] = {"kind": "y4m", "file": f"s/{fn}.y4m", "skip": 0}
        else:
            nm = "S_bbb_2160p" if t == "bbb_2160p" else "S_meridian_2160p"
            m = mac[nm]
            c["src"] = {"kind": "dec", "file": MASTER["bbb" if t == "bbb_2160p" else "meridian"][0],
                        "skip": 1798 if t == "bbb_2160p" else 1782, "hdr": m["hdr"]}
            c["stage"] = True
            c["check"] = {"plane_md5": m["plane_md5"], "frames": m["frames"]}
        cells[k] = c
    for t in ("chimera", "sollevante", "meridian"):
        for cls in ("1080p", "2160p"):
            k = f"M/{cls}/{t}"
            p = pts[f"{k}/def"]
            c = {"key": k, "tier": "M", "cls": cls, "title": t,
                 "name": f"M_{cls}_{t}", "frames": p["frames"], "fps": p["fps"],
                 "points": {"ft1": pt(k, "ft1"), "def": pt(k, "def")},
                 "windows": win(k), "vbr_k": vbr.get(k),
                 "goals": ["G1", "G2", "G3", "VBR"]}
            if (t, cls) == ("meridian", "2160p"):
                m = mac["M_meridian_2160p"]
                c["src"] = {"kind": "dec", "file": MASTER["meridian"][0],
                            "skip": 3564, "hdr": m["hdr"]}
                c["check"] = {"plane_md5": m["plane_md5"], "frames": m["frames"]}
            else:
                man = os.path.join(manifests, f"M_{t}_{cls}.json")
                if os.path.exists(man):
                    m = json.load(open(man))
                    c["src"] = {"kind": "ll", "file": (
                                    f"ll/M_{t}_{cls}.d" if m.get("chunks")
                                    else f"ll/M_{t}_{cls}.mkv"),
                                "skip": 0, "hdr": m["hdr"]}
                    c["check"] = {"plane_md5": m["plane_md5"], "frames": m["frames"]}
            cells[k] = c
    for t in ("tears", "meridian", "stem2", "sintel", "bbb"):
        k = f"L/2160p/{t}"
        p = pts[f"{k}/def"]
        f, skip = MASTER[t]
        hdr = (mac[f"L_{t}_2160p"]["hdr"] if f"L_{t}_2160p" in mac else
               vt[t]["hdr"])
        c = {"key": k, "tier": "L", "cls": "2160p", "title": t,
             "name": f"L_2160p_{t}", "frames": p["frames"], "fps": p["fps"],
             "points": {"def": pt(k, "def")}, "windows": win(k),
             "vbr_k": vbr.get(k), "goals": ["G1", "G2", "G3", "VBR"],
             "src": {"kind": "dec", "file": f, "skip": skip, "hdr": hdr}}
        if f"L_{t}_2160p" in mac:
            c["check"] = {"plane_md5": mac[f"L_{t}_2160p"]["plane_md5"],
                          "frames": mac[f"L_{t}_2160p"]["frames"]}
        else:
            c["check"] = {"plane_md5": vt[t]["f0_md5"], "frames": 1}
        cells[k] = c
    # cosmos's master is 1920x804: its 1080p cell is a native decode
    k = "L/1080p/cosmos"
    p = pts[f"{k}/def"]
    m = mac["L_cosmos_1080p"]
    cells[k] = {"key": k, "tier": "L", "cls": "1080p", "title": "cosmos",
                "name": "L_1080p_cosmos", "frames": p["frames"], "fps": p["fps"],
                "points": {"def": pt(k, "def")}, "windows": win(k),
                "vbr_k": vbr.get(k), "goals": ["G1", "G2", "G3", "VBR"],
                "src": {"kind": "dec", "file": "m/cosmos_laundromat_1080p.mp4",
                        "skip": 720, "hdr": m["hdr"]},
                "check": {"plane_md5": m["plane_md5"], "frames": m["frames"]}}
    # nocturne 2160p: the master's span as stream-copied ProRes chunks.
    # ffmpeg's ProRes decoder and VideoToolbox's don't agree to the bit
    # (68 dB PSNR on frame 3000), so the box's frames are the Mac's
    # software decode, checked against that, not the Mac feeder's. On x86
    # the feeder runs ffmpeg's C paths (cpuflags 0): its SIMD 10-to-8-bit
    # 4:2:2-to-4:2:0 conversion rounds differently from the C and NEON one.
    man = os.path.join(manifests, "N_nocturne_2160p.json")
    if os.path.exists(man):
        k = "L/2160p/nocturne"
        p = pts[f"{k}/def"]
        cells[k] = {"key": k, "tier": "L", "cls": "2160p", "title": "nocturne",
                    "name": "L_2160p_nocturne", "frames": p["frames"],
                    "fps": p["fps"], "points": {"def": pt(k, "def")},
                    "windows": win(k), "vbr_k": vbr.get(k),
                    "goals": ["G1", "G2", "G3", "VBR"],
                    "src": {"kind": "dec", "file": "ll/N_nocturne_2160p.d",
                            "skip": 0, "hdr": vt["nocturne"]["hdr"],
                            "cpuflags": "0"},
                    "check": {"plane_md5": "d71fbef16aabb6488226b11d2c98a9c4",
                              "frames": 1,
                              "mac_feeder_md5": vt["nocturne"]["f0_md5"]}}
    # tier L 720p and 1080p from the Mac's frames, sent lossless
    for t in ("tears", "meridian", "stem2", "sintel", "bbb", "cosmos",
              "nocturne"):
        for cls in ("720p", "1080p"):
            k = f"L/{cls}/{t}"
            man = os.path.join(manifests, f"L_{t}_{cls}.json")
            if k in cells or not os.path.exists(man):
                continue
            p = pts[f"{k}/def"]
            m = json.load(open(man))
            cells[k] = {"key": k, "tier": "L", "cls": cls, "title": t,
                        "name": f"L_{cls}_{t}", "frames": p["frames"],
                        "fps": p["fps"], "points": {"def": pt(k, "def")},
                        "windows": win(k), "vbr_k": vbr.get(k),
                        "goals": ["G1", "G2", "G3", "VBR"],
                        "src": {"kind": "ll", "file": (
                                    f"ll/L_{t}_{cls}.d" if m.get("chunks")
                                    else f"ll/L_{t}_{cls}.mkv"),
                                "skip": 0, "hdr": m["hdr"]},
                        "check": {"plane_md5": m["plane_md5"],
                                  "frames": m["frames"]}}
    # tier C: the Mac's Y4M clips sent raw (zstd on the wire), and lash1924's
    # 600 s excerpt decoded on the box from a stream-copied ProRes span of
    # the master. That decode is ffmpeg's, not the Mac's, so the lash frames
    # are not the Mac's; both encoders still read the same frames.
    for t, cls, fn in C_CLIPS:
        k = f"C/{cls}/{t}"
        p = pts[f"{k}/def"]
        cells[k] = {"key": k, "tier": "C", "cls": cls, "title": t,
                    "name": f"C_{cls}_{t}", "frames": p["frames"],
                    "fps": p["fps"],
                    "points": {a: pt(k, a) for a in ("ft1", "def")
                               if pts.get(f"{k}/{a}")},
                    "goals": ["G3"],
                    "src": {"kind": "y4m", "file": f"c/{fn}", "skip": 0},
                    "check": {"file_md5": C_MD5.get(fn)}}
    for cls, w, h in (("720p", 1280, 960), ("1080p", 1440, 1080)):
        k = f"C/{cls}/lash1924"
        p = pts[f"{k}/def"]
        src = {"kind": "dec", "file": LASH_SPAN[0], "skip": LASH_SPAN[1],
               "hdr": f"YUV4MPEG2 W{w} H{h} F24:1 Ip A1:1 C420jpeg "
                      "XYSCSS=420JPEG XCOLORRANGE=LIMITED"}
        if cls == "720p":
            src["vf"] = f"scale={w}:{h}"
        cells[k] = {"key": k, "tier": "C", "cls": cls, "title": "lash1924",
                    "name": f"C_{cls}_lash1924", "frames": p["frames"],
                    "fps": p["fps"], "points": {"def": pt(k, "def")},
                    "goals": ["G3"], "rounds": {"G3": 2}, "src": src,
                    "stage": True}
    return cells


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("box")
    ap.add_argument("cells")
    ap.add_argument("--hub", required=True)
    ap.add_argument("--goals")
    ap.add_argument("--itype", default="")
    ap.add_argument("--manifests", default=os.path.join(SCR, "manifests"))
    ap.add_argument("--batch", type=int, default=0)
    ap.add_argument("--out")
    ap.add_argument("--ladder", help="curves: KEY:crf|vbr,...")
    ap.add_argument("--stage", action="store_true",
                    help="decode the whole cell to a raw Y4M on the box first "
                         "and feed it by block copy (a lossless source whose "
                         "decode can't keep 1.5x ahead of the encoders)")
    ap.add_argument("--resolve", action="store_true",
                    help="solve ours onto this box's x265 bytes first; the "
                         "cells are renamed NAME_r and supersede NAME")
    a = ap.parse_args()
    cat = catalog(a.manifests)
    if a.ladder:
        lad = json.load(open(os.path.join(ROOT, "docs/data/parity-spec-ladders.json")))
        curves = []
        for spec in a.ladder.split(","):
            k, mode = spec.split(":")
            c = cat[k]
            if mode == "crf":
                rungs = [20, 23, 26, 29, 32]
            else:
                rungs = []
                for q in (20, 23, 26, 29, 32):
                    r = [v for kk, v in lad.items() if kk.startswith(f"{k}|crf|ref|{q}|la20|")]
                    rungs.append(int(round(r[-1]["kbps"])))
            gate = "1d5h_2160" if c["cls"] == "2160p" else "3d0h"
            model = (f"vmaf_v1.0.16_hfr/vmaf_v1.0.16_hfr_{gate}.json"
                     if c["fps"] >= 49.9 else f"vmaf_v1.0.16/vmaf_v1.0.16_{gate}.json")
            curves.append({"cell": c, "mode": mode, "rungs": rungs,
                           "stride": 3 if c["tier"] == "L" else 1,
                           "model": model})
        plan = {"box": a.box, "hub": a.hub, "itype": a.itype, "curves": curves,
                "cells": [cv["cell"] for cv in curves]}
        out = a.out or os.path.join(SCR, f"plan_{a.box}.json")
        json.dump(plan, open(out, "w"), indent=1)
        print(out, [(cv["cell"]["key"], cv["mode"], cv["rungs"]) for cv in curves])
        return
    sel = []
    for pat in a.cells.split(","):
        for k in cat:
            if fnmatch.fnmatch(k, pat) and k not in [c["key"] for c in sel]:
                sel.append(cat[k])
    for c in sel:
        if a.stage:
            c["stage"] = True
            c["name"] += "_s"
        if a.resolve:
            c["resolve"] = True
            c["name"] += "_r"
        if "src" not in c:
            sys.exit(f"{c['key']}: no source on the hub yet")
        if a.goals:
            c["goals"] = [g for g in c["goals"] if g in a.goals.split(",")]
    plan = {"box": a.box, "hub": a.hub, "itype": a.itype, "cells": sel}
    if a.batch:
        names = [c["name"] for c in sel]
        plan["batches"] = [names[i:i + a.batch] for i in range(0, len(names), a.batch)]
    files = sorted({c["src"]["file"] for c in sel})
    out = a.out or os.path.join(SCR, f"plan_{a.box}.json")
    json.dump(plan, open(out, "w"), indent=1)
    print(out)
    print("files:", " ".join(files))


if __name__ == "__main__":
    main()
