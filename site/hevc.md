---
title: The HEVC standard - yah265
description: What each tool in the H.265 standard does and which choices the standard leaves to an encoder.
---

# The HEVC standard

HEVC is the video coding standard that ITU-T publishes as H.265. This page walks through what it specifies, one tool at a time, for a reader who has never opened the specification. It starts with the way any video encoder works, because HEVC keeps that plan. The last section lists the choices the standard leaves to an encoder. [Design](design.md) covers how yah265 makes them.

## How video encoding works

Raw video is far too large to store or send. An encoder shrinks it by sending only what a decoder can't guess for itself.

The guess is called a prediction. The encoder cuts each picture into blocks and predicts every block from samples the decoder already has, taken either from the same picture (intra prediction) or from an earlier decoded picture (inter prediction). It then subtracts the prediction from the real block. The leftover is called the residual. The better the prediction, the smaller it is.

The residual then goes through a transform that turns a block of samples into a block of frequencies. Most of the block's energy ends up in a few of them. Quantization divides each frequency by a step size and rounds the result. Because that rounding is the only step that loses picture detail, the step size is the quality control. A number called the quantization parameter (QP) sets it. Last, an entropy coder writes the prediction choices and the rounded values in as few bits as it can.

The encoder also does the decoder's half of the work. It rebuilds each block from the rounded values exactly as a decoder will and predicts later blocks from the rebuilt picture. Both sides then predict from the same samples.

<figure class="diagram">
<svg class="d-wide" viewBox="0 0 720 350" role="img" aria-label="The prediction loop every block goes through">
<title>The prediction loop every block goes through</title>
<desc>A source block has a prediction subtracted from it. The residual is transformed, quantized and entropy coded into the bitstream. The quantized residual is also undone and added back to the prediction to rebuild the block. Intra prediction reads the rebuilt block before the in-loop filters. Inter prediction reads decoded pictures after them.</desc>
<rect class="d-box" x="20" y="30" width="110" height="40" rx="6"/>
<text class="d-label" x="75" y="54.5" text-anchor="middle">Source block</text>
<circle class="d-box" cx="175" cy="50" r="13"/>
<text class="d-text" x="175" y="55" text-anchor="middle">−</text>
<line class="d-arrow" x1="130" y1="50" x2="160" y2="50"/>
<line class="d-arrow" x1="188" y1="50" x2="254" y2="50"/>
<text class="d-label" x="221" y="40" text-anchor="middle">residual</text>
<rect class="d-box" x="256" y="30" width="140" height="40" rx="6"/>
<text class="d-label" x="326" y="46.5" text-anchor="middle">Transform</text>
<text class="d-label" x="326" y="62.5" text-anchor="middle">and quantize</text>
<line class="d-arrow" x1="396" y1="50" x2="448" y2="50"/>
<rect class="d-box" x="450" y="30" width="120" height="40" rx="6"/>
<text class="d-label" x="510" y="54.5" text-anchor="middle">Entropy coder</text>
<line class="d-arrow" x1="570" y1="50" x2="628" y2="50"/>
<text class="d-label" x="670" y="55" text-anchor="middle">bitstream</text>
<path class="d-arrow" d="M422 50 L422 128"/>
<rect class="d-box" x="350" y="130" width="180" height="40" rx="6"/>
<text class="d-label" x="440" y="146.5" text-anchor="middle">Undo the quantizer</text>
<text class="d-label" x="440" y="162.5" text-anchor="middle">and the transform</text>
<circle class="d-box" cx="290" cy="150" r="13"/>
<text class="d-text" x="290" y="155" text-anchor="middle">+</text>
<line class="d-arrow" x1="350" y1="150" x2="305" y2="150"/>
<rect class="d-accent" x="110" y="130" width="120" height="40" rx="6"/>
<text class="d-label" x="170" y="154.5" text-anchor="middle">Prediction</text>
<line class="d-arrow" x1="230" y1="150" x2="275" y2="150"/>
<line class="d-arrow" x1="175" y1="130" x2="175" y2="65"/>
<path class="d-arrow" d="M290 163 L290 250 L372 250"/>
<text class="d-label" x="296" y="215" text-anchor="start">rebuilt block</text>
<rect class="d-box" x="374" y="230" width="140" height="40" rx="6"/>
<text class="d-label" x="444" y="254.5" text-anchor="middle">In-loop filters</text>
<path class="d-arrow" d="M444 270 L444 310 L242 310"/>
<rect class="d-box" x="100" y="290" width="140" height="40" rx="6"/>
<text class="d-label" x="170" y="314.5" text-anchor="middle">Decoded pictures</text>
<path class="d-arrow" d="M140 290 L140 172"/>
<text class="d-label" x="134" y="235" text-anchor="end">inter</text>
<path class="d-arrow d-muted" d="M290 200 L200 200 L200 172"/>
<text class="d-label" x="245" y="193" text-anchor="middle">intra</text>
</svg>
<svg class="d-narrow" viewBox="0 0 360 390" role="img" aria-labelledby="loop-fig-t-n loop-fig-d-n">
<title id="loop-fig-t-n">The prediction loop every block goes through</title>
<desc id="loop-fig-d-n">A source block has a prediction subtracted from it. The residual is transformed, quantized and entropy coded into the bitstream. The quantized residual is also undone and added back to the prediction to rebuild the block. Intra prediction reads the rebuilt block before the in-loop filters. Inter prediction reads decoded pictures after them.</desc>
<rect class="d-box" x="8" y="10" width="110" height="40" rx="6"/>
<text class="d-label" x="63" y="34.5" text-anchor="middle">Source block</text>
<line class="d-arrow" x1="63" y1="50" x2="63" y2="97"/>
<circle class="d-box" cx="63" cy="112" r="13"/>
<text class="d-text" x="63" y="117" text-anchor="middle">−</text>
<line class="d-arrow" x1="63" y1="125" x2="63" y2="168"/>
<text class="d-label" x="71" y="150" text-anchor="start">residual</text>
<rect class="d-box" x="8" y="170" width="110" height="44" rx="6"/>
<text class="d-label" x="63" y="188.5" text-anchor="middle">Transform</text>
<text class="d-label" x="63" y="204.5" text-anchor="middle">and quantize</text>
<line class="d-arrow" x1="63" y1="214" x2="63" y2="288"/>
<rect class="d-box" x="8" y="290" width="110" height="40" rx="6"/>
<text class="d-label" x="63" y="314.5" text-anchor="middle">Entropy coder</text>
<line class="d-arrow" x1="63" y1="330" x2="63" y2="356"/>
<text class="d-label" x="63" y="376" text-anchor="middle">bitstream</text>
<rect class="d-accent" x="135" y="92" width="112" height="40" rx="6"/>
<text class="d-label" x="191" y="116.5" text-anchor="middle">Prediction</text>
<line class="d-arrow" x1="135" y1="112" x2="78" y2="112"/>
<line class="d-arrow" x1="192" y1="132" x2="192" y2="165"/>
<circle class="d-box" cx="192" cy="180" r="13"/>
<text class="d-text" x="192" y="185" text-anchor="middle">+</text>
<rect class="d-box" x="135" y="222" width="115" height="56" rx="6"/>
<text class="d-label" x="192.5" y="239.5" text-anchor="middle">Undo the</text>
<text class="d-label" x="192.5" y="254.5" text-anchor="middle">quantizer and</text>
<text class="d-label" x="192.5" y="269.5" text-anchor="middle">the transform</text>
<path class="d-arrow" d="M63 250 H133"/>
<line class="d-arrow" x1="192" y1="222" x2="192" y2="195"/>
<path class="d-arrow" d="M205 180 H282"/>
<text class="d-label" x="212" y="199" text-anchor="start">rebuilt</text>
<text class="d-label" x="212" y="214" text-anchor="start">block</text>
<rect class="d-box" x="284" y="160" width="70" height="40" rx="6"/>
<text class="d-label" x="319" y="176.5" text-anchor="middle">In-loop</text>
<text class="d-label" x="319" y="192.5" text-anchor="middle">filters</text>
<line class="d-arrow" x1="319" y1="160" x2="319" y2="134"/>
<rect class="d-box" x="284" y="92" width="70" height="40" rx="6"/>
<text class="d-label" x="319" y="108.5" text-anchor="middle">Decoded</text>
<text class="d-label" x="319" y="124.5" text-anchor="middle">pictures</text>
<line class="d-arrow" x1="284" y1="112" x2="249" y2="112"/>
<text class="d-label" x="266" y="102" text-anchor="middle">inter</text>
<path class="d-arrow d-muted" d="M232 180 V134"/>
<text class="d-label" x="238" y="162" text-anchor="start">intra</text>
</svg>
<figcaption>Every block goes around this loop. The lower half is the decoder's work, done inside the encoder so both predict from the same samples.</figcaption>
</figure>

Pictures come in three kinds. A decoder can start from an I picture because it uses only intra prediction. A P picture also predicts from earlier pictures. A B picture can predict from two pictures at once. One of them may be shown later than the B picture itself.

## What HEVC adds

HEVC came after H.264 and has the same steps. What grew is the number of choices at each step. The standard's stated goal was the same picture quality as H.264 at about half the bitrate.

| | H.264 | HEVC |
|---|---|---|
| largest block | 16x16 macroblock | coding tree unit, up to 64x64 |
| intra prediction modes | up to 9 | 35 |
| reusing a neighbor's motion | skip and direct modes | merge, from a list of up to five candidates |
| transform sizes | 4x4 and 8x8 | 4x4 up to 32x32 |
| filters inside the loop | deblocking | deblocking, then sample adaptive offset |
| entropy coder | CAVLC or CABAC | CABAC only |

The larger block matters most. A patch of sky in a 4K picture is thousands of samples of nearly the same color. One large block describes it in a handful of bits. The price is paid by the encoder, because every extra choice is something it has to consider.

<figure class="diagram">
<svg class="d-wide" viewBox="0 0 720 262" role="img" aria-label="The same 64x64 area in H.264 and in HEVC">
<title>The same 64x64 area in H.264 and in HEVC</title>
<desc>On the left H.264 covers a 64 by 64 area with sixteen 16 by 16 macroblocks. On the right HEVC covers it with one coding tree unit that is split only where the picture has detail: three 32 by 32 blocks and one quarter split further down to 8 by 8.</desc>
<text class="d-label" x="170" y="28" text-anchor="middle">H.264: sixteen fixed macroblocks</text>
<text class="d-label" x="550" y="28" text-anchor="middle">HEVC: one coding tree unit</text>
<rect class="d-box" x="70" y="44" width="200" height="200" rx="0"/>
<line class="d-line d-muted" x1="120" y1="44" x2="120" y2="244"/>
<line class="d-line d-muted" x1="70" y1="94" x2="270" y2="94"/>
<line class="d-line d-muted" x1="170" y1="44" x2="170" y2="244"/>
<line class="d-line d-muted" x1="70" y1="144" x2="270" y2="144"/>
<line class="d-line d-muted" x1="220" y1="44" x2="220" y2="244"/>
<line class="d-line d-muted" x1="70" y1="194" x2="270" y2="194"/>
<rect class="d-accent" x="450" y="44" width="200" height="200" rx="0"/>
<line class="d-line" x1="550" y1="44" x2="550" y2="244"/>
<line class="d-line" x1="450" y1="144" x2="650" y2="144"/>
<line class="d-line" x1="600" y1="144" x2="600" y2="244"/>
<line class="d-line" x1="550" y1="194" x2="650" y2="194"/>
<line class="d-line" x1="625" y1="194" x2="625" y2="244"/>
<line class="d-line" x1="600" y1="219" x2="650" y2="219"/>
<text class="d-label" x="500" y="99" text-anchor="middle">32x32</text>
<text class="d-label" x="600" y="99" text-anchor="middle">32x32</text>
<text class="d-label" x="500" y="199" text-anchor="middle">32x32</text>
<text class="d-label" x="575" y="174" text-anchor="middle">16</text>
<text class="d-label" x="95" y="74" text-anchor="middle">16</text>
<line class="d-arrow d-muted" x1="290" y1="144" x2="430" y2="144"/>
<text class="d-label" x="360" y="134" text-anchor="middle">same 64x64 area</text>
</svg>
<svg class="d-narrow" viewBox="0 0 360 445" role="img" aria-labelledby="ctu-mb-fig-t-n ctu-mb-fig-d-n">
<title id="ctu-mb-fig-t-n">The same 64x64 area in H.264 and in HEVC</title>
<desc id="ctu-mb-fig-d-n">On the left H.264 covers a 64 by 64 area with sixteen 16 by 16 macroblocks. On the right HEVC covers it with one coding tree unit that is split only where the picture has detail: three 32 by 32 blocks and one quarter split further down to 8 by 8.</desc>
<text class="d-label" x="180" y="18" text-anchor="middle">H.264: sixteen fixed macroblocks</text>
<rect class="d-box" x="100" y="28" width="160" height="160" rx="0"/>
<line class="d-line d-muted" x1="140" y1="28" x2="140" y2="188"/>
<line class="d-line d-muted" x1="100" y1="68" x2="260" y2="68"/>
<line class="d-line d-muted" x1="180" y1="28" x2="180" y2="188"/>
<line class="d-line d-muted" x1="100" y1="108" x2="260" y2="108"/>
<line class="d-line d-muted" x1="220" y1="28" x2="220" y2="188"/>
<line class="d-line d-muted" x1="100" y1="148" x2="260" y2="148"/>
<text class="d-label" x="120" y="52" text-anchor="middle">16</text>
<line class="d-arrow d-muted" x1="180" y1="198" x2="180" y2="238"/>
<text class="d-label" x="190" y="222" text-anchor="start">same 64x64 area</text>
<text class="d-label" x="180" y="262" text-anchor="middle">HEVC: one coding tree unit</text>
<rect class="d-accent" x="100" y="272" width="160" height="160" rx="0"/>
<line class="d-line" x1="180" y1="272" x2="180" y2="432"/>
<line class="d-line" x1="100" y1="352" x2="260" y2="352"/>
<line class="d-line" x1="220" y1="352" x2="220" y2="432"/>
<line class="d-line" x1="180" y1="392" x2="260" y2="392"/>
<line class="d-line" x1="240" y1="392" x2="240" y2="432"/>
<line class="d-line" x1="220" y1="412" x2="260" y2="412"/>
<text class="d-label" x="140" y="316" text-anchor="middle">32x32</text>
<text class="d-label" x="220" y="316" text-anchor="middle">32x32</text>
<text class="d-label" x="140" y="396" text-anchor="middle">32x32</text>
<text class="d-label" x="200" y="376" text-anchor="middle">16</text>
</svg>
<figcaption>HEVC spends one large block on a flat area and splits only where the picture has detail.</figcaption>
</figure>

## Pictures, slices and NAL units

An HEVC stream is a row of packets called NAL units. Each one starts with a two-byte header that says what kind it is.

Three kinds hold settings and no picture data. The video parameter set (VPS) describes the stream as a whole. Below it, the sequence parameter set (SPS) holds the picture size, the bit depth and which tools are switched on. Settings that can change from picture to picture go in the picture parameter set (PPS). The picture data itself travels in slices, where a slice is a run of blocks that can be decoded without the picture's other slices. A decoder may ignore the last kind, supplemental enhancement information (SEI). It holds extras such as HDR metadata.

<figure class="diagram">
<svg class="d-wide" viewBox="0 0 720 214" role="img" aria-label="The NAL units at the start of a stream">
<title>The NAL units at the start of a stream</title>
<desc>A row of NAL units in stream order: the video, sequence and picture parameter sets, then an SEI message and a slice for the key picture, then slices for a P picture and a B picture. Arrows show each slice naming its picture parameter set, which names its sequence parameter set, which names its video parameter set.</desc>
<rect class="d-fill" x="20" y="100" width="70" height="40" rx="6"/>
<text class="d-mono" x="55" y="124.5" text-anchor="middle">VPS</text>
<rect class="d-fill" x="100" y="100" width="70" height="40" rx="6"/>
<text class="d-mono" x="135" y="124.5" text-anchor="middle">SPS</text>
<rect class="d-fill" x="180" y="100" width="70" height="40" rx="6"/>
<text class="d-mono" x="215" y="124.5" text-anchor="middle">PPS</text>
<rect class="d-muted" x="275" y="100" width="60" height="40" rx="6"/>
<text class="d-mono" x="305" y="124.5" text-anchor="middle">SEI</text>
<rect class="d-accent" x="340" y="100" width="90" height="40" rx="6"/>
<text class="d-mono" x="385" y="124.5" text-anchor="middle">slice</text>
<rect class="d-box" x="455" y="100" width="90" height="40" rx="6"/>
<text class="d-mono" x="500" y="124.5" text-anchor="middle">slice</text>
<rect class="d-box" x="570" y="100" width="90" height="40" rx="6"/>
<text class="d-mono" x="615" y="124.5" text-anchor="middle">slice</text>
<text class="d-label" x="385" y="160" text-anchor="middle">key picture</text>
<text class="d-label" x="500" y="160" text-anchor="middle">P picture</text>
<text class="d-label" x="615" y="160" text-anchor="middle">B picture</text>
<path class="d-arrow" d="M385 100 C385 50 215 50 215 98"/>
<path class="d-arrow d-muted" d="M500 100 C500 30 225 30 222 98"/>
<path class="d-arrow d-muted" d="M615 100 C615 12 235 12 229 98"/>
<path class="d-arrow" d="M205 100 C195 70 150 70 140 98"/>
<path class="d-arrow" d="M125 100 C115 70 70 70 60 98"/>
<text class="d-label" x="430" y="18" text-anchor="middle">each slice names its PPS</text>
<text class="d-label" x="170" y="66" text-anchor="middle">its SPS</text>
<text class="d-label" x="92" y="66" text-anchor="middle">its VPS</text>
<path class="d-line d-muted" d="M275 176 V184 H430 V176"/>
<text class="d-label" x="352" y="202" text-anchor="middle">one access unit: everything for one picture</text>
<path class="d-line d-muted" d="M20 176 V184 H250 V176"/>
<text class="d-label" x="135" y="202" text-anchor="middle">parameter sets</text>
</svg>
<svg class="d-narrow" viewBox="0 0 360 360" role="img" aria-labelledby="nal-fig-t-n nal-fig-d-n">
<title id="nal-fig-t-n">The NAL units at the start of a stream</title>
<desc id="nal-fig-d-n">A row of NAL units in stream order: the video, sequence and picture parameter sets, then an SEI message and a slice for the key picture, then slices for a P picture and a B picture. Arrows show each slice naming its picture parameter set, which names its sequence parameter set, which names its video parameter set.</desc>
<text class="d-label" x="24" y="18" text-anchor="start">parameter sets</text>
<path class="d-line d-muted" d="M20 28 H14 V140 H20"/>
<rect class="d-fill" x="24" y="28" width="160" height="32" rx="6"/>
<text class="d-mono" x="36" y="48.5" text-anchor="start">VPS</text>
<rect class="d-fill" x="24" y="68" width="160" height="32" rx="6"/>
<text class="d-mono" x="36" y="88.5" text-anchor="start">SPS</text>
<rect class="d-fill" x="24" y="108" width="160" height="32" rx="6"/>
<text class="d-mono" x="36" y="128.5" text-anchor="start">PPS</text>
<text class="d-label" x="24" y="164" text-anchor="start">one access unit:</text>
<text class="d-label" x="24" y="180" text-anchor="start">everything for one picture</text>
<path class="d-line d-muted" d="M20 190 H14 V262 H20"/>
<rect class="d-muted" x="24" y="190" width="160" height="32" rx="6"/>
<text class="d-mono" x="36" y="210.5" text-anchor="start">SEI</text>
<rect class="d-accent" x="24" y="230" width="160" height="32" rx="6"/>
<text class="d-mono" x="36" y="250.5" text-anchor="start">slice</text>
<text class="d-label" x="174" y="250.5" text-anchor="end">key picture</text>
<rect class="d-box" x="24" y="278" width="160" height="32" rx="6"/>
<text class="d-mono" x="36" y="298.5" text-anchor="start">slice</text>
<text class="d-label" x="174" y="298.5" text-anchor="end">P picture</text>
<rect class="d-box" x="24" y="318" width="160" height="32" rx="6"/>
<text class="d-mono" x="36" y="338.5" text-anchor="start">slice</text>
<text class="d-label" x="174" y="338.5" text-anchor="end">B picture</text>
<path class="d-arrow" d="M184 80 C210 80 210 44 186 44"/>
<text class="d-label" x="216" y="66" text-anchor="start">its VPS</text>
<path class="d-arrow" d="M184 112 C210 112 210 90 186 90"/>
<text class="d-label" x="216" y="108" text-anchor="start">its SPS</text>
<path class="d-arrow" d="M184 246 C230 246 230 120 186 120"/>
<path class="d-arrow d-muted" d="M184 294 C250 294 250 128 186 128"/>
<path class="d-arrow d-muted" d="M184 334 C270 334 270 136 186 136"/>
<text class="d-label" x="262" y="210" text-anchor="start">each slice</text>
<text class="d-label" x="262" y="226" text-anchor="start">names its PPS</text>
</svg>
<figcaption>A stream is a row of NAL units. A slice holds picture data and points back at the parameter sets it needs.</figcaption>
</figure>

A decoder can only start at a key picture. HEVC has two main kinds. After an IDR picture, nothing refers to anything that came before it. A CRA picture (clean random access) lets some pictures that are shown before it predict across it, from both sides. Those are called leading pictures. A group of pictures that ends this way is an open GOP. It costs fewer bits than a closed one because the pictures just before the key picture still get two references.

B pictures are usually arranged as a pyramid. The middle picture of a group is coded first from the pictures at either end. Each half is then filled in the same way. That's why pictures are decoded in a different order from the one they're shown in.

Every slice also lists the pictures the decoder has to keep for later prediction. That list is the reference picture set. A decoder that joins late or loses a packet still knows exactly which pictures it should have.

<figure class="diagram">
<svg class="d-wide" viewBox="0 0 720 306" role="img" aria-label="A B-picture pyramid and an open GOP">
<title>A B-picture pyramid and an open GOP</title>
<desc>Seventeen pictures in the order they are shown. Pictures 0 and 8 are an I and a P picture. The seven B pictures between them form a pyramid: picture 4 predicts from 0 and 8, pictures 2 and 6 from their neighbors in the layer above, and the odd pictures from the pictures on either side. Picture 16 is a CRA key picture. Pictures 9 to 15 are leading pictures: they are decoded after the CRA and shown before it, and they predict from both picture 8 and the CRA.</desc>
<line class="d-arrow d-muted" x1="232" y1="88" x2="91" y2="74"/>
<line class="d-arrow d-muted" x1="232" y1="88" x2="373" y2="74"/>
<line class="d-arrow d-muted" x1="158" y1="132" x2="91" y2="74"/>
<line class="d-arrow d-muted" x1="158" y1="132" x2="225" y2="118"/>
<line class="d-arrow d-muted" x1="306" y1="132" x2="239" y2="118"/>
<line class="d-arrow d-muted" x1="306" y1="132" x2="373" y2="74"/>
<line class="d-arrow d-muted" x1="121" y1="176" x2="91" y2="74"/>
<line class="d-arrow d-muted" x1="121" y1="176" x2="151" y2="162"/>
<line class="d-arrow d-muted" x1="195" y1="176" x2="165" y2="162"/>
<line class="d-arrow d-muted" x1="195" y1="176" x2="225" y2="118"/>
<line class="d-arrow d-muted" x1="269" y1="176" x2="239" y2="118"/>
<line class="d-arrow d-muted" x1="269" y1="176" x2="299" y2="162"/>
<line class="d-arrow d-muted" x1="343" y1="176" x2="313" y2="162"/>
<line class="d-arrow d-muted" x1="343" y1="176" x2="373" y2="74"/>
<line class="d-arrow d-muted" x1="528" y1="88" x2="387" y2="74"/>
<line class="d-arrow d-muted" x1="528" y1="88" x2="669" y2="74"/>
<line class="d-arrow d-muted" x1="454" y1="132" x2="387" y2="74"/>
<line class="d-arrow d-muted" x1="454" y1="132" x2="521" y2="118"/>
<line class="d-arrow d-muted" x1="602" y1="132" x2="535" y2="118"/>
<line class="d-arrow d-muted" x1="602" y1="132" x2="669" y2="74"/>
<line class="d-arrow d-muted" x1="417" y1="176" x2="387" y2="74"/>
<line class="d-arrow d-muted" x1="417" y1="176" x2="447" y2="162"/>
<line class="d-arrow d-muted" x1="491" y1="176" x2="461" y2="162"/>
<line class="d-arrow d-muted" x1="491" y1="176" x2="521" y2="118"/>
<line class="d-arrow d-muted" x1="565" y1="176" x2="535" y2="118"/>
<line class="d-arrow d-muted" x1="565" y1="176" x2="595" y2="162"/>
<line class="d-arrow d-muted" x1="639" y1="176" x2="609" y2="162"/>
<line class="d-arrow d-muted" x1="639" y1="176" x2="669" y2="74"/>
<rect class="d-box" x="68" y="44" width="32" height="28" rx="4"/>
<text class="d-mono" x="84" y="62.5" text-anchor="middle">I</text>
<text class="d-mono" x="84" y="232" text-anchor="middle">0</text>
<text class="d-mono d-muted" x="84" y="256" text-anchor="middle">0</text>
<rect class="d-box" x="105" y="176" width="32" height="28" rx="4"/>
<text class="d-mono" x="121" y="194.5" text-anchor="middle">B</text>
<text class="d-mono" x="121" y="232" text-anchor="middle">1</text>
<text class="d-mono d-muted" x="121" y="256" text-anchor="middle">4</text>
<rect class="d-box" x="142" y="132" width="32" height="28" rx="4"/>
<text class="d-mono" x="158" y="150.5" text-anchor="middle">B</text>
<text class="d-mono" x="158" y="232" text-anchor="middle">2</text>
<text class="d-mono d-muted" x="158" y="256" text-anchor="middle">3</text>
<rect class="d-box" x="179" y="176" width="32" height="28" rx="4"/>
<text class="d-mono" x="195" y="194.5" text-anchor="middle">B</text>
<text class="d-mono" x="195" y="232" text-anchor="middle">3</text>
<text class="d-mono d-muted" x="195" y="256" text-anchor="middle">5</text>
<rect class="d-box" x="216" y="88" width="32" height="28" rx="4"/>
<text class="d-mono" x="232" y="106.5" text-anchor="middle">B</text>
<text class="d-mono" x="232" y="232" text-anchor="middle">4</text>
<text class="d-mono d-muted" x="232" y="256" text-anchor="middle">2</text>
<rect class="d-box" x="253" y="176" width="32" height="28" rx="4"/>
<text class="d-mono" x="269" y="194.5" text-anchor="middle">B</text>
<text class="d-mono" x="269" y="232" text-anchor="middle">5</text>
<text class="d-mono d-muted" x="269" y="256" text-anchor="middle">7</text>
<rect class="d-box" x="290" y="132" width="32" height="28" rx="4"/>
<text class="d-mono" x="306" y="150.5" text-anchor="middle">B</text>
<text class="d-mono" x="306" y="232" text-anchor="middle">6</text>
<text class="d-mono d-muted" x="306" y="256" text-anchor="middle">6</text>
<rect class="d-box" x="327" y="176" width="32" height="28" rx="4"/>
<text class="d-mono" x="343" y="194.5" text-anchor="middle">B</text>
<text class="d-mono" x="343" y="232" text-anchor="middle">7</text>
<text class="d-mono d-muted" x="343" y="256" text-anchor="middle">8</text>
<rect class="d-box" x="364" y="44" width="32" height="28" rx="4"/>
<text class="d-mono" x="380" y="62.5" text-anchor="middle">P</text>
<text class="d-mono" x="380" y="232" text-anchor="middle">8</text>
<text class="d-mono d-muted" x="380" y="256" text-anchor="middle">1</text>
<rect class="d-fill" x="401" y="176" width="32" height="28" rx="4"/>
<text class="d-mono" x="417" y="194.5" text-anchor="middle">B</text>
<text class="d-mono" x="417" y="232" text-anchor="middle">9</text>
<text class="d-mono d-muted" x="417" y="256" text-anchor="middle">12</text>
<rect class="d-fill" x="438" y="132" width="32" height="28" rx="4"/>
<text class="d-mono" x="454" y="150.5" text-anchor="middle">B</text>
<text class="d-mono" x="454" y="232" text-anchor="middle">10</text>
<text class="d-mono d-muted" x="454" y="256" text-anchor="middle">11</text>
<rect class="d-fill" x="475" y="176" width="32" height="28" rx="4"/>
<text class="d-mono" x="491" y="194.5" text-anchor="middle">B</text>
<text class="d-mono" x="491" y="232" text-anchor="middle">11</text>
<text class="d-mono d-muted" x="491" y="256" text-anchor="middle">13</text>
<rect class="d-fill" x="512" y="88" width="32" height="28" rx="4"/>
<text class="d-mono" x="528" y="106.5" text-anchor="middle">B</text>
<text class="d-mono" x="528" y="232" text-anchor="middle">12</text>
<text class="d-mono d-muted" x="528" y="256" text-anchor="middle">10</text>
<rect class="d-fill" x="549" y="176" width="32" height="28" rx="4"/>
<text class="d-mono" x="565" y="194.5" text-anchor="middle">B</text>
<text class="d-mono" x="565" y="232" text-anchor="middle">13</text>
<text class="d-mono d-muted" x="565" y="256" text-anchor="middle">15</text>
<rect class="d-fill" x="586" y="132" width="32" height="28" rx="4"/>
<text class="d-mono" x="602" y="150.5" text-anchor="middle">B</text>
<text class="d-mono" x="602" y="232" text-anchor="middle">14</text>
<text class="d-mono d-muted" x="602" y="256" text-anchor="middle">14</text>
<rect class="d-fill" x="623" y="176" width="32" height="28" rx="4"/>
<text class="d-mono" x="639" y="194.5" text-anchor="middle">B</text>
<text class="d-mono" x="639" y="232" text-anchor="middle">15</text>
<text class="d-mono d-muted" x="639" y="256" text-anchor="middle">16</text>
<rect class="d-accent" x="660" y="44" width="32" height="28" rx="4"/>
<text class="d-mono" x="676" y="62.5" text-anchor="middle">CRA</text>
<text class="d-mono" x="676" y="232" text-anchor="middle">16</text>
<text class="d-mono d-muted" x="676" y="256" text-anchor="middle">9</text>
<text class="d-label" x="10" y="232" text-anchor="start">shown</text>
<text class="d-label" x="10" y="256" text-anchor="start">decoded</text>
<path class="d-line d-muted" d="M403 268 V276 H653 V268"/>
<text class="d-label" x="528" y="294" text-anchor="middle">leading pictures: decoded after the CRA, shown before it</text>
</svg>
<svg class="d-narrow" viewBox="0 0 360 480" role="img" aria-labelledby="gop-fig-t-n gop-fig-d-n">
<title id="gop-fig-t-n">A B-picture pyramid and an open GOP</title>
<desc id="gop-fig-d-n">Seventeen pictures in the order they are shown. Pictures 0 and 8 are an I and a P picture. The seven B pictures between them form a pyramid: picture 4 predicts from 0 and 8, pictures 2 and 6 from their neighbors in the layer above, and the odd pictures from the pictures on either side. Picture 16 is a CRA key picture. Pictures 9 to 15 are leading pictures: they are decoded after the CRA and shown before it, and they predict from both picture 8 and the CRA.</desc>
<line class="d-arrow d-muted" x1="102" y1="126" x2="75" y2="38"/>
<line class="d-arrow d-muted" x1="102" y1="126" x2="129" y2="114"/>
<line class="d-arrow d-muted" x1="136" y1="88" x2="75" y2="38"/>
<line class="d-arrow d-muted" x1="136" y1="88" x2="197" y2="76"/>
<line class="d-arrow d-muted" x1="170" y1="126" x2="143" y2="114"/>
<line class="d-arrow d-muted" x1="170" y1="126" x2="197" y2="76"/>
<line class="d-arrow d-muted" x1="204" y1="50" x2="75" y2="38"/>
<line class="d-arrow d-muted" x1="204" y1="50" x2="333" y2="38"/>
<line class="d-arrow d-muted" x1="238" y1="126" x2="211" y2="76"/>
<line class="d-arrow d-muted" x1="238" y1="126" x2="265" y2="114"/>
<line class="d-arrow d-muted" x1="272" y1="88" x2="211" y2="76"/>
<line class="d-arrow d-muted" x1="272" y1="88" x2="333" y2="38"/>
<line class="d-arrow d-muted" x1="306" y1="126" x2="279" y2="114"/>
<line class="d-arrow d-muted" x1="306" y1="126" x2="333" y2="38"/>
<rect class="d-box" x="54" y="12" width="28" height="24" rx="4"/>
<text class="d-mono" x="68" y="28.5" text-anchor="middle">I</text>
<text class="d-mono" x="68" y="174" text-anchor="middle">0</text>
<text class="d-mono d-muted" x="68" y="196" text-anchor="middle">0</text>
<rect class="d-box" x="88" y="126" width="28" height="24" rx="4"/>
<text class="d-mono" x="102" y="142.5" text-anchor="middle">B</text>
<text class="d-mono" x="102" y="174" text-anchor="middle">1</text>
<text class="d-mono d-muted" x="102" y="196" text-anchor="middle">4</text>
<rect class="d-box" x="122" y="88" width="28" height="24" rx="4"/>
<text class="d-mono" x="136" y="104.5" text-anchor="middle">B</text>
<text class="d-mono" x="136" y="174" text-anchor="middle">2</text>
<text class="d-mono d-muted" x="136" y="196" text-anchor="middle">3</text>
<rect class="d-box" x="156" y="126" width="28" height="24" rx="4"/>
<text class="d-mono" x="170" y="142.5" text-anchor="middle">B</text>
<text class="d-mono" x="170" y="174" text-anchor="middle">3</text>
<text class="d-mono d-muted" x="170" y="196" text-anchor="middle">5</text>
<rect class="d-box" x="190" y="50" width="28" height="24" rx="4"/>
<text class="d-mono" x="204" y="66.5" text-anchor="middle">B</text>
<text class="d-mono" x="204" y="174" text-anchor="middle">4</text>
<text class="d-mono d-muted" x="204" y="196" text-anchor="middle">2</text>
<rect class="d-box" x="224" y="126" width="28" height="24" rx="4"/>
<text class="d-mono" x="238" y="142.5" text-anchor="middle">B</text>
<text class="d-mono" x="238" y="174" text-anchor="middle">5</text>
<text class="d-mono d-muted" x="238" y="196" text-anchor="middle">7</text>
<rect class="d-box" x="258" y="88" width="28" height="24" rx="4"/>
<text class="d-mono" x="272" y="104.5" text-anchor="middle">B</text>
<text class="d-mono" x="272" y="174" text-anchor="middle">6</text>
<text class="d-mono d-muted" x="272" y="196" text-anchor="middle">6</text>
<rect class="d-box" x="292" y="126" width="28" height="24" rx="4"/>
<text class="d-mono" x="306" y="142.5" text-anchor="middle">B</text>
<text class="d-mono" x="306" y="174" text-anchor="middle">7</text>
<text class="d-mono d-muted" x="306" y="196" text-anchor="middle">8</text>
<rect class="d-box" x="326" y="12" width="28" height="24" rx="4"/>
<text class="d-mono" x="340" y="28.5" text-anchor="middle">P</text>
<text class="d-mono" x="340" y="174" text-anchor="middle">8</text>
<text class="d-mono d-muted" x="340" y="196" text-anchor="middle">1</text>
<text class="d-label" x="4" y="174" text-anchor="start">shown</text>
<text class="d-label" x="4" y="196" text-anchor="start">decoded</text>
<line class="d-arrow d-muted" x1="102" y1="346" x2="75" y2="258"/>
<line class="d-arrow d-muted" x1="102" y1="346" x2="129" y2="334"/>
<line class="d-arrow d-muted" x1="136" y1="308" x2="75" y2="258"/>
<line class="d-arrow d-muted" x1="136" y1="308" x2="197" y2="296"/>
<line class="d-arrow d-muted" x1="170" y1="346" x2="143" y2="334"/>
<line class="d-arrow d-muted" x1="170" y1="346" x2="197" y2="296"/>
<line class="d-arrow d-muted" x1="204" y1="270" x2="75" y2="258"/>
<line class="d-arrow d-muted" x1="204" y1="270" x2="333" y2="258"/>
<line class="d-arrow d-muted" x1="238" y1="346" x2="211" y2="296"/>
<line class="d-arrow d-muted" x1="238" y1="346" x2="265" y2="334"/>
<line class="d-arrow d-muted" x1="272" y1="308" x2="211" y2="296"/>
<line class="d-arrow d-muted" x1="272" y1="308" x2="333" y2="258"/>
<line class="d-arrow d-muted" x1="306" y1="346" x2="279" y2="334"/>
<line class="d-arrow d-muted" x1="306" y1="346" x2="333" y2="258"/>
<rect class="d-box" x="54" y="232" width="28" height="24" rx="4"/>
<text class="d-mono" x="68" y="248.5" text-anchor="middle">P</text>
<text class="d-mono" x="68" y="394" text-anchor="middle">8</text>
<text class="d-mono d-muted" x="68" y="416" text-anchor="middle">1</text>
<rect class="d-fill" x="88" y="346" width="28" height="24" rx="4"/>
<text class="d-mono" x="102" y="362.5" text-anchor="middle">B</text>
<text class="d-mono" x="102" y="394" text-anchor="middle">9</text>
<text class="d-mono d-muted" x="102" y="416" text-anchor="middle">12</text>
<rect class="d-fill" x="122" y="308" width="28" height="24" rx="4"/>
<text class="d-mono" x="136" y="324.5" text-anchor="middle">B</text>
<text class="d-mono" x="136" y="394" text-anchor="middle">10</text>
<text class="d-mono d-muted" x="136" y="416" text-anchor="middle">11</text>
<rect class="d-fill" x="156" y="346" width="28" height="24" rx="4"/>
<text class="d-mono" x="170" y="362.5" text-anchor="middle">B</text>
<text class="d-mono" x="170" y="394" text-anchor="middle">11</text>
<text class="d-mono d-muted" x="170" y="416" text-anchor="middle">13</text>
<rect class="d-fill" x="190" y="270" width="28" height="24" rx="4"/>
<text class="d-mono" x="204" y="286.5" text-anchor="middle">B</text>
<text class="d-mono" x="204" y="394" text-anchor="middle">12</text>
<text class="d-mono d-muted" x="204" y="416" text-anchor="middle">10</text>
<rect class="d-fill" x="224" y="346" width="28" height="24" rx="4"/>
<text class="d-mono" x="238" y="362.5" text-anchor="middle">B</text>
<text class="d-mono" x="238" y="394" text-anchor="middle">13</text>
<text class="d-mono d-muted" x="238" y="416" text-anchor="middle">15</text>
<rect class="d-fill" x="258" y="308" width="28" height="24" rx="4"/>
<text class="d-mono" x="272" y="324.5" text-anchor="middle">B</text>
<text class="d-mono" x="272" y="394" text-anchor="middle">14</text>
<text class="d-mono d-muted" x="272" y="416" text-anchor="middle">14</text>
<rect class="d-fill" x="292" y="346" width="28" height="24" rx="4"/>
<text class="d-mono" x="306" y="362.5" text-anchor="middle">B</text>
<text class="d-mono" x="306" y="394" text-anchor="middle">15</text>
<text class="d-mono d-muted" x="306" y="416" text-anchor="middle">16</text>
<rect class="d-accent" x="326" y="232" width="28" height="24" rx="4"/>
<text class="d-mono" x="340" y="248.5" text-anchor="middle">CRA</text>
<text class="d-mono" x="340" y="394" text-anchor="middle">16</text>
<text class="d-mono d-muted" x="340" y="416" text-anchor="middle">9</text>
<text class="d-label" x="4" y="394" text-anchor="start">shown</text>
<text class="d-label" x="4" y="416" text-anchor="start">decoded</text>
<path class="d-line d-muted" d="M88 426 V434 H320 V426"/>
<text class="d-label" x="204" y="452" text-anchor="middle">leading pictures:</text>
<text class="d-label" x="204" y="468" text-anchor="middle">decoded after the CRA, shown before it</text>
</svg>
<figcaption>Arrows point from a picture to the pictures it predicts from. The decoder takes the pictures in the bottom row's order and shows them in the top row's.</figcaption>
</figure>

## The coding tree

Each picture is divided into a grid of coding tree units (CTUs). The stream picks the size once, up to 64x64. A CTU either stays whole or splits into four squares that can each split again until they reach 8x8. The squares at the end of the splitting are coding units (CUs).

<figure class="diagram">
<svg class="d-wide" viewBox="0 0 720 306" role="img" aria-label="A coding tree unit split down to 8x8">
<title>A coding tree unit split down to 8x8</title>
<desc>On the left a 64 by 64 block is split into four 32 by 32 blocks. Two of those stay whole. Two are split into 16 by 16 blocks, and one 16 by 16 block is split into four 8 by 8 blocks. On the right the same splits are drawn as a tree with one level per block size.</desc>
<rect class="d-box" x="30" y="30" width="240" height="240" rx="0"/>
<line class="d-line" x1="150" y1="30" x2="150" y2="270"/>
<line class="d-line" x1="30" y1="150" x2="270" y2="150"/>
<line class="d-line" x1="90" y1="30" x2="90" y2="150"/>
<line class="d-line" x1="30" y1="90" x2="150" y2="90"/>
<line class="d-line" x1="120" y1="90" x2="120" y2="150"/>
<line class="d-line" x1="90" y1="120" x2="150" y2="120"/>
<line class="d-line" x1="210" y1="150" x2="210" y2="270"/>
<line class="d-line" x1="150" y1="210" x2="270" y2="210"/>
<rect class="d-accent" x="90" y="90" width="30" height="30" rx="0"/>
<text class="d-label" x="210" y="95" text-anchor="middle">32x32</text>
<text class="d-label" x="90" y="215" text-anchor="middle">32x32</text>
<text class="d-label" x="60" y="65" text-anchor="middle">16</text>
<text class="d-label" x="180" y="185" text-anchor="middle">16</text>
<text class="d-label" x="150" y="292" text-anchor="middle">one 64x64 coding tree unit</text>
<line class="d-line" x1="530" y1="49" x2="410" y2="101"/>
<line class="d-line" x1="530" y1="49" x2="490" y2="101"/>
<line class="d-line" x1="530" y1="49" x2="570" y2="101"/>
<line class="d-line" x1="530" y1="49" x2="650" y2="101"/>
<line class="d-line" x1="410" y1="119" x2="365" y2="171"/>
<line class="d-line" x1="410" y1="119" x2="395" y2="171"/>
<line class="d-line" x1="410" y1="119" x2="425" y2="171"/>
<line class="d-line" x1="410" y1="119" x2="455" y2="171"/>
<line class="d-line" x1="650" y1="119" x2="605" y2="171"/>
<line class="d-line" x1="650" y1="119" x2="635" y2="171"/>
<line class="d-line" x1="650" y1="119" x2="665" y2="171"/>
<line class="d-line" x1="650" y1="119" x2="695" y2="171"/>
<line class="d-line" x1="455" y1="189" x2="425" y2="241"/>
<line class="d-line" x1="455" y1="189" x2="445" y2="241"/>
<line class="d-line" x1="455" y1="189" x2="465" y2="241"/>
<line class="d-line" x1="455" y1="189" x2="485" y2="241"/>
<rect class="d-box" x="521" y="31" width="18" height="18" rx="3"/>
<rect class="d-box" x="401" y="101" width="18" height="18" rx="3"/>
<rect class="d-box" x="481" y="101" width="18" height="18" rx="3"/>
<rect class="d-box" x="561" y="101" width="18" height="18" rx="3"/>
<rect class="d-box" x="641" y="101" width="18" height="18" rx="3"/>
<rect class="d-box" x="356" y="171" width="18" height="18" rx="3"/>
<rect class="d-box" x="386" y="171" width="18" height="18" rx="3"/>
<rect class="d-box" x="416" y="171" width="18" height="18" rx="3"/>
<rect class="d-box" x="446" y="171" width="18" height="18" rx="3"/>
<rect class="d-box" x="596" y="171" width="18" height="18" rx="3"/>
<rect class="d-box" x="626" y="171" width="18" height="18" rx="3"/>
<rect class="d-box" x="656" y="171" width="18" height="18" rx="3"/>
<rect class="d-box" x="686" y="171" width="18" height="18" rx="3"/>
<rect class="d-accent" x="416" y="241" width="18" height="18" rx="3"/>
<rect class="d-box" x="436" y="241" width="18" height="18" rx="3"/>
<rect class="d-box" x="456" y="241" width="18" height="18" rx="3"/>
<rect class="d-box" x="476" y="241" width="18" height="18" rx="3"/>
<text class="d-mono" x="318" y="45" text-anchor="middle">64</text>
<text class="d-mono" x="318" y="115" text-anchor="middle">32</text>
<text class="d-mono" x="318" y="185" text-anchor="middle">16</text>
<text class="d-mono" x="318" y="255" text-anchor="middle">8</text>
<text class="d-label" x="530" y="292" text-anchor="middle">the same splits as a tree: each leaf is a coding unit</text>
</svg>
<svg class="d-narrow" viewBox="0 0 360 540" role="img" aria-labelledby="ctu-fig-t-n ctu-fig-d-n">
<title id="ctu-fig-t-n">A coding tree unit split down to 8x8</title>
<desc id="ctu-fig-d-n">On the left a 64 by 64 block is split into four 32 by 32 blocks. Two of those stay whole. Two are split into 16 by 16 blocks, and one 16 by 16 block is split into four 8 by 8 blocks. On the right the same splits are drawn as a tree with one level per block size.</desc>
<rect class="d-box" x="80" y="10" width="200" height="200" rx="0"/>
<line class="d-line" x1="180" y1="10" x2="180" y2="210"/>
<line class="d-line" x1="80" y1="110" x2="280" y2="110"/>
<line class="d-line" x1="130" y1="10" x2="130" y2="110"/>
<line class="d-line" x1="80" y1="60" x2="180" y2="60"/>
<line class="d-line" x1="155" y1="60" x2="155" y2="110"/>
<line class="d-line" x1="130" y1="85" x2="180" y2="85"/>
<line class="d-line" x1="230" y1="110" x2="230" y2="210"/>
<line class="d-line" x1="180" y1="160" x2="280" y2="160"/>
<rect class="d-accent" x="130" y="60" width="25" height="25" rx="0"/>
<text class="d-label" x="230" y="64.17" text-anchor="middle">32x32</text>
<text class="d-label" x="130" y="164.17" text-anchor="middle">32x32</text>
<text class="d-label" x="105" y="39.17" text-anchor="middle">16</text>
<text class="d-label" x="205" y="139.17" text-anchor="middle">16</text>
<text class="d-label" x="180" y="232" text-anchor="middle">one 64x64 coding tree unit</text>
<line class="d-line" x1="195.2" y1="278" x2="89.6" y2="330"/>
<line class="d-line" x1="195.2" y1="278" x2="160" y2="330"/>
<line class="d-line" x1="195.2" y1="278" x2="230.4" y2="330"/>
<line class="d-line" x1="195.2" y1="278" x2="300.8" y2="330"/>
<line class="d-line" x1="89.6" y1="348" x2="50" y2="400"/>
<line class="d-line" x1="89.6" y1="348" x2="76.4" y2="400"/>
<line class="d-line" x1="89.6" y1="348" x2="102.8" y2="400"/>
<line class="d-line" x1="89.6" y1="348" x2="129.2" y2="400"/>
<line class="d-line" x1="300.8" y1="348" x2="261.2" y2="400"/>
<line class="d-line" x1="300.8" y1="348" x2="287.6" y2="400"/>
<line class="d-line" x1="300.8" y1="348" x2="314" y2="400"/>
<line class="d-line" x1="300.8" y1="348" x2="340.4" y2="400"/>
<line class="d-line" x1="129.2" y1="418" x2="102.8" y2="470"/>
<line class="d-line" x1="129.2" y1="418" x2="120.4" y2="470"/>
<line class="d-line" x1="129.2" y1="418" x2="138" y2="470"/>
<line class="d-line" x1="129.2" y1="418" x2="155.6" y2="470"/>
<rect class="d-box" x="187.2" y="260" width="16" height="18" rx="3"/>
<rect class="d-box" x="81.6" y="330" width="16" height="18" rx="3"/>
<rect class="d-box" x="152" y="330" width="16" height="18" rx="3"/>
<rect class="d-box" x="222.4" y="330" width="16" height="18" rx="3"/>
<rect class="d-box" x="292.8" y="330" width="16" height="18" rx="3"/>
<rect class="d-box" x="42" y="400" width="16" height="18" rx="3"/>
<rect class="d-box" x="68.4" y="400" width="16" height="18" rx="3"/>
<rect class="d-box" x="94.8" y="400" width="16" height="18" rx="3"/>
<rect class="d-box" x="121.2" y="400" width="16" height="18" rx="3"/>
<rect class="d-box" x="253.2" y="400" width="16" height="18" rx="3"/>
<rect class="d-box" x="279.6" y="400" width="16" height="18" rx="3"/>
<rect class="d-box" x="306" y="400" width="16" height="18" rx="3"/>
<rect class="d-box" x="332.4" y="400" width="16" height="18" rx="3"/>
<rect class="d-accent" x="94.8" y="470" width="16" height="18" rx="3"/>
<rect class="d-box" x="112.4" y="470" width="16" height="18" rx="3"/>
<rect class="d-box" x="130" y="470" width="16" height="18" rx="3"/>
<rect class="d-box" x="147.6" y="470" width="16" height="18" rx="3"/>
<text class="d-mono" x="20" y="274" text-anchor="middle">64</text>
<text class="d-mono" x="20" y="344" text-anchor="middle">32</text>
<text class="d-mono" x="20" y="414" text-anchor="middle">16</text>
<text class="d-mono" x="20" y="484" text-anchor="middle">8</text>
<text class="d-label" x="180" y="514" text-anchor="middle">the same splits as a tree:</text>
<text class="d-label" x="180" y="530" text-anchor="middle">each leaf is a coding unit</text>
</svg>
<figcaption>Each split is a single flag in the stream. The highlighted 8x8 block is the highlighted leaf.</figcaption>
</figure>

A coding unit is where the encoder picks intra or inter prediction. Inside it, two more divisions apply. Prediction units (PUs) say how the CU is cut for prediction. An inter CU has eight shapes to pick from, including four uneven ones. Transform units (TUs) say how the residual is cut for the transform, through a second quadtree with sizes from 32x32 down to 4x4.

<figure class="diagram">
<svg class="d-wide" viewBox="0 0 720 338" role="img" aria-label="Prediction units and transform units on one coding unit">
<title>Prediction units and transform units on one coding unit</title>
<desc>The top row shows the eight partition shapes for inter prediction: the whole unit, two halves split either way, four quarters, and four uneven splits that cut at one quarter of the height or width. Below, one coding unit is drawn twice: once cut into two prediction units, and once cut by a separate quadtree into transform units of two sizes.</desc>
<rect class="d-box" x="36" y="40" width="60" height="60" rx="0"/>
<text class="d-mono" x="66" y="120" text-anchor="middle">2Nx2N</text>
<rect class="d-box" x="120" y="40" width="60" height="60" rx="0"/>
<line class="d-line d-accent" x1="120" y1="70" x2="180" y2="70"/>
<text class="d-mono" x="150" y="120" text-anchor="middle">2NxN</text>
<rect class="d-box" x="204" y="40" width="60" height="60" rx="0"/>
<line class="d-line d-accent" x1="234" y1="40" x2="234" y2="100"/>
<text class="d-mono" x="234" y="120" text-anchor="middle">Nx2N</text>
<rect class="d-box" x="288" y="40" width="60" height="60" rx="0"/>
<line class="d-line d-accent" x1="288" y1="70" x2="348" y2="70"/>
<line class="d-line d-accent" x1="318" y1="40" x2="318" y2="100"/>
<text class="d-mono" x="318" y="120" text-anchor="middle">NxN</text>
<rect class="d-box" x="372" y="40" width="60" height="60" rx="0"/>
<line class="d-line d-accent" x1="372" y1="55" x2="432" y2="55"/>
<text class="d-mono" x="402" y="120" text-anchor="middle">2NxnU</text>
<rect class="d-box" x="456" y="40" width="60" height="60" rx="0"/>
<line class="d-line d-accent" x1="456" y1="85" x2="516" y2="85"/>
<text class="d-mono" x="486" y="120" text-anchor="middle">2NxnD</text>
<rect class="d-box" x="540" y="40" width="60" height="60" rx="0"/>
<line class="d-line d-accent" x1="555" y1="40" x2="555" y2="100"/>
<text class="d-mono" x="570" y="120" text-anchor="middle">nLx2N</text>
<rect class="d-box" x="624" y="40" width="60" height="60" rx="0"/>
<line class="d-line d-accent" x1="669" y1="40" x2="669" y2="100"/>
<text class="d-mono" x="654" y="120" text-anchor="middle">nRx2N</text>
<text class="d-label" x="360" y="24" text-anchor="middle">the eight ways to cut a coding unit into prediction units</text>
<rect class="d-box" x="120" y="170" width="130" height="130" rx="0"/>
<line class="d-line d-accent" x1="185" y1="170" x2="185" y2="300"/>
<text class="d-label" x="152.5" y="239" text-anchor="middle">PU 0</text>
<text class="d-label" x="217.5" y="239" text-anchor="middle">PU 1</text>
<text class="d-label" x="185" y="322" text-anchor="middle">prediction: two halves, a motion vector each</text>
<rect class="d-box" x="470" y="170" width="130" height="130" rx="0"/>
<line class="d-dash" x1="535" y1="170" x2="535" y2="300"/>
<line class="d-dash" x1="470" y1="235" x2="600" y2="235"/>
<line class="d-dash" x1="567.5" y1="170" x2="567.5" y2="235"/>
<line class="d-dash" x1="535" y1="202.5" x2="600" y2="202.5"/>
<text class="d-label" x="502.5" y="206.5" text-anchor="middle">TU</text>
<text class="d-label" x="502.5" y="271.5" text-anchor="middle">TU</text>
<text class="d-label" x="535" y="322" text-anchor="middle">transform: its own quadtree over the same unit</text>
<line class="d-arrow d-muted" x1="270" y1="235" x2="450" y2="235"/>
<text class="d-label" x="360" y="225" text-anchor="middle">the same coding unit</text>
</svg>
<svg class="d-narrow" viewBox="0 0 360 620" role="img" aria-labelledby="pu-tu-fig-t-n pu-tu-fig-d-n">
<title id="pu-tu-fig-t-n">Prediction units and transform units on one coding unit</title>
<desc id="pu-tu-fig-d-n">The top row shows the eight partition shapes for inter prediction: the whole unit, two halves split either way, four quarters, and four uneven splits that cut at one quarter of the height or width. Below, one coding unit is drawn twice: once cut into two prediction units, and once cut by a separate quadtree into transform units of two sizes.</desc>
<text class="d-label" x="180" y="18" text-anchor="middle">the eight ways to cut a coding unit</text>
<text class="d-label" x="180" y="34" text-anchor="middle">into prediction units</text>
<rect class="d-box" x="24" y="46" width="60" height="60" rx="0"/>
<text class="d-mono" x="54" y="124" text-anchor="middle">2Nx2N</text>
<rect class="d-box" x="108" y="46" width="60" height="60" rx="0"/>
<line class="d-line d-accent" x1="108" y1="76" x2="168" y2="76"/>
<text class="d-mono" x="138" y="124" text-anchor="middle">2NxN</text>
<rect class="d-box" x="192" y="46" width="60" height="60" rx="0"/>
<line class="d-line d-accent" x1="222" y1="46" x2="222" y2="106"/>
<text class="d-mono" x="222" y="124" text-anchor="middle">Nx2N</text>
<rect class="d-box" x="276" y="46" width="60" height="60" rx="0"/>
<line class="d-line d-accent" x1="276" y1="76" x2="336" y2="76"/>
<line class="d-line d-accent" x1="306" y1="46" x2="306" y2="106"/>
<text class="d-mono" x="306" y="124" text-anchor="middle">NxN</text>
<rect class="d-box" x="24" y="140" width="60" height="60" rx="0"/>
<line class="d-line d-accent" x1="24" y1="155" x2="84" y2="155"/>
<text class="d-mono" x="54" y="218" text-anchor="middle">2NxnU</text>
<rect class="d-box" x="108" y="140" width="60" height="60" rx="0"/>
<line class="d-line d-accent" x1="108" y1="185" x2="168" y2="185"/>
<text class="d-mono" x="138" y="218" text-anchor="middle">2NxnD</text>
<rect class="d-box" x="192" y="140" width="60" height="60" rx="0"/>
<line class="d-line d-accent" x1="207" y1="140" x2="207" y2="200"/>
<text class="d-mono" x="222" y="218" text-anchor="middle">nLx2N</text>
<rect class="d-box" x="276" y="140" width="60" height="60" rx="0"/>
<line class="d-line d-accent" x1="321" y1="140" x2="321" y2="200"/>
<text class="d-mono" x="306" y="218" text-anchor="middle">nRx2N</text>
<rect class="d-box" x="115" y="246" width="130" height="130" rx="0"/>
<line class="d-line d-accent" x1="180" y1="246" x2="180" y2="376"/>
<text class="d-label" x="147.5" y="315" text-anchor="middle">PU 0</text>
<text class="d-label" x="212.5" y="315" text-anchor="middle">PU 1</text>
<text class="d-label" x="180" y="398" text-anchor="middle">prediction: two halves, a motion vector each</text>
<line class="d-arrow d-muted" x1="180" y1="410" x2="180" y2="448"/>
<text class="d-label" x="190" y="434" text-anchor="start">the same coding unit</text>
<rect class="d-box" x="115" y="456" width="130" height="130" rx="0"/>
<line class="d-dash" x1="180" y1="456" x2="180" y2="586"/>
<line class="d-dash" x1="115" y1="521" x2="245" y2="521"/>
<line class="d-dash" x1="212.5" y1="456" x2="212.5" y2="521"/>
<line class="d-dash" x1="180" y1="488.5" x2="245" y2="488.5"/>
<text class="d-label" x="147.5" y="492.5" text-anchor="middle">TU</text>
<text class="d-label" x="147.5" y="557.5" text-anchor="middle">TU</text>
<text class="d-label" x="180" y="608" text-anchor="middle">transform: its own quadtree over the same unit</text>
</svg>
<figcaption>Prediction and transform split the same coding unit separately. A transform unit may straddle the line between two prediction units.</figcaption>
</figure>

## Intra prediction

Intra prediction fills a block from the decoded samples just above it and to its left. HEVC has 35 ways to do it. Planar fits a smooth surface through the neighbors. DC fills the block with their average. The other 33 are angular modes. Each one extends the neighbor samples into the block along a direction.

<figure class="diagram">
<svg class="d-wide" viewBox="0 0 720 336" role="img" aria-label="The 35 intra prediction modes">
<title>The 35 intra prediction modes</title>
<desc>On the left a block with the row of decoded samples above it and the column to its left, each twice the block's length. On the right a fan of 33 lines from one point, covering half a turn from the lower left through horizontal and vertical to the upper right. Modes 2, 10, 18, 26 and 34 are marked. Planar and DC are modes 0 and 1.</desc>
<rect class="d-accent" x="66" y="116" width="194" height="14" rx="0"/>
<rect class="d-accent" x="66" y="130" width="14" height="180" rx="0"/>
<rect class="d-box" x="80" y="130" width="90" height="90" rx="0"/>
<text class="d-label" x="125" y="179" text-anchor="middle">block</text>
<text class="d-label" x="176" y="106" text-anchor="middle">samples above and above-right</text>
<text class="d-label" x="82" y="330" text-anchor="start">left and below-left</text>
<line class="d-line d-accent" x1="500" y1="172" x2="409.49" y2="262.51"/>
<text class="d-mono" x="396.762" y="279.238" text-anchor="middle">2</text>
<line class="d-line d-muted" x1="500" y1="172" x2="400.657" y2="252.716"/>
<line class="d-line d-muted" x1="500" y1="172" x2="392.986" y2="242.228"/>
<line class="d-line d-muted" x1="500" y1="172" x2="386.961" y2="232.052"/>
<line class="d-line d-muted" x1="500" y1="172" x2="381.412" y2="220.176"/>
<line class="d-line d-muted" x1="500" y1="172" x2="376.781" y2="206.655"/>
<line class="d-line d-muted" x1="500" y1="172" x2="373.534" y2="191.76"/>
<line class="d-line d-muted" x1="500" y1="172" x2="372.249" y2="179.984"/>
<line class="d-line d-accent" x1="500" y1="172" x2="372" y2="172"/>
<text class="d-mono" x="354" y="176" text-anchor="middle">10</text>
<line class="d-line d-muted" x1="500" y1="172" x2="372.249" y2="164.016"/>
<line class="d-line d-muted" x1="500" y1="172" x2="373.534" y2="152.24"/>
<line class="d-line d-muted" x1="500" y1="172" x2="376.781" y2="137.345"/>
<line class="d-line d-muted" x1="500" y1="172" x2="381.412" y2="123.824"/>
<line class="d-line d-muted" x1="500" y1="172" x2="386.961" y2="111.948"/>
<line class="d-line d-muted" x1="500" y1="172" x2="392.986" y2="101.772"/>
<line class="d-line d-muted" x1="500" y1="172" x2="400.657" y2="91.2841"/>
<line class="d-line d-accent" x1="500" y1="172" x2="409.49" y2="81.4903"/>
<text class="d-mono" x="396.762" y="72.7624" text-anchor="middle">18</text>
<line class="d-line d-muted" x1="500" y1="172" x2="419.284" y2="72.6574"/>
<line class="d-line d-muted" x1="500" y1="172" x2="429.772" y2="64.9859"/>
<line class="d-line d-muted" x1="500" y1="172" x2="439.948" y2="58.9612"/>
<line class="d-line d-muted" x1="500" y1="172" x2="451.824" y2="53.4123"/>
<line class="d-line d-muted" x1="500" y1="172" x2="465.345" y2="48.7807"/>
<line class="d-line d-muted" x1="500" y1="172" x2="480.24" y2="45.5345"/>
<line class="d-line d-muted" x1="500" y1="172" x2="492.016" y2="44.2493"/>
<line class="d-line d-accent" x1="500" y1="172" x2="500" y2="44"/>
<text class="d-mono" x="500" y="30" text-anchor="middle">26</text>
<line class="d-line d-muted" x1="500" y1="172" x2="507.984" y2="44.2493"/>
<line class="d-line d-muted" x1="500" y1="172" x2="519.76" y2="45.5345"/>
<line class="d-line d-muted" x1="500" y1="172" x2="534.655" y2="48.7807"/>
<line class="d-line d-muted" x1="500" y1="172" x2="548.176" y2="53.4123"/>
<line class="d-line d-muted" x1="500" y1="172" x2="560.052" y2="58.9612"/>
<line class="d-line d-muted" x1="500" y1="172" x2="570.228" y2="64.9859"/>
<line class="d-line d-muted" x1="500" y1="172" x2="580.716" y2="72.6574"/>
<line class="d-line d-accent" x1="500" y1="172" x2="590.51" y2="81.4903"/>
<text class="d-mono" x="603.238" y="72.7624" text-anchor="middle">34</text>
<circle class="d-accent" cx="500" cy="172" r="4"/>
<text class="d-label" x="560" y="212" text-anchor="start">33 angular modes,</text>
<text class="d-label" x="560" y="230" text-anchor="start">numbered 2 to 34</text>
<rect class="d-fill" x="556" y="250" width="100" height="30" rx="6"/>
<text class="d-mono" x="606" y="269.5" text-anchor="middle">0  planar</text>
<rect class="d-fill" x="556" y="286" width="100" height="30" rx="6"/>
<text class="d-mono" x="606" y="305.5" text-anchor="middle">1  DC</text>
<text class="d-label" x="338" y="176" text-anchor="end">horizontal</text>
<text class="d-label" x="500" y="10" text-anchor="middle">vertical</text>
</svg>
<svg class="d-narrow" viewBox="0 0 360 610" role="img" aria-labelledby="intra-fig-t-n intra-fig-d-n">
<title id="intra-fig-t-n">The 35 intra prediction modes</title>
<desc id="intra-fig-d-n">On the left a block with the row of decoded samples above it and the column to its left, each twice the block's length. On the right a fan of 33 lines from one point, covering half a turn from the lower left through horizontal and vertical to the upper right. Modes 2, 10, 18, 26 and 34 are marked. Planar and DC are modes 0 and 1.</desc>
<rect class="d-accent" x="80" y="28" width="194" height="14" rx="0"/>
<rect class="d-accent" x="80" y="42" width="14" height="180" rx="0"/>
<rect class="d-box" x="94" y="42" width="90" height="90" rx="0"/>
<text class="d-label" x="139" y="91" text-anchor="middle">block</text>
<text class="d-label" x="190" y="18" text-anchor="middle">samples above and above-right</text>
<text class="d-label" x="80" y="240" text-anchor="start">left and below-left</text>
<line class="d-line d-accent" x1="236" y1="420" x2="145.49" y2="510.51"/>
<line class="d-line d-muted" x1="236" y1="420" x2="136.66" y2="500.72"/>
<line class="d-line d-muted" x1="236" y1="420" x2="128.99" y2="490.23"/>
<line class="d-line d-muted" x1="236" y1="420" x2="122.96" y2="480.05"/>
<line class="d-line d-muted" x1="236" y1="420" x2="117.41" y2="468.18"/>
<line class="d-line d-muted" x1="236" y1="420" x2="112.78" y2="454.65"/>
<line class="d-line d-muted" x1="236" y1="420" x2="109.53" y2="439.76"/>
<line class="d-line d-muted" x1="236" y1="420" x2="108.25" y2="427.98"/>
<line class="d-line d-accent" x1="236" y1="420" x2="108" y2="420"/>
<line class="d-line d-muted" x1="236" y1="420" x2="108.25" y2="412.02"/>
<line class="d-line d-muted" x1="236" y1="420" x2="109.53" y2="400.24"/>
<line class="d-line d-muted" x1="236" y1="420" x2="112.78" y2="385.35"/>
<line class="d-line d-muted" x1="236" y1="420" x2="117.41" y2="371.82"/>
<line class="d-line d-muted" x1="236" y1="420" x2="122.96" y2="359.95"/>
<line class="d-line d-muted" x1="236" y1="420" x2="128.99" y2="349.77"/>
<line class="d-line d-muted" x1="236" y1="420" x2="136.66" y2="339.28"/>
<line class="d-line d-accent" x1="236" y1="420" x2="145.49" y2="329.49"/>
<line class="d-line d-muted" x1="236" y1="420" x2="155.28" y2="320.66"/>
<line class="d-line d-muted" x1="236" y1="420" x2="165.77" y2="312.99"/>
<line class="d-line d-muted" x1="236" y1="420" x2="175.95" y2="306.96"/>
<line class="d-line d-muted" x1="236" y1="420" x2="187.82" y2="301.41"/>
<line class="d-line d-muted" x1="236" y1="420" x2="201.35" y2="296.78"/>
<line class="d-line d-muted" x1="236" y1="420" x2="216.24" y2="293.53"/>
<line class="d-line d-muted" x1="236" y1="420" x2="228.02" y2="292.25"/>
<line class="d-line d-accent" x1="236" y1="420" x2="236" y2="292"/>
<line class="d-line d-muted" x1="236" y1="420" x2="243.98" y2="292.25"/>
<line class="d-line d-muted" x1="236" y1="420" x2="255.76" y2="293.53"/>
<line class="d-line d-muted" x1="236" y1="420" x2="270.65" y2="296.78"/>
<line class="d-line d-muted" x1="236" y1="420" x2="284.18" y2="301.41"/>
<line class="d-line d-muted" x1="236" y1="420" x2="296.05" y2="306.96"/>
<line class="d-line d-muted" x1="236" y1="420" x2="306.23" y2="312.99"/>
<line class="d-line d-muted" x1="236" y1="420" x2="316.72" y2="320.66"/>
<line class="d-line d-accent" x1="236" y1="420" x2="326.51" y2="329.49"/>
<text class="d-mono" x="132.76" y="527.24" text-anchor="middle">2</text>
<text class="d-mono" x="90" y="424" text-anchor="middle">10</text>
<text class="d-mono" x="132.76" y="320.76" text-anchor="middle">18</text>
<text class="d-mono" x="236" y="278" text-anchor="middle">26</text>
<text class="d-mono" x="339.24" y="320.76" text-anchor="middle">34</text>
<circle class="d-accent" cx="236" cy="420" r="4"/>
<text class="d-label" x="74" y="424" text-anchor="end">horizontal</text>
<text class="d-label" x="236" y="258" text-anchor="middle">vertical</text>
<text class="d-label" x="180" y="556" text-anchor="middle">33 angular modes, numbered 2 to 34</text>
<rect class="d-fill" x="70" y="570" width="100" height="30" rx="6"/>
<text class="d-mono" x="120" y="589.5" text-anchor="middle">0  planar</text>
<rect class="d-fill" x="190" y="570" width="100" height="30" rx="6"/>
<text class="d-mono" x="240" y="589.5" text-anchor="middle">1  DC</text>
</svg>
<figcaption>Each angular mode copies the highlighted neighbor samples into the block along one of these directions. The fan is denser near horizontal and vertical.</figcaption>
</figure>

Naming one of 35 modes would cost five bits or more on every block if the standard didn't shorten it by building a list of three most probable modes from the blocks above and to the left. A mode on the list costs about two bits. Chroma can reuse the mode that luma chose.

## Inter prediction and merge

Inter prediction copies a block from an earlier decoded picture. A motion vector says where the copy comes from and a reference index says which picture. Vectors are precise to a quarter of a sample. Positions between whole samples are computed by a filter over eight neighboring samples for luma and four for chroma.

<figure class="diagram">
<svg class="d-wide" viewBox="0 0 720 262" role="img" aria-labelledby="mv-fig-t mv-fig-d">
<title id="mv-fig-t">A motion vector points at the block to copy</title>
<desc id="mv-fig-d">Two pictures side by side. In the picture being coded one block is highlighted. In the earlier decoded picture a dashed outline marks the same place, and a motion vector runs from it to a highlighted block down and to the left. That block is copied across as the prediction.</desc>
<text class="d-label" x="170" y="30" text-anchor="middle">an earlier decoded picture</text>
<text class="d-label" x="550" y="30" text-anchor="middle">the picture being coded</text>
<rect class="d-box" x="30" y="44" width="280" height="176" rx="0"/>
<rect class="d-box" x="410" y="44" width="280" height="176" rx="0"/>
<rect class="d-dash" x="150" y="84" width="50" height="50" rx="0"/>
<text class="d-label" x="175" y="76" text-anchor="middle">same place</text>
<rect class="d-accent" x="86" y="150" width="50" height="50" rx="0"/>
<line class="d-arrow d-accent" x1="175" y1="109" x2="116" y2="169"/>
<text class="d-label" x="138" y="128" text-anchor="end">motion vector</text>
<rect class="d-accent" x="530" y="84" width="50" height="50" rx="0"/>
<text class="d-label" x="555" y="76" text-anchor="middle">this block</text>
<line class="d-arrow" x1="140" y1="180" x2="524" y2="116"/>
<text class="d-label" x="430" y="176">copied as the prediction</text>
<text class="d-label" x="170" y="246" text-anchor="middle">the vector can stop between samples</text>
<text class="d-label" x="550" y="246" text-anchor="middle">only the leftover difference is coded</text>
</svg>
<svg class="d-narrow" viewBox="0 0 360 472" role="img" aria-labelledby="mv-fig-t-n mv-fig-d-n">
<title id="mv-fig-t-n">A motion vector points at the block to copy</title>
<desc id="mv-fig-d-n">Two pictures side by side. In the picture being coded one block is highlighted. In the earlier decoded picture a dashed outline marks the same place, and a motion vector runs from it to a highlighted block down and to the left. That block is copied across as the prediction.</desc>
<text class="d-label" x="180" y="18" text-anchor="middle">an earlier decoded picture</text>
<rect class="d-box" x="40" y="30" width="280" height="176" rx="0"/>
<rect class="d-dash" x="160" y="70" width="50" height="50" rx="0"/>
<text class="d-label" x="185" y="62" text-anchor="middle">same place</text>
<rect class="d-accent" x="96" y="136" width="50" height="50" rx="0"/>
<line class="d-arrow d-accent" x1="185" y1="95" x2="126" y2="155"/>
<text class="d-label" x="148" y="114" text-anchor="end">motion vector</text>
<text class="d-label" x="180" y="226" text-anchor="middle">the vector can stop between samples</text>
<text class="d-label" x="180" y="254" text-anchor="middle">the picture being coded</text>
<rect class="d-box" x="40" y="264" width="280" height="176" rx="0"/>
<rect class="d-accent" x="160" y="304" width="50" height="50" rx="0"/>
<text class="d-label" x="185" y="296" text-anchor="middle">this block</text>
<path class="d-arrow" d="M146 168 H338 V400 H185 V356"/>
<text class="d-label" x="330" y="420" text-anchor="end">copied as the prediction</text>
<text class="d-label" x="180" y="460" text-anchor="middle">only the leftover difference is coded</text>
</svg>
<figcaption>The stream holds the vector and which picture it points into. The decoder makes the same copy.</figcaption>
</figure>

HEVC has two ways to write the motion. Merge writes nothing but an index. The decoder builds a list of up to five candidates from the block's neighbors. The block takes one candidate's vector and reference whole. Skip mode is a merge with no residual, the cheapest block the standard can code. The other way is advanced motion vector prediction (AMVP), where the encoder picks one of two predicted vectors and codes the difference from it.

<figure class="diagram">
<svg class="d-wide" viewBox="0 0 720 300" role="img" aria-label="Where merge candidates come from">
<title>Where merge candidates come from</title>
<desc>A block with five marked neighbor positions: A1 and A0 at its lower left, B1 and B0 at its upper right, and B2 at its upper left corner. Beside it the block at the same place in an earlier picture has two temporal positions, below-right and center. The candidates fill a list of up to five entries and the stream holds only the index of the chosen one.</desc>
<rect class="d-box" x="70" y="76" width="150" height="150" rx="0"/>
<text class="d-label" x="145" y="147" text-anchor="middle">current block</text>
<text class="d-label" x="145" y="165" text-anchor="middle">(this picture)</text>
<rect class="d-accent" x="44" y="50" width="26" height="26" rx="0"/>
<text class="d-mono" x="57" y="67.5" text-anchor="middle">B2</text>
<rect class="d-accent" x="194" y="50" width="26" height="26" rx="0"/>
<text class="d-mono" x="207" y="67.5" text-anchor="middle">B1</text>
<rect class="d-accent" x="220" y="50" width="26" height="26" rx="0"/>
<text class="d-mono" x="233" y="67.5" text-anchor="middle">B0</text>
<rect class="d-accent" x="44" y="200" width="26" height="26" rx="0"/>
<text class="d-mono" x="57" y="217.5" text-anchor="middle">A1</text>
<rect class="d-accent" x="44" y="226" width="26" height="26" rx="0"/>
<text class="d-mono" x="57" y="243.5" text-anchor="middle">A0</text>
<rect class="d-dash" x="330" y="76" width="150" height="150" rx="0"/>
<text class="d-label" x="405" y="116" text-anchor="middle">same place in an</text>
<text class="d-label" x="405" y="134" text-anchor="middle">earlier picture</text>
<rect class="d-accent" x="480" y="226" width="26" height="26" rx="0"/>
<text class="d-mono" x="493" y="243.5" text-anchor="middle">T</text>
<rect class="d-fill" x="405" y="161" width="26" height="26" rx="0"/>
<text class="d-mono" x="418" y="178.5" text-anchor="middle">T</text>
<text class="d-label" x="405" y="272" text-anchor="middle">temporal candidate: below-right,</text>
<text class="d-label" x="405" y="290" text-anchor="middle">or the center if that's unusable</text>
<text class="d-label" x="132" y="272" text-anchor="middle">five spatial neighbors</text>
<rect class="d-box" x="590" y="70" width="96" height="28" rx="6"/>
<text class="d-label" x="638" y="88.5" text-anchor="middle">candidate 0</text>
<rect class="d-box" x="590" y="104" width="96" height="28" rx="6"/>
<text class="d-label" x="638" y="122.5" text-anchor="middle">candidate 1</text>
<rect class="d-accent" x="590" y="138" width="96" height="28" rx="6"/>
<text class="d-label" x="638" y="156.5" text-anchor="middle">candidate 2</text>
<rect class="d-box" x="590" y="172" width="96" height="28" rx="6"/>
<text class="d-label" x="638" y="190.5" text-anchor="middle">candidate 3</text>
<rect class="d-box" x="590" y="206" width="96" height="28" rx="6"/>
<text class="d-label" x="638" y="224.5" text-anchor="middle">candidate 4</text>
<text class="d-label" x="638" y="56" text-anchor="middle">merge list</text>
<line class="d-arrow" x1="510" y1="150" x2="582" y2="150"/>
<text class="d-label" x="548" y="140" text-anchor="middle">fill</text>
<text class="d-label" x="638" y="260" text-anchor="middle">the stream holds</text>
<text class="d-label" x="638" y="278" text-anchor="middle">one index, here 2</text>
</svg>
<svg class="d-narrow" viewBox="0 0 360 492" role="img" aria-labelledby="merge-fig-t-n merge-fig-d-n">
<title id="merge-fig-t-n">Where merge candidates come from</title>
<desc id="merge-fig-d-n">A block with five marked neighbor positions: A1 and A0 at its lower left, B1 and B0 at its upper right, and B2 at its upper left corner. Beside it the block at the same place in an earlier picture has two temporal positions, below-right and center. The candidates fill a list of up to five entries and the stream holds only the index of the chosen one.</desc>
<rect class="d-box" x="110" y="36" width="140" height="140" rx="0"/>
<text class="d-label" x="180" y="102" text-anchor="middle">current block</text>
<text class="d-label" x="180" y="120" text-anchor="middle">(this picture)</text>
<rect class="d-accent" x="84" y="10" width="26" height="26" rx="0"/>
<text class="d-mono" x="97" y="27.5" text-anchor="middle">B2</text>
<rect class="d-accent" x="224" y="10" width="26" height="26" rx="0"/>
<text class="d-mono" x="237" y="27.5" text-anchor="middle">B1</text>
<rect class="d-accent" x="250" y="10" width="26" height="26" rx="0"/>
<text class="d-mono" x="263" y="27.5" text-anchor="middle">B0</text>
<rect class="d-accent" x="84" y="150" width="26" height="26" rx="0"/>
<text class="d-mono" x="97" y="167.5" text-anchor="middle">A1</text>
<rect class="d-accent" x="84" y="176" width="26" height="26" rx="0"/>
<text class="d-mono" x="97" y="193.5" text-anchor="middle">A0</text>
<text class="d-label" x="180" y="226" text-anchor="middle">five spatial neighbors</text>
<rect class="d-dash" x="14" y="256" width="140" height="140" rx="0"/>
<text class="d-label" x="84" y="290" text-anchor="middle">same place in an</text>
<text class="d-label" x="84" y="308" text-anchor="middle">earlier picture</text>
<rect class="d-fill" x="76" y="328" width="26" height="26" rx="0"/>
<text class="d-mono" x="89" y="345.5" text-anchor="middle">T</text>
<rect class="d-accent" x="154" y="396" width="26" height="26" rx="0"/>
<text class="d-mono" x="167" y="413.5" text-anchor="middle">T</text>
<text class="d-label" x="92" y="444" text-anchor="middle">temporal candidate:</text>
<text class="d-label" x="92" y="462" text-anchor="middle">below-right, or the</text>
<text class="d-label" x="92" y="480" text-anchor="middle">center if that's unusable</text>
<text class="d-label" x="298" y="266" text-anchor="middle">merge list</text>
<rect class="d-box" x="250" y="276" width="96" height="28" rx="6"/>
<text class="d-label" x="298" y="294.5" text-anchor="middle">candidate 0</text>
<rect class="d-box" x="250" y="310" width="96" height="28" rx="6"/>
<text class="d-label" x="298" y="328.5" text-anchor="middle">candidate 1</text>
<rect class="d-accent" x="250" y="344" width="96" height="28" rx="6"/>
<text class="d-label" x="298" y="362.5" text-anchor="middle">candidate 2</text>
<rect class="d-box" x="250" y="378" width="96" height="28" rx="6"/>
<text class="d-label" x="298" y="396.5" text-anchor="middle">candidate 3</text>
<rect class="d-box" x="250" y="412" width="96" height="28" rx="6"/>
<text class="d-label" x="298" y="430.5" text-anchor="middle">candidate 4</text>
<line class="d-arrow" x1="186" y1="358" x2="242" y2="358"/>
<text class="d-label" x="214" y="348" text-anchor="middle">fill</text>
<text class="d-label" x="298" y="462" text-anchor="middle">the stream holds</text>
<text class="d-label" x="298" y="480" text-anchor="middle">one index, here 2</text>
</svg>
<figcaption>Merge copies a whole motion choice from a neighbor. The block costs one small index and no motion vector.</figcaption>
</figure>

A block in a B picture can average a prediction from each of its two reference lists. Weighted prediction scales a reference's brightness first so that a fade predicts well.

## Transforms and quantization

The residual is transformed in square blocks from 4x4 to 32x32 samples. The transform is an integer version of the discrete cosine transform, apart from one case. A 4x4 intra luma block uses a sine transform because its residual grows with distance from the predicting edge.

The quantizer's step size doubles every six QP steps. At 8 bits the QP runs from 0 to 51. Because the QP can change from block to block inside a picture, an encoder can spend bits where the eye will see them.

<figure class="diagram">
<svg class="d-wide" viewBox="0 0 720 250" role="img" aria-label="What the transform and the quantizer do to an 8x8 block">
<title>What the transform and the quantizer do to an 8x8 block</title>
<desc>Three 8 by 8 grids. In the first every cell is filled: a residual has a value at every sample. In the second the filled cells collect in the top left corner: after the transform most of the energy sits in a few low frequencies. In the third only five cells in the top left hold numbers and the other 59 are zero: quantization rounded the small values away.</desc>
<rect class="d-fill" x="40" y="50" width="20" height="20" rx="0"/>
<rect class="d-fill" x="60" y="50" width="20" height="20" rx="0"/>
<rect class="d-fill" x="80" y="50" width="20" height="20" rx="0"/>
<rect class="d-fill" x="100" y="50" width="20" height="20" rx="0"/>
<rect class="d-fill" x="120" y="50" width="20" height="20" rx="0"/>
<rect class="d-fill" x="140" y="50" width="20" height="20" rx="0"/>
<rect class="d-fill" x="160" y="50" width="20" height="20" rx="0"/>
<rect class="d-fill" x="180" y="50" width="20" height="20" rx="0"/>
<rect class="d-fill" x="40" y="70" width="20" height="20" rx="0"/>
<rect class="d-fill" x="60" y="70" width="20" height="20" rx="0"/>
<rect class="d-fill" x="80" y="70" width="20" height="20" rx="0"/>
<rect class="d-fill" x="100" y="70" width="20" height="20" rx="0"/>
<rect class="d-fill" x="120" y="70" width="20" height="20" rx="0"/>
<rect class="d-fill" x="140" y="70" width="20" height="20" rx="0"/>
<rect class="d-fill" x="160" y="70" width="20" height="20" rx="0"/>
<rect class="d-fill" x="180" y="70" width="20" height="20" rx="0"/>
<rect class="d-fill" x="40" y="90" width="20" height="20" rx="0"/>
<rect class="d-fill" x="60" y="90" width="20" height="20" rx="0"/>
<rect class="d-fill" x="80" y="90" width="20" height="20" rx="0"/>
<rect class="d-fill" x="100" y="90" width="20" height="20" rx="0"/>
<rect class="d-fill" x="120" y="90" width="20" height="20" rx="0"/>
<rect class="d-fill" x="140" y="90" width="20" height="20" rx="0"/>
<rect class="d-fill" x="160" y="90" width="20" height="20" rx="0"/>
<rect class="d-fill" x="180" y="90" width="20" height="20" rx="0"/>
<rect class="d-fill" x="40" y="110" width="20" height="20" rx="0"/>
<rect class="d-fill" x="60" y="110" width="20" height="20" rx="0"/>
<rect class="d-fill" x="80" y="110" width="20" height="20" rx="0"/>
<rect class="d-fill" x="100" y="110" width="20" height="20" rx="0"/>
<rect class="d-fill" x="120" y="110" width="20" height="20" rx="0"/>
<rect class="d-fill" x="140" y="110" width="20" height="20" rx="0"/>
<rect class="d-fill" x="160" y="110" width="20" height="20" rx="0"/>
<rect class="d-fill" x="180" y="110" width="20" height="20" rx="0"/>
<rect class="d-fill" x="40" y="130" width="20" height="20" rx="0"/>
<rect class="d-fill" x="60" y="130" width="20" height="20" rx="0"/>
<rect class="d-fill" x="80" y="130" width="20" height="20" rx="0"/>
<rect class="d-fill" x="100" y="130" width="20" height="20" rx="0"/>
<rect class="d-fill" x="120" y="130" width="20" height="20" rx="0"/>
<rect class="d-fill" x="140" y="130" width="20" height="20" rx="0"/>
<rect class="d-fill" x="160" y="130" width="20" height="20" rx="0"/>
<rect class="d-fill" x="180" y="130" width="20" height="20" rx="0"/>
<rect class="d-fill" x="40" y="150" width="20" height="20" rx="0"/>
<rect class="d-fill" x="60" y="150" width="20" height="20" rx="0"/>
<rect class="d-fill" x="80" y="150" width="20" height="20" rx="0"/>
<rect class="d-fill" x="100" y="150" width="20" height="20" rx="0"/>
<rect class="d-fill" x="120" y="150" width="20" height="20" rx="0"/>
<rect class="d-fill" x="140" y="150" width="20" height="20" rx="0"/>
<rect class="d-fill" x="160" y="150" width="20" height="20" rx="0"/>
<rect class="d-fill" x="180" y="150" width="20" height="20" rx="0"/>
<rect class="d-fill" x="40" y="170" width="20" height="20" rx="0"/>
<rect class="d-fill" x="60" y="170" width="20" height="20" rx="0"/>
<rect class="d-fill" x="80" y="170" width="20" height="20" rx="0"/>
<rect class="d-fill" x="100" y="170" width="20" height="20" rx="0"/>
<rect class="d-fill" x="120" y="170" width="20" height="20" rx="0"/>
<rect class="d-fill" x="140" y="170" width="20" height="20" rx="0"/>
<rect class="d-fill" x="160" y="170" width="20" height="20" rx="0"/>
<rect class="d-fill" x="180" y="170" width="20" height="20" rx="0"/>
<rect class="d-fill" x="40" y="190" width="20" height="20" rx="0"/>
<rect class="d-fill" x="60" y="190" width="20" height="20" rx="0"/>
<rect class="d-fill" x="80" y="190" width="20" height="20" rx="0"/>
<rect class="d-fill" x="100" y="190" width="20" height="20" rx="0"/>
<rect class="d-fill" x="120" y="190" width="20" height="20" rx="0"/>
<rect class="d-fill" x="140" y="190" width="20" height="20" rx="0"/>
<rect class="d-fill" x="160" y="190" width="20" height="20" rx="0"/>
<rect class="d-fill" x="180" y="190" width="20" height="20" rx="0"/>
<rect class="d-accent" x="280" y="50" width="20" height="20" rx="0"/>
<rect class="d-accent" x="300" y="50" width="20" height="20" rx="0"/>
<rect class="d-accent" x="320" y="50" width="20" height="20" rx="0"/>
<rect class="d-fill" x="340" y="50" width="20" height="20" rx="0"/>
<rect class="d-fill" x="360" y="50" width="20" height="20" rx="0"/>
<rect class="d-fill" x="380" y="50" width="20" height="20" rx="0"/>
<rect class="d-muted" x="400" y="50" width="20" height="20" rx="0"/>
<rect class="d-muted" x="420" y="50" width="20" height="20" rx="0"/>
<rect class="d-accent" x="280" y="70" width="20" height="20" rx="0"/>
<rect class="d-accent" x="300" y="70" width="20" height="20" rx="0"/>
<rect class="d-fill" x="320" y="70" width="20" height="20" rx="0"/>
<rect class="d-fill" x="340" y="70" width="20" height="20" rx="0"/>
<rect class="d-fill" x="360" y="70" width="20" height="20" rx="0"/>
<rect class="d-muted" x="380" y="70" width="20" height="20" rx="0"/>
<rect class="d-muted" x="400" y="70" width="20" height="20" rx="0"/>
<rect class="d-muted" x="420" y="70" width="20" height="20" rx="0"/>
<rect class="d-accent" x="280" y="90" width="20" height="20" rx="0"/>
<rect class="d-fill" x="300" y="90" width="20" height="20" rx="0"/>
<rect class="d-fill" x="320" y="90" width="20" height="20" rx="0"/>
<rect class="d-fill" x="340" y="90" width="20" height="20" rx="0"/>
<rect class="d-muted" x="360" y="90" width="20" height="20" rx="0"/>
<rect class="d-muted" x="380" y="90" width="20" height="20" rx="0"/>
<rect class="d-muted" x="400" y="90" width="20" height="20" rx="0"/>
<rect class="d-muted" x="420" y="90" width="20" height="20" rx="0"/>
<rect class="d-fill" x="280" y="110" width="20" height="20" rx="0"/>
<rect class="d-fill" x="300" y="110" width="20" height="20" rx="0"/>
<rect class="d-fill" x="320" y="110" width="20" height="20" rx="0"/>
<rect class="d-muted" x="340" y="110" width="20" height="20" rx="0"/>
<rect class="d-muted" x="360" y="110" width="20" height="20" rx="0"/>
<rect class="d-muted" x="380" y="110" width="20" height="20" rx="0"/>
<rect class="d-muted" x="400" y="110" width="20" height="20" rx="0"/>
<rect class="d-muted" x="420" y="110" width="20" height="20" rx="0"/>
<rect class="d-fill" x="280" y="130" width="20" height="20" rx="0"/>
<rect class="d-fill" x="300" y="130" width="20" height="20" rx="0"/>
<rect class="d-muted" x="320" y="130" width="20" height="20" rx="0"/>
<rect class="d-muted" x="340" y="130" width="20" height="20" rx="0"/>
<rect class="d-muted" x="360" y="130" width="20" height="20" rx="0"/>
<rect class="d-muted" x="380" y="130" width="20" height="20" rx="0"/>
<rect class="d-muted" x="400" y="130" width="20" height="20" rx="0"/>
<rect class="d-muted" x="420" y="130" width="20" height="20" rx="0"/>
<rect class="d-fill" x="280" y="150" width="20" height="20" rx="0"/>
<rect class="d-muted" x="300" y="150" width="20" height="20" rx="0"/>
<rect class="d-muted" x="320" y="150" width="20" height="20" rx="0"/>
<rect class="d-muted" x="340" y="150" width="20" height="20" rx="0"/>
<rect class="d-muted" x="360" y="150" width="20" height="20" rx="0"/>
<rect class="d-muted" x="380" y="150" width="20" height="20" rx="0"/>
<rect class="d-muted" x="400" y="150" width="20" height="20" rx="0"/>
<rect class="d-muted" x="420" y="150" width="20" height="20" rx="0"/>
<rect class="d-muted" x="280" y="170" width="20" height="20" rx="0"/>
<rect class="d-muted" x="300" y="170" width="20" height="20" rx="0"/>
<rect class="d-muted" x="320" y="170" width="20" height="20" rx="0"/>
<rect class="d-muted" x="340" y="170" width="20" height="20" rx="0"/>
<rect class="d-muted" x="360" y="170" width="20" height="20" rx="0"/>
<rect class="d-muted" x="380" y="170" width="20" height="20" rx="0"/>
<rect class="d-muted" x="400" y="170" width="20" height="20" rx="0"/>
<rect class="d-muted" x="420" y="170" width="20" height="20" rx="0"/>
<rect class="d-muted" x="280" y="190" width="20" height="20" rx="0"/>
<rect class="d-muted" x="300" y="190" width="20" height="20" rx="0"/>
<rect class="d-muted" x="320" y="190" width="20" height="20" rx="0"/>
<rect class="d-muted" x="340" y="190" width="20" height="20" rx="0"/>
<rect class="d-muted" x="360" y="190" width="20" height="20" rx="0"/>
<rect class="d-muted" x="380" y="190" width="20" height="20" rx="0"/>
<rect class="d-muted" x="400" y="190" width="20" height="20" rx="0"/>
<rect class="d-muted" x="420" y="190" width="20" height="20" rx="0"/>
<rect class="d-accent" x="520" y="50" width="20" height="20" rx="0"/>
<text class="d-mono" x="530" y="64.5" text-anchor="middle">9</text>
<rect class="d-accent" x="540" y="50" width="20" height="20" rx="0"/>
<text class="d-mono" x="550" y="64.5" text-anchor="middle">-3</text>
<rect class="d-accent" x="560" y="50" width="20" height="20" rx="0"/>
<text class="d-mono" x="570" y="64.5" text-anchor="middle">1</text>
<rect class="d-box" x="580" y="50" width="20" height="20" rx="0"/>
<rect class="d-box" x="600" y="50" width="20" height="20" rx="0"/>
<rect class="d-box" x="620" y="50" width="20" height="20" rx="0"/>
<rect class="d-box" x="640" y="50" width="20" height="20" rx="0"/>
<rect class="d-box" x="660" y="50" width="20" height="20" rx="0"/>
<rect class="d-accent" x="520" y="70" width="20" height="20" rx="0"/>
<text class="d-mono" x="530" y="84.5" text-anchor="middle">2</text>
<rect class="d-accent" x="540" y="70" width="20" height="20" rx="0"/>
<text class="d-mono" x="550" y="84.5" text-anchor="middle">-1</text>
<rect class="d-box" x="560" y="70" width="20" height="20" rx="0"/>
<rect class="d-box" x="580" y="70" width="20" height="20" rx="0"/>
<rect class="d-box" x="600" y="70" width="20" height="20" rx="0"/>
<rect class="d-box" x="620" y="70" width="20" height="20" rx="0"/>
<rect class="d-box" x="640" y="70" width="20" height="20" rx="0"/>
<rect class="d-box" x="660" y="70" width="20" height="20" rx="0"/>
<rect class="d-box" x="520" y="90" width="20" height="20" rx="0"/>
<rect class="d-box" x="540" y="90" width="20" height="20" rx="0"/>
<rect class="d-box" x="560" y="90" width="20" height="20" rx="0"/>
<rect class="d-box" x="580" y="90" width="20" height="20" rx="0"/>
<rect class="d-box" x="600" y="90" width="20" height="20" rx="0"/>
<rect class="d-box" x="620" y="90" width="20" height="20" rx="0"/>
<rect class="d-box" x="640" y="90" width="20" height="20" rx="0"/>
<rect class="d-box" x="660" y="90" width="20" height="20" rx="0"/>
<rect class="d-box" x="520" y="110" width="20" height="20" rx="0"/>
<rect class="d-box" x="540" y="110" width="20" height="20" rx="0"/>
<rect class="d-box" x="560" y="110" width="20" height="20" rx="0"/>
<rect class="d-box" x="580" y="110" width="20" height="20" rx="0"/>
<rect class="d-box" x="600" y="110" width="20" height="20" rx="0"/>
<rect class="d-box" x="620" y="110" width="20" height="20" rx="0"/>
<rect class="d-box" x="640" y="110" width="20" height="20" rx="0"/>
<rect class="d-box" x="660" y="110" width="20" height="20" rx="0"/>
<rect class="d-box" x="520" y="130" width="20" height="20" rx="0"/>
<rect class="d-box" x="540" y="130" width="20" height="20" rx="0"/>
<rect class="d-box" x="560" y="130" width="20" height="20" rx="0"/>
<rect class="d-box" x="580" y="130" width="20" height="20" rx="0"/>
<rect class="d-box" x="600" y="130" width="20" height="20" rx="0"/>
<rect class="d-box" x="620" y="130" width="20" height="20" rx="0"/>
<rect class="d-box" x="640" y="130" width="20" height="20" rx="0"/>
<rect class="d-box" x="660" y="130" width="20" height="20" rx="0"/>
<rect class="d-box" x="520" y="150" width="20" height="20" rx="0"/>
<rect class="d-box" x="540" y="150" width="20" height="20" rx="0"/>
<rect class="d-box" x="560" y="150" width="20" height="20" rx="0"/>
<rect class="d-box" x="580" y="150" width="20" height="20" rx="0"/>
<rect class="d-box" x="600" y="150" width="20" height="20" rx="0"/>
<rect class="d-box" x="620" y="150" width="20" height="20" rx="0"/>
<rect class="d-box" x="640" y="150" width="20" height="20" rx="0"/>
<rect class="d-box" x="660" y="150" width="20" height="20" rx="0"/>
<rect class="d-box" x="520" y="170" width="20" height="20" rx="0"/>
<rect class="d-box" x="540" y="170" width="20" height="20" rx="0"/>
<rect class="d-box" x="560" y="170" width="20" height="20" rx="0"/>
<rect class="d-box" x="580" y="170" width="20" height="20" rx="0"/>
<rect class="d-box" x="600" y="170" width="20" height="20" rx="0"/>
<rect class="d-box" x="620" y="170" width="20" height="20" rx="0"/>
<rect class="d-box" x="640" y="170" width="20" height="20" rx="0"/>
<rect class="d-box" x="660" y="170" width="20" height="20" rx="0"/>
<rect class="d-box" x="520" y="190" width="20" height="20" rx="0"/>
<rect class="d-box" x="540" y="190" width="20" height="20" rx="0"/>
<rect class="d-box" x="560" y="190" width="20" height="20" rx="0"/>
<rect class="d-box" x="580" y="190" width="20" height="20" rx="0"/>
<rect class="d-box" x="600" y="190" width="20" height="20" rx="0"/>
<rect class="d-box" x="620" y="190" width="20" height="20" rx="0"/>
<rect class="d-box" x="640" y="190" width="20" height="20" rx="0"/>
<rect class="d-box" x="660" y="190" width="20" height="20" rx="0"/>
<line class="d-arrow" x1="206" y1="130" x2="272" y2="130"/>
<text class="d-label" x="239" y="120" text-anchor="middle">transform</text>
<line class="d-arrow" x1="446" y1="130" x2="512" y2="130"/>
<text class="d-label" x="479" y="120" text-anchor="middle">quantize</text>
<text class="d-label" x="120" y="34" text-anchor="middle">residual samples</text>
<text class="d-label" x="360" y="34" text-anchor="middle">frequency coefficients</text>
<text class="d-label" x="600" y="34" text-anchor="middle">after rounding</text>
<text class="d-label" x="120" y="234" text-anchor="middle">every sample holds something</text>
<text class="d-label" x="360" y="234" text-anchor="middle">the energy collects at top left</text>
<text class="d-label" x="600" y="234" text-anchor="middle">five values left, the rest are zero</text>
</svg>
<svg class="d-narrow" viewBox="0 0 360 580" role="img" aria-labelledby="xform-fig-t-n xform-fig-d-n">
<title id="xform-fig-t-n">What the transform and the quantizer do to an 8x8 block</title>
<desc id="xform-fig-d-n">Three 8 by 8 grids. In the first every cell is filled: a residual has a value at every sample. In the second the filled cells collect in the top left corner: after the transform most of the energy sits in a few low frequencies. In the third only five cells in the top left hold numbers and the other 59 are zero: quantization rounded the small values away.</desc>
<rect class="d-fill" x="12" y="10" width="20" height="20" rx="0"/>
<rect class="d-fill" x="32" y="10" width="20" height="20" rx="0"/>
<rect class="d-fill" x="52" y="10" width="20" height="20" rx="0"/>
<rect class="d-fill" x="72" y="10" width="20" height="20" rx="0"/>
<rect class="d-fill" x="92" y="10" width="20" height="20" rx="0"/>
<rect class="d-fill" x="112" y="10" width="20" height="20" rx="0"/>
<rect class="d-fill" x="132" y="10" width="20" height="20" rx="0"/>
<rect class="d-fill" x="152" y="10" width="20" height="20" rx="0"/>
<rect class="d-fill" x="12" y="30" width="20" height="20" rx="0"/>
<rect class="d-fill" x="32" y="30" width="20" height="20" rx="0"/>
<rect class="d-fill" x="52" y="30" width="20" height="20" rx="0"/>
<rect class="d-fill" x="72" y="30" width="20" height="20" rx="0"/>
<rect class="d-fill" x="92" y="30" width="20" height="20" rx="0"/>
<rect class="d-fill" x="112" y="30" width="20" height="20" rx="0"/>
<rect class="d-fill" x="132" y="30" width="20" height="20" rx="0"/>
<rect class="d-fill" x="152" y="30" width="20" height="20" rx="0"/>
<rect class="d-fill" x="12" y="50" width="20" height="20" rx="0"/>
<rect class="d-fill" x="32" y="50" width="20" height="20" rx="0"/>
<rect class="d-fill" x="52" y="50" width="20" height="20" rx="0"/>
<rect class="d-fill" x="72" y="50" width="20" height="20" rx="0"/>
<rect class="d-fill" x="92" y="50" width="20" height="20" rx="0"/>
<rect class="d-fill" x="112" y="50" width="20" height="20" rx="0"/>
<rect class="d-fill" x="132" y="50" width="20" height="20" rx="0"/>
<rect class="d-fill" x="152" y="50" width="20" height="20" rx="0"/>
<rect class="d-fill" x="12" y="70" width="20" height="20" rx="0"/>
<rect class="d-fill" x="32" y="70" width="20" height="20" rx="0"/>
<rect class="d-fill" x="52" y="70" width="20" height="20" rx="0"/>
<rect class="d-fill" x="72" y="70" width="20" height="20" rx="0"/>
<rect class="d-fill" x="92" y="70" width="20" height="20" rx="0"/>
<rect class="d-fill" x="112" y="70" width="20" height="20" rx="0"/>
<rect class="d-fill" x="132" y="70" width="20" height="20" rx="0"/>
<rect class="d-fill" x="152" y="70" width="20" height="20" rx="0"/>
<rect class="d-fill" x="12" y="90" width="20" height="20" rx="0"/>
<rect class="d-fill" x="32" y="90" width="20" height="20" rx="0"/>
<rect class="d-fill" x="52" y="90" width="20" height="20" rx="0"/>
<rect class="d-fill" x="72" y="90" width="20" height="20" rx="0"/>
<rect class="d-fill" x="92" y="90" width="20" height="20" rx="0"/>
<rect class="d-fill" x="112" y="90" width="20" height="20" rx="0"/>
<rect class="d-fill" x="132" y="90" width="20" height="20" rx="0"/>
<rect class="d-fill" x="152" y="90" width="20" height="20" rx="0"/>
<rect class="d-fill" x="12" y="110" width="20" height="20" rx="0"/>
<rect class="d-fill" x="32" y="110" width="20" height="20" rx="0"/>
<rect class="d-fill" x="52" y="110" width="20" height="20" rx="0"/>
<rect class="d-fill" x="72" y="110" width="20" height="20" rx="0"/>
<rect class="d-fill" x="92" y="110" width="20" height="20" rx="0"/>
<rect class="d-fill" x="112" y="110" width="20" height="20" rx="0"/>
<rect class="d-fill" x="132" y="110" width="20" height="20" rx="0"/>
<rect class="d-fill" x="152" y="110" width="20" height="20" rx="0"/>
<rect class="d-fill" x="12" y="130" width="20" height="20" rx="0"/>
<rect class="d-fill" x="32" y="130" width="20" height="20" rx="0"/>
<rect class="d-fill" x="52" y="130" width="20" height="20" rx="0"/>
<rect class="d-fill" x="72" y="130" width="20" height="20" rx="0"/>
<rect class="d-fill" x="92" y="130" width="20" height="20" rx="0"/>
<rect class="d-fill" x="112" y="130" width="20" height="20" rx="0"/>
<rect class="d-fill" x="132" y="130" width="20" height="20" rx="0"/>
<rect class="d-fill" x="152" y="130" width="20" height="20" rx="0"/>
<rect class="d-fill" x="12" y="150" width="20" height="20" rx="0"/>
<rect class="d-fill" x="32" y="150" width="20" height="20" rx="0"/>
<rect class="d-fill" x="52" y="150" width="20" height="20" rx="0"/>
<rect class="d-fill" x="72" y="150" width="20" height="20" rx="0"/>
<rect class="d-fill" x="92" y="150" width="20" height="20" rx="0"/>
<rect class="d-fill" x="112" y="150" width="20" height="20" rx="0"/>
<rect class="d-fill" x="132" y="150" width="20" height="20" rx="0"/>
<rect class="d-fill" x="152" y="150" width="20" height="20" rx="0"/>
<rect class="d-accent" x="12" y="210" width="20" height="20" rx="0"/>
<rect class="d-accent" x="32" y="210" width="20" height="20" rx="0"/>
<rect class="d-accent" x="52" y="210" width="20" height="20" rx="0"/>
<rect class="d-fill" x="72" y="210" width="20" height="20" rx="0"/>
<rect class="d-fill" x="92" y="210" width="20" height="20" rx="0"/>
<rect class="d-fill" x="112" y="210" width="20" height="20" rx="0"/>
<rect class="d-muted" x="132" y="210" width="20" height="20" rx="0"/>
<rect class="d-muted" x="152" y="210" width="20" height="20" rx="0"/>
<rect class="d-accent" x="12" y="230" width="20" height="20" rx="0"/>
<rect class="d-accent" x="32" y="230" width="20" height="20" rx="0"/>
<rect class="d-fill" x="52" y="230" width="20" height="20" rx="0"/>
<rect class="d-fill" x="72" y="230" width="20" height="20" rx="0"/>
<rect class="d-fill" x="92" y="230" width="20" height="20" rx="0"/>
<rect class="d-muted" x="112" y="230" width="20" height="20" rx="0"/>
<rect class="d-muted" x="132" y="230" width="20" height="20" rx="0"/>
<rect class="d-muted" x="152" y="230" width="20" height="20" rx="0"/>
<rect class="d-accent" x="12" y="250" width="20" height="20" rx="0"/>
<rect class="d-fill" x="32" y="250" width="20" height="20" rx="0"/>
<rect class="d-fill" x="52" y="250" width="20" height="20" rx="0"/>
<rect class="d-fill" x="72" y="250" width="20" height="20" rx="0"/>
<rect class="d-muted" x="92" y="250" width="20" height="20" rx="0"/>
<rect class="d-muted" x="112" y="250" width="20" height="20" rx="0"/>
<rect class="d-muted" x="132" y="250" width="20" height="20" rx="0"/>
<rect class="d-muted" x="152" y="250" width="20" height="20" rx="0"/>
<rect class="d-fill" x="12" y="270" width="20" height="20" rx="0"/>
<rect class="d-fill" x="32" y="270" width="20" height="20" rx="0"/>
<rect class="d-fill" x="52" y="270" width="20" height="20" rx="0"/>
<rect class="d-muted" x="72" y="270" width="20" height="20" rx="0"/>
<rect class="d-muted" x="92" y="270" width="20" height="20" rx="0"/>
<rect class="d-muted" x="112" y="270" width="20" height="20" rx="0"/>
<rect class="d-muted" x="132" y="270" width="20" height="20" rx="0"/>
<rect class="d-muted" x="152" y="270" width="20" height="20" rx="0"/>
<rect class="d-fill" x="12" y="290" width="20" height="20" rx="0"/>
<rect class="d-fill" x="32" y="290" width="20" height="20" rx="0"/>
<rect class="d-muted" x="52" y="290" width="20" height="20" rx="0"/>
<rect class="d-muted" x="72" y="290" width="20" height="20" rx="0"/>
<rect class="d-muted" x="92" y="290" width="20" height="20" rx="0"/>
<rect class="d-muted" x="112" y="290" width="20" height="20" rx="0"/>
<rect class="d-muted" x="132" y="290" width="20" height="20" rx="0"/>
<rect class="d-muted" x="152" y="290" width="20" height="20" rx="0"/>
<rect class="d-fill" x="12" y="310" width="20" height="20" rx="0"/>
<rect class="d-muted" x="32" y="310" width="20" height="20" rx="0"/>
<rect class="d-muted" x="52" y="310" width="20" height="20" rx="0"/>
<rect class="d-muted" x="72" y="310" width="20" height="20" rx="0"/>
<rect class="d-muted" x="92" y="310" width="20" height="20" rx="0"/>
<rect class="d-muted" x="112" y="310" width="20" height="20" rx="0"/>
<rect class="d-muted" x="132" y="310" width="20" height="20" rx="0"/>
<rect class="d-muted" x="152" y="310" width="20" height="20" rx="0"/>
<rect class="d-muted" x="12" y="330" width="20" height="20" rx="0"/>
<rect class="d-muted" x="32" y="330" width="20" height="20" rx="0"/>
<rect class="d-muted" x="52" y="330" width="20" height="20" rx="0"/>
<rect class="d-muted" x="72" y="330" width="20" height="20" rx="0"/>
<rect class="d-muted" x="92" y="330" width="20" height="20" rx="0"/>
<rect class="d-muted" x="112" y="330" width="20" height="20" rx="0"/>
<rect class="d-muted" x="132" y="330" width="20" height="20" rx="0"/>
<rect class="d-muted" x="152" y="330" width="20" height="20" rx="0"/>
<rect class="d-muted" x="12" y="350" width="20" height="20" rx="0"/>
<rect class="d-muted" x="32" y="350" width="20" height="20" rx="0"/>
<rect class="d-muted" x="52" y="350" width="20" height="20" rx="0"/>
<rect class="d-muted" x="72" y="350" width="20" height="20" rx="0"/>
<rect class="d-muted" x="92" y="350" width="20" height="20" rx="0"/>
<rect class="d-muted" x="112" y="350" width="20" height="20" rx="0"/>
<rect class="d-muted" x="132" y="350" width="20" height="20" rx="0"/>
<rect class="d-muted" x="152" y="350" width="20" height="20" rx="0"/>
<rect class="d-accent" x="12" y="410" width="20" height="20" rx="0"/>
<rect class="d-accent" x="32" y="410" width="20" height="20" rx="0"/>
<rect class="d-accent" x="52" y="410" width="20" height="20" rx="0"/>
<rect class="d-box" x="72" y="410" width="20" height="20" rx="0"/>
<rect class="d-box" x="92" y="410" width="20" height="20" rx="0"/>
<rect class="d-box" x="112" y="410" width="20" height="20" rx="0"/>
<rect class="d-box" x="132" y="410" width="20" height="20" rx="0"/>
<rect class="d-box" x="152" y="410" width="20" height="20" rx="0"/>
<rect class="d-accent" x="12" y="430" width="20" height="20" rx="0"/>
<rect class="d-accent" x="32" y="430" width="20" height="20" rx="0"/>
<rect class="d-box" x="52" y="430" width="20" height="20" rx="0"/>
<rect class="d-box" x="72" y="430" width="20" height="20" rx="0"/>
<rect class="d-box" x="92" y="430" width="20" height="20" rx="0"/>
<rect class="d-box" x="112" y="430" width="20" height="20" rx="0"/>
<rect class="d-box" x="132" y="430" width="20" height="20" rx="0"/>
<rect class="d-box" x="152" y="430" width="20" height="20" rx="0"/>
<rect class="d-box" x="12" y="450" width="20" height="20" rx="0"/>
<rect class="d-box" x="32" y="450" width="20" height="20" rx="0"/>
<rect class="d-box" x="52" y="450" width="20" height="20" rx="0"/>
<rect class="d-box" x="72" y="450" width="20" height="20" rx="0"/>
<rect class="d-box" x="92" y="450" width="20" height="20" rx="0"/>
<rect class="d-box" x="112" y="450" width="20" height="20" rx="0"/>
<rect class="d-box" x="132" y="450" width="20" height="20" rx="0"/>
<rect class="d-box" x="152" y="450" width="20" height="20" rx="0"/>
<rect class="d-box" x="12" y="470" width="20" height="20" rx="0"/>
<rect class="d-box" x="32" y="470" width="20" height="20" rx="0"/>
<rect class="d-box" x="52" y="470" width="20" height="20" rx="0"/>
<rect class="d-box" x="72" y="470" width="20" height="20" rx="0"/>
<rect class="d-box" x="92" y="470" width="20" height="20" rx="0"/>
<rect class="d-box" x="112" y="470" width="20" height="20" rx="0"/>
<rect class="d-box" x="132" y="470" width="20" height="20" rx="0"/>
<rect class="d-box" x="152" y="470" width="20" height="20" rx="0"/>
<rect class="d-box" x="12" y="490" width="20" height="20" rx="0"/>
<rect class="d-box" x="32" y="490" width="20" height="20" rx="0"/>
<rect class="d-box" x="52" y="490" width="20" height="20" rx="0"/>
<rect class="d-box" x="72" y="490" width="20" height="20" rx="0"/>
<rect class="d-box" x="92" y="490" width="20" height="20" rx="0"/>
<rect class="d-box" x="112" y="490" width="20" height="20" rx="0"/>
<rect class="d-box" x="132" y="490" width="20" height="20" rx="0"/>
<rect class="d-box" x="152" y="490" width="20" height="20" rx="0"/>
<rect class="d-box" x="12" y="510" width="20" height="20" rx="0"/>
<rect class="d-box" x="32" y="510" width="20" height="20" rx="0"/>
<rect class="d-box" x="52" y="510" width="20" height="20" rx="0"/>
<rect class="d-box" x="72" y="510" width="20" height="20" rx="0"/>
<rect class="d-box" x="92" y="510" width="20" height="20" rx="0"/>
<rect class="d-box" x="112" y="510" width="20" height="20" rx="0"/>
<rect class="d-box" x="132" y="510" width="20" height="20" rx="0"/>
<rect class="d-box" x="152" y="510" width="20" height="20" rx="0"/>
<rect class="d-box" x="12" y="530" width="20" height="20" rx="0"/>
<rect class="d-box" x="32" y="530" width="20" height="20" rx="0"/>
<rect class="d-box" x="52" y="530" width="20" height="20" rx="0"/>
<rect class="d-box" x="72" y="530" width="20" height="20" rx="0"/>
<rect class="d-box" x="92" y="530" width="20" height="20" rx="0"/>
<rect class="d-box" x="112" y="530" width="20" height="20" rx="0"/>
<rect class="d-box" x="132" y="530" width="20" height="20" rx="0"/>
<rect class="d-box" x="152" y="530" width="20" height="20" rx="0"/>
<rect class="d-box" x="12" y="550" width="20" height="20" rx="0"/>
<rect class="d-box" x="32" y="550" width="20" height="20" rx="0"/>
<rect class="d-box" x="52" y="550" width="20" height="20" rx="0"/>
<rect class="d-box" x="72" y="550" width="20" height="20" rx="0"/>
<rect class="d-box" x="92" y="550" width="20" height="20" rx="0"/>
<rect class="d-box" x="112" y="550" width="20" height="20" rx="0"/>
<rect class="d-box" x="132" y="550" width="20" height="20" rx="0"/>
<rect class="d-box" x="152" y="550" width="20" height="20" rx="0"/>
<text class="d-mono" x="22" y="424.5" text-anchor="middle">9</text>
<text class="d-mono" x="42" y="424.5" text-anchor="middle">-3</text>
<text class="d-mono" x="62" y="424.5" text-anchor="middle">1</text>
<text class="d-mono" x="22" y="444.5" text-anchor="middle">2</text>
<text class="d-mono" x="42" y="444.5" text-anchor="middle">-1</text>
<text class="d-label" x="188" y="72" text-anchor="start">residual samples</text>
<text class="d-label" x="188" y="96" text-anchor="start">every sample</text>
<text class="d-label" x="188" y="112" text-anchor="start">holds something</text>
<text class="d-label" x="188" y="272" text-anchor="start">frequency coefficients</text>
<text class="d-label" x="188" y="296" text-anchor="start">the energy collects</text>
<text class="d-label" x="188" y="312" text-anchor="start">at top left</text>
<text class="d-label" x="188" y="472" text-anchor="start">after rounding</text>
<text class="d-label" x="188" y="496" text-anchor="start">five values left,</text>
<text class="d-label" x="188" y="512" text-anchor="start">the rest are zero</text>
<line class="d-arrow" x1="92" y1="174" x2="92" y2="206"/>
<text class="d-label" x="102" y="194" text-anchor="start">transform</text>
<line class="d-arrow" x1="92" y1="374" x2="92" y2="406"/>
<text class="d-label" x="102" y="394" text-anchor="start">quantize</text>
</svg>
<figcaption>The transform moves a block's energy into a few coefficients, so that rounding the rest to zero costs little.</figcaption>
</figure>

Coefficients are coded in groups of 4x4, starting from the last nonzero one and working back toward the top left. One trick saves a bit in many groups. In sign data hiding the encoder leaves out the sign of one coefficient. The decoder works it out from whether the group's sum is odd or even.

## Entropy coding

Everything in a slice is written by one coder, context-adaptive binary arithmetic coding (CABAC). It works in three steps. Each value is first turned into a string of yes-or-no decisions called bins. For most bins the coder keeps a running estimate of how likely a one is, in a context model chosen by what the bin means. With that estimate an arithmetic coder spends less than a bit on a likely outcome and more than a bit on an unlikely one.

<figure class="diagram">
<svg class="d-wide" viewBox="0 0 720 190" role="img" aria-label="The two paths a bin takes through CABAC">
<title>The two paths a bin takes through CABAC</title>
<desc>A syntax element is binarized into bins. A regular bin goes to a context model that estimates how likely a one is, then to the arithmetic coder, which sends the outcome back to update the estimate. A bypass bin skips the model and goes straight to the arithmetic coder at even odds.</desc>
<text class="d-label" x="60" y="105" text-anchor="middle">syntax</text>
<text class="d-label" x="60" y="122" text-anchor="middle">element</text>
<line class="d-arrow" x1="96" y1="110" x2="128" y2="110"/>
<rect class="d-box" x="130" y="90" width="100" height="40" rx="6"/>
<text class="d-label" x="180" y="114.5" text-anchor="middle">Binarize</text>
<line class="d-arrow" x1="230" y1="110" x2="268" y2="110"/>
<text class="d-label" x="250" y="100" text-anchor="middle">bins</text>
<circle class="d-accent" cx="280" cy="110" r="5"/>
<path class="d-arrow" d="M280 105 L280 60 L318 60"/>
<rect class="d-accent" x="320" y="40" width="170" height="40" rx="6"/>
<text class="d-label" x="405" y="56.5" text-anchor="middle">Context model:</text>
<text class="d-label" x="405" y="72.5" text-anchor="middle">how likely is a 1 here?</text>
<path class="d-arrow" d="M490 60 L555 60 L555 88"/>
<path class="d-arrow d-muted" d="M280 115 L280 170 L555 170 L555 132"/>
<text class="d-label" x="410" y="162" text-anchor="middle">bypass: no model, even odds</text>
<rect class="d-box" x="500" y="90" width="120" height="40" rx="6"/>
<text class="d-label" x="560" y="106.5" text-anchor="middle">Arithmetic</text>
<text class="d-label" x="560" y="122.5" text-anchor="middle">coder</text>
<line class="d-arrow" x1="620" y1="110" x2="660" y2="110"/>
<text class="d-label" x="686" y="115" text-anchor="middle">bits</text>
<path class="d-arrow d-muted" d="M520 90 C500 20 420 12 405 38"/>
<text class="d-label" x="520" y="22" text-anchor="start">update the estimate</text>
<text class="d-label" x="272" y="82" text-anchor="end">regular</text>
</svg>
<svg class="d-narrow" viewBox="0 0 360 346" role="img" aria-labelledby="cabac-fig-t-n cabac-fig-d-n">
<title id="cabac-fig-t-n">The two paths a bin takes through CABAC</title>
<desc id="cabac-fig-d-n">A syntax element is binarized into bins. A regular bin goes to a context model that estimates how likely a one is, then to the arithmetic coder, which sends the outcome back to update the estimate. A bypass bin skips the model and goes straight to the arithmetic coder at even odds.</desc>
<text class="d-label" x="180" y="18" text-anchor="middle">syntax element</text>
<line class="d-arrow" x1="180" y1="26" x2="180" y2="48"/>
<rect class="d-box" x="120" y="50" width="120" height="40" rx="6"/>
<text class="d-label" x="180" y="74.5" text-anchor="middle">Binarize</text>
<line class="d-arrow" x1="180" y1="90" x2="180" y2="117"/>
<text class="d-label" x="188" y="108" text-anchor="start">bins</text>
<circle class="d-accent" cx="180" cy="124" r="5"/>
<path class="d-arrow" d="M175 124 H105 V158"/>
<text class="d-label" x="140" y="116" text-anchor="middle">regular</text>
<rect class="d-accent" x="10" y="160" width="190" height="40" rx="6"/>
<text class="d-label" x="105" y="176.5" text-anchor="middle">Context model:</text>
<text class="d-label" x="105" y="192.5" text-anchor="middle">how likely is a 1 here?</text>
<path class="d-arrow d-muted" d="M185 124 H330 V270 H242"/>
<text class="d-label" x="322" y="170" text-anchor="end">bypass: no model,</text>
<text class="d-label" x="322" y="186" text-anchor="end">even odds</text>
<path class="d-arrow" d="M105 200 V270 H118"/>
<rect class="d-box" x="120" y="250" width="120" height="40" rx="6"/>
<text class="d-label" x="180" y="266.5" text-anchor="middle">Arithmetic</text>
<text class="d-label" x="180" y="282.5" text-anchor="middle">coder</text>
<path class="d-arrow d-muted" d="M170 250 V202"/>
<text class="d-label" x="178" y="230" text-anchor="start">update the estimate</text>
<line class="d-arrow" x1="180" y1="290" x2="180" y2="318"/>
<text class="d-label" x="180" y="337" text-anchor="middle">bits</text>
</svg>
<figcaption>A regular bin is coded with a learned probability. A bypass bin skips the model and costs one whole bit.</figcaption>
</figure>

Bins with no useful pattern, such as coefficient signs, take the bypass path, where the coder assumes even odds and skips the model. HEVC groups bypass bins together so that a decoder can read several at once.

One slice has to be decoded in order because the context models carry over from block to block. The standard has two optional tools for decoders that want to work in parallel. Tiles cut the picture into independent rectangles. Wavefront processing gives each row of CTUs its own start point. Both cost some bitrate.

## The in-loop filters

Quantizing each block on its own leaves visible steps at block edges. Two filters repair the rebuilt picture before it's shown or used as a reference. They're called in-loop filters because later pictures predict from the filtered result. The encoder has to apply exactly what the decoder will.

Deblocking runs first. It looks at each block edge on an 8x8 grid and decides how strong a step the coding could have caused there. It then smooths a few samples on each side. Sample adaptive offset (SAO) runs second. For each CTU the encoder can send a few small corrections. One kind is added to samples in a chosen brightness band. The other is added to samples that form a local peak or valley along a chosen direction. Ringing next to a hard edge in the picture looks like that.

<figure class="diagram">
<svg class="d-wide" viewBox="0 0 720 232" role="img" aria-label="One row of samples across a block edge, before and after each filter">
<title>One row of samples across a block edge, before and after each filter</title>
<desc>Three plots of sixteen samples along one row, with a block edge in the middle and the source drawn as a dashed line. In the rebuilt row the left block sits above the source and the right block below it, leaving a step at the edge. After deblocking the samples near the edge form a ramp. After SAO the whole row sits closer to the source.</desc>
<text class="d-label" x="120" y="28" text-anchor="middle">rebuilt</text>
<line class="d-dash d-muted" x1="119.75" y1="44" x2="119.75" y2="196"/>
<path class="d-dash" d="M32.0 160.0 L43.7 154.0 L55.4 148.0 L67.1 142.0 L78.8 136.0 L90.5 130.0 L102.2 124.0 L113.9 118.0 L125.6 112.0 L137.3 106.0 L149.0 100.0 L160.7 94.0 L172.4 88.0 L184.1 82.0 L195.8 76.0 L207.5 70.0"/>
<path class="d-line d-accent" d="M32.0 133.0 L43.7 136.0 L55.4 130.0 L67.1 115.0 L78.8 118.0 L90.5 112.0 L102.2 97.0 L113.9 100.0 L125.6 136.0 L137.3 121.0 L149.0 124.0 L160.7 118.0 L172.4 103.0 L184.1 106.0 L195.8 100.0 L207.5 85.0"/>
<text class="d-label" x="120" y="218" text-anchor="middle">a step at the block edge</text>
<text class="d-label" x="360" y="28" text-anchor="middle">after deblocking</text>
<line class="d-dash d-muted" x1="359.75" y1="44" x2="359.75" y2="196"/>
<path class="d-dash" d="M272.0 160.0 L283.7 154.0 L295.4 148.0 L307.1 142.0 L318.8 136.0 L330.5 130.0 L342.2 124.0 L353.9 118.0 L365.6 112.0 L377.3 106.0 L389.0 100.0 L400.7 94.0 L412.4 88.0 L424.1 82.0 L435.8 76.0 L447.5 70.0"/>
<path class="d-line d-accent" d="M272.0 133.0 L283.7 136.0 L295.4 130.0 L307.1 115.0 L318.8 118.0 L330.5 116.1 L342.2 109.1 L353.9 113.0 L365.6 118.7 L377.3 114.2 L389.0 118.6 L400.7 118.0 L412.4 103.0 L424.1 106.0 L435.8 100.0 L447.5 85.0"/>
<text class="d-label" x="360" y="218" text-anchor="middle">the step becomes a ramp</text>
<text class="d-label" x="600" y="28" text-anchor="middle">after SAO</text>
<line class="d-dash d-muted" x1="599.75" y1="44" x2="599.75" y2="196"/>
<path class="d-dash" d="M512.0 160.0 L523.7 154.0 L535.4 148.0 L547.1 142.0 L558.8 136.0 L570.5 130.0 L582.2 124.0 L593.9 118.0 L605.6 112.0 L617.3 106.0 L629.0 100.0 L640.7 94.0 L652.4 88.0 L664.1 82.0 L675.8 76.0 L687.5 70.0"/>
<path class="d-line d-accent" d="M512.0 149.2 L523.7 146.8 L535.4 140.8 L547.1 131.2 L558.8 128.8 L570.5 124.4 L582.2 118.1 L593.9 116.0 L605.6 114.7 L617.3 109.3 L629.0 107.4 L640.7 103.6 L652.4 94.0 L664.1 91.6 L675.8 85.6 L687.5 76.0"/>
<text class="d-label" x="600" y="218" text-anchor="middle">offsets pull it toward the source</text>
<line class="d-arrow d-muted" x1="222" y1="120" x2="256" y2="120"/>
<line class="d-arrow d-muted" x1="462" y1="120" x2="496" y2="120"/>
</svg>
<svg class="d-narrow" viewBox="0 0 360 552" role="img" aria-labelledby="deblock-fig-t-n deblock-fig-d-n">
<title id="deblock-fig-t-n">One row of samples across a block edge, before and after each filter</title>
<desc id="deblock-fig-d-n">Three plots of sixteen samples along one row, with a block edge in the middle and the source drawn as a dashed line. In the rebuilt row the left block sits above the source and the right block below it, leaving a step at the edge. After deblocking the samples near the edge form a ramp. After SAO the whole row sits closer to the source.</desc>
<text class="d-label" x="180" y="14" text-anchor="middle">rebuilt</text>
<line class="d-dash d-muted" x1="180.4" y1="22" x2="180.4" y2="136"/>
<path class="d-dash" d="M 40.0 109.0 L 58.7 104.5 L 77.4 100.0 L 96.2 95.5 L 114.9 91.0 L 133.6 86.5 L 152.3 82.0 L 171.0 77.5 L 189.8 73.0 L 208.5 68.5 L 227.2 64.0 L 245.9 59.5 L 264.6 55.0 L 283.4 50.5 L 302.1 46.0 L 320.8 41.5"/>
<path class="d-line d-accent" d="M 40.0 88.8 L 58.7 91.0 L 77.4 86.5 L 96.2 75.2 L 114.9 77.5 L 133.6 73.0 L 152.3 61.8 L 171.0 64.0 L 189.8 91.0 L 208.5 79.8 L 227.2 82.0 L 245.9 77.5 L 264.6 66.2 L 283.4 68.5 L 302.1 64.0 L 320.8 52.8"/>
<text class="d-label" x="180" y="156" text-anchor="middle">a step at the block edge</text>
<line class="d-arrow d-muted" x1="180" y1="164" x2="180" y2="186"/>
<text class="d-label" x="180" y="210" text-anchor="middle">after deblocking</text>
<line class="d-dash d-muted" x1="180.4" y1="218" x2="180.4" y2="332"/>
<path class="d-dash" d="M 40.0 305.0 L 58.7 300.5 L 77.4 296.0 L 96.2 291.5 L 114.9 287.0 L 133.6 282.5 L 152.3 278.0 L 171.0 273.5 L 189.8 269.0 L 208.5 264.5 L 227.2 260.0 L 245.9 255.5 L 264.6 251.0 L 283.4 246.5 L 302.1 242.0 L 320.8 237.5"/>
<path class="d-line d-accent" d="M 40.0 284.8 L 58.7 287.0 L 77.4 282.5 L 96.2 271.2 L 114.9 273.5 L 133.6 272.1 L 152.3 266.8 L 171.0 269.8 L 189.8 274.0 L 208.5 270.6 L 227.2 273.9 L 245.9 273.5 L 264.6 262.2 L 283.4 264.5 L 302.1 260.0 L 320.8 248.8"/>
<text class="d-label" x="180" y="352" text-anchor="middle">the step becomes a ramp</text>
<line class="d-arrow d-muted" x1="180" y1="360" x2="180" y2="382"/>
<text class="d-label" x="180" y="406" text-anchor="middle">after SAO</text>
<line class="d-dash d-muted" x1="180.4" y1="414" x2="180.4" y2="528"/>
<path class="d-dash" d="M 40.0 501.0 L 58.7 496.5 L 77.4 492.0 L 96.2 487.5 L 114.9 483.0 L 133.6 478.5 L 152.3 474.0 L 171.0 469.5 L 189.8 465.0 L 208.5 460.5 L 227.2 456.0 L 245.9 451.5 L 264.6 447.0 L 283.4 442.5 L 302.1 438.0 L 320.8 433.5"/>
<path class="d-line d-accent" d="M 40.0 492.9 L 58.7 491.1 L 77.4 486.6 L 96.2 479.4 L 114.9 477.6 L 133.6 474.3 L 152.3 469.6 L 171.0 468.0 L 189.8 467.0 L 208.5 463.0 L 227.2 461.6 L 245.9 458.7 L 264.6 451.5 L 283.4 449.7 L 302.1 445.2 L 320.8 438.0"/>
<text class="d-label" x="180" y="548" text-anchor="middle">offsets pull it toward the source</text>
</svg>
<figcaption>The dashed line is the source and the solid line is the decoded row. The vertical line marks the edge between two blocks.</figcaption>
</figure>

## Profiles, levels and HDR

A decoder doesn't have to support the whole standard. A profile names the tools and sample formats a stream may use. Main is 8-bit video with chroma at half resolution in both directions, written 4:2:0. Main 10 allows 10 bits. Other profiles add 4:2:2 and 4:4:4 chroma, where chroma keeps more or all of luma's resolution.

<figure class="diagram">
<svg class="d-wide" viewBox="0 0 720 228" role="img" aria-label="Where chroma samples sit in the three chroma formats">
<title>Where chroma samples sit in the three chroma formats</title>
<desc>Three 4 by 4 grids of luma samples drawn as circles, with chroma positions drawn as squares. In 4:2:0 there is one chroma position for every two by two group of luma samples. In 4:2:2 there is one for every two luma samples along a row. In 4:4:4 every luma sample has one.</desc>
<text class="d-mono" x="115" y="28" text-anchor="middle">4:2:0</text>
<circle class="d-box" cx="58" cy="60" r="7"/>
<circle class="d-box" cx="96" cy="60" r="7"/>
<circle class="d-box" cx="134" cy="60" r="7"/>
<circle class="d-box" cx="172" cy="60" r="7"/>
<circle class="d-box" cx="58" cy="98" r="7"/>
<circle class="d-box" cx="96" cy="98" r="7"/>
<circle class="d-box" cx="134" cy="98" r="7"/>
<circle class="d-box" cx="172" cy="98" r="7"/>
<circle class="d-box" cx="58" cy="136" r="7"/>
<circle class="d-box" cx="96" cy="136" r="7"/>
<circle class="d-box" cx="134" cy="136" r="7"/>
<circle class="d-box" cx="172" cy="136" r="7"/>
<circle class="d-box" cx="58" cy="174" r="7"/>
<circle class="d-box" cx="96" cy="174" r="7"/>
<circle class="d-box" cx="134" cy="174" r="7"/>
<circle class="d-box" cx="172" cy="174" r="7"/>
<rect class="d-accent" x="53" y="74" width="10" height="10" rx="0"/>
<rect class="d-accent" x="129" y="74" width="10" height="10" rx="0"/>
<rect class="d-accent" x="53" y="150" width="10" height="10" rx="0"/>
<rect class="d-accent" x="129" y="150" width="10" height="10" rx="0"/>
<text class="d-label" x="115" y="214" text-anchor="middle">4 chroma positions</text>
<text class="d-mono" x="360" y="28" text-anchor="middle">4:2:2</text>
<circle class="d-box" cx="303" cy="60" r="7"/>
<circle class="d-box" cx="341" cy="60" r="7"/>
<circle class="d-box" cx="379" cy="60" r="7"/>
<circle class="d-box" cx="417" cy="60" r="7"/>
<circle class="d-box" cx="303" cy="98" r="7"/>
<circle class="d-box" cx="341" cy="98" r="7"/>
<circle class="d-box" cx="379" cy="98" r="7"/>
<circle class="d-box" cx="417" cy="98" r="7"/>
<circle class="d-box" cx="303" cy="136" r="7"/>
<circle class="d-box" cx="341" cy="136" r="7"/>
<circle class="d-box" cx="379" cy="136" r="7"/>
<circle class="d-box" cx="417" cy="136" r="7"/>
<circle class="d-box" cx="303" cy="174" r="7"/>
<circle class="d-box" cx="341" cy="174" r="7"/>
<circle class="d-box" cx="379" cy="174" r="7"/>
<circle class="d-box" cx="417" cy="174" r="7"/>
<rect class="d-accent" x="298" y="55" width="10" height="10" rx="0"/>
<rect class="d-accent" x="374" y="55" width="10" height="10" rx="0"/>
<rect class="d-accent" x="298" y="93" width="10" height="10" rx="0"/>
<rect class="d-accent" x="374" y="93" width="10" height="10" rx="0"/>
<rect class="d-accent" x="298" y="131" width="10" height="10" rx="0"/>
<rect class="d-accent" x="374" y="131" width="10" height="10" rx="0"/>
<rect class="d-accent" x="298" y="169" width="10" height="10" rx="0"/>
<rect class="d-accent" x="374" y="169" width="10" height="10" rx="0"/>
<text class="d-label" x="360" y="214" text-anchor="middle">8 chroma positions</text>
<text class="d-mono" x="605" y="28" text-anchor="middle">4:4:4</text>
<circle class="d-box" cx="548" cy="60" r="7"/>
<circle class="d-box" cx="586" cy="60" r="7"/>
<circle class="d-box" cx="624" cy="60" r="7"/>
<circle class="d-box" cx="662" cy="60" r="7"/>
<circle class="d-box" cx="548" cy="98" r="7"/>
<circle class="d-box" cx="586" cy="98" r="7"/>
<circle class="d-box" cx="624" cy="98" r="7"/>
<circle class="d-box" cx="662" cy="98" r="7"/>
<circle class="d-box" cx="548" cy="136" r="7"/>
<circle class="d-box" cx="586" cy="136" r="7"/>
<circle class="d-box" cx="624" cy="136" r="7"/>
<circle class="d-box" cx="662" cy="136" r="7"/>
<circle class="d-box" cx="548" cy="174" r="7"/>
<circle class="d-box" cx="586" cy="174" r="7"/>
<circle class="d-box" cx="624" cy="174" r="7"/>
<circle class="d-box" cx="662" cy="174" r="7"/>
<rect class="d-accent" x="543" y="55" width="10" height="10" rx="0"/>
<rect class="d-accent" x="581" y="55" width="10" height="10" rx="0"/>
<rect class="d-accent" x="619" y="55" width="10" height="10" rx="0"/>
<rect class="d-accent" x="657" y="55" width="10" height="10" rx="0"/>
<rect class="d-accent" x="543" y="93" width="10" height="10" rx="0"/>
<rect class="d-accent" x="581" y="93" width="10" height="10" rx="0"/>
<rect class="d-accent" x="619" y="93" width="10" height="10" rx="0"/>
<rect class="d-accent" x="657" y="93" width="10" height="10" rx="0"/>
<rect class="d-accent" x="543" y="131" width="10" height="10" rx="0"/>
<rect class="d-accent" x="581" y="131" width="10" height="10" rx="0"/>
<rect class="d-accent" x="619" y="131" width="10" height="10" rx="0"/>
<rect class="d-accent" x="657" y="131" width="10" height="10" rx="0"/>
<rect class="d-accent" x="543" y="169" width="10" height="10" rx="0"/>
<rect class="d-accent" x="581" y="169" width="10" height="10" rx="0"/>
<rect class="d-accent" x="619" y="169" width="10" height="10" rx="0"/>
<rect class="d-accent" x="657" y="169" width="10" height="10" rx="0"/>
<text class="d-label" x="605" y="214" text-anchor="middle">16 chroma positions</text>
</svg>
<svg class="d-narrow" viewBox="0 0 360 184" role="img" aria-labelledby="chroma-fig-t-n chroma-fig-d-n">
<title id="chroma-fig-t-n">Where chroma samples sit in the three chroma formats</title>
<desc id="chroma-fig-d-n">Three 4 by 4 grids of luma samples drawn as circles, with chroma positions drawn as squares. In 4:2:0 there is one chroma position for every two by two group of luma samples. In 4:2:2 there is one for every two luma samples along a row. In 4:4:4 every luma sample has one.</desc>
<text class="d-mono" x="60" y="24" text-anchor="middle">4:2:0</text>
<circle class="d-box" cx="21" cy="48" r="6"/>
<circle class="d-box" cx="47" cy="48" r="6"/>
<circle class="d-box" cx="73" cy="48" r="6"/>
<circle class="d-box" cx="99" cy="48" r="6"/>
<circle class="d-box" cx="21" cy="74" r="6"/>
<circle class="d-box" cx="47" cy="74" r="6"/>
<circle class="d-box" cx="73" cy="74" r="6"/>
<circle class="d-box" cx="99" cy="74" r="6"/>
<circle class="d-box" cx="21" cy="100" r="6"/>
<circle class="d-box" cx="47" cy="100" r="6"/>
<circle class="d-box" cx="73" cy="100" r="6"/>
<circle class="d-box" cx="99" cy="100" r="6"/>
<circle class="d-box" cx="21" cy="126" r="6"/>
<circle class="d-box" cx="47" cy="126" r="6"/>
<circle class="d-box" cx="73" cy="126" r="6"/>
<circle class="d-box" cx="99" cy="126" r="6"/>
<rect class="d-accent" x="17" y="57" width="8" height="8" rx="0"/>
<rect class="d-accent" x="69" y="57" width="8" height="8" rx="0"/>
<rect class="d-accent" x="17" y="109" width="8" height="8" rx="0"/>
<rect class="d-accent" x="69" y="109" width="8" height="8" rx="0"/>
<text class="d-label" x="60" y="158" text-anchor="middle">4 chroma</text>
<text class="d-label" x="60" y="174" text-anchor="middle">positions</text>
<text class="d-mono" x="180" y="24" text-anchor="middle">4:2:2</text>
<circle class="d-box" cx="141" cy="48" r="6"/>
<circle class="d-box" cx="167" cy="48" r="6"/>
<circle class="d-box" cx="193" cy="48" r="6"/>
<circle class="d-box" cx="219" cy="48" r="6"/>
<circle class="d-box" cx="141" cy="74" r="6"/>
<circle class="d-box" cx="167" cy="74" r="6"/>
<circle class="d-box" cx="193" cy="74" r="6"/>
<circle class="d-box" cx="219" cy="74" r="6"/>
<circle class="d-box" cx="141" cy="100" r="6"/>
<circle class="d-box" cx="167" cy="100" r="6"/>
<circle class="d-box" cx="193" cy="100" r="6"/>
<circle class="d-box" cx="219" cy="100" r="6"/>
<circle class="d-box" cx="141" cy="126" r="6"/>
<circle class="d-box" cx="167" cy="126" r="6"/>
<circle class="d-box" cx="193" cy="126" r="6"/>
<circle class="d-box" cx="219" cy="126" r="6"/>
<rect class="d-accent" x="137" y="44" width="8" height="8" rx="0"/>
<rect class="d-accent" x="189" y="44" width="8" height="8" rx="0"/>
<rect class="d-accent" x="137" y="70" width="8" height="8" rx="0"/>
<rect class="d-accent" x="189" y="70" width="8" height="8" rx="0"/>
<rect class="d-accent" x="137" y="96" width="8" height="8" rx="0"/>
<rect class="d-accent" x="189" y="96" width="8" height="8" rx="0"/>
<rect class="d-accent" x="137" y="122" width="8" height="8" rx="0"/>
<rect class="d-accent" x="189" y="122" width="8" height="8" rx="0"/>
<text class="d-label" x="180" y="158" text-anchor="middle">8 chroma</text>
<text class="d-label" x="180" y="174" text-anchor="middle">positions</text>
<text class="d-mono" x="300" y="24" text-anchor="middle">4:4:4</text>
<circle class="d-box" cx="261" cy="48" r="6"/>
<circle class="d-box" cx="287" cy="48" r="6"/>
<circle class="d-box" cx="313" cy="48" r="6"/>
<circle class="d-box" cx="339" cy="48" r="6"/>
<circle class="d-box" cx="261" cy="74" r="6"/>
<circle class="d-box" cx="287" cy="74" r="6"/>
<circle class="d-box" cx="313" cy="74" r="6"/>
<circle class="d-box" cx="339" cy="74" r="6"/>
<circle class="d-box" cx="261" cy="100" r="6"/>
<circle class="d-box" cx="287" cy="100" r="6"/>
<circle class="d-box" cx="313" cy="100" r="6"/>
<circle class="d-box" cx="339" cy="100" r="6"/>
<circle class="d-box" cx="261" cy="126" r="6"/>
<circle class="d-box" cx="287" cy="126" r="6"/>
<circle class="d-box" cx="313" cy="126" r="6"/>
<circle class="d-box" cx="339" cy="126" r="6"/>
<rect class="d-accent" x="257" y="44" width="8" height="8" rx="0"/>
<rect class="d-accent" x="283" y="44" width="8" height="8" rx="0"/>
<rect class="d-accent" x="309" y="44" width="8" height="8" rx="0"/>
<rect class="d-accent" x="335" y="44" width="8" height="8" rx="0"/>
<rect class="d-accent" x="257" y="70" width="8" height="8" rx="0"/>
<rect class="d-accent" x="283" y="70" width="8" height="8" rx="0"/>
<rect class="d-accent" x="309" y="70" width="8" height="8" rx="0"/>
<rect class="d-accent" x="335" y="70" width="8" height="8" rx="0"/>
<rect class="d-accent" x="257" y="96" width="8" height="8" rx="0"/>
<rect class="d-accent" x="283" y="96" width="8" height="8" rx="0"/>
<rect class="d-accent" x="309" y="96" width="8" height="8" rx="0"/>
<rect class="d-accent" x="335" y="96" width="8" height="8" rx="0"/>
<rect class="d-accent" x="257" y="122" width="8" height="8" rx="0"/>
<rect class="d-accent" x="283" y="122" width="8" height="8" rx="0"/>
<rect class="d-accent" x="309" y="122" width="8" height="8" rx="0"/>
<rect class="d-accent" x="335" y="122" width="8" height="8" rx="0"/>
<text class="d-label" x="300" y="158" text-anchor="middle">16 chroma</text>
<text class="d-label" x="300" y="174" text-anchor="middle">positions</text>
</svg>
<figcaption>Circles are luma samples and squares are chroma positions, on a patch of sixteen luma samples.</figcaption>
</figure>

A level caps the picture size, the frame rate and the bitrate. A decoder then knows how much it has to handle. Each level comes in a Main tier and a High tier with a larger bitrate cap.

HDR needs no new coding tool. An HDR stream is a 10-bit stream with fields that say how to read its samples: the color primaries, the transfer function and the mastering display's range. Those fields live in the SPS and in SEI messages. [HDR](hdr.md) covers what yah265 writes. The [encoding guide](guide.md#profiles-depth-and-chroma) lists the profiles it codes.

## What the standard doesn't say

The specification defines what every bit of a stream means and what a decoder must do with it. Every conforming decoder produces the same pictures from the same stream, down to the last sample. The standard says nothing about how an encoder arrives at the stream.

<figure class="diagram">
<svg class="d-wide" viewBox="0 0 720 250" role="img" aria-label="What the standard fixes and what it leaves open">
<title>What the standard fixes and what it leaves open</title>
<desc>The encoder is drawn as a dashed box labeled left open, holding the choices the standard doesn't make: picture types, tree splits, modes and motion vectors, coefficient rounding, quantizers and threading. An arrow labeled bitstream leads to the decoder, drawn as a solid box labeled specified, which gives the same pictures on every decoder.</desc>
<rect class="d-dash" x="20" y="30" width="290" height="200" rx="8"/>
<text class="d-text" x="165" y="56" text-anchor="middle">Encoder: left open</text>
<text class="d-label" x="165" y="86" text-anchor="middle">which pictures are I, P or B</text>
<text class="d-label" x="165" y="110" text-anchor="middle">how to split each coding tree</text>
<text class="d-label" x="165" y="134" text-anchor="middle">which mode and motion vector</text>
<text class="d-label" x="165" y="158" text-anchor="middle">how to round coefficients</text>
<text class="d-label" x="165" y="182" text-anchor="middle">what quantizer each block gets</text>
<text class="d-label" x="165" y="206" text-anchor="middle">how to use many cores</text>
<line class="d-arrow d-accent" x1="310" y1="130" x2="448" y2="130"/>
<text class="d-label" x="379" y="112" text-anchor="middle">bitstream</text>
<text class="d-label" x="379" y="150" text-anchor="middle">every bit's</text>
<text class="d-label" x="379" y="167" text-anchor="middle">meaning is fixed</text>
<rect class="d-box" x="450" y="30" width="250" height="200" rx="8"/>
<text class="d-text" x="575" y="56" text-anchor="middle">Decoder: specified</text>
<text class="d-label" x="575" y="100" text-anchor="middle">one legal way to read</text>
<text class="d-label" x="575" y="118" text-anchor="middle">every bit</text>
<text class="d-label" x="575" y="160" text-anchor="middle">the same pictures</text>
<text class="d-label" x="575" y="178" text-anchor="middle">from every decoder</text>
</svg>
<svg class="d-narrow" viewBox="0 0 360 424" role="img" aria-labelledby="open-fig-t-n open-fig-d-n">
<title id="open-fig-t-n">What the standard fixes and what it leaves open</title>
<desc id="open-fig-d-n">The encoder is drawn as a dashed box labeled left open, holding the choices the standard doesn't make: picture types, tree splits, modes and motion vectors, coefficient rounding, quantizers and threading. An arrow labeled bitstream leads to the decoder, drawn as a solid box labeled specified, which gives the same pictures on every decoder.</desc>
<rect class="d-dash" x="10" y="10" width="340" height="196" rx="8"/>
<text class="d-text" x="180" y="36" text-anchor="middle">Encoder: left open</text>
<text class="d-label" x="180" y="66" text-anchor="middle">which pictures are I, P or B</text>
<text class="d-label" x="180" y="88" text-anchor="middle">how to split each coding tree</text>
<text class="d-label" x="180" y="110" text-anchor="middle">which mode and motion vector</text>
<text class="d-label" x="180" y="132" text-anchor="middle">how to round coefficients</text>
<text class="d-label" x="180" y="154" text-anchor="middle">what quantizer each block gets</text>
<text class="d-label" x="180" y="176" text-anchor="middle">how to use many cores</text>
<line class="d-arrow d-accent" x1="180" y1="206" x2="180" y2="262"/>
<text class="d-label" x="170" y="238" text-anchor="end">bitstream</text>
<text class="d-label" x="192" y="230" text-anchor="start">every bit's</text>
<text class="d-label" x="192" y="246" text-anchor="start">meaning is fixed</text>
<rect class="d-box" x="10" y="264" width="340" height="150" rx="8"/>
<text class="d-text" x="180" y="290" text-anchor="middle">Decoder: specified</text>
<text class="d-label" x="180" y="326" text-anchor="middle">one legal way to read</text>
<text class="d-label" x="180" y="344" text-anchor="middle">every bit</text>
<text class="d-label" x="180" y="378" text-anchor="middle">the same pictures</text>
<text class="d-label" x="180" y="396" text-anchor="middle">from every decoder</text>
</svg>
<figcaption>The standard defines the bitstream and the decoder. Everything inside the dashed box is an encoder's own business.</figcaption>
</figure>

Two encoders can write legal streams of the same size from the same source and differ widely in quality and in time taken. All of that difference comes from the open choices. These are the main ones, with the Design section where yah265 makes each:

- Which pictures become key pictures, P pictures and B pictures: [Lookahead](design.md#lookahead).
- How each CTU is split and which prediction each CU gets: [Mode decision](design.md#mode-decision).
- Which motion vector, out of thousands of positions: [Motion search](design.md#motion-search).
- How each coefficient is rounded: [Transforms and quantization](design.md#transforms-and-quantization).
- What QP each picture and block gets and how a target bitrate is met: [Rate control](design.md#rate-control).
- Which pictures are kept as references: [References](design.md#references).
- How the work is spread over many cores: [Threading](threading.md).

The standard also gives an encoder no way to know that its stream is legal. [Validating the bitstream](design.md#validating-the-bitstream) covers how yah265 checks.
