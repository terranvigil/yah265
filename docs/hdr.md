# HDR

yah265 writes HDR10, HDR10+, HLG and Dolby Vision profile 8 as Main10 HEVC.
The color flags and metadata tell a player what the pictures are. At preset
medium the transfer also changes how the pictures are coded, as
[Tuning for HDR](#tuning-for-hdr) describes.

| format | what goes in the stream | options |
|---|---|---|
| HDR10 | PQ transfer, BT.2020 primaries and matrix in the VUI, plus the mastering display (ST 2086) and content light level SEI in every key frame | `--transfer smpte2084 --colorprim bt2020 --colormatrix bt2020nc --master-display ... --max-cll ...` |
| HDR10+ | HDR10, plus an ST 2094-40 message in every picture | HDR10's options and `--dhdr10-info <json>` |
| HLG | HLG transfer in the VUI, or BT.2020 with an alternative transfer SEI naming HLG for SDR decoders | `--transfer arib-std-b67`, or `--transfer bt2020-10 --atc-sei arib-std-b67` |
| Dolby Vision 8.1 | HDR10, plus a Dolby Vision RPU in every picture | HDR10's options and `--dolby-vision-rpu <file> --dolby-vision-profile 8.1` |
| Dolby Vision 8.2 | SDR BT.709, plus an RPU in every picture | `--transfer bt709 --colorprim bt709 --colormatrix bt709` and the two Dolby Vision options with `8.2` |
| Dolby Vision 8.4 | HLG, plus an RPU in every picture | HLG's options and the two Dolby Vision options with `8.4` |

`--video-signal-type-preset BT2100_PQ_YCC:P3D65x1000n0005` sets HDR10's color
flags and a 1000-nit P3 mastering display in one go.

Dolby Vision profile 5 isn't offered. Its base layer is in Dolby's IPTPQc2
color space, which nothing in this toolchain can produce or check.

## Dynamic metadata comes from a file

HDR10+ and Dolby Vision describe the content scene by scene, and yah265 doesn't
measure it. The metadata comes from the grade, in the files the two common
tools write:

- `--dhdr10-info` reads the JSON that
  [hdr10plus_tool](https://github.com/quietvoid/hdr10plus_tool) writes, for
  example `hdr10plus_tool extract master.hevc -o meta.json`. Profile A and
  profile B (with a Bézier tone curve) both work, with one window per picture.
- `--dolby-vision-rpu` reads the RPU file that
  [dovi_tool](https://github.com/quietvoid/dovi_tool) writes. That's
  `dovi_tool extract-rpu` from an existing Dolby Vision stream, or
  `dovi_tool generate` from a colorist's Dolby Vision XML.

Entry *i* of either file goes with frame *i* of the input, whatever order the
encoder codes the pictures in. `--seek` skips entries along with frames, and
`--gop-parallel` gives each segment its own part of the file. If a file runs
out before the input does, the encode still writes the stream but exits with
an error, because pictures without metadata break HDR10+ or Dolby Vision
playback there. A metadata NAL lost to a failed allocation fails the run the
same way. A file with entries left over gets a warning, since a
`--frames` run over part of a title is the usual reason.

Both readers check what they read. HDR10+ values must fall inside the HDR10+
profiles' ranges, and HDR10+ needs a 10-bit HDR10 encode under it. Every RPU's
header must say profile 8, and `--dolby-vision-profile` must match the color
flags and a 10-bit encode. A command line that would mislabel the stream is
refused before it writes anything. Either file can come from a pipe.

Dolby Vision without a VBV, and 8.1 without `--master-display`, get a warning
and still encode. Deliveries are normally HRD-constrained. An 8.1 base
layer is a complete HDR10 stream. A CRF encode is still a valid one.

## Encoding HDR10+ and Dolby Vision 8.1

```
ffmpeg -i master.mov -pix_fmt yuv420p10le -strict -1 -f yuv4mpegpipe - |
  n265 - -o out.hevc --crf 22 \
    --colorprim bt2020 --transfer smpte2084 --colormatrix bt2020nc \
    --master-display 'G(13250,34500)B(7500,3000)R(34000,16000)WP(15635,16450)L(10000000,1)' \
    --max-cll 1000,400 \
    --dhdr10-info meta.json \
    --dolby-vision-rpu RPU.bin --dolby-vision-profile 8.1
```

A raw HEVC stream has nowhere to say "this is Dolby Vision" except the RPUs
themselves. A player needs the container to tell it, in MP4's `dvcC` box or
Matroska's equivalent. ffmpeg's stream copy from a raw `.hevc` doesn't
write that box. A Dolby Vision-aware muxer such as MP4Box or mkvmerge does.

## Tuning for HDR

A stream says PQ with `--transfer smpte2084`. It says HLG with `--transfer
arib-std-b67` or `--atc-sei arib-std-b67`. On either one, preset medium codes
chroma six quantizer steps finer than luma and turns off the extra psy weight
on dark blocks. Wide-gamut chroma stays in a narrow band around its midpoint.
At luma's quantizer it loses far more than SDR chroma does. `--hdr-opt` turns the
tuning on for a stream that doesn't name either transfer, and `--no-hdr-opt`
turns it off. `--hdr10-opt` is the same switch under x265's name. A
`--cbqpoffs` or `--crqpoffs` you name yourself still wins. From 720p up,
medium on HDR also keeps the full intra mode rank that SDR gives up for
speed, and at 2160p it keeps the intra transform split too. The other
presets code HDR like SDR.

## Quality on HDR content

We scored yah265 and x265 with Netflix's HDR-VMAF v1.1.8 on two HDR10
titles from Netflix Open Content: 10 seconds of Sol Levante
and 5 seconds of Sparks. Sol Levante is 24p animation. Sparks is 60p live
action. Both encoders ran at preset medium
with the same HDR10 signaling. yah265 ran at `--threads 6`, and x265 at
`--pools 6 --frame-threads 4` with no `--hdr10-opt`. Scores use HDR-VMAF's 3H
model at 1080p and its 1.5H model at 2160p, in the high frame rate variant
for Sparks. The table gives yah265's BD-rate against x265 on HDR-VMAF. A
negative number is the share of bitrate yah265 saves at equal quality.

| title | 1080p | 2160p |
|---|--:|--:|
| Sol Levante | -16.4% | -12.3% |
| Sparks | -10.9% | -12.7% |

Those numbers predate the HDR tuning above. The table below reads the current
binary the same way, with six of our rungs against x265's five. The HLG rows
are native HLG footage, described under [HLG](#hlg). x265's `--hdr10-opt` is
a PQ option and has no HLG reading.

| title | against x265 | against x265 `--hdr10-opt` |
|---|--:|--:|
| Sol Levante 1080p | -23.0% | -5.5% |
| Sol Levante 2160p | -17.7% | -5.4% |
| Sparks 1080p | -23.2% | -18.3% |
| Sparks 2160p | -24.2% | -18.4% |
| EBU / DVB HLG 1080p | -38.8% | - |
| EBU / DVB HLG 2160p | -34.6% | - |

`--hdr10-opt` is worth 6-19% to x265 on HDR-VMAF. Without the tuning yah265
was level with it on Sol Levante, and with it yah265 is 5% ahead there. At
1080p every cell reads ahead of the first tuned binary. At 2160p Sol Levante
is 1.2 points behind it and Sparks 0.8: the rest of the speed work medium
took is still in. This table ran yah265 at `--threads 4`. On the Sparks
1080p ladder that writes the same streams as `--threads 6`.
`scripts/hdr_bd.py` runs the ladders and the BD reading.

x265 ran CRF 20 to 32 in steps of 3. The two CRF scales don't line up on this
material: at the same CRF yah265 writes a much smaller file on Sol Levante. So
yah265 ran CRF 11 to 32 in the same steps. Each cell uses the five rungs
whose HDR-VMAF range best matches x265's. Those cover 92% or more of x265's
range in every cell. The tuned table uses CRF 11 to 26 on Sol Levante, 17
to 32 on Sparks and 20 to 35 on the HLG clip. Those six rungs cover 95% or
more of x265's range.

### HLG

The HLG rows read native HLG footage. The source is the EBU / DVB 8K HLG
test clip, published by DVB under CC BY 4.0 (credit EBU / DVB). It is HEVC
Main10 HLG10 in BT.2020, 7680x4320 at 50 fps. The download is the clip's
37 Mbit/s DASH rendition. We took frames 600 to 899 (12 s to 18 s), which
hold the clip's one cut. A near-static shot falls on either side of it: a
harbor in daylight and a sunset over water. They were scaled down to 3840x2160
and 1920x1080 in 10-bit 4:2:0 with zscale's Lanczos filter, HLG and BT.2020
kept. Both encoders signal HLG with `--transfer arib-std-b67`. HDR-VMAF has
no HLG transfer, so the reference and every decoded picture go through the
same HLG to PQ conversion at a 1000-nit peak before scoring. At 50 fps the
score uses the high frame rate model. yah265 ran at `--threads 4`, and x265
at `--pools 4 --frame-threads 4`.

The HLG tuning is worth 14.1% at 1080p and 15.0% at 2160p on HDR-VMAF.
That's medium against medium with `--no-hdr-opt`, at the same six CRFs.
Without it yah265 still saves 29.1% and 23.4% against x265. PSNR-Y reads
much closer. At 2160p yah265 is within 1% of x265 on it. The tuning costs
2-3% of PSNR-Y at both sizes, because it moves bits from luma to chroma.

CAMBI reads yah265's mean about 0.25 higher than x265's at the same bitrate on
this clip. Both stay near 2 to 3, far below the level where banding shows.

Earlier HLG rows came from Sol Levante and Sparks re-coded from PQ to HLG.
They read -22.4% on both. The native clip reads well ahead of that. It is
two near-static shots. Its source is itself an HEVC encode, and the
downscale averages away most of what noise is left. That makes it easy
material, and a lead on it shouldn't be read as a lead on all HLG.

Banding was read with HDR-VMAF's PQ CAMBI on the current binary. Sol
Levante's flat gradients band on both encoders at the lower rates. There
yah265's worst frames still read lower at the same bitrate, around 5 where
x265's reach 6. Its mean reads about 0.1 higher, because medium turns off the
dark psy weight on HDR. On Sparks yah265's mean reads higher than x265's at
every rate. Both stay far below the level where banding shows.

## How it was checked

- A CRF encode with `--dhdr10-info` and `--dolby-vision-rpu` gives the same
  NAL units, byte for byte, as the same encode without them run through
  `hdr10plus_tool inject` and `dovi_tool inject-rpu`. The tools add an access
  unit delimiter to each picture. The encoder's version SEI names the extra
  options. Those are the only differences.
- `dovi_tool extract-rpu` and `hdr10plus_tool extract` on an encode give back
  the input metadata exactly. That holds with B-frames reordering the
  pictures, with `--seek`, and across `--gop-parallel` segments.
- The ST 2094-40 writer reproduces a profile B message from another
  encoder's real HDR10+ stream byte for byte. A unit test holds it to that.
- ffmpeg parses the HDR10+ message and the Dolby Vision RPU of every frame.
  It decodes the streams with errors fatal.
- The Dolby Vision test uses Netflix's own grade for Sol Levante. That's the
  Dolby Vision XML published with it under CC BY 4.0, turned into profile 8.1
  RPUs by `dovi_tool`.
- Nothing else moves. Without these options the output is byte-identical
  to the encoder before they existed, except under `--temporal-layers`, where
  the NAL units written with each picture (the delimiter, timing SEI, hash
  and filler) now take that picture's temporal layer. With them, a CRF
  encode codes the same pictures. Under a bitrate or VBV target the
  metadata's bytes count toward it like any other part of the access unit,
  so the rate control spends slightly less on the pictures.
