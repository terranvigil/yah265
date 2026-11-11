#!/usr/bin/env python3
"""Carry parity results from one binary to another that writes the same
streams. Ladder rows are copied to the new binary's key; points and
windows are rebound in place. Each carried row names the binary it was
measured on. Use it only after an identity cmp shows the two binaries
write the same bytes on the carried rows' modes and classes.

  alias_bin.py OLD NEW [--skip MODE:CLASS ...]

--skip vbr:2160p leaves 2160p VBR rows on OLD, to be re-measured."""
import argparse, json, os

D = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "docs", "data")


def skipped(key, skips):
    parts = key.split("|")
    cls = parts[0].split("/")[1] if "/" in parts[0] else ""
    mode = parts[1] if len(parts) > 1 else ""
    return any(mode == m and cls == c for m, c in skips)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("old")
    ap.add_argument("new")
    ap.add_argument("--skip", action="append", default=[])
    a = ap.parse_args()
    skips = [tuple(s.split(":")) for s in a.skip]
    p = os.path.join(D, "parity-spec-ladders.json")
    lad = json.load(open(p))
    n = 0
    for k in [k for k in lad if k.endswith("|" + a.old) and "|ours|" in k]:
        if skipped(k, skips):
            continue
        nk = k[:-len(a.old)] + a.new
        if nk not in lad:
            lad[nk] = dict(lad[k], aliased_from=a.old)
            n += 1
    json.dump(lad, open(p, "w"), indent=1, sort_keys=True)
    print(f"ladders: {n} rows carried")
    for f in ("points", "windows"):
        p = os.path.join(D, f"parity-spec-{f}.json")
        d = json.load(open(p))
        m = 0
        for k, v in d.items():
            if isinstance(v, dict) and v.get("bin") == a.old:
                v["bin"], v["aliased_from"] = a.new, a.old
                m += 1
        json.dump(d, open(p, "w"), indent=1, sort_keys=True)
        print(f"{f}: {m} rebound")


if __name__ == "__main__":
    main()
