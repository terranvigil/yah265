#!/bin/sh
# Machine-checkable half of the clean-room policy (CONTRIBUTING.md): fail
# on GPL notices, x265/x264/HM attribution strings, or assembly files in
# the shipped tree. Run before any DSP merge; cheap.

set -u
fail=0

# shipped tree = everything git tracks except docs and this script's home
files=$(git ls-files -- src include cli tests 2>/dev/null)
[ -n "$files" ] || { echo "provenance: nothing tracked yet"; exit 0; }

for pat in 'GNU General Public License' 'GPLv2' 'GPL-2' \
           'x265 Project' 'MulticoreWare' 'VideoLAN' 'x264 project' \
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
