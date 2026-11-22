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
  'src/encoder/config.c',
  'src/encoder/encoder.c',
  'src/encoder/ttrace.c',
  'src/hw/vt_map.c',
  'src/hw/sc.c',
]

# The VideoToolbox hardware mode (docs/hardware-mode.md); the stub
# elsewhere.
vt_opt = get_option('videotoolbox')
vt_dep = dependency('', required : false)
if host_machine.system() == 'darwin' and not vt_opt.disabled()
  vt_dep = dependency('appleframeworks',
                      modules : ['VideoToolbox', 'CoreMedia', 'CoreVideo',
                                 'CoreFoundation'],
                      required : vt_opt)
elif vt_opt.enabled()
  error('-Dvideotoolbox=enabled needs macOS')
endif
lib_sources += vt_dep.found() ? 'src/hw/vt.c' : 'src/hw/vt_stub.c'

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
fs = import('fs')
all_families = []
families_8 = []
families_10 = []
if cpu_family == 'aarch64'
  all_families = ['transform', 'satd', 'mc', 'rdoq', 'cost', 'intra', 'deblock', 'sao']
  families_8 = ['transform', 'satd', 'mc', 'rdoq', 'cost', 'intra', 'deblock', 'sao']
  families_10 = ['transform', 'satd', 'rdoq', 'cost', 'intra', 'mc', 'deblock', 'sao']  # grown one proved family at a time
  add_project_arguments('-DN265_NEON=1', language : 'c')
endif
neon_families = is_8bit ? families_8 : families_10

# The SVE tier (docs/sve.md): src/encoder/arm/<family>_sve.c, built with
# +sve into its own archive like the x86 tiers. A family is in at both
# depths once its file exists, since every file holds both depths' lanes.
sve_all_families = []
sve_families_8 = []
sve_families_10 = []
sve_args = ['-march=armv8.2-a+dotprod+sve']
sve_opt = get_option('sve')
if cpu_family == 'aarch64' and not sve_opt.disabled()
  sve_ok = cc.compiles('''#include <arm_sve.h>
      svint64_t f(svint16_t a, svint64_t acc) { return svdot(acc, a, a); }''',
    args : sve_args, name : 'SVE intrinsics')
  if sve_opt.enabled() and not sve_ok
    error('-Dsve=enabled, but the compiler lacks the SVE intrinsics')
  endif
  if sve_ok and (sve_opt.enabled() or host_machine.system() == 'linux')
    sve_all_families = ['satd', 'mc']
    foreach f : sve_all_families
      if fs.exists('src/encoder/arm/' + f + '_sve.c')
        sve_families_8 += f
        sve_families_10 += f
      endif
    endforeach
  endif
endif
sve_families = is_8bit ? sve_families_8 : sve_families_10
foreach f : sve_families
  add_project_arguments('-DN265_SVE_' + f.to_upper() + '=1', language : 'c')
endforeach

# The x86-64 AVX2 tier (docs/x86-plan.md). AVX2 is not baseline x86-64,
# so the family files compile into their own static libraries with
# -mavx2 -mfma -mbmi2 and fold into the encoder libraries as objects;
# every other file stays baseline and reaches a kernel only through a
# CPUID-checked dispatch site. The family names and the per-family macro
# (N265_AVX2_<FAMILY>) mirror the NEON lists above. A family is IN at 8
# bits when its file src/encoder/x86/<family>_avx2.c exists, so a family
# item adds its file and its dispatch sites and touches nothing here; the
# file lands only once every neon.h entry point of the family has an
# _avx2 twin that checkasm holds bit-exact. The Main10 list is written
# out, for the NEON list's reason: a family joins it once its 16-bit
# lanes pass a checkasm built at 10 bits.
x86_all_families = []
x86_families_8 = []
x86_families_10 = []
x86_avx2_args = ['-mavx2', '-mfma', '-mbmi2']
# The SSE4.2 tier, for x86-64 CPUs without AVX2: the same rule, with
# src/encoder/x86/<family>_sse42.c built with -msse4.2.
x86_sse42_families_8 = []
x86_sse42_families_10 = []
x86_sse42_args = ['-msse4.2']
# The AVX-512 tier (F, BW, VL on top of the AVX2 flags): the same rule,
# with src/encoder/x86/<family>_avx512.c.
x86_avx512_families_8 = []
x86_avx512_families_10 = []
x86_avx512_args = x86_avx2_args + ['-mavx512f', '-mavx512bw', '-mavx512vl']
if cpu_family == 'x86_64'
  x86_all_families = ['transform', 'satd', 'mc', 'rdoq', 'cost', 'intra', 'deblock', 'sao']
  foreach f : x86_all_families
    if fs.exists('src/encoder/x86/' + f + '_avx2.c')
      x86_families_8 += f
    endif
    if fs.exists('src/encoder/x86/' + f + '_sse42.c')
      x86_sse42_families_8 += f
    endif
    if fs.exists('src/encoder/x86/' + f + '_avx512.c')
      x86_avx512_families_8 += f
    endif
  endforeach
  foreach f : ['transform', 'satd', 'mc', 'intra', 'rdoq', 'cost', 'deblock', 'sao']  # grown one proved family at a time
    if f in x86_families_8
      x86_families_10 += f
    endif
  endforeach
  foreach f : ['transform', 'satd', 'mc', 'intra', 'rdoq', 'cost', 'deblock', 'sao']  # the SSE4.2 Main10 list
    if f in x86_sse42_families_8
      x86_sse42_families_10 += f
    endif
  endforeach
  foreach f : ['satd', 'mc', 'transform', 'rdoq', 'cost', 'sao']  # the AVX-512 Main10 list
    if f in x86_avx512_families_8
      x86_avx512_families_10 += f
    endif
  endforeach
  add_project_arguments('-DN265_X86=1', language : 'c')
endif
avx2_families = is_8bit ? x86_families_8 : x86_families_10
foreach f : avx2_families
  add_project_arguments('-DN265_AVX2_' + f.to_upper() + '=1', language : 'c')
endforeach
sse42_families = is_8bit ? x86_sse42_families_8 : x86_sse42_families_10
foreach f : sse42_families
  add_project_arguments('-DN265_SSE42_' + f.to_upper() + '=1', language : 'c')
endforeach
avx512_families = is_8bit ? x86_avx512_families_8 : x86_avx512_families_10
foreach f : avx512_families
  add_project_arguments('-DN265_AVX512_' + f.to_upper() + '=1', language : 'c')
endforeach

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
  foreach f : sve_all_families
    args += '-UN265_SVE_' + f.to_upper()
    if f in (d == 8 ? sve_families_8 : sve_families_10)
      args += '-DN265_SVE_' + f.to_upper() + '=1'
    endif
  endforeach
  foreach f : x86_all_families
    args += '-UN265_AVX2_' + f.to_upper()
    if f in (d == 8 ? x86_families_8 : x86_families_10)
      args += '-DN265_AVX2_' + f.to_upper() + '=1'
    endif
    args += '-UN265_SSE42_' + f.to_upper()
    if f in (d == 8 ? x86_sse42_families_8 : x86_sse42_families_10)
      args += '-DN265_SSE42_' + f.to_upper() + '=1'
    endif
    args += '-UN265_AVX512_' + f.to_upper()
    if f in (d == 8 ? x86_avx512_families_8 : x86_avx512_families_10)
      args += '-DN265_AVX512_' + f.to_upper() + '=1'
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

# The AVX2, SSE4.2, AVX-512 and SVE objects, one archive per tier and depth, each
# folded into its depth's library. The 10-bit ones are compiled with that
# library's arguments, so symbols10.h renames their kernels too. The archive names stay off the
# libyah265* glob scripts/symbol_check.sh compares.
depth_objects = { '8' : [], '10' : [] }
foreach d : [8, 10]
  objs = []
  fams = d == 8 ? x86_families_8 : x86_families_10
  if fams.length() > 0
    avx2_srcs = []
    foreach f : fams
      avx2_srcs += 'src/encoder/x86/' + f + '_avx2.c'
    endforeach
    avx2_lib = static_library('n265avx2-@0@'.format(d), avx2_srcs,
      c_args : depth_args['@0@'.format(d)] + x86_avx2_args,
      include_directories : inc,
      install : false)
    objs += avx2_lib.extract_all_objects(recursive : false)
  endif
  fams = d == 8 ? x86_sse42_families_8 : x86_sse42_families_10
  if fams.length() > 0
    sse42_srcs = []
    foreach f : fams
      sse42_srcs += 'src/encoder/x86/' + f + '_sse42.c'
    endforeach
    sse42_lib = static_library('n265sse42-@0@'.format(d), sse42_srcs,
      c_args : depth_args['@0@'.format(d)] + x86_sse42_args,
      include_directories : inc,
      install : false)
    objs += sse42_lib.extract_all_objects(recursive : false)
  endif
  fams = d == 8 ? x86_avx512_families_8 : x86_avx512_families_10
  if fams.length() > 0
    avx512_srcs = []
    foreach f : fams
      avx512_srcs += 'src/encoder/x86/' + f + '_avx512.c'
    endforeach
    avx512_lib = static_library('n265avx512-@0@'.format(d), avx512_srcs,
      c_args : depth_args['@0@'.format(d)] + x86_avx512_args,
      include_directories : inc,
      install : false)
    objs += avx512_lib.extract_all_objects(recursive : false)
  endif
  fams = d == 8 ? sve_families_8 : sve_families_10
  if fams.length() > 0
    sve_srcs = []
    foreach f : fams
      sve_srcs += 'src/encoder/arm/' + f + '_sve.c'
    endforeach
    sve_lib = static_library('n265sve-@0@'.format(d), sve_srcs,
      c_args : depth_args['@0@'.format(d)] + sve_args,
      include_directories : inc,
      install : false)
    objs += sve_lib.extract_all_objects(recursive : false)
  endif
  depth_objects += { '@0@'.format(d) : objs }
endforeach

# Both linkage forms (yah264 lesson: plain library() made the CLI link
# the dylib and cross a shared-library boundary on every hot call, worth
# 12-14% of wall time). In-tree consumers - the CLI and tests, the
# binaries the boards time - link the STATIC form; the shared form with
# hidden visibility is what gets installed for external callers (the
# ffmpeg wrapper).
#
# The static archive is this build's own depth. The OTHER depth is built
# static only for n265 in this same build directory. The 8-bit build's
# SHARED library carries both depths, so one installed libyah265 serves
# yuv420p and yuv420p10 alike: the Main10 entry points are the _10 names,
# which is what lets the two share one object.
libyah265_static = static_library('yah265' + depth_suffix,
  depth_sources['@0@'.format(bit_depth)],
  c_args : depth_args['@0@'.format(bit_depth)],
  objects : depth_objects['@0@'.format(bit_depth)],
  include_directories : inc,
  dependencies : [m_dep, threads_dep, vt_dep],
  gnu_symbol_visibility : 'hidden',
  install : true)

other_depth = bit_depth == 8 ? 10 : 8
libyah265_other = static_library('yah265-alt@0@'.format(other_depth),
  depth_sources['@0@'.format(other_depth)],
  c_args : depth_args['@0@'.format(other_depth)],
  objects : depth_objects['@0@'.format(other_depth)],
  include_directories : inc,
  dependencies : [m_dep, threads_dep, vt_dep],
  gnu_symbol_visibility : 'hidden',
  install : false)

shared_objs = [libyah265_static.extract_all_objects(recursive : true)]
if is_8bit
  shared_objs += libyah265_other.extract_all_objects(recursive : true)
endif
libyah265 = shared_library('yah265' + depth_suffix,
  objects : shared_objs,
  dependencies : [m_dep, threads_dep, vt_dep],
  version : meson.project_version(),
  soversion : '0',
  install : true)

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
  dependencies : vt_dep,
  compile_args : is_8bit ? [] : ['-include', symbols10_h],
  include_directories : inc)

# The ONE command line (item 5a): both libraries linked, the encoder
# picked from the input's bit depth, `--output-depth 10` to put 8-bit
# content through Main10 on purpose. The CLI is compiled at 8 so the
# names the header gives it are the 8-bit library's; the Main10 entry
# points it declares for itself, suffixed.
y265 = executable('y265' + depth_suffix, 'cli/y265.c', 'cli/hdr_dyn.c',
  c_args : ['-UN265_BIT_DEPTH', '-DN265_BIT_DEPTH=8'],
  link_with : [lib8_static, lib10_static],
  include_directories : inc,
  dependencies : [m_dep, threads_dep, vt_dep],
  install : true)

# n265 was the command line's name before it was y265. The build tree
# keeps it as a link so the gates, the board and the harnesses still run.
n265 = custom_target('n265-alias',
  input : y265,
  output : 'n265' + depth_suffix,
  command : ['ln', '-sf', 'y265' + depth_suffix, '@OUTPUT@'],
  build_by_default : true)

subdir('tests')
