# Media traits

The [traits script](../scripts/media_traits.py) measures each test title
before anything is encoded. It writes one row per title to the
[traits CSV](data/media-traits.csv).

## Why measure the titles

A cell's number can move because of one quirk of the clip. Meridian 720p
opens on 60 black frames. When item 29 step 2's flat rule gave up an accident
those frames caused in the rate control, the cell read 1.79% worse. None of
that loss was about the 89 seconds of picture that follow. I want a trait
like that known before the reading.

## The titles

| tier | where | measured at |
|---|---|---|
| segment | the matrix's nine cells in `~/media/throughput` | each cell's own size |
| probe | the windows in `~/media/probe` | sliced out of their segment's series, because they're byte cuts of it |
| tier S | the shared twelve in `<media>/tier_s` | each clip's own size |
| long-form | the 600 s excerpts in `vbr_lf.py`'s table | 1080p geometry, through the long-form feeder |
| corpus | the seventeen clips in `tests/corpus` | each clip's own size |

A long-form excerpt is read at 1080p only. Its lead and trail runs, cuts,
fades and luma range hold at any size. Its flat share and noise level are
1080p numbers.

## The measurement pass

One pass per source reads each frame's luma once. It keeps a per-frame
series in a cache under `~/.cache/yah265-traits`, keyed on the source's path,
size and mtime. Because the CSV is a reduction of the series, a threshold
change costs seconds. Most traits are read on a box-averaged copy about 320
samples wide. The flat type is read at full size because it's the encoder's
own definition. The noise estimate is read at full size too, on at most 240
evenly spaced frames.

The flat type reproduces the encoder's content gate, `N265_FLAT_GATE` in
la.c, bit for bit. `media_traits.py check-flat` compares the two frame by
frame through `N265_FLAT_TRACE`. They matched on all 300 frames of
foreman_cif, on sita_toon_720p and fade_cif, and on 120 frames of Meridian
720p.

## Columns

| column | meaning |
|---|---|
| `w`, `h`, `fps`, `frames`, `seconds` | the source as read |
| `lead_black`, `trail_black` | frames from the start, or the end, that are black: luma p99 at or under 40 and mean at or under 24, in 8-bit codes |
| `lead_still`, `trail_still` | frames from the start, or the end, with no change: mean absolute luma difference to the previous frame under 0.1 on the small copy. That's a repeated or frozen picture. A locked-off live shot reads 0.12-0.3. A black run is also a still run |
| `lead_skip` | the first content frame: the longer of the two leading runs |
| `black_share` | share of all frames that are black |
| `fades`, `fade_frames` | fades to or from black: a mean-luma ramp of at least 6 frames, monotone by more than 0.5 a frame, into or out of a near-black frame, one with p99 at or under 48. Motion doesn't break it |
| `xfades`, `xfade_frames` | dissolves: windows of 8 frames whose ends differ by at least 6 codes on a copy about 40 samples wide, where every inner frame sits within 15% of that change from the straight blend of the ends and no single step is over 35% of it. A run counts from 12 frames. Frames already in a fade don't count |
| `cuts`, `cuts_per_min` | scene cuts: the scene score over 0.3, the enclab scene scan's threshold. The score is min(mafd, \|mafd - previous mafd\|) / 100 on the small copy's luma |
| `flat_share` | share of frames the encoder types flat, which is a flat-block share at or over 10% |
| `flat_share_nonblack` | the same over non-black frames |
| `lead_flat` | flat-typed frames from the start |
| `y_p01`, `y_p50`, `y_p99` | the median over non-black frames of each frame's 1st, 50th and 99th luma percentile |
| `dark_share` | share of non-black frames whose p99 is at or under 128 |
| `noise_sigma` | Immerkaer's noise estimator per 16x16 block, in 8-bit codes. It takes the 20th percentile over blocks off the clip rails, those with a mean of 25-229, then the median over the sampled non-black frames |
| `flags` | the traits below, with their values |

## What each flag can mislead

| flag | fires when | readings it can mislead |
|---|---|---|
| `lead_black:N` / `lead_still:N` | a leading run of at least 0.25 s and 6 frames. Still counts only past the black | Any rate control that learns from what it has coded: ABR and VBR operating points, the VBV's predictors, a CRF reading's first window. A first-second or first-window metric. Speed on a short clip, because black codes in almost no time |
| `trail_black:N` / `trail_still:N` | the same at the end | ABR's final rate error. The last window's metric |
| `fades:F+Xx/P%` | fades and dissolves cover at least 5% of frames | Weighted prediction, scene-cut placement, the lookahead's costs, cu-tree. Each reads differently inside a transition than on content |
| `cuts:N/min` | at least 20 cuts a minute | Key-frame placement, the lookahead and cu-tree depth, open GOP. A short-horizon rate control |
| `flat_mix:P%` | between 2% and 98% of non-black frames are flat-typed | The tools that branch on the flat type: the AQ clamp, the CRF base dithering, the cu-tree frame weight, SAO's content gate, the rect gate. Because a change to one branch moves only the frames it covers, the clip's total mixes two encoders |
| `dark:p99=N` | the median frame's p99 is at or under 150 | VMAF and PSNR on dark content. AQ, because variance is small everywhere. The flat gate, because dark detail reads as flat |
| `narrow:LO-HI` | the median frame's p99 minus its p01 is at or under 150 codes | The same tools as `dark`. PQ masters read as 8-bit with no tone map fall here: Meridian at 22-171 and Chimera at 52-198. So do some natural clips, such as ducks and riverbed |
| `noise:S` | sigma at or over 2.0 | Rate at any fixed quality, because grain costs bits. VMAF-NEG against PSNR, which disagree on grain |

The noise column is the weakest. Where a clip has no smooth area, the
estimator reads fine texture as noise: mobile_cif reads 3.3 and crowd_run
1.9. It also misses the two grain titles at 1080p. Nocturne at 1.56 and Lash
of the Whip at 0.80 are both under the flag. Read the number, and don't gate
on it.

A flag is a caution and decides nothing. The thresholds are at the top of
the script. Change one there and here together.

## The rule

I read a cell with a flagged trait two ways where the trait can steer the
reading. For a leading black or still run, the second reading starts at
`lead_skip` and runs to the end of the segment, at the cell's own rates.
`scripts/vbr_lf.py segc` runs the second reading. `seg` runs it after the
full reading.

When a change's gain or loss is only in a flagged trait's reading, I report
it as that and judge the change on the content reading. I keep the full
reading in the record. I don't change cell definitions or the shared offsets
here. They belong to all three codecs through enclab's
`spec/PARITY-SPEC.md`, where a change to one is proposed.

`vbr_lf.py`'s report prints each cell's flags with the cell's number.
`parity_spec.py` prints the flags in its solve log and lists the flagged
cells below its quality table.

## The census

`python3 scripts/media_traits.py show` prints each title's flags and
`lead_skip`. It found:

- The three matrix titles open on black at all three sizes:

  | title | black lead |
  |---|---|
  | Meridian | 60 frames, 1.0 s |
  | Chimera | 120 frames, 5.0 s |
  | Sol Levante | 193 frames, 8.0 s |

  The probe windows cut at frame 0, mer1080_w0 and sol1080_w0, open on
  black too. So does tier S's Meridian 2160p, at 60 frames. The long-form
  excerpts don't, because the shared offsets start past the opening card.
  StEM2's excerpt ends on 10 black frames. Cosmos and StEM2 end on a still,
  of 58 and 49 frames.
- Meridian and Chimera are mostly flat-typed. The encoder types 82% of
  Meridian 720p's content frames flat, and 96% at 1080p and 2160p. It types
  60% / 77% of Chimera's flat. Both are narrow-range PQ masters read as
  8-bit. The flat branches of AQ, the CRF base, cu-tree and SAO decide most
  of these cells. Sol Levante is 31-58% flat and rises with size. The
  long-form titles are 41-88% flat, except Nocturne at 0% and Lash at 100%.
- Meridian's long-form excerpt is dark, with a median p99 of 122. The
  segment isn't, at 171. The opening probe window is, at 140.
- Sol Levante cuts 22 times a minute. Its probe windows cut 24 and 32
  times. Sol's opening window also holds a fade-in and a dissolve, which
  make up 14% of it.
- The corpus's fade clips read as fades. fade_cif is 60% fade and
  xfade_720p is 18% dissolve. sintel_720p is the trailer. It opens on 24
  black frames and ends on 33. Transitions are 23% of it.
- Tier S, apart from Meridian, has no lead or trail run and no
  transitions.
