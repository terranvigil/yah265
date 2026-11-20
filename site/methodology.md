---
title: Methodology - yah265
description: How yah265 is compared with x265, what makes a number valid, how quality is scored and what each bar is.
---

# Methodology

This page has the rules behind the [results](results.md). yah265 is compared with x265 4.3+1-e9b8812 when both run at `--preset medium`.

Both sides run their own defaults. Neither gets a tuning flag the other doesn't. Any departure from a default is stated beside the reading. The main one is that the ladders run both encoders at 6 threads.

The test set, the goals, the bar and the report format come from one specification that yah265 shares with its sibling encoders, yah264 and yaav1.

## Words this page uses

- **Cell**: one clip or excerpt at one picture size.
- **Class**: a picture size. The classes are CIF, 720p, 1080p and 4K.
- **Tier**: a clip length. S is short clips, M is 2-minute segments and L is 10-minute films. C is a set of content types.
- **Anchor**: the x265 encode a cell is measured against.
- **Matched point**: the yah265 encode whose file size is within 1% of the anchor's.
- **Ladder**: the same cell coded at five quality settings, called rungs, on both encoders.
- **BD-rate**: the average bitrate difference between two ladders at equal quality.
- **Bar**: the pass mark for a reading.

## The matched point

A faster encoder can look faster only because it spent more bits, or look better only because it made a bigger file. Matching the file size takes both of those away.

<figure class="diagram">
<svg viewBox="0 0 720 300" role="img" aria-labelledby="mp-t mp-d">
<title id="mp-t">The matched point on two rate curves</title>
<desc id="mp-d">Two curves of VMAF v1 against file size, one per encoder. A vertical line marks the file size x265 wrote at CRF 28. yah265's CRF is solved until its file reaches that line. The quality difference and the encode times are both read there.</desc>
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
<text class="d-label" x="346" y="146">the anchor: x265 at CRF 28</text>
<line class="d-arrow d-muted" x1="212" y1="98" x2="318" y2="98"/>
<text class="d-label" x="206" y="92" text-anchor="end">yah265, with its CRF solved</text>
<text class="d-label" x="206" y="108" text-anchor="end">onto the same size</text>
<text class="d-label" x="330" y="50" text-anchor="middle">same file size, within 1%</text>
<text class="d-label" x="346" y="170">speed and quality are both read here</text>
</svg>
<figcaption>One cell's matched point. The curves are a sketch.</figcaption>
</figure>

x265 runs first, at its default rate factor of CRF 28. yah265 is then solved onto x265's file size within ±1% by trying integer CRFs and then a fractional one. Speed and quality are both read at that one point.

Each cell has two x265 anchors because x265's pictures change with its frame-thread count. G2 and G3 use the anchor at x265's default threading. G1 uses the one at a single frame thread. Each anchor gets its own solve of yah265.

## Timing

- Both encoders read the same frames on standard input from one feeder. Neither ever gets a file path the other doesn't.
- The feeder alone must deliver at least 1.5 times the faster encoder's frame rate. Below that the cell is feeder-bound and its ratio is void.
- Rounds are interleaved. The order of the two encoders reverses on alternate rounds. The reading is the median wall time.
- The machine must be quiet, judged on total CPU across all cores. No other work runs during a timed leg.
- Each run prints its output's md5. A run that didn't happen leaves a gap there.

G1 and G2 would take many hours at one thread on the 2-minute and 10-minute cells. They're timed on 30-second windows instead. A window starts every three minutes and is coded as its own stream. yah265 is matched again onto x265's summed window bytes. A goal whose summed bytes miss by more than 5% is void for that cell.

## The test media

| tier | content | how it's cut |
|---|---|---|
| S | twelve standard clips from CIF to 4K | each clip whole |
| M | Chimera, Sol Levante and Meridian at 1080p and 4K | from the first non-black frame to the end |
| L | seven open-licensed films at 720p, 1080p and 4K | 600 seconds from a recorded offset past the opening card, cut at the last frame before the end credits |
| C | a film scan, sports and screen content | quality counts toward the verdict. Speed is shown and doesn't count |

Openings and credits are cut because they can steer a reading. Black frames and text on flat backgrounds score near the top of VMAF's scale whatever the encoder does. A five-rung fit through points that saturated swings by tens of percent. Every start and end frame is recorded. The credits still get a row of their own in the dated report that the results page links to.

Each ten-minute excerpt has to represent its whole title. A scene scan classes every shot by detail and motion. The excerpt's share of each class may differ from the whole title's by about 10% at most. Two excerpts moved their offsets to meet that.

The smaller sizes of each film are scaled down from the master with VideoToolbox. Everything is fed as 8-bit 4:2:0 Y4M.

## Scoring quality

Quality is VMAF v1 with the `vmaf_v1.0.16` model. v1 predicts from four features: banding (CAMBI), chroma detail, luma detail and motion. Sharpening that adds no real detail doesn't raise its score. Banding and chroma loss lower it.

v1 is scored at display resolution. An encode smaller than 1920x1080 in both dimensions is scaled up to fit that frame before scoring. The scaling is bicubic and keeps the aspect ratio. The reference gets the same scaling as the decode. CAMBI is told the coded size and the coded bit depth.

Scoring runs at 10 bits. The model follows the picture:

| picture | model |
|---|---|
| 1080p and below | the 3H viewing-distance model |
| above 1080p | the 1.5H model |
| 50 fps and up | the high-frame-rate variant of either |

Every frame is scored at the matched point. The ladders score every frame on tier M and every third frame on tier L. The stride is odd because an even stride lines up with an encoder's B-frame pattern. When I measured that, it moved BD-rate by 1.3 to 6 points.

## The bar

| what | bar |
|---|---|
| speed, per class, tier and goal | median ratio at or under 1.00, slowest clip at or under 1.15 |
| quality at the matched point | median VMAF v1 difference no worse than -0.5 |
| any single cell | no more than 1.0 VMAF v1 behind, size within 1% |
| ladders, tiers M and L | BD-rate on VMAF v1 at or under +1.0% on every curve, at CRF and at VBR |
| VBR tracking, tiers M and L | every rung within ±10% of its target over the whole cell |
| VBR speed, tiers M and L | G3 only, held to the same speed bar |

The unit of passing is a combination of class, tier and goal, for example 1080p at tier M for G3. It passes when its median and slowest ratios meet the bar and quality holds for that class and tier. Full parity means every goal passes in every class and every tier on every machine. Tier C's speed is shown and kept outside that count.

The lookahead's depth is part of the reading. yah265's own depth is swept around x265's default on live action, animation and 60p content at 1080p and 4K, under CRF and VBR. x265 stays at its default 20 throughout. Every reading states both depths. Shot-based and chunked encoding never enter a reading.

## The ladders

CRF rungs are 20, 23, 26, 29 and 32 on both encoders. VBR rungs target the bitrates x265 reached at those CRFs on the same cell. Each VBR rung sets the maxrate to 1.5 times the target and the buffer to twice the target. BD-rate is taken over the bytes each encoder wrote. Only curves from the binary under test count.

## What makes a number valid

A cell counts only when five things hold:

- the machine was quiet
- the feeder kept up
- both encoders read the same input the same way
- the sizes matched within 1%
- every run's output md5 was printed

A cell that fails one of these is marked with the reason in the report. Its number stays out of the class statistics.
