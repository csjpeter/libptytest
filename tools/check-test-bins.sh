#!/usr/bin/env bash
# Checks that the tests declared in tests/CMakeLists.txt and the files in
# <build>/tests/<category>/bin/ are the same set:
#
#   - every "project_test(<category> <name> ...)" line has an executable
#     <build>/tests/<category>/bin/<name>;
#   - every file in a bin/ directory is declared;
#   - no name is declared twice within its category.
#
# A declared test missing from bin/ would silently never run; an undeclared
# file would be a test nobody asked for.  See policies/testing-model.md,
# "How a test is declared, built and found".
#
# Usage: check-test-bins.sh <build-dir> [<declaration-file>]
#        check-test-bins.sh --self-test
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

# check <build-dir> <declaration-file>: prints the problems, returns their count.
check() {
    local build="$1" decl="$2" problems=0
    if [ ! -f "$decl" ]; then
        echo "no declaration file: $decl"
        return 1
    fi

    # Declarations: the integrator's reading of the file, one shape only.
    local -A declared=()
    local cat name line
    while IFS= read -r line; do
        cat="$(echo "$line" | sed -E 's/^project_test\([[:space:]]*([^[:space:])]+)[[:space:]]+([^[:space:])]+).*/\1/')"
        name="$(echo "$line" | sed -E 's/^project_test\([[:space:]]*([^[:space:])]+)[[:space:]]+([^[:space:])]+).*/\2/')"
        if [ -n "${declared[$cat/$name]:-}" ]; then
            echo "declared twice: $cat $name"
            problems=$((problems + 1))
        fi
        declared[$cat/$name]=1
    done < <(grep -E '^project_test\([[:space:]]*[^[:space:])]+[[:space:]]+[^[:space:])]+' "$decl")

    if [ "${#declared[@]}" -eq 0 ]; then
        echo "no project_test(...) declaration in $decl"
        return 1
    fi

    local key
    for key in "${!declared[@]}"; do
        local f="$build/tests/${key%%/*}/bin/${key#*/}"
        if [ ! -f "$f" ]; then
            echo "declared but not built: ${key%%/*} ${key#*/} (no $f)"
            problems=$((problems + 1))
        elif [ ! -x "$f" ]; then
            echo "declared but not executable: $f"
            problems=$((problems + 1))
        fi
    done

    local f rel
    if [ -d "$build/tests" ]; then
        while IFS= read -r f; do
            rel="${f#"$build/tests/"}"            # <category>/bin/<name>
            key="${rel%%/*}/${rel##*/}"
            if [ -z "${declared[$key]:-}" ]; then
                echo "in bin/ but not declared: $f"
                problems=$((problems + 1))
            fi
        done < <(find "$build/tests" -mindepth 3 -maxdepth 3 -path '*/bin/*' | sort)
    fi

    if [ "$problems" -eq 0 ]; then
        echo "tests: ${#declared[@]} declared, all built into $build/tests/<category>/bin/, nothing else there"
    fi
    return "$problems"
}

# --self-test: the check has to be able to fail.
self_test() {
    local work="$ROOT/build/_testbins-$$" fails=0 out rc
    mkdir -p "$work/tests/unit/bin" "$work/tests/pty/bin"
    cat > "$work/decl.txt" <<'DECL'
# a comment mentioning project_test(unit ignored) is not a declaration
project_test(unit test_a SOURCES a.c)
project_test(pty  test_b SOURCES b.c LIBS x)
DECL
    printf '#!/bin/sh\n' > "$work/tests/unit/bin/test_a"
    printf '#!/bin/sh\n' > "$work/tests/pty/bin/test_b"
    chmod 755 "$work/tests/unit/bin/test_a" "$work/tests/pty/bin/test_b"

    expect() {  # expect <want-rc: 0|fail> <text-or-empty> <description>
        rc=0
        out="$(check "$work" "$work/decl.txt")" || rc=$?
        if { [ "$1" = 0 ] && [ "$rc" -ne 0 ]; } || { [ "$1" = fail ] && [ "$rc" -eq 0 ]; } ||
           { [ -n "$2" ] && ! grep -qF "$2" <<<"$out"; }; then
            echo "SELF-TEST FAIL: $3 (exit $rc): $out"
            fails=$((fails + 1))
        else
            echo "self-test ok: $3"
        fi
    }

    expect 0 "2 declared" "matching tree passes"
    printf '#!/bin/sh\n' > "$work/tests/unit/bin/stray"
    chmod 755 "$work/tests/unit/bin/stray"
    expect fail "in bin/ but not declared: $work/tests/unit/bin/stray" "undeclared file fails"
    rm "$work/tests/unit/bin/stray"
    rm "$work/tests/pty/bin/test_b"
    expect fail "declared but not built: pty test_b" "missing test fails"
    printf '#!/bin/sh\n' > "$work/tests/pty/bin/test_b"
    chmod 644 "$work/tests/pty/bin/test_b"
    expect fail "declared but not executable" "non-executable test fails"
    chmod 755 "$work/tests/pty/bin/test_b"
    echo 'project_test(unit test_a SOURCES again.c)' >> "$work/decl.txt"
    expect fail "declared twice: unit test_a" "duplicate name fails"

    [ -n "$ROOT" ] && [ -d "$ROOT/build" ] || { echo "unsafe root '$ROOT'"; return 1; }
    ( cd "$ROOT/build" && rm -rf "_testbins-$$" )
    if [ "$fails" -ne 0 ]; then
        echo "$fails self-test(s) failed"
        return 1
    fi
    echo "self-test: all 5 cases behave"
}

case "${1:-}" in
    --self-test) self_test ;;
    "") echo "usage: $0 <build-dir> [<declaration-file>] | --self-test" >&2; exit 2 ;;
    *) check "$1" "${2:-$ROOT/tests/CMakeLists.txt}" ;;
esac
