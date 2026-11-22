#!/usr/bin/env bash
# Copyright (c) 2026, the yah265 authors
# SPDX-License-Identifier: GPL-2.0-or-later
#
# Build a static ffmpeg + ffprobe with libyah265, plus y265, and bundle
# them with the licences and a SOURCE.md.
#
#   packaging/build-static.sh [--work DIR] [--jobs N] [--ffmpeg-src DIR]
#                             [--name NAME] [--sources]
#
# Linux (Alpine/musl): fully static. macOS: static apart from the system
# libraries and frameworks. The bundle lands in WORK/bundle/NAME and a
# tarball beside it; --sources also writes WORK/sources with the source
# tarballs the GPL asks the binary to travel with.
set -euo pipefail

root="$(cd "$(dirname "$0")/.." && pwd)"
work="$root/build-static"
jobs=""
ffsrc=""
name=""
sources=0
repo="${FFMPEG_REPO:-https://github.com/terranvigil/FFmpeg.git}"
ffref="${FFMPEG_REF:-$(cat "$root/packaging/ffmpeg-ref")}"

while [ $# -gt 0 ]; do
  case "$1" in
    --work) work="$2"; shift 2 ;;
    --jobs) jobs="$2"; shift 2 ;;
    --ffmpeg-src) ffsrc="$2"; shift 2 ;;
    --name) name="$2"; shift 2 ;;
    --sources) sources=1; shift ;;
    -h|--help) sed -n '5,15p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
    *) echo "unknown option: $1" >&2; exit 2 ;;
  esac
done
jobs="${jobs:-$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 4)}"
mkdir -p "$work"
work="$(cd "$work" && pwd)"

os="$(uname -s)"
arch="$(uname -m)"
case "$arch" in arm64|aarch64) arch=arm64 ;; x86_64|amd64) arch=x86_64 ;; esac
case "$os" in
  Linux) osname=linux ;;
  Darwin) osname=macos ;;
  *) echo "unsupported OS: $os" >&2; exit 1 ;;
esac

version="$(sed -n "s/^ *version *: *'\([^']*\)'.*/\1/p" "$root/meson.build" | head -1)"
rev="${YAH265_REV:-$(git -C "$root" rev-parse --short=12 HEAD 2>/dev/null || echo unknown)}"
name="${name:-yah265-ffmpeg-$version-$osname-$arch}"

for tool in meson ninja pkg-config make; do
  command -v "$tool" >/dev/null || { echo "missing: $tool" >&2; exit 1; }
done

prefix="$work/prefix"
rm -rf "$prefix"

# a fully static y265 on Linux; the library's shared form can't take -static
cli_link=()
[ "$osname" = linux ] && cli_link=(-Dc_link_args=-static)

echo "== libyah265 (8-bit, installed) -> $prefix"
meson setup "$work/lib8" "$root" --wipe -Dbuildtype=release \
  -Dprefix="$prefix" -Dlibdir=lib >"$work/meson8.log" 2>&1 \
  || meson setup "$work/lib8" "$root" -Dbuildtype=release \
       -Dprefix="$prefix" -Dlibdir=lib >"$work/meson8.log" 2>&1 \
  || { tail -20 "$work/meson8.log" >&2; exit 1; }
meson compile -C "$work/lib8" -j "$jobs" \
  yah265:static_library yah265:shared_library y265 >"$work/build8.log" 2>&1 \
  || { tail -30 "$work/build8.log" >&2; exit 1; }
meson install -C "$work/lib8" --no-rebuild >/dev/null

# the 8-bit archive lacks the Main10 entry points the ffmpeg wrapper calls
echo "== libyah265-10 (Main10 archive) and y265"
meson setup "$work/lib10" "$root" --wipe -Dbuildtype=release -Dbit_depth=10 \
  ${cli_link[@]+"${cli_link[@]}"} >"$work/meson10.log" 2>&1 \
  || meson setup "$work/lib10" "$root" -Dbuildtype=release -Dbit_depth=10 \
       ${cli_link[@]+"${cli_link[@]}"} >"$work/meson10.log" 2>&1 \
  || { tail -20 "$work/meson10.log" >&2; exit 1; }
meson compile -C "$work/lib10" -j "$jobs" \
  yah265-10:static_library y265-10 >"$work/build10.log" 2>&1 \
  || { tail -30 "$work/build10.log" >&2; exit 1; }
cp "$work/lib10/libyah265-10.a" "$prefix/lib/"
rm -f "$prefix"/lib/libyah265*.so* "$prefix"/lib/libyah265*.dylib

echo "== ffmpeg $ffref"
if [ -z "$ffsrc" ]; then
  ffsrc="$work/ffmpeg"
  if [ ! -d "$ffsrc/.git" ]; then
    git init -q "$ffsrc"
    git -C "$ffsrc" remote add origin "$repo"
  fi
  git -C "$ffsrc" fetch -q --depth 1 origin "$ffref"
  git -C "$ffsrc" checkout -q --force FETCH_HEAD
fi
ffsrc="$(cd "$ffsrc" && pwd)"
ffrev="$(git -C "$ffsrc" rev-parse HEAD 2>/dev/null || echo "$ffref")"

cfg=(
  --prefix="$work/ffinst"
  --enable-gpl --enable-libyah265
  --enable-static --disable-shared
  --disable-autodetect --enable-zlib
  --disable-doc --disable-debug --disable-ffplay
  --pkg-config-flags=--static
  --extra-ldflags="-L$prefix/lib"
  --extra-libs="-lyah265-10 -lm -lpthread"
)
if [ "$osname" = linux ]; then
  cfg+=(--extra-ldflags=-static)
else
  cfg+=(--enable-videotoolbox --enable-audiotoolbox)
fi

builddir="$work/ffbuild"
rm -rf "$builddir"
mkdir -p "$builddir"
(
  cd "$builddir"
  PKG_CONFIG_PATH="$prefix/lib/pkgconfig" PKG_CONFIG_LIBDIR="$prefix/lib/pkgconfig" \
    "$ffsrc/configure" "${cfg[@]}" >"$work/configure.log" 2>&1
) || { tail -30 "$work/configure.log" >&2; tail -40 "$builddir/ffbuild/config.log" >&2 || true; exit 1; }
make -C "$builddir" -j"$jobs" ffmpeg ffprobe >"$work/make.log" 2>&1 \
  || { tail -40 "$work/make.log" >&2; exit 1; }

echo "== bundle $name"
out="$work/bundle/$name"
rm -rf "$out"
mkdir -p "$out/bin" "$out/licenses"
cp "$builddir/ffmpeg" "$builddir/ffprobe" "$out/bin/"
cp "$work/lib10/y265-10" "$out/bin/y265"
strip "$out/bin/ffmpeg" "$out/bin/ffprobe" "$out/bin/y265" 2>/dev/null || true

cp "$root/LICENSE" "$out/LICENSE"
cp "$ffsrc/COPYING.GPLv2" "$out/licenses/ffmpeg-COPYING.GPLv2"
cp "$ffsrc/LICENSE.md" "$out/licenses/ffmpeg-LICENSE.md"
cp "$root/packaging/NOTICE-patents.txt" "$out/NOTICE-patents.txt"

zlib_ver=""; musl_ver=""
if [ "$osname" = linux ] && command -v apk >/dev/null; then
  zlib_ver="$(apk list --installed zlib-static 2>/dev/null | sed -n 's/^zlib-static-\([0-9.]*\)-r.*/\1/p')"
  musl_ver="$(apk list --installed musl 2>/dev/null | sed -n 's/^musl-\([0-9.]*\)-r.*/\1/p')"
fi

cfgline="configure ${cfg[*]}"
cfgline="${cfgline//$work/\$WORK}"
cc_ver="$( (cc --version 2>/dev/null || true) | head -1)"
{
  echo "# What built this"
  echo
  echo "| part | version | source |"
  echo "|---|---|---|"
  echo "| yah265 | $version ($rev) | https://github.com/terranvigil/yah265 |"
  echo "| ffmpeg fork | $ffrev | $repo |"
  [ -n "$zlib_ver" ] && echo "| zlib | $zlib_ver | https://zlib.net/fossils/zlib-$zlib_ver.tar.gz |"
  [ -n "$musl_ver" ] && echo "| musl libc | $musl_ver | https://musl.libc.org/releases/musl-$musl_ver.tar.gz |"
  echo
  echo "Toolchain: $cc_ver, $osname $arch."
  echo
  echo "ffmpeg was built with:"
  echo
  echo '```'
  echo "$cfgline"
  echo '```'
  echo
  echo "The script that built it is packaging/build-static.sh in the yah265"
  echo "source. The complete source of every part above is published in the"
  echo "same place as this binary: the release page or image it came from."
} >"$out/SOURCE.md"

cat >"$out/README.txt" <<EOF
yah265 $version: ffmpeg with the libyah265 HEVC encoder, and y265.

  bin/ffmpeg -i in.mp4 -c:v libyah265 -preset medium -crf 25 out.mp4
  bin/ffmpeg -i in.mp4 -pix_fmt yuv420p -f yuv4mpegpipe - | bin/y265 - -o out.hevc --crf 25

Always pass a rate option (-crf or -b:v). Docs: https://terranvigil.github.io/yah265/

This ffmpeg is GPL-2.0-or-later because libyah265 is. LICENSE and
licenses/ hold the licence texts; SOURCE.md says what built it and where
its source is. Read NOTICE-patents.txt before you redistribute it.
EOF

if [ -n "$musl_ver$zlib_ver" ] || [ "$sources" = 1 ]; then
  srcdir="$work/sources"
  mkdir -p "$srcdir"
  for spec in "zlib:$zlib_ver:https://zlib.net/fossils/zlib-$zlib_ver.tar.gz:LICENSE" \
              "musl:$musl_ver:https://musl.libc.org/releases/musl-$musl_ver.tar.gz:COPYRIGHT"; do
    IFS=: read -r lib ver url1 url2 lic <<<"$spec"
    [ -n "$ver" ] || continue
    tgz="$srcdir/$lib-$ver.tar.gz"
    [ -s "$tgz" ] || curl -fsSL -o "$tgz" "$url1:$url2"
    tar -xzOf "$tgz" "$lib-$ver/$lic" >"$out/licenses/$lib-$lic"
  done
fi
if [ "$osname" = macos ]; then
  # zlib comes from the SDK; its notice still goes with the binary
  printf '%s\n' "zlib is linked from the macOS SDK. Its licence:" "" \
    "$(sed -n '/Copyright (C)/,/madler@alumni.caltech.edu/p' "$(xcrun --show-sdk-path)/usr/include/zlib.h")" \
    >"$out/licenses/zlib-LICENSE"
fi

if [ "$sources" = 1 ]; then
  echo "== sources -> $srcdir"
  if git -C "$root" rev-parse HEAD >/dev/null 2>&1; then
    git -C "$root" archive --format=tar.gz --prefix="yah265-$version/" \
      -o "$srcdir/yah265-$version-src.tar.gz" HEAD
  else
    rename=(-s "|^$(basename "$root")|yah265-$version|")
    tar --version 2>/dev/null | grep -q GNU \
      && rename=(--transform "s|^$(basename "$root")|yah265-$version|")
    tar -czf "$srcdir/yah265-$version-src.tar.gz" -C "$root/.." \
      --exclude="$(basename "$root")/build*" --exclude=local --exclude=.claude \
      "${rename[@]}" "$(basename "$root")"
  fi
  if git -C "$ffsrc" rev-parse HEAD >/dev/null 2>&1; then
    git -C "$ffsrc" archive --format=tar.gz --prefix="ffmpeg-yah265-${ffrev:0:12}/" \
      -o "$srcdir/ffmpeg-yah265-${ffrev:0:12}-src.tar.gz" HEAD
  fi
fi

tar -czf "$work/bundle/$name.tar.gz" -C "$work/bundle" "$name"

"$out/bin/ffmpeg" -hide_banner -h encoder=libyah265 | head -1
echo "bundle: $work/bundle/$name.tar.gz"
