# Static embedding example: links the wl2:membus static module archive into a
# host executable through direct static registration. The dynamic module store
# is not used here; see the manifest-driven script examples for dynamic loading.
if(NOT TARGET wl2_core)
    cmake_minimum_required(VERSION 3.24)
    project(wl2_foundations_example LANGUAGES CXX)
    find_package(winglib2 CONFIG REQUIRED)
    set(WL2_CORE_TARGET winglib2::wl2_core)
    if(NOT TARGET winglib2::wl2_membus_static)
        message(FATAL_ERROR "This example requires Winglib2 installed with wl2_membus_static")
    endif()
    set(WL2_MEMBUS_TARGET winglib2::wl2_membus_static)
else()
    if(NOT TARGET wl2_membus_static)
        # Static modules are disabled in this build; skip the example quietly.
        return()
    endif()
    set(WL2_CORE_TARGET wl2_core)
    set(WL2_MEMBUS_TARGET wl2_membus_static)
endif()

add_executable(wl2_foundations_example main.cpp)
target_link_libraries(wl2_foundations_example PRIVATE ${WL2_CORE_TARGET} ${WL2_MEMBUS_TARGET})

wl2_add_embedded_resources(wl2_foundations_example
    ROOT ${CMAKE_CURRENT_SOURCE_DIR}/scripts
    PREFIX wl2:/foundations
    STORE_FILES main.js config.json
    COMPRESS_FILES worker.js)

if(WL2_BUILD_TESTING)
    add_test(NAME examples.foundations.combined
        COMMAND wl2_foundations_example)
    set_tests_properties(examples.foundations.combined PROPERTIES
        LABELS "examples;integration;js;membus;resources;thread"
        TIMEOUT 10)
endif()
