# The N265_* knob census

This file is generated. Edit the OVERRIDES table in `scripts/knob_census.py`, then regenerate with `python3 scripts/knob_census.py`. CI runs it with `--check`.

The tiers:

- default: shipped behavior. The env is its escape.
- instrument: measurement machinery.
- arm: a default-off gate or a refuted arm. I keep a refuted arm because it can be priced again.

In the default column, "presence" means the reader is a boolean: unset is off and any value arms it. "unset" means the value names a file or a string, and unset is off. `= N265_X` means the default is another knob's resolved value.

## default (216)

| knob | default | reader |
|---|---|---|
| `N265_2P_CLAMP` | 6.0 | src/encoder/encoder.c:891 |
| `N265_2P_GAIN` | 0.25 | src/encoder/rc.c:362 |
| `N265_2P_TRUST` | 2.0 | src/encoder/encoder.c:898 |
| `N265_ABR_COPEN` | 1 | src/encoder/rc.c:345 |
| `N265_ABR_CTF` | 1 | src/encoder/rc.c:349 |
| `N265_ABR_FBMAX` | 6.0 | src/encoder/rc.c:188 |
| `N265_ABR_FBSTEP` | 1 | src/encoder/rc.c:347 |
| `N265_ABR_HORIZON` | 6.0 | src/encoder/rc.c:180 |
| `N265_ABR_IBRAKE` | 1 | src/encoder/rc.c:346 |
| `N265_ABR_IGAIN` | 0.085 | src/encoder/rc.c:186 |
| `N265_ABR_IMAX` | 8.0 | src/encoder/rc.c:187 |
| `N265_ABR_LVL` | 2.0 | src/encoder/rc.c:375 |
| `N265_ABR_MEAS` | 1 | src/encoder/rc.c:331 |
| `N265_ABR_OPENSTEP` | 6.0 | src/encoder/rc.c:185 |
| `N265_ABR_QCOMP` | 0.6 | src/encoder/rc.c:179 |
| `N265_ABR_SB` | 1.7 | src/encoder/rc.c:374 |
| `N265_ABR_SBR` | 4.0 | src/encoder/rc.c:373 |
| `N265_ABR_SI` | 25.4 | src/encoder/rc.c:371 |
| `N265_ABR_SMEM` | 16.0 | src/encoder/rc.c:184 |
| `N265_ABR_SP` | 13.3 | src/encoder/rc.c:372 |
| `N265_ABR_STEP` | 1.0 | src/encoder/rc.c:183 |
| `N265_ABR_TCAP` | 1 | src/encoder/rc.c:343 |
| `N265_ABR_WMEM` | 1000.0 | src/encoder/rc.c:330 |
| `N265_AQ2_ABR` | 1 | src/encoder/encoder.c:2302 |
| `N265_AQ2_GMAX` | 1.0 | src/encoder/encoder.c:2307 |
| `N265_AQ2_GMIN` | 0.7 | src/encoder/encoder.c:2306 |
| `N265_AQ2_SPOW` | 1.0 | src/encoder/encoder.c:2305 |
| `N265_AQ2_SREF` | 2.5 | src/encoder/encoder.c:2303 |
| `N265_AQ_ANCHOR` | 6.4 | src/encoder/encoder.c:6967 |
| `N265_AQ_CLAMP` | 3.0 | src/encoder/encoder.c:7117 |
| `N265_AQ_FLOOR` | 4 | src/encoder/encoder.c:6918 |
| `N265_AQ_POOL` | 1 | src/encoder/encoder.c:2811 |
| `N265_AQ_SIGNAL` | 1 | src/encoder/encoder.c:2295 |
| `N265_AQ_STR` | 0.7 | src/encoder/encoder.c:2290 |
| `N265_BADAPT_MVCAP` | 4 | src/encoder/la.c:567 |
| `N265_BADAPT_MVGATE` | 20 | src/encoder/la.c:602 |
| `N265_BADAPT_REFW` | 128.0 | src/encoder/encoder.c:9064 |
| `N265_BADAPT_THR` | 0.35 | src/encoder/encoder.c:9063 |
| `N265_BI_GATE` | 4 | src/encoder/ctu.c:3856 |
| `N265_B_INTRA_MARGIN` | 30 | src/encoder/ctu.c:3790 |
| `N265_CB64_GATE` | 1 | src/encoder/ctu.c:3757 |
| `N265_CB64_GATEQ_REF` | -1 | src/encoder/ctu.c:3534 |
| `N265_CB64_SKIPGATE_REF` | -1 | src/encoder/ctu.c:3486 |
| `N265_CBF_FAST` | 2 | src/encoder/ctu.c:2282 |
| `N265_CCRF_BRAKE` | 1 under capped CRF, else unread | src/encoder/rc.c:407 |
| `N265_CCRF_BRAKE_FILL` | 0.8 under capped CRF, else unread | src/encoder/rc.c:421 |
| `N265_CCRF_BRK_R0` | 1.25 | src/encoder/rc.c:408 |
| `N265_CCRF_GUARD` | 1.3 under capped CRF, else 0 | src/encoder/rc.c:415 |
| `N265_CCRF_SKIP` | 1 under capped CRF, else 0 | src/encoder/rc.c:410 |
| `N265_CCRF_SKIP_FILL` | 0.8 | src/encoder/rc.c:411 |
| `N265_CCRF_SKIP_SATQ` | 51.0 | src/encoder/rc.c:412 |
| `N265_CCRF_SKIP_WARM` | 4.0 | src/encoder/rc.c:413 |
| `N265_CHROMA_PRED` | 1 | src/encoder/ctu.c:130 |
| `N265_CLAIM_PRIO` | 1 | src/encoder/encoder.c:7431 |
| `N265_CPRED_GATE` | 2 | src/encoder/ctu.c:3253 |
| `N265_CRF_DC_ANCHOR` | 3 | src/encoder/encoder.c:2197 |
| `N265_CRF_DC_BLACK` | 20.0 | src/encoder/encoder.c:2208 |
| `N265_CRF_DC_BOUND` | 5.0 | src/encoder/encoder.c:2209 |
| `N265_CRF_DC_LOW` | 3.0 | src/encoder/encoder.c:2211 |
| `N265_CRF_DC_MEM` | 1000.0 | src/encoder/encoder.c:2201 |
| `N265_CRF_DC_RETAKE` | 4.0 | src/encoder/encoder.c:2210 |
| `N265_CRF_DC_WIN` | 12.0 | src/encoder/encoder.c:2203 |
| `N265_CRF_DUR` | 0.3 | src/encoder/encoder.c:2171 |
| `N265_CRF_DUR_FPS` | 25.0 | src/encoder/encoder.c:2172 |
| `N265_CRF_SHIFT` | 5.4 | src/encoder/encoder.c:5841 |
| `N265_CT_FRAMEW` | 1.0 | src/encoder/encoder.c:2230 |
| `N265_CT_FRAME_LEVEL` | 1 | src/encoder/la.c:2397 |
| `N265_CT_MVRATE` | 16 | src/encoder/la.c:643 |
| `N265_CT_QPW` | 2.0 | src/encoder/encoder.c:2222 |
| `N265_CT_UP_GATE` | 0.24 | src/encoder/encoder.c:2265 |
| `N265_CUTREE_STR` | 2.0 | src/encoder/encoder.c:2221 |
| `N265_DECIMATE` | 15 | src/encoder/rdoq.c:317 |
| `N265_DRV_PAR` | 1 | src/encoder/encoder.c:3297 |
| `N265_ES_THR` | 100 | src/encoder/la.c:1950 |
| `N265_FGC_CHROMA` | 1 | src/encoder/grain.c:132 |
| `N265_FGC_CORE` | 1.0 | src/encoder/grain.c:142 |
| `N265_FGC_FRAMES` | 1 | src/encoder/grain.c:120 |
| `N265_FGC_MIN` | 1.0 | src/encoder/grain.c:180 |
| `N265_FGC_PCT` | 25 | src/encoder/grain.c:108 |
| `N265_FGC_RATIO` | 2.0 | src/encoder/grain.c:166 |
| `N265_FLAT_GATE` | 10 | src/encoder/la.c:140 |
| `N265_GATE_SCALE` | 100 | src/encoder/ctu.c:2298 |
| `N265_HDR_FULL` | 1 on PQ and HLG at medium | src/encoder/preset.c:1202 |
| `N265_HD_QP` | 27 | src/encoder/encoder.c:5817 |
| `N265_HF2` | 3 | src/encoder/encoder.c:4435 |
| `N265_HF2_TPP` | 150 | src/encoder/encoder.c:4463 |
| `N265_HF2_TPP14` | 190 | src/encoder/encoder.c:4474 |
| `N265_HFCACHE` | 1 | src/encoder/encoder.c:4372 |
| `N265_INTRA_ADAPT_K` | 32 | src/encoder/encoder.c:2418 |
| `N265_INTRA_ADAPT_THR` | 100 | src/encoder/encoder.c:2421 |
| `N265_INTRA_FAST` | 0 | src/encoder/ctu.c:1706 |
| `N265_INTRA_FAST_W` | 1280 | src/encoder/ctu.c:1744 |
| `N265_INTRA_GATE` | 7 | src/encoder/ctu.c:2434 |
| `N265_INTRA_MIN` | 4 | src/encoder/encoder.c:8561 |
| `N265_INTRA_RD_N` | 3 | src/encoder/ctu.c:4757 |
| `N265_INTRA_REUSE` | 1 | src/encoder/ctu.c:4852 |
| `N265_JIT_COH` | 0.75 | src/encoder/la.c:957 |
| `N265_JIT_IC` | 720.0 | src/encoder/la.c:964 |
| `N265_JIT_J` | 2.2 | src/encoder/la.c:950 |
| `N265_JIT_RDOQ` | 1 | src/encoder/encoder.c:2444 |
| `N265_JIT_WIN` | 24 | src/encoder/la.c:939 |
| `N265_KEY_RESET` | 1 | src/encoder/encoder.c:8619 |
| `N265_LAMBDA` | 0.68 | src/encoder/ctu.c:2052 |
| `N265_LA_MEPRED` | 1 | src/encoder/la.c:834 |
| `N265_LS` | 1 | src/encoder/rc.c:467 |
| `N265_LS_AMIN` | 1.0 | src/encoder/rc.c:478 |
| `N265_LS_CAP` | 0.25 with no buffer | src/encoder/rc.c:495 |
| `N265_LS_OPEN` | 1 with no buffer, else 0 | src/encoder/rc.c:493 |
| `N265_LS_QHI` | 21.0 | src/encoder/rc.c:481 |
| `N265_LS_QLO` | 15.0 | src/encoder/rc.c:480 |
| `N265_LS_QRAMP` | 2 | src/encoder/rc.c:479 |
| `N265_LS_VBVUP` | 1 | src/encoder/rc.c:468 |
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
| `N265_MERGE_PICK_RATE` | 0 | src/encoder/ctu.c:5814 |
| `N265_MERGE_RD_N` | 5 | src/encoder/ctu.c:3827 |
| `N265_ME_PATTERN` | 0 | src/encoder/mv.c:945 |
| `N265_MIN_KEYINT` | 4 | src/encoder/encoder.c:8543 |
| `N265_NO_AMP` | 1 | src/encoder/encoder.c:2340 |
| `N265_NO_RECT` | 1 | src/encoder/encoder.c:2338 |
| `N265_PART_GATE` | 2 | src/encoder/ctu.c:2516 |
| `N265_PB_STEP` | 1.0 | src/encoder/encoder.c:5858 |
| `N265_PSY_DARK_M` | 64.0 | src/encoder/encoder.c:2464 |
| `N265_PSY_DARK_QHI` | 40.0 | src/encoder/encoder.c:2462 |
| `N265_PSY_DARK_QW` | 12.0 | src/encoder/encoder.c:2463 |
| `N265_PSY_DARK_V` | 16.0 | src/encoder/encoder.c:2465 |
| `N265_QP_ROUND1` | 1 | src/encoder/encoder.c:2297 |
| `N265_QP_TDITHER` | 1 | src/encoder/encoder.c:2824 |
| `N265_RC_MVRATE` | 32 | src/encoder/rc.c:136 |
| `N265_RDOQ` | 1 | src/encoder/encoder.c:2440 |
| `N265_RDOQ_LAM` | 100 | src/encoder/rdoq.c:311 |
| `N265_RDOQ_UP` | 1 | src/encoder/rdoq.c:299 |
| `N265_READAHEAD_MB` | 64 | cli/y265.c:935 |
| `N265_RECT_FLAT` | 1 | src/encoder/ctu.c:2594 |
| `N265_RECT_RANK` | 50 | src/encoder/ctu.c:2559 |
| `N265_REF_LAG_HINT` | -1 | src/encoder/encoder.c:7409 |
| `N265_ROW_YIELD` | 5000 where 2 * min(rows, (cols + 1) / 2) > threads > 1, else 0 | src/encoder/encoder.c:3447 |
| `N265_RSKIP` | 2 | src/encoder/ctu.c:2195 |
| `N265_SAO_ANIM` | 1 | src/encoder/sao.c:473 |
| `N265_SAO_BIAS` | 100 | src/encoder/sao.c:519 |
| `N265_SAO_CHROMA` | 1 | src/encoder/sao.c:674 |
| `N265_SBH_RD` | 2 | src/encoder/rdoq.c:290 |
| `N265_SCREEN` | 0 | src/encoder/la.c:287 |
| `N265_SC_FAILW` | 0.40 | src/encoder/la.c:682 |
| `N265_SC_FAILW_SPIKE` | 1.6 | src/encoder/la.c:699 |
| `N265_SC_HOLD` | 1 | src/encoder/la.c:467 |
| `N265_SC_JUMP` | 0.10 | src/encoder/la.c:729 |
| `N265_SC_SPIKE` | 1.8 | src/encoder/la.c:813 |
| `N265_SKIP_THR` | 2 | src/encoder/ctu.c:2095 |
| `N265_SUBME` | 3 | src/encoder/mv.c:723 |
| `N265_SUBPEL_DEDUP` | 1 | src/encoder/mv.c:119 |
| `N265_TREE_K` | 12 | src/encoder/encoder.c:3377 |
| `N265_TREE_RAMP` | 4.0 | src/encoder/encoder.c:2568 |
| `N265_TREE_SLOTS` | K - 6 (with the switched HD or UHD tree only) | src/encoder/encoder.c:3462 |
| `N265_TT_SKIP_INTER` | 0 | src/encoder/ctu.c:2222 |
| `N265_UHD_CVBR` | 1 under capped VBR at UHD at medium | src/encoder/preset.c:1215 |
| `N265_UHD_QP` | 29 | src/encoder/encoder.c:5797 |
| `N265_VBR_CMEM` | 1 | src/encoder/rc.c:341 |
| `N265_VBR_CTKEEP` | 1 under capped VBR, else 0 | src/encoder/rc.c:256 |
| `N265_VBR_CTLPAY` | 0.02 | src/encoder/rc.c:326 |
| `N265_VBR_CTLSEG` | 1 | src/encoder/rc.c:327 |
| `N265_VBR_DEFPT` | 0.08 | src/encoder/rc.c:292 |
| `N265_VBR_DEFPT_CAP` | 6.0 | src/encoder/rc.c:299 |
| `N265_VBR_DEFPT_HOLD` | 2.0 | src/encoder/rc.c:302 |
| `N265_VBR_DEFPT_MEM` | 1000.0 | src/encoder/rc.c:306 |
| `N265_VBR_DEFPT_RAIL` | 1 | src/encoder/rc.c:296 |
| `N265_VBR_DEFPT_REL` | 0.05 | src/encoder/rc.c:294 |
| `N265_VBR_DEFPT_SLEW` | 0.25 | src/encoder/rc.c:298 |
| `N265_VBR_DEFPT_T0` | 60.0 | src/encoder/rc.c:297 |
| `N265_VBR_DEFPT_W` | 0.02 | src/encoder/rc.c:293 |
| `N265_VBR_FBFINE` | 3.0 | src/encoder/rc.c:280 |
| `N265_VBR_FBG` | 16.0 | src/encoder/rc.c:253 |
| `N265_VBR_FBGUP` | 12.0 | src/encoder/rc.c:254 |
| `N265_VBR_FBT0` | 2.0 | src/encoder/rc.c:252 |
| `N265_VBR_FBTRK` | 0.06 | src/encoder/rc.c:283 |
| `N265_VBR_FBTRK_MAX` | 6.0 | src/encoder/rc.c:285 |
| `N265_VBR_FBTRK_W` | 0.04 | src/encoder/rc.c:284 |
| `N265_VBR_FBUP` | = N265_ABR_FBMAX (capped VBR only) | src/encoder/rc.c:223 |
| `N265_VBR_FBW` | 2.0 under capped VBR, else 0 | src/encoder/rc.c:251 |
| `N265_VBR_FILLGOAL` | 0.5 | src/encoder/rc.c:321 |
| `N265_VBR_FILLQ` | 6.0 | src/encoder/rc.c:320 |
| `N265_VBR_LAG` | 7 (VBR, threads > 1) | src/encoder/encoder.c:3408 |
| `N265_VBR_LAG_BUFS` | 1.2 | src/encoder/encoder.c:3417 |
| `N265_VBR_LAG_CAL` | 0.1 | src/encoder/encoder.c:3478 |
| `N265_VBR_LAG_LOW` | 0.5 | src/encoder/encoder.c:3484 |
| `N265_VBR_LAG_PEND` | 1 | src/encoder/encoder.c:3501 |
| `N265_VBR_LAG_POSTI_RAISE` | 3.0 | src/encoder/encoder.c:3496 |
| `N265_VBR_LAG_WARM` | 2 * (lag + 1) | src/encoder/encoder.c:3476 |
| `N265_VBR_OVW` | 0.75 under capped VBR, else 0 | src/encoder/rc.c:255 |
| `N265_VBR_SPENDFILL` | 0.4 | src/encoder/rc.c:278 |
| `N265_VBR_SPENDHI` | 0.8 | src/encoder/rc.c:279 |
| `N265_VBR_VLVL` | 0.1 | src/encoder/rc.c:310 |
| `N265_VBV_BRAKE_FILL` | 0.9 | src/encoder/rc.c:419 |
| `N265_VBV_BRAKE_K0` | 0.05 | src/encoder/rc.c:416 |
| `N265_VBV_DECAY` | 0.0625 | src/encoder/rc.c:384 |
| `N265_VBV_HRD` | 1 | src/encoder/encoder.c:3074 |
| `N265_VBV_LOOKAHEAD` | 1 | src/encoder/encoder.c:2982 |
| `N265_VBV_OWN` | 3 under capped VBR, else 0 | src/encoder/rc.c:257 |
| `N265_VBV_RESERVE` | 0.2 | src/encoder/encoder.c:2989 |
| `N265_VBV_SAFE` | 0.85 | src/encoder/encoder.c:2975 |
| `N265_WP_GAIN` | 4 | src/encoder/wp.c:78 |
| `N265_WP_REFS` | 5 | src/encoder/wp.c:117 |
| `N265_WP_WIN` | 40 | src/encoder/wp.c:101 |

## instrument (56)

| knob | default | reader |
|---|---|---|
| `N265_2P_TRACE` | presence | src/encoder/encoder.c:906 |
| `N265_AQ2_TRACE` | presence | src/encoder/encoder.c:7220 |
| `N265_AQ_TRACE` | presence | src/encoder/encoder.c:7225 |
| `N265_BADAPT_TRACE` | presence | src/encoder/la.c:1827 |
| `N265_BRUN_TRACE` | presence | src/encoder/la.c:849 |
| `N265_CB64_PROF` | presence | src/encoder/ctu.c:3728 |
| `N265_CB64_TRACE` | presence | src/encoder/ctu.c:195 |
| `N265_CBIT_TRACE` | presence | src/encoder/encoder.c:2410 |
| `N265_CPRED_TRACE` | presence | src/encoder/ctu.c:5622 |
| `N265_CT_TRACE` | presence | src/encoder/la.c:2490 |
| `N265_CU_STATS` | presence | src/encoder/encoder.c:2406 |
| `N265_CU_TRACE` | unset | src/encoder/encoder.c:1739 |
| `N265_DECGRAD_TRACE` | presence | src/encoder/rdoq.c:363 |
| `N265_DEPTH_DUMP` | unset | src/encoder/encoder.c:1726 |
| `N265_DPB_TRACE` | presence | src/encoder/encoder.c:2384 |
| `N265_DUP_TRACE` | presence | src/encoder/la.c:869 |
| `N265_FFMPEG` | ffmpeg | cli/y265.c:2810 |
| `N265_FGC_TRACE` | presence | src/encoder/grain.c:191 |
| `N265_FLAT_TRACE` | presence | src/encoder/encoder.c:6824 |
| `N265_FORCE_NONIDR` | -1 | src/encoder/encoder.c:8572 |
| `N265_FP_CENSUS` | presence | src/encoder/encoder.c:2407 |
| `N265_GOP_TRACE` | presence | cli/y265.c:2173 |
| `N265_HF2_TRACE` | presence | src/encoder/encoder.c:4486 |
| `N265_INTRA_ORACLE` | 0 | src/encoder/ctu.c:2463 |
| `N265_INTRA_TRY_TRACE` | presence | src/encoder/ctu.c:1647 |
| `N265_INTRA_WIN_TRACE` | presence | src/encoder/encoder.c:2411 |
| `N265_IR_TRACE` | presence | src/encoder/encoder.c:2843 |
| `N265_JIT_TRACE` | presence | src/encoder/la.c:931 |
| `N265_LA_MVSTAT` | presence | src/encoder/la.c:1492 |
| `N265_LA_PROF` | presence | src/encoder/encoder.c:3577 |
| `N265_LEVEL_TRACE` | presence | src/encoder/encoder.c:3269 |
| `N265_MCSTF_TRACE` | presence | src/encoder/mcstf.c:126 |
| `N265_MEREUSE_TRACE` | presence | src/encoder/encoder.c:2408 |
| `N265_MERGE_TRACE` | -1 | src/encoder/mv.c:107 |
| `N265_NO_SPLIT` | presence | src/encoder/ctu.c:183 |
| `N265_QP_MAP_TRACE` | unset | src/encoder/encoder.c:7356 |
| `N265_QP_TRACE` | presence | src/encoder/encoder.c:7379 |
| `N265_RC_TRACE` | presence | src/encoder/encoder.c:906 |
| `N265_RDOQ_TRACE` | presence | src/encoder/ctu.c:863 |
| `N265_SAO_TRACE` | unset | src/encoder/sao.c:814 |
| `N265_SC_FORCE` | ? | src/encoder/la.c:768 |
| `N265_SC_TRACE` | presence | src/encoder/la.c:1669 |
| `N265_SERIAL_PROF` | 0 | src/encoder/encoder.c:3574 |
| `N265_SHOT_STATS` | unset | src/encoder/encoder.c:2430 |
| `N265_SHOT_TRACE` | presence | src/encoder/encoder.c:8908 |
| `N265_STALL_TRACE` | presence | src/encoder/encoder.c:4082 |
| `N265_SUBPEL_TRACE` | presence | src/encoder/ctu.c:3996 |
| `N265_SZ_TRACE` | presence | src/encoder/encoder.c:3071 |
| `N265_TASK_TRACE` | unset | src/encoder/ttrace.c:31 |
| `N265_VBV_TRACE` | presence | src/encoder/encoder.c:3070 |
| `N265_VMAF_BIN` | ~/.local/opt/vmaf-v1/bin/vmaf, else vmaf | cli/y265.c:2803 |
| `N265_VMAF_HDR_BIN` | ~/.local/opt/vmaf-hdr/bin/vmaf | cli/y265.c:2796 |
| `N265_VMAF_HDR_MODEL_DIR` | ~/.local/share/vmaf-hdr | cli/y265.c:2799 |
| `N265_VMAF_MODEL_DIR` | ~/.local/share/vmaf/model | cli/y265.c:2805 |
| `N265_VMAF_THREADS` | = --threads | cli/y265.c:2812 |
| `N265_WP_TRACE` | 0 | src/encoder/wp.c:51 |

## arm (176)

| knob | default | reader |
|---|---|---|
| `N265_2P_QCOMP` | = N265_ABR_QCOMP | src/encoder/encoder.c:889 |
| `N265_ABR_CMEM` | ? | src/encoder/rc.c:340 |
| `N265_ABR_CSEED` | 0.0 | src/encoder/rc.c:344 |
| `N265_ABR_FBUP` | = N265_VBR_FBUP under capped VBR (item 37), 1.5 under another VBV, else = N265_ABR_FBMAX | src/encoder/rc.c:222 |
| `N265_ABR_FLATC` | 17.0 under a VBV, else 0 | src/encoder/rc.c:417 |
| `N265_ABR_IOFF` | 0.0 | src/encoder/rc.c:350 |
| `N265_ABR_IOFF_CT` | 1.0 under a VBV without N265_VBR_CTKEEP, else 0 | src/encoder/rc.c:351 |
| `N265_ABR_IOFF_MAX` | 6.0 | src/encoder/rc.c:353 |
| `N265_ABR_MGUARD` | 0.0 | src/encoder/rc.c:332 |
| `N265_ABR_OPENI` | 0 | src/encoder/rc.c:348 |
| `N265_ABR_PLANERR` | 0 | src/encoder/rc.c:225 |
| `N265_API_DEFER` | presence | src/encoder/encoder.c:3572 |
| `N265_AQ` | presence | src/encoder/encoder.c:2470 |
| `N265_AQ2` | 0 | src/encoder/encoder.c:2300 |
| `N265_AQ2_FLAT` | 0 | src/encoder/encoder.c:2301 |
| `N265_AQ2_MREF` | 0.0 | src/encoder/encoder.c:2304 |
| `N265_AQ_BPULL` | 0.0 | src/encoder/encoder.c:2308 |
| `N265_AQ_BPULL_ABR` | 0.0 | src/encoder/encoder.c:2312 |
| `N265_AQ_BPULL_NR` | 0 | src/encoder/encoder.c:2309 |
| `N265_AQ_CLAMP_FLAT` | 0.0 | src/encoder/encoder.c:7147 |
| `N265_AQ_DC` | ? | src/encoder/encoder.c:6967 |
| `N265_AQ_FLAT` | presence | src/encoder/encoder.c:6851 |
| `N265_AQ_FLAT_SIGN` | 0 | src/encoder/encoder.c:2296 |
| `N265_AQ_FLAT_THR` | = ungated | src/encoder/encoder.c:7186 |
| `N265_ASM_OFF` | unset | src/common/cpu.c:200 |
| `N265_BADAPT_DEG` | 1.6 | src/encoder/encoder.c:9120 |
| `N265_BADAPT_FASTW` | 0.0 | src/encoder/encoder.c:9072 |
| `N265_BADAPT_MVMIN` | 0.0 | src/encoder/encoder.c:9073 |
| `N265_BI_REFINE` | 0 | src/encoder/ctu.c:3808 |
| `N265_CB64_AMVP` | 0 | src/encoder/ctu.c:3775 |
| `N265_CB64_FLATPIC` | 0 | src/encoder/ctu.c:3506 |
| `N265_CB64_GATEQ` | 0 | src/encoder/ctu.c:3520 |
| `N265_CB64_SKIPGATE` | 0 | src/encoder/ctu.c:3468 |
| `N265_CB64_SPLITGATE` | 0 | src/encoder/ctu.c:3626 |
| `N265_CBQPOFFS` | 0 | src/encoder/encoder.c:2863 |
| `N265_CDZ` | 0 | src/encoder/ctu.c:1034 |
| `N265_CHROMA_W` | 0 | src/encoder/ctu.c:65 |
| `N265_CHROMA_W_PCT` | 100 | src/encoder/ctu.c:71 |
| `N265_CPRED_W` | 100 | src/encoder/ctu.c:3226 |
| `N265_CPU_MAX` | unset | src/common/cpu.c:197 |
| `N265_CRF_LEGACY` | 0 | src/encoder/encoder.c:2168 |
| `N265_CRQPOFFS` | 0 | src/encoder/encoder.c:2865 |
| `N265_CT_CENTRE` | 0 | src/encoder/la.c:2349 |
| `N265_CT_FRAMEW_FLAT` | = N265_CT_QPW | src/encoder/encoder.c:2234 |
| `N265_CT_FRAME_CLAMP` | 0 | src/encoder/encoder.c:2238 |
| `N265_CT_FRAME_SHOT` | 0 | src/encoder/encoder.c:2240 |
| `N265_CT_FRAME_UP` | 0 | src/encoder/encoder.c:2248 |
| `N265_CT_LAMBDA` | 0.0 | src/encoder/encoder.c:2223 |
| `N265_DBF_BETA` | 0 | src/encoder/encoder.c:2850 |
| `N265_DBF_TC` | 0 | src/encoder/encoder.c:2851 |
| `N265_DECIMATE_GRAD` | = N265_DECIMATE | src/encoder/rdoq.c:330 |
| `N265_DEPTH_BOUND` | 0 | src/encoder/ctu.c:2642 |
| `N265_DPB_HOLD` | 0 | src/encoder/encoder.c:1163 |
| `N265_DUP_FAST` | 0 | src/encoder/la.c:859 |
| `N265_DUP_THR` | 0.0 | src/encoder/la.c:897 |
| `N265_EST_MODE` | 0 | src/encoder/est.c:24 |
| `N265_FULLPEL` | presence | src/encoder/mv.c:113 |
| `N265_GOP_CUTS` | 1 | cli/y265.c:2197 |
| `N265_GOP_K` | (--threads / N) + 1, at least 3 | cli/y265.c:2237 |
| `N265_GOP_LEN` | = --keyint | cli/y265.c:2194 |
| `N265_GOP_RELIEF` | 1 | cli/y265.c:2236 |
| `N265_GOP_SEEK` | 1 | cli/y265.c:2238 |
| `N265_GOP_VBV_IN` | 0.5 | cli/y265.c:2227 |
| `N265_GOP_VBV_OUT` | 0.6 | cli/y265.c:2228 |
| `N265_GOP_WINDOW` | (N + 1.5) x N265_GOP_LEN | cli/y265.c:2211 |
| `N265_HDR_CQP` | 0.0 | src/encoder/encoder.c:2879 |
| `N265_HDR_CQP_CB` | ? | src/encoder/encoder.c:2882 |
| `N265_HDR_CQP_CR` | ? | src/encoder/encoder.c:2883 |
| `N265_HDR_LQP` | 0.0 | src/encoder/encoder.c:2469 |
| `N265_HW` | off | src/encoder/encoder.c:1954 |
| `N265_HW_SC` | ? | src/encoder/encoder.c:1978 |
| `N265_INTER_NXN` | 0 | src/encoder/ctu.c:4798 |
| `N265_INTRA_B_MODES` | 0 | src/encoder/ctu.c:1611 |
| `N265_INTRA_FAST_FLAT` | 0 | src/encoder/ctu.c:1754 |
| `N265_INTRA_FAST_QP` | 0 | src/encoder/ctu.c:1719 |
| `N265_INTRA_FAST_STEP` | 4 | src/encoder/ctu.c:1729 |
| `N265_INTRA_MPM` | 0 | src/encoder/ctu.c:4768 |
| `N265_INTRA_RSKIP` | 0 | src/encoder/ctu.c:2208 |
| `N265_IRANK` | 0 | src/encoder/ctu.c:1807 |
| `N265_IR_NOBOUND` | 0 | src/encoder/encoder.c:2835 |
| `N265_IR_NOEDGE` | 0 | src/encoder/encoder.c:2841 |
| `N265_IR_NOMARGIN` | 0 | src/encoder/encoder.c:2837 |
| `N265_IR_TMVP` | 0 | src/encoder/encoder.c:2839 |
| `N265_I_INTRA` | 0 | src/encoder/ctu.c:4784 |
| `N265_JIT` | 0 | src/encoder/la.c:924 |
| `N265_KEY_DEFER` | 0 | src/encoder/encoder.c:8634 |
| `N265_K_CAP` | 0 | src/encoder/encoder.c:3389 |
| `N265_LS_CAP_VBV` | 0 under a buffer | src/encoder/rc.c:494 |
| `N265_LS_RAIL` | 0.0 | src/encoder/rc.c:482 |
| `N265_MAX_FRAME_WORK` | 6 with --bframes >= 2, else 4 | src/encoder/encoder.c:3299 |
| `N265_MCSTF` | 0 | src/encoder/encoder.c:2680 |
| `N265_MCSTF_CTF` | 1.0 | src/encoder/encoder.c:2697 |
| `N265_MCSTF_CTSC` | 1.0 | src/encoder/encoder.c:2699 |
| `N265_MCSTF_DC` | 0 | src/encoder/mcstf.c:259 |
| `N265_MCSTF_QPB` | 0.0 | src/encoder/encoder.c:2693 |
| `N265_MCSTF_QPB_I` | 0 | src/encoder/encoder.c:2695 |
| `N265_MCSTF_RANGE` | 2 | src/encoder/encoder.c:2685 |
| `N265_MEREUSE` | 0 | src/encoder/encoder.c:2409 |
| `N265_ME_SATD` | 0 | src/encoder/ctu.c:3384 |
| `N265_MODE_HIST` | presence | src/encoder/sao.c:418 |
| `N265_MVD_BITS` | 0 | src/encoder/ctu.c:3372 |
| `N265_NO_AMVP` | presence | src/encoder/ctu.c:177 |
| `N265_NO_EARLYSKIP` | presence | src/encoder/ctu.c:189 |
| `N265_NO_GATES` | presence | src/encoder/ctu.c:171 |
| `N265_NO_SAO` | presence | src/encoder/encoder.c:2405 |
| `N265_NO_TMVP` | presence | src/encoder/encoder.c:4318 |
| `N265_NR_INTER` | 0 | src/encoder/encoder.c:2741 |
| `N265_NR_INTRA` | 0 | src/encoder/encoder.c:2739 |
| `N265_OLD_BS` | presence | src/encoder/deblock.c:296 |
| `N265_OPENGOP_CUT` | 0 | src/encoder/encoder.c:1227 |
| `N265_OPENGOP_NOBARRIER` | 0 | src/encoder/encoder.c:1234 |
| `N265_PB_CASCADE` | auto | src/encoder/encoder.c:2271 |
| `N265_PB_CASCADE_FLAT` | auto | src/encoder/encoder.c:2280 |
| `N265_PSY` | 0.0 | src/encoder/encoder.c:2445 |
| `N265_PSY_DARK` | 0.0 | src/encoder/encoder.c:2461 |
| `N265_PSY_DARK_LO` | 0.0 | src/encoder/encoder.c:2468 |
| `N265_QP_FRAC` | = --bitrate or CRF+frame term | src/encoder/encoder.c:2792 |
| `N265_QP_FRAC_FLAT` | 0 | src/encoder/encoder.c:2805 |
| `N265_RC_LAG` | presence | src/encoder/encoder.c:3407 |
| `N265_RDOQ_ADAPT` | 0 | src/encoder/rdoq.c:305 |
| `N265_RDOQ_LAT_SZ` | 0 | src/encoder/ctu.c:1013 |
| `N265_RDOQ_TAIL` | 0 | src/encoder/rdoq.c:511 |
| `N265_RD_PROXY` | 0 | src/encoder/ctu.c:6008 |
| `N265_REF2_LITE` | 0 | src/encoder/ctu.c:3557 |
| `N265_REF2_ROLE` | 0 | src/encoder/encoder.c:9501 |
| `N265_REPORT_SCRIPT` | ? | cli/y265.c:4269 |
| `N265_RUNG_GATE` | -2 | src/encoder/mv.c:852 |
| `N265_RUNG_RERANK` | 0 | src/encoder/mv.c:868 |
| `N265_SAO_GATE` | 0 | src/encoder/sao.c:722 |
| `N265_SAO_QPLAM` | 0 | src/encoder/sao.c:509 |
| `N265_SAO_TYPES` | 0 | src/encoder/sao.c:437 |
| `N265_SATD8` | 0 | src/encoder/satd.c:138 |
| `N265_SC_CONJ` | 3 | src/encoder/ctu.c:2155 |
| `N265_SC_FLOOR` | 0.0 | src/encoder/la.c:736 |
| `N265_SHOT_CLAMP` | 2.0 | src/encoder/encoder.c:2629 |
| `N265_SHOT_EMA` | 0.5 | src/encoder/encoder.c:2634 |
| `N265_SHOT_K` | 1.0 | src/encoder/encoder.c:2628 |
| `N265_SHOT_WINDOW` | 0 | src/encoder/encoder.c:2610 |
| `N265_SKIP_EXIT` | 0 | src/encoder/ctu.c:2333 |
| `N265_STATIC_COMMIT` | 0 | src/encoder/ctu.c:2131 |
| `N265_SUBME_RUNG` | 0 | src/encoder/mv.c:838 |
| `N265_SUBPEL_AXIS` | 0 | src/encoder/mv.c:933 |
| `N265_SUBPEL_GATE` | 0 | src/encoder/mv.c:795 |
| `N265_SUBPEL_RATE` | 0 | src/encoder/mv.c:751 |
| `N265_TD_DIST` | 0 | src/encoder/ctu.c:998 |
| `N265_TT_SKIP_INTRA` | 0 | src/encoder/ctu.c:2232 |
| `N265_TT_SKIP_QP` | 0 | src/encoder/ctu.c:2244 |
| `N265_UHD_REF2` | ? | src/encoder/preset.c:1431 |
| `N265_VBR_DEFPT_SPEND` | 0.0 | src/encoder/rc.c:303 |
| `N265_VBR_LAG_GFLOOR` | 0.0 | src/encoder/encoder.c:3488 |
| `N265_VBR_LAG_GRAW` | 0 | src/encoder/encoder.c:3490 |
| `N265_VBR_LAG_GUARD` | 0.0 | src/encoder/encoder.c:3486 |
| `N265_VBR_LAG_POSTI` | 0 | src/encoder/encoder.c:3494 |
| `N265_VBR_OFFB` | 0.0 | src/encoder/encoder.c:3072 |
| `N265_VBR_OPEN` | 0 | src/encoder/rc.c:452 |
| `N265_VBR_OPEN_AB` | -1.238 | src/encoder/rc.c:456 |
| `N265_VBR_OPEN_ABR` | -0.806 | src/encoder/rc.c:455 |
| `N265_VBR_OPEN_AI` | -7.027 | src/encoder/rc.c:453 |
| `N265_VBR_OPEN_AP` | 2.084 | src/encoder/rc.c:454 |
| `N265_VBR_OPEN_BB` | 1.224 | src/encoder/rc.c:460 |
| `N265_VBR_OPEN_BBR` | 1.374 | src/encoder/rc.c:459 |
| `N265_VBR_OPEN_BI` | 2.157 | src/encoder/rc.c:457 |
| `N265_VBR_OPEN_BP` | 1.249 | src/encoder/rc.c:458 |
| `N265_VBR_OPEN_G` | -1.429 | src/encoder/rc.c:461 |
| `N265_VBR_SPEND` | 0.0 | src/encoder/rc.c:277 |
| `N265_VBR_SZM` | 0 | src/encoder/rc.c:432 |
| `N265_VBR_SZM_GMIN` | 0.7 | src/encoder/rc.c:440 |
| `N265_VBR_SZM_LAM` | 0.9 | src/encoder/rc.c:433 |
| `N265_VBR_SZM_SIMPLE` | 150.0 | src/encoder/rc.c:442 |
| `N265_VBR_SZM_STATIC` | 17.0 | src/encoder/rc.c:441 |
| `N265_VBR_SZM_Z` | 2.0 | src/encoder/rc.c:434 |
| `N265_VBR_SZM_ZLAG` | 1.0 | src/encoder/rc.c:439 |
| `N265_VBV_BRAKE` | 1 under --bitrate with a VBV, else 0 | src/encoder/rc.c:388 |
| `N265_VBV_SCPRED` | 0.5 under --bitrate, else 1.0 | src/encoder/rc.c:422 |
| `N265_WEIGHTB` | 0 | src/encoder/encoder.c:2899 |
| `N265_WEIGHTP` | 0 | src/encoder/encoder.c:2897 |
