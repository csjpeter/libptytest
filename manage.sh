#!/usr/bin/env bash
# Maintenance front end of libptytest.
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
BUILD_DIR_COVERAGE="$BUILD_DIR/_coverage"
BUILD_DIR_VALGRIND="$BUILD_DIR/_valgrind"
BUILD_DIR_CONSUMER="$BUILD_DIR/_consumer"

# Line coverage of the library sources below this fails the run.
COVERAGE_THRESHOLD=90

JOBS="$(nproc)"

usage() {
    cat <<'USAGE'
Usage: ./manage.sh <command> [args]

Commands:
  deps                  Install build and test dependencies (apt or dnf; needs root)
  build                 Release build (build/)
  debug                 Debug build with ASAN + UBSan (build/_debug)
  test [ctest args]     Unit tests, ASAN + UBSan -- the command run most often
  pty [ctest args]      PTY tests: real programs through a real PTY, ASAN + UBSan
  coverage              Line coverage of the library; fails below the threshold
  valgrind              Unit and PTY tests under Valgrind on a clean Release build
  consumer              Build and run the example against the built .deb packages
  check                 Every gate, with a summary to compare against CI
  package [deb|rpm|all] Build the packages with CPack (libptytest0, libptytest-dev)
  clean                 Remove build/
  help                  This message

One suite, or one test of a suite:
  ./manage.sh test -R test-screen
  build/_test/tests/test-screen test_utf8      (substring filter; -l lists)
USAGE
}

# ---------------------------------------------------------------------------
# Dependencies
# ---------------------------------------------------------------------------

APT_DEPS="build-essential cmake pkg-config git valgrind lcov lintian dpkg-dev
          rpm file locales"
DNF_DEPS="gcc make cmake pkgconf-pkg-config git valgrind lcov rpm-build
          dpkg glibc-langpack-en"

install_deps() {
    if command -v apt-get >/dev/null; then
        # shellcheck disable=SC2086
        apt-get update && apt-get install -y $APT_DEPS
    elif command -v dnf >/dev/null; then
        # shellcheck disable=SC2086
        dnf install -y $DNF_DEPS
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

build_release() {
    cmake -S "$SCRIPT_DIR" -B "$BUILD_DIR" -DCMAKE_BUILD_TYPE=Release \
          -DCMAKE_INSTALL_PREFIX=/usr \
          -DENABLE_COVERAGE=OFF -DPTYTEST_ENABLE_ASAN=OFF \
          -DPTYTEST_BUILD_TESTS=OFF -DPTYTEST_TEST_WRAPPER=
    cmake --build "$BUILD_DIR" --parallel "$JOBS"
    echo "Release build done: $BUILD_DIR"
}

build_debug_into() {
    local dir="$1"
    cmake -S "$SCRIPT_DIR" -B "$dir" -DCMAKE_BUILD_TYPE=Debug \
          -DENABLE_COVERAGE=OFF -DPTYTEST_ENABLE_ASAN=ON \
          -DPTYTEST_BUILD_TESTS=ON -DPTYTEST_TEST_WRAPPER=
    cmake --build "$dir" --parallel "$JOBS"
}

build_debug() {
    build_debug_into "$BUILD_DIR_DEBUG"
    echo "Debug build done: $BUILD_DIR_DEBUG"
}

# ---------------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------------

# run_label <label> [ctest args...]
run_label() {
    local label="$1"
    shift
    build_debug_into "$BUILD_DIR_TEST"
    # halt_on_error: a sanitiser finding fails the suite instead of being
    # printed and forgotten.
    ASAN_OPTIONS="halt_on_error=1:detect_leaks=1" \
    UBSAN_OPTIONS="halt_on_error=1:print_stacktrace=1" \
        ctest --test-dir "$BUILD_DIR_TEST" -L "^${label}\$" \
              --output-on-failure --no-tests=error "$@"
}

run_unit() { run_label unit "$@"; }
run_pty()  { run_label pty "$@"; }

run_valgrind() {
    if ! command -v valgrind >/dev/null; then
        echo "valgrind is not installed; install it with: ./manage.sh deps" >&2
        return 1
    fi
    # A clean Release build: sanitiser and coverage instrumentation both
    # interfere with Valgrind's own.  Children of the tests (the fixture,
    # cat, sh) are not traced: the library is what is under examination.
    cmake -S "$SCRIPT_DIR" -B "$BUILD_DIR_VALGRIND" -DCMAKE_BUILD_TYPE=Release \
          -DENABLE_COVERAGE=OFF -DPTYTEST_ENABLE_ASAN=OFF -DPTYTEST_BUILD_TESTS=ON \
          "-DPTYTEST_TEST_WRAPPER=valgrind;--error-exitcode=1;--leak-check=full;--errors-for-leak-kinds=definite;--track-fds=no;--quiet"
    cmake --build "$BUILD_DIR_VALGRIND" --parallel "$JOBS"
    ctest --test-dir "$BUILD_DIR_VALGRIND" --output-on-failure --no-tests=error
}

run_coverage() {
    if ! command -v lcov >/dev/null; then
        echo "lcov is not installed; install it with: ./manage.sh deps" >&2
        return 1
    fi
    cmake -S "$SCRIPT_DIR" -B "$BUILD_DIR_COVERAGE" -DCMAKE_BUILD_TYPE=Debug \
          -DENABLE_COVERAGE=ON -DPTYTEST_ENABLE_ASAN=OFF -DPTYTEST_BUILD_TESTS=ON \
          -DPTYTEST_TEST_WRAPPER=
    cmake --build "$BUILD_DIR_COVERAGE" --parallel "$JOBS"
    lcov --zerocounters --directory "$BUILD_DIR_COVERAGE"
    # The suites' verdict counts: a failing suite fails the coverage run.
    ctest --test-dir "$BUILD_DIR_COVERAGE" --output-on-failure --no-tests=error

    local info="$BUILD_DIR_COVERAGE/coverage.info"
    lcov --capture --directory "$BUILD_DIR_COVERAGE" --output-file "$info.all"
    # Product code only, one list for every report: the library sources.
    lcov --extract "$info.all" "$SCRIPT_DIR/src/*" --output-file "$info"
    lcov --list "$info"
    if command -v genhtml >/dev/null; then
        genhtml --quiet "$info" --output-directory "$BUILD_DIR_COVERAGE/html"
        echo "HTML report: $BUILD_DIR_COVERAGE/html/index.html"
    fi

    local pct
    pct="$(lcov --summary "$info" | awk '/lines\.*:/ { sub("%", "", $2); print $2 }')"
    if [ -z "$pct" ]; then
        echo "coverage: could not read the line rate from lcov --summary" >&2
        return 1
    fi
    echo "Line coverage of src/: ${pct}% (threshold ${COVERAGE_THRESHOLD}%)"
    if ! awk -v p="$pct" -v t="$COVERAGE_THRESHOLD" 'BEGIN { exit !(p >= t) }'; then
        echo "coverage: ${pct}% is below the threshold of ${COVERAGE_THRESHOLD}%" >&2
        return 1
    fi
}

# ---------------------------------------------------------------------------
# Packaging
#
# CPack builds the packages from the metadata in CMakeLists.txt.  Nothing
# about a package is decided here: no version, no dependency list, no file
# layout.  See policies/packaging-and-release-model.md.
# ---------------------------------------------------------------------------

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

    build_release

    # Packages of an earlier build would be mistaken for this one's.
    find "$BUILD_DIR" -maxdepth 1 -type f \( -name '*.deb' -o -name '*.rpm' \) -delete

    local g
    for g in $generators; do
        # umask 022 here, not left to the caller: CPack copies the umask of
        # whoever runs it into the directory modes of the package.  See
        # policies/packaging-and-release-model.md, "The umask leak".
        ( umask 022; cd "$BUILD_DIR" && cpack -G "$g" --config CPackConfig.cmake )
    done

    echo
    echo "Packages in $BUILD_DIR:"
    find "$BUILD_DIR" -maxdepth 1 -type f \( -name '*.deb' -o -name '*.rpm' \) | sort
}

run_lintian() {
    local deb rc=0 found=0
    for deb in "$BUILD_DIR"/*.deb; do
        [ -f "$deb" ] || continue
        found=1
        echo "--- $(basename "$deb")"
        # The control fields must parse before anything else is worth checking.
        dpkg-deb -f "$deb" Package Version Depends XB-Git-Commit XB-Build-Date
        lintian --fail-on error,warning --info "$deb" || rc=1
    done
    if [ "$found" -eq 0 ]; then
        echo "lintian: no .deb in $BUILD_DIR" >&2
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
    for deb in "$BUILD_DIR"/*.deb; do
        [ -f "$deb" ] || continue
        found=1
        dpkg-deb -x "$deb" "$root"
    done
    if [ "$found" -eq 0 ]; then
        echo "consumer: no .deb in $BUILD_DIR; run ./manage.sh package first" >&2
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
       "$SCRIPT_DIR/examples/test_tiny_menu.c" $libs
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

    local gates="unit pty valgrind coverage package lintian consumer"
    local -A result
    local g rc failed=0
    for g in $gates; do
        printf '%-10s ' "$g"
        rc=0
        case "$g" in
            unit)     run_unit       > "$logdir/$g.log" 2>&1 || rc=$? ;;
            pty)      run_pty        > "$logdir/$g.log" 2>&1 || rc=$? ;;
            valgrind) run_valgrind   > "$logdir/$g.log" 2>&1 || rc=$? ;;
            coverage) run_coverage   > "$logdir/$g.log" 2>&1 || rc=$? ;;
            package)  do_package all > "$logdir/$g.log" 2>&1 || rc=$? ;;
            lintian)  run_lintian    > "$logdir/$g.log" 2>&1 || rc=$? ;;
            consumer) run_consumer   > "$logdir/$g.log" 2>&1 || rc=$? ;;
        esac
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
    for g in $gates; do printf '%-10s %s\n' "$g" "${result[$g]}"; done
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

cmd="${1:-help}"
[ $# -gt 0 ] && shift
case "$cmd" in
    deps)      install_deps ;;
    build)     build_release ;;
    debug)     build_debug ;;
    test)      run_unit "$@" ;;
    pty)       run_pty "$@" ;;
    coverage)  run_coverage ;;
    valgrind)  run_valgrind ;;
    consumer)  run_consumer ;;
    check)     run_check ;;
    package)   do_package "${1:-deb}" ;;
    clean)     clean ;;
    help|-h|--help) usage ;;
    *)
        echo "unknown command: $cmd" >&2
        usage >&2
        exit 1
        ;;
esac
