# examples/http-showcase
#
# Builds a single statically-linked executable that runs an embedded wl2:http
# server script. It works both in-tree and as a standalone example configured
# against an installed Winglib2 package.
if(NOT TARGET wl2_core)
    cmake_minimum_required(VERSION 3.24)
    project(wl2_restinio_http_showcase_example LANGUAGES CXX)
    find_package(winglib2 CONFIG REQUIRED)
    set(WL2_CORE_TARGET winglib2::wl2_core)
    if(NOT TARGET winglib2::wl2_restinio_static)
        message(FATAL_ERROR "This example requires Winglib2 installed with wl2_restinio_static")
    endif()
    set(WL2_RESTINIO_STATIC_TARGET winglib2::wl2_restinio_static)
else()
    set(WL2_CORE_TARGET wl2_core)
    set(WL2_RESTINIO_STATIC_TARGET wl2_restinio_static)
endif()

add_executable(wl2_restinio_http_showcase_example main.cpp)
target_link_libraries(wl2_restinio_http_showcase_example PRIVATE
    ${WL2_CORE_TARGET} ${WL2_RESTINIO_STATIC_TARGET})

wl2_add_embedded_resources(wl2_restinio_http_showcase_example
    ROOT ${CMAKE_CURRENT_SOURCE_DIR}/scripts
    PREFIX wl2:/http-showcase
    STORE_FILES main.js)

if(WL2_BUILD_TESTING)
    add_test(NAME examples.wl2_restinio.http_showcase
        COMMAND wl2_restinio_http_showcase_example --selftest --port 0)
    set_tests_properties(examples.wl2_restinio.http_showcase PROPERTIES
        LABELS "examples;http;js;integration"
        TIMEOUT 15)
endif()
