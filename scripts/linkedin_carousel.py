#!/usr/bin/env python3
# Copyright (c) 2026, the yah265 authors
# SPDX-License-Identifier: GPL-2.0-or-later
"""Draws the LinkedIn launch carousel: a PDF with one 1080x1350 slide per
page, and a PNG of each slide.

Every figure on a slide comes from blog_charts.headline_cells() and
blog_charts.films(), and a slide that states one says "on the Mac" and, for
a quality figure, "VMAF v1". Drawing needs Pillow and macOS's system fonts.

  scripts/linkedin_carousel.py            write local/launch/yah265-carousel.pdf
                                          and local/launch/slides/NN-name.png
  scripts/linkedin_carousel.py --out DIR  write them under DIR
  scripts/linkedin_carousel.py --text     print the slides' words and draw nothing
"""
import argparse
import os
import subprocess
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import blog_charts as bc  # noqa: E402

W, H, M = 1080, 1350, 84
# --wide: 16:9 pages, which fill the width of LinkedIn's desktop document frame
WIDE = "--wide" in sys.argv
if WIDE:
    W, H, M = 1920, 1080, 110
LW, RX = 790, 1010  # a wide slide's left column width and right column start
S = 2  # drawn at twice the size, so the PDF stays sharp when zoomed
BG, SURFACE, SUNK, HAIR = "#131517", "#1b1c1e", "#23262a", "#2e3135"
INK, INK2, INK3 = "#e9ecee", "#b6bcc1", "#8f959a"
NEON, ACCENT = bc.BRAND, "#ff8a5c"
REPO = "github.com/terranvigil/yah265"
COUNT = {2: "Two", 3: "Three", 4: "Four", 5: "Five", 6: "Six", 7: "Seven", 8: "Eight"}
SANS = "/System/Library/Fonts/SFNS.ttf"
MONO = "/System/Library/Fonts/SFNSMono.ttf"
WEIGHT = {"regular": 400, "medium": 510, "semibold": 590, "bold": 700, "heavy": 860}


def default_out():
    """local/launch of the main checkout, also when run from a worktree."""
    try:
        common = subprocess.run(["git", "-C", bc.ROOT, "rev-parse", "--git-common-dir"],
                                capture_output=True, text=True, check=True).stdout.strip()
        root = os.path.dirname(os.path.abspath(os.path.join(bc.ROOT, common)))
    except (OSError, subprocess.CalledProcessError):
        root = bc.ROOT
    return os.path.join(root, "local/launch")


class Slide:
    fonts = {}

    def __init__(self, n, total, glow=0.20):
        from PIL import Image, ImageDraw, ImageFilter
        self.said = []
        self.im = Image.new("RGB", (W * S, H * S), BG)
        halo = Image.new("L", (W * S, H * S), 0)
        ImageDraw.Draw(halo).ellipse(((W - 460) * S, -420 * S, (W + 420) * S, 380 * S),
                                     fill=round(255 * glow))
        halo = halo.filter(ImageFilter.GaussianBlur(170 * S))
        self.im.paste(Image.new("RGB", self.im.size, NEON), (0, 0), halo)
        self.d = ImageDraw.Draw(self.im)
        logo = Image.open(os.path.join(bc.ROOT, "assets/yah265-logo-dark.png")).convert("RGBA")
        lh = 60 * S
        logo = logo.resize((round(logo.width * lh / logo.height), lh), Image.LANCZOS)
        self.im.paste(logo, ((M - 6) * S, 66 * S), logo)
        self.text(W - M, 80, f"{n} / {total}", 28, "medium", INK3, anchor="ra", say=False)
        seg = (W - 2 * M - 10 * (total - 1)) / total
        for i in range(total):
            x = M + i * (seg + 10)
            self.d.rounded_rectangle((x * S, (H - 60) * S, (x + seg) * S, (H - 54) * S),
                                     radius=3 * S, fill=NEON if i < n else HAIR)

    def font(self, size, weight="regular", mono=False):
        from PIL import ImageFont
        key = (size, weight, mono)
        if key not in self.fonts:
            try:
                f = ImageFont.truetype(MONO if mono else SANS, round(size * S))
                if mono:
                    f.set_variation_by_name(weight.capitalize())
                else:
                    f.set_variation_by_axes([100, max(17, min(96, size)), 400, WEIGHT[weight]])
            except OSError:
                sys.exit("drawing the carousel needs macOS's system fonts (SFNS.ttf)")
            self.fonts[key] = f
        return self.fonts[key]

    def width(self, s, size, weight="regular", mono=False, track=0):
        return self.d.textlength(s, font=self.font(size, weight, mono)) / S + track * len(s)

    def text(self, x, y, s, size, weight="regular", fill=INK, mono=False, anchor="la", track=0,
             say=True):
        if say:
            self.said.append(s)
        f = self.font(size, weight, mono)
        if not track:
            self.d.text((x * S, y * S), s, font=f, fill=fill, anchor=anchor)
            return
        for ch in s:
            self.d.text((x * S, y * S), ch, font=f, fill=fill, anchor=anchor)
            x += self.d.textlength(ch, font=f) / S + track

    def wrap(self, s, size, weight, maxw):
        lines, cur = [], ""
        for word in s.split():
            trial = f"{cur} {word}".strip()
            if cur and self.width(trial, size, weight) > maxw:
                lines.append(cur)
                cur = word
            else:
                cur = trial
        return lines + [cur] if cur else lines
        return lines

    def para(self, x, y, s, size, weight="regular", fill=INK, maxw=W - 2 * M, lead=1.22):
        """Wrapped text from y down. Returns the y under its last line."""
        self.said.append(s)
        for line in self.wrap(s, size, weight, maxw):
            self.text(x, y, line, size, weight, fill, say=False)
            y += round(size * lead)
        return y

    def eyebrow(self, s, y=236):
        self.d.rounded_rectangle((M * S, (y + 8) * S, (M + 10) * S, (y + 34) * S), radius=3 * S,
                                 fill=NEON)
        self.text(M + 28, y, s.upper(), 34, "bold", ACCENT, track=3)
        self.said[-1] = s

    def note(self, s, size=None):
        """The scope line, set so its last line sits above the progress bar."""
        if size is None:
            # a wide page is shown smaller in the feed, so its small print is larger
            size = 36 if WIDE else 28
        self.said.append(s)
        lines = self.wrap(s, size, "regular", W - 2 * M)
        y = H - 100 - len(lines) * round(size * 1.36)
        self.d.line((M * S, (y - 26) * S, (W - M) * S, (y - 26) * S), fill=HAIR, width=S)
        for line in lines:
            self.text(M, y, line, size, "regular", INK2, say=False)
            y += round(size * 1.36)

    def bar(self, x0, x1, y, h, color):
        """A bar square at its baseline and rounded at its data end."""
        self.d.rounded_rectangle((x0 * S, y * S, x1 * S, (y + h) * S), radius=6 * S, fill=color)
        self.d.rectangle((x0 * S, y * S, (x0 + 12) * S, (y + h) * S), fill=color)

    def box(self, x0, y0, x1, y1, fill=SURFACE, outline=HAIR, radius=22):
        self.d.rounded_rectangle((x0 * S, y0 * S, x1 * S, y1 * S), radius=radius * S, fill=fill,
                                 outline=outline, width=S)


def squared(slide, glow):
    """The slide at 4:5 shrunk onto a 1080x1080 page in the same background.
    LinkedIn fits a document page to its frame's height, so a 4:5 page
    leaves room at the sides where the neighbouring pages show. A square
    page fills the frame."""
    from PIL import Image, ImageChops, ImageDraw, ImageFilter
    side = H * S * 4 // 5          # 1080 at S = 1
    im = Image.new("RGB", (W * S, W * S), BG)
    halo = Image.new("L", im.size, 0)
    ImageDraw.Draw(halo).ellipse((620 * S, -420 * S, 1500 * S, 380 * S), fill=round(255 * glow))
    halo = halo.filter(ImageFilter.GaussianBlur(170 * S))
    im.paste(Image.new("RGB", im.size, NEON), (0, 0), halo)
    small = slide.resize((W * S * 4 // 5, side), Image.LANCZOS)
    # only what was drawn on the slide covers the square's own glow
    diff = ImageChops.difference(small, Image.new("RGB", small.size, BG)).convert("L")
    mask = diff.point(lambda v: 255 if v > 6 else v * 40)
    im.paste(small, ((W * S - small.width) // 2, (W * S - side) // 2), mask)
    return im


def scope(pl):
    return (f"On the Mac: the median of {pl['n']} size-matched encodes, CIF to 4K, both at "
            f"preset medium on an Apple M5 Max.")


def hook(s, pl):
    s.para(M, 250, "A new HEVC encoder, written in C.", 42, "medium", INK2)
    s.text(M - 14, 318, bc.headline_times(pl), 318, "heavy", NEON)
    y = s.para(M, 700, "faster than x265", 104, "bold", lead=1.1)
    y = s.para(M, y + 22, "at the same file size, with better quality.", 54, "medium", INK2,
               lead=1.25)
    s.text(M, y + 62, "Four reasons to switch", 40, "bold", ACCENT)
    x = M + s.width("Four reasons to switch", 40, "bold") + 22
    s.d.line((x * S, (y + 87) * S, (x + 54) * S, (y + 87) * S), fill=ACCENT, width=4 * S)
    s.d.line(((x + 38) * S, (y + 73) * S, (x + 54) * S, (y + 87) * S, (x + 38) * S, (y + 101) * S),
             fill=ACCENT, width=4 * S, joint="curve")
    s.note(scope(pl) + " Quality is VMAF v1.")


def faster(s, fl):
    ok = [r for r in fl["2160p"] if r[1]]
    ours, ref = sum(r[1] for r in ok), sum(r[2] for r in ok)
    s.eyebrow("Faster")
    y = s.para(M, 306, f"{COUNT.get(len(ok), len(ok))} 4K films in about half the time.",
               84, "bold", lead=1.12)
    y += 84
    full = W - 2 * M - 250
    for name, v, color in (("x265", ref, bc.NEUTRAL), ("yah265", ours, NEON)):
        s.text(M, y, name, 40, "bold" if name == "yah265" else "medium",
               INK if name == "yah265" else INK2)
        x1 = M + full * v / ref
        s.bar(M, x1, y + 62, 116, color)
        s.text(x1 + 26, y + 62 + 58, f"{v / 60:.0f} min", 62, "bold", anchor="lm")
        y += 244
    s.note(f"{COUNT.get(len(ok), len(ok))} ten-minute films at 4K on the Mac, an Apple M5 Max "
           f"on all cores. Both encoders ran at preset medium and made files the same size.")


def better(s, pl):
    s.eyebrow("Better looking")
    s.text(M - 10, 300, f"{pl['dv']:+.2f}", 290, "heavy", NEON)
    y = s.para(M, 660, "VMAF v1 points higher than x265 at the same file size.", 68, "bold",
               lead=1.16)
    s.para(M, y + 30, "VMAF is Netflix's quality metric.", 44, "medium", INK2)
    s.note(scope(pl))


def easy(s):
    s.eyebrow("Easy to try")
    y = s.para(M, 306, "Change one word in your ffmpeg command.", 84, "bold", lead=1.12) + 56
    s.box(M, y, W - M, y + 372)
    for i, c in enumerate(("#ff5f57", "#febc2e", "#28c840")):
        cx, cy = M + 44 + i * 34, y + 42
        s.d.ellipse(((cx - 10) * S, (cy - 10) * S, (cx + 10) * S, (cy + 10) * S), fill=c)
    x, size = M + 40, 39
    s.text(x, y + 88, "ffmpeg -i in.mp4 \\", size, "medium", INK, mono=True)
    old, new = "  -c:v libx265 \\", "  -c:v libyah265 \\"
    s.text(x, y + 152, old, size, "medium", INK3, mono=True)
    w0 = s.width("  -c:v ", size, "medium", mono=True)
    w1 = s.width("  -c:v libx265", size, "medium", mono=True)
    s.d.line(((x + w0 - 6) * S, (y + 177) * S, (x + w1 + 6) * S, (y + 177) * S), fill=INK2,
             width=3 * S)
    w2 = s.width("  -c:v libyah265", size, "bold", mono=True)
    s.d.rounded_rectangle(((x + w0 - 12) * S, (y + 208) * S, (x + w2 + 16) * S, (y + 270) * S),
                          radius=10 * S, fill="#45200f")
    s.text(x, y + 216, "  -c:v ", size, "medium", INK, mono=True, say=False)
    s.text(x + w0, y + 216, "libyah265", size, "bold", ACCENT, mono=True, say=False)
    s.text(x + w2 + 4, y + 216, " \\", size, "medium", INK, mono=True, say=False)
    s.said.append(new)
    s.text(x, y + 280, "  -preset medium -crf 25 out.mp4", size, "medium", INK, mono=True)
    s.para(M, y + 420, "The presets use x265's names.", 44,
           "medium", INK2, lead=1.3)
    s.note("One script in the repository builds that ffmpeg for you.")


def everywhere(s, sps):
    behind = [(m, k) for m in bc.MACHINES for k, v in sps[m].items() if k[2] == "G3" and v[0] >= 1]
    if behind:
        sys.exit(f"the machines slide says faster on all three, but x265 leads in {behind}")
    s.eyebrow("Runs where you do")
    y = s.para(M, 306, "Tuned for the chips you encode on.", 84, "bold", lead=1.12) + 52
    names = {"mac": "Apple silicon", "x86": "x86-64", "g4": "AWS Graviton"}
    for m in bc.MACHINES:
        s.box(M, y, W - M, y + 124)
        s.d.rounded_rectangle((M * S, (y + 26) * S, (M + 8) * S, (y + 98) * S), radius=4 * S,
                              fill=NEON)
        s.text(M + 44, y + 62, names[m], 52, "bold", anchor="lm")
        s.text(W - M - 40, y + 62, bc.MACHINE_LABEL[m].split(", ")[1], 36, "medium", INK2,
               mono=True, anchor="rm")
        y += 146
    s.para(M, y + 18, f"Faster than x265 on all {COUNT[len(bc.MACHINES)].lower()}.", 48, "bold")
    s.note("Timed against x265 at the same file size on an Apple M5 Max, an Intel Xeon and a "
           "Graviton4. Both encoders ran at preset medium on all cores.")


def action(s):
    y = s.para(M, 262, "Try it on your own content.", 108, "bold", lead=1.08) + 56
    s.box(M, y, W - M, y + 148, fill="#45200f", outline=NEON)
    size = 44
    while s.width(REPO, size, "bold", mono=True) > W - 2 * M - 80:
        size -= 1
    s.text(W / 2, y + 74, REPO, size, "bold", "#ffd9c7", mono=True, anchor="mm")
    y += 148 + 64
    y = s.para(M, y, "Scripts are provided to reproduce my results. All data from those "
               "tests are also included.", 46, "medium", INK, lead=1.3)
    s.para(M, y + 34, "I want your feedback!", 46, "medium", INK2, lead=1.3)
    s.note("The docs and the results are linked in the post.", 32)


def hook_wide(s, pl):
    s.para(M, 236, "A new HEVC encoder, written in C.", 42, "medium", INK2)
    s.text(M - 14, 300, bc.headline_times(pl), 360, "heavy", NEON)
    rw = W - M - RX
    y = s.para(RX, 360, "faster than x265", 104, "bold", maxw=rw, lead=1.1)
    y = s.para(RX, y + 22, "at the same file size, with better quality.", 54, "medium", INK2,
               maxw=rw, lead=1.25)
    s.text(RX, y + 56, "Four reasons to switch", 40, "bold", ACCENT)
    x = RX + s.width("Four reasons to switch", 40, "bold") + 22
    s.d.line((x * S, (y + 81) * S, (x + 54) * S, (y + 81) * S), fill=ACCENT, width=4 * S)
    s.d.line(((x + 38) * S, (y + 67) * S, (x + 54) * S, (y + 81) * S, (x + 38) * S, (y + 95) * S),
             fill=ACCENT, width=4 * S, joint="curve")
    s.note(scope(pl) + " Quality is VMAF v1.")


def faster_wide(s, fl):
    ok = [r for r in fl["2160p"] if r[1]]
    ours, ref = sum(r[1] for r in ok), sum(r[2] for r in ok)
    s.eyebrow("Faster")
    s.para(M, 306, f"{COUNT.get(len(ok), len(ok))} 4K films in about half the time.",
           84, "bold", maxw=LW, lead=1.12)
    y = 290
    full = W - M - RX - 250
    for name, v, color in (("x265", ref, bc.NEUTRAL), ("yah265", ours, NEON)):
        s.text(RX, y, name, 40, "bold" if name == "yah265" else "medium",
               INK if name == "yah265" else INK2)
        x1 = RX + full * v / ref
        s.bar(RX, x1, y + 62, 116, color)
        s.text(x1 + 26, y + 62 + 58, f"{v / 60:.0f} min", 62, "bold", anchor="lm")
        y += 244
    s.note(f"{COUNT.get(len(ok), len(ok))} ten-minute films at 4K on the Mac, an Apple M5 Max "
           f"on all cores. Both encoders ran at preset medium and made files the same size.")


def better_wide(s, pl):
    s.eyebrow("Better looking")
    s.text(M - 10, 320, f"{pl['dv']:+.2f}", 300, "heavy", NEON)
    rw = W - M - RX
    y = s.para(RX, 370, "VMAF v1 points higher than x265 at the same file size.", 68, "bold",
               maxw=rw, lead=1.16)
    s.para(RX, y + 30, "VMAF is Netflix's quality metric.", 44, "medium", INK2, maxw=rw)
    s.note(scope(pl))


def easy_wide(s):
    s.eyebrow("Easy to try")
    y = s.para(M, 306, "Change one word in your ffmpeg command.", 84, "bold", maxw=LW,
               lead=1.12)
    s.para(M, y + 40, "The presets use x265's names.", 44, "medium", INK2, maxw=LW, lead=1.3)
    x0, y = RX, 300
    s.box(x0, y, W - M, y + 372)
    for i, c in enumerate(("#ff5f57", "#febc2e", "#28c840")):
        cx, cy = x0 + 44 + i * 34, y + 42
        s.d.ellipse(((cx - 10) * S, (cy - 10) * S, (cx + 10) * S, (cy + 10) * S), fill=c)
    x, size = x0 + 40, 39
    last = "  -preset medium -crf 25 out.mp4"
    while s.width(last, size, "medium", mono=True) > W - M - x0 - 80:
        size -= 1
    s.text(x, y + 88, "ffmpeg -i in.mp4 \\", size, "medium", INK, mono=True)
    old, new = "  -c:v libx265 \\", "  -c:v libyah265 \\"
    s.text(x, y + 152, old, size, "medium", INK3, mono=True)
    w0 = s.width("  -c:v ", size, "medium", mono=True)
    w1 = s.width("  -c:v libx265", size, "medium", mono=True)
    s.d.line(((x + w0 - 6) * S, (y + 177) * S, (x + w1 + 6) * S, (y + 177) * S), fill=INK2,
             width=3 * S)
    w2 = s.width("  -c:v libyah265", size, "bold", mono=True)
    s.d.rounded_rectangle(((x + w0 - 12) * S, (y + 208) * S, (x + w2 + 16) * S, (y + 270) * S),
                          radius=10 * S, fill="#45200f")
    s.text(x, y + 216, "  -c:v ", size, "medium", INK, mono=True, say=False)
    s.text(x + w0, y + 216, "libyah265", size, "bold", ACCENT, mono=True, say=False)
    s.text(x + w2 + 4, y + 216, " \\", size, "medium", INK, mono=True, say=False)
    s.said.append(new)
    s.text(x, y + 280, last, size, "medium", INK, mono=True)
    s.note("One script in the repository builds that ffmpeg for you.")


def everywhere_wide(s, sps):
    behind = [(m, k) for m in bc.MACHINES for k, v in sps[m].items() if k[2] == "G3" and v[0] >= 1]
    if behind:
        sys.exit(f"the machines slide says faster on all three, but x265 leads in {behind}")
    s.eyebrow("Runs where you do")
    y = s.para(M, 306, "Tuned for the chips you encode on.", 84, "bold", maxw=LW, lead=1.12)
    s.para(M, y + 40, f"Faster than x265 on all {COUNT[len(bc.MACHINES)].lower()}.", 48, "bold",
           ACCENT, maxw=LW)
    names = {"mac": "Apple silicon", "x86": "x86-64", "g4": "AWS Graviton"}
    y = 300
    for m in bc.MACHINES:
        s.box(RX, y, W - M, y + 124)
        s.d.rounded_rectangle((RX * S, (y + 26) * S, (RX + 8) * S, (y + 98) * S), radius=4 * S,
                              fill=NEON)
        s.text(RX + 44, y + 62, names[m], 52, "bold", anchor="lm")
        s.text(W - M - 40, y + 62, bc.MACHINE_LABEL[m].split(", ")[1], 36, "medium", INK2,
               mono=True, anchor="rm")
        y += 146
    s.note("Timed against x265 at the same file size on an Apple M5 Max, an Intel Xeon and a "
           "Graviton4. Both encoders ran at preset medium on all cores.")


def action_wide(s):
    s.para(M, 262, "Try it on your own content.", 108, "bold", maxw=LW, lead=1.08)
    rw = W - M - RX
    y = 290
    s.box(RX, y, W - M, y + 148, fill="#45200f", outline=NEON)
    size = 44
    while s.width(REPO, size, "bold", mono=True) > rw - 80:
        size -= 1
    s.text((RX + W - M) / 2, y + 74, REPO, size, "bold", "#ffd9c7", mono=True, anchor="mm")
    y += 148 + 56
    y = s.para(RX, y, "Scripts are provided to reproduce my results. All data from those "
               "tests are also included.", 44, "medium", INK, maxw=rw, lead=1.3)
    s.para(RX, y + 30, "I want your feedback!", 44, "medium", INK2, maxw=rw, lead=1.3)
    s.note("The docs and the results are linked in the post.", 40)


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--out", default=None, help="where to write (default local/launch)")
    ap.add_argument("--text", action="store_true", help="print the slides' words and draw nothing")
    ap.add_argument("--square", action="store_true", help="1080x1080 pages")
    ap.add_argument("--wide", action="store_true",
                    help="1920x1080 pages, which fill LinkedIn's desktop document frame")
    a = ap.parse_args()
    D = r"\d{4}-\d\d-\d\d"
    paths = {"mac": bc.newest(rf"PARITY-REPORT-yah265-{D}\.csv"),
             "x86": bc.newest(rf"PARITY-REPORT-yah265-x86-{D}\.csv"),
             "g4": bc.newest(rf"PARITY-REPORT-yah265-graviton-{D}\.csv")}
    rows = {m: bc.report_rows(p) for m, p in paths.items()}
    sps = {m: bc.speed_groups(rs) for m, rs in rows.items()}
    pl, fl = bc.headline_cells(rows["mac"]), bc.films(rows["mac"])
    if WIDE:
        deck = [("hook", lambda s: hook_wide(s, pl), 0.34),
                ("faster", lambda s: faster_wide(s, fl), 0.16),
                ("better-looking", lambda s: better_wide(s, pl), 0.16),
                ("easy-to-try", easy_wide, 0.16),
                ("runs-where-you-do", lambda s: everywhere_wide(s, sps), 0.16),
                ("try-it", action_wide, 0.34)]
    else:
        deck = [("hook", lambda s: hook(s, pl), 0.34), ("faster", lambda s: faster(s, fl), 0.16),
                ("better-looking", lambda s: better(s, pl), 0.16), ("easy-to-try", easy, 0.16),
                ("runs-where-you-do", lambda s: everywhere(s, sps), 0.16),
                ("try-it", action, 0.34)]
    out = a.out or default_out()
    pages = []
    for i, (name, draw, glow) in enumerate(deck, 1):
        s = Slide(i, len(deck), 0 if a.square else glow)
        draw(s)
        if a.text:
            print("\n\n".join(s.said) + "\n")
            continue
        from PIL import Image
        if a.square:
            s.im = squared(s.im, glow)
        os.makedirs(os.path.join(out, "slides"), exist_ok=True)
        s.im.resize((s.im.width // S, s.im.height // S), Image.LANCZOS).save(
            os.path.join(out, "slides", f"{i:02d}-{name}.png"), optimize=True)
        pages.append(s.im)
    if pages:
        kind = "-square" if a.square else "-wide" if WIDE else ""
        pdf = os.path.join(out, f"yah265-carousel{kind}.pdf")
        pages[0].save(pdf, save_all=True, append_images=pages[1:], resolution=72 * S, quality=95,
                      title="yah265", author="yah265")
        print(f"{len(pages)} slides in {pdf}")


if __name__ == "__main__":
    main()
