# next265

**An H.265/HEVC encoder written from scratch, by AI, to find out how far
AI-driven development can get against x265.**

Sibling of [next264](../next264), which met its pure-C speed-parity goals
against x264. Same rules here, new codec: clean-room from the ITU-T H.265
spec, GPL-2.0-or-later, every stream verified against independent decoders, every
performance claim reproducible from a Makefile target.

Three goals, measured as x265-speed multiples at a matched operating point
(quality within 0.5 dVMAF, size within 1.0%):

1. Pure C, single-threaded, both encoders no-asm: faster than x265.
2. Pure C, multi-threaded: faster than x265.
3. As shipped (SIMD on), multi-threaded: faster than x265.

Status: research and planning. See `docs/plan.md`.
