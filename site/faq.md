---
title: A skeptic's questions - yah265
description: Short answers to the hard questions about yah265's comparison with x265. They cover what was tuned, what was left out, how x265 ran and how to check a number.
---

# A skeptic's questions

These are the questions I'd ask if someone else published this site.

## Is this x265 with the names changed?

No. I wrote yah265 from the ITU-T H.265 specification and from published papers. No third-party encoder code is used. I did reuse code from my own [yah264](https://terranvigil.github.io/yah264/) project. x265 was used for comparison only.

## Which x265, and how was it run?

x265 4.3 at `--preset medium`. [How x265 was run](x265-run.md) includes the build and the command for each goal.

## Is the headline cherry-picked?

No. It covers every CRF cell in the test set that was timed on the Mac as shipped, provided its two files match in size within 1%. Capped VBR has its own tables because its files share a target bitrate where the headline cells share a size. The content-type clips are timed in their own table beside their quality. A cell that breaks a validity rule is marked in the report and stays out of every statistic, whichever way it points. Every figure on this site also leaves out the reports' rows for two titles that aren't in the set. [The methodology](methodology.md#what-makes-a-number-valid) has the rules.

## You tuned for the metric you score with.

Yes. yah265's `medium` is tuned for VMAF v1, the only quality metric these pages judge by. x265's `medium` has other aims that include its psychovisual options. A lead here is a lead on that one score. Because a score can't settle how two encodes look, [Results](results.md#side-by-side-at-the-same-size) shows frame crops that a script picked by a fixed rule.

## You tuned on the clips you test on.

Yes. The test set has no held-out content, because the clips I developed on were windows of the same titles. Treat the results as what the encoder does on this set. The test that matters is your own content. [Check it yourself](check-it-yourself.md) walks through it.

## Does the lead hold beyond medium?

I only claim `medium` against `medium`, the preset most people run. Every speed and quality result on this site compares those two. The [guide](guide.md#pick-a-preset) explains why yah265 keeps four presets.

## Why time x265 without its assembly?

Two of the three goals switch the hand-written SIMD off on both sides to compare the algorithms alone. They're engineering checks, because x265 isn't meant to run that way. The headline uses only the third goal, where both encoders run as shipped.

## Did x265 get its tunes?

Neither encoder gets a `--tune`. yah265's `medium` detects screen recordings and handheld footage by itself. Because no reading uses x265's tunes for grain and animation, the content-type clips compare one set of defaults with another. Their speed is shown without counting toward the verdict.

## Is it only fast on a Mac?

No. The headline is the Mac's. yah265 also leads on an Intel Xeon and on an AWS Graviton4, where the same test set ran. On the Xeon I also held both encoders to AVX2, then to SSE4.2. [Results](results.md#by-machine) has each machine's table.

## Does the average hide bad frames?

It can. [Results](results.md#the-worst-frames) shows one clip frame by frame for that reason. The worst 5% of its frames sit beside the mean. It also compares each short clip's lowest frame, including the size where yah265's lowest frame is behind.

## Is the VMAF setup standard?

The scorer is Netflix's libvmaf with the `vmaf_v1.0.16` models. A picture smaller than 1080p is scaled up to display size before scoring, as Netflix's guidance for that model says. [The methodology](methodology.md#scoring-quality) names the commit, the models and the frame stride.

## How do I check a number?

Under every chart a line names its source file and the script that drew it, beside a link to its data as CSV. A script writes every table from the same files. The site's build fails when a page disagrees with the data. To take a reading yourself, start with [Check it yourself](check-it-yourself.md).
