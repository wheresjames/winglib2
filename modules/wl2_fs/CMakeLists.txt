include(cmake/options.cmake)

if(NOT WL2_ENABLE_FS)
    return()
endif()

wl2_add_module(wl2_fs
    MODULE_NAME wl2:fs
    REGISTER_FUNCTION wl2_fs_register_module
    STABLE_ID b3a1f2d4-6c8e-4a17-9b2f-0d5e7c1a4e90
    FEATURE WL2_ENABLE_FS
    SOURCES src/wl2_fs.cpp
    PUBLIC_LINK_LIBRARIES wl2_core
    DYNAMIC_INCLUDE_DIRS ${PROJECT_SOURCE_DIR}/include
    NO_DYNAMIC_LINK_LIBRARIES
    STATIC_PRIVATE_COMPILE_DEFINITIONS WL2_FS_STATIC_MODULE=1)

if(TARGET wl2_fs_static)
    if(WL2_BUILD_EXAMPLES)
        add_subdirectory(
            "${PROJECT_SOURCE_DIR}/examples/js/fs-inspect"
            "${CMAKE_BINARY_DIR}/examples/js/fs-inspect")
    endif()
    wl2_add_module_tests(wl2_fs)
endif()
