---
title: How x265 was run - yah265
description: The x265 version and build behind the comparisons and the command for each goal.
---

# How x265 was run

The comparisons on this site are against x265 <!--trust:x265-version-->4.3+1-e9b8812<!--/trust:x265-version-->. x265 always runs `--preset medium` with no `--tune`. Everything else is its default unless a table below says otherwise. See [the methodology](methodology.md) for why each measurement is taken the way it is.

## The build on each machine

<!--trust:x265-builds-->
| machine | x265 build | x265's instruction sets | yah265's instruction sets |
|---|---|---|---|
| Apple M5 Max, 18 cores | Homebrew | NEON, DotProd, I8MM | NEON |
| Intel Xeon 8488C, 16 vCPUs | the 4.3 release tarball, clang 18, 8-bit | its default, which stops at AVX2 on this CPU. A second run used `--asm avx512` | AVX-512 over AVX2 |
| AWS Graviton4, 16 cores | the 4.3 release tarball, clang 18, 8-bit | NEON, SVE, SVE2, DotProd, I8MM, SVE2 BitPerm | NEON, DotProd, SVE, SVE2 |

The Homebrew build reports `[Mac OS X][clang 21.0.0][64 bit] 8bit+10bit+12bit`.
<!--/trust:x265-builds-->

On the Xeon x265 stops at AVX2 by default while yah265 uses AVX-512. I also [timed x265](results.md#by-machine) with its AVX-512 code switched on, then both encoders held to AVX2 and to SSE4.2.

## The command for each goal

These are the commands for one cell, ten minutes of Tears of Steel at 1080p. Another cell changes only the input and yah265's CRF. Both encoders read the same Y4M frames from one pipe.

<!--trust:x265-commands-->
**[G3](methodology.md#the-three-speed-goals): both encoders as shipped**

```sh
cat tears_1080p.y4m | x265 --input - --y4m --preset medium --crf 28 --log-level error -o x265.hevc
cat tears_1080p.y4m | y265 - --crf 24.88 -o yah265.hevc
```

**[G2](methodology.md#the-three-speed-goals): plain C on all cores**

```sh
cat tears_1080p.y4m | x265 --input - --y4m --preset medium --crf 28 --no-asm --log-level error -o x265.hevc
cat tears_1080p.y4m | N265_ASM_OFF=all y265 - --crf 24.88 -o yah265.hevc
```

**[G1](methodology.md#the-three-speed-goals): plain C on one thread**

```sh
cat tears_1080p.y4m | x265 --input - --y4m --preset medium --crf 28 --pools none --frame-threads 1 --no-asm --log-level error -o x265.hevc
cat tears_1080p.y4m | N265_ASM_OFF=all y265 - --crf 24.88 --threads 1 -o yah265.hevc
```

**Capped VBR as shipped**

```sh
cat tears_1080p.y4m | x265 --input - --y4m --preset medium --bitrate 1279 --vbv-maxrate 1918 --vbv-bufsize 2558 --log-level error -o x265.hevc
cat tears_1080p.y4m | y265 - --bitrate 1279 --vbv-maxrate 1918 --vbv-bufsize 2558 -o yah265.hevc
```

**The untimed rate ladders and size matching**

```sh
cat tears_1080p.y4m | x265 --input - --y4m --preset medium --crf 26 --pools 6 --frame-threads 4 --log-level error -o x265.hevc
cat tears_1080p.y4m | y265 - --crf 26 --threads 6 -o yah265.hevc
```
<!--/trust:x265-commands-->

x265 runs at CRF <!--trust:x265-ref-crf-->28<!--/trust:x265-ref-crf-->, its default. yah265's CRF is fractional because I adjusted it until its file was within 1% of x265's size.

For capped VBR both encoders get the same three numbers. The target is the bitrate x265 reached on that clip at CRF. The peak is 1.5 times the target and the buffer is twice it.

[G1 and G2](methodology.md#the-three-speed-goals) switch the hand-written SIMD off on both sides to compare the algorithms alone.

## Threads and SIMD

<!--trust:x265-threads-->
| measurement | yah265 threads | x265 threads | SIMD |
|---|---|---|---|
| [G3](methodology.md#the-three-speed-goals) | its default: one worker per CPU | its defaults for pools and frame threads | on |
| [G2](methodology.md#the-three-speed-goals) | its default: one worker per CPU | its defaults for pools and frame threads | off: `N265_ASM_OFF=all` and `--no-asm` |
| [G1](methodology.md#the-three-speed-goals) | `--threads 1` | `--pools none --frame-threads 1` | off: `N265_ASM_OFF=all` and `--no-asm` |
| capped VBR | its default: one worker per CPU | its defaults for pools and frame threads | on |
| rate ladders and the size matching | `--threads 6` | `--pools 6 --frame-threads 4` | on |
| cloud machines, [G3](methodology.md#the-three-speed-goals), [G2](methodology.md#the-three-speed-goals) and capped VBR | `--threads 16` on 16 CPUs | `--pools 16` on the same 16 CPUs | as the goal says |
| Xeon held to one tier | `N265_CPU_MAX=sse42` or `avx2` | `--asm sse4.2` or `--asm avx2` | on, capped |
<!--/trust:x265-threads-->

x265's output depends on its frame-thread count. Because it picks four on the Mac, the untimed encodes use four too.

## Lookahead

<!--trust:x265-lookahead-->
| encoder | lookahead at 720p and up | lookahead at CIF | set by |
|---|--:|--:|---|
| yah265 | 20 | 12 | its `medium` |
| x265 | 20 | 20 | its `medium` |
<!--/trust:x265-lookahead-->

I kept x265 at its default depth and [swept yah265's own depth](results.md#lookahead-depth) around it.

## What x265 wasn't given

I didn't use x265's tunes for grain and animation. The Homebrew build has no screen-content mode. yah265's `medium` adapts to some of that content by itself.
