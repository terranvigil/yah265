# Media traits

`scripts/media_traits.py` measures what each test title is before anything
is encoded. It writes one row per title to `docs/data/media-traits.csv`.
The CSV is the record. This page says what each column means, what each
flag can mislead, and the rule a reading follows when its cell is flagged.

## Why

A cell's number can move because of one quirk of the clip, and not because
of the content an item is about. Meridian 720p opens on 60 black frames.
Item 29 step 2's flat rule gave up an accident those frames caused in the
rate control. The cell read 1.79% worse, and none of that loss was about
the 89 seconds of picture that follow. A trait like that should be known
before the reading, not found after it.

## The titles

| tier | where | measured at |
|---|---|---|
| segment | `~/media/throughput` (the matrix's nine cells) | each cell's own size |
| probe | `~/media/probe` windows | sliced out of their segment's series (they're byte cuts of it) |
| tier S | `<media>/tier_s` (the shared twelve) | each clip's own size |
| long-form | the 600 s excerpts in `vbr_lf.py`'s table (docs/parity-plan.md) | 1080p geometry, through the long-form feeder |
| corpus | `tests/corpus` (the seventeen) | each clip's own size |

A long-form excerpt is read at 1080p only. Its lead and trail runs, cuts,
fades and luma range hold at every size. Its flat share and noise level are
1080p numbers.

## How it's measured

One pass per source reads every frame's luma once. It keeps a per-frame
series, cached under `~/.cache/yah265-traits` and keyed on the source's
path, size and mtime. The CSV is a reduction of the series, so a threshold
change costs seconds. Most traits are read on a box-averaged copy about 320
samples wide. The flat type is read at full size, because it's the
encoder's own definition. The noise estimate is read at full size too, on
at most 240 evenly spaced frames.

The flat type is the encoder's content gate (`N265_FLAT_GATE`, la.c),
reproduced bit for bit. `media_traits.py check-flat` compares it with
`N265_FLAT_TRACE` frame by frame. It matched on every frame of foreman_cif
(300), sita_toon_720p, fade_cif and 120 frames of Meridian 720p.

## Columns

| column | meaning |
|---|---|
| `w`, `h`, `fps`, `frames`, `seconds` | the source as read |
| `lead_black`, `trail_black` | frames from the start (end) that are black: luma p99 at or under 40 and mean at or under 24 (8-bit codes) |
| `lead_still`, `trail_still` | frames from the start (end) with no change: mean absolute luma difference to the previous frame under 0.1 on the small copy (a repeated or frozen picture; a locked-off live shot reads 0.12-0.3). A black run is also a still run |
| `lead_skip` | the first content frame: the longer of the two leading runs |
| `black_share` | share of all frames that are black |
| `fades`, `fade_frames` | fades to or from black: a mean-luma ramp of at least 6 frames, monotone by more than 0.5 a frame, into or out of a near-black frame (p99 at or under 48). Motion doesn't break it |
| `xfades`, `xfade_frames` | dissolves: windows of 8 frames whose ends differ by at least 6 codes on a copy about 40 samples wide, where every inner frame sits within 15% of that change from the straight blend of the ends and no single step is over 35% of it. A run counts from 12 frames. Frames already in a fade don't count |
| `cuts`, `cuts_per_min` | scene cuts: the scene score over 0.3, the enclab scene scan's threshold. The score is min(mafd, \|mafd - previous mafd\|) / 100 on the small copy's luma |
| `flat_share` | share of frames the encoder types flat (flat-block share at or over 10%) |
| `flat_share_nonblack` | the same over non-black frames |
| `lead_flat` | flat-typed frames from the start |
| `y_p01`, `y_p50`, `y_p99` | the median over non-black frames of each frame's 1st, 50th and 99th luma percentile |
| `dark_share` | share of non-black frames whose p99 is at or under 128 |
| `noise_sigma` | Immerkaer's noise estimator per 16x16 block, the 20th percentile over blocks off the clip rails (mean 25-229), median over the sampled non-black frames, in 8-bit codes |
| `flags` | the traits below, with their values |

## Flags, and what each can mislead

| flag | fires when | readings it can mislead |
|---|---|---|
| `lead_black:N` / `lead_still:N` | a leading run of at least 0.25 s and 6 frames (still counts only past the black) | Any rate control that learns from what it has coded: ABR and VBR operating points, the VBV's predictors, a CRF reading's first window. A first-second or first-window metric. Speed on a short clip, because black codes in almost no time |
| `trail_black:N` / `trail_still:N` | the same at the end | ABR's final rate error. The last window's metric |
| `fades:F+Xx/P%` | fades and dissolves cover at least 5% of frames | Weighted prediction, scene-cut placement, the lookahead's costs, cu-tree. Each reads differently inside a transition than on content |
| `cuts:N/min` | at least 20 cuts a minute | Key-frame placement, the lookahead and cu-tree depth, open GOP. A short-horizon rate control |
| `flat_mix:P%` | between 2% and 98% of non-black frames are flat-typed | Every tool that branches on the flat type: the AQ clamp, the CRF base dithering, the cu-tree frame weight, SAO's content gate, the rect gate. A change to one branch moves only the frames it covers, so the clip's total mixes two encoders |
| `dark:p99=N` | the median frame's p99 is at or under 150 | VMAF and PSNR on dark content, AQ (variance is small everywhere), and the flat gate (dark detail reads as flat) |
| `narrow:LO-HI` | the median frame's p99 minus its p01 is at or under 150 codes | The same tools as `dark`. PQ masters read as 8-bit with no tone map land here (Meridian 22-171, Chimera 52-198), and so do some natural clips (ducks, riverbed) |
| `noise:S` | sigma at or over 2.0 | Rate at any fixed quality (grain costs bits), and VMAF-NEG against PSNR, which disagree on grain |

The noise column is the weakest. It reads fine texture as noise where a
clip has no smooth area (mobile_cif 3.3, crowd_run 1.9). It also misses
the two grain titles at 1080p: Nocturne reads 1.56 and Lash of the Whip
0.80, both under the flag. Read the number, not the flag, and don't gate
on it.

A flag is a caution, not a verdict. Thresholds sit at the top of the
script; change one there and here together.

## The rule

A cell with a flagged trait is read two ways where the trait can steer the
reading. For a leading black or still run, the second reading starts at
`lead_skip` and runs to the end of the segment, at the cell's own rates.
`scripts/vbr_lf.py segc` does it, and `seg` runs it after the full reading.

**A change whose gain or loss sits only in a flagged trait's reading is
reported as that, and judged on the content reading.** The full reading
stays in the record, beside it. Cell definitions and the shared offsets
don't change. They belong to all three codecs (enclab
`spec/PARITY-SPEC.md`), and a change to one is proposed there.

`vbr_lf.py`'s report prints each cell's flags beside its number.
`parity_spec.py` prints them in its solve log and lists the flagged cells
under the quality table.

## The census

`python3 scripts/media_traits.py show` prints every title's flags and
`lead_skip`. What it found:

- **Every matrix title opens on black.** Meridian 60 frames (1.0 s),
  Chimera 120 (5.0 s), Sol Levante 193 (8.0 s), at all three sizes. So do
  the probe windows cut at frame 0 (mer1080_w0, sol1080_w0) and tier S's
  Meridian 2160p (60). The long-form excerpts don't, because the shared
  offsets start past the opening card. StEM2's excerpt ends on 10 black
  frames, and Cosmos and StEM2 end on a still (58 and 49 frames).
- **Meridian and Chimera are mostly flat-typed.** The encoder types 82%
  of Meridian 720p's content frames flat (96% at 1080p and 2160p), and 60%
  / 77% of Chimera's. Both are narrow-range PQ masters read as 8-bit. So
  the flat branches of AQ, the CRF base, cu-tree and SAO decide most of
  these cells, not the natural-video branches. Sol Levante is 31-58% flat
  (it rises with size), the long-form titles 41-88% except Nocturne (0%)
  and Lash (100%).
- **Meridian's long-form excerpt is dark** (median p99 122). The segment
  isn't (171), and the opening probe window is (140).
- **Sol Levante cuts 22 times a minute**, and its probe windows 24 and 32.
  Sol's opening window also holds a fade-in and a dissolve (14%).
- **The corpus's fade clips read as fades.** fade_cif is 60% fade, and
  xfade_720p 18% dissolve. sintel_720p (the trailer) opens on 24 black
  frames, ends on 33, and is 23% transitions.
- Tier S, apart from Meridian, has no lead or trail run and no
  transitions.
