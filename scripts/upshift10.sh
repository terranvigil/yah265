#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
# 8-bit 4:2:0 y4m -> 10-bit 4:2:0 y4m (C420p10), phase 8 item 2.
#
# The Main10 encoder refuses 8-bit input (cli/n265.c): one library per
# sample width, and a silent upshift inside the reader would let a 10-bit
# measurement be taken on SDR content without saying so. This is the
# conversion, spelled out once, so that every 10-bit gate names it.
#
# It is a PURE LEFT SHIFT by two - sample * 4, not sample * 1023/255 -
# which is what a Main10 stream of SDR content is, and what makes the
# 10-bit encode of an upshifted clip comparable with the 8-bit encode of
# its original at the same QP: the residual is exactly four times, the
# forward transform sheds exactly two bits (shift1 = log2n + BitDepth - 9),
# and the coefficients that reach the quantizer are the same integers.
# ffmpeg's format filter does that shift for yuv420p -> yuv420p10le.
#
# usage: scripts/upshift10.sh in.y4m out.y4m [extra ffmpeg input args...]
#   e.g. scripts/upshift10.sh tests/corpus/foreman_cif.y4m /tmp/f10.y4m
# env: FFMPEG=path/to/ffmpeg

set -u

FFMPEG=${FFMPEG:-ffmpeg}

if [ $# -lt 2 ]; then
    echo "usage: $0 in.y4m out.y4m [ffmpeg input args]" >&2
    exit 2
fi
in=$1; out=$2; shift 2

command -v "$FFMPEG" >/dev/null 2>&1 || {
    echo "upshift10: ffmpeg not found (set FFMPEG=)" >&2; exit 1; }

# -strict -1 is required because yuv420p10le is not a registered y4m
# pixel format; the tag written is C420p10, which is what the reader
# accepts. It has to sit with the OUTPUT options, after -pix_fmt.
# shellcheck disable=SC2086
"$FFMPEG" -loglevel error -y "$@" -i "$in" -pix_fmt yuv420p10le -strict -1 \
    -f yuv4mpegpipe "$out" || {
    echo "upshift10: ffmpeg failed on $in" >&2; exit 1; }

[ -s "$out" ] || { echo "upshift10: produced no bytes" >&2; exit 1; }
