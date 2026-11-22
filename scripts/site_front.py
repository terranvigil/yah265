#!/usr/bin/env python3
# Copyright (c) 2026, the yah265 authors
# SPDX-License-Identifier: GPL-2.0-or-later
"""The front pages' numbers, charts and option list, from the data files.

Fills every `<!--gen:NAME-->...<!--/gen:NAME-->` span in README.md,
site/index.md, site/hdr.md and site/options.md, and draws two charts into
site/assets/charts/front/:

    home-hero.svg    the headline as a race between the two encoders
    home-speed.svg   times faster than x265 at G3 per class, on each machine
    hdr-bd.svg       bits saved at equal HDR-VMAF per HDR cell

The headline is blog_charts.headline_cells(), so the home page, the README,
the launch post, the results page and the social card state one pair of
figures. It covers the size-matched cells timed on the Mac: the valid G3
rows of tiers S, M and L in the newest
docs/parity/PARITY-REPORT-yah265-<date>.csv, less the titles on
blog_charts.EXCLUDED. Speed and the VMAF v1 lead are medians over the same
cells. Each chart is written twice, as name.svg and as name-narrow.svg for
phones. HDR numbers come from
docs/data/hdr-results.json. The option list comes from a captured
`y265 --help`, docs/data/cli-help.txt.

    scripts/site_front.py                    # refill pages, redraw charts
    scripts/site_front.py --check            # exit 1 if anything is stale
    scripts/site_front.py --capture <y265>   # recapture --help from a binary
    scripts/site_front.py --stamp            # print SEEN for the current capture
"""
import argparse
import csv
import hashlib
import json
import os
import re
import statistics as st
import subprocess
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import blog_charts as bc  # noqa: E402

ROOT = bc.ROOT
OUT = os.path.join(ROOT, "site/assets/charts/front")
HDR = os.path.join(ROOT, "docs/data/hdr-results.json")
HELP = os.path.join(ROOT, "docs/data/cli-help.txt")
PAGES = ["README.md", "site/index.md", "site/hdr.md", "site/options.md"]

# ------------------------------------------------------------------ data

def class_medians(rows):
    """class -> median G3 ratio over the valid cells of tiers S, M and L."""
    acc = {}
    for r in rows:
        v = bc.fnum(r["ratio"])
        if r["goal"] == "G3" and r["tier"] in bc.TIERS and r["valid"] == "yes" \
                and v is not None and "FAIL:" not in r["note"]:
            acc.setdefault(r["class"], []).append(v)
    return {c: (st.median(v), len(v)) for c, v in acc.items()}


# ------------------------------------------------------------------ charts

def head(W, H, title, desc):
    return [f"<svg xmlns='http://www.w3.org/2000/svg' viewBox='0 0 {W} {H}' role='img'>",
            f"<title>{bc.esc(title)}</title><desc>{bc.esc(desc)}</desc>", bc.STYLE,
            "<rect width='100%' height='100%' class='bg'/>"]


HERO_STYLE = """<style>
.run{transform-box:fill-box;transform-origin:left center;animation:run 2.4s linear both}
.after{animation:show .4s 2.4s both}
@keyframes run{from{transform:scaleX(0)}}
@keyframes show{from{opacity:0}}
@media (prefers-reduced-motion:reduce){.run,.after{animation:none}}
</style>"""


def chart_hero(pl):
    """The home page's race: both encoders start together on the same
    encode, and the picture stops when yah265 finishes. x265's bar is then
    as long as the headline's median ratio. On the narrow canvas the
    figure sits above the bars, so nothing hangs off the right edge."""
    nw = bc.NARROW
    W, L, R = bc.width(), bc.pick(104, 20), bc.pick(196, 20)
    ratio = pl["median"]
    X = lambda v: L + v * (W - L - R)
    title = f"When yah265 finishes, x265 is {ratio * 100:.0f}% of the way through"
    sub = (f"The median of {pl['n']} CRF encodes at the same file size on an Apple M5 Max. "
           "Both at preset medium with SIMD on all cores.")
    y1 = 104 if not nw else 158
    y2, h = y1 + (56 if not nw else 64), 34 if not nw else 28
    H = y2 + h + (72 if not nw else 92)
    o = head(W, H, title,
             "Two bars racing over the same encode. The yah265 bar reaches the end. The x265 "
             f"bar has covered {ratio * 100:.0f}% of the distance at that moment, which is the "
             f"median over {pl['n']} size-matched cells.")
    o.append(HERO_STYLE)
    o.append(bc.T(24, 40, title, 22, weight="600"))
    o.append(bc.T(24, 64, sub, 14, "t2"))
    for v in (0, 0.25, 0.5, 0.75, 1):
        o.append(f"<line x1='{X(v):.1f}' y1='{y1 - 10}' x2='{X(v):.1f}' y2='{y2 + h + 10}' "
                 f"class='{'base' if v == 1 else 'grid'}'"
                 + (" stroke-width='2' stroke-dasharray='4 4'" if v == 1 else "") + "/>")
        if nw and v in (0.25, 0.75):
            continue
        o.append(bc.T(X(v), y2 + h + 28, "start" if v == 0 else "finish" if v == 1
                      else f"{v * 100:.0f}%", 12, "t2",
                      "start" if nw and v == 0 else "end" if nw and v == 1 else "middle"))
    for y, name, v, cls in ((y1, "yah265", 1.0, "s1"), (y2, "x265", ratio, "sx")):
        o.append(f"<rect x='{X(0):.1f}' y='{y}' width='{X(1) - X(0):.1f}' height='{h}' rx='4' "
                 "class='band'/>")
        o.append(bc.bar(X(0), X(v), y, h, cls).replace("class='", "class='run "))
        if nw:
            o.append(bc.T(L, y - 8, name, 14, "t1", "start", "600"))
        else:
            o.append(bc.T(L - 14, y + h / 2 + 5, name, 15, "t1", "end", "600"))
    o.append("<g class='after'>")
    if nw:
        o.append(bc.T(L, 122, bc.headline_times(pl), 34, "note", weight="700"))
        o.append(bc.T(L + 104, 122, "faster than x265", 15, "t2"))
    else:
        o.append(bc.T(X(1) + 16, y1 + h / 2 - 1, bc.headline_times(pl), 30, "note", weight="700"))
        o.append(bc.T(X(1) + 16, y1 + h / 2 + 19, "faster than x265", 13, "t2"))
    o.append(bc.T(X(ratio) + 10, y2 + h / 2 + 5, f"{ratio * 100:.0f}% done", 13, "t1"))
    o.append("</g>")
    yb = y2 + h + 62
    o.append(f"<line x1='{bc.pick(24, 20)}' y1='{yb - 22}' x2='{W - bc.pick(24, 20)}' "
             f"y2='{yb - 22}' class='grid'/>")
    o.append(bc.marker(0, bc.pick(31, 26), yb - 4, "s1", 5))
    bc.note(o, bc.pick(44, 38), yb, f"At that file size yah265 also scores a median "
            f"{pl['dv']:.2f} VMAF v1 points higher than x265.", bc.pick(14, 12.5), "t1", W - 60)
    o.append("</svg>")
    return "\n".join(o)


def chart_home(cm, n_all):
    """One panel per machine, one bar per class, x265 as the 1x line."""
    W, Tp = bc.width(), 96
    L, R = bc.pick((150, 70), (70, 52))
    rowh, barh, ph = 30, 16, 40
    classes = [c for c in bc.CLASSES if any(c in cm[m] for m in bc.MACHINES)]
    vals = {m: {c: 1 / round(cm[m][c][0], 2) for c in classes if c in cm[m]} for m in bc.MACHINES}
    top = max(v for d in vals.values() for v in d.values())
    xmax = max(2.0, int(top * 2 + 1.999) / 2)
    X = lambda v: L + v / xmax * (W - L - R)
    H = Tp + len(bc.MACHINES) * (ph + rowh * len(classes)) + bc.pick(50, 82)
    title = "yah265 is faster than x265 at each picture size on all three machines"
    o = head(W, H, title,
             "Bars of the median times faster than x265 at the same file size, per picture "
             "size, on an Apple M5 Max, an Intel Xeon and an AWS Graviton4. Both encoders at "
             "preset medium with SIMD on all cores.")
    o.append(bc.T(24, 38, title, 21, weight="600"))
    o.append(bc.T(24, 62, "Median times faster at the same file size. Both encoders at preset "
                          "medium, SIMD on all cores.", 14, "t2"))
    if not bc.NARROW:
        o.append(bc.T(X(1), Tp + 22, "x265 = 1x", 12.5, "t2", "middle"))
    y = Tp
    for m in bc.MACHINES:
        o.append(bc.T(24, y + 24, bc.MACHINE_LONG[m], 15, weight="600"))
        y0 = y + ph - 4
        yb = y0 + rowh * len(classes)
        v = 0.5
        while v <= xmax + 1e-9:
            if v != 1:
                o.append(f"<line x1='{X(v):.1f}' y1='{y0}' x2='{X(v):.1f}' y2='{yb}' class='grid'/>")
            v += 0.5
        o.append(f"<line x1='{X(0):.1f}' y1='{y0}' x2='{X(0):.1f}' y2='{yb}' class='base'/>")
        for i, c in enumerate(classes):
            ry = y0 + rowh * i + (rowh - barh) / 2
            o.append(bc.T(L - 12, ry + barh - 3, bc.RES[c], 13, "t1", "end"))
            if c not in vals[m]:
                o.append(bc.T(L + 8, ry + barh - 3, "not measured", 12, "t2"))
                continue
            o.append(bc.bar(X(0), X(vals[m][c]), ry, barh, "s1"))
            o.append(bc.T(X(vals[m][c]) + 7, ry + barh - 3, f"{vals[m][c]:.2f}x", 13, "t1"))
        o.append(f"<line x1='{X(1):.1f}' y1='{y0 - 4}' x2='{X(1):.1f}' y2='{yb + 4}' class='lx' "
                 "stroke-width='2' stroke-dasharray='5 3'/>")
        y = yb
    v = 0.0
    while v <= xmax + 1e-9:
        o.append(bc.T(X(v), y + 20, "0" if v == 0 else f"{v:g}x", 12, "t2", "middle"))
        v += bc.pick(0.5, 1.0)
    bc.note(o, bc.pick(X(0), 24), y + 40, f"Median over the valid cells of a size: short clips, "
            f"two-minute segments and ten-minute films. {n_all} cells in all.", 12, "t2", W - 48)
    o.append("</svg>")
    return "\n".join(o)


def hdr_rows(data, ours, against):
    return [r for r in data["rows"] if r["ours"] == ours and r["against"] == against]


def chart_hdr(data):
    """Bits saved at equal HDR-VMAF: one panel per x265 configuration."""
    panels = [("Against x265", hdr_rows(data, "medium", "x265")),
              ("Against x265 with its --hdr10-opt",
               hdr_rows(data, "medium", "x265 --hdr10-opt"))]
    W, Tp = bc.width(), 92
    L, R = bc.pick((220, 80), (150, 56))
    rowh, barh, ph = 30, 16, 40
    top = max(-r["bd_pct"] for _, rs in panels for r in rs)
    xmax = (int(top / 10) + 1) * 10
    X = lambda v: L + v / xmax * (W - L - R)
    H = Tp + sum(ph + rowh * len(rs) for _, rs in panels) + 34
    title = "yah265 needs fewer bits than x265 for the same HDR-VMAF on each cell"
    o = head(W, H, title,
             "Bars of the share of bitrate yah265 saves at equal HDR-VMAF, per title and "
             "picture size, against x265 and against x265 with its PQ tuning on.")
    o.append(bc.T(24, 38, title, 21, weight="600"))
    o.append(bc.T(24, 62, f"Share of bitrate saved at equal {data['metric']}. Both encoders at "
                          "preset medium. Longer is better.", 14, "t2"))
    y = Tp - 12
    for name, rs in panels:
        o.append(bc.T(24, y + 24, name, 15, weight="600"))
        y0 = y + ph - 4
        yb = y0 + rowh * len(rs)
        v = 10
        while v <= xmax + 1e-9:
            o.append(f"<line x1='{X(v):.1f}' y1='{y0}' x2='{X(v):.1f}' y2='{yb}' class='grid'/>")
            v += 10
        o.append(f"<line x1='{X(0):.1f}' y1='{y0}' x2='{X(0):.1f}' y2='{yb}' class='base'/>")
        for i, r in enumerate(rs):
            ry = y0 + rowh * i + (rowh - barh) / 2
            o.append(bc.T(L - 12, ry + barh - 3, f"{r['title']} {bc.RES[r['size']]}", 13, "t1", "end"))
            o.append(bc.bar(X(0), X(-r["bd_pct"]), ry, barh, "s1"))
            o.append(bc.T(X(-r["bd_pct"]) + 7, ry + barh - 3, f"{-r['bd_pct']:.1f}%", 13, "t1"))
        y = yb
    v = 0
    while v <= xmax + 1e-9:
        o.append(bc.T(X(v), y + 20, f"{v:g}%", 12, "t2", "middle"))
        v += 10
    o.append("</svg>")
    return "\n".join(o)


# ------------------------------------------------------------------ tables

def quality_table(pl, lad):
    allv = [r[3] for r in lad] + [r[4] for r in lad]
    return "\n".join([
        "| quality on the Mac | cells | median |", "|---|--:|--:|",
        f"| VMAF v1 lead at the same file size, the cells above | {pl['dv_n']} | "
        f"{pl['dv']:+.2f} points |",
        f"| bits saved at equal VMAF v1, rate ladders at CRF and capped VBR | {len(allv)} | "
        f"{abs(st.median(allv)):.1f}% |"])


def hdr_table(data):
    a = {(r["title"], r["size"]): r["bd_pct"] for r in hdr_rows(data, "medium", "x265")}
    b = {(r["title"], r["size"]): r["bd_pct"] for r in hdr_rows(data, "medium", "x265 --hdr10-opt")}
    L = ["| title | transfer | size | against x265 | against x265 `--hdr10-opt` |",
         "|---|---|---|--:|--:|"]
    tr = {(r["title"], r["size"]): r["transfer"] for r in data["rows"]}
    for k, v in a.items():
        opt = f"{b[k]:.1f}%" if k in b else "PQ only"
        L.append(f"| {k[0]} | {tr[k]} | {bc.RES[k[1]]} | {v:.1f}% | {opt} |")
    return "\n".join(L)


def hdr_tuning_table(data):
    on = {r["size"]: r["bd_pct"] for r in hdr_rows(data, "medium", "x265") if r["transfer"] == "HLG"}
    off = {r["size"]: r["bd_pct"] for r in hdr_rows(data, "medium --no-hdr-opt", "x265")}
    gain = {r["size"]: r["bd_pct"] for r in hdr_rows(data, "medium", "yah265 --no-hdr-opt")}
    L = ["| HLG clip | tuned, against x265 | untuned, against x265 | tuned against untuned |",
         "|---|--:|--:|--:|"]
    for s in on:
        L.append(f"| {bc.RES[s]} | {on[s]:.1f}% | {off[s]:.1f}% | {gain[s]:.1f}% |")
    return "\n".join(L)


def hdr_ladder_table(data):
    L = ["| title | excerpt | yah265 `--crf` | x265 `--crf` |", "|---|---|---|---|"]
    for r in data["ladders"]:
        L.append(f"| {r['title']} | {r['frames']} | {r['ours_crf']} in steps of {r['step']} | "
                 f"{r['x265_crf']} in steps of {r['step']} |")
    return "\n".join(L)


# ------------------------------------------------------------------ options

OPT_HEAD = re.compile(r"^  ((?:-{1,2}[\w-]+)(?:(?:, | / )-{1,2}[\w-]+)*)(?: (<[^>]+>))?(?:\s+(.*))?$")

# One row per option the capture lists: (flags as the capture spells them,
# default, one line). The option and its argument are read from the capture.
# The line and the default are written here from the capture's text, and
# SEEN pins the text each was written from, so a changed help entry fails
# --check until its row is read again.
GROUPS = [
    ("Input and output", [
        ("-o", "`out.hevc`", "The output file, a raw Annex-B HEVC stream."),
        ("--frames", "all", "Encode at most this many frames."),
        ("--seek", "0", "Skip this many frames of the input first. Timestamps start at zero on the first frame coded."),
        ("--output-depth", "the input's", "Code at this sample width. `10` on 8-bit input writes Main10. `8` on 10-bit input drops two bits."),
        ("--dither", "off", "With 10-bit input and `--output-depth 8`, add an ordered dither before dropping the two bits."),
        ("--input-res", "none", "Picture size of raw YUV input, such as `1920x1080`. Naming it selects the raw reader."),
        ("--input-csp", "none", "Chroma format of raw input: `i420`, `i422` or `i444`."),
        ("--fps", "none", "Frame rate of raw input, as `30`, `30000/1001` or `29.97`."),
        ("--input-depth", "`8`", "Sample width of raw input. 10-bit samples are 16-bit little-endian."),
        ("--dump-recon", "off", "Write the encoder's reconstruction as raw YUV, for comparing against a decoder."),
    ]),
    ("Presets and tunes", [
        ("--preset", "`medium`", "The speed and quality point: `fast`, `medium`, `slow` or `slower`. It sets defaults, and any option you give wins over it."),
        ("-t, --tune", "none", "`zerolatency` turns off B-frames, the lookahead and cu-tree. The other four x265 tune names are refused with a reason."),
    ]),
    ("Rate control", [
        ("--crf", "off", "Constant rate factor. Naming it turns CRF on. It goes 1.8 QP coarser each time the frame rate doubles, and is neutral at 25 fps."),
        ("--qp", "`26`", "Fixed QP, 0 to 51 at 8 bits and -12 to 51 at 10. It's the mode when no rate option is given."),
        ("--bitrate", "off", "Target mean bitrate in kbit/s, in one pass. It wins over `--crf`."),
        ("--vbv-maxrate", "off", "Decoder buffer fill rate in kbit/s. With `--bitrate` it makes capped VBR, and with `--crf` capped CRF. Needs `--vbv-bufsize`."),
        ("--vbv-bufsize", "off", "Decoder buffer size in kbit. Needs `--vbv-maxrate`."),
        ("--vbv-init", "`0.9`", "How full the buffer starts, as a fraction of its size."),
        ("--strict-cbr", "off", "Hold a constant rate under `--bitrate` with a buffer. Filler data pads any picture that comes in under its share."),
        ("--max-ausize-factor", "`1.0`", "Bound one coded picture to this fraction of the buffer, 0.5 to 1.0. Needs a buffer."),
        ("--crf-min", "none", "Lowest rate factor a `--crf` encode may use."),
        ("--crf-max", "none", "Highest rate factor a `--crf` encode may use."),
        ("--qpmin", "none", "Lowest frame QP the rate control may choose."),
        ("--qpmax", "none", "Highest frame QP the rate control may choose."),
        ("--pass", "off", "Two-pass. `1` writes a statistics file, `2` reads it and plans the whole file, `3` does both. Pass 2 needs `--bitrate`."),
        ("--stats", "none", "The statistics file `--pass` writes or reads."),
        ("--aq", "`1`", "Adaptive quantization, which moves bits toward flat and dark areas."),
        ("--cutree", "`1`", "Lookahead QP offsets that favor blocks later frames predict from."),
        ("--cbqpoffs", "the preset's", "Cb quantizer offset from luma, -12 to 12. A negative value spends more bits on chroma."),
        ("--crqpoffs", "the preset's", "The same for Cr."),
        ("--lossless", "off", "Lossless coding. Rate control, AQ and the loop filters are off."),
        ("--pcm", "off", "Code each block as raw samples. A test mode with very large output."),
    ]),
    ("Steering an encode by frame", [
        ("--qpfile", "none", "Force frame types and QPs from a file, one `<frame> <type> [<qp>]` line each. Types are `I`, `K`, `i` and `-`."),
        ("--zones", "none", "QP or bitrate changes over frame ranges, as `start,end,q=N` or `start,end,b=F` joined by `/`."),
    ]),
    ("GOP and references", [
        ("--keyint", "`250`", "Longest gap between key frames. A scene cut restarts the count, except under `--no-scenecut`."),
        ("--no-scenecut", "off", "Put key frames only on frames 0, `--keyint`, twice `--keyint` and so on. A scene cut gets none. Every rung of a ladder then has its key frames on the same frames. `--scenecut` turns it back off. [More](guide.md#key-frames-for-streaming)"),
        ("--open-gop", "on", "Code the periodic key frame as a CRA and keep the B-frames that straddle it. `--no-open-gop` writes an IDR there. A scene cut is an IDR either way. Under `--no-scenecut` it gets no key frame."),
        ("--radl", "`0`", "How many of a CRA's leading pictures stay decodable when playback starts at that CRA."),
        ("--intra-refresh", "off", "No key frame after the first. A column of intra blocks sweeps the picture every `--keyint` frames, and a decoder can join at the start of any sweep. P-frames and 4:2:0 only. [More](guide.md#intra-refresh)"),
        ("--bframes", "`7`", "B-frames between anchors, 0 to 15. Above 7, `--rc-lookahead` must be at least one more."),
        ("--b-adapt", "`1`", "How the B run length is chosen: `0` fixed, `1` from motion. `2` to `4` are experimental."),
        ("--ref", "`1`", "References per list, 1 to 5. `slower` uses 2, and so does `medium` at 4K."),
        ("--rc-lookahead", "the preset's", "Lookahead depth in frames, 1 to 60. It's also the encoder's latency."),
        ("-w, --weightp", "on, off at `fast`", "Weighted prediction in P slices, which helps fades and lighting changes. `--no-weightp` turns it off."),
        ("--weightb", "off", "Weighted prediction in B slices."),
        ("--dup-threshold", "`0`", "How close a frame must be to a reference to be coded as a repeat. `0` takes exact repeats only."),
    ]),
    ("Block structure and quantization", [
        ("--ctu", "the preset's", "Coding tree block size: 16, 32 or 64."),
        ("--tu-inter-depth", "`2`", "Transform tree levels an inter block may use, 1 to 4."),
        ("--tu-intra-depth", "`2`", "Transform tree levels an intra block may use, 1 to 4."),
        ("--rdoq-level", "the preset's", "Rate-distortion optimized quantization: `0` off, `1` at the final encode of a block, `2` in all decisions."),
    ]),
    ("Threads", [
        ("--threads", "`0`", "Worker threads. `0` starts one per online CPU. At `1`, two runs of a command write the same bytes. Above one thread the stream can differ between runs."),
        ("--gop-parallel", "`0`", "Code a long input as segments on this many encoder instances and join them at key frames. The stream differs from the serial one. With `--shot-list` the segments start on listed shots."),
    ]),
    ("Hardware", [
        ("--hw", "`off`", "`videotoolbox` encodes with the Mac's HEVC hardware through VideoToolbox. The stream is the hardware's, not this encoder's. Options it can't follow are listed once at the start. `auto` uses this encoder when the hardware isn't there, and `videotoolbox` fails instead."),
        ("--hw-strict", "off", "With `--hw`, refuse to encode when any option you gave can't be followed."),
    ]),
    ("Shot-aware options", [
        ("--shot-scan", "none", "Scan the input for shots and write a shot list, then exit."),
        ("--shot-list", "none", "Put a key frame at the start of each listed shot."),
        ("--shot-keys", "off", "Put a key frame on each cut the encoder finds."),
        ("--shot-window", "`0`", "Buffer up to this many frames so a whole shot can be planned before it's coded. `--crf` only, at most 300. It adds that many frames of latency."),
        ("--shot-stats", "none", "Write one JSON line of statistics per shot. The stream doesn't change."),
    ]),
    ("Film grain, denoising and filters", [
        ("--film-grain", "off", "Estimate the source's grain and describe it in an SEI message. A decoder that supports the message adds grain back. The value scales the estimate. 8-bit 4:2:0 only."),
        ("--film-grain-denoise", "`0`", "With `--film-grain`, also remove the grain from the picture the encoder codes."),
        ("--nr-intra", "`0`", "Noise reduction strength on intra coefficients, 0 to 2000."),
        ("--nr-inter", "`0`", "The same on inter coefficients."),
        ("--mcstf", "off", "Motion-compensated temporal filter on the source, for noisy material."),
        ("--mcstf-ref-range", "`2`", "Neighbor frames on each side the filter reads, 1 to 4."),
        ("--selective-mcstf", "off", "Skip the filter on pictures that measure clean."),
    ]),
    ("Stream structure and SEI", [
        ("--repeat-headers", "off", "Write the parameter sets at each key frame, for a receiver that joins mid-stream."),
        ("--aud", "off", "An access unit delimiter at the head of each picture."),
        ("--eos", "off", "An end-of-sequence NAL before each IDR after the first and at the end of the stream."),
        ("--eob", "off", "An end-of-bitstream NAL as the last NAL."),
        ("--no-info", "info on", "Drop the SEI message naming the encoder, its version and its options. `--info` keeps it."),
        ("--no-fingerprint", "fingerprint on", "Drop the stream fingerprint, the mark that survives when SEI messages are stripped."),
        ("--hash", "`0`", "A hash of each decoded picture in an SEI message: `1` MD5, `2` CRC, `3` checksum."),
        ("--idr-recovery-sei", "off", "A recovery point SEI at each key frame."),
        ("--pic-struct", "off", "The picture timing SEI's pic_struct, 0 to 12. `7` doubles a frame and `8` triples it. Needs a buffer."),
        ("--hrd-concat", "off", "Mark the first key frame as spliced onto another stream. Needs a buffer."),
        ("--no-hrd", "off", "Leave the HRD timing out of the stream. It's written by default when a buffer and a frame rate are set. A live encode that raises its rate needs this."),
        ("--temporal-layers", "off", "Put unreferenced B-frames in a second temporal layer. A player can then drop them and halve the frame rate."),
        ("--level", "derived", "Signal this level, from `1` to `6.2`. Refused when the stream doesn't fit it."),
        ("--high-tier", "off", "Signal the High tier, which exists from level 4 up."),
        ("--vui-timing", "on", "Write the frame rate into the stream. `--no-vui-timing` leaves it out."),
    ]),
    ("Color and HDR", [
        ("--video-signal-type-preset", "none", "One name for a whole set of color flags, such as `BT2100_PQ_YCC`, with an optional mastering display after a colon."),
        ("--colorprim", "none", "Color primaries, such as `bt709` or `bt2020`."),
        ("--transfer", "none", "Transfer characteristics, such as `bt709`, `smpte2084` for PQ or `arib-std-b67` for HLG."),
        ("--colormatrix", "none", "Matrix coefficients, such as `bt709` or `bt2020nc`."),
        ("--range", "none", "`limited` or `full` sample range."),
        ("--videoformat", "none", "`component`, `pal`, `ntsc`, `secam`, `mac` or `unknown`."),
        ("--chromaloc", "none", "Chroma sample location, 0 to 5. BT.2020 and BT.2100 ask for 2."),
        ("--overscan", "`unknown`", "`show` or `crop`."),
        ("--sar", "none", "Sample aspect ratio, as `w:h` or a table index from 0 to 16."),
        ("--display-window", "none", "A display cropping region in pixels, as `left,top,right,bottom`."),
        ("--master-display", "none", "Mastering display SEI from ST 2086, as `G(x,y)B(x,y)R(x,y)WP(x,y)L(max,min)`."),
        ("--max-cll", "none", "Content light level SEI, as `cll,fall` in nits."),
        ("--hdr10 / --no-hdr10", "on once either is named", "Force the two HDR10 messages on or off."),
        ("--no-cll", "off", "Drop the content light level message and keep the mastering display."),
        ("--atc-sei", "none", "The transfer a decoder that reads this SEI should use instead of the color flags' one. Backward-compatible HLG uses it."),
        ("--dhdr10-info", "none", "HDR10+ dynamic metadata from a JSON file, one message per picture. Needs a 10-bit HDR10 encode."),
        ("--dolby-vision-rpu", "none", "A Dolby Vision profile 8 RPU per picture, from a file. Needs `--dolby-vision-profile`."),
        ("--dolby-vision-profile", "none", "`8.1`, `8.2` or `8.4`. The color flags must match it."),
        ("--hdr-opt / --no-hdr-opt", "on for PQ and HLG", "The HDR tuning of `medium`, on or off. `--hdr10-opt` is accepted as another name for it."),
    ]),
    ("Reports", [
        ("--psnr", "off", "Report PSNR at the end of the run. The stream doesn't change."),
        ("--ssim", "off", "Report SSIM on luma at the end of the run."),
        ("--csv", "none", "Write run statistics to a CSV file."),
        ("--csv-log-level", "`0`", "`0` writes one row for the run, `1` adds a row per frame, `2` adds the rate control's columns."),
        ("--frame-stats", "none", "Write one JSON line per coded frame. A frame where a live control event took effect also lists that event. The stream doesn't change."),
        ("--verbose, -v", "off", "Print the resolved configuration at the start of the encode."),
        ("--show-config", "off", "Print the resolved configuration and exit without encoding."),
        ("--version", "", "Print the version."),
    ]),
    ("Live control", [
        ("--control", "none", "Apply the events in a control log before the encode starts. A bad line or a refused event stops the run."),
        ("--control-fd", "none", "Read control log lines from this open descriptor while encoding. A late or refused line is skipped with a warning."),
        ("--control-out", "none", "Write every event that took effect as a control log. Fed back through `--control`, it gives the same stream at one thread."),
    ]),
    ("Not offered", [
        ("--scaling-list", "refused", "The encoder stops with an error. [Why](guide.md#left-out-on-purpose)"),
    ]),
]

# spellings the capture names only inside another option's text
ALSO = ["--no-open-gop", "--no-weightp", "--no-vui-timing", "--hdr10-opt"]

SEEN = {
    "-o": "78c09270",
    "--frames": "2028313c",
    "--seek": "25c60091",
    "--output-depth": "f5d82bb7",
    "--dither": "a2afb7ef",
    "--dump-recon": "ce1d5556",
    "--input-res": "6c45ac4e",
    "--input-csp": "aa440530",
    "--fps": "32b67444",
    "--input-depth": "0474ba88",
    "--preset": "6354eb7e",
    "-t, --tune": "20fc31ec",
    "--threads": "94de2a57",
    "--gop-parallel": "6ba3ce06",
    "--hw": "41d071d2",
    "--hw-strict": "cef62975",
    "--qp": "3feae834",
    "--crf": "6ec86a4b",
    "--bitrate": "17ce7429",
    "--vbv-maxrate": "8d49346c",
    "--vbv-bufsize": "4c2fdfa6",
    "--vbv-init": "307d90f0",
    "--strict-cbr": "33bf5828",
    "--max-ausize-factor": "b4bfd437",
    "--crf-min": "d6898046",
    "--crf-max": "89a3c1a3",
    "--qpmin": "841e0cfd",
    "--qpmax": "7421f600",
    "--pass": "c963a7f0",
    "--stats": "1b4a8402",
    "--qpfile": "6a584e90",
    "--zones": "881c56ce",
    "--aq": "83615b1c",
    "--cutree": "ac3f3bb7",
    "--rc-lookahead": "d3b478d1",
    "--shot-window": "98e21ad5",
    "--keyint": "22804ae5",
    "--no-scenecut": "c5f04e29",
    "--open-gop": "763c77dc",
    "--radl": "0f6d88fa",
    "--bframes": "96b0b99e",
    "--b-adapt": "8c5e38ef",
    "--ref": "dd1196a8",
    "--ctu": "d46c8086",
    "--tu-inter-depth": "a227bad8",
    "--tu-intra-depth": "a44166e9",
    "--rdoq-level": "65ffa0ac",
    "--cbqpoffs": "1ed875ff",
    "--crqpoffs": "16775206",
    "-w, --weightp": "a029e2d1",
    "--weightb": "70ed5047",
    "--dup-threshold": "d5dd6528",
    "--pcm": "e08d4ca7",
    "--lossless": "817a57bd",
    "--intra-refresh": "6024bed1",
    "--scaling-list": "ef457170",
    "--film-grain": "1def6b4f",
    "--film-grain-denoise": "63be09fb",
    "--nr-intra": "3d2e4e0c",
    "--nr-inter": "5c64862a",
    "--mcstf": "cc59dae4",
    "--mcstf-ref-range": "4e18b52d",
    "--selective-mcstf": "692a6d78",
    "--repeat-headers": "0f7db08d",
    "--aud": "441d3b2b",
    "--eos": "2a659ece",
    "--eob": "e88e4b44",
    "--idr-recovery-sei": "c891bf08",
    "--pic-struct": "f2e462cf",
    "--hrd-concat": "772bf0a2",
    "--no-hrd": "46659d51",
    "--temporal-layers": "4b8e7fb0",
    "--hash": "18e106b2",
    "--no-info": "41a3efe3",
    "--no-fingerprint": "8468837f",
    "--vui-timing": "ff08b572",
    "--level": "9b934986",
    "--high-tier": "f70798d8",
    "--video-signal-type-preset": "a8fc6bf4",
    "--colorprim": "5bdd6bc6",
    "--transfer": "d5648eeb",
    "--colormatrix": "2cf9d769",
    "--range": "7e0645b7",
    "--videoformat": "6ce98d5a",
    "--chromaloc": "de816651",
    "--overscan": "89184e01",
    "--sar": "79e1d1d8",
    "--display-window": "7cacafe8",
    "--master-display": "ca8e71fd",
    "--max-cll": "f43c2aa9",
    "--hdr10 / --no-hdr10": "23a385a1",
    "--no-cll": "bd76e977",
    "--atc-sei": "b96c0a89",
    "--dhdr10-info": "ab6e65b7",
    "--dolby-vision-rpu": "e8c31d65",
    "--dolby-vision-profile": "c74f03b8",
    "--hdr-opt / --no-hdr-opt": "797f674e",
    "--psnr": "a5d720eb",
    "--ssim": "dd5abe61",
    "--csv": "a32552e4",
    "--csv-log-level": "6b411b40",
    "--shot-scan": "8d986034",
    "--shot-list": "5d0ba269",
    "--shot-keys": "536e5b57",
    "--shot-stats": "6b48ef8b",
    "--frame-stats": "7b42237e",
    "--verbose, -v": "906ee3c9",
    "--show-config": "4e3a0626",
    "--version": "f16a2431",
    "--control": "afb0197a",
    "--control-fd": "f97ad926",
    "--control-out": "b71d68f6",
}


def parse_help(text):
    """[(flags, argument, text)] and the capture's build line."""
    lines = text.splitlines()
    build = lines[0][2:].strip() if lines and lines[0].startswith("# ") else None
    ents, cur = [], None
    for line in lines:
        m = OPT_HEAD.match(line)
        if m:
            cur = [m[1], m[2] or "", [m[3] or ""]]
            ents.append(cur)
        elif line.startswith(" " * 18) and cur is not None:
            cur[2].append(line.strip())
        else:
            cur = None
    return [(f, a, " ".join(x for x in t if x)) for f, a, t in ents], build


def stamp(text):
    return hashlib.sha1(text.encode()).hexdigest()[:8]


def row_ids(flags, line, taken):
    """The link targets of one row: opt-<long name> for each spelling the
    row heads, for a spelling in ALSO its text names, and for the plain
    form of a --no- switch that has no row of its own."""
    names = [f.lstrip("-") for f in re.split(r", | / ", flags)]
    long = [n for f, n in zip(re.split(r", | / ", flags), names) if f.startswith("--")]
    ids = long or names
    ids += [s[2:] for s in ALSO if f"`{s}`" in line]
    ids += [n[3:] for n in long if n.startswith("no-") and "--" + n[3:] not in taken]
    return list(dict.fromkeys(ids))


def options_tables(check_seen=True):
    if not os.path.exists(HELP):
        sys.exit(f"{HELP}: missing. Capture it with scripts/site_front.py --capture <y265>")
    ents, build = parse_help(open(HELP).read())
    if not build:
        sys.exit(f"{HELP}: the first line must be '# <build>'; use --capture")
    cap = {f: (a, t) for f, a, t in ents}
    rows = [r[0] for _, rs in GROUPS for r in rs]
    errs = [f"in the capture with no row: {f}" for f in cap if f not in rows]
    errs += [f"row with no entry in the capture: {f}" for f in rows if f not in cap]
    if check_seen:
        errs += [f"help text changed, read it and restamp: {f}" for f in cap
                 if f in rows and SEEN.get(f) != stamp(cap[f][1])]
    raw = open(HELP).read()
    errs += [f"{s} is no longer in the capture" for s in ALSO if s not in raw]
    if errs:
        sys.exit("docs/data/cli-help.txt and the option rows disagree:\n  " + "\n  ".join(errs))
    out = []
    taken = {x for f in rows for x in re.split(r", | / ", f)}
    for name, rs in GROUPS:
        out += [f"## {name}", "", "| option | default | what it does |", "|---|---|---|"]
        for f, d, line in rs:
            spec = " ".join(x for x in (f, cap[f][0]) if x).replace("|", "\\|")
            marks = "".join(f'<span id="opt-{i}"></span>' for i in row_ids(f, line, taken))
            out.append(f"| {marks}`{spec}` | {d} | {line} |")
        out.append("")
    listed = "\n".join(out)
    miss = [s for s in ALSO if s not in listed]
    if miss:
        sys.exit(f"no row mentions {miss}")
    return listed.rstrip(), build, len(rows)


def capture(binary):
    ver = subprocess.run([binary, "--version"], capture_output=True, text=True)
    hlp = subprocess.run([binary, "--help"], capture_output=True, text=True)
    sha = hashlib.sha256(open(binary, "rb").read()).hexdigest()[:8]
    name = (ver.stdout + ver.stderr).strip().splitlines()[0]
    with open(HELP, "w") as f:
        f.write(f"# {name}, release build {sha}\n" + hlp.stdout + hlp.stderr)
    print(f"wrote {os.path.relpath(HELP, ROOT)} from {name}, build {sha}")


# ------------------------------------------------------------------ main

def fill(path, text, V):
    missing = sorted({m[1] for m in bc.GEN.finditer(text) if m[1] not in V})
    if missing:
        sys.exit(f"{path}: no value for {missing}")

    def sub(m):
        v = V[m[1]]
        nl = "\n" if "\n" in v else ""
        return f"<!--gen:{m[1]}-->{nl}{v}{nl}<!--/gen:{m[1]}-->"
    return bc.GEN.sub(sub, text)


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--check", action="store_true",
                    help="change nothing; exit 1 if a page or a chart is out of date")
    ap.add_argument("--capture", metavar="N265", help="write docs/data/cli-help.txt from this binary")
    ap.add_argument("--stamp", action="store_true", help="print SEEN for the current capture")
    a = ap.parse_args()
    if a.capture:
        capture(a.capture)
        return
    if a.stamp:
        ents, _ = parse_help(open(HELP).read())
        print("SEEN = {")
        for f, _, t in ents:
            print(f'    "{f}": "{stamp(t)}",')
        print("}")
        return
    D = r"\d{4}-\d\d-\d\d"
    paths = {"mac": bc.newest(rf"PARITY-REPORT-yah265-{D}\.csv"),
             "x86": bc.newest(rf"PARITY-REPORT-yah265-x86-{D}\.csv"),
             "g4": bc.newest(rf"PARITY-REPORT-yah265-graviton-{D}\.csv")}
    rows = {m: bc.report_rows(p) for m, p in paths.items()}
    mac_md = paths["mac"][:-4] + ".md"
    sps = {m: bc.speed_groups(rs) for m, rs in rows.items()}
    lad = bc.ladders(mac_md)
    if not lad:
        sys.exit(f"{mac_md}: no BD ladder lines found")
    pl = bc.headline_cells(rows["mac"])
    cm = {m: class_medians(rs) for m, rs in rows.items()}
    hdr = json.load(open(HDR))
    opts, build, nopt = options_tables()
    V = {
        "headline": bc.headline(pl),
        "fig-frames": bc.frames_figure((0,), bc.FRAMES_ONE),
        "pooled-table": bc.headline_table(pl),
        "quality-table": quality_table(pl, lad),
        "goal-table": bc.goal_table(sps),
        "hdr-table": hdr_table(hdr),
        "hdr-tuning-table": hdr_tuning_table(hdr),
        "hdr-ladder-table": hdr_ladder_table(hdr),
        "options": opts,
    }
    n_all = sum(v[1] for d in cm.values() for v in d.values())
    charts = bc.render({"home-hero.svg": lambda: chart_hero(pl),
                        "home-speed.svg": lambda: chart_home(cm, n_all),
                        "hdr-bd.svg": lambda: chart_hdr(hdr)})
    stale = bc.publish_data(OUT, "site_front.py", {
        "home-hero": ([paths["mac"]], ["class", "cell", "times_faster"],
                      [[c, n, 1 / r] for c in bc.CLASSES for r, n in pl["pools"][c]]),
        "home-speed": (list(paths.values()), ["machine", "class", "median_times_faster", "cells"],
                       [[bc.MACHINE_SHORT[m], c, 1 / cm[m][c][0], cm[m][c][1]]
                        for m in bc.MACHINES for c in bc.CLASSES if c in cm[m]]),
        "hdr-bd": ([HDR], ["title", "transfer", "size", "against", "bd_rate_pct"],
                   [[r["title"], r["transfer"], r["size"], r["against"], r["bd_pct"]]
                    for r in hdr["rows"] if r["ours"] == "medium"]),
    }, a.check)
    for name, svg in charts.items():
        p = os.path.join(OUT, name)
        old = open(p).read() if os.path.exists(p) else None
        if old != svg + "\n":
            stale.append(name)
            if not a.check:
                os.makedirs(OUT, exist_ok=True)
                with open(p, "w") as f:
                    f.write(svg + "\n")
    for rel in PAGES:
        p = os.path.join(ROOT, rel)
        old = open(p).read()
        own = {"goal-table": bc.goal_table(sps, bc.GOALS_URL)} if rel == "README.md" else {}
        new = fill(rel, old, {**V, **own})
        if new != old:
            stale.append(rel)
            if not a.check:
                with open(p, "w") as f:
                    f.write(new)
    print(f"headline: {bc.headline_times(pl)} faster over {pl['n']} size-matched cells ("
          + ", ".join(f"{k} {len(v)}" for k, v in pl["pools"].items())
          + f"); VMAF v1 {pl['dv']:+.2f} over {pl['dv_n']} cells; {nopt} options")
    if a.check:
        if stale:
            print("stale: " + ", ".join(stale))
            sys.exit(1)
        print("up to date")
    else:
        print("updated: " + (", ".join(stale) or "nothing changed"))


if __name__ == "__main__":
    main()
