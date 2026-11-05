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
# the bare names it always had; the 10-bit library and the 10-bit binary
# are suffixed (`n265-10`) so the two can sit in one prefix.
#
# Item 5a: EVERY build directory now builds BOTH libraries, because one
# command line links both and picks the encoder from the input's bit
# depth. The depth therefore moved off the project arguments and onto
# each library TARGET (`-UN265_BIT_DEPTH -DN265_BIT_DEPTH=<d>`), and the
# Main10 library force-includes its symbol namespace so the two archives
# share no name. What `bit_depth` still selects is the depth the TESTS
# are built at - checkasm's lanes and its family list, the unit tests -
# and the suffix the installed library, its pkg-config file and the
# binary carry. It no longer decides what the CLI can encode.
bit_depth = get_option('bit_depth').to_int()
add_project_arguments('-DN265_BIT_DEPTH=@0@'.format(bit_depth), language : 'c')
depth_suffix = bit_depth == 8 ? '' : '-@0@'.format(bit_depth)
is_8bit = bit_depth == 8
symbols10_h = meson.current_source_dir() / 'src' / 'common' / 'symbols10.h'

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
  'src/encoder/nr.c',
  'src/encoder/mcstf.c',
  'src/encoder/psy.c',
  'src/encoder/ctu.c',
  'src/encoder/entropy.c',
  'src/encoder/fp.c',
  'src/encoder/mc.c',
  'src/encoder/refchk.c',
  'src/encoder/satd.c',
  'src/encoder/est.c',
  'src/encoder/la.c',
  'src/encoder/rc.c',
  'src/encoder/vbv.c',
  'src/encoder/wp.c',
  'src/encoder/twopass.c',
  'src/encoder/mv.c',
  'src/encoder/deblock.c',
  'src/encoder/sao.c',
  'src/encoder/grain.c',
  'src/encoder/metrics.c',
  'src/encoder/rcfile.c',
  'src/encoder/hash.c',
  'src/encoder/signal.c',
  'src/encoder/headers.c',
  'src/encoder/level.c',
  'src/encoder/preset.c',
  'src/encoder/encoder.c',
  'src/encoder/ttrace.c',
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
# Phase 8 item 1 added the depth test; item 3 makes it a PER-FAMILY one.
# A kernel that processes 16 samples to a vector is not a 10-bit kernel,
# so a family reaches the Main10 build only once its 16-bit lanes are
# bit-exact with the C reference under a checkasm built at the same
# depth - one family per commit, in the order the 10-bit profile asks
# for. A family absent from this depth's list is not compiled, its
# N265_NEON_<FAMILY> macro is undefined, and its dispatch sites do not
# compile either: the C reference by construction, with nothing to
# branch away from at run time.
#
# Item 5a makes the lists per LIBRARY rather than per build directory,
# since both depths are compiled here now.
all_families = []
families_8 = []
families_10 = []
if cpu_family == 'aarch64'
  all_families = ['transform', 'satd', 'mc', 'rdoq', 'cost', 'intra']
  families_8 = ['transform', 'satd', 'mc', 'rdoq', 'cost', 'intra']
  families_10 = ['transform', 'satd', 'rdoq', 'cost', 'intra', 'mc']  # grown one proved family at a time
  add_project_arguments('-DN265_NEON=1', language : 'c')
endif
neon_families = is_8bit ? families_8 : families_10

# The project arguments still carry THIS build's depth and its family
# macros, because tests/ compiles the family files itself and has to see
# the same lanes the library was built with. The libraries below override
# both, so neither of them depends on the option.
foreach f : neon_families
  add_project_arguments('-DN265_NEON_' + f.to_upper() + '=1', language : 'c')
endforeach

# depth -> the sources and the arguments of one encoder library. The -U
# ahead of each -D is what lets the target's depth win over the project
# argument without a macro-redefined warning; it is the whole trick.
depth_sources = {}
depth_args = {}
foreach d : [8, 10]
  fams = d == 8 ? families_8 : families_10
  srcs = lib_sources
  args = ['-UN265_BIT_DEPTH', '-DN265_BIT_DEPTH=@0@'.format(d)]
  foreach f : all_families
    args += '-UN265_NEON_' + f.to_upper()
    if f in fams
      args += '-DN265_NEON_' + f.to_upper() + '=1'
      srcs += 'src/encoder/arm/' + f + '_neon.c'
    endif
  endforeach
  if d != 8
    # Every symbol this library defines carries a _10 suffix - the public
    # API through include/yah265.h, the internals through this header -
    # so that both archives can be linked into one executable without a
    # definition in one resolving a reference in the other.
    args += ['-include', symbols10_h]
  endif
  depth_sources += { '@0@'.format(d) : srcs }
  depth_args += { '@0@'.format(d) : args }
endforeach

# Both linkage forms (yah264 lesson: plain library() made the CLI link
# the dylib and cross a shared-library boundary on every hot call, worth
# 12-14% of wall time). In-tree consumers - the CLI and tests, the
# binaries the boards time - link the STATIC form; the shared form with
# hidden visibility is what gets installed for external callers (the
# ffmpeg wrapper).
#
# The installed pair is this build's own depth. The OTHER depth is built
# static only, because the one thing that links it is n265 in this same
# build directory, and static is what keeps its hot calls direct.
libyah265_both = both_libraries('yah265' + depth_suffix,
  depth_sources['@0@'.format(bit_depth)],
  c_args : depth_args['@0@'.format(bit_depth)],
  include_directories : inc,
  dependencies : [m_dep, threads_dep],
  version : meson.project_version(),
  soversion : '0',
  gnu_symbol_visibility : 'hidden',
  install : true)

libyah265 = libyah265_both.get_shared_lib()
libyah265_static = libyah265_both.get_static_lib()

other_depth = bit_depth == 8 ? 10 : 8
libyah265_other = static_library('yah265-alt@0@'.format(other_depth),
  depth_sources['@0@'.format(other_depth)],
  c_args : depth_args['@0@'.format(other_depth)],
  include_directories : inc,
  dependencies : [m_dep, threads_dep],
  gnu_symbol_visibility : 'hidden',
  install : false)

lib8_static  = is_8bit ? libyah265_static : libyah265_other
lib10_static = is_8bit ? libyah265_other : libyah265_static

# One header, and it is depth-agnostic: N265_BIT_DEPTH defaults to 8
# inside it, so an external consumer that links the 8-bit library needs
# no define. A consumer of the 10-bit library defines it to 10 and the
# header renames its calls to the suffixed symbols. Installed once, by
# the 8-bit build, so the two builds cannot clobber each other.
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

# What tests/ links: this build's depth, with the Main10 namespace force
# included where that depth is 10, so checkasm and the unit tests see the
# names the library actually defines.
yah265_dep = declare_dependency(
  link_with : libyah265_static,
  compile_args : is_8bit ? [] : ['-include', symbols10_h],
  include_directories : inc)

# The ONE command line (item 5a): both libraries linked, the encoder
# picked from the input's bit depth, `--output-depth 10` to put 8-bit
# content through Main10 on purpose. The CLI is compiled at 8 so the
# names the header gives it are the 8-bit library's; the Main10 entry
# points it declares for itself, suffixed.
n265 = executable('n265' + depth_suffix, 'cli/n265.c', 'cli/hdr_dyn.c',
  c_args : ['-UN265_BIT_DEPTH', '-DN265_BIT_DEPTH=8'],
  link_with : [lib8_static, lib10_static],
  include_directories : inc,
  dependencies : [m_dep, threads_dep],
  install : true)

subdir('tests')
