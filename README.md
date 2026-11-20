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

<!--gen:headline-->yah265 encodes video 1.77x faster than x265 at the same file size, median over every timed cell (CRF, capped VBR and content types), with +1.18 VMAF v1 points.<!--/gen:headline-->

yah265 is an H.265/HEVC encoder written from scratch in C. It runs inside ffmpeg under the options you already use. Its sibling [yah264](https://terranvigil.github.io/yah264/) met the same goals against x264.

## Try it

```sh
ffmpeg -i in.mp4 -c:v libx265   -preset medium -crf 25 out.mp4   # what you run today
ffmpeg -i in.mp4 -c:v libyah265 -preset medium -crf 25 out.mp4   # swap one word
```

One command builds that ffmpeg:

```sh
git clone https://github.com/terranvigil/yah265 && cd yah265
scripts/build-ffmpeg.sh
```

It needs git, a C compiler, Meson, Ninja, pkg-config and make. The new ffmpeg is at `build-ffmpeg/ffmpeg/ffmpeg`. Add `--with-x265` to build `libx265` into it too, for side-by-side tests. Because yah265 is GPL, the ffmpeg it builds is a GPL binary.

Pass `-crf`. With no rate option the encoder runs a fixed-quantizer test mode.

## Results

"Faster" compares wall time for files of the same size, so 2x faster is half the time. Both encoders run `--preset medium`. The reference is x265 4.3. The test set runs from short standard clips to ten-minute excerpts of open films, at CIF, 720p, 1080p and 4K.

Every cell behind the headline was timed with SIMD on an 18-core Apple M5 Max. The capped VBR cells share a target bitrate where the others share a size.

<!--gen:pooled-table-->
| timed cells | count | median | slowest cell |
|---|--:|--:|---|
| CRF | 35 | 1.79x faster | Sintel 4K, 10 min, 1.52x faster |
| capped VBR | 26 | 1.64x faster | Meridian 1080p, 10 min, 1.18x faster |
| content types | 9 | 1.92x faster | Lash of the Whip 1080p, 1.85x faster |
| **all** | **70** | **1.77x faster** | Meridian 1080p, 10 min, 1.18x faster |
<!--/gen:pooled-table-->

<!--gen:quality-table-->
| quality on the Mac | cells | median |
|---|--:|--:|
| VMAF v1 lead at the same file size, CRF and content types | 44 | +1.18 points |
| bits saved at equal VMAF v1, rate ladders at CRF and capped VBR | 52 | 10.9% |
<!--/gen:quality-table-->

Each of the project's three speed goals passes on three machines. The table gives the range of group medians, where a group is one picture size in one length of clip.

<!--gen:goal-table-->
| goal | Apple M5 Max (NEON) | Xeon 8488C (AVX-512) | Graviton4 (SVE2) |
|---|--:|--:|--:|
| G1: C on one thread | 2.17x to 4.35x | 1.49x to 3.03x | 1.52x to 3.03x |
| G2: C on all cores | 2.86x to 4.17x | 2.27x to 4.76x | 2.38x to 4.76x |
| G3: SIMD on all cores | 1.61x to 2.13x | 1.35x to 2.44x | 1.47x to 3.23x |
<!--/gen:goal-table-->

The [results page](https://terranvigil.github.io/yah265/results.html) has every table and chart. [Check it yourself](https://terranvigil.github.io/yah265/check-it-yourself.html) shows how to rerun a reading.

## The y265 command line

`y265` is the encoder without ffmpeg around it. It needs a C11 compiler, Meson, Ninja and Python 3.

```sh
make build-rel
ffmpeg -i in.mp4 -pix_fmt yuv420p -f yuv4mpegpipe - | build-rel/y265 - -o out.hevc --crf 25
```

It reads Y4M or raw YUV from a file or a pipe and writes a raw HEVC stream, which ffmpeg plays and muxes. `build-rel/y265 --help` lists every option.

## Documentation

The site is at [terranvigil.github.io/yah265](https://terranvigil.github.io/yah265/).

| page | what's on it |
|---|---|
| [Build and first encode](https://terranvigil.github.io/yah265/start.html) | the ffmpeg build, a first encode, `y265` on its own |
| [Encoding guide](https://terranvigil.github.io/yah265/guide.html) | rate modes, presets, threads, what ships and what's left out |
| [Options](https://terranvigil.github.io/yah265/options.html) | every option with its default |
| [HDR](https://terranvigil.github.io/yah265/hdr.html) | HDR10, HDR10+, HLG and Dolby Vision profile 8 |
| [The HEVC standard](https://terranvigil.github.io/yah265/hevc.html) | how encoding and HEVC work |
| [Design](https://terranvigil.github.io/yah265/design.html) | how yah265 makes its choices |
| [Threading](https://terranvigil.github.io/yah265/threading.html) | the many-core pipeline |
| [Architectures](https://terranvigil.github.io/yah265/architectures.html) | what runs where, and where SIMD speed comes from |
| [Results](https://terranvigil.github.io/yah265/results.html) | every table and chart |
| [Blog](https://terranvigil.github.io/yah265/blog.html) | the launch post |

In the repository, the [plan](docs/plan.md) holds the goals and phases. [Ideas](docs/ideas.md) logs every refused change with its cause.

## Contributing

Read [CONTRIBUTING.md](CONTRIBUTING.md) first. The encoder is written from the ITU-T H.265 specification. No code from another encoder goes in.

A change has to pass the gate before it merges. `make test` runs the tests. `make conformance-fast` decodes every test stream with ffmpeg and libde265 and compares both with the encoder's own reconstruction. Two runs at `--threads 1` must write the same bytes. A SIMD kernel must match its C reference bit for bit. I don't merge a change that costs speed or quality on the test set.

## License

yah265 is GPL-2.0-or-later, with a commercial license beside it in the same arrangement as x264 and x265. See [LICENSE](LICENSE), and write to terranvigil@gmail.com for commercial terms.
