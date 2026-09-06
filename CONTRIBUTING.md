# Contributing to yah265

## Clean-room policy (read before writing any encoder code)

yah265 is an independent implementation. It is not derived from x265, x264, or
any other GPL-licensed encoder, and it must stay that way so the project can
ship under its own licence.

Rules:

1. Do not copy code from x265, x264, HM, or any other encoder into this
   repository, in any form, including transliteration into another style or
   language. Encoder code is written fresh for this repo. The narrow
   exception is yah264's codec-agnostic tooling (build files, harness and
   measurement scripts, checkasm scaffolding): that's our own work
   under this same policy and may be adapted.
2. Do not port the internal architecture of another encoder file by file.
   Reading another encoder's source and then writing "the same thing" from
   memory is a derivative work. Don't.
3. What you may use freely: the H.265/HEVC specification (ITU-T H.265),
   published papers, the HM reference software's behavior as a conformance
   oracle (run it, compare output; do not copy it), public documentation, and
   your own knowledge of how video coding works.
4. CLI flag names and semantics (`--preset`, `--crf`, `--tune`, `--bframes`,
   ...) are a user-facing convention and may match x265's. Command
   vocabularies are a method of operation, not protected expression. The API
   may follow the common encoder-library pattern (a params struct, picture
   in, NAL units out) but
   every line of header and implementation text must be written here,
   originally.
5. Core encoder modules (mode decision, entropy coding, rate control, motion
   estimation) are written from the specification and the literature, never
   from memory of another encoder's source. If you know another encoder's
   source well, contribute to the DSP kernels, build, tooling, or tests
   instead.
6. **Comparisons use the other encoder's released binary and nothing else.**
   A comparison runs x265 as shipped, with its public options, and reads its
   output: bytes, timings, metric scores, the decoded pictures. Nothing from
   x265 is checked in or linked into anything we ship. A result is a
   CALIBRATION NUMBER, never a design input. What you may bring back is a
   measurement, never a construction.
7. **"Inspiration and logic, never raw code."** You may read a paper, an
   algorithm description, or a published explanation of what a kernel
   computes, and implement it here originally. You may not transcribe their
   instruction sequence, their register allocation, or their file structure.
   If your kernel would look like a diff against theirs, it is a derivative
   work no matter what language it is written in.

If you are unsure whether something crosses the line, ask in a PR before
writing.

## Engineering rules

- Language: C11 for the library, SIMD intrinsics (NEON first) with runtime
  dispatch for kernels, Python for tooling. Public headers stay
  C99-compatible.
- Every DSP kernel ships with a C reference and a checkasm test that validates
  the optimized path against the reference and benchmarks it. No kernel
  merges without checkasm coverage.
- Encoded output must be bit-exact reproducible: identical bytes regardless of
  thread count and across runs. This is enforced in CI.
- Every encode produced in CI is decoded by an independent decoder (FFmpeg's
  hevc decoder; libde265 as a second opinion) and checked for bit-exact
  reconstruction match. Conformance is not optional.
- Match the surrounding code style. No trailing whitespace. Tabs are not
  used; indent with four spaces.
