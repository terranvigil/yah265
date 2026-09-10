#!/usr/bin/env bash
# Copyright (c) 2026, the yah265 authors
# SPDX-License-Identifier: GPL-2.0-or-later
#
# determ_gate.sh - the determinism matrix of docs/threading-design.md sec 3,
# widened on 2026-09-10 after an out-of-bounds reference read spent the whole
# ladder emitting several bitstreams per twelve-thread run at one corner of
# the config space that no battery visited (`sita_flat_720p --ctu 16 --ref 3
# --bframes 7`).
#
# The axes are crossed, not sampled, because that corner is what the sampling
# missed: CTB {16, 32, 64} x SAO {off, on} x RC {CQP, CRF, ABR} x bframes
# {0, 3, 7} x ref {1, 3}, per clip, at threads {1, 2, 4, 12} with the twelve
# -thread cell repeated five times, all of it under background load (the box
# is otherwise idle, and yah264's gate passed on an idle box against a default
# that emitted six distinct bitstreams under load).
#
# PASS for a cell = one distinct hash across every thread count and every
# repetition, over the BITSTREAM and the RECON DUMP together. The recon leg
# matters: a decision that differs only inside a frame that nothing references
# can leave the bitstream alone and still be a race.
#
#   scripts/determ_gate.sh                            # the full matrix
#   CLIPS=foreman_cif scripts/determ_gate.sh          # one clip
#   SPINNERS=0 scripts/determ_gate.sh                 # idle box (NOT the gate)
#   CTBS='16' RCS='cqp' scripts/determ_gate.sh        # a slice, while bisecting
#
# ARM is env assignments prefixed onto the encoder, ARGS is encoder flags, and
# they are not interchangeable - a flag in the ARM slot makes env(1) reject the
# command and every empty output then hashes the same (determ_repeat.sh's
# header records the day that happened).
set -u

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
ENC="${ENC:-$ROOT/build/n265}"
WORK="${WORK:-${TMPDIR:-/tmp}/determ_gate.$$}"

CLIPS="${CLIPS:-foreman_cif sita_flat_720p}"
FRAMES="${FRAMES:-60}"
THREADS="${THREADS:-1 2 4 12}"
REPEAT_T="${REPEAT_T:-12}"      # the thread count that is repeated
REPEAT="${REPEAT:-5}"
SPINNERS="${SPINNERS:-8}"
CTBS="${CTBS:-16 32 64}"
SAOS="${SAOS:-off on}"
RCS="${RCS:-cqp crf abr}"
BFRAMES="${BFRAMES:-0 3 7}"
REFS="${REFS:-1 3}"
ARM="${ARM:-}"
ARGS="${ARGS:---aq 1 --cutree 1}"

[ -x "$ENC" ] || { echo "determ_gate: $ENC not built" >&2; exit 1; }
mkdir -p "$WORK"

spin_pids=""
cleanup() {
    [ -n "$spin_pids" ] && kill $spin_pids 2>/dev/null
    rm -rf "$WORK"
}
trap cleanup EXIT INT TERM

i=0
while [ "$i" -lt "$SPINNERS" ]; do
    ( while :; do :; done ) &
    spin_pids="$spin_pids $!"
    i=$((i + 1))
done
[ "$SPINNERS" -gt 0 ] && echo "determ_gate: $SPINNERS spinners up"

# ABR needs a rate in the clip's own ballpark or the gate measures the
# clamp instead of the rate loop.
rate_for() {
    case "$1" in
    *_1080p) echo 4000 ;;
    *_720p)  echo 1500 ;;
    *)       echo 400 ;;
    esac
}

hash_of() { md5 -q "$1" 2>/dev/null || md5sum "$1" | cut -d' ' -f1; }

pass=0; fail=0; cells=0
for clip in $CLIPS; do
    src="$ROOT/tests/corpus/$clip.y4m"
    [ -f "$src" ] || { echo "  skip $clip (no clip)"; continue; }
    rate=$(rate_for "$clip")
    for ctb in $CTBS; do
    for sao in $SAOS; do
    for rc in $RCS; do
    for bf in $BFRAMES; do
    for rf in $REFS; do
        cells=$((cells + 1))
        case "$rc" in
        cqp) rcarg="--qp 26" ;;
        crf) rcarg="--crf 26" ;;
        abr) rcarg="--bitrate $rate" ;;
        esac
        case "$sao" in
        on)  saoenv="N265_SAO_TYPES=3" ;;
        *)   saoenv="" ;;
        esac
        name="$clip.ctu$ctb.sao$sao.$rc.b$bf.r$rf"
        : > "$WORK/hashes"
        aborted=0
        for t in $THREADS; do
            reps=1
            [ "$t" = "$REPEAT_T" ] && reps=$REPEAT
            k=0
            while [ "$k" -lt "$reps" ]; do
                k=$((k + 1))
                rm -f "$WORK/o.hevc" "$WORK/o.yuv"
                # shellcheck disable=SC2086
                env $ARM $saoenv "$ENC" "$src" --frames "$FRAMES" \
                    --ctu "$ctb" $rcarg --bframes "$bf" --ref "$rf" \
                    --threads "$t" $ARGS \
                    --dump-recon "$WORK/o.yuv" -o "$WORK/o.hevc" \
                    >/dev/null 2>&1 || {
                        echo "  ENCFAIL $name t$t" >&2; aborted=1; break; }
                # two missing files have equal hashes; guard the producer
                [ -s "$WORK/o.hevc" ] && [ -s "$WORK/o.yuv" ] || {
                    echo "  EMPTY $name t$t" >&2; aborted=1; break; }
                echo "$(hash_of "$WORK/o.hevc") $(hash_of "$WORK/o.yuv")" \
                    >> "$WORK/hashes"
            done
            [ "$aborted" = 1 ] && break
        done
        n=$(sort -u "$WORK/hashes" | wc -l | tr -d ' ')
        if [ "$aborted" = 1 ]; then
            echo "  FAIL $name: aborted"
            fail=$((fail + 1))
        elif [ "$n" != 1 ]; then
            echo "  FAIL $name: $n distinct (bitstream,recon) pairs"
            sort -u "$WORK/hashes" | sed 's/^/    /'
            fail=$((fail + 1))
        else
            pass=$((pass + 1))
        fi
    done; done; done; done; done
done

echo "DETERM-GATE $pass/$cells cells reproducible" \
     "(threads '$THREADS', t$REPEAT_T x$REPEAT, $SPINNERS spinners," \
     "$FRAMES frames, clips '$CLIPS')"
[ "$fail" -eq 0 ]
