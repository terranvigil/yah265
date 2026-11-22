#!/usr/bin/env python3
# Copyright (c) 2026, the yah265 authors
# SPDX-License-Identifier: GPL-2.0-or-later
"""y265 bench: yah265 against x265 on your own clip, at the same file size.

    y265 bench clip.mp4                 20 s from the middle of the clip
    y265 bench clip.mp4 --full          the whole file
    y265 bench clip.mp4 --seconds 60 --start 300
    y265 bench --check                  list the tools it needs

x265 encodes the sample at --preset medium, CRF 28. yah265's CRF is then
searched until its file is within 1% of x265's size. Both are timed in
interleaved rounds and scored with VMAF v1, and the run writes a summary,
a result card and a comparison page into one folder.

Installed as `y265-bench` beside the y265 binary; `y265 bench` runs it.
"""
import argparse, html, json, math, os, platform, re, resource, shutil
import statistics, subprocess, sys, tempfile, time

X265_LOOKAHEAD = 20                     # x265's documented medium default
DISP = (1920, 1080)
V1 = "vmaf_v1.0.16"
TOL = 0.01


def log(msg):
    print(msg, file=sys.stderr, flush=True)


# ------------------------------------------------------------------ tools

def install_lines():
    mac = sys.platform == "darwin"
    pk = "brew install" if mac else "sudo apt install"
    vmaf = ("git clone https://github.com/Netflix/vmaf && cd vmaf/libvmaf && "
            "meson setup build --buildtype=release -Denable_float=true && "
            "ninja -C build && sudo ninja -C build install")
    models = ("sudo mkdir -p /usr/local/share/vmaf/model && sudo cp -R "
              "vmaf/model/vmaf_v1.0.16 vmaf/model/vmaf_v1.0.16_hfr "
              "/usr/local/share/vmaf/model/")
    return {"ffmpeg": f"{pk} ffmpeg", "ffprobe": f"{pk} ffmpeg",
            "x265": f"{pk} x265", "vmaf": vmaf, "models": models,
            "veo": "optional: https://github.com/terranvigil/veo (a built-in "
                   "player is used without it)"}


def _exe(path):
    if not path:
        return None
    p = os.path.expanduser(path)
    if os.sep not in p:
        return shutil.which(p)
    return p if os.path.isfile(p) and os.access(p, os.X_OK) else None


def find_y265(explicit):
    here = os.path.dirname(os.path.realpath(__file__))
    for c in (explicit, os.environ.get("Y265_BIN"),
              os.path.join(here, "y265"), "y265",
              os.path.join(os.path.dirname(here), "build-rel", "y265")):
        p = _exe(c)
        if p:
            return p
    return None


def find_models(explicit, vmaf):
    cands = [explicit, os.environ.get("VMAF_MODELS"),
             "~/.local/share/vmaf/model", "/usr/local/share/vmaf/model",
             "/opt/homebrew/share/vmaf/model", "/usr/share/vmaf/model"]
    if vmaf:
        # a source tree's build: vmaf/libvmaf/build/tools/vmaf
        cands.append(os.path.join(os.path.dirname(os.path.realpath(vmaf)),
                                  "..", "..", "..", "model"))
    for c in cands:
        if c and os.path.isfile(os.path.join(os.path.expanduser(c), V1,
                                             V1 + "_3d0h.json")):
            return os.path.abspath(os.path.expanduser(c))
    return None


def ffmpeg_has_libx265(ffmpeg):
    try:
        r = subprocess.run([ffmpeg, "-hide_banner", "-encoders"],
                           capture_output=True, text=True, timeout=30)
    except OSError:
        return False
    return " libx265 " in r.stdout


def find_tools(a):
    t = {"y265": find_y265(a.y265), "ffmpeg": _exe("ffmpeg"),
         "ffprobe": _exe("ffprobe"),
         "x265": _exe(a.x265 or "x265"),
         "vmaf": None, "veo": None}
    for c in (a.vmaf, os.environ.get("VMAF"), "~/.local/opt/vmaf-v1/bin/vmaf",
              "vmaf"):
        t["vmaf"] = _exe(c)
        if t["vmaf"]:
            break
    t["models"] = find_models(a.vmaf_models, t["vmaf"])
    t["x265_via"] = "cli" if t["x265"] else None
    if not t["x265"] and t["ffmpeg"] and ffmpeg_has_libx265(t["ffmpeg"]):
        t["x265_via"] = "ffmpeg"
    if a.player != "builtin":
        t["veo"] = _exe(a.veo or os.environ.get("VEO") or "veo")
    return t


def missing(t):
    m = [k for k in ("ffmpeg", "ffprobe", "vmaf", "models") if not t[k]]
    if not t["x265_via"]:
        m.append("x265")
    return m


def print_check(t):
    il = install_lines()
    rows = [("y265", t["y265"]), ("ffmpeg", t["ffmpeg"]),
            ("ffprobe", t["ffprobe"]),
            ("x265", t["x265"] or ("ffmpeg's libx265" if t["x265_via"]
                                   else None)),
            ("vmaf", t["vmaf"]), ("models", t["models"]),
            ("veo", t["veo"] or "(built-in player)")]
    for k, v in rows:
        print(f"  {k:8} {v or 'MISSING'}")
        if not v and k in il:
            print(f"           install: {il[k]}")
    if not t["y265"]:
        print("           build: make build-rel (or set Y265_BIN)")


# ------------------------------------------------------------------ input

def probe(t, path):
    r = subprocess.run([t["ffprobe"], "-v", "error", "-select_streams", "v:0",
                        "-show_entries", "stream=width,height,r_frame_rate,"
                        "pix_fmt,nb_frames,color_transfer:format=duration",
                        "-of", "json", path], capture_output=True, text=True)
    if r.returncode != 0:
        sys.exit(f"y265 bench: ffprobe can't read {path}\n{r.stderr.strip()}")
    j = json.loads(r.stdout)
    if not j.get("streams"):
        sys.exit(f"y265 bench: {path} has no video stream")
    s = j["streams"][0]
    num, den = (int(x) for x in s["r_frame_rate"].split("/"))
    fps = num / den if den else 0.0
    dur = float(j.get("format", {}).get("duration") or 0)
    nb = int(s["nb_frames"]) if str(s.get("nb_frames", "")).isdigit() else 0
    if not dur and nb and fps:
        dur = nb / fps
    pf = s.get("pix_fmt", "")
    return {"w": int(s["width"]), "h": int(s["height"]), "fps": fps,
            "fps_q": f"{num}/{den}", "dur": dur, "frames": nb, "pix_fmt": pf,
            "depth": 10 if re.search(r"p1[0246]", pf) else 8,
            "transfer": s.get("color_transfer", "")}


def window(info, seconds, start, full):
    """(start seconds, frame count or None for all frames)."""
    if full or (info["dur"] and info["dur"] <= seconds + 1e-6):
        return 0.0, None
    if start is None:
        start = max(0.0, (info["dur"] - seconds) / 2) if info["dur"] else 0.0
    return start, max(1, int(round(seconds * info["fps"])))


def y4m_frames(path):
    with open(path, "rb") as f:
        hdr = f.readline()
    g = {x[:1]: x[1:] for x in hdr.split()[1:]}
    w, h = int(g[b"W"]), int(g[b"H"])
    c = g.get(b"C", b"420")
    bps = 2 if b"p10" in c or b"p12" in c else 1
    fb = w * h * 3 // 2 * bps + 6
    return (os.path.getsize(path) - len(hdr)) // fb


def stage(t, src, info, start, n, wd):
    pf = "yuv420p10le" if info["depth"] == 10 else "yuv420p"
    if (n is None and start == 0 and src.lower().endswith(".y4m")
            and info["pix_fmt"] == pf):
        return os.path.abspath(src), False
    frames = n or info["frames"] or int(info["dur"] * info["fps"]) or 1
    need = info["w"] * info["h"] * 3 // 2 * (2 if info["depth"] == 10 else 1)
    need *= frames
    free = shutil.disk_usage(wd).free
    if need * 1.2 + (1 << 30) > free:
        sys.exit(f"y265 bench: the sample needs about {need / 1e9:.1f} GB in "
                 f"{wd} and {free / 1e9:.1f} GB is free. Use a shorter "
                 f"--seconds or point --out somewhere with more room.")
    out = os.path.join(wd, "sample.y4m")
    cmd = [t["ffmpeg"], "-v", "error", "-y"]
    if start:
        cmd += ["-ss", f"{start:.3f}"]
    cmd += ["-i", src, "-map", "0:v:0", "-an", "-sn"]
    if n:
        cmd += ["-frames:v", str(n)]
    cmd += ["-pix_fmt", pf, "-strict", "-1", "-f", "yuv4mpegpipe", out]
    log(f"  cutting the sample ({frames} frames)")
    r = subprocess.run(cmd, stderr=subprocess.PIPE, text=True)
    if r.returncode != 0:
        sys.exit(f"y265 bench: ffmpeg failed to cut the sample\n{r.stderr}")
    return out, True


# ----------------------------------------------------------------- encode

def fmt_q(q):
    return f"{round(q, 2):g}"


def ours_cmd(t, sample, crf, threads, out):
    return [t["y265"], sample, "--preset", "medium", "--crf", fmt_q(crf),
            "--threads", str(threads), "-o", out]


def ref_cmd(t, sample, crf, threads, depth, out):
    if t["x265_via"] == "cli":
        c = [t["x265"], "--input", sample, "--preset", "medium",
             "--crf", fmt_q(crf), "--log-level", "error"]
        if threads:
            c += ["--pools", str(threads)]
        if depth == 10:
            c += ["--output-depth", "10"]
        return c + ["-o", out]
    xp = "log-level=error" + (f":pools={threads}" if threads else "")
    c = [t["ffmpeg"], "-v", "error", "-y", "-i", sample, "-c:v", "libx265",
         "-preset", "medium", "-crf", fmt_q(crf), "-x265-params", xp]
    if depth == 10:
        c += ["-pix_fmt", "yuv420p10le"]
    return c + ["-f", "hevc", out]


def run_timed(cmd):
    """(wall s, child cpu s)"""
    r0 = resource.getrusage(resource.RUSAGE_CHILDREN)
    t0 = time.perf_counter()
    r = subprocess.run(cmd, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
    wall = time.perf_counter() - t0
    r1 = resource.getrusage(resource.RUSAGE_CHILDREN)
    if r.returncode != 0:
        sys.exit("y265 bench: encode failed:\n  " + " ".join(cmd) + "\n"
                 + r.stderr.decode(errors="replace")[-2000:])
    cpu = (r1.ru_utime - r0.ru_utime) + (r1.ru_stime - r0.ru_stime)
    return wall, cpu


def secant(size_of, target, crf, slope=-0.13, steps=8, aim=0.005):
    """A secant in (crf, ln bytes), bisecting once the target is bracketed,
    stopping inside one tread of the integer-QP staircase. Returns
    (best (crf, bytes, err), points, slope)."""
    pts, best = [], None
    for _ in range(steps):
        crf = round(max(0.0, min(51.0, crf)), 2)
        if any(abs(crf - c) < 1e-6 for c, _ in pts):
            break
        sz = size_of(crf)
        err = abs(sz - target) / target
        if best is None or err < best[2]:
            best = (crf, sz, err)
        pts.append((crf, sz))
        if err < aim:
            break
        hi = [c for c, b in pts if b > target]
        lo = [c for c, b in pts if b < target]
        if hi and lo and min(lo) - max(hi) < 0.015:
            break
        if len(pts) >= 2:
            (ca, ba), (cb, bb) = pts[-2], pts[-1]
            if ba > 0 and bb > 0 and abs(cb - ca) > 1e-6:
                m = math.log(bb / ba) / (cb - ca)
                if m < -0.02:
                    slope = min(-0.05, max(-0.30, m))
        crf = crf + math.log(target / sz) / slope
        if hi and lo:
            h, l_ = max(hi), min(lo)
            if not (h < crf < l_) or any(abs(round(crf, 2) - c) < 1e-6
                                         for c, _ in pts):
                crf = (h + l_) / 2.0
    return best, pts, slope


def match(ours_size, ref_size, ref_crf, start=None):
    """Solve our CRF onto x265's size at REF_CRF. When our staircase steps
    over the 1% band, move x265 onto our nearest point instead. Returns a
    dict with both CRFs, both sizes and every try."""
    tries = []

    def ours(q):
        b = ours_size(q)
        tries.append({"encoder": "yah265", "crf": q, "bytes": b})
        log(f"  yah265 crf {fmt_q(q):>6}: {b:>10} B")
        return b

    def ref(q):
        b = ref_size(q)
        tries.append({"encoder": "x265", "crf": q, "bytes": b})
        log(f"  x265   crf {fmt_q(q):>6}: {b:>10} B")
        return b

    target = ref(ref_crf)
    best, _, slope = secant(ours, target,
                            ref_crf - 3 if start is None else start)
    out = {"ref_crf": ref_crf, "ref_bytes": target, "ours_crf": best[0],
           "ours_bytes": best[1], "moved_ref": False}
    if best[2] > TOL:
        xb, _, _ = secant(ref, best[1],
                          ref_crf + math.log(best[1] / target) / slope)
        if abs(best[1] / xb[1] - 1) < best[2]:
            out.update(ref_crf=xb[0], ref_bytes=xb[1], moved_ref=True)
    out["dsize"] = out["ours_bytes"] / out["ref_bytes"] - 1
    out["matched"] = abs(out["dsize"]) <= TOL
    out["tries"] = tries
    return out


# ------------------------------------------------------------------ score

def v1_model(models, w, h, fps):
    hfr = fps >= 49.9
    name = "1d5h_2160" if min(w, h) > 1080 else "3d0h"
    sub = V1 + ("_hfr" if hfr else "")
    return os.path.join(models, sub, f"{sub}_{name}.json")


def upscaled(w, h):
    return w < DISP[0] and h < DISP[1]


def _feed(t, inp, up, fifo):
    vf = []
    if up:
        vf = ["-vf", f"scale={DISP[0]}:{DISP[1]}:flags=bicubic"
              ":force_original_aspect_ratio=decrease:force_divisible_by=2"]
    return subprocess.Popen([t["ffmpeg"], "-v", "error", "-y", "-threads", "2",
                             "-i", inp] + vf +
                            ["-pix_fmt", "yuv420p10le", "-strict", "-1",
                             "-f", "yuv4mpegpipe", fifo],
                            stdin=subprocess.DEVNULL, stderr=subprocess.PIPE)


def score_v1(t, sample, bs, info, threads, out_json):
    """VMAF v1 of BS against SAMPLE, every frame, both sides at 10 bits
    through fifos, scaled up to fit 1920x1080 when smaller in both."""
    w, h = info["w"], info["h"]
    model = v1_model(t["models"], w, h, info["fps"])
    wd = tempfile.mkdtemp(prefix="y265bench-")
    rf, df = os.path.join(wd, "r.fifo"), os.path.join(wd, "d.fifo")
    os.mkfifo(rf)
    os.mkfifo(df)
    up = upscaled(w, h)
    pr, pd = _feed(t, sample, up, rf), _feed(t, bs, up, df)
    v = subprocess.run([t["vmaf"], "--threads", str(threads), "-r", rf,
                        "-d", df, "--json", "-o", out_json, "-q", "--model",
                        f"path={model}:name=v1:cambi.enc_bitdepth="
                        f"{info['depth']}:cambi.enc_width={w}"
                        f":cambi.enc_height={h}"],
                       stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
    if v.returncode != 0:
        pr.kill()
        pd.kill()
    pr.wait()
    pd.wait()
    shutil.rmtree(wd, ignore_errors=True)
    if v.returncode != 0:
        sys.exit("y265 bench: vmaf failed. A libvmaf older than the v1 "
                 "models can't read them; build it from Netflix's current "
                 "repository.\n" + v.stderr.decode(errors="replace")[-1500:])
    fr = [x["metrics"]["v1"] for x in json.load(open(out_json))["frames"]]
    return {"model": os.path.basename(model)[:-5], "display": "fit 1920x1080"
            if up else "native", "frames": len(fr), "per_frame": fr,
            **pool(fr)}


def pool(xs):
    s = sorted(xs)
    n = len(s)
    hm = n / sum(1.0 / (x + 1.0) for x in s) - 1.0 if n else 0.0
    return {"mean": sum(s) / n if n else 0.0,
            "p5": s[int(0.05 * (n - 1))] if n else 0.0,
            "min": s[0] if n else 0.0, "hmean": hm}


# ---------------------------------------------------------------- reports

def verdict(speed, dv1, dsize, matched, busy=None):
    if speed >= 1.0:
        sp = f"{speed:.2f}x as fast as x265"
    else:
        sp = f"slower than x265 ({speed:.2f}x its speed)"
    if dv1 >= 0.005:
        q = f"{dv1:.2f} VMAF v1 points ahead"
    elif dv1 <= -0.005:
        q = f"{-dv1:.2f} VMAF v1 points behind"
    else:
        q = "level on VMAF v1"
    if matched:
        sz = f"at the same file size ({100 * dsize:+.2f}%)"
    else:
        sz = (f"but the sizes didn't match: yah265's file was "
              f"{100 * dsize:+.1f}% against x265's, so the scores don't "
              f"compare")
    s = f"yah265 was {sp} and {q}, {sz}."
    if busy:
        s += (f" The machine was busy (load {busy[0]:.1f} on {busy[1]} "
              f"cores), so the speed number is rough.")
    return s


def cpu_name():
    try:
        if sys.platform == "darwin":
            return subprocess.run(["sysctl", "-n", "machdep.cpu.brand_string"],
                                  capture_output=True, text=True
                                  ).stdout.strip()
        with open("/proc/cpuinfo") as f:
            for line in f:
                if line.startswith("model name"):
                    return line.split(":", 1)[1].strip()
    except OSError:
        pass
    return platform.processor() or platform.machine()


def tool_version(cmd, rx):
    try:
        r = subprocess.run(cmd, capture_output=True, text=True, timeout=30)
    except OSError:
        return "unknown"
    m = re.search(rx, r.stdout + r.stderr)
    return m.group(1) if m else "unknown"


def our_lookahead(t, sample, threads):
    r = subprocess.run([t["y265"], sample, "--preset", "medium", "--crf", "28",
                        "--threads", str(threads), "--show-config"],
                       capture_output=True, text=True)
    m = re.search(r"rc-lookahead\s*:\s*(\d+)", r.stdout + r.stderr)
    return int(m.group(1)) if m else None


def kb(b):
    return f"{b / 1e6:.2f} MB" if b >= 1e6 else f"{b / 1e3:.1f} kB"


def esc(s):
    return html.escape(str(s), quote=True)


def card_svg(R):
    o, x = R["ours"], R["x265"]
    sp = R["speed_ratio"]
    dv = o["v1"]["mean"] - x["v1"]["mean"]
    win = "#1a7f4b"
    lose = "#b3261e"
    spc = win if sp >= 1 else lose
    dvc = win if dv >= 0 else lose
    sp_lab = "as fast as x265" if sp >= 1 else "of x265's speed"
    s = R["sample"]
    size = (f"same file size: {kb(o['bytes'])} vs {kb(x['bytes'])} "
            f"({100 * R['dsize']:+.2f}%)" if R["matched"] else
            f"sizes NOT matched: {kb(o['bytes'])} vs {kb(x['bytes'])} "
            f"({100 * R['dsize']:+.1f}%)")
    thr = R["threads_short"]
    foot = (f"{s['w']}x{s['h']} {s['fps']:.3g} fps, {s['frames']} frames "
            f"({s['seconds']:.1f} s), both --preset medium, {thr}")
    foot2 = f"{R['machine']['cpu']}, {R['date'][:10]}  |  y265 bench"
    busy = ("machine busy: speed is rough" if R["busy"] else "")
    return f"""<svg xmlns="http://www.w3.org/2000/svg" width="1200" height="630" viewBox="0 0 1200 630">
<rect width="1200" height="630" fill="#ffffff"/>
<rect x="0" y="0" width="1200" height="8" fill="#2f5bd3"/>
<g font-family="-apple-system, 'Helvetica Neue', Helvetica, Arial, sans-serif" fill="#1b1f24">
<text x="60" y="92" font-size="40" font-weight="700">yah265 vs x265 on {esc(R['title'])}</text>
<text x="60" y="134" font-size="24" fill="#57606a">{esc(size)}</text>
<text x="60" y="300" font-size="120" font-weight="700" fill="{spc}">{sp:.2f}x</text>
<text x="64" y="346" font-size="28" fill="#57606a">{sp_lab}</text>
<text x="64" y="384" font-size="22" fill="#57606a">{o['fps']:.1f} fps vs {x['fps']:.1f} fps</text>
<text x="64" y="418" font-size="22" fill="{lose}">{esc(busy)}</text>
<text x="640" y="300" font-size="120" font-weight="700" fill="{dvc}">{dv:+.2f}</text>
<text x="644" y="346" font-size="28" fill="#57606a">VMAF v1 points</text>
<text x="644" y="384" font-size="22" fill="#57606a">{o['v1']['mean']:.2f} vs {x['v1']['mean']:.2f} ({esc(o['v1']['model'])})</text>
<line x1="60" y1="470" x2="1140" y2="470" stroke="#d0d7de" stroke-width="2"/>
<text x="60" y="520" font-size="22" fill="#57606a">{esc(foot)}</text>
<text x="60" y="556" font-size="22" fill="#57606a">yah265 {esc(R['ours']['version'])} CRF {fmt_q(o['crf'])} / x265 {esc(R['x265']['version'])} CRF {fmt_q(x['crf'])}</text>
<text x="60" y="592" font-size="22" fill="#57606a">{esc(foot2)}</text>
</g>
</svg>
"""


def svg_to_png(svg, png):
    for cmd in (["rsvg-convert", "-o", png, svg], ["magick", svg, png],
                ["convert", svg, png]):
        if shutil.which(cmd[0]):
            if subprocess.run(cmd, capture_output=True).returncode == 0:
                return True
    return False


CARD_PAGE = """<!doctype html><html lang="en"><head><meta charset="utf-8">
<title>Result card</title><style>body{margin:16px;font:15px -apple-system,
Helvetica,Arial,sans-serif;background:#fff}img{max-width:100%}</style></head>
<body><img id="c" alt="result card"><p><button id="b">Save as PNG</button></p>
<script>const u='data:image/svg+xml;charset=utf-8,'+encodeURIComponent(__SVG__);
document.getElementById('c').src=u;document.getElementById('b').onclick=()=>{
const i=new Image();i.onload=()=>{const v=document.createElement('canvas');
v.width=2400;v.height=1260;v.getContext('2d').drawImage(i,0,0,2400,1260);
v.toBlob(b=>{const a=document.createElement('a');a.href=URL.createObjectURL(b);
a.download='y265-bench-card.png';a.click()})};i.src=u};</script></body></html>
"""


def mux_mp4(t, bs, fps_q, out):
    r = subprocess.run([t["ffmpeg"], "-v", "error", "-y", "-framerate", fps_q,
                        "-i", bs, "-c", "copy", "-tag:v", "hvc1", out],
                       capture_output=True, text=True)
    if r.returncode != 0:
        sys.exit(f"y265 bench: couldn't wrap {bs} as MP4\n{r.stderr}")


PLAYER = r"""<!doctype html>
<html lang="en"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>yah265 vs x265</title>
<style>
:root{--bg:#fff;--fg:#1b1f24;--mut:#57606a;--line:#d0d7de;--a:#2f5bd3;--b:#c25e00;--win:#1a7f4b;--lose:#b3261e}
@media (prefers-color-scheme:dark){:root{--bg:#0d1117;--fg:#e6edf3;--mut:#9da7b3;--line:#30363d;--a:#6e9bff;--b:#f0883e;--win:#3fb950;--lose:#f85149}}
body{margin:0;background:var(--bg);color:var(--fg);font:15px/1.45 -apple-system,"Helvetica Neue",Helvetica,Arial,sans-serif}
main{max-width:1280px;margin:0 auto;padding:16px}
h1{font-size:22px;margin:4px 0 2px}.mut{color:var(--mut)}
.verdict{font-size:17px;margin:8px 0 14px}
.strip{display:flex;justify-content:space-between;gap:12px;flex-wrap:wrap;margin-bottom:8px}
.strip b.a{color:var(--a)}.strip b.b{color:var(--b)}
#stage{position:relative;margin:0 auto;background:#000;user-select:none;touch-action:none;cursor:ew-resize;line-height:0}
#stage video{width:100%;display:block}
#va{position:absolute;inset:0;height:100%}
#div{position:absolute;top:0;bottom:0;width:2px;background:#fff;pointer-events:none}
#stage.side{display:grid;grid-template-columns:1fr 1fr;gap:4px;cursor:default}
#stage.side #va{position:static;clip-path:none!important}#stage.side #div{display:none}
#stage.side #va{order:0}#stage.side #vb{order:1}
.ctl{display:flex;gap:8px;align-items:center;flex-wrap:wrap;margin:10px 0}
button{font:inherit;padding:5px 12px;border:1px solid var(--line);background:transparent;color:var(--fg);border-radius:6px;cursor:pointer}
canvas{width:100%;height:160px;display:block;border:1px solid var(--line);border-radius:6px;cursor:pointer}
pre{white-space:pre-wrap;font:13px/1.4 ui-monospace,Menlo,monospace;color:var(--mut)}
#card{max-width:600px;width:100%;border:1px solid var(--line);border-radius:6px;margin-top:6px}
</style></head><body><main>
<h1 id="ttl"></h1><div class="mut" id="sub"></div>
<p class="verdict" id="verdict"></p>
<div class="strip"><div><b class="a">&#9664; yah265</b> <span class="mut" id="la"></span></div>
<div><span class="mut" id="lb"></span> <b class="b">x265 &#9654;</b></div></div>
<div id="stage"><video id="vb" src="x265.mp4" muted playsinline preload="auto"></video>
<video id="va" src="yah265.mp4" muted playsinline preload="auto"></video><div id="div"></div></div>
<div class="ctl"><button id="play">Play</button><button id="back">&#8592; frame</button><button id="fwd">frame &#8594;</button>
<button id="mode">Side by side</button><span class="mut" id="pos"></span></div>
<p class="mut" id="nohevc" hidden>This browser can't play HEVC. Safari plays it, and so do Chrome and Edge on hardware with an HEVC decoder.</p>
<canvas id="tl"></canvas>
<p class="mut">VMAF v1 per frame: <b style="color:var(--a)">yah265</b> and <b style="color:var(--b)">x265</b>. Click to seek. Keys: space, arrow keys.</p>
<h2 style="font-size:17px">Result card</h2>
<img id="card" alt="result card"><div class="ctl"><button id="png">Save card as PNG</button></div>
<h2 style="font-size:17px">What was compared</h2><pre id="what"></pre>
</main><script>
const D=__DATA__;
const $=id=>document.getElementById(id);
const va=$('va'),vb=$('vb'),st=$('stage'),dv=$('div'),tl=$('tl');
$('ttl').textContent='yah265 vs x265 on '+D.title;$('sub').textContent=D.sub;
$('verdict').textContent=D.verdict;$('la').textContent=D.la;$('lb').textContent=D.lb;$('what').textContent=D.what;
const svgUrl='data:image/svg+xml;charset=utf-8,'+encodeURIComponent(D.card);$('card').src=svgUrl;
let split=0.5;function setSplit(f){split=Math.max(0,Math.min(1,f));va.style.clipPath='inset(0 '+(100-split*100)+'% 0 0)';dv.style.left='calc('+(split*100)+'% - 1px)'}
setSplit(.5);
function frac(e){const r=st.getBoundingClientRect();return (e.clientX-r.left)/r.width}
let drag=false;st.addEventListener('pointerdown',e=>{if(st.classList.contains('side'))return;drag=true;st.setPointerCapture(e.pointerId);setSplit(frac(e))});
st.addEventListener('pointermove',e=>{if(drag)setSplit(frac(e))});st.addEventListener('pointerup',()=>drag=false);
$('mode').onclick=()=>{const s=st.classList.toggle('side');$('mode').textContent=s?'Split':'Side by side'};
const N=D.a.length,fps=D.fps;const frame=()=>Math.min(N-1,Math.floor(va.currentTime*fps+1e-3));
function seekF(f){f=Math.max(0,Math.min(N-1,f));va.pause();vb.pause();const t=(f+0.5)/fps;va.currentTime=t;vb.currentTime=t}
$('play').onclick=()=>{if(va.paused){vb.currentTime=va.currentTime;va.play();vb.play()}else{va.pause();vb.pause();vb.currentTime=va.currentTime}};
va.onplay=()=>$('play').textContent='Pause';va.onpause=()=>$('play').textContent='Play';
va.onended=()=>vb.pause();
$('back').onclick=()=>seekF(frame()-1);$('fwd').onclick=()=>seekF(frame()+1);
document.addEventListener('keydown',e=>{if(e.key===' '){e.preventDefault();$('play').click()}else if(e.key==='ArrowLeft')seekF(frame()-1);else if(e.key==='ArrowRight')seekF(frame()+1)});
function sync(now,m){if(!vb.paused&&Math.abs(vb.currentTime-m.mediaTime)>1.5/fps)vb.currentTime=m.mediaTime;draw();va.requestVideoFrameCallback(sync)}
if(va.requestVideoFrameCallback)va.requestVideoFrameCallback(sync);else va.ontimeupdate=draw;
va.onloadedmetadata=()=>{const w=va.videoWidth,h=va.videoHeight;if(w&&h)st.style.maxWidth='min(100%, calc(72vh * '+(w/h)+'), '+Math.max(w*2,960)+'px)'};
va.onseeked=draw;va.onerror=vb.onerror=()=>{$('nohevc').hidden=false};
const lo=Math.max(0,Math.floor(Math.min(...D.a,...D.b))-2);
function draw(){const r=tl.getBoundingClientRect(),dp=devicePixelRatio||1;tl.width=r.width*dp;tl.height=r.height*dp;const c=tl.getContext('2d');c.scale(dp,dp);
const W=r.width,H=r.height,cs=getComputedStyle(document.documentElement),X=i=>i/(Math.max(1,N-1))*(W-40)+30,Y=v=>H-14-(v-lo)/(100-lo)*(H-24);
c.fillStyle=cs.getPropertyValue('--mut');c.font='11px sans-serif';for(const v of [lo,Math.round((lo+100)/2),100]){c.fillText(v,2,Y(v)+4)}
for(const [arr,col] of [[D.b,'--b'],[D.a,'--a']]){c.strokeStyle=cs.getPropertyValue(col);c.lineWidth=1.3;c.beginPath();arr.forEach((v,i)=>i?c.lineTo(X(i),Y(v)):c.moveTo(X(i),Y(v)));c.stroke()}
const f=frame();c.strokeStyle=cs.getPropertyValue('--fg');c.beginPath();c.moveTo(X(f),4);c.lineTo(X(f),H-10);c.stroke();
$('pos').textContent='frame '+f+' of '+N+'   yah265 '+D.a[f].toFixed(2)+'   x265 '+D.b[f].toFixed(2)}
tl.onclick=e=>{const r=tl.getBoundingClientRect();seekF(Math.round((e.clientX-r.left-30)/(r.width-40)*(N-1)))};
addEventListener('resize',draw);draw();
$('png').onclick=()=>{const i=new Image();i.onload=()=>{const cv=document.createElement('canvas');cv.width=2400;cv.height=1260;const c=cv.getContext('2d');c.drawImage(i,0,0,2400,1260);
cv.toBlob(b=>{const a=document.createElement('a');a.href=URL.createObjectURL(b);a.download='y265-bench-card.png';a.click()})};i.src=svgUrl};
</script></body></html>
"""


def script_json(v):
    return json.dumps(v).replace("</", "<\\/")


def write_player(R, outdir, card):
    pd = os.path.join(outdir, "player")
    os.makedirs(pd, exist_ok=True)
    for arm, name in (("ours", "yah265.mp4"), ("x265", "x265.mp4")):
        shutil.copy(R[arm]["mp4"], os.path.join(pd, name))
    o, x = R["ours"], R["x265"]
    d = {"title": R["title"], "verdict": R["verdict"], "card": card,
         "sub": R["sub"], "what": R["what"], "fps": R["sample"]["fps"],
         "a": [round(v, 3) for v in o["v1"]["per_frame"]],
         "b": [round(v, 3) for v in x["v1"]["per_frame"]],
         "la": f"CRF {fmt_q(o['crf'])}, {kb(o['bytes'])}, "
               f"VMAF v1 {o['v1']['mean']:.2f}",
         "lb": f"CRF {fmt_q(x['crf'])}, {kb(x['bytes'])}, "
               f"VMAF v1 {x['v1']['mean']:.2f}"}
    page = PLAYER.replace("__DATA__", script_json(d))
    p = os.path.join(pd, "index.html")
    with open(p, "w") as f:
        f.write(page)
    return p


def veo_export(t, R, outdir):
    pd = os.path.join(outdir, "player")
    notes = os.path.join(outdir, "work", "notes.txt")
    with open(notes, "w") as f:
        f.write(R["verdict"] + "\n\n" + R["what"])
    o, x = R["ours"], R["x265"]
    cmd = [t["veo"], "compare", "export", "--no-progressive", "--out", pd,
           "--a", o["mp4"], "--vmaf-a", o["v1_json"], "--label-a", "yah265",
           "--detail-a", f"medium, CRF {fmt_q(o['crf'])}",
           "--b", x["mp4"], "--vmaf-b", x["v1_json"], "--label-b", "x265",
           "--detail-b", f"medium, CRF {fmt_q(x['crf'])}",
           "--title", R["title"], "--subtitle",
           "yah265 against x265 at the same file size",
           "--metric", "v1", "--metric-label", "VMAF v1",
           "--model", o["v1"]["model"],
           "--fps", f"{R['sample']['fps']:.6f}", "--notes", notes]
    r = subprocess.run(cmd, capture_output=True, text=True)
    if r.returncode != 0:
        log("  veo export failed; writing the built-in player instead\n"
            + r.stderr[-800:])
        return None
    return os.path.join(pd, "index.html")


def shown(cmd, sample):
    """CMD as a reader would type it: tool names bare, the sample as
    sample.y4m"""
    out = [os.path.basename(cmd[0])]
    out += ["sample.y4m" if c == sample else c for c in cmd[1:]]
    return out


def what_block(R):
    s, o, x = R["sample"], R["ours"], R["x265"]
    lines = [
        f"source       {R['source']}",
        f"sample       {s['desc']}",
        f"picture      {s['w']}x{s['h']}, {s['fps']:.3f} fps, {s['depth']}-bit "
        f"4:2:0, {s['frames']} frames",
        f"yah265       {o['version']} --preset medium --crf {fmt_q(o['crf'])}, "
        f"lookahead {o['lookahead']}",
        f"x265         {x['version']} --preset medium --crf {fmt_q(x['crf'])}, "
        f"lookahead {x['lookahead']}",
        f"threads      {R['threads_label']}",
        f"size match   {100 * R['dsize']:+.2f}% "
        + ("(within 1%)" if R["matched"] else "(NOT within 1%)")
        + ("; x265's CRF was moved onto yah265's nearest point"
           if R["moved_ref"] else ""),
        f"timing       {R['rounds']} interleaved rounds per encoder, order "
        f"reversed each round, median wall; both read the same y4m file",
        f"quality      VMAF v1 {o['v1']['model']}, every frame, 10-bit, "
        f"{o['v1']['display']}",
        f"machine      {R['machine']['cpu']}, {R['machine']['cores']} cores, "
        f"load {R['machine']['load']:.1f} at start, {R['machine']['os']}",
        f"date         {R['date']}",
    ] + ([
        "note         HDR source: VMAF v1 is an SDR metric, so its scores "
        "are rough here"] if R["hdr"] else []) + [
        f"yah265 cmd   {' '.join(o['cmd'])}",
        f"x265 cmd     {' '.join(x['cmd'])}",
    ]
    return "\n".join(lines)


def table(R):
    o, x = R["ours"], R["x265"]
    rows = [("", "yah265", "x265"),
            ("version", o["version"], x["version"]),
            ("rate", f"CRF {fmt_q(o['crf'])}", f"CRF {fmt_q(x['crf'])}"),
            ("size", kb(o["bytes"]), kb(x["bytes"])),
            ("wall (median)", f"{o['wall']:.2f} s", f"{x['wall']:.2f} s"),
            ("speed", f"{o['fps']:.1f} fps", f"{x['fps']:.1f} fps"),
            ("cpu time", f"{o['cpu']:.1f} s", f"{x['cpu']:.1f} s"),
            ("VMAF v1 mean", f"{o['v1']['mean']:.2f}",
             f"{x['v1']['mean']:.2f}"),
            ("VMAF v1 5th pct", f"{o['v1']['p5']:.2f}",
             f"{x['v1']['p5']:.2f}"),
            ("VMAF v1 min", f"{o['v1']['min']:.2f}", f"{x['v1']['min']:.2f}")]
    return "\n".join(f"  {a:16} {b:>14} {c:>14}" for a, b, c in rows)


# ------------------------------------------------------------------- main

def parse(argv):
    ap = argparse.ArgumentParser(
        prog="y265 bench", description="Encode your clip with yah265 and "
        "x265 at the same file size, time both and score both with VMAF v1.")
    ap.add_argument("input", nargs="?", help="any video ffmpeg can read")
    ap.add_argument("--seconds", type=float, default=20.0,
                    help="sample length (default 20)")
    ap.add_argument("--start", type=float, default=None,
                    help="sample start in seconds (default: centred)")
    ap.add_argument("--full", action="store_true", help="use the whole file")
    ap.add_argument("--threads", type=int, default=0,
                    help="threads per encoder, 0 for each one's own "
                         "default of all cores (default 0)")
    ap.add_argument("--crf", type=float, default=28.0,
                    help="x265's CRF, the point both are matched at "
                         "(default 28, x265's own default)")
    ap.add_argument("--rounds", type=int, default=3,
                    help="timed runs per encoder (default 3)")
    ap.add_argument("--out", help="result folder (default "
                    "./y265-bench-<name>-<time>)")
    ap.add_argument("--title", help="name shown on the card and the page")
    ap.add_argument("--player", choices=("auto", "veo", "builtin", "none"),
                    default="auto")
    ap.add_argument("--keep", action="store_true",
                    help="keep the staged sample and the work files")
    ap.add_argument("--check", action="store_true",
                    help="list the tools found and stop")
    for k in ("y265", "x265", "vmaf", "vmaf-models", "veo"):
        ap.add_argument("--" + k, help=argparse.SUPPRESS)
    a = ap.parse_args(argv)
    if not a.check and not a.input:
        ap.error("give a video file, or --check")
    if a.rounds < 1:
        ap.error("--rounds must be at least 1")
    return a


def main(argv=None):
    a = parse(sys.argv[1:] if argv is None else argv)
    t = find_tools(a)
    if a.check:
        print("y265 bench needs:")
        print_check(t)
        return 0 if t["y265"] and not missing(t) else 2
    miss = missing(t)
    if not t["y265"] or miss:
        print("y265 bench can't run yet. Missing:", file=sys.stderr)
        il = install_lines()
        for k in ([] if t["y265"] else ["y265"]) + miss:
            print(f"  {k:8} " + il.get(k, "make build-rel, or set Y265_BIN"),
                  file=sys.stderr)
        return 2
    if a.player == "veo" and not t["veo"]:
        print("y265 bench: --player veo, but veo isn't on the PATH",
              file=sys.stderr)
        return 2
    load0, cores = os.getloadavg()[0], os.cpu_count() or 1
    info = probe(t, a.input)
    if info["fps"] <= 0:
        sys.exit("y265 bench: the input has no usable frame rate")
    stem = os.path.splitext(os.path.basename(a.input))[0]
    stamp = time.strftime("%Y%m%d-%H%M%S")
    outdir = os.path.abspath(a.out or f"y265-bench-{stem}-{stamp}")
    wd = os.path.join(outdir, "work")
    os.makedirs(wd, exist_ok=True)
    log(f"y265 bench: {a.input} -> {outdir}")
    if info["transfer"] in ("smpte2084", "arib-std-b67"):
        log("  note: this is HDR (PQ/HLG). VMAF v1 is an SDR metric, so "
            "read its scores as rough.")
    start, n = window(info, a.seconds, a.start, a.full)
    sample, staged = stage(t, a.input, info, start, n, wd)
    frames = y4m_frames(sample)
    if frames < 1:
        sys.exit("y265 bench: the sample has no frames (is --start past "
                 "the end of the file?)")
    secs = frames / info["fps"]
    desc = (f"{start:.2f} s to {start + secs:.2f} s of "
            f"{info['dur']:.2f} s" if n else f"the whole file ({secs:.2f} s)")
    th = a.threads
    depth = info["depth"]

    log("matching sizes:")

    def ours_size(q):
        p = os.path.join(wd, f"solve-ours-{fmt_q(q)}.hevc")
        run_timed(ours_cmd(t, sample, q, th, p))
        b = os.path.getsize(p)
        os.remove(p)
        return b

    def ref_size(q):
        p = os.path.join(wd, f"solve-x265-{fmt_q(q)}.hevc")
        run_timed(ref_cmd(t, sample, q, th, depth, p))
        b = os.path.getsize(p)
        os.remove(p)
        return b

    m = match(ours_size, ref_size, a.crf)

    log(f"timing, {a.rounds} rounds each:")
    arms = {"ours": lambda out: ours_cmd(t, sample, m["ours_crf"], th, out),
            "x265": lambda out: ref_cmd(t, sample, m["ref_crf"], th, depth,
                                        out)}
    runs = {"ours": [], "x265": []}
    for r in range(a.rounds):
        for arm in (("ours", "x265") if r % 2 == 0 else ("x265", "ours")):
            out = os.path.join(outdir if r == 0 else wd,
                               ("yah265" if arm == "ours" else "x265")
                               + (".hevc" if r == 0 else f".r{r}.hevc"))
            wall, cpu = run_timed(arms[arm](out))
            runs[arm].append({"wall": wall, "cpu": cpu,
                              "bytes": os.path.getsize(out)})
            log(f"  round {r + 1} {arm:5} {wall:7.2f} s")
            if r:
                os.remove(out)

    log("scoring with VMAF v1:")
    vt = th or min(cores, 8)
    R = {"tool": "y265 bench", "date": time.strftime("%Y-%m-%d %H:%M:%S %z"),
         "source": os.path.abspath(a.input), "title": a.title or stem,
         "rounds": a.rounds, "moved_ref": m["moved_ref"],
         "solve": m["tries"],
         "threads_short": f"{th} threads each" if th else "all cores",
         "threads_label": (f"{th} threads each (yah265 --threads {th}, "
                           f"x265 --pools {th})" if th else
                           f"each encoder's default thread count "
                           f"({cores} cores)"),
         "machine": {"cpu": cpu_name(), "cores": cores, "load": load0,
                     "os": f"{platform.system()} {platform.release()}"},
         "sample": {"w": info["w"], "h": info["h"], "fps": info["fps"],
                    "fps_q": info["fps_q"], "depth": depth, "frames": frames,
                    "seconds": secs, "start": start, "desc": desc,
                    "staged": staged},
         "hdr": info["transfer"] in ("smpte2084", "arib-std-b67")}
    xver = (tool_version([t["x265"], "--version"], r"version (\S+)")
            if t["x265_via"] == "cli" else "libx265 in ffmpeg " +
            tool_version([t["ffmpeg"], "-version"], r"ffmpeg version (\S+)"))
    for arm, crf, ver, la in (
            ("ours", m["ours_crf"],
             tool_version([t["y265"], "--version"], r"y265 (\S+)"),
             our_lookahead(t, sample, th)),
            ("x265", m["ref_crf"], xver, X265_LOOKAHEAD)):
        name = "yah265" if arm == "ours" else "x265"
        bs = os.path.join(outdir, name + ".hevc")
        walls = [x["wall"] for x in runs[arm]]
        cpus = [x["cpu"] for x in runs[arm]]
        js = os.path.join(outdir, name + ".v1.json")
        log(f"  {name}")
        v1 = score_v1(t, sample, bs, info, vt, js)
        mp4 = os.path.join(outdir, name + ".mp4")
        mux_mp4(t, bs, info["fps_q"], mp4)
        wall = statistics.median(walls)
        R[arm] = {"version": ver, "crf": crf, "lookahead": la,
                  "bytes": os.path.getsize(bs), "hevc": bs, "mp4": mp4,
                  "v1_json": js, "wall": wall, "walls": walls,
                  "cpu": statistics.median(cpus), "fps": frames / wall,
                  "round_bytes": [x["bytes"] for x in runs[arm]],
                  "v1": v1, "cmd_full": arms[arm](bs),
                  "cmd": shown(arms[arm](name + ".hevc"), sample)}
    o, x = R["ours"], R["x265"]
    for arm in (o, x):
        if arm["v1"]["frames"] != frames:
            log(f"  warning: VMAF scored {arm['v1']['frames']} frames, the "
                f"sample has {frames}")
    R["dsize"] = o["bytes"] / x["bytes"] - 1
    R["matched"] = abs(R["dsize"]) <= TOL
    R["speed_ratio"] = x["wall"] / o["wall"]
    R["busy"] = load0 > max(1.0, cores / 4)
    R["verdict"] = verdict(R["speed_ratio"], o["v1"]["mean"] - x["v1"]["mean"],
                           R["dsize"], R["matched"],
                           (load0, cores) if R["busy"] else None)
    R["sub"] = (f"{info['w']}x{info['h']}, {desc}, both --preset medium, "
                f"{R['threads_label']}")
    R["what"] = what_block(R)

    card = card_svg(R)
    cs = os.path.join(outdir, "card.svg")
    with open(cs, "w") as f:
        f.write(card)
    png = os.path.join(outdir, "card.png")
    R["card"] = {"svg": cs, "png": png if svg_to_png(cs, png) else None}
    if not R["card"]["png"]:
        R["card"]["page"] = os.path.join(outdir, "card.html")
        with open(R["card"]["page"], "w") as f:
            f.write(CARD_PAGE.replace("__SVG__", script_json(card)))

    page = None
    if a.player != "none":
        if t["veo"] and a.player in ("auto", "veo"):
            page = veo_export(t, R, outdir)
            R["player"] = "veo" if page else None
        if not page:
            page = write_player(R, outdir, card)
            R["player"] = "builtin"
    R["page"] = page
    with open(os.path.join(outdir, "result.json"), "w") as f:
        json.dump({k: v for k, v in R.items()}, f, indent=1)

    summary = "\n".join([
        "", f"y265 bench: {R['title']}, {R['sub']}", "", table(R), "",
        R["verdict"], "", "What was compared:",
        "\n".join("  " + s for s in R["what"].splitlines()), "",
        "Files:", f"  {outdir}/result.json",
        f"  {R['card']['png'] or cs}" + ("" if R["card"]["png"] else
                                         f"  (save a PNG from "
                                         f"{R['card']['page']})"),
        f"  {page}" if page else "  (no player)",
        (f"  view it with: veo compare preview {os.path.dirname(page)}"
         if R.get("player") == "veo" else ""), ""])
    with open(os.path.join(outdir, "summary.txt"), "w") as f:
        f.write(summary.lstrip("\n"))
    print(summary)
    if not a.keep:
        shutil.rmtree(wd, ignore_errors=True)
    return 0


if __name__ == "__main__":
    sys.exit(main())
