#!/usr/bin/env python3
# Copyright (c) 2026, the yah265 authors
# SPDX-License-Identifier: GPL-2.0-or-later
"""Build the documentation site from site/*.md into site/_build/.

Requires the `markdown` package; scripts/site.sh sets up a venv.

Usage:
    python3 scripts/build_site.py [--strict] [--serve] [--port N] [--host H]

The output is static and every link in it is relative, so the directory
serves from any path, GitHub Pages included.

Every build ends with a link check: each internal href must name a built
file, each #fragment an id on that page, each image a built asset and
each repository link a file git doesn't ignore. A broken link fails the
build. `--strict` also fails on a page with no description, on a
repository link to an untracked file, and on a chart script whose
`--check` says a page or a chart is stale.

`--serve` rebuilds when a source file changes, so a reload shows the edit.
"""

import argparse
import html
import http.server
import json
import os
import pathlib
import re
import hashlib
import shutil
import subprocess
import sys
import threading

try:
    import markdown
except ImportError:
    sys.exit("missing dependency: pip install markdown (or run scripts/site.sh)")

ROOT = pathlib.Path(__file__).resolve().parent.parent
SRC = ROOT / "site"
OUT = SRC / "_build"

GITHUB_REPO = "https://github.com/terranvigil/yah265"
GITHUB = GITHUB_REPO + "/blob/main/"
# og:image needs an absolute address
SITE_URL = "https://terranvigil.github.io/yah265/"

# Reading order: group in the top bar ("" for a page that stands alone),
# built page, label. The top bar, the rail and the pager all follow it.
NAV = [
    ("", "index.html", "Home"),
    ("Use", "start.html", "Build and first encode"),
    ("Use", "guide.html", "Encoding guide"),
    ("Use", "from-x265.html", "Coming from x265"),
    ("Use", "playground.html", "Try it in your browser"),
    ("Use", "bench.html", "Try it on your own clip"),
    ("Use", "options.html", "Options"),
    ("Use", "hdr.html", "HDR"),
    ("Understand", "hevc.html", "The HEVC standard"),
    ("Understand", "design.html", "Design"),
    ("Understand", "threading.html", "Threading"),
    ("Understand", "architectures.html", "Architectures"),
    ("Evidence", "results.html", "Results"),
    ("Evidence", "compare.html", "Watch the encodes"),
    ("Evidence", "x265-run.html", "How x265 was run"),
    ("Evidence", "check-it-yourself.html", "Check it yourself"),
    ("Evidence", "faq.html", "A skeptic's questions"),
    ("", "blog.html", "Blog"),
]

# Built and published, but left out of the nav on purpose. Results links
# to methodology. shell-test shows every shell component on one page.
OFF_NAV = {"methodology.md", "shell-test.md"}
# Left out of the search index and marked noindex.
UNLISTED = {"shell-test.md"}

# Kept in the repository, never built.
UNPUBLISHED = {"README.md"}

LOGOS = ["yah265-logo-light.png", "yah265-logo-dark.png"]

# Run with --check under --strict, where they exist.
CHART_SCRIPTS = ["blog_charts.py", "site_front.py", "charts_internals.py",
                 "charts_results.py", "site_trust.py", "share_cards.py"]

# Working notes that aren't published with the repository. A link to one
# counts as a link to a missing file, on the site and in the README.
PRIVATE = re.compile(
    r"^(docs/(plan|ideas|x86-plan|x265-feature-audit|parity-journal|threading-design|"
    r"instruments)\.md|docs/phase[^/]*\.md|docs/research/|docs/[^/]*handoff[^/]*|"
    r"(.*/)?HANDOFF[^/]*|(.*/)?CLAUDE\.md)", re.I)

# A page's link card is its own image under SHARE, drawn by
# scripts/share_cards.py. The launch post's card stands in for a missing one.
SHARE = "assets/share"
DEFAULT_CARD = "assets/blog/card.png"

REPO_HREF = re.compile(
    r"(\.\./)?(docs|scripts|spec|src|include|tests|cli)/|"
    r"(\.\./)?(Makefile|README\.md|LICENSE|CONTRIBUTING\.md)$")

def _block_strip():
    # coding blocks under each title: 64/32/16/8 widths, like one row of a
    # split HEVC frame; seeded so every build writes the same strip
    import random
    rng = random.Random(265)
    tints = ["neon", "glow", "accent-lt", "accent", "accent-soft"]
    spans, used = [], 0
    while used < 768:
        w = min(rng.choice([64, 32, 32, 16, 16, 16, 8, 8]), 768 - used)
        spans.append(f'<span style="flex-grow:{w};background:var(--{rng.choice(tints)})"></span>')
        used += w
    return '<div class="titlerule" aria-hidden="true">' + "".join(spans) + "</div>"


TITLE_RULE = _block_strip()

LOGO = (
    '<picture><source srcset="assets/yah265-logo-dark.png" '
    'media="(prefers-color-scheme: dark)">'
    '<img src="assets/yah265-logo-light.png" alt="yah265" width="909" height="220">'
    '</picture>'
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


def nav_groups():
    """NAV as runs: [(group, [(href, label), ...]), ...] in order."""
    runs = []
    for group, href, label in NAV:
        if group and runs and runs[-1][0] == group:
            runs[-1][1].append((href, label))
        else:
            runs.append((group, [(href, label)]))
    return runs


def link(href, label, current):
    mark = ' aria-current="page"' if href == current else ""
    return f'<a href="{href}"{mark}>{html.escape(label)}</a>'


def render_nav(current):
    """The desktop top bar: lone pages as links, each group as a
    <details> menu so it opens without JavaScript."""
    out = []
    for group, pages in nav_groups():
        if not group:
            out.append(link(pages[0][0], pages[0][1], current))
            continue
        here = ' class="here"' if any(h == current for h, _ in pages) else ""
        items = "".join(link(h, l, current) for h, l in pages)
        out.append(f'<details class="grp" name="topnav"><summary{here}>'
                   f'{html.escape(group)}</summary><div class="menu">{items}</div></details>')
    return "\n      ".join(out)


def render_mobile_nav(current):
    """The phone's menu: every page, under its group label."""
    out = []
    for group, pages in nav_groups():
        if group:
            out.append(f'<div class="lbl">{html.escape(group)}</div>')
        out.extend(link(h, l, current) for h, l in pages)
    return "".join(out)


def render_pager(current):
    hrefs = [h for _, h, _ in NAV]
    if current not in hrefs:
        return ""
    i = hrefs.index(current)

    def side(cls, word, entry):
        group, href, label = entry
        over = f"{word} in {group}" if group else word
        return (f'<a class="{cls}" href="{href}"><span>{html.escape(over)}</span>'
                f'{html.escape(label)}</a>')

    out = [side("prev", "Previous", NAV[i - 1]) if i > 0 else "<span></span>"]
    if i + 1 < len(NAV):
        out.append(side("next", "Next", NAV[i + 1]))
    return '<nav class="pager" aria-label="Previous and next page">' + "".join(out) + "</nav>"


def render_rail(current, title, sections):
    """Every nav page in order under its group label, this page's sections
    nested under it. An off-nav page gets its own entry at the end so its
    sections still show."""
    out = []
    found = False
    for group, pages in nav_groups():
        if group:
            out.append(f'<div class="lbl">{html.escape(group)}</div>')
        rows = []
        for h, l in pages:
            if h == current:
                found = True
                rows.append(f'<li class="here">{link(h, l, current)}{sections}</li>')
            else:
                rows.append(f"<li>{link(h, l, current)}</li>")
        out.append('<ol class="pages">' + "".join(rows) + "</ol>")
    if not found:
        out.append('<ol class="pages"><li class="here">'
                   f'{link(current, title, current)}{sections}</li></ol>')
    return "".join(out)


def h2_items(tokens):
    def walk(nodes, out):
        for n in nodes:
            if n.get("level") == 2:
                out.append(n)
            walk(n.get("children") or [], out)
        return out
    return walk(tokens, [])


def render_sections(items):
    if not items:
        return ""
    rows = "".join(
        f'<li><a href="#{t["id"]}" data-s="{t["id"]}">{html.escape(html.unescape(t["name"]))}</a></li>'
        for t in items
    )
    return f'<ol class="sections">{rows}</ol>'


def render_onpage(items):
    """The section list again, folded, for widths where the rail is gone."""
    if not items:
        return ""
    return ('<details class="onpage"><summary>On this page</summary>'
            f'{render_sections(items)}</details>')


def repo_links(page):
    """A relative link to a repository file (docs/, scripts/, ...) points at
    that file on GitHub, so a page can cite its sources."""
    cited = []

    def fix(m):
        href = m.group(1)
        if REPO_HREF.match(href):
            cited.append(href.removeprefix("../"))
            return f'href="{GITHUB}{href.removeprefix("../")}"'
        return m.group(0)
    return re.sub(r'href="([^"#:]+)"', fix, page), cited


def plain(fragment):
    """Readable text of an HTML fragment, for the search index."""
    fragment = re.sub(r"(?s)<(svg|script|style)\b.*?</\1>", " ", fragment)
    fragment = re.sub(r"(?s)<!--.*?-->", " ", fragment)
    text = html.unescape(re.sub(r"<[^>]+>", " ", fragment))
    return re.sub(r"\s+", " ", text).strip()


def search_records(target, label, content):
    """One record per h2 section, and one for what comes before the first."""
    parts = re.split(r'(?s)<h2 id="([^"]+)">(.*?)</h2>', content)
    recs = []
    intro = plain(re.sub(r"(?s)<h1.*?</h1>", " ", parts[0]))
    if intro:
        recs.append({"p": target, "t": label, "h": label, "a": "", "x": intro[:6000]})
    for i in range(1, len(parts), 3):
        recs.append({"p": target, "t": label, "h": plain(parts[i + 1]),
                     "a": parts[i], "x": plain(parts[i + 2])[:6000]})
    return recs


def commit_hash():
    try:
        return subprocess.run(["git", "-C", str(ROOT), "rev-parse", "--short", "HEAD"],
                              capture_output=True, text=True, check=True).stdout.strip()
    except (OSError, subprocess.CalledProcessError):
        return ""


def swap_in(tmp):
    """Put a finished build at OUT. Several writers may build at once, so
    the old tree moves aside first and a lost race is retried."""
    old = tmp.with_name(tmp.name + ".old")
    for _ in range(20):
        try:
            if OUT.exists():
                os.rename(OUT, old)
            os.rename(tmp, OUT)
            break
        except OSError:
            shutil.rmtree(old, ignore_errors=True)
    else:
        sys.exit(f"could not replace {OUT}")
    shutil.rmtree(old, ignore_errors=True)


def build(strict=False):
    """Returns (problems, cited): the pages' own faults, and every
    repository file a page links to."""
    scratch = ROOT / "local"
    scratch.mkdir(exist_ok=True)
    tmp = scratch / f".site-build-{os.getpid()}"
    shutil.rmtree(tmp, ignore_errors=True)
    tmp.mkdir()

    layout = (SRC / "_layout.html").read_text()
    md = markdown.Markdown(extensions=["extra", "toc", "sane_lists", "smarty"])
    shutil.copytree(SRC / "assets", tmp / "assets")
    for name in LOGOS:
        logo = ROOT / "assets" / name
        if not logo.exists():
            sys.exit(f"missing logo: {logo}")
        shutil.copy2(logo, tmp / "assets" / name)
    # GitHub Pages serving a branch runs Jekyll unless told not to
    (tmp / ".nojekyll").write_text("")
    # the playground is a static app beside the pages; its encoder module
    # comes from `make wasm`, which the Pages workflow runs first
    pg = ROOT / "playground"
    shutil.copytree(pg, tmp / "playground",
                    ignore=shutil.ignore_patterns("wasm", "vmaf"))
    for mod in ("yah265.wasm", "vmaf.wasm"):
        if not (pg / mod).exists():
            print(f"warning: playground/{mod} is missing; run `make wasm`")
    # a version on every URL the page loads, so a browser never mixes a new
    # page with a cached worker or module from the publish before it; the
    # scripts pass it on to the workers and the .wasm files
    h = hashlib.sha256()
    for f in sorted((tmp / "playground").glob("*.*")):
        h.update(f.name.encode() + f.read_bytes())
    v = "?v=" + h.hexdigest()[:10]
    index = tmp / "playground" / "index.html"
    page = index.read_text()
    for ref in ('href="style.css"', 'src="mp4.js"', 'src="app.js"'):
        page = page.replace(ref, ref[:-1] + v + '"')
    index.write_text(page)

    labels = {h: l for _, h, l in NAV}
    commit = commit_hash()
    built_from = (
        f'Built from commit <a href="{GITHUB_REPO}/commit/{commit}"><code>{commit}</code></a>.'
        if commit else "")
    problems, cited, index = [], {}, []
    pages = 0
    for src in sorted(SRC.glob("*.md")):
        if src.name.startswith("_") or src.name in UNPUBLISHED:
            continue
        meta, body = parse_front_matter(src.read_text())
        md.reset()
        content = md.convert(body)
        target = src.with_suffix(".html").name
        h1 = re.search(r"(?s)<h1[^>]*>(.*?)</h1>", content)
        h1_text = plain(h1.group(1)) if h1 else src.stem
        title = meta.get("title") or (
            h1_text if h1_text == "yah265" else f"{h1_text} - yah265")
        short = title.split(" - ")[0]
        description = meta.get("description", "")
        if not description:
            (problems if strict else []).append(f"{src.name}: no description")
            if not strict:
                print(f"warning: {src.name} has no description (--strict fails on it)")
        if target == "index.html":
            content = re.sub(r"<h1[^>]*>yah265</h1>", f'<h1 class="mark">{LOGO}</h1>',
                             content, count=1)
        content = content.replace("</h1>", "</h1>" + TITLE_RULE, 1)
        for fig in re.findall(r'(?s)<figure class="diagram[^"]*">.*?</figure>', content):
            if not (re.search(r'<svg class="d-wide"', fig)
                    and re.search(r'<svg class="d-narrow" viewBox="0 0 360 ', fig)):
                name = re.search(r"<title[^>]*>(.*?)</title>", fig)
                problems.append(f"{src.name}: the diagram '{name[1] if name else '?'}' needs a "
                                "d-wide drawing and a 360-wide d-narrow one")
        content, unsourced = provenance(content)
        if strict:
            problems += [f"{src.name}: {u} has no provenance entry" for u in unsourced]
        content = narrow_charts(content)
        content = content.replace("<table>", '<div class="tbl"><table>')
        content = content.replace("</table>", "</table></div>")
        if target == "options.html":
            # three columns don't fit a phone; site.css stacks these rows there
            content = content.replace('<div class="tbl"><table>', '<div class="tbl"><table class="stack">')
        items = h2_items(getattr(md, "toc_tokens", []))
        sections = render_sections(items)
        if src.name not in UNLISTED:
            index.extend(search_records(target, labels.get(target, short), content))

        own = f"{SHARE}/{src.stem}.png"
        image = meta.get("image", "") or (own if (tmp / own).exists() else DEFAULT_CARD)
        if strict and image == DEFAULT_CARD and not meta.get("image"):
            problems.append(f"{src.name}: no share card (run scripts/share_cards.py)")
        social = ""
        if image:
            if not (tmp / image).exists():
                problems.append(f"{src.name}: image {image} is not a built asset")
            social = (f'<meta property="og:image" content="{SITE_URL}{html.escape(image)}">\n'
                      f'<meta name="twitter:image" content="{SITE_URL}{html.escape(image)}">\n')
        if src.name in UNLISTED:
            social += '<meta name="robots" content="noindex">\n'
        page = (
            layout.replace("{{content}}", content)
            .replace("{{title}}", html.escape(title))
            .replace("{{description}}", html.escape(description))
            .replace("{{nav}}", render_nav(target))
            .replace("{{mobile_nav}}", render_mobile_nav(target))
            .replace("{{toc}}", render_rail(target, short, sections))
            .replace("{{onpage}}", render_onpage(items))
            .replace("{{pager}}", render_pager(target))
            .replace("{{og_type}}", meta.get("type", "website"))
            .replace("{{og_url}}", SITE_URL + ("" if target == "index.html" else target))
            .replace("{{twitter_card}}", "summary_large_image" if image else "summary")
            .replace("{{social}}", social)
            .replace("{{built_from}}", built_from)
        )
        page, links = repo_links(page)
        for href in links:
            cited.setdefault(href, src.name)
        # source links say page.md so they also work on GitHub
        page = re.sub(r'href="([^"#:/]+)\.md((?:#[^"]*)?)"', r'href="\1.html\2"', page)
        (tmp / target).write_text(page)
        pages += 1

    data = json.dumps(index, ensure_ascii=False, separators=(",", ":"))
    (tmp / "search.json").write_text(data)
    # the same index as a script, so search also works from a file:// page
    (tmp / "assets" / "search-index.js").write_text(f"window.YAH265_SEARCH={data};\n")

    built = {p.name for p in tmp.glob("*.html")}
    missing = [h for _, h, _ in NAV if h not in built]
    if missing:
        shutil.rmtree(tmp, ignore_errors=True)
        sys.exit(f"nav names pages that were not built: {missing}")
    swap_in(tmp)
    print(f"built {pages} pages, {len(index)} search records -> {OUT}")
    return problems, cited


def provenance(content):
    """Under every chart: the data it was drawn from, the script that drew
    it and its numbers as a CSV. The chart scripts write provenance.json
    and the CSV beside the image. Returns the page and the charts that
    have no entry."""
    missing = []

    def sub(m):
        fig = m[0]
        img = re.search(r'<img [^>]*?src="(assets/[^"]+?)/([^"/]+)\.svg"', fig)
        if not img:
            return fig
        table = SRC / img[1] / "provenance.json"
        entry = json.loads(table.read_text()).get(img[2]) if table.exists() else None
        if not entry:
            missing.append(f"{img[1]}/{img[2]}.svg")
            return fig
        srcs = [f'<a href="{html.escape(p)}">{html.escape(label)}</a>'
                for p, label in entry["sources"]]
        said = ", ".join(srcs[:-1]) + (" and " if len(srcs) > 1 else "") + srcs[-1]
        line = (f'<span class="prov">Source: {said}. Drawn by '
                f'<a href="{entry["script"]}"><code>{entry["script"]}</code></a>. '
                f'<a href="{img[1]}/{entry["csv"]}" download>Download the chart\'s data (CSV)</a>.'
                f"</span>")
        if "</figcaption>" in fig:
            return fig.replace("</figcaption>", " " + line + "</figcaption>", 1)
        return fig.replace("</figure>", f"<figcaption>{line}</figcaption></figure>", 1)

    content = re.sub(r'(?s)<figure class="chart[^"]*">.*?</figure>', sub, content)
    return content, missing


def narrow_charts(content):
    """A chart drawn for the 900-unit canvas has a 480-unit twin beside it,
    name-narrow.svg. Phones get the twin."""
    def sub(m):
        twin = m[2] + "-narrow.svg"
        if not (SRC / twin).exists():
            return m[0]
        return (f'<picture><source media="(max-width: 40rem)" srcset="{twin}">'
                f"{m[0]}</picture>")
    return re.sub(r'<img ([^>]*?)src="(assets/[^"]+?)\.svg"[^>]*>', sub, content)


def check_readme():
    """The README is read on GitHub and never built, so its links to
    unpublished notes are looked for here."""
    text = (ROOT / "README.md").read_text()
    return [f"README.md: {h} (not published)"
            for h in re.findall(r"\]\(([^)#\s]+)", text) if PRIVATE.match(h)]


def check_repo_links(cited, strict):
    """A cited repository file must exist, must be one the repository
    publishes and must not be git-ignored. Under --strict it must be
    tracked too."""
    bad = []
    for href, page in sorted(cited.items()):
        path = href.split("#")[0]
        if PRIVATE.match(path):
            bad.append(f"{page}: {href} (not published)")
            continue
        if not (ROOT / path).exists():
            bad.append(f"{page}: {href} (no such file in the repository)")
            continue
        git = ["git", "-C", str(ROOT)]
        if subprocess.run(git + ["check-ignore", "-q", path]).returncode == 0:
            bad.append(f"{page}: {href} (git ignores it)")
        elif subprocess.run(git + ["ls-files", "--error-unmatch", path],
                            capture_output=True).returncode != 0:
            if strict:
                bad.append(f"{page}: {href} (untracked)")
            else:
                print(f"warning: {page} links {href}, which is untracked")
    return bad


def check_links():
    """Every internal link names a built page, every fragment an id, and
    every image a built file."""
    ids = {}
    for p in OUT.glob("*.html"):
        ids[p.name] = set(re.findall(r'\sid="([^"]+)"', p.read_text()))
    bad = []
    for p in sorted(OUT.glob("*.html")):
        text = p.read_text()
        for href in re.findall(r'\shref="([^"]+)"', text):
            if re.match(r"[a-z]+:|//", href):
                continue
            path, _, frag = href.partition("#")
            name = path or p.name
            if path.startswith(("assets/", "playground/")):
                if not (OUT / path).exists():
                    bad.append(f"{p.name}: {href}")
                continue
            if name not in ids:
                bad.append(f"{p.name}: {href} (no such page)")
            elif frag and frag not in ids[name]:
                bad.append(f"{p.name}: {href} (no such anchor)")
        for src in re.findall(r'\s(?:src|srcset)="([^"]+)"', text):
            if re.match(r"[a-z]+:|//", src):
                bad.append(f"{p.name}: {src} (loads from another site)")
            elif not (OUT / src.split("#")[0]).exists():
                bad.append(f"{p.name}: {src} (no such file)")
    return bad


def check_charts():
    bad = []
    for name in CHART_SCRIPTS:
        script = ROOT / "scripts" / name
        if script.exists():
            r = subprocess.run([sys.executable, str(script), "--check"])
            if r.returncode != 0:
                bad.append(f"scripts/{name} --check exited {r.returncode}")
    return bad


def run(strict):
    """Build and check. Returns the list of problems."""
    problems, cited = build(strict)
    problems += check_links() + check_repo_links(cited, strict) + check_readme()
    if strict:
        problems += check_charts()
    if problems:
        print("problems:\n  " + "\n  ".join(problems))
    else:
        print("links: every internal link resolves")
    return problems


def newest_source():
    paths = [p for p in SRC.rglob("*") if p.is_file() and "_build" not in p.parts]
    paths += [ROOT / "assets" / n for n in LOGOS]
    return max((p.stat().st_mtime for p in paths if p.exists()), default=0)


def serve(host, port, strict):
    state = {"seen": newest_source()}
    lock = threading.Lock()

    class Handler(http.server.SimpleHTTPRequestHandler):
        def __init__(self, *a, **kw):
            super().__init__(*a, directory=str(OUT), **kw)

        def do_GET(self):
            if self.path.split("?")[0].endswith(("/", ".html")):
                with lock:
                    now = newest_source()
                    if now > state["seen"]:
                        state["seen"] = now
                        try:
                            run(strict)
                        except SystemExit as e:
                            print(f"rebuild failed: {e}")
            super().do_GET()

        def end_headers(self):
            self.send_header("Cache-Control", "no-store")
            super().end_headers()

        def log_message(self, *a):
            pass

    http.server.ThreadingHTTPServer.allow_reuse_address = True
    try:
        httpd = http.server.ThreadingHTTPServer((host, port), Handler)
    except OSError as e:
        sys.exit(f"can't serve on {host}:{port}: {e}")
    with httpd:
        print(f"serving {OUT} at http://localhost:{port}/  (ctrl-c to stop)")
        try:
            httpd.serve_forever()
        except KeyboardInterrupt:
            pass


if __name__ == "__main__":
    ap = argparse.ArgumentParser()
    ap.add_argument("--serve", action="store_true",
                    help="serve after building, and rebuild when a source changes")
    ap.add_argument("--port", type=int, default=8300)
    ap.add_argument("--host", default="127.0.0.1")
    ap.add_argument("--strict", action="store_true",
                    help="also fail on a missing description, an untracked "
                         "repository link or a stale chart script")
    args = ap.parse_args()
    failed = bool(run(args.strict))
    if args.serve:
        serve(args.host, args.port, args.strict)
    sys.exit(1 if failed else 0)
