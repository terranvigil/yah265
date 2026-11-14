# yah265 parity report, every architecture, 2026-11-14

- Encoder: yah265 n265 at main 504fdda, preset medium.
- Mac binary: build-rel ab8a99eb (main 360ab7d), carried by identity to a9b880c8 (main 504fdda).
- Cloud binary: 504fdda built on each box from `git archive` with clang 18.
- Reference: x265 4.3+1-e9b8812, preset medium. Homebrew's build on the Mac, the 4.3 release tarball built with clang 18 on each cloud box.
- Mac: Apple M5 Max, 18 cores, macOS 26.6.2, arm64 NEON.
- x86-64: Intel Xeon Platinum 8488C (Sapphire Rapids) on EC2 m7i and c7i spot, AVX-512 over AVX2.
- Graviton4: Neoverse V2 on EC2 c8g and r8g spot, NEON and SVE2 at 128 bits.
- Graviton3: Neoverse V1 on c7g.4xlarge, SVE at 256 bits. A second line, tier S G3 only.
- Measured: Mac timed legs 2026-09-26 20:02 to 2026-11-12 14:07. Tier C on the Mac 2026-11-13 23:24 to 2026-11-14 03:17 EST, on a9b880c8. Cloud 2026-11-12 23:10 to 2026-11-14 04:00 UTC.
- Sources: the three reports PARITY-REPORT-yah265-2026-11-12, PARITY-REPORT-yah265-x86-2026-11-13 and PARITY-REPORT-yah265-graviton-2026-11-13 with their CSVs. The cloud records are in docs/data/cloud-tables-2026-11-13/.

The one new measurement on this page is tier C, on the Mac. Its points, ladders and timed rows are in docs/data/parity-spec-points.json, parity-spec-ladders.json and parity-spec.csv.

How to read: a speed ratio is our time over the reference's time at the same file size. Below 1.00 we're faster. Pass means median <= 1.00 and worst <= 1.15, with quality no worse than -0.5 VMAF v1. "-" means not measured. Each cell shows median / worst over the class and tier's valid cells. Lookahead is each medium's own on every machine. Ours is 20 at 720p and up and 12 at CIF, and x265's is 20. The Mac runs at auto threads on all 18 cores. On each cloud box both encoders are held to 16 vCPUs by `taskset`. The feeder runs on the other cores.

## Verdict
Is yah265 at full parity on every architecture? Not yet. Every gating combination of tiers S, M and L passes on all three machines that were timed. Tier C's quality also counts, and it fails on three 1080p cells. Two more gaps keep it short of a clean yes on every instruction set. SSE4.2 was never timed on real hardware. An AVX2-only x86 CPU wasn't timed as its own column either. Because both tiers are bit-exact with C, they write the streams the x86 table measured. Their quality holds for that reason, but their speed is unmeasured.

Tier C is now measured on the Mac. The spec counts its matched-size quality on every clip. Six of its nine cells pass. Three 1080p cells fail the per-cell floor and the BD bar: worldcup_far, and the two screen clips wikipedia and spreadsheet. Lash at 720p and 1080p, the sports clips worldcup and skater227, boxing at 2160p and the debugging screen clip all pass. So full parity holds on the gating set (tiers S, M and L) and not on tier C.

```
G1 pure C, 1 thread:   PASS in all 9 class-tiers on Mac, x86-64 and Graviton4 (CIF-S, 720p-S/L, 1080p-S/M/L, 2160p-S/M/L)
G2 pure C, all cores:  PASS in all 9 on Mac, x86-64 and Graviton4
G3 SIMD, all cores:    PASS in all 9 on Mac (NEON), x86-64 (AVX-512 over AVX2) and Graviton4 (SVE2 over NEON)
VBR wall (G3):         PASS, 26 of 26 cells on each machine, every one under 1.00
Quality:               PASS in every class of tiers S, M and L: Mac ladders 52 of 52 inside the BD bar, x86 by reproduction, Graviton4 by stream identity
Tier C quality:        PASS in 720p-C and 2160p-C; OPEN in 1080p-C (worldcup_far, wikipedia, spreadsheet); 12 of 18 tier C curves inside the BD bar
VBR tracking:          PASS, every measured tier M and L rung of ours inside +-10% on every machine
Full parity:           no: tier C quality open in 1080p-C. The gating set passes, 81 of 81 combinations (27 per machine) and 78 of 78 VBR walls
Not measured:          SSE4.2 speed, AVX2-only speed, Graviton3 beyond tier S G3, tier C off the Mac, tier C G1 and G2
```

The tightest timed cell anywhere is the Mac's meridian_1080p tier L VBR wall at 0.85. On x86 the tightest G3 cell is bus_cif at 0.82, or 0.84 against x265 at `--asm avx512`. Every median and every worst sits at least 0.15 inside its bar. I'd sign off parity on the gating set on NEON, AVX-512 and SVE2 from this table. Tier C's three 1080p cells stand between that and full parity. SSE4.2 and AVX2-only I'd call correct and untimed, with no speed figure claimed for either.

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
| 1080p | C | 0.53 / 0.54 | - | - | - | - | 7 / - / - (not gating) |
| 2160p | C | 0.28 / 0.28 | - | - | - | - | 1 / - / - (not gating) |

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
| AVX2 (x86-64) | Xeon 8488C, under AVX-512 | as the base of every x86 G3 cell, not alone | checkasm; escape cmp at `N265_CPU_MAX=avx2` SAME on 42 cells at 8 bits and 42 at 10 |
| SSE4.2 (x86-64) | Xeon checkasm, Rosetta, QEMU Nehalem | no | below |

SSE4.2 isn't timed anywhere on real hardware. I mark its speed unmeasured. Its correctness rests on four checks:

- checkasm passes at the sse4.2 tier on the Xeon, under Rosetta with and without ASan and UBSan, and under QEMU's Nehalem model.
- The escape cmp at `N265_CPU_MAX=sse42` is SAME against C on all 28 Rosetta cells. On the Xeon it is SAME on 42 cells at 8 bits and 42 at 10.
- Fast conformance passes at the tier with no failure.
- A deliberate break in each family was caught.

Its only speed figures are Rosetta's instruction counts, which say nothing about x86 wall time. An AVX2-only CPU is in a similar spot. The nearest reading caps one thread at AVX2 on the same Xeon. Against that cap the AVX-512 build is 1.068x faster over 19 cells. The 2026-11-11 probe timed both encoders on AVX2, but on an older binary. I'd expect an AVX2-only part to pass G3 with room, since x86's worst G3 cell is 0.82. That's an estimate.

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
| 1080p | C | +0.52 / -6.02 | -5.28 | +171.34% (wikipedia VBR) | - | - |
| 2160p | C | -0.24 / -0.24 | -0.68 | -3.81% (boxing CRF) | - | - |

Tiers S, M and L have no soft cell between -0.5 and -1.0 VMAF v1. All 52 of their Mac curves are inside +1.0%. The one within 0.5 points of the bar is chimera_2160p VBR at +0.70%. Tier C is in its own section below.

x86 doesn't write the Mac's streams. Clang fuses float multiply-adds on arm64 but not on baseline x86-64. The owner chose to leave that as is. So four Mac curves were re-run on x86 at the Mac's rungs and scored with the Mac's libvmaf build. All four come within 0.2 BD points of the Mac's. The largest move is chimera_2160p VBR, from +0.70% to +0.78%. "Inferred" in the table means the row rests on those four curves and on the kernels being bit-exact.

Graviton4 writes the Mac's streams. In each of G1, G2 and G3, 36 of its 38 timed encodes match the Mac's by md5. Of the VBR walls 25 of 26 match. One miss is stefan_cif. It was re-solved at another crf on purpose, and at the Mac's crf it was identical too. The other is nocturne_2160p, whose source frames differ (see the caveats).

## Tier C on the Mac
Tier C ran on a9b880c8 (main 504fdda) with the spec's harness, scripts/parity_spec.py. Each cell has a matched point at x265's CRF 28 bytes, scored on every frame. Each also has a five-rung CRF ladder (20 to 32) and a VBR ladder at x265's CRF rates, with maxrate 1.5x and a buffer of 2x, both encoders at 6 threads. VMAF v1 is scored at display size: lash at 720p (1280x960) is scaled up to fit 1080p. boxing_2160p is 60 fps and reads on the 1d5h_2160 _hfr model. skater227 and boxing are 10-bit sources converted to 8-bit 4:2:0. Lash is the 10-minute excerpt from 40 s. The other seven are the short clips, 130 frames (254 for boxing). Speed is one timed G3 row per cell, interleaved rounds on a quiet box, and doesn't gate.

| cell | content | dVMAF v1 / dPSNR-Y at matched size | BD-v1 CRF | BD-v1 VBR | G3 | verdict |
|---|---|---|---|---|---|---|
| lash1924_720p | film grain | +2.55 / -0.07 | -27.09% | -26.04% | 0.47 | PASS |
| lash1924_1080p | film grain | +2.92 / -0.08 | -31.89% | -31.37% | 0.54 | PASS |
| worldcup_1080p | sports | +0.52 / -0.52 | -6.15% | -7.80% | 0.52 | PASS |
| worldcup_far_1080p | sports | -1.46 / -1.05 | +18.83% | +15.02% | 0.53 | FAIL |
| skater227_1080p | sports | +0.60 / -0.90 | -4.29% | -16.78% | 0.50 | PASS |
| boxing_2160p | sports | -0.24 / -0.68 | -3.81% | -14.91% | 0.28 | PASS |
| wikipedia_1080p | screen | -6.02 / -5.28 | +57.69% | +171.34% | 0.53 | FAIL |
| debugging_1080p | screen | +0.53 / -0.13 | -4.27% | -16.28% | 0.51 | PASS |
| spreadsheet_1080p | screen | -4.35 / -4.23 | +58.71% | +23.00% | 0.53 | FAIL |

The three failures, read from their CRF curves:
- wikipedia: ours lands within 0.15 VMAF v1 of x265 at every rung but spends 1.53-1.64x its bits (crf 20: 6694 against 4215 kb/s at 98.88 and 99.03).
- spreadsheet: ours spends 1.26-1.42x the bits and still reads 0.4-2.2 lower (crf 26: 5284 against 3792 kb/s at 94.90 and 95.96).
- worldcup_far: ours spends 0.84-0.87x the bits and reads 0.5-4.9 lower (crf 29: 2732 against 3211 kb/s at 91.17 and 94.41). The gap opens toward the low rungs.

At the matched point x265 moved to crf 28.03 on wikipedia and 28.22 on spreadsheet. Our size steps there were too coarse to land within 1% of CRF 28's bytes. The wikipedia VBR figure rests on a thin overlap: x265 undershoots its targets by 32-40%, so the two curves share only 2195-2525 kb/s. Its CRF figure is the cleaner reading. Nothing was tuned for these cells.

The VBR tracking bar applies to tiers M and L, so tier C's tracking is shown for information. Ours stays inside +-10% on six cells, worst +5.0% (boxing). It misses on all three screen clips: wikipedia +8.4% to +13.7% (4 of 5 rungs out), debugging -4.9% to -12.8% (2 of 5) and spreadsheet -20.5% to -24.9% (5 of 5). x265 misses on the same three by 32-44%. Each clip runs 4.3 s against a 2 s buffer.

## VBR rate tracking
The bar puts every rung within +-10% of its target.
| source | rungs | ours, worst | x265, worst | inside +-10% |
|---|---|---|---|---|
| Mac ladders, 26 cells x 5 rungs | 130 | -7.7% (sintel_720p) | -14.6% (chimera_1080p) | ours 130 of 130 |
| x86 ladders, chimera_2160p and cosmos_1080p | 10 | -5.2% (chimera_2160p) | -14.4% (chimera_2160p) | ours 10 of 10 |
| x86 VBR walls | 26 | -7.3% (sintel_720p) | -14.5% (chimera, both sizes) | ours 26 of 26 |
| Graviton4 VBR walls | 26 | -7.3% (sintel_720p) | -14.5% (chimera, both sizes) | ours 26 of 26 |
| Mac tier C ladders, 9 cells x 5 rungs (information) | 45 | -24.9% (spreadsheet_1080p) | -44.3% (spreadsheet_1080p) | ours 34 of 45 |

The bar is absolute and applies to ours. x265 misses it on chimera at both sizes and on several stem2 rungs.

## Caveats
**Mac**
- Four Mac cells are void because the feeder couldn't keep 1.5x ahead: meridian_2160p tier M G3, meridian_720p tier L G3, and meridian_1080p tier L G2 and G3. The medians leave them out. That's why 2160p-M G3 counts 2 of 3 cells on the Mac. 720p-L G3, 1080p-L G2 and 1080p-L G3 count 6 of 7. All four pass on the cloud boxes, whose feeders kept up.
- Many Mac rows come from older binaries. Tier L's G1 and G2 windows, eleven tier L G3 cells and fifteen VBR walls ran on 12de49f2 (a74e541). That binary predates the deblock and SAO SIMD. Since that SIMD made the encoder about 0.3% faster, those ratios are slightly conservative. The ladders came from 12de49f2 and a759f17f. Each carried row names its source binary in docs/data. Identity cmps showed the streams byte-identical before any row was carried.
- Those eleven tier L G3 cells keep the crf solved on an older binary still, 4d51b17 (061abf3). Their timed encodes held the 1% size match.
- On 2026-11-12 the Mac's rows were carried again, from ab8a99eb to a9b880c8 (main 504fdda). identity_ab read 28 of 28 SAME. Six whole cells matched as well, across CRF, capped CRF and VBR.
- The Mac read tier M G1 and G2 on the whole segment. That predates the rule for 30 s windows.
- Tier L G1 and G2 come from one run each on every machine. The Mac's only single-run G3 rows are two of the void meridian cells.
- The credits rows for stem2 and bbb are still pending. On tears' credits we're behind, at +5.9% size at equal PSNR-Y (extrapolated). Credits are supplementary and don't gate.

**Cloud method**
- The owner's rule reports cloud wall time apart from the Mac table. The columns above are never pooled.
- G1's one-thread arms ran eight at a time, one per core. Both encoders therefore ran at all-core turbo. The ratio holds because both arms shared the load. Lone one-thread walls would be faster.
- Tier M G1 and G2 on the cloud were read on the Mac's matched 30 s windows, as the spec now asks. The Mac's own tier M rows read the whole segment.
- The cloud quality columns are the Mac's. Speed, size, md5 and VBR rate were measured on the boxes.
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

## Last 24 hours on main
- The scalar speed work merged. It covers the lowres plane, the cu-tree and AQ sums, the weighted-prediction sweeps, the 4x4 map writes and the prediction cache. Picture planes now use huge pages under Linux. No bit moved.
- New AVX-512 tier for the satd and transform families. New SVE tier for the pixel metrics and the vertical luma pass. The NEON SSD gained a udot arm.
- The UHD scaling item splits the driver's per-frame analysis over the pool and grows the UHD slot pool. It's a schedule change and moves no bit.
- The 2026-11-12 Mac report declared full parity on the Mac. The site and README took its numbers, scoped to the Mac. Its rows were then carried to a9b880c8 by identity.
- The x86-64 and Graviton tables ran on EC2 spot in 3c875c8. Nocturne's tier L cells followed in 559bb1e. Both machines pass 27 of 27.
- Tier C ran on the Mac for the first time. Six of nine cells pass. worldcup_far, wikipedia and spreadsheet at 1080p fail the floor and the BD bar. `--ladder` now takes tier C.
- The Graviton report said ours dispatches SVE2 on Graviton3. The V1 has no SVE2, so it runs SVE at 256 bits. Fixed.

## Not measured
- Tier C off the Mac. x86 and Graviton would read the same quality by the same argument as the gating set (reproduction and stream identity), but no tier C cell ran there.
- Tier C G1 and G2 on any machine. Its speed doesn't gate.
- SSE4.2 and AVX2-only wall time.
- Graviton3 G1, G2, tiers M and L, and VBR.
- Full BD ladders on x86 beyond the four reproduced curves, and on Graviton (identity stands in).
