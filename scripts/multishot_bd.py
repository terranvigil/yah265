#!/usr/bin/env python3
# Copyright (c) 2026, the yah265 authors
# SPDX-License-Identifier: GPL-2.0-or-later
# Ported from yah264 (codec-agnostic instrument; the encoder binary, its flags
# and the reference encoder are the only changes).
"""BD-rate on the multi-shot sequences: the across-shot allocation gate.

The ten-clip board is single-shot by construction and cannot see how bits
move between shots; this encodes the S0 multi-shot sequences
(local/corpus/<seq>.y4m with a <seq>.cuts file: "<first frame> <name>" per
shot) at several CRF points for every arm and reports, per sequence,
BD-v1 for every arm pair, the per-shot VMAF v1 at each point and the
per-shot bit share. Arms are "name=<command template>" with {src} {q} {out};
"name=env:VAR=val ..." runs our binary (BIN, default build/y265) under that
environment with --our-args (default `--crf {q} --aq 1 --cutree 1`, the band's
operating point), "x265" runs the reference at medium.

  scripts/multishot_bd.py --arms flat=env: aqdc=env:N265_AQ_DC=0.4 x265 \\
      --seqs ms_cif_30,ms_720p_50,ms_1080p_25 --points 22,26,30,34

v1 is scored by scripts/v1score.py, the parity harness's scorer: every
frame, at display size (fit to 1920x1080 below 1080p), on the model the
source picks. A shot's score is the mean of the whole-file per-frame array
over its frames, as shot_hull.py reads it. Encodes and per-frame v1 files
are cached under --work by (seq, arm, q); use a fresh --work after a
rebuild. BD sign: negative = the first arm of the pair spends fewer bits
at equal v1.
"""
import argparse, json, math, os, shlex, subprocess, sys
import numpy as np

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "scripts"))
import v1score as v1s  # noqa: E402


def bd_rate(r_ref, m_ref, r_test, m_test):
    deg = min(3, len(m_ref) - 1)
    f1 = np.polyfit(m_ref, np.log(r_ref), deg)
    f2 = np.polyfit(m_test, np.log(r_test), deg)
    lo, hi = max(min(m_ref), min(m_test)), min(max(m_ref), max(m_test))
    if hi <= lo:
        return None
    xs = np.linspace(lo, hi, 100)
    return (math.exp(np.mean(np.polyval(f2, xs) - np.polyval(f1, xs))) - 1) * 100


sizes = v1s.frame_sizes


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--arms", nargs="+", required=True)
    ap.add_argument("--seqs", default="ms_cif_30,ms_720p_50,ms_1080p_25")
    ap.add_argument("--points", default="22,26,30,34")
    ap.add_argument("--corpus", default=os.path.join(ROOT, "local", "corpus"))
    ap.add_argument("--work", default=os.path.join(ROOT, "local", "multishot_work"))
    ap.add_argument("--x265", dest="x265", default=os.environ.get("X265", "x265"))
    ap.add_argument("--threads", default="0", help="our --threads (0 = auto)")
    ap.add_argument("--vmaf-threads", dest="vmaf_threads",
                    default=os.environ.get("VMAF_THREADS", "4"))
    ap.add_argument("--our-args", dest="our_args", default="--crf {q} --aq 1 --cutree 1",
                    help="the rate-control flags the env: arms encode with. The default is "
                         "the band's CRF operating point; the across-shot term this gate "
                         "measures (N265_AQ_DC) only exists under CRF, and CQP would make "
                         "every arm here byte-identical. '--qp {q}' restores the CQP form.")
    a = ap.parse_args()
    ours = os.environ.get("BIN", os.path.join(ROOT, "build", "y265"))
    os.makedirs(a.work, exist_ok=True)
    pts = [int(x) for x in a.points.split(",")]
    arms = {}
    for spec in a.arms:
        name, _, tmpl = spec.partition("=")
        if name == "x265" and not tmpl:
            tmpl = f"{shlex.quote(a.x265)} --preset medium --crf {{q}} --pools none --frame-threads 1 --log-level none -o {{out}} {{src}}"
        elif tmpl.startswith("env:"):
            env = tmpl[4:].strip()
            tmpl = (f"env {env} " if env else "") + f"{shlex.quote(ours)} {{src}} {a.our_args} -o {{out}}"
        arms[name] = tmpl
    for seq in a.seqs.split(","):
        src = os.path.join(a.corpus, seq + ".y4m")
        cuts = [ln.split() for ln in open(os.path.join(a.corpus, seq + ".cuts")) if ln.strip()]
        names = [c[1] if len(c) > 1 else f"shot{i}" for i, c in enumerate(cuts)]
        cuts = [int(c[0]) for c in cuts]
        res = {}
        for arm, tmpl in arms.items():
            for q in pts:
                out = os.path.join(a.work, f"{seq}_{arm}_{q}.hevc")
                js = os.path.join(a.work, f"{seq}_{arm}_{q}.v1.json")
                if not os.path.exists(js):
                    cmd = tmpl.format(src=shlex.quote(src), q=q, out=shlex.quote(out))
                    r = subprocess.run(cmd, shell=True, capture_output=True, text=True)
                    if r.returncode:
                        sys.exit(f"encode failed: {cmd}\n{r.stderr[-400:]}")
                    pf, info = v1s.per_frame(src, out, a.vmaf_threads, cache=False)
                    with open(js, "w") as f:
                        json.dump({"frames": pf, **info}, f)
                per = json.load(open(js))["frames"]
                sz = sizes(out)
                n = len(per); b = cuts + [n]
                shots = [float(np.mean(per[b[i]:b[i + 1]])) for i in range(len(cuts))]
                share = [sum(sz[b[i]:b[i + 1]]) / max(1, sum(sz)) * 100 for i in range(len(cuts))]
                res[(arm, q)] = (os.path.getsize(out) * 8, float(np.mean(per)), shots, share)
                print(f"{seq:12s} {arm:8s} crf{q}: {res[(arm, q)][0] / 1000:9.0f} kbit  v1 {res[(arm, q)][1]:6.2f}"
                      f"  shots " + " ".join(f"{s:5.1f}" for s in shots)
                      + "  share% " + " ".join(f"{s:4.1f}" for s in share), flush=True)
        print(f"{seq}: shots = " + ", ".join(names))
        arm_names = list(arms)
        for i, t in enumerate(arm_names):
            for r in arm_names[i + 1:]:
                rr = [res[(r, q)][0] for q in pts]; mr = [res[(r, q)][1] for q in pts]
                rt = [res[(t, q)][0] for q in pts]; mt = [res[(t, q)][1] for q in pts]
                bd = bd_rate(rr, mr, rt, mt)
                print(f"BD-v1 {seq} {t} vs {r}: " + (f"{bd:+.2f}%" if bd is not None else "no overlap"), flush=True)


if __name__ == "__main__":
    main()
