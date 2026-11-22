# SPDX-License-Identifier: GPL-2.0-or-later
class FfmpegYah265 < Formula
  desc "FFmpeg with the libyah265 HEVC encoder, beside Homebrew's ffmpeg"
  homepage "https://terranvigil.github.io/yah265/"
  url "https://github.com/terranvigil/FFmpeg/archive/ee4958e501a1468e7d63eab4c4788ab23745086a.tar.gz"
  version "8.1.3-yah265"
  sha256 "17e94709988ecd029597a326e945b45944136d51a4fe8058a2e9d73f0c8a3c8a"
  license "GPL-2.0-or-later"
  head "https://github.com/terranvigil/FFmpeg.git", branch: "yah265"

  depends_on "pkgconf" => :build
  depends_on "terranvigil/yah265/y265"

  uses_from_macos "bzip2"
  uses_from_macos "zlib"

  on_intel do
    depends_on "nasm" => :build
  end

  def install
    args = %W[
      --prefix=#{libexec}
      --cc=#{ENV.cc}
      --host-cflags=#{ENV.cflags}
      --host-ldflags=#{ENV.ldflags}
      --enable-gpl
      --enable-libyah265
      --enable-static
      --disable-shared
      --disable-doc
      --disable-ffplay
      --disable-autodetect
      --enable-zlib
      --enable-bzlib
    ]
    args += %w[--enable-videotoolbox --enable-audiotoolbox] if OS.mac?
    system "./configure", *args
    system "make", "ffmpeg", "ffprobe"
    libexec.install "ffmpeg", "ffprobe"
    bin.install_symlink libexec/"ffmpeg" => "ffmpeg-yah265"
    bin.install_symlink libexec/"ffprobe" => "ffprobe-yah265"
  end

  test do
    system bin/"ffmpeg-yah265", "-hide_banner", "-loglevel", "error",
           "-f", "lavfi", "-i", "testsrc2=size=320x240:rate=25", "-frames:v", "12",
           "-pix_fmt", "yuv420p10le", "-c:v", "libyah265", "-crf", "30", testpath/"t.mp4"
    assert_equal "hevc,yuv420p10le",
                 shell_output("#{bin}/ffprobe-yah265 -v error -show_entries " \
                              "stream=codec_name,pix_fmt -of csv=p=0 #{testpath}/t.mp4").strip
  end
end
