---
title: Build and first encode - yah265
description: Build ffmpeg with yah265 in one command, run a first encode, and build the y265 command-line encoder on its own.
---

# Build and first encode

There are two ways to run yah265. Most people want it inside ffmpeg, where it's the `libyah265` encoder. The other is `y265`, the project's own command-line encoder. It reads raw video from a file or a pipe.

<figure class="diagram">
<svg class="d-wide" viewBox="0 0 720 250" role="img" aria-labelledby="start-fig-t start-fig-d">
<title id="start-fig-t">Two ways to run yah265</title>
<desc id="start-fig-d">In the first way one ffmpeg process decodes the source, encodes it with libyah265 and writes an MP4. In the second way ffmpeg decodes the source into a Y4M pipe, y265 reads the pipe and writes a raw HEVC stream, and a second ffmpeg run wraps that stream in an MP4.</desc>
<text class="d-label" x="10" y="24">1. Inside ffmpeg</text>
<rect class="d-box" x="10" y="38" width="120" height="56" rx="6"/>
<text class="d-text" x="70" y="71" text-anchor="middle">in.mp4</text>
<rect class="d-fill" x="190" y="38" width="340" height="56" rx="6"/>
<text class="d-label" x="204" y="60">ffmpeg</text>
<text class="d-label" x="204" y="80">decode</text>
<rect class="d-accent" x="280" y="48" width="140" height="36" rx="6"/>
<text class="d-mono" x="350" y="71" text-anchor="middle">libyah265</text>
<text class="d-label" x="484" y="80" text-anchor="middle">mux</text>
<rect class="d-box" x="590" y="38" width="120" height="56" rx="6"/>
<text class="d-text" x="650" y="71" text-anchor="middle">out.mp4</text>
<line class="d-arrow" x1="130" y1="66" x2="188" y2="66"/>
<line class="d-arrow" x1="530" y1="66" x2="588" y2="66"/>
<text class="d-label" x="10" y="150">2. The y265 command line</text>
<rect class="d-box" x="10" y="164" width="120" height="56" rx="6"/>
<text class="d-text" x="70" y="197" text-anchor="middle">in.mp4</text>
<rect class="d-fill" x="170" y="164" width="90" height="56" rx="6"/>
<text class="d-label" x="215" y="188" text-anchor="middle">ffmpeg</text>
<text class="d-label" x="215" y="206" text-anchor="middle">decode</text>
<rect class="d-accent" x="320" y="164" width="100" height="56" rx="6"/>
<text class="d-mono" x="370" y="197" text-anchor="middle">y265</text>
<rect class="d-fill" x="490" y="164" width="70" height="56" rx="6"/>
<text class="d-label" x="525" y="188" text-anchor="middle">ffmpeg</text>
<text class="d-label" x="525" y="206" text-anchor="middle">mux</text>
<rect class="d-box" x="600" y="164" width="110" height="56" rx="6"/>
<text class="d-text" x="655" y="197" text-anchor="middle">out.mp4</text>
<line class="d-arrow" x1="130" y1="192" x2="168" y2="192"/>
<line class="d-arrow" x1="260" y1="192" x2="318" y2="192"/>
<text class="d-label" x="289" y="180" text-anchor="middle">Y4M</text>
<line class="d-arrow" x1="420" y1="192" x2="488" y2="192"/>
<text class="d-label" x="454" y="180" text-anchor="middle">.hevc</text>
<line class="d-arrow" x1="560" y1="192" x2="598" y2="192"/>
</svg>
<svg class="d-narrow" viewBox="0 0 360 372" role="img" aria-labelledby="start-fig-t-n start-fig-d-n">
<title id="start-fig-t-n">Two ways to run yah265</title>
<desc id="start-fig-d-n">In the first way one ffmpeg process decodes the source, encodes it with libyah265 and writes an MP4. In the second way ffmpeg decodes the source into a Y4M pipe, y265 reads the pipe and writes a raw HEVC stream, and a second ffmpeg run wraps that stream in an MP4.</desc>
<text class="d-label" x="8" y="16">1. Inside</text>
<text class="d-label" x="8" y="32">ffmpeg</text>
<rect class="d-box" x="30" y="44" width="120" height="40" rx="6"/>
<text class="d-text" x="90" y="69" text-anchor="middle">in.mp4</text>
<line class="d-arrow" x1="90" y1="84" x2="90" y2="110"/>
<rect class="d-fill" x="8" y="112" width="164" height="132" rx="6"/>
<text class="d-label" x="90" y="132" text-anchor="middle">ffmpeg</text>
<text class="d-label" x="90" y="154" text-anchor="middle">decode</text>
<rect class="d-accent" x="30" y="166" width="120" height="36" rx="6"/>
<text class="d-mono" x="90" y="188.5" text-anchor="middle">libyah265</text>
<text class="d-label" x="90" y="226" text-anchor="middle">mux</text>
<line class="d-arrow" x1="90" y1="244" x2="90" y2="322"/>
<rect class="d-box" x="30" y="324" width="120" height="40" rx="6"/>
<text class="d-text" x="90" y="349" text-anchor="middle">out.mp4</text>
<text class="d-label" x="196" y="16">2. The y265</text>
<text class="d-label" x="196" y="32">command line</text>
<rect class="d-box" x="210" y="44" width="120" height="40" rx="6"/>
<text class="d-text" x="270" y="69" text-anchor="middle">in.mp4</text>
<line class="d-arrow" x1="270" y1="84" x2="270" y2="106"/>
<rect class="d-fill" x="225" y="108" width="90" height="44" rx="6"/>
<text class="d-label" x="270" y="126.5" text-anchor="middle">ffmpeg</text>
<text class="d-label" x="270" y="142.5" text-anchor="middle">decode</text>
<line class="d-arrow" x1="270" y1="152" x2="270" y2="182"/>
<text class="d-label" x="278" y="172">Y4M</text>
<rect class="d-accent" x="220" y="184" width="100" height="40" rx="6"/>
<text class="d-mono" x="270" y="208.5" text-anchor="middle">y265</text>
<line class="d-arrow" x1="270" y1="224" x2="270" y2="254"/>
<text class="d-label" x="278" y="244">.hevc</text>
<rect class="d-fill" x="225" y="256" width="90" height="44" rx="6"/>
<text class="d-label" x="270" y="274.5" text-anchor="middle">ffmpeg</text>
<text class="d-label" x="270" y="290.5" text-anchor="middle">mux</text>
<line class="d-arrow" x1="270" y1="300" x2="270" y2="322"/>
<rect class="d-box" x="210" y="324" width="120" height="40" rx="6"/>
<text class="d-text" x="270" y="349" text-anchor="middle">out.mp4</text>
</svg>
<figcaption>Both ways run the same encoder with the same options.</figcaption>
</figure>

## Build ffmpeg with yah265

You need git, a C compiler, Meson, Ninja, pkg-config and make. One command builds everything from a clone of the repository:

```sh
git clone https://github.com/terranvigil/yah265 && cd yah265
scripts/build-ffmpeg.sh
```

Once the script has built the yah265 library, it installs it under `build-ffmpeg/inst`. It then fetches an ffmpeg source tree that has the `libyah265` encoder. The `ffmpeg` and `ffprobe` it builds from that tree go in `build-ffmpeg/ffmpeg`. Two options change that:

| option | what it does |
|---|---|
| `--with-x265` | also enables `libx265`, for side-by-side tests |
| `--prefix DIR` | installs the yah265 library in `DIR` |

The ffmpeg it builds is a GPL binary. yah265 is GPL-2.0-or-later. ffmpeg only links a GPL library when it's configured with `--enable-gpl`.

## Your first encode

```sh
build-ffmpeg/ffmpeg/ffmpeg -i in.mp4 -c:v libyah265 -preset medium -crf 25 out.mp4
```

That's the command you'd give x265 with one word changed. `out.mp4` plays in anything that plays HEVC.

<figure class="diagram">
<svg class="d-wide" viewBox="0 0 720 132" role="img" aria-labelledby="swap-fig-t swap-fig-d">
<title id="swap-fig-t">One word changes in the ffmpeg command</title>
<desc id="swap-fig-d">Two ffmpeg command lines, one above the other. They match except for the encoder name after -c:v, which is libx265 in the first and libyah265 in the second.</desc>
<text class="d-label" x="20" y="18">today</text>
<text class="d-mono" x="186" y="47" text-anchor="end">ffmpeg -i in.mp4 -c:v</text>
<rect class="d-fill" x="194" y="28" width="92" height="28" rx="6"/>
<text class="d-mono" x="240" y="47" text-anchor="middle">libx265</text>
<text class="d-mono" x="296" y="47">-preset medium -crf 25 out.mp4</text>
<line class="d-arrow d-accent" x1="240" y1="60" x2="240" y2="84"/>
<text class="d-label" x="254" y="77">one word</text>
<text class="d-label" x="20" y="78">with yah265</text>
<text class="d-mono" x="186" y="107" text-anchor="end">ffmpeg -i in.mp4 -c:v</text>
<rect class="d-accent" x="194" y="88" width="92" height="28" rx="6"/>
<text class="d-mono" x="240" y="107" text-anchor="middle">libyah265</text>
<text class="d-mono" x="296" y="107">-preset medium -crf 25 out.mp4</text>
</svg>
<svg class="d-narrow" viewBox="0 0 360 182" role="img" aria-labelledby="swap-fig-t-n swap-fig-d-n">
<title id="swap-fig-t-n">One word changes in the ffmpeg command</title>
<desc id="swap-fig-d-n">Two ffmpeg command lines, one above the other. They match except for the encoder name after -c:v, which is libx265 in the first and libyah265 in the second.</desc>
<text class="d-label" x="8" y="16">today</text>
<text class="d-mono" x="8" y="46">ffmpeg -i in.mp4 -c:v</text>
<rect class="d-fill" x="180" y="28" width="92" height="28" rx="6"/>
<text class="d-mono" x="226" y="46" text-anchor="middle">libx265</text>
<text class="d-mono" x="8" y="76">-preset medium -crf 25 out.mp4</text>
<path class="d-arrow d-accent" d="M272 42 H330 V136 H274"/>
<text class="d-label" x="322" y="100" text-anchor="end">one word</text>
<text class="d-label" x="8" y="112">with yah265</text>
<text class="d-mono" x="8" y="140">ffmpeg -i in.mp4 -c:v</text>
<rect class="d-accent" x="180" y="122" width="92" height="28" rx="6"/>
<text class="d-mono" x="226" y="140" text-anchor="middle">libyah265</text>
<text class="d-mono" x="8" y="170">-preset medium -crf 25 out.mp4</text>
</svg>
<figcaption>Everything else on the command line stays as it was.</figcaption>
</figure>

Always pass a rate option. With none, the encoder codes every frame at one fixed quantizer, which is meant for testing. For streaming I'd use capped CRF, which adds a peak rate and a buffer to the CRF command:

```sh
ffmpeg -i in.mp4 -c:v libyah265 -preset medium -crf 25 -maxrate 4500k -bufsize 6000k out.mp4
```

The encoding guide covers [every rate mode](guide.md#pick-a-rate-mode) and the [ffmpeg options the encoder takes](guide.md#from-ffmpeg).

## Pick a quality

`-crf` sets the quality. The file size then follows the content. A lower number gives a better picture and a bigger file. I'd start at 25, then move by two or three at a time until the picture is good enough.

The CRF scale is yah265's own. A number that suited x265 won't give the same file size here. [Coming from x265](from-x265.md) has a translation table. It also maps x265's options onto `y265`'s. When you need a size or a bitrate instead, the guide's [rate modes](guide.md#pick-a-rate-mode) cover it.

## Build y265 on its own

`y265` needs a C11 compiler, Meson, Ninja and Python 3. The reference compiler is clang.

```sh
make build-rel
```

That leaves the encoder at `build-rel/y265`. The binary is named `y265`, though the project is yah265. One binary codes every bit depth. It picks the depth from its input.

`y265` turns Y4M or raw YUV into a raw HEVC stream. ffmpeg can feed it any source through a pipe:

```sh
ffmpeg -i in.mp4 -pix_fmt yuv420p -f yuv4mpegpipe - | build-rel/y265 - -o out.hevc --crf 25
```

A Y4M file works without the pipe, as `build-rel/y265 input.y4m -o out.hevc --crf 25`. Because raw YUV has no header, you describe it with `--input-res`, `--input-csp` and `--fps`.

## Play it and mux it

A file from the ffmpeg route is already an MP4. `y265` writes a bare stream. ffmpeg plays it as it is, or wraps it without re-encoding:

<!-- recipe: after start:170 -->
```sh
ffmpeg -i out.hevc -c copy out.mp4
```

## Check the stream

`y265` can write the pictures it expects a decoder to produce. Decode the stream with ffmpeg, then compare the two files:

```sh
build-rel/y265 input.y4m -o out.hevc --crf 25 --dump-recon recon.yuv
ffmpeg -i out.hevc -f rawvideo decoded.yuv
cmp recon.yuv decoded.yuv
```

`cmp` prints nothing when every sample matches. The project runs this check on every stream in its tests, against ffmpeg and against libde265.

## Build ffmpeg by hand

The script runs three steps. Run them yourself when you want a different prefix layout or your own ffmpeg configure flags.

<details>
<summary>The manual steps</summary>

<pre><code>meson setup build-ff -Dbuildtype=release -Dprefix=$PWD/inst && ninja -C build-ff install
git clone -b yah265 https://github.com/terranvigil/FFmpeg ffmpeg-yah265 && cd ffmpeg-yah265
PKG_CONFIG_PATH=../inst/lib/pkgconfig ./configure --enable-gpl --enable-libyah265 && make -j
</code></pre>

</details>

The first line installs the library, its header and its pkg-config file. The second fetches the ffmpeg tree with the wrapper. The third configures and builds it.

## Next

- The [encoding guide](guide.md) covers rate modes, presets, threads and what ships.
- [Options](options.md) lists every `y265` option with its default.
- [HDR](hdr.md) covers HDR10, HDR10+, HLG and Dolby Vision.
- [Check it yourself](check-it-yourself.md) reruns a published result on your machine.
