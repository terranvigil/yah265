#!/usr/bin/env python3
# Copyright (c) 2026, the yah265 authors
# SPDX-License-Identifier: GPL-2.0-or-later
"""Draws the share images into site/assets/share/.

  <page>.png          one 1200x627 link card per site page: its title and
                      its description. The home, results and blog cards
                      also carry the headline.
  launch-*.png        four images for the launch posts

Every figure on a card comes from blog_charts.headline_cells(), and a card
that states one says "on the Mac" and "VMAF v1" in its main line. Each PNG
is stamped with a key of what it shows, which is how --check knows a stale
one without drawing it. Drawing needs Pillow.

  scripts/share_cards.py           draw what is out of date
  scripts/share_cards.py --all     draw every image again
  scripts/share_cards.py --check   change nothing; exit 1 if one is stale
"""
import argparse
import glob
import hashlib
import os
import re
import statistics as st
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import blog_charts as bc  # noqa: E402

ROOT = bc.ROOT
SITE = os.path.join(ROOT, "site")
OUT = os.path.join(SITE, "assets/share")
LAYOUT = 3  # bump when a card's wording or layout changes
SKIP = {"README.md"}
HEADLINE_PAGES = {"index", "results", "blog"}
W, H, M = 1200, 627, 72
INK, INK2, BG, RULE = "#0b0b0b", "#52514e", "#fcfcfb", "#e3e2de"


def front_matter(path):
    text = open(path).read()
    m = re.match(r"---\n(.*?)\n---\n", text, re.S)
    meta = dict(re.findall(r"(?m)^(\w+):\s*(.*)$", m[1])) if m else {}
    return meta.get("title", "yah265").split(" - ")[0], meta.get("description", "")


def main_line(pl):
    """The one sentence a card with a number carries."""
    return [f"{bc.headline_times(pl)} faster than x265 on the Mac,",
            f"{pl['dv']:+.2f} VMAF v1 at the same file size"]


def foot(pl):
    return (f"Medians over {pl['n']} size-matched CRF encodes, CIF to 4K, both at preset "
            f"medium. Apple M5 Max, all cores.")


def class_medians(pl):
    return [(bc.RES[c], st.median(r for r, _ in pl["pools"][c]), len(pl["pools"][c]))
            for c in bc.CLASSES if pl["pools"][c]]


class Pen:
    def __init__(self, w, h):
        from PIL import Image, ImageDraw
        self.w, self.h = w, h
        self.im = Image.new("RGB", (w, h), BG)
        self.d = ImageDraw.Draw(self.im)

    def font(self, size, bold=False):
        from PIL import ImageFont
        for name, idx in (("/System/Library/Fonts/HelveticaNeue.ttc", 1 if bold else 0),
                          ("/System/Library/Fonts/Helvetica.ttc", 1 if bold else 0),
                          ("DejaVuSans-Bold.ttf" if bold else "DejaVuSans.ttf", 0)):
            try:
                return ImageFont.truetype(name, size, index=idx)
            except OSError:
                continue
        return ImageFont.load_default()

    def logo(self, x, y, height=64):
        from PIL import Image
        if os.path.exists(bc.LOGO):
            logo = Image.open(bc.LOGO).convert("RGBA")
            logo = logo.resize((round(logo.width * height / logo.height), height), Image.LANCZOS)
            self.im.paste(logo, (x, y), logo)
        else:
            self.d.text((x, y), "yah265", font=self.font(52, True), fill=INK)

    def fit(self, lines, size, maxw, bold=True, floor=30):
        while size > floor and max(self.d.textlength(t, font=self.font(size, bold))
                                   for t in lines) > maxw:
            size -= 2
        return size

    def wrap(self, text, size, maxw, bold=False):
        f, lines, cur = self.font(size, bold), [], ""
        for word in text.split():
            trial = f"{cur} {word}".strip()
            if cur and self.d.textlength(trial, font=f) > maxw:
                lines.append(cur)
                cur = word
            else:
                cur = trial
        return lines + [cur] if cur else lines

    def text(self, x, y, s, size, bold=False, fill=INK):
        self.d.text((x, y), s, font=self.font(size, bold), fill=fill)

    def lines(self, x, y, lines, size, bold=False, fill=INK, lead=1.2):
        for i, t in enumerate(lines):
            self.text(x, y + i * round(size * lead), t, size, bold, fill)
        return y + len(lines) * round(size * lead)

    def bar(self, x0, x1, y, h, color):
        self.d.rounded_rectangle((x0, y, x1, y + h), radius=8, fill=color)
        self.d.rectangle((x0, y, x0 + 8, y + h), fill=color)

    def save(self, path, key):
        from PIL import PngImagePlugin
        meta = PngImagePlugin.PngInfo()
        meta.add_text("yah265-card", key)
        self.im.save(path, pnginfo=meta, optimize=True)


def page_card(path, key, title, desc, pl):
    p = Pen(W, H)
    p.logo(M, 56)
    y = 168
    size = p.fit([title], 84, W - 2 * M, floor=48)
    y = p.lines(M, y, p.wrap(title, size, W - 2 * M, True), size, True) + 18
    if pl:
        size = p.fit(main_line(pl), 52, W - 2 * M)
        y = p.lines(M, y, main_line(pl), size, True) + 20
        p.lines(M, H - 96, p.wrap(foot(pl), 22, W - 2 * M), 22, fill=INK2)
    else:
        p.lines(M, y, p.wrap(desc, 34, W - 2 * M)[:5], 34, fill=INK2, lead=1.3)
        p.d.line((M, H - 92, W - M, H - 92), fill=RULE, width=2)
        p.text(M, H - 72, "yah265, a new HEVC encoder, written in C", 24, fill=INK2)
    p.save(path, key)


def launch_speed(path, key, pl, fl):
    """The headline over the 4K films' total encode time as two bars."""
    p = Pen(W, H)
    p.logo(M, 56)
    size = p.fit(main_line(pl), 92, W - 2 * M)
    p.lines(M, 150, main_line(pl), size, True, lead=1.18)
    ok = [r for r in fl["2160p"] if r[1]]
    ours, ref = sum(r[1] for r in ok), sum(r[2] for r in ok)
    p.text(M, 368, f"{len(ok)} ten-minute films at 4K, preset medium, same file sizes", 26, fill=INK2)
    x0, full = M + 122, W - M - 130 - 200
    for y, name, v, color in ((424, "x265", ref, bc.NEUTRAL), (484, "yah265", ours, bc.BRAND)):
        x1 = x0 + full * v / ref
        p.text(M, y + 4, name, 28, name == "yah265")
        p.bar(x0, x1, y, 40, color)
        p.text(x1 + 16, y + 4, f"{v / 60:.0f} min", 28, True)
    p.text(M, 562, foot(pl), 22, fill=INK2)
    p.save(path, key)


def launch_quality(path, key, pl):
    p = Pen(W, H)
    p.logo(M, 56)
    main = [f"{pl['dv']:+.2f} VMAF v1 on the Mac", "at x265's file size, in less time"]
    size = p.fit(main, 96, W - 2 * M)
    y = p.lines(M, 160, main, size, True, lead=1.18) + 30
    p.lines(M, y, [f"yah265 scores higher in {pl['dv_ahead']} of {pl['dv_n']} size-matched encodes,",
                   f"and finishes a median {bc.headline_times(pl)} faster."], 36, fill=INK2, lead=1.35)
    p.text(M, 562, foot(pl), 22, fill=INK2)
    p.save(path, key)


def launch_sizes(path, key, pl):
    """Median speed per picture size as bars past the x265 line."""
    p = Pen(W, H)
    p.logo(M, 56)
    cm = class_medians(pl)
    fast = [1 / r for _, r, _ in cm]
    main = [f"{min(fast):.2f}x to {max(fast):.2f}x faster than x265 on the Mac,",
            "ahead on VMAF v1 at every picture size"]
    size = p.fit(main, 64, W - 2 * M)
    y = p.lines(M, 150, main, size, True) + 34
    x0, full = M + 130, W - 2 * M - 130 - 190
    top = max(fast)
    even = x0 + full / top
    for name, r, n in cm:
        p.text(M, y + 6, name, 30, True)
        p.bar(x0, x0 + full / r / top, y, 44, bc.BRAND)
        p.text(x0 + full / r / top + 16, y + 6, f"{1 / r:.2f}x", 30, True)
        y += 62
    p.d.line((even, y - 62 * len(cm) - 8, even, y - 10), fill=INK, width=3)
    p.text(even + 10, y - 12, "x265", 22, fill=INK2)
    p.text(M, 582, foot(pl), 22, fill=INK2)
    p.save(path, key)


def launch_square(path, key, pl):
    S = 1080
    p = Pen(S, S)
    p.logo(M, 80, 80)
    main = [bc.headline_times(pl) + " faster", "than x265 on the Mac,", f"{pl['dv']:+.2f} VMAF v1",
            "at the same file size"]
    size = p.fit(main, 112, S - 2 * M)
    y = p.lines(M, 250, main, size, True, lead=1.2) + 50
    p.lines(M, y, p.wrap("A new HEVC encoder, written in C. It runs inside ffmpeg.",
                         40, S - 2 * M), 40, fill=INK2, lead=1.3)
    p.lines(M, S - 150, p.wrap(foot(pl), 26, S - 2 * M), 26, fill=INK2, lead=1.3)
    p.save(path, key)


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--check", action="store_true",
                    help="change nothing; exit 1 if an image is out of date")
    ap.add_argument("--all", action="store_true", help="draw every image again")
    a = ap.parse_args()
    mac = bc.newest(r"PARITY-REPORT-yah265-\d{4}-\d\d-\d\d\.csv")
    rows = bc.report_rows(mac)
    pl, fl = bc.headline_cells(rows), bc.films(rows)
    ok = [r for r in fl["2160p"] if r[1]]
    cm = class_medians(pl)
    figures = (f"{bc.headline_times(pl)}|{pl['dv']:+.2f}|{pl['n']}|{pl['dv_ahead']}|"
               + "|".join(f"{r:.4f}" for _, r, _ in cm)
               + f"|{sum(r[1] for r in ok):.0f}|{sum(r[2] for r in ok):.0f}")
    jobs = {}
    for path in sorted(glob.glob(os.path.join(SITE, "*.md"))):
        name = os.path.basename(path)
        if name in SKIP or name.startswith("_"):
            continue
        stem = name[:-3]
        title, desc = front_matter(path)
        num = stem in HEADLINE_PAGES
        said = figures if num else hashlib.sha1(desc.encode()).hexdigest()[:12]
        jobs[stem + ".png"] = (f"v{LAYOUT}|{title}|{said}",
                               lambda p, k, t=title, d=desc, n=num: page_card(p, k, t, d, pl if n else None),
                               (W, H))
    jobs["launch-speed.png"] = (f"v{LAYOUT}|speed|{figures}", lambda p, k: launch_speed(p, k, pl, fl), (W, H))
    jobs["launch-quality.png"] = (f"v{LAYOUT}|quality|{figures}", lambda p, k: launch_quality(p, k, pl), (W, H))
    jobs["launch-sizes.png"] = (f"v{LAYOUT}|sizes|{figures}", lambda p, k: launch_sizes(p, k, pl), (W, H))
    jobs["launch-square.png"] = (f"v{LAYOUT}|square|{figures}", lambda p, k: launch_square(p, k, pl),
                                 (1080, 1080))
    stale = []
    for name, (key, draw, dims) in jobs.items():
        key = key.encode("latin-1", "replace").decode("latin-1")
        p = os.path.join(OUT, name)
        if a.all or bc.card_stamp(p) != (*dims, key):
            stale.append(name)
            if not a.check:
                os.makedirs(OUT, exist_ok=True)
                try:
                    draw(p, key)
                except ImportError:
                    sys.exit(f"{name} is out of date and drawing it needs Pillow")
    extra = sorted(set(os.path.basename(p) for p in glob.glob(os.path.join(OUT, "*.png"))) - set(jobs))
    for name in extra:
        stale.append(name + " (no page)")
        if not a.check:
            os.remove(os.path.join(OUT, name))
    print(f"{len(jobs)} share images; headline {' '.join(main_line(pl))}")
    if a.check:
        if stale:
            print("stale: " + ", ".join(stale))
            sys.exit(1)
        print("up to date")
    else:
        print("updated: " + (", ".join(stale) or "nothing changed"))


if __name__ == "__main__":
    main()
