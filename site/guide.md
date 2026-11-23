---
title: Encoding guide - yah265
description: How to pick a rate mode, a preset and a thread count in yah265. It has a recipe for each rate mode and a list of what ships.
---

# Encoding guide

This page is for someone putting yah265 into a pipeline. Commands use `y265`, the project's own command line. The same options work from ffmpeg, as [From ffmpeg](#from-ffmpeg) shows.

## Pick a rate mode

A rate mode decides what stays fixed: the quality, the bitrate or the size.

| mode | options | use it when |
|---|---|---|
| CRF | `--crf` | you want a quality level, whatever size the content needs. It's the right choice for most files |
| capped CRF | `--crf` with `--vbv-maxrate` and `--vbv-bufsize` | you want CRF under a ceiling that a player or a network sets on the peak rate |
| target quality | `--target-vmaf` in place of `--crf` | you know the VMAF score you want and want the encoder to find the CRF |
| capped VBR | `--bitrate` with `--vbv-maxrate` and `--vbv-bufsize` | you have a target bitrate and a peak, as on a streaming ladder |
| CBR | capped VBR with `--strict-cbr` | the channel runs at a constant rate, as in broadcast |
| ABR | `--bitrate` alone | you want a mean bitrate where nothing reads the stream through a buffer |
| two-pass | `--pass` with `--stats` | the file has to hit a size. It costs a second encode |
| fixed QP | `--qp` | you're testing the encoder. It's also what you get with no rate option |

<figure class="diagram">
<svg class="d-wide" viewBox="0 0 720 330" role="img" aria-labelledby="rate-fig-t rate-fig-d">
<title id="rate-fig-t">Choosing a rate mode</title>
<desc id="rate-fig-d">A decision tree. If you don't need a set bitrate, use CRF, or capped CRF when there is a peak rate limit. If you need a set bitrate and the channel is constant rate, use CBR. Otherwise use two-pass when you can encode twice, capped VBR when a decoder buffer applies, and ABR when none does.</desc>
<rect class="d-fill" x="250" y="10" width="220" height="44" rx="6"/>
<text class="d-text" x="360" y="38" text-anchor="middle">Need a set bitrate?</text>
<path class="d-arrow" d="M250 32H150V96"/>
<text class="d-label" x="200" y="24" text-anchor="middle">no</text>
<path class="d-arrow" d="M470 32H540V96"/>
<text class="d-label" x="505" y="24" text-anchor="middle">yes</text>
<rect class="d-fill" x="50" y="98" width="200" height="44" rx="6"/>
<text class="d-text" x="150" y="126" text-anchor="middle">A peak rate limit?</text>
<line class="d-arrow" x1="100" y1="142" x2="100" y2="196"/>
<text class="d-label" x="88" y="174" text-anchor="end">no</text>
<line class="d-arrow" x1="210" y1="142" x2="210" y2="196"/>
<text class="d-label" x="222" y="174">yes</text>
<rect class="d-accent" x="30" y="198" width="110" height="44" rx="6"/>
<text class="d-text" x="85" y="226" text-anchor="middle">CRF</text>
<rect class="d-accent" x="160" y="198" width="130" height="44" rx="6"/>
<text class="d-text" x="225" y="226" text-anchor="middle">capped CRF</text>
<rect class="d-fill" x="430" y="98" width="220" height="44" rx="6"/>
<text class="d-text" x="540" y="126" text-anchor="middle">Constant-rate channel?</text>
<path class="d-arrow" d="M650 120H690V196"/>
<text class="d-label" x="682" y="174" text-anchor="end">yes</text>
<rect class="d-accent" x="640" y="198" width="70" height="44" rx="6"/>
<text class="d-text" x="675" y="226" text-anchor="middle">CBR</text>
<line class="d-arrow" x1="500" y1="142" x2="500" y2="196"/>
<text class="d-label" x="488" y="174" text-anchor="end">no</text>
<rect class="d-fill" x="390" y="198" width="220" height="44" rx="6"/>
<text class="d-text" x="500" y="226" text-anchor="middle">Time to encode twice?</text>
<line class="d-arrow" x1="420" y1="242" x2="360" y2="274"/>
<text class="d-label" x="372" y="254" text-anchor="end">yes</text>
<rect class="d-accent" x="290" y="276" width="110" height="44" rx="6"/>
<text class="d-text" x="345" y="304" text-anchor="middle">two-pass</text>
<line class="d-arrow" x1="500" y1="242" x2="500" y2="274"/>
<text class="d-label" x="508" y="262">a buffer</text>
<rect class="d-accent" x="430" y="276" width="140" height="44" rx="6"/>
<text class="d-text" x="500" y="304" text-anchor="middle">capped VBR</text>
<line class="d-arrow" x1="590" y1="242" x2="640" y2="274"/>
<text class="d-label" x="626" y="254">no buffer</text>
<rect class="d-accent" x="600" y="276" width="90" height="44" rx="6"/>
<text class="d-text" x="645" y="304" text-anchor="middle">ABR</text>
</svg>
<svg class="d-narrow" viewBox="0 0 360 480" role="img" aria-labelledby="rate-fig-t-n rate-fig-d-n">
<title id="rate-fig-t-n">Choosing a rate mode</title>
<desc id="rate-fig-d-n">A decision tree. If you don't need a set bitrate, use CRF, or capped CRF when there is a peak rate limit. If you need a set bitrate and the channel is constant rate, use CBR. Otherwise use two-pass when you can encode twice, capped VBR when a decoder buffer applies, and ABR when none does.</desc>
<rect class="d-fill" x="60" y="8" width="240" height="42" rx="6"/>
<text class="d-text" x="180" y="35" text-anchor="middle">Need a set bitrate?</text>
<line class="d-arrow" x1="120" y1="50" x2="120" y2="86"/>
<text class="d-label" x="112" y="72" text-anchor="end">no</text>
<path class="d-arrow" d="M300 29H340V250"/>
<text class="d-label" x="320" y="22" text-anchor="middle">yes</text>
<rect class="d-fill" x="40" y="88" width="180" height="42" rx="6"/>
<text class="d-text" x="130" y="115" text-anchor="middle">A peak rate limit?</text>
<line class="d-arrow" x1="80" y1="130" x2="80" y2="166"/>
<text class="d-label" x="72" y="152" text-anchor="end">no</text>
<line class="d-arrow" x1="180" y1="130" x2="180" y2="166"/>
<text class="d-label" x="188" y="152">yes</text>
<rect class="d-accent" x="20" y="168" width="100" height="40" rx="6"/>
<text class="d-text" x="70" y="194" text-anchor="middle">CRF</text>
<rect class="d-accent" x="135" y="168" width="120" height="40" rx="6"/>
<text class="d-text" x="195" y="194" text-anchor="middle">capped CRF</text>
<rect class="d-fill" x="150" y="252" width="202" height="42" rx="6"/>
<text class="d-text" x="251" y="279" text-anchor="middle">Constant-rate channel?</text>
<line class="d-arrow" x1="150" y1="273" x2="112" y2="273"/>
<text class="d-label" x="131" y="266" text-anchor="middle">yes</text>
<rect class="d-accent" x="30" y="253" width="80" height="40" rx="6"/>
<text class="d-text" x="70" y="279" text-anchor="middle">CBR</text>
<line class="d-arrow" x1="251" y1="294" x2="251" y2="330"/>
<text class="d-label" x="259" y="316">no</text>
<rect class="d-fill" x="80" y="332" width="200" height="42" rx="6"/>
<text class="d-text" x="180" y="359" text-anchor="middle">Time to encode twice?</text>
<line class="d-arrow" x1="110" y1="374" x2="62" y2="428"/>
<text class="d-label" x="78" y="398" text-anchor="end">yes</text>
<line class="d-arrow" x1="180" y1="374" x2="180" y2="428"/>
<text class="d-label" x="188" y="406">a buffer</text>
<line class="d-arrow" x1="250" y1="374" x2="300" y2="428"/>
<text class="d-label" x="286" y="396">no buffer</text>
<rect class="d-accent" x="8" y="430" width="100" height="42" rx="6"/>
<text class="d-text" x="58" y="457" text-anchor="middle">two-pass</text>
<rect class="d-accent" x="120" y="430" width="120" height="42" rx="6"/>
<text class="d-text" x="180" y="457" text-anchor="middle">capped VBR</text>
<rect class="d-accent" x="262" y="430" width="90" height="42" rx="6"/>
<text class="d-text" x="307" y="457" text-anchor="middle">ABR</text>
</svg>
<figcaption>Start from whether the bitrate is set for you.</figcaption>
</figure>

Bitrates are in kbit/s, buffer sizes in kbit. When you give a buffer, the stream signals it so a packager can check it.

### CRF

```sh
y265 in.y4m -o out.hevc --crf 23
```

The CRF scale is yah265's own. A number that suited x265 gives a different size here. Because the scale accounts for the frame rate, one number suits a 24 fps film as well as a 60 fps game capture. [Results](results.md#quality-at-the-same-size) compares the two encoders at the same file size.

### Capped CRF

```sh
y265 in.y4m -o out.hevc --crf 23 --vbv-maxrate 4500 --vbv-bufsize 6000
```

For streaming, this is the mode I'd pick. The quality holds until a hard scene would overflow the buffer. The encoder then coarsens that scene to fit. The cap limits the peak rate, not the file size. If `--crf-max` is set too low for the buffer, the buffer can underflow. The encoder reports that at the end of the run. To pick the CRF from a VMAF score, see [Target quality](#target-quality).

### Target quality

Replace `--crf N` with `--target-vmaf S`, where S is the VMAF score you want. Every other option stays the same, a VBV cap and the HDR flags included.

```sh
y265 in.y4m -o out.hevc --target-vmaf 93
```

The encoder tries a few CRFs, scores each one and encodes the file at the CRF that comes closest. It prints the CRF it chose, the measured score, the size and how long the search took. The file is the one `--crf` with that number writes. On my test clips the final score came within 0.4 of the target. Because the search costs a few extra encodes, a target run takes several times as long as a CRF run.

Under a VBV cap the search runs under the same cap. If the cap can't reach the score, the report says so and keeps the best CRF it found. On a PQ or HLG stream it scores HDR-VMAF instead of VMAF v1.

A clip longer than 60 seconds is searched on 12 short windows spread through it, which add up to 60 seconds. The final file is then scored whole, and the report shows both the predicted and the measured score.

VMAF isn't built into y265. During the search y265 runs ffmpeg and libvmaf's `vmaf` tool as separate programs and streams the frames to them, so they have to be installed. It finds them like this:

| what | set with | otherwise |
|---|---|---|
| the `vmaf` tool | `N265_VMAF_BIN` | `~/.local/opt/vmaf-v1/bin/vmaf`, then `vmaf` on your PATH |
| the VMAF v1 model files | `N265_VMAF_MODEL_DIR` | `~/.local/share/vmaf/model` |
| ffmpeg | `N265_FFMPEG` | `ffmpeg` on your PATH |

The model files are the `vmaf_v1.0.16` and `vmaf_v1.0.16_hfr` folders from Netflix's [vmaf repository](https://github.com/Netflix/vmaf). HDR-VMAF has its own pair, `N265_VMAF_HDR_BIN` and `N265_VMAF_HDR_MODEL_DIR`.

The input has to be a file, because the search reads it more than once. It can't be combined with `--qp`, `--bitrate` or two-pass. With ffmpeg, run the search with y265 and pass the CRF it prints to `-crf`.

### Capped VBR

```sh
y265 in.y4m -o out.hevc --bitrate 3000 --vbv-maxrate 4500 --vbv-bufsize 6000
```

Set the peak above the target so the encoder has room to spend on hard scenes. [Results](results.md#vbr) shows how close each title came to its target and how fast it ran.


### CBR

```sh
y265 in.y4m -o out.hevc --bitrate 3000 --vbv-maxrate 3000 --vbv-bufsize 6000 --strict-cbr
```

`--strict-cbr` marks the stream as constant rate. Any picture that comes in under its share is padded with filler data. This is generally a wasteful mode and generally only useful when targeting older hardware decoders designed for cable/MPEG-TS.

### Two-pass

```sh
y265 in.y4m -o /dev/null --pass 1 --stats in.stats --crf 23
y265 in.y4m -o out.hevc --pass 2 --stats in.stats --bitrate 3000
```

The first pass measures each frame. The second plans the whole file before it codes one. Pass 2 needs `--bitrate` and the same source. Pass 1 can run any mode at any preset. Two-pass doesn't combine with `--gop-parallel`.

<p class="optchips"><span><a href="options.md#rate-control">Options</a></span> <a href="options.md#opt-crf"><code>--crf</code></a> <a href="options.md#opt-bitrate"><code>--bitrate</code></a> <a href="options.md#opt-vbv-maxrate"><code>--vbv-maxrate</code></a> <a href="options.md#opt-vbv-bufsize"><code>--vbv-bufsize</code></a> <a href="options.md#opt-vbv-init"><code>--vbv-init</code></a> <a href="options.md#opt-max-ausize-factor"><code>--max-ausize-factor</code></a> <a href="options.md#opt-strict-cbr"><code>--strict-cbr</code></a> <a href="options.md#opt-crf-max"><code>--crf-max</code></a> <a href="options.md#opt-pass"><code>--pass</code></a> <a href="options.md#opt-stats"><code>--stats</code></a> <a href="options.md#opt-qp"><code>--qp</code></a></p>

## Pick a preset

`--preset` takes the preset names familiar from x264 and x265.

| preset | coding tree at 1080p | lookahead at 1080p | what it's for |
|---|---|---|---|
| `fast` | 32x32 | 12 frames | more speed for a little quality |
| `medium` | 64x64 | 20 frames | the default. The published results use it |
| `slow` | 64x64 | 20 frames | more analysis per block |
| `slower` | 64x64 | 25 frames | more analysis and a second reference |

`medium` changes with the picture size. A small picture gets a smaller coding tree and a shorter lookahead. 4K gets a second reference. [Design](design.md#what-medium-does-at-each-size) has the whole table.

yah265 offers four presets. I kept the list short because its `medium` already runs faster than x265's `medium` and scores higher at the same file size. x265's other preset names give an error that names the nearest preset. I'd use `medium` unless you've measured otherwise on your content.

Because a preset only sets defaults, any option you give wins over it. To see what a run will do, ask:

```sh
y265 in.y4m --preset medium --crf 23 --show-config
```

The command prints the resolved configuration and exits without encoding.

<p class="optchips"><span><a href="options.md#presets-and-tunes">Options</a></span> <a href="options.md#opt-preset"><code>--preset</code></a> <a href="options.md#opt-show-config"><code>--show-config</code></a> <a href="options.md#opt-verbose"><code>--verbose</code></a></p>

## Streaming and seeking

For the rate, a streaming encode usually wants [capped CRF](#capped-crf) or [capped VBR](#capped-vbr). What's left to decide is where the key frames fall, because a player seeks to a key frame and a packager cuts its segments at one.

A key frame arrives every 250 frames by default, and at each scene cut. Up to seven B-frames run between P-frames, fewer where motion is high.

By default key frames use an open GOP, which saves bits. If your packager needs closed segments, pass `--no-open-gop`.

<figure class="diagram">
<svg class="d-wide" viewBox="0 0 720 256" role="img" aria-labelledby="gop-t gop-d"><title id="gop-t">What each GOP option controls</title><desc id="gop-d">A row of frames in display order. An IDR key frame starts it, then runs of three B-frames between P anchors. Three dashed B-frames, the leading pictures, sit just before a CRA key frame. Labels name the option behind each part: --bframes over a B run, --rc-lookahead over the newest frames, --ref on an arrow from a P frame back to the key frame, --no-open-gop and --radl at the CRA, and --keyint under the span from one key frame to the next.</desc><defs><marker id="gop-ah" viewBox="0 0 10 10" refX="9" refY="5" markerWidth="7" markerHeight="7" orient="auto-start-reverse"><path class="d-head" d="M0 0 L10 5 L0 10 z"/></marker></defs><rect class="d-accent" x="20" y="100" width="32" height="40" rx="4"/><text class="d-label" x="36.0" y="124.5" text-anchor="middle">IDR</text><rect class="d-box" x="56" y="100" width="32" height="40" rx="4"/><text class="d-label" x="72.0" y="124.5" text-anchor="middle">B</text><rect class="d-box" x="92" y="100" width="32" height="40" rx="4"/><text class="d-label" x="108.0" y="124.5" text-anchor="middle">B</text><rect class="d-box" x="128" y="100" width="32" height="40" rx="4"/><text class="d-label" x="144.0" y="124.5" text-anchor="middle">B</text><rect class="d-fill" x="164" y="100" width="32" height="40" rx="4"/><text class="d-label" x="180.0" y="124.5" text-anchor="middle">P</text><rect class="d-box" x="200" y="100" width="32" height="40" rx="4"/><text class="d-label" x="216.0" y="124.5" text-anchor="middle">B</text><rect class="d-box" x="236" y="100" width="32" height="40" rx="4"/><text class="d-label" x="252.0" y="124.5" text-anchor="middle">B</text><rect class="d-box" x="272" y="100" width="32" height="40" rx="4"/><text class="d-label" x="288.0" y="124.5" text-anchor="middle">B</text><rect class="d-fill" x="308" y="100" width="32" height="40" rx="4"/><text class="d-label" x="324.0" y="124.5" text-anchor="middle">P</text><text class="d-text" x="362" y="125" text-anchor="middle">…</text><rect class="d-muted" x="390" y="100" width="32" height="40" rx="4" style="stroke-dasharray:4 3"/><text class="d-label" x="406.0" y="124.5" text-anchor="middle">B</text><rect class="d-muted" x="426" y="100" width="32" height="40" rx="4" style="stroke-dasharray:4 3"/><text class="d-label" x="442.0" y="124.5" text-anchor="middle">B</text><rect class="d-muted" x="462" y="100" width="32" height="40" rx="4" style="stroke-dasharray:4 3"/><text class="d-label" x="478.0" y="124.5" text-anchor="middle">B</text><rect class="d-accent" x="498" y="100" width="32" height="40" rx="4"/><text class="d-label" x="514.0" y="124.5" text-anchor="middle">CRA</text><rect class="d-box" x="534" y="100" width="32" height="40" rx="4"/><text class="d-label" x="550.0" y="124.5" text-anchor="middle">B</text><rect class="d-box" x="570" y="100" width="32" height="40" rx="4"/><text class="d-label" x="586.0" y="124.5" text-anchor="middle">B</text><rect class="d-box" x="606" y="100" width="32" height="40" rx="4"/><text class="d-label" x="622.0" y="124.5" text-anchor="middle">B</text><rect class="d-fill" x="642" y="100" width="32" height="40" rx="4"/><text class="d-label" x="658.0" y="124.5" text-anchor="middle">P</text><path class="d-line" d="M56 92 V86 H160 V92"/><text x="56" y="76" text-anchor="start"><tspan class="d-mono">--bframes</tspan><tspan class="d-label"> B-frames between anchors</tspan></text><path class="d-line" d="M390 92 V86 H494 V92"/><text class="d-label" x="390" y="76">leading pictures</text><path class="d-line" d="M534 54 V48 H674 V54"/><text x="674" y="36" text-anchor="end"><tspan class="d-mono">--rc-lookahead</tspan><tspan class="d-label"> frames read ahead, also the latency</tspan></text><path class="d-arrow" d="M180 141 Q108 182 36 141" marker-end="url(#gop-ah)"/><text x="108" y="192" text-anchor="middle"><tspan class="d-mono">--ref</tspan><tspan class="d-label"> references per list</tspan></text><path class="d-line" d="M514 141 V156 H400"/><text x="400" y="172" text-anchor="start"><tspan class="d-mono">--no-open-gop</tspan><tspan class="d-label"> an IDR instead, a closed segment</tspan></text><text x="400" y="192" text-anchor="start"><tspan class="d-mono">--radl</tspan><tspan class="d-label"> some leading pictures stay decodable</tspan></text><path class="d-line" d="M36 216 V222 H514 V216"/><text x="275" y="242" text-anchor="middle"><tspan class="d-mono">--keyint</tspan><tspan class="d-label"> key frame to key frame, 250 by default; a scene cut starts a new one</tspan></text></svg>
<svg class="d-narrow" viewBox="0 0 360 246" role="img" aria-labelledby="gopn-t gopn-d"><title id="gopn-t">What each GOP option controls</title><desc id="gopn-d">A row of frames in display order. An IDR key frame starts it, then runs of three B-frames between P anchors. Three dashed B-frames, the leading pictures, sit just before a CRA key frame. Labels name the option behind each part: --bframes over a B run, --rc-lookahead over the newest frames, --ref on an arrow from a P frame back to the key frame, --no-open-gop and --radl at the CRA, and --keyint under the span from one key frame to the next.</desc><defs><marker id="gopn-ah" viewBox="0 0 10 10" refX="9" refY="5" markerWidth="7" markerHeight="7" orient="auto-start-reverse"><path class="d-head" d="M0 0 L10 5 L0 10 z"/></marker></defs><rect class="d-accent" x="8" y="70" width="30" height="36" rx="4"/><text class="d-label" x="23.0" y="92.5" text-anchor="middle">IDR</text><rect class="d-box" x="42" y="70" width="30" height="36" rx="4"/><text class="d-label" x="57.0" y="92.5" text-anchor="middle">B</text><rect class="d-box" x="76" y="70" width="30" height="36" rx="4"/><text class="d-label" x="91.0" y="92.5" text-anchor="middle">B</text><rect class="d-fill" x="110" y="70" width="30" height="36" rx="4"/><text class="d-label" x="125.0" y="92.5" text-anchor="middle">P</text><text class="d-text" x="160" y="93" text-anchor="middle">…</text><rect class="d-muted" x="178" y="70" width="30" height="36" rx="4" style="stroke-dasharray:4 3"/><text class="d-label" x="193.0" y="92.5" text-anchor="middle">B</text><rect class="d-muted" x="212" y="70" width="30" height="36" rx="4" style="stroke-dasharray:4 3"/><text class="d-label" x="227.0" y="92.5" text-anchor="middle">B</text><rect class="d-accent" x="246" y="70" width="30" height="36" rx="4"/><text class="d-label" x="261.0" y="92.5" text-anchor="middle">CRA</text><rect class="d-box" x="280" y="70" width="30" height="36" rx="4"/><text class="d-label" x="295.0" y="92.5" text-anchor="middle">B</text><rect class="d-fill" x="314" y="70" width="30" height="36" rx="4"/><text class="d-label" x="329.0" y="92.5" text-anchor="middle">P</text><path class="d-line" d="M42 64 V58 H106 V64"/><text x="42" y="48" text-anchor="start"><tspan class="d-mono">--bframes</tspan><tspan class="d-label"> between anchors</tspan></text><path class="d-line" d="M280 64 V58 H344 V64"/><text x="344" y="26" text-anchor="end"><tspan class="d-mono">--rc-lookahead</tspan><tspan class="d-label"> reads ahead</tspan></text><path class="d-line" d="M178 108 V114 H242 V108"/><text class="d-label" x="178" y="132">leading pictures</text><path class="d-arrow" d="M125 107 Q74 136 23 107" marker-end="url(#gopn-ah)"/><text x="8" y="150" text-anchor="start"><tspan class="d-mono">--ref</tspan><tspan class="d-label"> refs per list</tspan></text><text x="8" y="174" text-anchor="start"><tspan class="d-mono">--no-open-gop</tspan><tspan class="d-label"> IDR, closed segment</tspan></text><text x="8" y="194" text-anchor="start"><tspan class="d-mono">--radl</tspan><tspan class="d-label"> leading pictures decodable</tspan></text><path class="d-line" d="M23 206 V212 H261 V206"/><text x="8" y="234" text-anchor="start"><tspan class="d-mono">--keyint</tspan><tspan class="d-label"> key frame to key frame</tspan></text></svg>
<figcaption>The option behind each part of a stream in display order.</figcaption>
</figure>

<p class="optchips"><span><a href="options.md#gop-and-references">Options</a></span> <a href="options.md#opt-keyint"><code>--keyint</code></a> <a href="options.md#opt-bframes"><code>--bframes</code></a> <a href="options.md#opt-ref"><code>--ref</code></a> <a href="options.md#opt-rc-lookahead"><code>--rc-lookahead</code></a> <a href="options.md#opt-no-open-gop"><code>--no-open-gop</code></a> <a href="options.md#opt-radl"><code>--radl</code></a> <a href="options.md#opt-no-weightp"><code>--no-weightp</code></a></p>

A command for a packager that needs closed segments:

```sh
y265 in.y4m -o out.hevc --crf 23 --vbv-maxrate 4500 --vbv-bufsize 6000 \
  --keyint 120 --no-open-gop
```

The key frame gap is the longest a segment can run. Because a scene cut starts a new count, key frames don't fall on a fixed grid. To put them on one, use `--no-scenecut`, as [Key frames for streaming](#key-frames-for-streaming) shows. To put one on a frame you choose, list it in a QP file, as [Steering an encode](#steering-an-encode) describes.

Some receivers want more in the stream. One that joins mid-stream needs the parameter sets repeated at key frames. Others look for access unit delimiters or a recovery point message at each key frame.

<p class="optchips"><span><a href="options.md#stream-structure-and-sei">Options</a></span> <a href="options.md#opt-repeat-headers"><code>--repeat-headers</code></a> <a href="options.md#opt-aud"><code>--aud</code></a> <a href="options.md#opt-idr-recovery-sei"><code>--idr-recovery-sei</code></a> <a href="options.md#opt-qpfile"><code>--qpfile</code></a></p>

### Key frames for streaming

An HLS or DASH player switches rungs at segment boundaries. Every rung of a ladder therefore needs its key frames on the same frames. With `--no-scenecut`, key frames fall only on frame 0, `--keyint`, twice `--keyint` and so on, at every resolution. The encoder still detects scene changes but puts no key frame on them. Add `--no-open-gop` too, because packagers want each segment to open on a closed GOP.

Two-second segments at 24 fps:

```sh
y265 in.y4m -o out_1080p.hevc --crf 23 --vbv-maxrate 4500 --vbv-bufsize 6000 \
  --keyint 48 --no-scenecut --no-open-gop
```

The same from ffmpeg:

```sh
ffmpeg -i in.mp4 -c:v libyah265 -crf 23 -maxrate 4500k -bufsize 6000k \
  -yah265-params scenecut=0:keyint=48:no-open-gop out_1080p.mp4
```

A `K` line in a QP file and a live-control key frame still add a key frame where they say. Give every rung the same ones, or the rungs stop lining up.

To line the rungs up on shot starts, pass `--shot-list` in place of `--no-scenecut`, as [Planning by shot](#planning-by-shot) describes.

<p class="optchips"><span><a href="options.md#gop-and-references">Options</a></span> <a href="options.md#opt-no-scenecut"><code>--no-scenecut</code></a> <a href="options.md#opt-keyint"><code>--keyint</code></a> <a href="options.md#opt-no-open-gop"><code>--no-open-gop</code></a></p>

## Low latency

`--tune zerolatency` gives one frame of latency. It turns off B-frames, the lookahead and the frame-level parallelism that delays output.

```sh
y265 in.y4m -o out.hevc --tune zerolatency --crf 23
```

Without the tune, the latency is the lookahead depth in frames. Two opt-in tools add to it: the temporal filter under [Grain and noisy sources](#grain-and-noisy-sources) and the shot window under [Planning by shot](#planning-by-shot).

<p class="optchips"><span><a href="options.md#presets-and-tunes">Options</a></span> <a href="options.md#opt-tune"><code>--tune</code></a> <a href="options.md#opt-rc-lookahead"><code>--rc-lookahead</code></a> <a href="options.md#opt-bframes"><code>--bframes</code></a></p>

### Intra refresh

A key frame is a big picture. On a tight link it arrives late. `--intra-refresh` sends no key frame after the first. Each frame codes one column of blocks as intra. The column sweeps across the picture every `--keyint` frames.

A receiver can join at the start of any sweep. Its picture is exact once that sweep ends. Each sweep starts with a recovery point message and the parameter sets.

```sh
y265 in.y4m -o out.hevc --tune zerolatency --intra-refresh --keyint 30 \
  --bitrate 1500 --vbv-maxrate 1500 --vbv-bufsize 500
```

It codes P-frames only, from 4:2:0 input. A scene change doesn't add a key frame. A frame you list as a key frame in a QP file is still one. A new sweep starts after it. It can't be combined with `--bframes` above 0, `--open-gop`, `--gop-parallel` or `--pcm`.

<p class="optchips"><span><a href="options.md#gop-and-references">Options</a></span> <a href="options.md#opt-intra-refresh"><code>--intra-refresh</code></a> <a href="options.md#opt-keyint"><code>--keyint</code></a> <a href="options.md#opt-tune"><code>--tune</code></a></p>

## Set the bit depth and profile

The input picks the profile. No option names it.

| input | stream |
|---|---|
| 4:2:0, 8-bit | Main |
| 4:2:0, 10-bit | Main10 |
| 4:4:4, 8-bit | Main 4:4:4 |
| 4:4:4, 10-bit | Main 4:4:4 10 |
| 4:2:2, 8 or 10-bit | Main 4:2:2 10 |

A few options change what the table gives:

| option | what it does |
|---|---|
| [`--output-depth 10`](options.md#opt-output-depth) | codes 8-bit input as Main10 |
| [`--output-depth 8`](options.md#opt-output-depth) | codes 10-bit input at 8 bits by dropping two bits |
| [`--dither`](options.md#opt-dither) | with `--output-depth 8`, spreads the lost precision as a fine pattern so gradients don't turn into steps |
| [`--lossless`](options.md#opt-lossless) | codes a stream that decodes to the exact source |
| [`--level`](options.md#opt-level), [`--high-tier`](options.md#opt-high-tier) | signal a level and tier of your choosing. The encoder refuses a level the stream doesn't fit |

## HDR

yah265 writes HDR10, HDR10+, HLG and Dolby Vision profile 8. At `medium`, a stream that says PQ or HLG also gets its own tuning without any option. See [HDR](hdr.md) for the command for each format and the metadata files.

<p class="optchips"><span><a href="options.md#color-and-hdr">Options</a></span> <a href="options.md#opt-transfer"><code>--transfer</code></a> <a href="options.md#opt-colorprim"><code>--colorprim</code></a> <a href="options.md#opt-colormatrix"><code>--colormatrix</code></a> <a href="options.md#opt-master-display"><code>--master-display</code></a> <a href="options.md#opt-max-cll"><code>--max-cll</code></a> <a href="options.md#opt-dhdr10-info"><code>--dhdr10-info</code></a> <a href="options.md#opt-dolby-vision-rpu"><code>--dolby-vision-rpu</code></a> <a href="options.md#opt-dolby-vision-profile"><code>--dolby-vision-profile</code></a> <a href="options.md#opt-hdr-opt"><code>--hdr-opt</code></a></p>

## Long inputs and many cores

By default the encoder starts one worker per online CPU. Above one thread it also reads its input on a separate thread to keep a slow source from stalling the workers.

At `--threads 1`, two runs of one command write the same bytes. With more threads that [isn't promised](threading.md#what-stays-identical).

A long input can also be [coded as segments](threading.md#segment-parallel-encoding) on several encoder instances at once, then joined at key frames:

```sh
y265 in.y4m -o out.hevc --crf 23 --gop-parallel 4
```

The joined stream differs from the serial one and has no HRD timing.

<p class="optchips"><span><a href="options.md#threads">Options</a></span> <a href="options.md#opt-threads"><code>--threads</code></a> <a href="options.md#opt-gop-parallel"><code>--gop-parallel</code></a></p>

<p class="optchips"><span>Doesn't combine with</span> <a href="options.md#opt-pass"><code>--pass</code></a> <a href="options.md#opt-qpfile"><code>--qpfile</code></a> <a href="options.md#opt-zones"><code>--zones</code></a> <a href="options.md#opt-csv"><code>--csv</code></a> <a href="options.md#opt-shot-stats"><code>--shot-stats</code></a></p>

### Several machines

Farm mode codes the segments of a `--gop-parallel` encode on several machines and joins them. The file is the one `--gop-parallel` writes on one machine, byte for byte.

```sh
scripts/farm.py -o out.hevc --worker ssh:box1:2 --worker ssh:box2:2 \
    --seg-threads 4 -- /share/in.y4m --crf 23
```

Each `--worker` names a machine and how many segments it codes at once. Every machine needs the same y265 build. The input has to be at the same path on each, such as a network share. Farm mode works with CRF, constant QP and capped CRF, but not with `--bitrate` or `--target-vmaf`.

If a machine fails or stalls, another one takes its segment. If the run stops, the same command picks up where it left off.

## Hardware encoding on a Mac

On a Mac you can hand the encode to Apple's hardware. The encoder lists any options the hardware can't follow before it starts. The hardware uses a fraction of the CPU and usually less time. At the same bitrate it scores lower, by about four VMAF v1 points at the median. With `--hw auto`, the encoder uses its own path when the hardware isn't there.

```sh
y265 in.y4m -o out.hevc --hw videotoolbox --crf 26
ffmpeg -i in.mp4 -c:v libyah265 -yah265-params hw=videotoolbox:crf=26 -tag:v hvc1 out.mp4
```

<p class="optchips"><span>Options</span> <a href="options.md#opt-hw"><code>--hw</code></a> <a href="options.md#opt-hw-strict"><code>--hw-strict</code></a></p>

## Grain and noisy sources

All of these change the picture. Read them as tools for specific sources.

| option | what it does | when |
|---|---|---|
| [`--film-grain <s>`](options.md#opt-film-grain) | describes the source's estimated grain in an SEI message. A decoder that supports the message adds grain back | grainy film. 8-bit 4:2:0 only |
| [`--film-grain-denoise 1`](options.md#opt-film-grain-denoise) | with `--film-grain`, also removes the grain before coding. This is the half that saves bits | when the player is known to synthesize grain |
| [`--nr-intra`](options.md#opt-nr-intra), [`--nr-inter`](options.md#opt-nr-inter) | attenuate coefficients that look like noise before they're quantized | noisy sources. Both are off by default |
| [`--mcstf`](options.md#opt-mcstf) | averages each anchor picture with its motion-compensated neighbors before coding | noisy sources. It adds latency |
| [`--selective-mcstf`](options.md#opt-selective-mcstf) | skips that filter on pictures that measure clean | a mixed library. Flat-shaded animation loses quality to `--mcstf` without it |

## Steering an encode

Zones change the quantizer or the bit budget over frame ranges. This command holds the first 240 frames at QP 30, then gives a later range half again as many bits:

```sh
y265 in.y4m -o out.hevc --crf 23 --zones 0,239,q=30/1200,1439,b=1.5
```

Zones count in display order. No two may overlap.

A QP file forces frame types and QPs frame by frame. It can force key frames and intra frames, but not P or B frames.

`--qpmin` and `--qpmax` limit the quantizer in any mode. `--crf-min` and `--crf-max` limit a CRF encode.

<p class="optchips"><span><a href="options.md#steering-an-encode-by-frame">Options</a></span> <a href="options.md#opt-zones"><code>--zones</code></a> <a href="options.md#opt-qpfile"><code>--qpfile</code></a> <a href="options.md#opt-qpmin"><code>--qpmin</code></a> <a href="options.md#opt-qpmax"><code>--qpmax</code></a> <a href="options.md#opt-crf-min"><code>--crf-min</code></a> <a href="options.md#opt-crf-max"><code>--crf-max</code></a></p>

### Planning by shot

Most films are a series of shots that want different settings. yah265 can find the shots once and put a key frame on each. All of this is opt-in. A plain encode is unchanged.

**Find the shots.** `--shot-scan` reads the whole input once and writes a shot list. The scan runs at a fixed size. A 4K source and its 720p rung get the same list.

```sh
y265 in.y4m --shot-scan shots.txt
```

**A key frame on each shot.** `--shot-list` puts a key frame at the start of every listed shot. Every rung of a ladder then switches at the same frames. A player can change rung at a shot. A packager can cut there. `--shot-keys` does the same from the encoder's own cut detector, without a file. A key frame per shot cost at most 0.3% in bits on the test set.

```sh
y265 in.y4m -o 1080p.hevc --crf 24 --shot-list shots.txt
```

**A quality level for each shot.** `--shot-plan` reads the same list and moves each shot's QP up or down. Shots where bits buy little VMAF get fewer of them. Shots where bits buy more get those bits. The moves come from what the scan measured about each shot. The title's average QP stays where `--crf` put it. It also puts the key frames `--shot-list` does. It works with `--crf`, with or without a VBV, and with `--bitrate` under a VBV whose maxrate is above it. On eight film excerpts it saved a median 1.2% of the bits at the same VMAF v1 under CRF, and 0.5% under capped VBR.

```sh
y265 in.y4m -o 1080p.hevc --crf 24 --shot-plan shots.txt
```

A ladder tool can write its own offsets into the list as `qp=` on each shot line. The encoder then uses them as written.

A shot window lets a CRF encode plan a whole shot at a time. It works only with CRF and adds latency. In my tests it saved little bitrate and made quality less even from shot to shot.

Shot statistics are one JSON line per shot. A ladder builder can use them to pick a rate per shot. The stream is the same with them on.

<p class="optchips"><span><a href="options.md#shot-aware-options">Options</a></span> <a href="options.md#opt-shot-scan"><code>--shot-scan</code></a> <a href="options.md#opt-shot-list"><code>--shot-list</code></a> <a href="options.md#opt-shot-plan"><code>--shot-plan</code></a> <a href="options.md#opt-shot-keys"><code>--shot-keys</code></a> <a href="options.md#opt-shot-window"><code>--shot-window</code></a> <a href="options.md#opt-shot-stats"><code>--shot-stats</code></a></p>

**Building a ladder.** The [hull script](scripts/shot_hull.py) scores each shot of several encodes on VMAF v1 and prints each shot's best rungs across resolutions.

## Live control

Live control changes an encode while it runs. You can change the bitrate, the buffer, the CRF and the QP limits, ask for a key frame, or offset the QP over a range of frames.

Each change is one line of a control log. A line names the input frame it applies to, counted from 0:

```
#livectl 1
# drop to 2 Mbit/s at 4 s, back up at 12 s (30 fps)
120 rate bitrate=2000 maxrate=3000 bufsize=3000
120 key
360 rate bitrate=6000 maxrate=9000 bufsize=9000
400 zone last=459 offset=-3
```

The kinds are `rate`, `crf`, `qp` (`min=`, `max=`), `key` and `zone` (`last=`, `offset=`). A key frame goes on exactly the frame it names. A rate or CRF change applies from the first frame coded at or after that frame. That can be a B frame's anchor a few frames later.

Three options feed and record the log:

- `--control <file>` applies a whole log. A bad line, or a change the encode can't take, stops the run before it starts.
- `--control-fd <n>` reads lines from an open descriptor while the encode runs. `next` in place of a frame number means the next frame in. A line for a frame that has already gone in is skipped with a warning.
- `--control-out <file>` writes every change that took effect. Fed back through `--control`, it gives the same stream at one thread.

`--frame-stats` lists each change on the frame where it took effect.

A rate change needs a rate mode that has one: `--bitrate` for the bitrate, a buffer for `maxrate` and `bufsize`. You can't add or remove a buffer mid-stream. Because a stream with a buffer has HRD timing by default, its rate and buffer can only go down from where they started. To raise them later, use `--no-hrd`, or start at your top rate or name a `--level`.

Under capped VBR, the encoder aims each rate change at its new bitrate from that frame on. Any balance left from before the change is paid back slowly, at 2% of the new rate.

Live control uses one encoder, so `--gop-parallel` codes serially with it.

In ffmpeg, an I frame you force with `-force_key_frames` becomes a key frame. A change to `bit_rate`, `rc_max_rate` or `rc_buffer_size` between two frames becomes a rate change on the next one.

<p class="optchips"><span><a href="options.md#live-control">Options</a></span> <a href="options.md#opt-control"><code>--control</code></a> <a href="options.md#opt-control-fd"><code>--control-fd</code></a> <a href="options.md#opt-control-out"><code>--control-out</code></a> <a href="options.md#opt-frame-stats"><code>--frame-stats</code></a> <a href="options.md#opt-no-hrd"><code>--no-hrd</code></a></p>

## Encode reports

`--report` writes one HTML page about an encode:

```sh
y265 clip.y4m -o clip.hevc --crf 26 --report clip.html
```

The page has the bitrate and QP over time, the buffer fill when a VBV is set, and a timeline of what the encoder detected: scene cuts, screen content, handheld jitter, dark and flat frames. Each shot gets a card with a thumbnail. Everything is inside the one file, so it opens offline and can be mailed to anyone. Thumbnails need ffmpeg. Without it the page shows gray boxes.

The report doesn't change the stream. `--frame-stats` writes the same per-frame data as JSON lines if you'd rather chart it yourself.

## Identification marks

Each stream has an SEI message that names the encoder and the options the run resolved. `--no-info` drops it.

The stream also has a fingerprint that survives when the SEI is stripped. It costs no bits and doesn't change the picture. See the [fingerprint design note](docs/stream-fingerprint.md) to read it back.

<p class="optchips"><span><a href="options.md#stream-structure-and-sei">Options</a></span> <a href="options.md#opt-no-info"><code>--no-info</code></a> <a href="options.md#opt-no-fingerprint"><code>--no-fingerprint</code></a></p>

## Environment knobs

Settings named `N265_*` in the environment reach tuning parameters the command line doesn't expose. They're for experiments. `--show-config` prints the ones a preset named and marks any your environment overrode. The [knob list](docs/knobs.md) names them all.

## From ffmpeg

ffmpeg's options map onto `y265`'s:

| ffmpeg | `y265` | note |
|---|---|---|
| `-preset` | `--preset` | `medium` when unset |
| `-tune` | `--tune` | |
| `-crf` | `--crf` | |
| `-qp` | `--qp` | |
| `-b:v` | `--bitrate` | wins over `-crf` and `-qp` |
| `-maxrate`, `-bufsize` | `--vbv-maxrate`, `--vbv-bufsize` | |
| `-g` | `--keyint` | |
| `-bf` | `--bframes` | |
| `-refs` | `--ref` | |
| `-threads` | `--threads` | 0 is one per CPU on both sides |
| `-color_primaries`, `-color_trc`, `-colorspace` | `--colorprim`, `--transfer`, `--colormatrix` | written only when set |
| `-color_range pc` | `--range full` | |
| `-yah265-params` | any other coding option | names without the dashes, joined by colons |

Always pass a rate option such as `-crf`. Without one the encode is fixed QP 26, which isn't what x265 does.

`-yah265-params` takes the long name of any option on the [Options](options.md) page:

```sh
ffmpeg -i in.mp4 -c:v libyah265 -preset medium -crf 25 \
  -yah265-params rc-lookahead=30:no-open-gop out.mp4
```

A switch takes no value, or `1` or `0`. HDR10+ and Dolby Vision need `y265`, because their metadata options don't work through `-yah265-params`.

The pixel format picks the bit depth. 4:2:0, 4:2:2 and 4:4:4 work at 8 and 10 bits. `-pix_fmt yuv420p10le` codes 8-bit content as Main10.

## What ships

| area | what's there |
|---|---|
| profiles | Main, Main10, Main 4:4:4, Main 4:4:4 10, Main 4:2:2 10, lossless |
| rate control | CRF, capped CRF, capped VBR, CBR with filler, ABR, two-pass, fixed QP, zones, a QP file |
| live control | rate, buffer, CRF and QP changes, key frames and zones while the encode runs, from a log or a descriptor |
| buffers | VBV with HRD signaling, a bound on one picture's size, level and tier checks |
| conformance | every stream fits a level, so any standard decoder can play it |
| structure | open and closed GOP, hierarchical B-frames, scene cut detection, weighted prediction, temporal layers |
| analysis | adaptive quantization, lookahead bit allocation, rate-distortion optimized quantization, screen content detection |
| HDR | HDR10, HDR10+, HLG, Dolby Vision profiles 8.1, 8.2 and 8.4, HDR tuning at `medium` |
| filters | film grain SEI, coefficient noise reduction, a temporal filter, dither |
| signaling | the VUI color flags, picture hash SEI, recovery point SEI, access unit delimiters, repeated headers |
| threads | one pool on all cores, segment-parallel encoding |
| speed | SIMD on arm64 and x86-64, picked at run time. [Architectures](architectures.md) has the list |
| front ends | the `y265` command line and the `libyah265` encoder in ffmpeg |
| reports | CSV statistics, per-frame and per-shot JSON statistics, the resolved configuration |

## Left out on purpose

| not offered | why |
|---|---|
| six of x265's preset names | `medium` already beats x265's `medium`, so I kept four. `y265` maps the other names to the nearest of `fast`, `medium`, `slow` and `slower`. See [Pick a preset](#pick-a-preset) |
| four of x265's tune names | each has a short reason under [Tune names](#tune-names) |
| `--scaling-list` | x265 leaves custom quantizer matrices off by default too. yah265 uses its own perceptual tuning instead |
| interlaced coding | almost all video made today is progressive. Broadcast and cable contribution still use it. If you need it, [open an issue](https://github.com/terranvigil/yah265/issues) |
| 12-bit | 8-bit and 10-bit cover the delivery formats, HDR10, HLG and Dolby Vision included |
| `--psy-rd`, `--psy-rdoq` | yah265's perceptual tuning is built into its presets, so there's nothing to set |
| x265's threading options | yah265 uses every core by default and needs no tuning. `--threads n` caps it at n |
| analysis save and load | they let a bitrate ladder reuse one encode's analysis. The per-shot scan covers the same need |
| Dolby Vision profile 5 | not supported yet. If you need it, [open an issue](https://github.com/terranvigil/yah265/issues) |
| HEVC screen-content tools (SCC) | few decoders play SCC streams. yah265 gets its screen-recording gains inside the normal profiles, so any HEVC player can show them |
| MV-HEVC (multiview, as in Apple's spatial video) | not supported yet. If you need it, [open an issue](https://github.com/terranvigil/yah265/issues) |

### Tune names

The encoder prints a reason when it refuses one of x265's other four tune names.

| refused tune | why | what to use |
|---|---|---|
| `psnr`, `ssim` | they make the encoder chase one metric's score, which can cost visible quality. yah265 tunes for how the picture looks | nothing: to measure SSIM, `--ssim` prints the score and leaves the encode as it is |
| `grain` | yah265 keeps film grain at its default settings, so it needs no separate tune | nothing. `--film-grain` is a different tool, described [above](#grain-and-noisy-sources) |
| `fastdecode` | it turns off the deblocking and SAO filters to make decoding cheaper. yah265 always runs them, because the picture looks blockier without them | `--no-weightp` and `--no-weightb` give the rest of it |
