#!/usr/bin/env python3
# Copyright (c) 2026, the yah265 authors
# SPDX-License-Identifier: GPL-2.0-or-later
"""The Graviton timing probe, run ON the rented box after setup_arm64.sh.
The arm64 twin of probe_x86.py.

Three tier S cells of the shared parity spec, both encoders built on the
box, eight arms per cell. The quality points (ours' solved --crf, x265 at
its crf) are read from parity-spec-points.json beside this script, which
is docs/data/parity-spec-points.json copied up:

    one thread, at the ft1 anchor
      o_c1  ours N265_ASM_OFF=all --threads 1          G1 ours
      x_c1  x265 --no-asm --pools none --frame-threads 1   G1 ref
      o_s1  ours --threads 1                           NEON/SVE, one thread
      x_s1  x265 --pools none --frame-threads 1        asm, one thread
    default threads, at the def anchor
      o_cN  ours N265_ASM_OFF=all                      G2 ours
      x_cN  x265 --no-asm                              G2 ref
      o_sN  ours                                       G3 ours
      x_sN  x265                                       G3 ref

Both encoders read the y4m from stdin through one `cat`, as the spec's
harness does. The wall is from the feeder's start to the encoder's exit.

The one-thread arms run concurrently to fit the box's hour: each round
starts two lanes, and every arm of a cell runs at the same time as the
cell's other three, each pinned to its own core. Graviton has one thread
per core, so each feeder gets a core of its own from the upper half of
the box. The arm-to-core map rotates every round. The default-thread
arms run alone on the box, one at a time, the arm order reversed on
alternate rounds.

    probe_arm64.py time [ROUNDS]  the timed rounds (default 3)
    probe_arm64.py perf           instruction counts and a profile of ours
"""
import json
import os
import subprocess
import sys
import threading
import time

P = os.path.dirname(os.path.abspath(__file__))
N265 = os.path.join(P, "src/build-rel/n265")
X265 = os.path.join(P, "x265build/x265")
MEDIA = os.path.join(P, "media")
OUT = os.path.join(P, "out")
RUNS = os.path.join(P, "runs.jsonl")

SPEC = [("shields", "720p", "shields_720p.y4m"),
        ("sunflower", "1080p", "sunflower_1080p.y4m"),
        ("bbb_2160p", "2160p", "src_bbb4k_300.y4m")]


def load_cells():
    """(title, file, ours ft1 crf, ours def crf, x265 crf) per cell"""
    pts = json.load(open(os.path.join(P, "parity-spec-points.json")))
    cells = []
    for title, cls, fn in SPEC:
        f1, fd = pts[f"S/{cls}/{title}/ft1"], pts[f"S/{cls}/{title}/def"]
        assert f1["ref_crf"] == fd["ref_crf"], title
        cells.append((title, fn, str(f1["ours_crf"]), str(fd["ours_crf"]),
                      str(f1["ref_crf"])))
    return cells


CELLS = load_cells()
T1_ARMS = ["o_c1", "x_c1", "o_s1", "x_s1"]
TN_ARMS = ["o_cN", "x_cN", "o_sN", "x_sN"]


def arm_cmd(arm, crf_ft1, crf_def, crf_ref, out):
    ours, pure_c, t1 = arm[0] == "o", arm[2] == "c", arm[3] == "1"
    env = dict(os.environ)
    env.pop("N265_ASM_OFF", None)
    if ours:
        cmd = [N265, "-", "--crf", crf_ft1 if t1 else crf_def, "-o", out]
        if t1:
            cmd += ["--threads", "1"]
        if pure_c:
            env["N265_ASM_OFF"] = "all"
    else:
        cmd = [X265, "--input", "-", "--y4m", "--preset", "medium",
               "--crf", crf_ref, "--log-level", "error", "-o", out]
        if t1:
            cmd += ["--pools", "none", "--frame-threads", "1"]
        if pure_c:
            cmd += ["--no-asm"]
    return cmd, env


def phys_cores():
    """(encoder core, feeder core) pairs: the lower half of the box runs
    encoders and the upper half their feeders, one thread per core."""
    n = os.cpu_count()
    return [(i, i + n // 2) for i in range(n // 2)]


def md5(path):
    return subprocess.run(["md5sum", path], capture_output=True,
                          text=True).stdout.split()[0]


def run_one(src, cmd, env, pin=None):
    feed_cmd = ["cat", src]
    if pin:
        cmd = ["taskset", "-c", str(pin[0])] + cmd
        feed_cmd = ["taskset", "-c", str(pin[1])] + feed_cmd
    t0 = time.perf_counter()
    feed = subprocess.Popen(feed_cmd, stdout=subprocess.PIPE)
    enc = subprocess.Popen(cmd, stdin=feed.stdout, env=env,
                           stdout=subprocess.DEVNULL,
                           stderr=subprocess.DEVNULL)
    feed.stdout.close()
    rc = enc.wait()
    dt = time.perf_counter() - t0
    feed.wait()
    return dt, rc


def record(row):
    with open(RUNS, "a") as f:
        f.write(json.dumps(row) + "\n")
    print(json.dumps(row), flush=True)


def timed(cell, arm, rnd, pin=None):
    title, fn, cf1, cfd, cfr = cell
    out = os.path.join(OUT, f"{title}_{arm}.hevc")
    cmd, env = arm_cmd(arm, cf1, cfd, cfr, out)
    dt, rc = run_one(os.path.join(MEDIA, fn), cmd, env, pin)
    record({"cell": title, "arm": arm, "round": rnd, "wall_s": round(dt, 3),
            "rc": rc, "bytes": os.path.getsize(out), "md5": md5(out),
            "pin": pin, "load1": os.getloadavg()[0],
            "t": time.strftime("%H:%M:%S")})


def busy(cell, arm, pin, stop, procs):
    """Rerun the arm, untimed, until the slowest arm of the cell ends, so
    every timed arm runs on an equally loaded box from start to finish
    (a lone tail would get the single-core turbo the others didn't)."""
    title, fn, cf1, cfd, cfr = cell
    cmd, env = arm_cmd(arm, cf1, cfd, cfr, "/dev/null")
    while not stop.is_set():
        p = subprocess.Popen(["taskset", "-c", str(pin[0])] + cmd,
                             stdin=open(os.path.join(MEDIA, fn), "rb"),
                             env=env, stdout=subprocess.DEVNULL,
                             stderr=subprocess.DEVNULL)
        procs.append(p)
        p.wait()


def lane(cells, cores, rnd):
    for cell in cells:
        k = rnd % 4
        arms = T1_ARMS[k:] + T1_ARMS[:k]
        stop, procs, fill, left = threading.Event(), [], [], [len(arms)]
        lock = threading.Lock()

        def one(a, pin):
            timed(cell, a, rnd, pin)
            with lock:
                left[0] -= 1
                if left[0] == 0:
                    stop.set()
                    return
            t = threading.Thread(target=busy, args=(cell, a, pin, stop, procs))
            fill.append(t)
            t.start()

        th = [threading.Thread(target=one, args=(a, cores[i]))
              for i, a in enumerate(arms)]
        for t in th:
            t.start()
        for t in th:
            t.join()
        while any(t.is_alive() for t in fill):
            for p in list(procs):
                if p.poll() is None:
                    p.kill()
            time.sleep(0.2)


def warm():
    for _, fn, _, _, _ in CELLS:
        subprocess.run(f"cat '{os.path.join(MEDIA, fn)}' > /dev/null",
                       shell=True)


def do_time(rounds):
    os.makedirs(OUT, exist_ok=True)
    pc = phys_cores()
    assert len(pc) >= 4, pc
    warm()
    # a round holds every arm, so a spot reclaim mid-run still leaves
    # whole rounds of every goal
    for rnd in range(rounds):
        if len(pc) < 8:
            lane(CELLS, pc[0:4], rnd)
        else:
            halves = [pc[0:4], pc[4:8]]
            if rnd % 2:
                halves.reverse()
            a = threading.Thread(target=lane, args=([CELLS[2]], halves[0], rnd))
            b = threading.Thread(target=lane, args=(CELLS[0:2], halves[1], rnd))
            a.start(); b.start(); a.join(); b.join()
        for cell in CELLS:
            arms = TN_ARMS if rnd % 2 == 0 else TN_ARMS[::-1]
            for arm in arms:
                timed(cell, arm, rnd)


def do_perf():
    """perf stat on the four one-thread arms over sunflower's first 100
    frames, and a flat profile of ours with its kernels on and off."""
    cell = CELLS[1]
    src = os.path.join(MEDIA, cell[1])
    for arm in T1_ARMS:
        out = os.path.join(OUT, f"perf_{arm}.hevc")
        cmd, env = arm_cmd(arm, cell[2], cell[3], cell[4], out)
        cmd = cmd + ["--frames", "100"]
        p = subprocess.run(["perf", "stat", "-x,", "-e",
                            "instructions,cycles,task-clock"] + cmd,
                           stdin=open(src, "rb"), env=env,
                           capture_output=True, text=True)
        print(f"perfstat {arm}: " + " | ".join(
            l for l in p.stderr.splitlines() if "," in l), flush=True)
    for arm in ("o_s1", "o_c1"):
        out = os.path.join(OUT, f"perf_{arm}.hevc")
        cmd, env = arm_cmd(arm, cell[2], cell[3], cell[4], out)
        data = os.path.join(P, f"perf_{arm}.data")
        subprocess.run(["perf", "record", "-q", "-o", data, "--"] + cmd +
                       ["--frames", "100"], stdin=open(src, "rb"), env=env,
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        rep = subprocess.run(["perf", "report", "-i", data, "--stdio",
                              "--sort", "symbol", "--percent-limit", "0.7"],
                             capture_output=True, text=True).stdout
        with open(os.path.join(P, f"profile_{arm}.txt"), "w") as f:
            f.write(rep)


if __name__ == "__main__":
    what = sys.argv[1] if len(sys.argv) > 1 else "time"
    if what == "time":
        do_time(int(sys.argv[2]) if len(sys.argv) > 2 else 3)
    elif what == "perf":
        do_perf()
