# yah265 parity report, every architecture, 2026-11-14

- Encoder: yah265 n265 at main d771c4a, preset medium.
- Mac binary: build-rel ab8a99eb (main 360ab7d), carried by identity to a9b880c8 (main 504fdda) and then to 3ee7824e (main d771c4a).
- Cloud binary: 504fdda built on each box from `git archive` with clang 18.
- Reference: x265 4.3+1-e9b8812, preset medium. Homebrew's build on the Mac, the 4.3 release tarball built with clang 18 on each cloud box.
- Mac: Apple M5 Max, 18 cores, macOS 26.6.2, arm64 NEON.
- x86-64: Intel Xeon Platinum 8488C (Sapphire Rapids) on EC2 m7i and c7i spot, AVX-512 over AVX2.
- Graviton4: Neoverse V2 on EC2 c8g and r8g spot, NEON and SVE2 at 128 bits.
- Graviton3: Neoverse V1 on c7g.4xlarge, SVE at 256 bits. A second line, tier S G3 only.
- Measured: Mac timed legs 2026-09-26 20:02 to 2026-11-12 14:07. Tier C on the Mac 2026-11-13 23:24 to 2026-11-14 03:17 EST, on a9b880c8. Items 61 and 62 re-measured four tier C clips on 2026-11-14, on their own binaries. Cloud 2026-11-12 23:10 to 2026-11-14 04:00 UTC. The capped SSE4.2 and AVX2 run on the x86 box 2026-11-15 20:27 to 20:52 UTC, on main d771c4a.
- Sources: the three reports PARITY-REPORT-yah265-2026-11-12, PARITY-REPORT-yah265-x86-2026-11-13 and PARITY-REPORT-yah265-graviton-2026-11-13 with their CSVs. The cloud records are in docs/data/cloud-tables-2026-11-13/.

The new measurements on this page are tier C, on the Mac, and the four clips items 61 and 62 moved. Their points, ladders and timed rows are in docs/data/parity-spec-points.json, parity-spec-ladders.json and parity-spec.csv.

How to read: a speed ratio is our time over the reference's time at the same file size. Below 1.00 we're faster. Pass means median <= 1.00 and worst <= 1.15, with quality no worse than -0.5 VMAF v1. "-" means not measured. Each cell shows median / worst over the class and tier's valid cells. Lookahead is each medium's own on every machine. Ours is 20 at 720p and up and 12 at CIF, and x265's is 20. The Mac runs at auto threads on all 18 cores. On each cloud box both encoders are held to 16 vCPUs by `taskset`. The feeder runs on the other cores.

## Verdict
Is yah265 at full parity on every architecture? Yes on every instruction set that was timed. Every gating combination of tiers S, M and L passes on all three machines that were timed. Tier C's quality also counts, and it now passes on all nine clips. SSE4.2 and AVX2 were timed on 2026-11-15 as tiers of their own, on tier S G3, with both encoders capped at the same tier on the x86 box. Both pass in every class. The worst cell is pedestrian_1080p at 0.92 under AVX2. Because both tiers are bit-exact with C, they write the streams the x86 table measured, and their quality holds for that reason.

Tier C first ran on a9b880c8 and failed on three 1080p cells: worldcup_far and the two screen clips wikipedia and spreadsheet. Two items fixed them. Item 61 finds screen content and follows its scrolls. Item 62 finds a handheld camera's jitter and keeps long B runs through it. Wikipedia's CRF curve went from +57.69% to -63.91% BD-v1. Spreadsheet's went from +58.71% to -53.94% and worldcup_far's from +18.83% to -2.22%. Neither item fires on any gating cell, so tiers S, M and L code the same bytes as before.

The binary changed after the cloud tables ran. Main is now d771c4a and its release build is 3ee7824e. The cloud numbers still hold for it, for two reasons. The cloud boxes ran 504fdda, the source of a9b880c8, and every gating cell still writes a9b880c8's streams byte for byte. On content they don't type, the two detectors add 0.02-0.04% to the instructions.

```
G1 pure C, 1 thread:   PASS in all 9 class-tiers on Mac, x86-64 and Graviton4 (CIF-S, 720p-S/L, 1080p-S/M/L, 2160p-S/M/L)
G2 pure C, all cores:  PASS in all 9 on Mac, x86-64 and Graviton4
G3 SIMD, all cores:    PASS in all 9 on Mac (NEON), x86-64 (AVX-512 over AVX2) and Graviton4 (SVE2 over NEON)
G3 at a capped tier:   PASS in CIF-S, 720p-S, 1080p-S and 2160p-S on x86-64 at SSE4.2 (worst 0.77) and at AVX2 (worst 0.92), both encoders capped alike
VBR wall (G3):         PASS, 26 of 26 cells on each machine, every one under 1.00
Quality:               PASS in every class of tiers S, M and L: Mac ladders 52 of 52 inside the BD bar, x86 by reproduction, Graviton4 by stream identity
Tier C quality:        PASS in 720p-C, 1080p-C and 2160p-C, 9 of 9 clips; 15 of 15 BD curves inside the bar, and the 3 screen VBR curves sit wholly above x265's
VBR tracking:          PASS, every measured tier M and L rung of ours inside +-10% on every machine
Full parity:           yes on NEON, AVX-512 and SVE2: 81 of 81 gating combinations (27 per machine), 78 of 78 VBR walls, tier C quality 9 of 9
Not measured:          SSE4.2 and AVX2 beyond tier S G3, a CPU that stops at AVX2 or SSE4.2 in silicon, Graviton3 beyond tier S G3, tier C off the Mac, tier C G1 and G2
```

The tightest timed cell anywhere is the Mac's meridian_1080p tier L VBR wall at 0.85. On x86 the tightest G3 cell is bus_cif at 0.82, or 0.84 against x265 at `--asm avx512`. Every median and every worst sits at least 0.15 inside its bar. I'd sign off full parity on NEON, AVX-512 and SVE2 from this table, tier C included. SSE4.2 and AVX2 I'd sign off on tier S. G1 and G2 are pure C and already timed, so G3 is the only goal a tier can move. Tiers M and L weren't run capped. On tier S the AVX2 cap costs at most 0.12 against the AVX-512 ratio, and x86's worst tier M or L G3 cell is 0.80. So I'd expect them to pass, but that's an estimate.

## G1 speed at one thread in pure C
| class | tier | Mac | x86-64 | Graviton4 | cells (Mac / x86 / G4) |
|---|---|---|---|---|---|
| CIF | S | 0.46 / 0.48 | 0.67 / 0.67 | 0.66 / 0.68 | 3 / 3 / 3 |
| 720p | S | 0.30 / 0.43 | 0.44 / 0.61 | 0.43 / 0.59 | 4 / 4 / 4 |
| 720p | L | 0.28 / 0.32 | 0.42 / 0.43 | 0.43 / 0.45 | 7 / 7 / 7 |
| 1080p | S | 0.32 / 0.37 | 0.45 / 0.53 | 0.45 / 0.55 | 3 / 3 / 3 |
| 1080p | M | 0.35 / 0.40 | 0.53 / 0.53 | 0.53 / 0.54 | 3 / 3 / 3 |
| 1080p | L | 0.32 / 0.36 | 0.46 / 0.49 | 0.47 / 0.51 | 7 / 7 / 7 |
| 2160p | S | 0.23 / 0.25 | 0.33 / 0.34 | 0.33 / 0.34 | 2 / 2 / 2 |
| 2160p | M | 0.30 / 0.36 | 0.42 / 0.47 | 0.44 / 0.49 | 3 / 3 / 3 |
| 2160p | L | 0.30 / 0.32 | 0.43 / 0.45 | 0.45 / 0.47 | 6 / 6 / 6 |
| 720p, 1080p, 2160p | C | - | - | - | outside the bar |

## G2 speed on all cores in pure C
| class | tier | Mac | x86-64 | Graviton4 | cells (Mac / x86 / G4) |
|---|---|---|---|---|---|
| CIF | S | 0.32 / 0.35 | 0.44 / 0.49 | 0.40 / 0.45 | 3 / 3 / 3 |
| 720p | S | 0.30 / 0.33 | 0.28 / 0.38 | 0.26 / 0.34 | 4 / 4 / 4 |
| 720p | L | 0.24 / 0.31 | 0.27 / 0.29 | 0.25 / 0.28 | 7 / 7 / 7 |
| 1080p | S | 0.30 / 0.34 | 0.38 / 0.43 | 0.42 / 0.46 | 3 / 3 / 3 |
| 1080p | M | 0.33 / 0.37 | 0.39 / 0.45 | 0.42 / 0.48 | 3 / 3 / 3 |
| 1080p | L | 0.30 / 0.31 | 0.36 / 0.39 | 0.38 / 0.41 | 6 / 7 / 7 |
| 2160p | S | 0.32 / 0.35 | 0.21 / 0.21 | 0.21 / 0.23 | 2 / 2 / 2 |
| 2160p | M | 0.32 / 0.39 | 0.27 / 0.39 | 0.29 / 0.45 | 3 / 3 / 3 |
| 2160p | L | 0.35 / 0.38 | 0.31 / 0.33 | 0.33 / 0.36 | 6 / 6 / 6 |
| 720p, 1080p, 2160p | C | - | - | - | outside the bar |

## G3 speed with SIMD on all cores
On x86 ours dispatches AVX-512 for the satd and transform families and AVX2 for every other kernel. Because x265's default assembly stops at AVX2 on this CPU, the extra column also times it at `--asm avx512`. Graviton3 ran tier S only.
| class | tier | Mac | x86-64 | x86-64 vs x265 avx512 | Graviton4 | Graviton3 | cells (Mac / x86 / G4) |
|---|---|---|---|---|---|---|---|
| CIF | S | 0.51 / 0.57 | 0.70 / 0.82 | 0.70 / 0.84 | 0.55 / 0.66 | 0.53 / 0.60 (2 of 3) | 3 / 3 / 3 |
| 720p | S | 0.62 / 0.65 | 0.60 / 0.75 | 0.62 / 0.77 | 0.48 / 0.57 | 0.45 / 0.54 | 4 / 4 / 4 |
| 720p | L | 0.47 / 0.56 | 0.53 / 0.56 | 0.54 / 0.57 | 0.42 / 0.46 | - | 6 / 7 / 7 |
| 1080p | S | 0.52 / 0.56 | 0.74 / 0.80 | 0.77 / 0.83 | 0.68 / 0.71 | 0.62 / 0.66 | 3 / 3 / 3 |
| 1080p | M | 0.59 / 0.66 | 0.74 / 0.80 | 0.75 / 0.82 | 0.64 / 0.72 | - | 3 / 3 / 3 |
| 1080p | L | 0.57 / 0.59 | 0.68 / 0.71 | 0.70 / 0.73 | 0.59 / 0.64 | - | 6 / 7 / 7 |
| 2160p | S | 0.62 / 0.63 | 0.41 / 0.43 | 0.41 / 0.44 | 0.31 / 0.33 | 0.31 / 0.34 | 2 / 2 / 2 |
| 2160p | M | 0.56 / 0.60 | 0.53 / 0.67 | 0.54 / 0.69 | 0.47 / 0.63 | - | 2 / 3 / 3 |
| 2160p | L | 0.58 / 0.66 | 0.59 / 0.62 | 0.60 / 0.63 | 0.49 / 0.54 | - | 6 / 6 / 6 |
| 720p | C | 0.47 / 0.47 | - | - | - | - | 1 / - / - (not gating) |
| 1080p | C | 0.53 / 0.54 | - | - | - | - | 7 / - / - (not gating; 4 timed on a9b880c8 before items 61 and 62) |
| 2160p | C | 0.28 / 0.28 | - | - | - | - | 1 / - / - (not gating) |

On 2026-11-15 the x86 box ran tier S G3 again with both encoders held to one tier: ours under `N265_CPU_MAX`, x265 under `--asm sse4.2` or `--asm avx2`. The AVX-512 column is the same run's control, ours uncapped against x265 at `--asm avx2`, which is x265's default on this CPU. PARITY-REPORT-yah265-x86-2026-11-13 has every cell and the method.
| class | tier | x86-64 SSE4.2 vs x265 sse4.2 | x86-64 AVX2 vs x265 avx2 | control: AVX-512 vs x265 avx2 | cells |
|---|---|---|---|---|---|
| CIF | S | 0.64 / 0.75 | 0.74 / 0.88 | 0.70 / 0.83 | 3 |
| 720p | S | 0.50 / 0.65 | 0.63 / 0.80 | 0.60 / 0.75 | 4 |
| 1080p | S | 0.69 / 0.77 | 0.85 / 0.92 | 0.74 / 0.80 | 3 |
| 2160p | S | 0.43 / 0.45 | 0.45 / 0.50 | 0.44 / 0.48 | 2 |

## VBR wall at G3 on one rung per real-title cell
The rung is x265's CRF 28 rate on the Mac, with maxrate 1.5x and a buffer of 2x. Two interleaved rounds, no size match.
| class | tier | Mac | x86-64 | Graviton4 | cells |
|---|---|---|---|---|---|
| 720p | L | 0.42 / 0.76 | 0.51 / 0.58 | 0.40 / 0.50 | 7 |
| 1080p | M | 0.63 / 0.68 | 0.79 / 0.81 | 0.69 / 0.72 | 3 |
| 1080p | L | 0.61 / 0.85 | 0.77 / 0.84 | 0.70 / 0.78 | 7 |
| 2160p | M | 0.64 / 0.66 | 0.60 / 0.75 | 0.53 / 0.71 | 3 |
| 2160p | L | 0.65 / 0.75 | 0.66 / 0.73 | 0.56 / 0.66 | 6 |

## Instruction-set coverage
| instruction set | where it ran | timed | how correctness is shown |
|---|---|---|---|
| NEON (arm64) | Mac M5 Max, Graviton4, Graviton3 | yes: G3 on all three | checkasm, escape cmp against C, conformance |
| SVE2, 128-bit (arm64) | Graviton4 | yes: every G3 cell in the bar | checkasm on the box, 0 failures; streams identical to the Mac's |
| SVE, 256-bit (arm64) | Graviton3 | tier S G3 only | 12 of 12 G3 streams identical to the Mac's |
| AVX-512 (x86-64) | Xeon 8488C | yes: every G3 cell in the bar | checkasm on the box at sse4.2, avx2 and avx512, 0 failures |
| AVX2 (x86-64) | Xeon 8488C, under AVX-512 and capped alone | yes: tier S G3 capped at AVX2 against x265 `--asm avx2`, worst 0.92 | checkasm; escape cmp at `N265_CPU_MAX=avx2` SAME on 42 cells at 8 bits and 42 at 10; the capped G3 streams equal the uncapped ones by md5 |
| SSE4.2 (x86-64) | Xeon 8488C capped, Rosetta, QEMU Nehalem | yes: tier S G3 capped at SSE4.2 against x265 `--asm sse4.2`, worst 0.77 | below |

SSE4.2 was timed only as a cap on the Xeon. Its correctness rests on four checks and the timed run:

- checkasm passes at the sse4.2 tier on the Xeon, under Rosetta with and without ASan and UBSan, and under QEMU's Nehalem model.
- The escape cmp at `N265_CPU_MAX=sse42` is SAME against C on all 28 Rosetta cells. On the Xeon it is SAME on 42 cells at 8 bits and 42 at 10.
- Fast conformance passes at the tier with no failure.
- A deliberate break in each family was caught.
- In the capped G3 run, all 12 tier S cells wrote the same md5 at SSE4.2, AVX2 and AVX-512.

Both tiers were timed on a capped Xeon, with no CPU that stops at AVX2 or SSE4.2 in silicon. A Haswell or Zen 2 box has other cores, caches and clocks, so its walls would differ. The ratio with both encoders on one instruction set is what the run settles. AVX2 is the tightest pairing because x265 gains more from AVX2 than we do. Its worst cell, pedestrian_1080p, reads 0.92. Before this run the nearest reading capped one thread at AVX2 on the same Xeon, where the AVX-512 build was 1.068x faster over 19 cells. The 2026-11-11 probe timed both encoders on AVX2 too, on an older binary.

## Quality on each machine
Quality is VMAF v1 scored at display size, on vmaf_v1.0.16 3d0h up to 1080p and 1d5h_2160 above. Matched size within 1% on every cell. BD is the Mac's five-rung ladder, CRF and VBR, against x265 on the same box. Tier S has no ladder under the spec.
| class | tier | dVMAF v1 median / worst | dPSNR-Y worst | worst BD-VMAF v1 curve | x86-64 | Graviton4 |
|---|---|---|---|---|---|---|
| CIF | S | +2.33 / +0.56 | -0.68 | n/a | inferred | identical streams (stefan_cif re-solved) |
| 720p | S | +2.39 / +0.86 | -0.29 | n/a | inferred | identical streams |
| 720p | L | +1.67 / +0.61 | -0.42 | -0.41% (nocturne VBR) | inferred | identical streams |
| 1080p | S | +0.61 / -0.05 | -0.59 | n/a | inferred | identical streams |
| 1080p | M | +1.96 / +0.63 | -0.40 | -4.83% (chimera VBR) | reproduced: sollevante CRF -0.03 | identical streams |
| 1080p | L | +1.08 / +0.70 | -0.81 | -6.88% (nocturne VBR) | reproduced: cosmos CRF -0.03, VBR +0.01 | identical streams |
| 2160p | S | +1.86 / +1.22 | -0.49 | n/a | inferred | identical streams |
| 2160p | M | +1.40 / +0.06 | -0.48 | +0.70% (chimera VBR) | reproduced: chimera VBR +0.07 | identical streams |
| 2160p | L | +1.18 / +0.44 | -0.54 | -4.59% (bbb VBR) | inferred | identical except nocturne |
| 720p | C | +2.55 / +2.55 | -0.07 | -26.04% (lash1924 VBR) | - | - |
| 1080p | C | +2.53 / +0.14 | -0.90 | -2.22% (worldcup_far CRF) | - | - |
| 2160p | C | -0.24 / -0.24 | -0.68 | -3.81% (boxing CRF) | - | - |

Tiers S, M and L have no soft cell between -0.5 and -1.0 VMAF v1. All 52 of their Mac curves are inside +1.0%. The one within 0.5 points of the bar is chimera_2160p VBR at +0.70%. Tier C is in its own section below.

x86 doesn't write the Mac's streams. Clang fuses float multiply-adds on arm64 but not on baseline x86-64. The owner chose to leave that as is. So four Mac curves were re-run on x86 at the Mac's rungs and scored with the Mac's libvmaf build. All four come within 0.2 BD points of the Mac's. The largest move is chimera_2160p VBR, from +0.70% to +0.78%. "Inferred" in the table means the row rests on those four curves and on the kernels being bit-exact.

Graviton4 writes the Mac's streams. In each of G1, G2 and G3, 36 of its 38 timed encodes match the Mac's by md5. Of the VBR walls 25 of 26 match. One miss is stefan_cif. It was re-solved at another crf on purpose, and at the Mac's crf it was identical too. The other is nocturne_2160p, whose source frames differ (see the caveats).

## Tier C on the Mac
Tier C ran on a9b880c8 (main 504fdda) with the spec's harness, scripts/parity_spec.py. Items 61 and 62 then re-measured the four clips they move. The three screen clips ran on 4d8c0ff1 and worldcup_far on 7097f4aa. All nine cells' rows are carried to 3ee7824e by identity. Each cell has a matched point at x265's CRF 28 bytes, scored on every frame. Each also has a five-rung CRF ladder (20 to 32) and a VBR ladder at x265's CRF rates, with maxrate 1.5x and a buffer of 2x, both encoders at 6 threads. VMAF v1 is scored at display size: lash at 720p (1280x960) is scaled up to fit 1080p. boxing_2160p is 60 fps and reads on the 1d5h_2160 _hfr model. skater227 and boxing are 10-bit sources converted to 8-bit 4:2:0. Lash is the 10-minute excerpt from 40 s. The other seven are the short clips, 130 frames (254 for boxing). Speed is one timed G3 row per cell, interleaved rounds on a quiet box, and doesn't gate. The four moved clips' G3 rows are still a9b880c8's.

| cell | content | dVMAF v1 / dPSNR-Y at matched size | BD-v1 CRF | BD-v1 VBR | G3 | verdict |
|---|---|---|---|---|---|---|
| lash1924_720p | film grain | +2.55 / -0.07 | -27.09% | -26.04% | 0.47 | PASS |
| lash1924_1080p | film grain | +2.92 / -0.08 | -31.89% | -31.37% | 0.54 | PASS |
| worldcup_1080p | sports | +0.52 / -0.52 | -6.15% | -7.80% | 0.52 | PASS |
| worldcup_far_1080p | sports | +0.14 / -0.41 | -2.22% | -3.91% | 0.53 (a9b880c8) | PASS |
| skater227_1080p | sports | +0.60 / -0.90 | -4.29% | -16.78% | 0.50 | PASS |
| boxing_2160p | sports | -0.24 / -0.68 | -3.81% | -14.91% | 0.28 | PASS |
| wikipedia_1080p | screen | +3.57 / +9.85 | -63.91% | no overlap, ours above | 0.53 (a9b880c8) | PASS |
| debugging_1080p | screen | +2.53 / +2.09 | -25.07% | no overlap, ours above | 0.51 (a9b880c8) | PASS |
| spreadsheet_1080p | screen | +3.15 / +6.83 | -53.94% | no overlap, ours above | 0.53 (a9b880c8) | PASS |

Three of these cells failed on a9b880c8. The table has the four moved clips before and after, in BD-v1 against x265. docs/phase14.md has both items in full.

| cell | CRF on a9b880c8 | CRF now | VBR on a9b880c8 | VBR now |
|---|--:|--:|--:|--:|
| wikipedia_1080p | +57.69% | -63.91% | +171.34% | no overlap, ours above |
| spreadsheet_1080p | +58.71% | -53.94% | +23.00% | no overlap, ours above |
| debugging_1080p | -4.27% | -25.07% | -16.28% | no overlap, ours above |
| worldcup_far_1080p | +18.83% | -2.22% | +15.02% | -3.91% |

- Item 61 types a frame as screen content from its luma and gives it three tools. A scroll vote corrects the lowres vectors. The B run ends where the picture changes. The lowres vector also seeds every search. The scrolls had beaten the search before, so wikipedia's P and B pictures coded close to intra.
- Item 62 watches the median lowres vector over 24 frames. When the camera jitters from frame to frame without panning, the B run stays long, the AMVP search runs in full and every RD trial runs the lattice. Before it, worldcup_far coded 26 P anchors in 130 frames.

The three screen VBR curves no longer share a VMAF v1 range with x265's, so no BD is defined. Our lowest rung beats x265's highest at fewer bits on all three. Wikipedia reads 98.62 at 1671 kbit/s, where x265's top rung reads 95.40 at 2525.

Both items were made for these clips, so their readings are on the content they were tuned on. Each was checked for reach on the gating set. No frame outside the three screen clips reaches item 61's 3% threshold. The highest elsewhere is a stem2 720p frame at 1.85%. Item 62's gate fires on no 1080p-class gating source. Both escapes give the old streams. The identity batteries in docs/phase14.md read SAME on every other cell.

The moved clips' speed changed with them, but their G3 rows weren't re-timed. Item 61 makes two of the screen clips cheaper. At one thread wikipedia takes 0.552x the instructions it took before and spreadsheet 0.610x. Debugging takes 1.049x because of the vote. Item 62 costs worldcup_far 12% more wall at one thread. I'd expect its G3 to rise from 0.53 to about 0.6. That's an estimate, because the clip wasn't timed. Tier C speed doesn't gate.

The VBR tracking bar applies to tiers M and L, so tier C's tracking is shown for information. Ours stays inside +-10% on six cells, worst +5.0% (boxing). It misses on all three screen clips, each time under its target: wikipedia -13.4% to -23.0% (5 of 5 rungs out), debugging +0.3% to -11.2% (1 of 5) and spreadsheet -22.7% to -26.8% (5 of 5). x265 misses on the same three by 32-44%. The still frames between changes cost next to nothing. Each clip runs 4.3 s against a 2 s buffer.

## VBR rate tracking
The bar puts every rung within +-10% of its target.
| source | rungs | ours, worst | x265, worst | inside +-10% |
|---|---|---|---|---|
| Mac ladders, 26 cells x 5 rungs | 130 | -7.7% (sintel_720p) | -14.6% (chimera_1080p) | ours 130 of 130 |
| x86 ladders, chimera_2160p and cosmos_1080p | 10 | -5.2% (chimera_2160p) | -14.4% (chimera_2160p) | ours 10 of 10 |
| x86 VBR walls | 26 | -7.3% (sintel_720p) | -14.5% (chimera, both sizes) | ours 26 of 26 |
| Graviton4 VBR walls | 26 | -7.3% (sintel_720p) | -14.5% (chimera, both sizes) | ours 26 of 26 |
| Mac tier C ladders, 9 cells x 5 rungs (information) | 45 | -26.8% (spreadsheet_1080p) | -44.3% (spreadsheet_1080p) | ours 34 of 45 |

The bar is absolute and applies to ours. x265 misses it on chimera at both sizes and on several stem2 rungs.

## Caveats
**Mac**
- Four Mac cells are void because the feeder couldn't keep 1.5x ahead: meridian_2160p tier M G3, meridian_720p tier L G3, and meridian_1080p tier L G2 and G3. The medians leave them out. That's why 2160p-M G3 counts 2 of 3 cells on the Mac. 720p-L G3, 1080p-L G2 and 1080p-L G3 count 6 of 7. All four pass on the cloud boxes, whose feeders kept up.
- Many Mac rows come from older binaries. Tier L's G1 and G2 windows, eleven tier L G3 cells and fifteen VBR walls ran on 12de49f2 (a74e541). That binary predates the deblock and SAO SIMD. Since that SIMD made the encoder about 0.3% faster, those ratios are slightly conservative. The ladders came from 12de49f2 and a759f17f. Each carried row names its source binary in docs/data. Identity cmps showed the streams byte-identical before any row was carried.
- Those eleven tier L G3 cells keep the crf solved on an older binary still, 4d51b17 (061abf3). Their timed encodes held the 1% size match.
- On 2026-11-12 the Mac's rows were carried again, from ab8a99eb to a9b880c8 (main 504fdda). identity_ab read 28 of 28 SAME. Six whole cells matched as well, across CRF, capped CRF and VBR.
- On 2026-11-14 they were carried once more, from a9b880c8 to 3ee7824e (main d771c4a), leaving out the four clips items 61 and 62 moved. Those four came over from the items' own binaries. Before the carry, a bitstream cmp of the two release builds read SAME on three tier S clips at one thread, on chimera_1080p's tier M G1 window at its crf, and on a capped-VBR skater227 rung. One rung of each moved clip, re-encoded on that build, matched its recorded md5. The rows went first to 7eea345a, a worktree build of the same source without NDEBUG, then to 3ee7824e, main's release build with NDEBUG as a9b880c8 had it. The two write the same streams (foreman_cif, wikipedia and worldcup_far checked).
- The Mac read tier M G1 and G2 on the whole segment. That predates the rule for 30 s windows.
- Tier L G1 and G2 come from one run each on every machine. The Mac's only single-run G3 rows are two of the void meridian cells.
- The credits rows for stem2 and bbb are still pending. On tears' credits we're behind, at +5.9% size at equal PSNR-Y (extrapolated). Credits are supplementary and don't gate.
- End credits can type as screen content under item 61. A first form of its detector read bbb's credit roll at up to 22% of tested blocks, against a 3% threshold. The credits rows above predate item 61, so the next credits run may move.

**Cloud method**
- The owner's rule reports cloud wall time apart from the Mac table. The columns above are never pooled.
- G1's one-thread arms ran eight at a time, one per core. Both encoders therefore ran at all-core turbo. The ratio holds because both arms shared the load. Lone one-thread walls would be faster.
- Tier M G1 and G2 on the cloud were read on the Mac's matched 30 s windows, as the spec now asks. The Mac's own tier M rows read the whole segment.
- The cloud quality columns are the Mac's. Speed, size, md5 and VBR rate were measured on the boxes.
- The cloud boxes built 504fdda, the source of a9b880c8. Main has since taken items 61 and 62. Their detectors leave every gating cell's bytes alone. Where they don't fire they cost 0.02-0.04% more instructions. So the cloud tables stand for d771c4a. No cloud run was repeated.
- foreman_cif and stefan_cif were re-solved on x86. The x265 built there writes 1-1.5% more or fewer bytes than brew's x265 on the Mac. At the Mac's crf ours then missed the 1% bar. stefan_cif was re-solved on Graviton4 for the same reason. Those rows replace the Mac-point rows.
- nocturne_2160p's cloud frames are a software decode of the ProRes master. The Mac's feeder decodes it with VideoToolbox. The two decodes differ by 68 dB PSNR-Y on frame 3000. The cloud frames match the Mac's own software decode by plane md5. To get that match on x86 the feeder ran ffmpeg's C paths. Our nocturne_2160p streams therefore differ from the Mac's on both clouds. The Mac's crf still held the 1% size bar on those frames.
- Nocturne's 720p and 1080p G3 and VBR cells were re-run from a raw copy staged on the box. Decoding the lossless pack on the fly couldn't stay 1.5x ahead of the encoders.
- Spot reclaims made the next box rerun the whole goal. No row mixes boxes.
- The x86 ladders ran on Sapphire Rapids and then, after three reclaims, on Ice Lake. No AMD x86 CPU was timed.
- Graviton3 is a second line. The feeder and the encoders shared its box. Its stefan_cif read -1.13% size, outside the bar, and wasn't re-solved. So its CIF row counts 2 of 3 cells.

**Correctness**
- TSan on x86 encodes at 4 and 16 threads reports one race. main reports the same one. A row's QP map write meets the whole-frame copy a row takes when it starts. It predates the UHD scaling item, as docs/threading-design.md records. TSan on conformance-fast is clean. Identity across 1, 6, 12, 16 and auto threads reads 30 of 30 SAME.

**Spend**
- The two cloud-table rounds cost about $54.23 in estimates. The whole EC2 ledger sums to about $57.50 of the owner's $100 cap: instance estimates plus the hubs' volumes. That figure is my sum of its rows, because the ledger has no grand-total line. The billing page hasn't been checked against it.
- The capped SSE4.2 and AVX2 run on 2026-11-15 took one m7i.8xlarge spot box for 0.67 h, about $0.37. That brings the ledger to about $57.87.

## Last 24 hours on main
- The scalar speed work merged. It covers the lowres plane, the cu-tree and AQ sums, the weighted-prediction sweeps, the 4x4 map writes and the prediction cache. Picture planes now use huge pages under Linux. No bit moved.
- New AVX-512 tier for the satd and transform families. New SVE tier for the pixel metrics and the vertical luma pass. The NEON SSD gained a udot arm.
- The UHD scaling item splits the driver's per-frame analysis over the pool and grows the UHD slot pool. It's a schedule change and moves no bit.
- The 2026-11-12 Mac report declared full parity on the Mac. The site and README took its numbers, scoped to the Mac. Its rows were then carried to a9b880c8 by identity.
- The x86-64 and Graviton tables ran on EC2 spot in 3c875c8. Nocturne's tier L cells followed in 559bb1e. Both machines pass 27 of 27.
- Tier C ran on the Mac for the first time. Six of nine cells passed on a9b880c8. worldcup_far, wikipedia and spreadsheet at 1080p failed the floor and the BD bar. `--ladder` now takes tier C.
- Item 61 (9923fe0) gives screen content a scroll vote, a change-driven B run and lowres search seeds. Wikipedia's CRF curve reads -63.91% BD-v1, spreadsheet's -53.94% and debugging's -25.07%. Their VBR curves sit wholly above x265's at fewer bits.
- Item 62 (d771c4a) keeps the long B run, the full AMVP search and the lattice through a handheld camera's jitter. worldcup_far reads -2.22% under CRF and -3.91% under VBR, and +0.14 VMAF v1 at matched size. It costs that clip 12% more wall at one thread.
- Tier C's quality now passes on all nine clips. The Mac's rows were carried to 3ee7824e by identity.
- The Graviton report said ours dispatches SVE2 on Graviton3. The V1 has no SVE2, so it runs SVE at 256 bits. Fixed.

## Not measured
- Tier C off the Mac. x86 and Graviton would read the same quality by the same argument as the gating set (reproduction and stream identity), but no tier C cell ran there.
- Tier C G1 and G2 on any machine, and G3 for the four clips items 61 and 62 moved on the binary that moved them. Tier C speed doesn't gate.
- SSE4.2 and AVX2 on tiers M and L, and on a CPU that stops at either tier in silicon. Tier S G3 at both caps is in the G3 section.
- Graviton3 G1, G2, tiers M and L, and VBR.
- Full BD ladders on x86 beyond the four reproduced curves, and on Graviton (identity stands in).
