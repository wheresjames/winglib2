# Slint is a Rust UI library shipped as a prebuilt C++ binary package (an
# installed CMake package exporting the Slint::Slint target). The wl2_slint
# module includes this file and calls wl2_find_slint() from its own scope; the
# include guard keeps the helper definitions idempotent if it is included more
# than once in a single configure.
#
# Every provider hands back the same Slint::Slint target through
# WL2_SLINT_TARGET, so the module CMake grows no platform branches. The provider
# model deliberately prefers the prebuilt package (no Rust toolchain); only the
# opt-in "source" provider needs cargo/Rust.
include_guard(GLOBAL)

# Download and extract the prebuilt C++ binary package into WL2_SLINT_ROOT, then
# return the directory that find_package(Slint) should search. The archive
# unpacks to an installed prefix containing lib/cmake/Slint/SlintConfig.cmake.
function(_wl2_fetch_slint out_prefix)
    set(_src "${WL2_SLINT_ROOT}/pkg")
    set(_marker "${_src}/.wl2-slint-${WL2_SLINT_VERSION}")
    if(NOT EXISTS "${_marker}")
        if(NOT WL2_SLINT_URL)
            message(FATAL_ERROR "WL2_SLINT_URL is unset; cannot fetch the Slint binary package")
        endif()
        set(_archive "${WL2_SLINT_ROOT}/slint-cpp-${WL2_SLINT_VERSION}.tar.gz")
        message(STATUS "Fetching prebuilt Slint ${WL2_SLINT_VERSION} from ${WL2_SLINT_URL}")
        set(_download_args "${WL2_SLINT_URL}" "${_archive}" SHOW_PROGRESS STATUS _dl_status)
        if(WL2_SLINT_URL_HASH)
            list(APPEND _download_args EXPECTED_HASH "${WL2_SLINT_URL_HASH}")
        else()
            message(WARNING "WL2_SLINT_URL_HASH is unset; the fetched Slint package is not integrity-checked")
        endif()
        file(DOWNLOAD ${_download_args})
        list(GET _dl_status 0 _dl_code)
        if(NOT _dl_code EQUAL 0)
            list(GET _dl_status 1 _dl_msg)
            message(FATAL_ERROR "Failed to download prebuilt Slint: ${_dl_msg}")
        endif()
        file(REMOVE_RECURSE "${_src}")
        file(MAKE_DIRECTORY "${_src}")
        file(ARCHIVE_EXTRACT INPUT "${_archive}" DESTINATION "${_src}")
        file(WRITE "${_marker}" "${WL2_SLINT_URL}\n")
    endif()

    # The archive contains a single top-level prefix dir; locate SlintConfig.cmake
    # so the layout assumption stays in one place and return its install prefix.
    file(GLOB_RECURSE _slint_configs "${_src}/SlintConfig.cmake" "${_src}/*/SlintConfig.cmake")
    if(NOT _slint_configs)
        message(FATAL_ERROR "Fetched Slint package did not contain SlintConfig.cmake under ${_src}")
    endif()
    list(GET _slint_configs 0 _slint_config)
    get_filename_component(_config_dir "${_slint_config}" DIRECTORY)
    set(${out_prefix} "${_config_dir}" PARENT_SCOPE)
endfunction()

# Build Slint from source via FetchContent. This is the only provider that needs
# a Rust toolchain (cargo); it is never selected by "auto".
function(_wl2_source_slint)
    find_program(WL2_SLINT_CARGO cargo)
    if(NOT WL2_SLINT_CARGO)
        message(FATAL_ERROR "WL2_SLINT_PROVIDER=source requires cargo/Rust on PATH")
    endif()
    include(FetchContent)
    set(SLINT_FEATURE_INTERPRETER ON CACHE BOOL "" FORCE)
    FetchContent_Declare(Slint
        GIT_REPOSITORY "${WL2_SLINT_GIT_REPOSITORY}"
        GIT_TAG "v${WL2_SLINT_VERSION}"
        SOURCE_SUBDIR api/cpp)
    FetchContent_MakeAvailable(Slint)
endfunction()

function(wl2_find_slint)
    include("${CMAKE_CURRENT_LIST_DIR}/options.cmake" OPTIONAL)

    set(WL2_HAVE_SLINT FALSE PARENT_SCOPE)
    set(WL2_SLINT_TARGET "" PARENT_SCOPE)
    set(WL2_SLINT_PROVIDER_USED "" PARENT_SCOPE)

    if(WL2_SLINT_PROVIDER STREQUAL "off")
        message(STATUS "WL2_SLINT_PROVIDER=off; wl2_slint module disabled")
        return()
    endif()

    # source (opt-in, requires Rust): build Slint from source.
    if(WL2_SLINT_PROVIDER STREQUAL "source")
        _wl2_source_slint()
        if(TARGET Slint::Slint)
            set(WL2_HAVE_SLINT TRUE PARENT_SCOPE)
            set(WL2_SLINT_TARGET Slint::Slint PARENT_SCOPE)
            set(WL2_SLINT_PROVIDER_USED source PARENT_SCOPE)
            message(STATUS "Using Slint ${WL2_SLINT_VERSION} built from source")
            return()
        endif()
        message(FATAL_ERROR "WL2_SLINT_PROVIDER=source but Slint::Slint was not created")
    endif()

    # local / auto: a target-local prebuilt package staged under WL2_SLINT_ROOT.
    if(WL2_SLINT_PROVIDER STREQUAL "local" OR WL2_SLINT_PROVIDER STREQUAL "auto")
        if(WL2_SLINT_ROOT)
            find_package(Slint CONFIG QUIET PATHS "${WL2_SLINT_ROOT}" NO_DEFAULT_PATH)
        endif()
        if(TARGET Slint::Slint)
            set(WL2_HAVE_SLINT TRUE PARENT_SCOPE)
            set(WL2_SLINT_TARGET Slint::Slint PARENT_SCOPE)
            set(WL2_SLINT_PROVIDER_USED local PARENT_SCOPE)
            message(STATUS "Using local Slint package from ${WL2_SLINT_ROOT}")
            return()
        endif()
        if(WL2_SLINT_PROVIDER STREQUAL "local")
            message(FATAL_ERROR "WL2_SLINT_PROVIDER=local but Slint was not found under ${WL2_SLINT_ROOT}")
        endif()
    endif()

    # fetch / auto(+WL2_FETCH_DEPS): download a pinned prebuilt C++ package.
    if(WL2_SLINT_PROVIDER STREQUAL "fetch" OR (WL2_SLINT_PROVIDER STREQUAL "auto" AND WL2_FETCH_DEPS))
        if(WL2_FETCH_DEPS)
            if(NOT WL2_SLINT_ROOT)
                message(FATAL_ERROR "WL2_SLINT_ROOT is unset; cannot stage fetched Slint")
            endif()
            _wl2_fetch_slint(_config_dir)
            find_package(Slint CONFIG REQUIRED PATHS "${_config_dir}" NO_DEFAULT_PATH)
            set(WL2_HAVE_SLINT TRUE PARENT_SCOPE)
            set(WL2_SLINT_TARGET Slint::Slint PARENT_SCOPE)
            set(WL2_SLINT_PROVIDER_USED fetch PARENT_SCOPE)
            message(STATUS "Using fetched prebuilt Slint ${WL2_SLINT_VERSION} staged at ${WL2_SLINT_ROOT}")
            return()
        elseif(WL2_SLINT_PROVIDER STREQUAL "fetch")
            message(FATAL_ERROR "WL2_SLINT_PROVIDER=fetch but WL2_FETCH_DEPS=OFF")
        endif()
    endif()

    # package / auto: a system-installed Slint package.
    if(WL2_SLINT_PROVIDER STREQUAL "package" OR WL2_SLINT_PROVIDER STREQUAL "auto")
        find_package(Slint CONFIG QUIET)
        if(TARGET Slint::Slint)
            set(WL2_HAVE_SLINT TRUE PARENT_SCOPE)
            set(WL2_SLINT_TARGET Slint::Slint PARENT_SCOPE)
            set(WL2_SLINT_PROVIDER_USED package PARENT_SCOPE)
            message(STATUS "Using package/system Slint")
            return()
        endif()
        if(WL2_SLINT_PROVIDER STREQUAL "package")
            message(FATAL_ERROR "WL2_SLINT_PROVIDER=package but Slint::Slint was not found")
        endif()
    endif()

    message(STATUS "Slint not found; wl2_slint module disabled")
endfunction()
