
# Winglib2: C++ runtime that embeds JavaScript (QuickJS/V8) with a static or
# dynamic native module system. Top-level build: core library, the wl2 runner,
# built-in modules, examples, tests, docs, and the installed CMake package.
cmake_minimum_required(VERSION 3.24)

#-------------------------------------------------------------
# Project config
include(${CMAKE_SOURCE_DIR}/cmake/debug.cmake)
include(${CMAKE_SOURCE_DIR}/cmake/readProjectConfig.cmake)

# PROJECT.txt is the preferred source of project metadata. readProjectConfig turns
# each "key value" line into an APP<KEY> variable (e.g. version -> APPVERSION).
# In-tree builds can omit it and fall back to source-root defaults; packaged or
# out-of-tree projects should provide PROJECT.txt so missing metadata fails here.
set(_wl2_project_config "${CMAKE_SOURCE_DIR}/PROJECT.txt")
set(_wl2_has_project_config OFF)
if(EXISTS "${_wl2_project_config}")
    set(_wl2_has_project_config ON)
    readProjectConfig("${_wl2_project_config}")
else()
    get_filename_component(APPNAME "${CMAKE_SOURCE_DIR}" NAME)
    set(APPVERSION "0.0.0")
    set(APPDESCRIPTION "")
    set(APPURL "")
    string(TIMESTAMP APPBUILD "%Y.%m.%d.%H%M")
endif()
dump_cmake_variables(APP)

# Validate required metadata up front so a missing/typo'd key fails here with a
# clear message instead of deep inside project()/package generation.
set(_wl2_required_project_fields APPNAME APPVERSION)
if(_wl2_has_project_config)
    list(APPEND _wl2_required_project_fields APPDESCRIPTION APPURL)
endif()
foreach(_wl2_required IN LISTS _wl2_required_project_fields)
    if(NOT DEFINED ${_wl2_required} OR "${${_wl2_required}}" STREQUAL "")
        message(FATAL_ERROR
            "PROJECT.txt is missing a required value (expected to set ${_wl2_required})")
    endif()
endforeach()
if(NOT APPVERSION MATCHES "^[0-9]+\\.[0-9]+\\.[0-9]+$")
    message(FATAL_ERROR "PROJECT.txt version must be MAJOR.MINOR.PATCH, got '${APPVERSION}'")
endif()

project("${APPNAME}"
        VERSION "${APPVERSION}"
        DESCRIPTION "${APPDESCRIPTION}"
        HOMEPAGE_URL "${APPURL}"
        LANGUAGES C CXX
    )

# Build metadata is exposed to C++ through a generated header derived from
# PROJECT.txt rather than global -D defines. This keeps the values scoped to
# consumers that include the header (no full-tree rebuild when the build stamp
# changes) and avoids leaking macros into bundled third-party dependencies.
# Downstream CMake should use the standard PROJECT_* variables.
configure_file(
    "${CMAKE_SOURCE_DIR}/cmake/winglib2_build_info.h.in"
    "${CMAKE_BINARY_DIR}/generated/winglib2/build_info.h"
    @ONLY)

# Show compiler info
message(STATUS " [-] CMAKE_C_COMPILER=${CMAKE_C_COMPILER}")
message(STATUS " [-] CMAKE_CXX_COMPILER=${CMAKE_CXX_COMPILER}")
message(STATUS " [-] CMAKE_LINKER=${CMAKE_LINKER}")
message(STATUS " [-] CMAKE_CXX_FLAGS_RELEASE=${CMAKE_CXX_FLAGS_RELEASE}")
message(STATUS " [-] CMAKE_BUILD_TYPE=${CMAKE_BUILD_TYPE}")
message(STATUS " [-] CMAKE_CXX_STANDARD=${CMAKE_CXX_STANDARD}")

#-------------------------------------------------------------
# Compiler cache (ccache)
#
# The test suite builds winglib2 from scratch several times: the out-of-tree
# module, linkage, and packaging tests each configure this same project into a
# throwaway build tree and recompile the identical core sources (notably the
# large quickjs.c translation unit). ccache turns those repeated compiles into
# cache hits, which is by far the biggest lever on total test time.
#
# Enabling it here means every one of those nested builds inherits the launcher
# automatically, since they all configure MAIN_SOURCE_DIR. A wrapper pins the
# settings that let a single cache be shared across the distinct build trees:
# CCACHE_NOHASHDIR keeps the differing build-directory paths (baked into
# RelWithDebInfo debug info) out of the hash, which would otherwise force a miss
# for every object in every separate tree.
option(WL2_USE_CCACHE "Accelerate compilation with ccache when it is available" ON)
if(WL2_USE_CCACHE AND NOT CMAKE_CXX_COMPILER_LAUNCHER AND NOT CMAKE_C_COMPILER_LAUNCHER)
    find_program(WL2_CCACHE_PROGRAM ccache)
    if(WL2_CCACHE_PROGRAM)
        if(CMAKE_HOST_UNIX)
            set(_wl2_ccache_launcher "${CMAKE_BINARY_DIR}/wl2-ccache-launcher.sh")
            file(WRITE "${_wl2_ccache_launcher}"
                "#!/bin/sh\n"
                "export CCACHE_NOHASHDIR=1\n"
                "export CCACHE_SLOPPINESS=time_macros,include_file_mtime,include_file_ctime,pch_defines\n"
                "exec \"${WL2_CCACHE_PROGRAM}\" \"$@\"\n")
            file(CHMOD "${_wl2_ccache_launcher}"
                PERMISSIONS OWNER_READ OWNER_WRITE OWNER_EXECUTE GROUP_READ GROUP_EXECUTE WORLD_READ WORLD_EXECUTE)
        else()
            set(_wl2_ccache_launcher "${WL2_CCACHE_PROGRAM}")
        endif()
        set(CMAKE_C_COMPILER_LAUNCHER "${_wl2_ccache_launcher}" CACHE STRING "" FORCE)
        set(CMAKE_CXX_COMPILER_LAUNCHER "${_wl2_ccache_launcher}" CACHE STRING "" FORCE)
        message(STATUS " [-] ccache enabled: ${WL2_CCACHE_PROGRAM}")
    else()
        message(STATUS " [-] ccache not found; compiling without a compiler cache")
    endif()
endif()


#-------------------------------------------------------------
# Build setup: install paths, C++ standard, and project options

include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

# C++20, no compiler extensions, position-independent (needed for shared modules).
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

# Keep project outputs stable before any fetched subproject is added. Some
# dependencies set CMAKE_*_OUTPUT_DIRECTORY in the cache when these are unset.
if(NOT DEFINED CMAKE_RUNTIME_OUTPUT_DIRECTORY)
    set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin" CACHE PATH "Runtime output directory")
endif()
if(NOT DEFINED CMAKE_LIBRARY_OUTPUT_DIRECTORY)
    set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib" CACHE PATH "Library output directory")
endif()
if(NOT DEFINED CMAKE_ARCHIVE_OUTPUT_DIRECTORY)
    set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib" CACHE PATH "Archive output directory")
endif()

# Project options, then pick and validate the JavaScript engine backend.
include(cmake/WL2Options.cmake)
if(WL2_ENABLE_V8)
    set(WL2_JS_ENGINE "v8" CACHE STRING "JavaScript engine backend: quickjs or v8" FORCE)
endif()
if(NOT WL2_JS_ENGINE STREQUAL "quickjs" AND NOT WL2_JS_ENGINE STREQUAL "v8")
    message(FATAL_ERROR "WL2_JS_ENGINE must be 'quickjs' or 'v8'")
endif()

# Helper modules: CTest, dependency target paths, embedded resources, and the
# module registry (wl2_add_module / builtin module helpers).
include(CTest)
include(cmake/WL2TargetPath.cmake)
include(cmake/WL2Resources.cmake)
include(cmake/WL2Modules.cmake)

# External dependency discovery/fetch.
include(cmake/WL2Dependencies.cmake)

# Core runtime library (wl2_core target).
include(cmake/WL2CoreLibrary.cmake)

# Built-in modules register themselves through the module registry; the wl2
# runner (app/wl2) links the enabled static ones from that registry.
wl2_add_modules(DIRECTORY modules)
wl2_dependency_print_summary()
add_subdirectory(app/wl2)

# Examples, tests, docs, and the installed CMake package. Order matters:
# packaging runs last so module/example install targets are already registered.
include(cmake/WL2Examples.cmake)
include(cmake/WL2Testing.cmake)
include(cmake/WL2Docs.cmake)
include(cmake/WL2Packaging.cmake)
