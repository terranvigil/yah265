#!/usr/bin/env python3
"""Read N265_CU_TRACE's per-node records and answer step 6a's questions.

The trace (src/encoder/ctu.c, `N265_CU_TRACE`) writes one fixed 72-byte
record per coding-tree node the quadtree visits, preceded by one header
record per frame, in coding order and CTU raster order. Nothing in it is
a decision the encoder took because of the trace: the encode is
byte-identical with the variable set and without it.

What this script adds is the TREE. The records arrive in pre-order DFS
per CTU, so walking them says which nodes are final coding units and
which are search that lost, and every share below is read off that walk:

  by-outcome   where the time went, charged to the final CU whose AREA it
               was spent inside - a discarded 8x8 subtree under a 16x16
               that ends as skip is time the skip cost
  by-size      the same, split by the size of the node the time was in
  predicates   for each candidate early-skip predicate, the share of
               skip-ending CU time it would catch and the share of
               residual-ending CU time it would wrongly catch

Everything streams: one frame's records at a time, counters only, so a
120-frame 1080p trace costs no more memory than a CIF one.

Usage:  cu_census.py trace.bin [trace.bin ...]
"""

import struct
import sys
import os
from collections import defaultdict

REC = "<13IfHHHHHH8B"
SZ = struct.calcsize(REC)
assert SZ == 76, SZ

(CUT_SKIP, CUT_MERGE, CUT_AMVP, CUT_INTRA, CUT_PART, CUT_OUTSIDE) = range(6)
CUT_FRAME = 0xFF
KIND_NAME = {CUT_SKIP: "skip", CUT_MERGE: "merge+res", CUT_AMVP: "amvp",
             CUT_INTRA: "intra", CUT_PART: "rect/amp"}
RESIDUAL = (CUT_MERGE, CUT_AMVP, CUT_INTRA, CUT_PART)

F_SPLIT, F_STATIC, F_GAMVP, F_GINTRA, F_CLEAN, F_NORES, F_FLAT, F_SKIPWIN = (
    1, 2, 4, 8, 16, 32, 64, 128)


def SW(r):
    """the conjunct N265_SKIP_EXIT carries: skip already beat merge"""
    return bool(r[FLAGS] & F_SKIPWIN)

# record field indices, in the order the struct packs them
(CYC, KIDS, CMERGE, CAMVP, CINTRA, CPART, CPRE, SAD, STHR, DSKIP, CTHR,
 ICOST, PCOST, PROP, X, Y, NTM, NTA, NTI, NTP, LOG2N, DEPTH, KIND, FLAGS,
 COLSKIP, QP, SLICE, RSV) = range(28)

# The predicates the census reads. Each is a function of one record; the
# report prints, for each, the share of skip-ending CU TIME it catches
# against the share of residual-ending CU time it wrongly catches.
# Family A, the merge-first exit: asked AFTER the merge and skip arms,
# where `N265_SKIP_EXIT` already stands, so it carries that gate's own
# conjunct (skip has beaten merge on J) and can only save what comes
# after - the AMVP search, the intra arm and the partitions.
PREDICATES = [
    ("SKIP_EXIT=1 (shipped): d_skip*4<=clean",
     lambda r: SW(r) and r[DSKIP] * 4 <= r[CTHR]),
    ("SKIP_EXIT=2: d_skip<=clean",
     lambda r: SW(r) and r[DSKIP] <= r[CTHR]),
    ("SKIP_EXIT=3: skip beat merge, no bound", SW),
    ("skipwin & d_skip*2<=clean",
     lambda r: SW(r) and r[DSKIP] * 2 <= r[CTHR]),
    ("skipwin & d_skip*3<=clean",
     lambda r: SW(r) and r[DSKIP] * 3 <= r[CTHR]),
    ("skipwin & d_skip*4<=3x clean",
     lambda r: SW(r) and r[DSKIP] * 4 <= r[CTHR] * 3),
    ("skipwin & colskip=100",
     lambda r: SW(r) and r[COLSKIP] != 255 and r[COLSKIP] >= 100),
    ("skipwin & prop==0", lambda r: SW(r) and r[PROP] == 0.0),
    ("skipwin & d_skip<=clean & colskip=100",
     lambda r: SW(r) and r[DSKIP] <= r[CTHR] and r[COLSKIP] != 255 and
     r[COLSKIP] >= 100),
    ("skipwin & d_skip<=clean & prop==0",
     lambda r: SW(r) and r[DSKIP] <= r[CTHR] and r[PROP] == 0.0),
    ("skipwin & d_skip*2<=clean & colskip=100",
     lambda r: SW(r) and r[DSKIP] * 2 <= r[CTHR] and r[COLSKIP] != 255 and
     r[COLSKIP] >= 100),
    ("skipwin & d_skip*2<=clean & prop==0",
     lambda r: SW(r) and r[DSKIP] * 2 <= r[CTHR] and r[PROP] == 0.0),
    ("skipwin & d_skip*2<=clean & col & prop",
     lambda r: SW(r) and r[DSKIP] * 2 <= r[CTHR] and r[PROP] == 0.0 and
     r[COLSKIP] != 255 and r[COLSKIP] >= 100),
    ("skipwin & d_skip<=clean & col & prop",
     lambda r: SW(r) and r[DSKIP] <= r[CTHR] and r[PROP] == 0.0 and
     r[COLSKIP] != 255 and r[COLSKIP] >= 100),
    ("skipwin & sad*8<=12x thr",
     lambda r: SW(r) and r[SAD] * 8 <= r[STHR] * 12),
    ("skipwin & sad*8<=16x thr",
     lambda r: SW(r) and r[SAD] * 8 <= r[STHR] * 16),
]

# Family B, the static commit: asked BEFORE the merge arm, where the
# strict static-skip exit already stands, so it saves the WHOLE node -
# and every CU it catches that would not have ended as skip is a changed
# decision, not a lost candidate.
PRED_B = [
    ("ORACLE: the CU ends as skip", lambda r: r[KIND] == CUT_SKIP),
    ("static (shipped): sad*8<=1x thr", lambda r: r[SAD] * 8 <= r[STHR]),
    ("sad*8<=4x thr", lambda r: r[SAD] * 8 <= r[STHR] * 4),
    ("sad*8<=8x thr", lambda r: r[SAD] * 8 <= r[STHR] * 8),
    ("sad*8<=10x thr", lambda r: r[SAD] * 8 <= r[STHR] * 10),
    ("sad*8<=12x thr", lambda r: r[SAD] * 8 <= r[STHR] * 12),
    ("colskip=100 & sad*8<=8x thr",
     lambda r: r[COLSKIP] != 255 and r[COLSKIP] >= 100 and
     r[SAD] * 8 <= r[STHR] * 8),
    ("colskip=100 & sad*8<=10x thr",
     lambda r: r[COLSKIP] != 255 and r[COLSKIP] >= 100 and
     r[SAD] * 8 <= r[STHR] * 10),
    ("colskip=100 & prop==0 & sad<=8x",
     lambda r: r[COLSKIP] != 255 and r[COLSKIP] >= 100 and r[PROP] == 0.0 and
     r[SAD] * 8 <= r[STHR] * 8),
    ("colskip=100 & prop==0 & sad<=10x",
     lambda r: r[COLSKIP] != 255 and r[COLSKIP] >= 100 and r[PROP] == 0.0 and
     r[SAD] * 8 <= r[STHR] * 10),
    ("colskip=100 & prop==0 & sad<=12x",
     lambda r: r[COLSKIP] != 255 and r[COLSKIP] >= 100 and r[PROP] == 0.0 and
     r[SAD] * 8 <= r[STHR] * 12),
    ("prop==0 & sad*8<=8x thr",
     lambda r: r[PROP] == 0.0 and r[SAD] * 8 <= r[STHR] * 8),
    ("prop==0 & sad*8<=10x thr",
     lambda r: r[PROP] == 0.0 and r[SAD] * 8 <= r[STHR] * 10),
]

# cand_sad*8 / static_thr. With N265_SKIP_THR at its default 2 the bound
# is 2*n*n, so the ratio is four times the merge candidate's SAD PER
# PIXEL: 8 on this scale is 2 levels a pixel. The whole population lives
# between 4 and 20, which is why the buckets are that fine there.
SAD_BUCKETS = [(0, 4), (4, 6), (6, 8), (8, 10), (10, 12), (12, 14),
               (14, 16), (16, 20), (20, 32), (32, 1 << 30)]
# d_skip / clean_thr, in hundredths: the forced skip's distortion against
# the quantizer-floor bound SKIP_EXIT is written in
DSK_BUCKETS = [(0, 5), (5, 10), (10, 25), (25, 50), (50, 100), (100, 200),
               (200, 400), (400, 1 << 30)]


def frames(path):
    """Yield (header, [records]) per frame, streaming."""
    with open(path, "rb") as f:
        blob = f.read()
    hdr = None
    cur = []
    for off in range(0, len(blob) - SZ + 1, SZ):
        r = struct.unpack_from(REC, blob, off)
        if r[KIND] == CUT_FRAME:
            if hdr is not None:
                yield hdr, cur
            hdr, cur = r, []
        else:
            cur.append(r)
    if hdr is not None:
        yield hdr, cur


def walk(nodes):
    """(node, final) for each record in a frame's pre-order node list.

    `final` is the FINAL coding unit whose area the node sits inside, or
    None where the node is a split node in the coded tree (its own
    analyze_cu ran and lost to the split) or off the picture."""
    out = []
    N = len(nodes)

    def rec(i, forced):
        n = nodes[i]
        i += 1
        outside = n[KIND] == CUT_OUTSIDE
        split = bool(n[FLAGS] & F_SPLIT)
        final = forced
        if final is None and not split and not outside:
            final = n
        out.append((n, final, outside))
        d, x, y, w = n[DEPTH] + 1, n[X], n[Y], 1 << n[LOG2N]
        while i < N and nodes[i][DEPTH] == d and \
                x <= nodes[i][X] < x + w and y <= nodes[i][Y] < y + w:
            i = rec(i, final)
        return i

    i = 0
    while i < N:
        i = rec(i, None)
    return out


class Census:
    def __init__(self, name):
        self.name = name
        self.frames = 0
        self.tot = 0
        self.cyc_out = defaultdict(int)
        self.cyc_arm = defaultdict(lambda: defaultdict(int))
        self.cyc_size = defaultdict(int)
        self.cyc_out_size = defaultdict(int)
        self.cyc_lost = defaultdict(int)
        self.n_final = defaultdict(int)
        self.area_final = defaultdict(int)
        self.nt = defaultdict(lambda: defaultdict(int))
        # predicate counters, per predicate:
        #  0,1  skip-ending time and count caught
        #  2,3  AMVP/rect-ending time and count caught - the CUs whose
        #       DECISION an early amvp exit would actually change
        #  4    their area
        #  5,6  merge/intra-ending time and count caught: the amvp arm
        #       ran and lost there, so gating it changes no decision and
        #       the time is a clean saving
        #  7    total time saved, everything past the merge arm
        self.pred = [[0] * 8 for _ in PREDICATES]
        # family B: caught skip time/count, caught non-skip time/count/area,
        # and the whole-node time it saves
        self.predb = [[0] * 6 for _ in PRED_B]
        self.sk_t = self.sk_n = self.re_t = self.re_n = 0
        self.ar_res = 0                         # area of residual-ending
        self.ri_t = self.ri_n = self.ri_a = 0   # amvp/rect-ending
        self.nu_t = self.nu_n = 0               # merge/intra-ending
        self.static_t = 0
        self.sad_hist = {k: [0] * len(SAD_BUCKETS)
                         for k in ("skip", "res", "split")}
        self.dsk_hist = {k: [0] * len(DSK_BUCKETS)
                         for k in ("skip", "res", "split")}
        self.gate = defaultdict(lambda: defaultdict(int))
        self.root = [0, 0, 0, 0]    # skip-root n, cyc; all root n, cyc
        self.col_hist = defaultdict(lambda: defaultdict(int))
        self.q = [0, 0, 0, 0]   # all-skip-quad cyc, all root cyc, n, n all-skip
        # coded CU AREA by size and by skip/not - the one table x265's
        # bitstream can be read against
        self.area_sz = defaultdict(int)

    def add(self, hdr, nodes):
        self.frames += 1
        roots = {}
        for n, final, outside in walk(nodes):
            s = max(0, n[CYC] - n[KIDS])
            self.tot += s
            if outside:
                self.cyc_out["boundary"] += s
                continue
            fk = KIND_NAME[final[KIND]] if final is not None else "split node"
            self.cyc_out[fk] += s
            self.cyc_size[n[LOG2N]] += s
            self.cyc_out_size[(fk, n[LOG2N])] += s
            a = self.cyc_arm[fk]
            a["merge"] += n[CMERGE]
            a["amvp"] += n[CAMVP]
            a["intra"] += n[CINTRA]
            a["part"] += n[CPART]
            a["other"] += max(0, s - n[CMERGE] - n[CAMVP] - n[CINTRA] -
                              n[CPART])
            t = self.nt[fk]
            t["merge"] += n[NTM]
            t["amvp"] += n[NTA]
            t["intra"] += n[NTI]
            t["part"] += n[NTP]
            if final is not n:
                self.cyc_lost[fk] += s
            if n[DEPTH] == 0:
                g = roots.setdefault((n[X] >> 6, n[Y] >> 6), [0, 0])
                g[0] += n[CYC]
                g[1] += (n[KIND] == CUT_SKIP and not (n[FLAGS] & F_SPLIT))
            if n[DEPTH] == 0:
                self.root[2] += 1
                self.root[3] += n[CYC]
                if n[KIND] == CUT_SKIP and not (n[FLAGS] & F_SPLIT):
                    self.root[0] += 1
                    self.root[1] += n[CYC]
            if final is n:
                self.n_final[fk] += 1
                self.area_final[fk] += (1 << n[LOG2N]) ** 2
                self.area_sz[(n[LOG2N], n[KIND] == CUT_SKIP)] += \
                    (1 << n[LOG2N]) ** 2
            # The predicate population is every node IN THE CODED TREE - a
            # final CU, or a node the split arm won at - because a gate on
            # the merge arm is asked at every one of them. A final CU's
            # area is its whole subtree (the discarded search under it is
            # what the gate takes away); a split node's is its own time
            # only, since its children still run.
            if n[SLICE] == 2 or (final is not n and final is not None):
                continue    # I slices have no merge arm to decide on
            k = n[KIND] if final is n else None
            area = n[CYC] if final is n else s
            # what an exit AFTER the merge arm actually deletes: the AMVP
            # search, the partitions, and the intra arm only where the
            # intra gate had not already taken it. The commit and the
            # bookkeeping still run, and so does the merge arm the
            # predicate is read off, so neither is in here.
            save = n[CAMVP] + n[CPART] + \
                (0 if n[FLAGS] & F_GINTRA else n[CINTRA])
            if k == CUT_SKIP:
                self.sk_t += area
                self.sk_n += 1
                if n[FLAGS] & F_STATIC:
                    self.static_t += area
                which = "skip"
            elif k in RESIDUAL:
                self.re_t += area
                self.re_n += 1
                self.ar_res += (1 << n[LOG2N]) ** 2
                which = "res"
            else:
                which = "split"
            g = self.gate[which]
            g["n"] += 1
            g["gamvp"] += bool(n[FLAGS] & F_GAMVP)
            g["gintra"] += bool(n[FLAGS] & F_GINTRA)
            g["static"] += bool(n[FLAGS] & F_STATIC)
            if n[STHR]:
                for bi, (lo, hi) in enumerate(SAD_BUCKETS):
                    if lo * n[STHR] <= n[SAD] * 8 < hi * n[STHR]:
                        self.sad_hist[which][bi] += 1
                        break
            if n[CTHR]:
                for bi, (lo, hi) in enumerate(DSK_BUCKETS):
                    if lo * n[CTHR] <= n[DSKIP] * 100 < hi * n[CTHR]:
                        self.dsk_hist[which][bi] += 1
                        break
            self.col_hist[which]["none" if n[COLSKIP] == 255 else
                                 "100" if n[COLSKIP] >= 100 else
                                 "90" if n[COLSKIP] >= 90 else
                                 "50" if n[COLSKIP] >= 50 else "lo"] += 1
            self.col_hist[which]["prop0"] += (n[PROP] == 0.0)
            risk = k in (CUT_AMVP, CUT_PART)
            if k != CUT_SKIP:
                if risk:
                    self.ri_t += area
                    self.ri_n += 1
                    self.ri_a += (1 << n[LOG2N]) ** 2
                else:
                    self.nu_t += area
                    self.nu_n += 1
            for pi, (_, f) in enumerate(PRED_B):
                if f(n):
                    b = self.predb[pi]
                    b[5] += max(0, area - n[CPRE])
                    if k == CUT_SKIP:
                        b[0] += area
                        b[1] += 1
                    else:
                        b[2] += area
                        b[3] += 1
                        b[4] += (1 << n[LOG2N]) ** 2 if k is not None else 0
            for pi, (_, f) in enumerate(PREDICATES):
                if f(n):
                    c = self.pred[pi]
                    c[7] += save
                    if k == CUT_SKIP:
                        c[0] += area
                        c[1] += 1
                    elif risk:
                        c[2] += area
                        c[3] += 1
                        c[4] += (1 << n[LOG2N]) ** 2
                    else:
                        c[5] += area
                        c[6] += 1
        for g in roots.values():
            self.q[1] += g[0]
            self.q[2] += 1
            if g[1] == 4:
                self.q[0] += g[0]
                self.q[3] += 1


def pct(a, b):
    return 100.0 * a / b if b else 0.0


ORDER = ("skip", "merge+res", "amvp", "intra", "rect/amp", "split node",
         "boundary")


def report(c):
    print(f"\n================ {c.name}  ({c.frames} frames, "
          f"{c.tot / 1e6:.1f} Mticks) ================")
    print("\n-- time by FINAL outcome of the CU whose area it was spent in --")
    print(f"{'outcome':<11} {'time%':>7} {'CUs':>9} {'area%':>7} | "
          f"{'merge':>6} {'amvp':>6} {'intra':>6} {'part':>6} {'other':>6} | "
          f"{'lost%':>6} {'TBmerge':>9} {'TBamvp':>9} {'TBintra':>9} "
          f"{'TBpart':>9}")
    ta = sum(c.area_final.values()) or 1
    for k in ORDER:
        t = c.cyc_out.get(k, 0)
        if not t:
            continue
        a = c.cyc_arm[k]
        s = sum(a.values()) or 1
        n = c.nt[k]
        print(f"{k:<11} {pct(t, c.tot):7.2f} {c.n_final.get(k, 0):9d} "
              f"{pct(c.area_final.get(k, 0), ta):7.2f} | "
              f"{pct(a['merge'], s):6.1f} {pct(a['amvp'], s):6.1f} "
              f"{pct(a['intra'], s):6.1f} {pct(a['part'], s):6.1f} "
              f"{pct(a['other'], s):6.1f} | {pct(c.cyc_lost.get(k, 0), t):6.1f} "
              f"{n['merge']:9d} {n['amvp']:9d} {n['intra']:9d} {n['part']:9d}")

    print("\n-- time by NODE SIZE (rows) x final outcome (columns), % of all --")
    cols = ("skip", "merge+res", "amvp", "intra", "rect/amp", "split node")
    print(f"{'node':<9} {'all':>7} " + " ".join(f"{k:>10}" for k in cols))
    for lg in sorted(c.cyc_size):
        row = " ".join(f"{pct(c.cyc_out_size.get((k, lg), 0), c.tot):10.2f}"
                       for k in cols)
        print(f"{1 << lg:>3}x{1 << lg:<5} {pct(c.cyc_size[lg], c.tot):7.2f} "
              + row)

    print("\n-- coded CU AREA by size, share of the coded picture --")
    ta2 = sum(c.area_sz.values()) or 1
    print(f"{'size':<8} {'all%':>8} {'skip%':>8} {'coded%':>8}")
    for lg in sorted({k[0] for k in c.area_sz}):
        a0 = c.area_sz.get((lg, True), 0)
        a1 = c.area_sz.get((lg, False), 0)
        print(f"{1 << lg:>3}x{1 << lg:<4} {pct(a0 + a1, ta2):8.2f} "
              f"{pct(a0, ta2):8.2f} {pct(a1, ta2):8.2f}")
    print(f"{'total':<8} {100.0:8.2f} "
          f"{pct(sum(v for k, v in c.area_sz.items() if k[1]), ta2):8.2f} "
          f"{pct(sum(v for k, v in c.area_sz.items() if not k[1]), ta2):8.2f}")

    print("\n-- the root reading --")
    print(f"   32x32 CTU roots that end as ONE skip CU: {c.root[0]} / "
          f"{c.root[2]} = {pct(c.root[0], c.root[2]):.1f}% of roots, "
          f"{pct(c.root[1], c.root[3]):.1f}% of all root-subtree time")
    print(f"   aligned 2x2 CTU quads whose four roots ALL end as skip: "
          f"{c.q[3]} / {c.q[2]} = {pct(c.q[3], c.q[2]):.1f}%; their share of "
          f"all root-subtree time {pct(c.q[0], c.q[1]):.1f}%")


def predicates(c):
    all_t = c.sk_t + c.re_t or 1
    print(f"\n-- predicates: {c.sk_n} skip-ending CUs "
          f"({pct(c.sk_t, all_t):.1f}% of CU time, of which "
          f"{pct(c.static_t, c.sk_t or 1):.1f}% already taken by the strict "
          f"static exit), {c.re_n} residual-ending ({pct(c.re_t, all_t):.1f}%) --")
    print(f"   of the residual-ending CUs, {pct(c.ri_t, c.re_t or 1):.1f}% of "
          f"the time and {c.ri_n} CUs end AMVP or rect/amp - the only ones "
          f"whose decision an amvp exit changes")
    print(f"\n{'predicate':<34} {'catch%t':>8} {'catch%n':>8} | "
          f"{'wrong%t':>8} {'wrong%n':>8} {'wrong%a':>8} | {'free%t':>7} "
          f"{'saves%':>7}")
    for (nm, _), p in zip(PREDICATES, c.pred):
        print(f"{nm:<34} {pct(p[0], c.sk_t or 1):8.1f} "
              f"{pct(p[1], c.sk_n or 1):8.1f} | {pct(p[2], c.ri_t or 1):8.1f} "
              f"{pct(p[3], c.ri_n or 1):8.1f} {pct(p[4], c.ri_a or 1):8.1f} | "
              f"{pct(p[5], c.nu_t or 1):7.1f} {pct(p[7], c.tot or 1):7.2f}")

    print(f"\n{'family B: static commit before the merge arm':<34} "
          f"{'catch%t':>8} {'catch%n':>8} | {'wrong%t':>8} {'wrong%n':>8} "
          f"{'wrong%a':>8} | {'saves%':>7}")
    for (nm, _), p in zip(PRED_B, c.predb):
        print(f"{nm:<34} {pct(p[0], c.sk_t or 1):8.1f} "
              f"{pct(p[1], c.sk_n or 1):8.1f} | {pct(p[2], c.re_t or 1):8.1f} "
              f"{pct(p[3], c.re_n or 1):8.1f} {pct(p[4], c.ar_res or 1):8.1f} | "
              f"{pct(p[5], c.tot or 1):7.2f}")

    print("\n-- cand_sad*8 / static_thr, share of CUs by final outcome --")
    hdr = " ".join(("%d-%s" % (lo, "inf" if hi > 1 << 29 else hi)).rjust(8)
                   for lo, hi in SAD_BUCKETS)
    print(f"{'':<16} {hdr}")
    for nm, key, tot in (("skip-ending", "skip", c.sk_n),
                         ("residual-ending", "res", c.re_n)):
        print(f"{nm:<16} " + " ".join(f"{pct(v, tot or 1):8.1f}"
                                      for v in c.sad_hist[key]))
    print("\n-- d_skip / clean_thr in hundredths, share of CUs by outcome --")
    hdr = " ".join(("%d-%s" % (lo, "inf" if hi > 1 << 29 else hi)).rjust(8)
                   for lo, hi in DSK_BUCKETS)
    print(f"{'':<16} {hdr}")
    for nm, key, tot in (("skip-ending", "skip", c.sk_n),
                         ("residual-ending", "res", c.re_n)):
        print(f"{nm:<16} " + " ".join(f"{pct(v, tot or 1):8.1f}"
                                      for v in c.dsk_hist[key]))
    print("\n-- what the economy gates already did to these CUs (share) --")
    print(f"{'':<16} {'gate_amvp':>10} {'gate_intra':>11} {'static exit':>12}")
    for nm, key in (("skip-ending", "skip"), ("residual-ending", "res")):
        g = c.gate[key]
        print(f"{nm:<16} {pct(g['gamvp'], g['n'] or 1):10.1f} "
              f"{pct(g['gintra'], g['n'] or 1):11.1f} "
              f"{pct(g['static'], g['n'] or 1):12.1f}")
    print("\n-- co-located skip of the previous frame in coding order, and "
          "the cu-tree propagate --")
    print(f"{'':<16} {'col=100%':>9} {'col>=90%':>9} {'col>=50%':>9} "
          f"{'col<50%':>9} {'no prev':>9} {'prop==0':>9}")
    for nm, key, tot in (("skip-ending", "skip", c.sk_n),
                         ("residual-ending", "res", c.re_n)):
        h = c.col_hist[key]
        print(f"{nm:<16} " + " ".join(
            f"{pct(h[k], tot or 1):9.1f}"
            for k in ("100", "90", "50", "lo", "none", "prop0")))


def main():
    args = sys.argv[1:]
    if not args:
        sys.exit(__doc__)
    for path in args:
        c = Census(os.path.basename(path).replace(".bin", ""))
        maxf = int(os.environ.get("MAXF", "0"))
        for hdr, nodes in frames(path):
            if maxf and c.frames >= maxf:
                break
            c.add(hdr, nodes)
        report(c)
        predicates(c)


if __name__ == "__main__":
    main()
