#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-only
# Copyright (c) 2026 Peter Csaszar (Császár Péter)
# Created: 2026-10-28
# Tests manage.sh tag-build, and that package never tags or pushes.
#
# policies/manage-sh-model.md: "package never tags and never pushes";
# tag-build VERSION makes the annotated tag vVERSION on HEAD locally and
# pushes nothing.  Every case runs against a throwaway repository whose
# origin is unreachable, and can fail.
#
# Usage: check-tag-build.sh
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
MANAGE="$ROOT/manage.sh"

tmp="/tmp/check-tag-build-$$"
mkdir "$tmp"
trap 'cd /; rm -rf "/tmp/check-tag-build-$$"' EXIT

fails=0
fail() { echo "check-tag-build: FAIL: $1" >&2; fails=$((fails + 1)); }

repo="$tmp/repo"
git init -q "$repo"
git -C "$repo" config user.email t@example.org
git -C "$repo" config user.name t
git -C "$repo" remote add origin "$tmp/no-such-origin"
git -C "$repo" commit -q --allow-empty -m "PTY-1: first"
git -C "$repo" commit -q --allow-empty -m "PTY-2: second"

export MANAGE_TAG_REPO="$repo"

rc=0
out="$("$MANAGE" tag-build 0.1.5.7 2>&1)" || rc=$?
[ "$rc" -eq 0 ] || fail "tag-build with an unreachable origin exited $rc: $out"
[ "$(git -C "$repo" cat-file -t v0.1.5.7 2>&1)" = tag ] || fail "v0.1.5.7 is not an annotated tag"
[ "$(git -C "$repo" rev-parse 'v0.1.5.7^{commit}')" = "$(git -C "$repo" rev-parse HEAD)" ] \
    || fail "the tag is not on HEAD"
git -C "$repo" tag -l -n1 v0.1.5.7 | grep -q 'PTY-1 PTY-2' || fail "message lacks the ticket keys"
case "$out" in *"not pushed"*) ;; *) fail "output does not say not pushed: $out" ;; esac

rc=0
"$MANAGE" tag-build 0.1.5.7 >/dev/null || rc=$?
[ "$rc" -eq 0 ] || fail "the same tag at the same commit again exited $rc"

git -C "$repo" commit -q --allow-empty -m "PTY-3: third"
rc=0
"$MANAGE" tag-build 0.1.5.7 >/dev/null 2>&1 || rc=$?
[ "$rc" -eq 1 ] || fail "a tag at another commit exited $rc, expected 1"

rc=0
"$MANAGE" tag-build >/dev/null 2>&1 || rc=$?
[ "$rc" -eq 2 ] || fail "no version exited $rc, expected 2"
rc=0
"$MANAGE" tag-build banana >/dev/null 2>&1 || rc=$?
[ "$rc" -eq 2 ] || fail "a non-version exited $rc, expected 2"

"$MANAGE" help 2>&1 | grep -q '^  tag-build VERSION' || fail "help does not list tag-build"

# package (and everything but tag-build) has no tag creation or push in it.
others="$(awk '/^do_tag_build\(\) \{/{skip=1} !skip{print} skip && /^}/{skip=0}' "$MANAGE" \
          | grep -vE '^[[:space:]]*#' | grep -nE 'git .*(push|tag -a)|MANAGE_NO_TAG' || [ $? -eq 1 ])"
[ -z "$others" ] || fail "manage.sh outside tag-build tags or pushes: $others"

if [ "$fails" -ne 0 ]; then
    echo "check-tag-build: $fails failure(s)" >&2
    exit 1
fi
echo "check-tag-build: ok"
