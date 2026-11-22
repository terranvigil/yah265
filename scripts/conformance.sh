#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
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
# bit depth; the two variables stay separate so a sanitizer tree or an
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

# A shot list path for the next run_vector: the source is scanned into it
# with --shot-scan before the encode, so the vector covers the scan and a
# list made from the very file it codes. Set by run_vector_scan.
VSCAN=""

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
    if [ -n "$VSCAN" ] && ! "$ENC" "$y4m" --shot-scan "$VSCAN" 2>/dev/null; then
        echo "SUMMARY $name FAIL (shot scan)"; fail=$((fail+1)); return
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
    # -export_side_data film_grain: ffmpeg's hevc decoder APPLIES an H.274
    # film grain SEI by default, and this comparison is about the coded
    # picture - what the encoder reconstructed - not about what a display
    # pipeline would put on the screen. The flag hands the parameters out
    # as side data instead of synthesizing, which is byte-neutral on a
    # stream without the SEI (proven, docs/phase12.md step 0) and is the
    # only thing that makes a --film-grain vector's recon claim mean
    # anything. dec265 ignores the message entirely and needs no flag.
    "$FFMPEG" -loglevel error -export_side_data film_grain \
        -i "$TMP/$name.hevc" -f rawvideo \
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
    # --hash (phase 14 item 1): the stream CLAIMS a hash of every decoded
    # picture, so the vector is only worth running if something checks the
    # claim. Three things do, and they fail on different bugs:
    #
    #   - scripts/hash_check.py recomputes all three of D.3.19's hashes
    #     from the pictures FFMPEG decoded, with its own implementation of
    #     the clause - the one checked against x265's own streams. It is
    #     the only oracle that covers the CRC and the checksum forms.
    #   - ffmpeg's own verification, which is live only under
    #     `-err_detect crccheck` and only for MD5, and which reports a
    #     disagreement as "mismatching checksum of plane N" at error
    #     level. The negative control is in docs/phase14.md.
    #   - dec265 -c, which parses the message and must not refuse the
    #     stream. It is a no-regression leg and not an oracle: the check
    #     is inert in the libde265 build this gate runs against (the
    #     suffix SEI reaches it after the picture has left the queue).
    #
    # The geometry handed to the checker is the CODED picture, which is
    # what D.3.19 hashes; every vector with --hash therefore has
    # dimensions that are already a multiple of the minimum coding block,
    # and ffmpeg's own check is what covers a padded one.
    case " $eargs " in
    *" --hash "*)
        if ! python3 scripts/hash_check.py "$TMP/$name.hevc" \
                "$TMP/$name.ff.yuv" --geometry "$size" --csp "$CSP" \
                --depth "$DEPTH" >"$TMP/$name.hash" 2>&1; then
            echo "SUMMARY $name FAIL (decoded picture hash)"
            sed -n 1,2p "$TMP/$name.hash" >&2
            fail=$((fail+1)); return
        fi
        if "$FFMPEG" -v debug -err_detect crccheck -i "$TMP/$name.hevc" \
                -f null - 2>&1 | grep -q "mismatching checksum"; then
            echo "SUMMARY $name FAIL (ffmpeg reports a hash mismatch)"
            fail=$((fail+1)); return
        fi
        if ! "$DEC265" -q -c "$TMP/$name.hevc" >/dev/null 2>&1; then
            echo "SUMMARY $name FAIL (dec265 -c refused the stream)"
            fail=$((fail+1)); return
        fi
        ;;
    esac
    # --open-gop (phase 14 item 3): the stream CLAIMS its CRA pictures are
    # random access points, and nothing above this line tests that claim -
    # every check so far decodes from the start, which is the one case a
    # CRA is indistinguishable from a trailing I picture in.
    #
    # So this leg cuts the stream at a CRA the way a player seeking to one
    # does and feeds the result to both decoders: ffmpeg in output order
    # byte for byte, dec265 as a multiset of frames because `-o` emits
    # decode order. Both are held to the SAME reference the recon match
    # above already agreed with, so a seek that matches it matches the
    # encoder's own reconstruction. A trailing picture that predicted
    # across the CRA, or a leading picture with the wrong NAL type, fails
    # here and nowhere else; the negative control is N265_OPENGOP_NOBARRIER
    # (docs/phase14.md item 3).
    #
    # Armed by the flag being NAMED, the way the hash leg is, and not by a
    # CRA merely being present - open GOP is the default, so most of the
    # list would otherwise pay for a second pair of decodes it has no
    # question about.
    case " $eargs " in
    *" --open-gop "*)
        radl=$(printf '%s\n' "$eargs" | sed -n 's/.*--radl \([0-9]*\).*/\1/p')
        if ! FFMPEG="$FFMPEG" DEC265="$DEC265" python3 scripts/seek_check.py \
                "$TMP/$name.hevc" "$TMP/$name.ff.yuv" --geometry "$size" \
                --csp "$CSP" --depth "$DEPTH" --expect-radl "${radl:-0}" \
                >"$TMP/$name.seek" 2>&1; then
            echo "SUMMARY $name FAIL (seek to a CRA)"
            sed -n 1,2p "$TMP/$name.seek" >&2
            fail=$((fail+1)); return
        fi
        ;;
    esac
    # --shot-list (per-shot stage A1): every listed shot start has to be a
    # place a decoder can begin. The seek leg cuts the stream at each IRAP
    # in turn and both decoders must match the full decode from there,
    # and every listed start must be one of those cuts.
    case " $eargs " in
    *" --shot-list "*)
        sl=$(printf '%s\n' "$eargs" | sed -n 's/.*--shot-list \([^ ]*\).*/\1/p')
        starts=$(awk '/^shots /{on=1; next} /^end/{on=0} on{sub(/#.*/, ""); if (NF) printf "%s%s", (n++ ? "," : ""), $1}' "$sl")
        if ! FFMPEG="$FFMPEG" DEC265="$DEC265" python3 scripts/seek_check.py \
                "$TMP/$name.hevc" "$TMP/$name.ff.yuv" --geometry "$size" \
                --csp "$CSP" --depth "$DEPTH" --all-irap \
                --expect-starts "$starts" >"$TMP/$name.seek" 2>&1; then
            echo "SUMMARY $name FAIL (seek to a listed shot)"
            sed -n 1,2p "$TMP/$name.seek" >&2
            fail=$((fail+1)); return
        fi
        ;;
    esac
    # --intra-refresh: a decoder that joins at the second sweep start
    # recovers exactly where the recovery point says it does
    case " $eargs " in
    *" --intra-refresh "*)
        if ! DEC265="$DEC265" python3 scripts/recovery_check.py \
                --stream "$TMP/$name.hevc" --recon "$TMP/$name.src.yuv" \
                --size "$size" --nth 2 >"$TMP/$name.rp" 2>&1; then
            echo "SUMMARY $name FAIL (join at a sweep start)"
            sed -n 1,2p "$TMP/$name.rp" >&2
            fail=$((fail+1)); return
        fi
        ;;
    esac
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

# --output-depth 8 on a 10-BIT source (phase 14 item 6): the source is
# generated at ten bits and the 8-bit binary encodes it, which is the one
# combination where the picture the library sees is not the bytes the file
# carries. Only the SOURCE format changes; the encoder, the decoders and
# the recon comparison are the 8-bit list's own.
run_vector_10in() {
    _sf10=$SRCFMT
    SRCFMT="-pix_fmt yuv420p10le -strict -1 -f yuv4mpegpipe"
    run_vector "$@"
    SRCFMT=$_sf10
}

# TWO-PASS (phase 12 item 4): the same vector, with a --pass 1 in front.
#
# The source is generated here and run_vector regenerates the identical
# file from the same deterministic lavfi graph, so pass 1 and pass 2 see
# the same pictures. run_vector then runs pass 2 TWICE and cmps the two
# streams, which is the determinism leg for a planned encode: the plan is
# read from the same file both times and the encode has to land on the
# same bytes.
run_vector_2p() {
    # $1 name  $2 WxH  $3 frames  $4 lavfi source  $5 encoder args
    n2p=$1; s2p=$2; f2p=$3; g2p=$4; a2p=$5
    y2p="$TMP/$n2p.y4m"
    st2p="$TMP/$n2p.stats"
    "$FFMPEG" -loglevel error -f lavfi -i "$g2p=size=$s2p:rate=25" \
        -vf "noise=alls=12:allf=t+u" -frames:v "$f2p" $SRCFMT "$y2p" -y \
        2>/dev/null
    if [ ! -s "$y2p" ]; then
        echo "SUMMARY $n2p SKIP (source gen failed)"
        return
    fi
    # shellcheck disable=SC2086
    "$ENC" "$y2p" -o "$TMP/$n2p.p1.hevc" $a2p --pass 1 --stats "$st2p" \
        2>/dev/null || {
        echo "SUMMARY $n2p FAIL (pass 1)"; fail=$((fail+1)); return; }
    run_vector "$n2p" "$s2p" "$f2p" "$g2p" "$a2p --pass 2 --stats $st2p"
}

# same vector, with an existing y4m as the source. The conversion to the
# list's depth happens in run_vector's own ffmpeg call, so one 8-bit
# corpus clip serves both lists.
run_vector_file() {
    VFILE=$1; shift
    run_vector "$@"
    VFILE=""
}

# same vector on an existing file, scanned first into the list path given
run_vector_scan() {
    VSCAN=$1; shift
    run_vector_file "$@"
    VSCAN=""
}

# same vector with the dither turned off at the source
run_vector_clean() {
    VENV=$1; shift
    VNOISE="null"
    run_vector "$@"
    VNOISE=""; VENV=""
}

# same vector on a source that is ANIMATION ON TWOS: the list's own
# dither first, then half the frame rate and back, so each frame is
# emitted twice and every other one is a byte-for-byte duplicate of its
# predecessor. The order is what makes it a duplicate - a temporal
# dither applied after the repeat would re-noise both copies - and the
# duplication has to be in this filter chain rather than in the input
# spec for exactly that reason.
run_vector_dup() {
    VENV=$1; shift
    VNOISE="noise=alls=12:allf=t+u,fps=12.5,fps=25"
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
#     everywhere; intra NxN is where it is signaled under one
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
    # ...and the same tree three and four levels deep (phase 14 item 5).
    # The chroma half of the recursion is format-specific - whether a
    # child carries its own chroma, and whether a node codes one cbf per
    # component or the stacked 4x4 chroma pair - so each format checks it.
    run_vector ${P}_tu3    176x144 16 testsrc2 "--keyint 8 --ctu 32 --bframes 5 --qp 30 --tu-inter-depth 3 --tu-intra-depth 3 --threads 1"
    run_vector ${P}_tu4    176x144 16 testsrc2 "--keyint 8 --ctu 32 --bframes 5 --qp 30 --tu-inter-depth 4 --tu-intra-depth 4 --threads 4"
    # IPB with the pyramid at one thread and at twelve: the split chroma
    # MC fractions, uni and bi, and run_vector's own two-run cmp carries
    # determinism with each.
    run_vector ${P}_ipb_t1  352x288 24 testsrc2 "--keyint 12 --bframes 7 --aq 1 --cutree 1 --threads 1"
    run_vector ${P}_ipb_t12 352x288 24 testsrc2 "--keyint 12 --bframes 7 --aq 1 --cutree 1 --threads 12"
    # the widest RPS and the deepest pyramid the encoder builds
    run_vector ${P}_deep   352x288 34 testsrc2 "--keyint 20 --bframes 15 --rc-lookahead 24 --ref 3 --b-adapt 0 --crf 26 --aq 1 --cutree 1 --threads 1"
    # CRF, and AQ + cu-tree: the fractional base QP writes cu_qp_delta at
    # quantization groups CQP never reaches, and every one of those QPs
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
    # ...and the same tree three and four levels deep (phase 14 item 5).
    # The chroma half of the recursion is format-specific - whether a
    # child carries its own chroma, and whether a node codes one cbf per
    # component or its own 4x4 chroma leaf - so each format checks it.
    run_vector ${P}_tu3    176x144 16 testsrc2 "--keyint 8 --ctu 32 --bframes 5 --qp 30 --tu-inter-depth 3 --tu-intra-depth 3 --threads 1"
    run_vector ${P}_tu4    176x144 16 testsrc2 "--keyint 8 --ctu 32 --bframes 5 --qp 30 --tu-inter-depth 4 --tu-intra-depth 4 --threads 4"
    # IPB with the pyramid at one thread and at twelve: quarter-pel
    # chroma MC on full-size planes, uni and bi, and run_vector's own
    # two-run cmp carries determinism with each.
    run_vector ${P}_ipb_t1  352x288 24 testsrc2 "--keyint 12 --bframes 7 --aq 1 --cutree 1 --threads 1"
    run_vector ${P}_ipb_t12 352x288 24 testsrc2 "--keyint 12 --bframes 7 --aq 1 --cutree 1 --threads 12"
    # the widest RPS and the deepest pyramid the encoder builds
    run_vector ${P}_deep   352x288 34 testsrc2 "--keyint 20 --bframes 15 --rc-lookahead 24 --ref 3 --b-adapt 0 --crf 26 --aq 1 --cutree 1 --threads 1"
    # CRF, and AQ + cu-tree: the fractional base QP writes cu_qp_delta at
    # quantization groups CQP never reaches, and every one of those QPs
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
    # --hash 2, the CRC form (phase 14 item 1). At 4:4:4 because the hash
    # is per COLOR COMPONENT and the chroma planes are full size here, so
    # a hash that walked a plane with 4:2:0's geometry would be wrong in a
    # way the 4:2:0 vectors cannot show. The CRC form because ffmpeg only
    # verifies MD5: on this vector the whole claim rests on
    # scripts/hash_check.py's independent implementation, which is the
    # reason that script exists.
    run_vector ${P}_hash_crc 352x288 16 testsrc2 "--keyint 8 --bframes 3 --crf 28 --aq 1 --cutree 1 --hash 2 --threads 4"
    # Weighted prediction at 4:4:4 (phase 14 item 2). The chroma weight is
    # applied to a chroma block that IS a luma block here, so the chroma
    # MC runs at quarter-pel on a full-size plane and the weighted path
    # sees block shapes the 4:2:0 vectors never present it - 64x64 among
    # them, which is the widest thing the weighted kernel is ever handed.
    run_vector_file tests/corpus/fade_cif.y4m \
           ${P}_wp_fade 352x288 24 testsrc2 "--keyint 12 --bframes 7 --crf 28 --aq 1 --cutree 1 --weightp --weightb --threads 4"
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
    # of Qp' desynchronizes the dequantizer from the decoder's, and the
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
    # CRF and ABR: the fractional base QP puts cu_qp_delta at quantization
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
    # desynchronizes the parse from that CTB on. Band and edge separately,
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
    # The whole of phase 14 item 1 at once, at the depth HDR is actually
    # delivered in: the decoded picture hash, the full color description
    # (BT.2100 PQ), and the two HDR10 static metadata SEIs REPEATED at
    # every IRAP - which is what --keyint 8 over 24 frames is for. The SPS
    # grows a VUI block that every field after it depends on, so a decoder
    # that mis-parses one flag reaches the slice data with everything
    # shifted; both decoders are held bit-exact, and the hash leg above
    # adds the picture-level claim on top.
    # Weighted prediction at Main10 (phase 14 item 2). The offsets are
    # signaled in 8-BIT levels at every depth and 8.5.3.3.4.3 shifts
    # them by WpOffsetBdShiftY at use, so a writer that scaled the offset
    # itself - or a predictor that forgot to - produces a stream that
    # parses and reconstructs a different picture. This is the vector
    # that says it does not. fade_cif is upshifted on the way in, as
    # every Main10 vector here is.
    run_vector_file tests/corpus/fade_cif.y4m \
           m10_wp_fade 352x288 32 testsrc2 "--keyint 12 --bframes 7 --crf 28 --aq 1 --cutree 1 --weightp --weightb --threads 4"
    # open GOP at the depth HDR is delivered in, with the seek leg on it:
    # the barrier and the NAL typing are depth-independent, and this is
    # what says the second dispatch table agrees.
    run_vector m10_opengop 352x288 26 testsrc2 "--keyint 8 --bframes 7 --ref 2 --crf 26 --aq 1 --cutree 1 --open-gop --threads 4"
    # the noise reduction at Main10, where the coefficients are four
    # times what they are at 8 bits and the estimator's reciprocal is
    # therefore a quarter: it says the second dispatch table agrees and
    # that nothing in the offset arithmetic assumed an 8-bit magnitude.
    run_vector m10_nr      352x288 24 testsrc2 "--keyint 6 --bframes 3 --qp 24 --aq 1 --cutree 1 --nr-intra 1000 --nr-inter 1000 --threads 12"
    # the source filter at Main10, where its weight tables are built at
    # four times the sample scale and the queue holds 16-bit planes
    run_vector m10_mcstf   352x288 23 testsrc2 "--keyint 8 --bframes 3 --qp 28 --aq 1 --cutree 1 --mcstf --threads 4"
    run_vector m10_hdr10 352x288 24 testsrc2 "--keyint 8 --bframes 3 --crf 28 --aq 1 --cutree 1 --hash 1 --colorprim bt2020 --transfer smpte2084 --colormatrix bt2020nc --range limited --chromaloc 2 --sar 1:1 --master-display G(13250,34500)B(7500,3000)R(34000,16000)WP(15635,16450)L(10000000,5) --max-cll 1000,400 --threads 4"
    # m10_hdr10 above codes with the PQ tuning (item 56) and this one with
    # the HLG tuning: chroma six steps finer than luma in the PPS
    run_vector m10_hlg 352x288 24 testsrc2 "--keyint 8 --bframes 3 --crf 28 --aq 1 --cutree 1 --hash 1 --colorprim bt2020 --transfer arib-std-b67 --colormatrix bt2020nc --range limited --threads 4"
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
# VBV (phase 12 item 2), one vector per mode. What is new on the WIRE is
# hrd_parameters in the SPS's VUI - nal_hrd_parameters_present_flag, the
# two E.2.3 scaled pairs and cbr_flag - so these two are the only vectors
# in the suite whose SPS a decoder can get wrong AFTER vui_timing_info and
# still reach the slice data with everything shifted by a few bits. Both
# decoders parse the whole VUI or the picture size they report is wrong,
# which is how the 4:4:4 separate_colour_plane_flag omission was found.
#
# The buffers are deliberately tight (one second under ABR, half a second
# of the cap under CRF) so the clamp actually fires inside 24 frames and
# the vector carries a stream the buffer really constrained.
run_vector vbv_abr       352x288 24  testsrc2  "--keyint 12 --bitrate 400 --vbv-maxrate 400 --vbv-bufsize 400 --aq 1 --cutree 1"
run_vector vbv_crf       352x288 24  testsrc2  "--keyint 12 --crf 28 --vbv-maxrate 500 --vbv-bufsize 250 --aq 1 --cutree 1"
# live control: a rate step down and up under capped VBR, a requested key
# frame inside a mini-GOP under open GOP (an IDR), and QP limits, a zone
# and a CRF change on one stream
printf '8 rate bitrate=200 maxrate=300 bufsize=300\n16 rate bitrate=400 maxrate=600 bufsize=600\n' > "$TMP/ctl_rate.log"
printf '11 key\n' > "$TMP/ctl_key.log"
printf '4 qp min=30 max=40\n10 zone last=17 offset=-4\n12 crf value=24\n18 qp min=0 max=51\n' > "$TMP/ctl_qp.log"
run_vector ctl_rate_vbr  352x288 24  testsrc2  "--keyint 30 --bitrate 400 --vbv-maxrate 600 --vbv-bufsize 600 --no-hrd --threads 1 --control $TMP/ctl_rate.log"
run_vector ctl_key_open  352x288 24  testsrc2  "--keyint 8 --open-gop --crf 28 --control $TMP/ctl_key.log"
run_vector ctl_qp_zone   352x288 24  testsrc2  "--keyint 30 --crf 28 --control $TMP/ctl_qp.log"
# TWO-PASS (phase 12 item 4). Nothing new is on the WIRE - a planned
# encode writes the same syntax an ABR one does, down to the fractional
# base's cu_qp_delta dither - so what these two vectors check is that the
# plan does not take the encoder anywhere the decoders cannot follow: the
# per-frame base comes from a file instead of from the loop, and on
# twopass_vbv it is then clamped by the buffer as well, which is the
# composition this item claims and the one place two rate-control
# mechanisms write the same quantizer.
run_vector_2p twopass_abr 352x288 24  testsrc2  "--keyint 12 --bitrate 400 --aq 1 --cutree 1"
run_vector_2p twopass_vbv 352x288 24  testsrc2  "--keyint 12 --bitrate 400 --vbv-maxrate 400 --vbv-bufsize 400 --aq 1 --cutree 1"
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
# before the SPS fix: sps_max_num_reorder_pics was signaled as
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
# a merge candidate is nearly always a neighbor's decided vector, --ref 3
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
# --no-scenecut: the cut at 10 places no key frame, so the IDRs are 0, 16
# and 32 only and the cut is coded inside the GOP
run_vector_graph "testsrc2=size=176x144:rate=25:duration=0.4[a];smptebars=size=176x144:rate=25:duration=1.2[b];[a][b]concat=n=2:v=1:a=0" \
           nosc_grid_t4 176x144 40 testsrc2 "--keyint 16 --no-scenecut --no-open-gop --bframes 7 --ref 2 --crf 28 --aq 1 --cutree 1 --threads 4"
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
# the transquant-bypass path down a three-level tree (phase 14 item 5)
run_vector lossless_tu3     352x288 12 testsrc2 "--keyint 6 --bframes 3 --lossless --tu-inter-depth 3 --tu-intra-depth 3 --threads 4"
VENV="N265_MIN_KEYINT=0"
run_vector_graph "testsrc2=size=176x144:rate=25:duration=0.4[a];smptebars=size=176x144:rate=25:duration=0.4[b];[a][b]concat=n=2:v=1:a=0" \
           lossless_cut  176x144 20 testsrc2 "--keyint 250 --bframes 7 --ref 2 --lossless --threads 1"
VENV=""
# N265_RDOQ=2 (phase 9 step 2): the lattice on the commit path only, so
# the levels a CU RECORDS are not the ones its trial chose. That makes
# the encoder's own --dump-recon the claim under test - a commit that
# rebuilt the recon from the wrong levels, or a cbf left over from the
# trial, stops matching at the first CTU - and both decoders are what say
# so. Two cells: the full pyramid with AQ and cu-tree at twelve threads,
# which is where the merge / AMVP / partition winners are re-quantized at
# their commit and where a merged 2Nx2N CU whose residual the lattice
# takes away has to come out as a SKIP; and CTB 16 at one thread for the
# other transform-tree geometry and the intra NxN commit.
run_vector_env "N265_RDOQ=2" \
           rdoq2_ipb_t12  352x288 24 testsrc2 "--keyint 12 --bframes 7 --crf 26 --aq 1 --cutree 1 --threads 12"
run_vector_env "N265_RDOQ=2" \
           rdoq2_ctu16_t1 176x144 16 testsrc2 "--keyint 8 --ctu 16 --bframes 5 --aq 1 --cutree 1 --threads 1"
# --rdoq-level (2026-09-14): the same three quantizers, reached the way a
# USER reaches them, and the two cells are the two ENDS - the flag's own
# map onto N265_RDOQ is what they check, since the default list already
# walks the middle (`--rdoq-level 1` IS `--preset medium`, byte for byte,
# and the identity cmp says so rather than a vector). Level 0 is plain
# quantization everywhere, where sign-bit hiding and the all-zero margin
# meet levels no lattice ever touched; level 2 is the lattice in every RD
# trial, which is the compiled-in default reached from a medium command
# line. One thread and twelve, so the flag carries the determinism leg.
run_vector rdoqlvl0_t12 352x288 24 testsrc2 "--keyint 12 --bframes 7 --crf 26 --aq 1 --cutree 1 --rdoq-level 0 --threads 12"
run_vector rdoqlvl2_t1  176x144 16 testsrc2 "--keyint 8 --ctu 16 --bframes 5 --aq 1 --cutree 1 --rdoq-level 2 --threads 1"
# --cbqpoffs / --crqpoffs (phase 13 B1): the PPS chroma QP offsets, which
# no stream on this list carried before - every one of them wrote
# pps_cb_qp_offset 0 and pps_cr_qp_offset 0, so the two se(v) were never
# read as anything but zero by either decoder. The cell takes the two in
# OPPOSITE directions on purpose: the planes then run at different QPs
# for the first time, which is exactly what the coding path was split to
# allow, and a decoder that reads one offset for both planes - or reads
# the offset AFTER Table 8-10 instead of into qPi before it - cannot
# match the recon. Deblocking is in the claim too: 8.7.2.5.5 indexes the
# chroma tC table with cQpPicOffset added, so the offsets move the filter
# as well as the quantizer. One thread and twelve, so the pair carries
# the determinism leg as well.
run_vector cbcrqpoffs_t12 352x288 24 testsrc2 "--keyint 12 --bframes 7 --crf 26 --aq 1 --cutree 1 --cbqpoffs -2 --crqpoffs 2 --threads 12"
run_vector cbcrqpoffs_t1  176x144 16 testsrc2 "--keyint 8 --ctu 16 --bframes 5 --aq 1 --cutree 1 --cbqpoffs -6 --crqpoffs -3 --threads 1"

# --tu-inter-depth / --tu-intra-depth (phase 14 item 5): the residual
# quadtree's depth as a user knob. The default 2 is every stream this
# project wrote before the flag existed and is covered by every vector
# above; what needs its own is the RECURSION - depth 3 and 4 make a node
# that split split again, which is the arrangement replay_tt had never
# been asked to walk, and depth 1 is the other end, where a 2Nx2N CU
# codes no split flag at all and a PARTITIONED inter CU infers one
# (7.4.9.8's interSplitFlag). Both decoders are held bit-exact as always,
# and the ctu-64 vector is the one that reaches trafoDepth 4.
run_vector tudepth3_t1   352x288 24 testsrc2 "--keyint 12 --bframes 7 --crf 26 --aq 1 --cutree 1 --tu-inter-depth 3 --tu-intra-depth 3 --threads 1"
run_vector tudepth3_t12  352x288 24 testsrc2 "--keyint 12 --bframes 7 --crf 26 --aq 1 --cutree 1 --tu-inter-depth 3 --tu-intra-depth 3 --threads 12"
run_vector tudepth4_c64  352x288 20 testsrc2 "--keyint 10 --ctu 64 --bframes 7 --qp 30 --tu-inter-depth 4 --tu-intra-depth 4 --threads 4"
run_vector tudepth1      352x288 20 testsrc2 "--keyint 10 --ctu 64 --bframes 5 --aq 1 --cutree 1 --tu-inter-depth 1 --tu-intra-depth 1 --threads 4"
run_vector tudepth_mixed 176x144 16 testsrc2 "--keyint 8 --ctu 16 --bframes 5 --aq 1 --cutree 1 --tu-inter-depth 3 --tu-intra-depth 1 --threads 1"

# The two rungs phase 14 item 5 built and REFUSED, spelled as the flags
# they are rather than by name: `--preset veryslow` and `--preset placebo`
# are not placed, so the name is refused, and the composition is still
# worth a decoded stream. Each is `slower`'s table and structure with the
# transform tree one and two levels deeper, on the 64 coding tree those
# presets run - which is the arrangement the shorter vectors above do not
# reach.
run_vector rung_veryslow 352x288 24 testsrc2 "--preset slower --tu-inter-depth 3 --tu-intra-depth 3 --keyint 12 --crf 26 --aq 1 --cutree 1 --threads 4"
run_vector rung_placebo  352x288 24 testsrc2 "--preset slower --tu-inter-depth 4 --tu-intra-depth 4 --keyint 12 --crf 26 --aq 1 --cutree 1 --threads 4"
run_vector ref4_b8_t1    352x288 30 testsrc2 "--keyint 12 --bframes 8 --rc-lookahead 20 --ref 4 --crf 26 --aq 1 --cutree 1 --threads 1"
run_vector ref5_b8_t12   352x288 30 testsrc2 "--keyint 12 --bframes 8 --rc-lookahead 20 --ref 5 --crf 26 --aq 1 --cutree 1 --threads 12"
# The widest pairs are bounded by Annex A and not by us: sps_max_dec_pic_
# buffering stops at 16, so `--ref 4 --bframes 15` (17) and `--ref 5
# --bframes 11` and up (17, 18) are refused at encoder_open with the
# message that says which. `--ref 4 --bframes 12` is legal (16) and is the
# vector that walks the deepest pool - twelve marked pictures at once.
run_vector ref4_b12_t1   352x288 34 testsrc2 "--keyint 20 --bframes 12 --rc-lookahead 24 --ref 4 --b-adapt 0 --crf 26 --aq 1 --cutree 1 --threads 1"

# N265_DUP_FAST (phase 11 item 1): the fast path codes a whole picture as
# skip toward the reference whose SOURCE is this picture's source, with
# no arm running that could have chosen otherwise. Those CUs are syntax
# in an arrangement nothing else on the list produces - a merge index
# picked by a rule rather than by RD, on a picture whose quadtree never
# splits and which codes no TU at all - and the claim is that both
# decoders reconstruct the reference exactly. The source is the list's
# own dithered testsrc2 decimated to half rate and repeated, which is
# animation on twos: the noise comes FIRST in the chain, so what repeats
# is a frame that already carries residual, and every other frame is a
# byte-for-byte duplicate of the one before it.
#
# Three cells. The pyramid at one thread and at twelve is the shape the
# arm was measured in and carries the determinism leg with it; it is also
# where a duplicate that is itself a REFERENCE has to stay a full picture
# in the DPB. The third walks CTB 16, the threshold form, and a picture
# that is not a multiple of the CTB size, where the fast path commits at
# a child node rather than at the CTB root.
run_vector_dup "N265_DUP_FAST=1" \
           dup_b7_t1      352x288 24 testsrc2 "--keyint 12 --bframes 7 --aq 1 --cutree 1 --threads 1"
run_vector_dup "N265_DUP_FAST=1" \
           dup_b7_t12     352x288 24 testsrc2 "--keyint 12 --bframes 7 --crf 26 --aq 1 --cutree 1 --threads 12"
run_vector_dup "N265_DUP_FAST=1 N265_DUP_THR=0.5" \
           dup_thr_ctu16  210x122 16 testsrc2 "--keyint 8 --ctu 16 --bframes 5 --aq 1 --cutree 1 --threads 4"

# --film-grain (phase 12 item 3). Two vectors in the FAST list, because the
# SEI is the one thing this item puts in the bitstream and the claim about
# it is exactly a conformance claim: both decoders parse the message and
# BOTH reconstruct the coded picture bit-exactly - ffmpeg because it was
# asked to hand the parameters out rather than synthesize, dec265 because
# it ignores the message. A decoder that mis-parsed the message would fall
# off the end of the SEI and take the rest of the access unit with it.
#
# The two shapes are the two that can break differently: several key
# frames (so the message is REPEATED, once per IRAP, which is what the
# persistence flag's scope requires) and the denoise arm (so the picture
# the message describes is not the source). The noisy source every vector
# already carries is what gives the estimator something to find.
# --hash (phase 14 item 1): one suffix SEI per picture, which is a NAL
# type nothing else in this suite writes and a message whose value the
# decoders can check. The pyramid is what makes it worth running: the
# messages are written in CODING order and the pictures are output in
# DISPLAY order, so a hash taken from the wrong frame passes a count and
# fails a comparison. run_vector's hash leg is what compares them.
run_vector hash_md5      352x288 24 testsrc2 "--keyint 12 --bframes 7 --crf 26 --aq 1 --cutree 1 --hash 1 --threads 4"
# ...and the VUI color block without it, on a picture that also has a
# CONFORMANCE window, so the two windows are in the same SPS and a
# decoder has to keep them apart: the conformance window crops 210x122 up
# to 216x128 and the default display window asks for six more pixels of
# the coded picture to be hidden. Everything here is signaling, so the
# claim is that both decoders still reconstruct the same samples.
run_vector vui_colour    210x122 12 testsrc2 "--keyint 6 --bframes 3 --qp 30 --colorprim bt709 --transfer bt709 --colormatrix bt709 --range full --videoformat pal --chromaloc 0 --overscan crop --sar 16:9 --display-window 2,2,4,4 --threads 4"
# Weighted prediction (phase 14 item 2), on the clip the tool exists for.
# fade_cif is foreman with a 40-frame fade to black and a 40-frame fade
# back (scripts/fetch_corpus.sh), so the estimator FIRES here - on a clip
# with no illumination ramp it declines and the vector would prove only
# that the syntax parses.
#
# Two vectors because the two PPS flags put the table in DIFFERENT slice
# types and reach different clauses of 8.5.3.3.4.3: `wp_fade` is
# weighted_pred_flag alone, which is x265's default and ours, so every
# weighted block is a UNI prediction in a P slice; `wp_fadeb` adds
# weighted_bipred_flag, which is where the BI form runs and where a
# decoder that reads the two lists' weights the wrong way round finally
# disagrees with us. Both decoders reconstruct explicit WP, so both legs
# of run_vector are real oracles here.
#
# The frame range starts at 0 and covers 32 frames, which at --keyint 12
# puts an IRAP inside the fade-out: the reference a weighted P points at
# is then a picture of a different brightness, which is the case the
# whole estimator is about.
run_vector_file tests/corpus/fade_cif.y4m \
           wp_fade  352x288 32 testsrc2 "--keyint 12 --bframes 7 --crf 28 --aq 1 --cutree 1 --weightp --threads 4"
run_vector_file tests/corpus/fade_cif.y4m \
           wp_fadeb 352x288 32 testsrc2 "--keyint 8 --bframes 7 --ref 3 --crf 30 --aq 1 --cutree 1 --weightp --weightb --threads 12"
# --open-gop (phase 14 item 3): the CRA, its leading pictures and the
# seek that is the whole point of them. Four vectors in the FAST list,
# because run_vector's seek leg is the only check in this suite that
# starts a decode anywhere but the beginning.
#
# The shapes are the ones that break differently. `--ref 3` FIRST,
# because that is where the prediction barrier bites at all: at --ref 1
# a trailing picture's nearest past reference is the CRA whatever else
# is marked, so a barrier bug is invisible there and the vector would
# pass an encoder that has none.
run_vector opengop_ref3  352x288 32 testsrc2 "--keyint 8 --bframes 7 --ref 3 --crf 26 --aq 1 --cutree 1 --open-gop --threads 4"
# the deep pyramid at one thread and at twelve: the leading pictures are
# the mini-GOP's own B tree, so the RASL/RADL typing and the retirement
# that keeps their references alive both ride the dispatch order.
run_vector opengop_t1    352x288 26 testsrc2 "--keyint 24 --bframes 7 --ref 2 --crf 26 --aq 1 --cutree 1 --open-gop --threads 1"
run_vector opengop_t12   352x288 26 testsrc2 "--keyint 24 --bframes 7 --ref 2 --crf 26 --aq 1 --cutree 1 --open-gop --threads 12"
# --radl 2: two of the leading pictures may not reference across the CRA,
# so a decoder seeking there outputs TWO MORE pictures than the CRA - in
# front of it, in output order. A RADL that took a reference it may not
# have decodes to garbage at the seek and to the right samples from the
# start, which is exactly the bug only this leg can see.
run_vector opengop_radl  352x288 26 testsrc2 "--keyint 8 --bframes 7 --ref 2 --crf 26 --aq 1 --cutree 1 --open-gop --radl 2 --threads 4"
# open GOP and item 4's tooling together, which is the combination that
# makes an ELEMENTARY stream seekable: --repeat-headers now puts the
# parameter sets in front of every CRA and not only every IDR, so the
# seek leg stops having to prepend them. --aud moves the access unit
# boundary a NAL earlier and --temporal-layers puts the RASL leaves at
# TemporalId 1, both of which the cut has to get right.
run_vector opengop_tools 352x288 32 testsrc2 "--keyint 8 --bframes 7 --ref 3 --crf 26 --aq 1 --cutree 1 --open-gop --repeat-headers --aud --eos --temporal-layers --idr-recovery-sei --hash 1 --threads 4"
# --intra-refresh: recon match from the start, and a cold join at the
# second sweep start (run_vector's recovery leg). Natural content, so the
# vectors reach the refreshed edge; one thread and twelve, three
# references, a 64x64 tree, and the zerolatency pairing under a buffer.
run_vector_file tests/corpus/foreman_cif.y4m \
           ir_foreman 352x288 40 testsrc2 "--intra-refresh --keyint 10 --crf 28 --threads 1"
run_vector_file tests/corpus/mobile_cif.y4m \
           ir_mobile 352x288 40 testsrc2 "--intra-refresh --keyint 12 --ref 3 --crf 26 --threads 12"
run_vector_file tests/corpus/park_joy_720p.y4m \
           ir_park 1280x720 24 testsrc2 "--intra-refresh --keyint 8 --ctu 64 --crf 30 --threads 12"
run_vector_file tests/corpus/stefan_cif.y4m \
           ir_zerolat 352x288 40 testsrc2 "--intra-refresh --tune zerolatency --keyint 10 --bitrate 400 --vbv-maxrate 400 --vbv-bufsize 400 --threads 4"
run_vector fgc_sei       352x288 20 testsrc2 "--keyint 6 --bframes 3 --aq 1 --cutree 1 --film-grain 1.0 --threads 4"
run_vector fgc_denoise   352x288 20 testsrc2 "--keyint 6 --bframes 3 --aq 1 --cutree 1 --film-grain 1.0 --film-grain-denoise 1 --threads 12"
# --nr-intra / --nr-inter (phase 14 item 6): the coefficient-domain noise
# reduction. It writes no syntax a decoder has to learn - it changes the
# LEVELS - so what this vector actually claims is that the encoder's own
# reconstruction still matches a decoder's after the attenuation, which
# is the one way a quantizer-side filter breaks: an encoder that
# attenuates on the analysis path and not on the commit path reconstructs
# from coefficients it did not code. Run at twelve threads because the
# tally is row-private and summed on the driver, and at --keyint 6 so the
# intra half and the inter half both carry offsets by the end.
run_vector nr_both       352x288 24 testsrc2 "--keyint 6 --bframes 3 --qp 24 --aq 1 --cutree 1 --nr-intra 1000 --nr-inter 1000 --threads 12"
run_vector nr_inter      352x288 20 testsrc2 "--keyint 6 --bframes 3 --crf 26 --aq 1 --cutree 1 --nr-inter 400 --rdoq-level 0 --threads 4"
# --mcstf (phase 14 item 6): the source filter. It writes no syntax at
# all - it changes the PICTURE - so the claim is the one thing a queue in
# front of the lookahead can break: that the encode still has exactly as
# many pictures in it as the caller handed over, in display order, with
# the last --mcstf-ref-range of them coming out of the flush. 23 frames
# is deliberately not a multiple of the filter's own 8-picture cadence or
# of the mini-GOP, so the tail is ragged on both.
run_vector mcstf_r2      352x288 23 testsrc2 "--keyint 8 --bframes 3 --qp 28 --aq 1 --cutree 1 --mcstf --threads 4"
run_vector mcstf_r4sel   352x288 23 testsrc2 "--keyint 8 --bframes 7 --crf 30 --aq 1 --cutree 1 --mcstf --mcstf-ref-range 4 --selective-mcstf --threads 12"
# The 10-to-8 downshift and --dither (phase 14 item 6). The stream is an
# ordinary Main profile one - a decoder learns nothing new - so what
# these two claim is the READER: that a 10-bit file reduced in the read
# loop produces a picture the encoder reconstructs and both decoders
# agree with, at both reductions. The dithered arm is the one that would
# expose an off-by-one in the clamp, since 1023 + 3 overflows the byte.
run_vector_10in down8       352x288 16 testsrc2 "--keyint 8 --bframes 3 --qp 28 --aq 1 --cutree 1 --output-depth 8 --threads 4"
run_vector_10in down8_dith  352x288 16 testsrc2 "--keyint 8 --bframes 3 --qp 28 --aq 1 --cutree 1 --output-depth 8 --dither --threads 4"
# The NAL-level switches (phase 14 item 4). Every one of them puts a NAL
# in the stream that nothing else here writes - an access unit delimiter
# (35), an end of sequence (36), an end of bitstream (37), a repeated
# parameter set at a mid-stream IRAP, and a user data unregistered SEI -
# and the claim is exactly a conformance claim: a decoder that stumbles
# on one of them loses the access unit it sits in. --keyint 6 over 24
# frames is what makes --repeat-headers and --eos do their work more than
# once, which is where a "once at the start" bug would hide.
run_vector nal_opts      352x288 24 testsrc2 "--keyint 6 --bframes 3 --qp 30 --repeat-headers --aud --eos --eob --info --threads 4"
# The rate-control exposure (phase 14 item 4). Two of its nine put
# something in the BITSTREAM and so are conformance claims rather than
# arithmetic: --strict-cbr writes filler data NALs (38) and sets
# cbr_flag in the HRD, which is a NAL type and a schedule flag nothing
# else in this suite produces; --qpfile moves a key frame off the keyint
# grid, which changes the POC line and the reference lists around it.
# --zones rides with the first because it is the other way a quantizer
# gets pinned from outside the loop, and a pinned QP that the AQ frame
# mean then moved would decode fine and be wrong.
printf '0 I 26\n7 I\n13 I 30\n' > "$TMP/rc_qpfile.txt"
run_vector rc_qpfile     352x288 20 testsrc2 "--keyint 100 --bframes 3 --crf 28 --qpfile $TMP/rc_qpfile.txt --threads 4"
run_vector rc_cbr        352x288 24 testsrc2 "--keyint 8 --bframes 3 --bitrate 700 --vbv-maxrate 700 --vbv-bufsize 700 --strict-cbr --max-ausize-factor 0.6 --qpmin 20 --qpmax 44 --zones 0,7,q=32 --threads 4"
# The cheap SEIs and the temporal sub-layer (phase 14 item 4). Four of the
# five are signaling - a recovery point at every IRAP, three new fields
# in pic_timing, one flag in the first buffering period - and the fifth is
# not: --temporal-layers changes a VCL NAL HEADER and adds the sub-layer
# tails to profile_tier_level and hrd_parameters, which are the two places
# in this stream where getting the loop bound wrong shifts every bit after
# it. The buffer is on because pic_struct and the concatenation flag live
# in messages the HRD carries, so one vector reaches all five.
#
# The pyramid is what makes the temporal id worth checking: only the
# NON-REFERENCE leaves go to sub-layer 1, and a decoder that saw a layer-0
# picture predicting from a layer-1 one would be reading a stream that is
# not conforming even though every sample in it is right.
run_vector sei_layers    352x288 24 testsrc2 "--keyint 8 --bframes 3 --crf 28 --vbv-maxrate 900 --vbv-bufsize 900 --idr-recovery-sei --pic-struct 0 --hrd-concat --temporal-layers --threads 4"
run_vector tlayers_only  352x288 20 testsrc2 "--keyint 10 --bframes 7 --qp 32 --temporal-layers --threads 12"
# The WORST access unit this encoder can write, and it is a vector
# because the failure mode is silent: pack_nal drops a NAL past the
# deferred ring's per-frame bound and says nothing, so an IRAP one NAL
# too long loses its SLICE and the stream decodes as "could not find ref
# with POC 0". That bound has now been found short twice, at phase 14
# items 1 and 4, and no gate that runs one flag at a time can see it.
# Every NAL-producing flag at once: EOS, AUD, VPS, SPS, PPS, the recovery
# point, the HRD timing pair, the HDR metadata, the version SEI, the
# grain SEI, the slice, the hash suffix SEI, filler, EOB.
# Phase 14 item 15 adds --video-signal-type-preset with a color volume,
# which makes the HDR10 message in that access unit its longest form -
# mastering display AND content light level - and --dup-threshold, which
# arms the near-duplicate path. --tune is NOT here: this vector names
# --bframes 3, and zerolatency's window of 1 is refused against it by
# design. It has a vector of its own below.
run_vector all_nals      352x288 16 testsrc2 "--keyint 8 --bframes 3 --bitrate 700 --vbv-maxrate 700 --vbv-bufsize 700 --strict-cbr --repeat-headers --aud --eos --eob --info --idr-recovery-sei --pic-struct 0 --hrd-concat --temporal-layers --hash 1 --film-grain 1.0 --max-cll 1000,400 --video-signal-type-preset BT2100_PQ_YCC:P3D65x1000n0005 --dup-threshold 0.5 --threads 4"

# Per-shot stage A1: a key frame on each shot. The source is four CIF
# clips cut together (10, 10, 6 and 8 frames), so it has three hard cuts.
# shots_scan codes under the list --shot-scan makes of it. shots_hand
# codes under a written list that also puts a shot two frames in, inside
# min-keyint, and one in the middle of a clip, with --keyint 12 under open
# GOP so CRAs fall between the listed IDRs. shots_keys is the no-file
# form at --ref 2, where a cut inside min-keyint would otherwise be a
# non-IDR I. run_vector's seek leg checks every IRAP and every listed
# start with both decoders.
ms_src="$TMP/shots_src.y4m"
if "$FFMPEG" -loglevel error -i tests/corpus/foreman_cif.y4m \
        -i tests/corpus/mobile_cif.y4m -i tests/corpus/akiyo_cif.y4m \
        -i tests/corpus/bus_cif.y4m -filter_complex \
        "[0:v]trim=end_frame=10,setpts=N/25/TB[a];[1:v]trim=end_frame=10,setpts=N/25/TB[b];[2:v]trim=end_frame=6,setpts=N/25/TB[c];[3:v]trim=end_frame=8,setpts=N/25/TB[d];[a][b][c][d]concat=n=4:v=1:a=0,setpts=N/25/TB,fps=25" \
        -y "$ms_src" 2>/dev/null; then
    run_vector_scan "$TMP/shots_scan.txt" "$ms_src" \
           shots_scan 352x288 34 testsrc2 "--keyint 100 --bframes 7 --crf 28 --aq 1 --cutree 1 --shot-list $TMP/shots_scan.txt --threads 4"
    printf 'yah265-shots 1\nshots 6\n0\n2\n10\n15 # mid-clip\n20\n26\nend\n' \
        > "$TMP/shots_hand.txt"
    run_vector_file "$ms_src" \
           shots_hand 352x288 34 testsrc2 "--keyint 12 --bframes 7 --ref 2 --crf 28 --aq 1 --cutree 1 --shot-list $TMP/shots_hand.txt --threads 12"
    run_vector_file "$ms_src" \
           shots_keys 352x288 34 testsrc2 "--keyint 100 --bframes 7 --ref 2 --crf 28 --aq 1 --cutree 1 --shot-keys --threads 4"
else
    echo "SUMMARY shots_scan SKIP (no tests/corpus)"
fi

# -t/--tune zerolatency (phase 14 item 15): no B frames, a window of one,
# no adaptive run length, no cu-tree and one frame in flight. Every one of
# those is a knob with its own coverage; what this vector checks is that
# the COMPOSITION decodes, because a tune is the one way a user reaches
# all five at once without naming any of them.
run_vector tune_zl       352x288 16 testsrc2 "--keyint 8 --tune zerolatency --threads 4"

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
