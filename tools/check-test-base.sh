#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-only
# Copyright (c) 2026 Peter Csaszar (Császár Péter)
# Created: 2026-10-29
# Tests the test-base verb of manage.sh (policies/manage-sh-model.md).
#
# It must work alone, be idempotent, be listed by help, and be run by deps.
# Every case can fail: a missing verb, a second run that differs, a help
# without the verb, a deps that does not call it.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
M="$ROOT/manage.sh"
fails=0
fail() { echo "check-test-base: FAIL: $1" >&2; fails=$((fails + 1)); }

out1="$(MANAGE_INNER=1 "$M" test-base)" || fail "test-base exited non-zero"
out2="$(MANAGE_INNER=1 "$M" test-base)" || fail "second test-base exited non-zero"
[ "$out1" = "$out2" ] || fail "test-base is not idempotent: '$out1' vs '$out2'"
case "$out1" in *"no test base"*) ;; *) fail "test-base does not say the project has none: '$out1'" ;; esac
"$M" help | grep -q '^  test-base ' || fail "help does not list test-base"
# deps needs root and the network: check by its text that it calls the verb.
awk '/^install_deps\(\)/,/^}/' "$M" | grep -q 'run_test_base' \
    || fail "deps does not run test-base"

if [ "$fails" -ne 0 ]; then exit 1; fi
echo "check-test-base: ok"
