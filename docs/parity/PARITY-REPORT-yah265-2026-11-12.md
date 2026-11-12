# yah265 parity report, 2026-11-12

Encoder: yah265 n265 360ab7d, a74e541, preset medium.  Reference: x265 4.3+1-e9b8812, preset medium.
Machine: Apple M5 Max, 18 cores, macOS 26.6.2.  Measured: timed legs 2026-09-26 20:02 to 2026-11-12 14:07.
Reference binaries: SIMD and pure C are one binary, `x265` = x265 4.3+1-e9b8812 (Homebrew); pure C = `--no-asm` (G1, G2). Ours: pure C = the same binary with N265_ASM_OFF=all.

How to read: a speed ratio is our time / the reference's time at the same file
size; below 1.00 we're faster. Pass = median <= 1.00 and worst <= 1.15, quality
no worse than -0.5 VMAF v1. "-" = not measured yet.
A dagger (†) marks a figure that includes a cell timed in one run only; "void"
means every cell there was measured but failed a validity rule (see the last
section). Quality holds for a class and tier when the median dVMAF v1 is
at least -0.5, no cell is more than 1.0 VMAF v1 or 1.0 dB PSNR-Y behind,
and every cell's size is within 1%. Both encoders read the same 8-bit
4:2:0 Y4M frames on stdin. Lookahead: ours 20 at 720p and up, 12 at CIF / x265 20 (each medium's own) unless a row says otherwise.

## Scope and provenance
These are Mac results, on an Apple M5 Max with arm64 NEON. The owner's definition of done is parity on every architecture: x86-64 SSE4.2, AVX2 and AVX-512, and arm64 SVE. That work is in progress. A first x86 probe on 2026-11-11 ran three cells on a Sapphire Rapids spot instance against x265 4.3, both encoders on AVX2. It read G1 0.37-0.46, G2 0.36-0.39 and G3 0.89-0.98.

The binary is build-rel ab8a99eb (main 360ab7d). Tiers S and M, nine tier L G3 cells and eleven VBR walls (every 2160p cell, and Meridian's tier L) were timed on it. The other rows were measured on older binaries. We carried them forward with scripts/alias_bin.py once identity cmps showed the streams byte-identical, and each carried row in docs/data names its source binary. Tier L's G1 and G2 windows, the other eleven tier L G3 cells and the other fifteen VBR walls ran on 12de49f2 (a74e541). That was before the deblock and SAO SIMD made the binary about 0.3% faster, so those ratios are slightly conservative. The ladders came from 12de49f2 and a759f17f. Those same eleven tier L cells keep the crf solved on 4d51b17 (061abf3), and their timed encodes held the size match.

This section and "Changed since the last report" were edited by hand after generation.

## Verdict
G1 pure C, 1 thread:   PASS in CIF-S, 720p-S, 720p-L, 1080p-S, 1080p-M, 1080p-L, 2160p-S, 2160p-M, 2160p-L; not measured in 720p-C, 1080p-C, 2160p-C
G2 pure C, all cores:  PASS in CIF-S, 720p-S, 720p-L, 1080p-S, 1080p-M, 1080p-L, 2160p-S, 2160p-M, 2160p-L; not measured in 720p-C, 1080p-C, 2160p-C
G3 SIMD, all cores:    PASS in CIF-S, 720p-S, 720p-L, 1080p-S, 1080p-M, 1080p-L, 2160p-S, 2160p-M, 2160p-L; not measured in 720p-C, 1080p-C, 2160p-C
Quality:               PASS in CIF-S, 720p-S, 720p-L, 1080p-S, 1080p-M, 1080p-L, 2160p-S, 2160p-M, 2160p-L; not measured in 720p-C, 1080p-C, 2160p-C
Full parity:           yes. Speed: 27 of 27 gating combinations pass (class, tier S/M/L and goal; pass = median <= 1.00, worst <= 1.15 and quality holding). Ladders: 52 of 52 curves landed inside the BD bar. VBR wall: 26 of 26 cells measured.
Tier C speed, not gating: not measured / void: 720p-C G1 (not measured), 1080p-C G1 (not measured), 2160p-C G1 (not measured), 720p-C G2 (not measured), 1080p-C G2 (not measured), 2160p-C G2 (not measured), 720p-C G3 (not measured), 1080p-C G3 (not measured), 2160p-C G3 (not measured)
What this rests on: the rows of docs/data/parity-spec.csv up to this date, the points in docs/data/parity-spec-points.json, the window matches, ladders and VBR walls beside them. Tier C is outside the speed count: its speed is shown and does not gate.

## Speed (median / worst ratio, per class and tier)
| class | tier | G1 | G2 | G3 | verdict |
|---|---|---|---|---|---|
| CIF | S | 0.46 / 0.48 | 0.32 / 0.35 | 0.51 / 0.57 | PASS |
| 720p | S | 0.30 / 0.43 | 0.30 / 0.33 | 0.62 / 0.65 | PASS |
| 720p | L | 0.28 / 0.32 | 0.24 / 0.31 | 0.47 / 0.56 | PASS |
| 720p | C | - | - | - | - (not gating) |
| 1080p | S | 0.32 / 0.37 | 0.30 / 0.34 | 0.52 / 0.56 | PASS |
| 1080p | M | 0.35 / 0.40 | 0.33 / 0.37 | 0.59 / 0.66 | PASS |
| 1080p | L | 0.32 / 0.36 | 0.30 / 0.31 | 0.57 / 0.59 | PASS |
| 1080p | C | - | - | - | - (not gating) |
| 2160p | S | 0.23 / 0.25 | 0.32 / 0.35 | 0.62 / 0.63 | PASS |
| 2160p | M | 0.30 / 0.36 | 0.32 / 0.39 | 0.56 / 0.60 | PASS |
| 2160p | L | 0.30 / 0.32 | 0.35 / 0.38 | 0.58 / 0.66 | PASS |
| 2160p | C | - | - | - | - (not gating) |

## Quality at matched size (VMAF v1 delta median / worst; PSNR-Y worst)
| class | tier | dVMAF | dPSNR-Y | worst cell | verdict |
|---|---|---|---|---|---|
| CIF | S | +2.33 / +0.56 | -0.68 | stefan_cif | PASS |
| 720p | S | +2.39 / +0.86 | -0.29 | shields_720p | PASS |
| 720p | L | +1.67 / +0.61 | -0.42 | bbb_720p | PASS |
| 720p | C | - | - | - | - |
| 1080p | S | +0.61 / -0.05 | -0.59 | sunflower_1080p | PASS |
| 1080p | M | +1.96 / +0.63 | -0.40 | chimera_1080p | PASS |
| 1080p | L | +1.08 / +0.70 | -0.81 | bbb_1080p | PASS |
| 1080p | C | - | - | - | - |
| 2160p | S | +1.86 / +1.22 | -0.49 | bbb_2160p | PASS |
| 2160p | M | +1.40 / +0.06 | -0.48 | chimera_2160p | PASS |
| 2160p | L | +1.18 / +0.44 | -0.54 | bbb_2160p | PASS |
| 2160p | C | - | - | - | - |

Soft cells (within the floor, below -0.5): none.
Cells whose media carries a trait that can steer the reading (docs/media-traits.md): foreman_cif (S) [flat_mix:3%]; ducks_720p (S) [narrow:51-196 noise:2.3]; park_joy_720p (S) [noise:2.4]; pedestrian_1080p (S) [flat_mix:33% narrow:30-164]; riverbed_1080p (S) [narrow:51-192]; tears_720p (L) [flat_mix:41%]; tears_1080p (L) [flat_mix:41%]; tears_2160p (L) [flat_mix:41%]; meridian_720p (L) [lead_black:60 flat_mix:82% narrow:22-171]; meridian_1080p (L) [lead_black:60 flat_mix:96% narrow:22-171]; meridian_2160p (L) [lead_black:60 flat_mix:96% narrow:22-171]; stem2_720p (L) [trail_black:10 trail_still:49 flat_mix:85%]; stem2_1080p (L) [trail_black:10 trail_still:49 flat_mix:85%]; stem2_2160p (L) [trail_black:10 trail_still:49 flat_mix:85%]; sintel_720p (L) [lead_black:24 trail_black:33 fades:8+2x/23% narrow:16-162]; sintel_1080p (L) [flat_mix:86%]; sintel_2160p (L) [flat_mix:86%]; bbb_720p (L) [flat_mix:59%]; bbb_1080p (L) [flat_mix:59%]; bbb_2160p (L) [flat_mix:59%]; cosmos_720p (L) [trail_still:58 flat_mix:87% narrow:22-164]; cosmos_1080p (L) [trail_still:58 flat_mix:87% narrow:22-164].

## Tier L end frames (end credits excluded)
A cell runs from its start frame to the last story frame (excerpt-relative, the same at every size).
- tears_720p (end frame 12671): G1 0.28; G2 0.23; G3 0.47
- tears_1080p (end frame 12671): G1 0.33; G2 0.30; G3 0.58
- tears_2160p (end frame 12671): G1 0.30; G2 0.34; G3 0.55
- meridian_720p (end frame 32886): G1 0.18; G2 0.31; G3 - [feeder-bound]
- meridian_1080p (end frame 32886): G1 0.19; G2 - [feeder-bound]; G3 - [feeder-bound]
- meridian_2160p (end frame 32886): G1 0.19; G2 0.32; G3 0.64
- stem2_720p (end frame 12939): G1 0.28; G2 0.22; G3 0.45
- stem2_1080p (end frame 12939): G1 0.32; G2 0.29; G3 0.56
- stem2_2160p (end frame 12939): G1 0.32; G2 0.36; G3 0.56
- sintel_720p (end frame 13177): G1 0.29; G2 0.24; G3 0.47
- sintel_1080p (end frame 13177): G1 0.31; G2 0.31; G3 0.58
- sintel_2160p (end frame 13177): G1 0.32; G2 0.38; G3 0.66
- bbb_720p (end frame 13928): G1 0.29; G2 0.28; G3 0.56
- bbb_1080p (end frame 13928): G1 0.33; G2 0.31; G3 0.59
- bbb_2160p (end frame 13928): G1 0.31; G2 0.37; G3 0.60
- cosmos_720p (end frame 13723): G1 0.32; G2 0.22; G3 0.48
- cosmos_1080p (end frame 13723): G1 0.36; G2 0.30; G3 0.55
- nocturne_720p (end frame 34319): G1 0.23; G2 0.27; G3 0.56
- nocturne_1080p (end frame 34319): G1 0.28; G2 0.29; G3 0.56
- nocturne_2160p (end frame 34319): G1 0.28; G2 0.29; G3 0.48

## G1/G2 window bytes (2-minute and 10-minute cells)
G1 and G2 time 30-second windows, one per 180 s, each coded as its own stream. Summed window size, ours against the reference, in the timed legs (G1 one thread, G2 default threads); in brackets the same figure at the whole cell's rate factor, before ours was matched on the windows. A goal whose windows differ by more than 5% is void for that cell.
- chimera_1080p (M): G1 +0.19% (untimed) (+12.43% before), G2 +0.44% (untimed) (+11.69% before)
- sollevante_1080p (M): G1 -0.01% (untimed) (-24.56% before), G2 +0.11% (untimed) (-25.14% before)
- meridian_1080p (M): G1 -0.05% (untimed) (+14.02% before), G2 -0.31% (untimed) (+2.31% before)
- chimera_2160p (M): G1 -0.03% (untimed) (-9.15% before), G2 -0.09% (untimed) (-9.49% before)
- sollevante_2160p (M): G1 -0.14% (untimed) (-15.28% before), G2 -0.08% (untimed) (-17.11% before)
- meridian_2160p (M): G1 +0.07% (untimed) (+8.69% before), G2 -0.01% (untimed) (+2.67% before)
- tears_720p (L): G1 -0.25% (-2.91% before), G2 -0.34% (-5.06% before)
- tears_1080p (L): G1 -0.17% (-2.01% before), G2 +0.14% (-4.31% before)
- tears_2160p (L): G1 -0.11% (+6.08% before), G2 -0.41% (+2.47% before)
- meridian_720p (L): G1 -0.17% (+5.49% before), G2 -0.11% (-8.70% before)
- meridian_1080p (L): G1 +0.20% (+0.71% before), G2 +0.12% (-9.33% before)
- meridian_2160p (L): G1 +0.33% (+12.17% before), G2 +0.15% (+1.10% before)
- stem2_720p (L): G1 -0.48% (-6.21% before), G2 +0.02% (-7.48% before)
- stem2_1080p (L): G1 -0.69% (-6.40% before), G2 +0.30% (-7.79% before)
- stem2_2160p (L): G1 +0.33% (+0.33% before), G2 +0.47% (-5.18% before)
- sintel_720p (L): G1 -0.40% (-37.11% before), G2 +0.30% (-38.17% before)
- sintel_1080p (L): G1 -0.14% (-55.73% before), G2 -0.02% (-56.39% before)
- sintel_2160p (L): G1 +0.24% (-64.38% before), G2 -0.03% (-66.03% before)
- bbb_720p (L): G1 +0.05% (-4.07% before), G2 +0.03% (-6.11% before)
- bbb_1080p (L): G1 +0.07% (-16.82% before), G2 -0.08% (-18.04% before)
- bbb_2160p (L): G1 +0.03% (-18.51% before), G2 +0.06% (-20.28% before)
- cosmos_720p (L): G1 -0.39% (+15.53% before), G2 -0.24% (+14.50% before)
- cosmos_1080p (L): G1 +0.15% (+21.81% before), G2 -0.09% (+19.80% before)
- nocturne_720p (L): G1 +0.05% (+28.36% before), G2 +0.03% (+24.65% before)
- nocturne_1080p (L): G1 -0.30% (+41.37% before), G2 -0.28% (+38.10% before)
- nocturne_2160p (L): G1 +0.09% (+39.55% before), G2 +0.15% (+37.52% before)

## BD-rate from the ladders (bar: BD-VMAF v1 <= +1.0%)
52 of 52 cell-modes have a landed curve; 52 of those 52 pass; 0 pending. Within 0.5 points of the bar: chimera_2160p VBR +0.70%.
Five rungs (CRF 20, 23, 26, 29, 32; VBR at x265's achieved rates there, maxrate 1.5x, buffer 2x), both encoders at 6 threads (x265 --pools 6 --frame-threads 4), lookahead ours 20 / x265 20, negative = ours needs fewer bits. Only curves of this binary count. VMAF v1 on every frame on tier M, every 3rd on tier L (an odd stride, coprime with both mini-GOP periods).
- chimera_1080p (M): CRF BD-VMAF v1 -6.07% (PSNR-Y +13.41%; 5 rungs, every frame); VBR BD-VMAF v1 -4.83% (PSNR-Y +16.09%; 5 rungs, every frame)
- sollevante_1080p (M): CRF BD-VMAF v1 -14.35% (PSNR-Y +2.52%; 5 rungs, every frame); VBR BD-VMAF v1 -9.67% (PSNR-Y +4.94%; 5 rungs, every frame)
- meridian_1080p (M): CRF BD-VMAF v1 -28.69% (PSNR-Y +6.05%; 5 rungs, every frame); VBR BD-VMAF v1 -27.64% (PSNR-Y +9.59%; 5 rungs, every frame)
- chimera_2160p (M): CRF BD-VMAF v1 -0.29% (PSNR-Y +19.83%; 5 rungs, every frame); VBR BD-VMAF v1 +0.70% (PSNR-Y +18.44%; 5 rungs, every frame)
- sollevante_2160p (M): CRF BD-VMAF v1 -12.18% (PSNR-Y +4.16%; 5 rungs, every frame); VBR BD-VMAF v1 -6.97% (PSNR-Y +5.70%; 5 rungs, every frame)
- meridian_2160p (M): CRF BD-VMAF v1 -16.89% (PSNR-Y +26.97%; 5 rungs, every frame); VBR BD-VMAF v1 -15.33% (PSNR-Y +22.77%; 5 rungs, every frame)
- tears_720p (L): CRF BD-VMAF v1 -7.23% (PSNR-Y +10.81%; 5 rungs, every 3 frames); VBR BD-VMAF v1 -4.01% (PSNR-Y +15.26%; 5 rungs, every 3 frames)
- tears_1080p (L): CRF BD-VMAF v1 -12.58% (PSNR-Y +10.66%; 5 rungs, every 3 frames); VBR BD-VMAF v1 -10.34% (PSNR-Y +15.72%; 5 rungs, every 3 frames)
- tears_2160p (L): CRF BD-VMAF v1 -13.31% (PSNR-Y +14.73%; 5 rungs, every 3 frames); VBR BD-VMAF v1 -14.01% (PSNR-Y +17.07%; 5 rungs, every 3 frames)
- meridian_720p (L): CRF BD-VMAF v1 -11.15% (PSNR-Y +4.54%; 5 rungs, every 3 frames); VBR BD-VMAF v1 -5.53% (PSNR-Y +11.42%; 5 rungs, every 3 frames)
- meridian_1080p (L): CRF BD-VMAF v1 -16.62% (PSNR-Y +7.13%; 5 rungs, every 3 frames); VBR BD-VMAF v1 -15.88% (PSNR-Y +11.51%; 5 rungs, every 3 frames)
- meridian_2160p (L): CRF BD-VMAF v1 -13.66% (PSNR-Y +18.18%; 5 rungs, every 3 frames); VBR BD-VMAF v1 -10.70% (PSNR-Y +23.45%; 5 rungs, every 3 frames)
- stem2_720p (L): CRF BD-VMAF v1 -13.83% (PSNR-Y +11.06%; 5 rungs, every 3 frames); VBR BD-VMAF v1 -7.64% (PSNR-Y +21.77%; 5 rungs, every 3 frames)
- stem2_1080p (L): CRF BD-VMAF v1 -17.97% (PSNR-Y +16.20%; 5 rungs, every 3 frames); VBR BD-VMAF v1 -13.81% (PSNR-Y +26.31%; 5 rungs, every 3 frames)
- stem2_2160p (L): CRF BD-VMAF v1 -12.55% (PSNR-Y +21.44%; 5 rungs, every 3 frames); VBR BD-VMAF v1 -9.12% (PSNR-Y +23.69%; 5 rungs, every 3 frames)
- sintel_720p (L): CRF BD-VMAF v1 -12.00% (PSNR-Y +10.02%; 5 rungs, every 3 frames); VBR BD-VMAF v1 -7.97% (PSNR-Y +16.43%; 5 rungs, every 3 frames)
- sintel_1080p (L): CRF BD-VMAF v1 -10.02% (PSNR-Y +19.26%; 5 rungs, every 3 frames); VBR BD-VMAF v1 -12.44% (PSNR-Y +15.95%; 5 rungs, every 3 frames)
- sintel_2160p (L): CRF BD-VMAF v1 -16.81% (PSNR-Y +16.48%; 5 rungs, every 3 frames); VBR BD-VMAF v1 -13.31% (PSNR-Y +13.40%; 5 rungs, every 3 frames)
- bbb_720p (L): CRF BD-VMAF v1 -5.25% (PSNR-Y +8.33%; 5 rungs, every 3 frames); VBR BD-VMAF v1 -0.42% (PSNR-Y +12.90%; 5 rungs, every 3 frames)
- bbb_1080p (L): CRF BD-VMAF v1 -9.04% (PSNR-Y +7.38%; 5 rungs, every 3 frames); VBR BD-VMAF v1 -6.89% (PSNR-Y +8.14%; 5 rungs, every 3 frames)
- bbb_2160p (L): CRF BD-VMAF v1 -4.94% (PSNR-Y +15.45%; 5 rungs, every 3 frames); VBR BD-VMAF v1 -4.59% (PSNR-Y +12.65%; 5 rungs, every 3 frames)
- cosmos_720p (L): CRF BD-VMAF v1 -15.30% (PSNR-Y +5.21%; 5 rungs, every 3 frames); VBR BD-VMAF v1 -14.13% (PSNR-Y +8.11%; 5 rungs, every 3 frames)
- cosmos_1080p (L): CRF BD-VMAF v1 -11.32% (PSNR-Y +9.18%; 5 rungs, every 3 frames); VBR BD-VMAF v1 -11.82% (PSNR-Y +11.15%; 5 rungs, every 3 frames)
- nocturne_720p (L): CRF BD-VMAF v1 -6.27% (PSNR-Y +8.36%; 5 rungs, every 3 frames); VBR BD-VMAF v1 -0.41% (PSNR-Y +19.30%; 5 rungs, every 3 frames)
- nocturne_1080p (L): CRF BD-VMAF v1 -8.31% (PSNR-Y +8.35%; 5 rungs, every 3 frames); VBR BD-VMAF v1 -6.88% (PSNR-Y +14.31%; 5 rungs, every 3 frames)
- nocturne_2160p (L): CRF BD-VMAF v1 -10.04% (PSNR-Y -3.77%; 5 rungs, every 3 frames); VBR BD-VMAF v1 -15.47% (PSNR-Y +1.99%; 5 rungs, every 3 frames)

### VBR rate tracking per rung (bar: within +-10% of the target over the whole cell)
Achieved bitrate against the rung's target, on the curve counted above.
- chimera_1080p (M): 5158 kbit/s ours -5.0% / reference -14.2%, 3367 kbit/s ours -5.2% / reference -14.4%, 2201 kbit/s ours -4.7% / reference -14.6%, 1439 kbit/s ours -3.7% / reference -14.5%, 936 kbit/s ours -3.0% / reference -14.2%
- sollevante_1080p (M): 20681 kbit/s ours -4.6% / reference -7.9%, 14587 kbit/s ours -4.3% / reference -7.8%, 10119 kbit/s ours -4.1% / reference -7.4%, 6904 kbit/s ours -4.0% / reference -6.9%, 4634 kbit/s ours -3.2% / reference -6.2%
- meridian_1080p (M): 1153 kbit/s ours -2.8% / reference -2.6%, 669 kbit/s ours -3.3% / reference -3.8%, 419 kbit/s ours -3.7% / reference -4.8%, 283 kbit/s ours -4.2% / reference -5.5%, 204 kbit/s ours -4.5% / reference -5.7%
- chimera_2160p (M): 15832 kbit/s ours -3.6% / reference -13.7%, 9763 kbit/s ours -3.6% / reference -14.0%, 6070 kbit/s ours -4.8% / reference -14.1%, 3864 kbit/s ours -5.2% / reference -14.4%, 2523 kbit/s ours -4.8% / reference -14.3%
- sollevante_2160p (M): 54857 kbit/s ours -6.1% / reference -7.7%, 37577 kbit/s ours -4.9% / reference -8.1%, 26236 kbit/s ours -4.8% / reference -8.2%, 18434 kbit/s ours -3.8% / reference -8.1%, 12943 kbit/s ours -4.2% / reference -7.6%
- meridian_2160p (M): 4432 kbit/s ours -1.4% / reference -1.6%, 2406 kbit/s ours -1.9% / reference -2.2%, 1341 kbit/s ours -2.5% / reference -3.2%, 820 kbit/s ours -3.1% / reference -4.8%, 561 kbit/s ours -3.5% / reference -5.8%
- tears_720p (L): 1688 kbit/s ours +0.2% / reference +1.5%, 1155 kbit/s ours +0.2% / reference +1.6%, 801 kbit/s ours +0.2% / reference +1.6%, 557 kbit/s ours +0.1% / reference +1.6%, 389 kbit/s ours +0.0% / reference +1.6%
- tears_1080p (L): 4124 kbit/s ours +0.5% / reference +1.7%, 2569 kbit/s ours +0.5% / reference +1.6%, 1676 kbit/s ours +0.4% / reference +1.6%, 1122 kbit/s ours +0.3% / reference +1.7%, 764 kbit/s ours +0.2% / reference +1.7%
- tears_2160p (L): 19778 kbit/s ours +0.2% / reference +0.4%, 9522 kbit/s ours +0.4% / reference +1.2%, 4837 kbit/s ours +0.2% / reference +1.6%, 2902 kbit/s ours +0.1% / reference +1.3%, 1924 kbit/s ours +0.0% / reference +1.2%
- meridian_720p (L): 579 kbit/s ours -2.3% / reference -3.5%, 380 kbit/s ours -2.5% / reference -3.4%, 259 kbit/s ours -2.5% / reference -3.0%, 183 kbit/s ours -2.4% / reference -2.3%, 133 kbit/s ours -2.1% / reference -1.3%
- meridian_1080p (L): 1619 kbit/s ours -1.9% / reference -3.2%, 941 kbit/s ours -2.1% / reference -3.1%, 583 kbit/s ours -2.2% / reference -3.1%, 383 kbit/s ours -2.1% / reference -2.9%, 264 kbit/s ours -1.9% / reference -2.3%
- meridian_2160p (L): 6111 kbit/s ours -1.7% / reference -3.7%, 3372 kbit/s ours -1.5% / reference -3.4%, 1877 kbit/s ours -1.6% / reference -3.2%, 1130 kbit/s ours -1.7% / reference -3.2%, 745 kbit/s ours -1.7% / reference -3.2%
- stem2_720p (L): 1464 kbit/s ours -4.4% / reference -8.1%, 1020 kbit/s ours -5.1% / reference -9.6%, 716 kbit/s ours -5.4% / reference -10.2%, 504 kbit/s ours -5.3% / reference -10.3%, 356 kbit/s ours -5.8% / reference -9.9%
- stem2_1080p (L): 3216 kbit/s ours -4.3% / reference -5.7%, 2160 kbit/s ours -4.6% / reference -9.3%, 1473 kbit/s ours -5.3% / reference -10.8%, 1010 kbit/s ours -5.2% / reference -11.2%, 696 kbit/s ours -6.4% / reference -10.9%
- stem2_2160p (L): 9480 kbit/s ours -4.6% / reference -3.8%, 5771 kbit/s ours -4.1% / reference -4.6%, 3825 kbit/s ours -4.5% / reference -7.3%, 2616 kbit/s ours -5.6% / reference -10.7%, 1816 kbit/s ours -5.2% / reference -12.0%
- sintel_720p (L): 1372 kbit/s ours -6.3% / reference -3.3%, 953 kbit/s ours -6.8% / reference -3.3%, 664 kbit/s ours -7.1% / reference -3.4%, 464 kbit/s ours -7.4% / reference -3.4%, 326 kbit/s ours -7.7% / reference -3.4%
- sintel_1080p (L): 3097 kbit/s ours -4.2% / reference -2.8%, 2107 kbit/s ours -4.6% / reference -2.9%, 1436 kbit/s ours -5.0% / reference -3.0%, 980 kbit/s ours -5.4% / reference -3.0%, 670 kbit/s ours -5.8% / reference -3.1%
- sintel_2160p (L): 13696 kbit/s ours -1.1% / reference -2.1%, 8891 kbit/s ours -1.2% / reference -2.1%, 5773 kbit/s ours -1.4% / reference -2.1%, 3751 kbit/s ours -1.8% / reference -2.2%, 2436 kbit/s ours -2.7% / reference -2.2%
- bbb_720p (L): 1563 kbit/s ours -0.9% / reference -3.5%, 1101 kbit/s ours -1.0% / reference -3.4%, 779 kbit/s ours -1.1% / reference -3.1%, 553 kbit/s ours -1.2% / reference -2.7%, 395 kbit/s ours -1.3% / reference -2.2%
- bbb_1080p (L): 3741 kbit/s ours -0.9% / reference -4.1%, 2558 kbit/s ours -1.0% / reference -4.1%, 1755 kbit/s ours -1.0% / reference -4.0%, 1205 kbit/s ours -1.1% / reference -3.8%, 833 kbit/s ours -1.1% / reference -3.5%
- bbb_2160p (L): 9343 kbit/s ours -0.8% / reference -4.1%, 6467 kbit/s ours -0.8% / reference -4.1%, 4446 kbit/s ours -0.9% / reference -4.1%, 3046 kbit/s ours -0.9% / reference -4.1%, 2111 kbit/s ours -0.9% / reference -3.9%
- cosmos_720p (L): 2053 kbit/s ours -2.6% / reference -0.4%, 1413 kbit/s ours -2.7% / reference -0.4%, 959 kbit/s ours -2.5% / reference -0.4%, 641 kbit/s ours -2.2% / reference -0.3%, 423 kbit/s ours -1.8% / reference -0.1%
- cosmos_1080p (L): 3689 kbit/s ours -2.7% / reference -0.6%, 2615 kbit/s ours -2.9% / reference -0.7%, 1820 kbit/s ours -2.9% / reference -0.8%, 1232 kbit/s ours -2.8% / reference -0.8%, 818 kbit/s ours -2.4% / reference -0.7%
- nocturne_720p (L): 3614 kbit/s ours -1.3% / reference +1.3%, 2113 kbit/s ours -1.1% / reference +2.7%, 1353 kbit/s ours -0.9% / reference +3.7%, 889 kbit/s ours -0.7% / reference +4.4%, 584 kbit/s ours -0.6% / reference +4.4%
- nocturne_1080p (L): 16661 kbit/s ours -1.3% / reference -0.8%, 6326 kbit/s ours -1.6% / reference -0.2%, 3080 kbit/s ours -1.4% / reference +1.7%, 1884 kbit/s ours -1.0% / reference +2.9%, 1213 kbit/s ours -0.6% / reference +3.4%
- nocturne_2160p (L): 114560 kbit/s ours -0.6% / reference -0.4%, 49543 kbit/s ours -0.8% / reference -0.7%, 17501 kbit/s ours -1.1% / reference -0.9%, 6908 kbit/s ours -1.4% / reference +0.1%, 3656 kbit/s ours -1.1% / reference +1.8%

## Lookahead depth sweep (BD-VMAF v1 against the reference)
Ours at --rc-lookahead 12, 20, 30 (20 is medium's own, the depth every other figure uses); x265 at its default 20 in every column. Same rungs, 6 threads; (sN) = scored every Nth frame. "best" names a depth only when the three differ by more than 0.4 points.
| cell | mode | ours 12 / x265 20 | ours 20 / x265 20 | ours 30 / x265 20 | best |
|---|---|---|---|---|---|
| chimera_2160p (M) | CRF | +0.28% | -0.29% | -0.27% | 20 |
| sollevante_1080p (M) | CRF | -13.76% | -14.35% | -14.41% | 30 |
| meridian_2160p (M) | CRF | -15.19% | -16.89% | -16.74% | 20 |
| tears_1080p (L) | CRF | -11.43% (s3) | -12.58% (s3) | -12.86% (s3) | 30 |
| bbb_1080p (L) | CRF | -5.68% (s3) | -9.04% (s3) | -10.21% (s3) | 30 |
| sintel_2160p (L) | CRF | -15.61% (s3) | -16.81% (s3) | -16.91% (s3) | 30 |
| chimera_2160p (M) | VBR | +0.61% | +0.70% | +0.68% | within spread |
| sollevante_1080p (M) | VBR | -8.90% | -9.67% | -9.70% | 30 |
| meridian_2160p (M) | VBR | -13.72% | -15.33% | -15.33% | 20 |
| tears_1080p (L) | VBR | -9.60% (s3) | -10.34% (s3) | -10.55% (s3) | 30 |
| bbb_1080p (L) | VBR | -4.28% (s3) | -6.89% (s3) | -7.35% (s3) | 30 |
| sintel_2160p (L) | VBR | -13.52% (s3) | -13.31% (s3) | -13.43% (s3) | within spread |

## VBR wall (G3 only: SIMD, default threads, one rung per cell)
One rung per M and L cell: x265's CRF 28 achieved rate as the target, maxrate 1.5x, buffer 2x, lookahead ours 20 / x265 20, 2 interleaved rounds, no size match. Tracking is the achieved bitrate against the target in the timed leg.
Median / worst per class and tier: 1080p-L 0.61 / 0.85 (7 cells); 1080p-M 0.63 / 0.68 (3 cells); 2160p-L 0.65 / 0.75 (6 cells); 2160p-M 0.64 / 0.66 (3 cells); 720p-L 0.42 / 0.76 (7 cells).
| cell | tier | target kbit/s | ratio | ours kbit/s | reference kbit/s | feed | measured |
|---|---|---|---|---|---|---|---|
| chimera_1080p | M | 1668 | 0.63 | 1600 (-4.1%) | 1426 (-14.5%) | 3758 fps | 2026-11-09 13:01 |
| sollevante_1080p | M | 7863 | 0.59 | 7561 (-3.8%) | 7306 (-7.1%) | 3766 fps | 2026-11-09 13:05 |
| meridian_1080p | M | 318 | 0.68 | 305 (-4.0%) | 301 (-5.4%) | 3987 fps | 2026-11-09 13:06 |
| chimera_2160p | M | 4515 | 0.60 | 4285 (-5.1%) | 3861 (-14.5%) | 981 fps | 2026-11-12 09:31 |
| sollevante_2160p | M | 20733 | 0.66 | 19915 (-4.0%) | 19038 (-8.2%) | 967 fps | 2026-11-12 09:42 |
| meridian_2160p | M | 949 | 0.64 | 921 (-3.0%) | 908 (-4.3%) | 933 fps | 2026-11-12 09:47 |
| tears_720p | L | 628 | 0.38 | 629 (+0.1%) | 638 (+1.6%) | 9428 fps | 2026-11-10 05:01 |
| tears_1080p | L | 1279 | 0.61 | 1284 (+0.4%) | 1300 (+1.6%) | 4698 fps | 2026-11-10 05:07 |
| tears_2160p | L | 3389 | 0.64 | 3395 (+0.2%) | 3435 (+1.4%) | 100 fps | 2026-11-12 10:11 |
| meridian_720p | L | 205 | 0.76 | 200 (-2.4%) | 200 (-2.6%) | 684 fps | 2026-11-12 10:17 |
| meridian_1080p | L | 438 | 0.85 | 428 (-2.2%) | 425 (-3.0%) | 304 fps | 2026-11-12 10:29 |
| meridian_2160p | L | 1326 | 0.65 | 1304 (-1.6%) | 1283 (-3.2%) | 246 fps | 2026-11-12 11:18 |
| stem2_720p | L | 567 | 0.42 | 538 (-5.1%) | 508 (-10.3%) | 7739 fps | 2026-11-10 06:31 |
| stem2_1080p | L | 1145 | 0.59 | 1078 (-5.8%) | 1017 (-11.2%) | 3641 fps | 2026-11-10 06:39 |
| stem2_2160p | L | 2964 | 0.61 | 2811 (-5.2%) | 2671 (-9.9%) | 118 fps | 2026-11-12 11:49 |
| sintel_720p | L | 523 | 0.42 | 485 (-7.3%) | 505 (-3.4%) | 9740 fps | 2026-11-10 07:13 |
| sintel_1080p | L | 1113 | 0.64 | 1054 (-5.3%) | 1080 (-3.0%) | 4924 fps | 2026-11-10 07:20 |
| sintel_2160p | L | 4329 | 0.75 | 4260 (-1.6%) | 4236 (-2.1%) | 92 fps | 2026-11-12 12:16 |
| bbb_720p | L | 620 | 0.54 | 613 (-1.2%) | 602 (-2.8%) | 7419 fps | 2026-11-10 07:49 |
| bbb_1080p | L | 1365 | 0.73 | 1350 (-1.1%) | 1312 (-3.9%) | 3821 fps | 2026-11-10 07:56 |
| bbb_2160p | L | 3453 | 0.68 | 3422 (-0.9%) | 3311 (-4.1%) | 221 fps | 2026-11-12 12:41 |
| cosmos_720p | L | 734 | 0.39 | 717 (-2.3%) | 731 (-0.4%) | 9776 fps | 2026-11-10 08:24 |
| cosmos_1080p | L | 1407 | 0.61 | 1367 (-2.8%) | 1395 (-0.8%) | 4913 fps | 2026-11-10 08:31 |
| nocturne_720p | L | 1022 | 0.47 | 1014 (-0.8%) | 1065 (+4.2%) | 685 fps | 2026-11-10 08:40 |
| nocturne_1080p | L | 2198 | 0.58 | 2174 (-1.1%) | 2254 (+2.6%) | 306 fps | 2026-11-10 08:58 |
| nocturne_2160p | L | 9142 | 0.49 | 9012 (-1.4%) | 9089 (-0.6%) | 107 fps | 2026-11-12 14:07 |

## End credits (supplementary rows, not a gate)
Every tier L cell stops at its last story frame. The credits that follow are coded on their own, at 1080p: x265 at CRF 28, ours at the cell's matched crf, five rungs each side for the size at equal PSNR-Y (PSNR-Y on every frame; no VMAF-NEG BD on credits). "extr" = extrapolated beyond the measured points.
| title | span (excerpt frames) | x265 CRF 28 bytes / Y | ours (matched crf) bytes / Y | ours size at x265's Y | ours size vs x265 at ours' Y | reading |
|---|---|---|---|---|---|---|
| tears | 12672-14399 | 29.53 MB / 37.59 | (24.88) 12.69 MB / 33.78 | +5.9% extr | +13.7% extr | **behind** |
| meridian | 32887-35963 | 0.53 MB / 56.10 | (26.36) 0.26 MB / 57.52 | -55.4% | -73.7% | ahead |
| stem2 | 12940-14399 | pending | | | | |
| sintel | 13178-14399 | 2.02 MB / 49.64 | (29.91) 0.53 MB / 45.29 | -35.4% | -35.1% extr | ahead |
| bbb | 13929-17660 | pending | | | | |
| cosmos | 13724-14399 | 0.17 MB / 57.33 | (25.27) 0.14 MB / 57.65 | -19.0% | -17.2% | ahead |
| nocturne | 34320-35999 | 1.10 MB / 42.70 | (24.52) 0.46 MB / 41.76 | -32.8% | -30.3% | ahead |

## Dominant content class per title (yah264's scene scan of 2026-09-26)
bbb detailed/moving 41%; cosmos mid/moving 68%; lash1924 detailed/moving 59%; meridian mid/static 98%; nocturne mid/moving 59%; sintel mid/moving 42% (whole title); stem2 mid/moving 33% (whole title); tears detailed/moving 40%. Tiers S and M and the tier C sports and screen clips were not scanned.

## Changed since the last report
The last committed report is 2026-11-04, on item 55's binary. Since then:
- Item 60 brings chimera 2160p's tier M VBR curve inside the bar, +2.67% -> +0.70% BD-v1. Under capped VBR at UHD, medium keeps the whole merge list and the intra-heavy-only 16x16 stop. It also bounds each quadrant of the 64x64 skip gate. CRF and every class below UHD don't move.
- Item 59 keeps the full intra rank on PQ and HLG streams from 720p up, and the intra transform split at UHD. SDR streams don't move, so no cell here does.
- HDR is now also read on native HLG footage, the EBU / DVB 8K clip. BD-HDR-VMAF against x265 medium reads -38.8% at 1080p and -34.6% at 2160p.
- Items 56-58 landed too. Item 56 tunes medium for PQ and HLG. Item 57 spends BD-v1 margin on speed at every class. Item 58 bounds the HD skip gate per quadrant and gives no dark psy to black.
- The SIMD is up to date. Deblock and SAO have NEON and AVX2 kernels at both depths. The x86 families have Main10 lanes on AVX2, and a new SSE4.2 tier covers every family. `N265_CPU_MAX` caps the tier, so one machine can check and time each one. gcc and QEMU legs run in Docker. None of it moves a bit.
- Every tier S and M point and nine tier L points were re-solved onto x265's bytes on the current streams.
- The docs and the site use American spelling. Prose only.
- Every gating speed median fell. 2160p-M G3 went 0.73 -> 0.56, and 2160p-L G2 went 0.44 -> 0.35.

Ratio moves of 0.05 or more against the 2026-11-10 draft (PARITY-REPORT-yah265-2026-11-10.csv):
- sollevante_2160p G3 (tier M): 0.79 -> 0.60
- chimera_2160p G3 (tier M): 0.67 -> 0.53
- sollevante_1080p G3 (tier M): 0.70 -> 0.58
- meridian_2160p G3 (tier L): 0.60 -> 0.64
- chimera_1080p G3 (tier M): 0.63 -> 0.59

## Not valid or not measured
- meridian_2160p G3 (tier M): feeder-bound; feeder 321 fps < 1.5x the faster arm's 265; ratio void; ours --crf 26.33 (solved); both y4m on stdin via cat
- meridian_720p G3 (tier L): feeder-bound; frames 0-32886 of the excerpt (master frame 1798 on), end credits excluded; lookahead: ours 20 / reference 20 (each encoder's medium default); feed: staged Y4M; feeder 683 fps < 1.5x the faster arm's 546; ratio void; warning: ours md5 differs from the solved stream; ours --crf 27.39 (solved); md5 ours b9ea1c40 ref 17347d64; instructions ours/ref 0.483
- meridian_1080p G2 (tier L): feeder-bound; frames 0-32886 of the excerpt (master frame 1798 on), end credits excluded; lookahead: ours 20 / reference 20 (each encoder's medium default); feed: staged Y4M; sampled: 3 window(s) of 30 s, 5394 of 32887 frames; threads: default both sides; window bytes +0.12% (matched on the windows at this thread count; -9.33% before the match)
- meridian_1080p G3 (tier L): feeder-bound; frames 0-32886 of the excerpt (master frame 1798 on), end credits excluded; lookahead: ours 20 / reference 20 (each encoder's medium default); feed: staged Y4M; feeder 308 fps < 1.5x the faster arm's 269; ratio void; warning: ours md5 differs from the solved stream; ours --crf 26.36 (solved); md5 ours 0056b525 ref da06d28f; instructions ours/ref 0.481
- No matched point yet: lash1924_720p (C), lash1924_1080p (C), worldcup_1080p (C), worldcup_far_1080p (C), skater227_1080p (C), boxing_2160p (C), wikipedia_1080p (C), debugging_1080p (C), spreadsheet_1080p (C).
- Tier C: G1, G2, G3 not measured on any cell.
- Tier M G1/G2 read on the whole segment, not on windows (before the window rule): chimera_1080p G1, chimera_1080p G2, sollevante_1080p G1, sollevante_1080p G2, meridian_1080p G1, meridian_1080p G2, chimera_2160p G1, chimera_2160p G2, sollevante_2160p G1, sollevante_2160p G2, meridian_2160p G1, meridian_2160p G2.
- The tier S clips are timed by parity_spec.py through one `cat`; tiers L and C and every window through lf_feed.py. Both are one feeder on stdin for both encoders.
