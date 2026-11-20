#!/bin/sh
# Cuts the same-size frame crops on the results page out of one testbench
# job: the source, yah265's encode and x265's encode of the same clip.
#
#   scripts/frame_crops.sh [job-dir]
#
# It decodes single frames and encodes nothing. Needs ffmpeg (FFMPEG=),
# cwebp, and python3 with numpy. The job's source file must be readable.
#
# How the frames are chosen, so nobody picks them by eye:
#   1. The job's VMAF v1 log has a score per frame for each encoder. Sort
#      the frames by yah265's lead (its score minus x265's, ties by frame
#      number) and take the frames at the lower quartile, the median and
#      the upper quartile of that order.
#   2. In each frame, take the 416x234 window where the two encodes differ
#      most from each other (largest sum of absolute luma difference). The
#      rule doesn't know which encode is closer to the source.
# Each crop is doubled with nearest-neighbor scaling and stored lossless,
# so the page shows the decoder's own pixels.
set -eu

JOB=${1:-$HOME/media/testbench/20261120-144540-tos-1080p-60-yah265}
FFMPEG=${FFMPEG:-ffmpeg}
ROOT=$(cd "$(dirname "$0")/.." && pwd)
OUT=$ROOT/site/assets/frames
NAME=tos
CW=416
CH=234
TMP=$(mktemp -d)
trap 'rm -r "$TMP"' EXIT
mkdir -p "$OUT"

# the job file names the source, its seek point and scale filter, both
# encodes, and the per-frame scores
python3 - "$JOB" "$TMP" <<'EOF'
import json, shlex, sys
job, tmp = sys.argv[1:3]
j = json.load(open(f"{job}/job.json"))
cmd = j["arms"]["ours"]["cmds"][0]
arg = lambda flag: cmd[cmd.index(flag) + 1]
ours, ref = j["scores"]["ours"]["per_frame"], j["scores"]["ref"]["per_frame"]
lead = [a - b for a, b in zip(ours, ref)]
order = sorted(range(len(lead)), key=lambda i: (lead[i], i))
picks = [order[round(q * (len(lead) - 1))] for q in (0.25, 0.5, 0.75)]
with open(f"{tmp}/env", "w") as f:
    for k, v in (("SRC", arg("-i")), ("SEEK", arg("-ss")), ("VF", arg("-vf")),
                 ("OURS", f"{job}/{j['arms']['ours']['file']}"),
                 ("REF", f"{job}/{j['arms']['ref']['file']}"),
                 ("FRAMES", " ".join(map(str, picks)))):
        f.write(f"{k}={shlex.quote(v)}\n")
json.dump({"job": j["id"], "title": j["title"]["name"], "frames_scored": len(lead),
           "encodes": {k: {"bytes": j["arms"][a]["bytes"], "vmaf_v1_mean": j["scores"][a]["mean"],
                           "wall_s": j["arms"][a]["wall"]}
                       for k, a in (("yah265", "ours"), ("x265", "ref"))},
           "picks": [{"at": q, "frame": i, "yah265": ours[i], "x265": ref[i]}
                     for q, i in zip(("lower quartile", "median", "upper quartile"), picks)]},
          open(f"{tmp}/meta.json", "w"))
EOF
. "$TMP/env"

for n in $FRAMES; do
  "$FFMPEG" -hide_banner -nostdin -v error -ss "$SEEK" -i "$SRC" -map 0:v:0 \
    -vf "$VF,select=eq(n\,$n)" -frames:v 1 -y "$TMP/$n.source.ppm"
  "$FFMPEG" -hide_banner -nostdin -v error -i "$OURS" \
    -vf "select=eq(n\,$n)" -frames:v 1 -y "$TMP/$n.yah265.ppm"
  "$FFMPEG" -hide_banner -nostdin -v error -i "$REF" \
    -vf "select=eq(n\,$n)" -frames:v 1 -y "$TMP/$n.x265.ppm"

  # the window where the two encodes differ most from each other
  XY=$(python3 - "$TMP/$n.yah265.ppm" "$TMP/$n.x265.ppm" $CW $CH <<'EOF'
import sys
import numpy as np

def luma(path):
    b = open(path, "rb").read()
    magic, w, h, mx = b.split(None, 4)[:4]
    assert magic == b"P6" and mx == b"255", path
    rgb = np.frombuffer(b[-int(w) * int(h) * 3:], np.uint8).reshape(int(h), int(w), 3)
    return rgb @ np.array([0.2126, 0.7152, 0.0722])

a, b = luma(sys.argv[1]), luma(sys.argv[2])
cw, ch = int(sys.argv[3]), int(sys.argv[4])
ii = np.pad(np.abs(a - b).cumsum(0).cumsum(1), ((1, 0), (1, 0)))
box = ii[ch:, cw:] - ii[:-ch, cw:] - ii[ch:, :-cw] + ii[:-ch, :-cw]
y, x = np.unravel_index(box.argmax(), box.shape)
print(x, y)
EOF
)
  X=${XY% *}
  Y=${XY#* }
  for arm in source yah265 x265; do
    "$FFMPEG" -hide_banner -nostdin -v error -i "$TMP/$n.$arm.ppm" \
      -vf "crop=$CW:$CH:$X:$Y,scale=iw*2:ih*2:flags=neighbor" -frames:v 1 -y "$TMP/crop.ppm"
    cwebp -quiet -lossless -z 9 "$TMP/crop.ppm" -o "$OUT/$NAME-$n-$arm.webp"
  done
  # the whole frame, small, to show where the crop sits
  "$FFMPEG" -hide_banner -nostdin -v error -i "$TMP/$n.source.ppm" \
    -vf "scale=640:-2:flags=bicubic" -frames:v 1 -y "$TMP/view.ppm"
  cwebp -quiet -q 72 "$TMP/view.ppm" -o "$OUT/$NAME-$n-frame.webp"
  echo "$n $X $Y" >> "$TMP/boxes"
done

python3 - "$TMP" "$OUT/$NAME.json" $CW $CH <<'EOF'
import json, sys
tmp, out, cw, ch = sys.argv[1], sys.argv[2], int(sys.argv[3]), int(sys.argv[4])
meta = json.load(open(f"{tmp}/meta.json"))
boxes = {int(n): (int(x), int(y)) for n, x, y in (l.split() for l in open(f"{tmp}/boxes"))}
for p in meta["picks"]:
    x, y = boxes[p["frame"]]
    p["crop"] = {"x": x, "y": y, "w": cw, "h": ch}
json.dump(meta, open(out, "w"), indent=1)
open(out, "a").write("\n")
EOF
ls -l "$OUT"
