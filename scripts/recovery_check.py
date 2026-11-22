#!/usr/bin/env python3
# Copyright (c) 2026, the yah265 authors
# SPDX-License-Identifier: GPL-2.0-or-later
# Adapted from yah264's scripts/recovery_check.py (codec-agnostic tooling).
"""Does an --intra-refresh stream really recover at its recovery point?

A recovery_point SEI makes a narrow, testable claim: start decoding at that
access unit, with nothing before it but the parameter sets, and every picture
from recovery_poc_cnt after it on is the picture a decode from the start
would have produced. This cuts the stream at the chosen recovery point,
pastes the stream's first VPS, SPS and PPS in front, decodes the cut with
ffmpeg and with dec265, and compares the tail with the encoder's own
--dump-recon.

    scripts/recovery_check.py --stream out.hevc --recon out.yuv \\
        --size 352x288 --nth 2

The count is read off the SEI, not assumed, so a stream that promises less
than it delivers fails. The cut decode asks ffmpeg for every picture
(-flags2 +showall): what a decoder hands to a display after a mid-stream
start is its own policy, and the selection is made here, from the end.
Only P streams are supported, where decoding order is output order.

Exit 0 clean, 1 mismatch, 2 the stream carries no such recovery point.
"""
import argparse
import hashlib
import os
import subprocess
import sys
import tempfile


def nal_units(buf):
    """(start_of_nal_header, start_of_start_code) for every NAL, in order."""
    out = []
    i = 0
    while True:
        j = buf.find(b"\x00\x00\x01", i)
        if j < 0:
            break
        sc = j - 1 if j > 0 and buf[j - 1] == 0 else j
        out.append((j + 3, sc))
        i = j + 3
    return out


def rbsp(buf, start):
    end = buf.find(b"\x00\x00\x01", start)
    raw = buf[start:end if end >= 0 else len(buf)]
    out = bytearray()
    zeros = 0
    for b in raw:
        if zeros >= 2 and b == 3:
            zeros = 0
            continue
        out.append(b)
        zeros = zeros + 1 if b == 0 else 0
    return bytes(out)


def recovery_poc_cnt(sei):
    """recovery_poc_cnt of a recovery_point SEI whose RBSP starts at its
    payloadType byte: skip the type and the size, read one se(v)."""
    i = 1
    while sei[i] == 0xFF:
        i += 1
    bits = "".join(f"{b:08b}" for b in sei[i + 1:i + 9])
    lz = bits.index("1")
    k = int(bits[lz:2 * lz + 1], 2) - 1
    return (k + 1) // 2 if k & 1 else -(k // 2)


def frames_md5(raw, fsize):
    return [hashlib.md5(raw[i:i + fsize]).hexdigest()
            for i in range(0, len(raw) - fsize + 1, fsize)]


def ffmpeg_frames(path, fsize):
    r = subprocess.run(["ffmpeg", "-v", "error", "-flags2", "+showall",
                        "-f", "hevc", "-i", path, "-f", "rawvideo",
                        "-pix_fmt", "yuv420p", "-"], capture_output=True)
    return frames_md5(r.stdout, fsize)


def dec265_frames(dec, path, fsize):
    fd, out = tempfile.mkstemp(suffix=".yuv")
    os.close(fd)
    try:
        subprocess.run([dec, "-q", "-o", out, path], capture_output=True)
        return frames_md5(open(out, "rb").read(), fsize)
    finally:
        os.unlink(out)


def find_dec265():
    root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    for c in (os.environ.get("DEC265"), "dec265", "libde265-dec265",
              os.path.join(root, "..", "libde265", "build", "dec265",
                           "dec265")):
        if not c:
            continue
        if os.path.isfile(c) and os.access(c, os.X_OK):
            return c
        for d in os.environ.get("PATH", "").split(os.pathsep):
            p = os.path.join(d, c)
            if os.path.isfile(p) and os.access(p, os.X_OK):
                return p
    return None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--stream", required=True)
    ap.add_argument("--recon", help="the encoder's --dump-recon (8-bit 4:2:0)")
    ap.add_argument("--size", help="WxH of the recon")
    ap.add_argument("--nth", type=int, default=2,
                    help="which recovery point to cut at (1 = the first)")
    ap.add_argument("--count", action="store_true",
                    help="print the number of recovery points and stop")
    ap.add_argument("--quiet", action="store_true")
    a = ap.parse_args()

    buf = open(a.stream, "rb").read()
    nals = nal_units(buf)
    ends = [sc for _, sc in nals[1:]] + [len(buf)]
    sets, cut, settle, pics, seen, rp_pic = b"", None, 0, 0, 0, 0
    have = set()
    for k, (hdr, sc) in enumerate(nals):
        t = (buf[hdr] >> 1) & 0x3F
        if t in (32, 33, 34) and t not in have:
            have.add(t)
            sets += buf[sc:ends[k]]
        if t < 32 and buf[hdr + 2] & 0x80:
            pics += 1
        if t == 39 and buf[hdr + 2] == 6:
            seen += 1
            if seen == a.nth and cut is None:
                cut, rp_pic = sc, pics
                settle = recovery_poc_cnt(rbsp(buf, hdr + 2))
    if a.count:
        print(seen)
        return 0
    if cut is None:
        print(f"recovery_check: {a.stream}: no recovery point #{a.nth} "
              f"(found {seen})")
        return 2
    if not a.recon or not a.size:
        print("recovery_check: --recon and --size are required")
        return 1
    w, h = (int(v) for v in a.size.lower().split("x"))
    fsize = w * h * 3 // 2
    ref = frames_md5(open(a.recon, "rb").read(), fsize)
    tail = len(ref) - rp_pic - settle
    if tail <= 0:
        print(f"recovery_check: recovery point #{a.nth} at picture {rp_pic} "
              f"settles past the {len(ref)}-frame recon")
        return 1

    dec = find_dec265()
    if not dec:
        print("recovery_check: dec265 not found (set DEC265=)")
        return 1
    fd, cpath = tempfile.mkstemp(suffix=".hevc")
    try:
        with os.fdopen(fd, "wb") as f:
            f.write(sets + buf[cut:])
        got = {"ffmpeg": ffmpeg_frames(cpath, fsize),
               "dec265": dec265_frames(dec, cpath, fsize)}
    finally:
        os.unlink(cpath)

    rc = 0
    for name, g in got.items():
        if len(g) < tail:
            print(f"recovery_check: {name} decoded {len(g)} frame(s) of the "
                  f"cut, fewer than the {tail} the recovery point covers")
            rc = 1
            continue
        bad = sum(1 for x, y in zip(g[-tail:], ref[-tail:]) if x != y)
        early = 0
        if settle:
            lo = len(ref) - tail - settle
            early = sum(1 for x, y in zip(g[-(tail + settle):-tail],
                                          ref[lo:lo + settle]) if x != y)
        if bad:
            print(f"recovery_check: {name}: {bad}/{tail} frame(s) after the "
                  f"recovery point differ from the reconstruction")
            rc = 1
        elif not a.quiet:
            print(f"recovery_check: {name}: {tail} frame(s) exact from a cold "
                  f"start at recovery point #{a.nth} (picture {rp_pic}, "
                  f"recovery_poc_cnt {settle}; {early} of the frames before "
                  f"it differed)")
    return rc


if __name__ == "__main__":
    sys.exit(main())
