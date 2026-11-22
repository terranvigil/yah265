#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-or-later
#
# Point the two formulas at a yah265 tag and the pinned ffmpeg commit.
#
#   packaging/homebrew/bump.sh vX.Y.Z
#
# Downloads both tarballs, writes their URLs and sha256 into y265.rb and
# ffmpeg-yah265.rb, and prints the diff. The tag must already be public.
set -euo pipefail

tag="${1:?usage: bump.sh vX.Y.Z}"
here="$(cd "$(dirname "$0")" && pwd)"
ffref="$(cat "$here/../ffmpeg-ref")"
tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT

sha() { curl -fsSL -o "$tmp/t.tgz" "$1" && shasum -a 256 "$tmp/t.tgz" | cut -d' ' -f1; }

y_url="https://github.com/terranvigil/yah265/archive/refs/tags/$tag.tar.gz"
f_url="https://github.com/terranvigil/FFmpeg/archive/$ffref.tar.gz"
y_sha="$(sha "$y_url")"
f_sha="$(sha "$f_url")"

# replace the url line and the sha256 line that follows it
set_src() {
  awk -v url="$2" -v sum="$3" '
    /^  url "/    { print "  url \"" url "\""; next }
    /^  sha256 "/ { print "  sha256 \"" sum "\""; next }
    { print }' "$1" >"$tmp/f" && cat "$tmp/f" >"$1"
}
set_src "$here/y265.rb" "$y_url" "$y_sha"
set_src "$here/ffmpeg-yah265.rb" "$f_url" "$f_sha"

git -C "$here" diff -- y265.rb ffmpeg-yah265.rb || true
