# common macros
# this file will also load platform specific macros

# platform specific macros
if(WIN32)
    include(${CMAKE_MODULE_PATH}/macros/windows.cmake)
elseif(UNIX)
    include(${CMAKE_MODULE_PATH}/macros/unix.cmake)

    if(APPLE)
        include(${CMAKE_MODULE_PATH}/macros/macos.cmake)
    else()
        include(${CMAKE_MODULE_PATH}/macros/linux.cmake)
    endif()
endif()

# override find_package function
# Skip this override when vcpkg is active: vcpkg.cmake already overrides find_package and
# saves the original cmake built-in as _find_package.  If we override find_package again,
# _find_package gets overwritten to point to vcpkg's macro instead of the cmake built-in.
# The openssl vcpkg-cmake-wrapper then calls _find_package -> vcpkg macro -> wrapper again
# causing infinite recursion (1000-depth crash).
if(NOT VCPKG_TOOLCHAIN)
    macro(find_package)  # cmake-lint: disable=C0103
        string(TOLOWER "${ARGV0}" ARGV0_LOWER)
        if(
            (("${ARGV0_LOWER}" STREQUAL "boost") AND DEFINED FETCH_CONTENT_BOOST_USED) OR
            (("${ARGV0_LOWER}" STREQUAL "libevdev") AND DEFINED EXTERNAL_PROJECT_LIBEVDEV_USED)
        )
            # Do nothing, as the package has already been fetched
        else()
            # Call the original find_package function
            _find_package(${ARGV})
        endif()
    endmacro()
endif()
