# The N265_* knob census (generated -- edit scripts/knob_census.py OVERRIDES, not this file)

Regenerate: `python3 scripts/knob_census.py`. CI check: `--check`.

Tier meanings: **default** = shipped behaviour, the env is its escape; **instrument** = measurement machinery (catalogued in docs/instruments.md when it has produced a result); **arm** = default-off gate or kept refuted arm -- kept deliberately because a refuted arm retains its re-pricing value.

## default (7)

| knob | default | reader |
|---|---|---|
| `N265_ABR_SB` | 1.7 | src/encoder/rc.c:126 |
| `N265_ABR_SBR` | 4.0 | src/encoder/rc.c:125 |
| `N265_ABR_SP` | 13.3 | src/encoder/rc.c:124 |
| `N265_GATE_SCALE` | 100 | src/encoder/ctu.c:610 |
| `N265_RDOQ` | 1 | src/encoder/encoder.c:357 |
| `N265_RDOQ_UP` | 1 | src/encoder/rdoq.c:199 |
| `N265_SKIP_THR` | 2 | src/encoder/ctu.c:541 |

## instrument (74)

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
| `N265_API_DEFER` | ? | src/encoder/encoder.c:450 |
| `N265_AQ_ANCHOR` | ? | src/encoder/encoder.c:1259 |
| `N265_AQ_CLAMP` | ? | src/encoder/encoder.c:1281 |
| `N265_AQ_DC` | ? | src/encoder/encoder.c:1259 |
| `N265_AQ_STR` | 1 | src/encoder/encoder.c:343 |
| `N265_BADAPT_REFW` | ? | src/encoder/encoder.c:1541 |
| `N265_BADAPT_THR` | ? | src/encoder/encoder.c:1540 |
| `N265_BADAPT_TRACE` | ? | src/encoder/la.c:419 |
| `N265_BI_GATE` | 4 | src/encoder/ctu.c:1407 |
| `N265_BI_REFINE` | 1 | src/encoder/ctu.c:1359 |
| `N265_B_INTRA_MARGIN` | 30 | src/encoder/ctu.c:1343 |
| `N265_CB64_AMVP` | 1 | src/encoder/ctu.c:1328 |
| `N265_CB64_GATE` | 1 | src/encoder/ctu.c:1314 |
| `N265_CB64_TRACE` | ? | src/encoder/ctu.c:65 |
| `N265_CBF_FAST` | 3 | src/encoder/ctu.c:594 |
| `N265_CRF_SHIFT` | 5.4 | src/encoder/encoder.c:1174 |
| `N265_CT_CENTRE` | 0 | src/encoder/la.c:615 |
| `N265_CT_TRACE` | ? | src/encoder/la.c:652 |
| `N265_CUTREE_STR` | ? | src/encoder/encoder.c:338 |
| `N265_DECIMATE` | 15 | src/encoder/rdoq.c:217 |
| `N265_DEPTH_BOUND` | 0 | src/encoder/ctu.c:792 |
| `N265_ES_THR` | 100 | src/encoder/ctu.c:559 |
| `N265_HFCACHE` | 1 | src/encoder/encoder.c:682 |
| `N265_INTRA_GATE` | 5 | src/encoder/ctu.c:650 |
| `N265_INTRA_MPM` | 0 | src/encoder/ctu.c:1818 |
| `N265_INTRA_RD_N` | 3 | src/encoder/ctu.c:1807 |
| `N265_I_INTRA` | 0 | src/encoder/ctu.c:1834 |
| `N265_LA_PROF` | 0.0 | src/encoder/encoder.c:451 |
| `N265_MAX_FRAME_WORK` | ? | src/encoder/encoder.c:377 |
| `N265_MERGE_RD_N` | ? | src/encoder/ctu.c:1378 |
| `N265_MERGE_TRACE` | -1 | src/encoder/mv.c:97 |
| `N265_ME_PATTERN` | 0 | src/encoder/mv.c:519 |
| `N265_ME_SATD` | 0 | src/encoder/ctu.c:1265 |
| `N265_MODE_HIST` | ? | src/encoder/sao.c:163 |
| `N265_MVD_BITS` | 0 | src/encoder/ctu.c:1253 |
| `N265_NO_AMP` | ? | src/encoder/encoder.c:350 |
| `N265_NO_RECT` | ? | src/encoder/encoder.c:349 |
| `N265_NO_SPLIT` | ? | src/encoder/ctu.c:53 |
| `N265_PART_GATE` | 0 | src/encoder/ctu.c:705 |
| `N265_PSY` | 0.0 | src/encoder/encoder.c:358 |
| `N265_QP_FRAC` | = --bitrate | src/encoder/encoder.c:332 |
| `N265_QP_ROUND1` | 1 | src/encoder/encoder.c:344 |
| `N265_QP_TRACE` | ? | src/encoder/encoder.c:1317 |
| `N265_RC_LAG` | 0 | src/encoder/encoder.c:392 |
| `N265_RC_TRACE` | ? | src/encoder/rc.c:133 |
| `N265_RDOQ_ADAPT` | 0 | src/encoder/rdoq.c:205 |
| `N265_RDOQ_LAM` | 100 | src/encoder/rdoq.c:211 |
| `N265_RECT_RANK` | 0 | src/encoder/ctu.c:735 |
| `N265_REF_LAG_HINT` | -1 | src/encoder/encoder.c:1346 |
| `N265_RSKIP` | 0 | src/encoder/ctu.c:577 |
| `N265_SAO_BIAS` | 100 | src/encoder/sao.c:203 |
| `N265_SAO_CHROMA` | 1 | src/encoder/sao.c:348 |
| `N265_SAO_GATE` | 2 | src/encoder/sao.c:380 |
| `N265_SAO_QPLAM` | 0 | src/encoder/sao.c:193 |
| `N265_SAO_TRACE` | ? | src/encoder/sao.c:467 |
| `N265_SAO_TYPES` | 0 | src/encoder/sao.c:171 |
| `N265_SBH_RD` | 2 | src/encoder/rdoq.c:190 |
| `N265_SC_TRACE` | ? | src/encoder/la.c:275 |
| `N265_SKIP_EXIT` | 0 | src/encoder/ctu.c:627 |
| `N265_STALL_TRACE` | ? | src/encoder/encoder.c:516 |
| `N265_SUBME` | 0 | src/encoder/mv.c:507 |
| `N265_SUBPEL_DEDUP` | 1 | src/encoder/mv.c:109 |

## arm (12)

| knob | default | reader |
|---|---|---|
| `N265_AQ` | ? | src/encoder/encoder.c:359 |
| `N265_AQ_FLAT` | ? | src/encoder/encoder.c:1224 |
| `N265_BADAPT_DEG` | ? | src/encoder/encoder.c:1558 |
| `N265_FULLPEL` | ? | src/encoder/mv.c:103 |
| `N265_LAMBDA` | ? | src/encoder/ctu.c:524 |
| `N265_NO_AMVP` | ? | src/encoder/ctu.c:47 |
| `N265_NO_EARLYSKIP` | ? | src/encoder/ctu.c:59 |
| `N265_NO_GATES` | ? | src/encoder/ctu.c:41 |
| `N265_NO_SAO` | ? | src/encoder/encoder.c:354 |
| `N265_NO_TMVP` | ? | src/encoder/encoder.c:642 |
| `N265_OLD_BS` | ? | src/encoder/deblock.c:151 |
| `N265_PB_STEP` | 0 | src/encoder/encoder.c:1181 |
