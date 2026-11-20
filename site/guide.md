---
title: Encoding guide - yah265
description: How to pick a rate mode, a preset and a thread count in yah265. It has a recipe for each rate mode and a list of what ships.
---

# Encoding guide

This page is for someone putting yah265 into a pipeline. Commands use `y265`, the project's own command line. The same options work from ffmpeg, as [From ffmpeg](#from-ffmpeg) shows. [Options](options.md) lists every option with its default.

## Rate modes

A rate mode decides what stays fixed: the quality, the bitrate or the size.

| mode | options | use it when |
|---|---|---|
| CRF | `--crf` | you want a quality level and the size can follow the content. It's the right choice for most files |
| capped CRF | `--crf` with `--vbv-maxrate` and `--vbv-bufsize` | a player or a network sets a ceiling on the peak rate and you want CRF under it |
| capped VBR | `--bitrate` with `--vbv-maxrate` and `--vbv-bufsize` | you have a target bitrate and a peak, as on a streaming ladder |
| CBR | capped VBR with `--strict-cbr` | the channel runs at a constant rate, as in broadcast |
| ABR | `--bitrate` alone | you want a mean bitrate and nothing reads the stream through a buffer |
| two-pass | `--pass` with `--stats` | the file has to hit a size. It costs a second encode |
| fixed QP | `--qp` | you're testing the encoder. It's also what you get with no rate option |

<figure class="diagram">
<svg viewBox="0 0 720 330" role="img" aria-labelledby="rate-fig-t rate-fig-d">
<title id="rate-fig-t">Choosing a rate mode</title>
<desc id="rate-fig-d">A decision tree. If you don't need a set bitrate, use CRF, or capped CRF when there is a peak rate limit. If you need a set bitrate and the channel is constant rate, use CBR. Otherwise use two-pass when you can encode twice, capped VBR when a decoder buffer applies, and ABR when none does.</desc>
<rect class="d-fill" x="250" y="10" width="220" height="44" rx="6"/>
<text class="d-text" x="360" y="38" text-anchor="middle">Need a set bitrate?</text>
<path class="d-arrow" d="M250 32H150V96"/>
<text class="d-label" x="200" y="24" text-anchor="middle">no</text>
<path class="d-arrow" d="M470 32H540V96"/>
<text class="d-label" x="505" y="24" text-anchor="middle">yes</text>
<rect class="d-fill" x="50" y="98" width="200" height="44" rx="6"/>
<text class="d-text" x="150" y="126" text-anchor="middle">A peak rate limit?</text>
<line class="d-arrow" x1="100" y1="142" x2="100" y2="196"/>
<text class="d-label" x="88" y="174" text-anchor="end">no</text>
<line class="d-arrow" x1="210" y1="142" x2="210" y2="196"/>
<text class="d-label" x="222" y="174">yes</text>
<rect class="d-accent" x="30" y="198" width="110" height="44" rx="6"/>
<text class="d-text" x="85" y="226" text-anchor="middle">CRF</text>
<rect class="d-accent" x="160" y="198" width="130" height="44" rx="6"/>
<text class="d-text" x="225" y="226" text-anchor="middle">capped CRF</text>
<rect class="d-fill" x="430" y="98" width="220" height="44" rx="6"/>
<text class="d-text" x="540" y="126" text-anchor="middle">Constant-rate channel?</text>
<path class="d-arrow" d="M650 120H690V196"/>
<text class="d-label" x="682" y="174" text-anchor="end">yes</text>
<rect class="d-accent" x="640" y="198" width="70" height="44" rx="6"/>
<text class="d-text" x="675" y="226" text-anchor="middle">CBR</text>
<line class="d-arrow" x1="500" y1="142" x2="500" y2="196"/>
<text class="d-label" x="488" y="174" text-anchor="end">no</text>
<rect class="d-fill" x="390" y="198" width="220" height="44" rx="6"/>
<text class="d-text" x="500" y="226" text-anchor="middle">Time to encode twice?</text>
<line class="d-arrow" x1="420" y1="242" x2="360" y2="274"/>
<text class="d-label" x="372" y="254" text-anchor="end">yes</text>
<rect class="d-accent" x="290" y="276" width="110" height="44" rx="6"/>
<text class="d-text" x="345" y="304" text-anchor="middle">two-pass</text>
<line class="d-arrow" x1="500" y1="242" x2="500" y2="274"/>
<text class="d-label" x="508" y="262">a buffer</text>
<rect class="d-accent" x="430" y="276" width="140" height="44" rx="6"/>
<text class="d-text" x="500" y="304" text-anchor="middle">capped VBR</text>
<line class="d-arrow" x1="590" y1="242" x2="640" y2="274"/>
<text class="d-label" x="626" y="254">no buffer</text>
<rect class="d-accent" x="600" y="276" width="90" height="44" rx="6"/>
<text class="d-text" x="645" y="304" text-anchor="middle">ABR</text>
</svg>
<figcaption>Start from whether the bitrate is set for you.</figcaption>
</figure>

Bitrates and buffer sizes are in kbit/s and kbit. Whenever you give a buffer, the stream describes it in HRD parameters and buffering period SEI so a decoder or a packager can check it.

### CRF

```sh
y265 in.y4m -o out.hevc --crf 23
```

The CRF scale is yah265's own. A number that suited x265 gives a different size here. Because the scale accounts for the frame rate, one number suits a 24 fps film and a 60 fps game capture. Results compares the two encoders [at the same file size](results.md#quality-at-the-same-size).

### Capped CRF

```sh
y265 in.y4m -o out.hevc --crf 23 --vbv-maxrate 4500 --vbv-bufsize 6000
```

The quality holds until a hard scene would overflow the buffer, and then the encoder coarsens that scene to fit. Plan for two things. Because the cap bounds the peak and says nothing about the total, you still can't predict the file size. A `--crf-max` set too low for the buffer asks for an underflow. The encoder reports one at the end of the run.

### Capped VBR

```sh
y265 in.y4m -o out.hevc --bitrate 3000 --vbv-maxrate 4500 --vbv-bufsize 6000
```

Set the peak above the target so the encoder has room to spend on hard scenes. Results shows [how close each title came to its target and how fast it ran](results.md#vbr). Above one thread this mode writes a different stream from `--threads 1`, as [Threads and memory](#threads-and-memory) explains.

### CBR

```sh
y265 in.y4m -o out.hevc --bitrate 3000 --vbv-maxrate 3000 --vbv-bufsize 6000 --strict-cbr
```

`--strict-cbr` marks the stream as constant rate and pads any picture that comes in under its share with filler data. The run reports how much padding it wrote.

### Two-pass

```sh
y265 in.y4m -o /dev/null --pass 1 --stats in.stats --crf 23
y265 in.y4m -o out.hevc --pass 2 --stats in.stats --bitrate 3000
```

The first pass measures every frame and the second plans the whole file before it codes one. Pass 2 needs `--bitrate` and the same source. Pass 1 can run any mode and any preset. Two-pass doesn't combine with `--gop-parallel`. It has no reading in the published results.

## Presets

`--preset` takes the preset names familiar from x264 and x265. Four are built.

| preset | coding tree at 1080p | lookahead at 1080p | what it's for |
|---|---|---|---|
| `fast` | 32x32 | 12 frames | more speed for a little quality |
| `medium` | 64x64 | 20 frames | the default, and the preset every published result uses |
| `slow` | 64x64 | 20 frames | more analysis per block |
| `slower` | 64x64 | 25 frames | more analysis and a second reference |

`medium` changes with the picture size. A small picture gets a smaller coding tree and a shorter lookahead, and 4K gets a second reference. Design has [the whole table](design.md#what-medium-does-at-each-size).

`slow` and `slower` have only been placed against x265's `medium`. Against x265's own `slow` and `slower` they don't yet win, so I'd use `medium` unless you've measured otherwise on your content.

x265's other six names are refused with an error that names the nearest preset. We built `faster`, `veryfast`, `veryslow` and `placebo` and measured each against the x265 preset of the same name. None of them earned the name. `ultrafast` and `superfast` aren't built.

A preset only sets defaults, so any option you give wins over it. To see what a run will do, ask:

```sh
y265 in.y4m --preset medium --crf 23 --show-config
```

That prints the resolved configuration and exits. `--verbose` prints the same table at the start of a real encode.

## Tunes

`--tune zerolatency` turns off B-frames, the lookahead and the frame-level parallelism that delays output, for one frame of latency. It's the only tune.

The encoder refuses x265's other four tune names and prints a reason for each.

| refused tune | why | what to use |
|---|---|---|
| `psnr`, `ssim` | each fits the encoder to a metric the project doesn't decide on | `--ssim` reports the number without changing the encode |
| `grain` | in x265 it's a set of rate-control and psychovisual changes for grainy sources. That hasn't been built here | `--film-grain` is a different tool, described [below](#film-grain-denoising-and-filters) |
| `fastdecode` | only part of it could be delivered | `--no-weightp` and `--no-weightb` directly |

## Profiles, depth and chroma

The input picks the profile. No option names it.

| input | stream |
|---|---|
| 4:2:0, 8-bit | Main |
| 4:2:0, 10-bit | Main10 |
| 4:4:4, 8-bit | Main 4:4:4 |
| 4:4:4, 10-bit | Main 4:4:4 10 |
| 4:2:2, 8 or 10-bit | Main 4:2:2 10 |

`--output-depth 10` codes 8-bit input as Main10. `--output-depth 8` codes 10-bit input at 8 bits by dropping two bits. Add `--dither` there to spread the lost precision as a fine pattern, so that gradients don't turn into steps. `--lossless` codes a stream that decodes to the exact source. `--level` and `--high-tier` signal a level and tier of your choosing. The encoder refuses a level the stream doesn't fit.

## GOP and references

A key frame arrives every `--keyint` frames, 250 by default. A scene cut is also a key frame and restarts the count.

By default the periodic key frame is a CRA with open GOP: the B-frames just before it in display order may predict across it, which saves bits. A player that seeks to the CRA drops those pictures. `--no-open-gop` writes an IDR there for packagers that need closed segments. `--radl` keeps some of the leading pictures decodable after a seek.

`--bframes` sets how many B-frames sit between anchors, 7 by default, arranged as a pyramid. The lookahead shortens the run where motion is high. `--ref` sets the references per list. `--rc-lookahead` sets the lookahead depth, and that depth is the encoder's latency in frames. Weighted prediction for fades is on in P slices from `medium` up.

## Threads and memory

`--threads 0` is the default and starts one worker per online CPU. Above one thread the encoder also reads its input on a separate thread to keep a slow source from stalling the workers.

What's promised is repeatability at `--threads 1`: two runs of one command write the same bytes. Identity across thread counts is checked and reported. It isn't promised. Capped VBR is different by design. Above one thread its rate control works with several frames in flight and writes a different stream from the one-thread encode. [Threading](threading.md#what-stays-identical) has the details.

`--gop-parallel <n>` codes a long input as segments on several encoder instances at once and joins them at key frames. It's opt-in. The joined stream differs from the serial one and has no HRD timing. It also doesn't combine with `--pass`, `--qpfile`, `--zones`, `--csv` or `--shot-stats`. [Threading](threading.md#segment-parallel-encoding) explains when it helps.

## Shot-aware options

Both of these are opt-in. The default encoder never looks further ahead than its lookahead.

`--shot-window <n>` buffers up to `n` frames so the encoder can plan a whole shot against the shots before it. It works with `--crf` only. It adds `n` frames of latency and holds that many more source pictures in memory. It stays off because, in our own test on multi-shot sequences, it saved a little bitrate and made quality less even from one shot to the next.

`--shot-stats <file>` writes one JSON line per shot: where it starts and ends, its bits, its frame types and its complexity. A ladder builder can use it to pick a rate per shot. The stream is the same with it on.

## Film grain, denoising and filters

All of these change the picture, so read them as tools for specific sources. None has a quality reading in the published results.

| option | what it does | when |
|---|---|---|
| `--film-grain <s>` | estimates the source's grain and describes it in an SEI message. A decoder that supports the message adds grain back | grainy film. 8-bit 4:2:0 only |
| `--film-grain-denoise 1` | with `--film-grain`, also removes the grain before coding. This is the half that saves bits | when the player is known to synthesize grain |
| `--nr-intra`, `--nr-inter` | attenuate coefficients that look like noise before they're quantized | noisy sources. `--nr-intra 0 --nr-inter 0` turns off what `medium` sets |
| `--mcstf` | averages each anchor picture with its motion-compensated neighbors before coding | noisy sources. It adds latency |
| `--selective-mcstf` | skips that filter on pictures that measure clean | a mixed library. Flat-shaded animation loses quality to `--mcstf` without it |

## Steering an encode

`--zones` changes the quantizer or the bit budget over frame ranges. `--zones 0,239,q=30/1200,1439,b=1.5` holds the first 240 frames at QP 30 and gives a later range half again as many bits. Zones count in display order and can't overlap.

`--qpfile` forces frame types and QPs from a file with one `<frame> <type> [<qp>]` line each. It takes key frames and intra frames. It refuses P and B types because the B-frame run belongs to the lookahead.

`--qpmin` and `--qpmax` bound the frame quantizer in every mode. `--crf-min` and `--crf-max` bound the rate factor of a `--crf` encode.

## What ships

| area | what's there |
|---|---|
| profiles | Main, Main10, Main 4:4:4, Main 4:4:4 10, Main 4:2:2 10, lossless |
| rate control | CRF, capped CRF, capped VBR, CBR with filler, ABR, two-pass, fixed QP, zones, a QP file |
| buffers | VBV with HRD signaling, a bound on one picture's size, level and tier checks |
| structure | open and closed GOP, hierarchical B-frames, scene cut detection, weighted prediction, temporal layers |
| analysis | adaptive quantization, lookahead bit allocation, rate-distortion optimized quantization, screen content detection |
| HDR | HDR10, HDR10+, HLG, Dolby Vision profiles 8.1, 8.2 and 8.4, HDR tuning at `medium` |
| filters | film grain SEI, coefficient noise reduction, a temporal filter, dither |
| signaling | every VUI color flag, picture hash SEI, recovery point SEI, access unit delimiters, repeated headers |
| threads | one pool on every core, segment-parallel encoding |
| speed | SIMD on arm64 and x86-64, picked at run time. [Architectures](architectures.md) has the list |
| front ends | the `y265` command line and the `libyah265` encoder in ffmpeg |
| reports | CSV statistics, per-shot statistics, the resolved configuration |

## Left out on purpose

| not offered | why |
|---|---|
| six of x265's preset names | four were built and didn't earn the name, as [Presets](#presets) says |
| four of x265's tune names | see [Tunes](#tunes) |
| `--intra-refresh` | a refresh only helps if nothing in the refreshed area predicts from outside it. The encoder can't promise that, so it refuses the option by name |
| `--scaling-list` | every quantizer path would have to apply the matrices or the encoder's pictures would stop matching a decoder's. It's refused by name |
| interlaced coding | we will add it if there is enough interest |
| 12-bit | only 8-bit and 10-bit are built |
| `--psy-rd`, `--psy-rdoq` | an option with that name wouldn't mean what x265's does, because yah265's perceptual weighting works differently |
| x265's threading options | yah265 sets up its threads by itself. It splits each frame into rows and works on rows from several frames at once, so every core stays busy without any tuning |
| analysis save and load | they serve a multi-stage pipeline that nothing here uses |
| a switch to turn off scene detection | the encoder always looks for scene changes |
| non-conforming streams | where no level admits a stream the encoder refuses to write it |
| Dolby Vision profile 5 | its base layer uses a color space nothing in this toolchain can produce or check |

## Identification marks

Every stream has an SEI message that names the encoder, its version, its license and the options the run resolved. Because it holds no clock, no path and no thread count, one command writes one message on any machine. `--no-info` drops it.

The stream also has a fingerprint that survives when the SEI is stripped. In a few places HEVC lets an encoder write the same motion two ways at the same cost in bits, and yah265 picks between them by a fixed rule. The mark costs no bits and changes no picture. `--no-fingerprint` turns it off. The [fingerprint design note](docs/stream-fingerprint.md) says how to read it back.

## Environment knobs

Settings named `N265_*` in the environment reach tuning parameters the command line doesn't expose. They're for experiments. A preset sets most of them. `--show-config` prints the ones a preset named and marks any your environment overrode. The [knob list](docs/knobs.md) names them all.

## From ffmpeg

`libyah265` hands its options to the same parser `y265` uses. An encode from ffmpeg therefore resolves the way the same command line would.

| ffmpeg | `y265` | note |
|---|---|---|
| `-preset` | `--preset` | `medium` when unset |
| `-tune` | `--tune` | |
| `-crf` | `--crf` | |
| `-qp` | `--qp` | |
| `-b:v` | `--bitrate` | wins over `-crf` and `-qp` |
| `-maxrate`, `-bufsize` | `--vbv-maxrate`, `--vbv-bufsize` | |
| `-g` | `--keyint` | |
| `-bf` | `--bframes` | |
| `-refs` | `--ref` | |
| `-threads` | `--threads` | 0 is one per CPU on both sides |
| `-color_primaries`, `-color_trc`, `-colorspace` | `--colorprim`, `--transfer`, `--colormatrix` | written only when set |
| `-color_range pc` | `--range full` | |
| `-yah265-params` | any other coding option | names without the dashes, joined by colons |

Pass `-crf`. With no rate option the encode is fixed QP 26, which isn't what x265 does with no rate option.

`-yah265-params` takes the long name of any option on the [Options](options.md) page:

```sh
ffmpeg -i in.mp4 -c:v libyah265 -preset medium -crf 25 \
  -yah265-params rc-lookahead=30:no-open-gop out.mp4
```

A switch takes no value, or `1` or `0`. A value with a colon in it can't go through the list. `dhdr10-info` and `dolby-vision-rpu` are refused there because they read files, so HDR10+ and Dolby Vision encodes go through `y265`.

The pixel format picks the bit depth. 4:2:0, 4:2:2 and 4:4:4 work at 8 and 10 bits, and `-pix_fmt yuv420p10le` codes 8-bit content as Main10.
