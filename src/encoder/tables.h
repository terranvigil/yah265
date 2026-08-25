/*
 * Spec constant tables and derived scan/matrix builders.
 * Copyright (c) 2026, the next265 authors. GPL-2.0-or-later; see LICENSE.
 */

#ifndef N265_TABLES_H
#define N265_TABLES_H

#include <stdint.h>

/* Context indices for the I-slice subset implemented so far. Layout is
 * ours; init values are ITU-T H.265 Table 9-5..9-32 constants. */
enum {
    CTX_SPLIT_CU = 0,       /* 3 */
    CTX_PART_MODE = 3,      /* 1 used (intra bin0) */
    CTX_PREV_INTRA = 4,     /* 1 */
    CTX_CHROMA_MODE = 5,    /* 1 */
    CTX_CBF_LUMA = 6,       /* 2 */
    CTX_CBF_CHROMA = 8,     /* 4 (trafo depth) */
    CTX_LAST_X = 12,        /* 18 */
    CTX_LAST_Y = 30,        /* 18 */
    CTX_SIG_GROUP = 48,     /* 4: luma 0-1, chroma 2-3 */
    CTX_SIG = 52,           /* 42: luma 0-26, chroma 27-41 */
    CTX_G1 = 94,            /* 24: luma 0-15, chroma 16-23 */
    CTX_G2 = 118,           /* 6: luma 0-3, chroma 4-5 */
    CTX_SAO_MERGE = 124,    /* 1 */
    CTX_SAO_TYPE = 125,     /* 1 */
    CTX_CU_SKIP = 126,      /* 3 (left/above skip neighbors) */
    CTX_PRED_MODE = 129,    /* 1 */
    CTX_MERGE_FLAG = 130,   /* 1 */
    CTX_MERGE_IDX = 131,    /* 1 */
    CTX_RQT_ROOT_CBF = 132, /* 1 */
    CTX_MVD_GR0 = 133,      /* 1 */
    CTX_MVD_GR1 = 134,      /* 1 */
    CTX_MVP_FLAG = 135,     /* 1 */
    CTX_COUNT = 136,
};

/* initType 0 (I slices) and initType 1 (P slices, cabac_init_flag 0) */
extern const uint8_t n265_ctx_init_i[CTX_COUNT];
extern const uint8_t n265_ctx_init_p[CTX_COUNT];

/* sig_coeff_flag ctxIdxMap for 4x4 TBs (9.3.4.2.5), index (yP<<2)|xP,
 * position 15 never coded. */
extern const uint8_t n265_sig_ctx_map4x4[15];

/* Scan tables: pos -> (x,y) packed as (y<<4)|x inside one 4x4 sub-block,
 * and sub-block scans per TB size. Index 0 diag, 1 horizontal, 2 vertical
 * (6.5.3/6.5.4). Built at first use. */
extern uint8_t n265_scan_coef[3][16];       /* coeff scan in a sub-block */
extern uint8_t n265_scan_sb[3][4][64];      /* sub-block scan, log2TbSize 2..5 */

/* scanIdx for an intra TB (residual coding semantics): MDCS applies to
 * 4x4 and luma 8x8 TBs. */
static inline int n265_scan_idx(int mode, int log2n, int c_idx)
{
    if (log2n == 2 || (log2n == 3 && c_idx == 0)) {
        if (mode >= 6 && mode <= 14)
            return 2;   /* near-horizontal modes: vertical scan */
        if (mode >= 22 && mode <= 30)
            return 1;   /* near-vertical modes: horizontal scan */
    }
    return 0;
}

void n265_tables_init(void);

/* Inverse-transform matrix rows, spec 8.6.4 constants: n265_tmatrix[i][j]
 * for the 32-point core; N-point rows are strided (i * 32/N). */
extern int8_t n265_tmatrix[32][32];

#endif
