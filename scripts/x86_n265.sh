#!/usr/bin/env bash
# Copyright (c) 2026, the yah265 authors
# SPDX-License-Identifier: GPL-2.0-or-later
#
# x86_n265.sh - the x86-64 n265 as a drop-in N265 for the gate scripts on
# an Apple Silicon Mac (docs/x86-plan.md).
#
#   N265=scripts/x86_n265.sh scripts/conformance.sh --fast
#
# It sets ROSETTA_ADVERTISE_AVX=1, without which Rosetta hides AVX2 from
# CPUID and the encoder takes the C path, and runs the binary at nice 10.
# A `--threads N` above X86_MAX_THREADS (or 0, auto) is lowered to it, so
# a vector written for 12 threads stays inside the shared box's cap. The
# gate it serves checks the reconstruction against the decoders, which
# holds at any thread count.
#
# Environment:
#   X86_N265          the binary (default build-x86/n265 in this tree)
#   X86_MAX_THREADS   the thread cap (default 4)
set -u

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
bin="${X86_N265:-$ROOT/build-x86/n265}"
cap="${X86_MAX_THREADS:-4}"
args=()
next=0
for a in "$@"; do
    if [ "$next" = 1 ]; then
        next=0
        case "$a" in
            ''|*[!0-9]*) ;;
            *) if [ "$a" -eq 0 ] || [ "$a" -gt "$cap" ]; then a="$cap"; fi ;;
        esac
    elif [ "$a" = --threads ]; then
        next=1
    fi
    args+=("$a")
done
export ROSETTA_ADVERTISE_AVX=1
exec nice -n 10 "$bin" "${args[@]}"
