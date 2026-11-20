# The documentation site

Every page of the yah265 site is built from this directory. Edit the
markdown, rebuild and look at it.

## Editing

    scripts/site.sh            # build and check
    scripts/site.sh --serve    # build, check and serve on http://localhost:8300
    scripts/site.sh --strict   # the build a release has to pass

The first run creates a venv under `local/sitevenv` and installs `markdown`
into it. The build writes `site/_build/`. Git ignores both. Run from a
terminal, the plain build also opens the home page.

`--serve` rebuilds when a file under `site/` changes. Reload the browser to
see the edit. `--port` picks another port.

One page per file. The `---` block at the top holds the page's `title` and
`description`. It can also name an `image` for the social card. Everything
after it is ordinary markdown. Links between pages use the `.md` extension
because they have to work on GitHub too. The build turns them into `.html`.
A link to a repository file such as `docs/knobs.md` becomes a link to that
file on GitHub.

## What the build checks

Every build ends with a link check. It fails when:

- an internal link names a page that wasn't built
- a `#fragment` names an id the page doesn't have
- an image isn't among the built files, or loads from another site
- a repository link names a file that doesn't exist or that git ignores

`--strict` also fails on a page with no `description`, on a repository link
to an untracked file, and on a stale chart. It learns that a chart is stale
by running each chart script with `--check`.

## What's where

| file | page |
|---|---|
| `index.md` | Home. `scripts/site_front.py` fills its figures |
| `start.md` | Build and first encode |
| `guide.md` | Encoding guide |
| `options.md` | Options. `scripts/site_front.py` writes it from the captured `--help` |
| `hdr.md` | HDR. Written by hand to follow `docs/hdr.md` |
| `hevc.md` | The HEVC standard |
| `design.md` | Design |
| `threading.md` | Threading |
| `architectures.md` | Architectures. `scripts/charts_internals.py` draws its charts |
| `results.md` | Results. `scripts/charts_results.py` fills its tables and draws its charts from the parity data |
| `check-it-yourself.md` | Check it yourself |
| `blog.md` | Blog. `scripts/blog_charts.py` fills its figures and draws `assets/blog/` |
| `methodology.md` | Methodology. Results links to it and the nav leaves it out |
| `shell-test.md` | Every shell component on one page. It's off the nav and out of the search index |
| `_layout.html` | the shell every page is poured into |
| `assets/site.css` | styles, with the color tokens listed at the top |
| `assets/site.js` | copy buttons, heading links and search |

The builder never writes a page's content. A page with generated numbers
keeps them between `<!--gen:NAME-->` marks for its chart script to fill.
Run that script after the data changes, then build.

The reading order lives in `NAV` at the top of `scripts/build_site.py`. The
top bar, the side rail and the previous and next links all follow it.

## Shell conventions for writers

A chart is an image inside `<figure class="chart">`. The image file holds
both its light and its dark colors because a page's styles can't reach
into an image.

A diagram is inline SVG inside `<figure class="diagram">`, drawn on a
`viewBox` 720 units wide. It takes color only from these classes:

| class | draws |
|---|---|
| `d-box` | an outlined box |
| `d-fill` | a filled box |
| `d-accent` | the thing the figure is about. It also recolors a line, an arrow or text |
| `d-muted` | a box, line or text that matters less |
| `d-line` | a line |
| `d-dash` | a dashed line |
| `d-arrow` | a line that ends in the shared arrowhead |
| `d-text` | text in the prose face |
| `d-label` | small sans text |
| `d-mono` | small monospace text |

Add `wide` to a figure's class to let it use the full column on a desktop.
Tables already do. A fenced block marked `sh` gets a tinted prompt. Every
block's copy button leaves a leading `$` out.

The site follows the reader's light or dark setting. It loads no fonts,
scripts or images from another site. The link check enforces that for
images.

## Publishing

Nothing publishes automatically, because GitHub Actions is disabled on this
repository on purpose. A branch deploy can serve the built site because
it's plain static HTML with relative links:

    scripts/site.sh --strict
    git worktree add ../yah265-pages --orphan gh-pages
    cp -R site/_build/. ../yah265-pages/
    git -C ../yah265-pages add -A
    git -C ../yah265-pages commit -m "site: build"
    git -C ../yah265-pages push origin gh-pages

Then set Settings, Pages, Source to "Deploy from a branch", branch
`gh-pages`, folder `/`. Pages serves the HTML as it is because the build
writes a `.nojekyll` file.

The social card needs an absolute address. The builder takes it from
`SITE_URL`. Change that if the site moves.

The repository is private. Pages on a private repository needs a paid
GitHub plan. On a personal account the published site is public even
when the repository isn't.
