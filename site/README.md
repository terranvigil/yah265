# The documentation site

Every page of the yah265 site is built from this directory. Edit the
markdown, rebuild, look at it.

## Editing

    scripts/site.sh            # build, check links, open the home page
    scripts/site.sh --serve    # build, check links, serve on http://localhost:8000

The first run creates a venv under `local/sitevenv` (git-ignored) and
installs `markdown` into it. Nothing else is needed: no Ruby is involved.
The build writes `site/_build/` (git-ignored).

One page per file. The `---` block at the top sets the browser title and
the meta description. Everything after it is ordinary markdown. Links
between pages use the `.md` extension. Because they become `.html` only at
build time, they work on GitHub too. A link to a repository file such as
`docs/knobs.md` becomes a link to that file on GitHub.

Every build ends with a link check. The build fails unless each internal link
names a built page and each `#fragment` names an id on that page.

## What's where

| file | page |
|---|---|
| `index.md` | home |
| `start.md` | getting started |
| `design.md` | design |
| `hdr.md` | HDR (the build takes it from `docs/hdr.md`) |
| `results.md` | results (the build takes it from the latest parity report) |
| `check-it-yourself.md` | check it yourself |
| `blog.md` | blog: the parity post (scripts/blog_charts.py fills its figures and draws `assets/blog/`) |
| `methodology.md` | methodology (built and linked from results, but left out of the nav) |
| `_layout.html` | the shell every page is poured into |
| `assets/site.css` | shared styles |

The nav order lives in `NAV` at the top of `scripts/build_site.py`.
The tooling is adapted from yah264's site.

## Publishing

Nothing publishes automatically. Because GitHub Actions is disabled on
this repository on purpose, the workflow deploy that yah264 uses isn't available.
Because the built site is plain static HTML, a branch deploy serves it:

    scripts/site.sh
    git worktree add ../yah265-pages --orphan gh-pages
    cp -R site/_build/. ../yah265-pages/
    git -C ../yah265-pages add -A
    git -C ../yah265-pages commit -m "site: build"
    git -C ../yah265-pages push origin gh-pages

Then set Settings, Pages, Source to "Deploy from a branch", branch
`gh-pages`, folder `/`. Pages serves the HTML as it is because the build
writes a `.nojekyll` file.

The repository is private. Pages on a private repository needs a paid
GitHub plan. On a personal account the published site is public even
when the repository isn't.
