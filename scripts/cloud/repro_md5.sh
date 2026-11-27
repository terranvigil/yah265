#!/bin/bash
# Copyright (c) 2026, the yah265 authors
# SPDX-License-Identifier: GPL-2.0-or-later
#
# repro_md5.sh -- runs ON a timing box after setup_tables.sh, in ~/t. Codes
# three clips at --threads 1 (--crf 28, --no-info, so no version string
# rides in the stream) with the box's SIMD and with N265_ASM_OFF=all, and
# writes the md5s to repro.txt. On arm64 they must equal the Mac's streams
# of the same commit; on x86-64 the SIMD and C streams must equal each
# other, and every x86-64 box must write the same ones.
set -euo pipefail
cd ~/t
mkdir -p work
B=src/build-rel/n265
{
    echo "box $(hostname) $(uname -m) n265 $(sha256sum $B | cut -c1-16)"
    for f in /data/s/foreman_cif.y4m /data/c/WorldCup_1920x1080_30p.y4m \
             /data/c/Wikipedia_1920x1080p30.y4m; do
        n=$(basename "$f" .y4m)
        $B "$f" --crf 28 --threads 1 --no-info -o work/r_$n.hevc 2>/dev/null
        N265_ASM_OFF=all $B "$f" --crf 28 --threads 1 --no-info \
            -o work/rc_$n.hevc 2>/dev/null
        echo "$n simd $(md5sum < work/r_$n.hevc | cut -c1-32) c $(md5sum < work/rc_$n.hevc | cut -c1-32) bytes $(stat -c %s work/r_$n.hevc)"
        rm -f work/r_$n.hevc work/rc_$n.hevc
    done
} > repro.txt 2>&1
cat repro.txt
