include("${CMAKE_CURRENT_LIST_DIR}/WL2Dependency.cmake")

function(wl2_find_quickjs)
    if(TARGET quickjs)
        set(WL2_HAVE_QUICKJS TRUE PARENT_SCOPE)
        return()
    endif()

    wl2_dependency_normalize_provider(QUICKJS WL2_QUICKJS_PROVIDER)

    if(WL2_QUICKJS_PROVIDER STREQUAL "off")
        set(WL2_HAVE_QUICKJS FALSE PARENT_SCOPE)
        return()
    endif()

    set(_quickjs_roots)
    if(WL2_QUICKJS_ROOT)
        list(APPEND _quickjs_roots "${WL2_QUICKJS_ROOT}")
    endif()
    if(WL2_DEPS_ROOT)
        list(APPEND _quickjs_roots "${WL2_DEPS_ROOT}/quickjs")
    endif()

    if(NOT WL2_QUICKJS_PROVIDER STREQUAL "package" AND NOT WL2_QUICKJS_PROVIDER STREQUAL "fetch")
        foreach(_root IN LISTS _quickjs_roots)
            wl2_add_imported_static_library(quickjs quickjs.h libquickjs.a ROOT "${_root}" EXTRA_LIBRARIES m pthread dl)
            if(quickjs_FOUND)
                message(STATUS "Using local QuickJS from ${_root}")
                set(WL2_HAVE_QUICKJS TRUE PARENT_SCOPE)
                return()
            endif()
        endforeach()
        if(WL2_QUICKJS_PROVIDER STREQUAL "local")
            message(FATAL_ERROR "WL2_QUICKJS_PROVIDER=local but QuickJS was not found at ${WL2_DEPS_ROOT}/quickjs or WL2_QUICKJS_ROOT")
        endif()
    endif()

    if((WL2_QUICKJS_PROVIDER STREQUAL "fetch" OR WL2_QUICKJS_PROVIDER STREQUAL "auto") AND WL2_FETCH_DEPS)
        include(FetchContent)
        set(FETCHCONTENT_BASE_DIR "${WL2_DEPS_ROOT}/_fetch" CACHE PATH "FetchContent base directory" FORCE)

        message(STATUS "Fetching QuickJS ${WL2_QUICKJS_VERSION} from ${WL2_QUICKJS_URL}")
        FetchContent_Declare(
            quickjs_source
            URL "${WL2_QUICKJS_URL}"
            DOWNLOAD_EXTRACT_TIMESTAMP TRUE
        )
        FetchContent_Populate(quickjs_source)

        add_library(quickjs STATIC
            "${quickjs_source_SOURCE_DIR}/quickjs.c"
            "${quickjs_source_SOURCE_DIR}/libregexp.c"
            "${quickjs_source_SOURCE_DIR}/libunicode.c"
            "${quickjs_source_SOURCE_DIR}/cutils.c"
            "${quickjs_source_SOURCE_DIR}/dtoa.c")

        target_include_directories(quickjs
            PUBLIC
                $<BUILD_INTERFACE:${quickjs_source_SOURCE_DIR}>
                $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}/quickjs>)

        target_compile_definitions(quickjs
            PRIVATE
                _GNU_SOURCE
                CONFIG_VERSION="${WL2_QUICKJS_VERSION}")

        if(MSVC)
            target_compile_definitions(quickjs PRIVATE inline=__inline)
        endif()

        if(UNIX)
            target_link_libraries(quickjs INTERFACE m pthread dl)
        endif()

        set(_quickjs_install_root "${WL2_DEPS_ROOT}/quickjs")
        add_custom_command(TARGET quickjs POST_BUILD
            COMMAND "${CMAKE_COMMAND}" -E make_directory "${_quickjs_install_root}/include" "${_quickjs_install_root}/lib"
            COMMAND "${CMAKE_COMMAND}" -E copy_if_different
                "${quickjs_source_SOURCE_DIR}/quickjs.h"
                "${quickjs_source_SOURCE_DIR}/quickjs-libc.h"
                "${_quickjs_install_root}/include/"
            COMMAND "${CMAKE_COMMAND}" -E copy_if_different
                "$<TARGET_FILE:quickjs>"
                "${_quickjs_install_root}/lib/libquickjs.a"
            COMMENT "Installing QuickJS to ${_quickjs_install_root}"
            VERBATIM)

        message(STATUS "Using fetched QuickJS; local install will be staged at ${_quickjs_install_root}")
        set(WL2_HAVE_QUICKJS TRUE PARENT_SCOPE)
        return()
    elseif(WL2_QUICKJS_PROVIDER STREQUAL "fetch")
        message(FATAL_ERROR "WL2_QUICKJS_PROVIDER=fetch but WL2_FETCH_DEPS=OFF")
    endif()

    if(WL2_QUICKJS_PROVIDER STREQUAL "package" OR WL2_QUICKJS_PROVIDER STREQUAL "auto")
        wl2_dependency_note_package(QUICKJS "${WL2_QUICKJS_PROVIDER}")
        find_package(PkgConfig QUIET)
        if(PkgConfig_FOUND)
            pkg_check_modules(PC_QUICKJS QUIET quickjs)
        endif()

        find_path(QUICKJS_INCLUDE_DIR
            NAMES quickjs.h
            HINTS
                ${PC_QUICKJS_INCLUDE_DIRS})

        find_library(QUICKJS_LIBRARY
            NAMES quickjs qjs
            HINTS
                ${PC_QUICKJS_LIBRARY_DIRS})

        if(QUICKJS_INCLUDE_DIR AND QUICKJS_LIBRARY)
            add_library(quickjs UNKNOWN IMPORTED GLOBAL)
            set_target_properties(quickjs PROPERTIES
                IMPORTED_LOCATION "${QUICKJS_LIBRARY}"
                INTERFACE_INCLUDE_DIRECTORIES "${QUICKJS_INCLUDE_DIR}")
            target_link_libraries(quickjs INTERFACE m pthread dl)
            message(STATUS "Using package/system QuickJS include=${QUICKJS_INCLUDE_DIR} lib=${QUICKJS_LIBRARY}")
            set(WL2_HAVE_QUICKJS TRUE PARENT_SCOPE)
            return()
        endif()
        if(WL2_QUICKJS_PROVIDER STREQUAL "package")
            message(FATAL_ERROR "WL2_QUICKJS_PROVIDER=package but QuickJS development files were not found")
        endif()
    endif()

    message(STATUS "QuickJS development files not found; expected local install at ${WL2_DEPS_ROOT}/quickjs")
    set(WL2_HAVE_QUICKJS FALSE PARENT_SCOPE)
endfunction()
