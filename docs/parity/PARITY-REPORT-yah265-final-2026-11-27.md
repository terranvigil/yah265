# yah265 parity report, final (2026-11-27)

> Every number here is copied from main cd485883: PARITY-REPORT-yah265-2026-11-24
> (the Mac), PARITY-REPORT-yah265-all-isa-2026-11-14 (x86-64 and Graviton,
> with the Mac column updated 11-24), the tier C sections of 2026-11-27 in the
> x86 and Graviton reports, the credits and fades reports of 11-23 and 11-24,
> and docs/data.

- Encoder: yah265 n265, preset medium. Gating rows rest on release build 3ee7824e (main d771c4a) and the binaries carried to it by identity. The four meridian cells timed on 2026-11-24 ran on main 61f4eab9.
- Reference: x265 4.3+1-e9b8812, preset medium. Homebrew's build on the Mac; the 4.3 release tarball built with clang 18 on each cloud box.
- Mac: Apple M5 Max, 18 cores, macOS 26.6.2, arm64 NEON. Timed legs 2026-09-26 20:02 to 2026-11-24 22:35.
- x86-64: Intel Xeon Platinum 8488C (Sapphire Rapids), EC2 spot, AVX-512 over AVX2. 2026-11-12 to 2026-11-15.
- AMD EPYC 9R14: Zen 4, EC2 spot, AVX-512, tier C G3 only, 2026-11-27.
- Graviton4: Neoverse V2, EC2 spot, SVE2 at 128 bits over NEON. Graviton3: Neoverse V1, SVE at 256 bits, tier S G3 only.
- Lookahead: ours 20 at 720p and up and 12 at CIF; x265 20 (each medium's own) on every machine, unless a row says otherwise.
- Durations: tier S short clips, tier M 2-minute real-title segments, tier L 10-minute excerpts (end credits excluded, black openings skipped), tier C content types (outside the speed gate).

How to read: a speed ratio is our time / the reference's time at the same file
size; below 1.00 we're faster. Pass = median <= 1.00 and worst <= 1.15, quality
no worse than -0.5 VMAF v1. Each cell is median / worst over the class and
tier. "-" = not measured. Quality is VMAF v1 at display size (vmaf_v1.0.16
3d0h up to 1080p, scaled up below 1920x1080; 1d5h_2160 above 1080p; _hfr at
50 fps and up). Machines are never pooled.

## Verdict
```
G1 pure C, 1 thread:   PASS in all 9 class-tiers (CIF-S, 720p-S/L, 1080p-S/M/L, 2160p-S/M/L) on Mac, x86-64 and Graviton4
G2 pure C, all cores:  PASS in all 9 on Mac, x86-64 and Graviton4
G3 SIMD, all cores:    PASS in all 9 on Mac (NEON), x86-64 (AVX-512 over AVX2) and Graviton4 (SVE2 over NEON)
G3 at a capped tier:   PASS in CIF-S, 720p-S, 1080p-S, 2160p-S on x86-64 at SSE4.2 (worst 0.77) and AVX2 (worst 0.92)
Quality:               PASS in all 12 class-tiers on the Mac, tier C included; 52 of 52 tier M/L BD curves inside +1.0%
VBR wall (G3):         PASS, 26 of 26 cells on each of the three machines
VBR tracking:          PASS, every tier M and L rung of ours inside +-10% on every machine
Full parity:           yes on NEON, AVX-512 and SVE2: 81 of 81 gating combinations (27 per machine), 78 of 78 VBR walls
Tier C speed:          not gating. G3 inside the bar on the Mac, Xeon 8488C, EPYC 9R14 and Graviton4; G1 and G2 not measured
```

Every gating median and worst sits at least 0.15 inside its bar. The tightest
timed cell anywhere is the Mac's meridian_1080p tier L VBR wall at 0.85. On
x86 the tightest G3 cell is bus_cif at 0.82 (0.84 against x265 at
`--asm avx512`). The tightest quality curve is chimera_2160p VBR at +0.70%
BD-v1 (x86 reproduction +0.78%).

## Speed (median / worst ratio, per class and tier)

### G1, pure C, one thread
| class | tier | Mac | x86-64 | Graviton4 | verdict |
|---|---|---|---|---|---|
| CIF | S | 0.46 / 0.48 | 0.67 / 0.67 | 0.66 / 0.68 | PASS |
| 720p | S | 0.30 / 0.43 | 0.44 / 0.61 | 0.43 / 0.59 | PASS |
| 720p | L | 0.28 / 0.32 | 0.42 / 0.43 | 0.43 / 0.45 | PASS |
| 720p | C | - | - | - | not gating |
| 1080p | S | 0.32 / 0.37 | 0.45 / 0.53 | 0.45 / 0.55 | PASS |
| 1080p | M | 0.35 / 0.40 | 0.53 / 0.53 | 0.53 / 0.54 | PASS |
| 1080p | L | 0.32 / 0.36 | 0.46 / 0.49 | 0.47 / 0.51 | PASS |
| 1080p | C | - | - | - | not gating |
| 2160p | S | 0.23 / 0.25 | 0.33 / 0.34 | 0.33 / 0.34 | PASS |
| 2160p | M | 0.30 / 0.36 | 0.42 / 0.47 | 0.44 / 0.49 | PASS |
| 2160p | L | 0.30 / 0.32 | 0.43 / 0.45 | 0.45 / 0.47 | PASS |
| 2160p | C | - | - | - | not gating |

### G2, pure C, all cores
| class | tier | Mac | x86-64 | Graviton4 | verdict |
|---|---|---|---|---|---|
| CIF | S | 0.32 / 0.35 | 0.44 / 0.49 | 0.40 / 0.45 | PASS |
| 720p | S | 0.30 / 0.33 | 0.28 / 0.38 | 0.26 / 0.34 | PASS |
| 720p | L | 0.24 / 0.31 | 0.27 / 0.29 | 0.25 / 0.28 | PASS |
| 720p | C | - | - | - | not gating |
| 1080p | S | 0.30 / 0.34 | 0.38 / 0.43 | 0.42 / 0.46 | PASS |
| 1080p | M | 0.33 / 0.37 | 0.39 / 0.45 | 0.42 / 0.48 | PASS |
| 1080p | L | 0.30 / 0.31 | 0.36 / 0.39 | 0.38 / 0.41 | PASS |
| 1080p | C | - | - | - | not gating |
| 2160p | S | 0.32 / 0.35 | 0.21 / 0.21 | 0.21 / 0.23 | PASS |
| 2160p | M | 0.32 / 0.39 | 0.27 / 0.39 | 0.29 / 0.45 | PASS |
| 2160p | L | 0.35 / 0.38 | 0.31 / 0.33 | 0.33 / 0.36 | PASS |
| 2160p | C | - | - | - | not gating |

### G3, SIMD, all cores
| class | tier | Mac | x86-64 | x86-64 vs x265 avx512 | Graviton4 | Graviton3 | verdict |
|---|---|---|---|---|---|---|---|
| CIF | S | 0.51 / 0.57 | 0.70 / 0.82 | 0.70 / 0.84 | 0.55 / 0.66 | 0.53 / 0.60 (2 of 3) | PASS |
| 720p | S | 0.62 / 0.65 | 0.60 / 0.75 | 0.62 / 0.77 | 0.48 / 0.57 | 0.45 / 0.54 | PASS |
| 720p | L | 0.47 / 0.56 | 0.53 / 0.56 | 0.54 / 0.57 | 0.42 / 0.46 | - | PASS |
| 720p | C | 0.47 / 0.47 | 0.69 / 0.69 | 0.71 / 0.71 | 0.61 / 0.61 | - | not gating |
| 1080p | S | 0.52 / 0.56 | 0.74 / 0.80 | 0.77 / 0.83 | 0.68 / 0.71 | 0.62 / 0.66 | PASS |
| 1080p | M | 0.59 / 0.66 | 0.74 / 0.80 | 0.75 / 0.82 | 0.64 / 0.72 | - | PASS |
| 1080p | L | 0.56 / 0.59 | 0.68 / 0.71 | 0.70 / 0.73 | 0.59 / 0.64 | - | PASS |
| 1080p | C | 0.53 / 0.54 | 0.88 / 1.03 (6 of 7) | 0.89 / 1.06 (6 of 7) | 0.73 / 0.99 | - | not gating |
| 2160p | S | 0.62 / 0.63 | 0.41 / 0.43 | 0.41 / 0.44 | 0.31 / 0.33 | 0.31 / 0.34 | PASS |
| 2160p | M | 0.53 / 0.60 | 0.53 / 0.67 | 0.54 / 0.69 | 0.47 / 0.63 | - | PASS |
| 2160p | L | 0.58 / 0.66 | 0.59 / 0.62 | 0.60 / 0.63 | 0.49 / 0.54 | - | PASS |
| 2160p | C | 0.28 / 0.28 | 0.40 / 0.40 | 0.42 / 0.42 | 0.38 / 0.38 | - | not gating |

Tier C off the Mac was timed on 2026-11-27 on 4d0d13a4, so it includes items
61 and 62. The Mac's 1080p-C G3 still includes four clips timed on a9b880c8,
before those items moved them (wikipedia, spreadsheet, debugging and
worldcup_far). AWS reclaimed both x86 boxes during lash1924_1080p, so x86
1080p-C is over 6 of its 7 clips. An AMD EPYC 9R14 (Zen 4) ran tier C beside
the Xeon: 720p 0.63, 1080p 0.77 / 0.99 (6 of 7), 2160p 0.32. The slowest tier C
cell on every machine is wikipedia_1080p (Xeon 1.03, EPYC 0.99, Graviton4
0.99), inside the 1.15 worst bar.

### G3 at a capped instruction set (x86-64, tier S only)
Both encoders held to one tier: ours under `N265_CPU_MAX`, x265 under `--asm`.
| class | tier | SSE4.2 vs x265 sse4.2 | AVX2 vs x265 avx2 | control: AVX-512 vs x265 avx2 |
|---|---|---|---|---|
| CIF | S | 0.64 / 0.75 | 0.74 / 0.88 | 0.70 / 0.83 |
| 720p | S | 0.50 / 0.65 | 0.63 / 0.80 | 0.60 / 0.75 |
| 1080p | S | 0.69 / 0.77 | 0.85 / 0.92 | 0.74 / 0.80 |
| 2160p | S | 0.43 / 0.45 | 0.45 / 0.50 | 0.44 / 0.48 |
| any | M, L | - | - | - |

### VBR wall (G3, one rung per real-title cell)
x265's CRF 28 rate as the target, maxrate 1.5x, buffer 2x, two interleaved rounds, no size match.
| class | tier | Mac | x86-64 | Graviton4 | cells |
|---|---|---|---|---|---|
| 720p | L | 0.42 / 0.76 | 0.51 / 0.58 | 0.40 / 0.50 | 7 |
| 1080p | M | 0.63 / 0.68 | 0.79 / 0.81 | 0.69 / 0.72 | 3 |
| 1080p | L | 0.61 / 0.85 | 0.77 / 0.84 | 0.70 / 0.78 | 7 |
| 2160p | M | 0.64 / 0.66 | 0.60 / 0.75 | 0.53 / 0.71 | 3 |
| 2160p | L | 0.65 / 0.75 | 0.66 / 0.73 | 0.56 / 0.66 | 6 |

## Quality at matched size (VMAF v1 delta median / worst; PSNR-Y worst)
Matched size within 1% on every cell. The worst BD curve is from the Mac's
five-rung ladders (CRF 20-32 and VBR at x265's rates, 6 threads each side).
Tier S has no ladder under the spec.
| class | tier | dVMAF v1 | dPSNR-Y | worst cell | worst BD-v1 curve | x86-64 | Graviton4 | verdict |
|---|---|---|---|---|---|---|---|---|
| CIF | S | +2.33 / +0.56 | -0.68 | stefan_cif | n/a | inferred | identical streams | PASS |
| 720p | S | +2.39 / +0.86 | -0.29 | shields_720p | n/a | inferred | identical streams | PASS |
| 720p | L | +1.67 / +0.61 | -0.42 | bbb_720p | -0.41% (nocturne VBR) | inferred | identical streams | PASS |
| 720p | C | +2.55 / +2.55 | -0.07 | lash1924_720p | -26.04% (lash1924 VBR) | inferred | identical streams | PASS |
| 1080p | S | +0.61 / -0.05 | -0.59 | sunflower_1080p | n/a | inferred | identical streams | PASS |
| 1080p | M | +1.96 / +0.63 | -0.40 | chimera_1080p | -4.83% (chimera VBR) | reproduced | identical streams | PASS |
| 1080p | L | +1.08 / +0.70 | -0.81 | bbb_1080p | -6.88% (nocturne VBR) | reproduced | identical streams | PASS |
| 1080p | C | +2.53 / +0.14 | -0.90 | worldcup_far_1080p | -2.22% (worldcup_far CRF) | inferred | identical streams | PASS |
| 2160p | S | +1.86 / +1.22 | -0.49 | bbb_2160p | n/a | inferred | identical streams | PASS |
| 2160p | M | +1.40 / +0.06 | -0.48 | chimera_2160p | +0.70% (chimera VBR) | reproduced (+0.78%) | identical streams | PASS |
| 2160p | L | +1.18 / +0.44 | -0.54 | bbb_2160p | -4.59% (bbb VBR) | inferred | identical except nocturne | PASS |
| 2160p | C | -0.24 / -0.24 | -0.68 | boxing_2160p | -3.81% (boxing CRF) | inferred | identical streams | PASS |

Soft cells (median passes, a cell between -0.5 and -1.0 VMAF v1): none.

"Inferred" means the row rests on four Mac curves re-run on x86 (all within
0.2 BD points) and on bit-exact kernels; x86 doesn't write the Mac's streams
because clang fuses float multiply-adds on arm64 only. Graviton4 writes the
Mac's streams: 36 of 38 timed encodes per goal match by md5, and the two
misses are explained (stefan_cif re-solved on purpose; nocturne_2160p decoded
differently on the cloud).

### BD-rate from the ladders (bar: BD-VMAF v1 <= +1.0%)
52 of 52 tier M and L cell-modes landed and pass. Range: -28.69%
(meridian_1080p M CRF) to +0.70% (chimera_2160p M VBR). Only chimera_2160p VBR
is within 0.5 points of the bar. Tier C: 15 of 15 BD curves inside the bar;
the three screen VBR curves share no v1 range with x265's (ours above at fewer
bits), so no BD is defined. Per-cell figures: PARITY-REPORT-yah265-2026-11-24.md.

### VBR rate tracking (bar: every rung within +-10%)
| source | rungs | ours, worst | x265, worst | ours inside |
|---|---|---|---|---|
| Mac ladders, tiers M and L | 130 | -7.7% (sintel_720p) | -14.6% (chimera_1080p) | 130 of 130 |
| x86 ladders (chimera_2160p, cosmos_1080p) | 10 | -5.2% | -14.4% | 10 of 10 |
| x86 VBR walls | 26 | -7.3% (sintel_720p) | -14.5% (chimera) | 26 of 26 |
| Graviton4 VBR walls | 26 | -7.3% (sintel_720p) | -14.5% (chimera) | 26 of 26 |
| Mac tier C ladders (information) | 45 | -26.8% (spreadsheet_1080p) | -44.3% (spreadsheet_1080p) | 34 of 45 |

### Lookahead depth (owner rule: swept, not inherited)
Ours at 12, 20 and 30 against x265 at 20, on six cells under CRF and VBR. 20
is medium's own and best or level on chimera_2160p and meridian_2160p; 30 is
up to 1.2 points better on bbb_1080p and level elsewhere; 12 is worse on
every CRF cell. Table: PARITY-REPORT-yah265-2026-11-24.md.

## End credits (supplementary, not a gate)
At 1080p, each title's credits coded alone; ours at the cell's matched crf,
x265 at CRF 28, five rungs a side, v1 and PSNR-Y on every frame.
| title | BD-PSNR-Y | BD-v1 | source |
|---|---|---|---|
| tears | +4.1% | -4.0% | CREDITS 11-23 |
| meridian | -60.5% | - (not scored on v1) | parity report 11-24 |
| stem2 | -36.5% | -37.5% | FADES 11-24 (was +20.8% / +43.0%) |
| sintel | -35.8% | - (not scored on v1) | parity report 11-24 |
| bbb | +10.7% | +7.9% | FADES 11-24 |
| cosmos | -18.9% | - (not scored on v1) | parity report 11-24 |
| nocturne | -29.4% | - (not scored on v1) | parity report 11-24 |

bbb is the one title behind on v1. Tears is behind on PSNR-Y and ahead on v1;
its cause is medium's speed cuts, and no story frame shares the credits'
texture, so no gate was built.

## Target quality (`--target-vmaf`), not part of the bar
Against dynamic-crf on the 1080p story cells of four tier L films, v1 85 and
93, 12 threads, every final file scored whole and independently on v1
(docs/data/target-vmaf-vs-dynamic-crf.csv). Times are from a loaded box.
| title | target | ours (floor) crf / whole v1 / kbit/s / wall s | dynamic-crf crf / whole v1 / kbit/s / wall s |
|---|---|---|---|
| sintel | 85 | 30.25 / 88.90 / 1073 / 457 | 32 / 86.38 / 856 / 989 |
| sintel | 93 | 23.0 / 95.46 / 2731 / 631 | 26 / 93.47 / 1851 / 694 |
| tears | 85 | 29.0 / 85.42 / 742 / 140 | 28 / 87.13 / 848 / 223 |
| tears | 93 | 23.25 / 93.15 / 1611 / 172 | 22 / 94.26 / 1922 / 230 |
| nocturne | 85 | 20.25 / 86.61 / 4931 / 554 | 21 / 85.64 / 4151 / 1249 |
| nocturne | 93 | 13.5 / 94.17 / 43004 / 1516 | 14 / 93.66 / 36886 / 2325 |
| meridian | 85 | 24.50 / 85.31 / 580 / not re-timed | 22 / 87.88 / 857 / 606 |
| meridian | 93 | 14.75 / 93.42 / 3318 / 394 | 13 / 94.38 / 4723 / 711 |

Every floor row now lands at or above its target on the whole file. The
search picks its CRF on windows, and windows can read above the whole film.
Since 01f36802, a whole file under the floor is encoded again at a lower CRF,
up to 3 times, and the run fails if it still misses. Meridian at 85 was
crf 25.25 at 84.47. One correction takes it to crf 24.50 at 85.31, and the
file grows from 35.4 MB to 39.7 MB. No other row was under, so none moved.

## Changed since the 2026-11-24 report
Encoder changes after the gating rows' binary (3ee7824e). None was re-timed;
each one's own gates are in its commit.
- Fades to black: weighted prediction now fires on them (`N265_WP_TIES` 24, 721b7c5d). stem2 credits go +43.0% -> -37.5% BD-v1 and sintel_720p -8.52% -> -10.31%. Tier M 1080p and tier D are unchanged. A few tier L windows move by a handful of maps, inside a crf +0.05 null band or better.
- All-flat HD pictures: the 64x64 skip gate passes only an exact-zero residual (`N265_CB64_FLATPIC`, e37bde6a). No tier S or D picture is all flat; story windows that are pool -0.46% BD-v1 against main.
- A dark or empty opening no longer sets the CRF level for the whole file (item 63, c40e2334). Sparks' whole film goes 0.25x -> 0.94x of x265's bytes at CRF 18. Tier D, CIF-7 and every tier M and L story opening are unchanged.
- Per-shot quality (`--shot-plan`, opt-in) ships under CRF, capped CRF and capped VBR (3d74ca79, eade6661). Under capped VBR, ten titles read median -0.54% BD-v1 against flat, worst +0.61%.
- Live control v1.3: a capped-VBR step back up lands on its target (fdd98093), and a RATE step past 3x refits from the coded runs (559604d5, 999c1198). A stream with no events is main's byte for byte.
- AVX-512 intra prediction at 16x16 and 32x32 (d6c042a8) and the RDOQ family (b558ceb1; rdoq_sb_nz stays on AVX2). Bit-exact; checkasm runs the AVX-512 tier under Intel SDE in CI. x86 G3 above was timed before them, so it's conservative.
- `--target-vmaf-floor` holds on the whole file: a file under the floor is encoded again at a lower CRF, up to 3 times, and the run fails if it still misses (01f36802). The floor now refuses `--no-target-vmaf-verify`.
- Correctness: the second TSan report (knob globals under `--gop-parallel`) is fixed (105e07d1), and CI's LeakSanitizer failures are fixed (0724cee8). No stream moved.
- Data: the four void meridian cells timed (da165269), peak RSS per thread count (61f4eab9), credits on v1 (b14e7bb1), a corpus table (73fe1a10), `make results` to rerun the Mac's results page (edc6af1f), and target-vmaf against dynamic-crf (80f43919).

Last 24 hours on main (2026-11-27): the `--target-vmaf-floor` correction (CLI only; no stream from a plain encode moves) and the cloud tier C G3 run.

- Tier C G3 off the Mac (7e71a725, cd485883): Xeon 8488C median 0.81 over 8 clips, EPYC 9R14 0.70 over 8, Graviton4 0.69 over 9, each at 16 threads on 4d0d13a4, size-matched on each box. Worst is wikipedia_1080p on all three (1.03, 0.99, 0.99). G1 and G2 weren't run on tier C.

## Not valid or not measured
- Tier C G1 and G2 on any machine. Tier C G3 for lash1924_1080p on x86, and fresh Mac G3 for the four clips items 61 and 62 moved. Tier C speed doesn't gate.
- SSE4.2 and AVX2 capped on tiers M and L, and any CPU that stops at AVX2 or SSE4.2 in silicon. No AMD x86 CPU outside tier C.
- Graviton3 G1, G2, tiers M and L, and VBR.
- Full BD ladders on x86 beyond the four reproduced curves, and on Graviton (stream identity stands in).
- HDR rescore of Meridian and Chimera from the 10-bit PQ masters. The HDR rows on the site are not this report's.
- Credits on v1 for meridian, sintel, cosmos and nocturne (PSNR-Y only).
- Memory at `--threads auto` (18 workers). Peak RSS is measured at 1-12 threads only.
- An x86 TSan rerun at 4 and 16 threads with both race fixes in.
- Tier M G1 and G2 on the Mac were read on the whole segment, before the 30 s window rule. Tier L G1 and G2 come from one run each on every machine.
- The gating rows were measured on 3ee7824e (Mac) and 504fdda (cloud). main (cd485883) carries the stream-moving changes listed above. They leave tiers S, M and D byte-identical, apart from tier M and L pictures that are all flat or fade to black. Those were measured as story windows, not as whole re-timed cells.
