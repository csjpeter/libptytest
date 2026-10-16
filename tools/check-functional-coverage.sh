#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-only
# Copyright (c) 2026 Peter Csaszar (Császár Péter)
# Created: 2026-10-16
# Checks docs/functional-coverage.md against the exported API: a capability with no row fails.
#
# The capabilities of a library are its exported functions (PTYTEST_API in
# include/ptytest.h) and its assertion macros (include/pty_assert.h).  Fails
# when
#   - one has no row in the map;
#   - a row names a test file or a test function that does not exist;
#   - a row is a gap without a ticket key ("GAP PTY-N").
# See policies/testing-model.md, "Every capability has a functional test".
#
# Usage: check-functional-coverage.sh [<root>]
#        check-functional-coverage.sh --self-test
set -euo pipefail

SELF="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/$(basename "${BASH_SOURCE[0]}")"

# capabilities <root>: one symbol per line.
capabilities() {
    local root="$1"
    grep -ho 'PTYTEST_API [^(]*(' "$root/include/ptytest.h" \
        | sed 's/($//; s/.*[ *]//' | grep '^pty_'
    grep -ho '^#define ASSERT[A-Z_]*' "$root/include/pty_assert.h" | awk '{print $2}'
}

# check <root>: prints the problems, returns 1 if there is any.
check() {
    local root="$1" map="$1/docs/functional-coverage.md" problems=0 sym row test file fn
    if [ ! -f "$map" ]; then
        echo "missing map: $map"
        return 1
    fi
    local caps
    caps="$(capabilities "$root")"
    if [ -z "$caps" ]; then
        echo "no capability found in the headers of $root"
        return 1
    fi
    while IFS= read -r sym; do
        row="$(grep -F "| \`$sym\` |" "$map" || true)"
        if [ -z "$row" ]; then
            echo "no row for capability: $sym"
            problems=$((problems + 1))
            continue
        fi
        test="$(echo "$row" | sed -E 's/^\|[^|]*\| *`?([^`|]*)`? *\|.*/\1/')"
        case "$test" in
            GAP\ [A-Z]*-[0-9]*) ;;
            GAP*) echo "gap without a ticket: $sym"; problems=$((problems + 1)) ;;
            *::*)
                file="${test%%::*}"
                fn="${test#*::}"
                if [ ! -f "$root/$file" ]; then
                    echo "$sym: test file does not exist: $file"
                    problems=$((problems + 1))
                elif ! grep -Eq "^static (void|int) $fn\(" "$root/$file"; then
                    echo "$sym: test function not found: $test"
                    problems=$((problems + 1))
                fi
                ;;
            *) echo "$sym: malformed row: $row"; problems=$((problems + 1)) ;;
        esac
    done <<<"$caps"
    [ "$problems" -eq 0 ]
}

self_test() {
    local work
    work="$(mktemp -d "/tmp/ptytest-fcov-$$-XXXXXX")"
    case "$work" in /tmp/ptytest-fcov-*) ;; *) echo "ERROR: unsafe path" >&2; exit 1 ;; esac
    mkdir -p "$work/include" "$work/docs" "$work/tests"
    printf 'PTYTEST_API int pty_alpha(void);\nPTYTEST_API void pty_beta(int x);\n' > "$work/include/ptytest.h"
    printf '#define ASSERT_GAMMA(s) do {} while (0)\n' > "$work/include/pty_assert.h"
    printf 'static void test_a(void) {}\n' > "$work/tests/t.c"
    local good='| `pty_alpha` | `tests/t.c::test_a` |
| `pty_beta` | `tests/t.c::test_a` |
| `ASSERT_GAMMA` | GAP PTY-1 |'
    local rc=0
    echo "$good" > "$work/docs/functional-coverage.md"
    "$SELF" "$work" >/dev/null || { echo "self-test: a complete map failed" >&2; rc=1; }
    echo "$good" | grep -v pty_beta > "$work/docs/functional-coverage.md"
    ! "$SELF" "$work" >/dev/null || { echo "self-test: a missing row passed" >&2; rc=1; }
    echo "${good//test_a/test_zz}" > "$work/docs/functional-coverage.md"
    ! "$SELF" "$work" >/dev/null || { echo "self-test: a missing test function passed" >&2; rc=1; }
    echo "${good//GAP PTY-1/GAP}" > "$work/docs/functional-coverage.md"
    ! "$SELF" "$work" >/dev/null || { echo "self-test: a gap without ticket passed" >&2; rc=1; }
    ( cd "$work" && rm -rf include docs tests )
    rmdir "$work"
    [ "$rc" -eq 0 ] && echo "check-functional-coverage self-test: ok"
    return "$rc"
}

if [ "${1:-}" = "--self-test" ]; then
    self_test
else
    ROOT="${1:-$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)}"
    check "$ROOT"
    echo "functional coverage map: every capability has a row"
fi
