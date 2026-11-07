# yah265

yah265 is an H.265/HEVC encoder written from scratch in C. It's faster than
x265 at `--preset medium` on every class of test video from CIF to 4K. At
the same file size its pictures score higher on VMAF v1.

The encoder is clean-room from the ITU-T H.265 specification. We decode every
stream with ffmpeg and with libde265 and check that both match our own
reconstruction. Every number below comes from a script in this repository. yah265 is the sibling
of [yah264](https://terranvigil.github.io/yah264/), which reached the same
goals against x264.

## Results

Both encoders run `--preset medium` and write files of the same size. The
reference is Homebrew's x265 4.3, timed on an 18-core Apple M5 Max. The
test set runs from short standard clips to ten-minute excerpts of open
films, at CIF, 720p, 1080p and 4K.

| goal | yah265 | x265 | yah265 runs |
|---|---|---|---|
| 1 | pure C on one thread | `--no-asm` on one thread | 1.96x to 3.45x as fast |
| 2 | pure C on all cores | `--no-asm` on all cores | 2.27x to 3.23x as fast |
| 3 | NEON on all cores | its own assembly on all cores | 1.37x to 1.89x as fast |
| quality | | | 12.4% fewer bits at equal VMAF v1 |

Each speed range runs from the slowest group's median to the fastest's. A
group is one resolution class in one tier of the test set. The quality row
is a median over 52 rate ladders on real films at CRF and at VBR. Every one
of those ladders comes out ahead of x265.

The [results page](site/results.md) has every table, down to the slowest
clip in each group, VBR rate tracking and the lookahead sweep. It's built
from the [parity report](docs/parity/PARITY-REPORT-yah265-2026-11-04.md).
[Check it yourself](site/check-it-yourself.md) shows how to rerun a reading.

## Build and use

You need a C11 compiler (clang is the reference), Meson, Ninja and
Python 3.

```
git clone https://github.com/terranvigil/yah265
cd yah265
make build-rel
build-rel/n265 input.y4m -o out.hevc --crf 23
```

The output is a raw Annex-B stream, which ffmpeg plays and muxes. The input
can be Y4M, raw planar YUV, or `-` for standard input. A pipe from ffmpeg
takes any source ffmpeg can read:

```
ffmpeg -i movie.mov -pix_fmt yuv420p -f yuv4mpegpipe - | build-rel/n265 - -o out.hevc --crf 23
```

`make test` runs the tests, and `make conformance-fast` runs the decoder
checks. [Getting started](site/start.md) walks through the command line.
`build-rel/n265 --help` lists every option.

## Presets

`--preset` takes x265's names. Four are built: `fast`, `medium` (the
default), `slow` and `slower`. Every result above is read at `medium`. It
adapts to the picture size. From 720p up it looks 20 frames ahead. From
1080p up it codes 64x64 coding trees where smaller pictures use 32x32.
We refuse x265's other six names with an error that points at the
nearest preset we have. Because a
preset only sets defaults, any option you give wins over it.
`--show-config` prints what a run will actually do.

## Features

- Main, Main10, Main 4:4:4, Main 4:4:4 10 and Main 4:2:2 10 from one
  binary. The input picks the profile.
- CRF, fixed QP, ABR, VBR and CBR with a VBV, capped CRF, and two-pass.
- HDR10, HDR10+, HLG and Dolby Vision profile 8 signaling. `medium` also
  tunes its coding for PQ and HLG pictures. The [HDR notes](docs/hdr.md)
  cover both.
- x265's option names wherever the two encoders share a feature.
- NEON kernels on arm64. Each one is bit-exact with its C reference.
- The same bytes from run to run at `--threads 1`.

## License

It's licensed GPL-2.0-or-later, with a commercial license beside it in the
same arrangement as x264 and x265. See LICENSE, and ask
terranvigil@gmail.com for commercial terms. Contributions follow the
clean-room rules in [CONTRIBUTING.md](CONTRIBUTING.md).

## More

- [Design](site/design.md) explains how the encoder is put together.
- The [plan](docs/plan.md) holds the goals and the phases. The
  [phase 14 notes](docs/phase14.md) record each item since.
- The [x265 feature audit](docs/x265-feature-audit.md) maps x265's
  options to ours.
- The [knob list](docs/knobs.md) names the `N265_*` tuning settings.
- [Ideas](docs/ideas.md) logs every refused change with its cause.
