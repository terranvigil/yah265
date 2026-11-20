#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Compare two raw YUV files. Exit 0 when equal, or when within the given
bounds (used for the known ffmpeg-vs-dec265 chroma SAO seam disagreement
at --ctu 16: luma must stay bit-exact, chroma may differ in a bounded,
labeled way)."""

import sys


def main():
    a_path, b_path = sys.argv[1], sys.argv[2]
    w = h = 0
    max_diffs = 0
    max_delta = 0
    max_pct = 0.0
    args = sys.argv[3:]
    while args:
        k = args.pop(0)
        if k == "--geometry":
            w, h = (int(v) for v in args.pop(0).split("x"))
        elif k == "--max-chroma-diffs":
            max_diffs = int(args.pop(0))
        elif k == "--max-chroma-pct":
            max_diffs = -1
            max_pct = float(args.pop(0))
        elif k == "--max-delta":
            max_delta = int(args.pop(0))
    a = open(a_path, "rb").read()
    b = open(b_path, "rb").read()
    if len(a) != len(b):
        print(f"yuvdiff: size mismatch {len(a)} vs {len(b)}")
        return 1
    if a == b:
        return 0
    if not w:
        print("yuvdiff: files differ")
        return 1
    frame = w * h * 3 // 2
    luma = w * h
    diffs = [i for i in range(len(a)) if a[i] != b[i]]
    luma_diffs = [i for i in diffs if i % frame < luma]
    if luma_diffs:
        print(f"yuvdiff: {len(luma_diffs)} luma diffs (first at {luma_diffs[0]})")
        return 1
    worst = max(abs(a[i] - b[i]) for i in diffs)
    if max_diffs < 0:
        nframes = len(a) // frame
        max_diffs = int(nframes * (w // 2) * (h // 2) * 2 * max_pct / 100)
    if len(diffs) <= max_diffs and worst <= max_delta:
        print(f"yuvdiff: {len(diffs)} bounded chroma diffs (max delta "
              f"{worst}) - within the known decoder-disagreement allowance")
        return 0
    print(f"yuvdiff: {len(diffs)} chroma diffs, max delta {worst} - "
          f"exceeds allowance ({max_diffs}/{max_delta})")
    return 1


if __name__ == "__main__":
    sys.exit(main())
