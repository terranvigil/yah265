---
title: Check it yourself - yah265
description: Rerun a speed, size and quality reading of yah265 against x265 on your own machine.
---

# Check it yourself

Everything on the [results page](results.md) comes from scripts in the repository. This page shows how to take the same kind of reading on your own machine, from one clip by hand up to the full harness.

## What you need

- yah265, built with `make build-rel` ([getting started](start.md) has the details).
- x265, the reference. The results use the Homebrew build of x265 4.3. An older x265 on arm64 has much less assembly and makes a G3 reading look better than it is.
- ffmpeg, to make input and to decode.
- libde265's `dec265`, for the conformance check.
- For quality, a libvmaf 3.x `vmaf` command line with Netflix's `vmaf_v1.0.16` model. The scripts look for it at `~/.local/opt/vmaf-v1/bin/vmaf`, or wherever `V1_BIN` points, with the models under `~/.local/share/vmaf/model`.

`scripts/fetch_corpus.sh` downloads seven standard CIF test clips from Xiph and checks each against a pinned hash. `--full` adds the larger 720p and 1080p clips.

## Is it correct?

```
make test
make conformance-fast
```

`make test` runs the unit tests and the repository's own checks. `make conformance-fast` encodes a set of vectors and decodes each with ffmpeg and with `dec265`. Both decoders must reproduce the encoder's reconstruction exactly. `make conformance` runs the full set.

## One clip by hand

A reading compares the two encoders at the same file size. Start with x265 at its default rate factor, CRF 28, reading the clip on standard input:

```
cat clip.y4m | x265 --input - --y4m --preset medium --crf 28 -o ref.hevc
```

Then encode with yah265 and move `--crf` until its file is within 1% of x265's:

```
cat clip.y4m | build-rel/n265 - -o ours.hevc --crf 24
ls -l ref.hevc ours.hevc
```

yah265's CRF scale is its own, so the matching number won't be 28. Once the sizes match, time both runs with `/usr/bin/time -p` and divide our wall time by x265's. That's the G3 ratio. For G1, run each encoder in pure C on one thread:

```
cat clip.y4m | N265_ASM_OFF=all build-rel/n265 - -o ours.hevc --crf 24 --threads 1
cat clip.y4m | x265 --input - --y4m --preset medium --crf 28 --no-asm --pools none --frame-threads 1 -o ref.hevc
```

G2 drops `--threads 1` and the two x265 threading options. Without `--pools none --frame-threads 1`, x265 quietly uses more than one thread.

x265's pictures change with its frame-thread count. So the one-thread run writes a different file, and yah265 needs its own match for G1.

A few habits keep the number fair. Run on a quiet machine. Interleave the two encoders over several rounds and take the median. Feed both from the same source the same way. A speed ratio can move by a few hundredths between runs, so don't settle anything on one.

## Quality across the rate range

```
scripts/v1_ladder.py clip.y4m
scripts/v1_ladder.py clip.y4m --vbr
```

`v1_ladder.py` codes a five-rung ladder at CRF 20, 23, 26, 29 and 32 on both encoders and prints yah265's BD-rate on VMAF v1 against x265 medium. Negative means yah265 needs fewer bits for the same quality. `--vbr` runs both encoders at x265's achieved rates instead, with maxrate 1.5 times the target and a buffer of twice it. Scoring follows the [methodology](methodology.md): every frame, at display resolution, with CAMBI told the coded size.

The script also says which of v1's four features makes up the gap. It swaps one feature at a time from x265's frames onto ours and reports what each swap is worth. Encodes and scores are cached under `~/.cache/yah265-v1`. `THREADS` sets the thread count for each encoder and for libvmaf. It defaults to 4.

## The full harness

`scripts/parity_spec.py` takes the whole parity reading. It splits into an untimed half that can share a machine and a timed half that needs it quiet:

| mode | what it does |
|---|---|
| `--solve` | encodes the x265 anchors, solves yah265 onto their sizes and scores both |
| `--time` | times G1, G2 and G3 at the solved points, rounds interleaved |
| `--windows`, `--time-windows` | matches and times the 30-second windows on the long cells |
| `--ladder` | the CRF and VBR ladders on tiers M and L |
| `--sweep` | the lookahead depth sweep |
| `--vbr-wall` | the timed VBR leg |
| `--credits` | the end-credits rows |
| `--report` | writes the report from the recorded rows |
| `--plan` | prints every command and the estimated hours, and runs nothing |

`--tier`, `--classes`, `--cells` and `--goals` narrow a run. The harness reads the project's shared test library at fixed paths, so another machine needs the same layout. The recorded rows behind the results are in `docs/data/`. The report counts a ladder only when it came from the binary under test, so a report rebuilt with a different binary shows those curves as pending.
