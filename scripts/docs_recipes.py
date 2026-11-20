#!/usr/bin/env python3
# Copyright (c) 2026, the yah265 authors
# SPDX-License-Identifier: GPL-2.0-or-later
"""Runs every shell block in the docs on a tiny clip and fails if one breaks.

A recipe is a fenced block marked `sh` in README.md or a site page. Each
runs under `bash -euo pipefail` in its own empty directory that holds a
tiny clip under every input name the docs use. `y265`, `ffmpeg` and
`ffprobe` resolve to the binaries named on the command line, both as bare
names and at the paths the docs give them (build/y265, build-rel/y265,
build-ffmpeg/ffmpeg/ffmpeg). The directory also links the repo's scripts/,
docs/ and site/, so a recipe that runs a script finds what it reads.

  scripts/docs_recipes.py --list                 name every recipe and what
                                                 would happen to it. Runs nothing
  scripts/docs_recipes.py --y265 build-rel/y265 --ffmpeg-dir build-ffmpeg/ffmpeg
  scripts/docs_recipes.py ... --only guide       one page's recipes

`make docs-recipes` runs it on build/y265 and the ffmpeg that
scripts/build-ffmpeg.sh leaves in build-ffmpeg/ffmpeg.

How a block is treated:
  run     the default
  skip    the line above the fence is <!-- recipe: skip REASON -->, or the
          block builds, installs, publishes or fetches (SLOW below). Those
          are run by the gates that own them
  needs   it names a tool that wasn't given or found, or libx265 when the
          ffmpeg given wasn't built --with-x265; that fails the run
          unless --allow-missing
A block marked <!-- recipe: after PAGE:LINE --> runs in the directory the
named recipe left, for a second step that reads the first one's output.
A mark may sit above a generated span's opening comment.
"""
import argparse
import glob
import os
import re
import shutil
import struct
import subprocess
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
PAGES = ["README.md"] + sorted(
    os.path.relpath(p, ROOT) for p in glob.glob(os.path.join(ROOT, "site/*.md"))
    if os.path.basename(p) not in ("README.md", "shell-test.md"))
FENCE = re.compile(r"(?m)^```(sh|bash)\n(.*?)^```", re.S)
MARK = re.compile(r"<!--\s*recipe:\s*(skip|after)\s*(.*?)\s*-->\s*$")
GEN_OPEN = re.compile(r"^<!--gen:[\w-]+-->\s*$")
REPO_LINKS = ("scripts", "docs", "site")
# builds, installs, publishing and downloads: owned by other gates
SLOW = re.compile(r"\b(make\s+\w|meson\b|ninja\b|git\s+(worktree|push|clone|commit|add)\b|"
                  r"scripts/(site|build-ffmpeg|conformance|fetch_corpus)\S*|brew\b|pip\b|curl\b|"
                  r"wget\b|cp -R site/_build)")
TOOLS = ("y265", "ffmpeg", "ffprobe", "ffplay", "x265", "vmaf", "dec265", "MP4Box", "mkvmerge")
# every input name a page uses, and the tiny clip that stands in for it
Y4M_NAMES = ("in.y4m", "input.y4m", "clip.y4m", "tears_1080p.y4m", "source.y4m")
RAW_NAMES = ("in.yuv",)
MUXED_NAMES = ("in.mp4", "in.mov", "in.mkv", "master.mov", "tearsofsteel_4k.mov")
# CIF, because the v1 scorer in v1_ladder.py refuses a much smaller picture
CLIP_W, CLIP_H, CLIP_N, CLIP_FPS = 352, 288, 12, 24
TIMEOUT_S = 120


def recipes(only=None):
    out = []
    for rel in PAGES:
        if only and only not in os.path.basename(rel):
            continue
        text = open(os.path.join(ROOT, rel)).read()
        for m in FENCE.finditer(text):
            line = text.count("\n", 0, m.start()) + 1
            above = text[:m.start()].rstrip("\n").split("\n")
            while len(above) > 1 and GEN_OPEN.match(above[-1]):
                above.pop()
            mark = MARK.search(above[-1])
            body = "\n".join(re.sub(r"^\$ ", "", ln) for ln in m[2].splitlines())
            out.append({"id": f"{os.path.basename(rel)[:-3]}:{line}", "page": rel, "line": line,
                        "body": body, "mark": (mark[1], mark[2]) if mark else None})
    return out


def plan(r, have):
    """(action, reason) for one recipe."""
    if r["mark"] and r["mark"][0] == "skip":
        return "skip", r["mark"][1] or "marked"
    slow = SLOW.search(r["body"])
    if slow:
        return "skip", f"builds or publishes ({slow[0].strip()})"
    used = [t for t in TOOLS if re.search(rf"(?<![\w./-]){re.escape(t)}(?![\w-])", r["body"])]
    if "libx265" in r["body"] and "ffmpeg" in used:
        used.append("libx265")
    missing = [t for t in used if t not in have]
    if missing:
        return "needs", ", ".join(missing)
    return "run", "after " + r["mark"][1] if r["mark"] else ""


def write_y4m(path):
    """A moving gradient, so the encoder has motion to code. No encode made it."""
    with open(path, "wb") as f:
        f.write(f"YUV4MPEG2 W{CLIP_W} H{CLIP_H} F{CLIP_FPS}:1 Ip A1:1 C420jpeg\n".encode())
        for n in range(CLIP_N):
            f.write(b"FRAME\n")
            f.write(bytes((x * 3 + y * 2 + n * 5) & 255 for y in range(CLIP_H) for x in range(CLIP_W)))
            f.write(struct.pack("B", 128) * (CLIP_W * CLIP_H // 2))


def stage(d, tools):
    for sub in REPO_LINKS:
        os.symlink(os.path.join(ROOT, sub), os.path.join(d, sub))
    places = {"y265": ("build/y265", "build-rel/y265"),
              "ffmpeg": ("build-ffmpeg/ffmpeg/ffmpeg",),
              "ffprobe": ("build-ffmpeg/ffmpeg/ffprobe",)}
    for name, rels in places.items():
        for rel in rels:
            if name in tools:
                os.makedirs(os.path.join(d, os.path.dirname(rel)), exist_ok=True)
                os.symlink(os.path.abspath(tools[name]), os.path.join(d, rel))
    src = os.path.join(d, Y4M_NAMES[0])
    write_y4m(src)
    for n in Y4M_NAMES[1:]:
        shutil.copy(src, os.path.join(d, n))
    with open(src, "rb") as f, open(os.path.join(d, RAW_NAMES[0]), "wb") as raw:
        f.readline()
        for _ in range(CLIP_N):
            f.readline()
            raw.write(f.read(CLIP_W * CLIP_H * 3 // 2))
    if "ffmpeg" in tools:
        # lossless, so making the stand-in isn't a recipe under test
        for n in MUXED_NAMES:
            subprocess.run([tools["ffmpeg"], "-v", "error", "-y", "-i", src, "-c:v", "ffv1",
                            os.path.join(d, n)], check=False)


def run(todo, tools, keep):
    bindir = tempfile.mkdtemp(prefix="recipes-bin-")
    for name, path in tools.items():
        os.symlink(os.path.abspath(path), os.path.join(bindir, name))
    env = dict(os.environ, PATH=bindir + os.pathsep + os.environ["PATH"])
    dirs, failed = {}, []
    for r in todo:
        if r["mark"] and r["mark"][0] == "after":
            d = dirs.get(r["mark"][1])
            if not d:
                failed.append((r["id"], f"its first step {r['mark'][1]} didn't run"))
                continue
        else:
            d = tempfile.mkdtemp(prefix="recipe-")
            stage(d, tools)
        dirs[r["id"]] = d
        try:
            p = subprocess.run(["bash", "-euo", "pipefail", "-c", r["body"]], cwd=d, env=env,
                               capture_output=True, text=True, timeout=TIMEOUT_S)
            ok, tail = p.returncode == 0, (p.stderr or p.stdout)[-600:]
        except subprocess.TimeoutExpired:
            ok, tail = False, f"no exit after {TIMEOUT_S} s"
        print(f"{'ok  ' if ok else 'FAIL'} {r['id']}")
        if not ok:
            failed.append((r["id"], tail))
    if not keep:
        for d in set(dirs.values()) | {bindir}:
            shutil.rmtree(d, ignore_errors=True)
    return failed


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--list", action="store_true", help="print the plan and run nothing")
    ap.add_argument("--only", metavar="PAGE", help="recipes of pages whose name contains this")
    ap.add_argument("--y265", help="the y265 binary")
    ap.add_argument("--ffmpeg-dir", help="a directory holding ffmpeg and ffprobe built with libyah265")
    ap.add_argument("--allow-missing", action="store_true",
                    help="a recipe whose tool is missing is skipped, not failed")
    ap.add_argument("--keep", action="store_true", help="leave the scratch directories")
    a = ap.parse_args()
    tools = {}
    if a.y265:
        tools["y265"] = a.y265
    for t in ("ffmpeg", "ffprobe"):
        p = a.ffmpeg_dir and os.path.join(a.ffmpeg_dir, t)
        if p and os.path.exists(p):
            tools[t] = p
    for t in TOOLS:
        if t not in tools and t not in ("y265", "ffmpeg", "ffprobe") and shutil.which(t):
            tools[t] = shutil.which(t)
    have = set(tools)
    if "ffmpeg" in tools:
        enc = subprocess.run([tools["ffmpeg"], "-hide_banner", "-encoders"],
                             capture_output=True, text=True).stdout
        if re.search(r"\blibx265\b", enc):
            have.add("libx265")
    rs = recipes(a.only)
    plans = [(r, *plan(r, have)) for r in rs]
    if a.list:
        for r, act, why in plans:
            first = next((ln for ln in r["body"].splitlines() if ln.strip()), "")
            print(f"{act:5} {r['id']:26} {first[:70]}" + (f"   [{why}]" if why else ""))
        n = {k: sum(1 for _, act, _ in plans if act == k) for k in ("run", "skip", "needs")}
        print(f"{len(plans)} recipes on {len({r['page'] for r in rs})} pages: {n['run']} would run, "
              f"{n['skip']} skipped, {n['needs']} need a tool that wasn't given")
        return 0
    for t in tools.values():
        if not os.path.exists(t):
            sys.exit(f"docs_recipes: no such binary {t}")
    needs = [(r["id"], why) for r, act, why in plans if act == "needs"]
    failed = run([r for r, act, _ in plans if act == "run"], tools, a.keep)
    if needs and not a.allow_missing:
        failed += [(i, f"needs {why}") for i, why in needs]
    for i, tail in failed:
        print(f"\n{i}:\n{tail}", file=sys.stderr)
    print(f"{len(plans)} recipes, {len(failed)} failed")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
