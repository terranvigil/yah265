#!/usr/bin/env bash
# Copyright (c) 2026, the yah265 authors
# SPDX-License-Identifier: GPL-2.0-or-later
#
# ffmpeg_hdr_sidedata.sh - check that -c:v libyah265 carries a source's
# mastering display and content light level into the stream's SEIs.
#
#   scripts/ffmpeg_hdr_sidedata.sh [--base FFMPEG]
#
# Env: FFMPEG, FFPROBE (the fork's build), Y265 (build-rel/y265).
# --base names an ffmpeg from before the change: the SDR transcodes must be
# byte-identical between the two. The PQ source is synthetic: y265 writes
# its SEIs, and ffmpeg's decoder turns them into side data.
set -euo pipefail

root="$(cd "$(dirname "$0")/.." && pwd)"
fork="${FORK:-$HOME/src/build/ffboard/ffmpeg-yah265}"
ffmpeg="${FFMPEG:-$fork/ffmpeg}"
ffprobe="${FFPROBE:-$fork/ffprobe}"
y265="${Y265:-$root/build-rel/y265}"
base=""

while [ $# -gt 0 ]; do
  case "$1" in
    --base) base="$2"; shift 2 ;;
    -h|--help) sed -n '5,12p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
    *) echo "unknown option: $1" >&2; exit 2 ;;
  esac
done

work="$(mktemp -d "${TMPDIR:-/tmp}/hdrsd.XXXXXX")"
trap 'rm -rf "$work"' EXIT
fail=0
pass() { echo "PASS  $*"; }
bad() { echo "FAIL  $*"; fail=1; }

md_src='G(8500,39850)B(6550,2300)R(35400,14600)WP(15635,16450)L(40000000,50)'
cll_src='1655,121'
md_user='G(13250,34500)B(7500,3000)R(34000,16000)WP(15635,16450)L(10000000,1)'
pq=(--colorprim bt2020 --transfer smpte2084 --colormatrix bt2020nc)

# the first frame's mastering display and light level, one key=value a line
sd() {
  "$ffprobe" -v error -select_streams v:0 -read_intervals '%+#1' -show_frames \
    -show_entries frame_side_data -of flat "$1" |
    sed -n 's/.*side_data_list\.side_data\.[0-9]*\.\([a-z_]*\)=\(.*\)/\1=\2/p' |
    { grep -E '^(red|green|blue|white_point)_[xy]=|^(min|max)_luminance=|^max_(content|average)=' || true; } |
    tr -d '"' | sort
}

# the fields a G()B()R()WP()L() string and a cll pair stand for
expect() {
  local md="$1" cll="$2"
  echo "$md" | sed -E 's/G\(([0-9]+),([0-9]+)\)B\(([0-9]+),([0-9]+)\)R\(([0-9]+),([0-9]+)\)WP\(([0-9]+),([0-9]+)\)L\(([0-9]+),([0-9]+)\)/green_x=\1\/50000\
green_y=\2\/50000\
blue_x=\3\/50000\
blue_y=\4\/50000\
red_x=\5\/50000\
red_y=\6\/50000\
white_point_x=\7\/50000\
white_point_y=\8\/50000\
max_luminance=\9\/10000/'
  echo "$md" | sed -E 's/.*L\([0-9]+,([0-9]+)\)/min_luminance=\1\/10000/'
  echo "max_content=${cll%,*}"
  echo "max_average=${cll#*,}"
}

same_fields() {  # name, want-file, got-file
  if diff -u "$2" "$3" >"$work/diff"; then
    pass "$1 ($(wc -l <"$3" | tr -d ' ') fields)"
  else
    bad "$1"; cat "$work/diff"
  fi
}

lavfi() { "$ffmpeg" -v error -y -f lavfi -i "testsrc2=s=320x240:r=24:d=0.5" "$@"; }
enc() { "$ffmpeg" -v error -y "$@"; }

# 1. a 10-bit PQ source whose SEIs carry the mastering display and light level
lavfi -pix_fmt yuv420p10le -strict -1 "$work/src10.y4m"
"$y265" "$work/src10.y4m" -o "$work/pq.hevc" --crf 30 "${pq[@]}" \
  --master-display "$md_src" --max-cll "$cll_src" 2>/dev/null
expect "$md_src" "$cll_src" | sort >"$work/want_src"
sd "$work/pq.hevc" >"$work/got_src"
same_fields "source carries the test values" "$work/want_src" "$work/got_src"

# 2. through libyah265 with no params: the same values, field for field
enc -i "$work/pq.hevc" -c:v libyah265 -crf 30 "$work/out.hevc"
sd "$work/out.hevc" >"$work/got_out"
same_fields "PQ transcode keeps the source's SEIs" "$work/want_src" "$work/got_out"

# 3. -yah265-params wins over the side data, field by field
enc -i "$work/pq.hevc" -c:v libyah265 -crf 30 \
  -yah265-params "master-display=$md_user" "$work/out_md.hevc"
expect "$md_user" "$cll_src" | sort >"$work/want_md"
sd "$work/out_md.hevc" >"$work/got_md"
same_fields "params master-display overrides the source" "$work/want_md" "$work/got_md"

enc -i "$work/pq.hevc" -c:v libyah265 -crf 30 \
  -yah265-params "max-cll=400,100" "$work/out_cll.hevc"
expect "$md_src" "400,100" | sort >"$work/want_cll"
sd "$work/out_cll.hevc" >"$work/got_cll"
same_fields "params max-cll overrides the source" "$work/want_cll" "$work/got_cll"

# the metadata moves no sample
a=$("$ffmpeg" -v error -i "$work/out.hevc" -f md5 -)
b=$("$ffmpeg" -v error -i "$work/out_md.hevc" -f md5 -)
[ "$a" = "$b" ] && pass "override moves no sample" || bad "override moved samples"

# 4. a later frame with other side data is warned about once
"$y265" "$work/src10.y4m" -o "$work/pq2.hevc" --crf 30 "${pq[@]}" \
  --master-display "$md_src" --max-cll 2000,300 2>/dev/null
"$ffmpeg" -v warning -y -i "concat:$work/pq.hevc|$work/pq2.hevc" -c:v libyah265 \
  -crf 30 "$work/out_cat.hevc" 2>"$work/cat.log"
n=$(grep -c "differs from the stream's" "$work/cat.log" || true)
[ "$n" = 1 ] && pass "changed side data mid-stream warns once" \
  || { bad "mid-stream change warned $n times"; cat "$work/cat.log"; }
sd "$work/out_cat.hevc" >"$work/got_cat"
same_fields "mid-stream change keeps init's values" "$work/want_src" "$work/got_cat"

# 5. no SEIs off 10-bit PQ/HLG, even with the side data present
enc -i "$work/pq.hevc" -vf "format=yuv420p,setparams=color_trc=bt709" \
  -c:v libyah265 -crf 30 "$work/sdr_sd.hevc"
[ -z "$(sd "$work/sdr_sd.hevc")" ] && pass "8-bit BT.709 output writes no HDR SEI" \
  || bad "8-bit BT.709 output wrote an HDR SEI"
enc -i "$work/pq.hevc" -vf "format=yuv420p" -c:v libyah265 -crf 30 "$work/pq8.hevc" 2>/dev/null
[ -z "$(sd "$work/pq8.hevc")" ] && pass "8-bit PQ output writes no HDR SEI" \
  || bad "8-bit PQ output wrote an HDR SEI"

# 6. SDR transcodes are byte-identical to the wrapper before the change
if [ -n "$base" ]; then
  lavfi -pix_fmt yuv420p "$work/sdr.y4m"
  for case in plain sd; do
    if [ $case = plain ]; then in=("$work/sdr.y4m"); vf=(); else in=("$work/pq.hevc"); vf=(-vf "format=yuv420p,setparams=color_trc=bt709"); fi
    "$ffmpeg" -v error -y -i "${in[@]}" ${vf[@]+"${vf[@]}"} -c:v libyah265 -crf 30 "$work/new_$case.hevc"
    "$base" -v error -y -i "${in[@]}" ${vf[@]+"${vf[@]}"} -c:v libyah265 -crf 30 "$work/old_$case.hevc"
    cmp -s "$work/new_$case.hevc" "$work/old_$case.hevc" \
      && pass "SDR ($case) byte-identical to the base wrapper" \
      || bad "SDR ($case) differs from the base wrapper"
  done
fi

[ $fail = 0 ] && echo "all passed" || { echo "FAILED"; exit 1; }
