include(cmake/options.cmake)

if(NOT WL2_ENABLE_ONVIF)
    return()
endif()

include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/WL2Onvif.cmake)
wl2_find_onvif()
if(NOT WL2_HAVE_ONVIF)
    message(STATUS "wlonvif not found; wl2:onvif disabled")
    return()
endif()

wl2_add_module(wl2_onvif
    MODULE_NAME wl2:onvif
    REGISTER_FUNCTION wl2_onvif_register_module
    VERSION 0.1.0
    STABLE_ID 53f57f40-5507-4a48-82d1-122fad7c6d71
    FEATURE WL2_ENABLE_ONVIF
    SOURCES src/wl2_onvif.cpp
    PUBLIC_LINK_LIBRARIES wl2_core ${WL2_ONVIF_TARGET}
    DYNAMIC_INCLUDE_DIRS ${PROJECT_SOURCE_DIR}/include
    DYNAMIC_LINK_LIBRARIES ${WL2_ONVIF_TARGET}
    STATIC_PRIVATE_COMPILE_DEFINITIONS WL2_ONVIF_STATIC_MODULE=1)

wl2_module_package_cmake_file("${CMAKE_CURRENT_SOURCE_DIR}/cmake/WL2Onvif.cmake" DESTINATION modules/wl2_onvif)
wl2_module_package_cmake_file("${CMAKE_CURRENT_SOURCE_DIR}/cmake/options.cmake" DESTINATION modules/wl2_onvif)
wl2_module_package_config_fragment(CONTENT
    "if(NOT TARGET wlonvif::client)\n    find_dependency(wlonvif CONFIG REQUIRED COMPONENTS client)\nendif()\n")

if(TARGET wl2_onvif_static)
    wl2_add_module_tests(wl2_onvif)
endif()
