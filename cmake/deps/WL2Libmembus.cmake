include("${CMAKE_CURRENT_LIST_DIR}/WL2Dependency.cmake")

function(wl2_find_libmembus)
    set(WL2_HAVE_LIBMEMBUS FALSE PARENT_SCOPE)
    set(WL2_LIBMEMBUS_TARGET "" PARENT_SCOPE)
    wl2_dependency_normalize_provider(LIBMEMBUS WL2_LIBMEMBUS_PROVIDER)

    if(NOT WL2_ENABLE_LIBMEMBUS OR WL2_LIBMEMBUS_PROVIDER STREQUAL "off")
        message(STATUS "libmembus integration disabled")
        return()
    endif()

    if(WL2_LIBMEMBUS_PROVIDER STREQUAL "fetch")
        wl2_fetch_libmembus(_wl2_fetched_libmembus_root)
        if(_wl2_fetched_libmembus_root)
            set(WL2_LIBMEMBUS_ROOT "${_wl2_fetched_libmembus_root}" CACHE PATH "Path to a libmembus checkout" FORCE)
            set(WL2_LIBMEMBUS_PROVIDER "local" CACHE STRING "libmembus provider: auto, local, package, fetch, or off" FORCE)
            set(WL2_LIBMEMBUS_PROVIDER "local")
        else()
            message(FATAL_ERROR "WL2_LIBMEMBUS_PROVIDER=fetch could not stage v${WL2_LIBMEMBUS_TARGET_VERSION}")
        endif()
    endif()

    if(WL2_LIBMEMBUS_PROVIDER STREQUAL "auto"
            AND WL2_FETCH_DEPS
            AND NOT EXISTS "${WL2_LIBMEMBUS_ROOT}/include/libmembus.h")
        message(STATUS "libmembus target-local source not found at ${WL2_LIBMEMBUS_ROOT}; fetching target source")
        wl2_fetch_libmembus(_wl2_fetched_libmembus_root)
        if(_wl2_fetched_libmembus_root)
            set(WL2_LIBMEMBUS_ROOT "${_wl2_fetched_libmembus_root}" CACHE PATH "Path to a libmembus checkout" FORCE)
        endif()
    endif()

    if(WL2_LIBMEMBUS_PROVIDER STREQUAL "local" OR WL2_LIBMEMBUS_PROVIDER STREQUAL "auto")
        if(EXISTS "${WL2_LIBMEMBUS_ROOT}/include/libmembus.h")
            if(WL2_LIBMEMBUS_PROVIDER STREQUAL "auto"
                    AND WL2_FETCH_DEPS
                    AND NOT EXISTS "${WL2_LIBMEMBUS_ROOT}/include/libmembus/memcmd.h")
                message(STATUS "libmembus local source is older than v${WL2_LIBMEMBUS_TARGET_VERSION}; fetching target source")
                wl2_fetch_libmembus(_wl2_fetched_libmembus_root)
                if(_wl2_fetched_libmembus_root)
                    set(WL2_LIBMEMBUS_ROOT "${_wl2_fetched_libmembus_root}" CACHE PATH "Path to a libmembus checkout" FORCE)
                endif()
            endif()

            set(_wl2_libmembus_sources
                ${WL2_LIBMEMBUS_ROOT}/source/cpp/memaud.cpp
                ${WL2_LIBMEMBUS_ROOT}/source/cpp/memmap.cpp
                ${WL2_LIBMEMBUS_ROOT}/source/cpp/memmsg.cpp
                ${WL2_LIBMEMBUS_ROOT}/source/cpp/memvid.cpp)

            foreach(_optional_source memcmd memkv select sys)
                if(EXISTS "${WL2_LIBMEMBUS_ROOT}/source/cpp/${_optional_source}.cpp")
                    list(APPEND _wl2_libmembus_sources
                        ${WL2_LIBMEMBUS_ROOT}/source/cpp/${_optional_source}.cpp)
                endif()
            endforeach()

            add_library(wl2_libmembus_dependency STATIC ${_wl2_libmembus_sources})
            target_include_directories(wl2_libmembus_dependency
                PUBLIC
                    $<BUILD_INTERFACE:${WL2_LIBMEMBUS_ROOT}/include>)

            if(EXISTS "${WL2_LIBMEMBUS_ROOT}/include/libmembus/memcmd.h")
                target_compile_definitions(wl2_libmembus_dependency PUBLIC WL2_LIBMEMBUS_HAS_1_2_SURFACE=1)
                if(POLICY CMP0167)
                    cmake_policy(PUSH)
                    cmake_policy(SET CMP0167 OLD)
                endif()
                find_package(Boost QUIET COMPONENTS stacktrace_backtrace)
                if(POLICY CMP0167)
                    cmake_policy(POP)
                endif()
                if(TARGET Boost::stacktrace_backtrace)
                    target_link_libraries(wl2_libmembus_dependency PUBLIC Boost::stacktrace_backtrace)
                else()
                    message(STATUS "libmembus v${WL2_LIBMEMBUS_TARGET_VERSION} surface detected; Boost::stacktrace_backtrace was not found")
                endif()
            else()
                target_compile_definitions(wl2_libmembus_dependency PUBLIC WL2_LIBMEMBUS_HAS_1_2_SURFACE=0)
                message(STATUS "libmembus local source is older than the v${WL2_LIBMEMBUS_TARGET_VERSION} target surface")
            endif()

            set(WL2_HAVE_LIBMEMBUS TRUE PARENT_SCOPE)
            set(WL2_LIBMEMBUS_TARGET wl2_libmembus_dependency PARENT_SCOPE)
            message(STATUS "libmembus integration enabled from local source: ${WL2_LIBMEMBUS_ROOT}")
            return()
        elseif(WL2_LIBMEMBUS_PROVIDER STREQUAL "local")
            message(FATAL_ERROR "WL2_LIBMEMBUS_PROVIDER=local but libmembus was not found at ${WL2_LIBMEMBUS_ROOT}")
        endif()
    endif()

    if(WL2_LIBMEMBUS_PROVIDER STREQUAL "package" OR WL2_LIBMEMBUS_PROVIDER STREQUAL "auto")
        wl2_dependency_note_package(LIBMEMBUS "${WL2_LIBMEMBUS_PROVIDER}")
        find_package(libmembus ${WL2_LIBMEMBUS_TARGET_VERSION} CONFIG QUIET)
        if(TARGET libmembus::libmembus)
            add_library(wl2_libmembus_dependency INTERFACE)
            target_link_libraries(wl2_libmembus_dependency INTERFACE libmembus::libmembus)
            target_compile_definitions(wl2_libmembus_dependency INTERFACE WL2_LIBMEMBUS_HAS_1_2_SURFACE=1)
            set(WL2_HAVE_LIBMEMBUS TRUE PARENT_SCOPE)
            set(WL2_LIBMEMBUS_TARGET wl2_libmembus_dependency PARENT_SCOPE)
            message(STATUS "libmembus integration enabled from package: target libmembus::libmembus")
            return()
        elseif(WL2_LIBMEMBUS_PROVIDER STREQUAL "package")
            message(FATAL_ERROR "WL2_LIBMEMBUS_PROVIDER=package but libmembus ${WL2_LIBMEMBUS_TARGET_VERSION} was not found")
        endif()
    endif()

    message(STATUS "libmembus not found; shared-memory wrappers will report unavailable")
endfunction()

function(wl2_fetch_libmembus out_var)
    set(${out_var} "" PARENT_SCOPE)
    if(NOT WL2_FETCH_DEPS)
        message(STATUS "libmembus fetch provider requested but WL2_FETCH_DEPS=OFF")
        return()
    endif()

    set(_archive "${WL2_DEPS_ROOT}/src/libmembus-v${WL2_LIBMEMBUS_TARGET_VERSION}.tar.gz")
    set(_source_root "${WL2_DEPS_ROOT}/src/libmembus-${WL2_LIBMEMBUS_TARGET_VERSION}")
    set(_url "https://github.com/wheresjames/libmembus/archive/refs/tags/v${WL2_LIBMEMBUS_TARGET_VERSION}.tar.gz")

    if(NOT EXISTS "${_source_root}/include/libmembus.h")
        file(MAKE_DIRECTORY "${WL2_DEPS_ROOT}/src")
        if(NOT EXISTS "${_archive}")
            message(STATUS "Fetching libmembus v${WL2_LIBMEMBUS_TARGET_VERSION} from ${_url}")
            file(DOWNLOAD
                "${_url}"
                "${_archive}"
                SHOW_PROGRESS
                STATUS _download_status)
            list(GET _download_status 0 _download_code)
            if(NOT _download_code EQUAL 0)
                list(GET _download_status 1 _download_message)
                message(STATUS "libmembus download failed: ${_download_message}")
                return()
            endif()
        endif()
        execute_process(
            COMMAND ${CMAKE_COMMAND} -E tar xzf "${_archive}"
            WORKING_DIRECTORY "${WL2_DEPS_ROOT}/src"
            RESULT_VARIABLE _extract_result)
        if(NOT _extract_result EQUAL 0)
            message(STATUS "libmembus extract failed: ${_extract_result}")
            return()
        endif()
    endif()

    if(EXISTS "${_source_root}/include/libmembus.h")
        set(${out_var} "${_source_root}" PARENT_SCOPE)
    endif()
endfunction()
