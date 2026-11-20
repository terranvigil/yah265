#!/usr/bin/env python3
# Copyright (c) 2026, the yah265 authors
# SPDX-License-Identifier: GPL-2.0-or-later
"""The shared parity spec (<media>/PARITY-SPEC.md), for yah265.

One cell is one clip (or segment) at one size. For every cell the
reference, x265 4.3+1 at `--preset medium`, runs at its DEFAULT rate
factor (CRF 28), and ours at `--preset medium` (our default, so no flag)
is solved onto the reference's file size within +/-1%: integer CRF first,
then fractional `--crf`. Both bitstreams are scored with parity_long's
scorer on every frame: VMAF v1 (the gate, stored in the spec's vmafneg
columns and named by the point's `metric`), PSNR-Y and SSIM. Three goals:

    G1  pure C, one thread     ours N265_ASM_OFF=all --threads 1
                               x265 --no-asm --pools none --frame-threads 1
    G2  pure C, default threads    ours N265_ASM_OFF=all; x265 --no-asm
    G3  as shipped, default threads

x265's pictures move with --frame-threads (not with --pools or --no-asm;
checked on foreman: the decoded md5 is the same at default, `--pools 3/4
--frame-threads 4` and `--no-asm`), so there are two reference anchors per
cell: `def` (the default threading, which G2 and G3 share) and `ft1`
(G1's one frame thread). Each gets its own solve of ours. Ours is
byte-identical at every thread count and with the kernels off, so one
solved bitstream serves every goal of an anchor.

Three modes, split so the untimed half can run in the day and the timed
half in a night block:

  --solve   untimed. Encodes both anchors, solves ours, scores. Memoized
            through bd_at_rate's encode and metric memo. Writes the solved
            points to docs/data/parity-spec-points.json. Runs at a small
            share: SOLVE_THREADS (default 2) for each encoder and the
            ffmpeg decode, VMAF_THREADS for libvmaf, one process at a
            time. x265's --frame-threads stays 4 (it moves the picture).
  --time    timed only. For each goal, rounds interleaved (the order of
            the two arms reversed on alternate rounds; 3 rounds for G2 and
            G3, 2 for G1), both encoders read the same y4m from stdin
            through one `cat`, output to internal disk. Median wall, the
            feeder's own fps, each arm's output md5, the one-minute load
            before every run. Appends rows to docs/data/parity-spec.csv
            and every run to docs/data/parity-spec-runs.csv, then writes
            the report.
  --report  PARITY-REPORT-yah265-<date>.md from the CSV (speed) and the
            points file (quality), in the spec's section 5.1 layout.

Selection: --tier S|M (default S), --goals 3,2,1, --classes CIF,720p,...,
--cells foreman,bus (titles). Environment: BIN or N265 (default build-rel/y265),
X265 (default x265 on PATH), SOLVE_THREADS, QUIET_MAX (150, the quiet bar
in per-core percent of total box CPU, three 10 s windows; --loadavg falls
back to the one-minute load against LOADMAX, 3.0),
FEED_MARGIN (1.5, the spec's: the feeder alone must deliver at least
this many times the faster encoder's fps, or the cell is FEEDER-BOUND and
its ratio void; the same test decides whether a cell is timed at all).
"""
import argparse
import csv
import datetime
import hashlib
import json
import math
import os
import platform
import re
import statistics
import subprocess
import sys
import shutil
import tempfile
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "scripts"))
import bd_at_rate as bdr                                  # noqa: E402
import bdcompare as bd                                    # noqa: E402
import boxlib                                             # noqa: E402

bd.SUBSAMPLE = 1                     # every frame, per the spec

SPEC = "<media>/PARITY-SPEC.md"
T7_TIER_S = "<media>/tier_s"
# tier S is read from an internal-disk copy (the T7 is USB); --solve
# makes the copy if it is missing and checks the size of every file
LOCAL_S = os.path.join(ROOT, "scratch/tier_s")
TIER_M_DIR = os.path.expanduser("~/media/throughput")

N265 = os.environ.get("BIN") or os.environ.get("N265", os.path.join(ROOT, "build-rel/y265"))
X265 = os.environ.get("X265", "x265")
REF_CRF = 28                         # x265 --help: "Default 28.0"
# the untimed share: y265 --threads, x265 --pools and ffmpeg's decode and
# filter threads; libvmaf's own count is VMAF_THREADS (default the same).
# 2 + VMAF_THREADS=1 is three cores, one process at a time.
SOLVE_THREADS = int(os.environ.get("SOLVE_THREADS", "2"))
LOADMAX = float(os.environ.get("LOADMAX", "3.0"))
LOADAVG = False                      # --loadavg: the old quiet check
FEED_MARGIN = float(os.environ.get("FEED_MARGIN", "1.5"))
TOL = 1.0                            # the spec's size bar, percent
AIM = 0.5                            # what the fractional solve aims for
PLAIN_VMAF = False                   # --plain-vmaf: 5.2a's optional column

POINTS = os.path.join(ROOT, "docs/data/parity-spec-points.json")
CSV_PATH = os.path.join(ROOT, "docs/data/parity-spec.csv")
RUNS_PATH = os.path.join(ROOT, "docs/data/parity-spec-runs.csv")
REPORT_DIR = os.path.join(ROOT, "docs/parity")
WORK = os.path.join(ROOT, "scratch/spec")

COLUMNS = ("codec,date,commit,ref,ref_version,tier,class,title,size,goal,"
           "ours_wall_s,ref_wall_s,ratio,feed_fps,ours_bytes,ref_bytes,"
           "dsize_pct,ours_vmafneg,ref_vmafneg,dvmafneg,ours_psnr_y,"
           "ref_psnr_y,dpsnr_y,bd_vmafneg_pct,ours_vmafneg_hmean,"
           "ref_vmafneg_hmean,ours_vmafneg_min,ref_vmafneg_min,"
           "ours_vmafneg_low1,ref_vmafneg_low1,ours_vmafneg_first1s,"
           "ref_vmafneg_first1s,ours_vmaf,ref_vmaf,ours_psnr_yuv611,"
           "ref_psnr_yuv611,ours_ssim_yuv611,ref_ssim_yuv611,"
           "frames_decoded_ok,rounds,valid,note").split(",")
# spec 5.2a: reported, not the bar; each is a key of a point's
# ours_supp / ref_supp and a CSV column with the arm's prefix
SUPP = ("vmafneg_hmean", "vmafneg_min", "vmafneg_low1", "vmafneg_first1s",
        "vmaf", "psnr_yuv611", "ssim_yuv611")
RUN_COLUMNS = ["date", "time", "commit", "tier", "title", "goal", "round",
               "arm", "wall_s", "bytes", "md5", "load1"]

CLASSES = ["CIF", "720p", "1080p", "2160p"]
ROUNDS = {1: 2, 2: 3, 3: 3}
GOAL_NAME = {1: "G1 pure C, 1 thread", 2: "G2 pure C, all cores",
             3: "G3 SIMD, all cores"}

# the bar (spec section 3)
BAR_MEDIAN, BAR_WORST = 1.00, 1.15
# quality holds for a (class, tier) when the MEDIAN dVMAF-NEG >= -0.5, no
# cell is below -1.0 VMAF-NEG or -1.0 dB PSNR-Y, and every size is within
# 1%; a cell between -0.5 and -1.0 is named even when its class passes
BAR_DVMAF, BAR_DVMAF_CELL, BAR_DPSNR = -0.5, -1.0, -1.0

# (tier, class, title, file); tier S from the spec's section 2 table
CELLS = [
    ("S", "CIF", "foreman", "foreman_cif.y4m"),
    ("S", "CIF", "bus", "bus_cif.y4m"),
    ("S", "CIF", "stefan", "stefan_cif.y4m"),
    ("S", "720p", "ducks", "ducks_720p.y4m"),
    ("S", "720p", "park_joy", "park_joy_720p.y4m"),
    ("S", "720p", "fourpeople", "fourpeople_720p.y4m"),
    ("S", "720p", "shields", "shields_720p.y4m"),
    ("S", "1080p", "sunflower", "sunflower_1080p.y4m"),
    ("S", "1080p", "pedestrian", "pedestrian_1080p.y4m"),
    ("S", "1080p", "riverbed", "riverbed_1080p.y4m"),
    ("S", "2160p", "meridian_2160p", "src_meridian_modern_600.y4m"),
    ("S", "2160p", "bbb_2160p", "src_bbb4k_300.y4m"),
    # tier M: the ~/media/throughput segments (Meridian's is 90 s, not 120),
    # cut at the spec's start frames past the openings (chimera 120,
    # sollevante 193, meridian 1782); the frame is in the file name
    ("M", "1080p", "chimera", "chimera_1080p_120s_from120.y4m"),
    ("M", "1080p", "sollevante", "sollevante_1080p_120s_from193.y4m"),
    ("M", "1080p", "meridian", "meridian_1080p_90s_from1782.y4m"),
    ("M", "2160p", "chimera", "chimera_2160p_120s_from120.y4m"),
    ("M", "2160p", "sollevante", "sollevante_2160p_120s_from193.y4m"),
    ("M", "2160p", "meridian", "meridian_2160p_90s_from1782.y4m"),
]

# the deviation rule: every departure from either encoder's defaults,
# stated in the note column and in the report
DEVIATIONS = [
    "ours: --crf solved onto x265's bytes (y265's own default rate "
    "control is --qp 26; it has no default CRF)",
    "both: y4m on stdin through one cat (x265 needs --input - --y4m "
    "for that)",
    "x265: --log-level error (no effect on the stream)",
]


def src_of(cell):
    tier, _, _, fn = cell
    return os.path.join(LOCAL_S if tier == "S" else TIER_M_DIR, fn)


def key_of(cell):
    return f"{cell[0]}/{cell[1]}/{cell[2]}"


def traits_of(cell):
    """The cell's flagged traits from the census (docs/media-traits.md),
    '' for none and '?' where the census has no row."""
    import media_traits
    fn = cell[3][:-4]
    if cell[0] == "S":
        return media_traits.flags(f"tierS/{fn}")
    return media_traits.flags("seg/" + re.sub(r"_\d+s$", "", fn))


def y4m_info(path):
    """(width, height, fps, frames) off the header and the file size."""
    with open(path, "rb") as f:
        hdr = f.readline()
    h = hdr.decode("latin-1")
    w = int(re.search(r" W(\d+)", h).group(1))
    ht = int(re.search(r" H(\d+)", h).group(1))
    m = re.search(r" F(\d+):(\d+)", h)
    fps = int(m.group(1)) / int(m.group(2))
    c = re.search(r" C(\S+)", h)
    if c and not c.group(1).startswith("420"):
        sys.exit(f"{path}: only 8-bit 4:2:0 is a tier S/M input")
    frame = w * ht * 3 // 2 + 6              # "FRAME\n" + planes
    frames = (os.path.getsize(path) - len(hdr)) // frame
    return w, ht, fps, frames


def md5_of(path):
    h = hashlib.md5()
    with open(path, "rb") as f:
        for blk in iter(lambda: f.read(1 << 22), b""):
            h.update(blk)
    return h.hexdigest()


def commit():
    return subprocess.run(["git", "-C", ROOT, "rev-parse", "--short",
                           "HEAD"], capture_output=True, text=True
                          ).stdout.strip()


def x265_version():
    p = subprocess.run([X265, "--version"], capture_output=True, text=True)
    m = re.search(r"version (\S+)", p.stdout + p.stderr)
    return m.group(1) if m else "?"


def select(args):
    goals = [int(g) for g in args.goals.split(",")]
    cells = [c for c in CELLS if c[0] == args.tier]
    if args.classes:
        want = args.classes.split(",")
        cells = [c for c in cells if c[1] in want]
    if args.cells:
        want = args.cells.split(",")
        cells = [c for c in cells if c[2] in want]
    if not cells:
        sys.exit("parity_spec.py: no cell selected")
    return goals, cells


# ------------------------------------------------------------------ solve

def ref_tmpl(anchor, threads):
    """The untimed reference. `def` is x265's default threading picture
    for picture (frame threads are what move it, and they are 4 on this
    box at the default; the pool only sets the wall)."""
    thr = (f"--pools {threads} --frame-threads 4" if anchor == "def"
           else "--pools none --frame-threads 1")
    return (f"{X265} --preset medium --crf {{q}} {thr} --log-level error "
            f"-o {{out}} {{src}}")


def our_tmpl(threads):
    return f"{N265} {{src}} --crf {{q}} --threads {threads} -o {{out}}"


def _fmt_q(q):
    return f"{q:.2f}".rstrip("0").rstrip(".") if isinstance(q, float) else str(q)


def solve_ours(tmpl, src, target, out, log):
    """Integer CRF bracket, then fractional --crf by the secant in log
    bytes, until the size is within AIM% of `target` (TOL% is the bar).
    Returns (crf, bytes). Every encode goes through bd_at_rate's memo."""
    sizes = {}

    def size_at(q):
        k = _fmt_q(q)
        if k not in sizes:
            sizes[k] = bdr.enc(tmpl, k, src, out)
            log(f"      ours crf {k:>6} -> {sizes[k]:>11} B "
                f"({(sizes[k] / target - 1) * 100:+.2f}%)")
        return sizes[k]

    # bracket: higher CRF = fewer bytes
    q = 28
    s = size_at(q)
    step = 3 if s > target else -3
    lo = hi = None
    while True:
        n = q + step
        if n < 0 or n > 51:
            break
        sn = size_at(n)
        if (s > target) != (sn > target):
            lo, hi = (q, n) if step > 0 else (n, q)
            break
        q, s = n, sn
    if lo is None:
        return q, s                   # off the end of the axis; caller flags
    while hi - lo > 1:
        mid = (lo + hi) // 2
        if size_at(mid) > target:
            lo = mid
        else:
            hi = mid
    best = min((abs(size_at(x) / target - 1), x) for x in (lo, hi))
    if best[0] * 100 <= AIM:
        return best[1], size_at(best[1])
    # fractional: secant in (crf, ln bytes) inside the bracket
    a, b = float(lo), float(hi)
    la, lb = math.log(size_at(lo)), math.log(size_at(hi))
    lt = math.log(target)
    cand = [(best[0], best[1])]
    for _ in range(6):
        if la == lb:
            break
        q = a + (la - lt) * (b - a) / (la - lb)
        q = round(min(max(q, a + 0.01), b - 0.01), 2)
        sq = size_at(q)
        err = abs(sq / target - 1)
        cand.append((err, q))
        if err * 100 <= AIM:
            break
        if sq > target:
            a, la = q, math.log(sq)
        else:
            b, lb = q, math.log(sq)
        if b - a < 0.015:
            break
    err, q = min(cand)
    return q, size_at(q)


def _pl():
    # imported late: parity_long imports this module at its own top
    import parity_long
    return parity_long


def pl_cell(c):
    return _pl()._mk(c[0], c[1], c[2], c[1], kind="y4m", spec_cell=c)


def gate_supp(r):
    """Spec 5.2a off a parity_long score record. Under v1 the hmean and
    min columns are the gate model's pooled ones; its pass keeps no
    per-frame scores, so low1 and first1s stay empty."""
    pl = _pl()
    s = pl.supp_of(r)
    if pl.GATE == "v1" and "v1_gate" in r:
        g = r["v1_gate"]
        s.update(vmafneg_hmean=r.get(f"v1_{g}_hmean"),
                 vmafneg_min=r.get(f"v1_{g}_min"))
    return s


def score(c, out, work):
    """(gate score, PSNR-Y, 5.2a columns) of a bitstream, by parity_long's
    scorer: VMAF v1 (or VMAF-NEG where v1 is off), PSNR and SSIM. The gate
    score lands in the spec's vmafneg columns, named by the point's
    `metric`."""
    pl = _pl()
    r = pl.score(pl_cell(c), out, work)
    g = pl.gate_score(r) if pl.has_gate(r) else None
    return g, r.get("psnr_y"), gate_supp(r)


def ensure_local_s(cells):
    os.makedirs(LOCAL_S, exist_ok=True)
    for c in cells:
        if c[0] != "S":
            continue
        dst, org = src_of(c), os.path.join(T7_TIER_S, c[3])
        if os.path.exists(dst) and (not os.path.exists(org) or
                                    os.path.getsize(dst) ==
                                    os.path.getsize(org)):
            continue
        print(f"  copying {c[3]} to internal disk", flush=True)
        subprocess.run(["cp", org, dst + ".part"], check=True)
        os.replace(dst + ".part", dst)


def load_points():
    try:
        return json.load(open(POINTS))
    except (OSError, ValueError):
        return {}


def put_point(k, pt):
    _pl().jput(POINTS, k, pt)


def shim_ffmpeg(threads):
    """bdcompare shells out to `ffmpeg` for the decode libvmaf reads and
    for the PSNR filter, at ffmpeg's default thread count (all cores). The
    untimed share is capped by a PATH shim that pins both to `threads`."""
    d = os.path.join(WORK, "bin")
    os.makedirs(d, exist_ok=True)
    real = None
    for p in os.environ.get("PATH", "").split(":"):
        f = os.path.join(p, "ffmpeg")
        if p != d and os.access(f, os.X_OK):
            real = f
            break
    if not real:
        sys.exit("parity_spec.py: no ffmpeg on PATH")
    with open(os.path.join(d, "ffmpeg"), "w") as f:
        f.write(f"#!/bin/bash\nexec {real} -filter_threads {threads} "
                f"-threads {threads} \"$@\"\n")
    os.chmod(os.path.join(d, "ffmpeg"), 0o755)
    os.environ["PATH"] = d + ":" + os.environ["PATH"]


def rebind(k, pt, c):
    """A point solved on another binary still holds when this binary
    writes the same bitstream at the solved crf: a rebuild that moves no
    stream (a docs merge, a knob that acts only under VBR) shouldn't cost
    a re-solve. One untimed encode, before the quiet check, decides."""
    work = tempfile.mkdtemp(prefix="rebind.", dir=WORK)
    try:
        out = os.path.join(work, "ours.hevc")
        bdr.enc(our_tmpl(SOLVE_THREADS), _fmt_q(pt["ours_crf"]),
                src_of(c), out)
        same = os.path.exists(out) and md5_of(out) == pt["ours_md5"]
    finally:
        shutil.rmtree(work, ignore_errors=True)
    print(f"  {k}: solved on binary {pt['bin']}; this one writes "
          f"{'the same' if same else 'a different'} stream at crf "
          f"{_fmt_q(pt['ours_crf'])}", flush=True)
    return same


def rescore(k, pt, c, log):
    """A point scored on an older metric takes the gate one without a
    re-solve. Both bitstreams come back at the solved crf (bd_at_rate's
    memo, or an encode) and must be the solved md5. Only the gate pass
    runs: PSNR and SSIM of the same bytes are kept."""
    pl = _pl()
    cell = pl_cell(c)
    n = pl.info(cell)["frames"]
    work = tempfile.mkdtemp(prefix="rescore.", dir=WORK)
    rec = {}
    try:
        for arm, tmpl, q in (
                ("ref", ref_tmpl(pt["anchor"], SOLVE_THREADS), pt["ref_crf"]),
                ("ours", our_tmpl(SOLVE_THREADS), pt["ours_crf"])):
            out = os.path.join(work, arm + ".hevc")
            bdr.enc(tmpl, _fmt_q(q), src_of(c), out)
            m = md5_of(out) if os.path.exists(out) else None
            if m != pt[f"{arm}_md5"]:
                sys.exit(f"parity_spec.py: {k}: {arm} at crf {_fmt_q(q)} "
                         f"writes {m}, the solved point has "
                         f"{pt[f'{arm}_md5']}; not rescored")
            rec[arm] = pl.score_v1(cell, out, work, 0, n, 1)
    finally:
        shutil.rmtree(work, ignore_errors=True)
    for arm, r in rec.items():
        pt.setdefault(f"{arm}_neg", pt[f"{arm}_vmafneg"])
        pt[f"{arm}_vmafneg"] = pl.gate_score(r)
        s = pt.setdefault(f"{arm}_supp", {})
        g = r["v1_gate"]
        s.update(vmafneg_hmean=r.get(f"v1_{g}_hmean"),
                 vmafneg_min=r.get(f"v1_{g}_min"),
                 vmafneg_low1=None, vmafneg_first1s=None, vmaf=None)
        s["frames_ok"] = s.get("frames_ok", True) and r["v1_frames"] == n
    pt["metric"] = pl.GATE
    pt["rescored"] = datetime.datetime.now().isoformat(timespec="seconds")
    put_point(k, pt)
    log(f"  {k}: rescored {pl.MNAME} ours {pt['ours_vmafneg']:.2f} x265 "
        f"{pt['ref_vmafneg']:.2f} "
        f"(d {pt['ours_vmafneg'] - pt['ref_vmafneg']:+.2f}; "
        f"NEG d {pt['ours_neg'] - pt['ref_neg']:+.2f})")


def do_solve(args):
    goals, cells = select(args)
    anchors = []
    if any(g in (2, 3) for g in goals):
        anchors.append("def")
    if 1 in goals:
        anchors.append("ft1")
    boxlib.wait_untimed("parity_spec.py --solve")
    ensure_local_s(cells)
    shim_ffmpeg(SOLVE_THREADS)
    os.environ.setdefault("VMAF_THREADS", str(SOLVE_THREADS))
    os.environ["BD_FIFO"] = "1"      # no disk for a 4K decode
    os.makedirs(WORK, exist_ok=True)
    pts = load_points()
    cm = commit()
    bh = bdr._bin_hash(N265)

    def log(s):
        print(s, flush=True)

    for c in cells:
        src = src_of(c)
        w, h, fps, frames = y4m_info(src)
        for an in anchors:
            k = f"{key_of(c)}/{an}"
            old = pts.get(k)
            if (old and old.get("bin") == bh and not args.force
                    and old.get("src_bytes") == os.path.getsize(src)):
                log(f"  {k}: solved already (crf {old['ours_crf']}, "
                    f"{old['dsize_pct']:+.2f}%)")
                if old.get("metric", "neg") != _pl().GATE:
                    if _pl().GATE != "v1":
                        sys.exit(f"parity_spec.py: {k} holds "
                                 f"{old.get('metric', 'neg')} scores and "
                                 "v1 is off; nothing to rescore with")
                    rescore(k, old, c, log)
                continue
            t0 = time.time()
            work = tempfile.mkdtemp(prefix="spec.", dir=WORK)
            log(f"  {k}: {w}x{h} {frames} frames @ {fps:.3f}; traits "
                f"[{traits_of(c)}]")
            rt = ref_tmpl(an, SOLVE_THREADS)
            ro = os.path.join(work, "ref.hevc")
            rb = bdr.enc(rt, REF_CRF, src, ro)
            if rb <= 0:
                log(f"  {k}: reference encode FAILED")
                continue
            rmd5 = md5_of(ro)
            rg, rpsnr, rsupp = score(c, ro, work)
            log(f"      x265 crf {REF_CRF} -> {rb} B  {_pl().MNAME} {rg}  "
                f"PSNR-Y {rpsnr}")
            ot = our_tmpl(SOLVE_THREADS)
            oo = os.path.join(work, "ours.hevc")
            q, ob = solve_ours(ot, src, rb, oo, log)
            bdr.enc(ot, _fmt_q(q), src, oo)
            omd5 = md5_of(oo)
            og, opsnr, osupp = score(c, oo, work)
            ds = (ob / rb - 1) * 100
            pts[k] = {
                "tier": c[0], "class": c[1], "title": c[2], "anchor": an,
                "src": src, "src_bytes": os.path.getsize(src),
                "size": f"{w}x{h}", "frames": frames, "fps": fps,
                "ref_crf": REF_CRF, "ref_bytes": rb, "ref_md5": rmd5,
                "ref_vmafneg": rg, "ref_psnr_y": rpsnr,
                "ours_crf": q, "ours_bytes": ob, "ours_md5": omd5,
                "ours_vmafneg": og, "ours_psnr_y": opsnr,
                "dsize_pct": round(ds, 3), "commit": cm, "bin": bh,
                "solved": datetime.datetime.now().isoformat(
                    timespec="seconds"),
                "solve_s": round(time.time() - t0, 1),
                "ref_supp": rsupp, "ours_supp": osupp,
                "metric": _pl().GATE,
            }
            put_point(k, pts[k])
            dv = (og - rg) if None not in (og, rg) else None
            dp = (opsnr - rpsnr) if None not in (opsnr, rpsnr) else None
            log(f"  {k}: ours crf {_fmt_q(q)} {ob} B ({ds:+.2f}%)  "
                f"d{_pl().MNAME} {dv:+.2f}  dPSNR-Y {dp:+.2f}  "
                f"[{time.time() - t0:.0f} s]" if dv is not None and
                dp is not None else f"  {k}: scoring FAILED")
            subprocess.run(["rm", "-rf", work])


# ------------------------------------------------------------------- time

def load1():
    return os.getloadavg()[0]


def wait_quiet(limit=300):
    """(quiet, what was read). Total box CPU by default (owner rule:
    quiet is judged on total CPU, not load average), retried until
    `limit` seconds have passed; --loadavg reads the one-minute load
    against LOADMAX the way this script did before."""
    t0 = time.time()
    if LOADAVG:
        while load1() > LOADMAX and time.time() - t0 < limit:
            time.sleep(10)
        ld = load1()
        return ld <= LOADMAX, (f"load {ld:.2f}", f"load {ld:.1f} (over "
                                                f"{LOADMAX})")
    while True:
        ok, smp = boxlib.quiet()
        if ok or time.time() - t0 >= limit:
            break
    return ok, (boxlib.fmt_quiet(ok, smp),
                f"box CPU {max(smp or [0]):.0f}% (over "
                f"{boxlib.QUIET_MAX:.0f}%)")


def feed_fps(src, frames):
    """The feeder alone: `cat src | cat > /dev/null`, best of two (the
    first also warms the page cache for both arms)."""
    best = None
    for _ in range(2):
        t0 = time.perf_counter()
        subprocess.run(f"cat '{src}' | cat > /dev/null", shell=True,
                       check=True)
        dt = time.perf_counter() - t0
        best = dt if best is None else min(best, dt)
    return frames / best


def arm_cmd(arm, goal, pt, out):
    pure_c, t1 = goal in (1, 2), goal == 1
    env = dict(os.environ)
    env.pop("N265_ASM_OFF", None)
    if arm == "ours":
        cmd = [N265, "-", "--crf", _fmt_q(pt["ours_crf"]), "-o", out]
        if t1:
            cmd += ["--threads", "1"]
        if pure_c:
            env["N265_ASM_OFF"] = "all"
    else:
        cmd = [X265, "--input", "-", "--y4m", "--preset", "medium",
               "--crf", str(pt["ref_crf"]), "--log-level", "error",
               "-o", out]
        if t1:
            cmd += ["--pools", "none", "--frame-threads", "1"]
        if pure_c:
            cmd += ["--no-asm"]
    return cmd, env


def timed_run(src, cmd, env):
    """Wall from the feeder's start to the encoder's exit."""
    t0 = time.perf_counter()
    feed = subprocess.Popen(["cat", src], stdout=subprocess.PIPE)
    enc = subprocess.Popen(cmd, stdin=feed.stdout, env=env,
                           stdout=subprocess.DEVNULL,
                           stderr=subprocess.DEVNULL)
    feed.stdout.close()
    rc = enc.wait()
    dt = time.perf_counter() - t0
    feed.wait()
    return dt, rc


def supp_cols(pt):
    out = {}
    for arm in ("ours", "ref"):
        sp = pt.get(f"{arm}_supp", {})
        for c in SUPP:
            x = sp.get(c)
            out[f"{arm}_{c}"] = ("" if x is None else
                                 f"{x:.4f}" if c == "ssim_yuv611"
                                 else f"{x:.2f}")
    return out


def append_csv(path, cols, rows):
    new = not os.path.exists(path)
    with open(path, "a", newline="") as f:
        w = csv.DictWriter(f, fieldnames=cols)
        if new:
            w.writeheader()
        for r in rows:
            w.writerow(r)


def f2(x):
    return "" if x is None else f"{x:.2f}"


def do_time(args):
    goals, cells = select(args)
    pts = load_points()
    today = datetime.date.today().isoformat()
    cm = commit()
    ver = x265_version()
    bh = bdr._bin_hash(N265)
    outdir = os.path.join(WORK, "out")
    os.makedirs(outdir, exist_ok=True)
    g1c = args.g1_classes.split(",") if args.g1_classes else None
    for c in cells:
        for g in goals:
            if g == 1 and g1c and c[1] not in g1c:
                continue
            k = f"{key_of(c)}/{'ft1' if g == 1 else 'def'}"
            if k not in pts:
                sys.exit(f"parity_spec.py: {k} is not solved; run --solve")
            if pts[k]["bin"] != bh and not rebind(k, pts[k], c):
                sys.exit(f"parity_spec.py: {k} was solved on another "
                         f"binary that writes a different stream; "
                         f"re-run --solve")
            if pts[k]["src_bytes"] != os.path.getsize(src_of(c)):
                sys.exit(f"parity_spec.py: {src_of(c)} changed size")
    # the quiet check is made ONCE, before the first run: the box CPU
    # during the block is our own arms, so a per-run check would read our
    # previous arm and stall the block
    quiet, (qline, qnote) = wait_quiet()
    print(f"timed: {len(cells)} cells, goals {goals}, commit {cm}, "
          f"x265 {ver}, {qline}", flush=True)
    lock = boxlib.timing_lock("parity_spec.py --time")
    lock.__enter__()

    feed = {}
    for c in cells:
        feed[key_of(c)] = feed_fps(src_of(c), y4m_info(src_of(c))[3])
        print(f"  feeder {key_of(c)}: {feed[key_of(c)]:.0f} fps",
              flush=True)

    # the spec's feeder rule decides whether a cell is timed at all: a
    # cell whose feeder is under FEED_MARGIN x the faster arm's fps in an
    # earlier block is not timed again (its ratio would be void)
    prior = {}
    for r in read_csv(RUNS_PATH):
        w = fnum(r["wall_s"])
        if w and r["md5"] != "FAILED":
            k = (r["tier"], r["title"], r["goal"])
            prior[k] = min(prior.get(k, w), w)

    all_cells = cells
    for g in goals:
        an = "ft1" if g == 1 else "def"
        # --g1-classes: G1 is the long leg (one thread, no SIMD) and a
        # night block may hold it for the small classes only
        cells = [c for c in all_cells if g != 1 or not g1c
                 or c[1] in g1c]
        walls = {key_of(c): {"ours": [], "ref": []} for c in cells}
        md5s = {key_of(c): {"ours": set(), "ref": set()} for c in cells}
        size = {key_of(c): {} for c in cells}
        loud = {key_of(c): [] for c in cells}
        fails = {key_of(c): [] for c in cells}
        bound = {}                       # cell -> the fps that voided it
        for c in cells:
            pw = prior.get((c[0], c[2], f"G{g}"))
            fr = y4m_info(src_of(c))[3]
            if pw and feed[key_of(c)] < FEED_MARGIN * fr / pw:
                bound[key_of(c)] = fr / pw
                print(f"  G{g} {key_of(c)}: FEEDER-BOUND on an earlier "
                      f"block ({feed[key_of(c)]:.0f} < {FEED_MARGIN} x "
                      f"{fr / pw:.0f} fps), not timed", flush=True)
        runs = []
        t_round = None
        for r in range(ROUNDS[g]):
            # --deadline: a round that would not finish is not started, so
            # every row keeps whole, interleaved rounds (fewer of them)
            if (args.deadline and t_round is not None
                    and time.time() + t_round > args.deadline):
                print(f"  G{g}: round {r + 1} would end past the deadline; "
                      f"stopping at {r} round(s)", flush=True)
                break
            t0r = time.time()
            for c in cells:
                ck = key_of(c)
                if ck in bound:
                    continue
                if args.deadline and time.time() >= args.deadline:
                    print(f"  G{g} r{r + 1} {ck}: past the deadline, "
                          f"skipped", flush=True)
                    continue
                pt = pts[f"{ck}/{an}"]
                src = src_of(c)
                order = ("ours", "ref") if r % 2 == 0 else ("ref", "ours")
                for arm in order:
                    out = os.path.join(outdir, f"{c[0]}_{c[2]}_g{g}_{arm}"
                                               f"_r{r + 1}.hevc")
                    cmd, env = arm_cmd(arm, g, pt, out)
                    ld = load1()
                    if not quiet:
                        loud[ck].append(ld)
                    dt, rc = timed_run(src, cmd, env)
                    ok = rc == 0 and os.path.exists(out)
                    m = md5_of(out) if ok else "FAILED"
                    b = os.path.getsize(out) if ok else 0
                    if ok:
                        walls[ck][arm].append(dt)
                        md5s[ck][arm].add(m)
                        size[ck][arm] = b
                    else:
                        fails[ck].append(f"{arm} r{r + 1} rc {rc}")
                    now = datetime.datetime.now()
                    runs.append({"date": today,
                                 "time": now.strftime("%H:%M:%S"),
                                 "commit": cm, "tier": c[0], "title": c[2],
                                 "goal": f"G{g}", "round": r + 1,
                                 "arm": arm, "wall_s": f"{dt:.3f}",
                                 "bytes": b, "md5": m, "load1": f"{ld:.2f}"})
                    print(f"  G{g} r{r + 1} {ck:<22} {arm:<4} {dt:9.3f} s "
                          f"{b:>11} B md5 {m} load {ld:.2f}", flush=True)
                    if ok:
                        os.unlink(out)
                # judged on this round's own walls: the rest of a
                # feeder-bound cell's rounds are not run
                ws = [min(walls[ck][a]) for a in ("ours", "ref")
                      if walls[ck][a]]
                if ws:
                    fast = pt["frames"] / min(ws)
                    if feed[ck] < FEED_MARGIN * fast:
                        bound[ck] = fast
                        print(f"  G{g} {ck}: FEEDER-BOUND ({feed[ck]:.0f} "
                              f"< {FEED_MARGIN} x {fast:.0f} fps), ratio "
                              f"void, no more rounds", flush=True)
            t_round = time.time() - t0r
        append_csv(RUNS_PATH, RUN_COLUMNS, runs)

        rows = []
        for c in cells:
            ck = key_of(c)
            pt = pts[f"{ck}/{an}"]
            wo, wr = walls[ck]["ours"], walls[ck]["ref"]
            if (not wo or not wr) and ck not in bound:
                continue                 # not measured: no row
            mo = statistics.median(wo) if wo else None
            mr = statistics.median(wr) if wr else None
            ratio = mo / mr if mo and mr and ck not in bound else None
            ob = size[ck].get("ours", pt["ours_bytes"])
            rb = size[ck].get("ref", pt["ref_bytes"])
            ds = (ob / rb - 1) * 100
            notes = []
            flags = []
            if ck in bound:
                flags.append("feeder-bound")
                notes.append(f"feeder {feed[ck]:.0f} fps < {FEED_MARGIN}x "
                             f"the faster arm's {bound[ck]:.0f}; ratio "
                             f"void")
            if loud[ck]:
                flags.append("not-quiet")
                notes.append(f"{qnote} when the block started")
            fok = (pt.get("ours_supp", {}).get("frames_ok")
                   and pt.get("ref_supp", {}).get("frames_ok"))
            if not fok:
                flags.append("frame-mismatch")
                notes.append("decode did not return every frame: ours "
                             f"{pt.get('ours_supp', {}).get('frames')}, x265 "
                             f"{pt.get('ref_supp', {}).get('frames')} of "
                             f"{pt['frames']}")
            if abs(ds) > TOL:
                flags.append("size-mismatch")
            if min(len(wo), len(wr)) < 2 and ck not in bound:
                flags.append("single-run")
                notes.append(f"one run only ({min(len(wo), len(wr))} per "
                             f"arm)")
            if fails[ck]:
                notes.append("failed: " + "; ".join(fails[ck]))
            # owner rule: above one thread, identity is a WARNING; only a
            # --threads 1 run-to-run mismatch fails (the row is then left
            # out of every speed reading, see speed_cell)
            for arm, name in (("ours", "ours"), ("ref", "x265")):
                if len(md5s[ck][arm]) > 1:
                    notes.append(("FAIL: --threads 1 run-to-run md5 "
                                  f"mismatch ({name})") if g == 1 else
                                 f"warning: {name} md5 differs between "
                                 f"rounds")
            if md5s[ck]["ours"] and md5s[ck]["ours"] != {pt["ours_md5"]}:
                notes.append("warning: ours md5 differs from the solved "
                             "bitstream (solved at --threads "
                             f"{SOLVE_THREADS})")
            if "ref" in size[ck] and abs(rb / pt["ref_bytes"] - 1) > 1e-3:
                notes.append("x265 bytes differ from the solved anchor")
            notes.append(f"ours --crf {_fmt_q(pt['ours_crf'])} (solved)")
            notes.append("both y4m on stdin via cat")
            dv = pt["ours_vmafneg"] - pt["ref_vmafneg"]
            dp = pt["ours_psnr_y"] - pt["ref_psnr_y"]
            rows.append({
                "codec": "yah265", "date": today, "commit": cm,
                "ref": "x265 --preset medium", "ref_version": ver,
                "tier": c[0], "class": c[1], "title": c[2],
                "size": pt["size"], "goal": f"G{g}",
                "ours_wall_s": f"{mo:.3f}" if mo else "",
                "ref_wall_s": f"{mr:.3f}" if mr else "",
                "ratio": f2(ratio), "feed_fps": f"{feed[ck]:.0f}",
                "ours_bytes": ob, "ref_bytes": rb, "dsize_pct": f2(ds),
                "ours_vmafneg": f2(pt["ours_vmafneg"]),
                "ref_vmafneg": f2(pt["ref_vmafneg"]), "dvmafneg": f2(dv),
                "ours_psnr_y": f2(pt["ours_psnr_y"]),
                "ref_psnr_y": f2(pt["ref_psnr_y"]), "dpsnr_y": f2(dp),
                "bd_vmafneg_pct": "",
                **supp_cols(pt), "frames_decoded_ok": "yes" if fok else "no",
                "rounds": min(len(wo), len(wr)),
                "valid": flags[0] if flags else "yes",
                "note": "; ".join(notes),
            })
            print(f"  G{g} {ck:<22} ratio {f2(ratio)}  ours {f2(mo)} s  "
                  f"x265 {f2(mr)} s  dsize {ds:+.2f}%  "
                  f"valid {rows[-1]['valid']}", flush=True)
        append_csv(CSV_PATH, COLUMNS, rows)
    lock.__exit__(None, None, None)
    write_report(today)


# ----------------------------------------------------------------- report

def read_csv(path):
    try:
        with open(path, newline="") as f:
            return list(csv.DictReader(f))
    except OSError:
        return []


def fnum(s):
    try:
        return float(s)
    except (TypeError, ValueError):
        return None


def latest_rows(rows, upto):
    """The newest row per (tier, title, goal) on or before `upto`."""
    out = {}
    for r in rows:
        if r["date"] <= upto:
            out[(r["tier"], r["title"], r["goal"])] = r
    return out


def speed_cell(rs):
    """(median, worst, worst title, verdict) over valid rows."""
    val = [(fnum(r["ratio"]), r["title"]) for r in rs
           if r["valid"] == "yes" and fnum(r["ratio"]) is not None
           and "FAIL:" not in r["note"]]
    if not val:
        return None
    med = statistics.median(v for v, _ in val)
    worst = max(val)
    ok = med <= BAR_MEDIAN and worst[0] <= BAR_WORST
    return med, worst[0], worst[1], ok


def quality_of(q, cls, tier):
    """(holds, worst title, worst dVMAF-NEG) for a class and tier, or None
    while any of its cells is unsolved: quality holds only when every cell
    is inside the bar."""
    rs = [r for r in q if r["tier"] == tier and r["class"] == cls]
    n = sum(1 for c in CELLS if c[0] == tier and c[1] == cls)
    if not rs or len(rs) < n:
        return None
    w = min(rs, key=lambda r: r["dv"])
    return quality_holds(rs), w["title"], w["dv"]


def quality_holds(rs):
    return (statistics.median(r["dv"] for r in rs) >= BAR_DVMAF
            and all(r["dv"] >= BAR_DVMAF_CELL and r["dp"] >= BAR_DPSNR
                    and abs(r["ds"]) <= TOL for r in rs))


def quality_watch(rs):
    """Cells between -0.5 and -1.0 VMAF-NEG: named even when the class
    passes."""
    return [r for r in rs if BAR_DVMAF_CELL <= r["dv"] < BAR_DVMAF]


def machine():
    brand = subprocess.run(["sysctl", "-n", "machdep.cpu.brand_string"],
                           capture_output=True, text=True).stdout.strip()
    ncpu = os.cpu_count()
    ver = platform.mac_ver()[0] or platform.release()
    return f"{brand}, {ncpu} cores, macOS {ver}"


def write_report(date):
    rows = read_csv(CSV_PATH)
    runs = [r for r in read_csv(RUNS_PATH) if r["date"] == date]
    pts = load_points()
    cur = latest_rows(rows, date)
    prev_dates = sorted({r["date"] for r in rows if r["date"] < date})
    prev = latest_rows(rows, prev_dates[-1]) if prev_dates else {}
    cm = (sorted({r["commit"] for r in cur.values()}) or [commit()])
    ver = next((r["ref_version"] for r in cur.values()), None) \
        or x265_version()
    block = (f"{runs[0]['time'][:5]}-{runs[-1]['time'][:5]}" if runs
             else "none (untimed; speed not measured yet)")
    tiers = sorted({c[0] for c in CELLS}, key="SML".index)

    def rows_for(cls, tier, goal):
        return [r for (t, _, g), r in cur.items()
                if t == tier and g == goal and r["class"] == cls]

    L = []
    L.append(f"# yah265 parity report, {date}")
    L.append("")
    L.append(f"Encoder: yah265 y265 {', '.join(cm)}, preset medium.  "
             f"Reference: x265 {ver}, preset medium.")
    L.append(f"Machine: {machine()}.  Night block: {block}.")
    L.append("")
    L.append("How to read: a speed ratio is our time / the reference's "
             "time at the same file size; below 1.00 we're faster. Pass = "
             "median <= 1.00 and worst <= 1.15, quality no worse than -0.5 "
             "VMAF-NEG (median of the class; no cell below -1.0 VMAF-NEG "
             "or -1.0 dB PSNR-Y; sizes within 1%). \"-\" = not measured "
             "yet.")
    L.append("")
    L.append("Deviations from either encoder's defaults: "
             + "; ".join(DEVIATIONS) + ".")
    L.append("")

    # the spec's unit of a pass is a (class, tier, goal) combination: its
    # median <= 1.00, its worst <= 1.15, and quality holds for that class
    # and tier. m counts only combinations with valid measurements.
    q = quality_rows(pts)
    combos = {}                  # (cls, tier, g) -> (pass, why) or None
    for tier in tiers:
        for cls in CLASSES:
            if not any(c[0] == tier and c[1] == cls for c in CELLS):
                continue
            qo = quality_of(q, cls, tier)
            for g in (1, 2, 3):
                sp = speed_cell(rows_for(cls, tier, f"G{g}"))
                if sp is None or qo is None:
                    combos[(cls, tier, g)] = None
                    continue
                why = []
                if not sp[3]:
                    why.append(f"worst {sp[2]} {sp[1]:.2f}, median "
                               f"{sp[0]:.2f}")
                if not qo[0]:
                    why.append(f"quality, worst {qo[1]} {qo[2]:+.2f}")
                combos[(cls, tier, g)] = (not why, "; ".join(why))

    L.append("## Verdict")
    L.append("```")
    for g in (1, 2, 3):
        passing, opening = [], []
        for (cls, tier, gg), v in combos.items():
            if gg != g or v is None:
                continue
            tag = f"{cls}-{tier}"
            (passing if v[0] else opening).append(
                tag if v[0] else f"{tag} ({v[1]})")
        txt = []
        if passing:
            txt.append("PASS in " + ", ".join(passing))
        if opening:
            txt.append("OPEN in " + ", ".join(opening))
        L.append(f"{GOAL_NAME[g] + ':':<22} "
                 + (" / ".join(txt) if txt else "-"))
    qp, qo = [], []
    for tier in tiers:
        for cls in CLASSES:
            if not any(c[0] == tier and c[1] == cls for c in CELLS):
                continue
            rs = [r for r in q if r["tier"] == tier and r["class"] == cls]
            if not rs:
                continue
            n = sum(1 for c in CELLS if c[0] == tier and c[1] == cls)
            tag = f"{cls}-{tier}" + ("" if len(rs) == n
                                     else f" ({len(rs)} of {n} cells)")
            if quality_holds(rs):
                qp.append(tag)
            else:
                w = min(rs, key=lambda r: r["dv"])
                qo.append(f"{tag} (worst {w['title']} {w['dv']:+.2f})")
    qtxt = " / ".join(([("holds in " + ", ".join(qp))] if qp else [])
                      + ([("OPEN in " + ", ".join(qo))] if qo else [])) \
        or "-"
    qw = quality_watch(q)
    if qw:
        qtxt += "; between -0.5 and -1.0: " + ", ".join(
            f"{r['title']} {r['dv']:+.2f}" for r in qw)
    L.append(f"{'Quality:':<22} {qtxt}")
    meas = [v for v in combos.values() if v is not None]
    npass = sum(1 for v in meas if v[0])
    nun = sum(1 for v in combos.values() if v is None)
    full = nun == 0 and npass == len(meas)
    L.append(f"{'Full parity:':<22} {'yes' if full else 'no'}, {npass} of "
             f"{len(meas)} combinations passing; {nun} of {len(combos)} "
             f"not measured (listed below; tiers L and C not yet in the "
             f"harness)")
    L.append("```")
    L.append("")

    # speed
    L.append("## Speed (median / worst ratio, per class and tier)")
    L.append("| class | tier | G1 | G2 | G3 | verdict |")
    L.append("|---|---|---|---|---|---|")
    for tier in tiers:
        for cls in CLASSES:
            if not any(c[0] == tier and c[1] == cls for c in CELLS):
                continue
            cols, verd, one = [], [], False
            for g in (1, 2, 3):
                rs = rows_for(cls, tier, f"G{g}")
                one |= any(fnum(r["rounds"]) == 1 for r in rs)
                s = speed_cell(rs)
                if s is None:
                    cols.append("-")
                    continue
                cols.append(f"{s[0]:.2f} / {s[1]:.2f}")
                verd.append(None if s[3] else f"G{g} {s[2]} {s[1]:.2f}")
            if not verd:
                v = "-"
            elif all(x is None for x in verd):
                v = "PASS" if len(verd) == 3 else "PASS so far"
            else:
                v = "OPEN (worst: " + ", ".join(x for x in verd if x) + ")"
            if one:
                v += "; one run only"
            L.append(f"| {cls} | {tier} | " + " | ".join(cols) + f" | {v} |")
    L.append("")

    # quality
    L.append("## Quality at matched size (VMAF-NEG delta median / worst; "
             "PSNR-Y worst)")
    L.append("| class | tier | dVMAF | dPSNR-Y | worst cell | verdict |")
    L.append("|---|---|---|---|---|---|")
    for tier in tiers:
        for cls in CLASSES:
            rs = [r for r in q if r["tier"] == tier and r["class"] == cls]
            if not any(c[0] == tier and c[1] == cls for c in CELLS):
                continue
            if not rs:
                L.append(f"| {cls} | {tier} | - | - | - | - |")
                continue
            med = statistics.median(r["dv"] for r in rs)
            w = min(rs, key=lambda r: r["dv"])
            wp = min(rs, key=lambda r: r["dp"])
            ok = quality_holds(rs)
            n = sum(1 for c in CELLS if c[0] == tier and c[1] == cls)
            part = "" if len(rs) == n else f" ({len(rs)} of {n} cells)"
            v = "PASS" if ok else (f"OPEN (worst: {w['title']} "
                                   f"{w['dv']:+.2f}, {wp['title']} "
                                   f"{wp['dp']:+.2f} dB)")
            qw = quality_watch(rs)
            if qw:
                v += "; between -0.5 and -1.0: " + ", ".join(
                    f"{r['title']} {r['dv']:+.2f}" for r in qw)
            L.append(f"| {cls} | {tier} | {med:+.2f} / {w['dv']:+.2f} | "
                     f"{wp['dp']:+.2f} | {w['title']} | {v}{part} |")
    L.append("")
    fl = [(c, traits_of(c)) for c in CELLS if c[0] in tiers]
    fl = [(c, t) for c, t in fl if t]
    if fl:
        L.append("Cells whose media carries a trait that can steer this "
                 "reading (docs/media-traits.md; a result that sits only "
                 "in such a trait is reported as that): " + "; ".join(
                     f"{c[2]} {c[1]} {c[0]} [{t}]" for c, t in fl) + ".")
        L.append("")
    L.append("Quality is read at the `def` anchor (x265 at its default "
             "threading, the point G2 and G3 are timed at). G1's anchor "
             "(one frame thread) is its own solve; its deltas are in the "
             "CSV.")
    L.append("")

    # spec 5.2a: supplementary, reported only
    L.append("## Supplementary quality (not the bar; median ours minus "
             "x265 over the class)")
    L.append("| class | tier | hmean NEG | min NEG | 1%-low NEG | "
             "first-1s NEG | VMAF | PSNR-YUV 6:1:1 | SSIM-YUV 6:1:1 | "
             "frames |")
    L.append("|---|---|---|---|---|---|---|---|---|---|")
    for tier in tiers:
        for cls in CLASSES:
            if not any(c[0] == tier and c[1] == cls for c in CELLS):
                continue
            ps = [p for p in pts.values() if p.get("anchor") == "def"
                  and p["tier"] == tier and p["class"] == cls
                  and "ours_supp" in p and "ref_supp" in p]
            if not ps:
                L.append(f"| {cls} | {tier} | - | - | - | - | - | - | - "
                         f"| - |")
                continue
            cols = []
            for c in SUPP:
                ds_ = [p["ours_supp"][c] - p["ref_supp"][c] for p in ps
                       if p["ours_supp"].get(c) is not None
                       and p["ref_supp"].get(c) is not None]
                cols.append("-" if not ds_ else
                            f"{statistics.median(ds_):+.4f}"
                            if c == "ssim_yuv611" else
                            f"{statistics.median(ds_):+.2f}")
            ok = sum(1 for p in ps if p["ours_supp"].get("frames_ok")
                     and p["ref_supp"].get("frames_ok"))
            cols.append(f"{ok}/{len(ps)} ok")
            L.append(f"| {cls} | {tier} | " + " | ".join(cols) + " |")
    L.append("")

    # changed
    L.append("## Changed since the last report")
    ch = []
    if not prev:
        ch.append("First report under the shared spec; nothing to compare "
                  "with. yah265's older board and goal legs continue as "
                  "internal progress lines only.")
    else:
        for (t, title, g), r in sorted(cur.items()):
            p = prev.get((t, title, g))
            a, b = fnum(p["ratio"]) if p else None, fnum(r["ratio"])
            if a is not None and b is not None and abs(b - a) >= 0.05:
                ch.append(f"{title} ({r['class']}-{t}) {g}: ratio "
                          f"{a:.2f} -> {b:.2f} ({p['commit']} -> "
                          f"{r['commit']})")
        ch = ch[:5] or ["No ratio moved by 0.05 or more."]
    L += ch
    L.append("")

    # not valid
    L.append("## Not valid or not measured")
    nv = []
    for (t, title, g), r in sorted(cur.items()):
        if r["valid"] != "yes" or "FAIL:" in r["note"]:
            nv.append(f"- {title} ({r['class']}-{t}) {g}: {r['valid']}; "
                      f"{r['note']}")
    missing = {}
    for c in CELLS:
        for g in (1, 2, 3):
            if (c[0], c[2], f"G{g}") not in cur:
                missing.setdefault((c[0], f"G{g}"), []).append(
                    f"{c[2]} {c[1]}")
    for (t, g), names in sorted(missing.items()):
        nv.append(f"- tier {t} {g}, speed not measured: "
                  + ", ".join(names))
    un = [f"{cls}-{tier} G{g}" for (cls, tier, g), v in combos.items()
          if v is None]
    if un:
        nv.append("- combinations not measured (not counted in m): "
                  + ", ".join(un))
    for k, p in sorted(pts.items()):
        for arm in ("ours", "ref"):
            sp = p.get(f"{arm}_supp")
            if sp is not None and not sp.get("frames_ok"):
                nv.append(f"- {p['title']} ({p['class']}-{p['tier']}, "
                          f"{p['anchor']} anchor): frame-mismatch, "
                          f"{'ours' if arm == 'ours' else 'x265'} decode "
                          f"gave {sp.get('frames')} of {p['frames']} "
                          f"frames")
    for k, p in sorted(pts.items()):
        if p.get("anchor") == "def" and abs(p["dsize_pct"]) > TOL:
            nv.append(f"- {p['title']} ({p['class']}-{p['tier']}): "
                      f"size-mismatch, ours {p['dsize_pct']:+.2f}% at "
                      f"--crf {_fmt_q(p['ours_crf'])} (nearest reachable "
                      f"size; the solve found no CRF within 1%)")
    unsolved = [f"{c[2]} {c[1]}-{c[0]}" for c in CELLS
                if f"{key_of(c)}/def" not in pts]
    if unsolved:
        nv.append("- quality not solved yet: " + ", ".join(unsolved))
    nv.append("- tiers L and C: not measured (later items).")
    nv.append("- BD-VMAF-NEG over a five-rung ladder (tiers M and L): "
              "from the next round, per the spec; column empty.")
    L += nv
    L.append("")
    os.makedirs(REPORT_DIR, exist_ok=True)
    path = os.path.join(REPORT_DIR, f"PARITY-REPORT-yah265-{date}.md")
    with open(path, "w") as f:
        f.write("\n".join(L))
    print(f"report: {path}")


def quality_rows(pts):
    out = []
    for k, p in pts.items():
        if p.get("anchor") != "def" or None in (
                p.get("ours_vmafneg"), p.get("ref_vmafneg"),
                p.get("ours_psnr_y"), p.get("ref_psnr_y")):
            continue
        out.append({"tier": p["tier"], "class": p["class"],
                    "title": p["title"],
                    "dv": p["ours_vmafneg"] - p["ref_vmafneg"],
                    "dp": p["ours_psnr_y"] - p["ref_psnr_y"],
                    "ds": p["dsize_pct"]})
    return out


LONG_MODES = (
    ("--plan", "dry run of the whole table: every command, the encode "
               "count and the estimated hours; nothing runs"),
    ("--windows", "untimed: G1/G2 window matches (tiers M, L, lash)"),
    ("--time-windows", "TIMED: G1/G2 on the windows"),
    ("--ladder", "untimed: CRF and VBR ladders (tiers M, L and C)"),
    ("--sweep", "untimed: the lookahead sweep (ours 12/20/30, x265 20)"),
    ("--credits", "untimed: the end-credits rows (tier L, 1080p)"),
    ("--vbr-wall", "TIMED: the VBR wall leg, G3 (tiers M and L)"),
    ("--selftest-long", "check parity_long.py's tables and verdict lines"),
)


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    m = ap.add_mutually_exclusive_group(required=True)
    m.add_argument("--solve", action="store_true")
    m.add_argument("--time", action="store_true")
    m.add_argument("--report", action="store_true")
    # yah264's shape (scripts/parity_long.py): tiers L and C, the windowed
    # G1/G2, the ladders, the VBR wall, the sweep, credits and the dry run
    for flag, what in LONG_MODES:
        m.add_argument(flag, action="store_true", help=what)
    ap.add_argument("--tier", default="", choices=("", "S", "M", "L", "C"),
                    help="S (the default for --solve/--time), M, L or C")
    ap.add_argument("--modes", default="crf,vbr",
                    help="--ladder/--sweep: crf, vbr or crf,vbr (both is crf,vbr)")
    ap.add_argument("--full", action="store_true",
                    help="--plan: print every feeder command whole")
    ap.add_argument("--goals", default="3,2,1")
    ap.add_argument("--classes", default="")
    ap.add_argument("--cells", default="")
    ap.add_argument("--plain-vmaf", action="store_true",
                    help="--solve: add plain VMAF v0.6.1 to the 5.2a "
                         "per-frame run (optional in the spec, for "
                         "publication passes; about 80%% more scoring)")
    ap.add_argument("--force", action="store_true",
                    help="--solve: re-solve cells already solved")
    ap.add_argument("--g1-classes", default="",
                    help="--time: run G1 on these classes only")
    ap.add_argument("--deadline", type=float, default=0,
                    help="--time: epoch seconds; a round that would end "
                         "after it is not started")
    ap.add_argument("--date", default=datetime.date.today().isoformat(),
                    help="--report: the report's date")
    ap.add_argument("--loadavg", action="store_true",
                    help="--time: judge quiet on the one-minute load "
                         "(LOADMAX) instead of total box CPU")
    args = ap.parse_args()
    global PLAIN_VMAF, LOADAVG
    PLAIN_VMAF = args.plain_vmaf
    LOADAVG = args.loadavg
    mode = next((f[2:].replace("-", "_") for f, _ in LONG_MODES
                 if getattr(args, f[2:].replace("-", "_"))), None)
    if mode or args.report or args.tier in ("L", "C"):
        import parity_long
        return parity_long.run(args, mode or ("report" if args.report else
                                              "solve" if args.solve else
                                              "time"))
    args.tier = args.tier or "S"
    if args.solve:
        do_solve(args)
    elif args.time:
        do_time(args)
    else:
        write_report(args.date)


if __name__ == "__main__":
    main()
