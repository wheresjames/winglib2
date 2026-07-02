include(cmake/options.cmake)

if(NOT WL2_ENABLE_HTTP)
    return()
endif()

# RESTinio (and its fmt/llhttp/expected-lite dependencies) are used only by this
# module, so they are discovered/fetched here. wl2_find_restinio() sets
# WL2_HAVE_RESTINIO / WL2_RESTINIO_TARGET in this scope and creates the GLOBAL
# wl2_restinio_dep target. Standalone Asio is reused from the wl2:asio fetch.
include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/WL2Restinio.cmake)
wl2_find_restinio()

if(NOT WL2_HAVE_RESTINIO)
    message(STATUS "RESTinio not available; wl2_restinio module disabled")
    return()
endif()

wl2_add_module(wl2_restinio
    MODULE_NAME wl2:http
    REGISTER_FUNCTION wl2_restinio_register_module
    STABLE_ID 8c49bf47-227b-487d-9e93-5f4ec3e8c054
    FEATURE WL2_ENABLE_HTTP
    SOURCES src/wl2_restinio.cpp
    PUBLIC_LINK_LIBRARIES wl2_core
    PRIVATE_LINK_LIBRARIES ${WL2_RESTINIO_TARGET}
    DYNAMIC_INCLUDE_DIRS ${PROJECT_SOURCE_DIR}/include
    DYNAMIC_LINK_LIBRARIES ${WL2_RESTINIO_TARGET}
    STATIC_PRIVATE_COMPILE_DEFINITIONS WL2_RESTINIO_STATIC_MODULE=1
    DYNAMIC_PRIVATE_COMPILE_DEFINITIONS WL2_VERSION="${PROJECT_VERSION}")

# RESTinio's header-only templates trip -Wall/-Wextra in ways we do not control;
# the environment turns those into errors. Keep our diagnostics but stop them
# from failing the build for this module's translation units.
foreach(_t wl2_restinio_static wl2_restinio)
    if(TARGET ${_t})
        target_compile_options(${_t} PRIVATE -Wno-error)
    endif()
endforeach()

if(TARGET wl2_restinio_static)
    if(WL2_BUILD_EXAMPLES AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/examples/http-showcase/CMakeLists.txt")
        add_subdirectory(examples/http-showcase)
    endif()
    wl2_add_module_tests(wl2_restinio)
endif()
