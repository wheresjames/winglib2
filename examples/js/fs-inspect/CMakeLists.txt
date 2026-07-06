if(NOT TARGET wl2_core)
    cmake_minimum_required(VERSION 3.24)
    project(wl2_fs_inspect_js LANGUAGES CXX)
    find_package(winglib2 CONFIG REQUIRED)
    enable_testing()
    set(WL2_BUILD_TESTING ON)
endif()

# Dynamic (default): the wl2 runner resolves the manifest-required wl2:fs
# module from the staged build-tree or installed dynamic module store.
# Static (explicit opt-in via WL2_EXAMPLES_MODULE_MODE=static): a standalone
# executable that embeds the module through static registration.
if(NOT DEFINED WL2_EXAMPLES_MODULE_MODE)
    set(WL2_EXAMPLES_MODULE_MODE "dynamic")
endif()

if(WL2_EXAMPLES_MODULE_MODE STREQUAL "dynamic")
    # In-tree the wl2 runner target is defined after the modules that add this
    # example, so it is referenced through a generator expression rather than
    # checked with if(TARGET ...) at configure time.
    if(TARGET wl2_core)
        set(WL2_EXE_TARGET wl2)
    elseif(TARGET winglib2::wl2)
        set(WL2_EXE_TARGET winglib2::wl2)
    else()
        message(FATAL_ERROR "This example requires the wl2 runner")
    endif()
    if(WL2_BUILD_TESTING)
        add_test(NAME examples.js.fs_inspect_dynamic
            COMMAND
                $<TARGET_FILE:${WL2_EXE_TARGET}>
                run
                --manifest ${CMAKE_CURRENT_SOURCE_DIR}/wl2.yml
                --module-policy isolated
                --allow filesystem-read:${CMAKE_CURRENT_SOURCE_DIR}/data
                --no-permission-prompt)
        set_tests_properties(examples.js.fs_inspect_dynamic PROPERTIES
            LABELS "examples;js;fs;dynamic;modules"
            WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/data
            TIMEOUT 10)
    endif()
else()
    # Static embedding variant of this example.
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
endif()
