#!/usr/bin/env python3
# Copyright (c) 2026, the yah265 authors
# SPDX-License-Identifier: GPL-2.0-or-later
"""v1score.py - the one VMAF v1 scorer the harnesses share.

Every harness that reads v1 scores through score() here, so they all
read it by the same rules (the owner's):

  display   A source smaller than 1920x1080 in both dimensions is scored
            scaled up, bicubic and aspect kept, to fit 1920x1080. A source
            at 1080p or above is scored at its own size, so a 4K source is
            scored at 3840x2160. The reference is the source scaled to that
            display size. Each decode, at whatever size it was coded, is
            scaled to the same display size, bicubic.
  depth     Both sides are converted to 10 bits (yuv420p10le). CAMBI is
            told the decode's coded size and depth (cambi.enc_width,
            enc_height, enc_bitdepth).
  model     vmaf_v1.0.16 3d0h at 1080p and below, 1d5h_2160 above 1080p,
            and the _hfr twin of either at 50 fps and up. The model follows
            the source, never the rung.
  frames    Every frame is scored. A stride exists for long cells, and it
            must be coprime with the mini-GOP.

Nothing scaled lands on disk: the decode goes through a fifo and the
reference through libvmaf's stdin.

As a command it prints the pooled v1 of one bitstream:

    scripts/v1score.py SRC.y4m BITSTREAM [--json OUT]

Env: V1_BIN (or PL_V1_BIN) names the float-enabled libvmaf, V1_DIR (or
PL_V1_DIR) the model directory. V1S_CACHE names the per-frame memo
(default ~/.cache/yah265-v1score, 0 disables it).
"""
import argparse, hashlib, json, os, subprocess, sys, tempfile

V1_BIN = os.path.expanduser(os.environ.get("V1_BIN") or os.environ.get(
    "PL_V1_BIN", "~/.local/opt/vmaf-v1/bin/vmaf"))
V1_DIR = os.path.expanduser(os.environ.get("V1_DIR") or os.environ.get(
    "PL_V1_DIR", "~/.local/share/vmaf/model/vmaf_v1.0.16"))
DISP = (1920, 1080)
SCALER = "bicubic"
HFR_FPS = 49.9
CACHE = os.environ.get("V1S_CACHE", os.path.expanduser("~/.cache/yah265-v1score"))
if CACHE == "0":
    CACHE = None


def y4m_geom(path):
    """(w, h, fps, bits) from a Y4M header."""
    with open(path, "rb") as f:
        tags = f.readline().decode("ascii", "replace").split()[1:]
    g = {t[:1]: t[1:] for t in tags}
    n, d = (int(x) for x in g.get("F", "25:1").split(":"))
    c = g.get("C", "420")
    bits = 10 if "p10" in c else 12 if "p12" in c else 8
    return int(g["W"]), int(g["H"]), n / d, bits


def upscaled(w, h):
    return w < DISP[0] and h < DISP[1]


def display_size(w, h):
    """The size a source of w x h is scored at. Below 1080p in both
    dimensions it is the fit into 1920x1080 with ffmpeg's own rounding
    (force_original_aspect_ratio=decrease, force_divisible_by=2), so a
    decode at the source's size scales to the pixels parity_long's filter
    always gave."""
    if not upscaled(w, h):
        return w, h
    dw, dh = DISP
    tw = (dh * w + h // 2) // h
    th = (dw * h + w // 2) // w
    return min(tw, dw) // 2 * 2, min(th, dh) // 2 * 2


def disp_tag(w, h):
    return "fit1080" if upscaled(w, h) else "native"


def model_name(w, h):
    return "1d5h_2160" if h > 1080 else "3d0h"


def is_hfr(fps):
    return fps >= HFR_FPS


def model_path(name, hfr):
    if hfr:
        return f"{V1_DIR}_hfr/vmaf_v1.0.16_hfr_{name}.json"
    return f"{V1_DIR}/vmaf_v1.0.16_{name}.json"


def gate_model(w, h, fps):
    """(name, path, hfr) of the model a source of w x h at fps is read on."""
    name, hfr = model_name(w, h), is_hfr(fps)
    return name, model_path(name, hfr), hfr


def probe(bitstream):
    """(w, h, bits) of a bitstream's coded pictures."""
    r = subprocess.run(
        ["ffprobe", "-v", "error", "-select_streams", "v:0", "-show_entries",
         "stream=width,height,pix_fmt", "-of", "json", bitstream],
        capture_output=True, text=True, check=True)
    s = json.loads(r.stdout)["streams"][0]
    pf = s.get("pix_fmt", "")
    return int(s["width"]), int(s["height"]), 10 if "10" in pf else 8


def frame_sizes(bitstream):
    """The packet size of every picture, in display order."""
    r = subprocess.run(
        ["ffprobe", "-v", "error", "-select_streams", "v:0", "-show_entries",
         "frame=pkt_size", "-of", "csv=p=0", bitstream],
        capture_output=True, text=True, check=True)
    return [int(x.strip().rstrip(",")) for x in r.stdout.split()
            if x.strip().rstrip(",").isdigit()]


def _scale(w, h, dw, dh):
    if (w, h) == (dw, dh):
        return []
    return ["-vf", f"scale={dw}:{dh}:flags={SCALER}"]


def _ten():
    return ["-pix_fmt", "yuv420p10le", "-strict", "-1", "-f", "yuv4mpegpipe"]


def _sha(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for b in iter(lambda: f.read(1 << 20), b""):
            h.update(b)
    return h.hexdigest()


def score(ref, bitstream, src_w, src_h, fps, enc_w=None, enc_h=None,
          enc_bits=None, models=None, frames=None, stride=1, threads="4",
          ff_threads="2", display=None, wd=None, cache=False, vmaf=None):
    """Score BITSTREAM against REF on v1.

    REF is a source Y4M path, or the argv of a process writing the source
    as Y4M on stdout. src_w and src_h are the
    source's size and pick the display size and the model; enc_w, enc_h
    and enc_bits are the decode's (probed when left out). MODELS is a list
    of (name, path) and defaults to the source's gate model, named "v1".
    FRAMES caps a path reference. DISPLAY overrides the display size and
    VMAF the libvmaf binary.

    Returns {"frames": libvmaf's per-frame list, "pooled": its pooled
    metrics, "disp": (w, h), "models": [names], "enc": (w, h, bits)}.
    """
    if enc_w is None or enc_h is None or enc_bits is None:
        pw, ph, pb = probe(bitstream)
        enc_w = pw if enc_w is None else enc_w
        enc_h = ph if enc_h is None else enc_h
        enc_bits = pb if enc_bits is None else enc_bits
    dw, dh = display or display_size(src_w, src_h)
    if abs(enc_w * src_h - enc_h * src_w) > 0.02 * src_w * enc_h:
        sys.exit(f"v1score: {bitstream} is {enc_w}x{enc_h}, not the "
                 f"source's {src_w}x{src_h} aspect")
    # libvmaf's CAMBI refuses these coded sizes
    if min(enc_w, enc_h) < 144 or max(enc_w, enc_h) < 216:
        sys.exit(f"v1score: {bitstream} is {enc_w}x{enc_h}; CAMBI needs both "
                 "sides at 144 or more and one at 216 or more")
    if models is None:
        models = [("v1", gate_model(src_w, src_h, fps)[1])]
    ck = None
    if cache and CACHE:
        k = [_sha(bitstream), ref if isinstance(ref, str) else list(ref),
             os.path.getsize(ref) if isinstance(ref, str) else None,
             [(n, os.path.basename(p)) for n, p in models], [dw, dh], SCALER,
             [enc_w, enc_h, enc_bits], frames, stride, [src_w, src_h]]
        os.makedirs(CACHE, exist_ok=True)
        ck = os.path.join(CACHE, hashlib.sha256(
            json.dumps(k).encode()).hexdigest()[:24] + ".json")
        if os.path.exists(ck):
            try:
                with open(ck) as f:
                    return json.load(f)
            except (OSError, ValueError):
                pass
    own = wd is None
    wd = wd or tempfile.mkdtemp(prefix="v1score.")
    dfifo = os.path.join(wd, "dec10.fifo")
    if os.path.exists(dfifo):
        os.remove(dfifo)
    os.mkfifo(dfifo)
    j = os.path.join(wd, "v1.json")
    dec = subprocess.Popen(
        ["ffmpeg", "-v", "error", "-y", "-threads", ff_threads, "-i",
         bitstream] + _scale(enc_w, enc_h, dw, dh) + _ten() + [dfifo],
        stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
    if isinstance(ref, str):
        fp = None
        cv = subprocess.Popen(
            ["ffmpeg", "-v", "error", "-i", ref]
            + (["-frames:v", str(frames)] if frames else [])
            + _scale(src_w, src_h, dw, dh) + _ten() + ["-"],
            stdout=subprocess.PIPE, stderr=subprocess.DEVNULL)
    else:
        fp = subprocess.Popen(ref, stdout=subprocess.PIPE,
                              stderr=subprocess.DEVNULL)
        cv = subprocess.Popen(
            ["ffmpeg", "-v", "error", "-f", "yuv4mpegpipe", "-i", "-"]
            + _scale(src_w, src_h, dw, dh) + _ten() + ["-"],
            stdin=fp.stdout, stdout=subprocess.PIPE,
            stderr=subprocess.DEVNULL)
        fp.stdout.close()
    margs = []
    for name, path in models:
        margs += ["--model", f"path={path}:name={name}"
                  f":cambi.enc_bitdepth={enc_bits}"
                  f":cambi.enc_width={enc_w}:cambi.enc_height={enc_h}"]
    v = subprocess.run(
        [vmaf or V1_BIN, "--threads", str(threads), "--subsample", str(stride),
         "-r", "/dev/stdin", "-d", dfifo, "--json", "-o", j, "-q"] + margs,
        stdin=cv.stdout, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
    cv.stdout.close()
    cv.wait()
    if fp:
        fp.wait()
    if v.returncode != 0 and dec.poll() is None:
        dec.kill()
    dec.wait()
    os.remove(dfifo)
    if v.returncode != 0:
        sys.exit(f"v1score: vmaf v1 failed on {bitstream}\n  "
                 f"{v.stderr.decode(errors='replace')[-600:]}")
    with open(j) as f:
        jj = json.load(f)
    os.remove(j)
    if own:
        os.rmdir(wd)
    out = {"frames": jj["frames"], "pooled": jj["pooled_metrics"],
           "disp": [dw, dh], "models": [n for n, _ in models],
           "enc": [enc_w, enc_h, enc_bits]}
    if ck:
        tmp = ck + f".{os.getpid()}.tmp"
        with open(tmp, "w") as f:
            json.dump(out, f)
        os.replace(tmp, ck)
    return out


def per_frame(src, bitstream, threads="4", cache=True, frames=None):
    """The gate model's per-frame v1 of BITSTREAM against the source Y4M
    SRC, with what it was read on: (scores, info)."""
    w, h, fps, _ = y4m_geom(src)
    r = score(src, bitstream, w, h, fps, threads=threads, cache=cache,
              frames=frames)
    name, path, hfr = gate_model(w, h, fps)
    info = {"model": os.path.basename(path)[:-5], "hfr": hfr,
            "disp": "%dx%d" % tuple(r["disp"]),
            "enc": "%dx%d" % tuple(r["enc"][:2]), "enc_bits": r["enc"][2]}
    return [fr["metrics"]["v1"] for fr in r["frames"]], info


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("src")
    ap.add_argument("bitstream")
    ap.add_argument("--threads", default=os.environ.get("VMAF_THREADS", "4"))
    ap.add_argument("--frames", type=int)
    ap.add_argument("--json", help="write the per-frame scores here")
    a = ap.parse_args()
    pf, info = per_frame(a.src, a.bitstream, a.threads, frames=a.frames)
    mean = sum(pf) / len(pf)
    print(f"{os.path.basename(a.bitstream)}: v1 {mean:.6f} over {len(pf)} "
          f"frames ({info['model']}, display {info['disp']}, coded "
          f"{info['enc']})")
    if a.json:
        with open(a.json, "w") as f:
            json.dump({"v1": mean, "frames": pf, **info}, f)


if __name__ == "__main__":
    main()
