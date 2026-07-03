include(cmake/options.cmake)

if(NOT WL2_ENABLE_YML)
    return()
endif()

# yaml-cpp is vendored (downloaded and compiled straight into this module) so it
# stays fully private and needs no separate target, export, or install rules.
include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/WL2YamlCpp.cmake)
wl2_find_yaml_cpp()

if(NOT WL2_HAVE_YAMLCPP)
    message(STATUS "yaml-cpp not available; wl2_yml module disabled")
    return()
endif()

wl2_add_module(wl2_yml
    MODULE_NAME wl2:yml
    REGISTER_FUNCTION wl2_yml_register_module
    STABLE_ID be935e3c-3348-4f62-a20e-17958558e8d9
    FEATURE WL2_ENABLE_YML
    SOURCES src/wl2_yml.cpp ${WL2_YAMLCPP_SOURCES}
    PUBLIC_LINK_LIBRARIES wl2_core
    DYNAMIC_INCLUDE_DIRS
        ${PROJECT_SOURCE_DIR}/include
        ${WL2_NLOHMANN_JSON_INCLUDE_DIR}
        ${WL2_YAMLCPP_INCLUDE_DIR}
        ${WL2_YAMLCPP_SRC_DIR}
    NO_DYNAMIC_LINK_LIBRARIES
    STATIC_PRIVATE_COMPILE_DEFINITIONS WL2_YML_STATIC_MODULE=1)

# Apply the includes and static-linkage define needed to both compile the
# vendored yaml-cpp sources and include <yaml-cpp/...> from the module source.
# nlohmann/json is the header-only bridge between JS values and yaml-cpp; core
# keeps its include path private, so add it explicitly too.
foreach(_wl2_yml_target wl2_yml_static wl2_yml)
    if(TARGET ${_wl2_yml_target})
        target_include_directories(${_wl2_yml_target}
            PRIVATE
                ${WL2_NLOHMANN_JSON_INCLUDE_DIR}
                ${WL2_YAMLCPP_INCLUDE_DIR}
                ${WL2_YAMLCPP_SRC_DIR})
        target_compile_definitions(${_wl2_yml_target}
            PRIVATE YAML_CPP_STATIC_DEFINE)
    endif()
endforeach()

if(TARGET wl2_yml_static)
    wl2_add_module_tests(wl2_yml)
endif()
