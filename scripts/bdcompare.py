#!/usr/bin/env python3
# Copyright (c) 2026, the yah265 authors
# SPDX-License-Identifier: GPL-2.0-or-later
# Ported from yah264 (codec-agnostic instrument; the encoder binary, its flags
# and the reference encoder are the only changes).
"""Quick BD-rate comparison between two encoder configurations.

Encodes truncated corpus clips at several rate points with both configs (in
parallel), decodes, and reports per-clip BD-rate. PSNR by default (fast,
numpy); --vmaf for the perceptual number when it matters. Meant for the tight
measure-ship-or-revert loop, where the full bench.py run is too slow.

Templates use {src}, {q}, {out}. Examples:

  # working tree vs x264 medium, CRF mode, VMAF
  scripts/bdcompare.py \
      --a './build/n265 {src} --qp {q} -o {out}' \
      --b 'x265 --preset medium --crf {q} --pools none --frame-threads 1 -o {out} {src}' \
      --points 20,23,26,29 --vmaf

  # feature evaluation: current binary vs a saved baseline binary, CQP PSNR
  scripts/bdcompare.py \
      --a './build/cli/yah265 --input-y4m {src} --qp {q} --cabac --bframes 3 --threads 8 -o {out}' \
      --b './baseline_n265 {src} --qp {q} -o {out}'

BD sign: negative means config A uses fewer bits than B at equal quality.

`--selftest` runs the BD integrator over recorded point sets and exits; no
encodes, numpy only. `make test` runs it.
"""
import argparse
import concurrent.futures as fut
import glob
import hashlib
import json
import math
import os
import re
import shlex
import subprocess
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def sh(cmd):
    return subprocess.run(cmd, shell=True, capture_output=True, text=True)


# Phase 12 item 3, step 0: does the decode in the band SYNTHESISE H.274 film
# grain, or not?
#
# ffmpeg's hevc decoder applies the film_grain_characteristics SEI by default
# and stops applying it when it is asked to hand the parameters out as frame
# side data instead (`-export_side_data film_grain`). So the band has two
# honest readings of a stream that carries the SEI, and this switch names
# which one is being taken:
#
#   FGC unset / 0  the SEI is parsed and NOT applied: the decoded planes are
#                  what the encoder actually coded. This is what every band
#                  reading before this item measured, and it stays the
#                  default. The flag is output-neutral on a stream WITHOUT
#                  the SEI, verified byte for byte (docs/phase12.md), which
#                  is why it costs the existing memo nothing.
#   FGC=1          grain is synthesised on output, scored against the
#                  ORIGINAL source: the honest viewer question, and the only
#                  reading under which a denoise-and-resynthesise arm can
#                  win anything.
#
# The mode is part of both memo keys in bd_at_rate.py: one bitstream has two
# different scores under it, so a cached point from one mode must never be
# served to the other.
FGC = int(os.environ.get("FGC", "0"))


def fgc_dec_flags():
    """ffmpeg input flags for the current FGC mode, as a list."""
    return [] if FGC else ["-export_side_data", "film_grain"]


def fgc_key():
    """The mode's term in a memo key. EMPTY in the default mode so that
    every entry cached before this item keeps its key."""
    return "fgc|" if FGC else ""


def decode_raw(path):
    import numpy as np
    p = subprocess.run(["ffmpeg", "-v", "error"] + fgc_dec_flags() +
                       ["-i", path, "-f", "rawvideo",
                        "-pix_fmt", "yuv420p", "-"], capture_output=True)
    return np.frombuffer(p.stdout, dtype=np.uint8)


def psnr_of(bitstream, src_raw):
    import numpy as np
    d = decode_raw(bitstream).astype(np.int32)
    n = min(len(d), len(src_raw))
    if n == 0 or len(d) != len(src_raw):
        return None
    mse = np.mean((d - src_raw[:n]) ** 2)
    return 10 * math.log10(255 * 255 / mse)


SUBSAMPLE = 1   # compute VMAF on every Nth frame (set from --subsample); 1 = all

# Sample depth of the SOURCE and of the decode the metrics read (8 or 10), set
# by bd_at_rate.py's --depth. It is not a property of the bitstream alone: a
# Main10 stream decoded to yuv420p would be scored two bits narrower than the
# source it is being compared against, which is a metric on content nobody
# encoded. See docs/instruments.md, "The band at 10 bits".
DEPTH = 8


def _dec_pixfmt():
    """(ffmpeg -pix_fmt, extra output flags) for a y4m decode at DEPTH.
    yuv420p10le is not a registered y4m pixel format, so writing it needs
    `-strict -1`; the tag that comes out is C420p10.

    `-sws_dither none` matters only where the decode is NARROWER than the
    stream - a Main10 bitstream scored in the 8-bit domain, which is the
    8-vs-10 self-comparison. swscale dithers by default, and dither is
    noise the encoder did not write; with it off the conversion is the
    exact inverse of upshift10.sh's left shift by two (verified: the
    upshifted corpus downshifts back to the original y4m byte for byte).
    An 8-bit stream decoded to yuv420p converts nothing, so the flag
    cannot move a number the 8-bit band ever read."""
    return ("yuv420p10le", " -strict -1") if DEPTH > 8 else ("yuv420p", "")


CHROMA_KEY = "chromaPSNR"
# The luma PSNR off the SAME ffmpeg run as the chroma guard. It costs nothing
# (the psnr filter prints y:, u: and v: on one line) and it is the second
# metric phase 12 item 3 has to report: VMAF-NEG's reading of SYNTHESISED
# grain is itself the open question, so a grain arm is argued on both numbers
# or on neither.
LUMA_KEY = "lumaPSNR"
# The two chroma planes SEPARATELY (phase 13 B0). CHROMA_KEY is their mean and
# a mean cannot say which plane is behind: bus's deficit against x265 is -0.86
# on Cb and -0.96 on Cr, and the goal legs' chroma reading is per PLANE with
# the bar on EITHER of them. Read off the same psnr line as the two above, so
# a cached point that gains all four pays one ffmpeg run for them.
CB_KEY = "cbPSNR"
CR_KEY = "crPSNR"


def chroma_psnr_of(bitstream, src_path):
    """Mean of the cb and cr PSNR of a decoded bitstream against the source,
    read the way scripts/board.py reads luma PSNR: ffmpeg's psnr filter with
    FRAME-INDEX alignment (the corpus y4m rates differ from the rawvideo
    default, so timestamp sync would compare the wrong frames) and
    shortest=1, which stops at the truncated encode's length.

    VMAF is luma-only, so chroma bits are the one thing an allocation term can
    quietly steal from; this is the chroma guard's reading
    (docs/phase7-psy-design.md M3). None if ffmpeg printed no psnr line."""
    p = sh(f'ffmpeg -loglevel info {" ".join(fgc_dec_flags())} '
           f'-i {shlex.quote(bitstream)} '
           f'-i {shlex.quote(src_path)} '
           f'-lavfi "[0:v]settb=1/25,setpts=N[d];[1:v]settb=1/25,setpts=N[r];'
           f'[d][r]psnr=shortest=1" -f null -')
    m = re.search(r"psnr.*\bu:([0-9.]+|inf).*?\bv:([0-9.]+|inf)", p.stderr)
    if not m:
        return None
    vals = [99.0 if g == "inf" else float(g) for g in m.groups()]
    return sum(vals) / 2.0


def planes_psnr_of(bitstream, src_path):
    """(y, u, v) PSNR in dB off one psnr-filter run, or (None, None, None).

    The one ffmpeg call every PSNR reading in this tree goes through: the
    filter prints y:, u: and v: on the same line, so the plane split costs
    nothing over the mean that `yuv_psnr_of` returns."""
    p = sh(f'ffmpeg -loglevel info {" ".join(fgc_dec_flags())} '
           f'-i {shlex.quote(bitstream)} '
           f'-i {shlex.quote(src_path)} '
           f'-lavfi "[0:v]settb=1/25,setpts=N[d];[1:v]settb=1/25,setpts=N[r];'
           f'[d][r]psnr=shortest=1" -f null -')
    m = re.search(r"psnr.*\by:([0-9.]+|inf).*?\bu:([0-9.]+|inf)"
                  r".*?\bv:([0-9.]+|inf)", p.stderr)
    if not m:
        return None, None, None
    y, u, v = [99.0 if g == "inf" else float(g) for g in m.groups()]
    return y, u, v


def yuv_psnr_of(bitstream, src_path):
    """(lumaPSNR, chromaPSNR) off one psnr-filter run; either may be None."""
    y, u, v = planes_psnr_of(bitstream, src_path)
    if y is None:
        return None, None
    return y, (u + v) / 2.0


def vmaf_of(bitstream, src_path, work, per_frame=False):
    # `per_frame` adds the PER-FRAME vmaf_neg list under "frames", in
    # display order, off the same libvmaf run. shot_hull.py slices a
    # per-shot mean out of it, and it has to be this call and not a
    # second one: the temporal features make a frame's score depend on
    # its neighbours, so a shot scored as its own clip is NOT the shot's
    # score inside the sequence (multishot_bd.py averages the same
    # whole-file per-frame array, which is what makes the two agree).
    # Nothing else about the call moves, and the default is off, so
    # every existing caller reads exactly what it read before.
    #
    # Unique temp paths (NOT derived from the bitstream) so shared/cached
    # bitstreams don't collide across threads.
    fd, dec = tempfile.mkstemp(suffix=".dec.y4m", dir=work); os.close(fd)
    fd, js = tempfile.mkstemp(suffix=".json", dir=work); os.close(fd)
    pixfmt, strict = _dec_pixfmt()
    sh(f'ffmpeg -v error -y {" ".join(fgc_dec_flags())} -i "{bitstream}" '
       f'-pix_fmt {pixfmt} -sws_dither none{strict} "{dec}"')
    binary = os.environ.get("VMAF", "vmaf")
    margs, primary, plabel = _vmaf_models()
    sub = f"--subsample {SUBSAMPLE}" if SUBSAMPLE > 1 else ""
    # libvmaf's thread pool changes nothing but the wall: scores are
    # identical to every digit at 1, 4 and 8 threads (park_joy 30 frames,
    # 2026-09-11), and 8 threads read a 1080p point about 6x sooner.
    # VMAF_THREADS overrides; 0 is libvmaf's single-threaded default.
    thr = os.environ.get("VMAF_THREADS", "8")
    r = sh(f'{shlex.quote(binary)} -r "{src_path}" -d "{dec}" --json -o "{js}" {margs} {sub} --threads {thr}')
    try:
        os.unlink(dec)
    except OSError:
        pass
    if r.returncode != 0:
        return None
    doc = json.load(open(js))
    pm = doc["pooled_metrics"]
    if primary not in pm:
        return None
    # Report the best available VMAF (v1 when YAH264_VMAF_MODEL is set) plus
    # NEG, which penalises enhancement/sharpening -- the honest gate for
    # texture-retention features (psy-RD) that plain VMAF over-credits.
    out = {plabel: pm[primary]["mean"]}
    if "vmaf_neg" in pm:
        out["VMAF-NEG"] = pm["vmaf_neg"]["mean"]
    if per_frame and "vmaf_neg" in pm:
        out["frames"] = [f["metrics"]["vmaf_neg"] for f in doc["frames"]]
    return out


_MODELS = None


def _vmaf_models():
    """(model-args string, primary pooled key, report label). Prefers the v1
    model from YAH264_VMAF_MODEL, mirroring bench.py; always adds NEG."""
    global _MODELS
    if _MODELS is not None:
        return _MODELS
    parts, primary, label = [], "vmaf", "VMAF"
    env_path = os.environ.get("YAH264_VMAF_MODEL")
    if env_path and os.path.exists(env_path):
        parts.append(f"--model path={shlex.quote(env_path)}:name=vmaf_v1")
        primary, label = "vmaf_v1", "VMAF-v1"
    parts.append("--model version=vmaf_v0.6.1:name=vmaf")
    parts.append("--model version=vmaf_v0.6.1neg:name=vmaf_neg")
    _MODELS = (" ".join(parts), primary, label)
    return _MODELS


# A degenerate cubic fit shows up as an absurd mean log-bitrate delta rather
# than as an inf, so the guard is on the delta: 5.0 is a BD-rate outside
# [-99.3%, +14741%], which no real sweep produces (the board's whole history
# spans -44.5% to +17.9%), while bus_cif's saturated points read 183.
_FIT_SANE_LOGDELTA = 5.0


def bd_rate(r_ref, m_ref, r_test, m_test):
    """BD-rate of test against ref over (rate, metric) points. Negative =
    test spends fewer bits at equal quality.

    Bjontegaard's cubic least-squares fit is the answer wherever it is well
    conditioned, and it is what every number on the board and in the band
    tables was read with. It is NOT well conditioned when the metric
    saturates: with four points, the fit interpolates, and two points pinned
    at VMAF-NEG 99.95 a hundredth apart across a 1.9x bitrate step force an
    almost vertical segment whose integral over the overlap explodes -- the
    board read 1.09e81% on bus_cif at fixed QPs 22/27/32/37, and blanked the
    row rather than chart it. Where the fit degenerates, integrate a MONOTONE
    piecewise cubic (PCHIP) through the same points instead: it cannot
    oscillate between them, so the saturated top contributes its own tiny
    quality span and nothing more. (2026-09-10, docs/instruments.md)"""
    import numpy as np
    deg = min(3, len(m_ref) - 1)
    f1 = np.polyfit(m_ref, np.log(r_ref), deg)
    f2 = np.polyfit(m_test, np.log(r_test), deg)
    lo, hi = max(min(m_ref), min(m_test)), min(max(m_ref), max(m_test))
    if hi <= lo:
        return None
    xs = np.linspace(lo, hi, 100)
    d = float(np.mean(np.polyval(f2, xs) - np.polyval(f1, xs)))
    if not math.isfinite(d) or abs(d) > _FIT_SANE_LOGDELTA:
        d = _bd_logdelta_pchip(r_ref, m_ref, r_test, m_test, lo, hi)
    if d is None or not math.isfinite(d):
        return None
    return (math.exp(d) - 1) * 100


def _pchip_edge(h0, h1, d0, d1):
    """One-sided three-point end slope, clamped so the end interval keeps
    the sign and the shape of the data (Fritsch-Carlson)."""
    s = ((2 * h0 + h1) * d0 - h0 * d1) / (h0 + h1)
    if s * d0 <= 0:
        return 0.0
    if d0 * d1 < 0 and abs(s) > abs(3 * d0):
        return 3 * d0
    return s


def _pchip_slopes(x, y):
    """Fritsch-Carlson shape-preserving slopes -- what scipy's
    PchipInterpolator computes, written out because scipy is not a
    dependency of this tree."""
    import numpy as np
    h = np.diff(x)
    dl = np.diff(y) / h
    m = np.zeros(len(y))
    for i in range(1, len(y) - 1):
        # zero at a sign change or a flat run, weighted harmonic mean else
        if dl[i - 1] * dl[i] > 0:
            w1, w2 = 2 * h[i] + h[i - 1], h[i] + 2 * h[i - 1]
            m[i] = (w1 + w2) / (w1 / dl[i - 1] + w2 / dl[i])
    if len(y) == 2:
        m[0] = m[1] = dl[0]
    else:
        m[0] = _pchip_edge(h[0], h[1], dl[0], dl[1])
        m[-1] = _pchip_edge(h[-1], h[-2], dl[-1], dl[-2])
    return m


def _pchip_eval(x, y, xs):
    """The monotone cubic through (x, y), sampled at xs (inside [x0, xn])."""
    import numpy as np
    m = _pchip_slopes(x, y)
    i = np.clip(np.searchsorted(x, xs) - 1, 0, len(x) - 2)
    h = x[i + 1] - x[i]
    t = (xs - x[i]) / h
    t2 = t * t
    t3 = t2 * t
    return ((2 * t3 - 3 * t2 + 1) * y[i] + (t3 - 2 * t2 + t) * h * m[i]
            + (-2 * t3 + 3 * t2) * y[i + 1] + (t3 - t2) * h * m[i + 1])


def _pchip_curve(rate, metric):
    """(quality, log-rate) sorted on quality, with repeated qualities
    collapsed -- a fully pinned metric would otherwise divide by a zero
    interval. None if fewer than two distinct qualities survive."""
    import numpy as np
    order = np.argsort(np.asarray(metric, dtype=float))
    x = np.asarray(metric, dtype=float)[order]
    y = np.log(np.asarray(rate, dtype=float)[order])
    kx, ky = [], []
    for xi, yi in zip(x, y):
        if kx and xi - kx[-1] < 1e-9:
            ky[-1] = (ky[-1] + yi) / 2
            continue
        kx.append(float(xi))
        ky.append(float(yi))
    if len(kx) < 2:
        return None
    return np.array(kx), np.array(ky)


def _bd_logdelta_pchip(r_ref, m_ref, r_test, m_test, lo, hi):
    """Mean log-bitrate delta over [lo, hi] under the monotone fit."""
    import numpy as np
    a = _pchip_curve(r_ref, m_ref)
    b = _pchip_curve(r_test, m_test)
    if a is None or b is None:
        return None
    lo = max(lo, a[0][0], b[0][0])
    hi = min(hi, a[0][-1], b[0][-1])
    if hi <= lo:
        return None
    xs = np.linspace(lo, hi, 1000)
    return float(np.mean(_pchip_eval(b[0], b[1], xs)
                         - _pchip_eval(a[0], a[1], xs)))


def selftest():
    """`bdcompare.py --selftest`: the BD integrator on the point set that
    used to blow it up and on one that never did. No encodes, no ffmpeg,
    numpy only; wired into `make test`. Exit 77 (meson's skip) without
    numpy."""
    try:
        import numpy as np
    except ImportError:
        print("bdcompare selftest: no numpy, skipped")
        return 77
    bad = []

    def check(ok, msg):
        print(("  ok    " if ok else "  FAIL  ") + msg)
        if not ok:
            bad.append(msg)

    # bus_cif at the board's fixed QPs 22/27/32/37, 120 frames, ab446a4:
    # VMAF-NEG pins at 99.95 on the top two points, 0.016 apart across a
    # 1.9x bitrate step. The cubic fit read 1.09e81% here and the row was
    # blanked; the monotone fit reads about -19%.
    xr = [1164704, 2385976, 4617544, 8661608]
    xm = [85.936, 98.323, 99.946, 99.962]
    orr = [1003640, 2111520, 4174480, 7987568]
    om = [87.344, 98.953, 99.949, 99.963]
    r = bd_rate(xr, xm, orr, om)
    check(r is not None and math.isfinite(r), "saturated set yields a number")
    check(r is not None and -60.0 < r < 0.0,
          f"saturated set is sane: {r:.2f}%")

    # foreman_cif, same QPs, same commit: top point 96.9, nowhere near
    # saturation. The classic cubic must still be what comes out, to the
    # precision the board prints, or the CSV time series has a step in it.
    fxr = [360448, 714816, 1519328, 3444912]
    fxm = [73.525, 85.743, 93.03, 96.912]
    forr = [264216, 504448, 988560, 2000008]
    fom = [72.133, 84.87, 92.59, 96.434]
    f = bd_rate(fxr, fxm, forr, fom)
    xs = np.linspace(max(min(fxm), min(fom)), min(max(fxm), max(fom)), 100)
    classic = (math.exp(np.mean(
        np.polyval(np.polyfit(fom, np.log(forr), 3), xs)
        - np.polyval(np.polyfit(fxm, np.log(fxr), 3), xs))) - 1) * 100
    check(f == classic,
          f"well-conditioned set is still the plain cubic: {f:.4f}%")

    # and the monotone fit is not a different instrument where the cubic
    # is sound -- it just does not explode where the cubic does
    lo = max(min(fxm), min(fom))
    hi = min(max(fxm), max(fom))
    d = _bd_logdelta_pchip(fxr, fxm, forr, fom, lo, hi)
    p = (math.exp(d) - 1) * 100
    check(abs(p - f) < 1.5, f"monotone fit agrees there: {p:.2f}%")

    # the interpolant itself: through every node, monotone in between
    x = np.array([0.0, 1.0, 2.0, 10.0])
    y = np.array([0.0, 0.1, 0.2, 9.0])
    check(bool(np.allclose(_pchip_eval(x, y, x), y)), "pchip hits its nodes")
    xs = np.linspace(0.0, 10.0, 501)
    check(bool(np.all(np.diff(_pchip_eval(x, y, xs)) >= -1e-12)),
          "pchip stays monotone between them")

    print("bdcompare selftest: " + ("FAILED" if bad else "passed"))
    return 1 if bad else 0


def main():
    if "--selftest" in sys.argv:
        sys.exit(selftest())
    ap = argparse.ArgumentParser()
    ap.add_argument("--a", required=True, help="encoder A command template")
    ap.add_argument("--b", required=True, help="encoder B command template (reference)")
    # Default CRF points span VMAF's DISCRIMINATING band (~55-95) on CIF, not the
    # saturated 95-100 top where BD-rate is noisy/overflow-prone (CRF 20-28 pins
    # near 100 on easy clips). Push points up for higher-res content.
    ap.add_argument("--points", default="26,32,38,44")
    ap.add_argument("--clips", default="foreman_cif,mobile_cif")
    ap.add_argument("--class", dest="klass", default=None,
                    help="select all clips of a content class from the "
                         "tests/corpus/CLASSES manifest (e.g. grain, motion); "
                         "overrides --clips. See docs/corpus-broadening-plan.md")
    ap.add_argument("--frames", type=int, default=120)
    ap.add_argument("--vmaf", action="store_true", help="VMAF instead of PSNR")
    ap.add_argument("--subsample", type=int, default=5,
                    help="compute VMAF on every Nth frame (libvmaf --subsample); "
                         "5-10 is ~Nx faster with negligible BD change")
    ap.add_argument("--jobs", type=int, default=16)
    ap.add_argument("--no-cache", dest="cache", action="store_false",
                    help="disable the persistent encode cache (on by default; a "
                         "fixed baseline binary is encoded once across sweeps)")
    args = ap.parse_args()
    global SUBSAMPLE
    SUBSAMPLE = max(1, args.subsample)

    points = [int(q) for q in args.points.split(",")]
    if args.klass:
        manifest = os.path.join(ROOT, "tests", "corpus", "CLASSES")
        if not os.path.exists(manifest):
            sys.exit("bdcompare: --class needs tests/corpus/CLASSES "
                     "(run scripts/fetch_corpus.sh)")
        clips = [ln.split()[0] for ln in open(manifest)
                 if len(ln.split()) == 2 and ln.split()[1] == args.klass]
        if not clips:
            sys.exit(f"bdcompare: no clips of class '{args.klass}' in {manifest}")
        print(f"class {args.klass}: {', '.join(clips)}")
    else:
        clips = args.clips.split(",")
    work = tempfile.mkdtemp(prefix="bdq_")

    srcs = {}
    for c in clips:
        full = os.path.join(ROOT, "tests", "corpus", c + ".y4m")
        if not os.path.exists(full):
            sys.exit(f"bdcompare: no corpus clip {full}")
        cut = os.path.join(work, c + ".y4m")
        sh(f'ffmpeg -v error -y -i "{full}" -frames:v {args.frames} "{cut}"')
        srcs[c] = cut

    # Persistent encode cache: keyed on (template, clip, q, frames, encoder-binary
    # mtime). A fixed baseline binary hits the cache across strength sweeps, so
    # its encodes are computed once, not per invocation (~halves repeated sweeps).
    # The binary mtime in the key invalidates automatically on rebuild.
    cache_dir = os.path.join(tempfile.gettempdir(), "bdcache")
    if args.cache:
        os.makedirs(cache_dir, exist_ok=True)

    def _ckey(tmpl, clip, q):
        binp = (shlex.split(tmpl)[0] if tmpl.strip() else "")
        mt = os.path.getmtime(binp) if os.path.exists(binp) else 0
        h = hashlib.sha1(f"{tmpl}|{clip}|{q}|{args.frames}|{mt}".encode()).hexdigest()[:20]
        return os.path.join(cache_dir, h + ".264")

    def encode(tag, tmpl, clip, q):
        out = os.path.join(work, f"{clip}_{tag}_{q}.264")
        if args.cache:
            ck = _ckey(tmpl, clip, q)
            if os.path.exists(ck):
                return (tag, clip, q, ck)
        cmd = tmpl.format(src=shlex.quote(srcs[clip]), q=q, out=shlex.quote(out))
        r = sh(cmd)
        if r.returncode != 0 or not os.path.exists(out):
            return (tag, clip, q, None)
        if args.cache:
            try:
                os.replace(out, ck); return (tag, clip, q, ck)
            except OSError:
                pass
        return (tag, clip, q, out)

    jobs = []
    with fut.ThreadPoolExecutor(max_workers=args.jobs) as ex:
        for clip in clips:
            for q in points:
                jobs.append(ex.submit(encode, "a", args.a, clip, q))
                jobs.append(ex.submit(encode, "b", args.b, clip, q))
        results = [j.result() for j in jobs]

    outs = {(t, c, q): o for t, c, q, o in results}
    if any(o is None for o in outs.values()):
        bad = [k for k, v in outs.items() if v is None]
        sys.exit(f"bdcompare: encode failed for {bad[:3]}")

    src_raw = {}
    if not args.vmaf:
        for c in clips:
            src_raw[c] = decode_raw(srcs[c]).astype("int32")

    def measure(clip, tag, q):
        o = outs[(tag, clip, q)]
        if args.vmaf:
            m = vmaf_of(o, srcs[clip], work)
        else:
            p = psnr_of(o, src_raw[clip])
            m = {"PSNR": p} if p is not None else None
        return (clip, tag, q, os.path.getsize(o), m)

    with fut.ThreadPoolExecutor(max_workers=args.jobs if args.vmaf else 2) as ex:
        ms = [ex.submit(measure, c, t, q) for c in clips for t in ("a", "b")
              for q in points]
        metrics = [m.result() for m in ms]

    table = {}
    for clip, tag, q, size, m in metrics:
        if m is None:
            sys.exit(f"bdcompare: metric failed for {clip} {tag} {q}")
        table.setdefault((clip, tag), []).append((q, size, m))

    # metric names come from the measured dict (e.g. VMAF-v1 + VMAF-NEG, or PSNR)
    names = list(next(iter(table.values()))[0][2].keys())
    for clip in clips:
        for i, nm in enumerate(names):
            rows = {}
            for tag in ("a", "b"):
                pts = sorted(table[(clip, tag)], key=lambda t: t[0], reverse=True)
                rows[tag] = ([p[1] for p in pts], [p[2][nm] for p in pts])
                if i == 0:      # print the size/quality points once per clip
                    print(f"{clip:14} {tag}: " +
                          " ".join(f"{s//1000}k/{m[nm]:.2f}" for _, s, m in pts))
            bd = bd_rate(*rows["b"], *rows["a"])
            # Flag saturation: if the top point on either side is >=98, VMAF is
            # near-pinned and the BD is measured in a low-discrimination regime
            # (unreliable / overflow-prone). Push --points higher.
            hi = max(max(rows["a"][1]), max(rows["b"][1])) if nm != "PSNR" else 0
            warn = "  [!] near-saturation, raise --points" if hi >= 98 else ""
            print(f"{clip:14} BD-rate({nm}) A vs B: "
                  + (f"{bd:+.2f}%" if bd is not None else "no quality overlap") + warn)


if __name__ == "__main__":
    main()
