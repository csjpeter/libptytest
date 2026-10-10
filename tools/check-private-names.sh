#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-only
# Copyright (c) 2026 Peter Csaszar (Császár Péter)
# Created: 2026-10-10
# Fails when a tracked file names the owner's private domain.
#
# policies/private-names-model.md: the domain is referenced nowhere.  This
# script must not contain the name itself, or it would fail its own check;
# the pattern is built from two parts joined here.
#
# Usage: check-private-names.sh [<project-dir>]
#        check-private-names.sh --self-test
#        check-private-names.sh --history [<project-dir>]   (commits that introduced it)
set -euo pipefail

PART_A="csaszar"
PART_B="[.]email"
PATTERN="${PART_A}${PART_B}"

# check <dir>: prints the offending lines, returns their count.
check() {
    local dir="$1" n=0 hit
    if ! git -C "$dir" rev-parse --is-inside-work-tree > /dev/null; then
        echo "not a git work tree: $dir"
        return 1
    fi
    while IFS= read -r hit; do
        echo "private domain: $hit"
        n=$((n + 1))
    done < <(git -C "$dir" grep -n -i -E "$PATTERN" -- . || true)
    return "$n"
}

self_test() {
    tmp="$(mktemp -d "/tmp/check-private-names-$$-XXXXXX")"
    case "$tmp" in /tmp/check-private-names-*) ;; *) echo "ERROR: unsafe path: '$tmp'" >&2; exit 1 ;; esac
    trap 'rm -f "$tmp/a.txt"; rm -rf "$tmp/.git"; rmdir "$tmp"' EXIT
    git -C "$tmp" init -q
    printf 'contact: user@example.org\nCsaszar (Császár Péter)\n' > "$tmp/a.txt"
    git -C "$tmp" add -A
    local rc=0
    check "$tmp" > /dev/null || rc=$?
    [ "$rc" -eq 0 ] || { echo "self-test: a clean tree was rejected"; return 1; }

    printf 'mail: someone@%s.%s\n' "$PART_A" "email" > "$tmp/a.txt"
    git -C "$tmp" add -A
    rc=0; check "$tmp" > /dev/null || rc=$?
    [ "$rc" -ne 0 ] || { echo "self-test: an address under the domain passed"; return 1; }

    printf 'host: box.%s.%s\n' "$PART_A" "email" | tr 'a-z' 'A-Z' > "$tmp/a.txt"
    git -C "$tmp" add -A
    rc=0; check "$tmp" > /dev/null || rc=$?
    [ "$rc" -ne 0 ] || { echo "self-test: an upper-case host under the domain passed"; return 1; }
    echo "check-private-names self-test: ok"
}

if [ "${1:-}" = "--self-test" ]; then
    self_test
    exit $?
fi

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

if [ "${1:-}" = "--history" ]; then
    dir="${2:-$ROOT}"
    # git log -S, all refs: the commits that added or removed the name.
    git -C "$dir" log --all --oneline -i -G "$PATTERN" -- . | cat
    exit 0
fi

rc=0
check "${1:-$ROOT}" || rc=$?
if [ "$rc" -eq 0 ]; then
    echo "private-names: no tracked file names the private domain"
else
    echo "private-names: $rc line(s)"
fi
exit "$rc"
