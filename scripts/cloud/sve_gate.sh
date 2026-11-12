#!/bin/bash
# Copyright (c) 2026, the yah265 authors
# SPDX-License-Identifier: GPL-2.0-or-later
#
# sve_gate.sh -- the SVE tier's correctness gate on a Graviton box, from
# ~ubuntu/probe after setup_arm64.sh (docs/sve.md): checkasm at every tier,
# plain and under ASan+UBSan, at 8 and 10 bits; the unit tests; the escape
# cmp on the CIF-7 at medium and fast (and a Main10 subset); fast
# conformance. It reads no clock.
set -u
cd "$(dirname "$0")/src"
CORPUS=${CORPUS:-$(cd .. && pwd)}
JOBS=${JOBS:-$(nproc)}
rc=0
step() { echo "== $*"; }

step "cpu: $(build/tests/checkasm satd | head -1)"
for b in build build10 build-asan build10-asan; do
    r=$("$b/tests/checkasm" 2>&1 | tail -1)
    echo "checkasm $b: $r"
    case "$r" in *" 0 failure(s)") ;; *) rc=1 ;; esac
done
if [ -w /proc/sys/abi/sve_default_vector_length ] || sudo -n true 2>/dev/null; then
    vl=$(cat /proc/sys/abi/sve_default_vector_length)
    if [ "$vl" -gt 16 ]; then
        # the same binary at 128 bits, the VL-agnostic claim on this core
        echo 16 | sudo tee /proc/sys/abi/sve_default_vector_length >/dev/null
        for b in build build10; do
            echo "checkasm $b at VL128: $("$b/tests/checkasm" 2>&1 | head -1 | cut -c1-60) / $("$b/tests/checkasm" 2>&1 | tail -1)"
        done
        echo "$vl" | sudo tee /proc/sys/abi/sve_default_vector_length >/dev/null
    fi
fi

step "meson test"
meson test -C build --print-errorlogs 2>&1 | grep -E "^(Ok|Fail|Skipped|Expected|Unexpected|Timeout):" || rc=1

step "escape cmp, 8 bits"
CORPUS=$CORPUS JOBS=$JOBS scripts/sve_identity.sh build/n265 || rc=1
step "escape cmp, Main10 (--output-depth 10)"
CORPUS=$CORPUS JOBS=$JOBS CLIPS="foreman_cif mobile_cif stefan_cif" FRAMES=12 \
    EXTRA="--output-depth 10" scripts/sve_identity.sh build/n265 || rc=1

step "fast conformance"
[ -e tests/corpus ] || ln -s "$CORPUS" tests/corpus
export DEC265=${DEC265:-$(cd .. && pwd)/libde265/build/dec265/dec265}
N265=build/n265 scripts/conformance.sh --fast > ../conf8.log 2>&1
tail -1 ../conf8.log
grep "^SUMMARY.*FAIL" ../conf8.log && rc=1
N265_10=build/n265 scripts/conformance.sh --depth10 --fast > ../conf10.log 2>&1
tail -1 ../conf10.log | sed 's/^/Main10 /'
grep "^SUMMARY.*FAIL" ../conf10.log && rc=1
echo "gate rc=$rc"
exit $rc
