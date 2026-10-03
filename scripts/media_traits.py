#!/usr/bin/env python3
# Copyright (c) 2026, the yah265 authors
# SPDX-License-Identifier: GPL-2.0-or-later
"""media_traits.py - what each test title is, before anything is encoded.

A test cell whose number moves because of a quirk of its first second
misleads the decision it is read for. Meridian 720p opens on 59 black
frames, and item 29 step 2's whole Meridian loss sat in what those frames
did to the rate control's operating point. This census measures, for
every title the harness reads, the attributes that can steer a rate
control or a BD reading, and writes them to docs/data/media-traits.csv.
docs/media-traits.md says what each column means and which readings each
trait can mislead.

The titles: the matrix segments (~/media/throughput), the probe windows
(~/media/probe, sliced out of their segment's per-frame series, since they
are byte cuts of it), the shared tier S (<media>/tier_s), the
long-form excerpts (vbr_lf.py's table, the 600 s spans, streamed through
the same feeder at 1080p geometry) and the 17-clip corpus.

Every trait comes off the luma plane. One pass per source reads each frame
once and keeps a per-frame series (cached under ~/.cache/yah265-traits,
keyed on the source's path, size and mtime); the CSV is a reduction of the
series. The flat type is the encoder's own definition, reproduced bit for
bit at the source's resolution (la.c, n265_la_flat_pct) and checked against
N265_FLAT_TRACE by `check-flat`. Everything else is read on a box-averaged
copy about 320 samples wide, except the noise estimate, which needs the
full resolution and reads a sample of frames.

    python3 scripts/media_traits.py scan [name ...]   per-frame series
    python3 scripts/media_traits.py csv               write the CSV
    python3 scripts/media_traits.py show [name ...]   flags, one line each
    python3 scripts/media_traits.py list              the titles
    python3 scripts/media_traits.py check-flat clip.y4m [frames]

Library use (the harness): `flags(name)` and `lead_skip(name)`.
"""
import csv, glob, hashlib, json, math, os, re, subprocess, sys

import numpy as np

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "scripts"))
CACHE = os.path.expanduser(os.environ.get("TRAITS_CACHE",
                                          "~/.cache/yah265-traits"))
CSV_PATH = os.path.join(ROOT, "docs", "data", "media-traits.csv")
TP = os.path.expanduser("~/media/throughput")
TP_T7 = "<staging>/yah265/throughput"
PROBE = os.path.expanduser("~/media/probe")
TIER_S = "<media>/tier_s"
CORPUS = os.path.join(ROOT, "tests", "corpus")
N265 = os.environ.get("N265", os.path.join(ROOT, "build", "n265"))

# ---------------------------------------------------------------- thresholds
# Every threshold the CSV's flags rest on. docs/media-traits.md restates
# them; change one there too.

SMALL_W = 320          # analysis copy width (box average)
BLACK_P99 = 40         # black frame: 99th percentile of luma at or under
BLACK_MEAN = 24        # ... and mean luma at or under (8-bit codes)
STILL_MAD = 0.1        # still frame: mean |Y - Y(prev)| under, small copy
SCENE_CUT = 0.3        # cut: the scene score over this (enclab scene scan)
FADE_LEN = 8           # transition window, frames
FADE_END_MAD = 6.0     # ... its endpoints at least this far apart
FADE_RESID = 0.15      # ... every inner frame within this share of it
FADE_STEP = 0.35       # ... and no one step more than this share (a cut)
XFADE_MIN = 12         # ... a dissolve run counts from this many frames
TINY_K = 8             # the dissolve test reads the small copy / 8 (~40 wide)
FADE_RAMP = 6          # fade: a monotone mean-luma ramp of at least this
                       # many frames into or out of a near-black frame
FADE_NEAR_P99 = 48     # near-black: p99 at or under
FLAT_GATE = 10         # the encoder's N265_FLAT_GATE default
NOISE_SAMPLES = 240    # frames the noise estimator reads, at most

# flags
F_LEAD_S = 0.25        # a leading/trailing black or still run of at least
                       # this many seconds (and at least 6 frames)
F_FADE_SHARE = 0.05    # transitions over this share of frames
F_CUTS_PER_MIN = 20.0  # dense cutting
F_DARK_P99 = 150       # dark: pooled p99 of non-black frames at or under
F_NARROW = 150         # narrow: pooled p99 - p01 at or under (lifted
                       # blacks, compressed highlights: PQ read as SDR)
F_FLAT_MIX = (0.02, 0.98)   # flat-typed share strictly inside this, of the
                            # non-black frames: the flat tools switch
F_NOISE = 2.0          # grain/noise: sigma at or over (8-bit codes)

# ---------------------------------------------------------------- titles


def _seg_sources():
    p2 = json.load(open(os.path.join(TP, "points2.json")))
    out = {}
    for cell, v in p2.items():
        name = f"{v['title']}_{v['res']}_{v['seg']}.y4m"
        src = os.path.join(TP, name)
        if not os.path.exists(src):
            src = os.path.join(TP_T7, name)
        out[f"seg/{cell}"] = dict(kind="y4m", path=src, tier="segment",
                                  fps=v["fps"])
    return out


def _probe_sources(segs):
    out = {}
    parent = {"mer1080": "seg/meridian_1080p", "sol1080": "seg/sollevante_1080p"}
    for p in sorted(glob.glob(os.path.join(PROBE, "*_w*.y4m"))):
        m = re.match(r"(\w+?)_w(\d+)\.y4m$", os.path.basename(p))
        if not m or m.group(1) not in parent:
            continue
        w, h, fps, n = y4m_info(p)
        out[f"probe/{m.group(1)}_w{m.group(2)}"] = dict(
            kind="slice", parent=parent[m.group(1)], first=int(m.group(2)),
            count=n, path=p, tier="probe", fps=fps)
    return out


def _dir_sources(d, tier, prefix):
    out = {}
    for p in sorted(glob.glob(os.path.join(d, "*.y4m"))):
        out[f"{prefix}/{os.path.basename(p)[:-4]}"] = dict(
            kind="y4m", path=p, tier=tier)
    return out


def _lf_sources():
    import vbr_lf as v
    out = {}
    for t, f in v.LF.items():
        res = "1080p"
        start, count = v.lf_span(t)
        out[f"lf/{t}_{res}"] = dict(
            kind="feed", tier="longform", title=t, res=res,
            feed=v.feeder_cmd(t, res), master=v.lf_master(t),
            start=start, count=count, fps=f["fps"][0] / f["fps"][1])
    return out


def titles():
    t = _seg_sources()
    t.update(_probe_sources(t))
    t.update(_dir_sources(TIER_S, "tierS", "tierS"))
    t.update(_lf_sources())
    t.update(_dir_sources(CORPUS, "corpus", "corpus"))
    return t

# ---------------------------------------------------------------- reading


def y4m_info(path):
    with open(path, "rb") as f:
        hdr = f.readline().decode("ascii", "replace")
    w = int(re.search(r" W(\d+)", hdr).group(1))
    h = int(re.search(r" H(\d+)", hdr).group(1))
    m = re.search(r" F(\d+):(\d+)", hdr)
    fps = int(m.group(1)) / int(m.group(2)) if m else 25.0
    fsz = 6 + w * h * 3 // 2
    n = (os.path.getsize(path) - len(hdr) - 1) // fsz
    return w, h, fps, n


def y4m_frames(fh):
    """(w, h, fps, generator of luma planes) from a y4m byte stream."""
    hdr = fh.readline().decode("ascii", "replace")
    w = int(re.search(r" W(\d+)", hdr).group(1))
    h = int(re.search(r" H(\d+)", hdr).group(1))
    m = re.search(r" F(\d+):(\d+)", hdr)
    fps = int(m.group(1)) / int(m.group(2)) if m else 25.0
    if re.search(r" C(\S+)", hdr) and not re.search(r" C420", hdr):
        raise RuntimeError("only 8-bit 4:2:0 y4m: " + hdr.strip())
    ysz, csz = w * h, (w // 2) * (h // 2) * 2

    def gen():
        while True:
            line = fh.readline()
            if not line:
                return
            if not line.startswith(b"FRAME"):
                raise RuntimeError("y4m: lost frame sync")
            y = fh.read(ysz)
            c = fh.read(csz)
            if len(y) != ysz or len(c) != csz:
                return
            yield np.frombuffer(y, np.uint8).reshape(h, w)
    return w, h, fps, gen()

# ---------------------------------------------------------------- per frame


def flat_pct(y):
    """n265_la_flat_pct at 8 bits: whole 16x16 blocks of the displayed
    picture, flat when range < 32 and 256*s2 - s1^2 < 65536."""
    h, w = y.shape
    bh, bw = h >> 4, w >> 4
    if bh <= 0 or bw <= 0:
        return 0
    b = y[:bh * 16, :bw * 16].reshape(bh, 16, bw, 16)
    lo = b.min(axis=(1, 3)).astype(np.int32)
    hi = b.max(axis=(1, 3)).astype(np.int32)
    bi = b.astype(np.int64)
    s1 = bi.sum(axis=(1, 3))
    s2 = (bi * bi).sum(axis=(1, 3))
    flat = ((hi - lo) < 32) & ((s2 * 256 - s1 * s1) < 65536)
    return int(flat.sum()) * 100 // (bw * bh)


def small(y):
    h, w = y.shape
    k = max(1, round(w / SMALL_W))
    hh, ww = h // k * k, w // k * k
    return y[:hh, :ww].reshape(hh // k, k, ww // k, k).mean(
        axis=(1, 3), dtype=np.float32)


def noise_sigma(y):
    """Immerkaer's estimator, per 16x16 block, the 20th percentile over
    blocks whose mean is off the clip rails (bars and blown highlights
    read 0 and would pull it down)."""
    f = y.astype(np.float32)
    c = (f[:-2, :-2] - 2 * f[:-2, 1:-1] + f[:-2, 2:]
         - 2 * f[1:-1, :-2] + 4 * f[1:-1, 1:-1] - 2 * f[1:-1, 2:]
         + f[2:, :-2] - 2 * f[2:, 1:-1] + f[2:, 2:])
    a = np.abs(c)
    h, w = a.shape
    bh, bw = h // 16, w // 16
    if bh == 0 or bw == 0:
        return float("nan")
    am = a[:bh * 16, :bw * 16].reshape(bh, 16, bw, 16).mean(axis=(1, 3))
    ym = f[1:bh * 16 + 1, 1:bw * 16 + 1].reshape(bh, 16, bw, 16).mean(
        axis=(1, 3))
    keep = (ym > 24) & (ym < 230)
    if keep.sum() < 8:
        return float("nan")
    s = am[keep] * math.sqrt(math.pi / 2) / 6.0
    return float(np.percentile(s, 20))


def scan_frames(frames, n_hint):
    """Per-frame series from an iterator of luma planes."""
    stride = max(1, (n_hint or 1) // NOISE_SAMPLES)
    cols = {k: [] for k in ("flat_pct", "ymean", "yp01", "yp50", "yp99",
                            "mad", "noise")}
    hist = np.zeros(256, np.int64)
    ring = []          # last FADE_LEN + 1 small planes
    prev = None
    trans = []         # (window end, residual share) of dissolve tests
    for i, y in enumerate(frames):
        s = small(y)
        cols["flat_pct"].append(flat_pct(y))
        hq = np.bincount(np.clip(s + 0.5, 0, 255).astype(np.uint8).ravel(),
                         minlength=256)
        cdf = np.cumsum(hq) / hq.sum()
        cols["ymean"].append(float(s.mean()))
        cols["yp01"].append(int(np.searchsorted(cdf, 0.01)))
        cols["yp50"].append(int(np.searchsorted(cdf, 0.50)))
        cols["yp99"].append(int(np.searchsorted(cdf, 0.99)))
        cols["mad"].append(float(np.abs(s - prev).mean()) if prev is not None
                           else float("nan"))
        cols["noise"].append(noise_sigma(y) if i % stride == 0
                             else float("nan"))
        th, tw = s.shape[0] // TINY_K * TINY_K, s.shape[1] // TINY_K * TINY_K
        ring.append(s[:th, :tw].reshape(th // TINY_K, TINY_K, tw // TINY_K,
                                        TINY_K).mean(axis=(1, 3)))
        if len(ring) > FADE_LEN + 1:
            ring.pop(0)
        if len(ring) == FADE_LEN + 1:
            a, b = ring[0], ring[-1]
            d = b - a
            end = float(np.abs(d).mean())
            if end >= FADE_END_MAD:
                st = max(float(np.abs(ring[j] - ring[j - 1]).mean())
                         for j in range(1, FADE_LEN + 1))
                if st <= FADE_STEP * end:
                    r = max(float(np.abs(ring[j] - (a + d * (j / FADE_LEN)))
                                  .mean()) for j in range(1, FADE_LEN))
                    trans.append((i, r / end))
        prev = s
    out = {k: np.array(v, np.float64) for k, v in cols.items()}
    # the dissolve test's residual share for the window ending at each
    # frame (inf where it doesn't apply), so its threshold is a reduce-time
    # choice and not a rescan
    xr = np.full(len(out["ymean"]), np.inf)
    for i, r in trans:
        xr[i] = r
    out["xres"] = xr
    return out


def scene_scores(mad):
    """ffmpeg's select scene score on the small copy's luma:
    min(mafd, |mafd - prev mafd|) / 100, clipped to [0, 1]."""
    s = np.zeros(len(mad))
    pm = 0.0
    for i in range(1, len(mad)):
        m = mad[i]
        s[i] = min(1.0, max(0.0, min(m, abs(m - pm)) / 100.0))
        pm = m
    return s

# ---------------------------------------------------------------- cache


def _key(t):
    if t["kind"] == "feed":
        sig = " ".join(os.path.basename(x) if x.startswith("/") else x
                       for x in t["feed"])
        st = os.stat(t["master"])
        sig += f"|{st.st_size}"
    else:
        st = os.stat(t["path"])
        sig = f"{os.path.realpath(t['path'])}|{st.st_size}|{int(st.st_mtime)}"
    return hashlib.sha1(sig.encode()).hexdigest()[:16]


def _cache_path(name, t):
    return os.path.join(CACHE, name.replace("/", "__") + "-" + _key(t) + ".npz")


def load_series(name, t=None):
    t = t or titles()[name]
    if t["kind"] == "slice":
        p = load_series(t["parent"])
        if p is None:
            return None
        a, n = t["first"], t["count"]
        out = {k: v[a:a + n].copy() for k, v in p.items() if k != "meta"}
        out["mad"][0] = float("nan")
        out["meta"] = dict(p["meta"], first=a, count=n)
        return out
    cp = _cache_path(name, t)
    if not os.path.exists(cp):
        return None
    z = np.load(cp, allow_pickle=False)
    out = {k: z[k] for k in z.files if k != "meta"}
    out["meta"] = json.loads(str(z["meta"]))
    return out


def scan(name, t):
    if t["kind"] == "slice":
        return
    cp = _cache_path(name, t)
    if os.path.exists(cp):
        print(f"  {name}: cached", flush=True)
        return
    os.makedirs(CACHE, exist_ok=True)
    if t["kind"] == "feed":
        p = subprocess.Popen(t["feed"], stdout=subprocess.PIPE,
                             stderr=subprocess.PIPE, bufsize=1 << 22)
        w, h, fps, gen = y4m_frames(p.stdout)
        ser = scan_frames(gen, t["count"])
        p.stdout.close()
        err = p.stderr.read()
        if p.wait() != 0:
            raise RuntimeError(f"{name}: feeder rc {p.returncode}: "
                               f"{err[-300:]}")
        if len(ser["ymean"]) != t["count"]:
            raise RuntimeError(f"{name}: {len(ser['ymean'])} frames, "
                               f"wanted {t['count']}")
    else:
        _, _, _, n = y4m_info(t["path"])
        with open(t["path"], "rb", buffering=1 << 22) as fh:
            w, h, fps, gen = y4m_frames(fh)
            ser = scan_frames(gen, n)
    meta = dict(w=w, h=h, fps=fps, frames=len(ser["ymean"]))
    np.savez_compressed(cp, meta=json.dumps(meta), **ser)
    print(f"  {name}: {w}x{h} {meta['frames']} frames", flush=True)

# ---------------------------------------------------------------- reduce


def _run(mask):
    n = 0
    for v in mask:
        if not v:
            break
        n += 1
    return n


def reduce(name, ser):
    m = ser["meta"]
    fps = m["fps"]
    n = len(ser["ymean"])
    black = (ser["yp99"] <= BLACK_P99) & (ser["ymean"] <= BLACK_MEAN)
    mad = ser["mad"]
    still = np.zeros(n, bool)
    still[1:] = mad[1:] < STILL_MAD
    # a still run counts its first frame: frames 0..k with every step still
    lead_still = _run(still[1:]) + 1 if n > 1 and still[1] else 0
    trail_still = _run(still[::-1]) + 1 if n > 1 and still[-1] else 0
    lead_black, trail_black = _run(black), _run(black[::-1])
    sc = scene_scores(mad)
    cut_idx = np.nonzero(sc > SCENE_CUT)[0]
    # fades: a monotone mean-luma ramp into or out of a near-black frame
    # (motion doesn't break it); dissolves: the tiny copy's linear test
    near = ser["yp99"] <= FADE_NEAR_P99
    ym = ser["ymean"]
    infade = np.zeros(n, bool)
    fades = 0
    for i in range(n):
        if not near[i] or (i > 0 and near[i - 1] and i + 1 < n and near[i + 1]):
            continue
        for step in (-1, 1):
            j = i
            while 0 <= j + step < n and ym[j + step] > ym[j] + 0.5:
                j += step
            if abs(j - i) >= FADE_RAMP:
                a, b = sorted((i, j))
                if not infade[a:b + 1].all():
                    fades += 1
                infade[a:b + 1] = True
    tr = np.zeros(n, bool)
    for i in np.nonzero(ser["xres"] <= FADE_RESID)[0]:
        tr[max(0, i - FADE_LEN):i + 1] = True
    tr &= ~infade
    xf = []
    i = 0
    while i < n:
        if tr[i]:
            j = i
            while j + 1 < n and tr[j + 1]:
                j += 1
            if j - i + 1 >= XFADE_MIN:
                xf.append((i, j))
            i = j + 1
        else:
            i += 1
    nb = ~black
    flat = ser["flat_pct"] >= FLAT_GATE
    flat_share = float(flat.mean())
    flat_nb = float(flat[nb].mean()) if nb.any() else float("nan")
    lead_flat = _run(flat)
    if nb.any():
        p01 = float(np.percentile(ser["yp01"][nb], 50))
        p50 = float(np.percentile(ser["yp50"][nb], 50))
        p99 = float(np.percentile(ser["yp99"][nb], 50))
        dark_share = float((ser["yp99"][nb] <= 128).mean())
    else:
        p01 = p50 = p99 = dark_share = float("nan")
    nz = ser["noise"][nb]
    nz = nz[~np.isnan(nz)]
    noise = float(np.median(nz)) if len(nz) else float("nan")
    secs = n / fps
    lead_skip = max(lead_black, lead_still)
    r = dict(
        name=name, w=m["w"], h=m["h"], fps=round(fps, 3), frames=n,
        seconds=round(secs, 2),
        lead_black=lead_black, lead_still=lead_still,
        trail_black=trail_black, trail_still=trail_still,
        lead_skip=lead_skip, black_share=round(float(black.mean()), 4),
        fades=fades, fade_frames=int(infade.sum()),
        xfades=len(xf), xfade_frames=int(sum(b - a + 1 for a, b in xf)),
        cuts=len(cut_idx), cuts_per_min=round(len(cut_idx) / secs * 60, 2),
        flat_share=round(flat_share, 4),
        flat_share_nonblack=round(flat_nb, 4) if flat_nb == flat_nb else "",
        lead_flat=lead_flat,
        y_p01=p01, y_p50=p50, y_p99=p99, dark_share=round(dark_share, 4),
        noise_sigma=round(noise, 3) if noise == noise else "",
    )
    r["flags"] = " ".join(_flags(r))
    return r


def _flags(r):
    f = []
    run = max(6, math.ceil(F_LEAD_S * r["fps"]))
    if r["lead_black"] >= run:
        f.append(f"lead_black:{r['lead_black']}")
    if r["lead_still"] - r["lead_black"] >= run:
        f.append(f"lead_still:{r['lead_still']}")
    if r["trail_black"] >= run:
        f.append(f"trail_black:{r['trail_black']}")
    if r["trail_still"] - r["trail_black"] >= run:
        f.append(f"trail_still:{r['trail_still']}")
    tf = (r["fade_frames"] + r["xfade_frames"]) / r["frames"]
    if tf >= F_FADE_SHARE:
        f.append(f"fades:{r['fades']}+{r['xfades']}x/{100 * tf:.0f}%")
    if r["cuts_per_min"] >= F_CUTS_PER_MIN:
        f.append(f"cuts:{r['cuts_per_min']:.0f}/min")
    fnb = r["flat_share_nonblack"]
    if fnb != "" and F_FLAT_MIX[0] < fnb < F_FLAT_MIX[1]:
        f.append(f"flat_mix:{100 * fnb:.0f}%")
    if r["y_p99"] == r["y_p99"] and r["y_p99"] <= F_DARK_P99:
        f.append(f"dark:p99={r['y_p99']:.0f}")
    if r["y_p99"] == r["y_p99"] and r["y_p99"] - r["y_p01"] <= F_NARROW:
        f.append(f"narrow:{r['y_p01']:.0f}-{r['y_p99']:.0f}")
    if r["noise_sigma"] != "" and r["noise_sigma"] >= F_NOISE:
        f.append(f"noise:{r['noise_sigma']:.1f}")
    return f

# ---------------------------------------------------------------- harness API

_csv = None


def _load_csv():
    global _csv
    if _csv is None:
        _csv = {}
        try:
            for row in csv.DictReader(open(CSV_PATH)):
                _csv[row["name"]] = row
        except OSError:
            pass
    return _csv


def _resolve(name):
    rows = _load_csv()
    for k in (name, f"seg/{name}", f"lf/{name}", f"tierS/{name}",
              f"corpus/{name}", f"probe/{name}"):
        if k in rows:
            return rows[k]
    # a long-form excerpt is censused at one size; its traits are the
    # title's, and lead_skip is in frames, which no size changes
    m = re.match(r"(\w+)_(720p|1080p|2160p)$", name)
    if m and f"lf/{m.group(1)}_1080p" in rows:
        return rows[f"lf/{m.group(1)}_1080p"]
    return None


def flags(name):
    """The flagged traits of a title, as one short string ('' if none,
    '?' if the census has no row for it)."""
    r = _resolve(name)
    return "?" if r is None else r["flags"]


def lead_skip(name):
    """Frames before the content starts (the longer of the leading black
    and still runs); 0 when unflagged or unknown."""
    r = _resolve(name)
    if r is None or "lead_" not in r["flags"]:
        return 0
    return int(r["lead_skip"])

# ---------------------------------------------------------------- commands

COLS = ["name", "tier", "w", "h", "fps", "frames", "seconds", "lead_black",
        "lead_still", "trail_black", "trail_still", "lead_skip",
        "black_share", "fades", "fade_frames", "xfades", "xfade_frames",
        "cuts", "cuts_per_min", "flat_share", "flat_share_nonblack",
        "lead_flat", "y_p01", "y_p50", "y_p99", "dark_share", "noise_sigma",
        "flags"]


def write_csv():
    ts = titles()
    rows = []
    for name, t in ts.items():
        ser = load_series(name, t)
        if ser is None:
            print(f"  {name}: not scanned", file=sys.stderr)
            continue
        r = reduce(name, ser)
        r["tier"] = t["tier"]
        rows.append(r)
    os.makedirs(os.path.dirname(CSV_PATH), exist_ok=True)
    with open(CSV_PATH, "w", newline="") as f:
        w = csv.DictWriter(f, COLS)
        w.writeheader()
        for r in rows:
            w.writerow({k: (f"{v:.0f}" if isinstance(v, float) and
                            k.startswith("y_p") else v)
                        for k, v in r.items() if k in COLS})
    print(f"wrote {CSV_PATH} ({len(rows)} titles)")


def check_flat(path, frames):
    """Our flat_pct against the encoder's N265_FLAT_TRACE, frame by frame."""
    env = dict(os.environ, N265_FLAT_TRACE="1")
    r = subprocess.run([N265, path, "--frames", str(frames), "--preset",
                        "fast", "--threads", "1", "-o", "/dev/null"],
                       env=env, capture_output=True, text=True)
    enc = {}
    for line in r.stderr.splitlines():
        if line.startswith("FLT,"):
            p = line.split(",")
            enc[int(p[1])] = (int(p[3]), int(p[4]))
    ours = {}
    with open(path, "rb") as fh:
        _, _, _, gen = y4m_frames(fh)
        for i, y in enumerate(gen):
            if i >= frames:
                break
            v = flat_pct(y)
            ours[i] = (v, int(v >= FLAT_GATE))
    bad = [i for i in ours if enc.get(i) != ours[i]]
    print(f"{os.path.basename(path)}: {len(ours)} frames, encoder rows "
          f"{len(enc)}, mismatches {len(bad)}"
          + (f" (first {bad[:5]}: ours {[ours[i] for i in bad[:5]]} "
             f"enc {[enc.get(i) for i in bad[:5]]})" if bad else ""))
    return not bad


def main():
    a = sys.argv[1:]
    if not a:
        sys.exit(__doc__)
    cmd, args = a[0], a[1:]
    ts = titles()
    if cmd == "list":
        for k, t in ts.items():
            print(k, t.get("path", t.get("master", "")), t["tier"])
    elif cmd == "scan":
        sel = [k for k in ts if not args or any(
            k == x or k.startswith(x) for x in args)]
        for k in sel:
            try:
                scan(k, ts[k])
            except (RuntimeError, OSError) as e:
                print(f"FAILED {k}: {e}", flush=True)
    elif cmd == "csv":
        write_csv()
    elif cmd == "show":
        for k in ts:
            if args and not any(k == x or k.startswith(x) for x in args):
                continue
            ser = load_series(k, ts[k])
            if ser is None:
                continue
            r = reduce(k, ser)
            print(f"{k:<34} skip {r['lead_skip']:>4}  {r['flags']}")
    elif cmd == "check-flat":
        ok = all(check_flat(p, int(args[-1]) if args[-1].isdigit() else 60)
                 for p in args if not p.isdigit())
        sys.exit(0 if ok else 1)
    else:
        sys.exit(__doc__)


if __name__ == "__main__":
    main()
