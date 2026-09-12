#!/bin/sh
# The Main10 build's smoke, phase 8 item 1. It is NOT a correctness gate:
# item 1 only claims that the pixel is a type, that the 8-bit build is
# byte-identical to what it was, and that the 10-bit build compiles and
# RUNS - its C path, since the NEON translation units are not in that
# build at all. What the 10-bit samples are worth is item 2's question,
# answered there by the recon-match gate on ffmpeg AND libde265 over the
# conformance vectors.
#
# So: take an 8-bit 4:2:0 source (the CLI upshifts it by two bits on the
# way in, which is what a Main10 stream of SDR content is), encode it
# with the 10-bit binary, and require that the encode finishes, that it
# produced bytes, and that ffmpeg decodes the frames back out.
#
# usage: scripts/smoke10.sh
# env: N265_10=path/to/n265-10  FFMPEG=path/to/ffmpeg

set -u

N265_10=${N265_10:-build10/n265-10}
FFMPEG=${FFMPEG:-ffmpeg}
FRAMES=${FRAMES:-12}

[ -x "$N265_10" ] || { echo "smoke10: $N265_10 not built" >&2; exit 1; }
command -v "$FFMPEG" >/dev/null 2>&1 || {
    echo "smoke10: ffmpeg not found; SKIP" >&2; exit 0; }

TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT

# 96x64 so the frame is neither CTB-aligned at 64 nor a single CTB, with
# noise so there is residual to code rather than an all-skip stream.
"$FFMPEG" -loglevel error -f lavfi -i "testsrc2=size=96x64:rate=25" \
    -vf "noise=alls=12:allf=t+u" -frames:v "$FRAMES" -pix_fmt yuv420p \
    "$TMP/in.y4m" -y 2>/dev/null
[ -s "$TMP/in.y4m" ] || { echo "smoke10: source gen failed; SKIP" >&2; exit 0; }

"$N265_10" "$TMP/in.y4m" -o "$TMP/out.hevc" --qp 30 --frames "$FRAMES" \
    >/dev/null 2>"$TMP/enc.log" || {
    echo "smoke10: FAIL (10-bit encode exited nonzero)" >&2
    cat "$TMP/enc.log" >&2; exit 1; }
[ -s "$TMP/out.hevc" ] || {
    echo "smoke10: FAIL (10-bit encode produced no bytes)" >&2; exit 1; }

# Decode, and count what came back. A stream the decoder cannot parse
# gives zero frames, which is the failure this leg is looking for.
"$FFMPEG" -loglevel error -i "$TMP/out.hevc" -f rawvideo "$TMP/dec.yuv" -y \
    2>"$TMP/dec.log" || {
    echo "smoke10: FAIL (ffmpeg could not decode the 10-bit stream)" >&2
    cat "$TMP/dec.log" >&2; exit 1; }

# 96x64 4:2:0 at 10 bits: two bytes a sample, 1.5 samples a pixel.
want=$((96 * 64 * 3 / 2 * 2 * FRAMES))
got=$(wc -c < "$TMP/dec.yuv" | tr -d ' ')
[ "$got" = "$want" ] || {
    echo "smoke10: FAIL (decoded $got bytes, wanted $want: $FRAMES frames of 96x64 yuv420p10)" >&2
    exit 1; }

bytes=$(wc -c < "$TMP/out.hevc" | tr -d ' ')
echo "smoke10: OK ($FRAMES frames, ${bytes}B of Main10 bitstream, decoded $got bytes)"
