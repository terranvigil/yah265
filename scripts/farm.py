#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Farm mode: one y265 encode split over several machines.

The coordinator asks a worker for the segment table --gop-parallel would
build (y265 --farm-plan), has workers code one segment each
(y265 --farm-seg), and concatenates the segments in order. The output is
the --gop-parallel stream at the same threads per instance:

    y265 IN -o ref.hevc OPTS --gop-parallel W --threads W*K
    scripts/farm.py -o farm.hevc --seg-threads K --worker ... -- IN OPTS

Workers:
    local[:SLOTS]         y265 on this machine (--y265)
    ssh:HOST[:SLOTS]      y265 on HOST over ssh (--remote-y265); the input
                          path must name the same file there
    cmd:PATH[:SLOTS]      PATH run with the y265 command line as its
                          arguments (a wrapper; the tests use it)

SLOTS is how many segments a worker codes at once (default 1). Every
N265_* variable in this environment goes to every worker, since the
knobs shape the stream. Each worker must report the same build
(--farm-hello), or the run is refused.

The state directory (default OUT.farm) holds the plan, segment 0's CRF
level and each finished segment with its hash. Run the same command
again to resume after a failure; --fresh starts over. A failed or
timed-out segment is retried, on another worker when there is one.

Exit codes: 0 done, 1 failed, 2 refused or bad usage.
"""

import argparse
import hashlib
import json
import os
import queue
import shlex
import signal
import subprocess
import sys
import threading
import time

PROTO = 1
REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def log(msg):
    print(f"farm: {msg}", file=sys.stderr, flush=True)


class Worker:
    def __init__(self, spec, y265, remote_y265, env):
        kind, _, rest = spec.partition(":")
        if kind == "local":
            target, slots = "", rest
        elif kind in ("ssh", "cmd"):
            # the last field is SLOTS only when it is a number
            target, _, slots = rest.rpartition(":")
            if not slots.isdigit():
                target, slots = rest, ""
        else:
            raise ValueError(f"unknown worker {spec!r}")
        if kind != "local" and not target:
            raise ValueError(f"worker {spec!r} names no host or command")
        self.kind, self.target = kind, target
        self.slots = int(slots) if slots else 1
        if self.slots < 1:
            raise ValueError(f"worker {spec!r}: SLOTS is at least 1")
        self.y265 = remote_y265 if kind == "ssh" else y265
        self.env = env
        self.busy = 0
        self.name = spec

    def argv(self, args):
        if self.kind == "local":
            return [self.y265] + args
        if self.kind == "cmd":
            return [self.target, self.y265] + args
        envs = [shlex.quote(f"{k}={v}") for k, v in sorted(self.env.items())]
        remote = " ".join((["env"] + envs if envs else []) +
                          [shlex.quote(a) for a in [self.y265] + args])
        return ["ssh", "-o", "BatchMode=yes", "-o", "ServerAliveInterval=30",
                self.target, remote]

    def run(self, args, timeout):
        return subprocess.run(self.argv(args), capture_output=True,
                              text=True, timeout=timeout)


def n265_env():
    return {k: v for k, v in os.environ.items() if k.startswith("N265_")}


def parse_plan(text):
    lines = [ln.split() for ln in text.splitlines() if ln.strip()]
    if not lines or lines[0] != ["y265-farm-plan", str(PROTO)]:
        raise ValueError("not a farm plan (protocol %d)" % PROTO)
    frames, segs = None, []
    for ln in lines[1:]:
        if ln[0] == "frames":
            frames = int(ln[1])
        elif ln[0] == "seg":
            i, s, e = map(int, ln[1:4])
            if i != len(segs) or s != (segs[-1][1] if segs else 0) or e <= s:
                raise ValueError(f"plan: segment line {ln} is out of order")
            segs.append((s, e))
    if frames is None or (segs and segs[-1][1] != frames):
        raise ValueError("plan: the segments don't cover the frames")
    return frames, segs


def sha256(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for b in iter(lambda: f.read(1 << 20), b""):
            h.update(b)
    return h.hexdigest()


class State:
    def __init__(self, path):
        self.path = path
        self.d = {}

    def load(self):
        try:
            with open(self.path) as f:
                self.d = json.load(f)
        except FileNotFoundError:
            self.d = {}

    def save(self):
        tmp = self.path + ".tmp"
        with open(tmp, "w") as f:
            json.dump(self.d, f, indent=1, sort_keys=True)
        os.replace(tmp, self.path)


class Job:
    def __init__(self, idx, worker, proc, part, logf, anchor):
        self.idx, self.worker, self.proc = idx, worker, proc
        self.part, self.logf, self.anchor = part, logf, anchor
        self.t0 = time.monotonic()
        self.done_line = None
        self.killed = None


REFUSED = ("-o", "--gop-parallel", "--threads", "--farm-seg", "--farm-plan",
           "--farm-anchor", "--farm-stdin-watch", "--farm-hello")


class Farm:
    def __init__(self, a, args, workers):
        self.a, self.args, self.workers = a, args, workers
        self.env = n265_env()
        self.events = queue.Queue()
        self.jobs = {}
        self.stop = False

    # ---- set-up ----

    def hello(self):
        ref = None
        for w in self.workers:
            try:
                r = w.run(["--farm-hello"], 60)
            except (OSError, subprocess.TimeoutExpired) as e:
                log(f"{w.name}: no answer to --farm-hello ({e})")
                return None
            f = dict(kv.split("=", 1) for kv in r.stdout.split()[1:]
                     if "=" in kv)
            if r.returncode or not r.stdout.startswith("y265-farm "):
                log(f"{w.name}: --farm-hello failed: "
                    f"{(r.stderr or r.stdout).strip()}")
                return None
            if f.get("proto") != str(PROTO):
                log(f"{w.name}: farm protocol {f.get('proto')}, want {PROTO}")
                return None
            key = (f.get("version"), f.get("build"))
            if ref is None:
                ref = key
            elif key != ref:
                log(f"refused: {w.name} runs y265 {key[0]} build {key[1]}, "
                    f"and {self.workers[0].name} runs {ref[0]} build "
                    f"{ref[1]}. Workers need the same build to write one "
                    f"stream.")
                return None
        return {"version": ref[0], "build": ref[1]}

    def plan(self):
        if self.a.plan:
            with open(self.a.plan) as f:
                return parse_plan(f.read())
        w = self.workers[0]
        r = w.run(self.args + ["--farm-plan", "--threads", "1"],
                  self.a.timeout or None)
        if r.returncode:
            raise RuntimeError(f"--farm-plan on {w.name}: {r.stderr.strip()}")
        return parse_plan(r.stdout)

    def key(self, build):
        # every argument that is a file here: the input and any side file
        files = {x: [os.stat(x).st_size, os.stat(x).st_mtime_ns]
                 for x in self.args if os.path.isfile(x)}
        k = {"proto": PROTO, "build": build, "args": self.args,
             "env": self.env, "seg_threads": self.a.seg_threads,
             "plan_file": self.a.plan, "files": files}
        return hashlib.sha256(json.dumps(k, sort_keys=True).encode()
                              ).hexdigest()

    # ---- segments ----

    def seg_path(self, i):
        return os.path.join(self.a.state, f"seg{i:05d}.hevc")

    def seg_args(self, i, anchor):
        s, e = self.segs[i]
        a = self.args + ["--threads", str(self.a.seg_threads), "--farm-seg",
                         f"{i}:{s}:{e}:{len(self.segs)}:{self.frames}",
                         "-o", "-", "--farm-stdin-watch"]
        if i > 0:
            a += ["--farm-anchor", anchor]
        return a

    def launch(self, i, w):
        st = self.state.d["segs"][str(i)]
        st["attempts"] += 1
        anchor = self.state.d.get("anchor")
        part = self.seg_path(i) + ".part"
        logp = os.path.join(self.a.state,
                            f"seg{i:05d}.try{st['attempts']}.log")
        out = open(part, "wb")
        logf = open(logp, "w")
        env = dict(os.environ)
        try:
            proc = subprocess.Popen(w.argv(self.seg_args(i, anchor)),
                                    stdin=subprocess.PIPE, stdout=out,
                                    stderr=subprocess.PIPE, env=env,
                                    start_new_session=True)
        except OSError as e:
            out.close()
            logf.close()
            log(f"seg {i}: cannot start on {w.name}: {e}")
            self.events.put(("exit", i, None))
            st["last_worker"] = w.name
            return
        out.close()
        w.busy += 1
        job = Job(i, w, proc, part, logf, anchor)
        self.jobs[i] = job
        st["last_worker"] = w.name
        s, e = self.segs[i]
        log(f"seg {i} [{s}, {e}) on {w.name}, try {st['attempts']}")
        threading.Thread(target=self.reader, args=(job,), daemon=True).start()

    def reader(self, job):
        for line in iter(job.proc.stderr.readline, b""):
            t = line.decode(errors="replace")
            job.logf.write(t)
            if t.startswith("y265-farm: anchor "):
                self.events.put(("anchor", job.idx, t.split()[2], job))
            elif t.startswith("y265-farm: done "):
                job.done_line = t.split()[2:]
        job.proc.wait()
        job.logf.close()
        self.events.put(("exit", job.idx, job))

    def kill(self, job, why):
        if job.killed:
            return
        job.killed = why
        try:
            os.killpg(job.proc.pid, signal.SIGKILL)
        except OSError:
            pass
        try:
            job.proc.stdin.close()
        except OSError:
            pass

    def finished(self, job):
        i = job.idx
        st = self.state.d["segs"][str(i)]
        job.worker.busy -= 1
        del self.jobs[i]
        rc = job.proc.returncode
        size = os.path.getsize(job.part) if os.path.exists(job.part) else -1
        ok = (rc == 0 and not job.killed and job.done_line and
              int(job.done_line[0]) == i and int(job.done_line[2]) == size)
        dt = time.monotonic() - job.t0
        if ok and i > 0 and job.anchor != self.state.d.get("anchor"):
            ok = False
            job.killed = "segment 0's level changed under it"
        if ok:
            os.replace(job.part, self.seg_path(i))
            st.update(status="done", bytes=size, sha256=sha256(
                self.seg_path(i)), worker=job.worker.name,
                seconds=round(dt, 2))
            self.state.save()
            log(f"seg {i} done on {job.worker.name}: {size} bytes, "
                f"{dt:.1f} s")
            return True
        why = job.killed or f"exit {rc}"
        log(f"seg {i} failed on {job.worker.name} ({why}), "
            f"log {os.path.basename(job.logf.name)}")
        st["failures"] = st.get("failures", 0) + 1
        self.state.save()
        if job.killed and job.killed.startswith("segment 0's level"):
            st["attempts"] -= 1
            return True
        if st["attempts"] >= self.a.attempts:
            log(f"seg {i}: {st['attempts']} tries failed; stopping. Run the "
                f"same command again to resume.")
            return False
        return True

    def on_anchor(self, idx, value, job):
        if idx != 0:
            return
        old = self.state.d.get("anchor")
        if old is None:
            self.state.d["anchor"] = value
            self.state.save()
            log(f"segment 0's CRF level: {value}")
            return
        if old == value:
            return
        # a retry of segment 0 read a different level (possible above one
        # thread): every later segment is recoded on the new one
        log(f"segment 0 printed {value}, an earlier try {old}; recoding "
            f"the segments after it")
        self.state.d["anchor"] = value
        for k, st in self.state.d["segs"].items():
            if int(k) > 0 and st["status"] == "done":
                st["status"] = "todo"
        for j in list(self.jobs.values()):
            if j.idx > 0:
                self.kill(j, "segment 0's level changed under it")
        self.state.save()

    def pick(self, w, todo):
        anchor_known = self.state.d.get("anchor") is not None
        ready = [i for i in todo if i == 0 or anchor_known]
        if not ready:
            return None
        others_free = any(o is not w and o.busy < o.slots
                          for o in self.workers)
        for i in ready:
            st = self.state.d["segs"][str(i)]
            if st.get("last_worker") != w.name or st["attempts"] == 0 \
               or len(self.workers) == 1 or not others_free:
                return i
        return None

    def run_segments(self):
        segs = self.state.d["segs"]

        def todo():
            run = set(self.jobs)
            return [i for i in range(len(self.segs))
                    if segs[str(i)]["status"] != "done" and i not in run]
        while True:
            left = todo()
            if not left and not self.jobs:
                return True
            for w in self.workers:
                while w.busy < w.slots and not self.stop:
                    i = self.pick(w, todo())
                    if i is None:
                        break
                    self.launch(i, w)
            try:
                ev = self.events.get(timeout=0.5)
            except queue.Empty:
                ev = None
            if ev and ev[0] == "anchor":
                self.on_anchor(ev[1], ev[2], ev[3])
            elif ev and ev[0] == "exit":
                job = ev[2]
                if job is None:
                    st = segs[str(ev[1])]
                    if st["attempts"] >= self.a.attempts:
                        return False
                elif not self.finished(job):
                    for j in list(self.jobs.values()):
                        self.kill(j, "the run stopped")
                    self.stop = True
            if self.stop and not self.jobs:
                return False
            if self.a.timeout:
                now = time.monotonic()
                for j in list(self.jobs.values()):
                    if now - j.t0 > self.a.timeout:
                        self.kill(j, f"timeout after {self.a.timeout:g} s")

    def stitch(self):
        tmp = self.a.output + ".part"
        total = 0
        with open(tmp, "wb") as out:
            for i in range(len(self.segs)):
                p = self.seg_path(i)
                st = self.state.d["segs"][str(i)]
                if sha256(p) != st["sha256"]:
                    raise RuntimeError(f"{p} changed after it was coded")
                with open(p, "rb") as f:
                    for b in iter(lambda: f.read(1 << 20), b""):
                        out.write(b)
                        total += len(b)
        os.replace(tmp, self.a.output)
        return total

    # ---- the run ----

    def dry_run(self):
        print("# hello, on every worker")
        for w in self.workers:
            print(shlex.join(w.argv(["--farm-hello"])))
        if self.a.plan:
            with open(self.a.plan) as f:
                self.frames, self.segs = parse_plan(f.read())
        else:
            print("# plan, on the first worker")
            print(shlex.join(self.workers[0].argv(
                self.args + ["--farm-plan", "--threads", "1"])))
            self.frames, self.segs = 2, [(0, 1), (1, 2)]
            print("# segments, for a two-segment plan of two frames")
        for i in range(len(self.segs)):
            w = self.workers[i % len(self.workers)]
            print(shlex.join(w.argv(self.seg_args(i, "<from segment 0>"))))
        return 0

    def main(self):
        if self.a.dry_run:
            return self.dry_run()
        os.makedirs(self.a.state, exist_ok=True)
        info = self.hello()
        if info is None:
            return 2
        self.state = State(os.path.join(self.a.state, "state.json"))
        self.state.load()
        key = self.key(info["build"])
        d = self.state.d
        if d and (self.a.fresh or d.get("key") != key):
            if not self.a.fresh:
                log("the state directory is from a different run (options, "
                    "environment, build or input); starting over")
            d.clear()
        if not d:
            frames, segs = self.plan()
            d.update(key=key, version=info["version"], build=info["build"],
                     args=self.args, env=self.env, frames=frames,
                     plan=segs, anchor=None,
                     segs={str(i): {"status": "todo", "attempts": 0}
                           for i in range(len(segs))})
            self.state.save()
        else:
            for k, st in d["segs"].items():
                p = self.seg_path(int(k))
                if st["status"] == "done" and not (
                        os.path.exists(p) and sha256(p) == st["sha256"]):
                    st["status"] = "todo"
                st["attempts"] = 0
            ndone = sum(st["status"] == "done" for st in d["segs"].values())
            log(f"resuming: {ndone} of {len(d['segs'])} segments kept")
        self.frames, self.segs = d["frames"], [tuple(s) for s in d["plan"]]
        if not self.segs:
            log("the input has no frames")
            return 1
        log(f"{len(self.segs)} segments over {self.frames} frames, "
            f"{len(self.workers)} workers, {self.a.seg_threads} threads "
            f"per segment")
        t0 = time.monotonic()
        try:
            ok = self.run_segments()
        except KeyboardInterrupt:
            for j in list(self.jobs.values()):
                self.kill(j, "interrupted")
            raise
        if not ok:
            return 1
        total = self.stitch()
        retries = sum(st.get("failures", 0) for st in d["segs"].values())
        log(f"wrote {self.a.output}: {total} bytes, {self.frames} frames, "
            f"{len(self.segs)} segments, {retries} retries, "
            f"{time.monotonic() - t0:.1f} s")
        if not self.a.keep:
            for i in range(len(self.segs)):
                os.remove(self.seg_path(i))
            for f in os.listdir(self.a.state):
                if f.endswith(".log") or f.endswith(".part"):
                    os.remove(os.path.join(self.a.state, f))
            os.remove(self.state.path)
            try:
                os.rmdir(self.a.state)
            except OSError:
                pass
        return 0


def main(argv=None):
    ap = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawTextHelpFormatter,
        usage="%(prog)s -o OUT [options] -- INPUT [y265 options]")
    ap.add_argument("-o", "--output", required=True)
    ap.add_argument("--worker", action="append", default=[],
                    help="local[:SLOTS], ssh:HOST[:SLOTS] or cmd:PATH[:SLOTS]"
                         " (default local:2)")
    ap.add_argument("--y265", default=None,
                    help="y265 for local and cmd workers (default: this "
                         "tree's build/y265, else y265 on PATH)")
    ap.add_argument("--remote-y265", default="y265",
                    help="y265 on ssh workers (default y265)")
    ap.add_argument("--seg-threads", type=int, default=4,
                    help="threads per segment, K in --threads W*K "
                         "(default 4)")
    ap.add_argument("--state", help="state directory (default OUT.farm)")
    ap.add_argument("--timeout", type=float, default=0,
                    help="seconds a segment may take before it is retried "
                         "(default: no limit)")
    ap.add_argument("--attempts", type=int, default=3,
                    help="tries per segment (default 3)")
    ap.add_argument("--plan", help="a segment table in --farm-plan's format, "
                    "instead of asking a worker")
    ap.add_argument("--fresh", action="store_true",
                    help="ignore the state directory and start over")
    ap.add_argument("--keep", action="store_true",
                    help="keep the state directory after the stitch")
    ap.add_argument("--dry-run", action="store_true",
                    help="print the worker commands and run nothing")
    ap.add_argument("args", nargs=argparse.REMAINDER)
    a = ap.parse_args(argv)
    args = a.args[1:] if a.args[:1] == ["--"] else a.args
    if not args:
        ap.error("give the input and the y265 options after --")
    bad = [x for x in args if x in REFUSED]
    if bad:
        ap.error(f"{bad[0]} is the coordinator's to set (-o, "
                 f"--seg-threads), not an option to pass through")
    if any(x.startswith("--target-vmaf") or x == "--no-target-vmaf-verify"
           for x in args):
        ap.error("--target-vmaf searches the whole input on one machine; "
                 "run it once with y265 and farm the --crf it picks")
    if a.seg_threads < 1:
        ap.error("--seg-threads is at least 1")
    a.state = a.state or a.output + ".farm"
    if a.y265 is None:
        cand = os.path.join(REPO, "build", "y265")
        a.y265 = cand if os.access(cand, os.X_OK) else "y265"
    try:
        workers = [Worker(s, a.y265, a.remote_y265, n265_env())
                   for s in (a.worker or ["local:2"])]
    except ValueError as e:
        ap.error(str(e))
    try:
        return Farm(a, args, workers).main()
    except (RuntimeError, ValueError, OSError,
            subprocess.TimeoutExpired) as e:
        log(f"error: {e}")
        return 1


if __name__ == "__main__":
    sys.exit(main())
