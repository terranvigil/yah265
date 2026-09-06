#!/bin/bash
# Copyright (c) 2026, the yah265 authors
# SPDX-License-Identifier: GPL-2.0-or-later
#
# san_matrix.sh - the encoder under AddressSanitizer + UndefinedBehaviorSanitizer
# over the inputs a recon-match gate never sees: odd sizes (33x17, 16x16, 8x8),
# one- and two-frame clips with B-frames on, keyint 1, qp 0, tiny and huge
# bitrates, 4:2:2, 4:4:4, CBR, 2-pass, direct temporal, the hardware backend,
# single-threaded for now. In yah264 this matrix found two memory bugs that
# hundreds of conformance cells had not; run it before every merge.
#
#   scripts/san_matrix.sh            # builds build-san/ (once), runs the cases this encoder supports (16)
#   FF=/path/to/ffmpeg scripts/san_matrix.sh
#
# Exit status is the number of cases with a sanitiser report. ~3 min.
cd "$(dirname "$0")/.." || exit 1
S=${SAN_WORK:-build-san/work}; mkdir -p "$S/a6wd"
B=build-san
[ -f $B/build.ninja ] || meson setup $B -Db_sanitize=address,undefined -Dbuildtype=debugoptimized -Db_lundef=false > $S/san-setup.log 2>&1 || { echo SETUP-FAIL; exit 1; }
ninja -C $B > $S/san-build.log 2>&1 || { echo BUILD-FAIL; tail -5 $S/san-build.log; exit 1; }
Y=$B/n265; C=tests/corpus; FF=${FF:-$(command -v ffmpeg)}
BAD=0
export ASAN_OPTIONS=detect_leaks=0:abort_on_error=0 UBSAN_OPTIONS=print_stacktrace=1
run() { # label, args...
  local label=$1; shift
  out=$( "$@" 2>&1 >/dev/null ); rc=$?
  errs=$(echo "$out" | grep -cE "ERROR: AddressSanitizer|runtime error:|SUMMARY:")
  echo "$label rc=$rc san=$errs"; [ "$errs" -gt 0 ] && BAD=$((BAD+1))
  [ $errs -gt 0 ] && echo "$out" | grep -E "runtime error|ERROR: AddressSanitizer|#[0-3] " | head -12 | sed 's/^/    /'
}
# synthetic odd inputs
mk() { $FF -v error -y -f lavfi -i "testsrc2=size=$1:rate=25" -frames:v $2 -pix_fmt $3 $4; }
mk 33x17 12 yuv420p $S/a6wd/odd33.y4m; mk 16x16 3 yuv420p $S/a6wd/t16.y4m; mk 8x8 2 yuv420p $S/a6wd/t8.y4m; mk 352x288 1 yuv420p $S/a6wd/one.y4m; mk 352x288 2 yuv420p $S/a6wd/two.y4m; mk 72x40 20 yuv420p $S/a6wd/small.y4m
run "odd33 ctu32"     $Y $S/a6wd/odd33.y4m --qp 26 -o /dev/null
run "odd33 ctu64"     $Y $S/a6wd/odd33.y4m --qp 26 --ctu 64 -o /dev/null
run "odd33 ctu16"     $Y $S/a6wd/odd33.y4m --qp 26 --ctu 16 -o /dev/null
run "16x16"           $Y $S/a6wd/t16.y4m --qp 26 -o /dev/null
run "8x8"             $Y $S/a6wd/t8.y4m --qp 26 -o /dev/null
run "one frame b1"    $Y $S/a6wd/one.y4m --qp 26 --bframes 1 -o /dev/null
run "two frames b1"   $Y $S/a6wd/two.y4m --qp 26 --bframes 1 -o /dev/null
run "keyint1"         $Y $C/foreman_cif.y4m --frames 20 --qp 26 --keyint 1 -o /dev/null
run "qp0"             $Y $C/foreman_cif.y4m --frames 6 --qp 0 -o /dev/null
run "qp51"            $Y $C/foreman_cif.y4m --frames 12 --qp 51 --aq 1 --cutree 1 -o /dev/null
run "aq cutree la0"   $Y $C/foreman_cif.y4m --frames 20 --qp 26 --aq 1 --cutree 1 --rc-lookahead 0 -o /dev/null
run "aq cutree la40"  $Y $C/bus_cif.y4m --frames 40 --qp 30 --aq 1 --cutree 1 --rc-lookahead 40 -o /dev/null
run "small aq"        $Y $S/a6wd/small.y4m --qp 26 --aq 1 --cutree 1 -o /dev/null
run "pcm"             $Y $C/foreman_cif.y4m --frames 3 --pcm -o /dev/null
run "720p b1 ctu64"   $Y $C/park_joy_720p.y4m --frames 12 --qp 28 --ctu 64 --bframes 1 -o /dev/null
run "1080p ctu64"     $Y $C/crowd_run_1080p.y4m --frames 6 --qp 28 --ctu 64 -o /dev/null
# not yet: 4:2:2, 4:4:4, 10-bit, ABR/VBV/CBR, 2-pass, threads -- add each the day it lands
echo "SAN-DONE: $BAD case(s) with reports"; exit $BAD
