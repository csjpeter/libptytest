#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-only OR LicenseRef-Proprietary
# Copyright (c) 2026 Peter Csaszar (Császár Péter)
# Created: 2026-10-11
# Tests manage.sh's timing record and the gate line it prints.
#
# policies/manage-sh-model.md, "Every verb times itself": a verb appends one
# line to the record, and "timing" prints the gate line from it, leaving out
# any part that was not measured.  Every case below can fail.
#
# Usage: check-timing.sh
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
MANAGE="$ROOT/manage.sh"

tmp="$(mktemp -d "/tmp/check-timing-$$-XXXXXX")"
case "$tmp" in /tmp/check-timing-*) ;; *) echo "ERROR: unsafe path: '$tmp'" >&2; exit 1 ;; esac
trap 'rm -f "$tmp/timing.log" "$tmp/issue-cli" "$tmp/args" "$tmp/out"; rmdir "$tmp"' EXIT

export MANAGE_TIMING_LOG="$tmp/timing.log"
fails=0

fail() { echo "check-timing: FAIL: $1" >&2; fails=$((fails + 1)); }

# expect_line <what> <expected>: the gate line printed now.
expect_line() {
    local got rc=0
    got="$("$MANAGE" timing)" || rc=$?
    [ "$got" = "$2" ] || fail "$1: got '$got', expected '$2'"
    echo "$rc"
}

# 1. No record: honest gap, non-zero exit.
rc="$(expect_line "no log" "Gate: not measured")"
[ "$rc" -ne 0 ] || fail "no log: exit status was 0"

# 2. An empty record is the same.
: > "$MANAGE_TIMING_LOG"
rc="$(expect_line "empty log" "Gate: not measured")"
[ "$rc" -ne 0 ] || fail "empty log: exit status was 0"

# 3. Only a verb outside the gate: nothing to report.
printf '2026-10-11T10:00:00Z test 7s ok\n' > "$MANAGE_TIMING_LOG"
rc="$(expect_line "test only" "Gate: not measured")"
[ "$rc" -ne 0 ] || fail "test only: exit status was 0"

# 4. check alone: the other parts are left out, not guessed.
printf '2026-10-11T10:00:00Z check 100s ok\n' > "$MANAGE_TIMING_LOG"
rc="$(expect_line "check only" "Gate: 100s (check 100s)")"
[ "$rc" -eq 0 ] || fail "check only: exit status $rc"

# 5. The latest run of each verb counts; a failed run does not.
cat > "$MANAGE_TIMING_LOG" <<'LOG'
2026-10-11T10:00:00Z check 100s ok
2026-10-11T10:05:00Z check 90s ok
2026-10-11T10:10:00Z package 20s ok
2026-10-11T10:11:00Z package 5s fail
2026-10-11T10:12:00Z install-deb 3s ok
LOG
rc="$(expect_line "latest runs" "Gate: 113s (check 90s, package 20s, install 3s)")"
[ "$rc" -eq 0 ] || fail "latest runs: exit status $rc"

# 6. With a key the very same line is commented through issue-cli.
cat > "$tmp/issue-cli" <<STUB
#!/usr/bin/env bash
printf '%s\n' "\$@" > "$tmp/args"
STUB
chmod +x "$tmp/issue-cli"
ISSUE_CLI="$tmp/issue-cli" "$MANAGE" timing KEY-1 > "$tmp/out"
want="$(printf 'issue\ncomment\nKEY-1\nGate: 113s (check 90s, package 20s, install 3s)')"
[ "$(cat "$tmp/args")" = "$want" ] || fail "key: issue-cli got: $(tr '\n' '|' < "$tmp/args")"

# 7. A verb that succeeds appends an ok line, one that fails a fail line.
: > "$MANAGE_TIMING_LOG"
"$MANAGE" licence > "$tmp/out"
[ "$(wc -l < "$MANAGE_TIMING_LOG")" -eq 1 ] || fail "licence: expected one log line"
grep -Eq '^[0-9]{4}-[0-9]{2}-[0-9]{2}T[0-9:]{8}Z licence [0-9]+s ok$' "$MANAGE_TIMING_LOG" \
    || fail "licence: log line has the wrong shape: $(cat "$MANAGE_TIMING_LOG")"
rc=0
"$MANAGE" package bogus > "$tmp/out" 2>&1 || rc=$?
[ "$rc" -ne 0 ] || fail "package bogus: exit status was 0"
grep -Eq ' package [0-9]+s fail$' "$MANAGE_TIMING_LOG" \
    || fail "package bogus: no fail line: $(cat "$MANAGE_TIMING_LOG")"

if [ "$fails" -ne 0 ]; then
    echo "check-timing: $fails case(s) failed" >&2
    exit 1
fi
echo "check-timing: ok"
