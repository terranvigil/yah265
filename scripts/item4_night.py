#!/usr/bin/env python3
# Copyright (c) 2026, the yah265 authors
# SPDX-License-Identifier: GPL-2.0-or-later
"""item4_night.py - parity plan item 4, the TIMED half. QUIET BOX ONLY.

Written to be queued in a night block, one group per invocation; nothing
here asks a question. Each group appends to scratch/vbrlf/night.csv and
writes scratch/vbrlf/DONE_<group> when it ends, even after a failed row.
`--plan` prints the commands and the estimate without running anything.

    python3 scripts/item4_night.py vbr-a          720p + 1080p segments  ~45 min
    python3 scripts/item4_night.py vbr-b          chimera + meridian 2160p ~50 min
    python3 scripts/item4_night.py vbr-c          sollevante 2160p       ~65 min
    python3 scripts/item4_night.py lf-a           tears, cosmos, sintel 1080p ~70 min
    python3 scripts/item4_night.py lf-b           bbb, stem2, meridian 1080p  ~75 min
    python3 scripts/item4_night.py lf-c           tears, meridian 2160p windows ~40 min
    python3 scripts/item4_night.py lf-d           nocturne, lash 1080p    ~75 min
    python3 scripts/item4_night.py lf-e           nocturne, lash 720p     ~45 min
    python3 scripts/item4_night.py lf-f           nocturne 2160p window   ~60 min

VBR walls (vbr-*). The matrix's shape (~/media/throughput/timed2.sh): both
encoders as shipped, reading the staged segment on stdin through lf_feed.py's
y4m feeder (the matrix read by path; enclab PARITY-SPEC wants one feeder
into stdin for both), at the cell's
rung-2 rate (x265 crf 23's bytes as kbit/s) with maxrate 1.5x and bufsize
2x. Auto threads, two rounds interleaved (one for Meridian's 90 s), then
our `--rc-lookahead 20` arm at auto (one round), then one thread over the
first 60 s (1440 frames at 24p, 1798 at 59.94p). The bytes each run wrote
are kept in the row: x265's VBV output depends on its pool size, so the
default-pool bytes are not the untimed pools-6 bytes.

Long-form walls (lf-*). Per cell, FIRST the feeder alone (VideoToolbox,
three decoders, into /dev/null). Then both encoders at auto threads from
the feeder, in CRF at the untimed matched point (`vbr_lf.py match`: x265
at its default crf 28, ours solved onto its bytes on the same frames) and
in VBR at x265 crf 28's rate. A cell with no matched row is SKIPPED, since
an unmatched cell is not spec-valid. Spec section 4 rule 2: if the feed
alone delivers under 1.5x the faster encoder's frame rate, the streamed
ratio is FEEDER-BOUND, and the excerpt is staged to the INTERNAL disk
(the T7 reads ~290 fps of raw 1080p, under 1.5x our auto rate) and both
are timed again from the file (lf_feed.py y4m through a pipe). Staging
keeps the internal disk above FREE_FLOOR_GB (60); when the excerpt does
not fit, the CENTRED sub-window that does is staged and the row's source
says so. At 2160p the excerpt is never streamed for time: the centred
60 s window the matched point was solved on (vbr_lf.timed_span) is
staged on the T7 (~74 fps of raw 4K, over 1.5x either encoder at 4K)
and timed from the file. One thread runs on a 60 s window (at most 1800
frames; 15 s and 360 frames at 2160p)
from the middle of the excerpt, fed from the pipe
(the feeder is far faster than any one-thread encoder). Every staged
file is deleted before the next cell.
"""
import hashlib, json, math, os, shutil, subprocess, sys, tempfile, time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "scripts"))
import vbr_lf as v                                               # noqa: E402

OUT = os.path.join(ROOT, "scratch", "vbrlf")
CSVP = os.path.join(OUT, "night.csv")
STAGE = os.environ.get("STAGE_DIR", "<staging>/yah265")
STAGE_INT = os.environ.get("STAGE_INT", os.path.join(OUT, "stage"))
N265 = v.N265
X265 = v.X265
FREE_FLOOR_GB = float(os.environ.get("FREE_FLOOR_GB", "60"))
STAGE_MAX_GB = float(os.environ.get("STAGE_MAX_GB", "150"))
T1_SECS = 60
FEED_WORKERS = os.environ.get("FEED_WORKERS", "3")
PLAN = "--plan" in sys.argv

GROUPS = {
    "vbr-a": [("seg", f"{t}_{r}") for r in ("720p", "1080p")
              for t in ("chimera", "sollevante", "meridian")],
    "vbr-b": [("seg", "chimera_2160p"), ("seg", "meridian_2160p")],
    "vbr-c": [("seg", "sollevante_2160p")],
    "lf-a": [("lf", f"{t}_1080p") for t in ("tears", "cosmos", "sintel")],
    "lf-b": [("lf", f"{t}_1080p") for t in ("bbb", "stem2", "meridian")],
    "lf-c": [("lf", f"{t}_2160p") for t in ("tears", "meridian")],
    "lf-d": [("lf", "nocturne_1080p"), ("lf", "lash_1080p")],
    "lf-e": [("lf", "nocturne_720p"), ("lf", "lash_720p")],
    "lf-f": [("lf", "nocturne_2160p")],
}
COLS = ("group,cell,arm,mode,q,threads,source,start,frames,round,real_s,"
        "user_s,sys_s,maxrss_mb,bytes,md5,valid,load_before,when").split(",")


def load1():
    return float(os.getloadavg()[0])


def free_gb(path):
    return shutil.disk_usage(path).free / 1e9


def row(**kw):
    if PLAN:
        return
    new = not os.path.exists(CSVP)
    with open(CSVP, "a") as f:
        if new:
            f.write(",".join(COLS) + "\n")
        f.write(",".join(str(kw.get(c, "")) for c in COLS) + "\n")
    print("  " + " ".join(f"{c}={kw.get(c, '')}" for c in COLS[1:14]),
          flush=True)


def timed(cmd, stdin_cmd=None):
    """/usr/bin/time -l around cmd (and only cmd); stdin from stdin_cmd's
    stdout when given. Returns real, user, sys, rss_mb."""
    if PLAN:
        print("    " + (" ".join(stdin_cmd) + " | " if stdin_cmd else "")
              + " ".join(cmd))
        return 0, 0, 0, 0
    fd, errp = tempfile.mkstemp(dir=OUT); os.close(fd)
    fp = None
    with open(errp, "w") as ef:
        if stdin_cmd:
            fp = subprocess.Popen(stdin_cmd, stdout=subprocess.PIPE,
                                  stderr=subprocess.DEVNULL)
        r = subprocess.run(["/usr/bin/time", "-l"] + cmd,
                           stdin=fp.stdout if fp else None,
                           stdout=subprocess.DEVNULL, stderr=ef)
        if fp:
            fp.stdout.close(); fp.wait()
    txt = open(errp, errors="replace").read().replace("\r", "\n")
    os.unlink(errp)
    real = user = sys_ = rss = None
    for ln in txt.splitlines():
        p = ln.split()
        if len(p) >= 6 and p[1] == "real":
            real, user, sys_ = float(p[0]), float(p[2]), float(p[4])
        if "maximum resident set size" in ln:
            rss = round(int(p[0]) / 1048576)
    if r.returncode != 0 or real is None or (fp and fp.returncode not in (0, None)):
        raise RuntimeError(f"rc {r.returncode}: {' '.join(cmd[:6])} ...\n"
                           f"{txt[-500:]}")
    return real, user, sys_, rss


def md5(p):
    if PLAN or not os.path.exists(p):
        return ""
    h = hashlib.md5()
    with open(p, "rb") as f:
        for b in iter(lambda: f.read(1 << 20), b""):
            h.update(b)
    return h.hexdigest()


def size(p):
    try:
        return os.path.getsize(p)
    except OSError:
        return ""

# ---------------------------------------------------------------- VBR walls


def seg_cmd(arm, k, out, threads):
    """Reads y4m on stdin, from the one feeder (enclab PARITY-SPEC)."""
    vbv = ["--bitrate", str(k), "--vbv-maxrate", str(round(k * v.MAXR)),
           "--vbv-bufsize", str(round(k * v.BUFR))]
    if arm == "x265":
        c = [X265, "--preset", "medium", *vbv, "--log-level", "none"]
        if threads == "1":
            c += ["--pools", "none", "--frame-threads", "1"]
        return c + ["-o", out, "--input", "-", "--y4m"]
    c = [N265, "-", "--preset", "medium", *vbv, "--aq", "1", "--cutree", "1",
         "--threads", "0" if threads == "auto" else threads, "-o", out]
    if arm == "ours20":
        c += ["--rc-lookahead", "20"]
    return c


def vbr_cell(group, cell):
    c = v.seg_cell(cell)
    k = v.seg_rates(cell)[1]
    src = c["src"]
    fps = c["fps"]
    n1 = 1798 if cell.startswith("meridian") else 1440   # the matrix's
    out = os.path.join(OUT, "night.hevc")
    rounds = 1 if cell.startswith("meridian") else 2
    print(f"== {cell}: VBR {k} kb/s, {c['frames']} frames, load {load1():.2f}",
          flush=True)
    staged = None
    if not src.startswith(v.TP + os.sep):
        # the 720p/1080p segments live on the T7 since 2026-09-26, which
        # reads ~290 fps of raw 1080p: under 1.5x our auto rate, so the
        # segment is copied to the internal disk for the run (spec
        # section 2: G3 at 1080p reads internal) and deleted after
        need = os.path.getsize(src) / 1e9
        if free_gb(OUT) - need < FREE_FLOOR_GB:
            print(f"  SKIP {cell}: {need:.1f} GB would take the internal "
                  f"disk under {FREE_FLOOR_GB:.0f} GB free", flush=True)
            return
        staged = os.path.join(STAGE_INT, os.path.basename(src))
        if PLAN:
            print(f"    copy {src} -> {staged}")
        else:
            os.makedirs(STAGE_INT, exist_ok=True)
            shutil.copyfile(src, staged)
        src = staged
    if not PLAN:
        subprocess.run(["cat", src], stdout=subprocess.DEVNULL)
    try:
        vbr_runs(group, cell, c, k, src, n1, out, rounds)
    finally:
        if staged and os.path.exists(staged):
            os.unlink(staged)


def vbr_runs(group, cell, c, k, src, n1, out, rounds):
    plan = []
    for r in range(1, rounds + 1):
        arms = ("x265", "ours") if r % 2 else ("ours", "x265")
        plan += [(a, "auto", r, None) for a in arms]
    plan += [("ours20", "auto", 1, None), ("ours", "1", 1, n1),
             ("x265", "1", 1, n1)]
    for arm, thr, r, fr in plan:
        l0 = load1()
        try:
            real, user, sy, rss = timed(seg_cmd(arm, k, out, thr),
                                        stdin_cmd=feed_file(src, 0, fr or 0))
        except RuntimeError as e:
            print(f"FAILED {cell} {arm} t{thr}: {e}", flush=True)
            continue
        row(group=group, cell=cell, arm=arm, mode="vbr", q=k, threads=thr,
            valid="single-run" if (thr == "1" or arm == "ours20" or
                                   rounds == 1) else "",
            source="y4m", start=0, frames=fr or c["frames"], round=r,
            real_s=real, user_s=user, sys_s=sy, maxrss_mb=rss,
            bytes=size(out), md5=md5(out), load_before=f"{l0:.2f}",
            when=time.strftime("%H:%M:%S"))
        if os.path.exists(out):
            os.unlink(out)

# ---------------------------------------------------------------- long-form


def lf_cmd(arm, mode, q, out, threads):
    if arm == "x265":
        c = [X265, "--preset", "medium", "--log-level", "none"]
        if threads == "1":
            c += ["--pools", "none", "--frame-threads", "1"]
    else:
        c = [N265, "-", "--preset", "medium", "--aq", "1", "--cutree", "1",
             "--threads", "0" if threads == "auto" else threads]
    if mode == "crf":
        c += ["--crf", str(q)]
    else:
        c += ["--bitrate", str(q), "--vbv-maxrate", str(round(q * v.MAXR)),
              "--vbv-bufsize", str(round(q * v.BUFR))]
    c += ["-o", out]
    if arm == "x265":
        c += ["--input", "-", "--y4m"]
    return c


def feed_vt(title, res, start, count, workers):
    f = v.feeder_cmd(title, res)
    f[f.index("--start") + 1] = str(start)
    f[f.index("--count") + 1] = str(count)
    f[f.index("--workers") + 1] = str(workers)
    return f


def feed_file(path, start=0, count=0):
    return ["python3", v.FEED, "y4m", "--src", path, "--start", str(start),
            "--count", str(count)]


def lf_cell(group, cell):
    title, res = cell.rsplit("_", 1)
    f = v.LF[title]
    fps = f["fps"][0] / f["fps"][1]
    s0, n = v.lf_span(title)
    w, h = v.lf_geom(title, res)
    per = w * h * 3 // 2 + 6
    out = os.path.join(OUT, "night.hevc")
    m = v.load_memo().get(f"match|{cell}")
    ts, tn = v.timed_span(title, res)
    if not m or (m["start"], m["count"]) != (ts, tn):
        # an unmatched cell is not spec-valid: don't spend the night on it
        print(f"== {cell}: SKIP, no untimed matched point for frames "
              f"{ts}+{tn} (vbr_lf.py match {cell})", flush=True)
        return
    xq, oq = m["x265_crf"], m["our_crf"]
    k = round(m["x265_kbps"])
    print(f"== {cell}: {w}x{h}, frames {ts}+{tn}, x265 crf {xq} / ours crf "
          f"{oq} ({m['size_pct']:+.2f}% bytes untimed), VBR {k} kb/s, load "
          f"{load1():.2f}", flush=True)
    arms = [("x265", "crf", xq), ("ours", "crf", oq), ("x265", "vbr", k),
            ("ours", "vbr", k)]

    def leg(source, feed, start, count, thr="auto", tag=""):
        walls = {}
        l0 = load1()
        try:
            real, user, sy, rss = timed(feed)
            row(group=group, cell=cell, arm="feeder", mode="-", q="-",
                threads="-", source=source, start=start, frames=count,
                round=1, real_s=real, user_s=user, sys_s=sy, maxrss_mb=rss,
                load_before=f"{l0:.2f}", when=time.strftime("%H:%M:%S"))
            walls["feeder"] = real
        except RuntimeError as e:
            print(f"FAILED feeder {cell} {source}: {e}", flush=True)
        for arm, mode, q in arms:
            l0 = load1()
            try:
                real, user, sy, rss = timed(lf_cmd(arm, mode, q, out, thr),
                                            stdin_cmd=feed)
            except RuntimeError as e:
                print(f"FAILED {cell} {arm} {mode}: {e}", flush=True)
                continue
            row(group=group, cell=cell, arm=arm, mode=mode, q=q, threads=thr,
                source=source + tag, start=start, frames=count, round=1,
                valid="single-run",
                real_s=real, user_s=user, sys_s=sy, maxrss_mb=rss,
                bytes=size(out), md5=md5(out), load_before=f"{l0:.2f}",
                when=time.strftime("%H:%M:%S"))
            walls[(arm, mode)] = real
            if os.path.exists(out):
                os.unlink(out)
        return walls

    def stage(start, count, where):
        """Stage [start, start+count) under `where` if it fits over the
        60 GB floor, else the CENTRED sub-window that does. Returns
        (path, first frame, count) or (None, 0, 0)."""
        if not PLAN:
            os.makedirs(where, exist_ok=True)
        room = min(free_gb(where if os.path.isdir(where) else
                           os.path.dirname(where)) - FREE_FLOOR_GB,
                   STAGE_MAX_GB)
        c = min(count, int(room * 1e9 // per))
        if c < round(10 * fps):
            print(f"  stage: {room:.1f} GB over the floor at {where}, too "
                  "little", flush=True)
            return None, 0, 0
        s1 = start + (count - c) // 2
        p = os.path.join(where, f"{cell}.y4m")
        if PLAN:
            print(f"    stage {s1}+{c} ({c * per / 1e9:.1f} GB) to {p}")
            return p, s1, c
        with open(p, "wb") as fo:
            r = subprocess.run(feed_vt(title, res, s1, c, FEED_WORKERS),
                               stdout=fo, stderr=subprocess.PIPE)
        if r.returncode != 0:
            os.unlink(p)
            print(f"  stage failed: {r.stderr[-300:]}", flush=True)
            return None, 0, 0
        return p, s1, c

    def fps_of(wall, count):
        return count / wall if wall else 0.0

    if res != "2160p":
        walls = leg("vt", feed_vt(title, res, s0, n, FEED_WORKERS), s0, n)
        fw = walls.get("feeder")
        enc = [x for kk, x in walls.items() if kk != "feeder"]
        # spec section 4 rule 2: the feed must deliver 1.5x the faster
        # encoder's frame rate, or the cell is FEEDER-BOUND
        bound = (fw is None or not enc or
                 fps_of(fw, n) < 1.5 * max(fps_of(x, n) for x in enc))
        if PLAN or bound:
            print(f"  feeder {fw} s against encoders {enc}: FEEDER-BOUND on "
                  "the VT feed, staging to the internal disk", flush=True)
            # the T7 reads ~290 fps of raw 1080p, under 1.5x our auto rate,
            # so a staged 720p/1080p excerpt goes to the internal disk
            p, s1, c = stage(s0, n, STAGE_INT)
            if p:
                leg("y4m", feed_file(p, 0, c), s1, c,
                    tag="-internal" if c == n else "-internal-centred-subwindow")
                if os.path.exists(p):
                    os.unlink(p)
    else:
        # 2160p: the T7 feeds ~74 fps of raw 4K, over 1.5x either encoder's
        # auto rate at 4K, and the matched 60 s window is 45 GB
        p, s1, c = stage(ts, tn, STAGE)
        if p:
            leg("y4m", feed_file(p, 0, c), s1, c,
                tag="-t7-window" if c == tn else "-t7-centred-subwindow")
            if os.path.exists(p):
                os.unlink(p)
    # one thread, a 60 s window from the middle, from the pipe
    # 60 s at 24/30p, capped at 1800 frames (30 s at 60p, the matrix's
    # Meridian window); at 2160p 15 s, capped at 360 frames
    n1 = min(n, round(T1_SECS * fps), 1800) if res != "2160p" else \
        min(n, round(T1_SECS / 4 * fps), 360)
    ws = s0 + (n - n1) // 2
    feed = feed_vt(title, res, ws, n1, "1")
    for arm, mode, q in arms:
        l0 = load1()
        try:
            real, user, sy, rss = timed(lf_cmd(arm, mode, q, out, "1"),
                                        stdin_cmd=feed)
        except RuntimeError as e:
            print(f"FAILED {cell} {arm} {mode} t1: {e}", flush=True)
            continue
        row(group=group, cell=cell, arm=arm, mode=mode, q=q, threads="1",
            source="vt-window", start=ws, frames=n1, round=1, real_s=real,
            valid="single-run",
            user_s=user, sys_s=sy, maxrss_mb=rss, bytes=size(out),
            load_before=f"{l0:.2f}", when=time.strftime("%H:%M:%S"))
        if os.path.exists(out):
            os.unlink(out)


def main():
    args = [a for a in sys.argv[1:] if a != "--plan"]
    if len(args) != 1 or args[0] not in GROUPS:
        sys.exit(__doc__)
    g = args[0]
    os.makedirs(OUT, exist_ok=True)
    done = os.path.join(OUT, f"DONE_{g}")
    if os.path.exists(done) and not PLAN:
        os.unlink(done)
    print(f"item4_night {g} start {time.strftime('%Y-%m-%d %H:%M:%S')} "
          f"load {load1():.2f}", flush=True)
    for kind, cell in GROUPS[g]:
        try:
            (vbr_cell if kind == "seg" else lf_cell)(g, cell)
        except Exception as e:                                  # noqa: BLE001
            print(f"FAILED {cell}: {e!r}", flush=True)
    if not PLAN:
        with open(done, "w") as f:
            f.write(time.strftime("%Y-%m-%d %H:%M:%S") + "\n")
    print(f"item4_night {g} done {time.strftime('%H:%M:%S')}", flush=True)


if __name__ == "__main__":
    main()
