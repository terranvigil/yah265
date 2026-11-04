#!/usr/bin/env python3
# Copyright (c) 2026, the yah265 authors
# SPDX-License-Identifier: GPL-2.0-or-later
"""devset.py - tier D, the narrow dev set (docs/parity-plan.md, "Tier D").

Eight short cells cut to ~/media/tier_d, each standing in for a group of
full-table cells. An item is read here in minutes; the full table
(parity_spec.py / parity_long.py) runs only at milestones and confirms
what tier D said.

Per cell, ours at --preset medium (build-rel/n265 unless --bin):

  CRF ladder  ours and x265 medium at CRF 20/23/26/29/32, both at 6
              threads (x265 --pools 6 --frame-threads 4). BD-VMAF-NEG,
              NEG model only, every frame.
  matched     ours solved onto x265 CRF 28's bytes within 1% (secant in
              log bytes, started from ours' own ladder): dVMAF-NEG,
              dPSNR-Y, dSIZE.
  VBR ladder  D2, D4, D5v and D3v: five rungs at x265's achieved CRF-ladder
              rates, maxrate 1.5x, buffer 2x, both at 6 threads. BD and
              per-rung tracking.
  speed       --threads 1, untimed: instructions retired and user CPU,
              pure C (ours N265_ASM_OFF=all, x265 --no-asm) and SIMD,
              x265 at --pools none --frame-threads 1. Ours at its matched
              CRF, x265 at 28. Read through boxlib.measure; load barely
              moves the instruction count, so no quiet box is needed.

x265 is memoised once per x265 binary. Ours is memoised on its binary
hash, its N265_* environment and its flags, so a re-read of an unchanged
build costs nothing and a new build costs only ours.

  devset.py                     read ours (build-rel/n265), print the table
  devset.py --bin B --arm 'N265_X=1' --flags '--foo 1'
  devset.py --ab [--base B]     the arm against main, ours only; main is
                                ~/src/yah265/build-rel/n265 unless --base
  devset.py --cells D1,D4 --no-speed --no-vbr
  devset.py --show              the table from what is cached, runs nothing
  devset.py --list              the cells, their windows and stand-ins
  devset.py --record            append the read to docs/data/devset.csv

Box share: DEVSET_CORES (12) caps the sum of the running tasks' threads;
a quality task takes PL_THREADS (6), a speed task one. libvmaf runs at
VMAF_THREADS (4) with ffmpeg's decode at FF_THREADS (2), inside the same
task's share. Every task waits while a timed leg holds TIMING_ACTIVE.
"""
import argparse
import csv
import datetime
import hashlib
import math
import os
import shlex
import statistics
import subprocess
import sys
import tempfile
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "scripts"))
import boxlib                                             # noqa: E402
import parity_long as pl                                  # noqa: E402

ps = pl.ps
bd = pl.bd

TIER_D = os.environ.get("DEVSET_DIR", os.path.expanduser("~/media/tier_d"))
CACHE = os.environ.get("DEVSET_CACHE",
                       os.path.expanduser("~/.cache/yah265-devset"))
# --ab's main: the binary --pin-main copied here, its commit beside it
MAIN_BIN = os.environ.get("DEVSET_MAIN", os.path.join(CACHE, "main", "n265"))
CORES = int(os.environ.get("DEVSET_CORES", "12"))
CSV_PATH = os.path.join(ROOT, "docs/data/devset.csv")
LOGS = os.path.join(ROOT, "scratch/devset")

# the encode/score memo and the per-task results live outside the tree,
# so every worktree shares x265's anchors and main's reads
pl.MEMO = os.path.join(CACHE, "memo.json")
RESULTS = os.path.join(CACHE, "results.json")
pl.SSIM_PASS = False                 # NEG and PSNR-Y only
SCORES = os.path.join(CACHE, "scores.json")
_score = pl.score


def score_memo(cell, bitstream, wd, s=0, n=None, stride=1, psnr_only=False):
    """parity_long's score, memoised on the bitstream's md5 and the frames
    it is scored against: an arm that leaves a cell's stream alone pays
    the encode and not the libvmaf run."""
    k = "|".join((pl.file_md5(bitstream), pl.feed_sig(pl.feeder(cell, s, n)),
                  str(stride), str(psnr_only), str(pl.SSIM_PASS)))
    have = pl.jload(SCORES).get(k)
    if have and (psnr_only or pl.has_gate(have, cell)):
        return have
    return pl.jput(SCORES, k, _score(cell, bitstream, wd, s, n, stride,
                                     psnr_only))


pl.score = score_memo

RUNGS = pl.RUNGS                     # 20/23/26/29/32
REF_CRF = pl.REF_CRF                 # 28
MATCH_AIM = 0.008                    # inside the 1% bar with a margin
GOP_STRIDE = 1

# id, class, title, file, frames, source, first source frame, VBR?,
# why it is here, the full-table cells it stands in for
CELLS = [
    ("D1", "1080p", "sunflower", "d1_sunflower_1080p.y4m", 500,
     "<media>/tier_s/sunflower_1080p.y4m", 0, False,
     "-2.10 VMAF-NEG; a suspected false scene cut",
     "S/1080p/sunflower; the S 1080p class (pedestrian, riverbed)"),
    ("D2", "2160p", "meridian", "d2_meridian_2160p.y4m", 300,
     "<media>/tier_s/src_meridian_modern_600.y4m", 0, True,
     "-0.56 VMAF-NEG (read on the old clip)",
     "S/2160p/meridian_2160p; M/2160p/meridian; L meridian 720p-2160p"),
    ("D3", "2160p", "chimera", "d3_chimera_2160p.y4m", 300,
     "~/media/throughput/chimera_2160p_120s.y4m", 0, False,
     "-0.33 at matched bytes; a suspected false cut near frame 160",
     "M/2160p/chimera; M/1080p/chimera"),
    ("D4", "1080p", "sollevante", "d4_sollevante_1080p.y4m", 300,
     "~/media/throughput/sollevante_1080p_120s.y4m", 1193, True,
     "the speed cell: intra-heavy, 22 cuts a minute",
     "M/1080p/sollevante; M/2160p/sollevante"),
    ("D5", "720p", "chimera", "d5_chimera_720p.y4m", 719,
     "~/media/throughput/chimera_720p_120s.y4m", 1020, False,
     "720p live action; the tier M G1/G2 window of the segment",
     "L 720p; M/1080p/chimera"),
    # the 30 s window reads VBR level with x265 (+0.5%); the +19% gap is
    # only on the whole segment, where x265 lands 14-15% under its
    # targets. So D5's VBR ladder reads the segment, VBR only. The copy
    # keeps the segment's file name, which is part of the memo keys.
    ("D5v", "720p", "chimera_seg", "chimera_720p_120s.y4m", 2760,
     "~/media/throughput/chimera_720p_120s.y4m", 0, "only",
     "VBR +19.6% BD on the whole trimmed 720p segment",
     "every VBR curve on tiers M and L"),
    # the 2160p VBR cell (item 48). D3's own window (0-299, the hard
    # opening) reads VBR -7.7% and doesn't show the segment's +2.8%; the
    # mid-segment stretch does (+7.8%, and +8.9% under CRF). It is read
    # in place from the segment, not cut: 600 frames are 7.5 GB. The
    # title is part of the memo keys, so it keeps its probe name.
    ("D3v", "2160p", "chimera_w", "chimera_2160p_120s.y4m", 600,
     "~/media/throughput/chimera_2160p_120s.y4m", 1200, "only",
     "VBR +7.8% BD, CRF +8.9%: the segment's mid-stretch",
     "M/2160p/chimera VBR",
     {"dir": os.path.expanduser("~/media/throughput"), "offset": 1200,
      "limit": 600}),
    ("D6", "720p", "lash1924", "d6_lash1924_720p.y4m", 300,
     "~/media/staging-y4m/lash1924-x10_720p.y4m", 2250, False,
     "film grain, 1280x960",
     "C/720p/lash1924; C/1080p/lash1924; L nocturne (grain)"),
    ("D7", "1080p", "bbb", "d7_bbb_1080p.y4m", 300,
     "~/media/staging-y4m/bbb-x10_1080p.y4m", 2850, False,
     "animation",
     "L bbb 720p-2160p; S/2160p/bbb_2160p; L sintel, L cosmos"),
    ("D8", "1080p", "tears", "d8_tears_1080p.y4m", 300,
     "~/media/staging-y4m/tears-x10_1080p.y4m", 2250, False,
     "1920x858, odd height; long-form live action and CG",
     "L tears 720p-2160p; the odd-height cells (sintel, cosmos)"),
    # the 3840x1714 tier L cell (item 49): the first of the excerpt's G1/G2
    # windows, read in place from the staged excerpt (600 frames of 4K are
    # 5.9 GB). Two scene cuts in a dark, grainy live-action stretch.
    ("D9", "2160p", "tears_w", "tears-x10_2160p.y4m", 600,
     "<staging>/yah264/y4m/tears-x10_2160p.y4m", 1752, False,
     "3840x1714; L/2160p/tears CRF +2.58% on the 3-rung screen",
     "L/2160p/tears; the UHD scope cells",
     {"dir": "<staging>/yah264/y4m", "offset": 1752,
      "limit": 600}),
    # the 60 fps long-form VBR cell (item 49): three minutes of the Meridian
    # excerpt from its first G1/G2 window, VBR only, read in place. The
    # hard stretch inside it is where a near-empty buffer used to code the
    # P anchors 16 QP over the leaf B pictures.
    ("D10v", "720p", "meridian_w", "meridian-x10_720p.y4m", 10800,
     "~/media/staging-y4m/meridian-x10_720p.y4m", 4582, "only",
     "VBR +7.46% on the 3-rung screen; L/720p/meridian VBR +2.41%",
     "L/720p/meridian VBR; the long 60 fps VBR cells",
     {"dir": os.path.expanduser("~/media/staging-y4m"), "offset": 4582,
      "limit": 10800}),
]


def cell_of(row):
    cid, cls, title, fn, frames = row[:5]
    c = pl._mk("D", cls, title, cls, kind="clip", file=fn, dir=TIER_D)
    c.update(id=cid, frames=frames, src=row[5], start=row[6], vbr=row[7],
             why=row[8], stands=row[9])
    # a window read in place from a longer file, not cut to tier_d
    if len(row) > 10:
        c.update(row[10])
    return c


ALL = [cell_of(r) for r in CELLS]
BY_ID = {c["id"]: c for c in ALL}

# ------------------------------------------------------------------ arms


def bin_hash(exe):
    return pl.bin_hash(exe)


def arm_sig(exe, knobs, flags):
    """What makes two reads of ours the same read."""
    k = " ".join(sorted(f"{a}={b}" for a, b in knobs.items()))
    return f"{bin_hash(exe)}|{k}|{flags}"


def parse_knobs(s):
    out = {}
    for w in shlex.split(s or ""):
        if "=" not in w or not w.startswith("N265_"):
            sys.exit(f"devset: --arm takes N265_*=value words, not {w!r}")
        a, b = w.split("=", 1)
        out[a] = b
    return out


def clean_env(knobs):
    e = {k: v for k, v in os.environ.items() if not k.startswith("N265_")}
    e.update(knobs)
    return e

# ------------------------------------------------------------------ store


def results():
    return pl.jload(RESULTS)


def rkey(task, cell, sig):
    return f"{task}|{cell['key']}|{sig}"


def ref_sig(extra=""):
    return f"x265|{bin_hash(ps.X265)}{extra}"

# ------------------------------------------------------------------ tasks
# Each task runs as its own process (the arm's binary and environment are
# process globals in parity_long), writes its numbers to RESULTS, and is
# skipped by the parent when they are there already.


def _rung(cell, arm, mode, q, stride=GOP_STRIDE, log=print):
    r = pl.measure(cell, arm, mode, q, stride=stride, log=log)
    out = {k: r.get(k) for k in ("bytes", "kbps", "neg", "psnr_y", "md5",
                                 "frames_ok", "q")}
    out["v1"] = pl.gate_score(r) if "v1_gate" in r else None
    return out


def qv(r):
    """A rung's gate metric: v1 where the harness gates on it, else NEG."""
    return r.get("v1") if pl.GATE == "v1" else r["neg"]


def scored(rec):
    """A cached task scored on the current gate metric (a NEG-era row
    is re-read: its encodes come back from the memo, its scores don't)."""
    rows = list(rec.get("rungs", []))
    if "point" in rec:
        rows.append(rec["point"])
    return all(qv(r) is not None for r in rows)


def t_ref_crf(cell, sig):
    rungs = [_rung(cell, "ref", "crf", q) for q in RUNGS]
    pt = _rung(cell, "ref", "crf", REF_CRF)
    return {"rungs": rungs, "point": pt}


def need(task, cell, sig):
    r = results().get(rkey(task, cell, sig))
    if r is None:
        sys.exit(f"devset: {task} for {cell['id']} is missing; the parent "
                 "orders the tasks, so this is a bug")
    return r


def t_ref_vbr(cell, sig):
    crf = need("ref_crf", cell, ref_sig())
    ks = [int(round(r["kbps"])) for r in crf["rungs"]]
    rungs = [_rung(cell, "ref", "vbr", k) for k in ks]
    return {"targets": ks, "rungs": rungs}


def interp_crf(rungs, target):
    """Ours' CRF for `target` bytes off its own ladder, linear in log
    bytes between the bracketing rungs; and the local slope."""
    pts = sorted((r["q"], r["bytes"]) for r in rungs)
    for (c0, b0), (c1, b1) in zip(pts, pts[1:]):
        if b1 <= target <= b0 or b0 <= target <= b1:
            m = (math.log(b1) - math.log(b0)) / (c1 - c0)
            if abs(m) < 1e-9:
                return c0, -0.13
            return c0 + (math.log(target) - math.log(b0)) / m, m
    (c0, b0), (c1, b1) = (pts[0], pts[1]) if target > pts[0][1] else \
        (pts[-2], pts[-1])
    m = (math.log(b1) - math.log(b0)) / (c1 - c0)
    return c0 + (math.log(target) - math.log(b0)) / m, m


def t_ours_crf(cell, sig):
    ref = need("ref_crf", cell, ref_sig())
    rungs = [_rung(cell, "ours", "crf", q) for q in RUNGS]
    target = ref["point"]["bytes"]
    start, slope = interp_crf(rungs, target)
    best, tried, _ = pl.secant(
        lambda q: pl.measure(cell, "ours", "crf", q)["bytes"], target,
        round(start, 2), slope=min(-0.05, max(-0.30, slope)), aim=MATCH_AIM)
    pt = _rung(cell, "ours", "crf", best[0])
    pt["solve_encodes"] = len(tried)
    return {"rungs": rungs, "point": pt, "match_crf": best[0]}


def t_ours_vbr(cell, sig):
    ref = need("ref_vbr", cell, ref_sig())
    rungs = [_rung(cell, "ours", "vbr", k) for k in ref["targets"]]
    return {"targets": ref["targets"], "rungs": rungs}


def t_speed(cell, sig, arm, tier):
    """One --threads 1 run under boxlib.measure: ours at its matched CRF,
    x265 at 28 (one frame thread, no pool)."""
    purec = tier == "c"
    wd = tempfile.mkdtemp(prefix="ds.", dir=pl.ensure_work())
    out = os.path.join(wd, "o.hevc")
    try:
        if arm == "ours":
            q = need("ours_crf", cell, sig)["match_crf"]
            cmd = pl.our_cmd(out, "crf", q, 1)
        else:
            q = REF_CRF
            cmd = pl.ref_cmd(out, "crf", q, "t1", purec)
        env = pl.arm_env(arm, purec)
        r = pl.pipe_run(pl.feeder(cell), cmd, env, timed=True)
        rec = {k: r.get(k) for k in ("wall", "user", "sys", "instr",
                                     "cycles", "rss")}
        rec.update(q=q, bytes=os.path.getsize(out), md5=pl.file_md5(out),
                   date=pl.now())
    finally:
        subprocess.run(["rm", "-rf", wd])
    return rec


TASKS = {"ref_crf": t_ref_crf, "ref_vbr": t_ref_vbr,
         "ours_crf": t_ours_crf, "ours_vbr": t_ours_vbr}


def run_task(task, cid, sig):
    """The worker side: one task on one cell, its record into RESULTS."""
    cell = BY_ID[cid]
    boxlib.wait_untimed(f"devset {task} {cid}")
    t0 = time.time()
    if task.startswith("speed_"):
        _, arm, tier = task.split("_")
        rec = t_speed(cell, sig, arm, tier)
    else:
        rec = TASKS[task](cell, sig)
    rec["task_s"] = round(time.time() - t0, 1)
    rec["date"] = pl.now()
    pl.jput(RESULTS, rkey(task, cell, sig), rec)
    print(f"devset: {task} {cid} done in {rec['task_s']:.0f} s", flush=True)

# --------------------------------------------------------------- schedule


# a quality task's share: its encode runs at PL_THREADS and its libvmaf at
# VMAF_THREADS beside ffmpeg's decode, never both at once, so 5 of 12
# leaves two one-thread speed lanes beside two quality tasks
QW = int(os.environ.get("DEVSET_QW", "5"))


def weight(task):
    return 1 if task.startswith("speed_") else QW


def cost(task, cell):
    """Rough relative length, longest first."""
    px = {"720p": 1.0, "1080p": 2.25, "2160p": 9.0}[cell["cls"]] \
        * cell["frames"] / 300
    f = {"ref_crf": 7, "ours_crf": 9, "ref_vbr": 6, "ours_vbr": 6,
         "speed_ref_c": 30, "speed_ref_simd": 8, "speed_ours_c": 6,
         "speed_ours_simd": 3}[task]
    return px * f * weight(task)


DEPS = {"ref_vbr": ["ref_crf"], "ours_crf": ["ref_crf"],
        "ours_vbr": ["ref_vbr"], "speed_ours_c": ["ours_crf"],
        "speed_ours_simd": ["ours_crf"]}


def plan(cells, sig, want_vbr, want_speed, want_ref_speed=True,
         want_crf=True):
    """[(task, cell, sig)] still missing from RESULTS, deps included."""
    have = results()
    todo = []

    def add(task, cell, s):
        if rkey(task, cell, s) in have and (task.startswith("speed_") or
                                            scored(have[rkey(task, cell, s)])):
            return
        if any(t == task and c is cell and x == s for t, c, x in todo):
            return
        for d in DEPS.get(task, []):
            add(d, cell, sig if d.startswith("ours") else ref_sig())
        todo.append((task, cell, s))
    for c in cells:
        if c["vbr"] == "only":
            if want_vbr:
                add("ours_vbr", c, sig)
            continue
        if want_crf:
            add("ours_crf", c, sig)
        if want_vbr and c["vbr"]:
            add("ours_vbr", c, sig)
        if want_speed:
            add("speed_ours_c", c, sig)
            add("speed_ours_simd", c, sig)
            if want_ref_speed:
                add("speed_ref_c", c, ref_sig())
                add("speed_ref_simd", c, ref_sig())
    return todo


def execute(todo, arms):
    """Run the tasks under the core cap, dependencies first, longest
    first. `arms` maps a sig to (binary, knobs, flags)."""
    os.makedirs(LOGS, exist_ok=True)
    pending = list(todo)
    running = {}
    done = set(k for k in results())
    t0 = time.time()
    fails = []

    def ready(t):
        task, cell, s = t
        for d in DEPS.get(task, []):
            ds = s if d.startswith("ours") else ref_sig()
            if rkey(d, cell, ds) not in done:
                return False
        return True
    while pending or running:
        for p, (t, lf) in list(running.items()):
            if p.poll() is not None:
                lf.close()
                del running[p]
                task, cell, s = t
                if p.returncode != 0:
                    fails.append(t)
                    print(f"  FAIL {task} {cell['id']} (log {lf.name})",
                          flush=True)
                else:
                    done.add(rkey(task, cell, s))
                    print(f"  [{time.time() - t0:6.0f} s] done {task:<16} "
                          f"{cell['id']}", flush=True)
        if fails and not running:
            break
        used = sum(weight(t[0]) for t, _ in running.values())
        # a ready speed task goes first: each is one long serial run, and
        # left to the end they are the tail of the read
        cand = sorted((t for t in pending if ready(t)),
                      key=lambda t: (not t[0].startswith("speed_"),
                                     -cost(t[0], t[1])))
        for t in cand:
            w = weight(t[0])
            if used and used + w > CORES:
                continue
            task, cell, s = t
            exe, knobs, flags = arms.get(s, (ps.X265, {}, ""))
            env = clean_env(knobs)
            env["N265"] = exe if not s.startswith("x265|") else \
                env.get("N265", ps.N265)
            env["DEVSET_FLAGS"] = flags
            lf = open(os.path.join(LOGS, f"{task}-{cell['id']}-"
                                   f"{hashlib.md5(s.encode()).hexdigest()[:8]}"
                                   ".log"), "w")
            p = subprocess.Popen([sys.executable, os.path.abspath(__file__),
                                  "--task", task, "--cell", cell["id"],
                                  "--sig", s], env=env, stdout=lf,
                                 stderr=subprocess.STDOUT)
            running[p] = (t, lf)
            pending.remove(t)
            used += w
        if not cand and not running and pending:
            sys.exit("devset: tasks left with unmet dependencies: "
                     + ", ".join(f"{t} {c['id']}" for t, c, _ in pending))
        time.sleep(2)
    if fails:
        sys.exit("devset: failed: " + ", ".join(f"{t} {c['id']}"
                                                 for t, c, _ in fails))
    return time.time() - t0

# ------------------------------------------------------------------ table


def bdn(ref, ours):
    if not ref or not ours:
        return None
    x, o = ref["rungs"], ours["rungs"]
    return bd.bd_rate([r["kbps"] for r in x], [qv(r) for r in x],
                      [r["kbps"] for r in o], [qv(r) for r in o])


def track(rec):
    return [100 * (r["kbps"] / k - 1) for r, k in zip(rec["rungs"],
                                                    rec["targets"])]


def read(cell, sig):
    """Every number of one cell for one arm, None where not read."""
    R = results()
    g = lambda t, s: R.get(rkey(t, cell, s))           # noqa: E731
    rc, rv = g("ref_crf", ref_sig()), g("ref_vbr", ref_sig())
    oc, ov = g("ours_crf", sig), g("ours_vbr", sig)
    # only the legs the cell is read on, whatever else the cache holds
    if not cell["vbr"]:
        rv = ov = None
    if cell["vbr"] == "only":
        oc = None
    out = {"id": cell["id"], "name": cell["name"]}
    out["bd_crf"] = bdn(rc, oc)
    out["bd_vbr"] = bdn(rv, ov)
    if ov:
        out["trk_o"] = track(ov)
    if rv:
        out["trk_x"] = track(rv)
    if rc and oc:
        a, b = oc["point"], rc["point"]
        out.update(dneg=qv(a) - qv(b), dpsnr=a["psnr_y"] - b["psnr_y"],
                   dsize=100 * (a["bytes"] / b["bytes"] - 1),
                   match_crf=oc["match_crf"], ref_neg=qv(b),
                   frames_ok=all(r.get("frames_ok") for r in
                                 oc["rungs"] + rc["rungs"] + [a, b]))
    for tier in ("c", "simd"):
        so, sx = g(f"speed_ours_{tier}", sig), g(f"speed_ref_{tier}",
                                                 ref_sig())
        if so:
            out[f"o_{tier}"] = so
        if so and sx and so.get("instr") and sx.get("instr"):
            out[f"ix_{tier}"] = so["instr"] / sx["instr"]
            out[f"cx_{tier}"] = so["user"] / sx["user"]
        if so and oc and so.get("md5") != oc["point"]["md5"]:
            out.setdefault("warn", []).append(
                f"t1 {tier} stream differs from the 6-thread matched one")
    return out


def f(v, fmt, w):
    return (fmt.format(v) if v is not None else "-").rjust(w)


def print_table(rows, title):
    print(f"\n{title}")
    print(f"{'cell':<20}{'BD-CRF':>8}{'BD-VBR':>8}{'trk max':>8}"
          f"{'dNEG':>7}{'dPSNR':>7}{'dSIZE':>7}{'crf':>7}"
          f"{'instr C':>9}{'instr S':>9}{'cpu C':>7}{'cpu S':>7}")
    for r in rows:
        trk = max((abs(x) for x in r["trk_o"]), default=None) \
            if "trk_o" in r else None
        print(f"{r['id'] + ' ' + r['name']:<20}"
              f"{f(r.get('bd_crf'), '{:+.2f}', 8)}"
              f"{f(r.get('bd_vbr'), '{:+.2f}', 8)}"
              f"{f(trk, '{:.1f}', 8)}"
              f"{f(r.get('dneg'), '{:+.2f}', 7)}"
              f"{f(r.get('dpsnr'), '{:+.2f}', 7)}"
              f"{f(r.get('dsize'), '{:+.2f}', 7)}"
              f"{f(r.get('match_crf'), '{:.2f}', 7)}"
              f"{f(r.get('ix_c'), '{:.3f}', 9)}"
              f"{f(r.get('ix_simd'), '{:.3f}', 9)}"
              f"{f(r.get('cx_c'), '{:.2f}', 7)}"
              f"{f(r.get('cx_simd'), '{:.2f}', 7)}")
        for w in r.get("warn", []):
            print(f"    warning: {w}")
        if r.get("frames_ok") is False:
            print("    warning: a scored stream decoded short")

    def med(k):
        v = [r[k] for r in rows if r.get(k) is not None]
        return statistics.median(v) if v else None
    print(f"{'median':<20}{f(med('bd_crf'), '{:+.2f}', 8)}"
          f"{f(med('bd_vbr'), '{:+.2f}', 8)}{'':>8}"
          f"{f(med('dneg'), '{:+.2f}', 7)}{f(med('dpsnr'), '{:+.2f}', 7)}"
          f"{'':>14}{f(med('ix_c'), '{:.3f}', 9)}"
          f"{f(med('ix_simd'), '{:.3f}', 9)}{f(med('cx_c'), '{:.2f}', 7)}"
          f"{f(med('cx_simd'), '{:.2f}', 7)}")
    print("BD = BD-rate on " + ("VMAF v1" if pl.GATE == "v1" else "VMAF-NEG") + " vs x265 medium (+ is behind); trk max = "
          "ours' worst VBR rung off target, %; dNEG/dPSNR/dSIZE at x265 "
          "CRF 28's bytes; instr/cpu = ours over x265 at --threads 1, "
          "C = pure C, S = SIMD")


def print_ab(base_rows, arm_rows, title):
    print(f"\n{title}")
    print(f"{'cell':<20}{'BD-CRF':>16}{'BD-VBR':>16}{'dNEG@match':>16}"
          f"{'instr C':>9}{'instr S':>9}")
    for b, a in zip(base_rows, arm_rows):
        def pair(k, fmt):
            if a.get(k) is None or b.get(k) is None:
                return "-".rjust(16)
            return f"{fmt.format(b[k])}>{fmt.format(a[k])}".rjust(16)

        def ir(t):
            ao, bo = a.get(f"o_{t}"), b.get(f"o_{t}")
            if not ao or not bo or not ao.get("instr") or not bo.get("instr"):
                return "-".rjust(9)
            return f"{ao['instr'] / bo['instr']:.3f}".rjust(9)
        print(f"{a['id'] + ' ' + a['name']:<20}{pair('bd_crf', '{:+.2f}')}"
              f"{pair('bd_vbr', '{:+.2f}')}{pair('dneg', '{:+.2f}')}"
              f"{ir('c')}{ir('simd')}")
    print("main>arm; instr = arm over main, ours only (< 1 is fewer "
          "instructions)")


COLS = ["date", "commit", "label", "bin", "knobs", "flags", "cell", "name",
        "bd_crf", "bd_vbr", "trk_o", "trk_x", "dneg", "dpsnr", "dsize",
        "match_crf", "ix_c", "ix_simd", "cx_c", "cx_simd", "our_instr_c",
        "our_instr_simd", "wall_s"]


def record(rows, label, exe, knobs, flags, wall):
    new = not os.path.exists(CSV_PATH)
    with open(CSV_PATH, "a", newline="") as fh:
        w = csv.writer(fh)
        if new:
            w.writerow(COLS)
        for r in rows:
            def g(k, fmt):
                return fmt.format(r[k]) if r.get(k) is not None else ""
            w.writerow([datetime.date.today().isoformat(), ps.commit(), label,
                        bin_hash(exe)[:12],
                        " ".join(f"{a}={b}" for a, b in sorted(knobs.items())),
                        flags, r["id"], r["name"], g("bd_crf", "{:.2f}"),
                        g("bd_vbr", "{:.2f}"),
                        "/".join(f"{x:+.1f}" for x in r.get("trk_o", [])),
                        "/".join(f"{x:+.1f}" for x in r.get("trk_x", [])),
                        g("dneg", "{:.3f}"), g("dpsnr", "{:.3f}"),
                        g("dsize", "{:.2f}"), g("match_crf", "{:.2f}"),
                        g("ix_c", "{:.4f}"), g("ix_simd", "{:.4f}"),
                        g("cx_c", "{:.3f}"), g("cx_simd", "{:.3f}"),
                        (r.get("o_c") or {}).get("instr", ""),
                        (r.get("o_simd") or {}).get("instr", ""),
                        f"{wall:.0f}" if wall is not None else ""])

# ------------------------------------------------------------------ main


def list_cells():
    for c in ALL:
        i = pl.info(c)
        print(f"{c['id']} {c['name']:<18} {i['w']}x{i['h']} {i['frames']:>4} "
              f"frames @ {pl.fps_of(i):.3f}  {c['file']}")
        print(f"    from {c['src']} frames {c['start']}-"
              f"{c['start'] + c['frames'] - 1}"
              f"{'; VBR ladder' if c['vbr'] else ''}")
        print(f"    why: {c['why']}")
        print(f"    stands in for: {c['stands']}")


def pin_main(exe):
    """Copy a binary to the cache as --ab's main, its commit beside it."""
    d = os.path.dirname(MAIN_BIN)
    os.makedirs(d, exist_ok=True)
    subprocess.run(["cp", exe, MAIN_BIN + ".tmp"], check=True)
    os.replace(MAIN_BIN + ".tmp", MAIN_BIN)
    with open(os.path.join(d, "main.txt"), "w") as fh:
        fh.write(f"{ps.commit()} {bin_hash(MAIN_BIN)}\n")
    print(f"devset: main pinned at {ps.commit()} ({bin_hash(MAIN_BIN)[:12]})")


def main_commit():
    try:
        with open(os.path.join(os.path.dirname(MAIN_BIN), "main.txt")) as fh:
            return fh.read().split()[0]
    except (OSError, IndexError):
        return "?"


def check_files(cells):
    for c in cells:
        i = pl.info(c)
        if i["frames"] != c["frames"]:
            sys.exit(f"devset: {i['path']} holds {i['frames']} frames, "
                     f"tier D says {c['frames']}")


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--bin", default=os.environ.get("N265", ps.N265))
    ap.add_argument("--arm", default="", help="N265_*=v words for ours")
    ap.add_argument("--flags", default="", help="extra flags for ours")
    ap.add_argument("--ab", action="store_true",
                    help="the arm against main, ours only")
    ap.add_argument("--base", default=MAIN_BIN)
    ap.add_argument("--base-arm", default="")
    ap.add_argument("--pin-main", action="store_true",
                    help="copy --bin to the cache as --ab's main")
    ap.add_argument("--cells", default="")
    ap.add_argument("--no-vbr", action="store_true")
    ap.add_argument("--no-speed", action="store_true")
    ap.add_argument("--vbr-only", action="store_true",
                    help="the VBR ladders alone (no CRF ladder, no speed)")
    ap.add_argument("--show", action="store_true")
    ap.add_argument("--list", action="store_true")
    ap.add_argument("--record", action="store_true")
    ap.add_argument("--label", default="")
    ap.add_argument("--task")
    ap.add_argument("--cell")
    ap.add_argument("--sig")
    a = ap.parse_args()

    if a.task:                                   # the worker side
        fl = os.environ.get("DEVSET_FLAGS", "")
        if fl:
            orig = pl.our_cmd

            def our_cmd(out, mode, q, threads, depth=None):
                c = orig(out, mode, q, threads, depth)
                return c[:-2] + shlex.split(fl) + c[-2:]
            pl.our_cmd = our_cmd
        run_task(a.task, a.cell, a.sig)
        return
    if a.list:
        list_cells()
        return
    if a.pin_main:
        pin_main(os.path.abspath(a.bin))
        return
    if a.ab and not os.path.exists(a.base):
        sys.exit(f"devset: no main at {a.base}; on main, `make build-rel` "
                 "then `scripts/devset.py --pin-main`, or pass --base")

    if a.vbr_only:
        a.no_speed = True
        if not a.cells:
            a.cells = ",".join(c["id"] for c in ALL if c["vbr"])
    cells = [BY_ID[x] for x in a.cells.split(",")] if a.cells else ALL
    check_files(cells)
    exe = os.path.abspath(a.bin)
    knobs = parse_knobs(a.arm)
    sig = arm_sig(exe, knobs, a.flags)
    arms = {sig: (exe, knobs, a.flags)}
    bsig = None
    if a.ab:
        bexe = os.path.abspath(a.base)
        bknobs = parse_knobs(a.base_arm)
        bsig = arm_sig(bexe, bknobs, "")
        arms[bsig] = (bexe, bknobs, "")
    ps.N265 = exe
    wall = None
    if not a.show:
        todo = plan(cells, sig, not a.no_vbr, not a.no_speed,
                    want_ref_speed=not a.ab, want_crf=not a.vbr_only)
        if bsig:
            todo += [t for t in plan(cells, bsig, not a.no_vbr,
                                     not a.no_speed, want_ref_speed=False,
                                     want_crf=not a.vbr_only)
                     if t not in todo]
        print(f"devset: {len(todo)} tasks to run at {CORES} cores "
              f"(ours {bin_hash(exe)[:12]}"
              + (f" {a.arm}" if a.arm else "") + (f" {a.flags}" if a.flags
                                                  else "")
              + (f"; main {bin_hash(os.path.abspath(a.base))[:12]}"
                 if bsig else "") + ")", flush=True)
        boxlib.wait_untimed("devset")
        wall = execute(todo, arms) if todo else 0.0
        print(f"devset: re-read wall {wall / 60:.1f} min", flush=True)
    rows = [read(c, sig) for c in cells]
    lab = a.label or (a.arm or a.flags or os.path.basename(
        os.path.dirname(os.path.dirname(exe))))
    if bsig:
        brows = [read(c, bsig) for c in cells]
        bc = main_commit() if os.path.abspath(a.base) == MAIN_BIN else "?"
        print_table(brows, f"tier D, main {bc} "
                    f"({bin_hash(os.path.abspath(a.base))[:12]})")
        print_table(rows, f"tier D, arm {lab} ({bin_hash(exe)[:12]})")
        print_ab(brows, rows, "arm against main")
    else:
        print_table(rows, f"tier D at {ps.commit()}, ours {lab} "
                    f"({bin_hash(exe)[:12]}), x265 {ps.x265_version()}")
    if a.record:
        record(rows, lab, exe, knobs, a.flags, wall)
        print(f"devset: recorded to {os.path.relpath(CSV_PATH, ROOT)}")


if __name__ == "__main__":
    main()
