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
  'src/common/cpu.c',
  'src/threadpool/ntp.c',
  'src/encoder/cabac.c',
  'src/encoder/tables.c',
  'src/encoder/transform.c',
  'src/encoder/intra.c',
  'src/encoder/residual.c',
  'src/encoder/rdoq.c',
  'src/encoder/psy.c',
  'src/encoder/ctu.c',
  'src/encoder/entropy.c',
  'src/encoder/mc.c',
  'src/encoder/refchk.c',
  'src/encoder/satd.c',
  'src/encoder/est.c',
  'src/encoder/la.c',
  'src/encoder/rc.c',
  'src/encoder/mv.c',
  'src/encoder/deblock.c',
  'src/encoder/sao.c',
  'src/encoder/headers.c',
  'src/encoder/preset.c',
  'src/encoder/encoder.c',
]

# Phase 6, goal 3. NEON is baseline on aarch64 and this box's clang already
# defaults to the host CPU, so there is no -march here on purpose: the only
# thing the flag does is let the dispatch sites and src/encoder/arm/*
# compile in. The C kernels stay first-class and stay where they are - the
# _neon translation units only ADD entry points, one file per kernel family
# so the family worktrees never touch the same TU and this list is written
# once. Runtime detection (src/common/cpu.c) still decides, so a build for
# aarch64 that runs somewhere without the optional extensions is fine.
if cpu_family == 'aarch64'
  add_project_arguments('-DN265_NEON=1', language : 'c')
  lib_sources += [
    'src/encoder/arm/transform_neon.c',
    'src/encoder/arm/satd_neon.c',
    'src/encoder/arm/mc_neon.c',
    'src/encoder/arm/cost_neon.c',
  ]
endif

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
