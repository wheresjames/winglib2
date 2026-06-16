# examples/modules/wl2_echo
#
# A complete, dependency-free reference for an out-of-tree native module. It
# builds the same way in two situations:
#   * in-tree, added with add_subdirectory() from the Winglib2 build, and
#   * standalone, configured against an installed Winglib2 package with
#     find_package(winglib2 CONFIG REQUIRED).
if(NOT TARGET wl2_core)
    cmake_minimum_required(VERSION 3.24)
    project(wl2_echo VERSION 0.1.0 LANGUAGES CXX)
    find_package(winglib2 CONFIG REQUIRED)
    enable_testing()
    set(WL2_BUILD_TESTING ON)
endif()

include(cmake/options.cmake)

if(NOT WL2_ENABLE_ECHO)
    return()
endif()

# Build both module targets for the example by default, so the static and
# dynamic shapes are always demonstrated.
set(WL2_BUILD_STATIC_MODULES ON)
set(WL2_BUILD_SHARED_MODULES ON)

if(TARGET wl2_core)
    set(_wl2_echo_core wl2_core)
    set(_wl2_echo_dynamic_includes "${PROJECT_SOURCE_DIR}/include")
else()
    set(_wl2_echo_core winglib2::wl2_core)
    set(_wl2_echo_dynamic_includes
        "$<TARGET_PROPERTY:winglib2::wl2_core,INTERFACE_INCLUDE_DIRECTORIES>")
endif()

# The dynamic module compiles its QuickJS factory but does not link QuickJS; the
# host runner resolves the JS engine symbols at load time. It needs the QuickJS
# headers and the WL2_HAVE_QUICKJS switch to build the working factory.
set(_wl2_echo_dynamic_defs)
if(TARGET quickjs)
    list(APPEND _wl2_echo_dynamic_includes
        "$<TARGET_PROPERTY:quickjs,INTERFACE_INCLUDE_DIRECTORIES>")
    list(APPEND _wl2_echo_dynamic_defs WL2_HAVE_QUICKJS=1)
endif()

# NO_INSTALL: an example module is never part of the installed winglib2 package.
# The dynamic target deliberately does not link wl2_core; like the in-tree
# modules it only needs the wl2 headers, and the host provides the symbols.
wl2_add_module(wl2_echo
    NO_INSTALL
    NO_DYNAMIC_LINK_LIBRARIES
    SOURCES src/wl2_echo.cpp
    PUBLIC_LINK_LIBRARIES ${_wl2_echo_core}
    DYNAMIC_INCLUDE_DIRS ${_wl2_echo_dynamic_includes}
    STATIC_PRIVATE_COMPILE_DEFINITIONS WL2_ECHO_STATIC_MODULE=1
    DYNAMIC_PRIVATE_COMPILE_DEFINITIONS ${_wl2_echo_dynamic_defs})

# Project-local JavaScript runner: a standalone executable that embeds the smoke
# script and links the static module.
wl2_add_javascript_executable(wl2_echo_js
    ROOT ${CMAKE_CURRENT_SOURCE_DIR}/js
    ENTRY main.js
    STORE_FILES main.js
    INCLUDES wl2_echo/wl2_echo.h
    STATIC_MODULES wl2_echo_register_module
    LINK_LIBRARIES wl2_echo_static)

if(WL2_BUILD_TESTING)
    add_executable(wl2_echo_tests test/wl2_echo_tests.cpp)
    target_link_libraries(wl2_echo_tests PRIVATE ${_wl2_echo_core} wl2_echo_static)
    add_test(NAME examples.modules.wl2_echo COMMAND wl2_echo_tests)
    set_tests_properties(examples.modules.wl2_echo PROPERTIES
        LABELS "examples;modules;echo;integration"
        TIMEOUT 15)

    add_test(NAME examples.modules.wl2_echo_standalone COMMAND wl2_echo_js)
    set_tests_properties(examples.modules.wl2_echo_standalone PROPERTIES
        LABELS "examples;modules;echo;js"
        TIMEOUT 10)
endif()
