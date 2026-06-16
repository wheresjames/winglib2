# curl is used only by the wl2_curl module, which includes this file and calls
# wl2_find_curl() from its own scope. The guard keeps the helper definitions
# idempotent if the file is included more than once in a single configure.
include_guard(GLOBAL)

if(DEFINED PROJECT_SOURCE_DIR AND EXISTS "${PROJECT_SOURCE_DIR}/cmake/deps/WL2Dependency.cmake")
    include("${PROJECT_SOURCE_DIR}/cmake/deps/WL2Dependency.cmake")
else()
    include("${CMAKE_CURRENT_LIST_DIR}/../../deps/WL2Dependency.cmake")
endif()
include(ExternalProject)

function(wl2_fetch_curl)
    file(MAKE_DIRECTORY
        "${WL2_CURL_ROOT}/include"
        "${WL2_CURL_ROOT}/${CMAKE_INSTALL_LIBDIR}")

    set(_curl_library "${WL2_CURL_ROOT}/${CMAKE_INSTALL_LIBDIR}/${CMAKE_STATIC_LIBRARY_PREFIX}curl${CMAKE_STATIC_LIBRARY_SUFFIX}")
    set(_curl_cmake_args
        -DCMAKE_INSTALL_PREFIX=${WL2_CURL_ROOT}
        -DCMAKE_POSITION_INDEPENDENT_CODE=ON
        -DBUILD_SHARED_LIBS=OFF
        -DBUILD_STATIC_LIBS=ON
        -DBUILD_CURL_EXE=OFF
        -DBUILD_EXAMPLES=OFF
        -DBUILD_TESTING=OFF
        -DBUILD_LIBCURL_DOCS=OFF
        -DBUILD_MISC_DOCS=OFF
        -DENABLE_CURL_MANUAL=OFF
        -DCURL_ENABLE_SSL=OFF
        -DCURL_USE_LIBPSL=OFF
        -DCURL_ZLIB=OFF
        -DCURL_BROTLI=OFF
        -DCURL_ZSTD=OFF
        -DUSE_NGHTTP2=OFF
        -DUSE_LIBIDN2=OFF
        -DCURL_USE_LIBSSH2=OFF
        -DCURL_DISABLE_LDAP=ON
        -DCURL_DISABLE_LDAPS=ON)
    if(CMAKE_BUILD_TYPE)
        list(APPEND _curl_cmake_args -DCMAKE_BUILD_TYPE=${CMAKE_BUILD_TYPE})
    endif()
    if(CMAKE_TOOLCHAIN_FILE)
        list(APPEND _curl_cmake_args -DCMAKE_TOOLCHAIN_FILE=${CMAKE_TOOLCHAIN_FILE})
    endif()
    if(WL2_CURL_FETCH_CMAKE_ARGS)
        separate_arguments(_wl2_curl_extra_args NATIVE_COMMAND "${WL2_CURL_FETCH_CMAKE_ARGS}")
        list(APPEND _curl_cmake_args ${_wl2_curl_extra_args})
    endif()

    ExternalProject_Add(wl2_curl_dependency_project
        URL "${WL2_CURL_URL}"
        URL_HASH "${WL2_CURL_URL_HASH}"
        DOWNLOAD_EXTRACT_TIMESTAMP TRUE
        PREFIX "${WL2_DEPS_ROOT}/_build/curl"
        CMAKE_ARGS ${_curl_cmake_args}
        BUILD_BYPRODUCTS "${_curl_library}")

    add_library(CURL::libcurl STATIC IMPORTED GLOBAL)
    set_target_properties(CURL::libcurl PROPERTIES
        IMPORTED_LOCATION "${_curl_library}"
        INTERFACE_INCLUDE_DIRECTORIES "${WL2_CURL_ROOT}/include"
        INTERFACE_COMPILE_DEFINITIONS CURL_STATICLIB)
    add_dependencies(CURL::libcurl wl2_curl_dependency_project)
endfunction()

function(_wl2_resolve_curl out_found out_target out_is_system out_provider_used)
    if(WL2_CURL_PROVIDER STREQUAL "local" OR WL2_CURL_PROVIDER STREQUAL "auto")
        find_package(CURL CONFIG QUIET
            PATHS "${WL2_CURL_ROOT}"
            NO_DEFAULT_PATH)
        if(TARGET CURL::libcurl)
            set(${out_found} TRUE PARENT_SCOPE)
            set(${out_target} CURL::libcurl PARENT_SCOPE)
            set(${out_provider_used} local PARENT_SCOPE)
            set(${out_is_system} FALSE PARENT_SCOPE)
            message(STATUS "Using local curl package from ${WL2_CURL_ROOT}")
            return()
        endif()

        find_path(WL2_CURL_INCLUDE_DIR
            NAMES curl/curl.h
            PATHS "${WL2_CURL_ROOT}/include"
            NO_DEFAULT_PATH)
        find_library(WL2_CURL_LIBRARY
            NAMES curl libcurl
            PATHS "${WL2_CURL_ROOT}/lib" "${WL2_CURL_ROOT}/lib64"
            NO_DEFAULT_PATH)
        if(WL2_CURL_INCLUDE_DIR AND WL2_CURL_LIBRARY)
            add_library(CURL::libcurl UNKNOWN IMPORTED GLOBAL)
            set_target_properties(CURL::libcurl PROPERTIES
                IMPORTED_LOCATION "${WL2_CURL_LIBRARY}"
                INTERFACE_INCLUDE_DIRECTORIES "${WL2_CURL_INCLUDE_DIR}")
            set(${out_found} TRUE PARENT_SCOPE)
            set(${out_target} CURL::libcurl PARENT_SCOPE)
            set(${out_provider_used} local PARENT_SCOPE)
            set(${out_is_system} FALSE PARENT_SCOPE)
            message(STATUS "Using local curl include=${WL2_CURL_INCLUDE_DIR} lib=${WL2_CURL_LIBRARY}")
            return()
        endif()

        if(WL2_CURL_PROVIDER STREQUAL "local")
            message(FATAL_ERROR "WL2_CURL_PROVIDER=local but curl was not found at ${WL2_CURL_ROOT}")
        endif()
    endif()

    if(WL2_CURL_PROVIDER STREQUAL "fetch" OR (WL2_CURL_PROVIDER STREQUAL "auto" AND WL2_FETCH_DEPS))
        if(WL2_FETCH_DEPS)
            wl2_fetch_curl()
            set(${out_found} TRUE PARENT_SCOPE)
            set(${out_target} CURL::libcurl PARENT_SCOPE)
            set(${out_provider_used} fetch PARENT_SCOPE)
            set(${out_is_system} FALSE PARENT_SCOPE)
            message(STATUS "Using fetched curl ${WL2_CURL_VERSION}; local install will be staged at ${WL2_CURL_ROOT}")
            return()
        elseif(WL2_CURL_PROVIDER STREQUAL "fetch")
            message(FATAL_ERROR "WL2_CURL_PROVIDER=fetch but WL2_FETCH_DEPS=OFF")
        endif()
    endif()

    if(WL2_CURL_PROVIDER STREQUAL "package" OR WL2_CURL_PROVIDER STREQUAL "auto")
        wl2_dependency_note_package(CURL "${WL2_CURL_PROVIDER}")
        find_package(CURL QUIET)
        if(TARGET CURL::libcurl)
            set(${out_found} TRUE PARENT_SCOPE)
            set(${out_target} CURL::libcurl PARENT_SCOPE)
            set(${out_provider_used} package PARENT_SCOPE)
            set(${out_is_system} TRUE PARENT_SCOPE)
            message(STATUS "Using package/system curl")
            return()
        endif()
        if(WL2_CURL_PROVIDER STREQUAL "package")
            message(FATAL_ERROR "WL2_CURL_PROVIDER=package but CURL::libcurl was not found")
        endif()
    endif()

    message(STATUS "curl not found; wl2_curl module disabled")
endfunction()

function(wl2_find_curl)
    include("${CMAKE_CURRENT_LIST_DIR}/options.cmake" OPTIONAL)
    wl2_declare_dependency(CURL
        ROOT_DEFAULT "${WL2_DEPS_ROOT}/curl"
        FIND_CALLBACK _wl2_resolve_curl)
    set(WL2_HAVE_CURL "${WL2_CURL_FOUND}" PARENT_SCOPE)
    set(WL2_CURL_TARGET "${WL2_CURL_TARGET}" PARENT_SCOPE)
    set(WL2_CURL_PROVIDER_USED "${WL2_CURL_PROVIDER_USED}" PARENT_SCOPE)
    set(WL2_CURL_IS_SYSTEM "${WL2_CURL_IS_SYSTEM}" PARENT_SCOPE)
endfunction()
