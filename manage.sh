#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-only
# Copyright (c) 2026 Peter Csaszar (Császár Péter)
# Created: 2026-10-03
# Maintenance front end of libptytest: build, test, coverage, package and check verbs.
#
# Every verb delegates to the real tool -- CMake, CTest, CPack, Valgrind,
# lcov -- and decides nothing their own configuration should decide.  The
# vocabulary is the workspace's: see policies/manage-sh-model.md.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

# Every build tree lives under build/, which "clean" removes as a whole and
# which is never committed.  build/ itself is the Release tree, so packages
# land in build/<package>_<version>_..._<arch>.deb as the packaging policy
# asks.
BUILD_DIR="$SCRIPT_DIR/build"
BUILD_DIR_DEBUG="$BUILD_DIR/_debug"
BUILD_DIR_TEST="$BUILD_DIR/_test"
# The instrumented trees of the coverage verb, one per scope, with their raw
# counters; the integrator reads them (policies/manage-sh-model.md).
BUILD_DIR_COVERAGE="$BUILD_DIR/coverage"
BUILD_DIR_VALGRIND="$BUILD_DIR/_valgrind"
BUILD_DIR_CONSUMER="$BUILD_DIR/_consumer"
PACKAGE_DIR="$BUILD_DIR/packages"

# build/result/<verb>.result, written by test, functional, pty, valgrind and
# coverage (policies/manage-sh-model.md, "The integrator reads the output").
# The override exists so that tools/check-result.sh can test the runner
# without touching the real record.
RESULT_DIR="${MANAGE_RESULT_DIR:-$BUILD_DIR/result}"

# A suite that runs longer than this is a timeout (seconds).
SUITE_TIMEOUT="${MANAGE_SUITE_TIMEOUT:-120}"

# One line per run of a verb: "<UTC time> <verb> <seconds>s <ok|fail>".  The
# override exists so that tools/check-timing.sh can test the record without
# touching the real one (policies/manage-sh-model.md, "Every verb times itself").
TIMING_LOG="${MANAGE_TIMING_LOG:-$BUILD_DIR/timing.log}"

# Line coverage of the library sources (src/) below these fails the run.
# The policy's floor is 80% for the unit tests alone and 80% for the
# externally driven tests together; a project may set them higher, never
# lower (policies/testing-model.md, "Coverage").  The figure for all tests
# together is this project's own gate, at the policy's target.
COVERAGE_UNIT_THRESHOLD=80
COVERAGE_EXTERNAL_THRESHOLD=80
COVERAGE_THRESHOLD=90

JOBS="$(nproc)"

usage() {
    cat <<'USAGE'
Usage: ./manage.sh <command> [args]

Commands:
  deps                  Install build and test dependencies (apt or dnf; needs root);
                        includes test-base
  test-base             Create or refresh the test base containers in
                        /var/lib/lxc-bases (this project has none: says so, exit 0)
  build                 Release build (build/)
  debug                 Debug build with ASAN + UBSan (build/_debug)
  test [-R REGEX]       Unit tests, ASAN + UBSan -- the command run most often
  pty [-R REGEX]        PTY tests: real programs through a real PTY, ASAN + UBSan
  coverage              Line coverage of src/: unit alone, functional+pty tests
                        alone, and all together; fails if any is below its
                        threshold; leaves the counters in build/coverage/{unit,external}
  valgrind              Every unit suite under Valgrind on a clean Release build
  consumer              Build and run the example against the built .deb packages
  test-bins             Self-test tools/check-test-bins.sh, then build and check
                        that declared tests and build/tests/*/bin/ agree
  licence               Self-test tools/check-licence.sh, then check that every
                        file and declaration says GPL-3.0-only
  functional [-R REGEX] Functional tests (tests/functional): the library through its
                        shared interface, a real child and a real PTY, ASAN + UBSan
  functional-coverage   Self-test tools/check-functional-coverage.sh, then fail if an
                        exported function or macro has no row in the map
                        docs/functional-coverage.md
  private-names         Self-test tools/check-private-names.sh, then fail if a
                        tracked file names the owner's private domain
  check                 Every gate, with a summary to compare against CI; includes
                        tools/check-package-output.sh, which proves the rule above
  package [deb|rpm|all] Build the packages into build/packages/ (emptied first;
                        left empty, exit non-zero, if the build, lintian or the
                        upward-only dependency check fails)
  test-base-check       Self-test tools/check-test-base.sh: test-base works alone,
                        is idempotent, is listed in help and run by deps
  version               Self-test tools/check-version.sh: the package version is
                        project_version.<commit count>, a tag takes no part
  tag-build VERSION     The annotated tag vVERSION on HEAD (message: the ticket
                        keys since the previous tag); never pushes (the host
                        pushes it with gh-push); the same tag at the same
                        commit again is fine.  package never tags or pushes
  tag-build-test        Self-test tools/check-tag-build.sh: tag-build tags and
                        pushes nothing, and package has no tag or push in it
  result-format         Self-test tools/check-result.sh (the suite runner and the
                        result file), then check build/result/*.result and the
                        coverage counters left by the verbs run before
  timing-log            Self-test tools/check-timing.sh: the timing record and
                        the gate line, including "not measured"
  timing [KEY-N]        Print the gate line from build/timing.log; with a key,
                        also comment it on that ticket with issue-cli
  clean                 Remove build/
  help                  This message

Tests are declared in tests/CMakeLists.txt and built into
build/tests/<category>/bin/<name> (here: build/_test/tests/... for test/pty).
test, functional, pty, valgrind and coverage write build/result/<verb>.result.
One suite, or one test of a suite:
  ./manage.sh test -R test_screen
  build/_test/tests/unit/bin/test_screen test_utf8   (substring filter; -l lists)
USAGE
}

# ---------------------------------------------------------------------------
# Dependencies
# ---------------------------------------------------------------------------

APT_DEPS="build-essential cmake pkg-config git valgrind lcov lintian dpkg-dev
          rpm file locales"
DNF_DEPS="gcc make cmake pkgconf-pkg-config git valgrind lcov rpm-build
          dpkg glibc-langpack-en"

# test-base: the project's test base containers in /var/lib/lxc-bases
# (policies/agent-container-model.md, section 4a).  libptytest tests run a
# real child on a real PTY on the host and need no container, so it has no
# base; the verb says so and succeeds.  deps includes it.
run_test_base() {
    echo "test-base: libptytest has no test base containers (nothing in /var/lib/lxc-bases to create or refresh)"
}

install_deps() {
    if command -v apt-get >/dev/null; then
        # shellcheck disable=SC2086
        apt-get update && apt-get install -y $APT_DEPS && run_test_base
    elif command -v dnf >/dev/null; then
        # shellcheck disable=SC2086
        dnf install -y $DNF_DEPS && run_test_base
    else
        echo "deps: no apt-get or dnf; install by hand:" >&2
        echo "  a C11 compiler, cmake >= 3.20, pkg-config, valgrind, lcov," >&2
        echo "  lintian, dpkg-deb, rpmbuild" >&2
        return 1
    fi
}

# ---------------------------------------------------------------------------
# Builds
#
# -DENABLE_COVERAGE is passed explicitly on every configure: coverage
# instrumentation is sticky in a CMake cache and would otherwise leak into
# release binaries and packages (policies/testing-model.md).
# ---------------------------------------------------------------------------

# licence: the checker must be able to fail (its self-test), then the tree.
run_licence() {
    "$SCRIPT_DIR/tools/check-licence.sh" --self-test
    "$SCRIPT_DIR/tools/check-licence.sh"
}

# private-names: the checker must be able to fail (its self-test), then the
# tree (policies/private-names-model.md).
run_private_names() {
    "$SCRIPT_DIR/tools/check-private-names.sh" --self-test
    "$SCRIPT_DIR/tools/check-private-names.sh"
}

# test-bins: the checker must be able to fail (its self-test), then the
# Release tree is built and checked.
run_test_bins() {
    "$SCRIPT_DIR/tools/check-test-bins.sh" --self-test
    build_release
}

# check_test_bins <build-dir>: the declared tests and the files in
# <build-dir>/tests/<category>/bin/ are the same set.
check_test_bins() {
    "$SCRIPT_DIR/tools/check-test-bins.sh" "$1"
}

# The Release build includes the tests: build/tests/<category>/bin/ is where
# the integrator finds and runs them (policies/testing-model.md).  They are
# not installed, so the packages do not change.
build_release() {
    cmake -S "$SCRIPT_DIR" -B "$BUILD_DIR" -DCMAKE_BUILD_TYPE=Release \
          -DCMAKE_INSTALL_PREFIX=/usr \
          -DENABLE_COVERAGE=OFF -DPTYTEST_ENABLE_ASAN=OFF \
          -DPTYTEST_BUILD_TESTS=ON -DPTYTEST_TEST_WRAPPER= &&
    cmake --build "$BUILD_DIR" --parallel "$JOBS" &&
    check_test_bins "$BUILD_DIR" &&
    echo "Release build done: $BUILD_DIR"
}

build_debug_into() {
    local dir="$1"
    cmake -S "$SCRIPT_DIR" -B "$dir" -DCMAKE_BUILD_TYPE=Debug \
          -DENABLE_COVERAGE=OFF -DPTYTEST_ENABLE_ASAN=ON \
          -DPTYTEST_BUILD_TESTS=ON -DPTYTEST_TEST_WRAPPER= &&
    cmake --build "$dir" --parallel "$JOBS" &&
    check_test_bins "$dir"
}

build_debug() {
    build_debug_into "$BUILD_DIR_DEBUG"
    echo "Debug build done: $BUILD_DIR_DEBUG"
}

# ---------------------------------------------------------------------------
# The result file and the suite runner
#
# A verb that runs suites calls result_begin first.  From then on every exit
# path (success, failure, a failed build, a signal) writes the final
# build/result/<verb>.result from the state the runner collected
# (policies/manage-sh-model.md, "The result file").
# ---------------------------------------------------------------------------

R_VERB=""
R_START=0
R_STARTED=""
R_SUITES=()
R_SCOPES=()
R_ASSERT=0
R_FAILED=0
R_BAD=0

result_commit() {
    local c
    c="$(git -C "$SCRIPT_DIR" rev-parse --short HEAD)" || c=unknown
    if [ -n "$(git -C "$SCRIPT_DIR" status --porcelain)" ]; then
        c="$c+"
    fi
    echo "$c"
}

# result_write <status> <exit code>: the file, whole, through a rename.
result_write() {
    local file="$RESULT_DIR/$R_VERB.result" tmp
    tmp="$file.$$"
    {
        echo "format: manage-result/1"
        echo "verb: $R_VERB"
        echo "commit: $(result_commit)"
        echo "started: $R_STARTED"
        echo "seconds: $(( $(date +%s) - R_START ))"
        echo "exit: $2"
        echo "status: $1"
        echo "suites: ${#R_SUITES[@]}"
        echo "assertions: $R_ASSERT"
        echo "failed: $R_FAILED"
        local i
        for ((i = 0; i < ${#R_SCOPES[@]}; i++)); do echo "${R_SCOPES[$i]}"; done
        for ((i = 0; i < ${#R_SUITES[@]}; i++)); do echo "${R_SUITES[$i]}"; done
    } > "$tmp" && mv "$tmp" "$file"
}

# The EXIT trap: the final file, on every way out.
result_end() {
    local rc=$?
    trap - EXIT
    local status=pass
    if [ "$rc" -ne 0 ] || [ "$R_BAD" -ne 0 ]; then
        status=fail
        [ "${#R_SUITES[@]}" -gt 0 ] || status=error
    elif [ "${#R_SUITES[@]}" -eq 0 ]; then
        # Exit 0 with no suite run: the tests do not exist.
        status=error
        rc=1
    fi
    result_write "$status" "$rc" || echo "result: cannot write $RESULT_DIR/$R_VERB.result" >&2
    exit "$rc"
}

# result_begin <verb>: delete the old file, write "running", arm the trap.
result_begin() {
    R_VERB="$1"
    R_START="$(date +%s)"
    R_STARTED="$(date -u +%Y-%m-%dT%H:%M:%SZ)"
    R_SUITES=()
    R_SCOPES=()
    R_ASSERT=0
    R_FAILED=0
    R_BAD=0
    mkdir -p "$RESULT_DIR"
    rm -f "$RESULT_DIR/$R_VERB.result"
    trap result_end EXIT
    trap 'exit 143' TERM
    trap 'exit 130' INT
    result_write running 0
}

# run_suites <name prefix> <build dir> <categories> [wrapper, ; separated]
#
# Runs every executable in <build dir>/tests/<category>/bin/ with no
# arguments, under a timeout, each to its end: a failed suite does not stop
# the next, it makes the return status non-zero.  A suite's assertion counts
# are read from the runner's own summary line.  No skip: exit 77, a missing
# count and a missing suite directory are failures.
run_suites() {
    local prefix="$1" dir="$2" cats="$3" rc=0
    local -a wrapper=()
    if [ -n "${4:-}" ]; then
        IFS=';' read -r -a wrapper <<< "$4"
    fi
    local cat bin name log t0 code status counts a f
    mkdir -p "$RESULT_DIR/$R_VERB.logs"
    for cat in $cats; do
        for bin in "$dir/tests/$cat/bin/"*; do
            [ -f "$bin" ] && [ -x "$bin" ] || continue
            name="$(basename "$bin")"
            if [ -n "${SUITE_FILTER:-}" ] && ! [[ "$name" =~ $SUITE_FILTER ]]; then
                continue
            fi
            log="$RESULT_DIR/$R_VERB.logs/${prefix//\//-}$name.log"
            t0="$(date +%s)"
            code=0
            echo "=== suite $prefix$name"
            ( cd "$dir/tests" && exec timeout "$SUITE_TIMEOUT" \
                  ${wrapper[@]+"${wrapper[@]}"} "$bin" ) > "$log" 2>&1 || code=$?
            cat "$log"
            counts="$(awk '
                /^Assertions run: *[0-9]+/ { sub(/^Assertions run: */, ""); a = $1 + 0; have = 1 }
                /^Assertions failed: *[0-9]+/ { sub(/^Assertions failed: */, ""); f = $1 + 0 }
                /^[0-9]+ assertions, [0-9]+ failed/ { a = $1 + 0; f = $3 + 0; have = 1 }
                END { if (have) print a, f + 0 }' "$log")"
            a=0
            f=0
            if [ -n "$counts" ]; then
                a="${counts% *}"
                f="${counts#* }"
            fi
            if [ "$code" -eq 124 ]; then
                status=timeout
                a=0
                f=0
            elif [ "$code" -ge 128 ] || { [ "$code" -ne 0 ] && [ -z "$counts" ]; }; then
                status=crash
                a=0
                f=0
            elif [ "$code" -eq 0 ] && [ -n "$counts" ] && [ "$a" -ge 1 ] && [ "$f" -eq 0 ]; then
                status=pass
            else
                status=fail
                [ "$f" -ge 1 ] || f=1
                if [ "$code" -eq 77 ]; then
                    echo "suite $name: exit 77 (skip) is a failure here" >&2
                elif [ "$code" -eq 0 ]; then
                    echo "suite $name: exit 0 but no usable assertion count" >&2
                fi
            fi
            R_SUITES+=("suite: $prefix$name $status $a $f $(( $(date +%s) - t0 ))")
            R_ASSERT=$((R_ASSERT + a))
            R_FAILED=$((R_FAILED + f))
            if [ "$status" != pass ]; then
                R_BAD=$((R_BAD + 1))
                rc=1
                echo "suite $prefix$name: $status (exit $code)" >&2
            fi
        done
    done
    return "$rc"
}

# ---------------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------------

# run_label <verb> <ctest label = category> [-R regex]
run_label() {
    local verb="$1" label="$2"
    shift 2
    SUITE_FILTER=""
    case "${1:-}" in
        "") ;;
        -R) [ $# -eq 2 ] || { echo "$verb: -R takes one regex" >&2; return 2; }
            SUITE_FILTER="$2" ;;
        *)  echo "$verb: expected nothing or -R REGEX, got '$*'" >&2; return 2 ;;
    esac
    result_begin "$verb"
    build_debug_into "$BUILD_DIR_TEST" || return 1
    # Evidence of an earlier run would be taken for this run's.
    if [ -d "$SCRIPT_DIR/build/_test/evidence" ]; then
        ( cd "$SCRIPT_DIR/build/_test" && rm -rf evidence )
    fi
    # halt_on_error: a sanitiser finding fails the suite instead of being
    # printed and forgotten.  A failed screen assertion leaves its evidence
    # (report, screen, screen with colours) under the build tree.  The
    # status is kept, not discarded, and returned after the hint.
    local rc=0
    export PTYTEST_EVIDENCE_DIR="$BUILD_DIR_TEST/evidence"
    export ASAN_OPTIONS="halt_on_error=1:detect_leaks=1"
    export UBSAN_OPTIONS="halt_on_error=1:print_stacktrace=1"
    run_suites "" "$BUILD_DIR_TEST" "$label" || rc=$?
    if [ -d "$BUILD_DIR_TEST/evidence" ]; then
        echo "Evidence of failed assertions: $BUILD_DIR_TEST/evidence"
    fi
    return "$rc"
}

run_unit() { run_label test unit "$@"; }
run_pty()  { run_label pty pty "$@"; }
run_functional() { run_label functional functional "$@"; }

# functional-coverage: the checker must be able to fail (its self-test), then
# the map (policies/testing-model.md, "Every capability has a functional test").
run_functional_coverage() {
    "$SCRIPT_DIR/tools/check-functional-coverage.sh" --self-test
    "$SCRIPT_DIR/tools/check-functional-coverage.sh"
}

# result-format: the runner and the result file must be able to fail (the
# self-test), then the files the verbs left.
run_result_format() {
    "$SCRIPT_DIR/tools/check-result.sh" --self-test
    "$SCRIPT_DIR/tools/check-result.sh"
}

run_valgrind() {
    result_begin valgrind
    if ! command -v valgrind >/dev/null; then
        echo "valgrind is not installed; install it with: ./manage.sh deps" >&2
        return 1
    fi
    # A clean Release build: sanitiser and coverage instrumentation both
    # interfere with Valgrind's own.  Children of the tests (the fixture,
    # cat, sh) are not traced: the library is what is under examination.
    # Every unit suite, and only the unit suites (a kind with its own verb
    # is not run here).
    cmake -S "$SCRIPT_DIR" -B "$BUILD_DIR_VALGRIND" -DCMAKE_BUILD_TYPE=Release \
          -DENABLE_COVERAGE=OFF -DPTYTEST_ENABLE_ASAN=OFF -DPTYTEST_BUILD_TESTS=ON \
          -DPTYTEST_TEST_WRAPPER= || return 1
    cmake --build "$BUILD_DIR_VALGRIND" --parallel "$JOBS" || return 1
    check_test_bins "$BUILD_DIR_VALGRIND" || return 1
    SUITE_FILTER=""
    run_suites "" "$BUILD_DIR_VALGRIND" unit \
        "valgrind;--error-exitcode=1;--leak-check=full;--errors-for-leak-kinds=definite;--track-fds=no;--quiet"
}

# coverage_scope <scope> <categories>: an instrumented build tree of its own
# under build/coverage/<scope>/, counters zeroed, only those suites run on
# it, then <scope>.info, product code only.
coverage_scope() {
    local scope="$1" cats="$2" dir="$BUILD_DIR_COVERAGE/$1" rc=0
    local info="$BUILD_DIR_COVERAGE/coverage-$1.info" sum
    cmake -S "$SCRIPT_DIR" -B "$dir" -DCMAKE_BUILD_TYPE=Debug \
          -DENABLE_COVERAGE=ON -DPTYTEST_ENABLE_ASAN=OFF -DPTYTEST_BUILD_TESTS=ON \
          -DPTYTEST_TEST_WRAPPER= || return 1
    cmake --build "$dir" --parallel "$JOBS" || return 1
    check_test_bins "$dir" || return 1
    lcov --zerocounters --directory "$dir" || return 1
    # The tests' verdict counts: a failing suite fails the coverage run, but
    # the other suites and the capture still happen.
    SUITE_FILTER=""
    run_suites "$scope/" "$dir" "$cats" || rc=1
    lcov --capture --directory "$dir" --output-file "$info.all" || return 1
    # Product code only, one list for every report: the library sources.
    lcov --extract "$info.all" "$SCRIPT_DIR/src/*" --output-file "$info" || return 1
    sum="$(lcov --summary "$info" | sed -n 's/.*lines\.*: *[0-9.]*% (\([0-9]*\) of \([0-9]*\) lines).*/\1 \2/p')"
    if [ -z "$sum" ]; then
        echo "coverage: could not read the line counts of $info" >&2
        return 1
    fi
    R_SCOPES+=("scope: $scope build/coverage/$scope ${sum% *} ${sum#* }")
    return "$rc"
}

# line_rate <info>: the line coverage percentage lcov reports.
line_rate() {
    local pct
    pct="$(lcov --summary "$1" | awk '/lines\.*:/ { sub("%", "", $2); print $2 }')"
    if [ -z "$pct" ]; then
        echo "coverage: could not read the line rate of $1" >&2
        return 1
    fi
    echo "$pct"
}

# coverage: two gated figures, as policies/testing-model.md "Coverage" asks,
# each on product code (src/) only --
#   - the unit tests alone;
#   - the tests that drive the program from outside (functional, pty,
#     integration) together;
# and the two together, held to this project's own higher threshold.  The
# instrumented trees and their counters stay in build/coverage/unit and
# build/coverage/external for the integrator.
run_coverage() {
    result_begin coverage
    if ! command -v lcov >/dev/null; then
        echo "lcov is not installed; install it with: ./manage.sh deps" >&2
        return 1
    fi
    local dir="$BUILD_DIR_COVERAGE" failed=0
    mkdir -p "$dir"
    coverage_scope unit unit || failed=1
    coverage_scope external "functional pty integration" || failed=1
    [ -f "$dir/coverage-unit.info" ] && [ -f "$dir/coverage-external.info" ] || return 1
    lcov --add-tracefile "$dir/coverage-unit.info" \
         --add-tracefile "$dir/coverage-external.info" \
         --output-file "$dir/coverage-all.info" || return 1

    local f
    for f in unit external all; do
        echo "=== $f ==="
        lcov --list "$dir/coverage-$f.info"
    done
    if command -v genhtml >/dev/null; then
        genhtml --quiet "$dir/coverage-all.info" --output-directory "$dir/html"
        echo "HTML report: $dir/html/index.html"
    fi

    local unit ext all
    unit="$(line_rate "$dir/coverage-unit.info")"
    ext="$(line_rate "$dir/coverage-external.info")"
    all="$(line_rate "$dir/coverage-all.info")"
    report_rate() {  # report_rate <label> <pct> <threshold>
        echo "Line coverage of src/, $1: $2% (threshold $3%)"
        if ! awk -v p="$2" -v t="$3" 'BEGIN { exit !(p >= t) }'; then
            echo "coverage: $1 at $2% is below the threshold of $3%" >&2
            failed=1
        fi
    }
    report_rate "unit tests alone" "$unit" "$COVERAGE_UNIT_THRESHOLD"
    report_rate "pty tests (functional+pty+integration)" "$ext" "$COVERAGE_EXTERNAL_THRESHOLD"
    report_rate "all tests" "$all" "$COVERAGE_THRESHOLD"
    return "$failed"
}

# ---------------------------------------------------------------------------
# Packaging
#
# CPack builds the packages from the metadata in CMakeLists.txt.  Nothing
# about a package is decided here: no version, no dependency list, no file
# layout.  See policies/packaging-and-release-model.md.
# ---------------------------------------------------------------------------

# Removes everything in build/packages/ and recreates it empty.  Literal
# names only: the recursive delete never takes a variable.
empty_packages_dir() {
    mkdir -p "$SCRIPT_DIR/build" || return 1
    ( cd "$SCRIPT_DIR/build" && rm -rf packages && mkdir packages )
}

# package <deb|rpm|all>
#
# build/packages/ holds exactly the packages of this run: it is emptied
# first, CPack writes into it (CPACK_OUTPUT_FILE_PREFIX), and every .deb has
# to pass lintian.  On any failure the directory is left empty and the exit
# status is non-zero, so whoever collects it never takes a partial or stale
# set.  See policies/packaging-and-release-model.md, "Where the output goes".
#
# Every step is checked explicitly rather than through "set -e": a caller
# such as run_check invokes this as "do_package ... || rc=$?", and bash
# turns errexit off inside a function called that way.
do_package() {
    local kind="${1:-deb}" generators
    case "$kind" in
        deb) generators="DEB" ;;
        rpm) generators="RPM" ;;
        all) generators="DEB RPM" ;;
        *)
            echo "package: expected deb, rpm or all, got '$kind'" >&2
            return 1
            ;;
    esac

    if ! empty_packages_dir; then
        echo "package: cannot empty $PACKAGE_DIR" >&2
        return 1
    fi

    if ! build_release; then
        package_failed "the build"
        return 1
    fi

    local g
    for g in $generators; do
        # umask 022 here, not left to the caller: CPack copies the umask of
        # whoever runs it into the directory modes of the package.  See
        # policies/packaging-and-release-model.md, "The umask leak".
        if ! ( umask 022; cd "$BUILD_DIR" && cpack -G "$g" --config CPackConfig.cmake ); then
            package_failed "cpack -G $g"
            return 1
        fi
    done

    # A package that is not lintian-clean is not a result, nor is one that
    # forbids the upgrade of what it depends on.
    if [ "$kind" != rpm ] && ! run_lintian; then
        package_failed "lintian"
        return 1
    fi
    if [ "$kind" != rpm ] && ! check_depends_upward; then
        package_failed "the dependency check"
        return 1
    fi

    echo
    echo "Packages in $PACKAGE_DIR:"
    find "$PACKAGE_DIR" -mindepth 1 | sort
}

# tag-build VERSION
#
# The tag follows the package version and never decides it: vVERSION is the
# exact Debian version the build carries (build/version.txt), on the commit
# that was built.  The message lists the ticket keys named in the commits
# since the previous tag.  A tag that exists at HEAD is left as it is; one at
# another commit is an error, not moved.  It never pushes: the host pushes the
# tag with gh-push (policies/manage-sh-model.md).  MANAGE_TAG_REPO names
# another repository (tools/check-tag-build.sh).
do_tag_build() {
    local version="${1:-}" repo="${MANAGE_TAG_REPO:-$SCRIPT_DIR}" tag head prev range keys
    if [ -z "$version" ] || [ $# -gt 1 ]; then
        echo "usage: ./manage.sh tag-build VERSION" >&2
        return 2
    fi
    case "$version" in
        [0-9]*.[0-9]*) ;;
        *) echo "tag-build: '$version' is not a version" >&2; return 2 ;;
    esac
    tag="v$version"
    head="$(git -C "$repo" rev-parse HEAD)" || return 1
    if git -C "$repo" rev-parse -q --verify "refs/tags/$tag" >/dev/null; then
        if [ "$(git -C "$repo" rev-parse "$tag^{commit}")" != "$head" ]; then
            echo "tag-build: $tag exists at another commit than HEAD $head" >&2
            return 1
        fi
        echo "tag-build: $tag already at HEAD"
        return 0
    fi
    prev="$(git -C "$repo" tag --list 'v*' --merged HEAD --sort=-v:refname | head -n 1)"
    range="HEAD"
    [ -z "$prev" ] || range="$prev..HEAD"
    keys="$(git -C "$repo" log --format=%B "$range" \
            | { grep -oE '[A-Z]+-[0-9]+' || [ $? -eq 1 ]; } | sort -u | tr '\n' ' ')"
    git -C "$repo" tag -a "$tag" -m "libptytest $version: ${keys:-no ticket keys}" || return 1
    echo "tag-build: $tag made at $head (not pushed)"
}

package_failed() {
    echo "package: $1 failed; $PACKAGE_DIR is left empty" >&2
    if ! empty_packages_dir; then
        echo "package: and $PACKAGE_DIR could not be emptied" >&2
    fi
}

# Every Depends, Pre-Depends, Recommends and Suggests of the built .debs
# allows newer versions: no "(=", "(<<" or "(<=" -- except on a package this
# same build produced (libptytest-dev on libptytest0), which is upgraded
# together with it.  The shlibs file is held to the same rule, since every
# consumer inherits it.  See policies/packaging-and-release-model.md,
# "Dependencies only ever look upward".
check_depends_upward() {
    local deb own="" rc=0 field rel pkg
    for deb in "$PACKAGE_DIR"/*.deb; do
        [ -f "$deb" ] || continue
        own="$own $(dpkg-deb -f "$deb" Package)"
    done
    for deb in "$PACKAGE_DIR"/*.deb; do
        [ -f "$deb" ] || continue
        for field in Depends Pre-Depends Recommends Suggests; do
            # One relation per line: "name (op version)", alternatives split.
            while IFS= read -r rel; do
                [ -n "$rel" ] || continue
                case "$rel" in
                    *"(="*|*"(<<"*|*"(<="*) ;;
                    *) continue ;;
                esac
                pkg="${rel%% *}"
                case " $own " in
                    *" $pkg "*) echo "allowed, same build: $(basename "$deb") $field: $rel" ;;
                    *) echo "FORBIDDEN: $(basename "$deb") $field: $rel" >&2; rc=1 ;;
                esac
            done < <(dpkg-deb -f "$deb" "$field" | tr ',|' '\n\n' | sed 's/^ *//; s/ *$//')
        done
        local members shlibs
        members="$(dpkg-deb --ctrl-tarfile "$deb" | tar -tf -)" || return 1
        if grep -qx './shlibs' <<<"$members"; then
            shlibs="$(dpkg-deb -I "$deb" shlibs)" || return 1
            echo "shlibs of $(basename "$deb"): $shlibs"
            case "$shlibs" in
                *"(="*|*"(<<"*|*"(<="*)
                    echo "FORBIDDEN: shlibs of $(basename "$deb") pins its consumers: $shlibs" >&2
                    rc=1 ;;
            esac
        fi
    done
    return "$rc"
}

run_lintian() {
    local deb rc=0 found=0
    for deb in "$PACKAGE_DIR"/*.deb; do
        [ -f "$deb" ] || continue
        found=1
        echo "--- $(basename "$deb")"
        # The control fields must parse before anything else is worth checking.
        dpkg-deb -f "$deb" Package Version Depends XB-Git-Commit XB-Build-Date || rc=1
        lintian --fail-on error,warning --info "$deb" || rc=1
    done
    if [ "$found" -eq 0 ]; then
        echo "lintian: no .deb in $PACKAGE_DIR" >&2
        return 1
    fi
    return "$rc"
}

# consumer -- the example, built and run against the content of the .deb
# packages: headers, pkg-config file, CMake package and shared library as a
# consuming project gets them.
run_consumer() {
    local root="$BUILD_DIR_CONSUMER/root" deb found=0
    # A fresh tree, so nothing from an earlier package can be picked up.
    if [ -d "$SCRIPT_DIR/build/_consumer" ]; then
        ( cd "$SCRIPT_DIR/build" && rm -rf _consumer )
    fi
    mkdir -p "$root"
    for deb in "$PACKAGE_DIR"/*.deb; do
        [ -f "$deb" ] || continue
        found=1
        dpkg-deb -x "$deb" "$root"
    done
    if [ "$found" -eq 0 ]; then
        echo "consumer: no .deb in $PACKAGE_DIR; run ./manage.sh package first" >&2
        return 1
    fi

    local libdir
    libdir="$(dirname "$(find "$root" -name 'libptytest.so.*' -type f | head -n 1)")"
    local pc_dir="$libdir/pkgconfig"

    echo "=== pkg-config ==="
    local cflags libs
    cflags="$(PKG_CONFIG_SYSROOT_DIR="$root" PKG_CONFIG_LIBDIR="$pc_dir" \
              pkg-config --cflags libptytest)"
    libs="$(PKG_CONFIG_SYSROOT_DIR="$root" PKG_CONFIG_LIBDIR="$pc_dir" \
            pkg-config --libs libptytest)"
    echo "cflags: $cflags"
    echo "libs:   $libs"
    # shellcheck disable=SC2086
    cc -std=c11 -Wall -Wextra -Werror $cflags -o "$BUILD_DIR_CONSUMER/test-pc" \
       "$SCRIPT_DIR/tests/pty/test_tiny_menu.c" $libs
    cc -std=c11 -Wall -Wextra -Werror -o "$BUILD_DIR_CONSUMER/tiny-menu" \
       "$SCRIPT_DIR/examples/tiny_menu.c"
    LD_LIBRARY_PATH="$libdir" timeout 60 \
        "$BUILD_DIR_CONSUMER/test-pc" "$BUILD_DIR_CONSUMER/tiny-menu"

    echo "=== CMake find_package ==="
    cmake -S "$SCRIPT_DIR/examples/consumer" -B "$BUILD_DIR_CONSUMER/cmake" \
          -DCMAKE_PREFIX_PATH="$root/usr"
    cmake --build "$BUILD_DIR_CONSUMER/cmake" --parallel "$JOBS"
    LD_LIBRARY_PATH="$libdir" timeout 60 \
        "$BUILD_DIR_CONSUMER/cmake/test-tiny-menu" "$BUILD_DIR_CONSUMER/cmake/tiny-menu"
}

# ---------------------------------------------------------------------------
# check -- every gate, each with its log, and a summary.
# ---------------------------------------------------------------------------

run_check() {
    mkdir -p "$BUILD_DIR"
    local logdir="$BUILD_DIR/_logs"
    mkdir -p "$logdir"

    local gates="licence private-names functional-coverage test-bins version tag-build-test timing-log test-base-check unit pty functional valgrind coverage result-format package consumer package-output"
    local -A result
    local g rc failed=0
    for g in $gates; do
        printf '%-15s ' "$g"
        rc=0
        # Each gate is a manage.sh process of its own.  Called as a function
        # under "|| rc=$?", a gate would run with errexit switched off (bash
        # ignores "set -e" inside anything whose status is tested), and a
        # failing step in its middle would be forgotten if a later one
        # succeeded.  A separate process keeps "set -e" in force.
        case "$g" in
            package)        "$SCRIPT_DIR/manage.sh" package all ;;
            package-output) timeout 900 "$SCRIPT_DIR/tools/check-package-output.sh" ;;
            unit)           "$SCRIPT_DIR/manage.sh" test ;;
            *)              "$SCRIPT_DIR/manage.sh" "$g" ;;
        esac > "$logdir/$g.log" 2>&1 || rc=$?
        if [ "$rc" -eq 0 ]; then
            result[$g]=PASS
            echo "PASS"
        else
            result[$g]="FAIL (exit $rc, see $logdir/$g.log)"
            echo "FAIL -- last lines of $logdir/$g.log:"
            tail -n 25 "$logdir/$g.log" | sed 's/^/    /'
            failed=$((failed + 1))
        fi
    done

    echo
    echo "=== summary ==="
    for g in $gates; do printf '%-15s %s\n' "$g" "${result[$g]}"; done
    if ! grep -h -E '^(Assertions run|Line coverage)' "$logdir/unit.log" \
             "$logdir/pty.log" "$logdir/coverage.log"; then
        echo "  (no assertion counts or coverage figure found in the logs)"
    fi
    if [ "$failed" -ne 0 ]; then
        echo "$failed gate(s) failed."
        return 1
    fi
    echo "All gates passed."
}

clean() {
    # Enter the directory, then delete a literal name: a recursive delete
    # never takes a bare variable.  The guard matters: "cd ''" succeeds and
    # changes nothing.
    [ -n "$SCRIPT_DIR" ] && [ -d "$SCRIPT_DIR" ] || {
        echo "clean: SCRIPT_DIR is not a directory: '$SCRIPT_DIR'" >&2
        return 1
    }
    ( cd "$SCRIPT_DIR" && rm -rf build )
    echo "Cleaned."
}

# ---------------------------------------------------------------------------
# Timing: every verb that does work appends its wall-clock seconds to the
# record; "timing" prints the gate line from it.  Nothing is estimated.
# ---------------------------------------------------------------------------

# gate_record: prints the log lines a gate line is made from: the latest
# successful run of each of check, package and install (install-deb).  This
# is the measured record; two reports are the same when their records are.
gate_record() {
    [ -s "$TIMING_LOG" ] || return 0
    awk '
        $4 == "ok" && ($2 == "check" || $2 == "package" || $2 == "install" || $2 == "install-deb") {
            v = ($2 == "install-deb") ? "install" : $2
            last[v] = $0
        }
        END {
            split("check package install", order, " ")
            for (i = 1; i <= 3; i++) if (order[i] in last) print last[order[i]]
        }' "$TIMING_LOG"
}

# gate_line: prints "Gate: <total>s (check <s>s, package <s>s, install <s>s)"
# from the record; a part with no record is left out.  With nothing recorded:
# "Gate: not measured", exit status 1.
gate_line() {
    local line=""
    line="$(gate_record | awk '
        {
            v = ($2 == "install-deb") ? "install" : $2
            secs[v] = $3; sub("s$", "", secs[v])
        }
        END {
            n = 0; total = 0; parts = ""
            split("check package install", order, " ")
            for (i = 1; i <= 3; i++) {
                v = order[i]
                if (v in secs) {
                    parts = parts (n ? ", " : "") v " " secs[v] "s"
                    total += secs[v]; n++
                }
            }
            if (n) printf "Gate: %ds (%s)\n", total, parts
        }')"
    if [ -z "$line" ]; then
        echo "Gate: not measured"
        return 1
    fi
    echo "$line"
}

# run_timing [KEY-N]: prints the gate line; with a key posts it as a comment
# unless the measured record is the one last posted for that key (kept in
# timing.posted next to the log).  A re-run verb or newer code gives a new
# record, so its line is posted even when the text is identical.
run_timing() {
    local line rc=0 record sum posted tmp
    line="$(gate_line)" || rc=$?
    echo "$line"
    if [ -n "${1:-}" ]; then
        record="$(gate_record)"
        sum="$(printf '%s\n' "$record" | cksum | awk '{print $1 "-" $2}')"
        posted="$(dirname "$TIMING_LOG")/timing.posted"
        if [ -n "$record" ] && [ -f "$posted" ] \
           && grep -qxF "$1 $sum" "$posted"; then
            echo "timing: record already posted for $1; nothing posted"
            return "$rc"
        fi
        # A key with nothing measured still gets the honest line.
        "${ISSUE_CLI:-issue-cli}" issue comment "$1" "$line" || return 1
        if [ -n "$record" ]; then
            mkdir -p "$(dirname "$posted")"
            tmp="$posted.$$"
            { [ ! -f "$posted" ] || grep -v "^$1 " "$posted" || [ $? -eq 1 ]; 
              printf '%s %s\n' "$1" "$sum"; } > "$tmp"
            mv "$tmp" "$posted"
        fi
    fi
    return "$rc"
}

# tools/check-result.sh sources this file to test the runner.
[ -z "${MANAGE_SOURCE_ONLY:-}" ] || return 0

cmd="${1:-help}"
[ $# -gt 0 ] && shift

# Verbs that do work time themselves: the verb runs as a child manage.sh so
# that its own "set -e" stays in force, and its start, end and status are
# read here.  MANAGE_INNER marks that child for one level only; the verbs it
# starts (check runs its gates this way) are timed in turn.
case "$cmd" in
    deps|test-base|build|debug|test|pty|functional|functional-coverage|coverage|valgrind|consumer|\
    licence|private-names|test-bins|version|tag-build-test|timing-log|test-base-check|result-format|check|package)
        if [ -z "${MANAGE_INNER:-}" ]; then
            t_start="$(date +%s)"
            t_rc=0
            MANAGE_INNER=1 "$SCRIPT_DIR/manage.sh" "$cmd" "$@" || t_rc=$?
            t_end="$(date +%s)"
            t_state=ok
            [ "$t_rc" -eq 0 ] || t_state=fail
            mkdir -p "$(dirname "$TIMING_LOG")"
            t_commit="$(git -C "$SCRIPT_DIR" rev-parse --short HEAD || echo unknown)"
            if [ -n "$(git -C "$SCRIPT_DIR" status --porcelain)" ]; then
                t_commit="$t_commit+"
            fi
            printf '%s %s %ss %s %s\n' "$(date -u +%Y-%m-%dT%H:%M:%SZ)" "$cmd" \
                   "$((t_end - t_start))" "$t_state" "$t_commit" >> "$TIMING_LOG"
            exit "$t_rc"
        fi
        unset MANAGE_INNER
        ;;
esac

case "$cmd" in
    deps)      install_deps ;;
    test-base) run_test_base ;;
    test-base-check) "$SCRIPT_DIR/tools/check-test-base.sh" ;;
    build)     build_release ;;
    debug)     build_debug ;;
    test)      run_unit "$@" ;;
    pty)       run_pty "$@" ;;
    functional) run_functional "$@" ;;
    functional-coverage) run_functional_coverage ;;
    coverage)  run_coverage ;;
    valgrind)  run_valgrind ;;
    consumer)  run_consumer ;;
    licence) run_licence ;;
    private-names) run_private_names ;;
    test-bins) run_test_bins ;;
    check)     run_check ;;
    timing)    run_timing "$@" ;;
    version)   "$SCRIPT_DIR/tools/check-version.sh" ;;
    tag-build) do_tag_build "$@" ;;
    tag-build-test) "$SCRIPT_DIR/tools/check-tag-build.sh" ;;
    timing-log) "$SCRIPT_DIR/tools/check-timing.sh" ;;
    result-format) run_result_format ;;
    package)   do_package "${1:-deb}" ;;
    clean)     clean ;;
    help|-h|--help) usage ;;
    *)
        echo "unknown command: $cmd" >&2
        usage >&2
        exit 2
        ;;
esac
