# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Péter Császár <peter@csaszar.email>
# Post-processing of each built Debian package, run by CPack after it has
# written the packages (CPACK_POST_BUILD_SCRIPTS).  Two things CPack cannot
# express through its variables are done here, so that the decision still
# lives in this repository's CMake files rather than in manage.sh:
#
# 1. The build date and the git commit, as XB- control fields.
#    CPackDeb writes the control file from a fixed set of
#    CPACK_DEBIAN_PACKAGE_* variables and has none for a custom field.  The
#    fields go beside the version, never into it: Version has one job, which
#    is telling apt which of two packages is newer.  An XB- field survives
#    into the apt index and into dpkg -s, everywhere except the pool file
#    name.  See policies/packaging-and-release-model.md, "Identifying a
#    build".
#
# 2. An ldconfig trigger instead of ldconfig calls.
#    For a package with a shared library in /usr/lib, CPack (3.31) generates
#    a postinst and a postrm whose only command is "ldconfig".  Debian
#    policy (8.1.1) asks for "activate-noawait ldconfig" in DEBIAN/triggers
#    instead, and lintian rejects both the calls (maintscript-calls-ldconfig)
#    and their absence without a trigger (lacks-ldconfig-trigger).  The
#    generated scripts are replaced only if they are exactly what CPack
#    writes; anything else stops the build.
#
# Only CPACK_-prefixed variables reach this script: it runs as a separate
# cmake -P invocation against CPackConfig.cmake.

if(NOT CPACK_PTYTEST_GIT_COMMIT OR NOT CPACK_PTYTEST_BUILD_DATE)
    message(FATAL_ERROR
        "build metadata is missing: CPACK_PTYTEST_GIT_COMMIT and "
        "CPACK_PTYTEST_BUILD_DATE must be set in CMakeLists.txt")
endif()

set(_ldconfig_postinst "#!/bin/sh\n\nset -e\n\nif [ \"$1\" = \"configure\" ]; then\n\tldconfig\nfi\n")
set(_ldconfig_postrm   "#!/bin/sh\n\nset -e\n\nif [ \"$1\" = \"remove\" ]; then\n\tldconfig\nfi\n")

foreach(_pkg IN LISTS CPACK_PACKAGE_FILES)
    if(NOT _pkg MATCHES "\\.deb$")
        continue()
    endif()

    find_program(DPKG_DEB_EXECUTABLE dpkg-deb)
    if(NOT DPKG_DEB_EXECUTABLE)
        # A package without its identification is not one to publish.
        message(FATAL_ERROR "dpkg-deb not found: cannot post-process ${_pkg}")
    endif()

    get_filename_component(_name "${_pkg}" NAME)
    set(_work "${CPACK_TOPLEVEL_DIRECTORY}/deb-post-build/${_name}")
    file(REMOVE_RECURSE "${_work}")
    file(MAKE_DIRECTORY "${_work}")

    # Unpack, adjust DEBIAN/, repack.  dpkg-deb -b rebuilds the archive
    # from the tree, so nothing outside DEBIAN/ changes.
    execute_process(COMMAND "${DPKG_DEB_EXECUTABLE}" -R "${_pkg}" "${_work}"
                    RESULT_VARIABLE _rc)
    if(NOT _rc EQUAL 0)
        message(FATAL_ERROR "could not unpack ${_pkg}")
    endif()

    file(READ "${_work}/DEBIAN/control" _control)
    string(STRIP "${_control}" _control)
    file(WRITE "${_work}/DEBIAN/control"
         "${_control}\nXB-Git-Commit: ${CPACK_PTYTEST_GIT_COMMIT}\nXB-Build-Date: ${CPACK_PTYTEST_BUILD_DATE}\n")

    if(EXISTS "${_work}/DEBIAN/shlibs")
        foreach(_script postinst postrm)
            set(_path "${_work}/DEBIAN/${_script}")
            if(EXISTS "${_path}")
                file(READ "${_path}" _content)
                if(NOT _content STREQUAL "${_ldconfig_${_script}}")
                    message(FATAL_ERROR
                        "${_name}: DEBIAN/${_script} is not the ldconfig-only "
                        "script CPack generates; refusing to replace it:\n${_content}")
                endif()
                file(REMOVE "${_path}")
            endif()
        endforeach()
        file(WRITE "${_work}/DEBIAN/triggers" "activate-noawait ldconfig\n")
        # md5sums lists only data files, so it is unaffected.
    endif()

    execute_process(COMMAND "${DPKG_DEB_EXECUTABLE}" --root-owner-group
                            -b "${_work}" "${_pkg}"
                    OUTPUT_QUIET
                    RESULT_VARIABLE _rc)
    if(NOT _rc EQUAL 0)
        message(FATAL_ERROR "could not repack ${_pkg}")
    endif()

    file(REMOVE_RECURSE "${_work}")
    message(STATUS "post-processed ${_name}")
endforeach()
