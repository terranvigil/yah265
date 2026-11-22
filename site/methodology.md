---
title: Methodology - yah265
description: How yah265 is compared with x265, what makes a number valid, how quality is scored and what each limit is.
---

# Methodology

This page has the rules behind the [results](results.md). [How x265 was run](x265-run.md) has the exact commands. yah265 is compared with x265 4.3+1-e9b8812 when both run at `--preset medium`.

Both sides run their own `medium` defaults with no `--tune`. yah265's `medium` adapts by itself to some content, such as screen recordings and handheld footage. x265 has tunes for grain and animation that I didn't use. The Homebrew build has no screen-content mode. Any departure from a default is stated beside the result. The main one is that the ladders run both encoders at 6 threads.

The test set and the limits come from one specification that yah265 shares with its sibling encoders, yah264 and yaav1.

## Words this page uses

- **Cell**: one clip or excerpt at one picture size.
- **Picture size**: CIF, 720p, 1080p or 4K.
- **Tier**: a clip length. S is short clips, M is 2-minute segments and L is 10-minute films. C is a set of content types.
- **Matched point**: the yah265 encode whose file size is within 1% of the x265 encode's.
- **Ladder**: the same cell coded at five quality settings, called rungs, on both encoders.
- **BD-rate**: the average bitrate difference between two ladders at equal quality.
- **Quality limit**: yah265 may need at most 1.0% more bits than x265 for the same VMAF v1.

## The three speed goals

I time yah265 against x265 three ways and call each one a goal.

| goal | what runs | why |
|---|---|---|
| G3 | both encoders as shipped, with SIMD on all cores | the speed a user sees |
| G1 | both encoders in plain C on one thread | compares the algorithms alone |
| G2 | both encoders in plain C on all cores | compares the algorithms and their threading |

G3 is the goal a user sees, because it times both encoders as shipped. The main result on the [home page](index.md) is G3.

G1 and G2 switch the hand-written SIMD off on both sides to compare the algorithms alone. Those two are engineering checks, because x265 isn't meant to run without its assembly.

## The matched point

An encoder can look faster, or score higher, just because it wrote a bigger file. Matching the file size rules that out.

<figure class="diagram">
<svg class="d-wide" viewBox="0 0 720 300" role="img" aria-labelledby="mp-t mp-d">
<title id="mp-t">The matched point on two rate curves</title>
<desc id="mp-d">Two curves of VMAF v1 against file size, one per encoder. A vertical line marks the file size x265 wrote at CRF 28. yah265's CRF is adjusted until its file reaches that line. The quality difference and the encode times are both read there.</desc>
<line class="d-line" x1="80" y1="250" x2="660" y2="250"/>
<line class="d-line" x1="80" y1="250" x2="80" y2="30"/>
<text class="d-label" x="370" y="286" text-anchor="middle">file size</text>
<text class="d-label" x="40" y="140" text-anchor="middle" transform="rotate(-90 40 140)">VMAF v1</text>
<path class="d-line" d="M110,220 Q260,110 640,70"/>
<path class="d-line d-accent" d="M110,200 Q250,80 640,50"/>
<text class="d-label" x="648" y="76">x265</text>
<text class="d-label d-accent" x="648" y="52">yah265</text>
<line class="d-dash" x1="330" y1="250" x2="330" y2="60"/>
<circle class="d-box" cx="330" cy="124" r="7"/>
<circle class="d-accent" cx="330" cy="98" r="7"/>
<text class="d-label" x="346" y="146">x265 at CRF 28</text>
<line class="d-arrow d-muted" x1="212" y1="98" x2="318" y2="98"/>
<text class="d-label" x="206" y="92" text-anchor="end">yah265, with its CRF adjusted</text>
<text class="d-label" x="206" y="108" text-anchor="end">onto the same size</text>
<text class="d-label" x="330" y="50" text-anchor="middle">same file size, within 1%</text>
<text class="d-label" x="346" y="170">speed and quality are both read here</text>
</svg>
<svg class="d-narrow" viewBox="0 0 360 352" role="img" aria-labelledby="mp-t-n mp-d-n">
<title id="mp-t-n">The matched point on two rate curves</title>
<desc id="mp-d-n">Two curves of VMAF v1 against file size, one per encoder. A vertical line marks the file size x265 wrote at CRF 28. yah265's CRF is adjusted until its file reaches that line. The quality difference and the encode times are both read there.</desc>
<line class="d-line" x1="40" y1="230" x2="340" y2="230"/>
<line class="d-line" x1="40" y1="230" x2="40" y2="10"/>
<text class="d-label" x="190" y="250" text-anchor="middle">file size</text>
<text class="d-label" x="22" y="120" text-anchor="middle" transform="rotate(-90 22 120)">VMAF v1</text>
<path class="d-line" d="M55,200 Q118.7,90 280,50"/>
<path class="d-line d-accent" d="M55,180 Q114.5,60 280,30"/>
<text class="d-label" x="288" y="56">x265</text>
<text class="d-label d-accent" x="288" y="34">yah265</text>
<line class="d-dash" x1="148.4" y1="230" x2="148.4" y2="40"/>
<circle class="d-box" cx="148.4" cy="104" r="7"/>
<circle class="d-accent" cx="148.4" cy="78" r="7"/>
<circle class="d-box" cx="16" cy="272" r="7"/>
<text class="d-label" x="32" y="277">x265 at CRF 28</text>
<circle class="d-accent" cx="16" cy="296" r="7"/>
<text class="d-label" x="32" y="301">yah265, with its CRF adjusted to the same size</text>
<line class="d-dash" x1="16" y1="312" x2="16" y2="342"/>
<text class="d-label" x="32" y="325">same file size, within 1%:</text>
<text class="d-label" x="32" y="342">speed and quality are both read here</text>
</svg>
<figcaption>One cell's matched point. The curves are a sketch.</figcaption>
</figure>

x265 runs first, at its default rate factor of CRF 28. I then adjust yah265's CRF until its file is within 1% of x265's size. Speed and quality are both read at that one point.

x265 writes a different file at one thread than at its default threading. So [G1](#the-three-speed-goals) gets its own x265 encode and its own size match.

## Timing

- Both encoders read the same frames on standard input from one feeder.
- The feeder alone must deliver at least 1.5 times the faster encoder's frame rate. I don't count a cell below that.
- Rounds are interleaved. The order of the two encoders reverses on alternate rounds. The result is the median wall time.
- The machine must be quiet, judged on total CPU across all cores. No other work runs during a timed leg.
- Each run prints its output's md5.

Because [G1 and G2](#the-three-speed-goals) would take many hours at one thread on the 2-minute and 10-minute cells, they're timed on 30-second windows instead. A window starts every three minutes. yah265 is size-matched again over the windows. If the total misses by more than 5%, I don't count the result.

## The test media

| tier | content | how it's cut |
|---|---|---|
| S | eleven standard clips from CIF to 4K | each clip whole |
| M | Sol Levante at 1080p and 4K | from the first non-black frame to the end |
| L | six open-licensed films at 720p, 1080p and 4K | 600 seconds from a recorded offset past the opening card, cut at the last frame before the end credits |
| C | a film scan, sports and screen content | quality counts toward the verdict. Speed is only shown |

The main result on the home page covers the size-matched CRF cells of tiers S, M and L on the Mac at [G3](#the-three-speed-goals). Capped VBR timings have their own table. Tier C's timings are reported with its quality.

The set has no held-out content, because the development clips were windows of these same titles.

Because each ten-minute excerpt has to represent its whole title, I checked each one's mix of detail and motion against the title's. I moved the offsets of two excerpts to match.

The smaller sizes of each film are scaled down from the master with VideoToolbox. Everything is fed as 8-bit 4:2:0 Y4M.

## Scoring quality

Quality is VMAF v1, Netflix's `vmaf_v1.0.16` models. The scorer is libvmaf 3.x built from Netflix's public repository at commit `0f9912e`. v1 predicts from four features: banding (CAMBI), chroma detail, luma detail and motion. Sharpening that adds no real detail doesn't raise its score. Banding and chroma loss lower it.

VMAF v1 is the only quality metric these pages judge by. yah265's `medium` is tuned for it. x265's `medium` has other aims that include its psychovisual options. A lead here is a lead on that one score. The results page also shows frame crops because a score can't settle how two encodes look.

v1 is scored at display resolution. Before scoring, an encode smaller than 1920x1080 in both dimensions is scaled up to fit that frame. For a CIF clip that's a 3.75x upscale. The reference gets the same bicubic scaling as the decode, with the aspect ratio kept. CAMBI is told the coded size and the coded bit depth.

In the report CSVs the columns whose names end in `vmafneg` hold VMAF v1 scores. The names are left from the metric the project used before.

Scoring runs at 10 bits. The model follows the picture:

| picture | model |
|---|---|
| 1080p and below | `vmaf_v1.0.16_3d0h`, for viewing at three picture heights |
| above 1080p | `vmaf_v1.0.16_1d5h_2160`, for one and a half |
| 50 fps and up | the `_hfr` variant of either |

All frames are scored at the matched point. The ladders score all frames on the 2-minute segments and every third frame on the 10-minute films. The step is odd because an even one lines up with an encoder's B-frame pattern, which moved BD-rate by 1.3 to 6 points.

### The frames shown side by side

A script picks the frames on Results by a fixed rule. It scores each frame by the mean of the two encoders' VMAF v1, then takes the lowest frame in a low-scoring shot. To place the crop, it measures both encodes against the reference block by block. The crop is the window where their losses differ most. The reference is a near-transparent copy of the source.

## The limits

| what | limit |
|---|---|
| speed, per picture size, clip length and goal | median at least 1.00x, slowest clip at least 0.87x |
| quality at the matched point | median VMAF v1 difference no worse than -0.5 |
| any single cell | no more than 1.0 VMAF v1 behind, size within 1% |
| ladders, 2-minute segments and 10-minute films | BD-rate on VMAF v1 at or under +1.0% on each curve, at CRF and at VBR |
| VBR tracking, 2-minute segments and 10-minute films | each rung within ±10% of its target over the whole cell |
| VBR speed, 2-minute segments and 10-minute films | [G3](#the-three-speed-goals) only, held to the same speed limit |

Each picture size and clip length is judged separately for each goal. It passes when both speed numbers meet the limit and quality holds too. Full parity means all three goals pass at each picture size and clip length, on each machine. Tier C's speed is shown outside that count.

The lookahead's depth is part of the measurement. I swept yah265's own depth around x265's default under CRF and VBR. The sweep covers live action, animation and 60p content at 1080p and 4K. x265 stays at its default 20 throughout. No measurement uses shot-based or chunked encoding.

## The ladders

CRF rungs are 20, 23, 26, 29 and 32 on both encoders. VBR rungs target the bitrates x265 reached at those CRFs on the same cell. Each VBR rung sets the maxrate to 1.5 times the target and the buffer to twice the target. BD-rate is taken over the bytes each encoder wrote. Only curves from the binary under test count.

## What makes a number valid

A cell counts only when five things hold:

- the machine was quiet
- the feeder kept up
- both encoders read the same input the same way
- the sizes matched within 1%
- each run's output md5 was printed

A cell that fails one of these is marked with the reason in the report. I didn't include its number in the statistics for its picture size.
