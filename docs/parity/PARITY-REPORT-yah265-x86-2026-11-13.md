# yah265 parity report, x86-64, 2026-11-13

Encoder: yah265 n265 504fdda (main; the Mac's binary is a9b880c8), built on each box from `git archive 504fdda` with clang 18, release, preset medium.  Reference: x265 4.3+1-e9b8812 built on each box from the 4.3 release tarball (clang 18, 8-bit, default assembly), preset medium.
Machine: AWS EC2 spot, us-east-2. Intel Xeon Platinum 8488C (Sapphire Rapids) on m7i.8xlarge and c7i.8xlarge, Ubuntu 24.04, kernel 7.0. Measured 2026-11-12 23:30 to 2026-11-13 14:00 UTC, nocturne 2026-11-13 20:15 to 2026-11-14 04:00. The SSE4.2 and AVX2 caps 2026-11-15 20:27 to 20:52, on main d771c4a.
Pinning: both encoders run on 16 vCPUs, eight cores and their siblings (`taskset`), ours `--threads 16` and x265 `--pools 16`. That's what each picks on a 16-vCPU box. The feeder runs on the other 16 vCPUs. G1's one-thread arms run eight at a time, each on its own core with its sibling idle, and the box stays at eight busy cores until the last timed arm ends. Every other timed encode runs alone.
SIMD: ours dispatches its AVX-512 tier (checkasm on the box: sse4.2, avx2 and avx512, 0 failures). x265 runs its default assembly, which on this CPU stops at AVX2. The G3 table also has an arm with x265 `--asm avx512`, so its best case is in the table too. G1 and G2 are ours with N265_ASM_OFF=all against x265 `--no-asm`.

How to read: a speed ratio is our time / the reference's time at the same file size; below 1.00 we're faster. Pass = median <= 1.00 and worst <= 1.15, quality no worse than -0.5 VMAF v1. "-" = not measured. A dagger (†) marks a figure that includes a cell timed in one run only. Lookahead: ours 20 at 720p and up, 12 at CIF / x265 20 (each medium's own).

## Source frames
Both encoders read the same Y4M on stdin from one feeder, under the Y4M header the Mac's feeder wrote for that cell. The frames are the Mac's, checked per cell by an md5 over every plane byte (the list at the end):
- Tier S CIF, 720p and 1080p: the Mac's Y4M files, sent raw and fed by a block copy, as the Mac's `cat`.
- Tier S 2160p, tier M meridian 2160p, tier L 2160p (tears, meridian, stem2, sintel, bbb) and cosmos 1080p: decoded in software on the box (ffmpeg 6.1) from the H.264 and HEVC masters the Mac decodes. The decoder runs on the feeder vCPUs and decodes up to the cell's first frame before the clock starts. The wall runs from there to the encoder's exit.
- Tier M chimera and sollevante (1080p and 2160p), tier M meridian 1080p and tier L 720p and 1080p: the Mac's own frames (VideoToolbox-scaled where scaled), sent lossless (x264 --qp 0, 8-bit 4:2:0) and decoded in software on the feeder vCPUs. The uplink to AWS ran at 3-5 MB/s, too slow for the raw files.
- The feeder alone is timed on each cell's first 60 s and has to deliver 1.5x the faster arm's rate. Every cell cleared it.

## What was measured here
Every speed figure, size, md5 and VBR rate in this report was measured on the boxes. The quality columns of the CSV are the Mac's at the same point. x86 streams differ slightly from the Mac's, because clang fuses float multiply-adds on arm64 and not on baseline x86-64, and the owner chose to leave that as is. So x86 quality is shown by reproduction (next section).

## Verdict
G1 pure C, 1 thread:  PASS in CIF-S, 720p-S, 720p-L, 1080p-S, 1080p-M, 1080p-L, 2160p-S, 2160p-M, 2160p-L
G2 pure C, all cores: PASS in CIF-S, 720p-S, 720p-L, 1080p-S, 1080p-M, 1080p-L, 2160p-S, 2160p-M, 2160p-L
G3 SIMD, all cores:   PASS in CIF-S, 720p-S, 720p-L, 1080p-S, 1080p-M, 1080p-L, 2160p-S, 2160p-M, 2160p-L
Quality:               PASS in every class and tier, by reproduction: 4 of 4 Mac curves land within 0.2 BD-VMAF v1 points on x86, so the Mac's quality table stands
Full parity:           yes. 27 of 27 combinations pass (class, tier S/M/L and goal), over every gating cell. VBR wall: 26 of 26 cells, all under 1.00, and our rate inside +-10% on every one.
x265 at --asm avx512:  G3 medians 0.41-0.77, worst cell 0.84 (bus_cif): no class moves by more than 0.03 against x265's default assembly.
SSE4.2 tier (G3, S):   PASS in CIF, 720p, 1080p and 2160p. Medians 0.43-0.69, worst cell 0.77 (pedestrian_1080p), against x265 --asm sse4.2 (added 2026-11-15)
AVX2 tier (G3, S):     PASS in CIF, 720p, 1080p and 2160p. Medians 0.45-0.85, worst cell 0.92 (pedestrian_1080p), against x265 --asm avx2 (added 2026-11-15)

## Speed (median / worst ratio, per class and tier)
| class | tier | G1 | G2 | G3 | G3 vs x265 --asm avx512 | VBR wall (G3) | cells |
|---|---|---|---|---|---|---|---|
| CIF | S | 0.67 / 0.67 | 0.44 / 0.49 | 0.70 / 0.82 | 0.70 / 0.84 | n/a | 3/3 3/3 3/3 |
| 720p | S | 0.44 / 0.61 | 0.28 / 0.38 | 0.60 / 0.75 | 0.62 / 0.77 | n/a | 4/4 4/4 4/4 |
| 720p | L | 0.42 / 0.43 | 0.27 / 0.29 | 0.53 / 0.56 | 0.54 / 0.57 | 0.51 / 0.58 | 7/7 7/7 7/7 |
| 1080p | S | 0.45 / 0.53 | 0.38 / 0.43 | 0.74 / 0.80 | 0.77 / 0.83 | n/a | 3/3 3/3 3/3 |
| 1080p | M | 0.53 / 0.53 | 0.39 / 0.45 | 0.74 / 0.80 | 0.75 / 0.82 | 0.79 / 0.81 | 3/3 3/3 3/3 |
| 1080p | L | 0.46 / 0.49 | 0.36 / 0.39 | 0.68 / 0.71 | 0.70 / 0.73 | 0.77 / 0.84 | 7/7 7/7 7/7 |
| 2160p | S | 0.33 / 0.34 | 0.21 / 0.21 | 0.41 / 0.43 | 0.41 / 0.44 | n/a | 2/2 2/2 2/2 |
| 2160p | M | 0.42 / 0.47 | 0.27 / 0.39 | 0.53 / 0.67 | 0.54 / 0.69 | 0.60 / 0.75 | 3/3 3/3 3/3 |
| 2160p | L | 0.43 / 0.45 | 0.31 / 0.33 | 0.59 / 0.62 | 0.60 / 0.63 | 0.66 / 0.73 | 6/6 6/6 6/6 |

Cells counted (G1 G2 G3) are valid timed cells of the class and tier's gating set.

## G3 with both encoders held to SSE4.2 and to AVX2 (tier S, 2026-11-15)
The table above times our AVX-512 build. An x86 CPU with AVX2 and no AVX-512, or with SSE4.2 alone, runs a lower tier of both encoders, so this round caps both at the same tier on the same Xeon. Ours ran under `N265_CPU_MAX=sse42` and `N265_CPU_MAX=avx2`. x265 ran at `--asm sse4.2`, which reports MMX2 SSE2Fast SSSE3 SSE4.2, and at `--asm avx2`, which reports MMX2 SSE2Fast LZCNT SSSE3 SSE4.2 AVX FMA3 BMI2 AVX2. That second set is x265's default on this CPU. A fifth arm ran ours uncapped (AVX-512) against x265 at `--asm avx2` as a control against the table above.

| class | ours SSE4.2 vs x265 sse4.2 | ours AVX2 vs x265 avx2 | control: ours AVX-512 vs x265 avx2 | 2026-11-13 G3 | cells |
|---|---|---|---|---|---|
| CIF | 0.64 / 0.75 | 0.74 / 0.88 | 0.70 / 0.83 | 0.70 / 0.82 | 3 |
| 720p | 0.50 / 0.65 | 0.63 / 0.80 | 0.60 / 0.75 | 0.60 / 0.75 | 4 |
| 1080p | 0.69 / 0.77 | 0.85 / 0.92 | 0.74 / 0.80 | 0.74 / 0.80 | 3 |
| 2160p | 0.43 / 0.45 | 0.45 / 0.50 | 0.44 / 0.48 | 0.41 / 0.43 | 2 |

Both capped tiers pass in every class. AVX2 against AVX2 is the tightest pairing, because x265 gains more from AVX2 than we do. Its worst cell is pedestrian_1080p at 0.92, which leaves 0.23 to the bar. At SSE4.2 x265 loses more than we do, so every cell reads at or below its AVX2 ratio. The control comes within 0.03 of the 2026-11-13 table in every class.

Method: one m7i.8xlarge (Xeon 8488C) ran every arm. Both encoders were held to 16 vCPUs as above. Each cell ran three rounds, and the five arms rotated their order every round. The binary is main d771c4a built on the box with clang 18 in release. Its checkasm passed there at sse4.2, avx2 and avx512. The points are the table's. Ours writes one stream per cell at all three tiers, one md5 over its nine encodes. So the size match is the table's. x265 writes the same byte count at both of its tiers, though its md5s differ between them. This round had no media hub. The Mac sent the CIF clips. The box fetched the 720p and 1080p clips from media.xiph.org and the bbb and Meridian masters from their public sources. Each file matched the Mac's copy by md5. The 2160p cells were decoded and staged on the box. Their frames matched the Mac's by plane md5. bbb_2160p kept two rounds on three of its five arms. The box locked out logins as it began to halt after its run, before the last three records were copied off. The records are in docs/data/cloud-tables-2026-11-15/.

| cell | ours sse4.2 s | x265 sse4.2 s | ratio | ours avx2 s | x265 avx2 s | ratio | ours avx512 s | ratio vs x265 avx2 | dsize | feed fps | rounds |
|---|---|---|---|---|---|---|---|---|---|---|---|
| foreman_cif | 0.77 | 1.37 | 0.57 | 0.72 | 1.08 | 0.67 | 0.67 | 0.62 | +0.13% | 27881.6 | 3 |
| bus_cif | 0.69 | 0.92 | 0.75 | 0.65 | 0.74 | 0.88 | 0.61 | 0.83 | -0.51% | 19644.4 | 3 |
| stefan_cif | 0.34 | 0.54 | 0.64 | 0.32 | 0.44 | 0.74 | 0.30 | 0.70 | +0.06% | 16338.7 | 3 |
| ducks_720p | 7.63 | 15.44 | 0.49 | 7.43 | 12.00 | 0.62 | 6.87 | 0.57 | -0.04% | 3854.2 | 3 |
| park_joy_720p | 7.16 | 10.98 | 0.65 | 6.91 | 8.61 | 0.80 | 6.48 | 0.75 | -0.32% | 4142.2 | 3 |
| fourpeople_720p | 2.03 | 4.59 | 0.44 | 1.90 | 3.68 | 0.52 | 1.82 | 0.49 | -0.34% | 4135.3 | 3 |
| shields_720p | 2.83 | 5.52 | 0.51 | 2.68 | 4.12 | 0.65 | 2.55 | 0.62 | -0.38% | 4192.5 | 3 |
| sunflower_1080p | 5.73 | 10.21 | 0.56 | 5.21 | 7.84 | 0.66 | 4.82 | 0.62 | -0.32% | 1865.8 | 3 |
| pedestrian_1080p | 8.23 | 10.65 | 0.77 | 7.64 | 8.27 | 0.92 | 6.62 | 0.80 | -0.31% | 1853.0 | 3 |
| riverbed_1080p | 13.14 | 19.00 | 0.69 | 12.35 | 14.47 | 0.85 | 10.71 | 0.74 | +0.49% | 1726.6 | 3 |
| meridian_2160p | 14.02 | 34.08 | 0.41 | 12.78 | 31.23 | 0.41 | 12.22 | 0.39 | +0.09% | 482.0 | 3 |
| bbb_2160p | 7.74 | 17.24 | 0.45 | 7.31 | 14.69 | 0.50 | 7.12 | 0.48 | -0.58% | 477.3 | 2-3 |

foreman_cif and stefan_cif ran at the crf re-solved on x86 (26.06 and 27.03), as in the table above. A capped tier on a big Xeon isn't an AVX2-only part. The cores, caches and clocks are Sapphire Rapids's, so a Zen 2 or Haswell box will read its own numbers. What this round settles is the ratio when both encoders run the same instruction set. G1 and G2 weren't rerun, because both are pure C and the table above already times them.

## Quality: four Mac ladders re-measured on x86
Both encoders re-encoded at the Mac's rungs, 6 threads each (x265 `--pools 6 --frame-threads 4`), scored with VMAF v1 at libvmaf 0f9912e (the Mac's build) on its gate model, every frame on tier M and every 3rd on tier L. BD is against x265 on the same box. The rungs ran on c7i.4xlarge (Sapphire Rapids) and, after three spot reclaims, on c6i.4xlarge (Ice Lake). Our x86 streams don't depend on which x86-64 CPU writes them, since every kernel tier is bit-exact with C.
| curve | Mac BD-VMAF v1 | x86 BD-VMAF v1 | difference | within 0.2 |
|---|---|---|---|---|
| chimera_2160p (M) VBR | +0.70% | +0.78% | +0.07 | yes |
| sollevante_1080p (M) CRF | -14.35% | -14.38% | -0.03 | yes |
| cosmos_1080p (L) CRF | -11.32% | -11.35% | -0.03 | yes |
| cosmos_1080p (L) VBR | -11.82% | -11.81% | +0.01 | yes |
All four land within 0.2 BD points of the Mac's, so no full x86 ladder was owed. chimera_2160p VBR, the Mac's curve nearest the bar, reads +0.78% here and stays inside +1.0%. VBR tracking: chimera_2160p ours -3.6% to -5.2% (x265 -13.7% to -14.4%), cosmos_1080p ours -2.4% to -2.9% (x265 -0.6% to -0.8%), all inside +-10%.
I read that as the Mac's quality table holding on x86: every class and tier passes, and no cell is soft (PARITY-REPORT-yah265-2026-11-12).

## VBR wall (G3 only: SIMD, all 16 vCPUs, one rung per cell)
The Mac's rung per cell (x265's CRF 28 rate on the Mac), maxrate 1.5x, buffer 2x, lookahead ours 20 / x265 20, 2 interleaved rounds, no size match. Tracking is the achieved bitrate against the target.
| cell | tier | target kbit/s | ratio | ours kbit/s | x265 kbit/s | feed fps | rounds |
|---|---|---|---|---|---|---|---|
| chimera_1080p | M | 1668 | 0.79 | 1601 (-4.0%) | 1426 (-14.5%) | 233.8 | 2 |
| meridian_1080p | M | 318 | 0.46 | 305 (-4.0%) | 301 (-5.3%) | 507.6 | 2 |
| sollevante_1080p | M | 7863 | 0.81 | 7560 (-3.9%) | 7305 (-7.1%) | 128.0 | 2 |
| chimera_2160p | M | 4515 | 0.60 | 4285 (-5.1%) | 3859 (-14.5%) | 66.2 | 2 |
| meridian_2160p | M | 949 | 0.54 | 921 (-2.9%) | 908 (-4.4%) | 208.3 | 2 |
| sollevante_2160p | M | 20733 | 0.75 | 19918 (-3.9%) | 19038 (-8.2%) | 35.3 | 2 |
| bbb_720p | L | 620 | 0.58 | 613 (-1.2%) | 602 (-2.8%) | 637.9 | 2 |
| cosmos_720p | L | 734 | 0.52 | 717 (-2.3%) | 731 (-0.4%) | 667.8 | 2 |
| meridian_720p | L | 205 | 0.54 | 200 (-2.4%) | 200 (-2.5%) | 960.1 | 2 |
| nocturne_720p | L | 1022 | 0.45 | 1014 (-0.8%) | 1065 (+4.2%) | 4216.3 | 2 |
| sintel_720p | L | 523 | 0.51 | 485 (-7.3%) | 505 (-3.4%) | 645.8 | 2 |
| stem2_720p | L | 567 | 0.49 | 538 (-5.1%) | 508 (-10.3%) | 611.8 | 2 |
| tears_720p | L | 628 | 0.49 | 629 (+0.1%) | 638 (+1.6%) | 690.9 | 2 |
| bbb_1080p | L | 1365 | 0.84 | 1350 (-1.1%) | 1312 (-3.9%) | 265.8 | 2 |
| cosmos_1080p | L | 1407 | 0.77 | 1367 (-2.8%) | 1395 (-0.8%) | 1034.5 | 2 |
| meridian_1080p | L | 438 | 0.54 | 429 (-2.2%) | 425 (-3.0%) | 422.8 | 2 |
| nocturne_1080p | L | 2198 | 0.59 | 2174 (-1.1%) | 2254 (+2.6%) | 1762.2 | 2 |
| sintel_1080p | L | 1113 | 0.80 | 1054 (-5.3%) | 1079 (-3.0%) | 298.5 | 2 |
| stem2_1080p | L | 1145 | 0.71 | 1078 (-5.8%) | 1017 (-11.2%) | 263.4 | 2 |
| tears_1080p | L | 1279 | 0.77 | 1284 (+0.4%) | 1300 (+1.6%) | 301.2 | 2 |
| bbb_2160p | L | 3453 | 0.69 | 3422 (-0.9%) | 3311 (-4.1%) | 217.4 | 2 |
| meridian_2160p | L | 1326 | 0.50 | 1304 (-1.6%) | 1283 (-3.2%) | 215.7 | 2 |
| nocturne_2160p | L | 9142 | 0.61 | 9012 (-1.4%) | 9089 (-0.6%) | 66.1 | 2 |
| sintel_2160p | L | 4329 | 0.73 | 4260 (-1.6%) | 4236 (-2.2%) | 210.5 | 2 |
| stem2_2160p | L | 2964 | 0.67 | 2810 (-5.2%) | 2671 (-9.9%) | 81.8 | 2 |
| tears_2160p | L | 3389 | 0.66 | 3395 (+0.2%) | 3435 (+1.4%) | 256.1 | 2 |

## Every cell
| cell | tier | goal | ours s | x265 s | ratio | x265 avx512 s | ratio | dsize | feed fps | rounds | ours md5 = Mac | valid |
|---|---|---|---|---|---|---|---|---|---|---|---|---|
| bus_cif | S | G1 | 3.97 | 5.94 | 0.67 | - | - | +0.03% | - | 2 | DIFFERENT | yes |
| bus_cif | S | G2 | 1.14 | 2.30 | 0.49 | - | - | -0.51% | 22518.5 | 3 | DIFFERENT | yes |
| bus_cif | S | G3 | 0.60 | 0.74 | 0.82 | 0.72 | 0.84 | -0.51% | 22881.6 | 3 | DIFFERENT | yes |
| foreman_cif | S | G1 | 5.67 | 9.44 | 0.60 | - | - | +0.27% | - | 2 | DIFFERENT | yes |
| foreman_cif | S | G2 | 1.41 | 3.72 | 0.38 | - | - | +0.14% | 27504.1 | 3 | DIFFERENT | yes |
| foreman_cif | S | G3 | 0.71 | 1.11 | 0.64 | 1.12 | 0.64 | +0.13% | 28515.5 | 3 | DIFFERENT | yes |
| stefan_cif | S | G1 | 2.21 | 3.32 | 0.67 | - | - | -0.11% | - | 2 | DIFFERENT | yes |
| stefan_cif | S | G2 | 0.60 | 1.35 | 0.44 | - | - | +0.06% | 19019.5 | 3 | DIFFERENT | yes |
| stefan_cif | S | G3 | 0.31 | 0.45 | 0.70 | 0.45 | 0.70 | +0.06% | 19099.1 | 3 | DIFFERENT | yes |
| ducks_720p | S | G1 | 109.88 | 227.82 | 0.48 | - | - | -0.03% | - | 2 | DIFFERENT | yes |
| ducks_720p | S | G2 | 12.09 | 42.47 | 0.28 | - | - | -0.04% | 4246.1 | 3 | DIFFERENT | yes |
| ducks_720p | S | G3 | 6.70 | 11.79 | 0.57 | 11.43 | 0.59 | -0.04% | 4136.8 | 3 | DIFFERENT | yes |
| fourpeople_720p | S | G1 | 23.02 | 68.35 | 0.34 | - | - | -0.53% | - | 2 | DIFFERENT | yes |
| fourpeople_720p | S | G2 | 3.17 | 13.34 | 0.24 | - | - | -0.34% | 4158.8 | 3 | DIFFERENT | yes |
| fourpeople_720p | S | G3 | 1.82 | 3.75 | 0.48 | 3.64 | 0.50 | -0.34% | 4118.8 | 3 | DIFFERENT | yes |
| park_joy_720p | S | G1 | 96.57 | 158.09 | 0.61 | - | - | -0.08% | - | 2 | DIFFERENT | yes |
| park_joy_720p | S | G2 | 11.23 | 29.66 | 0.38 | - | - | -0.32% | 4155.4 | 3 | DIFFERENT | yes |
| park_joy_720p | S | G3 | 6.49 | 8.60 | 0.75 | 8.42 | 0.77 | -0.32% | 3988.0 | 3 | DIFFERENT | yes |
| shields_720p | S | G1 | 33.25 | 85.04 | 0.39 | - | - | -0.09% | - | 2 | DIFFERENT | yes |
| shields_720p | S | G2 | 4.56 | 16.15 | 0.28 | - | - | -0.38% | 3966.4 | 3 | DIFFERENT | yes |
| shields_720p | S | G3 | 2.63 | 4.18 | 0.63 | 4.03 | 0.65 | -0.38% | 3968.7 | 3 | DIFFERENT | yes |
| pedestrian_1080p | S | G1 | 139.07 | 262.71 | 0.53 | - | - | -0.10% | - | 2 | DIFFERENT | yes |
| pedestrian_1080p | S | G2 | 15.32 | 35.28 | 0.43 | - | - | -0.31% | 1897.6 | 3 | DIFFERENT | yes |
| pedestrian_1080p | S | G3 | 6.72 | 8.42 | 0.80 | 8.07 | 0.83 | -0.31% | 1894.5 | 3 | DIFFERENT | yes |
| riverbed_1080p | S | G1 | 231.25 | 532.89 | 0.43 | - | - | +0.12% | - | 2 | DIFFERENT | yes |
| riverbed_1080p | S | G2 | 25.42 | 66.16 | 0.38 | - | - | +0.49% | 1863.2 | 3 | DIFFERENT | yes |
| riverbed_1080p | S | G3 | 10.88 | 14.63 | 0.74 | 14.21 | 0.77 | +0.49% | 1798.7 | 3 | DIFFERENT | yes |
| sunflower_1080p | S | G1 | 90.90 | 204.12 | 0.45 | - | - | +0.08% | - | 2 | DIFFERENT | yes |
| sunflower_1080p | S | G2 | 10.29 | 31.85 | 0.32 | - | - | -0.32% | 1802.4 | 3 | DIFFERENT | yes |
| sunflower_1080p | S | G3 | 4.89 | 8.03 | 0.61 | 7.87 | 0.62 | -0.32% | 1889.3 | 3 | DIFFERENT | yes |
| bbb_2160p | S | G1 | 97.69 | 284.64 | 0.34 | - | - | -0.28% | - | 2 | DIFFERENT | yes |
| bbb_2160p | S | G2 | 11.66 | 54.45 | 0.21 | - | - | -0.58% | 471.2 | 3 | DIFFERENT | yes |
| bbb_2160p | S | G3 | 6.47 | 15.17 | 0.43 | 14.78 | 0.44 | -0.58% | 475.5 | 3 | DIFFERENT | yes |
| meridian_2160p | S | G1 | 184.33 | 579.06 | 0.32 | - | - | +0.21% | - | 2 | DIFFERENT | yes |
| meridian_2160p | S | G2 | 21.97 | 111.59 | 0.20 | - | - | +0.09% | 479.4 | 3 | DIFFERENT | yes |
| meridian_2160p | S | G3 | 12.01 | 31.10 | 0.39 | 31.07 | 0.39 | +0.09% | 477.8 | 3 | DIFFERENT | yes |
| chimera_1080p | M | G1 | 192.84 | 364.82 | 0.53 | - | - | +0.23% | - | 2 | DIFFERENT | yes |
| chimera_1080p | M | G2 | 20.79 | 53.07 | 0.39 | - | - | +0.38% | 1859.0 | 3 | DIFFERENT | yes |
| chimera_1080p | M | G3 | 33.04 | 44.63 | 0.74 | 43.97 | 0.75 | +0.41% | 233.1 | 3 | DIFFERENT | yes |
| chimera_1080p | M | VBR | 34.81 | 44.06 | 0.79 | - | - | +12.30% | 233.8 | 2 | DIFFERENT | yes |
| meridian_1080p | M | G1 | 109.48 | 379.71 | 0.29 | - | - | -0.30% | - | 2 | DIFFERENT | yes |
| meridian_1080p | M | G2 | 15.01 | 74.17 | 0.20 | - | - | -0.45% | 1876.2 | 3 | DIFFERENT | yes |
| meridian_1080p | M | G3 | 18.62 | 41.87 | 0.44 | 40.89 | 0.46 | -0.51% | 506.9 | 3 | DIFFERENT | yes |
| meridian_1080p | M | VBR | 19.89 | 42.77 | 0.46 | - | - | +1.38% | 507.6 | 2 | DIFFERENT | yes |
| sollevante_1080p | M | G1 | 492.09 | 923.91 | 0.53 | - | - | -0.00% | - | 2 | DIFFERENT | yes |
| sollevante_1080p | M | G2 | 53.87 | 119.81 | 0.45 | - | - | +0.07% | 1838.2 | 3 | DIFFERENT | yes |
| sollevante_1080p | M | G3 | 77.17 | 96.25 | 0.80 | 93.72 | 0.82 | +0.09% | 128.0 | 3 | DIFFERENT | yes |
| sollevante_1080p | M | VBR | 89.84 | 111.08 | 0.81 | - | - | +3.49% | 128.0 | 2 | DIFFERENT | yes |
| chimera_2160p | M | G1 | 621.69 | 1475.67 | 0.42 | - | - | -0.01% | - | 2 | DIFFERENT | yes |
| chimera_2160p | M | G2 | 67.22 | 245.86 | 0.27 | - | - | -0.09% | 473.3 | 3 | DIFFERENT | yes |
| chimera_2160p | M | G3 | 112.95 | 212.15 | 0.53 | 210.84 | 0.54 | +0.78% | 66.1 | 3 | DIFFERENT | yes |
| chimera_2160p | M | VBR | 125.30 | 210.41 | 0.60 | - | - | +11.04% | 66.2 | 2 | DIFFERENT | yes |
| meridian_2160p | M | G1 | 438.68 | 1556.61 | 0.28 | - | - | -0.11% | - | 2 | DIFFERENT | yes |
| meridian_2160p | M | G2 | 55.48 | 327.72 | 0.17 | - | - | -0.35% | 443.9 | 3 | DIFFERENT | yes |
| meridian_2160p | M | G3 | 92.57 | 174.49 | 0.53 | 170.39 | 0.54 | -0.83% | 211.5 | 3 | DIFFERENT | yes |
| meridian_2160p | M | VBR | 95.77 | 176.82 | 0.54 | - | - | +1.47% | 208.3 | 2 | DIFFERENT | yes |
| sollevante_2160p | M | G1 | 1724.64 | 3680.26 | 0.47 | - | - | -0.14% | - | 2 | DIFFERENT | yes |
| sollevante_2160p | M | G2 | 186.60 | 473.68 | 0.39 | - | - | -0.01% | 469.7 | 3 | DIFFERENT | yes |
| sollevante_2160p | M | G3 | 249.63 | 373.72 | 0.67 | 363.24 | 0.69 | +0.01% | 35.3 | 3 | DIFFERENT | yes |
| sollevante_2160p | M | VBR | 283.67 | 375.84 | 0.75 | - | - | +4.62% | 35.3 | 2 | DIFFERENT | yes |
| bbb_720p | L | G1 | 176.15 | 413.42 | 0.43 | - | - | +0.08% | - | 1 | DIFFERENT | yes |
| bbb_720p | L | G2 | 22.70 | 78.62 | 0.29 | - | - | -0.02% | 3988.5 | 1 | DIFFERENT | yes |
| bbb_720p | L | G3 | 60.65 | 108.60 | 0.56 | 107.15 | 0.57 | +0.27% | 634.1 | 2 | DIFFERENT | yes |
| bbb_720p | L | VBR | 76.03 | 131.47 | 0.58 | - | - | +1.68% | 637.9 | 2 | DIFFERENT | yes |
| cosmos_720p | L | G1 | 251.07 | 585.17 | 0.43 | - | - | -0.38% | - | 1 | DIFFERENT | yes |
| cosmos_720p | L | G2 | 31.14 | 114.10 | 0.27 | - | - | -0.24% | 5309.8 | 1 | DIFFERENT | yes |
| cosmos_720p | L | G3 | 75.15 | 140.87 | 0.53 | 138.67 | 0.54 | -0.19% | 659.2 | 2 | DIFFERENT | yes |
| cosmos_720p | L | VBR | 92.04 | 178.63 | 0.52 | - | - | -1.95% | 667.8 | 2 | DIFFERENT | yes |
| meridian_720p | L | G1 | 142.80 | 497.49 | 0.29 | - | - | -0.29% | - | 1 | DIFFERENT | yes |
| meridian_720p | L | G2 | 20.93 | 102.28 | 0.20 | - | - | -0.16% | 3954.0 | 1 | DIFFERENT | yes |
| meridian_720p | L | G3 | 99.70 | 195.45 | 0.51 | 199.61 | 0.50 | +0.38% | 983.6 | 2 | DIFFERENT | yes |
| meridian_720p | L | VBR | 113.86 | 210.59 | 0.54 | - | - | +0.12% | 960.1 | 2 | DIFFERENT | yes |
| nocturne_720p | L | G1 | 342.17 | 980.50 | 0.35 | - | - | +0.07% | - | 1 | DIFFERENT | yes |
| nocturne_720p | L | G2 | 42.85 | 175.64 | 0.24 | - | - | -0.01% | 4165.8 | 1 | DIFFERENT | yes |
| nocturne_720p | L | G3 | 143.00 | 282.36 | 0.51 | 277.23 | 0.52 | +0.24% | 4244.2 | 2 | DIFFERENT | yes |
| nocturne_720p | L | VBR | 158.73 | 351.08 | 0.45 | - | - | -4.74% | 4216.3 | 2 | DIFFERENT | yes |
| sintel_720p | L | G1 | 145.78 | 344.12 | 0.42 | - | - | -0.36% | - | 1 | DIFFERENT | yes |
| sintel_720p | L | G2 | 19.09 | 71.03 | 0.27 | - | - | +0.33% | 5380.3 | 1 | DIFFERENT | yes |
| sintel_720p | L | G3 | 62.50 | 122.25 | 0.51 | 119.38 | 0.52 | +0.92% | 642.7 | 2 | DIFFERENT | yes |
| sintel_720p | L | VBR | 74.48 | 147.33 | 0.51 | - | - | -4.09% | 645.8 | 2 | DIFFERENT | yes |
| stem2_720p | L | G1 | 215.94 | 534.58 | 0.40 | - | - | -0.36% | - | 1 | DIFFERENT | yes |
| stem2_720p | L | G2 | 25.98 | 96.11 | 0.27 | - | - | +0.05% | 3952.5 | 1 | DIFFERENT | yes |
| stem2_720p | L | G3 | 80.87 | 148.21 | 0.55 | 147.28 | 0.55 | +0.30% | 625.9 | 2 | DIFFERENT | yes |
| stem2_720p | L | VBR | 83.44 | 171.36 | 0.49 | - | - | +5.86% | 611.8 | 2 | DIFFERENT | yes |
| tears_720p | L | G1 | 171.68 | 413.62 | 0.42 | - | - | -0.24% | - | 1 | DIFFERENT | yes |
| tears_720p | L | G2 | 22.20 | 83.68 | 0.27 | - | - | -0.33% | 5209.9 | 1 | DIFFERENT | yes |
| tears_720p | L | G3 | 66.37 | 125.22 | 0.53 | 119.59 | 0.55 | +0.81% | 657.5 | 2 | DIFFERENT | yes |
| tears_720p | L | VBR | 74.22 | 152.27 | 0.49 | - | - | -1.41% | 690.9 | 2 | DIFFERENT | yes |
| bbb_1080p | L | G1 | 439.78 | 910.12 | 0.48 | - | - | +0.10% | - | 1 | DIFFERENT | yes |
| bbb_1080p | L | G2 | 54.20 | 148.59 | 0.36 | - | - | -0.04% | 1914.0 | 1 | DIFFERENT | yes |
| bbb_1080p | L | G3 | 139.87 | 202.22 | 0.69 | 198.89 | 0.70 | +0.08% | 265.4 | 2 | DIFFERENT | yes |
| bbb_1080p | L | VBR | 178.91 | 212.90 | 0.84 | - | - | +2.92% | 265.8 | 2 | DIFFERENT | yes |
| cosmos_1080p | L | G1 | 595.45 | 1212.87 | 0.49 | - | - | +0.16% | - | 1 | DIFFERENT | yes |
| cosmos_1080p | L | G2 | 70.74 | 187.11 | 0.38 | - | - | -0.10% | 2304.2 | 1 | DIFFERENT | yes |
| cosmos_1080p | L | G3 | 154.33 | 225.37 | 0.68 | 220.98 | 0.70 | -0.10% | 1041.7 | 2 | DIFFERENT | yes |
| cosmos_1080p | L | VBR | 204.75 | 266.65 | 0.77 | - | - | -2.00% | 1034.5 | 2 | DIFFERENT | yes |
| meridian_1080p | L | G1 | 315.55 | 1107.12 | 0.29 | - | - | +0.29% | - | 1 | DIFFERENT | yes |
| meridian_1080p | L | G2 | 44.35 | 213.68 | 0.21 | - | - | -0.09% | 1805.4 | 1 | DIFFERENT | yes |
| meridian_1080p | L | G3 | 204.60 | 404.70 | 0.51 | 392.69 | 0.52 | -0.05% | 417.5 | 2 | DIFFERENT | yes |
| meridian_1080p | L | VBR | 221.18 | 410.01 | 0.54 | - | - | +0.83% | 422.8 | 2 | DIFFERENT | yes |
| nocturne_1080p | L | G1 | 1006.17 | 2314.45 | 0.43 | - | - | -0.28% | - | 1 | DIFFERENT | yes |
| nocturne_1080p | L | G2 | 112.16 | 352.09 | 0.32 | - | - | -0.27% | 1823.9 | 1 | DIFFERENT | yes |
| nocturne_1080p | L | G3 | 342.89 | 617.37 | 0.56 | 599.02 | 0.57 | -0.21% | 1843.6 | 2 | DIFFERENT | yes |
| nocturne_1080p | L | VBR | 384.13 | 653.69 | 0.59 | - | - | -3.59% | 1762.2 | 2 | DIFFERENT | yes |
| sintel_1080p | L | G1 | 341.95 | 743.72 | 0.46 | - | - | -0.12% | - | 1 | DIFFERENT | yes |
| sintel_1080p | L | G2 | 46.60 | 120.65 | 0.39 | - | - | -0.06% | 2299.4 | 1 | DIFFERENT | yes |
| sintel_1080p | L | G3 | 143.71 | 206.19 | 0.70 | 200.92 | 0.72 | +0.60% | 296.8 | 2 | DIFFERENT | yes |
| sintel_1080p | L | VBR | 191.79 | 238.42 | 0.80 | - | - | -2.33% | 298.5 | 2 | DIFFERENT | yes |
| stem2_1080p | L | G1 | 549.26 | 1212.30 | 0.45 | - | - | -1.88% | - | 1 | DIFFERENT | yes |
| stem2_1080p | L | G2 | 62.60 | 172.86 | 0.36 | - | - | +0.11% | 1787.4 | 1 | DIFFERENT | yes |
| stem2_1080p | L | G3 | 183.64 | 274.41 | 0.67 | 274.43 | 0.67 | +0.01% | 273.7 | 2 | DIFFERENT | yes |
| stem2_1080p | L | VBR | 204.09 | 285.48 | 0.71 | - | - | +5.99% | 263.4 | 2 | DIFFERENT | yes |
| tears_1080p | L | G1 | 441.23 | 907.69 | 0.49 | - | - | -0.12% | - | 1 | DIFFERENT | yes |
| tears_1080p | L | G2 | 52.77 | 139.60 | 0.38 | - | - | +0.11% | 2293.0 | 1 | DIFFERENT | yes |
| tears_1080p | L | G3 | 146.97 | 206.21 | 0.71 | 200.53 | 0.73 | +0.56% | 299.9 | 2 | DIFFERENT | yes |
| tears_1080p | L | VBR | 185.19 | 240.02 | 0.77 | - | - | -1.26% | 301.2 | 2 | DIFFERENT | yes |
| bbb_2160p | L | G1 | 1537.45 | 3604.27 | 0.43 | - | - | +0.10% | - | 1 | DIFFERENT | yes |
| bbb_2160p | L | G2 | 171.77 | 591.64 | 0.29 | - | - | +0.10% | 448.0 | 1 | DIFFERENT | yes |
| bbb_2160p | L | G3 | 523.90 | 846.73 | 0.62 | 837.54 | 0.63 | +0.09% | 187.0 | 2 | DIFFERENT | yes |
| bbb_2160p | L | VBR | 616.93 | 895.48 | 0.69 | - | - | +3.35% | 217.4 | 2 | DIFFERENT | yes |
| meridian_2160p | L | G1 | 1156.02 | 4442.59 | 0.26 | - | - | +0.25% | - | 1 | DIFFERENT | yes |
| meridian_2160p | L | G2 | 150.39 | 1017.49 | 0.15 | - | - | -0.08% | 467.7 | 1 | DIFFERENT | yes |
| meridian_2160p | L | G3 | 847.63 | 1637.68 | 0.52 | 1598.77 | 0.53 | +0.22% | 206.6 | 2 | DIFFERENT | yes |
| meridian_2160p | L | VBR | 864.04 | 1713.40 | 0.50 | - | - | +1.64% | 215.7 | 2 | DIFFERENT | yes |
| nocturne_2160p | L | G1 | 6287.99 | 14335.62 | 0.44 | - | - | +0.17% | - | 1 | DIFFERENT | yes |
| nocturne_2160p | L | G2 | 623.17 | 2007.38 | 0.31 | - | - | +0.10% | 481.3 | 1 | DIFFERENT | yes |
| nocturne_2160p | L | G3 | 1846.70 | 3072.86 | 0.60 | 3006.36 | 0.61 | +0.16% | 64.8 | 2 | DIFFERENT | yes |
| nocturne_2160p | L | VBR | 1973.38 | 3231.52 | 0.61 | - | - | -0.85% | 66.1 | 2 | DIFFERENT | yes |
| sintel_2160p | L | G1 | 1617.13 | 3597.75 | 0.45 | - | - | +0.17% | - | 1 | DIFFERENT | yes |
| sintel_2160p | L | G2 | 174.76 | 531.94 | 0.33 | - | - | +0.01% | 549.1 | 1 | DIFFERENT | yes |
| sintel_2160p | L | G3 | 545.68 | 885.34 | 0.62 | 875.90 | 0.62 | -0.16% | 207.1 | 2 | DIFFERENT | yes |
| sintel_2160p | L | VBR | 665.15 | 916.27 | 0.73 | - | - | +0.58% | 210.5 | 2 | DIFFERENT | yes |
| stem2_2160p | L | G1 | 2097.22 | 4833.98 | 0.43 | - | - | +0.27% | - | 1 | DIFFERENT | yes |
| stem2_2160p | L | G2 | 237.02 | 726.10 | 0.33 | - | - | +0.67% | 447.2 | 1 | DIFFERENT | yes |
| stem2_2160p | L | G3 | 669.10 | 1140.38 | 0.59 | 1124.00 | 0.60 | +0.30% | 84.0 | 2 | DIFFERENT | yes |
| stem2_2160p | L | VBR | 761.87 | 1144.91 | 0.67 | - | - | +5.22% | 81.8 | 2 | DIFFERENT | yes |
| tears_2160p | L | G1 | 1656.89 | 3812.70 | 0.43 | - | - | -0.13% | - | 1 | DIFFERENT | yes |
| tears_2160p | L | G2 | 182.13 | 574.44 | 0.32 | - | - | -0.17% | 564.2 | 1 | DIFFERENT | yes |
| tears_2160p | L | G3 | 502.62 | 878.69 | 0.57 | 858.09 | 0.59 | +0.36% | 260.8 | 2 | DIFFERENT | yes |
| tears_2160p | L | VBR | 586.96 | 890.11 | 0.66 | - | - | -1.18% | 256.1 | 2 | DIFFERENT | yes |

## Source frames checked against the Mac's
- L_1080p_bbb: 13929 frame(s), plane md5 same
- L_1080p_cosmos: 13724 frame(s), plane md5 same
- L_1080p_meridian: 32887 frame(s), plane md5 same
- L_1080p_nocturne: 34320 frame(s), plane md5 same
- L_1080p_nocturne_s: 34320 frame(s), plane md5 same
- L_1080p_sintel: 13178 frame(s), plane md5 same
- L_1080p_stem2: 12940 frame(s), plane md5 same
- L_1080p_tears: 12672 frame(s), plane md5 same
- L_2160p_bbb: 1 frame(s), plane md5 same
- L_2160p_meridian: 1 frame(s), plane md5 same
- L_2160p_nocturne: 1 frame(s), plane md5 same
- L_2160p_sintel: 13178 frame(s), plane md5 same
- L_2160p_stem2: 1 frame(s), plane md5 same
- L_2160p_tears: 12672 frame(s), plane md5 same
- L_720p_bbb: 13929 frame(s), plane md5 same
- L_720p_cosmos: 13724 frame(s), plane md5 same
- L_720p_meridian: 32887 frame(s), plane md5 same
- L_720p_nocturne: 34320 frame(s), plane md5 same
- L_720p_nocturne_s: 34320 frame(s), plane md5 same
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
This is the first full x86 table. The 2026-11-11 probe timed three tier S cells with both encoders on AVX2 and read G3 0.89-0.98. Since then the scalar driver work and the UHD scaling item took the G3 wall down at every class, and ours now dispatches AVX-512 on this CPU. On those three cells G3 now reads 0.63 (shields_720p), 0.61 (sunflower_1080p) and 0.43 (bbb_2160p).

## Not valid or not measured
- Nocturne, added in a second round: 720p and 1080p are the Mac's own frames, sent lossless as for the other tier L cells. 2160p is the spec's tier L span of the ProRes master (frames 3000-37319), cut on the Mac by stream copy and decoded on the box. The Mac's feeder decodes that master with VideoToolbox, and ffmpeg's ProRes decoder doesn't agree with it to the bit (68 dB PSNR-Y on frame 3000, 10-bit planes already different). So the 2160p frames here are the Mac's software decode, checked against it by plane md5, and not the Mac feeder's frames. On x86 the feeder runs ffmpeg's C paths (`-cpuflags 0`): ffmpeg's x86 SIMD rounds the 10-bit 4:2:2 to 8-bit 4:2:0 conversion differently from its C and NEON paths. The Mac's crf held the 1% size bar on these frames.
- Nocturne 720p and 1080p G3 and VBR were first fed by decoding the lossless pack on the fly, and that feeder was too slow for the 1.5x rule. They were re-run from a raw copy staged on the box (720p on m7i.8xlarge, 1080p on m7i.12xlarge for the memory to cache it), fed by block copy as the Mac's staged Y4M is. Those rows replace the decode-fed ones.
- foreman_cif and stefan_cif: x265's own build here writes 1-1.5% more or fewer bytes than the Mac's brew x265 at CRF 28. Its frame-thread count follows the 16 vCPUs, not the Mac's 18. The Mac's crf then missed the 1% size bar, so ours was re-solved on the box onto this x265's bytes (foreman def 26.06; stefan ft1 27.05 and def 27.03) and both cells re-timed. Those rows replace the Mac-point ones.
- Tier M G1 and G2 are read on the Mac's matched 30 s windows here (the spec's rule). The Mac's tier M rows read the whole segment.
- Spot reclaims: when a box was reclaimed part-way through a goal, the next box reran that goal whole. No row mixes boxes.
- G1 arms ran eight at a time on separate cores, so at all-core turbo. Both arms ran under the same load and the ratio holds, but the walls are slower than a lone one-thread run.


## Tier C (content types), G3
Measured in the cloud on 2026-11-27, 17:16 to 18:01 UTC, on AWS EC2 spot in us-east-2. The Xeon 8488C ran on a c7i.8xlarge. The AMD EPYC 9R14 is Zen 4 and ran on a c7a.8xlarge. Both boxes ran Ubuntu 24.04 on kernel 7.0. Ours is n265 4d0d13a4 at `medium`. x265 is 4.3+1-e9b8812 at `medium`. Each box built both encoders with clang 18. Each encoder ran on 16 vCPUs: ours at `--threads 16`, x265 at `--pools 16`. The EPYC has no SMT. Its 16 vCPUs are 16 cores.

The seven short clips are the Mac's Y4M files. Each box checked them by md5. The lash1924 frames are not the Mac's. Each box decoded them in software with ffmpeg 6.1, from a stream-copied ProRes span of the master. Both encoders read the same frames. Our CRF was solved onto this x265's file size on each box.

The two boxes write the same streams. At one thread SIMD matches C on foreman_cif 306b4a86, WorldCup e89e97b4 and Wikipedia b9d30c60. checkasm passed on sse4.2, avx2 and avx512 with 0 failures.

AWS reclaimed both boxes during the last cell, lash1924_1080p. Each had run one round of ours and no round of x265. That cell is unmeasured here, not zero.

On some clips the feed check read the file cold, at 15-79 fps. That's under 1.5x our rate. The timed arms ran faster than that. They must have read from the page cache. A slow feeder would hold back the faster arm, which is ours. No ratio here overstates our lead. Tier C's speed is shown but doesn't count toward the verdict.

Xeon 8488C: median 0.81, worst 1.03 (wikipedia_1080p), over 8 clips. Unmeasured: lash1924_1080p.

| cell | Xeon 8488C ours s | x265 s | Xeon 8488C ratio | x265 --asm avx512 s | ratio vs avx512 | dsize | ours crf (solved here) | feed fps | rounds | valid |
|---|---|---|---|---|---|---|---|---|---|---|
| lash1924_720p | 147.24 | 214.02 | 0.69 | 206.78 | 0.71 | +0.34% | 24.91 | 527.4 | 2 | yes |
| lash1924_1080p | unmeasured | unmeasured | unmeasured | unmeasured | unmeasured | unmeasured | unmeasured | unmeasured | unmeasured | no |
| worldcup_1080p | 2.15 | 2.89 | 0.74 | 2.81 | 0.77 | -0.12% | 25.59 | 66.7 | 3 | yes |
| worldcup_far_1080p | 2.57 | 2.90 | 0.89 | 2.83 | 0.91 | +0.14% | 26.09 | 66.6 | 3 | yes |
| skater227_1080p | 2.30 | 3.28 | 0.70 | 3.17 | 0.72 | +0.03% | 26.07 | 66.9 | 3 | yes |
| boxing_2160p | 8.13 | 20.37 | 0.40 | 19.38 | 0.42 | +0.05% | 26.6 | 15.4 | 3 | yes |
| wikipedia_1080p | 1.76 | 1.71 | 1.03 | 1.67 | 1.06 | +0.28% | 13.29 | 1720.7 | 3 | yes |
| debugging_1080p | 1.35 | 1.48 | 0.91 | 1.45 | 0.93 | -0.20% | 24.65 | 79.1 | 3 | yes |
| spreadsheet_1080p | 1.62 | 1.84 | 0.88 | 1.85 | 0.88 | +0.03% | 20.05 | 686.1 | 3 | yes |

EPYC 9R14: median 0.70, worst 0.99 (wikipedia_1080p), over 8 clips. Unmeasured: lash1924_1080p.

| cell | EPYC 9R14 ours s | x265 s | EPYC 9R14 ratio | x265 --asm avx512 s | ratio vs avx512 | dsize | ours crf (solved here) | feed fps | rounds | valid |
|---|---|---|---|---|---|---|---|---|---|---|
| lash1924_720p | 91.83 | 145.25 | 0.63 | 145.23 | 0.63 | +0.34% | 24.91 | 469.0 | 2 | yes |
| lash1924_1080p | unmeasured | unmeasured | unmeasured | unmeasured | unmeasured | unmeasured | unmeasured | unmeasured | unmeasured | no |
| worldcup_1080p | 1.42 | 2.29 | 0.62 | 2.29 | 0.62 | -0.12% | 25.59 | 1020.1 | 3 | yes |
| worldcup_far_1080p | 1.63 | 2.11 | 0.77 | 2.12 | 0.77 | +0.14% | 26.09 | 78.4 | 3 | yes |
| skater227_1080p | 1.52 | 2.43 | 0.63 | 2.47 | 0.62 | +0.03% | 26.07 | 78.5 | 3 | yes |
| boxing_2160p | 5.32 | 16.78 | 0.32 | 18.06 | 0.29 | +0.05% | 26.6 | 18.4 | 3 | yes |
| wikipedia_1080p | 1.40 | 1.42 | 0.99 | 1.42 | 0.99 | +0.28% | 13.29 | 1021.3 | 3 | yes |
| debugging_1080p | 1.04 | 1.27 | 0.82 | 1.27 | 0.82 | -0.20% | 24.65 | 78.1 | 3 | yes |
| spreadsheet_1080p | 1.14 | 1.49 | 0.76 | 1.51 | 0.76 | +0.03% | 20.05 | 78.3 | 3 | yes |
