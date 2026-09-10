# The N265_* knob census (generated -- edit scripts/knob_census.py OVERRIDES, not this file)

Regenerate: `python3 scripts/knob_census.py`. CI check: `--check`.

Tier meanings: **default** = shipped behaviour, the env is its escape; **instrument** = measurement machinery (catalogued in docs/instruments.md when it has produced a result); **arm** = default-off gate or kept refuted arm -- kept deliberately because a refuted arm retains its re-pricing value.

## default (7)

| knob | default | reader |
|---|---|---|
| `N265_ABR_SB` | 1.7 | src/encoder/rc.c:126 |
| `N265_ABR_SBR` | 4.0 | src/encoder/rc.c:125 |
| `N265_ABR_SP` | 13.3 | src/encoder/rc.c:124 |
| `N265_GATE_SCALE` | 100 | src/encoder/ctu.c:631 |
| `N265_RDOQ` | 1 | src/encoder/encoder.c:362 |
| `N265_RDOQ_UP` | 1 | src/encoder/rdoq.c:206 |
| `N265_SKIP_THR` | 2 | src/encoder/ctu.c:562 |

## instrument (81)

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
| `N265_API_DEFER` | ? | src/encoder/encoder.c:463 |
| `N265_AQ_ANCHOR` | ? | src/encoder/encoder.c:1296 |
| `N265_AQ_CLAMP` | ? | src/encoder/encoder.c:1318 |
| `N265_AQ_DC` | ? | src/encoder/encoder.c:1296 |
| `N265_AQ_FLOOR` | 4 | src/encoder/encoder.c:1264 |
| `N265_AQ_STR` | 1 | src/encoder/encoder.c:347 |
| `N265_AQ_TRACE` | ? | src/encoder/encoder.c:1319 |
| `N265_BADAPT_REFW` | ? | src/encoder/encoder.c:1659 |
| `N265_BADAPT_THR` | ? | src/encoder/encoder.c:1658 |
| `N265_BADAPT_TRACE` | ? | src/encoder/la.c:437 |
| `N265_BI_GATE` | 4 | src/encoder/ctu.c:1442 |
| `N265_BI_REFINE` | 1 | src/encoder/ctu.c:1394 |
| `N265_B_INTRA_MARGIN` | 30 | src/encoder/ctu.c:1378 |
| `N265_CB64_AMVP` | 1 | src/encoder/ctu.c:1363 |
| `N265_CB64_GATE` | 1 | src/encoder/ctu.c:1349 |
| `N265_CB64_TRACE` | ? | src/encoder/ctu.c:65 |
| `N265_CBF_FAST` | 3 | src/encoder/ctu.c:615 |
| `N265_CRF_SHIFT` | 5.4 | src/encoder/encoder.c:1181 |
| `N265_CT_CENTRE` | 0 | src/encoder/la.c:633 |
| `N265_CT_TRACE` | ? | src/encoder/la.c:670 |
| `N265_CUTREE_STR` | ? | src/encoder/encoder.c:342 |
| `N265_CU_STATS` | ? | src/encoder/encoder.c:359 |
| `N265_DBF_BETA` | 0 | src/encoder/encoder.c:373 |
| `N265_DBF_TC` | 0 | src/encoder/encoder.c:374 |
| `N265_DECIMATE` | 15 | src/encoder/rdoq.c:224 |
| `N265_DEPTH_BOUND` | 0 | src/encoder/ctu.c:813 |
| `N265_ES_THR` | 100 | src/encoder/ctu.c:580 |
| `N265_HFCACHE` | 1 | src/encoder/encoder.c:694 |
| `N265_INTRA_GATE` | 5 | src/encoder/ctu.c:671 |
| `N265_INTRA_MPM` | 0 | src/encoder/ctu.c:1853 |
| `N265_INTRA_RD_N` | 3 | src/encoder/ctu.c:1842 |
| `N265_I_INTRA` | 0 | src/encoder/ctu.c:1869 |
| `N265_LA_PROF` | 0.0 | src/encoder/encoder.c:464 |
| `N265_MAX_FRAME_WORK` | ? | src/encoder/encoder.c:390 |
| `N265_MERGE_RD_N` | ? | src/encoder/ctu.c:1413 |
| `N265_MERGE_TRACE` | -1 | src/encoder/mv.c:97 |
| `N265_ME_PATTERN` | 0 | src/encoder/mv.c:528 |
| `N265_ME_SATD` | 0 | src/encoder/ctu.c:1300 |
| `N265_MIN_KEYINT` | 4 | src/encoder/encoder.c:1585 |
| `N265_MODE_HIST` | ? | src/encoder/sao.c:155 |
| `N265_MVD_BITS` | 0 | src/encoder/ctu.c:1288 |
| `N265_NO_AMP` | ? | src/encoder/encoder.c:354 |
| `N265_NO_RECT` | ? | src/encoder/encoder.c:353 |
| `N265_NO_SPLIT` | ? | src/encoder/ctu.c:53 |
| `N265_PART_GATE` | 0 | src/encoder/ctu.c:726 |
| `N265_PSY` | 0.0 | src/encoder/encoder.c:363 |
| `N265_QP_FRAC` | = --bitrate | src/encoder/encoder.c:336 |
| `N265_QP_ROUND1` | 1 | src/encoder/encoder.c:348 |
| `N265_QP_TRACE` | ? | src/encoder/encoder.c:1371 |
| `N265_RC_LAG` | 0 | src/encoder/encoder.c:405 |
| `N265_RC_TRACE` | ? | src/encoder/rc.c:133 |
| `N265_RDOQ_ADAPT` | 0 | src/encoder/rdoq.c:212 |
| `N265_RDOQ_LAM` | 100 | src/encoder/rdoq.c:218 |
| `N265_RECT_RANK` | 0 | src/encoder/ctu.c:756 |
| `N265_REF_LAG_HINT` | -1 | src/encoder/encoder.c:1400 |
| `N265_RSKIP` | 0 | src/encoder/ctu.c:598 |
| `N265_SAO_BIAS` | 100 | src/encoder/sao.c:206 |
| `N265_SAO_CHROMA` | 1 | src/encoder/sao.c:351 |
| `N265_SAO_GATE` | 2 | src/encoder/sao.c:383 |
| `N265_SAO_QPLAM` | 0 | src/encoder/sao.c:196 |
| `N265_SAO_TRACE` | ? | src/encoder/sao.c:470 |
| `N265_SAO_TYPES` | 0 | src/encoder/sao.c:174 |
| `N265_SBH_RD` | 2 | src/encoder/rdoq.c:197 |
| `N265_SC_HOLD` | 1 | src/encoder/la.c:272 |
| `N265_SC_TRACE` | ? | src/encoder/la.c:293 |
| `N265_SKIP_EXIT` | 0 | src/encoder/ctu.c:648 |
| `N265_STALL_TRACE` | ? | src/encoder/encoder.c:530 |
| `N265_SUBME` | 0 | src/encoder/mv.c:516 |
| `N265_SUBPEL_DEDUP` | 1 | src/encoder/mv.c:109 |

## arm (13)

| knob | default | reader |
|---|---|---|
| `N265_AQ` | ? | src/encoder/encoder.c:364 |
| `N265_AQ_FLAT` | ? | src/encoder/encoder.c:1231 |
| `N265_BADAPT_DEG` | ? | src/encoder/encoder.c:1676 |
| `N265_FULLPEL` | ? | src/encoder/mv.c:103 |
| `N265_LAMBDA` | ? | src/encoder/ctu.c:545 |
| `N265_NO_AMVP` | ? | src/encoder/ctu.c:47 |
| `N265_NO_EARLYSKIP` | ? | src/encoder/ctu.c:59 |
| `N265_NO_GATES` | ? | src/encoder/ctu.c:41 |
| `N265_NO_SAO` | ? | src/encoder/encoder.c:358 |
| `N265_NO_TMVP` | ? | src/encoder/encoder.c:654 |
| `N265_OLD_BS` | ? | src/encoder/deblock.c:151 |
| `N265_PB_STEP` | 0 | src/encoder/encoder.c:1188 |
| `N265_SATD8` | 0 | src/encoder/satd.c:131 |
