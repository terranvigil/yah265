# Copyright (c) 2026, the yah265 authors
# SPDX-License-Identifier: GPL-2.0-or-later
#
# scratch.sh -- per-run scratch directories that go away again. Sourced, not
# run: `. scripts/scratch.sh` then `yah265_scratch_dir <tag> <varname>`.
#
# CALL IT DIRECTLY, NEVER IN `$(...)`. A command substitution is a subshell;
# the trap would be armed there and would fire the moment the substitution
# ended, deleting the directory before the caller's first write. That is why
# this takes the name of a variable to set instead of printing a path.
#
# The directory is removed on EXIT, on INT and on TERM, so a Ctrl-C or a
# killed agent leaves nothing behind. KEEP_SCRATCH=1 keeps it instead and
# prints the path, which is what you want when a cell failed and you need to
# look at what it wrote.
#
# The name is "$TMPDIR/yah265-<tag>.XXXXXXXX": one namespace, listed in
# scripts/scratch-prefixes.txt, so `make clean-scratch` can find a leftover
# from a run that died without running any trap at all (SIGKILL, a panic, a
# box that lost power).
#
# PER-RUN SCRATCH ONLY. A cache or a record that is meant to survive the run
# does not belong under $TMPDIR at all -- macOS sweeps it -- and lives under
# $YAH265_CACHE instead (yah265_cache_dir below).

yah265_scratch__dirs=()
yah265_scratch__armed=0
yah265_scratch__done=0

yah265_scratch_cleanup() {
    [ "$yah265_scratch__done" = "1" ] && return 0
    yah265_scratch__done=1
    local d
    for d in ${yah265_scratch__dirs[@]+"${yah265_scratch__dirs[@]}"}; do
        [ -n "$d" ] || continue
        if [ "${KEEP_SCRATCH:-0}" = "1" ]; then
            [ -d "$d" ] && echo "KEEP_SCRATCH: kept $d" >&2
        else
            rm -rf "$d"
        fi
    done
}

# yah265_scratch_dir <tag> <varname>   -> sets <varname> to the path
yah265_scratch_dir() {
    # Locals are spelled _yah265s_* because the caller hands us the NAME of the
    # variable to set, and a plain `local d` would shadow a caller asking for
    # `d`: the assignment would land on the local and vanish on return.
    local _yah265s_tag="${1:?yah265_scratch_dir: need a tag}"
    local _yah265s_var="${2:?yah265_scratch_dir: need a variable to set}"
    local _yah265s_ns _yah265s_d _yah265s_tmp
    _yah265s_ns="$(yah265_scratch_ns)"
    _yah265s_tmp="${TMPDIR:-/tmp}"; _yah265s_tmp="${_yah265s_tmp%/}"
    _yah265s_d="$(mktemp -d "$_yah265s_tmp/${_yah265s_ns}${_yah265s_tag}.XXXXXXXX")" || return 1
    yah265_scratch__dirs+=("$_yah265s_d")
    if [ "$yah265_scratch__armed" = "0" ]; then
        yah265_scratch__armed=1
        trap 'yah265_scratch_cleanup' EXIT
        trap 'yah265_scratch_cleanup; exit 130' INT
        trap 'yah265_scratch_cleanup; exit 143' TERM HUP
    fi
    eval "$_yah265s_var=\$_yah265s_d"
}

# The namespace, read from the shared list rather than spelled again here.
yah265_scratch_ns() {
    local f
    f="$(dirname "${BASH_SOURCE[0]}")/scratch-prefixes.txt"
    awk '$1=="ns"{print $2; exit}' "$f" 2>/dev/null || true
}

# yah265_cache_dir <name>    a NAMED, non-temp home for things that must outlive
# the run (an encode cache, a solve record). Created on demand, never swept.
yah265_cache_dir() {
    local name="${1:?yah265_cache_dir: need a name}" root
    root="${YAH265_CACHE:-$HOME/.cache/yah265}"
    mkdir -p "$root/$name" || return 1
    printf '%s\n' "$root/$name"
}

# yah265_scratch_df_guard [gb]   refuse to start on a nearly-full temp volume.
# The disk-full symptom is not "no space left on device": it is a harness cell
# reading an empty reconstruction file and reporting a content failure.
yah265_scratch_df_guard() {
    local need="${1:-20}" dir free
    dir="${TMPDIR:-/tmp}"
    free=$(df -g "$dir" 2>/dev/null | awk 'NR==2 {print $4}')
    case "$free" in ''|*[!0-9]*) return 0 ;; esac   # unreadable df: do not block
    if [ "$free" -lt "$need" ]; then
        echo "$(basename "$0"): only ${free} GB free on $dir, need ${need} GB." >&2
        echo "  Run 'make clean-scratch' to drop stale harness scratch, then retry." >&2
        return 1
    fi
    return 0
}
