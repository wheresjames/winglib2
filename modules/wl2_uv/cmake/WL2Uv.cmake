include_guard(GLOBAL)

include(ExternalProject)

function(_wl2_uv_make_target include_dir library)
    if(NOT TARGET wl2_uv_dep)
        add_library(wl2_uv_dep UNKNOWN IMPORTED GLOBAL)
        set_target_properties(wl2_uv_dep PROPERTIES
            IMPORTED_LOCATION "${library}"
            INTERFACE_INCLUDE_DIRECTORIES "${include_dir}")
    endif()
endfunction()

function(_wl2_uv_fetch)
    set(_library
        "${WL2_UV_ROOT}/lib/${CMAKE_STATIC_LIBRARY_PREFIX}uv${CMAKE_STATIC_LIBRARY_SUFFIX}")
    set(_cmake_args
        -DCMAKE_INSTALL_PREFIX=${WL2_UV_ROOT}
        -DCMAKE_POSITION_INDEPENDENT_CODE=ON
        -DLIBUV_BUILD_SHARED=OFF
        -DLIBUV_BUILD_TESTS=OFF
        -DLIBUV_BUILD_BENCH=OFF
        -DBUILD_TESTING=OFF)
    if(CMAKE_BUILD_TYPE)
        list(APPEND _cmake_args -DCMAKE_BUILD_TYPE=${CMAKE_BUILD_TYPE})
    endif()
    if(CMAKE_TOOLCHAIN_FILE)
        list(APPEND _cmake_args -DCMAKE_TOOLCHAIN_FILE=${CMAKE_TOOLCHAIN_FILE})
    endif()

    ExternalProject_Add(wl2_uv_dependency_project
        URL "${WL2_UV_URL}"
        URL_HASH "${WL2_UV_URL_HASH}"
        DOWNLOAD_EXTRACT_TIMESTAMP TRUE
        PREFIX "${WL2_DEPS_ROOT}/_build/libuv"
        CMAKE_ARGS ${_cmake_args}
        BUILD_BYPRODUCTS "${_library}")

    file(MAKE_DIRECTORY "${WL2_UV_ROOT}/include")
    _wl2_uv_make_target("${WL2_UV_ROOT}/include" "${_library}")
    add_dependencies(wl2_uv_dep wl2_uv_dependency_project)
    find_package(Threads REQUIRED)
    set_property(TARGET wl2_uv_dep APPEND PROPERTY
        INTERFACE_LINK_LIBRARIES Threads::Threads "${CMAKE_DL_LIBS}")
    if(UNIX AND NOT APPLE)
        set_property(TARGET wl2_uv_dep APPEND PROPERTY INTERFACE_LINK_LIBRARIES m rt)
    endif()
endfunction()

function(_wl2_uv_find_in_prefix prefix out_found)
    find_path(WL2_UV_INCLUDE_DIR
        NAMES uv.h
        PATHS "${prefix}/include"
        NO_DEFAULT_PATH)
    find_library(WL2_UV_LIBRARY
        NAMES uv libuv
        PATHS "${prefix}/lib" "${prefix}/lib64"
        NO_DEFAULT_PATH)
    if(WL2_UV_INCLUDE_DIR AND WL2_UV_LIBRARY)
        _wl2_uv_make_target("${WL2_UV_INCLUDE_DIR}" "${WL2_UV_LIBRARY}")
        set(${out_found} TRUE PARENT_SCOPE)
    else()
        set(${out_found} FALSE PARENT_SCOPE)
    endif()
endfunction()

function(wl2_find_uv)
    include("${CMAKE_CURRENT_LIST_DIR}/options.cmake" OPTIONAL)

    set(WL2_HAVE_UV FALSE PARENT_SCOPE)
    set(WL2_UV_TARGET "" PARENT_SCOPE)

    if(WL2_UV_PROVIDER STREQUAL "off")
        wl2_dependency_note_result(uv DISABLED "WL2_UV_PROVIDER=off")
        return()
    endif()

    if(WL2_UV_PROVIDER STREQUAL "local" OR WL2_UV_PROVIDER STREQUAL "auto")
        if(WL2_UV_ROOT)
            _wl2_uv_find_in_prefix("${WL2_UV_ROOT}" _found)
            if(_found)
                set(WL2_HAVE_UV TRUE PARENT_SCOPE)
                set(WL2_UV_TARGET wl2_uv_dep PARENT_SCOPE)
                wl2_dependency_note_result(uv local "${WL2_UV_ROOT}")
                return()
            endif()
        endif()
        if(WL2_UV_PROVIDER STREQUAL "local")
            message(FATAL_ERROR "WL2_UV_PROVIDER=local but libuv was not found under WL2_UV_ROOT")
        endif()
    endif()

    if(WL2_UV_PROVIDER STREQUAL "fetch")
        _wl2_uv_fetch()
        set(WL2_HAVE_UV TRUE PARENT_SCOPE)
        set(WL2_UV_TARGET wl2_uv_dep PARENT_SCOPE)
        wl2_dependency_note_result(uv download "${WL2_UV_VERSION} ${WL2_UV_ROOT}")
        return()
    endif()

    if(WL2_UV_PROVIDER STREQUAL "package"
            OR WL2_UV_PROVIDER STREQUAL "auto")
        find_path(WL2_UV_SYSTEM_INCLUDE_DIR NAMES uv.h)
        find_library(WL2_UV_SYSTEM_LIBRARY NAMES uv libuv)
        if(WL2_UV_SYSTEM_INCLUDE_DIR AND WL2_UV_SYSTEM_LIBRARY)
            _wl2_uv_make_target("${WL2_UV_SYSTEM_INCLUDE_DIR}" "${WL2_UV_SYSTEM_LIBRARY}")
            set(WL2_HAVE_UV TRUE PARENT_SCOPE)
            set(WL2_UV_TARGET wl2_uv_dep PARENT_SCOPE)
            wl2_dependency_note_result(uv system "${WL2_UV_SYSTEM_LIBRARY}")
            return()
        endif()
        if(WL2_UV_PROVIDER STREQUAL "package")
            message(FATAL_ERROR "WL2_UV_PROVIDER=package but system libuv was not found")
        endif()
    endif()

    message(STATUS "libuv not found; wl2_uv module disabled")
    wl2_dependency_note_result(uv DISABLED "not found")
endfunction()
