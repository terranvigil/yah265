---
title: Options - yah265
description: The options of the y265 command line, grouped by what they're for, with their defaults.
---

# Options

This page lists the options `y265` takes. From ffmpeg, the same names go through `-yah265-params` without the dashes, as the guide's [From ffmpeg](guide.md#from-ffmpeg) section shows.

Many switches also take a `--no-` form that turns them off. "The preset's" means the default depends on `--preset` and sometimes on the picture size. Run `y265 --show-config` with your input to see what a run resolves to. See the [encoding guide](guide.md) for how they fit together.

<!--gen:options-->
## Input and output

| option | default | what it does |
|---|---|---|
| <span id="opt-o"></span>`-o <file>` | `out.hevc` | The output file, a raw Annex-B HEVC stream. |
| <span id="opt-frames"></span>`--frames <n>` | all | Encode at most this many frames. |
| <span id="opt-seek"></span>`--seek <n>` | 0 | Skip this many frames of the input first. Timestamps start at zero on the first frame coded. |
| <span id="opt-output-depth"></span>`--output-depth <8\|10>` | the input's | Code at this sample width. `10` on 8-bit input writes Main10. `8` on 10-bit input drops two bits. |
| <span id="opt-dither"></span>`--dither` | off | With 10-bit input and `--output-depth 8`, add an ordered dither before dropping the two bits. |
| <span id="opt-input-res"></span>`--input-res <WxH>` | none | Picture size of raw YUV input, such as `1920x1080`. Naming it selects the raw reader. |
| <span id="opt-input-csp"></span>`--input-csp <c>` | none | Chroma format of raw input: `i420`, `i422` or `i444`. |
| <span id="opt-fps"></span>`--fps <N[/D]>` | none | Frame rate of raw input, as `30`, `30000/1001` or `29.97`. |
| <span id="opt-input-depth"></span>`--input-depth <8\|10>` | `8` | Sample width of raw input. 10-bit samples are 16-bit little-endian. |
| <span id="opt-dump-recon"></span>`--dump-recon <file>` | off | Write the encoder's reconstruction as raw YUV, for comparing against a decoder. |

## Presets and tunes

| option | default | what it does |
|---|---|---|
| <span id="opt-preset"></span>`--preset <name>` | `medium` | The speed and quality point: `fast`, `medium`, `slow` or `slower`. It sets defaults, and any option you give wins over it. |
| <span id="opt-tune"></span>`-t, --tune <name>` | none | `zerolatency` turns off B-frames, the lookahead and cu-tree. The other four x265 tune names are refused with a reason. |

## Rate control

| option | default | what it does |
|---|---|---|
| <span id="opt-crf"></span>`--crf <f>` | off | Constant rate factor. Naming it turns CRF on. It goes 1.8 QP coarser each time the frame rate doubles, and is neutral at 25 fps. |
| <span id="opt-qp"></span>`--qp <n>` | `26` | Fixed QP, 0 to 51 at 8 bits and -12 to 51 at 10. It's the mode when no rate option is given. |
| <span id="opt-bitrate"></span>`--bitrate <k>` | off | Target mean bitrate in kbit/s, in one pass. It wins over `--crf`. |
| <span id="opt-vbv-maxrate"></span>`--vbv-maxrate <k>` | off | Decoder buffer fill rate in kbit/s. With `--bitrate` it makes capped VBR, and with `--crf` capped CRF. Needs `--vbv-bufsize`. |
| <span id="opt-vbv-bufsize"></span>`--vbv-bufsize <k>` | off | Decoder buffer size in kbit. Needs `--vbv-maxrate`. |
| <span id="opt-vbv-init"></span>`--vbv-init <f>` | `0.9` | How full the buffer starts, as a fraction of its size. |
| <span id="opt-strict-cbr"></span>`--strict-cbr` | off | Hold a constant rate under `--bitrate` with a buffer. Filler data pads any picture that comes in under its share. |
| <span id="opt-max-ausize-factor"></span>`--max-ausize-factor <f>` | `1.0` | Bound one coded picture to this fraction of the buffer, 0.5 to 1.0. Needs a buffer. |
| <span id="opt-crf-min"></span>`--crf-min <f>` | none | Lowest rate factor a `--crf` encode may use. |
| <span id="opt-crf-max"></span>`--crf-max <f>` | none | Highest rate factor a `--crf` encode may use. |
| <span id="opt-qpmin"></span>`--qpmin <n>` | none | Lowest frame QP the rate control may choose. |
| <span id="opt-qpmax"></span>`--qpmax <n>` | none | Highest frame QP the rate control may choose. |
| <span id="opt-pass"></span>`--pass <1\|2\|3>` | off | Two-pass. `1` writes a statistics file, `2` reads it and plans the whole file, `3` does both. Pass 2 needs `--bitrate`. |
| <span id="opt-stats"></span>`--stats <file>` | none | The statistics file `--pass` writes or reads. |
| <span id="opt-aq"></span>`--aq <0\|1>` | `1` | Adaptive quantization, which moves bits toward flat and dark areas. |
| <span id="opt-cutree"></span>`--cutree <0\|1>` | `1` | Lookahead QP offsets that favor blocks later frames predict from. |
| <span id="opt-cbqpoffs"></span>`--cbqpoffs <n>` | the preset's | Cb quantizer offset from luma, -12 to 12. A negative value spends more bits on chroma. |
| <span id="opt-crqpoffs"></span>`--crqpoffs <n>` | the preset's | The same for Cr. |
| <span id="opt-lossless"></span>`--lossless` | off | Lossless coding. Rate control, AQ and the loop filters are off. |
| <span id="opt-pcm"></span>`--pcm` | off | Code each block as raw samples. A test mode with very large output. |

## Steering an encode by frame

| option | default | what it does |
|---|---|---|
| <span id="opt-qpfile"></span>`--qpfile <file>` | none | Force frame types and QPs from a file, one `<frame> <type> [<qp>]` line each. Types are `I`, `K`, `i` and `-`. |
| <span id="opt-zones"></span>`--zones <spec>` | none | QP or bitrate changes over frame ranges, as `start,end,q=N` or `start,end,b=F` joined by `/`. |

## GOP and references

| option | default | what it does |
|---|---|---|
| <span id="opt-keyint"></span>`--keyint <n>` | `250` | Longest gap between key frames. A scene cut restarts the count. |
| <span id="opt-open-gop"></span><span id="opt-no-open-gop"></span>`--open-gop` | on | Code the periodic key frame as a CRA and keep the B-frames that straddle it. `--no-open-gop` writes an IDR there. A scene cut is an IDR either way. |
| <span id="opt-radl"></span>`--radl <n>` | `0` | How many of a CRA's leading pictures stay decodable when playback starts at that CRA. |
| <span id="opt-bframes"></span>`--bframes <0..15>` | `7` | B-frames between anchors, 0 to 15. Above 7, `--rc-lookahead` must be at least one more. |
| <span id="opt-b-adapt"></span>`--b-adapt <0-4>` | `1` | How the B run length is chosen: `0` fixed, `1` from motion. `2` to `4` are experimental. |
| <span id="opt-ref"></span>`--ref <n>` | `1` | References per list, 1 to 5. `slower` uses 2, and so does `medium` at 4K. |
| <span id="opt-rc-lookahead"></span>`--rc-lookahead <n>` | the preset's | Lookahead depth in frames, 1 to 60. It's also the encoder's latency. |
| <span id="opt-weightp"></span><span id="opt-no-weightp"></span>`-w, --weightp` | on, off at `fast` | Weighted prediction in P slices, which helps fades and lighting changes. `--no-weightp` turns it off. |
| <span id="opt-weightb"></span>`--weightb` | off | Weighted prediction in B slices. |
| <span id="opt-dup-threshold"></span>`--dup-threshold <f>` | `0` | How close a frame must be to a reference to be coded as a repeat. `0` takes exact repeats only. |

## Block structure and quantization

| option | default | what it does |
|---|---|---|
| <span id="opt-ctu"></span>`--ctu <16\|32\|64>` | the preset's | Coding tree block size: 16, 32 or 64. |
| <span id="opt-tu-inter-depth"></span>`--tu-inter-depth <1..4>` | `2` | Transform tree levels an inter block may use, 1 to 4. |
| <span id="opt-tu-intra-depth"></span>`--tu-intra-depth <1..4>` | `2` | Transform tree levels an intra block may use, 1 to 4. |
| <span id="opt-rdoq-level"></span>`--rdoq-level <0\|1\|2>` | the preset's | Rate-distortion optimized quantization: `0` off, `1` at the final encode of a block, `2` in all decisions. |

## Threads

| option | default | what it does |
|---|---|---|
| <span id="opt-threads"></span>`--threads <n>` | `0` | Worker threads. `0` starts one per online CPU. At `1`, two runs of a command write the same bytes. Above one thread the stream can differ between runs. |
| <span id="opt-gop-parallel"></span>`--gop-parallel <n>` | `0` | Code a long input as segments on this many encoder instances and join them at key frames. The stream differs from the serial one. |

## Hardware

| option | default | what it does |
|---|---|---|
| <span id="opt-hw"></span>`--hw <mode>` | `off` | `videotoolbox` encodes with the Mac's HEVC hardware through VideoToolbox. The stream is the hardware's, not this encoder's. Options it can't follow are listed once at the start. `auto` uses this encoder when the hardware isn't there, and `videotoolbox` fails instead. |
| <span id="opt-hw-strict"></span>`--hw-strict` | off | With `--hw`, refuse to encode when any option you gave can't be followed. |

## Shot-aware options

| option | default | what it does |
|---|---|---|
| <span id="opt-shot-window"></span>`--shot-window <n>` | `0` | Buffer up to this many frames so a whole shot can be planned before it's coded. `--crf` only, at most 300. It adds that many frames of latency. |
| <span id="opt-shot-stats"></span>`--shot-stats <file>` | none | Write one JSON line of statistics per shot. The stream doesn't change. |

## Film grain, denoising and filters

| option | default | what it does |
|---|---|---|
| <span id="opt-film-grain"></span>`--film-grain <s>` | off | Estimate the source's grain and describe it in an SEI message. A decoder that supports the message adds grain back. The value scales the estimate. 8-bit 4:2:0 only. |
| <span id="opt-film-grain-denoise"></span>`--film-grain-denoise <0\|1>` | `0` | With `--film-grain`, also remove the grain from the picture the encoder codes. |
| <span id="opt-nr-intra"></span>`--nr-intra <0-2000>` | `0` | Noise reduction strength on intra coefficients, 0 to 2000. |
| <span id="opt-nr-inter"></span>`--nr-inter <0-2000>` | `0` | The same on inter coefficients. |
| <span id="opt-mcstf"></span>`--mcstf` | off | Motion-compensated temporal filter on the source, for noisy material. |
| <span id="opt-mcstf-ref-range"></span>`--mcstf-ref-range <1-4>` | `2` | Neighbor frames on each side the filter reads, 1 to 4. |
| <span id="opt-selective-mcstf"></span>`--selective-mcstf` | off | Skip the filter on pictures that measure clean. |

## Stream structure and SEI

| option | default | what it does |
|---|---|---|
| <span id="opt-repeat-headers"></span>`--repeat-headers` | off | Write the parameter sets at each key frame, for a receiver that joins mid-stream. |
| <span id="opt-aud"></span>`--aud` | off | An access unit delimiter at the head of each picture. |
| <span id="opt-eos"></span>`--eos` | off | An end-of-sequence NAL before each IDR after the first and at the end of the stream. |
| <span id="opt-eob"></span>`--eob` | off | An end-of-bitstream NAL as the last NAL. |
| <span id="opt-no-info"></span><span id="opt-info"></span>`--no-info` | info on | Drop the SEI message naming the encoder, its version and its options. `--info` keeps it. |
| <span id="opt-no-fingerprint"></span><span id="opt-fingerprint"></span>`--no-fingerprint` | fingerprint on | Drop the stream fingerprint, the mark that survives when SEI messages are stripped. |
| <span id="opt-hash"></span>`--hash <0-3>` | `0` | A hash of each decoded picture in an SEI message: `1` MD5, `2` CRC, `3` checksum. |
| <span id="opt-idr-recovery-sei"></span>`--idr-recovery-sei` | off | A recovery point SEI at each key frame. |
| <span id="opt-pic-struct"></span>`--pic-struct <n>` | off | The picture timing SEI's pic_struct, 0 to 12. `7` doubles a frame and `8` triples it. Needs a buffer. |
| <span id="opt-hrd-concat"></span>`--hrd-concat` | off | Mark the first key frame as spliced onto another stream. Needs a buffer. |
| <span id="opt-temporal-layers"></span>`--temporal-layers` | off | Put unreferenced B-frames in a second temporal layer. A player can then drop them and halve the frame rate. |
| <span id="opt-level"></span>`--level <l>` | derived | Signal this level, from `1` to `6.2`. Refused when the stream doesn't fit it. |
| <span id="opt-high-tier"></span>`--high-tier` | off | Signal the High tier, which exists from level 4 up. |
| <span id="opt-vui-timing"></span><span id="opt-no-vui-timing"></span>`--vui-timing` | on | Write the frame rate into the stream. `--no-vui-timing` leaves it out. |

## Color and HDR

| option | default | what it does |
|---|---|---|
| <span id="opt-video-signal-type-preset"></span>`--video-signal-type-preset <name[:volume]>` | none | One name for a whole set of color flags, such as `BT2100_PQ_YCC`, with an optional mastering display after a colon. |
| <span id="opt-colorprim"></span>`--colorprim <p>` | none | Color primaries, such as `bt709` or `bt2020`. |
| <span id="opt-transfer"></span>`--transfer <t>` | none | Transfer characteristics, such as `bt709`, `smpte2084` for PQ or `arib-std-b67` for HLG. |
| <span id="opt-colormatrix"></span>`--colormatrix <m>` | none | Matrix coefficients, such as `bt709` or `bt2020nc`. |
| <span id="opt-range"></span>`--range <r>` | none | `limited` or `full` sample range. |
| <span id="opt-videoformat"></span>`--videoformat <f>` | none | `component`, `pal`, `ntsc`, `secam`, `mac` or `unknown`. |
| <span id="opt-chromaloc"></span>`--chromaloc <0..5>` | none | Chroma sample location, 0 to 5. BT.2020 and BT.2100 ask for 2. |
| <span id="opt-overscan"></span>`--overscan <o>` | `unknown` | `show` or `crop`. |
| <span id="opt-sar"></span>`--sar <w:h\|n>` | none | Sample aspect ratio, as `w:h` or a table index from 0 to 16. |
| <span id="opt-display-window"></span>`--display-window <l,t,r,b>` | none | A display cropping region in pixels, as `left,top,right,bottom`. |
| <span id="opt-master-display"></span>`--master-display <s>` | none | Mastering display SEI from ST 2086, as `G(x,y)B(x,y)R(x,y)WP(x,y)L(max,min)`. |
| <span id="opt-max-cll"></span>`--max-cll <c,f>` | none | Content light level SEI, as `cll,fall` in nits. |
| <span id="opt-hdr10"></span><span id="opt-no-hdr10"></span>`--hdr10 / --no-hdr10` | on once either is named | Force the two HDR10 messages on or off. |
| <span id="opt-no-cll"></span><span id="opt-cll"></span>`--no-cll` | off | Drop the content light level message and keep the mastering display. |
| <span id="opt-atc-sei"></span>`--atc-sei <t>` | none | The transfer a decoder that reads this SEI should use instead of the color flags' one. Backward-compatible HLG uses it. |
| <span id="opt-dhdr10-info"></span>`--dhdr10-info <json>` | none | HDR10+ dynamic metadata from a JSON file, one message per picture. Needs a 10-bit HDR10 encode. |
| <span id="opt-dolby-vision-rpu"></span>`--dolby-vision-rpu <file>` | none | A Dolby Vision profile 8 RPU per picture, from a file. Needs `--dolby-vision-profile`. |
| <span id="opt-dolby-vision-profile"></span>`--dolby-vision-profile <p>` | none | `8.1`, `8.2` or `8.4`. The color flags must match it. |
| <span id="opt-hdr-opt"></span><span id="opt-no-hdr-opt"></span><span id="opt-hdr10-opt"></span>`--hdr-opt / --no-hdr-opt` | on for PQ and HLG | The HDR tuning of `medium`, on or off. `--hdr10-opt` is accepted as another name for it. |

## Reports

| option | default | what it does |
|---|---|---|
| <span id="opt-psnr"></span>`--psnr` | off | Report PSNR at the end of the run. The stream doesn't change. |
| <span id="opt-ssim"></span>`--ssim` | off | Report SSIM on luma at the end of the run. |
| <span id="opt-csv"></span>`--csv <file>` | none | Write run statistics to a CSV file. |
| <span id="opt-csv-log-level"></span>`--csv-log-level <0\|1\|2>` | `0` | `0` writes one row for the run, `1` adds a row per frame, `2` adds the rate control's columns. |
| <span id="opt-verbose"></span>`--verbose, -v` | off | Print the resolved configuration at the start of the encode. |
| <span id="opt-show-config"></span>`--show-config` | off | Print the resolved configuration and exit without encoding. |
| <span id="opt-version"></span>`--version` |  | Print the version. |

## Not offered

| option | default | what it does |
|---|---|---|
| <span id="opt-intra-refresh"></span>`--intra-refresh` | refused | The encoder stops with an error. [Why](guide.md#left-out-on-purpose) |
| <span id="opt-scaling-list"></span>`--scaling-list <s>` | refused | The encoder stops with an error. [Why](guide.md#left-out-on-purpose) |
<!--/gen:options-->
