---
title: Options - yah265
description: Every option of the y265 command line, grouped by what it's for, with its default.
---

# Options

This page lists every option `y265` takes. It has <!--gen:options-build-->100 entries from `y265 0.0.1, release build 12875ae0`<!--/gen:options-build-->. The option names and arguments come from the encoder's own `--help`. A [script](scripts/site_front.py) fails its check when this page and the help disagree. From ffmpeg, the same names go through `-yah265-params` without the dashes, as the guide's [From ffmpeg](guide.md#from-ffmpeg) section shows.

Many switches also take a `--no-` form that turns them off. "The preset's" means the default depends on `--preset` and sometimes on the picture size. Run `y265 --show-config` with your input to see what a run resolves to. The [encoding guide](guide.md) explains how the options fit together.

<!--gen:options-->
## Input and output

| option | default | what it does |
|---|---|---|
| `-o <file>` | `out.hevc` | The output file, a raw Annex-B HEVC stream. |
| `--frames <n>` | all | Encode at most this many frames. |
| `--seek <n>` | 0 | Skip this many frames of the input first. Timestamps start at zero on the first frame coded. |
| `--output-depth <8\|10>` | the input's | Code at this sample width. `10` on 8-bit input writes Main10. `8` on 10-bit input drops two bits. |
| `--dither` | off | With 10-bit input and `--output-depth 8`, add an ordered dither before dropping the two bits. |
| `--input-res <WxH>` | none | Picture size of raw YUV input, such as `1920x1080`. Naming it selects the raw reader. |
| `--input-csp <c>` | none | Chroma format of raw input: `i420`, `i422` or `i444`. |
| `--fps <N[/D]>` | none | Frame rate of raw input, as `30`, `30000/1001` or `29.97`. |
| `--input-depth <8\|10>` | `8` | Sample width of raw input. 10-bit samples are 16-bit little-endian. |
| `--dump-recon <file>` | off | Write the encoder's reconstruction as raw YUV, for comparing against a decoder. |

## Presets and tunes

| option | default | what it does |
|---|---|---|
| `--preset <name>` | `medium` | The speed and quality point: `fast`, `medium`, `slow` or `slower`. It sets defaults, and any option you give wins over it. |
| `-t, --tune <name>` | none | `zerolatency` turns off B-frames and the lookahead. The other four x265 tune names are refused with a reason. |

## Rate control

| option | default | what it does |
|---|---|---|
| `--crf <f>` | off | Constant rate factor. Naming it turns CRF on. The scale is yah265's own and moves 1.8 QP each time the frame rate doubles. |
| `--qp <n>` | `26` | Fixed QP, 0 to 51 at 8 bits and -12 to 51 at 10. It's the mode when no rate option is given. |
| `--bitrate <k>` | off | Target mean bitrate in kbit/s, in one pass. It wins over `--crf`. |
| `--vbv-maxrate <k>` | off | Decoder buffer fill rate in kbit/s. With `--bitrate` it makes capped VBR, and with `--crf` capped CRF. Needs `--vbv-bufsize`. |
| `--vbv-bufsize <k>` | off | Decoder buffer size in kbit. Needs `--vbv-maxrate`. |
| `--vbv-init <f>` | `0.9` | How full the buffer starts, as a fraction of its size. |
| `--strict-cbr` | off | Hold a constant rate under `--bitrate` with a buffer. Filler data pads any picture that comes in under its share. |
| `--max-ausize-factor <f>` | `1.0` | Bound one coded picture to this fraction of the buffer, from 0.5 up. Needs a buffer. |
| `--crf-min <f>` | none | Lowest rate factor a `--crf` encode may use. |
| `--crf-max <f>` | none | Highest rate factor a `--crf` encode may use. The encoder reports an underflow when the bound is too low for the buffer. |
| `--qpmin <n>` | none | Lowest frame QP the rate control may choose. |
| `--qpmax <n>` | none | Highest frame QP the rate control may choose. |
| `--pass <n>` | off | Two-pass. `1` writes a statistics file, `2` reads it and plans the whole file, `3` does both. Pass 2 needs `--bitrate`. |
| `--stats <file>` | none | The statistics file `--pass` writes or reads. |
| `--aq <0\|1>` | `1` | Adaptive quantization, which moves bits toward flat and dark areas. |
| `--cutree <0\|1>` | `1` | Lookahead QP offsets that favor blocks later frames predict from. |
| `--cbqpoffs <n>` | the preset's | Cb quantizer offset from luma, -12 to 12. A negative value spends more bits on chroma. |
| `--crqpoffs <n>` | the preset's | The same for Cr. |
| `--lossless` | off | Mathematically lossless coding. Rate options are ignored. |
| `--pcm` | off | Code every block as raw samples. A test mode with very large output. |

## Steering an encode by frame

| option | default | what it does |
|---|---|---|
| `--qpfile <file>` | none | Force frame types and QPs from a file, one `<frame> <type> [<qp>]` line each. Types are `I`, `K`, `i` and `-`. |
| `--zones <spec>` | none | QP or bitrate changes over frame ranges, as `start,end,q=N` or `start,end,b=F` joined by `/`. |

## GOP and references

| option | default | what it does |
|---|---|---|
| `--keyint <n>` | `250` | Key frame period in frames. |
| `--open-gop` | on | Code the periodic key frame as a CRA and keep the B-frames that straddle it. `--no-open-gop` writes an IDR there. A scene cut is an IDR either way. |
| `--radl <n>` | `0` | How many of a CRA's leading pictures stay decodable when playback starts at that CRA. |
| `--bframes <0..15>` | `7` | B-frames between anchors, 0 to 15. Above 7, `--rc-lookahead` must be at least one more. |
| `--b-adapt <0-4>` | `1` | How the B run length is chosen: `0` fixed, `1` from motion, `4` from a bit estimate. `2` and `3` are refused. |
| `--ref <n>` | the preset's | References per list, 1 to 5. |
| `--rc-lookahead <1..60>` | the preset's | Lookahead depth in frames, 1 to 60. It's also the encoder's latency. |
| `-w, --weightp / --no-weightp` | on from `medium` up | Weighted prediction in P slices, which helps fades and lighting changes. |
| `--weightb / --no-weightb` | off | Weighted prediction in B slices. |
| `--dup-threshold <f>` | `0` | How close a frame must be to a reference to be coded as a repeat. `0` takes exact repeats only. |

## Block structure and quantization

| option | default | what it does |
|---|---|---|
| `--ctu <16\|32\|64>` | the preset's | Coding tree block size: 16, 32 or 64. |
| `--tu-inter-depth <1..4>` | `2` | Transform tree levels an inter block may use, 1 to 4. `--tu-intra-depth` is the same for intra. |
| `--rdoq-level <0\|1\|2>` | the preset's | Rate-distortion optimized quantization: `0` off, `1` at the final encode of a block, `2` in every decision. |

## Threads

| option | default | what it does |
|---|---|---|
| `--threads <n>` | `0` | Worker threads. `0` starts one per online CPU. At `1`, two runs of a command write the same bytes. |
| `--gop-parallel <n>` | `0` | Code a long input as segments on this many encoder instances and join them at key frames. The stream differs from the serial one. |

## Shot-aware options

| option | default | what it does |
|---|---|---|
| `--shot-window <n>` | `0` | Buffer up to this many frames so a whole shot can be planned before it's coded. `--crf` only, at most 300. It adds that many frames of latency. |
| `--shot-stats <file>` | none | Write one JSON line of statistics per shot. The stream doesn't change. |

## Film grain, denoising and filters

| option | default | what it does |
|---|---|---|
| `--film-grain <s>` | off | Estimate the source's grain and describe it in an SEI message. A decoder that supports the message adds grain back. The value scales the estimate. 8-bit 4:2:0 only. |
| `--film-grain-denoise <0\|1>` | `0` | With `--film-grain`, also remove the grain from the picture the encoder codes. |
| `--nr-intra <0-2000>` | `100` at `medium`, else `0` | Noise reduction strength on intra coefficients, 0 to 2000. |
| `--nr-inter <0-2000>` | `100` at `medium`, else `0` | The same on inter coefficients. |
| `--mcstf` | off | Motion-compensated temporal filter on the source, for noisy material. |
| `--mcstf-ref-range <1-4>` | `2` | Neighbor frames on each side the filter reads, 1 to 4. |
| `--selective-mcstf` | off | Skip the filter on pictures that measure clean. |

## Stream structure and SEI

| option | default | what it does |
|---|---|---|
| `--repeat-headers` | off | Write the parameter sets at every key frame, for a receiver that joins mid-stream. |
| `--aud` | off | An access unit delimiter at the head of every picture. |
| `--eos` | off | An end-of-sequence NAL before each IDR after the first and at the end of the stream. |
| `--eob` | off | An end-of-bitstream NAL as the last NAL. |
| `--info / --no-info` | on | An SEI message naming the encoder, its version, its license and the options the run resolved. |
| `--no-fingerprint` | fingerprint on | Drop the stream fingerprint, the mark that survives when SEI messages are stripped. |
| `--hash <0-3>` | `0` | A hash of each decoded picture in an SEI message: `1` MD5, `2` CRC, `3` checksum. |
| `--idr-recovery-sei` | off | A recovery point SEI at every key frame. |
| `--pic-struct <n>` | off | Frame doubling or tripling in the picture timing SEI: `0`, `7` or `8`. Needs a buffer. |
| `--hrd-concat` | off | Mark the first key frame as spliced onto another stream. Needs a buffer. |
| `--temporal-layers` | off | Put unreferenced B-frames in a second temporal layer. A player can then drop them and halve the frame rate. |
| `--level <l>` | derived | Signal this level, from `1` to `6.2`. Refused when the stream doesn't fit it. |
| `--high-tier` | off | Signal the High tier, which exists from level 4 up. |
| `--vui-timing` | on | Write the frame rate into the stream. `--no-vui-timing` leaves it out. |

## Color and HDR

| option | default | what it does |
|---|---|---|
| `--video-signal-type-preset <name[:volume]>` | none | One name for a whole set of color flags, such as `BT2100_PQ_YCC`, with an optional mastering display after a colon. |
| `--colorprim <p>` | none | Color primaries, such as `bt709` or `bt2020`. |
| `--transfer <t>` | none | Transfer characteristics, such as `bt709`, `smpte2084` for PQ or `arib-std-b67` for HLG. |
| `--colormatrix <m>` | none | Matrix coefficients, such as `bt709` or `bt2020nc`. |
| `--range <r>` | none | `limited` or `full` sample range. |
| `--videoformat <f>` | none | `component`, `pal`, `ntsc`, `secam`, `mac` or `unknown`. |
| `--chromaloc <n>` | none | Chroma sample location, 0 to 5. BT.2020 and BT.2100 ask for 2. |
| `--overscan <o>` | `unknown` | `show` or `crop`. |
| `--sar <w:h\|n>` | none | Sample aspect ratio, as `w:h` or a table index from 0 to 16. |
| `--display-window <l,t,r,b>` | none | A display cropping region in pixels, as `left,top,right,bottom`. |
| `--master-display <s>` | none | Mastering display SEI from ST 2086, as `G(x,y)B(x,y)R(x,y)WP(x,y)L(max,min)`. |
| `--max-cll <c,f>` | none | Content light level SEI, as `cll,fall` in nits. |
| `--hdr10 / --no-hdr10` | on once either is named | Force the two HDR10 messages on or off. |
| `--no-cll` | off | Drop the content light level message and keep the mastering display. |
| `--atc-sei <t>` | none | The transfer an SDR decoder should assume instead. An HLG stream uses it. |
| `--dhdr10-info <json>` | none | HDR10+ dynamic metadata from a JSON file, one message per picture. |
| `--dolby-vision-rpu <file>` | none | A Dolby Vision profile 8 RPU per picture, from a file. Needs `--dolby-vision-profile`. |
| `--dolby-vision-profile <p>` | none | `8.1`, `8.2` or `8.4`. The color flags must match it. |
| `--hdr-opt / --no-hdr-opt` | on for PQ and HLG | The HDR tuning of `medium`, on or off. `--hdr10-opt` is accepted as another name for it. |

## Reports

| option | default | what it does |
|---|---|---|
| `--psnr` | off | Report PSNR at the end of the run. The stream doesn't change. |
| `--ssim` | off | Report SSIM on luma at the end of the run. |
| `--csv <file>` | none | Write run statistics to a CSV file. |
| `--csv-log-level <0\|1\|2>` | `0` | `0` writes one row for the run, `1` adds a row per frame, `2` adds the rate control's columns. |
| `--verbose, -v` | off | Print the resolved configuration at the start of the encode. |
| `--show-config` | off | Print the resolved configuration and exit without encoding. |
| `--version` |  | Print the version. |

## Refused by name

| option | default | what it does |
|---|---|---|
| `--intra-refresh` | refused | Periodic intra refresh isn't built. The encoder says why and exits. |
| `--scaling-list <s>` | refused | Quantizer scaling lists aren't built. The encoder says why and exits. |
<!--/gen:options-->
