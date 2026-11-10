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
