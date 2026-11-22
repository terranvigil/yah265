<p align="center">
  <picture>
    <source media="(prefers-color-scheme: dark)" srcset="assets/yah265-logo-dark.png">
    <img src="assets/yah265-logo-light.png" alt="yah265" width="420">
  </picture>
</p>

<p align="center">
  <a href="#try-it">Try it</a> ·
  <a href="#results">Results</a> ·
  <a href="#the-y265-command-line">y265</a> ·
  <a href="#documentation">Documentation</a> ·
  <a href="#contributing">Contributing</a> ·
  <a href="#license">License</a>
</p>

<!--gen:headline-->On an Apple M5 Max, yah265 encodes a median 1.8x faster than x265 at the same file size. Its VMAF v1 score is a median 1.23 points higher. That's the median of 30 encodes, from short clips to ten-minute films, at CIF to 4K. Both encoders ran at preset medium with SIMD on all cores.<!--/gen:headline-->

yah265 is a new HEVC encoder, written in C. It runs inside ffmpeg under the options you already use. Its sibling [yah264](https://terranvigil.github.io/yah264/) met the same goals against x264.

## Try it

**New:** [try yah265 in your browser](https://terranvigil.github.io/yah265/playground/), with nothing to install.

```sh
ffmpeg -i in.mp4 -c:v libx265   -preset medium -crf 25 out.mp4   # what you run today
ffmpeg -i in.mp4 -c:v libyah265 -preset medium -crf 25 out.mp4
```

Simple command builds that ffmpeg:

```sh
git clone https://github.com/terranvigil/yah265 && cd yah265
scripts/build-ffmpeg.sh
```

It needs git, a C compiler, Meson, Ninja, pkg-config and make. The new ffmpeg is at `build-ffmpeg/ffmpeg/ffmpeg`. Add `--with-x265` to build `libx265` into it too, for side-by-side tests. Because yah265 is GPL, the ffmpeg it builds is a GPL binary.

Always pass a rate option. Without one, the encoder runs a fixed-quantizer test mode.

## Results

"Faster" compares wall time for files of the same size. Both encoders run `--preset medium`. The reference is x265 4.3. The test set runs from short standard clips to ten-minute excerpts of open films, at CIF, 720p, 1080p and 4K.

To see the difference, [watch the encodes side by side](https://terranvigil.github.io/yah265/compare.html).

The table breaks the 1.8x down by picture size. It counts the clips where the two files match in size within 1%.

<!--gen:pooled-table-->
| size-matched cells on the Mac | count | median | slowest cell |
|---|--:|--:|---|
| CIF | 3 | 1.96x faster | Bus CIF, 1.75x faster |
| 720p | 10 | 1.92x faster | Shields 720p, 1.54x faster |
| 1080p | 10 | 1.79x faster | Big Buck Bunny 1080p, 10 min, 1.69x faster |
| 4K | 7 | 1.67x faster | Sintel 4K, 10 min, 1.52x faster |
| **all** | **30** | **1.8x faster** | Sintel 4K, 10 min, 1.52x faster |
<!--/gen:pooled-table-->

<!--gen:quality-table-->
| quality on the Mac | cells | median |
|---|--:|--:|
| VMAF v1 lead at the same file size, the cells above | 30 | +1.23 points |
| bits saved at equal VMAF v1, rate ladders at CRF and capped VBR | 38 | 10.2% |
<!--/gen:quality-table-->

yah265 is faster on three machines. I time speed three ways and call each one a goal. [G3](https://terranvigil.github.io/yah265/methodology.html#the-three-speed-goals) is the row a user sees: both encoders as shipped. [G1 and G2](https://terranvigil.github.io/yah265/methodology.html#the-three-speed-goals) compare the algorithms, with the hand-written SIMD off on both sides.

<!--gen:goal-table-->
| goal | Apple M5 Max (NEON) | Xeon 8488C (AVX-512) | Graviton4 (SVE2) |
|---|--:|--:|--:|
| [G3](https://terranvigil.github.io/yah265/methodology.html#the-three-speed-goals): as shipped, SIMD on all cores | 1.59x to 2.13x | 1.25x to 2.33x | 1.39x to 3.03x |
| [G1](https://terranvigil.github.io/yah265/methodology.html#the-three-speed-goals): algorithm only, plain C on one thread | 2.17x to 4.00x | 1.49x to 2.94x | 1.52x to 2.94x |
| [G2](https://terranvigil.github.io/yah265/methodology.html#the-three-speed-goals): algorithm only, plain C on all cores | 2.56x to 4.35x | 2.22x to 4.76x | 2.08x to 4.35x |
<!--/gen:goal-table-->

See the [results page](https://terranvigil.github.io/yah265/results.html) for the tables and charts, and [Check it yourself](https://terranvigil.github.io/yah265/check-it-yourself.html) to rerun a measurement.

## The y265 command line

`y265` is the encoder without ffmpeg around it. It needs a C11 compiler, Meson, Ninja and Python 3.

```sh
make build-rel
ffmpeg -i in.mp4 -pix_fmt yuv420p -f yuv4mpegpipe - | build-rel/y265 - -o out.hevc --crf 25
```

It reads Y4M or raw YUV from a file or a pipe. The output is a raw HEVC stream that ffmpeg plays and muxes. `build-rel/y265 --help` lists the options.

## Hardware option?

On Macs, `--hw videotoolbox` uses Apple's hardware HEVC encoder instead of yah265's own. The table gives the median over [ten clips](docs/hardware-mode.md) from 720p to 2160p at the same bitrate.

| mode | wall time | CPU used | VMAF v1 |
|---|--:|--:|--:|
| `--hw videotoolbox` vs yah265 SIMD | 0.6x as long | 1/40 as much | 4.4 points lower |

## Shot-aware support

Most video is a sequence of shots that each want different settings. yah265 gives a per-shot pipeline what it needs in a single pass.

A shot window lets a CRF encode buffer up to a whole shot and balance its quality against the shots before it. It's off by default because it adds latency.

Shot statistics write one JSON line per shot as the encode runs. Each line has where the shot starts and ends, its bits, its frame types and its complexity. The hull script scores each shot of several encodes against the source on VMAF v1. The encodes can be at different resolutions. It prints each shot's convex hull across all of them, which a ladder builder picks rungs from.

```sh
y265 in.y4m -o crf22.hevc --crf 22 --shot-stats crf22.jsonl
y265 in540.y4m -o 540p.hevc --crf 28 --shot-stats 540p.jsonl
scripts/shot_hull.py --src in.y4m \
    --enc crf22=crf22.hevc:crf22.jsonl --enc 540p=540p.hevc:540p.jsonl
```

A shot plan gives each shot its own quality level in one pass, from a scan of the source with no trial encodes. It runs under CRF and capped CRF.

The [encoding guide](https://terranvigil.github.io/yah265/guide.html#planning-by-shot) has the details.

## Up next

The shot tools above grow into a full per-shot ladder:

- The shot plan under VBR as well as CRF.
- My [veo project](https://github.com/terranvigil/veo) runs the ladder search itself over rungs, convex hull and VMAF. It uses yah265 as its encoder.

## Documentation

**[Docs home](https://terranvigil.github.io/yah265/)**

Use
- [Build and first encode](https://terranvigil.github.io/yah265/start.html): the ffmpeg build, a first encode, building `y265`
- [Encoding guide](https://terranvigil.github.io/yah265/guide.html): rate modes, presets, threads, live control, shots
- [Coming from x265](https://terranvigil.github.io/yah265/from-x265.html): option names and a CRF translation table
- [Try it in your browser](https://terranvigil.github.io/yah265/playground.html): the playground, nothing to install
- [Try it on your own clip](https://terranvigil.github.io/yah265/bench.html): `y265 bench` against x265
- [Options](https://terranvigil.github.io/yah265/options.html): the options and their defaults
- [HDR](https://terranvigil.github.io/yah265/hdr.html): HDR10, HDR10+, HLG and Dolby Vision profile 8

Understand
- [The HEVC standard](https://terranvigil.github.io/yah265/hevc.html): how encoding and HEVC work
- [Design](https://terranvigil.github.io/yah265/design.html): how yah265 makes its choices
- [Threading](https://terranvigil.github.io/yah265/threading.html): the many-core pipeline
- [Architectures](https://terranvigil.github.io/yah265/architectures.html): what runs where

Evidence
- [Results](https://terranvigil.github.io/yah265/results.html): the tables and charts
- [Watch the encodes](https://terranvigil.github.io/yah265/compare.html): side-by-side players
- [How x265 was run](https://terranvigil.github.io/yah265/x265-run.html): the commands and builds
- [Check it yourself](https://terranvigil.github.io/yah265/check-it-yourself.html): rerun a measurement
- [A skeptic's questions](https://terranvigil.github.io/yah265/faq.html)
- [Blog](https://terranvigil.github.io/yah265/blog.html): the launch post

## Contributing

Read [CONTRIBUTING.md](CONTRIBUTING.md) first. The encoder is written from the ITU-T H.265 specification.

A change has to pass the gate before it merges. `make test` runs the tests. `make conformance-fast` decodes the test streams with ffmpeg and libde265. Both must match the encoder's own output. Two runs at `--threads 1` must write the same bytes. A SIMD kernel must match its C reference bit for bit. I don't merge a change that costs speed or quality on the test set.

## License

yah265 is dual-licensed: GPL-2.0-or-later, or a commercial license. See [LICENSE](LICENSE). Write to terranvigil@gmail.com for commercial terms.
