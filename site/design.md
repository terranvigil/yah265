---
title: Design - yah265
description: How yah265 decides, codes and threads a picture, and the gates every change passes.
---

# Design

HEVC moved most of an encoder's cost into search. A coding tree can split from 64x64 down to 8x8. Every node offers merge, motion search, 35 intra directions and a transform tree of its own. The pixel work per decoded sample stayed close to H.264's. So the order in which candidates are tried, and how early the losers are dropped, is the encoder's real architecture. We designed that ladder first and built the coding tools on it afterwards.

## Mode decision

Each coding tree unit runs a staged decision. Merge and skip candidates go first, with early exits for a block that's already coded well enough. Every depth that survives gets one motion search for its whole block. A cheap Hadamard cost ranks the candidates, and only the winner pays for a full rate-distortion trial through the entropy coder's bit estimator.

Pruning decides the speed. A node stops recursing when its neighbours and its own costs say a split won't pay. Under a block that intra already won on an intra-heavy frame, the recursion stops too. Intra modes in P and B slices are ranked coarse to fine on wide pictures, so 19 of the 35 get priced. At `medium` the encoder doesn't try rectangular or asymmetric partitions, and it doesn't try 4x4 intra in P and B slices. The slower presets turn those back on.

The coding tree's size follows the picture. `medium` codes 32x32 trees below 1080p. From 1080p up it codes 64x64 trees with two gates on the top node. The first tries the whole 64x64 block as a skip and keeps it when the leftover residual is small. When that trial fails by a wide margin, the second sends the node straight to the split and spends nothing else on it. Both gates ramp in with the planned quantiser, so the file size moves smoothly with the CRF.

## Lookahead

Every frame passes through a lookahead queue before it's coded. The queue keeps a half-resolution copy of each frame, estimates costs on 8x8 blocks and memoises the motion search between each pair of frames. That one search feeds everything that needs motion before the real encode: frame typing, scene cuts, how many B-frames to run, weighted prediction and the propagation step that lowers the quantiser on blocks that later frames reference.

A scene cut has to be a jump in cost, too. That keeps a pulldown cadence from reading as a run of cuts. The key frame period restarts at a cut, so a periodic key frame never follows one closely.

The lookahead's depth was swept, not inherited. On the parity cells, ours ran at 12, 20 and 30 frames against x265 at its own 20. `medium` looks 20 frames ahead from 720p up and 12 at CIF.

## Transform, quantisation and the picture

The transforms, intra prediction, the 8-tap and 4-tap interpolation filters, deblocking and SAO are the specification's. Each has a C reference.

Rate-distortion optimised quantisation (RDOQ) is closed-form and linear in the number of coefficients. It works together with sign-bit hiding, so nothing is tried and rolled back. At `medium` it runs once per coded block, at the final encode. `slow` and `slower` run it in every decision.

Several choices at `medium` come from what VMAF v1 measures:

- Adaptive quantisation scales its strength per frame from the spread of its own field.
- From 720p up, the psy strength rises on dark, smooth 16x16 blocks. Without it, faint grain in dark areas got coded to flat steps, and v1 reads that as banding.
- Chroma gets more bits than luma's quantiser alone would give it. Inter chroma coefficients round at a third of a step, and both chroma quantisers run finer than luma's. v1 scores chroma, and it's scored at display size, where a small picture's chroma shows.

## Entropy coding

One CABAC engine drives both the real coder and a fractional-bit estimator. The estimator prices coefficient decisions. Signalling bits come from static tables. Each picture is coded as one slice, with no wavefront substreams, so the stream pays no bitrate for parallelism.

## Threading

One shared worker pool runs everything. The unit of work is a row of coding tree units. Rows from several frames are in flight at once. A row in a P or B frame starts as soon as every reference frame has finished the rows its motion search can reach. That reach is fixed by geometry: the search range, the subpel step and the interpolation filter's taps. Because each wait depends only on geometry, the threaded encoder makes exactly the decisions the one-thread encoder makes. Motion vectors are never clamped to fit a schedule.

Entropy coding runs as a trailing pass per frame over the committed records. On the 64x64 tree, twelve frames can be in flight. Reference frames get workers first, and rows on wide pictures yield their worker so others can move.

VBR with a buffer is the one exception to identical output across thread counts. Above one thread its rate control solves each frame from the actual bits of frames up to eight before it, so the stream differs from the one-thread stream. A buffer shorter than 1.2 seconds of maxrate gets the one-thread stream at any thread count, because frames in flight overdrew buffers that shallow.

## Rate control

Rate control covers fixed QP, CRF, ABR, VBR, CBR, capped CRF and two-pass. CRF plans a fractional base QP from the lookahead's cost estimate. Its scale moves with the frame rate.

VBR whose maxrate sits above its target runs on the CRF path with a slowly adjusted level. It keeps a running operating point over the whole encode. A brake inside the picture holds an access unit to the buffer. When the file runs more than 8% under its target after the first minute, the level moves back toward the target at a limited rate. Capped CRF has its own brake and a guard. A last-resort skip codes a block the brake can't pay for at the coarsest quantiser.

## References

One engine handles the reference picture sets and the decoded picture buffer. The encoder derives its reference lists from the set it writes, by the decoder's own rule, so the two can't disagree. Open GOP is the default: the key frame period codes a CRA with leading pictures.

## SIMD

The arm64 build carries NEON kernels for the transforms, the cost functions, interpolation and the other hot paths. They're intrinsics, one file per kernel family, chosen at run time. Every kernel reproduces its C reference byte for byte, or it doesn't merge. `N265_ASM_OFF=all` puts the shipping binary back on the C path with no rebuild, and that's how the pure-C goals are timed. The C path stays first-class because it's what other architectures run.

## The gates

Every change passes these before it merges:

- **Two decoders.** Each conformance encode is decoded by ffmpeg and by libde265, and both reconstructions must match the encoder's own. Display-order checks go through ffmpeg.
- **Repeatability.** Two runs at `--threads 1` must give the same bytes. Across thread counts a mismatch is reported as a warning.
- **checkasm.** Each SIMD kernel is tested against its C reference, under AddressSanitizer too.
- **An escape.** A change that moves streams gets an environment setting that undoes it. The gate compares that setting's output against the previous commit, byte for byte, to show the change moves only what it claims to.
- **No regression.** A change that loses quality or speed doesn't ship. Every refusal is logged with its cause in [docs/ideas.md](docs/ideas.md).

## Clean room

The encoder is written fresh from the ITU-T H.265 specification, published papers and the reference decoder's behaviour as an oracle. No code comes from x265 or any other encoder. The exception is yah264, our own encoder written under the same rules, whose code and tooling may be reused. The command-line vocabulary follows x265's so a user's command line carries over. [CONTRIBUTING.md](CONTRIBUTING.md) has the full policy.
