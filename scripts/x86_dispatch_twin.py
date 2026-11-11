#!/usr/bin/env python3
# Copyright (c) 2026, the yah265 authors
# SPDX-License-Identifier: GPL-2.0-or-later
"""Add an AVX2 dispatch twin after every NEON dispatch block of one family,
or (--sse42) an SSE4.2 twin after every AVX2 block.

usage: scripts/x86_dispatch_twin.py [--sse42] FAMILY file...
   e.g. scripts/x86_dispatch_twin.py mc src/encoder/mc.c
        scripts/x86_dispatch_twin.py --sse42 mc src/encoder/mc.c

It edits the files in place and adds the avx2.h include beside neon.h's.
Run it once per family; a second run would add a second twin.

A NEON block is

    #if N265_NEON_<FAMILY>
        <lines, calling n265_*_neon(...) under n265_asm_on(...)>
    #endif

and its twin is the same block with N265_NEON_ -> N265_AVX2_,
n265_asm_on -> n265_avx2_on and _neon( -> _avx2(. Lines that name a
NEON-only extension (dotprod) make the script refuse that block.
"""
import re
import sys

args = sys.argv[1:]
sse42 = args[0] == '--sse42'
if sse42:
    args = args[1:]
fam = args[0].upper()
if sse42:
    src_mac, dst_mac = 'N265_AVX2_', 'N265_SSE42_'
    src_on, dst_on = 'n265_avx2_on(', 'n265_sse42_on('
    src_sfx, dst_sfx = '_avx2', '_sse42'
    src_inc, dst_inc = 'encoder/x86/avx2.h', 'encoder/x86/sse42.h'
else:
    src_mac, dst_mac = 'N265_NEON_', 'N265_AVX2_'
    src_on, dst_on = 'n265_asm_on(', 'n265_avx2_on('
    src_sfx, dst_sfx = '_neon', '_avx2'
    src_inc, dst_inc = 'encoder/arm/neon.h', 'encoder/x86/avx2.h'
for p in args[1:]:
    lines = open(p).read().split('\n')
    out = []
    i = 0
    n = 0
    while i < len(lines):
        l = lines[i]
        out.append(l)
        if l.strip() == '#if ' + src_mac + fam:
            blk = [l]
            j = i + 1
            depth = 0
            while True:
                m = lines[j]
                blk.append(m)
                out.append(m)
                if m.startswith('#if'):
                    depth += 1
                if m.startswith('#endif'):
                    if depth == 0:
                        break
                    depth -= 1
                j += 1
            body = '\n'.join(blk)
            if 'DOTPROD' in body or 'n265_cpu_flags' in body:
                sys.exit('%s: block at line %d is NEON-specific' % (p, i + 1))
            twin = body.replace(src_mac + fam, dst_mac + fam)
            twin = twin.replace(src_on, dst_on)
            twin = re.sub(r'(n265_\w+?)' + src_sfx + r'\(',
                          r'\1' + dst_sfx + '(', twin)
            # the NEON block keeps the only copy of its comments
            twin = re.sub(r'\n[ \t]*/\*.*?\*/[ \t]*(?=\n)', '', twin,
                          flags=re.S)
            out.extend(twin.split('\n'))
            n += 1
            i = j + 1
            continue
        i += 1
    s = '\n'.join(out)
    if n and '#include "%s"' % dst_inc not in s:
        s = re.sub(r'^(#include "%s".*)$' % re.escape(src_inc),
                   r'\1\n#include "%s"   /* self-guarded */' % dst_inc,
                   s, count=1, flags=re.M)
        if '#include "%s"' % dst_inc not in s:
            sys.exit('%s: no %s include to put %s beside'
                     % (p, src_inc, dst_inc))
    open(p, 'w').write(s)
    print('%s: %d block(s)' % (p, n))
