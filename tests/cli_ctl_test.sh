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

[ $fail = 0 ] && echo "cli_ctl_test: OK"
exit $fail
