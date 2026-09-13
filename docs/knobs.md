# The N265_* knob census (generated -- edit scripts/knob_census.py OVERRIDES, not this file)

Regenerate: `python3 scripts/knob_census.py`. CI check: `--check`.

Tier meanings: **default** = shipped behaviour, the env is its escape; **instrument** = measurement machinery (catalogued in docs/instruments.md when it has produced a result); **arm** = default-off gate or kept refuted arm -- kept deliberately because a refuted arm retains its re-pricing value.

## default (7)

| knob | default | reader |
|---|---|---|
| `N265_ABR_SB` | 1.7 | src/encoder/rc.c:157 |
| `N265_ABR_SBR` | 4.0 | src/encoder/rc.c:156 |
| `N265_ABR_SP` | 13.3 | src/encoder/rc.c:155 |
| `N265_GATE_SCALE` | 100 | src/encoder/ctu.c:1502 |
| `N265_RDOQ` | 1 | src/encoder/encoder.c:966 |
| `N265_RDOQ_UP` | 1 | src/encoder/rdoq.c:288 |
| `N265_SKIP_THR` | 2 | src/encoder/ctu.c:1424 |

## instrument (144)

| knob | default | reader |
|---|---|---|
| `N265_ABR_CMEM` | 1000.0 | src/encoder/rc.c:135 |
| `N265_ABR_COPEN` | 1 | src/encoder/rc.c:138 |
| `N265_ABR_CSEED` | 0.0 | src/encoder/rc.c:137 |
| `N265_ABR_CTF` | 1 | src/encoder/rc.c:142 |
| `N265_ABR_FBMAX` | 6.0 | src/encoder/rc.c:129 |
| `N265_ABR_FBSTEP` | 1 | src/encoder/rc.c:140 |
| `N265_ABR_FBUP` | ? | src/encoder/rc.c:130 |
| `N265_ABR_HORIZON` | 6.0 | src/encoder/rc.c:121 |
| `N265_ABR_IBRAKE` | 1 | src/encoder/rc.c:139 |
| `N265_ABR_IGAIN` | 0.085 | src/encoder/rc.c:127 |
| `N265_ABR_IMAX` | 8.0 | src/encoder/rc.c:128 |
| `N265_ABR_IOFF` | 0.0 | src/encoder/rc.c:143 |
| `N265_ABR_IOFF_CT` | 0.0 | src/encoder/rc.c:144 |
| `N265_ABR_LVL` | 2.0 | src/encoder/rc.c:158 |
| `N265_ABR_MEAS` | 1 | src/encoder/rc.c:133 |
| `N265_ABR_MGUARD` | 0.0 | src/encoder/rc.c:134 |
| `N265_ABR_OPENI` | 0 | src/encoder/rc.c:141 |
| `N265_ABR_OPENSTEP` | 6.0 | src/encoder/rc.c:126 |
| `N265_ABR_PLANERR` | 0 | src/encoder/rc.c:131 |
| `N265_ABR_QCOMP` | 0.6 | src/encoder/rc.c:120 |
| `N265_ABR_SI` | 25.4 | src/encoder/rc.c:154 |
| `N265_ABR_SMEM` | 16.0 | src/encoder/rc.c:125 |
| `N265_ABR_STEP` | 1.0 | src/encoder/rc.c:124 |
| `N265_ABR_TCAP` | 1 | src/encoder/rc.c:136 |
| `N265_ABR_WMEM` | 1000.0 | src/encoder/rc.c:132 |
| `N265_API_DEFER` | ? | src/encoder/encoder.c:1297 |
| `N265_AQ_ANCHOR` | ? | src/encoder/encoder.c:2871 |
| `N265_AQ_CLAMP` | ? | src/encoder/encoder.c:2894 |
| `N265_AQ_CLAMP_FLAT` | ? | src/encoder/encoder.c:2918 |
| `N265_AQ_DC` | ? | src/encoder/encoder.c:2871 |
| `N265_AQ_FLAT_SIGN` | 0 | src/encoder/encoder.c:881 |
| `N265_AQ_FLOOR` | 4 | src/encoder/encoder.c:2779 |
| `N265_AQ_SIGNAL` | 1 | src/encoder/encoder.c:880 |
| `N265_AQ_STR` | ? | src/encoder/encoder.c:874 |
| `N265_AQ_TRACE` | ? | src/encoder/encoder.c:2939 |
| `N265_ASM_OFF` | unset | src/common/cpu.c:110 |
| `N265_BADAPT_REFW` | ? | src/encoder/encoder.c:3895 |
| `N265_BADAPT_THR` | ? | src/encoder/encoder.c:3894 |
| `N265_BADAPT_TRACE` | ? | src/encoder/la.c:606 |
| `N265_BI_GATE` | 4 | src/encoder/ctu.c:2652 |
| `N265_BI_REFINE` | 0 | src/encoder/ctu.c:2604 |
| `N265_BRUN_TRACE` | ? | src/encoder/la.c:253 |
| `N265_B_INTRA_MARGIN` | 30 | src/encoder/ctu.c:2586 |
| `N265_CB64_AMVP` | 0 | src/encoder/ctu.c:2571 |
| `N265_CB64_GATE` | 1 | src/encoder/ctu.c:2553 |
| `N265_CB64_TRACE` | ? | src/encoder/ctu.c:190 |
| `N265_CBF_FAST` | 2 | src/encoder/ctu.c:1486 |
| `N265_CBIT_TRACE` | ? | src/encoder/encoder.c:944 |
| `N265_CHROMA_PRED` | 1 | src/encoder/ctu.c:125 |
| `N265_CHROMA_W` | 0 | src/encoder/ctu.c:65 |
| `N265_CPRED_GATE` | 2 | src/encoder/ctu.c:2373 |
| `N265_CPRED_TRACE` | ? | src/encoder/ctu.c:3705 |
| `N265_CRF_SHIFT` | 5.4 | src/encoder/encoder.c:2524 |
| `N265_CT_CENTRE` | 0 | src/encoder/la.c:986 |
| `N265_CT_FRAMEW` | 1.0 | src/encoder/encoder.c:813 |
| `N265_CT_FRAMEW_FLAT` | = N265_CT_QPW | src/encoder/encoder.c:817 |
| `N265_CT_FRAME_CLAMP` | 0 | src/encoder/encoder.c:821 |
| `N265_CT_FRAME_LEVEL` | 1 | src/encoder/la.c:1035 |
| `N265_CT_FRAME_SHOT` | 0 | src/encoder/encoder.c:823 |
| `N265_CT_FRAME_UP` | 0 | src/encoder/encoder.c:831 |
| `N265_CT_QPW` | ? | src/encoder/encoder.c:805 |
| `N265_CT_TRACE` | ? | src/encoder/la.c:1133 |
| `N265_CT_UP_GATE` | 0.24 | src/encoder/encoder.c:849 |
| `N265_CUTREE_STR` | ? | src/encoder/encoder.c:804 |
| `N265_CU_STATS` | ? | src/encoder/encoder.c:943 |
| `N265_CU_TRACE` | ? | src/encoder/encoder.c:628 |
| `N265_DBF_BETA` | 0 | src/encoder/encoder.c:1090 |
| `N265_DBF_TC` | 0 | src/encoder/encoder.c:1091 |
| `N265_DECGRAD_TRACE` | ? | src/encoder/rdoq.c:353 |
| `N265_DECIMATE` | 15 | src/encoder/rdoq.c:306 |
| `N265_DECIMATE_GRAD` | = N265_DECIMATE | src/encoder/rdoq.c:320 |
| `N265_DEPTH_BOUND` | 0 | src/encoder/ctu.c:1847 |
| `N265_DEPTH_DUMP` | ? | src/encoder/encoder.c:615 |
| `N265_DPB_HOLD` | 0 | src/encoder/encoder.c:211 |
| `N265_DPB_TRACE` | ? | src/encoder/encoder.c:921 |
| `N265_ES_THR` | 100 | src/encoder/la.c:729 |
| `N265_FLAT_GATE` | 10 | src/encoder/la.c:102 |
| `N265_FLAT_TRACE` | ? | src/encoder/encoder.c:2723 |
| `N265_FORCE_NONIDR` | -1 | src/encoder/encoder.c:3594 |
| `N265_HF2` | 3 | src/encoder/encoder.c:1743 |
| `N265_HF2_TPP` | 150 | src/encoder/encoder.c:1771 |
| `N265_HF2_TPP14` | 190 | src/encoder/encoder.c:1782 |
| `N265_HF2_TRACE` | ? | src/encoder/encoder.c:1794 |
| `N265_HFCACHE` | 1 | src/encoder/encoder.c:1679 |
| `N265_INTRA_ADAPT_K` | 32 | src/encoder/encoder.c:952 |
| `N265_INTRA_ADAPT_THR` | 100 | src/encoder/encoder.c:955 |
| `N265_INTRA_GATE` | 7 | src/encoder/ctu.c:1639 |
| `N265_INTRA_MIN` | 4 | src/encoder/encoder.c:3583 |
| `N265_INTRA_MPM` | 0 | src/encoder/ctu.c:3083 |
| `N265_INTRA_ORACLE` | 0 | src/encoder/ctu.c:1668 |
| `N265_INTRA_RD_N` | 3 | src/encoder/ctu.c:3072 |
| `N265_INTRA_WIN_TRACE` | ? | src/encoder/encoder.c:945 |
| `N265_I_INTRA` | 0 | src/encoder/ctu.c:3099 |
| `N265_LA_PROF` | 0.0 | src/encoder/encoder.c:1302 |
| `N265_LEVEL_TRACE` | ? | src/encoder/encoder.c:1157 |
| `N265_MAX_FRAME_WORK` | ? | src/encoder/encoder.c:1180 |
| `N265_MERGE_RD_N` | ? | src/encoder/ctu.c:2623 |
| `N265_MERGE_TRACE` | -1 | src/encoder/mv.c:98 |
| `N265_ME_PATTERN` | 0 | src/encoder/mv.c:552 |
| `N265_ME_SATD` | 0 | src/encoder/ctu.c:2504 |
| `N265_MIN_KEYINT` | 4 | src/encoder/encoder.c:3565 |
| `N265_MODE_HIST` | ? | src/encoder/sao.c:252 |
| `N265_MVD_BITS` | 0 | src/encoder/ctu.c:2492 |
| `N265_NO_AMP` | 1 | src/encoder/encoder.c:909 |
| `N265_NO_RECT` | 1 | src/encoder/encoder.c:907 |
| `N265_NO_SPLIT` | ? | src/encoder/ctu.c:178 |
| `N265_PART_GATE` | 2 | src/encoder/ctu.c:1721 |
| `N265_PB_CASCADE` | auto | src/encoder/encoder.c:855 |
| `N265_PB_CASCADE_FLAT` | auto | src/encoder/encoder.c:864 |
| `N265_PSY` | 0.0 | src/encoder/encoder.c:967 |
| `N265_QP_FRAC` | = --bitrate or CRF+frame term | src/encoder/encoder.c:1070 |
| `N265_QP_FRAC_FLAT` | 0 | src/encoder/encoder.c:1083 |
| `N265_QP_MAP_TRACE` | ? | src/encoder/encoder.c:3002 |
| `N265_QP_ROUND1` | 1 | src/encoder/encoder.c:882 |
| `N265_QP_TRACE` | ? | src/encoder/encoder.c:3018 |
| `N265_RC_LAG` | 0 | src/encoder/encoder.c:1233 |
| `N265_RC_TRACE` | ? | src/encoder/rc.c:164 |
| `N265_RDOQ_ADAPT` | 0 | src/encoder/rdoq.c:294 |
| `N265_RDOQ_LAM` | 100 | src/encoder/rdoq.c:300 |
| `N265_RDOQ_LAT_SZ` | 0 | src/encoder/ctu.c:831 |
| `N265_RDOQ_TRACE` | ? | src/encoder/ctu.c:699 |
| `N265_RECT_FLAT` | 1 | src/encoder/ctu.c:1799 |
| `N265_RECT_RANK` | 50 | src/encoder/ctu.c:1764 |
| `N265_REF_LAG_HINT` | -1 | src/encoder/encoder.c:3048 |
| `N265_RSKIP` | 2 | src/encoder/ctu.c:1466 |
| `N265_SAO_ANIM` | 1 | src/encoder/sao.c:308 |
| `N265_SAO_BIAS` | 100 | src/encoder/sao.c:344 |
| `N265_SAO_CHROMA` | 1 | src/encoder/sao.c:500 |
| `N265_SAO_GATE` | 0 | src/encoder/sao.c:549 |
| `N265_SAO_QPLAM` | 0 | src/encoder/sao.c:334 |
| `N265_SAO_TRACE` | ? | src/encoder/sao.c:642 |
| `N265_SAO_TYPES` | 0 | src/encoder/sao.c:271 |
| `N265_SATD8` | 0 | src/encoder/satd.c:134 |
| `N265_SBH_RD` | 2 | src/encoder/rdoq.c:279 |
| `N265_SC_HOLD` | 1 | src/encoder/la.c:238 |
| `N265_SC_TRACE` | ? | src/encoder/la.c:452 |
| `N265_SERIAL_PROF` | 0 | src/encoder/encoder.c:1299 |
| `N265_SHOT_STATS` | ? | src/encoder/encoder.c:962 |
| `N265_SHOT_TRACE` | ? | src/encoder/encoder.c:3790 |
| `N265_SHOT_WINDOW` | 0 | src/encoder/encoder.c:1020 |
| `N265_SKIP_EXIT` | 0 | src/encoder/ctu.c:1537 |
| `N265_STALL_TRACE` | ? | src/encoder/encoder.c:1488 |
| `N265_SUBME` | 3 | src/encoder/mv.c:540 |
| `N265_SUBPEL_DEDUP` | 1 | src/encoder/mv.c:110 |

## arm (19)

| knob | default | reader |
|---|---|---|
| `N265_ABR_IOFF_MAX` | 6.0 | src/encoder/rc.c:145 |
| `N265_AQ` | ? | src/encoder/encoder.c:968 |
| `N265_AQ_FLAT` | ? | src/encoder/encoder.c:2740 |
| `N265_BADAPT_DEG` | ? | src/encoder/encoder.c:3920 |
| `N265_CHROMA_W_PCT` | 100 | src/encoder/ctu.c:71 |
| `N265_CPRED_W` | 100 | src/encoder/ctu.c:2346 |
| `N265_CT_LAMBDA` | ? | src/encoder/encoder.c:806 |
| `N265_FULLPEL` | ? | src/encoder/mv.c:104 |
| `N265_LAMBDA` | ? | src/encoder/ctu.c:1381 |
| `N265_NO_AMVP` | ? | src/encoder/ctu.c:172 |
| `N265_NO_EARLYSKIP` | ? | src/encoder/ctu.c:184 |
| `N265_NO_GATES` | ? | src/encoder/ctu.c:166 |
| `N265_NO_SAO` | ? | src/encoder/encoder.c:942 |
| `N265_NO_TMVP` | ? | src/encoder/encoder.c:1631 |
| `N265_OLD_BS` | ? | src/encoder/deblock.c:154 |
| `N265_PB_STEP` | 0 | src/encoder/encoder.c:2527 |
| `N265_SHOT_CLAMP` | 2.0 | src/encoder/encoder.c:1039 |
| `N265_SHOT_EMA` | 0.5 | src/encoder/encoder.c:1044 |
| `N265_SHOT_K` | 1.0 | src/encoder/encoder.c:1038 |
