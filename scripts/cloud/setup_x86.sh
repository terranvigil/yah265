#!/bin/bash
# Copyright (c) 2026, the yah265 authors
# SPDX-License-Identifier: GPL-2.0-or-later
#
# setup_x86.sh -- runs ON the rented x86 box, as root, in ~ubuntu/probe.
# Installs the toolchain, builds ours (release, clang, AVX2 tier with
# runtime CPUID dispatch) from yah265.tar.gz and x265 4.3 from the release
# tarball, and records every version into env.txt.
#
# x265 is built 8-bit only, with clang and its default assembly; the Mac's
# brew build is the same release tarball with 10- and 12-bit libraries
# linked beside it, which the 8-bit encode doesn't touch.
set -euxo pipefail
cd "$(dirname "$0")"
P=$PWD
export DEBIAN_FRONTEND=noninteractive

apt-get update -q
apt-get install -y -q --no-install-recommends clang meson ninja-build cmake \
    nasm make python3 util-linux linux-tools-common "linux-tools-$(uname -r)" \
    || apt-get install -y -q --no-install-recommends clang meson ninja-build \
        cmake nasm make python3 util-linux

X265_URL=https://github.com/Multicorewareinc/x265/releases/download/4.3/x265_4.3.tar.gz
X265_SHA=83c53e4c8bbb8f1e33ed59e10a7d621d1d7801ca853910c3eb41f038b8ffb121
curl -sL -o x265_4.3.tar.gz "$X265_URL"
echo "$X265_SHA  x265_4.3.tar.gz" | sha256sum -c -
rm -rf x265src && mkdir x265src && tar -xzf x265_4.3.tar.gz -C x265src --strip-components 1
CC=clang CXX=clang++ cmake -S x265src/source -B x265build -G Ninja \
    -DCMAKE_BUILD_TYPE=Release -DENABLE_SHARED=OFF -DENABLE_CLI=ON
ninja -C x265build x265

rm -rf src && mkdir src && tar -xzf yah265.tar.gz -C src
CC=clang meson setup src/build-rel src -Dbuildtype=release
ninja -C src/build-rel

{
    echo "date: $(date -u +%FT%TZ)"
    echo "kernel: $(uname -r)"
    lscpu | grep -E 'Model name|^CPU\(s\)|Thread|Core|Socket|MHz|L2|L3|Flags' | sed 's/Flags:.*/flags: (see below)/'
    echo "avx512 flags: $(grep -o 'avx512[a-z_0-9]*' /proc/cpuinfo | sort -u | tr '\n' ' ')"
    echo "clang: $(clang --version | head -1)"
    echo "meson: $(meson --version)  cmake: $(cmake --version | head -1)  nasm: $(nasm -v)"
    echo "x265: $(./x265build/x265 --version 2>&1 | tr '\r' '\n' | grep -E 'version|build info|cpu capab' | tr '\n' ' ')"
    echo "x265 --no-asm: $(./x265build/x265 --no-asm --version 2>&1 | tr '\r' '\n' | grep -E 'cpu capab')"
    echo "ours checkasm:"
    ./src/build-rel/tests/checkasm 2>&1 | tail -3
} > env.txt 2>&1
cat env.txt
