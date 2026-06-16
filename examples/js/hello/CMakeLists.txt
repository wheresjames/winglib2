if(NOT TARGET wl2_core)
    cmake_minimum_required(VERSION 3.24)
    project(wl2_hello_js LANGUAGES CXX)
    find_package(winglib2 CONFIG REQUIRED)
    enable_testing()
    set(WL2_BUILD_TESTING ON)
endif()

wl2_add_javascript_executable(wl2_hello_js
    ROOT ${CMAKE_CURRENT_SOURCE_DIR}
    ENTRY hello.js)

if(WL2_BUILD_TESTING)
    add_test(NAME examples.js.hello_standalone
        COMMAND wl2_hello_js)
    set_tests_properties(examples.js.hello_standalone PROPERTIES
        LABELS "examples;js"
        TIMEOUT 10)
endif()
