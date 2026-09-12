#!/bin/sh
# Conformance gate: every encode must reconstruct bit-exact through BOTH
# independent decoders (ffmpeg hevc + dec265), and the encoder must be
# byte-identical across runs. Phase 0 note: everything is all-intra IDR so
# decode order == display order; when B-frames land, the display-order leg
# must compare through ffmpeg only (dec265 -o emits decode order).
#
# usage: scripts/conformance.sh [--fast] [--depth10] [--csp444|--csp422]
# env: N265=path/to/n265  N265_10=path/to/n265-10
#      DEC265=path/to/dec265  FFMPEG=path/to/ffmpeg
#
# --depth10 (phase 8 item 2) runs the MAIN10 list: the source is
# generated as C420p10 and both decoders are held to the same bit-exact
# recon match. It is a SEPARATE list and not an addition to the 8-bit
# one, so `make conformance` and `make conformance-fast` stay exactly the
# runs they were - the 8-bit invariant includes their pass count.
#
# --csp444 (phase 8 item 6a) runs the 4:4:4 list, at 8 bits on its own
# and at 10 with --depth10 as well. The chroma format is a RUNTIME SPS
# property - one library codes both - so the source's C tag is what
# selects it, exactly as the depth tag selects the library. Its own list
# again, for the same reason the Main10 one is separate: the 4:2:0 pass
# counts are part of the invariant and may not move.
#
# --csp422 (phase 8 item 6b) is the same arrangement for 4:2:2, which is
# Main 4:2:2 10 at BOTH depths (Table A.2 has no 8-bit 4:2:2 profile).
#
# Since item 5a $N265 and $N265_10 are normally the SAME binary, because
# build/n265 links both libraries and picks the encoder from the input's
# bit depth; the two variables stay separate so a sanitiser tree or an
# older binary can be pointed at one leg without the other.

set -u

N265=${N265:-build/n265}
N265_10=${N265_10:-build/n265}
FFMPEG=${FFMPEG:-ffmpeg}
DEC265=${DEC265:-}
FAST=0
DEPTH=8
CSP=420
for a in "$@"; do
    case "$a" in
    --fast)    FAST=1 ;;
    --depth10) DEPTH=10 ;;
    --csp444)  CSP=444 ;;
    --csp422)  CSP=422 ;;
    *) echo "conformance: unknown option $a" >&2; exit 2 ;;
    esac
done

# find dec265: env, PATH (Debian ships it as libde265-dec265), then the
# sibling libde265 build
if [ -z "$DEC265" ]; then
    for cand in dec265 libde265-dec265; do
        if command -v "$cand" >/dev/null 2>&1; then
            DEC265=$cand
            break
        fi
    done
    if [ -z "$DEC265" ] && [ -x ../libde265/build/dec265/dec265 ]; then
        DEC265=../libde265/build/dec265/dec265
    fi
    if [ -z "$DEC265" ]; then
        echo "conformance: dec265 not found (set DEC265=); FAILING: the gate requires two decoders" >&2
        exit 1
    fi
fi
command -v "$FFMPEG" >/dev/null 2>&1 || { echo "conformance: ffmpeg not found" >&2; exit 1; }

# The binary under test and the source format go together. The one CLI
# reads the depth off the y4m tag, so the source format IS the leg
# selector; SRCFMT is what decides which library runs.
# -strict -1: neither yuv420p10le nor any 4:2:2 or 4:4:4 pixel format is
# a registered y4m format, so ffmpeg refuses to write the tag without it.
# The tags it then writes - C420p10, C422, C422p10, C444, C444p10 - are
# what the reader accepts.
if [ "$DEPTH" = 10 ]; then
    ENC=$N265_10
    case "$CSP" in
    444) SRCFMT="-pix_fmt yuv444p10le -strict -1 -f yuv4mpegpipe" ;;
    422) SRCFMT="-pix_fmt yuv422p10le -strict -1 -f yuv4mpegpipe" ;;
    *)   SRCFMT="-pix_fmt yuv420p10le -strict -1 -f yuv4mpegpipe" ;;
    esac
else
    ENC=$N265
    case "$CSP" in
    444) SRCFMT="-pix_fmt yuv444p -strict -1 -f yuv4mpegpipe" ;;
    422) SRCFMT="-pix_fmt yuv422p -strict -1 -f yuv4mpegpipe" ;;
    *)   SRCFMT="-pix_fmt yuv420p" ;;
    esac
fi
[ -x "$ENC" ] || { echo "conformance: $ENC not built" >&2; exit 1; }

TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT

pass=0
fail=0

# env assignments prefixed onto the encoder for the next run_vector call;
# set by run_vector_env, cleared after. Deliberately unquoted where it is
# expanded so the assignments split into separate words.
VENV=""

# a full lavfi graph for the next run_vector, instead of the usual
# "<source>=size=WxH:rate=25". Set by run_vector_graph, cleared after.
VGRAPH=""
# Every vector's source is dithered so the encoder has residual to code.
# A vector for the SAO CONTENT GATE cannot be: the gate types a frame by
# how many 16x16 source blocks are flat to within one luma level, and
# alls=12 noise leaves none anywhere. VNOISE lets those vectors ask for
# the clean source instead.
VNOISE=""

# An existing y4m file as the source for the next run_vector, instead of a
# generated one. The 10-bit list uses it for the corpus clips - synthetic
# testsrc2 is the right source for syntax coverage, but only real content
# exercises the lookahead, the AQ field and the flat classifier on the
# statistics they were fitted against. Set by run_vector_file; cleared
# after. Empty on every 8-bit vector, which is why that list is
# byte-for-byte the run it was.
VFILE=""

run_vector() {
    # $1 name  $2 WxH  $3 frames  $4 lavfi source  $5 encoder args
    # ("--pcm" makes the reference the source itself; anything else makes
    # it the encoder's own reconstruction via --dump-recon)
    name=$1; size=$2; frames=$3; src=$4; eargs=$5
    y4m="$TMP/$name.y4m"
    # shellcheck disable=SC2086
    if [ -n "$VFILE" ]; then
        "$FFMPEG" -loglevel error -i "$VFILE" -frames:v "$frames" $SRCFMT \
            "$y4m" -y 2>/dev/null
    else
        "$FFMPEG" -loglevel error -f lavfi -i "${VGRAPH:-$src=size=$size:rate=25}" \
            -vf "${VNOISE:-noise=alls=12:allf=t+u}" -frames:v "$frames" $SRCFMT "$y4m" -y \
            2>/dev/null
    fi
    if [ ! -s "$y4m" ]; then
        echo "SUMMARY $name SKIP (source gen failed)"
        return
    fi

    # shellcheck disable=SC2086
    env $VENV "$ENC" "$y4m" -o "$TMP/$name.hevc" $eargs \
        --dump-recon "$TMP/$name.recon.yuv" 2>/dev/null || {
        echo "SUMMARY $name FAIL (encode)"; fail=$((fail+1)); return; }
    # shellcheck disable=SC2086
    env $VENV "$ENC" "$y4m" -o "$TMP/$name.2.hevc" $eargs 2>/dev/null
    cmp -s "$TMP/$name.hevc" "$TMP/$name.2.hevc" || {
        echo "SUMMARY $name FAIL (determinism: two runs differ)"
        fail=$((fail+1)); return; }

    # The reference every decoder is held to. Normally it is the
    # encoder's own reconstruction; for the two LOSSLESS modes - --pcm and
    # --lossless (phase 8 item 5b) - it is the SOURCE, which makes each
    # such vector a round-trip claim and not only an agreement one.
    case " $eargs " in
    *" --pcm "*|*" --lossless "*)
        "$FFMPEG" -loglevel error -i "$y4m" -f rawvideo "$TMP/$name.src.yuv" -y
        ;;
    *)
        cp "$TMP/$name.recon.yuv" "$TMP/$name.src.yuv"
        ;;
    esac
    # A --lossless run must ALSO have reconstructed the source itself, or
    # the two decoders agreeing with the source would be agreeing with
    # something the encoder never produced.
    case " $eargs " in
    *" --lossless "*)
        if ! cmp -s "$TMP/$name.src.yuv" "$TMP/$name.recon.yuv"; then
            echo "SUMMARY $name FAIL (encoder recon is not the source)"
            fail=$((fail+1)); return
        fi
        ;;
    esac
    "$FFMPEG" -loglevel error -i "$TMP/$name.hevc" -f rawvideo \
        "$TMP/$name.ff.yuv" -y 2>"$TMP/$name.fferr"
    # ffmpeg and dec265 disagree on bounded chroma seam samples for
    # CTB-16 SAO streams (reproduced on x265 output too; recorded in
    # docs/instruments.md). dec265 stays bit-exact below; ffmpeg gets a
    # small labeled chroma allowance only on ctu-16 vectors - and NOT on a
    # lossless one, where SAO is off at the sequence level and the seam
    # the allowance covers does not exist.
    ffallow=0
    case "$eargs" in *"--ctu 16"*) ffallow=1 ;; esac
    case " $eargs " in *" --lossless "*) ffallow=0 ;; esac
    if [ "$ffallow" = 1 ]; then
        if ! python3 scripts/yuvdiff.py "$TMP/$name.src.yuv" \
                "$TMP/$name.ff.yuv" --geometry "$size" \
                --max-chroma-pct 1.0 --max-delta 16; then
            echo "SUMMARY $name FAIL (ffmpeg recon mismatch)"
            fail=$((fail+1)); return
        fi
    else
        if ! cmp -s "$TMP/$name.src.yuv" "$TMP/$name.ff.yuv"; then
            echo "SUMMARY $name FAIL (ffmpeg recon mismatch)"
            sed -n 1,3p "$TMP/$name.fferr" >&2
            fail=$((fail+1)); return
        fi
    fi
    if ! "$DEC265" -q -o "$TMP/$name.de.yuv" "$TMP/$name.hevc" \
            >/dev/null 2>"$TMP/$name.deerr" || [ ! -s "$TMP/$name.de.yuv" ]; then
        echo "SUMMARY $name FAIL (dec265 run failed)"
        sed -n 1,3p "$TMP/$name.deerr" >&2
        fail=$((fail+1)); return
    fi
    if ! cmp -s "$TMP/$name.src.yuv" "$TMP/$name.de.yuv"; then
        echo "SUMMARY $name FAIL (dec265 recon mismatch)"
        fail=$((fail+1)); return
    fi
    echo "SUMMARY $name PASS"
    pass=$((pass+1))
}

# same vector, with N265_* assignments in front of the encoder. SAO is off
# by default since R7, so the sao() writer and the offset application are
# only reachable this way and would otherwise go untested.
# same vector, with a full lavfi graph as the source. The motion vectors
# a pan produces are the point: nothing testsrc2 does on its own moves
# far enough to reach past a reference's border.
run_vector_graph() {
    VGRAPH=$1; shift
    run_vector "$@"
    VGRAPH=""
}

run_vector_env() {
    VENV=$1; shift
    run_vector "$@"
    VENV=""
}

# same vector, with an existing y4m as the source. The conversion to the
# list's depth happens in run_vector's own ffmpeg call, so one 8-bit
# corpus clip serves both lists.
run_vector_file() {
    VFILE=$1; shift
    run_vector "$@"
    VFILE=""
}

# same vector with the dither turned off at the source
run_vector_clean() {
    VENV=$1; shift
    VNOISE="null"
    run_vector "$@"
    VNOISE=""; VENV=""
}

# ---------------------------------------------------------------------
# The 4:2:2 list (phase 8 item 6b), run at BOTH depths from one place.
# Unlike 4:4:4, the two depths are the SAME profile: Table A.2 has no
# 8-bit 4:2:2 row, so an 8-bit stream is Main 4:2:2 10 with max_8bit
# clear, and --csp422 --depth10 differs only in the sample width.
#
# What the cells cover, which is every clause the format moves:
#   - the transform tree, where a chroma unit is TWO stacked square
#     blocks with their own cbf, scan and residual (7.3.8.8's
#     `cbf_cb[x0][y0 + (1 << (log2TrafoSize-1))]` and 7.3.8.10's tIdx
#     loop), and where the 8x8 node keeps a stacked 4x4 PAIR at the
#     parent. Low QP drives the tree deep enough to reach that pair
#     everywhere; intra NxN is where it is signalled under one
#     intra_chroma_pred_mode.
#   - Table 8-3's chroma intra mode remap (8.4.3). It is an identity
#     below mode 6 and a wrong table decodes anything whose chroma modes
#     stay low, so the cells need real directional content and not only
#     flat synthetic frames - hence the corpus clips and testsrc2's
#     edges.
#   - the lower chroma block predicted from the UPPER one's
#     reconstruction (8.4.4.1 decodes the two in order), which only
#     intra cells reach.
#   - the chroma QP mapping, Min(qPi, 51) instead of Table 8-10 (8.6.1),
#     which separates from 4:2:0's only above luma QP 29 - hence the QP
#     ladder rather than one mid-range point.
#   - 1/8-pel horizontal and 1/4-pel vertical chroma MC on a half-width,
#     full-height plane (8.5.3.3.3.3), which every inter cell exercises
#     and the pyramid cells exercise bi.
#   - chroma deblocking on the 8-chroma-sample grid in BOTH directions,
#     which here is 16 luma samples horizontally and EIGHT vertically.
#   - SAO on half-width, full-height planes.
# Both decoders are held bit-exact against the encoder's own
# --dump-recon, with no allowance anywhere - the ctu-16 cell included.
if [ "$CSP" = 422 ]; then
    P=c422
    [ "$DEPTH" = 10 ] && P=c422d10
    # The QP ladder. QpC is Min(qPi, 51) here, so the mapping differs
    # from 4:2:0's only above luma QP 29 and the top of the ladder is
    # where a wrong table shows; QP 0 is where the tree goes deepest and
    # the stacked 4x4 chroma pair is reached everywhere.
    run_vector ${P}_qp0   176x144  3 testsrc2 "--qp 0  --keyint 1"
    run_vector ${P}_qp22  176x144  5 testsrc2 "--qp 22 --keyint 1"
    run_vector ${P}_qp51  176x144  3 testsrc2 "--qp 51 --keyint 1"
    if [ "$DEPTH" = 10 ]; then
        run_vector ${P}_qpm12 176x144 3 testsrc2 "--qp -12 --keyint 1"
    fi
    # intra only, CTB-aligned and with a conformance window - whose
    # offsets are in CHROMA units, and at 4:2:2 those are half a luma
    # sample across and a whole one down
    run_vector ${P}_intra  176x144  5 testsrc2 "--keyint 1"
    run_vector ${P}_crop   210x122  5 testsrc2 "--keyint 1"
    # IPB with the pyramid at one thread and at twelve: the split chroma
    # MC fractions, uni and bi, and run_vector's own two-run cmp carries
    # determinism with each.
    run_vector ${P}_ipb_t1  352x288 24 testsrc2 "--keyint 12 --bframes 7 --aq 1 --cutree 1 --threads 1"
    run_vector ${P}_ipb_t12 352x288 24 testsrc2 "--keyint 12 --bframes 7 --aq 1 --cutree 1 --threads 12"
    # the widest RPS and the deepest pyramid the encoder builds
    run_vector ${P}_deep   352x288 34 testsrc2 "--keyint 20 --bframes 15 --rc-lookahead 24 --ref 3 --b-adapt 0 --crf 26 --aq 1 --cutree 1 --threads 1"
    # CRF, and AQ + cu-tree: the fractional base QP writes cu_qp_delta at
    # quantisation groups CQP never reaches, and every one of those QPs
    # goes through the chroma mapping again
    run_vector ${P}_crf    352x288 24 testsrc2 "--keyint 12 --bframes 5 --crf 26 --threads 1"
    run_vector ${P}_aq     352x288 18 testsrc2 "--keyint 9 --aq 1 --cutree 1 --crf 28"
    # non-zero deblock offsets, on a chroma grid whose HORIZONTAL edges
    # this format moves from 16 luma samples to 8
    N265_DBF_BETA=-3 N265_DBF_TC=-3 \
    run_vector ${P}_dbf_neg 176x144 16 testsrc2 "--keyint 8 --aq 1"
    # SAO on half-width, full-height planes, band and edge apart
    run_vector_env "N265_SAO_TYPES=3" \
                   ${P}_sao_full 176x144 12 testsrc2 "--keyint 6"
    run_vector_env "N265_SAO_TYPES=1" \
                   ${P}_sao_band 176x144 12 testsrc2 "--keyint 6 --qp 8"
    run_vector_env "N265_SAO_TYPES=2" \
                   ${P}_sao_edge 176x144 12 testsrc2 "--keyint 6 --qp 8"
    # PCM and --lossless: the reference is the SOURCE for both, so each
    # is a round trip. PCM writes chroma at the half-width plane size
    # here, and a bypass block codes a chroma residual of twice 4:2:0's
    # samples in two stacked pieces.
    run_vector ${P}_pcm      176x144  3 testsrc2 "--pcm"
    run_vector ${P}_lossless 176x144 12 testsrc2 "--keyint 6 --bframes 7 --lossless --threads 4"
    # the two other transform-tree geometries: min CB == max CB == 16,
    # and CTB 64, whose TU32 leaves make the chroma unit two 16x16
    # blocks - the largest chroma transform block this format has
    run_vector ${P}_ctu16  352x288 12 testsrc2 "--keyint 6 --ctu 16 --ref 3 --bframes 7 --aq 1 --cutree 1 --threads 12"
    run_vector ${P}_ctu64  210x122  3 testsrc2 "--ctu 64"
    # ...and one cell whose QP is high enough that the quadtree stays at
    # depth 0 and the RQT keeps TU32 leaves, so the 16x16 chroma pair is
    # a measurable share of the TU population rather than a rarity
    run_vector ${P}_tu32   352x288  3 testsrc2 "--qp 42 --keyint 1 --ctu 64"
    # The preset ladder. `slow` turns the rectangular and asymmetric
    # inter partitions back on, and rd_part is the one place the chroma
    # prediction is built PU BY PU - at 4:2:2 a PU's chroma is half as
    # wide and as tall as its luma, a shape no other format presents;
    # `fast` clears amp_enabled_flag in the SPS.
    run_vector ${P}_slow_t4  352x288 20 testsrc2 "--keyint 10 --preset slow --qp 26 --threads 4"
    run_vector ${P}_fast_t12 352x288 20 testsrc2 "--keyint 10 --preset fast --aq 1 --cutree 1 --threads 12"
    # REAL CONTENT. There is no native 4:2:2 clip in tests/corpus - all
    # fifteen are 8-bit 4:2:0 - so run_vector's own ffmpeg call
    # interpolates the chroma VERTICALLY on the way in (swscale's default
    # for yuv420p -> yuv422p). testsrc2 covers the syntax; only a natural
    # clip drives the lookahead, the AQ variance field and the motion
    # search on the statistics they were fitted against, and only real
    # edges drive the intra mode histogram far enough up Table 8-3 to
    # tell a wrong entry from a right one.
    run_vector_file tests/corpus/foreman_cif.y4m \
                   ${P}_foreman 352x288 24 testsrc2 "--keyint 12 --bframes 7 --crf 28 --aq 1 --cutree 1 --threads 4"
    if [ "$FAST" -eq 0 ]; then
        run_vector_file tests/corpus/mobile_cif.y4m \
                   ${P}_mobile 352x288 16 testsrc2 "--keyint 8 --bframes 3 --qp 22 --aq 1 --threads 1"
        run_vector ${P}_720_t12 1280x720 12 testsrc2 "--keyint 6 --bframes 5 --aq 1 --cutree 1 --threads 12"
    fi
    echo "conformance: $pass pass, $fail fail (4:2:2, $DEPTH-bit)"
    [ "$fail" -eq 0 ]
    exit
fi

# ---------------------------------------------------------------------
# The 4:4:4 list (phase 8 item 6a), run at BOTH depths from one place:
# --csp444 alone is Main 4:4:4, --csp444 --depth10 is Main 4:4:4 10. The
# chroma format is a runtime SPS property, so the same binary and the
# same library code both; what changes is the source's C tag.
#
# What the cells cover, which is every clause the format moves:
#   - the transform tree, where a chroma TB is the luma TB at every size
#     and the 8x8 node's four-into-one chroma merge does not exist
#     (7.3.8.8 / 7.3.8.10). Low QP is what drives the tree deep enough to
#     reach the 4x4 chroma leaf and intra NxN's four chroma blocks.
#   - the chroma QP mapping, Min(qPi, 51) instead of Table 8-10 (8.6.1),
#     which only SEPARATES from 4:2:0's above luma QP 29 - hence the QP
#     ladder rather than one mid-range point.
#   - intra chroma with the luma reference filtering and the luma scan
#     derivation (8.4.4.2.3, 7.4.9.11), and 32x32 chroma blocks, which
#     only exist at this format and only above CTB 32.
#   - quarter-pel chroma MC on full-size planes (8.5.3.3.3.3), which
#     every inter cell exercises and the pyramid cells exercise bi.
#   - chroma deblocking on the 8x8 CHROMA grid with four-line segments.
#   - SAO on full-size chroma planes.
# Both decoders are held bit-exact against the encoder's own
# --dump-recon, with no allowance anywhere.
if [ "$CSP" = 444 ]; then
    P=c444
    [ "$DEPTH" = 10 ] && P=c444d10
    # The QP ladder. QpC is Min(qPi, 51) at this format, so the mapping
    # differs from 4:2:0's only above luma QP 29 and the top of the
    # ladder is where a wrong table shows; QP 0 is where the tree goes
    # deepest and the 4x4 chroma leaf is reached everywhere.
    run_vector ${P}_qp0   176x144  3 testsrc2 "--qp 0  --keyint 1"
    run_vector ${P}_qp22  176x144  5 testsrc2 "--qp 22 --keyint 1"
    run_vector ${P}_qp51  176x144  3 testsrc2 "--qp 51 --keyint 1"
    if [ "$DEPTH" = 10 ]; then
        run_vector ${P}_qpm12 176x144 3 testsrc2 "--qp -12 --keyint 1"
    fi
    # intra only, CTB-aligned and with a conformance window - whose
    # offsets are in CHROMA units, which at 4:4:4 are luma samples
    run_vector ${P}_intra  176x144  5 testsrc2 "--keyint 1"
    run_vector ${P}_crop   210x122  5 testsrc2 "--keyint 1"
    # IPB with the pyramid at one thread and at twelve: quarter-pel
    # chroma MC on full-size planes, uni and bi, and run_vector's own
    # two-run cmp carries determinism with each.
    run_vector ${P}_ipb_t1  352x288 24 testsrc2 "--keyint 12 --bframes 7 --aq 1 --cutree 1 --threads 1"
    run_vector ${P}_ipb_t12 352x288 24 testsrc2 "--keyint 12 --bframes 7 --aq 1 --cutree 1 --threads 12"
    # the widest RPS and the deepest pyramid the encoder builds
    run_vector ${P}_deep   352x288 34 testsrc2 "--keyint 20 --bframes 15 --rc-lookahead 24 --ref 3 --b-adapt 0 --crf 26 --aq 1 --cutree 1 --threads 1"
    # CRF, and AQ + cu-tree: the fractional base QP writes cu_qp_delta at
    # quantisation groups CQP never reaches, and every one of those QPs
    # goes through the chroma mapping again
    run_vector ${P}_crf    352x288 24 testsrc2 "--keyint 12 --bframes 5 --crf 26 --threads 1"
    run_vector ${P}_aq     352x288 18 testsrc2 "--keyint 9 --aq 1 --cutree 1 --crf 28"
    # non-zero deblock offsets, on the chroma grid this format moves from
    # 16 luma samples to 8
    N265_DBF_BETA=-3 N265_DBF_TC=-3 \
    run_vector ${P}_dbf_neg 176x144 16 testsrc2 "--keyint 8 --aq 1"
    # SAO on full-size chroma planes, band and edge apart
    run_vector_env "N265_SAO_TYPES=3" \
                   ${P}_sao_full 176x144 12 testsrc2 "--keyint 6"
    run_vector_env "N265_SAO_TYPES=1" \
                   ${P}_sao_band 176x144 12 testsrc2 "--keyint 6 --qp 8"
    run_vector_env "N265_SAO_TYPES=2" \
                   ${P}_sao_edge 176x144 12 testsrc2 "--keyint 6 --qp 8"
    # PCM and --lossless: the reference is the SOURCE for both, so each
    # is a round trip. PCM writes chroma at the full plane size here, and
    # a bypass block codes a chroma residual three times the samples.
    run_vector ${P}_pcm      176x144  3 testsrc2 "--pcm"
    run_vector ${P}_lossless 176x144 12 testsrc2 "--keyint 6 --bframes 7 --lossless --threads 4"
    # the two other transform-tree geometries: min CB == max CB == 16,
    # and CTB 64, which is the only place a 32x32 CHROMA transform block
    # exists at all
    run_vector ${P}_ctu16  352x288 12 testsrc2 "--keyint 6 --ctu 16 --ref 3 --bframes 7 --aq 1 --cutree 1 --threads 12"
    run_vector ${P}_ctu64  210x122  3 testsrc2 "--ctu 64"
    # ...and one cell whose QP is high enough that the quadtree stays at
    # depth 0 and the RQT keeps TU32 leaves: a 32x32 CHROMA transform
    # block, which exists at no other chroma format, and with it the
    # 32x32 reference filtering that must NOT take luma's strong
    # smoothing (biIntFlag carries cIdx == 0). Measured at 10-12% of the
    # TU population here, against 0% at low QP.
    run_vector ${P}_tu32   352x288  3 testsrc2 "--qp 42 --keyint 1 --ctu 64"
    # The preset ladder. `slow` is what turns the rectangular and
    # asymmetric inter partitions back on, and rd_part is the one place
    # the chroma prediction is built PU BY PU rather than whole-CU - a
    # different chroma geometry from every cell above it; `fast` clears
    # amp_enabled_flag in the SPS and codes without the rect part modes.
    run_vector ${P}_slow_t4  352x288 20 testsrc2 "--keyint 10 --preset slow --qp 26 --threads 4"
    run_vector ${P}_fast_t12 352x288 20 testsrc2 "--keyint 10 --preset fast --aq 1 --cutree 1 --threads 12"
    # REAL CONTENT. There is no native 4:4:4 clip in tests/corpus - all
    # fifteen are 8-bit 4:2:0 - so run_vector's own ffmpeg call upsamples
    # the chroma on the way in (swscale's default for yuv420p ->
    # yuv444p). testsrc2 covers the syntax; only a natural clip drives
    # the lookahead, the AQ variance field and the motion search on the
    # statistics they were fitted against.
    run_vector_file tests/corpus/foreman_cif.y4m \
                   ${P}_foreman 352x288 24 testsrc2 "--keyint 12 --bframes 7 --crf 28 --aq 1 --cutree 1 --threads 4"
    if [ "$FAST" -eq 0 ]; then
        run_vector_file tests/corpus/mobile_cif.y4m \
                   ${P}_mobile 352x288 16 testsrc2 "--keyint 8 --bframes 3 --qp 22 --aq 1 --threads 1"
        run_vector ${P}_720_t12 1280x720 12 testsrc2 "--keyint 6 --bframes 5 --aq 1 --cutree 1 --threads 12"
    fi
    echo "conformance: $pass pass, $fail fail (4:4:4, $DEPTH-bit)"
    [ "$fail" -eq 0 ]
    exit
fi

# ---------------------------------------------------------------------
# The Main10 list (phase 8 item 2). Separate from the 8-bit list on
# purpose: `make conformance` and `make conformance-fast` have to stay
# the runs they were, pass count included, because the 8-bit invariant
# is read off them. What this list covers is the inheritance list of
# item 1 - the QP range and every table indexed by it, the I/O, the
# headers, the flat classifier - each put somewhere a decoder can see it.
#
# Both decoders are held BIT-EXACT against the encoder's own
# --dump-recon, as at 8 bits. The one 8-bit vector with a labeled
# allowance (--ctu 16, the ffmpeg/dec265 chroma SAO seam) has no 10-bit
# sibling here: yuvdiff.py's allowance is byte-oriented and would have to
# learn 16-bit samples first, and every other cell is exact anyway.
if [ "$DEPTH" = 10 ]; then
    # The QP LADDER, which is what the whole item is about: -12 is the
    # bottom of QpY's range at Main10 (-QpBdOffsetY, 7.4.3.3) and exists
    # only at this depth, 0 is where the 8-bit range starts, 22 is the
    # middle and 51 the top. A table left indexed by the bare QP instead
    # of Qp' desynchronises the dequantiser from the decoder's, and the
    # two ends are where it shows first.
    run_vector m10_qpm12   176x144  3  testsrc2  "--qp -12"
    run_vector m10_qp0     176x144  3  testsrc2  "--qp 0"
    run_vector m10_qp22    176x144  5  testsrc2  "--qp 22"
    run_vector m10_qp51    176x144  3  testsrc2  "--qp 51"
    # intra only, at two geometries: CTB-aligned and a conformance window
    run_vector m10_intra   176x144  5  testsrc2  "--keyint 1"
    run_vector m10_crop    210x122  5  testsrc2  "--keyint 1"
    # IPB with the pyramid, one thread and twelve. The threaded cell is
    # the row wavefront at its widest, and run_vector's own two-run cmp
    # carries the same-thread-count determinism leg with it.
    run_vector m10_ipb_t1  352x288 24  testsrc2  "--keyint 12 --bframes 7 --aq 1 --cutree 1 --threads 1"
    run_vector m10_ipb_t12 352x288 24  testsrc2  "--keyint 12 --bframes 7 --aq 1 --cutree 1 --threads 12"
    # the deep window at three references: the widest RPS and the
    # deepest pyramid this encoder builds
    run_vector m10_deep    352x288 34  testsrc2  "--keyint 20 --bframes 15 --rc-lookahead 24 --ref 3 --b-adapt 0 --crf 26 --aq 1 --cutree 1 --threads 1"
    # CRF and ABR: the fractional base QP puts cu_qp_delta at quantisation
    # groups the CQP path never reaches, and cu_qp_delta is one of the
    # three fields that carry QpBdOffset
    run_vector m10_crf     352x288 24  testsrc2  "--keyint 12 --bframes 5 --crf 26 --threads 1"
    run_vector m10_abr     352x288 24  testsrc2  "--keyint 12 --bitrate 400 --aq 1 --cutree 1 --threads 4"
    # AQ + cu-tree on their own, where the AQ variance floor and the
    # log2-variance anchor both live
    run_vector m10_aq      352x288 18  testsrc2  "--keyint 9 --aq 1 --cutree 1 --crf 28"
    # deblocking offsets: both decoders have to shift the Table 8-12
    # index by what we did AND scale beta/tC by 1 << (BitDepth - 8)
    N265_DBF_BETA=-3 N265_DBF_TC=-3 \
    run_vector m10_dbf_neg 176x144 16  testsrc2  "--keyint 8 --aq 1"
    # SAO, explicitly: sao_offset_abs is cMax 31 at Main10 and cMax 7 at
    # 8 bits, and a truncated-unary string written at the wrong cMax
    # desynchronises the parse from that CTB on. Band and edge separately,
    # because only one of them codes a sign.
    run_vector_env "N265_SAO_TYPES=3" \
                   m10_sao_full 176x144 12 testsrc2 "--keyint 6"
    run_vector_env "N265_SAO_TYPES=1" \
                   m10_sao_band 176x144 12 testsrc2 "--keyint 6 --qp 8"
    run_vector_env "N265_SAO_TYPES=2" \
                   m10_sao_edge 176x144 12 testsrc2 "--keyint 6 --qp 8"
    # the content gate at 10 bits: the flat classifier reads the FULL-RES
    # source, so it is the one lookahead term the 8-bit lowres proxy does
    # not shield, and a mis-scaled bound would put the SPS flag and the
    # two slice_sao_* flags in states the 8-bit encoder never picks.
    run_vector_clean "N265_FLAT_GATE=25" \
                   m10_flat_mix 176x144 12 testsrc2 "--keyint 6 --qp 34 --aq 1 --cutree 1"
    # a real hard cut inside min-keyint: the scenecut path, which places a
    # non-IDR intra picture and its own short-term RPS
    VENV="N265_MIN_KEYINT=0"
    run_vector_graph "testsrc2=size=176x144:rate=25:duration=0.4[a];smptebars=size=176x144:rate=25:duration=0.4[b];[a][b]concat=n=2:v=1:a=0" \
                   m10_cut 176x144 20 testsrc2 "--keyint 250 --bframes 7 --ref 2 --aq 1 --cutree 1 --threads 1"
    VENV=""
    # PCM: pcm_sample_bit_depth_luma/chroma_minus1 at 10, samples written
    # ten bits wide, and the reference is the SOURCE - lossless end to
    # end, which is the leg that says the I/O and the header agree.
    run_vector m10_pcm     176x144  3  testsrc2  "--pcm"
    run_vector m10_pcm_crop 100x60  3  testsrc2  "--pcm"
    # --lossless at Main10 (phase 8 item 5b). The depth is the point: a
    # bypass residual runs -1023..1023 here against -255..255 at 8 bits,
    # so coeff_abs_level_remaining reaches four more binary orders and its
    # Golomb-Rice escape is exercised where the 8-bit list cannot reach
    # it. The reference is the SOURCE, as for --pcm above, and the
    # encoder's own recon is checked against it first.
    run_vector m10_lossless_ipb_t1  352x288 24 testsrc2 "--keyint 12 --bframes 7 --lossless --threads 1"
    run_vector m10_lossless_ipb_t12 352x288 24 testsrc2 "--keyint 12 --bframes 7 --lossless --threads 12"
    run_vector m10_lossless_ctu16   176x144 12 testsrc2 "--keyint 6 --ctu 16 --lossless --ref 3 --bframes 7 --threads 4"
    run_vector m10_lossless_crop    210x122  6 testsrc2 "--keyint 3 --lossless --ctu 64"
    VENV="N265_MIN_KEYINT=0"
    run_vector_graph "testsrc2=size=176x144:rate=25:duration=0.4[a];smptebars=size=176x144:rate=25:duration=0.4[b];[a][b]concat=n=2:v=1:a=0" \
           m10_lossless_cut  176x144 20 testsrc2 "--keyint 250 --bframes 7 --ref 2 --lossless --threads 1"
    VENV=""
    run_vector_file tests/corpus/foreman_cif.y4m \
           m10_lossless_foreman 352x288 16 testsrc2 "--keyint 8 --bframes 7 --lossless --threads 4"
    # REAL CONTENT, upshifted on the way in (the corpus is 8-bit 4:2:0
    # throughout - there is no native 10-bit y4m in tests/corpus). testsrc2
    # covers the syntax; only a natural clip drives the lookahead, the AQ
    # variance field and the motion search on the statistics they were
    # fitted against, and only an animation clip types FLAT and so puts the
    # SAO content gate in the state it spends its life in.
    run_vector_file tests/corpus/foreman_cif.y4m \
                   m10_foreman 352x288 24 testsrc2 "--keyint 12 --bframes 7 --crf 28 --aq 1 --cutree 1 --threads 4"
    run_vector_file tests/corpus/mobile_cif.y4m \
                   m10_mobile 352x288 16 testsrc2 "--keyint 8 --bframes 3 --qp 22 --aq 1 --threads 1"
    if [ "$FAST" -eq 0 ]; then
        run_vector_file tests/corpus/sintel_720p.y4m \
                   m10_sintel 1280x720 12 testsrc2 "--keyint 6 --bframes 5 --crf 30 --aq 1 --cutree 1 --threads 12"
        run_vector m10_720_t12 1280x720 12 testsrc2 "--keyint 6 --bframes 5 --aq 1 --cutree 1 --threads 12"
        run_vector m10_ctu16   352x288 12 testsrc2 "--keyint 6 --ctu 16 --ref 3 --bframes 7 --aq 1 --cutree 1 --threads 12"
        run_vector m10_ctu64   210x122  3 testsrc2 "--ctu 64"
    fi
    echo "conformance: $pass pass, $fail fail (Main10)"
    [ "$fail" -eq 0 ]
    exit
fi

# geometry coverage: CTB-aligned, unaligned (forced splits + conformance
# window), tiny, tall/narrow, HD; QP sweep on the real path
run_vector pcm_qcif       176x144  3  testsrc2  "--pcm"
run_vector pcm_crop       100x60   3  testsrc2  "--pcm"
run_vector intra_qcif     176x144  5  testsrc2  ""
run_vector intra_qvga     320x240  5  testsrc2  ""
run_vector intra_1ctb     64x64    3  testsrc2  ""
run_vector intra_crop     100x60   5  testsrc2  ""
run_vector intra_crop2    210x122  5  testsrc2  ""
run_vector intra_narrow   32x256   3  testsrc2  ""
run_vector intra_qp8      176x144  3  testsrc2  "--qp 8"
run_vector intra_qp45     176x144  3  testsrc2  "--qp 45"
run_vector intra_smooth   100x60   3  gradients ""
run_vector inter_ippp     176x144 20  testsrc2  "--keyint 10 --bframes 0"
run_vector inter_b1       176x144 21  testsrc2  "--keyint 10"
run_vector inter_static   176x144 15  smptebars "--keyint 15"
run_vector inter_crop     210x122 12  testsrc2  "--keyint 8"
run_vector aq_intra       352x288  3  testsrc2  "--keyint 1 --aq 1"
run_vector cutree_ipb     176x144 18  testsrc2  "--keyint 9 --cutree 1"
run_vector aq_ipb         176x144 15  testsrc2  "--keyint 8 --aq 1"
# rectangular/asymmetric inter PUs: low QP forces 8x8 CUs (8x4/4x8 PUs,
# where bi-prediction is forbidden and the merge list drops A1/B1), CTB 64
# reaches the 32x32 AMP shapes
run_vector inter_rect8    176x144 16  testsrc2  "--keyint 8 --qp 20"
# ABR: the rate factor is fractional, so the per-CTU dither writes
# cu_qp_delta at quant groups the CRF/CQP paths never touch, and it does so
# with the PPS flag on even where AQ and cu-tree are off
run_vector abr_cif       352x288 24  testsrc2  "--keyint 12 --bitrate 400 --aq 1 --cutree 1"
run_vector abr_flat      176x144 16  testsrc2  "--keyint 8 --bitrate 200"
# deblocking offsets: the PPS grows its control block
# (deblocking_filter_control_present_flag with pps_beta_offset_div2 and
# pps_tc_offset_div2), and both decoders have to shift the Table 8-12
# lookups by the same amount we did. One vector per sign so a decoder that
# ignores the offsets, or reads them with the wrong sign, cannot pass both.
N265_DBF_BETA=-3 N265_DBF_TC=-3 \
run_vector deblock_neg    176x144 16  testsrc2  "--keyint 8 --aq 1"
N265_DBF_BETA=3 N265_DBF_TC=2 \
run_vector deblock_pos    176x144 16  testsrc2  "--keyint 8 --aq 1"
unset N265_DBF_BETA N265_DBF_TC
run_vector intra_ctu16    176x144  3  testsrc2  "--ctu 16"
run_vector intra_ctu64    176x144  3  testsrc2  "--ctu 64"
run_vector intra_ctu64c   210x122  3  testsrc2  "--ctu 64"
# SAO: the filter is off by default (R7), so every path through
# n265_sao_write and n265_sao_apply needs its type forced on here.
run_vector_env "N265_SAO_TYPES=3 N265_SAO_CHROMA=0" \
                          sao_full  176x144 12  testsrc2  "--keyint 6"
run_vector_env "N265_SAO_TYPES=1" \
                          sao_band  176x144 12  testsrc2  "--keyint 6"
run_vector_env "N265_SAO_TYPES=2" \
                          sao_edge  176x144 12  testsrc2  "--keyint 6"
run_vector_env "N265_SAO_TYPES=3" \
                          sao_crop  210x122 12  testsrc2  "--keyint 6 --ctu 64"
# The content gate (N265_FLAT_GATE, 2026-09-11) answers PER FRAME, so it
# puts sample_adaptive_offset_enabled_flag and the two slice_sao_* flags
# in states the type knob alone cannot reach: SPS on with every slice
# off, and SPS on with the slices disagreeing inside one stream. The
# dither has to come off the source for a flat block to survive to the
# classifier at all, which is what run_vector_clean is for.
#
# testsrc2 at this size reads 22-29% flat blocks and drifts DOWN over
# twelve frames, so 25 lands mid-drift and the mixed vector really does
# switch the filter off part-way through the sequence; it also carries
# --aq, because the same per-frame type moves the AQ clamp and that has
# to reach both decoders through the QP field. (If a future ffmpeg moves
# the drift, the vector still tests a legal stream, just a less
# interesting one.)
run_vector_clean "N265_FLAT_GATE=1" \
                          sao_anim_on   176x144 12  smptebars "--keyint 6 --qp 34"
run_vector_env "N265_FLAT_GATE=101" \
                          sao_anim_off  176x144 12  testsrc2  "--keyint 6"
run_vector_clean "N265_FLAT_GATE=25" \
                          sao_anim_mix  176x144 12  testsrc2  "--keyint 6 --qp 34 --aq 1 --cutree 1"
# Threaded cells: the same content at --threads 4 and 12, CQP and ABR,
# with a B pyramid. run_vector already encodes each vector twice and
# cmp's the two runs, so these carry the same-thread-count determinism
# leg as well as the two-decoder leg.
#
# The full pyramid, at one thread and at twelve. These could not exist
# before the SPS fix: sps_max_num_reorder_pics was signalled as
# `bframes` against sps_max_dec_pic_buffering_minus1 = N265_DPB_SLOTS,
# so at --bframes 7 the declared reorder depth exceeded the declared
# buffer count. That is out of range by 7.4.3.2.1 and dec265 refuses the
# stream. Both fields now come from the reference structure itself.
run_vector b7_t1_cif   352x288 24  testsrc2  "--keyint 12 --bframes 7 --aq 1 --cutree 1 --threads 1"
run_vector b7_t12_cif  352x288 24  testsrc2  "--keyint 12 --bframes 7 --aq 1 --cutree 1 --threads 12"
run_vector thr4_cif    352x288 24  testsrc2  "--keyint 12 --bframes 5 --aq 1 --cutree 1 --threads 4"
run_vector thr12_cif   352x288 24  testsrc2  "--keyint 12 --bframes 5 --aq 1 --cutree 1 --threads 12"
run_vector thr4_abr    352x288 24  testsrc2  "--keyint 12 --bitrate 400 --aq 1 --cutree 1 --threads 4"
run_vector thr12_abr   352x288 24  testsrc2  "--keyint 12 --bitrate 400 --aq 1 --cutree 1 --threads 12"
# T2/3: frames overlap. K = N265_MAX_FRAME_WORK is a scheduling knob and
# cannot change bits, so the same content is coded at K = 1, the shipped
# K = 4 and K = 8, at twelve threads, once per RC mode. The K = 1 cell is
# the control: it is the T1 schedule (one frame in flight) running on the
# T2 code, so a difference between it and its K = 4 sibling would be the
# overlap changing bits, which is the thing the phase forbids.
run_vector k4_t12_crf  352x288 24  testsrc2  "--keyint 12 --bframes 5 --crf 26 --aq 1 --cutree 1 --threads 12"
run_vector_env "N265_MAX_FRAME_WORK=1" \
           k1_t12_cif  352x288 24  testsrc2  "--keyint 12 --bframes 5 --aq 1 --cutree 1 --threads 12"
run_vector_env "N265_MAX_FRAME_WORK=8" \
           k8_t12_b7   352x288 24  testsrc2  "--keyint 12 --bframes 7 --aq 1 --cutree 1 --threads 12"
run_vector_env "N265_MAX_FRAME_WORK=8" \
           k8_t12_abr  352x288 24  testsrc2  "--keyint 12 --bitrate 400 --aq 1 --cutree 1 --threads 12"
# T3/1: output is a prefix, so up to K frames stay in flight ACROSS API
# calls. Two cells for the two ends of that contract: --bframes 0 at
# twelve threads (the case it exists for - it used to overlap nothing but
# the flush tail), and N265_API_DEFER, which holds each call's prefix back
# until the deferred-NAL ring is half full. The bytes may not move.
run_vector b0_t12_cif  352x288 24  testsrc2  "--keyint 12 --bframes 0 --aq 1 --cutree 1 --threads 12"
run_vector_env "N265_API_DEFER=1" \
           defer_t12_cif 352x288 24 testsrc2 "--keyint 12 --bframes 5 --aq 1 --cutree 1 --threads 12"
# The right-edge reconstruction bug (2026-09-09). A fast horizontal pan
# makes the motion search find a vector near the search range, and a
# merge or AMVP candidate then carries it to a CU at the picture's right
# edge, where it is NOT clamped - only the search is. 8.5.3.3.2 says the
# decoder clips every reference coordinate into the picture, so the edge
# sample repeats without limit; our pictures carry a finite
# N265_REF_PAD border instead, and the read ran off the end of it into
# the next row's LEFT border. Both decoders rejected the recon before
# n265_ref_win landed. Reproduces at every thread count, so it gets one
# cell at 1 and one at 12.
run_vector_graph "testsrc2=size=176x144:rate=25,scroll=horizontal=0.4" \
           pan_edge_t1  176x144 40 testsrc2 "--keyint 10 --bframes 3 --ref 3 --aq 1 --cutree 1 --threads 1"
run_vector_graph "testsrc2=size=176x144:rate=25,scroll=horizontal=0.4" \
           pan_edge_t12 176x144 40 testsrc2 "--keyint 10 --bframes 3 --ref 3 --aq 1 --cutree 1 --threads 12"
# D4's observation lag. Unlike K this one IS bitstream-affecting, so it
# gets a cell at one thread and at twelve: the stream must differ from
# the D = 0 one and must not differ between thread counts.
run_vector_env "N265_RC_LAG=3" \
           lag3_t1_abr 352x288 24  testsrc2  "--keyint 12 --bitrate 400 --aq 1 --cutree 1 --threads 1"
run_vector_env "N265_RC_LAG=3" \
           lag3_t12_abr 352x288 24 testsrc2  "--keyint 12 --bitrate 400 --aq 1 --cutree 1 --threads 12"
# The preset ladder (2026-09-10). fast and slow are DEFAULT SOURCES, so
# they reach syntax the shipped defaults never do - fast clears
# amp_enabled_flag in the SPS and codes without the rectangular part
# modes, slow raises the reference count and the B run - and a preset
# must not be able to break cross-thread-count identity either. One cell
# each at one thread and at twelve.
run_vector fast_t1_cif  352x288 24 testsrc2 "--keyint 12 --preset fast --aq 1 --cutree 1 --threads 1"
run_vector fast_t12_cif 352x288 24 testsrc2 "--keyint 12 --preset fast --aq 1 --cutree 1 --threads 12"
run_vector slow_t1_cif  352x288 24 testsrc2 "--keyint 12 --preset slow --aq 1 --cutree 1 --threads 1"
run_vector slow_t12_cif 352x288 24 testsrc2 "--keyint 12 --preset slow --aq 1 --cutree 1 --threads 12"
# 2026-09-10: the corner that spent the whole ladder reading off the end of
# the reference allocation. CTB 16 makes the coding blocks small enough that
# a merge candidate is nearly always a neighbour's decided vector, --ref 3
# with --bframes 7 gives tmvp_mv's POC scaling its widest distance ratios,
# and the product points far outside the picture. The decoder clips such a
# read to the picture edge (8.5.3.3.2); before n265_mc_clip this encoder did
# not, and its reconstruction stopped matching either decoder. The cell is
# here to keep the three agreeing, at one thread and at twelve.
run_vector ctu16_r3_b7_t1  352x288 24 testsrc2 "--keyint 12 --ctu 16 --ref 3 --bframes 7 --aq 1 --cutree 1 --threads 1"
run_vector ctu16_r3_b7_t12 352x288 24 testsrc2 "--keyint 12 --ctu 16 --ref 3 --bframes 7 --aq 1 --cutree 1 --threads 12"
# 2026-09-10: the retirement moved to dispatch, so a picture leaves the
# RPS - and with it the decoder's DPB - as soon as nothing left in the
# mini-GOP plan reads it, and sps_max_dec_pic_buffering_minus1 became the
# walked peak rather than a bound. This is the widest plan the retirement
# has to get right: --bframes 7 at --ref 2, with a keyint that is NOT a
# multiple of the run, so b-adapt and the key edge both cut mini-GOPs
# short and the lifetimes differ from plan to plan. Retiring one frame
# early is a missing reference, which is exactly what the two decoders
# catch.
run_vector dpb_retire_b7_t1  352x288 30 testsrc2 "--keyint 10 --bframes 7 --ref 2 --crf 26 --aq 1 --cutree 1 --threads 1"
run_vector dpb_retire_b7_t12 352x288 30 testsrc2 "--keyint 10 --bframes 7 --ref 2 --crf 26 --aq 1 --cutree 1 --threads 12"
# 2026-09-12: the non-IDR intra picture. An I slice carried as a TRAIL_R,
# so the POC line continues, the DPB is NOT flushed, and the picture
# signals a short-term RPS of its own that uses none of the pictures it
# lists - a shape nothing else in this suite produces. Two ways in, both
# needed: the forced knob puts one mid-mini-GOP on a source with no cut
# in it (deterministic, and the only way to place it at a chosen POC),
# and the concat graph makes a REAL hard cut inside min-keyint, which is
# the path the default takes. A decoder that drops the listed-but-unused
# pictures at this picture, or that reads the POC as reset, loses the
# reference the next mini-GOP predicts from, and the recon stops
# matching - which is the whole gate. --keyint 250 keeps a periodic IDR
# out of the way, and on the cut cells it is also what puts the auto
# min-keyint (25 at 25 fps) above the cut; --ref 2 --bframes 7 makes the
# carried set the widest it gets.
run_vector_env "N265_FORCE_NONIDR=13" \
           nonidr_forced_t1  352x288 24 testsrc2 "--keyint 250 --bframes 7 --ref 2 --crf 26 --aq 1 --cutree 1 --threads 1"
run_vector_env "N265_FORCE_NONIDR=13" \
           nonidr_forced_t12 352x288 24 testsrc2 "--keyint 250 --bframes 7 --ref 2 --crf 26 --aq 1 --cutree 1 --threads 12"
NONIDR_CUT="testsrc2=size=176x144:rate=25:duration=0.4[a];smptebars=size=176x144:rate=25:duration=0.4[b];[a][b]concat=n=2:v=1:a=0"
# (VENV around the graph form: the two wrappers set different variables
# and run_vector reads both. N265_MIN_KEYINT=0 is what lets the detector
# reach the new kind; the default 4 cannot place one.)
VENV="N265_MIN_KEYINT=0"
run_vector_graph "$NONIDR_CUT" \
           nonidr_cut_t1  176x144 20 testsrc2 "--keyint 250 --bframes 7 --ref 2 --aq 1 --cutree 1 --threads 1"
VENV="N265_MIN_KEYINT=0"
run_vector_graph "$NONIDR_CUT" \
           nonidr_cut_t12 176x144 20 testsrc2 "--keyint 250 --bframes 7 --ref 2 --aq 1 --cutree 1 --threads 12"
VENV=""
# ...and the same cut at the shipped bound, where it is an IDR instead.
# Both structures have to be conformant on the same source, and this cell
# is what says the two differ on a source that has a cut inside the
# window at all.
run_vector_graph "$NONIDR_CUT" \
           nonidr_cut_idr_t12 176x144 20 testsrc2 "--keyint 250 --bframes 7 --ref 2 --aq 1 --cutree 1 --threads 12"
# 2026-09-12: the deep window (phase 7 item 3, D3), an OPT-IN mode the
# default path never takes. --bframes 15 is a mini-GOP of 16 under a
# five-level pyramid: the first structure in this suite whose reorder
# depth is 4 rather than 3, whose leaves sit at cascade +5, and whose
# declared buffering the walk has to solve at a run length nothing else
# here produces. --b-adapt 0 is what makes the cell worth running - the
# motion rule picks its own run length on a synthetic source, and this
# vector exists to code the full-depth pyramid every time - and
# --keyint 20 then cuts the second mini-GOP short at the key edge, so
# the tail shape rides along. --ref 2 is the widest carried anchor set
# the mode is priced at.
run_vector deep_b15_t1  352x288 34 testsrc2 "--keyint 20 --bframes 15 --rc-lookahead 24 --ref 2 --b-adapt 0 --crf 26 --aq 1 --cutree 1 --threads 1"
run_vector deep_b15_t12 352x288 34 testsrc2 "--keyint 20 --bframes 15 --rc-lookahead 24 --ref 2 --b-adapt 0 --crf 26 --aq 1 --cutree 1 --threads 12"
# 2026-09-12: --ref 3 above --bframes 7, which encoder open refused until
# the pool was sized from the shape (item dpbpool). These are the two
# shapes that refusal covered: --bframes 8, where the marked-picture peak
# is seven, and --bframes 15, where it is eight and the pool runs eleven
# slots - more than any other vector here allocates. They are the only
# structures in this suite that carry three anchors AND a five-level
# pyramid, so every list is at its widest - three entries per list, both
# lists full on every B - and the RPS lists more pictures per slice than
# anything else does. A pool one slot short reads here as a reference the
# decoder does not have, which is what the two decoders catch.
# --b-adapt 0 on the deep cell for the reason deep_b15 has it: the
# full-depth pyramid every time.
run_vector r3_b8_t1   352x288 30 testsrc2 "--keyint 12 --bframes 8 --rc-lookahead 20 --ref 3 --crf 26 --aq 1 --cutree 1 --threads 1"
run_vector r3_b8_t12  352x288 30 testsrc2 "--keyint 12 --bframes 8 --rc-lookahead 20 --ref 3 --crf 26 --aq 1 --cutree 1 --threads 12"
run_vector r3_b15_t1  352x288 34 testsrc2 "--keyint 20 --bframes 15 --rc-lookahead 24 --ref 3 --b-adapt 0 --crf 26 --aq 1 --cutree 1 --threads 1"
run_vector r3_b15_t12 352x288 34 testsrc2 "--keyint 20 --bframes 15 --rc-lookahead 24 --ref 3 --b-adapt 0 --crf 26 --aq 1 --cutree 1 --threads 12"
# 2026-09-12: the shot planner (phase 7 item 6 step 2), the other opt-in
# mode the default path never takes. --shot-window 20 holds a queue far
# deeper than --rc-lookahead asked for, which is new geometry for the
# free run and for the flush, and --keyint 12 puts three shot openers in
# 34 frames so several plans are priced and the base QP each one carries
# lands in a real stream. CRF, because the mode refuses to run anywhere
# else.
run_vector shotwin_t1  352x288 34 testsrc2 "--keyint 12 --bframes 7 --shot-window 20 --crf 26 --aq 1 --cutree 1 --threads 1"
run_vector shotwin_t12 352x288 34 testsrc2 "--keyint 12 --bframes 7 --shot-window 20 --crf 26 --aq 1 --cutree 1 --threads 12"
# --lossless (phase 8 item 5b): transquant_bypass_enabled_flag in the PPS
# and cu_transquant_bypass_flag on every CU, so the coded levels ARE the
# residual samples and the reconstruction is the SOURCE. run_vector picks
# the source as the reference for these, and checks the encoder's own
# --dump-recon against it first, so each cell is a round trip: source ->
# bitstream -> ffmpeg and dec265 -> source, byte for byte.
#
# What the four cells are for. The IPB pair puts bypass CUs on the P and
# B paths with the full pyramid - inter prediction, merge, skip and the
# forced TU32 quad at CB64 all reached - at one thread and at twelve, and
# run_vector's own two-run cmp carries determinism with them. ctu16 walks
# the other transform-tree geometry (min CB == max CB == 16) and is the
# cell that says the SAO seam allowance is not being leaned on: SAO is off
# at the sequence level in a lossless stream, so this one is held exact
# through both decoders. The scenecut cell places a non-IDR intra picture
# and its own short-term RPS inside a bypass sequence. The crop cell is
# the conformance window, where the padded CTU column past the picture
# edge has to reconstruct exactly like any other.
run_vector lossless_ipb_t1  352x288 24 testsrc2 "--keyint 12 --bframes 7 --lossless --threads 1"
run_vector lossless_ipb_t12 352x288 24 testsrc2 "--keyint 12 --bframes 7 --lossless --threads 12"
run_vector lossless_ctu16   176x144 12 testsrc2 "--keyint 6 --ctu 16 --lossless --ref 3 --bframes 7 --threads 4"
run_vector lossless_crop    210x122  6 testsrc2 "--keyint 3 --lossless --ctu 64"
VENV="N265_MIN_KEYINT=0"
run_vector_graph "testsrc2=size=176x144:rate=25:duration=0.4[a];smptebars=size=176x144:rate=25:duration=0.4[b];[a][b]concat=n=2:v=1:a=0" \
           lossless_cut  176x144 20 testsrc2 "--keyint 250 --bframes 7 --ref 2 --lossless --threads 1"
VENV=""
if [ "$FAST" -eq 0 ]; then
    run_vector thr4_720    1280x720 12 testsrc2 "--keyint 6 --bframes 5 --aq 1 --cutree 1 --threads 4"
    run_vector thr12_720   1280x720 12 testsrc2 "--keyint 6 --bframes 5 --aq 1 --cutree 1 --threads 12"
    run_vector thr12_720abr 1280x720 12 testsrc2 "--keyint 6 --bitrate 4000 --aq 1 --cutree 1 --threads 12"
    run_vector inter_cif   352x288 25  testsrc2  "--keyint 12"
    run_vector inter_b_cif 352x288 24  testsrc2  "--keyint 9"
    run_vector inter_b_smooth 176x144 16 gradients ""
    run_vector inter_ctu64 176x144 12  testsrc2  "--keyint 6 --ctu 64"
    run_vector inter_amp   352x288 20  testsrc2  "--keyint 10 --ctu 64 --aq 1"
    # AMVP 2Nx2N at the CB64 node: merge_flag 0 on a 64x64 CU, so
    # inter_pred_idc / ref_idx / mvd / mvp_flag ride the forced TU32
    # transform quad. QP 38 keeps the quadtree at depth 0 often enough
    # for the arm to be chosen (verified: the stream moves with the arm).
    run_vector inter_cb64  352x288 20  testsrc2  "--keyint 10 --ctu 64 --qp 38"
    run_vector abr_720     1280x720 12 testsrc2  "--keyint 6 --bitrate 4000 --aq 1 --cutree 1"
    run_vector pcm_cif     352x288  5  testsrc2  "--pcm"
    # real content, where the residual is not synthetic: the magnitudes a
    # bypass block codes come from the picture, and testsrc2's do not look
    # like a camera's.
    run_vector_file tests/corpus/foreman_cif.y4m \
           lossless_foreman 352x288 16 testsrc2 "--keyint 8 --bframes 7 --lossless --threads 4"
    run_vector intra_cif   352x288 10 testsrc2  ""
    run_vector intra_720   1280x720 3 testsrc2  ""
    run_vector intra_bars  176x144 30 smptebars ""
    run_vector intra_qp0   64x64    2  testsrc2  "--qp 0"
    run_vector intra_qp51  176x144  3  testsrc2  "--qp 51"
fi

echo "conformance: $pass pass, $fail fail"
[ "$fail" -eq 0 ]
