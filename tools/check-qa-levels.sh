#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-only
# Copyright (c) 2026 Peter Csaszar (Császár Péter)
# Created: 2026-11-10
# Tests manage.sh's QA levels: which gates check (QA 1) and check-deep (QA 2) run.
#
# policies/testing-model.md, "QA levels": check never starts valgrind or the
# coverage measurement; it skips the suites when every changed path is
# non-code, and a test file counts as code; check-deep runs valgrind and
# coverage.  Every case below can fail.
#
# Usage: check-qa-levels.sh
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

tmp="$(mktemp -d "/tmp/check-qa-levels-$$-XXXXXX")"
case "$tmp" in /tmp/check-qa-levels-*) ;; *) echo "ERROR: unsafe path: '$tmp'" >&2; exit 1 ;; esac
cleanup() {
    [ -n "$tmp" ] && [ -d "$tmp" ] && ( cd "$tmp" && rm -rf repo ) || true
    rm -f "$tmp/notice" "$tmp/err" "$tmp/out"
    rmdir "$tmp"
}
trap cleanup EXIT

fails=0
fail() { echo "check-qa-levels: FAIL: $1" >&2; fails=$((fails + 1)); }

export MANAGE_SOURCE_ONLY=1
# shellcheck disable=SC1091
source "$ROOT/manage.sh"
set -e

has() { case " $1 " in *" $2 "*) return 0 ;; esac; return 1; }

# gates_for <paths...>: the QA 1 gates (stdout) and the notice (stderr, kept
# in $tmp/notice) for a diff of those paths.
gates_for() {
    printf '%s\n' "$@" | qa1_gates 2> "$tmp/notice"
}

# 1. A code change: all of QA 1, and nothing of QA 2.
g="$(gates_for src/pty_screen.c README.md)"
for s in unit pty functional package consumer package-output licence; do
    has "$g" "$s" || fail "code diff: check lacks $s"
done
for s in valgrind coverage result-format; do
    ! has "$g" "$s" || fail "code diff: check runs $s"
done
[ ! -s "$tmp/notice" ] || fail "code diff: a skip notice was printed"

# 2. A test file is code.
g="$(gates_for tests/unit/test_screen.c)"
has "$g" unit || fail "test-file diff: unit suite skipped"
has "$g" pty || fail "test-file diff: pty suite skipped"

# 3. Only non-code: the suites are skipped, the cheap checks stay, it says so.
g="$(gates_for README.md docs/functional-coverage.md man/man3/pty_open.3 LICENSE .gitignore)"
for s in unit pty functional; do
    ! has "$g" "$s" || fail "docs diff: check runs $s"
done
for s in licence private-names package; do
    has "$g" "$s" || fail "docs diff: check lacks $s"
done
for s in valgrind coverage; do
    ! has "$g" "$s" || fail "docs diff: check runs $s"
done
grep -qx 'no code changed: suites skipped: unit pty functional' "$tmp/notice" \
    || fail "docs diff: notice is '$(cat "$tmp/notice")'"

# 4. One code path among docs, and an empty diff, run everything.
g="$(gates_for docs/a.md tools/check-x.sh)"
has "$g" unit || fail "mixed diff: suites skipped"
g="$(printf '' | qa1_gates 2> "$tmp/notice")"
has "$g" unit || fail "empty diff: suites skipped"

# 5. check-deep: valgrind and coverage, the sanitizer suites, the result check.
g="$(qa2_gates)"
for s in valgrind coverage unit pty functional result-format; do
    has "$g" "$s" || fail "check-deep lacks $s"
done

# 6. The list of non-code paths is one variable, and the verbs exist in help.
[ -n "$NON_CODE_PATHS" ] || fail "NON_CODE_PATHS is empty"
h="$(usage)"
case "$h" in *"check-deep "*"QA 2"*) ;; *) fail "help does not describe check-deep as QA 2" ;; esac
case "$h" in *"check "*"QA 1"*) ;; *) fail "help does not describe check as QA 1" ;; esac

# 6b. "step N/M: NAME": one line per step, M = the steps that really run
# (a docs-only diff has fewer), N counting from 1.  A stub manage.sh stands
# in for the gates; the output is run_gates' own.
stub="$tmp/stub"
mkdir -p "$stub"
printf '#!/bin/sh\nexit 0\n' > "$stub/manage.sh"
mkdir "$stub/tools"
cp "$stub/manage.sh" "$stub/tools/check-package-output.sh"
chmod +x "$stub/manage.sh" "$stub/tools/check-package-output.sh"
real_script_dir="$SCRIPT_DIR"
real_build_dir="$BUILD_DIR"
SCRIPT_DIR="$stub"
BUILD_DIR="$stub/build"
check_steps() { # <label> <gates>
    local label="$1" gates="$2" n=0 g exp m
    m="$(wc -w <<< "$gates")"
    run_gates "$gates" > "$tmp/out" 2> "$tmp/err" || fail "$label: stub run failed: $(tail -n 3 "$tmp/out"; tail -n 3 "$tmp/err")"
    exp=""
    for g in $gates; do n=$((n + 1)); exp+="step $n/$m: $g"$'\n'; done
    [ "$(grep -E '^step ' "$tmp/out")"$'\n' = "$exp" ] \
        || fail "$label: step lines are '$(grep -E '^step ' "$tmp/out" | tr '\n' ';')', expected '$exp'"
}
docs_gates="$(printf '%s\n' README.md | qa1_gates 2> "$tmp/notice")"
code_gates="$(printf '%s\n' src/a.c | qa1_gates 2> "$tmp/notice")"
check_steps "check" "$code_gates"
check_steps "check (docs only)" "$docs_gates"
check_steps "check-deep" "$(qa2_gates)"
[ "$(wc -w <<< "$docs_gates")" -lt "$(wc -w <<< "$code_gates")" ] || fail "skipped steps are counted in M"
check_steps "single step" "unit"
grep -qx 'step 1/1: unit' "$tmp/out" || fail "single step: no 'step 1/1: unit' line"
SCRIPT_DIR="$real_script_dir"
BUILD_DIR="$real_build_dir"
rm -f "$stub/manage.sh" "$stub/tools/check-package-output.sh"
rm -f "$stub/build/_logs"/*.log
rmdir "$stub/build/_logs" "$stub/build" "$stub/tools" "$stub"

# 7. The real diff against a base, in a throw-away repository.
git init -q -b main "$tmp/repo"
git -C "$tmp/repo" config user.email t@example.org
git -C "$tmp/repo" config user.name t
mkdir "$tmp/repo/src"
echo a > "$tmp/repo/src/a.c"
git -C "$tmp/repo" add -A
git -C "$tmp/repo" commit -q -m base
git -C "$tmp/repo" update-ref refs/remotes/origin/main HEAD
SCRIPT_DIR="$tmp/repo"
QA_BASE=origin/main
echo doc > "$tmp/repo/README.md"
git -C "$tmp/repo" add README.md
git -C "$tmp/repo" commit -q -m docs
p="$(changed_paths)"
[ "$p" = "README.md" ] || fail "changed_paths: '$p'"
printf '%s\n' "$p" | all_non_code || fail "docs-only branch is not non-code"
echo b >> "$tmp/repo/src/a.c"
p="$(changed_paths)"
if printf '%s\n' "$p" | all_non_code; then fail "an edited source file counts as non-code"; fi
git -C "$tmp/repo" checkout -q -- src/a.c
echo t > "$tmp/repo/new_test.c"
p="$(changed_paths)"
if printf '%s\n' "$p" | all_non_code; then fail "an untracked source file counts as non-code"; fi
QA_BASE=origin/nonexistent
if changed_paths 2> "$tmp/err" > "$tmp/out"; then fail "an unknown base was accepted"; fi

if [ "$fails" -ne 0 ]; then
    echo "check-qa-levels: $fails failure(s)" >&2
    exit 1
fi
echo "check-qa-levels: ok"
