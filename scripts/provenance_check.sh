#!/bin/sh
# Machine-checkable half of the clean-room policy (CONTRIBUTING.md): fail
# on x265/x264/HM attribution strings or assembly files in the shipped
# tree. Run before any DSP merge; cheap. Until 2026-09-14 it also banned
# GPL notices, which would flag an x265 or x264 header by its license
# block; the tree's own license is GPL-2.0-or-later since 20d9415, so
# that pattern would flag every SPDX line, and the attribution strings
# carry the check alone now.

set -u
fail=0

# shipped tree = everything git tracks except docs and this script's home
files=$(git ls-files -- src include cli tests 2>/dev/null)
[ -n "$files" ] || { echo "provenance: nothing tracked yet"; exit 0; }

for pat in 'x265 Project' 'MulticoreWare' 'VideoLAN' 'x264 project' \
           'Fraunhofer' 'JCT-VC' 'HM Reference'; do
    hits=$(grep -l "$pat" $files 2>/dev/null)
    if [ -n "$hits" ]; then
        echo "provenance FAIL: '$pat' found in:"
        echo "$hits"
        fail=1
    fi
done

asm=$(git ls-files -- '*.S' '*.s' '*.asm' 2>/dev/null)
if [ -n "$asm" ]; then
    echo "provenance FAIL: assembly files tracked (policy: intrinsics only):"
    echo "$asm"
    fail=1
fi

[ "$fail" -eq 0 ] && echo "provenance: clean"
exit "$fail"
