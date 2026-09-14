
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

| goal | tier | median speed | worst clip | quality | size | result | row |
|---|---|--:|---|---|---|---|---|
| 1 | pure C, 1 thread | 0.51x | sita_toon_720p 0.77x | 15/15 ahead | 15/15 smaller | **met** | `ed553d9` |
| 2 | pure C, all cores | 0.38x | touchdown_1080p 0.55x | 15/15 ahead | 15/15 smaller | **met** | `6acae22` |
| 3 | as shipped, all cores | 0.74x | touchdown_1080p 1.14x | 15/15 ahead | 15/15 smaller | three legs met; the worst-clip leg at the bar | `ed553d9` |

The row column names the `commit` the legs were read at in
`docs/data/goal-legs.csv`, which is the only place these numbers come
from; each goal shows its latest 15-clip block there at `--preset medium`
with no arm.

On the band, `medium` reads a CRF BD-VMAF-NEG of **-7.81%** against x265
medium with all fifteen clips ahead and the closest clip `stefan_cif` at
-0.49% (board row `32b24e5`, `docs/data/parity-history.csv`; the same
number is the phase 12 exit's). `slow` and `slower` keep the larger leads
at their own cost: **-13.46%** and **-21.84%**, which are CIF-7 CRF band
medians against the same reference and so a different population from the
fifteen above (docs/phase9-goals-plan.md sec 12.3; neither table has
moved since).

**Why one clip is slower.** touchdown_1080p, a football sequence with a
crowd in the stands, is the one clip at the worst-clip bar (1.11-1.16x
across the four goal-3 reads of the shipped `medium`, bar 1.15x; rows
`8ac6bba`, `b2708d1`, `6acae22` and `ed553d9`). It is not a wasted-work problem: both encoders
mark the same share of the picture as unchanged (63% ours, 60% x265), a
perfect early-skip oracle would buy under 4% of its wall, and two
clip-specific items went at the skip side and found nothing left. What is
different about the clip is thousands of small 8x8 blocks that each carry
a little real change (the crowd texture), and on those we run a
rate-distortion optimised quantiser (RDOQ) that x265 medium does not run
at all. That step is about a quarter of the clip's single-thread time and
it is also part of why our pictures are better there (12 BD points ahead
of x265 on that clip at the shipped default, 15 with the quantiser turned
up). The remaining levers are quality trades, priced and recorded in
`docs/phase9-goals-plan.md` (sections 11 and 12) and
`docs/phase9-lattice.md`; the clip is recorded at the bar rather than
bought with more of the lead.

**...and how to trade it yourself.** `--rdoq-level` hands that quantiser
to the user, in x265's numbering so a number carries over: **0** turns
RDOQ off, **1** is what `medium` runs (the lattice once per block, at the
commit), **2** runs it in every rate-distortion decision, which is what
`slow` and `slower` do. Given, it overrides the preset. On the fifteen
clips at `--preset medium`, all cores, as shipped, against x265 medium:

| arm | median speed | touchdown_1080p | CRF BD-VMAF-NEG median | worst clip |
|---|--:|--:|--:|---|
| default (`--rdoq-level 1`) | 0.74x | 1.14x | -7.8% | stefan_cif -0.5% |
| `--rdoq-level 0` | **0.60x** | **0.99x** | +10.2% | sita_flat_720p +23.0% |
| `--rdoq-level 2` | 0.81x | 1.25x | **-11.9%** | sita_toon_720p -3.2% |

Level 0 is the one arm measured anywhere that puts touchdown_1080p under
x265's own wall, and it is not free: the median clip goes from 7.8% ahead
of the reference to 10.2% behind it, and only two of the fifteen stay
ahead. Level 2 is the other direction - four more BD points of median for
a tenth more time, the trade `--preset slow` makes by name. The
per-clip tables, the single-thread clock and the across-shot numbers are
in `docs/instruments.md` under `--rdoq-level`.

**Chroma has its own two knobs, and its own reading.** `--cbqpoffs` and
`--crqpoffs` are x265's chroma QP offsets by their own names, range and
default: negative spends more on chroma, positive moves those bits to
luma. They default to 0 and stay there on measurement - at the pinned
operating point our chroma-to-luma distortion split is within 12% of
x265's own on all fifteen clips at three rungs, under half a QP step, so
there is nothing for a default to correct. What one BUYS is still worth
knowing: `--cbqpoffs -1 --crqpoffs -1` is about ten BD points of chroma
on every CIF clip for 1.3 points of luma. The goal legs now print the
chroma level per plane at equal bytes beside the four luma legs, which is
what makes either number checkable; `docs/phase13.md`.

**The preset names are x265's, and the ones we do not have say so.**
`--preset` knows all ten names x265 prints and accepts the four we have
placed - `fast`, `medium` (default), `slow`, `slower`. The other six are
refused by name, pointing at the nearest rung we do have, rather than as
an unknown option. Two rungs below `fast` were built and measured against
the x265 preset of their own name and refused: `faster` (the SA8D-proxy
mode decision plus the frozen bit estimator) wins the CRF band on all
fifteen clips by a median of 10% and still loses three of the four legs -
touchdown_1080p at 1.44x of x265 faster's wall, and bus_cif missing the
quality and size bars by a hair - and `veryfast` loses the band itself on
seven of the fifteen. Both are recorded with the binding leg in
`docs/phase13.md`; neither is named in `--preset`.

Full campaign record: `docs/plan.md` (phases 0-13; phase 14's backlog is
`docs/x265-feature-audit.md`), the per-phase docs under `docs/`, and the
refusal log in `docs/ideas.md`.
