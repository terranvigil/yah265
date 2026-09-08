# The N265_* knob census (generated -- edit scripts/knob_census.py OVERRIDES, not this file)

Regenerate: `python3 scripts/knob_census.py`. CI check: `--check`.

Tier meanings: **default** = shipped behaviour, the env is its escape; **instrument** = measurement machinery (catalogued in docs/instruments.md when it has produced a result); **arm** = default-off gate or kept refuted arm -- kept deliberately because a refuted arm retains its re-pricing value.

## default (3)

| knob | default | reader |
|---|---|---|
| `N265_RDOQ` | 1 | src/encoder/encoder.c:127 |
| `N265_RDOQ_UP` | 1 | src/encoder/rdoq.c:177 |
| `N265_SKIP_THR` | 2 | src/encoder/ctu.c:392 |

## instrument (20)

| knob | default | reader |
|---|---|---|
| `N265_AQ_ANCHOR` | ? | src/encoder/encoder.c:495 |
| `N265_AQ_DC` | ? | src/encoder/encoder.c:495 |
| `N265_AQ_STR` | 1 | src/encoder/encoder.c:113 |
| `N265_BI_REFINE` | 1 | src/encoder/ctu.c:867 |
| `N265_B_INTRA_MARGIN` | 30 | src/encoder/ctu.c:851 |
| `N265_CB64_AMVP` | 1 | src/encoder/ctu.c:836 |
| `N265_CB64_TRACE` | ? | src/encoder/ctu.c:2360 |
| `N265_CRF_SHIFT` | 5.4 | src/encoder/encoder.c:418 |
| `N265_CT_TRACE` | ? | src/encoder/la.c:372 |
| `N265_CUTREE_STR` | ? | src/encoder/encoder.c:108 |
| `N265_MERGE_TRACE` | ? | src/encoder/mv.c:239 |
| `N265_ME_SATD` | 0 | src/encoder/ctu.c:823 |
| `N265_MODE_HIST` | ? | src/encoder/sao.c:272 |
| `N265_MVD_BITS` | 0 | src/encoder/ctu.c:811 |
| `N265_NO_AMP` | ? | src/encoder/encoder.c:120 |
| `N265_NO_RECT` | ? | src/encoder/encoder.c:119 |
| `N265_NO_SPLIT` | ? | src/encoder/ctu.c:2297 |
| `N265_QP_ROUND1` | 1 | src/encoder/encoder.c:114 |
| `N265_QP_TRACE` | ? | src/encoder/encoder.c:519 |
| `N265_SC_TRACE` | ? | src/encoder/la.c:242 |

## arm (13)

| knob | default | reader |
|---|---|---|
| `N265_AQ` | ? | src/encoder/encoder.c:129 |
| `N265_AQ_FLAT` | ? | src/encoder/encoder.c:460 |
| `N265_BADAPT_THR` | ? | src/encoder/encoder.c:714 |
| `N265_FULLPEL` | ? | src/encoder/mv.c:462 |
| `N265_LAMBDA` | ? | src/encoder/ctu.c:375 |
| `N265_NO_AMVP` | ? | src/encoder/ctu.c:1882 |
| `N265_NO_EARLYSKIP` | ? | src/encoder/ctu.c:2320 |
| `N265_NO_GATES` | ? | src/encoder/ctu.c:1738 |
| `N265_NO_SAO` | ? | src/encoder/encoder.c:124 |
| `N265_NO_TMVP` | ? | src/encoder/encoder.c:299 |
| `N265_OLD_BS` | ? | src/encoder/deblock.c:151 |
| `N265_PB_STEP` | 0 | src/encoder/encoder.c:425 |
| `N265_PSY` | 0.0 | src/encoder/encoder.c:128 |
