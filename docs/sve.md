# The aarch64 SVE tier

yah265 has an SVE tier on Linux aarch64, written as C intrinsics beside
the NEON one. It covers the pixel metrics and the vertical luma
interpolation, at 8 bits and Main10. The kernels are vector-length agnostic
and bit-exact with their C references at any vector length. Because each
one ships only where it measured faster than NEON, the tier dispatches
differently on a 256-bit core and a 128-bit one.

No Apple part has SVE. I built, checked and timed the tier on AWS Graviton
spot instances:

| CPU | core | SVE | vector length | instance types used |
|---|---|---|---|---|
| Graviton3 | Neoverse V1 | SVE, no SVE2 | 256 bits | c7gd.2xlarge, c7g.4xlarge |
| Graviton4 | Neoverse V2 | SVE and SVE2 | 128 bits | r8g.2xlarge, c8g.4xlarge |

Both cores issue the same vector width per cycle through NEON and SVE. The
V1 has two 256-bit SVE pipes or four 128-bit NEON ones. The V2 has four
128-bit pipes for both. SVE can't win on raw throughput. A kernel wins by
executing fewer instructions: wider lanes per instruction at 256 bits,
predication instead of scalar tails, and instructions NEON lacks, such
as the 16-bit dot product into 64-bit lanes.

## What ships where

| kernel | Graviton3 (256-bit) | Graviton4 (128-bit) |
|---|---|---|
| SATD, 8 bits | w >= 16, in 8x8 tiles with w and h multiples of 8 | no |
| SATD, Main10 | w >= 16 | w >= 16 |
| SAD, 8 bits | w = 12, 24, 48 | w = 12, 24, 48 |
| SAD, Main10 | w >= 12 | 12 <= w <= 32 |
| SSD, 8 bits | 4x4 and w >= 12 | 4x4 and w >= 12 |
| SSD, Main10 | w >= 8 | w >= 8 |
| sample sum and sum of squares | 16x16 | 16x16 |
| vertical luma pass, to pixels | w = 4, 8, 12 | w = 4, 12 |
| vertical luma pass, to 14-bit | all w | w = 4, 12 |

Everything else stays on NEON. `src/encoder/arm/sve.h` has one predicate
per kernel, read off the checkasm tables below. Because the predicates test
`N265_CPU_SVE` and `N265_CPU_SVE256` and name no CPU model, a future 256-bit
SVE2 core takes the Graviton3 column. A 128-bit SVE-only core takes the
Graviton4 one.

The next table gives user-space cycles at encode level against
`N265_CPU_MAX=neon` on the same binary, at one thread and `--preset medium`.
Each cell is the `perf stat` median of 3 with the arms interleaved. The two
arms wrote identical streams. Main10 is `--output-depth 10` on the same
clips.

| clip | Graviton3 8-bit | Graviton3 Main10 | Graviton4 8-bit | Graviton4 Main10 |
|---|--:|--:|--:|--:|
| park_joy_720p, 30 frames | 0.994 | 0.974 | 0.999 | 0.990 |
| foreman_cif, 60 frames | 0.984 | 0.957 | 0.999 | 0.977 |
| mobile_cif, 60 frames | 0.988 | 0.964 | 0.999 | |

The tier saves about 1% on Graviton3 at 8 bits and 3-4% at Main10. On
Graviton4 it saves 1-2% at Main10 and nothing measurable at 8 bits, where
the kernels that win are a small share of the encode. The run-to-run spread
is 0.2%. The park_joy row is from the 8-vCPU development boxes. The other
rows are from the 16-vCPU probe boxes.
[`scripts/cloud/sve_ab.sh`](../scripts/cloud/sve_ab.sh) runs the comparison.

## Build and dispatch

- **Detection** is in `src/common/cpu.c` and is Linux only. `HWCAP_SVE`
  sets `N265_CPU_SVE`. `HWCAP2_SVE2` sets `N265_CPU_SVE2`.
  `prctl(PR_SVE_GET_VL)` sets `N265_CPU_SVE256` when the process runs at
  256 bits or more. macOS reports none of them.
- **Tiers.** `N265_CPU_MAX` takes `sve`, which is SVE without SVE2, and
  `sve2`. Both are above `i8mm`. `N265_CPU_MAX=neon` is the NEON tier on an
  SVE machine. The A/B and the escape cmp both use it.
- **meson.** The option is `-Dsve=auto|enabled|disabled`. The default
  `auto` means Linux aarch64 with a compiler that has the SVE intrinsics. A
  family is in once `src/encoder/arm/<family>_sve.c` exists. Those files
  compile into their own archive with `-march=armv8.2-a+dotprod+sve`, as
  the x86 tiers do. That keeps the rest of the library at baseline armv8-a.
  Main10 names take the `_10` suffix through `symbols10.h`. On macOS
  `-Dsve=enabled` compiles files that nothing selects.
- **Dispatch.** The SVE kernel is tried inside its NEON twin's
  `n265_asm_on` branch, under its own predicate:

  ```c
  if (n265_asm_on(N265_ASM_SATD)) {
  #if N265_SVE_SATD
      if (N265_SVE_SAD_OK(n265_cpu_word(), w, h))
          return n265_sad_wh_sve(a, as, b, bs, w, h);
  #endif
      return n265_sad_wh_neon(a, as, b, bs, w, h);
  }
  ```

  `N265_ASM_OFF` and `N265_CPU_MAX` therefore turn both off. The first
  version checked SVE in a block of its own ahead of NEON. On Graviton4 at
  8 bits that check cost +0.1 to +0.3% cycles, more than the kernels saved.
  Nested, a site where SVE doesn't apply pays one bit test.
- **checkasm** runs an `sve` tier, with the feature word cut to drop SVE2,
  and an `sve2` tier. Each tier runs the families SVE dispatches from,
  whose entry points then take the SVE path. Each tier also runs
  `tests/checkasm/sve.c`, which holds each SVE kernel to its C reference
  directly and through its entry point. That test uses:

  - random and adversarial input
  - tight and padded strides
  - borders and row padding scrambled between two runs
  - guard bytes around every output
  - page guards hard against both ends of each declared window

  `--bench` prints NEON against SVE per shape.

## The kernels

**SATD** is in `satd_sve.c`. It is the NEON 8x8 tile's transform with each
128-bit segment holding one tile. The transposes are `trn1` and `trn2` at
16, 32 and 64 bits, which never cross a segment. A 256-bit vector therefore
takes two horizontally adjacent tiles through the same instructions. Pass 2
drops its last stage, because `|x + y| + |x - y| = 2 max(|x|, |y|)`. At
Main10 that keeps the whole tile in int16, since pass 2's two stages reach
4 x 8184 = 32736. The NEON Main10 tile widens pass 2 to int32 instead. The
per-tile rounding is done per segment, with a `trn` pair summing two u64
lanes. At 128 bits a vector holds one tile. At 8 bits that measured even
with NEON.

**SAD, SSD and the sums** are in `satd_sve.c` too. Because one loop walks
the row in `svcntb()` or `svcnth()` chunks under `whilelt`, any width runs
with no tail code. At 8 bits the kernel takes the `udot` of `|a - b|` into
u32 lanes, with ones for SAD or with itself for SSD. At Main10 it takes the
16-bit `udot` into u64 lanes, which NEON doesn't have. The wins are at
Main10 and at 12, 24 and 48, the widths NEON handles with tails. At 16, 32
and 64 the NEON SAD keeps its accumulators in u16 across the block and stays
faster.

**The vertical luma pass** is in `mc_sve.c`. It covers the
cached-first-stage forms `n265_mc_luma_hf` and `n265_mc_luma_14_hf`.
Because `ld1sh` loads each int16 row sign-extended into int32 lanes, a tap
is one `mla` per `svcntw()` outputs. Four output rows share eleven loaded
rows. The taps are constants per `fy`. The stores narrow with a truncating
`st1b` or `st1h` after the clip. The 14-bit store keeps the low 16 bits of
`s >> 6`, which matches the C's `int16_t` conversion of a value that can
exceed int16. At 256 bits the 14-bit form wins at all widths. The pixel
form pays four instructions for its round, clip and store and wins only at
the narrow widths.

## Kernel timings

The table is from `checkasm --bench sve` on clang 18 release builds. Each
cell is ns per call, best of three, as NEON / SVE = ratio. Above 1, SVE is
faster. The luma rows are at `fy` = 2, which uses all eight taps. At `fy` =
1 and 3 the 14-bit ratios are the same and the pixel ones a few points
lower. `sum_sq` has no NEON kernel off 16 wide. Only its 16-wide rows are a
comparison.

| kernel | shape | Graviton3 8-bit | Graviton3 Main10 | Graviton4 8-bit | Graviton4 Main10 |
|---|---|--:|--:|--:|--:|
| satd | 8x8 | 14.7 / 24.4 = 0.60 | 21 / 26 = 0.78 | 12.3 / 15.3 = 0.81 | 15.5 / 15.1 = 1.03 |
| satd | 16x16 | 59 / 42 = 1.40 | 77 / 46 = 1.67 | 49 / 51 = 0.97 | 62 / 53 = 1.17 |
| satd | 16x8 | 32 / 24 = 1.32 | 41 / 26 = 1.56 | 26 / 28 = 0.93 | 33 / 29 = 1.13 |
| satd | 24x24 | 126 / 116 = 1.09 | 163 / 121 = 1.35 | 105 / 106 = 0.99 | 133 / 112 = 1.19 |
| satd | 32x32 | 220 / 152 = 1.45 | 285 / 162 = 1.76 | 184 / 183 = 1.00 | 234 / 184 = 1.27 |
| satd | 32x16 | 112 / 79 = 1.43 | 145 / 85 = 1.71 | 93 / 95 = 0.99 | 118 / 96 = 1.24 |
| satd | 48x48 | 486 / 335 = 1.45 | 633 / 350 = 1.81 | 410 / 406 = 1.01 | 519 / 406 = 1.28 |
| satd | 48x24 | 245 / 171 = 1.43 | 317 / 179 = 1.78 | 206 / 206 = 1.00 | 261 / 206 = 1.26 |
| satd | 64x64 | 860 / 594 = 1.45 | 1118 / 611 = 1.83 | 729 / 717 = 1.02 | 920 / 717 = 1.28 |
| satd | 64x32 | 431 / 299 = 1.44 | 559 / 310 = 1.80 | 365 / 361 = 1.01 | 462 / 361 = 1.28 |
| sad | 4x4 | 4.6 / 6.5 = 0.71 | 5.4 / 6.7 = 0.81 | 3.3 / 5.0 = 0.66 | 3.9 / 4.8 = 0.83 |
| sad | 8x8 | 6.9 / 9.6 = 0.72 | 7.7 / 10.6 = 0.73 | 5.6 / 8.0 = 0.71 | 5.9 / 7.6 = 0.77 |
| sad | 8x4 | 5.4 / 6.5 = 0.82 | 5.4 / 6.7 = 0.81 | 4.0 / 5.0 = 0.80 | 4.3 / 4.8 = 0.90 |
| sad | 12x12 | 24 / 13 = 1.88 | 24 / 15 = 1.64 | 22 / 11 = 1.99 | 21 / 15 = 1.41 |
| sad | 12x6 | 14.6 / 8.1 = 1.81 | 13.8 / 8.7 = 1.59 | 12.9 / 6.6 = 1.97 | 11.8 / 8.9 = 1.32 |
| sad | 16x16 | 9.8 / 15.8 = 0.62 | 38 / 19 = 2.01 | 7.9 / 14.2 = 0.56 | 29 / 20 = 1.49 |
| sad | 16x8 | 6.5 / 9.6 = 0.68 | 20 / 11 = 1.89 | 5.4 / 8.1 = 0.67 | 16.1 / 10.7 = 1.50 |
| sad | 24x24 | 45 / 23 = 1.95 | 65 / 42 = 1.55 | 37 / 31 = 1.17 | 48 / 39 = 1.24 |
| sad | 24x12 | 24 / 13 = 1.87 | 30 / 23 = 1.32 | 19.6 / 17.0 = 1.15 | 25 / 20 = 1.24 |
| sad | 32x32 | 29 / 30 = 0.98 | 94 / 55 = 1.71 | 27 / 41 = 0.67 | 71 / 65 = 1.09 |
| sad | 32x16 | 16.6 / 16.2 = 1.02 | 46 / 29 = 1.57 | 14.9 / 20.0 = 0.75 | 37 / 34 = 1.09 |
| sad | 48x48 | 140 / 80 = 1.74 | 162 / 127 = 1.28 | 119 / 77 = 1.56 | 131 / 145 = 0.91 |
| sad | 48x24 | 75 / 39 = 1.94 | 86 / 60 = 1.44 | 62 / 39 = 1.58 | 67 / 70 = 0.95 |
| sad | 64x64 | 102 / 104 = 0.98 | 246 / 210 = 1.17 | 80 / 151 = 0.53 | 209 / 237 = 0.88 |
| sad | 64x32 | 54 / 51 = 1.05 | 128 / 106 = 1.21 | 41 / 72 = 0.57 | 103 / 119 = 0.87 |
| ssd | 4x4 | 7.3 / 5.6 = 1.32 | 5.0 / 6.3 = 0.79 | 6.4 / 4.3 = 1.50 | 3.6 / 4.5 = 0.80 |
| ssd | 8x8 | 7.3 / 8.9 = 0.82 | 14.9 / 10.3 = 1.44 | 6.2 / 7.1 = 0.88 | 13.6 / 7.3 = 1.85 |
| ssd | 8x4 | 5.8 / 5.6 = 1.03 | 8.5 / 6.3 = 1.35 | 4.8 / 4.3 = 1.12 | 7.9 / 4.5 = 1.77 |
| ssd | 12x12 | 14.5 / 12.0 = 1.21 | 22 / 14 = 1.55 | 12.3 / 10.0 = 1.23 | 19.6 / 14.5 = 1.36 |
| ssd | 12x6 | 8.7 / 7.1 = 1.23 | 12.5 / 8.3 = 1.51 | 7.5 / 5.7 = 1.31 | 11.1 / 8.2 = 1.36 |
| ssd | 16x16 | 27 / 15 = 1.77 | 36 / 18 = 1.92 | 22 / 13 = 1.71 | 31 / 19 = 1.64 |
| ssd | 16x8 | 14.6 / 8.9 = 1.64 | 19.1 / 10.3 = 1.85 | 12.8 / 7.2 = 1.79 | 16.4 / 10.7 = 1.53 |
| ssd | 24x24 | 46 / 22 = 2.04 | 71 / 42 = 1.67 | 39 / 27 = 1.41 | 54 / 39 = 1.39 |
| ssd | 24x12 | 23 / 12 = 1.89 | 33 / 22 = 1.49 | 21 / 14 = 1.47 | 28 / 21 = 1.34 |
| ssd | 32x32 | 70 / 29 = 2.41 | 107 / 55 = 1.96 | 65 / 36 = 1.80 | 82 / 64 = 1.28 |
| ssd | 32x16 | 35 / 15 = 2.28 | 51 / 28 = 1.78 | 34 / 19 = 1.80 | 42 / 33 = 1.28 |
| ssd | 48x48 | 138 / 80 = 1.73 | 221 / 127 = 1.74 | 114 / 76 = 1.50 | 174 / 144 = 1.20 |
| ssd | 48x24 | 73 / 39 = 1.89 | 108 / 59 = 1.83 | 55 / 39 = 1.42 | 85 / 70 = 1.21 |
| ssd | 64x64 | 206 / 105 = 1.96 | 395 / 211 = 1.87 | 190 / 129 = 1.48 | 297 / 240 = 1.24 |
| ssd | 64x32 | 107 / 54 = 1.97 | 194 / 104 = 1.88 | 101 / 65 = 1.57 | 147 / 117 = 1.26 |
| sum_sq | 16x16 | 16.6 / 14.3 = 1.16 | 32 / 17 = 1.91 | 13.3 / 11.1 = 1.20 | 23 / 19 = 1.20 |
| sum_sq | 16x8 | 8.8 / 8.5 = 1.04 | 12.9 / 9.4 = 1.38 | 6.8 / 6.8 = 1.00 | 9.8 / 10.3 = 0.95 |
| vertical luma, to pixels | 4x4 | 27 / 17 = 1.52 | 32 / 18 = 1.79 | 23 / 11 = 1.97 | 27 / 11 = 2.40 |
| vertical luma, to 14-bit | 4x4 | 29 / 15 = 1.90 | 30 / 15 = 2.00 | 24 / 10 = 2.40 | 25 / 10 = 2.46 |
| vertical luma, to pixels | 8x8 | 38 / 34 = 1.13 | 39 / 34 = 1.14 | 35 / 38 = 0.91 | 34 / 38 = 0.89 |
| vertical luma, to 14-bit | 8x8 | 39 / 28 = 1.38 | 39 / 28 = 1.40 | 35 / 31 = 1.10 | 35 / 31 = 1.11 |
| vertical luma, to pixels | 12x12 | 112 / 97 = 1.15 | 131 / 98 = 1.34 | 98 / 83 = 1.18 | 114 / 83 = 1.37 |
| vertical luma, to 14-bit | 12x12 | 124 / 80 = 1.54 | 124 / 80 = 1.55 | 101 / 68 = 1.47 | 101 / 68 = 1.47 |
| vertical luma, to pixels | 16x16 | 138 / 129 = 1.07 | 137 / 130 = 1.05 | 126 / 146 = 0.86 | 120 / 146 = 0.82 |
| vertical luma, to 14-bit | 16x16 | 137 / 107 = 1.28 | 137 / 107 = 1.29 | 124 / 121 = 1.03 | 124 / 121 = 1.03 |
| vertical luma, to pixels | 24x24 | 303 / 289 = 1.05 | 299 / 291 = 1.03 | 277 / 327 = 0.85 | 266 / 327 = 0.81 |
| vertical luma, to 14-bit | 24x24 | 301 / 238 = 1.27 | 301 / 238 = 1.26 | 271 / 270 = 1.00 | 272 / 270 = 1.01 |
| vertical luma, to pixels | 32x32 | 534 / 510 = 1.05 | 527 / 514 = 1.03 | 488 / 580 = 0.84 | 466 / 579 = 0.80 |
| vertical luma, to 14-bit | 32x32 | 530 / 422 = 1.26 | 530 / 422 = 1.26 | 476 / 480 = 0.99 | 475 / 479 = 0.99 |
| vertical luma, to pixels | 48x48 | 1192 / 1147 = 1.04 | 1177 / 1156 = 1.02 | 1091 / 1300 = 0.84 | 1040 / 1301 = 0.80 |
| vertical luma, to 14-bit | 48x48 | 1181 / 948 = 1.25 | 1181 / 947 = 1.25 | 1056 / 1077 = 0.98 | 1056 / 1076 = 0.98 |
| vertical luma, to pixels | 64x64 | 2113 / 2038 = 1.04 | 2086 / 2051 = 1.02 | 1935 / 2311 = 0.84 | 1843 / 2312 = 0.80 |
| vertical luma, to 14-bit | 64x64 | 2092 / 1683 = 1.24 | 2092 / 1682 = 1.24 | 1864 / 1913 = 0.97 | 1866 / 1909 = 0.98 |

## Kernels that didn't ship

| what | result |
|---|---|
| SATD at 8 bits on Graviton4 | 0.93-1.02x. At 128 bits it is the NEON tile, instruction for instruction. |
| SATD 8x8 alone | 0.60-0.81x, 1.03x at Main10 on Graviton4. At 256 bits half the vector is empty. |
| SAD at w = 16, 32, 64, 8 bits | 0.53-0.75x on Graviton4, 0.62-1.05x on Graviton3. NEON's u16 accumulators beat `udot` there. |
| SAD at w = 48 and 64, Main10, Graviton4 | 0.87-0.95x. |
| SAD 4 and 8 wide | 0.66-0.90x at both depths on both CPUs. |
| SSD 4x4 at Main10, SSD 8x8 at 8 bits | 0.79-0.88x. |
| Vertical luma pass, w >= 16, Graviton4 | 0.80-0.86x to pixels, 0.97-1.03x to 14-bit. |
| Vertical luma pass to pixels, w >= 16, Graviton3 | 1.02-1.07x at `fy` = 2 and 0.96-0.98x at `fy` = 1: a wash. |
| Dequant (transform family) | 0.78-1.10x on Graviton3 in two designs. The NEON kernel's widening multiply-accumulate pairs are already as few instructions per sample as SVE's 32-bit lanes, and tracking the nonzero box costs SVE more. I removed it. |
| Forward and inverse transforms | I didn't write them. Their cost is the coefficient multiply-accumulate. SVE without SVE2 has no indexed integer multiply-accumulate and no widening one. At 256 bits a tap is a broadcast plus a 32-bit `mla` per eight columns, which is the same instruction count as NEON's `smlal` pair per eight. SVE2 at 128 bits has `smlalb` and `smlalt`, which is NEON's pair again. |

## Two NEON fixes

The first Graviton benches compared SVE against a slower NEON than the
Mac runs. I fixed both causes in the NEON code. The SVE numbers above are
against the fixed NEON.

- The SSD `udot` arm was missing on Linux. It was compiled under
  `__ARM_FEATURE_DOTPROD`, which Apple clang defines by default and a
  generic Linux `-march=armv8-a` doesn't. It now builds with a
  `target("+dotprod")` attribute wherever the compiler can. The runtime
  check on `N265_CPU_DOTPROD` still decides. 64x64 SSD went from 366 to
  206 ns on Graviton3.
- clang 18 didn't unroll the vertical luma taps. It left the NEON kernel's
  eleven row registers and tap loop as a runtime loop with the rows spilled
  to the stack. The kernel ran about as fast as the C. At 8x8 on Graviton4
  NEON took 56 ns against 61 ns for the C. `#pragma GCC unroll` on those
  loops brings it to 35 ns. Apple clang already unrolled them.

Both fixes are bit-exact. The escape cmp and the Mac identity run below
cover them.

## The gate

[`scripts/cloud/sve_gate.sh`](../scripts/cloud/sve_gate.sh) ran these legs
on a Graviton3 c7g.4xlarge and a Graviton4 c8g.4xlarge:

| leg | Graviton3 | Graviton4 |
|---|---|---|
| checkasm, 8-bit and Main10 | pass: 12 checks over neon, dotprod, sve | pass: 15 checks over neon, dotprod, sve, sve2 |
| the same under ASan+UBSan | pass | pass |
| the same at 128 bits on the 256-bit core | pass, with `/proc/sys/abi/sve_default_vector_length` = 16 | native |
| checkasm built with gcc 13, both depths | pass | |
| `meson test` | 16 ok, 1 skip, 0 fail | 16 ok, 1 skip, 0 fail |
| escape cmp, CIF-7 x medium, fast, 24 frames, one thread | 28 of 28 SAME, stream and recon | 28 of 28 SAME |
| escape cmp, Main10, foreman, mobile, stefan, 12 frames | 12 of 12 SAME | 12 of 12 SAME |
| fast conformance | 123 pass, 0 fail | 123 pass, 0 fail |
| fast conformance, Main10 | 34 pass, 0 fail | 34 pass, 0 fail |

The escape cmp is `scripts/sve_identity.sh`. It compares `N265_ASM_OFF=all`
against the default, which is SVE, and against `N265_CPU_MAX=neon`. Its
streams were the same on both machines, cell for cell.

Conformance needs dec265 from libde265 at commit 4d45a6b, the one the Mac
uses. Ubuntu 24.04's packaged 1.0.15 fails `opengop_tools` and `nal_opts`
with a dec265 recon mismatch. Both streams have end-of-sequence NALs.
Because the packaged decoder fails them with the kernels off too,
`setup_arm64.sh` builds dec265 from source.

### Mutations

I applied each of these breaks to the source by itself and rebuilt.
checkasm caught each one at its depth:

| break | caught as |
|---|---|
| SATD rounding constant | `satd_sve 8x8` mismatch, both depths |
| SATD dropped max term (8-bit and Main10 forms) | mismatch at its depth |
| SATD 64-bit transpose pairing swapped | mismatch, both depths |
| SATD per-segment pair sum taking one lane twice | mismatch, both depths |
| SAD `abd` as `sub` (8-bit and Main10 forms) | mismatch at its depth |
| SSD reads one sample past the row (both forms) | mismatch at its depth (the scramble pass) |
| sum of squares as plain sum | mismatch |
| vertical pass rounding constant | mismatch, both depths |
| 14-bit store saturates instead of wrapping | `mc_v_sve 4x12 fy2` 14-bit mismatch (kind 1 input) |
| pixel clip bound one low | mismatch, both depths |
| tap rows off by one | mismatch, both depths |
| pixel store one column past the block | mismatch and pad check |
| 14-bit form reads one column left of the window | mismatch |

### On the Mac

The Mac has no SVE and the tree builds no SVE objects there. `make test`
passes with 17 ok, provenance clean and the symbol check OK. The two NEON
changes and the dispatch nesting move no byte: `identity_ab.sh MATRIX=t1`
against main before the tier is SAME on 14 of 14 cells.

### Linux against the Mac

The Graviton build's NEON streams equal the Mac's default streams on 20 of
20 cells, md5 for md5. The cells are the escape cmp's, run with
`N265_CPU_MAX=neon` at one thread: CIF-7 at medium and fast for 24 frames,
and the Main10 three for 12 frames.

## Graviton against x265

`probe_arm64.py` timed both encoders over 3 rounds on two 16-vCPU
instances, a Graviton4 c8g.4xlarge and a Graviton3 c7g.4xlarge. I built both
encoders on the box with clang 18. x265 is 4.3 from the release tarball with its own
aarch64 assembly. It reports that assembly as NEON, SVE, SVE2, DotProd and
I8MM on Graviton4. On Graviton3 it reports NEON, SVE, DotProd and I8MM. The
cells are the probe's three tier S cells at their solved quality points,
from `docs/data/parity-spec-points.json`. Each side runs its own default
lookahead, which is 20 at 720p and up on both.

Each number is yah265's median wall time over x265's. Below 1, yah265 is
faster.

| cell | G1 | G2 | G3 | one thread, SIMD |
|---|--:|--:|--:|--:|
| Graviton4, shields 720p | 0.401 | 0.420 | 0.771 | 0.691 |
| Graviton4, sunflower 1080p | 0.463 | 0.380 | 0.629 | 0.797 |
| Graviton4, bbb 2160p | 0.376 | 0.499 | 0.785 | 0.617 |
| Graviton3, shields 720p | 0.410 | 0.434 | 0.713 | 0.636 |
| Graviton3, sunflower 1080p | 0.481 | 0.394 | 0.598 | 0.733 |
| Graviton3, bbb 2160p | 0.388 | 0.500 | 0.773 | 0.564 |

- G1: one thread, yah265 with `N265_ASM_OFF=all` against x265
  `--no-asm --pools none --frame-threads 1`, at the ft1 point.
- G2: default threads, yah265 with `N265_ASM_OFF=all` against x265
  `--no-asm`, at the def point.
- G3: default threads, both with their SIMD, at the def point.
- One thread, SIMD: yah265 against x265 `--pools none --frame-threads 1`,
  at the ft1 point.

The one-thread arms ran four at a time on their own cores, each with a
`cat` feeder on a core of its own. The default-thread arms ran alone. Each
arm has three wall times. yah265's spread by 1.3% at most. x265's spread by
up to 3.1%, on its default-thread arms on sunflower on Graviton3.

yah265's SIMD buys 1.67-1.77x at one thread on Graviton4 and 1.63-1.72x on
Graviton3. x265's assembly buys it 2.88-3.01x and 2.51-2.61x. That's why
the margin at G3 is narrower than at G1 and G2. Most of the margin comes
from yah265's C.

At these points yah265's bytes against x265's are -0.17% to +1.39% at ft1
and -0.64% to +0.31% at def. yah265's streams on Linux have the same byte
counts as the Mac's on all six cells. The md5 check is the CIF cross-check
in the gate. x265's streams differ from the Mac's brew build by up to 9445
bytes.

## The cloud kit

The arm64 scripts are in `scripts/cloud/`:

| script | what it does |
|---|---|
| `ec2.sh` | picks the Ubuntu 24.04 arm64 image from the instance type, and takes `deadman MIN` |
| `setup_arm64.sh` | installs clang, both decoders and perf. Builds yah265 as release, debug, both sanitizer trees and both depths. Builds x265 4.3 from the release tarball with its own aarch64 assembly. Writes `env.txt` |
| `sve_gate.sh` | the correctness gate above |
| `sve_ab.sh` | the encode-level A/B |
| `probe_arm64.py` | the G1/G2/G3 timing probe. It reads its quality points from `docs/data/parity-spec-points.json` |

`scripts/sve_identity.sh` is the escape cmp.

The probe's sources, downloaded on the instance from their public origins,
matched the Mac's md5s. shields and sunflower came from xiph.org's derf
collection. `src_bbb4k_300` was decoded from Blender's
`bbb_sunflower_2160p_30fps_normal.mp4` at `-ss 60`, 300 frames.
