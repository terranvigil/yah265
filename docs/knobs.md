# The N265_* knob census (generated -- edit scripts/knob_census.py OVERRIDES, not this file)

Regenerate: `python3 scripts/knob_census.py`. CI check: `--check`.

Tier meanings: **default** = shipped behaviour, the env is its escape; **instrument** = measurement machinery (catalogued in docs/instruments.md when it has produced a result); **arm** = default-off gate or kept refuted arm -- kept deliberately because a refuted arm retains its re-pricing value.

## default (7)

| knob | default | reader |
|---|---|---|
| `N265_ABR_SB` | 1.7 | src/encoder/rc.c:126 |
| `N265_ABR_SBR` | 4.0 | src/encoder/rc.c:125 |
| `N265_ABR_SP` | 13.3 | src/encoder/rc.c:124 |
| `N265_GATE_SCALE` | 100 | src/encoder/ctu.c:606 |
| `N265_RDOQ` | 1 | src/encoder/encoder.c:344 |
| `N265_RDOQ_UP` | 1 | src/encoder/rdoq.c:199 |
| `N265_SKIP_THR` | 2 | src/encoder/ctu.c:537 |

## instrument (78)

| knob | default | reader |
|---|---|---|
| `N265_ABR_FBMAX` | 6.0 | src/encoder/rc.c:112 |
| `N265_ABR_HORIZON` | 6.0 | src/encoder/rc.c:104 |
| `N265_ABR_IGAIN` | 0.085 | src/encoder/rc.c:110 |
| `N265_ABR_IMAX` | 8.0 | src/encoder/rc.c:111 |
| `N265_ABR_LVL` | 2.0 | src/encoder/rc.c:127 |
| `N265_ABR_OPENSTEP` | 6.0 | src/encoder/rc.c:109 |
| `N265_ABR_PLANERR` | 0 | src/encoder/rc.c:113 |
| `N265_ABR_QCOMP` | 0.6 | src/encoder/rc.c:103 |
| `N265_ABR_SI` | 25.4 | src/encoder/rc.c:123 |
| `N265_ABR_SMEM` | 16.0 | src/encoder/rc.c:108 |
| `N265_ABR_STEP` | 1.0 | src/encoder/rc.c:107 |
| `N265_ABR_WMEM` | 1000.0 | src/encoder/rc.c:114 |
| `N265_AQ_ANCHOR` | ? | src/encoder/encoder.c:1197 |
| `N265_AQ_CLAMP` | ? | src/encoder/encoder.c:1219 |
| `N265_AQ_DC` | ? | src/encoder/encoder.c:1197 |
| `N265_AQ_FLOOR` | 4 | src/encoder/encoder.c:1165 |
| `N265_AQ_STR` | 1 | src/encoder/encoder.c:329 |
| `N265_AQ_TRACE` | ? | src/encoder/encoder.c:1220 |
| `N265_BADAPT_REFW` | ? | src/encoder/encoder.c:1550 |
| `N265_BADAPT_THR` | ? | src/encoder/encoder.c:1549 |
| `N265_BADAPT_TRACE` | ? | src/encoder/la.c:431 |
| `N265_BI_GATE` | 4 | src/encoder/ctu.c:1403 |
| `N265_BI_REFINE` | 1 | src/encoder/ctu.c:1355 |
| `N265_B_INTRA_MARGIN` | 30 | src/encoder/ctu.c:1339 |
| `N265_CB64_AMVP` | 1 | src/encoder/ctu.c:1324 |
| `N265_CB64_GATE` | 1 | src/encoder/ctu.c:1310 |
| `N265_CB64_TRACE` | ? | src/encoder/ctu.c:65 |
| `N265_CBF_FAST` | 3 | src/encoder/ctu.c:590 |
| `N265_CRF_SHIFT` | 5.4 | src/encoder/encoder.c:1082 |
| `N265_CT_CENTRE` | 0 | src/encoder/la.c:627 |
| `N265_CT_TRACE` | ? | src/encoder/la.c:664 |
| `N265_CUTREE_STR` | ? | src/encoder/encoder.c:324 |
| `N265_CU_STATS` | ? | src/encoder/encoder.c:341 |
| `N265_DBF_BETA` | 0 | src/encoder/encoder.c:355 |
| `N265_DBF_TC` | 0 | src/encoder/encoder.c:356 |
| `N265_DECIMATE` | 15 | src/encoder/rdoq.c:217 |
| `N265_DEPTH_BOUND` | 0 | src/encoder/ctu.c:788 |
| `N265_ES_THR` | 100 | src/encoder/ctu.c:555 |
| `N265_HFCACHE` | 1 | src/encoder/encoder.c:623 |
| `N265_INTRA_GATE` | 5 | src/encoder/ctu.c:646 |
| `N265_INTRA_MPM` | 0 | src/encoder/ctu.c:1814 |
| `N265_INTRA_RD_N` | 3 | src/encoder/ctu.c:1803 |
| `N265_I_INTRA` | 0 | src/encoder/ctu.c:1830 |
| `N265_MAX_FRAME_WORK` | 4 | src/encoder/encoder.c:372 |
| `N265_MERGE_RD_N` | ? | src/encoder/ctu.c:1374 |
| `N265_MERGE_TRACE` | -1 | src/encoder/mv.c:97 |
| `N265_ME_PATTERN` | 0 | src/encoder/mv.c:499 |
| `N265_ME_SATD` | 0 | src/encoder/ctu.c:1261 |
| `N265_MIN_KEYINT` | 4 | src/encoder/encoder.c:1476 |
| `N265_MODE_HIST` | ? | src/encoder/sao.c:155 |
| `N265_MVD_BITS` | 0 | src/encoder/ctu.c:1249 |
| `N265_NO_AMP` | ? | src/encoder/encoder.c:336 |
| `N265_NO_RECT` | ? | src/encoder/encoder.c:335 |
| `N265_NO_SPLIT` | ? | src/encoder/ctu.c:53 |
| `N265_PART_GATE` | 0 | src/encoder/ctu.c:701 |
| `N265_PSY` | 0.0 | src/encoder/encoder.c:345 |
| `N265_QP_FRAC` | = --bitrate | src/encoder/encoder.c:318 |
| `N265_QP_ROUND1` | 1 | src/encoder/encoder.c:330 |
| `N265_QP_TRACE` | ? | src/encoder/encoder.c:1272 |
| `N265_RC_LAG` | 0 | src/encoder/encoder.c:375 |
| `N265_RC_TRACE` | ? | src/encoder/rc.c:133 |
| `N265_RDOQ_ADAPT` | 0 | src/encoder/rdoq.c:205 |
| `N265_RDOQ_LAM` | 100 | src/encoder/rdoq.c:211 |
| `N265_RECT_RANK` | 0 | src/encoder/ctu.c:731 |
| `N265_RSKIP` | 0 | src/encoder/ctu.c:573 |
| `N265_SAO_BIAS` | 100 | src/encoder/sao.c:206 |
| `N265_SAO_CHROMA` | 1 | src/encoder/sao.c:351 |
| `N265_SAO_GATE` | 2 | src/encoder/sao.c:383 |
| `N265_SAO_QPLAM` | 0 | src/encoder/sao.c:196 |
| `N265_SAO_TRACE` | ? | src/encoder/sao.c:470 |
| `N265_SAO_TYPES` | 0 | src/encoder/sao.c:174 |
| `N265_SBH_RD` | 2 | src/encoder/rdoq.c:190 |
| `N265_SC_HOLD` | 1 | src/encoder/la.c:266 |
| `N265_SC_TRACE` | ? | src/encoder/la.c:287 |
| `N265_SKIP_EXIT` | 0 | src/encoder/ctu.c:623 |
| `N265_STALL_TRACE` | ? | src/encoder/encoder.c:465 |
| `N265_SUBME` | 0 | src/encoder/mv.c:487 |
| `N265_SUBPEL_DEDUP` | 1 | src/encoder/mv.c:109 |

## arm (12)

| knob | default | reader |
|---|---|---|
| `N265_AQ` | ? | src/encoder/encoder.c:346 |
| `N265_AQ_FLAT` | ? | src/encoder/encoder.c:1132 |
| `N265_BADAPT_DEG` | ? | src/encoder/encoder.c:1567 |
| `N265_FULLPEL` | ? | src/encoder/mv.c:103 |
| `N265_LAMBDA` | ? | src/encoder/ctu.c:520 |
| `N265_NO_AMVP` | ? | src/encoder/ctu.c:47 |
| `N265_NO_EARLYSKIP` | ? | src/encoder/ctu.c:59 |
| `N265_NO_GATES` | ? | src/encoder/ctu.c:41 |
| `N265_NO_SAO` | ? | src/encoder/encoder.c:340 |
| `N265_NO_TMVP` | ? | src/encoder/encoder.c:583 |
| `N265_OLD_BS` | ? | src/encoder/deblock.c:151 |
| `N265_PB_STEP` | 0 | src/encoder/encoder.c:1089 |
