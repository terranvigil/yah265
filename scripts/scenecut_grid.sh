#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-or-later
# --no-scenecut on a real multi-shot clip: every IDR on a multiple of
# --keyint, the same frames at the source size and at half of it, and
# (as a check that the clip has cuts at all) at least one IDR off the
# grid without the flag. Reads the key frames back from --frame-stats.
#
#   scripts/scenecut_grid.sh [clip.y4m] [frames] [keyint]
#
# Default clip: local/corpus/ms_cif_30.y4m (five shots). N265 names the
# binary (default build/n265); THREADS its --threads (default 4).
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/.." && pwd)
CLIP=${1:-$ROOT/local/corpus/ms_cif_30.y4m}
FRAMES=${2:-600}
KEYINT=${3:-48}
N265=${N265:-$ROOT/build/n265}
THREADS=${THREADS:-4}
if [ ! -f "$CLIP" ]; then
    echo "scenecut_grid: no clip at $CLIP; skipping"
    exit 77
fi
work=$(mktemp -d "${TMPDIR:-/tmp}/scgrid.XXXXXX")
trap 'rm -rf "$work"' EXIT

w=$(head -c 200 "$CLIP" | head -1 | tr ' ' '\n' | sed -n 's/^W//p')
h=$(head -c 200 "$CLIP" | head -1 | tr ' ' '\n' | sed -n 's/^H//p')
hw=$(( w / 4 * 2 )); hh=$(( h / 4 * 2 ))
ffmpeg -v error -i "$CLIP" -frames:v "$FRAMES" -vf "scale=$hw:$hh" \
    -pix_fmt yuv420p -f yuv4mpegpipe -strict -1 "$work/half.y4m"

idrs() {   # clip, extra args...: the IDR display indices, sorted
    local clip=$1; shift
    "$N265" -o "$work/o.hevc" --crf 28 --keyint "$KEYINT" --no-open-gop \
        --threads "$THREADS" --frames "$FRAMES" \
        --frame-stats "$work/fs.jsonl" "$@" "$clip" >/dev/null 2>&1
    python3 -c '
import json, sys
print(" ".join(str(d) for d in sorted(
    r["frame"] for r in map(json.loads, open(sys.argv[1])) if r["idr"])))
' "$work/fs.jsonl"
}

fail=0
dflt=$(idrs "$CLIP")
full=$(idrs "$CLIP" --no-scenecut)
half=$(idrs "$work/half.y4m" --no-scenecut)
want=$(python3 -c "print(' '.join(str(d) for d in range(0, $FRAMES, $KEYINT)))")
echo "default       IDR: $dflt"
echo "no-scenecut   IDR: $full  (${w}x${h})"
echo "no-scenecut   IDR: $half  (${hw}x${hh})"
off=$(python3 -c "print(sum(int(d) % $KEYINT != 0 for d in '$dflt'.split()))")
if [ "$off" -lt 1 ]; then
    echo "FAIL: no cut placed a key frame without the flag; the clip tests nothing"
    fail=1
fi
[ "$full" = "$want" ] || { echo "FAIL: ${w}x${h} key frames are not 0, $KEYINT, ..."; fail=1; }
[ "$half" = "$full" ] || { echo "FAIL: the two sizes put key frames on different frames"; fail=1; }
[ $fail = 0 ] && echo "PASS: key frames on multiples of $KEYINT at both sizes"
exit $fail
