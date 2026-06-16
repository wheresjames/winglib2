if(NOT TARGET wl2_core)
    cmake_minimum_required(VERSION 3.24)
    project(wl2_fs_inspect_js LANGUAGES CXX)
    find_package(winglib2 CONFIG REQUIRED)
    enable_testing()
    set(WL2_BUILD_TESTING ON)
endif()

if(TARGET wl2_fs_static)
    set(WL2_FS_TARGET wl2_fs_static)
elseif(TARGET winglib2::wl2_fs_static)
    set(WL2_FS_TARGET winglib2::wl2_fs_static)
else()
    message(FATAL_ERROR "This example requires Winglib2 built with wl2_fs_static")
endif()

wl2_add_javascript_executable(wl2_fs_inspect_js
    ROOT ${CMAKE_CURRENT_SOURCE_DIR}
    ENTRY main.js
    STORE_FILES main.js
    INCLUDES wl2_fs/wl2_fs.h
    STATIC_MODULES wl2_fs_register_module
    FILESYSTEM_READ_ROOTS ${CMAKE_CURRENT_SOURCE_DIR}/data
    LINK_LIBRARIES ${WL2_FS_TARGET})

if(WL2_BUILD_TESTING)
    add_test(NAME examples.js.fs_inspect_standalone
        COMMAND wl2_fs_inspect_js)
    set_tests_properties(examples.js.fs_inspect_standalone PROPERTIES
        LABELS "examples;js;fs"
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/data
        TIMEOUT 10)
endif()
