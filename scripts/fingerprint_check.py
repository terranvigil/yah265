#!/usr/bin/env python3
"""Read a stream's provenance marks: the identification SEI and the fingerprint.

Phase 14 item 10. Two marks, and the point of the second one is that it
survives losing the first:

  identification  a user_data_unregistered SEI (D.2.6) under this
                  project's UUID, naming the encoder, its version, its
                  licence, its URL and the options the run resolved. Any
                  remux or one-line NAL filter drops it; `--strip-sei`
                  here does exactly that, to prove the point.

  fingerprint     a signature in a choice the syntax leaves open. Where
                  the merge candidate list holds the SAME MOTION at both
                  of its last two indices, merge_idx may name either of
                  them: the two codings have the same length, the same
                  context-coded prefix, and they reconstruct the same
                  picture. So which one is written is free, and a fixed
                  pattern in that choice names the encoder without
                  spending a bit. docs/stream-fingerprint.md.

The first mark this script reads on its own - it is plain Annex-B and
plain RBSP parsing. The second one lives inside CABAC-coded slice data,
so reading it needs a decoder, and this script drives one: libde265's
`dec265` built with DE265_LOG_TRACE, which prints every merge_idx it
decodes and the candidate list it derived. `--dec265` or $DEC265_TRACE
names it; without one the fingerprint leg says so instead of guessing.

A decoder derives only as many merge candidates as the index it read
asks for, so the duplicate pair is visible from the HIGH index and not
from the low one. The signature is keyed on the site's position rather
than on a running count for exactly that reason: every mark this script
sees is independent, and the test is that the positions carrying a 1 are
a subset of the signature's 1 bits.

The tracing decoder is not cheap: it prints a line per syntax element,
so a 720p clip is tens of millions of lines and minutes of wall. The mark
accumulates fast - a CIF clip reads in 30-60 frames - so on HD pass
`--frames`.

usage: fingerprint_check.py [options] stream.265
"""

import argparse
import os
import re
import shutil
import subprocess
import sys
import tempfile

# --- the marks, and they are one table -------------------------------------
# The UUID of the identification SEI (src/encoder/headers.c). Its first
# three bytes are also bits 0..23 of the fingerprint's signature, which is
# what makes the two marks one scheme.
UUID = bytes([0x7A, 0xA4, 0x16, 0x3D, 0xC8, 0x5E, 0x4F, 0x71,
              0x9D, 0x2C, 0x6B, 0x08, 0xE3, 0x51, 0xA7, 0x94])

FP_BITS = 32
FP_KEY_BITS = 24        # the UUID prefix: fixed for the life of the format
SIG_KEY = UUID[0] | (UUID[1] << 8) | (UUID[2] << 16)

# Versions this reader knows how to name, packed the way src/encoder/fp.c
# packs them: major 3 bits, minor 3, patch 2.
KNOWN_VERSIONS = {
    ((0 & 7) << 5) | ((0 & 7) << 2) | (1 & 3): "0.0.1",
}

# What it takes to call the fingerprint present: this many marks read,
# every one of them at a position the signature sets, and this many of
# the signature's own 1 positions covered. The discriminating number is
# the first one - an encoder choosing the high index for its own reasons
# lands on a 0 position 13 times in 24, so twenty clean marks is about
# one in a hundred thousand - and the coverage rule is what stops a
# handful of lucky marks from reading as a match.
DEFAULT_MIN_MARKS = 20
DEFAULT_MIN_POSITIONS = 8

M32 = 0xFFFFFFFF


def fp_position(poc_lsb, x0, y0):
    """src/encoder/fp.c's n265_fp_bit, in uint32 arithmetic."""
    z = ((poc_lsb & 255) * 0x9E3779B1) & M32
    z ^= ((x0 & 0xFFFF) * 0x85EBCA6B) & M32
    z ^= ((y0 & 0xFFFF) * 0xC2B2AE35) & M32
    z ^= z >> 15
    z = (z * 0x2545F491) & M32
    z ^= z >> 13
    return z & (FP_BITS - 1)


# --- Annex B ---------------------------------------------------------------

def nal_units(data):
    """Yield (start, end, payload) for each NAL in an Annex-B byte string.

    `start` is the index of the leading zero byte of the start code, so a
    filter can drop the whole unit including its prefix; `payload` is the
    NAL with emulation prevention still in it."""
    i = 0
    n = len(data)
    starts = []
    while i + 2 < n:
        if data[i] == 0 and data[i + 1] == 0 and data[i + 2] == 1:
            pre = 1 if (i > 0 and data[i - 1] == 0) else 0
            starts.append((i - pre, i + 3))
            i += 3
        else:
            i += 1
    for k, (s, b) in enumerate(starts):
        e = starts[k + 1][0] if k + 1 < len(starts) else n
        yield s, e, data[b:e]


def unescape(p):
    """Strip emulation_prevention_three_byte (7.4.2)."""
    out = bytearray()
    zeros = 0
    for b in p:
        if zeros >= 2 and b == 3:
            zeros = 0
            continue
        out.append(b)
        zeros = zeros + 1 if b == 0 else 0
    return bytes(out)


def strip_sei(data):
    """Drop every prefix (39) and suffix (40) SEI NAL. The remux test."""
    out = bytearray()
    for s, e, p in nal_units(data):
        if len(p) < 2:
            continue
        t = (p[0] >> 1) & 0x3F
        if t in (39, 40):
            continue
        out += data[s:e]
    return bytes(out)


def read_identification(data, foreign=None):
    """The user_data_unregistered SEI under our UUID, as text, or None.

    `foreign[0]` is set where the stream carries one under some other
    UUID, which is a different answer from carrying none at all."""
    if foreign is None:
        foreign = [False]
    for _s, _e, p in nal_units(data):
        if len(p) < 3:
            continue
        if ((p[0] >> 1) & 0x3F) != 39:
            continue
        rb = unescape(p)[2:]        # past the two-byte NAL header
        i = 0
        while i < len(rb):
            ptype = 0
            while i < len(rb) and rb[i] == 0xFF:
                ptype += 255
                i += 1
            if i >= len(rb):
                break
            ptype += rb[i]
            i += 1
            psize = 0
            while i < len(rb) and rb[i] == 0xFF:
                psize += 255
                i += 1
            if i >= len(rb):
                break
            psize += rb[i]
            i += 1
            body = rb[i:i + psize]
            i += psize
            if ptype == 5 and len(body) >= 16:
                if body[:16] == UUID:
                    return body[16:].decode("utf-8", "replace")
                foreign[0] = True   # somebody else's unregistered data
    return None


# --- the fingerprint, through a tracing decoder ----------------------------

RE_CTB = re.compile(r"^----- decode CTB (\d+);(\d+) .*POC=(-?\d+)")
RE_QT = re.compile(r"^- read_coding_quadtree (\d+);(\d+) cbsize:(\d+)")
RE_PU = re.compile(r"prediction unit (\d+),(\d+), merge mode, index: (\d+)")
RE_SKIP = re.compile(r"prediction skip 2Nx2N, merge_idx: (\d+)")
RE_LIST = re.compile(r"^mergeCandList:")
RE_CAND = re.compile(r"^  ([01]): (yes|no ) +(-?\d+|--);(-?\d+|--) ref=(-?\d+|--)")


class Site:
    __slots__ = ("poc", "x", "y", "idx", "cands")

    def __init__(self, poc, x, y, idx, cands):
        self.poc, self.x, self.y = poc, x, y
        self.idx, self.cands = idx, cands


def find_dec265(explicit):
    for c in (explicit, os.environ.get("DEC265_TRACE"),
              os.path.expanduser("~/src/libde265/build-trace/dec265/dec265")):
        if c and os.path.isfile(c) and os.access(c, os.X_OK):
            return c
    return shutil.which("dec265")


def trace_sites(dec, path, frames):
    """Every merge PU the decoder read, with the candidate list it derived.

    The decoder logs, per merge PU: the chosen merge_idx, then (after the
    derivation) `mergeCandList:` and MaxNumMergeCand candidates, two lines
    each. It derives only merge_idx+1 of them, so entries past that are
    stale - which is fine, because the only pair this reader tests is the
    last one, and that pair is complete exactly when the index named it.
    """
    cmd = [dec, "-q", "-v", "-v", "-v", path]
    if frames:
        cmd[1:1] = ["-f", str(frames)]
    # dec265's trace goes to stdout; its warnings go to stderr
    r = subprocess.run(cmd, stdout=subprocess.PIPE,
                       stderr=subprocess.DEVNULL)
    sites = []
    poc = 0
    cux = cuy = 0
    pend = None         # (x, y, idx) waiting for its candidate list
    cands = None
    def flush():
        nonlocal pend, cands
        if pend is not None and cands:
            # two lines per candidate, L0 then L1
            grouped = [tuple(cands[i:i + 2])
                       for i in range(0, len(cands) - 1, 2)]
            sites.append(Site(poc, pend[0], pend[1], pend[2], grouped))
        pend, cands = None, None

    for line in r.stdout.decode("utf-8", "replace").splitlines():
        if cands is not None:
            if RE_CAND.match(line):
                cands.append(line[3:].strip())
                continue
            flush()
        m = RE_CTB.match(line)
        if m:
            cux, cuy, poc = int(m.group(1)), int(m.group(2)), int(m.group(3))
            continue
        m = RE_QT.match(line)
        if m:
            cux, cuy = int(m.group(1)), int(m.group(2))
            continue
        m = RE_PU.search(line)
        if m:
            pend = (int(m.group(1)), int(m.group(2)), int(m.group(3)))
            continue
        m = RE_SKIP.search(line)
        if m:
            pend = (cux, cuy, int(m.group(1)))
            continue
        if RE_LIST.match(line):
            cands = []
            continue
    flush()
    return sites, r.returncode


def read_fingerprint(sites, min_marks, min_positions):
    """Collect the marks. A mark is a site whose merge_idx is the LAST
    candidate index and whose last two candidates are the same motion:
    both of those are things the decoder's own derivation shows, so the
    reader never has to know what the encoder was thinking.

    Only the high index is readable - a decoder derives merge_idx+1
    candidates and no more - so a 0 bit looks exactly like no site at
    all. That is why the test is one-sided: every mark must land on a
    position the signature SETS, and enough of them must."""
    marks = 0
    ones = set()
    n_high = 0
    for s in sites:
        if len(s.cands) < 2:
            continue
        last = len(s.cands) - 1
        if s.idx != last:
            continue            # the low half of the pair is not readable
        n_high += 1
        if s.cands[last] != s.cands[last - 1]:
            continue            # not a duplicate pair, so not a site
        marks += 1
        ones.add(fp_position(s.poc & 255, s.x, s.y))
    key_ones = {p for p in ones if p < FP_KEY_BITS}
    bad = {p for p in key_ones if not (SIG_KEY >> p) & 1}
    ver_ones = {p - FP_KEY_BITS for p in ones if p >= FP_KEY_BITS}
    want = min(min_positions, bin(SIG_KEY).count("1"))
    return {
        "sites_high": n_high,
        "marks": marks,
        "key_positions": sorted(key_ones),
        "key_mismatch": sorted(bad),
        "version_bits": sorted(ver_ones),
        "match": (not bad) and marks >= min_marks and len(key_ones) >= want,
    }


def name_version(bits):
    """The version byte is only ever seen through its 1 bits, so the
    answer is every known version those bits are consistent with."""
    seen = 0
    for b in bits:
        seen |= 1 << b
    out = [n for v, n in KNOWN_VERSIONS.items() if (v & seen) == seen]
    return out


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("stream")
    ap.add_argument("--dec265", help="a dec265 built with DE265_LOG_TRACE")
    ap.add_argument("--strip-sei", action="store_true",
                    help="drop every SEI NAL first, as a remux would")
    ap.add_argument("--frames", type=int, default=0)
    ap.add_argument("--min-marks", type=int, default=DEFAULT_MIN_MARKS)
    ap.add_argument("--min-positions", type=int,
                    default=DEFAULT_MIN_POSITIONS)
    ap.add_argument("-v", "--verbose", action="store_true")
    a = ap.parse_args()

    data = open(a.stream, "rb").read()
    path = a.stream
    tmp = None
    if a.strip_sei:
        data = strip_sei(data)
        tmp = tempfile.NamedTemporaryFile(suffix=".265", delete=False)
        tmp.write(data)
        tmp.close()
        path = tmp.name

    try:
        foreign = [False]
        ident = read_identification(data, foreign)
        if ident:
            print("identification: %s" % ident)
        elif foreign[0]:
            print("identification: none (the stream carries another "
                  "encoder's unregistered SEI)")
        else:
            print("identification: none")

        dec = find_dec265(a.dec265)
        if not dec:
            print("fingerprint:    unread (no tracing dec265; set "
                  "$DEC265_TRACE or pass --dec265)")
            return 2
        sites, rc = trace_sites(dec, path, a.frames)
        if not sites:
            print("fingerprint:    unread (the decoder traced no merge "
                  "candidate lists; is it built with DE265_LOG_TRACE?)")
            return 2
        r = read_fingerprint(sites, a.min_marks, a.min_positions)
        if r["match"]:
            vs = name_version(r["version_bits"])
            v = ("version " + "/".join(vs)) if vs else "version unknown"
            print("fingerprint:    yah265 (HEVC), %s" % v)
        else:
            print("fingerprint:    no fingerprint")
        if a.verbose or not r["match"]:
            print("  merge PUs at the last index: %d" % r["sites_high"])
            print("  marks read: %d, at signature positions %s"
                  % (r["marks"], r["key_positions"]))
            if r["key_mismatch"]:
                print("  positions that are 0 in the signature: %s"
                      % r["key_mismatch"])
            if r["version_bits"]:
                print("  version bits seen: %s" % r["version_bits"])
            if rc != 0:
                print("  (decoder exit %d)" % rc)
        return 0 if r["match"] else 1
    finally:
        if tmp:
            os.unlink(tmp.name)


if __name__ == "__main__":
    sys.exit(main())
