#!/usr/bin/env python3
# Copyright (c) 2026, the yah265 authors
# SPDX-License-Identifier: GPL-2.0-or-later
"""ladder_run.py -- a sample of the Mac's BD ladders, re-measured on a box.

The x86 and arm64 builds write slightly different streams (floating-point
contraction), so x86 quality is shown by reproduction: the Mac's curves,
both encoders re-encoded here at the Mac's rungs (CRF 20-32; VBR at the
Mac's targets, maxrate 1.5x, buffer 2x), 6 threads each (x265 --pools 6
--frame-threads 4), scored with VMAF v1's gate model exactly as
parity_long.score_v1 does (both sides at 10 bits, CAMBI told the 8-bit
encode, the Mac's stride). BD is computed on the Mac from the records.

    ladder_run.py PLAN.json [--jobs 4]
PLAN: {"box", "hub", "curves": [{cell (a tables_plan cell), mode, rungs,
       stride, model}]}
"""
import argparse
import json
import os
import subprocess
import sys
import tempfile
import threading
import time

HOME = os.path.expanduser("~/t")
sys.path.insert(0, os.path.join(HOME, "cloud"))
import tables_run as tr                                   # noqa: E402

VMAF = os.path.join(HOME, "vmaf/libvmaf/build/tools/vmaf")
MODELS = os.path.join(HOME, "models")
THREADS = 6


def encode(c, mode, arm, q, out):
    if mode == "crf":
        rate = ["--crf", f"{q:g}"]
    else:
        rate = ["--bitrate", str(q), "--vbv-maxrate", str(round(1.5 * q)),
                "--vbv-bufsize", str(round(2 * q))]
    if arm == "ours":
        cmd = [tr.N265, "-"] + rate + ["--threads", str(THREADS), "-o", out]
    else:
        cmd = [tr.X265, "--input", "-", "--y4m", "--preset", "medium"] + rate \
            + ["--pools", str(THREADS), "--frame-threads", "4",
               "--log-level", "error", "-o", out]
    fp = subprocess.Popen(tr.feed_cmd(c, 0, c["frames"], threads=4),
                          stdout=subprocess.PIPE)
    t0 = time.time()
    rc = subprocess.run(cmd, stdin=fp.stdout, stdout=subprocess.DEVNULL,
                        stderr=subprocess.DEVNULL).returncode
    fp.stdout.close()
    fp.wait()
    if rc != 0:
        raise RuntimeError(f"{arm} {mode} {q} failed")
    return time.time() - t0


def score(c, bs, model, stride, wd):
    w, h = (int(x[1:]) for x in c["src"]["hdr"].split()[1:3])
    dfifo = os.path.join(wd, "d.y4m")
    os.mkfifo(dfifo)
    j = os.path.join(wd, "v.json")
    conv = ["-pix_fmt", "yuv420p10le", "-strict", "-1", "-f", "yuv4mpegpipe"]
    dec = subprocess.Popen(["ffmpeg", "-v", "error", "-y", "-threads", "2",
                            "-i", bs] + conv + [dfifo],
                           stdout=subprocess.DEVNULL)
    fp = subprocess.Popen(tr.feed_cmd(c, 0, c["frames"], threads=4),
                          stdout=subprocess.PIPE)
    cv = subprocess.Popen(["ffmpeg", "-v", "error", "-f", "yuv4mpegpipe",
                           "-i", "-"] + conv + ["-"], stdin=fp.stdout,
                          stdout=subprocess.PIPE)
    fp.stdout.close()
    m = (f"path={MODELS}/{model}:name=v1:cambi.enc_bitdepth=8"
         f":cambi.enc_width={w}:cambi.enc_height={h}")
    v = subprocess.run([VMAF, "--threads", "4", "--subsample", str(stride),
                        "-r", "/dev/stdin", "-d", dfifo, "--json", "-o", j,
                        "-q", "--model", m], stdin=cv.stdout,
                       stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
    cv.stdout.close()
    cv.wait()
    fp.wait()
    dec.wait()
    if v.returncode != 0:
        raise RuntimeError(v.stderr.decode()[-400:])
    jj = json.load(open(j))
    return jj["pooled_metrics"]["v1"]["mean"], len(jj["frames"])


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("plan")
    ap.add_argument("--jobs", type=int, default=4)
    a = ap.parse_args()
    plan = json.load(open(a.plan))
    tr.load_done(plan)
    os.makedirs(tr.WORK, exist_ok=True)
    threading.Thread(target=tr.deadman, daemon=True).start()
    jobs = []
    for cv in plan["curves"]:
        for arm in ("ref", "ours"):
            for q in cv["rungs"]:
                k = (cv["cell"]["name"], f"lad_{cv['mode']}", 0, arm,
                     int(q * 100) if cv["mode"] == "crf" else int(q))
                if k not in tr._done:
                    jobs.append((cv, arm, q, k))
    lock = threading.Lock()

    def worker():
        while True:
            with lock:
                if not jobs:
                    return
                cv, arm, q, k = jobs.pop(0)
            c = cv["cell"]
            wd = tempfile.mkdtemp(dir=tr.WORK)
            bs = os.path.join(wd, "o.hevc")
            try:
                es = encode(c, cv["mode"], arm, q, bs)
                size = os.path.getsize(bs)
                md = tr.md5_of(bs)
                v1, nf = score(c, bs, cv["model"], cv["stride"], wd)
                secs = c["frames"] / c["fps"]
                tr.record(plan, {"kind": "run", "cell": k[0], "goal": k[1],
                                 "round": 0, "arm": arm, "win": k[4],
                                 "rc": 0, "q": q, "bytes": size, "md5": md,
                                 "kbps": size * 8 / secs / 1000, "v1": v1,
                                 "scored": nf, "stride": cv["stride"],
                                 "model": cv["model"], "enc_s": round(es, 1)})
                tr.push(plan)
            except Exception as e:                        # noqa: BLE001
                tr.record(plan, {"kind": "err", "cell": k[0], "goal": k[1],
                                 "round": 0, "arm": arm, "rc": 1,
                                 "q": q, "err": str(e)[-400:]})
            finally:
                subprocess.run(["rm", "-rf", wd])

    th = [threading.Thread(target=worker) for _ in range(a.jobs)]
    for t in th:
        t.start()
    for t in th:
        t.join()
    tr.record(plan, {"kind": "done", "rc": 0, "cell": "-", "goal": "done",
                     "round": 0, "arm": "-"})
    tr.push(plan)
    subprocess.run("sudo shutdown -c; sudo shutdown -h +5", shell=True)


if __name__ == "__main__":
    main()
