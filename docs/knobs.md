# The N265_* knob census (generated -- edit scripts/knob_census.py OVERRIDES, not this file)

Regenerate: `python3 scripts/knob_census.py`. CI check: `--check`.

Tier meanings: **default** = shipped behaviour, the env is its escape; **instrument** = measurement machinery (catalogued in docs/instruments.md when it has produced a result); **arm** = default-off gate or kept refuted arm -- kept deliberately because a refuted arm retains its re-pricing value.

## default (7)

| knob | default | reader |
|---|---|---|
| `N265_ABR_SB` | 1.7 | src/encoder/rc.c:145 |
| `N265_ABR_SBR` | 4.0 | src/encoder/rc.c:144 |
| `N265_ABR_SP` | 13.3 | src/encoder/rc.c:143 |
| `N265_GATE_SCALE` | 100 | src/encoder/ctu.c:674 |
| `N265_RDOQ` | 1 | src/encoder/encoder.c:755 |
| `N265_RDOQ_UP` | 1 | src/encoder/rdoq.c:278 |
| `N265_SKIP_THR` | 2 | src/encoder/ctu.c:596 |

## instrument (121)

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
| `N265_API_DEFER` | ? | src/encoder/encoder.c:894 |
| `N265_AQ_ANCHOR` | ? | src/encoder/encoder.c:2193 |
| `N265_AQ_CLAMP` | ? | src/encoder/encoder.c:2215 |
| `N265_AQ_CLAMP_FLAT` | ? | src/encoder/encoder.c:2239 |
| `N265_AQ_DC` | ? | src/encoder/encoder.c:2193 |
| `N265_AQ_FLAT_SIGN` | 0 | src/encoder/encoder.c:706 |
| `N265_AQ_FLOOR` | 4 | src/encoder/encoder.c:2116 |
| `N265_AQ_SIGNAL` | 1 | src/encoder/encoder.c:705 |
| `N265_AQ_STR` | ? | src/encoder/encoder.c:699 |
| `N265_AQ_TRACE` | ? | src/encoder/encoder.c:2260 |
| `N265_ASM_OFF` | unset | src/common/cpu.c:101 |
| `N265_BADAPT_REFW` | ? | src/encoder/encoder.c:2679 |
| `N265_BADAPT_THR` | ? | src/encoder/encoder.c:2678 |
| `N265_BADAPT_TRACE` | ? | src/encoder/la.c:523 |
| `N265_BI_GATE` | 4 | src/encoder/ctu.c:1648 |
| `N265_BI_REFINE` | 0 | src/encoder/ctu.c:1600 |
| `N265_B_INTRA_MARGIN` | 30 | src/encoder/ctu.c:1582 |
| `N265_CB64_AMVP` | 0 | src/encoder/ctu.c:1567 |
| `N265_CB64_GATE` | 1 | src/encoder/ctu.c:1549 |
| `N265_CB64_TRACE` | ? | src/encoder/ctu.c:66 |
| `N265_CBF_FAST` | 2 | src/encoder/ctu.c:658 |
| `N265_CRF_SHIFT` | 5.4 | src/encoder/encoder.c:1968 |
| `N265_CT_CENTRE` | 0 | src/encoder/la.c:720 |
| `N265_CT_FRAMEW` | 1.0 | src/encoder/encoder.c:638 |
| `N265_CT_FRAMEW_FLAT` | = N265_CT_QPW | src/encoder/encoder.c:642 |
| `N265_CT_FRAME_CLAMP` | 0 | src/encoder/encoder.c:646 |
| `N265_CT_FRAME_LEVEL` | 1 | src/encoder/la.c:769 |
| `N265_CT_FRAME_SHOT` | 0 | src/encoder/encoder.c:648 |
| `N265_CT_FRAME_UP` | 0 | src/encoder/encoder.c:656 |
| `N265_CT_QPW` | ? | src/encoder/encoder.c:630 |
| `N265_CT_TRACE` | ? | src/encoder/la.c:867 |
| `N265_CT_UP_GATE` | 0.24 | src/encoder/encoder.c:674 |
| `N265_CUTREE_STR` | ? | src/encoder/encoder.c:629 |
| `N265_CU_STATS` | ? | src/encoder/encoder.c:752 |
| `N265_DBF_BETA` | 0 | src/encoder/encoder.c:804 |
| `N265_DBF_TC` | 0 | src/encoder/encoder.c:805 |
| `N265_DECGRAD_TRACE` | ? | src/encoder/rdoq.c:343 |
| `N265_DECIMATE` | 15 | src/encoder/rdoq.c:296 |
| `N265_DECIMATE_GRAD` | = N265_DECIMATE | src/encoder/rdoq.c:310 |
| `N265_DEPTH_BOUND` | 0 | src/encoder/ctu.c:1019 |
| `N265_DPB_HOLD` | 0 | src/encoder/encoder.c:149 |
| `N265_DPB_TRACE` | ? | src/encoder/encoder.c:745 |
| `N265_ES_THR` | 100 | src/encoder/ctu.c:614 |
| `N265_FLAT_GATE` | 10 | src/encoder/la.c:85 |
| `N265_FLAT_TRACE` | ? | src/encoder/encoder.c:2066 |
| `N265_HF2` | 3 | src/encoder/encoder.c:1226 |
| `N265_HF2_TPP` | 150 | src/encoder/encoder.c:1254 |
| `N265_HF2_TPP14` | 190 | src/encoder/encoder.c:1265 |
| `N265_HF2_TRACE` | ? | src/encoder/encoder.c:1277 |
| `N265_HFCACHE` | 1 | src/encoder/encoder.c:1164 |
| `N265_INTRA_GATE` | 7 | src/encoder/ctu.c:811 |
| `N265_INTRA_MPM` | 0 | src/encoder/ctu.c:2059 |
| `N265_INTRA_ORACLE` | 0 | src/encoder/ctu.c:840 |
| `N265_INTRA_RD_N` | 3 | src/encoder/ctu.c:2048 |
| `N265_I_INTRA` | 0 | src/encoder/ctu.c:2075 |
| `N265_LA_PROF` | 0.0 | src/encoder/encoder.c:895 |
| `N265_MAX_FRAME_WORK` | ? | src/encoder/encoder.c:821 |
| `N265_MERGE_RD_N` | ? | src/encoder/ctu.c:1619 |
| `N265_MERGE_TRACE` | -1 | src/encoder/mv.c:98 |
| `N265_ME_PATTERN` | 0 | src/encoder/mv.c:550 |
| `N265_ME_SATD` | 0 | src/encoder/ctu.c:1500 |
| `N265_MIN_KEYINT` | 4 | src/encoder/encoder.c:2574 |
| `N265_MODE_HIST` | ? | src/encoder/sao.c:221 |
| `N265_MVD_BITS` | 0 | src/encoder/ctu.c:1488 |
| `N265_NO_AMP` | 1 | src/encoder/encoder.c:734 |
| `N265_NO_RECT` | 1 | src/encoder/encoder.c:732 |
| `N265_NO_SPLIT` | ? | src/encoder/ctu.c:54 |
| `N265_PART_GATE` | 2 | src/encoder/ctu.c:893 |
| `N265_PB_CASCADE` | auto | src/encoder/encoder.c:680 |
| `N265_PB_CASCADE_FLAT` | auto | src/encoder/encoder.c:689 |
| `N265_PSY` | 0.0 | src/encoder/encoder.c:756 |
| `N265_QP_FRAC` | = --bitrate or CRF+frame term | src/encoder/encoder.c:785 |
| `N265_QP_FRAC_FLAT` | 0 | src/encoder/encoder.c:797 |
| `N265_QP_MAP_TRACE` | ? | src/encoder/encoder.c:2323 |
| `N265_QP_ROUND1` | 1 | src/encoder/encoder.c:707 |
| `N265_QP_TRACE` | ? | src/encoder/encoder.c:2339 |
| `N265_RC_LAG` | 0 | src/encoder/encoder.c:836 |
| `N265_RC_TRACE` | ? | src/encoder/rc.c:152 |
| `N265_RDOQ_ADAPT` | 0 | src/encoder/rdoq.c:284 |
| `N265_RDOQ_LAM` | 100 | src/encoder/rdoq.c:290 |
| `N265_RECT_FLAT` | 1 | src/encoder/ctu.c:971 |
| `N265_RECT_RANK` | 50 | src/encoder/ctu.c:936 |
| `N265_REF_LAG_HINT` | -1 | src/encoder/encoder.c:2368 |
| `N265_RSKIP` | 2 | src/encoder/ctu.c:638 |
| `N265_SAO_ANIM` | 1 | src/encoder/sao.c:277 |
| `N265_SAO_BIAS` | 100 | src/encoder/sao.c:313 |
| `N265_SAO_CHROMA` | 1 | src/encoder/sao.c:468 |
| `N265_SAO_GATE` | 0 | src/encoder/sao.c:517 |
| `N265_SAO_QPLAM` | 0 | src/encoder/sao.c:303 |
| `N265_SAO_TRACE` | ? | src/encoder/sao.c:610 |
| `N265_SAO_TYPES` | 0 | src/encoder/sao.c:240 |
| `N265_SATD8` | 0 | src/encoder/satd.c:134 |
| `N265_SBH_RD` | 2 | src/encoder/rdoq.c:269 |
| `N265_SC_HOLD` | 1 | src/encoder/la.c:191 |
| `N265_SC_TRACE` | ? | src/encoder/la.c:379 |
| `N265_SKIP_EXIT` | 0 | src/encoder/ctu.c:709 |
| `N265_STALL_TRACE` | ? | src/encoder/encoder.c:992 |
| `N265_SUBME` | 3 | src/encoder/mv.c:538 |
| `N265_SUBPEL_DEDUP` | 1 | src/encoder/mv.c:110 |

## arm (13)

| knob | default | reader |
|---|---|---|
| `N265_AQ` | ? | src/encoder/encoder.c:757 |
| `N265_AQ_FLAT` | ? | src/encoder/encoder.c:2083 |
| `N265_BADAPT_DEG` | ? | src/encoder/encoder.c:2696 |
| `N265_CT_LAMBDA` | ? | src/encoder/encoder.c:631 |
| `N265_FULLPEL` | ? | src/encoder/mv.c:104 |
| `N265_LAMBDA` | ? | src/encoder/ctu.c:579 |
| `N265_NO_AMVP` | ? | src/encoder/ctu.c:48 |
| `N265_NO_EARLYSKIP` | ? | src/encoder/ctu.c:60 |
| `N265_NO_GATES` | ? | src/encoder/ctu.c:42 |
| `N265_NO_SAO` | ? | src/encoder/encoder.c:751 |
| `N265_NO_TMVP` | ? | src/encoder/encoder.c:1120 |
| `N265_OLD_BS` | ? | src/encoder/deblock.c:146 |
| `N265_PB_STEP` | 0 | src/encoder/encoder.c:2007 |
