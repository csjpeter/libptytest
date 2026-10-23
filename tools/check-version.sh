#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-only
# Copyright (c) 2026 Peter Csaszar (Created: 2026-10-23)
# Tests the package version derivation (cmake/ptytest_version.cmake).
#
# policies/packaging-and-release-model.md, "Deriving the version": the version
# is project_version.<total commit count>; a tag takes no part.  Every case
# runs the real module in a throwaway repository and can fail.
#
# Usage: check-version.sh
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
MODULE="$ROOT/cmake/ptytest_version.cmake"

tmp="/tmp/check-version-$$"
mkdir "$tmp"
trap 'cd /; rm -rf "/tmp/check-version-$$"' EXIT

fails=0
fail() { echo "check-version: FAIL: $1" >&2; fails=$((fails + 1)); }

cat > "$tmp/probe.cmake" <<PROBE
include("$MODULE")
ptytest_derive_version(v "\${VER}" "\${SRC}")
message("\${v}")
PROBE

# derive <project-version> <dir>: the derived version.
derive() {
    cmake -DVER="$1" -DSRC="$2" -P "$tmp/probe.cmake" 2>&1
}

# expect <what> <project-version> <dir> <expected>
expect() {
    local got
    got="$(derive "$2" "$3")"
    [ "$got" = "$4" ] || fail "$1: got '$got', expected '$4'"
}

repo="$tmp/repo"
mkdir "$repo"
git -C "$repo" init -q
git -C "$repo" config user.name t
git -C "$repo" config user.email t@example.org
for i in 1 2 3 4 5; do
    git -C "$repo" commit -q --allow-empty -m "c$i"
done

expect "N commits" 0.1.0 "$repo" 0.1.0.5
git -C "$repo" tag -a v0.1.0 -m t
expect "same tree after a tag" 0.1.0 "$repo" 0.1.0.5
git -C "$repo" commit -q --allow-empty -m c6
expect "one more commit after the tag" 0.1.0 "$repo" 0.1.0.6
git -C "$repo" tag -a v0.1.0.6 -m t
expect "tagged with its own version" 0.1.0 "$repo" 0.1.0.6
git -C "$repo" commit -q --allow-empty -m c7
expect "next build" 0.1.0 "$repo" 0.1.0.7

# A raised project version sorts above every earlier build.
dpkg --compare-versions "$(derive 0.1.1 "$repo")" gt "0.1.0.7" \
    || fail "raised project version does not sort above 0.1.0.7"
# The tag restart of the old rule would have been a downgrade.
dpkg --compare-versions "$(derive 0.1.0 "$repo")" gt "0.1.0.6" \
    || fail "a build after a tag sorts below the earlier build"

# No git: the bare project version.
mkdir "$tmp/plain"
expect "no git" 0.1.0 "$tmp/plain" 0.1.0

# A shallow top-level clone is refused with an explanation.
git clone -q --depth 1 "file://$repo" "$tmp/shallow"
cat > "$tmp/shallow/CMakeLists.txt" <<PROBE
cmake_minimum_required(VERSION 3.20)
project(probe NONE)
include("$MODULE")
ptytest_derive_version(v 0.1.0 "\${CMAKE_SOURCE_DIR}")
message(STATUS "v=\${v}")
PROBE
mkdir "$tmp/top"
if out="$(cmake -S "$tmp/shallow" -B "$tmp/top" 2>&1)"; then
    fail "shallow top-level clone was accepted: $out"
else
    case "$out" in
        *"shallow clone"*) ;;
        *) fail "shallow refusal has no explanation: $out" ;;
    esac
fi

if [ "$fails" -ne 0 ]; then
    echo "check-version: $fails failure(s)" >&2
    exit 1
fi
echo "check-version: ok"
