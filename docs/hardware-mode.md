# Hardware mode

`--hw videotoolbox` encodes HEVC on the Mac's fixed-function engine through
VideoToolbox, behind yah265's options. Our options map onto the hardware's
settings as far as they go. Our scene-cut and our keyframe interval decide
where the key frames fall. When the session opens, the encoder names every
option the hardware can't follow. The stream is Apple's. None of the
recon-match, conformance, determinism or parity gates apply to it, and it
never appears in their rows.

## How it works

`--hw off|auto|videotoolbox` picks the mode. `off` is the default. Because
the option goes through the library's own parser, the ffmpeg wrapper takes
`hw=videotoolbox` in `-yah265-params` unchanged. A library caller sets the
mode with `n265_set_hw` before the open. `n265_encoder_backend` then names
what opened. The `N265_HW` environment variable overrides both for test
scripts. The meson option `videotoolbox` builds the mode on macOS.
Elsewhere a stub's open fails with a reason, so callers carry no platform
checks.

The session requires the hardware encoder. We refuse VideoToolbox's
software encoder, and we refuse a session that won't say it's hardware.
Under `videotoolbox` a refusal fails the encode. Under `auto` it prints one
line naming why and the encode runs on our own encoder.

Each input picture is copied into a buffer from the session's pool. The
session holds pictures past the call while the caller reuses its planes.
8-bit input codes as Main. 10-bit input codes as Main10 from a 10-bit
biplanar buffer, and so does `--output-depth 10`. On the session's thread
the output callback turns each length-prefixed sample into Annex B. The
encode call hands the samples out in decode order as one access unit each.
The VPS, SPS and PPS come from the format description and lead the first
access unit. ffmpeg asks for the headers before the first picture when it
writes an MP4. That call opens a one-frame probe session with the same
settings and takes the headers from it. The reorder depth a container
needs comes from that SPS.

Our scene-cut runs on each picture as it arrives. It uses the lookahead's
half-size costs and its cut rule. A cut forces a key frame with no added
latency. There's no frame of lookahead, so there's no flash guard either,
and a one-frame flash costs an extra key frame. `--keyint` counts from the
last key frame, as in software. The hardware's own interval sits at twice
that as a backstop. We set it that far out because the hardware puts its
key a couple of frames short of the interval it's given. On sintel_720p
the hardware's key frames fall on exactly the pictures the software
encoder picks. `N265_HW_SC=0` leaves the key frames to the hardware. The
first key frame is an IDR and later ones are CRAs.

The session writes no HDR10 SEI even when it's given the mastering display
and light level. We build those two messages from `--master-display` and
`--max-cll` and send them with the parameter sets.

## What maps and what doesn't

The map is one C table in `src/hw/vt_map.c`. The warnings read it, so the
text and the behavior can't drift apart.

| yah265 | on the hardware | class |
|---|---|---|
| `--keyint` | our interval, with the hardware's at twice it as a backstop | maps |
| `--bitrate` | average bitrate | maps |
| `--colorprim`, `--transfer`, `--colormatrix` | the session's color properties, for the values VideoToolbox names | maps |
| `--range` | video or full range buffers at 10 bits | maps |
| `--sar` | pixel aspect ratio | maps |
| `--master-display`, `--max-cll` | our own SEI beside the parameter sets | maps |
| `--repeat-headers` | VPS, SPS, PPS and that SEI before every key frame | maps |
| `--threads` | nothing; it's a schedule | maps |
| `--vbv-maxrate`, `--vbv-bufsize` | a data-rate limit of maxrate over a window of bufsize / maxrate seconds | approximated |
| `--crf` | the 0-to-1 quality key through a rate-matched table | approximated |
| `--qp` | minimum and maximum frame QP pinned to N | approximated |
| `--bframes` | frame reordering on or off | approximated |
| `--tune zerolatency` | real-time mode | approximated |
| `--preset`, `--level`, `--high-tier`, `--ref`, `--b-adapt`, the GOP options, `--rc-lookahead`, `--cutree`, `--aq`, `--ctu`, the transform and RDOQ options, the chroma offsets, weighted prediction, noise reduction, MCSTF, film grain, `--dup-threshold`, `--shot-window`, `--hdr-opt`, the CRF and QP bounds, `--vbv-init`, `--strict-cbr`, `--max-ausize-factor`, `--qpfile`, `--zones`, `--pass`, `--stats`, `--hash`, AUD, EOS, EOB, the timing and recovery SEI, `--temporal-layers`, `--videoformat`, `--overscan`, `--chromaloc`, `--display-window`, `--atc-sei`, `--info`, `--fingerprint`, `--vui-timing` | nothing | ignored |
| `--pcm`, `--lossless`, `--psnr`, `--ssim`, `--intra-refresh`, `--dhdr10-info`, the Dolby Vision options, `--dump-recon`, `--dump-recon-decode`, `--csv`, `--shot-stats`, 4:2:2 and 4:4:4 input | nothing | refused |

At open the encoder prints one block naming each ignored or approximated
option the user gave. Defaults print nothing. `--hw-strict` turns that
block into an error for scripts. A refused option fails `--hw
videotoolbox` and sends `--hw auto` to our encoder. The CLI's own reports
and dumps fail in either mode. `--gop-parallel` codes serially.

Where the hardware won't take a pinned frame QP, `--qp N` runs as the
quality of `--crf N` and says so. A color value VideoToolbox has no name
for is left unsignaled with a line saying so.

The CRF table matches rate, not quality. Each point is the quality at
which the hardware spends what our medium spends at that CRF. It's the
median over eight corpus clips at 120 frames, and it's linear between
points. At CRF 28 the hardware's file is within 17% of ours on six of the
eight clips. On park_joy and sintel it's far larger, at 2.15x and 1.52x.

| CRF | 20 | 24 | 28 | 32 | 36 |
|---|--:|--:|--:|--:|--:|
| quality | 0.63 | 0.54 | 0.43 | 0.31 | 0.205 |

## The trade

We ran our medium at `--crf 25` on ten clips from 720p to 2160p, then ran
the hardware at the average bitrate that encode landed on. Our side is the
release build at auto threads on the 18-core Mac. Each side ran three
times and the table takes the median wall and CPU time. VMAF v1 scores
both by the project's display rule, and `docs/data/hw-videotoolbox.csv`
has every column. `scripts/hw_measure.py` reproduces it.

| clip | kbit/s | wall, hardware / ours | CPU, ours / hardware | VMAF v1, hardware minus ours | size, hardware / ours |
|---|--:|--:|--:|--:|--:|
| ducks_720p | 14115 | 0.29 | 104 | -6.1 | 1.05 |
| park_joy_720p | 7651 | 0.40 | 75 | -4.3 | 1.05 |
| shields_720p | 1349 | 0.78 | 29 | -1.6 | 1.08 |
| fourpeople_720p | 902 | 1.17 | 22 | -3.7 | 1.15 |
| crowd_run_1080p | 19426 | 0.21 | 107 | -1.5 | 1.03 |
| pedestrian_1080p | 1764 | 0.43 | 52 | -4.6 | 1.03 |
| riverbed_1080p | 11420 | 0.17 | 137 | -9.5 | 1.00 |
| sunflower_1080p | 897 | 0.86 | 24 | -3.3 | 1.09 |
| bbb_2160p | 2241 | 0.87 | 18 | -7.4 | 1.05 |
| meridian_2160p | 1784 | 1.00 | 16 | -13.6 | 1.70 |
| **median** | | **0.60** | **41** | **-4.4** | **1.05** |

The hardware uses about a fortieth of our CPU and less of the clock. It's
slower only on fourpeople, where our encode is cheap and the hardware
still pays a fixed cost of about half a second to open the session and
read the input. Quality pays for the savings. The hardware scores below us
on all ten clips even though it spends a little more than we do on nine
of them. meridian_2160p is the worst case because the hardware won't code
4K at 60 frames a second that thin. It overshot the target by 70% and
still scored worst. I'd use the hardware mode when CPU matters more than
bits.

## What we chose and why

| choice | why |
|---|---|
| default `--hw off` | `auto` would swap in a different encoder, and every gate in the tree assumes ours |
| hardware only, never VideoToolbox's software encoder | the mode exists for the hardware's CPU savings; without the hardware our own encoder is the software path |
| the option in the library's parser | the CLI and the ffmpeg wrapper take it the same way, and the wrapper needed no change |
| our scene-cut and interval place the key frames | they land where the software encoder's do, so the two streams cut the same way |
| `--crf` onto the quality key through a fixed table, with a warning | the hardware has no CRF, and a monotone table is cheap |
| warn once on options the user named; `--hw-strict` to refuse | a default run shouldn't print noise, and scripts need a way to fail loudly |
| `auto` falls back with one line; `videotoolbox` never does | a forced mode that quietly ran something else would mislead |
| our own HDR10 SEI | the session takes the values and writes neither message |

## Measuring it

- The hardware isn't byte-stable from run to run. Don't put its streams
  through a determinism check.
- A raw stream from the hardware has no VUI timing, and a decoder guesses
  the rate. Score it by frame index. `scripts/hw_measure.py` feeds libvmaf
  through ffmpeg with passthrough frame timing.
- CPU counts the encoder process only, so it leaves out work the hardware
  does elsewhere.
