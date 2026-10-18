#!/usr/bin/env python3
# Copyright (c) 2026, the yah265 authors
# SPDX-License-Identifier: GPL-2.0-or-later
"""Shared box helpers for the harnesses: what a run cost, whether the box
is quiet, and whether a timed leg owns it.

    import boxlib
    r = boxlib.measure([n265, src, "--frames", "30", "-o", "/dev/null"])
    r["wall"], r["user"], r["instr"], r["rss"]

    boxlib.quiet()                 # (ok, [three box-CPU samples in %])
    with boxlib.timing_lock("goal_legs G3"):
        ...                        # timed work
    boxlib.wait_untimed("bdcompare")   # untimed work waits for a timed leg

Command line (the shell harnesses use these):

    boxlib.py measure [--json] [--quiet] -- CMD ...
                                            one run, one line of costs
    boxlib.py quiet [--samples 3 --secs 10 --max 150]
                                            exit 0 quiet, 1 not
    boxlib.py lock take [--label L] [--pid P]   exit 1 if a live leg holds it
    boxlib.py lock release [--pid P] [--force]
    boxlib.py lock check                    exit 0 free, 1 held
    boxlib.py lock wait [--what NAME]       block until free (TIMING_WAIT=0
                                            refuses instead of waiting)
"""
import contextlib
import datetime
import json
import os
import platform
import re
import resource
import shutil
import subprocess
import sys
import tempfile
import time

# ------------------------------------------------------------- measure

# instructions retired barely move under load, so a speed item can be read
# on a busy box where the wall cannot (docs/instruments.md, "Instruction
# counts"). macOS reports them through `/usr/bin/time -l`; on Linux the
# counter is `perf stat`, which needs perf_event_paranoid <= 1.
_DARWIN = platform.system() == "Darwin"


def _num(pat, text, cast=float):
    m = re.search(pat, text, re.M)
    return cast(m.group(1)) if m else None


def parse_time_l(text):
    """The fields of a macOS `/usr/bin/time -l` report. x265 ends its
    progress line in a carriage return even at --log-level none, so a
    report that shares a stream with it has its first line glued to that
    progress text: split on \\r as well as \\n before matching."""
    text = text.replace("\r", "\n")
    return {
        "wall": _num(r"^\s*([\d.]+) real\b", text),
        "user": _num(r"\b([\d.]+) user\b", text),
        "sys": _num(r"\b([\d.]+) sys\b", text),
        "rss": _num(r"^\s*(\d+)\s+maximum resident set size", text, int),
        "instr": _num(r"^\s*(\d+)\s+instructions retired", text, int),
        "cycles": _num(r"^\s*(\d+)\s+cycles elapsed", text, int),
        "footprint": _num(r"^\s*(\d+)\s+peak memory footprint", text, int),
    }


def measure(cmd, env=None, stdin=None, stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL, cwd=None, shell=False):
    """Run `cmd` once and return its costs: wall (s, our own stopwatch
    around the child), user and sys CPU (s), instr (instructions retired),
    cycles, rss (bytes), rc. A field the platform cannot read is None.

    The report goes to its own file (`time -o`), never to the child's
    stderr, so an encoder's progress output cannot corrupt it. With
    shell=True the counts cover the whole pipeline the shell waits for
    (a `cat | encoder` feed included), which is what a wall leg pays."""
    argv = ["/bin/sh", "-c", cmd] if shell else list(cmd)
    fd, rep = tempfile.mkstemp(prefix="yah265-time.", suffix=".txt")
    os.close(fd)
    try:
        if _DARWIN:
            full = ["/usr/bin/time", "-l", "-o", rep] + argv
        elif shutil.which("perf"):
            full = ["perf", "stat", "-x,", "-o", rep,
                    "-e", "instructions,cycles"] + argv
        else:
            full = argv
        before = resource.getrusage(resource.RUSAGE_CHILDREN)
        t0 = time.perf_counter()
        p = subprocess.run(full, env=env, stdin=stdin, stdout=stdout,
                           stderr=stderr, cwd=cwd)
        wall = time.perf_counter() - t0
        after = resource.getrusage(resource.RUSAGE_CHILDREN)
        with open(rep, errors="replace") as f:
            text = f.read()
    finally:
        os.unlink(rep)
    r = {"wall": wall, "user": None, "sys": None, "instr": None,
         "cycles": None, "rss": None, "rc": p.returncode}
    if _DARWIN:
        t = parse_time_l(text)
        for k in ("user", "sys", "instr", "cycles", "rss"):
            r[k] = t[k]
    else:
        # getrusage's ru_maxrss is KiB on Linux; the max over every child
        # reaped so far, which is this one when nothing else ran meanwhile
        r["user"] = after.ru_utime - before.ru_utime
        r["sys"] = after.ru_stime - before.ru_stime
        r["rss"] = after.ru_maxrss * 1024
        for ln in text.splitlines():
            f = ln.split(",")
            if len(f) > 2 and f[0].isdigit():
                if f[2].startswith("instructions"):
                    r["instr"] = int(f[0])
                elif f[2].startswith("cycles"):
                    r["cycles"] = int(f[0])
    return r


def fmt_g(n):
    return "-" if n is None else f"{n / 1e9:.3f}G"


def fmt_line(r):
    def s(x, f):
        return "-" if x is None else f.format(x)
    return (f"wall {r['wall']:.3f} s  user {s(r['user'], '{:.2f}')} s  "
            f"sys {s(r['sys'], '{:.2f}')} s  instr {fmt_g(r['instr'])}  "
            f"cycles {fmt_g(r['cycles'])}  "
            f"rss {s(r['rss'] and r['rss'] / 2**20, '{:.1f}')} MB  "
            f"rc {r['rc']}")


# ------------------------------------------------------------- quiet box

# "quiet" is judged on TOTAL box CPU, not the load average (owner rule):
# the one-minute load lags a minute behind and counts threads waiting on
# I/O, so it reads our own previous arm as somebody else and misses a
# fresh all-core job for half a minute. The bar is yah264's: under 150%
# (one and a half cores' worth) in each of three 10 s windows.
QUIET_MAX = float(os.environ.get("QUIET_MAX", "150"))


def ncpu():
    return os.cpu_count() or 1


def box_cpu_samples(samples=3, secs=10):
    """Busy CPU over `samples` consecutive windows of `secs` seconds, each
    in per-core percent (100 = one core flat out, ncpu*100 = all).

    macOS: `top -l N+1 -s secs` prints one CPU line per window, the first
    of which covers no window and is dropped. Elsewhere /proc/stat deltas.
    """
    if _DARWIN:
        p = subprocess.run(["top", "-l", str(int(samples) + 1),
                            "-s", str(max(1, int(round(secs)))), "-n", "0"],
                           capture_output=True, text=True)
        idle = [float(m) for m in
                re.findall(r"CPU usage:.*?([\d.]+)% idle", p.stdout)]
        return [(100.0 - i) * ncpu() for i in idle[1:samples + 1]]
    out = []

    def snap():
        with open("/proc/stat") as f:
            v = [int(x) for x in f.readline().split()[1:]]
        return sum(v), v[3] + (v[4] if len(v) > 4 else 0)
    a = snap()
    for _ in range(samples):
        time.sleep(secs)
        b = snap()
        tot, idl = b[0] - a[0], b[1] - a[1]
        out.append(100.0 * ncpu() * (1 - idl / tot) if tot else 0.0)
        a = b
    return out


def quiet(samples=3, secs=10, limit=None):
    """(ok, samples): ok when every window is under `limit` per-core
    percent. The windows are back to back, so a job that starts or ends
    inside the 30 s is seen in at least one of them."""
    limit = QUIET_MAX if limit is None else limit
    s = box_cpu_samples(samples, secs)
    return (bool(s) and max(s) < limit), s


def fmt_quiet(ok, s, limit=None):
    limit = QUIET_MAX if limit is None else limit
    return (f"box CPU {' / '.join(f'{x:.0f}%' for x in s)} "
            f"(bar < {limit:.0f}% each) {'QUIET' if ok else 'NOT QUIET'}")


# ------------------------------------------------------------- timing lock

# One file says a timed leg owns the box. Timed harnesses take it for the
# length of the leg; untimed harnesses (bands, solves, identity matrices)
# check it before starting NEW work and wait while it is held, so a long
# untimed queue cannot walk into the middle of somebody's stopwatch. The
# default path is outside the repo, under a name the sibling encoders can
# share, because the box is shared and so is the stopwatch.
LOCK = os.environ.get("TIMING_LOCK", os.path.expanduser(
    "~/.cache/enclab/TIMING_ACTIVE"))


def _alive(pid):
    try:
        os.kill(pid, 0)
    except ProcessLookupError:
        return False
    except PermissionError:
        return True
    return True


def lock_holder():
    """The lock's record when a LIVE process holds it, else None. A record
    whose pid is gone (a SIGKILLed leg) is stale and reads as free."""
    try:
        with open(LOCK) as f:
            rec = json.load(f)
    except (OSError, ValueError):
        return None
    pid = rec.get("pid")
    if not isinstance(pid, int) or not _alive(pid):
        return None
    return rec


def _ours(rec):
    """Held by this process or the one that started it: a timed harness
    may run untimed sub-work (a solve) under its own lock."""
    return rec and rec.get("pid") in (os.getpid(), os.getppid())


def take_lock(label, pid=None):
    """Take the lock for `pid` (default this process). False when another
    live process holds it."""
    pid = pid or os.getpid()
    os.makedirs(os.path.dirname(LOCK), exist_ok=True)
    rec = {"pid": pid, "label": label, "host": platform.node(),
           "cwd": os.getcwd(),
           "since": datetime.datetime.now().isoformat(timespec="seconds")}
    for _ in range(2):
        try:
            fd = os.open(LOCK, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o644)
        except FileExistsError:
            cur = lock_holder()
            if cur and cur.get("pid") != pid:
                return False
            # stale, or already ours: replace it
            with contextlib.suppress(FileNotFoundError):
                os.unlink(LOCK)
            continue
        with os.fdopen(fd, "w") as f:
            json.dump(rec, f)
        return True
    return False


def release_lock(pid=None, force=False):
    pid = pid or os.getpid()
    try:
        with open(LOCK) as f:
            rec = json.load(f)
    except (OSError, ValueError):
        rec = None
    if rec is None or force or rec.get("pid") == pid \
            or not _alive(rec.get("pid", -1)):
        with contextlib.suppress(FileNotFoundError):
            os.unlink(LOCK)
        return True
    return False


def describe(rec):
    return (f"pid {rec.get('pid')} '{rec.get('label', '')}' since "
            f"{rec.get('since', '?')} ({rec.get('cwd', '?')})")


@contextlib.contextmanager
def timing_lock(label, wait=None):
    """Hold the lock for a timed leg. If another leg holds it, wait for
    it (TIMING_WAIT=0, or wait=False, refuses instead)."""
    if wait is None:
        wait = os.environ.get("TIMING_WAIT", "1") != "0"
    while not take_lock(label):
        rec = lock_holder()
        if not wait:
            sys.exit(f"{label}: the timing lock is held by "
                     f"{describe(rec or {})}; refusing (TIMING_WAIT=0)")
        print(f"{label}: waiting for the timed leg {describe(rec or {})}",
              file=sys.stderr, flush=True)
        time.sleep(30)
    try:
        yield
    finally:
        release_lock()


def wait_untimed(what, poll=30):
    """Called by an untimed harness before it starts new work: return at
    once when no timed leg holds the box, else wait until it is released.
    TIMING_WAIT=0 exits instead of waiting."""
    told = False
    while True:
        rec = lock_holder()
        if rec is None or _ours(rec):
            if told:
                print(f"{what}: timing lock released, starting",
                      file=sys.stderr, flush=True)
            return
        if os.environ.get("TIMING_WAIT", "1") == "0":
            sys.exit(f"{what}: a timed leg holds the box ({describe(rec)}); "
                     f"refusing to start (TIMING_WAIT=0)")
        if not told:
            print(f"{what}: a timed leg holds the box ({describe(rec)}); "
                  f"waiting for {LOCK} to go", file=sys.stderr, flush=True)
            told = True
        time.sleep(poll)


# ------------------------------------------------------------- cli

def _cli(argv):
    if not argv or argv[0] in ("-h", "--help"):
        print(__doc__)
        return 0
    op, rest = argv[0], argv[1:]
    if op == "measure":
        as_json = "--json" in rest
        mute = "--quiet" in rest
        if "--" in rest:
            cmd = rest[rest.index("--") + 1:]
        else:
            cmd = [a for a in rest if a not in ("--json", "--quiet")]
        if not cmd:
            print("boxlib measure: no command", file=sys.stderr)
            return 2
        # the child's own output passes through unless --quiet, so a
        # caller can still redirect it; the cost line is ours and goes to
        # stdout last (an x265 progress line on a terminal ends in \r, so
        # mute it or the cost line lands on top of it)
        dn = subprocess.DEVNULL if mute else None
        r = measure(cmd, stdout=dn, stderr=dn)
        print(json.dumps(r) if as_json else fmt_line(r), flush=True)
        return r["rc"]
    if op == "quiet":
        def opt(name, d):
            return float(rest[rest.index(name) + 1]) if name in rest else d
        ok, s = quiet(int(opt("--samples", 3)), opt("--secs", 10),
                      opt("--max", QUIET_MAX))
        print(fmt_quiet(ok, s, opt("--max", QUIET_MAX)))
        return 0 if ok else 1
    if op == "lock" and rest:
        sub = rest[0]

        def opt(name, d=None):
            return rest[rest.index(name) + 1] if name in rest else d
        pid = int(opt("--pid", 0)) or os.getppid()
        if sub == "take":
            if take_lock(opt("--label", "timed leg"), pid):
                print(f"timing lock taken: {LOCK} (pid {pid})")
                return 0
            print(f"timing lock HELD by {describe(lock_holder() or {})}")
            return 1
        if sub == "release":
            if release_lock(pid, "--force" in rest):
                print("timing lock released")
                return 0
            print(f"timing lock is not ours: "
                  f"{describe(lock_holder() or {})} (use --force)")
            return 1
        if sub == "check":
            rec = lock_holder()
            print("timing lock: free" if rec is None
                  else f"timing lock: HELD by {describe(rec)}")
            return 0 if rec is None else 1
        if sub == "wait":
            wait_untimed(opt("--what", "untimed harness"))
            return 0
    print(f"boxlib: unknown command {' '.join(argv)!r}; see --help",
          file=sys.stderr)
    return 2


if __name__ == "__main__":
    sys.exit(_cli(sys.argv[1:]))
