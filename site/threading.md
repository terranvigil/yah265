---
title: Threading - yah265
description: How yah265 spreads an encode over many cores, what a row of blocks waits for and which outputs are promised to repeat.
---

# Threading

yah265 uses every core by default. This page explains how the work is divided and what limits it. It also says exactly what the threading promises about the output. [Design](design.md#the-pipeline) has the pipeline this page takes apart.

## One pool, rows as work

One shared pool of worker threads runs the whole encode. `--threads` sets its size. The default is one worker per CPU.

The unit of work is a row of coding tree units. A frame becomes three tasks per row: encode the row, deblock it, then run SAO and publish the finished row for other frames to predict from. One more task per frame does the entropy coding. It walks the decisions the rows recorded and writes the bits.

The stream stays simple because the entropy coder is kept out of the rows. HEVC has optional tools that make a stream easier to code in parallel, at a cost in bitrate. yah265 writes one slice per picture and uses none of them. Because the entropy passes of different frames don't depend on each other, they run in parallel as ordinary pool work.

Inside a frame, a block needs its left, upper and upper-right neighbors to be finished. Each row therefore trails the row above it by two coding tree units.

## What a row waits for

A row in a P or B frame predicts from other frames. It can start as soon as each reference frame has finished the rows its motion search can reach. That reach is fixed by geometry: the search range, the fractional-sample step and the length of the interpolation filter.

<figure class="diagram">
<svg viewBox="0 0 720 274" role="img" aria-label="Rows of three frames in flight, and what a row waits for">
<title>Rows of three frames in flight, and what a row waits for</title>
<desc>Three frames side by side, each drawn as six rows with a bar for how far each row has been coded. The first frame is a reference and is nearly done. The second predicts from the first and has four rows under way, each trailing the row above. The third predicts from both and has just started. The block being coded in the second frame is joined to an outlined band of three rows in the first frame, the rows its motion search can reach. A block in the third frame is joined to a band of two rows in the second.</desc>
<text class="d-label" x="120" y="46" text-anchor="middle">frame 1, a reference</text>
<rect class="d-muted" x="20" y="60" width="200" height="25" rx="0"/>
<rect class="d-fill" x="20" y="60" width="200" height="25" rx="0"/>
<rect class="d-muted" x="20" y="85" width="200" height="25" rx="0"/>
<rect class="d-fill" x="20" y="85" width="200" height="25" rx="0"/>
<rect class="d-muted" x="20" y="110" width="200" height="25" rx="0"/>
<rect class="d-fill" x="20" y="110" width="200" height="25" rx="0"/>
<rect class="d-muted" x="20" y="135" width="200" height="25" rx="0"/>
<rect class="d-fill" x="20" y="135" width="200" height="25" rx="0"/>
<rect class="d-muted" x="20" y="160" width="200" height="25" rx="0"/>
<rect class="d-fill" x="20" y="160" width="200" height="25" rx="0"/>
<rect class="d-muted" x="20" y="185" width="200" height="25" rx="0"/>
<rect class="d-fill" x="20" y="185" width="120" height="25" rx="0"/>
<text class="d-label" x="360" y="46" text-anchor="middle">frame 2 predicts from 1</text>
<rect class="d-muted" x="260" y="60" width="200" height="25" rx="0"/>
<rect class="d-fill" x="260" y="60" width="200" height="25" rx="0"/>
<rect class="d-muted" x="260" y="85" width="200" height="25" rx="0"/>
<rect class="d-fill" x="260" y="85" width="200" height="25" rx="0"/>
<rect class="d-muted" x="260" y="110" width="200" height="25" rx="0"/>
<rect class="d-fill" x="260" y="110" width="150" height="25" rx="0"/>
<rect class="d-muted" x="260" y="135" width="200" height="25" rx="0"/>
<rect class="d-fill" x="260" y="135" width="100" height="25" rx="0"/>
<rect class="d-muted" x="260" y="160" width="200" height="25" rx="0"/>
<rect class="d-muted" x="260" y="185" width="200" height="25" rx="0"/>
<text class="d-label" x="600" y="46" text-anchor="middle">frame 3 predicts from 1 and 2</text>
<rect class="d-muted" x="500" y="60" width="200" height="25" rx="0"/>
<rect class="d-fill" x="500" y="60" width="60" height="25" rx="0"/>
<rect class="d-muted" x="500" y="85" width="200" height="25" rx="0"/>
<rect class="d-muted" x="500" y="110" width="200" height="25" rx="0"/>
<rect class="d-muted" x="500" y="135" width="200" height="25" rx="0"/>
<rect class="d-muted" x="500" y="160" width="200" height="25" rx="0"/>
<rect class="d-muted" x="500" y="185" width="200" height="25" rx="0"/>
<rect class="d-line d-accent" x="20" y="110" width="200" height="75" rx="0"/>
<line class="d-line d-accent" x1="360" y1="135" x2="220" y2="110"/>
<line class="d-line d-accent" x1="360" y1="160" x2="220" y2="185"/>
<rect class="d-accent" x="360" y="135" width="20" height="25" rx="0"/>
<rect class="d-line d-accent" x="260" y="60" width="200" height="50" rx="0"/>
<line class="d-line d-accent" x1="560" y1="60" x2="460" y2="60"/>
<line class="d-line d-accent" x1="560" y1="85" x2="460" y2="110"/>
<rect class="d-accent" x="560" y="60" width="20" height="25" rx="0"/>
<text class="d-label" x="120" y="234" text-anchor="middle">rows 1 to 5 done</text>
<text class="d-label" x="360" y="234" text-anchor="middle">each row trails the one above</text>
<text class="d-label" x="600" y="234" text-anchor="middle">started before frame 2 is done</text>
<text class="d-label" x="360" y="264" text-anchor="middle">outlined rows: what the highlighted block's motion search can reach in its reference</text>
</svg>
<figcaption>A row can run as soon as the outlined rows are finished in every reference. It doesn't wait for the whole reference frame.</figcaption>
</figure>

The start rule is only a good guess. Whenever a block is about to touch a reference picture, the encoder checks that the rows it will read are published and waits if they aren't. While it waits, the worker picks up work from an older frame. Because waits always point at older frames and the oldest frame never waits on another, the pool can't lock up.

I chose waiting over clamping. Another way to make frames overlap is to forbid motion vectors that point too far down the reference. That changes what the encoder can choose. Here a vector may point anywhere the standard allows.

## Frames in flight

One frame can't keep a large pool busy, because only so many of its rows can run at once. The encoder therefore keeps several frames in flight. The count follows the picture.

| picture | frames in flight |
|---|--:|
| too small to fill the pool with two frames, such as CIF | 8 |
| 720p | 8 |
| 1080p and 4K on the 64x64 tree | 12 |
| anything else | 6 |

<figure class="diagram">
<svg viewBox="0 0 720 350" role="img" aria-label="Twelve frames in flight, reference frames first">
<title>Twelve frames in flight, reference frames first</title>
<desc>A timeline with one bar per frame in coding order. The bars overlap, and at the marked moment all twelve are being coded at once. Frames that other frames predict from start first. The four B frames that nothing predicts from start last.</desc>
<text class="d-mono" x="96" y="55" text-anchor="end">P8</text>
<rect class="d-accent" x="110" y="44" width="301.6" height="14" rx="3"/>
<text class="d-mono" x="96" y="75" text-anchor="end">B4</text>
<rect class="d-accent" x="168" y="64" width="301.6" height="14" rx="3"/>
<text class="d-mono" x="96" y="95" text-anchor="end">P16</text>
<rect class="d-accent" x="197" y="84" width="301.6" height="14" rx="3"/>
<text class="d-mono" x="96" y="115" text-anchor="end">B2</text>
<rect class="d-accent" x="226" y="104" width="301.6" height="14" rx="3"/>
<text class="d-mono" x="96" y="135" text-anchor="end">B6</text>
<rect class="d-accent" x="226" y="124" width="301.6" height="14" rx="3"/>
<text class="d-mono" x="96" y="155" text-anchor="end">B12</text>
<rect class="d-accent" x="255" y="144" width="301.6" height="14" rx="3"/>
<text class="d-mono" x="96" y="175" text-anchor="end">B10</text>
<rect class="d-accent" x="284" y="164" width="301.6" height="14" rx="3"/>
<text class="d-mono" x="96" y="195" text-anchor="end">B14</text>
<rect class="d-accent" x="284" y="184" width="301.6" height="14" rx="3"/>
<text class="d-mono" x="96" y="215" text-anchor="end">B1</text>
<rect class="d-fill" x="318.8" y="204" width="301.6" height="14" rx="3"/>
<text class="d-mono" x="96" y="235" text-anchor="end">B3</text>
<rect class="d-fill" x="330.4" y="224" width="301.6" height="14" rx="3"/>
<text class="d-mono" x="96" y="255" text-anchor="end">B5</text>
<rect class="d-fill" x="342" y="244" width="301.6" height="14" rx="3"/>
<text class="d-mono" x="96" y="275" text-anchor="end">B7</text>
<rect class="d-fill" x="353.6" y="264" width="301.6" height="14" rx="3"/>
<line class="d-dash" x1="382.6" y1="30" x2="382.6" y2="288"/>
<text class="d-label" x="382.6" y="22" text-anchor="middle">twelve frames in flight at this moment</text>
<line class="d-arrow d-muted" x1="110" y1="300" x2="690" y2="300"/>
<text class="d-label" x="690" y="318" text-anchor="end">time</text>
<rect class="d-accent" x="110" y="322" width="12" height="12" rx="2"/>
<text class="d-label" x="130" y="333" text-anchor="start">frames that other frames predict from</text>
<rect class="d-fill" x="410" y="322" width="12" height="12" rx="2"/>
<text class="d-label" x="430" y="333" text-anchor="start">frames nothing predicts from</text>
</svg>
<figcaption>A sketch of the schedule on a 64x64 tree, drawn to show the order. The bar lengths aren't measured.</figcaption>
</figure>

Two rules keep a deep queue moving. Frames that others predict from get workers first, since later frames are waiting on their rows. On wide pictures a row that finds the row above too close gives up its worker and resumes later.

More frames in flight need more memory, because each one holds its own pictures.

## The lookahead and the driver

One thread, the driver, prepares each frame before the pool can start on it. It feeds the lookahead, classifies flat and dark areas, builds the adaptive quantization field, analyzes weighted prediction and runs the propagation step that decides where later frames will look.

All of that scales with the picture. At 4K the driver alone was slower than the pool. Workers sat idle while they waited for the next frame.

<figure class="diagram">
<svg viewBox="0 0 720 272" role="img" aria-label="The driver's per-frame analysis, alone and split over the pool">
<title>The driver's per-frame analysis, alone and split over the pool</title>
<desc>Two timelines of the work the driver does before it can dispatch a frame: the lookahead push, the flat census, the AQ field, the dark map, the weighted prediction analysis and the cu-tree. On one thread the six steps run end to end. With many threads each step is cut into items that free workers run beside the driver, so each step is shorter and the frame is dispatched sooner.</desc>
<text class="d-label" x="20" y="34" text-anchor="start">one thread: the driver does each step alone</text>
<rect class="d-box" x="20" y="44" width="134" height="34" rx="6"/>
<text class="d-label" x="87" y="65.5" text-anchor="middle">push</text>
<rect class="d-box" x="160" y="44" width="84" height="34" rx="6"/>
<text class="d-label" x="202" y="65.5" text-anchor="middle">flat census</text>
<rect class="d-box" x="250" y="44" width="74" height="34" rx="6"/>
<text class="d-label" x="287" y="65.5" text-anchor="middle">AQ field</text>
<rect class="d-box" x="330" y="44" width="74" height="34" rx="6"/>
<text class="d-label" x="367" y="65.5" text-anchor="middle">dark map</text>
<rect class="d-box" x="410" y="44" width="84" height="34" rx="6"/>
<text class="d-label" x="452" y="65.5" text-anchor="middle">weights</text>
<rect class="d-box" x="500" y="44" width="134" height="34" rx="6"/>
<text class="d-label" x="567" y="65.5" text-anchor="middle">cu-tree</text>
<rect class="d-accent" x="640" y="44" width="60" height="34" rx="6"/>
<text class="d-label" x="670" y="65.5" text-anchor="middle">dispatch</text>
<text class="d-label" x="20" y="124" text-anchor="start">many threads: free workers take pieces of each step</text>
<rect class="d-fill" x="20" y="176" width="78" height="16" rx="3"/>
<rect class="d-fill" x="20" y="196" width="78" height="16" rx="3"/>
<rect class="d-fill" x="20" y="216" width="78" height="16" rx="3"/>
<rect class="d-box" x="20" y="136" width="78" height="34" rx="6"/>
<text class="d-label" x="59" y="157.5" text-anchor="middle">push</text>
<rect class="d-fill" x="104" y="176" width="60" height="16" rx="3"/>
<rect class="d-fill" x="104" y="196" width="60" height="16" rx="3"/>
<rect class="d-fill" x="104" y="216" width="60" height="16" rx="3"/>
<rect class="d-box" x="104" y="136" width="60" height="34" rx="6"/>
<text class="d-label" x="134" y="157.5" text-anchor="middle">census</text>
<rect class="d-fill" x="170" y="176" width="44" height="16" rx="3"/>
<rect class="d-fill" x="170" y="196" width="44" height="16" rx="3"/>
<rect class="d-fill" x="170" y="216" width="44" height="16" rx="3"/>
<rect class="d-box" x="170" y="136" width="44" height="34" rx="6"/>
<text class="d-label" x="192" y="157.5" text-anchor="middle">AQ</text>
<rect class="d-fill" x="220" y="176" width="48" height="16" rx="3"/>
<rect class="d-fill" x="220" y="196" width="48" height="16" rx="3"/>
<rect class="d-fill" x="220" y="216" width="48" height="16" rx="3"/>
<rect class="d-box" x="220" y="136" width="48" height="34" rx="6"/>
<text class="d-label" x="244" y="157.5" text-anchor="middle">dark</text>
<rect class="d-fill" x="274" y="176" width="68" height="16" rx="3"/>
<rect class="d-fill" x="274" y="196" width="68" height="16" rx="3"/>
<rect class="d-fill" x="274" y="216" width="68" height="16" rx="3"/>
<rect class="d-box" x="274" y="136" width="68" height="34" rx="6"/>
<text class="d-label" x="308" y="157.5" text-anchor="middle">weights</text>
<rect class="d-fill" x="348" y="176" width="74" height="16" rx="3"/>
<rect class="d-fill" x="348" y="196" width="74" height="16" rx="3"/>
<rect class="d-fill" x="348" y="216" width="74" height="16" rx="3"/>
<rect class="d-box" x="348" y="136" width="74" height="34" rx="6"/>
<text class="d-label" x="385" y="157.5" text-anchor="middle">cu-tree</text>
<rect class="d-accent" x="428" y="136" width="60" height="34" rx="6"/>
<text class="d-label" x="458" y="157.5" text-anchor="middle">dispatch</text>
<text class="d-label" x="504" y="158" text-anchor="start">dispatched sooner</text>
<text class="d-label" x="20" y="256" text-anchor="start">shaded bars: the same step's items, run by workers that had nothing else to do</text>
<line class="d-line d-muted" x1="18" y1="136" x2="18" y2="232"/>
</svg>
<figcaption>The split changes when the analysis finishes. Every sum is still added up in its old order, so the results are the same numbers.</figcaption>
</figure>

Above one thread the driver now cuts each of those steps into items and shares them with any worker that's free. The split isn't always a win. Because the hand-off cost more than it saved on small pictures, each step keeps a running time for both forms and uses the faster one.

## Input

Reading the source is work too. Above one thread a separate thread reads frames ahead into a small buffer. A slow disk or a slow decoder upstream then doesn't add its time to every frame. At `--threads 1` the read stays inline.

## What stays identical

At `--threads 1`, two runs of the same command write the same bytes. That's a promise. Every change has to pass a check of it before it merges.

Above one thread, identical output is checked and reported. It isn't promised. The design aims for it all the same. A task reads only values that are fixed by the input and the settings. The schedule decides when a task runs and never what it reads. Every round of checks compares streams across thread counts and across repeated runs. A mismatch is recorded as a warning with its cause.

Capped VBR is different by design. Above one thread its stream differs from the one-thread stream, for the reason in the next section.

## Rate control with frames in flight

Rate decisions are made in coding order on the driver, one frame at a time, before a frame enters the pool. The per-block offsets are fixed at that point too.

Fixed QP and CRF need nothing from earlier frames. Their frames overlap freely. A mode that targets a bitrate wants to know how many bits the earlier frames really took. Plain ABR waits for them. Its frames run one after another and only the rows inside a frame run in parallel.

Capped VBR doesn't wait. Above one thread it plans each frame from the actual sizes of frames up to eight back and from the planned sizes of the ones still in flight. Its speed is kept that way, at the price of a stream that differs from the one-thread stream. A buffer shorter than 1.2 seconds of maxrate is the exception. Because frames in flight overdrew buffers that shallow, the encoder waits there at every thread count and writes the one-thread stream.

## Segment-parallel encoding

`--gop-parallel <n>` is a different way to use cores. It cuts a long input at key frame periods and codes the segments on several encoder instances at once, dividing the threads between them. The segments are joined at IDR pictures into one stream.

It's off by default. The joined stream differs from the serial one and has no buffer timing messages. It can't be combined with two-pass, zones or a QP file, among others that `--help` lists.

## Setting it yourself

| setting | what it does |
|---|---|
| `--threads <n>` | size of the worker pool. 0, the default, is one per CPU |
| `--threads 1` | one worker, inline reads, byte-identical run to run |
| `--gop-parallel <n>` | code segments on `n` encoder instances at once |
| `N265_READAHEAD_MB=0` | turn off the reader thread |
| `N265_DRV_PAR=0` | keep the driver's analysis on the driver |

The two environment settings move no bit of the output. They exist for timing one schedule against the other. The [encoding guide](guide.md#threads-and-memory) has the advice on which to use.

## Limits

The driver still paces a 4K encode on a machine with many cores. Part of its work is a chain in which each frame's analysis reads the previous frame's result. That part stays serial.

The chart shows how much each encoder gains from the Mac's cores. It compares the same short clips in plain C on one thread and on all cores.

<figure class="chart">
<img src="assets/charts/internals/threads.svg" alt="Bar chart of one-thread wall time over all-core wall time in plain C on an Apple M5 Max, per picture size, for yah265 and x265. The table below has the values.">
<figcaption>Short clips on the Mac with SIMD off and <code>medium</code> on each side. Because the Mac's cores aren't all the same speed, the ceiling is below its core count.</figcaption>
</figure>

<!--gen:threads-table-->
| class | yah265 | x265 | clips |
|---|--:|--:|--:|
| CIF | 4.1x | 2.9x | 3 |
| 720p | 7.5x | 6.9x | 4 |
| 1080p | 10.6x | 9.7x | 3 |
| 4K | 5.8x | 8.4x | 1 |
<!--/gen:threads-table-->

Up to 1080p yah265 gains more from the cores than x265 does. At 4K it gains less. These rows were timed before the driver's analysis was split over the pool.

[Results](results.md#speed) has the wall-time comparisons with x265 at one thread and on all cores.
