
# yah265

**An H.265/HEVC encoder written from scratch, by AI, to find out how far
AI-driven development can get against x265.**

yah265 is a sibling of [yah264](../yah264), which met its pure-C speed-parity
goals against x264. The rules are the same here for a new codec. The encoder
is clean-room from the ITU-T H.265 spec. It's licensed GPL-2.0-or-later, with
a commercial license beside it in the same arrangement as x264 and x265. See
LICENSE, and ask terranvigil@gmail.com for commercial terms. Every stream is
verified against independent decoders. Every performance claim is
reproducible from a Makefile target.

There are three goals. Each is measured as a multiple of x265's speed at a
matched operating point. That point holds quality within 0.5 dVMAF, size
within 1.0% and PSNR-Y within 1.0 dB.

1. Pure C, single-threaded, both encoders no-asm: faster than x265.
2. Pure C, multi-threaded: faster than x265.
3. As shipped (SIMD on), multi-threaded: faster than x265.

## Results (2026-09-15, brew x265 4.3, 15 clips, `--preset medium` both sides)

Each goal is read on five legs at one pinned operating point:

- median speed at or under x265's wall
- the worst clip under 1.15x of it, and named
- quality within 0.5 VMAF-NEG at equal bytes
- size within 1.0% at equal quality
- PSNR-Y within 1.0 dB at equal bytes

The [goal-legs script](scripts/goal_legs.py) reads them and appends them to
the [goal-legs table](docs/data/goal-legs.csv).

| goal | tier | median speed | worst clip | quality | size | PSNR-Y | result | row |
|---|---|--:|---|---|---|---|---|---|
| 1 | pure C, 1 thread | 0.51x | sita_toon_720p 0.77x | 15/15 ahead | 15/15 smaller | 0/15 below the bar | **met** | `ed553d9` |
| 2 | pure C, all cores | 0.36x | park_joy_720p 0.49x | 15/15 ahead | 15/15 smaller | 0/15 below the bar | **met** | `e50312c` |
| 3 | as shipped, all cores | 0.68x | touchdown_1080p 0.87x | 15/15 ahead | 15/15 smaller | 0/15 below the bar | **met** | `e50312c` |

The quality, size and PSNR-Y columns are properties of the bitstream and not
of the clock. One reading covers all three tiers because the three goals
solve the same operating point and differ only in how it's timed.

The PSNR-Y leg was added on 2026-09-14 as a floor. It isn't a decision
metric. The band still decides on VMAF-NEG. The leg is there so a
VMAF-NEG gain can never be bought by letting measurable pixel accuracy
slide. At the pinned point the median clip is 0.08 dB under x265. The two
worst are sita_flat_720p at -0.87 dB and bus_cif at -0.72 dB. Both are inside
the 1.0 dB bar. They're recorded as debt against a tighter 0.5 dB line that
gates nothing. Those two clips are also where the two metrics disagree most.
Item B4 of the [phase 13 notes](docs/phase13.md) measured six metrics on the
same rungs and says why.

The row column names the `commit` the legs were read at in the goal-legs
table. That table is the only place these numbers come
from. Each goal shows its latest 15-clip block there at `--preset medium`
with no arm.

On the band, `medium` reads a CRF BD-VMAF-NEG of **-7.81%** against x265
medium. All fifteen clips are ahead. The closest is `stefan_cif` at -0.49%.
The reading comes from board row `32b24e5` of the
[parity history](docs/data/parity-history.csv) and matches the phase 12
exit's. `slow` and `slower` keep the larger leads at their own cost,
**-13.46%** and **-21.84%**. Those are CIF-7 CRF band medians against the same
reference. They come from a different population than the fifteen above.
They're in section 12.3 of the
[phase 9 goals plan](docs/phase9-goals-plan.md). Neither table has moved
since.

**The one clip that was slower, and what closed it.** Until 2026-09-15,
touchdown_1080p sat at the worst-clip bar. It's a football sequence with a
crowd in the stands. Across four goal-3 reads of the shipped `medium` it read
1.11-1.16x against a bar of 1.15x.

It was never a wasted-work problem. Both encoders mark the same share of the
picture as unchanged (63% ours, 60% x265). A perfect early-skip oracle would
have bought under 4% of its wall. Two clip-specific items went at the skip
side and found nothing. What sets the clip apart is thousands of small 8x8
blocks in the crowd texture that each carry a little real change. On those we
run a rate-distortion optimized quantizer (RDOQ) that x265 medium doesn't run
at all. That step is about a quarter of the clip's single-thread time. It's
also part of why our pictures are better there, 12 BD points ahead of x265 on
that clip at the default.

The leg didn't close by trading quality. The phase 13 utilization census
found a scheduling defect. The thread pool woke every waiter on 91% of
per-block publishes. That cost 4 s of system time per 120 frames of 1080p at
18 threads. Keying the wakes is bit-exact and is item C4 of the phase 13 notes. It took
every clip's all-core wall down 5 to 16%. Touchdown went from 1.14x to 0.87x.
The record of the clip's analysis is in sections 11 and 12 of the phase 9
goals plan and in the [phase 9 lattice notes](docs/phase9-lattice.md).

**...and how to trade it yourself.** `--rdoq-level` hands that quantizer to
the user. It uses x265's numbering so a setting means the same on both.
Level 0 turns RDOQ off. Level 1 is what `medium` runs: the lattice once per
block, at the commit. Level 2 runs it in every rate-distortion decision. That
is what `slow` and `slower` do. When the flag is given, it overrides the
preset. This is the reading on the fifteen
clips at `--preset medium`, all cores, as shipped, against x265 medium:

| arm | median speed | touchdown_1080p | CRF BD-VMAF-NEG median | worst clip |
|---|--:|--:|--:|---|
| default (`--rdoq-level 1`) | 0.74x | 1.14x | -7.8% | stefan_cif -0.5% |
| `--rdoq-level 0` | **0.60x** | **0.99x** | +10.2% | sita_flat_720p +23.0% |
| `--rdoq-level 2` | 0.81x | 1.25x | **-11.9%** | sita_toon_720p -3.2% |

Level 0 is the one arm measured anywhere that puts touchdown_1080p under
x265's own wall. It isn't free, though. The median clip goes from 7.8% ahead
of the reference to 10.2% behind it, and only two of the fifteen stay ahead.
Level 2 goes the other way. It buys four more BD points of median for a tenth
more time, which is the trade `--preset slow` makes by name. The per-clip
tables, the single-thread clock and the across-shot numbers are under
`--rdoq-level` in the [instruments notes](docs/instruments.md).

**Chroma has its own two knobs, and its own reading.** `--cbqpoffs` and
`--crqpoffs` are x265's chroma QP offsets, with x265's names, range and
default. A negative value spends more on chroma. A positive one moves those
bits to luma. They default to 0 and stay there on measurement. At the pinned
operating point our chroma-to-luma distortion split is within 12% of x265's
own on all fifteen clips at three rungs. Because that's under half a QP
step, a default has nothing to correct. What an offset buys is still worth knowing.
`--cbqpoffs -1 --crqpoffs -1` is about ten BD points of chroma on every CIF
clip for 1.3 points of luma. The goal legs now print the chroma level per
plane at equal bytes beside the four luma legs. That's what makes either
number checkable. The phase 13 notes have the detail.

**The preset names are x265's, and the ones we don't have say so.**
`--preset` knows all ten names x265 prints. It accepts the four we have
placed: `fast`, `medium` (default), `slow` and `slower`. The other six are
refused by name and point at the nearest rung we do have, instead of failing
as an unknown option. Four of the six were built and measured against the x265
preset of their own name. All four were refused.

Below `fast`, `faster` is the SA8D-proxy mode decision plus the frozen bit
estimator. It wins the CRF band on all fifteen clips by a median of 10% and
still loses three of the four legs. touchdown_1080p runs at 1.44x of x265
faster's wall, and bus_cif misses the quality and size bars by a hair.
`veryfast` loses the band itself on seven of the fifteen, as the phase 13
notes record.

Above `slower`, `veryslow` and `placebo` lose the band on five and four of
the seven CIF clips. The reason is upstream of them.

`--preset slow` reads
+10.24% against x265's own `slow`, and `slower` reads +1.18% against x265's
`slower`. Meanwhile `medium` reads -6.65% against x265 medium with every clip
ahead. Our ladder above medium doesn't yet earn its own names. A rung built
on top of it inherits that before it spends a lever. That's item 5 of the
[phase 14 notes](docs/phase14.md). None of the four is named in
`--preset`.

**Two flags came out of that item whatever the rungs did.**
`--tu-inter-depth` and `--tu-intra-depth` make the residual quadtree's depth
a user option. They take x265's names and numbering, 1..4 with a default of 2.
The third level is worth half a point of CIF band median and one and a half
points at 720p, for 8-11% of the single-thread wall. `--ref` now reaches 4
and 5. Those measured as 1.4-1.9x of the wall for no band. They're there for
a user who wants them and aren't on a rung.

The full campaign record is the [plan](docs/plan.md). It covers phases
0-13. Phase 14's backlog is the
[x265 feature audit](docs/x265-feature-audit.md). The per-phase docs are under
`docs/`. The refusal log is [ideas](docs/ideas.md).
