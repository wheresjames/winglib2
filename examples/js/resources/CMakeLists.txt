if(NOT TARGET wl2_core)
    cmake_minimum_required(VERSION 3.24)
    project(wl2_resources_js LANGUAGES CXX)
    find_package(winglib2 CONFIG REQUIRED)
    enable_testing()
    set(WL2_BUILD_TESTING ON)
endif()

# JavaScript-only counterpart to examples/cpp/resources: the same embedded
# resource tree (stored, compressed, recursive directory, excluded pattern,
# empty file) read from JavaScript through wl2.resources.
wl2_add_javascript_executable(wl2_resources_js
    MANIFEST ${CMAKE_CURRENT_SOURCE_DIR}/resources.yml)

if(WL2_BUILD_TESTING)
    add_test(NAME examples.js.resources_standalone
        COMMAND wl2_resources_js)
    set_tests_properties(examples.js.resources_standalone PROPERTIES
        LABELS "examples;js;resources"
        TIMEOUT 10)
endif()
