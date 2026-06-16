include(cmake/options.cmake)

if(NOT WL2_ENABLE_MEMBUS OR NOT WL2_HAVE_LIBMEMBUS)
    return()
endif()

wl2_add_module(wl2_membus
    MODULE_NAME wl2:membus
    REGISTER_FUNCTION wl2_membus_register_module
    STABLE_ID 2a05080e-c801-4a84-8fc3-d0e2361fdb64
    FEATURE WL2_ENABLE_MEMBUS
    SOURCES src/wl2_membus.cpp
    PUBLIC_LINK_LIBRARIES wl2_core
    DYNAMIC_INCLUDE_DIRS ${PROJECT_SOURCE_DIR}/include
    NO_DYNAMIC_LINK_LIBRARIES
    STATIC_PRIVATE_COMPILE_DEFINITIONS WL2_MEMBUS_STATIC_MODULE=1)

if(TARGET wl2_membus_static)
    if(WL2_BUILD_EXAMPLES)
        add_subdirectory(
            "${PROJECT_SOURCE_DIR}/examples/cpp/foundations"
            "${CMAKE_BINARY_DIR}/examples/cpp/foundations")
    endif()
    if(WL2_BUILD_TESTING)
        add_test(NAME scripts.wl2_membus_smoke
            COMMAND
                $<TARGET_FILE:wl2>
                run
                ${PROJECT_SOURCE_DIR}/test/scripts/wl2_membus_smoke.js)
        set_tests_properties(scripts.wl2_membus_smoke PROPERTIES
            LABELS "js;membus")
    endif()
endif()
