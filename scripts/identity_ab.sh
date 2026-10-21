#!/usr/bin/env bash
# Copyright (c) 2026, the yah265 authors
# SPDX-License-Identifier: GPL-2.0-or-later
#
# identity_ab.sh - do two encoders (or two arms of one) write the same
# streams? Ported from yah264's scripts/identity_ab.sh.
#
#   scripts/identity_ab.sh OLD_BIN NEW_BIN
#   PROFILE_B=info,fingerprint scripts/identity_ab.sh old/n265 build/n265
#   ARM_B='N265_MERGE_PICK_RATE=0' scripts/identity_ab.sh build/n265 build/n265
#   ONLY='vbr|recon' MATRIX=t1 scripts/identity_ab.sh A B
#   scripts/identity_ab.sh --profiles            # list the escape profiles
#
# Encodes every cell of a CIF matrix with both sides and compares md5s:
# CQP, CRF, ABR, VBR at the default 1.5x / 2x shape and VBR on a 0.5 s
# buffer, presets fast / medium / slow, at one thread and at THREADED,
# each as the bitstream and (the cells named recon_*) as --dump-recon.
# It is the gate for a change that must not move a bit; arguing that a
# change is byte-identical by construction is not a gate.
#
# Statuses, one row per cell:
#   SAME    the md5s match
#   DIFF    they don't, and at one thread that is final. Threaded cells are
#           re-run (REPEAT times per side) before DIFF is called
#   VARIES  threaded only: a side gave more than one stream across its
#           re-runs and no run of OLD matched a run of NEW. Threaded output
#           need not repeat (owner rule, 2026-09-26), so this warns and
#           does not fail; read the cell at one thread
#   FAIL    an encode exited non-zero or wrote nothing
# A control cell runs the two sides at different CRFs and must read DIFF;
# if it reads SAME the comparison is broken and the script exits 2.
#
# Exit: 0 when every cell is SAME or VARIES, 1 on any DIFF or FAIL, 2 when
# the control fails or the arguments are wrong.
#
# Escape PROFILES name the era strings CLAUDE.md lists, so an identity cmp
# against an older binary is one word instead of a recalled env list.
# PROFILE_A applies to OLD, PROFILE_B to NEW; both take a comma list.
#
# Environment:
#   PROFILE_A, PROFILE_B   escape profiles (see --profiles)
#   ARM_A, ARM_B   NAME=VALUE env settings for the OLD / NEW side only
#   FLAGS_A, FLAGS_B   extra CLI flags for the OLD / NEW side only
#   MATRIX     full (default) or t1 (drop the threaded cells)
#   CCRF=1     add the capped-CRF cells (phase 14 item 47)
#   ONLY       extended regex on cell names; only matching cells run
#   FRAMES     frames per cell (default 24)
#   THREADED   thread count for the threaded cells (default 3)
#   BUDGET     total threads the script may keep busy (default 4)
#   REPEAT     re-runs per side before a threaded cell is called (default 3)
#   CORPUS     clip directory (default tests/corpus)
#   LIST=1     print the cells that would run, and stop
#   KEEP_SCRATCH=1 keeps the working directory
#
# Every N265_* in the caller's environment is cleared first, so both sides
# start from the shipped defaults; the profiles and ARM_A / ARM_B are the
# only way to set one. Untimed: it waits while a timed leg holds the
# timing lock, and refuses under 20 GB free.
set -u

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
SELF="$ROOT/scripts/identity_ab.sh"

hash_of() {
    if command -v md5 >/dev/null 2>&1; then md5 -q "$1"
    else md5sum "$1" | cut -d' ' -f1; fi
}

# profile NAME -> "flags|env" on stdout, or exit 1 for an unknown name.
# One line per era string in CLAUDE.md; the comment says what it undoes.
profile() {
    case "$1" in
        info)         echo "--no-info|";;          # item 9, identification SEI
        fingerprint)  echo "--no-fingerprint|";;   # item 10, merge_idx mark
        weightp)      echo "--no-weightp|";;       # item 2, weightp on
        stream)       echo "--no-info --no-fingerprint|";;  # both stream marks
        ship)         echo "|N265_SKIP_EXIT=1 N265_RDOQ=1";;  # phase 9 step 7
        item22)       echo "|N265_HD_QP=99";;
        item23)       echo "|N265_INTER_NXN=0 N265_INTRA_RSKIP=0";;
        item24)       echo "|N265_HD_QP=31 N265_CB64_SPLITGATE=0";;
        item25)       echo "|N265_HD_QP=30";;
        item25sched)  echo "|N265_TREE_K=6 N265_CLAIM_PRIO=0 N265_ROW_YIELD=0";;
        item26)       echo "|N265_UHD_QP=31 N265_CB64_SPLITGATE=0";;
        item28)       echo "|N265_TREE_RAMP=0 N265_AQ_POOL=0 N265_QP_TDITHER=0";;
        item29)       echo "|N265_ABR_FBUP=6 N265_ABR_IOFF_CT=0 N265_VBV_BRAKE=0 N265_ABR_FLATC=0 N265_VBV_SCPRED=1";;
        item29s2)     echo "|N265_VBV_BRAKE=0 N265_ABR_FLATC=0 N265_VBV_SCPRED=1";;
        item30)       echo "|N265_MERGE_PICK_RATE=0";;
        vbrspeed)     echo "|N265_VBR_LAG=0";;
        item33)       echo "|N265_VBR_LAG_BUFS=0";;
        item37)       echo "|N265_VBR_FBW=0 N265_VBR_OVW=0 N265_VBR_CTKEEP=0 N265_VBV_OWN=0 N265_VBR_FBUP=1.5";;
        item35)       echo "|N265_SC_JUMP=0";;  # item 35, scene-cut jump test
        item36)       echo "|N265_ME_PATTERN=0 N265_BADAPT_REFW=128";;  # item 36, hexagon walk + b-adapt width
        item38)       echo "|N265_VBR_FBFINE=6 N265_VBR_SPENDFILL=0";;  # item 38, capped VBR spends its surplus with buffer room
        item40)       echo "|N265_INTRA_FAST=0 N265_TT_SKIP_INTER=0";;  # item 40, coarse intra rank (1080p+) + UHD 16x16 split
        item41)       echo "|N265_ME_PATTERN=0 N265_BADAPT_FASTW=0";;   # item 41, 720p hexagon walk + fast-pan b-adapt width
        item43)       echo "|N265_KEY_RESET=0";;   # item 43, --keyint counted from the last key frame
        item44)       echo "|N265_AQ2=0 N265_AQ_BPULL=0";;   # item 44, adaptive AQ gain + HD B pull
        item46)       echo "--rc-lookahead 12|N265_CRF_DC_WIN=12";;   # item 46, medium window 20 at 720p and up (CIF never moved; the flag moves slow, so medium cells only)
        item47)       echo "|N265_CCRF_BRAKE=0 N265_CCRF_GUARD=1";;   # item 47, capped CRF's brake and guard (only the CCRF=1 cells move)
        item48)       echo "|N265_CB64_SKIPGATE=64 N265_VBR_FBTRK=0 N265_UHD_REF2=0";;   # item 48, UHD skip gate 32 + lite second reference, and capped VBR's deficit rail
        item49)       echo "|N265_VBR_CMEM=1000 N265_VBR_VLVL=0";;   # item 49, capped VBR: the whole-encode operating point and the clamp level
        asmoff)       echo "|N265_ASM_OFF=all";;
        *) return 1;;
    esac
}
PROFILE_NAMES="info fingerprint weightp stream ship item22 item23 item24 item25 item25sched item26 item28 item29 item29s2 item30 vbrspeed item33 item35 item36 item37 item38 item40 item41 item43 item44 item46 item47 item48 item49 asmoff"

# ---------------------------------------------------------------- one cell
# Called back through xargs: identity_ab.sh --cell WORK LINE
if [ "${1:-}" = "--cell" ]; then
    work="$2"; line="$3"
    IFS='|' read -r name threads clip frames args_a args_b <<EOF
$line
EOF
    [ -n "$args_b" ] || args_b="$args_a"
    d="$work/cells/$name"; mkdir -p "$d"

    # enc SIDE RUN -> prints the md5 of the stream (or of the recon), or FAIL
    enc() {
        local side="$1" run="$2" bin arm args extra out log recon=0 hashed
        if [ "$side" = a ]; then bin="$OLD"; arm="$ENV_A"; args="$args_a"; extra="$XFLAGS_A"
        else bin="$NEW"; arm="$ENV_B"; args="$args_b"; extra="$XFLAGS_B"; fi
        case " $args " in *" RECON "*) recon=1; args="${args/RECON/}";; esac
        out="$d/$side$run.hevc"; log="$d/$side$run.log"
        rm -f "$out" "$d/$side$run.yuv"
        # word lists on purpose, split into arrays so nothing else expands
        local -a armv argv extrav reconv
        read -r -a armv <<< "$arm"
        read -r -a argv <<< "$args"
        read -r -a extrav <<< "$extra"
        reconv=()
        [ "$recon" = 1 ] && reconv=(--dump-recon "$d/$side$run.yuv")
        env ${armv[@]+"${armv[@]}"} "$bin" "$clip" --frames "$frames" \
            --threads "$threads" ${argv[@]+"${argv[@]}"} \
            ${extrav[@]+"${extrav[@]}"} ${reconv[@]+"${reconv[@]}"} \
            -o "$out" >"$log" 2>&1 || { echo FAIL; return; }
        hashed="$out"; [ "$recon" = 1 ] && hashed="$d/$side$run.yuv"
        [ -s "$hashed" ] || { echo FAIL; return; }
        hash_of "$hashed"
    }

    a=$(enc a 1); b=$(enc b 1); status=SAME
    if [ "$a" = FAIL ] || [ "$b" = FAIL ]; then
        status=FAIL
    elif [ "$a" != "$b" ]; then
        status=DIFF
        if [ "$threads" != 1 ]; then
            as="$a"; bs="$b"; i=2
            while [ "$i" -le "$REPEAT" ]; do
                as="$as $(enc a "$i")"; bs="$bs $(enc b "$i")"; i=$((i + 1))
            done
            case " $as $bs " in *" FAIL "*) status=FAIL;; esac
            if [ "$status" != FAIL ]; then
                hit=0
                for x in $as; do for y in $bs; do
                    [ "$x" = "$y" ] && hit=1
                done; done
                na=$(printf '%s\n' $as | sort -u | wc -l | tr -d ' ')
                nb=$(printf '%s\n' $bs | sort -u | wc -l | tr -d ' ')
                if [ "$hit" = 1 ]; then status=SAME
                elif [ "$na" -gt 1 ] || [ "$nb" -gt 1 ]; then status=VARIES
                fi
                a="$a(x$na)"; b="$b(x$nb)"
            fi
        fi
    fi
    printf '%s|%s|%s|%s|%s\n' "$name" "$threads" "$status" "$a" "$b" \
        > "$d/result"
    [ "${KEEP_SCRATCH:-0}" = 1 ] || rm -f "$d"/*.hevc "$d"/*.yuv
    exit 0
fi

# ---------------------------------------------------------------- driver
if [ "${1:-}" = "--profiles" ]; then
    printf '%-12s %s\n' profile "flags | env"
    for p in $PROFILE_NAMES; do
        printf '%-12s %s\n' "$p" "$(profile "$p")"
    done
    exit 0
fi
[ $# -eq 2 ] || { sed -n '5,13p' "$0" | sed 's/^# \{0,1\}//'; exit 2; }
OLD="$1"; NEW="$2"
for b in "$OLD" "$NEW"; do
    [ -x "$b" ] || { echo "identity_ab: not an executable: $b" >&2; exit 2; }
done
OLD="$(cd "$(dirname "$OLD")" && pwd)/$(basename "$OLD")"
NEW="$(cd "$(dirname "$NEW")" && pwd)/$(basename "$NEW")"

MATRIX="${MATRIX:-full}"; ONLY="${ONLY:-}"
FRAMES="${FRAMES:-24}"; THREADED="${THREADED:-3}"; BUDGET="${BUDGET:-4}"
REPEAT="${REPEAT:-3}"; ARM_A="${ARM_A:-}"; ARM_B="${ARM_B:-}"
CORPUS="${CORPUS:-$ROOT/tests/corpus}"

# expand PROFILE_x into flags and env, then append FLAGS_x and ARM_x
expand() {   # expand PROFILES -> sets P_FLAGS P_ENV
    P_FLAGS=""; P_ENV=""
    local p r
    for p in $(printf '%s' "$1" | tr ',' ' '); do
        r=$(profile "$p") || {
            echo "identity_ab: unknown profile '$p' (have: $PROFILE_NAMES)" >&2
            exit 2; }
        P_FLAGS="$P_FLAGS ${r%%|*}"; P_ENV="$P_ENV ${r#*|}"
    done
}
expand "${PROFILE_A:-}"; XFLAGS_A="$P_FLAGS ${FLAGS_A:-}"; ENV_A="$P_ENV $ARM_A"
expand "${PROFILE_B:-}"; XFLAGS_B="$P_FLAGS ${FLAGS_B:-}"; ENV_B="$P_ENV $ARM_B"

# an env word that is not NAME=VALUE makes env(1) run it as the command,
# so every encode would fail the same way on both sides
for w in $ENV_A $ENV_B; do
    case "$w" in
        [A-Za-z_]*=*) ;;
        *) echo "identity_ab: env word '$w' is not NAME=VALUE" >&2; exit 2;;
    esac
done
for v in $(env | sed -n 's/^\(N265_[A-Za-z0-9_]*\)=.*/\1/p'); do unset "$v"; done
export OLD NEW ENV_A ENV_B XFLAGS_A XFLAGS_B REPEAT

python3 "$ROOT/scripts/boxlib.py" lock wait --what identity_ab || exit 2
. "$ROOT/scripts/scratch.sh"
yah265_scratch_df_guard 20 || exit 2
yah265_scratch_dir identityab WORK

c() { echo "$CORPUS/$1.y4m"; }
F="$FRAMES"; T="$THREADED"

# name|threads|clip|frames|args_old|args_new (args_new empty = same)
cells() {
    local fm st mt rates r
    fm=$(c foreman_cif); st=$(c stefan_cif); mt=$(c mobile_cif)
    # rate modes at medium; the two VBR shapes are the default 1.5x / 2x
    # (1.33 s of maxrate) and a 0.5 s buffer, which since item 33 codes at
    # lag 0 at every thread count
    rates="cqp:$fm:--qp 30
crf:$fm:--crf 28
abr:$fm:--bitrate 400
vbr:$st:--bitrate 600 --vbv-maxrate 900 --vbv-bufsize 1200
vbr_buf05:$st:--bitrate 600 --vbv-maxrate 900 --vbv-bufsize 450
fast:$mt:--preset fast --crf 28
slow:$mt:--preset slow --crf 28"
    # CCRF=1 adds capped CRF (item 47) on foreman at 1.2x / 1 s and at
    # 1.0x / 0.5 s of its uncapped rate at crf 28 (141 kbit/s); off by
    # default so the matrix stays the 28 cells older reads name
    if [ "${CCRF:-0}" = 1 ]; then
        rates="$rates
ccrf:$fm:--crf 28 --vbv-maxrate 170 --vbv-bufsize 170
ccrf_buf05:$fm:--crf 28 --vbv-maxrate 141 --vbv-bufsize 70"
    fi
    printf '%s\n' "$rates" | while IFS=: read -r n clip args; do
        case "$args" in *--preset*) ;; *) args="--preset medium $args";; esac
        echo "$n|1|$clip|$F|$args"
        echo "recon_$n|1|$clip|$F|$args RECON"
        if [ "$MATRIX" != t1 ]; then
            echo "${n}_t$T|$T|$clip|$F|$args"
            echo "recon_${n}_t$T|$T|$clip|$F|$args RECON"
        fi
    done
}

{
    cells
    # the control: NEW is asked for a different stream; it must read DIFF
    echo "control_must_differ|1|$(c foreman_cif)|8|--crf 28|--crf 34"
} | { if [ -n "$ONLY" ]; then grep -E "^[^|]*(${ONLY})[^|]*\||^control_must_differ\|"; else cat; fi; } \
  > "$WORK/cells.txt"

missing=$(cut -d'|' -f3 "$WORK/cells.txt" | sort -u | while read -r f; do
    [ -f "$f" ] || echo "$f"; done)
[ -z "$missing" ] || { echo "identity_ab: missing clips:" $missing >&2; exit 2; }

ncell=$(wc -l < "$WORK/cells.txt" | tr -d ' ')
if [ "${LIST:-0}" = 1 ]; then cat "$WORK/cells.txt"; exit 0; fi
echo "identity_ab: $ncell cells, matrix $MATRIX, $F frames, threaded=$THREADED, budget $BUDGET"
# unquoted on purpose: collapses the blanks the expansion left behind
ea=$(echo $ENV_A); fa=$(echo $XFLAGS_A); eb=$(echo $ENV_B); fb=$(echo $XFLAGS_B)
echo "  OLD $OLD  md5 $(hash_of "$OLD")${ea:+  env $ea}${fa:+  flags $fa}"
echo "  NEW $NEW  md5 $(hash_of "$NEW")${eb:+  env $eb}${fb:+  flags $fb}"

# one-thread cells BUDGET at a time, then threaded cells as many as fit
run_set() {   # run_set PARALLEL < lines
    tr '\n' '\0' | xargs -0 -n 1 -P "$1" "$SELF" --cell "$WORK"
}
t0=$(date +%s)
awk -F'|' '$2 == 1' "$WORK/cells.txt" | run_set "$BUDGET"
tp=$(( BUDGET / THREADED )); [ "$tp" -ge 1 ] || tp=1
awk -F'|' '$2 != 1' "$WORK/cells.txt" | run_set "$tp"

same=0; diff=0; vary=0; fail=0; ctrl=BAD
printf '\n%-22s %3s  %-6s  %-38s %s\n' cell thr status "old md5" "new md5"
while IFS='|' read -r name _; do
    r="$WORK/cells/$name/result"
    if [ ! -f "$r" ]; then
        printf '%-22s %3s  %-6s\n' "$name" "?" FAIL; fail=$((fail + 1)); continue
    fi
    IFS='|' read -r n t s a b < "$r"
    if [ "$n" = control_must_differ ]; then
        [ "$s" = DIFF ] && ctrl=OK
        printf '%-22s %3s  %-6s  (control: must be DIFF -> %s)\n' "$n" "$t" "$s" "$ctrl"
        continue
    fi
    printf '%-22s %3s  %-6s  %-38s %s\n' "$n" "$t" "$s" "$a" "$b"
    case "$s" in
        SAME) same=$((same + 1));; DIFF) diff=$((diff + 1));;
        VARIES) vary=$((vary + 1));; *) fail=$((fail + 1));;
    esac
done < "$WORK/cells.txt"

echo
if [ "$ctrl" != OK ]; then
    echo "identity_ab: CONTROL FAILED (the comparison cannot see a difference)"
    exit 2
fi
verdict=PASS; [ $((diff + fail)) -eq 0 ] || verdict=DIFF
echo "identity_ab: $verdict  same $same  diff $diff  varies $vary  fail $fail  ($(( $(date +%s) - t0 )) s)"
[ "$vary" -eq 0 ] || echo "  (VARIES = threaded output not repeatable here; informational)"
[ "$verdict" = PASS ]
