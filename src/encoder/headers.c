/*
 * Parameter set writers: VPS, SPS, PPS (ITU-T H.265 7.3.2).
 * Copyright (c) 2026, the yah265 authors. GPL-2.0-or-later; see LICENSE.
 */

#include "encoder/encoder.h"
#include "encoder/sao.h"

/* profile_tier_level with maxNumSubLayersMinus1 == 0 (7.3.3).
 *
 * The level and the tier are SOLVED once, at encoder open, from the whole
 * of Annex A - MaxLumaPs and the per-dimension bound Sqrt(MaxLumaPs * 8),
 * MaxDpbSize (A.4.2) against the declared buffering, MaxLumaSr from the
 * source frame rate, and MaxBR by tier wherever the encoder knows the
 * rate. src/encoder/level.c holds the tables and the derivation; this
 * writer only spends the two numbers it produced.
 *
 * Until phase 8 item 7 the pick lived here and read the picture size and
 * the DPB alone - eight of the thirteen levels, no rate leg - which
 * signalled 720p50 at a level its own frame rate violates (Table A.7,
 * MaxLumaSr) and a PCM stream at a level admitting a tenth of its bit
 * rate. Both were wrong by a table, and both moved. */
static void write_ptl(bitstream_t *bs, int level_idc, int tier,
                      int chroma_idc)
{
    bs_put_bits(bs, 0, 2);          /* general_profile_space */
    bs_put_bits(bs, (uint32_t)(tier != 0), 1);  /* general_tier_flag */
    /* Main (1) at 8 bits, Main10 (2) at 10. A Main stream is also
     * Main10-decodable so it sets both compatibility flags; a Main10
     * stream is not Main-decodable and sets only its own.
     *
     * Item 2's review of the rest of the PTL for Main10, which ended in
     * no change, and says so here rather than leaving the reader to
     * re-derive it:
     *   - The four source/packing flags below are profile-independent
     *     (7.3.3). The 44 bits after them are general_reserved_zero_43bits
     *     plus the reserved bit: the profile-specific constraint flags
     *     (general_max_12bit_constraint_flag and the rest) exist only for
     *     profile_idc 4..10, so at Main and Main10 alike zeros are not a
     *     default, they are the syntax.
     *   - The level tables are profile-independent too. MaxLumaPs and the
     *     DPB rule (A.4.2) are what src/encoder/level.c reads, and Table
     *     A.2's CpbBrVclFactor / CpbBrNalFactor are 1000 / 1100 at Main
     *     AND at Main10, so the bit-rate leg item 7 added does not move
     *     with the depth either. What DOES move with the depth is the
     *     PCM payload, and level.c takes the sample width as an argument
     *     for exactly that.
     *   - The VUI is absent unless --vui-timing asks for it, and what it
     *     then carries is the frame rate, which has no depth field to
     *     keep consistent with bit_depth_luma_minus8.
     * Verified on the wire: dec265 reads profile_idc Main10 with
     * compatibility flag 2 alone, and ffprobe reads "Main 10" /
     * yuv420p10le. */
    /* Phase 8 items 6a and 6b: at 4:4:4 and at 4:2:2 alike the profile is
     * the RANGE EXTENSION one, general_profile_idc 4 (A.3.5), and the
     * profiles this encoder emits there - Main 4:4:4, Main 4:4:4 10, and
     * Main 4:2:2 10 at both depths - are told apart by the constraint
     * flags below and by nothing else. The compatibility word carries bit
     * 4 alone: none of them is decodable by a Main or Main10 decoder. */
    int rext = chroma_idc != 1;
    bs_put_bits(bs, rext ? 4 : (BIT_DEPTH == 8 ? 1 : 2), 5);
    bs_put_bits(bs, rext ? 0x08000000u
                         : (BIT_DEPTH == 8 ? 0x60000000u : 0x20000000u), 32);
    bs_put_bits(bs, 1, 1);          /* general_progressive_source_flag */
    bs_put_bits(bs, 0, 1);          /* general_interlaced_source_flag */
    bs_put_bits(bs, 1, 1);          /* general_non_packed_constraint_flag */
    bs_put_bits(bs, 1, 1);          /* general_frame_only_constraint_flag */
    if (rext) {
        /* The nine profile-specific constraint flags of A.3.5, which
         * EXIST only for general_profile_idc 4..10 - at Main and Main10
         * the same 44 bits are general_reserved_zero_43bits plus the
         * reserved bit, which is why the branch below is a different
         * SYNTAX and not a different value. Table A.2's rows for the two
         * profiles this item emits:
         *
         *   Main 4:4:4     12bit 1, 10bit 1, 8bit 1, 422 0, 420 0,
         *                  mono 0, intra 0, one-picture 0, lower-rate 1
         *   Main 4:4:4 10  the same with 8bit 0
         *   Main 4:2:2 10  12bit 1, 10bit 1, 8bit 0, 422 1, 420 0,
         *                  mono 0, intra 0, one-picture 0, lower-rate 1
         *
         * The three "max N bit" flags are upper bounds the stream
         * promises to stay inside, so an 8-bit 4:4:4 stream sets all
         * three and a Main 4:4:4 10 one drops the 8-bit flag; the two
         * chroma flags say which subsamplings the stream IS constrained
         * to, and setting 422 alone is the whole difference between the
         * 4:2:2 profile and the 4:4:4 ones.
         *
         * Table A.2 has NO 8-bit 4:2:2 row - the lowest 4:2:2 profile is
         * Main 4:2:2 10 - so an 8-bit 4:2:2 stream is signalled as that,
         * with max_8bit CLEAR. The flag is a constraint the stream
         * declines to make and not a description of its sample width,
         * and bit_depth 8 sits inside Main 4:2:2 10 already; x265 names
         * the same profile main422-10 at both depths for the same
         * reason.
         *
         * `lower_bit_rate` is the CpbBrVclFactor row this encoder's
         * streams sit in and is 1 for all of them. */
        bs_put_bits(bs, 1, 1);      /* general_max_12bit_constraint_flag */
        bs_put_bits(bs, 1, 1);      /* general_max_10bit_constraint_flag */
        bs_put_bits(bs, chroma_idc == 3 && BIT_DEPTH == 8, 1);
                                    /* general_max_8bit_constraint_flag */
        bs_put_bits(bs, chroma_idc == 2, 1); /* max_422chroma_constraint */
        bs_put_bits(bs, 0, 1);      /* general_max_420chroma_constraint */
        bs_put_bits(bs, 0, 1);      /* general_max_monochrome_constraint */
        bs_put_bits(bs, 0, 1);      /* general_intra_constraint_flag */
        bs_put_bits(bs, 0, 1);      /* general_one_picture_only_constraint */
        bs_put_bits(bs, 1, 1);      /* general_lower_bit_rate_constraint */
        bs_put_bits(bs, 0, 32);     /* general_reserved_zero_34bits... */
        bs_put_bits(bs, 0, 2);      /* ...cont */
        bs_put_bits(bs, 0, 1);      /* general_inbld_flag / reserved bit */
    } else {
        bs_put_bits(bs, 0, 32);     /* general_reserved_zero_44bits... */
        bs_put_bits(bs, 0, 12);     /* ...cont */
    }
    bs_put_bits(bs, (uint32_t)level_idc, 8);   /* general_level_idc */
}

void n265_write_vps(struct n265_frame_work *e)
{
    bitstream_t *bs = &e->rbsp;
    bs_reset(bs);
    bs_put_bits(bs, 0, 4);          /* vps_video_parameter_set_id */
    bs_put_bits(bs, 1, 1);          /* vps_base_layer_internal_flag */
    bs_put_bits(bs, 1, 1);          /* vps_base_layer_available_flag */
    bs_put_bits(bs, 0, 6);          /* vps_max_layers_minus1 */
    bs_put_bits(bs, 0, 3);          /* vps_max_sub_layers_minus1 */
    bs_put_bits(bs, 1, 1);          /* vps_temporal_id_nesting_flag */
    bs_put_bits(bs, 0xffff, 16);    /* vps_reserved_0xffff_16bits */
    write_ptl(bs, e->level_idc, e->level_tier, e->chroma_idc);
    bs_put_bits(bs, 0, 1);          /* vps_sub_layer_ordering_info_present */
    bs_put_ue(bs, (uint32_t)(e->dpb_buffering - 1)); /* vps_max_dec_pic_buffering_minus1 */
    bs_put_ue(bs, (uint32_t)e->dpb_reorder);         /* vps_max_num_reorder_pics[0] */
    bs_put_ue(bs, 0);               /* vps_max_latency_increase_plus1[0]:
                                     * 0 = no limit, so it cannot be
                                     * under-declared */
    bs_put_bits(bs, 0, 6);          /* vps_max_layer_id */
    bs_put_ue(bs, 0);               /* vps_num_layer_sets_minus1 */
    bs_put_bits(bs, 0, 1);          /* vps_timing_info_present_flag */
    bs_put_bits(bs, 0, 1);          /* vps_extension_flag */
    bs_rbsp_trailing(bs);
}

void n265_write_sps(struct n265_frame_work *e)
{
    bitstream_t *bs = &e->rbsp;
    int crop_r = e->pad_w - e->param.width;   /* luma samples */
    int crop_b = e->pad_h - e->param.height;

    bs_reset(bs);
    bs_put_bits(bs, 0, 4);          /* sps_video_parameter_set_id */
    bs_put_bits(bs, 0, 3);          /* sps_max_sub_layers_minus1 */
    bs_put_bits(bs, 1, 1);          /* sps_temporal_id_nesting_flag */
    write_ptl(bs, e->level_idc, e->level_tier, e->chroma_idc);
    bs_put_ue(bs, 0);               /* sps_seq_parameter_set_id */
    bs_put_ue(bs, (uint32_t)e->chroma_idc);   /* 1 = 420, 2 = 422, 3 = 444 */
    if (e->chroma_idc == 3) {
        /* 7.3.2.2: 4:4:4 alone carries this, and 0 is what makes
         * ChromaArrayType 3 rather than three separate monochrome
         * planes. Leaving it out shifts every field after it - the
         * picture size parsed as 87x0 and both decoders refused the
         * stream, which is how it was found. */
        bs_put_bits(bs, 0, 1);      /* separate_colour_plane_flag */
    }
    bs_put_ue(bs, (uint32_t)e->pad_w);
    bs_put_ue(bs, (uint32_t)e->pad_h);
    if (crop_r || crop_b) {
        /* The offsets are in CHROMA units (7.4.3.2: SubWidthC /
         * SubHeightC times the offset is the luma crop), so at 4:4:4 the
         * unit IS a luma sample and both shifts are zero, and at 4:2:2
         * the vertical one is - which is why the two axes read csx and
         * csy apart rather than one number. */
        bs_put_bits(bs, 1, 1);      /* conformance_window_flag */
        bs_put_ue(bs, 0);                                /* left */
        bs_put_ue(bs, (uint32_t)(crop_r >> e->csx));     /* right */
        bs_put_ue(bs, 0);                                /* top */
        bs_put_ue(bs, (uint32_t)(crop_b >> e->csy));     /* bottom */
    } else {
        bs_put_bits(bs, 0, 1);
    }
    bs_put_ue(bs, BIT_DEPTH - 8);   /* bit_depth_luma_minus8 */
    bs_put_ue(bs, BIT_DEPTH - 8);   /* bit_depth_chroma_minus8 */
    bs_put_ue(bs, 4);               /* log2_max_pic_order_cnt_lsb_minus4 */
    bs_put_bits(bs, 0, 1);          /* sps_sub_layer_ordering_info_present */
    bs_put_ue(bs, (uint32_t)(e->dpb_buffering - 1)); /* sps_max_dec_pic_buffering_minus1 */
    bs_put_ue(bs, (uint32_t)e->dpb_reorder);         /* sps_max_num_reorder_pics */
    bs_put_ue(bs, 0);               /* sps_max_latency_increase_plus1:
                                     * 0 = no limit */
    if (e->mode == N265_MODE_PCM) {
        bs_put_ue(bs, 1);           /* log2_min_luma_coding_block_size_minus3: 16 */
        bs_put_ue(bs, 0);           /* log2_diff_max_min: CTB 16 */
        bs_put_ue(bs, 0);           /* log2_min_luma_transform_block_size_minus2: 4 */
        bs_put_ue(bs, 2);           /* log2_diff max TB 16 */
    } else {
        bs_put_ue(bs, 0);           /* min CB 8 */
        bs_put_ue(bs, (uint32_t)(e->ctb_log2 - 3));
        bs_put_ue(bs, 0);           /* min TB 4 */
        /* max TB = min(CTB, 32) */
        bs_put_ue(bs, (uint32_t)((e->ctb_log2 > 5 ? 5 : e->ctb_log2) - 2));
    }
    /* max_transform_hierarchy_depth_inter 1: interSplitFlag (7.4.9.8)
     * fires only at depth 0 for a non-2Nx2N inter CU, so a partitioned
     * CU still CODES split_transform_flag rather than inferring it */
    bs_put_ue(bs, e->mode == N265_MODE_PCM ? 0 : 1);    /* tu depth inter */
    bs_put_ue(bs, e->mode == N265_MODE_PCM ? 0 : 1);    /* tu depth intra */
    bs_put_bits(bs, 0, 1);          /* scaling_list_enabled_flag */
    /* amp_enabled_flag: also a binarization switch - with it set,
     * part_mode codes 2NxN/Nx2N in three bins instead of two, so it must
     * track exactly what ctu.c/entropy.c assume */
    bs_put_bits(bs, e->amp != 0, 1);
    /* sample_adaptive_offset_enabled_flag. Off under --lossless, and not
     * because 8.7.3 forbids it: SAO's own bypass condition would leave
     * every sample of every CU untouched here, so the filter can only
     * spend sao() syntax to do nothing. Off at the SEQUENCE level is the
     * cheapest way to say that, and it keeps the slice flags out too. */
    bs_put_bits(bs, e->mode != N265_MODE_PCM && !e->lossless &&
                    n265_sao_seq_enabled(), 1);
    if (e->mode == N265_MODE_PCM) {
        bs_put_bits(bs, 1, 1);      /* pcm_enabled_flag */
        bs_put_bits(bs, BIT_DEPTH - 1, 4);  /* pcm_sample_bit_depth_luma_minus1 */
        bs_put_bits(bs, BIT_DEPTH - 1, 4);  /* pcm_sample_bit_depth_chroma_minus1 */
        bs_put_ue(bs, 1);           /* log2_min_pcm_luma_coding_block_size_minus3: 16 */
        bs_put_ue(bs, 0);           /* log2_diff_max_min_pcm_luma_coding_block_size */
        bs_put_bits(bs, 1, 1);      /* pcm_loop_filter_disabled_flag */
    } else {
        bs_put_bits(bs, 0, 1);      /* pcm_enabled_flag */
    }
    bs_put_ue(bs, 0);               /* num_short_term_ref_pic_sets */
    bs_put_bits(bs, 0, 1);          /* long_term_ref_pics_present_flag */
    bs_put_bits(bs, e->mode != N265_MODE_PCM, 1);   /* sps_temporal_mvp */
    bs_put_bits(bs, e->mode != N265_MODE_PCM, 1);   /* strong smoothing */
    /* vui_parameters_present_flag. Present by default since 2026-09-12
     * (the owner's call, after item 7 made the level depend on the frame
     * rate); --no-vui-timing omits it, which is what every stream measured
     * before that date carried.
     *
     * What it buys is the one thing a decoder cannot otherwise check
     * about the level: since item 7 the level is derived partly from the
     * FRAME RATE (Table A.7's MaxLumaSr), and without vui_timing_info a
     * conforming decoder has no frame rate in the stream to check it
     * against. What it costs is about a dozen bytes in every SPS (the
     * 76 bits of syntax, plus the emulation prevention a 32-bit
     * num_units_in_tick of 1 forces), which is why item 7 shipped it off. The owner took that cost on
     * 2026-09-12; the recon and every VCL NAL are unchanged, which is the
     * identity check from then on. */
    if (!e->vui_timing) {
        bs_put_bits(bs, 0, 1);      /* vui_parameters_present_flag */
    } else {
        bs_put_bits(bs, 1, 1);      /* vui_parameters_present_flag */
        /* vui_parameters (E.2.1), in order. Everything before the timing
         * block is absent; field_seq_flag 0 and frame_field_info 0 say
         * the same thing the PTL's progressive/frame-only constraint
         * flags do. */
        bs_put_bits(bs, 0, 1);      /* aspect_ratio_info_present_flag */
        bs_put_bits(bs, 0, 1);      /* overscan_info_present_flag */
        bs_put_bits(bs, 0, 1);      /* video_signal_type_present_flag */
        bs_put_bits(bs, 0, 1);      /* chroma_loc_info_present_flag */
        bs_put_bits(bs, 0, 1);      /* neutral_chroma_indication_flag */
        bs_put_bits(bs, 0, 1);      /* field_seq_flag */
        bs_put_bits(bs, 0, 1);      /* frame_field_info_present_flag */
        bs_put_bits(bs, 0, 1);      /* default_display_window_flag */
        bs_put_bits(bs, 1, 1);      /* vui_timing_info_present_flag */
        /* The clock a picture is one tick of: num_units_in_tick /
         * time_scale is the frame period, so the y4m's F<num>:<den>
         * lands the other way round. */
        bs_put_bits(bs, (uint32_t)e->param.fps_den, 32); /* vui_num_units_in_tick */
        bs_put_bits(bs, (uint32_t)e->param.fps_num, 32); /* vui_time_scale */
        bs_put_bits(bs, 0, 1);      /* vui_poc_proportional_to_timing_flag */
        bs_put_bits(bs, 0, 1);      /* vui_hrd_parameters_present_flag: no
                                     * HRD, so no CPB schedule is claimed */
        bs_put_bits(bs, 0, 1);      /* bitstream_restriction_flag */
    }
    bs_put_bits(bs, 0, 1);          /* sps_extension_present_flag */
    bs_rbsp_trailing(bs);
}

void n265_write_pps(struct n265_frame_work *e)
{
    bitstream_t *bs = &e->rbsp;
    bs_reset(bs);
    bs_put_ue(bs, 0);               /* pps_pic_parameter_set_id */
    bs_put_ue(bs, 0);               /* pps_seq_parameter_set_id */
    bs_put_bits(bs, 0, 1);          /* dependent_slice_segments_enabled_flag */
    bs_put_bits(bs, 0, 1);          /* output_flag_present_flag */
    bs_put_bits(bs, 0, 3);          /* num_extra_slice_header_bits */
    /* sign_data_hiding_enabled_flag. 7.4.9.11 makes signHidden depend on
     * !cu_transquant_bypass_flag, so under --lossless - where every CU is
     * bypass - the tool is already inert by the spec's own condition.
     * Signalling it off says the same thing in one place instead of
     * leaving the reader to re-derive it per CU, and it is what the
     * residual writer's `sbh` argument is passed. */
    bs_put_bits(bs, e->mode != N265_MODE_PCM && !e->lossless, 1);
    bs_put_bits(bs, 0, 1);          /* cabac_init_present_flag */
    bs_put_ue(bs, 0);               /* num_ref_idx_l0_default_active_minus1 */
    bs_put_ue(bs, 0);               /* num_ref_idx_l1_default_active_minus1 */
    bs_put_se(bs, 0);               /* init_qp_minus26 */
    bs_put_bits(bs, 0, 1);          /* constrained_intra_pred_flag */
    bs_put_bits(bs, 0, 1);          /* transform_skip_enabled_flag */
    if (e->qp_delta_on) {
        bs_put_bits(bs, 1, 1);      /* cu_qp_delta_enabled_flag */
        bs_put_ue(bs, 0);           /* diff_cu_qp_depth: QG = CTB */
    } else {
        bs_put_bits(bs, 0, 1);      /* cu_qp_delta_enabled_flag */
    }
    bs_put_se(bs, 0);               /* pps_cb_qp_offset */
    bs_put_se(bs, 0);               /* pps_cr_qp_offset */
    bs_put_bits(bs, 0, 1);          /* pps_slice_chroma_qp_offsets_present */
    bs_put_bits(bs, 0, 1);          /* weighted_pred_flag */
    bs_put_bits(bs, 0, 1);          /* weighted_bipred_flag */
    bs_put_bits(bs, e->lossless != 0, 1); /* transquant_bypass_enabled_flag */
    bs_put_bits(bs, 0, 1);          /* tiles_enabled_flag */
    bs_put_bits(bs, 0, 1);          /* entropy_coding_sync_enabled_flag */
    bs_put_bits(bs, 1, 1);          /* pps_loop_filter_across_slices_enabled */
    if (e->mode == N265_MODE_PCM || e->lossless) {
        /* --lossless: 8.7.2's edge filter is skipped wherever
         * cu_transquant_bypass_flag is 1 on either side of the edge, and
         * here that is every edge in the picture. Disabling it in the PPS
         * is the same output with no per-sample condition to get wrong,
         * and it is what pcm_loop_filter_disabled_flag does for PCM. */
        bs_put_bits(bs, 1, 1);      /* deblocking_filter_control_present_flag */
        bs_put_bits(bs, 0, 1);      /* deblocking_filter_override_enabled */
        bs_put_bits(bs, 1, 1);      /* pps_deblocking_filter_disabled_flag */
    } else if (e->dbf_beta || e->dbf_tc) {
        /* non-zero deblock offsets: the control block carries them and
         * both sides derive beta and tC from the same shifted index */
        bs_put_bits(bs, 1, 1);      /* deblocking_filter_control_present_flag */
        bs_put_bits(bs, 0, 1);      /* deblocking_filter_override_enabled */
        bs_put_bits(bs, 0, 1);      /* pps_deblocking_filter_disabled_flag */
        bs_put_se(bs, e->dbf_beta); /* pps_beta_offset_div2 */
        bs_put_se(bs, e->dbf_tc);   /* pps_tc_offset_div2 */
    } else {
        /* control absent: deblocking on, zero offsets */
        bs_put_bits(bs, 0, 1);      /* deblocking_filter_control_present_flag */
    }
    bs_put_bits(bs, 0, 1);          /* pps_scaling_list_data_present_flag */
    bs_put_bits(bs, 0, 1);          /* lists_modification_present_flag */
    bs_put_ue(bs, 0);               /* log2_parallel_merge_level_minus2 */
    bs_put_bits(bs, 0, 1);          /* slice_segment_header_extension_present */
    bs_put_bits(bs, 0, 1);          /* pps_extension_present_flag */
    bs_rbsp_trailing(bs);
}
