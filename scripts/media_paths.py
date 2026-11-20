"""Where this box keeps its test media.

The tracked scripts name shared media as `<media>/...` and scratch as
`<staging>/...`. The roots come from the env vars YAH265_MEDIA and
YAH265_STAGING, or else from the untracked local/paths.env at the repo
root (KEY=VALUE lines, # comments). The env var wins.

    import media_paths as mp
    mp.media()                        # the media root, or exit naming it
    mp.expand("<media>/tier_s")       # placeholder -> real path
"""
import os

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ENV_FILE = os.path.join(ROOT, "local", "paths.env")
VARS = {"<media>": "YAH265_MEDIA", "<staging>": "YAH265_STAGING"}

_file = None


def _read_file():
    global _file
    if _file is None:
        _file = {}
        try:
            with open(ENV_FILE) as fh:
                for line in fh:
                    line = line.strip()
                    if not line or line.startswith("#") or "=" not in line:
                        continue
                    k, v = line.split("=", 1)
                    k = k.strip()
                    if k.startswith("export "):
                        k = k[len("export "):].strip()
                    _file[k] = v.strip().strip("'\"")
        except OSError:
            pass
    return _file


def _root(tag, required):
    var = VARS[tag]
    v = os.environ.get(var) or _read_file().get(var)
    if v:
        return os.path.expanduser(v).rstrip("/") or "/"
    if required:
        raise SystemExit(
            f"media_paths: {tag} is not configured. Set {var}, or put "
            f"'{var}=/path' in {ENV_FILE} (untracked).")
    return None


def media(required=True):
    return _root("<media>", required)


def staging(required=True):
    return _root("<staging>", required)


def expand(path, required=True):
    """`path` with a leading <media> or <staging> replaced by its root.
    Other paths come back unchanged. With required=False an unconfigured
    root gives None instead of exiting."""
    for tag in VARS:
        if path == tag or path.startswith(tag + "/"):
            r = _root(tag, required)
            return None if r is None else r + path[len(tag):]
    return path


def under_roots(path):
    """True if `path` is under the configured media or staging root."""
    p = os.path.abspath(path)
    for tag in VARS:
        r = _root(tag, False)
        if r and (p == r or p.startswith(r.rstrip("/") + "/")):
            return True
    return False
