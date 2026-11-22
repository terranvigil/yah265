# ffmpeg integration

`libyah265` is an encoder wrapper in a fork of ffmpeg. With it,
`-c:v libyah265` works the way `-c:v libx265` does. Decode, encode and mux
run in one process. The fork is the `yah265` branch of
[terranvigil/FFmpeg](https://github.com/terranvigil/FFmpeg). That branch is
built on `yah264`, which is ffmpeg n8.1.2 plus the yah264 wrapper. One ffmpeg
can build both encoders. Because the wrapper is LGPL code in libavcodec, it
isn't in this repository. This repo ships a shared library, the header and a
pkg-config file.

## The wrapper and y265 write the same stream

The wrapper hands its options to the library's option parser,
`n265_config_parse`. `y265` runs the same parser. `n265_config_apply` then
resolves the options against the picture as the command line does. Given the
same options, the wrapper and `y265` write the same bytes, SEI included:

```
ffmpeg -i clip.y4m -c:v libyah265 -preset medium -crf 25 -threads 1 -f hevc a.hevc
y265 clip.y4m --preset medium --crf 25 --threads 1 -o b.hevc
cmp a.hevc b.hevc
```

That holds at CIF, 720p, 1080p and 2160p, at 8 and 10 bits, under CRF and
under VBR with a buffer.

NOTE: ffmpeg reads the Y4M `A` tag, the aspect, and `y265` doesn't. When a
source has non-square pixels, such as the CIF clips' `A128:117`, the wrapper
writes the SAR. `y265` needs `--sar 128:117` to match.

The SEI lists options in the order they were given. For the SEI to match,
the CLI's flags have to come in the wrapper's order:

1. preset
2. tune
3. rate control
4. VBV
5. keyint
6. bframes
7. ref
8. the color description
9. SAR
10. `-yah265-params`, in its own order

## Options

| ffmpeg | y265 | note |
|---|---|---|
| `-preset` | `--preset` | unset is y265's default, medium |
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
| `-color_range pc` | `--range full` | a decoder assumes limited, which isn't written |
| chroma sample location | `--chromaloc` | 4:2:0 only. Left, type 0, is the default and isn't written |
| sample aspect ratio | `--sar` | not for 1:1 |
| frame rate, or the inverse time base | the VUI timing | |
| `-yah265-params k=v:k:k=v` | any coding option | the CLI's names without the dashes |

With no rate option, the encode is y265's default, CQP at QP 26. x265's
default is CRF 28. Name `-crf` if that's what you want.

`-yah265-params` takes the coding options `y265 --help` lists, by their long
names. A switch takes no value, or `1` or `0`: `no-info`, `info=0`, `aud`,
`weightp=0`. A value with a colon in it can't go through the list.
`-x265-params` has the same limit. `dhdr10-info` and `dolby-vision-rpu` are
refused because they read files the command line loads. I haven't wired the
frame side data path yet.

Pixel formats are yuv420p, yuv422p and yuv444p, and their 10-bit forms. The
installed library has both depths. The pixel format picks one. To code
8-bit content as Main10, add `-pix_fmt yuv420p10le`.

## Packets and timestamps

The wrapper uses ffmpeg's receive-packet API. It declares
`AV_CODEC_CAP_OTHER_THREADS` so that ffmpeg doesn't add frame threads of its
own. Init builds an `n265_config`, parses the options above into it, applies
it and opens the encoder.

The library returns NAL units in coding order. One call can return several
pictures. `n265_encoder_access_units` names each access unit's NALs and the
display index of its picture. The wrapper makes one packet per access unit.
The packet's pts is the input pts at that display index. Its dts trails by
the stream's reorder depth, which `n265_encoder_reorder_depth` returns and
the SPS writes as `sps_max_num_reorder_pics`. The k-th packet's dts is the
pts at display index k minus that depth, extrapolated before the first
picture. An IDR or CRA packet is a key packet. A NAL with no picture, the
kind `--eos` and `--eob` write at the end of a flush, goes in the last
packet.

The parameter sets are written in-band in the first access unit, as `y265`
writes them. Raw Annex-B output decodes because of that. Under
`AV_CODEC_FLAG_GLOBAL_HEADER`, which MP4 and Matroska set, the same bytes
also go to extradata from `n265_encoder_headers`.

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

The config has to outlive the encoder because open keeps pointers to its
strings. The library's configuration is process-global until open. Apply and
open must not race with another apply and open in the same process. The
wrapper marks its init as not thread-safe so that libavcodec serializes it.

The Main10 entry points are the same calls with a `_10` suffix, such as
`n265_config_apply_10` and `n265_encoder_open_10`. A caller that links the
8-bit names declares them itself. Building against the header with
`N265_BIT_DEPTH=10` renames the calls instead.

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

`--enable-gpl` is required because the library is GPL-2.0-or-later. The
default 8-bit build installs `libyah265` with both depths in it, plus
`yah265.h` and `yah265.pc`. A `-Dbit_depth=10` build installs a 10-bit-only
`libyah265-10` and `yah265-10.pc`. The fork's configure doesn't look for
those.

Rebuild the wrapper after `include/yah265.h` changes. If the install prefix
moved, delete `libavcodec/libyah265.o` and `.d` first, because make can keep
the old object.

## Speed

The wrapper and a pipe into `y265` wrote the same stream and ran within
noise of each other at the default thread count.

| clip | `-c:v libyah265` | `ffmpeg ... -f yuv4mpegpipe - \| y265 -` |
|---|---|---|
| crowd_run 1080p, 500 frames | 4.51 s, 4.84 s | 4.60 s, 5.05 s |
| bbb 2160p, 300 frames | 2.63 s, 2.75 s | 2.62 s, 2.77 s |

Both used `-preset medium -crf 25`. Because the `y265` reader runs on its
own thread, the pipe costs about what ffmpeg's in-process handoff does.
