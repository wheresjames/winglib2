# Locate/fetch RESTinio and its dependencies, exposing everything through a
# single INTERFACE target (wl2_restinio_dep). RESTinio is header-only; its
# dependencies are standalone Asio (reused from the wl2:asio fetch when present),
# fmt, llhttp, and expected-lite.
#
# The wl2_restinio module includes this file and calls wl2_find_restinio() from
# its own scope. The include guard keeps the definitions idempotent.
include_guard(GLOBAL)

include(FetchContent)

# Reuse standalone Asio: prefer the target the wl2:asio provider already made,
# otherwise locate the tree the winglib2 build fetched under WL2_DEPS_ROOT. Sets
# _asio_include_dir in the caller scope on success (empty on failure).
function(_wl2_http_locate_asio out_include_dir)
    set(${out_include_dir} "" PARENT_SCOPE)
    if(TARGET wl2_asio_dep)
        get_target_property(_dirs wl2_asio_dep INTERFACE_INCLUDE_DIRECTORIES)
        if(_dirs)
            list(GET _dirs 0 _dir)
            set(${out_include_dir} "${_dir}" PARENT_SCOPE)
            return()
        endif()
    endif()
    set(_search)
    if(WL2_DEPS_ROOT)
        list(APPEND _search "${WL2_DEPS_ROOT}/asio")
    endif()
    if(WL2_ASIO_ROOT)
        list(APPEND _search "${WL2_ASIO_ROOT}")
    endif()
    foreach(_root IN LISTS _search)
        file(GLOB_RECURSE _hdr "${_root}/*/asio.hpp" "${_root}/asio.hpp")
        if(_hdr)
            list(GET _hdr 0 _one)
            get_filename_component(_dir "${_one}" DIRECTORY)
            set(${out_include_dir} "${_dir}" PARENT_SCOPE)
            return()
        endif()
    endforeach()
    # Last resort: a system-installed standalone Asio.
    find_path(WL2_HTTP_ASIO_INCLUDE_DIR NAMES asio.hpp)
    if(WL2_HTTP_ASIO_INCLUDE_DIR)
        set(${out_include_dir} "${WL2_HTTP_ASIO_INCLUDE_DIR}" PARENT_SCOPE)
    endif()
endfunction()

# Fetched third-party targets are compiled with the winglib2 warning flags,
# which include -Werror from the environment. Silence warnings on those targets
# so upstream code does not fail our stricter build.
function(_wl2_http_silence target)
    if(TARGET ${target})
        get_target_property(_type ${target} TYPE)
        if(NOT _type STREQUAL "INTERFACE_LIBRARY")
            target_compile_options(${target} PRIVATE -w)
        endif()
    endif()
endfunction()

function(wl2_find_restinio)
    include("${CMAKE_CURRENT_LIST_DIR}/options.cmake" OPTIONAL)

    set(WL2_HAVE_RESTINIO FALSE PARENT_SCOPE)
    set(WL2_RESTINIO_TARGET "" PARENT_SCOPE)

    if(WL2_HTTP_PROVIDER STREQUAL "off")
        message(STATUS "WL2_HTTP_PROVIDER=off; wl2_restinio module disabled")
        return()
    endif()

    _wl2_http_locate_asio(_asio_include_dir)
    if(NOT _asio_include_dir)
        message(STATUS "standalone Asio not found; wl2_restinio module disabled "
            "(enable wl2:asio or set WL2_DEPS_ROOT)")
        return()
    endif()
    message(STATUS "wl2:http will use standalone Asio from ${_asio_include_dir}")

    # --- fmt, expected-lite, llhttp (built as part of this project) ---------
    FetchContent_Declare(wl2http_fmt
        GIT_REPOSITORY https://github.com/fmtlib/fmt
        GIT_TAG ${WL2_HTTP_FMT_TAG} GIT_SHALLOW TRUE)
    FetchContent_Declare(wl2http_expected_lite
        GIT_REPOSITORY https://github.com/martinmoene/expected-lite
        GIT_TAG ${WL2_HTTP_EXPECTED_LITE_TAG} GIT_SHALLOW TRUE)
    # llhttp: build only the static library.
    set(BUILD_SHARED_LIBS OFF)
    set(BUILD_STATIC_LIBS ON)
    FetchContent_Declare(wl2http_llhttp
        GIT_REPOSITORY https://github.com/nodejs/llhttp
        GIT_TAG ${WL2_HTTP_LLHTTP_TAG} GIT_SHALLOW TRUE)
    FetchContent_MakeAvailable(wl2http_fmt wl2http_expected_lite wl2http_llhttp)
    _wl2_http_silence(fmt)
    _wl2_http_silence(llhttp_static)

    # --- RESTinio (header-only; CMake root is dev/restinio) -----------------
    set(RESTINIO_ASIO_SOURCE standalone)
    set(asio_INCLUDE_DIRS "${_asio_include_dir}")
    set(RESTINIO_INSTALL OFF)
    FetchContent_Declare(wl2http_restinio
        GIT_REPOSITORY https://github.com/Stiffstream/restinio
        GIT_TAG ${WL2_HTTP_RESTINIO_TAG} GIT_SHALLOW TRUE
        SOURCE_SUBDIR dev/restinio)
    FetchContent_MakeAvailable(wl2http_restinio)

    if(NOT TARGET restinio::restinio)
        message(STATUS "restinio target not created; wl2_restinio module disabled")
        return()
    endif()

    # Optional HTTPS/TLS: RESTinio's TLS support needs OpenSSL. When it is not
    # found the module still builds as a cleartext server.
    set(_tls_libs "")
    set(_tls_defs "")
    if(WL2_HTTP_TLS)
        find_package(OpenSSL)
        if(OpenSSL_FOUND)
            set(_tls_libs "OpenSSL::SSL;OpenSSL::Crypto")
            set(_tls_defs "WL2_HTTP_TLS=1")
            message(STATUS "wl2:http HTTPS/TLS enabled (OpenSSL ${OPENSSL_VERSION})")
        else()
            message(STATUS "wl2:http HTTPS/TLS requested but OpenSSL not found; building cleartext only")
        endif()
    endif()

    # zlib powers gzip response compression (always available; small).
    find_package(ZLIB REQUIRED)

    # Single INTERFACE target carrying everything a consumer needs. Asio headers
    # are marked SYSTEM so their -Wall/-Wextra diagnostics do not reach -Werror.
    if(NOT TARGET wl2_restinio_dep)
        find_package(Threads REQUIRED)
        add_library(wl2_restinio_dep INTERFACE IMPORTED GLOBAL)
        set_target_properties(wl2_restinio_dep PROPERTIES
            INTERFACE_LINK_LIBRARIES "restinio::restinio;fmt::fmt;Threads::Threads;ZLIB::ZLIB;${_tls_libs}"
            INTERFACE_SYSTEM_INCLUDE_DIRECTORIES "${_asio_include_dir}"
            INTERFACE_INCLUDE_DIRECTORIES "${_asio_include_dir}"
            INTERFACE_COMPILE_DEFINITIONS "ASIO_STANDALONE;ASIO_NO_DEPRECATED;${_tls_defs}")
    endif()

    set(WL2_HAVE_RESTINIO TRUE PARENT_SCOPE)
    set(WL2_RESTINIO_TARGET wl2_restinio_dep PARENT_SCOPE)
    message(STATUS "Using fetched RESTinio ${WL2_HTTP_RESTINIO_TAG}")
endfunction()
