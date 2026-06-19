# Standalone Asio is header-only, so this helper only has to locate (or download)
# a tree containing asio.hpp and expose it through a single INTERFACE target that
# also carries ASIO_STANDALONE and Threads::Threads. The wl2_asio module includes
# this file and calls wl2_find_asio() from its own scope; the include guard keeps
# the definitions idempotent if it is included more than once in one configure.
include_guard(GLOBAL)

# Create (once) the INTERFACE target every provider hands back. include_dir must
# contain asio.hpp.
function(_wl2_asio_make_target include_dir)
    if(NOT TARGET wl2_asio_dep)
        find_package(Threads REQUIRED)
        add_library(wl2_asio_dep INTERFACE IMPORTED GLOBAL)
        set_target_properties(wl2_asio_dep PROPERTIES
            INTERFACE_INCLUDE_DIRECTORIES "${include_dir}"
            INTERFACE_COMPILE_DEFINITIONS "ASIO_STANDALONE;ASIO_NO_DEPRECATED"
            INTERFACE_LINK_LIBRARIES Threads::Threads)
    endif()
endfunction()

# Download and extract the standalone Asio source archive into WL2_ASIO_ROOT.
# Header-only: there is nothing to build, so this is a configure-time fetch that
# guarantees the include directory exists when the target is created.
function(_wl2_fetch_asio out_include_dir)
    set(_src "${WL2_ASIO_ROOT}/src")
    set(_marker "${_src}/.wl2-asio-${WL2_ASIO_VERSION}")
    if(NOT EXISTS "${_marker}")
        set(_archive "${WL2_ASIO_ROOT}/asio-${WL2_ASIO_VERSION}.tar.gz")
        message(STATUS "Fetching standalone Asio ${WL2_ASIO_VERSION} from ${WL2_ASIO_URL}")
        file(DOWNLOAD "${WL2_ASIO_URL}" "${_archive}"
            EXPECTED_HASH "${WL2_ASIO_URL_HASH}"
            SHOW_PROGRESS
            STATUS _dl_status)
        list(GET _dl_status 0 _dl_code)
        if(NOT _dl_code EQUAL 0)
            list(GET _dl_status 1 _dl_msg)
            message(FATAL_ERROR "Failed to download standalone Asio: ${_dl_msg}")
        endif()
        file(REMOVE_RECURSE "${_src}")
        file(MAKE_DIRECTORY "${_src}")
        file(ARCHIVE_EXTRACT INPUT "${_archive}" DESTINATION "${_src}")
        file(WRITE "${_marker}" "${WL2_ASIO_URL_HASH}\n")
    endif()

    # The GitHub tag archive unpacks to asio-asio-<ver>/asio/include/asio.hpp;
    # glob for asio.hpp so the layout assumption stays in one place.
    file(GLOB_RECURSE _asio_headers "${_src}/asio.hpp" "${_src}/*/asio.hpp")
    if(NOT _asio_headers)
        message(FATAL_ERROR "Fetched Asio archive did not contain asio.hpp under ${_src}")
    endif()
    list(GET _asio_headers 0 _asio_header)
    get_filename_component(_include_dir "${_asio_header}" DIRECTORY)
    set(${out_include_dir} "${_include_dir}" PARENT_SCOPE)
endfunction()

# Look for an existing asio.hpp. When system_paths is TRUE, default search paths
# are allowed (package/system provider); otherwise only WL2_ASIO_ROOT is searched.
function(_wl2_locate_asio system_paths out_include_dir)
    set(_hint_args)
    if(WL2_ASIO_ROOT)
        list(APPEND _hint_args PATHS "${WL2_ASIO_ROOT}/include" "${WL2_ASIO_ROOT}/src")
    endif()
    if(system_paths)
        find_path(WL2_ASIO_INCLUDE_DIR NAMES asio.hpp ${_hint_args})
    else()
        find_path(WL2_ASIO_INCLUDE_DIR NAMES asio.hpp ${_hint_args} NO_DEFAULT_PATH)
    endif()
    set(${out_include_dir} "${WL2_ASIO_INCLUDE_DIR}" PARENT_SCOPE)
endfunction()

function(wl2_find_asio)
    include("${CMAKE_CURRENT_LIST_DIR}/options.cmake" OPTIONAL)

    set(WL2_HAVE_ASIO FALSE PARENT_SCOPE)
    set(WL2_ASIO_TARGET "" PARENT_SCOPE)
    set(WL2_ASIO_PROVIDER_USED "" PARENT_SCOPE)

    if(WL2_ASIO_PROVIDER STREQUAL "off")
        message(STATUS "WL2_ASIO_PROVIDER=off; wl2_asio module disabled")
        wl2_dependency_note_result(asio DISABLED "WL2_DEPS_ASIO=off")
        return()
    endif()

    # local / auto: a target-local checkout under WL2_ASIO_ROOT.
    if(WL2_ASIO_PROVIDER STREQUAL "local" OR WL2_ASIO_PROVIDER STREQUAL "auto")
        _wl2_locate_asio(FALSE _include_dir)
        if(_include_dir)
            _wl2_asio_make_target("${_include_dir}")
            set(WL2_HAVE_ASIO TRUE PARENT_SCOPE)
            set(WL2_ASIO_TARGET wl2_asio_dep PARENT_SCOPE)
            set(WL2_ASIO_PROVIDER_USED local PARENT_SCOPE)
            wl2_dependency_note_result(asio local "${_include_dir}")
            message(STATUS "Using local standalone Asio from ${_include_dir}")
            return()
        endif()
        if(WL2_ASIO_PROVIDER STREQUAL "local")
            message(FATAL_ERROR "WL2_ASIO_PROVIDER=local but asio.hpp was not found under ${WL2_ASIO_ROOT}")
        endif()
    endif()

    # fetch / auto: download a pinned standalone Asio release.
    if(WL2_ASIO_PROVIDER STREQUAL "fetch" OR WL2_ASIO_PROVIDER STREQUAL "auto")
        if(NOT WL2_ASIO_ROOT)
            message(FATAL_ERROR "WL2_ASIO_ROOT is unset; cannot stage fetched Asio")
        endif()
        _wl2_fetch_asio(_include_dir)
        _wl2_asio_make_target("${_include_dir}")
        set(WL2_HAVE_ASIO TRUE PARENT_SCOPE)
        set(WL2_ASIO_TARGET wl2_asio_dep PARENT_SCOPE)
        set(WL2_ASIO_PROVIDER_USED fetch PARENT_SCOPE)
        wl2_dependency_note_result(asio download "${WL2_ASIO_VERSION} ${WL2_ASIO_ROOT}")
        message(STATUS "Using fetched standalone Asio ${WL2_ASIO_VERSION} staged at ${WL2_ASIO_ROOT}")
        return()
    endif()

    # package / auto: a system-installed standalone Asio.
    if(WL2_ASIO_PROVIDER STREQUAL "package" OR WL2_ASIO_PROVIDER STREQUAL "auto")
        _wl2_locate_asio(TRUE _include_dir)
        if(_include_dir)
            _wl2_asio_make_target("${_include_dir}")
            set(WL2_HAVE_ASIO TRUE PARENT_SCOPE)
            set(WL2_ASIO_TARGET wl2_asio_dep PARENT_SCOPE)
            set(WL2_ASIO_PROVIDER_USED package PARENT_SCOPE)
            wl2_dependency_note_result(asio system "${_include_dir}")
            message(STATUS "Using system standalone Asio from ${_include_dir}")
            return()
        endif()
        if(WL2_ASIO_PROVIDER STREQUAL "package")
            message(FATAL_ERROR "WL2_ASIO_PROVIDER=package but asio.hpp was not found")
        endif()
    endif()

    message(STATUS "standalone Asio not found; wl2_asio module disabled")
    wl2_dependency_note_result(asio DISABLED "not found")
endfunction()
