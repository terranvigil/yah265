---
title: Coming from x265 - yah265
description: How an x265 command line maps onto y265. It lists the options that keep their name, the ones spelled differently and the ones y265 refuses with the reason for each. A chart translates a CRF.
---

# Coming from x265

This page is for someone moving an x265 command line to `y265`. Most options keep their name and meaning, though a few are spelled differently. Some aren't accepted. `y265` stops with an error on an option it doesn't know.

## Options that keep their name

<!--map:accepted-->
| area | options |
|---|---|
| rate | `--crf`, `--qp`, `--bitrate`, `--vbv-maxrate`, `--vbv-bufsize`, `--vbv-init`, `--crf-min`, `--crf-max`, `--qpmin`, `--qpmax`, `--strict-cbr`, `--pass`, `--stats`, `--zones`, `--qpfile`, `--max-ausize-factor` |
| preset and tune | `--preset` with `fast`, `medium`, `slow` or `slower`, and `--tune zerolatency` |
| GOP | `--keyint`, `--open-gop`, `--radl`, `--bframes`, `--ref`, `--rc-lookahead`, `--weightp`, `--weightb`, `--temporal-layers`, `--intra-refresh` |
| coding | `--ctu`, `--tu-intra-depth`, `--tu-inter-depth`, `--rdoq-level`, `--cbqpoffs`, `--crqpoffs`, `--lossless` |
| input | `--input-res`, `--input-csp`, `--input-depth`, `--output-depth`, `--fps`, `--frames`, `--seek`, `--dither` |
| stream | `--repeat-headers`, `--aud`, `--eos`, `--eob`, `--hash`, `--high-tier`, `--info`, `--idr-recovery-sei`, `--pic-struct`, `--hrd-concat` |
| filters | `--nr-intra`, `--nr-inter`, `--mcstf` |
| color and HDR | `--colorprim`, `--transfer`, `--colormatrix`, `--range`, `--videoformat`, `--chromaloc`, `--overscan`, `--sar`, `--display-window`, `--video-signal-type-preset`, `--master-display`, `--max-cll`, `--hdr10`, `--atc-sei`, `--dolby-vision-rpu`, `--dolby-vision-profile` |
| reports | `--ssim`, `--csv`, `--csv-log-level` |
<!--/map:accepted-->

The same name doesn't promise the same default. [Options](options.md) lists the defaults. `--show-config` prints what a command resolves to without encoding.

## Options with a different spelling

<!--map:renamed-->
| x265 | `y265` | note |
|---|---|---|
| `--input`, `--y4m` | the first bare argument | a file name, or `-` for a pipe. A Y4M header is recognized by itself |
| `--output` | `-o` | |
| `--pools`, `--frame-threads` | `--threads` | one count. The default, 0, is one worker per CPU |
| `--aq-mode` | `--aq` | 1 or 0. The preset sets the strength |
| `--cutree`, `--no-cutree` | `--cutree` | takes 1 or 0 |
| `--level-idc` | `--level` | written `4.1` or `41` |
| `--vui-timing-info` | `--vui-timing` | on by default |
| `--recon` | `--dump-recon` | raw YUV |
| `--log-level` | `--verbose` | prints the resolved configuration |
| `--hdr10-opt` | `--hdr-opt` | either name turns on yah265's own HDR tuning, which works differently |
| `--b-adapt` | `--b-adapt` | 0 and 1 mean the same. 2 to 4 are experimental |
| `--dup-threshold` | `--dup-threshold` | the unit is different. Start from 0, the default |
| `--film-grain` | `--film-grain` | x265 takes a file of grain parameters. `y265` estimates the grain itself and takes a strength |
| `--profile` | none | the input and `--output-depth` pick the profile |
| `--scenecut <n>` | `--scenecut` | a switch with no threshold. `--no-scenecut` keeps x265's meaning and puts key frames on the `--keyint` grid. See [Key frames for streaming](guide.md#key-frames-for-streaming) |
<!--/map:renamed-->

## Options y265 doesn't accept

<!--map:refused-->
| x265 | why | use instead |
|---|---|---|
| `--preset` with `ultrafast`, `superfast`, `veryfast`, `faster`, `veryslow` or `placebo` | [in the guide](guide.md#left-out-on-purpose) | the error names the nearest of the four presets |
| `--tune psnr`, `--tune ssim` | [in the guide](guide.md#tune-names) | `--ssim` reports the number without changing the encode |
| `--tune grain` | [in the guide](guide.md#tune-names) | `--film-grain` is a different tool that the [guide](guide.md#grain-and-noisy-sources) describes |
| `--tune fastdecode` | [in the guide](guide.md#tune-names) | `--no-weightp` and `--no-weightb` |
| `--tune animation` | `medium` is tuned on a test set that includes animated films | `medium` as it is |
| `--psy-rd`, `--psy-rdoq` | [in the guide](guide.md#left-out-on-purpose) | the preset sets it |
| `--aq-strength` | the preset sets the strength frame by frame | `--aq 0` turns AQ off |
| `--rd`, `--me`, `--subme`, `--merange`, `--rect`, `--amp`, `--limit-refs`, `--rskip`, `--early-skip`, `--max-merge` | how hard the encoder searches is the preset's job here | a preset. The [environment knobs](guide.md#environment-knobs) are there for experiments |
| `--signhide`, `--tskip`, `--strong-intra-smoothing`, `--temporal-mvp` | single coding tools have no switch | a preset |
| `--sao`, `--no-sao`, `--deblock`, `--no-deblock` | the in-loop filters have no switch | none |
| `--min-keyint` | the encoder sets it from `--keyint` and the frame rate | `--keyint` |
| `--b-pyramid` | B-frames are always arranged as a pyramid | `--bframes` |
| `--wpp`, `--slices`, `--lookahead-slices`, `--lookahead-threads` | [in the guide](guide.md#left-out-on-purpose) | `--threads` |
| `--asm`, `--no-asm` | the SIMD tier is picked at run time | `N265_ASM_OFF=all` in the environment, for testing |
| `--ipratio`, `--pbratio`, `--qcomp` | rate control isn't steered through these ratios | the guide's [rate modes](guide.md#pick-a-rate-mode) |
| `--scaling-list` | [in the guide](guide.md#left-out-on-purpose) | none |
| `--interlace`, `--field` | [in the guide](guide.md#left-out-on-purpose) | none |
| `--analysis-save`, `--analysis-load` | [in the guide](guide.md#left-out-on-purpose) | none |
<!--/map:refused-->

## Translating a CRF

The CRF scale is yah265's own. For the same file size, yah265's number is usually about two below x265's.

<!--trust:fig-crf-->
<figure class="chart"><img src="assets/charts/trust/crf-map.svg" alt="Line chart of the yah265 CRF that writes the same file size as each x265 CRF from 23 to 32. The median line runs about two below the same-number diagonal. A few animated films are on or above it."></figure>
<!--/trust:fig-crf-->

<!--trust:crf-table-->
| x265 `--crf` | `y265 --crf`, median | range over the cells | cells |
|--:|--:|--:|--:|
| 23 | 21.2 | 20.0 to 25.0 | 16 |
| 26 | 23.8 | 21.4 to 28.0 | 19 |
| 28 | 26.0 | 23.5 to 29.9 | 30 |
| 29 | 26.8 | 24.8 to 30.9 | 19 |
| 32 | 29.4 | 27.9 to 32.0 | 16 |
<!--/trust:crf-table-->

Because the range is wide, treat the table as a starting point and check the size on your own content.
