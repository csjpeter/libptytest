# SPDX-License-Identifier: GPL-3.0-only
# Copyright (c) 2026 Peter Csaszar (Császár Péter)
# Created: 2026-10-23
# Derivation of the package version: project version + total commit count.

# ptytest_derive_version(<out> <project-version> <source-dir>)
#
# <project-version>.<git rev-list --count HEAD>.  A tag takes no part and
# restarts nothing: the same tree has the same version before and after it is
# tagged.  Without git, or outside a repository, the bare project version.
# The count needs the full history: a shallow clone is an error for a
# top-level build (a consumer's shallow checkout gets the bare version).
# See policies/packaging-and-release-model.md, "Deriving the version".
function(ptytest_derive_version _out _version _srcdir)
    set(_result "${_version}")
    find_package(Git QUIET)
    if(Git_FOUND AND EXISTS "${_srcdir}/.git")
        execute_process(
            COMMAND "${GIT_EXECUTABLE}" rev-parse --is-shallow-repository
            WORKING_DIRECTORY "${_srcdir}"
            OUTPUT_VARIABLE _shallow
            OUTPUT_STRIP_TRAILING_WHITESPACE
            RESULT_VARIABLE _shallow_rc)
        if(_shallow_rc EQUAL 0 AND _shallow STREQUAL "true")
            if(_srcdir STREQUAL CMAKE_SOURCE_DIR)
                message(FATAL_ERROR "the package version counts commits; "
                        "${_srcdir} is a shallow clone, build from a full one")
            endif()
            # Used as a submodule of another project: no package is built.
            set(${_out} "${_version}" PARENT_SCOPE)
            return()
        endif()
        execute_process(
            COMMAND "${GIT_EXECUTABLE}" rev-list --count HEAD
            WORKING_DIRECTORY "${_srcdir}"
            OUTPUT_VARIABLE _count
            OUTPUT_STRIP_TRAILING_WHITESPACE
            RESULT_VARIABLE _count_rc)
        if(_count_rc EQUAL 0 AND _count)
            set(_result "${_version}.${_count}")
        endif()
    endif()
    set(${_out} "${_result}" PARENT_SCOPE)
endfunction()
