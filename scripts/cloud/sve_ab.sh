#!/bin/bash
# Copyright (c) 2026, the yah265 authors
# SPDX-License-Identifier: GPL-2.0-or-later
#
# sve_ab.sh -- the SVE tier's encode-level A/B on a Graviton box: one
# thread, the default binary against N265_CPU_MAX=neon, arms interleaved,
# cycles and instructions from perf stat (the whole encode, user space).
# The streams of both arms must match; a cell that differs is printed as
# DIFF.
#
#   sve_ab.sh BIN ROUNDS CLIP:FRAMES[:EXTRA] ...
#   e.g. sve_ab.sh src/build-rel/y265 3 park_joy_720p.y4m:60 \
#            foreman_cif.y4m:60:--output-depth=10
set -u
BIN=$1; ROUNDS=$2; shift 2
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT

one() {   # one ARM CLIP FRAMES EXTRA -> "cycles instructions md5"
    local arm=$1 clip=$2 frames=$3 extra=$4 envs=()
    [ "$arm" = neon ] && envs=(N265_CPU_MAX=neon)
    local out="$tmp/$arm.hevc"
    env "${envs[@]}" perf stat -x, -e cycles:u,instructions:u -o "$tmp/ps" \
        "$BIN" "$clip" --frames "$frames" --threads 1 ${extra//=/ } \
        -o "$out" >/dev/null 2>&1
    local c i
    c=$(awk -F, '/cycles/{print $1}' "$tmp/ps")
    i=$(awk -F, '/instructions/{print $1}' "$tmp/ps")
    echo "$c $i $(md5sum < "$out" | cut -c1-12)"
}

for cell in "$@"; do
    IFS=: read -r clip frames extra <<< "$cell"
    cs=(); cn=(); is=(); in=(); ms=""; mn=""
    for r in $(seq "$ROUNDS"); do
        for arm in sve neon; do
            [ $(( r % 2 )) = 0 ] && arm=$([ $arm = sve ] && echo neon || echo sve)
            read -r c i m <<< "$(one "$arm" "$clip" "$frames" "${extra:-}")"
            if [ "$arm" = sve ]; then cs+=("$c"); is+=("$i"); ms=$m
            else cn+=("$c"); in+=("$i"); mn=$m; fi
        done
    done
    python3 - "$clip ${extra:-}" "$ms" "$mn" "${cs[*]}" "${cn[*]}" "${is[*]}" "${in[*]}" <<'EOF'
import sys, statistics as st
name, ms, mn = sys.argv[1:4]
cs, cn, i_s, i_n = (list(map(int, a.split())) for a in sys.argv[4:8])
mc_s, mc_n = st.median(cs), st.median(cn)
print(f"{name:40s} cycles sve/neon {mc_s/mc_n:.4f} (sve {mc_s/1e9:.3f}G neon {mc_n/1e9:.3f}G, "
      f"spread {min(cs)/max(cs):.3f}/{min(cn)/max(cn):.3f})  instr {st.median(i_s)/st.median(i_n):.4f}  "
      f"stream {'SAME' if ms == mn else 'DIFF'}")
EOF
done
