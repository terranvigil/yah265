#!/usr/bin/env python3
# Copyright (c) 2026, the yah265 authors
# SPDX-License-Identifier: GPL-2.0-or-later
"""Add an AVX2 dispatch twin after every NEON dispatch block of one family.

usage: scripts/x86_dispatch_twin.py FAMILY file...
   e.g. scripts/x86_dispatch_twin.py mc src/encoder/mc.c

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

fam = sys.argv[1].upper()
for p in sys.argv[2:]:
    lines = open(p).read().split('\n')
    out = []
    i = 0
    n = 0
    while i < len(lines):
        l = lines[i]
        out.append(l)
        if l.strip() == '#if N265_NEON_' + fam:
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
            twin = body.replace('N265_NEON_' + fam, 'N265_AVX2_' + fam)
            twin = twin.replace('n265_asm_on(', 'n265_avx2_on(')
            twin = re.sub(r'(n265_\w+?)_neon\(', r'\1_avx2(', twin)
            # the NEON block keeps the only copy of its comments
            twin = re.sub(r'\n[ \t]*/\*.*?\*/[ \t]*(?=\n)', '', twin,
                          flags=re.S)
            out.extend(twin.split('\n'))
            n += 1
            i = j + 1
            continue
        i += 1
    s = '\n'.join(out)
    if n and '#include "encoder/x86/avx2.h"' not in s:
        s = re.sub(r'^(#include "encoder/arm/neon.h".*)$',
                   r'\1\n#include "encoder/x86/avx2.h"   /* self-guarded */',
                   s, count=1, flags=re.M)
        if '#include "encoder/x86/avx2.h"' not in s:
            sys.exit('%s: no neon.h include to put avx2.h beside' % p)
    open(p, 'w').write(s)
    print('%s: %d block(s)' % (p, n))
