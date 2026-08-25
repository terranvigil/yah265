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
    # $1 name  $2 WxH  $3 frames  $4 lavfi source  $5 encoder args
    # ("--pcm" makes the reference the source itself; anything else makes
    # it the encoder's own reconstruction via --dump-recon)
    name=$1; size=$2; frames=$3; src=$4; eargs=$5
    y4m="$TMP/$name.y4m"
    "$FFMPEG" -loglevel error -f lavfi -i "$src=size=$size:rate=25" \
        -vf "noise=alls=12:allf=t+u" -frames:v "$frames" -pix_fmt yuv420p "$y4m" -y \
        2>/dev/null
    if [ ! -s "$y4m" ]; then
        echo "SUMMARY $name SKIP (source gen failed)"
        return
    fi

    # shellcheck disable=SC2086
    "$N265" "$y4m" -o "$TMP/$name.hevc" $eargs \
        --dump-recon "$TMP/$name.recon.yuv" 2>/dev/null || {
        echo "SUMMARY $name FAIL (encode)"; fail=$((fail+1)); return; }
    # shellcheck disable=SC2086
    "$N265" "$y4m" -o "$TMP/$name.2.hevc" $eargs 2>/dev/null
    cmp -s "$TMP/$name.hevc" "$TMP/$name.2.hevc" || {
        echo "SUMMARY $name FAIL (determinism: two runs differ)"
        fail=$((fail+1)); return; }

    case " $eargs " in
    *" --pcm "*)
        "$FFMPEG" -loglevel error -i "$y4m" -f rawvideo "$TMP/$name.src.yuv" -y
        ;;
    *)
        cp "$TMP/$name.recon.yuv" "$TMP/$name.src.yuv"
        ;;
    esac
    "$FFMPEG" -loglevel error -i "$TMP/$name.hevc" -f rawvideo \
        "$TMP/$name.ff.yuv" -y 2>"$TMP/$name.fferr"
    # ffmpeg and dec265 disagree on bounded chroma seam samples for
    # CTB-16 SAO streams (reproduced on x265 output too; recorded in
    # docs/instruments.md). dec265 stays bit-exact below; ffmpeg gets a
    # small labeled chroma allowance only on ctu-16 vectors.
    case "$eargs" in
    *"--ctu 16"*)
        if ! python3 scripts/yuvdiff.py "$TMP/$name.src.yuv" \
                "$TMP/$name.ff.yuv" --geometry "$size" \
                --max-chroma-pct 1.0 --max-delta 16; then
            echo "SUMMARY $name FAIL (ffmpeg recon mismatch)"
            fail=$((fail+1)); return
        fi
        ;;
    *)
        if ! cmp -s "$TMP/$name.src.yuv" "$TMP/$name.ff.yuv"; then
            echo "SUMMARY $name FAIL (ffmpeg recon mismatch)"
            sed -n 1,3p "$TMP/$name.fferr" >&2
            fail=$((fail+1)); return
        fi
        ;;
    esac
    if ! "$DEC265" -q -o "$TMP/$name.de.yuv" "$TMP/$name.hevc" \
            >/dev/null 2>"$TMP/$name.deerr" || [ ! -s "$TMP/$name.de.yuv" ]; then
        echo "SUMMARY $name FAIL (dec265 run failed)"
        sed -n 1,3p "$TMP/$name.deerr" >&2
        fail=$((fail+1)); return
    fi
    if ! cmp -s "$TMP/$name.src.yuv" "$TMP/$name.de.yuv"; then
        echo "SUMMARY $name FAIL (dec265 recon mismatch)"
        fail=$((fail+1)); return
    fi
    echo "SUMMARY $name PASS"
    pass=$((pass+1))
}

# geometry coverage: CTB-aligned, unaligned (forced splits + conformance
# window), tiny, tall/narrow, HD; QP sweep on the real path
run_vector pcm_qcif       176x144  3  testsrc2  "--pcm"
run_vector pcm_crop       100x60   3  testsrc2  "--pcm"
run_vector intra_qcif     176x144  5  testsrc2  ""
run_vector intra_qvga     320x240  5  testsrc2  ""
run_vector intra_1ctb     64x64    3  testsrc2  ""
run_vector intra_crop     100x60   5  testsrc2  ""
run_vector intra_crop2    210x122  5  testsrc2  ""
run_vector intra_narrow   32x256   3  testsrc2  ""
run_vector intra_qp8      176x144  3  testsrc2  "--qp 8"
run_vector intra_qp45     176x144  3  testsrc2  "--qp 45"
run_vector intra_smooth   100x60   3  gradients ""
run_vector inter_ippp     176x144 20  testsrc2  "--keyint 10"
run_vector inter_static   176x144 15  smptebars "--keyint 15"
run_vector inter_crop     210x122 12  testsrc2  "--keyint 8"
run_vector intra_ctu16    176x144  3  testsrc2  "--ctu 16"
run_vector intra_ctu64    176x144  3  testsrc2  "--ctu 64"
run_vector intra_ctu64c   210x122  3  testsrc2  "--ctu 64"
if [ "$FAST" -eq 0 ]; then
    run_vector inter_cif   352x288 25  testsrc2  "--keyint 12"
    run_vector inter_ctu64 176x144 12  testsrc2  "--keyint 6 --ctu 64"
    run_vector pcm_cif     352x288  5  testsrc2  "--pcm"
    run_vector intra_cif   352x288 10 testsrc2  ""
    run_vector intra_720   1280x720 3 testsrc2  ""
    run_vector intra_bars  176x144 30 smptebars ""
    run_vector intra_qp0   64x64    2  testsrc2  "--qp 0"
    run_vector intra_qp51  176x144  3  testsrc2  "--qp 51"
fi

echo "conformance: $pass pass, $fail fail"
[ "$fail" -eq 0 ]
