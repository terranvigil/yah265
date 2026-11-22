---
title: Try it in your browser - yah265
description: A page that runs yah265 in your browser. Drop a short clip, pick a preset and a CRF, and compare the encode with the source. Nothing is uploaded.
---

# Try it in your browser

The [playground](playground/) runs yah265 inside your browser, so you can try it without installing anything. Drop a short clip on the page, pick a preset and a CRF, and press Encode. Your clip never leaves your machine.

<p class="callout"><a href="playground/">Open the playground</a></p>

## What you get

When the encode ends, the page shows your source and yah265's output on one picture, split down the middle. Drag the divider to move it. You can play the clip or step through it frame by frame.

Below the picture it lists:

- the file size and the bitrate
- PSNR-Y against the source
- the encode time and the speed in frames per second

Two buttons save the encode. The `.mp4` plays in most video players and in Safari. The `.hevc` is the raw stream, which ffmpeg, VLC and mpv play.

The right half of the picture is the encoder's own reconstruction. Those are the exact frames a decoder makes from the stream, so your browser doesn't need HEVC playback to show them.

## What it accepts

- Any clip your browser can play, such as MP4 with H.264, WebM or MOV.
- A raw 4:2:0 8-bit `.y4m` file.

The page scales the clip down to fit 1280x720 and reads it at up to 30 frames a second. It encodes the first 10 seconds at 480p, or about 4 seconds at 720p. The limit is memory, because the page holds the source and the result.

## About the speed

The playground runs a WebAssembly build of yah265 on one thread, with none of its SIMD kernels. The native encoder uses NEON, SVE, SSE4.2, AVX2 or AVX-512 and all of your cores. On one thread alone, the native build ran 3 to 6 times faster than the page. So the speed the page shows is the slow end for yah265.

The stream itself is the one `y265 --threads 1` writes. To compare yah265 with x265 at full speed on your own clip, use [`y265 bench`](bench.md).

## How it's built

`make wasm` builds the encoder with Emscripten, and `make playground-serve` serves the page on port 8265. The page and its scripts are in the repository's `playground/` folder. The docs site rebuilds the module on every publish.
