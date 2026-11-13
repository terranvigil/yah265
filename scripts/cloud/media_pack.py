#!/usr/bin/env python3
# Copyright (c) 2026, the yah265 authors
# SPDX-License-Identifier: GPL-2.0-or-later
"""media_pack.py -- the Mac's exact source frames, sent to the cloud hub.

The tier M segments and the tier L 720p and 1080p excerpts exist only as
the Mac's own Y4M files (the 1080p and 720p ones were scaled by
VideoToolbox, which no Linux box can redo). Raw they are 600 GB, too much
for the uplink, so each cell is coded losslessly (x264 --qp 0, 8-bit
4:2:0 kept) on the way out and decoded on the timing box. The frames the
box feeds are then the Mac's frames byte for byte, which the manifest
proves: the md5 of every plane byte of the cell, taken here as the frames
are read, against the same md5 taken on the box after the decode.

    media_pack.py list                 the items
    media_pack.py send NAME... --to HOST:DIR --ssh "ssh -i key ..."
    media_pack.py md5 NAME...          the plane md5 only (no encode)

Nothing lands on the Mac's disk: Y4M -> x264 -> ssh -> the hub's file.
"""
import argparse
import hashlib
import json
import os
import shlex
import subprocess
import sys
import tempfile
import time

TM = os.path.expanduser("~/media/throughput")
STAGE = [os.path.expanduser("~/media/staging-y4m"),
         "<staging>/yah264/y4m"]
# tier L: cell length from parity_long's TITLES (last story frame + 1)
L_FRAMES = {"tears": 12672, "meridian": 32887, "stem2": 12940,
            "sintel": 13178, "bbb": 13929, "cosmos": 13724,
            "nocturne": 34320}


def items():
    it = {}
    for t, f in (("chimera", "chimera_{s}_120s_from120.y4m"),
                 ("sollevante", "sollevante_{s}_120s_from193.y4m"),
                 ("meridian", "meridian_{s}_90s_from1782.y4m")):
        for s in ("1080p", "2160p"):
            it[f"M_{t}_{s}"] = (os.path.join(TM, f.format(s=s)), 0, None)
    for s in ("1080p", "720p"):
        for t, n in L_FRAMES.items():
            p = next((os.path.join(d, f"{t}-x10_{s}.y4m") for d in STAGE
                      if os.path.exists(os.path.join(d, f"{t}-x10_{s}.y4m"))),
                     None)
            it[f"L_{t}_{s}"] = (p, 0, n)
    ts = os.path.expanduser("~/src/yah265/scratch/tier_s")
    for nm, f in (("S_bbb_2160p", "src_bbb4k_300.y4m"),
                  ("S_meridian_2160p", "src_meridian_modern_600.y4m")):
        it[nm] = (os.path.join(ts, f), 0, None)
    for t in ("tears", "sintel"):
        it[f"L_{t}_2160p"] = ("<staging>/yah264/y4m/"
                              f"{t}-x10_2160p.y4m", 0, L_FRAMES[t])
    return it


def header(path):
    with open(path, "rb") as f:
        h = f.readline()
    tags = h.decode().split()
    w = int(next(t[1:] for t in tags if t[0] == "W"))
    ht = int(next(t[1:] for t in tags if t[0] == "H"))
    return h, w, ht


def frames_of(path, start, count, cb):
    """Read frames start..start+count of a Y4M, call cb(planes) for each,
    return (n, md5 of every plane byte)."""
    h, w, ht = header(path)
    fs = w * ht * 3 // 2
    per = fs + 6
    total = (os.path.getsize(path) - len(h)) // per
    count = total - start if count is None else count
    if start + count > total:
        sys.exit(f"{path}: {total} frames, wanted {start}+{count}")
    md = hashlib.md5()
    with open(path, "rb", buffering=0) as f:
        f.seek(len(h) + start * per)
        for i in range(count):
            b = f.read(per)
            if len(b) != per or b[:6] != b"FRAME\n":
                sys.exit(f"{path}: frame {start + i} is not a bare FRAME")
            md.update(b[6:])
            if cb:
                cb(b[6:])
    return count, md.hexdigest()


def send(name, to, ssh, preset):
    path, start, count = items()[name]
    h, w, ht = header(path)
    host, d = to.split(":", 1)
    remote = f"{d}/{name}.mkv"
    enc = subprocess.Popen(
        ["ffmpeg", "-v", "error", "-f", "rawvideo", "-pix_fmt", "yuv420p",
         "-s", f"{w}x{ht}", "-r", "25", "-i", "-", "-c:v", "libx264",
         "-preset", preset, "-qp", "0", "-pix_fmt", "yuv420p", "-threads",
         "4", "-f", "matroska", "-"], stdin=subprocess.PIPE,
        stdout=subprocess.PIPE)
    up = subprocess.Popen(shlex.split(ssh) + [
        host, f"cat > {remote}.part && mv {remote}.part {remote}"],
        stdin=enc.stdout)
    enc.stdout.close()
    t0 = time.time()
    n, md = frames_of(path, start, count, enc.stdin.write)
    enc.stdin.close()
    if enc.wait() != 0 or up.wait() != 0:
        sys.exit(f"{name}: encode or upload failed")
    man = {"name": name, "src": path, "start": start, "frames": n,
           "w": w, "h": ht, "hdr": h.decode().rstrip("\n"),
           "plane_md5": md, "preset": preset,
           "secs": round(time.time() - t0, 1)}
    subprocess.run(shlex.split(ssh) + [host, f"cat > {d}/{name}.json"],
                   input=json.dumps(man).encode(), check=True)
    print(json.dumps(man), flush=True)
    return man


def send_chunked(name, to, ssh, preset, chunk):
    """The same, cut into CHUNK-frame streams under DIR/NAME.d/, each
    whole before it is named; a rerun after a lost hub sends only the
    chunks that aren't there. The plane md5 still covers every frame."""
    path, start, count = items()[name]
    h, w, ht = header(path)
    host, d = to.split(":", 1)
    rd = f"{d}/{name}.d"
    have = set(subprocess.run(shlex.split(ssh) + [
        host, f"mkdir -p {rd}; ls {rd}"], capture_output=True,
        text=True).stdout.split())
    fs = w * ht * 3 // 2
    total = (os.path.getsize(path) - len(h)) // (fs + 6)
    count = total - start if count is None else count
    md = hashlib.md5()
    t0 = time.time()
    k = 0
    for s in range(0, count, chunk):
        n = min(chunk, count - s)
        fn = f"c{k:04d}.mkv"
        k += 1
        if fn in have:
            frames_of(path, start + s, n, md.update)
            continue
        # the chunk is coded to a local file first and named on the hub
        # only once its whole size is there: a dropped connection must not
        # leave a short chunk under the final name
        tmp = os.path.join(tempfile.gettempdir(), f"mp_{name}_{fn}")
        enc = subprocess.Popen(
            ["ffmpeg", "-v", "error", "-y", "-f", "rawvideo", "-pix_fmt",
             "yuv420p", "-s", f"{w}x{ht}", "-r", "25", "-i", "-", "-c:v",
             "libx264", "-preset", preset, "-qp", "0", "-pix_fmt", "yuv420p",
             "-threads", "4", "-f", "matroska", tmp], stdin=subprocess.PIPE)

        def both(b):
            md.update(b)
            enc.stdin.write(b)
        frames_of(path, start + s, n, both)
        enc.stdin.close()
        if enc.wait() != 0:
            sys.exit(f"{name}: chunk {fn} encode failed")
        size = os.path.getsize(tmp)
        with open(tmp, "rb") as f:
            up = subprocess.run(shlex.split(ssh) + [
                host, f"cat > {rd}/{fn}.part && test $(stat -c %s "
                f"{rd}/{fn}.part) = {size} && mv {rd}/{fn}.part {rd}/{fn}"],
                stdin=f)
        os.remove(tmp)
        if up.returncode != 0:
            sys.exit(f"{name}: chunk {fn} upload failed")
    man = {"name": name, "src": path, "start": start, "frames": count,
           "w": w, "h": ht, "hdr": h.decode().rstrip("\n"),
           "plane_md5": md.hexdigest(), "preset": preset, "chunk": chunk,
           "chunks": k, "secs": round(time.time() - t0, 1)}
    subprocess.run(shlex.split(ssh) + [host, f"cat > {d}/{name}.json"],
                   input=json.dumps(man).encode(), check=True)
    print(json.dumps(man), flush=True)
    return man


def main():
    p = argparse.ArgumentParser()
    p.add_argument("cmd", choices=("list", "send", "md5"))
    p.add_argument("names", nargs="*")
    p.add_argument("--to")
    p.add_argument("--ssh", default="ssh")
    p.add_argument("--preset", default="superfast")
    p.add_argument("--chunk", type=int, default=0)
    a = p.parse_args()
    it = items()
    if a.cmd == "list":
        for k, v in it.items():
            print(k, v)
        return
    for nm in a.names:
        if a.cmd == "send" and a.chunk:
            send_chunked(nm, a.to, a.ssh, a.preset, a.chunk)
        elif a.cmd == "send":
            send(nm, a.to, a.ssh, a.preset)
        else:
            path, s, c = it[nm]
            h, _, _ = header(path)
            n, md = frames_of(path, s, c, None)
            print(json.dumps({"name": nm, "frames": n, "plane_md5": md,
                              "hdr": h.decode().rstrip("\n")}), flush=True)


if __name__ == "__main__":
    main()
