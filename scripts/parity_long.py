#!/usr/bin/env python3
# Copyright (c) 2026, the yah265 authors
# SPDX-License-Identifier: GPL-2.0-or-later
"""parity_long.py - the rest of yah264's parity shape, for yah265.

scripts/parity_spec.py measures tiers S and M at the matched point. This
module brings the harness to the shape yah264 declared parity on
(PARITY-REPORT-yah264-2026-10-12, its scripts/longform_throughput.py and
the ladder lane beside it), against x265 medium:

  tier L    20 cells: tears, meridian, stem2, sintel, bbb, cosmos and
            nocturne at 720p, 1080p and 2160p (cosmos has no 2160p). Each
            is the 600 s excerpt from the spec's offset, cut at the last
            story frame (end credits excluded). Read from the staged Y4M
            where one exists (~/media/staging-y4m, the T7 staging), else
            decoded from the master by lf_feed.py (VideoToolbox).
  tier C    lash1924 at 720p (1280x960) and "1080p" (native 1440x1080),
            600 s from 40 s; worldcup, worldcup_far, skater227, wikipedia,
            debugging, spreadsheet (1080p) and boxing (2160p, 4096 wide)
            from ~/media/tier_c_int. Quality counts; speed is shown and
            never gates.
  windows   G1 and G2 on tiers M and L (and lash) are timed on stratified
            30 s windows, one per 180 s, each coded as its own stream. Ours
            is re-matched onto x265's summed window bytes at each timed
            thread count (G1 one frame thread, G2 default); a goal whose
            summed bytes miss by more than 5% is void for that cell.
  ladders   tiers M, L and C. CRF rungs 20/23/26/29/32 on both sides;
            VBR rungs at x265's achieved rates there, maxrate 1.5K, buffer
            2K; both encoders at 6 threads (x265 --pools 6 --frame-threads
            4). BD-VMAF-NEG per curve, VBR tracking per rung against the
            +-10% bar. VMAF-NEG on every frame on tiers M and C, every 3rd
            on tier L.
  VBR wall  G3 only: one rung per M and L cell at x265's CRF 28 achieved
            rate, 1.5x / 2x, two interleaved rounds, no size match.
  sweep     ours at --rc-lookahead 12, 20 (medium's own) and 30 on six
            cells, CRF and VBR, against x265 at its default 20.
  credits   per title at 1080p, the credits span coded on its own: x265 at
            CRF 28 and ours at the cell's matched CRF, five rungs each side
            for the size at equal PSNR-Y. Supplementary, never a gate.
  report    PARITY-REPORT-yah265-<date>.md and .csv, section for section
            yah264's 10-12 page, with the 27-combination count.

Everything here runs through parity_spec.py's modes; see its --help. The
untimed modes (--solve, --windows, --ladder, --sweep, --credits) take a
box share of PL_THREADS (6) per encoder and VMAF_THREADS (4) for libvmaf.
The timed modes (--time, --time-windows, --vbr-wall) start only after
parity_spec's quiet check. --plan prints every command and the estimated
hours and runs nothing.

Clean-room: nothing here names another encoder's internals. yah264's code
is copied where it fits (CONTRIBUTING.md rule 1).
"""
import csv
import datetime
import fcntl
import hashlib
import json
import math
import os
import re
import shlex
import statistics
import subprocess
import sys
import tempfile
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "scripts"))
import bdcompare as bd                                    # noqa: E402
import bd_at_rate as bdr                                  # noqa: E402
import boxlib                                             # noqa: E402
import media_traits as mt                                 # noqa: E402


def _spec():
    # parity_spec.py runs as __main__ and dispatches here; importing it again
    # would make a second copy of its globals
    m = sys.modules.get("__main__")
    if m is not None and getattr(m, "__file__", "").endswith("parity_spec.py"):
        return m
    import parity_spec
    return parity_spec


ps = _spec()

PY = sys.executable
FEED = os.environ.get("LF_FEED", os.path.join(ROOT, "scripts", "lf_feed.py"))
MEDIA = os.environ.get("LF_MEDIA", "<media>")
MASTERS = os.environ.get("LF_MASTERS", os.path.expanduser(
    "~/src/yah264-board4k/scratch/masters"))
STAGED = [os.environ.get("LF_STAGE_INTERNAL",
                         os.path.expanduser("~/media/staging-y4m")),
          os.environ.get("LF_STAGE_Y4M", "<staging>/yah264/y4m")]
TIER_C_DIR = os.environ.get("LF_TIER_C",
                            os.path.expanduser("~/media/tier_c_int"))

# the untimed share, the spec's ladder thread count
THREADS = int(os.environ.get("PL_THREADS", "6"))
VMAF_THREADS = os.environ.get("VMAF_THREADS", "4")
FF_THREADS = os.environ.get("FF_THREADS", "2")
SSIM_PASS = os.environ.get("PL_SSIM", "1") == "1"
PLAIN_VMAF = os.environ.get("PL_PLAIN_VMAF", "0") == "1"
# VMAF v1 rides beside VMAF-NEG v0.6.1 in the same pass (owner, 10-22: the
# bar moves to v1 once the shared spec lands; scoring both now saves the
# re-encode). It needs a float-enabled libvmaf; that build scores v0.6.1neg
# frame for frame as the stock one does. PL_V1=0 scores v0 alone.
V1_BIN = os.path.expanduser(os.environ.get(
    "PL_V1_BIN", "~/.local/opt/vmaf-v1/bin/vmaf"))
V1_DIR = os.path.expanduser(os.environ.get(
    "PL_V1_DIR", "~/.local/share/vmaf/model/vmaf_v1.0.16"))
V1_ON = os.environ.get("PL_V1", "1") == "1" and os.path.exists(V1_BIN)
# v1 alone decides the bar since the owner's word of 10-23; PL_V0=1 also
# scores VMAF-NEG v0.6.1, for comparison with rows measured before it
V0_ON = os.environ.get("PL_V0", "0") == "1" or not V1_ON
GATE = "v1" if V1_ON else "neg"
MNAME = "VMAF v1" if GATE == "v1" else "VMAF-NEG"
# both sides are 8-bit encodes scored at 10 bits; CAMBI must be told the
# encode's depth or it reads the up-converted picture as a 10-bit encode
V1_ENC_BITS = 8
# v1 is scored at display resolution (owner rule 10-25): an encode smaller
# than 1920x1080 in both dimensions is scaled up, bicubic and aspect kept,
# to fit 1920x1080, both sides, with CAMBI told the coded size
V1_DISP = (1920, 1080)


def v1_upscaled(w, h):
    return w < V1_DISP[0] and h < V1_DISP[1]


def v1_disp_tag(w, h):
    return "fit1080" if v1_upscaled(w, h) else "native"
# the draft spec (yah264, 10-22, owner review pending): the gate model is
# 1080p 3H, 4K 1.5H at 2160p, the _hfr twin at 50 fps and up; 4K 3H and
# phone 5H ride along as information. Scored at 10 bits, both sides
# converted up, as Netflix advises for SDR; v0 stays at 8 bits in its own
# pass so every v0 figure keeps its meaning.
V1_GATE = {"2160p": "1d5h_2160"}
V1_INFO = ("3d0h_2160", "5d0h")


def v1_models(cell):
    gate = V1_GATE.get(cell["cls"], "3d0h")
    hfr = fps_of(info(cell)) >= 49.9
    out = []
    for m in (gate,) + tuple(x for x in V1_INFO if x != gate):
        path = (f"{V1_DIR}_hfr/vmaf_v1.0.16_hfr_{m}.json" if hfr
                else f"{V1_DIR}/vmaf_v1.0.16_{m}.json")
        out.append((m, path))
    return out, gate, hfr

REF_CRF = ps.REF_CRF                       # 28, x265 --help
RUNGS = (20, 23, 26, 29, 32)               # spec ladder recipe, both sides
# PL_RUNGS picks a subset for a first-pass screen (e.g. "20,26,32"). The
# rows are the same records the full ladder reuses, and a curve read with
# fewer than five rungs is printed as a screen, never as the spec's BD.
LAD_RUNGS = (tuple(int(q) for q in os.environ["PL_RUNGS"].split(","))
             if os.environ.get("PL_RUNGS") else RUNGS)
MAXR, BUFR = 1.5, 2.0
OUR_DEPTH, REF_DEPTH = 20, 20              # medium's own lookahead on each side
SWEEP_DEPTHS = (12, 20, 30)
SWEEP_SPREAD = 0.4                         # yah264's threaded-VBR run-to-run spread
SWEEP_CELLS = [x for x in os.environ.get(
    "PL_SWEEP", "M/2160p/chimera,M/1080p/sollevante,M/2160p/meridian,"
    "L/1080p/tears,L/1080p/bbb,L/2160p/sintel").split(",") if x]
TRACK_BAR = 10.0
BD_BAR = 1.0
WIN_SECS, STRATUM_SECS = 30.0, 180.0
WIN_TOL, WIN_VOID = 0.5, 5.0               # percent: aim, and the void line
G3_ROUNDS = int(os.environ.get("PL_G3_ROUNDS", "2"))
WIN_ROUNDS = int(os.environ.get("PL_WIN_ROUNDS", "1"))
VBR_ROUNDS = int(os.environ.get("PL_VBR_ROUNDS", "2"))
FEEDCHECK_SECS = 60.0
# x265 medium codes 4 B frames (period 5), ours 7 (period 8): a score stride
# has to be coprime with both or it samples the same GOP slots every time
GOP_PERIODS = (5, 8)
STRIDE = {"S": 1, "M": 1, "L": 3, "C": 1}

WIN_PATH = os.path.join(ROOT, "docs/data/parity-spec-windows.json")
LAD_PATH = os.path.join(ROOT, "docs/data/parity-spec-ladders.json")
VBRW_PATH = os.path.join(ROOT, "docs/data/parity-spec-vbrwall.csv")
MEMO = os.path.join(ps.WORK, "long-memo.json")
VBRW_COLS = ["date", "time", "commit", "tier", "class", "title", "size", "k",
             "maxrate", "bufsize", "rounds", "ours_wall_s", "ref_wall_s",
             "ratio", "ours_kbps", "ref_kbps", "ours_err_pct", "ref_err_pct",
             "feed_fps", "feeder_bound", "load1", "ours_md5", "ref_md5",
             "ours_bin", "lookahead", "note"]

# ------------------------------------------------------------------ cells

# The long-form titles: yah264's FULL table (frames counted there), the
# spec's offsets, and the last story frame (excerpt-relative, every size).
TITLES = {
    "tears":    dict(file="longform/tearsofsteel_4k.mov", native=(3840, 1714),
                     fps=(24, 1), t0=0.0, frames=17616, offset=60,
                     last=12671, kind="live action + VFX"),
    "meridian": dict(file="Meridian_UHD4k5994_HDR_P3PQ.mp4",
                     native=(3840, 2160), fps=(60000, 1001), t0=0.0,
                     frames=43093, offset=30, last=32886,
                     kind="live action, 59.94p"),
    "stem2":    dict(file="derived/stem2_uhd_hevc_q90.mp4",
                     native=(3840, 2160), fps=(24, 1), t0=0.0, frames=25114,
                     offset=235, last=12939, kind="live action"),
    "sintel":   dict(file="Sintel.2010.4k.mkv", native=(4096, 1744),
                     fps=(24, 1), t0=0.0, frames=21312, offset=195,
                     last=13177, kind="animation"),
    "bbb":      dict(file="bbb_sunflower_2160p_30fps_normal.mp4",
                     native=(3840, 2160), fps=(30, 1), t0=0.066667,
                     frames=19036, offset=30, last=13928,
                     kind="animation, 30p"),
    "cosmos":   dict(file="cosmos_laundromat_1080p.mp4", native=(1920, 804),
                     fps=(24, 1), t0=0.0, frames=17532, offset=30,
                     last=13723, kind="animation",
                     sizes=("720p", "1080p")),
    "nocturne": dict(file="longform/nocturne_filmgrain/"
                          "nocturne_60fps_uhd_filmgrain_sdr.mov",
                     native=(3840, 2160), fps=(60, 1), t0=0.0, frames=39870,
                     offset=50, last=34319,
                     kind="live action, 60p, grain added in post"),
    # tier C grain: a 1924 film scan, 1440x1080, so 1280x960 and native
    "lash1924": dict(file="longform/lash_of_the_whip_1924/02049825.mov",
                     native=(1440, 1080), fps=(24, 1), t0=0.0, frames=65729,
                     offset=40, last=None, kind="film scan grain, 1924",
                     sizes=("720p", "1080p")),
}
L_TITLES = ("tears", "meridian", "stem2", "sintel", "bbb", "cosmos",
            "nocturne")
SIZES = ("720p", "1080p", "2160p")
SIZE_W = {"720p": 1280, "1080p": 1920}
EXCERPT_SECS = 600

# tier C clips: (class, file, content, 10-bit source)
C_CLIPS = {
    "worldcup":     ("1080p", "WorldCup_1920x1080_30p.y4m", "sports", False),
    "worldcup_far": ("1080p", "WorldCup_far_1920x1080_30p.y4m", "sports",
                     False),
    "skater227":    ("1080p", "Skater227_1920x1080_30fps_8bit.y4m", "sports",
                     True),
    "boxing":       ("2160p", "BoxingPractice_4096x2160_60fps_8bit.y4m",
                     "sports", True),
    "wikipedia":    ("1080p", "Wikipedia_1920x1080p30.y4m", "screen", False),
    "debugging":    ("1080p", "Debugging_1920x1080_30fps_8bit_420.y4m",
                     "screen", False),
    "spreadsheet":  ("1080p", "Spreadsheet_1920x1080_30fps_8bit_420_130f.y4m",
                     "screen", False),
}

# yah264's 2026-09-26 scene scan (detail/motion, share of the window)
CONTENT_CLASS = {"tears": "detailed/moving 40%", "meridian": "mid/static 98%",
                 "sintel": "mid/moving 42% (whole title)",
                 "bbb": "detailed/moving 41%", "cosmos": "mid/moving 68%",
                 "stem2": "mid/moving 33% (whole title)",
                 "nocturne": "mid/moving 59%",
                 "lash1924": "detailed/moving 59%"}

# PL_SMOKE=1: a 30-frame CIF cell only, every thread count 1, its own
# stores under scratch, for checking the machinery on a busy box
SMOKE = os.environ.get("PL_SMOKE") == "1"
if SMOKE:
    THREADS = 1
    VMAF_THREADS = FF_THREADS = "1"
    _sd = os.path.join(ps.WORK, "smoke")
    WIN_PATH = os.path.join(_sd, "windows.json")
    LAD_PATH = os.path.join(_sd, "ladders.json")
    VBRW_PATH = os.path.join(_sd, "vbrwall.csv")
    MEMO = os.path.join(_sd, "memo.json")
    POINTS = os.path.join(_sd, "points.json")
    WIN_SECS, STRATUM_SECS = 0.25, 0.5
else:
    POINTS = ps.POINTS


def even(x):
    return int(x / 2 + 0.5) * 2


def full_geom(title, size):
    nw, nh = TITLES[title]["native"]
    if size == "2160p" or SIZE_W[size] >= nw:
        return nw, nh
    w = SIZE_W[size]
    return w, even(w * nh / nw)


def cname(title, cls):
    """foreman_cif, tears_720p; a tier S title that names its size already
    (bbb_2160p) keeps it once."""
    sz = cls.lower() if cls == "CIF" else cls
    return title if title.endswith("_" + sz) else f"{title}_{sz}"


def _mk(tier, cls, title, size, **kw):
    c = dict(tier=tier, cls=cls, title=title, size=size,
             key=f"{tier}/{cls}/{title}", name=cname(title, cls))
    c.update(kw)
    return c


def all_cells():
    """Every cell this module knows, in spec order; nothing on disk read."""
    out = []
    for c in ps.CELLS:
        if c[0] == "M":
            out.append(_mk("M", c[1], c[2], c[1], kind="y4m",
                           file=c[3], spec_cell=c))
    for t in L_TITLES:
        for s in TITLES[t].get("sizes", SIZES):
            out.append(_mk("L", s, t, s, kind="excerpt"))
    for s in TITLES["lash1924"]["sizes"]:
        out.append(_mk("C", s, "lash1924", s, kind="excerpt",
                       grain=True))
    for t, (cls, fn, what, ten) in C_CLIPS.items():
        out.append(_mk("C", cls, t, cls, kind="clip", file=fn,
                       content=what, ten_bit=ten))
    if SMOKE:
        out = [_mk("C", "CIF", "smoke", "CIF", kind="clip",
                   file="foreman_cif.y4m", content="smoke", ten_bit=False,
                   dir=ps.T7_TIER_S, limit=30, smoke=True)]
    return out


def y4m_shape(path):
    with open(path, "rb") as f:
        hdr = f.readline()
    tags = hdr.decode("ascii", "replace").split()
    w = int(next(t[1:] for t in tags if t[0] == "W"))
    h = int(next(t[1:] for t in tags if t[0] == "H"))
    n, d = (int(x) for x in next(t[1:] for t in tags if t[0] == "F").split(":"))
    if any(t[0] == "C" and not t.startswith("C420") for t in tags[1:]):
        sys.exit(f"parity_long: {path}: only 8-bit 4:2:0 is an input")
    per = w * h * 3 // 2 + 6
    return w, h, (n, d), (os.path.getsize(path) - len(hdr)) // per


def excerpt_span(title):
    """(start frame in the master, frames in the cell). The cell stops at
    the last story frame where the excerpt runs into credits."""
    f = TITLES[title]
    num, den = f["fps"]
    n = int(round(EXCERPT_SECS * num / den))
    if f["last"] is not None:
        n = min(n, f["last"] + 1)
    return int(round(f["offset"] * num / den)), n


def master_path(title):
    f = TITLES[title]
    usb = os.path.join(MEDIA, f["file"])
    local = os.path.join(MASTERS, os.path.basename(f["file"]))
    try:
        if os.path.getsize(local) == os.path.getsize(usb):
            return local
    except OSError:
        pass
    return usb


def staged_path(title, size):
    """The staged excerpt Y4M (the whole 600 s, frame 0 = the offset) whose
    geometry is the cell's and which holds the cell's frames, or None."""
    w, h = full_geom(title, size)
    need = excerpt_span(title)[1]
    for d in STAGED:
        p = os.path.join(d, f"{title}-x10_{size}.y4m")
        try:
            pw, ph, _, n = y4m_shape(p)
        except (OSError, StopIteration, ValueError):
            continue
        if (pw, ph) == (w, h) and n >= need:
            return p
    return None


_INFO = {}


def info(cell):
    """w, h, fps (num, den), frames, source kind ("y4m" | "vt"), path,
    start (the cell's frame 0 in that source)."""
    k = cell["key"]
    if k in _INFO:
        return _INFO[k]
    if cell["kind"] == "excerpt":
        t = cell["title"]
        w, h = full_geom(t, cell["size"])
        st, n = excerpt_span(t)
        sp = staged_path(t, cell["size"])
        d = dict(w=w, h=h, fps=TITLES[t]["fps"], frames=n,
                 src="y4m" if sp else "vt",
                 path=sp or master_path(t), start=0 if sp else st,
                 master_start=st)
    else:
        if cell["kind"] == "y4m":
            p = ps.src_of(cell["spec_cell"])
        else:
            p = os.path.join(cell.get("dir", TIER_C_DIR), cell["file"])
        w, h, fps, n = y4m_shape(p)
        # a window of a longer file, read in place (tier D's D3v)
        off = cell.get("offset", 0)
        n -= off
        if cell.get("limit"):
            n = min(n, cell["limit"])
        d = dict(w=w, h=h, fps=fps, frames=n, src="y4m", path=p, start=off,
                 master_start=off)
    _INFO[k] = d
    return d


def fps_of(i):
    return i["fps"][0] / i["fps"][1]


def credits_span(title):
    """(start, end) excerpt-relative from the census, or None."""
    return mt.credits(f"lf/{title}_1080p") if title in TITLES else None


def span_note(cell):
    i = info(cell)
    n = i["frames"]
    if cell["kind"] == "excerpt":
        last = TITLES[cell["title"]]["last"]
        return (f"frames 0-{n - 1} of the excerpt (master frame "
                f"{i['master_start']} on)"
                + (", end credits excluded" if last is not None else ""))
    return f"frames 0-{n - 1}"


def windows(frames, fps):
    """yah264's stratified sample: one WIN_SECS window per STRATUM_SECS,
    centered in its stratum, a pure function of the frame count and rate."""
    n = max(1, int(frames / fps / STRATUM_SECS + 0.5))
    wlen = min(frames, int(round(WIN_SECS * fps)))
    st = frames / n
    return [(int(k * st + (st - wlen) / 2), wlen) for k in range(n)]


def cell_windows(cell):
    i = info(cell)
    return windows(i["frames"], fps_of(i))


def windowed(cell):
    """G1/G2 on windows (tiers M and L, and the 10-minute lash cells)."""
    return (cell["tier"] in ("M", "L") or cell["kind"] == "excerpt"
            or bool(cell.get("smoke")))


def select(args, tiers):
    want_t = [t for t in (args.tier,) if t] or list(tiers)
    cs = [c for c in all_cells() if c["tier"] in want_t or SMOKE]
    if getattr(args, "classes", ""):
        cs = [c for c in cs if c["cls"] in args.classes.split(",")]
    if getattr(args, "cells", ""):
        want = args.cells.split(",")
        cs = [c for c in cs if c["title"] in want or c["name"] in want
              or c["key"] in want]
    return cs

# ------------------------------------------------------------------ feed


def feeder(cell, s=0, n=None, workers=1):
    """argv of the process writing frames s..s+n of the cell as Y4M."""
    i = info(cell)
    n = n or i["frames"] - s
    if i["src"] == "y4m":
        return [PY, FEED, "y4m", "--src", i["path"],
                "--start", str(i["start"] + s), "--count", str(n)]
    f = TITLES[cell["title"]]
    nw, nh = f["native"]
    return [PY, FEED, "vt", "--src", i["path"], "--native", f"{nw}x{nh}",
            "--size", f"{i['w']}x{i['h']}", "--fps", "%d/%d" % i["fps"],
            "--t0", f"{f['t0']:.6f}", "--start", str(i["start"] + s),
            "--count", str(n), "--workers", str(workers), "--threads", "1"]


def timed_workers(cell):
    """yah264's rule: below native size one VideoToolbox stream is slower
    than both encoders, so a decoder-fed cell there takes three."""
    i = info(cell)
    if i["src"] != "vt":
        return 1
    return 1 if (i["w"], i["h"]) == TITLES[cell["title"]]["native"] else 3


def feed_sig(argv):
    """The feeder as a memo sees it: masters and staged files by name."""
    return " ".join(os.path.basename(a) if a.startswith("/") else a
                    for a in argv[1:])

# ----------------------------------------------------------------- encode


def rate_args(mode, q):
    if mode == "crf":
        return ["--crf", ps._fmt_q(q)]
    k = int(q)
    return ["--bitrate", str(k), "--vbv-maxrate", str(int(round(MAXR * k))),
            "--vbv-bufsize", str(int(round(BUFR * k)))]


# x265's threading per use. Its pictures move with --frame-threads and not
# with the pool's size or --no-asm under CRF (parity_spec.py), so the
# untimed `solve` anchor runs at the box share with the timed leg's
# frame-thread count. Having no pool at all does move them at 720p and up
# (1-2.5% fewer bytes on most titles, 11-16% on meridian, L windows
# 10-22), so G1's anchor runs poolless exactly as its timed leg does.
# Under VBV the pool size moves the bytes (vbr_lf.py), so a VBR ladder is
# at --pools 6 on both untimed and every reading of it.
REF_THREADS = {"solve": ["--frame-threads", "4"],
               "solve_ft1": ["--pools", "none", "--frame-threads", "1"],
               "t1": ["--pools", "none", "--frame-threads", "1"],
               "def": []}


def ref_cmd(out, mode, q, prof, purec=False):
    thr = list(REF_THREADS[prof])
    if prof == "solve":
        thr = ["--pools", str(THREADS)] + thr
    return ([ps.X265, "--input", "-", "--y4m", "--preset", "medium"]
            + rate_args(mode, q) + thr + (["--no-asm"] if purec else [])
            + ["--log-level", "error", "-o", out])


def our_cmd(out, mode, q, threads, depth=None):
    c = [ps.N265, "-"] + rate_args(mode, q)
    if threads is not None:
        c += ["--threads", str(threads)]
    if depth and depth != OUR_DEPTH:
        c += ["--rc-lookahead", str(depth)]
    return c + ["-o", out]


def arm_env(arm, purec):
    e = dict(os.environ)
    e.pop("N265_ASM_OFF", None)
    if arm == "ours" and purec:
        e["N265_ASM_OFF"] = "all"
    return e


def untimed_cmd(arm, mode, q, out, prof="solve", depth=None):
    if arm == "ours":
        return our_cmd(out, mode, q, THREADS, depth)
    return ref_cmd(out, mode, q, prof)


def timed_cmd(arm, goal, mode, q, out):
    pure_c, t1 = goal in (1, 2), goal == 1
    if arm == "ours":
        return our_cmd(out, mode, q, 1 if t1 else None), arm_env(arm, pure_c)
    return (ref_cmd(out, mode, q, "t1" if t1 else "def", pure_c),
            arm_env(arm, pure_c))


def pipe_run(feed, enc, env, timed=False):
    """feeder | encoder. Timed, the encoder runs under boxlib.measure (its
    wall, CPU, instructions retired and peak RSS). A feeder failure is
    fatal even when the encoder succeeded: an encoder fed a short stream
    reports a flattering wall."""
    ferr = tempfile.TemporaryFile()
    eerr = tempfile.TemporaryFile()
    t0 = time.monotonic()
    fp = subprocess.Popen(feed, stdout=subprocess.PIPE, stderr=ferr)
    if timed:
        cost = boxlib.measure(enc, env=env, stdin=fp.stdout, stderr=eerr)
        rc = cost["rc"]
        fp.stdout.close()
    else:
        ep = subprocess.Popen(enc, stdin=fp.stdout, stdout=subprocess.DEVNULL,
                              stderr=eerr, env=env)
        fp.stdout.close()
        rc = ep.wait()
    frc = fp.wait()
    wall = time.monotonic() - t0
    eerr.seek(0)
    etxt = eerr.read().decode(errors="replace")
    if rc != 0:
        ferr.seek(0)
        sys.exit(f"parity_long: encode failed ({rc}): "
                 f"{shlex.join(enc[:6])} ...\n  {etxt[-600:]}\n  feeder: "
                 f"{ferr.read().decode(errors='replace')[-300:]}")
    if frc != 0:
        ferr.seek(0)
        sys.exit(f"parity_long: the feeder failed ({frc}); the encode saw a "
                 f"short stream and is discarded:\n  "
                 f"{ferr.read().decode(errors='replace')[-600:]}")
    out = {"pipe_wall": wall}
    if timed:
        out.update(cost)
    return out


def file_md5(path):
    h = hashlib.md5()
    with open(path, "rb") as f:
        for b in iter(lambda: f.read(8 << 20), b""):
            h.update(b)
    return h.hexdigest()

# ------------------------------------------------------------------ score


def check_stride(stride):
    if any(math.gcd(stride, p) != 1 for p in GOP_PERIODS):
        sys.exit(f"parity_long: score stride {stride} shares a factor with a "
                 f"mini-GOP period {GOP_PERIODS}; use 1 or a stride coprime "
                 "with both")


def _fifo(wd, name):
    p = os.path.join(wd, name)
    if os.path.exists(p):
        os.remove(p)
    os.mkfifo(p)
    return p


def w611(y, u, v):
    return (6 * y + u + v) / 8


def ssim_611(feed, bitstream, wd):
    """(SSIM 6:1:1, frames the filter saw) against the same feeder; the
    filter runs shortest=1 so a short decode is never padded."""
    stats = os.path.join(wd, "ssim.log")
    fp = subprocess.Popen(feed, stdout=subprocess.PIPE,
                          stderr=subprocess.DEVNULL)
    r = subprocess.run(
        ["ffmpeg", "-v", "error", "-nostdin", "-threads", FF_THREADS,
         "-filter_threads", "1", "-i", bitstream, "-f", "yuv4mpegpipe",
         "-i", "pipe:0", "-lavfi",
         f"[0:v]format=yuv420p[d];[d][1:v]ssim=stats_file={stats}:shortest=1",
         "-f", "null", "-"], stdin=fp.stdout, stdout=subprocess.DEVNULL,
        stderr=subprocess.PIPE)
    fp.stdout.close()
    fp.wait()
    if r.returncode != 0:
        return None, 0
    vals = []
    with open(stats) as f:
        for ln in f:
            m = re.search(r"Y:([\d.]+) U:([\d.]+) V:([\d.]+)", ln)
            if m:
                vals.append(w611(*(float(x) for x in m.groups())))
    os.remove(stats)
    return (sum(vals) / len(vals) if vals else None), len(vals)


def has_gate(r, cell=None):
    """A row scored the current way. A row from before the display rule
    still counts where that rule doesn't scale (1080p and up)."""
    if GATE == "v1":
        if r.get("v1_cambi_enc_bits") != V1_ENC_BITS:
            return False
        if "v1_disp" in r:
            return True
        if cell is None:
            return False
        if "file" not in cell and cell.get("kind") != "y4m":
            # the report's spec cells carry no source; the class says it
            return cell.get("cls") in ("1080p", "2160p")
        return not v1_upscaled(info(cell)["w"], info(cell)["h"])
    return r.get("neg") is not None


def gate_score(r):
    """The gate metric of a scored row: v1's gate model, or VMAF-NEG."""
    return r[f"v1_{r['v1_gate']}"] if GATE == "v1" else r["neg"]


def score_v1(cell, bitstream, wd, s, n, stride):
    """VMAF v1 in a pass of its own: both sides converted to 10 bits
    (yuv420p10le), the gate model and the informational ones together."""
    models, gate, hfr = v1_models(cell)
    feed = feeder(cell, s, n)
    dfifo = _fifo(wd, "dec10.fifo")
    j = os.path.join(wd, "v1.json")
    i = info(cell)
    w, h = i["w"], i["h"]
    up = (["-vf", f"scale={V1_DISP[0]}:{V1_DISP[1]}:flags=bicubic"
           ":force_original_aspect_ratio=decrease:force_divisible_by=2"]
          if v1_upscaled(w, h) else [])
    up += ["-pix_fmt", "yuv420p10le", "-strict", "-1", "-f", "yuv4mpegpipe"]
    dec = subprocess.Popen(
        ["ffmpeg", "-v", "error", "-y", "-threads", FF_THREADS, "-i",
         bitstream] + up + [dfifo],
        stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
    fp = subprocess.Popen(feed, stdout=subprocess.PIPE,
                          stderr=subprocess.DEVNULL)
    cv = subprocess.Popen(["ffmpeg", "-v", "error", "-f", "yuv4mpegpipe",
                           "-i", "-"] + up + ["-"],
                          stdin=fp.stdout, stdout=subprocess.PIPE,
                          stderr=subprocess.DEVNULL)
    fp.stdout.close()
    margs = []
    for m, path in models:
        margs += ["--model", f"path={path}:name=v1_{m}"
                  f":cambi.enc_bitdepth={V1_ENC_BITS}"
                  f":cambi.enc_width={w}:cambi.enc_height={h}"]
    v = subprocess.run(
        [V1_BIN, "--threads", VMAF_THREADS, "--subsample", str(stride),
         "-r", "/dev/stdin", "-d", dfifo, "--json", "-o", j, "-q"] + margs,
        stdin=cv.stdout, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
    cv.stdout.close()
    cv.wait()
    fp.wait()
    if v.returncode != 0 and dec.poll() is None:
        dec.kill()
    dec.wait()
    os.remove(dfifo)
    if v.returncode != 0:
        sys.exit(f"parity_long: vmaf v1 failed on {bitstream}\n  "
                 f"{v.stderr.decode(errors='replace')[-600:]}")
    with open(j) as f:
        jj = json.load(f)
    os.remove(j)
    pooled = jj["pooled_metrics"]
    out = {"v1_gate": gate, "v1_hfr": hfr, "v1_bits": 10,
           "v1_cambi_enc_bits": V1_ENC_BITS, "v1_disp": v1_disp_tag(w, h),
           "v1_frames": len(jj["frames"])}
    for m, _ in models:
        pm = pooled[f"v1_{m}"]
        out[f"v1_{m}"] = pm["mean"]
        out[f"v1_{m}_hmean"] = pm.get("harmonic_mean")
        out[f"v1_{m}_min"] = pm.get("min")
    return out


def score(cell, bitstream, wd, s=0, n=None, stride=1, psnr_only=False):
    """VMAF-NEG, PSNR-Y and the 5.2a columns of a bitstream against the
    frames it was made from: the same feeder into libvmaf's stdin, the
    bitstream decoded by ffmpeg into a fifo. Nothing lands on disk. A short
    count is recorded (frames_ok false), not fatal. psnr_only runs libvmaf
    with no model (the credits rows)."""
    check_stride(stride)
    i = info(cell)
    n = n or i["frames"] - s
    feed = feeder(cell, s, n)
    dfifo = _fifo(wd, "dec.fifo")
    j = os.path.join(wd, "v.json")
    dec = subprocess.Popen(
        ["ffmpeg", "-v", "error", "-y", "-threads", FF_THREADS, "-i",
         bitstream, "-pix_fmt", "yuv420p", "-f", "yuv4mpegpipe", dfifo],
        stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
    fp = subprocess.Popen(feed, stdout=subprocess.PIPE,
                          stderr=subprocess.DEVNULL)
    if psnr_only or not V0_ON:
        models = ["--no_prediction"]
    else:
        models = ["--model", "version=vmaf_v0.6.1neg:name=neg"]
        if PLAIN_VMAF or getattr(ps, "PLAIN_VMAF", False):
            models += ["--model", "version=vmaf_v0.6.1:name=vmaf"]
    v = subprocess.run(
        ["vmaf", "--threads", VMAF_THREADS, "--subsample", str(stride),
         "-r", "/dev/stdin", "-d", dfifo, "--json", "-o", j,
         "--feature", "psnr", "-q"] + models,
        stdin=fp.stdout, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
    fp.stdout.close()
    fp.wait()
    if v.returncode != 0 and dec.poll() is None:
        dec.kill()
    dec.wait()
    os.remove(dfifo)
    if v.returncode != 0:
        sys.exit(f"parity_long: vmaf failed on {bitstream}\n  "
                 f"{v.stderr.decode(errors='replace')[-600:]}")
    with open(j) as f:
        jj = json.load(f)
    os.remove(j)
    want = (n + stride - 1) // stride
    pooled, frames = jj["pooled_metrics"], jj["frames"]
    out = {"psnr_y": pooled["psnr_y"]["mean"], "stride": stride,
           "frames_scored": len(frames), "frames_want": want}
    if "psnr_cb" in pooled:
        out["psnr_yuv611"] = w611(pooled["psnr_y"]["mean"],
                                  pooled["psnr_cb"]["mean"],
                                  pooled["psnr_cr"]["mean"])
    ok = len(frames) == want
    if not psnr_only and V0_ON:
        neg = [fr["metrics"]["neg"] for fr in frames]
        low = sorted(neg)[:max(1, math.ceil(0.01 * len(neg)))]
        first = [fr["metrics"]["neg"] for fr in frames
                 if fr["frameNum"] < fps_of(i)]
        out.update(neg=pooled["neg"]["mean"],
                   neg_hmean=pooled["neg"].get("harmonic_mean"),
                   neg_min=pooled["neg"].get("min"),
                   neg_low1=sum(low) / len(low) if low else None,
                   neg_first1s=sum(first) / len(first) if first else None,
                   vmaf=pooled["vmaf"]["mean"] if "vmaf" in pooled else None)
    if not psnr_only:
        if V1_ON:
            out.update(score_v1(cell, bitstream, wd, s, n, stride))
        if SSIM_PASS:
            out["ssim_yuv611"], out["frames_decoded"] = ssim_611(
                feed, bitstream, wd)
            ok = ok and out["frames_decoded"] == n
    out["frames_ok"] = ok
    if not ok:
        print(f"  [FRAME-MISMATCH] {cell['name']}: scored {len(frames)} "
              f"(want {want}), decoded {out.get('frames_decoded', '?')} "
              f"(want {n})", flush=True)
    return out

# ------------------------------------------------------------------ store


def jload(path):
    try:
        with open(path) as f:
            return json.load(f)
    except FileNotFoundError:
        return {}
    except ValueError as e:
        sys.exit(f"parity_long: {path} does not parse ({e}); refusing to "
                 "write over it")


def jput(path, key, val):
    """Read, add, write under a lock on a sibling file, renamed into place,
    so two processes recording at once never lose a record."""
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path + ".lock", "a") as lk:
        fcntl.flock(lk, fcntl.LOCK_EX)
        d = jload(path)
        d[key] = val
        fd, tmp = tempfile.mkstemp(prefix=".tmp.", dir=os.path.dirname(path))
        with os.fdopen(fd, "w") as f:
            json.dump(d, f, indent=1, sort_keys=True)
            f.write("\n")
        os.replace(tmp, path)
    return val


_BH = {}


def bin_hash(exe):
    if exe not in _BH:
        _BH[exe] = bdr._bin_hash(exe)
    return _BH[exe]


def now():
    return datetime.datetime.now().isoformat(timespec="seconds")


def measure(cell, arm, mode, q, s=0, n=None, prof="solve", depth=None,
            stride=None, psnr_only=False, log=print):
    """One untimed encode of frames s..s+n, memoized on the feeder, the
    command and the binary: bytes, md5, kbps, and the scores when `stride`
    is given. The bitstream is deleted; only its numbers are kept."""
    i = info(cell)
    n = n or i["frames"] - s
    cmd = untimed_cmd(arm, mode, q, "OUT", prof, depth)
    exe = ps.N265 if arm == "ours" else ps.X265
    knobs = " ".join(sorted(f"{k}={v}" for k, v in os.environ.items()
                            if k.startswith("N265_") and k != "N265_ASM_OFF"))
    key = "|".join((feed_sig(feeder(cell, s, n)), shlex.join(cmd[:-1]),
                    bin_hash(exe), knobs))
    m = jload(MEMO).get(key)
    want_score = stride is not None
    if m and (not want_score or (m.get("stride") == stride and
                                 (psnr_only or has_gate(m, cell)))):
        return m
    wd = tempfile.mkdtemp(prefix="pl.", dir=ensure_work())
    try:
        out = os.path.join(wd, "o.hevc")
        cmd[-1] = out
        t0 = time.monotonic()
        pipe_run(feeder(cell, s, n), cmd, arm_env(arm, False))
        el = time.monotonic() - t0
        size = os.path.getsize(out) if os.path.exists(out) else 0
        if size < 64:
            sys.exit(f"parity_long: {arm} wrote no bitstream for "
                     f"{cell['name']} {mode} {q}")
        rec = {"bytes": size, "md5": file_md5(out), "frames": n,
               "kbps": size * 8 / (n / fps_of(i)) / 1000,
               "enc_s": round(el, 1), "date": now(), "arm": arm,
               "mode": mode, "q": q, "threads": THREADS, "prof": prof,
               "depth": depth or (OUR_DEPTH if arm == "ours" else REF_DEPTH)}
        if want_score:
            rec.update(score(cell, out, wd, s, n, stride, psnr_only))
    finally:
        subprocess.run(["rm", "-rf", wd])
    jput(MEMO, key, rec)
    log(f"      {arm:<4} {mode} {ps._fmt_q(q):>7} -> {rec['bytes']:>11} B "
        f"{rec['kbps']:9.1f} kbit/s"
        + (f"  NEG {rec['neg']:.2f}" if "neg" in rec else "")
        + (f"  v1 {gate_score(rec):.2f}" if "v1_gate" in rec else "")
        + (f"  PSNR-Y {rec['psnr_y']:.2f}" if "psnr_y" in rec else "")
        + f"  [{rec['enc_s']:.0f} s]")
    return rec


def ensure_work():
    d = os.path.join(ps.WORK, "long")
    os.makedirs(d, exist_ok=True)
    return d


def secant(size_of, target, crf, slope=-0.13, steps=8, aim=0.005):
    """yah264's rate-factor solve: a secant in (crf, ln bytes), bisecting
    once the target is bracketed, stopping inside one tread of the integer
    QP staircase. Returns (best (crf, bytes, err), points, slope)."""
    pts, best = [], None
    for _ in range(steps):
        crf = round(max(0.0, min(51.0, crf)), 2)
        if any(abs(crf - c) < 1e-6 for c, _ in pts):
            break
        sz = size_of(crf)
        err = abs(sz - target) / target
        if best is None or err < best[2]:
            best = (crf, sz, err)
        pts.append((crf, sz))
        if err < aim:
            break
        hi = [c for c, b in pts if b > target]
        lo = [c for c, b in pts if b < target]
        if hi and lo and min(lo) - max(hi) < 0.015:
            break
        if len(pts) >= 2:
            (ca, ba), (cb, bb) = pts[-2], pts[-1]
            if ba > 0 and bb > 0 and abs(cb - ca) > 1e-6:
                m = math.log(bb / ba) / (cb - ca)
                if m < -0.02:
                    slope = min(-0.05, max(-0.30, m))
        crf = crf + math.log(target / sz) / slope
        if hi and lo:
            h, l_ = max(hi), min(lo)
            if not (h < crf < l_) or any(abs(round(crf, 2) - c) < 1e-6
                                         for c, _ in pts):
                crf = (h + l_) / 2.0
    return best, pts, slope

# ------------------------------------------------------------- solve: point


SUPP_MAP = (("vmafneg_hmean", "neg_hmean"), ("vmafneg_min", "neg_min"),
            ("vmafneg_low1", "neg_low1"), ("vmafneg_first1s", "neg_first1s"),
            ("vmaf", "vmaf"), ("psnr_yuv611", "psnr_yuv611"),
            ("ssim_yuv611", "ssim_yuv611"))


def supp_of(rec):
    d = {k: rec.get(f) for k, f in SUPP_MAP}
    d["frames"] = rec.get("frames_scored")
    d["frames_ok"] = bool(rec.get("frames_ok"))
    return d


def anchors_for(cell, goals):
    """The point anchors a cell needs: `def` for G3 (and the quality
    reading); `ft1` for a whole-clip G1. A windowed cell's G1 and G2 are
    matched on the windows instead."""
    out = ["def"] if 3 in goals or not windowed(cell) else []
    if 1 in goals and not windowed(cell):
        out.append("ft1")
    return out or ["def"]


def do_point(cell, anchor, force=False, log=print):
    i = info(cell)
    k = f"{cell['key']}/{anchor}"
    pts = jload(POINTS)
    bh, rh = bin_hash(ps.N265), bin_hash(ps.X265)
    old = pts.get(k)
    if old and not force and old.get("bin") == bh and \
            old.get("ref_bin") == rh and old.get("frames") == i["frames"]:
        log(f"  {k}: solved already (crf {old['ours_crf']}, "
            f"{old['dsize_pct']:+.2f}%)")
        return old
    t0 = time.time()
    prof = "solve" if anchor == "def" else "solve_ft1"
    log(f"  {k}: {i['w']}x{i['h']} {i['frames']} frames @ {fps_of(i):.3f}, "
        f"{'staged ' + i['path'] if i['src'] == 'y4m' else 'decoder-fed'}")
    ref = measure(cell, "ref", "crf", REF_CRF, prof=prof, stride=1, log=log)
    target = ref["bytes"]
    start = old["ours_crf"] if old else REF_CRF

    def size_of(q):
        return measure(cell, "ours", "crf", q, log=log)["bytes"]
    best, tried, slope = secant(size_of, target, start)
    ref_crf = REF_CRF
    if best[2] > ps.TOL / 100:
        # our curve steps over the target (the integer-QP staircase): move
        # the reference onto our nearest point instead, and say so
        log(f"    ours steps over the target ({100 * best[2]:.2f}% at crf "
            f"{best[0]}); moving x265 onto our point")

        def ref_size(q):
            return measure(cell, "ref", "crf", q, prof=prof, log=log)["bytes"]
        xb, _, _ = secant(ref_size, best[1], REF_CRF + math.log(
            best[1] / target) / slope)
        if xb[2] < best[2]:
            ref = measure(cell, "ref", "crf", xb[0], prof=prof, stride=1,
                          log=log)
            ref_crf, target = xb[0], ref["bytes"]
            best = (best[0], best[1], abs(best[1] / target - 1))
    ours = measure(cell, "ours", "crf", best[0], stride=1, log=log)
    ds = (ours["bytes"] / target - 1) * 100
    gs = lambda r: gate_score(r) if has_gate(r, cell) else None
    pt = {"tier": cell["tier"], "class": cell["cls"], "title": cell["title"],
          "anchor": anchor, "src": i["path"], "src_kind": i["src"],
          "src_bytes": os.path.getsize(i["path"]), "start": i["start"],
          "size": f"{i['w']}x{i['h']}", "frames": i["frames"],
          "fps": fps_of(i), "ref_crf": ref_crf, "ref_bytes": ref["bytes"],
          "ref_md5": ref["md5"], "ref_vmafneg": gs(ref),
          "ref_psnr_y": ref.get("psnr_y"), "ours_crf": best[0],
          "ours_bytes": ours["bytes"], "ours_md5": ours["md5"],
          "ours_vmafneg": gs(ours), "ours_psnr_y": ours.get("psnr_y"),
          "dsize_pct": round(ds, 3), "commit": ps.commit(), "bin": bh,
          "ref_bin": rh, "solved": now(), "solve_s": round(time.time() - t0, 1),
          "encodes": len(tried), "threads": THREADS,
          "ours_supp": supp_of(ours), "ref_supp": supp_of(ref),
          "lookahead": f"ours {OUR_DEPTH} / x265 {REF_DEPTH}", "metric": GATE}
    jput(POINTS, k, pt)
    log(f"  {k}: ours crf {best[0]} {ours['bytes']} B ({ds:+.2f}%)  d{MNAME} "
        f"{(gs(ours) or 0) - (gs(ref) or 0):+.2f}  dPSNR-Y "
        f"{ours['psnr_y'] - ref['psnr_y']:+.2f}  [{time.time() - t0:.0f} s]")
    return pt


def do_solve(args):
    goals = [int(g) for g in args.goals.split(",")]
    for c in select(args, ("L", "C")):
        for an in anchors_for(c, goals):
            do_point(c, an, args.force)

# ----------------------------------------------------------- solve: windows


def win_key(cell, g):
    return f"{cell['key']}/G{g}"


def do_windows(args):
    """Untimed: ours solved onto x265's summed window bytes, per timed
    thread count. G1's anchor is x265 at one frame thread, G2's at its
    default four (the pool and --no-asm do not move its CRF pictures);
    ours writes one stream at every thread count, so its encodes serve
    both goals."""
    goals = [g for g in (int(x) for x in args.goals.split(",")) if g in (1, 2)]
    for cell in select(args, ("M", "L", "C")):
        if not windowed(cell):
            continue
        wins = cell_windows(cell)
        pt = jload(POINTS).get(f"{cell['key']}/def")
        for g in goals:
            k = win_key(cell, g)
            old = jload(WIN_PATH).get(k)
            if old and not args.force and old.get("bin") == bin_hash(
                    ps.N265) and old.get("windows") == [list(w) for w in wins]:
                print(f"  {k}: solved already (ours crf {old['ours_crf']}, "
                      f"{old['dsize_pct']:+.2f}%)", flush=True)
                continue
            prof = "solve_ft1" if g == 1 else "solve"
            print(f"  {k}: windows " + ", ".join(f"{s}+{n}" for s, n in wins),
                  flush=True)

            def tot(arm, q, pr=prof):
                rs = [measure(cell, arm, "crf", q, s, n, prof=pr)
                      for s, n in wins]
                return sum(r["bytes"] for r in rs), [r["md5"] for r in rs]
            xb, xmd = tot("ref", REF_CRF)
            start = pt["ours_crf"] if pt else REF_CRF
            before = tot("ours", start)[0]
            best, tried, slope = secant(lambda q: tot("ours", q)[0], xb, start,
                                        aim=WIN_TOL / 100)
            rec = {"windows": [list(w) for w in wins], "ref_crf": REF_CRF,
                   "ref_bytes": xb, "ref_md5": xmd, "ours_crf": best[0],
                   "ours_bytes": best[1], "ours_md5": tot("ours", best[0])[1],
                   "dsize_pct": round(100 * (best[1] / xb - 1), 3),
                   "before_crf": start,
                   "before_dsize_pct": round(100 * (before / xb - 1), 3),
                   "x265_threads": " ".join(REF_THREADS[prof]),
                   "encodes": len(tried), "bin": bin_hash(ps.N265),
                   "ref_bin": bin_hash(ps.X265), "date": now()}
            if abs(rec["dsize_pct"]) > ps.TOL:
                # the 30 s staircase is coarse: move the reference instead
                xr, _, _ = secant(lambda q: tot("ref", q)[0], best[1],
                                  REF_CRF + math.log(best[1] / xb) / slope,
                                  aim=WIN_TOL / 100)
                if xr[2] < best[2]:
                    rec.update(ref_crf=xr[0], ref_bytes=xr[1],
                               ref_md5=tot("ref", xr[0])[1],
                               dsize_pct=round(100 * (best[1] / xr[1] - 1), 3),
                               ref_moved=True)
            jput(WIN_PATH, k, rec)
            print(f"  {k}: ours crf {rec['ours_crf']} {rec['dsize_pct']:+.2f}% "
                  f"(was {rec['before_dsize_pct']:+.2f}% at crf {start})"
                  + (f"; x265 moved to crf {rec['ref_crf']}"
                     if rec.get("ref_moved") else "")
                  + ("  [over 1%]" if abs(rec["dsize_pct"]) > ps.TOL else ""),
                  flush=True)

# ------------------------------------------------------------- ladders


def lad_key(cell, mode, arm, q, depth):
    exe = ps.N265 if arm == "ours" else ps.X265
    r = ps._fmt_q(q) if mode != "vbr" else f"k{int(q)}"
    return f"{cell['key']}|{mode}|{arm}|{r}|la{depth}|{bin_hash(exe)}"


def vbr_targets(cell, lad=None):
    lad = lad or jload(LAD_PATH)
    rs = [lad.get(lad_key(cell, "crf", "ref", q, REF_DEPTH))
          for q in LAD_RUNGS]
    if not all(rs):
        return None
    return [int(round(r["kbps"])) for r in rs]


def ladder_rung(cell, mode, arm, q, depth, stride):
    k = lad_key(cell, mode, arm, q, depth)
    have = jload(LAD_PATH).get(k)
    if have and have.get("stride") == stride and has_gate(have, cell):
        return have
    r = measure(cell, arm, mode, q, depth=depth if arm == "ours" else None,
                stride=stride)
    rec = {k_: r.get(k_) for k_ in ("bytes", "kbps", "neg", "neg_hmean",
                                    "psnr_y", "frames", "frames_scored",
                                    "frames_ok", "stride", "md5", "enc_s",
                                    "date")}
    rec.update({k_: v for k_, v in r.items() if k_.startswith("v1_")})
    rec.update(depth=depth, threads=THREADS, tier=cell["tier"])
    if mode == "vbr":
        rec.update(target=int(q), maxrate=int(round(MAXR * q)),
                   bufsize=int(round(BUFR * q)),
                   rate_err_pct=round(100 * (r["kbps"] / q - 1), 2))
    return jput(LAD_PATH, k, rec)


def run_ladder(cell, modes, depths):
    stride = STRIDE[cell["tier"]]
    for mode in modes:
        if mode == "crf":
            qs = LAD_RUNGS
        else:
            for q in LAD_RUNGS:          # the targets come from these
                ladder_rung(cell, "crf", "ref", q, REF_DEPTH, stride)
            qs = vbr_targets(cell)
        print(f"  {cell['name']} ({cell['tier']}) {mode.upper()}: rungs "
              + ", ".join(ps._fmt_q(q) for q in qs), flush=True)
        for q in qs:
            ladder_rung(cell, mode, "ref", q, REF_DEPTH, stride)
        for d in depths:
            for q in qs:
                ladder_rung(cell, mode, "ours", q, d, stride)
            c = curve(cell, mode, d)
            if c:
                print(f"  {cell['name']} {mode.upper()} depth {d}/{REF_DEPTH}"
                      + ("" if c["n"] == len(RUNGS)
                         else f" ({c['n']}-rung screen, not the spec's BD)")
                      + ":"
                      f" BD-{c['metric'].upper()} {c['bn']:+.2f}%"
                      + (f" (NEG {c['bneg']:+.2f}%)" if c["bneg"] is not None
                         and c["metric"] != "neg" else "")
                      + f" (PSNR-Y {c['bp']:+.2f}%)"
                      + (f"; tracking ours {fmt_track(c['track_o'])}"
                         if mode == "vbr" else ""), flush=True)


def split_modes(arg):
    modes = ["crf", "vbr"] if arg == "both" else arg.split(",")
    bad = [m for m in modes if m not in ("crf", "vbr")]
    if bad:
        sys.exit(f"parity_long: unknown --modes {','.join(bad)} "
                 "(crf, vbr, crf,vbr or both)")
    return modes


def do_ladder(args):
    modes = split_modes(args.modes)
    for cell in select(args, ("M", "L", "C")):
        run_ladder(cell, modes, (OUR_DEPTH,))


def do_sweep(args):
    modes = split_modes(args.modes)
    want = set(SWEEP_CELLS)
    for cell in select(args, ("M", "L")):
        if cell["key"] in want:
            run_ladder(cell, modes, SWEEP_DEPTHS)


def curve(cell, mode, depth=OUR_DEPTH, lad=None):
    """One BD curve of the current binaries: five rungs both sides at one
    stride, or None. Rows of another build of ours never count."""
    lad = lad if lad is not None else jload(LAD_PATH)
    qs = list(LAD_RUNGS) if mode == "crf" else vbr_targets(cell, lad)
    if not qs:
        return None
    x = [lad.get(lad_key(cell, mode, "ref", q, REF_DEPTH)) for q in qs]
    o = [lad.get(lad_key(cell, mode, "ours", q, depth)) for q in qs]
    if not all(x) or not all(o):
        return None
    if len({r["stride"] for r in x + o}) != 1:
        return None
    if not all(has_gate(r, cell) for r in x + o):
        return None
    bn = bd.bd_rate([r["kbps"] for r in x], [gate_score(r) for r in x],
                    [r["kbps"] for r in o], [gate_score(r) for r in o])
    bneg = (bd.bd_rate([r["kbps"] for r in x], [r["neg"] for r in x],
                       [r["kbps"] for r in o], [r["neg"] for r in o])
            if all(r.get("neg") is not None for r in x + o) else None)
    bp = bd.bd_rate([r["kbps"] for r in x], [r["psnr_y"] for r in x],
                    [r["kbps"] for r in o], [r["psnr_y"] for r in o])
    if bn is None:
        return None
    c = {"bn": bn, "bp": bp, "bneg": bneg, "metric": GATE, "n": len(qs),
         "stride": x[0]["stride"],
         "rungs": qs, "frames_ok": all(r.get("frames_ok") for r in x + o)}
    if mode == "vbr":
        c["track_o"] = [100 * (r["kbps"] / q - 1) for r, q in zip(o, qs)]
        c["track_x"] = [100 * (r["kbps"] / q - 1) for r, q in zip(x, qs)]
    return c


def ladder_rows_present(cell, mode, depth=OUR_DEPTH):
    lad = jload(LAD_PATH)
    pre = f"{cell['key']}|{mode}|ours|"
    return sum(1 for k in lad if k.startswith(pre) and f"|la{depth}|" in k)


def fmt_track(e):
    return "/".join(f"{x:+.1f}" for x in e) + (
        " OUT" if max(abs(x) for x in e) > TRACK_BAR else "")

# ------------------------------------------------------------- credits


def cred_cell(title):
    """The credits span of a title as a cell of its own: the staged 1080p
    excerpt from the first credits frame to the census's end."""
    sp = credits_span(title)
    if not sp:
        return None
    p = staged_path(title, "1080p")
    if not p:
        return None
    w, h, fps, n = y4m_shape(p)
    c = _mk("L", "1080p", title, "1080p", kind="cred")
    c["key"] = f"cred/1080p/{title}"
    _INFO[c["key"]] = dict(w=w, h=h, fps=fps, frames=sp[1] - sp[0] + 1,
                           src="y4m", path=p, start=sp[0], master_start=0)
    return c


def do_credits(args):
    titles = [t for t in L_TITLES if not args.cells or t in
              args.cells.split(",")]
    for t in titles:
        c = cred_cell(t)
        if not c:
            print(f"  {t}: no credits span or no staged 1080p excerpt")
            continue
        pt = jload(POINTS).get(f"L/1080p/{t}/def")
        if not pt:
            print(f"  {t}: the 1080p cell has no matched point yet; solve "
                  "it first (the credits are coded at its crf)")
            continue
        print(f"  {t}: credits frames {info(c)['start']}-"
              f"{info(c)['start'] + info(c)['frames'] - 1}", flush=True)
        for q in RUNGS + (REF_CRF,):
            ladder_rung_cred(c, "ref", q)
        for q in RUNGS + (pt["ours_crf"],):
            ladder_rung_cred(c, "ours", q)


def ladder_rung_cred(cell, arm, q):
    k = lad_key(cell, "cred", arm, q, OUR_DEPTH if arm == "ours" else
                REF_DEPTH)
    if k in jload(LAD_PATH):
        return
    r = measure(cell, arm, "crf", q, stride=1, psnr_only=True)
    jput(LAD_PATH, k, {"bytes": r["bytes"], "kbps": r["kbps"],
                       "psnr_y": r["psnr_y"], "frames": r["frames"],
                       "frames_ok": r["frames_ok"], "q": q, "date": r["date"],
                       "md5": r["md5"]})


def size_at(pts, y):
    """Bytes on an encoder's own curve at PSNR-Y y, interpolated in log
    bytes: (value, extrapolated?)."""
    pts = sorted(pts, key=lambda p: p["psnr_y"])
    ys = [p["psnr_y"] for p in pts]
    lb = [math.log(p["bytes"]) for p in pts]
    if ys[0] <= y <= ys[-1]:
        for a in range(len(ys) - 1):
            if ys[a] <= y <= ys[a + 1]:
                f = 0 if ys[a + 1] == ys[a] else (y - ys[a]) / (ys[a + 1] - ys[a])
                return math.exp(lb[a] + f * (lb[a + 1] - lb[a])), False
    i = (0, 1) if y < ys[0] else (-2, -1)
    dy = ys[i[1]] - ys[i[0]]
    s = (lb[i[1]] - lb[i[0]]) / dy if dy else 0.0
    return math.exp(lb[i[0]] + s * (y - ys[i[0]])), True


def credits_rows():
    lad = jload(LAD_PATH)
    out = []
    for t in L_TITLES:
        c = cred_cell(t)
        sp = credits_span(t)
        if not sp:
            continue
        pt = jload(POINTS).get(f"L/1080p/{t}/def")
        if not c or not pt:
            out.append((t, sp, None))
            continue

        def get(arm, q):
            return lad.get(lad_key(c, "cred", arm, q, OUR_DEPTH if arm ==
                                   "ours" else REF_DEPTH))
        xs = [get("ref", q) for q in RUNGS]
        os_ = [get("ours", q) for q in RUNGS]
        x28, mp = get("ref", REF_CRF), get("ours", pt["ours_crf"])
        if not all(xs) or not all(os_) or not x28 or not mp:
            out.append((t, sp, None))
            continue
        a, ea = size_at(os_ + [mp], x28["psnr_y"])
        b, eb = size_at(xs + [x28], mp["psnr_y"])
        out.append((t, sp, dict(x=x28, o=mp, crf=pt["ours_crf"],
                                at_ref=(100 * (a / x28["bytes"] - 1), ea),
                                at_ours=(100 * (mp["bytes"] / b - 1), eb))))
    return out

# --------------------------------------------------------------- timed


def quiet_or_exit():
    """parity_spec's quiet check (total box CPU), once, before the first
    timed run: during the block the CPU is our own arms."""
    q, (line, note) = ps.wait_quiet()
    print(f"  box: {line}", flush=True)
    return note, q


def feed_alone(cell, s, n, workers):
    """The feeder alone over the first FEEDCHECK_SECS of the span, fps."""
    i = info(cell)
    m = min(n, max(1, int(round(FEEDCHECK_SECS * fps_of(i)))))
    t0 = time.monotonic()
    subprocess.run(feeder(cell, s, m, workers), stdout=subprocess.DEVNULL,
                   check=True)
    return m / max(time.monotonic() - t0, 1e-3)


def leg(cell, arm, goal, mode, q, s, n, workers, wd):
    """One timed encode; the stream goes to scratch so its size and md5
    are recorded (an arm that did not run is visible), then is removed."""
    out = os.path.join(wd, f"{arm}.hevc")
    enc, env = timed_cmd(arm, goal, mode, q, out)
    r = pipe_run(feeder(cell, s, n, workers), enc, env, timed=True)
    size = os.path.getsize(out) if os.path.exists(out) else 0
    md5 = file_md5(out) if size else "-"
    if os.path.exists(out):
        os.remove(out)
    if size < 64:
        sys.exit(f"parity_long: {cell['name']}: the {arm} leg wrote {size} "
                 "bytes; nothing is recorded")
    return dict(wall=r["wall"], cpu=(r["user"] or 0) + (r["sys"] or 0),
                rss=r["rss"], instr=r["instr"], bytes=size, md5=md5,
                load1=os.getloadavg()[0])


def run_row(cell, goal, rnd, arm, m):
    return {"date": datetime.date.today().isoformat(),
            "time": datetime.datetime.now().strftime("%H:%M:%S"),
            "commit": ps.commit(), "tier": cell["tier"],
            "title": cell["name"], "goal": f"G{goal}", "round": rnd,
            "arm": arm, "wall_s": f"{m['wall']:.3f}", "bytes": m["bytes"],
            "md5": m["md5"], "load1": f"{m['load1']:.2f}"}


def spec_row(cell, goal, pt, walls, sizes, feed, rounds, flags, notes,
             quality=True):
    i = info(cell)
    mo = statistics.median(walls["ours"]) if walls["ours"] else None
    mr = statistics.median(walls["ref"]) if walls["ref"] else None
    ratio = mo / mr if mo and mr and "feeder-bound" not in flags else None
    ob, rb = sizes
    ds = (ob / rb - 1) * 100 if ob and rb else None
    r = {k: "" for k in ps.COLUMNS}
    r.update({"codec": "yah265", "date": datetime.date.today().isoformat(),
              "commit": ps.commit(), "ref": "x265 --preset medium"
              + ("" if goal == 3 else " --no-asm"),
              "ref_version": ps.x265_version(), "tier": cell["tier"],
              "class": cell["cls"], "title": cell["title"],
              "size": f"{i['w']}x{i['h']}", "goal": f"G{goal}",
              "ours_wall_s": f"{mo:.3f}" if mo else "",
              "ref_wall_s": f"{mr:.3f}" if mr else "",
              "ratio": ps.f2(ratio), "feed_fps": f"{feed:.0f}",
              "ours_bytes": ob, "ref_bytes": rb, "dsize_pct": ps.f2(ds),
              "rounds": rounds, "valid": flags[0] if flags else "yes",
              "note": "; ".join(notes)})
    if quality and pt:
        r.update({"ours_vmafneg": ps.f2(pt["ours_vmafneg"]),
                  "ref_vmafneg": ps.f2(pt["ref_vmafneg"]),
                  "dvmafneg": ps.f2(pt["ours_vmafneg"] - pt["ref_vmafneg"]),
                  "ours_psnr_y": ps.f2(pt["ours_psnr_y"]),
                  "ref_psnr_y": ps.f2(pt["ref_psnr_y"]),
                  "dpsnr_y": ps.f2(pt["ours_psnr_y"] - pt["ref_psnr_y"]),
                  **ps.supp_cols(pt)})
        fok = (pt.get("ours_supp", {}).get("frames_ok")
               and pt.get("ref_supp", {}).get("frames_ok"))
        r["frames_decoded_ok"] = "yes" if fok else "no"
        if not fok and "frame-mismatch" not in flags:
            r["valid"] = "frame-mismatch"
    return r


def common_notes(cell):
    i = info(cell)
    out = [span_note(cell),
           f"lookahead: ours {OUR_DEPTH} / reference {REF_DEPTH} (each "
           "encoder's medium default)",
           "feed: " + ("staged Y4M" if i["src"] == "y4m" and
                       cell["kind"] == "excerpt" else
                       "Y4M block copy" if i["src"] == "y4m" else
                       "VideoToolbox decode")]
    if cell.get("ten_bit"):
        out.append("10-bit source converted to 8-bit 4:2:0")
    if cell["tier"] == "C":
        out.append("tier C: speed shown, not gating")
    return out


def time_span(cell, goal, mode, qs, spans, rounds, wd, runs, log=print):
    """Rounds interleaved (order reversed each round, and each window), the
    feeder judged on each round. Returns (walls, bytes, md5s, feed fps,
    feeder-bound fps or None, instruction counts)."""
    workers = timed_workers(cell)
    feed = feed_alone(cell, spans[0][0], spans[0][1], workers)
    walls = {"ours": [], "ref": []}
    size = {"ours": [], "ref": []}
    md5s = {"ours": [], "ref": []}
    instr = {"ours": [], "ref": []}
    bound = None
    for r in range(rounds):
        tw = {"ours": 0.0, "ref": 0.0}
        tb = {"ours": 0, "ref": 0}
        for wi, (s, n) in enumerate(spans):
            order = ("ours", "ref") if (r + wi) % 2 == 0 else ("ref", "ours")
            for arm in order:
                m = leg(cell, arm, goal, mode, qs[arm], s, n, workers, wd)
                tw[arm] += m["wall"]
                tb[arm] += m["bytes"]
                md5s[arm].append(m["md5"])
                if m["instr"]:
                    instr[arm].append(m["instr"])
                runs.append(run_row(cell, goal, r + 1, arm, m))
                log(f"  G{goal} r{r + 1} {cell['name']:<20} {arm:<4} "
                    f"{s}+{n} {m['wall']:9.3f} s {m['bytes']:>11} B "
                    f"md5 {m['md5'][:12]} load {m['load1']:.2f}")
        for arm in tw:
            walls[arm].append(tw[arm])
            size[arm].append(tb[arm])
        nf = sum(n for _, n in spans)
        fast = nf / min(tw.values())
        if feed < ps.FEED_MARGIN * fast:
            bound = fast
            log(f"  G{goal} {cell['name']}: FEEDER-BOUND ({feed:.0f} < "
                f"{ps.FEED_MARGIN} x {fast:.0f} fps); no more rounds")
            break
    return walls, size, md5s, feed, bound, instr


def do_time(args):
    """Tiers L and C, timed: G3 on the whole cell; G1 and G2 on the whole
    clip for the short tier C clips. G1/G2 of a windowed cell is
    --time-windows."""
    goals = [int(g) for g in args.goals.split(",")]
    cells = select(args, ("L", "C"))
    pts = jload(POINTS)
    plan = []
    for c in cells:
        for g in goals:
            if g in (1, 2) and windowed(c):
                continue
            an = "ft1" if g == 1 else "def"
            pt = pts.get(f"{c['key']}/{an}")
            if not pt:
                sys.exit(f"parity_long: {c['key']}/{an} is not solved; run "
                         "--solve first")
            plan.append((c, g, pt))
    if not plan:
        sys.exit("parity_long: nothing to time (G1/G2 of a windowed cell is "
                 "--time-windows)")
    ld, quiet = quiet_or_exit()
    wd = tempfile.mkdtemp(prefix="plt.", dir=ensure_work())
    try:
        for c, g, pt in plan:
            runs, rows = [], []
            rounds = (G3_ROUNDS if c["kind"] == "excerpt" else
                      ps.ROUNDS[g])
            n = info(c)["frames"]
            walls, size, md5s, feed, bound, instr = time_span(
                c, g, "crf", {"ours": pt["ours_crf"], "ref": pt["ref_crf"]},
                [(0, n)], rounds, wd, runs)
            flags, notes = [], common_notes(c)
            ob = statistics.median(size["ours"])
            rb = statistics.median(size["ref"])
            if bound:
                flags.append("feeder-bound")
                notes.append(f"feeder {feed:.0f} fps < {ps.FEED_MARGIN}x the "
                             f"faster arm's {bound:.0f}; ratio void")
            if not quiet:
                flags.append("not-quiet")
                notes.append(f"{ld} when the block started")
            if abs(ob / rb - 1) * 100 > ps.TOL:
                flags.append("size-mismatch")
            if min(len(walls["ours"]), len(walls["ref"])) < 2 and not bound:
                flags.append("single-run")
            for arm, name in (("ours", "ours"), ("ref", "x265")):
                if len(set(md5s[arm])) > 1:
                    notes.append(("FAIL: --threads 1 run-to-run md5 mismatch "
                                  f"({name})") if g == 1 else
                                 f"warning: {name} md5 differs between rounds")
            if set(md5s["ours"]) != {pt["ours_md5"]}:
                notes.append("warning: ours md5 differs from the solved "
                             "stream")
            notes.append(f"ours --crf {ps._fmt_q(pt['ours_crf'])} (solved)")
            if pt["ref_crf"] != REF_CRF:
                notes.append(f"reference crf {pt['ref_crf']}, not its "
                             f"default {REF_CRF} (moved onto our nearest size)")
            notes.append(f"md5 ours {md5s['ours'][0][:8]} ref "
                         f"{md5s['ref'][0][:8]}")
            if instr["ours"] and instr["ref"]:
                notes.append("instructions ours/ref "
                             f"{statistics.median(instr['ours']) / statistics.median(instr['ref']):.3f}")
            rows.append(spec_row(c, g, pt, walls, (ob, rb), feed,
                                 min(len(walls["ours"]), len(walls["ref"])),
                                 flags, notes))
            ps.append_csv(ps.RUNS_PATH, ps.RUN_COLUMNS, runs)
            ps.append_csv(ps.CSV_PATH, ps.COLUMNS, rows)
            print(f"  G{g} {c['name']:<20} ratio {rows[-1]['ratio'] or '-'} "
                  f"valid {rows[-1]['valid']}", flush=True)
    finally:
        subprocess.run(["rm", "-rf", wd])


def do_time_windows(args):
    """G1 and G2 on the stratified windows of tiers M and L (and lash),
    at the rate factors matched on those windows per thread count."""
    goals = [g for g in (int(x) for x in args.goals.split(",")) if g in (1, 2)]
    cells = [c for c in select(args, ("M", "L", "C")) if windowed(c)]
    wins = jload(WIN_PATH)
    plan = []
    for c in cells:
        for g in goals:
            w = wins.get(win_key(c, g))
            if not w:
                sys.exit(f"parity_long: {win_key(c, g)} has no window match; "
                         "run --windows first")
            plan.append((c, g, w))
    ld, quiet = quiet_or_exit()
    wd = tempfile.mkdtemp(prefix="plw.", dir=ensure_work())
    try:
        for c, g, w in plan:
            runs = []
            spans = [tuple(x) for x in w["windows"]]
            walls, size, md5s, feed, bound, instr = time_span(
                c, g, "crf", {"ours": w["ours_crf"], "ref": w["ref_crf"]},
                spans, WIN_ROUNDS, wd, runs)
            ob = statistics.median(size["ours"])
            rb = statistics.median(size["ref"])
            wdel = 100 * (ob / rb - 1)
            flags, notes = [], common_notes(c)
            nf = sum(n for _, n in spans)
            notes.append(f"sampled: {len(spans)} window(s) of "
                         f"{WIN_SECS:g} s, {nf} of {info(c)['frames']} frames")
            notes.append("threads: 1 both sides" if g == 1 else
                         "threads: default both sides")
            notes.append(f"window bytes {wdel:+.2f}% (matched on the windows "
                         f"at this thread count; {w['before_dsize_pct']:+.2f}%"
                         " before the match)")
            if w["ref_crf"] != REF_CRF:
                notes.append(f"reference at crf {w['ref_crf']} on the "
                             f"windows, not its default {REF_CRF}")
            if bound:
                flags.append("feeder-bound")
            if abs(wdel) > WIN_VOID:
                flags.append("size-mismatch")
                notes.append(f"VOID for G{g}: window bytes over {WIN_VOID:g}%")
            if not quiet:
                flags.append("not-quiet")
            if WIN_ROUNDS < 2 and "single-run" not in flags:
                pass               # yah264 reads windows once; not flagged
            if md5s["ours"] != w["ours_md5"] * len(walls["ours"]):
                notes.append("warning: ours window md5s differ from the "
                             "untimed solve")
            row = spec_row(c, g, None, walls, (ob, rb), feed,
                           min(len(walls["ours"]), len(walls["ref"])),
                           flags, notes, quality=False)
            ps.append_csv(ps.RUNS_PATH, ps.RUN_COLUMNS, runs)
            ps.append_csv(ps.CSV_PATH, ps.COLUMNS, [row])
            print(f"  G{g} {c['name']:<20} ratio {row['ratio'] or '-'} "
                  f"windows {wdel:+.2f}% valid {row['valid']}", flush=True)
    finally:
        subprocess.run(["rm", "-rf", wd])


def do_vbr_wall(args):
    """G3 only: one rung per M and L cell at x265's CRF 28 achieved rate,
    maxrate 1.5K, buffer 2K, rounds interleaved, both achieved rates
    printed, no size match."""
    pts = jload(POINTS)
    allp = ps.load_points()
    plan = []
    for c in select(args, ("M", "L")):
        pt = pts.get(f"{c['key']}/def") or allp.get(f"{c['key']}/def")
        if not pt:
            sys.exit(f"parity_long: {c['key']}/def is not solved; its x265 "
                     "CRF 28 rate is the rung's target")
        k = int(round(pt["ref_bytes"] * 8 / (pt["frames"] / pt["fps"]) / 1000))
        plan.append((c, k))
    ld, quiet = quiet_or_exit()
    wd = tempfile.mkdtemp(prefix="plv.", dir=ensure_work())
    try:
        for c, k in plan:
            runs = []
            n = info(c)["frames"]
            walls, size, md5s, feed, bound, _ = time_span(
                c, 3, "vbr", {"ours": k, "ref": k}, [(0, n)], VBR_ROUNDS,
                wd, runs)
            secs = n / fps_of(info(c))
            kb = {a: statistics.median(size[a]) * 8 / secs / 1000
                  for a in size}
            mo, mr = (statistics.median(walls["ours"]),
                      statistics.median(walls["ref"]))
            row = {"date": datetime.date.today().isoformat(),
                   "time": datetime.datetime.now().strftime("%H:%M"),
                   "commit": ps.commit(), "tier": c["tier"],
                   "class": c["cls"], "title": c["title"],
                   "size": f"{info(c)['w']}x{info(c)['h']}", "k": k,
                   "maxrate": int(round(MAXR * k)),
                   "bufsize": int(round(BUFR * k)),
                   "rounds": len(walls["ours"]), "ours_wall_s": f"{mo:.3f}",
                   "ref_wall_s": f"{mr:.3f}", "ratio": f"{mo / mr:.3f}",
                   "ours_kbps": f"{kb['ours']:.1f}",
                   "ref_kbps": f"{kb['ref']:.1f}",
                   "ours_err_pct": f"{100 * (kb['ours'] / k - 1):.2f}",
                   "ref_err_pct": f"{100 * (kb['ref'] / k - 1):.2f}",
                   "feed_fps": f"{feed:.0f}",
                   "feeder_bound": "yes" if bound else "no",
                   "load1": f"{os.getloadavg()[0]:.2f}",
                   "ours_md5": md5s["ours"][0],
                   "ref_md5": md5s["ref"][0], "ours_bin": bin_hash(ps.N265),
                   "lookahead": f"ours {OUR_DEPTH} / x265 {REF_DEPTH}",
                   "note": "" if quiet else f"not quiet: {ld}"}
            ps.append_csv(ps.RUNS_PATH, ps.RUN_COLUMNS, [
                dict(r, goal="VBR") for r in runs])
            ps.append_csv(VBRW_PATH, VBRW_COLS, [row])
            print(f"  VBR {c['name']:<20} ratio {row['ratio']}  ours "
                  f"{row['ours_kbps']} ({row['ours_err_pct']}%) x265 "
                  f"{row['ref_kbps']} ({row['ref_err_pct']}%) target {k}",
                  flush=True)
    finally:
        subprocess.run(["rm", "-rf", wd])

# ------------------------------------------------------------------ plan

# The dry run's throughput model is the logged walls: every valid timed row
# of docs/data/parity-spec.csv gives Mpx/s per class, goal and arm, and a
# class without a row borrows its neighbor's. The untimed 6-thread rate is
# taken as half the default-thread G3 rate (an assumption: 6 of 18 cores,
# scaling sublinearly), and libvmaf scores 25 Mpx/s at four threads (the
# median over 1350 rungs of yah264's ladder lane on this box, VMAF-NEG and
# PSNR, 2026-10-08..12). A VideoToolbox decode caps a decoder-fed cell.
UNTIMED_SHARE = 0.5
VMAF_MPXS = 25.0
SSIM_COST = 0.25            # the SSIM pass, as a share of the VMAF pass
VT_FPS1, VT_FPS3 = 230.0, 435.0
CLASS_ORDER = ("CIF", "720p", "1080p", "2160p")
SOLVE_ENC = 5               # one reference + about four secant steps
WIN_EVALS = 3               # secant evaluations of a window match


def rate_model():
    rows = ps.read_csv(ps.CSV_PATH)
    pts = ps.load_points()
    got = {}
    for r in rows:
        if r.get("valid") not in ("yes", "single-run") or \
                "window" in r.get("note", ""):
            continue
        an = "ft1" if r["goal"] == "G1" else "def"
        pt = pts.get(f"{r['tier']}/{r['class']}/{r['title']}/{an}")
        if not pt:
            continue
        w, h = (int(x) for x in r["size"].split("x"))
        mpx = pt["frames"] * w * h / 1e6
        for arm, col in (("ours", "ours_wall_s"), ("ref", "ref_wall_s")):
            wall = ps.fnum(r[col])
            if wall:
                got.setdefault((r["class"], r["goal"], arm), []).append(
                    mpx / wall)
    return {k: statistics.median(v) for k, v in got.items()}


def model_rate(model, cls, goal, arm):
    i = CLASS_ORDER.index(cls)
    for d in (0, -1, 1, -2, 2, -3, 3):
        j = i + d
        if 0 <= j < len(CLASS_ORDER) and (CLASS_ORDER[j], goal, arm) in model:
            return model[(CLASS_ORDER[j], goal, arm)]
    return {"G1": 2.0, "G2": 25.0, "G3": 60.0}[goal]


def feed_cap(cell):
    """Frames a second the cell's feeder can give, modeled."""
    i = info(cell)
    if i["src"] == "y4m":
        mbps = 900.0 if i["path"].startswith("<") else 5000.0
        return mbps * 1e6 / (i["w"] * i["h"] * 1.5)
    nat = TITLES[cell["title"]]["native"]
    vt = VT_FPS3 if timed_workers(cell) > 1 else VT_FPS1
    return vt * (3840 * 2160 / (nat[0] * nat[1])) ** 0.7


def est(cell, frames, goal, arm, model, untimed=False):
    i = info(cell)
    mpx = frames * i["w"] * i["h"] / 1e6
    r = model_rate(model, cell["cls"], goal, arm)
    if untimed:
        r *= UNTIMED_SHARE
    return max(mpx / r, frames / feed_cap(cell))


def score_s(cell, frames, stride):
    i = info(cell)
    return frames / stride * i["w"] * i["h"] / 1e6 / VMAF_MPXS * (
        1 + SSIM_COST)


def do_plan(args):
    model = rate_model()
    tiers = [args.tier] if args.tier else ["S", "M", "L", "C"]
    pts, wins, lad = ps.load_points(), jload(WIN_PATH), jload(LAD_PATH)
    pts.update(jload(POINTS))
    tot = {"cells": 0, "untimed_enc": 0, "scored": 0, "timed_runs": 0,
           "u_s": 0.0, "t_s": 0.0, "u_left_s": 0.0, "t_left_s": 0.0}
    by = {}

    def add(kind, n_enc, n_sc, secs, done=False, timed=False):
        b = by.setdefault(kind, [0, 0, 0.0, 0.0])
        b[0] += n_enc
        b[1] += n_sc
        b[2] += secs
        b[3] += 0.0 if done else secs
        tot["timed_runs" if timed else "untimed_enc"] += n_enc
        tot["scored"] += n_sc
        tot["t_s" if timed else "u_s"] += secs
        if not done:
            tot["t_left_s" if timed else "u_left_s"] += secs

    print("# Parity table, DRY RUN (nothing below has been run)\n")
    print(f"ours : {ps.N265}  (bin {bin_hash(ps.N265) if os.path.exists(ps.N265) else 'not built'})")
    print(f"x265 : {ps.X265}  ({ps.x265_version()})")
    print(f"untimed share: encoders at {THREADS} threads (x265 --pools "
          f"{THREADS} --frame-threads 4, or 1 for a G1 anchor), libvmaf at "
          f"{VMAF_THREADS}; lookahead ours {OUR_DEPTH} / x265 {REF_DEPTH}")
    print("rate model (Mpx/s, median of the logged timed rows; '~' = "
          "borrowed from a neighbouring class):")
    for cls in CLASS_ORDER:
        bits = []
        for g in ("G1", "G2", "G3"):
            for arm in ("ours", "ref"):
                have = (cls, g, arm) in model
                bits.append(f"{g} {arm} {'' if have else '~'}"
                            f"{model_rate(model, cls, g, arm):.1f}")
        print(f"  {cls:<6} " + ", ".join(bits))
    print()
    cells = []
    if "S" in tiers or "M" in tiers:
        for c in ps.CELLS:
            if c[0] in tiers and c[0] == "S":
                cells.append(_mk("S", c[1], c[2], c[1], kind="spec",
                                 spec_cell=c))
    cells += [c for c in all_cells() if c["tier"] in tiers]
    for c in cells:
        if c["kind"] == "spec":
            p = ps.src_of(c["spec_cell"])
            if not os.path.exists(p):
                p = os.path.join(ps.T7_TIER_S, c["spec_cell"][3])
            try:
                w, h, fps, n = y4m_shape(p)
            except OSError:
                print(f"### {c['key']}: source missing ({p})")
                continue
            _INFO[c["key"]] = dict(w=w, h=h, fps=fps, frames=n, src="y4m",
                                   path=p, start=0, master_start=0)
        try:
            i = info(c)
        except (OSError, StopIteration) as e:
            print(f"### {c['key']}: source missing ({e})")
            continue
        tot["cells"] += 1
        n = i["frames"]
        st = STRIDE[c["tier"]]
        print(f"### {c['tier']} {c['name']}  {i['w']}x{i['h']} "
              f"{fps_of(i):.3f} fps, {n} frames ({n / fps_of(i) / 60:.1f} "
              f"min); {'staged ' + i['path'] if i['src'] == 'y4m' else 'decoder-fed from ' + i['path']}")
        if i["h"] % 8 or i["w"] % 8:
            print(f"  geometry: coded {(i['w'] + 7) // 8 * 8}x"
                  f"{(i['h'] + 7) // 8 * 8}, conformance window crops "
                  f"{(-i['w']) % 8} columns, {(-i['h']) % 8} rows")
        fd = shlex.join(feeder(c, 0, n, timed_workers(c)))
        print(f"  feed : {fd if args.full else fd[:150] + ' ...'}")
        # untimed: the matched points
        win = windowed(c) or c["tier"] == "M"
        ans = ["def"] if win else ["def", "ft1"]
        for an in ans:
            pt = pts.get(f"{c['key']}/{an}")
            secs = SOLVE_ENC * est(c, n, "G3", "ours", model, True) + \
                2 * score_s(c, n, 1)
            add("matched points", SOLVE_ENC, 2, secs,
                done=bool(pt and pt.get("bin") == bin_hash(ps.N265)))
            print(f"  point {an}: " + (f"SOLVED ours crf {pt['ours_crf']} "
                                       f"({pt['dsize_pct']:+.2f}%)" if pt
                                       else "not solved")
                  + f"; x265: {shlex.join(ref_cmd('<out>', 'crf', REF_CRF, 'solve' if an == 'def' else 'solve_ft1'))}")
        o_crf = (pts.get(f"{c['key']}/def") or {}).get("ours_crf", "<crf>")
        print(f"  G3   : {shlex.join(timed_cmd('ours', 3, 'crf', o_crf, '<out>')[0])}")
        print(f"         {shlex.join(timed_cmd('ref', 3, 'crf', REF_CRF, '<out>')[0])}")
        rounds = G3_ROUNDS if c["kind"] == "excerpt" else ps.ROUNDS[3]
        g3 = rounds * (est(c, n, "G3", "ours", model) +
                       est(c, n, "G3", "ref", model)) + n / feed_cap(c)
        add("timed G3 (whole cell)", 2 * rounds, 0, g3, timed=True)
        line = f"  est  : G3 {g3 / 60:.1f} min ({rounds} rounds)"
        if win:
            ws = cell_windows(c)
            nw = sum(k for _, k in ws)
            print(f"  windows: " + ", ".join(f"{s}+{k}" for s, k in ws)
                  + f" ({nw} frames, {100 * nw / n:.0f}% of the cell)")
            for g in (1, 2):
                wr = wins.get(win_key(c, g))
                u = (1 + WIN_EVALS) * len(ws)
                secs = (est(c, nw, "G3", "ref", model, True) + WIN_EVALS *
                        est(c, nw, "G3", "ours", model, True))
                add("window matches", u, 0, secs,
                    done=bool(wr and wr.get("bin") == bin_hash(ps.N265)))
                tg = WIN_ROUNDS * (est(c, nw, f"G{g}", "ours", model) +
                                   est(c, nw, f"G{g}", "ref", model))
                add(f"timed G{g} (windows)", 2 * WIN_ROUNDS * len(ws), 0, tg,
                    timed=True)
                line += f", G{g} {tg / 60:.1f} min"
                print(f"  G{g}   : <feed --start S --count N> | "
                      f"{shlex.join(timed_cmd('ours', g, 'crf', (wr or {}).get('ours_crf', '<crf>'), '<out>')[0])}"
                      f"  [{'matched ' + format((wr or {}).get('dsize_pct', 0), '+.2f') + '%' if wr else 'not matched'}]")
        else:
            for g in (1, 2):
                r = ps.ROUNDS[g]
                tg = r * (est(c, n, f"G{g}", "ours", model) +
                          est(c, n, f"G{g}", "ref", model))
                add(f"timed G{g} (whole clip)", 2 * r, 0, tg, timed=True)
                line += f", G{g} {tg / 60:.1f} min"
        if c["tier"] in ("M", "L"):
            cnt = 2 * 2 * len(RUNGS)
            secs = len(RUNGS) * 2 * (est(c, n, "G3", "ours", model, True) +
                                     est(c, n, "G3", "ref", model, True)) + \
                cnt * score_s(c, n, st)
            nl = ladder_rows_present(c, "crf") + ladder_rows_present(c, "vbr")
            add("ladders (CRF + VBR)", cnt, cnt, secs, done=nl >= 10)
            vw = VBR_ROUNDS * (est(c, n, "G3", "ours", model) +
                               est(c, n, "G3", "ref", model))
            add("timed VBR wall", 2 * VBR_ROUNDS, 0, vw, timed=True)
            line += f", VBR wall {vw / 60:.1f} min"
            if c["key"] in SWEEP_CELLS:
                sc = 2 * 2 * len(RUNGS)
                secs = 2 * len(RUNGS) * 2 * est(c, n, "G3", "ours", model,
                                                True) + sc * score_s(c, n, st)
                add("lookahead sweep (depths 20, 30)", sc, sc, secs)
                print(f"  sweep: ours --rc-lookahead 20 and 30 on both "
                      f"ladders (x265 {REF_DEPTH} throughout)")
            print(f"  ladders: CRF {', '.join(map(str, RUNGS))}; VBR at "
                  f"x265's rates there, maxrate {MAXR}x, buffer {BUFR}x; "
                  f"{MNAME} every {'frame' if st == 1 else f'{st}rd frame'}")
        print(line + "\n")
    # credits rows
    if "L" in tiers:
        for t in L_TITLES:
            c = cred_cell(t)
            if not c:
                continue
            n = info(c)["frames"]
            cnt = 2 * len(RUNGS) + 2
            secs = cnt / 2 * (est(c, n, "G3", "ours", model, True) +
                              est(c, n, "G3", "ref", model, True)) + \
                cnt * score_s(c, n, 1) / 4
            add("credits rows (PSNR-Y only)", cnt, cnt, secs)
    print("## Totals (estimates)\n")
    print(f"cells: {tot['cells']}")
    print(f"{'item':<34} {'encodes':>8} {'scored':>7} {'hours':>7} "
          f"{'left':>7}")
    for k, (ne, ns, s, left) in by.items():
        print(f"{k:<34} {ne:>8} {ns:>7} {s / 3600:>7.1f} {left / 3600:>7.1f}")
    print(f"\nuntimed: {tot['untimed_enc']} encodes ({tot['scored']} scored), "
          f"{tot['u_s'] / 3600:.1f} h from scratch, "
          f"{tot['u_left_s'] / 3600:.1f} h left on this binary")
    print(f"timed:   {tot['timed_runs']} encoder runs, {tot['t_s'] / 3600:.1f}"
          " h of quiet box (every timed leg runs again on a new binary)")
    gating = sum(1 for cls, t in SPEC_ROWS if t != "C") * 3
    print(f"gating combinations: {gating} (class x tier S/M/L x goal); "
          "tier C shown, never counted")

# ----------------------------------------------------------------- report

SPEC_ROWS = [("CIF", "S"), ("720p", "S"), ("720p", "L"), ("720p", "C"),
             ("1080p", "S"), ("1080p", "M"), ("1080p", "L"), ("1080p", "C"),
             ("2160p", "S"), ("2160p", "M"), ("2160p", "L"), ("2160p", "C")]
GATING_TIERS = ("S", "M", "L")
GOALS = ("G1", "G2", "G3")
MED_BAR, WORST_BAR = ps.BAR_MEDIAN, ps.BAR_WORST
VMAF_BAR, VMAF_FLOOR, PSNR_FLOOR = (ps.BAR_DVMAF, ps.BAR_DVMAF_CELL,
                                    ps.BAR_DPSNR)


def num(r, k):
    return ps.fnum((r or {}).get(k))


def med(v):
    return statistics.median(v)


def group_stats(rows):
    ok = [r for r in rows if num(r, "ratio") is not None
          and r["valid"] in ("yes", "single-run")
          and "FAIL:" not in r.get("note", "")]
    if not ok:
        return None
    rs = sorted(ok, key=lambda r: num(r, "ratio"))
    return dict(med=med([num(r, "ratio") for r in rs]),
                worst=num(rs[-1], "ratio"),
                worst_cell=cname(rs[-1]['title'], rs[-1]['class']),
                single=any(r["valid"] == "single-run" for r in ok), n=len(ok))


def parity_lines(combos, qok, g):
    """The full-parity lines (yah264's rule): yes only when every gating
    combination is measured and passing with its quality holding, every
    gated curve has landed inside the BD bar, every VBR rung tracks, every
    VBR wall cell is measured inside the speed bar, and the sweep has
    landed. Tier C speed is its own line, never counted; tier C quality
    counts."""
    gate = [x for x in combos if x[1] in GATING_TIERS]
    measured = [x for x in gate if x[3] in ("pass", "open")]
    passing = [x for x in measured if x[3] == "pass" and qok.get((x[0], x[1]))]
    unmeasured = [f"{c}-{t} {gl}" + (" (void)" if st == "void" else "")
                  for c, t, gl, st in gate if st in ("none", "void")]
    why = []
    if len(passing) < len(measured):
        why.append(f"{len(measured) - len(passing)} speed combination(s) open")
    if unmeasured:
        why.append(f"{len(unmeasured)} speed combination(s) not measured")
    if g["bd_missing"]:
        why.append(f"{len(g['bd_missing'])} ladder curve(s) missing")
    if g["bd_fail"]:
        why.append(f"{len(g['bd_fail'])} ladder curve(s) over the BD bar")
    if g["track_fail"]:
        why.append(f"{len(g['track_fail'])} VBR cell(s) outside "
                   f"+-{TRACK_BAR:g}% tracking")
    if g["vbr_missing"]:
        why.append(f"{len(g['vbr_missing'])} VBR wall cell(s) missing")
    if g["vbr_fail"]:
        why.append(f"{len(g['vbr_fail'])} VBR wall class(es) over the speed "
                   "bar")
    if g["sweep_pending"]:
        why.append(f"lookahead sweep unfinished on "
                   f"{len(g['sweep_pending'])} cell-mode(s)")
    cq = [f"{c}-{t}" for (c, t), ok in qok.items() if t == "C" and ok is False]
    if cq:
        why.append(f"tier C quality open in {', '.join(cq)}")
    out = [f"Full parity:           {'no: ' + '; '.join(why) if why else 'yes'}"
           f". Speed: {len(passing)} of {len(gate)} gating combinations pass "
           f"(class, tier S/M/L and goal; pass = median <= {MED_BAR:.2f}, "
           f"worst <= {WORST_BAR:.2f} and quality holding). Ladders: "
           f"{g['bd_landed'] - len(g['bd_fail'])} of {g['bd_total']} curves "
           f"landed inside the BD bar. VBR wall: "
           f"{g['vbr_total'] - len(g['vbr_missing'])} of {g['vbr_total']} "
           "cells measured."]
    if unmeasured:
        out.append(f"Not measured (gating): {', '.join(unmeasured)}")
    for key, lab in (("bd_missing", "Ladders missing:      "),
                     ("bd_fail", "Ladders over the bar: "),
                     ("track_fail", "Tracking outside bar: "),
                     ("vbr_missing", "VBR wall missing:     "),
                     ("vbr_fail", "VBR wall over the bar:")):
        if g.get(key):
            out.append(f"{lab} {', '.join(g[key])}")
    tc = [x for x in combos if x[1] not in GATING_TIERS]
    if tc:
        okc = [f"{c}-{t} {gl}" for c, t, gl, st in tc if st == "pass"]
        opc = [f"{c}-{t} {gl}" for c, t, gl, st in tc if st == "open"]
        gap = [f"{c}-{t} {gl}" + (" (void)" if st == "void" else
                                  " (not measured)")
               for c, t, gl, st in tc if st in ("none", "void")]
        out.append("Tier C speed, not gating: " + "; ".join(p for p in (
            ("inside the bar: " + ", ".join(okc)) if okc else "",
            ("over the bar: " + ", ".join(opc)) if opc else "",
            ("not measured / void: " + ", ".join(gap)) if gap else "") if p))
    if g["sweep_pending"]:
        out.append("Sweep incomplete:      " + ", ".join(g["sweep_pending"]))
    return out


def vbr_legs():
    legs = {}
    for r in ps.read_csv(VBRW_PATH):
        legs[(r["tier"], r["class"], r["title"])] = r
    return legs


def gate_state(cells):
    g = {"bd_total": 0, "bd_landed": 0, "bd_fail": [], "bd_missing": [],
         "track_fail": [], "vbr_total": 0, "vbr_missing": [], "vbr_fail": [],
         "sweep_pending": []}
    lad = jload(LAD_PATH)
    legs = vbr_legs()
    by = {}
    for c in cells:
        if c["tier"] not in ("M", "L"):
            continue
        for mode in ("crf", "vbr"):
            name = f"{c['name']} ({c['tier']}) {mode.upper()}"
            g["bd_total"] += 1
            cv = curve(c, mode, OUR_DEPTH, lad)
            if not cv:
                g["bd_missing"].append(name)
                continue
            g["bd_landed"] += 1
            if cv["bn"] > BD_BAR:
                g["bd_fail"].append(f"{name} {cv['bn']:+.2f}%")
            if mode == "vbr" and any(abs(e) > TRACK_BAR
                                     for e in cv["track_o"]):
                g["track_fail"].append(name)
        g["vbr_total"] += 1
        v = legs.get((c["tier"], c["cls"], c["title"]))
        if v is None:
            g["vbr_missing"].append(f"{c['name']} ({c['tier']})")
        elif v["feeder_bound"] == "yes":
            g["vbr_missing"].append(f"{c['name']} ({c['tier']}) "
                                    "(feeder-bound)")
        else:
            by.setdefault((c["cls"], c["tier"]), []).append(float(v["ratio"]))
            if abs(float(v["ours_err_pct"])) > TRACK_BAR:
                g["track_fail"].append(f"{c['name']} ({c['tier']}) VBR wall "
                                       "leg")
        if c["key"] in SWEEP_CELLS:
            for mode in ("crf", "vbr"):
                miss = [str(d) for d in SWEEP_DEPTHS
                        if not curve(c, mode, d, lad)]
                if miss:
                    g["sweep_pending"].append(
                        f"{c['name']} ({c['tier']}) {mode.upper()} depth "
                        + "/".join(miss))
    for (cls, tier), r in sorted(by.items()):
        if med(r) > MED_BAR or max(r) > WORST_BAR:
            g["vbr_fail"].append(f"{cls}-{tier} {med(r):.2f} / {max(r):.2f}")
    return g


def selftest_verdict():
    """yah264's check of the full-parity lines, on this module's rule."""
    full = {"bd_total": 2, "bd_landed": 2, "bd_fail": [], "bd_missing": [],
            "track_fail": [], "vbr_total": 1, "vbr_missing": [],
            "vbr_fail": [], "sweep_pending": []}
    qok = {("1080p", "S"): True, ("2160p", "C"): True}
    combos = [("1080p", "S", g_, "pass") for g_ in GOALS] + [
        ("2160p", "C", "G1", "none"), ("2160p", "C", "G2", "none"),
        ("2160p", "C", "G3", "void")]
    out = parity_lines(combos, qok, full)
    assert out[0].startswith("Full parity:           yes"), out[0]
    assert "3 of 3 gating combinations" in out[0], out[0]
    assert any(ln.startswith("Tier C speed, not gating")
               and "2160p-C G3 (void)" in ln for ln in out), out
    for key, val in (("bd_missing", ["x CRF"]), ("bd_fail", ["x VBR +1.01%"]),
                     ("track_fail", ["x VBR"]), ("vbr_missing", ["x"]),
                     ("vbr_fail", ["1080p-L 1.02 / 1.20"])):
        o2 = parity_lines(combos, qok, dict(full, **{key: val}))
        assert o2[0].startswith("Full parity:           no"), (key, o2[0])
    o3 = parity_lines(combos, qok, dict(full, sweep_pending=["x VBR 30"]))
    assert "sweep unfinished on 1" in o3[0] and o3[-1].startswith("Sweep"), o3
    o4 = parity_lines(combos[:2] + [("1080p", "S", "G3", "none")] + combos[3:],
                      qok, full)
    assert "Not measured (gating): 1080p-S G3" in o4[1], o4
    o5 = parity_lines(combos, {("1080p", "S"): False}, full)
    assert o5[0].startswith("Full parity:           no"), o5[0]
    o6 = parity_lines(combos, {**qok, ("2160p", "C"): False}, full)
    assert "tier C quality open in 2160p-C" in o6[0], o6[0]


def selftest():
    """No media read: the tables, the windows, the stride rule and the
    verdict lines."""
    cs = all_cells() if not SMOKE else []
    lc = [c for c in cs if c["tier"] == "L"]
    assert len(lc) == 20, len(lc)
    assert len([c for c in cs if c["tier"] == "C"]) == 9
    assert {c["cls"] for c in cs if c["tier"] == "C"} == {"720p", "1080p",
                                                         "2160p"}
    gating = sum(1 for _, t in SPEC_ROWS if t in GATING_TIERS) * 3
    assert gating == 27, gating
    want = {"tears": (1440, 12672), "meridian": (1798, 32887),
            "stem2": (5640, 12940), "sintel": (4680, 13178),
            "bbb": (900, 13929), "cosmos": (720, 13724),
            "nocturne": (3000, 34320), "lash1924": (960, 14400)}
    for t, v in want.items():
        assert excerpt_span(t) == v, (t, excerpt_span(t), v)
        if TITLES[t]["last"] is not None:
            assert credits_span(t)[0] == TITLES[t]["last"] + 1, t
    assert full_geom("tears", "720p") == (1280, 572)
    assert full_geom("tears", "1080p") == (1920, 858)
    assert full_geom("sintel", "720p") == (1280, 546)
    assert full_geom("sintel", "1080p") == (1920, 818)
    assert full_geom("cosmos", "1080p") == (1920, 804)
    assert full_geom("cosmos", "720p") == (1280, 536)
    assert full_geom("lash1924", "720p") == (1280, 960)
    assert full_geom("lash1924", "1080p") == (1440, 1080)
    assert full_geom("sintel", "2160p") == (4096, 1744)
    # a 528 s cell: three windows of 720 frames, centered in their strata
    assert windows(12672, 24.0) == [(1752, 720), (5976, 720), (10200, 720)]
    assert len(windows(2760, 24000 / 1001)) == 1        # tier M: one
    for s in (1, 3, 7):
        assert all(math.gcd(s, p) == 1 for p in GOP_PERIODS)
    for s in (2, 4, 5):
        assert any(math.gcd(s, p) != 1 for p in GOP_PERIODS)
    b, _, _ = secant(lambda q: int(1e6 * math.exp(-0.12 * (q - 28))), 1.3e6,
                     28)
    assert b[2] < 0.005, b
    pts = [{"psnr_y": 40.0, "bytes": 1000}, {"psnr_y": 42.0, "bytes": 2000}]
    v, e = size_at(pts, 41.0)
    assert abs(v - math.sqrt(2) * 1000) < 1e-6 and not e
    assert size_at(pts, 44.0)[1]
    selftest_verdict()
    print("parity_long selftest: ok")


def latest_rows(upto):
    """The newest spec row per (tier, class, title, goal) on or before
    `upto`; tier M titles repeat across classes, so the class is in the
    key."""
    out = {}
    for r in ps.read_csv(ps.CSV_PATH):
        if r["date"] <= upto:
            out[(r["tier"], r["class"], r["title"], r["goal"])] = r
    return out


def report_cells():
    out = []
    for c in ps.CELLS:
        out.append(_mk(c[0], c[1], c[2], c[1], kind="spec", spec_cell=c))
    return out + [c for c in all_cells() if c["tier"] != "M"]


def quality_points():
    pts = ps.load_points()
    if SMOKE:
        pts = jload(POINTS)
    return {k: p for k, p in pts.items() if p.get("anchor") == "def"}


def write_full_report(date):
    cells = report_cells()
    cur = latest_rows(date)
    pts = quality_points()
    rows = []
    for c in cells:
        pt = pts.get(f"{c['key']}/def")
        for g in GOALS:
            r = cur.get((c["tier"], c["cls"], c["title"], g))
            if r is None and g == "G3" and pt:
                # solved and scored but not timed: quality only
                r = {k: "" for k in ps.COLUMNS}
                r.update(codec="yah265", date=date, tier=c["tier"],
                         title=c["title"], size=pt["size"], goal="G3",
                         ours_bytes=pt["ours_bytes"], ref_bytes=pt["ref_bytes"],
                         dsize_pct=ps.f2(pt["dsize_pct"]), valid="yes",
                         note="not timed yet", **{"class": c["cls"]})
            if r is None:
                continue
            r = dict(r)
            # a point scored on the gate metric outranks a timed row's
            # score, which may predate the metric switch
            fresh = (pt and pt.get("ours_vmafneg") is not None
                     and pt.get("ref_vmafneg") is not None)
            if g == "G3" and fresh and (pt.get("metric") == GATE
                                        or not r.get("dvmafneg")):
                r.update(ours_vmafneg=ps.f2(pt["ours_vmafneg"]),
                         ref_vmafneg=ps.f2(pt["ref_vmafneg"]),
                         dvmafneg=ps.f2(pt["ours_vmafneg"] - pt["ref_vmafneg"]),
                         ours_psnr_y=ps.f2(pt["ours_psnr_y"]),
                         ref_psnr_y=ps.f2(pt["ref_psnr_y"]),
                         dpsnr_y=ps.f2(pt["ours_psnr_y"] - pt["ref_psnr_y"]))
            if g == "G3" and c["tier"] in ("M", "L"):
                cv = curve(c, "crf")
                if cv:
                    r["bd_vmafneg_pct"] = f"{cv['bn']:+.2f}"
            r["_cell"] = c
            rows.append(r)
    L = []
    cm = sorted({r["commit"] for r in rows if r.get("commit")}) or [ps.commit()]
    L.append(f"# yah265 parity report, {date}\n")
    L.append(f"Encoder: yah265 n265 {', '.join(cm)}, preset medium.  "
             f"Reference: x265 {ps.x265_version()}, preset medium.")
    runs = [r for r in ps.read_csv(ps.RUNS_PATH) if r["date"] <= date]
    meas = (f"timed legs {runs[0]['date']} {runs[0]['time'][:5]} to "
            f"{runs[-1]['date']} {runs[-1]['time'][:5]}" if runs else "-")
    L.append(f"Machine: {ps.machine()}.  Measured: {meas}.")
    L.append(f"Reference binaries: SIMD and pure C are one binary, "
             f"`{ps.X265}` = x265 {ps.x265_version()} (Homebrew); pure C = "
             "`--no-asm` (G1, G2). Ours: pure C = the same binary with "
             "N265_ASM_OFF=all.")
    L.append("")
    L.append("How to read: a speed ratio is our time / the reference's time "
             "at the same file")
    L.append("size; below 1.00 we're faster. Pass = median <= 1.00 and worst "
             "<= 1.15, quality")
    L.append(f'no worse than -0.5 {MNAME}. "-" = not measured yet.')
    L.append('A dagger (†) marks a figure that includes a cell timed in one '
             'run only; "void"')
    L.append("means every cell there was measured but failed a validity rule "
             "(see the last")
    L.append("section). Quality holds for a class and tier when the median "
             f"d{MNAME} is")
    L.append(f"at least -0.5, no cell is more than 1.0 {MNAME} or 1.0 dB "
             "PSNR-Y behind,")
    L.append("and every cell's size is within 1%. Both encoders read the same "
             "8-bit")
    L.append("4:2:0 Y4M frames on stdin. Lookahead: ours 20 at 720p and up, "
             "12 at CIF / x265 20 (each medium's own) unless a row says "
             "otherwise.")
    L.append("")

    def grp(cls, tier, goal=None):
        return [r for r in rows if r["class"] == cls and r["tier"] == tier
                and (goal is None or r["goal"] == goal)]

    verdict_goal, speed_lines = {}, []
    for cls, tier in SPEC_ROWS:
        txt, states, opens = [], {}, []
        for g in GOALS:
            rs = [r for r in grp(cls, tier, g) if num(r, "ratio") is not None]
            st = group_stats(rs)
            if st:
                txt.append(f"{st['med']:.2f} / {st['worst']:.2f}"
                           + (" †" if st["single"] else ""))
                ok = st["med"] <= MED_BAR and st["worst"] <= WORST_BAR
                states[g] = "pass" if ok else "open"
                if not ok:
                    opens.append((st["worst"], g, st["worst_cell"]))
            elif rs:
                txt.append("void")
                states[g] = "void"
            else:
                txt.append("-")
                states[g] = "none"
            verdict_goal.setdefault(g, []).append((cls, tier, states[g], st))
        if opens:
            w = max(opens)
            v = f"OPEN (worst {w[2]} {w[0]:.2f}, {w[1]})"
        elif all(states[g] == "pass" for g in GOALS):
            v = "PASS"
        elif any(states[g] == "pass" for g in GOALS):
            v = ("incomplete: " + ", ".join(g for g in GOALS
                                            if states[g] != "pass")
                 + " not measured or void")
        else:
            v = "-"
        if tier == "C":
            v += " (not gating)"
        speed_lines.append(f"| {cls} | {tier} | " + " | ".join(txt)
                           + f" | {v} |")

    qual_lines, qual_state = [], []
    for cls, tier in SPEC_ROWS:
        rs = [r for r in grp(cls, tier, "G3") if num(r, "dvmafneg") is not None
              and r["valid"] != "frame-mismatch"]
        if not rs:
            qual_lines.append(f"| {cls} | {tier} | - | - | - | - |")
            qual_state.append((cls, tier, "none", None))
            continue
        dv = sorted(rs, key=lambda r: num(r, "dvmafneg"))
        dp = sorted(rs, key=lambda r: num(r, "dpsnr_y"))
        mv = med([num(r, "dvmafneg") for r in rs])
        worst = cname(dv[0]['title'], cls)
        why = []
        if mv < VMAF_BAR:
            why.append(f"median {mv:+.2f}")
        floor = [r for r in rs if num(r, "dvmafneg") < VMAF_FLOOR
                 or num(r, "dpsnr_y") < PSNR_FLOOR]
        if floor:
            why.append("floor: " + ", ".join(
                f"{cname(r['title'], cls)} {num(r, 'dvmafneg'):+.2f} VMAF / "
                f"{num(r, 'dpsnr_y'):+.2f} dB" for r in floor))
            worst = cname(floor[0]['title'], cls)
        off = [r for r in rs if num(r, "dsize_pct") is not None
               and abs(num(r, "dsize_pct")) > ps.TOL]
        if off:
            why.append("size: " + ", ".join(
                f"{cname(r['title'], cls)} {num(r, 'dsize_pct'):+.2f}%"
                for r in off))
        n_all = sum(1 for c in cells if c["tier"] == tier and c["cls"] == cls)
        part = "" if len(rs) == n_all else f" ({len(rs)} of {n_all} cells)"
        if part:
            why.append("cells unsolved" + part)
        qual_state.append((cls, tier, "pass" if not why else "open",
                           "; ".join(why) or worst))
        qual_lines.append(f"| {cls} | {tier} | {mv:+.2f} / "
                          f"{num(dv[0], 'dvmafneg'):+.2f} | "
                          f"{num(dp[0], 'dpsnr_y'):+.2f} | {worst} | "
                          + (f"OPEN ({'; '.join(why)})" if why else "PASS")
                          + " |")

    def vline(states):
        p = [f"{c}-{t}" for c, t, s, _ in states if s == "pass"]
        o = [f"{c}-{t} ({st['worst_cell']} {st['worst']:.2f})"
             if isinstance(st, dict) else f"{c}-{t} ({st})"
             for c, t, s, st in states if s == "open"]
        vd = [f"{c}-{t}" for c, t, s, _ in states if s == "void"]
        nm = [f"{c}-{t}" for c, t, s, _ in states if s == "none"]
        parts = [x for x in (("PASS in " + ", ".join(p)) if p else "",
                             ("OPEN in " + ", ".join(o)) if o else "",
                             ("void in " + ", ".join(vd)) if vd else "",
                             ("not measured in " + ", ".join(nm)) if nm
                             else "") if x]
        return "; ".join(parts) or "-"

    qok = {(c, t): (st == "pass" if st != "none" else None)
           for c, t, st, _ in qual_state}
    combos = [(c, t, g, st) for g in GOALS for c, t, st, _ in verdict_goal[g]]
    gs = gate_state(cells)
    L.append("## Verdict")
    L.append(f"G1 pure C, 1 thread:   {vline(verdict_goal['G1'])}")
    L.append(f"G2 pure C, all cores:  {vline(verdict_goal['G2'])}")
    L.append(f"G3 SIMD, all cores:    {vline(verdict_goal['G3'])}")
    L.append(f"Quality:               {vline(qual_state)}")
    L += parity_lines(combos, qok, gs)
    L.append("What this rests on: the rows of docs/data/parity-spec.csv up to "
             "this date, the points in docs/data/parity-spec-points.json, the "
             "window matches, ladders and VBR walls beside them. Tier C is "
             "outside the speed count: its speed is shown and does not gate.")
    L.append("")
    L.append("## Speed (median / worst ratio, per class and tier)")
    L.append("| class | tier | G1 | G2 | G3 | verdict |")
    L.append("|---|---|---|---|---|---|")
    L += speed_lines
    L.append("")
    L.append(f"## Quality at matched size ({MNAME} delta median / worst; "
             "PSNR-Y worst)")
    L.append("| class | tier | dVMAF | dPSNR-Y | worst cell | verdict |")
    L.append("|---|---|---|---|---|---|")
    L += qual_lines
    soft = sorted((num(r, "dvmafneg"), cname(r['title'], r['class']),
                   r["tier"]) for r in rows if r["goal"] == "G3"
                  and num(r, "dvmafneg") is not None
                  and VMAF_FLOOR <= num(r, "dvmafneg") < VMAF_BAR)
    L.append("")
    L.append("Soft cells (within the floor, below -0.5): " + (", ".join(
        f"{c} {d:+.2f} ({t})" for d, c, t in soft) if soft else "none") + ".")
    fl = []
    for c in cells:
        if c["kind"] == "spec":
            t = ps.traits_of(c["spec_cell"])
        elif c["kind"] == "excerpt":
            t = mt.flags(f"{c['title']}_{c['size']}")
        else:
            t = ""
        if t and t != "?":
            fl.append(f"{c['name']} ({c['tier']}) [{t}]")
    if fl:
        L.append("Cells whose media carries a trait that can steer the "
                 "reading (docs/media-traits.md): " + "; ".join(fl) + ".")

    # tier L end frames
    L.append("")
    L.append("## Tier L end frames (end credits excluded)")
    L.append("A cell runs from its start frame to the last story frame "
             "(excerpt-relative, the same at every size).")
    for c in cells:
        if c["tier"] != "L":
            continue
        last = TITLES[c["title"]]["last"]
        bits = []
        for g in GOALS:
            r = cur.get(("L", c["cls"], c["title"], g))
            bits.append(f"{g} " + (f"{num(r, 'ratio'):.2f}"
                                   if num(r, "ratio") is not None else "-")
                        + (f" [{r['valid']}]" if r and r["valid"] != "yes"
                           else ""))
        L.append(f"- {c['name']} (end frame {last}): " + "; ".join(bits))

    # windows
    wins = jload(WIN_PATH)
    wl = []
    for c in cells:
        if c["kind"] == "spec" and c["tier"] != "M":
            continue
        cc = c if c["kind"] != "spec" else _mk("M", c["cls"], c["title"],
                                               c["cls"])
        if not (cc["tier"] in ("M", "L") or cc.get("kind") == "excerpt"):
            continue
        parts = []
        for g in (1, 2):
            w = wins.get(f"{cc['key']}/G{g}")
            r = cur.get((cc["tier"], cc["cls"], cc["title"], f"G{g}"))
            timed = num(r, "dsize_pct") if r and "window" in r.get(
                "note", "") else None
            if w is None and timed is None:
                continue
            parts.append(f"G{g} " + (f"{timed:+.2f}%" if timed is not None
                                     else f"{w['dsize_pct']:+.2f}% (untimed)")
                         + (f" ({w['before_dsize_pct']:+.2f}% before)" if w
                            else " (not matched)")
                         + (f" **VOID: over {WIN_VOID:g}%**"
                            if timed is not None and abs(timed) > WIN_VOID
                            else ""))
        if parts:
            wl.append(f"- {cc['name']} ({cc['tier']}): " + ", ".join(parts))
    L.append("")
    L.append("## G1/G2 window bytes (2-minute and 10-minute cells)")
    L.append(f"G1 and G2 time {WIN_SECS:g}-second windows, one per "
             f"{STRATUM_SECS:g} s, each coded as its own stream. Summed "
             "window size, ours against the reference, in the timed legs (G1 "
             "one thread, G2 default threads); in brackets the same figure at "
             "the whole cell's rate factor, before ours was matched on the "
             f"windows. A goal whose windows differ by more than {WIN_VOID:g}%"
             " is void for that cell.")
    L += wl or ["- none matched yet."]

    # ladders
    lad = jload(LAD_PATH)
    bl, tl, pend, fails, near, landed = [], [], [], [], [], 0
    for c in cells:
        if c["tier"] not in ("M", "L"):
            continue
        cc = c if c["kind"] != "spec" else _mk("M", c["cls"], c["title"],
                                               c["cls"])
        parts = []
        for mode in ("crf", "vbr"):
            M_ = mode.upper()
            cv = curve(cc, mode, OUR_DEPTH, lad)
            if not cv:
                n_ = ladder_rows_present(cc, mode)
                parts.append(f"{M_} pending" + (f" ({n_} of 5 rungs of ours)"
                                                 if n_ else ""))
                pend.append(f"{cc['name']} ({cc['tier']}) {M_}")
                continue
            landed += 1
            over = cv["bn"] > BD_BAR
            if over:
                fails.append(f"{cc['name']} {M_} ({cv['bn']:+.2f}%)")
            elif 0.5 <= cv["bn"] <= BD_BAR:
                near.append(f"{cc['name']} {M_} {cv['bn']:+.2f}%")
            how = "every frame" if cv["stride"] == 1 else \
                f"every {cv['stride']} frames"
            parts.append(f"{M_} BD-{MNAME} {cv['bn']:+.2f}% (PSNR-Y "
                         f"{cv['bp']:+.2f}%; {cv['n']} rungs, {how})"
                         + ("  **OVER the +1.0% bar**" if over else "")
                         + ("" if cv["frames_ok"] else "  FRAME-MISMATCH"))
            if mode == "vbr":
                bad = [e for e in cv["track_o"] if abs(e) > TRACK_BAR]
                tl.append(f"- {cc['name']} ({cc['tier']}): " + ", ".join(
                    f"{q} kbit/s ours {eo:+.1f}% / reference {ex:+.1f}%"
                    for q, eo, ex in zip(cv["rungs"], cv["track_o"],
                                         cv["track_x"]))
                    + (f"  **{len(bad)} rung(s) over +-{TRACK_BAR:g}%**"
                       if bad else ""))
        bl.append(f"- {cc['name']} ({cc['tier']}): " + "; ".join(parts))
    L.append("")
    L.append(f"## BD-rate from the ladders (bar: BD-{MNAME} <= "
             f"+{BD_BAR:.1f}%)")
    L.append(f"{landed} of {landed + len(pend)} cell-modes have a landed "
             f"curve; {landed - len(fails)} of those {landed} pass"
             + (f"; failing: {', '.join(fails)}" if fails else "")
             + f"; {len(pend)} pending. Within 0.5 points of the bar: "
             + (", ".join(near) if near else "none") + ".")
    L.append(f"Five rungs (CRF {', '.join(map(str, RUNGS))}; VBR at x265's "
             f"achieved rates there, maxrate {MAXR:g}x, buffer {BUFR:g}x), both "
             f"encoders at {THREADS} threads (x265 --pools {THREADS} "
             f"--frame-threads 4), lookahead ours {OUR_DEPTH} / x265 "
             f"{REF_DEPTH}, negative = ours needs fewer bits. Only curves of "
             f"this binary count. {MNAME} on every frame on tier M, every 3rd "
             "on tier L (an odd stride, coprime with both mini-GOP periods).")
    L += bl
    if tl:
        L.append("")
        L.append(f"### VBR rate tracking per rung (bar: within "
                 f"+-{TRACK_BAR:g}% of the target over the whole cell)")
        L.append("Achieved bitrate against the rung's target, on the curve "
                 "counted above.")
        L += tl
    if pend:
        L.append("")
        L.append("### Pending on this binary")
        L.append("; ".join(pend) + ".")

    # sweep
    L.append("")
    L.append(f"## Lookahead depth sweep (BD-{MNAME} against the reference)")
    L.append(f"Ours at --rc-lookahead {', '.join(map(str, SWEEP_DEPTHS))} "
             f"({OUR_DEPTH} is medium's own, the depth every other figure "
             f"uses); x265 at its default {REF_DEPTH} in every column. Same "
             f"rungs, {THREADS} threads; (sN) = scored every Nth frame. "
             "\"best\" names a depth only when the three differ by more than "
             f"{SWEEP_SPREAD:g} points.")
    L.append("| cell | mode | " + " | ".join(
        f"ours {d} / x265 {REF_DEPTH}" for d in SWEEP_DEPTHS) + " | best |")
    L.append("|---|---|" + "---|" * len(SWEEP_DEPTHS) + "---|")
    for mode in ("crf", "vbr"):
        for k in SWEEP_CELLS:
            tier, cls, title = k.split("/")
            cc = _mk(tier, cls, title, cls)
            got, txt = {}, []
            for d in SWEEP_DEPTHS:
                cv = curve(cc, mode, d, lad)
                if cv:
                    got[d] = cv["bn"]
                    txt.append(f"{cv['bn']:+.2f}%" + (
                        f" (s{cv['stride']})" if cv["stride"] != 1 else ""))
                else:
                    txt.append("pending")
            best = "-"
            if len(got) == len(SWEEP_DEPTHS):
                best = (str(min(got, key=got.get))
                        if max(got.values()) - min(got.values()) > SWEEP_SPREAD
                        else "within spread")
            L.append(f"| {cname(title, cls)} ({tier}) | {mode.upper()} | "
                     + " | ".join(txt) + f" | {best} |")

    # VBR wall
    legs = vbr_legs()
    vr, ratios, miss = [], {}, []
    for c in cells:
        if c["tier"] not in ("M", "L"):
            continue
        v = legs.get((c["tier"], c["cls"], c["title"]))
        nm = c['name']
        if not v:
            vr.append(f"| {nm} | {c['tier']} | - | not measured | - | - | - "
                      "| - |")
            miss.append(nm)
            continue
        fx = float(v["feed_fps"])
        flag = " feeder-bound" if v["feeder_bound"] == "yes" else ""
        if abs(float(v["ours_err_pct"])) > TRACK_BAR:
            flag += f" **over +-{TRACK_BAR:g}%**"
        vr.append(f"| {nm} | {c['tier']} | {v['k']} | {float(v['ratio']):.2f} "
                  f"| {float(v['ours_kbps']):.0f} ({float(v['ours_err_pct']):+.1f}%) "
                  f"| {float(v['ref_kbps']):.0f} ({float(v['ref_err_pct']):+.1f}%) "
                  f"| {fx:.0f} fps | {v['date']} {v['time']}{flag} |")
        if not flag.startswith(" feeder"):
            ratios.setdefault((c["cls"], c["tier"]), []).append(
                float(v["ratio"]))
    L.append("")
    L.append("## VBR wall (G3 only: SIMD, default threads, one rung per cell)")
    L.append(f"One rung per M and L cell: x265's CRF {REF_CRF} achieved rate "
             f"as the target, maxrate {MAXR:g}x, buffer {BUFR:g}x, lookahead "
             f"ours {OUR_DEPTH} / x265 {REF_DEPTH}, {VBR_ROUNDS} interleaved "
             "rounds, no size match. Tracking is the achieved bitrate against "
             "the target in the timed leg.")
    L.append("Median / worst per class and tier: " + ("; ".join(
        f"{c_}-{t_} {med(r_):.2f} / {max(r_):.2f} ({len(r_)} cells)"
        for (c_, t_), r_ in sorted(ratios.items())) or "none measured") + ".")
    L.append("| cell | tier | target kbit/s | ratio | ours kbit/s | "
             "reference kbit/s | feed | measured |")
    L.append("|---|---|---|---|---|---|---|---|")
    L += vr

    # credits
    L.append("")
    L.append("## End credits (supplementary rows, not a gate)")
    L.append("Every tier L cell stops at its last story frame. The credits "
             "that follow are coded on their own, at 1080p: x265 at CRF "
             f"{REF_CRF}, ours at the cell's matched crf, five rungs each "
             "side for the size at equal PSNR-Y (PSNR-Y on every frame; no "
             "VMAF-NEG BD on credits). \"extr\" = extrapolated beyond the "
             "measured points.")
    L.append("| title | span (excerpt frames) | x265 CRF 28 bytes / Y | ours "
             "(matched crf) bytes / Y | ours size at x265's Y | ours size vs "
             "x265 at ours' Y | reading |")
    L.append("|---|---|---|---|---|---|---|")
    for t, sp, d in credits_rows():
        if d is None:
            L.append(f"| {t} | {sp[0]}-{sp[1]} | pending | | | | |")
            continue
        a, ea = d["at_ref"]
        b, eb = d["at_ours"]
        if (a > 0) != (b > 0) and ea and eb:
            rd = "no overlap; figure extrapolated"
        elif a > 1.0 or b > 1.0:
            rd = "**behind**"
        else:
            rd = "ahead"
        L.append(f"| {t} | {sp[0]}-{sp[1]} | {d['x']['bytes'] / 1e6:.2f} MB "
                 f"/ {d['x']['psnr_y']:.2f} | ({ps._fmt_q(d['crf'])}) "
                 f"{d['o']['bytes'] / 1e6:.2f} MB / {d['o']['psnr_y']:.2f} | "
                 f"{a:+.1f}%{' extr' if ea else ''} | "
                 f"{b:+.1f}%{' extr' if eb else ''} | {rd} |")

    titles = sorted({c["title"] for c in cells if c["tier"] in ("L", "C")})
    cc_ = [f"{t} {CONTENT_CLASS[t]}" for t in titles if t in CONTENT_CLASS]
    L.append("")
    L.append("## Dominant content class per title (yah264's scene scan of "
             "2026-09-26)")
    L.append("; ".join(cc_) + ". Tiers S and M and the tier C sports and "
             "screen clips were not scanned.")
    notes = os.environ.get("PARITY_NOTES")
    if notes and os.path.exists(notes):
        L.append("")
        L.append("## Notes from the timing lane")
        L += open(notes).read().strip().splitlines()

    # changed
    L.append("")
    L.append("## Changed since the last report")
    L += changed_lines(rows, date)
    # not valid
    L.append("")
    L.append("## Not valid or not measured")
    L += invalid_lines(rows, cells, cur)

    os.makedirs(ps.REPORT_DIR, exist_ok=True)
    md = os.path.join(ps.REPORT_DIR, f"PARITY-REPORT-yah265-{date}.md")
    cs = os.path.join(ps.REPORT_DIR, f"PARITY-REPORT-yah265-{date}.csv")
    if SMOKE:
        md = os.path.join(ps.WORK, "smoke", os.path.basename(md))
        cs = os.path.join(ps.WORK, "smoke", os.path.basename(cs))
    with open(md, "w") as f:
        f.write("\n".join(L) + "\n")
    with open(cs, "w", newline="") as f:
        w = csv.DictWriter(f, fieldnames=ps.COLUMNS, extrasaction="ignore",
                           lineterminator="\n")
        w.writeheader()
        for r in rows:
            w.writerow({k: r.get(k, "") for k in ps.COLUMNS})
    print(f"report: {md}\n        {cs} ({len(rows)} rows)")


def changed_lines(rows, date):
    d = ps.REPORT_DIR
    prev = sorted(f for f in (os.listdir(d) if os.path.isdir(d) else [])
                  if re.fullmatch(r"PARITY-REPORT-yah265-\d{4}-\d{2}-\d{2}\.csv",
                                  f) and f[21:31] < date)
    if not prev:
        return ["- First report in this layout (yah264's shape, all four "
                "tiers); there is no earlier CSV to compare with."]
    with open(os.path.join(d, prev[-1])) as f:
        old = {(r["tier"], r.get("class", ""), r["title"], r["goal"]): r
               for r in csv.DictReader(f)}
    moved = []
    for r in rows:
        o = old.get((r["tier"], r["class"], r["title"], r["goal"]))
        a, b = num(o, "ratio"), num(r, "ratio")
        if a is not None and b is not None and abs(b - a) >= 0.03:
            moved.append((abs(b - a), f"- {cname(r['title'], r['class'])} "
                          f"{r['goal']} (tier {r['tier']}): {a:.2f} -> "
                          f"{b:.2f}"))
        elif o is None and b is not None:
            moved.append((0, f"- {cname(r['title'], r['class'])} {r['goal']} "
                          f"(tier {r['tier']}): new, {b:.2f}"))
    return [f"Against {prev[-1]}:"] + (
        [m for _, m in sorted(moved, reverse=True)[:5]]
        or ["- no ratio moved by 0.03 or more"])


def invalid_lines(rows, cells, cur):
    out = []
    for r in rows:
        if r["valid"] not in ("yes", "single-run"):
            out.append(f"- {cname(r['title'], r['class'])} {r['goal']} (tier "
                       f"{r['tier']}): {r['valid']}; {r.get('note', '')}")
    one = sorted({f"{cname(r['title'], r['class'])} {r['goal']}" for r in rows
                  if r["valid"] == "single-run"})
    if one:
        out.append("- From one run only: " + ", ".join(one) + ".")
    untimed = sorted({cname(r['title'], r['class']) for r in rows
                      if r.get("note") == "not timed yet"})
    if untimed:
        out.append("- Solved and scored but not timed yet (quality only): "
                   + ", ".join(untimed) + ".")
    pts = quality_points()
    unsolved = [f"{c['name']} ({c['tier']})" for c in cells
                if f"{c['key']}/def" not in pts]
    if unsolved:
        out.append("- No matched point yet: " + ", ".join(unsolved) + ".")
    for tier in ("S", "M", "L", "C"):
        have = {r["goal"] for r in rows if r["tier"] == tier
                and num(r, "ratio") is not None}
        gone = [g for g in GOALS if g not in have]
        if gone:
            out.append(f"- Tier {tier}: {', '.join(gone)} not measured on any "
                       "cell.")
    old_m = [f"{cname(r['title'], r['class'])} {r['goal']}" for r in rows
             if r["tier"] == "M" and r["goal"] in ("G1", "G2")
             and "window" not in r.get("note", "")
             and num(r, "ratio") is not None]
    if old_m:
        out.append("- Tier M G1/G2 read on the whole segment, not on "
                   "windows (before the window rule): " + ", ".join(old_m)
                   + ".")
    out.append("- The tier S clips are timed by parity_spec.py through one "
               "`cat`; tiers L and C and every window through lf_feed.py. "
               "Both are one feeder on stdin for both encoders.")
    return out


def run(args, mode):
    """parity_spec.py's entry: `mode` is the flag that was given."""
    fn = {"solve": do_solve, "time": do_time, "windows": do_windows,
          "time_windows": do_time_windows, "ladder": do_ladder,
          "sweep": do_sweep, "credits": do_credits, "vbr_wall": do_vbr_wall,
          "plan": do_plan}.get(mode)
    if mode == "report":
        return write_full_report(args.date)
    if mode == "selftest_long":
        return selftest()
    if fn is None:
        sys.exit(f"parity_long: no mode {mode}")
    label = f"parity_spec.py --{mode.replace('_', '-')}"
    if mode in ("time", "time_windows", "vbr_wall"):
        with boxlib.timing_lock(label):
            return fn(args)
    if mode != "plan":
        # untimed work never starts inside somebody's timed leg
        boxlib.wait_untimed(label)
    return fn(args)


if __name__ == "__main__":
    sys.exit("parity_long.py runs through scripts/parity_spec.py (see its "
             "--help); `--selftest-long` checks this module")
