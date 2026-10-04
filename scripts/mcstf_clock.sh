#!/bin/bash
# Copyright (c) 2026, the yah265 authors
# SPDX-License-Identifier: GPL-2.0-or-later
#
# The clock for phase 14 item 31: what --mcstf costs at the goal preset.
# TIMED, so it wants the quiet box (load under 3) and nothing else
# running; about 12 minutes on the M5 Max.
#
# Two arms on one binary, so nothing but the filter differs:
#   on   N265_MCSTF=1, the environment door onto the filter at its
#        defaults (the flag would also land in the identification SEI's
#        options; the knob moves nothing but the filter)
#   off  the default, filter off
# All four clips are above the clean floor, so every "on" run pays the
# filter's motion search. A clean clip (sita_toon, sunflower) pays only
# the noise reading and is not worth a clock.
# Four clips, CRF 28, at --threads 1 (goal 1's shape) and at auto
# (goal 3's), three rounds with the arm order reversed on alternate
# rounds, output to /dev/null, the one-minute load read before every run.
# Medians and the on/off ratio print at the end; every run lands in
# $OUT/mcstf_clock.csv.
#
#   scripts/mcstf_clock.sh [N265] [OUT]
set -u
ROOT=$(cd "$(dirname "$0")/.." && pwd)
BIN=${1:-$ROOT/build-rel/n265}
OUT=${2:-$ROOT/scratch/clock}
mkdir -p "$OUT"
CSV=$OUT/mcstf_clock.csv
CLIPS=(
    "$ROOT/tests/corpus/foreman_cif.y4m"
    "$ROOT/tests/corpus/park_joy_720p.y4m"
    "$ROOT/tests/corpus/crowd_run_1080p.y4m"
    "$ROOT/tests/corpus/touchdown_1080p.y4m"
)
FRAMES=120
ROUNDS=3

load1() { sysctl -n vm.loadavg | awk '{print $2}'; }

echo "clip,threads,arm,round,wall_s,load1" > "$CSV"
run() {
    local clip=$1 thr=$2 arm=$3 r=$4 l t0 t1
    local -a env_arm=()
    [ "$arm" = on ] && env_arm=(N265_MCSTF=1)
    l=$(load1)
    t0=$(perl -MTime::HiRes=time -e 'printf "%.6f", time')
    env ${env_arm[@]+"${env_arm[@]}"} "$BIN" "$clip" --frames $FRAMES --crf 28 \
        --threads "$thr" -o /dev/null 2> /dev/null
    t1=$(perl -MTime::HiRes=time -e 'printf "%.6f", time')
    echo "$(basename "$clip" .y4m),$thr,$arm,$r,$(echo "$t1 - $t0" | bc),$l" >> "$CSV"
}

for c in "${CLIPS[@]}"; do cat "$c" > /dev/null; done
for thr in 1 0; do
    for r in $(seq 1 $ROUNDS); do
        for c in "${CLIPS[@]}"; do
            if [ $((r % 2)) -eq 1 ]; then
                run "$c" $thr on "$r"; run "$c" $thr off "$r"
            else
                run "$c" $thr off "$r"; run "$c" $thr on "$r"
            fi
        done
    done
done

python3 - "$CSV" <<'EOF'
import csv, statistics, sys
rows = list(csv.DictReader(open(sys.argv[1])))
cells = {}
for r in rows:
    cells.setdefault((r["clip"], r["threads"]), {}).setdefault(
        r["arm"], []).append(float(r["wall_s"]))
print(f"{'clip':<18} {'thr':>4} {'off s':>8} {'on s':>8} {'on/off':>7}")
for (clip, thr), d in cells.items():
    off, on = statistics.median(d["off"]), statistics.median(d["on"])
    print(f"{clip:<18} {'auto' if thr == '0' else thr:>4} {off:8.3f} "
          f"{on:8.3f} {on / off:7.3f}x")
print("max load1 seen:", max(float(r["load1"]) for r in rows))
EOF
