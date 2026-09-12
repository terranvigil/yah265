# The N265_* knob census (generated -- edit scripts/knob_census.py OVERRIDES, not this file)

Regenerate: `python3 scripts/knob_census.py`. CI check: `--check`.

Tier meanings: **default** = shipped behaviour, the env is its escape; **instrument** = measurement machinery (catalogued in docs/instruments.md when it has produced a result); **arm** = default-off gate or kept refuted arm -- kept deliberately because a refuted arm retains its re-pricing value.

## default (7)

| knob | default | reader |
|---|---|---|
| `N265_ABR_SB` | 1.7 | src/encoder/rc.c:145 |
| `N265_ABR_SBR` | 4.0 | src/encoder/rc.c:144 |
| `N265_ABR_SP` | 13.3 | src/encoder/rc.c:143 |
| `N265_GATE_SCALE` | 100 | src/encoder/ctu.c:733 |
| `N265_RDOQ` | 1 | src/encoder/encoder.c:759 |
| `N265_RDOQ_UP` | 1 | src/encoder/rdoq.c:278 |
| `N265_SKIP_THR` | 2 | src/encoder/ctu.c:655 |

## instrument (124)

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
| `N265_API_DEFER` | ? | src/encoder/encoder.c:898 |
| `N265_AQ_ANCHOR` | ? | src/encoder/encoder.c:2231 |
| `N265_AQ_CLAMP` | ? | src/encoder/encoder.c:2253 |
| `N265_AQ_CLAMP_FLAT` | ? | src/encoder/encoder.c:2277 |
| `N265_AQ_DC` | ? | src/encoder/encoder.c:2231 |
| `N265_AQ_FLAT_SIGN` | 0 | src/encoder/encoder.c:709 |
| `N265_AQ_FLOOR` | 4 | src/encoder/encoder.c:2154 |
| `N265_AQ_SIGNAL` | 1 | src/encoder/encoder.c:708 |
| `N265_AQ_STR` | ? | src/encoder/encoder.c:702 |
| `N265_AQ_TRACE` | ? | src/encoder/encoder.c:2298 |
| `N265_ASM_OFF` | unset | src/common/cpu.c:101 |
| `N265_BADAPT_REFW` | ? | src/encoder/encoder.c:2749 |
| `N265_BADAPT_THR` | ? | src/encoder/encoder.c:2748 |
| `N265_BADAPT_TRACE` | ? | src/encoder/la.c:562 |
| `N265_BI_GATE` | 4 | src/encoder/ctu.c:1707 |
| `N265_BI_REFINE` | 0 | src/encoder/ctu.c:1659 |
| `N265_BRUN_TRACE` | ? | src/encoder/la.c:218 |
| `N265_B_INTRA_MARGIN` | 30 | src/encoder/ctu.c:1641 |
| `N265_CB64_AMVP` | 0 | src/encoder/ctu.c:1626 |
| `N265_CB64_GATE` | 1 | src/encoder/ctu.c:1608 |
| `N265_CB64_TRACE` | ? | src/encoder/ctu.c:122 |
| `N265_CBF_FAST` | 2 | src/encoder/ctu.c:717 |
| `N265_CBIT_TRACE` | ? | src/encoder/encoder.c:756 |
| `N265_CHROMA_W` | 0 | src/encoder/ctu.c:64 |
| `N265_CRF_SHIFT` | 5.4 | src/encoder/encoder.c:1943 |
| `N265_CT_CENTRE` | 0 | src/encoder/la.c:932 |
| `N265_CT_FRAMEW` | 1.0 | src/encoder/encoder.c:641 |
| `N265_CT_FRAMEW_FLAT` | = N265_CT_QPW | src/encoder/encoder.c:645 |
| `N265_CT_FRAME_CLAMP` | 0 | src/encoder/encoder.c:649 |
| `N265_CT_FRAME_LEVEL` | 1 | src/encoder/la.c:981 |
| `N265_CT_FRAME_SHOT` | 0 | src/encoder/encoder.c:651 |
| `N265_CT_FRAME_UP` | 0 | src/encoder/encoder.c:659 |
| `N265_CT_QPW` | ? | src/encoder/encoder.c:633 |
| `N265_CT_TRACE` | ? | src/encoder/la.c:1079 |
| `N265_CT_UP_GATE` | 0.24 | src/encoder/encoder.c:677 |
| `N265_CUTREE_STR` | ? | src/encoder/encoder.c:632 |
| `N265_CU_STATS` | ? | src/encoder/encoder.c:755 |
| `N265_DBF_BETA` | 0 | src/encoder/encoder.c:808 |
| `N265_DBF_TC` | 0 | src/encoder/encoder.c:809 |
| `N265_DECGRAD_TRACE` | ? | src/encoder/rdoq.c:343 |
| `N265_DECIMATE` | 15 | src/encoder/rdoq.c:296 |
| `N265_DECIMATE_GRAD` | = N265_DECIMATE | src/encoder/rdoq.c:310 |
| `N265_DEPTH_BOUND` | 0 | src/encoder/ctu.c:1078 |
| `N265_DPB_HOLD` | 0 | src/encoder/encoder.c:151 |
| `N265_DPB_TRACE` | ? | src/encoder/encoder.c:748 |
| `N265_ES_THR` | 100 | src/encoder/la.c:679 |
| `N265_FLAT_GATE` | 10 | src/encoder/la.c:85 |
| `N265_FLAT_TRACE` | ? | src/encoder/encoder.c:2104 |
| `N265_HF2` | 3 | src/encoder/encoder.c:1230 |
| `N265_HF2_TPP` | 150 | src/encoder/encoder.c:1258 |
| `N265_HF2_TPP14` | 190 | src/encoder/encoder.c:1269 |
| `N265_HF2_TRACE` | ? | src/encoder/encoder.c:1281 |
| `N265_HFCACHE` | 1 | src/encoder/encoder.c:1168 |
| `N265_INTRA_GATE` | 7 | src/encoder/ctu.c:870 |
| `N265_INTRA_MPM` | 0 | src/encoder/ctu.c:2118 |
| `N265_INTRA_ORACLE` | 0 | src/encoder/ctu.c:899 |
| `N265_INTRA_RD_N` | 3 | src/encoder/ctu.c:2107 |
| `N265_I_INTRA` | 0 | src/encoder/ctu.c:2134 |
| `N265_LA_PROF` | 0.0 | src/encoder/encoder.c:899 |
| `N265_MAX_FRAME_WORK` | ? | src/encoder/encoder.c:825 |
| `N265_MERGE_RD_N` | ? | src/encoder/ctu.c:1678 |
| `N265_MERGE_TRACE` | -1 | src/encoder/mv.c:98 |
| `N265_ME_PATTERN` | 0 | src/encoder/mv.c:550 |
| `N265_ME_SATD` | 0 | src/encoder/ctu.c:1559 |
| `N265_MIN_KEYINT` | 4 | src/encoder/encoder.c:2644 |
| `N265_MODE_HIST` | ? | src/encoder/sao.c:221 |
| `N265_MVD_BITS` | 0 | src/encoder/ctu.c:1547 |
| `N265_NO_AMP` | 1 | src/encoder/encoder.c:737 |
| `N265_NO_RECT` | 1 | src/encoder/encoder.c:735 |
| `N265_NO_SPLIT` | ? | src/encoder/ctu.c:110 |
| `N265_PART_GATE` | 2 | src/encoder/ctu.c:952 |
| `N265_PB_CASCADE` | auto | src/encoder/encoder.c:683 |
| `N265_PB_CASCADE_FLAT` | auto | src/encoder/encoder.c:692 |
| `N265_PSY` | 0.0 | src/encoder/encoder.c:760 |
| `N265_QP_FRAC` | = --bitrate or CRF+frame term | src/encoder/encoder.c:789 |
| `N265_QP_FRAC_FLAT` | 0 | src/encoder/encoder.c:801 |
| `N265_QP_MAP_TRACE` | ? | src/encoder/encoder.c:2361 |
| `N265_QP_ROUND1` | 1 | src/encoder/encoder.c:710 |
| `N265_QP_TRACE` | ? | src/encoder/encoder.c:2377 |
| `N265_RC_LAG` | 0 | src/encoder/encoder.c:840 |
| `N265_RC_TRACE` | ? | src/encoder/rc.c:152 |
| `N265_RDOQ_ADAPT` | 0 | src/encoder/rdoq.c:284 |
| `N265_RDOQ_LAM` | 100 | src/encoder/rdoq.c:290 |
| `N265_RECT_FLAT` | 1 | src/encoder/ctu.c:1030 |
| `N265_RECT_RANK` | 50 | src/encoder/ctu.c:995 |
| `N265_REF_LAG_HINT` | -1 | src/encoder/encoder.c:2407 |
| `N265_RSKIP` | 2 | src/encoder/ctu.c:697 |
| `N265_SAO_ANIM` | 1 | src/encoder/sao.c:277 |
| `N265_SAO_BIAS` | 100 | src/encoder/sao.c:313 |
| `N265_SAO_CHROMA` | 1 | src/encoder/sao.c:468 |
| `N265_SAO_GATE` | 0 | src/encoder/sao.c:517 |
| `N265_SAO_QPLAM` | 0 | src/encoder/sao.c:303 |
| `N265_SAO_TRACE` | ? | src/encoder/sao.c:610 |
| `N265_SAO_TYPES` | 0 | src/encoder/sao.c:240 |
| `N265_SATD8` | 0 | src/encoder/satd.c:134 |
| `N265_SBH_RD` | 2 | src/encoder/rdoq.c:269 |
| `N265_SC_HOLD` | 1 | src/encoder/la.c:203 |
| `N265_SC_TRACE` | ? | src/encoder/la.c:409 |
| `N265_SKIP_EXIT` | 0 | src/encoder/ctu.c:768 |
| `N265_STALL_TRACE` | ? | src/encoder/encoder.c:996 |
| `N265_SUBME` | 3 | src/encoder/mv.c:538 |
| `N265_SUBPEL_DEDUP` | 1 | src/encoder/mv.c:110 |

## arm (14)

| knob | default | reader |
|---|---|---|
| `N265_AQ` | ? | src/encoder/encoder.c:761 |
| `N265_AQ_FLAT` | ? | src/encoder/encoder.c:2121 |
| `N265_BADAPT_DEG` | ? | src/encoder/encoder.c:2766 |
| `N265_CHROMA_W_PCT` | 100 | src/encoder/ctu.c:70 |
| `N265_CT_LAMBDA` | ? | src/encoder/encoder.c:634 |
| `N265_FULLPEL` | ? | src/encoder/mv.c:104 |
| `N265_LAMBDA` | ? | src/encoder/ctu.c:638 |
| `N265_NO_AMVP` | ? | src/encoder/ctu.c:104 |
| `N265_NO_EARLYSKIP` | ? | src/encoder/ctu.c:116 |
| `N265_NO_GATES` | ? | src/encoder/ctu.c:98 |
| `N265_NO_SAO` | ? | src/encoder/encoder.c:754 |
| `N265_NO_TMVP` | ? | src/encoder/encoder.c:1124 |
| `N265_OLD_BS` | ? | src/encoder/deblock.c:146 |
| `N265_PB_STEP` | 0 | src/encoder/encoder.c:1946 |
