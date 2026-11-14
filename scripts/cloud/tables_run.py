#!/usr/bin/env python3
# Copyright (c) 2026, the yah265 authors
# SPDX-License-Identifier: GPL-2.0-or-later
"""tables_run.py -- the parity spec's timed legs on a cloud box.

Runs ON the box after setup_tables.sh, from a plan written on the Mac by
tables_plan.py (one JSON per box: its cells, their sources, the Mac's
matched points). The method is parity_spec.py / parity_long.py's:

  G1   one thread, pure C (ours N265_ASM_OFF=all --threads 1, x265
       --no-asm --pools none --frame-threads 1), on the whole tier S clip
       or on the Mac's stratified 30 s windows (tiers M and L), each
       window its own stream, the cell's wall the sum of its windows.
  G2   pure C at the default thread count, the same spans.
  G3   SIMD at the default thread count, the whole cell. On x86 a third
       arm runs x265 with --asm avx512.
  VBR  G3 at the Mac's rung (x265's CRF 28 rate there), maxrate 1.5x,
       buffer 2x, the whole cell.

Rounds as the Mac's: G1 2, G2 3, G3 3 on tiers S and M; G1 1, G2 1 on
tier L windows; G3 2 and VBR 2 on tier L; VBR 2 on tier M. Arm order
alternates by round. Medians are taken on the Mac.

The box: the encoders are pinned to 16 vCPUs (eight cores and their
siblings on x86, sixteen cores on Graviton), ours --threads 16 and x265
--pools 16, which is what each would pick on a 16-vCPU box. The feeder
runs on the other vCPUs. G1's one-thread arms run eight at a time, each
on its own core (siblings idle), and the box is kept at eight busy cores
until the last timed arm ends, so every arm runs on an equally loaded box.
The default-thread arms run alone, one at a time.

Every record goes to results.jsonl and is copied to the hub after each
run; a rerun (after a spot reclaim) skips what the hub already holds.

    tables_run.py PLAN.json [--only prep,g1,g2,g3,vbr]
"""
import argparse
import json
import math
import os
import socket
import statistics
import subprocess
import sys
import threading
import time

P = os.path.dirname(os.path.abspath(__file__))
HOME = os.path.expanduser("~/t")
N265 = os.path.join(HOME, "src/build-rel/n265")
X265 = os.path.join(HOME, "x265build/x265")
FEEDER = os.path.join(HOME, "src/scripts/cloud/feeder.py")
MEDIA = "/data"
WORK = os.path.join(HOME, "work")
RES = os.path.join(HOME, "results.jsonl")
G1_LANES = 8
FEEDCHECK_SECS = 60.0
ROUNDS = {("S", "G1"): 2, ("S", "G2"): 3, ("S", "G3"): 3,
          ("M", "G1"): 2, ("M", "G2"): 3, ("M", "G3"): 3, ("M", "VBR"): 2,
          ("L", "G1"): 1, ("L", "G2"): 1, ("L", "G3"): 2, ("L", "VBR"): 2}


def sh(cmd, **kw):
    return subprocess.run(cmd, shell=True, capture_output=True, text=True,
                          **kw).stdout.strip()


def cpu_sets():
    """(encoder vCPUs, feeder vCPUs, one vCPU per encoder core)."""
    rows = [r.split(",") for r in sh("lscpu -p=CPU,CORE").splitlines()
            if not r.startswith("#")]
    by_core = {}
    for cpu, core in rows:
        by_core.setdefault(int(core), []).append(int(cpu))
    cores = [sorted(v) for _, v in sorted(by_core.items())]
    smt = len(cores[0]) > 1
    n_enc = 8 if smt else 16
    enc = sorted(c for v in cores[:n_enc] for c in v)
    feed = sorted(c for v in cores[n_enc:] for c in v)
    # a 16-vCPU box has no spare cores: the feeder shares the encoder's
    return enc, feed or enc, [v[0] for v in cores[:n_enc]]


ENC, FEED, ENC_CORES = cpu_sets()
X86 = os.uname().machine == "x86_64"
THREADS = len(ENC)


def cl(cpus):
    return ",".join(str(c) for c in cpus)


# ---------------------------------------------------------------- results

_done = set()
_lock = threading.Lock()
HUB = None
BOX = socket.gethostname()


def rkey(r):
    return (r["cell"], r["goal"], r["round"], r["arm"], r.get("win", -1))


def load_done(plan):
    global HUB
    HUB = plan["hub"]
    tag = plan["box"]
    if not os.path.exists(RES):
        subprocess.run(["scp", "-q", "-o", "StrictHostKeyChecking=no",
                        "-i", os.path.expanduser("~/.ssh/hub.pem"),
                        f"ubuntu@{HUB}:/data/res/{tag}.jsonl", RES])
    if os.path.exists(RES):
        for ln in open(RES):
            r = json.loads(ln)
            if r.get("rc") == 0 and r.get("kind", "run") == "run":
                _done.add(rkey(r))
            if r.get("kind") == "solve":
                _solved.append(r)
    # a (cell, goal) counts only when every round and window of it ran on
    # one box: a half-finished one left by a reclaimed box is redone whole
    if "cells" in plan and "curves" not in plan:
        for c in plan["cells"]:
            for goal in c["goals"]:
                arms = ["ours", "ref"] + (["ref512"] if X86 and goal == "G3"
                                          else [])
                want = {(c["name"], goal, rnd, a, wi)
                        for rnd in range(1, ROUNDS[(c["tier"], goal)] + 1)
                        for a in arms
                        for wi in range(len(spans(c, goal)))}
                if not want <= _done:
                    _done.difference_update(want)
                    _done.discard((c["name"], goal, 0, "feed", -1))


def push(plan):
    subprocess.run(["scp", "-q", "-o", "StrictHostKeyChecking=no", "-i",
                    os.path.expanduser("~/.ssh/hub.pem"), RES,
                    f"ubuntu@{HUB}:/data/res/{plan['box']}.jsonl"])


def record(plan, r):
    r.update(t=time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()),
             load1=round(os.getloadavg()[0], 2), box=BOX,
             itype=plan.get("itype"))
    with _lock:
        with open(RES, "a") as f:
            f.write(json.dumps(r) + "\n")
        if r.get("rc") == 0 and r.get("kind", "run") == "run":
            _done.add(rkey(r))
    print(json.dumps(r), flush=True)


def deadman():
    while True:
        sh("sudo shutdown -c; sudo shutdown -h +45")
        time.sleep(600)


# ------------------------------------------------------------------ feeds

def src_path(c):
    f = c["src"]["file"]
    return f if f.startswith("/") else os.path.join(MEDIA, f)


def feed_cmd(c, s, n, ready=None, md5=None, threads=None, out=None):
    """argv of the process writing frames s..s+n of the cell as Y4M."""
    sr = c["src"]
    if sr["kind"] == "y4m":
        # a raw Y4M on the box: header + a block copy, as the Mac's cat
        return ["python3", "-c", RAWCOPY, src_path(c), str(sr["skip"] + s),
                str(n)] + ([out] if out else [])
    a = ["python3", FEEDER, "--src", src_path(c), "--hdr", sr["hdr"],
         "--skip", str(sr["skip"] + s), "--count", str(n),
         "--threads", str(threads or len(FEED))]
    if sr.get("vf"):
        a += ["--vf", sr["vf"]]
    if sr.get("seek"):
        a += ["--seek", str(sr["seek"])]
    if sr.get("cpuflags") is not None and X86:
        a += ["--cpuflags", str(sr["cpuflags"])]
    if ready:
        a += ["--ready", ready]
    if md5:
        a += ["--md5", md5]
    if out:
        a += ["--out", out]
    return a


# header + frames s..s+n of a raw Y4M, block copy (stdout or a file)
RAWCOPY = r"""
import os, sys
p, s, n = sys.argv[1], int(sys.argv[2]), int(sys.argv[3])
o = open(sys.argv[4], 'wb') if len(sys.argv) > 4 else os.fdopen(1, 'wb', 0)
f = open(p, 'rb'); h = f.readline(); t = h.split()
w = int(next(x[1:] for x in t if x[:1] == b'W')); ht = int(next(x[1:] for x in t if x[:1] == b'H'))
per = w * ht * 3 // 2 + 6
f.seek(len(h) + s * per); o.write(h); left = n * per
while left:
    b = f.read(min(left, per * 8))
    if not b: sys.exit('short')
    o.write(b); left -= len(b)
o.close()
"""


def window_file(c, wi):
    return os.path.join(WORK, f"{c['name']}_w{wi}.y4m")


def spans(c, goal):
    if goal in ("G1", "G2") and c.get("windows"):
        return c["windows"][goal]["spans"]
    return [[0, c["frames"]]]


# ----------------------------------------------------------------- encode

def arm_cmd(c, goal, arm, out, threads_one=False):
    env = dict(os.environ)
    env.pop("N265_ASM_OFF", None)
    purec = goal in ("G1", "G2")
    if goal == "VBR":
        k = c["vbr_k"]
        rate = ["--bitrate", str(k), "--vbv-maxrate", str(round(1.5 * k)),
                "--vbv-bufsize", str(round(2 * k))]
    else:
        pt = (c["windows"][goal] if goal in ("G1", "G2") and c.get("windows")
              else c["points"]["ft1" if goal == "G1" else "def"])
        q = pt["ours_crf"] if arm == "ours" else pt["ref_crf"]
        rate = ["--crf", f"{q:g}" if isinstance(q, float) else str(q)]
    if arm == "ours":
        cmd = [N265, "-"] + rate + ["--threads",
                                    "1" if goal == "G1" else str(THREADS),
                                    "-o", out]
        if purec:
            env["N265_ASM_OFF"] = "all"
    else:
        cmd = [X265, "--input", "-", "--y4m", "--preset", "medium"] + rate
        cmd += (["--pools", "none", "--frame-threads", "1"] if goal == "G1"
                else ["--pools", str(THREADS)])
        if purec:
            cmd += ["--no-asm"]
        if arm == "ref512":
            cmd += ["--asm", "avx512"]
        cmd += ["--log-level", "error", "-o", out]
    return cmd, env


def md5_of(path):
    return sh(f"md5sum '{path}'").split()[0]


def timed(c, goal, arm, s, n, enc_cpus, feed_cpus, tag, src_file=None):
    """One timed encode: the feeder pinned to feed_cpus, the encoder to
    enc_cpus; the clock runs from the moment the feeder holds the first
    frame to the encoder's exit."""
    out = os.path.join(WORK, f"{tag}.hevc")
    cmd, env = arm_cmd(c, goal, arm, out)
    ready = os.path.join(WORK, f"{tag}.ready")
    if os.path.exists(ready):
        os.remove(ready)
    if src_file:
        fc = ["cat", src_file]
        rd = None
    else:
        fc = feed_cmd(c, s, n, ready=ready)
        rd = ready if c["src"]["kind"] != "y4m" else None
    # stderr goes to files: x265 can write more than a pipe holds (a
    # warning per frame on some titles) and would block on an unread pipe
    ferr = open(os.path.join(WORK, f"{tag}.feed.err"), "w+")
    eerr = open(os.path.join(WORK, f"{tag}.enc.err"), "w+")
    fp = subprocess.Popen(["taskset", "-c", cl(feed_cpus)] + fc,
                          stdout=subprocess.PIPE, stderr=ferr)
    if rd:
        while not os.path.exists(rd):
            if fp.poll() is not None:
                ferr.seek(0)
                return {"rc": -1, "err": ferr.read()[-300:]}
            time.sleep(0.005)
    t0 = time.perf_counter()
    ep = subprocess.Popen(["taskset", "-c", cl(enc_cpus)] + cmd,
                          stdin=fp.stdout, env=env, stdout=subprocess.DEVNULL,
                          stderr=eerr)
    fp.stdout.close()
    erc = ep.wait()
    dt = time.perf_counter() - t0
    frc = fp.wait()
    ok = erc == 0 and frc == 0 and os.path.exists(out)
    r = {"rc": 0 if ok else (erc or frc or -2), "wall_s": round(dt, 3),
         "bytes": os.path.getsize(out) if os.path.exists(out) else 0,
         "md5": md5_of(out) if os.path.exists(out) else "-"}
    if not ok:
        eerr.seek(0)
        ferr.seek(0)
        r["err"] = eerr.read()[-300:] + " | " + ferr.read()[-300:]
    if os.path.exists(out):
        os.remove(out)
    ferr.close()
    eerr.close()
    return r


# ------------------------------------------------------------------- prep

def feed_alone(c, s, n, src_file=None):
    m = min(n, max(1, int(round(FEEDCHECK_SECS * c["fps"]))))
    if src_file:
        t0 = time.perf_counter()
        sh(f"taskset -c {cl(FEED)} head -c {os.path.getsize(src_file)} "
           f"'{src_file}' > /dev/null")
        return n / max(time.perf_counter() - t0, 1e-3)
    ready = os.path.join(WORK, "fa.ready")
    if os.path.exists(ready):
        os.remove(ready)
    fp = subprocess.Popen(["taskset", "-c", cl(FEED)] +
                          feed_cmd(c, s, m, ready=ready),
                          stdout=subprocess.DEVNULL)
    while not os.path.exists(ready) and fp.poll() is None:
        time.sleep(0.005)
    t0 = time.perf_counter()
    fp.wait()
    return m / max(time.perf_counter() - t0, 1e-3)


def prep(plan, c):
    """The cell's frames checked against the Mac's (plane md5 over the
    span where the Mac has one, else its first two frames), and the G1/G2
    windows staged as raw Y4M on the local disk."""
    k = (c["name"], "prep", 0, "check", -1)
    if c.get("stage"):
        wf = os.path.join(WORK, f"{c['name']}.y4m")
        md5f = os.path.join(WORK, f"{c['name']}.md5")
        if not os.path.exists(wf + ".ok"):
            subprocess.run(["taskset", "-c", cl(ENC + FEED)] +
                           feed_cmd(c, 0, c["frames"], md5=md5f,
                                    threads=THREADS, out=wf), check=True,
                           stdout=subprocess.DEVNULL)
            open(wf + ".ok", "w").close()
        if k not in _done and c.get("check"):
            got = open(md5f).read().split()
            want = c["check"].get("plane_md5")
            record(plan, {"kind": "run", "cell": c["name"], "goal": "prep",
                          "round": 0, "arm": "check", "rc": 0,
                          "plane_md5": got[0], "frames": int(got[1]),
                          "want_md5": want, "same": got[0] == want})
            push(plan)
        c["src"] = {"kind": "y4m", "file": wf, "skip": 0}
    elif k in _done or not c.get("check"):
        pass
    else:
        chk = c["check"]
        md5f = os.path.join(WORK, f"{c['name']}.md5")
        n = chk.get("frames", c["frames"])
        t0 = time.time()
        subprocess.run(["taskset", "-c", cl(ENC + FEED)] +
                       feed_cmd(c, 0, n, md5=md5f, threads=THREADS),
                       stdout=subprocess.DEVNULL, check=False)
        got = open(md5f).read().split() if os.path.exists(md5f) else ["-", "0"]
        r = {"kind": "run", "cell": c["name"], "goal": "prep", "round": 0,
             "arm": "check", "rc": 0, "plane_md5": got[0],
             "frames": int(got[1]), "want_md5": chk.get("plane_md5"),
             "same": got[0] == chk.get("plane_md5"),
             "secs": round(time.time() - t0, 1)}
        record(plan, r)
        push(plan)
    if c.get("resolve"):
        resolve(plan, c)
    if c.get("windows"):
        for wi, (s, n) in enumerate(c["windows"]["G1"]["spans"]):
            wf = window_file(c, wi)
            if os.path.exists(wf + ".ok"):
                continue
            subprocess.run(["taskset", "-c", cl(ENC + FEED)] +
                           feed_cmd(c, s, n, threads=THREADS, out=wf),
                           check=True, stdout=subprocess.DEVNULL)
            open(wf + ".ok", "w").close()


def untimed_size(c, goal, arm, q=None):
    """Bytes of one untimed encode of the whole cell at goal's threading."""
    out = os.path.join(WORK, f"solve_{arm}.hevc")
    cc = json.loads(json.dumps(c))
    an = "ft1" if goal == "G1" else "def"
    if q is not None:
        cc["points"][an]["ours_crf"] = q
    cmd, env = arm_cmd(cc, goal, arm, out)
    fp = subprocess.Popen(feed_cmd(c, 0, c["frames"]), stdout=subprocess.PIPE)
    subprocess.run(["taskset", "-c", cl(ENC)] + cmd, stdin=fp.stdout, env=env,
                   stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
                   check=True)
    fp.stdout.close()
    fp.wait()
    b = os.path.getsize(out)
    os.remove(out)
    return b


def resolve(plan, c):
    """The size gate failed on this box with the Mac's crf: solve ours onto
    this box's x265 bytes (aim 0.4%), at each anchor the cell's goals use,
    the reference config being the timed one (G1: --no-asm, one thread)."""
    for an, goal in (("ft1", "G1"), ("def", "G3")):
        if (an == "ft1" and "G1" not in c["goals"]) or \
                (an == "def" and not {"G2", "G3"} & set(c["goals"])):
            continue
        k = (c["name"], "solve", 0, an, -1)
        rec = next((r for r in _solved if r["anchor"] == an
                    and r["cell"] == c["name"]), None)
        if rec is None:
            target = untimed_size(c, goal, "ref")
            q = c["points"][an]["ours_crf"]
            tried = []
            for _ in range(7):
                b = untimed_size(c, goal, "ours", q)
                tried.append((q, b))
                if abs(b / target - 1) < 0.004:
                    break
                if len(tried) >= 2 and tried[-1][1] != tried[-2][1]:
                    (q0, b0), (q1, b1) = tried[-2], tried[-1]
                    slope = (math.log(b1) - math.log(b0)) / (q1 - q0)
                    q = q1 + (math.log(target) - math.log(b1)) / slope
                else:
                    q = q + math.log(b / target) / 0.115
                q = round(max(10.0, min(45.0, q)), 2)
            best = min(tried, key=lambda t: abs(t[1] / target - 1))
            rec = {"kind": "solve", "cell": c["name"], "goal": "solve",
                   "round": 0, "arm": an, "anchor": an, "rc": 0,
                   "target": target, "tried": tried, "ours_crf": best[0],
                   "dsize_pct": round(100 * (best[1] / target - 1), 3)}
            record(plan, rec)
            push(plan)
            _solved.append(rec)
        c["points"][an]["ours_crf"] = rec["ours_crf"]
        c["points"][an]["box_solved"] = True


_solved = []


def drop_windows(c):
    if c.get("windows"):
        for wi in range(len(c["windows"]["G1"]["spans"])):
            for x in (window_file(c, wi), window_file(c, wi) + ".ok"):
                if os.path.exists(x):
                    os.remove(x)


# ------------------------------------------------------------------- legs

def g1(plan, cells):
    """Every G1 arm of every cell, eight at a time, each on its own core."""
    jobs = []
    for c in cells:
        if "G1" not in c["goals"]:
            continue
        sp = spans(c, "G1")
        for rnd in range(1, ROUNDS[(c["tier"], "G1")] + 1):
            arms = ["ours", "ref"] if rnd % 2 else ["ref", "ours"]
            for wi, (s, n) in enumerate(sp):
                for arm in arms:
                    if (c["name"], "G1", rnd, arm, wi) not in _done:
                        jobs.append((c, rnd, arm, wi, s, n))
    if not jobs:
        return
    print(f"G1: {len(jobs)} arms on {G1_LANES} lanes", flush=True)
    q = list(jobs)
    qlock = threading.Lock()
    left = [len(jobs)]
    stop = threading.Event()
    feed_cycle = FEED

    def lane(core):
        while True:
            with qlock:
                job = q.pop(0) if q else None
            if job is None:
                break
            c, rnd, arm, wi, s, n = job
            wf = window_file(c, wi) if c.get("windows") else None
            src = wf if wf else (src_path(c) if c["src"]["kind"] == "y4m"
                                 else None)
            r = timed(c, "G1", arm, s, n, [core], feed_cycle,
                      f"g1_{core}", src_file=src)
            r.update(kind="run", cell=c["name"], goal="G1", round=rnd,
                     arm=arm, win=wi, frames=n, core=core)
            record(plan, r)
            with qlock:
                left[0] -= 1
                if left[0] == 0:
                    stop.set()
        # keep this core loaded until the last timed arm anywhere ends
        c, rnd, arm, wi, s, n = jobs[0]
        wf = window_file(c, wi) if c.get("windows") else None
        src = wf if wf else src_path(c)
        while not stop.is_set():
            cmd, env = arm_cmd(c, "G1", "ref", "/dev/null")
            p = subprocess.Popen(
                f"cat '{src}' | taskset -c {core} " +
                " ".join(f"'{x}'" for x in cmd), shell=True, env=env,
                stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
                start_new_session=True)
            while p.poll() is None and not stop.is_set():
                time.sleep(1)
            if p.poll() is None:
                try:
                    os.killpg(p.pid, 9)
                except ProcessLookupError:
                    pass
            p.wait()

    th = [threading.Thread(target=lane, args=(core,))
          for core in ENC_CORES[:G1_LANES]]
    for t in th:
        t.start()
    for t in th:
        t.join()
    push(plan)


def default_legs(plan, c, goal):
    if goal not in c["goals"]:
        return
    sp = spans(c, goal)
    arms = ["ours", "ref"] + (["ref512"] if X86 and goal == "G3" else [])
    rounds = ROUNDS[(c["tier"], goal)]
    staged = goal == "G2" and c.get("windows")
    src0 = (window_file(c, 0) if staged else
            src_path(c) if c["src"]["kind"] == "y4m" else None)
    fk = (c["name"], goal, 0, "feed", -1)
    if fk not in _done:
        fps = feed_alone(c, sp[0][0], sp[0][1], src_file=src0)
        record(plan, {"kind": "run", "cell": c["name"], "goal": goal,
                      "round": 0, "arm": "feed", "rc": 0,
                      "feed_fps": round(fps, 1)})
    for rnd in range(1, rounds + 1):
        k = (rnd - 1) % len(arms)
        order = arms[k:] + arms[:k]
        for wi, (s, n) in enumerate(sp):
            for arm in (order if wi % 2 == 0 else order[::-1]):
                if (c["name"], goal, rnd, arm, wi) in _done:
                    continue
                src = (window_file(c, wi) if staged else
                       src_path(c) if c["src"]["kind"] == "y4m" else None)
                r = timed(c, goal, arm, s, n, ENC, FEED, f"{goal}_{arm}",
                          src_file=src)
                r.update(kind="run", cell=c["name"], goal=goal, round=rnd,
                         arm=arm, win=wi, frames=n)
                record(plan, r)
                push(plan)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("plan")
    ap.add_argument("--only", default="prep,g1,g2,g3,vbr")
    a = ap.parse_args()
    plan = json.load(open(a.plan))
    os.makedirs(WORK, exist_ok=True)
    threading.Thread(target=deadman, daemon=True).start()
    load_done(plan)
    only = a.only.split(",")
    print(f"box {BOX}: encoder vCPUs {cl(ENC)}; feeder vCPUs {cl(FEED)}; "
          f"G1 cores {ENC_CORES[:G1_LANES]}", flush=True)
    record(plan, {"kind": "env", "enc": ENC, "feed": FEED,
                  "g1_cores": ENC_CORES[:G1_LANES], "threads": THREADS,
                  "rc": 0, "cell": "-", "goal": "env", "round": 0,
                  "arm": "-"})
    cells = plan["cells"]
    # stage the windows of a batch of cells, run G1 across the batch, then
    # each cell's default-thread legs, then free the disk
    for batch in plan.get("batches", [[c["name"] for c in cells]]):
        bc = [c for c in cells if c["name"] in batch]
        if "prep" in only:
            for c in bc:
                prep(plan, c)
        if "g1" in only:
            g1(plan, bc)
        for c in bc:
            for goal, flag in (("G2", "g2"), ("G3", "g3"), ("VBR", "vbr")):
                if flag in only:
                    default_legs(plan, c, goal)
            drop_windows(c)
    push(plan)
    record(plan, {"kind": "done", "rc": 0, "cell": "-", "goal": "done",
                  "round": 0, "arm": "-"})
    push(plan)
    if plan.get("halt_when_done", True):
        sh("sudo shutdown -c; sudo shutdown -h +5")


if __name__ == "__main__":
    main()
