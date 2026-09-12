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

# a full lavfi graph for the next run_vector, instead of the usual
# "<source>=size=WxH:rate=25". Set by run_vector_graph, cleared after.
VGRAPH=""
# Every vector's source is dithered so the encoder has residual to code.
# A vector for the SAO CONTENT GATE cannot be: the gate types a frame by
# how many 16x16 source blocks are flat to within one luma level, and
# alls=12 noise leaves none anywhere. VNOISE lets those vectors ask for
# the clean source instead.
VNOISE=""

run_vector() {
    # $1 name  $2 WxH  $3 frames  $4 lavfi source  $5 encoder args
    # ("--pcm" makes the reference the source itself; anything else makes
    # it the encoder's own reconstruction via --dump-recon)
    name=$1; size=$2; frames=$3; src=$4; eargs=$5
    y4m="$TMP/$name.y4m"
    "$FFMPEG" -loglevel error -f lavfi -i "${VGRAPH:-$src=size=$size:rate=25}" \
        -vf "${VNOISE:-noise=alls=12:allf=t+u}" -frames:v "$frames" -pix_fmt yuv420p "$y4m" -y \
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

# same vector with the dither turned off at the source
run_vector_clean() {
    VENV=$1; shift
    VNOISE="null"
    run_vector "$@"
    VNOISE=""; VENV=""
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
