#!/usr/bin/env python3
# Copyright (c) 2026, the yah265 authors
# SPDX-License-Identifier: GPL-2.0-or-later
"""warm_check.py - every lazily cached knob is warmed on the driver.

A knob read through `static T name(void) { static T v = <unset>; if (unset)
v = ...; return v; }` is safe only if the driver calls it before any
worker exists: two workers setting `v` at once is a data race, even when
both write the same value. Each file's warm function (n265_*_warm, called
in n265_encoder_open) is where that happens. Twice a getter was added
without a warm call (N265_MERGE_PICK_RATE, N265_CB64_SPLITGATE) and only
TSan on a lucky schedule found it. This finds it at `make test`.

A getter is any `static <type> name(void)` whose body opens with a
`static` local. It's warmed if `name()` appears inside a function whose
name contains `warm`. Statics that are not lazily set (constant tables,
report printers run once at exit) are listed in NOT_KNOBS.
"""
import glob
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
NOT_KNOBS = {
    # report printers: their static locals are constant name tables
    "rt_report", "px_report", "dg_report", "et_report",
    # built once under n265_tables_init on the driver
    "build_tmatrix",
}

GETTER = re.compile(r"static\s+(?:inline\s+)?[\w \*]+?\b(\w+)\s*\(void\)\s*\{\s*static\s")
WARM_FN = re.compile(r"\n(?:static\s+)?void\s+(\w*warm\w*)\s*\(void\)\s*\{(.*?)\n\}", re.S)


def main():
    bad = []
    for path in sorted(glob.glob(os.path.join(ROOT, "src", "**", "*.c"), recursive=True)):
        src = open(path).read()
        getters = [g for g in GETTER.findall(src) if g not in NOT_KNOBS]
        if not getters:
            continue
        warmed = " ".join(body for _, body in WARM_FN.findall(src))
        for g in getters:
            if not re.search(r"\b%s\(\)" % re.escape(g), warmed):
                bad.append(f"{os.path.relpath(path, ROOT)}: {g}() is lazily cached but not warmed")
    if bad:
        print("warm_check: FAIL")
        for b in bad:
            print("  " + b)
        print("call each one from its file's warm function (see n265_ctu_warm)")
        return 1
    print("warm_check: OK")
    return 0


if __name__ == "__main__":
    sys.exit(main())
