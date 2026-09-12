#!/usr/bin/env python3
"""Fit the learned depth-bound table and print its coverage curve.

Phase 7 item 4, step 0 (docs/phase7-depth-design.md). Reads the per-CTU
records N265_DEPTH_DUMP writes, quantises the pre-decision features into a
small grid, stores per cell the depth interval that covers a target share
of the training CTUs, and reports - on content HELD OUT BY CLIP - what
fraction of CTUs that interval actually contains and how many of the four
depth levels it leaves to search.

Nothing here reads a decided depth: every feature comes off the
lookahead's lowres maps, the frame's source luma or the AQ field. The
target, [dmin, dmax], is what the unbounded encoder used, which is the
only column that needs the encode.

numpy only; scipy and sklearn are not on the box.

Usage:
    python3 scripts/depth_fit.py --dumps DIR
    python3 scripts/depth_fit.py --dumps DIR --export src/encoder/depth_table.h

Dump file names are CLIP.CRF.csv, which is how a row is attributed to its
clip; the split is by clip and never by frame range.
"""

import argparse
import glob
import os
import re
import sys

import numpy as np

# ---------------------------------------------------------------- columns

COLS = ("disp slice bdepth nonref flat cx cy qp ctqp lvar fine "
        "imean imax pmean pmax ratio mvmean mvspread dmin dmax").split()
C = {n: i for i, n in enumerate(COLS)}
MAXD = 3                # depths 0..3 at CTB 64 / min CB 8

# The two clip GROUPS the split-by-clip runs between. The CIF band is the
# corpus the quality bar is read on and is the test set the design doc
# names; the extras are fetch_corpus.sh --full's 720p/1080p clips and the
# animation excerpts. ms_cif_30 is a concatenation of five BAND clips, so
# it sits with the band however synthetic it is - putting it in training
# would leak the test set by frame range, which is the one thing the
# design doc forbids. ms_720p_50 is built from four extras and sits there.
BAND = ("akiyo_cif foreman_cif mobile_cif stefan_cif bus_cif tempete_cif "
        "coastguard_cif ms_cif_30").split()
EXTRAS = ("park_joy_720p ducks_720p in_to_tree_720p sintel_720p "
          "sita_flat_720p sita_toon_720p crowd_run_1080p touchdown_1080p "
          "ms_720p_50").split()

# ------------------------------------------------------------------- grid
#
# Chosen from the data (see the "Step 0 result" section of
# docs/phase7-depth-design.md for the marginals it was chosen against).
# The design doc's opening suggestion was ratio x MV spread x fine x slice
# type x QP band; `fine` and the lowres INTER cost turned out to separate
# the outcome far harder than the ratio does, so the ratio keeps three
# coarse bins and `pmean` joins the grid. Every axis is a quantity that
# exists before the CTU is analysed.
#
# Each entry is (feature, edges); np.digitize puts a value into
# len(edges) + 1 bins.

GRID = [
    ("slice",    [0.5, 1.5]),                 # B / P / I
    ("qp",       [23.5, 28.5, 33.5]),         # QP band
    ("fine",     [2.0, 4.0, 6.0, 8.0]),       # log2(fine + 1)
    ("pmean",    [150.0, 400.0, 900.0, 2000.0]),   # lowres inter cost
    ("ratio",    [0.35, 0.75]),               # sum(pcost) / sum(icost)
    ("mvspread", [0.5, 6.0]),                 # lowres |MV| max - min
]

# A deliberately coarse alternative (--grid coarse). The fine grid leaves
# a third of the band's held-out CTUs in a cell the band's own 84k rows
# cannot fill, and a table that falls back to the whole tree is not a
# table under test; this one has 15x fewer cells and fills all of them,
# which is what separates "the grid is too fine" from "the features do
# not predict the depth".
GRID_COARSE = [
    ("slice",    [1.5]),                      # inter / intra
    ("fine",     [3.0, 6.0]),
    ("pmean",    [200.0, 900.0]),
    ("ratio",    [0.5]),
]

N_MIN = 40              # a cell fitted on fewer rows than this is not fitted


def grid_shape():
    return tuple(len(e) + 1 for _, e in GRID)


def cell_index(a):
    """Flat cell index per row."""
    shape = grid_shape()
    idx = np.zeros(len(a), dtype=np.int64)
    for k, (name, edges) in enumerate(GRID):
        b = np.digitize(a[:, C[name]], edges)
        idx = idx * shape[k] + b
    return idx


# -------------------------------------------------------------- load/fit

def load(dumps):
    """clip -> stacked record array. Caches each CSV as .npy beside it."""
    out = {}
    for f in sorted(glob.glob(os.path.join(dumps, "*.csv"))):
        base = os.path.basename(f)[:-4]
        m = re.match(r"^(.*)\.(\d+)$", base)
        if not m:
            print("skip (name is not CLIP.CRF): %s" % f, file=sys.stderr)
            continue
        clip = m.group(1)
        cache = f[:-4] + ".npy"
        if os.path.exists(cache) and os.path.getmtime(cache) > os.path.getmtime(f):
            a = np.load(cache)
        else:
            a = np.loadtxt(f, delimiter=",", usecols=range(1, 21),
                           dtype=np.float32)
            np.save(cache, a)
        out.setdefault(clip, []).append(a)
    return {k: np.concatenate(v) for k, v in out.items()}


def fit(a, target=0.99):
    """Per cell, the depth interval covering `target` of its rows.

    lo is the largest L with P(dmin < L) <= (1 - target) / 2 and hi the
    smallest H with P(dmax > H) <= (1 - target) / 2 - the [p1, p99] the
    design doc asks for, computed off the counts rather than through a
    percentile so the answer is exact on four integer levels.

    Returns (lo, hi, n) flat arrays over the grid. A cell with fewer than
    N_MIN rows is left at the whole tree.
    """
    ncell = int(np.prod(grid_shape()))
    tail = (1.0 - target) / 2.0
    idx = cell_index(a)
    dmin = a[:, C["dmin"]].astype(np.int64)
    dmax = a[:, C["dmax"]].astype(np.int64)
    n = np.bincount(idx, minlength=ncell)
    lo = np.zeros(ncell, dtype=np.int8)
    hi = np.full(ncell, MAXD, dtype=np.int8)
    # counts of dmin == d and dmax == d per cell, in one pass per level
    cmin = np.stack([np.bincount(idx[dmin == d], minlength=ncell)
                     for d in range(MAXD + 1)])
    cmax = np.stack([np.bincount(idx[dmax == d], minlength=ncell)
                     for d in range(MAXD + 1)])
    live = n >= N_MIN
    below = np.zeros(ncell)                      # P(dmin < L)
    for L in range(1, MAXD + 1):
        below = below + cmin[L - 1]
        take = live & (below <= tail * n)
        lo[take] = L
    above = np.zeros(ncell)                      # P(dmax > H)
    for H in range(MAXD - 1, -1, -1):
        above = above + cmax[H + 1]
        take = live & (above <= tail * n)
        hi[take] = H
    hi = np.maximum(hi, lo)
    return lo, hi, n


def evaluate(lo, hi, ntrain, a, margin):
    """Coverage and mean searched width of the fitted table on `a`."""
    idx = cell_index(a)
    dmin = a[:, C["dmin"]]
    dmax = a[:, C["dmax"]]
    pl = np.maximum(lo[idx].astype(np.int32) - margin, 0)
    ph = np.minimum(hi[idx].astype(np.int32) + margin, MAXD)
    ph = np.maximum(ph, pl)
    cov = float(np.mean((pl <= dmin) & (ph >= dmax)))
    width = float(np.mean(ph - pl + 1))
    miss_lo = float(np.mean(pl > dmin))
    miss_hi = float(np.mean(ph < dmax))
    fallback = float(np.mean(ntrain[idx] < N_MIN))
    return cov, width, miss_lo, miss_hi, fallback


# ------------------------------------------------------------- reporting

def oracle_width(a):
    return float(np.mean(a[:, C["dmax"]] - a[:, C["dmin"]] + 1))


def curve(tag, train, test, target, margins=(0, 1, 2), sub=None):
    lo, hi, n = fit(train[0], target)
    fitted = int(np.sum(n >= N_MIN))
    print("\n%s   train %d CTUs / %d clips, test %d CTUs / %d clips" %
          (tag, len(train[0]), train[1], len(test[0]), test[1]))
    print("  cells fitted %d of %d   oracle mean width %.3f" %
          (fitted, len(n), oracle_width(test[0])))
    print("  %-6s %8s %8s %8s %8s %8s" %
          ("margin", "cover", "width", "miss_lo", "miss_hi", "no-cell"))
    for m in margins:
        cov, w, ml, mh, fb = evaluate(lo, hi, n, test[0], m)
        print("  %-6d %7.3f%% %8.3f %7.3f%% %7.3f%% %7.3f%%" %
              (m, 100 * cov, w, 100 * ml, 100 * mh, 100 * fb))
    if sub:
        for name, mask in sub:
            if mask.sum() < 500:
                continue
            print("   -- %-18s n=%8d oracle %.3f" %
                  (name, mask.sum(), oracle_width(test[0][mask])))
            for m in margins:
                cov, w, ml, mh, fb = evaluate(lo, hi, n, test[0][mask], m)
                print("      m=%d  cover %7.3f%%  width %.3f" %
                      (m, 100 * cov, w))
    return lo, hi, n


def slice_masks(a):
    s = a[:, C["slice"]]
    return [("slice B", s == 0), ("slice P", s == 1), ("slice I", s == 2)]


def sweep(tag, train, test, targets, margins=(0, 1, 2)):
    """The coverage/width trade the stop rule is read off.

    The per-cell target is the only thing that trades one for the other:
    fit to 0.999 and every cell keeps the whole tree, fit to 0.6 and the
    table is narrow and wrong. What matters is the NARROWEST mean width at
    which held-out coverage is still 99%, at each margin.
    """
    print("\n%s" % tag)
    print("  %-7s" % "target" +
          "".join("  m=%d cover   width" % m for m in margins))
    for t in targets:
        lo, hi, n = fit(train[0], t)
        row = "  %-7.3f" % t
        for m in margins:
            cov, w, _, _, _ = evaluate(lo, hi, n, test[0], m)
            row += "  %8.3f%% %7.3f" % (100 * cov, w)
        print(row)


def population(tag, a):
    """What the table is trying to predict, before any feature is read."""
    dmin, dmax = a[:, C["dmin"]], a[:, C["dmax"]]
    print("\n%s   N=%d" % (tag, len(a)))
    print("  P(dmin=0) %.4f   P(dmax=3) %.4f   oracle mean width %.3f" %
          ((dmin == 0).mean(), (dmax == 3).mean(),
           (dmax - dmin + 1).mean()))
    print("  the best width-3 interval, chosen with no features at all:"
          " [0,2] covers %.3f, [1,3] covers %.3f" %
          ((dmax <= 2).mean(), (dmin >= 1).mean()))


# ---------------------------------------------------------------- export

def export(path, lo, hi, n, prov):
    shape = grid_shape()
    ncell = int(np.prod(shape))
    with open(path, "w") as f:
        f.write("/*\n * Generated by scripts/depth_fit.py - do not edit.\n"
                " *\n")
        for line in prov:
            f.write(" * %s\n" % line)
        f.write(" */\n\n#ifndef N265_DEPTH_TABLE_H\n"
                "#define N265_DEPTH_TABLE_H\n\n")
        f.write("#define N265_DT_CELLS %d\n\n" % ncell)
        f.write("/* axes, in the order the index is built:\n")
        for (name, edges), nb in zip(GRID, shape):
            f.write(" *   %-9s %d bins, edges %s\n" % (name, nb, edges))
        f.write(" */\n\n")
        f.write("static const unsigned char n265_dt_lo[N265_DT_CELLS] = {\n")
        for i in range(0, ncell, 32):
            f.write("    " + ",".join("%d" % v for v in lo[i:i + 32]) + ",\n")
        f.write("};\n\n")
        f.write("static const unsigned char n265_dt_hi[N265_DT_CELLS] = {\n")
        for i in range(0, ncell, 32):
            f.write("    " + ",".join("%d" % v for v in hi[i:i + 32]) + ",\n")
        f.write("};\n\n#endif\n")


# ------------------------------------------------------------------ main

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--dumps", required=True)
    ap.add_argument("--target", type=float, default=0.99,
                    help="per-cell coverage the interval is fitted to")
    ap.add_argument("--export", default=None)
    ap.add_argument("--grid", choices=("fine", "coarse"), default="fine")
    args = ap.parse_args()

    global GRID
    if args.grid == "coarse":
        GRID = GRID_COARSE

    clips = load(args.dumps)
    if not clips:
        sys.exit("no dumps under %s" % args.dumps)
    band = [c for c in clips if c in BAND]
    extras = [c for c in clips if c in EXTRAS]
    other = [c for c in clips if c not in BAND and c not in EXTRAS]
    if other:
        print("unclassified clips, ignored: %s" % ", ".join(other),
              file=sys.stderr)
    print("band   (%d): %s" % (len(band), " ".join(sorted(band))))
    print("extras (%d): %s" % (len(extras), " ".join(sorted(extras))))
    print("grid: " + " x ".join("%s(%d)" % (n, len(e) + 1) for n, e in GRID)
          + " = %d cells, N_MIN %d, per-cell target %.3f"
          % (int(np.prod(grid_shape())), N_MIN, args.target))

    A = lambda names: (np.concatenate([clips[c] for c in names]), len(names))
    b, x = A(band), A(extras)

    population("population, all clips", np.concatenate([b[0], x[0]]))
    population("population, band", b[0])
    population("population, extras", x[0])

    curve("[1] train EXTRAS -> test BAND", x, b, args.target,
          sub=slice_masks(b[0]))
    curve("[2] train BAND -> test EXTRAS", b, x, args.target,
          sub=slice_masks(x[0]))

    # Within-resolution held-out: the two directions above are also a CIF
    # vs HD split, so on their own they cannot say whether a miss is the
    # content or the resolution. These two hold out clips of the SAME
    # kind, which separates the two.
    if len(band) >= 4:
        h = sorted(band)[:len(band) // 2]
        t = [c for c in sorted(band) if c not in h]
        curve("[3] within CIF: train %s -> test %s" % (",".join(h), ",".join(t)),
              A(h), A(t), args.target)
    if len(extras) >= 4:
        h = sorted(extras)[:len(extras) // 2]
        t = [c for c in sorted(extras) if c not in h]
        curve("[4] within HD: train %s -> test %s" % (",".join(h), ",".join(t)),
              A(h), A(t), args.target)

    tg = (0.999, 0.99, 0.95, 0.90, 0.80, 0.60)
    sweep("[S1] per-cell target sweep, train EXTRAS -> test BAND", x, b, tg)
    sweep("[S2] per-cell target sweep, train BAND -> test EXTRAS", b, x, tg)

    if args.export:
        allc = (np.concatenate([clips[c] for c in sorted(clips)]), len(clips))
        lo, hi, n = fit(allc[0], args.target)
        export(args.export, lo, hi, n,
               ["fitted on: " + " ".join(sorted(clips)),
                "rows %d, per-cell target %.3f, N_MIN %d" %
                (len(allc[0]), args.target, N_MIN)])
        print("\nwrote %s (%d cells, %d bytes of table)" %
              (args.export, len(lo), 2 * len(lo)))


if __name__ == "__main__":
    main()
