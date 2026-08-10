include(cmake/options.cmake)

if(NOT WL2_ENABLE_UV)
    return()
endif()

include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/WL2Uv.cmake)
wl2_find_uv()
if(NOT WL2_HAVE_UV)
    return()
endif()

wl2_add_module(wl2_uv
    MODULE_NAME wl2:uv
    REGISTER_FUNCTION wl2_uv_register_module
    VERSION 0.1.0
    STABLE_ID 8f6fdd67-04ed-4590-a3c1-4273a85ce672
    FEATURE WL2_ENABLE_UV
    SOURCES src/wl2_uv.cpp
    PUBLIC_LINK_LIBRARIES wl2_core
    PRIVATE_LINK_LIBRARIES ${WL2_UV_TARGET}
    DYNAMIC_INCLUDE_DIRS ${PROJECT_SOURCE_DIR}/include
    DYNAMIC_LINK_LIBRARIES ${WL2_UV_TARGET}
    STATIC_PRIVATE_COMPILE_DEFINITIONS WL2_UV_STATIC_MODULE=1
    DYNAMIC_PRIVATE_COMPILE_DEFINITIONS WL2_VERSION="${PROJECT_VERSION}")

wl2_module_package_cmake_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/cmake/WL2Uv.cmake"
    DESTINATION modules/wl2_uv)
wl2_module_package_cmake_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/cmake/options.cmake"
    DESTINATION modules/wl2_uv)

wl2_module_package_config_fragment(CONTENT [==[
if(NOT TARGET wl2_uv_dep)
    find_path(WL2_UV_SYSTEM_INCLUDE_DIR NAMES uv.h REQUIRED)
    find_library(WL2_UV_SYSTEM_LIBRARY NAMES uv libuv REQUIRED)
    add_library(wl2_uv_dep UNKNOWN IMPORTED)
    set_target_properties(wl2_uv_dep PROPERTIES
        IMPORTED_LOCATION "${WL2_UV_SYSTEM_LIBRARY}"
        INTERFACE_INCLUDE_DIRECTORIES "${WL2_UV_SYSTEM_INCLUDE_DIR}")
endif()
]==])

if(TARGET wl2_uv_static)
    wl2_add_module_tests(wl2_uv)
endif()
