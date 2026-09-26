#!/usr/bin/env python3
# Copyright (c) 2026, the yah265 authors
# SPDX-License-Identifier: GPL-2.0-or-later
"""lf_feed10.py - yah264's lf_feed.py, unmodified, for a 10-bit 4:2:2 master.

lf_feed.py is used read-only (enclab PARITY-SPEC). Its scaled path downloads
the VideoToolbox frame as nv12, and a ProRes 422 10-bit master (Nocturne,
Lash of the Whip) decodes to p210 frames, which hwdownload refuses as nv12:
the feeder then writes nothing. This wrapper imports lf_feed.py from its own
path and changes one thing, the download format, to p210le; lf_feed's own
`-pix_fmt yuv420p` then converts to 8-bit 4:2:0. The native-size path never
downloads through the filter and is untouched. Arguments are lf_feed.py's.
"""
import importlib.util, os, sys

SRC = os.environ.get("LF_FEED", os.path.expanduser(
    "~/src/yah264-board4k/scripts/lf_feed.py"))
spec = importlib.util.spec_from_file_location("lf_feed", SRC)
lf = importlib.util.module_from_spec(spec)
spec.loader.exec_module(lf)
_chunk_cmd = lf.chunk_cmd


def chunk_cmd(a, s, n, seek, stats):
    return [x.replace("hwdownload,format=nv12", "hwdownload,format=p210le")
            for x in _chunk_cmd(a, s, n, seek, stats)]


lf.chunk_cmd = chunk_cmd

if __name__ == "__main__":
    lf.main()
