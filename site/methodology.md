---
title: Methodology - yah265
description: How the parity readings are taken, what makes a number valid, and how quality is scored.
---

# Methodology

The parity readings follow one specification shared by yah264, yah265 and yaav1. One test set, one set of goals, one bar and one report format serve all three encoders. Each encoder is compared with its own reference at that reference's medium preset, and for yah265 that's x265 4.3+1-e9b8812 at `--preset medium`.

Both sides run their own defaults. Neither gets a tuning flag the other doesn't. Any deviation from either encoder's defaults is stated beside the reading. The main deviation is in the ladders, which run both encoders at 6 threads.

## The matched point

Every cell is one clip or excerpt at one size. x265 runs at its default rate factor, CRF 28. yah265 is then solved onto x265's file size within ±1%, first by integer CRF and then by a fractional one. Speed and quality are both read at that point, so neither encoder can buy speed with bits.

x265's pictures change with its frame-thread count. So each cell has two x265 anchors, one at its default threading for G2 and G3 and one at a single frame thread for G1. Each anchor gets its own solve of yah265.

## Timing

- Both encoders read the same frames on standard input from one feeder. Neither ever gets a file path the other doesn't.
- The feeder alone must deliver at least 1.5 times the faster encoder's frame rate. Below that the cell is feeder-bound and its ratio is void.
- Rounds are interleaved, with the order of the two encoders reversed on alternate rounds. The median wall time is taken.
- The box must be quiet, judged on total CPU across all cores. No other work runs during a timed leg.
- Each run prints its output's md5, so a run that didn't happen is visible.

On the 2-minute and 10-minute cells, G1 and G2 would take many hours at one thread. They're timed on 30-second windows instead, one every three minutes, each coded as its own stream. yah265 is re-matched onto x265's summed window bytes. A goal whose summed bytes miss by more than 5% is void for that cell.

## The test media

| tier | content | how it's cut |
|---|---|---|
| S | twelve standard clips from CIF to 4K | each clip whole |
| M | Chimera, Sol Levante and Meridian at 1080p and 4K | from the first non-black frame to the end |
| L | seven open-licensed films at 720p, 1080p and 4K | 600 seconds from a recorded offset past the opening card, cut at the last frame before the end credits |
| C | a film scan, sports and screen content | quality counts, speed is shown and doesn't gate |

Openings and credits are cut because they can steer a reading. Black frames and text on flat backgrounds score near the top of VMAF's scale whatever the encoder does, and a five-rung fit through saturated points swings by tens of percent. Every start and end frame is recorded. The credits still get a supplementary row each on the results page.

Each ten-minute excerpt must represent its whole title. A scene scan classes every shot by detail and motion. The excerpt's share of each class may differ from the whole title's by about 10% at most. Two excerpts moved their offsets to meet that.

The smaller sizes of each film are scaled down from the master with VideoToolbox. Everything is fed as 8-bit 4:2:0 Y4M.

## Scoring quality

Quality is VMAF v1, the `vmaf_v1.0.16` model. v1 predicts from four features: banding (CAMBI), chroma detail, luma detail and motion. Sharpening that adds no real detail doesn't raise its score, and banding and chroma loss lower it.

v1 is scored at display resolution. An encode smaller than 1920x1080 in both dimensions is scaled up to fit that frame before scoring. The scaling is bicubic and keeps the aspect ratio. The reference and the decode get the same scaling. CAMBI is told the coded size and the coded bit depth. Scoring runs at 10 bits. It uses the 3H model at 1080p and below and the 1.5H model above, with the high-frame-rate variants at 50 fps and up.

At the matched point every frame is scored. The ladders score every frame on tier M and every third frame on tier L. The stride is odd so it can't line up with either encoder's mini-GOP period. An even stride aliases with the B-frame pattern, and that moved BD-rate by 1.3 to 6 points when it was measured.

## The bar

| what | bar |
|---|---|
| speed, per class, tier and goal | median ratio at or under 1.00, worst at or under 1.15 |
| quality at the matched point | median VMAF v1 difference no worse than -0.5 |
| any single cell | no more than 1.0 VMAF v1 or 1.0 dB PSNR-Y behind, size within 1% |
| ladders, tiers M and L | BD-rate on VMAF v1 at or under +1.0% on every curve, at CRF and at VBR |
| VBR tracking, tiers M and L | every rung within ±10% of its target over the whole cell |
| VBR speed, tiers M and L | G3 only, held to the same speed bar |

The unit of passing is a combination of class, tier and goal, such as 1080p, tier M, G3. It passes when its median and worst ratios meet the bar and quality holds for that class and tier. Full parity means every goal passes in every class and every tier. Tier C's speed is shown and kept outside that count.

The lookahead's depth is part of the reading. yah265 sweeps its own depth around x265's default instead of copying it, at CRF and VBR, on live action, animation and 60p content, at 1080p and 4K. x265 stays at its default 20 throughout. Every reading states both depths. Shot-based and chunked encoding never enter a reading.

## The ladders

CRF rungs are 20, 23, 26, 29 and 32 on both encoders. VBR rungs target x265's achieved bitrates at those CRFs on the same cell, with maxrate 1.5 times the target and a buffer of twice it. BD-rate is taken over the bytes each encoder actually wrote. Only curves from the binary under test count.

## What makes a number valid

A cell counts only when five things hold. The box was quiet. The feeder kept up. Both encoders read the same input path. The sizes matched within 1%. Every arm's output md5 was printed. A cell that fails one of these is marked with the reason in the report. Its number stays out of the class statistics.
