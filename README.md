
# yah265

**An H.265/HEVC encoder written from scratch, by AI, to find out how far
AI-driven development can get against x265.**

Sibling of [yah264](../yah264), which met its pure-C speed-parity goals
against x264. Same rules here, new codec: clean-room from the ITU-T H.265
spec, GPL-2.0-or-later, every stream verified against independent decoders, every
performance claim reproducible from a Makefile target.

Three goals, measured as x265-speed multiples at a matched operating point
(quality within 0.5 dVMAF, size within 1.0%):

1. Pure C, single-threaded, both encoders no-asm: faster than x265.
2. Pure C, multi-threaded: faster than x265.
3. As shipped (SIMD on), multi-threaded: faster than x265.

## Results (2026-09-14, brew x265 4.3, 15 clips, `--preset medium` both sides)

Each goal is read on four legs at one pinned operating point: median
speed at or under x265's wall, the worst clip under 1.15x of it and named,
quality within 0.5 VMAF-NEG at equal bytes, size within 1.0% at equal
quality (`scripts/goal_legs.py`, `docs/data/goal-legs.csv`).

| goal | tier | median speed | worst clip | quality | size | result |
|---|---|--:|---|---|---|---|
| 1 | pure C, 1 thread | 0.47x | sita_toon 0.80x | 15/15 ahead | 15/15 smaller | **met** |
| 2 | pure C, all cores | 0.38x | sita_toon 0.55x | 15/15 ahead | 15/15 smaller | **met** |
| 3 | as shipped, all cores | 0.74x | touchdown_1080p 1.16x | 15/15 ahead | 15/15 smaller | three legs met; the worst-clip leg at the bar |

On the band, `medium` reads a CRF BD-VMAF-NEG of -7.8% against x265 medium
with all fifteen clips ahead; `slow` and `slower` keep the larger leads
(-14 and -21) at their own cost.

**Why one clip is slower.** touchdown_1080p, a football sequence with a
crowd in the stands, is the one clip over the worst-clip bar (1.14-1.18x
across reads, bar 1.15x). It is not a wasted-work problem: both encoders
mark the same share of the picture as unchanged (63% ours, 60% x265), a
perfect early-skip oracle would buy under 4% of its wall, and two
clip-specific items went at the skip side and found nothing left. What is
different about the clip is thousands of small 8x8 blocks that each carry
a little real change (the crowd texture), and on those we run a
rate-distortion optimised quantiser (RDOQ) that x265 medium does not run
at all. That step is about a quarter of the clip's single-thread time and
it is also part of why our pictures are better there (19 BD points ahead
of x265 on that clip). The remaining levers are quality trades, priced
and recorded in `docs/phase9-goals-plan.md` (sections 11 and 12) and
`docs/phase9-lattice.md`; the clip is recorded at the bar rather than
bought with more of the lead.

Full campaign record: `docs/plan.md` (phases 0-12), the per-phase docs
under `docs/`, and the refusal log in `docs/ideas.md`.
