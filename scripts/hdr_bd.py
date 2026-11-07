#!/usr/bin/env python3
# Copyright (c) 2026, the yah265 authors
# SPDX-License-Identifier: GPL-2.0-or-later
"""HDR CRF ladders scored with HDR-VMAF, and BD-rate between them.

    scripts/hdr_bd.py enc ARM --kind n265|x265 [--bin PATH] \
        [--env K=V ...] [--args "..."] --clip sol_1080 --crfs 11,14,17,20,23
    scripts/hdr_bd.py bd --ref ARM --test ARM [--test ARM ...] [--clip ...]

Every rung is encoded, decoded with ffmpeg and scored with HDR-VMAF
v1.1.8 (the model and colour flags yah264's HDR page used: 3 picture
heights at 1080p, 1.5 at 2160p, the high-frame-rate twin for Sparks),
with PSNR-Y from the same libvmaf run. A rung is kept beside the command
that made it, the binary's hash included, and redone only when that
changes. HLG clips are scored as PQ: HDR-VMAF has no HLG transfer, so the
reference and every decoded picture go through the same HLG -> PQ
conversion (zscale, 1000-nit nominal peak) before scoring.

Environment: VMAF_HDR_BIN (default ~/.local/opt/vmaf-hdr/bin/vmaf),
VMAF_HDR_MODELS (~/.local/share/vmaf-hdr), HDR_WORK (the ladders,
<staging>/yah265/hdr/item56), THREADS (6).

bd prints BD-rate on HDR-VMAF and on PSNR-Y, negative where the test
spends fewer bits at equal score, over the score range both ladders
cover. Rungs are the arm's own; pick each arm's CRFs so its score range
covers the reference's.
"""
import argparse
import hashlib
import json
import os
import shlex
import subprocess
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import boxlib                                             # noqa: E402
from bdcompare import bd_rate                             # noqa: E402

HOME = os.path.expanduser("~")
VMAF = os.environ.get("VMAF_HDR_BIN", f"{HOME}/.local/opt/vmaf-hdr/bin/vmaf")
MODELS = os.environ.get("VMAF_HDR_MODELS", f"{HOME}/.local/share/vmaf-hdr")
WORK = os.environ.get("HDR_WORK", "<staging>/yah265/hdr/item56")
THREADS = os.environ.get("THREADS", "6")
SRC = "<staging>/yah264/hdr"
HLG = f"{WORK}/src"
# both titles were graded on a 1000-nit P3 D65 display
MD = "G(13250,34500)B(7500,3000)R(34000,16000)WP(15635,16450)L(10000000,1)"

# name: source, frames, fps, transfer, MaxCLL/MaxFALL
CLIPS = {
    "sol_1080": (f"{SRC}/sol_1080.y4m", 240, 24.0, "pq", "1170,1022"),
    "sol_2160": (f"{SRC}/sol_2160.y4m", 240, 24.0, "pq", "1170,1022"),
    "sparks_1080": (f"{SRC}/sparks_1080.y4m", 300, 60000 / 1001, "pq", "1000,219"),
    "sparks_2160": (f"{SRC}/sparks_2160.y4m", 300, 60000 / 1001, "pq", "1000,219"),
    "solhlg_1080": (f"{HLG}/solhlg_1080.y4m", 240, 24.0, "hlg", None),
    "sparkshlg_1080": (f"{HLG}/sparkshlg_1080.y4m", 300, 60000 / 1001, "hlg", None),
}

HLG2PQ = ("zscale=tin=arib-std-b67:min=bt2020nc:pin=bt2020:rin=tv:t=linear:"
          "npl=1000:p=bt2020,format=gbrpf32le,zscale=t=smpte2084:"
          "m=bt2020nc:r=tv:p=bt2020,format=yuv420p10le")


def colour(trc):
    tf = "smpte2084" if trc == "pq" else "arib-std-b67"
    return ["--colorprim", "bt2020", "--transfer", tf, "--colormatrix",
            "bt2020nc", "--range", "limited"]


def model(clip):
    hfr = clip.startswith("sparks")
    d = "hdr_vmaf_v1.1.8" + ("_hfr" if hfr else "")
    base = "hdr_vmaf_v1.1.8" + ("_hfr" if hfr else "")
    tail = "_1d5h_2160" if clip.endswith("2160") else "_3d0h"
    return f"{MODELS}/{d}/{base}{tail}.json"


def sha(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for b in iter(lambda: f.read(1 << 20), b""):
            h.update(b)
    return h.hexdigest()


def enc_cmd(a, clip, q, out):
    src, frames, fps, trc, cll = CLIPS[clip]
    col = colour(trc)
    extra = shlex.split(a.args or "")
    if a.kind == "n265":
        cmd = [a.bin, src, "-o", out, "--preset", "medium", "--crf", str(q),
               "--threads", THREADS] + col
        if trc == "pq":
            cmd += ["--master-display", MD, "--max-cll", cll]
    else:
        cmd = [a.bin, "--input", src, "-o", out, "--preset", "medium",
               "--crf", str(q), "--pools", THREADS, "--frame-threads", "4",
               "--output-depth", "10", "--profile", "main10"] + col
        if trc == "pq":
            cmd += ["--master-display", MD, "--max-cll", cll, "--hdr10"]
        cmd += ["--log-level", "error"]
    return cmd + extra


def pq_ref(clip):
    """The PQ reference HDR-VMAF scores against."""
    src, frames, fps, trc, cll = CLIPS[clip]
    if trc == "pq":
        return src
    out = f"{HLG}/{clip}_aspq.y4m"
    if not os.path.exists(out):
        dec(src, out, HLG2PQ)
    return out


def dec(inp, out, vf=None):
    cmd = ["ffmpeg", "-v", "error", "-nostdin", "-xerror", "-i", inp]
    if vf:
        cmd += ["-vf", vf]
    cmd += ["-pix_fmt", "yuv420p10le", "-strict", "-1", "-f",
            "yuv4mpegpipe", "-y", out + ".tmp"]
    subprocess.run(cmd, check=True)
    os.replace(out + ".tmp", out)


def score(clip, hevc, out_json):
    src, frames, fps, trc, cll = CLIPS[clip]
    y = hevc + ".y4m"
    dec(hevc, y, HLG2PQ if trc == "hlg" else None)
    vc = []
    for s in ("ref", "dist"):
        vc += [f"--color_range_{s}", "limited", f"--color_primaries_{s}",
               "bt2020", f"--color_trc_{s}", "pq", f"--color_matrix_{s}",
               "bt2020nc"]
    cmd = [VMAF, "--reference", pq_ref(clip), "--distorted", y, "--threads",
           THREADS, *vc, "--model", f"path={model(clip)}", "--feature",
           "psnr", "--json", "--output", out_json + ".tmp"]
    try:
        subprocess.run(cmd, check=True, stdout=subprocess.DEVNULL,
                       stderr=subprocess.DEVNULL)
    finally:
        os.path.exists(y) and os.unlink(y)
    j = json.load(open(out_json + ".tmp"))
    if len(j["frames"]) != frames:
        sys.exit(f"{out_json}: {len(j['frames'])} frames, want {frames}")
    os.replace(out_json + ".tmp", out_json)


def point(arm, clip, q):
    base = f"{WORK}/{arm}/{clip}_crf{q}"
    j = json.load(open(base + ".json"))
    m = j["pooled_metrics"]
    src, frames, fps, trc, cll = CLIPS[clip]
    kbps = os.path.getsize(base + ".hevc") * 8 / (frames / fps) / 1000
    cambi = next(v for k, v in m.items() if k.startswith("cambi"))
    meta = json.load(open(base + ".cmd"))
    return dict(q=q, kbps=kbps, vmaf=m["vmaf"]["mean"],
                psnr=m["psnr_y"]["mean"], cambi=cambi["mean"],
                cambi_max=cambi["max"], instr=meta.get("instr"))


def cmd_enc(a):
    a.bin = a.bin or ("x265" if a.kind == "x265" else "build-rel/n265")
    a.bin = os.path.abspath(a.bin) if os.sep in a.bin else \
        subprocess.run(["which", a.bin], capture_output=True,
                       text=True).stdout.strip()
    binsha = sha(a.bin)
    d = f"{WORK}/{a.arm}"
    os.makedirs(d, exist_ok=True)
    env = dict(os.environ)
    envs = dict(kv.split("=", 1) for kv in a.env)
    env.update(envs)
    for clip in a.clip:
        for q in [int(x) for x in a.crfs.split(",")]:
            base = f"{d}/{clip}_crf{q}"
            cmd = enc_cmd(a, clip, q, base + ".hevc")
            key = dict(cmd=cmd, env=envs, bin=binsha)
            try:
                old = json.load(open(base + ".cmd"))
                if {k: old.get(k) for k in key} == key and \
                        os.path.exists(base + ".json"):
                    continue
            except (OSError, ValueError):
                pass
            boxlib.wait_untimed("hdr_bd")
            r = boxlib.measure(cmd, env=env)
            if r["rc"] != 0:
                sys.exit(f"encode failed: {shlex.join(cmd)}")
            score(clip, base + ".hevc", base + ".json")
            key["instr"] = r["instr"]
            json.dump(key, open(base + ".cmd", "w"))
            p = point(a.arm, clip, q)
            print(f"{a.arm:14s} {clip:14s} crf{q:<3d} {p['kbps']:9.0f} kbps "
                  f"vmaf {p['vmaf']:6.2f} psnr {p['psnr']:6.2f} "
                  f"cambi {p['cambi']:.2f}/{p['cambi_max']:.2f}", flush=True)


def ladder(arm, clip):
    d = f"{WORK}/{arm}"
    pts = []
    if not os.path.isdir(d):
        return pts
    for f in os.listdir(d):
        if f.startswith(clip + "_crf") and f.endswith(".json"):
            q = int(f[len(clip) + 4:-5])
            pts.append(point(arm, clip, q))
    return sorted(pts, key=lambda p: p["kbps"])


def cmd_bd(a):
    clips = a.clip or list(CLIPS)
    for clip in clips:
        ref = ladder(a.ref, clip)
        if len(ref) < 4:
            continue
        for t in a.test:
            tp = ladder(t, clip)
            if len(tp) < 4:
                continue
            row = [f"{clip:14s} {t:14s} vs {a.ref:12s}"]
            for m in ("vmaf", "psnr"):
                b = bd_rate([p["kbps"] for p in ref], [p[m] for p in ref],
                            [p["kbps"] for p in tp], [p[m] for p in tp])
                lo = max(min(p[m] for p in ref), min(p[m] for p in tp))
                hi = min(max(p[m] for p in ref), max(p[m] for p in tp))
                span = (hi - lo) / (max(p[m] for p in ref) -
                                    min(p[m] for p in ref))
                row.append(f"BD-{m} {b:+7.2f}% ({span * 100:3.0f}% of ref)"
                           if b is not None else f"BD-{m}    n/a")
            ri = sum(p["instr"] or 0 for p in ref)
            ti = sum(p["instr"] or 0 for p in tp)
            if ri and ti and a.instr:
                row.append(f"instr {ti / ri:.3f}x")
            print("  ".join(row))


def cmd_show(a):
    for clip in a.clip or list(CLIPS):
        for arm in a.arm:
            for p in ladder(arm, clip):
                print(f"{arm:14s} {clip:14s} crf{p['q']:<3d} {p['kbps']:9.0f}"
                      f" kbps vmaf {p['vmaf']:6.2f} psnr {p['psnr']:6.2f} "
                      f"cambi {p['cambi']:.2f}/{p['cambi_max']:.2f}")


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    sub = ap.add_subparsers(dest="cmd", required=True)
    e = sub.add_parser("enc")
    e.add_argument("arm")
    e.add_argument("--kind", choices=("n265", "x265"), default="n265")
    e.add_argument("--bin")
    e.add_argument("--env", action="append", default=[])
    e.add_argument("--args")
    e.add_argument("--clip", action="append", required=True,
                   choices=list(CLIPS))
    e.add_argument("--crfs", required=True)
    b = sub.add_parser("bd")
    b.add_argument("--ref", required=True)
    b.add_argument("--test", action="append", required=True)
    b.add_argument("--clip", action="append", choices=list(CLIPS))
    b.add_argument("--instr", action="store_true",
                   help="also print the summed instruction ratio")
    s = sub.add_parser("show")
    s.add_argument("arm", nargs="+")
    s.add_argument("--clip", action="append", choices=list(CLIPS))
    a = ap.parse_args()
    {"enc": cmd_enc, "bd": cmd_bd, "show": cmd_show}[a.cmd](a)


if __name__ == "__main__":
    main()
