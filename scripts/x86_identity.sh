#!/usr/bin/env bash
# Copyright (c) 2026, the yah265 authors
# SPDX-License-Identifier: GPL-2.0-or-later
#
# x86_identity.sh - the kernel item's escape cmp on an x86-64 build:
# every clip at one thread, N265_ASM_OFF=all against the shipped binary
# (the AVX2 tier) and against N265_CPU_MAX=sse42 (the SSE4.2 tier), as
# the bitstream and as --dump-recon. Every kernel is bit-exact with its C
# reference, so every cell must read SAME.
#
#   scripts/x86_identity.sh BIN                 # CIF-7, medium and fast
#   PRESETS=medium FRAMES=12 scripts/x86_identity.sh build-x86/n265
#
# Under Rosetta the AVX2 tier is reached only with ROSETTA_ADVERTISE_AVX=1,
# which this script sets. It also refuses to run when the binary does not
# report AVX2 on this CPU, since the cmp would then be C against C and
# could not fail, and drops the sse42 arm when the build has no SSE4.2
# kernels. It reads no clock.
#
# Environment:
#   CLIPS     clip names under CORPUS (default the CIF-7)
#   PRESETS   (default "medium fast")
#   FRAMES    frames per encode (default 24)
#   JOBS      encodes at once, one thread each (default 4)
#   CORPUS    (default tests/corpus, then the main checkout's)
#   ARM_ON    extra NAME=VALUE settings for both sides, e.g. N265_SATD8=1
set -u

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BIN="${1:?usage: x86_identity.sh BIN}"
CLIPS="${CLIPS:-akiyo_cif foreman_cif mobile_cif stefan_cif bus_cif tempete_cif coastguard_cif}"
PRESETS="${PRESETS:-medium fast}"
FRAMES="${FRAMES:-24}"
JOBS="${JOBS:-4}"
CORPUS="${CORPUS:-$ROOT/tests/corpus}"
if [ ! -d "$CORPUS" ]; then
    main="$(git -C "$ROOT" worktree list --porcelain | awk '/^worktree /{print $2; exit}')"
    CORPUS="$main/tests/corpus"
fi
export ROSETTA_ADVERTISE_AVX=1

hash_of() {
    if command -v md5 >/dev/null 2>&1; then md5 -q "$1"
    else md5sum "$1" | cut -d' ' -f1; fi
}

# the build's checkasm prints the feature word the library detects
ca="$(dirname "$BIN")/tests/checkasm"
if ! "$ca" satd 2>/dev/null | head -1 | grep -q ' avx2,'; then
    echo "x86_identity: $ca does not report AVX2 here; refusing a C-vs-C cmp" >&2
    exit 2
fi
ARMS="on off"
if "$ca" 2>/dev/null | grep -q '^checkasm: checking sse42'; then
    ARMS="on sse42 off"
else
    echo "x86_identity: no SSE4.2 kernels in this build; avx2 arm only" >&2
fi

tmp="$(mktemp -d "${TMPDIR:-/tmp}/x86id.XXXXXX")"
trap 'rm -rf "$tmp"' EXIT

encode() {   # encode clip preset arm
    local clip="$1" preset="$2" arm="$3" out="$tmp/$1.$2.$3"
    # never empty: bash 3.2 calls an empty array unbound under set -u
    local envs=(ROSETTA_ADVERTISE_AVX=1)
    [ "$arm" = off ] && envs+=(N265_ASM_OFF=all)
    [ "$arm" = sse42 ] && envs+=(N265_CPU_MAX=sse42)
    for e in ${ARM_ON:-}; do envs+=("$e"); done
    env "${envs[@]}" nice -n 10 "$BIN" "$CORPUS/$clip.y4m" -o "$out.hevc" \
        --preset "$preset" --frames "$FRAMES" --threads 1 \
        --dump-recon "$out.yuv" > /dev/null 2>&1 || echo FAIL > "$out.fail"
}

running=0
for clip in $CLIPS; do
    [ -f "$CORPUS/$clip.y4m" ] || { echo "x86_identity: $clip missing" >&2; exit 2; }
    for preset in $PRESETS; do
        for arm in $ARMS; do
            encode "$clip" "$preset" "$arm" &
            running=$((running + 1))
            # macOS bash 3.2 has no `wait -n`, so the jobs run in batches
            if [ "$running" -ge "$JOBS" ]; then
                wait
                running=0
            fi
        done
    done
done
wait

rc=0
for clip in $CLIPS; do
    for preset in $PRESETS; do
        b="$tmp/$clip.$preset"
        for arm in $ARMS; do
            [ "$arm" = off ] && continue
            if [ -f "$b.$arm.fail" ] || [ -f "$b.off.fail" ] ||
               [ ! -s "$b.$arm.hevc" ] || [ ! -s "$b.off.hevc" ]; then
                printf '%-16s %-7s %-5s FAIL (encode)\n' "$clip" "$preset" "$arm"
                rc=1; continue
            fi
            s="SAME"; r="SAME"
            [ "$(hash_of "$b.$arm.hevc")" = "$(hash_of "$b.off.hevc")" ] || { s=DIFF; rc=1; }
            [ "$(hash_of "$b.$arm.yuv")" = "$(hash_of "$b.off.yuv")" ] || { r=DIFF; rc=1; }
            printf '%-16s %-7s %-5s stream %s  recon %s  %s\n' "$clip" "$preset" \
                "${arm/on/avx2}" "$s" "$r" "$(hash_of "$b.$arm.hevc")"
        done
    done
done
exit $rc
