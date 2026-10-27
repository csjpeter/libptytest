#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-only
# Copyright (c) 2026 Peter Csaszar (Császár Péter)
# Created: 2026-10-27
# Tests manage.sh's suite runner and the format of build/result/<verb>.result.
#
# policies/manage-sh-model.md, "The integrator reads the output": test,
# functional, pty, valgrind and coverage write one result file in the format
# manage-result/1.  Without arguments this script checks the files the verbs
# left (they must exist, parse and agree with their exit code) and the
# counters coverage left in build/coverage/{unit,external}.  With --self-test
# it first proves that the runner and the validator can fail.
#
# Usage: check-result.sh [--self-test]
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
fails=0

fail() { echo "check-result: FAIL: $1" >&2; fails=$((fails + 1)); }

# validate <file> <verb>: the integrator's own checks, in shell.  Prints the
# first problem and returns non-zero.
validate() {
    local file="$1" verb="$2" line key value n=0 nsuite=0 sa=0 sf=0 bad=0 scopes=0
    local -A kv=()
    local -a order=(format verb commit started seconds exit status suites assertions failed)
    local -a suites=()
    problem() { echo "$file: $1" >&2; return 1; }
    [ -f "$file" ] || { problem "missing"; return 1; }
    while IFS= read -r line || [ -n "$line" ]; do
        case "$line" in
            *": "*) ;;
            *) problem "line $((n + 1)) is not 'key: value': '$line'"; return 1 ;;
        esac
        key="${line%%: *}"
        value="${line#*: }"
        if [ "$n" -lt 10 ]; then
            [ "$key" = "${order[$n]}" ] || { problem "line $((n + 1)): expected '${order[$n]}', got '$key'"; return 1; }
            kv[$key]="$value"
        elif [ "$key" = suite ]; then
            suites+=("$value")
        elif [ "$key" = scope ]; then
            scopes=$((scopes + 1))
        fi
        n=$((n + 1))
    done < "$file"
    [ "$n" -ge 10 ] || { problem "fewer than the 10 header lines"; return 1; }
    [ "${kv[format]}" = "manage-result/1" ] || { problem "format is '${kv[format]}'"; return 1; }
    [ "${kv[verb]}" = "$verb" ] || { problem "verb is '${kv[verb]}', expected '$verb'"; return 1; }
    [[ "${kv[commit]}" =~ ^[0-9a-f]+\+?$ ]] || { problem "commit '${kv[commit]}'"; return 1; }
    [[ "${kv[started]}" =~ ^[0-9]{4}-[0-9]{2}-[0-9]{2}T[0-9:]{8}Z$ ]] || { problem "started '${kv[started]}'"; return 1; }
    for key in seconds exit suites assertions failed; do
        [[ "${kv[$key]}" =~ ^[0-9]+$ ]] || { problem "$key '${kv[$key]}' is not a number"; return 1; }
    done
    case "${kv[status]}" in
        pass|fail|error) ;;
        running) problem "status is still running"; return 1 ;;
        *) problem "status '${kv[status]}'"; return 1 ;;
    esac
    if [ "${kv[exit]}" -eq 0 ] && [ "${kv[status]}" != pass ]; then
        problem "exit 0 with status ${kv[status]}"; return 1
    fi
    if [ "${kv[exit]}" -ne 0 ] && [ "${kv[status]}" = pass ]; then
        problem "exit ${kv[exit]} with status pass"; return 1
    fi
    [ "${#suites[@]}" -eq "${kv[suites]}" ] || { problem "suites is ${kv[suites]}, ${#suites[@]} suite lines"; return 1; }
    local -A seen=()
    local s name st a f t
    for s in ${suites[@]+"${suites[@]}"}; do
        read -r name st a f t extra <<< "$s"
        [ -z "${extra:-}" ] && [ -n "${t:-}" ] || { problem "bad suite line '$s'"; return 1; }
        [[ "$a$f$t" =~ ^[0-9]+$ ]] || { problem "bad numbers in '$s'"; return 1; }
        [ -z "${seen[$name]:-}" ] || { problem "suite $name twice"; return 1; }
        seen[$name]=1
        case "$st" in
            pass) [ "$a" -ge 1 ] || { problem "pass suite $name with no assertions"; return 1; } ;;
            fail|crash|timeout) bad=$((bad + 1)) ;;
            *) problem "suite status '$st'"; return 1 ;;
        esac
        sa=$((sa + a)); sf=$((sf + f)); nsuite=$((nsuite + 1))
    done
    [ "$sa" -eq "${kv[assertions]}" ] || { problem "assertions ${kv[assertions]}, suites sum $sa"; return 1; }
    [ "$sf" -eq "${kv[failed]}" ] || { problem "failed ${kv[failed]}, suites sum $sf"; return 1; }
    if [ "${kv[status]}" = pass ]; then
        [ "$bad" -eq 0 ] && [ "${kv[failed]}" -eq 0 ] || { problem "pass with a failed suite"; return 1; }
        [ "$nsuite" -ge 1 ] && [ "${kv[assertions]}" -ge 1 ] || { problem "pass with no tests"; return 1; }
    fi
    if [ "$verb" = coverage ] && [ "${kv[status]}" = pass ] && [ "$scopes" -lt 1 ]; then
        problem "coverage pass without a scope line"; return 1
    fi
    return 0
}

# --- the files the verbs left ------------------------------------------------

check_files() {
    local v dir="${MANAGE_RESULT_DIR:-$ROOT/build/result}"
    for v in test functional pty valgrind coverage; do
        validate "$dir/$v.result" "$v" || fail "$v.result is not a valid result"
    done
    local scope
    for scope in unit external; do
        if [ ! -d "$ROOT/build/coverage/$scope" ]; then
            fail "build/coverage/$scope does not exist"
        elif [ -z "$(find "$ROOT/build/coverage/$scope" -name '*.gcno' -print -quit)" ] \
             || [ -z "$(find "$ROOT/build/coverage/$scope" -name '*.gcda' -print -quit)" ]; then
            fail "build/coverage/$scope holds no .gcno/.gcda counters"
        fi
    done
}

# --- the self-test -----------------------------------------------------------

self_test() {
    local tmp
    tmp="$(mktemp -d "/tmp/check-result-$$-XXXXXX")"
    case "$tmp" in /tmp/check-result-*) ;; *) echo "ERROR: unsafe path: '$tmp'" >&2; exit 1 ;; esac
    # shellcheck disable=SC2064
    trap "cleanup_tmp '$tmp'" EXIT

    # A good file passes the validator; every defect below makes it fail.
    cat > "$tmp/good.result" <<'EOF'
format: manage-result/1
verb: test
commit: abc1234
started: 2026-10-27T10:00:00Z
seconds: 3
exit: 0
status: pass
suites: 2
assertions: 15
failed: 0
suite: one pass 10 0 1
suite: two pass 5 0 2
EOF
    validate "$tmp/good.result" test 2>> "$tmp/validate.err" || fail "validator rejects a good file"
    mutate() {  # mutate <what> <sed expression>
        sed "$2" "$tmp/good.result" > "$tmp/bad.result"
        if validate "$tmp/bad.result" test 2>> "$tmp/validate.err"; then fail "validator accepts: $1"; fi
    }
    mutate "status running" 's/^status: pass/status: running/'
    mutate "exit 0 with status fail" 's/^status: pass/status: fail/'
    mutate "exit 1 with status pass" 's/^exit: 0/exit: 1/'
    mutate "wrong suites count" 's/^suites: 2/suites: 3/'
    mutate "wrong assertion sum" 's/^assertions: 15/assertions: 16/'
    mutate "pass suite with 0 assertions" 's/^suite: two pass 5 0 2/suite: two pass 0 0 2/; s/^assertions: 15/assertions: 10/'
    mutate "a failed suite under status pass" 's/^suite: two pass 5 0 2/suite: two fail 5 1 2/; s/^failed: 0/failed: 1/'
    mutate "wrong format" 's/manage-result\/1/manage-result\/2/'
    mutate "wrong verb" 's/^verb: test/verb: pty/'
    mutate "duplicate suite" 's/^suite: two /suite: one /'
    mutate "suite name with space" 's/^suite: two pass/suite: tw o pass/'
    mutate "missing header key" '/^seconds:/d'
    mutate "not key: value" 's/^seconds: 3/seconds/'
    if validate "$tmp/missing.result" test 2>> "$tmp/validate.err"; then fail "validator accepts a missing file"; fi

    # The runner: fake suites in a fake build tree, run through manage.sh's
    # own functions (sourced, not executed).
    local fake="$tmp/build" bin="$tmp/build/tests/unit/bin"
    mkdir -p "$bin" "$tmp/result"
    cat > "$bin/s_pass" <<'EOF'
#!/bin/sh
# The result file must say "running" while a suite runs, never an old pass.
if grep -q '^status: running' "$RESULT_FILE_UNDER_TEST"; then echo seen > "$RESULT_MARKER"; fi
printf 'Assertions run:   5\nAssertions failed:0\n'
EOF
    cat > "$bin/s_fail" <<'EOF'
#!/bin/sh
printf 'Assertions run:   4\nAssertions failed:2\n'
exit 1
EOF
    cat > "$bin/s_style2" <<'EOF'
#!/bin/sh
echo "7 assertions, 0 failed"
EOF
    cat > "$bin/s_crash" <<'EOF'
#!/bin/sh
kill -SEGV $$
EOF
    cat > "$bin/s_slow" <<'EOF'
#!/bin/sh
sleep 30
EOF
    cat > "$bin/s_skip" <<'EOF'
#!/bin/sh
printf 'Assertions run:   3\nAssertions failed:0\n'
exit 77
EOF
    cat > "$bin/s_nocount" <<'EOF'
#!/bin/sh
echo "all fine"
EOF
    chmod +x "$bin"/s_*

    export MANAGE_SOURCE_ONLY=1 MANAGE_RESULT_DIR="$tmp/result" MANAGE_SUITE_TIMEOUT=2
    export RESULT_FILE_UNDER_TEST="$tmp/result/test.result" RESULT_MARKER="$tmp/marker"
    # A stale pass that a dead run must not leave behind.
    cp "$tmp/good.result" "$tmp/result/test.result"
    local rc=0
    ( # shellcheck disable=SC1091
      source "$ROOT/manage.sh"
      result_begin test
      SUITE_FILTER=""
      run_suites "" "$fake" unit ) > "$tmp/run.out" 2>&1 || rc=$?
    [ "$rc" -ne 0 ] || fail "runner: exit 0 though suites failed"
    [ -f "$tmp/marker" ] || fail "runner: result file was not 'running' while a suite ran"
    local res="$tmp/result/test.result" want
    validate "$res" test 2>> "$tmp/validate.err" || fail "runner: the file written is not valid ($(cat "$tmp/run.out" | tail -n 3))"
    grep -q '^status: fail' "$res" || fail "runner: status is not fail"
    grep -q '^exit: 1$' "$res" || fail "runner: exit is not 1"
    for want in "s_pass pass 5 0" "s_fail fail 4 2" "s_style2 pass 7 0" "s_crash crash 0 0" \
                "s_slow timeout 0 0" "s_skip fail 3 1" "s_nocount fail 0 1"; do
        grep -q "^suite: ${want} [0-9]*$" "$res" || fail "runner: no line 'suite: $want' in $res"
    done
    grep -q '^suites: 7$' "$res" || fail "runner: suites is not 7"
    grep -q '^assertions: 19$' "$res" || fail "runner: assertions is not 19"

    # Only passing suites: pass, exit 0.
    local pass="$tmp/pbuild/tests/unit/bin"
    mkdir -p "$pass"
    cp "$bin/s_pass" "$bin/s_style2" "$pass/"
    rc=0
    ( source "$ROOT/manage.sh"
      result_begin test
      SUITE_FILTER=""
      run_suites "" "$tmp/pbuild" unit ) > "$tmp/run.out" 2>&1 || rc=$?
    [ "$rc" -eq 0 ] || fail "runner: exit $rc for passing suites"
    validate "$res" test 2>> "$tmp/validate.err" && grep -q '^status: pass' "$res" || fail "runner: passing suites give no valid pass file"

    # -R selects suites by regex.
    ( source "$ROOT/manage.sh"
      result_begin test
      SUITE_FILTER="^s_style2$"
      run_suites "" "$tmp/pbuild" unit ) > "$tmp/run.out" 2>&1 || true
    grep -q '^suites: 1$' "$res" || fail "runner: -R did not select one suite"

    # No suite at all (a build that produced nothing): error, never pass.
    rc=0
    ( source "$ROOT/manage.sh"
      result_begin test
      exit 0 ) > "$tmp/run.out" 2>&1 || rc=$?
    [ "$rc" -ne 0 ] || fail "runner: exit 0 with no suite"
    grep -q '^status: error' "$res" || fail "runner: no suite is not status error"

    # A verb that dies before any suite (a failed build): error, its code kept.
    rc=0
    ( source "$ROOT/manage.sh"
      result_begin test
      exit 3 ) > "$tmp/run.out" 2>&1 || rc=$?
    [ "$rc" -eq 3 ] || fail "runner: exit code 3 became $rc"
    grep -q '^status: error' "$res" && grep -q '^exit: 3$' "$res" || fail "runner: failed build is not status error, exit 3"
    unset MANAGE_SOURCE_ONLY MANAGE_RESULT_DIR MANAGE_SUITE_TIMEOUT

    # An unknown verb prints the usage and exits 2; help exits 0.
    rc=0
    "$ROOT/manage.sh" no-such-verb > /dev/null 2> "$tmp/usage.err" || rc=$?
    [ "$rc" -eq 2 ] || fail "unknown verb exits $rc, not 2"
    grep -q '^Usage:' "$tmp/usage.err" || fail "unknown verb: usage is not on stderr"
    "$ROOT/manage.sh" help > /dev/null || fail "help does not exit 0"
}

cleanup_tmp() {
    local d="$1"
    case "$d" in /tmp/check-result-*) ;; *) return 0 ;; esac
    ( cd "$d" && rm -rf build pbuild result )
    rm -f "$d/good.result" "$d/bad.result" "$d/missing.result" "$d/run.out" \
          "$d/usage.err" "$d/marker" "$d/validate.err"
    rmdir "$d"
}

case "${1:-}" in
    --self-test) self_test ;;
    "") check_files ;;
    *) echo "usage: check-result.sh [--self-test]" >&2; exit 2 ;;
esac

if [ "$fails" -ne 0 ]; then
    echo "check-result: $fails check(s) failed" >&2
    exit 1
fi
echo "check-result: ok"
