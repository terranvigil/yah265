# The N265_* knob census (generated -- edit scripts/knob_census.py OVERRIDES, not this file)

Regenerate: `python3 scripts/knob_census.py`. CI check: `--check`.

Tier meanings: **default** = shipped behavior, the env is its escape; **instrument** = measurement machinery (cataloged in docs/instruments.md when it has produced a result); **arm** = default-off gate or kept refuted arm -- kept deliberately because a refuted arm retains its re-pricing value.

In the default column, **presence** means the reader is a boolean: unset is off and any value arms it. **unset** means the value names a file or a string and unset is off. `= N265_X` means the default is another knob's resolved value.

## default (117)

| knob | default | reader |
|---|---|---|
| `N265_ABR_SB` | 1.7 | src/encoder/rc.c:359 |
| `N265_ABR_SBR` | 4.0 | src/encoder/rc.c:358 |
| `N265_ABR_SP` | 13.3 | src/encoder/rc.c:357 |
| `N265_AQ2_ABR` | 1 | src/encoder/encoder.c:2005 |
| `N265_AQ2_GMAX` | 1.0 | src/encoder/encoder.c:2010 |
| `N265_AQ2_GMIN` | 0.7 | src/encoder/encoder.c:2009 |
| `N265_AQ2_SPOW` | 1.0 | src/encoder/encoder.c:2008 |
| `N265_AQ2_SREF` | 2.5 | src/encoder/encoder.c:2006 |
| `N265_BADAPT_MVCAP` | 4 | src/encoder/la.c:567 |
| `N265_BADAPT_MVGATE` | 20 | src/encoder/la.c:602 |
| `N265_CB64_GATEQ_REF` | -1 | src/encoder/ctu.c:3498 |
| `N265_CB64_SKIPGATE_REF` | -1 | src/encoder/ctu.c:3463 |
| `N265_CCRF_BRAKE` | 1 under capped CRF, else unread | src/encoder/rc.c:393 |
| `N265_CCRF_BRAKE_FILL` | 0.8 under capped CRF, else unread | src/encoder/rc.c:407 |
| `N265_CCRF_BRK_R0` | 1.25 | src/encoder/rc.c:394 |
| `N265_CCRF_GUARD` | 1.3 under capped CRF, else 0 | src/encoder/rc.c:401 |
| `N265_CCRF_SKIP` | 1 under capped CRF, else 0 | src/encoder/rc.c:396 |
| `N265_CCRF_SKIP_FILL` | 0.8 | src/encoder/rc.c:397 |
| `N265_CCRF_SKIP_SATQ` | 51.0 | src/encoder/rc.c:398 |
| `N265_CCRF_SKIP_WARM` | 4.0 | src/encoder/rc.c:399 |
| `N265_CLAIM_PRIO` | 1 | src/encoder/encoder.c:6943 |
| `N265_CRF_DC_MEM` | 1000.0 | src/encoder/encoder.c:1911 |
| `N265_CRF_DUR` | 0.3 | src/encoder/encoder.c:1881 |
| `N265_CRF_DUR_FPS` | 25.0 | src/encoder/encoder.c:1882 |
| `N265_CT_MVRATE` | 16 | src/encoder/la.c:643 |
| `N265_DRV_PAR` | 1 | src/encoder/encoder.c:2982 |
| `N265_FGC_CHROMA` | 1 | src/encoder/grain.c:133 |
| `N265_FGC_CORE` | 1.0 | src/encoder/grain.c:143 |
| `N265_FGC_FRAMES` | 1 | src/encoder/grain.c:121 |
| `N265_FGC_MIN` | 1.0 | src/encoder/grain.c:181 |
| `N265_FGC_PCT` | 25 | src/encoder/grain.c:109 |
| `N265_FGC_RATIO` | 2.0 | src/encoder/grain.c:167 |
| `N265_GATE_SCALE` | 100 | src/encoder/ctu.c:2273 |
| `N265_HDR_FULL` | 1 on PQ and HLG at medium | src/encoder/preset.c:1216 |
| `N265_INTRA_FAST` | 0 | src/encoder/ctu.c:1708 |
| `N265_INTRA_FAST_W` | 1280 | src/encoder/ctu.c:1746 |
| `N265_JIT_COH` | 0.75 | src/encoder/la.c:958 |
| `N265_JIT_IC` | 720.0 | src/encoder/la.c:965 |
| `N265_JIT_J` | 2.2 | src/encoder/la.c:951 |
| `N265_JIT_RDOQ` | 1 | src/encoder/encoder.c:2147 |
| `N265_JIT_WIN` | 24 | src/encoder/la.c:940 |
| `N265_KEY_RESET` | 1 | src/encoder/encoder.c:8097 |
| `N265_LAMBDA` | 0.68 | src/encoder/ctu.c:2026 |
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
| `N265_MERGE_PICK_RATE` | 0 | src/encoder/ctu.c:5754 |
| `N265_ME_PATTERN` | 0 | src/encoder/mv.c:896 |
| `N265_PB_STEP` | 1.0 | src/encoder/encoder.c:5446 |
| `N265_PSY_DARK_M` | 64.0 | src/encoder/encoder.c:2167 |
| `N265_PSY_DARK_QHI` | 40.0 | src/encoder/encoder.c:2165 |
| `N265_PSY_DARK_QW` | 12.0 | src/encoder/encoder.c:2166 |
| `N265_PSY_DARK_V` | 16.0 | src/encoder/encoder.c:2168 |
| `N265_RC_MVRATE` | 32 | src/encoder/rc.c:136 |
| `N265_RDOQ` | 1 | src/encoder/encoder.c:2143 |
| `N265_RDOQ_UP` | 1 | src/encoder/rdoq.c:298 |
| `N265_READAHEAD_MB` | 64 | cli/n265.c:857 |
| `N265_ROW_YIELD` | 5000 where 2 * min(rows, (cols + 1) / 2) > threads > 1, else 0 | src/encoder/encoder.c:3132 |
| `N265_SCREEN` | 0 | src/encoder/la.c:287 |
| `N265_SC_FAILW` | 0.40 | src/encoder/la.c:683 |
| `N265_SC_FAILW_SPIKE` | 1.6 | src/encoder/la.c:700 |
| `N265_SC_JUMP` | 0.10 | src/encoder/la.c:730 |
| `N265_SC_SPIKE` | 1.8 | src/encoder/la.c:814 |
| `N265_TREE_K` | 12 | src/encoder/encoder.c:3062 |
| `N265_TREE_SLOTS` | K - 6 (with the switched HD or UHD tree only) | src/encoder/encoder.c:3147 |
| `N265_TT_SKIP_INTER` | 0 | src/encoder/ctu.c:2197 |
| `N265_UHD_CVBR` | 1 under capped VBR at UHD at medium | src/encoder/preset.c:1229 |
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
| `N265_VBR_LAG` | 7 (VBR, threads > 1) | src/encoder/encoder.c:3093 |
| `N265_VBR_LAG_BUFS` | 1.2 | src/encoder/encoder.c:3102 |
| `N265_VBR_LAG_CAL` | 0.1 | src/encoder/encoder.c:3163 |
| `N265_VBR_LAG_LOW` | 0.5 | src/encoder/encoder.c:3169 |
| `N265_VBR_LAG_PEND` | 1 | src/encoder/encoder.c:3186 |
| `N265_VBR_LAG_POSTI_RAISE` | 3.0 | src/encoder/encoder.c:3181 |
| `N265_VBR_LAG_WARM` | 2 * (lag + 1) | src/encoder/encoder.c:3161 |
| `N265_VBR_OVW` | 0.75 under capped VBR, else 0 | src/encoder/rc.c:248 |
| `N265_VBR_SPENDFILL` | 0.4 | src/encoder/rc.c:271 |
| `N265_VBR_SPENDHI` | 0.8 | src/encoder/rc.c:272 |
| `N265_VBR_VLVL` | 0.1 | src/encoder/rc.c:303 |
| `N265_VBV_DECAY` | 0.0625 | src/encoder/rc.c:369 |
| `N265_VBV_LOOKAHEAD` | 1 | src/encoder/encoder.c:2667 |
| `N265_VBV_OWN` | 3 under capped VBR, else 0 | src/encoder/rc.c:250 |
| `N265_VBV_RESERVE` | 0.2 | src/encoder/encoder.c:2674 |
| `N265_VBV_SAFE` | 0.85 | src/encoder/encoder.c:2660 |

## instrument (203)

| knob | default | reader |
|---|---|---|
| `N265_2P_CLAMP` | 6.0 | src/encoder/encoder.c:757 |
| `N265_2P_GAIN` | 0.25 | src/encoder/rc.c:347 |
| `N265_2P_QCOMP` | = N265_ABR_QCOMP | src/encoder/encoder.c:755 |
| `N265_2P_TRACE` | presence | src/encoder/encoder.c:772 |
| `N265_2P_TRUST` | 2.0 | src/encoder/encoder.c:764 |
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
| `N265_API_DEFER` | presence | src/encoder/encoder.c:3257 |
| `N265_AQ2_TRACE` | presence | src/encoder/encoder.c:6732 |
| `N265_AQ_ANCHOR` | 6.4 | src/encoder/encoder.c:6538 |
| `N265_AQ_CLAMP` | 3.0 | src/encoder/encoder.c:6629 |
| `N265_AQ_CLAMP_FLAT` | 0.0 | src/encoder/encoder.c:6659 |
| `N265_AQ_DC` | ? | src/encoder/encoder.c:6538 |
| `N265_AQ_FLAT_SIGN` | 0 | src/encoder/encoder.c:1999 |
| `N265_AQ_FLOOR` | 4 | src/encoder/encoder.c:6489 |
| `N265_AQ_POOL` | 1 | src/encoder/encoder.c:2514 |
| `N265_AQ_SIGNAL` | 1 | src/encoder/encoder.c:1998 |
| `N265_AQ_STR` | 0.7 | src/encoder/encoder.c:1992 |
| `N265_AQ_TRACE` | presence | src/encoder/encoder.c:6737 |
| `N265_ASM_OFF` | unset | src/common/cpu.c:200 |
| `N265_BADAPT_REFW` | 128.0 | src/encoder/encoder.c:8502 |
| `N265_BADAPT_THR` | 0.35 | src/encoder/encoder.c:8501 |
| `N265_BADAPT_TRACE` | presence | src/encoder/la.c:1814 |
| `N265_BI_GATE` | 4 | src/encoder/ctu.c:3820 |
| `N265_BI_REFINE` | 0 | src/encoder/ctu.c:3772 |
| `N265_BRUN_TRACE` | presence | src/encoder/la.c:850 |
| `N265_B_INTRA_MARGIN` | 30 | src/encoder/ctu.c:3754 |
| `N265_CB64_AMVP` | 0 | src/encoder/ctu.c:3739 |
| `N265_CB64_GATE` | 1 | src/encoder/ctu.c:3721 |
| `N265_CB64_PROF` | presence | src/encoder/ctu.c:3692 |
| `N265_CB64_SKIPGATE` | 0 | src/encoder/ctu.c:3445 |
| `N265_CB64_SPLITGATE` | 0 | src/encoder/ctu.c:3590 |
| `N265_CB64_TRACE` | presence | src/encoder/ctu.c:196 |
| `N265_CBF_FAST` | 2 | src/encoder/ctu.c:2257 |
| `N265_CBIT_TRACE` | presence | src/encoder/encoder.c:2113 |
| `N265_CHROMA_PRED` | 1 | src/encoder/ctu.c:131 |
| `N265_CHROMA_W` | 0 | src/encoder/ctu.c:66 |
| `N265_CPRED_GATE` | 2 | src/encoder/ctu.c:3229 |
| `N265_CPRED_TRACE` | presence | src/encoder/ctu.c:5562 |
| `N265_CPU_MAX` | unset | src/common/cpu.c:197 |
| `N265_CRF_DC_ANCHOR` | 3 | src/encoder/encoder.c:1907 |
| `N265_CRF_DC_WIN` | 12.0 | src/encoder/encoder.c:1913 |
| `N265_CRF_SHIFT` | 5.4 | src/encoder/encoder.c:5429 |
| `N265_CT_CENTRE` | 0 | src/encoder/la.c:2336 |
| `N265_CT_FRAMEW` | 1.0 | src/encoder/encoder.c:1931 |
| `N265_CT_FRAMEW_FLAT` | = N265_CT_QPW | src/encoder/encoder.c:1935 |
| `N265_CT_FRAME_CLAMP` | 0 | src/encoder/encoder.c:1939 |
| `N265_CT_FRAME_LEVEL` | 1 | src/encoder/la.c:2385 |
| `N265_CT_FRAME_SHOT` | 0 | src/encoder/encoder.c:1941 |
| `N265_CT_FRAME_UP` | 0 | src/encoder/encoder.c:1949 |
| `N265_CT_QPW` | 2.0 | src/encoder/encoder.c:1923 |
| `N265_CT_TRACE` | presence | src/encoder/la.c:2479 |
| `N265_CT_UP_GATE` | 0.24 | src/encoder/encoder.c:1967 |
| `N265_CUTREE_STR` | 2.0 | src/encoder/encoder.c:1922 |
| `N265_CU_STATS` | presence | src/encoder/encoder.c:2109 |
| `N265_CU_TRACE` | unset | src/encoder/encoder.c:1602 |
| `N265_DBF_BETA` | 0 | src/encoder/encoder.c:2534 |
| `N265_DBF_TC` | 0 | src/encoder/encoder.c:2535 |
| `N265_DECGRAD_TRACE` | presence | src/encoder/rdoq.c:363 |
| `N265_DECIMATE` | 15 | src/encoder/rdoq.c:316 |
| `N265_DECIMATE_GRAD` | = N265_DECIMATE | src/encoder/rdoq.c:330 |
| `N265_DEPTH_BOUND` | 0 | src/encoder/ctu.c:2618 |
| `N265_DEPTH_DUMP` | unset | src/encoder/encoder.c:1589 |
| `N265_DPB_HOLD` | 0 | src/encoder/encoder.c:1025 |
| `N265_DPB_TRACE` | presence | src/encoder/encoder.c:2087 |
| `N265_DUP_FAST` | 0 | src/encoder/la.c:860 |
| `N265_DUP_THR` | 0.0 | src/encoder/la.c:898 |
| `N265_DUP_TRACE` | presence | src/encoder/la.c:870 |
| `N265_ES_THR` | 100 | src/encoder/la.c:1937 |
| `N265_FGC_TRACE` | presence | src/encoder/grain.c:192 |
| `N265_FLAT_GATE` | 10 | src/encoder/la.c:140 |
| `N265_FLAT_TRACE` | presence | src/encoder/encoder.c:6398 |
| `N265_FORCE_NONIDR` | -1 | src/encoder/encoder.c:8050 |
| `N265_FP_CENSUS` | presence | src/encoder/encoder.c:2110 |
| `N265_FULLPEL` | presence | src/encoder/mv.c:113 |
| `N265_GOP_TRACE` | presence | cli/n265.c:2485 |
| `N265_HD_QP` | 27 | src/encoder/encoder.c:5405 |
| `N265_HF2` | 3 | src/encoder/encoder.c:4099 |
| `N265_HF2_TPP` | 150 | src/encoder/encoder.c:4127 |
| `N265_HF2_TPP14` | 190 | src/encoder/encoder.c:4138 |
| `N265_HF2_TRACE` | presence | src/encoder/encoder.c:4150 |
| `N265_HFCACHE` | 1 | src/encoder/encoder.c:4035 |
| `N265_INTER_NXN` | 0 | src/encoder/ctu.c:4762 |
| `N265_INTRA_ADAPT_K` | 32 | src/encoder/encoder.c:2121 |
| `N265_INTRA_ADAPT_THR` | 100 | src/encoder/encoder.c:2124 |
| `N265_INTRA_GATE` | 7 | src/encoder/ctu.c:2410 |
| `N265_INTRA_MIN` | 4 | src/encoder/encoder.c:8039 |
| `N265_INTRA_MPM` | 0 | src/encoder/ctu.c:4732 |
| `N265_INTRA_ORACLE` | 0 | src/encoder/ctu.c:2439 |
| `N265_INTRA_RD_N` | 3 | src/encoder/ctu.c:4721 |
| `N265_INTRA_REUSE` | 1 | src/encoder/ctu.c:4816 |
| `N265_INTRA_RSKIP` | 0 | src/encoder/ctu.c:2183 |
| `N265_INTRA_TRY_TRACE` | presence | src/encoder/ctu.c:1649 |
| `N265_INTRA_WIN_TRACE` | presence | src/encoder/encoder.c:2114 |
| `N265_I_INTRA` | 0 | src/encoder/ctu.c:4748 |
| `N265_JIT_TRACE` | presence | src/encoder/la.c:932 |
| `N265_LA_MEPRED` | 1 | src/encoder/la.c:835 |
| `N265_LA_MVSTAT` | presence | src/encoder/la.c:1493 |
| `N265_LA_PROF` | presence | src/encoder/encoder.c:3262 |
| `N265_LEVEL_TRACE` | presence | src/encoder/encoder.c:2954 |
| `N265_MAX_FRAME_WORK` | 6 with --bframes >= 2, else 4 | src/encoder/encoder.c:2984 |
| `N265_MCSTF_TRACE` | presence | src/encoder/mcstf.c:126 |
| `N265_MEREUSE_TRACE` | presence | src/encoder/encoder.c:2111 |
| `N265_MERGE_RD_N` | 5 | src/encoder/ctu.c:3791 |
| `N265_MERGE_TRACE` | -1 | src/encoder/mv.c:107 |
| `N265_ME_SATD` | 0 | src/encoder/ctu.c:3360 |
| `N265_MIN_KEYINT` | 4 | src/encoder/encoder.c:8021 |
| `N265_MODE_HIST` | presence | src/encoder/sao.c:419 |
| `N265_MVD_BITS` | 0 | src/encoder/ctu.c:3348 |
| `N265_NO_AMP` | 1 | src/encoder/encoder.c:2043 |
| `N265_NO_RECT` | 1 | src/encoder/encoder.c:2041 |
| `N265_NO_SAO` | presence | src/encoder/encoder.c:2108 |
| `N265_NO_SPLIT` | presence | src/encoder/ctu.c:184 |
| `N265_NR_INTER` | 0 | src/encoder/encoder.c:2444 |
| `N265_NR_INTRA` | 0 | src/encoder/encoder.c:2442 |
| `N265_OPENGOP_CUT` | 0 | src/encoder/encoder.c:1090 |
| `N265_OPENGOP_NOBARRIER` | 0 | src/encoder/encoder.c:1097 |
| `N265_PART_GATE` | 2 | src/encoder/ctu.c:2492 |
| `N265_PB_CASCADE` | auto | src/encoder/encoder.c:1973 |
| `N265_PB_CASCADE_FLAT` | auto | src/encoder/encoder.c:1982 |
| `N265_PSY` | 0.0 | src/encoder/encoder.c:2148 |
| `N265_QP_FRAC` | = --bitrate or CRF+frame term | src/encoder/encoder.c:2495 |
| `N265_QP_FRAC_FLAT` | 0 | src/encoder/encoder.c:2508 |
| `N265_QP_MAP_TRACE` | unset | src/encoder/encoder.c:6868 |
| `N265_QP_ROUND1` | 1 | src/encoder/encoder.c:2000 |
| `N265_QP_TDITHER` | 1 | src/encoder/encoder.c:2527 |
| `N265_QP_TRACE` | presence | src/encoder/encoder.c:6891 |
| `N265_RC_LAG` | presence | src/encoder/encoder.c:3092 |
| `N265_RC_TRACE` | presence | src/encoder/encoder.c:772 |
| `N265_RDOQ_ADAPT` | 0 | src/encoder/rdoq.c:304 |
| `N265_RDOQ_LAM` | 100 | src/encoder/rdoq.c:310 |
| `N265_RDOQ_LAT_SZ` | 0 | src/encoder/ctu.c:1015 |
| `N265_RDOQ_TRACE` | presence | src/encoder/ctu.c:865 |
| `N265_RECT_FLAT` | 1 | src/encoder/ctu.c:2570 |
| `N265_RECT_RANK` | 50 | src/encoder/ctu.c:2535 |
| `N265_REF_LAG_HINT` | -1 | src/encoder/encoder.c:6921 |
| `N265_RSKIP` | 2 | src/encoder/ctu.c:2170 |
| `N265_SAO_ANIM` | 1 | src/encoder/sao.c:475 |
| `N265_SAO_BIAS` | 100 | src/encoder/sao.c:522 |
| `N265_SAO_CHROMA` | 1 | src/encoder/sao.c:677 |
| `N265_SAO_GATE` | 0 | src/encoder/sao.c:726 |
| `N265_SAO_QPLAM` | 0 | src/encoder/sao.c:512 |
| `N265_SAO_TRACE` | unset | src/encoder/sao.c:819 |
| `N265_SAO_TYPES` | 0 | src/encoder/sao.c:438 |
| `N265_SATD8` | 0 | src/encoder/satd.c:138 |
| `N265_SBH_RD` | 2 | src/encoder/rdoq.c:289 |
| `N265_SC_FORCE` | ? | src/encoder/la.c:769 |
| `N265_SC_HOLD` | 1 | src/encoder/la.c:467 |
| `N265_SC_TRACE` | presence | src/encoder/la.c:1659 |
| `N265_SERIAL_PROF` | 0 | src/encoder/encoder.c:3259 |
| `N265_SHOT_STATS` | unset | src/encoder/encoder.c:2133 |
| `N265_SHOT_TRACE` | presence | src/encoder/encoder.c:8378 |
| `N265_SHOT_WINDOW` | 0 | src/encoder/encoder.c:2313 |
| `N265_SKIP_EXIT` | 0 | src/encoder/ctu.c:2308 |
| `N265_SKIP_THR` | 2 | src/encoder/ctu.c:2069 |
| `N265_STALL_TRACE` | presence | src/encoder/encoder.c:3748 |
| `N265_SUBME` | 3 | src/encoder/mv.c:674 |
| `N265_SUBPEL_DEDUP` | 1 | src/encoder/mv.c:119 |
| `N265_SUBPEL_GATE` | 0 | src/encoder/mv.c:746 |
| `N265_SUBPEL_TRACE` | presence | src/encoder/ctu.c:3960 |
| `N265_SZ_TRACE` | presence | src/encoder/encoder.c:2756 |
| `N265_TASK_TRACE` | unset | src/encoder/ttrace.c:31 |
| `N265_TREE_RAMP` | 4.0 | src/encoder/encoder.c:2271 |
| `N265_UHD_QP` | 29 | src/encoder/encoder.c:5385 |
| `N265_VBR_OFFB` | 0.0 | src/encoder/encoder.c:2757 |
| `N265_VBR_OPEN` | 0 | src/encoder/rc.c:439 |
| `N265_VBR_SZM` | 0 | src/encoder/rc.c:419 |
| `N265_VBV_BRAKE` | 1 under --bitrate with a VBV, else 0 | src/encoder/rc.c:373 |
| `N265_VBV_BRAKE_FILL` | 0.9 | src/encoder/rc.c:405 |
| `N265_VBV_BRAKE_K0` | 0.05 | src/encoder/rc.c:402 |
| `N265_VBV_HRD` | 1 | src/encoder/encoder.c:2759 |
| `N265_VBV_SCPRED` | 0.5 under --bitrate, else 1.0 | src/encoder/rc.c:408 |
| `N265_VBV_TRACE` | presence | src/encoder/encoder.c:2755 |
| `N265_WEIGHTB` | 0 | src/encoder/encoder.c:2583 |
| `N265_WEIGHTP` | 0 | src/encoder/encoder.c:2581 |
| `N265_WP_GAIN` | 4 | src/encoder/wp.c:78 |
| `N265_WP_REFS` | 5 | src/encoder/wp.c:117 |
| `N265_WP_TRACE` | 0 | src/encoder/wp.c:51 |
| `N265_WP_WIN` | 40 | src/encoder/wp.c:101 |

## arm (97)

| knob | default | reader |
|---|---|---|
| `N265_ABR_IOFF_MAX` | 6.0 | src/encoder/rc.c:338 |
| `N265_AQ` | presence | src/encoder/encoder.c:2173 |
| `N265_AQ2` | 0 | src/encoder/encoder.c:2003 |
| `N265_AQ2_FLAT` | 0 | src/encoder/encoder.c:2004 |
| `N265_AQ2_MREF` | 0.0 | src/encoder/encoder.c:2007 |
| `N265_AQ_BPULL` | 0.0 | src/encoder/encoder.c:2011 |
| `N265_AQ_BPULL_ABR` | 0.0 | src/encoder/encoder.c:2015 |
| `N265_AQ_BPULL_NR` | 0 | src/encoder/encoder.c:2012 |
| `N265_AQ_FLAT` | presence | src/encoder/encoder.c:6425 |
| `N265_AQ_FLAT_THR` | = ungated | src/encoder/encoder.c:6698 |
| `N265_BADAPT_DEG` | 1.6 | src/encoder/encoder.c:8558 |
| `N265_BADAPT_FASTW` | 0.0 | src/encoder/encoder.c:8510 |
| `N265_BADAPT_MVMIN` | 0.0 | src/encoder/encoder.c:8511 |
| `N265_CB64_GATEQ` | 0 | src/encoder/ctu.c:3484 |
| `N265_CBQPOFFS` | 0 | src/encoder/encoder.c:2547 |
| `N265_CDZ` | 0 | src/encoder/ctu.c:1036 |
| `N265_CHROMA_W_PCT` | 100 | src/encoder/ctu.c:72 |
| `N265_CPRED_W` | 100 | src/encoder/ctu.c:3202 |
| `N265_CRF_LEGACY` | 0 | src/encoder/encoder.c:1878 |
| `N265_CRQPOFFS` | 0 | src/encoder/encoder.c:2549 |
| `N265_CT_LAMBDA` | 0.0 | src/encoder/encoder.c:1924 |
| `N265_EST_MODE` | 0 | src/encoder/est.c:24 |
| `N265_GOP_CUTS` | 1 | cli/n265.c:2509 |
| `N265_GOP_K` | (--threads / N) + 1, at least 3 | cli/n265.c:2537 |
| `N265_GOP_LEN` | = --keyint | cli/n265.c:2506 |
| `N265_GOP_RELIEF` | 1 | cli/n265.c:2536 |
| `N265_GOP_SEEK` | 1 | cli/n265.c:2538 |
| `N265_GOP_VBV_IN` | 0.5 | cli/n265.c:2527 |
| `N265_GOP_VBV_OUT` | 0.6 | cli/n265.c:2528 |
| `N265_GOP_WINDOW` | (N + 1.5) x N265_GOP_LEN | cli/n265.c:2511 |
| `N265_HDR_CQP` | 0.0 | src/encoder/encoder.c:2563 |
| `N265_HDR_CQP_CB` | ? | src/encoder/encoder.c:2566 |
| `N265_HDR_CQP_CR` | ? | src/encoder/encoder.c:2567 |
| `N265_HDR_LQP` | 0.0 | src/encoder/encoder.c:2172 |
| `N265_INTRA_B_MODES` | 0 | src/encoder/ctu.c:1613 |
| `N265_INTRA_FAST_FLAT` | 0 | src/encoder/ctu.c:1756 |
| `N265_INTRA_FAST_QP` | 0 | src/encoder/ctu.c:1721 |
| `N265_INTRA_FAST_STEP` | 4 | src/encoder/ctu.c:1731 |
| `N265_IRANK` | 0 | src/encoder/ctu.c:1790 |
| `N265_JIT` | 0 | src/encoder/la.c:925 |
| `N265_KEY_DEFER` | 0 | src/encoder/encoder.c:8112 |
| `N265_K_CAP` | 0 | src/encoder/encoder.c:3074 |
| `N265_MCSTF` | 0 | src/encoder/encoder.c:2383 |
| `N265_MCSTF_CTF` | 1.0 | src/encoder/encoder.c:2400 |
| `N265_MCSTF_CTSC` | 1.0 | src/encoder/encoder.c:2402 |
| `N265_MCSTF_DC` | 0 | src/encoder/mcstf.c:259 |
| `N265_MCSTF_QPB` | 0.0 | src/encoder/encoder.c:2396 |
| `N265_MCSTF_QPB_I` | 0 | src/encoder/encoder.c:2398 |
| `N265_MCSTF_RANGE` | 2 | src/encoder/encoder.c:2388 |
| `N265_MEREUSE` | 0 | src/encoder/encoder.c:2112 |
| `N265_NO_AMVP` | presence | src/encoder/ctu.c:178 |
| `N265_NO_EARLYSKIP` | presence | src/encoder/ctu.c:190 |
| `N265_NO_GATES` | presence | src/encoder/ctu.c:172 |
| `N265_NO_TMVP` | presence | src/encoder/encoder.c:3981 |
| `N265_OLD_BS` | presence | src/encoder/deblock.c:296 |
| `N265_PSY_DARK` | 0.0 | src/encoder/encoder.c:2164 |
| `N265_PSY_DARK_LO` | 0.0 | src/encoder/encoder.c:2171 |
| `N265_RDOQ_TAIL` | 0 | src/encoder/rdoq.c:506 |
| `N265_RD_PROXY` | 0 | src/encoder/ctu.c:5924 |
| `N265_REF2_LITE` | 0 | src/encoder/ctu.c:3521 |
| `N265_REF2_ROLE` | 0 | src/encoder/encoder.c:8939 |
| `N265_RUNG_GATE` | -2 | src/encoder/mv.c:803 |
| `N265_RUNG_RERANK` | 0 | src/encoder/mv.c:819 |
| `N265_SC_CONJ` | 3 | src/encoder/ctu.c:2129 |
| `N265_SC_FLOOR` | 0.0 | src/encoder/la.c:737 |
| `N265_SHOT_CLAMP` | 2.0 | src/encoder/encoder.c:2332 |
| `N265_SHOT_EMA` | 0.5 | src/encoder/encoder.c:2337 |
| `N265_SHOT_K` | 1.0 | src/encoder/encoder.c:2331 |
| `N265_STATIC_COMMIT` | 0 | src/encoder/ctu.c:2105 |
| `N265_SUBME_RUNG` | 0 | src/encoder/mv.c:789 |
| `N265_SUBPEL_AXIS` | 0 | src/encoder/mv.c:884 |
| `N265_SUBPEL_RATE` | 0 | src/encoder/mv.c:702 |
| `N265_TD_DIST` | 0 | src/encoder/ctu.c:1000 |
| `N265_TT_SKIP_INTRA` | 0 | src/encoder/ctu.c:2207 |
| `N265_TT_SKIP_QP` | 0 | src/encoder/ctu.c:2219 |
| `N265_UHD_REF2` | ? | src/encoder/preset.c:1436 |
| `N265_VBR_DEFPT_SPEND` | 0.0 | src/encoder/rc.c:296 |
| `N265_VBR_LAG_GFLOOR` | 0.0 | src/encoder/encoder.c:3173 |
| `N265_VBR_LAG_GRAW` | 0 | src/encoder/encoder.c:3175 |
| `N265_VBR_LAG_GUARD` | 0.0 | src/encoder/encoder.c:3171 |
| `N265_VBR_LAG_POSTI` | 0 | src/encoder/encoder.c:3179 |
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
