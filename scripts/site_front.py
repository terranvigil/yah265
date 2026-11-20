#!/usr/bin/env python3
# Copyright (c) 2026, the yah265 authors
# SPDX-License-Identifier: GPL-2.0-or-later
"""The front pages' numbers, charts and option list, from the data files.

Fills every `<!--gen:NAME-->...<!--/gen:NAME-->` span in README.md,
site/index.md, site/hdr.md and site/options.md, and draws two charts into
site/assets/charts/front/:

    home-speed.svg   times faster than x265 at G3 per class, on each machine
    hdr-bd.svg       bits saved at equal HDR-VMAF per HDR cell

The headline is blog_charts.pooled(), so the home page, the README and the
launch post state one figure. It pools every G3 cell timed on the Mac:

    CRF            valid G3 rows of the newest docs/parity/PARITY-REPORT-yah265-<date>.csv
    capped VBR     the "VBR wall" table of the report beside that CSV
    content types  tier C G3 rows of docs/data/parity-spec.csv

The quality lead is the median VMAF v1 difference over the CRF and
content-type cells, which are size-matched. The VBR cells share a target
rate and no size, so they have no place in it. HDR numbers come from
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


def chart_home(cm, n_all):
    """One panel per machine, one bar per class, x265 as the 1x line."""
    W, L, R, Tp = 900, 150, 70, 96
    rowh, barh, ph = 30, 16, 40
    classes = [c for c in bc.CLASSES if any(c in cm[m] for m in bc.MACHINES)]
    vals = {m: {c: 1 / round(cm[m][c][0], 2) for c in classes if c in cm[m]} for m in bc.MACHINES}
    top = max(v for d in vals.values() for v in d.values())
    xmax = max(2.0, int(top * 2 + 1.999) / 2)
    X = lambda v: L + v / xmax * (W - L - R)
    H = Tp + len(bc.MACHINES) * (ph + rowh * len(classes)) + 50
    title = "yah265 is faster than x265 at every picture size on all three machines"
    o = head(W, H, title,
             "Bars of the median times faster than x265 at the same file size, per picture "
             "size, on an Apple M5 Max, an Intel Xeon and an AWS Graviton4. Both encoders at "
             "preset medium with SIMD on all cores.")
    o.append(bc.T(24, 38, title, 21, weight="600"))
    o.append(bc.T(24, 62, "Median times faster at the same file size. Both encoders at preset "
                          "medium, SIMD on all cores.", 14, "t2"))
    o.append("<rect x='24' y='74' width='12' height='12' rx='2' class='s1'/>")
    o.append(bc.T(42, 85, "yah265", 13, "t2"))
    o.append("<line x1='112' y1='80' x2='136' y2='80' class='lx' stroke-width='2' "
             "stroke-dasharray='5 3'/>")
    o.append(bc.T(142, 85, "x265, 1x", 13, "t2"))
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
        v += 0.5
    o.append(bc.T(X(0), y + 40, f"Median over every valid cell of a size: short clips, two-minute "
                                f"segments and ten-minute films. {n_all} cells in all.", 12, "t2"))
    o.append("</svg>")
    return "\n".join(o)


def hdr_rows(data, ours, against):
    return [r for r in data["rows"] if r["ours"] == ours and r["against"] == against]


def chart_hdr(data):
    """Bits saved at equal HDR-VMAF: one panel per x265 configuration."""
    panels = [("Against x265", hdr_rows(data, "medium", "x265")),
              ("Against x265 with --hdr10-opt, its PQ tuning",
               hdr_rows(data, "medium", "x265 --hdr10-opt"))]
    W, L, R, Tp = 900, 220, 80, 92
    rowh, barh, ph = 30, 16, 40
    top = max(-r["bd_pct"] for _, rs in panels for r in rs)
    xmax = (int(top / 10) + 1) * 10
    X = lambda v: L + v / xmax * (W - L - R)
    H = Tp + sum(ph + rowh * len(rs) for _, rs in panels) + 34
    title = "yah265 needs fewer bits than x265 for the same HDR-VMAF on every cell"
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
        f"| VMAF v1 lead at the same file size, CRF and content types | {pl['dv_n']} | "
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
        opt = f"{b[k]:.1f}%" if k in b else "no HLG reading"
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
        ("-t, --tune", "none", "`zerolatency` turns off B-frames and the lookahead. The other four x265 tune names are refused with a reason."),
    ]),
    ("Rate control", [
        ("--crf", "off", "Constant rate factor. Naming it turns CRF on. The scale is yah265's own and moves 1.8 QP each time the frame rate doubles."),
        ("--qp", "`26`", "Fixed QP, 0 to 51 at 8 bits and -12 to 51 at 10. It's the mode when no rate option is given."),
        ("--bitrate", "off", "Target mean bitrate in kbit/s, in one pass. It wins over `--crf`."),
        ("--vbv-maxrate", "off", "Decoder buffer fill rate in kbit/s. With `--bitrate` it makes capped VBR, and with `--crf` capped CRF. Needs `--vbv-bufsize`."),
        ("--vbv-bufsize", "off", "Decoder buffer size in kbit. Needs `--vbv-maxrate`."),
        ("--vbv-init", "`0.9`", "How full the buffer starts, as a fraction of its size."),
        ("--strict-cbr", "off", "Hold a constant rate under `--bitrate` with a buffer. Filler data pads any picture that comes in under its share."),
        ("--max-ausize-factor", "`1.0`", "Bound one coded picture to this fraction of the buffer, from 0.5 up. Needs a buffer."),
        ("--crf-min", "none", "Lowest rate factor a `--crf` encode may use."),
        ("--crf-max", "none", "Highest rate factor a `--crf` encode may use. The encoder reports an underflow when the bound is too low for the buffer."),
        ("--qpmin", "none", "Lowest frame QP the rate control may choose."),
        ("--qpmax", "none", "Highest frame QP the rate control may choose."),
        ("--pass", "off", "Two-pass. `1` writes a statistics file, `2` reads it and plans the whole file, `3` does both. Pass 2 needs `--bitrate`."),
        ("--stats", "none", "The statistics file `--pass` writes or reads."),
        ("--aq", "`1`", "Adaptive quantization, which moves bits toward flat and dark areas."),
        ("--cutree", "`1`", "Lookahead QP offsets that favor blocks later frames predict from."),
        ("--cbqpoffs", "the preset's", "Cb quantizer offset from luma, -12 to 12. A negative value spends more bits on chroma."),
        ("--crqpoffs", "the preset's", "The same for Cr."),
        ("--lossless", "off", "Mathematically lossless coding. Rate options are ignored."),
        ("--pcm", "off", "Code every block as raw samples. A test mode with very large output."),
    ]),
    ("Steering an encode by frame", [
        ("--qpfile", "none", "Force frame types and QPs from a file, one `<frame> <type> [<qp>]` line each. Types are `I`, `K`, `i` and `-`."),
        ("--zones", "none", "QP or bitrate changes over frame ranges, as `start,end,q=N` or `start,end,b=F` joined by `/`."),
    ]),
    ("GOP and references", [
        ("--keyint", "`250`", "Key frame period in frames."),
        ("--open-gop", "on", "Code the periodic key frame as a CRA and keep the B-frames that straddle it. `--no-open-gop` writes an IDR there. A scene cut is an IDR either way."),
        ("--radl", "`0`", "How many of a CRA's leading pictures stay decodable when playback starts at that CRA."),
        ("--bframes", "`7`", "B-frames between anchors, 0 to 15. Above 7, `--rc-lookahead` must be at least one more."),
        ("--b-adapt", "`1`", "How the B run length is chosen: `0` fixed, `1` from motion, `4` from a bit estimate. `2` and `3` are refused."),
        ("--ref", "the preset's", "References per list, 1 to 5."),
        ("--rc-lookahead", "the preset's", "Lookahead depth in frames, 1 to 60. It's also the encoder's latency."),
        ("-w, --weightp / --no-weightp", "on from `medium` up", "Weighted prediction in P slices, which helps fades and lighting changes."),
        ("--weightb / --no-weightb", "off", "Weighted prediction in B slices."),
        ("--dup-threshold", "`0`", "How close a frame must be to a reference to be coded as a repeat. `0` takes exact repeats only."),
    ]),
    ("Block structure and quantization", [
        ("--ctu", "the preset's", "Coding tree block size: 16, 32 or 64."),
        ("--tu-inter-depth", "`2`", "Transform tree levels an inter block may use, 1 to 4. `--tu-intra-depth` is the same for intra."),
        ("--rdoq-level", "the preset's", "Rate-distortion optimized quantization: `0` off, `1` at the final encode of a block, `2` in every decision."),
    ]),
    ("Threads", [
        ("--threads", "`0`", "Worker threads. `0` starts one per online CPU. At `1`, two runs of a command write the same bytes."),
        ("--gop-parallel", "`0`", "Code a long input as segments on this many encoder instances and join them at key frames. The stream differs from the serial one."),
    ]),
    ("Shot-aware options", [
        ("--shot-window", "`0`", "Buffer up to this many frames so a whole shot can be planned before it's coded. `--crf` only, at most 300. It adds that many frames of latency."),
        ("--shot-stats", "none", "Write one JSON line of statistics per shot. The stream doesn't change."),
    ]),
    ("Film grain, denoising and filters", [
        ("--film-grain", "off", "Estimate the source's grain and describe it in an SEI message. A decoder that supports the message adds grain back. The value scales the estimate. 8-bit 4:2:0 only."),
        ("--film-grain-denoise", "`0`", "With `--film-grain`, also remove the grain from the picture the encoder codes."),
        ("--nr-intra", "`100` at `medium`, else `0`", "Noise reduction strength on intra coefficients, 0 to 2000."),
        ("--nr-inter", "`100` at `medium`, else `0`", "The same on inter coefficients."),
        ("--mcstf", "off", "Motion-compensated temporal filter on the source, for noisy material."),
        ("--mcstf-ref-range", "`2`", "Neighbor frames on each side the filter reads, 1 to 4."),
        ("--selective-mcstf", "off", "Skip the filter on pictures that measure clean."),
    ]),
    ("Stream structure and SEI", [
        ("--repeat-headers", "off", "Write the parameter sets at every key frame, for a receiver that joins mid-stream."),
        ("--aud", "off", "An access unit delimiter at the head of every picture."),
        ("--eos", "off", "An end-of-sequence NAL before each IDR after the first and at the end of the stream."),
        ("--eob", "off", "An end-of-bitstream NAL as the last NAL."),
        ("--info / --no-info", "on", "An SEI message naming the encoder, its version, its license and the options the run resolved."),
        ("--no-fingerprint", "fingerprint on", "Drop the stream fingerprint, the mark that survives when SEI messages are stripped."),
        ("--hash", "`0`", "A hash of each decoded picture in an SEI message: `1` MD5, `2` CRC, `3` checksum."),
        ("--idr-recovery-sei", "off", "A recovery point SEI at every key frame."),
        ("--pic-struct", "off", "Frame doubling or tripling in the picture timing SEI: `0`, `7` or `8`. Needs a buffer."),
        ("--hrd-concat", "off", "Mark the first key frame as spliced onto another stream. Needs a buffer."),
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
        ("--atc-sei", "none", "The transfer an SDR decoder should assume instead. An HLG stream uses it."),
        ("--dhdr10-info", "none", "HDR10+ dynamic metadata from a JSON file, one message per picture."),
        ("--dolby-vision-rpu", "none", "A Dolby Vision profile 8 RPU per picture, from a file. Needs `--dolby-vision-profile`."),
        ("--dolby-vision-profile", "none", "`8.1`, `8.2` or `8.4`. The color flags must match it."),
        ("--hdr-opt / --no-hdr-opt", "on for PQ and HLG", "The HDR tuning of `medium`, on or off. `--hdr10-opt` is the same switch."),
    ]),
    ("Reports", [
        ("--psnr", "off", "Report PSNR at the end of the run. The stream doesn't change."),
        ("--ssim", "off", "Report SSIM on luma at the end of the run."),
        ("--csv", "none", "Write run statistics to a CSV file."),
        ("--csv-log-level", "`0`", "`0` writes one row for the run, `1` adds a row per frame, `2` adds the rate control's columns."),
        ("--verbose, -v", "off", "Print the resolved configuration at the start of the encode."),
        ("--show-config", "off", "Print the resolved configuration and exit without encoding."),
        ("--version", "", "Print the version."),
    ]),
    ("Refused by name", [
        ("--intra-refresh", "refused", "Periodic intra refresh isn't built. The encoder says why and exits."),
        ("--scaling-list", "refused", "Quantizer scaling lists aren't built. The encoder says why and exits."),
    ]),
]

# spellings the capture names only inside another option's text
ALSO = ["--no-open-gop", "--tu-intra-depth", "--no-vui-timing", "--hdr10-opt"]

SEEN = {
    "-o": "9fa2f079",
    "--frames": "2028313c",
    "--output-depth": "fc6b46eb",
    "--seek": "80c0c496",
    "--input-res": "6c45ac4e",
    "--input-csp": "78321905",
    "--fps": "7fe1b4a2",
    "--input-depth": "8f21393b",
    "--preset": "f2d45fcf",
    "-t, --tune": "5e38813f",
    "--qp": "c48e970b",
    "--crf": "b587edfe",
    "--bitrate": "e0fd20ee",
    "--ref": "36b31a87",
    "--b-adapt": "bcc5f61f",
    "--rdoq-level": "b99a8980",
    "--tu-inter-depth": "1ebc417a",
    "--cbqpoffs": "9b6cbf84",
    "--crqpoffs": "36aab384",
    "--pcm": "b80b450b",
    "--lossless": "fba14d07",
    "--ctu": "d5aacf84",
    "--keyint": "7c632317",
    "--open-gop": "e213a08c",
    "--radl": "eb0b1895",
    "--bframes": "f5c6864d",
    "--aq": "536a83dd",
    "--rc-lookahead": "5aca27d0",
    "--cutree": "9ad9d8bd",
    "--dup-threshold": "ad8e60e5",
    "--threads": "3d001468",
    "--gop-parallel": "83de968a",
    "--dump-recon": "42ddf272",
    "--shot-window": "3b08e1e0",
    "--idr-recovery-sei": "6db9d089",
    "--pic-struct": "583e8780",
    "--hrd-concat": "2c4d6bc5",
    "--temporal-layers": "1891618b",
    "--intra-refresh": "8889d787",
    "--scaling-list": "22bac059",
    "--crf-min": "3f69e7a5",
    "--crf-max": "9748916e",
    "--qpmin": "2b9abd46",
    "--qpmax": "20aec13b",
    "--strict-cbr": "4ec9761b",
    "--max-ausize-factor": "992f57ed",
    "--qpfile": "93478aaf",
    "--zones": "cf2ef6c0",
    "--repeat-headers": "500706ae",
    "--aud": "19620ef9",
    "--eos": "c665c201",
    "--eob": "ff8dcf20",
    "--info / --no-info": "b0dd7561",
    "--no-fingerprint": "da3231bf",
    "--psnr": "503acaa3",
    "--ssim": "47e04971",
    "--csv": "00abed75",
    "--csv-log-level": "5b0c7c71",
    "--shot-stats": "d63df2b2",
    "--pass": "f40786d3",
    "--stats": "b7c193cc",
    "--level": "adce689d",
    "--high-tier": "7825e8fe",
    "--vui-timing": "598ed4b8",
    "--film-grain": "e0fccddb",
    "--film-grain-denoise": "b2d0be05",
    "--nr-intra": "989c3561",
    "--nr-inter": "3eb010d8",
    "--mcstf": "467ab6df",
    "--mcstf-ref-range": "361bc3ff",
    "--selective-mcstf": "6b1675e9",
    "--dither": "78464877",
    "--vbv-maxrate": "eab8c70b",
    "--vbv-bufsize": "336ab0b5",
    "--vbv-init": "9a5b8f45",
    "-w, --weightp / --no-weightp": "9bf3d14d",
    "--weightb / --no-weightb": "f31a3373",
    "--hash": "c0e11655",
    "--video-signal-type-preset": "2e0e5da2",
    "--colorprim": "e8e319e3",
    "--transfer": "82c65378",
    "--colormatrix": "42286714",
    "--range": "f3ff186e",
    "--videoformat": "6ce98d5a",
    "--chromaloc": "83a9b114",
    "--overscan": "7bb2f6e9",
    "--sar": "39a07a2b",
    "--display-window": "2eea5559",
    "--master-display": "b7438816",
    "--max-cll": "08fdf14c",
    "--hdr10 / --no-hdr10": "4fcf93d8",
    "--no-cll": "eeb16b18",
    "--atc-sei": "2ce5ed8f",
    "--dhdr10-info": "691e2e73",
    "--dolby-vision-rpu": "a60e7982",
    "--dolby-vision-profile": "02fc8aae",
    "--hdr-opt / --no-hdr-opt": "488523b9",
    "--verbose, -v": "2d5609f4",
    "--show-config": "be07a401",
    "--version": "0ff7e558",
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
    for name, rs in GROUPS:
        out += [f"## {name}", "", "| option | default | what it does |", "|---|---|---|"]
        for f, d, line in rs:
            spec = " ".join(x for x in (f, cap[f][0]) if x).replace("|", "\\|")
            out.append(f"| `{spec}` | {d} | {line} |")
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
    rows = {m: list(csv.DictReader(open(p))) for m, p in paths.items()}
    mac_md = paths["mac"][:-4] + ".md"
    sps = {m: bc.speed_groups(rs) for m, rs in rows.items()}
    lad = bc.ladders(mac_md)
    if not lad:
        sys.exit(f"{mac_md}: no BD ladder lines found")
    pl = bc.pooled(rows["mac"], mac_md)
    cm = {m: class_medians(rs) for m, rs in rows.items()}
    hdr = json.load(open(HDR))
    opts, build, nopt = options_tables()
    V = {
        "headline": bc.headline(pl),
        "pooled-table": bc.headline_table(pl),
        "quality-table": quality_table(pl, lad),
        "goal-table": bc.goal_table(sps),
        "hdr-table": hdr_table(hdr),
        "hdr-tuning-table": hdr_tuning_table(hdr),
        "hdr-ladder-table": hdr_ladder_table(hdr),
        "options": opts,
        "options-build": f"{nopt} entries from `{build}`",
    }
    charts = {"home-speed.svg": chart_home(cm, sum(v[1] for d in cm.values() for v in d.values())),
              "hdr-bd.svg": chart_hdr(hdr)}
    stale = []
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
        new = fill(rel, old, V)
        if new != old:
            stale.append(rel)
            if not a.check:
                with open(p, "w") as f:
                    f.write(new)
    print(f"headline: {bc.times(pl['median'])} faster over {pl['n']} cells ("
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
