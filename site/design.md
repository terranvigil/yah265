---
title: Design - yah265
description: How yah265 makes the choices the HEVC standard leaves open, from the lookahead to the checks a change passes.
---

# Design

The HEVC standard defines a legal stream and leaves the encoding to the encoder. This page covers how yah265 makes those choices. See [The HEVC standard](hevc.md) for the terms used here.

## Search order comes first

HEVC moved most of an encoder's cost into search. A coding tree can split from 64x64 down to 8x8. Each node offers merge, motion search, 35 intra modes and a transform tree of its own. The pixel work per decoded sample stayed close to H.264's. So the first thing I designed was the order candidates are tried in and how early the losers are dropped.

## The pipeline

A frame passes through four stages:

1. A reader thread takes it off the input.
2. The lookahead works out from a small copy what the frame will cost.
3. One thread, called the driver here, decides the frame's type, its references and its planned quantizer.
4. A shared pool of workers codes the frame as rows of coding tree units while rows of other frames run beside it. A last pass per frame turns the rows' recorded decisions into bits.

<figure class="diagram">
<svg class="d-wide" viewBox="0 0 720 298" role="img" aria-label="The path a frame takes through yah265">
<title>The path a frame takes through yah265</title>
<desc>A reader thread hands frames to the lookahead. The lookahead keeps a small copy of each and estimates costs and motion. Frame decisions set each frame's type, QP plan and references and dispatch it to a shared worker pool in coding order. In the pool a frame's rows are encoded, deblocked, run through SAO and published. One entropy pass per frame writes the stream. Published rows feed later frames' prediction. The bits each frame used feed back to the frame decisions.</desc>
<rect class="d-box" x="20" y="30" width="140" height="44" rx="6"/>
<text class="d-label" x="90" y="48.5" text-anchor="middle">Reader thread</text>
<text class="d-label" x="90" y="64.5" text-anchor="middle">frames off the pipe</text>
<line class="d-arrow" x1="160" y1="52" x2="198" y2="52"/>
<rect class="d-box" x="200" y="30" width="190" height="44" rx="6"/>
<text class="d-label" x="295" y="48.5" text-anchor="middle">Lookahead</text>
<text class="d-label" x="295" y="64.5" text-anchor="middle">small copy, costs, motion</text>
<line class="d-arrow" x1="390" y1="52" x2="428" y2="52"/>
<rect class="d-accent" x="430" y="30" width="180" height="44" rx="6"/>
<text class="d-label" x="520" y="48.5" text-anchor="middle">Frame decisions</text>
<text class="d-label" x="520" y="64.5" text-anchor="middle">type, QP plan, references</text>
<rect class="d-dash" x="20" y="122" width="680" height="116" rx="8"/>
<text class="d-label" x="120" y="142" text-anchor="start">worker pool: rows of several frames at once</text>
<path class="d-arrow" d="M470 74 L470 98 L100 98 L100 158"/>
<text class="d-label" x="285" y="91" text-anchor="middle">dispatch, in coding order</text>
<rect class="d-box" x="40" y="160" width="120" height="40" rx="6"/>
<text class="d-label" x="100" y="184.5" text-anchor="middle">Encode rows</text>
<line class="d-arrow" x1="160" y1="180" x2="198" y2="180"/>
<rect class="d-box" x="200" y="160" width="120" height="40" rx="6"/>
<text class="d-label" x="260" y="184.5" text-anchor="middle">Deblock rows</text>
<line class="d-arrow" x1="320" y1="180" x2="358" y2="180"/>
<rect class="d-box" x="360" y="160" width="150" height="40" rx="6"/>
<text class="d-label" x="435" y="176.5" text-anchor="middle">SAO rows,</text>
<text class="d-label" x="435" y="192.5" text-anchor="middle">then publish</text>
<line class="d-arrow" x1="510" y1="180" x2="548" y2="180"/>
<rect class="d-box" x="550" y="160" width="130" height="40" rx="6"/>
<text class="d-label" x="615" y="176.5" text-anchor="middle">Entropy pass,</text>
<text class="d-label" x="615" y="192.5" text-anchor="middle">one per frame</text>
<path class="d-arrow d-muted" d="M435 200 L435 224 L100 224 L100 202"/>
<text class="d-label" x="268" y="218" text-anchor="middle">published rows are what later frames predict from</text>
<path class="d-arrow d-muted" d="M650 160 L650 52 L612 52"/>
<text class="d-label" x="658" y="100" text-anchor="start">bits</text>
<text class="d-label" x="658" y="116" text-anchor="start">used</text>
<line class="d-arrow" x1="615" y1="200" x2="615" y2="268"/>
<text class="d-label" x="615" y="286" text-anchor="middle">stream, in coding order</text>
</svg>
<svg class="d-narrow" viewBox="0 0 360 546" role="img" aria-labelledby="design-path-n-t design-path-n-d">
<title id="design-path-n-t">The path a frame takes through yah265</title>
<desc id="design-path-n-d">A reader thread hands frames to the lookahead. The lookahead keeps a small copy of each and estimates costs and motion. Frame decisions set each frame's type, QP plan and references and dispatch it to a shared worker pool in coding order. In the pool a frame's rows are encoded, deblocked, run through SAO and published. One entropy pass per frame writes the stream. Published rows feed later frames' prediction. The bits each frame used feed back to the frame decisions.</desc>
<rect class="d-box" x="64" y="8" width="186" height="44" rx="6"/>
<text class="d-label" x="157" y="26.5" text-anchor="middle">Reader thread</text>
<text class="d-label" x="157" y="42.5" text-anchor="middle">frames off the pipe</text>
<line class="d-arrow" x1="157" y1="52" x2="157" y2="74"/>
<rect class="d-box" x="64" y="76" width="186" height="44" rx="6"/>
<text class="d-label" x="157" y="94.5" text-anchor="middle">Lookahead</text>
<text class="d-label" x="157" y="110.5" text-anchor="middle">small copy, costs, motion</text>
<line class="d-arrow" x1="157" y1="120" x2="157" y2="142"/>
<rect class="d-accent" x="64" y="144" width="186" height="44" rx="6"/>
<text class="d-label" x="157" y="162.5" text-anchor="middle">Frame decisions</text>
<text class="d-label" x="157" y="178.5" text-anchor="middle">type, QP plan, references</text>
<line class="d-arrow" x1="157" y1="188" x2="157" y2="266"/>
<text class="d-label" x="165" y="214" text-anchor="start">dispatch, in coding order</text>
<rect class="d-dash" x="48" y="228" width="306" height="260" rx="8"/>
<text class="d-label" x="170" y="250" text-anchor="start">worker pool: rows of</text>
<text class="d-label" x="170" y="266" text-anchor="start">several frames at once</text>
<rect class="d-box" x="64" y="268" width="186" height="36" rx="6"/>
<text class="d-label" x="157" y="290.5" text-anchor="middle">Encode rows</text>
<line class="d-arrow" x1="157" y1="304" x2="157" y2="322"/>
<rect class="d-box" x="64" y="324" width="186" height="36" rx="6"/>
<text class="d-label" x="157" y="346.5" text-anchor="middle">Deblock rows</text>
<line class="d-arrow" x1="157" y1="360" x2="157" y2="378"/>
<rect class="d-box" x="64" y="380" width="186" height="36" rx="6"/>
<text class="d-label" x="157" y="402.5" text-anchor="middle">SAO rows, then publish</text>
<line class="d-arrow" x1="157" y1="416" x2="157" y2="434"/>
<rect class="d-box" x="64" y="436" width="186" height="40" rx="6"/>
<text class="d-label" x="157" y="452.5" text-anchor="middle">Entropy pass,</text>
<text class="d-label" x="157" y="468.5" text-anchor="middle">one per frame</text>
<path class="d-arrow d-muted" d="M250 398 L266 398 L266 286 L252 286"/>
<text class="d-label" x="274" y="304" text-anchor="start">published</text>
<text class="d-label" x="274" y="320" text-anchor="start">rows are</text>
<text class="d-label" x="274" y="336" text-anchor="start">what later</text>
<text class="d-label" x="274" y="352" text-anchor="start">frames</text>
<text class="d-label" x="274" y="368" text-anchor="start">predict from</text>
<path class="d-arrow d-muted" d="M64 456 L40 456 L40 166 L62 166"/>
<text class="d-label" x="6" y="300" text-anchor="start">bits</text>
<text class="d-label" x="6" y="316" text-anchor="start">used</text>
<line class="d-arrow" x1="157" y1="476" x2="157" y2="514"/>
<text class="d-label" x="157" y="534" text-anchor="middle">stream, in coding order</text>
</svg>
<figcaption>One thread makes the per-frame decisions. Everything inside the dashed box runs on the worker pool.</figcaption>
</figure>

The row is the unit of work. A row never waits for a whole frame. See [Threading](threading.md) for what it does wait for and how many frames are in flight.

The hot loops have vector versions for [arm64 and x86-64](architectures.md) that the encoder picks at run time. Each one reproduces its C reference byte for byte.

## Validating the bitstream

A stream is only useful when any decoder rebuilds the pictures the encoder believes it wrote. The encoder predicts each block from its own rebuilt pictures. If a decoder's picture differs by one sample, the blocks predicted from that sample inherit the error. The error then spreads from picture to picture until the next key picture.

So the test is exact equality. Each conformance encode goes through two independent decoders, ffmpeg and libde265. Each decoder's output has to match, sample for sample, the pictures the encoder writes with `--dump-recon`. I use two because a mistake that one decoder tolerates may fail in the other. See [Check it yourself](check-it-yourself.md) to run the same comparison on your own encode.

## Lookahead

A frame passes through a lookahead queue before it's coded. The queue keeps a half-resolution copy of each frame, estimates costs on small blocks and remembers the motion search between each pair of frames. That one search feeds everything that needs motion before the real encode. It decides frame types, scene cuts and how many B-frames to run. It also drives weighted prediction and the [propagation step](#rate-control) of rate control.

The lookahead calls a scene cut only where the cost jumps. The key frame period restarts at a cut so that a periodic key frame doesn't follow right after one.

The lookahead also recognizes [two kinds of content](results.md#content-types) that need different handling. A scroll in a screen recording defeats a search that expects small motion. So on screen content the encoder measures the scroll first and gives it to the motion search. Handheld footage of fine texture jitters from frame to frame. There the encoder spends more effort on each decision.

I [swept the lookahead's depth](results.md#lookahead-depth) on the test set to pick [the depths `medium` runs](#what-medium-does-at-each-size).

## Mode decision

Each coding tree unit runs a staged decision. Merge and skip candidates go first. A block that's already coded well enough leaves there. A block size that survives gets one motion search for the whole block before a cheap Hadamard cost ranks the candidates. Only the winner pays for a full rate-distortion trial.

<figure class="diagram">
<svg class="d-wide" viewBox="0 0 720 294" role="img" aria-label="The decision ladder for one block">
<title>The decision ladder for one block</title>
<desc>A funnel in three stages. Merge candidates, one motion search per block size and the intra modes enter at the top. Stage one tries merge and skip. A block that is already coded well leaves there as a skip. Stage two ranks the survivors by a cheap Hadamard cost and drops the losers. Stage three gives one candidate a full rate-distortion trial. The winner goes into the frame's records.</desc>
<polygon class="d-fill" points="115,40 545,40 500,96 160,96"/>
<polygon class="d-fill" points="160,106 500,106 450,162 210,162"/>
<polygon class="d-accent" points="210,172 450,172 415,228 245,228"/>
<text class="d-label" x="330" y="64" text-anchor="middle">1. Try merge and skip first</text>
<text class="d-label" x="330" y="82" text-anchor="middle">a block that's already coded well stops here</text>
<text class="d-label" x="330" y="130" text-anchor="middle">2. Rank the rest by Hadamard cost</text>
<text class="d-label" x="330" y="148" text-anchor="middle">cheap, no entropy coding</text>
<text class="d-label" x="330" y="196" text-anchor="middle">3. One full trial</text>
<text class="d-label" x="330" y="214" text-anchor="middle">real distortion, real bits</text>
<text class="d-label" x="330" y="26" text-anchor="middle">merge candidates, one motion search per block size, intra modes</text>
<line class="d-arrow" x1="545" y1="68" x2="600" y2="68"/>
<text class="d-label" x="606" y="64" text-anchor="start">early exit</text>
<text class="d-label" x="606" y="80" text-anchor="start">as a skip</text>
<line class="d-arrow d-muted" x1="498" y1="134" x2="600" y2="134"/>
<text class="d-label" x="606" y="138" text-anchor="start">losers dropped</text>
<line class="d-arrow" x1="330" y1="228" x2="330" y2="262"/>
<text class="d-label" x="330" y="280" text-anchor="middle">the winner goes into the frame's records</text>
<text class="d-label" x="30" y="72" text-anchor="start">all</text>
<text class="d-label" x="30" y="88" text-anchor="start">candidates</text>
<text class="d-label" x="60" y="138" text-anchor="start">survivors</text>
<text class="d-label" x="130" y="204" text-anchor="start">one</text>
</svg>
<svg class="d-narrow" viewBox="0 0 360 404" role="img" aria-labelledby="design-ladder-n-t design-ladder-n-d">
<title id="design-ladder-n-t">The decision ladder for one block</title>
<desc id="design-ladder-n-d">A funnel in three stages. Merge candidates, one motion search per block size and the intra modes enter at the top. Stage one tries merge and skip. A block that is already coded well leaves there as a skip. Stage two ranks the survivors by a cheap Hadamard cost and drops the losers. Stage three gives one candidate a full rate-distortion trial. The winner goes into the frame's records.</desc>
<text class="d-label" x="180" y="20" text-anchor="middle">all candidates: merge candidates,</text>
<text class="d-label" x="180" y="38" text-anchor="middle">one motion search per block size, intra modes</text>
<line class="d-arrow" x1="180" y1="46" x2="180" y2="64"/>
<polygon class="d-fill" points="8,66 352,66 322,134 38,134"/>
<text class="d-label" x="180" y="90" text-anchor="middle">1. Try merge and skip first</text>
<text class="d-label" x="180" y="108" text-anchor="middle">a block that's already coded well</text>
<text class="d-label" x="180" y="126" text-anchor="middle">stops here</text>
<line class="d-arrow" x1="120" y1="134" x2="120" y2="174"/>
<text class="d-label" x="112" y="160" text-anchor="end">survivors</text>
<line class="d-arrow" x1="300" y1="134" x2="300" y2="150"/>
<text class="d-label" x="352" y="168" text-anchor="end">early exit as a skip</text>
<polygon class="d-fill" points="38,176 322,176 292,236 68,236"/>
<text class="d-label" x="180" y="200" text-anchor="middle">2. Rank the rest by Hadamard cost</text>
<text class="d-label" x="180" y="220" text-anchor="middle">cheap, no entropy coding</text>
<line class="d-arrow" x1="120" y1="236" x2="120" y2="276"/>
<text class="d-label" x="112" y="262" text-anchor="end">one</text>
<line class="d-arrow d-muted" x1="280" y1="236" x2="280" y2="252"/>
<text class="d-label" x="352" y="270" text-anchor="end">losers dropped</text>
<polygon class="d-accent" points="60,278 300,278 270,340 90,340"/>
<text class="d-label" x="180" y="304" text-anchor="middle">3. One full trial</text>
<text class="d-label" x="180" y="324" text-anchor="middle">real distortion, real bits</text>
<line class="d-arrow" x1="180" y1="340" x2="180" y2="372"/>
<text class="d-label" x="180" y="392" text-anchor="middle">the winner goes into the frame's records</text>
</svg>
<figcaption>Each stage sees fewer candidates than the one above it, at a higher cost for each.</figcaption>
</figure>

Pruning decides the speed. A node stops splitting when its neighbors and its own costs say a split won't pay. On an intra-heavy frame it also stops under a block that intra already won. On wide pictures the intra modes of P and B pictures are ranked coarse to fine so that only 19 of the 35 get priced.

`medium` leaves out the rectangular and uneven partitions, along with 4x4 intra in P and B pictures. The slower presets turn those back on.

`medium` codes 32x32 trees below 1080p and 64x64 trees from there up. The 64x64 trees have two gates on the top node.

<figure class="diagram">
<svg class="d-wide" viewBox="0 0 720 260" role="img" aria-label="The two gates on a 64x64 node">
<title>The two gates on a 64x64 node</title>
<desc>A 64 by 64 node first goes to gate one, which tries the whole block as a single skip. If little residual is left the skip is kept and the node is done. Otherwise gate two asks whether the skip missed by a wide margin. If it did, the node goes straight to four 32 by 32 nodes. If it didn't, the node gets a full decision at 64 by 64 and the split is tried as well.</desc>
<rect class="d-box" x="20" y="100" width="90" height="44" rx="6"/>
<text class="d-label" x="65" y="118.5" text-anchor="middle">64x64</text>
<text class="d-label" x="65" y="134.5" text-anchor="middle">node</text>
<line class="d-arrow" x1="110" y1="122" x2="148" y2="122"/>
<rect class="d-accent" x="150" y="96" width="190" height="52" rx="6"/>
<text class="d-label" x="245" y="118.5" text-anchor="middle">Gate 1: try the whole</text>
<text class="d-label" x="245" y="134.5" text-anchor="middle">block as one skip</text>
<line class="d-arrow" x1="340" y1="122" x2="448" y2="122"/>
<text class="d-label" x="394" y="112" text-anchor="middle">too much left</text>
<rect class="d-accent" x="450" y="96" width="190" height="52" rx="6"/>
<text class="d-label" x="545" y="118.5" text-anchor="middle">Gate 2: did the skip</text>
<text class="d-label" x="545" y="134.5" text-anchor="middle">miss by a wide margin?</text>
<line class="d-arrow" x1="245" y1="148" x2="245" y2="198"/>
<text class="d-label" x="253" y="178" text-anchor="start">little residual left</text>
<rect class="d-box" x="150" y="200" width="190" height="40" rx="6"/>
<text class="d-label" x="245" y="224.5" text-anchor="middle">Keep the skip. Done.</text>
<line class="d-arrow" x1="545" y1="148" x2="545" y2="198"/>
<text class="d-label" x="553" y="178" text-anchor="start">yes</text>
<rect class="d-box" x="450" y="200" width="190" height="40" rx="6"/>
<text class="d-label" x="545" y="224.5" text-anchor="middle">Go straight to four 32x32</text>
<line class="d-arrow" x1="545" y1="96" x2="545" y2="62"/>
<text class="d-label" x="553" y="82" text-anchor="start">no</text>
<rect class="d-box" x="410" y="20" width="270" height="40" rx="6"/>
<text class="d-label" x="545" y="44.5" text-anchor="middle">Full decision at 64x64, then try the split</text>
</svg>
<svg class="d-narrow" viewBox="0 0 360 404" role="img" aria-labelledby="design-gates-n-t design-gates-n-d">
<title id="design-gates-n-t">The two gates on a 64x64 node</title>
<desc id="design-gates-n-d">A 64 by 64 node first goes to gate one, which tries the whole block as a single skip. If little residual is left the skip is kept and the node is done. Otherwise gate two asks whether the skip missed by a wide margin. If it did, the node goes straight to four 32 by 32 nodes. If it didn't, the node gets a full decision at 64 by 64 and the split is tried as well.</desc>
<rect class="d-box" x="120" y="8" width="120" height="36" rx="6"/>
<text class="d-label" x="180" y="30.5" text-anchor="middle">64x64 node</text>
<line class="d-arrow" x1="180" y1="44" x2="180" y2="62"/>
<rect class="d-accent" x="30" y="64" width="300" height="40" rx="6"/>
<text class="d-label" x="180" y="88.5" text-anchor="middle">Gate 1: try the whole block as one skip</text>
<line class="d-arrow" x1="80" y1="104" x2="80" y2="148"/>
<text class="d-label" x="88" y="126" text-anchor="start">little residual left</text>
<rect class="d-box" x="8" y="150" width="164" height="36" rx="6"/>
<text class="d-label" x="90" y="172.5" text-anchor="middle">Keep the skip. Done.</text>
<line class="d-arrow" x1="280" y1="104" x2="280" y2="208"/>
<text class="d-label" x="272" y="174" text-anchor="end">too much left</text>
<rect class="d-accent" x="150" y="210" width="202" height="44" rx="6"/>
<text class="d-label" x="251" y="228.5" text-anchor="middle">Gate 2: did the skip</text>
<text class="d-label" x="251" y="244.5" text-anchor="middle">miss by a wide margin?</text>
<line class="d-arrow" x1="251" y1="254" x2="251" y2="294"/>
<text class="d-label" x="259" y="278" text-anchor="start">yes</text>
<rect class="d-box" x="120" y="296" width="232" height="36" rx="6"/>
<text class="d-label" x="236" y="318.5" text-anchor="middle">Go straight to four 32x32</text>
<path class="d-arrow" d="M150 232 L30 232 L30 358"/>
<text class="d-label" x="38" y="280" text-anchor="start">no</text>
<rect class="d-box" x="8" y="360" width="344" height="36" rx="6"/>
<text class="d-label" x="180" y="382.5" text-anchor="middle">Full decision at 64x64, then try the split</text>
</svg>
<figcaption>Both gates save work at the top of the tree, where a wrong guess is the most expensive.</figcaption>
</figure>

Both gates ramp in with the planned quantizer so that the file size moves smoothly as the CRF changes. At 1080p the first gate also checks each 32x32 quarter of the block, because residual in one corner can hide in the block's total.

## Motion search

Motion search finds where in the reference picture each block best comes from. Each candidate vector is charged for the bits it would cost to write. A slightly worse match that's cheap to code can win.

The search starts from the vectors the block's neighbors used, since those are the ones the decoder can nearly guess. From the best of them it walks whole-sample positions, then refines to half and quarter samples. Small pictures walk a unit diamond. From 720p up the walk is a hexagon that finishes with the diamond, because on large pictures with fine texture the diamond alone can stop at a poor match.

At 4K each list has a second reference picture. Because a full search of it would double the work, the second reference gets a short search around a few likely vectors.

I didn't restrict the search to suit the threading. A vector may point anywhere the standard allows. The schedule [waits for the reference rows](threading.md#what-a-row-waits-for) the vector needs.

## Transforms and quantization

The transforms, intra prediction, the interpolation filters, deblocking and SAO are the specification's. The freedom is in the rounding. Rate-distortion optimized quantization (RDOQ) asks of each coefficient whether rounding it down saves more bits than it costs in distortion. yah265's version runs in one pass. `medium` runs it once per coded block in the final encode while `slow` and `slower` run it in all decisions.

## Entropy coding

One CABAC engine drives both the real coder and a fractional-bit estimator. The estimator prices coefficient decisions during the search.

Each picture is coded as one slice with no wavefront substreams and no tiles. Those tools let decoders and encoders work in parallel at a cost in bitrate. Because yah265 gets its parallelism from the schedule, the stream pays nothing for it.

## Rate control

Rate control turns what you asked for into a quantizer for each block. See the [encoding guide](guide.md#pick-a-rate-mode) for when to use each rate mode.

<figure class="diagram">
<svg class="d-wide" viewBox="0 0 720 290" role="img" aria-label="Which planner each rate mode uses">
<title>Which planner each rate mode uses</title>
<desc>Seven rate modes on the left feed three planners. Fixed QP uses the quantizer you gave. CRF, capped CRF and capped VBR use the quality planner, where a base QP comes from the lookahead's cost. ABR, CBR and the second pass of two-pass use the rate loop, where a QP comes from the bits spent so far. Capped CRF, capped VBR and CBR then pass through the buffer model. All paths end at the per-block offsets that set each block's QP.</desc>
<rect class="d-box" x="20" y="30" width="150" height="28" rx="6"/>
<text class="d-label" x="95" y="48.5" text-anchor="middle">fixed QP</text>
<line class="d-arrow d-muted" x1="170" y1="44" x2="228" y2="44"/>
<rect class="d-box" x="20" y="66" width="150" height="28" rx="6"/>
<text class="d-label" x="95" y="84.5" text-anchor="middle">CRF</text>
<line class="d-arrow d-muted" x1="170" y1="80" x2="228" y2="108"/>
<rect class="d-fill" x="20" y="102" width="150" height="28" rx="6"/>
<text class="d-label" x="95" y="120.5" text-anchor="middle">capped CRF</text>
<line class="d-arrow d-muted" x1="170" y1="116" x2="228" y2="121"/>
<rect class="d-fill" x="20" y="138" width="150" height="28" rx="6"/>
<text class="d-label" x="95" y="156.5" text-anchor="middle">capped VBR</text>
<line class="d-arrow d-muted" x1="170" y1="152" x2="228" y2="134"/>
<rect class="d-box" x="20" y="174" width="150" height="28" rx="6"/>
<text class="d-label" x="95" y="192.5" text-anchor="middle">ABR</text>
<line class="d-arrow d-muted" x1="170" y1="188" x2="228" y2="208"/>
<rect class="d-fill" x="20" y="210" width="150" height="28" rx="6"/>
<text class="d-label" x="95" y="228.5" text-anchor="middle">CBR</text>
<line class="d-arrow d-muted" x1="170" y1="224" x2="228" y2="221"/>
<rect class="d-box" x="20" y="246" width="150" height="28" rx="6"/>
<text class="d-label" x="95" y="264.5" text-anchor="middle">two-pass, second pass</text>
<line class="d-arrow d-muted" x1="170" y1="260" x2="228" y2="234"/>
<rect class="d-box" x="230" y="26" width="200" height="36" rx="6"/>
<text class="d-label" x="330" y="48.5" text-anchor="middle">The quantizer you gave</text>
<rect class="d-accent" x="230" y="94" width="200" height="54" rx="6"/>
<text class="d-label" x="330" y="117.5" text-anchor="middle">Quality planner: a base QP</text>
<text class="d-label" x="330" y="133.5" text-anchor="middle">from the lookahead's cost</text>
<rect class="d-box" x="230" y="194" width="200" height="54" rx="6"/>
<text class="d-label" x="330" y="217.5" text-anchor="middle">Rate loop: a QP from</text>
<text class="d-label" x="330" y="233.5" text-anchor="middle">the bits spent so far</text>
<rect class="d-fill" x="470" y="146" width="100" height="56" rx="6"/>
<text class="d-label" x="520" y="170.5" text-anchor="middle">Buffer</text>
<text class="d-label" x="520" y="186.5" text-anchor="middle">model</text>
<line class="d-arrow" x1="430" y1="132" x2="468" y2="160"/>
<line class="d-arrow" x1="430" y1="210" x2="468" y2="188"/>
<line class="d-arrow" x1="570" y1="174" x2="608" y2="174"/>
<line class="d-arrow" x1="430" y1="44" x2="608" y2="44"/>
<line class="d-arrow" x1="430" y1="108" x2="608" y2="108"/>
<line class="d-arrow" x1="430" y1="236" x2="608" y2="236"/>
<rect class="d-box" x="610" y="26" width="96" height="222" rx="6"/>
<text class="d-label" x="658" y="110" text-anchor="middle">Per-block</text>
<text class="d-label" x="658" y="127" text-anchor="middle">offsets,</text>
<text class="d-label" x="658" y="144" text-anchor="middle">then each</text>
<text class="d-label" x="658" y="161" text-anchor="middle">block's QP</text>
<text class="d-label" x="520" y="272" text-anchor="middle">shaded modes pass through the buffer model</text>
</svg>
<svg class="d-narrow" viewBox="0 0 360 444" role="img" aria-labelledby="design-rate-n-t design-rate-n-d">
<title id="design-rate-n-t">Which planner each rate mode uses</title>
<desc id="design-rate-n-d">Seven rate modes on the left feed three planners. Fixed QP uses the quantizer you gave. CRF, capped CRF and capped VBR use the quality planner, where a base QP comes from the lookahead's cost. ABR, CBR and the second pass of two-pass use the rate loop, where a QP comes from the bits spent so far. Capped CRF, capped VBR and CBR then pass through the buffer model. All paths end at the per-block offsets that set each block's QP.</desc>
<rect class="d-box" x="8" y="8" width="112" height="28" rx="6"/>
<text class="d-label" x="64" y="26.5" text-anchor="middle">fixed QP</text>
<line class="d-arrow d-muted" x1="120" y1="22" x2="148" y2="22"/>
<rect class="d-box" x="150" y="4" width="168" height="36" rx="6"/>
<text class="d-label" x="234" y="26.5" text-anchor="middle">The quantizer you gave</text>
<rect class="d-box" x="8" y="58" width="112" height="28" rx="6"/>
<text class="d-label" x="64" y="76.5" text-anchor="middle">CRF</text>
<rect class="d-fill" x="8" y="94" width="112" height="28" rx="6"/>
<text class="d-label" x="64" y="112.5" text-anchor="middle">capped CRF</text>
<rect class="d-fill" x="8" y="130" width="112" height="28" rx="6"/>
<text class="d-label" x="64" y="148.5" text-anchor="middle">capped VBR</text>
<line class="d-arrow d-muted" x1="120" y1="72" x2="148" y2="92"/>
<line class="d-arrow d-muted" x1="120" y1="108" x2="148" y2="106"/>
<line class="d-arrow d-muted" x1="120" y1="144" x2="148" y2="120"/>
<rect class="d-accent" x="150" y="62" width="168" height="88" rx="6"/>
<text class="d-label" x="234" y="90" text-anchor="middle">Quality planner:</text>
<text class="d-label" x="234" y="107" text-anchor="middle">a base QP from the</text>
<text class="d-label" x="234" y="124" text-anchor="middle">lookahead's cost</text>
<rect class="d-box" x="8" y="176" width="112" height="28" rx="6"/>
<text class="d-label" x="64" y="194.5" text-anchor="middle">ABR</text>
<rect class="d-fill" x="8" y="212" width="112" height="28" rx="6"/>
<text class="d-label" x="64" y="230.5" text-anchor="middle">CBR</text>
<rect class="d-box" x="8" y="248" width="112" height="40" rx="6"/>
<text class="d-label" x="64" y="264.5" text-anchor="middle">two-pass,</text>
<text class="d-label" x="64" y="280.5" text-anchor="middle">second pass</text>
<line class="d-arrow d-muted" x1="120" y1="190" x2="148" y2="212"/>
<line class="d-arrow d-muted" x1="120" y1="226" x2="148" y2="224"/>
<line class="d-arrow d-muted" x1="120" y1="268" x2="148" y2="238"/>
<rect class="d-box" x="150" y="196" width="168" height="56" rx="6"/>
<text class="d-label" x="234" y="220.5" text-anchor="middle">Rate loop: a QP from</text>
<text class="d-label" x="234" y="236.5" text-anchor="middle">the bits spent so far</text>
<rect class="d-fill" x="200" y="300" width="100" height="40" rx="6"/>
<text class="d-label" x="250" y="324.5" text-anchor="middle">Buffer model</text>
<path class="d-arrow" d="M318 22 L346 22 L346 370"/>
<path class="d-arrow" d="M318 106 L332 106 L332 370"/>
<line class="d-arrow" x1="332" y1="320" x2="302" y2="320"/>
<line class="d-arrow" x1="250" y1="252" x2="250" y2="298"/>
<line class="d-arrow" x1="170" y1="252" x2="170" y2="370"/>
<line class="d-arrow" x1="250" y1="340" x2="250" y2="370"/>
<rect class="d-box" x="8" y="372" width="346" height="36" rx="6"/>
<text class="d-label" x="180" y="394.5" text-anchor="middle">Per-block offsets, then each block's QP</text>
<text class="d-label" x="180" y="432" text-anchor="middle">shaded modes pass through the buffer model</text>
</svg>
<figcaption>Capped VBR uses the quality planner, as CRF does. The buffer model only sees the modes that have a buffer.</figcaption>
</figure>

CRF plans a fractional base QP for each frame from the lookahead's cost estimate. Two per-block adjustments follow in all modes. Adaptive quantization shifts bits toward blocks where the eye sees errors. The lookahead's propagation step lowers the QP on blocks that later frames predict from.

Chasing a bitrate frame by frame makes quality swing. So capped VBR plans by quality, like CRF, and corrects toward its target slowly.

A buffer adds a hard limit that planning alone can't guarantee. While a picture is being coded, the encoder coarsens it if it's about to overrun the buffer. As a last resort it codes blocks as skips.

With [several frames in flight](threading.md#rate-control-with-frames-in-flight) the rate loop can't see all the earlier frames' sizes in time. See [VBR](results.md#vbr) for the rate-tracking and speed results.

## References

One engine handles the reference picture sets and the decoded picture buffer. Because the encoder builds its reference lists the way a decoder does, the two can't disagree. B-frames are coded as a pyramid. Open GOP is the default and periodic key frames are CRA pictures.

## What medium does at each size

The results on this site were measured at `medium`, the default preset. It changes with the picture, because a choice that pays at 4K can cost quality at CIF.

| picture | coding tree | lookahead | whole-sample search | references per list | chroma QP offset | dark psy strength |
|---|---|---|---|--:|--:|--:|
| CIF | 32x32 | 12 frames | diamond | 1 | -1 | off |
| 720p | 32x32 | 20 frames | hexagon | 1 | -2 | 2 |
| 1080p | 64x64 | 20 frames | hexagon | 1 | -1 | 4 |
| 4K | 64x64 | 20 frames | hexagon | 2 | -1 | 4 |

The values are what `--show-config` prints for an 8-bit SDR input at each size. A picture between two sizes takes the row its area falls in. HDR input [changes the last two columns](hdr.md).

## Tuning for VMAF v1

The quality results on this site are scored with VMAF v1. Several of `medium`'s choices come from what that metric measures.

- VMAF v1 reads banding. Without a psy term the rate-distortion decision codes faint grain in dark areas to flat steps. From 720p up the psy strength therefore rises on dark, smooth 16x16 blocks.
- It scores chroma. It also scores a small picture scaled up to display size, where the chroma loss shows. So inter chroma coefficients round less harshly and both chroma quantizers run finer than luma's.
- Adaptive quantization scales its strength frame by frame.

See [Quality at the same size](results.md#quality-at-the-same-size) and [Quality across the rate range](results.md#quality-across-the-rate-range) for the results.

## Checks every change passes

- Each conformance encode is decoded by two decoders that [must match the encoder's own pictures](#validating-the-bitstream).
- Two runs at `--threads 1` must give the same bytes. Across thread counts a mismatch is reported as a warning.
- A tool called checkasm tests each SIMD kernel against its C reference.
- A change that alters the output gets a setting that turns it off. With it off, the output must match the previous commit byte for byte.
- A change that loses quality or speed doesn't ship.

## Written from the specification

I wrote the encoder from the ITU-T H.265 specification and took the algorithms from published papers. Two reference decoders check the streams. The command line uses x265's option names for the features both encoders share. See the [encoding guide](guide.md#left-out-on-purpose) for the options yah265 refuses and the [contribution rules](CONTRIBUTING.md) for the full policy.
