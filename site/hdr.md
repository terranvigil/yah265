---
title: HDR - yah265
description: How yah265 writes HDR10, HDR10+, HLG and Dolby Vision profile 8, where the dynamic metadata comes from and how it was checked.
---

# HDR

yah265 writes HDR10, HDR10+, HLG and Dolby Vision profile 8 as Main10 HEVC.
All of it is signaling. The color flags and metadata tell a player what the
pictures are. Nothing in them steers how the encoder codes a picture.

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
color space. Nothing in this toolchain can produce or check that.

## Dynamic metadata comes from a file

HDR10+ and Dolby Vision describe the content scene by scene. yah265 doesn't
measure it. The metadata comes from the grade, in the files the two common
tools write:

- `--dhdr10-info` reads the JSON that
  [hdr10plus_tool](https://github.com/quietvoid/hdr10plus_tool) writes.
  `hdr10plus_tool extract master.hevc -o meta.json` makes one. Profile A and
  profile B with a Bézier tone curve both work. Each takes one window per
  picture.
- `--dolby-vision-rpu` reads the RPU file that
  [dovi_tool](https://github.com/quietvoid/dovi_tool) writes. That's
  `dovi_tool extract-rpu` from an existing Dolby Vision stream, or
  `dovi_tool generate` from a colorist's Dolby Vision XML.

Entry *i* of either file goes with frame *i* of the input, whatever order the
encoder codes the pictures in. `--seek` skips entries along with frames.
`--gop-parallel` gives each segment its own part of the file. If a file runs
out before the input does, the encode still writes the stream. It exits with
an error, though, because pictures without metadata break HDR10+ or Dolby
Vision playback there. A metadata NAL lost to a failed allocation fails the
run the same way. A file with entries left over gets a warning. The usual
reason is a `--frames` run over part of a title.

Both readers check what they read. HDR10+ values must fall inside the HDR10+
profiles' ranges. HDR10+ also needs a 10-bit HDR10 encode under it. Every
RPU's header must say profile 8. `--dolby-vision-profile` must match the
color flags and a 10-bit encode. A command line that would mislabel the stream is
refused before it writes anything. Either file can come from a pipe.

Dolby Vision without a VBV gets a warning and still encodes. Deliveries are
normally HRD-constrained, but a CRF encode is still a valid one. An 8.1 encode
without `--master-display` gets a warning too, because an 8.1 base layer is a
complete HDR10 stream.

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

## Quality on HDR content

We scored yah265 and x265 with Netflix's HDR-VMAF v1.1.8 on two HDR10
titles from Netflix Open Content. We used 10 seconds of the 24p animation Sol
Levante and 5 seconds of the 60p live-action Sparks. Both encoders ran at preset medium
with the same HDR10 signaling. yah265 ran at `--threads 6`. x265 ran at
`--pools 6 --frame-threads 4` with no `--hdr10-opt`. Scores use HDR-VMAF's 3H
model at 1080p and its 1.5H model at 2160p. Sparks used the high frame rate
variant of each. The table gives yah265's BD-rate against x265 on HDR-VMAF. A
negative number is the share of bitrate yah265 saves at equal quality.

| title | 1080p | 2160p |
|---|--:|--:|
| Sol Levante | -16.4% | -12.3% |
| Sparks | -10.9% | -12.7% |

x265 ran CRF 20 to 32 in steps of 3. The two CRF scales don't line up on this
material: at the same CRF yah265 writes a much smaller file on Sol Levante. So
yah265 ran CRF 11 to 32 in the same steps. Each cell uses the five rungs
whose HDR-VMAF range best matches x265's. Those cover 92% or more of x265's
range in every cell.

Banding was read with HDR-VMAF's PQ CAMBI. Sol Levante's flat gradients band
on both encoders at the lower rates. yah265 bands less there at the same
bitrate. Its worst frames read about a point lower. They sit around 5 where
x265's reach 6. On Sparks yah265 reads slightly higher than x265 at the upper
rates and lower at the lower ones. Both stay far below the level where
banding shows.

## How it was checked

- **Against the tools' own injection.** A CRF encode with `--dhdr10-info`
  and `--dolby-vision-rpu` gives the same NAL units, byte for byte, as the
  same encode without them run through `hdr10plus_tool inject` and
  `dovi_tool inject-rpu`. They differ in two ways only. The tools add an access
  unit delimiter to each picture. The encoder's version SEI names the extra
  options.
- **Round trips.** `dovi_tool extract-rpu` and `hdr10plus_tool extract` on an
  encode give back the input metadata exactly. That holds with B-frames
  reordering the pictures, with `--seek`, and across `--gop-parallel`
  segments.
- **Against another encoder.** The ST 2094-40 writer reproduces a profile B
  message from a real HDR10+ stream byte for byte. The unit test pins it.
- **Players.** ffmpeg parses the HDR10+ message and the Dolby Vision RPU of
  every frame. It decodes the streams with errors fatal.
- **Real metadata.** The Dolby Vision test uses Netflix's own grade for Sol
  Levante. That's the Dolby Vision XML published with it under CC BY 4.0.
  `dovi_tool` turned it into profile 8.1 RPUs.
- **Nothing else moves.** Without these options the output is byte-identical
  to the encoder before they existed, except under `--temporal-layers`, where
  the NAL units written with each picture (the delimiter, timing SEI, hash
  and filler) now take that picture's temporal layer. With them, a CRF
  encode codes the same pictures. Under a bitrate or VBV target the
  metadata's bytes count toward it like any other part of the access unit.
  The rate control therefore spends slightly less on the pictures.
