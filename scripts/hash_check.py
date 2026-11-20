#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Check a stream's decoded picture hash SEIs against the pictures.

Phase 14 item 1. The encoder writes one decoded_picture_hash message per
picture (H.265 D.2.19) and claims it is the MD5, CRC or checksum of the
coded picture as D.3.19 defines them. Only ONE of those three claims has
a decoder behind it on this box - ffmpeg verifies the MD5 form under
`-err_detect crccheck` and libde265's `-c` is inert in the build we have -
so the other two need an oracle of their own, and this is it: a second
implementation of D.3.19, written from the syntax table rather than from
the encoder's code, fed the pictures a DECODER produced.

It is a real oracle because it can be pointed at x265: run it on an x265
stream and its own hashes, and agreement says this file reads the spec
the way the reference encoder does. Disagreement on our streams after
that is ours.

Two things it does not check, and does not pretend to:
  - order. The SEIs arrive in CODING order and a YUV file is in DISPLAY
    order, so the comparison is a MULTISET unless --in-order says the
    stream has no reordering in it (all-intra, or --bframes 0).
  - padding. D.3.19 hashes the coded picture, so a clip whose dimensions
    are not a multiple of the minimum coding block (8) has padding in the
    hash that the decoder's cropped output does not carry. Give such a
    clip the padded geometry, or leave it to ffmpeg's own check.

usage: hash_check.py STREAM.hevc RECON.yuv --geometry WxH
                     [--csp 420|422|444] [--depth 8|10] [--in-order]
"""

import argparse
import hashlib
import sys


# ---------------------------------------------------------------- stream

def nal_units(data):
    """Annex B: yield (nal_unit_type, rbsp) with emulation prevention
    undone."""
    i = 0
    starts = []
    while True:
        j = data.find(b"\x00\x00\x01", i)
        if j < 0:
            break
        starts.append(j + 3)
        i = j + 3
    for k, s in enumerate(starts):
        e = len(data) if k + 1 == len(starts) else starts[k + 1] - 3
        while e > s and data[e - 1] == 0:
            e -= 1
        if e - s < 2:
            continue
        nut = (data[s] >> 1) & 0x3F
        payload = data[s + 2:e]
        rbsp = bytearray()
        z = 0
        for b in payload:
            if z >= 2 and b == 3:
                z = 0
                continue
            rbsp.append(b)
            z = z + 1 if b == 0 else 0
        yield nut, bytes(rbsp)


def sei_messages(rbsp):
    """sei_message() loop of D.2.1: yield (payload_type, payload)."""
    i = 0
    n = len(rbsp)
    while i < n:
        t = 0
        while i < n and rbsp[i] == 255:
            t += 255
            i += 1
        if i >= n:
            return
        t += rbsp[i]
        i += 1
        size = 0
        while i < n and rbsp[i] == 255:
            size += 255
            i += 1
        if i >= n:
            return
        size += rbsp[i]
        i += 1
        if i + size > n:
            return
        yield t, rbsp[i:i + size]
        i += size
        # the rbsp_trailing_bits after the last message
        if i < n and rbsp[i] == 0x80:
            return


def signalled_hashes(path):
    """Every decoded_picture_hash message in the file, in stream order."""
    data = open(path, "rb").read()
    out = []
    for nut, rbsp in nal_units(data):
        if nut != 40:               # SUFFIX_SEI_NUT
            continue
        for t, p in sei_messages(rbsp):
            if t != 132 or not p:
                continue
            htype = p[0]
            vals = []
            q = 1
            if htype == 0:
                for _ in range(3):
                    vals.append(p[q:q + 16].hex())
                    q += 16
            elif htype == 1:
                for _ in range(3):
                    vals.append("%04x" % int.from_bytes(p[q:q + 2], "big"))
                    q += 2
            else:
                for _ in range(3):
                    vals.append("%08x" % int.from_bytes(p[q:q + 4], "big"))
                    q += 4
            out.append((htype, tuple(vals)))
    return out


# ----------------------------------------------------------------- D.3.19

def plane_bytes(plane, w, h, depth):
    """D.3.19's pictureData for one plane: one byte per sample at 8 bits,
    the low byte then the high one above it. `plane` is already that
    layout when it came off disk, so this is the identity for a raw
    file - it exists to say so."""
    return plane


def md5_plane(plane):
    return hashlib.md5(plane).hexdigest()


def crc_plane(plane):
    crc = 0xFFFF
    for byte in plane:
        for b in range(7, -1, -1):
            msb = (crc >> 15) & 1
            bit = (byte >> b) & 1
            crc = (((crc << 1) + bit) & 0xFFFF) ^ (0x1021 if msb else 0)
    for _ in range(16):
        msb = (crc >> 15) & 1
        crc = ((crc << 1) & 0xFFFF) ^ (0x1021 if msb else 0)
    return "%04x" % crc


def checksum_plane(plane, w, h, depth):
    total = 0
    wide = depth > 8
    for y in range(h):
        row = plane[y * w * (2 if wide else 1):(y + 1) * w * (2 if wide else 1)]
        ymask = (y & 0xFF) ^ (y >> 8)
        for x in range(w):
            mask = ((x & 0xFF) ^ (x >> 8) ^ ymask) & 0xFF
            if wide:
                total = (total + ((row[2 * x] ^ mask) & 0xFF)) & 0xFFFFFFFF
                total = (total + ((row[2 * x + 1] ^ mask) & 0xFF)) & 0xFFFFFFFF
            else:
                total = (total + ((row[x] ^ mask) & 0xFF)) & 0xFFFFFFFF
    return "%08x" % total


def frame_hashes(f, w, h, csx, csy, depth, htype):
    """One frame off a planar raw file, or None at EOF."""
    bpp = 2 if depth > 8 else 1
    vals = []
    for i in range(3):
        pw = w >> csx if i else w
        ph = h >> csy if i else h
        plane = f.read(pw * ph * bpp)
        if len(plane) < pw * ph * bpp:
            return None
        plane = plane_bytes(plane, pw, ph, depth)
        if htype == 0:
            vals.append(md5_plane(plane))
        elif htype == 1:
            vals.append(crc_plane(plane))
        else:
            vals.append(checksum_plane(plane, pw, ph, depth))
    return tuple(vals)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("stream")
    ap.add_argument("recon")
    ap.add_argument("--geometry", required=True, help="WxH of the CODED picture")
    ap.add_argument("--csp", default="420", choices=("420", "422", "444"))
    ap.add_argument("--depth", type=int, default=8, choices=(8, 10))
    ap.add_argument("--in-order", action="store_true",
                    help="coding order is display order: compare position "
                         "by position instead of as a multiset")
    a = ap.parse_args()

    w, h = (int(v) for v in a.geometry.lower().split("x"))
    csx = 0 if a.csp == "444" else 1
    csy = 1 if a.csp == "420" else 0

    sig = signalled_hashes(a.stream)
    if not sig:
        print("hash_check: no decoded picture hash SEI in %s" % a.stream)
        return 1
    types = {t for t, _ in sig}
    if len(types) != 1:
        print("hash_check: the stream mixes hash types %s" % sorted(types))
        return 1
    htype = types.pop()

    got = []
    with open(a.recon, "rb") as f:
        while True:
            v = frame_hashes(f, w, h, csx, csy, a.depth, htype)
            if v is None:
                break
            got.append(v)

    name = {0: "MD5", 1: "CRC", 2: "checksum"}[htype]
    if len(got) != len(sig):
        print("hash_check: %d %s messages against %d pictures in %s"
              % (len(sig), name, len(got), a.recon))
        return 1

    if a.in_order:
        bad = [i for i, (s, g) in enumerate(zip([v for _, v in sig], got))
               if s != g]
        if bad:
            i = bad[0]
            print("hash_check: %s mismatch at picture %d: signalled %s, "
                  "computed %s (%d of %d wrong)"
                  % (name, i, sig[i][1], got[i], len(bad), len(got)))
            return 1
    else:
        want = sorted(v for _, v in sig)
        have = sorted(got)
        if want != have:
            extra = [v for v in want if v not in have]
            print("hash_check: %s mismatch: %d of %d signalled hashes are "
                  "not any picture's, e.g. %s"
                  % (name, len(extra), len(want), extra[:1]))
            return 1

    print("hash_check: %d pictures, %s, all correct%s"
          % (len(got), name, " (in order)" if a.in_order else ""))
    return 0


if __name__ == "__main__":
    sys.exit(main())
