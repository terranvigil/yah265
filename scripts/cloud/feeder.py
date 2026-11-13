#!/usr/bin/env python3
# Copyright (c) 2026, the yah265 authors
# SPDX-License-Identifier: GPL-2.0-or-later
"""feeder.py -- the one input path both encoders read on a cloud box.

The Linux twin of lf_feed.py. It decodes a compressed source (a master,
or the Mac's frames packed losslessly by media_pack.py) with ffmpeg in
software, drops the frames before the cell, and writes a Y4M stream on
stdout under the header the Mac's feeder wrote for that cell, so both
encoders see the Mac's stream byte for byte where the frames are the
Mac's.

A cell that starts deep in a master would otherwise put the decode of
everything before it into the encoder's wall. So the feeder decodes up
to the cell's first frame, holds it, and only then touches --ready (a
path); the harness starts the encoder and its clock on that file, as
the Mac's wall starts with a feeder that seeks in milliseconds.

    feeder.py --src F --hdr "YUV4MPEG2 ..." --skip N --count C
              [--vf FILTER] [--threads T] [--ready PATH] [--md5 PATH]
              [--out PATH]

--md5 writes the md5 of every plane byte sent (untimed checks only).
--out writes the stream to a file instead of stdout (staging windows).
"""
import argparse
import hashlib
import os
import subprocess
import sys


def main():
    p = argparse.ArgumentParser()
    p.add_argument("--src", required=True)
    p.add_argument("--hdr", required=True)
    p.add_argument("--skip", type=int, default=0)
    p.add_argument("--count", type=int, required=True)
    p.add_argument("--vf")
    p.add_argument("--threads", default="0")
    p.add_argument("--ready")
    p.add_argument("--md5")
    p.add_argument("--out")
    p.add_argument("--seek", type=float, default=0.0,
                   help="input seek in seconds before --skip (intra-only "
                        "sources, or a lossless pack with known keyframes)")
    a = p.parse_args()
    tags = a.hdr.split()
    w = int(next(t[1:] for t in tags if t[0] == "W"))
    h = int(next(t[1:] for t in tags if t[0] == "H"))
    fs = w * h * 3 // 2
    cmd = ["ffmpeg", "-v", "error", "-nostdin", "-threads", a.threads]
    if a.seek:
        cmd += ["-ss", f"{a.seek:.6f}"]
    if os.path.isdir(a.src):
        # media_pack's chunked pack: the chunks in order, one stream
        lst = os.path.join(a.src, f"concat.{os.getpid()}.txt")
        with open(lst, "w") as f:
            for c in sorted(x for x in os.listdir(a.src)
                            if x.endswith(".mkv")):
                f.write(f"file '{os.path.join(a.src, c)}'\n")
        cmd += ["-f", "concat", "-safe", "0", "-i", lst]
    else:
        cmd += ["-i", a.src]
    cmd += ["-map", "0:v:0", "-an", "-sn", "-dn"]
    if a.vf:
        cmd += ["-vf", a.vf]
    cmd += ["-fps_mode", "passthrough", "-pix_fmt", "yuv420p",
            "-f", "rawvideo", "-"]
    dec = subprocess.Popen(cmd, stdout=subprocess.PIPE, bufsize=0)
    if os.path.isdir(a.src):
        import atexit
        atexit.register(lambda: os.path.exists(lst) and os.remove(lst))
    src = dec.stdout

    def frame():
        buf = bytearray(fs)
        mv = memoryview(buf)
        got = 0
        while got < fs:
            n = src.readinto(mv[got:])
            if not n:
                return None
            got += n
        return buf

    for i in range(a.skip):
        if frame() is None:
            sys.exit(f"feeder: source ended at {i} of {a.skip} skipped")
    first = frame()
    if first is None:
        sys.exit("feeder: no first frame")
    out = open(a.out, "wb") if a.out else os.fdopen(1, "wb", buffering=0)
    md = hashlib.md5() if a.md5 else None
    if a.ready:
        with open(a.ready, "w") as f:
            f.write("ready\n")
    out.write(a.hdr.encode() + b"\n")
    sent = 0
    f = first
    try:
        while True:
            out.write(b"FRAME\n")
            out.write(f)
            if md:
                md.update(f)
            sent += 1
            if sent == a.count:
                break
            f = frame()
            if f is None:
                sys.exit(f"feeder: source ended after {sent} of {a.count}")
    except BrokenPipeError:
        sys.exit(f"feeder: reader closed after {sent} frames")
    out.close()
    dec.kill()
    dec.wait()
    if md:
        with open(a.md5, "w") as fo:
            fo.write(f"{md.hexdigest()} {sent}\n")


if __name__ == "__main__":
    main()
