#!/usr/bin/env bash
# Copyright (c) 2026, the yah265 authors
# SPDX-License-Identifier: GPL-2.0-or-later
# Ported from yah264 (codec-agnostic instrument; the encoder binary, its flags
# and the reference encoder are the only changes).
#
# determ_repeat.sh - same binary, same config, same thread count, N times.
# Every run must produce one bitstream.
#
# This is the gate that was missing. The tree's determinism scripts compare
# ACROSS thread counts (stair_determ.sh) or against a reference build
# (w2_canary.sh); nothing re-ran one configuration and compared it with itself,
# so a shipped default spent a day emitting 3-5 distinct bitstreams per 12 runs
# at foreman --ref 1 t18 without any battery noticing. TSan is silent on it --
# the reader was consuming a flag published before the data it advertised, which
# is a happens-before violation TSan can miss when the write lands first often
# enough.
#
# Thread-VARIANT output is allowed here by owner ruling; same-config
# reproducibility is not optional, because every identity gate in this tree
# (escape-env md5 equality, A/B md5 difference, delete probes gated on output
# identity) reads a lottery ticket without it.
#
#   scripts/determ_repeat.sh                       # the default matrix
#   RUNS=20 CLIPS='foreman_cif' scripts/determ_repeat.sh
#   ARM='N265_AQ_FLAT=2' scripts/determ_repeat.sh    # gate an arm the same way
#   ARGS='--ctu 64' scripts/determ_repeat.sh   # ...or a MODE
#
# ARM is env, ARGS is encoder flags, and they are not interchangeable: a flag in
# the ARM slot makes env(1) reject the whole command, every run then produces
# nothing, and the nothings all have the same md5 -- a clean sweep for an encode
# that never ran. scripts/stair_determ.sh grew the same pair of slots after that
# happened there on 2026-09-01.
set -u

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
ENC="${ENC:-$ROOT/build/n265}"
WORK="${WORK:-${TMPDIR:-/tmp}/determ_repeat.$$}"
RUNS="${RUNS:-12}"
FRAMES="${FRAMES:-120}"
# sita_flat_720p joined the default set on 2026-09-10: its motion reach is
# the largest measured (598 lines at CTB 16), which is what turned an
# out-of-bounds reference read into visible non-determinism.
CLIPS="${CLIPS:-foreman_cif bus_cif stefan_cif park_joy_720p sita_flat_720p}"
THREADS="${THREADS:-1 2 4 12}"   # widened at T1 (threading-design sec 7)
REFS="${REFS:-b0 b1}"           # bframes off/on, the configs that exist
ARM="${ARM:-}"
# ...and the default ARGS carry a CTB-16 pass with a deep reference
# structure for the same reason: the encoder's default is CTB 32 and
# --ref 1, so nothing in this battery had ever visited the corner.
ARGS="${ARGS:---ctu 16 --ref 3}"

mkdir -p "$WORK"
trap 'rm -rf "$WORK"' EXIT

fails=0; total=0
for clip in $CLIPS; do
    src="$ROOT/tests/corpus/$clip.y4m"
    [ -f "$src" ] || { echo "  skip $clip (no clip)"; continue; }
    for r in $REFS; do
        for t in $THREADS; do
            total=$((total + 1))
            # Two things about this loop's stdout, both of which bit.
            #
            # The diagnostics go to STDERR. The loop's stdout is the hash file,
            # so an unredirected `echo ENCFAIL` was written INTO it -- invisible
            # to the operator, and counted as a distinct "bitstream", which is
            # how an encode that never ran surfaced as "2 distinct bitstreams"
            # instead of as a producer failure. The script's own header warns
            # about exactly this shape from the other direction.
            #
            # And an abort is carried in its own flag, not in n. Breaking out
            # leaves the partial hashes behind, and identical hashes followed by
            # an abort sort -u to 1 -- a clean pass for a config that never
            # finished. n is recomputed below, so it cannot hold the signal.
            # (The loop does not fork -- the redirect is on `done` -- so the
            # flag survives.)
            aborted=0
            for i in $(seq "$RUNS"); do
                out="$WORK/r.hevc"; rm -f "$out"
                # shellcheck disable=SC2086
                env $ARM "$ENC" "$src" --frames "$FRAMES" --bframes "${r#b}" \
                    --qp 26 --aq 1 --cutree 1 --threads "$t" $ARGS -o "$out" >/dev/null 2>&1 || {
                        echo "  ENCFAIL $clip ref$r t$t" >&2; aborted=1; break; }
                # Guard the producer: two missing files have equal md5s.
                [ -s "$out" ] || { echo "  EMPTY $clip ref$r t$t" >&2; aborted=1; break; }
                md5 -q "$out" 2>/dev/null || md5sum "$out" | cut -d' ' -f1
            done > "$WORK/hashes"
            n=$(sort -u "$WORK/hashes" | wc -l | tr -d ' ')
            if [ "$aborted" = 1 ]; then
                echo "  FAIL $clip ref$r t$t: aborted after $(wc -l < "$WORK/hashes" | tr -d ' ') of $RUNS runs"
                fails=$((fails + 1))
            elif [ "$n" != 1 ]; then
                echo "  FAIL $clip ref$r t$t: $n distinct bitstreams in $RUNS runs"
                fails=$((fails + 1))
            fi
        done
    done
done
echo "DETERM-REPEAT $((total - fails))/$total configs reproducible over $RUNS runs each  arm='${ARM:-<default>}' args='${ARGS:-<default>}'"
[ "$fails" -eq 0 ]
