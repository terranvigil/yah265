---
title: Getting started - yah265
description: Build yah265, run your first encode, and pick a preset and a rate mode.
---

# Getting started

## Build it

You need a C11 compiler (clang is the reference), Meson, Ninja and Python 3. ffmpeg is worth having too. It makes input and decodes what comes out.

```
git clone https://github.com/terranvigil/yah265
cd yah265
make build-rel
```

That leaves a release build of the encoder at `build-rel/n265`. `make build` instead gives a `debugoptimized` tree under `build/` with the assertions on. The two write the same bytes. The release build is the one every timed figure on this site uses.

There's one binary for every bit depth. It links an 8-bit library and a 10-bit one and picks from the input.

## Your first encode

```
build-rel/n265 input.y4m -o out.hevc --crf 23
```

The output is a raw Annex-B HEVC stream. ffmpeg plays it, decodes it, and muxes it into MP4 or Matroska:

```
ffmpeg -i out.hevc -c copy out.mp4
```

The input can be a Y4M file, raw planar YUV, or `-` for standard input. A pipe from ffmpeg takes any source ffmpeg can read:

```
ffmpeg -i movie.mov -pix_fmt yuv420p -f yuv4mpegpipe - | build-rel/n265 - -o out.hevc --crf 23
```

Because raw YUV has no header, you give the geometry yourself with `--input-res`, `--input-csp`, `--fps` and `--input-depth`. Without a rate flag the encoder codes a fixed QP of 26. Most work wants `--crf`.

## Profiles

The input's chroma format and sample width pick the profile. No flag says so.

| input | stream |
|---|---|
| 4:2:0, 8-bit | Main |
| 4:2:0, 10-bit | Main10 |
| 4:4:4, 8-bit | Main 4:4:4 |
| 4:4:4, 10-bit | Main 4:4:4 10 |
| 4:2:2, 8 or 10-bit | Main 4:2:2 10 |

`--output-depth 10` codes 8-bit input as Main10. Going the other way isn't offered. `--lossless` codes a mathematically lossless stream.

## Presets

`--preset` takes x265's names. Four of them are built: `fast`, `medium`, `slow` and `slower`. `medium` is the default and the preset every result on this site is read at. It's also resolution-aware. At 720p and up it looks 20 frames ahead, where CIF looks 12. From 1080p up it codes 64x64 coding trees, where smaller pictures use 32x32.

x265's other six names are known and refused. The error names the nearest preset that exists. `faster`, `veryfast`, `veryslow` and `placebo` were each built and measured against the x265 preset of the same name. None of them earned it.

`slow` and `slower` spend more time for more quality than `medium`. They've only been placed against x265's `medium`. Read against x265's own `slow` and `slower`, they don't yet win.

A preset only sets defaults. Any option you give wins over it. To see what a run will actually do, ask the encoder:

```
build-rel/n265 input.y4m --preset medium --show-config
```

That prints the resolved configuration and exits. `--verbose` prints the same table at the start of a real encode.

`--tune zerolatency` turns off B-frames and the lookahead for one-frame latency. The encoder refuses the other x265 tune names and gives a reason for each.

## Rate modes

| mode | options | what it does |
|---|---|---|
| CRF | `--crf 23` | constant quality, the size follows the content |
| fixed QP | `--qp 26` | one quantizer for every frame, mostly for testing |
| ABR | `--bitrate 3000` | a mean bitrate in kbit/s, with no buffer |
| VBR | `--bitrate 3000 --vbv-maxrate 4500 --vbv-bufsize 6000` | a mean bitrate held under a decoder buffer |
| CBR | VBR's options with `--strict-cbr` | a constant rate. Filler data pads any picture that comes in under its share |
| capped CRF | `--crf 23 --vbv-maxrate 4500 --vbv-bufsize 6000` | constant quality that never breaks the buffer |
| two-pass | `--pass 1 --stats s.log`, then `--pass 2 --stats s.log` | a first pass measures the clip and the second spends the bits |

The CRF scale is yah265's own. A given number doesn't give the same file size as x265 at that number. The scale accounts for the frame rate. It moves 1.8 QP each time the frame rate doubles.

With a VBV, the stream includes HRD parameters and buffering-period SEI. `--crf-min`, `--crf-max`, `--qpmin` and `--qpmax` bound the rate control. `--zones` and `--qpfile` steer it by frame range.

## Threads

`--threads 0` is the default and starts one worker per online CPU. At `--threads 1`, two runs of the same command give the same bytes. Above one thread the output is the same at every thread count, except for VBR with a buffer. There the rate control works with several frames in flight and the stream differs from the one-thread stream.

`--gop-parallel <n>` codes a long input as segments on several encoder instances at once and joins them at key frames. It's opt-in. The joined stream differs from the serial one.

## Options worth knowing

| option | what it does |
|---|---|
| `--keyint <n>` | key frame period in frames (default 250). A scene cut restarts the count |
| `--open-gop` / `--no-open-gop` | a CRA at the period with leading pictures (default), or an IDR |
| `--bframes <n>` | B-frames between anchors, 0 to 15 (default 7) |
| `--ref <n>` | references per list, 1 to 5 |
| `--rc-lookahead <n>` | lookahead depth in frames |
| `--rdoq-level <0-2>` | rate-distortion optimized quantization: off, at the final encode, or in every decision |
| `--cbqpoffs` / `--crqpoffs` | chroma quantizer offsets from luma |
| `--psnr`, `--ssim` | report the metric at the end of the run |
| `--csv <file>` | per-run or per-frame statistics |
| `--dump-recon <file>` | write the reconstruction as raw YUV |
| `--repeat-headers` | parameter sets at every key frame, for a receiver that joins mid-stream |

`build-rel/n265 --help` lists everything.

## The identification marks

Every stream has an SEI message that names the encoder, its version, its license and the options the run resolved. It holds no clock, no path and no thread count. `--no-info` drops it.

The stream also has a fingerprint that survives when the SEI is stripped. Where the merge candidate list holds the same motion at its last two positions, the encoder picks which index to write by a fixed rule. The mark costs no bits because the two codings are the same length and decode to the same picture. `--no-fingerprint` turns it off. `scripts/fingerprint_check.py` reads both marks back from a stream.

## Environment knobs

Settings named `N265_*` in the environment reach tuning parameters the command line doesn't expose. They're for experiments. A preset sets most of them. `--show-config` prints the ones a preset named and marks any your environment overrode. The generated [knob list](docs/knobs.md) names them.
