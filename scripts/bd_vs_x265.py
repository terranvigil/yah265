#!/usr/bin/env python3
# Copyright (c) 2026, the yah265 authors
# SPDX-License-Identifier: GPL-2.0-or-later
"""BD-rate (VMAF-NEG) of one of our configurations against x265 medium at
MATCHED ACHIEVED BITRATE on the band ladders (scripts/ladders.json): the
quality leg the way yah264 reads it. ARGS = our extra flags; CLIPS, FRAMES,
JOBS as band_at_rate.py.

    ARGS='--aq 1 --cutree 1' python3 scripts/bd_vs_x265.py
    ARGS='--crf {q} --aq 1 --cutree 1' KNOB=crf python3 scripts/bd_vs_x265.py

X265PRESET names the reference rung (phase 13 C1): a rung of our ladder
is placed against the SAME-NAMED x265 rung, because that is the
comparison a user makes. The default is `medium` and it is spelled into
the template exactly where the literal always stood, so the default's
command string - and with it `bd_at_rate`'s encode memo key, which IS
that string - is character for character the one every cached reference
point was encoded under. The key gains the preset only when it is not
medium.

    ARGS='--preset faster --crf {q} --aq 1 --cutree 1' KNOB=crf \\
        X265PRESET=faster python3 scripts/bd_vs_x265.py

DEPTH=10 runs the same leg at Main10: our `build/n265` on the upshifted
corpus in `local/corpus10/` (scripts/upshift10.sh), x265 with
`--output-depth 10`, and both sides scored on 10-bit planes against the
10-bit source. SRCDIR/N265 override the two paths it picks.

A second column carries BD-rate on chroma PSNR off the same points: the metric
is luma-only, so that column is the only thing that says whether an arm bought
its luma number out of the chroma planes. The rule and the CIF-7 baseline are
in docs/instruments.md under the band tool.

`--extra` (or EXTRA=1) is phase 13 B4's reporting mode: the SAME four
matched-byte rungs read by six metrics instead of one - VMAF-NEG (the band's),
VMAF (the default model, enhancement credit included), PSNR-Y, SSIM-Y,
SSIM-U/V and MS-SSIM - with a `disagree` column that names every clip where
VMAF-NEG and PSNR-Y point opposite ways and says which side SSIM-Y takes. It
adds a decode and two filter runs per point and no term to either memo key,
so the default columns reproduce to the digit with it on or off.
"""
import json, os, re, statistics, subprocess, sys
from concurrent.futures import ThreadPoolExecutor
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
HERE = os.path.join(ROOT, "scripts")
FRAMES = int(os.environ.get("FRAMES", "120"))
# DEPTH=10 puts the whole leg at Main10: our binary is the 10-bit one, the
# source is the upshifted corpus scripts/upshift10.sh writes, x265 is the
# same brew build asked for a Main10 stream, and the metrics read 10-bit
# planes on both sides (bd_at_rate --depth). docs/instruments.md, "The band
# at 10 bits". Nothing about the 8-bit leg moves.
DEPTH = int(os.environ.get("DEPTH", "8"))
SRCDIR = os.environ.get("SRCDIR", "tests/corpus" if DEPTH == 8
                        else "local/corpus10")
N = os.environ.get("N265", "build/n265")   # one binary, both depths (item 5a)
X = os.environ.get("X265", "x265")
ARGS = os.environ.get("ARGS", "--aq 1 --cutree 1")
KNOB = os.environ.get("KNOB", "qp")            # qp | crf: which knob bd_at_rate bisects
JOBS = int(os.environ.get("JOBS", "4"))
# `--extra` (or EXTRA=1) is phase 13 B4's reporting mode: the same four
# matched-byte rungs, read by six metrics instead of one, so a clip where
# the band's metric and PSNR disagree says so on one line. It changes
# nothing about the encodes, nothing about the ladders and nothing about
# either memo key - the default mode's numbers reproduce to the digit with
# it on or off, which is the gate this mode shipped under.
EXTRA = "--extra" in sys.argv or os.environ.get("EXTRA", "0") != "0"
ladders = json.load(open(os.path.join(HERE, "ladders.json")))
sel = os.environ.get("CLIPS", "")
clips = sel.split(",") if sel else list(ladders)
def fps_of(clip):
    with open(os.path.join(ROOT, SRCDIR, clip + ".y4m"), "rb") as f:
        hdr = f.readline().decode("latin-1")
    m = re.search(r" F(\d+):(\d+)", hdr)
    return int(m.group(1)) / int(m.group(2)) if m else 30.0
def targets_of(clip):
    secs = FRAMES / fps_of(clip)
    return [int(kbps * 1000.0 / 8.0 * secs) for kbps in ladders[clip]]
ours = f"{N} {{src}} --frames {FRAMES} " + (ARGS if "{q}" in ARGS else f"--{KNOB} {{q}} {ARGS}") + " -o {out}"
# x265 reads the depth of a y4m off its header, so --input-depth is a
# no-op here and is written anyway to say what the arm is; --output-depth
# is the one that picks the Main10 encoder out of the 8+10+12 brew build.
xdepth = "" if DEPTH == 8 else f" --input-depth {DEPTH} --output-depth {DEPTH}"
XPRESET = os.environ.get("X265PRESET", "medium")
ref = (f"{X} --preset {XPRESET} --crf {{q}} --frames {FRAMES} --pools none "
       f"--frame-threads 1 --log-level none{xdepth} -o {{out}} {{src}}")
def run(clip):
    cmd = ["python3", "scripts/bd_at_rate.py", "--clip", clip, "--frames", str(FRAMES),
           "--targets", ",".join(str(t) for t in targets_of(clip)),
           "--srcdir", SRCDIR, "--depth", str(DEPTH),
           "--label-a", "ours", "--label-b", "x265", "--a", ours, "--b", ref]
    if EXTRA:
        cmd.append("--extra")
    r = subprocess.run(cmd, cwd=ROOT, capture_output=True, text=True)
    return clip, r.stdout + r.stderr
FGC = int(os.environ.get("FGC", "0"))
print(f"depth: {DEPTH}  src: {SRCDIR}  FGC: {'synthesised' if FGC else 'not applied'}"
      f"\nours: {ours}\nx265: {ref}\n")
res = {}
chroma = {}
luma = {}
# B4's six: the tag bd_at_rate prints, the column head, and what it means
# when the number is NEGATIVE (always: ours spends fewer bits at the same
# reading of that metric).
EXTRA_COLS = [("bdVMAF", "VMAF"), ("bdPSNRY", "PSNR-Y"),
              ("bdSSIMY", "SSIM-Y"), ("bdSSIMU", "SSIM-U"),
              ("bdSSIMV", "SSIM-V"), ("bdMSSSIM", "MS-SSIM")]
extra = {tag: {} for tag, _ in EXTRA_COLS}
with ThreadPoolExecutor(max_workers=JOBS) as ex:
    for clip, out in ex.map(run, clips):
        print(out.rstrip()); m = re.search(r"BD-rate\(VMAF-NEG\) ours vs x265: ([+-][\d.]+)%", out)
        if m: res[clip] = float(m.group(1))
        # The chroma guard (docs/phase7-psy-design.md M3): VMAF is luma-only,
        # so this column is what says whether an arm bought its luma number
        # out of the chroma planes. A reading, never the flip.
        mc = re.search(r"chromaPSNR ([+-][\d.]+)%", out)
        if mc: chroma[clip] = float(mc.group(1))
        # Luma PSNR off the same points: the second metric a grain arm is
        # argued on, since VMAF-NEG's reading of synthesised samples is
        # itself the open question (phase 12 item 3).
        my = re.search(r"lumaPSNR ([+-][\d.]+)%", out)
        if my: luma[clip] = float(my.group(1))
        if EXTRA:
            for tag, _ in EXTRA_COLS:
                # `sat` (the overlap window collapsed) is deliberately not
                # matched here: the cell stays empty rather than carrying a
                # number the instrument does not stand behind. `~` marks a
                # thin window and rides along with the value.
                me = re.search(rf"\b{tag} ([+-][\d.]+)%(~?)", out)
                if me: extra[tag][clip] = (float(me.group(1)), me.group(2))
v = list(res.values())
if v:
    tag = "" if DEPTH == 8 else f" (Main10, {DEPTH}-bit)"
    print(f"\n=== BD-rate(VMAF-NEG) ours vs x265 {XPRESET} at matched achieved bitrate{tag} ===")
    for c in clips:
        if c in res:
            ys = f"   lumaPSNR {luma[c]:+7.2f}%" if c in luma else ""
            cs = f"   chromaPSNR {chroma[c]:+7.2f}%" if c in chroma else ""
            print(f"  {c:<18} {res[c]:+7.2f}%{ys}{cs}")
    print(f"  median {statistics.median(v):+.2f}%   mean {statistics.mean(v):+.2f}%   worst {max(v):+.2f}%   ahead {sum(1 for x in v if x < 0)}/{len(v)}")
    yv = [luma[c] for c in clips if c in luma]
    if yv:
        print(f"  lumaPSNR     median {statistics.median(yv):+.2f}%   mean {statistics.mean(yv):+.2f}%   worst {max(yv):+.2f}%   ahead {sum(1 for x in yv if x < 0)}/{len(yv)}")
    cv = [chroma[c] for c in clips if c in chroma]
    if cv:
        print(f"  chromaPSNR   median {statistics.median(cv):+.2f}%   mean {statistics.mean(cv):+.2f}%   worst {max(cv):+.2f}%   ahead {sum(1 for x in cv if x < 0)}/{len(cv)}")
if EXTRA and res:
    have = [(tag, head) for tag, head in EXTRA_COLS if extra[tag]]
    print("\n=== BD-rate by metric, same four matched-byte rungs "
          "(negative = ours spends fewer bits at equal reading) ===")
    print("    `sat` = the metric's overlap window collapsed on this clip, "
          "no reading;\n    `~` = thin window, read the sign and not the "
          "magnitude.")
    print(f"  {'clip':<18} {'VMAF-NEG':>10}" +
          "".join(f" {h:>10}" for _, h in have) + "   disagree")
    for c in clips:
        if c not in res:
            continue
        row = f"  {c:<18} {res[c]:>+10.2f}"
        for tag, _ in have:
            e = extra[tag].get(c)
            row += f" {e[0]:>+9.2f}{e[1] or ' '}" if e else f" {'sat':>10}"
        # The B4 question in one column: the band's metric says we are
        # ahead (negative) and PSNR-Y says we are behind (positive), or
        # the other way round. SSIM-Y is the tie-break and is named when
        # it takes a side.
        py = extra["bdPSNRY"].get(c)
        sy = extra["bdSSIMY"].get(c)
        flag = ""
        if py is not None and (res[c] < 0) != (py[0] < 0):
            flag = "NEG/PSNR-Y"
            if sy is not None:
                flag += "; SSIM-Y with " + ("NEG" if (sy[0] < 0) == (res[c] < 0)
                                            else "PSNR-Y")
        print(row + ("   " + flag if flag else ""))
    srow = f"  {'median':<18} {statistics.median(list(res.values())):>+10.2f}"
    arow = f"  {'ahead':<18} {sum(1 for x in res.values() if x < 0):>7}/{len(res)}"
    for tag, _ in have:
        vals = [extra[tag][c][0] for c in clips if c in extra[tag]]
        srow += f" {statistics.median(vals):>+10.2f}"
        arow += f" {sum(1 for x in vals if x < 0):>7}/{len(vals)}"
    print(srow)
    print(arow)
