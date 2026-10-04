# yah265 parity report, 2026-10-04

Encoder: yah265 n265 b0d383f, preset medium.  Reference: x265 4.3+1-e9b8812, preset medium.
Machine: Apple M5 Max, 18 cores, macOS 26.6.2.  Night block: 01:11-01:56.

How to read: a speed ratio is our time / the reference's time at the same file size; below 1.00 we're faster. Pass = median <= 1.00 and worst <= 1.15, quality no worse than -0.5 VMAF-NEG (median of the class; no cell below -1.0 VMAF-NEG or -1.0 dB PSNR-Y; sizes within 1%). "-" = not measured yet.

Deviations from either encoder's defaults: ours: --crf solved onto x265's bytes (n265's own default rate control is --qp 26; it has no default CRF); both: y4m on stdin through one cat (x265 needs --input - --y4m for that); x265: --log-level error (no effect on the stream).

## Verdict
```
G1 pure C, 1 thread:   PASS in CIF-S, 720p-S
G2 pure C, all cores:  PASS in CIF-S, 720p-S / OPEN in 1080p-S (quality, worst sunflower -2.10), 2160p-S (quality, worst meridian_2160p -0.56)
G3 SIMD, all cores:    PASS in CIF-S, 720p-S / OPEN in 1080p-S (quality, worst sunflower -2.10), 2160p-S (quality, worst meridian_2160p -0.56)
Quality:               holds in CIF-S, 720p-S / OPEN in 1080p-S (worst sunflower -2.10), 2160p-S (worst meridian_2160p -0.56); between -0.5 and -1.0: meridian_2160p -0.56
Full parity:           no, 6 of 10 combinations passing; 8 of 18 not measured (listed below; tiers L and C not yet in the harness)
```

## Speed (median / worst ratio, per class and tier)
| class | tier | G1 | G2 | G3 | verdict |
|---|---|---|---|---|---|
| CIF | S | 0.51 / 0.51 | 0.34 / 0.36 | 0.61 / 0.68 | PASS |
| 720p | S | 0.39 / 0.54 | 0.40 / 0.43 | 0.83 / 0.87 | PASS |
| 1080p | S | - | 0.41 / 0.48 | 0.87 / 0.95 | PASS so far; one run only |
| 2160p | S | - | 0.30 / 0.34 | 0.60 / 0.72 | PASS so far |
| 1080p | M | - | - | - | - |
| 2160p | M | - | - | - | - |

## Quality at matched size (VMAF-NEG delta median / worst; PSNR-Y worst)
| class | tier | dVMAF | dPSNR-Y | worst cell | verdict |
|---|---|---|---|---|---|
| CIF | S | +0.74 / +0.47 | -0.48 | stefan | PASS |
| 720p | S | +1.29 / +0.66 | -0.16 | shields | PASS |
| 1080p | S | +1.12 / -2.10 | -0.97 | sunflower | OPEN (worst: sunflower -2.10, sunflower -0.97 dB) |
| 2160p | S | -0.36 / -0.56 | -1.06 | meridian_2160p | OPEN (worst: meridian_2160p -0.56, bbb_2160p -1.06 dB); between -0.5 and -1.0: meridian_2160p -0.56 |
| 1080p | M | - | - | - | - |
| 2160p | M | - | - | - | - |

Cells whose media carries a trait that can steer this reading (docs/media-traits.md; a result that sits only in such a trait is reported as that): foreman CIF S [flat_mix:3%]; ducks 720p S [narrow:51-196 noise:2.3]; park_joy 720p S [noise:2.4]; pedestrian 1080p S [flat_mix:33% narrow:30-164]; riverbed 1080p S [narrow:51-192]; meridian_2160p 2160p S [lead_black:60 flat_mix:91% dark:p99=140 narrow:26-140]; chimera 1080p M [lead_black:120 flat_mix:77% narrow:52-197]; sollevante 1080p M [lead_black:193 cuts:22/min flat_mix:39%]; meridian 1080p M [lead_black:60 flat_mix:96% narrow:22-171]; chimera 2160p M [lead_black:120 flat_mix:77% narrow:52-197]; sollevante 2160p M [lead_black:193 cuts:22/min flat_mix:58%]; meridian 2160p M [lead_black:60 flat_mix:96% narrow:22-171].

Quality is read at the `def` anchor (x265 at its default threading, the point G2 and G3 are timed at). G1's anchor (one frame thread) is its own solve; its deltas are in the CSV.

## Supplementary quality (not the bar; median ours minus x265 over the class)
| class | tier | hmean NEG | min NEG | 1%-low NEG | first-1s NEG | VMAF | PSNR-YUV 6:1:1 | SSIM-YUV 6:1:1 | frames |
|---|---|---|---|---|---|---|---|---|---|
| CIF | S | +0.77 | +1.26 | +1.71 | +1.07 | - | -0.07 | -0.0015 | 3/3 ok |
| 720p | S | +1.27 | +0.83 | +1.26 | +2.37 | - | +0.05 | -0.0008 | 4/4 ok |
| 1080p | S | +1.08 | +2.21 | +0.89 | +2.38 | - | -0.04 | -0.0011 | 3/3 ok |
| 2160p | S | -0.51 | -6.09 | -7.50 | +0.15 | - | -0.68 | -0.0017 | 2/2 ok |
| 1080p | M | - | - | - | - | - | - | - | - |
| 2160p | M | - | - | - | - | - | - | - | - |

## Changed since the last report
shields (720p-S) G3: ratio 0.88 -> 0.83 (ccada12 -> b0d383f)

## Not valid or not measured
- pedestrian (1080p-S) G1: single-run; one run only (1 per arm); ours --crf 24.12 (solved); both y4m on stdin via cat
- riverbed (1080p-S) G1: single-run; one run only (1 per arm); ours --crf 24.63 (solved); both y4m on stdin via cat
- sunflower (1080p-S) G1: single-run; one run only (1 per arm); ours --crf 25.36 (solved); both y4m on stdin via cat
- tier M G1, speed not measured: chimera 1080p, sollevante 1080p, meridian 1080p, chimera 2160p, sollevante 2160p, meridian 2160p
- tier M G2, speed not measured: chimera 1080p, sollevante 1080p, meridian 1080p, chimera 2160p, sollevante 2160p, meridian 2160p
- tier M G3, speed not measured: chimera 1080p, sollevante 1080p, meridian 1080p, chimera 2160p, sollevante 2160p, meridian 2160p
- tier S G1, speed not measured: meridian_2160p 2160p, bbb_2160p 2160p
- combinations not measured (not counted in m): 1080p-S G1, 2160p-S G1, 1080p-M G1, 1080p-M G2, 1080p-M G3, 2160p-M G1, 2160p-M G2, 2160p-M G3
- quality not solved yet: chimera 1080p-M, sollevante 1080p-M, meridian 1080p-M, chimera 2160p-M, sollevante 2160p-M, meridian 2160p-M
- tiers L and C: not measured (later items).
- BD-VMAF-NEG over a five-rung ladder (tiers M and L): from the next round, per the spec; column empty.
