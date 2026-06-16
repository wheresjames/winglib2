# examples/tcp-echo
#
# Builds a single statically-linked executable that runs the embedded loopback
# echo script. Like the other Winglib2 examples it builds two ways:
#   * in-tree, added with add_subdirectory() from the module build, and
#   * standalone, configured against an installed Winglib2 package with
#     find_package(winglib2 CONFIG REQUIRED).
if(NOT TARGET wl2_core)
    cmake_minimum_required(VERSION 3.24)
    project(wl2_asio_tcp_echo_example LANGUAGES CXX)
    find_package(winglib2 CONFIG REQUIRED)
    set(WL2_CORE_TARGET winglib2::wl2_core)
    if(NOT TARGET winglib2::wl2_asio_static)
        message(FATAL_ERROR "This example requires Winglib2 installed with wl2_asio_static")
    endif()
    set(WL2_ASIO_STATIC_TARGET winglib2::wl2_asio_static)
else()
    set(WL2_CORE_TARGET wl2_core)
    set(WL2_ASIO_STATIC_TARGET wl2_asio_static)
endif()

add_executable(wl2_asio_tcp_echo_example main.cpp)
target_link_libraries(wl2_asio_tcp_echo_example PRIVATE
    ${WL2_CORE_TARGET} ${WL2_ASIO_STATIC_TARGET})

wl2_add_embedded_resources(wl2_asio_tcp_echo_example
    ROOT ${CMAKE_CURRENT_SOURCE_DIR}/scripts
    PREFIX wl2:/tcp-echo
    STORE_FILES main.js)

if(WL2_BUILD_TESTING)
    add_test(NAME examples.wl2_asio.tcp_echo
        COMMAND wl2_asio_tcp_echo_example)
    set_tests_properties(examples.wl2_asio.tcp_echo PROPERTIES
        LABELS "examples;asio;js;integration"
        TIMEOUT 15)
endif()
