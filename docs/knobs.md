# The N265_* knob census (generated -- edit scripts/knob_census.py OVERRIDES, not this file)

Regenerate: `python3 scripts/knob_census.py`. CI check: `--check`.

Tier meanings: **default** = shipped behaviour, the env is its escape; **instrument** = measurement machinery (catalogued in docs/instruments.md when it has produced a result); **arm** = default-off gate or kept refuted arm -- kept deliberately because a refuted arm retains its re-pricing value.

## default (17)

| knob | default | reader |
|---|---|---|
| `N265_ABR_FBMAX` | 6.0 | src/encoder/rc.c:112 |
| `N265_ABR_HORIZON` | 6.0 | src/encoder/rc.c:104 |
| `N265_ABR_IGAIN` | 0.085 | src/encoder/rc.c:110 |
| `N265_ABR_IMAX` | 8.0 | src/encoder/rc.c:111 |
| `N265_ABR_LVL` | 2.0 | src/encoder/rc.c:126 |
| `N265_ABR_OPENSTEP` | 6.0 | src/encoder/rc.c:109 |
| `N265_ABR_QCOMP` | 0.6 | src/encoder/rc.c:103 |
| `N265_ABR_SB` | 1.7 | src/encoder/rc.c:125 |
| `N265_ABR_SBR` | 4.0 | src/encoder/rc.c:124 |
| `N265_ABR_SI` | 25.4 | src/encoder/rc.c:122 |
| `N265_ABR_SMEM` | 16.0 | src/encoder/rc.c:108 |
| `N265_ABR_SP` | 13.3 | src/encoder/rc.c:123 |
| `N265_ABR_STEP` | 1.0 | src/encoder/rc.c:107 |
| `N265_ABR_WMEM` | 120.0 | src/encoder/rc.c:113 |
| `N265_RDOQ` | 1 | src/encoder/encoder.c:139 |
| `N265_RDOQ_UP` | 1 | src/encoder/rdoq.c:206 |
| `N265_SKIP_THR` | 2 | src/encoder/ctu.c:397 |

## instrument (33)

| knob | default | reader |
|---|---|---|
| `N265_AQ_ANCHOR` | ? | src/encoder/encoder.c:520 |
| `N265_AQ_CLAMP` | ? | src/encoder/encoder.c:542 |
| `N265_AQ_DC` | ? | src/encoder/encoder.c:520 |
| `N265_AQ_STR` | 1 | src/encoder/encoder.c:125 |
| `N265_BADAPT_REFW` | ? | src/encoder/encoder.c:790 |
| `N265_BADAPT_THR` | ? | src/encoder/encoder.c:789 |
| `N265_BADAPT_TRACE` | ? | src/encoder/la.c:406 |
| `N265_BI_REFINE` | 1 | src/encoder/ctu.c:872 |
| `N265_B_INTRA_MARGIN` | 30 | src/encoder/ctu.c:856 |
| `N265_CB64_AMVP` | 1 | src/encoder/ctu.c:841 |
| `N265_CB64_TRACE` | ? | src/encoder/ctu.c:2421 |
| `N265_CRF_SHIFT` | 5.4 | src/encoder/encoder.c:436 |
| `N265_CT_CENTRE` | 0 | src/encoder/la.c:602 |
| `N265_CT_TRACE` | ? | src/encoder/la.c:639 |
| `N265_CUTREE_STR` | ? | src/encoder/encoder.c:120 |
| `N265_DECIMATE` | 15 | src/encoder/rdoq.c:487 |
| `N265_INTRA_MPM` | 0 | src/encoder/ctu.c:1264 |
| `N265_INTRA_RD_N` | 3 | src/encoder/ctu.c:1253 |
| `N265_MERGE_TRACE` | ? | src/encoder/mv.c:239 |
| `N265_ME_SATD` | 0 | src/encoder/ctu.c:828 |
| `N265_MODE_HIST` | ? | src/encoder/sao.c:272 |
| `N265_MVD_BITS` | 0 | src/encoder/ctu.c:816 |
| `N265_NO_AMP` | ? | src/encoder/encoder.c:132 |
| `N265_NO_RECT` | ? | src/encoder/encoder.c:131 |
| `N265_NO_SPLIT` | ? | src/encoder/ctu.c:2358 |
| `N265_PSY` | 0.0 | src/encoder/encoder.c:140 |
| `N265_QP_ROUND1` | 1 | src/encoder/encoder.c:126 |
| `N265_QP_TRACE` | ? | src/encoder/encoder.c:578 |
| `N265_RC_TRACE` | ? | src/encoder/rc.c:132 |
| `N265_RDOQ_ADAPT` | 0 | src/encoder/rdoq.c:217 |
| `N265_RDOQ_LAM` | 100 | src/encoder/rdoq.c:244 |
| `N265_SBH_RD` | 2 | src/encoder/rdoq.c:190 |
| `N265_SC_TRACE` | ? | src/encoder/la.c:262 |

## arm (13)

| knob | default | reader |
|---|---|---|
| `N265_AQ` | ? | src/encoder/encoder.c:141 |
| `N265_AQ_FLAT` | ? | src/encoder/encoder.c:485 |
| `N265_BADAPT_DEG` | ? | src/encoder/encoder.c:807 |
| `N265_FULLPEL` | ? | src/encoder/mv.c:462 |
| `N265_LAMBDA` | ? | src/encoder/ctu.c:380 |
| `N265_NO_AMVP` | ? | src/encoder/ctu.c:1943 |
| `N265_NO_EARLYSKIP` | ? | src/encoder/ctu.c:2381 |
| `N265_NO_GATES` | ? | src/encoder/ctu.c:1799 |
| `N265_NO_SAO` | ? | src/encoder/encoder.c:136 |
| `N265_NO_TMVP` | ? | src/encoder/encoder.c:312 |
| `N265_OLD_BS` | ? | src/encoder/deblock.c:151 |
| `N265_PB_STEP` | 0 | src/encoder/encoder.c:443 |
| `N265_QP_FRAC` | = --bitrate | src/encoder/encoder.c:114 |
