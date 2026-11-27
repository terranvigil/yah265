#!/usr/bin/env bash
# Copyright (c) 2026, the yah265 authors
# SPDX-License-Identifier: GPL-2.0-or-later
#
# --target-vmaf-floor's whole-file correction, against a stand-in scorer
# that plays back a script of scores: a whole file under the floor is
# encoded again toward quality until it passes, every re-encode is the
# plain --crf stream, and at the bound the best file is kept and the run
# fails. The default (closest) mode doesn't correct. Needs ffmpeg and
# python3 (exit 77 without them).
#
#   tests/cli_tvmaf_floor_test.sh path/to/y265
set -u
Y265=${1:?usage: cli_tvmaf_floor_test.sh path/to/y265}
command -v ffmpeg >/dev/null && command -v python3 >/dev/null || exit 77
T=$(mktemp -d)
trap 'rm -rf "$T"' EXIT
fail=0
bad() { echo "cli_tvmaf_floor_test: FAIL $*" >&2; fail=1; }

# 72 frames of 1920x64 (wide enough that the scorer doesn't scale up), so
# --target-vmaf-sample 1 searches 12 windows of 3 frames
python3 - "$T/in.y4m" <<'EOF'
import sys
w, h = 1920, 64
with open(sys.argv[1], "wb") as f:
    f.write(b"YUV4MPEG2 W1920 H64 F30:1 Ip A1:1 C420\n")
    for n in range(72):
        f.write(b"FRAME\n")
        f.write(bytes(((x + 3 * n) * 7) & 255 for x in range(w)) * h)
        f.write(bytes([128]) * (w * h // 2))
EOF

# the stand-in vmaf: drains both inputs, counts the frames and returns the
# next score of $FAKE_SCORES, logging "frames score" to $FAKE_LOG
cat > "$T/vmaf" <<'EOF'
#!/usr/bin/env python3
import os, sys
a = sys.argv
dist, out = a[a.index("-d") + 1], a[a.index("-o") + 1]
def frames(f):
    data = f.read()
    hdr = data[:data.index(b"\n") + 1]
    tok = dict((t[0], t[1:]) for t in hdr.decode().split()[1:])
    size = int(tok["W"]) * int(tok["H"]) * 3
    return (len(data) - len(hdr)) // (6 + size)
nref = frames(sys.stdin.buffer)
with open(dist, "rb") as f:
    n = frames(f)
log = os.environ["FAKE_LOG"]
k = len(open(log).read().splitlines()) if os.path.exists(log) else 0
score = float(open(os.environ["FAKE_SCORES"]).read().split()[k])
with open(log, "a") as f:
    f.write("%d %d %.3f\n" % (nref, n, score))
with open(out, "w") as f:
    f.write('{"frames": [%s], "pooled_metrics": {"tvmaf": {"mean": %f}}}'
            % (", ".join('{"frameNum": %d}' % i for i in range(n)), score))
EOF
chmod +x "$T/vmaf"
mkdir -p "$T/model/vmaf_v1.0.16"
: > "$T/model/vmaf_v1.0.16/vmaf_v1.0.16_3d0h.json"
export N265_VMAF_BIN="$T/vmaf" N265_VMAF_MODEL_DIR="$T/model"
export FAKE_SCORES="$T/scores" FAKE_LOG="$T/log"

tv() {  # name, scores, extra args...
    local name=$1
    printf '%s\n' $2 > "$FAKE_SCORES"
    rm -f "$FAKE_LOG"
    shift 2
    "$Y265" "$T/in.y4m" --threads 1 --target-vmaf 90 --target-vmaf-sample 1 \
        --target-vmaf-json "$T/$name.json" -o "$T/$name.hevc" "$@" \
        2> "$T/$name.err"
}
jget() { python3 -c "import json,sys; print(json.load(open(sys.argv[1]))$2)" "$1"; }
same_as_crf() {  # name: the kept file is the plain --crf stream
    local crf
    crf=$(jget "$T/$1.json" "['crf']")
    "$Y265" "$T/in.y4m" --threads 1 --crf "$crf" -o "$T/$1.ref.hevc" \
        2> /dev/null
    cmp -s "$T/$1.hevc" "$T/$1.ref.hevc" ||
        bad "$1: the kept file is not the --crf $crf stream"
}

# the search passes on its windows, the whole file misses, two
# corrections bring it over
tv fix "90.2 89.0 89.8 90.3" --target-vmaf-floor
[ $? = 0 ] || bad "fix: exit code"
[ "$(jget "$T/fix.json" "['floor_met']")" = True ] || bad "fix: floor_met"
[ "$(jget "$T/fix.json" "['measured']")" = 90.3 ] || bad "fix: measured"
python3 - "$T/fix.json" <<'EOF' || bad "fix: the corrections"
import json, sys
j = json.load(open(sys.argv[1]))
c = [j["search_crf"]] + [x["crf"] for x in j["corrections"]]
assert len(c) == 3 and all(b < a for a, b in zip(c, c[1:])), c
assert j["crf"] == c[-1]
EOF
[ "$(cut -d' ' -f2 "$FAKE_LOG" | tr '\n' ' ')" = "36 72 72 72 " ] ||
    bad "fix: scored frames $(cut -d' ' -f2 "$FAKE_LOG" | tr '\n' ' ')"
[ "$(grep -c 'correction [0-9]: crf .* -> .*encode .* s, score .* s' \
    "$T/fix.err")" = 2 ] || bad "fix: each correction is not reported"
[ ! -e "$T/fix.hevc.tvfloor" ] || bad "fix: the set-aside file is left"
same_as_crf fix

# every correction misses: the bound stops it, the best (the first
# correction) is kept and the run fails
tv miss "90.2 85.0 86.0 85.5 85.8" --target-vmaf-floor
[ $? = 1 ] || bad "miss: exit code"
grep -q 'missed  the floor' "$T/miss.err" || bad "miss: no failure line"
[ "$(jget "$T/miss.json" "['floor_met']")" = False ] || bad "miss: floor_met"
[ "$(jget "$T/miss.json" "['measured']")" = 86.0 ] || bad "miss: best not kept"
[ "$(jget "$T/miss.json" "['crf']")" = \
  "$(jget "$T/miss.json" "['corrections'][0]['crf']")" ] ||
    bad "miss: the kept crf"
[ "$(wc -l < "$FAKE_LOG" | tr -d ' ')" = 5 ] || bad "miss: not 3 corrections"
[ ! -e "$T/miss.hevc.tvfloor" ] || bad "miss: the set-aside file is left"
same_as_crf miss

# closest mode reports the miss and doesn't correct
tv near "90.2 89.0"
[ $? = 0 ] || bad "near: exit code"
[ "$(wc -l < "$FAKE_LOG" | tr -d ' ')" = 2 ] || bad "near: it corrected"
python3 -c "import json,sys; j=json.load(open(sys.argv[1])); assert 'corrections' not in j" \
    "$T/near.json" || bad "near: floor fields in the report"

# the floor needs the measurement
tv noverify "90.2" --target-vmaf-floor --no-target-vmaf-verify
[ $? = 2 ] || bad "noverify: not refused"

[ $fail = 0 ] && echo "cli_tvmaf_floor_test: ok"
exit $fail
