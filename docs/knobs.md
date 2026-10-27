# The N265_* knob census (generated -- edit scripts/knob_census.py OVERRIDES, not this file)

Regenerate: `python3 scripts/knob_census.py`. CI check: `--check`.

Tier meanings: **default** = shipped behaviour, the env is its escape; **instrument** = measurement machinery (catalogued in docs/instruments.md when it has produced a result); **arm** = default-off gate or kept refuted arm -- kept deliberately because a refuted arm retains its re-pricing value.

In the default column, **presence** means the reader is a boolean: unset is off and any value arms it. **unset** means the value names a file or a string and unset is off. `= N265_X` means the default is another knob's resolved value.

## default (106)

| knob | default | reader |
|---|---|---|
| `N265_ABR_SB` | 1.7 | src/encoder/rc.c:359 |
| `N265_ABR_SBR` | 4.0 | src/encoder/rc.c:358 |
| `N265_ABR_SP` | 13.3 | src/encoder/rc.c:357 |
| `N265_AQ2_ABR` | 1 | src/encoder/encoder.c:1864 |
| `N265_AQ2_GMAX` | 1.0 | src/encoder/encoder.c:1869 |
| `N265_AQ2_GMIN` | 0.7 | src/encoder/encoder.c:1868 |
| `N265_AQ2_SPOW` | 1.0 | src/encoder/encoder.c:1867 |
| `N265_AQ2_SREF` | 2.5 | src/encoder/encoder.c:1865 |
| `N265_BADAPT_MVCAP` | 4 | src/encoder/la.c:343 |
| `N265_BADAPT_MVGATE` | 20 | src/encoder/la.c:378 |
| `N265_CB64_SKIPGATE_REF` | -1 | src/encoder/ctu.c:3302 |
| `N265_CCRF_BRAKE` | 1 under capped CRF, else unread | src/encoder/rc.c:393 |
| `N265_CCRF_BRAKE_FILL` | 0.8 under capped CRF, else unread | src/encoder/rc.c:407 |
| `N265_CCRF_BRK_R0` | 1.25 | src/encoder/rc.c:394 |
| `N265_CCRF_GUARD` | 1.3 under capped CRF, else 0 | src/encoder/rc.c:401 |
| `N265_CCRF_SKIP` | 1 under capped CRF, else 0 | src/encoder/rc.c:396 |
| `N265_CCRF_SKIP_FILL` | 0.8 | src/encoder/rc.c:397 |
| `N265_CCRF_SKIP_SATQ` | 51.0 | src/encoder/rc.c:398 |
| `N265_CCRF_SKIP_WARM` | 4.0 | src/encoder/rc.c:399 |
| `N265_CLAIM_PRIO` | 1 | src/encoder/encoder.c:6408 |
| `N265_CRF_DC_MEM` | 1000.0 | src/encoder/encoder.c:1770 |
| `N265_CRF_DUR` | 0.3 | src/encoder/encoder.c:1740 |
| `N265_CRF_DUR_FPS` | 25.0 | src/encoder/encoder.c:1741 |
| `N265_CT_MVRATE` | 16 | src/encoder/la.c:419 |
| `N265_FGC_CHROMA` | 1 | src/encoder/grain.c:133 |
| `N265_FGC_CORE` | 1.0 | src/encoder/grain.c:143 |
| `N265_FGC_FRAMES` | 1 | src/encoder/grain.c:121 |
| `N265_FGC_MIN` | 1.0 | src/encoder/grain.c:181 |
| `N265_FGC_PCT` | 25 | src/encoder/grain.c:109 |
| `N265_FGC_RATIO` | 2.0 | src/encoder/grain.c:167 |
| `N265_GATE_SCALE` | 100 | src/encoder/ctu.c:2105 |
| `N265_INTRA_FAST` | 0 | src/encoder/ctu.c:1564 |
| `N265_INTRA_FAST_W` | 1280 | src/encoder/ctu.c:1602 |
| `N265_KEY_RESET` | 1 | src/encoder/encoder.c:7495 |
| `N265_LAMBDA` | 0.68 | src/encoder/ctu.c:1858 |
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
| `N265_MERGE_PICK_RATE` | 0 | src/encoder/ctu.c:5550 |
| `N265_ME_PATTERN` | 0 | src/encoder/mv.c:878 |
| `N265_PB_STEP` | 1.0 | src/encoder/encoder.c:5004 |
| `N265_PSY_DARK_M` | 64.0 | src/encoder/encoder.c:2023 |
| `N265_PSY_DARK_QHI` | 40.0 | src/encoder/encoder.c:2021 |
| `N265_PSY_DARK_QW` | 12.0 | src/encoder/encoder.c:2022 |
| `N265_PSY_DARK_V` | 16.0 | src/encoder/encoder.c:2024 |
| `N265_RC_MVRATE` | 32 | src/encoder/rc.c:136 |
| `N265_RDOQ` | 1 | src/encoder/encoder.c:2002 |
| `N265_RDOQ_UP` | 1 | src/encoder/rdoq.c:291 |
| `N265_ROW_YIELD` | 5000 where 2 * min(rows, (cols + 1) / 2) > threads > 1, else 0 | src/encoder/encoder.c:2931 |
| `N265_SC_FAILW` | 0.40 | src/encoder/la.c:459 |
| `N265_SC_FAILW_SPIKE` | 1.6 | src/encoder/la.c:476 |
| `N265_SC_JUMP` | 0.10 | src/encoder/la.c:506 |
| `N265_SC_SPIKE` | 1.8 | src/encoder/la.c:590 |
| `N265_TREE_K` | 12 | src/encoder/encoder.c:2861 |
| `N265_TREE_SLOTS` | K - 6 (with the switched HD or UHD tree only) | src/encoder/encoder.c:2938 |
| `N265_TT_SKIP_INTER` | 0 | src/encoder/ctu.c:2029 |
| `N265_VBR_CMEM` | 1 | src/encoder/rc.c:326 |
| `N265_VBR_CTKEEP` | 1 under capped VBR, else 0 | src/encoder/rc.c:249 |
| `N265_VBR_DEFPT` | 0.08 | src/encoder/rc.c:285 |
| `N265_VBR_DEFPT_CAP` | 6.0 | src/encoder/rc.c:292 |
| `N265_VBR_DEFPT_HOLD` | 2.0 | src/encoder/rc.c:295 |
| `N265_VBR_DEFPT_MEM` | 1000.0 | src/encoder/rc.c:299 |
| `N265_VBR_DEFPT_RAIL` | 1 | src/encoder/rc.c:289 |
| `N265_VBR_DEFPT_REL` | 0.05 | src/encoder/rc.c:287 |
| `N265_VBR_DEFPT_SLEW` | 0.25 | src/encoder/rc.c:291 |
| `N265_VBR_DEFPT_T0` | 60.0 | src/encoder/rc.c:290 |
| `N265_VBR_DEFPT_W` | 0.02 | src/encoder/rc.c:286 |
| `N265_VBR_FBFINE` | 3.0 | src/encoder/rc.c:273 |
| `N265_VBR_FBG` | 16.0 | src/encoder/rc.c:246 |
| `N265_VBR_FBGUP` | 12.0 | src/encoder/rc.c:247 |
| `N265_VBR_FBT0` | 2.0 | src/encoder/rc.c:245 |
| `N265_VBR_FBTRK` | 0.06 | src/encoder/rc.c:276 |
| `N265_VBR_FBTRK_MAX` | 6.0 | src/encoder/rc.c:278 |
| `N265_VBR_FBTRK_W` | 0.04 | src/encoder/rc.c:277 |
| `N265_VBR_FBUP` | = N265_ABR_FBMAX (capped VBR only) | src/encoder/rc.c:216 |
| `N265_VBR_FBW` | 2.0 under capped VBR, else 0 | src/encoder/rc.c:244 |
| `N265_VBR_FILLGOAL` | 0.5 | src/encoder/rc.c:314 |
| `N265_VBR_FILLQ` | 6.0 | src/encoder/rc.c:313 |
| `N265_VBR_LAG` | 7 (VBR, threads > 1) | src/encoder/encoder.c:2892 |
| `N265_VBR_LAG_BUFS` | 1.2 | src/encoder/encoder.c:2901 |
| `N265_VBR_LAG_CAL` | 0.1 | src/encoder/encoder.c:2954 |
| `N265_VBR_LAG_LOW` | 0.5 | src/encoder/encoder.c:2960 |
| `N265_VBR_LAG_PEND` | 1 | src/encoder/encoder.c:2977 |
| `N265_VBR_LAG_POSTI_RAISE` | 3.0 | src/encoder/encoder.c:2972 |
| `N265_VBR_LAG_WARM` | 2 * (lag + 1) | src/encoder/encoder.c:2952 |
| `N265_VBR_OVW` | 0.75 under capped VBR, else 0 | src/encoder/rc.c:248 |
| `N265_VBR_SPENDFILL` | 0.4 | src/encoder/rc.c:271 |
| `N265_VBR_SPENDHI` | 0.8 | src/encoder/rc.c:272 |
| `N265_VBR_VLVL` | 0.1 | src/encoder/rc.c:303 |
| `N265_VBV_DECAY` | 0.0625 | src/encoder/rc.c:369 |
| `N265_VBV_LOOKAHEAD` | 1 | src/encoder/encoder.c:2496 |
| `N265_VBV_OWN` | 3 under capped VBR, else 0 | src/encoder/rc.c:250 |
| `N265_VBV_RESERVE` | 0.2 | src/encoder/encoder.c:2503 |
| `N265_VBV_SAFE` | 0.85 | src/encoder/encoder.c:2489 |

## instrument (201)

| knob | default | reader |
|---|---|---|
| `N265_2P_CLAMP` | 6.0 | src/encoder/encoder.c:682 |
| `N265_2P_GAIN` | 0.25 | src/encoder/rc.c:347 |
| `N265_2P_QCOMP` | = N265_ABR_QCOMP | src/encoder/encoder.c:680 |
| `N265_2P_TRACE` | presence | src/encoder/encoder.c:697 |
| `N265_2P_TRUST` | 2.0 | src/encoder/encoder.c:689 |
| `N265_ABR_CMEM` | ? | src/encoder/rc.c:325 |
| `N265_ABR_COPEN` | 1 | src/encoder/rc.c:330 |
| `N265_ABR_CSEED` | 0.0 | src/encoder/rc.c:329 |
| `N265_ABR_CTF` | 1 | src/encoder/rc.c:334 |
| `N265_ABR_FBMAX` | 6.0 | src/encoder/rc.c:181 |
| `N265_ABR_FBSTEP` | 1 | src/encoder/rc.c:332 |
| `N265_ABR_FBUP` | = N265_VBR_FBUP under capped VBR (item 37), 1.5 under another VBV, else = N265_ABR_FBMAX | src/encoder/rc.c:215 |
| `N265_ABR_FLATC` | 17.0 under a VBV, else 0 | src/encoder/rc.c:403 |
| `N265_ABR_HORIZON` | 6.0 | src/encoder/rc.c:173 |
| `N265_ABR_IBRAKE` | 1 | src/encoder/rc.c:331 |
| `N265_ABR_IGAIN` | 0.085 | src/encoder/rc.c:179 |
| `N265_ABR_IMAX` | 8.0 | src/encoder/rc.c:180 |
| `N265_ABR_IOFF` | 0.0 | src/encoder/rc.c:335 |
| `N265_ABR_IOFF_CT` | 1.0 under a VBV without N265_VBR_CTKEEP, else 0 | src/encoder/rc.c:336 |
| `N265_ABR_LVL` | 2.0 | src/encoder/rc.c:360 |
| `N265_ABR_MEAS` | 1 | src/encoder/rc.c:316 |
| `N265_ABR_MGUARD` | 0.0 | src/encoder/rc.c:317 |
| `N265_ABR_OPENI` | 0 | src/encoder/rc.c:333 |
| `N265_ABR_OPENSTEP` | 6.0 | src/encoder/rc.c:178 |
| `N265_ABR_PLANERR` | 0 | src/encoder/rc.c:218 |
| `N265_ABR_QCOMP` | 0.6 | src/encoder/rc.c:172 |
| `N265_ABR_SI` | 25.4 | src/encoder/rc.c:356 |
| `N265_ABR_SMEM` | 16.0 | src/encoder/rc.c:177 |
| `N265_ABR_STEP` | 1.0 | src/encoder/rc.c:176 |
| `N265_ABR_TCAP` | 1 | src/encoder/rc.c:328 |
| `N265_ABR_WMEM` | 1000.0 | src/encoder/rc.c:315 |
| `N265_API_DEFER` | presence | src/encoder/encoder.c:3048 |
| `N265_AQ2_TRACE` | presence | src/encoder/encoder.c:6201 |
| `N265_AQ_ANCHOR` | 6.4 | src/encoder/encoder.c:6007 |
| `N265_AQ_CLAMP` | 3.0 | src/encoder/encoder.c:6098 |
| `N265_AQ_CLAMP_FLAT` | 0.0 | src/encoder/encoder.c:6128 |
| `N265_AQ_DC` | ? | src/encoder/encoder.c:6007 |
| `N265_AQ_FLAT_SIGN` | 0 | src/encoder/encoder.c:1858 |
| `N265_AQ_FLOOR` | 4 | src/encoder/encoder.c:5892 |
| `N265_AQ_POOL` | 1 | src/encoder/encoder.c:2366 |
| `N265_AQ_SIGNAL` | 1 | src/encoder/encoder.c:1857 |
| `N265_AQ_STR` | 0.7 | src/encoder/encoder.c:1851 |
| `N265_AQ_TRACE` | presence | src/encoder/encoder.c:6206 |
| `N265_ASM_OFF` | unset | src/common/cpu.c:110 |
| `N265_BADAPT_REFW` | 128.0 | src/encoder/encoder.c:7900 |
| `N265_BADAPT_THR` | 0.35 | src/encoder/encoder.c:7899 |
| `N265_BADAPT_TRACE` | presence | src/encoder/la.c:1371 |
| `N265_BI_GATE` | 4 | src/encoder/ctu.c:3624 |
| `N265_BI_REFINE` | 0 | src/encoder/ctu.c:3576 |
| `N265_BRUN_TRACE` | presence | src/encoder/la.c:626 |
| `N265_B_INTRA_MARGIN` | 30 | src/encoder/ctu.c:3558 |
| `N265_CB64_AMVP` | 0 | src/encoder/ctu.c:3543 |
| `N265_CB64_GATE` | 1 | src/encoder/ctu.c:3525 |
| `N265_CB64_PROF` | presence | src/encoder/ctu.c:3496 |
| `N265_CB64_SKIPGATE` | 0 | src/encoder/ctu.c:3284 |
| `N265_CB64_SPLITGATE` | 0 | src/encoder/ctu.c:3394 |
| `N265_CB64_TRACE` | presence | src/encoder/ctu.c:196 |
| `N265_CBF_FAST` | 2 | src/encoder/ctu.c:2089 |
| `N265_CBIT_TRACE` | presence | src/encoder/encoder.c:1972 |
| `N265_CHROMA_PRED` | 1 | src/encoder/ctu.c:131 |
| `N265_CHROMA_W` | 0 | src/encoder/ctu.c:66 |
| `N265_CPRED_GATE` | 2 | src/encoder/ctu.c:3068 |
| `N265_CPRED_TRACE` | presence | src/encoder/ctu.c:5358 |
| `N265_CRF_DC_ANCHOR` | 3 | src/encoder/encoder.c:1766 |
| `N265_CRF_DC_WIN` | 12.0 | src/encoder/encoder.c:1772 |
| `N265_CRF_SHIFT` | 5.4 | src/encoder/encoder.c:4987 |
| `N265_CT_CENTRE` | 0 | src/encoder/la.c:1773 |
| `N265_CT_FRAMEW` | 1.0 | src/encoder/encoder.c:1790 |
| `N265_CT_FRAMEW_FLAT` | = N265_CT_QPW | src/encoder/encoder.c:1794 |
| `N265_CT_FRAME_CLAMP` | 0 | src/encoder/encoder.c:1798 |
| `N265_CT_FRAME_LEVEL` | 1 | src/encoder/la.c:1822 |
| `N265_CT_FRAME_SHOT` | 0 | src/encoder/encoder.c:1800 |
| `N265_CT_FRAME_UP` | 0 | src/encoder/encoder.c:1808 |
| `N265_CT_QPW` | 2.0 | src/encoder/encoder.c:1782 |
| `N265_CT_TRACE` | presence | src/encoder/la.c:1920 |
| `N265_CT_UP_GATE` | 0.24 | src/encoder/encoder.c:1826 |
| `N265_CUTREE_STR` | 2.0 | src/encoder/encoder.c:1781 |
| `N265_CU_STATS` | presence | src/encoder/encoder.c:1968 |
| `N265_CU_TRACE` | unset | src/encoder/encoder.c:1475 |
| `N265_DBF_BETA` | 0 | src/encoder/encoder.c:2386 |
| `N265_DBF_TC` | 0 | src/encoder/encoder.c:2387 |
| `N265_DECGRAD_TRACE` | presence | src/encoder/rdoq.c:356 |
| `N265_DECIMATE` | 15 | src/encoder/rdoq.c:309 |
| `N265_DECIMATE_GRAD` | = N265_DECIMATE | src/encoder/rdoq.c:323 |
| `N265_DEPTH_BOUND` | 0 | src/encoder/ctu.c:2450 |
| `N265_DEPTH_DUMP` | unset | src/encoder/encoder.c:1462 |
| `N265_DPB_HOLD` | 0 | src/encoder/encoder.c:929 |
| `N265_DPB_TRACE` | presence | src/encoder/encoder.c:1946 |
| `N265_DUP_FAST` | 0 | src/encoder/la.c:636 |
| `N265_DUP_THR` | 0.0 | src/encoder/la.c:674 |
| `N265_DUP_TRACE` | presence | src/encoder/la.c:646 |
| `N265_ES_THR` | 100 | src/encoder/la.c:1494 |
| `N265_FGC_TRACE` | presence | src/encoder/grain.c:192 |
| `N265_FLAT_GATE` | 10 | src/encoder/la.c:102 |
| `N265_FLAT_TRACE` | presence | src/encoder/encoder.c:5780 |
| `N265_FORCE_NONIDR` | -1 | src/encoder/encoder.c:7448 |
| `N265_FP_CENSUS` | presence | src/encoder/encoder.c:1969 |
| `N265_FULLPEL` | presence | src/encoder/mv.c:113 |
| `N265_GOP_TRACE` | presence | cli/n265.c:2253 |
| `N265_HD_QP` | 27 | src/encoder/encoder.c:4963 |
| `N265_HF2` | 3 | src/encoder/encoder.c:3872 |
| `N265_HF2_TPP` | 150 | src/encoder/encoder.c:3900 |
| `N265_HF2_TPP14` | 190 | src/encoder/encoder.c:3911 |
| `N265_HF2_TRACE` | presence | src/encoder/encoder.c:3923 |
| `N265_HFCACHE` | 1 | src/encoder/encoder.c:3808 |
| `N265_INTER_NXN` | 0 | src/encoder/ctu.c:4558 |
| `N265_INTRA_ADAPT_K` | 32 | src/encoder/encoder.c:1980 |
| `N265_INTRA_ADAPT_THR` | 100 | src/encoder/encoder.c:1983 |
| `N265_INTRA_GATE` | 7 | src/encoder/ctu.c:2242 |
| `N265_INTRA_MIN` | 4 | src/encoder/encoder.c:7437 |
| `N265_INTRA_MPM` | 0 | src/encoder/ctu.c:4528 |
| `N265_INTRA_ORACLE` | 0 | src/encoder/ctu.c:2271 |
| `N265_INTRA_RD_N` | 3 | src/encoder/ctu.c:4517 |
| `N265_INTRA_REUSE` | 1 | src/encoder/ctu.c:4612 |
| `N265_INTRA_RSKIP` | 0 | src/encoder/ctu.c:2015 |
| `N265_INTRA_TRY_TRACE` | presence | src/encoder/ctu.c:1505 |
| `N265_INTRA_WIN_TRACE` | presence | src/encoder/encoder.c:1973 |
| `N265_I_INTRA` | 0 | src/encoder/ctu.c:4544 |
| `N265_LA_MEPRED` | 1 | src/encoder/la.c:611 |
| `N265_LA_MVSTAT` | presence | src/encoder/la.c:1053 |
| `N265_LA_PROF` | presence | src/encoder/encoder.c:3053 |
| `N265_LEVEL_TRACE` | presence | src/encoder/encoder.c:2769 |
| `N265_MAX_FRAME_WORK` | 6 with --bframes >= 2, else 4 | src/encoder/encoder.c:2793 |
| `N265_MCSTF_TRACE` | presence | src/encoder/mcstf.c:126 |
| `N265_MEREUSE_TRACE` | presence | src/encoder/encoder.c:1970 |
| `N265_MERGE_RD_N` | 5 | src/encoder/ctu.c:3595 |
| `N265_MERGE_TRACE` | -1 | src/encoder/mv.c:107 |
| `N265_ME_SATD` | 0 | src/encoder/ctu.c:3199 |
| `N265_MIN_KEYINT` | 4 | src/encoder/encoder.c:7419 |
| `N265_MODE_HIST` | presence | src/encoder/sao.c:253 |
| `N265_MVD_BITS` | 0 | src/encoder/ctu.c:3187 |
| `N265_NO_AMP` | 1 | src/encoder/encoder.c:1902 |
| `N265_NO_RECT` | 1 | src/encoder/encoder.c:1900 |
| `N265_NO_SAO` | presence | src/encoder/encoder.c:1967 |
| `N265_NO_SPLIT` | presence | src/encoder/ctu.c:184 |
| `N265_NR_INTER` | 0 | src/encoder/encoder.c:2296 |
| `N265_NR_INTRA` | 0 | src/encoder/encoder.c:2294 |
| `N265_OPENGOP_CUT` | 0 | src/encoder/encoder.c:994 |
| `N265_OPENGOP_NOBARRIER` | 0 | src/encoder/encoder.c:1001 |
| `N265_PART_GATE` | 2 | src/encoder/ctu.c:2324 |
| `N265_PB_CASCADE` | auto | src/encoder/encoder.c:1832 |
| `N265_PB_CASCADE_FLAT` | auto | src/encoder/encoder.c:1841 |
| `N265_PSY` | 0.0 | src/encoder/encoder.c:2004 |
| `N265_QP_FRAC` | = --bitrate or CRF+frame term | src/encoder/encoder.c:2347 |
| `N265_QP_FRAC_FLAT` | 0 | src/encoder/encoder.c:2360 |
| `N265_QP_MAP_TRACE` | unset | src/encoder/encoder.c:6333 |
| `N265_QP_ROUND1` | 1 | src/encoder/encoder.c:1859 |
| `N265_QP_TDITHER` | 1 | src/encoder/encoder.c:2379 |
| `N265_QP_TRACE` | presence | src/encoder/encoder.c:6356 |
| `N265_RC_LAG` | presence | src/encoder/encoder.c:2891 |
| `N265_RC_TRACE` | presence | src/encoder/encoder.c:697 |
| `N265_RDOQ_ADAPT` | 0 | src/encoder/rdoq.c:297 |
| `N265_RDOQ_LAM` | 100 | src/encoder/rdoq.c:303 |
| `N265_RDOQ_LAT_SZ` | 0 | src/encoder/ctu.c:979 |
| `N265_RDOQ_TRACE` | presence | src/encoder/ctu.c:841 |
| `N265_RECT_FLAT` | 1 | src/encoder/ctu.c:2402 |
| `N265_RECT_RANK` | 50 | src/encoder/ctu.c:2367 |
| `N265_REF_LAG_HINT` | -1 | src/encoder/encoder.c:6386 |
| `N265_RSKIP` | 2 | src/encoder/ctu.c:2002 |
| `N265_SAO_ANIM` | 1 | src/encoder/sao.c:309 |
| `N265_SAO_BIAS` | 100 | src/encoder/sao.c:356 |
| `N265_SAO_CHROMA` | 1 | src/encoder/sao.c:512 |
| `N265_SAO_GATE` | 0 | src/encoder/sao.c:561 |
| `N265_SAO_QPLAM` | 0 | src/encoder/sao.c:346 |
| `N265_SAO_TRACE` | unset | src/encoder/sao.c:654 |
| `N265_SAO_TYPES` | 0 | src/encoder/sao.c:272 |
| `N265_SATD8` | 0 | src/encoder/satd.c:134 |
| `N265_SBH_RD` | 2 | src/encoder/rdoq.c:282 |
| `N265_SC_FORCE` | ? | src/encoder/la.c:545 |
| `N265_SC_HOLD` | 1 | src/encoder/la.c:243 |
| `N265_SC_TRACE` | presence | src/encoder/la.c:1216 |
| `N265_SERIAL_PROF` | 0 | src/encoder/encoder.c:3050 |
| `N265_SHOT_STATS` | unset | src/encoder/encoder.c:1992 |
| `N265_SHOT_TRACE` | presence | src/encoder/encoder.c:7776 |
| `N265_SHOT_WINDOW` | 0 | src/encoder/encoder.c:2165 |
| `N265_SKIP_EXIT` | 0 | src/encoder/ctu.c:2140 |
| `N265_SKIP_THR` | 2 | src/encoder/ctu.c:1901 |
| `N265_STALL_TRACE` | presence | src/encoder/encoder.c:3524 |
| `N265_SUBME` | 3 | src/encoder/mv.c:656 |
| `N265_SUBPEL_DEDUP` | 1 | src/encoder/mv.c:119 |
| `N265_SUBPEL_GATE` | 0 | src/encoder/mv.c:728 |
| `N265_SUBPEL_TRACE` | presence | src/encoder/ctu.c:3764 |
| `N265_SZ_TRACE` | presence | src/encoder/encoder.c:2585 |
| `N265_TASK_TRACE` | unset | src/encoder/ttrace.c:31 |
| `N265_TREE_RAMP` | 4.0 | src/encoder/encoder.c:2123 |
| `N265_UHD_QP` | 29 | src/encoder/encoder.c:4943 |
| `N265_VBR_OFFB` | 0.0 | src/encoder/encoder.c:2586 |
| `N265_VBR_OPEN` | 0 | src/encoder/rc.c:439 |
| `N265_VBR_SZM` | 0 | src/encoder/rc.c:419 |
| `N265_VBV_BRAKE` | 1 under --bitrate with a VBV, else 0 | src/encoder/rc.c:373 |
| `N265_VBV_BRAKE_FILL` | 0.9 | src/encoder/rc.c:405 |
| `N265_VBV_BRAKE_K0` | 0.05 | src/encoder/rc.c:402 |
| `N265_VBV_HRD` | 1 | src/encoder/encoder.c:2588 |
| `N265_VBV_SCPRED` | 0.5 under --bitrate, else 1.0 | src/encoder/rc.c:408 |
| `N265_VBV_TRACE` | presence | src/encoder/encoder.c:2584 |
| `N265_WEIGHTB` | 0 | src/encoder/encoder.c:2412 |
| `N265_WEIGHTP` | 0 | src/encoder/encoder.c:2410 |
| `N265_WP_GAIN` | 4 | src/encoder/wp.c:78 |
| `N265_WP_REFS` | 5 | src/encoder/wp.c:117 |
| `N265_WP_TRACE` | 0 | src/encoder/wp.c:51 |
| `N265_WP_WIN` | 40 | src/encoder/wp.c:101 |

## arm (87)

| knob | default | reader |
|---|---|---|
| `N265_ABR_IOFF_MAX` | 6.0 | src/encoder/rc.c:338 |
| `N265_AQ` | presence | src/encoder/encoder.c:2025 |
| `N265_AQ2` | 0 | src/encoder/encoder.c:1862 |
| `N265_AQ2_FLAT` | 0 | src/encoder/encoder.c:1863 |
| `N265_AQ2_MREF` | 0.0 | src/encoder/encoder.c:1866 |
| `N265_AQ_BPULL` | 0.0 | src/encoder/encoder.c:1870 |
| `N265_AQ_BPULL_ABR` | 0.0 | src/encoder/encoder.c:1874 |
| `N265_AQ_BPULL_NR` | 0 | src/encoder/encoder.c:1871 |
| `N265_AQ_FLAT` | presence | src/encoder/encoder.c:5831 |
| `N265_AQ_FLAT_THR` | = ungated | src/encoder/encoder.c:6167 |
| `N265_BADAPT_DEG` | 1.6 | src/encoder/encoder.c:7943 |
| `N265_BADAPT_FASTW` | 0.0 | src/encoder/encoder.c:7908 |
| `N265_BADAPT_MVMIN` | 0.0 | src/encoder/encoder.c:7909 |
| `N265_CBQPOFFS` | 0 | src/encoder/encoder.c:2399 |
| `N265_CDZ` | 0 | src/encoder/ctu.c:1000 |
| `N265_CHROMA_W_PCT` | 100 | src/encoder/ctu.c:72 |
| `N265_CPRED_W` | 100 | src/encoder/ctu.c:3041 |
| `N265_CRF_LEGACY` | 0 | src/encoder/encoder.c:1737 |
| `N265_CRQPOFFS` | 0 | src/encoder/encoder.c:2401 |
| `N265_CT_LAMBDA` | 0.0 | src/encoder/encoder.c:1783 |
| `N265_EST_MODE` | 0 | src/encoder/est.c:24 |
| `N265_GOP_CUTS` | 1 | cli/n265.c:2277 |
| `N265_GOP_K` | (--threads / N) + 1, at least 3 | cli/n265.c:2305 |
| `N265_GOP_LEN` | = --keyint | cli/n265.c:2274 |
| `N265_GOP_RELIEF` | 1 | cli/n265.c:2304 |
| `N265_GOP_SEEK` | 1 | cli/n265.c:2306 |
| `N265_GOP_VBV_IN` | 0.5 | cli/n265.c:2295 |
| `N265_GOP_VBV_OUT` | 0.6 | cli/n265.c:2296 |
| `N265_GOP_WINDOW` | (N + 1.5) x N265_GOP_LEN | cli/n265.c:2279 |
| `N265_INTRA_B_MODES` | 0 | src/encoder/ctu.c:1469 |
| `N265_INTRA_FAST_FLAT` | 0 | src/encoder/ctu.c:1612 |
| `N265_INTRA_FAST_QP` | 0 | src/encoder/ctu.c:1577 |
| `N265_INTRA_FAST_STEP` | 4 | src/encoder/ctu.c:1587 |
| `N265_KEY_DEFER` | 0 | src/encoder/encoder.c:7510 |
| `N265_K_CAP` | 0 | src/encoder/encoder.c:2873 |
| `N265_MCSTF` | 0 | src/encoder/encoder.c:2235 |
| `N265_MCSTF_CTF` | 1.0 | src/encoder/encoder.c:2252 |
| `N265_MCSTF_CTSC` | 1.0 | src/encoder/encoder.c:2254 |
| `N265_MCSTF_DC` | 0 | src/encoder/mcstf.c:259 |
| `N265_MCSTF_QPB` | 0.0 | src/encoder/encoder.c:2248 |
| `N265_MCSTF_QPB_I` | 0 | src/encoder/encoder.c:2250 |
| `N265_MCSTF_RANGE` | 2 | src/encoder/encoder.c:2240 |
| `N265_MEREUSE` | 0 | src/encoder/encoder.c:1971 |
| `N265_NO_AMVP` | presence | src/encoder/ctu.c:178 |
| `N265_NO_EARLYSKIP` | presence | src/encoder/ctu.c:190 |
| `N265_NO_GATES` | presence | src/encoder/ctu.c:172 |
| `N265_NO_TMVP` | presence | src/encoder/encoder.c:3754 |
| `N265_OLD_BS` | presence | src/encoder/deblock.c:158 |
| `N265_PSY_DARK` | 0.0 | src/encoder/encoder.c:2020 |
| `N265_RD_PROXY` | 0 | src/encoder/ctu.c:5720 |
| `N265_REF2_LITE` | 0 | src/encoder/ctu.c:3325 |
| `N265_REF2_ROLE` | 0 | src/encoder/encoder.c:8320 |
| `N265_RUNG_GATE` | -2 | src/encoder/mv.c:785 |
| `N265_RUNG_RERANK` | 0 | src/encoder/mv.c:801 |
| `N265_SC_CONJ` | 3 | src/encoder/ctu.c:1961 |
| `N265_SC_FLOOR` | 0.0 | src/encoder/la.c:513 |
| `N265_SHOT_CLAMP` | 2.0 | src/encoder/encoder.c:2184 |
| `N265_SHOT_EMA` | 0.5 | src/encoder/encoder.c:2189 |
| `N265_SHOT_K` | 1.0 | src/encoder/encoder.c:2183 |
| `N265_STATIC_COMMIT` | 0 | src/encoder/ctu.c:1937 |
| `N265_SUBME_RUNG` | 0 | src/encoder/mv.c:771 |
| `N265_SUBPEL_AXIS` | 0 | src/encoder/mv.c:866 |
| `N265_SUBPEL_RATE` | 0 | src/encoder/mv.c:684 |
| `N265_TT_SKIP_INTRA` | 0 | src/encoder/ctu.c:2039 |
| `N265_TT_SKIP_QP` | 0 | src/encoder/ctu.c:2051 |
| `N265_UHD_REF2` | ? | src/encoder/preset.c:1204 |
| `N265_VBR_DEFPT_SPEND` | 0.0 | src/encoder/rc.c:296 |
| `N265_VBR_LAG_GFLOOR` | 0.0 | src/encoder/encoder.c:2964 |
| `N265_VBR_LAG_GRAW` | 0 | src/encoder/encoder.c:2966 |
| `N265_VBR_LAG_GUARD` | 0.0 | src/encoder/encoder.c:2962 |
| `N265_VBR_LAG_POSTI` | 0 | src/encoder/encoder.c:2970 |
| `N265_VBR_OPEN_AB` | -1.238 | src/encoder/rc.c:443 |
| `N265_VBR_OPEN_ABR` | -0.806 | src/encoder/rc.c:442 |
| `N265_VBR_OPEN_AI` | -7.027 | src/encoder/rc.c:440 |
| `N265_VBR_OPEN_AP` | 2.084 | src/encoder/rc.c:441 |
| `N265_VBR_OPEN_BB` | 1.224 | src/encoder/rc.c:447 |
| `N265_VBR_OPEN_BBR` | 1.374 | src/encoder/rc.c:446 |
| `N265_VBR_OPEN_BI` | 2.157 | src/encoder/rc.c:444 |
| `N265_VBR_OPEN_BP` | 1.249 | src/encoder/rc.c:445 |
| `N265_VBR_OPEN_G` | -1.429 | src/encoder/rc.c:448 |
| `N265_VBR_SPEND` | 0.0 | src/encoder/rc.c:270 |
| `N265_VBR_SZM_GMIN` | 0.7 | src/encoder/rc.c:427 |
| `N265_VBR_SZM_LAM` | 0.9 | src/encoder/rc.c:420 |
| `N265_VBR_SZM_SIMPLE` | 150.0 | src/encoder/rc.c:429 |
| `N265_VBR_SZM_STATIC` | 17.0 | src/encoder/rc.c:428 |
| `N265_VBR_SZM_Z` | 2.0 | src/encoder/rc.c:421 |
| `N265_VBR_SZM_ZLAG` | 1.0 | src/encoder/rc.c:426 |
