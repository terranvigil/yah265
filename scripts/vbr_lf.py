#!/usr/bin/env python3
# Copyright (c) 2026, the yah265 authors
# SPDX-License-Identifier: GPL-2.0-or-later
"""vbr_lf.py - parity plan item 4, the untimed half: VBR against x265 on the
matrix segments, and CRF and VBR on the long-form excerpts.

scripts/abr_bd.py is the corpus version of the VBR question. This is its
sibling for the real titles, where a point is too big to cache as a
bitstream: every point is encoded, measured and deleted, and only its
numbers are kept (a memo, keyed by the full command and the binary).

VBR, the shape (docs/parity-plan.md item 4). Both encoders get the same
--bitrate k and the same buffer, --vbv-maxrate MAXR*k and --vbv-bufsize
BUFR*k (1.5x and 2x by default). The ladder is five rates per segment
cell, x265 medium's own CRF 20/23/26/29/32 bytes from the matrix
(~/media/throughput/points2.json `ladder_targets`) turned into kbit/s over
the segment's duration. BD-VMAF-NEG is taken over the bytes each encoder
actually wrote, not the targets. Lookahead: x265 medium looks 20 frames
ahead, our medium 12; the `ours20` arm adds `--rc-lookahead 20`.

Long-form. A cell is a 600 s excerpt of a master at a recorded offset
(the plan's table), streamed through yah264's lf_feed.py from the shared masters on the T7 (VideoToolbox
decode, scale_vt below native) into the encoder's stdin, and streamed a
second time as the VMAF reference. Nothing is staged. Each cell reads
THREE rungs, not five, for the box's sake: x265 CRF 20/26/32 (the same
span as the matrix's five), our CRF at fixed values 8 below them (the
matrix's matched offset; the BD needs overlap, not matched bytes), and
VBR at x265's three CRF rates on both sides.

Untimed. The box share is six threads: an encoder at six (ours --threads
6; x265 --pools 6 --frame-threads 4, whose frame-thread count is the
default's), then libvmaf at five and a one-thread decode. Ours is byte-
identical at every thread count. x265's VBV output is NOT invariant to its
pool size (300 frames of meridian_720p at 1000k: 406 466 bytes at pools 3,
407 275 at pools 6, repeatable at each), so x265's untimed VBR anchors are
the pools-6 streams and the timed night run's default-pool streams differ
from them by a fraction of a percent in bytes.

Every frame is scored (vmaf-subsample-trap). The VBV reading is a walk
over the access-unit sizes in decode order (ffprobe packets), buffer
starting at 0.9 full and refilled maxrate/fps per picture, the same walk
for both encoders; it is a comparison, not a conformance check.

    python3 scripts/vbr_lf.py seg meridian_720p ...      segment VBR, all arms
    python3 scripts/vbr_lf.py lf tears_1080p ...         long-form CRF and VBR
    python3 scripts/vbr_lf.py queue                      everything, in order
    python3 scripts/vbr_lf.py report                     tables + CSV
"""
import hashlib, json, math, os, shlex, subprocess, sys, tempfile, time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "scripts"))
import bdcompare as bd                                           # noqa: E402

TP = os.path.expanduser("~/media/throughput")
TP_T7 = "<staging>/yah265/throughput"
N265 = os.environ.get("N265", os.path.join(ROOT, "build-rel", "n265"))
X265 = os.environ.get("X265", "x265")
FEED = os.environ.get("LF_FEED", os.path.expanduser(
    "~/src/yah264-board4k/scripts/lf_feed.py"))
# The shared masters (md5-verified copy of the Seagate, 2026-09-26). The
# memo names a master by its basename, so a verified move keeps its points.
MEDIA = os.environ.get("LF_MEDIA", "<media>")
FEED10 = os.path.join(ROOT, "scripts", "lf_feed10.py")
STOPF = os.path.join(os.environ.get("VBRLF_WORK", os.path.join(
    ROOT, "scratch", "vbrlf")), "STOP")
WORK = os.environ.get("VBRLF_WORK", os.path.join(ROOT, "scratch", "vbrlf"))
MEMO = os.path.join(WORK, "results.json")
CSV = os.path.join(ROOT, "docs", "data", "vbr-longform.csv")
MAXR = float(os.environ.get("MAXR", "1.5"))
BUFR = float(os.environ.get("BUFR", "2.0"))
ENC_THREADS = os.environ.get("ENC_THREADS", "6")
VMAF_THREADS = os.environ.get("VMAF_THREADS", "5")
VBV_INIT = 0.9

# ---------------------------------------------------------------- sources

SEG_TITLES = ("meridian", "chimera", "sollevante")
SEG_RES = ("720p", "1080p", "2160p")

# The shared long-form excerpts (enclab spec/PARITY-SPEC.md, tier L): 600 s
# from the offset, start_frame = round(offset x fps). native/fps/t0/frames
# are yah264's longform_throughput.py FULL table, counted there. `cls` is
# the title's dominant content class (detail x motion) from enclab
# research/scene-scan/, whole title.
LF = {
    "tears":    dict(file="longform/tearsofsteel_4k.mov", native=(3840, 1714),
                     fps=(24, 1), t0=0.0, frames=17616, offset=60,
                     cls="detailed/moving 43%"),
    "meridian": dict(file="Meridian_UHD4k5994_HDR_P3PQ.mp4",
                     native=(3840, 2160), fps=(60000, 1001), t0=0.0,
                     frames=43093, offset=30, cls="mid/static 95%"),
    "sintel":   dict(file="Sintel.2010.4k.mkv", native=(4096, 1744),
                     fps=(24, 1), t0=0.0, frames=21312, offset=195,
                     cls="mid/moving 42%"),
    "bbb":      dict(file="bbb_sunflower_2160p_30fps_normal.mp4",
                     native=(3840, 2160), fps=(30, 1), t0=0.066667,
                     frames=19036, offset=30, cls="detailed/moving 40%"),
    "cosmos":   dict(file="cosmos_laundromat_1080p.mp4", native=(1920, 804),
                     fps=(24, 1), t0=0.0, frames=17532, offset=30,
                     cls="mid/moving 68%"),
    # Sintel and StEM2 moved 60 -> 195 and 235 s on 2026-09-26 so each
    # excerpt represents its whole title (class shift 16% / 37% -> 9%).
    # StEM2 reads the one-time VideoToolbox HEVC q90 copy of the ProRes.
    "stem2":    dict(file="derived/stem2_uhd_hevc_q90.mp4", native=(3840, 2160),
                     fps=(24, 1), t0=0.0, frames=25114, offset=235,
                     cls="mid/moving 33%"),
    # Added 2026-09-26 (spec): the grain titles. ProRes 422 10-bit masters,
    # fed through lf_feed10.py (lf_feed.py with a p210 download) and
    # converted to 8-bit 4:2:0 by the feeder. Neither is in the scene scan.
    "nocturne": dict(file="longform/nocturne_filmgrain/"
                          "nocturne_60fps_uhd_filmgrain_sdr.mov",
                     native=(3840, 2160), fps=(60, 1), t0=0.0, frames=39870,
                     offset=50, p210=True, cls="film grain (post), 60p"),
    "lash":     dict(file="longform/lash_of_the_whip_1924/02049825.mov",
                     native=(1440, 1080), fps=(24, 1), t0=0.0, frames=65729,
                     offset=40, p210=True, cls="film scan grain, 1924",
                     sizes=("720p", "1080p")),
}
LF_SECS = 600
X265_LF_CRFS = (20, 26, 32)
OUR_CRF_OFFSET = 8


def even(x):
    return int(x / 2 + 0.5) * 2


def lf_geom(title, res):
    nw, nh = LF[title]["native"]
    if res == "2160p":
        return nw, nh
    w = {"720p": 1280, "1080p": 1920}[res]
    if w >= nw:
        return nw, nh
    return w, even(w * nh / nw)


def lf_span(title):
    f = LF[title]
    num, den = f["fps"]
    start = round(f["offset"] * num / den)
    count = round(LF_SECS * num / den)
    assert start + count <= f["frames"], title
    return start, count


def lf_master(title):
    return os.path.join(MEDIA, LF[title]["file"])


def feed_sig(feed):
    """The feeder command as the memo sees it: masters by basename."""
    return " ".join(os.path.basename(x) if x.startswith("/") and
                    x.endswith((".mov", ".mp4", ".mkv")) else x for x in feed)


def feeder_cmd(title, res):
    f = LF[title]
    w, h = lf_geom(title, res)
    start, count = lf_span(title)
    feeder = FEED10 if f.get("p210") else FEED
    return ["python3", feeder, "vt", "--src", lf_master(title),
            "--native", "%dx%d" % f["native"], "--size", f"{w}x{h}",
            "--fps", "%d/%d" % f["fps"], "--t0", str(f["t0"]),
            "--start", str(start), "--count", str(count), "--workers", "1"]


def seg_cell(cell):
    p2 = json.load(open(os.path.join(TP, "points2.json")))[cell]
    name = f"{p2['title']}_{p2['res']}_{p2['seg']}.y4m"
    # the 720p and 1080p segments moved to the T7 on 2026-09-26 (md5-
    # verified); the 2160p ones stay internal
    src = os.path.join(TP, name)
    if not os.path.exists(src):
        src = os.path.join(TP_T7, name)
    return dict(src=src, frames=p2["frames"], fps=p2["fps"],
                ladder=p2["ladder_targets"], crf_bd=p2["bd_neg_pct"])


def kbps_of(nbytes, frames, fps):
    return nbytes * 8 / (frames / fps) / 1000

# ---------------------------------------------------------------- commands


def enc_cmd(arm, mode, q, src, out):
    """src is a path, or '-' for stdin (y4m)."""
    if arm == "x265":
        c = [X265, "--preset", "medium", "--pools", ENC_THREADS,
             "--frame-threads", "4", "--log-level", "none"]
        if mode == "crf":
            c += ["--crf", str(q)]
        else:
            c += ["--bitrate", str(q), "--vbv-maxrate", str(round(q * MAXR)),
                  "--vbv-bufsize", str(round(q * BUFR))]
        c += ["-o", out] + (["--input", "-", "--y4m"] if src == "-" else [src])
        return c
    c = [N265, src, "--preset", "medium"]
    if mode == "crf":
        c += ["--crf", str(q)]
    else:
        c += ["--bitrate", str(q), "--vbv-maxrate", str(round(q * MAXR)),
              "--vbv-bufsize", str(round(q * BUFR))]
    c += ["--aq", "1", "--cutree", "1"]
    if arm == "ours20":
        c += ["--rc-lookahead", "20"]
    c += ["--threads", ENC_THREADS, "-o", out]
    return c


LOOKAHEAD = {"x265": 20, "ours": 12, "ours20": 20}

_bh = {}


def bin_hash(exe):
    if exe not in _bh:
        p = exe if os.path.sep in exe else subprocess.run(
            ["which", exe], capture_output=True, text=True).stdout.strip()
        h = hashlib.sha256()
        with open(p, "rb") as f:
            for b in iter(lambda: f.read(1 << 20), b""):
                h.update(b)
        _bh[exe] = h.hexdigest()[:16]
    return _bh[exe]

# ---------------------------------------------------------------- memo


def load_memo():
    try:
        return json.load(open(MEMO))
    except (OSError, ValueError):
        return {}


def save_memo(m):
    tmp = MEMO + ".tmp"
    with open(tmp, "w") as f:
        json.dump(m, f, indent=1, sort_keys=True)
    os.replace(tmp, MEMO)

# ---------------------------------------------------------------- measuring


def au_sizes(path):
    r = subprocess.run(["ffprobe", "-v", "error", "-show_entries",
                        "packet=size", "-of", "csv=p=0", path],
                       capture_output=True, text=True)
    return [int(x) for x in r.stdout.split() if x.strip().isdigit()]


def vbv_walk(sizes, fps, kbps):
    """(underflows, min fill fraction, peak 1 s rate in kbit/s)."""
    maxr, buf = kbps * MAXR * 1000, kbps * BUFR * 1000
    fill, under, lo = VBV_INIT * buf, 0, 1.0
    for s in sizes:
        fill -= s * 8
        if fill < 0:
            under += 1
        lo = min(lo, fill / buf)
        fill = min(fill + maxr / fps, buf)
    win = max(1, round(fps))
    run, peak = 0, 0
    for i, s in enumerate(sizes):
        run += s
        if i >= win:
            run -= sizes[i - win]
        if i >= win - 1:
            peak = max(peak, run)
    return under, lo, peak * 8 / (win / fps) / 1000


def score(dist, ref, work, frames, ref_feed=None):
    """VMAF, VMAF-NEG and PSNR-Y over every frame. ref is a y4m path, or
    None with ref_feed a command whose stdout is the reference."""
    d = tempfile.mkdtemp(prefix="sc", dir=work)
    dfifo = os.path.join(d, "dist.y4m")
    os.mkfifo(dfifo)
    js = os.path.join(d, "v.json")
    dec = subprocess.Popen(
        ["ffmpeg", "-v", "error", "-nostdin", "-y", "-threads", "1", "-i", dist,
         "-pix_fmt", "yuv420p", "-sws_dither", "none", "-f", "yuv4mpegpipe",
         dfifo], stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
    fp = None
    if ref is None:
        rfifo = os.path.join(d, "ref.y4m")
        os.mkfifo(rfifo)
        fp = subprocess.Popen(" ".join(shlex.quote(x) for x in ref_feed) +
                              f" > {shlex.quote(rfifo)}", shell=True,
                              stderr=subprocess.PIPE)
        ref = rfifo
    margs = bd._vmaf_models()[0].split()
    r = subprocess.run(["vmaf", "-r", ref, "-d", dfifo, "--json", "-o", js,
                        *margs, "--feature", "psnr", "--threads", VMAF_THREADS],
                       capture_output=True, text=True)
    if r.returncode != 0:
        for pr in (dec, fp):
            if pr and pr.poll() is None:
                pr.kill()
    dec.wait()
    ferr = b""
    if fp:
        ferr = fp.communicate()[1]
        if fp.returncode != 0:
            raise RuntimeError(f"reference feeder failed: {ferr[-400:]}")
    if r.returncode != 0 or dec.returncode != 0:
        raise RuntimeError(f"vmaf rc {r.returncode} dec rc {dec.returncode}: "
                           f"{r.stderr[-400:]} {dec.stderr.read()[-300:]}")
    doc = json.load(open(js))
    pm = doc["pooled_metrics"]
    n = len(doc["frames"])
    for f in (js, dfifo, ref if fp else None):
        if f:
            try:
                os.unlink(f)
            except OSError:
                pass
    os.rmdir(d)
    if n != frames:
        raise RuntimeError(f"scored {n} frames, wanted {frames}")
    return dict(neg=pm["vmaf_neg"]["mean"], vmaf=pm["vmaf"]["mean"],
                psnr_y=pm["psnr_y"]["mean"], scored=n)


def point(memo, key, arm, mode, q, frames, fps, src=None, feed=None,
          target_kbps=None):
    """One encode, measured and deleted. src: a y4m path; feed: a feeder
    command (stdin encode, and the VMAF reference fed again)."""
    out = os.path.join(WORK, "p.hevc")
    cmd = enc_cmd(arm, mode, q, src if src else "-", out)
    sig = " ".join(cmd[1:]) + "|" + bin_hash(cmd[0]) + "|" + \
        (os.path.basename(src) if src else feed_sig(feed))
    env = ";".join(f"{k}={x}" for k, x in sorted(os.environ.items())
                   if k.startswith("N265_"))
    if env:                   # arms selected by env must not share a memo
        sig += "|" + env
    if key in memo and memo[key].get("sig") == sig:
        return memo[key]
    if os.path.exists(STOPF):        # a clean stop between points
        print(f"STOP file: stopping before {key}", flush=True)
        sys.exit(3)
    t = time.time()
    if feed:
        fp = subprocess.Popen(feed, stdout=subprocess.PIPE,
                              stderr=subprocess.PIPE)
        ep = subprocess.run(cmd, stdin=fp.stdout, capture_output=True)
        fp.stdout.close()
        ferr = fp.stderr.read()
        fp.wait()
        if fp.returncode != 0:
            raise RuntimeError(f"{key}: feeder rc {fp.returncode}: "
                               f"{ferr[-400:]}")
    else:
        ep = subprocess.run(cmd, capture_output=True)
    if ep.returncode != 0 or not os.path.exists(out):
        raise RuntimeError(f"{key}: encoder rc {ep.returncode}: "
                           f"{ep.stderr[-600:]}")
    te = time.time() - t
    nbytes = os.path.getsize(out)
    sizes = au_sizes(out)
    if len(sizes) != frames:
        raise RuntimeError(f"{key}: {len(sizes)} access units, wanted {frames}")
    m = score(out, src, WORK, frames, ref_feed=feed)
    os.unlink(out)
    kbps = kbps_of(nbytes, frames, fps)
    r = dict(sig=sig, arm=arm, mode=mode, q=q, bytes=nbytes, kbps=kbps,
             frames=frames, lookahead=LOOKAHEAD[arm], enc_s=round(te, 1),
             all_s=round(time.time() - t, 1), **m)
    if mode == "vbr":
        u, lo, pk = vbv_walk(sizes, fps, q)
        r.update(target_kbps=q, err_pct=100 * (kbps / q - 1), underflows=u,
                 min_fill=lo, peak1s_kbps=pk, maxrate=round(q * MAXR),
                 bufsize=round(q * BUFR))
    memo[key] = r
    save_memo(memo)
    print(f"  {key:<34} {nbytes:>11} B {kbps:9.1f} kb/s NEG {m['neg']:6.2f} "
          f"Y {m['psnr_y']:5.2f}"
          + (f" err {r['err_pct']:+6.2f}% uf {r['underflows']}"
             if mode == "vbr" else "")
          + f"  ({r['all_s']:.0f} s)", flush=True)
    return r

# ---------------------------------------------------------------- cells


def seg_rates(cell):
    c = seg_cell(cell)
    return [round(kbps_of(b, c["frames"], c["fps"])) for b in c["ladder"]]


def run_seg(cell, arms=("x265", "ours", "ours20")):
    memo = load_memo()
    c = seg_cell(cell)
    print(f"seg {cell}: rates {seg_rates(cell)} kb/s, arms {arms}", flush=True)
    for arm in arms:
        for k in seg_rates(cell):
            point(memo, f"seg|{cell}|{arm}|vbr|{k}", arm, "vbr", k,
                  c["frames"], c["fps"], src=c["src"])


def our_crf_guess(memo, cell, target, xq):
    ours = sorted((r["q"], math.log(r["bytes"])) for k, r in memo.items()
                  if k.startswith(f"lf|{cell}|ours|crf|"))
    if not ours:
        return xq - OUR_CRF_OFFSET
    t = math.log(target)
    if len(ours) == 1:
        slope = -math.log(2) / 6
    else:
        (q0, l0), (q1, l1) = ours[0], ours[-1]
        slope = (l1 - l0) / (q1 - q0) if q1 != q0 else -math.log(2) / 6
    # nearest point as the anchor
    qa, la = min(ours, key=lambda p: abs(p[1] - t))
    return int(round(qa + (t - la) / slope))


def run_lf(cell, modes=("crf", "vbr")):
    title, res = cell.rsplit("_", 1)
    memo = load_memo()
    f = LF[title]
    fps = f["fps"][0] / f["fps"][1]
    start, count = lf_span(title)
    feed = feeder_cmd(title, res)
    print(f"lf {cell}: {lf_geom(title, res)} frames {start}+{count}",
          flush=True)
    xr = {}
    for q in X265_LF_CRFS:
        xr[q] = point(memo, f"lf|{cell}|x265|crf|{q}", "x265", "crf", q,
                      count, fps, feed=feed)
    if "crf" in modes:
        # our CRFs are placed onto x265's rung bytes from what our own
        # points have written so far (log-bytes, ln 2 per 6 CRF until two
        # points give a slope); the first guess is the matrix's offset
        for xq in X265_LF_CRFS:
            t = xr[xq]["bytes"]
            # a rung already covered (a point within 1.35x of its bytes)
            # is not re-guessed: the guess moves as points arrive
            if any(abs(math.log(r["bytes"] / t)) < math.log(1.35)
                   for k, r in memo.items()
                   if k.startswith(f"lf|{cell}|ours|crf|")):
                continue
            q = our_crf_guess(memo, cell, t, xq)
            point(memo, f"lf|{cell}|ours|crf|{q}", "ours", "crf", q, count,
                  fps, feed=feed)
    if "vbr" in modes:
        rates = [round(xr[q]["kbps"]) for q in X265_LF_CRFS]
        for arm in ("x265", "ours"):
            for k in rates:
                point(memo, f"lf|{cell}|{arm}|vbr|{k}", arm, "vbr", k, count,
                      fps, feed=feed)

# ---------------------------------------------------------------- matched point
#
# The night script's long-form cell (enclab PARITY-SPEC section 3): x265 at
# its DEFAULT rate factor (crf 28), ours solved onto its file size within
# +/-1% (a fractional CRF, secant on log-bytes), both scored on every frame.
# The frames are the ones the night times: the whole excerpt at 720p and
# 1080p, a centred WIN_2160-second window at 2160p. The night reads this row
# and skips a cell that has none, because an unmatched cell isn't valid.
X265_DEFAULT_CRF = 28
WIN_2160 = float(os.environ.get("WIN_2160", "60"))
MATCH_TOL = 0.009


def timed_span(title, res):
    s0, n = lf_span(title)
    if res != "2160p":
        return s0, n
    f = LF[title]
    want = min(n, round(WIN_2160 * f["fps"][0] / f["fps"][1]))
    return s0 + (n - want) // 2, want


def span_feed(title, res, start, count, workers=1):
    feed = feeder_cmd(title, res)
    feed[feed.index("--start") + 1] = str(start)
    feed[feed.index("--count") + 1] = str(count)
    feed[feed.index("--workers") + 1] = str(workers)
    return feed


def encode_bytes(cmd, feed, out, frames):
    fp = subprocess.Popen(feed, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    ep = subprocess.run(cmd, stdin=fp.stdout, capture_output=True)
    fp.stdout.close()
    ferr = fp.stderr.read()
    fp.wait()
    if fp.returncode != 0:
        raise RuntimeError(f"feeder rc {fp.returncode}: {ferr[-400:]}")
    if ep.returncode != 0 or not os.path.exists(out):
        raise RuntimeError(f"encoder rc {ep.returncode}: {ep.stderr[-400:]}")
    n = len(au_sizes(out))
    if n != frames:
        raise RuntimeError(f"{n} access units, wanted {frames}")
    return os.path.getsize(out)


def run_match(cell):
    title, res = cell.rsplit("_", 1)
    memo = load_memo()
    mk = f"match|{cell}"
    start, count = timed_span(title, res)
    f = LF[title]
    fps = f["fps"][0] / f["fps"][1]
    feed = span_feed(title, res, start, count)
    fs = feed_sig(feed)
    if mk in memo and memo[mk].get("feed") == fs and \
            memo[mk].get("bin") == bin_hash(N265):
        return memo[mk]
    if os.path.exists(STOPF):
        print(f"STOP file: stopping before {mk}", flush=True)
        sys.exit(3)
    print(f"match {cell}: frames {start}+{count}", flush=True)
    t = time.time()
    xo = os.path.join(WORK, "mx.hevc")
    xb = encode_bytes(enc_cmd("x265", "crf", X265_DEFAULT_CRF, "-", xo), feed,
                      xo, count)
    # first guess: the ladder's own points on the same frames, else offset
    lad = sorted((r["q"], math.log(r["bytes"])) for k, r in memo.items()
                 if k.startswith(f"lf|{cell}|ours|crf|")) \
        if res != "2160p" else []
    if len(lad) >= 2:
        q = our_crf_guess(memo, cell, xb, X265_DEFAULT_CRF)
    else:
        q = X265_DEFAULT_CRF - OUR_CRF_OFFSET
    tried = {}
    best = None
    # bracketed secant on log-bytes: bytes fall as crf rises. The rate is a
    # step function of the CRF in places, so a bracket narrower than 0.02
    # ends the search at its nearer side, matched or not (the row says).
    for _ in range(8):
        q = round(q, 2)
        if q not in tried:
            o = os.path.join(WORK, f"mo{len(tried)}.hevc")
            tried[q] = (encode_bytes(enc_cmd("ours", "crf", q, "-", o), feed,
                                     o, count), o)
            print(f"  ours crf {q:.2f}: {tried[q][0]} B "
                  f"({100 * (tried[q][0] / xb - 1):+.2f}%)", flush=True)
        best = min(tried, key=lambda z: abs(math.log(tried[z][0] / xb)))
        if abs(tried[best][0] / xb - 1) <= MATCH_TOL:
            break
        over = [z for z in tried if tried[z][0] > xb]     # crf too low
        under = [z for z in tried if tried[z][0] < xb]    # crf too high
        if over and under:
            lo, hi = max(over), min(under)
            llo, lhi = math.log(tried[lo][0]), math.log(tried[hi][0])
            # a cliff: the rate drops ten times faster than a CRF step
            # does (ln 2 per 6) across a narrow bracket, so no CRF inside
            # it lands between; the item-24/26 rate switch is one such
            if hi - lo <= 0.02 or (hi - lo <= 0.1 and
                                   (llo - lhi) / (hi - lo) > 10 * math.log(2) / 6):
                break
            q = lo + (llo - math.log(xb)) / (llo - lhi) * (hi - lo)
            q = min(max(q, lo + 0.01), hi - 0.01)
        else:
            step = (math.log(tried[best][0]) - math.log(xb)) / (math.log(2) / 6)
            q = best + max(-8.0, min(8.0, step))
    ob, oo = tried[best]
    for z, (_, o) in tried.items():
        if o != oo and os.path.exists(o):
            os.unlink(o)
    mx = score(xo, None, WORK, count, ref_feed=feed)
    mo = score(oo, None, WORK, count, ref_feed=feed)
    md5 = {}
    for name, o in (("x265", xo), ("ours", oo)):
        h = hashlib.md5()
        with open(o, "rb") as fh:
            for b in iter(lambda: fh.read(1 << 20), b""):
                h.update(b)
        md5[name] = h.hexdigest()
        os.unlink(o)
    r = dict(feed=fs, bin=bin_hash(N265), start=start, count=count,
             x265_crf=X265_DEFAULT_CRF, x265_bytes=xb,
             x265_kbps=kbps_of(xb, count, fps), our_crf=best, our_bytes=ob,
             size_pct=100 * (ob / xb - 1), matched=abs(ob / xb - 1) <= 0.01,
             x265_neg=mx["neg"], our_neg=mo["neg"], x265_psnr_y=mx["psnr_y"],
             our_psnr_y=mo["psnr_y"], d_neg=mo["neg"] - mx["neg"],
             d_psnr_y=mo["psnr_y"] - mx["psnr_y"], md5=md5,
             solves=len(tried), all_s=round(time.time() - t, 1))
    memo = load_memo()
    memo[mk] = r
    save_memo(memo)
    print(f"  {mk}: x265 crf 28 {xb} B NEG {mx['neg']:.2f} | ours crf "
          f"{best:.2f} {ob} B ({r['size_pct']:+.2f}%) NEG {mo['neg']:.2f} "
          f"dNEG {r['d_neg']:+.2f} dY {r['d_psnr_y']:+.2f} "
          f"({r['all_s']:.0f} s)", flush=True)
    return r


# ---------------------------------------------------------------- report


def pts(memo, prefix, arm, mode):
    rs = sorted((v for k, v in memo.items()
                 if k.startswith(prefix) and v["arm"] == arm
                 and v["mode"] == mode), key=lambda r: r["bytes"])
    return [r["bytes"] for r in rs], [r["neg"] for r in rs], rs


def bdv(a, b):
    """BD-rate of test b against ref a (pts tuples); None without overlap."""
    if len(a[0]) < 3 or len(b[0]) < 3:
        return None
    return bd.bd_rate(a[0], a[1], b[0], b[1])


def fmt(x, spec="+.2f", tail="%"):
    return "n/a" if x is None else format(x, spec) + tail


def report():
    memo = load_memo()
    rows = []
    print("## VBR on the matrix segments (maxrate %.1fx, bufsize %.1fx)\n"
          % (MAXR, BUFR))
    print("| cell | CRF BD (matrix) | VBR BD ours (la 12) | VBR BD ours20 | "
          "ours20 vs ours | rate err x265 | ours | ours20 | underflows x/o/o20 |")
    print("|---|--:|--:|--:|--:|---|---|---|---|")
    for t in SEG_TITLES:
        for r in SEG_RES:
            cell = f"{t}_{r}"
            p = f"seg|{cell}|"
            x, o, o2 = (pts(memo, p, a, "vbr") for a in ("x265", "ours",
                                                         "ours20"))
            if not x[0] and not o[0]:
                continue
            def err(rs):
                if not rs:
                    return "n/a"
                e = [r["err_pct"] for r in rs]
                return f"{sum(e)/len(e):+.2f} ({min(e):+.1f}..{max(e):+.1f})"
            uf = "/".join(str(sum(r["underflows"] for r in z[2])) if z[2]
                          else "-" for z in (x, o, o2))
            print(f"| {cell} | {seg_cell(cell)['crf_bd']:+.2f}% | "
                  f"{fmt(bdv(x, o))} | {fmt(bdv(x, o2))} | {fmt(bdv(o, o2))} | "
                  f"{err(x[2])} | {err(o[2])} | {err(o2[2])} | {uf} |")
            for z in (x, o, o2):
                for rr in z[2]:
                    rows.append(("seg", cell, rr))
    print("\n### per rung\n")
    print("| cell | target kb/s | x265 kb/s / NEG | ours | ours20 |")
    print("|---|--:|---|---|---|")
    for t in SEG_TITLES:
        for r in SEG_RES:
            cell = f"{t}_{r}"
            p = f"seg|{cell}|"
            got = {a: {rr["q"]: rr for rr in pts(memo, p, a, "vbr")[2]}
                   for a in ("x265", "ours", "ours20")}
            if not any(got.values()):
                continue
            for k in seg_rates(cell):
                cells = []
                for a in ("x265", "ours", "ours20"):
                    rr = got[a].get(k)
                    cells.append("-" if not rr else
                                 f"{rr['kbps']:.0f} / {rr['neg']:.2f}")
                print(f"| {cell} | {k} | " + " | ".join(cells) + " |")
    print("\n## Long-form, 600 s excerpts, three rungs\n")
    print("| cell | class (whole title) | CRF BD | VBR BD | "
          "VBR rate err x265 / ours | underflows x265 / ours |")
    print("|---|---|--:|--:|---|---|")
    for title in LF:
        for res in ("720p", "1080p", "2160p"):
            cell = f"{title}_{res}"
            p = f"lf|{cell}|"
            xc, oc = pts(memo, p, "x265", "crf"), pts(memo, p, "ours", "crf")
            xv, ov = pts(memo, p, "x265", "vbr"), pts(memo, p, "ours", "vbr")
            if not xc[0]:
                continue
            def err(rs):
                if not rs:
                    return "n/a"
                e = [r["err_pct"] for r in rs]
                return f"{sum(e)/len(e):+.2f}"
            uf = " / ".join(str(sum(r["underflows"] for r in z[2])) if z[2]
                            else "-" for z in (xv, ov))
            print(f"| {cell} | {LF[title]['cls']} | {fmt(bdv(xc, oc))} | "
                  f"{fmt(bdv(xv, ov))} | "
                  f"{err(xv[2])} / {err(ov[2])} | {uf} |")
            for z in (xc, oc, xv, ov):
                for rr in z[2]:
                    rows.append(("lf", cell, rr))
    print("\n## Matched points (x265 default crf 28; the night's frames)\n")
    print("| cell | frames | x265 bytes | ours crf | size | matched | "
          "dVMAF-NEG | dPSNR-Y |")
    print("|---|---|--:|--:|--:|---|--:|--:|")
    for k in sorted(k for k in memo if k.startswith("match|")):
        m = memo[k]
        print(f"| {k[6:]} | {m['start']}+{m['count']} | {m['x265_bytes']} | "
              f"{m['our_crf']:.2f} | {m['size_pct']:+.2f}% | "
              f"{'yes' if m['matched'] else 'no'} | {m['d_neg']:+.2f} | "
              f"{m['d_psnr_y']:+.2f} |")
    cols = ["tier", "cell", "arm", "mode", "q", "lookahead", "frames", "bytes",
            "kbps", "target_kbps", "err_pct", "maxrate", "bufsize",
            "underflows", "min_fill", "peak1s_kbps", "neg", "vmaf", "psnr_y"]
    with open(CSV, "w") as f:
        f.write(",".join(cols) + "\n")
        for tier, cell, rr in rows:
            v = dict(rr, tier=tier, cell=cell)
            out = []
            for c in cols:
                x = v.get(c, "")
                out.append(f"{x:.4f}" if isinstance(x, float) else str(x))
            f.write(",".join(out) + "\n")
    print(f"\nwrote {CSV} ({len(rows)} points)")

# ---------------------------------------------------------------- queue

QUEUE = (
    # the matched points the night blocks read: Sunday's lf-a first, then
    # Monday's cells
    [("match", f"{t}_1080p", None) for t in ("tears", "cosmos", "sintel")] +
    [("match", f"{t}_1080p", None) for t in
     ("bbb", "stem2", "meridian", "nocturne", "lash")] +
    [("match", f"{t}_720p", None) for t in ("nocturne", "lash")] +
    [("match", f"{t}_2160p", None) for t in ("tears", "meridian", "nocturne")] +
    [("seg", f"{t}_720p", None) for t in SEG_TITLES] +
    [("seg", f"{t}_1080p", ("x265", "ours")) for t in SEG_TITLES] +
    [("lf", f"{t}_1080p", None) for t in
     ("tears", "cosmos", "sintel", "bbb", "stem2", "meridian", "nocturne",
      "lash")] +
    [("seg", f"{t}_2160p", ("x265", "ours")) for t in SEG_TITLES] +
    [("seg", f"{t}_{r}", ("ours20",)) for r in ("1080p", "2160p")
     for t in SEG_TITLES] +
    [("lf", f"{t}_720p", None) for t in ("nocturne", "lash")] +
    [("lf", f"{t}_2160p", None) for t in ("tears", "meridian", "nocturne")]
)


def main():
    os.makedirs(WORK, exist_ok=True)
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    cmd, args = sys.argv[1], sys.argv[2:]
    if cmd == "seg":
        for c in args:
            run_seg(c)
    elif cmd == "lf":
        for c in args:
            run_lf(c)
    elif cmd == "match":
        for c in args:
            run_match(c)
    elif cmd == "queue":
        for kind, cell, arms in QUEUE:
            try:
                if kind == "seg":
                    run_seg(cell, arms or ("x265", "ours", "ours20"))
                elif kind == "match":
                    run_match(cell)
                else:
                    run_lf(cell)
            except RuntimeError as e:
                print(f"FAILED {kind} {cell}: {e}", flush=True)
        with open(os.path.join(WORK, "DONE"), "w") as f:
            f.write(time.ctime() + "\n")
    elif cmd == "report":
        report()
    elif cmd == "rates":
        for t in SEG_TITLES:
            for r in SEG_RES:
                print(f"{t}_{r}", seg_rates(f"{t}_{r}"))
        for t in LF:
            print(t, lf_span(t), [lf_geom(t, r) for r in SEG_RES])
    else:
        sys.exit(__doc__)


if __name__ == "__main__":
    main()
