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

# excerpts: name  url(.xz y4m)  first_frame  nframes  sha256  class
# Streamed and cut (curl | xz | ffmpeg select), never stored whole; the
# pin is on the cut file. The 2D animation regime (flat fills, hard
# outlines, limited motion, 0.01-0.05 bpp) that sintel alone stood for.
# Sita Sings the Blues is public domain (CC0, Nina Paley 2013).
clips_excerpt=(
    "sita_flat_720p   $base/y4m/sita_sings_the_blues_720p24.y4m.xz  11400  120  dd3bb7adb868444d9cb7bb7d54ad42c58d3c572ab8e73fb697aa40a1b8b40441  animation"
    "sita_toon_720p   $base/y4m/sita_sings_the_blues_720p24.y4m.xz   8760  120  fa0ee26c3f170eac0e492ae53d399e412ca32fa6776b69f32013e50354c15e74  animation"
)

# generated: name  sha256  class.  The recipe is gen_one() below.
#
# The derf collection has no clip with a global illumination ramp, and a
# fade is the regime weighted prediction exists for (phase 14 item 2), so
# these two are SYNTHESIZED from clips already in the corpus - and the
# recipe is written here rather than in a note, so it can be re-run and
# the pin can fail.
#
#   fade_cif     foreman_cif frames 0-119, a 40-frame linear fade to
#                black over frames 20-59 and a 40-frame fade back over
#                60-99. Built as two passes and concatenated, because a
#                chained `fade=out,fade=in` is NOT that clip: each fade
#                filter blacks out everything outside its own window, so
#                the chain returns 120 black frames. (Measured: it does.)
#   xfade_720p   park_joy_720p and in_to_tree_720p, 80 frames each, with
#                an 0.8 s (40-frame) cross-fade centered on frame 60. The
#                harder half of the regime: the mean moves globally, but
#                the CONTENT is a mix of two pictures, so one weight per
#                reference can only take the illumination part of it.
#
# Both pins are on ffmpeg 9.0.1's output. A different ffmpeg whose
# fade/xfade rounding differs fails the pin rather than quietly handing
# the band a different clip.
clips_gen=(
    "fade_cif     64690791f320fd81d6060643c54d1cf8935ea97bad2139bea5d21f921ddb5576  fade  1"
    "xfade_720p   41b5de7b2ab3b352253ea55a68c3f4e08b5d34e5f451ce079d32c15da5671828  fade  2"
)

manifest="$dest/CLASSES"
: > "$manifest"

sha() { shasum -a 256 "$1" | awk '{print $1}'; }

# the two recipes above, by name. Writes "$dest/$1.y4m".
gen_one() {
    local name="$1" tmp
    tmp="$(mktemp -d "${TMPDIR:-/tmp}/n265gen.XXXXXX")"
    case "$name" in
    fade_cif)
        ffmpeg -loglevel error -y -i "$dest/foreman_cif.y4m" \
            -vf "select='between(n,0,59)',fade=t=out:s=20:n=40" \
            -fps_mode vfr -frames:v 60 -pix_fmt yuv420p \
            -f yuv4mpegpipe "$tmp/a.y4m"
        ffmpeg -loglevel error -y -i "$dest/foreman_cif.y4m" \
            -vf "select='between(n,60,119)',setpts=N/FRAME_RATE/TB,fade=t=in:s=0:n=40" \
            -fps_mode vfr -frames:v 60 -pix_fmt yuv420p \
            -f yuv4mpegpipe "$tmp/b.y4m"
        printf "file '%s'\nfile '%s'\n" "$tmp/a.y4m" "$tmp/b.y4m" > "$tmp/cat.txt"
        ffmpeg -loglevel error -y -f concat -safe 0 -i "$tmp/cat.txt" \
            -fps_mode vfr -frames:v 120 -pix_fmt yuv420p \
            -f yuv4mpegpipe "$dest/$name.y4m"
        ;;
    xfade_720p)
        ffmpeg -loglevel error -y -i "$dest/park_joy_720p.y4m" \
            -i "$dest/in_to_tree_720p.y4m" -filter_complex \
            "[0:v]select='between(n,0,79)',setpts=N/FRAME_RATE/TB[a];\
[1:v]select='between(n,0,79)',setpts=N/FRAME_RATE/TB[b];\
[a][b]xfade=transition=fade:duration=0.8:offset=0.8,format=yuv420p" \
            -fps_mode vfr -frames:v 120 -f yuv4mpegpipe "$dest/$name.y4m"
        ;;
    *) echo "  no recipe for $name" >&2; rm -rf "$tmp"; exit 1 ;;
    esac
    rm -rf "$tmp"
}

# name  sha256  class  tier(1 = always, 2 = --full, when its sources are here)
generate() {
    local name="$1" want="$2" class="$3"
    local out="$dest/$name.y4m"
    echo "$name $class" >> "$manifest"
    if [ -f "$out" ]; then
        echo "have  $name  [$class]"
        return
    fi
    for c in "${caches[@]}"; do
        if [ -f "$c/$name.y4m" ] && [ "$(sha "$c/$name.y4m")" = "$want" ]; then
            echo "copy  $name  [$class]  (from $c)"
            cp "$c/$name.y4m" "$out"
            return
        fi
    done
    echo "gen   $name  [$class]"
    gen_one "$name"
    local got
    got="$(sha "$out")"
    if [ "$got" != "$want" ]; then
        echo "  hash mismatch for $name: got $got" >&2
        echo "  (the recipe is ffmpeg's; 9.0.1 is what the pin was taken on)" >&2
        rm -f "$out"
        exit 1
    fi
}

fetch_excerpt() {
    local name="$1" url="$2" first="$3" n="$4" want="$5" class="$6"
    local out="$dest/$name.y4m" last=$((first + n - 1))
    echo "$name $class" >> "$manifest"
    if [ -f "$out" ]; then
        echo "have  $name  [$class]"
        return
    fi
    for c in "${caches[@]}"; do
        if [ -f "$c/$name.y4m" ] && [ "$(sha "$c/$name.y4m")" = "$want" ]; then
            echo "copy  $name  [$class]  (from $c)"
            cp "$c/$name.y4m" "$out"
            return
        fi
    done
    echo "fetch $name  [$class]  (frames $first-$last of $(basename "$url"))"
    curl -fsSL "$url" | xz -dc | ffmpeg -loglevel error -y -f yuv4mpegpipe -i pipe:0 \
        -vf "select='between(n,$first,$last)'" -fps_mode vfr -frames:v "$n" \
        -f yuv4mpegpipe -pix_fmt yuv420p "$out" || true
    local got
    got="$(sha "$out")"
    if [ "$got" != "$want" ]; then
        echo "  hash mismatch for $name: got $got" >&2
        rm -f "$out"
        exit 1
    fi
}

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

# tier 1 generated: foreman_cif is already here, so this one always can be
for row in "${clips_gen[@]}"; do
    read -r c_name c_want c_class c_tier <<< "$row"
    [ "$c_tier" = 1 ] || continue
    generate "$c_name" "$c_want" "$c_class"
done

if [ "$full" = 1 ]; then
    echo "-- tier 2 (--full) --"
    for row in "${clips_full[@]}"; do
        read -r c_name c_url c_want c_class <<< "$row"
        fetch "$c_name" "$c_url" "$c_want" "$c_class"
    done
    for row in "${clips_excerpt[@]}"; do
        read -r c_name c_url c_first c_n c_want c_class <<< "$row"
        fetch_excerpt "$c_name" "$c_url" "$c_first" "$c_n" "$c_want" "$c_class"
    done
    # ...and the generated clips whose SOURCES are tier 2
    for row in "${clips_gen[@]}"; do
        read -r c_name c_want c_class c_tier <<< "$row"
        [ "$c_tier" = 2 ] || continue
        generate "$c_name" "$c_want" "$c_class"
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
