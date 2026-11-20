---
title: Results - yah265
description: The parity tables against x265 medium, from CIF to 4K and from short clips to ten-minute films, and how to read them.
---

# Results

On the Mac, yah265 has reached full parity with x265. Every goal passes in every class and every timed tier, from CIF to 4K and from ten-second clips to ten-minute films. Both encoders run at `--preset medium`. The reference is the Homebrew build of x265 4.3+1-e9b8812. It includes x265's own arm64 assembly. Everything was timed on one Apple M5 Max with 18 cores. The same goals also pass on an x86-64 Xeon with AVX-512 and on a Graviton4 with SVE2. Their tables are in the [all-architecture report](docs/parity/PARITY-REPORT-yah265-all-isa-2026-11-14.md).

[The methodology](methodology.md) covers the test set, the rules that make a number valid and how quality is scored. The full report this page is built from is [in the repository](docs/parity/PARITY-REPORT-yah265-2026-11-12.md).

## How to read the ratios

A speed ratio is our wall time divided by x265's when both encoders write files of the same size. Below 1.00 means yah265 finished first. A ratio of 0.38 means we took 38% of x265's time. That's the same as running 2.6 times as fast.

Each table cell gives two ratios: the median over a group's clips and the worst clip. A group passes when its median is at or under 1.00 and its worst clip is at or under 1.15. Its quality must also hold.

| goal | yah265 | x265 | threads |
|---|---|---|---|
| G1 | pure C | `--no-asm` | one, which x265 gets from `--pools none --frame-threads 1` |
| G2 | pure C | `--no-asm` | each encoder's default |
| G3 | as shipped, with NEON | as shipped, with its assembly | each encoder's default |

yah265's pure C is the same binary with `N265_ASM_OFF=all`. x265's is the same binary with `--no-asm`.

## The test set

| tier | what it is |
|---|---|
| S, short clips | twelve standard test clips. The set has three at CIF, four at 720p, three at 1080p and two at 4K |
| M, 2-minute segments | Chimera, Sol Levante and Meridian at 1080p and 4K. Each starts at its first non-black frame |
| L, 10-minute excerpts | seven open films at 720p, 1080p and 4K: Tears of Steel, Meridian, StEM2, Sintel, Big Buck Bunny, Cosmos Laundromat and Nocturne. Each is cut at its last frame before the end credits |
| C, content types | film grain, sports and screen content. Speed here doesn't count toward parity. None of it has been measured yet |

Both encoders read the same 8-bit 4:2:0 frames on standard input from one feeder.

## Speed

Median / worst ratio in each class and tier. Lower is faster.

| class | tier | G1 (C on one thread) | G2 (C on all cores) | G3 (SIMD on all cores) | verdict |
|---|---|--:|--:|--:|---|
| CIF | S | 0.46 / 0.48 | 0.32 / 0.35 | 0.51 / 0.57 | pass |
| 720p | S | 0.30 / 0.43 | 0.30 / 0.33 | 0.62 / 0.65 | pass |
| 720p | L | 0.28 / 0.32 | 0.24 / 0.31 | 0.47 / 0.56 | pass |
| 1080p | S | 0.32 / 0.37 | 0.30 / 0.34 | 0.52 / 0.56 | pass |
| 1080p | M | 0.35 / 0.40 | 0.33 / 0.37 | 0.59 / 0.66 | pass |
| 1080p | L | 0.32 / 0.36 | 0.30 / 0.31 | 0.57 / 0.59 | pass |
| 2160p | S | 0.23 / 0.25 | 0.32 / 0.35 | 0.62 / 0.63 | pass |
| 2160p | M | 0.30 / 0.36 | 0.32 / 0.39 | 0.56 / 0.60 | pass |
| 2160p | L | 0.30 / 0.32 | 0.35 / 0.38 | 0.58 / 0.66 | pass |

All 27 combinations of class, tier and goal pass. The slowest clips are Meridian's 1080p segment and Sintel at 4K, both at 0.66 with SIMD on. G3 is the closest of the three goals. On this machine x265's assembly makes it about three times as fast as its own C.

Four Meridian cells are left out of the count. Their feeder couldn't deliver frames 1.5 times as fast as the faster encoder ran. By rule, that makes the ratio void. They are the 4K segment at G3, the 720p film at G3, and the 1080p film at G2 and G3. The other cells in each row make up its median.

### Each ten-minute film

Each film runs from its start frame to its last story frame.

| cell | last story frame | G1 | G2 | G3 |
|---|--:|--:|--:|--:|
| tears_720p | 12671 | 0.28 | 0.23 | 0.47 |
| tears_1080p | 12671 | 0.33 | 0.30 | 0.58 |
| tears_2160p | 12671 | 0.30 | 0.34 | 0.55 |
| meridian_720p | 32886 | 0.18 | 0.31 | void |
| meridian_1080p | 32886 | 0.19 | void | void |
| meridian_2160p | 32886 | 0.19 | 0.32 | 0.64 |
| stem2_720p | 12939 | 0.28 | 0.22 | 0.45 |
| stem2_1080p | 12939 | 0.32 | 0.29 | 0.56 |
| stem2_2160p | 12939 | 0.32 | 0.36 | 0.56 |
| sintel_720p | 13177 | 0.29 | 0.24 | 0.47 |
| sintel_1080p | 13177 | 0.31 | 0.31 | 0.58 |
| sintel_2160p | 13177 | 0.32 | 0.38 | 0.66 |
| bbb_720p | 13928 | 0.29 | 0.28 | 0.56 |
| bbb_1080p | 13928 | 0.33 | 0.31 | 0.59 |
| bbb_2160p | 13928 | 0.31 | 0.37 | 0.60 |
| cosmos_720p | 13723 | 0.32 | 0.22 | 0.48 |
| cosmos_1080p | 13723 | 0.36 | 0.30 | 0.55 |
| nocturne_720p | 34319 | 0.23 | 0.27 | 0.56 |
| nocturne_1080p | 34319 | 0.28 | 0.29 | 0.56 |
| nocturne_2160p | 34319 | 0.28 | 0.29 | 0.48 |

On the 2-minute and 10-minute cells, G1 and G2 are timed on 30-second windows. A window starts every three minutes and is coded as its own stream. On every cell our summed window bytes come within 0.7% of x265's. The rule allows 5%.

## Quality at the same size

The VMAF v1 difference at the matched file size, ours minus x265's. Positive means yah265 looks better. The worst cell is the one with the smallest lead.

| class | tier | median | worst | worst cell |
|---|---|--:|--:|---|
| CIF | S | +2.33 | +0.56 | stefan_cif |
| 720p | S | +2.39 | +0.86 | shields_720p |
| 720p | L | +1.67 | +0.61 | bbb_720p |
| 1080p | S | +0.61 | -0.05 | sunflower_1080p |
| 1080p | M | +1.96 | +0.63 | chimera_1080p |
| 1080p | L | +1.08 | +0.70 | bbb_1080p |
| 2160p | S | +1.86 | +1.22 | bbb_2160p |
| 2160p | M | +1.40 | +0.06 | chimera_2160p |
| 2160p | L | +1.18 | +0.44 | bbb_2160p |

The bar is a median no worse than -0.5 with no single cell more than 1.0 behind. sunflower_1080p is the only cell behind x265 at all, by 0.05. Every cell also stays within 1.0 dB of x265 on PSNR-Y and within 1% of its file size.

## Quality across the rate range

Because one matched point says little about the rest of the curve, every 2-minute and 10-minute cell also runs a five-rung ladder on both encoders. BD-rate turns two rate-quality curves into one number: the share of bitrate yah265 needs to change to match x265's quality. Negative means yah265 needs fewer bits. The bar is +1.0% on every curve.

CRF rungs are 20, 23, 26, 29 and 32 on both sides. VBR rungs target x265's achieved bitrates at those CRFs. Each VBR rung sets the maxrate to 1.5 times the target and the buffer to twice the target. Both encoders run at 6 threads and look 20 frames ahead.

| cell | tier | CRF BD-rate | VBR BD-rate |
|---|---|--:|--:|
| chimera_1080p | M | -6.07% | -4.83% |
| sollevante_1080p | M | -14.35% | -9.67% |
| meridian_1080p | M | -28.69% | -27.64% |
| chimera_2160p | M | -0.29% | +0.70% |
| sollevante_2160p | M | -12.18% | -6.97% |
| meridian_2160p | M | -16.89% | -15.33% |
| tears_720p | L | -7.23% | -4.01% |
| tears_1080p | L | -12.58% | -10.34% |
| tears_2160p | L | -13.31% | -14.01% |
| meridian_720p | L | -11.15% | -5.53% |
| meridian_1080p | L | -16.62% | -15.88% |
| meridian_2160p | L | -13.66% | -10.70% |
| stem2_720p | L | -13.83% | -7.64% |
| stem2_1080p | L | -17.97% | -13.81% |
| stem2_2160p | L | -12.55% | -9.12% |
| sintel_720p | L | -12.00% | -7.97% |
| sintel_1080p | L | -10.02% | -12.44% |
| sintel_2160p | L | -16.81% | -13.31% |
| bbb_720p | L | -5.25% | -0.42% |
| bbb_1080p | L | -9.04% | -6.89% |
| bbb_2160p | L | -4.94% | -4.59% |
| cosmos_720p | L | -15.30% | -14.13% |
| cosmos_1080p | L | -11.32% | -11.82% |
| nocturne_720p | L | -6.27% | -0.41% |
| nocturne_1080p | L | -8.31% | -6.88% |
| nocturne_2160p | L | -10.04% | -15.47% |

All 52 curves pass. The median is -10.9%. yah265 is ahead of x265 on 51 of them. The closest to the bar is Chimera at 4K under VBR, at +0.70%.

### VBR rate tracking

Each VBR rung's achieved bitrate, against its target, over the whole cell. yah265's bar is ±10% on every rung. x265's column is there for comparison and isn't held to any bar.

| cell | tier | ours, five rungs | x265, five rungs |
|---|---|---|---|
| chimera_1080p | M | -5.2% to -3.0% | -14.6% to -14.2% |
| sollevante_1080p | M | -4.6% to -3.2% | -7.9% to -6.2% |
| meridian_1080p | M | -4.5% to -2.8% | -5.7% to -2.6% |
| chimera_2160p | M | -5.2% to -3.6% | -14.4% to -13.7% |
| sollevante_2160p | M | -6.1% to -3.8% | -8.2% to -7.6% |
| meridian_2160p | M | -3.5% to -1.4% | -5.8% to -1.6% |
| tears_720p | L | +0.0% to +0.2% | +1.5% to +1.6% |
| tears_1080p | L | +0.2% to +0.5% | +1.6% to +1.7% |
| tears_2160p | L | +0.0% to +0.4% | +0.4% to +1.6% |
| meridian_720p | L | -2.5% to -2.1% | -3.5% to -1.3% |
| meridian_1080p | L | -2.2% to -1.9% | -3.2% to -2.3% |
| meridian_2160p | L | -1.7% to -1.5% | -3.7% to -3.2% |
| stem2_720p | L | -5.8% to -4.4% | -10.3% to -8.1% |
| stem2_1080p | L | -6.4% to -4.3% | -11.2% to -5.7% |
| stem2_2160p | L | -5.6% to -4.1% | -12.0% to -3.8% |
| sintel_720p | L | -7.7% to -6.3% | -3.4% to -3.3% |
| sintel_1080p | L | -5.8% to -4.2% | -3.1% to -2.8% |
| sintel_2160p | L | -2.7% to -1.1% | -2.2% to -2.1% |
| bbb_720p | L | -1.3% to -0.9% | -3.5% to -2.2% |
| bbb_1080p | L | -1.1% to -0.9% | -4.1% to -3.5% |
| bbb_2160p | L | -0.9% to -0.8% | -4.1% to -3.9% |
| cosmos_720p | L | -2.7% to -1.8% | -0.4% to -0.1% |
| cosmos_1080p | L | -2.9% to -2.4% | -0.8% to -0.6% |
| nocturne_720p | L | -1.3% to -0.6% | +1.3% to +4.4% |
| nocturne_1080p | L | -1.6% to -0.6% | -0.8% to +3.4% |
| nocturne_2160p | L | -1.4% to -0.6% | -0.9% to +1.8% |

All 130 of yah265's rungs come in between -7.7% and +0.5% of their targets.

### Lookahead depth

The lookahead's depth was swept on six cells. Ours ran at 12, 20 and 30 frames, and x265 stayed at its own 20 throughout. The table gives BD-rate on VMAF v1.

| cell | mode | ours 12 | ours 20 | ours 30 |
|---|---|--:|--:|--:|
| chimera_2160p (M) | CRF | +0.28% | -0.29% | -0.27% |
| sollevante_1080p (M) | CRF | -13.76% | -14.35% | -14.41% |
| meridian_2160p (M) | CRF | -15.19% | -16.89% | -16.74% |
| tears_1080p (L) | CRF | -11.43% | -12.58% | -12.86% |
| bbb_1080p (L) | CRF | -5.68% | -9.04% | -10.21% |
| sintel_2160p (L) | CRF | -15.61% | -16.81% | -16.91% |
| chimera_2160p (M) | VBR | +0.61% | +0.70% | +0.68% |
| sollevante_1080p (M) | VBR | -8.90% | -9.67% | -9.70% |
| meridian_2160p (M) | VBR | -13.72% | -15.33% | -15.33% |
| tears_1080p (L) | VBR | -9.60% | -10.34% | -10.55% |
| bbb_1080p (L) | VBR | -4.28% | -6.89% | -7.35% |
| sintel_2160p (L) | VBR | -13.52% | -13.31% | -13.43% |

20 is `medium`'s depth from 720p up. Thirty frames gains 1.2 points on Big Buck Bunny at CRF. Elsewhere it moves half a point or less. An earlier sweep from 8 to 40 frames on the development set put the knee at 20. Past that point a deeper window costs more time than it buys.

## VBR speed

The VBR wall counts toward the bar at G3 only. Each 2-minute and 10-minute cell runs one rung at x265's CRF 28 bitrate with the maxrate at 1.5 times that target and the buffer at twice it. Because there's no size match here, the table shows both achieved rates.

| cell | tier | target kbit/s | ratio | ours vs target | x265 vs target |
|---|---|--:|--:|--:|--:|
| chimera_1080p | M | 1668 | 0.63 | -4.1% | -14.5% |
| sollevante_1080p | M | 7863 | 0.59 | -3.8% | -7.1% |
| meridian_1080p | M | 318 | 0.68 | -4.0% | -5.4% |
| chimera_2160p | M | 4515 | 0.60 | -5.1% | -14.5% |
| sollevante_2160p | M | 20733 | 0.66 | -4.0% | -8.2% |
| meridian_2160p | M | 949 | 0.64 | -3.0% | -4.3% |
| tears_720p | L | 628 | 0.38 | +0.1% | +1.6% |
| tears_1080p | L | 1279 | 0.61 | +0.4% | +1.6% |
| tears_2160p | L | 3389 | 0.64 | +0.2% | +1.4% |
| meridian_720p | L | 205 | 0.76 | -2.4% | -2.6% |
| meridian_1080p | L | 438 | 0.85 | -2.2% | -3.0% |
| meridian_2160p | L | 1326 | 0.65 | -1.6% | -3.2% |
| stem2_720p | L | 567 | 0.42 | -5.1% | -10.3% |
| stem2_1080p | L | 1145 | 0.59 | -5.8% | -11.2% |
| stem2_2160p | L | 2964 | 0.61 | -5.2% | -9.9% |
| sintel_720p | L | 523 | 0.42 | -7.3% | -3.4% |
| sintel_1080p | L | 1113 | 0.64 | -5.3% | -3.0% |
| sintel_2160p | L | 4329 | 0.75 | -1.6% | -2.1% |
| bbb_720p | L | 620 | 0.54 | -1.2% | -2.8% |
| bbb_1080p | L | 1365 | 0.73 | -1.1% | -3.9% |
| bbb_2160p | L | 3453 | 0.68 | -0.9% | -4.1% |
| cosmos_720p | L | 734 | 0.39 | -2.3% | -0.4% |
| cosmos_1080p | L | 1407 | 0.61 | -2.8% | -0.8% |
| nocturne_720p | L | 1022 | 0.47 | -0.8% | +4.2% |
| nocturne_1080p | L | 2198 | 0.58 | -1.1% | +2.6% |
| nocturne_2160p | L | 9142 | 0.49 | -1.4% | -0.6% |

The slowest VBR cell is Meridian's 1080p film, at 0.85.

## End credits

The scored cells stop before the end credits because text on a flat background saturates VMAF and makes a BD fit swing. Each title's credits still get a row, so a weakness there stays visible. They're coded on their own at 1080p. x265 runs at CRF 28 and yah265 at the cell's matched CRF. The rows compare the file size each encoder needs for equal PSNR-Y. These rows aren't a gate.

| title | yah265's size at x265's PSNR-Y | reading |
|---|--:|---|
| Tears of Steel | +5.9% (extrapolated) | behind |
| Meridian | -55.4% | ahead |
| StEM2 | - | pending |
| Sintel | -35.4% | ahead |
| Big Buck Bunny | - | pending |
| Cosmos Laundromat | -19.0% | ahead |
| Nocturne | -32.8% | ahead |

yah265 is behind on one of the five credit rolls read so far, Tears of Steel. It's open work. The StEM2 and Big Buck Bunny rows are pending.

## Not measured yet

Tier C hasn't been measured. Besides four sports clips and three screen recordings, it holds a scan of the 1924 film Lash of the Whip. Once it's read, its quality will count toward parity. Its speed will be shown but won't count.
