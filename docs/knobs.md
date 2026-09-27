# The N265_* knob census (generated -- edit scripts/knob_census.py OVERRIDES, not this file)

Regenerate: `python3 scripts/knob_census.py`. CI check: `--check`.

Tier meanings: **default** = shipped behaviour, the env is its escape; **instrument** = measurement machinery (catalogued in docs/instruments.md when it has produced a result); **arm** = default-off gate or kept refuted arm -- kept deliberately because a refuted arm retains its re-pricing value.

In the default column, **presence** means the reader is a boolean: unset is off and any value arms it. **unset** means the value names a file or a string and unset is off. `= N265_X` means the default is another knob's resolved value.

## default (35)

| knob | default | reader |
|---|---|---|
| `N265_ABR_SB` | 1.7 | src/encoder/rc.c:218 |
| `N265_ABR_SBR` | 4.0 | src/encoder/rc.c:217 |
| `N265_ABR_SP` | 13.3 | src/encoder/rc.c:216 |
| `N265_BADAPT_MVCAP` | 4 | src/encoder/la.c:341 |
| `N265_BADAPT_MVGATE` | 20 | src/encoder/la.c:376 |
| `N265_CLAIM_PRIO` | 1 | src/encoder/encoder.c:5676 |
| `N265_CRF_DC_MEM` | 1000.0 | src/encoder/encoder.c:1589 |
| `N265_CRF_DUR` | 0.3 | src/encoder/encoder.c:1559 |
| `N265_CRF_DUR_FPS` | 25.0 | src/encoder/encoder.c:1560 |
| `N265_CT_MVRATE` | 16 | src/encoder/la.c:417 |
| `N265_FGC_CHROMA` | 1 | src/encoder/grain.c:133 |
| `N265_FGC_CORE` | 1.0 | src/encoder/grain.c:143 |
| `N265_FGC_FRAMES` | 1 | src/encoder/grain.c:121 |
| `N265_FGC_MIN` | 1.0 | src/encoder/grain.c:181 |
| `N265_FGC_PCT` | 25 | src/encoder/grain.c:109 |
| `N265_FGC_RATIO` | 2.0 | src/encoder/grain.c:167 |
| `N265_GATE_SCALE` | 100 | src/encoder/ctu.c:1837 |
| `N265_LAMBDA` | 0.68 | src/encoder/ctu.c:1634 |
| `N265_MCSTF_CLEAN` | 1.5 | src/encoder/mcstf.c:97 |
| `N265_MCSTF_PERIOD` | 8 | src/encoder/mcstf.c:114 |
| `N265_MCSTF_SIGMA` | 6.0 | src/encoder/mcstf.c:38 |
| `N265_PB_STEP` | 1.0 | src/encoder/encoder.c:4534 |
| `N265_RC_MVRATE` | 32 | src/encoder/rc.c:136 |
| `N265_RDOQ` | 1 | src/encoder/encoder.c:1805 |
| `N265_RDOQ_UP` | 1 | src/encoder/rdoq.c:288 |
| `N265_ROW_YIELD` | 5000 where 2 * min(rows, (cols + 1) / 2) > threads > 1, else 0 | src/encoder/encoder.c:2633 |
| `N265_SC_FAILW` | 0.40 | src/encoder/la.c:457 |
| `N265_SC_FAILW_SPIKE` | 1.6 | src/encoder/la.c:474 |
| `N265_SC_SPIKE` | 1.8 | src/encoder/la.c:530 |
| `N265_TREE_K` | 12 | src/encoder/encoder.c:2608 |
| `N265_TREE_SLOTS` | K - 6 (with the switched HD or UHD tree only) | src/encoder/encoder.c:2640 |
| `N265_VBV_DECAY` | 0.0625 | src/encoder/rc.c:228 |
| `N265_VBV_LOOKAHEAD` | 1 | src/encoder/encoder.c:2246 |
| `N265_VBV_RESERVE` | 0.2 | src/encoder/encoder.c:2253 |
| `N265_VBV_SAFE` | 0.85 | src/encoder/encoder.c:2239 |

## instrument (191)

| knob | default | reader |
|---|---|---|
| `N265_2P_CLAMP` | 6.0 | src/encoder/encoder.c:668 |
| `N265_2P_GAIN` | 0.25 | src/encoder/rc.c:206 |
| `N265_2P_QCOMP` | = N265_ABR_QCOMP | src/encoder/encoder.c:666 |
| `N265_2P_TRACE` | presence | src/encoder/encoder.c:683 |
| `N265_2P_TRUST` | 2.0 | src/encoder/encoder.c:675 |
| `N265_ABR_CMEM` | 1000.0 | src/encoder/rc.c:187 |
| `N265_ABR_COPEN` | 1 | src/encoder/rc.c:190 |
| `N265_ABR_CSEED` | 0.0 | src/encoder/rc.c:189 |
| `N265_ABR_CTF` | 1 | src/encoder/rc.c:194 |
| `N265_ABR_FBMAX` | 6.0 | src/encoder/rc.c:181 |
| `N265_ABR_FBSTEP` | 1 | src/encoder/rc.c:192 |
| `N265_ABR_FBUP` | = N265_ABR_FBMAX | src/encoder/rc.c:182 |
| `N265_ABR_HORIZON` | 6.0 | src/encoder/rc.c:173 |
| `N265_ABR_IBRAKE` | 1 | src/encoder/rc.c:191 |
| `N265_ABR_IGAIN` | 0.085 | src/encoder/rc.c:179 |
| `N265_ABR_IMAX` | 8.0 | src/encoder/rc.c:180 |
| `N265_ABR_IOFF` | 0.0 | src/encoder/rc.c:195 |
| `N265_ABR_IOFF_CT` | 0.0 | src/encoder/rc.c:196 |
| `N265_ABR_LVL` | 2.0 | src/encoder/rc.c:219 |
| `N265_ABR_MEAS` | 1 | src/encoder/rc.c:185 |
| `N265_ABR_MGUARD` | 0.0 | src/encoder/rc.c:186 |
| `N265_ABR_OPENI` | 0 | src/encoder/rc.c:193 |
| `N265_ABR_OPENSTEP` | 6.0 | src/encoder/rc.c:178 |
| `N265_ABR_PLANERR` | 0 | src/encoder/rc.c:183 |
| `N265_ABR_QCOMP` | 0.6 | src/encoder/rc.c:172 |
| `N265_ABR_SI` | 25.4 | src/encoder/rc.c:215 |
| `N265_ABR_SMEM` | 16.0 | src/encoder/rc.c:177 |
| `N265_ABR_STEP` | 1.0 | src/encoder/rc.c:176 |
| `N265_ABR_TCAP` | 1 | src/encoder/rc.c:188 |
| `N265_ABR_WMEM` | 1000.0 | src/encoder/rc.c:184 |
| `N265_API_DEFER` | presence | src/encoder/encoder.c:2714 |
| `N265_AQ_ANCHOR` | 6.4 | src/encoder/encoder.c:5351 |
| `N265_AQ_CLAMP` | 3.0 | src/encoder/encoder.c:5434 |
| `N265_AQ_CLAMP_FLAT` | 0.0 | src/encoder/encoder.c:5464 |
| `N265_AQ_DC` | ? | src/encoder/encoder.c:5351 |
| `N265_AQ_FLAT_SIGN` | 0 | src/encoder/encoder.c:1677 |
| `N265_AQ_FLOOR` | 4 | src/encoder/encoder.c:5236 |
| `N265_AQ_POOL` | 1 | src/encoder/encoder.c:2116 |
| `N265_AQ_SIGNAL` | 1 | src/encoder/encoder.c:1676 |
| `N265_AQ_STR` | 0.7 | src/encoder/encoder.c:1670 |
| `N265_AQ_TRACE` | presence | src/encoder/encoder.c:5515 |
| `N265_ASM_OFF` | unset | src/common/cpu.c:110 |
| `N265_BADAPT_REFW` | 128.0 | src/encoder/encoder.c:6919 |
| `N265_BADAPT_THR` | 0.35 | src/encoder/encoder.c:6918 |
| `N265_BADAPT_TRACE` | presence | src/encoder/la.c:1292 |
| `N265_BI_GATE` | 4 | src/encoder/ctu.c:3238 |
| `N265_BI_REFINE` | 0 | src/encoder/ctu.c:3190 |
| `N265_BRUN_TRACE` | presence | src/encoder/la.c:566 |
| `N265_B_INTRA_MARGIN` | 30 | src/encoder/ctu.c:3172 |
| `N265_CB64_AMVP` | 0 | src/encoder/ctu.c:3157 |
| `N265_CB64_GATE` | 1 | src/encoder/ctu.c:3139 |
| `N265_CB64_PROF` | presence | src/encoder/ctu.c:3110 |
| `N265_CB64_SKIPGATE` | 0 | src/encoder/ctu.c:2985 |
| `N265_CB64_SPLITGATE` | 0 | src/encoder/ctu.c:3008 |
| `N265_CB64_TRACE` | presence | src/encoder/ctu.c:196 |
| `N265_CBF_FAST` | 2 | src/encoder/ctu.c:1821 |
| `N265_CBIT_TRACE` | presence | src/encoder/encoder.c:1775 |
| `N265_CHROMA_PRED` | 1 | src/encoder/ctu.c:131 |
| `N265_CHROMA_W` | 0 | src/encoder/ctu.c:66 |
| `N265_CPRED_GATE` | 2 | src/encoder/ctu.c:2769 |
| `N265_CPRED_TRACE` | presence | src/encoder/ctu.c:4941 |
| `N265_CRF_DC_ANCHOR` | 3 | src/encoder/encoder.c:1585 |
| `N265_CRF_DC_WIN` | 12.0 | src/encoder/encoder.c:1591 |
| `N265_CRF_SHIFT` | 5.4 | src/encoder/encoder.c:4517 |
| `N265_CT_CENTRE` | 0 | src/encoder/la.c:1678 |
| `N265_CT_FRAMEW` | 1.0 | src/encoder/encoder.c:1609 |
| `N265_CT_FRAMEW_FLAT` | = N265_CT_QPW | src/encoder/encoder.c:1613 |
| `N265_CT_FRAME_CLAMP` | 0 | src/encoder/encoder.c:1617 |
| `N265_CT_FRAME_LEVEL` | 1 | src/encoder/la.c:1727 |
| `N265_CT_FRAME_SHOT` | 0 | src/encoder/encoder.c:1619 |
| `N265_CT_FRAME_UP` | 0 | src/encoder/encoder.c:1627 |
| `N265_CT_QPW` | 2.0 | src/encoder/encoder.c:1601 |
| `N265_CT_TRACE` | presence | src/encoder/la.c:1825 |
| `N265_CT_UP_GATE` | 0.24 | src/encoder/encoder.c:1645 |
| `N265_CUTREE_STR` | 2.0 | src/encoder/encoder.c:1600 |
| `N265_CU_STATS` | presence | src/encoder/encoder.c:1771 |
| `N265_CU_TRACE` | unset | src/encoder/encoder.c:1310 |
| `N265_DBF_BETA` | 0 | src/encoder/encoder.c:2136 |
| `N265_DBF_TC` | 0 | src/encoder/encoder.c:2137 |
| `N265_DECGRAD_TRACE` | presence | src/encoder/rdoq.c:353 |
| `N265_DECIMATE` | 15 | src/encoder/rdoq.c:306 |
| `N265_DECIMATE_GRAD` | = N265_DECIMATE | src/encoder/rdoq.c:320 |
| `N265_DEPTH_BOUND` | 0 | src/encoder/ctu.c:2182 |
| `N265_DEPTH_DUMP` | unset | src/encoder/encoder.c:1297 |
| `N265_DPB_HOLD` | 0 | src/encoder/encoder.c:768 |
| `N265_DPB_TRACE` | presence | src/encoder/encoder.c:1749 |
| `N265_DUP_FAST` | 0 | src/encoder/la.c:576 |
| `N265_DUP_THR` | 0.0 | src/encoder/la.c:614 |
| `N265_DUP_TRACE` | presence | src/encoder/la.c:586 |
| `N265_ES_THR` | 100 | src/encoder/la.c:1415 |
| `N265_FGC_TRACE` | presence | src/encoder/grain.c:192 |
| `N265_FLAT_GATE` | 10 | src/encoder/la.c:102 |
| `N265_FLAT_TRACE` | presence | src/encoder/encoder.c:5158 |
| `N265_FORCE_NONIDR` | -1 | src/encoder/encoder.c:6540 |
| `N265_FP_CENSUS` | presence | src/encoder/encoder.c:1772 |
| `N265_FULLPEL` | presence | src/encoder/mv.c:113 |
| `N265_HD_QP` | 27 | src/encoder/encoder.c:4493 |
| `N265_HF2` | 3 | src/encoder/encoder.c:3510 |
| `N265_HF2_TPP` | 150 | src/encoder/encoder.c:3538 |
| `N265_HF2_TPP14` | 190 | src/encoder/encoder.c:3549 |
| `N265_HF2_TRACE` | presence | src/encoder/encoder.c:3561 |
| `N265_HFCACHE` | 1 | src/encoder/encoder.c:3446 |
| `N265_INTER_NXN` | 0 | src/encoder/ctu.c:4156 |
| `N265_INTRA_ADAPT_K` | 32 | src/encoder/encoder.c:1783 |
| `N265_INTRA_ADAPT_THR` | 100 | src/encoder/encoder.c:1786 |
| `N265_INTRA_GATE` | 7 | src/encoder/ctu.c:1974 |
| `N265_INTRA_MIN` | 4 | src/encoder/encoder.c:6529 |
| `N265_INTRA_MPM` | 0 | src/encoder/ctu.c:4126 |
| `N265_INTRA_ORACLE` | 0 | src/encoder/ctu.c:2003 |
| `N265_INTRA_RD_N` | 3 | src/encoder/ctu.c:4115 |
| `N265_INTRA_REUSE` | 1 | src/encoder/ctu.c:4210 |
| `N265_INTRA_RSKIP` | 0 | src/encoder/ctu.c:1791 |
| `N265_INTRA_TRY_TRACE` | presence | src/encoder/ctu.c:1389 |
| `N265_INTRA_WIN_TRACE` | presence | src/encoder/encoder.c:1776 |
| `N265_I_INTRA` | 0 | src/encoder/ctu.c:4142 |
| `N265_LA_MEPRED` | 1 | src/encoder/la.c:551 |
| `N265_LA_MVSTAT` | presence | src/encoder/la.c:988 |
| `N265_LA_PROF` | presence | src/encoder/encoder.c:2719 |
| `N265_LEVEL_TRACE` | presence | src/encoder/encoder.c:2516 |
| `N265_MAX_FRAME_WORK` | 6 with --bframes >= 2, else 4 | src/encoder/encoder.c:2540 |
| `N265_MCSTF_TRACE` | presence | src/encoder/mcstf.c:125 |
| `N265_MEREUSE_TRACE` | presence | src/encoder/encoder.c:1773 |
| `N265_MERGE_RD_N` | 5 | src/encoder/ctu.c:3209 |
| `N265_MERGE_TRACE` | -1 | src/encoder/mv.c:107 |
| `N265_ME_PATTERN` | 0 | src/encoder/mv.c:824 |
| `N265_ME_SATD` | 0 | src/encoder/ctu.c:2900 |
| `N265_MIN_KEYINT` | 4 | src/encoder/encoder.c:6511 |
| `N265_MODE_HIST` | presence | src/encoder/sao.c:252 |
| `N265_MVD_BITS` | 0 | src/encoder/ctu.c:2888 |
| `N265_NO_AMP` | 1 | src/encoder/encoder.c:1705 |
| `N265_NO_RECT` | 1 | src/encoder/encoder.c:1703 |
| `N265_NO_SAO` | presence | src/encoder/encoder.c:1770 |
| `N265_NO_SPLIT` | presence | src/encoder/ctu.c:184 |
| `N265_NR_INTER` | 0 | src/encoder/encoder.c:2046 |
| `N265_NR_INTRA` | 0 | src/encoder/encoder.c:2044 |
| `N265_OPENGOP_CUT` | 0 | src/encoder/encoder.c:833 |
| `N265_OPENGOP_NOBARRIER` | 0 | src/encoder/encoder.c:840 |
| `N265_PART_GATE` | 2 | src/encoder/ctu.c:2056 |
| `N265_PB_CASCADE` | auto | src/encoder/encoder.c:1651 |
| `N265_PB_CASCADE_FLAT` | auto | src/encoder/encoder.c:1660 |
| `N265_PSY` | 0.0 | src/encoder/encoder.c:1807 |
| `N265_QP_FRAC` | = --bitrate or CRF+frame term | src/encoder/encoder.c:2097 |
| `N265_QP_FRAC_FLAT` | 0 | src/encoder/encoder.c:2110 |
| `N265_QP_MAP_TRACE` | unset | src/encoder/encoder.c:5608 |
| `N265_QP_ROUND1` | 1 | src/encoder/encoder.c:1678 |
| `N265_QP_TDITHER` | 1 | src/encoder/encoder.c:2129 |
| `N265_QP_TRACE` | presence | src/encoder/encoder.c:5624 |
| `N265_RC_LAG` | 0 | src/encoder/encoder.c:2644 |
| `N265_RC_TRACE` | presence | src/encoder/encoder.c:683 |
| `N265_RDOQ_ADAPT` | 0 | src/encoder/rdoq.c:294 |
| `N265_RDOQ_LAM` | 100 | src/encoder/rdoq.c:300 |
| `N265_RDOQ_LAT_SZ` | 0 | src/encoder/ctu.c:897 |
| `N265_RDOQ_TRACE` | presence | src/encoder/ctu.c:759 |
| `N265_RECT_FLAT` | 1 | src/encoder/ctu.c:2134 |
| `N265_RECT_RANK` | 50 | src/encoder/ctu.c:2099 |
| `N265_REF_LAG_HINT` | -1 | src/encoder/encoder.c:5654 |
| `N265_RSKIP` | 2 | src/encoder/ctu.c:1778 |
| `N265_SAO_ANIM` | 1 | src/encoder/sao.c:308 |
| `N265_SAO_BIAS` | 100 | src/encoder/sao.c:355 |
| `N265_SAO_CHROMA` | 1 | src/encoder/sao.c:511 |
| `N265_SAO_GATE` | 0 | src/encoder/sao.c:560 |
| `N265_SAO_QPLAM` | 0 | src/encoder/sao.c:345 |
| `N265_SAO_TRACE` | unset | src/encoder/sao.c:653 |
| `N265_SAO_TYPES` | 0 | src/encoder/sao.c:271 |
| `N265_SATD8` | 0 | src/encoder/satd.c:134 |
| `N265_SBH_RD` | 2 | src/encoder/rdoq.c:279 |
| `N265_SC_FORCE` | ? | src/encoder/la.c:485 |
| `N265_SC_HOLD` | 1 | src/encoder/la.c:241 |
| `N265_SC_TRACE` | presence | src/encoder/la.c:1137 |
| `N265_SERIAL_PROF` | 0 | src/encoder/encoder.c:2716 |
| `N265_SHOT_STATS` | unset | src/encoder/encoder.c:1795 |
| `N265_SHOT_TRACE` | presence | src/encoder/encoder.c:6787 |
| `N265_SHOT_WINDOW` | 0 | src/encoder/encoder.c:1947 |
| `N265_SKIP_EXIT` | 0 | src/encoder/ctu.c:1872 |
| `N265_SKIP_THR` | 2 | src/encoder/ctu.c:1677 |
| `N265_STALL_TRACE` | presence | src/encoder/encoder.c:3162 |
| `N265_SUBME` | 3 | src/encoder/mv.c:616 |
| `N265_SUBPEL_DEDUP` | 1 | src/encoder/mv.c:119 |
| `N265_SUBPEL_GATE` | 0 | src/encoder/mv.c:688 |
| `N265_SUBPEL_TRACE` | presence | src/encoder/ctu.c:3378 |
| `N265_TASK_TRACE` | unset | src/encoder/ttrace.c:31 |
| `N265_TREE_RAMP` | 4.0 | src/encoder/encoder.c:1905 |
| `N265_UHD_QP` | 29 | src/encoder/encoder.c:4473 |
| `N265_VBV_HRD` | 1 | src/encoder/encoder.c:2335 |
| `N265_VBV_TRACE` | presence | src/encoder/encoder.c:2334 |
| `N265_WEIGHTB` | 0 | src/encoder/encoder.c:2162 |
| `N265_WEIGHTP` | 0 | src/encoder/encoder.c:2160 |
| `N265_WP_GAIN` | 4 | src/encoder/wp.c:78 |
| `N265_WP_REFS` | 5 | src/encoder/wp.c:117 |
| `N265_WP_TRACE` | 0 | src/encoder/wp.c:51 |
| `N265_WP_WIN` | 40 | src/encoder/wp.c:101 |

## arm (29)

| knob | default | reader |
|---|---|---|
| `N265_ABR_IOFF_MAX` | 6.0 | src/encoder/rc.c:197 |
| `N265_AQ` | presence | src/encoder/encoder.c:1808 |
| `N265_AQ_FLAT` | presence | src/encoder/encoder.c:5175 |
| `N265_AQ_FLAT_THR` | = ungated | src/encoder/encoder.c:5503 |
| `N265_BADAPT_DEG` | 1.6 | src/encoder/encoder.c:6950 |
| `N265_CBQPOFFS` | 0 | src/encoder/encoder.c:2149 |
| `N265_CHROMA_W_PCT` | 100 | src/encoder/ctu.c:72 |
| `N265_CPRED_W` | 100 | src/encoder/ctu.c:2742 |
| `N265_CRF_LEGACY` | 0 | src/encoder/encoder.c:1556 |
| `N265_CRQPOFFS` | 0 | src/encoder/encoder.c:2151 |
| `N265_CT_LAMBDA` | 0.0 | src/encoder/encoder.c:1602 |
| `N265_EST_MODE` | 0 | src/encoder/est.c:24 |
| `N265_INTRA_B_MODES` | 0 | src/encoder/ctu.c:1353 |
| `N265_MEREUSE` | 0 | src/encoder/encoder.c:1774 |
| `N265_NO_AMVP` | presence | src/encoder/ctu.c:178 |
| `N265_NO_EARLYSKIP` | presence | src/encoder/ctu.c:190 |
| `N265_NO_GATES` | presence | src/encoder/ctu.c:172 |
| `N265_NO_TMVP` | presence | src/encoder/encoder.c:3392 |
| `N265_OLD_BS` | presence | src/encoder/deblock.c:158 |
| `N265_RD_PROXY` | 0 | src/encoder/ctu.c:5276 |
| `N265_RUNG_GATE` | -2 | src/encoder/mv.c:745 |
| `N265_RUNG_RERANK` | 0 | src/encoder/mv.c:761 |
| `N265_SC_CONJ` | 3 | src/encoder/ctu.c:1737 |
| `N265_SHOT_CLAMP` | 2.0 | src/encoder/encoder.c:1966 |
| `N265_SHOT_EMA` | 0.5 | src/encoder/encoder.c:1971 |
| `N265_SHOT_K` | 1.0 | src/encoder/encoder.c:1965 |
| `N265_STATIC_COMMIT` | 0 | src/encoder/ctu.c:1713 |
| `N265_SUBME_RUNG` | 0 | src/encoder/mv.c:731 |
| `N265_SUBPEL_RATE` | 0 | src/encoder/mv.c:644 |
