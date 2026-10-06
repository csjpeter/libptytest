#!/usr/bin/env bash
# Proves the packaging output rule on a copy of the working tree:
#
#   1. a successful "package deb" leaves in build/packages/ exactly the
#      packages of that run -- planted stale files are gone, and every
#      package carries the same build stamp;
#   2. a package that fails lintian makes "package deb" exit non-zero and
#      leaves build/packages/ empty;
#   3. an exact dependency on a package of another repository is refused
#      the same way, while -dev's exact dependency on its own library is
#      allowed, and the library's shlibs asks for a minimum version;
#   4. a build that fails does the same.
#
# The copy lives in build/_pkgtest/ and is removed afterwards.
# See policies/packaging-and-release-model.md, "Where the output goes".
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
[ -n "$ROOT" ] && [ -d "$ROOT/tools" ] || { echo "unsafe root '$ROOT'" >&2; exit 1; }
COPY="$ROOT/build/_pkgtest/src"
LOGS="$ROOT/build/_pkgtest/logs"
FAILS=0

remove_copy() {
    if [ -d "$ROOT/build/_pkgtest" ]; then
        ( cd "$ROOT/build" && rm -rf _pkgtest )
    fi
}

fail() {
    echo "FAIL: $*"
    FAILS=$((FAILS + 1))
}

# The packages directory's entries, one per line.
listing() {
    find "$COPY/build/packages" -mindepth 1 -printf '%f\n' | sort
}

# run_package <step>: runs "package deb" in the copy; output to a log file,
# printed when the outcome is not the expected one (see the callers).
run_package() {
    local rc=0
    ( cd "$COPY" && ./manage.sh package deb ) > "$LOGS/$1.log" 2>&1 || rc=$?
    echo "$rc"
}

show_log() {
    echo "----- $LOGS/$1.log (last 40 lines) -----"
    tail -n 40 "$LOGS/$1.log"
}

remove_copy
mkdir -p "$COPY" "$LOGS"
# The working tree as it is, committed or not, without build output.
( cd "$ROOT" && git ls-files -z --cached --others --exclude-standard ) |
    ( cd "$ROOT" && tar --null -T - -cf - ) | ( cd "$COPY" && tar -xf - )

# 1. success, over stale content
mkdir -p "$COPY/build/packages"
touch "$COPY/build/packages/stale_0.0.1_amd64.deb" "$COPY/build/packages/notes.txt"
mkdir -p "$COPY/build/packages/olddir"
rc=$(run_package success)
if [ "$rc" -ne 0 ]; then
    fail "package deb exited $rc on a clean tree"
    show_log success
else
    mapfile -t files < <(listing)
    echo "after success: ${files[*]}"
    if [ "${#files[@]}" -ne 2 ]; then
        fail "expected exactly 2 entries, found ${#files[@]}: ${files[*]}"
    fi
    stamps=()
    for f in "${files[@]}"; do
        case "$f" in
            libptytest0_*.deb|libptytest-dev_*.deb) ;;
            *) fail "unexpected entry '$f'" ;;
        esac
        # name_version_STAMP_commit_arch.deb: the stamp identifies the run.
        stamps+=("$(echo "$f" | awk -F_ '{print $3}')")
    done
    if [ "${#stamps[@]}" -eq 2 ] && [ "${stamps[0]}" != "${stamps[1]}" ]; then
        fail "packages of two runs: stamps ${stamps[0]} and ${stamps[1]}"
    fi
    # The library's shlibs, which every consumer inherits, asks for at
    # least this version (policies/packaging-and-release-model.md,
    # "Dependencies only ever look upward").
    rt="$(find "$COPY/build/packages" -name 'libptytest0_*.deb' | head -n 1)"
    if [ -n "$rt" ]; then
        sh="$(dpkg-deb -I "$rt" shlibs)"
        echo "shlibs: $sh"
        case "$sh" in
            *"libptytest0 (>= "*) ;;
            *) fail "shlibs does not ask for a minimum version: $sh" ;;
        esac
    fi
    if ! grep -q 'allowed, same build: libptytest-dev_.* Depends: libptytest0 (= ' "$LOGS/success.log"; then
        fail "the exact dependency of -dev on its own library was not recognised as allowed"
    fi
fi

# 2. lintian fails: without the overrides the XB- fields are warnings
: > "$COPY/debian/lintian-overrides"
rc=$(run_package lintian)
if [ "$rc" -eq 0 ]; then
    fail "package deb exited 0 although lintian must fail"
    show_log lintian
elif ! grep -q 'package: lintian failed' "$LOGS/lintian.log"; then
    fail "exit $rc, but not because of lintian"
    show_log lintian
fi
if [ -n "$(listing)" ]; then
    fail "build/packages/ not empty after the lintian failure: $(listing | tr '\n' ' ')"
fi
( cd "$ROOT" && cat debian/lintian-overrides ) > "$COPY/debian/lintian-overrides"

# 3. an exact dependency on another repository's package is refused
cp "$COPY/CMakeLists.txt" "$LOGS/CMakeLists.txt.orig"
printf '\nset(CPACK_DEBIAN_RUNTIME_PACKAGE_DEPENDS "libc6 (= 2.41)")\n' >> "$COPY/CMakeLists.txt"
sed -i 's/^include(CPack)$//' "$COPY/CMakeLists.txt"
printf 'include(CPack)\n' >> "$COPY/CMakeLists.txt"
rc=$(run_package depends)
if [ "$rc" -eq 0 ]; then
    fail "package deb exited 0 with an exact dependency on libc6"
    show_log depends
elif ! grep -q 'FORBIDDEN: libptytest0_.* Depends: libc6 (= 2.41)' "$LOGS/depends.log" ||
     ! grep -q 'package: the dependency check failed' "$LOGS/depends.log"; then
    fail "exit $rc, but not because of the dependency check"
    show_log depends
fi
if [ -n "$(listing)" ]; then
    fail "build/packages/ not empty after the dependency failure: $(listing | tr '\n' ' ')"
fi
cp "$LOGS/CMakeLists.txt.orig" "$COPY/CMakeLists.txt"

# 4. the build fails
printf '\n#error deliberately broken by check-package-output.sh\n' >> "$COPY/src/pty_screen.c"
rc=$(run_package build)
if [ "$rc" -eq 0 ]; then
    fail "package deb exited 0 although the build must fail"
    show_log build
elif ! grep -q 'package: the build failed' "$LOGS/build.log"; then
    fail "exit $rc, but not because of the build"
    show_log build
fi
if [ -n "$(listing)" ]; then
    fail "build/packages/ not empty after the build failure: $(listing | tr '\n' ' ')"
fi

if [ "$FAILS" -ne 0 ]; then
    echo "$FAILS check(s) failed; the copy is kept in $ROOT/build/_pkgtest for inspection"
    exit 1
fi
remove_copy
echo "package output rule: 4 scenarios passed"
