# Copyright (c) 2026, the yah265 authors
# SPDX-License-Identifier: GPL-2.0-or-later
"""The interleaved wall A/B loop shared by scripts/wall_ab.py (env arms on
one binary) and scripts/bin_ab.py (two binaries). Ported from yah264's
bench/wall_ab.py and bench/bin_ab.py, whose discipline is the one that
survived their campaign:

  - every arm runs once per round, round-robin, the start rotated each
    round, so whatever the box does to "the first run" lands on every arm
  - medians, never minima
  - a duplicate of the baseline rides along as `ctrl`: it runs the same
    command as `base`, so any gap between the two is the box drifting,
    and a result smaller than that gap is not a result
  - a cell whose arm spread (max/min over its runs) is above SPREAD is
    VOID
  - timed runs write to /dev/null (yah264 measured 0.5-8.6 s of variable
    file I/O inside a 720p wall when the stream went to disk); the md5
    per cell comes from one extra UNTIMED encode per arm
  - instructions retired are read off every timed run beside the wall
"""
import hashlib
import os
import statistics
import subprocess
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import boxlib                                             # noqa: E402
import scratch                                            # noqa: E402

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
CORPUS = os.path.join(ROOT, "tests/corpus")

# name -> (corpus clip, frames, crf). The CRFs are the goal-1 solved points
# from docs/data/goal-legs.csv (2026-10-04), so a cell sits near the
# operating point the goals are read at; the HD frame counts are short
# because an A/B needs five rounds of every arm.
CLIPS = {
    "foreman": ("foreman_cif", 120, "23"),
    "mobile":  ("mobile_cif", 120, "27"),
    "stefan":  ("stefan_cif", 120, "28"),
    "bus":     ("bus_cif", 120, "30"),
    "akiyo":   ("akiyo_cif", 120, "21"),
    "pjoy":    ("park_joy_720p", 60, "21"),
    "ducks":   ("ducks_720p", 60, "20"),
    "sintel":  ("sintel_720p", 60, "18"),
    "crowd":   ("crowd_run_1080p", 30, "22"),
    # all-intra: the intra decision loops are invisible on an
    # inter-dominated cell
    "allI":    ("foreman_cif", 60, "23", ["--keyint", "1"]),
}


def clip_of(name):
    c = CLIPS.get(name)
    if c is None:
        sys.exit(f"unknown cell {name!r}; known: {', '.join(CLIPS)}")
    return c


def strip_knobs(env):
    """The environment with every N265_* removed: the baseline is the
    binary's shipped default, never whatever the caller's shell held."""
    gone = sorted(k for k in env if k.startswith("N265_"))
    return {k: v for k, v in env.items() if k not in gone}, gone


def parse_arm(spec):
    """'N265_A=1 N265_B=2 --flag v' -> (env dict, argv list). A token of
    the form NAME=VALUE is an environment setting; anything else is a CLI
    argument, so an arm can be a flag the binary takes."""
    env, argv = {}, []
    for t in spec.split():
        n, eq, v = t.partition("=")
        if eq and n and (n[0].isalpha() or n[0] == "_") \
                and all(ch.isalnum() or ch == "_" for ch in n):
            env[n] = v
        else:
            argv.append(t)
    return env, argv


def cmd_of(binary, cell, threads, preset, extra, out):
    c = clip_of(cell)
    clip, nf, crf = c[0], c[1], c[2]
    tail = c[3] if len(c) > 3 else []
    nf = int(os.environ.get("FRAMES", nf))
    return ([binary, os.path.join(CORPUS, clip + ".y4m"), "--frames",
             str(nf), "--crf", crf, "--preset", preset, "--threads",
             str(threads)] + tail + list(extra) + ["-o", out])


def md5(path):
    h = hashlib.md5()
    with open(path, "rb") as f:
        for b in iter(lambda: f.read(1 << 20), b""):
            h.update(b)
    return h.hexdigest()


def preflight(label):
    """Untimed work waits for nobody, but a timed one wants the box: report
    the box CPU, and refuse when QUIET_GATE=1 and the box is busy."""
    if os.environ.get("QUIET_GATE", "0") == "1":
        ok, s = boxlib.quiet()
        print(f"-- {boxlib.fmt_quiet(ok, s)}", flush=True)
        if not ok:
            sys.exit(f"{label}: box not quiet (QUIET_GATE=1)")
    else:
        ok, s = boxlib.quiet(samples=1, secs=2)
        print(f"-- {boxlib.fmt_quiet(ok, s)} (one 2 s window; "
              f"QUIET_GATE=1 for the 3 x 10 s gate)", flush=True)


def run(arms, cells, threads, preset, runs, spread_max, want_md5, label,
        base="base"):
    """arms: [(name, binary, env, extra argv)]. Prints one block per cell,
    returns {cell: {arm: (median wall, median instr, spread, md5)}}."""
    wd = scratch.mkdtemp("ab")
    res = {}
    with boxlib.timing_lock(label):
        for cell in cells:
            clip_of(cell)
            t = {a[0]: [] for a in arms}
            n = {a[0]: [] for a in arms}
            for r in range(runs):
                k = r % len(arms)
                for name, binary, env, extra in arms[k:] + arms[:k]:
                    m = boxlib.measure(cmd_of(binary, cell, threads, preset,
                                              extra, os.devnull), env=env)
                    if m["rc"] != 0:
                        sys.exit(f"{label}: {cell} arm {name} exited "
                                 f"{m['rc']}")
                    t[name].append(m["wall"])
                    n[name].append(m["instr"])
            h = {}
            if want_md5:
                for name, binary, env, extra in arms:
                    out = os.path.join(wd, f"{name}.hevc")
                    subprocess.run(cmd_of(binary, cell, threads, preset,
                                          extra, out), env=env, check=True,
                                   stdout=subprocess.DEVNULL,
                                   stderr=subprocess.DEVNULL)
                    h[name] = md5(out)
                    os.unlink(out)
            res[cell] = {}
            mb = statistics.median(t[base])
            ib = [x for x in n[base] if x]
            ib = statistics.median(ib) if ib else None
            for name, _b, env, extra in arms:
                med = statistics.median(t[name])
                sp = max(t[name]) / min(t[name])
                iv = [x for x in n[name] if x]
                im = statistics.median(iv) if iv else None
                g = 100.0 * (mb - med) / mb
                ig = (f"{100.0 * (im - ib) / ib:+6.2f}%" if im and ib
                      else "     -")
                void = " VOID" if sp > spread_max else "     "
                tag = ""
                if h:
                    tag = ("  md5 SAME   " if h[name] == h[base]
                           else "  md5 DIFFERS")
                # what this arm sets beyond the baseline's own settings
                benv, bext = arms[0][2], arms[0][3]
                desc = " ".join([f"{k}={v}" for k, v in env.items()
                                 if benv.get(k) != v]
                                + list(extra[len(bext):]))
                print(f"{cell:<8} {name:<5} {med * 1e3:8.0f} ms  {g:+7.2f}%  "
                      f"sp {sp:.3f}{void}  instr "
                      f"{boxlib.fmt_g(im):>8} {ig}{tag}   "
                      f"{desc or '(default)'}", flush=True)
                res[cell][name] = (med, im, sp, h.get(name))
    return res


def legend(base="base"):
    print(f"gain: ({base} - arm) / {base} on the median wall; NEGATIVE means "
          f"the arm is SLOWER. instr: median instructions retired and the "
          f"arm's change against {base} (positive = more work). ctrl is a "
          f"second {base}: its gain is the box's drift, so a gain inside "
          f"it is noise. VOID = that arm's max/min over its runs passed "
          f"SPREAD.")
