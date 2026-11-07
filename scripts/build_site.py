#!/usr/bin/env python3
# Copyright (c) 2026, the yah265 authors
# SPDX-License-Identifier: GPL-2.0-or-later
"""Build the documentation site from site/*.md into site/_build/.

Adapted from yah264's scripts/build_site.py (its codec-agnostic tooling).
Requires the `markdown` package; scripts/site.sh sets up a venv.

Usage:
    python3 scripts/build_site.py [--serve] [--port N]

Every build ends with a link check: each internal href must name a built
file, and each #fragment an id on that page. A broken link fails the build.
"""

import argparse
import html
import http.server
import pathlib
import re
import shutil
import socketserver
import sys

try:
    import markdown
except ImportError:
    sys.exit("missing dependency: pip install markdown (or run scripts/site.sh)")

ROOT = pathlib.Path(__file__).resolve().parent.parent
SRC = ROOT / "site"
OUT = SRC / "_build"

GITHUB = "https://github.com/terranvigil/yah265/blob/main/"

# Nav order: built page, label in the top bar.
NAV = [
    ("index.html", "Home"),
    ("start.html", "Getting started"),
    ("design.html", "Design"),
    ("hdr.html", "HDR"),
    ("results.html", "Results"),
    ("check-it-yourself.html", "Check it yourself"),
    ("blog.html", "Blog"),
]

# Built and published, but left out of the nav on purpose (the owner's
# rule, as on yah264's site). Results links to it.
OFF_NAV = {"methodology.md"}

# Kept in the repository, never built.
UNPUBLISHED = {"README.md"}

SMPTE = (
    '<div class="smpte" aria-hidden="true">'
    '<span style="background:#b8b8a8"></span><span style="background:#b8b83e"></span>'
    '<span style="background:#3eb8b8"></span><span style="background:#3eb83e"></span>'
    '<span style="background:#b83eb8"></span><span style="background:#b83e3e"></span>'
    '<span style="background:#3e3eb8"></span></div>'
)


def parse_front_matter(text):
    """A `key: value` block between --- lines at the top of a file."""
    meta = {}
    if text.startswith("---\n"):
        end = text.find("\n---\n", 4)
        if end != -1:
            for line in text[4:end].splitlines():
                if ":" in line:
                    k, v = line.split(":", 1)
                    meta[k.strip()] = v.strip()
            text = text[end + 5:]
    return meta, text.lstrip("\n")


def render_nav(current):
    out = []
    for href, label in NAV:
        mark = ' aria-current="page"' if href == current else ""
        out.append(f'<a href="{href}"{mark}>{html.escape(label)}</a>')
    return "\n      ".join(out)


def render_pager(current):
    hrefs = [h for h, _ in NAV]
    if current not in hrefs:
        return ""
    i = hrefs.index(current)
    out = []
    if i > 0:
        h, l = NAV[i - 1]
        out.append(f'<a class="prev" href="{h}"><span>Previous</span>{html.escape(l)}</a>')
    else:
        out.append("<span></span>")
    if i + 1 < len(NAV):
        h, l = NAV[i + 1]
        out.append(f'<a class="next" href="{h}"><span>Next</span>{html.escape(l)}</a>')
    return '<nav class="pager">' + "".join(out) + "</nav>"


def render_rail(current, title, sections):
    """Every nav page in order, this page's sections nested under it. An
    off-nav page gets its own entry at the end so its sections still show."""
    rows = []
    found = False
    for h, l in NAV:
        if h == current:
            found = True
            rows.append(f'<li class="here"><a href="{h}" aria-current="page">'
                        f'{html.escape(l)}</a>{sections}</li>')
        else:
            rows.append(f'<li><a href="{h}">{html.escape(l)}</a></li>')
    if not found:
        rows.append(f'<li class="here"><a href="{current}" aria-current="page">'
                    f'{html.escape(title)}</a>{sections}</li>')
    return '<div class="lbl">Pages</div><ol class="pages">' + "".join(rows) + "</ol>"


def render_sections(tokens):
    def walk(nodes, out):
        for n in nodes:
            if n.get("level") == 2:
                out.append(n)
            walk(n.get("children") or [], out)
        return out

    items = walk(tokens, [])
    if not items:
        return ""
    rows = "".join(
        f'<li><a href="#{t["id"]}" data-s="{t["id"]}">{html.escape(t["name"])}</a></li>'
        for t in items
    )
    return f'<ol class="sections">{rows}</ol>'


def repo_links(page):
    """A relative link to a repository file (docs/, scripts/, ...) points at
    that file on GitHub, so a page can cite its sources."""
    def fix(m):
        href = m.group(1)
        if re.match(r"(\.\./)?(docs|scripts|spec|src|include|tests|cli)/|"
                    r"(\.\./)?(Makefile|README\.md|LICENSE|CONTRIBUTING\.md)$", href):
            return f'href="{GITHUB}{href.removeprefix("../")}"'
        return m.group(0)
    return re.sub(r'href="([^"#:]+)"', fix, page)


def build():
    if OUT.exists():
        shutil.rmtree(OUT)
    OUT.mkdir(parents=True)

    layout = (SRC / "_layout.html").read_text()
    md = markdown.Markdown(extensions=["extra", "toc", "sane_lists", "smarty"])
    shutil.copytree(SRC / "assets", OUT / "assets")
    # GitHub Pages serving a branch runs Jekyll unless told not to
    (OUT / ".nojekyll").write_text("")

    pages = 0
    for src in sorted(SRC.glob("*.md")):
        if src.name.startswith("_") or src.name in UNPUBLISHED:
            continue
        meta, body = parse_front_matter(src.read_text())
        md.reset()
        content = md.convert(body)
        content = content.replace("</h1>", "</h1>" + SMPTE, 1)
        target = src.with_suffix(".html").name
        title = meta.get("title", src.stem)
        short = title.split(" - ")[0]
        sections = render_sections(getattr(md, "toc_tokens", []))
        page = (
            layout.replace("{{content}}", content)
            .replace("{{title}}", html.escape(title))
            .replace("{{description}}", html.escape(meta.get("description", "")))
            .replace("{{nav}}", render_nav(target))
            .replace("{{toc}}", render_rail(target, short, sections))
            .replace("{{pager}}", render_pager(target))
            .replace("{{og_type}}", meta.get("type", "website"))
        )
        page = repo_links(page)
        # source links say page.md so they also work on GitHub
        page = re.sub(r'href="([^"#:/]+)\.md((?:#[^"]*)?)"', r'href="\1.html\2"', page)
        (OUT / target).write_text(page)
        pages += 1

    built = {p.name for p in OUT.glob("*.html")}
    missing = [h for h, _ in NAV if h not in built]
    if missing:
        sys.exit(f"nav names pages that were not built: {missing}")
    print(f"built {pages} pages -> {OUT}")


def check_links():
    """Every internal link names a built page, and every fragment an id."""
    ids = {}
    for p in OUT.glob("*.html"):
        ids[p.name] = set(re.findall(r'\sid="([^"]+)"', p.read_text()))
    bad = []
    for p in sorted(OUT.glob("*.html")):
        for href in re.findall(r'href="([^"]+)"', p.read_text()):
            if re.match(r"[a-z]+:|//", href) or href.startswith("data:"):
                continue
            path, _, frag = href.partition("#")
            name = path or p.name
            if path.startswith("assets/"):
                if not (OUT / path).exists():
                    bad.append(f"{p.name}: {href}")
                continue
            if name not in ids:
                bad.append(f"{p.name}: {href} (no such page)")
            elif frag and frag not in ids[name]:
                bad.append(f"{p.name}: {href} (no such anchor)")
    if bad:
        print("broken links:\n  " + "\n  ".join(bad))
        sys.exit(1)
    print("links: every internal link resolves")


def serve(port):
    handler = lambda *a, **kw: http.server.SimpleHTTPRequestHandler(
        *a, directory=str(OUT), **kw
    )
    with socketserver.TCPServer(("", port), handler) as httpd:
        print(f"serving {OUT} at http://localhost:{port}/  (ctrl-c to stop)")
        httpd.serve_forever()


if __name__ == "__main__":
    ap = argparse.ArgumentParser()
    ap.add_argument("--serve", action="store_true", help="serve after building")
    ap.add_argument("--port", type=int, default=8000)
    args = ap.parse_args()
    build()
    check_links()
    if args.serve:
        serve(args.port)
