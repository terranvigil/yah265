# site/: the documentation site

Every page of the yah265 site is built from this directory. Edit the
markdown, rebuild, look at it.

## Editing

    scripts/site.sh            # build, check links, open the home page
    scripts/site.sh --serve    # build, check links, serve on http://localhost:8000

The first run creates a venv under `local/sitevenv` (git-ignored) and
installs `markdown`. Nothing else is needed, and no Ruby is involved. The
build writes `site/_build/` (git-ignored).

One page per file. The `---` block at the top sets the browser title and
the meta description. Everything after it is ordinary markdown. Links
between pages use the `.md` extension and become `.html` at build time,
so they work on GitHub too. A link to a repository file such as
`docs/knobs.md` becomes a link to that file on GitHub.

Every build ends with a link check. Each internal link must name a built
page, and each `#fragment` an id on that page, or the build fails.

## What's where

| file | page |
|---|---|
| `index.md` | home |
| `start.md` | getting started |
| `design.md` | design |
| `hdr.md` | HDR, built from `docs/hdr.md` |
| `results.md` | results, built from the latest parity report |
| `check-it-yourself.md` | check it yourself |
| `blog.md` | blog, a placeholder |
| `methodology.md` | methodology, built but left out of the nav; results links to it |
| `_layout.html` | the shell every page is poured into |
| `assets/site.css` | shared styles |

The nav order lives in `NAV` at the top of `scripts/build_site.py`.
The tooling is adapted from yah264's site.

## Publishing

Nothing publishes automatically. GitHub Actions is disabled on this
repository on purpose, so the workflow deploy yah264 uses isn't available.
The built site is plain static HTML, so a branch deploy serves it:

    scripts/site.sh
    git worktree add ../yah265-pages --orphan gh-pages
    cp -R site/_build/. ../yah265-pages/
    git -C ../yah265-pages add -A
    git -C ../yah265-pages commit -m "site: build"
    git -C ../yah265-pages push origin gh-pages

Then set Settings, Pages, Source to "Deploy from a branch", branch
`gh-pages`, folder `/`. The build writes a `.nojekyll` file, so Pages
serves the HTML as it is.

The repository is private. Pages on a private repository needs a paid
GitHub plan, and on a personal account the published site is public even
when the repository isn't.
