# SPDX-License-Identifier: GPL-2.0-or-later
class Y265 < Formula
  desc "HEVC/H.265 encoder: the y265 command-line tool and the libyah265 library"
  homepage "https://terranvigil.github.io/yah265/"
  url "https://github.com/terranvigil/yah265/archive/refs/tags/v0.0.1.tar.gz"
  sha256 "0000000000000000000000000000000000000000000000000000000000000000"
  license "GPL-2.0-or-later"
  head "https://github.com/terranvigil/yah265.git", branch: "main"

  depends_on "meson" => :build
  depends_on "ninja" => :build
  depends_on "pkgconf" => :build

  def install
    system "meson", "setup", "build", *std_meson_args
    system "meson", "compile", "-C", "build", "-j#{ENV.make_jobs}",
           "yah265:shared_library", "yah265:static_library", "y265"
    system "meson", "install", "-C", "build", "--no-rebuild"
  end

  test do
    w = 64
    h = 64
    frame = ("\x80".b * (w * h * 3 / 2))
    File.open(testpath/"in.y4m", "wb") do |f|
      f.write "YUV4MPEG2 W#{w} H#{h} F25:1 Ip A1:1 C420jpeg\n"
      4.times { f.write "FRAME\n#{frame}" }
    end
    system bin/"y265", testpath/"in.y4m", "-o", testpath/"out.hevc", "--crf", "30"
    assert_match(/\A\x00\x00\x00?\x01/n, File.binread(testpath/"out.hevc", 4))
    assert_path_exists lib/"pkgconfig/yah265.pc"
    assert_path_exists include/"yah265.h"
  end
end
