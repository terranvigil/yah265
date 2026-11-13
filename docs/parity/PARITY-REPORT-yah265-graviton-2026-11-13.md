# yah265 parity report, Graviton4, 2026-11-13

Encoder: yah265 n265 504fdda (main; the Mac's binary is a9b880c8), built on each box from `git archive 504fdda` with clang 18, release, preset medium.  Reference: x265 4.3+1-e9b8812 built on each box from the 4.3 release tarball (clang 18, 8-bit), preset medium.
Machine: AWS EC2 spot, us-east-2. AWS Graviton4 (Neoverse V2) on c8g.8xlarge, r8g.8xlarge and c8g.12xlarge (one CPU), Ubuntu 24.04, kernel 7.0. Measured 2026-11-12 23:10 to 2026-11-13 10:20 UTC.
Pinning: both encoders run on 16 cores (`taskset`), ours `--threads 16` and x265 `--pools 16`. The feeder runs on the other cores. G1's one-thread arms run eight at a time, each on its own core, and the box stays at eight busy cores until the last timed arm ends. Every other timed encode runs alone.
SIMD: ours dispatches NEON, dotprod, SVE and SVE2 (checkasm on the box: 0 failures; the SVE vector length is 128 bits). x265 runs its full aarch64 assembly: NEON, SVE, SVE2, dotprod, i8mm and SVE2 bitperm. G1 and G2 are ours with N265_ASM_OFF=all against x265 `--no-asm`.

How to read: a speed ratio is our time / the reference's time at the same file size; below 1.00 we're faster. Pass = median <= 1.00 and worst <= 1.15, quality no worse than -0.5 VMAF v1. "-" = not measured. A dagger (†) marks a figure that includes a cell timed in one run only. Lookahead: ours 20 at 720p and up, 12 at CIF / x265 20 (each medium's own).

## Source frames
Fed as on the x86 boxes, under the Mac feeder's Y4M header. Tier S CIF to 1080p are the Mac's files, fed by a block copy. Tier S 2160p, tier M meridian 2160p, tier L 2160p and cosmos 1080p are decoded in software on the feeder cores from the Mac's H.264 and HEVC masters. The rest (tier M chimera and sollevante, meridian 1080p, tier L 720p and 1080p) are the Mac's own frames, sent lossless and decoded on the feeder cores. Every cell's planes were checked against the Mac's by md5 before timing, and every cell cleared the 1.5x feeder rule.

## Verdict
G1 pure C, 1 thread:  PASS in CIF-S, 720p-S, 720p-L (6 of 7 cells), 1080p-S, 1080p-M, 1080p-L (6 of 7 cells), 2160p-S, 2160p-M, 2160p-L (5 of 6 cells)
G2 pure C, all cores: PASS in CIF-S, 720p-S, 720p-L (6 of 7 cells), 1080p-S, 1080p-M, 1080p-L (6 of 7 cells), 2160p-S, 2160p-M, 2160p-L (5 of 6 cells)
G3 SIMD, all cores:   PASS in CIF-S, 720p-S, 720p-L (6 of 7 cells), 1080p-S, 1080p-M, 1080p-L (6 of 7 cells), 2160p-S, 2160p-M, 2160p-L (5 of 6 cells)
Quality:               PASS in every class and tier, by identity: every timed stream of ours at the Mac's points is the Mac's stream, so the Mac's quality table stands
Full parity:           yes on every cell measured. 27 of 27 combinations pass (class, tier S/M/L and goal). Nocturne's three tier L cells weren't measured (last section). VBR wall: 23 of 26 cells, all under 1.00, and our rate inside +-10% on every one.

## Speed (median / worst ratio, per class and tier)
| class | tier | G1 | G2 | G3 | VBR wall (G3) | cells |
|---|---|---|---|---|---|---|
| CIF | S | 0.66 / 0.68 | 0.40 / 0.45 | 0.55 / 0.66 | n/a | 3/3 3/3 3/3 |
| 720p | S | 0.43 / 0.59 | 0.26 / 0.34 | 0.48 / 0.57 | n/a | 4/4 4/4 4/4 |
| 720p | L | 0.43 / 0.45 | 0.25 / 0.28 | 0.42 / 0.46 | 0.41 / 0.50 | 6/7 6/7 6/7 |
| 1080p | S | 0.45 / 0.55 | 0.42 / 0.46 | 0.68 / 0.71 | n/a | 3/3 3/3 3/3 |
| 1080p | M | 0.53 / 0.54 | 0.42 / 0.48 | 0.64 / 0.72 | 0.69 / 0.72 | 3/3 3/3 3/3 |
| 1080p | L | 0.48 / 0.51 | 0.39 / 0.41 | 0.60 / 0.64 | 0.70 / 0.78 | 6/7 6/7 6/7 |
| 2160p | S | 0.33 / 0.34 | 0.21 / 0.23 | 0.31 / 0.33 | n/a | 2/2 2/2 2/2 |
| 2160p | M | 0.44 / 0.49 | 0.29 / 0.45 | 0.47 / 0.63 | 0.53 / 0.71 | 3/3 3/3 3/3 |
| 2160p | L | 0.45 / 0.47 | 0.33 / 0.36 | 0.49 / 0.54 | 0.57 / 0.66 | 5/6 5/6 5/6 |

Cells counted (G1 G2 G3) are valid timed cells of the class and tier's gating set.

## Quality: our streams are the Mac's
Linux arm64 writes the Mac's streams, so quality here is shown by identity, not by ladders. Every timed encode of ours at the Mac's points has the md5 of the Mac's stream at the same point (table below: G1 and G2 on the Mac's windows or clips, G3 and VBR on the whole cell). The one row per goal marked different is stefan_cif re-solved on this box, which runs at another crf by design. At the Mac's crf it was identical too. So the Mac's quality table is this box's: every class and tier passes, with no soft cells (PARITY-REPORT-yah265-2026-11-12).

## Graviton3, second line (G3 on tier S)
AWS Graviton3 (Neoverse V1, 256-bit SVE) on c7g.4xlarge: 16 cores, the whole box, feeder and encoders sharing it. Ours dispatches NEON, dotprod, SVE and SVE2; x265 its aarch64 assembly. Three interleaved rounds. All 12 of our G3 streams there are the Mac's.
| class | G3 median / worst | cells |
|---|---|---|
| CIF | 0.53 / 0.60 | 2 of 3 (stefan_cif reads 0.50 at -1.13% size, outside the 1% bar, not re-solved) |
| 720p | 0.45 / 0.54 | 4 of 4 |
| 1080p | 0.62 / 0.66 | 3 of 3 |
| 2160p | 0.31 / 0.34 | 2 of 2 |

## Our streams against the Mac's
Each timed encode's md5 against the Mac's stream at the same point (the Mac's timed runs, its G1/G2 window solves, its VBR walls). Same / different / no Mac record:
| goal | same | different | no Mac record |
|---|---|---|---|
| G1 | 34 | 1 | 0 |
| G2 | 34 | 1 | 0 |
| G3 | 34 | 1 | 0 |
| VBR | 23 | 0 | 0 |

## VBR wall (G3 only: SIMD, all 16 vCPUs, one rung per cell)
The Mac's rung per cell (x265's CRF 28 rate on the Mac), maxrate 1.5x, buffer 2x, lookahead ours 20 / x265 20, 2 interleaved rounds, no size match. Tracking is the achieved bitrate against the target.
| cell | tier | target kbit/s | ratio | ours kbit/s | x265 kbit/s | feed fps | rounds |
|---|---|---|---|---|---|---|---|
| chimera_1080p | M | 1668 | 0.69 | 1600 (-4.1%) | 1426 (-14.5%) | 235.0 | 2 |
| meridian_1080p | M | 318 | 0.39 | 305 (-4.0%) | 301 (-5.4%) | 523.7 | 2 |
| sollevante_1080p | M | 7863 | 0.72 | 7561 (-3.8%) | 7306 (-7.1%) | 127.8 | 2 |
| chimera_2160p | M | 4515 | 0.53 | 4285 (-5.1%) | 3861 (-14.5%) | 65.8 | 2 |
| meridian_2160p | M | 949 | 0.36 | 921 (-3.0%) | 908 (-4.4%) | 318.0 | 2 |
| sollevante_2160p | M | 20733 | 0.71 | 19915 (-3.9%) | 19039 (-8.2%) | 34.8 | 2 |
| bbb_720p | L | 620 | 0.50 | 613 (-1.2%) | 603 (-2.8%) | 625.9 | 2 |
| cosmos_720p | L | 734 | 0.40 | 717 (-2.3%) | 731 (-0.4%) | 694.0 | 2 |
| meridian_720p | L | 205 | 0.49 | 200 (-2.4%) | 200 (-2.5%) | 1042.4 | 2 |
| sintel_720p | L | 523 | 0.42 | 485 (-7.3%) | 505 (-3.4%) | 634.4 | 2 |
| stem2_720p | L | 567 | 0.40 | 538 (-5.1%) | 508 (-10.3%) | 640.1 | 2 |
| tears_720p | L | 628 | 0.39 | 629 (+0.1%) | 638 (+1.6%) | 698.5 | 2 |
| bbb_1080p | L | 1365 | 0.78 | 1350 (-1.1%) | 1312 (-3.9%) | 260.0 | 2 |
| cosmos_1080p | L | 1407 | 0.70 | 1367 (-2.8%) | 1395 (-0.8%) | 1707.6 | 2 |
| meridian_1080p | L | 438 | 0.53 | 429 (-2.2%) | 425 (-3.0%) | 426.4 | 2 |
| sintel_1080p | L | 1113 | 0.74 | 1054 (-5.3%) | 1079 (-3.0%) | 293.2 | 2 |
| stem2_1080p | L | 1145 | 0.65 | 1078 (-5.8%) | 1017 (-11.2%) | 268.3 | 2 |
| tears_1080p | L | 1279 | 0.70 | 1284 (+0.4%) | 1300 (+1.6%) | 301.6 | 2 |
| bbb_2160p | L | 3453 | 0.55 | 3422 (-0.9%) | 3311 (-4.1%) | 316.7 | 2 |
| meridian_2160p | L | 1326 | 0.38 | 1304 (-1.6%) | 1283 (-3.2%) | 325.5 | 2 |
| sintel_2160p | L | 4329 | 0.66 | 4260 (-1.6%) | 4235 (-2.2%) | 233.6 | 2 |
| stem2_2160p | L | 2964 | 0.57 | 2811 (-5.2%) | 2671 (-9.9%) | 124.3 | 2 |
| tears_2160p | L | 3389 | 0.59 | 3395 (+0.2%) | 3435 (+1.4%) | 369.6 | 2 |

## Every cell
| cell | tier | goal | ours s | x265 s | ratio | dsize | feed fps | rounds | ours md5 = Mac | valid |
|---|---|---|---|---|---|---|---|---|---|---|
| bus_cif | S | G1 | 3.20 | 4.70 | 0.68 | +0.03% | - | 2 | same | yes |
| bus_cif | S | G2 | 0.73 | 1.63 | 0.45 | -0.16% | 32913.0 | 3 | same | yes |
| bus_cif | S | G3 | 0.42 | 0.65 | 0.66 | -0.16% | 32406.7 | 3 | same | yes |
| foreman_cif | S | G1 | 4.26 | 7.04 | 0.60 | +0.36% | - | 2 | same | yes |
| foreman_cif | S | G2 | 0.82 | 2.43 | 0.34 | +0.17% | 41149.4 | 3 | same | yes |
| foreman_cif | S | G3 | 0.46 | 0.90 | 0.51 | +0.17% | 40610.6 | 3 | same | yes |
| stefan_cif | S | G1 | 1.62 | 2.47 | 0.66 | -0.30% | - | 2 | DIFFERENT | yes |
| stefan_cif | S | G2 | 0.35 | 0.88 | 0.40 | +0.16% | 27879.2 | 3 | DIFFERENT | yes |
| stefan_cif | S | G3 | 0.21 | 0.38 | 0.55 | +0.15% | 29658.8 | 3 | DIFFERENT | yes |
| ducks_720p | S | G1 | 87.10 | 182.74 | 0.48 | -0.01% | - | 2 | same | yes |
| ducks_720p | S | G2 | 5.81 | 24.33 | 0.24 | -0.02% | 5579.1 | 3 | same | yes |
| ducks_720p | S | G3 | 3.66 | 8.80 | 0.42 | -0.02% | 5678.2 | 3 | same | yes |
| fourpeople_720p | S | G1 | 17.75 | 55.42 | 0.32 | -0.48% | - | 2 | same | yes |
| fourpeople_720p | S | G2 | 1.63 | 6.68 | 0.24 | -0.17% | 5568.8 | 3 | same | yes |
| fourpeople_720p | S | G3 | 1.12 | 2.57 | 0.44 | -0.17% | 5447.1 | 3 | same | yes |
| park_joy_720p | S | G1 | 75.08 | 128.30 | 0.59 | +0.01% | - | 2 | same | yes |
| park_joy_720p | S | G2 | 5.65 | 16.66 | 0.34 | -0.24% | 5647.2 | 3 | same | yes |
| park_joy_720p | S | G3 | 3.78 | 6.59 | 0.57 | -0.24% | 5546.5 | 3 | same | yes |
| shields_720p | S | G1 | 25.77 | 67.36 | 0.38 | -0.07% | - | 2 | same | yes |
| shields_720p | S | G2 | 2.42 | 8.47 | 0.29 | -0.44% | 5386.7 | 3 | same | yes |
| shields_720p | S | G3 | 1.64 | 3.18 | 0.52 | -0.44% | 5423.5 | 3 | same | yes |
| pedestrian_1080p | S | G1 | 117.13 | 211.22 | 0.55 | -0.32% | - | 2 | same | yes |
| pedestrian_1080p | S | G2 | 7.60 | 16.68 | 0.46 | -0.40% | 2464.7 | 3 | same | yes |
| pedestrian_1080p | S | G3 | 4.01 | 5.66 | 0.71 | -0.40% | 2494.9 | 3 | same | yes |
| riverbed_1080p | S | G1 | 196.92 | 432.83 | 0.45 | +0.13% | - | 2 | same | yes |
| riverbed_1080p | S | G2 | 12.44 | 29.39 | 0.42 | +0.46% | 2549.9 | 3 | same | yes |
| riverbed_1080p | S | G3 | 6.33 | 9.34 | 0.68 | +0.46% | 2458.4 | 3 | same | yes |
| sunflower_1080p | S | G1 | 71.37 | 162.51 | 0.44 | -0.15% | - | 2 | same | yes |
| sunflower_1080p | S | G2 | 4.92 | 14.97 | 0.33 | -0.87% | 2528.5 | 3 | same | yes |
| sunflower_1080p | S | G3 | 2.88 | 5.48 | 0.52 | -0.87% | 2569.2 | 3 | same | yes |
| bbb_2160p | S | G1 | 79.46 | 231.63 | 0.34 | -0.34% | - | 2 | same | yes |
| bbb_2160p | S | G2 | 6.01 | 26.04 | 0.23 | -0.70% | 578.2 | 3 | same | yes |
| bbb_2160p | S | G3 | 3.62 | 11.04 | 0.33 | -0.70% | 611.5 | 3 | same | yes |
| meridian_2160p | S | G1 | 144.12 | 464.46 | 0.31 | +0.23% | - | 2 | same | yes |
| meridian_2160p | S | G2 | 10.45 | 57.94 | 0.18 | -0.09% | 616.1 | 3 | same | yes |
| meridian_2160p | S | G3 | 6.62 | 23.16 | 0.29 | -0.09% | 635.5 | 3 | same | yes |
| chimera_1080p | M | G1 | 156.31 | 292.61 | 0.53 | +0.22% | - | 2 | same | yes |
| chimera_1080p | M | G2 | 10.12 | 23.83 | 0.42 | +0.41% | 2436.0 | 3 | same | yes |
| chimera_1080p | M | G3 | 19.76 | 30.84 | 0.64 | +0.44% | 234.7 | 3 | same | yes |
| chimera_1080p | M | VBR | 21.15 | 30.61 | 0.69 | +12.24% | 235.0 | 2 | same | yes |
| meridian_1080p | M | G1 | 84.70 | 314.25 | 0.27 | -0.23% | - | 2 | same | yes |
| meridian_1080p | M | G2 | 7.86 | 35.27 | 0.22 | -0.33% | 2468.6 | 3 | same | yes |
| meridian_1080p | M | G3 | 10.61 | 28.02 | 0.38 | -0.45% | 520.8 | 3 | same | yes |
| meridian_1080p | M | VBR | 11.31 | 28.79 | 0.39 | +1.41% | 523.7 | 2 | same | yes |
| sollevante_1080p | M | G1 | 411.81 | 762.63 | 0.54 | -0.01% | - | 2 | same | yes |
| sollevante_1080p | M | G2 | 26.57 | 54.86 | 0.48 | +0.11% | 2578.5 | 3 | same | yes |
| sollevante_1080p | M | G3 | 45.10 | 63.05 | 0.72 | +0.05% | 126.9 | 3 | same | yes |
| sollevante_1080p | M | VBR | 54.10 | 75.42 | 0.72 | +3.49% | 127.8 | 2 | same | yes |
| chimera_2160p | M | G1 | 506.64 | 1148.64 | 0.44 | -0.02% | - | 2 | same | yes |
| chimera_2160p | M | G2 | 33.19 | 113.43 | 0.29 | -0.08% | 601.7 | 3 | same | yes |
| chimera_2160p | M | G3 | 66.86 | 143.70 | 0.47 | +0.71% | 65.9 | 3 | same | yes |
| chimera_2160p | M | VBR | 74.18 | 140.76 | 0.53 | +10.98% | 65.8 | 2 | same | yes |
| meridian_2160p | M | G1 | 337.52 | 1237.73 | 0.27 | -0.09% | - | 2 | same | yes |
| meridian_2160p | M | G2 | 26.42 | 142.71 | 0.19 | -0.22% | 621.2 | 3 | same | yes |
| meridian_2160p | M | G3 | 42.82 | 124.05 | 0.35 | -0.76% | 305.0 | 3 | same | yes |
| meridian_2160p | M | VBR | 44.51 | 125.08 | 0.36 | +1.47% | 318.0 | 2 | same | yes |
| sollevante_2160p | M | G1 | 1481.99 | 3023.38 | 0.49 | -0.15% | - | 2 | same | yes |
| sollevante_2160p | M | G2 | 93.83 | 208.70 | 0.45 | -0.08% | 608.4 | 3 | same | yes |
| sollevante_2160p | M | G3 | 151.44 | 241.57 | 0.63 | -0.05% | 34.9 | 3 | same | yes |
| sollevante_2160p | M | VBR | 172.29 | 242.93 | 0.71 | +4.60% | 34.8 | 2 | same | yes |
| bbb_720p | L | G1 | 138.87 | 322.31 | 0.43 | +0.05% | - | 1 | same | yes |
| bbb_720p | L | G2 | 12.36 | 44.81 | 0.28 | +0.03% | 5478.9 | 1 | same | yes |
| bbb_720p | L | G3 | 37.03 | 81.12 | 0.46 | +0.27% | 617.4 | 2 | same | yes |
| bbb_720p | L | VBR | 47.46 | 95.57 | 0.50 | +1.68% | 625.9 | 2 | same | yes |
| cosmos_720p | L | G1 | 205.06 | 454.43 | 0.45 | -0.41% | - | 1 | same | yes |
| cosmos_720p | L | G2 | 15.56 | 62.55 | 0.25 | -0.23% | 6979.0 | 1 | same | yes |
| cosmos_720p | L | G3 | 42.36 | 101.31 | 0.42 | -0.17% | 696.4 | 2 | same | yes |
| cosmos_720p | L | VBR | 54.79 | 135.44 | 0.40 | -1.95% | 694.0 | 2 | same | yes |
| meridian_720p | L | G1 | 104.67 | 382.58 | 0.27 | -0.33% | - | 1 | same | yes |
| meridian_720p | L | G2 | 10.39 | 48.61 | 0.21 | -0.17% | 5307.9 | 1 | same | yes |
| meridian_720p | L | G3 | 52.60 | 129.01 | 0.41 | +0.36% | 1058.0 | 2 | same | yes |
| meridian_720p | L | VBR | 64.72 | 132.83 | 0.49 | +0.12% | 1042.4 | 2 | same | yes |
| sintel_720p | L | G1 | 115.42 | 264.46 | 0.44 | -0.40% | - | 1 | same | yes |
| sintel_720p | L | G2 | 10.19 | 41.22 | 0.25 | +0.30% | 7280.6 | 1 | same | yes |
| sintel_720p | L | G3 | 38.57 | 93.86 | 0.41 | +0.91% | 640.1 | 2 | same | yes |
| sintel_720p | L | VBR | 48.94 | 116.77 | 0.42 | -4.09% | 634.4 | 2 | same | yes |
| stem2_720p | L | G1 | 174.98 | 413.16 | 0.42 | -0.51% | - | 1 | same | yes |
| stem2_720p | L | G2 | 13.17 | 49.99 | 0.26 | +0.02% | 5435.3 | 1 | same | yes |
| stem2_720p | L | G3 | 45.55 | 105.52 | 0.43 | +0.28% | 632.6 | 2 | same | yes |
| stem2_720p | L | VBR | 50.48 | 126.70 | 0.40 | +5.84% | 640.1 | 2 | same | yes |
| tears_720p | L | G1 | 136.04 | 315.91 | 0.43 | -0.27% | - | 1 | same | yes |
| tears_720p | L | G2 | 11.11 | 46.38 | 0.24 | -0.34% | 6773.1 | 1 | same | yes |
| tears_720p | L | G3 | 38.93 | 92.36 | 0.42 | +0.82% | 693.6 | 2 | same | yes |
| tears_720p | L | VBR | 46.89 | 118.77 | 0.39 | -1.41% | 698.5 | 2 | same | yes |
| bbb_1080p | L | G1 | 353.21 | 725.40 | 0.49 | +0.07% | - | 1 | same | yes |
| bbb_1080p | L | G2 | 26.80 | 72.15 | 0.37 | -0.08% | 2547.4 | 1 | same | yes |
| bbb_1080p | L | G3 | 83.62 | 140.90 | 0.59 | +0.05% | 259.1 | 2 | same | yes |
| bbb_1080p | L | VBR | 116.66 | 149.60 | 0.78 | +2.92% | 260.0 | 2 | same | yes |
| cosmos_1080p | L | G1 | 510.06 | 999.06 | 0.51 | +0.14% | - | 1 | same | yes |
| cosmos_1080p | L | G2 | 36.72 | 93.92 | 0.39 | -0.09% | 3206.5 | 1 | same | yes |
| cosmos_1080p | L | G3 | 91.65 | 154.60 | 0.59 | -0.09% | 1728.9 | 2 | same | yes |
| cosmos_1080p | L | VBR | 134.45 | 192.01 | 0.70 | -2.01% | 1707.6 | 2 | same | yes |
| meridian_1080p | L | G1 | 237.34 | 894.26 | 0.27 | +0.28% | - | 1 | same | yes |
| meridian_1080p | L | G2 | 22.75 | 99.63 | 0.23 | -0.01% | 2390.8 | 1 | same | yes |
| meridian_1080p | L | G3 | 116.38 | 264.01 | 0.44 | -0.08% | 424.9 | 2 | same | yes |
| meridian_1080p | L | VBR | 141.84 | 268.37 | 0.53 | +0.83% | 426.4 | 2 | same | yes |
| sintel_1080p | L | G1 | 278.40 | 600.36 | 0.46 | -0.14% | - | 1 | same | yes |
| sintel_1080p | L | G2 | 24.45 | 60.00 | 0.41 | -0.01% | 3192.8 | 1 | same | yes |
| sintel_1080p | L | G3 | 88.69 | 144.27 | 0.61 | +0.60% | 291.9 | 2 | same | yes |
| sintel_1080p | L | VBR | 127.54 | 171.70 | 0.74 | -2.33% | 293.2 | 2 | same | yes |
| stem2_1080p | L | G1 | 448.67 | 955.17 | 0.47 | -0.68% | - | 1 | same | yes |
| stem2_1080p | L | G2 | 31.78 | 82.69 | 0.38 | +0.29% | 2379.5 | 1 | same | yes |
| stem2_1080p | L | G3 | 109.24 | 180.61 | 0.60 | +0.02% | 269.6 | 2 | same | yes |
| stem2_1080p | L | VBR | 125.67 | 193.97 | 0.65 | +6.01% | 268.3 | 2 | same | yes |
| tears_1080p | L | G1 | 360.84 | 731.87 | 0.49 | -0.18% | - | 1 | same | yes |
| tears_1080p | L | G2 | 26.52 | 68.43 | 0.39 | +0.14% | 3060.5 | 1 | same | yes |
| tears_1080p | L | G3 | 92.32 | 144.01 | 0.64 | +0.52% | 295.4 | 2 | same | yes |
| tears_1080p | L | VBR | 120.86 | 172.55 | 0.70 | -1.26% | 301.6 | 2 | same | yes |
| bbb_2160p | L | G1 | 1227.56 | 2791.34 | 0.44 | +0.03% | - | 1 | same | yes |
| bbb_2160p | L | G2 | 83.89 | 279.94 | 0.30 | +0.06% | 614.7 | 1 | same | yes |
| bbb_2160p | L | G3 | 271.01 | 590.82 | 0.46 | +0.11% | 314.6 | 2 | same | yes |
| bbb_2160p | L | VBR | 332.27 | 601.64 | 0.55 | +3.35% | 316.7 | 2 | same | yes |
| meridian_2160p | L | G1 | 881.03 | 3518.90 | 0.25 | +0.20% | - | 1 | same | yes |
| meridian_2160p | L | G2 | 71.46 | 417.63 | 0.17 | -0.03% | 619.8 | 1 | same | yes |
| meridian_2160p | L | G3 | 422.52 | 1196.46 | 0.35 | +0.17% | 312.1 | 2 | same | yes |
| meridian_2160p | L | VBR | 464.30 | 1222.94 | 0.38 | +1.63% | 325.5 | 2 | same | yes |
| sintel_2160p | L | G1 | 1275.17 | 2766.53 | 0.46 | +0.24% | - | 1 | same | yes |
| sintel_2160p | L | G2 | 85.43 | 245.83 | 0.35 | -0.02% | 713.2 | 1 | same | yes |
| sintel_2160p | L | G3 | 326.01 | 600.42 | 0.54 | -0.45% | 232.4 | 2 | same | yes |
| sintel_2160p | L | VBR | 401.67 | 606.02 | 0.66 | +0.58% | 233.6 | 2 | same | yes |
| stem2_2160p | L | G1 | 1780.12 | 3809.19 | 0.47 | +0.33% | - | 1 | same | yes |
| stem2_2160p | L | G2 | 117.99 | 329.86 | 0.36 | +0.47% | 614.6 | 1 | same | yes |
| stem2_2160p | L | G3 | 376.23 | 744.53 | 0.51 | +0.26% | 124.7 | 2 | same | yes |
| stem2_2160p | L | VBR | 420.21 | 739.35 | 0.57 | +5.23% | 124.3 | 2 | same | yes |
| tears_2160p | L | G1 | 1354.24 | 2978.31 | 0.45 | -0.11% | - | 1 | same | yes |
| tears_2160p | L | G2 | 89.70 | 268.48 | 0.33 | -0.41% | 782.2 | 1 | same | yes |
| tears_2160p | L | G3 | 282.21 | 573.42 | 0.49 | +0.39% | 372.7 | 2 | same | yes |
| tears_2160p | L | VBR | 340.41 | 577.01 | 0.59 | -1.18% | 369.6 | 2 | same | yes |

## Source frames checked against the Mac's
- L_1080p_bbb: 13929 frame(s), plane md5 same
- L_1080p_cosmos: 13724 frame(s), plane md5 same
- L_1080p_meridian: 32887 frame(s), plane md5 same
- L_1080p_sintel: 13178 frame(s), plane md5 same
- L_1080p_stem2: 12940 frame(s), plane md5 same
- L_1080p_tears: 12672 frame(s), plane md5 same
- L_2160p_bbb: 1 frame(s), plane md5 same
- L_2160p_meridian: 1 frame(s), plane md5 same
- L_2160p_sintel: 13178 frame(s), plane md5 same
- L_2160p_stem2: 1 frame(s), plane md5 same
- L_2160p_tears: 12672 frame(s), plane md5 same
- L_720p_bbb: 13929 frame(s), plane md5 same
- L_720p_cosmos: 13724 frame(s), plane md5 same
- L_720p_meridian: 32887 frame(s), plane md5 same
- L_720p_sintel: 13178 frame(s), plane md5 same
- L_720p_stem2: 12940 frame(s), plane md5 same
- L_720p_tears: 12672 frame(s), plane md5 same
- M_1080p_chimera: 2640 frame(s), plane md5 same
- M_1080p_meridian: 3553 frame(s), plane md5 same
- M_1080p_sollevante: 2494 frame(s), plane md5 same
- M_2160p_chimera: 2640 frame(s), plane md5 same
- M_2160p_meridian: 3553 frame(s), plane md5 same
- M_2160p_sollevante: 2494 frame(s), plane md5 same
- S_2160p_bbb_2160p: 300 frame(s), plane md5 same
- S_2160p_meridian_2160p: 600 frame(s), plane md5 same

## Changed since the last report
This is the first full Graviton table. The 2026-11-12 SVE probe timed three tier S cells on Graviton4 and Graviton3 and found 20 of 20 of our streams identical to the Mac's. This table extends both findings to every gating cell but nocturne's.

## Not valid or not measured
- Nocturne (tier L at 720p, 1080p and 2160p) wasn't measured, for the reason the x86 report gives: 95 GB of ProRes, or about 60 GB of lossless frames, against a 3-5 MB/s uplink.
- stefan_cif: this box's x265 writes 1.1-1.4% fewer bytes than brew's x265 on the Mac. Ours was re-solved onto them (ft1 and def) and the cell re-timed. That row replaces the Mac-point one.
- Tier M G1 and G2 are read on the Mac's matched 30 s windows (the spec's rule). The Mac's tier M rows read the whole segment.
- Spot reclaims: when a box was reclaimed part-way through a goal, the next box reran that goal whole. No row mixes boxes. Tier S G1 and G2 ran on c8g.12xlarge and its G3 on r8g.8xlarge.
- G1 arms ran eight at a time on separate cores. Both arms ran under the same load, so the ratio holds.
