---
title: A skeptic's questions - yah265
description: Short answers to the hard questions about yah265's comparison with x265. They cover what was tuned, what was left out, how x265 ran and how to check a number.
---

# A skeptic's questions

These are the questions I'd ask if someone else published this site.

## Does yah265 use any x265 code?

No. I wrote yah265 from the ITU-T H.265 specification and from published papers. No third-party encoder code is used. I did reuse code from my own [yah264](https://terranvigil.github.io/yah264/) project. x265 was used for comparison only.

## Which x265, and how was it run?

x265 4.3 at `--preset medium`. [How x265 was run](x265-run.md) includes the build and the command for each goal.

## Is the headline cherry-picked?

No. It covers every CRF cell in the test set. Capped VBR and the content-type clips have their own tables. I didn't include cells that failed a [validity check](methodology.md#what-makes-a-number-valid).

## You tuned for the metric you score with.

Yes. yah265's `medium` is tuned for VMAF v1, the only quality metric these pages judge by. x265's `medium` has other aims. So a lead here is a lead on that one score. [Results](results.md#side-by-side-at-the-same-size) also shows frame crops so you can judge by eye.

## You tuned on the clips you test on.

Yes. Nothing in the test set is held out, because I developed on parts of the same titles. Treat the results as what the encoder does on this set. The test that matters is your own content. [Check it yourself](check-it-yourself.md) walks through it.

## Does the lead hold beyond medium?

I only claim `medium` against `medium`, the preset most people run. The [guide](guide.md#pick-a-preset) explains why yah265 has four presets.

## Why time x265 without its assembly?

Two of the three goals compare the algorithms with SIMD off on both sides. The headline uses only the third, where both encoders run as shipped.

## Did x265 get its tunes?

Neither encoder gets a `--tune`. yah265's `medium` detects screen recordings and handheld footage by itself. x265 has tunes for grain and animation that I didn't use.

## Is it only fast on a Mac?

No. The headline is the Mac's. yah265 also leads on an Intel Xeon and on an AWS Graviton4, where the same test set ran. On the Xeon I also held both encoders to AVX2, then to SSE4.2. [Results](results.md#by-machine) has each machine's table.

## Does the average hide bad frames?

It can. [Results](results.md#the-worst-frames) shows one clip frame by frame, with the worst 5% of its frames beside the mean. It also compares each short clip's lowest frame. At CIF, yah265's lowest frame is behind x265's.

## Is the VMAF setup standard?

The scorer is Netflix's libvmaf with the `vmaf_v1.0.16` models. A picture smaller than 1080p is scaled up to display size before scoring, as Netflix's guidance for that model says. [The methodology](methodology.md#scoring-quality) has the exact setup.

## How do I check a number?

Each chart's caption links to the chart's data as CSV and names the script that drew the chart. The tables come from the same data. To take a measurement yourself, start with [Check it yourself](check-it-yourself.md).
