#!/usr/bin/env bash
# Copyright (c) 2026, the yah265 authors
# SPDX-License-Identifier: GPL-2.0-or-later
#
# Builds the playground's VMAF v1 module: Netflix's libvmaf v3.2.1 under
# Emscripten (C only, no threads), the wrapper in vmaf_pg.c, and the
# vmaf_v1.0.16 3d0h model embedded. Writes playground/vmaf.js and
# playground/vmaf.wasm. `make wasm` runs it with emcc on PATH.
#
#   playground/vmaf/build.sh BUILD_DIR
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/../.." && pwd)
B=$(mkdir -p "$1" && cd "$1" && pwd)
TAG=v3.2.1
SRC=$B/vmaf-$TAG

if [ ! -d "$SRC" ]; then
    git clone -q --depth 1 --branch "$TAG" https://github.com/Netflix/vmaf.git "$SRC.tmp"
    # libsvm's own swap() template is ambiguous with std::swap under
    # Emscripten's libc++, so it gets its own name
    perl -pi -e 's/(?<![\w:.>])swap\(/svm_swap(/g' "$SRC.tmp/libvmaf/src/svm.cpp"
    mv "$SRC.tmp" "$SRC"
fi

# the encoder's cross file plus a C++ compiler for libsvm
sed "s/^c = 'emcc'/c = 'emcc'\ncpp = 'em++'/" "$ROOT/playground/wasm/emscripten.cross" > "$B/vmaf.cross"
if [ ! -d "$B/libvmaf" ]; then
    meson setup "$B/libvmaf" "$SRC/libvmaf" --cross-file "$B/vmaf.cross" \
        -Dbuildtype=release -Ddefault_library=static \
        -Denable_tests=false -Denable_docs=false -Denable_tools=false \
        -Denable_asm=false -Denable_float=false -Dbuilt_in_models=false \
        -Dc_args="-O3 -D_GNU_SOURCE" -Dcpp_args="-O3 -D_GNU_SOURCE"
fi
ninja -C "$B/libvmaf"

cp "$SRC/model/vmaf_v1.0.16/vmaf_v1.0.16_3d0h.json" "$B/v1.json"
emcc -O3 -c "$HERE/vmaf_pg.c" -I"$SRC/libvmaf/include" -I"$B/libvmaf/include" \
    -o "$B/vmaf_pg.o"
(cd "$B" && em++ -O3 vmaf_pg.o libvmaf/src/libvmaf.a -o vmaf.js \
    -sMODULARIZE=1 -sEXPORT_NAME=createVmaf -sENVIRONMENT=web,worker,node \
    -sALLOW_MEMORY_GROWTH=1 -sMAXIMUM_MEMORY=2gb -sSTACK_SIZE=4mb \
    -sEXPORTED_FUNCTIONS=_vw_open,_vw_ref,_vw_dis,_vw_push,_vw_score,_vw_close \
    -sEXPORTED_RUNTIME_METHODS=cwrap,HEAPU8 \
    --embed-file v1.json@/v1.json)
cp "$B/vmaf.js" "$B/vmaf.wasm" "$ROOT/playground/"
