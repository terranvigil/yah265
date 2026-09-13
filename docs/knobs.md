# The N265_* knob census (generated -- edit scripts/knob_census.py OVERRIDES, not this file)

Regenerate: `python3 scripts/knob_census.py`. CI check: `--check`.

Tier meanings: **default** = shipped behaviour, the env is its escape; **instrument** = measurement machinery (catalogued in docs/instruments.md when it has produced a result); **arm** = default-off gate or kept refuted arm -- kept deliberately because a refuted arm retains its re-pricing value.

## default (9)

| knob | default | reader |
|---|---|---|
| `N265_ABR_SB` | 1.7 | src/encoder/rc.c:154 |
| `N265_ABR_SBR` | 4.0 | src/encoder/rc.c:153 |
| `N265_ABR_SP` | 13.3 | src/encoder/rc.c:152 |
| `N265_GATE_SCALE` | 100 | src/encoder/ctu.c:1384 |
| `N265_INTRA_ADAPT_K` | 32 | src/encoder/encoder.c:930 |
| `N265_INTRA_ADAPT_THR` | 100 | src/encoder/encoder.c:933 |
| `N265_RDOQ` | 1 | src/encoder/encoder.c:944 |
| `N265_RDOQ_UP` | 1 | src/encoder/rdoq.c:288 |
| `N265_SKIP_THR` | 2 | src/encoder/ctu.c:1306 |

## instrument (138)

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
| `N265_ABR_LVL` | 2.0 | src/encoder/rc.c:155 |
| `N265_ABR_MEAS` | 1 | src/encoder/rc.c:133 |
| `N265_ABR_MGUARD` | 0.0 | src/encoder/rc.c:134 |
| `N265_ABR_OPENI` | 0 | src/encoder/rc.c:141 |
| `N265_ABR_OPENSTEP` | 6.0 | src/encoder/rc.c:126 |
| `N265_ABR_PLANERR` | 0 | src/encoder/rc.c:131 |
| `N265_ABR_QCOMP` | 0.6 | src/encoder/rc.c:120 |
| `N265_ABR_SI` | 25.4 | src/encoder/rc.c:151 |
| `N265_ABR_SMEM` | 16.0 | src/encoder/rc.c:125 |
| `N265_ABR_STEP` | 1.0 | src/encoder/rc.c:124 |
| `N265_ABR_TCAP` | 1 | src/encoder/rc.c:136 |
| `N265_ABR_WMEM` | 1000.0 | src/encoder/rc.c:132 |
| `N265_API_DEFER` | ? | src/encoder/encoder.c:1237 |
| `N265_AQ_ANCHOR` | ? | src/encoder/encoder.c:2743 |
| `N265_AQ_CLAMP` | ? | src/encoder/encoder.c:2766 |
| `N265_AQ_CLAMP_FLAT` | ? | src/encoder/encoder.c:2790 |
| `N265_AQ_DC` | ? | src/encoder/encoder.c:2743 |
| `N265_AQ_FLAT_SIGN` | 0 | src/encoder/encoder.c:859 |
| `N265_AQ_FLOOR` | 4 | src/encoder/encoder.c:2651 |
| `N265_AQ_SIGNAL` | 1 | src/encoder/encoder.c:858 |
| `N265_AQ_STR` | ? | src/encoder/encoder.c:852 |
| `N265_AQ_TRACE` | ? | src/encoder/encoder.c:2811 |
| `N265_ASM_OFF` | unset | src/common/cpu.c:110 |
| `N265_BADAPT_REFW` | ? | src/encoder/encoder.c:3691 |
| `N265_BADAPT_THR` | ? | src/encoder/encoder.c:3690 |
| `N265_BADAPT_TRACE` | ? | src/encoder/la.c:606 |
| `N265_BI_GATE` | 4 | src/encoder/ctu.c:2534 |
| `N265_BI_REFINE` | 0 | src/encoder/ctu.c:2486 |
| `N265_BRUN_TRACE` | ? | src/encoder/la.c:253 |
| `N265_B_INTRA_MARGIN` | 30 | src/encoder/ctu.c:2468 |
| `N265_CB64_AMVP` | 0 | src/encoder/ctu.c:2453 |
| `N265_CB64_GATE` | 1 | src/encoder/ctu.c:2435 |
| `N265_CB64_TRACE` | ? | src/encoder/ctu.c:190 |
| `N265_CBF_FAST` | 2 | src/encoder/ctu.c:1368 |
| `N265_CBIT_TRACE` | ? | src/encoder/encoder.c:922 |
| `N265_CHROMA_PRED` | 1 | src/encoder/ctu.c:125 |
| `N265_CHROMA_W` | 0 | src/encoder/ctu.c:65 |
| `N265_CPRED_GATE` | 2 | src/encoder/ctu.c:2255 |
| `N265_CPRED_TRACE` | ? | src/encoder/ctu.c:3587 |
| `N265_CRF_SHIFT` | 5.4 | src/encoder/encoder.c:2405 |
| `N265_CT_CENTRE` | 0 | src/encoder/la.c:986 |
| `N265_CT_FRAMEW` | 1.0 | src/encoder/encoder.c:791 |
| `N265_CT_FRAMEW_FLAT` | = N265_CT_QPW | src/encoder/encoder.c:795 |
| `N265_CT_FRAME_CLAMP` | 0 | src/encoder/encoder.c:799 |
| `N265_CT_FRAME_LEVEL` | 1 | src/encoder/la.c:1035 |
| `N265_CT_FRAME_SHOT` | 0 | src/encoder/encoder.c:801 |
| `N265_CT_FRAME_UP` | 0 | src/encoder/encoder.c:809 |
| `N265_CT_QPW` | ? | src/encoder/encoder.c:783 |
| `N265_CT_TRACE` | ? | src/encoder/la.c:1133 |
| `N265_CT_UP_GATE` | 0.24 | src/encoder/encoder.c:827 |
| `N265_CUTREE_STR` | ? | src/encoder/encoder.c:782 |
| `N265_CU_STATS` | ? | src/encoder/encoder.c:921 |
| `N265_DBF_BETA` | 0 | src/encoder/encoder.c:1068 |
| `N265_DBF_TC` | 0 | src/encoder/encoder.c:1069 |
| `N265_DECGRAD_TRACE` | ? | src/encoder/rdoq.c:353 |
| `N265_DECIMATE` | 15 | src/encoder/rdoq.c:306 |
| `N265_DECIMATE_GRAD` | = N265_DECIMATE | src/encoder/rdoq.c:320 |
| `N265_DEPTH_BOUND` | 0 | src/encoder/ctu.c:1729 |
| `N265_DEPTH_DUMP` | ? | src/encoder/encoder.c:615 |
| `N265_DPB_HOLD` | 0 | src/encoder/encoder.c:211 |
| `N265_DPB_TRACE` | ? | src/encoder/encoder.c:899 |
| `N265_ES_THR` | 100 | src/encoder/la.c:729 |
| `N265_FLAT_GATE` | 10 | src/encoder/la.c:102 |
| `N265_FLAT_TRACE` | ? | src/encoder/encoder.c:2595 |
| `N265_FORCE_NONIDR` | -1 | src/encoder/encoder.c:3390 |
| `N265_HF2` | 3 | src/encoder/encoder.c:1632 |
| `N265_HF2_TPP` | 150 | src/encoder/encoder.c:1660 |
| `N265_HF2_TPP14` | 190 | src/encoder/encoder.c:1671 |
| `N265_HF2_TRACE` | ? | src/encoder/encoder.c:1683 |
| `N265_HFCACHE` | 1 | src/encoder/encoder.c:1568 |
| `N265_INTRA_GATE` | 7 | src/encoder/ctu.c:1521 |
| `N265_INTRA_MIN` | 4 | src/encoder/encoder.c:3379 |
| `N265_INTRA_MPM` | 0 | src/encoder/ctu.c:2965 |
| `N265_INTRA_ORACLE` | 0 | src/encoder/ctu.c:1550 |
| `N265_INTRA_RD_N` | 3 | src/encoder/ctu.c:2954 |
| `N265_INTRA_WIN_TRACE` | ? | src/encoder/encoder.c:923 |
| `N265_I_INTRA` | 0 | src/encoder/ctu.c:2981 |
| `N265_LA_PROF` | 0.0 | src/encoder/encoder.c:1238 |
| `N265_LEVEL_TRACE` | ? | src/encoder/encoder.c:1135 |
| `N265_MAX_FRAME_WORK` | ? | src/encoder/encoder.c:1158 |
| `N265_MERGE_RD_N` | ? | src/encoder/ctu.c:2505 |
| `N265_MERGE_TRACE` | -1 | src/encoder/mv.c:98 |
| `N265_ME_PATTERN` | 0 | src/encoder/mv.c:552 |
| `N265_ME_SATD` | 0 | src/encoder/ctu.c:2386 |
| `N265_MIN_KEYINT` | 4 | src/encoder/encoder.c:3361 |
| `N265_MODE_HIST` | ? | src/encoder/sao.c:252 |
| `N265_MVD_BITS` | 0 | src/encoder/ctu.c:2374 |
| `N265_NO_AMP` | 1 | src/encoder/encoder.c:887 |
| `N265_NO_RECT` | 1 | src/encoder/encoder.c:885 |
| `N265_NO_SPLIT` | ? | src/encoder/ctu.c:178 |
| `N265_PART_GATE` | 2 | src/encoder/ctu.c:1603 |
| `N265_PB_CASCADE` | auto | src/encoder/encoder.c:833 |
| `N265_PB_CASCADE_FLAT` | auto | src/encoder/encoder.c:842 |
| `N265_PSY` | 0.0 | src/encoder/encoder.c:945 |
| `N265_QP_FRAC` | = --bitrate or CRF+frame term | src/encoder/encoder.c:1048 |
| `N265_QP_FRAC_FLAT` | 0 | src/encoder/encoder.c:1061 |
| `N265_QP_MAP_TRACE` | ? | src/encoder/encoder.c:2874 |
| `N265_QP_ROUND1` | 1 | src/encoder/encoder.c:860 |
| `N265_QP_TRACE` | ? | src/encoder/encoder.c:2890 |
| `N265_RC_LAG` | 0 | src/encoder/encoder.c:1173 |
| `N265_RC_TRACE` | ? | src/encoder/rc.c:161 |
| `N265_RDOQ_ADAPT` | 0 | src/encoder/rdoq.c:294 |
| `N265_RDOQ_LAM` | 100 | src/encoder/rdoq.c:300 |
| `N265_RDOQ_LAT_SZ` | 0 | src/encoder/ctu.c:726 |
| `N265_RDOQ_TRACE` | ? | src/encoder/ctu.c:594 |
| `N265_RECT_FLAT` | 1 | src/encoder/ctu.c:1681 |
| `N265_RECT_RANK` | 50 | src/encoder/ctu.c:1646 |
| `N265_REF_LAG_HINT` | -1 | src/encoder/encoder.c:2920 |
| `N265_RSKIP` | 2 | src/encoder/ctu.c:1348 |
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
| `N265_SHOT_STATS` | ? | src/encoder/encoder.c:940 |
| `N265_SHOT_TRACE` | ? | src/encoder/encoder.c:3586 |
| `N265_SHOT_WINDOW` | 0 | src/encoder/encoder.c:998 |
| `N265_SKIP_EXIT` | 0 | src/encoder/ctu.c:1419 |
| `N265_STALL_TRACE` | ? | src/encoder/encoder.c:1380 |
| `N265_SUBME` | 3 | src/encoder/mv.c:540 |
| `N265_SUBPEL_DEDUP` | 1 | src/encoder/mv.c:110 |

## arm (18)

| knob | default | reader |
|---|---|---|
| `N265_AQ` | ? | src/encoder/encoder.c:946 |
| `N265_AQ_FLAT` | ? | src/encoder/encoder.c:2612 |
| `N265_BADAPT_DEG` | ? | src/encoder/encoder.c:3716 |
| `N265_CHROMA_W_PCT` | 100 | src/encoder/ctu.c:71 |
| `N265_CPRED_W` | 100 | src/encoder/ctu.c:2228 |
| `N265_CT_LAMBDA` | ? | src/encoder/encoder.c:784 |
| `N265_FULLPEL` | ? | src/encoder/mv.c:104 |
| `N265_LAMBDA` | ? | src/encoder/ctu.c:1263 |
| `N265_NO_AMVP` | ? | src/encoder/ctu.c:172 |
| `N265_NO_EARLYSKIP` | ? | src/encoder/ctu.c:184 |
| `N265_NO_GATES` | ? | src/encoder/ctu.c:166 |
| `N265_NO_SAO` | ? | src/encoder/encoder.c:920 |
| `N265_NO_TMVP` | ? | src/encoder/encoder.c:1520 |
| `N265_OLD_BS` | ? | src/encoder/deblock.c:154 |
| `N265_PB_STEP` | 0 | src/encoder/encoder.c:2408 |
| `N265_SHOT_CLAMP` | 2.0 | src/encoder/encoder.c:1017 |
| `N265_SHOT_EMA` | 0.5 | src/encoder/encoder.c:1022 |
| `N265_SHOT_K` | 1.0 | src/encoder/encoder.c:1016 |
