# ffmpeg integration

`libyah265` is an encoder wrapper in a fork of ffmpeg, so `-c:v libyah265`
works the way `-c:v libx265` does. Decode, encode and mux run in one process
instead of a pipe into `n265`. The fork is `github.com/terranvigil/FFmpeg`,
branch `yah265`. That branch sits on top of `yah264`, which is ffmpeg n8.1.2
plus the yah264 wrapper, so one ffmpeg can build both encoders. The wrapper
lives in libavcodec and is LGPL, so it can't live in this repository. This
repo ships the installable side: a shared library, the header and a
pkg-config file.

## Same options, same stream

The wrapper doesn't map options onto a parameter struct of its own. It hands
them to the library's option parser, `n265_config_parse`, which is the code
`n265` itself runs for every coding option. `n265_config_apply` then
resolves them against the picture exactly as the command line does: the
preset's resolution rules, the tune, the range checks, the identification
SEI's option string. So the two can't drift. Given the same options, the
wrapper and `n265` write the same bytes, SEI included:

```
ffmpeg -i clip.y4m -c:v libyah265 -preset medium -crf 25 -threads 1 -f hevc a.hevc
n265 clip.y4m --preset medium --crf 25 --threads 1 -o b.hevc
cmp a.hevc b.hevc
```

That holds at CIF, 720p, 1080p and 2160p, at 8 and 10 bits, under CRF and
under VBR with a buffer. One thing to watch: ffmpeg reads the Y4M `A` (aspect)
tag and n265 doesn't. On a source with non-square pixels, such as the CIF
clips' `A128:117`, the wrapper writes the SAR and `n265` needs `--sar 128:117`
to match. The SEI lists options in the order they were given, so the CLI's
flags have to come in the wrapper's order for the SEI to match too. That
order is: preset, tune, rate control, VBV, keyint, bframes, ref, the colour
description, SAR, then `-yah265-params` in its own order.

## Options

| ffmpeg | n265 | note |
|---|---|---|
| `-preset` | `--preset` | unset is n265's default, medium |
| `-tune` | `--tune` | |
| `-crf` | `--crf` | |
| `-qp` | `--qp` | |
| `-b:v` | `--bitrate`, kbit/s | wins over `-crf` and `-qp` |
| `-maxrate`, `-bufsize` | `--vbv-maxrate`, `--vbv-bufsize` | kbit/s and kbit |
| `-g` | `--keyint` | unset leaves the default |
| `-bf` | `--bframes` | unset leaves the preset's |
| `-refs` | `--ref` | unset leaves the preset's |
| `-threads` | `--threads` | 0 is one per CPU on both sides |
| `-color_primaries`, `-color_trc`, `-colorspace` | `--colorprim`, `--transfer`, `--colormatrix` | H.273 codes; only when set |
| `-color_range pc` | `--range full` | limited is what a decoder assumes, so it isn't written |
| chroma sample location | `--chromaloc` | 4:2:0 only, and not for left (type 0, the default) |
| sample aspect ratio | `--sar` | not for 1:1 |
| frame rate, or the inverse time base | the VUI timing | |
| `-yah265-params k=v:k:k=v` | any coding option | the CLI's names without the dashes |

With no rate option at all the encode is n265's default, CQP at QP 26. That's
not x265's CRF 28, so name `-crf` if that's what you want.

`-yah265-params` takes every coding option `n265 --help` lists, by its long
name. A switch takes no value, or `1`/`0`: `no-info`, `info=0`, `aud`,
`weightp=0`. A value with a colon in it can't go through the list, the same
limit `-x265-params` has. `dhdr10-info` and `dolby-vision-rpu` are refused:
they read files the command line loads, and the frame side data path isn't
wired yet.

Pixel formats are yuv420p, yuv422p and yuv444p, and their 10-bit forms. The
installed library carries both depths, so one build takes all six. The pixel
format picks the depth. To code 8-bit content as Main10, add
`-pix_fmt yuv420p10le`.

## How it works

The wrapper uses ffmpeg's receive-packet API and declares
`AV_CODEC_CAP_OTHER_THREADS`, so ffmpeg doesn't add frame threads of its own.
Init builds an `n265_config`, parses the options above into it, applies it
and opens the encoder.

The library returns NAL units in coding order, and one call can carry several
pictures. `n265_encoder_access_units` names each access unit's NALs and its
picture's display index. The wrapper makes one packet per access unit and
takes its pts from the input pts at that display index. dts trails by the
stream's reorder depth (`n265_encoder_reorder_depth`, the SPS's
`sps_max_num_reorder_pics`). The k-th packet's dts is the pts at display
index k minus that depth, extrapolated before the first picture. An IDR or
CRA packet is a key packet. A NAL with no picture (`--eos`, `--eob` at the
end of a flush) rides on the last packet.

The parameter sets are written in-band in the first access unit, as `n265`
writes them, so raw Annex-B output decodes. With
`AV_CODEC_FLAG_GLOBAL_HEADER` (MP4, Matroska) they also go to extradata,
from `n265_encoder_headers`. Those are the same bytes.

## Library API

```c
n265_config *c = n265_config_new();
n265_config_parse(c, "preset", "medium");
n265_config_parse_list(c, "crf=25:no-info");
n265_param p;
n265_param_default(&p);
p.width = 1920; p.height = 1080; p.fps_num = 25; p.fps_den = 1;
n265_config_apply(c, &p);
n265_encoder *e = n265_encoder_open(&p);
/* n265_encoder_encode, n265_encoder_access_units, ... */
n265_encoder_close(e);
n265_config_free(c);
```

The config has to outlive the encoder, because open keeps pointers to its
strings. The library's configuration is process-global until open, so
apply and open must not race with another apply and open in the same
process. The wrapper marks its init as not thread-safe, and libavcodec
serializes it.

The Main10 entry points are the same calls with a `_10` suffix
(`n265_config_apply_10`, `n265_encoder_open_10`, ...). A caller that links
the 8-bit names declares them itself. Building against the header with
`N265_BIT_DEPTH=10` renames every call instead.

## Building it

```
FFB=$HOME/src/build/ffboard
meson setup build-ff -Dbuildtype=release -Dprefix="$FFB/y265inst"
ninja -C build-ff install

git clone -b yah265 https://github.com/terranvigil/FFmpeg "$FFB/ffmpeg-yah265"
cd "$FFB/ffmpeg-yah265"
PKG_CONFIG_PATH="$FFB/y265inst/lib/pkgconfig" \
  ./configure --enable-gpl --enable-libyah265
make -j6
otool -L ffmpeg | grep yah265    # ldd on Linux
```

`--enable-gpl` is required because the library is GPL-2.0-or-later. The 8-bit
build (the default) installs `libyah265` with both depths in it, `yah265.h`
and `yah265.pc`. A `-Dbit_depth=10` build installs a 10-bit-only
`libyah265-10` and `yah265-10.pc`, which the fork's configure doesn't look
for.

Rebuild the wrapper after `include/yah265.h` changes. Delete
`libavcodec/libyah265.o` and `.d` first if the install prefix moved, or make
can keep the old object.

## Speed

In-process and piped ran within noise of each other at the default thread
count, both writing the same stream:

| clip | `-c:v libyah265` | `ffmpeg ... -f yuv4mpegpipe - \| n265 -` |
|---|---|---|
| crowd_run 1080p, 500 frames | 4.51 s, 4.84 s | 4.60 s, 5.05 s |
| bbb 2160p, 300 frames | 2.63 s, 2.75 s | 2.62 s, 2.77 s |

Both used `-preset medium -crf 25`. The `n265` reader already runs on its own
thread, so the pipe costs about what ffmpeg's in-process handoff does.
