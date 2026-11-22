# homebrew-yah265

Homebrew formulas for [yah265](https://github.com/terranvigil/yah265), an HEVC encoder.

```sh
brew tap terranvigil/yah265
brew install y265            # the y265 command line and libyah265
brew install ffmpeg-yah265   # ffmpeg with -c:v libyah265
```

Both build from source on your machine. `ffmpeg-yah265` installs as `ffmpeg-yah265` and `ffprobe-yah265`, so it doesn't replace Homebrew's own `ffmpeg`.

```sh
ffmpeg-yah265 -i in.mp4 -c:v libyah265 -preset medium -crf 25 out.mp4
```

yah265 and any ffmpeg built with it are GPL-2.0-or-later.

## Maintainers

The formulas live in `packaging/homebrew/` in the yah265 repo and are copied here at each release. `packaging/homebrew/bump.sh vX.Y.Z` writes the tag's tarball and the pinned ffmpeg commit into both.
