# The N265_* knob census (generated -- edit scripts/knob_census.py OVERRIDES, not this file)

Regenerate: `python3 scripts/knob_census.py`. CI check: `--check`.

Tier meanings: **default** = shipped behaviour, the env is its escape; **instrument** = measurement machinery (catalogued in docs/instruments.md when it has produced a result); **arm** = default-off gate or kept refuted arm -- kept deliberately because a refuted arm retains its re-pricing value.

## default (10)

| knob | default | reader |
|---|---|---|
| `N265_ABR_SB` | 1.7 | src/encoder/rc.c:157 |
| `N265_ABR_SBR` | 4.0 | src/encoder/rc.c:156 |
| `N265_ABR_SP` | 13.3 | src/encoder/rc.c:155 |
| `N265_GATE_SCALE` | 100 | src/encoder/ctu.c:1562 |
| `N265_RDOQ` | 1 | src/encoder/encoder.c:1004 |
| `N265_RDOQ_UP` | 1 | src/encoder/rdoq.c:288 |
| `N265_VBV_DECAY` | 0.0625 | src/encoder/rc.c:167 |
| `N265_VBV_LOOKAHEAD` | 1 | src/encoder/encoder.c:1212 |
| `N265_VBV_RESERVE` | 0.2 | src/encoder/encoder.c:1219 |
| `N265_VBV_SAFE` | 0.85 | src/encoder/encoder.c:1205 |

## instrument (153)

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
| `N265_API_DEFER` | ? | src/encoder/encoder.c:1485 |
| `N265_AQ_ANCHOR` | ? | src/encoder/encoder.c:3328 |
| `N265_AQ_CLAMP` | ? | src/encoder/encoder.c:3351 |
| `N265_AQ_CLAMP_FLAT` | ? | src/encoder/encoder.c:3381 |
| `N265_AQ_DC` | ? | src/encoder/encoder.c:3328 |
| `N265_AQ_FLAT_SIGN` | 0 | src/encoder/encoder.c:917 |
| `N265_AQ_FLOOR` | 4 | src/encoder/encoder.c:3213 |
| `N265_AQ_SIGNAL` | 1 | src/encoder/encoder.c:916 |
| `N265_AQ_STR` | ? | src/encoder/encoder.c:910 |
| `N265_AQ_TRACE` | ? | src/encoder/encoder.c:3432 |
| `N265_ASM_OFF` | unset | src/common/cpu.c:110 |
| `N265_BADAPT_REFW` | ? | src/encoder/encoder.c:4441 |
| `N265_BADAPT_THR` | ? | src/encoder/encoder.c:4440 |
| `N265_BADAPT_TRACE` | ? | src/encoder/la.c:701 |
| `N265_BI_GATE` | 4 | src/encoder/ctu.c:2712 |
| `N265_BI_REFINE` | 0 | src/encoder/ctu.c:2664 |
| `N265_BRUN_TRACE` | ? | src/encoder/la.c:253 |
| `N265_B_INTRA_MARGIN` | 30 | src/encoder/ctu.c:2646 |
| `N265_CB64_AMVP` | 0 | src/encoder/ctu.c:2631 |
| `N265_CB64_GATE` | 1 | src/encoder/ctu.c:2613 |
| `N265_CB64_TRACE` | ? | src/encoder/ctu.c:191 |
| `N265_CBF_FAST` | 2 | src/encoder/ctu.c:1546 |
| `N265_CBIT_TRACE` | ? | src/encoder/encoder.c:980 |
| `N265_CHROMA_PRED` | 1 | src/encoder/ctu.c:126 |
| `N265_CHROMA_W` | 0 | src/encoder/ctu.c:66 |
| `N265_CPRED_GATE` | 2 | src/encoder/ctu.c:2433 |
| `N265_CPRED_TRACE` | ? | src/encoder/ctu.c:4129 |
| `N265_CRF_SHIFT` | 5.4 | src/encoder/encoder.c:2709 |
| `N265_CT_CENTRE` | 0 | src/encoder/la.c:1081 |
| `N265_CT_FRAMEW` | 1.0 | src/encoder/encoder.c:849 |
| `N265_CT_FRAMEW_FLAT` | = N265_CT_QPW | src/encoder/encoder.c:853 |
| `N265_CT_FRAME_CLAMP` | 0 | src/encoder/encoder.c:857 |
| `N265_CT_FRAME_LEVEL` | 1 | src/encoder/la.c:1130 |
| `N265_CT_FRAME_SHOT` | 0 | src/encoder/encoder.c:859 |
| `N265_CT_FRAME_UP` | 0 | src/encoder/encoder.c:867 |
| `N265_CT_QPW` | ? | src/encoder/encoder.c:841 |
| `N265_CT_TRACE` | ? | src/encoder/la.c:1228 |
| `N265_CT_UP_GATE` | 0.24 | src/encoder/encoder.c:885 |
| `N265_CUTREE_STR` | ? | src/encoder/encoder.c:840 |
| `N265_CU_STATS` | ? | src/encoder/encoder.c:979 |
| `N265_CU_TRACE` | ? | src/encoder/encoder.c:663 |
| `N265_DBF_BETA` | 0 | src/encoder/encoder.c:1128 |
| `N265_DBF_TC` | 0 | src/encoder/encoder.c:1129 |
| `N265_DECGRAD_TRACE` | ? | src/encoder/rdoq.c:353 |
| `N265_DECIMATE` | 15 | src/encoder/rdoq.c:306 |
| `N265_DECIMATE_GRAD` | = N265_DECIMATE | src/encoder/rdoq.c:320 |
| `N265_DEPTH_BOUND` | 0 | src/encoder/ctu.c:1907 |
| `N265_DEPTH_DUMP` | ? | src/encoder/encoder.c:650 |
| `N265_DPB_HOLD` | 0 | src/encoder/encoder.c:230 |
| `N265_DPB_TRACE` | ? | src/encoder/encoder.c:957 |
| `N265_DUP_FAST` | 0 | src/encoder/la.c:263 |
| `N265_DUP_THR` | 0.0 | src/encoder/la.c:282 |
| `N265_DUP_TRACE` | ? | src/encoder/la.c:273 |
| `N265_ES_THR` | 100 | src/encoder/la.c:824 |
| `N265_FLAT_GATE` | 10 | src/encoder/la.c:102 |
| `N265_FLAT_TRACE` | ? | src/encoder/encoder.c:3145 |
| `N265_FORCE_NONIDR` | -1 | src/encoder/encoder.c:4140 |
| `N265_FULLPEL` | ? | src/encoder/mv.c:106 |
| `N265_HF2` | 3 | src/encoder/encoder.c:1928 |
| `N265_HF2_TPP` | 150 | src/encoder/encoder.c:1956 |
| `N265_HF2_TPP14` | 190 | src/encoder/encoder.c:1967 |
| `N265_HF2_TRACE` | ? | src/encoder/encoder.c:1979 |
| `N265_HFCACHE` | 1 | src/encoder/encoder.c:1864 |
| `N265_INTRA_ADAPT_K` | 32 | src/encoder/encoder.c:988 |
| `N265_INTRA_ADAPT_THR` | 100 | src/encoder/encoder.c:991 |
| `N265_INTRA_GATE` | 7 | src/encoder/ctu.c:1699 |
| `N265_INTRA_MIN` | 4 | src/encoder/encoder.c:4129 |
| `N265_INTRA_MPM` | 0 | src/encoder/ctu.c:3498 |
| `N265_INTRA_ORACLE` | 0 | src/encoder/ctu.c:1728 |
| `N265_INTRA_RD_N` | 3 | src/encoder/ctu.c:3487 |
| `N265_INTRA_WIN_TRACE` | ? | src/encoder/encoder.c:981 |
| `N265_I_INTRA` | 0 | src/encoder/ctu.c:3514 |
| `N265_LA_PROF` | 0.0 | src/encoder/encoder.c:1490 |
| `N265_LEVEL_TRACE` | ? | src/encoder/encoder.c:1346 |
| `N265_MAX_FRAME_WORK` | ? | src/encoder/encoder.c:1368 |
| `N265_MERGE_RD_N` | ? | src/encoder/ctu.c:2683 |
| `N265_MERGE_TRACE` | -1 | src/encoder/mv.c:100 |
| `N265_ME_PATTERN` | 0 | src/encoder/mv.c:750 |
| `N265_ME_SATD` | 0 | src/encoder/ctu.c:2564 |
| `N265_MIN_KEYINT` | 4 | src/encoder/encoder.c:4111 |
| `N265_MODE_HIST` | ? | src/encoder/sao.c:252 |
| `N265_MVD_BITS` | 0 | src/encoder/ctu.c:2552 |
| `N265_NO_AMP` | 1 | src/encoder/encoder.c:945 |
| `N265_NO_RECT` | 1 | src/encoder/encoder.c:943 |
| `N265_NO_SPLIT` | ? | src/encoder/ctu.c:179 |
| `N265_PART_GATE` | 2 | src/encoder/ctu.c:1781 |
| `N265_PB_CASCADE` | auto | src/encoder/encoder.c:891 |
| `N265_PB_CASCADE_FLAT` | auto | src/encoder/encoder.c:900 |
| `N265_PSY` | 0.0 | src/encoder/encoder.c:1005 |
| `N265_QP_FRAC` | = --bitrate or CRF+frame term | src/encoder/encoder.c:1108 |
| `N265_QP_FRAC_FLAT` | 0 | src/encoder/encoder.c:1121 |
| `N265_QP_MAP_TRACE` | ? | src/encoder/encoder.c:3501 |
| `N265_QP_ROUND1` | 1 | src/encoder/encoder.c:918 |
| `N265_QP_TRACE` | ? | src/encoder/encoder.c:3517 |
| `N265_RC_LAG` | 0 | src/encoder/encoder.c:1421 |
| `N265_RC_TRACE` | ? | src/encoder/rc.c:170 |
| `N265_RDOQ_ADAPT` | 0 | src/encoder/rdoq.c:294 |
| `N265_RDOQ_LAM` | 100 | src/encoder/rdoq.c:300 |
| `N265_RDOQ_LAT_SZ` | 0 | src/encoder/ctu.c:832 |
| `N265_RDOQ_TRACE` | ? | src/encoder/ctu.c:700 |
| `N265_RECT_FLAT` | 1 | src/encoder/ctu.c:1859 |
| `N265_RECT_RANK` | 50 | src/encoder/ctu.c:1824 |
| `N265_REF_LAG_HINT` | -1 | src/encoder/encoder.c:3547 |
| `N265_RSKIP` | 2 | src/encoder/ctu.c:1526 |
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
| `N265_SC_TRACE` | ? | src/encoder/la.c:547 |
| `N265_SERIAL_PROF` | 0 | src/encoder/encoder.c:1487 |
| `N265_SHOT_STATS` | ? | src/encoder/encoder.c:1000 |
| `N265_SHOT_TRACE` | ? | src/encoder/encoder.c:4336 |
| `N265_SHOT_WINDOW` | 0 | src/encoder/encoder.c:1058 |
| `N265_SKIP_EXIT` | 0 | src/encoder/ctu.c:1597 |
| `N265_SKIP_THR` | 2 | src/encoder/ctu.c:1425 |
| `N265_STALL_TRACE` | ? | src/encoder/encoder.c:1671 |
| `N265_SUBME` | 3 | src/encoder/mv.c:542 |
| `N265_SUBPEL_DEDUP` | 1 | src/encoder/mv.c:112 |
| `N265_SUBPEL_GATE` | 0 | src/encoder/mv.c:614 |
| `N265_SUBPEL_TRACE` | ? | src/encoder/ctu.c:2852 |
| `N265_VBV_HRD` | 1 | src/encoder/encoder.c:1223 |
| `N265_VBV_TRACE` | ? | src/encoder/encoder.c:1222 |

## arm (25)

| knob | default | reader |
|---|---|---|
| `N265_ABR_IOFF_MAX` | 6.0 | src/encoder/rc.c:145 |
| `N265_AQ` | ? | src/encoder/encoder.c:1006 |
| `N265_AQ_FLAT` | ? | src/encoder/encoder.c:3162 |
| `N265_AQ_FLAT_THR` | = ungated | src/encoder/encoder.c:3420 |
| `N265_BADAPT_DEG` | ? | src/encoder/encoder.c:4466 |
| `N265_CHROMA_W_PCT` | 100 | src/encoder/ctu.c:72 |
| `N265_CPRED_W` | 100 | src/encoder/ctu.c:2406 |
| `N265_CT_LAMBDA` | ? | src/encoder/encoder.c:842 |
| `N265_LAMBDA` | ? | src/encoder/ctu.c:1382 |
| `N265_NO_AMVP` | ? | src/encoder/ctu.c:173 |
| `N265_NO_EARLYSKIP` | ? | src/encoder/ctu.c:185 |
| `N265_NO_GATES` | ? | src/encoder/ctu.c:167 |
| `N265_NO_SAO` | ? | src/encoder/encoder.c:978 |
| `N265_NO_TMVP` | ? | src/encoder/encoder.c:1816 |
| `N265_OLD_BS` | ? | src/encoder/deblock.c:154 |
| `N265_PB_STEP` | 0 | src/encoder/encoder.c:2712 |
| `N265_RUNG_GATE` | -2 | src/encoder/mv.c:671 |
| `N265_RUNG_RERANK` | 0 | src/encoder/mv.c:687 |
| `N265_SC_CONJ` | 3 | src/encoder/ctu.c:1485 |
| `N265_SHOT_CLAMP` | 2.0 | src/encoder/encoder.c:1077 |
| `N265_SHOT_EMA` | 0.5 | src/encoder/encoder.c:1082 |
| `N265_SHOT_K` | 1.0 | src/encoder/encoder.c:1076 |
| `N265_STATIC_COMMIT` | 0 | src/encoder/ctu.c:1461 |
| `N265_SUBME_RUNG` | 0 | src/encoder/mv.c:657 |
| `N265_SUBPEL_RATE` | 0 | src/encoder/mv.c:570 |
