#!/bin/bash
# Copyright (c) 2026, the yah265 authors
# SPDX-License-Identifier: GPL-2.0-or-later
#
# setup_tables.sh -- runs ON a timing box (x86-64 or arm64) as ubuntu, in ~/t.
# Mounts nothing: the box's root volume holds the media it pulls from the
# hub (HUB, a private address) into /data. Builds ours (release, clang) from
# the hub's yah265.tar.gz and x265 4.3 from its release tarball with its
# default assembly (8-bit), and records every version into env.txt.
#
#   setup_tables.sh HUB FILE...     FILE relative to the hub's /data
set -euxo pipefail
HUB=$1; shift
T=~/t
mkdir -p $T && cd $T
export DEBIAN_FRONTEND=noninteractive
sudo apt-get update -q
PK="clang meson ninja-build cmake make python3 util-linux ffmpeg rsync"
[ "$(uname -m)" = x86_64 ] && PK="$PK nasm"
sudo apt-get install -y -q --no-install-recommends $PK
sudo mkdir -p /data /data/res && sudo chown -R ubuntu:ubuntu /data
RS="rsync -a --partial -e 'ssh -i $HOME/.ssh/hub.pem -o StrictHostKeyChecking=no'"
eval $RS ubuntu@$HUB:/data/src/yah265.tar.gz $T/
X265_URL=https://github.com/Multicorewareinc/x265/releases/download/4.3/x265_4.3.tar.gz
X265_SHA=83c53e4c8bbb8f1e33ed59e10a7d621d1d7801ca853910c3eb41f038b8ffb121
curl -sL -o x265_4.3.tar.gz "$X265_URL"
echo "$X265_SHA  x265_4.3.tar.gz" | sha256sum -c -
rm -rf x265src && mkdir x265src && tar -xzf x265_4.3.tar.gz -C x265src --strip-components 1
CC=clang CXX=clang++ cmake -S x265src/source -B x265build -G Ninja \
    -DCMAKE_BUILD_TYPE=Release -DENABLE_SHARED=OFF -DENABLE_CLI=ON
ninja -C x265build x265
rm -rf src && mkdir src && tar -xzf yah265.tar.gz -C src
(cd src && CC=clang meson setup build-rel -Dbuildtype=release && ninja -C build-rel)
# a file is pulled once the Mac has marked it whole on the hub (/data/ok/NAME)
for f in "$@"; do
    until ssh -i $HOME/.ssh/hub.pem -o StrictHostKeyChecking=no ubuntu@$HUB "test -f /data/ok/$(basename "$f")"; do sleep 30; done
    mkdir -p "/data/$(dirname "$f")"
    case "$f" in
        *.d) mkdir -p "/data/$f"; until eval $RS "ubuntu@$HUB:/data/$f/" "/data/$f/"; do sleep 30; done ;;
        *) until eval $RS "ubuntu@$HUB:/data/$f" "/data/$f"; do sleep 30; done ;;
    esac
done
{
    echo "date: $(date -u +%FT%TZ)"
    echo "kernel: $(uname -r)"
    lscpu | grep -E 'Model name|Vendor|^CPU\(s\)|Thread|Core|Socket|MHz|L1d|L2|L3'
    [ "$(uname -m)" = x86_64 ] && echo "avx512: $(grep -o 'avx512[a-z_0-9]*' /proc/cpuinfo | sort -u | tr '\n' ' ')"
    [ "$(uname -m)" = aarch64 ] && echo "features: $(grep -m1 Features /proc/cpuinfo | cut -d: -f2)"
    echo "clang: $(clang --version | head -1)"
    echo "ffmpeg: $(ffmpeg -version | head -1)"
    echo "x265: $(./x265build/x265 --version 2>&1 | tr '\r' '\n' | grep -E 'version|build info|cpu capab' | tr '\n' ' ')"
    [ "$(uname -m)" = x86_64 ] && echo "x265 --asm avx512: $(./x265build/x265 --asm avx512 --version 2>&1 | tr '\r' '\n' | grep -E 'cpu capab')"
    echo "x265 --no-asm: $(./x265build/x265 --no-asm --version 2>&1 | tr '\r' '\n' | grep -E 'cpu capab')"
    echo "n265 sha256: $(sha256sum src/build-rel/n265 | cut -c1-16)"
    echo "ours checkasm:"
    ./src/build-rel/tests/checkasm 2>&1 | grep -E '^checkasm' || true
} > env.txt 2>&1
cat env.txt
