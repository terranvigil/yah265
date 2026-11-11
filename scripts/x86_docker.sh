#!/usr/bin/env bash
# Copyright (c) 2026, the yah265 authors
# SPDX-License-Identifier: GPL-2.0-or-later
#
# x86_docker.sh - the gcc and QEMU legs of the x86 gate (docs/x86-plan.md),
# on an Apple Silicon Mac through Docker.
#
#   scripts/x86_docker.sh                 # every leg
#   LEGS="build checkasm" scripts/x86_docker.sh
#   X86_IMAGE=yah264-x86:24.04 X86_QIMAGE=yah264-sve:24.04 \
#       LIBDE265_SRC=../libde265 scripts/x86_docker.sh
#
# The two images are built from docker/x86 when absent. Any image with
# gcc, meson, ninja, ffmpeg and python3 serves for the first, and any
# host-arch one with qemu-x86_64 for the second. Where the first has no
# dec265, LIBDE265_SRC names a libde265 source tree that the build leg
# compiles in the container.
#
# Legs, in order:
#   build     gcc builds of both depths in linux/amd64 (Docker Desktop runs
#             it under Rosetta), the warnings kept in the log, plus a
#             static build of checkasm for QEMU
#   checkasm  checkasm at every N265_CPU_MAX tier, both depths
#   test      make test on the gcc builds
#   conf      conformance --fast, 8-bit and Main10, both decoders
#   qemu      the static checkasm under qemu-x86_64 in a host-arch
#             container, once per -cpu model: Nehalem (SSE4.2, no AVX),
#             Haswell (AVX2) and max
#
# Every container runs with --cpus 4 and is removed when it exits. Each
# leg waits for the timing lock first (scripts/boxlib.py). Logs go to
# scratch/x86-docker/. No clock is read: an emulated run has no speed.
set -uo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"
IMAGE="${X86_IMAGE:-yah265-x86:24.04}"
QIMAGE="${X86_QIMAGE:-yah265-qemu:24.04}"
LEGS="${LEGS:-build checkasm test conf qemu}"
OUT="$ROOT/scratch/x86-docker"
mkdir -p "$OUT"

has_leg() { case " $LEGS " in *" $1 "*) return 0 ;; *) return 1 ;; esac; }
wait_box() {
    python3 -c "import sys; sys.path.insert(0, 'scripts'); import boxlib; boxlib.wait_untimed('x86_docker $1')"
}

# tests/corpus is a symlink in a worktree; its target is mounted at its own
# path so the link resolves inside the container
mounts=(-v "$ROOT:/work")
if [ -L "$ROOT/tests/corpus" ]; then
    c="$(cd "$ROOT/tests/corpus" && pwd -P)"
    mounts+=(-v "$c:$c:ro")
fi
# the container's conformance fixtures stay out of the host's cache
mkdir -p "$OUT/fixtures"
mounts+=(-v "$OUT/fixtures:/work/tests/.fixtures")
DE265_SRC=""
if [ -n "${LIBDE265_SRC:-}" ]; then
    DE265_SRC="$(cd "$LIBDE265_SRC" && pwd -P)"
    mounts+=(-v "$DE265_SRC:$DE265_SRC:ro")
fi
DEC265="libde265-dec265"
[ -n "$DE265_SRC" ] && DEC265="scratch/x86-docker/libde265/dec265/dec265"

dk() {   # dk <platform> <image> <shell command>
    docker run --rm --platform "$1" --cpus 4 -u "$(id -u):$(id -g)" \
        -e HOME=/tmp -e XDG_CACHE_HOME=/tmp/.cache -w /work \
        "${mounts[@]}" "$2" bash -c "$3"
}

rows=()
fail=0
row() { rows+=("$(printf '%-9s %-28s %-4s %s' "$1" "$2" "$3" "${4:-}")"); \
        printf '  %-9s %-28s %-4s %s\n' "$1" "$2" "$3" "${4:-}"; \
        [ "$3" = FAIL ] && fail=$((fail + 1)); return 0; }
run() {   # run <leg> <name> <log> <platform> <image> <cmd>
    local log="$OUT/$3"
    if dk "$4" "$5" "$6" > "$log" 2>&1; then
        row "$1" "$2" PASS "$3"
    else
        row "$1" "$2" FAIL "$3 (rc $?)"
    fi
}

img() {   # img <tag> <dockerfile> <platform>
    # a daemon that is still starting can report a present image missing
    for _ in 1 2 3 4 5 6; do
        docker image inspect "$1" > /dev/null 2>&1 && return 0
        sleep 5
    done
    wait_box image
    docker build --platform "$3" -t "$1" -f "$2" docker/x86 \
        > "$OUT/image-$(basename "$2").log" 2>&1
}
img "$IMAGE" docker/x86/Dockerfile linux/amd64 ||
    { echo "x86_docker: $IMAGE did not build"; exit 1; }
if has_leg qemu; then
    img "$QIMAGE" docker/x86/Dockerfile.qemu "linux/$(uname -m)" ||
        { echo "x86_docker: $QIMAGE did not build"; exit 1; }
fi

A=linux/amd64
B8=build-gcc B10=build10-gcc S8=build-gcc-static S10=build10-gcc-static

if has_leg build; then
    wait_box build
    run build "gcc 8-bit" build8.log $A "$IMAGE" "set -e
        gcc --version | head -1
        [ -f $B8/build.ninja ] || CC=gcc meson setup $B8
        ninja -C $B8 -j 4"
    run build "gcc Main10" build10.log $A "$IMAGE" "set -e
        [ -f $B10/build.ninja ] || CC=gcc meson setup $B10 -Dbit_depth=10
        ninja -C $B10 -j 4"
    if [ -n "$DE265_SRC" ]; then
        run build "dec265 from source" dec265.log $A "$IMAGE" "set -e
            cmake -S $DE265_SRC -B scratch/x86-docker/libde265 \
                -DCMAKE_BUILD_TYPE=Release -DENABLE_SDL=OFF -DENABLE_ENCODER=OFF
            cmake --build scratch/x86-docker/libde265 -j 4 --target dec265"
    fi
    run build "gcc static checkasm" build-static.log $A "$IMAGE" "set -e
        [ -f $S8/build.ninja ] || CC=gcc meson setup $S8 -Dc_link_args=-static
        [ -f $S10/build.ninja ] || CC=gcc meson setup $S10 -Dbit_depth=10 -Dc_link_args=-static
        ninja -C $S8 -j 4 tests/checkasm
        ninja -C $S10 -j 4 tests/checkasm"
    w="$(cat "$OUT/build8.log" "$OUT/build10.log" | grep -c 'warning:')"
    row build "gcc warnings" NOTE "$w, in build8.log and build10.log"
fi

if has_leg checkasm; then
    wait_box checkasm
    for b in $B8 $B10; do
        for t in c sse42 avx2; do
            run checkasm "$b $t" "checkasm-$b-$t.log" $A "$IMAGE" \
                "N265_CPU_MAX=$t $b/tests/checkasm; r=\$?; [ \$r = 0 ] || [ \$r = 77 ]"
        done
    done
fi

if has_leg test; then
    wait_box test
    run test "make test" test.log $A "$IMAGE" \
        "make test BUILD=$B8 BUILD10=$B10"
fi

if has_leg conf; then
    wait_box conf
    for t in avx2 sse42; do
        run conf "8-bit $t" "conf8-$t.log" $A "$IMAGE" \
            "N265_CPU_MAX=$t N265=$B8/n265 DEC265=$DEC265 scripts/conformance.sh --fast"
        run conf "Main10 $t" "conf10-$t.log" $A "$IMAGE" \
            "N265_CPU_MAX=$t N265_10=$B8/n265 DEC265=$DEC265 scripts/conformance.sh --depth10 --fast"
    done
fi

if has_leg qemu; then
    wait_box qemu
    for b in $S8 $S10; do
        for cpu in Nehalem Haswell max; do
            run qemu "$b -cpu $cpu" "qemu-$b-$cpu.log" "linux/$(uname -m)" "$QIMAGE" \
                "qemu-x86_64 -cpu $cpu $b/tests/checkasm"
        done
    done
fi

{
    echo "x86_docker report: image $IMAGE, legs [$LEGS]"
    printf '%s\n' "${rows[@]}"
    echo "failures: $fail"
} | tee "$OUT/report.txt"
[ "$fail" = 0 ]
