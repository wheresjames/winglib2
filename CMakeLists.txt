
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
# Build setup: install paths, C++ standard, and project options

include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

# C++20, no compiler extensions, position-independent (needed for shared modules).
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

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
add_subdirectory(app/wl2)

# Examples, tests, docs, and the installed CMake package. Order matters:
# packaging runs last so module/example install targets are already registered.
include(cmake/WL2Examples.cmake)
include(cmake/WL2Testing.cmake)
include(cmake/WL2Docs.cmake)
include(cmake/WL2Packaging.cmake)
