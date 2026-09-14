#!/usr/bin/env python3
# Copyright (c) 2026, the yah265 authors
# SPDX-License-Identifier: GPL-2.0-or-later
"""vbv_check.py - does the coded stream fit the buffer it claims to?

The hard gate for phase 12 item 2. For each clip and each (maxrate, bufsize)
pair it encodes with `--vbv-maxrate`/`--vbv-bufsize`, reads N265_VBV_TRACE,
and RE-DERIVES the CPB trajectory from the coded picture sizes alone:

    fill(0)     = init * bufsize
    fill(k + 1) = min(fill(k) - bits(k) + maxrate / fps, bufsize)

An underflow is any k with fill(k) - bits(k) < 0. The encoder's own
fill_in / fill_out columns are checked against that walk rather than
trusted - a checker that reads the encoder's opinion of its own buffer
checks nothing - and a disagreement is reported as `DESYNC`, which is a
failure in its own right.

    CLIPS=foreman_cif python3 scripts/vbv_check.py
    MODE=crf CRF=28 python3 scripts/vbv_check.py
    PAIRS='1.0x1.0s,1.0x0.5s,1.5x2.0s' JOBS=4 python3 scripts/vbv_check.py

A pair is `<rate multiple>x<buffer seconds>`: the rate multiplies the leg's
base rate (the ladder's rung 2 under ABR, the achieved rate under CRF) and
the buffer is that many SECONDS of the resulting maxrate, which is how a
VBV is specified in practice and what makes 0.5 s the hard cell.

Exit status is 1 if any cell underflows or desyncs, which is the gate.
"""
import json, os, re, statistics, subprocess, sys, tempfile
from concurrent.futures import ThreadPoolExecutor

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
HERE = os.path.join(ROOT, "scripts")
FRAMES = int(os.environ.get("FRAMES", "120"))
N = os.environ.get("N265", os.path.join(ROOT, "build", "n265"))
ARGS = os.environ.get("ARGS", "--aq 1 --cutree 1")
MODE = os.environ.get("MODE", "abr")            # abr | crf
CRF = os.environ.get("CRF", "28")
RUNG = int(os.environ.get("RUNG", "2"))         # 1-based ladder rung
INIT = os.environ.get("VBV_INIT", "0.9")
JOBS = int(os.environ.get("JOBS", "2"))
THREADS = os.environ.get("THREADS", "")
PAIRS = os.environ.get("PAIRS", "1.0x1.0s,1.0x0.5s,1.5x2.0s")
VERBOSE = os.environ.get("VERBOSE", "") != ""

ladders = json.load(open(os.path.join(HERE, "ladders.json")))
sel = os.environ.get("CLIPS", "")
clips = sel.split(",") if sel else list(ladders)


def fps_of(clip):
    with open(os.path.join(ROOT, "tests", "corpus", clip + ".y4m"), "rb") as f:
        hdr = f.readline().decode("latin-1")
    m = re.search(r" F(\d+):(\d+)", hdr)
    return int(m.group(1)) / int(m.group(2)) if m else 30.0


def parse_pairs(spec):
    out = []
    for p in spec.split(","):
        m = re.fullmatch(r"\s*([\d.]+)x([\d.]+)s\s*", p)
        if not m:
            sys.exit(f"vbv_check: {p!r} is not <mult>x<seconds>s")
        out.append((float(m.group(1)), float(m.group(2))))
    return out


def run(clip, extra, trace=True):
    """Encode and return (bytes, stderr)."""
    src = os.path.join(ROOT, "tests", "corpus", clip + ".y4m")
    fd, out = tempfile.mkstemp(suffix=".hevc")
    os.close(fd)
    env = dict(os.environ)
    if trace:
        env["N265_VBV_TRACE"] = "1"
    cmd = [N, src, "--frames", str(FRAMES)] + ARGS.split() + extra
    if THREADS:
        cmd += ["--threads", THREADS]
    cmd += ["-o", out]
    r = subprocess.run(cmd, capture_output=True, text=True, env=env)
    size = os.path.getsize(out)
    os.unlink(out)
    if r.returncode != 0:
        return None, r.stderr
    return size, r.stderr


ROW = re.compile(r"^VBV,(-?\d+),(-?\d+),(\d+),([-\d.e+]+),([-\d.e+]+),"
                 r"([-\d.e+]+),([-\d.e+]+),([-\d.e+]+),([-\d.e+]+)$", re.M)


def walk(rows, maxrate, bufsize, init, fps):
    """The CPB trajectory, re-derived from the coded sizes alone."""
    inflow = maxrate / fps
    fill = min(init * bufsize, bufsize)
    minfill = fill
    under = 0
    desync = 0
    for r in rows:
        bits = r["bits"]
        if abs(fill - r["fill_in"]) > 1.0:
            desync += 1
        fill -= bits
        minfill = min(minfill, fill)
        if fill < 0.0:
            under += 1
        fill = min(fill + inflow, bufsize)
        if abs(fill - r["fill_out"]) > 1.0:
            desync += 1
    return minfill, under, desync


def cell(clip, mult, secs, base_rate):
    """One (clip, pair) cell. base_rate is bit/s."""
    fps = fps_of(clip)
    maxrate = mult * base_rate
    bufsize = secs * maxrate
    extra = ["--vbv-maxrate", "%.0f" % (maxrate / 1000.0),
             "--vbv-bufsize", "%.0f" % (bufsize / 1000.0),
             "--vbv-init", INIT]
    if MODE == "abr":
        extra = ["--bitrate", "%.0f" % (base_rate / 1000.0)] + extra
    else:
        extra = ["--crf", CRF] + extra
    size, err = run(clip, extra)
    if size is None:
        return dict(clip=clip, pair=(mult, secs), err=err.strip()[-300:])
    rows = [dict(code=int(m.group(1)), disp=int(m.group(2)),
                 slice=int(m.group(3)), pred=float(m.group(4)),
                 allow=float(m.group(5)), bits=float(m.group(6)),
                 fill_in=float(m.group(7)), fill_out=float(m.group(8)),
                 dqp=float(m.group(9)))
            for m in ROW.finditer(err)]
    if not rows:
        return dict(clip=clip, pair=(mult, secs),
                    err="no VBV rows in the trace")
    minfill, under, desync = walk(rows, maxrate, bufsize, float(INIT), fps)
    clamped = sum(1 for r in rows if r["dqp"] > 1e-9)
    return dict(clip=clip, pair=(mult, secs), maxrate=maxrate,
                bufsize=bufsize, frames=len(rows), bytes=size,
                achieved=size * 8.0 * fps / len(rows),
                minfill=minfill, minpct=100.0 * minfill / bufsize,
                under=under, desync=desync, clamped=clamped,
                maxdqp=max(r["dqp"] for r in rows), err=None)


def base_rate_of(clip):
    """The rate every pair is a multiple of."""
    if MODE == "abr":
        rungs = ladders[clip]
        return rungs[min(RUNG, len(rungs)) - 1] * 1000.0
    # capped CRF: the rate the pinned CRF achieves with no VBV at all, so
    # "maxrate = 1.2x the achieved rate" is a statement about this clip and
    # not about a ladder it is not being run on.
    fps = fps_of(clip)
    size, _ = run(clip, ["--crf", CRF], trace=False)
    if size is None:
        return 0.0
    return size * 8.0 * fps / FRAMES


def main():
    pairs = parse_pairs(PAIRS)
    if MODE == "crf":
        # the item's capped-CRF cell: maxrate 1.2x the achieved rate
        pairs = parse_pairs(os.environ.get("PAIRS_CRF", "1.2x1.0s"))
    print(f"vbv_check: mode={MODE} frames={FRAMES} args={ARGS!r} "
          f"init={INIT} pairs={pairs}", flush=True)
    bases = {}
    with ThreadPoolExecutor(max_workers=JOBS) as ex:
        for c, b in zip(clips, ex.map(base_rate_of, clips)):
            bases[c] = b
            print(f"  {c:<18} base {b/1000:8.0f} kbit/s", flush=True)

    jobs = [(c, m, s) for c in clips for (m, s) in pairs]
    rows = []
    with ThreadPoolExecutor(max_workers=JOBS) as ex:
        for r in ex.map(lambda a: cell(a[0], a[1], a[2], bases[a[0]]), jobs):
            rows.append(r)
            if r["err"]:
                print(f"  {r['clip']:<18} {r['pair']}  ERROR {r['err']}",
                      flush=True)
            else:
                print("  %-18s %3.1fx/%.1fs  min fill %9.0f (%6.1f%%)  "
                      "underflow %d  desync %d  clamped %d/%d  max dQP %.2f"
                      % (r["clip"], r["pair"][0], r["pair"][1], r["minfill"],
                         r["minpct"], r["under"], r["desync"], r["clamped"],
                         r["frames"], r["maxdqp"]), flush=True)

    print("\n=== VBV underflow table (%s, %d frames) ===" % (MODE, FRAMES))
    print("| clip | " + " | ".join("%.1fx/%.1fs" % p for p in pairs) + " |")
    bad = 0
    for c in clips:
        cells = []
        for p in pairs:
            r = next((x for x in rows if x["clip"] == c and x["pair"] == p),
                     None)
            if r is None or r["err"]:
                cells.append("ERR")
                bad += 1
            else:
                cells.append("%d / %.0f%%" % (r["under"], r["minpct"]))
                bad += r["under"] + r["desync"]
        print("| %s | %s |" % (c, " | ".join(cells)))
    print("(cell is `underflows / min fill as a %% of the buffer`; "
          "the gate is zero underflows everywhere)")
    ok = [r for r in rows if not r["err"]]
    if ok:
        print("total underflows %d, total desyncs %d, worst min fill %.1f%%, "
              "median clamped share %.1f%%"
              % (sum(r["under"] for r in ok), sum(r["desync"] for r in ok),
                 min(r["minpct"] for r in ok),
                 statistics.median(100.0 * r["clamped"] / r["frames"]
                                   for r in ok)))
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
