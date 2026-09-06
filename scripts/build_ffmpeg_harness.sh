#!/usr/bin/env bash
# Copyright (c) 2026, the yah265 authors
# SPDX-License-Identifier: GPL-2.0-or-later
#
# build_ffmpeg_harness.sh - rebuild the in-process speed harness after a
# reboot. The harness chain lives partly in /tmp (library installs), so a
# reboot destroys the instrument; this makes it one command instead of
# archaeology. See docs/instruments.md "one harness per table".
#
# Produces: ../ffmpeg-yah265/ffmpeg with libyah265 + libx265 +
# libnext264 + libx264 (all arm64), and verifies arch + linkage.
set -euo pipefail

root="$(cd "$(dirname "$0")/.." && pwd)"
fork="$root/../ffmpeg-yah265"

# 1. yah265 release library (asserts off - never time assertions)
if [ ! -d "$root/build-rel" ]; then
    meson setup "$root/build-rel" -Dbuildtype=release -Db_ndebug=true \
        --prefix=/tmp/n265inst
fi
ninja -C "$root/build-rel"
meson install -C "$root/build-rel" >/dev/null

# 2. sibling installs: yah264's library and arm64 x264 builds are
# yah264's responsibility (their docs/ffmpeg-integration-plan.md has
# the recipe); warn rather than rebuild what is not ours.
for d in /tmp/n264inst/lib/pkgconfig /tmp/x264asm/lib/pkgconfig; do
    [ -d "$d" ] || echo "warning: $d missing - rebuild yah264's side" \
        "or configure without libnext264/libx264" >&2
done

# 3. the fork. NOTE the explicit PKG_CONFIG_PATH: this box's default
# pkg-config x264 is an x86_64 Intel-brew leftover in /usr/local that
# resolves fine and then fails to link on arm64.
cd "$fork"
PKG_CONFIG_PATH=/tmp/n265inst/lib/pkgconfig:/tmp/n264inst/lib/pkgconfig:/tmp/x264asm/lib/pkgconfig \
./configure --cc=clang --enable-gpl \
    --enable-libx265 --enable-libyah265 \
    --enable-libx264 --enable-libnext264 \
    --disable-doc --disable-ffplay --disable-ffprobe >/dev/null
make -j8 ffmpeg

# 4. verify: every encoder present, every non-system library arm64
./ffmpeg -hide_banner -encoders 2>/dev/null |
    grep -qE "libyah265" || { echo "libyah265 missing"; exit 1; }
bad=0
for l in $(otool -L ffmpeg | grep -oE "(/opt/homebrew|/tmp)[^ ]*"); do
    a="$(lipo -archs "$l" 2>/dev/null)"
    [ "$a" = "arm64" ] || { echo "NOT arm64: $l ($a)"; bad=1; }
done
[ "$bad" = 0 ] && echo "harness ready: $fork/ffmpeg (all arm64)"
