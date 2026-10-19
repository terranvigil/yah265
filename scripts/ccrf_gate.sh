#!/usr/bin/env bash
# Copyright (c) 2026, the yah265 authors
# SPDX-License-Identifier: GPL-2.0-or-later
#
# ccrf_gate.sh - the capped-CRF buffer gate (phase 14 item 47).
#
# Capped CRF (--crf with --vbv-maxrate / --vbv-bufsize) declares an HRD
# buffer, and a stream that underflows it is non-conformant. This runs
# scripts/vbv_check.py in MODE=crf over every clip with a ladder (the
# seventeen corpus clips), at three buffer shapes and three CRFs, and
# fails on any underflow, desync or SEI disagreement. The shape follows
# yah264's capped-CRF compliance gate: many short cells, each clean or
# not, rather than one long read.
#
#   scripts/ccrf_gate.sh
#   N265=build-rel/n265 THREADS=6 JOBS=1 scripts/ccrf_gate.sh
#   SEG=1 scripts/ccrf_gate.sh          # also the dev set's D5v, whole
#
# A pair is `<multiple of the clip's uncapped CRF rate>x<buffer seconds>`.
# Environment: N265 (default build/n265), CRFS (default "22 28 34"),
# PAIRS (default 1.0x0.5s,1.2x1.0s,1.5x2.0s), THREADS (default 1), JOBS
# (default 6; keep JOBS x THREADS at or under the core share), CLIPS,
# FRAMES (default 120). SEG=1 adds chimera_720p_120s from SEG_DIR
# (default ~/media/tier_d), all 2760 frames, at THREADS and SEG_JOBS (1).
set -u
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
CRFS="${CRFS:-22 28 34}"
PAIRS="${PAIRS:-1.0x0.5s,1.2x1.0s,1.5x2.0s}"
THREADS="${THREADS:-1}"
JOBS="${JOBS:-6}"
SEG_DIR="${SEG_DIR:-$HOME/media/tier_d}"
bad=0
for crf in $CRFS; do
    echo "=== capped CRF $crf, corpus, t$THREADS ==="
    MODE=crf CRF="$crf" PAIRS_CRF="$PAIRS" THREADS="$THREADS" JOBS="$JOBS" \
        python3 "$ROOT/scripts/vbv_check.py" > "${TMPDIR:-/tmp}/ccrf_gate.$$" 2>&1
    rc=$?
    sed -n '/=== VBV underflow table/,/^(cell/p' "${TMPDIR:-/tmp}/ccrf_gate.$$"
    grep -E '^total|^SEI:|SEI (ERROR|DISAGREES)|ERROR' "${TMPDIR:-/tmp}/ccrf_gate.$$"
    [ $rc -eq 0 ] || bad=1
    if [ "${SEG:-}" = 1 ]; then
        echo "=== capped CRF $crf, D5v whole segment, t$THREADS ==="
        MODE=crf CRF="$crf" PAIRS_CRF="$PAIRS" THREADS="$THREADS" JOBS="${SEG_JOBS:-1}" \
            CORPUS="$SEG_DIR" CLIPS=chimera_720p_120s FRAMES=2760 \
            python3 "$ROOT/scripts/vbv_check.py" > "${TMPDIR:-/tmp}/ccrf_gate.$$" 2>&1
        rc=$?
        grep -E 'min fill|^total' "${TMPDIR:-/tmp}/ccrf_gate.$$"
        [ $rc -eq 0 ] || bad=1
    fi
done
rm -f "${TMPDIR:-/tmp}/ccrf_gate.$$"
if [ $bad -ne 0 ]; then
    echo "ccrf_gate: FAIL"
    exit 1
fi
echo "ccrf_gate: PASS"
