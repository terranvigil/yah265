# yah265 parity report, 2026-11-04

Encoder: yah265 n265 6763fad, 9e0c2db, a51e409, preset medium.  Reference: x265 4.3+1-e9b8812, preset medium.
Machine: Apple M5 Max, 18 cores, macOS 26.6.2.  Measured: timed legs 2026-09-26 20:02 to 2026-11-04 02:58.
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
| CIF | S | 0.51 / 0.51 | 0.35 / 0.37 | 0.53 / 0.58 | PASS |
| 720p | S | 0.38 / 0.52 | 0.36 / 0.41 | 0.70 / 0.72 | PASS |
| 720p | L | 0.37 / 0.44 | 0.31 / 0.35 | 0.55 / 0.69 | PASS |
| 720p | C | - | - | - | - (not gating) |
| 1080p | S | 0.38 / 0.40 | 0.36 / 0.37 | 0.61 / 0.64 | PASS |
| 1080p | M | 0.38 / 0.47 | 0.37 / 0.44 | 0.66 / 0.70 | PASS |
| 1080p | L | 0.36 / 0.42 | 0.34 / 0.36 | 0.62 / 0.66 | PASS |
| 1080p | C | - | - | - | - (not gating) |
| 2160p | S | 0.29 / 0.32 | 0.39 / 0.43 | 0.67 / 0.73 | PASS |
| 2160p | M | 0.40 / 0.48 | 0.43 / 0.51 | 0.73 / 0.79 | PASS |
| 2160p | L | 0.40 / 0.42 | 0.44 / 0.50 | 0.69 / 0.84 | PASS |
| 2160p | C | - | - | - | - (not gating) |

## Quality at matched size (VMAF v1 delta median / worst; PSNR-Y worst)
| class | tier | dVMAF | dPSNR-Y | worst cell | verdict |
|---|---|---|---|---|---|
| CIF | S | +2.10 / +0.58 | -0.67 | stefan_cif | PASS |
| 720p | S | +2.46 / +0.80 | -0.28 | shields_720p | PASS |
| 720p | L | +1.67 / +0.88 | -0.41 | bbb_720p | PASS |
| 720p | C | - | - | - | - |
| 1080p | S | +0.58 / -0.15 | -0.62 | sunflower_1080p | PASS |
| 1080p | M | +2.09 / +0.53 | -0.40 | chimera_1080p | PASS |
| 1080p | L | +1.08 / +0.75 | -0.81 | bbb_1080p | PASS |
| 1080p | C | - | - | - | - |
| 2160p | S | +2.08 / +1.40 | -0.46 | bbb_2160p | PASS |
| 2160p | M | +1.53 / +0.39 | -0.42 | chimera_2160p | PASS |
| 2160p | L | +1.27 / +0.76 | -0.40 | bbb_2160p | PASS |
| 2160p | C | - | - | - | - |

Soft cells (within the floor, below -0.5): none.
Cells whose media carries a trait that can steer the reading (docs/media-traits.md): foreman_cif (S) [flat_mix:3%]; ducks_720p (S) [narrow:51-196 noise:2.3]; park_joy_720p (S) [noise:2.4]; pedestrian_1080p (S) [flat_mix:33% narrow:30-164]; riverbed_1080p (S) [narrow:51-192]; tears_720p (L) [flat_mix:41%]; tears_1080p (L) [flat_mix:41%]; tears_2160p (L) [flat_mix:41%]; meridian_720p (L) [lead_black:60 flat_mix:82% narrow:22-171]; meridian_1080p (L) [lead_black:60 flat_mix:96% narrow:22-171]; meridian_2160p (L) [lead_black:60 flat_mix:96% narrow:22-171]; stem2_720p (L) [trail_black:10 trail_still:49 flat_mix:85%]; stem2_1080p (L) [trail_black:10 trail_still:49 flat_mix:85%]; stem2_2160p (L) [trail_black:10 trail_still:49 flat_mix:85%]; sintel_720p (L) [lead_black:24 trail_black:33 fades:8+2x/23% narrow:16-162]; sintel_1080p (L) [flat_mix:86%]; sintel_2160p (L) [flat_mix:86%]; bbb_720p (L) [flat_mix:59%]; bbb_1080p (L) [flat_mix:59%]; bbb_2160p (L) [flat_mix:59%]; cosmos_720p (L) [trail_still:58 flat_mix:87% narrow:22-164]; cosmos_1080p (L) [trail_still:58 flat_mix:87% narrow:22-164].

## Tier L end frames (end credits excluded)
A cell runs from its start frame to the last story frame (excerpt-relative, the same at every size).
- tears_720p (end frame 12671): G1 0.37; G2 0.29; G3 0.54
- tears_1080p (end frame 12671): G1 0.36; G2 0.34; G3 0.62
- tears_2160p (end frame 12671): G1 0.39; G2 0.43; G3 0.70
- meridian_720p (end frame 32886): G1 0.21; G2 0.34; G3 0.69
- meridian_1080p (end frame 32886): G1 0.19; G2 0.32; G3 0.66
- meridian_2160p (end frame 32886): G1 0.22; G2 0.35; G3 0.67
- stem2_720p (end frame 12939): G1 0.37; G2 0.29; G3 0.53
- stem2_1080p (end frame 12939): G1 0.36; G2 0.34; G3 0.61
- stem2_2160p (end frame 12939): G1 0.41; G2 0.45; G3 0.69
- sintel_720p (end frame 13177): G1 0.37; G2 0.31; G3 0.54
- sintel_1080p (end frame 13177): G1 0.34; G2 0.35; G3 0.62
- sintel_2160p (end frame 13177): G1 0.42; G2 0.50; G3 0.84
- bbb_720p (end frame 13928): G1 0.37; G2 0.35; G3 0.64
- bbb_1080p (end frame 13928): G1 0.37; G2 0.36; G3 0.64
- bbb_2160p (end frame 13928): G1 0.40; G2 0.47; G3 0.72
- cosmos_720p (end frame 13723): G1 0.44; G2 0.29; G3 0.55
- cosmos_1080p (end frame 13723): G1 0.42; G2 0.36; G3 0.60
- nocturne_720p (end frame 34319): G1 0.28; G2 0.32; G3 0.64
- nocturne_1080p (end frame 34319): G1 0.31; G2 0.32; G3 0.60
- nocturne_2160p (end frame 34319): G1 0.33; G2 0.34; G3 0.54

## G1/G2 window bytes (2-minute and 10-minute cells)
G1 and G2 time 30-second windows, one per 180 s, each coded as its own stream. Summed window size, ours against the reference, in the timed legs (G1 one thread, G2 default threads); in brackets the same figure at the whole cell's rate factor, before ours was matched on the windows. A goal whose windows differ by more than 5% is void for that cell.
- chimera_1080p (M): G1 +0.43% (untimed) (+11.84% before), G2 +0.33% (untimed) (+11.10% before)
- sollevante_1080p (M): G1 -0.06% (untimed) (-24.56% before), G2 +0.04% (untimed) (-25.14% before)
- meridian_1080p (M): G1 -0.10% (untimed) (+12.50% before), G2 +0.00% (untimed) (+0.94% before)
- chimera_2160p (M): G1 -0.30% (untimed) (-14.14% before), G2 -0.26% (untimed) (-14.46% before)
- sollevante_2160p (M): G1 +0.01% (untimed) (-16.27% before), G2 +0.10% (untimed) (-18.07% before)
- meridian_2160p (M): G1 +0.32% (untimed) (+8.51% before), G2 +0.18% (untimed) (+2.50% before)
- tears_720p (L): G1 -0.27% (-3.84% before), G2 -0.49% (-5.97% before)
- tears_1080p (L): G1 -0.06% (-2.98% before), G2 +0.46% (-5.26% before)
- tears_2160p (L): G1 -0.28% (+6.11% before), G2 -0.46% (+2.50% before)
- meridian_720p (L): G1 +0.23% (+4.26% before), G2 -0.35% (-9.76% before)
- meridian_1080p (L): G1 -0.40% (-0.40% before), G2 +0.22% (-10.33% before)
- meridian_2160p (L): G1 +0.35% (+12.65% before), G2 +0.18% (+1.54% before)
- stem2_720p (L): G1 -0.30% (-6.85% before), G2 -0.46% (-8.11% before)
- stem2_1080p (L): G1 +0.30% (-11.29% before), G2 +0.03% (-12.60% before)
- stem2_2160p (L): G1 -0.35% (-1.83% before), G2 -0.45% (-7.22% before)
- sintel_720p (L): G1 -0.19% (-37.75% before), G2 +0.30% (-38.80% before)
- sintel_1080p (L): G1 -0.17% (-56.26% before), G2 -0.07% (-56.90% before)
- sintel_2160p (L): G1 +0.17% (-64.62% before), G2 +0.37% (-66.26% before)
- bbb_720p (L): G1 -0.13% (-5.33% before), G2 -0.05% (-7.34% before)
- bbb_1080p (L): G1 -0.06% (-18.12% before), G2 -0.03% (-19.31% before)
- bbb_2160p (L): G1 +0.09% (-20.64% before), G2 +0.25% (-22.36% before)
- cosmos_720p (L): G1 -0.30% (+15.78% before), G2 -0.30% (+14.74% before)
- cosmos_1080p (L): G1 +0.06% (+22.35% before), G2 -0.03% (+20.34% before)
- nocturne_720p (L): G1 +0.05% (+27.84% before), G2 +0.10% (+24.15% before)
- nocturne_1080p (L): G1 -0.30% (+42.22% before), G2 -0.36% (+38.93% before)
- nocturne_2160p (L): G1 -0.19% (+44.51% before), G2 +0.33% (+42.40% before)

## BD-rate from the ladders (bar: BD-VMAF v1 <= +1.0%)
52 of 52 cell-modes have a landed curve; 52 of those 52 pass; 0 pending. Within 0.5 points of the bar: none.
Five rungs (CRF 20, 23, 26, 29, 32; VBR at x265's achieved rates there, maxrate 1.5x, buffer 2x), both encoders at 6 threads (x265 --pools 6 --frame-threads 4), lookahead ours 20 / x265 20, negative = ours needs fewer bits. Only curves of this binary count. VMAF v1 on every frame on tier M, every 3rd on tier L (an odd stride, coprime with both mini-GOP periods).
- chimera_1080p (M): CRF BD-VMAF v1 -4.49% (PSNR-Y +13.78%; 5 rungs, every frame); VBR BD-VMAF v1 -3.44% (PSNR-Y +16.22%; 5 rungs, every frame)
- sollevante_1080p (M): CRF BD-VMAF v1 -15.18% (PSNR-Y +0.60%; 5 rungs, every frame); VBR BD-VMAF v1 -10.42% (PSNR-Y +2.61%; 5 rungs, every frame)
- meridian_1080p (M): CRF BD-VMAF v1 -30.63% (PSNR-Y +5.25%; 5 rungs, every frame); VBR BD-VMAF v1 -28.91% (PSNR-Y +9.32%; 5 rungs, every frame)
- chimera_2160p (M): CRF BD-VMAF v1 -3.90% (PSNR-Y +14.10%; 5 rungs, every frame); VBR BD-VMAF v1 -0.76% (PSNR-Y +15.30%; 5 rungs, every frame)
- sollevante_2160p (M): CRF BD-VMAF v1 -15.29% (PSNR-Y -0.16%; 5 rungs, every frame); VBR BD-VMAF v1 -9.27% (PSNR-Y +2.80%; 5 rungs, every frame)
- meridian_2160p (M): CRF BD-VMAF v1 -18.13% (PSNR-Y +23.44%; 5 rungs, every frame); VBR BD-VMAF v1 -17.98% (PSNR-Y +23.00%; 5 rungs, every frame)
- tears_720p (L): CRF BD-VMAF v1 -8.61% (PSNR-Y +9.58%; 5 rungs, every 3 frames); VBR BD-VMAF v1 -5.46% (PSNR-Y +13.97%; 5 rungs, every 3 frames)
- tears_1080p (L): CRF BD-VMAF v1 -12.54% (PSNR-Y +10.69%; 5 rungs, every 3 frames); VBR BD-VMAF v1 -10.47% (PSNR-Y +15.62%; 5 rungs, every 3 frames)
- tears_2160p (L): CRF BD-VMAF v1 -15.14% (PSNR-Y +12.00%; 5 rungs, every 3 frames); VBR BD-VMAF v1 -15.16% (PSNR-Y +16.86%; 5 rungs, every 3 frames)
- meridian_720p (L): CRF BD-VMAF v1 -13.72% (PSNR-Y +2.79%; 5 rungs, every 3 frames); VBR BD-VMAF v1 -8.24% (PSNR-Y +9.65%; 5 rungs, every 3 frames)
- meridian_1080p (L): CRF BD-VMAF v1 -18.07% (PSNR-Y +7.80%; 5 rungs, every 3 frames); VBR BD-VMAF v1 -17.43% (PSNR-Y +12.60%; 5 rungs, every 3 frames)
- meridian_2160p (L): CRF BD-VMAF v1 -14.86% (PSNR-Y +16.49%; 5 rungs, every 3 frames); VBR BD-VMAF v1 -12.34% (PSNR-Y +25.30%; 5 rungs, every 3 frames)
- stem2_720p (L): CRF BD-VMAF v1 -16.41% (PSNR-Y +8.91%; 5 rungs, every 3 frames); VBR BD-VMAF v1 -11.28% (PSNR-Y +19.17%; 5 rungs, every 3 frames)
- stem2_1080p (L): CRF BD-VMAF v1 -19.19% (PSNR-Y +16.49%; 5 rungs, every 3 frames); VBR BD-VMAF v1 -14.88% (PSNR-Y +26.63%; 5 rungs, every 3 frames)
- stem2_2160p (L): CRF BD-VMAF v1 -16.97% (PSNR-Y +16.47%; 5 rungs, every 3 frames); VBR BD-VMAF v1 -12.68% (PSNR-Y +23.01%; 5 rungs, every 3 frames)
- sintel_720p (L): CRF BD-VMAF v1 -13.87% (PSNR-Y +8.27%; 5 rungs, every 3 frames); VBR BD-VMAF v1 -10.16% (PSNR-Y +14.33%; 5 rungs, every 3 frames)
- sintel_1080p (L): CRF BD-VMAF v1 -10.73% (PSNR-Y +19.05%; 5 rungs, every 3 frames); VBR BD-VMAF v1 -12.92% (PSNR-Y +15.96%; 5 rungs, every 3 frames)
- sintel_2160p (L): CRF BD-VMAF v1 -20.75% (PSNR-Y +12.45%; 5 rungs, every 3 frames); VBR BD-VMAF v1 -16.75% (PSNR-Y +11.70%; 5 rungs, every 3 frames)
- bbb_720p (L): CRF BD-VMAF v1 -6.94% (PSNR-Y +6.39%; 5 rungs, every 3 frames); VBR BD-VMAF v1 -2.47% (PSNR-Y +10.84%; 5 rungs, every 3 frames)
- bbb_1080p (L): CRF BD-VMAF v1 -9.51% (PSNR-Y +6.05%; 5 rungs, every 3 frames); VBR BD-VMAF v1 -6.75% (PSNR-Y +7.14%; 5 rungs, every 3 frames)
- bbb_2160p (L): CRF BD-VMAF v1 -8.44% (PSNR-Y +11.08%; 5 rungs, every 3 frames); VBR BD-VMAF v1 -6.27% (PSNR-Y +10.74%; 5 rungs, every 3 frames)
- cosmos_720p (L): CRF BD-VMAF v1 -15.67% (PSNR-Y +4.51%; 5 rungs, every 3 frames); VBR BD-VMAF v1 -14.75% (PSNR-Y +7.25%; 5 rungs, every 3 frames)
- cosmos_1080p (L): CRF BD-VMAF v1 -10.52% (PSNR-Y +10.18%; 5 rungs, every 3 frames); VBR BD-VMAF v1 -11.19% (PSNR-Y +11.55%; 5 rungs, every 3 frames)
- nocturne_720p (L): CRF BD-VMAF v1 -9.09% (PSNR-Y +7.23%; 5 rungs, every 3 frames); VBR BD-VMAF v1 -2.90% (PSNR-Y +18.92%; 5 rungs, every 3 frames)
- nocturne_1080p (L): CRF BD-VMAF v1 -11.83% (PSNR-Y +10.46%; 5 rungs, every 3 frames); VBR BD-VMAF v1 -8.08% (PSNR-Y +19.75%; 5 rungs, every 3 frames)
- nocturne_2160p (L): CRF BD-VMAF v1 -17.93% (PSNR-Y -1.97%; 5 rungs, every 3 frames); VBR BD-VMAF v1 -19.47% (PSNR-Y +5.86%; 5 rungs, every 3 frames)

### VBR rate tracking per rung (bar: within +-10% of the target over the whole cell)
Achieved bitrate against the rung's target, on the curve counted above.
- chimera_1080p (M): 5158 kbit/s ours -5.5% / reference -14.2%, 3367 kbit/s ours -5.6% / reference -14.4%, 2201 kbit/s ours -5.0% / reference -14.6%, 1439 kbit/s ours -3.8% / reference -14.5%, 936 kbit/s ours -3.1% / reference -14.2%
- sollevante_1080p (M): 20681 kbit/s ours -4.6% / reference -7.9%, 14587 kbit/s ours -4.3% / reference -7.8%, 10119 kbit/s ours -4.1% / reference -7.4%, 6904 kbit/s ours -4.1% / reference -6.9%, 4634 kbit/s ours -3.3% / reference -6.2%
- meridian_1080p (M): 1153 kbit/s ours -2.9% / reference -2.6%, 669 kbit/s ours -3.4% / reference -3.8%, 419 kbit/s ours -3.8% / reference -4.8%, 283 kbit/s ours -4.2% / reference -5.5%, 204 kbit/s ours -4.5% / reference -5.7%
- chimera_2160p (M): 15832 kbit/s ours -4.2% / reference -13.7%, 9763 kbit/s ours -4.0% / reference -14.0%, 6070 kbit/s ours -5.2% / reference -14.1%, 3864 kbit/s ours -5.4% / reference -14.4%, 2523 kbit/s ours -5.2% / reference -14.3%
- sollevante_2160p (M): 54857 kbit/s ours -6.2% / reference -7.7%, 37577 kbit/s ours -6.2% / reference -8.1%, 26236 kbit/s ours -4.8% / reference -8.2%, 18434 kbit/s ours -3.9% / reference -8.1%, 12943 kbit/s ours -4.2% / reference -7.6%
- meridian_2160p (M): 4432 kbit/s ours -1.5% / reference -1.6%, 2406 kbit/s ours -1.9% / reference -2.2%, 1341 kbit/s ours -2.5% / reference -3.2%, 820 kbit/s ours -3.1% / reference -4.8%, 561 kbit/s ours -3.4% / reference -5.8%
- tears_720p (L): 1688 kbit/s ours +0.2% / reference +1.5%, 1155 kbit/s ours +0.2% / reference +1.6%, 801 kbit/s ours +0.2% / reference +1.6%, 557 kbit/s ours +0.1% / reference +1.6%, 389 kbit/s ours +0.0% / reference +1.6%
- tears_1080p (L): 4124 kbit/s ours +0.5% / reference +1.7%, 2569 kbit/s ours +0.5% / reference +1.6%, 1676 kbit/s ours +0.4% / reference +1.6%, 1122 kbit/s ours +0.4% / reference +1.7%, 764 kbit/s ours +0.3% / reference +1.7%
- tears_2160p (L): 19778 kbit/s ours +0.3% / reference +0.4%, 9522 kbit/s ours +0.4% / reference +1.2%, 4837 kbit/s ours +0.3% / reference +1.6%, 2902 kbit/s ours +0.2% / reference +1.3%, 1924 kbit/s ours +0.1% / reference +1.2%
- meridian_720p (L): 579 kbit/s ours -2.4% / reference -3.5%, 380 kbit/s ours -2.7% / reference -3.4%, 259 kbit/s ours -2.6% / reference -3.0%, 183 kbit/s ours -2.4% / reference -2.3%, 133 kbit/s ours -2.1% / reference -1.3%
- meridian_1080p (L): 1619 kbit/s ours -2.0% / reference -3.2%, 941 kbit/s ours -2.2% / reference -3.1%, 583 kbit/s ours -2.2% / reference -3.1%, 383 kbit/s ours -2.1% / reference -2.9%, 264 kbit/s ours -1.8% / reference -2.3%
- meridian_2160p (L): 6111 kbit/s ours -1.7% / reference -3.7%, 3372 kbit/s ours -1.6% / reference -3.4%, 1877 kbit/s ours -1.6% / reference -3.2%, 1130 kbit/s ours -1.6% / reference -3.2%, 745 kbit/s ours -1.6% / reference -3.2%
- stem2_720p (L): 1464 kbit/s ours -4.6% / reference -8.1%, 1020 kbit/s ours -5.3% / reference -9.6%, 716 kbit/s ours -5.7% / reference -10.2%, 504 kbit/s ours -5.5% / reference -10.3%, 356 kbit/s ours -5.9% / reference -9.9%
- stem2_1080p (L): 3216 kbit/s ours -4.2% / reference -5.7%, 2160 kbit/s ours -4.6% / reference -9.3%, 1473 kbit/s ours -5.5% / reference -10.8%, 1010 kbit/s ours -5.4% / reference -11.2%, 696 kbit/s ours -6.6% / reference -10.9%
- stem2_2160p (L): 9480 kbit/s ours -4.4% / reference -3.8%, 5771 kbit/s ours -4.3% / reference -4.6%, 3825 kbit/s ours -4.8% / reference -7.3%, 2616 kbit/s ours -5.4% / reference -10.7%, 1816 kbit/s ours -5.5% / reference -12.0%
- sintel_720p (L): 1372 kbit/s ours -6.4% / reference -3.3%, 953 kbit/s ours -6.8% / reference -3.3%, 664 kbit/s ours -7.1% / reference -3.4%, 464 kbit/s ours -7.4% / reference -3.4%, 326 kbit/s ours -7.6% / reference -3.4%
- sintel_1080p (L): 3097 kbit/s ours -4.3% / reference -2.8%, 2107 kbit/s ours -4.6% / reference -2.9%, 1436 kbit/s ours -5.0% / reference -3.0%, 980 kbit/s ours -5.3% / reference -3.0%, 670 kbit/s ours -5.7% / reference -3.1%
- sintel_2160p (L): 13696 kbit/s ours -1.3% / reference -2.1%, 8891 kbit/s ours -1.3% / reference -2.1%, 5773 kbit/s ours -1.4% / reference -2.1%, 3751 kbit/s ours -1.8% / reference -2.2%, 2436 kbit/s ours -2.6% / reference -2.2%
- bbb_720p (L): 1563 kbit/s ours -0.9% / reference -3.5%, 1101 kbit/s ours -1.0% / reference -3.4%, 779 kbit/s ours -1.1% / reference -3.1%, 553 kbit/s ours -1.2% / reference -2.7%, 395 kbit/s ours -1.3% / reference -2.2%
- bbb_1080p (L): 3741 kbit/s ours -1.0% / reference -4.1%, 2558 kbit/s ours -1.0% / reference -4.1%, 1755 kbit/s ours -1.0% / reference -4.0%, 1205 kbit/s ours -1.1% / reference -3.8%, 833 kbit/s ours -1.1% / reference -3.5%
- bbb_2160p (L): 9343 kbit/s ours -0.8% / reference -4.1%, 6467 kbit/s ours -0.9% / reference -4.1%, 4446 kbit/s ours -0.9% / reference -4.1%, 3046 kbit/s ours -0.9% / reference -4.1%, 2111 kbit/s ours -1.0% / reference -3.9%
- cosmos_720p (L): 2053 kbit/s ours -2.7% / reference -0.4%, 1413 kbit/s ours -2.8% / reference -0.4%, 959 kbit/s ours -2.6% / reference -0.4%, 641 kbit/s ours -2.3% / reference -0.3%, 423 kbit/s ours -1.9% / reference -0.1%
- cosmos_1080p (L): 3689 kbit/s ours -2.8% / reference -0.6%, 2615 kbit/s ours -3.0% / reference -0.7%, 1820 kbit/s ours -3.0% / reference -0.8%, 1232 kbit/s ours -2.9% / reference -0.8%, 818 kbit/s ours -2.6% / reference -0.7%
- nocturne_720p (L): 3614 kbit/s ours -1.3% / reference +1.3%, 2113 kbit/s ours -1.0% / reference +2.7%, 1353 kbit/s ours -0.9% / reference +3.7%, 889 kbit/s ours -0.7% / reference +4.4%, 584 kbit/s ours -0.6% / reference +4.4%
- nocturne_1080p (L): 16661 kbit/s ours -1.2% / reference -0.8%, 6326 kbit/s ours -1.6% / reference -0.2%, 3080 kbit/s ours -1.4% / reference +1.7%, 1884 kbit/s ours -1.2% / reference +2.9%, 1213 kbit/s ours -1.0% / reference +3.4%
- nocturne_2160p (L): 114560 kbit/s ours -0.6% / reference -0.4%, 49543 kbit/s ours -0.7% / reference -0.7%, 17501 kbit/s ours -1.0% / reference -0.9%, 6908 kbit/s ours -1.4% / reference +0.1%, 3656 kbit/s ours -1.2% / reference +1.8%

## Lookahead depth sweep (BD-VMAF v1 against the reference)
Ours at --rc-lookahead 12, 20, 30 (20 is medium's own, the depth every other figure uses); x265 at its default 20 in every column. Same rungs, 6 threads; (sN) = scored every Nth frame. "best" names a depth only when the three differ by more than 0.4 points.
| cell | mode | ours 12 / x265 20 | ours 20 / x265 20 | ours 30 / x265 20 | best |
|---|---|---|---|---|---|
| chimera_2160p (M) | CRF | -3.49% | -3.90% | -3.87% | 20 |
| sollevante_1080p (M) | CRF | -14.56% | -15.18% | -15.30% | 30 |
| meridian_2160p (M) | CRF | -16.26% | -18.13% | -18.39% | 30 |
| tears_1080p (L) | CRF | -11.39% (s3) | -12.54% (s3) | -12.76% (s3) | 30 |
| bbb_1080p (L) | CRF | -6.28% (s3) | -9.51% (s3) | -10.53% (s3) | 30 |
| sintel_2160p (L) | CRF | -19.56% (s3) | -20.75% (s3) | -20.80% (s3) | 30 |
| chimera_2160p (M) | VBR | -1.04% | -0.76% | -0.19% | 12 |
| sollevante_1080p (M) | VBR | -9.69% | -10.42% | -10.42% | 20 |
| meridian_2160p (M) | VBR | -16.36% | -17.98% | -17.82% | 20 |
| tears_1080p (L) | VBR | -9.69% (s3) | -10.47% (s3) | -10.66% (s3) | 30 |
| bbb_1080p (L) | VBR | -4.41% (s3) | -6.75% (s3) | -7.10% (s3) | 30 |
| sintel_2160p (L) | VBR | -16.60% (s3) | -16.75% (s3) | -16.65% (s3) | within spread |

## VBR wall (G3 only: SIMD, default threads, one rung per cell)
One rung per M and L cell: x265's CRF 28 achieved rate as the target, maxrate 1.5x, buffer 2x, lookahead ours 20 / x265 20, 2 interleaved rounds, no size match. Tracking is the achieved bitrate against the target in the timed leg.
Median / worst per class and tier: 1080p-L 0.68 / 0.78 (7 cells); 1080p-M 0.67 / 0.69 (3 cells); 2160p-L 0.71 / 0.87 (6 cells); 2160p-M 0.69 / 0.81 (3 cells); 720p-L 0.48 / 0.76 (7 cells).
| cell | tier | target kbit/s | ratio | ours kbit/s | reference kbit/s | feed | measured |
|---|---|---|---|---|---|---|---|
| chimera_1080p | M | 1668 | 0.67 | 1596 (-4.3%) | 1426 (-14.5%) | 3814 fps | 2026-11-02 23:51 |
| sollevante_1080p | M | 7863 | 0.67 | 7554 (-3.9%) | 7306 (-7.1%) | 3535 fps | 2026-11-02 23:54 |
| meridian_1080p | M | 318 | 0.69 | 305 (-4.1%) | 301 (-5.4%) | 3653 fps | 2026-11-02 23:56 |
| chimera_2160p | M | 4515 | 0.69 | 4272 (-5.4%) | 3861 (-14.5%) | 911 fps | 2026-11-03 00:01 |
| sollevante_2160p | M | 20733 | 0.81 | 19900 (-4.0%) | 19037 (-8.2%) | 983 fps | 2026-11-03 00:13 |
| meridian_2160p | M | 949 | 0.65 | 921 (-2.9%) | 908 (-4.4%) | 930 fps | 2026-11-03 00:18 |
| tears_720p | L | 628 | 0.44 | 629 (+0.1%) | 638 (+1.6%) | 9252 fps | 2026-11-02 18:03 |
| tears_1080p | L | 1279 | 0.66 | 1284 (+0.4%) | 1300 (+1.6%) | 4427 fps | 2026-11-02 18:10 |
| tears_2160p | L | 3389 | 0.73 | 3396 (+0.2%) | 3435 (+1.4%) | 100 fps | 2026-11-02 18:35 |
| meridian_720p | L | 205 | 0.76 | 200 (-2.5%) | 200 (-2.6%) | 685 fps | 2026-11-02 18:41 |
| meridian_1080p | L | 438 | 0.77 | 429 (-2.1%) | 425 (-3.0%) | 3632 fps | 2026-11-02 18:52 |
| meridian_2160p | L | 1326 | 0.68 | 1305 (-1.6%) | 1283 (-3.2%) | 247 fps | 2026-11-02 19:42 |
| stem2_720p | L | 567 | 0.48 | 537 (-5.3%) | 508 (-10.3%) | 7706 fps | 2026-11-02 19:46 |
| stem2_1080p | L | 1145 | 0.62 | 1087 (-5.1%) | 1017 (-11.2%) | 3608 fps | 2026-11-02 19:54 |
| stem2_2160p | L | 2964 | 0.69 | 2801 (-5.5%) | 2671 (-9.9%) | 119 fps | 2026-11-02 20:27 |
| sintel_720p | L | 523 | 0.48 | 485 (-7.3%) | 505 (-3.4%) | 9595 fps | 2026-11-02 20:31 |
| sintel_1080p | L | 1113 | 0.70 | 1055 (-5.2%) | 1080 (-3.0%) | 4673 fps | 2026-11-02 20:38 |
| sintel_2160p | L | 4329 | 0.87 | 4258 (-1.6%) | 4236 (-2.1%) | 92 fps | 2026-11-02 21:07 |
| bbb_720p | L | 620 | 0.59 | 613 (-1.2%) | 602 (-2.8%) | 7496 fps | 2026-11-02 21:10 |
| bbb_1080p | L | 1365 | 0.78 | 1351 (-1.1%) | 1312 (-3.9%) | 3744 fps | 2026-11-02 21:17 |
| bbb_2160p | L | 3453 | 0.76 | 3420 (-0.9%) | 3311 (-4.1%) | 206 fps | 2026-11-02 21:42 |
| cosmos_720p | L | 734 | 0.46 | 716 (-2.4%) | 732 (-0.3%) | 9806 fps | 2026-11-02 21:47 |
| cosmos_1080p | L | 1407 | 0.68 | 1365 (-3.0%) | 1395 (-0.8%) | 4750 fps | 2026-11-02 21:54 |
| nocturne_720p | L | 1022 | 0.48 | 1014 (-0.8%) | 1065 (+4.2%) | 686 fps | 2026-11-02 22:03 |
| nocturne_1080p | L | 2198 | 0.60 | 2171 (-1.2%) | 2254 (+2.6%) | 306 fps | 2026-11-02 22:21 |
| nocturne_2160p | L | 9142 | 0.54 | 9016 (-1.4%) | 9090 (-0.6%) | 108 fps | 2026-11-02 23:49 |

## End credits (supplementary rows, not a gate)
Every tier L cell stops at its last story frame. The credits that follow are coded on their own, at 1080p: x265 at CRF 28, ours at the cell's matched crf, five rungs each side for the size at equal PSNR-Y (PSNR-Y on every frame; no VMAF-NEG BD on credits). "extr" = extrapolated beyond the measured points.
| title | span (excerpt frames) | x265 CRF 28 bytes / Y | ours (matched crf) bytes / Y | ours size at x265's Y | ours size vs x265 at ours' Y | reading |
|---|---|---|---|---|---|---|
| tears | 12672-14399 | 29.53 MB / 37.59 | (24.88) 12.39 MB / 33.62 | +5.3% extr | +15.5% extr | **behind** |
| meridian | 32887-35963 | 0.53 MB / 56.10 | (26.36) 0.25 MB / 57.49 | -56.8% | -74.7% | ahead |
| stem2 | 12940-14399 | 0.44 MB / 56.05 | (26.39) 0.54 MB / 56.40 | +16.4% | +17.5% | **behind** |
| sintel | 13178-14399 | 2.02 MB / 49.64 | (29.91) 0.52 MB / 44.66 | -33.6% | -27.7% extr | ahead |
| bbb | 13929-17660 | 20.75 MB / 41.42 | (27.52) 6.07 MB / 36.36 | -6.6% extr | +7.2% extr | no overlap; figure extrapolated |
| cosmos | 13724-14399 | 0.17 MB / 57.33 | (25.27) 0.14 MB / 57.72 | -21.4% | -19.8% | ahead |
| nocturne | 34320-35999 | 1.10 MB / 42.70 | (24.52) 0.74 MB / 41.38 | +13.7% | +32.5% | **behind** |

## Dominant content class per title (yah264's scene scan of 2026-09-26)
bbb detailed/moving 41%; cosmos mid/moving 68%; lash1924 detailed/moving 59%; meridian mid/static 98%; nocturne mid/moving 59%; sintel mid/moving 42% (whole title); stem2 mid/moving 33% (whole title); tears detailed/moving 40%. Tiers S and M and the tier C sports and screen clips were not scanned.

## Changed since the last report
Against PARITY-REPORT-yah265-2026-10-22.csv:
- meridian_1080p G3 (tier M): 1.11 -> 0.66
- chimera_1080p G3 (tier M): 0.80 -> 0.63
- fourpeople_720p G3 (tier S): 0.83 -> 0.71
- sunflower_1080p G3 (tier S): 0.64 -> 0.53
- meridian_2160p G3 (tier S): 0.71 -> 0.61

## Not valid or not measured
- meridian_2160p G3 (tier M): feeder-bound; feeder 310 fps < 1.5x the faster arm's 229; ratio void; ours --crf 26.37 (solved); both y4m on stdin via cat
- No matched point yet: lash1924_720p (C), lash1924_1080p (C), worldcup_1080p (C), worldcup_far_1080p (C), skater227_1080p (C), boxing_2160p (C), wikipedia_1080p (C), debugging_1080p (C), spreadsheet_1080p (C).
- Tier C: G1, G2, G3 not measured on any cell.
- Tier M G1/G2 read on the whole segment, not on windows (before the window rule): chimera_1080p G1, chimera_1080p G2, sollevante_1080p G1, sollevante_1080p G2, meridian_1080p G1, meridian_1080p G2, chimera_2160p G1, chimera_2160p G2, sollevante_2160p G1, sollevante_2160p G2, meridian_2160p G1, meridian_2160p G2.
- The tier S clips are timed by parity_spec.py through one `cat`; tiers L and C and every window through lf_feed.py. Both are one feeder on stdin for both encoders.
