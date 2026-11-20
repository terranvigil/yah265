#!/bin/sh
# Cuts the frame crops on the results page out of a matched-size pair of
# encodes: yah265.mp4 and x265.mp4 of the same clip, a near-transparent
# reference_x264.mp4, and a VMAF v1 log for each encode with one score per
# frame (yah265.vmaf.json, x265.vmaf.json).
#
#   scripts/frame_crops.sh [dir]
#
# It decodes and encodes nothing. Needs ffmpeg (FFMPEG=), cwebp, and
# python3 with numpy.
#
# The rule, so nobody picks frames by eye:
#   1. A frame's score is the mean of the two encoders' VMAF v1 on it. A
#      shot's score is its lowest frame. Shots are cut where ffmpeg's scene
#      score on the reference passes 0.3.
#   2. Take that lowest frame from each of the three lowest-scoring shots.
#      A frame within 2 seconds of one already taken is passed over for the
#      next shot's.
#   3. In each frame, measure each encode's loss against the reference per
#      16x16 block (mean absolute luma difference). Crop the 208x112 window
#      where the two encodes' losses differ most, whichever one is ahead.
# Each crop is scaled 4x with nearest-neighbor and stored lossless, so the
# page shows the decoder's own pixels.
set -eu

DIR=${1:-$HOME/media/compare-yah265/tears}
FFMPEG=${FFMPEG:-ffmpeg}
ROOT=$(cd "$(dirname "$0")/.." && pwd)
OUT=$ROOT/site/assets/frames
NAME=tears
CW=208
CH=112
ZOOM=4
SHOTS=3
APART=2
TMP=$(mktemp -d)
trap 'rm -r "$TMP"' EXIT
mkdir -p "$OUT"

"$FFMPEG" -hide_banner -nostdin -v error -i "$DIR/reference_x264.mp4" \
  -vf "select='gt(scene,0.3)',metadata=print:file=-" -an -f null - > "$TMP/cuts"

python3 - "$DIR" "$TMP" $SHOTS $APART <<'EOF'
import json, os, re, sys
d, tmp, shots, apart = sys.argv[1], sys.argv[2], int(sys.argv[3]), float(sys.argv[4])
log = {k: json.load(open(f"{d}/{k}.vmaf.json")) for k in ("yah265", "x265")}
score = {k: [f["metrics"]["v1"] for f in v["frames"]] for k, v in log.items()}
n = len(score["yah265"])
assert n == len(score["x265"])
times = [float(t) for t in re.findall(r"pts_time:([\d.]+)", open(f"{tmp}/cuts").read())]
rate = json.load(open(f"{d}/manifest.json"))["window"]["fps"]
edges = [0] + [round(t * rate) for t in times] + [n]
mean = [(a + b) / 2 for a, b in zip(score["yah265"], score["x265"])]
low = sorted((mean[i], i) for i in (min(range(a, b), key=lambda k: (mean[k], k))
                                    for a, b in zip(edges, edges[1:]) if b > a))
picks = []
for _, i in low:
    if all(abs(i - j) >= apart * rate for j in picks):
        picks.append(i)
    if len(picks) == shots:
        break
json.dump({"clip": "Tears of Steel, 1080p, 90 s", "frames_scored": n, "fps": rate,
           "shots": len(edges) - 1,
           "encodes": {k: {"bytes": os.path.getsize(f"{d}/{k}.mp4"),
                           "vmaf_v1_mean": sum(score[k]) / n} for k in score},
           "picks": [{"frame": i, "at_s": round(i / rate, 2), "mean": round(mean[i], 3),
                      "yah265": score["yah265"][i], "x265": score["x265"][i]} for i in picks]},
          open(f"{tmp}/meta.json", "w"))
open(f"{tmp}/frames", "w").write(" ".join(map(str, picks)))
EOF

for n in $(cat "$TMP/frames"); do
  for arm in reference yah265 x265; do
    file=$arm
    [ "$arm" = reference ] && file=reference_x264
    "$FFMPEG" -hide_banner -nostdin -v error -i "$DIR/$file.mp4" \
      -vf "select=eq(n\,$n)" -frames:v 1 -y "$TMP/$n.$arm.ppm"
  done

  XY=$(python3 - "$TMP/$n" $CW $CH <<'EOF'
import sys
import numpy as np

def luma(path):
    b = open(path, "rb").read()
    magic, w, h, mx = b.split(None, 4)[:4]
    assert magic == b"P6" and mx == b"255", path
    rgb = np.frombuffer(b[-int(w) * int(h) * 3:], np.uint8).reshape(int(h), int(w), 3)
    return rgb @ np.array([0.2126, 0.7152, 0.0722])

B = 16
ref, a, b = (luma(f"{sys.argv[1]}.{k}.ppm") for k in ("reference", "yah265", "x265"))
bw, bh = int(sys.argv[2]) // B, int(sys.argv[3]) // B
h, w = ref.shape

def loss(x):
    e = np.abs(x - ref)[:h // B * B, :w // B * B]
    return e.reshape(h // B, B, w // B, B).mean((1, 3))

la, lb = loss(a), loss(b)
ii = np.pad(np.abs(la - lb).cumsum(0).cumsum(1), ((1, 0), (1, 0)))
box = ii[bh:, bw:] - ii[:-bh, bw:] - ii[bh:, :-bw] + ii[:-bh, :-bw]
y, x = np.unravel_index(box.argmax(), box.shape)
win = (slice(y, y + bh), slice(x, x + bw))
print(x * B, y * B, round(float(la[win].mean()), 3), round(float(lb[win].mean()), 3))
EOF
)
  set -- $XY
  for arm in reference yah265 x265; do
    "$FFMPEG" -hide_banner -nostdin -v error -i "$TMP/$n.$arm.ppm" \
      -vf "crop=$CW:$CH:$1:$2,scale=iw*$ZOOM:ih*$ZOOM:flags=neighbor" -frames:v 1 -y "$TMP/crop.ppm"
    cwebp -quiet -lossless -z 9 "$TMP/crop.ppm" -o "$OUT/$NAME-$n-$arm.webp"
  done
  # the whole frame, small, to show where the crop sits
  "$FFMPEG" -hide_banner -nostdin -v error -i "$TMP/$n.reference.ppm" \
    -vf "scale=640:-2:flags=bicubic" -frames:v 1 -y "$TMP/view.ppm"
  cwebp -quiet -q 72 "$TMP/view.ppm" -o "$OUT/$NAME-$n-frame.webp"
  echo "$n $XY" >> "$TMP/boxes"
done

python3 - "$TMP" "$OUT/$NAME.json" $CW $CH $ZOOM <<'EOF'
import json, sys
tmp, out = sys.argv[1:3]
cw, ch, zoom = map(int, sys.argv[3:6])
meta = json.load(open(f"{tmp}/meta.json"))
boxes = {int(l.split()[0]): l.split()[1:] for l in open(f"{tmp}/boxes")}
meta["zoom"] = zoom
for p in meta["picks"]:
    x, y, la, lb = boxes[p["frame"]]
    p["crop"] = {"x": int(x), "y": int(y), "w": cw, "h": ch}
    p["loss"] = {"yah265": float(la), "x265": float(lb)}
with open(out, "w") as f:
    json.dump(meta, f, indent=1)
    f.write("\n")
EOF
ls -l "$OUT"
