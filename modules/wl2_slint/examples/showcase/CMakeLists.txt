# examples/showcase
#
# Builds a single statically-linked executable that runs a richer embedded
# Slint UI. This demonstrates the JavaScript-facing interpreter path with
# properties, structs, array models, callbacks with arguments, timers, and
# several standard widgets.
if(NOT TARGET wl2_core)
    cmake_minimum_required(VERSION 3.24)
    project(wl2_slint_showcase_example LANGUAGES CXX)
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

add_executable(wl2_slint_showcase_example main.cpp)
target_link_libraries(wl2_slint_showcase_example PRIVATE
    ${WL2_CORE_TARGET} ${WL2_SLINT_STATIC_TARGET})

wl2_add_embedded_resources(wl2_slint_showcase_example
    ROOT ${CMAKE_CURRENT_SOURCE_DIR}
    PREFIX wl2:/showcase
    STORE_FILES scripts/main.js ui/app.slint)

if(WL2_BUILD_TESTING)
    add_test(NAME examples.wl2_slint.showcase_compile
        COMMAND wl2_slint_showcase_example --compile-only)
    set_tests_properties(examples.wl2_slint.showcase_compile PROPERTIES
        LABELS "examples;slint;smoke"
        TIMEOUT 15)
endif()

if(WL2_BUILD_TESTING AND WL2_SLINT_DISPLAY_TESTS)
    add_test(NAME examples.wl2_slint.showcase
        COMMAND wl2_slint_showcase_example --selftest)
    set_tests_properties(examples.wl2_slint.showcase PROPERTIES
        LABELS "examples;slint;display;smoke"
        ENVIRONMENT "SLINT_BACKEND=winit-software"
        TIMEOUT 30)
endif()
