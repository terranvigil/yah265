---
title: yah265
description: An H.265/HEVC encoder written from scratch, faster than x265 at medium with better quality at the same file size.
---

# yah265

<p class="standfirst">yah265 is an H.265/HEVC encoder written from scratch. It's faster than x265 at <code>--preset medium</code> on every class of test video from CIF to 4K. At the same file size its pictures score higher on VMAF v1.</p>

yah265 is the sibling of [yah264](https://terranvigil.github.io/yah264/), which reached the same goals against x264. The rules carry over. The encoder is clean-room from the ITU-T H.265 specification. Every stream is checked against two independent decoders. Every number on this site comes from a script in the repository.

<div class="nums">
<div><span>pure C on one thread</span><b>1.96x to 3.45x</b></div>
<div><span>pure C on all cores</span><b>2.27x to 3.23x</b></div>
<div><span>SIMD on all cores</span><b>1.37x to 1.89x</b></div>
<div><span>median bits saved</span><b>12.4%</b></div>
</div>

The speed figures say how many times faster yah265 runs than x265 when both write files of the same size. Each range runs from the slowest group's median to the fastest's. The last figure is the share of bitrate yah265 saves at equal VMAF v1 quality. It's a median over 52 rate ladders on real films at CRF and at VBR. Every one of those ladders comes out ahead of x265.

## What it does

- Main, Main10, Main 4:4:4, Main 4:4:4 10 and Main 4:2:2 10 from one binary. The input picks the profile.
- CRF, fixed QP, ABR, VBR and CBR with a VBV, capped CRF, and two-pass.
- HDR10, HDR10+, HLG and Dolby Vision profile 8 signalling. The [HDR page](hdr.md) covers it.
- Four presets in x265's names: `fast`, `medium`, `slow` and `slower`.
- x265's option names wherever the two encoders share a feature.
- NEON kernels on arm64. Each one is bit-exact with its C reference.

## Where to go next

[Getting started](start.md) builds the encoder and walks through the command line. [Design](design.md) explains how the encoder is put together. [Results](results.md) has the full parity tables and how to read them, and [check it yourself](check-it-yourself.md) shows how to rerun a reading on your own machine.

## Licence

yah265 is GPL-2.0-or-later. A commercial licence is also available. x264 and x265 use the same arrangement. Write to terranvigil@gmail.com for commercial terms.
