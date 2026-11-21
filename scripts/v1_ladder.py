#!/usr/bin/env python3
# Copyright (c) 2026, the yah265 authors
# SPDX-License-Identifier: GPL-2.0-or-later
"""v1_ladder.py - BD on VMAF v1 against x265 medium, with v1's own features.

VMAF v1 (vmaf_v1.0.16) predicts from four features only: CAMBI (banding),
SpEED chroma (speed_chroma_uv), ADM3 (detail) and motion3. This reads a
five-rung ladder on both encoders, prints BD-v1 (and BD-PSNR-Y with --psnr), and then
says WHICH feature carries the gap. It re-evaluates the model's SVR on the
per-frame features (the model file is the source; the re-evaluation is
checked against libvmaf's own score on every rung), interpolates x265's
per-frame features onto our rate (log rate, between its two bracketing
rungs), and swaps one feature at a time to x265's: the v1 gain from each
swap is that feature's share of the gap at matched rate.

Scoring is the parity pipeline's (parity_long.py score_v1): both sides at
10 bits (yuv420p10le), CAMBI told the encode is 8-bit and its coded size
(cambi.enc_bitdepth=8, enc_width, enc_height), the 3d0h model at 1080p and
below and 1d5h_2160 above it (the _hfr twins at 50 fps and up), every
frame. v1 is read at DISPLAY resolution (owner rule 10-25): a picture
smaller than 1920x1080 in both dimensions is scaled up, bicubic and
aspect kept, to fit 1920x1080, reference and decode alike. V1_NATIVE=1
scores at the coded size instead. PSNR-Y is always read at the coded
size, in a pass of its own.

    scripts/v1_ladder.py SRC.y4m [--arm LABEL] [--env N265_X=1 ...]
                         [--vbr] [--rungs 20,23,26,29,32] [--frames N]
                         [-- ours flags]

--vbr runs both sides at x265's CRF rungs' achieved rates, with
--vbv-maxrate 1.5x and --vbv-bufsize 2x. Encodes and scores are memoized
under ~/.cache/yah265-v1 (V1_CACHE), keyed by the command, the source and
our binary's hash. THREADS (default 4) is each encoder's and libvmaf's
thread count; BIN overrides build-rel/y265.
"""
import argparse, hashlib, json, math, os, subprocess, sys, time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "scripts"))
from bdcompare import bd_rate  # noqa: E402

V1 = os.path.expanduser(os.environ.get("V1_BIN", "~/.local/opt/vmaf-v1/bin/vmaf"))
MD = os.path.expanduser("~/.local/share/vmaf/model/vmaf_v1.0.16")
CACHE = os.path.expanduser(os.environ.get("V1_CACHE", "~/.cache/yah265-v1"))
LOCK = os.path.expanduser("~/.cache/enclab/TIMING_ACTIVE")
THR = os.environ.get("THREADS", "4")
BIN = os.environ.get("BIN", os.path.join(ROOT, "build-rel", "y265"))
FEATS = [("cambi", "cambi"), ("speed_chroma_uv", "chroma"),
         ("integer_adm3", "adm3"), ("integer_motion3", "motion3")]
# the owner's display rule (10-25), as parity_long.py V1_DISP
DISP = (1920, 1080)


def geom(src):
    with open(src, "rb") as f:
        h = f.readline().split()
    g = {t[:1]: t[1:] for t in h[1:]}
    fps = g.get(b"F", b"24000:1001").split(b":")
    return int(g[b"W"]), int(g[b"H"]), int(fps[0]) / int(fps[1])


def sha(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for b in iter(lambda: f.read(1 << 20), b""):
            h.update(b)
    return h.hexdigest()[:16]


def ff_y4m(inp, frames, up, fifo):
    """ffmpeg writing INP to FIFO as y4m: 10 bits when UP is set, scaled
    to fit the display (parity_long.py score_v1's filter) when the picture
    is smaller than it in both dimensions; native 8 bits otherwise."""
    vf = []
    if up == "fit":
        vf = ["-vf", f"scale={DISP[0]}:{DISP[1]}:flags=bicubic"
              ":force_original_aspect_ratio=decrease:force_divisible_by=2"]
    pf = "yuv420p10le" if up else "yuv420p"
    fr = ["-frames:v", str(frames)] if frames else []
    return subprocess.Popen(["ffmpeg", "-v", "error", "-y", "-threads", "2", "-i", inp]
                            + fr + vf + ["-pix_fmt", pf, "-strict", "-1", "-f",
                                         "yuv4mpegpipe", fifo], stdin=subprocess.DEVNULL)


def vmaf_pass(src, bs, frames, up, args, wd, tag):
    """One libvmaf run, reference and decode each fed by ffmpeg through a
    fifo, so nothing scaled lands on disk."""
    rf, df = os.path.join(wd, tag + ".r.fifo"), os.path.join(wd, tag + ".d.fifo")
    for f in (rf, df):
        if os.path.exists(f):
            os.remove(f)
        os.mkfifo(f)
    vj = os.path.join(wd, tag + ".json")
    pr = ff_y4m(src, frames, up, rf)
    pd = ff_y4m(bs, None, up, df)
    subprocess.run([V1, "--threads", THR, "-r", rf, "-d", df, "--json", "-o", vj, "-q"]
                   + args, check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    pr.wait()
    pd.wait()
    for f in (rf, df):
        os.remove(f)
    fr = json.load(open(vj))["frames"]
    os.remove(vj)
    return fr


def disp_mode(w, h):
    if os.environ.get("V1_NATIVE") == "1":
        return "native"
    return "fit" if w < DISP[0] and h < DISP[1] else "native"


def rate_args(mode, q):
    if mode == "crf":
        return ["--crf", str(q)]
    k = int(q)
    return ["--bitrate", str(k), "--vbv-maxrate", str(int(round(1.5 * k))),
            "--vbv-bufsize", str(int(round(2 * k)))]


def rung(src, enc, mode, q, env, flags, binh, frames=None):
    w, h, fps = geom(src)
    fl = ["--frames", str(frames)] if frames else []
    if enc == "ours":
        cmd = [BIN, src, "--preset", "medium", "--threads", THR] + rate_args(mode, q) + fl + flags
    else:
        cmd = ["x265", "--input", src, "--preset", "medium"] + rate_args(mode, q) + fl + [
            "--pools", THR, "--frame-threads", "4", "--log-level", "error"]
    dm = disp_mode(w, h)
    # owner rule: 1d5h_2160 above 1080p, 3d0h at and below it
    sub = (MD + "_hfr", "vmaf_v1.0.16_hfr_") if fps >= 49.9 else (MD, "vmaf_v1.0.16_")
    model = os.path.join(sub[0], sub[1] + ("1d5h_2160" if h > 1080 else "3d0h") + ".json")
    k = [cmd, sorted(env.items()), src, binh if enc == "ours" else "x265", "disp:" + dm]
    # before the rule the switch was at 3840x1600 pixels: key the sizes whose
    # model changed (1440p) on it, so their old scores aren't reused, and
    # leave every other size's cached scores valid
    if (h > 1080) != (w * h >= 3840 * 1600):
        k.append(os.path.basename(model))
    key = hashlib.sha256(json.dumps(k).encode()).hexdigest()[:20]
    js = os.path.join(CACHE, key + ".json")
    if os.path.exists(js):
        return json.load(open(js))
    os.makedirs(CACHE, exist_ok=True)
    while os.path.exists(LOCK):
        time.sleep(30)
    bs = os.path.join(CACHE, key + ".hevc")
    e = dict(os.environ)
    e.update(env)
    subprocess.run(cmd + ["-o", bs], env=e, check=True,
                   stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    fr = vmaf_pass(src, bs, frames, dm if dm == "fit" else "10", [
        "--model", f"path={model}:name=v1:cambi.enc_bitdepth=8"
        f":cambi.enc_width={w}:cambi.enc_height={h}"], CACHE, key + ".v1")
    fp = vmaf_pass(src, bs, frames, None, ["--no_prediction", "--feature", "psnr"],
                   CACHE, key + ".ps")
    keys = list(fr[0]["metrics"])
    cols = {}
    for f, short in FEATS:
        cols[short] = [k for k in keys if k.startswith(f)
                       and "_u_" not in k and "_v_" not in k][0]
    out = {"cmd": cmd, "env": env, "bytes": os.path.getsize(bs),
           "kbps": os.path.getsize(bs) * 8 / 1000 / (len(fr) / fps),
           "model": model, "disp": dm, "frames": len(fr),
           "v1": sum(x["metrics"]["v1"] for x in fr) / len(fr),
           "psnr_y": sum(x["metrics"]["psnr_y"] for x in fp) / len(fp),
           "feat": {s: [x["metrics"][c] for x in fr] for s, c in cols.items()},
           "v1pf": [x["metrics"]["v1"] for x in fr]}
    os.remove(bs)
    json.dump(out, open(js, "w"))
    return out


def load_model(name):
    import numpy as np
    m = json.load(open(os.path.join(MD, name)))["model_dict"]  # name may be absolute
    lines = m["model"].split("\n")
    gamma = float([l for l in lines if l.startswith("gamma")][0].split()[1])
    rho = float([l for l in lines if l.startswith("rho")][0].split()[1])
    co, sv = [], []
    for l in lines[lines.index("SV") + 1:]:
        t = l.split()
        if not t:
            continue
        co.append(float(t[0]))
        v = [0.0] * 4
        for p in t[1:]:
            k, x = p.split(":")
            v[int(k) - 1] = float(x)
        sv.append(v)
    return dict(gamma=gamma, rho=rho, co=np.array(co), sv=np.array(sv),
                sl=np.array(m["slopes"]), ic=np.array(m["intercepts"]),
                st=m.get("score_transform", {}),
                ccp=float(m.get("chroma_correction_parameter", 0.0)))


def predict(M, X):
    """The model's per-frame score from raw features (frames x 4), with
    the model's chroma-from-luma correction: a chroma feature of exactly 0
    is replaced by C * (1 - adm3). SpEED chroma reads 0 on small pictures
    (every CIF frame at its coded size), so there v1 sees chroma only
    through ADM3. Scaled to the display it reads chroma again."""
    import numpy as np
    X = X.copy()
    z = X[:, 1] == 0.0
    X[z, 1] = M["ccp"] * (1.0 - X[z, 2])
    Xn = X * M["sl"][1:] + M["ic"][1:]
    d = ((Xn[:, None, :] - M["sv"][None, :, :]) ** 2).sum(-1)
    p = (np.exp(-M["gamma"] * d) * M["co"]).sum(-1) - M["rho"]
    p = (p - M["ic"][0]) / M["sl"][0]
    st = M["st"]
    if st.get("enabled"):
        p = st.get("p0", 0) + st.get("p1", 1) * p + st.get("p2", 0) * p * p
    return np.clip(p, 0, 100)


def attrib(ours, ref):
    import numpy as np
    M = load_model(ours[0]["model"])
    F = lambda r: np.array([r["feat"][s] for _, s in FEATS]).T
    ref = sorted(ref, key=lambda r: r["kbps"])
    xl = np.log([r["kbps"] for r in ref])
    print(f"{'kbps':>8} {'v1':>7} {'x265@r':>7} {'gap':>6} | v1 gained by taking "
          f"x265's: " + " ".join(f"{s:>8}" for _, s in FEATS))
    for r in sorted(ours, key=lambda r: r["kbps"]):
        X = F(r)
        base = predict(M, X)
        chk = abs(base.mean() - r["v1"])
        lr = math.log(r["kbps"])
        if lr < xl[0] or lr > xl[-1]:
            print(f"{r['kbps']:8.0f} {r['v1']:7.3f}  outside x265's rates")
            continue
        j = max(0, min(len(xl) - 2, int(np.searchsorted(xl, lr)) - 1))
        t = (lr - xl[j]) / (xl[j + 1] - xl[j])
        A, B = F(ref[j]), F(ref[j + 1])
        n = min(len(X), len(A), len(B))
        XF = A[:n] * (1 - t) + B[:n] * t
        X, base = X[:n], base[:n]
        g = []
        for k in range(4):
            G = X.copy()
            G[:, k] = XF[:, k]
            g.append(predict(M, G).mean() - base.mean())
        xv = predict(M, XF).mean()
        print(f"{r['kbps']:8.0f} {base.mean():7.3f} {xv:7.3f} {base.mean() - xv:+6.3f} | "
              + " ".join(f"{x:+8.3f}" for x in g)
              + ("" if chk < 0.01 else f"   (model check off by {chk:.3f})"))


def main():
    a = sys.argv[1:]
    flags = []
    if "--" in a:
        i = a.index("--")
        flags, a = a[i + 1:], a[:i]
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("src")
    ap.add_argument("--arm", default="ours")
    ap.add_argument("--env", action="append", default=[])
    ap.add_argument("--vbr", action="store_true")
    ap.add_argument("--rungs", default="20,23,26,29,32")
    ap.add_argument("--frames", type=int, default=None)
    ap.add_argument("--psnr", action="store_true", help="also print BD-PSNR-Y")
    o = ap.parse_args(a)
    env = dict(kv.split("=", 1) for kv in o.env)
    binh = sha(BIN)
    rungs = [int(x) for x in o.rungs.split(",")]
    xc = [rung(o.src, "x265", "crf", q, {}, [], binh, o.frames) for q in rungs]
    if o.vbr:
        rates = [int(round(r["kbps"])) for r in xc]
        ref = [rung(o.src, "x265", "vbr", k, {}, [], binh, o.frames) for k in rates]
        ours = [rung(o.src, "ours", "vbr", k, env, flags, binh, o.frames) for k in rates]
    else:
        ref = xc
        ours = [rung(o.src, "ours", "crf", q, env, flags, binh, o.frames) for q in rungs]
    R = lambda s: [r["kbps"] for r in s]
    M = lambda s, k: [r[k] for r in s]
    bv = bd_rate(R(ref), M(ref, "v1"), R(ours), M(ours, "v1"))
    bp = bd_rate(R(ref), M(ref, "psnr_y"), R(ours), M(ours, "psnr_y"))
    fmt = lambda x: "n/a" if x is None else f"{x:+.2f}%"
    print(f"{os.path.basename(o.src)} {o.arm} {'vbr' if o.vbr else 'crf'} "
          f"{' '.join(o.env + flags)}: BD-v1 {fmt(bv)}"
          + (f"  BD-PSNR-Y {fmt(bp)}" if o.psnr else ""))
    attrib(ours, ref)


if __name__ == "__main__":
    main()
