# The N265_* knob census (generated -- edit scripts/knob_census.py OVERRIDES, not this file)

Regenerate: `python3 scripts/knob_census.py`. CI check: `--check`.

Tier meanings: **default** = shipped behaviour, the env is its escape; **instrument** = measurement machinery (catalogued in docs/instruments.md when it has produced a result); **arm** = default-off gate or kept refuted arm -- kept deliberately because a refuted arm retains its re-pricing value.

In the default column, **presence** means the reader is a boolean: unset is off and any value arms it. **unset** means the value names a file or a string and unset is off. `= N265_X` means the default is another knob's resolved value.

## default (21)

| knob | default | reader |
|---|---|---|
| `N265_ABR_SB` | 1.7 | src/encoder/rc.c:166 |
| `N265_ABR_SBR` | 4.0 | src/encoder/rc.c:165 |
| `N265_ABR_SP` | 13.3 | src/encoder/rc.c:164 |
| `N265_FGC_CHROMA` | 1 | src/encoder/grain.c:133 |
| `N265_FGC_CORE` | 1.0 | src/encoder/grain.c:143 |
| `N265_FGC_FRAMES` | 1 | src/encoder/grain.c:121 |
| `N265_FGC_MIN` | 1.0 | src/encoder/grain.c:181 |
| `N265_FGC_PCT` | 25 | src/encoder/grain.c:109 |
| `N265_FGC_RATIO` | 2.0 | src/encoder/grain.c:167 |
| `N265_GATE_SCALE` | 100 | src/encoder/ctu.c:1677 |
| `N265_LAMBDA` | 0.68 | src/encoder/ctu.c:1497 |
| `N265_MCSTF_CLEAN` | 1.5 | src/encoder/mcstf.c:97 |
| `N265_MCSTF_PERIOD` | 8 | src/encoder/mcstf.c:114 |
| `N265_MCSTF_SIGMA` | 6.0 | src/encoder/mcstf.c:38 |
| `N265_PB_STEP` | 1.0 | src/encoder/encoder.c:3787 |
| `N265_RDOQ` | 1 | src/encoder/encoder.c:1591 |
| `N265_RDOQ_UP` | 1 | src/encoder/rdoq.c:288 |
| `N265_VBV_DECAY` | 0.0625 | src/encoder/rc.c:176 |
| `N265_VBV_LOOKAHEAD` | 1 | src/encoder/encoder.c:1908 |
| `N265_VBV_RESERVE` | 0.2 | src/encoder/encoder.c:1915 |
| `N265_VBV_SAFE` | 0.85 | src/encoder/encoder.c:1901 |

## instrument (168)

| knob | default | reader |
|---|---|---|
| `N265_2P_CLAMP` | 6.0 | src/encoder/encoder.c:571 |
| `N265_2P_GAIN` | 0.25 | src/encoder/rc.c:154 |
| `N265_2P_QCOMP` | = N265_ABR_QCOMP | src/encoder/encoder.c:569 |
| `N265_2P_TRACE` | presence | src/encoder/encoder.c:586 |
| `N265_2P_TRUST` | 2.0 | src/encoder/encoder.c:578 |
| `N265_ABR_CMEM` | 1000.0 | src/encoder/rc.c:135 |
| `N265_ABR_COPEN` | 1 | src/encoder/rc.c:138 |
| `N265_ABR_CSEED` | 0.0 | src/encoder/rc.c:137 |
| `N265_ABR_CTF` | 1 | src/encoder/rc.c:142 |
| `N265_ABR_FBMAX` | 6.0 | src/encoder/rc.c:129 |
| `N265_ABR_FBSTEP` | 1 | src/encoder/rc.c:140 |
| `N265_ABR_FBUP` | = N265_ABR_FBMAX | src/encoder/rc.c:130 |
| `N265_ABR_HORIZON` | 6.0 | src/encoder/rc.c:121 |
| `N265_ABR_IBRAKE` | 1 | src/encoder/rc.c:139 |
| `N265_ABR_IGAIN` | 0.085 | src/encoder/rc.c:127 |
| `N265_ABR_IMAX` | 8.0 | src/encoder/rc.c:128 |
| `N265_ABR_IOFF` | 0.0 | src/encoder/rc.c:143 |
| `N265_ABR_IOFF_CT` | 0.0 | src/encoder/rc.c:144 |
| `N265_ABR_LVL` | 2.0 | src/encoder/rc.c:167 |
| `N265_ABR_MEAS` | 1 | src/encoder/rc.c:133 |
| `N265_ABR_MGUARD` | 0.0 | src/encoder/rc.c:134 |
| `N265_ABR_OPENI` | 0 | src/encoder/rc.c:141 |
| `N265_ABR_OPENSTEP` | 6.0 | src/encoder/rc.c:126 |
| `N265_ABR_PLANERR` | 0 | src/encoder/rc.c:131 |
| `N265_ABR_QCOMP` | 0.6 | src/encoder/rc.c:120 |
| `N265_ABR_SI` | 25.4 | src/encoder/rc.c:163 |
| `N265_ABR_SMEM` | 16.0 | src/encoder/rc.c:125 |
| `N265_ABR_STEP` | 1.0 | src/encoder/rc.c:124 |
| `N265_ABR_TCAP` | 1 | src/encoder/rc.c:136 |
| `N265_ABR_WMEM` | 1000.0 | src/encoder/rc.c:132 |
| `N265_API_DEFER` | presence | src/encoder/encoder.c:2314 |
| `N265_AQ_ANCHOR` | 6.4 | src/encoder/encoder.c:4556 |
| `N265_AQ_CLAMP` | 3.0 | src/encoder/encoder.c:4579 |
| `N265_AQ_CLAMP_FLAT` | 0.0 | src/encoder/encoder.c:4609 |
| `N265_AQ_DC` | 1.0 | src/encoder/encoder.c:4556 |
| `N265_AQ_FLAT_SIGN` | 0 | src/encoder/encoder.c:1492 |
| `N265_AQ_FLOOR` | 4 | src/encoder/encoder.c:4441 |
| `N265_AQ_SIGNAL` | 1 | src/encoder/encoder.c:1491 |
| `N265_AQ_STR` | 0.7 | src/encoder/encoder.c:1485 |
| `N265_AQ_TRACE` | presence | src/encoder/encoder.c:4660 |
| `N265_ASM_OFF` | unset | src/common/cpu.c:110 |
| `N265_BADAPT_REFW` | 128.0 | src/encoder/encoder.c:5996 |
| `N265_BADAPT_THR` | 0.35 | src/encoder/encoder.c:5995 |
| `N265_BADAPT_TRACE` | presence | src/encoder/la.c:754 |
| `N265_BI_GATE` | 4 | src/encoder/ctu.c:2828 |
| `N265_BI_REFINE` | 0 | src/encoder/ctu.c:2780 |
| `N265_BRUN_TRACE` | presence | src/encoder/la.c:253 |
| `N265_B_INTRA_MARGIN` | 30 | src/encoder/ctu.c:2762 |
| `N265_CB64_AMVP` | 0 | src/encoder/ctu.c:2747 |
| `N265_CB64_GATE` | 1 | src/encoder/ctu.c:2729 |
| `N265_CB64_TRACE` | presence | src/encoder/ctu.c:196 |
| `N265_CBF_FAST` | 2 | src/encoder/ctu.c:1661 |
| `N265_CBIT_TRACE` | presence | src/encoder/encoder.c:1561 |
| `N265_CHROMA_PRED` | 1 | src/encoder/ctu.c:131 |
| `N265_CHROMA_W` | 0 | src/encoder/ctu.c:66 |
| `N265_CPRED_GATE` | 2 | src/encoder/ctu.c:2549 |
| `N265_CPRED_TRACE` | presence | src/encoder/ctu.c:4266 |
| `N265_CRF_SHIFT` | 5.4 | src/encoder/encoder.c:3784 |
| `N265_CT_CENTRE` | 0 | src/encoder/la.c:1134 |
| `N265_CT_FRAMEW` | 1.0 | src/encoder/encoder.c:1424 |
| `N265_CT_FRAMEW_FLAT` | = N265_CT_QPW | src/encoder/encoder.c:1428 |
| `N265_CT_FRAME_CLAMP` | 0 | src/encoder/encoder.c:1432 |
| `N265_CT_FRAME_LEVEL` | 1 | src/encoder/la.c:1183 |
| `N265_CT_FRAME_SHOT` | 0 | src/encoder/encoder.c:1434 |
| `N265_CT_FRAME_UP` | 0 | src/encoder/encoder.c:1442 |
| `N265_CT_QPW` | 2.0 | src/encoder/encoder.c:1416 |
| `N265_CT_TRACE` | presence | src/encoder/la.c:1281 |
| `N265_CT_UP_GATE` | 0.24 | src/encoder/encoder.c:1460 |
| `N265_CUTREE_STR` | 2.0 | src/encoder/encoder.c:1415 |
| `N265_CU_STATS` | presence | src/encoder/encoder.c:1560 |
| `N265_CU_TRACE` | unset | src/encoder/encoder.c:1206 |
| `N265_DBF_BETA` | 0 | src/encoder/encoder.c:1798 |
| `N265_DBF_TC` | 0 | src/encoder/encoder.c:1799 |
| `N265_DECGRAD_TRACE` | presence | src/encoder/rdoq.c:353 |
| `N265_DECIMATE` | 15 | src/encoder/rdoq.c:306 |
| `N265_DECIMATE_GRAD` | = N265_DECIMATE | src/encoder/rdoq.c:320 |
| `N265_DEPTH_BOUND` | 0 | src/encoder/ctu.c:2022 |
| `N265_DEPTH_DUMP` | unset | src/encoder/encoder.c:1193 |
| `N265_DPB_HOLD` | 0 | src/encoder/encoder.c:671 |
| `N265_DPB_TRACE` | presence | src/encoder/encoder.c:1538 |
| `N265_DUP_FAST` | 0 | src/encoder/la.c:263 |
| `N265_DUP_THR` | 0.0 | src/encoder/la.c:282 |
| `N265_DUP_TRACE` | presence | src/encoder/la.c:273 |
| `N265_ES_THR` | 100 | src/encoder/la.c:877 |
| `N265_FGC_TRACE` | presence | src/encoder/grain.c:192 |
| `N265_FLAT_GATE` | 10 | src/encoder/la.c:102 |
| `N265_FLAT_TRACE` | presence | src/encoder/encoder.c:4373 |
| `N265_FORCE_NONIDR` | -1 | src/encoder/encoder.c:5617 |
| `N265_FULLPEL` | presence | src/encoder/mv.c:106 |
| `N265_HF2` | 3 | src/encoder/encoder.c:2912 |
| `N265_HF2_TPP` | 150 | src/encoder/encoder.c:2940 |
| `N265_HF2_TPP14` | 190 | src/encoder/encoder.c:2951 |
| `N265_HF2_TRACE` | presence | src/encoder/encoder.c:2963 |
| `N265_HFCACHE` | 1 | src/encoder/encoder.c:2848 |
| `N265_INTRA_ADAPT_K` | 32 | src/encoder/encoder.c:1569 |
| `N265_INTRA_ADAPT_THR` | 100 | src/encoder/encoder.c:1572 |
| `N265_INTRA_GATE` | 7 | src/encoder/ctu.c:1814 |
| `N265_INTRA_MIN` | 4 | src/encoder/encoder.c:5606 |
| `N265_INTRA_MPM` | 0 | src/encoder/ctu.c:3633 |
| `N265_INTRA_ORACLE` | 0 | src/encoder/ctu.c:1843 |
| `N265_INTRA_RD_N` | 3 | src/encoder/ctu.c:3622 |
| `N265_INTRA_WIN_TRACE` | presence | src/encoder/encoder.c:1562 |
| `N265_I_INTRA` | 0 | src/encoder/ctu.c:3649 |
| `N265_LA_PROF` | presence | src/encoder/encoder.c:2319 |
| `N265_LEVEL_TRACE` | presence | src/encoder/encoder.c:2175 |
| `N265_MAX_FRAME_WORK` | 6 with --bframes >= 2, else 4 | src/encoder/encoder.c:2197 |
| `N265_MCSTF_TRACE` | presence | src/encoder/mcstf.c:125 |
| `N265_MERGE_RD_N` | 5 | src/encoder/ctu.c:2799 |
| `N265_MERGE_TRACE` | -1 | src/encoder/mv.c:100 |
| `N265_ME_PATTERN` | 0 | src/encoder/mv.c:788 |
| `N265_ME_SATD` | 0 | src/encoder/ctu.c:2680 |
| `N265_MIN_KEYINT` | 4 | src/encoder/encoder.c:5588 |
| `N265_MODE_HIST` | presence | src/encoder/sao.c:252 |
| `N265_MVD_BITS` | 0 | src/encoder/ctu.c:2668 |
| `N265_NO_AMP` | 1 | src/encoder/encoder.c:1520 |
| `N265_NO_RECT` | 1 | src/encoder/encoder.c:1518 |
| `N265_NO_SPLIT` | presence | src/encoder/ctu.c:184 |
| `N265_OPENGOP_CUT` | 0 | src/encoder/encoder.c:736 |
| `N265_OPENGOP_NOBARRIER` | 0 | src/encoder/encoder.c:743 |
| `N265_PART_GATE` | 2 | src/encoder/ctu.c:1896 |
| `N265_PB_CASCADE` | auto | src/encoder/encoder.c:1466 |
| `N265_PB_CASCADE_FLAT` | auto | src/encoder/encoder.c:1475 |
| `N265_PSY` | 0.0 | src/encoder/encoder.c:1593 |
| `N265_QP_FRAC` | = --bitrate or CRF+frame term | src/encoder/encoder.c:1778 |
| `N265_QP_FRAC_FLAT` | 0 | src/encoder/encoder.c:1791 |
| `N265_QP_MAP_TRACE` | unset | src/encoder/encoder.c:4729 |
| `N265_QP_ROUND1` | 1 | src/encoder/encoder.c:1493 |
| `N265_QP_TRACE` | presence | src/encoder/encoder.c:4745 |
| `N265_RC_LAG` | 0 | src/encoder/encoder.c:2250 |
| `N265_RC_TRACE` | presence | src/encoder/encoder.c:586 |
| `N265_RDOQ_ADAPT` | 0 | src/encoder/rdoq.c:294 |
| `N265_RDOQ_LAM` | 100 | src/encoder/rdoq.c:300 |
| `N265_RDOQ_LAT_SZ` | 0 | src/encoder/ctu.c:886 |
| `N265_RDOQ_TRACE` | presence | src/encoder/ctu.c:748 |
| `N265_RECT_FLAT` | 1 | src/encoder/ctu.c:1974 |
| `N265_RECT_RANK` | 50 | src/encoder/ctu.c:1939 |
| `N265_REF_LAG_HINT` | -1 | src/encoder/encoder.c:4775 |
| `N265_RSKIP` | 2 | src/encoder/ctu.c:1641 |
| `N265_SAO_ANIM` | 1 | src/encoder/sao.c:308 |
| `N265_SAO_BIAS` | 100 | src/encoder/sao.c:344 |
| `N265_SAO_CHROMA` | 1 | src/encoder/sao.c:500 |
| `N265_SAO_GATE` | 0 | src/encoder/sao.c:549 |
| `N265_SAO_QPLAM` | 0 | src/encoder/sao.c:334 |
| `N265_SAO_TRACE` | unset | src/encoder/sao.c:642 |
| `N265_SAO_TYPES` | 0 | src/encoder/sao.c:271 |
| `N265_SATD8` | 0 | src/encoder/satd.c:134 |
| `N265_SBH_RD` | 2 | src/encoder/rdoq.c:279 |
| `N265_SC_HOLD` | 1 | src/encoder/la.c:238 |
| `N265_SC_TRACE` | presence | src/encoder/la.c:600 |
| `N265_SERIAL_PROF` | 0 | src/encoder/encoder.c:2316 |
| `N265_SHOT_STATS` | unset | src/encoder/encoder.c:1581 |
| `N265_SHOT_TRACE` | presence | src/encoder/encoder.c:5864 |
| `N265_SHOT_WINDOW` | 0 | src/encoder/encoder.c:1646 |
| `N265_SKIP_EXIT` | 0 | src/encoder/ctu.c:1712 |
| `N265_SKIP_THR` | 2 | src/encoder/ctu.c:1540 |
| `N265_STALL_TRACE` | presence | src/encoder/encoder.c:2567 |
| `N265_SUBME` | 3 | src/encoder/mv.c:580 |
| `N265_SUBPEL_DEDUP` | 1 | src/encoder/mv.c:112 |
| `N265_SUBPEL_GATE` | 0 | src/encoder/mv.c:652 |
| `N265_SUBPEL_TRACE` | presence | src/encoder/ctu.c:2968 |
| `N265_VBV_HRD` | 1 | src/encoder/encoder.c:1997 |
| `N265_VBV_TRACE` | presence | src/encoder/encoder.c:1996 |
| `N265_WEIGHTB` | 0 | src/encoder/encoder.c:1824 |
| `N265_WEIGHTP` | 0 | src/encoder/encoder.c:1822 |
| `N265_WP_GAIN` | 4 | src/encoder/wp.c:78 |
| `N265_WP_REFS` | 3 | src/encoder/wp.c:117 |
| `N265_WP_TRACE` | 0 | src/encoder/wp.c:51 |
| `N265_WP_WIN` | 40 | src/encoder/wp.c:101 |

## arm (28)

| knob | default | reader |
|---|---|---|
| `N265_ABR_IOFF_MAX` | 6.0 | src/encoder/rc.c:145 |
| `N265_AQ` | presence | src/encoder/encoder.c:1594 |
| `N265_AQ_FLAT` | presence | src/encoder/encoder.c:4390 |
| `N265_AQ_FLAT_THR` | = ungated | src/encoder/encoder.c:4648 |
| `N265_BADAPT_DEG` | 1.6 | src/encoder/encoder.c:6027 |
| `N265_CBQPOFFS` | 0 | src/encoder/encoder.c:1811 |
| `N265_CHROMA_W_PCT` | 100 | src/encoder/ctu.c:72 |
| `N265_CPRED_W` | 100 | src/encoder/ctu.c:2522 |
| `N265_CRQPOFFS` | 0 | src/encoder/encoder.c:1813 |
| `N265_CT_LAMBDA` | 0.0 | src/encoder/encoder.c:1417 |
| `N265_EST_MODE` | 0 | src/encoder/est.c:24 |
| `N265_INTRA_B_MODES` | 0 | src/encoder/ctu.c:1312 |
| `N265_NO_AMVP` | presence | src/encoder/ctu.c:178 |
| `N265_NO_EARLYSKIP` | presence | src/encoder/ctu.c:190 |
| `N265_NO_GATES` | presence | src/encoder/ctu.c:172 |
| `N265_NO_SAO` | presence | src/encoder/encoder.c:1559 |
| `N265_NO_TMVP` | presence | src/encoder/encoder.c:2794 |
| `N265_OLD_BS` | presence | src/encoder/deblock.c:158 |
| `N265_RD_PROXY` | 0 | src/encoder/ctu.c:4586 |
| `N265_RUNG_GATE` | -2 | src/encoder/mv.c:709 |
| `N265_RUNG_RERANK` | 0 | src/encoder/mv.c:725 |
| `N265_SC_CONJ` | 3 | src/encoder/ctu.c:1600 |
| `N265_SHOT_CLAMP` | 2.0 | src/encoder/encoder.c:1665 |
| `N265_SHOT_EMA` | 0.5 | src/encoder/encoder.c:1670 |
| `N265_SHOT_K` | 1.0 | src/encoder/encoder.c:1664 |
| `N265_STATIC_COMMIT` | 0 | src/encoder/ctu.c:1576 |
| `N265_SUBME_RUNG` | 0 | src/encoder/mv.c:695 |
| `N265_SUBPEL_RATE` | 0 | src/encoder/mv.c:608 |
