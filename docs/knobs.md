# The N265_* knob census (generated -- edit scripts/knob_census.py OVERRIDES, not this file)

Regenerate: `python3 scripts/knob_census.py`. CI check: `--check`.

Tier meanings: **default** = shipped behaviour, the env is its escape; **instrument** = measurement machinery (catalogued in docs/instruments.md when it has produced a result); **arm** = default-off gate or kept refuted arm -- kept deliberately because a refuted arm retains its re-pricing value.

## default (7)

| knob | default | reader |
|---|---|---|
| `N265_ABR_SB` | 1.7 | src/encoder/rc.c:126 |
| `N265_ABR_SBR` | 4.0 | src/encoder/rc.c:125 |
| `N265_ABR_SP` | 13.3 | src/encoder/rc.c:124 |
| `N265_GATE_SCALE` | 100 | src/encoder/ctu.c:482 |
| `N265_RDOQ` | 1 | src/encoder/encoder.c:141 |
| `N265_RDOQ_UP` | 1 | src/encoder/rdoq.c:206 |
| `N265_SKIP_THR` | 2 | src/encoder/ctu.c:413 |

## instrument (75)

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
| `N265_AQ_ANCHOR` | ? | src/encoder/encoder.c:625 |
| `N265_AQ_CLAMP` | ? | src/encoder/encoder.c:647 |
| `N265_AQ_DC` | ? | src/encoder/encoder.c:625 |
| `N265_AQ_FLOOR` | 4 | src/encoder/encoder.c:593 |
| `N265_AQ_STR` | 1 | src/encoder/encoder.c:126 |
| `N265_AQ_TRACE` | ? | src/encoder/encoder.c:648 |
| `N265_BADAPT_REFW` | ? | src/encoder/encoder.c:980 |
| `N265_BADAPT_THR` | ? | src/encoder/encoder.c:979 |
| `N265_BADAPT_TRACE` | ? | src/encoder/la.c:424 |
| `N265_BI_GATE` | 4 | src/encoder/ctu.c:1278 |
| `N265_BI_REFINE` | 1 | src/encoder/ctu.c:1230 |
| `N265_B_INTRA_MARGIN` | 30 | src/encoder/ctu.c:1214 |
| `N265_CB64_AMVP` | 1 | src/encoder/ctu.c:1199 |
| `N265_CB64_GATE` | 1 | src/encoder/ctu.c:1185 |
| `N265_CB64_TRACE` | ? | src/encoder/ctu.c:3118 |
| `N265_CBF_FAST` | 3 | src/encoder/ctu.c:466 |
| `N265_CRF_SHIFT` | 5.4 | src/encoder/encoder.c:511 |
| `N265_CT_CENTRE` | 0 | src/encoder/la.c:620 |
| `N265_CT_TRACE` | ? | src/encoder/la.c:657 |
| `N265_CUTREE_STR` | ? | src/encoder/encoder.c:121 |
| `N265_CU_STATS` | ? | src/encoder/encoder.c:138 |
| `N265_DBF_BETA` | 0 | src/encoder/encoder.c:152 |
| `N265_DBF_TC` | 0 | src/encoder/encoder.c:153 |
| `N265_DECIMATE` | 15 | src/encoder/rdoq.c:487 |
| `N265_DEPTH_BOUND` | 0 | src/encoder/ctu.c:664 |
| `N265_ES_THR` | 100 | src/encoder/ctu.c:431 |
| `N265_HFCACHE` | 1 | src/encoder/encoder.c:381 |
| `N265_INTRA_GATE` | 5 | src/encoder/ctu.c:522 |
| `N265_INTRA_MPM` | 0 | src/encoder/ctu.c:1688 |
| `N265_INTRA_RD_N` | 3 | src/encoder/ctu.c:1677 |
| `N265_I_INTRA` | 0 | src/encoder/ctu.c:1704 |
| `N265_MERGE_RD_N` | ? | src/encoder/ctu.c:1249 |
| `N265_MERGE_TRACE` | -1 | src/encoder/mv.c:246 |
| `N265_ME_PATTERN` | 0 | src/encoder/mv.c:456 |
| `N265_ME_SATD` | 0 | src/encoder/ctu.c:1136 |
| `N265_MIN_KEYINT` | 4 | src/encoder/encoder.c:906 |
| `N265_MODE_HIST` | ? | src/encoder/sao.c:602 |
| `N265_MVD_BITS` | 0 | src/encoder/ctu.c:1124 |
| `N265_NO_AMP` | ? | src/encoder/encoder.c:133 |
| `N265_NO_RECT` | ? | src/encoder/encoder.c:132 |
| `N265_NO_SPLIT` | ? | src/encoder/ctu.c:3041 |
| `N265_PART_GATE` | 0 | src/encoder/ctu.c:577 |
| `N265_PSY` | 0.0 | src/encoder/encoder.c:142 |
| `N265_QP_FRAC` | = --bitrate | src/encoder/encoder.c:115 |
| `N265_QP_ROUND1` | 1 | src/encoder/encoder.c:127 |
| `N265_QP_TRACE` | ? | src/encoder/encoder.c:700 |
| `N265_RC_TRACE` | ? | src/encoder/rc.c:133 |
| `N265_RDOQ_ADAPT` | 0 | src/encoder/rdoq.c:217 |
| `N265_RDOQ_LAM` | 100 | src/encoder/rdoq.c:244 |
| `N265_RECT_RANK` | 0 | src/encoder/ctu.c:607 |
| `N265_RSKIP` | 0 | src/encoder/ctu.c:449 |
| `N265_SAO_BIAS` | 100 | src/encoder/sao.c:185 |
| `N265_SAO_CHROMA` | 1 | src/encoder/sao.c:330 |
| `N265_SAO_GATE` | 2 | src/encoder/sao.c:362 |
| `N265_SAO_QPLAM` | 0 | src/encoder/sao.c:175 |
| `N265_SAO_TRACE` | ? | src/encoder/sao.c:447 |
| `N265_SAO_TYPES` | 0 | src/encoder/sao.c:153 |
| `N265_SBH_RD` | 2 | src/encoder/rdoq.c:190 |
| `N265_SC_HOLD` | 1 | src/encoder/la.c:259 |
| `N265_SC_TRACE` | ? | src/encoder/la.c:280 |
| `N265_SKIP_EXIT` | 0 | src/encoder/ctu.c:499 |
| `N265_SUBME` | 0 | src/encoder/mv.c:444 |
| `N265_SUBPEL_DEDUP` | 1 | src/encoder/mv.c:592 |

## arm (12)

| knob | default | reader |
|---|---|---|
| `N265_AQ` | ? | src/encoder/encoder.c:143 |
| `N265_AQ_FLAT` | ? | src/encoder/encoder.c:560 |
| `N265_BADAPT_DEG` | ? | src/encoder/encoder.c:997 |
| `N265_FULLPEL` | ? | src/encoder/mv.c:589 |
| `N265_LAMBDA` | ? | src/encoder/ctu.c:396 |
| `N265_NO_AMVP` | ? | src/encoder/ctu.c:2473 |
| `N265_NO_EARLYSKIP` | ? | src/encoder/ctu.c:3064 |
| `N265_NO_GATES` | ? | src/encoder/ctu.c:2284 |
| `N265_NO_SAO` | ? | src/encoder/encoder.c:137 |
| `N265_NO_TMVP` | ? | src/encoder/encoder.c:341 |
| `N265_OLD_BS` | ? | src/encoder/deblock.c:151 |
| `N265_PB_STEP` | 0 | src/encoder/encoder.c:518 |
