# HDR

yah265 writes HDR10, HDR10+, HLG and Dolby Vision profile 8 as Main10 HEVC.
All of it is signalling. The colour flags and metadata tell a player what the
pictures are, and nothing in them steers how the encoder codes a picture.

| format | what goes in the stream | options |
|---|---|---|
| HDR10 | PQ transfer, BT.2020 primaries and matrix in the VUI, plus the mastering display (ST 2086) and content light level SEI in every key frame | `--transfer smpte2084 --colorprim bt2020 --colormatrix bt2020nc --master-display ... --max-cll ...` |
| HDR10+ | HDR10, plus an ST 2094-40 message in every picture | HDR10's options and `--dhdr10-info <json>` |
| HLG | HLG transfer in the VUI, or BT.2020 with an alternative transfer SEI naming HLG for SDR decoders | `--transfer arib-std-b67`, or `--transfer bt2020-10 --atc-sei arib-std-b67` |
| Dolby Vision 8.1 | HDR10, plus a Dolby Vision RPU in every picture | HDR10's options and `--dolby-vision-rpu <file> --dolby-vision-profile 8.1` |
| Dolby Vision 8.2 | SDR BT.709, plus an RPU in every picture | `--transfer bt709 --colorprim bt709 --colormatrix bt709` and the two Dolby Vision options with `8.2` |
| Dolby Vision 8.4 | HLG, plus an RPU in every picture | HLG's options and the two Dolby Vision options with `8.4` |

`--video-signal-type-preset BT2100_PQ_YCC:P3D65x1000n0005` sets HDR10's colour
flags and a 1000-nit P3 mastering display in one go.

Dolby Vision profile 5 isn't offered. Its base layer is in Dolby's IPTPQc2
colour space, which nothing in this toolchain can produce or check.

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
  `dovi_tool generate` from a colourist's Dolby Vision XML.

Entry *i* of either file goes with frame *i* of the input, whatever order the
encoder codes the pictures in. `--seek` skips entries along with frames, and
`--gop-parallel` gives each segment its own part of the file. If a file runs
out before the input does, the encode still writes the stream but exits with
an error, because pictures without metadata break HDR10+ or Dolby Vision
playback there. A file with entries left over gets a warning, since a
`--frames` run over part of a title is the usual reason.

Both readers check what they read. HDR10+ values must fall inside the HDR10+
profiles' ranges, and HDR10+ needs a 10-bit HDR10 encode under it. Every RPU's
header must say profile 8, and `--dolby-vision-profile` must match the colour
flags and a 10-bit encode. A command line that would mislabel the stream is
refused before it writes anything. Either file can come from a pipe.

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
themselves, so a player needs the container to tell it, in MP4's `dvcC` box
or Matroska's equivalent. ffmpeg's stream copy from a raw `.hevc` doesn't
write that box. A Dolby Vision-aware muxer such as MP4Box or mkvmerge does.

## How it was checked

- **Against the tools' own injection.** Encoding with `--dhdr10-info` and
  `--dolby-vision-rpu` gives the same NAL units, byte for byte, as encoding
  without them and then running `hdr10plus_tool inject` and
  `dovi_tool inject-rpu` on the result. The only extras on the tools' side are
  an access unit delimiter per picture and the encoder's version SEI.
- **Round trips.** `dovi_tool extract-rpu` and `hdr10plus_tool extract` on an
  encode give back the input metadata exactly. That holds with B-frames
  reordering the pictures, with `--seek`, and across `--gop-parallel`
  segments.
- **Against another encoder.** The ST 2094-40 writer reproduces a profile B
  message from a real HDR10+ stream byte for byte. The unit test pins it.
- **Players.** ffmpeg parses the HDR10+ message and the Dolby Vision RPU of
  every frame, and decodes the streams with errors fatal.
- **Real metadata.** The Dolby Vision test uses Netflix's own grade for Sol
  Levante: the Dolby Vision XML published with it under CC BY 4.0, turned
  into profile 8.1 RPUs by `dovi_tool`.
- **Nothing else moves.** Without these options the output is byte-identical
  to the encoder before they existed. With them, a CRF encode codes the same
  pictures. Under a bitrate or VBV target the metadata's bytes count toward it
  like any other part of the access unit, so the rate control spends slightly
  less on the pictures.
