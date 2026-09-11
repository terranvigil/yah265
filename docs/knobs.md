# The N265_* knob census (generated -- edit scripts/knob_census.py OVERRIDES, not this file)

Regenerate: `python3 scripts/knob_census.py`. CI check: `--check`.

Tier meanings: **default** = shipped behaviour, the env is its escape; **instrument** = measurement machinery (catalogued in docs/instruments.md when it has produced a result); **arm** = default-off gate or kept refuted arm -- kept deliberately because a refuted arm retains its re-pricing value.

## default (7)

| knob | default | reader |
|---|---|---|
| `N265_ABR_SB` | 1.7 | src/encoder/rc.c:145 |
| `N265_ABR_SBR` | 4.0 | src/encoder/rc.c:144 |
| `N265_ABR_SP` | 13.3 | src/encoder/rc.c:143 |
| `N265_GATE_SCALE` | 100 | src/encoder/ctu.c:643 |
| `N265_RDOQ` | 1 | src/encoder/encoder.c:668 |
| `N265_RDOQ_UP` | 1 | src/encoder/rdoq.c:223 |
| `N265_SKIP_THR` | 2 | src/encoder/ctu.c:574 |

## instrument (99)

| knob | default | reader |
|---|---|---|
| `N265_ABR_CMEM` | 1000.0 | src/encoder/rc.c:127 |
| `N265_ABR_COPEN` | 1 | src/encoder/rc.c:130 |
| `N265_ABR_CSEED` | 0.0 | src/encoder/rc.c:129 |
| `N265_ABR_FBMAX` | 6.0 | src/encoder/rc.c:121 |
| `N265_ABR_FBSTEP` | 1 | src/encoder/rc.c:132 |
| `N265_ABR_FBUP` | ? | src/encoder/rc.c:122 |
| `N265_ABR_HORIZON` | 6.0 | src/encoder/rc.c:113 |
| `N265_ABR_IBRAKE` | 1 | src/encoder/rc.c:131 |
| `N265_ABR_IGAIN` | 0.085 | src/encoder/rc.c:119 |
| `N265_ABR_IMAX` | 8.0 | src/encoder/rc.c:120 |
| `N265_ABR_LVL` | 2.0 | src/encoder/rc.c:146 |
| `N265_ABR_MEAS` | 1 | src/encoder/rc.c:125 |
| `N265_ABR_MGUARD` | 0.0 | src/encoder/rc.c:126 |
| `N265_ABR_OPENI` | 0 | src/encoder/rc.c:133 |
| `N265_ABR_OPENSTEP` | 6.0 | src/encoder/rc.c:118 |
| `N265_ABR_PLANERR` | 0 | src/encoder/rc.c:123 |
| `N265_ABR_QCOMP` | 0.6 | src/encoder/rc.c:112 |
| `N265_ABR_SI` | 25.4 | src/encoder/rc.c:142 |
| `N265_ABR_SMEM` | 16.0 | src/encoder/rc.c:117 |
| `N265_ABR_STEP` | 1.0 | src/encoder/rc.c:116 |
| `N265_ABR_TCAP` | 1 | src/encoder/rc.c:128 |
| `N265_ABR_WMEM` | 1000.0 | src/encoder/rc.c:124 |
| `N265_API_DEFER` | ? | src/encoder/encoder.c:769 |
| `N265_AQ_ANCHOR` | ? | src/encoder/encoder.c:1932 |
| `N265_AQ_CLAMP` | ? | src/encoder/encoder.c:1954 |
| `N265_AQ_DC` | ? | src/encoder/encoder.c:1932 |
| `N265_AQ_FLOOR` | 4 | src/encoder/encoder.c:1900 |
| `N265_AQ_STR` | 1 | src/encoder/encoder.c:634 |
| `N265_AQ_TRACE` | ? | src/encoder/encoder.c:1955 |
| `N265_ASM_OFF` | unset | src/common/cpu.c:101 |
| `N265_BADAPT_REFW` | ? | src/encoder/encoder.c:2347 |
| `N265_BADAPT_THR` | ? | src/encoder/encoder.c:2346 |
| `N265_BADAPT_TRACE` | ? | src/encoder/la.c:450 |
| `N265_BI_GATE` | 4 | src/encoder/ctu.c:1448 |
| `N265_BI_REFINE` | 1 | src/encoder/ctu.c:1400 |
| `N265_B_INTRA_MARGIN` | 30 | src/encoder/ctu.c:1384 |
| `N265_CB64_AMVP` | 1 | src/encoder/ctu.c:1369 |
| `N265_CB64_GATE` | 1 | src/encoder/ctu.c:1355 |
| `N265_CB64_TRACE` | ? | src/encoder/ctu.c:66 |
| `N265_CBF_FAST` | 3 | src/encoder/ctu.c:627 |
| `N265_CRF_SHIFT` | 5.4 | src/encoder/encoder.c:1817 |
| `N265_CT_CENTRE` | 0 | src/encoder/la.c:646 |
| `N265_CT_TRACE` | ? | src/encoder/la.c:683 |
| `N265_CUTREE_STR` | ? | src/encoder/encoder.c:629 |
| `N265_CU_STATS` | ? | src/encoder/encoder.c:665 |
| `N265_DBF_BETA` | 0 | src/encoder/encoder.c:679 |
| `N265_DBF_TC` | 0 | src/encoder/encoder.c:680 |
| `N265_DECIMATE` | 15 | src/encoder/rdoq.c:241 |
| `N265_DEPTH_BOUND` | 0 | src/encoder/ctu.c:825 |
| `N265_DPB_HOLD` | 0 | src/encoder/encoder.c:149 |
| `N265_DPB_TRACE` | ? | src/encoder/encoder.c:658 |
| `N265_ES_THR` | 100 | src/encoder/ctu.c:592 |
| `N265_HF2` | 3 | src/encoder/encoder.c:1097 |
| `N265_HF2_TPP` | 150 | src/encoder/encoder.c:1125 |
| `N265_HF2_TPP14` | 190 | src/encoder/encoder.c:1136 |
| `N265_HF2_TRACE` | ? | src/encoder/encoder.c:1148 |
| `N265_HFCACHE` | 1 | src/encoder/encoder.c:1035 |
| `N265_INTRA_GATE` | 5 | src/encoder/ctu.c:683 |
| `N265_INTRA_MPM` | 0 | src/encoder/ctu.c:1859 |
| `N265_INTRA_RD_N` | 3 | src/encoder/ctu.c:1848 |
| `N265_I_INTRA` | 0 | src/encoder/ctu.c:1875 |
| `N265_LA_PROF` | 0.0 | src/encoder/encoder.c:770 |
| `N265_MAX_FRAME_WORK` | ? | src/encoder/encoder.c:696 |
| `N265_MERGE_RD_N` | ? | src/encoder/ctu.c:1419 |
| `N265_MERGE_TRACE` | -1 | src/encoder/mv.c:98 |
| `N265_ME_PATTERN` | 0 | src/encoder/mv.c:533 |
| `N265_ME_SATD` | 0 | src/encoder/ctu.c:1306 |
| `N265_MIN_KEYINT` | 4 | src/encoder/encoder.c:2242 |
| `N265_MODE_HIST` | ? | src/encoder/sao.c:156 |
| `N265_MVD_BITS` | 0 | src/encoder/ctu.c:1294 |
| `N265_NO_AMP` | ? | src/encoder/encoder.c:647 |
| `N265_NO_RECT` | ? | src/encoder/encoder.c:645 |
| `N265_NO_SPLIT` | ? | src/encoder/ctu.c:54 |
| `N265_PART_GATE` | 0 | src/encoder/ctu.c:738 |
| `N265_PSY` | 0.0 | src/encoder/encoder.c:669 |
| `N265_QP_FRAC` | = --bitrate | src/encoder/encoder.c:623 |
| `N265_QP_ROUND1` | 1 | src/encoder/encoder.c:635 |
| `N265_QP_TRACE` | ? | src/encoder/encoder.c:2007 |
| `N265_RC_LAG` | 0 | src/encoder/encoder.c:711 |
| `N265_RC_TRACE` | ? | src/encoder/rc.c:152 |
| `N265_RDOQ_ADAPT` | 0 | src/encoder/rdoq.c:229 |
| `N265_RDOQ_LAM` | 100 | src/encoder/rdoq.c:235 |
| `N265_RECT_RANK` | 0 | src/encoder/ctu.c:768 |
| `N265_REF_LAG_HINT` | -1 | src/encoder/encoder.c:2036 |
| `N265_RSKIP` | 0 | src/encoder/ctu.c:610 |
| `N265_SAO_BIAS` | 100 | src/encoder/sao.c:207 |
| `N265_SAO_CHROMA` | 1 | src/encoder/sao.c:352 |
| `N265_SAO_GATE` | 2 | src/encoder/sao.c:384 |
| `N265_SAO_QPLAM` | 0 | src/encoder/sao.c:197 |
| `N265_SAO_TRACE` | ? | src/encoder/sao.c:471 |
| `N265_SAO_TYPES` | 0 | src/encoder/sao.c:175 |
| `N265_SATD8` | 0 | src/encoder/satd.c:134 |
| `N265_SBH_RD` | 2 | src/encoder/rdoq.c:214 |
| `N265_SC_HOLD` | 1 | src/encoder/la.c:119 |
| `N265_SC_TRACE` | ? | src/encoder/la.c:306 |
| `N265_SKIP_EXIT` | 0 | src/encoder/ctu.c:660 |
| `N265_STALL_TRACE` | ? | src/encoder/encoder.c:867 |
| `N265_SUBME` | 0 | src/encoder/mv.c:521 |
| `N265_SUBPEL_DEDUP` | 1 | src/encoder/mv.c:110 |

## arm (12)

| knob | default | reader |
|---|---|---|
| `N265_AQ` | ? | src/encoder/encoder.c:670 |
| `N265_AQ_FLAT` | ? | src/encoder/encoder.c:1867 |
| `N265_BADAPT_DEG` | ? | src/encoder/encoder.c:2364 |
| `N265_FULLPEL` | ? | src/encoder/mv.c:104 |
| `N265_LAMBDA` | ? | src/encoder/ctu.c:557 |
| `N265_NO_AMVP` | ? | src/encoder/ctu.c:48 |
| `N265_NO_EARLYSKIP` | ? | src/encoder/ctu.c:60 |
| `N265_NO_GATES` | ? | src/encoder/ctu.c:42 |
| `N265_NO_SAO` | ? | src/encoder/encoder.c:664 |
| `N265_NO_TMVP` | ? | src/encoder/encoder.c:995 |
| `N265_OLD_BS` | ? | src/encoder/deblock.c:146 |
| `N265_PB_STEP` | 0 | src/encoder/encoder.c:1824 |
