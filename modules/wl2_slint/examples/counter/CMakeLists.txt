# examples/counter
#
# Builds a single statically-linked executable that runs the embedded counter UI.
# Like the other Winglib2 examples it builds two ways:
#   * in-tree, added with add_subdirectory() from the module build, and
#   * standalone, configured against an installed Winglib2 package with
#     find_package(winglib2 CONFIG REQUIRED).
if(NOT TARGET wl2_core)
    cmake_minimum_required(VERSION 3.24)
    project(wl2_slint_counter_example LANGUAGES CXX)
    find_package(winglib2 CONFIG REQUIRED)
    set(WL2_CORE_TARGET winglib2::wl2_core)
    if(NOT TARGET winglib2::wl2_slint_static)
        message(FATAL_ERROR "This example requires Winglib2 installed with wl2_slint_static")
    endif()
    set(WL2_SLINT_STATIC_TARGET winglib2::wl2_slint_static)
else()
    set(WL2_CORE_TARGET wl2_core)
    set(WL2_SLINT_STATIC_TARGET wl2_slint_static)
endif()

add_executable(wl2_slint_counter_example main.cpp)
target_link_libraries(wl2_slint_counter_example PRIVATE
    ${WL2_CORE_TARGET} ${WL2_SLINT_STATIC_TARGET})

# Embed the script and UI markup as wl2:/counter/... resources so the executable
# carries its UI with no runtime file access.
wl2_add_embedded_resources(wl2_slint_counter_example
    ROOT ${CMAKE_CURRENT_SOURCE_DIR}
    PREFIX wl2:/counter
    STORE_FILES scripts/main.js ui/app.slint)

# Opt-in smoke test: opens a real window, so it needs a display. Registered only
# when WL2_SLINT_DISPLAY_TESTS is ON and labeled "display" for exclusion. Run it
# under a virtual framebuffer, e.g.
#   xvfb-run -a ctest -R examples.wl2_slint.counter
# The --selftest flag makes the app increment once and quit on its own.
if(WL2_BUILD_TESTING AND WL2_SLINT_DISPLAY_TESTS)
    add_test(NAME examples.wl2_slint.counter
        COMMAND wl2_slint_counter_example --selftest)
    set_tests_properties(examples.wl2_slint.counter PROPERTIES
        LABELS "examples;slint;display;smoke"
        ENVIRONMENT "SLINT_BACKEND=winit-software"
        TIMEOUT 30)
endif()
