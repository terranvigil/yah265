# The N265_* knob census (generated -- edit scripts/knob_census.py OVERRIDES, not this file)

Regenerate: `python3 scripts/knob_census.py`. CI check: `--check`.

Tier meanings: **default** = shipped behaviour, the env is its escape; **instrument** = measurement machinery (catalogued in docs/instruments.md when it has produced a result); **arm** = default-off gate or kept refuted arm -- kept deliberately because a refuted arm retains its re-pricing value.

## default (3)

| knob | default | reader |
|---|---|---|
| `N265_RDOQ` | 1 | src/encoder/encoder.c:128 |
| `N265_RDOQ_UP` | 1 | src/encoder/rdoq.c:206 |
| `N265_SKIP_THR` | 2 | src/encoder/ctu.c:401 |

## instrument (35)

| knob | default | reader |
|---|---|---|
| `N265_AQ_ANCHOR` | ? | src/encoder/encoder.c:559 |
| `N265_AQ_DC` | ? | src/encoder/encoder.c:559 |
| `N265_AQ_STR` | 1 | src/encoder/encoder.c:114 |
| `N265_BI_REFINE` | 1 | src/encoder/ctu.c:1066 |
| `N265_B_INTRA_MARGIN` | 30 | src/encoder/ctu.c:1050 |
| `N265_CB64_AMVP` | 1 | src/encoder/ctu.c:1035 |
| `N265_CB64_TRACE` | ? | src/encoder/ctu.c:2752 |
| `N265_CBF_FAST` | 3 | src/encoder/ctu.c:449 |
| `N265_CRF_SHIFT` | 5.4 | src/encoder/encoder.c:482 |
| `N265_CT_TRACE` | ? | src/encoder/la.c:372 |
| `N265_CUTREE_STR` | ? | src/encoder/encoder.c:109 |
| `N265_DECIMATE` | 15 | src/encoder/rdoq.c:487 |
| `N265_DEPTH_BOUND` | 0 | src/encoder/ctu.c:550 |
| `N265_ES_THR` | 100 | src/encoder/ctu.c:419 |
| `N265_HFCACHE` | 1 | src/encoder/encoder.c:357 |
| `N265_INTRA_GATE` | 4 | src/encoder/ctu.c:469 |
| `N265_INTRA_MPM` | 0 | src/encoder/ctu.c:1458 |
| `N265_INTRA_RD_N` | 3 | src/encoder/ctu.c:1447 |
| `N265_MERGE_TRACE` | ? | src/encoder/mv.c:239 |
| `N265_ME_SATD` | 0 | src/encoder/ctu.c:1022 |
| `N265_MODE_HIST` | ? | src/encoder/sao.c:272 |
| `N265_MVD_BITS` | 0 | src/encoder/ctu.c:1010 |
| `N265_NO_AMP` | ? | src/encoder/encoder.c:121 |
| `N265_NO_RECT` | ? | src/encoder/encoder.c:120 |
| `N265_NO_SPLIT` | ? | src/encoder/ctu.c:2679 |
| `N265_PART_GATE` | 0 | src/encoder/ctu.c:524 |
| `N265_PSY` | 0.0 | src/encoder/encoder.c:129 |
| `N265_QP_ROUND1` | 1 | src/encoder/encoder.c:115 |
| `N265_QP_TRACE` | ? | src/encoder/encoder.c:583 |
| `N265_RDOQ_ADAPT` | 0 | src/encoder/rdoq.c:217 |
| `N265_RDOQ_LAM` | 100 | src/encoder/rdoq.c:244 |
| `N265_RSKIP` | 0 | src/encoder/ctu.c:432 |
| `N265_SBH_RD` | 2 | src/encoder/rdoq.c:190 |
| `N265_SC_TRACE` | ? | src/encoder/la.c:242 |
| `N265_SUBPEL_DEDUP` | 1 | src/encoder/mv.c:469 |

## arm (12)

| knob | default | reader |
|---|---|---|
| `N265_AQ` | ? | src/encoder/encoder.c:130 |
| `N265_AQ_FLAT` | ? | src/encoder/encoder.c:524 |
| `N265_BADAPT_THR` | ? | src/encoder/encoder.c:783 |
| `N265_FULLPEL` | ? | src/encoder/mv.c:466 |
| `N265_LAMBDA` | ? | src/encoder/ctu.c:384 |
| `N265_NO_AMVP` | ? | src/encoder/ctu.c:2147 |
| `N265_NO_EARLYSKIP` | ? | src/encoder/ctu.c:2702 |
| `N265_NO_GATES` | ? | src/encoder/ctu.c:1996 |
| `N265_NO_SAO` | ? | src/encoder/encoder.c:125 |
| `N265_NO_TMVP` | ? | src/encoder/encoder.c:317 |
| `N265_OLD_BS` | ? | src/encoder/deblock.c:151 |
| `N265_PB_STEP` | 0 | src/encoder/encoder.c:489 |
