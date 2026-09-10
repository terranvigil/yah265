# The N265_* knob census (generated -- edit scripts/knob_census.py OVERRIDES, not this file)

Regenerate: `python3 scripts/knob_census.py`. CI check: `--check`.

Tier meanings: **default** = shipped behaviour, the env is its escape; **instrument** = measurement machinery (catalogued in docs/instruments.md when it has produced a result); **arm** = default-off gate or kept refuted arm -- kept deliberately because a refuted arm retains its re-pricing value.

## default (7)

| knob | default | reader |
|---|---|---|
| `N265_ABR_SB` | 1.7 | src/encoder/rc.c:127 |
| `N265_ABR_SBR` | 4.0 | src/encoder/rc.c:126 |
| `N265_ABR_SP` | 13.3 | src/encoder/rc.c:125 |
| `N265_GATE_SCALE` | 100 | src/encoder/ctu.c:632 |
| `N265_RDOQ` | 1 | src/encoder/encoder.c:353 |
| `N265_RDOQ_UP` | 1 | src/encoder/rdoq.c:207 |
| `N265_SKIP_THR` | 2 | src/encoder/ctu.c:563 |

## instrument (79)

| knob | default | reader |
|---|---|---|
| `N265_ABR_FBMAX` | 6.0 | src/encoder/rc.c:113 |
| `N265_ABR_HORIZON` | 6.0 | src/encoder/rc.c:105 |
| `N265_ABR_IGAIN` | 0.085 | src/encoder/rc.c:111 |
| `N265_ABR_IMAX` | 8.0 | src/encoder/rc.c:112 |
| `N265_ABR_LVL` | 2.0 | src/encoder/rc.c:128 |
| `N265_ABR_OPENSTEP` | 6.0 | src/encoder/rc.c:110 |
| `N265_ABR_PLANERR` | 0 | src/encoder/rc.c:114 |
| `N265_ABR_QCOMP` | 0.6 | src/encoder/rc.c:104 |
| `N265_ABR_SI` | 25.4 | src/encoder/rc.c:124 |
| `N265_ABR_SMEM` | 16.0 | src/encoder/rc.c:109 |
| `N265_ABR_STEP` | 1.0 | src/encoder/rc.c:108 |
| `N265_ABR_WMEM` | 1000.0 | src/encoder/rc.c:115 |
| `N265_AQ_ANCHOR` | ? | src/encoder/encoder.c:1210 |
| `N265_AQ_CLAMP` | ? | src/encoder/encoder.c:1232 |
| `N265_AQ_DC` | ? | src/encoder/encoder.c:1210 |
| `N265_AQ_FLOOR` | 4 | src/encoder/encoder.c:1178 |
| `N265_AQ_STR` | 1 | src/encoder/encoder.c:331 |
| `N265_AQ_TRACE` | ? | src/encoder/encoder.c:1233 |
| `N265_BADAPT_REFW` | ? | src/encoder/encoder.c:1563 |
| `N265_BADAPT_THR` | ? | src/encoder/encoder.c:1562 |
| `N265_BADAPT_TRACE` | ? | src/encoder/la.c:432 |
| `N265_BI_GATE` | 4 | src/encoder/ctu.c:1443 |
| `N265_BI_REFINE` | 1 | src/encoder/ctu.c:1395 |
| `N265_B_INTRA_MARGIN` | 30 | src/encoder/ctu.c:1379 |
| `N265_CB64_AMVP` | 1 | src/encoder/ctu.c:1364 |
| `N265_CB64_GATE` | 1 | src/encoder/ctu.c:1350 |
| `N265_CB64_TRACE` | ? | src/encoder/ctu.c:66 |
| `N265_CBF_FAST` | 3 | src/encoder/ctu.c:616 |
| `N265_CRF_SHIFT` | 5.4 | src/encoder/encoder.c:1095 |
| `N265_CT_CENTRE` | 0 | src/encoder/la.c:628 |
| `N265_CT_TRACE` | ? | src/encoder/la.c:665 |
| `N265_CUTREE_STR` | ? | src/encoder/encoder.c:326 |
| `N265_CU_STATS` | ? | src/encoder/encoder.c:350 |
| `N265_DBF_BETA` | 0 | src/encoder/encoder.c:364 |
| `N265_DBF_TC` | 0 | src/encoder/encoder.c:365 |
| `N265_DECIMATE` | 15 | src/encoder/rdoq.c:225 |
| `N265_DEPTH_BOUND` | 0 | src/encoder/ctu.c:814 |
| `N265_ES_THR` | 100 | src/encoder/ctu.c:581 |
| `N265_HFCACHE` | 1 | src/encoder/encoder.c:633 |
| `N265_INTRA_GATE` | 5 | src/encoder/ctu.c:672 |
| `N265_INTRA_MPM` | 0 | src/encoder/ctu.c:1854 |
| `N265_INTRA_RD_N` | 3 | src/encoder/ctu.c:1843 |
| `N265_I_INTRA` | 0 | src/encoder/ctu.c:1870 |
| `N265_MAX_FRAME_WORK` | 4 | src/encoder/encoder.c:381 |
| `N265_MERGE_RD_N` | ? | src/encoder/ctu.c:1414 |
| `N265_MERGE_TRACE` | -1 | src/encoder/mv.c:98 |
| `N265_ME_PATTERN` | 0 | src/encoder/mv.c:511 |
| `N265_ME_SATD` | 0 | src/encoder/ctu.c:1301 |
| `N265_MIN_KEYINT` | 4 | src/encoder/encoder.c:1489 |
| `N265_MODE_HIST` | ? | src/encoder/sao.c:156 |
| `N265_MVD_BITS` | 0 | src/encoder/ctu.c:1289 |
| `N265_NO_AMP` | ? | src/encoder/encoder.c:344 |
| `N265_NO_RECT` | ? | src/encoder/encoder.c:342 |
| `N265_NO_SPLIT` | ? | src/encoder/ctu.c:54 |
| `N265_PART_GATE` | 0 | src/encoder/ctu.c:727 |
| `N265_PSY` | 0.0 | src/encoder/encoder.c:354 |
| `N265_QP_FRAC` | = --bitrate | src/encoder/encoder.c:320 |
| `N265_QP_ROUND1` | 1 | src/encoder/encoder.c:332 |
| `N265_QP_TRACE` | ? | src/encoder/encoder.c:1285 |
| `N265_RC_LAG` | 0 | src/encoder/encoder.c:384 |
| `N265_RC_TRACE` | ? | src/encoder/rc.c:134 |
| `N265_RDOQ_ADAPT` | 0 | src/encoder/rdoq.c:213 |
| `N265_RDOQ_LAM` | 100 | src/encoder/rdoq.c:219 |
| `N265_RECT_RANK` | 0 | src/encoder/ctu.c:757 |
| `N265_RSKIP` | 0 | src/encoder/ctu.c:599 |
| `N265_SAO_BIAS` | 100 | src/encoder/sao.c:207 |
| `N265_SAO_CHROMA` | 1 | src/encoder/sao.c:352 |
| `N265_SAO_GATE` | 2 | src/encoder/sao.c:384 |
| `N265_SAO_QPLAM` | 0 | src/encoder/sao.c:197 |
| `N265_SAO_TRACE` | ? | src/encoder/sao.c:471 |
| `N265_SAO_TYPES` | 0 | src/encoder/sao.c:175 |
| `N265_SATD8` | 0 | src/encoder/satd.c:132 |
| `N265_SBH_RD` | 2 | src/encoder/rdoq.c:198 |
| `N265_SC_HOLD` | 1 | src/encoder/la.c:267 |
| `N265_SC_TRACE` | ? | src/encoder/la.c:288 |
| `N265_SKIP_EXIT` | 0 | src/encoder/ctu.c:649 |
| `N265_STALL_TRACE` | ? | src/encoder/encoder.c:475 |
| `N265_SUBME` | 0 | src/encoder/mv.c:499 |
| `N265_SUBPEL_DEDUP` | 1 | src/encoder/mv.c:110 |

## arm (12)

| knob | default | reader |
|---|---|---|
| `N265_AQ` | ? | src/encoder/encoder.c:355 |
| `N265_AQ_FLAT` | ? | src/encoder/encoder.c:1145 |
| `N265_BADAPT_DEG` | ? | src/encoder/encoder.c:1580 |
| `N265_FULLPEL` | ? | src/encoder/mv.c:104 |
| `N265_LAMBDA` | ? | src/encoder/ctu.c:546 |
| `N265_NO_AMVP` | ? | src/encoder/ctu.c:48 |
| `N265_NO_EARLYSKIP` | ? | src/encoder/ctu.c:60 |
| `N265_NO_GATES` | ? | src/encoder/ctu.c:42 |
| `N265_NO_SAO` | ? | src/encoder/encoder.c:349 |
| `N265_NO_TMVP` | ? | src/encoder/encoder.c:593 |
| `N265_OLD_BS` | ? | src/encoder/deblock.c:152 |
| `N265_PB_STEP` | 0 | src/encoder/encoder.c:1102 |
