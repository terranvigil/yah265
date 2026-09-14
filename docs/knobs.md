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
| `N265_RDOQ` | 1 | src/encoder/encoder.c:977 |
| `N265_RDOQ_UP` | 1 | src/encoder/rdoq.c:288 |
| `N265_SKIP_THR` | 2 | src/encoder/ctu.c:1424 |

## instrument (145)

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
| `N265_API_DEFER` | ? | src/encoder/encoder.c:1308 |
| `N265_AQ_ANCHOR` | ? | src/encoder/encoder.c:2917 |
| `N265_AQ_CLAMP` | ? | src/encoder/encoder.c:2940 |
| `N265_AQ_CLAMP_FLAT` | ? | src/encoder/encoder.c:2970 |
| `N265_AQ_DC` | ? | src/encoder/encoder.c:2917 |
| `N265_AQ_FLAT_SIGN` | 0 | src/encoder/encoder.c:892 |
| `N265_AQ_FLOOR` | 4 | src/encoder/encoder.c:2802 |
| `N265_AQ_SIGNAL` | 1 | src/encoder/encoder.c:891 |
| `N265_AQ_STR` | ? | src/encoder/encoder.c:885 |
| `N265_AQ_TRACE` | ? | src/encoder/encoder.c:3021 |
| `N265_ASM_OFF` | unset | src/common/cpu.c:110 |
| `N265_BADAPT_REFW` | ? | src/encoder/encoder.c:3983 |
| `N265_BADAPT_THR` | ? | src/encoder/encoder.c:3982 |
| `N265_BADAPT_TRACE` | ? | src/encoder/la.c:606 |
| `N265_BI_GATE` | 4 | src/encoder/ctu.c:2652 |
| `N265_BI_REFINE` | 0 | src/encoder/ctu.c:2604 |
| `N265_BRUN_TRACE` | ? | src/encoder/la.c:253 |
| `N265_B_INTRA_MARGIN` | 30 | src/encoder/ctu.c:2586 |
| `N265_CB64_AMVP` | 0 | src/encoder/ctu.c:2571 |
| `N265_CB64_GATE` | 1 | src/encoder/ctu.c:2553 |
| `N265_CB64_TRACE` | ? | src/encoder/ctu.c:190 |
| `N265_CBF_FAST` | 2 | src/encoder/ctu.c:1486 |
| `N265_CBIT_TRACE` | ? | src/encoder/encoder.c:955 |
| `N265_CHROMA_PRED` | 1 | src/encoder/ctu.c:125 |
| `N265_CHROMA_W` | 0 | src/encoder/ctu.c:65 |
| `N265_CPRED_GATE` | 2 | src/encoder/ctu.c:2373 |
| `N265_CPRED_TRACE` | ? | src/encoder/ctu.c:3771 |
| `N265_CRF_SHIFT` | 5.4 | src/encoder/encoder.c:2535 |
| `N265_CT_CENTRE` | 0 | src/encoder/la.c:986 |
| `N265_CT_FRAMEW` | 1.0 | src/encoder/encoder.c:824 |
| `N265_CT_FRAMEW_FLAT` | = N265_CT_QPW | src/encoder/encoder.c:828 |
| `N265_CT_FRAME_CLAMP` | 0 | src/encoder/encoder.c:832 |
| `N265_CT_FRAME_LEVEL` | 1 | src/encoder/la.c:1035 |
| `N265_CT_FRAME_SHOT` | 0 | src/encoder/encoder.c:834 |
| `N265_CT_FRAME_UP` | 0 | src/encoder/encoder.c:842 |
| `N265_CT_QPW` | ? | src/encoder/encoder.c:816 |
| `N265_CT_TRACE` | ? | src/encoder/la.c:1133 |
| `N265_CT_UP_GATE` | 0.24 | src/encoder/encoder.c:860 |
| `N265_CUTREE_STR` | ? | src/encoder/encoder.c:815 |
| `N265_CU_STATS` | ? | src/encoder/encoder.c:954 |
| `N265_CU_TRACE` | ? | src/encoder/encoder.c:639 |
| `N265_DBF_BETA` | 0 | src/encoder/encoder.c:1101 |
| `N265_DBF_TC` | 0 | src/encoder/encoder.c:1102 |
| `N265_DECGRAD_TRACE` | ? | src/encoder/rdoq.c:353 |
| `N265_DECIMATE` | 15 | src/encoder/rdoq.c:306 |
| `N265_DECIMATE_GRAD` | = N265_DECIMATE | src/encoder/rdoq.c:320 |
| `N265_DEPTH_BOUND` | 0 | src/encoder/ctu.c:1847 |
| `N265_DEPTH_DUMP` | ? | src/encoder/encoder.c:626 |
| `N265_DPB_HOLD` | 0 | src/encoder/encoder.c:211 |
| `N265_DPB_TRACE` | ? | src/encoder/encoder.c:932 |
| `N265_ES_THR` | 100 | src/encoder/la.c:729 |
| `N265_FLAT_GATE` | 10 | src/encoder/la.c:102 |
| `N265_FLAT_TRACE` | ? | src/encoder/encoder.c:2734 |
| `N265_FORCE_NONIDR` | -1 | src/encoder/encoder.c:3682 |
| `N265_HF2` | 3 | src/encoder/encoder.c:1754 |
| `N265_HF2_TPP` | 150 | src/encoder/encoder.c:1782 |
| `N265_HF2_TPP14` | 190 | src/encoder/encoder.c:1793 |
| `N265_HF2_TRACE` | ? | src/encoder/encoder.c:1805 |
| `N265_HFCACHE` | 1 | src/encoder/encoder.c:1690 |
| `N265_INTRA_ADAPT_K` | 32 | src/encoder/encoder.c:963 |
| `N265_INTRA_ADAPT_THR` | 100 | src/encoder/encoder.c:966 |
| `N265_INTRA_GATE` | 7 | src/encoder/ctu.c:1639 |
| `N265_INTRA_MIN` | 4 | src/encoder/encoder.c:3671 |
| `N265_INTRA_MPM` | 0 | src/encoder/ctu.c:3141 |
| `N265_INTRA_ORACLE` | 0 | src/encoder/ctu.c:1668 |
| `N265_INTRA_RD_N` | 3 | src/encoder/ctu.c:3130 |
| `N265_INTRA_WIN_TRACE` | ? | src/encoder/encoder.c:956 |
| `N265_I_INTRA` | 0 | src/encoder/ctu.c:3157 |
| `N265_LA_PROF` | 0.0 | src/encoder/encoder.c:1313 |
| `N265_LEVEL_TRACE` | ? | src/encoder/encoder.c:1168 |
| `N265_MAX_FRAME_WORK` | ? | src/encoder/encoder.c:1191 |
| `N265_MERGE_RD_N` | ? | src/encoder/ctu.c:2623 |
| `N265_MERGE_TRACE` | -1 | src/encoder/mv.c:99 |
| `N265_ME_PATTERN` | 0 | src/encoder/mv.c:614 |
| `N265_ME_SATD` | 0 | src/encoder/ctu.c:2504 |
| `N265_MIN_KEYINT` | 4 | src/encoder/encoder.c:3653 |
| `N265_MODE_HIST` | ? | src/encoder/sao.c:252 |
| `N265_MVD_BITS` | 0 | src/encoder/ctu.c:2492 |
| `N265_NO_AMP` | 1 | src/encoder/encoder.c:920 |
| `N265_NO_RECT` | 1 | src/encoder/encoder.c:918 |
| `N265_NO_SPLIT` | ? | src/encoder/ctu.c:178 |
| `N265_PART_GATE` | 2 | src/encoder/ctu.c:1721 |
| `N265_PB_CASCADE` | auto | src/encoder/encoder.c:866 |
| `N265_PB_CASCADE_FLAT` | auto | src/encoder/encoder.c:875 |
| `N265_PSY` | 0.0 | src/encoder/encoder.c:978 |
| `N265_QP_FRAC` | = --bitrate or CRF+frame term | src/encoder/encoder.c:1081 |
| `N265_QP_FRAC_FLAT` | 0 | src/encoder/encoder.c:1094 |
| `N265_QP_MAP_TRACE` | ? | src/encoder/encoder.c:3090 |
| `N265_QP_ROUND1` | 1 | src/encoder/encoder.c:893 |
| `N265_QP_TRACE` | ? | src/encoder/encoder.c:3106 |
| `N265_RC_LAG` | 0 | src/encoder/encoder.c:1244 |
| `N265_RC_TRACE` | ? | src/encoder/rc.c:164 |
| `N265_RDOQ_ADAPT` | 0 | src/encoder/rdoq.c:294 |
| `N265_RDOQ_LAM` | 100 | src/encoder/rdoq.c:300 |
| `N265_RDOQ_LAT_SZ` | 0 | src/encoder/ctu.c:831 |
| `N265_RDOQ_TRACE` | ? | src/encoder/ctu.c:699 |
| `N265_RECT_FLAT` | 1 | src/encoder/ctu.c:1799 |
| `N265_RECT_RANK` | 50 | src/encoder/ctu.c:1764 |
| `N265_REF_LAG_HINT` | -1 | src/encoder/encoder.c:3136 |
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
| `N265_SERIAL_PROF` | 0 | src/encoder/encoder.c:1310 |
| `N265_SHOT_STATS` | ? | src/encoder/encoder.c:973 |
| `N265_SHOT_TRACE` | ? | src/encoder/encoder.c:3878 |
| `N265_SHOT_WINDOW` | 0 | src/encoder/encoder.c:1031 |
| `N265_SKIP_EXIT` | 0 | src/encoder/ctu.c:1537 |
| `N265_STALL_TRACE` | ? | src/encoder/encoder.c:1499 |
| `N265_SUBME` | 3 | src/encoder/mv.c:541 |
| `N265_SUBPEL_DEDUP` | 1 | src/encoder/mv.c:111 |
| `N265_SUBPEL_TRACE` | ? | src/encoder/ctu.c:2791 |

## arm (21)

| knob | default | reader |
|---|---|---|
| `N265_ABR_IOFF_MAX` | 6.0 | src/encoder/rc.c:145 |
| `N265_AQ` | ? | src/encoder/encoder.c:979 |
| `N265_AQ_FLAT` | ? | src/encoder/encoder.c:2751 |
| `N265_AQ_FLAT_THR` | = ungated | src/encoder/encoder.c:3009 |
| `N265_BADAPT_DEG` | ? | src/encoder/encoder.c:4008 |
| `N265_CHROMA_W_PCT` | 100 | src/encoder/ctu.c:71 |
| `N265_CPRED_W` | 100 | src/encoder/ctu.c:2346 |
| `N265_CT_LAMBDA` | ? | src/encoder/encoder.c:817 |
| `N265_FULLPEL` | ? | src/encoder/mv.c:105 |
| `N265_LAMBDA` | ? | src/encoder/ctu.c:1381 |
| `N265_NO_AMVP` | ? | src/encoder/ctu.c:172 |
| `N265_NO_EARLYSKIP` | ? | src/encoder/ctu.c:184 |
| `N265_NO_GATES` | ? | src/encoder/ctu.c:166 |
| `N265_NO_SAO` | ? | src/encoder/encoder.c:953 |
| `N265_NO_TMVP` | ? | src/encoder/encoder.c:1642 |
| `N265_OLD_BS` | ? | src/encoder/deblock.c:154 |
| `N265_PB_STEP` | 0 | src/encoder/encoder.c:2538 |
| `N265_SHOT_CLAMP` | 2.0 | src/encoder/encoder.c:1050 |
| `N265_SHOT_EMA` | 0.5 | src/encoder/encoder.c:1055 |
| `N265_SHOT_K` | 1.0 | src/encoder/encoder.c:1049 |
| `N265_SUBPEL_RATE` | 0 | src/encoder/mv.c:569 |
