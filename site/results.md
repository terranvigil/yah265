---
title: Results - yah265
description: The parity tables against x265 medium, from CIF to 4K and from short clips to ten-minute films, and how to read them.
---

# Results

yah265 has reached full parity with x265. Every goal passes in every class and every timed tier, from CIF to 4K and from ten-second clips to ten-minute films. Both encoders run at `--preset medium`. The reference is the Homebrew build of x265 4.3+1-e9b8812. It includes x265's own arm64 assembly. Everything was timed on one Apple M5 Max with 18 cores.

[The methodology](methodology.md) covers the test set, the rules that make a number valid and how quality is scored. The full report this page is built from is [in the repository](docs/parity/PARITY-REPORT-yah265-2026-11-04.md).

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
| CIF | S | 0.51 / 0.51 | 0.35 / 0.37 | 0.53 / 0.58 | pass |
| 720p | S | 0.38 / 0.52 | 0.36 / 0.41 | 0.70 / 0.72 | pass |
| 720p | L | 0.37 / 0.44 | 0.31 / 0.35 | 0.55 / 0.69 | pass |
| 1080p | S | 0.38 / 0.40 | 0.36 / 0.37 | 0.61 / 0.64 | pass |
| 1080p | M | 0.38 / 0.47 | 0.37 / 0.44 | 0.66 / 0.70 | pass |
| 1080p | L | 0.36 / 0.42 | 0.34 / 0.36 | 0.62 / 0.66 | pass |
| 2160p | S | 0.29 / 0.32 | 0.39 / 0.43 | 0.67 / 0.73 | pass |
| 2160p | M | 0.40 / 0.48 | 0.43 / 0.51 | 0.73 / 0.79 | pass |
| 2160p | L | 0.40 / 0.42 | 0.44 / 0.50 | 0.69 / 0.84 | pass |

All 27 combinations of class, tier and goal pass. The slowest clip anywhere is Sintel at 4K with SIMD on, at 0.84. G3 is the closest of the three goals. On this machine x265's assembly makes it about three times as fast as its own C.

One 4K cell is left out of the G3 count. The feeder for Meridian's 4K segment couldn't deliver frames 1.5 times as fast as the faster encoder ran. By rule, that makes its ratio void. The other two 4K segments make up that row.

### Each ten-minute film

Each film runs from its start frame to its last story frame.

| cell | last story frame | G1 | G2 | G3 |
|---|--:|--:|--:|--:|
| tears_720p | 12671 | 0.37 | 0.29 | 0.54 |
| tears_1080p | 12671 | 0.36 | 0.34 | 0.62 |
| tears_2160p | 12671 | 0.39 | 0.43 | 0.70 |
| meridian_720p | 32886 | 0.21 | 0.34 | 0.69 |
| meridian_1080p | 32886 | 0.19 | 0.32 | 0.66 |
| meridian_2160p | 32886 | 0.22 | 0.35 | 0.67 |
| stem2_720p | 12939 | 0.37 | 0.29 | 0.53 |
| stem2_1080p | 12939 | 0.36 | 0.34 | 0.61 |
| stem2_2160p | 12939 | 0.41 | 0.45 | 0.69 |
| sintel_720p | 13177 | 0.37 | 0.31 | 0.54 |
| sintel_1080p | 13177 | 0.34 | 0.35 | 0.62 |
| sintel_2160p | 13177 | 0.42 | 0.50 | 0.84 |
| bbb_720p | 13928 | 0.37 | 0.35 | 0.64 |
| bbb_1080p | 13928 | 0.37 | 0.36 | 0.64 |
| bbb_2160p | 13928 | 0.40 | 0.47 | 0.72 |
| cosmos_720p | 13723 | 0.44 | 0.29 | 0.55 |
| cosmos_1080p | 13723 | 0.42 | 0.36 | 0.60 |
| nocturne_720p | 34319 | 0.28 | 0.32 | 0.64 |
| nocturne_1080p | 34319 | 0.31 | 0.32 | 0.60 |
| nocturne_2160p | 34319 | 0.33 | 0.34 | 0.54 |

On the 2-minute and 10-minute cells, G1 and G2 are timed on 30-second windows. A window starts every three minutes and is coded as its own stream. On every cell our summed window bytes come within 0.5% of x265's. The rule allows 5%.

## Quality at the same size

The VMAF v1 difference at the matched file size, ours minus x265's. Positive means yah265 looks better. The worst cell is the one with the smallest lead.

| class | tier | median | worst | worst cell |
|---|---|--:|--:|---|
| CIF | S | +2.10 | +0.58 | stefan_cif |
| 720p | S | +2.46 | +0.80 | shields_720p |
| 720p | L | +1.67 | +0.88 | bbb_720p |
| 1080p | S | +0.58 | -0.15 | sunflower_1080p |
| 1080p | M | +2.09 | +0.53 | chimera_1080p |
| 1080p | L | +1.08 | +0.75 | bbb_1080p |
| 2160p | S | +2.08 | +1.40 | bbb_2160p |
| 2160p | M | +1.53 | +0.39 | chimera_2160p |
| 2160p | L | +1.27 | +0.76 | bbb_2160p |

The bar is a median no worse than -0.5 with no single cell more than 1.0 behind. sunflower_1080p is the only cell behind x265 at all, by 0.15. Every cell also stays within 1.0 dB of x265 on PSNR-Y and within 1% of its file size.

## Quality across the rate range

Because one matched point says little about the rest of the curve, every 2-minute and 10-minute cell also runs a five-rung ladder on both encoders. BD-rate turns two rate-quality curves into one number: the share of bitrate yah265 needs to change to match x265's quality. Negative means yah265 needs fewer bits. The bar is +1.0% on every curve.

CRF rungs are 20, 23, 26, 29 and 32 on both sides. VBR rungs target x265's achieved bitrates at those CRFs. Each VBR rung sets the maxrate to 1.5 times the target and the buffer to twice the target. Both encoders run at 6 threads and look 20 frames ahead.

| cell | tier | CRF BD-rate | VBR BD-rate |
|---|---|--:|--:|
| chimera_1080p | M | -4.49% | -3.44% |
| sollevante_1080p | M | -15.18% | -10.42% |
| meridian_1080p | M | -30.63% | -28.91% |
| chimera_2160p | M | -3.90% | -0.76% |
| sollevante_2160p | M | -15.29% | -9.27% |
| meridian_2160p | M | -18.13% | -17.98% |
| tears_720p | L | -8.61% | -5.46% |
| tears_1080p | L | -12.54% | -10.47% |
| tears_2160p | L | -15.14% | -15.16% |
| meridian_720p | L | -13.72% | -8.24% |
| meridian_1080p | L | -18.07% | -17.43% |
| meridian_2160p | L | -14.86% | -12.34% |
| stem2_720p | L | -16.41% | -11.28% |
| stem2_1080p | L | -19.19% | -14.88% |
| stem2_2160p | L | -16.97% | -12.68% |
| sintel_720p | L | -13.87% | -10.16% |
| sintel_1080p | L | -10.73% | -12.92% |
| sintel_2160p | L | -20.75% | -16.75% |
| bbb_720p | L | -6.94% | -2.47% |
| bbb_1080p | L | -9.51% | -6.75% |
| bbb_2160p | L | -8.44% | -6.27% |
| cosmos_720p | L | -15.67% | -14.75% |
| cosmos_1080p | L | -10.52% | -11.19% |
| nocturne_720p | L | -9.09% | -2.90% |
| nocturne_1080p | L | -11.83% | -8.08% |
| nocturne_2160p | L | -17.93% | -19.47% |

All 52 curves pass. The median is -12.4%. The closest to the bar is Chimera at 4K under VBR, at -0.76%.

### VBR rate tracking

Each VBR rung's achieved bitrate, against its target, over the whole cell. yah265's bar is ±10% on every rung. x265's column is there for comparison and isn't held to any bar.

| cell | tier | ours, five rungs | x265, five rungs |
|---|---|---|---|
| chimera_1080p | M | -5.6% to -3.1% | -14.6% to -14.2% |
| sollevante_1080p | M | -4.6% to -3.3% | -7.9% to -6.2% |
| meridian_1080p | M | -4.5% to -2.9% | -5.7% to -2.6% |
| chimera_2160p | M | -5.4% to -4.0% | -14.4% to -13.7% |
| sollevante_2160p | M | -6.2% to -3.9% | -8.2% to -7.6% |
| meridian_2160p | M | -3.4% to -1.5% | -5.8% to -1.6% |
| tears_720p | L | +0.0% to +0.2% | +1.5% to +1.6% |
| tears_1080p | L | +0.3% to +0.5% | +1.6% to +1.7% |
| tears_2160p | L | +0.1% to +0.4% | +0.4% to +1.6% |
| meridian_720p | L | -2.7% to -2.1% | -3.5% to -1.3% |
| meridian_1080p | L | -2.2% to -1.8% | -3.2% to -2.3% |
| meridian_2160p | L | -1.7% to -1.6% | -3.7% to -3.2% |
| stem2_720p | L | -5.9% to -4.6% | -10.3% to -8.1% |
| stem2_1080p | L | -6.6% to -4.2% | -11.2% to -5.7% |
| stem2_2160p | L | -5.5% to -4.3% | -12.0% to -3.8% |
| sintel_720p | L | -7.6% to -6.4% | -3.4% to -3.3% |
| sintel_1080p | L | -5.7% to -4.3% | -3.1% to -2.8% |
| sintel_2160p | L | -2.6% to -1.3% | -2.2% to -2.1% |
| bbb_720p | L | -1.3% to -0.9% | -3.5% to -2.2% |
| bbb_1080p | L | -1.1% to -1.0% | -4.1% to -3.5% |
| bbb_2160p | L | -1.0% to -0.8% | -4.1% to -3.9% |
| cosmos_720p | L | -2.8% to -1.9% | -0.4% to -0.1% |
| cosmos_1080p | L | -3.0% to -2.6% | -0.8% to -0.6% |
| nocturne_720p | L | -1.3% to -0.6% | +1.3% to +4.4% |
| nocturne_1080p | L | -1.6% to -1.0% | -0.8% to +3.4% |
| nocturne_2160p | L | -1.4% to -0.6% | -0.9% to +1.8% |

All 130 of yah265's rungs come in between -7.6% and +0.5% of their targets.

### Lookahead depth

The lookahead's depth was swept on six cells. Ours ran at 12, 20 and 30 frames, and x265 stayed at its own 20 throughout. The table gives BD-rate on VMAF v1.

| cell | mode | ours 12 | ours 20 | ours 30 |
|---|---|--:|--:|--:|
| chimera_2160p (M) | CRF | -3.49% | -3.90% | -3.87% |
| sollevante_1080p (M) | CRF | -14.56% | -15.18% | -15.30% |
| meridian_2160p (M) | CRF | -16.26% | -18.13% | -18.39% |
| tears_1080p (L) | CRF | -11.39% | -12.54% | -12.76% |
| bbb_1080p (L) | CRF | -6.28% | -9.51% | -10.53% |
| sintel_2160p (L) | CRF | -19.56% | -20.75% | -20.80% |
| chimera_2160p (M) | VBR | -1.04% | -0.76% | -0.19% |
| sollevante_1080p (M) | VBR | -9.69% | -10.42% | -10.42% |
| meridian_2160p (M) | VBR | -16.36% | -17.98% | -17.82% |
| tears_1080p (L) | VBR | -9.69% | -10.47% | -10.66% |
| bbb_1080p (L) | VBR | -4.41% | -6.75% | -7.10% |
| sintel_2160p (L) | VBR | -16.60% | -16.75% | -16.65% |

20 is `medium`'s depth from 720p up. Thirty frames gains up to a point on some cells and loses ground on Chimera's VBR curve. An earlier sweep from 8 to 40 frames on the development set put the knee at 20. Past that point a deeper window costs more time than it buys.

## VBR speed

The VBR wall counts toward the bar at G3 only. Each 2-minute and 10-minute cell runs one rung at x265's CRF 28 bitrate with the maxrate at 1.5 times that target and the buffer at twice it. Because there's no size match here, the table shows both achieved rates.

| cell | tier | target kbit/s | ratio | ours vs target | x265 vs target |
|---|---|--:|--:|--:|--:|
| chimera_1080p | M | 1668 | 0.67 | -4.3% | -14.5% |
| sollevante_1080p | M | 7863 | 0.67 | -3.9% | -7.1% |
| meridian_1080p | M | 318 | 0.69 | -4.1% | -5.4% |
| chimera_2160p | M | 4515 | 0.69 | -5.4% | -14.5% |
| sollevante_2160p | M | 20733 | 0.81 | -4.0% | -8.2% |
| meridian_2160p | M | 949 | 0.65 | -2.9% | -4.4% |
| tears_720p | L | 628 | 0.44 | +0.1% | +1.6% |
| tears_1080p | L | 1279 | 0.66 | +0.4% | +1.6% |
| tears_2160p | L | 3389 | 0.73 | +0.2% | +1.4% |
| meridian_720p | L | 205 | 0.76 | -2.5% | -2.6% |
| meridian_1080p | L | 438 | 0.77 | -2.1% | -3.0% |
| meridian_2160p | L | 1326 | 0.68 | -1.6% | -3.2% |
| stem2_720p | L | 567 | 0.48 | -5.3% | -10.3% |
| stem2_1080p | L | 1145 | 0.62 | -5.1% | -11.2% |
| stem2_2160p | L | 2964 | 0.69 | -5.5% | -9.9% |
| sintel_720p | L | 523 | 0.48 | -7.3% | -3.4% |
| sintel_1080p | L | 1113 | 0.70 | -5.2% | -3.0% |
| sintel_2160p | L | 4329 | 0.87 | -1.6% | -2.1% |
| bbb_720p | L | 620 | 0.59 | -1.2% | -2.8% |
| bbb_1080p | L | 1365 | 0.78 | -1.1% | -3.9% |
| bbb_2160p | L | 3453 | 0.76 | -0.9% | -4.1% |
| cosmos_720p | L | 734 | 0.46 | -2.4% | -0.3% |
| cosmos_1080p | L | 1407 | 0.68 | -3.0% | -0.8% |
| nocturne_720p | L | 1022 | 0.48 | -0.8% | +4.2% |
| nocturne_1080p | L | 2198 | 0.60 | -1.2% | +2.6% |
| nocturne_2160p | L | 9142 | 0.54 | -1.4% | -0.6% |

The slowest VBR cell is Sintel at 4K, at 0.87.

## End credits

The scored cells stop before the end credits because text on a flat background saturates VMAF and makes a BD fit swing. Each title's credits still get a row, so a weakness there stays visible. They're coded on their own at 1080p. x265 runs at CRF 28 and yah265 at the cell's matched CRF. The rows compare the file size each encoder needs for equal PSNR-Y. These rows aren't a gate.

| title | yah265's size at x265's PSNR-Y | reading |
|---|--:|---|
| Tears of Steel | +5.3% (extrapolated) | behind |
| Meridian | -56.8% | ahead |
| StEM2 | +16.4% | behind |
| Sintel | -33.6% | ahead |
| Big Buck Bunny | -6.6% (extrapolated) | the two curves don't overlap |
| Cosmos Laundromat | -21.4% | ahead |
| Nocturne | +13.7% | behind |

yah265 is behind on three of the seven credit rolls. It's open work.

## Not measured yet

Tier C hasn't been measured. Besides four sports clips and three screen recordings, it holds a scan of the 1924 film Lash of the Whip. Once it's read, its quality will count toward parity. Its speed will be shown but won't count.
