if(NOT TARGET wl2_core)
    cmake_minimum_required(VERSION 3.24)
    project(wl2_curl_resources_js LANGUAGES CXX)
    find_package(winglib2 CONFIG REQUIRED)
    enable_testing()
    set(WL2_BUILD_TESTING ON)
endif()

if(TARGET wl2_curl_static)
    set(WL2_CURL_TARGET wl2_curl_static)
elseif(TARGET winglib2::wl2_curl_static)
    set(WL2_CURL_TARGET winglib2::wl2_curl_static)
else()
    message(FATAL_ERROR "This example requires Winglib2 built with wl2_curl_static")
endif()

wl2_add_javascript_executable(wl2_curl_resources_js
    ROOT ${CMAKE_CURRENT_SOURCE_DIR}
    ENTRY main.js
    STORE_FILES main.js config.json payload.txt
    INCLUDES wl2_curl/wl2_curl.h
    STATIC_MODULES wl2_curl_register_module
    LINK_LIBRARIES ${WL2_CURL_TARGET})

if(WL2_BUILD_TESTING)
    add_test(NAME examples.js.curl_resources_standalone
        COMMAND wl2_curl_resources_js)
    set_tests_properties(examples.js.curl_resources_standalone PROPERTIES
        LABELS "examples;js;curl;resources"
        TIMEOUT 10)
endif()
