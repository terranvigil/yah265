# yah265 parity report, 2026-09-26

Encoder: yah265 n265 ccada12, preset medium.  Reference: x265 4.3+1-e9b8812, preset medium.
Machine: Apple M5 Max, 18 cores, macOS 26.6.2.  Night block: 20:02-20:33.

How to read: a speed ratio is our time / the reference's time at the same file size; below 1.00 we're faster. Pass = median <= 1.00 and worst <= 1.15, quality no worse than -0.5 VMAF-NEG (median of the class; no cell below -1.0 VMAF-NEG or -1.0 dB PSNR-Y; sizes within 1%). "-" = not measured yet.

Deviations from either encoder's defaults: ours: --crf solved onto x265's bytes (n265's own default rate control is --qp 26; it has no default CRF); both: y4m on stdin through one cat (x265 needs --input - --y4m for that); x265: --log-level error (no effect on the stream).

## Verdict
```
G1 pure C, 1 thread:   PASS in CIF-S, 720p-S
G2 pure C, all cores:  PASS in CIF-S, 720p-S / OPEN in 1080p-S (quality, worst sunflower -2.30)
G3 SIMD, all cores:    PASS in CIF-S, 720p-S / OPEN in 1080p-S (quality, worst sunflower -2.30)
Quality:               holds in CIF-S, 720p-S / OPEN in 1080p-S (worst sunflower -2.30), 2160p-S (worst meridian_2160p -2.37)
Full parity:           no, 6 of 8 combinations passing; 10 of 18 not measured (listed below; tiers L and C not yet in the harness)
```

## Speed (median / worst ratio, per class and tier)
| class | tier | G1 | G2 | G3 | verdict |
|---|---|---|---|---|---|
| CIF | S | 0.53 / 0.53 | 0.36 / 0.37 | 0.62 / 0.68 | PASS |
| 720p | S | 0.40 / 0.57 | 0.41 / 0.44 | 0.86 / 0.89 | PASS |
| 1080p | S | - | 0.42 / 0.49 | 0.84 / 0.97 | PASS so far |
| 2160p | S | - | - | - | - |
| 1080p | M | - | - | - | - |
| 2160p | M | - | - | - | - |

## Quality at matched size (VMAF-NEG delta median / worst; PSNR-Y worst)
| class | tier | dVMAF | dPSNR-Y | worst cell | verdict |
|---|---|---|---|---|---|
| CIF | S | +0.71 / +0.43 | -0.49 | stefan | PASS |
| 720p | S | +1.10 / +0.59 | -0.17 | shields | PASS |
| 1080p | S | +1.27 / -2.30 | -1.01 | sunflower | OPEN (worst: sunflower -2.30, sunflower -1.01 dB) |
| 2160p | S | -1.36 / -2.37 | -1.22 | meridian_2160p | OPEN (worst: meridian_2160p -2.37, meridian_2160p -1.22 dB) |
| 1080p | M | - | - | - | - |
| 2160p | M | - | - | - | - |

Quality is read at the `def` anchor (x265 at its default threading, the point G2 and G3 are timed at). G1's anchor (one frame thread) is its own solve; its deltas are in the CSV.

## Supplementary quality (not the bar; median ours minus x265 over the class)
| class | tier | hmean NEG | min NEG | 1%-low NEG | first-1s NEG | VMAF | PSNR-YUV 6:1:1 | SSIM-YUV 6:1:1 | frames |
|---|---|---|---|---|---|---|---|---|---|
| CIF | S | +0.73 | +1.39 | +1.32 | +0.97 | - | -0.08 | -0.0017 | 3/3 ok |
| 720p | S | +1.08 | +0.72 | +1.15 | +2.15 | - | -0.02 | -0.0012 | 4/4 ok |
| 1080p | S | +1.24 | +1.64 | +1.76 | +2.74 | - | -0.02 | -0.0009 | 3/3 ok |
| 2160p | S | -1.41 | -7.35 | -8.45 | -0.07 | - | -0.94 | -0.0022 | 2/2 ok |
| 1080p | M | - | - | - | - | - | - | - | - |
| 2160p | M | - | - | - | - | - | - | - | - |

## Changed since the last report
First report under the shared spec; nothing to compare with. yah265's older board and goal legs continue as internal progress lines only.

## Not valid or not measured
- bbb_2160p (2160p-S) G2: size-mismatch; x265 bytes differ from the solved anchor; ours --crf 27.63 (solved); both y4m on stdin via cat
- bbb_2160p (2160p-S) G3: size-mismatch; x265 bytes differ from the solved anchor; ours --crf 27.63 (solved); both y4m on stdin via cat
- meridian_2160p (2160p-S) G2: size-mismatch; ours --crf 20.84 (solved); both y4m on stdin via cat
- meridian_2160p (2160p-S) G3: size-mismatch; ours --crf 20.84 (solved); both y4m on stdin via cat
- pedestrian (1080p-S) G2: size-mismatch; ours --crf 24.09 (solved); both y4m on stdin via cat
- pedestrian (1080p-S) G3: size-mismatch; ours --crf 24.09 (solved); both y4m on stdin via cat
- tier M G1, speed not measured: chimera 1080p, sollevante 1080p, meridian 1080p, chimera 2160p, sollevante 2160p, meridian 2160p
- tier M G2, speed not measured: chimera 1080p, sollevante 1080p, meridian 1080p, chimera 2160p, sollevante 2160p, meridian 2160p
- tier M G3, speed not measured: chimera 1080p, sollevante 1080p, meridian 1080p, chimera 2160p, sollevante 2160p, meridian 2160p
- tier S G1, speed not measured: sunflower 1080p, pedestrian 1080p, riverbed 1080p, meridian_2160p 2160p, bbb_2160p 2160p
- combinations not measured (not counted in m): 1080p-S G1, 2160p-S G1, 2160p-S G2, 2160p-S G3, 1080p-M G1, 1080p-M G2, 1080p-M G3, 2160p-M G1, 2160p-M G2, 2160p-M G3
- pedestrian (1080p-S): size-mismatch, ours +1.65% at --crf 24.09 (nearest reachable size; the solve found no CRF within 1%)
- bbb_2160p (2160p-S): size-mismatch, ours -3.59% at --crf 27.63 (nearest reachable size; the solve found no CRF within 1%)
- meridian_2160p (2160p-S): size-mismatch, ours -12.72% at --crf 20.84 (nearest reachable size; the solve found no CRF within 1%)
- quality not solved yet: chimera 1080p-M, sollevante 1080p-M, meridian 1080p-M, chimera 2160p-M, sollevante 2160p-M, meridian 2160p-M
- tiers L and C: not measured (later items).
- BD-VMAF-NEG over a five-rung ladder (tiers M and L): from the next round, per the spec; column empty.
