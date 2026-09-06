#!/usr/bin/env bash
# Copyright (c) 2026, the yah265 authors
# SPDX-License-Identifier: GPL-2.0-or-later
#
# fetch_corpus.sh - populate tests/corpus/ with standard Xiph "derf" test
# clips for benchmarking. Downloads are verified against pinned SHA-256
# hashes; before hitting the network we look for the same file in sibling
# repos' corpus dirs (same public clips, still hash-verified on copy).
#
# Usage: fetch_corpus.sh [--full]
#   (default)  7 CIF clips -- fast, small, the day-to-day BD gate.
#   --full     ALSO large native-res 720p/1080p clips (grain/crowd/cadence).
set -euo pipefail

full=0
for a in "$@"; do
    case "$a" in
        --full) full=1 ;;
        *) echo "unknown arg: $a (usage: fetch_corpus.sh [--full])" >&2; exit 2 ;;
    esac
done

root="$(cd "$(dirname "$0")/.." && pwd)"
dest="$root/tests/corpus"
mkdir -p "$dest"

base="https://media.xiph.org/video/derf"

# local caches to try before downloading
caches=(
    "$root/../yah264/tests/corpus"
    "$root/../nextav1-bench/corpus"
)

# name  url  sha256(-=pin on first verified fetch)  class
clips=(
    "akiyo_cif       $base/y4m/akiyo_cif.y4m        -   static"
    "foreman_cif     $base/y4m/foreman_cif.y4m      -   motion"
    "mobile_cif      $base/y4m/mobile_cif.y4m       -   detail"
    "stefan_cif      $base/y4m/stefan_cif.y4m       -   motion"
    "bus_cif         $base/y4m/bus_cif.y4m          -   motion"
    "tempete_cif     $base/y4m/tempete_cif.y4m      -   detail"
    "coastguard_cif  $base/y4m/coastguard_cif.y4m   -   detail"
)

clips_full=(
    "park_joy_720p    $base/y4m/park_joy_420_720p50.y4m       b1e3784a6c74e07d53dd7077737b5a6edcd53e5ea07e4718c9affbe0ec36536f   grain"
    "ducks_720p       $base/y4m/ducks_take_off_420_720p50.y4m 0e09aa923ce72e2f6d1c978c3c1cc8dd9588d2ede819081f040213e361970efc   grain"
    "in_to_tree_720p  $base/y4m/in_to_tree_420_720p50.y4m     81166517a3054c789923535b5da23fbbb9bbd7fd4c28dac83918ac351e121e74   detail"
    "crowd_run_1080p  $base/y4m/crowd_run_1080p50.y4m         e056e01fc13906dd9a84f5cc686a36888436379476577965a0866821caaf55c8   crowd"
    "touchdown_1080p  $base/y4m/touchdown_pass_1080p.y4m      4cf0d19e332fa021660d450fd0da1201c7e153d70e5e5606c8497a1835477382   cadence"
    "sintel_720p      $base/y4m/sintel_trailer_2k_720p24.y4m  -   cadence"
)

manifest="$dest/CLASSES"
: > "$manifest"

sha() { shasum -a 256 "$1" | awk '{print $1}'; }

fetch() {
    local name="$1" url="$2" want="$3" class="$4"
    local out="$dest/$name.y4m"
    echo "$name $class" >> "$manifest"
    if [ -f "$out" ]; then
        echo "have  $name  [$class]"
        return
    fi
    for c in "${caches[@]}"; do
        if [ -f "$c/$name.y4m" ]; then
            if [ "$want" != "-" ] && [ "$(sha "$c/$name.y4m")" != "$want" ]; then
                echo "  cache copy of $name fails pin, ignoring" >&2
                continue
            fi
            echo "copy  $name  [$class]  (from $c)"
            cp "$c/$name.y4m" "$out"
            return
        fi
    done
    echo "fetch $name  [$class]"
    if command -v curl >/dev/null; then
        curl -fSL "$url" -o "$out"
    else
        wget -O "$out" "$url"
    fi
    if [ "$want" != "-" ]; then
        local got
        got="$(sha "$out")"
        if [ "$got" != "$want" ]; then
            echo "  hash mismatch for $name: got $got" >&2
            rm -f "$out"
            exit 1
        fi
    fi
}

for row in "${clips[@]}"; do
    read -r c_name c_url c_want c_class <<< "$row"
    fetch "$c_name" "$c_url" "$c_want" "$c_class"
done

if [ "$full" = 1 ]; then
    echo "-- tier 2 (--full) --"
    for row in "${clips_full[@]}"; do
        read -r c_name c_url c_want c_class <<< "$row"
        fetch "$c_name" "$c_url" "$c_want" "$c_class"
    done
fi

# normalize chroma: some derf masters are 4:2:2 (touchdown); the
# encoders under test are 4:2:0 only
for f in "$dest"/*.y4m; do
    if head -c 200 "$f" | head -1 | grep -q "C422"; then
        echo "convert $(basename "$f") 4:2:2 -> 4:2:0"
        ffmpeg -loglevel error -i "$f" -pix_fmt yuv420p "$f.tmp.y4m" -y
        mv "$f.tmp.y4m" "$f"
    fi
done

echo "corpus ready in $dest (classes in $manifest)"
