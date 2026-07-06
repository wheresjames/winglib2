if(NOT TARGET wl2_core)
    cmake_minimum_required(VERSION 3.24)
    project(wl2_curl_resources_js LANGUAGES CXX)
    find_package(winglib2 CONFIG REQUIRED)
    enable_testing()
    set(WL2_BUILD_TESTING ON)
endif()

# Dynamic (default): the wl2 runner resolves the manifest-required wl2:curl
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
        # The script tolerates an unreachable network; no network grant needed.
        add_test(NAME examples.js.curl_resources_dynamic
            COMMAND
                $<TARGET_FILE:${WL2_EXE_TARGET}>
                run
                --manifest ${CMAKE_CURRENT_SOURCE_DIR}/wl2.yml
                --module-policy isolated
                --no-permission-prompt)
        set_tests_properties(examples.js.curl_resources_dynamic PROPERTIES
            LABELS "examples;js;curl;resources;dynamic;modules"
            TIMEOUT 10)
    endif()
else()
    # Static embedding variant of this example.
    if(TARGET wl2_curl_static)
        set(WL2_CURL_TARGET wl2_curl_static)
    elseif(TARGET winglib2::wl2_curl_static)
        set(WL2_CURL_TARGET winglib2::wl2_curl_static)
    else()
        message(FATAL_ERROR "This example requires Winglib2 built with wl2_curl_static")
    endif()

    wl2_add_javascript_executable(wl2_curl_resources_js
        ROOT ${CMAKE_CURRENT_SOURCE_DIR}
        ENTRY main.js
        STORE_FILES main.js config.json payload.txt
        INCLUDES wl2_curl/wl2_curl.h
        STATIC_MODULES wl2_curl_register_module
        LINK_LIBRARIES ${WL2_CURL_TARGET})

    if(WL2_BUILD_TESTING)
        add_test(NAME examples.js.curl_resources_standalone
            COMMAND wl2_curl_resources_js)
        set_tests_properties(examples.js.curl_resources_standalone PROPERTIES
            LABELS "examples;js;curl;resources"
            TIMEOUT 10)
    endif()
endif()
