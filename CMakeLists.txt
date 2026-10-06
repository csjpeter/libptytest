cmake_minimum_required(VERSION 3.20)
project(libptytest VERSION 0.1.1 LANGUAGES C)

# ---------------------------------------------------------------------------
# Version
#
# project(... VERSION ...) above is the single source: there is no VERSION
# file and no version written into any packaging metadata.  A build between
# releases is refined with the number of commits since the last tag, so
# successive builds sort correctly for apt.  No "-" in the result: in a
# Debian version the last "-" separates the upstream version from the
# Debian revision.  See policies/packaging-and-release-model.md.
# ---------------------------------------------------------------------------
set(PTYTEST_VERSION "${PROJECT_VERSION}")
set(PTYTEST_GIT_COMMIT "unknown")

find_package(Git QUIET)
if(Git_FOUND AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/.git")
    execute_process(
        COMMAND "${GIT_EXECUTABLE}" rev-parse --short HEAD
        WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
        OUTPUT_VARIABLE _ptytest_sha
        OUTPUT_STRIP_TRAILING_WHITESPACE
        RESULT_VARIABLE _ptytest_sha_rc)
    if(_ptytest_sha_rc EQUAL 0 AND _ptytest_sha)
        set(PTYTEST_GIT_COMMIT "${_ptytest_sha}")
    endif()

    # A repository without a tag yet is a normal state, not an error, so
    # the tag list is asked for rather than "git describe" failing on it.
    execute_process(
        COMMAND "${GIT_EXECUTABLE}" tag --list "v*" --sort=-v:refname --merged HEAD
        WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
        OUTPUT_VARIABLE _ptytest_tags
        OUTPUT_STRIP_TRAILING_WHITESPACE
        RESULT_VARIABLE _ptytest_tags_rc)
    if(_ptytest_tags_rc EQUAL 0 AND _ptytest_tags)
        string(REPLACE "\n" ";" _ptytest_tags "${_ptytest_tags}")
        list(GET _ptytest_tags 0 _ptytest_tag)
        execute_process(
            COMMAND "${GIT_EXECUTABLE}" rev-list --count "${_ptytest_tag}..HEAD"
            WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
            OUTPUT_VARIABLE _ptytest_commits
            OUTPUT_STRIP_TRAILING_WHITESPACE
            RESULT_VARIABLE _ptytest_commits_rc)
        if(_ptytest_commits_rc EQUAL 0 AND _ptytest_commits
           AND NOT _ptytest_commits STREQUAL "0")
            set(PTYTEST_VERSION "${PROJECT_VERSION}.${_ptytest_commits}")
        endif()
    endif()
endif()

string(TIMESTAMP PTYTEST_BUILD_DATE "%Y-%m-%dT%H:%M:%SZ" UTC)
string(TIMESTAMP PTYTEST_BUILD_STAMP "%Y%m%dT%H%M%S" UTC)
message(STATUS "libptytest version ${PTYTEST_VERSION} (commit ${PTYTEST_GIT_COMMIT})")

# ---------------------------------------------------------------------------
# Build options
#
# Used by another project through add_subdirectory(), only the library is
# built; tests, documentation and packaging belong to this project's own
# build.
# ---------------------------------------------------------------------------
if(CMAKE_CURRENT_SOURCE_DIR STREQUAL CMAKE_SOURCE_DIR)
    set(PTYTEST_TOP_LEVEL ON)
else()
    set(PTYTEST_TOP_LEVEL OFF)
endif()

option(PTYTEST_BUILD_TESTS  "Build the unit and PTY tests"          ${PTYTEST_TOP_LEVEL})
option(PTYTEST_ENABLE_ASAN  "Build with AddressSanitizer + UBSan"   OFF)
option(ENABLE_COVERAGE      "Build with gcov instrumentation"       OFF)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND PTYTEST_TOP_LEVEL)
    set(CMAKE_BUILD_TYPE Release)
endif()

include(GNUInstallDirs)

set(PTYTEST_WARNINGS -Wall -Wextra -Wshadow -Wstrict-prototypes -Wformat=2)

# Hardening for what is shipped; lintian checks for it.
set(PTYTEST_HARDENING
    $<$<CONFIG:Release>:-D_FORTIFY_SOURCE=2>
    -fstack-protector-strong)
set(PTYTEST_HARDENING_LINK "LINKER:-z,relro" "LINKER:-z,now")

if(PTYTEST_ENABLE_ASAN AND ENABLE_COVERAGE)
    message(FATAL_ERROR "ASAN and coverage are separate builds: gcov's "
                        "bookkeeping and the sanitiser's do not mix "
                        "(policies/testing-model.md)")
endif()

if(PTYTEST_ENABLE_ASAN)
    set(PTYTEST_SAN_FLAGS -fsanitize=address,undefined
                          -fno-sanitize-recover=undefined
                          -fno-omit-frame-pointer)
    add_compile_options(${PTYTEST_SAN_FLAGS})
    add_link_options(${PTYTEST_SAN_FLAGS})
endif()

if(ENABLE_COVERAGE)
    add_compile_options(--coverage -O0 -g)
    add_link_options(--coverage)
endif()

# ---------------------------------------------------------------------------
# The library: one set of sources, built shared (what the package ships and
# what consumers link) and static (what the unit tests link, since they
# reach the internal emulator interface the shared library does not export).
# ---------------------------------------------------------------------------
set(PTYTEST_SOURCES
    src/pty_compare.c
    src/pty_screen.c
    src/pty_session.c
    src/pty_sync.c)

set(PTYTEST_PUBLIC_HEADERS
    include/ptytest.h
    include/pty_assert.h)

find_library(PTYTEST_UTIL_LIB util)

function(ptytest_configure_target _tgt)
    target_include_directories(${_tgt}
        PUBLIC  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
                $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}/ptytest>
        PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_compile_definitions(${_tgt} PRIVATE
        PTYTEST_VERSION_STRING="${PTYTEST_VERSION}")
    target_compile_options(${_tgt} PRIVATE ${PTYTEST_WARNINGS} ${PTYTEST_HARDENING})
    # forkpty() lives in libutil on older glibc and on the BSDs.
    if(PTYTEST_UTIL_LIB)
        target_link_libraries(${_tgt} PRIVATE util)
    endif()
endfunction()

add_library(ptytest SHARED ${PTYTEST_SOURCES})
ptytest_configure_target(ptytest)
set_target_properties(ptytest PROPERTIES
    OUTPUT_NAME ptytest
    VERSION     ${PROJECT_VERSION}
    SOVERSION   ${PROJECT_VERSION_MAJOR}
    C_VISIBILITY_PRESET hidden
    PUBLIC_HEADER "${PTYTEST_PUBLIC_HEADERS}")
target_link_options(ptytest PRIVATE ${PTYTEST_HARDENING_LINK} "LINKER:--as-needed")

add_library(ptytest_static STATIC ${PTYTEST_SOURCES})
ptytest_configure_target(ptytest_static)
set_target_properties(ptytest_static PROPERTIES
    OUTPUT_NAME ptytest
    POSITION_INDEPENDENT_CODE ON)
if(PTYTEST_UTIL_LIB)
    # A static archive does not carry its dependencies: say so to consumers.
    target_link_libraries(ptytest_static INTERFACE util)
endif()

add_library(ptytest::ptytest ALIAS ptytest)
add_library(ptytest::ptytest_static ALIAS ptytest_static)

if(NOT PTYTEST_TOP_LEVEL)
    return()
endif()

# ---------------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------------
if(PTYTEST_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

# ---------------------------------------------------------------------------
# Manual pages, compressed: lintian rejects uncompressed ones.  Alias pages
# (one per function that shares a page) are symlinks to the compressed page.
# ---------------------------------------------------------------------------
set(PTYTEST_MAN3_PAGES
    libptytest pty_open pty_run pty_send pty_wait_for pty_wait_exit
    pty_feed pty_cell_text pty_row_text pty_region_compare
    pty_region_compare_style pty_screen_dump pty_wait_region pty_save_evidence
    pty_assert)

# alias page -> page it points to
set(PTYTEST_MAN3_ALIASES
    pty_close:pty_open pty_resize:pty_open pty_get_size:pty_open
    pty_set_close_grace:pty_open pty_version:pty_open
    pty_setenv:pty_run pty_signal:pty_run
    pty_send_key:pty_send pty_send_str:pty_send
    pty_wait_until:pty_wait_for pty_settle:pty_wait_for pty_drain:pty_wait_for
    pty_trace_enable:pty_feed
    pty_cell_attr:pty_cell_text pty_cell_fg:pty_cell_text
    pty_cell_bg:pty_cell_text pty_cursor:pty_cell_text
    pty_row_contains:pty_row_text pty_screen_contains:pty_row_text
    pty_screen_text:pty_row_text pty_screen_is_blank:pty_row_text
    pty_cell_describe:pty_cell_text
    pty_screen_compare:pty_region_compare
    pty_region_style_map:pty_region_compare_style
    pty_screen_dump_ansi:pty_screen_dump pty_region_dump_c:pty_screen_dump
    pty_wait_region_style:pty_wait_region)

set(PTYTEST_MAN_OUT "${CMAKE_BINARY_DIR}/man")
set(PTYTEST_MAN_GZ "")
foreach(_entry IN ITEMS ${PTYTEST_MAN3_PAGES} libptytest@7)
    if(_entry MATCHES "@7$")
        string(REPLACE "@7" "" _page "${_entry}")
        set(_sec 7)
    else()
        set(_page "${_entry}")
        set(_sec 3)
    endif()
    set(_src "${CMAKE_SOURCE_DIR}/man/man${_sec}/${_page}.${_sec}")
    set(_dst "${PTYTEST_MAN_OUT}/man${_sec}/${_page}.${_sec}")
    # Copy, then compress in place; -n keeps the build reproducible.
    add_custom_command(
        OUTPUT  "${_dst}.gz"
        COMMAND "${CMAKE_COMMAND}" -E make_directory "${PTYTEST_MAN_OUT}/man${_sec}"
        COMMAND "${CMAKE_COMMAND}" -E copy "${_src}" "${_dst}"
        COMMAND gzip -9nf "${_dst}"
        DEPENDS "${_src}"
        COMMENT "Compressing man page ${_page}.${_sec}")
    list(APPEND PTYTEST_MAN_GZ "${_dst}.gz")
endforeach()

set(PTYTEST_MAN_LINKS "")
foreach(_alias IN LISTS PTYTEST_MAN3_ALIASES)
    string(REPLACE ":" ";" _pair "${_alias}")
    list(GET _pair 0 _from)
    list(GET _pair 1 _to)
    add_custom_command(
        OUTPUT  "${PTYTEST_MAN_OUT}/man3/${_from}.3.gz"
        COMMAND "${CMAKE_COMMAND}" -E create_symlink
                "${_to}.3.gz" "${PTYTEST_MAN_OUT}/man3/${_from}.3.gz"
        DEPENDS "${PTYTEST_MAN_OUT}/man3/${_to}.3.gz")
    list(APPEND PTYTEST_MAN_LINKS "${PTYTEST_MAN_OUT}/man3/${_from}.3.gz")
endforeach()
add_custom_target(ptytest_man ALL DEPENDS ${PTYTEST_MAN_GZ} ${PTYTEST_MAN_LINKS})

# ---------------------------------------------------------------------------
# Debian documentation: copyright and changelog, which lintian requires in
# every binary package.
# ---------------------------------------------------------------------------
set(PTYTEST_MAINTAINER "Péter Császár <peter@csaszar.email>")
set(PTYTEST_PKG_RUNTIME "libptytest${PROJECT_VERSION_MAJOR}")
set(PTYTEST_PKG_DEV     "libptytest-dev")

file(WRITE "${CMAKE_BINARY_DIR}/debian-doc/copyright"
"Format: https://www.debian.org/doc/packaging-manuals/copyright-format/1.0/
Upstream-Name: libptytest
Source: https://github.com/csjpeter/libptytest

Files: *
Copyright: 2026 Péter Császár <peter@csaszar.email>
License: GPL-3+
 This program is free software: you can redistribute it and/or modify it
 under the terms of the GNU General Public License as published by the Free
 Software Foundation, either version 3 of the License, or (at your option)
 any later version.
 .
 This program is distributed in the hope that it will be useful, but WITHOUT
 ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 more details.
 .
 On Debian systems, the complete text of the GNU General Public License
 version 3 can be found in \"/usr/share/common-licenses/GPL-3\".
")

string(TIMESTAMP PTYTEST_CHANGELOG_DATE "%a, %d %b %Y %H:%M:%S +0000" UTC)

function(ptytest_write_changelog _pkg)
    set(_dir "${CMAKE_BINARY_DIR}/debian-doc/${_pkg}")
    file(WRITE "${_dir}/changelog"
"${_pkg} (${PTYTEST_VERSION}) stable; urgency=medium

  * Build from git commit ${PTYTEST_GIT_COMMIT}.
  * The per-change history is the git history; this file exists because
    Debian policy requires a changelog in every binary package.

 -- ${PTYTEST_MAINTAINER}  ${PTYTEST_CHANGELOG_DATE}
")
    add_custom_command(
        OUTPUT  "${_dir}/changelog.gz"
        COMMAND "${CMAKE_COMMAND}" -E copy "${_dir}/changelog" "${_dir}/changelog.tmp"
        COMMAND gzip -9nf "${_dir}/changelog.tmp"
        COMMAND "${CMAKE_COMMAND}" -E rename "${_dir}/changelog.tmp.gz" "${_dir}/changelog.gz"
        DEPENDS "${_dir}/changelog"
        COMMENT "Compressing changelog for ${_pkg}")
    add_custom_target("ptytest_changelog_${_pkg}" ALL DEPENDS "${_dir}/changelog.gz")
endfunction()

ptytest_write_changelog("${PTYTEST_PKG_RUNTIME}")
ptytest_write_changelog("${PTYTEST_PKG_DEV}")

# ---------------------------------------------------------------------------
# Install rules: the list of what the software consists of.
#
#   runtime  libptytest.so.N                    -> libptytestN
#   dev      headers, libptytest.so, .a, pkg-config, CMake package,
#            man pages (3 and 7)                -> libptytest-dev
# ---------------------------------------------------------------------------
include(CMakePackageConfigHelpers)

install(TARGETS ptytest ptytest_static
        EXPORT ptytestTargets
        LIBRARY DESTINATION "${CMAKE_INSTALL_LIBDIR}"
                COMPONENT runtime
                NAMELINK_COMPONENT dev
        ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}"
                COMPONENT dev
        PUBLIC_HEADER DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}/ptytest"
                COMPONENT dev)

install(EXPORT ptytestTargets
        NAMESPACE ptytest::
        DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/ptytest"
        COMPONENT dev)

configure_package_config_file(
    "${CMAKE_SOURCE_DIR}/cmake/ptytestConfig.cmake.in"
    "${CMAKE_BINARY_DIR}/ptytestConfig.cmake"
    INSTALL_DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/ptytest")
write_basic_package_version_file(
    "${CMAKE_BINARY_DIR}/ptytestConfigVersion.cmake"
    VERSION ${PROJECT_VERSION}
    COMPATIBILITY SameMajorVersion)
install(FILES "${CMAKE_BINARY_DIR}/ptytestConfig.cmake"
              "${CMAKE_BINARY_DIR}/ptytestConfigVersion.cmake"
        DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/ptytest"
        COMPONENT dev)

if(PTYTEST_UTIL_LIB)
    set(PTYTEST_PC_LIBS_PRIVATE "-lutil")
else()
    set(PTYTEST_PC_LIBS_PRIVATE "")
endif()
configure_file("${CMAKE_SOURCE_DIR}/cmake/libptytest.pc.in"
               "${CMAKE_BINARY_DIR}/libptytest.pc" @ONLY)
install(FILES "${CMAKE_BINARY_DIR}/libptytest.pc"
        DESTINATION "${CMAKE_INSTALL_LIBDIR}/pkgconfig"
        COMPONENT dev)

install(DIRECTORY "${PTYTEST_MAN_OUT}/man3" "${PTYTEST_MAN_OUT}/man7"
        DESTINATION "${CMAKE_INSTALL_MANDIR}"
        COMPONENT dev)

install(FILES "${CMAKE_BINARY_DIR}/debian-doc/copyright"
              "${CMAKE_BINARY_DIR}/debian-doc/${PTYTEST_PKG_RUNTIME}/changelog.gz"
        DESTINATION "${CMAKE_INSTALL_DATADIR}/doc/${PTYTEST_PKG_RUNTIME}"
        COMPONENT runtime)
install(FILES "${CMAKE_BINARY_DIR}/debian-doc/copyright"
              "${CMAKE_BINARY_DIR}/debian-doc/${PTYTEST_PKG_DEV}/changelog.gz"
        DESTINATION "${CMAKE_INSTALL_DATADIR}/doc/${PTYTEST_PKG_DEV}"
        COMPONENT dev)

# lintian overrides: the XB- build metadata fields (see the file).
foreach(_comp_pkg IN ITEMS "runtime:${PTYTEST_PKG_RUNTIME}" "dev:${PTYTEST_PKG_DEV}")
    string(REPLACE ":" ";" _cp "${_comp_pkg}")
    list(GET _cp 0 _comp)
    list(GET _cp 1 PACKAGE)
    configure_file("${CMAKE_SOURCE_DIR}/debian/lintian-overrides"
                   "${CMAKE_BINARY_DIR}/debian-doc/${PACKAGE}/lintian-overrides" @ONLY)
    install(FILES "${CMAKE_BINARY_DIR}/debian-doc/${PACKAGE}/lintian-overrides"
            DESTINATION "${CMAKE_INSTALL_DATADIR}/lintian/overrides"
            RENAME "${PACKAGE}"
            COMPONENT ${_comp})
endforeach()

# ===========================================================================
# Packaging (CPack)
#
# All package metadata lives here and nowhere else: no debian/control, no
# .spec file.  manage.sh only configures a Release build and calls CPack.
# See policies/packaging-and-release-model.md.
# ===========================================================================
set(CPACK_PACKAGE_NAME          "libptytest")
set(CPACK_PACKAGE_VERSION       "${PTYTEST_VERSION}")
set(CPACK_PACKAGE_CONTACT       "${PTYTEST_MAINTAINER}")
set(CPACK_PACKAGE_VENDOR        "csjpeter")
set(CPACK_PACKAGE_HOMEPAGE_URL  "https://github.com/csjpeter/libptytest")
# No CPACK_PACKAGE_DESCRIPTION_SUMMARY: CPackDeb would prepend it to each
# component's description, and the first line of each is its own synopsis.
set(CPACK_STRIP_FILES ON)
# Every package of a build, and nothing else, goes to build/packages/; CPack's
# own staging tree stays in the build directory.  manage.sh empties the
# directory before each run.  See policies/packaging-and-release-model.md,
# "Where the output goes".
set(CPACK_OUTPUT_FILE_PREFIX "${CMAKE_BINARY_DIR}/packages")
# Without this the directory modes come from the building user's umask.
set(CPACK_INSTALL_DEFAULT_DIRECTORY_PERMISSIONS
    OWNER_READ OWNER_WRITE OWNER_EXECUTE
    GROUP_READ GROUP_EXECUTE
    WORLD_READ WORLD_EXECUTE)

set(CPACK_GENERATOR "DEB;RPM")
set(CPACK_DEB_COMPONENT_INSTALL ON)
set(CPACK_RPM_COMPONENT_INSTALL ON)
set(CPACK_COMPONENTS_ALL runtime dev)
set(CPACK_COMPONENTS_GROUPING IGNORE)

# --- Debian ----------------------------------------------------------------
set(CPACK_DEBIAN_PACKAGE_MAINTAINER "${PTYTEST_MAINTAINER}")
set(CPACK_DEBIAN_PACKAGE_PRIORITY   "optional")
set(CPACK_DEBIAN_PACKAGE_HOMEPAGE   "${CPACK_PACKAGE_HOMEPAGE_URL}")
# Discovered from the binaries rather than guessed by hand.
set(CPACK_DEBIAN_PACKAGE_SHLIBDEPS  ON)

execute_process(COMMAND dpkg --print-architecture
                OUTPUT_VARIABLE PTYTEST_DEB_ARCH
                OUTPUT_STRIP_TRAILING_WHITESPACE
                RESULT_VARIABLE _ptytest_arch_rc)
if(NOT _ptytest_arch_rc EQUAL 0 OR NOT PTYTEST_DEB_ARCH)
    set(PTYTEST_DEB_ARCH "amd64")
endif()

set(CPACK_DEBIAN_RUNTIME_PACKAGE_NAME    "${PTYTEST_PKG_RUNTIME}")
set(CPACK_DEBIAN_RUNTIME_PACKAGE_SECTION "libs")
# shlibs file and the ldconfig trigger, so packages linking the library get
# a correct dependency on it.
# (CPackDeb has no per-component switch for this; the dev package has no
# shared object, so the global one affects only the runtime package.)
set(CPACK_DEBIAN_PACKAGE_GENERATE_SHLIBS ON)
# What consumers inherit through shlibdeps: at least this version, never
# exactly it.  An exact dependency makes apt remove every consumer when the
# library is upgraded.  See policies/packaging-and-release-model.md,
# "Dependencies only ever look upward".
set(CPACK_DEBIAN_PACKAGE_GENERATE_SHLIBS_POLICY ">=")
set(CPACK_DEBIAN_RUNTIME_FILE_NAME
    "${PTYTEST_PKG_RUNTIME}_${PTYTEST_VERSION}_${PTYTEST_BUILD_STAMP}_${PTYTEST_GIT_COMMIT}_${PTYTEST_DEB_ARCH}.deb")
set(CPACK_COMPONENT_RUNTIME_DESCRIPTION
"PTY-driven, cell-exact test library for terminal programs
libptytest runs a terminal program inside a pseudo-terminal, sends it
keystrokes, and turns its output into a grid of cells through a built-in
terminal emulator, so a test can check every character, attribute and
colour on the screen, or compare the whole screen at once.

This package contains the shared library.")

set(CPACK_DEBIAN_DEV_PACKAGE_NAME    "${PTYTEST_PKG_DEV}")
set(CPACK_DEBIAN_DEV_PACKAGE_SECTION "libdevel")
set(CPACK_DEBIAN_DEV_PACKAGE_DEPENDS "${PTYTEST_PKG_RUNTIME} (= ${PTYTEST_VERSION})")
set(CPACK_DEBIAN_DEV_FILE_NAME
    "${PTYTEST_PKG_DEV}_${PTYTEST_VERSION}_${PTYTEST_BUILD_STAMP}_${PTYTEST_GIT_COMMIT}_${PTYTEST_DEB_ARCH}.deb")
set(CPACK_COMPONENT_DEV_DESCRIPTION
"PTY-driven, cell-exact test library for terminal programs - development
libptytest runs a terminal program inside a pseudo-terminal, sends it
keystrokes, and turns its output into a grid of cells through a built-in
terminal emulator, so a test can check every character, attribute and
colour on the screen, or compare the whole screen at once.

This package contains the headers, the static library, the pkg-config
and CMake package files, and the manual pages.")

# Build date and commit as XB- control fields, and an ldconfig trigger in
# place of CPack's ldconfig calls.  See cmake/deb-post-build.cmake.
set(CPACK_POST_BUILD_SCRIPTS "${CMAKE_SOURCE_DIR}/cmake/deb-post-build.cmake")
# CPACK_-prefixed, because only those reach a post-build script.
set(CPACK_PTYTEST_GIT_COMMIT "${PTYTEST_GIT_COMMIT}")
set(CPACK_PTYTEST_BUILD_DATE "${PTYTEST_BUILD_DATE}")

# --- RPM -------------------------------------------------------------------
set(CPACK_RPM_PACKAGE_LICENSE "GPL-3.0-or-later")
set(CPACK_RPM_PACKAGE_GROUP   "Development/Libraries")
set(CPACK_RPM_RUNTIME_PACKAGE_SUMMARY
    "PTY-driven, cell-exact test library for terminal programs")
set(CPACK_RPM_DEV_PACKAGE_SUMMARY
    "PTY-driven, cell-exact test library for terminal programs - development")
set(CPACK_RPM_RUNTIME_PACKAGE_NAME "libptytest")
set(CPACK_RPM_DEV_PACKAGE_NAME     "libptytest-devel")
set(CPACK_RPM_DEV_PACKAGE_REQUIRES "libptytest = ${PTYTEST_VERSION}")
set(CPACK_RPM_PACKAGE_AUTOREQPROV ON)
set(CPACK_RPM_RUNTIME_FILE_NAME RPM-DEFAULT)
set(CPACK_RPM_DEV_FILE_NAME RPM-DEFAULT)
# Directories the system already owns must not be claimed by this package.
set(CPACK_RPM_EXCLUDE_FROM_AUTO_FILELIST_ADDITION
    /usr/share/man /usr/share/man/man3 /usr/share/man/man7
    /usr/share/doc /usr/lib64/pkgconfig /usr/lib64/cmake
    /usr/lib/pkgconfig /usr/lib/cmake)

include(CPack)
