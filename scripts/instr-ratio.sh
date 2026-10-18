#!/usr/bin/env bash
# Copyright (c) 2026, the yah265 authors
# SPDX-License-Identifier: GPL-2.0-or-later
#
# instr-ratio.sh -- the INSTRUCTION ratio beside the wall ratio, ours over
# x265's, one run a side. Ported from yah264's scripts/instr-ratio.sh.
#
# Wall is instructions times cycles per instruction. A row that reads 1.4x
# can be 1.4x the work, or the same work at 1.4x the CPI, and the two have
# different fixes. Instruction counts are near-deterministic where wall is
# not, which is why one run a side is enough and why this can be read on a
# busy box: the instr column holds under load, the wall column does not.
# If the two ratios track, the row is work volume and the fix is to do
# less. If they diverge, the row is CPI.
#
# Goal 1's shape by default: pure C, one thread, both sides
# (N265_ASM_OFF=all against x265 --no-asm --pools none --frame-threads 1),
# at each clip's solved goal-1 CRF pair from the newest default-arm block
# of docs/data/goal-legs.csv, so a row sits beside that table. Both
# encoders read the y4m from stdin.
#
# Usage: scripts/instr-ratio.sh [frames]          (default 60)
# Env:
#   CLIPS   "clip:ourcrf:x265crf ..." (default: the goal-1 CIF block)
#   TIER    c (default) or simd: simd drops N265_ASM_OFF and --no-asm
#   PRESET / X265PRESET   medium / medium
#   N265    default build-rel/n265 (build/n265 when there is no release tree)
#   X265    default x265 on PATH
set -uo pipefail
root="$(cd "$(dirname "$0")/.." && pwd)"
FRAMES="${1:-60}"
PRESET="${PRESET:-medium}"
X265PRESET="${X265PRESET:-medium}"
TIER="${TIER:-c}"
N265="${N265:-$root/build-rel/n265}"
[ -x "$N265" ] || N265="$root/build/n265"
X265="${X265:-x265}"

python3 "$root/scripts/boxlib.py" lock wait --what instr-ratio || exit 1

# every N265_* the caller set is an arm: it rides into OUR side only, and
# the header says so, so a row can't be mistaken for the default
arm=$(env | sed -n 's/^\(N265_[A-Za-z0-9_]*=.*\)$/\1/p' | grep -v '^N265_ASM_OFF=' | sort | tr '\n' ' ')

if [ -z "${CLIPS:-}" ]; then
    CLIPS=$(python3 - "$root/docs/data/goal-legs.csv" <<'PY'
import csv, sys
try:
    rows = list(csv.DictReader(open(sys.argv[1])))
except OSError:
    rows = []
g = [r for r in rows if r["goal"] == "1" and r["preset"] == "medium"
     and not r.get("arm") and (r.get("x265_preset") or "medium") == "medium"]
if g:
    last = (g[-1]["date"], g[-1]["commit"])
    print(" ".join(f"{r['clip']}:{r['our_crf']}:{r['x265_crf']}" for r in g
                   if (r["date"], r["commit"]) == last and r["clip"].endswith("_cif")))
else:
    print("foreman_cif:28:28 mobile_cif:28:28")
PY
)
fi

noasm_env=(N265_ASM_OFF=all)
noasm_x=(--no-asm)
if [ "$TIER" = simd ]; then noasm_env=(); noasm_x=(); fi

measure() {  # measure SRC CMD... -> json on stdout
    local src="$1"; shift
    python3 "$root/scripts/boxlib.py" measure --json --quiet -- "$@" < "$src"
}

echo "instr-ratio: $FRAMES frames, tier $TIER, ours $PRESET vs x265 $X265PRESET, one thread"
echo "  ours $N265${arm:+  arm $arm}"
echo "  x265 $("$X265" --version 2>&1 | tr '\r' '\n' | sed -n 's/.*version //p' | head -1)"
printf '%-16s %9s %11s %11s %8s %8s %8s %13s\n' clip crf "ours instr" "x265 instr" "instr x" "cyc x" "wall x" "rss MB o/x"
printf '%-16s %9s %11s %11s %8s %8s %8s %13s\n' ---------------- --------- ----------- ----------- -------- -------- -------- -------------
for entry in $CLIPS; do
    clip="${entry%%:*}"; rest="${entry#*:}"
    oq="${rest%%:*}"; xq="${rest##*:}"
    src="$root/tests/corpus/$clip.y4m"
    [ -f "$src" ] || { printf '%-16s %9s\n' "$clip" "(missing)"; continue; }
    o=$(measure "$src" env ${noasm_env[@]+"${noasm_env[@]}"} "$N265" - --frames "$FRAMES" \
            --crf "$oq" --preset "$PRESET" --threads 1 -o /dev/null)
    x=$(measure "$src" "$X265" --input - --y4m --preset "$X265PRESET" \
            --crf "$xq" --frames "$FRAMES" --pools none --frame-threads 1 \
            ${noasm_x[@]+"${noasm_x[@]}"} --log-level none -o /dev/null)
    python3 - "$clip" "$oq/$xq" "$o" "$x" <<'PY'
import json, sys
c, q, o, x = sys.argv[1], sys.argv[2], json.loads(sys.argv[3]), json.loads(sys.argv[4])
if o["rc"] or x["rc"]:
    print(f"{c:<16} {q:>9} FAILED (rc ours {o['rc']}, x265 {x['rc']})")
elif not o["instr"] or not x["instr"]:
    print(f"{c:<16} {q:>9} NO-COUNTER")
else:
    cyc = (f"{o['cycles'] / x['cycles']:7.2f}x"
           if o["cycles"] and x["cycles"] else f"{'-':>8}")
    print(f"{c:<16} {q:>9} {o['instr']/1e9:10.2f}G {x['instr']/1e9:10.2f}G "
          f"{o['instr']/x['instr']:7.2f}x {cyc} {o['wall']/x['wall']:7.2f}x "
          f"{o['rss']/2**20:6.0f}/{x['rss']/2**20:<6.0f}")
PY
done
echo
echo "instr x, cyc x and wall x are ours/x265: above 1 means we do more / take longer."
echo "instr and wall track => the row is WORK VOLUME. They diverge => it is CPI."
