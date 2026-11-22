---
title: Try it on your own clip - yah265
description: One command that runs yah265 and x265 on your video at the same file size and reports speed and VMAF v1 for both.
---

# Try it on your own clip

The results on this site come from a test set I picked. Because I picked it, a fair question is whether yah265 only wins on those clips. `y265 bench` lets you check that on any video you have.

```sh
$ y265 bench holiday.mp4
```

It takes a sample from your file and encodes it with yah265 and with x265 at `--preset medium`. It moves yah265's CRF until the two files are the same size. Then it times the two encoders and scores their output with VMAF v1. At the end you get a short table and one sentence that says who won. You also get a result card you can post and a page that plays the two encodes side by side.

## What you need

`y265 bench --check` lists what it found, with an install line for anything missing.

| tool | why | install |
|---|---|---|
| ffmpeg and ffprobe | to cut the sample and decode | `brew install ffmpeg` or `sudo apt install ffmpeg` |
| x265 | the encoder yah265 is compared with | `brew install x265` or `sudo apt install x265` |
| libvmaf's `vmaf` tool | to score quality | build it from Netflix's [vmaf repository](https://github.com/Netflix/vmaf) with `-Denable_float=true` |
| the `vmaf_v1.0.16` models | the VMAF v1 model files | copy `model/vmaf_v1.0.16` and `model/vmaf_v1.0.16_hfr` from the same repository |

Without the x265 command line, the bench uses ffmpeg's libx265 when ffmpeg has it. Python 3 runs the bench itself and needs no extra packages.

## How it keeps the comparison fair

The rules are the ones on the [methodology](methodology.md) page, cut down to one clip:

- x265 encodes first, at CRF 28, its default. yah265's CRF is searched until its file is within 1% of x265's size. Speed and quality are both read at that one size.
- The two encoders read the same sample file. They run in alternating rounds with the order flipped each time. The bench keeps the median time.
- They get the same number of threads. By default each one uses all cores because that's how most people run them.
- VMAF v1 scores every frame, the way the results page scores them.

The sentence at the end is written from the numbers. If yah265 is slower or scores lower on your clip, it says "slower" or "behind". If the machine was busy when the bench started, it says the speed number is rough. I'd rather you see a loss than a result you can't trust.

A busy machine moves the speed number far more than the quality number, so close other heavy work before you run it.

## How long it takes

By default the bench takes 20 seconds from the middle of your file, because opening titles and end credits say little about an encoder. A 20-second 1080p sample probably takes a few minutes on a laptop. Most of that time goes to the size search and the VMAF scoring.

| option | what it does |
|---|---|
| `--seconds 60` | a longer sample |
| `--start 300` | start the sample 300 seconds in |
| `--full` | the whole file |
| `--threads 8` | 8 threads for each encoder |
| `--crf 23` | match at x265's CRF 23 instead of 28 |
| `--rounds 5` | more timed rounds |
| `--out DIR` | where the results go |

## What you get

The results folder holds both encodes and a JSON file with every command and every number. It also holds the result card as an SVG and a player page. Open the player in Safari, or in Chrome or Edge on a machine with an HEVC decoder. Drag the line across the picture to compare the two encodes. The chart under the picture plots VMAF v1 for every frame. Click the chart to jump to a frame.

A run on a 720p clip on my Mac printed this. The Mac was busy with other work at the time. That makes the speed line an example of a result the bench won't vouch for.

```
y265 bench: ducks_720p, 1280x720, the whole file (10.00 s), both --preset medium, 8 threads each (yah265 --threads 8, x265 --pools 8)

                           yah265           x265
  version                   0.0.1  4.3+1-e9b8812
  rate                   CRF 27.2         CRF 28
  size                   11.48 MB       11.47 MB
  wall (median)           11.45 s         9.53 s
  speed                  43.7 fps       52.5 fps
  cpu time                 39.6 s         55.2 s
  VMAF v1 mean              78.01          73.88
  VMAF v1 5th pct           65.16          63.81
  VMAF v1 min               62.83          61.88

yah265 was slower than x265 (0.83x its speed) and 4.13 VMAF v1 points ahead, at the same file size (+0.10%). The machine was busy (load 31.5 on 18 cores), so the speed number is rough.
```

One clip and one file size are a small sample. The [results](results.md) page has the median over many clips and the full rate range. If your clip shows something different, I'd like to see it.
