# Contributing to yah265

## Clean-room policy (read before writing any encoder code)

yah265 is an independent implementation written from the ITU-T H.265
specification. It isn't derived from x265, x264 or any other encoder. x265 is
used only as a benchmark, through its released binary, its public options and
its measured behavior.

Rules:

1. Do not copy code from x265, x264, HM, or any other encoder into this
   repository, in any form, including transliteration into another style or
   language. Encoder code is written fresh for this repo. The exception is
   yah264, a sibling encoder written under this same policy. Its tooling,
   encoder code and algorithms may all be copied here directly. Tooling
   means build files, harness and measurement scripts and checkasm
   scaffolding. The exception doesn't cover anything yah264 itself wasn't
   allowed to contain.
2. Do not port the internal architecture of another encoder file by file.
   Reading another encoder's source and then writing "the same thing" from
   memory is a derivative work. Don't.
3. You may freely use the ITU-T H.265 specification, published papers,
   public documentation and your own knowledge of how video coding works.
   You may also run the HM reference software as a conformance oracle and
   compare its output. Do not copy it.
4. CLI flag names and semantics such as `--preset`, `--crf`, `--tune` and
   `--bframes` are a user-facing convention and may match x265's. Command
   vocabularies are a method of operation, not protected expression. The API
   may follow the common encoder-library pattern of a params struct, a
   picture in and NAL units out. Every line of header and implementation
   text must still be written here, originally.
5. Core encoder modules (mode decision, entropy coding, rate control, motion
   estimation) are written from the specification and the literature, never
   from memory of another encoder's source. If you know another encoder's
   source well, contribute to the DSP kernels, build, tooling, or tests
   instead.
6. **Comparisons use the other encoder's released binary and nothing else.**
   A comparison runs x265 as shipped, with its public options, and reads its
   output. That output is bytes, timings, metric scores and the decoded
   pictures. Nothing from x265 is checked in or linked into anything yah265
   ships. A result is a
   CALIBRATION NUMBER, never a design input. What you may bring back is a
   measurement, never a construction.
7. **"Inspiration and logic, never raw code."** You may read a paper, an
   algorithm description, or a published explanation of what a kernel
   computes, and implement it here originally. You may not transcribe
   another encoder's instruction sequence, register allocation or file
   structure. If your kernel would look like a diff against that encoder's,
   it is a derivative work no matter what language it is written in.

If you are unsure whether something crosses the line, ask in a PR before
writing.

## Engineering rules

- The library is C11. Kernels use SIMD intrinsics with runtime dispatch,
  NEON first. Tooling is Python. Public headers stay C99-compatible.
- Every DSP kernel ships with a C reference and a checkasm test that validates
  the optimized path against the reference and benchmarks it. No kernel
  merges without checkasm coverage.
- Two runs of one configuration at `--threads 1` must write identical
  bytes. Above one thread the same check runs, and a mismatch is a warning.
- Every encode the conformance gate produces is decoded by FFmpeg's HEVC
  decoder and by libde265. Both decodes must match the encoder's
  reconstruction bit for bit. The gate runs locally and in CI.
- Match the surrounding code style. No trailing whitespace. Tabs are not
  used; indent with four spaces.
