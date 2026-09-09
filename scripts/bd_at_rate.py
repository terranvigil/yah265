#!/usr/bin/env python3
# Copyright (c) 2026, the yah265 authors
# SPDX-License-Identifier: GPL-2.0-or-later
# Ported from yah264 (codec-agnostic instrument; the encoder binary, its flags
# and the reference encoder are the only changes).
"""BD-rate between two encoder configs measured at MATCHED ACHIEVED BITRATE.

The ad-hoc `bdcompare --points` sweep compares at a matched CRF *number*, which
is only sound while both arms' CRF-to-quality mapping is fixed. Any change that
translates the CRF axis -- mb-tree on/off does exactly this, since it engages
N265_CRF_CL_SHIFT -- makes that comparison measure ladder placement instead of
the encoder (the local measurement records).

So: for each target size, binary-search each arm's CRF onto it independently,
then BD over the resulting (bytes, VMAF) pairs. Placement cancels by
construction.
"""
import argparse, hashlib, json, os, shlex, shutil, subprocess, sys, tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import bdcompare as bd

bd.SUBSAMPLE = 1

# Encode/VMAF memo. The four targets' bisections revisit the same QPs and
# every lo/hi/final encode repeats a bisection point, so ~60% of the encodes
# per clip are duplicates; and the reference arm is identical across every
# one of our arms. Keyed by the exact command with {q} filled, the source
# path and size, and a hash of the encoder binary named first in the
# template (so a rebuild of ours never hits a stale entry). BD_CACHE names
# the directory (default ~/.cache/yah265-bd); BD_CACHE=0 disables it.
_CACHE = os.environ.get("BD_CACHE", os.path.expanduser("~/.cache/yah265-bd"))
if _CACHE == "0":
    _CACHE = None
_binhash = {}


def _bin_hash(tmpl):
    exe = shlex.split(tmpl)[0]
    if exe not in _binhash:
        path = shutil.which(exe) or exe
        h = hashlib.sha256()
        try:
            with open(path, "rb") as f:
                for blk in iter(lambda: f.read(1 << 20), b""):
                    h.update(blk)
        except OSError:
            h.update(exe.encode())
        _binhash[exe] = h.hexdigest()[:16]
    return _binhash[exe]


def _key(tmpl, q, src):
    cmd = tmpl.format(q=str(q), src=src, out="{out}")
    st = os.stat(src)
    return hashlib.sha256(f"{cmd}|{src}|{st.st_size}|{_bin_hash(tmpl)}"
                          .encode()).hexdigest()[:24]


def _cache_paths(tmpl, q, src):
    if not _CACHE:
        return None, None
    k = _key(tmpl, q, src)
    return os.path.join(_CACHE, k + ".hevc"), os.path.join(_CACHE, k + ".json")


def enc(tmpl, q, src, out):
    """Encode at q into `out`; returns the size. Served from the cache when
    the same command on the same binary has run before."""
    cb, cj = _cache_paths(tmpl, q, src)
    if cb and os.path.exists(cb):
        shutil.copyfile(cb, out)
        return os.path.getsize(out)
    subprocess.run(tmpl.format(q=str(q), src=src, out=out),
                   shell=True, capture_output=True)
    if not os.path.exists(out):
        return 0
    if cb:
        os.makedirs(_CACHE, exist_ok=True)
        tmp = cb + f".{os.getpid()}.tmp"
        shutil.copyfile(out, tmp)
        os.replace(tmp, cb)            # atomic: parallel arms may race here
    return os.path.getsize(out)


def vmaf_cached(tmpl, q, src, out, work):
    cb, cj = _cache_paths(tmpl, q, src)
    if cj and os.path.exists(cj):
        try:
            return json.load(open(cj))
        except (OSError, ValueError):
            pass
    v = bd.vmaf_of(out, src, work)
    if cj and v:
        tmp = cj + f".{os.getpid()}.tmp"
        with open(tmp, "w") as f:
            json.dump(v, f)
        os.replace(tmp, cj)
    return v


def solve(tmpl, src, target, out, sizes, lo=0, hi=51):
    """The integer QP (or CRF, once it exists) whose size lands nearest `target`
    bytes. Monotone: higher = fewer bits. Integer bisection, then the better
    of the two neighbours. `sizes` memoises q -> bytes within the run."""
    def size_at(q):
        if q not in sizes:
            sizes[q] = enc(tmpl, q, src, out)
        return sizes[q]
    while hi - lo > 1:
        mid = (lo + hi) // 2
        if size_at(mid) > target: lo = mid
        else: hi = mid
    cand = []
    for q in (lo, hi):
        cand.append((abs(size_at(q) - target), q))
    return min(cand)[1]


def curve(tmpl, src, targets, work, label):
    pts = []
    out = os.path.join(work, "s.hevc")
    sizes = {}
    for t in targets:
        q = solve(tmpl, src, t, out, sizes)
        sz = enc(tmpl, q, src, out)
        v = vmaf_cached(tmpl, q, src, out, work)
        if not v or "VMAF-NEG" not in v:
            return None
        pts.append((sz, v["VMAF-NEG"]))
        print(f"    {label:<22} target {t:>9} -> q {q:3d}  {sz:>9} B  "
              f"NEG {v['VMAF-NEG']:6.2f}", flush=True)
    return pts


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--a", required=True)
    ap.add_argument("--b", required=True)
    ap.add_argument("--clip", required=True)
    ap.add_argument("--frames", type=int, default=120)
    ap.add_argument("--targets", required=True,
                    help="comma-separated target byte sizes")
    ap.add_argument("--label-a", default="A")
    ap.add_argument("--label-b", default="B")
    args = ap.parse_args()

    src = f"tests/corpus/{args.clip}.y4m"
    targets = [int(x) for x in args.targets.split(",")]
    work = tempfile.mkdtemp(prefix="bdrate.")
    a = curve(args.a, src, targets, work, args.label_a)
    b = curve(args.b, src, targets, work, args.label_b)
    if not a or not b:
        print(f"{args.clip}: encode/VMAF failed"); return
    ra, ma = [p[0] for p in a], [p[1] for p in a]
    rb, mb = [p[0] for p in b], [p[1] for p in b]
    r = bd.bd_rate(rb, mb, ra, ma)
    print(f"  {args.clip:<15} BD-rate(VMAF-NEG) {args.label_a} vs {args.label_b}: "
          f"{r:+.2f}%" if isinstance(r, float) else f"  {args.clip}: {r}")


main()
