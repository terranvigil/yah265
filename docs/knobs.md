# Env knobs

| knob | default | effect |
|---|---|---|
| N265_MODE_HIST | unset | print the intra mode-pick histogram at exit (coverage-honesty tap) |
- `N265_SC_TRACE`: print per-frame lookahead pcost/icost sums, ratio,
  and the scenecut verdict.
- `N265_CB64_TRACE`: print each committed CB64 whole-CU skip (poc, ctu,
  merge idx, motion).
