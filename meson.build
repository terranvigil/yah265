project('next265', 'c',
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
  'src/encoder/mv.c',
  'src/encoder/deblock.c',
  'src/encoder/sao.c',
  'src/encoder/headers.c',
  'src/encoder/encoder.c',
]

libnext265 = static_library('next265', lib_sources,
  include_directories : inc,
  dependencies : [m_dep, threads_dep])

next265_dep = declare_dependency(
  link_with : libnext265,
  include_directories : inc)

n265 = executable('n265', 'cli/n265.c',
  dependencies : next265_dep,
  install : true)

subdir('tests')
