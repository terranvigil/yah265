# Hardware mode

`--hw videotoolbox` encodes HEVC on the Mac's fixed-function engine through
VideoToolbox, behind yah265's options. Those options map onto the hardware's
settings as far as they go. When the session opens, the encoder names the
options the hardware can't follow. yah265 still places the key frames.
Because the stream is Apple's, the recon-match, conformance, determinism and
parity checks don't apply to it.

## The mode and the session

`--hw off|auto|videotoolbox` picks the mode. `off` is the default. Because
the option goes through the library's own parser, the ffmpeg wrapper takes
`hw=videotoolbox` in `-yah265-params` unchanged. A library caller sets the mode with `n265_set_hw` before the open and reads
what opened from `n265_encoder_backend`. For test scripts the `N265_HW`
environment variable overrides both. The meson option `videotoolbox` builds
the mode on macOS. Elsewhere callers need no platform checks, because a
stub's open fails with a reason.

The session requires the hardware encoder. yah265 refuses VideoToolbox's software encoder and any session that won't
say it's hardware. Under `videotoolbox` a refusal fails the encode. Under
`auto` it prints one line naming why before the encode runs on yah265's own
encoder.

Each input picture is copied into a buffer from the session's pool, because
the session holds pictures past the call while the caller reuses its planes.
8-bit input codes as Main. 10-bit input and `--output-depth 10` code as
Main10 from a 10-bit biplanar buffer.

On the session's thread
the output callback turns each length-prefixed sample into Annex B. The
encode call hands the samples out in decode order as one access unit each.
The VPS, SPS and PPS come from the format description and lead the first
access unit. When ffmpeg writes an MP4, it asks for the headers before the first picture.
That call opens a one-frame probe session with the same
settings and takes the headers from it. The reorder depth a container
needs comes from that SPS.

yah265's scene-cut runs on each picture as it arrives, with the lookahead's
half-size costs and its cut rule. A cut forces a key frame with no added
latency. Because there's no frame of lookahead, there's no flash guard
either. A one-frame flash costs an extra key frame.

`--keyint` counts from the last key frame, as in software. The hardware's
own interval is set to twice that as a backstop. I set it that far out because the hardware puts its
key a couple of frames short of the interval it's given. On sintel_720p
the hardware's key frames fall on exactly the pictures the software
encoder picks. `N265_HW_SC=0` leaves the key frames to the hardware. The
first key frame is an IDR and later ones are CRAs.

The session writes no HDR10 SEI even when it's given the mastering display
and light level. yah265 builds those two messages from `--master-display` and `--max-cll` and
sends them with the parameter sets.

## What maps and what doesn't

The [option map](../src/hw/vt_map.c) is one C table. Because the warnings read that table, the text and the behavior can't drift
apart.

| yah265 | on the hardware | class |
|---|---|---|
| `--keyint` | yah265's interval, with the hardware's at twice it as a backstop | maps |
| `--bitrate` | average bitrate | maps |
| `--colorprim`, `--transfer`, `--colormatrix` | the session's color properties, for the values VideoToolbox names | maps |
| `--range` | video or full range buffers at 10 bits | maps |
| `--sar` | pixel aspect ratio | maps |
| `--master-display`, `--max-cll` | yah265's own SEI beside the parameter sets | maps |
| `--repeat-headers` | VPS, SPS, PPS and that SEI before every key frame | maps |
| `--threads` | nothing, because it's a schedule | maps |
| `--vbv-maxrate`, `--vbv-bufsize` | a data-rate limit of maxrate over a window of bufsize / maxrate seconds | approximated |
| `--crf` | the 0-to-1 quality key through a rate-matched table | approximated |
| `--qp` | minimum and maximum frame QP fixed at N | approximated |
| `--bframes` | frame reordering on or off | approximated |
| `--tune zerolatency` | real-time mode | approximated |
| `--preset`, `--level`, `--high-tier`, `--ref`, `--b-adapt`, the GOP options, `--rc-lookahead`, `--cutree`, `--aq`, `--ctu`, the transform and RDOQ options, the chroma offsets, weighted prediction, noise reduction, MCSTF, film grain, `--dup-threshold`, `--shot-window`, `--hdr-opt`, the CRF and QP bounds, `--vbv-init`, `--strict-cbr`, `--max-ausize-factor`, `--qpfile`, `--zones`, `--pass`, `--stats`, `--hash`, AUD, EOS, EOB, the timing and recovery SEI, `--temporal-layers`, `--videoformat`, `--overscan`, `--chromaloc`, `--display-window`, `--atc-sei`, `--info`, `--fingerprint`, `--vui-timing` | nothing | ignored |
| `--pcm`, `--lossless`, `--psnr`, `--ssim`, `--intra-refresh`, `--dhdr10-info`, the Dolby Vision options, `--dump-recon`, `--dump-recon-decode`, `--csv`, `--shot-stats`, 4:2:2 and 4:4:4 input | nothing | refused |

At open the encoder prints one block naming each ignored or approximated
option the user gave. Defaults print nothing. `--hw-strict` turns that
block into an error for scripts. A refused option fails `--hw
videotoolbox` and sends `--hw auto` to yah265's encoder. The CLI's own reports
and dumps fail in either mode. `--gop-parallel` codes serially.

Where the hardware won't take a fixed frame QP, `--qp N` runs as the
quality of `--crf N` and says so. A color value VideoToolbox has no name
for is left unsignaled with a line saying so.

The CRF table matches rate, not quality. Each point is the quality at which
the hardware spends what yah265's medium spends at that CRF, as the median
over eight corpus clips at 120 frames. The table is linear between points.

At CRF 28 the hardware's file is within 17% of yah265's on six of the eight
clips. On park_joy and sintel it's far larger, at 2.15x and 1.52x.

| CRF | 20 | 24 | 28 | 32 | 36 |
|---|--:|--:|--:|--:|--:|
| quality | 0.63 | 0.54 | 0.43 | 0.31 | 0.205 |

## The trade

I ran yah265's medium at `--crf 25` on ten clips from 720p to 2160p, then
ran the hardware at the average bitrate each encode reached. yah265's side
is the release build at auto threads on the 18-core Mac. Each side ran three
times and the table takes the median wall and CPU time. VMAF v1 scores both
by the project's display rule. The [measurement
CSV](data/hw-videotoolbox.csv) has the other columns. The [measuring
script](../scripts/hw_measure.py) reproduces it.

| clip | kbit/s | wall, hardware / yah265 | CPU, yah265 / hardware | VMAF v1, hardware minus yah265 | size, hardware / yah265 |
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

The hardware uses about a fortieth of yah265's CPU and less of the clock.
It's slower only on fourpeople, where yah265's encode is cheap and the
hardware still pays a fixed cost of about half a second to open the session
and read the input. For those savings the hardware scores below yah265 on all ten clips, even
though it spends a little more on nine of them. meridian_2160p is the worst case because the hardware won't code
4K at 60 frames a second that thin. It overshot the target by 70% and
still scored worst. I'd use the hardware mode when CPU matters more than
bits.

## What I chose and why

| choice | why |
|---|---|
| default `--hw off` | `auto` would swap in a different encoder where the checks in the tree assume yah265's |
| hardware only, never VideoToolbox's software encoder | the mode exists for the hardware's CPU savings. Without the hardware, yah265's own encoder is the software path |
| the option in the library's parser | the CLI and the ffmpeg wrapper take it the same way with no change to the wrapper |
| yah265's scene-cut and interval place the key frames | the two streams cut the same way because the key frames fall where the software encoder's do |
| `--crf` onto the quality key through a fixed table, with a warning | the hardware has no CRF and a monotone table is cheap |
| warn once on options the user named, with `--hw-strict` to refuse | a default run shouldn't print noise. Scripts need a way to fail loudly |
| `auto` falls back with one line and `videotoolbox` never does | a forced mode that quietly ran something else would mislead |
| yah265's own HDR10 SEI | the session takes the values and writes neither message |

## Measuring it

- The hardware isn't byte-stable from run to run. Don't put its streams
  through a determinism check.
- Because a raw stream from the hardware has no VUI timing, a decoder
  guesses the rate. Score it by frame index. The measuring script feeds
  libvmaf through ffmpeg with passthrough frame timing.
- CPU counts the encoder process only and leaves out work the hardware does
  elsewhere.
