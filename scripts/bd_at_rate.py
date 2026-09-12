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
# path and size, the SAMPLE DEPTH, and a hash of the encoder binary named
# first in the template (looking past a leading `env VAR=..`, so a rebuild
# of ours never hits a stale entry), and every N265_* variable in the
# environment (arms are selected by them; the command string alone cannot
# tell arms apart). BD_CACHE names the directory (default
# ~/.cache/yah265-bd); BD_CACHE=0 disables it.
#
# The depth is in the key EXPLICITLY (phase 8 item 4), not left to the
# source path that happens to differ: a 10-bit point must never be served
# for an 8-bit one, and the depth changes what the METRIC read as well as
# what the encoder wrote (bdcompare.DEPTH picks the decode format).
_CACHE = os.environ.get("BD_CACHE", os.path.expanduser("~/.cache/yah265-bd"))
if _CACHE == "0":
    _CACHE = None
_binhash = {}


def _exe_of(tmpl):
    """The encoder binary in a template, looking past a leading `env` and
    any VAR=value assignments in front of it."""
    toks = shlex.split(tmpl)
    # a leading env by any path (`env`, `/usr/bin/env`): 2026-09-12 an arm
    # written as `/usr/bin/env N265_X=1 build/n265` hashed the env binary
    if toks and os.path.basename(toks[0]) == "env":
        toks = toks[1:]
    for t in toks:
        if "=" not in t or t.startswith("/") or t.startswith("."):
            return t
    return toks[0] if toks else ""


def _env_key():
    """Every N265_* variable in our environment, since arms are selected
    by them and the command string alone cannot tell two arms apart."""
    return ";".join(f"{k}={v}" for k, v in sorted(os.environ.items())
                    if k.startswith("N265_"))


def _bin_hash(tmpl):
    exe = _exe_of(tmpl)
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


def _depth_key():
    """The depth term of both memo keys. EMPTY at 8, so every 8-bit entry
    already in the cache keeps its key and the depth mode costs the 8-bit
    band nothing; `d10|` at Main10, so a 10-bit point can never be served
    for an 8-bit one even if the two ever named the same source."""
    return "" if bd.DEPTH == 8 else f"d{bd.DEPTH}|"


def _key(tmpl, q, src):
    cmd = tmpl.format(q=str(q), src=src, out="{out}")
    st = os.stat(src)
    return hashlib.sha256(f"{cmd}|{src}|{st.st_size}|{_depth_key()}"
                          f"{_bin_hash(tmpl)}|"
                          f"{_env_key()}".encode()).hexdigest()[:24]


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
    r = subprocess.run(tmpl.format(q=str(q), src=src, out=out),
                       shell=True, capture_output=True)
    if not os.path.exists(out):
        return 0
    # only a complete encode may enter the cache: a killed child leaves a
    # truncated file that would otherwise be served to every later arm
    if cb and r.returncode == 0 and os.path.getsize(out) > 0:
        os.makedirs(_CACHE, exist_ok=True)
        tmp = cb + f".{os.getpid()}.tmp"
        shutil.copyfile(out, tmp)
        os.replace(tmp, cb)            # atomic: parallel arms may race here
    return os.path.getsize(out)


def _content_json(out, src):
    """Second memo, keyed by what the bitstream IS rather than which binary
    wrote it. Every item re-measures "today" on a rebuilt binary, and every
    identity arm reproduces a bitstream byte for byte; the per-binary memo
    above misses all of those and re-reads VMAF (the slow half of an HD
    point). The metric depends only on the bitstream, the source and the
    model set, so that is the key."""
    if not _CACHE:
        return None
    h = hashlib.sha256()
    with open(out, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    st = os.stat(src)
    h.update(f"|{src}|{st.st_size}|{_depth_key()}{bd.SUBSAMPLE}|"
             f"{bd._vmaf_models()[0]}".encode())
    d = os.path.join(_CACHE, "vmaf")
    os.makedirs(d, exist_ok=True)
    return os.path.join(d, h.hexdigest()[:24] + ".json")


def _write_json(path, v):
    tmp = path + f".{os.getpid()}.tmp"
    with open(tmp, "w") as f:
        json.dump(v, f)
    os.replace(tmp, path)


def vmaf_cached(tmpl, q, src, out, work):
    cb, cj = _cache_paths(tmpl, q, src)
    v = None
    if cj and os.path.exists(cj):
        try:
            v = json.load(open(cj))
        except (OSError, ValueError):
            v = None
    ck = _content_json(out, src)
    if not v and ck and os.path.exists(ck):
        try:
            v = json.load(open(ck))
        except (OSError, ValueError):
            v = None
    if not v:
        v = bd.vmaf_of(out, src, work)
    # The chroma guard's reading (docs/phase7-psy-design.md M3) lives in the
    # same memo as the VMAF one, so a point cached before the guard existed
    # gains it on its next touch and never re-reads VMAF for it. It is read
    # off the same bitstream and cannot move a luma number.
    if v and bd.CHROMA_KEY not in v:
        c = bd.chroma_psnr_of(out, src)
        if c is not None:
            v[bd.CHROMA_KEY] = c
    if v:
        if ck:
            _write_json(ck, v)
        if cj:
            _write_json(cj, v)
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
        c = v.get(bd.CHROMA_KEY)
        pts.append((sz, v["VMAF-NEG"], c))
        cs = f"  cPSNR {c:5.2f}" if isinstance(c, float) else ""
        print(f"    {label:<22} target {t:>9} -> q {q:3d}  {sz:>9} B  "
              f"NEG {v['VMAF-NEG']:6.2f}{cs}", flush=True)
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
    ap.add_argument("--srcdir", default="tests/corpus",
                    help="where the source y4m lives (local/corpus10 for the "
                         "upshifted Main10 corpus)")
    ap.add_argument("--depth", type=int, default=8, choices=(8, 10),
                    help="sample depth of the source and of the decode the "
                         "metrics read; in both memo keys")
    args = ap.parse_args()

    # before any encode or any key: the depth picks the decode format
    bd.DEPTH = args.depth
    src = os.path.join(args.srcdir, args.clip + ".y4m")
    targets = [int(x) for x in args.targets.split(",")]
    work = tempfile.mkdtemp(prefix="bdrate.")
    a = curve(args.a, src, targets, work, args.label_a)
    b = curve(args.b, src, targets, work, args.label_b)
    if not a or not b:
        print(f"{args.clip}: encode/VMAF failed"); return
    ra, ma = [p[0] for p in a], [p[1] for p in a]
    rb, mb = [p[0] for p in b], [p[1] for p in b]
    r = bd.bd_rate(rb, mb, ra, ma)
    if not isinstance(r, float):
        print(f"  {args.clip}: {r}")
        return
    line = (f"  {args.clip:<15} BD-rate(VMAF-NEG) {args.label_a} vs "
            f"{args.label_b}: {r:+.2f}%")
    # The chroma guard, on the same points: VMAF is luma-only, so an arm can
    # win the luma reading by starving chroma and the band would call it
    # quality. Skipped silently where ffmpeg gave no psnr line, which leaves
    # the luma reading exactly as it was.
    ca = [p[2] for p in a]
    cbp = [p[2] for p in b]
    if all(isinstance(x, float) for x in ca + cbp):
        rc = bd.bd_rate(rb, cbp, ra, ca)
        if isinstance(rc, float):
            line += f"  chromaPSNR {rc:+.2f}%"
    print(line)


# guarded so the bisection and the encode memo can be imported
# (scripts/bd_8v10.py); running the file is exactly what it was
if __name__ == "__main__":
    main()
