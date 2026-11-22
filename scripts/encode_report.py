#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""One self-contained HTML page per encode, from y265's own stats.

    y265 in.y4m -o out.hevc --report out.html

runs this script for you. By hand:

    y265 in.y4m -o out.hevc --frame-stats f.jsonl --show-config 2> cfg.txt
    encode_report.py --frames f.jsonl --config cfg.txt --stream out.hevc \\
        -o out.html

(--show-config exits before encoding, so capture it from a second run or
use --verbose on the real one.)

The page shows bitrate and QP over time with the frame types, the VBV
fill when a buffer is set, the shots with a thumbnail each, what the
encoder detected (scene cuts, screen content, handheld jitter, dark and
flat frames) and the resolved configuration. Charts are inline SVG, the
thumbnails are inline JPEG, and nothing loads from the network.

Thumbnails need ffmpeg on PATH and the stream; without either the page
is written without them. Only the standard library is used.
"""

import argparse
import base64
import html
import json
import math
import os
import re
import shutil
import subprocess
import sys
import tempfile

TYPES = ("I", "P", "B")
SHOT_KEYS = ("scene", "forced", "qpfile")
MAX_POINTS = 1600
# a frame reads dark when this share of its 16x16 blocks is dark and smooth
DARK_FRAME = 0.25
# a shot carries a detection's badge when this share of its frames has it
BADGE_SHARE = 0.2


# ---- input ------------------------------------------------------------

def load_frames(path):
    rows = []
    with open(path) as f:
        for ln, line in enumerate(f, 1):
            line = line.strip()
            if not line:
                continue
            try:
                r = json.loads(line)
            except ValueError as ex:
                raise SystemExit(f"{path}:{ln}: not JSON: {ex}")
            if "frame" not in r or "bytes" not in r:
                raise SystemExit(f"{path}:{ln}: no frame or bytes key")
            rows.append(r)
    if not rows:
        raise SystemExit(f"{path}: no frames")
    rows.sort(key=lambda r: r["frame"])
    return rows


CFG_RE = re.compile(r"^y265 \[info\]:\s?(\s*)(.*?)\s+:\s(.*)$")


def load_config(path):
    """Rows of `y265 [info]: key : value`, as (indent, key, value)."""
    out = []
    if not path or not os.path.exists(path):
        return out
    with open(path, errors="replace") as f:
        for line in f:
            m = CFG_RE.match(line.rstrip("\n"))
            if m:
                out.append((len(m.group(1)) > 0, m.group(2).strip(),
                            m.group(3).strip()))
    return out


def config_facts(cfg):
    """fps, size and VBV buffer out of the config rows."""
    facts = {"fps": None, "w": None, "h": None, "bufsize": None,
             "maxrate": None}
    for _, k, v in cfg:
        if k == "input":
            m = re.search(r"(\d+)x(\d+)", v)
            if m:
                facts["w"], facts["h"] = int(m.group(1)), int(m.group(2))
            m = re.search(r"([\d.]+)(?:/([\d.]+))?\s*fps", v)
            if m:
                num = float(m.group(1))
                den = float(m.group(2)) if m.group(2) else 1.0
                if den > 0:
                    facts["fps"] = num / den
        elif k == "vbv":
            m = re.search(r"maxrate ([\d.]+) kbit/s", v)
            if m:
                facts["maxrate"] = float(m.group(1))
            m = re.search(r"bufsize ([\d.]+) kbit", v)
            if m:
                facts["bufsize"] = float(m.group(1))
    return facts


# ---- analysis ---------------------------------------------------------

def runs(flags):
    """[start, end) runs where flags[i] is true."""
    out, start = [], None
    for i, f in enumerate(flags):
        if f and start is None:
            start = i
        elif not f and start is not None:
            out.append((start, i))
            start = None
    if start is not None:
        out.append((start, len(flags)))
    return out


def has_detections(frames):
    return any("screen" in r for r in frames)


def shot_starts(frames):
    """A shot opens at the first frame and at every scene cut the
    detector saw, coded as an intra picture or not. Stats from before
    the detection fields fall back to every I frame."""
    if has_detections(frames):
        s = [i for i, r in enumerate(frames)
             if i == 0 or r.get("scenecut") or r.get("key") in SHOT_KEYS]
    else:
        s = [i for i, r in enumerate(frames) if i == 0 or r["type"] == "I"]
    return s


def build_shots(frames, fps):
    starts = shot_starts(frames)
    shots = []
    for k, a in enumerate(starts):
        b = starts[k + 1] if k + 1 < len(starts) else len(frames)
        seg = frames[a:b]
        nbytes = sum(r["bytes"] for r in seg)
        n = len(seg)
        why = frames[a].get("key") or (
            "scene" if frames[a].get("scenecut") else "first" if a == 0
            else "")
        det = {}
        for key in ("screen", "jitter", "flat"):
            c = sum(1 for r in seg if r.get(key))
            if c / n >= BADGE_SHARE:
                det[key] = c / n
        c = sum(1 for r in seg if r.get("dark", 0) >= DARK_FRAME)
        if c / n >= BADGE_SHARE:
            det["dark"] = c / n
        shots.append({
            "first": frames[a]["frame"], "last": frames[b - 1]["frame"],
            "i0": a, "i1": b, "frames": n, "bytes": nbytes,
            "kbps": nbytes * 8 * fps / n / 1000.0,
            "qp": sum(r["qp"] for r in seg) / n,
            "why": why, "det": det, "thumb": None,
            "mid": frames[(a + b - 1) // 2]["frame"],
        })
    return shots


def rolling_kbps(frames, fps):
    """Bitrate over the one-second window ending at each frame. The first
    second reads the first full window, so the opening intra picture
    doesn't read as a one-frame rate."""
    n = len(frames)
    win = max(1, min(n, int(round(fps))))
    pre = [0]
    for r in frames:
        pre.append(pre[-1] + r["bytes"])
    out = []
    for i in range(n):
        a = max(0, min(i - win + 1, n - win))
        out.append((pre[a + win] - pre[a]) * 8 * fps / win / 1000.0)
    return out


def bucket(values, n_out):
    """Down to at most n_out points by mean, as (x_center, value)."""
    n = len(values)
    if n <= n_out:
        return [(i + 0.5, v) for i, v in enumerate(values)]
    out = []
    for k in range(n_out):
        a = k * n // n_out
        b = max(a + 1, (k + 1) * n // n_out)
        seg = values[a:b]
        out.append(((a + b) / 2.0, sum(seg) / len(seg)))
    return out


def nice_max(v):
    if v <= 0:
        return 1.0
    e = 10 ** math.floor(math.log10(v))
    for m in (1, 1.2, 1.5, 2, 2.5, 3, 4, 5, 6, 8, 10):
        if m * e >= v:
            return m * e
    return 10 * e


def fmt_num(v):
    if v >= 10000:
        return f"{v / 1000:.0f}k"
    if v >= 1000:
        return f"{v / 1000:.1f}k".replace(".0k", "k")
    if v == int(v):
        return f"{int(v)}"
    return f"{v:.1f}"


def fmt_time(s):
    if s >= 3600:
        return f"{int(s // 3600)}:{int(s % 3600 // 60):02d}:{s % 60:04.1f}"
    if s >= 60:
        return f"{int(s // 60)}:{s % 60:04.1f}"
    return f"{s:.1f}s"


def fmt_bytes(b):
    for unit, d in (("GB", 1e9), ("MB", 1e6), ("kB", 1e3)):
        if b >= d:
            return f"{b / d:.2f} {unit}"
    return f"{b} B"


# ---- thumbnails ------------------------------------------------------

def thumbnails(stream, shots, width, max_n):
    """One JPEG per shot (its middle frame), decoded from the stream.
    Returns how many were made; a missing ffmpeg or stream makes none."""
    ff = shutil.which("ffmpeg")
    if not ff or not stream or not os.path.exists(stream) or not shots:
        return 0
    pick = shots
    if len(shots) > max_n:
        step = len(shots) / max_n
        pick = [shots[int(k * step)] for k in range(max_n)]
    want = sorted({s["mid"] for s in pick})
    expr = "+".join(f"eq(n\\,{n})" for n in want)
    tmp = tempfile.mkdtemp(prefix="y265-thumbs-")
    try:
        base = [ff, "-nostdin", "-v", "error", "-i", stream, "-vf",
                f"select={expr},scale={width}:-2"]
        tail = ["-q:v", "6", os.path.join(tmp, "t%05d.jpg")]
        err = ""
        # -fps_mode is ffmpeg 5.1 on; -vsync is the spelling before it
        for sync in (["-fps_mode", "passthrough"], ["-vsync", "0"]):
            try:
                subprocess.run(base + sync + tail, check=True, timeout=600,
                               stdout=subprocess.DEVNULL,
                               stderr=subprocess.PIPE)
                err = ""
                break
            except subprocess.CalledProcessError as ex:
                err = ex.stderr.decode(errors="replace").strip()
            except (subprocess.SubprocessError, OSError) as ex:
                err = str(ex)
                break
        if err:
            print(f"encode_report: no thumbnails: {err}", file=sys.stderr)
            return 0
        files = sorted(os.listdir(tmp))
        by_frame = {}
        for n, name in zip(want, files):
            with open(os.path.join(tmp, name), "rb") as f:
                by_frame[n] = base64.b64encode(f.read()).decode("ascii")
        made = 0
        for s in pick:
            if s["mid"] in by_frame:
                s["thumb"] = by_frame[s["mid"]]
                made += 1
        return made
    finally:
        shutil.rmtree(tmp, ignore_errors=True)


# ---- SVG pieces --------------------------------------------------------
#
# Each plot is an SVG stretched to its box (preserveAspectRatio none) in
# frame units on x and 0..100 on y, with non-scaling strokes, so the
# lines stay 2px at any width. Axis labels are HTML placed by percent,
# which keeps the text readable on a phone.

H = 100.0


def svg_open(n, cls="plot"):
    return (f'<svg class="{cls}" viewBox="0 0 {n} {H:g}" '
            f'preserveAspectRatio="none" aria-hidden="true">')


def path_line(points, ymax, cls):
    if not points:
        return ""
    d = []
    for k, (x, v) in enumerate(points):
        y = H - min(v, ymax) / ymax * H
        d.append(f"{'M' if k == 0 else 'L'}{x:.2f} {y:.2f}")
    return f'<path class="{cls}" d="{" ".join(d)}"/>'


def path_area(points, ymax, n, cls):
    if not points:
        return ""
    d = [f"M{points[0][0]:.2f} {H:g}"]
    for x, v in points:
        d.append(f"L{x:.2f} {H - min(v, ymax) / ymax * H:.2f}")
    d.append(f"L{points[-1][0]:.2f} {H:g} Z")
    return f'<path class="{cls}" d="{" ".join(d)}"/>'


def grid(ymax, ticks):
    out = []
    for t in ticks:
        y = H - t / ymax * H
        out.append(f'<line class="grid" x1="0" x2="100%" y1="{y:.2f}" '
                   f'y2="{y:.2f}"/>')
    return "".join(out)


def y_labels(ymax, ticks, unit=""):
    out = []
    for t in ticks:
        out.append(f'<span style="bottom:{t / ymax * 100:.2f}%">'
                   f'{fmt_num(t)}{unit}</span>')
    return f'<div class="ylab">{"".join(out)}</div>'


def x_labels(n, fps):
    dur = n / fps
    raw = dur / 6
    steps = [0.5, 1, 2, 5, 10, 15, 30, 60, 120, 300, 600, 900, 1800, 3600]
    step = next((s for s in steps if s >= raw), steps[-1])
    out = []
    t = 0.0
    while t <= dur + 1e-9:
        end = ' class="end"' if t > 0 and dur - t < step / 2 else ""
        out.append(f'<span{end} style="left:{t * fps / n * 100:.2f}%">'
                   f'{fmt_time(t)}</span>')
        t += step
    return f'<div class="xlab">{"".join(out)}</div>'


def ticks_for(ymax):
    return [ymax * k / 4 for k in range(5)]


def chart(title, legend, body_svg, ymax, n, fps, unit="", note=""):
    ticks = ticks_for(ymax)
    leg = "".join(f'<span class="lg"><i class="{c}"></i>{html.escape(t)}'
                  f'</span>' for c, t in legend)
    return (f'<section class="card chart-card"><div class="chead">'
            f'<h2>{html.escape(title)}</h2><div class="legend">{leg}</div>'
            f'</div>{note}<div class="chart">{y_labels(ymax, ticks, unit)}'
            f'<div class="area" data-n="{n}">{svg_open(n)}'
            f'{grid(ymax, ticks)}{body_svg}</svg>'
            f'<div class="cross"></div></div></div>'
            f'{x_labels(n, fps)}</section>')


def ticks_svg(xs, cls, y0=0.0, y1=H):
    return "".join(f'<line class="{cls}" x1="{x + 0.5:g}" x2="{x + 0.5:g}" '
                   f'y1="{y0:g}" y2="{y1:g}"/>' for x in xs)


def band_row(label, runs_, cls, n, ticks_at=None, title=""):
    body = "".join(f'<rect class="{cls}" x="{a}" y="15" width="{b - a}" '
                   f'height="70"/>' for a, b in runs_)
    if ticks_at:
        body += ticks_svg(ticks_at, cls + " tk")
    t = f' title="{html.escape(title)}"' if title else ""
    return (f'<div class="brow"><div class="blab"{t}>{html.escape(label)}'
            f'</div><div class="area" data-n="{n}">{svg_open(n, "band")}'
            f'{body}</svg><div class="cross"></div></div></div>')


# ---- the page --------------------------------------------------------

CSS = """
:root{color-scheme:light;--page:#f9f9f7;--surface:#fcfcfb;--ink:#0b0b0b;
--ink2:#52514e;--muted:#898781;--grid:#e1e0d9;--axis:#c3c2b7;
--ring:rgba(11,11,11,.10);--s1:#2a78d6;--s2:#eb6834;--s3:#1baf7a;
--s4:#eda100;--s5:#e87ba4;--s6:#008300;--s7:#6250d6;--s8:#e34948;
--wash:rgba(42,120,214,.10)}
@media (prefers-color-scheme:dark){:root:not([data-theme="light"]){
color-scheme:dark;--page:#0d0d0d;--surface:#1a1a19;--ink:#f0efec;
--ink2:#c3c2b7;--muted:#898781;--grid:#2c2c2a;--axis:#383835;
--ring:rgba(255,255,255,.10);--s1:#3987e5;--s2:#d95926;--s3:#199e70;
--s4:#c98500;--s5:#d55181;--s6:#0ca30c;--s7:#9085e9;--s8:#e66767;
--wash:rgba(57,135,229,.16)}}
:root[data-theme="dark"]{color-scheme:dark;--page:#0d0d0d;--surface:#1a1a19;
--ink:#f0efec;--ink2:#c3c2b7;--muted:#898781;--grid:#2c2c2a;--axis:#383835;
--ring:rgba(255,255,255,.10);--s1:#3987e5;--s2:#d95926;--s3:#199e70;
--s4:#c98500;--s5:#d55181;--s6:#0ca30c;--s7:#9085e9;--s8:#e66767;
--wash:rgba(57,135,229,.16)}
*{box-sizing:border-box}
body{margin:0;background:var(--page);color:var(--ink);
font:14px/1.45 system-ui,-apple-system,"Segoe UI",sans-serif}
main{max-width:1100px;margin:0 auto;padding:20px 16px 48px}
header{display:flex;flex-wrap:wrap;align-items:baseline;gap:8px 16px;
justify-content:space-between;margin-bottom:16px}
h1{font-size:20px;margin:0;word-break:break-all}
h2{font-size:15px;margin:0}
.sub{color:var(--ink2);font-size:13px}
button.theme{font:inherit;font-size:12px;color:var(--ink2);
background:var(--surface);border:1px solid var(--ring);border-radius:6px;
padding:4px 10px;cursor:pointer}
.card{background:var(--surface);border:1px solid var(--ring);
border-radius:10px;padding:14px 16px;margin:0 0 14px}
.tiles{display:grid;grid-template-columns:repeat(auto-fit,minmax(130px,1fr));
gap:10px;margin-bottom:14px}
.tile{background:var(--surface);border:1px solid var(--ring);
border-radius:10px;padding:10px 12px}
.tile b{display:block;font-size:20px;font-variant-numeric:tabular-nums}
.tile span{color:var(--ink2);font-size:12px}
.chead{display:flex;flex-wrap:wrap;gap:4px 14px;align-items:baseline;
justify-content:space-between;margin-bottom:8px}
.legend{display:flex;flex-wrap:wrap;gap:4px 12px;font-size:12px;
color:var(--ink2)}
.lg i{display:inline-block;width:12px;height:3px;border-radius:2px;
vertical-align:middle;margin-right:5px;background:currentColor}
.note{color:var(--muted);font-size:12px;margin:-4px 0 6px}
.chart{position:relative;height:170px;margin-left:44px}
.ylab{position:absolute;left:-44px;top:0;bottom:0;width:40px}
.ylab span{position:absolute;right:0;transform:translateY(50%);
font-size:11px;color:var(--muted);font-variant-numeric:tabular-nums}
.xlab{position:relative;height:18px;margin:4px 0 0 44px}
.xlab span{position:absolute;transform:translateX(-50%);font-size:11px;
color:var(--muted);white-space:nowrap}
.xlab span:first-child{transform:none}
.xlab span.end{transform:translateX(-100%)}
.area{position:absolute;inset:0}
.brow .area{position:relative;inset:auto;flex:1;height:18px}
svg.plot,svg.band{position:absolute;inset:0;width:100%;height:100%;
overflow:visible}
svg *{vector-effect:non-scaling-stroke}
.grid{stroke:var(--grid);stroke-width:1}
.base{stroke:var(--axis);stroke-width:1}
.line1{fill:none;stroke:var(--s1);stroke-width:2;stroke-linejoin:round}
.line2{fill:none;stroke:var(--s7);stroke-width:2;stroke-linejoin:round}
.ref{stroke:var(--muted);stroke-width:1;stroke-dasharray:4 4;fill:none}
.area1{fill:var(--s1);opacity:.18}
.tI{stroke:var(--s2);stroke-width:2}
.tP{stroke:var(--s1);stroke-width:1}
.tB{stroke:var(--s3);stroke-width:1}
.cross{position:absolute;top:0;bottom:0;width:0;border-left:1px solid
var(--ink2);opacity:0;pointer-events:none}
.brow{display:flex;align-items:center;gap:8px;margin:5px 0}
.blab{width:112px;flex:none;font-size:12px;color:var(--ink2);
text-align:right}
.band rect{opacity:.85}
.tk{stroke-width:2}
.bScene{fill:var(--s8);stroke:var(--s8)}
.bKey{fill:var(--s2);stroke:var(--s2)}
.bScreen{fill:var(--s7)}
.bJit{fill:var(--s4)}
.bDark{fill:var(--ink2)}
.bFlat{fill:var(--s5)}
.bShotA{fill:var(--s1);opacity:.55!important}
.bShotB{fill:var(--s3);opacity:.55!important}
.lgI{color:var(--s2)}.lgP{color:var(--s1)}.lgB{color:var(--s3)}
.lg1{color:var(--s1)}.lg2{color:var(--s7)}.lgA{color:var(--s1);opacity:.4}
.lgR{color:var(--muted)}
.shots{display:flex;gap:10px;overflow-x:auto;padding-bottom:6px;
scroll-snap-type:x proximity}
.shot{flex:none;width:176px;border:1px solid var(--ring);border-radius:8px;
padding:8px;scroll-snap-align:start;background:var(--page)}
.shot img,.shot .noimg{display:block;width:160px;height:90px;
object-fit:cover;border-radius:4px;background:var(--grid)}
.shot .noimg{display:flex;align-items:center;justify-content:center;
color:var(--muted);font-size:11px}
.shot h3{font-size:13px;margin:6px 0 2px}
.shot p{margin:0;font-size:12px;color:var(--ink2);
font-variant-numeric:tabular-nums}
.badges{display:flex;flex-wrap:wrap;gap:4px;margin-top:4px}
.badge{font-size:11px;border:1px solid var(--ring);border-radius:999px;
padding:0 7px;color:var(--ink2)}
table{border-collapse:collapse;width:100%;font-size:13px;
font-variant-numeric:tabular-nums}
th,td{text-align:left;padding:4px 8px;border-bottom:1px solid var(--grid);
vertical-align:top}
th{color:var(--ink2);font-weight:600}
td.num,th.num{text-align:right}
.tblwrap{overflow-x:auto}
details summary{cursor:pointer;color:var(--ink2);margin:6px 0}
#tip{position:fixed;z-index:9;pointer-events:none;background:var(--surface);
border:1px solid var(--ring);border-radius:8px;padding:8px 10px;
font-size:12px;box-shadow:0 4px 14px rgba(0,0,0,.18);display:none;
max-width:240px}
#tip b{font-size:13px}
#tip .r{display:flex;justify-content:space-between;gap:14px}
#tip .r span:first-child{color:var(--ink2)}
.det-list{margin:6px 0 0;padding-left:18px;color:var(--ink2);font-size:13px}
@media (max-width:560px){.blab{width:76px;font-size:11px}
.chart{height:140px}}
"""

JS = r"""
(function(){
var D=JSON.parse(document.getElementById('data').textContent);
var tip=document.getElementById('tip');
var areas=[].slice.call(document.querySelectorAll('.area'));
function at(i){
  var r=D.f[i];
  var h='<b>Frame '+r[0]+'</b> <span style="color:var(--ink2)">'
       +(r[0]/D.fps).toFixed(2)+'s</span>';
  function row(k,v){h+='<div class="r"><span>'+k+'</span><span>'+v+
       '</span></div>';}
  row('Type',r[1]+(r[6]?' ('+r[6]+')':''));
  row('Size',r[2].toLocaleString()+' B');
  row('QP',r[3]);
  row('Rate, 1 s',Math.round(D.kbps[i]).toLocaleString()+' kbit/s');
  if(r[4]!==null)row('Buffer',r[4]+'%');
  if(r[5])row('Detected',r[5]);
  return h;
}
function show(e,area){
  var b=area.getBoundingClientRect();
  var x=Math.max(0,Math.min(b.width-1,e.clientX-b.left));
  var i=Math.min(D.f.length-1,Math.floor(x/b.width*D.f.length));
  var px=(i+0.5)/D.f.length;
  areas.forEach(function(a){var c=a.querySelector('.cross');
    if(c){c.style.left=(px*100)+'%';c.style.opacity=.6;}});
  tip.innerHTML=at(i);tip.style.display='block';
  var tx=e.clientX+14,ty=e.clientY+14;
  var tw=tip.offsetWidth,th=tip.offsetHeight;
  if(tx+tw>innerWidth-8)tx=e.clientX-tw-14;
  if(ty+th>innerHeight-8)ty=e.clientY-th-14;
  tip.style.left=Math.max(8,tx)+'px';tip.style.top=Math.max(8,ty)+'px';
}
function hide(){tip.style.display='none';
  areas.forEach(function(a){var c=a.querySelector('.cross');
    if(c)c.style.opacity=0;});}
areas.forEach(function(a){
  a.addEventListener('pointermove',function(e){show(e,a);});
  a.addEventListener('pointerleave',hide);
});
var btn=document.getElementById('theme');
function cur(){var t=document.documentElement.getAttribute('data-theme');
  if(t)return t;return matchMedia('(prefers-color-scheme: dark)').matches
  ?'dark':'light';}
function label(){btn.textContent=cur()==='dark'?'Light theme':'Dark theme';}
try{var s=localStorage.getItem('y265-report-theme');
  if(s)document.documentElement.setAttribute('data-theme',s);}catch(e){}
label();
btn.addEventListener('click',function(){
  var n=cur()==='dark'?'light':'dark';
  document.documentElement.setAttribute('data-theme',n);
  try{localStorage.setItem('y265-report-theme',n);}catch(e){}
  label();});
})();
"""

DET_NAMES = {"screen": "screen", "jitter": "handheld", "flat": "flat",
             "dark": "dark"}


def frame_det(r):
    out = []
    if r.get("scenecut"):
        out.append("scene cut (" + r["scenecut"] + ")")
    if r.get("screen"):
        out.append("screen")
    if r.get("jitter"):
        out.append("handheld")
    if r.get("flat"):
        out.append("flat")
    if r.get("dark", 0) >= DARK_FRAME:
        out.append(f"dark {r['dark'] * 100:.0f}%")
    return ", ".join(out)


def render(frames, cfg, title, fps_override=None, stream=None,
           thumb_width=160, max_thumbs=48):
    facts = config_facts(cfg)
    fps = fps_override or facts["fps"]
    fps_note = ""
    if not fps:
        fps = 25.0
        fps_note = "Frame rate unknown, timed at 25 fps."
    n = len(frames)
    gaps = [i for i, r in enumerate(frames) if r["frame"] != i]
    shots = build_shots(frames, fps)
    made = thumbnails(stream, shots, thumb_width, max_thumbs)
    kbps = rolling_kbps(frames, fps)
    inst = [r["bytes"] * 8 * fps / 1000.0 for r in frames]
    total = sum(r["bytes"] for r in frames)
    dur = n / fps
    avg_kbps = total * 8 / dur / 1000.0 if dur > 0 else 0.0
    qps = [r["qp"] for r in frames]
    det_ok = has_detections(frames)

    parts = []
    # tiles
    keys = sum(1 for r in frames if r["type"] == "I")
    tiles = [
        (fmt_time(dur), f"{n:,} frames at {fps:.3g} fps"),
        (f"{avg_kbps:,.0f}", "kbit/s average"),
        (fmt_bytes(total), "stream size"),
        (f"{sum(qps) / n:.1f}", "mean QP"),
        (f"{len(shots)}", "shots"),
        (f"{keys}", "intra pictures"),
    ]
    if facts["w"]:
        tiles.insert(1, (f"{facts['w']}x{facts['h']}", "picture size"))
    parts.append('<div class="tiles">' + "".join(
        f'<div class="tile"><b>{html.escape(a)}</b>'
        f'<span>{html.escape(b)}</span></div>' for a, b in tiles) +
        '</div>')

    # bitrate
    pts_i = bucket(inst, MAX_POINTS)
    pts_r = bucket(kbps, MAX_POINTS)
    top = max(max(v for _, v in pts_r), avg_kbps)
    # the per-frame area is capped at the chart top: an I frame can be
    # fifty times its neighbours and would flatten everything else
    ymax = nice_max(top * 1.15)
    if facts["maxrate"]:
        ymax = max(ymax, nice_max(facts["maxrate"] * 1.1))
    body = path_area(pts_i, ymax, n, "area1") + path_line(pts_r, ymax,
                                                          "line1")
    legend = [("lgA", "per frame"), ("lg1", "1 s window")]
    if facts["maxrate"]:
        y = H - facts["maxrate"] / ymax * H
        body += (f'<line class="ref" x1="0" x2="{n}" y1="{y:.2f}" '
                 f'y2="{y:.2f}"/>')
        legend.append(("lgR", "VBV maxrate"))
    parts.append(chart("Bitrate", legend, body, ymax, n, fps,
                       note='<p class="note">kbit/s. Frames are in display '
                       'order.</p>'))

    # QP with frame types
    pts_q = bucket([float(q) for q in qps], MAX_POINTS)
    qmax = nice_max(max(qps) * 1.1)
    body = path_line(pts_q, qmax, "line2")
    if n <= 3000:
        for t, cls in (("B", "tB"), ("P", "tP")):
            body += ticks_svg([i for i, r in enumerate(frames)
                               if r["type"] == t], cls, H - 8, H)
    body += ticks_svg([i for i, r in enumerate(frames) if r["type"] == "I"],
                      "tI", H - 16, H)
    legend = [("lg2", "slice QP"), ("lgI", "I"), ("lgP", "P"), ("lgB", "B")]
    if n > 3000:
        legend = [("lg2", "slice QP"), ("lgI", "I")]
    parts.append(chart("QP and frame types", legend, body, qmax, n, fps,
                       note='<p class="note">Ticks along the bottom mark '
                       'each frame by type.</p>'))

    # VBV
    has_vbv = any("vbv_fill" in r for r in frames)
    if has_vbv and facts["bufsize"]:
        cap = facts["bufsize"] * 1000.0
        by_c = sorted(frames, key=lambda r: r.get("cidx", r["frame"]))
        fill = [min(100.0, max(0.0, r.get("vbv_fill", 0) / cap * 100))
                for r in by_c]
        pts_v = bucket(fill, MAX_POINTS)
        body = path_line(pts_v, 100.0, "line1")
        low = min(fill)
        parts.append(chart(
            "Buffer fill", [("lg1", "VBV fill")],
            body, 100.0, n, fps, unit="%",
            note=f'<p class="note">Percent of the {facts["bufsize"]:g} kbit '
                 f'buffer after each frame, in decode order. Lowest '
                 f'{low:.0f}%.</p>'))

    # detections
    rows = []
    if det_ok:
        sc = [i for i, r in enumerate(frames) if r.get("scenecut")]
        kf = [i for i, r in enumerate(frames) if r["type"] == "I"]
        rows.append(band_row("Shots", [(s["i0"], s["i1"]) for s in
                                       shots[0::2]], "bShotA", n))
        shots_b = "".join(f'<rect class="bShotB" x="{s["i0"]}" y="15" '
                          f'width="{s["i1"] - s["i0"]}" height="70"/>'
                          for s in shots[1::2])
        rows[-1] = rows[-1].replace("</svg>", shots_b + "</svg>", 1)
        rows.append(band_row("Scene cuts", [], "bScene", n, ticks_at=sc))
        rows.append(band_row("Intra pictures", [], "bKey", n, ticks_at=kf))
        rows.append(band_row("Screen content",
                             runs([r.get("screen") for r in frames]),
                             "bScreen", n))
        rows.append(band_row("Handheld jitter",
                             runs([r.get("jitter") for r in frames]),
                             "bJit", n))
        if any("dark" in r for r in frames):
            rows.append(band_row(
                "Dark", runs([r.get("dark", 0) >= DARK_FRAME for r in frames]),
                "bDark", n, title="at least a quarter of the picture in "
                "dark, smooth blocks"))
        rows.append(band_row("Flat", runs([r.get("flat") for r in frames]),
                             "bFlat", n))
        counts = [
            ("Scene cuts", len(sc)),
            ("Screen-content frames", sum(1 for r in frames
                                          if r.get("screen"))),
            ("Handheld-jitter frames", sum(1 for r in frames
                                           if r.get("jitter"))),
            ("Flat frames", sum(1 for r in frames if r.get("flat"))),
        ]
        if any("dark" in r for r in frames):
            counts.append(("Dark frames", sum(
                1 for r in frames if r.get("dark", 0) >= DARK_FRAME)))
        lst = "".join(f"<li>{html.escape(k)}: {v:,}</li>" for k, v in counts)
        parts.append(
            '<section class="card"><div class="chead"><h2>What the encoder '
            'detected</h2></div>' + "".join(rows) + x_labels(n, fps).replace(
                'class="xlab"', 'class="xlab" style="margin-left:120px"') +
            f'<ul class="det-list">{lst}</ul></section>')
    else:
        parts.append('<section class="card"><h2>What the encoder detected'
                     '</h2><p class="note">These stats were written before '
                     '--frame-stats carried detections.</p></section>')

    # shots
    cards = []
    for k, s in enumerate(shots):
        img = (f'<img alt="Shot {k + 1}" src="data:image/jpeg;base64,'
               f'{s["thumb"]}">' if s["thumb"] else
               '<div class="noimg">no thumbnail</div>')
        badges = "".join(f'<span class="badge">{DET_NAMES[d]}</span>'
                         for d in s["det"])
        t0, t1 = s["first"] / fps, (s["last"] + 1) / fps
        cards.append(
            f'<div class="shot">{img}<h3>Shot {k + 1}</h3>'
            f'<p>{fmt_time(t0)} to {fmt_time(t1)}</p>'
            f'<p>{s["frames"]} frames, {s["kbps"]:,.0f} kbit/s</p>'
            f'<p>QP {s["qp"]:.1f}</p>'
            f'<div class="badges">{badges}</div></div>')
    thumb_note = "" if made else (
        '<p class="note">No thumbnails: ffmpeg or the stream was not '
        'found.</p>')
    shot_rows = "".join(
        f'<tr><td>{k + 1}</td><td class="num">{s["first"]}</td>'
        f'<td class="num">{s["last"]}</td>'
        f'<td class="num">{s["frames"]}</td>'
        f'<td class="num">{s["kbps"]:,.0f}</td>'
        f'<td class="num">{s["qp"]:.1f}</td>'
        f'<td>{html.escape(s["why"])}</td>'
        f'<td>{html.escape(", ".join(DET_NAMES[d] for d in s["det"]))}</td>'
        f'</tr>' for k, s in enumerate(shots))
    parts.append(
        '<section class="card"><div class="chead"><h2>Shots</h2>'
        '<span class="sub">A shot starts at each scene cut.</span></div>'
        f'{thumb_note}<div class="shots">{"".join(cards)}</div>'
        '<details><summary>Shot table</summary><div class="tblwrap"><table>'
        '<tr><th>#</th><th class="num">First</th><th class="num">Last</th>'
        '<th class="num">Frames</th><th class="num">kbit/s</th>'
        '<th class="num">QP</th><th>Opened by</th><th>Detected</th></tr>'
        f'{shot_rows}</table></div></details></section>')

    # frame types table
    trows = []
    for t in TYPES:
        sel = [r for r in frames if r["type"] == t]
        if not sel:
            continue
        b = sum(r["bytes"] for r in sel)
        trows.append(
            f'<tr><td>{t}</td><td class="num">{len(sel):,}</td>'
            f'<td class="num">{b / len(sel):,.0f}</td>'
            f'<td class="num">{sum(r["qp"] for r in sel) / len(sel):.1f}</td>'
            f'<td class="num">{b / total * 100:.1f}%</td></tr>')
    parts.append(
        '<section class="card"><h2>Frame types</h2><div class="tblwrap">'
        '<table><tr><th>Type</th><th class="num">Frames</th>'
        '<th class="num">Mean bytes</th><th class="num">Mean QP</th>'
        '<th class="num">Share of bytes</th></tr>' + "".join(trows) +
        '</table></div></section>')

    # configuration
    if cfg:
        main_rows = "".join(
            f'<tr><td>{html.escape(k)}</td><td>{html.escape(v)}</td></tr>'
            for ind, k, v in cfg if not ind)
        knob_rows = "".join(
            f'<tr><td>{html.escape(k)}</td><td>{html.escape(v)}</td></tr>'
            for ind, k, v in cfg if ind)
        knobs = (f'<details><summary>Preset knobs</summary><div '
                 f'class="tblwrap"><table>{knob_rows}</table></div>'
                 f'</details>' if knob_rows else "")
        parts.append('<section class="card"><h2>Configuration</h2>'
                     f'<div class="tblwrap"><table>{main_rows}</table></div>'
                     f'{knobs}</section>')
    else:
        parts.append('<section class="card"><h2>Configuration</h2>'
                     '<p class="note">No configuration was given.</p>'
                     '</section>')

    notes = []
    if fps_note:
        notes.append(fps_note)
    if gaps:
        notes.append(f"{len(gaps)} frame numbers are missing or out of "
                     f"step; the time axis counts rows.")
    note_html = "".join(f'<p class="note">{html.escape(t)}</p>'
                        for t in notes)

    data = {
        "fps": fps,
        "kbps": [round(v, 1) for v in kbps],
        "f": [[r["frame"], r["type"], r["bytes"], r["qp"],
               round(r["vbv_fill"] / (facts["bufsize"] * 1000) * 100)
               if "vbv_fill" in r and facts["bufsize"] else None,
               frame_det(r), r.get("key", "")] for r in frames],
    }
    blob = json.dumps(data, separators=(",", ":")).replace("</", "<\\/")
    esc = html.escape(title)
    return (
        '<!doctype html><html lang="en"><head><meta charset="utf-8">'
        '<meta name="viewport" content="width=device-width,initial-scale=1">'
        f'<title>Encode report</title><style>{CSS}</style></head><body>'
        f'<main><header><div><h1>{esc}</h1><div class="sub">yah265 encode '
        f'report</div></div><button class="theme" id="theme" type="button">'
        f'Theme</button></header>{note_html}{"".join(parts)}</main>'
        f'<div id="tip" role="tooltip"></div>'
        f'<script type="application/json" id="data">{blob}</script>'
        f'<script>{JS}</script></body></html>')


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--frames", required=True,
                    help="the --frame-stats JSON Lines file")
    ap.add_argument("--config", help="--show-config or --verbose output")
    ap.add_argument("--stream", help="the coded stream, for thumbnails")
    ap.add_argument("--title", help="page heading (default: the stream)")
    ap.add_argument("--fps", type=float, help="override the frame rate")
    ap.add_argument("--no-thumbs", action="store_true")
    ap.add_argument("--thumb-width", type=int, default=160)
    ap.add_argument("--max-thumbs", type=int, default=48)
    ap.add_argument("-o", "--output", required=True, help="the HTML page")
    a = ap.parse_args(argv)
    frames = load_frames(a.frames)
    cfg = load_config(a.config)
    title = os.path.basename(a.title or a.stream or a.frames)
    page = render(frames, cfg, title, a.fps,
                  None if a.no_thumbs else a.stream, a.thumb_width,
                  a.max_thumbs)
    with open(a.output, "w") as f:
        f.write(page)
    return 0


if __name__ == "__main__":
    sys.exit(main())
