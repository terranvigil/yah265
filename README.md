
# yah265

**An H.265/HEVC encoder written from scratch, by AI, to find out how far
AI-driven development can get against x265.**

Sibling of [yah264](../yah264), which met its pure-C speed-parity goals
against x264. Same rules here, new codec: clean-room from the ITU-T H.265
spec, GPL-2.0-or-later, every stream verified against independent decoders, every
performance claim reproducible from a Makefile target.

Three goals, measured as x265-speed multiples at a matched operating point
(quality within 0.5 dVMAF, size within 1.0%, PSNR-Y within 1.0 dB):

1. Pure C, single-threaded, both encoders no-asm: faster than x265.
2. Pure C, multi-threaded: faster than x265.
3. As shipped (SIMD on), multi-threaded: faster than x265.

## Results (2026-09-14, brew x265 4.3, 15 clips, `--preset medium` both sides)

Each goal is read on five legs at one pinned operating point: median
speed at or under x265's wall, the worst clip under 1.15x of it and named,
quality within 0.5 VMAF-NEG at equal bytes, size within 1.0% at equal
quality, and PSNR-Y within 1.0 dB at equal bytes
(`scripts/goal_legs.py`, `docs/data/goal-legs.csv`).

| goal | tier | median speed | worst clip | quality | size | PSNR-Y | result |
|---|---|--:|---|---|---|---|---|
| 1 | pure C, 1 thread | 0.47x | sita_toon 0.80x | 15/15 ahead | 15/15 smaller | 0/15 below the bar | **met** |
| 2 | pure C, all cores | 0.38x | sita_toon 0.55x | 15/15 ahead | 15/15 smaller | 0/15 below the bar | **met** |
| 3 | as shipped, all cores | 0.74x | touchdown_1080p 1.16x | 15/15 ahead | 15/15 smaller | 0/15 below the bar | four legs met; the worst-clip leg at the bar |

The quality, size and PSNR-Y columns are properties of the bitstream and
not of the clock, so one reading covers all three tiers: the three goals
solve the same operating point and differ only in how it is timed.

**The PSNR-Y leg is a floor, not a decision metric** (added 2026-09-14).
The band still decides on VMAF-NEG; this leg is there so a VMAF-NEG gain
can never be bought by letting measurable pixel accuracy slide. At the
pinned point the median clip is 0.08 dB under x265 and the two worst are
sita_flat_720p at -0.87 dB and bus_cif at -0.72 dB - inside the 1.0 dB
bar, and recorded as debt against a tighter 0.5 dB line that gates
nothing. Those two clips are also where the two metrics disagree most;
`docs/phase13.md` B4 measured six metrics on the same rungs and says why.

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

Full campaign record: `docs/plan.md` (phases 0-12), the per-phase docs
under `docs/`, and the refusal log in `docs/ideas.md`.
