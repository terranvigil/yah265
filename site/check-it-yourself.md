---
title: Check it yourself - yah265
description: Rerun a speed, size and quality reading of yah265 against x265 on your own machine, one clip at a time or with the full harness.
---

# Check it yourself

Everything on the [results page](results.md) comes from scripts in the repository. This page shows how to take the same kind of reading on your own machine. It goes from one clip by hand to the full harness.

The harness at the end of this page reruns whole tables. It expects the project's own media library.

## What you need

- yah265. Build it with `make build-rel`, as [Build and first encode](start.md) describes.
- x265 as the reference. The results use the Homebrew build of x265 4.3. Because an older x265 on arm64 has much less assembly, it makes a G3 reading look better than it is.
- ffmpeg, to make input and to decode.
- libde265's `dec265`, for the conformance check.
- For quality, a libvmaf 3.x `vmaf` command line with Netflix's `vmaf_v1.0.16` model. The scripts look for the tool at `~/.local/opt/vmaf-v1/bin/vmaf`, or wherever `V1_BIN` points. They look for the models under `~/.local/share/vmaf/model`.

You also need a clip. This command downloads seven standard CIF test clips from Xiph into `tests/corpus/`. It checks each against a pinned hash:

```sh
$ scripts/fetch_corpus.sh
```

Add `--full` for the larger 720p and 1080p clips.

## Is it correct?

```sh
$ make test
$ make conformance-fast
```

The first command runs the unit tests and the repository's own checks. The second encodes a set of test streams, then decodes each one with ffmpeg and with `dec265`. Both decoders must reproduce the encoder's own reconstruction exactly. `make conformance` runs the full set.

## One clip by hand

A reading compares the two encoders at the same file size. It then asks which one finished first and which one scores higher on VMAF v1.

<figure class="diagram">
<svg class="d-wide" viewBox="0 0 720 250" role="img" aria-labelledby="ciy-t ciy-d">
<title id="ciy-t">The steps of one reading</title>
<desc id="ciy-d">x265 encodes the clip at CRF 28. yah265 encodes it at a trial CRF. If the two files aren't within 1% in size, the CRF moves and yah265 encodes again. Once they match, the two encoders are timed in interleaved rounds and their outputs are scored with VMAF v1.</desc>
<rect class="d-box" x="10" y="30" width="150" height="60" rx="6"/>
<text class="d-text" x="85" y="56" text-anchor="middle">x265</text>
<text class="d-label" x="85" y="76" text-anchor="middle">at CRF 28</text>
<line class="d-arrow" x1="160" y1="60" x2="208" y2="60"/>
<rect class="d-accent" x="210" y="30" width="150" height="60" rx="6"/>
<text class="d-text" x="285" y="56" text-anchor="middle">yah265</text>
<text class="d-label" x="285" y="76" text-anchor="middle">at a trial CRF</text>
<line class="d-arrow" x1="360" y1="60" x2="408" y2="60"/>
<rect class="d-fill" x="410" y="30" width="170" height="60" rx="6"/>
<text class="d-text" x="495" y="56" text-anchor="middle">Same size?</text>
<text class="d-label" x="495" y="76" text-anchor="middle">within 1% of x265's file</text>
<path class="d-arrow" d="M495,30 V10 H285 V28"/>
<text class="d-label" x="390" y="24" text-anchor="middle">no: move the CRF</text>
<line class="d-arrow" x1="495" y1="90" x2="495" y2="148"/>
<text class="d-label" x="505" y="124">yes</text>
<rect class="d-box" x="250" y="150" width="210" height="70" rx="6"/>
<text class="d-text" x="355" y="180" text-anchor="middle">Time both</text>
<text class="d-label" x="355" y="202" text-anchor="middle">interleaved rounds, median</text>
<rect class="d-box" x="500" y="150" width="210" height="70" rx="6"/>
<text class="d-text" x="605" y="180" text-anchor="middle">Score both</text>
<text class="d-label" x="605" y="202" text-anchor="middle">VMAF v1 against the source</text>
<path class="d-arrow" d="M495,120 H355 V148"/>
<path class="d-arrow" d="M495,120 H605 V148"/>
</svg>
<svg class="d-narrow" viewBox="0 0 360 346" role="img" aria-labelledby="ciy-t-n ciy-d-n">
<title id="ciy-t-n">The steps of one reading</title>
<desc id="ciy-d-n">x265 encodes the clip at CRF 28. yah265 encodes it at a trial CRF. If the two files aren't within 1% in size, the CRF moves and yah265 encodes again. Once they match, the two encoders are timed in interleaved rounds and their outputs are scored with VMAF v1.</desc>
<rect class="d-box" x="60" y="8" width="180" height="50" rx="6"/>
<text class="d-text" x="150" y="30" text-anchor="middle">x265</text>
<text class="d-label" x="150" y="48" text-anchor="middle">at CRF 28</text>
<line class="d-arrow" x1="150" y1="58" x2="150" y2="82"/>
<rect class="d-accent" x="60" y="84" width="180" height="50" rx="6"/>
<text class="d-text" x="150" y="106" text-anchor="middle">yah265</text>
<text class="d-label" x="150" y="124" text-anchor="middle">at a trial CRF</text>
<line class="d-arrow" x1="150" y1="134" x2="150" y2="158"/>
<rect class="d-fill" x="40" y="160" width="220" height="50" rx="6"/>
<text class="d-text" x="150" y="182" text-anchor="middle">Same size?</text>
<text class="d-label" x="150" y="200" text-anchor="middle">within 1% of x265's file</text>
<path class="d-arrow" d="M260,185 H284 V109 H242"/>
<text class="d-label" x="292" y="140">no: move</text>
<text class="d-label" x="292" y="156">the CRF</text>
<line class="d-line" x1="150" y1="210" x2="150" y2="240"/>
<text class="d-label" x="158" y="230">yes</text>
<path class="d-arrow" d="M150,240 H90 V266"/>
<path class="d-arrow" d="M150,240 H270 V266"/>
<rect class="d-box" x="8" y="268" width="164" height="70" rx="6"/>
<text class="d-text" x="90" y="292" text-anchor="middle">Time both</text>
<text class="d-label" x="90" y="312" text-anchor="middle">interleaved rounds,</text>
<text class="d-label" x="90" y="328" text-anchor="middle">median</text>
<rect class="d-box" x="188" y="268" width="164" height="70" rx="6"/>
<text class="d-text" x="270" y="292" text-anchor="middle">Score both</text>
<text class="d-label" x="270" y="312" text-anchor="middle">VMAF v1 against</text>
<text class="d-label" x="270" y="328" text-anchor="middle">the source</text>
</svg>
<figcaption>The size match comes first. Speed and quality are read only after it holds.</figcaption>
</figure>

Start with x265 at its default rate factor of CRF 28. It reads the clip on standard input:

```sh
$ cat clip.y4m | x265 --input - --y4m --preset medium --crf 28 -o ref.hevc
```

Then encode with yah265, moving `--crf` until its file is within 1% of x265's:

<!-- recipe: after check-it-yourself:104 -->
```sh
$ cat clip.y4m | build-rel/y265 - -o ours.hevc --crf 24
$ ls -l ref.hevc ours.hevc
```

Because yah265's CRF scale is its own, the matching number won't be 28.

Once the sizes match, time both runs:

```sh
$ time (cat clip.y4m | build-rel/y265 - -o ours.hevc --crf 24)
$ time (cat clip.y4m | x265 --input - --y4m --preset medium --crf 28 -o ref.hevc)
```

Divide x265's `real` time by yah265's. That's the G3 speed the results page reports. Over 1.00 means yah265 finished first.

For G1, run each encoder in plain C on one thread:

```sh
$ cat clip.y4m | N265_ASM_OFF=all build-rel/y265 - -o ours.hevc --crf 24 --threads 1
$ cat clip.y4m | x265 --input - --y4m --preset medium --crf 28 \
    --no-asm --pools none --frame-threads 1 -o ref.hevc
```

G2 drops `--threads 1` and the two x265 threading options. x265 uses more than one thread unless it gets both `--pools none` and `--frame-threads 1`.

x265's pictures change with its frame-thread count. Because the one-thread run writes a different file, yah265 needs its own size match for G1.

A few habits keep the number fair:

- Run on a quiet machine.
- Take the median of several rounds that interleave the two encoders.
- Feed both from the same source the same way.
- Don't settle anything on one run. A speed ratio can move by a few hundredths between runs.

## Quality across the rate range

```sh
$ scripts/v1_ladder.py clip.y4m
$ scripts/v1_ladder.py clip.y4m --vbr
```

The script codes a five-rung ladder on both encoders, at CRF 20, 23, 26, 29 and 32. It then prints yah265's BD-rate on VMAF v1 against x265 medium, where negative means yah265 needs fewer bits for the same score. With `--vbr` both encoders run at the bitrates x265 reached on those rungs, where the maxrate is 1.5 times the target and the buffer is twice the target. Scoring follows the [methodology](methodology.md#scoring-quality).

This is the CRF ladder on the foreman CIF clip. It took 50 seconds on the Mac.

```
$ scripts/v1_ladder.py foreman_cif.y4m
foreman_cif.y4m ours crf : BD-v1 -9.66%
    kbps      v1  x265@r    gap | v1 gained by taking x265's:    cambi   chroma     adm3  motion3
     101  42.049  outside x265's rates
     154  55.999  54.712 +1.288 |   +0.018   -2.101   +0.796   -0.000
     235  67.192  64.779 +2.413 |   +0.005   -2.643   +0.225   +0.000
     371  76.654  73.757 +2.898 |   +0.003   -2.912   +0.011   +0.000
     584  83.686  81.264 +2.422 |   +0.001   -2.185   -0.240   +0.000
```

Each row is one yah265 rung, with x265's score read off its curve at the same bitrate. For the last four columns the script swaps one of v1's features at a time from x265's frames onto yah265's. Each column reports what that swap is worth.

Encodes and scores are cached under `~/.cache/yah265-v1`. `THREADS` sets the thread count for each encoder and for libvmaf. It defaults to 4. `BIN` overrides the yah265 binary.

## Reading size and quality together

A speed ratio means nothing until the sizes match, because an encoder that writes a larger file had more bits to spend. It should score higher for that reason alone. Check the 1% size match before reading anything else.

At the matched size, compare the two VMAF v1 scores directly. One clip's lead says little about the next clip's. That's why the results page reports the median over a group and the clip with the smallest lead.

One matched point is one bitrate. Read the ladder too, because an encoder can lead at CRF 28 and trail at high quality. BD-rate folds the whole ladder into one number. That number covers only the scores both curves reach. Where one encoder's lowest score is above the other's highest, there's no BD-rate to quote. The curves themselves are the answer.

Under VBR, check the bitrate each encoder reached against its target before comparing quality. An encoder that undershoots its target saves bits it was asked to spend.

## The full harness

`scripts/parity_spec.py` takes the whole parity reading. It splits into two halves. The untimed half can share a machine. The timed half needs the machine quiet.

| mode | what it does |
|---|---|
| `--solve` | encodes the x265 anchors, solves yah265 onto their sizes and scores both |
| `--time` | times G1, G2 and G3 at the solved points in interleaved rounds |
| `--windows`, `--time-windows` | matches and times the 30-second windows on the long cells |
| `--ladder` | the CRF and VBR ladders on the 2-minute, 10-minute and content-type cells |
| `--sweep` | the lookahead depth sweep |
| `--vbr-wall` | the timed VBR leg |
| `--credits` | the end-credits rows |
| `--report` | writes the report from the recorded rows |
| `--plan` | prints every command and the estimated hours without running anything |

`--tier`, `--classes`, `--cells` and `--goals` narrow a run. Print the plan before anything else:

```sh
$ scripts/parity_spec.py --plan --tier S
```

Because the harness reads the project's shared test library at fixed paths, another machine needs the same layout. The recorded rows behind the results are in `docs/data/`. The report counts a ladder only when it came from the binary under test. A report rebuilt with a different binary shows those curves as pending.

To check that the results page still matches those rows:

```sh
$ scripts/charts_results.py --check
```
