#!/bin/sh
# The encode side of the Annex A level table check (phase 8 item 7).
#
# tests/unit/level_test.c checks the DERIVATION exhaustively, in memory.
# This checks that the number it derives reaches general_level_idc in the
# VPS and the SPS and comes back out of a decoder: for each of the
# thirteen levels a synthetic clip is built at that level's own bound -
# the largest picture MaxLumaPs admits, or the frame rate that puts a
# smaller picture at MaxLumaSr - encoded at the DEFAULT settings, and the
# level read back with ffprobe. Then the three things a table check is
# for: one step over a bound moves to the next level, a --level below what
# the stream needs is refused, and a --high-tier the level cannot carry is
# refused.
#
# About two minutes; the 4096x2304 rows are 1.2 s of encode each at auto
# threads. ENC and FFMPEG/FFPROBE override the binaries.
set -u
ENC=${ENC:-build/n265}
FFMPEG=${FFMPEG:-ffmpeg}
FFPROBE=${FFPROBE:-ffprobe}
TMP=${TMPDIR:-/tmp}/n265-level-$$
mkdir -p "$TMP"
trap 'rm -rf "$TMP"' EXIT
pass=0; fail=0

# The y4m for one (size, rate). Cached by size: the frame rate lives in
# the header alone, so three levels can share one 162 MB payload.
src() {   # $1 WxH  $2 fps -> prints the path
    _sz=$1; _r=$2
    _base="$TMP/$_sz.raw.y4m"
    if [ ! -s "$_base" ]; then
        "$FFMPEG" -loglevel error -f lavfi -i "testsrc2=size=$_sz:rate=25" \
            -frames:v 12 -pix_fmt yuv420p "$_base" -y </dev/null || return 1
    fi
    if [ "$_r" = 25 ]; then
        echo "$_base"; return 0
    fi
    _out="$TMP/$_sz.$_r.y4m"
    if [ ! -s "$_out" ]; then
        # rewrite the header line only; the payload is copied byte for byte
        _hdr=$(head -1 "$_base" | wc -c | tr -d ' ')
        { head -1 "$_base" | sed "s/ F[0-9:]*/ F$_r:1/"
          tail -c "+$((_hdr + 1))" "$_base"; } > "$_out"
    fi
    echo "$_out"
}

level_of() {    # $1 bitstream -> general_level_idc as ffprobe reports it
    "$FFPROBE" -v error -select_streams v:0 -show_entries stream=level \
        -of csv=p=0 "$1"
}

# $1 label  $2 WxH  $3 fps  $4 expected idc  $5 extra encoder args
row() {
    y4m=$(src "$2" "$3") || { echo "FAIL $1 (source gen)"; fail=$((fail+1)); return; }
    # shellcheck disable=SC2086
    if ! "$ENC" "$y4m" --frames 12 -o "$TMP/o.hevc" $5 >/dev/null 2>&1; then
        echo "FAIL $1 (encode)"; fail=$((fail+1)); return
    fi
    got=$(level_of "$TMP/o.hevc")
    if [ "$got" = "$4" ]; then
        echo "PASS $1 $2 @${3}fps -> idc $got"
        pass=$((pass+1))
    else
        echo "FAIL $1 $2 @${3}fps -> idc $got, expected $4"
        fail=$((fail+1))
    fi
}

# $1 label  $2 WxH  $3 fps  $4 args that must be REFUSED
refuse() {
    y4m=$(src "$2" "$3") || { echo "FAIL $1 (source gen)"; fail=$((fail+1)); return; }
    # shellcheck disable=SC2086
    if "$ENC" "$y4m" --frames 2 -o "$TMP/o.hevc" $4 >/dev/null 2>&1; then
        echo "FAIL $1 ($4 was accepted)"
        fail=$((fail+1))
    else
        echo "PASS $1 ($4 refused)"
        pass=$((pass+1))
    fi
}

echo "== each level at its own bound =="
# Levels 1 to 3.1 are selected by the picture; 4 up by the frame rate,
# except 6 where the picture crosses MaxLumaPs again.
row "level 1"    176x144   15  30  ""
row "level 2"    352x288   25  60  ""
row "level 2.1"  352x288   60  63  ""
row "level 3"    720x576   25  90  ""
row "level 3.1"  1280x720  30  93  ""
row "level 4"    1280x720  60 120  ""
row "level 4.1"  1920x1080 50 123  ""
row "level 5"    1920x1080 120 150 ""
row "level 5.1"  1920x1080 240 153 ""
row "level 5.2"  1920x1080 500 156 ""
row "level 6"    4096x2304  25 180 ""
row "level 6.1"  4096x2304 120 183 ""
row "level 6.2"  4096x2304 240 186 ""

echo "== one step over a bound moves to the next level =="
# MaxLumaPs, at 3.1's bound of 983040 and at 5's of 8912896
row "3.1 at MaxLumaPs"   1280x768  25  93 ""
row "one CTB row over"   1280x776  25 120 ""
row "5 at MaxLumaPs"     4096x2176 25 150 ""
row "one CTB row over"   4096x2184 25 180 ""
# MaxLumaSr, at 3.1's bound of 33177600 (1280x720 at 36 fps exactly)
row "3.1 at MaxLumaSr"   1280x720  36  93 ""
row "one frame over"     1280x720  37 120 ""

echo "== the tier =="
row "high tier"          1920x1080 50 123 "--high-tier"
row "high tier on CIF"   352x288   25 120 "--high-tier"
row "ABR inside 4.1"     1920x1080 50 123 "--bitrate 20000"
row "ABR over 4.1"       1920x1080 50 153 "--bitrate 40000"

echo "== refusals =="
refuse "level under the frame rate" 1280x720  60 "--level 3.1"
refuse "level under the picture"    1920x1080 50 "--level 3"
refuse "high tier under level 4"    352x288   25 "--high-tier --level 2"
refuse "not a level"                352x288   25 "--level 3.2"
refuse "the raw idc is not a name"  352x288   25 "--level 123"

echo "SUMMARY level_check pass=$pass fail=$fail"
[ "$fail" = 0 ]
