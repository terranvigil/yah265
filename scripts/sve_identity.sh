#!/usr/bin/env bash
# Copyright (c) 2026, the yah265 authors
# SPDX-License-Identifier: GPL-2.0-or-later
#
# sve_identity.sh - the SVE tier's escape cmp, on an aarch64 Linux box with
# SVE: every clip at one thread, N265_ASM_OFF=all against the shipped
# binary (the SVE tier) and against N265_CPU_MAX=neon (the NEON tier), as
# the bitstream and as --dump-recon. Every kernel is bit-exact with its C
# reference, so every cell must read SAME. It refuses to run where the
# build's checkasm reports no SVE, since the cmp would then be NEON
# against C and say nothing about the tier. It reads no clock.
#
#   scripts/sve_identity.sh BIN                 # CIF-7, medium and fast
#   PRESETS=medium FRAMES=12 scripts/sve_identity.sh build/y265
#
# Environment: CLIPS, PRESETS, FRAMES (24), JOBS (4), CORPUS, ARM_ON
# (extra NAME=VALUE settings for every arm, e.g. N265_ASM_OFF=mc to
# check that a broken kernel's class escape works), EXTRA (extra CLI
# arguments, e.g. "--output-depth 10").
set -u

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BIN="${1:?usage: sve_identity.sh BIN}"
CLIPS="${CLIPS:-akiyo_cif foreman_cif mobile_cif stefan_cif bus_cif tempete_cif coastguard_cif}"
PRESETS="${PRESETS:-medium fast}"
FRAMES="${FRAMES:-24}"
JOBS="${JOBS:-4}"
CORPUS="${CORPUS:-$ROOT/tests/corpus}"

ca="$(dirname "$BIN")/tests/checkasm"
if ! "$ca" satd 2>/dev/null | head -1 | grep -q ' sve'; then
    echo "sve_identity: $ca does not report SVE here; refusing" >&2
    exit 2
fi

tmp="$(mktemp -d "${TMPDIR:-/tmp}/sveid.XXXXXX")"
trap 'rm -rf "$tmp"' EXIT
ARMS="on neon off"

encode() {   # encode clip preset arm
    local clip="$1" preset="$2" arm="$3" out="$tmp/$1.$2.$3"
    local envs=()
    [ "$arm" = off ] && envs+=(N265_ASM_OFF=all)
    [ "$arm" = neon ] && envs+=(N265_CPU_MAX=neon)
    for e in ${ARM_ON:-}; do envs+=("$e"); done
    env "${envs[@]}" nice -n 10 "$BIN" "$CORPUS/$clip.y4m" -o "$out.hevc" \
        --preset "$preset" --frames "$FRAMES" --threads 1 ${EXTRA:-} \
        --dump-recon "$out.yuv" > /dev/null 2>&1 || echo FAIL > "$out.fail"
}

for clip in $CLIPS; do
    [ -f "$CORPUS/$clip.y4m" ] || { echo "sve_identity: $clip missing" >&2; exit 2; }
    for preset in $PRESETS; do
        for arm in $ARMS; do
            encode "$clip" "$preset" "$arm" &
            while [ "$(jobs -r | wc -l)" -ge "$JOBS" ]; do wait -n; done
        done
    done
done
wait

rc=0
for clip in $CLIPS; do
    for preset in $PRESETS; do
        b="$tmp/$clip.$preset"
        for arm in on neon; do
            if [ -f "$b.$arm.fail" ] || [ -f "$b.off.fail" ] ||
               [ ! -s "$b.$arm.hevc" ] || [ ! -s "$b.off.hevc" ]; then
                printf '%-16s %-7s %-5s FAIL (encode)\n' "$clip" "$preset" "$arm"
                rc=1; continue
            fi
            s="SAME"; r="SAME"
            cmp -s "$b.$arm.hevc" "$b.off.hevc" || { s=DIFF; rc=1; }
            cmp -s "$b.$arm.yuv" "$b.off.yuv" || { r=DIFF; rc=1; }
            label=$arm; [ "$arm" = on ] && label=sve
            printf '%-16s %-7s %-5s stream %s  recon %s  %s\n' "$clip" "$preset" \
                "$label" "$s" "$r" "$(md5sum < "$b.$arm.hevc" | cut -c1-32)"
        done
    done
done
exit $rc
