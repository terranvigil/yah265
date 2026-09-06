/*
 * Spec constants (ITU-T H.265 Tables 9-5..9-32, 8.6.4 transform core,
 * 6.5.3 scan orders) and their builders.
 * Copyright (c) 2026, the yah265 authors. GPL-2.0-or-later; see LICENSE.
 */

#include "encoder/tables.h"

/* initType 0 (I slices) initValues, laid out per tables.h. */
const uint8_t n265_ctx_init_i[CTX_COUNT] = {
    /* split_cu_flag */
    139, 141, 157,
    /* part_mode (intra bin0) */
    184,
    /* prev_intra_luma_pred_flag */
    184,
    /* intra_chroma_pred_mode */
    63,
    /* cbf_luma: ctxInc = trafoDepth==0 ? 1 : 0 */
    111, 141,
    /* cbf_cb/cbf_cr, ctxInc = trafoDepth */
    94, 138, 182, 154,
    /* last_sig_coeff_x_prefix */
    110, 110, 124, 125, 140, 153, 125, 127, 140, 109, 111, 143, 127, 111,
     79, 108, 123,  63,
    /* last_sig_coeff_y_prefix */
    110, 110, 124, 125, 140, 153, 125, 127, 140, 109, 111, 143, 127, 111,
     79, 108, 123,  63,
    /* coded_sub_block_flag */
     91, 171, 134, 141,
    /* sig_coeff_flag (42) */
    111, 111, 125, 110, 110,  94, 124, 108, 124, 107, 125, 141, 179, 153,
    125, 107, 125, 141, 179, 153, 125, 107, 125, 141, 179, 153, 125, 140,
    139, 182, 182, 152, 136, 152, 136, 153, 136, 139, 111, 136, 139, 111,
    /* coeff_abs_level_greater1_flag (24) */
    140,  92, 137, 138, 140, 152, 138, 139, 153,  74, 149,  92, 139, 107,
    122, 152, 140, 179, 166, 182, 140, 227, 122, 197,
    /* coeff_abs_level_greater2_flag (6) */
    138, 153, 136, 167, 152, 152,
    /* sao_merge_flag, sao_type_idx */
    153, 200,
    /* cu_skip_flag (unused in I) */
    154, 154, 154,
    /* pred_mode_flag, merge_flag, merge_idx, rqt_root_cbf (unused in I) */
    154, 154, 154, 154,
    /* abs_mvd_greater0/1, mvp_l0_flag (unused in I) */
    154, 154, 154,
    /* inter_pred_idc (unused in I) */
    154, 154, 154, 154, 154,
    /* split_transform_flag */
    153, 138, 138,
    /* cu_qp_delta_abs */
    154, 154,
    /* ref_idx_lX */
    153, 153,
};

/* initType 1: P slices with cabac_init_flag 0 */
const uint8_t n265_ctx_init_p[CTX_COUNT] = {
    /* split_cu_flag */
    107, 139, 126,
    /* part_mode (intra bin0) */
    154,
    /* prev_intra_luma_pred_flag */
    154,
    /* intra_chroma_pred_mode */
    152,
    /* cbf_luma */
    153, 111,
    /* cbf_cb/cbf_cr */
    149, 107, 167, 154,
    /* last_sig_coeff_x_prefix */
    125, 110,  94, 110,  95,  79, 125, 111, 110,  78, 110, 111, 111,  95,
     94, 108, 123, 108,
    /* last_sig_coeff_y_prefix */
    125, 110,  94, 110,  95,  79, 125, 111, 110,  78, 110, 111, 111,  95,
     94, 108, 123, 108,
    /* coded_sub_block_flag */
    121, 140,  61, 154,
    /* sig_coeff_flag (42) */
    155, 154, 139, 153, 139, 123, 123,  63, 153, 166, 183, 140, 136, 153,
    154, 166, 183, 140, 136, 153, 154, 166, 183, 140, 136, 153, 154, 170,
    153, 123, 123, 107, 121, 107, 121, 167, 151, 183, 140, 151, 183, 140,
    /* coeff_abs_level_greater1_flag (24) */
    154, 196, 196, 167, 154, 152, 167, 182, 182, 134, 149, 136, 153, 121,
    136, 137, 169, 194, 166, 167, 154, 167, 137, 182,
    /* coeff_abs_level_greater2_flag (6) */
    107, 167,  91, 122, 107, 167,
    /* sao_merge_flag, sao_type_idx */
    153, 185,
    /* cu_skip_flag */
    197, 185, 201,
    /* pred_mode_flag, merge_flag, merge_idx, rqt_root_cbf */
    149, 110, 122, 79,
    /* abs_mvd_greater0/1, mvp_l0_flag */
    140, 198, 168,
    /* inter_pred_idc */
     95,  79,  63,  31,  31,
    /* split_transform_flag */
    124, 138,  94,
    /* cu_qp_delta_abs */
    154, 154,
    /* ref_idx_lX */
    153, 153,
};

/* initType 2: B slices with cabac_init_flag 0 */
const uint8_t n265_ctx_init_b[CTX_COUNT] = {
    /* split_cu_flag */
    107, 139, 126,
    /* part_mode (intra bin0) */
    154,
    /* prev_intra_luma_pred_flag */
    183,
    /* intra_chroma_pred_mode */
    152,
    /* cbf_luma */
    153, 111,
    /* cbf_cb/cbf_cr */
    149,  92, 167, 154,
    /* last_sig_coeff_x_prefix */
    125, 110, 124, 110,  95,  94, 125, 111, 111,  79, 125, 126, 111, 111,
     79, 108, 123,  93,
    /* last_sig_coeff_y_prefix */
    125, 110, 124, 110,  95,  94, 125, 111, 111,  79, 125, 126, 111, 111,
     79, 108, 123,  93,
    /* coded_sub_block_flag */
    121, 140,  61, 154,
    /* sig_coeff_flag (42) */
    170, 154, 139, 153, 139, 123, 123,  63, 124, 166, 183, 140, 136, 153,
    154, 166, 183, 140, 136, 153, 154, 166, 183, 140, 136, 153, 154, 170,
    153, 138, 138, 122, 121, 122, 121, 167, 151, 183, 140, 151, 183, 140,
    /* coeff_abs_level_greater1_flag (24) */
    154, 196, 167, 167, 154, 152, 167, 182, 182, 134, 149, 136, 153, 121,
    136, 122, 169, 208, 166, 167, 154, 152, 167, 182,
    /* coeff_abs_level_greater2_flag (6) */
    107, 167,  91, 107, 107, 167,
    /* sao_merge_flag, sao_type_idx */
    153, 160,
    /* cu_skip_flag */
    197, 185, 201,
    /* pred_mode_flag, merge_flag, merge_idx, rqt_root_cbf */
    134, 154, 137, 79,
    /* abs_mvd_greater0/1, mvp_l0_flag */
    169, 198, 168,
    /* inter_pred_idc */
     95,  79,  63,  31,  31,
    /* split_transform_flag */
    224, 167, 122,
    /* cu_qp_delta_abs */
    154, 154,
    /* ref_idx_lX */
    153, 153,
};

/* 9.3.4.2.5, ctxIdxMap for 4x4 TBs */
const uint8_t n265_sig_ctx_map4x4[15] = {
    0, 1, 4, 5, 2, 3, 4, 5, 6, 6, 8, 8, 7, 7, 8
};

uint8_t n265_scan_coef[3][16];
uint8_t n265_scan_sb[3][4][64];
int8_t n265_tmatrix[32][32];

/* up-right diagonal scan order (6.5.3): each diagonal starts at (0, d) and
 * walks x++, y-- */
static void build_scan(uint8_t *out, int size, int idx)
{
    int i = 0;
    if (idx == 0) {
        for (int d = 0; d <= 2 * (size - 1); d++)
            for (int x = 0; x <= d; x++) {
                int y = d - x;
                if (x < size && y < size)
                    out[i++] = (uint8_t)((y << 4) | x);
            }
    } else if (idx == 1) {          /* horizontal: row-major */
        for (int y = 0; y < size; y++)
            for (int x = 0; x < size; x++)
                out[i++] = (uint8_t)((y << 4) | x);
    } else {                        /* vertical: column-major */
        for (int x = 0; x < size; x++)
            for (int y = 0; y < size; y++)
                out[i++] = (uint8_t)((y << 4) | x);
    }
}

/* Transform core: rows 0/8/16/24 etc. of the 32-point matrix serve the
 * smaller sizes. Built from the per-size odd-row constant sets; even rows
 * recurse. Spec 8.6.4's literal table reproduces exactly. */
static void build_tmatrix(void)
{
    static const int odd4[]  = { 83, 36 };
    static const int odd8[]  = { 89, 75, 50, 18 };
    static const int odd16[] = { 90, 87, 80, 70, 57, 43, 25,  9 };
    static const int odd32[] = { 90, 90, 88, 85, 82, 78, 73, 67,
                                 61, 54, 46, 38, 31, 22, 13,  4 };
    static int t4[4][4], t8[8][8], t16[16][16], t32[32][32];

    for (int j = 0; j < 4; j++) {
        t4[0][j] = 64;
        t4[2][j] = (j == 1 || j == 2) ? -64 : 64;
    }
    t4[1][0] = 83; t4[1][1] = 36; t4[1][2] = -36; t4[1][3] = -83;
    t4[3][0] = 36; t4[3][1] = -83; t4[3][2] = 83; t4[3][3] = -36;

/* dest size M = 2N. Even rows: T_M[2k][j] = T_N[k][j], mirrored with +1
 * (row index even => cos(2k*pi - x) = cos(x)). Odd rows from the odd
 * constant set: angle unit pi/(2M), a = (2k+1)(2j+1) folded into the
 * first quadrant with sign. */
#define GROW(dst, src, N, odd) do { \
    for (int k = 0; k < (N); k++) \
        for (int j = 0; j < (N); j++) { \
            dst[2 * k][j] = src[k][j]; \
            dst[2 * k][2 * (N) - 1 - j] = src[k][j]; \
        } \
    for (int k = 0; k < (N); k++) \
        for (int j = 0; j < 2 * (N); j++) { \
            int a = ((2 * k + 1) * (2 * j + 1)) % (8 * (N)); \
            int sign = 1; \
            if (a > 4 * (N)) a = 8 * (N) - a;            /* cos(2pi-x) */ \
            if (a > 2 * (N)) { a = 4 * (N) - a; sign = -1; } /* cos(pi-x) */ \
            dst[2 * k + 1][j] = sign * odd[(a - 1) / 2]; \
        } \
} while (0)

    GROW(t8, t4, 4, odd8);
    GROW(t16, t8, 8, odd16);
    GROW(t32, t16, 16, odd32);
#undef GROW

    for (int i = 0; i < 32; i++)
        for (int j = 0; j < 32; j++)
            n265_tmatrix[i][j] = (int8_t)t32[i][j];
    (void)odd4;
}

void n265_tables_init(void)
{
    static int done;
    if (done)
        return;
    for (int k = 0; k < 3; k++) {
        build_scan(n265_scan_coef[k], 4, k);
        for (int l = 2; l <= 5; l++)
            build_scan(n265_scan_sb[k][l - 2], 1 << (l - 2), k);
    }
    build_tmatrix();
    done = 1;
}
