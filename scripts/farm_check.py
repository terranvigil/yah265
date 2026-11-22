#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""The farm mode gate: scripts/farm.py's output against --gop-parallel's.

For each clip it encodes the reference with
`--gop-parallel W --threads W*K` and the farm with W local workers at
--seg-threads K, and cmps the two streams and the two segment tables.
With --faults it also kills one worker mid-segment, hangs one past
--timeout, and resumes after a damaged segment file, and each of those
runs must give the same bytes.

Without --clip it makes a synthetic 352x288 clip with a scene cut, so
`make test` needs no media and no ffmpeg.

    scripts/farm_check.py --y265 build/y265 --faults
    scripts/farm_check.py --clip foreman_cif.y4m --gop-len 64 --faults
"""

import argparse
import filecmp
import json
import os
import random
import shutil
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
FARM = os.path.join(HERE, "farm.py")

KILLER = """#!/bin/sh
# kill the first segment this worker is handed, mid-encode
case " $* " in *" --farm-seg "*)
    if mkdir "{mark}" 2>/dev/null; then
        "$@" 0<&0 &
        pid=$!
        sleep {delay}
        if kill -9 $pid 2>/dev/null; then touch "{mark}/alive"; fi
        wait $pid
        exit 137
    fi ;;
esac
exec "$@"
"""

HANGER = """#!/bin/sh
# hang on the first segment this worker is handed
case " $* " in *" --farm-seg "*)
    if mkdir "{mark}" 2>/dev/null; then exec sleep 600; fi ;;
esac
exec "$@"
"""


def synth(path, w=352, h=288, n=96, cut=58):
    """two textured scenes panning at different speeds"""
    rng = random.Random(265)

    def world(seed, ww, hh, step):
        r = random.Random(seed)
        rows = []
        for y in range(hh):
            noise = r.randbytes(ww)
            rows.append(bytes(((x * step + y * 3 + (x * y >> 5) +
                                (noise[x] & 15)) & 255) for x in range(ww)))
        return rows
    worlds = [world(1, w + n * 2, h + n, 2), world(2, w + n * 2, h + n, 5)]
    cw, ch = w // 2, h // 2
    with open(path, "wb") as f:
        f.write(b"YUV4MPEG2 W%d H%d F25:1 Ip A1:1 C420jpeg\n" % (w, h))
        for i in range(n):
            sc = 1 if i >= cut else 0
            wl = worlds[sc]
            dx, dy = (2 * i, i // 2) if sc == 0 else (i, i)
            f.write(b"FRAME\n")
            for y in range(h):
                f.write(wl[y + dy][dx:dx + w])
            u = bytes([96 + 40 * sc + (rng.random() < 0.5)]) * (cw * ch)
            f.write(u)
            f.write(bytes([160 - 30 * sc]) * (cw * ch))


def run(cmd, env, log):
    with open(log, "w") as lf:
        r = subprocess.run(cmd, env=env, stdout=lf, stderr=subprocess.STDOUT)
    return r.returncode


def trace_plan(log):
    segs = []
    with open(log) as f:
        for ln in f:
            p = ln.strip().split(",")
            if len(p) == 5 and p[0] == "GOP" and p[1].isdigit():
                segs.append((int(p[2]), int(p[3])))
    return segs


def check(a, clip, tmp, tag):
    W, K = a.workers, a.seg_threads
    env = dict(os.environ, N265_GOP_LEN=str(a.gop_len))
    opts = a.opts.split()
    fails = []
    if a.shots:
        shots = os.path.join(tmp, f"{tag}.shots")
        if subprocess.run([a.y265, clip, "--shot-scan", shots],
                          capture_output=True).returncode:
            return [f"{tag}: --shot-scan failed"]
        opts += ["--shot-list", shots]
    if a.no_scenecut:
        opts += ["--no-scenecut", "--keyint", str(max(a.gop_len // 2, 8))]
    ref = os.path.join(tmp, f"{tag}.ref.hevc")
    rc = run([a.y265, clip, "-o", ref] + opts +
             ["--gop-parallel", str(W), "--threads", str(W * K)],
             dict(env, N265_GOP_TRACE="1"), ref + ".log")
    if rc:
        return [f"{tag}: reference encode failed (exit {rc})"]
    with open(ref + ".log") as f:
        m = [ln for ln in f if "gop-parallel:" in ln]
    print(f"{tag}: reference {os.path.getsize(ref)} bytes, "
          f"{m[-1].split('gop-parallel: ')[1].strip() if m else '?'}")

    plan = subprocess.run([a.y265, clip] + opts + ["--farm-plan"], env=env,
                          capture_output=True, text=True)
    segs = [tuple(map(int, ln.split()[2:4])) for ln in plan.stdout.split("\n")
            if ln.startswith("seg ")]
    tsegs = trace_plan(ref + ".log")
    # the trace names every segment but the last
    if plan.returncode or segs[:-1] != tsegs:
        fails.append(f"{tag}: plan {segs} is not --gop-parallel's {tsegs}")
    if len(segs) < 2:
        fails.append(f"{tag}: one segment only; lower --gop-len")

    def farm(name, workers, extra=()):
        out = os.path.join(tmp, f"{tag}.{name}.hevc")
        cmd = [sys.executable, FARM, "-o", out, "--y265", a.y265,
               "--seg-threads", str(K)] + \
              sum([["--worker", w] for w in workers], []) + list(extra) + \
              ["--", clip] + opts
        rc = run(cmd, env, out + ".log")
        same = rc == 0 and filecmp.cmp(ref, out, shallow=False)
        print(f"{tag}: farm {name}: exit {rc}, "
              f"{'IDENTICAL' if same else 'DIFFERS'}")
        if not same:
            fails.append(f"{tag}: farm {name} (see {out}.log)")
        return out

    farm("plain", ["local"] * W)
    if not a.faults:
        return fails

    mark = os.path.join(tmp, f"{tag}.killmark")
    killer = os.path.join(tmp, f"{tag}.killer.sh")
    with open(killer, "w") as f:
        f.write(KILLER.format(mark=mark, delay=a.kill_delay))
    os.chmod(killer, 0o755)
    out = farm("kill", [f"cmd:{killer}"] + ["local"] * (W - 1))
    with open(out + ".log") as f:
        retried = sum("failed on cmd:" in ln for ln in f)
    alive = os.path.exists(os.path.join(mark, "alive"))
    print(f"{tag}: kill: {retried} failed try, "
          f"{'killed mid-segment' if alive else 'segment had ended'}")
    if retried != 1:
        fails.append(f"{tag}: kill: expected one failed try, saw {retried}")

    mark = os.path.join(tmp, f"{tag}.hangmark")
    hanger = os.path.join(tmp, f"{tag}.hanger.sh")
    with open(hanger, "w") as f:
        f.write(HANGER.format(mark=mark))
    os.chmod(hanger, 0o755)
    farm("hang", [f"cmd:{hanger}"] + ["local"] * (W - 1),
         ["--timeout", str(a.hang_timeout)])

    # resume: keep the state, damage one segment, run again
    state = os.path.join(tmp, f"{tag}.resume.farm")
    farm("resume1", ["local"] * W, ["--keep", "--state", state])
    seg1 = os.path.join(state, "seg00001.hevc")
    with open(seg1, "r+b") as f:
        f.seek(40)
        f.write(b"\xff\xff")
    farm("resume2", ["local"] * W, ["--keep", "--state", state])
    with open(os.path.join(state, "state.json")) as f:
        st = json.load(f)["segs"]
    redone = [k for k, v in st.items() if v["attempts"] > 0]
    print(f"{tag}: resume: segments recoded {redone}")
    if redone != ["1"]:
        fails.append(f"{tag}: resume recoded {redone}, expected ['1']")
    return fails


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawTextHelpFormatter)
    ap.add_argument("--y265", default=os.path.join(os.path.dirname(HERE),
                                                   "build", "y265"))
    ap.add_argument("--clip", action="append", default=[])
    ap.add_argument("--workers", type=int, default=2, help="W (default 2)")
    ap.add_argument("--seg-threads", type=int, default=2,
                    help="K (default 2)")
    ap.add_argument("--gop-len", type=int, default=32,
                    help="N265_GOP_LEN for both runs (default 32)")
    ap.add_argument("--opts", default="--crf 28",
                    help="y265 options for both runs (default --crf 28)")
    ap.add_argument("--faults", action="store_true")
    ap.add_argument("--no-scenecut", action="store_true",
                    help="both runs on the --keyint grid, keyint half of "
                         "--gop-len")
    ap.add_argument("--shots", action="store_true",
                    help="scan the clip for shots and pass --shot-list to "
                         "both runs")
    ap.add_argument("--kill-delay", type=float, default=0.3)
    ap.add_argument("--hang-timeout", type=float, default=20)
    ap.add_argument("--keep", action="store_true",
                    help="keep the work directory")
    a = ap.parse_args()
    a.y265 = os.path.abspath(a.y265)
    tmp = tempfile.mkdtemp(prefix="farm_check.")
    fails = []
    try:
        clips = a.clip
        if not clips:
            clips = [os.path.join(tmp, "synth.y4m")]
            synth(clips[0])
        for c in clips:
            tag = os.path.splitext(os.path.basename(c))[0]
            fails += check(a, os.path.abspath(c), tmp, tag)
    finally:
        if a.keep or fails:
            print(f"work directory: {tmp}")
        else:
            shutil.rmtree(tmp, ignore_errors=True)
    for f in fails:
        print(f"FAIL {f}")
    print("farm_check: " + ("FAIL" if fails else "PASS"))
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
