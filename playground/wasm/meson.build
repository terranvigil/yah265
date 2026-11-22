# SPDX-License-Identifier: GPL-2.0-or-later
# The browser playground's module. Emscripten only; the root meson.build
# reaches this instead of tests/.
pg_link = [
  '-sMODULARIZE=1', '-sEXPORT_NAME=createYah265',
  '-sENVIRONMENT=web,worker,node',
  '-sALLOW_MEMORY_GROWTH=1', '-sMAXIMUM_MEMORY=2gb',
  '-sSTACK_SIZE=4mb',
  '-sEXPORTED_FUNCTIONS=_pg_open,_pg_frame,_pg_encode,_pg_flush,_pg_close,_pg_version,_pg_reorder',
  '-sEXPORTED_RUNTIME_METHODS=FS,ccall,cwrap,HEAPU8,UTF8ToString',
]
executable('yah265', 'pg.c',
  c_args : ['-UN265_BIT_DEPTH', '-DN265_BIT_DEPTH=8'],
  link_with : lib8_static,
  include_directories : inc,
  dependencies : m_dep,
  link_args : pg_link)
