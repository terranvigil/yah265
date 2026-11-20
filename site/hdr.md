---
title: HDR - yah265
description: How yah265 writes HDR10, HDR10+, HLG and Dolby Vision profile 8, how it tunes its coding for HDR and how its HDR quality compares with x265.
---

# HDR

yah265 writes HDR10, HDR10+, HLG and Dolby Vision profile 8 as Main10 HEVC. The color flags and metadata tell a player what the pictures are. At `--preset medium` the transfer function also changes how the pictures are coded, as [Tuning for HDR](#tuning-for-hdr) describes.

| format | what goes in the stream | options |
|---|---|---|
| HDR10 | PQ transfer, BT.2020 primaries and matrix in the VUI, plus the mastering display (ST 2086) and content light level SEI in every key frame | `--transfer smpte2084 --colorprim bt2020 --colormatrix bt2020nc --master-display ... --max-cll ...` |
| HDR10+ | HDR10, plus an ST 2094-40 message in every picture | HDR10's options and `--dhdr10-info <json>` |
| HLG | HLG transfer in the VUI, or BT.2020 with an alternative transfer SEI naming HLG for SDR decoders | `--transfer arib-std-b67`, or `--transfer bt2020-10 --atc-sei arib-std-b67` |
| Dolby Vision 8.1 | HDR10, plus a Dolby Vision RPU in every picture | HDR10's options and `--dolby-vision-rpu <file> --dolby-vision-profile 8.1` |
| Dolby Vision 8.2 | SDR BT.709, plus an RPU in every picture | `--transfer bt709 --colorprim bt709 --colormatrix bt709` and the two Dolby Vision options with `8.2` |
| Dolby Vision 8.4 | HLG, plus an RPU in every picture | HLG's options and the two Dolby Vision options with `8.4` |

`--video-signal-type-preset BT2100_PQ_YCC:P3D65x1000n0005` sets HDR10's color flags and a 1000-nit P3 mastering display in one go.

Dolby Vision profile 5 isn't offered. Its base layer is in Dolby's IPTPQc2 color space, which nothing in this toolchain can produce or check.

<figure class="diagram">
<svg class="d-wide" viewBox="0 0 720 250" role="img" aria-labelledby="hdr-fig-t hdr-fig-d">
<title id="hdr-fig-t">Where each piece of HDR signaling goes in the stream</title>
<desc id="hdr-fig-d">A stream drawn as a row of pictures. The color flags are in the sequence parameter set at the start. The mastering display and light level messages come at each key frame. An HDR10+ message and a Dolby Vision RPU come with every picture.</desc>
<rect class="d-accent" x="10" y="100" width="90" height="56" rx="6"/>
<text class="d-text" x="55" y="124" text-anchor="middle">SPS</text>
<text class="d-label" x="55" y="144" text-anchor="middle">VUI</text>
<rect class="d-fill" x="120" y="100" width="110" height="56" rx="6"/>
<text class="d-text" x="175" y="133" text-anchor="middle">key frame</text>
<rect class="d-box" x="250" y="100" width="90" height="56" rx="6"/>
<text class="d-text" x="295" y="133" text-anchor="middle">picture</text>
<rect class="d-box" x="360" y="100" width="90" height="56" rx="6"/>
<text class="d-text" x="405" y="133" text-anchor="middle">picture</text>
<rect class="d-fill" x="470" y="100" width="110" height="56" rx="6"/>
<text class="d-text" x="525" y="133" text-anchor="middle">key frame</text>
<rect class="d-box" x="600" y="100" width="90" height="56" rx="6"/>
<text class="d-text" x="645" y="133" text-anchor="middle">picture</text>
<text class="d-label" x="10" y="24">Once per stream</text>
<text class="d-label" x="10" y="44">transfer, primaries, matrix</text>
<line class="d-arrow" x1="55" y1="54" x2="55" y2="98"/>
<text class="d-label" x="200" y="24">At every key frame</text>
<text class="d-label" x="200" y="44">mastering display, light level</text>
<line class="d-arrow" x1="210" y1="54" x2="180" y2="98"/>
<line class="d-arrow" x1="380" y1="54" x2="520" y2="98"/>
<text class="d-label" x="360" y="222" text-anchor="middle">With every picture: an HDR10+ message and a Dolby Vision RPU</text>
<line class="d-line" x1="175" y1="190" x2="645" y2="190"/>
<line class="d-arrow" x1="175" y1="190" x2="175" y2="158"/>
<line class="d-arrow" x1="295" y1="190" x2="295" y2="158"/>
<line class="d-arrow" x1="405" y1="190" x2="405" y2="158"/>
<line class="d-arrow" x1="525" y1="190" x2="525" y2="158"/>
<line class="d-arrow" x1="645" y1="190" x2="645" y2="158"/>
</svg>
<svg class="d-narrow" viewBox="0 0 360 360" role="img" aria-labelledby="hdr-fig-t-n hdr-fig-d-n">
<title id="hdr-fig-t-n">Where each piece of HDR signaling goes in the stream</title>
<desc id="hdr-fig-d-n">A stream drawn as a column of pictures. The color flags are in the sequence parameter set at the start. The mastering display and light level messages come at each key frame. An HDR10+ message and a Dolby Vision RPU come with every picture.</desc>
<rect class="d-accent" x="40" y="8" width="110" height="52" rx="6"/>
<text class="d-text" x="95" y="32" text-anchor="middle">SPS</text>
<text class="d-label" x="95" y="51" text-anchor="middle">VUI</text>
<rect class="d-fill" x="40" y="74" width="110" height="36" rx="6"/>
<text class="d-text" x="95" y="97" text-anchor="middle">key frame</text>
<rect class="d-box" x="40" y="120" width="110" height="36" rx="6"/>
<text class="d-text" x="95" y="143" text-anchor="middle">picture</text>
<rect class="d-box" x="40" y="166" width="110" height="36" rx="6"/>
<text class="d-text" x="95" y="189" text-anchor="middle">picture</text>
<rect class="d-fill" x="40" y="212" width="110" height="36" rx="6"/>
<text class="d-text" x="95" y="235" text-anchor="middle">key frame</text>
<rect class="d-box" x="40" y="258" width="110" height="36" rx="6"/>
<text class="d-text" x="95" y="281" text-anchor="middle">picture</text>
<text class="d-label" x="196" y="22">Once per stream</text>
<text class="d-label" x="196" y="40">transfer, primaries,</text>
<text class="d-label" x="196" y="56">matrix</text>
<line class="d-arrow" x1="190" y1="34" x2="152" y2="34"/>
<text class="d-label" x="196" y="146">At every key frame</text>
<text class="d-label" x="196" y="164">mastering display,</text>
<text class="d-label" x="196" y="180">light level</text>
<line class="d-arrow" x1="190" y1="148" x2="152" y2="94"/>
<line class="d-arrow" x1="190" y1="166" x2="152" y2="228"/>
<line class="d-line" x1="20" y1="92" x2="20" y2="318"/>
<line class="d-arrow" x1="20" y1="92" x2="38" y2="92"/>
<line class="d-arrow" x1="20" y1="138" x2="38" y2="138"/>
<line class="d-arrow" x1="20" y1="184" x2="38" y2="184"/>
<line class="d-arrow" x1="20" y1="230" x2="38" y2="230"/>
<line class="d-arrow" x1="20" y1="276" x2="38" y2="276"/>
<text class="d-label" x="8" y="336">With every picture: an HDR10+ message</text>
<text class="d-label" x="8" y="352">and a Dolby Vision RPU</text>
</svg>
<figcaption>Static HDR10 metadata repeats at key frames so a player can join mid-stream. Dynamic metadata travels with each picture.</figcaption>
</figure>

## Dynamic metadata comes from a file

HDR10+ and Dolby Vision describe the content scene by scene. yah265 doesn't measure that. The metadata comes from the grade, in the files the two common tools write:

- `--dhdr10-info` reads the JSON that [hdr10plus_tool](https://github.com/quietvoid/hdr10plus_tool) writes. `hdr10plus_tool extract master.hevc -o meta.json` makes one. Profile A and profile B with a Bézier tone curve both work. Each takes one window per picture.
- `--dolby-vision-rpu` reads the RPU file that [dovi_tool](https://github.com/quietvoid/dovi_tool) writes. That's `dovi_tool extract-rpu` from an existing Dolby Vision stream, or `dovi_tool generate` from a colorist's Dolby Vision XML.

Entry *i* of either file goes with frame *i* of the input, whatever order the encoder codes the pictures in. `--seek` skips entries along with frames, and `--gop-parallel` gives each segment its own part of the file. If a file runs out before the input does, the encode still writes the stream and then exits with an error, because pictures without metadata break HDR10+ or Dolby Vision playback from that point. A metadata NAL lost to a failed allocation fails the run the same way. A file with entries left over gets a warning. The usual reason is a `--frames` run over part of a title.

Both readers check what they read. HDR10+ values must fall inside the HDR10+ profiles' ranges, and HDR10+ needs a 10-bit HDR10 encode under it. Every RPU's header must say profile 8, and `--dolby-vision-profile` must match the color flags and a 10-bit encode. A command line that would mislabel the stream is refused before it writes anything. Either file can come from a pipe.

Two cases get a warning and still encode. Dolby Vision without a VBV is one, because deliveries are normally buffer-constrained while a CRF encode is still valid. An 8.1 encode without `--master-display` is the other, because an 8.1 base layer is a complete HDR10 stream.

## Encoding HDR10+ and Dolby Vision 8.1

<!-- recipe: skip needs an HDR10+ JSON and a Dolby Vision RPU file -->
```sh
ffmpeg -i master.mov -pix_fmt yuv420p10le -strict -1 -f yuv4mpegpipe - |
  y265 - -o out.hevc --crf 22 \
    --colorprim bt2020 --transfer smpte2084 --colormatrix bt2020nc \
    --master-display 'G(13250,34500)B(7500,3000)R(34000,16000)WP(15635,16450)L(10000000,1)' \
    --max-cll 1000,400 \
    --dhdr10-info meta.json \
    --dolby-vision-rpu RPU.bin --dolby-vision-profile 8.1
```

A raw HEVC stream has nowhere to say "this is Dolby Vision" except the RPUs themselves. A player needs the container to tell it, in MP4's `dvcC` box or Matroska's equivalent. ffmpeg's stream copy from a raw `.hevc` doesn't write that box. A Dolby Vision-aware muxer such as MP4Box or mkvmerge does.

## Tuning for HDR

A stream says PQ with `--transfer smpte2084`. It says HLG with `--transfer arib-std-b67` or `--atc-sei arib-std-b67`. On either one, `medium` codes chroma six quantizer steps finer than luma and turns off the extra psy weight it gives dark blocks on SDR. The reason is that wide-gamut chroma stays in a narrow band around its midpoint, so at luma's quantizer it loses far more than SDR chroma does.

From 720p up, `medium` on HDR also keeps the full intra mode search that SDR gives up for speed. At 4K it keeps the intra transform split too.

`--hdr-opt` turns the tuning on for a stream that names neither transfer, and `--no-hdr-opt` turns it off. `--hdr10-opt` is accepted as another name for yah265's switch. The tuning behind it is yah265's own. A `--cbqpoffs` or `--crqpoffs` you give yourself still wins. The other presets code HDR like SDR.

## Quality on HDR content

<figure class="chart"><img src="assets/charts/front/hdr-bd.svg" alt="Bars of the share of bitrate yah265 saves against x265 at equal HDR-VMAF on Sol Levante, Sparks and an HLG clip at 1080p and 4K. Every bar is a saving. The savings are smaller against x265 with its PQ tuning on, and smallest on Sol Levante."></figure>

We scored both encoders with Netflix's HDR-VMAF v1.1.8. The table gives yah265's BD-rate against x265, where a negative number is the share of bitrate yah265 saves at equal quality.

<!--gen:hdr-table-->
| title | transfer | size | against x265 | against x265 `--hdr10-opt` |
|---|---|---|--:|--:|
| Sol Levante | PQ | 1080p | -23.0% | -5.5% |
| Sol Levante | PQ | 4K | -17.7% | -5.4% |
| Sparks | PQ | 1080p | -23.2% | -18.3% |
| Sparks | PQ | 4K | -24.2% | -18.4% |
| EBU / DVB HLG | HLG | 1080p | -38.8% | no HLG reading |
| EBU / DVB HLG | HLG | 4K | -34.6% | no HLG reading |
<!--/gen:hdr-table-->

yah265 is ahead on every cell. The margin is thinnest on Sol Levante against x265 with its `--hdr10-opt` option on. That option has no HLG reading.

I wouldn't read the HLG clip's lead as a lead on all HLG, because the clip is easy material. It's two near-static shots from a source that is itself an HEVC encode. The downscale averages away most of the noise that was left. The next table shows what the tuning alone is worth on it.

<!--gen:hdr-tuning-table-->
| HLG clip | tuned, against x265 | untuned, against x265 | tuned against untuned |
|---|--:|--:|--:|
| 1080p | -38.8% | -29.1% | -14.1% |
| 4K | -34.6% | -23.4% | -15.0% |
<!--/gen:hdr-tuning-table-->

### How the HDR readings were run

The PQ titles are from Netflix Open Content: Sol Levante is 24p animation and Sparks is 60p live action. The HLG clip is the EBU / DVB 8K HLG test clip, published by DVB under CC BY 4.0 (credit EBU / DVB). We took frames 600 to 899 of its 37 Mbit/s rendition, around the clip's one cut, and scaled them down with HLG and BT.2020 kept.

Both encoders ran `medium` with the same signaling. yah265 ran at `--threads 4`. x265 ran at `--pools 6 --frame-threads 4` on the PQ titles and `--pools 4 --frame-threads 4` on the HLG clip. Scores use HDR-VMAF's 3H model at 1080p and its 1.5H model at 4K, in the high frame rate variant at 50 fps and above. Because HDR-VMAF has no HLG transfer, the HLG reference and every decoded picture go through the same HLG to PQ conversion at a 1000-nit peak before scoring.

Each encoder ran its own CRF ladder, because the two scales don't line up on this material:

<!--gen:hdr-ladder-table-->
| title | excerpt | yah265 `--crf` | x265 `--crf` |
|---|---|---|---|
| Sol Levante | 10 seconds, 24 fps | 11 to 26 in steps of 3 | 20 to 32 in steps of 3 |
| Sparks | 5 seconds, 60 fps | 17 to 32 in steps of 3 | 20 to 32 in steps of 3 |
| EBU / DVB HLG | 6 seconds, 50 fps | 20 to 35 in steps of 3 | 20 to 32 in steps of 3 |
<!--/gen:hdr-ladder-table-->

yah265's six rungs cover 95% or more of x265's HDR-VMAF range in every cell. The [HDR ladder script](scripts/hdr_bd.py) runs the ladders and the BD reading.

### Banding

Banding was read with HDR-VMAF's PQ CAMBI. Sol Levante's flat gradients band on both encoders at the lower rates. There yah265's worst frames read lower than x265's at the same bitrate. yah265's mean reads slightly higher on all three titles, because `medium` turns off the dark psy weight on HDR. Both encoders stay far below the level where banding shows. The [HDR notes](docs/hdr.md) have the CAMBI figures.

## How it was checked

- A CRF encode with `--dhdr10-info` and `--dolby-vision-rpu` gives the same NAL units, byte for byte, as the same encode without them run through `hdr10plus_tool inject` and `dovi_tool inject-rpu`. Only two things differ. The tools add an access unit delimiter to each picture. The encoder's version SEI names the extra options.
- `dovi_tool extract-rpu` and `hdr10plus_tool extract` on an encode give back the input metadata exactly. That holds with B-frames reordering the pictures, with `--seek`, and across `--gop-parallel` segments.
- The ST 2094-40 writer reproduces a profile B message from a real HDR10+ stream byte for byte. A unit test holds it to that.
- ffmpeg parses the HDR10+ message and the Dolby Vision RPU of every frame. It decodes the streams with errors fatal.
- The Dolby Vision test uses Netflix's own grade for Sol Levante. That's the Dolby Vision XML published with it under CC BY 4.0, turned into profile 8.1 RPUs by `dovi_tool`.
- Without these options the output is byte-identical to the encoder before they existed. The one exception is `--temporal-layers`, where the NAL units written with each picture now take that picture's temporal layer.
- With the options, a CRF encode codes the same pictures. Under a bitrate or VBV target the metadata's bytes count toward it like any other part of the access unit. The rate control therefore spends slightly less on the pictures.
