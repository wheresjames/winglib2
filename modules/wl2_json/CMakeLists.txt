include(cmake/options.cmake)

if(NOT WL2_ENABLE_JSON)
    return()
endif()

wl2_add_module(wl2_json
    MODULE_NAME wl2:json
    REGISTER_FUNCTION wl2_json_register_module
    STABLE_ID 8c73c0d6-01c0-4c22-8e4f-407356cf76be
    FEATURE WL2_ENABLE_JSON
    SOURCES src/wl2_json.cpp
    PUBLIC_LINK_LIBRARIES wl2_core
    DYNAMIC_INCLUDE_DIRS ${PROJECT_SOURCE_DIR}/include ${WL2_NLOHMANN_JSON_INCLUDE_DIR}
    NO_DYNAMIC_LINK_LIBRARIES
    STATIC_PRIVATE_COMPILE_DEFINITIONS WL2_JSON_STATIC_MODULE=1)

if(TARGET wl2_json_static)
    target_include_directories(wl2_json_static PRIVATE ${WL2_NLOHMANN_JSON_INCLUDE_DIR})
endif()
if(TARGET wl2_json)
    target_include_directories(wl2_json PRIVATE ${WL2_NLOHMANN_JSON_INCLUDE_DIR})
endif()

if(TARGET wl2_json_static)
    wl2_add_module_tests(wl2_json)
endif()
