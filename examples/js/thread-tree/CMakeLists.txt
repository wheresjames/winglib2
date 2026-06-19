if(NOT TARGET wl2)
    cmake_minimum_required(VERSION 3.24)
    project(wl2_thread_tree_examples LANGUAGES CXX)
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

if(WL2_BUILD_TESTING)
    add_test(NAME examples.thread_tree.spawn
        COMMAND
            $<TARGET_FILE:${WL2_EXE_TARGET}>
            ${CMAKE_CURRENT_SOURCE_DIR}/spawn.js)
    add_test(NAME examples.thread_tree.request_reply
        COMMAND
            $<TARGET_FILE:${WL2_EXE_TARGET}>
            ${CMAKE_CURRENT_SOURCE_DIR}/request-reply.js)
    add_test(NAME examples.thread_tree.buffer_pipeline
        COMMAND
            $<TARGET_FILE:${WL2_EXE_TARGET}>
            ${CMAKE_CURRENT_SOURCE_DIR}/buffer-pipeline.js)

    set_tests_properties(
        examples.thread_tree.spawn
        examples.thread_tree.request_reply
        examples.thread_tree.buffer_pipeline
        PROPERTIES
            LABELS "examples;js;thread"
            TIMEOUT 10)

    if(WL2_HAVE_MEMBUS_EXAMPLE_TARGET)
        add_test(NAME examples.thread_tree.membus_worker
            COMMAND
                $<TARGET_FILE:${WL2_EXE_TARGET}>
                --allow-shared-memory
                --shared-memory-allow /wl2_thread_tree_example
                ${CMAKE_CURRENT_SOURCE_DIR}/membus-worker.js)
        set_tests_properties(examples.thread_tree.membus_worker PROPERTIES
            LABELS "examples;js;thread;membus;integration"
            TIMEOUT 10)
    endif()
endif()
