#!/usr/bin/env bash
# Copyright (c) 2026, the yah265 authors
# SPDX-License-Identifier: GPL-2.0-or-later
# Build the docs site. Creates the venv on first run.
#
#   scripts/site.sh           build and check; opens the home page from a terminal
#   scripts/site.sh --serve   build, then serve on http://localhost:8300
#   scripts/site.sh --strict  also run every chart script's --check
set -euo pipefail

root="$(cd "$(dirname "$0")/.." && pwd)"
venv="$root/local/sitevenv"

if [ ! -x "$venv/bin/python" ]; then
  echo "creating $venv"
  mkdir -p "$root/local"
  # built aside and moved in, so two first runs at once don't collide
  tmp="$venv.$$"
  python3 -m venv "$tmp"
  "$tmp/bin/pip" -q install markdown
  if [ -x "$venv/bin/python" ]; then
    rm -rf "$tmp"
  else
    rm -rf "$venv"
    mv "$tmp" "$venv"
    # a venv records its own path; point it at where it now lives
    python3 -m venv --upgrade "$venv" >/dev/null
  fi
fi

serve=0
for a in "$@"; do [ "$a" = "--serve" ] && serve=1; done

status=0
"$venv/bin/python" "$root/scripts/build_site.py" "$@" || status=$?

if [ "$serve" = 0 ] && [ "$status" = 0 ] && [ -t 1 ]; then
  open "$root/site/_build/index.html"
fi
exit "$status"
