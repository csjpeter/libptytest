#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Péter Császár <peter@csaszar.email>
#
# Checks that the project says one thing about its licence everywhere:
#
#   - every tracked file except the exempt ones carries
#     "SPDX-License-Identifier: GPL-3.0-or-later" within its first 5 lines;
#   - LICENSE is the GNU GPL version 3 text;
#   - the Debian copyright file in CMakeLists.txt says GPL-3+ and the RPM
#     licence field says GPL-3.0-or-later;
#   - README.md names GPL-3.0-or-later.
#
# Exempt: LICENSE (the licence text itself), .gitignore, and Markdown
# documents (their licence is stated in README.md).
#
# See policies/studies/licensing-survey.md: LICENSE, debian/copyright and the
# package's licence field must all say the same thing.
#
# Usage: check-licence.sh [<project-dir>]
#        check-licence.sh --self-test
set -euo pipefail

ID="SPDX-License-Identifier: GPL-3.0-or-later"

# check <dir>: prints the problems, returns their count.
check() {
    local dir="$1" problems=0 f
    local -a files

    if git -C "$dir" rev-parse --is-inside-work-tree > /dev/null; then
        mapfile -t files < <(git -C "$dir" ls-files)
    else
        echo "not a git work tree: $dir"
        return 1
    fi

    for f in "${files[@]}"; do
        case "$f" in
            LICENSE|.gitignore|*.md) continue ;;
        esac
        [ -f "$dir/$f" ] || continue
        if ! head -n 5 "$dir/$f" | grep -qF "$ID"; then
            echo "no \"$ID\" in the first 5 lines: $f"
            problems=$((problems + 1))
        fi
    done

    if ! grep -qF "Version 3, 29 June 2007" "$dir/LICENSE" 2> /dev/null \
        || ! grep -qF "GNU GENERAL PUBLIC LICENSE" "$dir/LICENSE"; then
        echo "LICENSE is not the GNU GPL version 3 text"
        problems=$((problems + 1))
    fi
    if ! grep -q '^License: GPL-3+$' "$dir/CMakeLists.txt" 2> /dev/null; then
        echo "CMakeLists.txt: the Debian copyright file does not say \"License: GPL-3+\""
        problems=$((problems + 1))
    fi
    if ! grep -q 'CPACK_RPM_PACKAGE_LICENSE "GPL-3.0-or-later"' "$dir/CMakeLists.txt" 2> /dev/null; then
        echo "CMakeLists.txt: CPACK_RPM_PACKAGE_LICENSE is not \"GPL-3.0-or-later\""
        problems=$((problems + 1))
    fi
    if ! grep -qF "GPL-3.0-or-later" "$dir/README.md" 2> /dev/null; then
        echo "README.md does not name GPL-3.0-or-later"
        problems=$((problems + 1))
    fi
    return "$problems"
}

good_cmake() {
    printf '# %s\nLicense: GPL-3+\nset(CPACK_RPM_PACKAGE_LICENSE "GPL-3.0-or-later")\n' "$ID"
}

self_test() {
    tmp="$(mktemp -d "/tmp/check-licence-$$-XXXXXX")"
    case "$tmp" in /tmp/check-licence-*) ;; *) echo "ERROR: unsafe path: '$tmp'" >&2; exit 1 ;; esac
    trap 'rm -f "$tmp/LICENSE" "$tmp/CMakeLists.txt" "$tmp/README.md" "$tmp/a.c"; rm -rf "$tmp/.git"; rmdir "$tmp"' EXIT

    git -C "$tmp" init -q
    printf 'GNU GENERAL PUBLIC LICENSE\nVersion 3, 29 June 2007\n' > "$tmp/LICENSE"
    good_cmake > "$tmp/CMakeLists.txt"
    printf 'GPL-3.0-or-later\n' > "$tmp/README.md"
    printf '/* %s */\nint main(void) { return 0; }\n' "$ID" > "$tmp/a.c"
    git -C "$tmp" add -A

    local rc=0
    check "$tmp" > /dev/null || rc=$?
    [ "$rc" -eq 0 ] || { echo "self-test: a correct tree was rejected"; return 1; }

    # Each of these must be found, or the check cannot fail.
    printf 'int main(void) { return 0; }\n' > "$tmp/a.c"
    rc=0; check "$tmp" > /dev/null || rc=$?
    [ "$rc" -ne 0 ] || { echo "self-test: a file without the identifier passed"; return 1; }
    printf '/* %s */\n' "$ID" > "$tmp/a.c"

    good_cmake | sed 's/GPL-3+/MIT/' > "$tmp/CMakeLists.txt"
    rc=0; check "$tmp" > /dev/null || rc=$?
    [ "$rc" -ne 0 ] || { echo "self-test: a wrong Debian licence passed"; return 1; }
    good_cmake | sed 's/"GPL-3.0-or-later"/"MIT"/' > "$tmp/CMakeLists.txt"
    rc=0; check "$tmp" > /dev/null || rc=$?
    [ "$rc" -ne 0 ] || { echo "self-test: a wrong RPM licence passed"; return 1; }
    good_cmake > "$tmp/CMakeLists.txt"

    printf 'MIT\n' > "$tmp/LICENSE"
    rc=0; check "$tmp" > /dev/null || rc=$?
    [ "$rc" -ne 0 ] || { echo "self-test: a wrong LICENSE passed"; return 1; }
    echo "check-licence self-test: ok"
}

if [ "${1:-}" = "--self-test" ]; then
    self_test
    exit $?
fi

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
rc=0
check "${1:-$ROOT}" || rc=$?
if [ "$rc" -eq 0 ]; then
    echo "licence: every file and declaration says $ID"
else
    echo "licence: $rc problem(s)"
fi
exit "$rc"
