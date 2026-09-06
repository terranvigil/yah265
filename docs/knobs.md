# The N265_* knob census (generated -- edit scripts/knob_census.py OVERRIDES, not this file)

Regenerate: `python3 scripts/knob_census.py`. CI check: `--check`.

Tier meanings: **default** = shipped behaviour, the env is its escape; **instrument** = measurement machinery (catalogued in docs/instruments.md when it has produced a result); **arm** = default-off gate or kept refuted arm -- kept deliberately because a refuted arm retains its re-pricing value.

## default (1)

| knob | default | reader |
|---|---|---|
| `N265_SKIP_THR` | 2 | src/encoder/ctu.c:339 |

## instrument (5)

| knob | default | reader |
|---|---|---|
| `N265_CB64_TRACE` | ? | src/encoder/ctu.c:1572 |
| `N265_CT_TRACE` | ? | src/encoder/la.c:364 |
| `N265_NO_SPLIT` | ? | src/encoder/ctu.c:1509 |
| `N265_QP_TRACE` | ? | src/encoder/entropy.c:127 |
| `N265_SC_TRACE` | ? | src/encoder/la.c:237 |

## arm (9)

| knob | default | reader |
|---|---|---|
| `N265_AQ` | ? | src/encoder/encoder.c:101 |
| `N265_AQ_FLAT` | ? | src/encoder/encoder.c:371 |
| `N265_FULLPEL` | ? | src/encoder/mv.c:400 |
| `N265_LAMBDA` | ? | src/encoder/ctu.c:322 |
| `N265_MODE_HIST` | ? | src/encoder/sao.c:272 |
| `N265_NO_AMVP` | ? | src/encoder/ctu.c:1122 |
| `N265_NO_EARLYSKIP` | ? | src/encoder/ctu.c:1532 |
| `N265_NO_GATES` | ? | src/encoder/ctu.c:1025 |
| `N265_NO_SAO` | ? | src/encoder/encoder.c:100 |
