#!/usr/bin/env bash
# Copyright (c) 2026, the yah265 authors
# SPDX-License-Identifier: GPL-2.0-or-later
#
# The CLI side of live control (contract section 9): a --control file is
# checked whole, room in the encoder's queue included, before any output
# exists; a --control-fd line that would make a file line fail later is
# skipped, and the file's lines all still land.
#
#   tests/cli_ctl_test.sh path/to/y265
set -u
Y265=${1:?usage: cli_ctl_test.sh path/to/y265}
T=$(mktemp -d)
trap 'rm -rf "$T"' EXIT
fail=0
bad() { echo "cli_ctl_test: FAIL $*" >&2; fail=1; }

# 24 grey 64x64 frames
{
    printf 'YUV4MPEG2 W64 H64 F30:1 Ip A1:1 C420\n'
    for _ in $(seq 24); do
        printf 'FRAME\n'
        head -c 6144 /dev/zero | LC_ALL=C tr '\0' '\200'
    done
} > "$T/in.y4m"
run() { "$Y265" "$T/in.y4m" --threads 1 --crf 28 "$@"; }

# a refused event stops the run before the output is cut, naming its line
printf '5 zone last=20 offset=-2\n10 key\n15 zone last=25 offset=1\n' \
    > "$T/conflict.log"
run -o "$T/a.hevc" --control "$T/conflict.log" \
    --control-out "$T/a.out" 2> "$T/a.err"
[ $? = 2 ] || bad "a refused file line did not stop the run"
grep -q 'conflict.log:3:' "$T/a.err" || bad "the refusal names no line"
[ ! -e "$T/a.hevc" ] && [ ! -e "$T/a.out" ] ||
    bad "the refused run wrote output"

# every other way a file can fail stops the run the same way, with no
# output of any kind: a malformed line, a real that isn't a plain
# decimal, an unknown header, frames out of order, and an event the
# encode's mode refuses (a bitrate under CRF, a QP floor under CQP)
stops() {   # name, file contents, expected line, encoder args...
    local name=$1 body=$2 want=$3
    shift 3
    printf '%s' "$body" > "$T/$name.log"
    rm -f "$T/$name".*.out
    "$Y265" "$T/in.y4m" --threads 1 "$@" -o "$T/$name.hevc.out" \
        --control "$T/$name.log" --control-out "$T/$name.ctl.out" \
        --frame-stats "$T/$name.fs.out" --dump-recon "$T/$name.yuv.out" \
        2> "$T/$name.err"
    [ $? = 2 ] || bad "$name: the run did not stop with status 2"
    grep -q "$name.log:$want:" "$T/$name.err" ||
        bad "$name: the message names no line $want: $(head -1 "$T/$name.err")"
    ! ls "$T/$name".*.out >/dev/null 2>&1 || bad "$name: the run wrote output"
}
stops malformed $'5 key\n8 bogus x=1\n' 2 --crf 28
stops exponent $'5 key\n9 crf value=2.5e1\n' 2 --crf 28
stops header $'#livectl 99\n5 key\n' 1 --crf 28
stops order $'9 key\n5 key\n' 2 --crf 28
stops emode $'5 key\n7 crf value=24\n9 rate bitrate=300\n' 3 --crf 28
stops cqp $'2 key\n4 qp min=20\n' 2 --qp 30

# more events in one lag span than the queue keeps room for: refused at
# load, naming the first line over; one fewer encodes
for _ in $(seq 2048); do echo '3 key'; done > "$T/full.log"
run -o "$T/b.hevc" --control "$T/full.log" 2> "$T/b.err" ||
    bad "2048 events on one frame were refused: $(tail -1 "$T/b.err")"
echo '900 key' >> "$T/full.log"
run -o "$T/c.hevc" --control "$T/full.log" 2> "$T/c.err"
[ $? = 2 ] || bad "2049 events within the span did not stop the run"
grep -q 'full.log:2049:' "$T/c.err" || bad "the span refusal names no line"
[ ! -e "$T/c.hevc" ] || bad "the span-refused run wrote output"

# --control with --control-fd. The file's first 1100 lines fill the feed
# window, so its zone on 10..20 is not fed yet when the fd lines arrive.
# The fd zone on 5..12 would make it fail mid-run: skipped with a
# warning. The fd zone on 21..22 and the fd key don't meet it: applied.
{ for _ in $(seq 1100); do echo '1 key'; done
  echo '10 zone last=20 offset=-2'; } > "$T/mixed.log"
printf '5 zone last=12 offset=1\n21 zone last=22 offset=1\n2 key\n' \
    > "$T/fd.log"
run -o "$T/d.hevc" --control "$T/mixed.log" --control-fd 3 \
    --control-out "$T/d.out" 3< "$T/fd.log" 2> "$T/d.err" ||
    bad "the mixed run failed: $(tail -1 "$T/d.err")"
grep -q 'fd line 1 skipped:.*mixed.log line 1101' "$T/d.err" ||
    bad "the conflicting fd line was not skipped naming the file line"
grep -q '^10 zone last=20 offset=-2$' "$T/d.out" ||
    bad "the file's zone did not land"
grep -q '^21 zone last=22 offset=1$' "$T/d.out" ||
    bad "the fd zone that fits did not land"
grep -q '^5 zone' "$T/d.out" && bad "the skipped fd zone landed"
[ "$(grep -c '^2 key$' "$T/d.out")" = 1 ] || bad "the fd key did not land"
[ "$(grep -c '^1 key$' "$T/d.out")" = 1100 ] ||
    bad "not every file key landed"

# rule (b) v1.2: a RATE step past 3x moves the operating point by two
# thirds of the law and refits it from coded groups; a smaller step and a
# step up under CBR don't
{
    printf 'YUV4MPEG2 W64 H64 F30:1 Ip A1:1 C420\n'
    for i in $(seq 90); do
        printf 'FRAME\n'
        head -c 4096 /dev/zero | LC_ALL=C tr '\0' "\\$(printf '%03o' $((i + 40)))"
        head -c 2048 /dev/zero | LC_ALL=C tr '\0' '\200'
    done
} > "$T/ramp.y4m"
step() {    # log args... -> the trace's step and refit lines
    printf '%s\n' "$1" > "$T/step.log"
    shift
    N265_RC_TRACE=1 "$Y265" "$T/ramp.y4m" --threads 1 --level 4.1 \
        -o "$T/s.hevc" --control "$T/step.log" "$@" 2>&1 |
        grep -E '^RC (step|refit)'
}
n=$(step '40 rate bitrate=20' --bitrate 200 | grep -c '^RC refit')
[ "$n" = 2 ] || bad "a 10x step down refit $n times, not 2"
step '40 rate bitrate=2000' --bitrate 200 | grep -q '^RC step' ||
    bad "a 10x step up did not move"
step '40 rate bitrate=100' --bitrate 200 | grep -q . && bad "a 2x step moved"
step '40 rate bitrate=2000 maxrate=3000 bufsize=4000' --bitrate 200 \
    --vbv-maxrate 300 --vbv-bufsize 400 --no-hrd | grep -q '^RC step' ||
    bad "a capped VBR step up did not move"
step '40 rate bitrate=2000 maxrate=2000 bufsize=2000' --bitrate 200 \
    --vbv-maxrate 200 --vbv-bufsize 200 --no-hrd | grep -q . &&
    bad "a CBR step up moved"
step '3 rate bitrate=20' --bitrate 200 | grep -q '^RC refit' &&
    bad "a step with no group before it refit"

[ $fail = 0 ] && echo "cli_ctl_test: OK"
exit $fail
