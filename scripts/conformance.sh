#!/bin/sh
# Conformance gate: every encode must reconstruct bit-exact through BOTH
# independent decoders (ffmpeg hevc + dec265), and the encoder must be
# byte-identical across runs. Phase 0 note: everything is all-intra IDR so
# decode order == display order; when B-frames land, the display-order leg
# must compare through ffmpeg only (dec265 -o emits decode order).
#
# usage: scripts/conformance.sh [--fast]
# env: N265=path/to/n265  DEC265=path/to/dec265  FFMPEG=path/to/ffmpeg

set -u

N265=${N265:-build/n265}
FFMPEG=${FFMPEG:-ffmpeg}
DEC265=${DEC265:-}
FAST=0
[ "${1:-}" = "--fast" ] && FAST=1

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
[ -x "$N265" ] || { echo "conformance: $N265 not built" >&2; exit 1; }

TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT

pass=0
fail=0

# env assignments prefixed onto the encoder for the next run_vector call;
# set by run_vector_env, cleared after. Deliberately unquoted where it is
# expanded so the assignments split into separate words.
VENV=""

run_vector() {
    # $1 name  $2 WxH  $3 frames  $4 lavfi source  $5 encoder args
    # ("--pcm" makes the reference the source itself; anything else makes
    # it the encoder's own reconstruction via --dump-recon)
    name=$1; size=$2; frames=$3; src=$4; eargs=$5
    y4m="$TMP/$name.y4m"
    "$FFMPEG" -loglevel error -f lavfi -i "$src=size=$size:rate=25" \
        -vf "noise=alls=12:allf=t+u" -frames:v "$frames" -pix_fmt yuv420p "$y4m" -y \
        2>/dev/null
    if [ ! -s "$y4m" ]; then
        echo "SUMMARY $name SKIP (source gen failed)"
        return
    fi

    # shellcheck disable=SC2086
    env $VENV "$N265" "$y4m" -o "$TMP/$name.hevc" $eargs \
        --dump-recon "$TMP/$name.recon.yuv" 2>/dev/null || {
        echo "SUMMARY $name FAIL (encode)"; fail=$((fail+1)); return; }
    # shellcheck disable=SC2086
    env $VENV "$N265" "$y4m" -o "$TMP/$name.2.hevc" $eargs 2>/dev/null
    cmp -s "$TMP/$name.hevc" "$TMP/$name.2.hevc" || {
        echo "SUMMARY $name FAIL (determinism: two runs differ)"
        fail=$((fail+1)); return; }

    case " $eargs " in
    *" --pcm "*)
        "$FFMPEG" -loglevel error -i "$y4m" -f rawvideo "$TMP/$name.src.yuv" -y
        ;;
    *)
        cp "$TMP/$name.recon.yuv" "$TMP/$name.src.yuv"
        ;;
    esac
    "$FFMPEG" -loglevel error -i "$TMP/$name.hevc" -f rawvideo \
        "$TMP/$name.ff.yuv" -y 2>"$TMP/$name.fferr"
    # ffmpeg and dec265 disagree on bounded chroma seam samples for
    # CTB-16 SAO streams (reproduced on x265 output too; recorded in
    # docs/instruments.md). dec265 stays bit-exact below; ffmpeg gets a
    # small labeled chroma allowance only on ctu-16 vectors.
    case "$eargs" in
    *"--ctu 16"*)
        if ! python3 scripts/yuvdiff.py "$TMP/$name.src.yuv" \
                "$TMP/$name.ff.yuv" --geometry "$size" \
                --max-chroma-pct 1.0 --max-delta 16; then
            echo "SUMMARY $name FAIL (ffmpeg recon mismatch)"
            fail=$((fail+1)); return
        fi
        ;;
    *)
        if ! cmp -s "$TMP/$name.src.yuv" "$TMP/$name.ff.yuv"; then
            echo "SUMMARY $name FAIL (ffmpeg recon mismatch)"
            sed -n 1,3p "$TMP/$name.fferr" >&2
            fail=$((fail+1)); return
        fi
        ;;
    esac
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
run_vector_env() {
    VENV=$1; shift
    run_vector "$@"
    VENV=""
}

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
# D4's observation lag. Unlike K this one IS bitstream-affecting, so it
# gets a cell at one thread and at twelve: the stream must differ from
# the D = 0 one and must not differ between thread counts.
run_vector_env "N265_RC_LAG=3" \
           lag3_t1_abr 352x288 24  testsrc2  "--keyint 12 --bitrate 400 --aq 1 --cutree 1 --threads 1"
run_vector_env "N265_RC_LAG=3" \
           lag3_t12_abr 352x288 24 testsrc2  "--keyint 12 --bitrate 400 --aq 1 --cutree 1 --threads 12"
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
    run_vector intra_cif   352x288 10 testsrc2  ""
    run_vector intra_720   1280x720 3 testsrc2  ""
    run_vector intra_bars  176x144 30 smptebars ""
    run_vector intra_qp0   64x64    2  testsrc2  "--qp 0"
    run_vector intra_qp51  176x144  3  testsrc2  "--qp 51"
fi

echo "conformance: $pass pass, $fail fail"
[ "$fail" -eq 0 ]
