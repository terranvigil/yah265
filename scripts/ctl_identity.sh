#!/usr/bin/env bash
# Copyright (c) 2026, the yah265 authors
# SPDX-License-Identifier: GPL-2.0-or-later
#
# Live control's identity legs, at --threads 1, across the rate modes:
#   none    no control option: the stream and recon equal BASE's
#   empty   an empty --control file: equal to BASE too
#   null    events that change nothing (the open-time rate, qp 0..51, the
#           open crf, a key on frame 0): equal to BASE
#   replay  a stepped log's --control-out fed back through --control:
#           equal to the run that wrote it
#
#   BASE=/path/to/old/y265 scripts/ctl_identity.sh
#   CLIPS="foreman_cif mobile_cif" FRAMES=60 BASE=... scripts/ctl_identity.sh
set -u
ROOT=$(cd "$(dirname "$0")/.." && pwd)
NEW=${NEW:-$ROOT/build/y265}
BASE=${BASE:?BASE must name the binary to compare against}
CORPUS=${CORPUS:-$ROOT/tests/corpus}
CLIPS=${CLIPS:-"foreman_cif mobile_cif park_joy_720p"}
FRAMES=${FRAMES:-60}
T=$(mktemp -d)
trap 'rm -rf "$T"' EXIT
fail=0

enc() {     # bin out recon args...
    local bin=$1 out=$2 rec=$3
    shift 3
    "$bin" "$src" --frames "$FRAMES" --threads 1 -o "$out" \
        --dump-recon "$rec" "$@" 2>/dev/null
}

same() {    # label a b
    if cmp -s "$2" "$3"; then
        printf '  %-28s ok\n' "$1"
    else
        printf '  %-28s DIFFER\n' "$1"
        fail=1
    fi
}

for clip in $CLIPS; do
    src=$CORPUS/$clip.y4m
    case $clip in
        *_720p) br=2500 ;;
        *_1080p) br=5000 ;;
        *) br=500 ;;
    esac
    mr=$((br * 3 / 2))
    for mode in crf cqp abr cvbr ccrf; do
        case $mode in
            crf)  args=(--crf 28)
                  null=$'0 crf value=28\n0 qp min=0 max=51'
                  step=$'20 crf value=24\n30 qp min=30\n40 key' ;;
            cqp)  args=(--qp 30)
                  null=$'0 key\n0 zone last=10 offset=0'
                  step=$'20 key\n25 zone last=34 offset=-3' ;;
            abr)  args=(--bitrate "$br")
                  null=$'0 rate bitrate='"$br"$'\n0 qp min=0 max=51'
                  step=$'20 rate bitrate='$((br / 2))$'\n40 rate bitrate='"$br" ;;
            cvbr) args=(--bitrate "$br" --vbv-maxrate "$mr" --vbv-bufsize "$mr")
                  null="0 rate bitrate=$br maxrate=$mr bufsize=$mr"
                  step=$'20 rate bitrate='$((br / 2))' maxrate='$((mr / 2))' bufsize='$((mr / 2))$'\n40 rate bitrate='"$br"' maxrate='"$mr"' bufsize='"$mr" ;;
            ccrf) args=(--crf 28 --vbv-maxrate "$mr" --vbv-bufsize "$mr")
                  null=$'0 rate maxrate='"$mr"' bufsize='"$mr"$'\n0 crf value=28'
                  step=$'20 rate maxrate='$((mr / 2))$'\n40 rate maxrate='"$mr" ;;
        esac
        echo "$clip $mode"
        enc "$BASE" "$T/b.hevc" "$T/b.yuv" "${args[@]}"
        enc "$NEW" "$T/n.hevc" "$T/n.yuv" "${args[@]}"
        same "none, stream" "$T/b.hevc" "$T/n.hevc"
        same "none, recon" "$T/b.yuv" "$T/n.yuv"
        : > "$T/empty.log"
        enc "$NEW" "$T/e.hevc" "$T/e.yuv" "${args[@]}" --control "$T/empty.log"
        same "empty log" "$T/b.hevc" "$T/e.hevc"
        printf '%s\n' "$null" > "$T/null.log"
        enc "$NEW" "$T/z.hevc" "$T/z.yuv" "${args[@]}" --control "$T/null.log"
        same "null events" "$T/b.hevc" "$T/z.hevc"
        printf '%s\n' "$step" > "$T/step.log"
        enc "$NEW" "$T/s.hevc" "$T/s.yuv" "${args[@]}" --no-hrd \
            --control "$T/step.log" --control-out "$T/out.log"
        enc "$NEW" "$T/r.hevc" "$T/r.yuv" "${args[@]}" --no-hrd \
            --control "$T/out.log"
        same "replay" "$T/s.hevc" "$T/r.hevc"
        if cmp -s "$T/s.hevc" "$T/n.hevc"; then
            printf '  %-28s DIFFER expected, got the plain stream\n' "step moves the stream"
            fail=1
        fi
    done
done
# --control-out order: a RATE at 100 then a KEY at 120 apply backwards
# (the key at its push, the rate at a later anchor's decision), and a tie
# on frame 100 keeps hand-in order. Submitted out of order on a
# descriptor, the log comes back sorted and replays to the same bytes.
src=$CORPUS/foreman_cif.y4m
echo "control-out order"
printf '%s\n' '120 key' '100 rate bitrate=300' '100 key' '60 qp min=20' \
    > "$T/ord.in"
printf '%s\n' '#livectl 1' '60 qp min=20' '100 rate bitrate=300' '100 key' \
    '120 key' > "$T/ord.want"
"$NEW" "$src" --frames 140 --threads 1 --bitrate 500 -o "$T/o1.hevc" \
    --control-fd 3 --control-out "$T/ord.out" 3< "$T/ord.in" 2>/dev/null
same "sorted, ties in hand-in order" "$T/ord.want" "$T/ord.out"
"$NEW" "$src" --frames 140 --threads 1 --bitrate 500 -o "$T/o2.hevc" \
    --control "$T/ord.out" 2>/dev/null
same "replay" "$T/o1.hevc" "$T/o2.hevc"
# rule (f): a 10x step down in plain ABR forgives all but a second of the
# carried balance, so the rate is on the new target within about a
# second. The check is the mean over the two seconds after the step's
# first half second, within 10% of the target.
echo "abr 10x step down"
printf '60 rate bitrate=300\n' > "$T/sd.log"
"$NEW" "$src" --frames 180 --threads 1 --bitrate 3000 -o "$T/sd.hevc" \
    --control "$T/sd.log" --frame-stats "$T/sd.jsonl" 2>/dev/null
kbps=$(python3 -c '
import json, sys
by = {l["frame"]: l["bytes"] for l in map(json.loads, open(sys.argv[1]))}
print(round(sum(by[f] for f in range(75, 135)) * 8 * 30 / 60 / 1000))' \
    "$T/sd.jsonl")
if [ "$kbps" -ge 270 ] && [ "$kbps" -le 330 ]; then
    printf '  %-28s ok (%s kbit/s)\n' "on target within 1 s" "$kbps"
else
    printf '  %-28s FAIL (%s kbit/s, target 300)\n' "on target within 1 s" "$kbps"
    fail=1
fi
[ $fail = 0 ] && echo "ctl_identity: OK" || echo "ctl_identity: FAIL"
exit $fail
