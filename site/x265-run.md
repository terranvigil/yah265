---
title: How x265 was run - yah265
description: The x265 version and build behind every comparison, with the command for each goal and the threads, lookahead and instruction sets each encoder ran with.
---

# How x265 was run

Every comparison on this site is against x265 <!--trust:x265-version-->4.3+1-e9b8812<!--/trust:x265-version-->. This page gives the build on each machine, the command for each kind of reading and the settings each encoder ran with. A script writes the tables and the commands. Its sources are the parity reports and the code that timed the runs. The page can't drift from what was run.

x265 always runs `--preset medium` with no `--tune`. Everything else is its default unless a table below says otherwise. [The methodology](methodology.md) explains why each reading is taken the way it is.

## The build on each machine

<!--trust:x265-builds-->
| machine | x265 build | x265's instruction sets | yah265's instruction sets |
|---|---|---|---|
| Apple M5 Max, 18 cores | Homebrew | NEON, DotProd, I8MM | NEON |
| Intel Xeon 8488C, 16 vCPUs | the 4.3 release tarball, clang 18, 8-bit | its default, which stops at AVX2 on this CPU. A second arm ran `--asm avx512` | AVX-512 over AVX2 |
| AWS Graviton4, 16 cores | the 4.3 release tarball, clang 18, 8-bit | NEON, SVE, SVE2, DotProd, I8MM, SVE2 BitPerm | NEON, DotProd, SVE, SVE2 |

The Homebrew build reports `[Mac OS X][clang 21.0.0][64 bit] 8bit+10bit+12bit`.
<!--/trust:x265-builds-->

On the Mac x265 is the Homebrew package as it installs. On the cloud machines I built it from the 4.3 release tarball with its assembly on. The Xeon needs a note, because x265's default there uses AVX2 while yah265 uses AVX-512. I timed a second x265 arm with its AVX-512 code switched on. I also held both encoders to AVX2, then to SSE4.2. [Results](results.md#by-machine) has all of those readings.

## The command for each goal

These are the commands for one cell, ten minutes of Tears of Steel at 1080p. Another cell changes only the input and yah265's CRF. Both encoders read the same Y4M frames from one pipe.

<!--trust:x265-commands-->
**G3: both encoders as shipped**

```sh
cat tears_1080p.y4m | x265 --input - --y4m --preset medium --crf 28 --log-level error -o x265.hevc
cat tears_1080p.y4m | y265 - --crf 24.88 -o yah265.hevc
```

**G2: plain C on every core**

```sh
cat tears_1080p.y4m | x265 --input - --y4m --preset medium --crf 28 --no-asm --log-level error -o x265.hevc
cat tears_1080p.y4m | N265_ASM_OFF=all y265 - --crf 24.88 -o yah265.hevc
```

**G1: plain C on one thread**

```sh
cat tears_1080p.y4m | x265 --input - --y4m --preset medium --crf 28 --pools none --frame-threads 1 --no-asm --log-level error -o x265.hevc
cat tears_1080p.y4m | N265_ASM_OFF=all y265 - --crf 24.88 --threads 1 -o yah265.hevc
```

**Capped VBR as shipped**

```sh
cat tears_1080p.y4m | x265 --input - --y4m --preset medium --bitrate 1279 --vbv-maxrate 1918 --vbv-bufsize 2558 --log-level error -o x265.hevc
cat tears_1080p.y4m | y265 - --bitrate 1279 --vbv-maxrate 1918 --vbv-bufsize 2558 -o yah265.hevc
```

**The untimed rate ladders and size solve**

```sh
cat tears_1080p.y4m | x265 --input - --y4m --preset medium --crf 26 --pools 6 --frame-threads 4 --log-level error -o x265.hevc
cat tears_1080p.y4m | y265 - --crf 26 --threads 6 -o yah265.hevc
```
<!--/trust:x265-commands-->

x265 runs at CRF <!--trust:x265-ref-crf-->28<!--/trust:x265-ref-crf-->, its default. yah265's CRF is fractional because it's solved until its file is within 1% of x265's size.

For capped VBR both encoders get the same three numbers. The target is the bitrate x265 reached at one CRF rung on that cell. The peak is 1.5 times the target. The buffer is twice the target.

G1 and G2 switch the hand-written SIMD off on both sides to compare the algorithms alone. Read them as engineering checks, because x265 isn't meant to run without its assembly.

## Threads and SIMD

<!--trust:x265-threads-->
| reading | yah265 threads | x265 threads | SIMD |
|---|---|---|---|
| G3 | its default: one worker per CPU | its defaults for pools and frame threads | on |
| G2 | its default: one worker per CPU | its defaults for pools and frame threads | off: `N265_ASM_OFF=all` and `--no-asm` |
| G1 | `--threads 1` | `--pools none --frame-threads 1` | off: `N265_ASM_OFF=all` and `--no-asm` |
| capped VBR | its default: one worker per CPU | its defaults for pools and frame threads | on |
| rate ladders and the size solve | `--threads 6` | `--pools 6 --frame-threads 4` | on |
| cloud machines, every timed goal | `--threads 16` on 16 CPUs | `--pools 16` on the same 16 CPUs | as the goal says |
| Xeon held to one tier | `N265_CPU_MAX=sse42` or `avx2` | `--asm sse4.2` or `--asm avx2` | on, capped |
<!--/trust:x265-threads-->

x265's pictures change with its frame-thread count. On the 18-core Mac its own choice is four. The untimed encodes name four to code the same pictures as the timed default. G1's anchor runs without a pool because its timed run does.

## Lookahead

<!--trust:x265-lookahead-->
| encoder | lookahead at 720p and up | lookahead at CIF | set by |
|---|--:|--:|---|
| yah265 | 20 | 12 | its `medium` |
| x265 | 20 | 20 | its `medium` |
<!--/trust:x265-lookahead-->

x265 stays at its default depth in every reading. yah265's own depth was swept around it. [Results](results.md#lookahead-depth) has that sweep.

## What x265 wasn't given

No reading uses x265's tunes for grain and for animation. The Homebrew build has no screen-content mode. Because yah265's `medium` adapts to some of that content by itself, the content-type clips compare one encoder's defaults with the other's. [The skeptic's questions](faq.md) says what I make of that.
