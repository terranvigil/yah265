/*
 * Parameter set writers: VPS, SPS, PPS (ITU-T H.265 7.3.2).
 * Copyright (c) 2026, the yah265 authors. GPL-2.0-or-later; see LICENSE.
 */

#include "encoder/encoder.h"
#include "encoder/sao.h"

/* MaxDpbSize for a level (A.4.2) with maxDpbPicBuf = 6. A picture well
 * below the level's MaxLumaPs is granted more picture storage buffers,
 * which is how a deep pyramid pays for itself: the level rises until the
 * DPB the reference structure needs fits inside it. */
static int max_dpb_size(int ps, int max_luma_ps)
{
    if (ps <= max_luma_ps >> 2)
        return 16;
    if (ps <= max_luma_ps >> 1)
        return 12;
    if ((int64_t)ps <= ((int64_t)max_luma_ps * 3) >> 2)
        return 8;
    return 6;
}

/* general_level_idc = level * 30. Picked from Table A.8's MaxLumaPs, the
 * per-dimension bound sqrt(MaxLumaPs * 8), and the DPB: A.4.2 requires
 * sps_max_dec_pic_buffering_minus1 < MaxDpbSize, so a level that fits the
 * picture but not the reference structure is not a legal choice.
 * Bitrate limits are ignored: phase 0's PCM payload exceeds every tier's
 * MaxBR by construction, which is recorded in docs/ideas.md and dies with
 * PCM. */
static int pick_level_idc(int w, int h, int dpb)
{
    static const struct { int idc; int max_luma_ps; } lv[] = {
        { 30,    36864 }, { 60,   122880 }, { 63,   245760 },
        { 90,   552960 }, { 93,   983040 }, {120,  2228224 },
        {150,  8912896 }, {180, 35651584 },
    };
    int ps = w * h;
    for (unsigned i = 0; i < sizeof(lv) / sizeof(lv[0]); i++) {
        int dim_ok = (int64_t)w * w <= (int64_t)lv[i].max_luma_ps * 8 &&
                     (int64_t)h * h <= (int64_t)lv[i].max_luma_ps * 8;
        if (ps <= lv[i].max_luma_ps && dim_ok &&
            dpb <= max_dpb_size(ps, lv[i].max_luma_ps))
            return lv[i].idc;
    }
    return 186; /* 6.2 */
}

/* profile_tier_level with maxNumSubLayersMinus1 == 0 (7.3.3). */
static void write_ptl(bitstream_t *bs, int w, int h, int dpb)
{
    bs_put_bits(bs, 0, 2);          /* general_profile_space */
    bs_put_bits(bs, 0, 1);          /* general_tier_flag: main */
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
     *     DPB rule (A.4.2) are all pick_level_idc reads, and Table A.2's
     *     CpbBrVclFactor / CpbBrNalFactor are 1000 / 1100 at Main AND at
     *     Main10, so even the bit-rate leg this encoder does not consult
     *     would not have moved. The debt is that it does not consult it
     *     (docs/ideas.md) and that debt is the same at both depths.
     *   - The VUI is absent (vui_parameters_present_flag 0 in the SPS),
     *     so there is no depth field there to keep consistent with
     *     bit_depth_luma_minus8. When a VUI lands it inherits the same
     *     constant.
     * Verified on the wire: dec265 reads profile_idc Main10 with
     * compatibility flag 2 alone, and ffprobe reads "Main 10" /
     * yuv420p10le. */
    bs_put_bits(bs, BIT_DEPTH == 8 ? 1 : 2, 5);   /* general_profile_idc */
    bs_put_bits(bs, BIT_DEPTH == 8 ? 0x60000000u : 0x20000000u, 32);
    bs_put_bits(bs, 1, 1);          /* general_progressive_source_flag */
    bs_put_bits(bs, 0, 1);          /* general_interlaced_source_flag */
    bs_put_bits(bs, 1, 1);          /* general_non_packed_constraint_flag */
    bs_put_bits(bs, 1, 1);          /* general_frame_only_constraint_flag */
    bs_put_bits(bs, 0, 32);         /* general_reserved_zero_44bits... */
    bs_put_bits(bs, 0, 12);         /* ...cont */
    bs_put_bits(bs, (uint32_t)pick_level_idc(w, h, dpb), 8);
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
    write_ptl(bs, e->param.width, e->param.height, e->dpb_buffering);
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
    write_ptl(bs, e->param.width, e->param.height, e->dpb_buffering);
    bs_put_ue(bs, 0);               /* sps_seq_parameter_set_id */
    bs_put_ue(bs, 1);               /* chroma_format_idc: 4:2:0 */
    bs_put_ue(bs, (uint32_t)e->pad_w);
    bs_put_ue(bs, (uint32_t)e->pad_h);
    if (crop_r || crop_b) {
        bs_put_bits(bs, 1, 1);      /* conformance_window_flag */
        bs_put_ue(bs, 0);                       /* left, chroma units */
        bs_put_ue(bs, (uint32_t)(crop_r / 2));  /* right */
        bs_put_ue(bs, 0);                       /* top */
        bs_put_ue(bs, (uint32_t)(crop_b / 2));  /* bottom */
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
    bs_put_bits(bs, e->mode != N265_MODE_PCM && n265_sao_seq_enabled(), 1);
                                    /* sample_adaptive_offset_enabled_flag */
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
    bs_put_bits(bs, 0, 1);          /* vui_parameters_present_flag */
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
    bs_put_bits(bs, e->mode != N265_MODE_PCM, 1); /* sign_data_hiding */
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
    bs_put_bits(bs, 0, 1);          /* transquant_bypass_enabled_flag */
    bs_put_bits(bs, 0, 1);          /* tiles_enabled_flag */
    bs_put_bits(bs, 0, 1);          /* entropy_coding_sync_enabled_flag */
    bs_put_bits(bs, 1, 1);          /* pps_loop_filter_across_slices_enabled */
    if (e->mode == N265_MODE_PCM) {
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
