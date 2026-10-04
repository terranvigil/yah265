# The N265_* knob census (generated -- edit scripts/knob_census.py OVERRIDES, not this file)

Regenerate: `python3 scripts/knob_census.py`. CI check: `--check`.

Tier meanings: **default** = shipped behaviour, the env is its escape; **instrument** = measurement machinery (catalogued in docs/instruments.md when it has produced a result); **arm** = default-off gate or kept refuted arm -- kept deliberately because a refuted arm retains its re-pricing value.

In the default column, **presence** means the reader is a boolean: unset is off and any value arms it. **unset** means the value names a file or a string and unset is off. `= N265_X` means the default is another knob's resolved value.

## default (41)

| knob | default | reader |
|---|---|---|
| `N265_ABR_SB` | 1.7 | src/encoder/rc.c:243 |
| `N265_ABR_SBR` | 4.0 | src/encoder/rc.c:242 |
| `N265_ABR_SP` | 13.3 | src/encoder/rc.c:241 |
| `N265_BADAPT_MVCAP` | 4 | src/encoder/la.c:341 |
| `N265_BADAPT_MVGATE` | 20 | src/encoder/la.c:376 |
| `N265_CLAIM_PRIO` | 1 | src/encoder/encoder.c:5865 |
| `N265_CRF_DC_MEM` | 1000.0 | src/encoder/encoder.c:1598 |
| `N265_CRF_DUR` | 0.3 | src/encoder/encoder.c:1568 |
| `N265_CRF_DUR_FPS` | 25.0 | src/encoder/encoder.c:1569 |
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
| `N265_MERGE_PICK_RATE` | 0 | src/encoder/ctu.c:5133 |
| `N265_PB_STEP` | 1.0 | src/encoder/encoder.c:4685 |
| `N265_RC_MVRATE` | 32 | src/encoder/rc.c:136 |
| `N265_RDOQ` | 1 | src/encoder/encoder.c:1814 |
| `N265_RDOQ_UP` | 1 | src/encoder/rdoq.c:288 |
| `N265_ROW_YIELD` | 5000 where 2 * min(rows, (cols + 1) / 2) > threads > 1, else 0 | src/encoder/encoder.c:2666 |
| `N265_SC_FAILW` | 0.40 | src/encoder/la.c:457 |
| `N265_SC_FAILW_SPIKE` | 1.6 | src/encoder/la.c:474 |
| `N265_SC_SPIKE` | 1.8 | src/encoder/la.c:530 |
| `N265_TREE_K` | 12 | src/encoder/encoder.c:2617 |
| `N265_TREE_SLOTS` | K - 6 (with the switched HD or UHD tree only) | src/encoder/encoder.c:2673 |
| `N265_VBR_LAG` | 7 (VBR, threads > 1) | src/encoder/encoder.c:2636 |
| `N265_VBR_LAG_CAL` | 0.1 | src/encoder/encoder.c:2689 |
| `N265_VBR_LAG_LOW` | 0.5 | src/encoder/encoder.c:2695 |
| `N265_VBR_LAG_PEND` | 1 | src/encoder/encoder.c:2699 |
| `N265_VBR_LAG_WARM` | 2 * (lag + 1) | src/encoder/encoder.c:2687 |
| `N265_VBV_DECAY` | 0.0625 | src/encoder/rc.c:253 |
| `N265_VBV_LOOKAHEAD` | 1 | src/encoder/encoder.c:2255 |
| `N265_VBV_RESERVE` | 0.2 | src/encoder/encoder.c:2262 |
| `N265_VBV_SAFE` | 0.85 | src/encoder/encoder.c:2248 |

## instrument (196)

| knob | default | reader |
|---|---|---|
| `N265_2P_CLAMP` | 6.0 | src/encoder/encoder.c:668 |
| `N265_2P_GAIN` | 0.25 | src/encoder/rc.c:231 |
| `N265_2P_QCOMP` | = N265_ABR_QCOMP | src/encoder/encoder.c:666 |
| `N265_2P_TRACE` | presence | src/encoder/encoder.c:683 |
| `N265_2P_TRUST` | 2.0 | src/encoder/encoder.c:675 |
| `N265_ABR_CMEM` | 1000.0 | src/encoder/rc.c:212 |
| `N265_ABR_COPEN` | 1 | src/encoder/rc.c:215 |
| `N265_ABR_CSEED` | 0.0 | src/encoder/rc.c:214 |
| `N265_ABR_CTF` | 1 | src/encoder/rc.c:219 |
| `N265_ABR_FBMAX` | 6.0 | src/encoder/rc.c:181 |
| `N265_ABR_FBSTEP` | 1 | src/encoder/rc.c:217 |
| `N265_ABR_FBUP` | 1.5 under a VBV, else = N265_ABR_FBMAX | src/encoder/rc.c:207 |
| `N265_ABR_FLATC` | 17.0 under a VBV, else 0 | src/encoder/rc.c:259 |
| `N265_ABR_HORIZON` | 6.0 | src/encoder/rc.c:173 |
| `N265_ABR_IBRAKE` | 1 | src/encoder/rc.c:216 |
| `N265_ABR_IGAIN` | 0.085 | src/encoder/rc.c:179 |
| `N265_ABR_IMAX` | 8.0 | src/encoder/rc.c:180 |
| `N265_ABR_IOFF` | 0.0 | src/encoder/rc.c:220 |
| `N265_ABR_IOFF_CT` | 1.0 under a VBV, else 0 | src/encoder/rc.c:221 |
| `N265_ABR_LVL` | 2.0 | src/encoder/rc.c:244 |
| `N265_ABR_MEAS` | 1 | src/encoder/rc.c:210 |
| `N265_ABR_MGUARD` | 0.0 | src/encoder/rc.c:211 |
| `N265_ABR_OPENI` | 0 | src/encoder/rc.c:218 |
| `N265_ABR_OPENSTEP` | 6.0 | src/encoder/rc.c:178 |
| `N265_ABR_PLANERR` | 0 | src/encoder/rc.c:208 |
| `N265_ABR_QCOMP` | 0.6 | src/encoder/rc.c:172 |
| `N265_ABR_SI` | 25.4 | src/encoder/rc.c:240 |
| `N265_ABR_SMEM` | 16.0 | src/encoder/rc.c:177 |
| `N265_ABR_STEP` | 1.0 | src/encoder/rc.c:176 |
| `N265_ABR_TCAP` | 1 | src/encoder/rc.c:213 |
| `N265_ABR_WMEM` | 1000.0 | src/encoder/rc.c:209 |
| `N265_API_DEFER` | presence | src/encoder/encoder.c:2770 |
| `N265_AQ_ANCHOR` | 6.4 | src/encoder/encoder.c:5540 |
| `N265_AQ_CLAMP` | 3.0 | src/encoder/encoder.c:5623 |
| `N265_AQ_CLAMP_FLAT` | 0.0 | src/encoder/encoder.c:5653 |
| `N265_AQ_DC` | ? | src/encoder/encoder.c:5540 |
| `N265_AQ_FLAT_SIGN` | 0 | src/encoder/encoder.c:1686 |
| `N265_AQ_FLOOR` | 4 | src/encoder/encoder.c:5425 |
| `N265_AQ_POOL` | 1 | src/encoder/encoder.c:2125 |
| `N265_AQ_SIGNAL` | 1 | src/encoder/encoder.c:1685 |
| `N265_AQ_STR` | 0.7 | src/encoder/encoder.c:1679 |
| `N265_AQ_TRACE` | presence | src/encoder/encoder.c:5704 |
| `N265_ASM_OFF` | unset | src/common/cpu.c:110 |
| `N265_BADAPT_REFW` | 128.0 | src/encoder/encoder.c:7242 |
| `N265_BADAPT_THR` | 0.35 | src/encoder/encoder.c:7241 |
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
| `N265_CBIT_TRACE` | presence | src/encoder/encoder.c:1784 |
| `N265_CHROMA_PRED` | 1 | src/encoder/ctu.c:131 |
| `N265_CHROMA_W` | 0 | src/encoder/ctu.c:66 |
| `N265_CPRED_GATE` | 2 | src/encoder/ctu.c:2769 |
| `N265_CPRED_TRACE` | presence | src/encoder/ctu.c:4941 |
| `N265_CRF_DC_ANCHOR` | 3 | src/encoder/encoder.c:1594 |
| `N265_CRF_DC_WIN` | 12.0 | src/encoder/encoder.c:1600 |
| `N265_CRF_SHIFT` | 5.4 | src/encoder/encoder.c:4668 |
| `N265_CT_CENTRE` | 0 | src/encoder/la.c:1682 |
| `N265_CT_FRAMEW` | 1.0 | src/encoder/encoder.c:1618 |
| `N265_CT_FRAMEW_FLAT` | = N265_CT_QPW | src/encoder/encoder.c:1622 |
| `N265_CT_FRAME_CLAMP` | 0 | src/encoder/encoder.c:1626 |
| `N265_CT_FRAME_LEVEL` | 1 | src/encoder/la.c:1731 |
| `N265_CT_FRAME_SHOT` | 0 | src/encoder/encoder.c:1628 |
| `N265_CT_FRAME_UP` | 0 | src/encoder/encoder.c:1636 |
| `N265_CT_QPW` | 2.0 | src/encoder/encoder.c:1610 |
| `N265_CT_TRACE` | presence | src/encoder/la.c:1829 |
| `N265_CT_UP_GATE` | 0.24 | src/encoder/encoder.c:1654 |
| `N265_CUTREE_STR` | 2.0 | src/encoder/encoder.c:1609 |
| `N265_CU_STATS` | presence | src/encoder/encoder.c:1780 |
| `N265_CU_TRACE` | unset | src/encoder/encoder.c:1310 |
| `N265_DBF_BETA` | 0 | src/encoder/encoder.c:2145 |
| `N265_DBF_TC` | 0 | src/encoder/encoder.c:2146 |
| `N265_DECGRAD_TRACE` | presence | src/encoder/rdoq.c:353 |
| `N265_DECIMATE` | 15 | src/encoder/rdoq.c:306 |
| `N265_DECIMATE_GRAD` | = N265_DECIMATE | src/encoder/rdoq.c:320 |
| `N265_DEPTH_BOUND` | 0 | src/encoder/ctu.c:2182 |
| `N265_DEPTH_DUMP` | unset | src/encoder/encoder.c:1297 |
| `N265_DPB_HOLD` | 0 | src/encoder/encoder.c:768 |
| `N265_DPB_TRACE` | presence | src/encoder/encoder.c:1758 |
| `N265_DUP_FAST` | 0 | src/encoder/la.c:576 |
| `N265_DUP_THR` | 0.0 | src/encoder/la.c:614 |
| `N265_DUP_TRACE` | presence | src/encoder/la.c:586 |
| `N265_ES_THR` | 100 | src/encoder/la.c:1415 |
| `N265_FGC_TRACE` | presence | src/encoder/grain.c:192 |
| `N265_FLAT_GATE` | 10 | src/encoder/la.c:102 |
| `N265_FLAT_TRACE` | presence | src/encoder/encoder.c:5347 |
| `N265_FORCE_NONIDR` | -1 | src/encoder/encoder.c:6863 |
| `N265_FP_CENSUS` | presence | src/encoder/encoder.c:1781 |
| `N265_FULLPEL` | presence | src/encoder/mv.c:113 |
| `N265_HD_QP` | 27 | src/encoder/encoder.c:4644 |
| `N265_HF2` | 3 | src/encoder/encoder.c:3577 |
| `N265_HF2_TPP` | 150 | src/encoder/encoder.c:3605 |
| `N265_HF2_TPP14` | 190 | src/encoder/encoder.c:3616 |
| `N265_HF2_TRACE` | presence | src/encoder/encoder.c:3628 |
| `N265_HFCACHE` | 1 | src/encoder/encoder.c:3513 |
| `N265_INTER_NXN` | 0 | src/encoder/ctu.c:4156 |
| `N265_INTRA_ADAPT_K` | 32 | src/encoder/encoder.c:1792 |
| `N265_INTRA_ADAPT_THR` | 100 | src/encoder/encoder.c:1795 |
| `N265_INTRA_GATE` | 7 | src/encoder/ctu.c:1974 |
| `N265_INTRA_MIN` | 4 | src/encoder/encoder.c:6852 |
| `N265_INTRA_MPM` | 0 | src/encoder/ctu.c:4126 |
| `N265_INTRA_ORACLE` | 0 | src/encoder/ctu.c:2003 |
| `N265_INTRA_RD_N` | 3 | src/encoder/ctu.c:4115 |
| `N265_INTRA_REUSE` | 1 | src/encoder/ctu.c:4210 |
| `N265_INTRA_RSKIP` | 0 | src/encoder/ctu.c:1791 |
| `N265_INTRA_TRY_TRACE` | presence | src/encoder/ctu.c:1389 |
| `N265_INTRA_WIN_TRACE` | presence | src/encoder/encoder.c:1785 |
| `N265_I_INTRA` | 0 | src/encoder/ctu.c:4142 |
| `N265_LA_MEPRED` | 1 | src/encoder/la.c:551 |
| `N265_LA_MVSTAT` | presence | src/encoder/la.c:988 |
| `N265_LA_PROF` | presence | src/encoder/encoder.c:2775 |
| `N265_LEVEL_TRACE` | presence | src/encoder/encoder.c:2525 |
| `N265_MAX_FRAME_WORK` | 6 with --bframes >= 2, else 4 | src/encoder/encoder.c:2549 |
| `N265_MCSTF_TRACE` | presence | src/encoder/mcstf.c:125 |
| `N265_MEREUSE_TRACE` | presence | src/encoder/encoder.c:1782 |
| `N265_MERGE_RD_N` | 5 | src/encoder/ctu.c:3209 |
| `N265_MERGE_TRACE` | -1 | src/encoder/mv.c:107 |
| `N265_ME_PATTERN` | 0 | src/encoder/mv.c:824 |
| `N265_ME_SATD` | 0 | src/encoder/ctu.c:2900 |
| `N265_MIN_KEYINT` | 4 | src/encoder/encoder.c:6834 |
| `N265_MODE_HIST` | presence | src/encoder/sao.c:253 |
| `N265_MVD_BITS` | 0 | src/encoder/ctu.c:2888 |
| `N265_NO_AMP` | 1 | src/encoder/encoder.c:1714 |
| `N265_NO_RECT` | 1 | src/encoder/encoder.c:1712 |
| `N265_NO_SAO` | presence | src/encoder/encoder.c:1779 |
| `N265_NO_SPLIT` | presence | src/encoder/ctu.c:184 |
| `N265_NR_INTER` | 0 | src/encoder/encoder.c:2055 |
| `N265_NR_INTRA` | 0 | src/encoder/encoder.c:2053 |
| `N265_OPENGOP_CUT` | 0 | src/encoder/encoder.c:833 |
| `N265_OPENGOP_NOBARRIER` | 0 | src/encoder/encoder.c:840 |
| `N265_PART_GATE` | 2 | src/encoder/ctu.c:2056 |
| `N265_PB_CASCADE` | auto | src/encoder/encoder.c:1660 |
| `N265_PB_CASCADE_FLAT` | auto | src/encoder/encoder.c:1669 |
| `N265_PSY` | 0.0 | src/encoder/encoder.c:1816 |
| `N265_QP_FRAC` | = --bitrate or CRF+frame term | src/encoder/encoder.c:2106 |
| `N265_QP_FRAC_FLAT` | 0 | src/encoder/encoder.c:2119 |
| `N265_QP_MAP_TRACE` | unset | src/encoder/encoder.c:5797 |
| `N265_QP_ROUND1` | 1 | src/encoder/encoder.c:1687 |
| `N265_QP_TDITHER` | 1 | src/encoder/encoder.c:2138 |
| `N265_QP_TRACE` | presence | src/encoder/encoder.c:5813 |
| `N265_RC_LAG` | presence | src/encoder/encoder.c:2635 |
| `N265_RC_TRACE` | presence | src/encoder/encoder.c:683 |
| `N265_RDOQ_ADAPT` | 0 | src/encoder/rdoq.c:294 |
| `N265_RDOQ_LAM` | 100 | src/encoder/rdoq.c:300 |
| `N265_RDOQ_LAT_SZ` | 0 | src/encoder/ctu.c:897 |
| `N265_RDOQ_TRACE` | presence | src/encoder/ctu.c:759 |
| `N265_RECT_FLAT` | 1 | src/encoder/ctu.c:2134 |
| `N265_RECT_RANK` | 50 | src/encoder/ctu.c:2099 |
| `N265_REF_LAG_HINT` | -1 | src/encoder/encoder.c:5843 |
| `N265_RSKIP` | 2 | src/encoder/ctu.c:1778 |
| `N265_SAO_ANIM` | 1 | src/encoder/sao.c:309 |
| `N265_SAO_BIAS` | 100 | src/encoder/sao.c:356 |
| `N265_SAO_CHROMA` | 1 | src/encoder/sao.c:512 |
| `N265_SAO_GATE` | 0 | src/encoder/sao.c:561 |
| `N265_SAO_QPLAM` | 0 | src/encoder/sao.c:346 |
| `N265_SAO_TRACE` | unset | src/encoder/sao.c:654 |
| `N265_SAO_TYPES` | 0 | src/encoder/sao.c:272 |
| `N265_SATD8` | 0 | src/encoder/satd.c:134 |
| `N265_SBH_RD` | 2 | src/encoder/rdoq.c:279 |
| `N265_SC_FORCE` | ? | src/encoder/la.c:485 |
| `N265_SC_HOLD` | 1 | src/encoder/la.c:241 |
| `N265_SC_TRACE` | presence | src/encoder/la.c:1137 |
| `N265_SERIAL_PROF` | 0 | src/encoder/encoder.c:2772 |
| `N265_SHOT_STATS` | unset | src/encoder/encoder.c:1804 |
| `N265_SHOT_TRACE` | presence | src/encoder/encoder.c:7110 |
| `N265_SHOT_WINDOW` | 0 | src/encoder/encoder.c:1956 |
| `N265_SKIP_EXIT` | 0 | src/encoder/ctu.c:1872 |
| `N265_SKIP_THR` | 2 | src/encoder/ctu.c:1677 |
| `N265_STALL_TRACE` | presence | src/encoder/encoder.c:3229 |
| `N265_SUBME` | 3 | src/encoder/mv.c:616 |
| `N265_SUBPEL_DEDUP` | 1 | src/encoder/mv.c:119 |
| `N265_SUBPEL_GATE` | 0 | src/encoder/mv.c:688 |
| `N265_SUBPEL_TRACE` | presence | src/encoder/ctu.c:3378 |
| `N265_TASK_TRACE` | unset | src/encoder/ttrace.c:31 |
| `N265_TREE_RAMP` | 4.0 | src/encoder/encoder.c:1914 |
| `N265_UHD_QP` | 29 | src/encoder/encoder.c:4624 |
| `N265_VBV_BRAKE` | 1 under --bitrate with a VBV, else 0 | src/encoder/rc.c:257 |
| `N265_VBV_BRAKE_FILL` | 0.9 | src/encoder/rc.c:261 |
| `N265_VBV_BRAKE_K0` | 0.05 | src/encoder/rc.c:258 |
| `N265_VBV_HRD` | 1 | src/encoder/encoder.c:2344 |
| `N265_VBV_SCPRED` | 0.5 under --bitrate, else 1.0 | src/encoder/rc.c:262 |
| `N265_VBV_TRACE` | presence | src/encoder/encoder.c:2343 |
| `N265_WEIGHTB` | 0 | src/encoder/encoder.c:2171 |
| `N265_WEIGHTP` | 0 | src/encoder/encoder.c:2169 |
| `N265_WP_GAIN` | 4 | src/encoder/wp.c:78 |
| `N265_WP_REFS` | 5 | src/encoder/wp.c:117 |
| `N265_WP_TRACE` | 0 | src/encoder/wp.c:51 |
| `N265_WP_WIN` | 40 | src/encoder/wp.c:101 |

## arm (29)

| knob | default | reader |
|---|---|---|
| `N265_ABR_IOFF_MAX` | 6.0 | src/encoder/rc.c:222 |
| `N265_AQ` | presence | src/encoder/encoder.c:1817 |
| `N265_AQ_FLAT` | presence | src/encoder/encoder.c:5364 |
| `N265_AQ_FLAT_THR` | = ungated | src/encoder/encoder.c:5692 |
| `N265_BADAPT_DEG` | 1.6 | src/encoder/encoder.c:7273 |
| `N265_CBQPOFFS` | 0 | src/encoder/encoder.c:2158 |
| `N265_CHROMA_W_PCT` | 100 | src/encoder/ctu.c:72 |
| `N265_CPRED_W` | 100 | src/encoder/ctu.c:2742 |
| `N265_CRF_LEGACY` | 0 | src/encoder/encoder.c:1565 |
| `N265_CRQPOFFS` | 0 | src/encoder/encoder.c:2160 |
| `N265_CT_LAMBDA` | 0.0 | src/encoder/encoder.c:1611 |
| `N265_EST_MODE` | 0 | src/encoder/est.c:24 |
| `N265_INTRA_B_MODES` | 0 | src/encoder/ctu.c:1353 |
| `N265_MEREUSE` | 0 | src/encoder/encoder.c:1783 |
| `N265_NO_AMVP` | presence | src/encoder/ctu.c:178 |
| `N265_NO_EARLYSKIP` | presence | src/encoder/ctu.c:190 |
| `N265_NO_GATES` | presence | src/encoder/ctu.c:172 |
| `N265_NO_TMVP` | presence | src/encoder/encoder.c:3459 |
| `N265_OLD_BS` | presence | src/encoder/deblock.c:158 |
| `N265_RD_PROXY` | 0 | src/encoder/ctu.c:5303 |
| `N265_RUNG_GATE` | -2 | src/encoder/mv.c:745 |
| `N265_RUNG_RERANK` | 0 | src/encoder/mv.c:761 |
| `N265_SC_CONJ` | 3 | src/encoder/ctu.c:1737 |
| `N265_SHOT_CLAMP` | 2.0 | src/encoder/encoder.c:1975 |
| `N265_SHOT_EMA` | 0.5 | src/encoder/encoder.c:1980 |
| `N265_SHOT_K` | 1.0 | src/encoder/encoder.c:1974 |
| `N265_STATIC_COMMIT` | 0 | src/encoder/ctu.c:1713 |
| `N265_SUBME_RUNG` | 0 | src/encoder/mv.c:731 |
| `N265_SUBPEL_RATE` | 0 | src/encoder/mv.c:644 |
