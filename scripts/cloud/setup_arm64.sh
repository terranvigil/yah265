#!/bin/bash
# Copyright (c) 2026, the yah265 authors
# SPDX-License-Identifier: GPL-2.0-or-later
#
# setup_arm64.sh -- runs ON a rented Graviton box, as root, in ~ubuntu/probe.
# The arm64 twin of setup_x86.sh: installs the toolchain and ffmpeg,
# builds dec265 from libde265 at the commit the Mac's gates use (Ubuntu's
# 1.0.15 mis-decodes two fast-conformance vectors that carry EOS NALs),
# ours from yah265.tar.gz (release for the clock, the default tree and
# both sanitizer trees for the gates, at 8 and 10 bits) and x265 4.3 from
# the release tarball with its aarch64 assembly, and records every
# version and the CPU's SVE shape into env.txt.
#
#   setup_arm64.sh            everything
#   setup_arm64.sh ours       rebuild ours only (after a new tarball)
set -euxo pipefail
cd "$(dirname "$0")"
P=$PWD
export DEBIAN_FRONTEND=noninteractive
what=${1:-all}

if [ "$what" = all ]; then
    apt-get update -q
    apt-get install -y -q --no-install-recommends clang meson ninja-build \
        cmake make g++ git python3 util-linux ffmpeg unzip libclang-rt-18-dev \
        "linux-tools-$(uname -r)" linux-tools-common \
        || apt-get install -y -q --no-install-recommends clang meson \
            ninja-build cmake make g++ git python3 util-linux ffmpeg unzip \
            libclang-rt-18-dev

    rm -rf libde265
    git clone -q https://github.com/strukturag/libde265.git
    git -C libde265 checkout -q 4d45a6b
    cmake -S libde265 -B libde265/build -G Ninja -DCMAKE_BUILD_TYPE=Release
    ninja -C libde265/build dec265

    X265_URL=https://github.com/Multicorewareinc/x265/releases/download/4.3/x265_4.3.tar.gz
    X265_SHA=83c53e4c8bbb8f1e33ed59e10a7d621d1d7801ca853910c3eb41f038b8ffb121
    curl -sL -o x265_4.3.tar.gz "$X265_URL"
    echo "$X265_SHA  x265_4.3.tar.gz" | sha256sum -c -
    rm -rf x265src && mkdir x265src
    tar -xzf x265_4.3.tar.gz -C x265src --strip-components 1
    CC=clang CXX=clang++ cmake -S x265src/source -B x265build -G Ninja \
        -DCMAKE_BUILD_TYPE=Release -DENABLE_SHARED=OFF -DENABLE_CLI=ON
    ninja -C x265build x265
fi

rm -rf src && mkdir src && tar -xzf yah265.tar.gz -C src
chown -R ubuntu:ubuntu src
sudo -u ubuntu bash -c "cd src &&
    CC=clang meson setup build-rel -Dbuildtype=release &&
    CC=clang meson setup build &&
    CC=clang meson setup build10 -Dbit_depth=10 &&
    CC=clang meson setup build-asan -Db_sanitize=address,undefined -Db_lundef=false &&
    CC=clang meson setup build10-asan -Dbit_depth=10 -Db_sanitize=address,undefined -Db_lundef=false &&
    for b in build-rel build build10 build-asan build10-asan; do ninja -C \$b; done"

{
    echo "date: $(date -u +%FT%TZ)"
    echo "kernel: $(uname -r)"
    lscpu | grep -E 'Model name|Vendor|^CPU\(s\)|Thread|Core|Socket|MHz|L1d|L2|L3'
    echo "features: $(grep -m1 Features /proc/cpuinfo | cut -d: -f2)"
    echo "sve default vl (bytes): $(cat /proc/sys/abi/sve_default_vector_length 2>/dev/null)"
    echo "clang: $(clang --version | head -1)"
    echo "meson: $(meson --version)  cmake: $(cmake --version | head -1)"
    echo "ffmpeg: $(ffmpeg -version | head -1)"
    echo "dec265: libde265 $(git -C libde265 log --oneline -1)"
    echo "x265: $(./x265build/x265 --version 2>&1 | tr '\r' '\n' | grep -E 'version|build info|cpu capab' | tr '\n' ' ')"
    echo "x265 --no-asm: $(./x265build/x265 --no-asm --version 2>&1 | tr '\r' '\n' | grep -E 'cpu capab')"
    echo "ours checkasm:"
    ./src/build-rel/tests/checkasm 2>&1 | grep -E '^checkasm'
} > env.txt 2>&1
cat env.txt
