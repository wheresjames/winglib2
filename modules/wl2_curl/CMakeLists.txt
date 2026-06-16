include(cmake/options.cmake)

if(NOT WL2_ENABLE_CURL)
    return()
endif()

# curl is only used by this module, so it is discovered here rather than in the
# global build space. wl2_find_curl() sets WL2_HAVE_CURL / WL2_CURL_TARGET in this
# scope and creates the GLOBAL CURL::libcurl imported target.
include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/WL2Curl.cmake)
wl2_find_curl()

if(NOT WL2_HAVE_CURL)
    message(STATUS "libcurl not found; wl2_curl module disabled")
    return()
endif()

wl2_add_module(wl2_curl
    MODULE_NAME wl2:curl
    REGISTER_FUNCTION wl2_curl_register_module
    STABLE_ID 7ef0cc47-6153-45f4-a607-c9b88f7654ef
    FEATURE WL2_ENABLE_CURL
    SOURCES src/wl2_curl.cpp
    PUBLIC_LINK_LIBRARIES wl2_core ${WL2_CURL_TARGET}
    DYNAMIC_INCLUDE_DIRS ${PROJECT_SOURCE_DIR}/include
    DYNAMIC_LINK_LIBRARIES ${WL2_CURL_TARGET}
    STATIC_PRIVATE_COMPILE_DEFINITIONS WL2_CURL_STATIC_MODULE=1)

wl2_module_package_cmake_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/cmake/WL2Curl.cmake"
    DESTINATION modules/wl2_curl)
wl2_module_package_cmake_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/cmake/options.cmake"
    DESTINATION modules/wl2_curl)
if(WL2_CURL_IS_SYSTEM)
    wl2_module_package_config_fragment(CONTENT
        "if(NOT TARGET CURL::libcurl)\n    find_dependency(CURL)\nendif()\n")
else()
    wl2_module_bundle_dependency(CURL
        TARGET ${WL2_CURL_TARGET}
        INCLUDE_DIR "${WL2_CURL_ROOT}/include"
        COMPILE_DEFINITIONS CURL_STATICLIB)
endif()

if(TARGET wl2_curl_static)
    if(WL2_BUILD_EXAMPLES)
        add_subdirectory(
            "${PROJECT_SOURCE_DIR}/examples/js/curl-resources"
            "${CMAKE_BINARY_DIR}/examples/js/curl-resources")
    endif()
    wl2_add_module_tests(wl2_curl)
endif()
