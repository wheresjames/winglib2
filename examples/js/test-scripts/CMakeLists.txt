if(NOT TARGET wl2)
    cmake_minimum_required(VERSION 3.24)
    project(wl2_js_script_examples LANGUAGES CXX)
    find_package(winglib2 CONFIG REQUIRED)
    enable_testing()
    set(WL2_BUILD_TESTING ON)
    set(WL2_EXE_TARGET winglib2::wl2)
    set(WL2_HAVE_MEMBUS_EXAMPLE_TARGET FALSE)
    if(TARGET winglib2::wl2_membus_static)
        set(WL2_HAVE_MEMBUS_EXAMPLE_TARGET TRUE)
    endif()
else()
    set(WL2_EXE_TARGET wl2)
    set(WL2_HAVE_MEMBUS_EXAMPLE_TARGET FALSE)
    if(TARGET wl2_membus_static)
        set(WL2_HAVE_MEMBUS_EXAMPLE_TARGET TRUE)
    endif()
endif()

if(WL2_BUILD_TESTING AND WL2_HAVE_MEMBUS_EXAMPLE_TARGET)
    add_test(NAME examples.js.membus_queue
        COMMAND
            $<TARGET_FILE:${WL2_EXE_TARGET}>
            --allow-shared-memory
            --shared-memory-allow /wl2_example_queue_
            ${CMAKE_CURRENT_SOURCE_DIR}/membus_queue.js)
    add_test(NAME examples.js.membus_kv
        COMMAND
            $<TARGET_FILE:${WL2_EXE_TARGET}>
            --allow-shared-memory
            --shared-memory-allow /wl2_example_kv_
            ${CMAKE_CURRENT_SOURCE_DIR}/membus_kv.js)
    add_test(NAME examples.js.membus_video
        COMMAND
            $<TARGET_FILE:${WL2_EXE_TARGET}>
            --allow-shared-memory
            --shared-memory-allow /wl2_example_video_
            ${CMAKE_CURRENT_SOURCE_DIR}/membus_video.js)
    add_test(NAME examples.js.membus_select
        COMMAND
            $<TARGET_FILE:${WL2_EXE_TARGET}>
            ${CMAKE_CURRENT_SOURCE_DIR}/membus_select.js)

    set_tests_properties(
        examples.js.membus_queue
        examples.js.membus_kv
        examples.js.membus_video
        examples.js.membus_select
        PROPERTIES
            LABELS "examples;js;membus"
            TIMEOUT 10)
endif()
