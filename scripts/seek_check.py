#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""The random access claim an open-GOP stream makes, checked.

A CRA picture is a random access point. A decoder handed the stream from
that CRA onward has to do two things (8.1.3, 7.4.2.2): derive
NoRaslOutputFlag = 1 for it, and DISCARD the RASL pictures that follow it
in decoding order, because they predict from pictures the decoder never
saw. Its RADL pictures, if the encoder made any, it keeps. What comes out
is every picture from the first RADL - or from the CRA where there are
none - onward, in output order, and it has to be BIT-EXACT with what a
decode of the whole stream produced for those same pictures. A stream
where it is not has a CRA that is not a random access point, whatever its
NAL type says.

So this script cuts the stream at a CRA the way a player seeking to one
does. The cut takes the whole access unit: its access unit delimiter and
its prefix SEIs come with the picture. The parameter sets go in front
ONLY when that access unit does not already carry them - under
`--repeat-headers` it does, and an open-GOP stream written with that flag
is seekable on its own; without it they come from the head of the stream,
which is what a container hands a decoder on a seek. Then both decoders
get the result:

  ffmpeg   compared in OUTPUT order, byte for byte, against the tail of
           the full decode.
  dec265   compared as a MULTISET of frames, because dec265 -o emits
           DECODE order and the two orders differ over exactly the
           pictures this feature reorders. A frame count and a set of
           frames together are what the display-order claim reduces to
           once the order itself is ffmpeg's job.

Both legs are held to the same reference, which is the full decode the
caller passes in - in conformance.sh that is the file the recon match
already agreed with, so a seek that matches it matches the encoder's own
reconstruction.

With --all-irap it cuts at every IRAP in turn, IDRs included, and
--expect-starts names display frames that must each start one of those
cuts. That is the check for --shot-list: every listed shot start is a
place a decoder can begin.

usage:
  seek_check.py <stream.265> <full-decode.yuv> --geometry WxH
                [--csp 420|422|444] [--depth 8|10] [--cra N]
                [--all-irap [--expect-starts 0,150,300]]
                [--ffmpeg path] [--dec265 path] [--keep DIR]
"""
import argparse
import hashlib
import os
import subprocess
import sys
import tempfile

VCL_MAX = 31
RADL_N, RADL_R, RASL_N, RASL_R = 6, 7, 8, 9
CRA_NUT = 21
AUD = 35
PREFIX_SEI = 39
PARAM = (32, 33, 34)        # VPS, SPS, PPS
IRAP = range(16, 24)
# the non-VCL NALs that belong to the FRONT of an access unit (7.4.2.4.4),
# so a cut at a picture takes them with it
AU_HEAD = (AUD, PREFIX_SEI) + PARAM


def nals(d):
    """(start of the start code, payload start, type) for every NAL."""
    out = []
    i = 0
    while True:
        j = d.find(b"\x00\x00\x01", i)
        if j < 0:
            break
        sc = j - 1 if j > 0 and d[j - 1] == 0 else j
        out.append((sc, j + 3, (d[j + 3] >> 1) & 0x3F))
        i = j + 3
    return out


def poc_lsb(rbsp, irap):
    """slice_pic_order_cnt_lsb out of a slice header (7.3.6.1).

    Ahead of it: first_slice_segment_in_pic_flag, then - only for an IRAP
    - no_output_of_prior_pics_flag, then two ue(v), the PPS id and the
    slice type. Emulation prevention cannot reach these bytes (it takes
    two zero bytes to arm) but is stripped anyway."""
    b = rbsp.replace(b"\x00\x00\x03", b"\x00\x00")
    bits = "".join(f"{x:08b}" for x in b)
    p = 2 if irap else 1
    for _ in range(2):                      # pps id, slice type
        z = 0
        while bits[p + z] == "0":
            z += 1
        p += 2 * z + 1
    return int(bits[p:p + 8], 2)


def cut(d, want, types=(CRA_NUT,)):
    """Cut at the `want`th picture whose NAL type is in `types` (a CRA).

    Returns (bytes, cra poc lsb, radl count, kept count) - `kept` being
    how many pictures a decoder starting here is required to OUTPUT: the
    VCL NALs from the cut on, minus the RASL pictures it must discard."""
    ns = nals(d)
    first_vcl = next((k for k, (_, _, t) in enumerate(ns) if t <= VCL_MAX),
                     len(ns))
    heads = b""
    for k in range(first_vcl):
        if ns[k][2] in PARAM:
            end = ns[k + 1][0] if k + 1 < len(ns) else len(d)
            heads += d[ns[k][0]:end]
    seen = 0
    for k, (sc, po, t) in enumerate(ns):
        if t not in types or (po + 2 < len(d) and not d[po + 2] & 0x80):
            continue
        seen += 1
        if seen != want:
            continue
        b = k
        while b > 0 and ns[b - 1][2] in AU_HEAD:
            b -= 1
        # ...and under --repeat-headers the access unit already carries
        # the parameter sets, so prepending them again would only make a
        # decoder parse the same three RBSPs twice
        have_sets = any(t2 in PARAM for _, _, t2 in ns[b:k])
        # The leading pictures of THIS CRA run until the next IRAP. Only
        # ITS RASL pictures are discarded: NoRaslOutputFlag is 1 for the
        # CRA the decode starts at and 0 for every later one, so the
        # later CRAs' RASL pictures decode and are output like any other
        # leading picture (8.1.3).
        radl = drop = 0
        for _, _, t2 in ns[k + 1:]:
            if t2 in IRAP:
                break
            if t2 in (RADL_N, RADL_R):
                radl += 1
            elif t2 in (RASL_N, RASL_R):
                drop += 1
        kept = sum(1 for _, _, t2 in ns[k:] if t2 <= VCL_MAX) - drop
        # an IDR's slice header carries no POC: it is 0 by definition
        poc = poc_lsb(d[po + 2:po + 12], True) if t == CRA_NUT else 0
        return ((b"" if have_sets else heads) + d[ns[b][0]:], poc, radl,
                kept)
    return None, None, None, None


def frame_bytes(w, h, csp, depth):
    csx = 0 if csp == "444" else 1
    csy = 1 if csp == "420" else 0
    luma = w * h
    chroma = 2 * ((w >> csx) * (h >> csy))
    return (luma + chroma) * (2 if depth > 8 else 1)


def frames(path, fs):
    d = open(path, "rb").read()
    return [d[i:i + fs] for i in range(0, len(d) - fs + 1, fs)]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("stream")
    ap.add_argument("full")
    ap.add_argument("--geometry", required=True)
    ap.add_argument("--csp", default="420", choices=("420", "422", "444"))
    ap.add_argument("--depth", type=int, default=8, choices=(8, 10))
    ap.add_argument("--cra", type=int, default=2,
                    help="which CRA to seek to (1-based); falls back to the "
                         "last one present")
    ap.add_argument("--ffmpeg", default=os.environ.get("FFMPEG", "ffmpeg"))
    ap.add_argument("--dec265", default=os.environ.get("DEC265", "dec265"))
    ap.add_argument("--keep", help="write the cut stream and decodes here")
    ap.add_argument("--expect-radl", type=int, default=0,
                    help="fail when the encode named --radl but the cut has no RADL")
    ap.add_argument("--all-irap", action="store_true",
                    help="cut at every IRAP, IDR or CRA, and check each")
    ap.add_argument("--expect-starts",
                    help="with --all-irap: display frames that must each "
                         "start a cut, comma-separated")
    a = ap.parse_args()

    w, h = (int(x) for x in a.geometry.lower().split("x"))
    fs = frame_bytes(w, h, a.csp, a.depth)
    pix = {("420", 8): "yuv420p", ("422", 8): "yuv422p", ("444", 8): "yuv444p",
           ("420", 10): "yuv420p10le", ("422", 10): "yuv422p10le",
           ("444", 10): "yuv444p10le"}[(a.csp, a.depth)]

    d = open(a.stream, "rb").read()
    if a.all_irap:
        return all_irap(a, d, fs, pix)
    ncra = sum(1 for _, _, t in nals(d) if t == CRA_NUT)
    if ncra == 0:
        print("seek_check: no CRA in the stream - nothing to seek to",
              file=sys.stderr)
        return 2
    which = min(a.cra, ncra)
    stream, cra, radl, kept = cut(d, which)
    if a.expect_radl > 0 and radl == 0:
        print(f"seek_check: the encode asked for --radl {a.expect_radl} and the "
              f"cut has no RADL picture, so this check proves nothing about them",
              file=sys.stderr)
        return 1

    full = frames(a.full, fs)
    # What a decoder starting here has to output, counted off the CUT
    # STREAM and not off any decoder: every picture from the CRA on,
    # minus the RASL pictures 8.1.3 makes it discard. Those pictures are
    # a contiguous run at the END of display order - the CRA's leading
    # pictures are the display positions immediately before it and its
    # trailing pictures everything after - so the reference is the tail
    # of the full decode, and the count is what says where the tail
    # starts. Counting rather than reading the CRA's POC is what keeps
    # this right across the 8-bit pic_order_cnt_lsb wrap and across a
    # mid-stream IDR, both of which break POC-as-display-index.
    start = len(full) - kept
    if start < 0:
        print(f"seek_check: the cut stream has {kept} output pictures and "
              f"the full decode only {len(full)}", file=sys.stderr)
        return 2
    want = full[start:]

    tmp = a.keep or tempfile.mkdtemp(prefix="seekchk")
    os.makedirs(tmp, exist_ok=True)
    cutpath = os.path.join(tmp, "seek.265")
    ffout = os.path.join(tmp, "seek.ff.yuv")
    deout = os.path.join(tmp, "seek.de.yuv")
    open(cutpath, "wb").write(stream)

    # libavcodec 8.0 to 8.1.2 drop a picture whose RPS keeps (but never
    # uses) one missing after a seek, which a RADL may legally do. With
    # output_corrupt they decode it; a RADL that really predicted from a
    # missing picture would read a generated one and fail the comparison.
    r = subprocess.run([a.ffmpeg, "-loglevel", "error", "-flags", "output_corrupt",
                        "-i", cutpath,
                        "-pix_fmt", pix, "-f", "rawvideo", ffout, "-y"],
                       capture_output=True)
    if r.returncode != 0 or not os.path.exists(ffout):
        print("seek_check: ffmpeg refused the cut stream: "
              + r.stderr.decode()[:200], file=sys.stderr)
        return 1
    got = frames(ffout, fs)
    if got != want:
        n = sum(1 for x, y in zip(got, want) if x != y)
        print(f"seek_check: ffmpeg gave {len(got)} frames, wanted "
              f"{len(want)} from display {start}; {n} of the overlap "
              f"differ", file=sys.stderr)
        return 1

    r = subprocess.run([a.dec265, "-q", "-o", deout, cutpath],
                       capture_output=True)
    if r.returncode != 0 or not os.path.exists(deout):
        print("seek_check: dec265 refused the cut stream: "
              + r.stderr.decode()[:200], file=sys.stderr)
        return 1
    dg = frames(deout, fs)
    hs = lambda fr: sorted(hashlib.md5(x).hexdigest() for x in fr)
    if hs(dg) != hs(want):
        print(f"seek_check: dec265 gave {len(dg)} frames, wanted "
              f"{len(want)} from display {start} (compared as a multiset: "
              f"dec265 -o emits decode order)", file=sys.stderr)
        return 1

    print(f"seek_check: CRA {which}/{ncra} at POC lsb {cra}, {radl} RADL, "
          f"{len(want)} frames from display {start}, both decoders exact")
    return 0


def decode_check(a, stream, want, fs, pix, tmp):
    """Both decoders on a cut stream against `want`; None, or the failure."""
    cutpath = os.path.join(tmp, "seek.265")
    ffout = os.path.join(tmp, "seek.ff.yuv")
    deout = os.path.join(tmp, "seek.de.yuv")
    open(cutpath, "wb").write(stream)
    r = subprocess.run([a.ffmpeg, "-loglevel", "error", "-flags", "output_corrupt",
                        "-i", cutpath, "-pix_fmt", pix, "-f", "rawvideo", ffout,
                        "-y"], capture_output=True)
    if r.returncode != 0 or not os.path.exists(ffout):
        return "ffmpeg refused the cut stream: " + r.stderr.decode()[:200]
    if frames(ffout, fs) != want:
        return f"ffmpeg did not give the {len(want)} expected frames"
    r = subprocess.run([a.dec265, "-q", "-o", deout, cutpath],
                       capture_output=True)
    if r.returncode != 0 or not os.path.exists(deout):
        return "dec265 refused the cut stream: " + r.stderr.decode()[:200]
    hs = lambda fr: sorted(hashlib.md5(x).hexdigest() for x in fr)
    if hs(frames(deout, fs)) != hs(want):
        return f"dec265 did not give the {len(want)} expected frames"
    return None


def all_irap(a, d, fs, pix):
    full = frames(a.full, fs)
    n = sum(1 for _, po, t in nals(d)
            if t in IRAP and po + 2 < len(d) and d[po + 2] & 0x80)
    tmp = a.keep or tempfile.mkdtemp(prefix="seekchk")
    os.makedirs(tmp, exist_ok=True)
    starts = []
    for k in range(1, n + 1):
        stream, _, _, kept = cut(d, k, tuple(IRAP))
        start = len(full) - kept
        if start < 0:
            print(f"seek_check: IRAP {k}: {kept} output pictures, the full "
                  f"decode has {len(full)}", file=sys.stderr)
            return 2
        why = decode_check(a, stream, full[start:], fs, pix, tmp)
        if why:
            print(f"seek_check: IRAP {k}/{n} at display {start}: {why}",
                  file=sys.stderr)
            return 1
        starts.append(start)
    miss = []
    if a.expect_starts:
        want = [int(x) for x in a.expect_starts.split(",") if x.strip()]
        miss = [x for x in want if x not in starts]
    if miss:
        print(f"seek_check: no IRAP starts at display {miss}; the IRAPs start "
              f"at {starts}", file=sys.stderr)
        return 1
    print(f"seek_check: {n} IRAPs, both decoders exact from each; they start "
          f"at display {' '.join(map(str, starts))}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
