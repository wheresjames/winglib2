# examples/js/3d-dashboard
#
# A cross-module example: a Slint dashboard window hosting a live wl2:3d viewport.
# It builds two ways, like the other Winglib2 examples:
#   * in-tree, added with add_subdirectory() from cmake/WL2Examples.cmake, and
#   * standalone, configured against an installed Winglib2 package.
# It needs the wl2_3d, wl2_slint, and wl2_membus static modules; when any is
# unavailable the example is skipped rather than failing the configure.
if(NOT TARGET wl2_core)
    cmake_minimum_required(VERSION 3.24)
    project(wl2_3d_dashboard_example LANGUAGES CXX)
    find_package(winglib2 CONFIG REQUIRED)
    set(WL2_CORE_TARGET winglib2::wl2_core)
    foreach(_dep wl2_3d_static wl2_slint_static wl2_membus_static)
        if(NOT TARGET winglib2::${_dep})
            message(FATAL_ERROR "This example requires Winglib2 installed with ${_dep}")
        endif()
    endforeach()
    set(WL2_CORE_TARGET winglib2::wl2_core)
    set(WL2_3D_STATIC_TARGET winglib2::wl2_3d_static)
    set(WL2_SLINT_STATIC_TARGET winglib2::wl2_slint_static)
    set(WL2_MEMBUS_STATIC_TARGET winglib2::wl2_membus_static)
else()
    if(NOT (TARGET wl2_3d_static AND TARGET wl2_slint_static AND TARGET wl2_membus_static))
        # One of the modules is disabled in this build; skip the example quietly.
        return()
    endif()
    set(WL2_CORE_TARGET wl2_core)
    set(WL2_3D_STATIC_TARGET wl2_3d_static)
    set(WL2_SLINT_STATIC_TARGET wl2_slint_static)
    set(WL2_MEMBUS_STATIC_TARGET wl2_membus_static)
endif()

add_executable(wl2_3d_dashboard_example main.cpp)
target_link_libraries(wl2_3d_dashboard_example PRIVATE
    ${WL2_CORE_TARGET}
    ${WL2_3D_STATIC_TARGET}
    ${WL2_SLINT_STATIC_TARGET}
    ${WL2_MEMBUS_STATIC_TARGET})

# Embed the script, UI markup, and glTF asset as wl2:/3d-dashboard resources so
# the executable carries everything it needs with no runtime file access.
wl2_add_embedded_resources(wl2_3d_dashboard_example
    ROOT ${CMAKE_CURRENT_SOURCE_DIR}
    PREFIX wl2:/3d-dashboard
    STORE_FILES scripts/main.js ui/app.slint assets/site.gltf)

# Display-free smoke test: --compile-only compiles the UI and runs the full
# engine feature demo with assertions, opening no window. Safe for CI on any
# build (renderer on or off).
if(WL2_BUILD_TESTING)
    add_test(NAME examples.js.3d_dashboard_compile
        COMMAND wl2_3d_dashboard_example --compile-only)
    set_tests_properties(examples.js.3d_dashboard_compile PROPERTIES
        LABELS "examples;3d;slint;membus;smoke"
        TIMEOUT 30)
endif()

# Windowed self-test: opens a real window, drives a slice of the controls from a
# UI timer, and quits. Needs a display, so it is registered only when
# WL2_SLINT_DISPLAY_TESTS is ON. Run under a virtual framebuffer, e.g.
#   xvfb-run -a ctest -R examples.js.3d_dashboard$
if(WL2_BUILD_TESTING AND WL2_SLINT_DISPLAY_TESTS)
    add_test(NAME examples.js.3d_dashboard
        COMMAND wl2_3d_dashboard_example --selftest)
    set_tests_properties(examples.js.3d_dashboard PROPERTIES
        LABELS "examples;3d;slint;display;smoke"
        ENVIRONMENT "SLINT_BACKEND=winit-software"
        TIMEOUT 45)
endif()
