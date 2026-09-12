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

# Phase 8 item 1: the sample width is a compile-time type (src/common/
# pixel.h), so the depth is a build and not a runtime choice. The 8-bit
# build is the measured encoder and must stay byte-identical, so it gets
# the bare names it always had; the 10-bit build suffixes its library and
# its binary (`n265-10`) so the two can sit in one prefix.
bit_depth = get_option('bit_depth').to_int()
add_project_arguments('-DN265_BIT_DEPTH=@0@'.format(bit_depth), language : 'c')
depth_suffix = bit_depth == 8 ? '' : '-@0@'.format(bit_depth)
is_8bit = bit_depth == 8

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
#
# Phase 8 item 1 adds the depth test. Every kernel here is typed on
# uint8_t and processes 16 samples a vector; a 10-bit build runs the C
# reference for all of them, by not compiling the kernels at all rather
# than by branching away from them, so there is no 16-bit lane path to
# get wrong before item 3 writes one. N265_NEON is therefore the single
# guard: undefined at 10-bit, so the dispatch sites compile out too.
if cpu_family == 'aarch64' and is_8bit
  add_project_arguments('-DN265_NEON=1', language : 'c')
  lib_sources += [
    'src/encoder/arm/transform_neon.c',
    'src/encoder/arm/satd_neon.c',
    'src/encoder/arm/mc_neon.c',
    'src/encoder/arm/rdoq_neon.c',
    'src/encoder/arm/cost_neon.c',
    'src/encoder/arm/intra_neon.c',
  ]
endif

# Both linkage forms (yah264 lesson: plain library() made the CLI link
# the dylib and cross a shared-library boundary on every hot call, worth
# 12-14% of wall time). In-tree consumers - the CLI and tests, the
# binaries the boards time - link the STATIC form; the shared form with
# hidden visibility is what gets installed for external callers (the
# ffmpeg wrapper).
libyah265_both = both_libraries('yah265' + depth_suffix, lib_sources,
  include_directories : inc,
  dependencies : [m_dep, threads_dep],
  version : meson.project_version(),
  soversion : '0',
  gnu_symbol_visibility : 'hidden',
  install : true)

libyah265 = libyah265_both.get_shared_lib()
libyah265_static = libyah265_both.get_static_lib()

# One header, and it is depth-agnostic: N265_BIT_DEPTH defaults to 8
# inside it, so an external consumer that links the 8-bit library needs
# no define. A consumer of the 10-bit library defines it to 10. Installed
# once, by the 8-bit build, so the two builds cannot clobber each other.
if is_8bit
  install_headers('include/yah265.h')
endif

pkg = import('pkgconfig')
pkg.generate(libyah265,
  name : 'yah265' + depth_suffix,
  filebase : 'yah265' + depth_suffix,
  description : 'HEVC/H.265 encoder',
  url : 'https://github.com/terranvigil/yah265',
  extra_cflags : is_8bit ? [] : ['-DN265_BIT_DEPTH=@0@'.format(bit_depth)],
  subdirs : '.')

yah265_dep = declare_dependency(
  link_with : libyah265_static,
  include_directories : inc)

n265 = executable('n265' + depth_suffix, 'cli/n265.c',
  dependencies : yah265_dep,
  install : true)

subdir('tests')
