# The N265_* knob census (generated -- edit scripts/knob_census.py OVERRIDES, not this file)

Regenerate: `python3 scripts/knob_census.py`. CI check: `--check`.

Tier meanings: **default** = shipped behaviour, the env is its escape; **instrument** = measurement machinery (catalogued in docs/instruments.md when it has produced a result); **arm** = default-off gate or kept refuted arm -- kept deliberately because a refuted arm retains its re-pricing value.

In the default column, **presence** means the reader is a boolean: unset is off and any value arms it. **unset** means the value names a file or a string and unset is off. `= N265_X` means the default is another knob's resolved value.

## default (65)

| knob | default | reader |
|---|---|---|
| `N265_ABR_SB` | 1.7 | src/encoder/rc.c:286 |
| `N265_ABR_SBR` | 4.0 | src/encoder/rc.c:285 |
| `N265_ABR_SP` | 13.3 | src/encoder/rc.c:284 |
| `N265_BADAPT_MVCAP` | 4 | src/encoder/la.c:343 |
| `N265_BADAPT_MVGATE` | 20 | src/encoder/la.c:378 |
| `N265_CLAIM_PRIO` | 1 | src/encoder/encoder.c:6011 |
| `N265_CRF_DC_MEM` | 1000.0 | src/encoder/encoder.c:1602 |
| `N265_CRF_DUR` | 0.3 | src/encoder/encoder.c:1572 |
| `N265_CRF_DUR_FPS` | 25.0 | src/encoder/encoder.c:1573 |
| `N265_CT_MVRATE` | 16 | src/encoder/la.c:419 |
| `N265_FGC_CHROMA` | 1 | src/encoder/grain.c:133 |
| `N265_FGC_CORE` | 1.0 | src/encoder/grain.c:143 |
| `N265_FGC_FRAMES` | 1 | src/encoder/grain.c:121 |
| `N265_FGC_MIN` | 1.0 | src/encoder/grain.c:181 |
| `N265_FGC_PCT` | 25 | src/encoder/grain.c:109 |
| `N265_FGC_RATIO` | 2.0 | src/encoder/grain.c:167 |
| `N265_GATE_SCALE` | 100 | src/encoder/ctu.c:1837 |
| `N265_LAMBDA` | 0.68 | src/encoder/ctu.c:1634 |
| `N265_MCSTF_CLEAN` | 1.5 | src/encoder/mcstf.c:98 |
| `N265_MCSTF_CUT` | 3.0 | src/encoder/mcstf.c:257 |
| `N265_MCSTF_MODEL` | 2 | src/encoder/mcstf.c:241 |
| `N265_MCSTF_PERIOD` | 8 | src/encoder/mcstf.c:115 |
| `N265_MCSTF_PICCAP` | 2.0 | src/encoder/mcstf.c:249 |
| `N265_MCSTF_QPREF` | 32.0 | src/encoder/mcstf.c:264 |
| `N265_MCSTF_QPSLOPE` | 12.0 | src/encoder/mcstf.c:265 |
| `N265_MCSTF_RHO0` | 2.0 | src/encoder/mcstf.c:251 |
| `N265_MCSTF_RHO1` | 2.0 | src/encoder/mcstf.c:253 |
| `N265_MCSTF_S0` | 1.0 | src/encoder/mcstf.c:247 |
| `N265_MCSTF_SHARP` | 2.0 | src/encoder/mcstf.c:243 |
| `N265_MCSTF_SIGMA` | 6.0 | src/encoder/mcstf.c:39 |
| `N265_MCSTF_SMIN` | 0.25 | src/encoder/mcstf.c:245 |
| `N265_MCSTF_STR` | 1.0 | src/encoder/mcstf.c:261 |
| `N265_MCSTF_SUBPEL` | 2.0 | src/encoder/mcstf.c:255 |
| `N265_MERGE_PICK_RATE` | 0 | src/encoder/ctu.c:5133 |
| `N265_ME_PATTERN` | 0 | src/encoder/mv.c:824 |
| `N265_PB_STEP` | 1.0 | src/encoder/encoder.c:4763 |
| `N265_RC_MVRATE` | 32 | src/encoder/rc.c:136 |
| `N265_RDOQ` | 1 | src/encoder/encoder.c:1818 |
| `N265_RDOQ_UP` | 1 | src/encoder/rdoq.c:288 |
| `N265_ROW_YIELD` | 5000 where 2 * min(rows, (cols + 1) / 2) > threads > 1, else 0 | src/encoder/encoder.c:2714 |
| `N265_SC_FAILW` | 0.40 | src/encoder/la.c:459 |
| `N265_SC_FAILW_SPIKE` | 1.6 | src/encoder/la.c:476 |
| `N265_SC_JUMP` | 0.10 | src/encoder/la.c:506 |
| `N265_SC_SPIKE` | 1.8 | src/encoder/la.c:590 |
| `N265_TREE_K` | 12 | src/encoder/encoder.c:2656 |
| `N265_TREE_SLOTS` | K - 6 (with the switched HD or UHD tree only) | src/encoder/encoder.c:2721 |
| `N265_VBR_CTKEEP` | 1 under capped VBR, else 0 | src/encoder/rc.c:249 |
| `N265_VBR_FBG` | 16.0 | src/encoder/rc.c:246 |
| `N265_VBR_FBGUP` | 12.0 | src/encoder/rc.c:247 |
| `N265_VBR_FBT0` | 2.0 | src/encoder/rc.c:245 |
| `N265_VBR_FBUP` | = N265_ABR_FBMAX (capped VBR only) | src/encoder/rc.c:216 |
| `N265_VBR_FBW` | 2.0 under capped VBR, else 0 | src/encoder/rc.c:244 |
| `N265_VBR_LAG` | 7 (VBR, threads > 1) | src/encoder/encoder.c:2675 |
| `N265_VBR_LAG_BUFS` | 1.2 | src/encoder/encoder.c:2684 |
| `N265_VBR_LAG_CAL` | 0.1 | src/encoder/encoder.c:2737 |
| `N265_VBR_LAG_LOW` | 0.5 | src/encoder/encoder.c:2743 |
| `N265_VBR_LAG_PEND` | 1 | src/encoder/encoder.c:2760 |
| `N265_VBR_LAG_POSTI_RAISE` | 3.0 | src/encoder/encoder.c:2755 |
| `N265_VBR_LAG_WARM` | 2 * (lag + 1) | src/encoder/encoder.c:2735 |
| `N265_VBR_OVW` | 0.75 under capped VBR, else 0 | src/encoder/rc.c:248 |
| `N265_VBV_DECAY` | 0.0625 | src/encoder/rc.c:296 |
| `N265_VBV_LOOKAHEAD` | 1 | src/encoder/encoder.c:2291 |
| `N265_VBV_OWN` | 3 under capped VBR, else 0 | src/encoder/rc.c:250 |
| `N265_VBV_RESERVE` | 0.2 | src/encoder/encoder.c:2298 |
| `N265_VBV_SAFE` | 0.85 | src/encoder/encoder.c:2284 |

## instrument (199)

| knob | default | reader |
|---|---|---|
| `N265_2P_CLAMP` | 6.0 | src/encoder/encoder.c:672 |
| `N265_2P_GAIN` | 0.25 | src/encoder/rc.c:274 |
| `N265_2P_QCOMP` | = N265_ABR_QCOMP | src/encoder/encoder.c:670 |
| `N265_2P_TRACE` | presence | src/encoder/encoder.c:687 |
| `N265_2P_TRUST` | 2.0 | src/encoder/encoder.c:679 |
| `N265_ABR_CMEM` | 1000.0 | src/encoder/rc.c:254 |
| `N265_ABR_COPEN` | 1 | src/encoder/rc.c:257 |
| `N265_ABR_CSEED` | 0.0 | src/encoder/rc.c:256 |
| `N265_ABR_CTF` | 1 | src/encoder/rc.c:261 |
| `N265_ABR_FBMAX` | 6.0 | src/encoder/rc.c:181 |
| `N265_ABR_FBSTEP` | 1 | src/encoder/rc.c:259 |
| `N265_ABR_FBUP` | = N265_VBR_FBUP under capped VBR (item 37), 1.5 under another VBV, else = N265_ABR_FBMAX | src/encoder/rc.c:215 |
| `N265_ABR_FLATC` | 17.0 under a VBV, else 0 | src/encoder/rc.c:302 |
| `N265_ABR_HORIZON` | 6.0 | src/encoder/rc.c:173 |
| `N265_ABR_IBRAKE` | 1 | src/encoder/rc.c:258 |
| `N265_ABR_IGAIN` | 0.085 | src/encoder/rc.c:179 |
| `N265_ABR_IMAX` | 8.0 | src/encoder/rc.c:180 |
| `N265_ABR_IOFF` | 0.0 | src/encoder/rc.c:262 |
| `N265_ABR_IOFF_CT` | 1.0 under a VBV without N265_VBR_CTKEEP, else 0 | src/encoder/rc.c:263 |
| `N265_ABR_LVL` | 2.0 | src/encoder/rc.c:287 |
| `N265_ABR_MEAS` | 1 | src/encoder/rc.c:252 |
| `N265_ABR_MGUARD` | 0.0 | src/encoder/rc.c:253 |
| `N265_ABR_OPENI` | 0 | src/encoder/rc.c:260 |
| `N265_ABR_OPENSTEP` | 6.0 | src/encoder/rc.c:178 |
| `N265_ABR_PLANERR` | 0 | src/encoder/rc.c:218 |
| `N265_ABR_QCOMP` | 0.6 | src/encoder/rc.c:172 |
| `N265_ABR_SI` | 25.4 | src/encoder/rc.c:283 |
| `N265_ABR_SMEM` | 16.0 | src/encoder/rc.c:177 |
| `N265_ABR_STEP` | 1.0 | src/encoder/rc.c:176 |
| `N265_ABR_TCAP` | 1 | src/encoder/rc.c:255 |
| `N265_ABR_WMEM` | 1000.0 | src/encoder/rc.c:251 |
| `N265_API_DEFER` | presence | src/encoder/encoder.c:2831 |
| `N265_AQ_ANCHOR` | 6.4 | src/encoder/encoder.c:5645 |
| `N265_AQ_CLAMP` | 3.0 | src/encoder/encoder.c:5728 |
| `N265_AQ_CLAMP_FLAT` | 0.0 | src/encoder/encoder.c:5758 |
| `N265_AQ_DC` | ? | src/encoder/encoder.c:5645 |
| `N265_AQ_FLAT_SIGN` | 0 | src/encoder/encoder.c:1690 |
| `N265_AQ_FLOOR` | 4 | src/encoder/encoder.c:5530 |
| `N265_AQ_POOL` | 1 | src/encoder/encoder.c:2161 |
| `N265_AQ_SIGNAL` | 1 | src/encoder/encoder.c:1689 |
| `N265_AQ_STR` | 0.7 | src/encoder/encoder.c:1683 |
| `N265_AQ_TRACE` | presence | src/encoder/encoder.c:5809 |
| `N265_ASM_OFF` | unset | src/common/cpu.c:110 |
| `N265_BADAPT_REFW` | 128.0 | src/encoder/encoder.c:7428 |
| `N265_BADAPT_THR` | 0.35 | src/encoder/encoder.c:7427 |
| `N265_BADAPT_TRACE` | presence | src/encoder/la.c:1371 |
| `N265_BI_GATE` | 4 | src/encoder/ctu.c:3238 |
| `N265_BI_REFINE` | 0 | src/encoder/ctu.c:3190 |
| `N265_BRUN_TRACE` | presence | src/encoder/la.c:626 |
| `N265_B_INTRA_MARGIN` | 30 | src/encoder/ctu.c:3172 |
| `N265_CB64_AMVP` | 0 | src/encoder/ctu.c:3157 |
| `N265_CB64_GATE` | 1 | src/encoder/ctu.c:3139 |
| `N265_CB64_PROF` | presence | src/encoder/ctu.c:3110 |
| `N265_CB64_SKIPGATE` | 0 | src/encoder/ctu.c:2985 |
| `N265_CB64_SPLITGATE` | 0 | src/encoder/ctu.c:3008 |
| `N265_CB64_TRACE` | presence | src/encoder/ctu.c:196 |
| `N265_CBF_FAST` | 2 | src/encoder/ctu.c:1821 |
| `N265_CBIT_TRACE` | presence | src/encoder/encoder.c:1788 |
| `N265_CHROMA_PRED` | 1 | src/encoder/ctu.c:131 |
| `N265_CHROMA_W` | 0 | src/encoder/ctu.c:66 |
| `N265_CPRED_GATE` | 2 | src/encoder/ctu.c:2769 |
| `N265_CPRED_TRACE` | presence | src/encoder/ctu.c:4941 |
| `N265_CRF_DC_ANCHOR` | 3 | src/encoder/encoder.c:1598 |
| `N265_CRF_DC_WIN` | 12.0 | src/encoder/encoder.c:1604 |
| `N265_CRF_SHIFT` | 5.4 | src/encoder/encoder.c:4746 |
| `N265_CT_CENTRE` | 0 | src/encoder/la.c:1773 |
| `N265_CT_FRAMEW` | 1.0 | src/encoder/encoder.c:1622 |
| `N265_CT_FRAMEW_FLAT` | = N265_CT_QPW | src/encoder/encoder.c:1626 |
| `N265_CT_FRAME_CLAMP` | 0 | src/encoder/encoder.c:1630 |
| `N265_CT_FRAME_LEVEL` | 1 | src/encoder/la.c:1822 |
| `N265_CT_FRAME_SHOT` | 0 | src/encoder/encoder.c:1632 |
| `N265_CT_FRAME_UP` | 0 | src/encoder/encoder.c:1640 |
| `N265_CT_QPW` | 2.0 | src/encoder/encoder.c:1614 |
| `N265_CT_TRACE` | presence | src/encoder/la.c:1920 |
| `N265_CT_UP_GATE` | 0.24 | src/encoder/encoder.c:1658 |
| `N265_CUTREE_STR` | 2.0 | src/encoder/encoder.c:1613 |
| `N265_CU_STATS` | presence | src/encoder/encoder.c:1784 |
| `N265_CU_TRACE` | unset | src/encoder/encoder.c:1314 |
| `N265_DBF_BETA` | 0 | src/encoder/encoder.c:2181 |
| `N265_DBF_TC` | 0 | src/encoder/encoder.c:2182 |
| `N265_DECGRAD_TRACE` | presence | src/encoder/rdoq.c:353 |
| `N265_DECIMATE` | 15 | src/encoder/rdoq.c:306 |
| `N265_DECIMATE_GRAD` | = N265_DECIMATE | src/encoder/rdoq.c:320 |
| `N265_DEPTH_BOUND` | 0 | src/encoder/ctu.c:2182 |
| `N265_DEPTH_DUMP` | unset | src/encoder/encoder.c:1301 |
| `N265_DPB_HOLD` | 0 | src/encoder/encoder.c:772 |
| `N265_DPB_TRACE` | presence | src/encoder/encoder.c:1762 |
| `N265_DUP_FAST` | 0 | src/encoder/la.c:636 |
| `N265_DUP_THR` | 0.0 | src/encoder/la.c:674 |
| `N265_DUP_TRACE` | presence | src/encoder/la.c:646 |
| `N265_ES_THR` | 100 | src/encoder/la.c:1494 |
| `N265_FGC_TRACE` | presence | src/encoder/grain.c:192 |
| `N265_FLAT_GATE` | 10 | src/encoder/la.c:102 |
| `N265_FLAT_TRACE` | presence | src/encoder/encoder.c:5452 |
| `N265_FORCE_NONIDR` | -1 | src/encoder/encoder.c:7048 |
| `N265_FP_CENSUS` | presence | src/encoder/encoder.c:1785 |
| `N265_FULLPEL` | presence | src/encoder/mv.c:113 |
| `N265_HD_QP` | 27 | src/encoder/encoder.c:4722 |
| `N265_HF2` | 3 | src/encoder/encoder.c:3655 |
| `N265_HF2_TPP` | 150 | src/encoder/encoder.c:3683 |
| `N265_HF2_TPP14` | 190 | src/encoder/encoder.c:3694 |
| `N265_HF2_TRACE` | presence | src/encoder/encoder.c:3706 |
| `N265_HFCACHE` | 1 | src/encoder/encoder.c:3591 |
| `N265_INTER_NXN` | 0 | src/encoder/ctu.c:4156 |
| `N265_INTRA_ADAPT_K` | 32 | src/encoder/encoder.c:1796 |
| `N265_INTRA_ADAPT_THR` | 100 | src/encoder/encoder.c:1799 |
| `N265_INTRA_GATE` | 7 | src/encoder/ctu.c:1974 |
| `N265_INTRA_MIN` | 4 | src/encoder/encoder.c:7037 |
| `N265_INTRA_MPM` | 0 | src/encoder/ctu.c:4126 |
| `N265_INTRA_ORACLE` | 0 | src/encoder/ctu.c:2003 |
| `N265_INTRA_RD_N` | 3 | src/encoder/ctu.c:4115 |
| `N265_INTRA_REUSE` | 1 | src/encoder/ctu.c:4210 |
| `N265_INTRA_RSKIP` | 0 | src/encoder/ctu.c:1791 |
| `N265_INTRA_TRY_TRACE` | presence | src/encoder/ctu.c:1389 |
| `N265_INTRA_WIN_TRACE` | presence | src/encoder/encoder.c:1789 |
| `N265_I_INTRA` | 0 | src/encoder/ctu.c:4142 |
| `N265_LA_MEPRED` | 1 | src/encoder/la.c:611 |
| `N265_LA_MVSTAT` | presence | src/encoder/la.c:1053 |
| `N265_LA_PROF` | presence | src/encoder/encoder.c:2836 |
| `N265_LEVEL_TRACE` | presence | src/encoder/encoder.c:2564 |
| `N265_MAX_FRAME_WORK` | 6 with --bframes >= 2, else 4 | src/encoder/encoder.c:2588 |
| `N265_MCSTF_TRACE` | presence | src/encoder/mcstf.c:126 |
| `N265_MEREUSE_TRACE` | presence | src/encoder/encoder.c:1786 |
| `N265_MERGE_RD_N` | 5 | src/encoder/ctu.c:3209 |
| `N265_MERGE_TRACE` | -1 | src/encoder/mv.c:107 |
| `N265_ME_SATD` | 0 | src/encoder/ctu.c:2900 |
| `N265_MIN_KEYINT` | 4 | src/encoder/encoder.c:7019 |
| `N265_MODE_HIST` | presence | src/encoder/sao.c:253 |
| `N265_MVD_BITS` | 0 | src/encoder/ctu.c:2888 |
| `N265_NO_AMP` | 1 | src/encoder/encoder.c:1718 |
| `N265_NO_RECT` | 1 | src/encoder/encoder.c:1716 |
| `N265_NO_SAO` | presence | src/encoder/encoder.c:1783 |
| `N265_NO_SPLIT` | presence | src/encoder/ctu.c:184 |
| `N265_NR_INTER` | 0 | src/encoder/encoder.c:2091 |
| `N265_NR_INTRA` | 0 | src/encoder/encoder.c:2089 |
| `N265_OPENGOP_CUT` | 0 | src/encoder/encoder.c:837 |
| `N265_OPENGOP_NOBARRIER` | 0 | src/encoder/encoder.c:844 |
| `N265_PART_GATE` | 2 | src/encoder/ctu.c:2056 |
| `N265_PB_CASCADE` | auto | src/encoder/encoder.c:1664 |
| `N265_PB_CASCADE_FLAT` | auto | src/encoder/encoder.c:1673 |
| `N265_PSY` | 0.0 | src/encoder/encoder.c:1820 |
| `N265_QP_FRAC` | = --bitrate or CRF+frame term | src/encoder/encoder.c:2142 |
| `N265_QP_FRAC_FLAT` | 0 | src/encoder/encoder.c:2155 |
| `N265_QP_MAP_TRACE` | unset | src/encoder/encoder.c:5936 |
| `N265_QP_ROUND1` | 1 | src/encoder/encoder.c:1691 |
| `N265_QP_TDITHER` | 1 | src/encoder/encoder.c:2174 |
| `N265_QP_TRACE` | presence | src/encoder/encoder.c:5959 |
| `N265_RC_LAG` | presence | src/encoder/encoder.c:2674 |
| `N265_RC_TRACE` | presence | src/encoder/encoder.c:687 |
| `N265_RDOQ_ADAPT` | 0 | src/encoder/rdoq.c:294 |
| `N265_RDOQ_LAM` | 100 | src/encoder/rdoq.c:300 |
| `N265_RDOQ_LAT_SZ` | 0 | src/encoder/ctu.c:897 |
| `N265_RDOQ_TRACE` | presence | src/encoder/ctu.c:759 |
| `N265_RECT_FLAT` | 1 | src/encoder/ctu.c:2134 |
| `N265_RECT_RANK` | 50 | src/encoder/ctu.c:2099 |
| `N265_REF_LAG_HINT` | -1 | src/encoder/encoder.c:5989 |
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
| `N265_SC_FORCE` | ? | src/encoder/la.c:545 |
| `N265_SC_HOLD` | 1 | src/encoder/la.c:243 |
| `N265_SC_TRACE` | presence | src/encoder/la.c:1216 |
| `N265_SERIAL_PROF` | 0 | src/encoder/encoder.c:2833 |
| `N265_SHOT_STATS` | unset | src/encoder/encoder.c:1808 |
| `N265_SHOT_TRACE` | presence | src/encoder/encoder.c:7296 |
| `N265_SHOT_WINDOW` | 0 | src/encoder/encoder.c:1960 |
| `N265_SKIP_EXIT` | 0 | src/encoder/ctu.c:1872 |
| `N265_SKIP_THR` | 2 | src/encoder/ctu.c:1677 |
| `N265_STALL_TRACE` | presence | src/encoder/encoder.c:3307 |
| `N265_SUBME` | 3 | src/encoder/mv.c:616 |
| `N265_SUBPEL_DEDUP` | 1 | src/encoder/mv.c:119 |
| `N265_SUBPEL_GATE` | 0 | src/encoder/mv.c:688 |
| `N265_SUBPEL_TRACE` | presence | src/encoder/ctu.c:3378 |
| `N265_SZ_TRACE` | presence | src/encoder/encoder.c:2380 |
| `N265_TASK_TRACE` | unset | src/encoder/ttrace.c:31 |
| `N265_TREE_RAMP` | 4.0 | src/encoder/encoder.c:1918 |
| `N265_UHD_QP` | 29 | src/encoder/encoder.c:4702 |
| `N265_VBR_OFFB` | 0.0 | src/encoder/encoder.c:2381 |
| `N265_VBR_OPEN` | 0 | src/encoder/rc.c:336 |
| `N265_VBR_SZM` | 0 | src/encoder/rc.c:316 |
| `N265_VBV_BRAKE` | 1 under --bitrate with a VBV, else 0 | src/encoder/rc.c:300 |
| `N265_VBV_BRAKE_FILL` | 0.9 | src/encoder/rc.c:304 |
| `N265_VBV_BRAKE_K0` | 0.05 | src/encoder/rc.c:301 |
| `N265_VBV_HRD` | 1 | src/encoder/encoder.c:2383 |
| `N265_VBV_SCPRED` | 0.5 under --bitrate, else 1.0 | src/encoder/rc.c:305 |
| `N265_VBV_TRACE` | presence | src/encoder/encoder.c:2379 |
| `N265_WEIGHTB` | 0 | src/encoder/encoder.c:2207 |
| `N265_WEIGHTP` | 0 | src/encoder/encoder.c:2205 |
| `N265_WP_GAIN` | 4 | src/encoder/wp.c:78 |
| `N265_WP_REFS` | 5 | src/encoder/wp.c:117 |
| `N265_WP_TRACE` | 0 | src/encoder/wp.c:51 |
| `N265_WP_WIN` | 40 | src/encoder/wp.c:101 |

## arm (56)

| knob | default | reader |
|---|---|---|
| `N265_ABR_IOFF_MAX` | 6.0 | src/encoder/rc.c:265 |
| `N265_AQ` | presence | src/encoder/encoder.c:1821 |
| `N265_AQ_FLAT` | presence | src/encoder/encoder.c:5469 |
| `N265_AQ_FLAT_THR` | = ungated | src/encoder/encoder.c:5797 |
| `N265_BADAPT_DEG` | 1.6 | src/encoder/encoder.c:7459 |
| `N265_CBQPOFFS` | 0 | src/encoder/encoder.c:2194 |
| `N265_CHROMA_W_PCT` | 100 | src/encoder/ctu.c:72 |
| `N265_CPRED_W` | 100 | src/encoder/ctu.c:2742 |
| `N265_CRF_LEGACY` | 0 | src/encoder/encoder.c:1569 |
| `N265_CRQPOFFS` | 0 | src/encoder/encoder.c:2196 |
| `N265_CT_LAMBDA` | 0.0 | src/encoder/encoder.c:1615 |
| `N265_EST_MODE` | 0 | src/encoder/est.c:24 |
| `N265_INTRA_B_MODES` | 0 | src/encoder/ctu.c:1353 |
| `N265_MCSTF` | 0 | src/encoder/encoder.c:2030 |
| `N265_MCSTF_CTF` | 1.0 | src/encoder/encoder.c:2047 |
| `N265_MCSTF_CTSC` | 1.0 | src/encoder/encoder.c:2049 |
| `N265_MCSTF_DC` | 0 | src/encoder/mcstf.c:259 |
| `N265_MCSTF_QPB` | 0.0 | src/encoder/encoder.c:2043 |
| `N265_MCSTF_QPB_I` | 0 | src/encoder/encoder.c:2045 |
| `N265_MCSTF_RANGE` | 2 | src/encoder/encoder.c:2035 |
| `N265_MEREUSE` | 0 | src/encoder/encoder.c:1787 |
| `N265_NO_AMVP` | presence | src/encoder/ctu.c:178 |
| `N265_NO_EARLYSKIP` | presence | src/encoder/ctu.c:190 |
| `N265_NO_GATES` | presence | src/encoder/ctu.c:172 |
| `N265_NO_TMVP` | presence | src/encoder/encoder.c:3537 |
| `N265_OLD_BS` | presence | src/encoder/deblock.c:158 |
| `N265_RD_PROXY` | 0 | src/encoder/ctu.c:5303 |
| `N265_RUNG_GATE` | -2 | src/encoder/mv.c:745 |
| `N265_RUNG_RERANK` | 0 | src/encoder/mv.c:761 |
| `N265_SC_CONJ` | 3 | src/encoder/ctu.c:1737 |
| `N265_SC_FLOOR` | 0.0 | src/encoder/la.c:513 |
| `N265_SHOT_CLAMP` | 2.0 | src/encoder/encoder.c:1979 |
| `N265_SHOT_EMA` | 0.5 | src/encoder/encoder.c:1984 |
| `N265_SHOT_K` | 1.0 | src/encoder/encoder.c:1978 |
| `N265_STATIC_COMMIT` | 0 | src/encoder/ctu.c:1713 |
| `N265_SUBME_RUNG` | 0 | src/encoder/mv.c:731 |
| `N265_SUBPEL_RATE` | 0 | src/encoder/mv.c:644 |
| `N265_VBR_LAG_GFLOOR` | 0.0 | src/encoder/encoder.c:2747 |
| `N265_VBR_LAG_GRAW` | 0 | src/encoder/encoder.c:2749 |
| `N265_VBR_LAG_GUARD` | 0.0 | src/encoder/encoder.c:2745 |
| `N265_VBR_LAG_POSTI` | 0 | src/encoder/encoder.c:2753 |
| `N265_VBR_OPEN_AB` | -1.238 | src/encoder/rc.c:340 |
| `N265_VBR_OPEN_ABR` | -0.806 | src/encoder/rc.c:339 |
| `N265_VBR_OPEN_AI` | -7.027 | src/encoder/rc.c:337 |
| `N265_VBR_OPEN_AP` | 2.084 | src/encoder/rc.c:338 |
| `N265_VBR_OPEN_BB` | 1.224 | src/encoder/rc.c:344 |
| `N265_VBR_OPEN_BBR` | 1.374 | src/encoder/rc.c:343 |
| `N265_VBR_OPEN_BI` | 2.157 | src/encoder/rc.c:341 |
| `N265_VBR_OPEN_BP` | 1.249 | src/encoder/rc.c:342 |
| `N265_VBR_OPEN_G` | -1.429 | src/encoder/rc.c:345 |
| `N265_VBR_SZM_GMIN` | 0.7 | src/encoder/rc.c:324 |
| `N265_VBR_SZM_LAM` | 0.9 | src/encoder/rc.c:317 |
| `N265_VBR_SZM_SIMPLE` | 150.0 | src/encoder/rc.c:326 |
| `N265_VBR_SZM_STATIC` | 17.0 | src/encoder/rc.c:325 |
| `N265_VBR_SZM_Z` | 2.0 | src/encoder/rc.c:318 |
| `N265_VBR_SZM_ZLAG` | 1.0 | src/encoder/rc.c:323 |
