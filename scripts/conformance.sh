#!/bin/sh
# Conformance gate: every encode must reconstruct bit-exact through BOTH
# independent decoders (ffmpeg hevc + dec265), and the encoder must be
# byte-identical across runs. Phase 0 note: everything is all-intra IDR so
# decode order == display order; when B-frames land, the display-order leg
# must compare through ffmpeg only (dec265 -o emits decode order).
#
# usage: scripts/conformance.sh [--fast]
# env: N265=path/to/n265  DEC265=path/to/dec265  FFMPEG=path/to/ffmpeg

set -u

N265=${N265:-build/n265}
FFMPEG=${FFMPEG:-ffmpeg}
DEC265=${DEC265:-}
FAST=0
[ "${1:-}" = "--fast" ] && FAST=1

# find dec265: env, PATH (Debian ships it as libde265-dec265), then the
# sibling libde265 build
if [ -z "$DEC265" ]; then
    for cand in dec265 libde265-dec265; do
        if command -v "$cand" >/dev/null 2>&1; then
            DEC265=$cand
            break
        fi
    done
    if [ -z "$DEC265" ] && [ -x ../libde265/build/dec265/dec265 ]; then
        DEC265=../libde265/build/dec265/dec265
    fi
    if [ -z "$DEC265" ]; then
        echo "conformance: dec265 not found (set DEC265=); FAILING: the gate requires two decoders" >&2
        exit 1
    fi
fi
command -v "$FFMPEG" >/dev/null 2>&1 || { echo "conformance: ffmpeg not found" >&2; exit 1; }
[ -x "$N265" ] || { echo "conformance: $N265 not built" >&2; exit 1; }

TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT

pass=0
fail=0

run_vector() {
    # $1 name  $2 WxH  $3 frames  $4 lavfi source
    name=$1; size=$2; frames=$3; src=$4
    y4m="$TMP/$name.y4m"
    "$FFMPEG" -loglevel error -f lavfi -i "$src=size=$size:rate=25" \
        -vf "noise=alls=12:allf=t+u" -frames:v "$frames" -pix_fmt yuv420p "$y4m" -y \
        2>/dev/null
    if [ ! -s "$y4m" ]; then
        echo "SUMMARY $name SKIP (source gen failed)"
        return
    fi

    "$N265" "$y4m" -o "$TMP/$name.hevc" 2>/dev/null || {
        echo "SUMMARY $name FAIL (encode)"; fail=$((fail+1)); return; }
    "$N265" "$y4m" -o "$TMP/$name.2.hevc" 2>/dev/null
    cmp -s "$TMP/$name.hevc" "$TMP/$name.2.hevc" || {
        echo "SUMMARY $name FAIL (determinism: two runs differ)"
        fail=$((fail+1)); return; }

    "$FFMPEG" -loglevel error -i "$y4m" -f rawvideo "$TMP/$name.src.yuv" -y
    "$FFMPEG" -loglevel error -i "$TMP/$name.hevc" -f rawvideo \
        "$TMP/$name.ff.yuv" -y 2>"$TMP/$name.fferr"
    if ! cmp -s "$TMP/$name.src.yuv" "$TMP/$name.ff.yuv"; then
        echo "SUMMARY $name FAIL (ffmpeg recon mismatch)"
        sed -n 1,3p "$TMP/$name.fferr" >&2
        fail=$((fail+1)); return
    fi
    "$DEC265" -q -o "$TMP/$name.de.yuv" "$TMP/$name.hevc" >/dev/null 2>&1
    if ! cmp -s "$TMP/$name.src.yuv" "$TMP/$name.de.yuv"; then
        echo "SUMMARY $name FAIL (dec265 recon mismatch)"
        fail=$((fail+1)); return
    fi
    echo "SUMMARY $name PASS"
    pass=$((pass+1))
}

# geometry coverage: CTB-aligned, unaligned (conformance window), tiny,
# tall/narrow, HD
run_vector aligned_qcif   176x144  5  testsrc2
run_vector aligned_qvga   320x240  5  testsrc2
run_vector tiny_1ctb      64x64    3  testsrc2
run_vector crop_100x60    100x60   5  testsrc2
run_vector crop_210x122   210x122  5  testsrc2
run_vector narrow_32x256  32x256   3  testsrc2
if [ "$FAST" -eq 0 ]; then
    run_vector aligned_cif 352x288 10 testsrc2
    run_vector hd_720      1280x720 3 testsrc2
    run_vector long_qcif   176x144 30 smptebars
fi

echo "conformance: $pass pass, $fail fail"
[ "$fail" -eq 0 ]
