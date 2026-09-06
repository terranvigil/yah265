project('yah265', 'c',
  version : '0.0.1',
  license : 'GPL-2.0-or-later',
  default_options : [
    'c_std=c11',
    'warning_level=3',
    'buildtype=debugoptimized',
  ],
  meson_version : '>=1.1.0')

cc = meson.get_compiler('c')
cpu_family = host_machine.cpu_family()

# libm implicit on macOS/clang, explicit on Linux/gcc
m_dep = cc.find_library('m', required : false)
threads_dep = dependency('threads')

add_project_arguments('-D_GNU_SOURCE', language : 'c')

inc = include_directories('include', 'src')

lib_sources = [
  'src/common/bitstream.c',
  'src/encoder/cabac.c',
  'src/encoder/tables.c',
  'src/encoder/transform.c',
  'src/encoder/intra.c',
  'src/encoder/residual.c',
  'src/encoder/ctu.c',
  'src/encoder/entropy.c',
  'src/encoder/mc.c',
  'src/encoder/satd.c',
  'src/encoder/est.c',
  'src/encoder/la.c',
  'src/encoder/mv.c',
  'src/encoder/deblock.c',
  'src/encoder/sao.c',
  'src/encoder/headers.c',
  'src/encoder/encoder.c',
]

# Both linkage forms (yah264 lesson: plain library() made the CLI link
# the dylib and cross a shared-library boundary on every hot call, worth
# 12-14% of wall time). In-tree consumers - the CLI and tests, the
# binaries the boards time - link the STATIC form; the shared form with
# hidden visibility is what gets installed for external callers (the
# ffmpeg wrapper).
libyah265_both = both_libraries('yah265', lib_sources,
  include_directories : inc,
  dependencies : [m_dep, threads_dep],
  version : meson.project_version(),
  soversion : '0',
  gnu_symbol_visibility : 'hidden',
  install : true)

libyah265 = libyah265_both.get_shared_lib()
libyah265_static = libyah265_both.get_static_lib()

install_headers('include/yah265.h')

pkg = import('pkgconfig')
pkg.generate(libyah265,
  name : 'yah265',
  filebase : 'yah265',
  description : 'HEVC/H.265 encoder',
  url : 'https://github.com/terranvigil/yah265',
  subdirs : '.')

yah265_dep = declare_dependency(
  link_with : libyah265_static,
  include_directories : inc)

n265 = executable('n265', 'cli/n265.c',
  dependencies : yah265_dep,
  install : true)

subdir('tests')
