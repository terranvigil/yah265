#!/usr/bin/env bash
# Copyright (c) 2026, the yah265 authors
# SPDX-License-Identifier: GPL-2.0-or-later
#
# Build an ffmpeg that encodes with yah265 (-c:v libyah265).
#
#   scripts/build-ffmpeg.sh [--prefix DIR] [--jobs N] [--with-x265]
#
# Installs libyah265 into DIR (default: ./build-ffmpeg/inst), clones the
# ffmpeg fork's yah265 branch next to it and builds ffmpeg and ffprobe
# there. --with-x265 also enables libx265 when pkg-config can find it,
# so one binary runs both encoders for comparisons.
set -euo pipefail

root="$(cd "$(dirname "$0")/.." && pwd)"
work="$root/build-ffmpeg"
prefix=""
jobs=""
x265=0
repo="${FFMPEG_REPO:-https://github.com/terranvigil/FFmpeg.git}"
branch="${FFMPEG_BRANCH:-yah265}"

while [ $# -gt 0 ]; do
  case "$1" in
    --prefix) prefix="$2"; shift 2 ;;
    --jobs) jobs="$2"; shift 2 ;;
    --with-x265) x265=1; shift ;;
    -h|--help) sed -n '5,12p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
    *) echo "unknown option: $1" >&2; exit 2 ;;
  esac
done
prefix="${prefix:-$work/inst}"
if [ -z "$jobs" ]; then
  jobs="$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 4)"
fi

for tool in meson ninja git pkg-config make; do
  command -v "$tool" >/dev/null || { echo "missing: $tool" >&2; exit 1; }
done

echo "== libyah265 -> $prefix"
meson setup "$work/lib" "$root" -Dbuildtype=release -Dprefix="$prefix" --reconfigure >/dev/null 2>&1 \
  || meson setup "$work/lib" "$root" -Dbuildtype=release -Dprefix="$prefix"
ninja -C "$work/lib" install >/dev/null

src="$work/ffmpeg"
if [ -d "$src/.git" ]; then
  echo "== ffmpeg: updating $src"
  git -C "$src" fetch -q origin "$branch"
  git -C "$src" checkout -q -B "$branch" FETCH_HEAD
else
  echo "== ffmpeg: cloning $repo ($branch)"
  git clone -q --depth 1 -b "$branch" "$repo" "$src"
fi

extra=()
if [ "$x265" = 1 ]; then
  if pkg-config --exists x265; then
    extra+=(--enable-libx265)
  else
    echo "note: pkg-config can't find x265, building without it" >&2
  fi
fi

# a moved prefix can leave a stale wrapper object behind
rm -f "$src/libavcodec/libyah265.o" "$src/libavcodec/libyah265.d"

echo "== ffmpeg: configure"
(
  cd "$src"
  PKG_CONFIG_PATH="$prefix/lib/pkgconfig${PKG_CONFIG_PATH:+:$PKG_CONFIG_PATH}" \
    ./configure --enable-gpl --enable-libyah265 \
    --extra-ldflags="-Wl,-rpath,$prefix/lib" ${extra[@]+"${extra[@]}"} >"$work/configure.log" 2>&1
) || { tail -20 "$work/configure.log" >&2; exit 1; }
echo "== ffmpeg: make -j$jobs"
make -C "$src" -j"$jobs" ffmpeg ffprobe >"$work/make.log" 2>&1 \
  || { tail -30 "$work/make.log" >&2; exit 1; }

"$src/ffmpeg" -hide_banner -h encoder=libyah265 | head -1
echo
echo "built: $src/ffmpeg"
echo "try:   $src/ffmpeg -i in.mp4 -c:v libyah265 -preset medium -crf 25 out.mp4"
