# The N265_* knob census (generated -- edit scripts/knob_census.py OVERRIDES, not this file)

Regenerate: `python3 scripts/knob_census.py`. CI check: `--check`.

Tier meanings: **default** = shipped behaviour, the env is its escape; **instrument** = measurement machinery (catalogued in docs/instruments.md when it has produced a result); **arm** = default-off gate or kept refuted arm -- kept deliberately because a refuted arm retains its re-pricing value.

## default (3)

| knob | default | reader |
|---|---|---|
| `N265_RDOQ` | 1 | src/encoder/encoder.c:128 |
| `N265_RDOQ_UP` | 1 | src/encoder/rdoq.c:206 |
| `N265_SKIP_THR` | 2 | src/encoder/ctu.c:401 |

## instrument (48)

| knob | default | reader |
|---|---|---|
| `N265_AQ_ANCHOR` | ? | src/encoder/encoder.c:559 |
| `N265_AQ_DC` | ? | src/encoder/encoder.c:559 |
| `N265_AQ_STR` | 1 | src/encoder/encoder.c:114 |
| `N265_BI_GATE` | 4 | src/encoder/ctu.c:1241 |
| `N265_BI_REFINE` | 1 | src/encoder/ctu.c:1193 |
| `N265_B_INTRA_MARGIN` | 30 | src/encoder/ctu.c:1177 |
| `N265_CB64_AMVP` | 1 | src/encoder/ctu.c:1162 |
| `N265_CB64_GATE` | 1 | src/encoder/ctu.c:1148 |
| `N265_CB64_TRACE` | ? | src/encoder/ctu.c:3020 |
| `N265_CBF_FAST` | 3 | src/encoder/ctu.c:449 |
| `N265_CRF_SHIFT` | 5.4 | src/encoder/encoder.c:482 |
| `N265_CT_TRACE` | ? | src/encoder/la.c:372 |
| `N265_CUTREE_STR` | ? | src/encoder/encoder.c:109 |
| `N265_DECIMATE` | 15 | src/encoder/rdoq.c:487 |
| `N265_DEPTH_BOUND` | 0 | src/encoder/ctu.c:627 |
| `N265_ES_THR` | 100 | src/encoder/ctu.c:419 |
| `N265_HFCACHE` | 1 | src/encoder/encoder.c:357 |
| `N265_INTRA_GATE` | 4 | src/encoder/ctu.c:485 |
| `N265_INTRA_MPM` | 0 | src/encoder/ctu.c:1651 |
| `N265_INTRA_RD_N` | 3 | src/encoder/ctu.c:1640 |
| `N265_MERGE_RD_N` | ? | src/encoder/ctu.c:1212 |
| `N265_MERGE_TRACE` | -1 | src/encoder/mv.c:246 |
| `N265_ME_PATTERN` | 0 | src/encoder/mv.c:456 |
| `N265_ME_SATD` | 0 | src/encoder/ctu.c:1099 |
| `N265_MODE_HIST` | ? | src/encoder/sao.c:599 |
| `N265_MVD_BITS` | 0 | src/encoder/ctu.c:1087 |
| `N265_NO_AMP` | ? | src/encoder/encoder.c:121 |
| `N265_NO_RECT` | ? | src/encoder/encoder.c:120 |
| `N265_NO_SPLIT` | ? | src/encoder/ctu.c:2943 |
| `N265_PART_GATE` | 0 | src/encoder/ctu.c:540 |
| `N265_PSY` | 0.0 | src/encoder/encoder.c:129 |
| `N265_QP_ROUND1` | 1 | src/encoder/encoder.c:115 |
| `N265_QP_TRACE` | ? | src/encoder/encoder.c:583 |
| `N265_RDOQ_ADAPT` | 0 | src/encoder/rdoq.c:217 |
| `N265_RDOQ_LAM` | 100 | src/encoder/rdoq.c:244 |
| `N265_RECT_RANK` | 0 | src/encoder/ctu.c:570 |
| `N265_RSKIP` | 0 | src/encoder/ctu.c:432 |
| `N265_SAO_BIAS` | 100 | src/encoder/sao.c:182 |
| `N265_SAO_CHROMA` | 1 | src/encoder/sao.c:327 |
| `N265_SAO_GATE` | 2 | src/encoder/sao.c:359 |
| `N265_SAO_QPLAM` | 0 | src/encoder/sao.c:172 |
| `N265_SAO_TRACE` | ? | src/encoder/sao.c:444 |
| `N265_SAO_TYPES` | 0 | src/encoder/sao.c:150 |
| `N265_SBH_RD` | 2 | src/encoder/rdoq.c:190 |
| `N265_SC_TRACE` | ? | src/encoder/la.c:242 |
| `N265_SKIP_EXIT` | 0 | src/encoder/ctu.c:465 |
| `N265_SUBME` | 0 | src/encoder/mv.c:444 |
| `N265_SUBPEL_DEDUP` | 1 | src/encoder/mv.c:592 |

## arm (12)

| knob | default | reader |
|---|---|---|
| `N265_AQ` | ? | src/encoder/encoder.c:130 |
| `N265_AQ_FLAT` | ? | src/encoder/encoder.c:524 |
| `N265_BADAPT_THR` | ? | src/encoder/encoder.c:785 |
| `N265_FULLPEL` | ? | src/encoder/mv.c:589 |
| `N265_LAMBDA` | ? | src/encoder/ctu.c:384 |
| `N265_NO_AMVP` | ? | src/encoder/ctu.c:2384 |
| `N265_NO_EARLYSKIP` | ? | src/encoder/ctu.c:2966 |
| `N265_NO_GATES` | ? | src/encoder/ctu.c:2197 |
| `N265_NO_SAO` | ? | src/encoder/encoder.c:125 |
| `N265_NO_TMVP` | ? | src/encoder/encoder.c:317 |
| `N265_OLD_BS` | ? | src/encoder/deblock.c:151 |
| `N265_PB_STEP` | 0 | src/encoder/encoder.c:489 |
